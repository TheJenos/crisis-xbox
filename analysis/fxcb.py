"""Extract Xenos shader microcode from CryEngine shader caches (.pak -> .fxcb).

.fxcb ("KCPC" big-endian resource file): header (magic, version, entry count,
directory offset) and a directory of 12-byte entries (name crc, size:24 |
flags:8, offset). Entry data is LZSS-compressed (4-byte big-endian unpacked
size, then Okumura-style LZSS: N=4096, F=18, threshold 2).

Inside the unpacked entries, compiled Xbox 360 shaders are found by their
D3D container magic 0x102A11xx; the microcode is located from the container
header. Output: <out>/<vs|ps>_<n>.bin as little-endian (host) dwords, the same
format as the runtime's SHADER_DUMP.

usage: python -I fxcb.py <pak> <outdir> [limit]
"""
import hashlib
import os
import struct
import sys
import zipfile


def lzss_decode(src, out_size):
    N, F, THRESHOLD = 4096, 18, 2
    ring = bytearray(N)
    r = N - F
    out = bytearray()
    i = 0
    flags = 0
    while len(out) < out_size and i < len(src):
        flags >>= 1
        if not flags & 0x100:
            flags = src[i] | 0xFF00
            i += 1
            if i >= len(src):
                break
        if flags & 1:
            c = src[i]
            i += 1
            out.append(c)
            ring[r] = c
            r = (r + 1) & (N - 1)
        else:
            if i + 1 >= len(src):
                break
            lo, hi = src[i], src[i + 1]
            i += 2
            pos = lo | ((hi & 0xF0) << 4)
            length = (hi & 0x0F) + THRESHOLD + 1
            for k in range(length):
                c = ring[(pos + k) & (N - 1)]
                out.append(c)
                ring[r] = c
                r = (r + 1) & (N - 1)
    return bytes(out[:out_size])


def entries(fxcb):
    magic, version, count, diroff = struct.unpack(">4sIII", fxcb[:16])
    if magic != b"KCPC":
        return
    for n in range(count):
        name, sf, off = struct.unpack(">III", fxcb[diroff + n * 12: diroff + n * 12 + 12])
        size, flags = sf >> 8, sf & 0xFF
        yield name, size, flags, off


def unpack_entry(fxcb, size, off):
    raw = fxcb[off: off + size]
    if len(raw) < 4:
        return b""
    out_size = struct.unpack(">I", raw[:4])[0]
    if out_size > 16 * 1024 * 1024:
        return b""
    return lzss_decode(raw[4:], out_size)


def find_shaders(blob):
    """Yield (is_pixel, microcode bytes) for each D3D Xbox shader container.

    Container header (big-endian dwords): magic 0x102A11xx, microcode offset,
    microcode size, 0, offset of the constant-table block (size dword, then a
    D3D9 CTAB whose version dword says ps_3_0 / vs_3_0)."""
    i = 0
    while True:
        i = blob.find(b"*", i)
        if i < 0 or i + 0x20 > len(blob):
            return
        try:
            magic, mc_off, mc_size, _, ctab_off = struct.unpack(">IIIII", blob[i: i + 20])
            version = struct.unpack(">I", blob[i + ctab_off + 12: i + ctab_off + 16])[0]
        except struct.error:
            i += 4
            continue
        if version >> 16 in (0xFFFF, 0xFFFE) and mc_size and mc_size % 12 == 0 and i + mc_off + mc_size <= len(blob):
            yield version >> 16 == 0xFFFF, blob[i + mc_off: i + mc_off + mc_size]
            i += mc_off + mc_size
        else:
            i += 4


def main():
    pak, outdir = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    os.makedirs(outdir, exist_ok=True)
    z = zipfile.ZipFile(pak)
    seen = set()
    counts = {True: 0, False: 0}
    files = 0
    for info in z.infolist():
        if not info.filename.endswith(".fxcb"):
            continue
        files += 1
        data = z.read(info)
        for name, size, flags, off in entries(data):
            blob = unpack_entry(data, size, off)
            for pixel, mc in find_shaders(blob):
                h = hashlib.sha1(mc).hexdigest()[:16]
                if h in seen:
                    continue
                seen.add(h)
                words = struct.unpack(">%dI" % (len(mc) // 4), mc)
                with open(os.path.join(outdir, ("ps_" if pixel else "vs_") + h + ".bin"), "wb") as f:
                    f.write(struct.pack("<%dI" % len(words), *words))
                counts[pixel] += 1
        if limit and len(seen) >= limit:
            break
    print(f"{files} cache files, {counts[False]} vertex + {counts[True]} pixel unique shaders -> {outdir}")


if __name__ == "__main__":
    main()
