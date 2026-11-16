#!/usr/bin/env python3
"""Static analysis of Diablo II 1.13c's x86 images for recompilation.

Loads the PE images at their (possibly rebased) addresses, finds functions and their basic blocks,
resolves jump tables, and records imports/exports. Used by recomp.py (C emission) and for surveys:

    python3 tools/analyze.py <game dir>          # per-module stats and the mnemonic histogram
"""
import os, sys, struct, collections, pickle
import pefile, capstone
from capstone import x86

# Modules recompiled (others are replaced natively: video codecs, JPEG, unused renderers, PlugY).
MODULES = ['Game.exe', 'Fog.dll', 'Storm.dll', 'D2Lang.dll', 'D2CMP.dll', 'D2Common.dll', 'D2gfx.dll',
           'D2Glide.dll', 'D2sound.dll', 'D2Win.dll', 'D2Net.dll', 'D2MCPClient.dll', 'D2Multi.dll',
           'D2Launch.dll', 'Bnclient.dll', 'D2Game.dll', 'D2Client.dll']
# Storm.dll (0x6FBF0000-0x6FC50000) overlaps D2Net, D2Lang and D2Game: Storm moves to a free range, the
# D2 modules keep their preferred bases (mods such as PlugY patch them by module offsets).
REBASE = {'storm.dll': 0x70000000}

Cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
Cs.detail = True


class Image:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.lname = self.name.lower()
        pe = pefile.PE(path)
        self.pe = pe
        self.pref = pe.OPTIONAL_HEADER.ImageBase
        self.base = REBASE.get(self.lname, self.pref)
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.img = bytearray(pe.get_memory_mapped_image())
        self.img += bytes(max(0, self.size - len(self.img)))
        self.sections = [(s.Name.rstrip(b'\0').decode(errors='replace'), s.VirtualAddress,
                          max(s.Misc_VirtualSize, s.SizeOfRawData), s.Characteristics) for s in pe.sections]
        self.code = [(a, a + n) for _, a, n, ch in self.sections if ch & 0x20000000]
        # base relocations (HIGHLOW): rva of the pointer
        self.relocs = set()
        for blk in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for r in blk.entries:
                if r.type == 3:
                    self.relocs.add(r.rva)
        delta = self.base - self.pref
        if delta:
            for rva in self.relocs:
                v = struct.unpack_from('<I', self.img, rva)[0]
                struct.pack_into('<I', self.img, rva, (v + delta) & 0xFFFFFFFF)
        # imports: iat slot va -> (dll, name or ordinal)
        self.imports = {}
        for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            dll = e.dll.decode().lower()
            for i in e.imports:
                va = i.address - self.pref + self.base
                self.imports[va] = (dll, i.name.decode() if i.name else int(i.ordinal))
        # exports: ordinal -> rva, name -> ordinal
        self.exports, self.export_names = {}, {}
        exp = getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None)
        if exp:
            for s in exp.symbols:
                if s.forwarder:
                    continue
                self.exports[s.ordinal] = s.address
                if s.name:
                    self.export_names[s.name.decode()] = s.ordinal
        self.entry = pe.OPTIONAL_HEADER.AddressOfEntryPoint
        del self.pe

    def va(self, rva):
        return self.base + rva

    def in_code(self, va):
        rva = va - self.base
        return any(a <= rva < b for a, b in self.code)

    def contains(self, va):
        return 0 <= va - self.base < self.size

    def dword(self, va):
        return struct.unpack_from('<I', self.img, va - self.base)[0]

    def has_reloc(self, va):
        return (va - self.base) in self.relocs


class Insn:
    __slots__ = ('addr', 'size', 'mn', 'ops', 'prefix', 'op_str', 'bytes', 'regs_read', 'regs_write', 'groups')

    def __init__(self, ci):
        self.addr, self.size = ci.address, ci.size
        self.mn = ci.mnemonic
        self.op_str = ci.op_str
        self.bytes = bytes(ci.bytes)
        self.prefix = [p for p in ci.prefix if p]
        self.ops = []
        for o in ci.operands:
            if o.type == x86.X86_OP_REG:
                self.ops.append(('reg', ci.reg_name(o.reg), o.size))
            elif o.type == x86.X86_OP_IMM:
                self.ops.append(('imm', o.imm & 0xFFFFFFFF, o.size))
            else:
                m = o.mem
                self.ops.append(('mem', ci.reg_name(m.segment) if m.segment else None,
                                 ci.reg_name(m.base) if m.base else None,
                                 ci.reg_name(m.index) if m.index else None, m.scale, m.disp & 0xFFFFFFFF, o.size))


class Analysis:
    def __init__(self, gamedir):
        self.images = []
        for m in MODULES:
            p = os.path.join(gamedir, m)
            if not os.path.exists(p):
                # case-insensitive lookup
                for f in os.listdir(gamedir):
                    if f.lower() == m.lower():
                        p = os.path.join(gamedir, f)
            self.images.append(Image(p))
        self.by_name = {i.lname: i for i in self.images}
        self.insns = {}          # va -> Insn
        self.funcs = set()       # function entry vas
        self.jtables = {}        # jmp insn va -> [target vas]
        self.jtable_ranges = []  # (va, va_end) of tables (data inside code)
        self.addr_taken = set()

    def image_of(self, va):
        for im in self.images:
            if im.contains(va):
                return im
        return None

    def decode(self, im, va):
        if va in self.insns:
            return self.insns[va]
        rva = va - im.base
        ci = next(Cs.disasm(bytes(im.img[rva:rva + 16]), va), None)
        if ci is None:
            return None
        ins = Insn(ci)
        self.insns[va] = ins
        return ins

    def run(self):
        work = []
        for im in self.images:
            seeds = {im.va(im.entry)} if im.entry else set()
            seeds |= {im.va(r) for r in im.exports.values()}
            # pointers stored in non-code sections (vtables, callback tables, CRT init tables)
            for rva in im.relocs:
                if any(a <= rva < b for a, b in im.code):
                    continue
                v = im.dword(im.va(rva))
                if im.in_code(v):
                    seeds.add(v)
            if not im.relocs:
                # no relocation table (Game.exe): any aligned dword in the data sections that points into
                # code may be a function pointer (vtables, callbacks)
                for name, a, n, ch in im.sections:
                    if ch & 0x20000000:
                        continue
                    for off in range(0, n - 3, 4):
                        v = struct.unpack_from('<I', im.img, a + off)[0]
                        if im.in_code(v):
                            seeds.add(v)
            for s in seeds:
                if im.in_code(s):
                    self.funcs.add(s)
                    work.append(s)
        self.addr_taken |= set(self.funcs)
        seen = set()
        while work:
            va = work.pop()
            im = self.image_of(va)
            while va not in seen and im.in_code(va):
                seen.add(va)
                ins = self.decode(im, va)
                if ins is None:
                    break
                # pointers into code carried by this instruction (relocations inside its bytes)
                for off in range(ins.size - 3):
                    if im.has_reloc(va + off):
                        v = im.dword(va + off)
                        if im.in_code(v) and not self.is_jtable_operand(ins, v):
                            if v not in self.funcs:
                                self.funcs.add(v)
                                self.addr_taken.add(v)
                                work.append(v)
                if not im.relocs:
                    # without relocations an immediate pointing into code (push offset fn / mov r, offset fn)
                    # is the only trace of an address-taken function
                    for o in ins.ops:
                        if o[0] == 'imm' and ins.mn not in ('call', 'jmp') and not ins.mn.startswith('j') \
                                and im.in_code(o[1]) and o[1] not in self.funcs:
                            self.funcs.add(o[1]); self.addr_taken.add(o[1]); work.append(o[1])
                mn = ins.mn
                nxt = va + ins.size
                if mn in ('ret', 'retf', 'hlt', 'int3', 'ud2', 'iretd'):
                    break
                if mn == 'jmp' or mn.startswith('j') or mn == 'call' or mn.startswith('loop'):
                    op = ins.ops[0] if ins.ops else None
                    if op and op[0] == 'imm':
                        t = op[1]
                        if im.in_code(t):
                            if mn == 'call':
                                if t not in self.funcs:
                                    self.funcs.add(t)
                                work.append(t)
                            else:
                                work.append(t)
                    elif mn == 'jmp' and op and op[0] == 'mem' and op[3] and op[4] == 4 and op[5]:
                        targets = self.jump_table(im, op[5])
                        if targets:
                            self.jtables[va] = targets
                            work.extend(targets)
                    if mn == 'jmp':
                        break
                va = nxt
        return self

    def is_jtable_operand(self, ins, v):
        if ins.mn == 'jmp' and ins.ops and ins.ops[0][0] == 'mem' and ins.ops[0][3]:
            return True
        # byte index tables: movzx reg, byte ptr [reg + table]
        if ins.mn in ('movzx', 'mov') and any(o[0] == 'mem' and o[3] is None and o[2] and o[5] == v for o in ins.ops):
            return False
        return False

    def jump_table(self, im, table):
        """Targets of jmp [reg*4 + table]: the run of relocated code pointers at the table, or (CRT memcpy:
        tables indexed from 1, or addressed through a neighbour) every relocated code pointer close by.
        Extra cases are harmless; a missing one is a crash."""
        targets = []
        va = table
        while im.contains(va + 3) and (im.has_reloc(va) or not im.relocs):
            t = im.dword(va)
            if not im.in_code(t):
                break
            targets.append(t)
            va += 4
            if va in self.funcs:
                break
        if len(targets) < 2:
            for off in range(-64, 65):
                p = table + 4 * off
                if im.contains(p + 3) and im.has_reloc(p) and im.in_code(im.dword(p)):
                    targets.append(im.dword(p))
            va = table + 4
        if targets:
            self.jtable_ranges.append((table, va))
        return sorted(set(targets))


def survey(gamedir):
    a = Analysis(gamedir).run()
    hist = collections.Counter()
    per = collections.defaultdict(collections.Counter)
    for va, ins in a.insns.items():
        im = a.image_of(va)
        hist[ins.mn] += 1
        per[im.name][ins.mn] += 1
    for im in a.images:
        n = sum(per[im.name].values())
        f = sum(1 for v in a.funcs if im.contains(v))
        print(f"{im.name:16} base={im.base:08x} insns={n:7} funcs={f:5}")
    print('functions', len(a.funcs), 'jump tables', len(a.jtables), 'insns', len(a.insns))
    print(' '.join(f'{m}:{c}' for m, c in hist.most_common()))
    return a


if __name__ == '__main__':
    survey(sys.argv[1])
