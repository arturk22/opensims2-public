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
B=open(sys.argv[1],'rb').read()
DATA_VM, DATA_OFF = 0x2778000, 41381888
def f2v(o): return o+0x1000 if o<DATA_OFF else o-DATA_OFF+DATA_VM
def v2f(v): return v-0x1000 if v<DATA_VM else v-DATA_VM+DATA_OFF
def w(v): o=v2f(v); return struct.unpack('>I',B[o:o+4])[0]
def cstr(v):
    o=v2f(v); e=B.index(b'\0',o); return B[o:e].decode()
names={}
for l in open(sys.argv[2]):
    if l[0]=='#': continue
    a,n,c=l.split(); names[int(a,16)]=n
XTI=0x02ab0698
res=[]
for m in re.finditer(re.escape(struct.pack('>I',XTI)),B):
    if m.start()%4 or m.start()<DATA_OFF: continue
    ti=f2v(m.start())-8
    if w(ti)!=8: continue
    nm=cstr(w(ti+4))
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
