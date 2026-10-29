"""Static recompiler: MIPS R4300 functions from analysis/functions.json -> C.

Usage: python -I tools/recomp/recomp.py <rom.z64> <out_dir>
Writes funcs.h (prototypes), funcs_NN.c (bodies) and lookup.c (address -> function table).
Semantics live in runtime/recomp.h; see that header for the memory layout.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mips  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
FUNCS_PER_FILE = 150


def R(n):
    return "0" if n == 0 else f"ctx->r[{n}]"


def U32(n):
    return "0u" if n == 0 else f"(uint32_t)ctx->r[{n}]"


def setr(n, expr):
    return "" if n == 0 else f"ctx->r[{n}] = {expr};"


def addr_expr(i):
    return f"({U32(i.rs)} + (uint32_t){i.imm})"


FPU_ROUND = {"round": 0, "trunc": 1, "ceil": 2, "floor": 3}
COND_BITS = {name: k for k, name in enumerate(mips.FPU_COND)}


def fpu(i):
    o = i.op
    fs, ft, fd = i.fs, i.ft, i.fd
    if o in ("mfc1",):
        return setr(i.rt, f"SX32(ctx->f[{fs}])")
    if o == "mtc1":
        return f"ctx->f[{fs}] = {U32(i.rt)};"
    if o == "dmfc1":
        return setr(i.rt, f"fl_get(ctx, {fs})")
    if o == "dmtc1":
        return f"fl_set(ctx, {fs}, {R(i.rt)});"
    if o == "cfc1":
        return setr(i.rt, "SX32(ctx->fcr31)" if fs == 31 else "SX32(0x00000a00u)")
    if o == "ctc1":
        return f"ctx->fcr31 = {U32(i.rt)};" if fs == 31 else ""
    name, fmt = o.rsplit(".", 1)
    get = {"s": "fs_get", "d": "fd_get"}.get(fmt)
    put = {"s": "fs_set", "d": "fd_set"}.get(fmt)
    a, b = f"{get}(ctx, {fs})" if get else None, f"{get}(ctx, {ft})" if get else None
    if name.startswith("c."):
        bits = COND_BITS[name[2:]] & 7
        parts = []
        if bits & 4:
            parts.append(f"({a} < {b})")
        if bits & 2:
            parts.append(f"({a} == {b})")
        if bits & 1:
            parts.append(f"isnan({a}) || isnan({b})")
        return f"fcond_set(ctx, {' || '.join(parts) if parts else '0'});"
    ops = {"add": "+", "sub": "-", "mul": "*", "div": "/"}
    nan = "fnan_s" if fmt == "s" else "fnan_d"
    if name in ops:
        return f"{put}(ctx, {fd}, {nan}({a} {ops[name]} {b}));"
    if name == "sqrt":
        return f"{put}(ctx, {fd}, {nan}({'sqrtf' if fmt == 's' else 'sqrt'}({a})));"
    if name == "abs":
        return f"{put}(ctx, {fd}, {'fabsf' if fmt == 's' else 'fabs'}({a}));"
    if name == "neg":
        return f"{put}(ctx, {fd}, -{a});"
    if name == "mov":
        return f"ctx->f[{fd}] = ctx->f[{fs}];" if fmt == "s" else f"fl_set(ctx, {fd}, fl_get(ctx, {fs}));"
    # Conversions: source value as double (exact for s, w and d; l may round, as on hardware).
    src = {"s": f"(double)fs_get(ctx, {fs})", "d": f"fd_get(ctx, {fs})",
           "w": f"(double)(int32_t)ctx->f[{fs}]", "l": f"(double)(int64_t)fl_get(ctx, {fs})"}[fmt]
    if name == "cvt.s":
        if fmt == "l":
            return f"fs_set(ctx, {fd}, (float)(int64_t)fl_get(ctx, {fs}));"
        if fmt == "w":
            return f"fs_set(ctx, {fd}, (float)(int32_t)ctx->f[{fs}]);"
        return f"fs_set(ctx, {fd}, fnan_s((float){src}));"
    if name == "cvt.d":
        return f"fd_set(ctx, {fd}, fnan_d({src}));"
    kind, dst = name.split(".")
    mode = "RM_CUR(ctx)" if kind == "cvt" else str(FPU_ROUND[kind])
    if dst == "w":
        return f"ctx->f[{fd}] = (uint32_t)fpu_to_w({src}, {mode});"
    return f"fl_set(ctx, {fd}, (uint64_t)fpu_to_l({src}, {mode}));"


def plain(i):
    """C statements for one non-control-flow instruction."""
    o = i.op
    rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
    if o in ("nop", "sync", "cache"):
        return ""
    if i.kind == "fpu":
        return fpu(i)
    if o in ("addu", "add"):
        return setr(rd, f"SX32({U32(rs)} + {U32(rt)})")
    if o in ("subu", "sub"):
        return setr(rd, f"SX32({U32(rs)} - {U32(rt)})")
    if o in ("addiu", "addi"):
        return setr(rt, f"SX32({U32(rs)} + (uint32_t){i.imm})")
    if o in ("daddiu", "daddi"):
        return setr(rt, f"{R(rs)} + (uint64_t)(int64_t){i.imm}")
    if o in ("daddu", "dadd"):
        return setr(rd, f"{R(rs)} + {R(rt)}")
    if o in ("dsubu", "dsub"):
        return setr(rd, f"{R(rs)} - {R(rt)}")
    if o in ("and", "or", "xor"):
        return setr(rd, f"{R(rs)} {dict(zip(('and', 'or', 'xor'), '&|^'))[o]} {R(rt)}")
    if o == "nor":
        return setr(rd, f"~({R(rs)} | {R(rt)})")
    if o in ("andi", "ori", "xori"):
        return setr(rt, f"{R(rs)} {dict(zip(('andi', 'ori', 'xori'), '&|^'))[o]} (uint64_t){i.uimm}u")
    if o == "lui":
        return setr(rt, f"SX32({i.uimm << 16:#x}u)")
    if o == "slt":
        return setr(rd, f"(uint64_t)((int64_t){R(rs)} < (int64_t){R(rt)})")
    if o == "sltu":
        return setr(rd, f"(uint64_t)({R(rs)} < {R(rt)})")
    if o == "slti":
        return setr(rt, f"(uint64_t)((int64_t){R(rs)} < (int64_t){i.imm})")
    if o == "sltiu":
        return setr(rt, f"(uint64_t)({R(rs)} < (uint64_t)(int64_t){i.imm})")
    if o == "sll":
        return setr(rd, f"SX32({U32(rt)} << {sa})")
    if o == "srl":
        return setr(rd, f"SX32({U32(rt)} >> {sa})")
    if o == "sra":
        return setr(rd, f"SX32((int32_t){U32(rt)} >> {sa})")
    if o == "sllv":
        return setr(rd, f"SX32({U32(rt)} << ({U32(rs)} & 31))")
    if o == "srlv":
        return setr(rd, f"SX32({U32(rt)} >> ({U32(rs)} & 31))")
    if o == "srav":
        return setr(rd, f"SX32((int32_t){U32(rt)} >> ({U32(rs)} & 31))")
    if o in ("dsll", "dsll32"):
        return setr(rd, f"{R(rt)} << {sa + (32 if o.endswith('32') else 0)}")
    if o in ("dsrl", "dsrl32"):
        return setr(rd, f"{R(rt)} >> {sa + (32 if o.endswith('32') else 0)}")
    if o in ("dsra", "dsra32"):
        return setr(rd, f"(uint64_t)((int64_t){R(rt)} >> {sa + (32 if o.endswith('32') else 0)})")
    if o == "dsllv":
        return setr(rd, f"{R(rt)} << ({R(rs)} & 63)")
    if o == "dsrlv":
        return setr(rd, f"{R(rt)} >> ({R(rs)} & 63)")
    if o == "dsrav":
        return setr(rd, f"(uint64_t)((int64_t){R(rt)} >> ({R(rs)} & 63))")
    if o in ("mult", "multu", "div", "divu", "dmult", "dmultu", "ddiv", "ddivu"):
        return f"op_{o}(ctx, {R(rs)}, {R(rt)});"
    if o == "mfhi":
        return setr(rd, "ctx->hi")
    if o == "mflo":
        return setr(rd, "ctx->lo")
    if o == "mthi":
        return f"ctx->hi = {R(rs)};"
    if o == "mtlo":
        return f"ctx->lo = {R(rs)};"
    a = addr_expr(i)
    loads = {"lb": "(uint64_t)(int64_t)(int8_t)mem_r8", "lbu": "(uint64_t)mem_r8",
             "lh": "(uint64_t)(int64_t)(int16_t)mem_r16", "lhu": "(uint64_t)mem_r16",
             "lw": "SX32(mem_r32", "ll": "SX32(mem_r32", "lwu": "(uint64_t)mem_r32", "ld": "mem_r64"}
    if o in loads:
        fn = loads[o]
        close = ")" if fn.startswith("SX32(") else ""
        return setr(rt, f"{fn}(rdram, {a}){close}")
    if o in ("lwl", "lwr"):
        return setr(rt, f"op_{o}(rdram, {a}, {R(rt)})")
    if o in ("ldl", "ldr"):
        return f'recomp_unhandled({i.addr:#x}u, "{o}");'
    stores = {"sb": ("mem_w8", "(uint8_t)"), "sh": ("mem_w16", "(uint16_t)"),
              "sw": ("mem_w32", "(uint32_t)"), "sd": ("mem_w64", "")}
    if o in stores:
        fn, cast = stores[o]
        return f"{fn}(rdram, {a}, {cast}{R(rt)});"
    if o == "sc":
        return f"mem_w32(rdram, {a}, (uint32_t){R(rt)}); {setr(rt, '1')}"
    if o in ("swl", "swr"):
        return f"op_{o}(rdram, {a}, {R(rt)});"
    if o in ("sdl", "sdr", "scd", "lld"):
        return f'recomp_unhandled({i.addr:#x}u, "{o}");'
    if o == "lwc1":
        return f"ctx->f[{rt}] = mem_r32(rdram, {a});"
    if o == "swc1":
        return f"mem_w32(rdram, {a}, ctx->f[{rt}]);"
    if o == "ldc1":
        return f"fl_set(ctx, {rt}, mem_r64(rdram, {a}));"
    if o == "sdc1":
        return f"mem_w64(rdram, {a}, fl_get(ctx, {rt}));"
    if o in ("mfc0", "dmfc0"):
        return setr(rt, f"SX32(recomp_cop0_read(ctx, {rd}))")
    if o in ("mtc0", "dmtc0"):
        return f"recomp_cop0_write(ctx, {rd}, {U32(rt)});"
    traps = {"teq": "==", "tne": "!=", "tge": ">=", "tlt": "<"}
    if o in traps:
        return f'if ((int64_t){R(rs)} {traps[o]} (int64_t){R(rt)}) recomp_unhandled({i.addr:#x}u, "trap {o}");'
    if o in ("tgeu", "tltu"):
        return f'if ({R(rs)} {">=" if o == "tgeu" else "<"} {R(rt)}) recomp_unhandled({i.addr:#x}u, "trap {o}");'
    return f'recomp_unhandled({i.addr:#x}u, "{o}");'


BRANCH_BASE = {"b": "beq", "beq": "beq", "beql": "beq", "bne": "bne", "bnel": "bne",
               "blez": "blez", "blezl": "blez", "bgtz": "bgtz", "bgtzl": "bgtz",
               "bltz": "bltz", "bltzl": "bltz", "bltzal": "bltz", "bltzall": "bltz",
               "bgez": "bgez", "bgezl": "bgez", "bgezal": "bgez", "bgezall": "bgez",
               "bc1t": "bc1t", "bc1tl": "bc1t", "bc1f": "bc1f", "bc1fl": "bc1f"}


def branch_cond(i):
    o = BRANCH_BASE[i.op]
    if o == "beq":
        return f"{R(i.rs)} == {R(i.rt)}"
    if o == "bne":
        return f"{R(i.rs)} != {R(i.rt)}"
    if o == "blez":
        return f"(int64_t){R(i.rs)} <= 0"
    if o == "bgtz":
        return f"(int64_t){R(i.rs)} > 0"
    if o == "bltz":
        return f"(int64_t){R(i.rs)} < 0"
    if o == "bgez":
        return f"(int64_t){R(i.rs)} >= 0"
    if o == "bc1t":
        return "FCOND(ctx)"
    if o == "bc1f":
        return "!FCOND(ctx)"
    raise ValueError(i.op)


class FuncGen:
    def __init__(self, f, insns, known):
        self.f = f
        self.start = int(f["addr"], 16)
        self.end = self.start + int(f["size"], 16)
        self.insns = insns            # addr -> Insn for this function
        self.known = known            # addr -> function name
        self.tables = {int(t["jr"], 16): [int(x, 16) for x in t["targets"]] for t in f["jump_tables"]}
        self.labels = set()
        self.delay_labels = set()

    def inside(self, a):
        return self.start <= a < self.end

    def call(self, target):
        if target in self.known:
            return f"{self.known[target]}(rdram, ctx);"
        return f"recomp_lookup({target:#x}u)(rdram, ctx);"

    def collect_labels(self):
        for a, i in self.insns.items():
            if i.target is not None and i.kind in ("branch", "jump") and not i.link and self.inside(i.target):
                self.labels.add(i.target)
        for targets in self.tables.values():
            self.labels.update(targets)
        for a, i in self.insns.items():
            if i.has_delay_slot and (a + 4) in self.labels:
                self.delay_labels.add(a + 4)
                self.labels.add(a + 8)

    def emit(self):
        self.collect_labels()
        out = [f"void {self.known[self.start]}(uint8_t *rdram, Ctx *ctx) {{"]
        a = self.start
        while a < self.end:
            i = self.insns[a]
            if a in self.labels and a not in self.delay_labels:
                out.append(f"L_{a:08X}:;")
            if i.has_delay_slot:
                d = self.insns.get(a + 4)
                ds = plain(d) if d is not None else ""
                out.append(f"    // {a:08x}: {mips.fmt_insn(i)} / {mips.fmt_insn(d) if d else ''}")
                out.append(self.control(i, ds))
                if (a + 4) in self.delay_labels:
                    out.append(f"    goto L_{a + 8:08X};")
                    out.append(f"L_{a + 4:08X}:;\n    {ds}")
                a += 8
                continue
            if i.op == "invalid":
                out.append(f'    recomp_unhandled({a:#x}u, "invalid instruction {i.word:08x}");')
            else:
                s = plain(i)
                if s:
                    out.append(f"    {s} // {a:08x}: {mips.fmt_insn(i)}")
            a += 4
        # Falling off the end continues into whatever code follows.
        out.append(f"    {self.call(self.end)} return;")
        out.append("}")
        return "\n".join(out)

    def goto_or_tail(self, target):
        if self.inside(target) and target != self.start:
            return f"goto L_{target:08X};"
        if target == self.start:
            return f"goto L_{target:08X};"
        return f"{{ {self.call(target)} return; }}"

    def control(self, i, ds):
        a = i.addr
        # The link register is written before the delay slot runs, as on the R4300.
        if i.kind == "branch" and i.link:
            link = f"ctx->r[31] = SX32({a + 8:#x}u);"
            if i.likely:
                return f"    {{ int c_ = {branch_cond(i)}; {link} if (c_) {{ {ds} {self.call(i.target)} }} }}"
            return f"    {{ int c_ = {branch_cond(i)}; {link} {ds} if (c_) {{ {self.call(i.target)} }} }}"
        if i.kind == "branch":
            if i.op == "b":
                return f"    {ds} {self.goto_or_tail(i.target)}"
            if i.likely:
                return f"    if ({branch_cond(i)}) {{ {ds} {self.goto_or_tail(i.target)} }}"
            return f"    {{ int c_ = {branch_cond(i)}; {ds} if (c_) {self.goto_or_tail(i.target)} }}"
        if i.op == "j":
            return f"    {ds} {self.goto_or_tail(i.target)}"
        if i.op == "jal":
            return f"    ctx->r[31] = SX32({a + 8:#x}u); {ds} {self.call(i.target)}"
        if i.op == "jalr":
            return (f"    {{ uint32_t t_ = {U32(i.rs)}; {setr(i.rd, f'SX32({a + 8:#x}u)')} {ds} "
                    f"recomp_lookup(t_)(rdram, ctx); }}")
        if i.op == "jr":
            if i.rs == 31:
                return f"    {ds} return;"
            if a in self.tables:
                cases = " ".join(f"case {t:#x}u: goto L_{t:08X};" for t in sorted(set(self.tables[a])))
                return (f"    {{ uint32_t t_ = {U32(i.rs)}; {ds} switch (t_) {{ {cases} "
                        f'default: recomp_unhandled({a:#x}u, "jump table"); return; }} }}')
            const = self.register_constant(a, i.rs)
            if const is not None:
                # jr to an address built with lui/addiu just before (the entry stub).
                return f"    {ds} {{ {self.call(const)} return; }}"
            # jr to a saved return address (bal subroutines keep ra in a callee-saved register;
            # libultra's exception return): since every call is a C call, this is a return.
            return f"    {ds} return; /* jr {mips.GPR[i.rs]} */"
        raise ValueError(i.op)

    def register_constant(self, at, reg):
        lo = None
        for back in range(1, 10):
            i = self.insns.get(at - back * 4)
            if i is None or i.kind in ("branch", "jump"):
                return None
            if i.op in ("addiu", "ori") and i.rt == reg and i.rs == reg and lo is None:
                lo = i
                continue
            if i.op == "lui" and i.rt == reg:
                base = i.uimm << 16
                if lo is None:
                    return base
                return (base + lo.imm) & 0xFFFFFFFF if lo.op == "addiu" else base | lo.uimm
            if i.rt == reg or i.rd == reg:
                return None
        return None


def main():
    rom = open(sys.argv[1], "rb").read()
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    regions = json.load(open(os.path.join(ROOT, "analysis", "rom_map.json"), encoding="utf-8"))["regions"]
    segs = [(int(r["rom_start"], 16), int(r["rom_end"], 16), int(r["vram"], 16)) for r in regions if r["kind"] == "code"]
    funcs = json.load(open(os.path.join(ROOT, "analysis", "functions.json"), encoding="utf-8"))["functions"]

    def word(a):
        for rs, re_, va in segs:
            if va <= a < va + (re_ - rs):
                return struct.unpack_from(">I", rom, rs + a - va)[0]
        return None

    known = {int(f["addr"], 16): f"func_{int(f['addr'], 16):08X}" for f in funcs}
    bodies = []
    for f in funcs:
        start = int(f["addr"], 16)
        end = start + int(f["size"], 16) + 4  # +4: a final delay slot may sit past the size
        insns = {a: mips.decode(word(a), a) for a in range(start, end, 4) if word(a) is not None}
        bodies.append(FuncGen(f, insns, known).emit())

    hdr = ["// Generated by tools/recomp/recomp.py -- do not edit.", "#ifndef FUNCS_H", "#define FUNCS_H",
           '#include "recomp.h"']
    hdr += [f"void {n}(uint8_t *rdram, Ctx *ctx);" for n in known.values()]
    hdr += ["#endif"]
    open(os.path.join(out_dir, "funcs.h"), "w").write("\n".join(hdr) + "\n")

    for k in range(0, len(bodies), FUNCS_PER_FILE):
        with open(os.path.join(out_dir, f"funcs_{k // FUNCS_PER_FILE:02d}.c"), "w") as fh:
            fh.write('// Generated by tools/recomp/recomp.py -- do not edit.\n#include <math.h>\n#include "funcs.h"\n\n')
            fh.write("\n\n".join(bodies[k:k + FUNCS_PER_FILE]) + "\n")

    with open(os.path.join(out_dir, "lookup.c"), "w") as fh:
        fh.write('// Generated by tools/recomp/recomp.py -- do not edit.\n#include "funcs.h"\n\n')
        fh.write("const struct { uint32_t addr; RecompFunc fn; } recomp_func_table[] = {\n")
        for a in sorted(known):
            fh.write(f"    {{{a:#x}u, {known[a]}}},\n")
        fh.write("};\n")
        fh.write(f"const unsigned recomp_func_count = {len(known)};\n")
    n_files = (len(bodies) + FUNCS_PER_FILE - 1) // FUNCS_PER_FILE
    print(f"{len(bodies)} funcoes em {n_files} arquivos")


if __name__ == "__main__":
    main()
