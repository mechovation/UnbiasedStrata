#!/usr/bin/env python3
"""Explicit, offline-assets-only PR4 GPU maintenance qualification. No deployment.
Requires the existing CUDA build and qualification-only probe/phase binaries.
Never prints credentials, prompts, image bytes, response text or native commands.
"""
import argparse
import array
import base64
import concurrent.futures
import ctypes as c
import fcntl
import hashlib
import http.client
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import time
import zlib

GPU = 'GPU-d14daf49-45c8-58c5-cf61-5a40342672af'
PROD = 'unbiased-strata-5070ti'
NAME = 'pr4-closing-acceptance'
PORT = 18084
ROOT = Path(__file__).resolve().parents[2]


def require(value, message):
    if not value:
        raise RuntimeError(message)


def docker(*args, timeout=90):
    p = subprocess.run(['docker', *args], capture_output=True, text=True, timeout=timeout)
    require(p.returncode == 0, 'Docker operation failed: ' + args[0])
    return p.stdout.strip()


def inspect(name):
    return json.loads(docker('inspect', name))[0]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def png(rgb, w=256, h=256):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress((b'\0' + bytes(rgb) * w) * h)) + chunk(b'IEND', b'')


def image(rgb, w=256, h=256):
    return {'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,' + base64.b64encode(png(rgb, w, h)).decode()}}


def text(value):
    return {'type': 'text', 'text': value}


def body(content, stream=False, tokens=32):
    return {'model': 'qwen3.8-flash-next', 'messages': [{'role': 'user', 'content': content}], 'temperature': 0,
            'max_tokens': tokens, 'stream': stream, 'chat_template_kwargs': {'enable_thinking': False}}


def corpus():
    a, b, d = image((255, 0, 0), 224, 224), image((0, 0, 255), 448, 112), image((0, 255, 0), 112, 448)
    cases = [('text-only', body('Describe nothing.')), ('image-only', body([a])),
             ('leading-image', body([a, text('Describe.')])), ('trailing-image', body([text('Describe this:'), a])),
             ('adjacent', body([a, b, d])), ('duplicates', body([a, a, a])),
             ('interleaved', body([text('one\n'), a, text('\ntwo '), b, text('three'), a, text(' end')])),
             ('odd-aspect', body([image((255, 255, 0), 333, 197), d])),
             ('large-resize', body([image((0, 255, 255), 2048, 1024)])),
             ('minimum-resize', body([image((255, 0, 0), 32, 32)])),
             ('literals', body([text('<|image_pad|> <|vision_start|> <think> <tool_call>'), a,
                                text('literal <__media__> <|im_end|>'), b, text('<|vision_end|>')])),
             ('utf8-boundaries', body([text('é🙂漢字\n'), a, text('\n\n العربية é'), b, text('!?')]))]
    history = body([text('Compare.'), b])
    history['messages'] = [{'role': 'user', 'content': [text('First:'), a]},
                           {'role': 'assistant', 'content': '<think>Consider carefully.</think>Earlier view.'}, history['messages'][0]]
    cases.append(('assistant-history', history))
    tools = body([text('Inspect.'), a])
    tools['tools'] = [{'type': 'function', 'function': {'name': 'inspect', 'description': 'literal <|image_pad|>',
                                                     'parameters': {'type': 'object', 'properties': {}}}}]
    cases.append(('tools-definition', tools))
    tool_history = json.loads(json.dumps(tools))
    tool_history['messages'] += [{'role': 'assistant', 'content': '', 'tool_calls': [{'id': 'call_1', 'type': 'function',
                                 'function': {'name': 'inspect', 'arguments': '{}'}}]},
                                {'role': 'tool', 'tool_call_id': 'call_1', 'content': [text('Tool image.'), b]},
                                {'role': 'user', 'content': 'Describe both. <|im_start|> is literal.'}]
    cases += [('tool-image-history', tool_history), ('long-composition', body([text('x' * 8192), a, text('y' * 8192)]))]
    for name, rgb in [('cache-a', (255, 0, 0)), ('cache-a-hit', (255, 0, 0)), ('cache-b', (0, 0, 255)),
                      ('cache-c', (0, 255, 0)), ('cache-a-evicted', (255, 0, 0)), ('cache-a-restart', (255, 0, 0))]:
        cases.append((name, body([text('Describe.'), image(rgb)])))
    return cases


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--authorize-maintenance', action='store_true', required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--qualification', type=Path, required=True)
    ap.add_argument('--profile', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    build, qualification, out = args.build.resolve(), args.qualification.resolve(), args.output.resolve()
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    lock = open('/tmp/unbiased-strata-vision-trial.lock', 'w')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    key = (ROOT / 'secrets/strata-api-key').read_text().strip()

    def record(case, **data):
        line = json.dumps(dict(case=case, **data))
        require(key not in line, 'refusing sensitive evidence')
        with (out / 'results.jsonl').open('a') as f:
            f.write(line + '\n')
        print(line, flush=True)

    nv = c.CDLL('libnvidia-ml.so.1')
    require(nv.nvmlInit_v2() == 0, 'NVML initialization denied')
    device, events = c.c_void_p(), c.c_void_p()
    require(nv.nvmlDeviceGetHandleByUUID(GPU.encode(), c.byref(device)) == 0, 'approved GPU unavailable')
    require(nv.nvmlEventSetCreate(c.byref(events)) == 0 and nv.nvmlDeviceRegisterEvents(device, c.c_ulonglong(8), events) == 0,
            'driver event observation unavailable')

    class Process(c.Structure):
        _fields_ = [('pid', c.c_uint), ('bytes', c.c_ulonglong), ('gi', c.c_uint), ('ci', c.c_uint)]

    driver_fault = False

    def registry():
        nonlocal driver_fault
        if nv.nvmlEventSetWait_v2(events, c.create_string_buffer(64), 0) != 10:
            driver_fault = True
        processes, count = (Process * 16)(), c.c_uint(16)
        require(nv.nvmlDeviceGetComputeRunningProcesses_v2(device, c.byref(count), processes) == 0, 'GPU registry observation denied')
        require(not driver_fault, 'driver fault: operator recovery required')
        return [{'pid': p.pid, 'bytes': p.bytes} for p in processes[:count.value]]

    def empty():
        for _ in range(100):
            if not registry():
                return True
            time.sleep(.1)
        return False

    def request(path, data=None, timeout=180):
        conn = http.client.HTTPConnection('127.0.0.1', PORT, timeout=timeout)
        conn.request('POST' if data is not None else 'GET', path, None if data is None else json.dumps(data),
                     {'Authorization': 'Bearer ' + key, 'Content-Type': 'application/json'})
        response = conn.getresponse()
        raw = response.read(8 * 1024 * 1024)
        require(len(raw) < 8 * 1024 * 1024, 'response exceeded qualification bound')
        status = response.status
        conn.close()
        return status, raw

    def chat(data):
        status, raw = request('/v1/chat/completions', data)
        require(status == 200, 'chat refused: ' + str(status))
        if data.get('stream'):
            require(b'data: [DONE]' in raw, 'incomplete SSE')
            chunks = [json.loads(line[6:]) for line in raw.splitlines() if line.startswith(b'data: {')]
            require(not any('error' in x for x in chunks), 'SSE returned error')
            answer = ''.join(x['choices'][0]['delta'].get('content') or '' for x in chunks if x.get('choices'))
        else:
            answer = json.loads(raw)['choices'][0]['message'].get('content') or ''
        require(bool(answer.strip()), 'empty assistant response')
        return answer

    prod = inspect(PROD)
    require(prod['State']['Running'] and prod['State']['Health']['Status'] == 'healthy', 'production not initially healthy')
    require(prod['HostConfig']['DeviceRequests'][0]['DeviceIDs'] == [GPU], 'production device mismatch')
    original_settings = hashlib.sha256(json.dumps([prod['Config'], prod['HostConfig'], prod['Mounts']], sort_keys=True).encode()).hexdigest()
    compose = {str(p): digest(p) for p in (ROOT / 'docker-compose.yaml', ROOT / 'docker-compose.yaml.bak')}
    profile = json.loads(args.profile.read_text())
    # Explicit pinned qwen3vl default, not the exploratory CPU fixture's minimum.
    profile['image_min_tokens'], profile['batch_max_tokens'] = 8, 1024
    (out / 'gpu-profile.json').write_text(json.dumps(profile, indent=2) + '\n')
    manifest = {'production_id': prod['Id'], 'image': prod['Image'], 'settings_sha': original_settings, 'compose': compose,
                'binary_sha': {p.name: digest(p) for p in (build / 'bin').iterdir() if p.name in ('unbiased-strata', 'strata-vision-reader', 'strata-process-guard')},
                'profile_sha': digest(out / 'gpu-profile.json')}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    active = False
    paused = False
    current = ''

    def resources():
        class Memory(c.Structure):
            _fields_ = [('total', c.c_ulonglong), ('free', c.c_ulonglong), ('used', c.c_ulonglong)]
        memory = Memory()
        require(nv.nvmlDeviceGetMemoryInfo(device, c.byref(memory)) == 0, 'GPU memory observation denied')
        pid = inspect(NAME)['State']['Pid']
        if not pid:
            return
        cg = Path('/sys/fs/cgroup') / next(line.split('::', 1)[1] for line in Path(f'/proc/{pid}/cgroup').read_text().splitlines() if line.startswith('0::')).lstrip('/')
        record(current + '-resources', gpu_used_mib=memory.used // 1048576, gpu_total_mib=memory.total // 1048576,
               memory_peak=int((cg / 'memory.peak').read_text()), memory_events=(cg / 'memory.events').read_text().strip(),
               pids_peak=int((cg / 'pids.peak').read_text()))

    def stop():
        nonlocal active
        if not active:
            return
        resources()
        docker('stop', '-t', '45', NAME, timeout=70)
        state = inspect(NAME)['State']
        require(not state['Running'] and not state['OOMKilled'], 'candidate containment/OOM failure')
        # The unchanged legacy path reports its intentional engine QUIT as 1.
        # Native PR4 shutdown must report 0; do not broaden that acceptance.
        exit_ok = state['ExitCode'] == 0 or (current.startswith('legacy-') and state['ExitCode'] == 1)
        require(empty(), 'candidate GPU quiescence not certified')
        docker('rm', NAME)
        active = False
        record(current + '-stopped', exit=state['ExitCode'], gpu_empty=True)
        require(exit_ok, 'candidate shutdown failed: ' + str(state['ExitCode']))

    def start(stage, native, probe=False, phase=False):
        nonlocal active, current
        stop()
        current = stage
        directory = out / stage
        directory.mkdir(mode=0o700)
        audit = out / (stage + '-audit')
        audit.mkdir(mode=0o700)
        executable_dir = qualification / 'phase-bin' if phase else build / 'bin'
        original = list(prod['Config']['Cmd'])
        boundary = original.index('--')
        front, engine = original[:boundary], original[boundary + 1:]
        front = ['--threads', '4', '--threads-http', '8', '--picture-cache', '2', '--log-disable', *front]
        if native:
            front = ['--native-vision', '/profile.json', *front]
        if probe:
            front[front.index('--engine') + 1] = '/qualification/probe'
            engine = ['--fixture-dir', '/evidence/' + stage]
        command = ['run', '-d', '--name', NAME, '--restart=no', '--read-only', '--user', '1000:1000',
                   '--memory=96g', '--memory-swap=96g', '--pids-limit=512', '--cpuset-cpus=0-47',
                   '--tmpfs', '/tmp:rw,nosuid,nodev,size=64m', '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges',
                   '--gpus', '"device=' + GPU + '"', '-e', 'NVIDIA_VISIBLE_DEVICES=' + GPU,
                   '-e', 'CUDA_VISIBLE_DEVICES=' + GPU, '-e', 'CUDA_DEVICE_ORDER=PCI_BUS_ID',
                   '-e', 'NVIDIA_DRIVER_CAPABILITIES=compute,utility', '-p', f'127.0.0.1:{PORT}:8080',
                   '-v', str(executable_dir) + ':/candidate:ro', '-v', str(qualification) + ':/qualification:ro',
                   '-v', str(out) + ':/evidence:rw', '-v', str(audit) + ':/audit:rw',
                   '-v', str(out / 'gpu-profile.json') + ':/profile.json:ro', '--entrypoint', '/candidate/unbiased-strata']
        for mount in prod['Mounts']:
            command += ['-v', mount['Source'] + ':' + mount['Destination'] + ':ro']
        for value in prod['Config']['Env']:
            if value.split('=', 1)[0] in {'STRATA_PF_FUSED', 'STRATA_PLE_PROBE', 'STRATA_PREFILL_CPU_SHARE'}:
                command += ['-e', value]
        docker(*command, prod['Image'], *front, '--', *engine)
        active = True
        began = time.monotonic()
        for _ in range(600):
            require(inspect(NAME)['State']['Running'], 'candidate exited before readiness')
            try:
                if request('/health', timeout=2)[0] == 200:
                    break
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(1)
        else:
            raise RuntimeError('candidate readiness deadline')
        record(stage + '-ready', seconds=round(time.monotonic() - began, 2), contexts=registry())
        return audit / 'phases'

    def phases(path):
        return [line.split()[2] for line in path.read_text().splitlines()] if path.exists() else []

    def recover_reader():
        top = docker('top', NAME, '-eo', 'pid,comm').splitlines()[1:]
        readers = [int(line.split()[0]) for line in top if 'strata-vision-r' in line]
        require(len(readers) == 1, 'owned reader identity ambiguous')
        os.kill(readers[0], signal.SIGKILL)
        for _ in range(100):
            if all(p['pid'] != readers[0] for p in registry()):
                break
            time.sleep(.1)
        else:
            raise RuntimeError('reader context remained registered')
        time.sleep(2)
        before = registry()
        require(request('/vision/recover', {})[0] == 200, 'explicit recovery refused')
        for _ in range(100):
            status, raw = request('/props')
            if status == 200 and json.loads(raw)['vision_status'] == 'available':
                break
            time.sleep(.1)
        else:
            raise RuntimeError('reader recovery readiness deadline')
        record(current + '-reader-recovered', before=before, after=registry())

    def validate_phases(path, terminal_containment=False):
        engine = False
        reader = None
        counts = {}
        for name in phases(path):
            counts[name] = counts.get(name, 0) + 1
            if name == 'engine_gen':
                require(not engine and reader is None, 'overlapping GEN or reader GPU phase')
                engine = True
            elif name == 'engine_done':
                require(engine and reader is None, 'DONE without exclusive engine turn')
                engine = False
            elif name.startswith('reader_') and name.endswith('_begin'):
                require(not engine and reader is None, 'reader allocation/preprocessing/encode overlaps engine work')
                reader = name[:-6]
            elif name.startswith('reader_') and name.endswith('_end'):
                require(not engine and reader == name[:-4], 'reader phase ordering mismatch')
                reader = None
        require(reader is None and (not engine or terminal_containment), 'unterminated phase')
        for name in ('reader_load_begin', 'reader_prepare_begin', 'reader_encode_begin', 'engine_gen', 'engine_done'):
            require(counts.get(name, 0) > 0, 'missing positive phase control: ' + name)
        record(current + '-exclusion', no_overlap=True, terminal_containment=engine, counts=counts)

    try:
        paused = True
        docker('stop', '-t', '60', prod['Id'])
        require(empty(), 'approved GPU occupied after production stop')
        cases = corpus()
        for native in (False, True):
            stage = 'native-probe' if native else 'legacy-probe'
            audit = start(stage, native, probe=True, phase=native)
            for name, data in cases:
                if name == 'cache-a-restart' and native:
                    recover_reader()
                before = phases(audit).count('reader_encode_begin')
                chat(data)
                encoded = phases(audit).count('reader_encode_begin') - before
                if native and name.startswith('cache-'):
                    require(encoded == (0 if name in ('cache-a-hit', 'cache-a-restart') else 1), 'late cache encode-count mismatch')
                record(stage + '-case', name=name, passed=True, encodes=encoded if native else None)
            stop()
            if native:
                validate_phases(audit)
        baseline = [json.loads(line) for line in (out / 'legacy-probe/results').read_text().splitlines()]
        candidate = [json.loads(line) for line in (out / 'native-probe/results').read_text().splitlines()]
        require(len(baseline) == len(candidate) == len(cases), 'probe request count mismatch')
        for (name, _), a, b in zip(cases, baseline, candidate):
            require(all(a[k] == b[k] for k in ('tokens', 'token_sha', 'keys_sha', 'shapes', 'image_cells')), 'token/key/grid parity mismatch: ' + name)
            exact = a['row_sha'] == b['row_sha']
            max_abs = 0.0
            if not exact:
                left = (out / ('legacy-probe/' + str(a['request']) + '.rows')).read_bytes()
                right = (out / ('native-probe/' + str(b['request']) + '.rows')).read_bytes()
                require(len(left) == len(right), 'row byte-count mismatch')
                at = 0
                while at < len(left):
                    require(left[at:at + 20] == right[at:at + 20], 'SVE1 header mismatch')
                    header = struct.unpack_from('<5I', left, at)
                    at += 20
                    size = header[1] * header[4] * 4
                    x, y = array.array('f'), array.array('f')
                    x.frombytes(left[at:at + size]); y.frombytes(right[at:at + size])
                    for ref, got in zip(x, y):
                        delta = abs(ref - got)
                        max_abs = max(max_abs, delta)
                        require(delta <= 1e-5 + 1e-5 * abs(ref), 'GPU row numeric parity mismatch: ' + name)
                    at += size
            record('gpu-parity', name=name, exact_tokens=True, exact_grids_keys=True, exact_rows=exact, max_abs=max_abs,
                   image_cells=b['image_cells'])
        for stage in ('legacy-probe', 'native-probe'):
            for file in (out / stage).glob('*.rows'):
                file.unlink()  # retain only hashes/counts, not synthetic fixture embeddings

        for native in (False, True):
            stage = 'native-real' if native else 'legacy-real'
            audit = start(stage, native, phase=native)
            for color, rgb in [('red', (255, 0, 0)), ('blue', (0, 0, 255)), ('green', (0, 255, 0))]:
                for stream in (False, True):
                    answer = chat(body([image(rgb), text('What is the dominant color? Answer with one color word.')], stream))
                    require(color in answer.lower(), 'real-engine color regression: ' + color)
                    record(stage + '-quality', color=color, stream=stream, correct=True)
            for name, content in [('image-to-text', 'What is two plus two? Answer with a single digit.'),
                                  ('literal-text', 'Repeat exactly: <|image_pad|> <|vision_start|> <|vision_end|>')]:
                answer = chat(body(content))
                require('4' in answer if name == 'image-to-text' else '<|image_pad|>' in answer, 'text/literal quality regression')
                record(stage + '-quality', name=name, correct=True)
            for expected, pictures in [('red', [image((255, 0, 0)), image((0, 0, 255))]),
                                       ('blue', [image((0, 0, 255)), image((255, 0, 0))])]:
                answer = chat(body([*pictures, text('What is the dominant color in the FIRST image? Answer with one color word.')]))
                require(expected in answer.lower(), 'ordered-image quality regression')
                record(stage + '-ordered-quality', expected=expected, correct=True)
            for name, content in [('malformed-image', [{'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,AAAA'}}]),
                                  ('too-many-images', [image((255, 0, 0))] * 5)]:
                if native and name == 'malformed-image':
                    continue  # run after high-water recovery; preserve the two-attempt budget
                status, _ = request('/v1/chat/completions', body(content))
                # The occurrence cap belongs to the explicit native profile;
                # the legacy baseline has no four-occurrence policy.
                expected = 400 if native or name == 'malformed-image' else 200
                require(status == expected, 'image budget/rejection mapping changed')
                chat(body('Say OK.'))
                record(stage + '-rejection-promotion', name=name, status=status, follower=True)
            if native:
                # Actual native LLM interval; a queued vision source may not cause
                # reader preprocessing/encode until STOP + terminal drain.
                conn = http.client.HTTPConnection('127.0.0.1', PORT, timeout=180)
                conn.request('POST', '/v1/chat/completions', json.dumps(body('Write a long numbered list of integers.', True, 4096)),
                             {'Authorization': 'Bearer ' + key, 'Content-Type': 'application/json'})
                response = conn.getresponse()
                require(response.status == 200 and response.readline().startswith(b'data:'), 'active stream not established')
                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                    follower = pool.submit(chat, body([image((255, 255, 0), 333, 197), text('Describe briefly.')]))
                    require(request('/vision/recover', {})[0] == 503, 'recovery overlapped active engine')
                    time.sleep(.2)
                    response.close(); conn.close()
                    follower.result(timeout=180)
                record('real-cancel-queued-vision', passed=True)
                token = json.loads(request('/tokenize', {'content': ' hello'})[1])['tokens'][0]
                status, raw = request('/completion', {'prompt': [token] * 262135, 'n_predict': 1, 'temperature': 0}, timeout=600)
                require(status == 200 and json.loads(raw)['tokens_evaluated'] == 262135, 'final-profile full-context failure')
                record('final-profile-full-context', evaluated_tokens=262135, contexts=registry())
                resources()
                recover_reader()  # no intervening generation may shrink the KV allocation
                chat(body([image((0, 0, 255)), text('Name the color briefly.')]))
                # A malformed PREPARE contains the reader; text must still promote.
                # Do this last: initial load plus the high-water recovery consumes
                # the reviewed two-attempt episode budget. Never raise it for a test.
                status, _ = request('/v1/chat/completions', body([{'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,AAAA'}}]))
                require(status == 400, 'malformed image rejection changed')
                chat(body('Say OK.'))
                record(stage + '-rejection-promotion', name='malformed-image', status=status, follower=True)
                # Shut down with real active and queued response owners, not just idle.
                conn = http.client.HTTPConnection('127.0.0.1', PORT, timeout=180)
                conn.request('POST', '/v1/chat/completions', json.dumps(body('Write a long numbered list.', True, 4096)),
                             {'Authorization': 'Bearer ' + key, 'Content-Type': 'application/json'})
                response = conn.getresponse()
                require(response.status == 200, 'shutdown stream not established')
                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                    follower = pool.submit(request, '/v1/chat/completions', body('Say OK.'))
                    time.sleep(.2)
                    docker('kill', '--signal', 'TERM', NAME)
                    try:
                        status, _ = follower.result(timeout=60)
                        require(status in (200, 500, 503), 'queued shutdown terminal mapping changed')
                    except (OSError, http.client.HTTPException):
                        pass  # HTTP owner shutdown is permitted to close the socket
                    response.close(); conn.close()
                record('real-active-queued-shutdown', settled=True)
                stop()
                validate_phases(audit, terminal_containment=True)
            else:
                stop()
        record('PR4_GPU_CLOSING_ACCEPTANCE_PASS', cases=len(cases), rows_tokens_quality_exclusion=True)
    finally:
        try:
            stop()
            require(empty(), 'GPU not safely empty: production must remain stopped')
            if paused:
                current_prod = inspect(prod['Id'])
                settings = hashlib.sha256(json.dumps([current_prod['Config'], current_prod['HostConfig'], current_prod['Mounts']], sort_keys=True).encode()).hexdigest()
                require(settings == original_settings and current_prod['Image'] == prod['Image'], 'production settings changed')
                docker('start', prod['Id'])
                for _ in range(600):
                    if inspect(prod['Id'])['State']['Health']['Status'] == 'healthy':
                        break
                    time.sleep(1)
                else:
                    raise RuntimeError('production restoration health deadline')
                require(all(digest(p) == h for p, h in compose.items()), 'Compose/backup changed')
                record('production-restored', healthy=True, original_settings=True, compose_backup_unchanged=True)
        finally:
            nv.nvmlEventSetFree(events)
            nv.nvmlShutdown()


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise InterruptedError('qualification interrupted')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    main()
