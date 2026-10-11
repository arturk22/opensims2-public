"""Names from vtables (used by symxfer.py and evalmap.py).

Every function in one slot group (rtti.slot_members) implements the same
virtual method, so a name on one of them names the others: the method and
signature come from the group's majority, the class from the vtable that
holds the function.  A derived name is only used when it exists in
baserom.sym.  Slots of a class' own new virtuals that no other class shares
get the one baserom name of that class with the same exact size (vtsize)."""
from collections import Counter, defaultdict
import rtti

STRONG = ('native', 'manual')


def method_key(name):
    s = rtti.split_structor(name)
    if s:
        return ('S', s[1], s[2])
    s = rtti.split_member(name)
    if s:
        return ('M', s[3], s[1], s[2])
    return None


def name_from_key(key, cname):
    if key[0] == 'S':
        return rtti.structor_name(cname, key[1], key[2])
    return rtti.member_name(cname, key[2], key[3], key[1])


def owner_order(R, ts):
    """classes holding one function, the most basic first"""
    def depth(t):
        return len(R.ancestors(t))
    return sorted(ts, key=depth)


def vtable_names(R, groups, allout, basenames, stats, sizes=None, basesize=None):
    """returns {addr: (name, conf)} to add or replace"""
    new = {}
    used = set(v[0] for v in allout.values())
    lonely = []
    for g, mem in groups.items():
        byaddr = defaultdict(set)
        for a, t, th in mem:
            byaddr[a].add(t)
        votes = Counter()
        for a in byaddr:
            v = allout.get(a)
            if v:
                k = method_key(v[0])
                if k:
                    votes[k] += 2 if v[1] in STRONG or v[1] == 'vtable' else 1
        if not votes:
            lonely.append(byaddr)
            continue
        mc = votes.most_common(2)
        top, n = mc[0]
        if len(mc) > 1 and mc[1][1] * 2 > n:
            stats['vt_tied'] += 1
            continue
        if n < 2:
            # one weak name: trusted only if the name it implies for
            # another function of the group exists in baserom
            ok = False
            for a, ts in byaddr.items():
                if a in allout:
                    continue
                if any(name_from_key(top, R.ti[t]) in basenames for t in ts):
                    ok = True
                    break
            if not ok:
                stats['vt_single_unconfirmed'] += 1
                continue
            stats['vt_single_confirmed'] += 1
        for a, ts in byaddr.items():
            nm = None
            for t in owner_order(R, ts):
                c = name_from_key(top, R.ti[t])
                if c in basenames:
                    nm = c
                    break
            if nm is None:
                continue
            cur = allout.get(a)
            if cur and cur[0] == nm:
                continue
            if cur and cur[1] in STRONG:
                stats['vt_conflict_strong'] += 1
                continue
            if cur:
                stats['vt_replaced_' + cur[1]] += 1
            else:
                stats['vt_new'] += 1
            new[a] = (nm, 'vtable')
            used.add(nm)
    if sizes is None:
        return new
    # own virtuals nobody else shares: unique exact size within the class
    byclass = defaultdict(set)
    for byaddr in lonely:
        for a, ts in byaddr.items():
            if a not in allout and a not in new and a in sizes:
                byclass[owner_order(R, ts)[0]].add(a)
    if byclass:
        cand = defaultdict(lambda: defaultdict(list))
        want = set(R.ti[t] for t in byclass)
        for nm, sz in basesize.items():
            if nm in used:
                continue
            c = rtti.name_class(nm)
            if c in want:
                cand[c][sz].append(nm)
        for t, addrs in byclass.items():
            cs = cand.get(R.ti[t])
            if not cs:
                continue
            bysz = defaultdict(list)
            for a in addrs:
                bysz[sizes[a]].append(a)
            for sz, al in bysz.items():
                if sz >= 32 and len(al) == 1 and len(cs.get(sz, ())) == 1:
                    new[al[0]] = (cs[sz][0], 'vtsize')
                    stats['vt_size'] += 1
    return new
