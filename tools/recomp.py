#!/usr/bin/env python3
"""x86 -> C static recompiler for Diablo II 1.13c.

    python3 tools/recomp.py <game dir> <out dir>

Emits out/<module>_<n>.c (functions `void f_XXXXXXXX(Cpu*)`), out/functions.c (guest address ->
function table) and out/imports.c (every import slot: module, symbol). See runtime/cpu.h for the model.
"""
import os, sys, collections
sys.path.insert(0, os.path.dirname(__file__))
from analyze import Analysis

REG32 = {'eax': 0, 'ecx': 1, 'edx': 2, 'ebx': 3, 'esp': 4, 'ebp': 5, 'esi': 6, 'edi': 7}
REG16 = {'ax': 0, 'cx': 1, 'dx': 2, 'bx': 3, 'sp': 4, 'bp': 5, 'si': 6, 'di': 7}
REG8L = {'al': 0, 'cl': 1, 'dl': 2, 'bl': 3}
REG8H = {'ah': 0, 'ch': 1, 'dh': 2, 'bh': 3}
RN = ['EAX', 'ECX', 'EDX', 'EBX', 'ESP', 'EBP', 'ESI', 'EDI']

CF, ZF, SF, OF, PF = 1, 2, 4, 8, 16
ALL = CF | ZF | SF | OF | PF
CC_READS = {'o': OF, 'no': OF, 'b': CF, 'ae': CF, 'e': ZF, 'ne': ZF, 'be': CF | ZF, 'a': CF | ZF, 's': SF, 'ns': SF,
            'p': PF, 'np': PF, 'l': SF | OF, 'ge': SF | OF, 'le': ZF | SF | OF, 'g': ZF | SF | OF}
CC_ALIAS = {'c': 'b', 'nae': 'b', 'nc': 'ae', 'nb': 'ae', 'z': 'e', 'nz': 'ne', 'na': 'be', 'nbe': 'a', 'pe': 'p',
            'po': 'np', 'nge': 'l', 'nl': 'ge', 'ng': 'le', 'nle': 'g'}
JCC = {'j' + k for k in list(CC_READS) + list(CC_ALIAS)}


def cc_of(mn, prefix):
    k = mn[len(prefix):]
    return CC_ALIAS.get(k, k)


def mask(bits):
    return {8: '0xFFu', 16: '0xFFFFu', 32: '0xFFFFFFFFu'}[bits]


def utype(bits):
    return {8: 'u8', 16: 'u16', 32: 'u32', 64: 'u64'}[bits]


def stype(bits):
    return {8: 's8', 16: 's16', 32: 's32', 64: 's64'}[bits]


class Unsupported(Exception):
    pass


# Widescreen: the game's 800-pixel width at these instructions comes from the runtime's g_w800 (800 by
# default; the phone build sets 16:9 = 1068). D2gfx's mode size and window checks, D2Glide's output width,
# D2Client's screen size globals and a full-width panel, D2Win's text clipping.
WIDE_SITES = {0x6FA87FFF, 0x6FA88141, 0x6FA883F0, 0x6FA88747, 0x6FA88750,
              0x6F85DAF5, 0x6F85DCF6,
              0x6FAC0E09, 0x6FAF5332, 0x6FAD32E8,
              0x6F8ED40F, 0x6F8ED417, 0x6F8ED41E}
# Immediates replaced by an expression of g_w800: D2Client's panel X offset in the 800 mode (80 = (800-640)/2,
# the panels' Inventory.txt coordinates are 640-based) grows by half the extra width, centring the panels.
WIDE_EXPR = {0x6FB739FB: (0x50, '(80u + (g_w800 - 800u) / 2u)')}
# Pushed X coordinates shifted by half the extra width: the 800-mode decorative borders around the left
# and right panels (Panel\800BorderFrame, drawn at fixed positions) follow the centred panels.
WIDE_ADD = {0x6FAD71FB, 0x6FAD7215, 0x6FAD7236, 0x6FAD7253, 0x6FAD7270,
            0x6FAD710A, 0x6FAD7130, 0x6FAD7152, 0x6FAD7174, 0x6FAD7196}


# Functions wrapped in a runtime counter (the generated f_ calls the body with the counter raised): D2Client's
# control panel drawing, so the widescreen HUD shift in glide.c knows which draws are the panel's.
WRAP = {0x6FAD9250: 'g_hud_draw'}


# ------------------------------------------------------------------ flag effects (liveness)
def flag_effect(ins):
    """(reads, writes) flag masks of an instruction."""
    mn = ins.mn
    base = mn.split()[-1]
    if base in JCC:
        return CC_READS[cc_of(base, 'j')], 0
    if base.startswith('set'):
        return CC_READS[cc_of(base, 'set')], 0
    if base.startswith('cmov'):
        return CC_READS[cc_of(base, 'cmov')], 0
    if base.startswith('fcmov'):
        return ALL, 0
    if base in ('add', 'sub', 'cmp', 'neg', 'and', 'or', 'xor', 'test', 'xadd', 'cmpxchg', 'cmpsb', 'cmpsw',
                'cmpsd', 'scasb', 'scasw', 'scasd', 'bsf', 'bsr', 'imul', 'mul', 'sahf', 'popfd', 'fcomi',
                'fucomi', 'fcomip', 'fucomip', 'fcompi', 'fucompi', 'shld', 'shrd'):
        if base == 'cmpsd' and ins.ops and ins.ops[0][0] == 'reg' and ins.ops[0][1].startswith('xmm'):
            return 0, 0
        if base in ('cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw', 'scasd') and ('repe' in mn or 'repne' in mn or 'rep' in mn):
            return 0, ALL
        return 0, ALL
    if base in ('adc', 'sbb', 'rcl', 'rcr'):
        return CF, ALL
    if base in ('inc', 'dec'):
        return 0, ALL & ~CF
    if base in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror'):
        return ALL, ALL  # count 0 keeps flags: conservatively read them
    if base in ('bt', 'bts', 'btr', 'btc', 'clc', 'stc'):
        return 0, CF
    if base == 'cmc':
        return CF, CF
    if base in ('lahf', 'pushfd', 'salc'):
        return ALL, 0
    if base in ('div', 'idiv', 'aaa', 'aas', 'daa', 'das', 'aam', 'aad'):
        return 0, ALL
    if base in ('loope', 'loopne'):
        return ZF, 0
    if base in ('ret', 'retf', 'iretd'):
        return ALL, 0
    return 0, 0


# ------------------------------------------------------------------ emitter
class Emitter:
    def __init__(self, an, im, func, blocks, live_out):
        self.an, self.im, self.func, self.live_out = an, im, func, live_out
        self.blocks = blocks  # sorted list of insn addrs in this function
        self.inset = set(blocks)
        self.labels = set()
        self.jtargets = {func}
        for va in blocks:
            ins = an.insns[va]
            for t in successors(an, ins, self.inset)[0]:
                if t != ins.addr + ins.size:
                    self.jtargets.add(t)
        self.out = []
        self.tmp = 0

    def t(self):
        self.tmp += 1
        return f'_t{self.tmp}'

    # ---- operands
    def ea(self, op):
        _, seg, base, index, scale, disp, size = op
        parts = []
        if base:
            parts.append(f'c->r[{RN[REG32[base]]}]')
        if index:
            parts.append(f'c->r[{RN[REG32[index]]}]' + (f'*{scale}' if scale != 1 else ''))
        if disp or not parts:
            parts.append(f'0x{disp:X}u')
        e = ' + '.join(parts)
        if seg == 'fs':
            e = f'c->fs_base + {e}'
        return f'(u32)({e})'

    def bits(self, op):
        return op[-1] * 8

    def rd(self, op, ea=None):
        k = op[0]
        if k == 'imm':
            b = op[2] * 8 if op[2] else 32
            if op[1] == 0x320 and getattr(self, 'cur', 0) in WIDE_SITES:
                return 'g_w800'
            we = WIDE_EXPR.get(getattr(self, 'cur', 0))
            if we and op[1] == we[0]:
                return we[1]
            return f'0x{op[1] & ((1 << min(b, 32)) - 1):X}u'
        if k == 'reg':
            n = op[1]
            if n in REG32:
                return f'c->r[{RN[REG32[n]]}]'
            if n in REG16:
                return f'(u16)c->r[{RN[REG16[n]]}]'
            if n in REG8L:
                return f'(u8)c->r[{RN[REG8L[n]]}]'
            if n in REG8H:
                return f'(u8)(c->r[{RN[REG8H[n]]}] >> 8)'
            raise Unsupported('reg ' + n)
        b = self.bits(op)
        a = ea or self.ea(op)
        if b in (8, 16, 32, 64):
            return f'rd{b}({a})'
        raise Unsupported(f'mem{b}')

    def wr(self, op, v, ea=None):
        k = op[0]
        if k == 'reg':
            n = op[1]
            if n in REG32:
                return f'c->r[{RN[REG32[n]]}] = (u32)({v});'
            if n in REG16:
                r = f'c->r[{RN[REG16[n]]}]'
                return f'{r} = ({r} & 0xFFFF0000u) | (u16)({v});'
            if n in REG8L:
                r = f'c->r[{RN[REG8L[n]]}]'
                return f'{r} = ({r} & ~0xFFu) | (u8)({v});'
            if n in REG8H:
                r = f'c->r[{RN[REG8H[n]]}]'
                return f'{r} = ({r} & ~0xFF00u) | ((u32)(u8)({v}) << 8);'
            raise Unsupported('reg ' + n)
        if k == 'mem':
            b = self.bits(op)
            return f'wr{b}({ea or self.ea(op)}, ({utype(b)})({v}));'
        raise Unsupported('write imm')

    def target_code(self, t, tail):
        """Direct transfer to guest address t: a label here, a tail call, or the dispatcher."""
        if t in self.inset:
            self.labels.add(t)
            return f'goto L_{t:08X};'
        if t in self.an.funcs:
            return f'f_{t:08X}(c); return;'
        return f'jump_indirect(c, 0x{t:08X}u); return;'

    # ---- flag helpers: emit the live flags of an operation
    def flags_arith(self, kind, a, b, r, bits, live, cin=None):
        s = []
        sh = bits - 1
        if live & CF:
            if kind == 'add':
                s.append(f'c->cf = ({r}) < ({a});' if cin is None else f'c->cf = {cin} ? ({r}) <= ({a}) : ({r}) < ({a});')
            else:
                s.append(f'c->cf = ({a}) < ({b});' if cin is None else f'c->cf = {cin} ? ({a}) <= ({b}) : ({a}) < ({b});')
        if live & OF:
            if kind == 'add':
                s.append(f'c->of = ((~(({a}) ^ ({b})) & (({a}) ^ ({r}))) >> {sh}) & 1;')
            else:
                s.append(f'c->of = (((({a}) ^ ({b})) & (({a}) ^ ({r}))) >> {sh}) & 1;')
        s += self.flags_szp(r, bits, live)
        return s

    def flags_szp(self, r, bits, live):
        s = []
        if live & ZF:
            s.append(f'c->zf = ({r}) == 0;')
        if live & SF:
            s.append(f'c->sf = (({r}) >> {bits - 1}) & 1;')
        if live & PF:
            s.append(f'c->pf = parity8({r});')
        return s

    def flags_logic(self, r, bits, live):
        s = []
        if live & CF:
            s.append('c->cf = 0;')
        if live & OF:
            s.append('c->of = 0;')
        return s + self.flags_szp(r, bits, live)

    # ---- one instruction
    def emit(self, ins, live, nxt_ins):
        """C statements for ins; live = flags live after it. Returns list of lines (may consume nxt_ins)."""
        mn = ins.mn
        ops = ins.ops
        self.cur = ins.addr
        L = []
        words = mn.split()
        rep = None
        if words[0] in ('rep', 'repe', 'repz', 'repne', 'repnz'):
            rep = words[0]
        lock = words[0] == 'lock'
        base = words[-1]
        a0 = ops[0] if ops else None
        a1 = ops[1] if len(ops) > 1 else None
        nxt = ins.addr + ins.size

        def two(fn):
            # read-modify-write dest with src
            b = self.bits(a0)
            ut = utype(b)
            e = None
            if a0[0] == 'mem':
                e = self.t()
                L.append(f'u32 {e} = {self.ea(a0)};')
            return b, ut, e

        if base == 'nop' or base == 'wait' or base == 'fwait' or base.startswith('prefetch') or base == 'pause' \
                or base in ('fnclex', 'emms', 'cld' if False else '__none__'):
            return L
        if base == 'mov':
            L.append(self.wr(a0, self.rd(a1)))
        elif base in ('movzx', 'movsx'):
            sb = self.bits(a1) if a1[0] == 'mem' else a1[2] * 8
            v = self.rd(a1)
            if base == 'movsx':
                v = f'(u32)(s32)({stype(sb)})({v})'
            L.append(self.wr(a0, v))
        elif base == 'lea':
            L.append(self.wr(a0, self.ea(a1)))
        elif base == 'push':
            if a0[0] == 'imm':
                v = 'g_w800' if a0[1] == 0x320 and ins.addr in WIDE_SITES else f'0x{a0[1] & 0xFFFFFFFF:X}u'
            else:
                v = self.rd(a0)
            if ins.addr in WIDE_ADD:
                v = f'({v} + (g_w800 - 800u) / 2u)'
            if self.bits(a0) == 16 and a0[0] != 'imm':
                L.append(f'{{ u16 _v = {v}; c->r[ESP] -= 2; wr16(c->r[ESP], _v); }}')
            else:
                L.append(f'{{ u32 _v = {v}; push32(c, _v); }}')
        elif base == 'pop':
            if a0[0] == 'mem':
                # address computed after esp is incremented (x86 semantics)
                L.append(f'{{ u32 _v = pop32(c); {self.wr(a0, "_v")} }}')
            else:
                L.append(f'{{ u32 _v = pop32(c); {self.wr(a0, "_v")} }}')
        elif base == 'xchg':
            if a0 == a1:
                return L
            b = self.bits(a0)
            ea0 = ea1 = None
            if a0[0] == 'mem':
                ea0 = self.t(); L.append(f'u32 {ea0} = {self.ea(a0)};')
            if a1[0] == 'mem':
                ea1 = self.t(); L.append(f'u32 {ea1} = {self.ea(a1)};')
            L.append(f'{{ {utype(b)} _x = {self.rd(a0, ea0)}, _y = {self.rd(a1, ea1)}; {self.wr(a0, "_y", ea0)} {self.wr(a1, "_x", ea1)} }}')
        elif base in ('add', 'sub', 'adc', 'sbb', 'cmp'):
            b, ut, e = two(None)
            fl = live & ALL
            fused = None
            if base in ('cmp',) and nxt_ins is not None:
                fused = self.try_fuse(ins, nxt_ins, live, 'cmp')
                if fused:
                    return fused
            av, bv = self.rd(a0, e), self.rd(a1)
            if a1[0] == 'imm' and a1[2] and a1[2] * 8 < b:
                pass
            L.append(f'{{ {ut} _a = {av}, _b = ({ut}){bv};')
            if base == 'add':
                L.append(f'{ut} _r = _a + _b;')
                L += self.flags_arith('add', '_a', '_b', '_r', b, fl)
            elif base == 'adc':
                L.append(f'u8 _ci = c->cf; {ut} _r = _a + _b + _ci;')
                L += self.flags_arith('add', '_a', '_b', '_r', b, fl, cin='_ci')
            elif base == 'sbb':
                L.append(f'u8 _ci = c->cf; {ut} _r = _a - _b - _ci;')
                L += self.flags_arith('sub', '_a', '_b', '_r', b, fl, cin='_ci')
            else:
                L.append(f'{ut} _r = _a - _b;')
                L += self.flags_arith('sub', '_a', '_b', '_r', b, fl)
            if base != 'cmp':
                L.append(self.wr(a0, '_r', e))
            L.append('}')
        elif base in ('and', 'or', 'xor', 'test'):
            if base == 'test' and nxt_ins is not None:
                fused = self.try_fuse(ins, nxt_ins, live, 'test')
                if fused:
                    return fused
            b, ut, e = two(None)
            opch = {'and': '&', 'or': '|', 'xor': '^', 'test': '&'}[base]
            if base == 'xor' and a0 == a1:
                L.append(self.wr(a0, '0', e))
                L += self.flags_logic('0', b, live)
                return L
            L.append(f'{{ {ut} _r = {self.rd(a0, e)} {opch} ({ut}){self.rd(a1)};')
            L += self.flags_logic('_r', b, live)
            if base != 'test':
                L.append(self.wr(a0, '_r', e))
            L.append('}')
        elif base in ('inc', 'dec'):
            b, ut, e = two(None)
            op = '+' if base == 'inc' else '-'
            if lock and e:
                L.append(f'{{ {ut} _a = __atomic_fetch_{"add" if base == "inc" else "sub"}(({ut}*)H({e}), 1, __ATOMIC_SEQ_CST); {ut} _r = _a {op} 1;')
            else:
                L.append(f'{{ {ut} _a = {self.rd(a0, e)}; {ut} _r = _a {op} 1;')
                L.append(self.wr(a0, '_r', e))
            if live & OF:
                L.append(f'c->of = _r == ({ut})0x{(1 << (b - 1)) if base == "inc" else (1 << (b - 1)) - 1:X}u;')
            L += self.flags_szp('_r', b, live)
            L.append('}')
        elif base == 'neg':
            b, ut, e = two(None)
            L.append(f'{{ {ut} _a = {self.rd(a0, e)}; {ut} _r = ({ut})(0 - _a);')
            if live & CF:
                L.append('c->cf = _a != 0;')
            if live & OF:
                L.append(f'c->of = _a == ({ut})0x{1 << (b - 1):X}u;')
            L += self.flags_szp('_r', b, live)
            L.append(self.wr(a0, '_r', e))
            L.append('}')
        elif base == 'not':
            b, ut, e = two(None)
            L.append(self.wr(a0, f'~{self.rd(a0, e)}', e))
        elif base in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
            b, ut, e = two(None)
            cnt = '1' if len(ops) == 1 else (f'({self.rd(a1)} & 31)' if a1[0] != 'imm' else str(a1[1] & 31))
            L.append(f'{{ u32 _n = {cnt}; if (_n) {{ {ut} _a = {self.rd(a0, e)}; {ut} _r;')
            if base in ('shl', 'sal'):
                L.append(f'_r = ({ut})((u32)_a << _n); c->cf = _n <= {b} ? ((u32)_a >> ({b} - _n)) & 1 : 0;')
                L.append(f'c->of = ((_r >> {b - 1}) & 1) ^ c->cf;')
            elif base == 'shr':
                L.append(f'_r = ({ut})((u32)_a >> _n); c->cf = ((u32)_a >> (_n - 1)) & 1;')
                L.append(f'c->of = (_a >> {b - 1}) & 1;')
            elif base == 'sar':
                L.append(f'_r = ({ut})((s32)({stype(b)})_a >> (_n > {b - 1} ? {b - 1} : _n)); c->cf = ((s32)({stype(b)})_a >> (_n - 1 > {b - 1} ? {b - 1} : _n - 1)) & 1;')
                L.append('c->of = 0;')
            elif base == 'rol':
                L.append(f'u32 _k = _n % {b}; _r = ({ut})(((u32)_a << _k) | ((u32)_a >> (({b} - _k) % {b}))); c->cf = _r & 1;')
                L.append(f'c->of = ((_r >> {b - 1}) & 1) ^ c->cf;')
            elif base == 'ror':
                L.append(f'u32 _k = _n % {b}; _r = ({ut})(((u32)_a >> _k) | ((u32)_a << (({b} - _k) % {b}))); c->cf = (_r >> {b - 1}) & 1;')
                L.append(f'c->of = ((_r >> {b - 1}) ^ (_r >> {b - 2})) & 1;')
            elif base == 'rcl':
                L.append(f'u64 _w = ((u64)c->cf << {b}) | _a; u32 _k = _n % {b + 1}; _w = ((_w << _k) | (_w >> ({b + 1} - _k))) & ((1ull << {b + 1}) - 1);')
                L.append(f'_r = ({ut})_w; c->cf = (_w >> {b}) & 1; c->of = ((_r >> {b - 1}) & 1) ^ c->cf;')
            else:  # rcr
                L.append(f'u64 _w = ((u64)c->cf << {b}) | _a; u32 _k = _n % {b + 1}; _w = ((_w >> _k) | (_w << ({b + 1} - _k))) & ((1ull << {b + 1}) - 1);')
                L.append(f'_r = ({ut})_w; c->cf = (_w >> {b}) & 1; c->of = ((_r >> {b - 1}) ^ (_r >> {b - 2})) & 1;')
            if base in ('shl', 'sal', 'shr', 'sar'):
                L += self.flags_szp('_r', b, live)
            L.append(self.wr(a0, '_r', e))
            L.append('} }')
        elif base in ('shld', 'shrd'):
            b, ut, e = two(None)
            cnt = f'({self.rd(ops[2])} & 31)' if ops[2][0] != 'imm' else str(ops[2][1] & 31)
            L.append(f'{{ u32 _n = {cnt}; if (_n) {{ u32 _a = {self.rd(a0, e)}, _s = {self.rd(a1)}; u32 _r;')
            if base == 'shld':
                L.append('_r = (_a << _n) | (_s >> (32 - _n)); c->cf = (_a >> (32 - _n)) & 1;')
            else:
                L.append('_r = (_a >> _n) | (_s << (32 - _n)); c->cf = (_a >> (_n - 1)) & 1;')
            L += self.flags_szp('_r', 32, live)
            L.append('c->of = 0;')
            L.append(self.wr(a0, '_r', e))
            L.append('} }')
        elif base == 'imul':
            if len(ops) == 1:
                b = self.bits(a0)
                if b == 32:
                    L.append(f'{{ s64 _p = (s64)(s32)c->r[EAX] * (s32){self.rd(a0)}; c->r[EAX] = (u32)_p; c->r[EDX] = (u32)(_p >> 32);')
                    L.append('c->cf = c->of = _p != (s64)(s32)_p; }')
                elif b == 16:
                    L.append(f'{{ s32 _p = (s32)(s16)c->r[EAX] * (s16){self.rd(a0)}; c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | (u16)_p; c->r[EDX] = (c->r[EDX] & 0xFFFF0000u) | (u16)(_p >> 16);')
                    L.append('c->cf = c->of = _p != (s32)(s16)_p; }')
                else:
                    L.append(f'{{ s32 _p = (s32)(s8)c->r[EAX] * (s8){self.rd(a0)}; c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | (u16)_p;')
                    L.append('c->cf = c->of = _p != (s32)(s8)_p; }')
            else:
                src1 = self.rd(a1) if len(ops) == 3 else self.rd(a0)
                src2 = self.rd(ops[2]) if len(ops) == 3 else self.rd(a1)
                b = self.bits(a0)
                L.append(f'{{ s64 _p = (s64)({stype(b)}){src1} * (s64)({stype(b)}){src2};')
                if live & (CF | OF):
                    L.append(f'c->cf = c->of = _p != (s64)({stype(b)})_p;')
                L += self.flags_szp(f'({utype(b)})_p', b, live & (ZF | SF | PF))
                L.append(self.wr(a0, '_p') + ' }')
        elif base == 'mul':
            b = self.bits(a0)
            if b == 32:
                L.append(f'{{ u64 _p = (u64)c->r[EAX] * (u32){self.rd(a0)}; c->r[EAX] = (u32)_p; c->r[EDX] = (u32)(_p >> 32); c->cf = c->of = c->r[EDX] != 0; }}')
            elif b == 16:
                L.append(f'{{ u32 _p = (u32)(u16)c->r[EAX] * (u16){self.rd(a0)}; c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | (u16)_p; c->r[EDX] = (c->r[EDX] & 0xFFFF0000u) | (u16)(_p >> 16); c->cf = c->of = (_p >> 16) != 0; }}')
            else:
                L.append(f'{{ u32 _p = (u32)(u8)c->r[EAX] * (u8){self.rd(a0)}; c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | (u16)_p; c->cf = c->of = (_p >> 8) != 0; }}')
        elif base in ('div', 'idiv'):
            b = self.bits(a0)
            if b == 32:
                if base == 'div':
                    L.append(f'{{ u32 _d = {self.rd(a0)}; u64 _n = ((u64)c->r[EDX] << 32) | c->r[EAX]; if (!_d) guest_int(c, 0x{ins.addr:08X}u, 0); c->r[EAX] = (u32)(_n / _d); c->r[EDX] = (u32)(_n % _d); }}')
                else:
                    L.append(f'{{ s32 _d = (s32){self.rd(a0)}; s64 _n = (s64)(((u64)c->r[EDX] << 32) | c->r[EAX]); if (!_d) guest_int(c, 0x{ins.addr:08X}u, 0); c->r[EAX] = (u32)(s32)(_n / _d); c->r[EDX] = (u32)(s32)(_n % _d); }}')
            elif b == 16:
                if base == 'div':
                    L.append(f'{{ u32 _d = (u16){self.rd(a0)}; u32 _n = ((c->r[EDX] & 0xFFFF) << 16) | (c->r[EAX] & 0xFFFF); if (!_d) guest_int(c, 0x{ins.addr:08X}u, 0); c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | (u16)(_n / _d); c->r[EDX] = (c->r[EDX] & 0xFFFF0000u) | (u16)(_n % _d); }}')
                else:
                    L.append(f'{{ s32 _d = (s16){self.rd(a0)}; s32 _n = (s32)(((c->r[EDX] & 0xFFFF) << 16) | (c->r[EAX] & 0xFFFF)); if (!_d) guest_int(c, 0x{ins.addr:08X}u, 0); c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | (u16)(_n / _d); c->r[EDX] = (c->r[EDX] & 0xFFFF0000u) | (u16)(_n % _d); }}')
            else:
                if base == 'div':
                    L.append(f'{{ u32 _d = (u8){self.rd(a0)}; u32 _n = c->r[EAX] & 0xFFFF; if (!_d) guest_int(c, 0x{ins.addr:08X}u, 0); c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | ((_n % _d) << 8) | (u8)(_n / _d); }}')
                else:
                    L.append(f'{{ s32 _d = (s8){self.rd(a0)}; s32 _n = (s16)(c->r[EAX] & 0xFFFF); if (!_d) guest_int(c, 0x{ins.addr:08X}u, 0); c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | ((u32)(u8)(_n % _d) << 8) | (u8)(_n / _d); }}')
        elif base == 'cdq':
            L.append('c->r[EDX] = (s32)c->r[EAX] < 0 ? 0xFFFFFFFFu : 0;')
        elif base == 'cwde':
            L.append('c->r[EAX] = (u32)(s32)(s16)c->r[EAX];')
        elif base == 'cbw':
            L.append('c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | (u16)(s16)(s8)c->r[EAX];')
        elif base == 'cwd':
            L.append('c->r[EDX] = (c->r[EDX] & 0xFFFF0000u) | ((s16)c->r[EAX] < 0 ? 0xFFFFu : 0);')
        elif base.startswith('set') and base[3:] in CC_READS or (base.startswith('set') and base[3:] in CC_ALIAS):
            L.append(self.wr(a0, f'CC_{cc_of(base, "set").upper()}(c) ? 1 : 0'))
        elif base.startswith('cmov'):
            L.append(f'if (CC_{cc_of(base, "cmov").upper()}(c)) {self.wr(a0, self.rd(a1))}')
        elif base in JCC:
            t = a0[1]
            L.append(f'if (CC_{cc_of(base, "j").upper()}(c)) {{ {self.target_code(t, False)} }}')
        elif base == 'jecxz':
            L.append(f'if (c->r[ECX] == 0) {{ {self.target_code(a0[1], False)} }}')
        elif base in ('loop', 'loope', 'loopne'):
            cond = {'loop': '', 'loope': ' && c->zf', 'loopne': ' && !c->zf'}[base]
            L.append(f'if (--c->r[ECX] != 0{cond}) {{ {self.target_code(a0[1], False)} }}')
        elif base == 'jmp':
            if a0[0] == 'imm':
                L.append(self.target_code(a0[1], True))
            elif ins.addr in self.an.jtables:
                targets = self.an.jtables[ins.addr]
                L.append(f'{{ u32 _t = {self.rd(a0)}; switch (_t) {{')
                for tg in sorted(set(targets)):
                    L.append(f'case 0x{tg:08X}u: {self.target_code(tg, True)}')
                L.append('default: jump_indirect(c, _t); return; } }')
            else:
                L.append(f'{{ u32 _t = {self.rd(a0)}; jump_indirect(c, _t); return; }}')
        elif base == 'call':
            if a0[0] == 'imm':
                t = a0[1]
                if t == nxt:
                    L.append(f'push32(c, 0x{nxt:08X}u);')
                elif t in self.an.funcs:
                    L.append(f'push32(c, 0x{nxt:08X}u); f_{t:08X}(c);')
                else:
                    L.append(f'push32(c, 0x{nxt:08X}u); call_indirect(c, 0x{t:08X}u);')
            else:
                L.append(f'{{ u32 _t = {self.rd(a0)}; push32(c, 0x{nxt:08X}u); call_indirect(c, _t); }}')
        elif base == 'ret':
            n = a0[1] if a0 else 0
            L.append(f'c->r[ESP] += {4 + n}; return;')
        elif base == 'leave':
            L.append('c->r[ESP] = c->r[EBP]; c->r[EBP] = pop32(c);')
        elif base == 'enter':
            size, lvl = a0[1], a1[1]
            if lvl != 0:
                raise Unsupported('enter level')
            L.append(f'push32(c, c->r[EBP]); c->r[EBP] = c->r[ESP]; c->r[ESP] -= {size};')
        elif base == 'cld':
            L.append('c->df = 0;')
        elif base == 'std':
            L.append('c->df = 1;')
        elif base == 'clc':
            L.append('c->cf = 0;')
        elif base == 'stc':
            L.append('c->cf = 1;')
        elif base == 'cmc':
            L.append('c->cf ^= 1;')
        elif base == 'lahf':
            L.append('c->r[EAX] = (c->r[EAX] & ~0xFF00u) | ((u32)(c->sf << 7 | c->zf << 6 | c->af << 4 | c->pf << 2 | 2 | c->cf) << 8);')
        elif base == 'sahf':
            L.append('{ u8 _h = (u8)(c->r[EAX] >> 8); c->sf = (_h >> 7) & 1; c->zf = (_h >> 6) & 1; c->af = (_h >> 4) & 1; c->pf = (_h >> 2) & 1; c->cf = _h & 1; }')
        elif base == 'salc':
            L.append('c->r[EAX] = (c->r[EAX] & ~0xFFu) | (c->cf ? 0xFF : 0);')
        elif base == 'pushfd':
            L.append('push32(c, 0x202u | c->cf | c->pf << 2 | c->af << 4 | c->zf << 6 | c->sf << 7 | c->df << 10 | c->of << 11);')
        elif base == 'popfd':
            L.append('{ u32 _f = pop32(c); c->cf = _f & 1; c->pf = (_f >> 2) & 1; c->af = (_f >> 4) & 1; c->zf = (_f >> 6) & 1; c->sf = (_f >> 7) & 1; c->df = (_f >> 10) & 1; c->of = (_f >> 11) & 1; }')
        elif base == 'pushal':
            L.append('{ u32 _s = c->r[ESP]; push32(c, c->r[EAX]); push32(c, c->r[ECX]); push32(c, c->r[EDX]); push32(c, c->r[EBX]); push32(c, _s); push32(c, c->r[EBP]); push32(c, c->r[ESI]); push32(c, c->r[EDI]); }')
        elif base == 'popal':
            L.append('c->r[EDI] = pop32(c); c->r[ESI] = pop32(c); c->r[EBP] = pop32(c); c->r[ESP] += 4; c->r[EBX] = pop32(c); c->r[EDX] = pop32(c); c->r[ECX] = pop32(c); c->r[EAX] = pop32(c);')
        elif base == 'bswap':
            L.append(self.wr(a0, f'__builtin_bswap32({self.rd(a0)})'))
        elif base in ('bt', 'bts', 'btr', 'btc'):
            b = self.bits(a0)
            if a0[0] == 'mem' and a1[0] == 'reg':
                L.append(f'{{ s32 _o = (s32){self.rd(a1)}; u32 _a = {self.ea(a0)} + (u32)((_o >> 3) & ~3); u32 _bit = _o & 31; u32 _v = rd32(_a); c->cf = (_v >> _bit) & 1;')
                op = {'bt': None, 'bts': '_v | (1u << _bit)', 'btr': '_v & ~(1u << _bit)', 'btc': '_v ^ (1u << _bit)'}[base]
                if op:
                    L.append(f'wr32(_a, {op});')
                L.append('}')
            else:
                e = None
                if a0[0] == 'mem':
                    e = self.t(); L.append(f'u32 {e} = {self.ea(a0)};')
                L.append(f'{{ u32 _bit = {self.rd(a1)} & {b - 1}; {utype(b)} _v = {self.rd(a0, e)}; c->cf = (_v >> _bit) & 1;')
                op = {'bt': None, 'bts': '_v | (1u << _bit)', 'btr': '_v & ~(1u << _bit)', 'btc': '_v ^ (1u << _bit)'}[base]
                if op:
                    L.append(self.wr(a0, op, e))
                L.append('}')
        elif base in ('bsf', 'bsr'):
            fn = '__builtin_ctz' if base == 'bsf' else '31 - __builtin_clz'
            L.append(f'{{ u32 _v = {self.rd(a1)}; c->zf = _v == 0; if (_v) {self.wr(a0, f"{fn}(_v)")} }}')
        elif base == 'xadd':
            b, ut, e = two(None)
            L.append(f'{{ {ut} _a = {self.rd(a0, e)}, _b = {self.rd(a1)}; {ut} _r = _a + _b;')
            L += self.flags_arith('add', '_a', '_b', '_r', b, live)
            L.append(self.wr(a1, '_a'))
            L.append(self.wr(a0, '_r', e) + ' }')
        elif base == 'cmpxchg':
            b, ut, e = two(None)
            acc = {8: '(u8)c->r[EAX]', 16: '(u16)c->r[EAX]', 32: 'c->r[EAX]'}[b]
            accw = {8: 'al', 16: 'ax', 32: 'eax'}[b]
            L.append(f'{{ {ut} _d = {self.rd(a0, e)}, _a = {acc}; {ut} _r = _a - _d;')
            L += self.flags_arith('sub', '_a', '_d', '_r', b, live | ZF)
            L.append(f'if (_a == _d) {{ {self.wr(a0, self.rd(a1), e)} }} else {{ {self.wr(("reg", accw, b // 8), "_d")} }} }}')
        elif base == 'cmpxchg8b':
            L.append(f'{{ u32 _a = {self.ea(a0)}; u64 _d = rd64(_a); u64 _x = ((u64)c->r[EDX] << 32) | c->r[EAX]; if (_d == _x) {{ wr64(_a, ((u64)c->r[ECX] << 32) | c->r[EBX]); c->zf = 1; }} else {{ c->r[EAX] = (u32)_d; c->r[EDX] = (u32)(_d >> 32); c->zf = 0; }} }}')
        elif base in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                      'cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw', 'scasd') and not (ops and ops[0][0] == 'reg' and ops[0][1].startswith('xmm')):
            L += self.string_op(base, rep, live)
        elif base == 'xlatb':
            L.append('c->r[EAX] = (c->r[EAX] & ~0xFFu) | rd8(c->r[EBX] + (u8)c->r[EAX]);')
        elif base == 'cpuid':
            L.append('guest_cpuid(c);')
        elif base == 'rdtsc':
            L.append('guest_rdtsc(c);')
        elif base == 'int3':
            L.append(f'guest_int(c, 0x{ins.addr:08X}u, 3); return;')
        elif base == 'int':
            L.append(f'guest_int(c, 0x{ins.addr:08X}u, {a0[1]}); return;')
        elif base[0] == 'f':
            L += self.fpu(ins, base, ops, live)
        else:
            raise Unsupported(mn)
        return L

    def try_fuse(self, ins, nx, live, kind):
        """cmp/test followed by jcc/setcc/cmovcc whose flags die there: one C comparison."""
        nb = nx.mn
        if nx.addr != ins.addr + ins.size or nx.addr in self.jtargets:
            return None
        if nb in JCC:
            cc = cc_of(nb, 'j')
        elif nb.startswith('set'):
            cc = cc_of(nb, 'set')
        elif nb.startswith('cmov'):
            cc = cc_of(nb, 'cmov')
        else:
            return None
        if self.live_out.get(nx.addr, ALL) & ALL:
            return None  # flags still needed after the consumer
        if cc in ('o', 'no', 'p', 'np'):
            return None
        a0, a1 = ins.ops
        b = self.bits(a0)
        ut, st = utype(b), stype(b)
        L = []
        e = None
        if a0[0] == 'mem':
            e = self.t(); L.append(f'u32 {e} = {self.ea(a0)};')
        av, bv = self.rd(a0, e), self.rd(a1)
        if kind == 'cmp':
            conds = {'e': '_a == _b', 'ne': '_a != _b', 'b': '_a < _b', 'ae': '_a >= _b', 'be': '_a <= _b', 'a': '_a > _b',
                     'l': f'({st})_a < ({st})_b', 'ge': f'({st})_a >= ({st})_b', 'le': f'({st})_a <= ({st})_b',
                     'g': f'({st})_a > ({st})_b', 's': f'({st})({ut})(_a - _b) < 0', 'ns': f'({st})({ut})(_a - _b) >= 0'}
            pre = f'{ut} _a = {av}, _b = ({ut}){bv};'
        else:
            conds = {'e': '_r == 0', 'ne': '_r != 0', 's': f'({st})_r < 0', 'ns': f'({st})_r >= 0', 'l': f'({st})_r < 0',
                     'ge': f'({st})_r >= 0', 'le': f'({st})_r <= 0', 'g': f'({st})_r > 0', 'b': '0', 'ae': '1',
                     'be': '_r == 0', 'a': '_r != 0'}
            pre = f'{ut} _r = {av} & ({ut}){bv};'
        cond = conds[cc]
        self.fused_next = nx.addr
        if nb in JCC:
            L.append(f'{{ {pre} if ({cond}) {{ {self.target_code(nx.ops[0][1], False)} }} }}')
        elif nb.startswith('set'):
            L.append(f'{{ {pre} {self.wr(nx.ops[0], f"({cond}) ? 1 : 0")} }}')
        else:
            L.append(f'{{ {pre} if ({cond}) {{ {self.wr(nx.ops[0], self.rd(nx.ops[1]))} }} }}')
        return L

    def string_op(self, base, rep, live):
        sz = {'b': 1, 'w': 2, 'd': 4}[base[-1]]
        b = sz * 8
        ut = utype(b)
        acc = {1: '(u8)c->r[EAX]', 2: '(u16)c->r[EAX]', 4: 'c->r[EAX]'}[sz]
        step = f'(c->df ? -{sz} : {sz})'
        kind = base[:-1]
        body = {
            'movs': f'wr{b}(c->r[EDI], rd{b}(c->r[ESI])); c->r[ESI] += {step}; c->r[EDI] += {step};',
            'stos': f'wr{b}(c->r[EDI], {acc}); c->r[EDI] += {step};',
            'lods': (f'c->r[EAX] = (c->r[EAX] & ~0x{(1 << b) - 1:X}u) | rd{b}(c->r[ESI]);' if sz < 4 else 'c->r[EAX] = rd32(c->r[ESI]);') + f' c->r[ESI] += {step};',
        }
        if kind in body:
            if rep:
                if kind == 'movs' and sz == 4:
                    return [f'{{ u32 _n = c->r[ECX]; if (_n) {{ if (!c->df && (c->r[EDI] - c->r[ESI] >= _n * 4 || c->r[ESI] - c->r[EDI] >= _n * 4)) {{ memmove(H(c->r[EDI]), H(c->r[ESI]), _n * 4); c->r[ESI] += _n * 4; c->r[EDI] += _n * 4; c->r[ECX] = 0; }} else while (c->r[ECX]) {{ {body[kind]} c->r[ECX]--; }} }} }}']
                if kind == 'stos' and sz == 4:
                    return [f'{{ u32 _n = c->r[ECX]; if (_n && !c->df) {{ u32 _v = c->r[EAX]; u32* _p = (u32*)H(c->r[EDI]); for (u32 _i = 0; _i < _n; ++_i) ((ga32*)_p)[_i] = _v; c->r[EDI] += _n * 4; c->r[ECX] = 0; }} else while (c->r[ECX]) {{ {body[kind]} c->r[ECX]--; }} }}']
                if kind == 'stos' and sz == 1:
                    return [f'{{ u32 _n = c->r[ECX]; if (_n && !c->df) {{ memset(H(c->r[EDI]), (u8)c->r[EAX], _n); c->r[EDI] += _n; c->r[ECX] = 0; }} else while (c->r[ECX]) {{ {body[kind]} c->r[ECX]--; }} }}']
                return [f'while (c->r[ECX]) {{ {body[kind]} c->r[ECX]--; }}']
            return [body[kind]]
        # cmps / scas
        if kind == 'cmps':
            one = f'{{ {ut} _a = rd{b}(c->r[ESI]), _b = rd{b}(c->r[EDI]); {ut} _r = _a - _b; c->r[ESI] += {step}; c->r[EDI] += {step};'
        else:
            one = f'{{ {ut} _a = {acc}, _b = rd{b}(c->r[EDI]); {ut} _r = _a - _b; c->r[EDI] += {step};'
        fl = ' '.join(self.flags_arith('sub', '_a', '_b', '_r', b, ALL))
        if not rep:
            return [one + ' ' + fl + ' }']
        stop = 'if (!c->zf) break;' if rep in ('repe', 'repz', 'rep') else 'if (c->zf) break;'
        return [f'while (c->r[ECX]) {{ {one} {fl} c->r[ECX]--; {stop} }} }}'.replace('} }', '}', 0)]

    # ---- x87
    def fsrc(self, op):
        """value of a memory/st operand as double."""
        if op[0] == 'reg':
            return f'ST(c, {int(op[1][3])})'
        b = self.bits(op)
        return {32: f'(double)rdf32({self.ea(op)})', 64: f'rdf64({self.ea(op)})', 80: f'rdf80({self.ea(op)})'}[b]

    def fisrc(self, op):
        b = self.bits(op)
        return {16: f'(double)(s16)rd16({self.ea(op)})', 32: f'(double)(s32)rd32({self.ea(op)})', 64: f'(double)(s64)rd64({self.ea(op)})'}[b]

    def fpu(self, ins, base, ops, live):
        L = []
        a0 = ops[0] if ops else None
        a1 = ops[1] if len(ops) > 1 else None
        sti = lambda o: int(o[1][3]) if o and o[0] == 'reg' and o[1].startswith('st') else None
        arith = {'add': '+', 'mul': '*', 'sub': '-', 'div': '/'}
        if base == 'fld':
            L.append(f'{{ double _v = {self.fsrc(a0)}; fpush(c, _v); }}')
        elif base == 'fild':
            L.append(f'fpush(c, {self.fisrc(a0)});')
        elif base in ('fld1', 'fldz', 'fldpi', 'fldl2e', 'fldln2', 'fldlg2', 'fldl2t'):
            v = {'fld1': '1.0', 'fldz': '0.0', 'fldpi': 'M_PI', 'fldl2e': 'M_LOG2E', 'fldln2': 'M_LN2',
                 'fldlg2': '0.30102999566398119521', 'fldl2t': '3.32192809488736234787'}[base]
            L.append(f'fpush(c, {v});')
        elif base in ('fst', 'fstp'):
            if a0[0] == 'reg':
                L.append(f'ST(c, {sti(a0)}) = ST(c, 0);')
            else:
                b = self.bits(a0)
                L.append({32: f'wrf32({self.ea(a0)}, (float)ST(c, 0));', 64: f'wrf64({self.ea(a0)}, ST(c, 0));',
                          80: f'wrf80({self.ea(a0)}, ST(c, 0));'}[b])
            if base == 'fstp':
                L.append('fpop(c);')
        elif base in ('fist', 'fistp', 'fisttp'):
            b = self.bits(a0)
            v = 'fround(c, ST(c, 0))' if base != 'fisttp' else '(s64)ST(c, 0)'
            L.append(f'wr{b}({self.ea(a0)}, ({utype(b)})({stype(b)}){v});')
            if base != 'fist':
                L.append('fpop(c);')
        elif base == 'fxch':
            i = sti(ops[-1]) if ops else 1  # capstone lists the implicit st(0) first: fxch st(0), st(i)
            if i == 0 and len(ops) == 1:
                i = 1
            L.append(f'{{ double _v = ST(c, 0); ST(c, 0) = ST(c, {i}); ST(c, {i}) = _v; }}')
        elif base[1:4] in arith or base[1:5] in ('subr', 'divr') or base[:2] == 'fi' and base[2:5] in arith:
            intop = base.startswith('fi')
            core = base[2:] if intop else base[1:]
            pop = core.endswith('p')
            if pop:
                core = core[:-1]
            rev = core.endswith('r')
            if rev:
                core = core[:-1]
            op = arith[core]
            if intop:
                src = self.fisrc(a0)
                L.append(f'ST(c, 0) = {"(" + src + ") " + op + " ST(c, 0)" if rev else "ST(c, 0) " + op + " (" + src + ")"};')
            elif a0 is None or (a0[0] == 'reg' and a1 is None and pop):
                # faddp / fsubrp with implied st(1), st
                i = sti(a0) if a0 else 1
                expr = f'ST(c, 0) {op} ST(c, {i})' if rev else f'ST(c, {i}) {op} ST(c, 0)'
                L.append(f'ST(c, {i}) = {expr}; fpop(c);')
            elif a0[0] == 'reg' and a1 is None:
                i = sti(a0)
                L.append(f'ST(c, 0) = {"ST(c, " + str(i) + ") " + op + " ST(c, 0)" if rev else "ST(c, 0) " + op + " ST(c, " + str(i) + ")"};')
            elif a0[0] == 'mem':
                src = self.fsrc(a0)
                L.append(f'ST(c, 0) = {"(" + src + ") " + op + " ST(c, 0)" if rev else "ST(c, 0) " + op + " (" + src + ")"};')
            else:
                d, s = sti(a0), sti(a1)
                expr = f'ST(c, {s}) {op} ST(c, {d})' if rev else f'ST(c, {d}) {op} ST(c, {s})'
                L.append(f'ST(c, {d}) = {expr};')
                if pop:
                    L.append('fpop(c);')
        elif base in ('fcom', 'fcomp', 'fucom', 'fucomp', 'ficom', 'ficomp'):
            if a0 is None:
                src = 'ST(c, 1)'
            elif base.startswith('fi'):
                src = self.fisrc(a0)
            else:
                src = self.fsrc(a0)
            L.append(f'fcom_set(c, ST(c, 0), {src});')
            if base.endswith('p'):
                L.append('fpop(c);')
        elif base in ('fcompp', 'fucompp'):
            L.append('fcom_set(c, ST(c, 0), ST(c, 1)); fpop(c); fpop(c);')
        elif base in ('fcomi', 'fucomi', 'fcomip', 'fucomip', 'fcompi', 'fucompi'):
            i = sti(a1) if a1 else (sti(a0) if a0 else 1)
            L.append(f'fcomi_set(c, ST(c, 0), ST(c, {i}));')
            if base.endswith('p') or base.endswith('pi'):
                L.append('fpop(c);')
        elif base == 'ftst':
            L.append('fcom_set(c, ST(c, 0), 0.0);')
        elif base == 'fxam':
            L.append('{ double _v = ST(c, 0); u16 _s = c->fpu_sw & ~0x4700; if (signbit(_v)) _s |= 0x200; if (isnan(_v)) _s |= 0x100; else if (isinf(_v)) _s |= 0x500; else if (_v == 0) _s |= 0x4000; else _s |= 0x400; c->fpu_sw = _s; }')
        elif base == 'fchs':
            L.append('ST(c, 0) = -ST(c, 0);')
        elif base == 'fabs':
            L.append('ST(c, 0) = fabs(ST(c, 0));')
        elif base == 'fsqrt':
            L.append('ST(c, 0) = sqrt(ST(c, 0));')
        elif base == 'fsin':
            L.append('ST(c, 0) = sin(ST(c, 0));')
        elif base == 'fcos':
            L.append('ST(c, 0) = cos(ST(c, 0));')
        elif base == 'fsincos':
            L.append('{ double _v = ST(c, 0); ST(c, 0) = sin(_v); fpush(c, cos(_v)); }')
        elif base == 'fptan':
            L.append('ST(c, 0) = tan(ST(c, 0)); fpush(c, 1.0);')
        elif base == 'fpatan':
            L.append('ST(c, 1) = atan2(ST(c, 1), ST(c, 0)); fpop(c);')
        elif base == 'f2xm1':
            L.append('ST(c, 0) = exp2(ST(c, 0)) - 1.0;')
        elif base == 'fyl2x':
            L.append('ST(c, 1) = ST(c, 1) * log2(ST(c, 0)); fpop(c);')
        elif base == 'fyl2xp1':
            L.append('ST(c, 1) = ST(c, 1) * log2(ST(c, 0) + 1.0); fpop(c);')
        elif base == 'fscale':
            L.append('ST(c, 0) = ldexp(ST(c, 0), (int)trunc(ST(c, 1)));')
        elif base == 'frndint':
            L.append('ST(c, 0) = (double)fround(c, ST(c, 0));')
        elif base in ('fprem', 'fprem1'):
            L.append('ST(c, 0) = ' + ('fmod' if base == 'fprem' else 'remainder') + '(ST(c, 0), ST(c, 1)); c->fpu_sw &= ~0x400;')
        elif base in ('fnstsw', 'fstsw'):
            if a0[0] == 'reg':
                L.append('c->r[EAX] = (c->r[EAX] & 0xFFFF0000u) | fstsw(c);')
            else:
                L.append(f'wr16({self.ea(a0)}, fstsw(c));')
        elif base in ('fnstcw', 'fstcw'):
            L.append(f'wr16({self.ea(a0)}, c->fpu_cw);')
        elif base == 'fldcw':
            L.append(f'c->fpu_cw = rd16({self.ea(a0)});')
        elif base in ('fnclex', 'fclex'):
            L.append('c->fpu_sw &= 0x7F00;')
        elif base in ('fninit', 'finit'):
            L.append('c->fpu_cw = 0x37F; c->fpu_sw = 0; c->top = 0;')
        elif base == 'ffree':
            pass
        elif base in ('fnsave', 'fsave', 'frstor', 'fnstenv', 'fstenv', 'fldenv'):
            L.append(f'unimpl(c, 0x{ins.addr:08X}u, "{base}");')
        elif base.startswith('fcmov'):
            cc = {'fcmovb': 'B', 'fcmove': 'E', 'fcmovbe': 'BE', 'fcmovu': 'P', 'fcmovnb': 'AE', 'fcmovne': 'NE',
                  'fcmovnbe': 'A', 'fcmovnu': 'NP'}[base]
            L.append(f'if (CC_{cc}(c)) ST(c, 0) = ST(c, {sti(a1)});')
        else:
            raise Unsupported(base)
        return L


# ------------------------------------------------------------------ functions and liveness
def build_function(an, im, entry):
    """Instruction addresses of the function at entry (jumps followed; other function entries are tail calls)."""
    seen = set()
    work = [entry]
    while work:
        va = work.pop()
        while va not in seen and im.in_code(va):
            if va != entry and va in an.funcs:
                break
            ins = an.insns.get(va)
            if ins is None:
                break
            seen.add(va)
            mn = ins.mn.split()[-1]
            if mn in ('ret', 'retf', 'iretd', 'int3', 'hlt', 'ud2'):
                break
            if (mn in JCC or mn.startswith('loop') or mn == 'jecxz') and ins.ops[0][0] == 'imm':
                work.append(ins.ops[0][1])
            if mn == 'jmp':
                if ins.ops[0][0] == 'imm':
                    work.append(ins.ops[0][1])
                elif va in an.jtables:
                    work.extend(an.jtables[va])
                break
            if mn == 'int' :
                break
            va += ins.size
    return sorted(seen)


def successors(an, ins, inset):
    mn = ins.mn.split()[-1]
    nxt = ins.addr + ins.size
    if mn in ('ret', 'retf', 'iretd', 'int3', 'hlt', 'ud2', 'int'):
        return [], True
    if mn == 'jmp':
        if ins.ops[0][0] == 'imm':
            t = ins.ops[0][1]
            return ([t], False) if t in inset else ([], True)
        if ins.addr in an.jtables:
            ts = [t for t in an.jtables[ins.addr] if t in inset]
            return ts, len(ts) != len(an.jtables[ins.addr])
        return [], True
    s, exit_ = [], False
    if (mn in JCC or mn.startswith('loop') or mn == 'jecxz') and ins.ops[0][0] == 'imm':
        t = ins.ops[0][1]
        if t in inset:
            s.append(t)
        else:
            exit_ = True
    if nxt in inset:
        s.append(nxt)
    else:
        exit_ = True
    return s, exit_


def liveness(an, addrs):
    inset = set(addrs)
    eff = {}
    succ = {}
    for va in addrs:
        ins = an.insns[va]
        r, w = flag_effect(ins)
        if ins.mn.split()[-1] == 'call':
            r, w = 0, ALL
        eff[va] = (r, w)
        succ[va] = successors(an, ins, inset)
    live_in = {va: 0 for va in addrs}
    live_out = {}
    changed = True
    while changed:
        changed = False
        for va in reversed(addrs):
            s, ex = succ[va]
            out = ALL if ex else 0
            for t in s:
                out |= live_in[t]
            r, w = eff[va]
            li = r | (out & ~w)
            live_out[va] = out
            if li != live_in[va]:
                live_in[va] = li
                changed = True
    return live_out


def recompile(gamedir, outdir):
    an = Analysis(gamedir).run()
    os.makedirs(outdir, exist_ok=True)
    funcs = sorted(an.funcs)
    stats = collections.Counter()
    unsup = collections.Counter()
    chunks = collections.defaultdict(list)
    for f in funcs:
        im = an.image_of(f)
        addrs = build_function(an, im, f)
        if not addrs:
            chunks[im.lname].append(f'void f_{f:08X}(Cpu* c) {{ unimpl(c, 0x{f:08X}u, "undecoded"); }}\n')
            continue
        live = liveness(an, addrs)
        em = Emitter(an, im, f, addrs, live)
        body = []
        skip = None
        pos = {va: i for i, va in enumerate(addrs)}
        for i, va in enumerate(addrs):
            ins = an.insns[va]
            if va == skip:
                continue
            nx = an.insns.get(addrs[i + 1]) if i + 1 < len(addrs) else None
            em.fused_next = None
            try:
                code = em.emit(ins, live.get(va, ALL), nx)
            except Unsupported as e:
                unsup[str(e)] += 1
                code = [f'unimpl(c, 0x{va:08X}u, "{ins.mn} {ins.op_str}");', 'return;']
            body.append((va, code))
            stats['insns'] += 1
            # the fused consumer was emitted with this instruction
            if em.fused_next is not None:
                skip = em.fused_next
                # its fallthrough handled below via its own address
                body.append((em.fused_next, None))
            last = em.fused_next if em.fused_next is not None else va
            lins = an.insns[last]
            s, ex = successors(an, lins, em.inset)
            nxt = lins.addr + lins.size
            mn = lins.mn.split()[-1]
            falls = mn not in ('ret', 'retf', 'iretd', 'int3', 'hlt', 'ud2', 'jmp', 'int')
            following = addrs[pos[last] + 1] if pos[last] + 1 < len(addrs) else None
            if falls and following != nxt:
                body.append((None, [em.target_code(nxt, True)]))
        wrap = WRAP.get(f)
        lines = [f'static void b_{f:08X}(Cpu* c) {{' if wrap else f'void f_{f:08X}(Cpu* c) {{']
        if addrs[0] != f:  # blocks below the entry (reached by jumps): start at the entry
            em.labels.add(f)
            lines.append(f'  goto L_{f:08X};')
        for va, code in body:
            if va is not None and va in em.labels:
                lines.append(f'L_{va:08X}:;')
            if code:
                lines.extend('  ' + l for l in code)
        lines.append('}\n')
        if wrap:
            lines.append(f'void f_{f:08X}(Cpu* c) {{ extern int {wrap}; {wrap}++; b_{f:08X}(c); {wrap}--; }}\n')
        chunks[im.lname].append('\n'.join(lines))
        stats['funcs'] += 1
    hdr = '#include "cpu.h"\n#include "funcs.h"\n'
    nfiles = 0
    for mod, fs in chunks.items():
        per = 400
        for n in range(0, len(fs), per):
            name = f'{mod.replace(".", "_")}_{n // per:03d}.c'
            with open(os.path.join(outdir, name), 'w') as fp:
                fp.write(hdr)
                fp.write('\n'.join(fs[n:n + per]))
            nfiles += 1
    with open(os.path.join(outdir, 'funcs.h'), 'w') as fp:
        fp.write('#pragma once\n#include "cpu.h"\n')
        for f in funcs:
            fp.write(f'void f_{f:08X}(Cpu* c);\n')
    with open(os.path.join(outdir, 'functions.c'), 'w') as fp:
        fp.write('#include "cpu.h"\n#include "funcs.h"\n#include "tables.h"\n')
        fp.write(f'const unsigned guest_fn_count = {len(funcs)};\n')
        fp.write('const GuestFnEntry guest_fns[] = {\n')
        for f in funcs:
            fp.write(f'  {{0x{f:08X}u, f_{f:08X}}},\n')
        fp.write('};\n')
    with open(os.path.join(outdir, 'images.c'), 'w') as fp:
        fp.write('#include "tables.h"\n')
        imps = []
        for im in an.images:
            for va, (dll, sym) in sorted(im.imports.items()):
                imps.append((va, dll, sym))
        fp.write(f'const unsigned guest_import_count = {len(imps)};\n')
        fp.write('const GuestImport guest_imports[] = {\n')
        for va, dll, sym in imps:
            if isinstance(sym, int):
                fp.write(f'  {{0x{va:08X}u, "{dll}", 0, {sym}}},\n')
            else:
                fp.write(f'  {{0x{va:08X}u, "{dll}", "{sym}", 0}},\n')
        fp.write('};\n')
        fp.write(f'const unsigned guest_image_count = {len(an.images)};\n')
        fp.write('const GuestImage guest_images[] = {\n')
        for im in an.images:
            fp.write(f'  {{"{im.name}", 0x{im.base:08X}u, 0x{im.pref:08X}u, 0x{im.size:X}u, 0x{im.va(im.entry) if im.entry else 0:08X}u}},\n')
        fp.write('};\n')
    print(f'functions {stats["funcs"]} insns {stats["insns"]} files {nfiles}')
    print('unsupported:', ' '.join(f'{k}:{v}' for k, v in unsup.most_common(40)))


if __name__ == '__main__':
    recompile(sys.argv[1], sys.argv[2])
