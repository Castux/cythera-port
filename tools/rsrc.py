#!/usr/bin/env python3
"""Classic Mac OS resource fork parser.

Usage: rsrc.py <file.rsrc> [list | dump TYPE ID OUT]
"""
import struct, sys

def parse(data):
    doff, moff, dlen, mlen = struct.unpack('>IIII', data[:16])
    m = data[moff:moff+mlen]
    tlist_off, nlist_off = struct.unpack('>HH', m[24:28])
    ntypes = struct.unpack('>H', m[tlist_off:tlist_off+2])[0] + 1
    res = {}
    for i in range(ntypes):
        e = tlist_off + 2 + i*8
        rtype = m[e:e+4].decode('mac_roman')
        cnt, roff = struct.unpack('>HH', m[e+4:e+8])
        for j in range(cnt+1):
            r = tlist_off + roff + j*12
            rid, noff, attrs_off = struct.unpack('>hHI', m[r:r+8])
            attrs = attrs_off >> 24
            off = attrs_off & 0xFFFFFF
            name = None
            if noff != 0xFFFF:
                n = nlist_off + noff
                name = m[n+1:n+1+m[n]].decode('mac_roman')
            ln = struct.unpack('>I', data[doff+off:doff+off+4])[0]
            body = data[doff+off+4:doff+off+4+ln]
            res[(rtype, rid)] = (name, attrs, body)
    return res

if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    res = parse(data)
    cmd = sys.argv[2] if len(sys.argv) > 2 else 'list'
    if cmd == 'list':
        for (t, i), (n, a, b) in sorted(res.items()):
            print(f"{t!r:8} {i:6} {len(b):8} attr={a:02x} {n or ''}")
    elif cmd == 'summary':
        from collections import Counter
        c = Counter(); s = Counter()
        for (t, i), (n, a, b) in res.items():
            c[t] += 1; s[t] += len(b)
        for t in sorted(c): print(f"{t!r:8} {c[t]:5} {s[t]:9}")
    elif cmd == 'dump':
        t, i, out = sys.argv[3], int(sys.argv[4]), sys.argv[5]
        open(out, 'wb').write(res[(t, i)][2])
