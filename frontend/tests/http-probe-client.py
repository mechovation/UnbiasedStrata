#!/usr/bin/env python3
"""Real loopback HTTP qualification for the opt-in PR4 candidate (stdlib only).

Build probe separately with C++17; the frontend must have its real companion guard
and reader beside it. Run INSIDE the main agent's offline CPU qualification container:
  timeout -k 10s 1800s python3 frontend/tests/http-probe-client.py \
    --server /tmp/ub-pr4-native-clean/bin/unbiased-strata \
    --engine /tmp/pr4-completion/pr4-http-engine \
    --model /assets/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf \
    --projector /projector.gguf --profile frontend/tests/data/vision-runtime-cpu.json \
    --output /tmp/pr4-completion/http

The output directory must be persistent/bind-mounted, not the small service /tmp.
No downloads, Docker operations, GPU, credentials, semantic answer assumptions, or
production services. Gates are fixture files, never inherited test environment.
Use --only SUBSTRING to rerun selected unittest names. Logs and report.json persist.
"""
import argparse
import base64
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest
import zlib

ARGS = None


def wait(predicate, description, timeout=15):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        value = predicate()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError('timeout: ' + description)


def png(variant=0, size=112):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    pixels = bytearray()
    for y in range(size):
        pixels.append(0)
        for x in range(size):
            pixels.extend(((x * 3 + variant * 91) % 256, (y * 5 + variant * 43) % 256,
                           ((x ^ y) * 7 + variant * 113) % 256))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


def bmp(width=56, height=56):
    stride = (width * 3 + 3) & ~3
    pixels = (bytes([33, 81, 203]) * width + bytes(stride - width * 3)) * height
    return (b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54)
            + struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels)


def image(data, mime='image/png'):
    return {'type': 'image_url', 'image_url': {'url': 'data:' + mime + ';base64,' + base64.b64encode(data).decode()}}


def chat(pictures=(), stream=False):
    return {'model': 'pr4-http', 'messages': [{'role': 'user', 'content': [
        {'type': 'text', 'text': 'Describe briefly.'}, *pictures]}],
        'max_tokens': 1, 'temperature': 0, 'stream': stream,
        'chat_template_kwargs': {'enable_thinking': False}}


def text(tokens=3, stream=False):
    return {'prompt': [100] * tokens, 'n_predict': 1, 'temperature': 0, 'stream': stream}


def identity(pid):
    try:
        stat = Path('/proc/%d/stat' % pid).read_text().rsplit(')', 1)[1].split()
        return int(stat[1]), stat[0], stat[19]  # ppid, state, starttime
    except (OSError, ValueError, IndexError):
        return None


class Server:
    serial = 0

    def __init__(self, name, controls=None, profile=None, projector=None):
        Server.serial += 1
        self.path = ARGS.output / ('%02d-%s' % (Server.serial, name))
        self.path.mkdir(parents=True)
        self.log = self.path / 'server.log'
        self.events_path = self.path / 'events.jsonl'
        self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=20)
        self.known = {}
        self.peaks = {}
        self.lock = threading.Lock()
        self.tracking = True
        for key, value in (controls or {}).items():
            self.control(key, value)
        config = json.loads(ARGS.profile.read_text())
        config.update(profile or {})
        profile_path = self.path / 'profile.json'
        profile_path.write_text(json.dumps(config, indent=2) + '\n')
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.output = self.log.open('w')
        self.command = [str(ARGS.server), '--engine', str(ARGS.engine), '-m', str(ARGS.model),
                        '--mmproj', str(projector or ARGS.projector), '--native-vision', str(profile_path),
                        '--image-max-tokens', '64', '--threads', '4', '--threads-http', '8',
                        '--picture-cache', '2', '--jinja', '--host', '127.0.0.1', '--port', str(self.port),
                        '--alias', 'pr4-http', '--verbose', '--', '--fixture-dir', str(self.path)]
        (self.path / 'command.json').write_text(json.dumps(self.command, indent=2) + '\n')
        self.proc = subprocess.Popen(self.command, stdout=self.output, stderr=subprocess.STDOUT,
                                     start_new_session=True, env=dict(os.environ, CUDA_VISIBLE_DEVICES='-1',
                                     NVIDIA_VISIBLE_DEVICES='void'))
        self.tracker = threading.Thread(target=self.track, daemon=True)
        self.tracker.start()

    def control(self, name, value='1'):
        (self.path / name).write_text(str(value) + '\n')

    def track(self):
        while self.tracking:
            self.discover()
            time.sleep(.02)

    def discover(self):
        entries = {}
        for p in Path('/proc').iterdir():
            if p.name.isdigit():
                ident = identity(int(p.name))
                if ident:
                    entries[int(p.name)] = ident
        family = {self.proc.pid}
        with self.lock:
            family.update(pid for pid, start in self.known.items()
                          if pid in entries and entries[pid][2] == start)
            for _ in range(5):
                family.update(pid for pid, ident in entries.items() if ident[0] in family)
            for pid in family - {self.proc.pid}:
                if pid in entries:
                    self.known[pid] = entries[pid][2]
            for pid in family:
                try:
                    role = Path(os.readlink('/proc/%d/exe' % pid)).name
                    values = dict(line.split(':',1) for line in Path('/proc/%d/status' % pid).read_text().splitlines() if ':' in line)
                    sample = {'rss_kib':int(values.get('VmRSS','0').split()[0]),
                              'virtual_kib':int(values.get('VmSize','0').split()[0]),
                              'fds':len(list(Path('/proc/%d/fd' % pid).iterdir())),
                              'threads':int(values.get('Threads','0'))}
                    peak = self.peaks.setdefault(role,{})
                    for key,value in sample.items(): peak[key] = max(peak.get(key,0),value)
                except (OSError,ValueError):
                    pass

    def alive_children(self):
        self.discover()
        with self.lock:
            children = list(self.known.items())
        alive = []
        for pid, start in children:
            current = identity(pid)
            if current and current[2] == start and current[1] != 'Z':
                alive.append(pid)
        return alive

    def readers(self):
        result = []
        for pid in self.alive_children():
            try:
                if Path(os.readlink('/proc/%d/exe' % pid)).name == 'strata-vision-reader':
                    result.append(pid)
            except OSError:
                pass
        return result

    def events(self, kind=None):
        try:
            lines = self.events_path.read_text().splitlines(keepends=True)
        except FileNotFoundError:
            return []
        events = [json.loads(s) for s in lines if s.endswith('\n')]
        return [e for e in events if kind is None or e['event'] == kind]

    def gens(self):
        return self.events('gen')

    def http(self, route='/health', body=None, timeout=40):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=timeout)
        try:
            conn.request('GET' if body is None else 'POST', route,
                         None if body is None else json.dumps(body), {'Content-Type': 'application/json'})
            response = conn.getresponse()
            return response.status, response.getheader('Content-Type', ''), response.read().decode()
        finally:
            conn.close()

    def ready(self):
        if self.proc.poll() is not None:
            raise AssertionError('startup exited %s; see %s' % (self.proc.returncode, self.log))
        try:
            return self.http(timeout=.2)[0] == 200
        except (OSError, http.client.HTTPException):
            return False

    def start(self):
        wait(self.ready, 'global HTTP READY: ' + str(self.log), 180)
        props = json.loads(self.http('/props')[2])
        assert props['vision_configured'] is True, props
        assert props['vision_status'] == 'available', props
        return self

    def submit(self, route='/completion', body=None):
        return self.pool.submit(self.http, route, text() if body is None else body)

    def deferred(self, count):
        return wait(lambda: len(re.findall(r'defer task, id =', self.log.read_text())) >= count,
                    '%d queue deferrals' % count)

    def raw(self, body, route='/completion'):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=20)
        conn.request('POST', route, json.dumps(body), {'Content-Type': 'application/json'})
        return conn

    @staticmethod
    def disconnect(conn):
        if conn.sock:
            conn.sock.shutdown(socket.SHUT_RDWR)
        conn.close()

    def no_orphans(self):
        def reaped():
            self.discover()
            with self.lock:
                children = list(self.known.items())
            # A zombie is not proof of reaping either. Compare starttime to avoid
            # mistaking an unrelated reused PID for one of our children.
            return all((identity(pid) or (0, '', ''))[2] != start for pid, start in children)
        wait(reaped, 'all owned reader/guard/engine children reaped (including zombies)', 10)

    def exited_failed(self):
        wait(lambda: self.proc.poll() is not None, 'fail-closed service exit', 20)
        assert self.proc.returncode != 0, 'fatal native failure returned success'
        self.no_orphans()

    def close(self):
        for i in range(1, 30):
            self.control('release-%d' % i)
            self.control('done-release-%d' % i)
        self.control('startup-release')
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            self.proc.wait(timeout=12)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait(timeout=3)
        self.pool.shutdown(wait=True, cancel_futures=True)
        try:
            self.no_orphans()
        finally:
            self.tracking = False
            self.tracker.join(timeout=2)
            # Cleanup only our isolated process group, never external services.
            for pid in self.alive_children():
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            self.output.close()
            (self.path / 'resource-peaks.json').write_text(json.dumps(self.peaks,indent=2)+'\n')


class Qualification(unittest.TestCase):
    def setUp(self):
        self.servers = []

    def tearDown(self):
        for s in reversed(self.servers):
            s.close()

    def server(self, **kwargs):
        s = Server(self._testMethodName, **kwargs)
        self.servers.append(s)
        return s

    def ok(self, result, stream=False, chat_response=False):
        status, content_type, body = result
        self.assertEqual(status, 200, body)
        if stream:
            self.assertIn('text/event-stream', content_type)
            events = [line[6:] for line in body.splitlines() if line.startswith('data: ')]
            self.assertTrue(events, body)
            parsed = [json.loads(e) for e in events if e != '[DONE]']
            self.assertTrue(parsed, body)
            self.assertFalse(any('error' in e for e in parsed), body)
            if chat_response:
                self.assertIn('[DONE]', events)
        else:
            parsed = json.loads(body)
            self.assertNotIn('error', parsed)
            if chat_response:
                self.assertEqual(parsed['usage']['completion_tokens'], 1)
            else:
                self.assertEqual(parsed['tokens_predicted'], 1)
        return body

    def test_01_startup_shared_readiness_and_http_formats(self):
        s = self.server(controls={'startup-hold': '1'})
        wait(lambda: s.events('startup'), 'engine start after real reader load', 180)
        self.assertTrue(s.readers(), 'reader must be loaded before engine starts')
        self.assertFalse(s.events('ready'))
        for _ in range(10):
            try:
                self.assertNotEqual(s.http(timeout=.2)[0], 200, 'premature global READY')
            except (OSError, http.client.HTTPException):
                pass
        s.control('startup-release')
        s.start()
        for stream in (False, True):
            self.ok(s.http('/completion', text(stream=stream)), stream)
            self.ok(s.http('/v1/chat/completions', chat(stream=stream)), stream, True)
            self.ok(s.http('/v1/completions', {'model': 'pr4-http', 'prompt': 'Hello',
                                             'max_tokens': 1, 'stream': stream}), stream, True)
        self.assertEqual(len(s.gens()), 6)
        self.assertTrue(all(e['kind'] == 'GEN' and e['path'] == '' and not e['hashes'] for e in s.gens()))

    def test_02_startup_failure_rollback(self):
        s = self.server(controls={'startup-fail': '1'})
        wait(lambda: s.events('startup'), 'probe startup', 180)
        s.exited_failed()
        self.assertFalse(s.events('ready'))
        self.assertNotIn('listening on', s.log.read_text())

    def test_03_startup_silent_timeout_rollback(self):
        s = self.server(controls={'startup-silent': '1'}, profile={'engine_ready_ms': 1500})
        wait(lambda: s.events('startup'), 'silent probe startup', 180)
        s.exited_failed()
        self.assertFalse(s.events('ready'))

    def test_04_real_rows_order_cache_and_text_isolation(self):
        s = self.server().start()
        a, b = image(png(0)), image(png(1))
        for stream in (False, True):
            self.ok(s.http('/v1/chat/completions', chat([a, b, a], stream)), stream, True)
            e = s.gens()[-1]
            self.assertEqual(e['kind'], 'GENI')
            self.assertEqual(len(e['hashes']), 3)
            self.assertEqual(e['hashes'][0], e['hashes'][2])
            self.assertNotEqual(e['hashes'][0], e['hashes'][1])
            keys = e['keys'].split(',')
            self.assertEqual(keys[0], keys[2])
            self.assertNotEqual(keys[0], keys[1])
            self.assertTrue(all(0 < n <= 64 for n in e['cells']))
        first, second = s.gens()
        self.assertEqual(first['hashes'], second['hashes'], 'cache resend changed real rows')
        self.assertEqual(first['keys'], second['keys'])
        # We cannot infer reader encode counts; equality proves stable cache-compatible handoff, not a hit count.
        self.ok(s.http('/completion', text()))
        self.assertEqual(s.gens()[-1]['path'], '')
        self.assertEqual(s.gens()[-1]['kind'], 'GEN')
        self.ok(s.http('/v1/chat/completions', chat([image(bmp(), 'image/bmp')])), chat_response=True)

    def test_05_context_pre_sse_rejection_advances_without_nudge(self):
        for stream in (False, True):
            with self.subTest(stream=stream):
                s = self.server(controls={'hold-1': '1'}).start()
                a = s.submit()
                wait(lambda: len(s.gens()) == 1, 'A GEN')
                b = s.submit(body=text(504, stream))  # READY 512, reserved tail 8: no output room
                s.deferred(1)
                c = s.submit(body=text(4))
                s.deferred(2)
                s.control('release-1')
                self.ok(a.result(timeout=20))
                status, kind, body = b.result(timeout=20)
                self.assertEqual(status, 400, body)
                self.assertNotIn('text/event-stream', kind)
                self.assertIn('error', json.loads(body))
                self.ok(c.result(timeout=20))  # no /health poll or fourth request to advance C
                self.assertEqual(len(s.gens()), 2)

    def test_06_cancel_before_gen_queued_image(self):
        s = self.server(controls={'hold-1': '1'}).start()
        a = s.submit()
        wait(lambda: len(s.gens()) == 1, 'blocking A GEN')
        conn = s.raw(chat([image(png())], True), '/v1/chat/completions')
        s.deferred(1)  # B was really parsed/admitted, not merely written to a socket
        target = re.findall(r'defer task, id = (\d+)', s.log.read_text())[-1]
        follower = s.submit(body=text(4))
        s.deferred(2)
        s.disconnect(conn)
        cancelled = re.compile(r'cancel task, id_task = ' + target +
                               r'\n[\s\S]*new task, id = \d+/1, front = 1\n')
        wait(lambda: cancelled.search(s.log.read_text()), 'queued B cancellation posted and removed')
        s.control('release-1')
        self.ok(a.result(timeout=20))
        self.ok(follower.result(timeout=20))
        self.assertEqual([e['kind'] for e in s.gens()], ['GEN', 'GEN'])
        self.ok(s.http('/v1/chat/completions', chat([image(png())])), chat_response=True)

    def test_07_active_disconnect_stop_residual_done_order(self):
        s = self.server(controls={'hold-1': '1', 'withhold-done-1': '1'}).start()
        conn = s.raw(chat([image(png())], True), '/v1/chat/completions')
        wait(lambda: len(s.gens()) == 1, 'active vision GENI', 90)
        self.assertEqual(s.gens()[0]['kind'], 'GENI')
        s.disconnect(conn)
        wait(lambda: s.events('residual'), 'STOP and residual T/PP', 20)
        self.assertTrue(any(e.get('line') == 'STOP' for e in s.events('command')))
        follower = s.submit(body=text(4))
        s.deferred(1)
        # Observe a sustained withheld terminal, not just an instantaneous count.
        until = time.monotonic() + .5
        while time.monotonic() < until:
            self.assertEqual(len(s.gens()), 1)
            self.assertFalse(follower.done())
            self.assertFalse(s.events('ordering-violation'))
            time.sleep(.02)
        s.control('done-release-1')
        self.ok(follower.result(timeout=20))
        self.ok(s.http('/v1/chat/completions', chat([image(png(1))])), chat_response=True)
        self.assertEqual([e['kind'] for e in s.gens()], ['GENI', 'GEN', 'GENI'])
        self.assertFalse(s.events('probe-failure'))

    def test_08_http_and_queue_saturation_bounded(self):
        s = self.server(controls={'hold-1': '1'}).start()
        leading = s.submit()
        wait(lambda: len(s.gens()) == 1, 'leading GEN')
        pending = [s.submit(body=text(4 + i)) for i in range(12)]
        wait(lambda: any(f.done() and f.result()[0] == 503 for f in pending), 'bounded HTTP admission 503')
        self.assertEqual(len(s.gens()), 1)
        s.control('release-1')
        self.ok(leading.result(timeout=20))
        statuses = []
        for future in pending:
            result = future.result(timeout=25)
            statuses.append(result[0])
            if result[0] == 200:
                self.ok(result)
            else:
                self.assertEqual(result[0], 503, result[2])
        self.assertIn(503, statuses)
        self.assertEqual(len(s.gens()), 1 + statuses.count(200))
        self.ok(s.http('/completion', text()))

    def test_09_bad_sources_and_dimensions_pre_sse_400(self):
        s = self.server(profile={'attempts': 5}).start()
        bad_dimension = bytearray(bmp())
        struct.pack_into('<i', bad_dimension, 18, 4097)
        bad = [image(b'not an image'), image(bytes(bad_dimension), 'image/bmp'),
               {'type': 'image_url', 'image_url': {'url': 'file:///etc/passwd'}},
               {'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,@@@'}}]
        for picture in bad:
            if json.loads(s.http('/props')[2])['vision_status'] != 'available':
                wait(lambda: s.http('/vision/recover', {})[0] == 200, 'source refusal restart backoff', 15)
                wait(lambda: json.loads(s.http('/props')[2])['vision_status'] == 'available', 'source refusal reader restart', 180)
            with self.subTest(source=str(picture)[:80]):
                status, kind, body = s.http('/v1/chat/completions', chat([picture], True))
                self.assertEqual(status, 400, body)
                self.assertNotIn('text/event-stream', kind)
        self.assertEqual(s.gens(), [])
        self.ok(s.http('/completion', text()))

    def test_10_total_deadline_504_and_recovery(self):
        s = self.server(controls={'hold-1': '1', 'withhold-done-1': '1'},
                        profile={'total_ms': 1500}).start()
        a = s.submit()
        wait(lambda: len(s.gens()) == 1, 'blocking GEN')
        b = s.submit('/v1/chat/completions', chat([image(png())], True))
        s.deferred(1)
        wait(lambda: s.events('residual'), 'A total deadline issues STOP', 10)
        # Keep A's terminal withheld until B's admission-time total budget also
        # expires. B must then reject before GEN/SSE when the queue promotes it.
        time.sleep(1.6)
        s.control('done-release-1')
        result = b.result(timeout=20)
        self.assertEqual(result[0], 504, result[2])
        self.assertNotIn('text/event-stream', result[1])
        self.assertEqual(len(s.gens()), 1)
        self.assertEqual(a.result(timeout=20)[0], 504)
        self.ok(s.http('/completion', text()))

    def test_11_reader_death_text_then_explicit_vision_recovery(self):
        s = self.server().start()
        old = wait(s.readers, 'reader pid')
        self.assertEqual(len(old), 1)
        os.kill(old[0], signal.SIGKILL)
        wait(lambda: json.loads(s.http('/props')[2])['vision_status'] == 'unavailable', 'reader failure propagated')
        props = json.loads(s.http('/props')[2])
        self.assertTrue(props['vision_configured'])
        self.ok(s.http('/completion', text()))
        refused = s.http('/v1/chat/completions', chat([image(png())]))
        self.assertEqual(refused[0], 503, refused[2])
        wait(lambda: s.http('/vision/recover', {})[0] == 200, 'explicit recovery backoff', 15)
        wait(lambda: json.loads(s.http('/props')[2])['vision_status'] == 'available', 'explicit admitted restart', 180)
        self.assertTrue(set(s.readers()).isdisjoint(old))
        self.ok(s.http('/v1/chat/completions', chat([image(png())])), chat_response=True)
        self.ok(s.http('/completion', text()))

    def native_fatal(self, mode):
        s = self.server(controls={'mode-1': mode}).start()
        future = s.submit('/v1/chat/completions', chat([image(png())]))
        try:
            result = future.result(timeout=90)
            self.assertGreaterEqual(result[0], 500, result[2])
        except (OSError, http.client.HTTPException):
            pass  # supervised shutdown may close the listener before delivering an error
        wait(lambda: s.gens(), 'fatal native GEN')
        s.exited_failed()
        self.assertEqual(len(s.gens()), 1)

    def test_12_native_err_request_failure_and_reuse(self):
        s = self.server(controls={'mode-1': 'err'}).start()
        result = s.http('/v1/chat/completions', chat([image(png())]))
        self.assertEqual(result[0], 500, result[2])
        self.ok(s.http('/completion', text()))
        self.assertIsNone(s.proc.poll())
        self.assertEqual(len(s.gens()), 2)

    def test_13_native_malformed_failclosed(self):
        self.native_fatal('malformed')

    def test_14_native_death_failclosed(self):
        self.native_fatal('death')


    def test_15_resource_boundary_and_occurrence_refusal(self):
        s = self.server().start()
        def solid_png(w, h):
            def chunk(kind, data):
                return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
            compress = zlib.compressobj()
            row = b'\x00' + b'\x31\x42\x53' * w
            data = b''.join(compress.compress(row) for _ in range(h)) + compress.flush()
            return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', data) + chunk(b'IEND', b'')
        # The exact configured source-pixel/dimension ceiling, highly compressed
        # within PR3's unchanged source cap. Measured in the real isolated reader.
        self.ok(s.http('/v1/chat/completions', chat([image(solid_png(4096,4096))]), timeout=90), chat_response=True)
        before = len(s.gens())
        refused = s.http('/v1/chat/completions', chat([image(png())] * 5, True))
        self.assertEqual(refused[0], 400, refused[2])
        self.assertNotIn('text/event-stream', refused[1])
        refused = s.http('/v1/chat/completions', chat([image(solid_png(4097,1))], True))
        self.assertEqual(refused[0], 400, refused[2])
        self.assertEqual(len(s.gens()), before)
        self.ok(s.http('/completion', text()))

    def test_16_native_normal_stop_and_stop_string(self):
        for stop_string in (False,True):
            with self.subTest(stop_string=stop_string):
                s = self.server(controls={'mode-1':'normal-stop','hold-1':'1','withhold-done-1':'1'}).start()
                body = text(stream=True)
                if stop_string:
                    piece = json.loads(s.http('/detokenize', {'tokens':[1]})[2])['content']
                    self.assertTrue(piece)
                    body.update(n_predict=2, stop=[piece])
                request = s.submit(body=body)
                wait(lambda: s.events('residual'), 'parent normal STOP', 15)
                follower = s.submit(body=text(4))
                s.deferred(1)
                self.assertEqual(len(s.gens()), 1)
                self.assertFalse(request.done())
                self.assertFalse(follower.done())
                s.control('done-release-1')
                self.ok(request.result(timeout=20), True)
                self.ok(follower.result(timeout=20))
                self.assertEqual(len(s.gens()), 2)
                self.assertIsNone(s.proc.poll())

    def test_17_shutdown_active_and_queued_no_orphans(self):
        s = self.server(controls={'hold-1':'1','withhold-done-1':'1'}).start()
        active = s.submit('/v1/chat/completions', chat([image(png())]))
        wait(lambda: s.gens(), 'active shutdown vision GENI', 90)
        follower = s.submit()
        s.deferred(1)
        s.control('done-release-1')
        s.proc.terminate()
        wait(lambda: s.proc.poll() is not None, 'bounded graceful shutdown', 15)
        self.assertEqual(s.proc.returncode, 0)
        self.assertEqual(len(s.gens()), 1)
        s.no_orphans()
        self.assertFalse(Path(s.gens()[0]['path']).exists())
        for response in (active,follower):
            try:
                self.assertGreaterEqual(response.result(timeout=5)[0], 500)
            except (OSError,http.client.HTTPException):
                pass

class Report(unittest.TextTestResult):
    def startTest(self, test):
        self.started = time.monotonic()
        self.failure_count = len(self.failures) + len(self.errors)
        super().startTest(test)

    def stopTest(self, test):
        super().stopTest(test)
        REPORT.append({'test': str(test), 'seconds': round(time.monotonic() - self.started, 3),
                       'passed': len(self.failures) + len(self.errors) == self.failure_count})
        (ARGS.output / 'report.json').write_text(json.dumps(REPORT, indent=2) + '\n')


REPORT = []


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('server', 'engine', 'model', 'projector', 'profile', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--only', default='')
    ARGS = parser.parse_args()
    for name in ('server', 'engine', 'model', 'projector', 'profile', 'output'):
        setattr(ARGS, name, getattr(ARGS, name).resolve())
    for name in ('server', 'engine', 'model', 'projector', 'profile'):
        if not getattr(ARGS, name).is_file():
            parser.error('%s missing: %s' % (name, getattr(ARGS, name)))
    for companion in ('strata-process-guard', 'strata-vision-reader'):
        if not (ARGS.server.parent / companion).is_file():
            parser.error('missing companion: ' + companion)
    ARGS.output.mkdir(parents=True, exist_ok=True)
    # Do not accidentally overwrite previous diagnostics or mix stale gate files.
    if any(ARGS.output.iterdir()):
        parser.error('--output must be a new/empty directory')
    tests = unittest.defaultTestLoader.loadTestsFromTestCase(Qualification)
    suite = unittest.TestSuite(t for t in tests if ARGS.only in t.id())
    if not suite.countTestCases():
        parser.error('--only matched no tests')
    result = unittest.TextTestRunner(verbosity=2, resultclass=Report).run(suite)
    print('PR4 HTTP diagnostics: ' + str(ARGS.output), flush=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(main())
