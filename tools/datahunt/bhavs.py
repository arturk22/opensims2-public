#!/usr/bin/env python3
"""Parse every BHAV under TSData/Res (except Locale) once and cache it in
./bhavs.pkl: (package, group, instance, name, signature, type, args, locals,
[(opcode, true, false, node version, operands)])."""
import dbpf,struct,pickle,os,sys
def parse(d):
    name=d[:64].split(b'\0')[0].decode('latin1')
    sig,cnt=struct.unpack_from('<HH',d,64); ttype,argc,locc,fl=d[68:72]; i=76
    wide=sig>=0x8007; nv=sig>=0x8005; osz=16 if sig>=0x8003 else 8
    ins=[]
    for k in range(cnt):
        op=struct.unpack_from('<H',d,i)[0]; i+=2
        if wide: t,f=struct.unpack_from('<HH',d,i); i+=4
        else: t,f=d[i],d[i+1]; i+=2
        v=None
        if nv: v=d[i]; i+=1
        ins.append((op,t,f,v,d[i:i+osz])); i+=osz
    return name,sig,ttype,argc,locc,ins
def load():
    if os.path.exists('bhavs.pkl'): return pickle.load(open('bhavs.pkl','rb'))
    out=[]
    for f in dbpf.allpkgs(dbpf.RES):
        if '/Locale/' in f: continue
        p=dbpf.Pkg(f)
        for e in p.of(0x42484156):
            try: out.append((os.path.relpath(f,dbpf.RES),e[1],e[2])+parse(p.data(e)))
            except Exception as ex: print('ERR',f,hex(e[2]),ex,file=sys.stderr)
    pickle.dump(out,open('bhavs.pkl','wb')); return out
if __name__=='__main__':
    b=load(); print(len(b))
