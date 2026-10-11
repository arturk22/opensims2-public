#!/usr/bin/env python3
"""Per-byte histogram of a primitive's operands over all unique BHAVs.
usage: ophist.py <opcode>   (e.g. 0x1C); import it for uses(opcode)."""
import bhavs,collections,sys
B=bhavs.load()
seen=set(); U=[]
for b in B:
    k=(b[1],b[2])
    if k in seen: continue
    seen.add(k); U.append(b)
def uses(op):
    for b in U:
        for idx,i in enumerate(b[8]):
            if i[0]==op: yield b,idx,i
if __name__=='__main__':
    op=int(sys.argv[1],0); n=0; h=[collections.Counter() for _ in range(16)]; vv=collections.Counter()
    for b,idx,i in uses(op):
        n+=1; vv[i[3]]+=1
        for k in range(len(i[4])): h[k][i[4][k]]+=1
    print('uses',n,'nodeversion',dict(vv))
    for k in range(16): print(k, [(hex(a),c) for a,c in h[k].most_common(12)], 'distinct',len(h[k]))
