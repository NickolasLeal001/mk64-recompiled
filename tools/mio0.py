"""MIO0 decompressor and ROM scanner.

MIO0 layout (big-endian): "MIO0", u32 decompressed size, u32 comp offset, u32 raw offset,
then a bit stream of layout words from offset 0x10. Bit 1 = copy one raw byte;
bit 0 = back-reference from a 16-bit comp entry: length = (v >> 12) + 3, distance = (v & 0xfff) + 1.

Usage: python -I tools/mio0.py <rom.z64>   # lists every block that decodes cleanly
"""
import struct
import sys


class Mio0Error(ValueError):
    pass


def decompress(data, start=0):
    if data[start:start + 4] != b"MIO0":
        raise Mio0Error("missing MIO0 magic")
    size, comp_off, raw_off = struct.unpack_from(">III", data, start + 4)
    if size == 0 or size > 0x400000 or comp_off < 0x10 or raw_off < comp_off:
        raise Mio0Error("implausible header")
    out = bytearray()
    layout = start + 0x10
    comp = start + comp_off
    raw = start + raw_off
    end_limit = len(data)
    bits = 0
    nbits = 0
    while len(out) < size:
        if nbits == 0:
            if layout + 4 > start + comp_off:
                raise Mio0Error("layout stream overrun")
            bits = struct.unpack_from(">I", data, layout)[0]
            layout += 4
            nbits = 32
        flag = bits & 0x80000000
        bits = (bits << 1) & 0xFFFFFFFF
        nbits -= 1
        if flag:
            if raw >= end_limit:
                raise Mio0Error("raw stream overrun")
            out.append(data[raw])
            raw += 1
        else:
            if comp + 2 > end_limit:
                raise Mio0Error("comp stream overrun")
            v = (data[comp] << 8) | data[comp + 1]
            comp += 2
            length = (v >> 12) + 3
            dist = (v & 0xFFF) + 1
            if dist > len(out):
                raise Mio0Error("back-reference before start")
            for _ in range(length):
                out.append(out[-dist])
    del out[size:]
    # Compressed block ends after whichever stream reached furthest.
    consumed = max(comp, raw) - start
    return bytes(out), consumed


def scan(rom):
    """Yield (offset, decompressed_size, compressed_size) for blocks that decode cleanly."""
    i = rom.find(b"MIO0")
    while i != -1:
        if i % 4 == 0:
            try:
                out, consumed = decompress(rom, i)
                yield i, len(out), consumed
            except Mio0Error:
                pass
        i = rom.find(b"MIO0", i + 4)


def main():
    rom = open(sys.argv[1], "rb").read()
    blocks = list(scan(rom))
    total_c = sum(b[2] for b in blocks)
    total_d = sum(b[1] for b in blocks)
    for off, dsize, csize in blocks:
        print(f"{off:#08x} comp {csize:#07x} -> {dsize:#07x}")
    print(f"{len(blocks)} blocos validos; {total_c:#x} bytes comprimidos -> {total_d:#x}", file=sys.stderr)


if __name__ == "__main__":
    main()
