"""Replay regression runner.

Each tests/replays/<name>.json holds a scripted input (MK64_INPUT syntax), the retrace to stop at,
the capture interval and the expected FNV-1a hash of each captured frame (and, with
"check_eeprom", the SHA-1 of the EEPROM the run leaves behind). The game runs headless
and deterministic; any differing hash fails the replay.

Usage:
  python -I tools/replay.py <rom.z64> [name ...]          run and compare
  python -I tools/replay.py <rom.z64> --record name ...   (re)record expected hashes
Frames of the last run stay in build/replays/<name>/ as BMP for inspection.
"""
import hashlib
import json
import os
import shutil
import struct
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
    env.update({k: str(v) for k, v in spec.get("env", {}).items()})
    checks = spec.get("ram_checks", [])
    if checks:
        env["MK64_DUMPRAM"] = ",".join(sorted({str(c["retrace"]) for c in checks}))
    subprocess.run([EXE, rom], cwd=work, env=env, stdout=subprocess.DEVNULL, check=False, timeout=1800)
    hashes = {}
    path = os.path.join(work, "replay_hashes.txt")
    if os.path.exists(path):
        for line in open(path):
            n, h = line.split()
            hashes[n] = h
    # check_eeprom: the save written by the run (records) is part of the expected result.
    eep = os.path.join(work, "mk64.eep")
    if spec.get("check_eeprom") and os.path.exists(eep):
        hashes["eeprom"] = hashlib.sha1(open(eep, "rb").read()).hexdigest()
    return hashes, [c["descricao"] for c in checks if not ram_check(work, c)]


# ram_checks: [{"retrace", "addr", "type" (u32, s32, s16, f32, fix16), "min", "max", "bits_set",
# "bits_clear", "descricao"}], evaluated on the RDRAM dumped at that retrace (N64 byte order).
def ram_check(work, c):
    path = os.path.join(work, f"ram_{c['retrace']}.bin")
    if not os.path.exists(path):
        return False
    data = open(path, "rb").read()
    off = int(c["addr"], 16) & 0x00FFFFFF
    kind = c.get("type", "u32")
    fmt = {"u32": ">I", "s32": ">i", "s16": ">h", "f32": ">f", "fix16": ">i"}[kind]
    v = struct.unpack_from(fmt, data, off)[0]
    if kind == "fix16":
        v /= 65536.0
    ok = True
    if "min" in c:
        ok &= v >= c["min"]
    if "max" in c:
        ok &= v <= c["max"]
    if "bits_set" in c:
        ok &= (v & int(c["bits_set"], 16)) == int(c["bits_set"], 16)
    if "bits_clear" in c:
        ok &= (v & int(c["bits_clear"], 16)) == 0
    return ok


def main():
    rom = sys.argv[1]
    args = sys.argv[2:]
    record = "--record" in args
    names = [a for a in args if a != "--record"] or sorted(f[:-5] for f in os.listdir(DIR) if f.endswith(".json"))
    failed = 0
    for name in names:
        path = os.path.join(DIR, name + ".json")
        spec = json.load(open(path, encoding="utf-8"))
        got, failed_checks = run(rom, spec, name)
        for what in failed_checks:
            print(f"{name}: verificacao de RAM falhou: {what}")
        failed += bool(failed_checks)
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
