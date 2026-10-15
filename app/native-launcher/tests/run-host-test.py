#!/usr/bin/env python3
# XENO STORE - host test of the native launcher's logic (src/launcher.c).
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds src/launcher.c for Linux with tests/host_shim.c (the console calls)
# and tests/host_compat.h, on free loopback ports instead of 9095/9021 and a
# temporary file instead of /app0/xeno-store.elf, then runs it against a fake
# store and a fake ELF loader:
#   store-up     the store answers 200: browser opened, the loader untouched
#   cold-start   no store: the ELF goes to the loader byte for byte, the store
#                comes up 1 s later answering 503 once, then 200
#   no-loader    nothing listens: 6 connect attempts, status 1
#   non-200      a store answers 404 (no /api/launcher/open): no second store is started
#   bad-elf      the bundled file is not an ELF: the loader is not contacted
#   loader-drops the loader stops reading part way: reported, status 1
#   missing-elf  the bundled file is missing
#   timeout      the loader takes the ELF but no store ever answers (30 s)
# Every run must end with sceSystemServiceLoadExec("exit").
#
# usage: [VERBOSE=1] python3 tests/run-host-test.py [--cc gcc] [--skip-timeout]

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server(threading.Thread):
    """A one-thread TCP server; `handle(conn)` serves each connection."""

    def __init__(self, port, handle):
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(8)
        self.sock.settimeout(0.2)
        self.handle = handle
        self.stopped = False
        self.start()

    def run(self):
        while not self.stopped:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with conn:
                conn.settimeout(10)
                try:
                    self.handle(conn)
                except OSError:
                    pass

    def stop(self):
        self.stopped = True
        self.join(2)
        self.sock.close()


class FakeStore:
    def __init__(self, port, statuses):
        self.statuses = list(statuses)   # one per request; the last one repeats
        self.requests = []
        self.server = Server(port, self.handle)

    def handle(self, conn):
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(4096)
            if not chunk:
                return
            data += chunk
        head, _, body = data.partition(b"\r\n\r\n")
        length = 0
        for line in head.split(b"\r\n")[1:]:
            name, _, value = line.partition(b":")
            if name.strip().lower() == b"content-length":
                length = int(value.strip())
        while len(body) < length:
            body += conn.recv(4096)
        self.requests.append((head.decode(), body))
        status = self.statuses.pop(0) if len(self.statuses) > 1 else self.statuses[0]
        payload = b'{"ok":true,"launched":true,"alreadyOpen":false,"url":"http://127.0.0.1/?tv=1"}'
        conn.sendall(("%s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
                      "Connection: close\r\n\r\n" % (status, len(payload))).encode() + payload)

    def stop(self):
        self.server.stop()


class FakeLoader:
    """Reads one payload to the end of the stream, then (like elfldr) keeps the
    connection as the payload's stdout and writes to it."""

    def __init__(self, port, on_payload=None):
        self.payloads = []
        self.errors = []
        self.on_payload = on_payload
        self.server = Server(port, self.handle)

    def handle(self, conn):
        data = b""
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                data += chunk
        except ConnectionResetError:
            self.errors.append("reset after %d bytes" % len(data))
            return
        self.payloads.append(data)
        if self.on_payload:
            self.on_payload()
        try:
            conn.sendall(b"[store] stdout line from the started payload\n")
        except OSError:
            pass

    def stop(self):
        self.server.stop()


def build(cc, out, store_port, loader_port, elf_path):
    cmd = [cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
           "-include", os.path.join(HERE, "host_compat.h"),
           "-DSTORE_PORT=%d" % store_port, "-DLOADER_PORT=%d" % loader_port,
           '-DSTORE_ELF="%s"' % elf_path,
           "-c", os.path.join(ROOT, "src", "launcher.c"), "-o", out + ".launcher.o"]
    subprocess.run(cmd, check=True)
    subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-c", os.path.join(HERE, "host_shim.c"), "-o", out + ".shim.o"], check=True)
    subprocess.run([cc, out + ".launcher.o", out + ".shim.o", "-o", out], check=True)


def write_elf(path, size=300 * 1024):
    head = bytearray(64)
    head[0:6] = b"\x7fELF\x02\x01"
    head[18] = 62   # EM_X86_64
    with open(path, "wb") as fh:
        fh.write(bytes(head) + os.urandom(size - 64))


def run(binary, timeout=60):
    started = time.monotonic()
    proc = subprocess.run([binary], capture_output=True, text=True, timeout=timeout)
    if os.environ.get("VERBOSE"):
        sys.stdout.write(proc.stderr)
    return proc.returncode, proc.stderr, time.monotonic() - started


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=shutil.which("cc") or "gcc")
    ap.add_argument("--skip-timeout", action="store_true", help="skip the 30 s timeout case")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="xeno-launcher-test-")
    store_port, loader_port = free_port(), free_port()
    elf = os.path.join(work, "xeno-store.elf")
    binary = os.path.join(work, "launcher")
    build(args.cc, binary, store_port, loader_port, elf)
    failures = []

    def check(name, condition, log):
        if not condition:
            failures.append(name)
            print("FAIL %s\n%s" % (name, log))

    def expect_exit(name, rc, log, status):
        check(name + ": status %d" % status, "done (status %d)" % status in log, log)
        check(name + ": LoadExec exit", 'sceSystemServiceLoadExec("exit")' in log and rc == 0, log)

    # store-up
    write_elf(elf)
    store = FakeStore(store_port, ["HTTP/1.0 200 OK"])
    loader = FakeLoader(loader_port)
    rc, log, took = run(binary)
    store.stop(); loader.stop()
    expect_exit("store-up", rc, log, 0)
    check("store-up: opened", "store is running - browser opened" in log, log)
    check("store-up: one request", len(store.requests) == 1, log)
    head, body = store.requests[0] if store.requests else ("", b"")
    check("store-up: request line", head.startswith("POST /api/launcher/open HTTP/1.0\r\n"), head)
    check("store-up: host header", "Host: 127.0.0.1:%d" % store_port in head, head)
    check("store-up: body", body == b"{}", repr(body))
    check("store-up: loader untouched", not loader.payloads, log)
    print("store-up     %.2fs" % took)

    # cold-start: the loader "starts" the store 1 s after it has the payload
    store_box = {}

    def start_store_later():
        def later():
            time.sleep(1.0)
            store_box["store"] = FakeStore(store_port, ["HTTP/1.1 503 Service Unavailable", "HTTP/1.1 200 OK"])
        threading.Thread(target=later, daemon=True).start()

    loader = FakeLoader(loader_port, start_store_later)
    rc, log, took = run(binary)
    loader.stop()
    store = store_box.get("store")
    if store:
        store.stop()
    expect_exit("cold-start", rc, log, 0)
    with open(elf, "rb") as fh:
        expected = fh.read()
    check("cold-start: payload byte for byte", loader.payloads == [expected], log)
    check("cold-start: handed over", "store handed to the ELF loader (%d bytes)" % len(expected) in log, log)
    check("cold-start: 503 then 200", store is not None and len(store.requests) == 2, log)
    check("cold-start: opened", "store started after" in log, log)
    print("cold-start   %.2fs" % took)

    # no-loader
    rc, log, took = run(binary)
    expect_exit("no-loader", rc, log, 1)
    check("no-loader: 6 attempts", log.count("connect ") == 6 and "connect 6/6 failed" in log, log)
    check("no-loader: message", "no ELF loader on 127.0.0.1:%d" % loader_port in log, log)
    print("no-loader    %.2fs" % took)

    # non-200
    store = FakeStore(store_port, ["HTTP/1.1 404 Not Found"])
    rc, log, took = run(binary)
    store.stop()
    expect_exit("non-200", rc, log, 1)
    check("non-200: not treated as opened", 'answered "HTTP/1.1 404 Not Found" - not 200' in log, log)
    check("non-200: no second store", "connect 1/6" not in log and "not starting a second one" in log, log)
    print("non-200      %.2fs" % took)

    # bad-elf
    with open(elf, "wb") as fh:
        fh.write(b"#!/bin/sh\necho not an elf\n" * 100)
    loader = FakeLoader(loader_port)
    rc, log, took = run(binary)
    time.sleep(0.3)
    loader.stop()
    expect_exit("bad-elf", rc, log, 1)
    check("bad-elf: refused", "is not an x86-64 ELF - not sent" in log, log)
    check("bad-elf: loader not contacted", not loader.payloads and not loader.errors,
          "%s\n%s" % (log, loader.errors))
    check("bad-elf: no connect", "ELF loader connected" not in log, log)
    print("bad-elf      %.2fs" % took)

    # loader-drops: the loader stops reading part way through a 16 MiB payload
    write_elf(elf, 16 * 1024 * 1024)

    def drop(conn):
        conn.recv(65536)
        drop.calls += 1

    drop.calls = 0
    server = Server(loader_port, drop)
    rc, log, took = run(binary)
    server.stop()
    expect_exit("loader-drops", rc, log, 1)
    check("loader-drops: reported", "the ELF loader stopped taking the store after" in log, log)
    check("loader-drops: not reported as sent", "store handed to the ELF loader" not in log, log)
    print("loader-drops %.2fs" % took)

    # missing-elf
    os.remove(elf)
    rc, log, took = run(binary)
    expect_exit("missing-elf", rc, log, 1)
    check("missing-elf: message", "cannot open %s" % elf in log, log)
    print("missing-elf  %.2fs" % took)

    # timeout
    if not args.skip_timeout:
        write_elf(elf)
        loader = FakeLoader(loader_port)
        rc, log, took = run(binary, timeout=90)
        loader.stop()
        expect_exit("timeout", rc, log, 1)
        check("timeout: message", "did not open within 30 s" in log, log)
        check("timeout: ~30 s", 29.5 <= took <= 40, "took %.2fs" % took)
        check("timeout: periodic log", log.count("nothing answers on") >= 5, log)
        print("timeout      %.2fs" % took)

    shutil.rmtree(work, ignore_errors=True)
    if failures:
        print("host test: %d failure(s): %s" % (len(failures), ", ".join(failures)))
        return 1
    print("host test: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
