"""Summarize dumped PNG frames: size and min/max/mean of the red channel.

usage: python -I frame_stats.py <dir> [every_nth]
Reads the uncompressed-deflate PNGs written by the runtime's DUMP_FRAMES.
"""
import glob
import struct
import sys
import zlib


def stats(path):
    data = open(path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", data[pos + 8:pos + 16])
        elif kind == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 4 + 1
    px = [raw[y * stride + 1 + x * 4] for y in range(0, h, 8) for x in range(0, w, 8)]
    return w, h, min(px), max(px), round(sum(px) / len(px), 1)


files = sorted(glob.glob(sys.argv[1] + "/*.png"))
step = int(sys.argv[2]) if len(sys.argv) > 2 else max(1, len(files) // 10)
for f in files[::step]:
    print(f.split("\\")[-1].split("/")[-1], stats(f))
