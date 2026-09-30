#!/usr/bin/env python3
"""Dump a Delver archive ("Cythera Data" data fork, saved games).

Usage:
  delv_archive.py [--data FILE] info
  delv_archive.py [--data FILE] list [SEL...]
  delv_archive.py [--data FILE] dump ID           text view on stdout
  delv_archive.py [--data FILE] export OUTDIR [SEL...] [--map-tile N]

SEL is a resource id (8E01), a page (8E or 8Exx: all ids 8E00-8EFF) or a
page range (02-3F); always hex, "0x" optional.  Without SEL, everything.  FILE defaults to
"gamedata/Cythera Data".  Standard library only.

export writes, for every resource, the decrypted bytes as ID.bin plus a
viewable conversion where one exists:
  images (landscapes, portraits, skill icons, tile sheets, general graphics)
                  -> .png (palette: 'clut' 256 of FILE.rsrc; index 0 is
                     transparent in tile sheets and general graphics)
  maps            -> .txt (header) and .png (base layer, N px per tile,
                     default 8; --map-tile 32 = full size)
  prop lists      -> .txt (one line per prop, with names)
  sounds (asnd)   -> .wav
  music           -> .tune (QuickTime Music tune, as stored)
  scripts/data    -> .txt (structure of the VM objects, function bodies
                     in hex; delvmod's ddasm does real disassembly)
  F0xx name lists -> .txt; other F0xx and AI records -> .txt hex dump
plus index.tsv (the list output).  See docs/ANALYSIS.md section 3.
"""
import math, os, struct, sys, zlib
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------- knowledge
# Page = high byte of the resource id = index in the master TOC.  (delvmod
# numbers "subindices" from 0, i.e. page - 1.)  Names mostly from delvmod.
PAGES = {
    0x01: 'Global symbols', 0x02: 'Static data (strings)', 0x03: 'AI support',
    0x04: 'Compiled AI scripts', 0x05: 'Script data',
    0x08: 'Shared dialogue scripts', 0x09: 'Scripts', 0x0A: 'Potion scripts',
    0x0B: 'Scripts', 0x0C: 'Scripts', 0x0D: 'Scripts',
    0x0E: 'Game mechanics scripts', 0x0F: 'Scripts', 0x10: 'Object scripts',
    0x11: 'Object scripts', 0x12: 'Object scripts', 0x13: 'Object scripts',
    0x14: 'Zone scripts', 0x15: 'Composite zone scripts',
    0x18: 'Character scripts', 0x19: 'Monster scripts',
    0x1A: 'Skill and action scripts', 0x1B: 'Area scripts',
    0x1C: 'Area scripts', 0x1E: 'Scripts',
    0x30: 'Default method implementations',
    0x80: 'Maps', 0x81: 'Prop lists', 0x82: 'Explored-area bitmaps (saves)',
    0x84: 'Landscape graphics', 0x88: 'Character portraits',
    0x8A: 'Skill icons', 0x8E: 'Tile sheets', 0x8F: 'General graphics',
    0x90: 'Music', 0x91: 'Sounds', 0xE0: 'Journal entries (saves)',
    0xF0: 'General data',
}
# Encryption is not recorded in the archive: the engine decrypts when the
# caller asks for it (TCachedSegFiles::GetEncryptedSegment, used by the VM).
ENCRYPTED_PAGES = {0x02, 0x03, 0x05, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
                   0x0F, 0x10, 0x11, 0x14, 0x15, 0x18, 0x19, 0x1A, 0x1B, 0x1C,
                   0x1D, 0x1E, 0x1F, 0x30}
CLEAR_IDS = {0x0210}
CLEAR_PAGES = {0x01, 0x04}               # and everything >= 0x80
DCG = {0x84: (288, 32), 0x88: (64, 64), 0x8E: (32, 512), 0x8F: None}
SIZED_IDS = {0x8EFF}                     # has the 8Fxx header despite its page
ZONES = ["Nowhere", "World", "Odemia", "LandKing Hall", "Abandoned Farmhouse",
    "Farmhouse Cellar", "Catamarca", "Under Catamarca", "Cademia",
    "UrSylph's Prison", "Headwater Ruins", "Maayti Ruins", "Pnyx", "Kosha",
    "Pnyx Upstairs", "Iron Mine", "Land's End Volcano", "Charax's House",
    "North Shore Vineyard", "Hall of Truth", "Southland Vineyard",
    "Under Cademia", "Goat Farm", "Kosha Grotto", "Mining camp",
    "Tyrant's Tomb", "Scylla Temple", "Crab Cove", "Machaon's Workshop",
    "Abydos", "Under Abydos", "Inner Brotherhood", "Bandit Camp",
    "Brotherhood Dungeon", "Harpy Cave", "Eioneus Cave", "Tavara Fort",
    "Tavara No Fort", "Sitia Bridge", "Tree of Life", "Omen Test",
    "Harpy Abyss"]
NAMES = {
    0x0101: 'Global symbol list', 0x0201: 'Character names',
    0x0203: 'Character class names', 0x0204: 'Character class descriptions',
    0x0205: 'Character class stats', 0x0206: 'Character class skills',
    0x0218: 'Sign text', 0x0219: 'Scroll text', 0x021A: 'Quest text',
    0x021B: 'Book item text', 0x021D: 'Bookshelf text',
    0x021F: 'Ring inscriptions', 0x0220: 'Gravestone inscriptions',
    0x0241: 'Death text', 0x0242: 'Defeat text', 0x0243: 'Victory text',
    0x0A00: 'Sustenance potion', 0x0A01: 'Healing potion',
    0x0A02: "Mage's Friend potion", 0x0A03: 'Free Motion potion',
    0x0A04: 'Antidote potion', 0x0A05: 'Clear Mind potion',
    0x0A06: "Smith's Friend potion", 0x0A07: 'Far Sight potion',
    0xF000: 'Prop -> first tile', 0xF002: 'Tile attributes',
    0xF004: 'Tile names', 0xF008: 'Monster statistics', 0xF009: 'Characters',
    0xF00B: 'Schedules', 0xF00C: 'Zoneports', 0xF010: 'Tile faux props',
    0xF011: 'Prop-aspect X offsets', 0xF012: 'Prop-aspect Y offsets',
    0xF013: 'Composite tiles', 0xF014: 'Symbols', 0xF015: 'Persistence symbols',
}
for _n, _z in enumerate(ZONES):
    for _p in (0x80, 0x81, 0x14):
        NAMES.setdefault(_p << 8 | _n, _z)

# ----------------------------------------------------------------- archive
def crypt(data, rid, skip=0):
    """TSegFile::Encrypt/Decrypt (symmetric): XOR with the low byte of a
    16-bit LCG keyed by the resource id.  skip = offset into the segment."""
    key = (rid ^ (rid >> 8)) & 0xFFFF
    m, b = ((rid & 0x3F) << 2) + 1, (rid >> 6) & 0xFF
    for _ in range(skip):
        key = (key * m + b) & 0xFFFF
    out = bytearray(len(data))
    for i, c in enumerate(data):
        key = (key * m + b) & 0xFFFF
        out[i] = c ^ (key & 0xFF)
    return bytes(out)

def entropy(b):
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in Counter(b).values()) if n else 0

class Archive:
    def __init__(self, path):
        self.path = path
        self.buf = open(path, 'rb').read()
        d = self.buf
        if len(d) < 0x880 or struct.unpack('>I', d[0x80:0x84])[0] != 0x80:
            sys.exit(f"{path}: not a Delver archive (no TOC at 0x80)")
        self.title = d[1:1 + d[0]].decode('mac_roman')
        self.player = d[0x21:0x21 + d[0x20]].decode('mac_roman')
        self.pages = {}                      # page -> (offset, length)
        self.entries = {}                    # rid -> (offset, length)
        for p in range(256):
            off, ln = struct.unpack('>II', d[0x80 + 8 * p:0x88 + 8 * p])
            if not ln:
                continue
            self.pages[p] = (off, ln)
            if p == 0:
                continue                     # page 0 is the master TOC itself
            for n in range(min(ln // 8, 256)):
                o, l = struct.unpack('>II', d[off + 8 * n:off + 8 * n + 8])
                if o:
                    self.entries[p << 8 | n] = (o, l)
        self._enc = {}

    def ids(self):
        return sorted(self.entries)

    def raw(self, rid):
        o, l = self.entries[rid]
        return self.buf[o:o + l]

    def encryption(self, rid):
        """(encrypted, how): how is 'known' or 'guess' (entropy)."""
        if rid not in self._enc:
            p = rid >> 8
            if rid in CLEAR_IDS or p in CLEAR_PAGES or p >= 0x80:
                self._enc[rid] = (False, 'known')
            elif p in ENCRYPTED_PAGES:
                self._enc[rid] = (True, 'known')
            else:
                r = self.raw(rid)
                self._enc[rid] = (entropy(crypt(r, rid)) < entropy(r), 'guess')
        return self._enc[rid]

    def data(self, rid):
        r = self.raw(rid)
        return crypt(r, rid) if self.encryption(rid)[0] else r

def parse_id(s):
    return int(s, 16)                        # always hex, 0x optional

def select(arc, sels):
    if not sels:
        return arc.ids()
    out = []
    for s in sels:
        if '-' in s:
            a, b = (parse_id(x) for x in s.split('-'))
            want = lambda r: a <= r >> 8 <= b
        else:
            v = parse_id(s.lower().replace('xx', ''))
            want = (lambda r: r >> 8 == v) if v < 0x100 else (lambda r: r == v)
        out += [r for r in arc.ids() if want(r) and r not in out]
    if not out:
        sys.exit(f"nothing matches {' '.join(sels)}")
    return out

# ---------------------------------------------------------------- graphics
def dcg_decompress(data, size):
    """Delver Compressed Graphics -> bytearray of `size` 8-bit pixels."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n and len(out) < size:
        op = data[i]
        if op < 0x80:                       # short copy, 2 bytes
            b1 = data[i + 1]
            dist = ((b1 >> 5) << 7 | (op & 0x7F)) + 1
            lits, length = (b1 >> 3) & 3, (b1 & 7) + 3
            i += 2
        elif op < 0xC0:                     # long copy, 3 bytes
            b1, b2 = data[i + 1], data[i + 2]
            dist = ((b2 >> 2) << 9 | (b1 >> 5) << 6 | (op & 0x3F)) + 1
            lits, length = b2 & 3, (b1 & 0x1F) + 3
            i += 3
        elif op < 0xE0:                     # literals: Cx 4*(x+1), Dx x bytes
            cnt = ((op & 0xF) + 1) * 4 if op < 0xD0 else op & 0xF
            out += data[i + 1:i + 1 + cnt]
            i += 1 + cnt
            continue
        elif op < 0xF0:                     # short run
            out += bytes([data[i + 1]]) * ((op & 0xF) + 3)
            i += 2
            continue
        elif op == 0xF0:                    # long run
            out += bytes([data[i + 2]]) * (data[i + 1] + 3)
            i += 3
            continue
        elif op == 0xFF:
            break
        else:
            raise ValueError(f"DCG: unknown opcode {op:02X} at {i:#x}")
        out += data[i:i + lits]
        i += lits
        start = len(out) - dist
        if start < 0:
            raise ValueError(f"DCG: copy before start at {i:#x}")
        for k in range(length):             # may overlap (pattern repeat)
            out.append(out[start + k])
    out += bytes(max(0, size - len(out)))
    return out[:size]

def decode_image(rid, data):
    """-> (width, height, pixels) or None."""
    page = rid >> 8
    if rid in SIZED_IDS:
        page = 0x8F
    if page == 0x8A:
        return (32, len(data) // 32, data)
    if page not in DCG or not data:
        return None
    if DCG[page]:
        w, h = DCG[page]
        return (w, h, dcg_decompress(data, w * h))
    # 8Fxx: 4-byte header (width, height); rows are padded to 4 pixels.
    w, h = struct.unpack('>HH', data[:4])
    stride = (w + 3) & ~3
    pix = dcg_decompress(data[4:], stride * h)
    if stride != w:
        pix = b''.join(pix[y * stride:y * stride + w] for y in range(h))
    return (w, h, pix)

def load_palette(path):
    """Colours of 'clut' 256 in <scenario>.rsrc; greys if unavailable."""
    try:
        import rsrc
        body = rsrc.parse(open(path + '.rsrc', 'rb').read())[('clut', 256)][2]
        cnt = struct.unpack('>H', body[6:8])[0] + 1
        pal = [(0, 0, 0)] * 256
        for k in range(cnt):
            v, r, g, b = struct.unpack('>HHHH', body[8 + 8 * k:16 + 8 * k])
            pal[v & 0xFF] = (r >> 8, g >> 8, b >> 8)
        return pal
    except (OSError, KeyError) as e:
        print(f"warning: no clut 256 in {path}.rsrc ({e}); using greys",
              file=sys.stderr)
        return [(255 - i, 255 - i, 255 - i) for i in range(256)]

def write_png(path, w, h, pixels, palette, transparent0=False):
    """8-bit indexed PNG."""
    def chunk(t, body):
        c = t + body
        return struct.pack('>I', len(body)) + c + struct.pack('>I', zlib.crc32(c))
    rows = b''.join(b'\0' + bytes(pixels[y * w:(y + 1) * w]) for y in range(h))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 3, 0, 0, 0))
    png += chunk(b'PLTE', b''.join(bytes(c) for c in palette))
    if transparent0:
        png += chunk(b'tRNS', b'\0')
    png += chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)

class Tiles:
    """32x32 tile images by tile id (0x000-0xFFF from sheets 8Exx,
    0x1000-0x1FFF composed of 8x8 pieces as listed in F013)."""
    def __init__(self, arc):
        self.arc, self.sheets, self.cache = arc, {}, {}
        self.comp = arc.data(0xF013) if 0xF013 in arc.entries else b''

    def sheet(self, n):
        if n not in self.sheets:
            rid = 0x8E00 | n
            self.sheets[n] = (dcg_decompress(self.arc.data(rid), 32 * 512)
                              if rid in self.arc.entries and rid not in SIZED_IDS
                              else None)
        return self.sheets[n]

    def get(self, t):
        if t in self.cache:
            return self.cache[t]
        img = bytearray(32 * 32)
        if t < 0x1000:
            s = self.sheet(t >> 4)
            if s:
                o = (t & 15) * 32 * 32
                img[:] = s[o:o + 1024]
        elif (t - 0x1000) * 32 + 32 <= len(self.comp):
            words = struct.unpack('>16H', self.comp[(t - 0x1000) * 32:][:32])
            for k, w in enumerate(words):
                s = self.sheet((w >> 4) & 0xFF)
                if not s:
                    continue
                # piece `seg` of tile (w & 15): 8x8, numbered down then across
                seg, sx = w >> 12, (w >> 12) // 4 * 8
                sy = (w & 15) * 32 + seg % 4 * 8
                dx, dy = k % 4 * 8, k // 4 * 8   # placed across then down
                for r in range(8):
                    img[(dy + r) * 32 + dx:(dy + r) * 32 + dx + 8] = \
                        s[(sy + r) * 32 + sx:(sy + r) * 32 + sx + 8]
        self.cache[t] = img
        return img

# -------------------------------------------------------------- data files
def read_cstr(d, i):
    j = d.index(b'\0', i) if b'\0' in d[i:] else len(d)
    return d[i:j].decode('mac_roman'), j + 1

def name_list(d):
    """u16 + C string records (F004 tile names: u16 = last tile id of the
    name; F014/F015: u16 = symbol value).  None if it doesn't parse."""
    out, i = [], 0
    try:
        while i + 2 < len(d):
            v = struct.unpack('>H', d[i:i + 2])[0]
            s, i = read_cstr(d, i + 2)
            if not s and not v:
                break
            if any(c < ' ' for c in s):
                return None
            out.append((v, s))
    except ValueError:
        return None
    return out

def plural(name, pl=False):
    """"pot\\s" -> pot / pots; "kni\\fe/ves" -> knife / knives."""
    if '\\' not in name:
        return name
    stem, end = name.split('\\', 1)
    if '/' in end:
        return stem + end.split('/')[1 if pl else 0]
    return stem + end if pl else stem

class Names:
    """Tile and prop names from F004/F000."""
    def __init__(self, arc):
        self.tiles = name_list(arc.data(0xF004)) if 0xF004 in arc.entries else []
        f0 = arc.data(0xF000) if 0xF000 in arc.entries else b''
        self.prop_tile = struct.unpack(f'>{len(f0) // 2}H', f0)

    def tile(self, t):
        for last, name in self.tiles:
            if t <= last:
                return plural(name)
        return '?'

    def prop(self, ptype, aspect=0):
        if ptype >= len(self.prop_tile):
            return '?'
        return self.tile(self.prop_tile[ptype] + aspect)

def map_header(d):
    w, h, _, roof1, roof2, hprop, vprop, n, e, s, west = struct.unpack('>5H2B4H', d[:20])
    return dict(width=w, height=h, roof_layers=(roof1, roof2),
                edge_propagation=(hprop, vprop), exit_zoneports=(n, e, s, west),
                tiles_at=0x20 + 0x40 * (roof1 + roof2))

def map_tiles(d):
    hd = map_header(d)
    o, cnt = hd['tiles_at'], hd['width'] * hd['height']
    return struct.unpack(f'>{cnt}H', d[o:o + 2 * cnt])

def render_map(arc, tiles, d, path, palette, tpx):
    hd = map_header(d)
    w, h, grid = hd['width'], hd['height'], map_tiles(d)
    step = 32 // tpx
    small = {}
    def tile_rows(t):
        if t not in small:
            img = tiles.get(t & 0x1FFF)
            small[t] = [bytes(img[(y * step) * 32:(y * step) * 32 + 32][::step])
                        for y in range(tpx)]
        return small[t]
    pix = bytearray()
    for ty in range(h):
        rows = [tile_rows(grid[ty * w + tx]) for tx in range(w)]
        for y in range(tpx):
            pix += b''.join(r[y] for r in rows)
    write_png(path, w * tpx, h * tpx, pix, palette)

def props_text(arc, names, d):
    """16-byte records: flags, 12+12-bit x/y (or container), aspect<<10 |
    type, d1:d2 (persistence), propref, storeref, u16."""
    lines = ['#     flags  x    y     type asp   d1:d2 propref storeref  u     name']
    for k in range(len(d) // 16):
        r = d[16 * k:16 * k + 16]
        flags = r[0]
        x, y = r[1] << 4 | r[2] >> 4, (r[2] & 15) << 8 | r[3]
        at, d3, pref, sref, u = struct.unpack('>HHHIH', r[4:])
        ptype, asp = at & 0x3FF, at >> 10
        where = f'{x:4} {y:4}'
        cont = (r[2] << 8 | r[3]) - 0x100
        if flags == 0xFF:
            where = 'deleted  '
        elif flags & 0x10:
            where = f'held #{cont + 0x100:<3}'
        elif flags & 0x08:
            where = f'in #{cont:<5}'
        rot = 'r' if asp & 0x20 else ' '
        lines.append(f'{k:<5} {flags:02x}   {where:9}  {ptype:4} {asp & 0x1F:2}{rot}   '
                     f'{d3 >> 8:02x}:{d3 & 0xFF:02x}  {pref:04x}    {sref:08x}  {u:04x}  '
                     f'{names.prop(ptype, asp & 0x1F) if flags != 0xFF else ""}')
    return '\n'.join(lines) + '\n'

def write_wav(path, d):
    """'asnd': magic, u32 N (samples = 512 + 1024*N), Fixed rate, then
    signed 8-bit samples stored as big-endian int16."""
    n, rate = struct.unpack('>II', d[4:12])
    rate = round(rate / 65536)
    cnt = (len(d) - 12) // 2
    s = struct.unpack(f'>{cnt}h', d[12:12 + 2 * cnt])
    pcm = bytes(max(0, min(255, v + 128)) for v in s)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' +
                struct.pack('<IHHIIHH', 16, 1, 1, rate, rate, 1, 8) +
                b'data' + struct.pack('<I', len(pcm)) + pcm)
    return rate, cnt

def hexdump(d, base=0, indent=''):
    out = []
    for i in range(0, len(d), 16):
        c = d[i:i + 16]
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in c)
        out.append(f'{indent}{base + i:06x}  {c.hex(" "):<47}  {asc}')
    return '\n'.join(out)

# --------------------------------------------------------- VM object dump
def word(v, rid):
    """Tagged 32-bit VM value."""
    if v <= 0x07FFFFFF:
        return str(v)
    if v <= 0x0FFFFFFF:
        return str(v - 0x10000000)
    if 0x30000000 <= v < 0x40000000:
        return f'res:{v & 0xFFFF:04X}[{v >> 16 & 0xFFF}]'
    if 0x40000000 <= v < 0x50000000:
        return f'obj:{v >> 16 & 0xFF:02X}/{v & 0xFFFF:04X}'
    special = {0x50000000: 'false', 0x50000001: 'true', 0x5000FFFF: 'none',
               0x5000FFFE: 'empty'}
    if v in special:
        return special[v]
    if v & 0x80000000:
        r = v >> 16 & 0x7FFF
        return f'@{v & 0xFFFF:04x}' if r == rid else f'ref:{r:04X}:{v & 0xFFFF:04x}'
    return f'<{v:08X}>'

class VMDump:
    """Structure of a script/data resource: a class (u16 at 0 = offset of
    its field table) or a bare object: 81 function, 9x array, Ax table,
    otherwise a C string.  Refs 0x8RRRoooo with RRR = this resource are
    followed; function bodies are shown in hex up to the next object.
    0201 (character names) uses 0x9165oooo for its own strings; such refs
    to a missing/too short resource that fit here are taken as local (~).
    `sizes` maps resource ids to lengths."""
    def __init__(self, d, rid, sizes=None):
        self.d, self.rid, self.objs, self.lines = d, rid, {}, []
        self.sizes = sizes or {}

    def local(self, v):
        """Offset of a ref into this resource, or None."""
        if not v & 0x80000000:
            return None
        r, o = v >> 16 & 0x7FFF, v & 0xFFFF
        if r == self.rid or (o >= self.sizes.get(r, 0) and o < len(self.d)):
            return o
        return None

    def kind(self, off):
        v = self.d[off]
        if v == 0x81: return 'function'
        if v & 0xF0 == 0x90: return 'array'
        if v & 0xF0 == 0xA0: return 'table'
        return 'string'

    def scan(self, off, kind=None):
        if off in self.objs or off >= len(self.d):
            return
        kind = kind or self.kind(off)
        self.objs[off] = kind
        d = self.d
        if kind in ('array', 'table', 'class'):
            cnt = struct.unpack('>H', d[off:off + 2])[0] & 0xFFF
            esz = 4 if kind == 'array' else 6
            for k in range(cnt):
                e = off + 2 + esz * k
                if e + 4 > len(d):
                    break
                v = struct.unpack('>I', d[e:e + 4])[0]
                if self.local(v) is not None:
                    self.scan(self.local(v))

    def run(self):
        d = self.d
        if len(d) < 2:
            return hexdump(d)
        if d[0] < 0x80 and (d[0] or d[1]) and struct.unpack('>H', d[:2])[0] < len(d):
            top = struct.unpack('>H', d[:2])[0]
            self.lines.append(f'class, field table at {top:#06x}')
            self.scan(top, 'class')
        else:
            self.scan(0)
        ends = sorted(self.objs) + [len(d)]
        pos = 2 if 'class' in self.objs.values() else 0
        for i, off in enumerate(ends[:-1]):
            if off > pos:
                self.lines.append(f'\n{pos:04x}: unreferenced, {off - pos} bytes')
                self.lines.append(hexdump(d[pos:off], pos, '  '))
            pos = max(pos, self.show(off, self.objs[off], ends[i + 1]))
        if pos < len(d):
            self.lines.append(f'\n{pos:04x}: unreferenced, {len(d) - pos} bytes')
            self.lines.append(hexdump(d[pos:], pos, '  '))
        return '\n'.join(self.lines) + '\n'

    def show(self, off, kind, nxt):
        d, L = self.d, self.lines
        if kind == 'string':
            s, end = read_cstr(d, off)
            L.append(f'{off:04x}: string {s!r}')
            return end
        if kind == 'function':
            argc, nloc = d[off + 1], d[off + 2]
            L.append(f'\n{off:04x}: function, {argc} args, {nloc} locals, '
                     f'{nxt - off - 3} bytes of code')
            L.append(hexdump(d[off + 3:nxt], off + 3, '  '))
            return nxt
        cnt = struct.unpack('>H', d[off:off + 2])[0] & 0xFFF
        esz = 4 if kind == 'array' else 6
        L.append(f'\n{off:04x}: {kind}, {cnt} entries')
        for k in range(cnt):
            e = off + 2 + esz * k
            if e + esz > len(d):
                L.append('  (truncated)')
                break
            v = struct.unpack('>I', d[e:e + 4])[0]
            txt = word(v, self.rid)
            o = self.local(v)
            if o is not None:
                if v >> 16 & 0x7FFF != self.rid:
                    txt = f'~@{o:04x}'
                if self.objs.get(o) == 'string':
                    txt += ' ' + repr(read_cstr(d, o)[0])
                else:
                    txt += f' ({self.objs.get(o, "?")})'
            key = f'[{k}]' if kind == 'array' else \
                f'{struct.unpack(">H", d[e + 4:e + 6])[0]:#06x}'
            L.append(f'  {key:>8} = {txt}')
        return off + 2 + esz * cnt

def vm_dump(arc, d, rid):
    try:
        return VMDump(d, rid, {r: l for r, (_, l) in arc.entries.items()}).run()
    except (IndexError, struct.error, ValueError) as e:
        return f'(structure dump failed: {e})\n' + hexdump(d)

# ------------------------------------------------------------ description
def describe(arc, rid, d):
    """Short content summary for `list`."""
    p = rid >> 8
    try:
        if p == 0x80:
            h = map_header(d)
            return f"{h['width']}x{h['height']}"
        if p == 0x81:
            return f'{len(d) // 16} props'
        if p == 0x8F or rid in SIZED_IDS:
            return '%dx%d' % struct.unpack('>HH', d[:4])
        if p in DCG:
            return '%dx%d' % DCG[p]
        if p == 0x8A:
            return f'32x{len(d) // 32}'
        if p == 0x91 and d[:4] == b'asnd':
            n, rate = struct.unpack('>II', d[4:12])
            return f'{(len(d) - 12) // 2} samples @ {rate / 65536:.0f} Hz'
        if p == 0x90 and d[4:8] == b'musi':
            return 'QT music tune'
        if p == 0x04 and d:
            return repr(d[1:1 + d[0]].decode('mac_roman'))
        if p < 0x80 and len(d) >= 2:
            if d[0] == 0x81: return 'function'
            if d[0] & 0xF0 == 0x90: return f'array[{struct.unpack(">H", d[:2])[0] & 0xFFF}]'
            if d[0] & 0xF0 == 0xA0: return f'table[{struct.unpack(">H", d[:2])[0] & 0xFFF}]'
            if d[0] < 0x80: return 'class'
    except (struct.error, IndexError):
        pass
    return ''

def list_rows(arc, ids):
    rows = []
    for rid in ids:
        d = arc.data(rid)
        enc, how = arc.encryption(rid)
        flags = ('E' if enc else '-') + ('?' if how == 'guess' else ' ') + \
                ('C' if rid >> 8 in DCG else '-')
        rows.append((rid, arc.entries[rid][0], len(d), flags,
                     PAGES.get(rid >> 8, '?'), NAMES.get(rid, ''), describe(arc, rid, d)))
    return rows

def fmt_row(r):
    rid, off, ln, flags, kind, name, desc = r
    return f'{rid:04X}  {off:08x} {ln:8}  {flags}  {kind:28.28} {name:24.24} {desc}'

# ------------------------------------------------------------------ export
def export(arc, ids, out, tpx):
    os.makedirs(out, exist_ok=True)
    pal = load_palette(arc.path)
    names, tiles = Names(arc), Tiles(arc)
    rows = list_rows(arc, ids)
    with open(os.path.join(out, 'index.tsv'), 'w', encoding='utf-8') as f:
        f.write('id\toffset\tsize\tflags\tkind\tname\tinfo\n')
        for r in rows:
            f.write(f'{r[0]:04X}\t{r[1]:#x}\t' + '\t'.join(map(str, r[2:])) + '\n')
    counts = Counter()
    for rid in ids:
        p, d = rid >> 8, arc.data(rid)
        base = os.path.join(out, f'{rid:04X}')
        with open(base + '.bin', 'wb') as f:
            f.write(d)
        try:
            txt = None
            img = decode_image(rid, d)
            if img:
                write_png(base + '.png', *img, pal, transparent0=p in (0x8E, 0x8F))
                counts['png'] += 1
            elif p == 0x80:
                h = map_header(d)
                txt = ''.join(f'{k}: {v}\n' for k, v in h.items())
                render_map(arc, tiles, d, base + '.png', pal, tpx)
                counts['png'] += 1
            elif p == 0x81:
                txt = props_text(arc, names, d)
            elif p == 0x91 and d[:4] == b'asnd':
                write_wav(base + '.wav', d)
                counts['wav'] += 1
            elif p == 0x90:
                with open(base + '.tune', 'wb') as f:
                    f.write(d)
                counts['tune'] += 1
            elif p == 0xF0:
                nl = name_list(d) if rid in (0xF004, 0xF014, 0xF015) else None
                txt = ''.join(f'{v:#06x} {s}\n' for v, s in nl) if nl else hexdump(d) + '\n'
            elif p == 0x04:
                txt = f'name: {d[1:1 + d[0]].decode("mac_roman")!r}\n' + \
                      hexdump(d[1 + d[0]:], 1 + d[0]) + '\n'
            elif p < 0x80:
                txt = vm_dump(arc, d, rid)
            if txt is not None:
                with open(base + '.txt', 'w', encoding='utf-8') as f:
                    f.write(txt)
                counts['txt'] += 1
        except (ValueError, IndexError, struct.error) as e:
            print(f'{rid:04X}: conversion failed: {e}', file=sys.stderr)
            counts['failed'] += 1
    print(f"exported {len(ids)} resources to {out} (" +
          ', '.join(f'{v} {k}' for k, v in sorted(counts.items())) + ')')

def dump(arc, rid):
    p, d = rid >> 8, arc.data(rid)
    print(fmt_row(list_rows(arc, [rid])[0]))
    if p == 0x80:
        for k, v in map_header(d).items():
            print(f'{k}: {v}')
    elif p == 0x81:
        print(props_text(arc, Names(arc), d), end='')
    elif p < 0x80 and p != 0x04:
        print(vm_dump(arc, d, rid), end='')
    elif rid in (0xF004, 0xF014, 0xF015) and name_list(d):
        for v, s in name_list(d):
            print(f'{v:#06x} {s}')
    else:
        print(hexdump(d if p not in DCG else d[:256]))

def info(arc):
    d = arc.buf
    print(f"file:    {arc.path} ({len(d)} bytes)")
    print(f"title:   {arc.title!r}")
    print(f"player:  {arc.player!r}")
    print(f"0x40:    {d[0x40:0x50].hex(' ')}  (unknown; delvmod: 0x40=0x13, 0x42=2, 0x48=2)")
    print(f"TOC:     master page at {arc.pages[0][0]:#x}, {arc.pages[0][1]} bytes; "
          f"{len(arc.pages) - 1} sub-pages, {len(arc.entries)} resources, "
          f"{sum(l for _, l in arc.entries.values())} bytes of data")
    print(f"\npage  count     bytes  enc  kind")
    for p in sorted(arc.pages):
        if not p:
            continue
        ids = [r for r in arc.entries if r >> 8 == p]
        encs = {arc.encryption(r) for r in ids}
        e = ''.join(sorted({('E' if x else '-') + ('?' if h == 'guess' else '') for x, h in encs}))
        print(f"{p:02X}xx {len(ids):6} {sum(arc.entries[r][1] for r in ids):9}  {e:4} "
              f"{PAGES.get(p, '?')}{' (DCG compressed)' if p in DCG else ''}")

def main(argv):
    args = list(argv)
    def opt(name, default=None):
        if name in args:
            i = args.index(name); args.pop(i)
            return args.pop(i)
        return default
    data = opt('--data', os.path.join(ROOT, 'gamedata', 'Cythera Data'))
    tpx = int(opt('--map-tile', '8'))
    if tpx not in (1, 2, 4, 8, 16, 32):
        sys.exit('--map-tile must be 1, 2, 4, 8, 16 or 32')
    if not args:
        sys.exit(__doc__)
    arc = Archive(data)
    cmd, rest = args[0], args[1:]
    if cmd == 'info':
        info(arc)
    elif cmd == 'list':
        print('id    offset       size  flg  kind                         name                     info')
        for r in list_rows(arc, select(arc, rest)):
            print(fmt_row(r))
        print('flags: E encrypted (? = guessed by entropy), C DCG-compressed image', file=sys.stderr)
    elif cmd == 'dump' and len(rest) == 1:
        rid = parse_id(rest[0])
        if rid not in arc.entries:
            sys.exit(f'no resource {rid:04X}')
        dump(arc, rid)
    elif cmd == 'export' and rest:
        export(arc, select(arc, rest[1:]), rest[0], tpx)
    else:
        sys.exit(__doc__)

if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except OSError as e:                     # output piped into `head`
        if not isinstance(e, BrokenPipeError) and e.errno != 22:
            raise
