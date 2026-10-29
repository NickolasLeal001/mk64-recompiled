"""Builds analysis/rom_map.json from the ROM plus the facts established in analysis.

Each region records where the fact came from. Ranges not yet explained stay "unknown",
and the coverage report at the end says how much of the ROM is still unexplained.

Usage: python -I tools/build_rom_map.py <rom.z64>
"""
import itertools
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mio0  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "analysis", "rom_map.json")
SHA1 = "579c48e211ae952530ffc8738709f078d5dd215e"

RACING_ROM, RACING_VRAM = 0xF7510, 0x8028DF00
COURSE_TABLE_VRAM = 0x802B8D80  # read by FUN_802aa918 (course load) with stride 0x30
COURSE_COUNT = 20

# Code and fixed-size loads, each one seen as a literal call in the pseudocode.
FIXED = [
    ("header", "header", 0x0, 0x40, None, None, "N64 header: MARIOKART64 / NKTE"),
    ("ipl3", "boot", 0x40, 0x1000, None, None, "IPL3 boot code, present in every retail ROM"),
    ("main", "code", 0x1000, 0xF7510, 0x80000400, 0x1974D0,
     "entry 0x80000400 clears BSS 0x800f6910+0xa0fc0 (rizin ev_1e55e6d2)"),
    ("racing", "code", 0xF7510, 0x123640, 0x8028DF00, 0x2C470,
     "FUN_800010cc: FUN_80001158(0x8028df00, 0xf7510, 0x2c130)"),
    ("seg3", "code", 0x123640, 0x12AAE0, 0x80280000, 0x74A0,
     "FUN_8000105c: FUN_80001158(0x80280000, 0x123640, 0x74a0)"),
    ("common_data", "data", 0x12AAE0, 0x132B50, None, None,
     "FUN_80001264: FUN_802a7d70(0x12aae0, 0x132b50) -> heap, registered as segment"),
    ("startup_mio0", "mio0", 0x132B50, 0x145470, 0x8027B5E0, None,
     "FUN_80001264: DMA 0x132b50 -> 0x8027b5e0 size 0x12920; MIO0 header at 0x132b50"),
    ("fixed_0x724220", "data", 0x724220, 0x729A30, 0x802BA370, 0x5810,
     "FUN_80001264: DMA 0x724220 -> 0x802ba370 size 0x5810"),
]

# Audio tables loaded by FUN_800baa80 from literal ROM addresses: u16 type, u16 count,
# then count x (u32 offset, u32 size) relative to the table start. Extents come from the entries.
AUDIO_TABLES = [
    ("audio_banks", 0x966260, 1, "FUN_800baa80(0x966260, ...) / FUN_800bb43c(_DAT_803b7070, 0x966260)"),
    ("audio_samples", 0x979AA0, 2, "FUN_800baa80(0x979aa0, ...) / FUN_800bb43c(_DAT_803b7074, 0x979aa0)"),
    ("audio_sequences", 0xBC5F60, 3, "FUN_800baa80(0xbc5f60, ...) / FUN_800bb43c(_DAT_803b706c, 0xbc5f60)"),
]
AUDIO_SEQ_BANK_MAP = 0xBE90E0  # FUN_800baa80(0xbe90e0, ..., 0x100): u16 offsets per sequence, then bank lists


def audio_regions(rom):
    regions = []
    for name, base, kind, ev in AUDIO_TABLES:
        t, n = struct.unpack_from(">HH", rom, base)
        if t != kind:
            sys.exit(f"{name}: expected table type {kind}, found {t}")
        ents = [struct.unpack_from(">II", rom, base + 4 + i * 8) for i in range(n)]
        end = base + max(o + s for o, s in ents)
        regions.append({"name": name, "kind": "audio", "rom_start": hex(base), "rom_end": hex(end),
                        "entries": n, "evidence": ev})
    # Map: one u16 offset per sequence, each pointing at [count, bank...] bytes.
    seqs = regions[2]["entries"]
    offs = struct.unpack_from(f">{seqs}H", rom, AUDIO_SEQ_BANK_MAP)
    end = AUDIO_SEQ_BANK_MAP + max(o + 1 + rom[AUDIO_SEQ_BANK_MAP + o] for o in offs)
    regions.append({"name": "audio_seq_bank_map", "kind": "audio", "rom_start": hex(AUDIO_SEQ_BANK_MAP),
                    "rom_end": hex(end), "evidence": "FUN_800baa80(0xbe90e0, ..., 0x100)"})
    return regions

# Bases added to (segmented address & 0xffffff) before DMA. Each bank ends where the next
# region proven by a literal in the code begins; "index" lists the pointer tables found into it.
BANKS = [
    ("bank_0x145470", 0x145470, "FUN_80027040/80027560/80027a20/80004ef0: (addr & 0xffffff) + 0x145470; "
     "indexed by player character (+0x254)",
     [{"vram": "0x800ddec0", "where": "main", "entries": 3959, "what": "kart frames (MIO0), segment 0x0f"},
      {"vram": "0x800e1f20", "where": "main", "entries": 256, "what": "MIO0 frames, segment 0x0f"},
      {"vram": "0x800e2340", "where": "main", "entries": 8,
       "what": "per-character palette (FUN_80027a20 reads 0x200 bytes); each lies in that character's raw block"},
      {"vram": "0x800ddeb0", "where": "main", "entries": 8, "what": "per-character u16 frame DMA size"}]),
    ("bank_0x641f70", 0x641F70, "FUN_8006ed94/802a84f4/802a85cc: (addr & 0xffffff) + 0x641f70, "
     "decompressed into the heap",
     [{"vram": "0x800e54d0", "where": "main", "entries": 20,
       "what": "one MIO0 per course, sizes at 0x800e5520 (FUN_8006ef60 -> FUN_8006ed94)"},
      {"where": "course seg9", "what": "per-course texture lists, 16-byte entries "
       "{addr, comp size, size, ?}; 280 distinct textures, all match MIO0 blocks (FUN_802aa7c8)"},
      {"where": "code", "what": "123 literal FUN_802a84f4 calls + FUN_8006ed94(0x0f0d7510, ...); "
       "401 of 418 MIO0 blocks referenced in total"}]),
    ("bank_0x729a30", 0x729A30, "FUN_80099000/80099aec/80099ec4: (addr & 0xffffff) + 0x729a30",
     [{"rom": "0x12aae4", "where": "common_data", "stride": "0x28",
       "what": "menu/HUD texture descriptors; reference 324 of the 325 MIO0 blocks"}]),
    ("bank_0x7fa3c0", 0x7FA3C0, "FUN_80099088: (addr & 0xffffff) + 0x7fa3c0", []),
]


# Course table entry (0x30 bytes), as consumed by FUN_802aa918:
#  +0x00/+0x04 ROM range, MIO0, FUN_802aa88c decompresses it -> segment 6 (course data)
#  +0x08/+0x0C ROM range, raw DMA by FUN_802a82e4 -> segment 0xF (vertices + packed display lists)
#  +0x10/+0x14 ROM range, raw DMA by FUN_802a7d70 -> segment 9 (texture list)
#  +0x18 seg addr / +0x1C count: vertices in seg F, expanded by FUN_802a87a8 -> segment 4
#  +0x20 seg addr / +0x24 size / +0x2C u16: packed display lists in seg F, FUN_802a9af4 -> segment 7
#  +0x28 seg addr: texture list in seg 9, loaded from bank 0x641f70 by FUN_802aa7c8 -> segment 5
COURSE_RANGES = (("data", 0x0, 6), ("vtx_dl", 0x8, 0xF), ("textures", 0x10, 9))


def course_regions(rom):
    base = RACING_ROM + (COURSE_TABLE_VRAM - RACING_VRAM)
    regions = []
    for i in range(COURSE_COUNT):
        e = struct.unpack_from(">12I", rom, base + i * 0x30)
        src = f"course table 0x{COURSE_TABLE_VRAM:08x}[{i}] (FUN_802aa918)"
        for name, field, seg in COURSE_RANGES:
            s, t = e[field // 4], e[field // 4 + 1]
            if s and t > s:
                r = {"name": f"course{i:02d}_{name}", "kind": "course", "rom_start": hex(s), "rom_end": hex(t),
                     "segment": seg, "evidence": f"{src} +{field:#x}"}
                if name == "vtx_dl":
                    r["vertices"] = {"seg_addr": hex(e[6]), "count": e[7]}
                    r["display_lists"] = {"seg_addr": hex(e[8]), "size": hex(e[9]), "flag": hex(e[11] >> 16)}
                if name == "textures":
                    r["texture_list"] = hex(e[10])
                regions.append(r)
    return regions


def main():
    rom = open(sys.argv[1], "rb").read()
    regions = []
    for name, kind, s, e, vram, vsize, ev in FIXED:
        r = {"name": name, "kind": kind, "rom_start": hex(s), "rom_end": hex(e), "evidence": ev}
        if vram:
            r["vram"] = hex(vram)
        if vsize:
            r["vram_size"] = hex(vsize)
        regions.append(r)
    regions += course_regions(rom)
    regions += audio_regions(rom)

    starts = {int(r["rom_start"], 16): r["name"] for r in regions}
    starts.update({b[1]: b[0] for b in BANKS})
    for name, start, ev, index in BANKS:
        end = min(s for s in starts if s > start)
        regions.append({"name": name, "kind": "bank", "rom_start": hex(start), "rom_end": hex(end),
                        "evidence": ev, "end_evidence": f"next literal region {starts[end]} starts here",
                        "index": index})

    def gap(s, e):
        # A gap made of a few runs of one repeated byte (alignment, then 0xff fill) is padding;
        # anything else is unexplained.
        runs = [(k, len(list(g))) for k, g in itertools.groupby(rom[s:e])]
        if len(runs) > 4:
            return [{"name": f"unknown_{s:06x}", "kind": "unknown", "rom_start": hex(s), "rom_end": hex(e),
                     "evidence": "not explained yet"}]
        out = []
        for byte, n in runs:
            out.append({"name": f"pad_{s:06x}", "kind": "padding", "rom_start": hex(s), "rom_end": hex(s + n),
                        "evidence": f"all bytes 0x{byte:02x}"})
            s += n
        return out

    # Overlap check and gap filling.
    regions.sort(key=lambda r: int(r["rom_start"], 16))
    pos = 0
    filled = []
    for r in regions:
        s, e = int(r["rom_start"], 16), int(r["rom_end"], 16)
        if s < pos:
            sys.exit(f"overlap: {r['name']} starts {s:#x} before previous end {pos:#x}")
        if s > pos:
            filled += gap(pos, s)
        filled.append(r)
        pos = e
    if pos < len(rom):
        filled += gap(pos, len(rom))

    # Attach MIO0 statistics to each region.
    blocks = list(mio0.scan(rom))
    for r in filled:
        s, e = int(r["rom_start"], 16), int(r["rom_end"], 16)
        inside = [b for b in blocks if s <= b[0] < e]
        if inside:
            r["mio0_blocks"] = len(inside)
            r["mio0_bytes"] = hex(sum(b[2] for b in inside))
    stray = [b for b in blocks if not any(int(r["rom_start"], 16) <= b[0] < int(r["rom_end"], 16)
                                          and r["kind"] != "unknown" for r in filled)]

    json.dump({"rom_sha1": SHA1, "rom_size": hex(len(rom)), "regions": filled},
              open(OUT, "w", encoding="utf-8"), indent=1)

    size = len(rom)
    by_kind = {}
    for r in filled:
        n = int(r["rom_end"], 16) - int(r["rom_start"], 16)
        k = r["kind"] + ("~" if r.get("end_is_inferred") else "")
        by_kind[k] = by_kind.get(k, 0) + n
    print("cobertura por tipo (~ = fim inferido):")
    for k, n in sorted(by_kind.items(), key=lambda x: -x[1]):
        print(f"  {k:10} {n:#9x}  {100 * n / size:5.1f}%")
    print(f"regioes: {len(filled)}; blocos MIO0 fora de regioes conhecidas: {len(stray)}")
    for r in filled:
        if r["kind"] == "unknown":
            print(f"  desconhecido {r['rom_start']}-{r['rom_end']} ({int(r['rom_end'],16)-int(r['rom_start'],16):#x} bytes,"
                  f" {r.get('mio0_blocks', 0)} MIO0)")


if __name__ == "__main__":
    main()
