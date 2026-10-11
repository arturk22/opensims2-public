#!/usr/bin/env python3
"""Find the vtables of the SimAntics operand classes (XPrimParam subclasses,
e.g. SetToNextParam) through their RTTI in our ppc binary and print manual
names for their virtuals:
  slot 0x00 Write, 0x04 Read, 0x08 UpgradeVersion, 0x0C GetVersion,
  0x10 SwizzleParams (byte-swaps the operand fields to host order: its body
  gives the exact field layout of each primitive's operands).
Slots were identified from XPrimParam::Write (writes GetVersion(), calls
SwizzleParams, writes 16 bytes, calls SwizzleParams again) and Read.
usage: paramvtables.py <sims2_bv.ppc> <map.txt>  >> manual names
"""
import struct, re, sys

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
B=open(sys.argv[1],'rb').read()
f2v, v2f, DATA_OFF = make_maps(sys.argv[1])
def w(v): o=v2f(v); return struct.unpack('>I',B[o:o+4])[0]
def cstr(v):
    try:
        o=v2f(v); e=B.index(b'\0',o); n=B[o:e].decode()
    except (ValueError, UnicodeDecodeError):
        return None
    return n if re.match(r'^\d+[A-Za-z_]\w*$', n) else None
names={}
for l in open(sys.argv[2]):
    if l[0]=='#': continue
    a,n,c=l.split(); names[int(a,16)]=n
def find_typeinfo(name):
    for m in re.finditer(re.escape(name + b'\0'), B):
        if m.start() and B[m.start() - 1:m.start()].isalnum():
            continue
        sv = f2v(m.start())
        for t in re.finditer(re.escape(struct.pack('>I', sv)), B):
            if t.start() % 4 == 0 and t.start() >= DATA_OFF:
                return f2v(t.start()) - 4
    raise SystemExit('no typeinfo for %r' % name)
XTI = find_typeinfo(b'10XPrimParam')
res=[]
for m in re.finditer(re.escape(struct.pack('>I',XTI)),B):
    if m.start()%4 or m.start()<DATA_OFF: continue
    ti=f2v(m.start())-8
    nm=cstr(w(ti+4))
    if nm is None: continue
    for v in re.finditer(re.escape(struct.pack('>I',ti)),B):
        if v.start()%4 or v.start()<DATA_OFF: continue
        vt=f2v(v.start())
        if w(vt-4)!=0: continue
        slots=[w(vt+4+4*k) for k in range(5)]
        res.append((nm,vt+4,slots))
SLOT = ['5WriteER11cIGZOStream', '4ReadER11cIGZIStreaml', '14UpgradeVersionEv',
        '10GetVersionEv', '13SwizzleParamsEv']
seen = {}
for nm, vp, slots in sorted(res):
    print('# %s vtable (vptr) %#x' % (nm, vp))
    for k, a in enumerate(slots):
        if a in seen:
            continue
        # shared implementations belong to the base class
        owners = [n for n2, v2, s2 in res for n in [n2] if s2[k] == a]
        cls = nm if len(owners) == 1 else '10XPrimParam'
        seen[a] = 1
        if a in names and not names[a].startswith('__ZN10XPrimParam') and cls == '10XPrimParam':
            continue
        print('%08x __ZN%s%s' % (a, cls, SLOT[k]))
