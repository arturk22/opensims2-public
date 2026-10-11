#!/usr/bin/env python3
"""Dump the vtables of classes (by mangled RTTI name, e.g. 9cTSObject) of our
ppc binary with the symxfer names of each slot, to resolve the decompiler's
virtual calls ("(**(code **)(*this + 0x4d4))" -> slot 0x4d4).
Slot offsets are relative to the vptr (the word after the typeinfo pointer),
which is what compiled code adds to *this.
usage: vtables.py <sims2_bv.ppc> <map.txt> <rtti name>... [--slots 0x4d4,...]
"""
import re, struct, subprocess, sys

def segments(path):
    """(vmaddr, vmsize, fileoff) of each segment, from otool -l."""
    import subprocess
    out = subprocess.run(['otool', '-l', path], capture_output=True, text=True).stdout
    segs, cur = [], None
    for line in out.splitlines():
        t = line.split()
        if not t:
            continue
        if t[0] == 'cmd' and t[1] == 'LC_SEGMENT':
            cur = {}
        elif cur is not None and t[0] in ('vmaddr', 'vmsize', 'fileoff'):
            cur[t[0]] = int(t[1], 0)
            if len(cur) == 3:
                segs.append((cur['vmaddr'], cur['vmsize'], cur['fileoff']))
                cur = None
    return segs


def make_maps(path):
    segs = [x for x in segments(path) if x[1]]
    def f2v(o):
        for va, sz, fo in segs:
            if fo <= o < fo + sz and fo + va:
                return o - fo + va
        return o + 0x1000
    def v2f(v):
        for va, sz, fo in segs:
            if va <= v < va + sz:
                return v - va + fo
        raise ValueError(hex(v))
    data_off = [fo for va, sz, fo in segs if fo and va > 0x1000][0]
    return f2v, v2f, data_off

def main():
    args = sys.argv[1:]
    want = None
    if '--slots' in args:
        k = args.index('--slots'); want = set(int(x, 16) for x in args[k + 1].split(',')); del args[k:k + 2]
    binp, mapp, classes = args[0], args[1], args[2:]
    B = open(binp, 'rb').read()
    f2v, v2f, DATA_OFF = make_maps(binp)
    w = lambda v: struct.unpack('>I', B[v2f(v):v2f(v) + 4])[0]
    names = {}
    for l in open(mapp):
        if l[0] != '#':
            a, n, c = l.split(); names[int(a, 16)] = n
    demangle = lambda n: subprocess.run(['c++filt', '-_'], input=n, capture_output=True, text=True).stdout.strip() if n else '?'
    for cls in classes:
        for m in re.finditer(re.escape(cls.encode() + b'\0'), B):
            if m.start() and B[m.start() - 1:m.start()].isalnum():
                continue
            sv = f2v(m.start())
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
