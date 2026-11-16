#!/usr/bin/env python3
"""mpqcheck.py <mpq...> -- <name...>: is a file in an MPQ's hash table (and with which locale)."""
import sys, struct
def crypt_table():
    t = [0] * 0x500; seed = 0x00100001
    for i in range(0x100):
        idx = i
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB; a = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB; b = seed & 0xFFFF
            t[idx] = a | b; idx += 0x100
    return t
CT = crypt_table()
def h(s, kind):
    s1, s2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in s.upper().encode('latin-1'):
        s1 = (CT[(kind << 8) + ch] ^ (s1 + s2)) & 0xFFFFFFFF
        s2 = (ch + s1 + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
    return s1
def decrypt(data, key):
    out = []; s2 = 0xEEEEEEEE
    for (v,) in struct.iter_unpack('<I', data):
        s2 = (s2 + CT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        d = v ^ ((key + s2) & 0xFFFFFFFF); out.append(d)
        key = ((~key << 0x15) + 0x11111111 | key >> 0x0B) & 0xFFFFFFFF
        s2 = (d + s2 + (s2 << 5) + 3) & 0xFFFFFFFF
    return out
args = sys.argv[1:]; i = args.index('--'); mpqs, names = args[:i], args[i+1:]
for m in mpqs:
    f = open(m, 'rb'); d = f.read(0x100000)
    off = d.find(b'MPQ\x1a')
    hdr = struct.unpack_from('<4sIIHHIIII', d, off)
    hpos, bpos, hcount = hdr[5] + off, hdr[6] + off, hdr[7]
    f.seek(hpos); ht = decrypt(f.read(hcount * 16), h('(hash table)', 3))
    for n in names:
        a, b, start = h(n, 1), h(n, 2), h(n, 0) % hcount
        found = []
        for k in range(hcount):
            j = (start + k) % hcount; e = ht[4*j:4*j+4]
            if e[3] == 0xFFFFFFFF: break
            if e[0] == a and e[1] == b and e[3] != 0xFFFFFFFE: found.append(f'locale={e[2] & 0xFFFF:04x} plat={e[2] >> 16:x} block={e[3]}')
        print(f'{m.split("/")[-1]:14} {n}: {found or "-"}')
