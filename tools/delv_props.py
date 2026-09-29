#!/usr/bin/env python3
"""List Cythera maps and their props, using delvmod (work/delvmod).

Usage: delv_props.py [--data FILE] maps
       delv_props.py [--data FILE] props MAP [--png OUT.png] [--ascii]

MAP is a map id (e.g. 0x28 or 40) or a case-insensitive name substring
("omen").  FILE defaults to "gamedata/Cythera Data" (the data fork; delvmod
does not need the .rsrc sidecar).  Map names come from delvmod's hint table.

Prop columns: index, x, y, type, aspect (+r if rotated), flags, the two
persistence bytes d1:d2 (e.g. a lever and the gate it opens share d1), name,
and "in #N" if the prop is inside prop N (then x,y are those of the outermost
container).  There is no z: each floor/level is a separate map.

--ascii prints the base map around the props, one character per tile.
--png needs Pillow, which lives in a local venv:
    python3 -m venv work/venv && work/venv/bin/pip install Pillow
    work/venv/bin/python tools/delv_props.py props omen --png /tmp/omen.png
"""
import contextlib, io, os, sys, types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'work', 'delvmod'))

# delvmod is Python 2 code; patch the few spots we need.  rdasm (the script
# assembler) needs parsley and is unused here, so stub it out.
sys.modules['delv.rdasm'] = types.ModuleType('delv.rdasm')
import delv.archive, delv.hints, delv.level, delv.library, delv.colormap

class _Dict(dict):
    def has_key(self, k): return k in self

def _empty(self): self.props = []; self.propsat = _Dict()
delv.level.PropList.empty = _empty
delv.archive.ResourceFile.read = lambda self, n=None: bytes(self.readb(n))

def load(path):
    scen = delv.archive.Scenario(path)
    lib = delv.library.Library(scen)
    lib.archives = [scen]
    lib.load_props()
    for pt in lib.props: pt.offset_by_aspect = list(pt.offset_by_aspect)
    lib.get_prop = lib._get_prop
    return scen, lib

def get_map(lib, n):
    with contextlib.redirect_stdout(io.StringIO()):  # "map loading" chatter
        return lib.get_object(0x8000 | n, rw=False)

def maps(scen):
    ids = set(scen.resource_ids())
    return [(n, delv.hints._RES_HINTS.get(0x8000 | n, '???'), (0x8100 | n) in ids)
            for n in range(256) if (0x8000 | n) in ids]

def find_map(scen, key):
    try:
        return int(key, 0)
    except ValueError:
        hits = [n for n, name, _ in maps(scen) if key.lower() in name.lower()]
        if len(hits) != 1:
            sys.exit(f"map {key!r}: {len(hits)} matches")
        return hits[0]

def container_of(p):
    """Index of the prop containing p, or None."""
    return p.container if p.flags != 0xFF and p.flags & 0x08 else None

def outer_loc(props, p):
    while container_of(p) is not None and container_of(p) < len(props):
        p = props[container_of(p)]
    return p.loc if not p.flags & 0x10 else None

def list_props(lib, props):
    rows = []
    for p in props:
        if p.flags == 0xFF:
            continue
        loc = outer_loc(props, p)
        c = container_of(p)
        where = f"in #{c}" if c is not None else (
                f"held by #{p.container + 0x100}" if p.flags & 0x10 else '')
        rows.append(((loc or (9999, 9999))[::-1], p.index, loc, p, where))
    for _, _, loc, p, where in sorted(rows, key=lambda r: r[:2]):
        x, y = loc if loc else ('-', '-')
        asp = f"{p.aspect}{'r' if p.rotated else ''}"
        print(f"#{p.index:<4} {x:>4} {y:>4}  type={p.proptype:<4} asp={asp:<4}"
              f" flags={p.flags:02x} d={p.get_d1():02x}:{p.get_d2():02x}"
              f"  {p.get_name(lib):<18} {where}")

def region(props, m, margin=2):
    locs = [p.loc for p in props if p.show_in_map()]
    xs, ys = [l[0] for l in locs], [l[1] for l in locs]
    return (max(min(xs) - margin, 0), max(min(ys) - margin, 0),
            min(max(xs) + margin + 1, m.width), min(max(ys) + margin + 1, m.height))

def ascii_map(lib, m, box):
    """One char per base tile: first letter of the tile name, '.' for floor."""
    x0, y0, x1, y1 = box
    print('     ' + ''.join(str(x % 10) for x in range(x0, x1)))
    for y in range(y0, y1):
        row = ''
        for x in range(x0, x1):
            name = lib.get_tile(m.get_tile(x, y)).get_name() or ' '
            row += '.' if 'floor' in name else (' ' if name == 'Nothing' else name[0])
        print(f"{y:4} {row}")

def render_png(lib, m, props, box, out):
    from PIL import Image, ImageDraw
    x0, y0, x1, y1 = box
    img = Image.new('RGB', ((x1 - x0) * 32, (y1 - y0) * 32))
    def blit(tid, x, y, off=(0, 0), rot=False, masked=False):
        if not tid: return
        t = lib.get_tile(tid)
        tile = Image.frombuffer('P', (32, 32), t.get_image(rot), 'raw', 'P', 0, 1)
        tile.putpalette(delv.colormap.pil)
        xo, yo = off[::-1] if rot else off
        mask = tile.point(lambda i: 255 if i else 0, '1') if masked else None
        img.paste(tile.convert('RGB'), ((x - x0) * 32 - xo, (y - y0) * 32 - yo), mask)
    for y in range(y0, y1):
        for x in range(x0, x1):
            blit(m.get_tile(x, y), x, y)
    shown = [p for p in props if p.show_in_map()]
    shown.sort(key=lambda p: lib.get_tile(lib.get_prop(p.proptype).get_tile(p.aspect)).draw_priority())
    for p in shown:
        pt = lib.get_prop(p.proptype)
        tid, off, rot = pt.get_tile(p.aspect), pt.get_offset(p.aspect), bool(p.rotated)
        # Multi-tile props (as in redelv): extra tiles above and/or left.
        (x, y), big = p.loc, lib.get_tile(tid).attributes & 0xC0
        up, left = ((x, y - 1), (x - 1, y))[::-1 if rot else 1]
        parts = {0x40: [(1, up)], 0x80: [(1, left)],
                 0xC0: [(3, (x - 1, y - 1)), (2, up), (1, left)]}.get(big, [])
        for back, (bx, by) in parts + [(0, (x, y))]:
            blit(tid - back, bx, by, off, rot, True)
    d = ImageDraw.Draw(img)
    labels = {}
    for p in props:
        loc = outer_loc(props, p)
        if loc and p.flags != 0xFF and x0 <= loc[0] < x1 and y0 <= loc[1] < y1:
            labels.setdefault(loc, []).append(str(p.index))
    for (x, y), idx in labels.items():
        px, py = (x - x0) * 32 + 1, (y - y0) * 32 + 1
        d.text((px + 1, py + 1), ','.join(idx), fill='black')
        d.text((px, py), ','.join(idx), fill='yellow')
    for x in range(x0, x1, 5):  # tile x coordinate ruler
        d.text(((x - x0) * 32 + 2, img.height - 11), str(x), fill='cyan')
    for y in range(y0, y1, 5):
        d.text((2, (y - y0) * 32 + 20), str(y), fill='cyan')
    img.save(out)
    print(f"wrote {out} (tiles x {x0}..{x1-1}, y {y0}..{y1-1})", file=sys.stderr)

if __name__ == '__main__':
    args = sys.argv[1:]
    def opt(name, has_value=True):
        if name not in args: return None
        i = args.index(name); args.pop(i)
        return args.pop(i) if has_value else True
    data = opt('--data') or os.path.join(ROOT, 'gamedata', 'Cythera Data')
    png, want_ascii = opt('--png'), opt('--ascii', False)
    scen, lib = load(data)
    cmd = args[0] if args else 'maps'
    if cmd == 'maps':
        for n, name, has_props in maps(scen):
            m = get_map(lib, n)
            print(f"0x{n:02X} {n:3}  {m.width:4}x{m.height:<4} "
                  f"{'props' if has_props else '     '}  {name}")
    elif cmd == 'props':
        n = find_map(scen, args[1])
        m, props = get_map(lib, n), lib.get_object(0x8100 | n, rw=False)
        print(f"map 0x{n:02X} {delv.hints._RES_HINTS.get(0x8000 | n)}: "
              f"{m.width}x{m.height}, {len(props)} props")
        list_props(lib, props)
        if want_ascii: ascii_map(lib, m, region(props, m))
        if png: render_png(lib, m, props, region(props, m), png)
    else:
        sys.exit(__doc__)
