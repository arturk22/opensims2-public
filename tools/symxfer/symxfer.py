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


WEAK = ('near', 'moved', 'gap', 'fillnear', 'between')


def refill(seq, base, allout, stats, label, kgrams=None):
    """Second-level alignment over the names already placed (all classes,
    including vtable-derived ones): chain them as single-entry anchors,
    together with the unique size K-grams (kgrams, from anchors_for), keep
    chains worth >= 3 (a strong name 1, a weak 'exact' 0.5, a K-gram whose
    sizes are all >= 64 and add up to >= 800 bytes 3) and align every
    stretch of our entries between two consecutive chain anchors against
    the baserom entries between the same two anchors, then extend each
    chain at both ends.  This reaches regions where the K-gram chains were
    too short to be kept (one 4-size anchor) or too far apart (DP too
    large), and code that differs a little between the two builds.
    Unnamed entries get the aligned name (fill / fillnear); a weak name
    (near, moved) loses to an exact-size fill."""
    pos = {}
    for j, b in enumerate(base):
        pos.setdefault(b[1], j)
    anc = []
    wt = {}
    for i, o in enumerate(seq):
        v = allout.get(o[0])
        if v and o[2] == 'F' and v[0] in pos and v[1] not in WEAK:
            a = (pos[v[0]], i, 1)
            anc.append(a)
            wt[a] = 1 if v[1] in ('native', 'manual', 'vtable', 'thunk') else 0.5
    for a in kgrams or ():
        j, i, ln = a
        if ln > 1:
            szs = [seq[i + k][1] for k in range(ln)]
            wt[a] = 3 if min(szs) >= 64 and sum(szs) >= 800 else 0.5
            anc.append(a)
    anc = sorted(set(anc))
    chains = chain(anc, maxjump=300)
    added = [0]

    def put(o, b, kind):
        if kind not in ('exact', 'near'):
            return
        conf = 'fill' if kind == 'exact' else 'fillnear'
        cur = allout.get(o[0])
        if cur is None:
            allout[o[0]] = (b[1], conf); added[0] += 1
            stats[label + '_refill_' + conf] += 1
        elif cur[0] != b[1] and cur[1] in ('near', 'moved') and conf == 'fill':
            allout[o[0]] = (b[1], conf)
            stats[label + '_refill_replaced_' + cur[1]] += 1

    def unnamed_in(O):
        return any(o[0] not in allout or allout[o[0]][1] in WEAK for o in O if o[2] == 'F')
    EXT = 30
    for c in chains:
        if sum(wt[a] for a in c) < 3:
            continue
        for a in c:          # the K-grams themselves
            j, i, ln = a
            if ln > 1:
                for k in range(ln):
                    if seq[i + k][0] not in allout:
                        allout[seq[i + k][0]] = (base[j + k][1], 'fill'); added[0] += 1
                        stats[label + '_refill_kgram'] += 1
        for a1, a2 in zip(c, c[1:]):
            j1, i1, l1 = a1
            j2, i2, _ = a2
            O = seq[i1 + l1:i2]
            B = base[j1 + l1:j2]
            if not O or not B:
                continue
            # a weak name that is not between the two anchors in baserom is
            # out of order: drop it and let the alignment name the entry
            tight = len(O) <= 40 and len(B) <= 2 * len(O) + 10
            for o in O if tight else ():
                v = allout.get(o[0])
                if v and v[1] in ('near', 'moved') and not (j1 < pos.get(v[0], -1) < j2):
                    del allout[o[0]]
                    stats[label + '_refill_dropped_' + v[1]] += 1
            if not unnamed_in(O):
                continue
            r = align_segment(O, B)
            if r is None:
                stats[label + '_refill_toolong'] += 1
                continue
            clean = len(O) == len(B) == len(r) and all(t[2] in ('exact', 'near') for t in r)
            for t in r:
                o, b = O[t[0]], B[t[1]]
                if t[2] == 'gap':
                    addr = o[0] + t[3]
                    if addr not in allout:
                        allout[addr] = (b[1], 'gap'); added[0] += 1
                elif clean or o[1] >= TINY:
                    put(o, b, t[2])
        # extend after the last anchor and before the first
        j, i, ln = c[-1]
        O = seq[i + ln:i + ln + EXT]; B = base[j + ln:j + ln + 2 * EXT]
        if unnamed_in(O):
            for t in align_segment(O, B, local=True) or []:
                if t[2] in ('exact', 'near') and O[t[0]][1] >= TINY:
                    put(O[t[0]], B[t[1]], t[2])
        j, i, ln = c[0]
        O = seq[max(0, i - EXT):i][::-1]; B = base[max(0, j - 2 * EXT):j][::-1]
        if unnamed_in(O):
            for t in align_segment(O, B, local=True) or []:
                if t[2] in ('exact', 'near') and O[t[0]][1] >= TINY:
                    put(O[t[0]], B[t[1]], t[2])
    return added[0]


def between(seq, base, allout, stats, label):
    """Runs of r unnamed entries whose two named neighbours are r + 1 apart
    in baserom, with the r baserom entries in between unused: name them one
    to one whatever their sizes (code that changed between the builds)."""
    pos = {}
    for j, b in enumerate(base):
        pos.setdefault(b[1], j)
    used = set(v[0] for v in allout.values())
    F = [o for o in seq if o[2] == 'F']
    i = 0
    n = 0
    while i < len(F):
        if F[i][0] in allout:
            i += 1
            continue
        k = i
        while k < len(F) and F[k][0] not in allout:
            k += 1
        if i > 0 and k < len(F):
            ja = pos.get(allout[F[i - 1][0]][0])
            jc = pos.get(allout[F[k][0]][0])
            r = k - i
            if ja is not None and jc is not None and jc - ja - 1 == r and r <= 3 \
                    and allout[F[i - 1][0]][1] not in WEAK and allout[F[k][0]][1] not in WEAK:
                bs = base[ja + 1:jc]
                if all(b[1] not in used for b in bs):
                    for o, b in zip(F[i:k], bs):
                        allout[o[0]] = (b[1], 'between')
                        used.add(b[1])
                        n += 1
        i = k
    stats[label + '_between'] += n
    return n


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


RANK = {'manual': -1, 'native': 0, 'vtable': 0.5, 'exact': 1, 'thunk': 1, 'fill': 1.5,
        'sum': 2, 'vtsize': 2, 'between': 3.5, 'near': 3, 'gap': 4, 'fillnear': 4.5, 'moved': 5, 'thunk?': 6}


def dedupe(allout):
    """a baserom name used twice -> keep the 'better' one only (in place)"""
    best = {}
    for addr, (name, conf) in allout.items():
        if name not in best or RANK[conf] < RANK[allout[best[name]][1]]:
            best[name] = addr
    for addr in [k for k, v in allout.items() if best[v[0]] != k]:
        del allout[addr]
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary')
    ap.add_argument('basesym')
    ap.add_argument('nm')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--manual', help='addr name overrides (manual.txt)')
    ap.add_argument('--cache', help='pickle of the first (size K-gram) stage, for development')
    ap.add_argument('--v1', action='store_true', help='round-3 algorithm only (no refill/vtable passes)')
    a = ap.parse_args()
    seqs = build_ours(a.binary, a.nm)
    base = build_base(a.basesym)
    stats = defaultdict(int)
    allout = {}
    coalnames = set(o[3] for o in seqs['__textcoal_nt'] if o[3])
    import os, pickle
    if a.cache and os.path.exists(a.cache):
        allout = pickle.load(open(a.cache, 'rb'))
    else:
        for sn, seq in seqs.items():
            b = base if sn != '__text' else [e for e in base if e[1] not in coalnames]
            for addr, v in transfer(seq, b, stats, sn).items():
                # 'sum' names were confirmed by thunks only 9% of the time: drop
                if addr not in allout and v[1] != 'sum':
                    allout[addr] = v
        if a.cache:
            pickle.dump(allout, open(a.cache, 'wb'))
    funcs = [o[0] for sq in seqs.values() for o in sq if o[2] == 'F']
    basenames = set(e[1] for e in base)
    fix_thunks(a.binary, allout, basenames, stats, funcs)
    manual = []
    if a.manual:
        for line in open(a.manual):
            p = line.split()
            if len(p) >= 2 and not line.startswith('#'):
                manual.append((int(p[0], 16), p[1]))

    def apply_manual():
        for addr, nm in manual:
            for k in [k for k, v in allout.items() if v[0] == nm and k != addr]:
                del allout[k]
            allout[addr] = (nm, 'manual')
    apply_manual()
    if not a.v1:
        import rtti, vtnames
        R = rtti.RTTI(rtti.Image(a.binary))
        groups = rtti.slot_members(R)
        sizes = {o[0]: o[1] for sq in seqs.values() for o in sq if o[2] == 'F'}
        basesize = {}
        for e in base:
            basesize.setdefault(e[1], e[2])
        bases = {sn: base if sn != '__text' else [e for e in base if e[1] not in coalnames]
                 for sn in seqs}
        kg = {sn: [x for x in anchors_for(seqs[sn], bases[sn]) if x[2] > 1] for sn in seqs}
        for it in range(3):
            st = defaultdict(int)
            for sn, seq in seqs.items():
                refill(seq, bases[sn], allout, st, sn, kg[sn])
                between(seq, bases[sn], allout, st, sn)
            # one function per name before deriving from names
            dedupe(allout)
            new = vtnames.vtable_names(R, groups, allout, basenames, st, sizes, basesize)
            allout.update(new)
            fix_thunks(a.binary, allout, basenames, st, funcs)
            apply_manual()
            dedupe(allout)
            print('pass', it, dict(st), file=sys.stderr)
            for k, v in st.items():
                stats['p%d_%s' % (it, k)] = v
            if not new and not any(k.endswith('fill') for k in st):
                break
    best = dedupe(allout)
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
