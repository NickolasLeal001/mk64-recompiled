"""Regression checks for the decoder and the function inventory.

Usage: python -I tools/recomp/test_analysis.py <rom.z64>
The decoder was validated against Rizin on all 253,318 instructions inside functions
(REA ev_b94e4b28...); these spot checks keep later edits from regressing it.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mips  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print("FAIL", msg)


def main():
    rom = open(sys.argv[1], "rb").read()

    def at(a):
        return mips.decode(struct.unpack_from(">I", rom, 0x1000 + a - 0x80000400)[0], a)

    # Entry point: BSS clear loop then jr to 0x80000484.
    check(mips.fmt_insn(at(0x80000400)) == "lui t0, 0x800f", "entry lui")
    check(at(0x8000041c).op == "bne" and at(0x8000041c).target == 0x80000410, "entry loop branch")
    check(at(0x80000430).op == "jr" and at(0x80000430).rs == 10, "entry jr t2")
    # Reserved fields: lui with rs != 0 is not an instruction.
    check(mips.decode(0x3F800000, 0).op == "invalid", "1.0f is not lui")
    check(mips.decode(0x00000000, 0).op == "nop", "nop")
    check(mips.decode(0x03E00008, 0).op == "jr", "jr ra")
    check(mips.decode(0x46000005, 0).op == "abs.s", "abs.s")
    check(mips.decode(0x46001021, 0).op == "cvt.d.s", "cvt.d.s")
    check(mips.decode(0x46201021, 0).op == "invalid", "cvt.d.d does not exist")
    check(mips.decode(0x45010003, 0x100).op == "bc1t", "bc1t")

    d = json.load(open(os.path.join(ROOT, "analysis", "functions.json"), encoding="utf-8"))
    funcs = d["functions"]
    by_addr = {int(f["addr"], 16): f for f in funcs}
    check(len(funcs) >= 3000, f"function count {len(funcs)}")
    iv = sorted((int(f["addr"], 16), int(f["addr"], 16) + int(f["size"], 16)) for f in funcs)
    check(all(c >= b for (a, b), (c, _) in zip(iv, iv[1:])), "functions overlap")
    for a in (0x80000400, 0x80000484, 0x8028df00, 0x80280000, 0x80033ae0, 0x8002d268, 0x800cdc30):
        check(a in by_addr, f"missing function {a:#x}")
    check(by_addr[0x800cdc30].get("name") == "osPiStartDma", "osPiStartDma name")
    unresolved = [x for f in funcs for x in f["unresolved_jr"]]
    check(sorted(unresolved) == ["0x80040c8c", "0x800d1790"], f"unexpected unresolved jr: {unresolved}")
    check(d["segments"]["main"]["text_end"] == "0x800d8f70", "main text end (RSP code follows)")

    print("ok" if not failures else f"{failures} falha(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
