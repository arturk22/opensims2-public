#!/usr/bin/env python3
"""Count where a 32-bit constant is built in PowerPC code (lis + ori/addi on the
same register within 8 instructions), to tell whether a message ID, hash or
CLSID exists in the Mac base 1.0 binary or only in Bon Voyage.

    tools/re/constscan.py <binary.ppc> 0xB1265EF3 [0x...]...

The binaries live outside the repo (references/base_mac, references/bv_mac).
The count is of code sites, not of data words; 0 in the base binary while the
BV binary has hits is a good sign that the feature is from an expansion.
"""
import struct
import sys


def sites(buf, val):
    hi, lo = (val >> 16) & 0xFFFF, val & 0xFFFF
    hi_addi = (hi + 1) & 0xFFFF if lo & 0x8000 else hi  # addi sign-extends lo
    n = 0
    words = struct.unpack('>%dI' % (len(buf) // 4), buf[:len(buf) // 4 * 4])
    for i, w in enumerate(words):
        if w >> 26 != 15 or (w >> 16) & 0x1F != 0:   # lis rT,imm (addis rT,0,imm)
            continue
        imm, rt = w & 0xFFFF, (w >> 21) & 0x1F
        if imm not in (hi, hi_addi):
            continue
        for w2 in words[i + 1:i + 9]:
            op = w2 >> 26
            if (w2 >> 16) & 0x1F != rt or w2 & 0xFFFF != lo:
                continue
            if (op == 24 and imm == hi) or (op == 14 and imm == hi_addi):
                n += 1
                break
    return n


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    buf = open(sys.argv[1], 'rb').read()
    for a in sys.argv[2:]:
        v = int(a, 0) & 0xFFFFFFFF
        print('0x%08X %d' % (v, sites(buf, v)))


if __name__ == '__main__':
    main()
