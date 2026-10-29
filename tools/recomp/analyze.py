"""Static analysis of the ROM's code segments: function discovery, extents and indirect jumps.

Seeds: segment entry points, every jal target, and code addresses materialised with
lui + addiu/ori (function pointers passed to osCreateThread, stored in tables, etc.).
Each function is explored by following its branches; it ends after the last instruction
reached (plus delay slot). Jump tables behind `jr` are resolved from the lui/addiu + lw
pattern that loads them, and their targets become part of the owning function.

Usage: python -I tools/recomp/analyze.py <rom.z64>   -> writes analysis/functions.json
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mips  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
MAP = os.path.join(ROOT, "analysis", "rom_map.json")
OUT = os.path.join(ROOT, "analysis", "functions.json")


class Segment:
    def __init__(self, name, rom, rom_start, rom_end, vram):
        self.name, self.vram = name, vram
        self.size = rom_end - rom_start
        self.words = struct.unpack_from(f">{self.size // 4}I", rom, rom_start)
        self.insns = [mips.decode(w, vram + k * 4) for k, w in enumerate(self.words)]

    def contains(self, a):
        return self.vram <= a < self.vram + self.size

    def insn(self, a):
        return self.insns[(a - self.vram) // 4]

    def word(self, a):
        return self.words[(a - self.vram) // 4]


def load_segments(rom):
    regions = json.load(open(MAP, encoding="utf-8"))["regions"]
    segs = []
    for r in regions:
        if r["kind"] == "code":
            segs.append(Segment(r["name"], rom, int(r["rom_start"], 16), int(r["rom_end"], 16),
                                int(r["vram"], 16)))
    return segs


def seg_of(segs, a):
    for s in segs:
        if s.contains(a):
            return s
    return None


def find_constants(seg):
    """Yield (addr_of_use, value) for lui/addiu|ori pairs in straight-line code."""
    hi = {}
    for i in seg.insns:
        if i.op == "lui":
            hi[i.rt] = (i.uimm << 16, i.addr)
            continue
        if i.op in ("addiu", "ori") and i.rs in hi:
            base, at = hi[i.rs]
            if i.addr - at <= 0x40:
                val = (base + i.imm) & 0xFFFFFFFF if i.op == "addiu" else base | i.uimm
                yield i.addr, val
        if i.kind in ("branch", "jump") and i.op not in ("jal",):
            pass  # keep tracking across branches: delay-slot pairs are common
        if hasattr(i, "rt") and i.op not in ("lui",) and i.kind in ("alu", "load") and i.rt in hi and i.op != "addiu":
            hi.pop(i.rt, None)


def resolve_jump_table(seg, jr_insn, segs):
    """Find `lw rX, lo(rY)` where rY = lui hi + index, feeding `jr rX`; return table address."""
    a = jr_insn.addr
    reg = jr_insn.rs
    lo = None
    for back in range(1, 12):
        p = a - back * 4
        if not seg.contains(p):
            break
        i = seg.insn(p)
        if i.op == "lw" and i.rt == reg and lo is None:
            lo, base_reg = i.imm, i.rs
            continue
        if lo is not None and i.op == "addu" and i.rd == base_reg:
            # table index added to the lui register: pick whichever operand has a lui
            for cand in (i.rs, i.rt):
                for back2 in range(1, 12):
                    q = p - back2 * 4
                    if not seg.contains(q):
                        break
                    j = seg.insn(q)
                    if j.op == "lui" and j.rt == cand:
                        return ((j.uimm << 16) + lo) & 0xFFFFFFFF
        if lo is not None and i.op == "lui" and i.rt == base_reg:
            return ((i.uimm << 16) + lo) & 0xFFFFFFFF
    return None


def switch_bound(seg, jr_addr, start):
    """Entry count N from the `sltiu at, idx, N` + branch-to-default guarding a switch, if any."""
    for back in range(1, 24):
        p = jr_addr - back * 4
        if p < start or not seg.contains(p):
            break
        i = seg.insn(p)
        if i.op == "sltiu" and i.imm > 0:
            return i.imm
    return None


def read_table(segs, table, owner_seg, start, limit, count=None):
    """Read a jump table: `count` entries when the switch bound is known, otherwise the
    consecutive words that point inside [start, limit)."""
    s = seg_of(segs, table)
    out = []
    a = table
    while s and s.contains(a) and (count is None or len(out) < count):
        w = s.word(a)
        if not (start <= w < limit) or not owner_seg.contains(w):
            break
        out.append(w)
        a += 4
    return out


def register_constant(seg, at, reg):
    """Value of `reg` at `at` if it was just built with lui (+ addiu/ori) in straight-line code."""
    lo = None
    for back in range(1, 10):
        p = at - back * 4
        if not seg.contains(p):
            return None
        i = seg.insn(p)
        if i.kind in ("branch", "jump"):
            return None
        if i.op in ("addiu", "ori") and i.rt == reg and i.rs == reg and lo is None:
            lo = i
            continue
        if i.op == "lui" and i.rt == reg:
            base = i.uimm << 16
            if lo is None:
                return base
            return (base + lo.imm) & 0xFFFFFFFF if lo.op == "addiu" else base | lo.uimm
        if getattr(i, "rt", None) == reg or getattr(i, "rd", None) == reg and i.kind == "alu":
            return None
    return None


def looks_like_function_start(seg, a):
    """A label right after `jr ra` + delay slot that opens a stack frame (addiu sp, sp, -N).

    Mid-function labels can also follow a `jr ra` (early returns), but they do not allocate a frame.
    """
    if not seg.contains(a):
        return False
    i = seg.insn(a)
    return i.op == "addiu" and i.rt == 29 and i.rs == 29 and i.imm < 0 and follows_return(seg, a)


def is_epilogue_start(i):
    """Restoring registers from the stack, popping the frame or returning: tail of a function."""
    if i.kind == "load" and i.rs == 29:
        return True
    if i.op == "addiu" and i.rt == 29 and i.rs == 29 and i.imm > 0:
        return True
    return i.op == "jr" and i.rs == 31


def follows_return(seg, a):
    """True if a sits right after `jr ra` + delay slot (allowing nop padding)."""
    p = a - 4
    while seg.contains(p) and seg.insn(p).op == "nop":
        p -= 4
    for q in (p - 4, p):
        if seg.contains(q):
            i = seg.insn(q)
            if i.op == "jr" and i.rs == 31:
                return True
    return False


def explore(seg, segs, start, limit):
    """Follow control flow from start.

    Returns (end, jump_tables, unresolved_jr, calls, new_seeds) where new_seeds maps addresses
    that must be functions of their own: bal targets, jr-to-constant targets and tail calls.
    """
    seen = set()
    todo = [start]
    end = start
    tables = []
    unresolved = []
    calls = set()
    new_seeds = {}
    while todo:
        a = todo.pop()
        while seg.contains(a) and a < limit and a not in seen:
            seen.add(a)
            i = seg.insn(a)
            end = max(end, a + 4)
            if i.op == "invalid":
                break
            if i.op == "jal":
                calls.add(i.target)
            if i.kind == "branch" and i.link:
                # bal/bgezal: a call to a local subroutine, which returns here.
                end = max(end, a + 8)
                calls.add(i.target)
                new_seeds[i.target] = "bal"
            elif i.kind == "branch":
                end = max(end, a + 8)
                if start <= i.target < limit:
                    todo.append(i.target)
                if i.op == "b":
                    seen.add(a + 4)
                    if start <= i.target < limit and not (i.target != start and looks_like_function_start(seg, i.target)):
                        a = i.target
                        continue
                    if i.target != start:
                        new_seeds[i.target] = "tailcall"
                    break
            elif i.op == "j":
                end = max(end, a + 8)
                seen.add(a + 4)
                if start <= i.target < limit and not looks_like_function_start(seg, i.target):
                    a = i.target
                    continue
                if seg_of(segs, i.target):
                    new_seeds[i.target] = "tailcall"
                break
            elif i.op == "jr":
                end = max(end, a + 8)
                seen.add(a + 4)
                if i.rs != 31:
                    t = resolve_jump_table(seg, i, segs)
                    targets = read_table(segs, t, seg, start, limit, switch_bound(seg, a, start)) if t else []
                    const = None if targets else register_constant(seg, a, i.rs)
                    if targets:
                        tables.append({"jr": hex(a), "table": hex(t), "targets": [hex(x) for x in targets]})
                        todo.extend(targets)
                    elif const is not None and seg_of(segs, const):
                        new_seeds[const] = "jr-constant"
                    else:
                        unresolved.append(hex(a))
                break
            a += 4
    return end, tables, unresolved, calls, new_seeds


def code_end(seg, segs, start):
    """Extend text past `start` while words decode, up to the last `jr ra` + delay slot.

    Stops at a j/jal whose target is outside every CPU segment: that is RSP microcode
    (targets in SP IMEM, 0x84001xxx) stored after the CPU text, not code for this recompiler.
    """
    last = None
    a = start
    while seg.contains(a) and seg.insn(a).op != "invalid":
        i = seg.insn(a)
        if i.op in ("j", "jal") and not seg_of(segs, i.target):
            break
        if i.op == "jr" and i.rs == 31:
            last = a + 8
        a += 4
    return max(start, ((last or start) + 15) & ~15)


def main():
    rom = open(sys.argv[1], "rb").read()
    segs = load_segments(rom)

    seeds = {s.vram: "segment entry" for s in segs}
    for s in segs:
        for i in s.insns:
            if i.op == "jal" and seg_of(segs, i.target) and seg_of(segs, i.target).insn(i.target).op != "invalid":
                seeds.setdefault(i.target, "jal")

    # Each segment is text followed by data; text ends where the last jal-reached function ends,
    # extended over any following words that still decode up to a final `jr ra`.
    for s in segs:
        starts = sorted(a for a in seeds if s.contains(a))
        ends = [explore(s, segs, a, (starts[k + 1] if k + 1 < len(starts) else s.vram + s.size))[0]
                for k, a in enumerate(starts)]
        s.text_end = code_end(s, segs, (max(ends) + 15) & ~15)

    # Data after the text can decode as jal (e.g. the word at 0x800e5530 reads as
    # "jal 0x80002000", which is mid-function): keep only calls made from inside the text.
    seeds = {s.vram: "segment entry" for s in segs}
    for s in segs:
        for i in s.insns:
            if i.addr >= s.text_end:
                break
            if i.op == "jal" and seg_of(segs, i.target) and seg_of(segs, i.target).insn(i.target).op != "invalid":
                seeds.setdefault(i.target, "jal")

    for s in segs:
        for use, val in find_constants(s):
            t = seg_of(segs, val)
            if t and val % 4 == 0 and val < t.text_end and t.insn(val).op != "invalid":
                prev = t.insn(val - 4) if t.contains(val - 4) else None
                prev2 = t.insn(val - 8) if t.contains(val - 8) else None
                # Only accept addresses that look like a function start: right after a
                # `jr ra` + delay slot, or after nop padding.
                if prev is None or prev.op == "nop" and (prev2 is None or prev2.op in ("nop", "jr")) or \
                        (prev2 is not None and prev2.op == "jr" and prev2.rs == 31):
                    seeds.setdefault(val, "pointer")

    branch_targets = set()
    for s in segs:
        for i in s.insns:
            if i.kind in ("branch", "jump") and i.target is not None:
                branch_targets.add(i.target)

    funcs = []
    for s in segs:
        dead = set()
        # Repeat until stable and the whole text is covered: seeds discovered while exploring
        # (bal targets, tail calls, jr to constants) split functions, and code nobody references
        # (dead code, or reached through tables we do not parse) becomes "unreferenced".
        while True:
            starts = sorted(a for a in seeds if s.contains(a))
            extents = []
            found = {}
            for k, a in enumerate(starts):
                limit = starts[k + 1] if k + 1 < len(starts) else s.text_end
                r = explore(s, segs, a, limit)
                extents.append((a, r))
                found.update({t: why for t, why in r[4].items() if t not in seeds})
            covered = set()
            for a, (end, *_rest) in extents:
                covered.update(range(a, end, 4))
            # Uncovered code is split into pieces at nops and after each `jr ra` + delay slot.
            # A piece that starts by restoring the frame (epilogue after an infinite loop, a
            # duplicate return) is dead code of the function before it; any other piece is an
            # unreferenced function of its own.
            # Only the first new function per gap is taken in each pass: exploring it first lets
            # its own switch cases and early returns be covered before the rest of the gap is cut.
            piece_start = True
            gap_has_seed = False
            for a in range(s.vram, s.text_end, 4):
                i = s.insn(a)
                if a in covered:
                    piece_start = True
                    gap_has_seed = False
                    continue
                if i.op in ("nop", "invalid"):
                    piece_start = True
                    continue
                if piece_start:
                    if a in dead or (is_epilogue_start(i) and a not in branch_targets):
                        dead.add(a)
                    elif not gap_has_seed:
                        found.setdefault(a, "unreferenced")
                        gap_has_seed = True
                    piece_start = False
                if a - 4 >= s.vram and s.insn(a - 4).op == "jr" and s.insn(a - 4).rs == 31:
                    piece_start = True  # next instruction after this delay slot starts a new piece
            found = {t: w for t, w in found.items() if t not in seeds}
            if not found:
                break
            for t, why in found.items():
                if seg_of(segs, t) and t < seg_of(segs, t).text_end:
                    seeds[t] = why
            if all(not (seg_of(segs, t) and t < seg_of(segs, t).text_end) for t in found):
                break
        for k, (a, (end, tables, unresolved, calls, _new)) in enumerate(extents):
            # Absorb dead fragments (and nop padding) that sit between this function and the next.
            nxt = extents[k + 1][0] if k + 1 < len(extents) else s.text_end
            dead_tail = sorted(d for d in dead if end <= d < nxt)
            if dead_tail:
                # Extend through each dead piece up to its own `jr ra` + delay slot.
                for d in dead_tail:
                    p = d
                    while p < nxt and not (s.insn(p).op == "jr" and s.insn(p).rs == 31):
                        p += 4
                    end = max(end, min(p + 8, nxt))
            funcs.append({"addr": hex(a), "size": hex(end - a), "segment": s.name, "seed": seeds[a],
                          "dead_tail": bool(dead_tail), "jump_tables": tables, "unresolved_jr": unresolved,
                          "calls": sorted(hex(c) for c in calls)})

    lib = json.load(open(os.path.join(ROOT, "analysis", "libultra.json"), encoding="utf-8"))
    lo, hi = int(lib["block"]["start"], 16), int(lib["block"]["end"], 16)
    names = {int(x["addr"], 16): x["name"] for x in lib["identified"]}
    for f in funcs:
        a = int(f["addr"], 16)
        f["library"] = "libultra" if f["segment"] == "main" and lo <= a < hi else None
        if a in names:
            f["name"] = names[a]
    text = {s.name: {"vram": hex(s.vram), "text_end": hex(s.text_end)} for s in segs}
    json.dump({"segments": text, "functions": funcs}, open(OUT, "w", encoding="utf-8"), indent=1)
    for s in segs:
        print(f"{s.name}: texto {s.vram:#x}-{s.text_end:#x}")
    by_seed = {}
    for f in funcs:
        by_seed[f["seed"]] = by_seed.get(f["seed"], 0) + 1
    print(f"funcoes: {len(funcs)} {by_seed}")
    print(f"tabelas de salto resolvidas: {sum(len(f['jump_tables']) for f in funcs)}; "
          f"jr indiretos sem tabela: {sum(len(f['unresolved_jr']) for f in funcs)}")


if __name__ == "__main__":
    main()
