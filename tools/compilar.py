"""Builds the port from the user's ROM, from scratch: checks the ROM, recompiles its code to C
(generated/), compiles the executable and puts it in a folder ready to run.

Usage: python -I tools/compilar.py <rom> [--saida PASTA] [--sem-rom] [--sem-teste]
  <rom>        the Mario Kart 64 (USA) ROM, in .z64, .v64 or .n64 byte order
  --saida      where mk64.exe goes (default: dist/)
  --sem-rom    do not copy the ROM next to the executable (it then needs the ROM path as argument)
  --sem-teste  skip the quick headless run of the finished executable

Steps, each stopping the flow with a message if it fails:
  1. ROM: byte order, size and SHA-1 (only the USA version is supported); a .v64/.n64 file is
     converted to .z64 order in build/rom.z64, which the recompiler reads.
  2. Recompilation: tools/recomp/analyze.py (analysis/functions.json) and tools/recomp/recomp.py,
     written to a new folder that then replaces generated/ (never committed: it comes from the ROM).
  3. Build: build.bat (Visual Studio + CMake + Ninja; shaders with the Vulkan SDK glslc if present).
  4. Output: mk64.exe and, unless --sem-rom, the ROM as rom.z64 (what the executable opens by default).
  5. Test: the executable runs 300 retraces without a window and must exit cleanly.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
MK64_US_SHA1 = "579c48e211ae952530ffc8738709f078d5dd215e"  # src/rom.h
ORDERS = {b"\x80\x37\x12\x40": "z64", b"\x37\x80\x40\x12": "v64", b"\x40\x12\x37\x80": "n64"}


def fail(msg):
    print(f"\nERRO: {msg}", file=sys.stderr)
    sys.exit(1)


def step(n, title):
    print(f"\n[{n}/5] {title}", flush=True)


def to_z64(data, order):
    if order == "v64":
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b)
    if order == "n64":
        b = bytearray(data)
        for k in range(4):
            b[k::4] = data[3 - k::4]
        return bytes(b)
    return data


def check_rom(path):
    if not os.path.isfile(path):
        fail(f"ROM nao encontrada: {path}")
    data = open(path, "rb").read()
    order = ORDERS.get(data[:4])
    if not order:
        fail("o arquivo nao parece uma ROM de N64 (.z64/.v64/.n64).")
    if len(data) % 4:
        fail(f"tamanho invalido ({len(data)} bytes).")
    z64 = to_z64(data, order)
    sha1 = hashlib.sha1(z64).hexdigest()
    title = z64[0x20:0x34].decode("ascii", "replace").strip()
    print(f"  {path}\n  formato {order}, {len(data) // (1024 * 1024)} MB, titulo '{title}', sha1 {sha1}")
    if sha1 != MK64_US_SHA1:
        fail(f"esta nao e a ROM suportada (Mario Kart 64 USA, sha1 {MK64_US_SHA1}).")
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    if order == "z64":
        return os.path.abspath(path), z64
    out = os.path.join(ROOT, "build", "rom.z64")
    open(out, "wb").write(z64)
    print(f"  convertida para .z64 em {out}")
    return out, z64


def run(cmd, **kw):
    t0 = time.time()
    r = subprocess.run(cmd, cwd=ROOT, **kw)
    if r.returncode:
        fail(f"o comando terminou com codigo {r.returncode}: {' '.join(cmd)}")
    return time.time() - t0


def recompile(rom):
    py = [sys.executable, "-I"]
    dt = run(py + [os.path.join("tools", "recomp", "analyze.py"), rom], stdout=subprocess.DEVNULL)
    print(f"  analise: analysis/functions.json ({dt:.0f} s)")
    new = os.path.join(ROOT, "build", "generated_novo")
    shutil.rmtree(new, ignore_errors=True)
    os.makedirs(new)
    dt = run(py + [os.path.join("tools", "recomp", "recomp.py"), rom, new], stdout=subprocess.DEVNULL)
    files = os.listdir(new)
    if not any(f.startswith("funcs_") for f in files):
        fail("a recompilacao nao gerou nenhum arquivo de codigo.")
    gen = os.path.join(ROOT, "generated")
    shutil.rmtree(gen, ignore_errors=True)
    shutil.move(new, gen)
    print(f"  recompilacao: {len(files)} arquivos em generated/ ({dt:.0f} s)")


def build():
    # The compiler's output, without /Ob1 override notes and any /showIncludes line ninja did not
    # take (build.bat sets the code page they must be in).
    t0 = time.time()
    p = subprocess.Popen(["cmd", "/c", os.path.join(ROOT, "build.bat")], cwd=ROOT, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    for raw in p.stdout:
        line = raw.decode("cp850", "replace").rstrip()
        if "incluindo arquivo:" in line or "including file:" in line or "D9025" in line:
            continue
        if line.startswith("[") and ("Building" in line or "Linking" in line):
            print(f"\r  {line[:100]:<100}", end="", flush=True)
            continue
        print(f"\n  {line}" if line else "", flush=True)
    print()
    if p.wait():
        fail(f"o build terminou com codigo {p.returncode}.")
    dt = time.time() - t0
    exe = os.path.join(ROOT, "build", "mk64.exe")
    if not os.path.isfile(exe):
        fail("o build terminou sem gerar build/mk64.exe.")
    cache = os.path.join(ROOT, "build", "CMakeCache.txt")
    glslc = [l for l in open(cache, encoding="utf-8", errors="replace") if l.startswith("GLSLC:")]
    if not glslc or "NOTFOUND" in glslc[0]:
        print("  aviso: glslc (Vulkan SDK) nao encontrado; o executavel sai so com o renderizador cpu.")
    print(f"  build/mk64.exe ({os.path.getsize(exe) // 1024} KB, {dt:.0f} s)")
    return exe


def main():
    ap = argparse.ArgumentParser(description="Recompila a ROM do Mario Kart 64 e gera o executavel do port.")
    ap.add_argument("rom")
    ap.add_argument("--saida", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--sem-rom", action="store_true")
    ap.add_argument("--sem-teste", action="store_true")
    args = ap.parse_args()
    t0 = time.time()

    step(1, "Conferindo a ROM")
    rom, z64 = check_rom(args.rom)
    step(2, "Recompilando o codigo da ROM para C")
    recompile(rom)
    step(3, "Compilando o executavel")
    exe = build()

    step(4, f"Montando a pasta {args.saida}")
    os.makedirs(args.saida, exist_ok=True)
    out_exe = os.path.join(args.saida, "mk64.exe")
    shutil.copy2(exe, out_exe)
    if not args.sem_rom:
        open(os.path.join(args.saida, "rom.z64"), "wb").write(z64)
    print("  mk64.exe" + ("" if args.sem_rom else " + rom.z64 (a configuracao mk64.ini e criada na primeira execucao)"))

    step(5, "Teste rapido sem janela")
    if args.sem_teste:
        print("  pulado (--sem-teste)")
    else:
        work = os.path.join(ROOT, "build", "teste_compilar")
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        env = dict(os.environ, MK64_HEADLESS="1", MK64_EXIT_AT="300")
        r = subprocess.run([out_exe, rom], cwd=work, env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=600)
        if r.returncode:
            fail(f"o executavel terminou com codigo {r.returncode} (veja {work}\\mk64_runtime.log).")
        print("  300 retraces sem erro")

    print(f"\nPronto em {time.time() - t0:.0f} s: {out_exe}")


if __name__ == "__main__":
    main()
