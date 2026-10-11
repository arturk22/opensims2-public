#!/usr/bin/env python3
"""Check the SREL familial relation values 1-9 against the family ties
(0x8C870743) of the four shipped neighborhoods."""
import dbpf,struct,collections,glob
res=collections.Counter(); tot=collections.Counter(); ex=collections.defaultdict(list)
for hood in ['N001','N002','N003','Tutorial']:
    f=glob.glob(dbpf.RES+'/UserData/Neighborhoods/%s/*_Neighborhood.package'%hood)[0]
    p=dbpf.Pkg(f)
    d=p.data(p.of(0x8C870743)[0])
    ver,n=struct.unpack_from('<II',d,0); i=8
    parents=collections.defaultdict(set); kids=collections.defaultdict(set); sib=collections.defaultdict(set); spouse=collections.defaultdict(set)
    for k in range(n):
        nid,u,c=struct.unpack_from('<HII',d,i); i+=10
        for j in range(c):
            t,o=struct.unpack_from('<IH',d,i); i+=6
            if t in (0,1): parents[nid].add(o); kids[o].add(nid)
            elif t==4: kids[nid].add(o); parents[o].add(nid)
            elif t==3: sib[nid].add(o); sib[o].add(nid)
            elif t==2: spouse[nid].add(o)
    def sibs(x):
        s=set(sib[x])
        for pp in parents[x]: s|=kids[pp]
        s.discard(x); return s
    def gp(x): return {g for pp in parents[x] for g in parents[pp]}
    def gc(x): return {g for c in kids[x] for g in kids[c]}
    def uncle(x): return {u for pp in parents[x] for u in sibs(pp)}|{s2 for pp in parents[x] for u in sibs(pp) for s2 in spouse[u]}
    def nephew(x): return {n for s in sibs(x) for n in kids[s]}|{n for sp in spouse[x] for s in sibs(sp) for n in kids[s]}
    def cousin(x): return {c for u in uncle(x) for c in kids[u]}
    H={"child":lambda x:kids[x],"parent":lambda x:parents[x],"sib":sibs,"spouse":lambda x:spouse[x],'gp':gp,'gc':gc,'uncle':uncle,'nephew':nephew,'cousin':cousin}
    for e in p.of(0xCC364C2A):
        a,b=e[2]>>16,e[2]&0xffff; v=p.data(e)
        cnt=struct.unpack_from('<I',v,4)[0]
        vals=struct.unpack_from('<%di'%cnt,v,8)
        if cnt<4: continue
        fam=vals[3]
        if 1<=fam<=9:
            tot[fam]+=1
            for hn,h in H.items():
                if b in h(a): res[(fam,hn)]+=1
            if len(ex[fam])<3: ex[fam].append((hood,a,b,{hn for hn,h in H.items() if b in h(a)}))
for f in sorted(tot): print(f,tot[f],{hn:res[(f,hn)] for hn in H}, ex[f])
