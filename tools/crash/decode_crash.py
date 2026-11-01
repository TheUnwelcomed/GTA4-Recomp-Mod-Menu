"""Decode a '[ps5 crash]' line (or 'ps5 prof' block addresses) against the
PS5 link of the same build (out/ps5/ps5-link/build/llvm-pie.elf, symbols in).

usage: python3 decode_crash.py <llvm-pie.elf> <hex> [<hex> ...]
Addresses are anchor-relative as the crash handler prints them:
v - &rex::ps5diag::NowMs + 0x10000000.
"""
import bisect
import subprocess
import sys

elf = sys.argv[1]
vals = [int(x, 16) for x in sys.argv[2:]]
out = subprocess.run(['nm', '-C', '--defined-only', elf], capture_output=True, text=True).stdout
syms = []
anchor = None
for line in out.splitlines():
    parts = line.split(' ', 2)
    if len(parts) < 3 or parts[1] not in 'tTwW':
        continue
    a = int(parts[0], 16)
    syms.append((a, parts[2]))
    if parts[2] == 'rex::ps5diag::NowMs()':
        anchor = a
syms.sort()
addrs = [s[0] for s in syms]
for v in vals:
    a = v - 0x10000000 + anchor
    i = bisect.bisect_right(addrs, a) - 1
    name, off = (syms[i][1], a - syms[i][0]) if i >= 0 else ('?', 0)
    print('%08X -> %08X  %s+0x%X' % (v, a, name[:150], off))
