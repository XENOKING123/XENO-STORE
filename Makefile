# XENO STORE — build system (SPEC §2). Run every target from the repository root.
#
#   make web      node web/build.mjs            -> web/dist/index.html
#   make catalog  tools/catalog/build_catalog.py -> catalog/xeno-catalog.json (keeps a richer synced one;
#                 CATALOG_FLAGS=--force rebuilds it from the snapshots anyway)
#   make assets   tools/gen_assets.py            -> src/assets.h
#   make host     host binary build/host/xeno-store (-DXENO_HOST=1, OpenSSL if found)
#   make test     host build + tests/host/run.sh + launcher-test + saves-test
#   make saves-test  tools/saves/tests (save submission intake; the tests write their own fixtures)
#   make ps5      xeno-store.elf (needs PS5_PAYLOAD_SDK)
#   make eboot    app/native-launcher/build.sh    -> app/dist/eboot.bin + app/dist/sce_module/libc.prx
#                 (the Games-row launcher as a native app; fetches its own pinned SDK v0.42)
#   make eboot-raw app/build_eboot.sh             -> app/dist/eboot.bin (the old freestanding launcher)
#   make launcher-test  host test of the native launcher's logic (fake store + fake ELF loader)
#   make ffpkg    tools/make_ffpkg.py             -> dist/PPSA99500.ffpkg (the Games-row app, needs makefs)
#   make all      web catalog assets ps5 eboot ffpkg (what a release ships)
#   make pkg      tools/make_homebrew.py          -> dist/XENO-STORE/ + zip for etaHEN / itemzflow (optional, not released)
#   make launcher app/build_launcher.sh          -> app/dist/xeno-store-launcher.elf (needs PS5_PAYLOAD_SDK)
#   make clean

VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
PYTHON  ?= python3
NODE    ?= node

SRC_DIR   := src
BUILD_DIR := build
ASSETS    := $(SRC_DIR)/assets.h

# Every src/*.c is built except the two transport backends, which are picked per target.
COMMON_SRCS := $(filter-out http_posix.c http_sce.c,$(sort $(notdir $(wildcard $(SRC_DIR)/*.c))))
HOST_SRCS   := $(COMMON_SRCS) http_posix.c
PS5_SRCS    := $(COMMON_SRCS) http_sce.c
HEADERS     := $(wildcard $(SRC_DIR)/*.h)

BASE_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O2 -fstack-protector-strong -D_FILE_OFFSET_BITS=64 -DXENO_VERSION=\"$(VERSION)\"

# ---- host --------------------------------------------------------------------
HOST_CC     ?= cc
HOST_CFLAGS := $(BASE_CFLAGS) -DXENO_HOST=1 -D_GNU_SOURCE -pthread
HOST_LDLIBS := -lpthread -lm
# gcc's -Wformat-truncation fires on intentionally bounded snprintf() chains;
# only add the opt-out when the compiler knows the flag (clang does not).
HOST_CFLAGS += $(shell echo 'int main(void){return 0;}' | $(HOST_CC) -Werror -Wno-format-truncation -x c -c -o /dev/null - >/dev/null 2>&1 && echo -Wno-format-truncation)
ifeq ($(shell pkg-config --exists openssl && echo yes),yes)
  HOST_CFLAGS += -DXENO_HAVE_OPENSSL=1 $(shell pkg-config --cflags openssl)
  HOST_LDLIBS += $(shell pkg-config --libs openssl)
endif
HOST_OBJS := $(addprefix $(BUILD_DIR)/host/,$(HOST_SRCS:.c=.o))
HOST_BIN  := $(BUILD_DIR)/host/xeno-store

# ---- ps5 ---------------------------------------------------------------------
# The library set mirrors this repo's proven payloads (payload/Makefile and
# payload/dpi/Makefile): every one is a DT_NEEDED entry, because a payload whose
# dependency graph rtld cannot resolve is rejected silently at load time.
# --no-as-needed keeps them all, including libSceNotification and the libSceNet
# the SDK wrapper appends, exactly as before.
# libSceIpmi is the one addition: launcher_icon.c resolves libSceAppInstUtil's
# title installer by NID at runtime, and that path needs libSceIpmi loaded even
# though no symbol is referenced directly. PS5_LIBS is used only by the PS5 link
# rules, so the host build never sees it.
PS5_LIBS := -Wl,--no-as-needed -lSceSystemService -lSceUserService -lSceIpmi -lSceAppInstUtil -lSceNotification -lkernel_sys
ELF := xeno-store.elf

.PHONY: all web catalog assets host test saves-test ps5 ps5-check pkg eboot eboot-raw launcher-test ffpkg desktop launcher clean distclean

all: web catalog assets ps5 eboot ffpkg

web:
	$(NODE) web/build.mjs

# The offline build never shrinks the catalog catalog-sync.yml commits: when
# catalog/report.json says the current file is an online build with more games,
# it is left alone (exit 0) and only --validate runs. CATALOG_FLAGS=--force
# replaces it with the snapshot build.
CATALOG_FLAGS ?=

catalog:
	$(PYTHON) tools/catalog/build_catalog.py --offline $(CATALOG_FLAGS)
	$(PYTHON) tools/catalog/build_catalog.py --validate

assets:
	$(PYTHON) tools/gen_assets.py

# Only rebuilt when missing; 'assets' (above) regenerates on demand and leaves
# the file untouched when nothing embedded changed, so objects stay fresh.
$(ASSETS):
	$(PYTHON) tools/gen_assets.py

# ---- host build --------------------------------------------------------------
host: assets $(HOST_BIN)

$(HOST_BIN): $(HOST_OBJS)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_OBJS) $(HOST_LDLIBS)
	@echo "host binary: $@"

$(BUILD_DIR)/host/%.o: $(SRC_DIR)/%.c $(HEADERS) $(ASSETS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c -o $@ $<

test: host launcher-test saves-test
	tests/host/run.sh

# The save submission intake (tools/saves/intake.py, run by saves-intake.yml): stdlib only.
saves-test:
	$(PYTHON) -I -m unittest discover -s tools/saves/tests

# The native launcher's logic, built for this machine and run against a fake store and a fake
# ELF loader (the 30 s give-up case is left to the full run: app/native-launcher/tests/run-host-test.py).
launcher-test:
	$(PYTHON) app/native-launcher/tests/run-host-test.py --skip-timeout

# Compile-only check of every PS5 translation unit (no link); handy without a full SDK link.
ps5-check: assets
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK is not set)
endif
	$(MAKE) PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) $(addprefix $(BUILD_DIR)/ps5/,$(PS5_SRCS:.c=.o))

# ---- PS5 build ---------------------------------------------------------------
ps5: assets
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK is not set — export it to the ps5-payload-dev SDK directory)
endif
	$(MAKE) PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) $(ELF)

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
PS5_CFLAGS := $(BASE_CFLAGS)
PS5_OBJS := $(addprefix $(BUILD_DIR)/ps5/,$(PS5_SRCS:.c=.o))

$(ELF): $(PS5_OBJS)
	$(CC) $(PS5_CFLAGS) -o $@ $(PS5_OBJS) $(PS5_LIBS)
	@echo "payload: $@"

$(BUILD_DIR)/ps5/%.o: $(SRC_DIR)/%.c $(HEADERS) $(ASSETS)
	@mkdir -p $(dir $@)
	$(CC) $(PS5_CFLAGS) -c -o $@ $<
endif

# ---- packaging ---------------------------------------------------------------
pkg:
	$(PYTHON) tools/make_homebrew.py

# Games-row app: dist/PPSA99500.ffpkg (UFS2 image for ShadowMountPlus, needs makefs; an optional
# fake .pkg too when XENO_FPKG_BUILD points at ps5upload's fpkg_build). eboot.bin is the launcher
# from 'make eboot'. Skips cleanly (exit 3) when makefs is missing.
#
# 'make eboot' builds the launcher as a native PS5 app with the ps5-native-app-boilerplate
# toolchain (BlackBearReloaded, GPL-3.0-or-later) in app/native-launcher: it needs clang-18,
# ninja, wget and unzip, and fetches the SDK v0.42 that toolchain pins into
# app/native-launcher/.deps (PS5_PAYLOAD_SDK is not used). Next to eboot.bin it writes
# sce_module/libc.prx, the toolchain's clean-room libc module that an app folder carries;
# make_ffpkg.py stages it too.
eboot:
	bash app/native-launcher/build.sh app/dist

# The old freestanding launcher (raw syscalls, ET_EXEC), kept until the native one is proven
# on a console. It needs no sce_module, so the libc.prx left by 'make eboot' is removed.
eboot-raw:
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK is not set — export it to the ps5-payload-dev SDK directory)
endif
	rm -rf app/dist/sce_module
	PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) app/build_eboot.sh

# XENO STORE Desktop for this machine (needs Go 1.22+); CI builds every OS.
desktop:
	cd desktop && CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=$(VERSION)" -o ../dist/XENO-STORE-Desktop$(if $(filter Windows_NT,$(OS)),.exe,) .

ffpkg:
	@$(PYTHON) tools/make_ffpkg.py; rc=$$?; \
	  if [ $$rc -eq 3 ]; then echo "make: ffpkg skipped — neither makefs nor fpkg_build found"; \
	  elif [ $$rc -ne 0 ]; then exit $$rc; fi

# App-launcher ELF (eboot for the .pkg): probes the store, starts it via the ELF
# loader if needed, then opens the browser. Server-only link set.
launcher:
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK is not set — export it to the ps5-payload-dev SDK directory)
endif
	PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) app/build_launcher.sh

# 'clean' removes the payload build outputs; web/dist belongs to 'make web'
# (and the UI builders working in parallel), so only 'distclean' removes it.
clean:
	rm -rf $(BUILD_DIR) $(ELF) xeno-store-browser.elf $(ASSETS) dist app/dist app/native-launcher/build

distclean: clean
	rm -rf web/dist app/native-launcher/.deps app/native-launcher/runtime/libc.prx
