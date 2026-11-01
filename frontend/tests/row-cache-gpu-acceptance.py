#!/usr/bin/env python3
"""PR5-only, explicitly authorized 5070 Ti maintenance qualification; no deployment.
Uses existing local assets/images, never downloads, and never logs credentials,
prompts, URLs, image bytes or assistant text. The 3060 is never selected.
"""
import argparse
import concurrent.futures
import ctypes as c
import fcntl
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
GPU = 'GPU-d14daf49-45c8-58c5-cf61-5a40342672af'
PROD = 'unbiased-strata-5070ti'
NAME = 'pr5-gpu-acceptance'
PORT = 18085
spec = importlib.util.spec_from_file_location('vision_gpu_fixtures', Path(__file__).with_name('vision_gpu_fixtures.py'))
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
body, image, text = fixtures.body, fixtures.image, fixtures.text


class QualificationFailure(RuntimeError):
    """Only fixed, public-safe invariant messages may be logged."""


def require(ok, message):
    if not ok:
        raise QualificationFailure(message)


def docker(*args, timeout=90):
    p = subprocess.run(['docker', *map(str, args)], capture_output=True, text=True, timeout=timeout)
    require(p.returncode == 0, 'Docker operation failed: ' + str(args[0]))
    return p.stdout.strip()


def inspect(name):
    return json.loads(docker('inspect', name))[0]


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


class Driver:
    class Process(c.Structure):
        _fields_ = [('pid', c.c_uint), ('bytes', c.c_ulonglong), ('gi', c.c_uint), ('ci', c.c_uint)]

    def __init__(self):
        self.nv = c.CDLL('libnvidia-ml.so.1')
        self.device, self.events = c.c_void_p(), c.c_void_p()
        self.fault = False
        require(self.nv.nvmlInit_v2() == 0, 'NVML init denied')
        require(self.nv.nvmlDeviceGetHandleByUUID(GPU.encode(), c.byref(self.device)) == 0, 'approved GPU unavailable')
        require(self.nv.nvmlEventSetCreate(c.byref(self.events)) == 0 and
                self.nv.nvmlDeviceRegisterEvents(self.device, c.c_ulonglong(8), self.events) == 0, 'XID observation denied')

    def registry(self):
        if self.nv.nvmlEventSetWait_v2(self.events, c.create_string_buffer(64), 0) != 10:
            self.fault = True
        require(not self.fault, 'driver fault requires operator recovery')
        processes, count = (self.Process * 16)(), c.c_uint(16)
        require(self.nv.nvmlDeviceGetComputeRunningProcesses_v2(self.device, c.byref(count), processes) == 0,
                'GPU process observation denied')
        return {p.pid: p.bytes for p in processes[:count.value]}

    def empty(self):
        for _ in range(150):
            if not self.registry():
                return True
            time.sleep(.1)
        return False

    def memory(self):
        class Memory(c.Structure):
            _fields_ = [('total', c.c_ulonglong), ('free', c.c_ulonglong), ('used', c.c_ulonglong)]
        m = Memory()
        require(self.nv.nvmlDeviceGetMemoryInfo(self.device, c.byref(m)) == 0, 'GPU memory observation denied')
        return {name: getattr(m, name) for name in ('total', 'free', 'used')}

    def close(self):
        self.nv.nvmlEventSetFree(self.events)
        self.nv.nvmlShutdown()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--authorize-maintenance', action='store_true', required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--qualification', type=Path, required=True)
    ap.add_argument('--profile', type=Path, required=True)
    ap.add_argument('--poly-profile', type=Path, required=True,
                    help='explicit engine-only frozen poly profile; all other settings must match --profile')
    ap.add_argument('--poly-model', type=Path, required=True)
    ap.add_argument('--poly-projector', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    build, tools, out = args.build.resolve(), args.qualification.resolve(), args.output.resolve()
    poly_model, poly_projector = args.poly_model.resolve(), args.poly_projector.resolve()
    require(poly_model.is_file() and poly_projector.is_file(), 'existing poly assets required')
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    lock = open('/tmp/unbiased-strata-vision-trial.lock', 'w')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    key = (ROOT / 'secrets/strata-api-key').read_text().strip()
    require(bool(key), 'empty credential file')

    def record(case, **data):
        line = json.dumps(dict(case=case, **data))
        require(key not in line, 'sensitive evidence refused')
        with (out / 'results.jsonl').open('a') as f:
            f.write(line + '\n')
        print(line, flush=True)

    prod = inspect(PROD)
    require(prod['State']['Running'] and prod['State']['Health']['Status'] == 'healthy', 'production not initially healthy')
    require(prod['HostConfig']['DeviceRequests'][0]['DeviceIDs'] == [GPU], 'production GPU mismatch')
    require(key not in json.dumps(prod['Config']['Cmd']), 'literal credential argv refused')
    settings = lambda p: hashlib.sha256(json.dumps([p['Config'], p['HostConfig'], p['Mounts']], sort_keys=True).encode()).hexdigest()
    original = settings(prod)
    require(not docker('ps','-aq','--filter','name=^'+NAME+'$'), 'candidate name already owned')
    compose = {str(p): digest(p) for p in (ROOT/'docker-compose.yaml', ROOT/'docker-compose.yaml.bak')}
    profile = json.loads(args.profile.read_text())
    require(profile['gpu_uuid'] == GPU and profile['attempts'] == 2, 'reviewed GPU profile required')
    require('STRATA_FREEZE_EXPERTS' not in profile['engine_environment'] and
            'STRATA_FREEZE_EXPERTS' not in profile['reader_environment'], 'Flash profile must remain unfrozen')
    poly_profile = json.loads(args.poly_profile.read_text())
    expected_poly = json.loads(json.dumps(profile))
    expected_poly['engine_environment']['STRATA_FREEZE_EXPERTS'] = '1'
    require(poly_profile == expected_poly, 'poly profile may change only the exact engine freeze opt-in')
    (out/'profile.json').write_text(json.dumps(profile, indent=2)+'\n')
    (out/'poly-profile.json').write_text(json.dumps(poly_profile, indent=2)+'\n')
    binaries = [build/'bin'/s for s in ('unbiased-strata','strata-vision-reader','strata-process-guard','strata-poly')]
    sources = ['frontend/vision_cache.hpp','frontend/vision_recipe_budget.hpp','frontend/vision_job.hpp',
               'frontend/vision_job_io.hpp','frontend/vision_control.hpp','frontend/vision_queue.hpp',
               'frontend/vision_supervisor.hpp','frontend/vision_composition.hpp','frontend/vision_reader.cpp','frontend/vision_runtime.hpp',
               'frontend/tests/row-cache-gpu-acceptance.py','frontend/tests/row-cache-gpu-build.sh',
               'frontend/tests/row-cache-gpu-fixture.cpp','frontend/tests/row-cache-gpu-phase.cpp',
               'frontend/tests/vision_gpu_fixtures.py','frontend/tests/vision-gpu-probe.cpp','frontend/tests/vision-gpu-phases.inc']
    (out/'manifest.json').write_text(json.dumps(dict(production_id=prod['Id'], production_image=prod['Image'],
        source_sha={name:digest(ROOT/name) for name in sources},
        production_settings_sha=original, compose=compose, binaries={p.name:digest(p) for p in binaries},
        profile_sha=digest(out/'profile.json'), poly_profile_sha=digest(out/'poly-profile.json'),
        poly_policy={'freeze_experts':'1', 'kv_whole':True, 'reserve_mib':1200},
        poly_model_sha=digest(poly_model), poly_projector_sha=digest(poly_projector)), indent=2)+'\n')
    driver = Driver()
    active, paused, current = False, False, ''

    def request(path, data=None, timeout=180, port=PORT, token=key):
        conn = http.client.HTTPConnection('127.0.0.1', port, timeout=timeout)
        headers = {'Content-Type':'application/json'}
        if token is not None:
            headers['Authorization'] = 'Bearer ' + token
        conn.request('POST' if data is not None else 'GET', path, None if data is None else json.dumps(data), headers)
        r = conn.getresponse(); raw = r.read(8*1024*1024)
        require(len(raw) < 8*1024*1024, 'response exceeded qualification bound')
        status = r.status; conn.close()
        return status, raw

    def chat(data, port=PORT):
        status, raw = request('/v1/chat/completions', data, port=port)
        require(status == 200, 'chat refused')
        if data.get('stream'):
            require(b'data: [DONE]' in raw, 'incomplete SSE')
            chunks = [json.loads(line[6:]) for line in raw.splitlines() if line.startswith(b'data: {')]
            require(not any('error' in x for x in chunks), 'SSE error')
            answer = ''.join(x['choices'][0]['delta'].get('content') or '' for x in chunks if x.get('choices'))
            usage = next((x['usage'] for x in reversed(chunks) if x.get('usage')), {})
        else:
            result = json.loads(raw)
            answer, usage = result['choices'][0]['message'].get('content') or '', result.get('usage', {})
        require(bool(answer.strip()), 'empty assistant response')
        return answer, usage

    def phases(stage=None):
        path = out/(stage or current)/'audit/phases'
        return [s.split()[2] for s in path.read_text().splitlines()] if path.exists() else []

    def counts():
        names = phases()
        return {s:names.count(s) for s in ('reader_decode_begin','reader_prepare_begin','reader_encode_begin','engine_gen','engine_done')}

    def no_reader_work(before):
        after = counts()
        require(all(after[k] == before[k] for k in ('reader_decode_begin','reader_prepare_begin','reader_encode_begin')),
                'complete hit performed reader work')
        return after

    def resources():
        state = inspect(NAME)['State']
        pid = state['Pid']
        if not pid:
            return
        path = next(s.split('::',1)[1] for s in Path(f'/proc/{pid}/cgroup').read_text().splitlines() if s.startswith('0::'))
        cg = Path('/sys/fs/cgroup')/path.lstrip('/')
        events = dict(s.split() for s in (cg/'memory.events').read_text().splitlines())
        require(all(int(events.get(k,0)) == 0 for k in ('max','oom','oom_kill')), 'memory pressure/containment failure')
        record(current+'-resources', gpu=driver.memory(), contexts=driver.registry(), memory_peak=int((cg/'memory.peak').read_text()),
               pids_peak=int((cg/'pids.peak').read_text()), memory_events=events)

    def stop(check_exit=True):
        nonlocal active
        if not active:
            return
        if not docker('ps','-aq','--filter','name=^'+NAME+'$'):
            active = False
            return
        resource_failure = None
        if inspect(NAME)['State']['Running']:
            try:
                resources()
            except BaseException as exc:
                resource_failure = exc
            # Observation/resource failure never skips containment of our own
            # candidate. Independent healthy/empty proof still gates restoration.
            docker('stop','-t','45',NAME,timeout=70)
        state = inspect(NAME)['State']
        require(not state['Running'], 'candidate still running')
        require(driver.empty(), 'candidate GPU quiescence uncertain')
        docker('rm',NAME); active = False
        record(current+'-stopped', exit=state['ExitCode'], oom=state['OOMKilled'], gpu_empty=True)
        expected = state['ExitCode'] == 0 or ('legacy' in current and state['ExitCode'] == 1)
        if check_exit:
            require(expected and not state['OOMKilled'] and resource_failure is None, 'candidate exit/containment failed')

    def base(stage, selected_profile='profile.json'):
        nonlocal current
        stop(); current = stage
        directory = out/stage; directory.mkdir(mode=0o700)
        (directory/'audit').mkdir(mode=0o700); (directory/'probe').mkdir(mode=0o700)
        command = ['run','-d','--name',NAME,'--label','unbiasedstrata.test=pr5','--pull','never','--restart=no',
            '--read-only','--user','1000:1000','--memory=96g','--memory-swap=96g','--pids-limit=512','--cpuset-cpus=0-47',
            '--tmpfs','/tmp:rw,nosuid,nodev,size=64m','--cap-drop','ALL','--security-opt','no-new-privileges',
            '--gpus','"device='+GPU+'"','-e','NVIDIA_VISIBLE_DEVICES='+GPU,'-e','CUDA_VISIBLE_DEVICES='+GPU,
            '-e','CUDA_DEVICE_ORDER=PCI_BUS_ID','-e','NVIDIA_DRIVER_CAPABILITIES=compute,utility',
            '-v',str(tools)+':/qualification:ro','-v',str(build/'bin')+':/engines:ro',
            '-v',str(tools/'phase-bin')+':/candidate:ro','-v',str(out)+':/evidence:rw',
            '-v',str(directory/'audit')+':/audit:rw','-v',str(directory/'probe')+':/probe:rw',
            '-v',str(out/selected_profile)+':/profile.json:ro',
            '-v',str(poly_model)+':/poly/model.gguf:ro','-v',str(poly_projector)+':/poly/projector.gguf:ro']
        for m in prod['Mounts']:
            command += ['-v',m['Source']+':'+m['Destination']+':ro']
        for value in prod['Config']['Env']:
            if value.split('=',1)[0] in {'STRATA_PF_FUSED','STRATA_PLE_PROBE','STRATA_PREFILL_CPU_SHARE'}:
                command += ['-e',value]
        return command

    def start(stage, family='strata', native=True, probe=False, image_tokens=1024):
        nonlocal active
        command = base(stage, 'poly-profile.json' if family == 'poly' else 'profile.json')
        original_cmd = list(prod['Config']['Cmd']); boundary = original_cmd.index('--')
        front, engine = original_cmd[:boundary], original_cmd[boundary+1:]
        if family == 'poly':
            front[front.index('--engine')+1] = '/engines/strata-poly'
            front[front.index('-m')+1] = '/poly/model.gguf'
            front[front.index('--mmproj')+1] = '/poly/projector.gguf'
            engine = ['--model','/poly/model.gguf','--threads','16','--max-context','32768',
                      '--vram-reserve-mib','1200','--kv','q8_0','--no-draft','--prompt-cache','2','--kv-whole']
            if not native and not probe:
                # Legacy launches inherit Docker env; native launches use only
                # the selected profile's clean engine env. Never set the flag on
                # the native parent/reader or on Flash/probe engines.
                command += ['-e','STRATA_FREEZE_EXPERTS=1']
        if probe:
            front[front.index('--engine')+1] = '/qualification/probe'
            engine = ['--fixture-dir','/probe'] # constant across config arms: output path must not confound key namespaces
        front += ['--threads','4','--threads-http','8','--picture-cache','2','--image-max-tokens',str(image_tokens),'--log-disable']
        if native:
            front += ['--native-vision','/profile.json']
        command += ['-p',f'127.0.0.1:{PORT}:8080','--entrypoint','/candidate/unbiased-strata',prod['Image'],*front,'--',*engine]
        active = True  # finally owns even a partially successful Docker launch
        docker(*command)
        began = time.monotonic()
        for _ in range(600):
            require(inspect(NAME)['State']['Running'], 'candidate exited before READY')
            try:
                if request('/health',timeout=2)[0] == 200:
                    break
            except (OSError,http.client.HTTPException):
                pass
            time.sleep(1)
        else:
            raise RuntimeError('candidate READY deadline')
        contexts = driver.registry()
        if native:
            require(inspect(NAME)['State']['Pid'] not in contexts, 'native parent allocated a GPU context')
            require(len(contexts) == (1 if probe else 2), 'native GPU ownership mismatch')
        record(stage+'-ready', seconds=round(time.monotonic()-began,2), contexts=contexts,
               frozen_poly_engine=family == 'poly' and not probe, whole_kv=family == 'poly' and not probe)
        if family == 'poly' and not probe:
            # Observe the actual child launch rather than trusting argv assembly.
            # Only fixed public-safe facts are retained, never process env dumps.
            check = docker('exec',NAME,'sh','-ec',r'''
                engines=0; readers=0
                for p in /proc/[0-9]*; do
                    [ -r "$p/comm" ] || continue
                    read -r comm < "$p/comm" || continue
                    case "$comm" in
                        strata-poly)
                            engines=$((engines+1))
                            tr '\000' '\n' < "$p/environ" | grep -Fxq STRATA_FREEZE_EXPERTS=1
                            tr '\000' '\n' < "$p/cmdline" | grep -Fxq -- --kv-whole ;;
                        strata-vision-r*)
                            readers=$((readers+1))
                            reader_env=$(tr '\000' '\n' < "$p/environ")
                            if printf '%s\n' "$reader_env" | grep -q '^STRATA_FREEZE_EXPERTS='; then exit 1; fi ;;
                    esac
                done
                [ "$engines" -eq 1 ]; [ "$readers" -eq "$1" ]
                echo POLY_FROZEN_WHOLE_KV_CONFIRMED
                ''','policy-check','1' if native else '0')
            require(check == 'POLY_FROZEN_WHOLE_KV_CONFIRMED', 'actual poly launch policy mismatch')
            record(stage+'-policy', engine_freeze=True, whole_kv=True, reader_isolated=True)

    def captures(stage=None):
        return [json.loads(s) for s in (out/(stage or current)/'probe/results').read_text().splitlines()]

    def same(a,b,keys=True):
        require(all(a[k] == b[k] for k in ('tokens','token_sha','shapes','image_cells')), 'exact prompt/grid parity failed')
        if keys:
            require(a['keys_sha'] == b['keys_sha'], 'same-config image-key mismatch')
        require(a['row_sha'] == b['row_sha'], 'exact row parity failed')

    def exclusion(stage=None):
        engine, reader = False, None
        for name in phases(stage):
            if name == 'engine_gen':
                require(not engine and reader is None, 'overlapping GPU turn'); engine = True
            elif name == 'engine_done':
                require(engine and reader is None, 'DONE/reader ordering failed'); engine = False
            elif name.startswith('reader_') and name.endswith('_begin'):
                require(not engine and reader is None, 'reader overlapped engine'); reader = name[:-6]
            elif name.startswith('reader_') and name.endswith('_end'):
                require(not engine and reader == name[:-4], 'reader phase ordering failed'); reader = None
        require(not engine and reader is None, 'unterminated GPU phase')
        record((stage or current)+'-exclusion', no_overlap=True, counts={n:phases(stage).count(n) for n in
               ('reader_load_begin','reader_decode_begin','reader_prepare_begin','reader_encode_begin','engine_gen','engine_done')})

    def recover():
        top = docker('top',NAME,'-eo','pid,comm').splitlines()[1:]
        readers = [int(s.split()[0]) for s in top if 'strata-vision-r' in s]
        require(len(readers) == 1, 'owned reader ambiguous')
        before = driver.registry(); require(readers[0] in before, 'reader GPU binding absent')
        engine = {pid:b for pid,b in before.items() if pid != readers[0]}
        os.kill(readers[0], signal.SIGKILL)
        for _ in range(150):
            if readers[0] not in driver.registry():
                break
            time.sleep(.1)
        else:
            raise RuntimeError('reader GPU context did not clear')
        for _ in range(150):
            status, raw = request('/props')
            if status == 200 and json.loads(raw)['vision_status'] == 'unavailable':
                break
            time.sleep(.1)
        else:
            raise RuntimeError('reader unavailability deadline')
        return engine

    def restart(engine):
        time.sleep(1.1)  # explicit profile backoff, not a registration/ordering oracle
        require(request('/vision/recover',{})[0] == 200, 'explicit recovery refused')
        for _ in range(150):
            status, raw = request('/props')
            if status == 200 and json.loads(raw)['vision_status'] == 'available':
                break
            time.sleep(.1)
        else:
            raise RuntimeError('reader recovery deadline')
        after = driver.registry()
        require(all(after.get(pid) == n for pid,n in engine.items()), 'engine allocation changed during reader recovery')
        record(current+'-recovered', retained_engine_allocations=engine, contexts=after)

    def smoke_production():
        require(request('/v1/models',port=8080,token=None)[0] == 401, 'production auth regression')
        require(request('/v1/models',port=8080,token='invalid-qualification-token')[0] == 401, 'production wrong-key regression')
        require(request('/health',port=8080,token=None)[0] == 200 and request('/v1/models',port=8080)[0] == 200,
                'production health/models regression')
        for stream in (False,True):
            answer,_ = chat(body('What is six times seven? Answer with the number.',stream,tokens=32),port=8080)
            require('42' in answer, 'production text/SSE regression')
        for _ in range(2):
            answer,_ = chat(body([image((255,0,0)),text('Name the dominant color. Answer with one color word.')]),port=8080)
            require('red' in answer.lower(), 'production vision regression')
        record('production-smoke-pass', authentication=True, health_models=True, text=True, stream=True, vision=True)

    failure = None
    try:
        paused = True; docker('stop','-t','60',prod['Id'])
        require(driver.empty(), 'approved GPU occupied after production stop')
        record('maintenance-start', original_image=prod['Image'], gpu_empty=True)
        command = base('gpu-cache-queue')
        flash = list(prod['Config']['Cmd'])
        model, projector = flash[flash.index('-m')+1], flash[flash.index('--mmproj')+1]
        command += ['--entrypoint','/qualification/gpu-cache',prod['Image'],'/profile.json',model,projector,
                    '/candidate/strata-process-guard','/candidate/strata-vision-reader','/qualification/probe',
                    '/probe']
        active = True
        docker(*command)
        for _ in range(300):
            if not inspect(NAME)['State']['Running']:
                break
            time.sleep(1)
        else:
            raise RuntimeError('GPU queue fixture deadline')
        state = inspect(NAME)['State']; output = docker('logs',NAME)
        require(state['ExitCode'] == 0 and 'PR5_GPU_CACHE_QUEUE_PASS' in output and key not in output, 'GPU queue fixture failed')
        record('gpu-cache-queue-pass', physical_safety=True, warm_zero_work=True, exact_ids_keys_rows=True,
               producer_waiter_cancel=True, eviction_pins=True, all_gone=True, readerless=True, restart=True)
        stop(); exclusion('gpu-cache-queue')
        cases = fixtures.corpus()
        for probe_family in ('strata','poly'):
            for native in (False,True):
                stage = probe_family+('-native-probe' if native else '-legacy-probe')
                start(stage,family=probe_family,native=native,probe=True)
                for name,data in cases:
                    if native and name == 'cache-a-restart':
                        restart(recover())
                    before = counts(); chat(data)
                    if native and name in ('cache-a-hit','cache-a-restart'):
                        no_reader_work(before)
                    record(stage+'-case', name=name, passed=True, delta={k:counts()[k]-v for k,v in before.items()})
                stop()
                if native:
                    exclusion(stage)
            legacy = captures(probe_family+'-legacy-probe')
            candidate = captures(probe_family+'-native-probe')
            require(len(legacy) == len(candidate) == len(cases), 'probe corpus count mismatch')
            for (name,_),a,b in zip(cases,legacy,candidate):
                same(a,b,keys=False)  # PR5 deliberately versions keys; tokenizer metadata stays exact
                if a['image_cells']:
                    require(a['keys_sha'] != b['keys_sha'], 'native image namespace was not versioned')
                record('gpu-corpus-parity', family=probe_family, name=name, exact_tokens_grids_rows=True,
                       versioned_native_keys=bool(a['image_cells']))
            variant_stage = probe_family+'-config64-probe'
            start(variant_stage,family=probe_family,probe=True,image_tokens=64)
            chat(cases[1][1]); before=counts(); chat(cases[1][1]); no_reader_work(before)
            variant=captures(); same(variant[0],variant[1]); same(candidate[1],variant[0],keys=False)
            require(candidate[1]['keys_sha'] != variant[0]['keys_sha'], 'output-affecting config did not namespace image key')
            record('config-key-versioning', family=probe_family, same_input_tokens_rows=True, different_policy_keys=True, warm_zero_work=True)
            stop(); exclusion(variant_stage)

        for family in ('strata','poly'):
            for native in (False,True):
                stage = family+('-native' if native else '-legacy')
                start(stage,family,native)
                for color,rgb in [('red',(255,0,0)),('blue',(0,0,255)),('green',(0,255,0))]:
                    for stream in (False,True):
                        before=counts(); answer,usage=chat(body([image(rgb),text('Name the dominant color. Answer with one color word.')],stream))
                        require(color in answer.lower(), 'real-engine color regression')
                        if native and stream:
                            no_reader_work(before)
                        record(stage+'-color', color=color, stream=stream, correct=True, usage=usage, delta={k:counts()[k]-v for k,v in before.items()})
                ordered = [image((255,0,0),224,224),image((0,0,255),448,112)]
                for label,pictures in [('red',ordered),('red',ordered),('blue',ordered[::-1]),('red',[ordered[0],ordered[1],ordered[0]])]:
                    before=counts(); answer,usage=chat(body([*pictures,text('Name the dominant color in the FIRST image. Answer with one color word.')]))
                    require(label in answer.lower(), 'real-engine ordered/cache identity regression')
                    if native and label == 'blue':
                        no_reader_work(before)
                    record(stage+'-ordered', first=label, occurrences=len(pictures), correct=True, usage=usage)
                for name,prompt,wanted in [('image-to-text','What is two plus two? Answer with one digit.','4'),
                                           ('literal','Repeat exactly <|image_pad|>.','<|image_pad|>')]:
                    answer,_=chat(body(prompt)); require(wanted in answer, 'text/literal regression')
                    record(stage+'-text', name=name, correct=True)
                if native:
                    # Warm context refusal stays pre-SSE and cannot run reader/GEN.
                    warm=body([image((255,0,0)),text('Name the dominant color. Answer with one color word.')])
                    chat(warm); before=counts()
                    nctx=262144 if family == 'strata' else 32768
                    over=body([image((255,0,0)),text(' x'*nctx)],True,tokens=1)
                    require(request('/v1/chat/completions',over)[0] == 400, 'warm context admission regression')
                    no_reader_work(before); require(counts()['engine_gen'] == before['engine_gen'], 'refused request reached GEN')
                    chat(warm); record(stage+'-warm-refusal', pre_sse=True, zero_reader_gen=True, follower=True)
                    # Actual real-engine cancellation with pending uncached vision.
                    conn=http.client.HTTPConnection('127.0.0.1',PORT,timeout=180)
                    conn.request('POST','/v1/chat/completions',json.dumps(body('Write a very long numbered list of integers.',True,4096)),
                                 {'Authorization':'Bearer '+key,'Content-Type':'application/json'})
                    response=conn.getresponse(); require(response.status == 200 and response.readline().startswith(b'data:'), 'stream not active')
                    require(counts()['engine_gen'] > counts()['engine_done'], 'engine terminal overtook cancellation setup')
                    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                        follower=pool.submit(chat,body([image((255,255,0),333,197),text('Describe briefly.')]))
                        require(request('/vision/recover',{})[0] == 503, 'recovery overlapped engine')
                        response.close(); conn.close(); follower.result(timeout=180)
                    record(stage+'-cancel-follower', passed=True)
                    # Pin a complete hit and check largest-context admission
                    # with both reader and engine resident.
                    chat(warm)
                    token=json.loads(request('/tokenize',{'content':' hello'})[1])['tokens'][0]
                    status,raw=request('/completion',{'prompt':[token]*(nctx-9),'n_predict':1,'temperature':0},timeout=600)
                    require(status == 200 and json.loads(raw)['tokens_evaluated'] == nctx-9, 'high-water context regression')
                    resources(); engine=recover()
                    before=counts(); answer,_=chat(warm); require('red' in answer.lower(), 'readerless cached vision regression'); no_reader_work(before)
                    require(request('/v1/chat/completions',body([image((1,2,3)),text('Describe.')]))[0] == 503, 'readerless miss was admitted')
                    # Readerless cached GENI can legitimately shrink KV. Regrow
                    # it explicitly, then make NO generation between this receipt
                    # and restart. This also tests that reader absence cannot be
                    # used to steal its reserved restart headroom.
                    status,raw=request('/completion',{'prompt':[token]*(nctx-9),'n_predict':1,'temperature':0},timeout=600)
                    require(status == 200 and json.loads(raw)['tokens_evaluated'] == nctx-9, 'readerless high-water context regression')
                    require(set(driver.registry()) == set(engine), 'readerless GPU owner changed')
                    after=driver.registry(); resources()
                    record(stage+'-static-budget', resident_full_kv=engine, readerless_full_kv=after,
                           evaluated_tokens=nctx-9)
                    require(all(after[pid] <= n for pid,n in engine.items()),
                            'readerless engine allocation grew beyond the qualified full-KV boundary')
                    engine=after
                    restart(engine)
                    resources()
                    # Do not let a cached GENI shrink KV and hide insufficient
                    # encoder headroom. The first post-restart turn must ENCODE
                    # a fresh admitted image while the full engine allocation
                    # is still resident; GEN follows only after reader completion.
                    before=counts()
                    answer,_=chat(body([image((255,255,0),1024,1024),text('Name the dominant color. Answer with one color word.')]))
                    require('yellow' in answer.lower(), 'fresh high-water encode regression')
                    require(all(counts()[k] == before[k]+1 for k in
                                ('reader_decode_begin','reader_prepare_begin','reader_encode_begin')), 'fresh high-water encode not observed')
                    before=counts(); answer,_=chat(warm); require('red' in answer.lower(), 'restarted cached vision regression'); no_reader_work(before)
                    record(stage+'-highwater-cache-restart', evaluated_tokens=nctx-9, cached_readerless=True, miss_refused=True,
                           regrown_before_restart=True, fresh_encode_before_gen=True, warm_restart_zero_work=True)
                resources(); stop()
                if native:
                    exclusion(stage)
        record('PR5_GPU_ACCEPTANCE_PASS', corpus_cases_per_projector=len(cases), projector_widths=[2560,2048], engines=['strata','strata-poly'],
               exact_rows_tokens=True, zero_warm_reader_work=True, bounded_singleflight=True)
    except BaseException as exc:
        failure=exc
        record('qualification-failed', stage=current, error_type=type(exc).__name__,
               check=str(exc) if isinstance(exc,QualificationFailure) else None)
    finally:
        try:
            stop(check_exit=False)
            require(driver.empty(), 'GPU uncertain: production must remain stopped')
            if paused:
                now=inspect(prod['Id'])
                require(settings(now) == original and now['Image'] == prod['Image'], 'production settings changed')
                require(all(digest(p) == h for p,h in compose.items()), 'Compose/backup changed')
                docker('start',prod['Id'])
                for _ in range(600):
                    if inspect(prod['Id'])['State']['Health']['Status'] == 'healthy':
                        break
                    time.sleep(1)
                else:
                    raise RuntimeError('production restoration deadline')
                smoke_production()
                record('production-restored', healthy=True, original_image_settings=True, compose_backup_unchanged=True, candidate_removed=True)
        except BaseException as exc:
            record('restoration-requires-attention', error_type=type(exc).__name__, gpu_work_must_not_restart_blindly=True)
            if failure is None:
                failure=exc
        finally:
            # All fixtures are synthetic. Keep public-safe hashes/counts, not rows.
            for path in out.glob('*/probe/*.rows'):
                path.unlink()
            driver.close()
    if failure is not None:
        raise SystemExit(1)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise InterruptedError('qualification interrupted')
    signal.signal(signal.SIGTERM,interrupted)
    signal.signal(signal.SIGINT,interrupted)
    main()
