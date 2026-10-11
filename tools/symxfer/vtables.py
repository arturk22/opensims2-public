#!/usr/bin/env python3
"""Dump the vtables of classes (by mangled RTTI name, e.g. 9cTSObject) of our
ppc binary with the symxfer names of each slot, to resolve the decompiler's
virtual calls ("(**(code **)(*this + 0x4d4))" -> slot 0x4d4).
Slot offsets are relative to the vptr (the word after the typeinfo pointer),
which is what compiled code adds to *this.
usage: vtables.py <sims2_bv.ppc> <map.txt> <rtti name>... [--slots 0x4d4,...]
"""
import re, struct, subprocess, sys

def main():
    args = sys.argv[1:]
    want = None
    if '--slots' in args:
        k = args.index('--slots'); want = set(int(x, 16) for x in args[k + 1].split(',')); del args[k:k + 2]
    binp, mapp, classes = args[0], args[1], args[2:]
    B = open(binp, 'rb').read()
    DATA_VM, DATA_OFF = 0x2778000, 41381888
    f2v = lambda o: o + 0x1000 if o < DATA_OFF else o - DATA_OFF + DATA_VM
    v2f = lambda v: v - 0x1000 if v < DATA_VM else v - DATA_VM + DATA_OFF
    w = lambda v: struct.unpack('>I', B[v2f(v):v2f(v) + 4])[0]
    names = {}
    for l in open(mapp):
        if l[0] != '#':
            a, n, c = l.split(); names[int(a, 16)] = n
    demangle = lambda n: subprocess.run(['c++filt', '-_'], input=n, capture_output=True, text=True).stdout.strip() if n else '?'
    for cls in classes:
        for m in re.finditer(re.escape(b'\0' + cls.encode() + b'\0'), B):
            sv = f2v(m.start() + 1)
            for t in re.finditer(re.escape(struct.pack('>I', sv)), B):
                if t.start() % 4 or t.start() < DATA_OFF:
                    continue
                ti = f2v(t.start()) - 4
                for v in re.finditer(re.escape(struct.pack('>I', ti)), B):
                    if v.start() % 4 or v.start() < DATA_OFF:
                        continue
                    vp = f2v(v.start()) + 4
                    top = struct.unpack('>i', B[v.start() - 4:v.start()])[0]
                    print('# %s vptr %#x offset-to-top %d' % (cls, vp, top))
                    k = 0
                    while True:
                        a = w(vp + 4 * k)
                        if not (0x2000 <= a < 0x1ccde4c):
                            break
                        if want is None or 4 * k in want:
                            print('  %#05x %08x %s' % (4 * k, a, demangle(names.get(a))))
                        k += 1

main()
