"""Read the plaintext headers of an Xbox 360 XEX2 executable (no decryption).

usage: python -I xexinfo.py <default.xex>
"""
import struct
import sys

KEYS = {
    0x000002FF: "RESOURCE_INFO",
    0x000003FF: "FILE_FORMAT_INFO",
    0x00010001: "ORIGINAL_BASE_ADDRESS",
    0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP",
    0x000183FF: "ORIGINAL_PE_NAME",
    0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO",
    0x00020200: "DEFAULT_STACK_SIZE",
    0x00020301: "DEFAULT_FS_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE",
    0x00030000: "SYSTEM_FLAGS",
    0x00040006: "EXECUTION_INFO",
    0x00040310: "GAME_RATINGS",
    0x00040404: "LAN_KEY",
    0x000405FF: "XBOX360_LOGO",
    0x00E10402: "TITLE_WORKSPACE_SIZE",
}
PAGE_KIND = {1: "code", 2: "data", 3: "read-only data"}


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def main():
    x = open(sys.argv[1], "rb").read()
    assert x[:4] == b"XEX2", "not an XEX2 file"
    flags, pe_off, _, sec_off, nopt = struct.unpack_from(">IIIII", x, 4)
    print(f"file size           {len(x):,} bytes")
    print(f"module flags        {flags:#010x}  (title={bool(flags & 1)}, dll={bool(flags & 8)})")
    print(f"PE data offset      {pe_off:#x}")

    hdr = {}
    for i in range(nopt):
        k, v = struct.unpack_from(">II", x, 24 + i * 8)
        hdr[k] = v

    for k, v in sorted(hdr.items()):
        name = KEYS.get(k, f"key_{k:08x}")
        if k & 0xFF in (0, 1):
            print(f"{name:<20}{v:#010x}")

    if 0x000183FF in hdr:
        o = hdr[0x000183FF]
        print(f"original PE name    {cstr(x[o + 4 : o + be32(x, o)])}")

    if 0x00040006 in hdr:
        o = hdr[0x00040006]
        media, ver, base_ver, title = struct.unpack_from(">IIII", x, o)
        plat, exe_t, disc, ndisc = x[o + 16 : o + 20]
        v = lambda n: f"{n >> 28}.{(n >> 24) & 0xF}.{(n >> 8) & 0xFFFF}.{n & 0xFF}"
        print(f"title id            {title:08X}")
        print(f"title version       {v(ver)}  (base {v(base_ver)})")
        print(f"disc                {disc} of {ndisc}")

    if 0x000003FF in hdr:
        o = hdr[0x000003FF]
        enc, comp = struct.unpack_from(">HH", x, o + 4)
        print(f"encryption          {['none', 'AES (retail)'][enc] if enc < 2 else enc}")
        print(f"compression         {['none', 'basic', 'LZX', 'delta'][comp] if comp < 4 else comp}")

    if 0x00020200 in hdr:
        print(f"default stack size  {hdr[0x00020200]:,} bytes")

    # Security info: image size, load address, page map
    image_size = be32(x, sec_off + 4)
    image_flags = be32(x, sec_off + 0x10C)
    load_addr = be32(x, sec_off + 0x110)
    npages = be32(x, sec_off + 0x180)
    page_size = 0x1000 if image_flags & 0x10000000 else 0x10000
    print(f"\nimage size          {image_size:,} bytes ({image_size / 2**20:.1f} MB)")
    print(f"load address        {load_addr:#010x}")
    print(f"page size           {page_size:#x}")
    totals = {}
    regions = []
    addr = load_addr
    for i in range(npages):
        val = be32(x, sec_off + 0x184 + i * 24)
        count, kind = val >> 4, val & 0xF
        size = count * page_size
        totals[kind] = totals.get(kind, 0) + size
        regions.append((addr, size, kind))
        addr += size
    print("memory layout:")
    # merge adjacent regions of the same kind
    merged = []
    for r in regions:
        if merged and merged[-1][2] == r[2] and merged[-1][0] + merged[-1][1] == r[0]:
            merged[-1] = (merged[-1][0], merged[-1][1] + r[1], r[2])
        else:
            merged.append(r)
    for a, s, k in merged:
        print(f"  {a:#010x}-{a + s:#010x}  {s / 2**20:7.2f} MB  {PAGE_KIND.get(k, k)}")
    for k, s in totals.items():
        print(f"  total {PAGE_KIND.get(k, k):<15}{s:,} bytes")
    code = totals.get(1, 0)
    print(f"  => ~{code // 4:,} PowerPC instruction slots in code pages")

    if 0x000200FF in hdr:
        o = hdr[0x000200FF]
        n = (be32(x, o) - 4) // 16
        print(f"\nstatic libraries linked ({n}):")
        for i in range(n):
            e = x[o + 4 + i * 16 : o + 20 + i * 16]
            maj, mnr, bld, qfe = struct.unpack(">HHHH", e[8:])
            print(f"  {cstr(e[:8]):<10}{maj}.{mnr}.{bld}.{qfe & 0xFF}")

    if 0x000103FF in hdr:
        o = hdr[0x000103FF]
        strtab_size, nstr = struct.unpack_from(">II", x, o + 4)
        names, p = [], o + 12
        while len(names) < nstr:
            s = cstr(x[p:])
            names.append(s)
            p += len(s) + 1
            while p % 4:
                p += 1
        p = o + 12 + strtab_size
        print("\nimport libraries:")
        total = 0
        for _ in range(nstr):
            size = be32(x, p)
            ver, minver = be32(x, p + 28), be32(x, p + 32)
            idx, count = struct.unpack_from(">HH", x, p + 36)
            total += count
            print(f"  {names[idx]:<14}{count:5} import records")
            p += size
        print(f"  total {total} records (~{total // 2} imported functions/variables; each function has 2 records)")


if __name__ == "__main__":
    main()
