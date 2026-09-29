#!/usr/bin/env python3
"""PEF (Preferred Executable Format) container parser for classic Mac OS PPC code.

Usage: pef.py <file> [info | imports | exports | relocs]
"""
import struct, sys

class Section:
    pass

class PEF:
    def __init__(self, data):
        self.data = data
        (tag1, tag2, arch, fmtver, ts, oldDef, oldImp, cur,
         nsec, ninst, _) = struct.unpack('>4s4s4sIIIIIHHI', data[:40])
        assert tag1 == b'Joy!' and tag2 == b'peff' and arch == b'pwpc'
        self.sections = []
        for i in range(nsec):
            h = data[40 + i*28: 40 + (i+1)*28]
            s = Section()
            (s.name_off, s.default_addr, s.total_len, s.unpacked_len, s.packed_len,
             s.container_off, s.kind, s.share, s.align, _) = struct.unpack('>iIIIIIBBBB', h)
            s.raw = data[s.container_off:s.container_off + s.packed_len]
            self.sections.append(s)
        self.loader = next(s for s in self.sections if s.kind == 4)
        self.parse_loader()

    def section_data(self, idx):
        s = self.sections[idx]
        if s.kind == 2:
            return unpack_pidata(s.raw, s.unpacked_len) + bytes(s.total_len - s.unpacked_len)
        return s.raw + bytes(s.total_len - len(s.raw))

    def parse_loader(self):
        L = self.loader.raw
        (self.main_sec, self.main_off, self.init_sec, self.init_off,
         self.term_sec, self.term_off, nlib, nimp, nrelsec, reloc_off,
         strtab_off, hash_off, hash_pow, nexp) = struct.unpack('>iIiIiIIIIIIIII', L[:56])
        self.strtab = strtab_off
        self.libs = []
        p = 56
        for i in range(nlib):
            (name_off, oldimp, cur, nsym, firstsym, opts, _, _) = struct.unpack('>IIIIIBBH', L[p:p+24])
            self.libs.append((self.cstr(name_off), firstsym, nsym, opts))
            p += 24
        self.imports = []
        for i in range(nimp):
            w = struct.unpack('>I', L[p:p+4])[0]
            cls = w >> 24; name = self.cstr(w & 0xFFFFFF)
            lib = next(l[0] for l in self.libs if l[1] <= i < l[1] + l[2])
            self.imports.append((lib, name, cls & 0x0F, cls >> 4))
            p += 4
        self.relsecs = []
        for i in range(nrelsec):
            sidx, _, cnt, first = struct.unpack('>HHII', L[p:p+12])
            self.relsecs.append((sidx, cnt, first))
            p += 12
        self.reloc_instr_off = reloc_off
        # exports
        hcount = 1 << hash_pow
        keytab = hash_off + hcount*4
        symtab = keytab + nexp*4
        self.exports = []
        for i in range(nexp):
            key = struct.unpack('>I', L[keytab+i*4:keytab+i*4+4])[0]
            cls_name, val, sec = struct.unpack('>IIh', L[symtab+i*10:symtab+i*10+10])
            nlen = key >> 16
            noff = cls_name & 0xFFFFFF
            name = L[strtab_off+noff:strtab_off+noff+nlen].decode('mac_roman')
            self.exports.append((name, cls_name >> 24, sec, val))

    def cstr(self, off):
        L = self.loader.raw
        s = self.strtab + off
        e = L.index(b'\0', s)
        return L[s:e].decode('mac_roman')

def unpack_pidata(raw, outlen):
    out = bytearray(); p = 0
    def arg():
        nonlocal p
        v = 0
        while True:
            b = raw[p]; p += 1
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80: return v
    while p < len(raw):
        b = raw[p]; p += 1
        op = b >> 5; cnt = b & 0x1F
        if cnt == 0: cnt = arg()
        if op == 0:
            out += bytes(cnt)
        elif op == 1:
            out += raw[p:p+cnt]; p += cnt
        elif op == 2:
            rc = arg(); blk = raw[p:p+cnt]; p += cnt
            out += blk * (rc + 1)
        elif op == 3:
            ccount = cnt; ncount = arg(); rcount = arg()
            common = raw[p:p+ccount]; p += ccount
            for i in range(rcount):
                out += common; out += raw[p:p+ncount]; p += ncount
            out += common
        elif op == 4:
            ccount = cnt; ncount = arg(); rcount = arg()
            for i in range(rcount):
                out += bytes(ccount); out += raw[p:p+ncount]; p += ncount
            out += bytes(ccount)
        else:
            raise ValueError('bad pidata op %d' % op)
    return bytes(out)

if __name__ == '__main__':
    pef = PEF(open(sys.argv[1], 'rb').read())
    cmd = sys.argv[2] if len(sys.argv) > 2 else 'info'
    if cmd == 'info':
        for i, s in enumerate(pef.sections):
            print(f"sec {i}: kind={s.kind} addr={s.default_addr:#x} total={s.total_len:#x} unpacked={s.unpacked_len:#x} packed={s.packed_len:#x} off={s.container_off:#x}")
        print(f"main {pef.main_sec}:{pef.main_off:#x} init {pef.init_sec}:{pef.init_off:#x} term {pef.term_sec}:{pef.term_off:#x}")
        for l in pef.libs: print('lib', l)
        print(len(pef.imports), 'imports;', len(pef.exports), 'exports')
    elif cmd == 'imports':
        for i, (lib, name, cls, flags) in enumerate(pef.imports):
            print(f"{i:4} {lib:20} {name:36} cls={cls} fl={flags}")
    elif cmd == 'exports':
        for e in pef.exports: print(e)
