"""Search the decrypted image for printable strings containing any keyword.

usage: python -I find_strings.py <image.bin> <keyword> [keyword ...]
Prints the guest address (image base 0x82000000) and the string.
"""
import re
import sys

data = open(sys.argv[1], "rb").read()
keywords = [k.lower() for k in sys.argv[2:]]
BASE = 0x82000000
seen = set()
for m in re.finditer(rb"[\x20-\x7e]{4,}", data):
    s = m.group().decode()
    low = s.lower()
    if any(k in low for k in keywords) and s not in seen:
        seen.add(s)
        print(f"{BASE + m.start():08X}  {s[:110]}")
print(f"{len(seen)} matches", file=sys.stderr)
