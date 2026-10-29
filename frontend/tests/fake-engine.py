#!/usr/bin/env python3
"""A stand-in for strata / strata-poly that speaks the line protocol (docs/BACKENDS.md), for tests without a GPU.

  GEN <max_new> [key=value ...] <id,id,...>   -> T <id> per token, then DONE
  STOP                                         -> the current request ends with finish=cancel
  QUIT                                         -> exit 0

Environment variables choose the answers, so one script covers the cases:
  FAKE_TOKENS     comma-separated ids to emit (default: the prompt ids echoed back, capped by max_new)
  FAKE_N_TOKENS   emit this many tokens of id FAKE_TOKEN_ID instead (burst tests)
  FAKE_TOKEN_ID   the id used by FAKE_N_TOKENS (default 100)
  FAKE_DELAY_MS   sleep this long between tokens (default 0)
  FAKE_CONTEXT    the context in the READY line (default 4096)
  FAKE_ERR        if set, answer every GEN with "ERR <text>" instead
  FAKE_REFUSE_GENI  if set, reject GENI (a text-only backend without --vision)
  FAKE_DIE_AFTER  if set, exit 3 after emitting this many tokens of the first request
  FAKE_NO_STOP    if set, READY without "stop": STOP is ignored and the request runs to its end
  FAKE_PROMPT_N   if set, lie about the prompt size in DONE (mismatch test)
  FAKE_GATE       first GEN waits for this file (bounded by FAKE_GATE_SECONDS, default 15)
  FAKE_CAPTURE    append JSON command/terminal events to this file
  FAKE_TERMINALS  JSON list of per-request modes: ok, err, mismatch, nonfinite, eof
  FAKE_CANCEL_GATE  on STOP emit residual T/PP, then wait for this file before DONE
  FAKE_CLOSE_INPUT  close stdin before READY, then exit after two seconds (write-failure fixture)
"""
import json
import os
from pathlib import Path
import select
import sys
import time

out = sys.stdout


def say(s):
    out.write(s + "\n")
    out.flush()


env = os.environ.get
say("INFO engine=fake batch_slots=0")
if env("FAKE_CLOSE_INPUT"):
    os.close(0)
say("READY %s%s" % (env("FAKE_CONTEXT", "4096"), "" if env("FAKE_NO_STOP") else " stop"))
if env("FAKE_CLOSE_INPUT"):
    time.sleep(2)
    sys.exit(0)

def capture(event, **fields):
    if env("FAKE_CAPTURE"):
        with open(env("FAKE_CAPTURE"), "a") as f:
            f.write(json.dumps(dict(event=event, **fields)) + "\n")


def gate(path):
    if not path:
        return
    deadline = time.monotonic() + float(env("FAKE_GATE_SECONDS", "15"))
    while not Path(path).exists():
        if time.monotonic() >= deadline:
            raise TimeoutError("fake engine gate timed out")
        time.sleep(0.005)


first = True
request_number = 0
pending = []  # lines read while checking for STOP that are not STOP/QUIT


def readline():
    if pending:
        return pending.pop(0)
    line = sys.stdin.readline()
    if line == "":
        sys.exit(0)
    line = line.rstrip("\n")
    capture("command", line=line)
    return line


def stop_requested():
    """Consume what is readable now; True if a STOP was among it."""
    stop = False
    while select.select([sys.stdin], [], [], 0)[0]:
        line = sys.stdin.readline()
        if line == "":
            sys.exit(0)
        line = line.rstrip("\n")
        capture("command", line=line)
        if line == "STOP":
            stop = True
        elif line == "QUIT":
            sys.exit(0)
        else:
            pending.append(line)
    return stop


while True:
    line = readline()
    if line == "QUIT":
        sys.exit(0)
    if line == "STOP" or line == "":
        continue
    parts = line.split()
    if parts[0] not in ("GEN", "GENI") or len(parts) < 3:
        say("ERR expected: GEN <max_new> <id,id,...>")
        continue
    if parts[0] == "GENI" and env("FAKE_REFUSE_GENI"):
        say("ERR GENI requires --vision")
        continue
    max_new = int(parts[1])
    prompt = [int(x) for x in parts[-1].split(",") if x]
    prompt_n = len(prompt)
    request_number += 1
    modes = json.loads(env("FAKE_TERMINALS", "[]"))
    mode = modes[request_number - 1] if request_number <= len(modes) else "ok"
    if first:
        gate(env("FAKE_GATE"))
    if mode == "eof":
        sys.exit(3)
    if mode == "err":
        first = False
        say("ERR selected failure")
        capture("terminal", request=request_number)
        continue
    if env("FAKE_ERR"):
        say("ERR " + env("FAKE_ERR"))
        continue
    say("RESUME 0")
    say("PP %d %d 1 1000" % (prompt_n, prompt_n))
    if env("FAKE_N_TOKENS"):
        tokens = [int(env("FAKE_TOKEN_ID", "100"))] * int(env("FAKE_N_TOKENS"))
    elif env("FAKE_TOKENS"):
        tokens = [int(x) for x in env("FAKE_TOKENS").split(",") if x]
    else:
        tokens = prompt
    delay = float(env("FAKE_DELAY_MS", "0")) / 1000.0
    die_after = int(env("FAKE_DIE_AFTER", "0") or 0)
    produced = 0
    finish = "stop"
    for t in tokens:
        if produced >= max_new:
            finish = "length"
            break
        if stop_requested() and not env("FAKE_NO_STOP"):
            finish = "cancel"
            if env("FAKE_CANCEL_GATE"):
                say("T 100")
                say("PP 1 %d 1 1000" % prompt_n)
                capture("residual", request=request_number)
                gate(env("FAKE_CANCEL_GATE"))
            break
        say("T %d" % t)
        produced += 1
        if die_after and first and produced >= die_after:
            sys.exit(3)
        if delay:
            time.sleep(delay)
    first = False
    # DONE <generated> <prompt> <prompt_ms> <decode_ms> <finish> <drafts_accepted> <drafts_offered> <reused>
    if mode == "nonfinite":
        finish = "nonfinite"
    reported_prompt = str(prompt_n + 1) if mode == "mismatch" else env("FAKE_PROMPT_N", str(prompt_n))
    capture("terminal", request=request_number)
    say("DONE %d %s 12.5 %.1f %s 0 0 0" % (produced, reported_prompt, produced * 10.0, finish))
