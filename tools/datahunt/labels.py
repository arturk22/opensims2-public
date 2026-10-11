#!/usr/bin/env python3
"""Print the SimAntics editor label tables (objects.package, group
0x7FE59FD0, STR#). usage: labels.py <instance-hex>..."""
import dbpf,sys
p=dbpf.Pkg(dbpf.RES+'/Objects/objects.package')
want=[int(x,16) for x in sys.argv[1:]]
for e in p.of(0x53545223):
    if e[1]==0x7FE59FD0 and e[2] in want:
        n,s=dbpf.strs(p.data(e)); s=[x for x in s if x[0]==1]
        print('==',hex(e[2]),n)
        for k,x in enumerate(s): print(k, repr(x[1]), repr(x[2]) if x[2] else '')
