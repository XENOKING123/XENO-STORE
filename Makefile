# XENO STORE — build system (SPEC §2). Run every target from the repository root.
#
#   make web      node web/build.mjs            -> web/dist/index.html
#   make catalog  tools/catalog/build_catalog.py -> catalog/xeno-catalog.json (keeps a richer synced one;
#                 CATALOG_FLAGS=--force rebuilds it from the snapshots anyway)
#   make assets   tools/gen_assets.py            -> src/assets.h
#   make host     host binary build/host/xeno-store (-DXENO_HOST=1, OpenSSL if found)
#   make test     host build + tests/host/run.sh
#   make ps5      xeno-store.elf (needs PS5_PAYLOAD_SDK)
#   make eboot    app/native-launcher/build.sh    -> app/dist/eboot.bin + app/dist/sce_module/libc.prx
#                 (the Games-row launcher as a native app; fetches its own pinned SDK v0.42)
#   make eboot-raw app/build_eboot.sh             -> app/dist/eboot.bin (the old freestanding launcher)
#   make launcher-test  host test of the native launcher's logic (fake store + fake ELF loader)
#   make tv       app/tv (the native TV app)      -> app/tv/dist/PPSA99500 (needs 'make tv-deps' and
#                 clang-18; the TV app targets are listed at the end of this file)
#   make ffpkg    tools/make_ffpkg.py             -> dist/PPSA99500.ffpkg (the Games-row app, needs makefs):
#                 the TV app with xeno-store.elf in xeno/ when app/tv/dist/PPSA99500 holds a built
#                 app folder, otherwise the native launcher with the ELF next to it
#   make all      web catalog assets ps5 eboot tv ffpkg (what a release ships; tv only on a machine
#                 set up for it, see TV_ALL below)
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

# ---- TV app in 'make all' ------------------------------------------------------
# 'make all' builds the TV app (the 'tv' target, at the end of this file) only where it can:
# clang-18 on PATH and the pinned dependencies fetched ('make tv-deps', about 400 MB).
# Elsewhere it prints a note and goes on, and 'make ffpkg' packs an app folder already in
# app/tv/dist/PPSA99500 (for example the tv-app CI artifact) or, without one, the native
# launcher. TV=1 makes 'make all' build the TV app regardless (and fail where the toolchain
# is missing); TV=0 leaves it out. Defined here because 'all' expands TV_ALL as it is read.
TV_DIR        := app/tv
TV_DEPS_READY := $(and $(shell command -v clang-18 2>/dev/null),$(wildcard $(TV_DIR)/.deps/native/ps5-payload-sdk),$(wildcard $(TV_DIR)/.deps/pacbrew/v0.40.2),$(wildcard $(TV_DIR)/.deps/ps5-opengl/ps5-opengl-sdk-*))
TV_ALL        := $(if $(filter 1,$(TV)),tv,$(if $(filter 0,$(TV)),,$(if $(TV_DEPS_READY),tv,tv-skip)))

.PHONY: all web catalog assets host test ps5 ps5-check pkg eboot eboot-raw launcher-test ffpkg desktop launcher clean distclean

all: web catalog assets ps5 eboot $(TV_ALL) ffpkg

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

test: host launcher-test
	tests/host/run.sh

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

# With the TV app folder in place ('make tv', or the tv-app CI artifact) the image holds the TV app
# instead of the launcher: tools/make_ffpkg.py --app-dir stages its eboot.bin, sce_module/ and
# assets/, app/tv's NOTICE and licences, and the store ELF as xeno/xeno-store.elf.
# The folder is looked for when the recipe runs, so 'make all' packs the app folder 'make tv'
# built moments before. 'make ffpkg FFPKG_APP_DIR=' builds the launcher image anyway, and
# 'make ffpkg FFPKG_APP_DIR=<dir>' packs another app folder.
FFPKG_APP_DIR ?= auto

ffpkg:
	@app='$(FFPKG_APP_DIR)'; \
	  if [ "$$app" = auto ]; then app=; if [ -s app/tv/dist/PPSA99500/eboot.bin ]; then app=app/tv/dist/PPSA99500; fi; fi; \
	  if [ -n "$$app" ]; then echo "ffpkg: TV app folder $$app"; else echo "ffpkg: native launcher (no TV app folder)"; fi; \
	  $(PYTHON) tools/make_ffpkg.py $${app:+--app-dir "$$app"}; rc=$$?; \
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
clean: tv-clean
	rm -rf $(BUILD_DIR) $(ELF) xeno-store-browser.elf $(ASSETS) dist app/dist app/native-launcher/build

distclean: clean tv-distclean
	rm -rf web/dist app/native-launcher/.deps app/native-launcher/runtime/libc.prx

# ---- TV app (app/tv) ---------------------------------------------------------
# The native PS5 TV app: Orbit Store 1.2.0's TV app (GPL-3.0-or-later), built on
# BlackBearReloaded's ps5-homebrew-ui kit, vendored in app/tv with its own Makefile and
# pinned toolchain (provenance and licences: app/tv/NOTICE). It needs clang-18, lld-18,
# llvm-18, libclang-rt-18-dev, ninja, wget, unzip and python3; the unit tests and the
# preview also need libwebp-dev and Mesa (libegl-dev, libgl-dev, libegl-mesa0,
# libgl1-mesa-dri), and tv-preview-live python3-pil.
#
#   make tv-deps          the pinned SDK v0.42 + zlib, PacBrew libwebp, ps5-opengl and
#                         GoogleTest, fetched and hash-checked into app/tv/.deps
#   make tv               the app folder app/tv/dist/<titleId> (eboot.bin, sce_module/libc.prx,
#                         assets/, sce_sys/), built without a bundled payload ('make ffpkg'
#                         adds xeno/xeno-store.elf when it packs the image)
#   make tv-check         the lints 'make tv' runs first: param.json against VERSION
#                         (tools/tv/check_param.py), the i18n coverage gate (with the backend
#                         sentences of src/ as warnings), the colour lint and the rebrand check
#   make tv-test          the app's GoogleTest unit tests (ASan + UBSan)
#   make tv-preview       every screen rendered on this machine against the built-in fixture
#                         (Mesa llvmpipe, no display) -> app/tv/build/preview/pictures
#   make tv-probe         the contract probe: the app's own HTTP/JSON/API/model code built for
#                         the host (app/tv/build/host/tv-contract-probe); tests/host/test_tvapi_probe.py
#                         runs it against the backend when XENO_TV_PROBE points at it
#   make tv-preview-live  every screen against the real host backend in a test world
#                         (tools/tv/preview_live.py, tools/tv/README.md) -> build/tv-preview
#                         (index.html, report.json, the PNGs); LANGS=all adds the locale sweep
#   make tv-clean         removes app/tv/build, app/tv/dist and the generated libc.prx;
#   make tv-distclean     also app/tv/.deps
#
# 'make all' and TV_ALL: see the top of this file.
# The app folder is named after sce_sys/param.json's titleId.
TV_TITLE_ID  = $(shell $(PYTHON) -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["titleId"])' $(TV_DIR)/sce_sys/param.json)
TV_APPDIR    = $(TV_DIR)/dist/$(TV_TITLE_ID)
# The app's scripts read these from the environment. PS5_CLANG: tools/clang18-prefixmap
# maps app/tv to /work/app in __FILE__, so eboot.bin is the same from any checkout.
# HOST_CC/HOST_CXX: its host compilers (clang), set so the store's HOST_CC never reaches it.
# -j1: the app Makefile's prerequisites are not written for parallel make; ninja still
# runs the compiles in parallel (BUILD_JOBS, every core by default).
TV_HOST_CC  ?= clang
TV_HOST_CXX ?= clang++
TV_MAKE     := $(MAKE) -j1 -C $(TV_DIR) PS5_CLANG=$(CURDIR)/$(TV_DIR)/tools/clang18-prefixmap \
               USE_CCACHE=0 HOST_CC=$(TV_HOST_CC) HOST_CXX=$(TV_HOST_CXX)
# The preview binary 'make tv-preview' builds; tv-preview-live rebuilds it (with the fixture
# pictures) only when the app's sources are newer.
TV_PREVIEW_BIN := $(TV_DIR)/build/preview/xeno_preview
TV_PREVIEW_SRC := $(shell find $(TV_DIR)/src $(TV_DIR)/host $(TV_DIR)/tools/preview.sh -type f 2>/dev/null)
TV_PREVIEW_LIVE_FLAGS ?=

.PHONY: tv tv-deps tv-check tv-test tv-preview tv-preview-live tv-probe tv-skip tv-clean tv-distclean

tv-deps:
	$(TV_MAKE) deps

tv: tv-check
	$(TV_MAKE) app ORBIT_PAYLOAD=none
	cd $(TV_DIR)/runtime && sha256sum --check --strict libc.prx.sha256
	@set -e; d='$(TV_APPDIR)'; \
	for f in eboot.bin sce_module/libc.prx sce_sys/param.json sce_sys/icon0.png; do \
	  test -s "$$d/$$f" || { echo "make tv: $$d/$$f is missing" >&2; exit 1; }; \
	done; \
	test -d "$$d/assets/fonts" || { echo "make tv: $$d/assets is incomplete" >&2; exit 1; }; \
	cmp "$$d/sce_module/libc.prx" $(TV_DIR)/runtime/libc.prx; \
	echo "TV app folder: $$d"; (cd "$$d" && sha256sum eboot.bin sce_module/libc.prx)

tv-skip:
	@echo "make all: the TV app is left out (it needs clang-18 and 'make tv-deps'; TV=1 forces it)."
	@echo "make all: 'make ffpkg' packs app/tv/dist/PPSA99500 when it holds a built app folder, otherwise the native launcher."

# The lock-step and lint gates (PORT-SPEC 3.4). i18n-coverage.py cross-checks the backend's
# sentences in src/ as warnings only; it fails on a missing or stale catalogue key.
tv-check:
	$(PYTHON) tools/tv/check_param.py VERSION $(TV_DIR)/sce_sys/param.json
	$(PYTHON) $(TV_DIR)/tools/i18n-coverage.py --backend $(SRC_DIR) --quiet
	$(PYTHON) $(TV_DIR)/tools/lint-colors.py
	$(PYTHON) $(TV_DIR)/tools/rebrand-i18n.py --check

tv-test:
	$(TV_MAKE) test

tv-preview:
	rm -rf $(TV_DIR)/build/preview/pictures
	$(TV_MAKE) preview
	@n=$$(find $(TV_DIR)/build/preview/pictures -maxdepth 1 -name '*.png' | wc -l); \
	echo "tv-preview: $$n pictures in $(TV_DIR)/build/preview/pictures"; test "$$n" -gt 0

$(TV_PREVIEW_BIN): $(TV_PREVIEW_SRC)
	$(MAKE) --no-print-directory tv-preview

# Exit status 0 = every check passed, 1 = a check failed, 2 = the run could not start
# (tools/tv/README.md). The output folder keeps its report when a check fails.
tv-preview-live: host $(TV_PREVIEW_BIN)
	$(PYTHON) -I tools/tv/preview_live.py --out $(BUILD_DIR)/tv-preview $(if $(LANGS),--langs $(LANGS)) $(TV_PREVIEW_LIVE_FLAGS)

tv-probe:
	BUILD_JOBS=$(or $(BUILD_JOBS),2) bash $(TV_DIR)/tools/build-contract-probe.sh

tv-clean:
	rm -rf $(TV_DIR)/build $(TV_DIR)/dist $(TV_DIR)/runtime/libc.prx

tv-distclean: tv-clean
	rm -rf $(TV_DIR)/.deps
