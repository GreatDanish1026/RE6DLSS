"""Turns RE6DLSS-rt-NN-<id>.bin render target snapshots into PNGs (pure Python).
For each file: <name>.rgb.png (colour channels; float formats get a simple x/(1+x) curve) and
<name>.a.png (alpha), plus <name>.ch0.png / .ch1.png ... for single/two-channel formats, each
channel stretched to its own min..max so depth and velocity are visible. Prints channel ranges.
Usage: python -I tools/rt2png.py <folder>"""
import glob
import os
import struct
import sys
import zlib

FORMATS = {10: ("e", 4), 2: ("f", 4), 16: ("f", 2), 34: ("e", 2), 41: ("f", 1), 54: ("e", 1),
           28: ("B", 4), 87: ("B", 4), 35: ("H", 2), 56: ("H", 1)}


def png(path, w, h, rows, channels):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    color = {1: 0, 3: 2, 4: 6}[channels]
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main(folder):
    for path in sorted(glob.glob(os.path.join(folder, "RE6DLSS-rt-*.bin"))):
        data = open(path, "rb").read()
        magic, w, h, fmt, step = struct.unpack_from("<5I", data, 0)
        if fmt not in FORMATS:
            print(os.path.basename(path), "format", fmt, "not handled")
            continue
        kind, ch = FORMATS[fmt]
        size = struct.calcsize(kind)
        n = w * h * ch
        vals = struct.unpack_from("<%d%s" % (n, kind), data, 20)
        if fmt == 87:  # bgra -> rgba
            v = list(vals)
            for i in range(0, n, 4):
                v[i], v[i + 2] = v[i + 2], v[i]
            vals = v
        lo = [min(vals[c::ch]) for c in range(ch)]
        hi = [max(vals[c::ch]) for c in range(ch)]
        base = os.path.splitext(path)[0]
        print("%s %dx%d fmt %d: %s" % (os.path.basename(path), w, h, fmt,
              ", ".join("ch%d %.4g..%.4g" % (c, lo[c], hi[c]) for c in range(ch))))
        def norm(x, c):
            span = hi[c] - lo[c]
            return 0 if span <= 0 else int(255 * (x - lo[c]) / span)
        if ch >= 3:
            rows = []
            for y in range(h):
                r = []
                for x in range(w):
                    i = (y * w + x) * ch
                    for c in range(3):
                        v = vals[i + c]
                        if kind in "ef":
                            v = max(v, 0.0)
                            v = int(255 * (v / (1.0 + v)) ** (1 / 2.2))
                        elif kind == "B":
                            pass
                        else:
                            v = v >> 8
                        r.append(max(0, min(255, int(v))))
                rows.append(r)
            png(base + ".rgb.png", w, h, rows, 3)
        for c in range(ch if ch < 3 else 4):
            if ch >= 3 and c < 3:
                continue
            rows = [[norm(vals[(y * w + x) * ch + c], c) for x in range(w)] for y in range(h)]
            png(base + (".a.png" if ch >= 3 else ".ch%d.png" % c), w, h, rows, 1)


if __name__ == "__main__":
    main(sys.argv[1])
