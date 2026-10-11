#!/usr/bin/env python3
"""Transfer function names from baserom.sym (sims2_mac_decomp, a different
PowerPC build of the Mac Bon Voyage port) onto our ppc slice of the Aspyr
Mac Bon Voyage 1.10 executable.

Why this works
--------------
* Our binary keeps only external/weak symbols (all in __textcoal_nt); the
  25 MB of __text has no names.  It does keep __eh_frame: one FDE per
  function, giving exact function starts and sizes (dwarfdump --eh-frame).
* baserom.sym is a full nm listing (82k 'T' symbols) of a build compiled
  from the same sources by the same compiler, but linked differently: weak /
  inline functions are interleaved with their object file instead of being
  collected in __textcoal_nt, and object files are in a different order.
* So, per section, our function size sequence and baserom's size sequence
  share long runs of identical sizes, in a block-permuted order.

Algorithm
---------
1. Our functions: FDEs per section (__text, __textcoal_nt, __StaticInit);
   holes between FDEs become 'gap' entries (code without unwind info).
   Our own nm names are attached to coal entries.
2. baserom functions: sorted 'T' symbols, size = distance to the next symbol
   (an upper bound when baserom has unnamed code after it).
3. Anchors: (a) exact shared names (coal); (b) K-grams (K=4) of consecutive
   exact sizes unique in both sequences.
4. Anchors are chained (both indices increasing, bounded jumps); between
   consecutive anchors of a chain a small DP aligns the segments, allowing
   skipped baserom entries (functions that live in another section of ours),
   skipped ours entries (unnamed), a baserom entry covering several of our
   functions (baserom size = sum, i.e. unnamed baserom code follows), and
   several baserom entries filling one of our FDE-less gaps.
5. Output: "addr name confidence" lines.  Confidence:
     native  - name from our own nm
     exact   - aligned inside a chain, sizes equal
     (sum)   - aligned inside a chain, baserom size = sum of ours; used only
               to steer the alignment, never written (thunk check: 9%)
     near    - aligned inside a chain, sizes within max(16, 1/12) bytes
     gap     - baserom functions laid out inside an FDE-less gap, sizes fit
     moved   - unassigned near a chain, exact size unique on both sides
               (functions reordered inside their object file)
     thunk   - this-adjusting thunk named after its target; the derived
               name also exists in baserom (confirms the target)
     thunk?  - same, derived name not in baserom
Usage:
  symxfer.py <ours.ppc> <baserom.sym> <ours_nm.txt> -o map.txt [--stats]
"""
import argparse, bisect, re, subprocess, sys
from collections import defaultdict

K = 4
TINY = 32


def macho_sections(path):
    out = subprocess.run(['otool', '-l', path], capture_output=True, text=True).stdout
    secs, cur = [], {}
    for line in out.splitlines():
        line = line.strip()
        if line.startswith('sectname'):
            cur = {'name': line.split()[1]}
        elif line.startswith('segname') and cur and 'seg' not in cur:
            cur['seg'] = line.split()[1]
        elif line.startswith('addr') and cur and 'addr' not in cur:
            cur['addr'] = int(line.split()[1], 16)
        elif line.startswith('size') and cur and 'size' not in cur:
            cur['size'] = int(line.split()[1], 16)
            secs.append(cur)
            cur = {}
    return secs


def read_fdes(path):
    out = subprocess.run(['dwarfdump', '--eh-frame', path], capture_output=True,
                         text=True).stdout
    res = []
    for m in re.finditer(r'FDE cie=[0-9a-f]+ pc=([0-9a-f]+)\.\.\.([0-9a-f]+)', out):
        a, b = int(m.group(1), 16), int(m.group(2), 16)
        res.append((a, b - a))
    return sorted(set(res))


def build_ours(binary, nmfile):
    secs = {s['name']: s for s in macho_sections(binary) if s.get('seg') == '__TEXT'}
    fdes = read_fdes(binary)
    names = {}
    for line in open(nmfile):
        p = line.split()
        if len(p) == 2:
            names.setdefault(int(p[0], 16), p[1])
    seqs = {}
    for sn in ('__text', '__textcoal_nt', '__StaticInit'):
        s = secs[sn]
        lo, hi = s['addr'], s['addr'] + s['size']
        fs = [f for f in fdes if lo <= f[0] < hi]
        seq, pos = [], lo
        for a, sz in fs:
            if a > pos:
                seq.append([pos, a - pos, 'G', names.get(pos)])
            seq.append([a, sz, 'F', names.get(a)])
            pos = max(pos, a + sz)
        if pos < hi:
            seq.append([pos, hi - pos, 'G', names.get(pos)])
        seqs[sn] = seq
    return seqs


def build_base(symfile):
    base = []
    for line in open(symfile):
        p = line.split()
        if len(p) == 3 and p[1] == 'T':
            base.append([int(p[0], 16), p[2]])
    base.sort()
    # drop aliases (same address)
    uniq = []
    for e in base:
        if uniq and uniq[-1][0] == e[0]:
            continue
        uniq.append(e)
    for e, nx in zip(uniq, uniq[1:] + [[uniq[-1][0] + 0x80, '']]):
        e.append(nx[0] - e[0])
    return uniq  # [addr, name, sizeUB]


def anchors_for(seq, base):
    """Return list of (j, i, length) anchors."""
    anc = []
    # name anchors
    pos_by_name = {}
    for j, b in enumerate(base):
        pos_by_name[b[1]] = j
    for i, o in enumerate(seq):
        if o[3] and o[3] in pos_by_name:
            anc.append((pos_by_name[o[3]], i, 1))
    # size K-gram anchors (only F entries, sizes >= 8)
    idx = defaultdict(list)
    for i in range(len(seq) - K + 1):
        w = seq[i:i + K]
        if all(x[2] == 'F' for x in w):
            idx[tuple(x[1] for x in w)].append(i)
    bidx = defaultdict(list)
    for j in range(len(base) - K + 1):
        bidx[tuple(x[2] for x in base[j:j + K])].append(j)
    for key, il in idx.items():
        if len(il) == 1 and len(bidx.get(key, ())) == 1:
            if sum(key) < 64:      # tiny functions are too common
                continue
            anc.append((bidx[key][0], il[0], K))
    anc.sort()
    return anc


def chain(anc, maxjump=400):
    """Greedy chaining on j-sorted anchors: keep anchors that continue an
    existing chain (i increasing, both jumps bounded). Returns chains."""
    chains = []
    tails = []  # (last_j, last_i, chain_idx), small list of open chains
    for a in anc:
        j, i, ln = a
        best = None
        for t in range(len(tails) - 1, max(-1, len(tails) - 50), -1):
            lj, li, ci = tails[t]
            if lj <= j and li <= i and j - lj <= maxjump and i - li <= maxjump:
                # prefer the one whose diagonal is closest
                d = abs((i - li) - (j - lj))
                if best is None or d < best[0]:
                    best = (d, t)
        if best is not None:
            t = best[1]
            ci = tails[t][2]
            chains[ci].append(a)
            tails[t] = (j + ln - 1, i + ln - 1, ci)
        else:
            chains.append([a])
            tails.append((j + ln - 1, i + ln - 1, len(chains) - 1))
        if len(tails) > 400:
            tails = tails[-200:]
    return chains


def near(a, b):
    return abs(a - b) <= max(16, b // 12)


def align_segment(O, B, local=False):
    """DP alignment of our entries O against baserom entries B.
    Returns list of (o_index, b_index, kind) assignments (relative).
    local=True: the alignment may stop anywhere (chain extension)."""
    n, m = len(O), len(B)
    if n == 0 or m == 0:
        return []
    if n * m > 250000:
        return None
    NEG = -10 ** 9
    S = [[NEG] * (m + 1) for _ in range(n + 1)]
    P = [[None] * (m + 1) for _ in range(n + 1)]
    S[0][0] = 0
    for i in range(n + 1):
        for j in range(m + 1):
            cur = S[i][j]
            if cur == NEG:
                continue
            # skip ours
            if i < n and cur - 2 > S[i + 1][j]:
                S[i + 1][j] = cur - 2; P[i + 1][j] = (i, j, 'so')
            # skip baserom (cheap: lives in another section of ours)
            if j < m and cur - 1 > S[i][j + 1]:
                S[i][j + 1] = cur - 1; P[i][j + 1] = (i, j, 'sb')
            if i < n and j < m:
                o, b = O[i], B[j]
                if o[2] == 'F':
                    if o[1] == b[2]:
                        v = cur + 4
                        if v > S[i + 1][j + 1]:
                            S[i + 1][j + 1] = v; P[i + 1][j + 1] = (i, j, 'exact')
                    elif near(o[1], b[2]):
                        v = cur + 1
                        if v > S[i + 1][j + 1]:
                            S[i + 1][j + 1] = v; P[i + 1][j + 1] = (i, j, 'near')
                    if b[2] > o[1]:
                        # baserom fn followed by unnamed code: covers k of ours
                        tot = 0
                        for k in range(i, min(n, i + 12)):
                            tot += O[k][1]
                            if tot == b[2]:
                                v = cur + 2
                                if v > S[k + 1][j + 1]:
                                    S[k + 1][j + 1] = v; P[k + 1][j + 1] = (i, j, 'sum')
                                break
                            if tot > b[2]:
                                break
                else:  # gap: several baserom fns fill it exactly
                    tot = 0
                    for k in range(j, min(m, j + 40)):
                        tot += B[k][2]
                        if tot == o[1]:
                            v = cur + 2 * (k - j + 1)
                            if v > S[i + 1][k + 1]:
                                S[i + 1][k + 1] = v; P[i + 1][k + 1] = (i, j, 'gap')
                            break
                        if tot > o[1]:
                            break
    # traceback from best end (must consume everything; we allow trailing skips)
    i, j = n, m
    if local:
        best = max((S[a][b], a, b) for a in range(n + 1) for b in range(m + 1))
        i, j = best[1], best[2]
    res = []
    while (i, j) != (0, 0):
        pi, pj, kind = P[i][j]
        if kind in ('exact', 'sum', 'near'):
            res.append((pi, pj, kind))
        elif kind == 'gap':
            off = 0
            for k in range(pj, j):
                res.append((pi, k, 'gap', off))
                off += B[k][2]
        i, j = pi, pj
    return res


def transfer(seq, base, stats, label):
    anc = anchors_for(seq, base)
    chains = [c for c in chain(anc) if len(c) >= 2 or (len(c) == 1 and c[0][2] == 1)]
    out = {}  # our addr -> (name, conf)

    RANK = {'native': 0, 'exact': 1, 'sum': 2, 'near': 3, 'gap': 4, 'moved': 4}

    def put(addr, name, conf, ext=False):
        r = RANK[conf] + (5 if ext else 0)
        if addr not in out or r < out[addr][2]:
            out[addr] = (name, conf, r)

    EXT = 40
    for c in chains:
        # extend after the last anchor / before the first one
        j, i, ln = c[-1]
        O = seq[i + ln:i + ln + EXT]; B = base[j + ln:j + ln + EXT * 2]
        for t in align_segment(O, B, local=True) or []:
            if t[2] == 'gap':
                put(O[t[0]][0] + t[3], B[t[1]][1], 'gap', True)
            elif O[t[0]][1] >= TINY:
                put(O[t[0]][0], B[t[1]][1], t[2], True)
        j, i, ln = c[0]
        O = seq[max(0, i - EXT):i][::-1]; B = base[max(0, j - EXT * 2):j][::-1]
        for t in align_segment(O, B, local=True) or []:
            if t[2] in ('exact', 'near') and O[t[0]][1] >= TINY:   # 'sum' would name the wrong end
                put(O[t[0]][0], B[t[1]][1], t[2], True)
    for c in chains:
        for a in c:
            j, i, ln = a
            for k in range(ln):
                o, b = seq[i + k], base[j + k]
                conf = 'native' if o[3] == b[1] else 'exact'
                put(o[0], b[1], conf)
        for a1, a2 in zip(c, c[1:]):
            j1, i1, l1 = a1
            j2, i2, _ = a2
            O = seq[i1 + l1:i2]
            B = base[j1 + l1:j2]
            r = align_segment(O, B)
            if r is None:
                stats['toolong'] += 1
                continue
            # runs of tiny equal-size functions (thunks, accessors) are only
            # trusted when the segment aligns one-to-one without skips
            clean = len(O) == len(B) == len(r) and all(t[2] in ('exact', 'near') for t in r)
            for t in r:
                if t[2] == 'gap':
                    put(O[t[0]][0] + t[3], B[t[1]][1], 'gap')
                elif clean or O[t[0]][1] >= TINY:
                    put(O[t[0]][0], B[t[1]][1], t[2])
    # functions moved inside their object file: unassigned entries around a
    # chain whose exact size is unique on both sides of the window
    used = set(v[0] for v in out.values())
    W = 60
    for c in chains:
        if len(c) < 3:
            continue
        jlo, jhi = max(0, c[0][0] - W), c[-1][0] + W
        ilo, ihi = max(0, c[0][1] - W), c[-1][1] + W
        bs = defaultdict(list)
        for j in range(jlo, min(len(base), jhi)):
            if base[j][1] not in used:
                bs[base[j][2]].append(j)
        os_ = defaultdict(list)
        for i in range(ilo, min(len(seq), ihi)):
            if seq[i][2] == 'F' and seq[i][0] not in out:
                os_[seq[i][1]].append(i)
        for sz, jl in bs.items():
            if sz >= 96 and len(jl) == 1 and len(os_.get(sz, ())) == 1:
                i = os_[sz][0]
                put(seq[i][0], base[jl[0]][1], 'moved')
                used.add(base[jl[0]][1])
    # native names always win
    for o in seq:
        if o[3]:
            out[o[0]] = (o[3], 'native', 0)
    nF = sum(1 for o in seq if o[2] == 'F')
    named = sum(1 for o in seq if o[2] == 'F' and o[0] in out)
    stats[label] = (nF, named, len(chains), len(anc))
    return {a: v[:2] for a, v in out.items()}


def fix_thunks(binary, allout, basenames, stats, funcs):
    """Name this-adjusting thunks ("addi r3,r3,-k; b T") after their target:
    runs of 8-byte thunks cannot be aligned by size, but their targets can.
    The derived thunk name existing in baserom independently confirms the
    target's name (stats: thunk_confirmed / thunk_unconfirmed)."""
    import struct
    data = open(binary, 'rb').read()
    TEXT_VM = 0x1000  # __TEXT vmaddr, fileoff 0

    def word(a):
        o = a - TEXT_VM
        return struct.unpack('>I', data[o:o + 4])[0]
    # baserom thunk names placed by size alignment are unreliable: drop them
    for addr in list(allout):
        name, conf = allout[addr]
        if conf != 'native' and (name.startswith('__ZTh') or name.startswith('__ZTv')):
            del allout[addr]
            stats['thunk_dropped'] += 1
    for addr in funcs:
        if addr in allout and allout[addr][1] == 'native':
            continue
        w0, w1 = word(addr), word(addr + 4)
        if (w0 >> 16) != 0x3863 or (w1 >> 26) != 18 or (w1 & 3):
            continue
        k = -((w0 & 0xFFFF) - 0x10000 if w0 & 0x8000 else (w0 & 0xFFFF))
        off = w1 & 0x3FFFFFC
        if off & 0x2000000:
            off -= 0x4000000
        tgt = addr + 4 + off
        if tgt not in allout or not allout[tgt][0].startswith('__ZN'):
            continue
        tname = allout[tgt][0]
        new = '__ZThn%d_%s' % (k, tname[3:])
        if new in basenames:
            allout[addr] = (new, 'thunk')
            stats['thunk_confirmed'] += 1
            stats['thunk_confirmed_' + allout[tgt][1]] += 1
        else:
            allout[addr] = (new, 'thunk?')
            stats['thunk_unconfirmed'] += 1
            stats['thunk_unconfirmed_' + allout[tgt][1]] += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary')
    ap.add_argument('basesym')
    ap.add_argument('nm')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--manual', help='addr name overrides (manual.txt)')
    a = ap.parse_args()
    seqs = build_ours(a.binary, a.nm)
    base = build_base(a.basesym)
    stats = defaultdict(int)
    allout = {}
    coalnames = set(o[3] for o in seqs['__textcoal_nt'] if o[3])
    for sn, seq in seqs.items():
        b = base if sn != '__text' else [e for e in base if e[1] not in coalnames]
        for addr, v in transfer(seq, b, stats, sn).items():
            # 'sum' names were confirmed by thunks only 9% of the time: drop
            if addr not in allout and v[1] != 'sum':
                allout[addr] = v
    fix_thunks(a.binary, allout, set(e[1] for e in base), stats,
               [o[0] for sq in seqs.values() for o in sq if o[2] == 'F'])
    if a.manual:
        for line in open(a.manual):
            p = line.split()
            if len(p) >= 2 and not line.startswith('#'):
                addr = int(p[0], 16)
                for k in [k for k, v in allout.items() if v[0] == p[1]]:
                    del allout[k]
                allout[addr] = (p[1], 'manual')
    # a baserom name used twice -> keep the 'better' one only
    rank = {'manual': -1, 'native': 0, 'exact': 1, 'thunk': 1, 'sum': 2, 'near': 3, 'gap': 4, 'moved': 5, 'thunk?': 6}
    best = {}
    for addr, (name, conf) in allout.items():
        if name not in best or rank[conf] < rank[allout[best[name]][1]]:
            best[name] = addr
    with open(a.out, 'w') as f:
        f.write('# addr name confidence  (symxfer: baserom.sym -> Mac BV 1.10 ppc)\n')
        for addr in sorted(allout):
            name, conf = allout[addr]
            if best[name] != addr:
                continue
            f.write('%08x %s %s\n' % (addr, name, conf))
    for k, v in stats.items():
        print(k, v, file=sys.stderr)


if __name__ == '__main__':
    main()
