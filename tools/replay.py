"""Replay regression runner.

Each tests/replays/<name>.json holds a scripted input (MK64_INPUT syntax), the retrace to stop at,
the capture interval and the expected FNV-1a hash of each captured frame. The game runs headless
and deterministic; any differing hash fails the replay.

Usage:
  python -I tools/replay.py <rom.z64> [name ...]          run and compare
  python -I tools/replay.py <rom.z64> --record name ...   (re)record expected hashes
Frames of the last run stay in build/replays/<name>/ as BMP for inspection.
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DIR = os.path.join(ROOT, "tests", "replays")
EXE = os.path.join(ROOT, "build", "mk64.exe")


def run(rom, spec, name):
    work = os.path.join(ROOT, "build", "replays", name)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    env = dict(os.environ, MK64_HEADLESS="1", MK64_INPUT=spec["input"], MK64_EXIT_AT=str(spec["exit_at"]),
               MK64_SHOTS=str(spec["shots"]))
    for k in (2, 3, 4):
        if spec.get(f"input{k}"):
            env[f"MK64_INPUT{k}"] = spec[f"input{k}"]
    if spec.get("autopilot"):
        env["MK64_AUTOPILOT"] = str(spec["autopilot"])
    subprocess.run([EXE, rom], cwd=work, env=env, stdout=subprocess.DEVNULL, check=False, timeout=1800)
    hashes = {}
    path = os.path.join(work, "replay_hashes.txt")
    if os.path.exists(path):
        for line in open(path):
            n, h = line.split()
            hashes[n] = h
    return hashes


def main():
    rom = sys.argv[1]
    args = sys.argv[2:]
    record = "--record" in args
    names = [a for a in args if a != "--record"] or sorted(f[:-5] for f in os.listdir(DIR) if f.endswith(".json"))
    failed = 0
    for name in names:
        path = os.path.join(DIR, name + ".json")
        spec = json.load(open(path, encoding="utf-8"))
        got = run(rom, spec, name)
        if record:
            spec["expected"] = got
            json.dump(spec, open(path, "w", encoding="utf-8"), indent=1)
            print(f"{name}: gravado ({len(got)} quadros)")
            continue
        exp = spec.get("expected", {})
        bad = [n for n in exp if got.get(n) != exp[n]]
        if bad or not exp:
            failed += 1
            print(f"{name}: FALHOU em {bad[:5] or 'sem referencia'}")
        else:
            print(f"{name}: ok ({len(exp)} quadros)")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
