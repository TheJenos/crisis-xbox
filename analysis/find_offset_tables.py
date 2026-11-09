"""Find jump tables XenonAnalyse misses: tables of 8- or 16-bit offsets.

The compiler emits these two idioms (r12/r0 as scratch, rI = case index):

  16-bit byte offsets:  lis r12,T@ha / rlwinm r0,rI,1,0,30 / addi r12,r12,T@l / lhzx r0,r12,r0
                        lis r12,B@ha / addi r12,r12,B@l / nop / add r12,r12,r0 / mtctr r12 / bctr
  8-bit word offsets:   lis r12,T@ha / addi r12,r12,T@l / lbzx r0,r12,rI / rlwinm r0,r0,2,...
                        lis r12,B@ha / nop / addi r12,r12,B@l / add r12,r12,r0 / mtctr r12 / bctr

with a bounds check (cmplwi crN,rI,max / bgt crN,default) shortly before.
Prints [[switch]] entries (XenonRecomp format) for tables not already in the
given switch file.

usage: python -I find_offset_tables.py <image.bin> <existing switch toml>
"""
import re
import struct
import sys

BASE = 0x82000000

# Tables whose index is bounded far from the jump (by construction): entry count.
# 837EF0FC: index = (r11 >> 12) & 0xF, checked against 15 long before the jump.
OVERRIDES = {0x837EF0FC: 16}


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def main():
    img = open(sys.argv[1], "rb").read()
    known = set(int(m, 16) for m in re.findall(r"base = 0x([0-9A-Fa-f]+)", open(sys.argv[2]).read()))
    word = lambda a: struct.unpack(">I", img[a - BASE:a - BASE + 4])[0]
    out = []
    for off in range(0, len(img) - 4, 4):
        if struct.unpack(">I", img[off:off + 4])[0] != 0x4E800420:  # bctr
            continue
        bctr = BASE + off
        if bctr in known:
            continue
        win = [word(bctr - 4 * k) for k in range(12, 0, -1)]  # 12 instructions before
        ops = [(w >> 26, (w >> 1) & 0x3FF, w) for w in win]
        # Find the load of the offset (lhzx / lbzx) in the window.
        li = next((i for i, (op, xo, w) in enumerate(ops) if op == 31 and xo in (279, 87)), None)
        if li is None:
            continue
        lw = ops[li][2]
        half = ops[li][1] == 279
        rA, rB = (lw >> 16) & 31, (lw >> 11) & 31
        # Table address: lis/addi into rA before the load.
        hi = lo = None
        for op, xo, w in ops[:li]:
            if op == 15 and ((w >> 21) & 31) == rA and ((w >> 16) & 31) == 0:
                hi = s16(w & 0xFFFF)
            if op == 14 and ((w >> 21) & 31) == rA and ((w >> 16) & 31) == rA:
                lo = s16(w & 0xFFFF)
        if hi is None or lo is None:
            continue
        table = ((hi << 16) + lo) & 0xFFFFFFFF
        # Index register: halfword form shifts rI into rB with rlwinm before the load.
        index = rB
        shift = 0
        for op, xo, w in ops[:li]:
            if op == 21 and ((w >> 16) & 31) == rB and half:
                index = (w >> 21) & 31
        for op, xo, w in ops[li + 1:]:
            if op == 21 and ((w >> 21) & 31) == ((lw >> 21) & 31) and not half:
                shift = (w >> 11) & 31
        # Target base: the last lis/addi pair after the load.
        bhi = blo = None
        for op, xo, w in ops[li + 1:]:
            if op == 15 and ((w >> 16) & 31) == 0:
                bhi = s16(w & 0xFFFF)
            if op == 14:
                blo = s16(w & 0xFFFF)
        if bhi is None or blo is None:
            continue
        base = ((bhi << 16) + blo) & 0xFFFFFFFF
        # Bounds check: the nearest cmplwi on the index register before the
        # bctr, and the bgt on that condition field (the default case).
        count = default = None
        for k in range(1, 48):
            a = bctr - 4 * k
            w = word(a)
            if (w >> 26) == 10 and ((w >> 16) & 31) == index:
                count = (w & 0xFFFF) + 1
                crf = (w >> 23) & 7
                for j in range(1, k):
                    b = word(a + 4 * j)
                    if (b >> 26) == 16 and ((b >> 21) & 31) == 12 and ((b >> 16) & 31) == crf * 4 + 1:  # bgt crf
                        default = (a + 4 * j + s16(b & 0xFFFC)) & 0xFFFFFFFF
                        break
                break
        if bctr in OVERRIDES:
            count = OVERRIDES[bctr]
            default = None
        if not count or count > 512 or (default is None and bctr not in OVERRIDES):
            print("# skipped bctr %08X (table %08X): bounds not found" % (bctr, table), file=sys.stderr)
            continue
        labels = []
        for i in range(count):
            if half:
                v = struct.unpack(">H", img[table - BASE + 2 * i:table - BASE + 2 * i + 2])[0]
            else:
                v = img[table - BASE + i] << shift
            labels.append((base + v) & 0xFFFFFFFF)
        if default is None:
            default = labels[0] # the index can't be out of range
        out.append((bctr, table, 2 if half else 1, index, default, labels))

    print("# ---- 8/16-BIT OFFSET JUMPTABLES (analysis/find_offset_tables.py) ----")
    for bctr, table, size, index, default, labels in out:
        print("[[switch]]")
        print("base = 0x%08X" % bctr)
        print("# table = 0x%08X, entry size %d" % (table, size))
        print("r = %d" % index)
        print("default = 0x%08X" % default)
        print("labels = [")
        for l in labels:
            print("    0x%08X," % l)
        print("]")
        print()
    print("# %d tables" % len(out), file=sys.stderr)


if __name__ == "__main__":
    main()
