#!/usr/bin/env python3
"""CPU/no-model queue regressions. All gates use observed events, never sleep-only ordering.

Usage: queue-lifecycle.py <frontend binary> <vocabulary.gguf>
Failed runs retain their logs and fake-engine command captures in the printed directory.
"""
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest

BIN, MODEL = sys.argv[1:3]
del sys.argv[1:3]
HERE = Path(__file__).resolve().parent


def wait_for(predicate, description, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.01)
    raise AssertionError("timed out: " + description)


class Server:
    def __init__(self, **env):
        self.tmp = tempfile.TemporaryDirectory(prefix="us2-queue-")
        self.path = Path(self.tmp.name)
        self.log = self.path / "server.log"
        self.capture = self.path / "commands.jsonl"
        self.gate = self.path / "release"
        self.cancel_gate = self.path / "cancel-release"
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=12)
        self.output = self.log.open("w")
        self.proc = subprocess.Popen([
            BIN, "--engine", str(HERE / "fake-engine.py"), "-m", MODEL,
            "--host", "127.0.0.1", "--port", str(self.port), "--alias", "fake",
            "--threads-http", "16", "--verbose", "--", "--max-context", "64",
        ], stdout=self.output, stderr=subprocess.STDOUT, env=dict(
            os.environ, CUDA_VISIBLE_DEVICES="-1", NVIDIA_VISIBLE_DEVICES="void",
            FAKE_CONTEXT="64", FAKE_TOKENS="9707", FAKE_CAPTURE=str(self.capture),
            FAKE_GATE=str(self.gate), **env))
        try:
            wait_for(self.ready, "server ready")
        except Exception:
            self.close(False)
            raise

    def ready(self):
        if self.proc.poll() is not None:
            raise AssertionError("frontend exited during startup")
        try:
            c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=0.2)
            c.request("GET", "/health")
            r = c.getresponse()
            r.read()
            c.close()
            return r.status == 200
        except OSError:
            return False

    def events(self):
        if not self.capture.exists():
            return []
        # A concurrent append may leave a partial last line; wait for its newline.
        return [json.loads(line) for line in self.capture.read_text().splitlines(keepends=True)
                if line.endswith("\n")]

    def gens(self):
        return [e["line"] for e in self.events()
                if e["event"] == "command" and e["line"].startswith(("GEN ", "GENI "))]

    def deferred(self, count):
        return wait_for(lambda: len(re.findall(r"defer task, id =", self.log.read_text())) >= count,
                        "%d actual deferred tasks" % count)

    def request(self, tokens=3, stream=False, cap=4):
        body = {"prompt": [100] * tokens, "n_predict": cap, "stream": stream,
                "temperature": 0}
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=8)
        try:
            c.request("POST", "/completion", json.dumps(body), {"Content-Type": "application/json"})
            r = c.getresponse()
            return r.status, r.getheader("Content-Type"), r.read().decode()
        finally:
            c.close()

    def submit(self, **kwargs):
        return self.pool.submit(self.request, **kwargs)

    def leading(self):
        a = self.submit()
        wait_for(lambda: len(self.gens()) == 1, "leading GEN at gate")
        return a

    def close(self, success):
        self.gate.touch()
        self.cancel_gate.touch()
        self.proc.terminate()
        try:
            self.proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=2)
        self.pool.shutdown(wait=True, cancel_futures=True)
        self.output.close()
        if success:
            self.tmp.cleanup()
        else:
            # Keep TemporaryDirectory from deleting failure diagnostics at GC.
            self.tmp._finalizer.detach()
            print("queue diagnostics retained: " + str(self.path), file=sys.stderr)


class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.servers = []
        result = self._outcome.result
        self.failure_count = len(result.failures) + len(result.errors)

    def server(self, **env):
        s = Server(**env)
        self.servers.append(s)
        return s

    def tearDown(self):
        result = self._outcome.result
        failed = len(result.failures) + len(result.errors) > self.failure_count
        for s in self.servers:
            s.close(not failed)

    def assert_ok(self, future):
        status, _, body = future.result(timeout=9)
        self.assertEqual(status, 200, body)
        result = json.loads(body)
        self.assertEqual(result["content"], "Hello")
        self.assertEqual(result["tokens_predicted"], 1)
        return result

    def test_rejection_advances_without_nudge(self):
        for stream in (False, True):
            with self.subTest(stream=stream):
                s = self.server()
                a = s.leading()
                b = s.submit(tokens=56, stream=stream)  # room = 0
                s.deferred(1)
                c = s.submit(tokens=4)
                s.deferred(2)
                s.gate.touch()
                self.assert_ok(a)
                status, content_type, body = b.result(timeout=9)
                self.assertEqual(status, 400, body)
                self.assertNotIn("text/event-stream", content_type)
                error = json.loads(body)["error"]
                self.assertEqual(error["n_prompt_tokens"], 56)
                self.assertEqual(error["n_ctx"], 64)
                self.assert_ok(c)  # No fourth request, health poll, or completion nudge.
                self.assertEqual(len(s.gens()), 2)

    def test_bounded_rejection_chain(self):
        s = self.server()
        a = s.leading()
        rejected = []
        for i in range(6):  # Below the 16 HTTP workers, not a thread-pool exhaustion test.
            rejected.append(s.submit(tokens=57, stream=bool(i % 2)))
            s.deferred(i + 1)
        c = s.submit(tokens=4)
        s.deferred(7)
        s.gate.touch()
        self.assert_ok(a)
        for b in rejected:
            self.assertEqual(b.result(timeout=9)[0], 400)
        self.assert_ok(c)
        self.assertEqual(len(s.gens()), 2)

    def test_admission_and_caps(self):
        s = self.server()
        s.gate.touch()
        for n, cap, expected in [(55, -1, 1), (55, 99, 1), (50, 2, 2), (50, -1, 6)]:
            with self.subTest(n=n, cap=cap):
                self.assert_ok(s.submit(tokens=n, cap=cap))
                self.assertEqual(int(s.gens()[-1].split()[1]), expected)
        for n in (56, 57):
            status, _, body = s.request(tokens=n)
            self.assertEqual(status, 400, body)
        # Empty input can be rejected by HTTP tokenization or launch, but never reaches GEN.
        self.assertEqual(s.request(tokens=0)[0], 400)
        self.assertEqual(len(s.gens()), 4)
        self.assert_ok(s.submit())

    def test_terminal_errors_promote_follower(self):
        for mode in ("err", "mismatch", "nonfinite"):
            with self.subTest(mode=mode):
                s = self.server(FAKE_TERMINALS=json.dumps([mode, "ok"]))
                a = s.leading()
                c = s.submit(tokens=4)
                s.deferred(1)
                s.gate.touch()
                status, _, body = a.result(timeout=9)
                self.assertEqual(status, 500, body)
                self.assert_ok(c)
                self.assertEqual(len(s.gens()), 2)

    def test_eof_does_not_generate_successor(self):
        s = self.server(FAKE_TERMINALS='["eof"]')
        a = s.leading()
        c = s.submit(tokens=4)
        s.deferred(1)
        s.gate.touch()
        # Supervisor may close HTTP before a result is delivered. Neither client may succeed.
        for future in (a, c):
            try:
                self.assertNotEqual(future.result(timeout=9)[0], 200)
            except (OSError, http.client.HTTPException):
                pass
        wait_for(lambda: s.proc.poll() is not None, "frontend shutdown after engine EOF")
        self.assertEqual(len(s.gens()), 1)

    def test_deferred_cancel_never_reaches_engine(self):
        s = self.server()
        a = s.leading()
        c = http.client.HTTPConnection("127.0.0.1", s.port, timeout=8)
        c.request("POST", "/completion", json.dumps({"prompt": [100] * 5}),
                  {"Content-Type": "application/json"})
        s.deferred(1)
        target = re.findall(r"defer task, id = (\d+)", s.log.read_text())[-1]
        follower = s.submit(tokens=4)
        s.deferred(2)
        c.sock.shutdown(socket.SHUT_RDWR)
        c.close()
        # The reader logs "cancel task" BEFORE post(). The queue's subsequent front=1 post log is
        # emitted under its mutex AFTER cleanup_pending_task has removed B from the deferred queue.
        cancelled_and_posted = re.compile(r"cancel task, id_task = " + target +
                                        r"\n[\s\S]*new task, id = \d+/1, front = 1\n")
        wait_for(lambda: cancelled_and_posted.search(s.log.read_text()),
                 "deferred cancellation cleanup completed")
        s.gate.touch()
        self.assert_ok(a)
        self.assert_ok(follower)
        self.assertEqual(len(s.gens()), 2)
        self.assertEqual([len(line.split()[-1].split(",")) for line in s.gens()], [3, 4])

    def test_active_cancel_drains_residual_before_follower(self):
        with tempfile.TemporaryDirectory(prefix="us2-cancel-") as tmp:
            gate = Path(tmp) / "release"
            s = self.server(FAKE_N_TOKENS="1000", FAKE_TOKEN_ID="9707", FAKE_DELAY_MS="5",
                            FAKE_CANCEL_GATE=str(gate))
            c = http.client.HTTPConnection("127.0.0.1", s.port, timeout=8)
            c.request("POST", "/completion", json.dumps({"prompt": [100] * 3,
                      "stream": True, "n_predict": 50}), {"Content-Type": "application/json"})
            response = c.getresponse()  # begin headers are sent before the gated engine reply
            wait_for(lambda: len(s.gens()) == 1, "active request GEN")
            follower = s.submit(tokens=4, cap=1)
            s.deferred(1)
            # Abort the socket rather than merely abandoning a buffered response object.
            c.sock.shutdown(socket.SHUT_RDWR)
            response.close()
            c.close()
            wait_for(lambda: "cancelled by the client" in s.log.read_text(), "active cancellation")
            s.gate.touch()
            wait_for(lambda: any(e["event"] == "residual" for e in s.events()), "residual output before DONE")
            self.assertEqual(len(s.gens()), 1)
            self.assertFalse(follower.done())
            gate.touch()
            result = follower.result(timeout=9)
            self.assertEqual(result[0], 200, result)
            self.assertEqual(json.loads(result[2])["content"], "Hello")
            self.assertEqual(json.loads(result[2])["tokens_predicted"], 1)
            self.assertEqual(len(s.gens()), 2)


if __name__ == "__main__":
    unittest.main()
