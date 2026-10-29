"""GPU x software comparison of the display picture.

Runs each replay twice, with MK64_RENDERER=cpu and =gpu at the same internal resolution (and
aspect ratio / interpolation, if given), and compares the enlarged frames (frame_XXXXX_hi.bmp):
mean difference per channel and share of pixels with a channel differing by more than 8/255,
after putting the GPU image on the RGBA16 grid of the software one (there, more than one RGBA16
step, 9/255).
Fails above the thresholds (mean <= 2/255, at most 1% of the pixels above 8/255). The worst frames
go to build/gpu_compare/mosaico.png (software | GPU | difference x4) and each frame's difference
to build/gpu_compare/<replay>/diff_XXXXX.png. The 1x frames must also hash the same.

Usage:
  python -I tools/gpu_compare.py <rom.z64> [name ...] [--scale N] [--aspect 16:9] [--interp]
         [--pair A.bmp B.bmp out.png]   (only builds a side-by-side PNG of two BMPs)
"""
import importlib.util
import os
import shutil
import struct
import sys
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "build", "gpu_compare")
MEAN_MAX = 2.0      # /255
OVER_MAX = 1.0      # % of pixels with a channel differing by more than 8/255
MOSAIC_FRAMES = 6
# Frames where the software picture itself is off and the GPU is not expected to follow it; they
# are reported but do not fail. Replay -> {frame: reason}.
KNOWN = {
    "vs4p_luigi": dict.fromkeys(
        ["02500", "02600", "02700", "02800", "02900"],
        "parede colada na camera do jogador 1: o software (1x e ampliado) deixa a areia atras dela "
        "vazar em pontos soltos (ruido das baricentricas em float num triangulo enorme); a GPU desenha a parede inteira"),
}


def load_replay_module():
    spec = importlib.util.spec_from_file_location("replay", os.path.join(os.path.dirname(os.path.abspath(__file__)), "replay.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_bmp(path):
    """(width, height, rows of RGB bytes, top row first) of a 24/32-bit BMP."""
    data = open(path, "rb").read()
    off = struct.unpack_from("<I", data, 10)[0]
    hsize = struct.unpack_from("<I", data, 14)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    comp = struct.unpack_from("<I", data, 30)[0]
    bypp = bpp // 8
    shifts = (16, 8, 0)  # B, G, R in memory for plain 24/32-bit
    if comp == 3 and bpp == 32:
        masks = struct.unpack_from("<III", data, 54 if hsize >= 52 else 54)
        shifts = tuple((m & -m).bit_length() - 1 for m in masks)
    stride = (w * bypp + 3) & ~3
    rows = []
    for y in range(abs(h)):
        src_y = (abs(h) - 1 - y) if h > 0 else y
        line = data[off + src_y * stride: off + src_y * stride + w * bypp]
        if bypp == 3 and shifts == (16, 8, 0):
            rgb = bytearray(w * 3)
            rgb[0::3] = line[2::3]
            rgb[1::3] = line[1::3]
            rgb[2::3] = line[0::3]
        else:
            px = struct.unpack("<%dI" % w, line) if bypp == 4 else None
            rgb = bytearray(w * 3)
            if px is not None:
                rs, gs, bs = shifts
                for x, p in enumerate(px):
                    rgb[3 * x] = (p >> rs) & 255
                    rgb[3 * x + 1] = (p >> gs) & 255
                    rgb[3 * x + 2] = (p >> bs) & 255
        rows.append(bytes(rgb))
    return w, abs(h), rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


# The software display copies are RGBA16 (5 bits per channel, shown as c * 255 // 31); the GPU
# keeps 8 bits. The GPU image is put on the same grid before comparing, so the thresholds measure
# the renderers and not the storage format (the raw mean is reported too).
GRID = bytes(round(x * 31 / 255) * 255 // 31 for x in range(256))


def compare(a, b, grid=True):
    """mean difference (/255 per channel), % of pixels over 8/255, difference rows (x4)."""
    w, h, ra = a
    _, _, rb = b
    lim = 8
    if grid:
        rb = [r.translate(GRID) for r in rb]
        lim = 9  # one RGBA16 step is 8 or 9 / 255: "over" means more than one step
    total = 0
    over = 0
    diff_rows = []
    amp = bytes(min(255, 4 * d) for d in range(256))
    for y in range(h):
        la, lb = ra[y], rb[y]
        d = bytes(abs(p - q) for p, q in zip(la, lb))
        total += sum(d)
        for x in range(0, len(d), 3):
            if d[x] > lim or d[x + 1] > lim or d[x + 2] > lim:
                over += 1
        diff_rows.append(d.translate(amp))
    n = w * h
    return total / (n * 3), 100.0 * over / n, diff_rows


def downscale(w, h, rows, f):
    if f == 1:
        return w, h, rows
    out = []
    for y in range(0, h - h % f, f):
        r = rows[y]
        line = bytearray()
        for x in range(0, w - w % f, f):
            line += r[3 * x:3 * x + 3]
        out.append(bytes(line))
    return w // f, len(out), out


def mosaic(entries, path):
    """Rows of software | GPU | difference for the given (a, b, diff) images."""
    tiles = []
    for a, b, d in entries:
        f = max(1, a[0] // 480)
        tiles.append([downscale(*a, f), downscale(*b, f), downscale(a[0], a[1], d, f)])
    width = max(sum(t[0] + 4 for t in row) for row in tiles)
    rows = []
    for row in tiles:
        hh = max(t[1] for t in row)
        for y in range(hh):
            line = bytearray()
            for t in row:
                line += t[2][y] if y < t[1] else bytes(t[0] * 3)
                line += bytes(12)
            line += bytes(width * 3 - len(line))
            rows.append(bytes(line))
        rows.extend([bytes(width * 3)] * 4)
    write_png(path, width, len(rows), rows)


def main():
    args = sys.argv[1:]
    if "--pair" in args:
        k = args.index("--pair")
        a, b = read_bmp(args[k + 1]), read_bmp(args[k + 2])
        mosaic([(a, b, compare(a, b)[2])], args[k + 3])
        return
    rom = args[0]
    opts = {"--scale": "2", "--aspect": None}
    names = []
    interp = "--interp" in args
    k = 1
    while k < len(args):
        if args[k] in opts:
            opts[args[k]] = args[k + 1]
            k += 2
            continue
        if args[k] != "--interp":
            names.append(args[k])
        k += 1
    replay = load_replay_module()
    names = names or sorted(f[:-5] for f in os.listdir(replay.DIR) if f.endswith(".json"))
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    env = {"MK64_RES_SCALE": opts["--scale"], "MK64_INTERP": "1" if interp else "0"}
    if opts["--aspect"]:
        env["MK64_ASPECT"] = opts["--aspect"]
    os.environ.update(env)
    worst = []
    failed = 0
    import json
    for name in names:
        spec = json.load(open(os.path.join(replay.DIR, name + ".json"), encoding="utf-8"))
        hashes = {}
        for mode in ("cpu", "gpu"):
            os.environ["MK64_RENDERER"] = mode
            hashes[mode], _ = replay.run(rom, spec, name)
            dst = os.path.join(OUT, name, mode)
            shutil.copytree(os.path.join(ROOT, "build", "replays", name), dst)
        bad_1x = [n for n in hashes["cpu"] if hashes["gpu"].get(n) != hashes["cpu"][n]]
        frames = sorted(f for f in os.listdir(os.path.join(OUT, name, "cpu")) if f.endswith("_hi.bmp"))
        stats = []
        raw = []
        for f in frames:
            gpath = os.path.join(OUT, name, "gpu", f)
            if not os.path.exists(gpath):
                stats.append((f, 255.0, 100.0))
                continue
            a, b = read_bmp(os.path.join(OUT, name, "cpu", f)), read_bmp(gpath)
            if a[:2] != b[:2]:
                stats.append((f, 255.0, 100.0))
                continue
            mean, over, diff = compare(a, b)
            raw.append(compare(a, b, False)[0])
            write_png(os.path.join(OUT, name, "diff_" + f[6:11] + ".png"), a[0], a[1], diff)
            stats.append((f, mean, over))
            worst.append((over + mean, name, f))
        if not frames:
            print(f"{name}: sem quadros ampliados")
            failed += 1
            continue
        mean_all = sum(s[1] for s in stats) / len(stats)
        top = max(stats, key=lambda s: s[2] + s[1])
        known = KNOWN.get(name, {})
        over_limit = [s for s in stats if s[1] > MEAN_MAX or s[2] > OVER_MAX]
        for s in over_limit:
            if s[0][6:11] in known:
                print(f"{name}/{s[0][6:11]}: diferenca conhecida ({known[s[0][6:11]]})")
        ok = all(s[0][6:11] in known for s in over_limit) and not bad_1x
        failed += not ok
        raw_mean = sum(raw) / len(raw) if raw else 0.0
        print(f"{name}: {'ok' if ok else 'FALHOU'} - media {mean_all:.2f}/255 (sem a grade RGBA16: {raw_mean:.2f}); pior {top[0][6:11]}: "
              f"media {top[1]:.2f}/255, {top[2]:.2f}% acima de 8/255" + (f"; 1x difere em {bad_1x[:3]}" if bad_1x else ""))
    worst.sort(reverse=True)
    entries = []
    for _, name, f in worst[:MOSAIC_FRAMES]:
        a = read_bmp(os.path.join(OUT, name, "cpu", f))
        b = read_bmp(os.path.join(OUT, name, "gpu", f))
        entries.append((a, b, compare(a, b)[2]))
    if entries:
        mosaic(entries, os.path.join(OUT, "mosaico.png"))
        print("piores quadros: " + ", ".join(f"{n}/{f[6:11]}" for _, n, f in worst[:MOSAIC_FRAMES]))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
