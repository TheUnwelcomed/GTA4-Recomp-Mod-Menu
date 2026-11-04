"""Names for the host addresses in the port's crash and stack lines.

The port logs host stacks relative to an anchor (`rex::ps5diag::NowMs`):
`ps5 stack: tid ... rip X | W1 W2 ...` and `[ps5 crash] thread ... (rip X,
anchor-relative Y) | stack: W1 W2 ...`, each word W = address - &NowMs +
0x10000000. With the symbols of the very build that ran (`llvm-nm -n -C
llvm-pie.elf | grep " [tTwW] "`, kept per release), each word names its
function: the recompiled game functions are `sub_8xxxxxxx`, the rest the
port's own. Log 108's fault, for instance, walked up from sub_828C9C68 (a
shader's texture placed) to the streaming's load.

usage: python3 -I hostsym.py <syms.txt> <word> [...]
"""
import bisect
import sys


def main():
    syms = []
    anchor = None
    for line in open(sys.argv[1]):
        p = line.split(None, 2)
        if len(p) != 3:
            continue
        addr, name = int(p[0], 16), p[2].strip()
        syms.append((addr, name))
        if name.startswith('rex::ps5diag::NowMs'):
            anchor = addr
    assert anchor is not None, 'no rex::ps5diag::NowMs in the symbols'
    syms.sort()
    keys = [a for a, _ in syms]
    for tok in sys.argv[2:]:
        a = int(tok, 16) - 0x10000000 + anchor
        i = bisect.bisect_right(keys, a) - 1
        if i < 0:
            print('%s: before the first symbol' % tok)
            continue
        print('%-9s %08X  %s+0x%X' % (tok, a, syms[i][1][:120], a - syms[i][0]))


if __name__ == '__main__':
    main()
