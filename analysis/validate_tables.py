"""Sanity checks for a XenonAnalyse switch table TOML that has '# table = ...' comments.

- table data ranges must not overlap (an overlap means a wrong case count)
- writes all case labels to a file so diag.exe can check they decode as instructions

usage: python -I validate_tables.py <switch_tables.toml> <labels_out.txt>
"""
import re
import sys
from collections import Counter

text = open(sys.argv[1]).read()
blocks = text.split("[[switch]]")[1:]
tables = []
labels = set()
for b in blocks:
    base = int(re.search(r"base = 0x([0-9A-F]+)", b).group(1), 16)
    m = re.search(r"# table = 0x([0-9A-F]+), entry size (\d+)", b)
    labs = [int(x, 16) for x in re.findall(r"^\s+0x([0-9A-F]+),", b, re.M)]
    labels.update(labs)
    if m:
        addr, size = int(m.group(1), 16), int(m.group(2))
        tables.append((addr, addr + size * len(labs), base, len(labs)))

print(f"{len(blocks)} tables, {sum(t[3] for t in tables)} cases, {len(labels)} distinct labels")
print("case counts:", sorted(Counter(min(t[3], 64) for t in tables).items())[:12], "...")

tables.sort()
overlaps = 0
for prev, cur in zip(tables, tables[1:]):
    if cur[0] < prev[1]:
        if cur[0] == prev[0]:
            continue  # same table data shared by two jumps (e.g. duplicated code paths)
        overlaps += 1
        print(f"  overlap: table {prev[0]:X} (bctr {prev[2]:X}, {prev[3]} cases) runs into table {cur[0]:X} (bctr {cur[2]:X})")
shared = sum(1 for p, c in zip(tables, tables[1:]) if p[0] == c[0])
print(f"{overlaps} overlapping table ranges, {shared} tables sharing data with another")

with open(sys.argv[2], "w") as f:
    for l in sorted(labels):
        f.write(f"{l:X}\n")
