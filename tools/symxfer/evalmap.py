#!/usr/bin/env python3
"""Coverage and confidence of a symxfer map, measured independently of how
the names were placed.

* coverage: named FDE functions per section and confidence class;
* thunks: a this-adjusting thunk "addi r3,r3,-k; b T" whose target T is named
  X::f gives the name __ZThn<k>_X::f; that name existing in baserom.sym
  confirms T's name (per confidence class of T);
* vtables: slot k of every vtable descending from the class that introduced
  slot k holds the same method (same name and signature, the class may
  change).  For each slot group with >= 2 distinct named functions the
  majority method wins; a named slot is consistent when it has that method
  and its class is the vtable's class or one of its ancestors (per class).
usage: evalmap.py <sims2_bv.ppc> <baserom.sym> <ours_nm.txt> <map.txt> [map2.txt ...]
"""
import os, struct, sys
from collections import defaultdict, Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import symxfer, rtti, vtnames


def load_map(path):
    m = {}
    for l in open(path):
        if l[0] == '#':
            continue
        p = l.split()
        m[int(p[0], 16)] = (p[1], p[2] if len(p) > 2 else 'native')
    return m


def vtable_check(R, m):
    groups = rtti.slot_members(R)
    res = defaultdict(lambda: [0, 0])
    bad = []
    for g, mem in groups.items():
        byaddr = defaultdict(set)
        for a, t, th in mem:
            byaddr[a].add(t)
        keys = {a: vtnames.method_key(m[a][0]) for a in byaddr if a in m}
        keys = {a: k for a, k in keys.items() if k}
        if len(keys) < 2:
            continue
        top, n = Counter(keys.values()).most_common(1)[0]
        if n < 2:
            continue
        for a, k in keys.items():
            cls = rtti.name_class(m[a][0])
            okc = all(cls == R.ti[t] or cls in {R.ti[x] for x in R.ancestors(t)}
                      for t in byaddr[a])
            ok = k == top and okc
            res[m[a][1]][0 if ok else 1] += 1
            if not ok:
                bad.append((a, m[a][0], m[a][1], ','.join(R.ti[t] for t in byaddr[a]), str(top)))
    return res, bad


def thunk_check(img, m, basenames):
    res = defaultdict(lambda: [0, 0])
    for a, (n, c) in m.items():
        if not (n.startswith('__ZTh') and c.startswith('thunk')):
            continue
        w0, w1 = img.word(a), img.word(a + 4)
        if w1 is None or (w1 >> 26) != 18:
            continue
        off = w1 & 0x3FFFFFC
        if off & 0x2000000:
            off -= 0x4000000
        t = a + 4 + off
        if t in m:
            res[m[t][1]][0 if n in basenames else 1] += 1
    return res


def main():
    binp, symp, nmp = sys.argv[1:4]
    seqs = symxfer.build_ours(binp, nmp)
    base = symxfer.build_base(symp)
    basenames = set(b[1] for b in base)
    img = rtti.Image(binp)
    R = rtti.RTTI(img)
    for mp in sys.argv[4:]:
        m = load_map(mp)
        print('== %s (%d names)' % (mp, len(m)))
        for sn, seq in seqs.items():
            F = [o for o in seq if o[2] == 'F']
            c = Counter(m[o[0]][1] for o in F if o[0] in m)
            ing = sum(1 for a in m if any(o[0] < a < o[0] + o[1] for o in []))
            print('  %-14s %6d/%6d named  %s' % (sn, sum(c.values()), len(F),
                  ' '.join('%s=%d' % kv for kv in sorted(c.items()))))
        gaps = [o for o in seqs['__text'] if o[2] == 'G']
        starts = sorted(m)
        import bisect
        ng = 0
        for o in gaps:
            lo = bisect.bisect_left(starts, o[0]); hi = bisect.bisect_left(starts, o[0] + o[1])
            ng += hi - lo
        print('  names inside FDE-less gaps of __text: %d' % ng)
        th = thunk_check(img, m, basenames)
        print('  thunk-confirmed target names:', ' '.join(
            '%s=%d/%d(%.0f%%)' % (k, v[0], v[0] + v[1], 100.0 * v[0] / max(1, v[0] + v[1]))
            for k, v in sorted(th.items())))
        vt, bad = vtable_check(R, m)
        print('  vtable-consistent slot names:', ' '.join(
            '%s=%d/%d(%.0f%%)' % (k, v[0], v[0] + v[1], 100.0 * v[0] / max(1, v[0] + v[1]))
            for k, v in sorted(vt.items())))
        if os.environ.get('EVAL_BAD'):
            with open(os.environ['EVAL_BAD'], 'w') as f:
                for b in sorted(bad):
                    f.write('%08x %s %s in %s; majority %s\n' % b)


if __name__ == '__main__':
    main()
