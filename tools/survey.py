#!/usr/bin/env python3
"""Survey: decode all code reachable from entry/exports/reloc targets, count instructions by mnemonic."""
import sys, glob, os, collections, pefile, capstone
from capstone import x86
D=sys.argv[1]
SKIP=('plugy','d2ddraw','d2direct3d','d2gdi','d2vidtst','bnupdate')
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail=True
tot=collections.Counter(); prefix=collections.Counter()
for f in sorted(glob.glob(os.path.join(D,'*.dll'))+[os.path.join(D,'Game.exe')]):
    name=os.path.basename(f)
    if name.lower().startswith(SKIP): continue
    pe=pefile.PE(f); base=pe.OPTIONAL_HEADER.ImageBase
    img=pe.get_memory_mapped_image()
    code=[(s.VirtualAddress, s.VirtualAddress+max(s.Misc_VirtualSize,s.SizeOfRawData)) for s in pe.sections if s.Characteristics&0x20000000]
    incode=lambda rva:any(a<=rva<b for a,b in code)
    seeds={pe.OPTIONAL_HEADER.AddressOfEntryPoint}
    for e in getattr(getattr(pe,'DIRECTORY_ENTRY_EXPORT',None),'symbols',[]) or []: seeds.add(e.address)
    for blk in getattr(pe,'DIRECTORY_ENTRY_BASERELOC',[]):
        for r in blk.entries:
            if r.type!=3: continue
            v=pe.get_dword_at_rva(r.rva)
            if v and incode(v-base): seeds.add(v-base)
    seen=set(); work=[s for s in seeds if incode(s)]; funcs=set(work); n=0; bad=0
    while work:
        rva=work.pop()
        while incode(rva) and rva not in seen:
            seen.add(rva)
            ins=next(md.disasm(img[rva:rva+16], base+rva),None)
            if ins is None: bad+=1; break
            n+=1; tot[ins.mnemonic]+=1
            for p in ins.prefix:
                if p: prefix[p]+=1
            m=ins.mnemonic; op=ins.operands
            nxt=rva+ins.size
            if m in('ret','retf','hlt','int3','ud2'): break
            if m=='jmp' or (m.startswith('j')) or m=='call' or m.startswith('loop') or m=='jecxz':
                if op and op[0].type==x86.X86_OP_IMM:
                    t=op[0].imm-base
                    if incode(t):
                        work.append(t)
                        if m=='call': funcs.add(t)
                if m=='jmp': break
            rva=nxt
    print(f"{name:16} insns={n:7} funcs~={len(funcs):5} undecodable={bad}")
print("total insns",sum(tot.values()),"distinct mnemonics",len(tot))
print(' '.join(f"{m}:{c}" for m,c in tot.most_common()))
