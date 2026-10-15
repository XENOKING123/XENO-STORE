#!/usr/bin/env python3
# Copyright (C) 2026 XENO STORE contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
fake_loader.py - a stand-in for the PS5 ELF loader (elfldr on :9021) on the
host, so the TV app's start and restart run for real in the live preview
(PORT-SPEC 4.1).

The TV app's starter connects, sends the raw bytes of an ELF and closes; the
console's loader runs it. This one:

  1. accepts a connection on 127.0.0.1:<port> and reads until the sender closes
     (at most 64 MiB + 1, like the starter's own limit);
  2. writes the bytes to a temp file and checks them: the ELF magic, ELFCLASS64,
     little-endian and e_machine x86-64 (62);
  3. logs "received N bytes" (stdout and the JSON-lines --log);
  4. when the bytes are one of the --host-binary files (the world's bundled
     xeno-store, or its saved update), runs that file as
         <file> --port P --data D --no-browser [--root R]... [--log-level L]
     with its output in --child-log-dir, after --delay seconds (the console's
     loader is not instant either, and the app's "starting" screen needs a
     moment to be captured); when they are one of the --plain files (a
     stand-in that exits at once, for the crash fallback), runs it at once
     with no arguments; any other ELF is logged and not run;
  5. with --record-only it only records (the "is anything relaunching itself"
     sentinel on the backend's own XENO_PORT_LOADER).

On SIGTERM/SIGINT it stops every child it started (SIGTERM, then SIGKILL after
10 s) and exits. The children get this process's environment, so start it with
the backend's environment (XENO_HOST_ART_FIXTURE, XENO_TVAPI_UPDATES, ...).

Log lines (--log, one JSON object per line, also echoed to stdout):
  {"event":"listening","port":N}
  {"event":"received","bytes":N,"sha256":..,"elf":true,"x86_64":true,"match":"host|plain|none",
   "file":<matched path or "">,"seq":k,"t":<unix>}
  {"event":"started","pid":N,"argv":[...],"log":<child log path>,"seq":k,"t":..}
  {"event":"exited","pid":N,"rc":N,"seconds":S,"seq":k,"t":..}
  {"event":"rejected","why":..,"bytes":N,"seq":k,"t":..}

    python3 -I tools/tv/fake_loader.py --port 9021 --host-binary build/host/xeno-store \
        --backend-port 9095 --data /tmp/w/data --root /tmp/w/R1 --log /tmp/w/loader.jsonl

Python 3 standard library only.
"""
import argparse
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

MAX_ELF = 64 * 1024 * 1024
EM_X86_64 = 62


def elf_info(data):
    """(is_elf, is_x86_64_elf64_le, why)"""
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return False, False, "no ELF magic"
    if data[4] != 2:
        return True, False, "not ELFCLASS64"
    if data[5] != 1:
        return True, False, "not little-endian"
    machine = int.from_bytes(data[18:20], "little")
    if machine != EM_X86_64:
        return True, False, f"e_machine {machine}, not x86-64"
    return True, True, ""


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class FakeLoader:
    def __init__(self, port, host_binaries=(), plain=(), backend_port=None, data=None, roots=(),
                 log_level="debug", log_path=None, child_log_dir=None, record_only=False,
                 host="127.0.0.1", keep_files=False, extra_args=(), delay=0.0):
        self.port, self.host = port, host
        self.backend_port, self.data, self.roots = backend_port, data, list(roots)
        self.log_level, self.extra_args = log_level, list(extra_args)
        self.record_only, self.keep_files = record_only, keep_files
        self.delay = max(0.0, float(delay))
        self.log_path = log_path
        self.child_log_dir = os.path.abspath(child_log_dir or tempfile.mkdtemp(prefix="fake-loader."))
        os.makedirs(self.child_log_dir, exist_ok=True)
        self.known = {}                         # sha256 -> ("host"|"plain", path)
        for p in plain:
            self.known[sha256_file(p)] = ("plain", os.path.abspath(p))
        for p in host_binaries:
            self.known[sha256_file(p)] = ("host", os.path.abspath(p))
        self.children = []                      # (Popen, seq, start time, log file)
        self.seq = 0
        self.mu = threading.Lock()
        self.sock = None
        self.stopping = threading.Event()
        self.events = []

    # ---- logging ------------------------------------------------------------
    def emit(self, **ev):
        ev.setdefault("t", round(time.time(), 3))
        line = json.dumps(ev, ensure_ascii=False)
        with self.mu:
            self.events.append(ev)
            if self.log_path:
                with open(self.log_path, "a", encoding="utf-8") as f:
                    f.write(line + "\n")
        print(line, flush=True)

    # ---- serving ------------------------------------------------------------
    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((self.host, self.port))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.emit(event="listening", port=self.port, recordOnly=self.record_only,
                  known=[{"kind": k, "file": p} for k, p in self.known.values()])
        threading.Thread(target=self._accept_loop, name="fake-loader", daemon=True).start()
        threading.Thread(target=self._reap_loop, name="fake-loader-reap", daemon=True).start()
        return self

    def _accept_loop(self):
        while not self.stopping.is_set():
            try:
                conn, _ = self.sock.accept()
            except OSError:
                break
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn):
        with self.mu:
            self.seq += 1
            seq = self.seq
        chunks, n = [], 0
        conn.settimeout(30)
        try:
            while True:
                b = conn.recv(1 << 16)
                if not b:
                    break
                chunks.append(b)
                n += len(b)
                if n > MAX_ELF + 1:
                    break
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass
        data = b"".join(chunks)
        sha = hashlib.sha256(data).hexdigest()
        is_elf, ok, why = elf_info(data)
        kind, path = self.known.get(sha, ("none", ""))
        print(f"fake_loader: received {len(data)} bytes", flush=True)
        self.emit(event="received", bytes=len(data), sha256=sha, elf=is_elf, x86_64=ok, match=kind, file=path, seq=seq)
        if len(data) > MAX_ELF:
            self.emit(event="rejected", why="larger than 64 MiB", bytes=len(data), seq=seq)
            return
        if not ok:
            self.emit(event="rejected", why=why, bytes=len(data), seq=seq)
            return
        # The bytes go through a temp file, as the console's loader maps them;
        # the run uses the known file itself so the process name stays readable.
        fd, tmp = tempfile.mkstemp(prefix=f"elf-{seq}-", suffix=".elf", dir=self.child_log_dir)
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        if not self.keep_files:
            os.unlink(tmp)
        if self.record_only or kind == "none":
            if kind == "none":
                self.emit(event="rejected", why="an ELF this world does not know: not run", bytes=len(data), seq=seq)
            return
        self._run(kind, path, seq)

    def _run(self, kind, path, seq):
        if kind == "host" and self.delay and self.stopping.wait(self.delay):
            return
        argv = [path]
        if kind == "host":
            argv += ["--port", str(self.backend_port), "--data", self.data, "--no-browser"]
            for r in self.roots:
                argv += ["--root", r]
            if self.log_level:
                argv += ["--log-level", self.log_level]
            argv += self.extra_args
        log = os.path.join(self.child_log_dir, f"child-{seq}-{kind}.log")
        env = dict(os.environ)
        for k in ("HTTPS_PROXY", "HTTP_PROXY", "https_proxy", "http_proxy"):
            env.pop(k, None)
        lf = open(log, "ab")
        try:
            p = subprocess.Popen(argv, stdout=lf, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                                 start_new_session=True)
        except OSError as e:
            lf.close()
            self.emit(event="rejected", why=f"cannot run: {e}", bytes=0, seq=seq)
            return
        with self.mu:
            self.children.append((p, seq, time.time(), lf))
        self.emit(event="started", pid=p.pid, argv=argv, log=log, kind=kind, seq=seq)

    def _reap_loop(self):
        while not self.stopping.is_set():
            with self.mu:
                live = list(self.children)
            for p, seq, t0, lf in live:
                rc = p.poll()
                if rc is not None:
                    with self.mu:
                        self.children = [c for c in self.children if c[0] is not p]
                    lf.close()
                    self.emit(event="exited", pid=p.pid, rc=rc, seconds=round(time.time() - t0, 2), seq=seq)
            time.sleep(0.1)

    def stop(self, timeout=10.0):
        self.stopping.set()
        try:
            self.sock.close()
        except OSError:
            pass
        with self.mu:
            live = list(self.children)
        for p, *_ in live:
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        deadline = time.time() + timeout
        for p, seq, t0, lf in live:
            try:
                rc = p.wait(timeout=max(0.1, deadline - time.time()))
            except subprocess.TimeoutExpired:
                p.kill()
                rc = p.wait()
            lf.close()
            self.emit(event="exited", pid=p.pid, rc=rc, seconds=round(time.time() - t0, 2), seq=seq, stopped=True)


def read_events(path):
    """The JSON-lines log of a fake loader, as a list of dicts ([] when missing)."""
    out = []
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        out.append(json.loads(line))
                    except ValueError:
                        pass
    except OSError:
        pass
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="A stand-in for the PS5 ELF loader (see the module docstring).")
    ap.add_argument("--port", type=int, required=True, help="port to listen on (0 = any)")
    ap.add_argument("--host-binary", action="append", default=[], help="an ELF to run as the backend (repeatable)")
    ap.add_argument("--plain", action="append", default=[], help="an ELF to run with no arguments (repeatable)")
    ap.add_argument("--backend-port", type=int, help="--port for the backend")
    ap.add_argument("--data", help="--data for the backend")
    ap.add_argument("--root", action="append", default=[], help="--root for the backend (repeatable)")
    ap.add_argument("--log-level", default="debug")
    ap.add_argument("--log", help="JSON-lines event log")
    ap.add_argument("--child-log-dir")
    ap.add_argument("--record-only", action="store_true")
    ap.add_argument("--keep-files", action="store_true", help="keep the received bytes in --child-log-dir")
    ap.add_argument("--delay", type=float, default=0.0, help="seconds to wait before running a --host-binary copy")
    a = ap.parse_args(argv)
    if a.host_binary and not a.record_only and (a.backend_port is None or not a.data):
        ap.error("--host-binary needs --backend-port and --data")
    loader = FakeLoader(a.port, a.host_binary, a.plain, a.backend_port, a.data, a.root, a.log_level, a.log,
                        a.child_log_dir, a.record_only, keep_files=a.keep_files, delay=a.delay)
    done = threading.Event()

    def on_signal(*_):
        done.set()

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    loader.start()
    print(f"fake_loader listening on 127.0.0.1:{loader.port}", flush=True)
    while not done.wait(0.5):
        pass
    loader.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
