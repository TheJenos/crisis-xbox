"""Minimal read-only XDVDFS (Xbox 360 disc filesystem) lister/extractor.

usage: python -I xiso.py <iso> list
       python -I xiso.py <iso> extract <path-in-iso> <out-file>
"""
import struct
import sys

SECTOR = 2048
PARTITIONS = [0xFD90000, 0x2080000, 0x18300000, 0]  # XGD2, XGD3, XGD1, raw


def find_partition(f):
    for off in PARTITIONS:
        f.seek(off + 32 * SECTOR)
        if f.read(20) == b"MICROSOFT*XBOX*MEDIA":
            return off
    raise SystemExit("no XDVDFS volume descriptor found")


def walk(f, base, sector, size, prefix=""):
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    out = []
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, sec, sz, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:  # padding
            continue
        name = table[off + 14 : off + 14 + nlen].decode("ascii", "replace")
        path = prefix + name
        if attr & 0x10:
            out.append((path + "/", sec, sz, True))
            if sz:
                out += walk(f, base, sec, sz, path + "/")
        else:
            out.append((path, sec, sz, False))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
    return out


def main():
    iso, cmd = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sec, root_size = struct.unpack("<II", f.read(8))
        entries = walk(f, base, root_sec, root_size)
        if cmd == "list":
            print(f"partition offset: {base:#x}")
            for path, sec, sz, is_dir in sorted(entries):
                if not is_dir:
                    print(f"{sz:>12,}  {path}")
            files = [e for e in entries if not e[3]]
            print(f"\n{len(files)} files, {sum(e[2] for e in files):,} bytes")
        elif cmd == "extractall":
            # Every file into <outdir>, plus <outdir>/disc-index.txt for the runtime:
            #   "D\t<path>" for directories, "F\t<size>\t<path>" for files ('/' separators).
            import os
            out_dir = sys.argv[3]
            os.makedirs(out_dir, exist_ok=True)
            index = []
            total = sum(e[2] for e in entries if not e[3])
            done = 0
            for path, sec, sz, is_dir in sorted(entries):
                rel = path.lstrip("/")
                dest = os.path.join(out_dir, *rel.split("/"))
                if is_dir:
                    os.makedirs(dest, exist_ok=True)
                    index.append(f"D\t{rel}")
                    continue
                os.makedirs(os.path.dirname(dest) or out_dir, exist_ok=True)
                index.append(f"F\t{sz}\t{rel}")
                if os.path.isfile(dest) and os.path.getsize(dest) == sz:
                    done += sz
                    continue  # already extracted (resumable)
                f.seek(base + sec * SECTOR)
                with open(dest + ".part", "wb") as o:
                    remaining = sz
                    while remaining:
                        chunk = f.read(min(remaining, 4 << 20))
                        o.write(chunk)
                        remaining -= len(chunk)
                os.replace(dest + ".part", dest)
                done += sz
                print(f"{done * 100 // max(total, 1):3d}%  {rel} ({sz:,} bytes)", flush=True)
            with open(os.path.join(out_dir, "disc-index.txt"), "w", newline="\n") as o:
                o.write("\n".join(index) + "\n")
            print(f"extracted {sum(1 for e in entries if not e[3])} files, {total:,} bytes -> {out_dir}")
        elif cmd == "extract":
            want, dst = sys.argv[3].lower(), sys.argv[4]
            for path, sec, sz, is_dir in entries:
                if path.lower() == want and not is_dir:
                    f.seek(base + sec * SECTOR)
                    with open(dst, "wb") as o:
                        remaining = sz
                        while remaining:
                            chunk = f.read(min(remaining, 1 << 20))
                            o.write(chunk)
                            remaining -= len(chunk)
                    print(f"extracted {path} ({sz:,} bytes) -> {dst}")
                    return
            raise SystemExit(f"not found: {want}")


if __name__ == "__main__":
    main()
