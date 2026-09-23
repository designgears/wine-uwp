#!/usr/bin/env python3
"""Read a Windows minidump (Sentry/crashpad writes them; Shapr3D's are in
LocalState/crash_report/reports/*.dmp): the faulting thread, exception code and address
resolved to module+offset, then a scan of that thread's stack for values that land in a
module. The scan is a heuristic - it finds return addresses and also stale values - but with
a Wine builtin's DWARF (`llvm-symbolizer --obj=<build dll> 0x180000000+offset`) it names the
callers quickly. Usage: ./minidump.py <file.dmp>
"""
import struct,sys
d=open(sys.argv[1],'rb').read()
n,rva=struct.unpack_from('<II',d,8)
streams={}
for i in range(n):
    t,sz,r=struct.unpack_from('<III',d,rva+12*i); streams[t]=(sz,r)
def rstr(r):
    l=struct.unpack_from('<I',d,r)[0]; return d[r+4:r+4+l].decode('utf-16le')
mods=[]
sz,r=streams[4]; c=struct.unpack_from('<I',d,r)[0]
for i in range(c):
    o=r+4+i*108; base,size=struct.unpack_from('<QI',d,o); name=struct.unpack_from('<I',d,o+20)[0]
    mods.append((base,size,rstr(name).split('\\')[-1]))
def where(a):
    for b,s,nm in mods:
        if b<=a<b+s: return f"{nm}+{a-b:#x}"
    return "?"
sz,r=streams[6]; tid=struct.unpack_from('<I',d,r)[0]
code,flags,rec,addr,np_=struct.unpack_from('<IIQQI',d,r+8)
info=struct.unpack_from('<15Q',d,r+8+32)[:np_]
print(f"thread {tid:#x} code {code:#x} addr {addr:#x} = {where(addr)}", [hex(x) for x in info])
# thread context rsp & scan stack for return addresses into modules
# ---- stack scan of the faulting thread
sz,r=streams[3]; nthreads=struct.unpack_from('<I',d,r)[0]
mem=[]
if 9 in streams:
    sz9,r9=streams[9]; nr,base_rva=struct.unpack_from('<QQ',d,r9)
    off=base_rva
    for i in range(nr):
        start,size=struct.unpack_from('<QQ',d,r9+16+16*i); mem.append((start,size,off)); off+=size
if 5 in streams:
    sz5,r5=streams[5]; nr=struct.unpack_from('<I',d,r5)[0]
    for i in range(nr):
        start,dsz,drva=struct.unpack_from('<QII',d,r5+4+16*i); mem.append((start,dsz,drva))
def readq(a):
    for s0,sz0,o in mem:
        if s0<=a<s0+sz0-7: return struct.unpack_from('<Q',d,o+(a-s0))[0]
    return None
for i in range(nthreads):
    o=r+4+48*i
    t_id=struct.unpack_from('<I',d,o)[0]
    if t_id!=tid: continue
    stk_start,stk_sz,stk_rva=struct.unpack_from('<QII',d,o+24)
    ctx_sz,ctx_rva=struct.unpack_from('<II',d,o+40)
    rsp=struct.unpack_from('<Q',d,ctx_rva+0x98)[0]; rip=struct.unpack_from('<Q',d,ctx_rva+0xf8)[0]
    print('rip',where(rip),'rsp',hex(rsp))
    n=0
    for a in range(rsp, min(rsp+0x3000, stk_start+stk_sz), 8):
        v=struct.unpack_from('<Q',d,stk_rva+(a-stk_start))[0]
        w=where(v)
        if w!='?' and not w.startswith(('ntdll','kernelbase')):
            print('  ',hex(a-rsp),w); n+=1
            if n>40: break
