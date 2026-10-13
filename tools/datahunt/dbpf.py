#!/usr/bin/env python3
"""Minimal DBPF/QFS/STR# reader for throwaway data sweeps (docs/data-findings.md).
Game data root: $OS2_GAME (default: the main checkout's data/game)."""
import struct, os, glob, subprocess
def _main_checkout():
    here=os.path.dirname(os.path.abspath(__file__))
    common=subprocess.run(['git','-C',here,'rev-parse','--path-format=absolute','--git-common-dir'],
                          capture_output=True,text=True).stdout.strip()
    return os.path.dirname(common) if common else os.path.join(here,'..','..')
GAME=os.environ.get('OS2_GAME') or os.path.join(_main_checkout(),'data','game')
RES=GAME+'/TSData/Res'
def qfs(d):
    # d starts with compressed header: u32 csize, 0x10FB, 3-byte BE usize
    if len(d)<9 or d[4]!=0x10 or d[5]!=0xFB: return None
    usize=(d[6]<<16)|(d[7]<<8)|d[8]
    out=bytearray(); i=9; n=len(d)
    while i<n:
        c=d[i]
        if c<0x80:
            b1=d[i+1]; i+=2
            pl=c&3; cl=((c>>2)&7)+3; off=((c&0x60)<<3)+b1+1
        elif c<0xC0:
            b1,b2=d[i+1],d[i+2]; i+=3
            pl=(b1>>6)&3; cl=(c&0x3f)+4; off=((b1&0x3f)<<8)+b2+1
        elif c<0xE0:
            b1,b2,b3=d[i+1],d[i+2],d[i+3]; i+=4
            pl=c&3; cl=((c&0x0c)<<6)+b3+5; off=((c&0x10)<<12)+(b1<<8)+b2+1
        elif c<0xFC:
            pl=((c&0x1f)<<2)+4; cl=0; off=0; i+=1
        else:
            pl=c&3; cl=0; off=0; i+=1
        out+=d[i:i+pl]; i+=pl
        if cl:
            s=len(out)-off
            for k in range(cl): out.append(out[s+k])
        if c>=0xFC: break
    return bytes(out)
class Pkg:
    def __init__(s,path):
        s.path=path; f=open(path,'rb'); s.f=f
        h=f.read(96)
        assert h[:4]==b'DBPF',path
        maj,minr=struct.unpack_from('<II',h,4)
        ic,io,isz=struct.unpack_from('<III',h,36)
        iv=struct.unpack_from('<I',h,60)[0]
        f.seek(io); raw=f.read(isz)
        es=24 if iv==2 else 20
        s.entries=[]
        for k in range(ic):
            if es==24: t,g,i,ih,o,sz=struct.unpack_from('<6I',raw,k*24)
            else: t,g,i,o,sz=struct.unpack_from('<5I',raw,k*20); ih=0
            s.entries.append((t,g,i,ih,o,sz))
        s.comp=set()
        for e in s.entries:
            if e[0]==0xE86B1EEF:
                f.seek(e[4]); d=f.read(e[5]); w=20 if es==24 else 16
                for k in range(len(d)//w):
                    v=struct.unpack_from('<%dI'%(w//4),d,k*w)
                    s.comp.add(v[:4] if w==20 else (v[0],v[1],v[2],0))
    def raw(s,e):
        s.f.seek(e[4]); return s.f.read(e[5])
    def data(s,e):
        d=s.raw(e)
        if (e[0],e[1],e[2],e[3]) in s.comp:
            u=qfs(d)
            if u is not None: return u
        return d
    def of(s,t): return [e for e in s.entries if e[0]==t]
def allpkgs(root=GAME):
    return sorted(glob.glob(root+'/**/*.package',recursive=True))
def strs(d):
    # STR# : 64-byte name, u16 format, u16 count, then entries (lang byte, cstr value, cstr desc)
    name=d[:64].split(b'\0')[0].decode('latin1')
    fmt,cnt=struct.unpack_from('<HH',d,64); i=68; out=[]
    if fmt==0xFFFD:
        for k in range(cnt):
            lang=d[i]; i+=1
            j=d.index(b'\0',i); v=d[i:j].decode('latin1'); i=j+1
            j=d.index(b'\0',i); c=d[i:j].decode('latin1'); i=j+1
            out.append((lang,v,c))
    return name,out
