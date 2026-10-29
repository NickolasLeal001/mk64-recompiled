"""MIPS R4300i instruction decoder (big-endian words), as used by the N64 CPU.

decode(word, addr) returns an Insn with the raw fields plus:
  op      mnemonic in lower case (rizin/binutils spelling, e.g. "addiu", "c.lt.s", "bc1tl")
  kind    "branch" (conditional, has delay slot), "jump" (j/jal/jr/jalr), "likely" flag,
          "load"/"store"/"alu"/"fpu"/"cop0"/"misc"/"invalid"
  target  absolute destination for branches and j/jal
"""
from dataclasses import dataclass, field

GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


@dataclass
class Insn:
    addr: int
    word: int
    op: str
    kind: str = "alu"
    rs: int = 0
    rt: int = 0
    rd: int = 0
    sa: int = 0
    imm: int = 0          # sign-extended 16-bit immediate
    uimm: int = 0         # zero-extended 16-bit immediate
    target: int = None    # branch/jump destination
    fmt: str = None       # FPU format: s, d, w, l
    fs: int = 0
    ft: int = 0
    fd: int = 0
    likely: bool = False
    link: bool = False    # writes ra (jal, jalr, bgezal...)
    extra: dict = field(default_factory=dict)

    @property
    def has_delay_slot(self):
        return self.kind in ("branch", "jump")


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0C: "syscall", 0x0D: "break", 0x0F: "sync",
    0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo",
    0x14: "dsllv", 0x16: "dsrlv", 0x17: "dsrav",
    0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x1C: "dmult", 0x1D: "dmultu", 0x1E: "ddiv", 0x1F: "ddivu",
    0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor",
    0x2A: "slt", 0x2B: "sltu", 0x2C: "dadd", 0x2D: "daddu", 0x2E: "dsub", 0x2F: "dsubu",
    0x30: "tge", 0x31: "tgeu", 0x32: "tlt", 0x33: "tltu", 0x34: "teq", 0x36: "tne",
    0x38: "dsll", 0x3A: "dsrl", 0x3B: "dsra", 0x3C: "dsll32", 0x3E: "dsrl32", 0x3F: "dsra32",
}

REGIMM = {
    0x00: ("bltz", False, False), 0x01: ("bgez", False, False),
    0x02: ("bltzl", True, False), 0x03: ("bgezl", True, False),
    0x10: ("bltzal", False, True), 0x11: ("bgezal", False, True),
    0x12: ("bltzall", True, True), 0x13: ("bgezall", True, True),
}
REGIMM_TRAP = {0x08: "tgei", 0x09: "tgeiu", 0x0A: "tlti", 0x0B: "tltiu", 0x0C: "teqi", 0x0E: "tnei"}

IMM_ALU = {0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori",
           0x0E: "xori", 0x0F: "lui", 0x18: "daddi", 0x19: "daddiu"}
BRANCH = {0x04: ("beq", False), 0x05: ("bne", False), 0x06: ("blez", False), 0x07: ("bgtz", False),
          0x14: ("beql", True), 0x15: ("bnel", True), 0x16: ("blezl", True), 0x17: ("bgtzl", True)}
LOAD = {0x1A: "ldl", 0x1B: "ldr", 0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu",
        0x25: "lhu", 0x26: "lwr", 0x27: "lwu", 0x30: "ll", 0x34: "lld", 0x37: "ld",
        0x31: "lwc1", 0x35: "ldc1"}
STORE = {0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2C: "sdl", 0x2D: "sdr", 0x2E: "swr",
         0x38: "sc", 0x3C: "scd", 0x3F: "sd", 0x39: "swc1", 0x3D: "sdc1"}

FPU_FMT = {16: "s", 17: "d", 20: "w", 21: "l"}
FPU_OP = {0x00: "add", 0x01: "sub", 0x02: "mul", 0x03: "div", 0x04: "sqrt", 0x05: "abs", 0x06: "mov",
          0x07: "neg", 0x08: "round.l", 0x09: "trunc.l", 0x0A: "ceil.l", 0x0B: "floor.l",
          0x0C: "round.w", 0x0D: "trunc.w", 0x0E: "ceil.w", 0x0F: "floor.w",
          0x20: "cvt.s", 0x21: "cvt.d", 0x24: "cvt.w", 0x25: "cvt.l"}
FPU_COND = ["f", "un", "eq", "ueq", "olt", "ult", "ole", "ule",
            "sf", "ngle", "seq", "ngl", "lt", "nge", "le", "ngt"]


def decode(word, addr):
    """Decodes one instruction; words whose must-be-zero fields are set decode as "invalid"."""
    i = _decode(word, addr)
    if i.op != "invalid" and not _reserved_fields_clear(i, word):
        return Insn(addr, word, "invalid", "invalid")
    return i


_SHIFT_IMM = {"sll", "srl", "sra", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32"}
_MULDIV = {"mult", "multu", "div", "divu", "dmult", "dmultu", "ddiv", "ddivu"}


def _reserved_fields_clear(i, word):
    o = i.op
    opc = word >> 26
    if opc == 0x00:
        if o == "nop" or o in ("syscall", "break") or o.startswith("t"):
            return True
        if o == "sync":
            return (word & 0x03FFFFC0) == 0
        if o in _SHIFT_IMM:
            return i.rs == 0
        if o in ("mfhi", "mflo"):
            return i.rs == 0 and i.rt == 0 and i.sa == 0
        if o in ("mthi", "mtlo"):
            return i.rt == 0 and i.rd == 0 and i.sa == 0
        if o == "jr":
            return i.rt == 0 and i.rd == 0 and i.sa == 0
        if o == "jalr":
            return i.rt == 0 and i.sa == 0
        if o in _MULDIV:
            return i.rd == 0 and i.sa == 0
        return i.sa == 0
    if o == "lui":
        return i.rs == 0
    if o in ("blez", "bgtz", "blezl", "bgtzl"):
        return i.rt == 0
    if opc == 0x10 and not (i.rs & 0x10):
        return (word & 0x7FF) == 0
    if opc == 0x10:
        return (word & 0x01FFFFC0) == 0
    if opc == 0x11 and i.rs in (0, 1, 2, 4, 5, 6):
        return (word & 0x7FF) == 0
    if opc == 0x11 and o.startswith("c."):
        return i.fd == 0
    if opc == 0x11 and i.kind == "fpu" and o.split(".")[0] in ("sqrt", "abs", "mov", "neg") or (
            opc == 0x11 and (o.startswith("cvt.") or o[:5] in ("round", "trunc", "floor") or o[:4] == "ceil")):
        return i.ft == 0
    return True


def _decode(word, addr):
    opc = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    sa = (word >> 6) & 31
    funct = word & 63
    uimm = word & 0xFFFF
    imm = _s16(uimm)
    i = Insn(addr, word, "invalid", "invalid", rs, rt, rd, sa, imm, uimm)
    btarget = (addr + 4 + (imm << 2)) & 0xFFFFFFFF

    if opc == 0x00:
        name = SPECIAL.get(funct)
        if name is None:
            return i
        i.op = name
        if name == "sll" and word == 0:
            i.op, i.kind = "nop", "misc"
        elif name in ("jr", "jalr"):
            i.kind = "jump"
            i.link = name == "jalr"
        elif name in ("syscall", "break", "sync") or name.startswith("t"):
            i.kind = "misc"
        else:
            i.kind = "alu"
        return i
    if opc == 0x01:
        if rt in REGIMM:
            name, likely, link = REGIMM[rt]
            i.op, i.kind, i.likely, i.link, i.target = name, "branch", likely, link, btarget
        elif rt in REGIMM_TRAP:
            i.op, i.kind = REGIMM_TRAP[rt], "misc"
        return i
    if opc in (0x02, 0x03):
        i.op = "j" if opc == 2 else "jal"
        i.kind = "jump"
        i.link = opc == 3
        i.target = ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        return i
    if opc in BRANCH:
        name, likely = BRANCH[opc]
        i.op, i.kind, i.likely, i.target = name, "branch", likely, btarget
        if name == "beq" and rs == 0 and rt == 0:
            i.op = "b"
        return i
    if opc in IMM_ALU:
        i.op, i.kind = IMM_ALU[opc], "alu"
        return i
    if opc in LOAD:
        i.op, i.kind = LOAD[opc], "load"
        return i
    if opc in STORE:
        i.op, i.kind = STORE[opc], "store"
        return i
    if opc == 0x2F:
        i.op, i.kind = "cache", "misc"
        return i
    if opc == 0x10:  # COP0
        if rs & 0x10:
            names = {0x01: "tlbr", 0x02: "tlbwi", 0x06: "tlbwr", 0x08: "tlbp", 0x18: "eret"}
            if funct in names:
                i.op, i.kind = names[funct], "cop0"
        elif rs in (0, 1, 4, 5):
            i.op, i.kind = {0: "mfc0", 1: "dmfc0", 4: "mtc0", 5: "dmtc0"}[rs], "cop0"
        return i
    if opc == 0x11:  # COP1
        i.fs, i.ft, i.fd = rd, rt, sa
        if rs in (0, 1, 2, 4, 5, 6):
            i.op, i.kind = {0: "mfc1", 1: "dmfc1", 2: "cfc1", 4: "mtc1", 5: "dmtc1", 6: "ctc1"}[rs], "fpu"
        elif rs == 8:
            if rt > 3:
                return i
            i.op = ("bc1f", "bc1t", "bc1fl", "bc1tl")[rt]
            i.kind, i.likely, i.target = "branch", rt >= 2, btarget
        elif rs in FPU_FMT:
            fmt = FPU_FMT[rs]
            if funct >= 0x30 and fmt in ("s", "d"):
                i.op = f"c.{FPU_COND[funct & 15]}.{fmt}"
            elif funct in FPU_OP:
                name = FPU_OP[funct]
                if name.startswith("cvt.") and name[4:] == fmt:
                    return i
                if fmt in ("w", "l") and not name.startswith("cvt."):
                    return i
                i.op = f"{name}.{fmt}"
            else:
                return i
            i.kind, i.fmt = "fpu", fmt
        return i
    return i


def fmt_insn(i):
    """Short disassembly text, close to rizin's, used for debugging and cross-checks."""
    g = GPR
    o = i.op
    if o in ("invalid", "nop", "syscall", "break", "sync", "eret", "tlbr", "tlbwi", "tlbwr", "tlbp"):
        return o
    if o in ("j", "jal", "b"):
        return f"{o} 0x{i.target:x}"
    if o in ("jr",):
        return f"jr {g[i.rs]}"
    if o == "jalr":
        return f"jalr {g[i.rs]}" if i.rd == 31 else f"jalr {g[i.rd]}, {g[i.rs]}"
    if i.kind == "branch":
        if o.startswith("bc1"):
            return f"{o} 0x{i.target:x}"
        if o in ("beq", "bne", "beql", "bnel"):
            return f"{o} {g[i.rs]}, {g[i.rt]}, 0x{i.target:x}"
        return f"{o} {g[i.rs]}, 0x{i.target:x}"
    if i.kind in ("load", "store"):
        reg = f"f{i.rt}" if o.endswith("c1") else g[i.rt]
        return f"{o} {reg}, {i.imm}({g[i.rs]})"
    if o == "lui":
        return f"lui {g[i.rt]}, 0x{i.uimm:x}"
    if o in ("andi", "ori", "xori"):
        return f"{o} {g[i.rt]}, {g[i.rs]}, 0x{i.uimm:x}"
    if o in IMM_ALU.values():
        return f"{o} {g[i.rt]}, {g[i.rs]}, {i.imm}"
    if o in ("sll", "srl", "sra", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32"):
        return f"{o} {g[i.rd]}, {g[i.rt]}, {i.sa}"
    if o in ("mfhi", "mflo"):
        return f"{o} {g[i.rd]}"
    if o in ("mthi", "mtlo"):
        return f"{o} {g[i.rs]}"
    if o in ("mult", "multu", "div", "divu", "dmult", "dmultu", "ddiv", "ddivu") or (
            i.kind == "misc" and o.startswith("t")):
        return f"{o} {g[i.rs]}, {g[i.rt]}"
    if o in ("mfc1", "mtc1", "dmfc1", "dmtc1", "cfc1", "ctc1"):
        return f"{o} {g[i.rt]}, f{i.fs}"
    if o in ("mfc0", "mtc0", "dmfc0", "dmtc0"):
        return f"{o} {g[i.rt]}, {i.rd}"
    if o == "cache":
        return f"cache 0x{i.rt:x}, {i.imm}({g[i.rs]})"
    if i.kind == "fpu":
        if o.startswith("c."):
            return f"{o} f{i.fs}, f{i.ft}"
        if o.split(".")[0] in ("add", "sub", "mul", "div"):
            return f"{o} f{i.fd}, f{i.fs}, f{i.ft}"
        return f"{o} f{i.fd}, f{i.fs}"
    if o in ("sllv", "srlv", "srav", "dsllv", "dsrlv", "dsrav"):
        return f"{o} {g[i.rd]}, {g[i.rt]}, {g[i.rs]}"
    return f"{o} {g[i.rd]}, {g[i.rs]}, {g[i.rt]}"
