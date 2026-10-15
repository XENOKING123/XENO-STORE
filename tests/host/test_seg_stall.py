#!/usr/bin/env python3
"""
test_seg_stall.py — segment threads never outlive their job.

On the console a segment can sit in sceHttpSendRequest, which cannot be
aborted, for longer than any fixed wait. XENO_HOST_SEG_STALL_MS models that:
every ranged segment request opened in the first STALL_S seconds blocks until
then and ignores the stop request. While those threads are alive the job worker
must keep waiting, so

  * Remove after Pause is deferred (the job and its chunk table stay allocated)
    and the job is reaped once the stalled requests return,
  * Resume after Pause leaves the job queued (no new chunk plan) until the old
    segments are gone, then the download completes with the right bytes,
  * the job log notes the stuck segment after 60 s.

Slow by design (~80 s): the stall has to outlast the old 60 s wait cap.
"""
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402
import range_server  # noqa: E402

STALL_S = 75
CHECK_UNTIL_S = 64   # after the pause: past the old cap, still inside the stall


def ctl(rs, cmd):
    with urllib.request.urlopen(f"{rs}/ctl/{cmd}", timeout=10) as r:
        return json.loads(r.read())


class SegmentStallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="xeno-segstall-")
        cls.httpd, cls.rport = range_server.serve_in_thread(os.path.join(cls.tmp, "files"))
        cls.rs = f"http://127.0.0.1:{cls.rport}"
        cls.srv = harness.XenoServer(os.path.join(cls.tmp, "data"), env={"XENO_HOST_SEG_STALL_MS": str(STALL_S * 1000)}).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        cls.httpd.shutdown()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_stalled_segments_keep_the_job_alive(self):
        srv = self.srv
        st, _ = srv.api("PUT", "/api/settings", {"downloads": {"maxBytesPerSec": 0, "concurrentJobs": 2, "connections": 4, "turboStart": False}})
        self.assertEqual(st, 200)
        a = srv.add(url=f"{self.rs}/ok/big.bin", filename="stall-a.bin", format="BIN")["id"]
        b = srv.add(url=f"{self.rs}/ok/mid.bin", filename="stall-b.bin", format="BIN")["id"]
        for jid in (a, b):
            srv.wait_job(jid, None, timeout=30, until=lambda j: j["state"] == "downloading" and j["written"] > 0 and j["activeSegments"] > 0)
        t_pause = time.time()
        for jid in (a, b):
            self.assertEqual(srv.action(jid, "pause")[0], 200)
        self.assertEqual(srv.action(a, "remove", {"deleteFiles": False})[0], 200)
        self.assertEqual(srv.action(b, "resume")[0], 200)

        # Past the old 60 s cap the stalled requests are still blocked: the
        # removed job must still exist and the resumed one must not restart.
        while time.time() - t_pause < CHECK_UNTIL_S:
            ja, jb = srv.job(a), srv.job(b)
            self.assertIsNotNone(ja, "removed job freed while its segments are still alive")
            self.assertEqual(jb["state"], "queued", "resumed job restarted while its old segments are still alive")
            self.assertIsNone(srv.proc.poll())
            time.sleep(1)
        self.assertIn("still stuck in a network call", srv.log_text())

        # The stall ends: the removed job is reaped, the resumed one finishes.
        harness.wait_until(lambda: srv.job(a) is None, timeout=STALL_S, what="removed job reaped")
        jb = srv.wait_job(b, ("done", "failed"), timeout=STALL_S + 30)
        self.assertEqual(jb["state"], "done", json.dumps(jb)[:600])
        self.assertEqual(harness.sha256_file(jb["path"]), ctl(self.rs, "sha256/mid.bin")["sha256"])
        self.assertIsNone(srv.proc.poll())
        self.assertNotIn("AddressSanitizer", srv.log_text())


if __name__ == "__main__":
    unittest.main()
