"""Wrap the ROM's code segments in a big-endian MIPS ELF so Rizin/Ghidra map them at their RAM addresses.

Usage: python -I tools/mkelf.py <rom.z64> <out.elf>
Segments come from analysis/rom_map.json (regions with "kind": "code").
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAP = os.path.join(HERE, "..", "analysis", "rom_map.json")


def code_segments():
    regions = json.load(open(MAP, encoding="utf-8"))["regions"]
    segs = []
    for r in regions:
        if r["kind"] != "code":
            continue
        start, end = int(r["rom_start"], 16), int(r["rom_end"], 16)
        vaddr = int(r["vram"], 16)
        memsz = int(r.get("vram_size", hex(end - start)), 16)
        segs.append(("." + r["name"], start, end, vaddr, memsz))
    return segs


def build(rom, segs):
    names = b"\0" + b"\0".join(s[0].encode() for s in segs) + b"\0.shstrtab\0"
    off = 0x1000
    body = bytearray()
    ph = b""
    sh = b"\0" * 40
    name_pos = 1
    for name, start, end, vaddr, memsz in segs:
        data = rom[start:end]
        file_off = off + len(body)
        ph += struct.pack(">IIIIIIII", 1, file_off, vaddr, vaddr, len(data), memsz, 7, 0x10)
        sh += struct.pack(">IIIIIIIIII", name_pos, 1, 7, vaddr, file_off, len(data), 0, 0, 16, 0)
        name_pos += len(name) + 1
        body += data
        body += b"\0" * ((-len(body)) % 16)
    shstr_off = off + len(body)
    sh += struct.pack(">IIIIIIIIII", name_pos, 3, 0, 0, shstr_off, len(names), 0, 0, 1, 0)
    sh_off = (shstr_off + len(names) + 3) & ~3
    entry = segs[0][3]
    eh = struct.pack(">16sHHIIIIIHHHHHH", b"\x7fELF\x01\x02\x01" + b"\0" * 9, 2, 8, 1, entry,
                     52, sh_off, 0x20000000, 52, 32, len(segs), 40, len(segs) + 2, len(segs) + 1)
    out = bytearray(eh + ph)
    out += b"\0" * (off - len(out))
    out += body + names
    out += b"\0" * (sh_off - len(out))
    out += sh
    return out


def main():
    rom = open(sys.argv[1], "rb").read()
    segs = code_segments()
    open(sys.argv[2], "wb").write(build(rom, segs))
    for name, start, end, vaddr, memsz in segs:
        print(f"{name:10} rom {start:#08x}-{end:#08x} -> {vaddr:#010x} (mem {memsz:#x})")


if __name__ == "__main__":
    main()
