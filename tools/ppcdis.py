#!/usr/bin/env python3
"""Symbolizing PowerPC disassembler for the Cythera PEF binary.

Loads the PEF container, applies relocations (code at CODE_BASE, data at
DATA_BASE, imports as IMPORT_BASE+i), recovers function names from
CodeWarrior traceback tables, and resolves TOC-relative references.

Usage:
  ppcdis.py <pef> funcs                 list functions (addr size name)
  ppcdis.py <pef> dis <name|addr> ...   disassemble functions
  ppcdis.py <pef> all > out.s           disassemble everything
  ppcdis.py <pef> xref <name>           callers of a function/import
"""
import struct, sys, re, os, pickle
sys.path.insert(0, os.path.dirname(__file__))
from pef import PEF
import capstone

CODE_BASE = 0x00000000
DATA_BASE = 0x00100000
IMPORT_BASE = 0x0F000000

def demangle(n):
    m = re.match(r'^\.?(.+?)__(\d+)(.*)$', n)
    if not m: return n.lstrip('.')
    fn, ln, rest = m.group(1), int(m.group(2)), m.group(3)
    cls = rest[:ln]
    if len(cls) < ln: return n.lstrip('.')
    return f"{cls}::{fn}"

class Image:
    def __init__(self, path):
        self.pef = PEF(open(path, 'rb').read())
        self.code = bytearray(self.pef.section_data(0))
        self.datao = bytearray(self.pef.section_data(1))
        self.data = bytearray(self.datao)
        self.bases = [CODE_BASE, DATA_BASE]
        self.reloc_targets = {}  # data offset -> kind
        self.apply_relocs()
        # main TVector
        mo = self.pef.main_off
        self.main_code = self.dword(DATA_BASE + mo)
        self.toc = self.dword(DATA_BASE + mo + 4)
        self.find_tracebacks()

    def dword(self, addr):
        if DATA_BASE <= addr < DATA_BASE + len(self.data):
            o = addr - DATA_BASE
            return struct.unpack('>I', self.data[o:o+4])[0]
        if CODE_BASE <= addr < CODE_BASE + len(self.code):
            o = addr - CODE_BASE
            return struct.unpack('>I', self.code[o:o+4])[0]
        return None

    def apply_relocs(self):
        L = self.pef.loader.raw
        imports = [IMPORT_BASE + i for i in range(len(self.pef.imports))]
        for sidx, cnt, first in self.pef.relsecs:
            buf = self.data if sidx == 1 else self.code
            ins = L[self.pef.reloc_instr_off + first*2: self.pef.reloc_instr_off + (first+cnt)*2]
            hw = struct.unpack('>%dH' % cnt, ins)
            raddr = 0; imp = 0; sectC = self.bases[0]; sectD = self.bases[1]
            def add(off, val, kind):
                v = struct.unpack('>I', buf[off:off+4])[0]
                buf[off:off+4] = struct.pack('>I', (v + val) & 0xFFFFFFFF)
                if sidx == 1: self.reloc_targets[off] = kind
            i = 0
            hist = []
            def run(ip):
                nonlocal raddr, imp, sectC, sectD
                h = hw[ip]
                if h >> 14 == 0:
                    skip = (h >> 6) & 0xFF; rc = h & 0x3F
                    raddr += skip*4
                    for _ in range(rc): add(raddr, sectD, 'D'); raddr += 4
                    return 1
                if h >> 13 == 2:
                    sub = (h >> 9) & 0xF; n = (h & 0x1FF) + 1
                    for _ in range(n):
                        if sub == 0: add(raddr, sectC, 'C'); raddr += 4
                        elif sub == 1: add(raddr, sectD, 'D'); raddr += 4
                        elif sub == 2: add(raddr, sectC, 'C'); add(raddr+4, sectD, 'D'); raddr += 12
                        elif sub == 3: add(raddr, sectC, 'C'); add(raddr+4, sectD, 'D'); raddr += 8
                        elif sub == 4: add(raddr, sectD, 'D'); raddr += 8
                        elif sub == 5: add(raddr, imports[imp], 'I'); imp += 1; raddr += 4
                        else: raise ValueError('run sub %d' % sub)
                    return 1
                if h >> 13 == 3:
                    sub = (h >> 9) & 0xF; idx = h & 0x1FF
                    if sub == 0: add(raddr, imports[idx], 'I'); imp = idx + 1; raddr += 4
                    elif sub == 1: sectC = self.bases[idx]
                    elif sub == 2: sectD = self.bases[idx]
                    elif sub == 3: add(raddr, self.bases[idx], 'S'); raddr += 4
                    else: raise ValueError('sm sub %d' % sub)
                    return 1
                if h >> 12 == 8:
                    raddr += (h & 0xFFF) + 1; return 1
                if h >> 12 == 9:
                    raise ValueError('repeat must be handled by caller')
                if h >> 10 == 0x28:
                    raddr = ((h & 0x3FF) << 16) | hw[ip+1]; return 2
                if h >> 10 == 0x29:
                    idx = ((h & 0x3FF) << 16) | hw[ip+1]
                    add(raddr, imports[idx], 'I'); imp = idx + 1; raddr += 4; return 2
                if h >> 10 == 0x2D:
                    sub = (h >> 6) & 0xF; idx = ((h & 0x3F) << 16) | hw[ip+1]
                    if sub == 0: add(raddr, self.bases[idx], 'S'); raddr += 4
                    elif sub == 1: sectC = self.bases[idx]
                    elif sub == 2: sectD = self.bases[idx]
                    return 2
                raise ValueError('reloc op %04x' % h)
            starts = []
            while i < len(hw):
                h = hw[i]
                if h >> 12 == 9:
                    blk = ((h >> 8) & 0xF) + 1; rep = (h & 0xFF) + 1
                    body = starts[-blk:]
                    for _ in range(rep):
                        for s in body: run(s)
                    starts.append(i); i += 1; continue
                if h >> 10 == 0x2C:
                    blk = ((h >> 6) & 0xF) + 1; rep = ((h & 0x3F) << 16) | hw[i+1]
                    body = starts[-blk:]
                    for _ in range(rep):
                        for s in body: run(s)
                    starts.append(i); i += 2; continue
                starts.append(i)
                i += run(i)

    def find_tracebacks(self):
        """Scan the code section for traceback tables and record function extents."""
        c = self.code
        self.funcs = {}  # start -> (name, size)
        n = len(c)
        o = 0
        while o < n - 12:
            w = struct.unpack('>I', c[o:o+4])[0]
            if w == 0 and c[o+4] == 0 and c[o+5] in (0, 9, 1, 0x0c):  # version 0, lang C/C++
                f = c[o+6:o+12]
                has_tboff = f[0] & 0x20
                name_present = f[1] & 0x40
                fixedparms = f[4]; floatparms = f[5] >> 1
                p = o + 12
                if fixedparms or floatparms: p += 4
                tboff = None
                if has_tboff:
                    tboff = struct.unpack('>I', c[p:p+4])[0]; p += 4
                if f[1] & 0x80:  # int_hndl
                    p += 4
                if f[0] & 0x08:  # has_ctl
                    nctl = struct.unpack('>I', c[p:p+4])[0]; p += 4 + 4*nctl
                if name_present and tboff is not None and 0 < tboff <= o and p + 2 <= n:
                    nl = struct.unpack('>H', c[p:p+2])[0]
                    if 0 < nl < 256:
                        name = c[p+2:p+2+nl].decode('mac_roman', 'replace')
                        start = o - tboff
                        self.funcs[start] = (name, tboff)
                        o = (p + 2 + nl + 3) & ~3
                        continue
            o += 4
        self.fstarts = sorted(self.funcs)
        self.names = {s: demangle(self.funcs[s][0]) for s in self.fstarts}
        # glue stubs: lwz r12,X(r2); stw r2,20(r1); lwz r0,0(r12); lwz r2,4(r12); mtctr r0; bctr
        self.glue = {}
        for o in range(0, n - 24, 4):
            a, b, cc, d, e, f = struct.unpack('>6I', c[o:o+24])
            if (a & 0xFFFF0000) == 0x81820000 and b == 0x90410014 and cc == 0x800C0000 \
               and d == 0x804C0004 and e == 0x7C0903A6 and f == 0x4E800420:
                disp = a & 0xFFFF
                if disp & 0x8000: disp -= 0x10000
                tgt = self.dword(self.toc + disp)
                nm = self.symname(tgt, deref=True)
                self.glue[o] = nm
                self.names.setdefault(o, 'glue:' + nm)

    def import_name(self, v):
        i = v - IMPORT_BASE
        if 0 <= i < len(self.pef.imports): return self.pef.imports[i][1]
        return None

    def symname(self, v, deref=False):
        if v is None: return '?'
        if IMPORT_BASE <= v < IMPORT_BASE + 0x10000:
            return self.import_name(v)
        if v in self.names: return self.names[v]
        if DATA_BASE <= v < DATA_BASE + len(self.data):
            # TVector?
            cp = self.dword(v)
            if deref and cp is not None and cp in self.names: return 'TV:' + self.names[cp]
            return 'D_%06x' % (v - DATA_BASE)
        return '0x%x' % v

    def func_at(self, addr):
        import bisect
        i = bisect.bisect_right(self.fstarts, addr) - 1
        if i >= 0:
            s = self.fstarts[i]
            if addr < s + self.funcs[s][1] + 64: return s
        return None

    def toc_ref(self, disp):
        a = self.toc + disp
        v = self.dword(a)
        o = a - DATA_BASE
        kind = self.reloc_targets.get(o)
        if kind == 'I': return self.import_name(v) or '?'
        if kind == 'D' or kind == 'S':
            return '&' + self.symname(v, deref=True)
        if kind == 'C': return '&' + self.symname(v)
        return 'toc[%d]=%s' % (disp, 'None' if v is None else '%#x' % v)

    def disasm(self, start, end, out):
        md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
        md.skipdata = True
        buf = bytes(self.code[start:end])
        for ins in md.disasm(buf, start):
            txt = f"{ins.mnemonic:8} {ins.op_str}"
            comment = ''
            if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('0x'):
                t = int(ins.op_str, 16)
                nm = self.names.get(t)
                if nm: comment = nm
            m = re.search(r'(-?0x[0-9a-f]+|-?\d+)\(r2\)', ins.op_str)
            if m and ins.mnemonic in ('lwz', 'lfd', 'lfs', 'lbz', 'lhz', 'lha', 'stw', 'addi'):
                disp = int(m.group(1), 0)
                comment = self.toc_ref(disp)
            if ins.mnemonic == 'addi' and ins.op_str.split(',')[1].strip() == 'r2':
                comment = '&toc+%s' % ins.op_str.split(',')[2].strip()
            out.write(f"  {ins.address:06x}: {txt:40} {('; ' + comment) if comment else ''}\n")

    def disfunc(self, start, out):
        name, size = self.funcs.get(start, (self.names.get(start, '?'), 64))
        out.write(f"\n{self.names.get(start, name)}:  ; {start:#x} size {size:#x}\n")
        self.disasm(start, start + size, out)

def load(path):
    cache = path + '.ppcdis.pickle'
    return Image(path)

if __name__ == '__main__':
    img = load(sys.argv[1])
    cmd = sys.argv[2]
    out = sys.stdout
    if cmd == 'funcs':
        for s in img.fstarts:
            print(f"{s:06x} {img.funcs[s][1]:6x} {img.names[s]}")
        print(len(img.fstarts), 'functions;', len(img.glue), 'glue stubs; toc=%#x main=%#x' % (img.toc, img.main_code), file=sys.stderr)
    elif cmd == 'dis':
        for a in sys.argv[3:]:
            if re.match(r'^(0x)?[0-9a-fA-F]+$', a) and not a.isalpha():
                img.disfunc(img.func_at(int(a, 16)) or int(a, 16), out)
            else:
                for s in img.fstarts:
                    if img.names[s] == a or img.funcs[s][0].lstrip('.') == a or img.names[s].endswith('::' + a) and '::' in a is False:
                        img.disfunc(s, out)
    elif cmd == 'all':
        prev = 0
        for s in img.fstarts:
            if s > prev:
                out.write(f"\n; --- unnamed region {prev:#x}-{s:#x}\n")
                img.disasm(prev, s, out)
            img.disfunc(s, out)
            prev = s + img.funcs[s][1]
        # runtime library code and glue after the last traceback table
        if prev < len(img.code):
            out.write(f"\n; --- unnamed region {prev:#x}-{len(img.code):#x}\n")
            img.disasm(prev, len(img.code), out)
    elif cmd == 'xref':
        # direct calls (bl) to a function or an import's glue stub
        byname = {}
        for a, nm in img.names.items():
            byname.setdefault(nm[5:] if nm.startswith('glue:') else nm, []).append(a)
        c = img.code
        for want in sys.argv[3:]:
            targets = set(byname.get(want, []))
            if re.match(r'^(0x)?[0-9a-fA-F]+$', want) and not targets:
                targets = {int(want, 16)}
            n = 0
            for o in range(0, len(c) - 3, 4):
                w = struct.unpack('>I', c[o:o+4])[0]
                if w & 0xFC000003 != 0x48000001: continue
                d = w & 0x03FFFFFC
                if d & 0x02000000: d -= 0x04000000
                if o + d in targets:
                    f = img.func_at(o)
                    fn = img.names.get(f, '?') if f is not None and o < f + img.funcs[f][1] else '(no traceback)'
                    print(f"{o:06x} {fn}")
                    n += 1
            print(f"; {want}: {n} call sites", file=sys.stderr)
