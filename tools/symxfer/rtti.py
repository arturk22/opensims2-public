#!/usr/bin/env python3
"""RTTI and vtables of our ppc binary (Apple gcc 4, Itanium C++ ABI).

typeinfo object:  [vptr of __cxxabiv1::*_class_type_info][name ptr][...]
  __si_class_type_info:  [base typeinfo]
  __vmi_class_type_info: [flags][n bases] n x [base typeinfo][offset<<8|flags]
vtable:           [offset-to-top][typeinfo ptr][slot 0][slot 1]...
                                                ^ vptr (what objects point to)

Everything is found from the data alone: a typeinfo is a __DATA word pair
whose second word points at a mangled class name (a C string in __TEXT that
the demangler accepts as a type); a vtable is a word equal to a typeinfo
address preceded by a small offset-to-top, followed by code pointers.

Used by symxfer.py (vtable naming and the consistency metric) and on its own:
  rtti.py <sims2_bv.ppc> [map.txt] [--class 9cTSObject]   dump vtables
"""
import re, struct, subprocess, sys
from collections import defaultdict


def segments(path):
    out = subprocess.run(['otool', '-l', path], capture_output=True, text=True).stdout
    segs, cur = [], None
    for line in out.splitlines():
        t = line.split()
        if not t:
            continue
        if t[0] == 'cmd' and t[1] == 'LC_SEGMENT':
            cur = {}
        elif cur is not None and t[0] in ('segname', 'vmaddr', 'vmsize', 'fileoff'):
            cur[t[0]] = t[1] if t[0] == 'segname' else int(t[1], 0)
            if len(cur) == 4:
                segs.append(cur)
                cur = None
    return segs


class Image:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        self.segs = {s['segname']: s for s in segments(path)}
        t, d = self.segs['__TEXT'], self.segs['__DATA']
        self.text = (t['vmaddr'], t['vmaddr'] + t['vmsize'])
        self.dseg = (d['vmaddr'], d['vmaddr'] + d['vmsize'], d['fileoff'])
        # file-backed part of __DATA only
        self.dfile_end = d['vmaddr'] + min(d['vmsize'], len(self.data) - d['fileoff'])

    def off(self, v):
        for s in self.segs.values():
            if s['vmaddr'] <= v < s['vmaddr'] + s['vmsize'] and s['segname'] != '__PAGEZERO':
                return v - s['vmaddr'] + s['fileoff']
        return None

    def word(self, v):
        o = self.off(v)
        if o is None or o + 4 > len(self.data):
            return None
        return struct.unpack('>I', self.data[o:o + 4])[0]

    def cstr(self, v, maxlen=400):
        o = self.off(v)
        if o is None:
            return None
        e = self.data.find(b'\0', o, o + maxlen)
        if e < 0:
            return None
        return self.data[o:e]

    def data_words(self):
        lo, hi, fo = self.dseg
        n = (self.dfile_end - lo) // 4
        return lo, struct.unpack('>%dI' % n, self.data[fo:fo + 4 * n])


TYPE_RE = re.compile(rb'^(?:N(?:[0-9]+[A-Za-z_][A-Za-z0-9_]*|S[a-z]|St)+.*E|[0-9]+[A-Za-z_][A-Za-z0-9_]*.*)$')


def is_type_name(s):
    if not s or len(s) < 2 or not TYPE_RE.match(s):
        return False
    if s[0:1].isdigit():
        m = re.match(rb'^([0-9]+)', s)
        n = int(m.group(1))
        return len(s) >= len(m.group(1)) + n
    return True


class RTTI:
    """typeinfos {addr: name}, bases {addr: [base addrs]}, vtables
    {vptr: (class typeinfo, offset_to_top, [slot addrs])}."""

    def __init__(self, img):
        self.img = img
        lo, W = img.data_words()
        t0, t1 = img.text
        self.ti = {}
        namecache = {}
        for k in range(len(W) - 1):
            p = W[k + 1]
            if not (t0 <= p < t1):
                continue
            if p not in namecache:
                s = img.cstr(p)
                namecache[p] = s.decode('latin1') if is_type_name(s) else None
            if namecache[p]:
                # the first word is the class_type_info vptr: 0 (bound by dyld)
                # or a pointer outside __TEXT; never a text pointer
                if t0 <= W[k] < t1:
                    continue
                self.ti[lo + 4 * k] = namecache[p]
        tiset = self.ti
        self.bases = {}
        self.base_off = {}   # (ti, base ti) -> offset of the base subobject
        for a in tiset:
            k = (a - lo) // 4
            w2 = W[k + 2] if k + 2 < len(W) else None
            if w2 in tiset:
                self.bases[a] = [w2]
                self.base_off[(a, w2)] = 0
            elif w2 is not None and w2 <= 0x3 and k + 4 < len(W) and 1 <= W[k + 3] <= 16 \
                    and W[k + 4] in tiset:
                n = W[k + 3]
                bl = []
                for b in range(n):
                    bt, of = W[k + 4 + 2 * b], W[k + 5 + 2 * b]
                    if bt in tiset:
                        bl.append(bt)
                        if not of & 1:      # not virtual
                            o8 = of >> 8
                            if o8 >= 0x800000:
                                o8 -= 1 << 24
                            self.base_off[(a, bt)] = o8
                self.bases[a] = bl
            else:
                self.bases[a] = []
        self.vt = {}
        for k in range(1, len(W) - 1):
            if W[k] not in tiset:
                continue
            top = W[k - 1]
            if top >= 0x80000000:
                top -= 1 << 32
            if not (-0x4000 <= top <= 0):
                continue
            vp = lo + 4 * (k + 1)
            if vp in tiset:
                continue
            slots = []
            j = k + 1
            while j < len(W):
                a = W[j]
                if t0 <= a < t1 and a % 4 == 0:
                    slots.append(a)
                    j += 1
                elif a == 0 and slots == [] and False:
                    j += 1
                else:
                    break
            if slots:
                self.vt[vp] = (W[k], top, slots)
        self.name2ti = {}
        for a, n in self.ti.items():
            self.name2ti.setdefault(n, a)

    def primary_vtable(self, ti):
        best = None
        for vp, (t, top, slots) in self.vt.items():
            if t == ti and top == 0:
                if best is None or len(slots) > len(self.vt[best][2]):
                    best = vp
        return best

    def ancestors(self, ti):
        out, todo = set(), [ti]
        while todo:
            x = todo.pop()
            for b in self.bases.get(x, ()):
                if b not in out:
                    out.add(b)
                    todo.append(b)
        return out


def thunk_target(img, a):
    """target of a this-adjusting thunk "addi r3,r3,-k; b T" (or None)"""
    w0, w1 = img.word(a), img.word(a + 4)
    if w0 is None or w1 is None or (w0 >> 16) != 0x3863 or (w1 >> 26) != 18 or (w1 & 3):
        return None
    off = w1 & 0x3FFFFFC
    if off & 0x2000000:
        off -= 0x4000000
    return a + 4 + off


def slot_members(R):
    """Group every vtable slot by the slot it overrides: group = (typeinfo of
    the class that introduced the slot, slot index).  All functions of one
    group implement the same virtual method (same name and signature).
    Returns {group: set((function addr, typeinfo of the class whose vtable
    holds it, via_thunk))}.  Secondary vtables (a non-primary base at offset
    -top) hold thunks to the derived class' overrides: the thunk's target is
    the member, with the derived class."""
    prim = {}
    for vp, (t, top, slots) in R.vt.items():
        if top == 0 and (t not in prim or len(slots) > len(R.vt[prim[t]][2])):
            prim[t] = vp

    def chain_of(t):
        ch, seen = [t], {t}
        while R.bases.get(ch[-1]):
            b = R.bases[ch[-1]][0]
            if b in seen or b not in prim or R.base_off.get((ch[-1], b), 0) != 0:
                break
            ch.append(b); seen.add(b)
        return ch

    def intro(t, k):
        it = t
        for c in chain_of(t):
            if len(R.vt[prim[c]][2]) > k:
                it = c
        return it

    groups = defaultdict(set)
    for vp, (t, top, slots) in R.vt.items():
        if top == 0:
            if prim.get(t) != vp:
                continue
            for k, a in enumerate(slots):
                groups[(intro(t, k), k)].add((a, t, False))
        else:
            bs = [b for b in R.bases.get(t, ()) if R.base_off.get((t, b)) == -top]
            if len(bs) != 1 or bs[0] not in prim:
                continue
            b = bs[0]
            nb = len(R.vt[prim[b]][2])
            for k, a in enumerate(slots[:nb]):
                tg = thunk_target(R.img, a)
                if tg is not None:
                    groups[(intro(b, k), k)].add((tg, t, True))
                else:
                    groups[(intro(b, k), k)].add((a, b, False))
    return groups


# --- mangled-name helpers (enough for member functions) ----------------------

def parse_nested(name):
    """'__ZNK3nsA4cFoo3barEi' -> (prefix '__ZNK', ['3nsA', '4cFoo'], last, sig)
    where last is the method's source name ('3bar') or a structor code
    ('C1', 'D0', ...).  None for anything else (templates, operators, ...)."""
    if name.startswith('__ZNK'):
        pre, i = '__ZNK', 5
    elif name.startswith('__ZN'):
        pre, i = '__ZN', 4
    else:
        return None
    parts = []
    while i < len(name):
        c = name[i]
        if c.isdigit():
            j = i
            while name[j].isdigit():
                j += 1
            L = int(name[i:j])
            if j + L > len(name):
                return None
            parts.append(name[i:j + L]); i = j + L
        elif c == 'S' and i + 1 < len(name) and name[i + 1] in 'tabsiod':
            parts.append(name[i:i + 2]); i += 2
        elif name[i:i + 2] in ('C1', 'C2', 'D0', 'D1', 'D2'):
            parts.append(name[i:i + 2]); i += 2
        elif c == 'E':
            if len(parts) < 2:
                return None
            return pre, parts[:-1], parts[-1], name[i + 1:]
        else:
            return None
    return None


def _cname(cls):
    return cls[0] if len(cls) == 1 and cls[0][0].isdigit() else 'N' + ''.join(cls) + 'E'


def split_member(name):
    """'__ZN3nsA4cFoo3barEi' -> ('N3nsA4cFooE' class type name, '3bar', 'i',
    prefix '__ZN' or '__ZNK').  None if not a plain member function."""
    p = parse_nested(name)
    if not p or not p[2][0].isdigit():
        return None
    return _cname(p[1]), p[2], p[3], p[0]


def split_structor(name):
    p = parse_nested(name)
    if not p or p[2][0] not in 'CD' or p[0] != '__ZN':
        return None
    return _cname(p[1]), p[2], p[3]


def class_quals(cname):
    """type name -> the qualifier string used inside __ZN...E"""
    if cname.startswith('N') and cname.endswith('E'):
        return cname[1:-1]
    return cname


def member_name(cname, meth, sig, pre='__ZN'):
    return '%s%s%sE%s' % (pre, class_quals(cname), meth, sig)


def structor_name(cname, kind, sig):
    return '__ZN%s%sE%s' % (class_quals(cname), kind, sig)


def name_class(name):
    """class type name of a member function / structor / thunk name"""
    if name.startswith('__ZTh') or name.startswith('__ZTv'):
        m = re.match(r'^__ZT[hv]n?[0-9]+_(?:n?[0-9]+_)?(.*)$', name)
        if not m:
            return None
        name = '__Z' + m.group(1)
    s = split_structor(name)
    if s:
        return s[0]
    s = split_member(name)
    return s[0] if s else None


def main():
    args = sys.argv[1:]
    cls = None
    if '--class' in args:
        k = args.index('--class'); cls = args[k + 1]; del args[k:k + 2]
    img = Image(args[0])
    names = {}
    if len(args) > 1:
        for l in open(args[1]):
            if l[0] != '#':
                p = l.split()
                names[int(p[0], 16)] = p[1]
    r = RTTI(img)
    print('# %d typeinfos, %d vtables' % (len(r.ti), len(r.vt)), file=sys.stderr)
    for vp in sorted(r.vt):
        t, top, slots = r.vt[vp]
        if cls and r.ti[t] != cls:
            continue
        print('# %s vptr %#x top %d bases %s' % (r.ti[t], vp, top,
              ','.join(r.ti[b] for b in r.bases.get(t, []))))
        for k, a in enumerate(slots):
            print('  %#05x %08x %s' % (4 * k, a, names.get(a, '?')))


if __name__ == '__main__':
    main()
