#!/usr/bin/env python3
"""Read and edit Cythera saved games (docs/ANALYSIS.md section 3.1).

Usage:
  delv_save.py show SAVE [--char N] [--props]   readable summary
  delv_save.py flags SAVE [--all] [--refs]       story flags (read-only)
  delv_save.py check SAVE                        decode and re-encode every
                                                 part; must give the same bytes
  delv_save.py set SAVE [-o OUT] [--char N] KEY=VALUE...

set writes OUT (default: SAVE itself, in place; OUT gets SAVE's .rsrc
sidecar and a Finder type).  --char picks the character (default 1, the
hero).  Keys:
  pos=X,Y | pos=ZONE,X,Y   move; another zone moves the whole party and its
                           belongings as the engine does (ZONE: number or name)
  hp mp maxhp maxmp body reflex mind level exp training nutrition=N
  skill:NAME=N             skill level 0-15 (adds the skill if missing)
  gold=N                   the character's oboloi (first stack, or a new one)
  give=ITEM[:N]            add an item (name as in `show`, or a prop type)
  time=HH:MM  day=N  karma=N
SAVE is a saved game's data fork (e.g. "Saved Games/Hero").  Game data
comes from gamedata/ (tools/delv_archive.py).  Standard library only.
"""
import bisect, os, re, shutil, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import delv_archive as da

HERO = 1
OBOLS = 0x82                       # prop type of the coins
CHAR_FMT = '>4h32s8IIhBI27s'       # the 'Char' chunk (TDelverApp::SaveToFile)
HOUR = 0x1000                      # game clock units per hour; a day is 24 h
FLAGS_DELETED = 0xFF

# Character record (F009, 512 x 32 bytes; the VM's Character fields are
# SetField__Fsss5VAddr's cases).  name: (offset, size)
CHAR_FIELDS = {
    'body': (9, 1), 'reflex': (10, 1), 'mind': (11, 1), 'exp': (12, 2),
    'hp': (14, 1), 'maxhp': (15, 1), 'mp': (16, 1), 'maxmp': (17, 1),
    'level': (19, 1), 'nutrition': (27, 1), 'training': (28, 1),
}

def u16(b, o):
    return b[o] << 8 | b[o + 1]


class Prop:
    """A 16-byte prop record: flags, 24-bit location (x:12 y:12 on the map,
    else the holder: a character < 0x100 or a container's list index),
    aspect<<10 | type, d1, d2, propref, storeref, u16."""
    def __init__(self, raw):
        self.r = bytearray(raw)
    flags = property(lambda s: s.r[0], lambda s, v: s.r.__setitem__(0, v))
    loc = property(lambda s: int.from_bytes(s.r[1:4], 'big'),
                   lambda s, v: s.r.__setitem__(slice(1, 4), v.to_bytes(3, 'big')))
    type = property(lambda s: u16(s.r, 4) & 0x3FF)
    aspect = property(lambda s: u16(s.r, 4) >> 10)
    d3 = property(lambda s: u16(s.r, 6))

    def set_aspect(self, a):
        v = (a & 0x3F) << 10 | self.type
        self.r[4:6] = v.to_bytes(2, 'big')

    def set_d3(self, v):
        self.r[6:8] = v.to_bytes(2, 'big')

    def held_by(self):
        """Character holding it (flags 0x10: carried, 0x18: worn, 0x1c: a
        skill), else None."""
        return self.loc & 0xFFFF if self.flags in (0x10, 0x18, 0x1C) else None


class Level:
    """The props of one zone as the engine has them in memory: 256
    character slots (F306 for the current zone) then the 81zz records, so
    list index = 0x100 + record number, which is what container refs use."""
    def __init__(self, zone, slots, recs):
        self.zone = zone
        self.props = [Prop(slots[16 * i:16 * i + 16]) for i in range(256)] + \
                     [Prop(recs[16 * i:16 * i + 16]) for i in range(len(recs) // 16)]

    def slots_bytes(self):
        return b''.join(bytes(p.r) for p in self.props[:256])

    def recs_bytes(self):
        n = len(self.props)                  # SaveLevelProps drops deleted tails
        while n > 0x100 and self.props[n - 1].flags == FLAGS_DELETED:
            n -= 1
        return b''.join(bytes(p.r) for p in self.props[0x100:n])

    def contents(self, idx):
        return [i for i in range(0x100, len(self.props))
                if self.props[i].flags == 0x09 and self.props[i].loc & 0xFFFF == idx]


class Scenario:
    """What the save refers to in the scenario: names, maps, classes."""
    def __init__(self):
        self.arc = da.Archive(da.DEFAULT_DATA)
        self.names = da.Names(self.arc)
        self._cls = {}

    def char_name(self, i):
        d = self.arc.data(0x0201)
        n = u16(d, 0) & 0xFFF
        if i >= n:
            return f'#{i}'
        off = u16(d, 2 + 4 * i + 2)
        return da.read_cstr(d, off)[0]

    def res_string(self, v):
        """A VM value 0x3kkkrrrr (entry k of array resource rrrr) as text."""
        rid, k = v & 0xFFFF, v >> 16 & 0xFFF
        if v >> 28 != 3 or rid not in self.arc.entries:
            return f'<{v:08x}>'
        d = self.arc.data(rid)
        if d[0] & 0xF0 != 0x90 or k >= u16(d, 0) & 0xFFF:
            return f'<{v:08x}>'
        return da.read_cstr(d, u16(d, 2 + 4 * k + 2))[0]

    def zone_name(self, z):
        return da.ZONES[z] if z < len(da.ZONES) else f'zone {z}'

    def zone_size(self, z):
        if 0x8000 | z not in self.arc.entries:
            return None
        h = da.map_header(self.arc.data(0x8000 | z))
        return h['width'], h['height']

    def tile_at(self, z, x, y):
        m = self.arc.data(0x8000 | z)
        h = da.map_header(m)
        o = h['tiles_at'] + 2 * (y * h['width'] + x)
        return u16(m, o) & 0x1FFF

    def tile_attr(self, t):
        return struct.unpack('>I', self.arc.data(0xF002)[4 * t:4 * t + 4])[0]

    def props(self, z):
        rid = 0x8100 | z
        return self.arc.data(rid) if rid in self.arc.entries else b''

    def cls(self, rid):
        """{key: value} of a class resource's field table, or {}."""
        if rid not in self._cls:
            f = {}
            if rid in self.arc.entries:
                d = self.arc.data(rid)
                try:
                    t = u16(d, 0)
                    if d[0] < 0x80 and t < len(d):
                        for k in range(u16(d, t) & 0xFFF):
                            e = t + 2 + 6 * k
                            f[u16(d, e + 4)] = (struct.unpack('>I', d[e:e + 4])[0], d)
                except IndexError:
                    pass
            self._cls[rid] = f
        return self._cls[rid]

    def cls_int(self, rid, key):
        """A class field holding [n] (a one-entry array) or n."""
        v = self.cls(rid).get(key)
        if not v:
            return None
        v, d = v
        if v & 0x80000000:
            o = v & 0xFFFF
            if o + 6 <= len(d) and d[o] & 0xF0 == 0x90:
                v = struct.unpack('>I', d[o + 2:o + 6])[0]
            else:
                return None
        return v if v <= 0x07FFFFFF else None

    def stacking(self, ptype):
        """Class field 0x28 of the prop's class (10xx+type): bit 0x01 count
        in d2, 0x02 count in d1:d2, 0x20 aspect shows the count (the
        engine's prop attribute word has it in bits 8-15, SetItemCount)."""
        return self.cls_int(0x1000 + ptype, 0x28) or 0

    def skill_name(self, ptype):
        """Skills are props held with flags 0x1c; their class is 1Axx and
        its Look method (key 2) returns the name as a string literal."""
        v = self.cls(0x1A00 | ptype & 0xFF).get(2)
        if v:
            v, d = v
            o = v & 0xFFFF
            if d[o:o + 1] == b'\x81' and d[o + 3:o + 5] == b'\x8b\x44':
                return da.read_cstr(d, o + 5)[0]
        return f'skill {ptype:#x}'

    def prop_name(self, p):
        if p.flags == 0x1C:
            return self.skill_name(p.type)
        return self.names.prop(p.type, p.aspect & 0x1F)

    def count(self, p):
        st = self.stacking(p.type)
        if st & 0x01:
            return p.r[7] or 1
        if st & 0x02:
            return p.d3 or 1
        return 1

    def set_count(self, p, n):
        st = self.stacking(p.type)
        if st & 0x01:
            p.r[7] = n
        elif st & 0x02:
            p.set_d3(n)
        else:
            raise SystemExit(f'{self.prop_name(p)} does not stack')
        if st & 0x20:                        # SetItemCount: aspect by count
            a = next(i for i, top in enumerate((1, 2, 3, 4, 9, 19, 34, 1 << 30)) if n <= top)
            p.set_aspect(p.aspect & ~0x1F & 0x3F | a)


class Save:
    def __init__(self, path, scen=None):
        self.path = path
        self.arc = da.Archive(path)
        if not self.arc.is_save:
            sys.exit(f'{path}: not a saved game (no game state at 0400)')
        self.scen = scen or Scenario()
        self.chunks = [[t, bytes(c)] for t, c in da.stream_chunks(self.arc.data(0x0400))]
        self.chars = bytearray(self.arc.data(0xF009))
        v = struct.unpack(CHAR_FMT, self.chunk(b'Char'))
        (self.karma, self.languages, self.difficulty, self.unique_names,
         self.qv, qf, self.time, self.day, self.automap, self.play_seconds,
         self.char_pad) = v[0], v[1], v[2], v[3], bytearray(v[4]), list(v[5:13]), \
            v[13], v[14], v[15], v[16], v[17]
        self.qf = qf
        self.levels = {}

    # ---------------------------------------------------------- structure
    def chunk(self, tag):
        return next(c for t, c in self.chunks if t == tag)

    def set_chunk(self, tag, data):
        next(c for c in self.chunks if c[0] == tag)[1] = bytes(data)

    def char_bytes(self):
        return struct.pack(CHAR_FMT, self.karma, self.languages, self.difficulty,
                           self.unique_names, bytes(self.qv), *self.qf, self.time,
                           self.day, self.automap, self.play_seconds, self.char_pad)

    def stream(self):
        self.set_chunk(b'Char', self.char_bytes())
        return b''.join(t + struct.pack('>I', len(c) + 4) + c for t, c in self.chunks)

    def resource(self, rid):
        """The save's resource, else the scenario's (what the engine reads)."""
        if rid in self.arc.entries:
            return self.arc.data(rid)
        return self.scen.arc.data(rid) if rid in self.scen.arc.entries else None

    def level(self, zone):
        """Props of a zone.  The current zone's character slots are F306;
        another zone's are rebuilt when the party moves there."""
        if zone not in self.levels:
            recs = self.resource(0x8100 | zone) or b''
            slots = self.arc.data(0xF306) if zone == self.zone else bytes(0x1000)
            self.levels[zone] = Level(zone, slots, recs)
        return self.levels[zone]

    def encoded(self):
        """{rid: bytes} of everything this class re-encodes."""
        out = {0x0400: self.stream(), 0xF009: bytes(self.chars)}
        for z, lv in self.levels.items():
            out[0x8100 | z] = lv.recs_bytes()
            if z == self.zone:
                out[0xF306] = lv.slots_bytes()
        return out

    def changes(self):
        out = {}
        for rid, data in self.encoded().items():
            if rid in self.arc.entries and self.arc.data(rid) == data:
                continue
            if rid not in self.arc.entries and rid >> 8 == 0x81 and \
                    self.scen.props(rid & 0xFF) == data:
                continue
            out[rid] = data
        return out

    def write(self, out):
        data = self.arc.rebuilt(self.changes())
        tmp = out + '.tmp'
        with open(tmp, 'wb') as f:
            f.write(data)
        if os.path.exists(out):
            os.remove(out)                   # rename() doesn't replace on Windows
        os.rename(tmp, out)
        if os.path.abspath(out) != os.path.abspath(self.path):
            if os.path.exists(self.path + '.rsrc'):
                shutil.copy(self.path + '.rsrc', out + '.rsrc')
            finder_type(out)

    # ----------------------------------------------------------- characters
    def get(self, i, field):
        o, n = CHAR_FIELDS[field]
        return int.from_bytes(self.chars[32 * i + o:32 * i + o + n], 'big')

    def put(self, i, field, v):
        o, n = CHAR_FIELDS[field]
        if not 0 <= v < 1 << 8 * n:
            sys.exit(f'{field}={v}: out of range 0-{(1 << 8 * n) - 1}')
        self.chars[32 * i + o:32 * i + o + n] = v.to_bytes(n, 'big')

    def char_loc(self, i):
        w = struct.unpack('>I', self.chars[32 * i:32 * i + 4])[0]
        return w >> 24, w >> 12 & 0xFFF, w & 0xFFF

    def set_char_loc(self, i, zone, x, y):
        self.chars[32 * i:32 * i + 4] = struct.pack('>I', zone << 24 | x << 12 | y)

    def alive(self, i):
        return u16(self.chars, 32 * i + 6) & 1

    def in_party(self, i):
        return self.alive(i) and self.chars[32 * i + 8] & 0x40

    def party(self):
        return [i for i in range(256) if self.in_party(i)]

    @property
    def zone(self):
        return self.char_loc(HERO)[0]

    def name(self, i):
        return self.arc.player if i == HERO and self.arc.player else self.scen.char_name(i)

    def held(self, c, zone=None):
        lv = self.level(self.zone if zone is None else zone)
        return [i for i, p in enumerate(lv.props) if i >= 0x100 and p.held_by() == c]

    def skills(self, c):
        lv = self.level(self.zone)
        return [i for i in self.held(c) if lv.props[i].flags == 0x1C]

    def gold(self, c):
        lv, n = self.level(self.zone), 0
        todo = [i for i in self.held(c) if lv.props[i].flags != 0x1C]
        while todo:
            i = todo.pop()
            if lv.props[i].type == OBOLS:
                n += self.scen.count(lv.props[i])
            todo += lv.contents(i)
        return n

    # ------------------------------------------------------------- clock
    def clock(self):
        t = self.time
        return self.day, t // HOUR, (t % HOUR) * 60 // HOUR

    def todo(self):
        """0401 (TToDo::AddToDo/DoneToDo): 256 x (u8 done, u8, u16 day
        added, u32 text: a VM value, none = unused).  -> used entries."""
        d = self.resource(0x0401) or b''
        out = []
        for n in range(len(d) // 8):
            done, _, day, v = struct.unpack('>BBHI', d[8 * n:8 * n + 8])
            if v != 0x5000FFFF:
                out.append((n, done, day, self.scen.res_string(v)))
        return out

    # ------------------------------------------------------------ flags
    def qf_set(self, n):
        return self.qf[n >> 5] >> (n & 31) & 1

    # ------------------------------------------------------------ edits
    def move(self, c, zone, x, y):
        size = self.scen.zone_size(zone)
        if size is None:
            sys.exit(f'no map for zone {zone}')
        if not (0 <= x < size[0] and 0 <= y < size[1]):
            sys.exit(f'{x},{y} is outside {self.scen.zone_name(zone)} ({size[0]}x{size[1]})')
        t = self.scen.tile_at(zone, x, y)
        if self.scen.tile_attr(t) & 0x200:   # water, walls, trees (a guess)
            print(f'warning: {x},{y} is {self.scen.names.tile(t)} (tile {t:#x}), '
                  'probably not walkable', file=sys.stderr)
        if zone == self.zone:
            z, _, _ = self.char_loc(c)
            if z != zone:
                sys.exit(f'{self.name(c)} is not in the current zone')
            self.set_char_loc(c, zone, x, y)
            slot = self.level(zone).props[c]
            if slot.flags != FLAGS_DELETED:
                slot.loc = x << 12 | y
            return
        self.change_zone(zone, x, y)

    def change_zone(self, new, x, y):
        """TGameViewer::GoToLocation, minus the scripts it runs: carry every
        character's belongings over (ShuffleUpPartyInventory, CopyProp),
        store the old zone (SaveLevelProps), load the new one and append them
        (LoadLevelProps), place the party (MovePartyBetweenLevels) and the
        characters who live there (CueCharacters)."""
        old = self.zone
        lo = self.level(old)
        props, count = lo.props, len(lo.props)
        def copy(i, parent):                 # CopyProp
            j = len(props)
            props.append(Prop(props[i].r))
            if parent:
                props[i].loc = parent
                props[j].loc = parent
            props[i].flags = FLAGS_DELETED
            for k in range(0x100, count):
                if props[k].flags == 0x09 and props[k].loc & 0xFFFF == i:
                    copy(k, j)
        for c in range(256):
            for i in range(0x100, count):
                if props[i].flags in (0x10, 0x18, 0x1C) and props[i].loc & 0xFFFF == c:
                    copy(i, 0)
        carried = props[count:]
        del props[count:]
        self.slots_to_chars(props, old)
        ln = self.level(new)
        if 0x8100 | new in self.arc.entries:  # LoadLevelProps' merge with the scenario
            orig = self.scen.props(new)
            for k, q in enumerate(ln.props[0x100:]):
                o = Prop(orig[16 * k:16 * k + 16]) if 16 * k + 16 <= len(orig) else None
                if q.flags in (0x20, 0x21) and o and o.type == q.type and o.flags in (0, 1):
                    q.r[:] = o.r
        ln.props[:256] = [Prop(p.r) for p in props[:256]]
        base = len(ln.props)
        for p in carried:                     # LoadLevelProps: renumber containers
            q = Prop(p.r)
            if q.loc & 0xFFFF >= 0x100 and q.flags == 0x09:
                q.loc = q.loc - count + base
            ln.props.append(q)
        for i in range(256):                  # MovePartyBetweenLevels
            s = ln.props[i]
            if self.in_party(i):
                if self.chars[32 * i] == old:
                    self.set_char_loc(i, old, x, y)
                    self.place(ln, i)
                    s.r[6] = s.r[7] = 0
                    self.chars[32 * i] = new
                else:
                    s.flags = FLAGS_DELETED
        for i in range(256):                  # CueCharacters
            if self.alive(i):
                if self.chars[32 * i] == new:
                    self.place(ln, i)
                else:
                    ln.props[i].flags = FLAGS_DELETED
        self.slots_to_chars(ln.props, new)    # what saving the game does next
        # The old zone's monsters and windows refer to its props: drop them
        # (GoToLocation's LeavingLevel); RebuildParty hatches the party's
        # slots (flags 0x42) again when the game is loaded.
        self.set_chunk(b'Mons', b'')
        self.set_chunk(b'Wind', b'')

    def slots_to_chars(self, props, zone):
        """SaveLevelProps: every used slot puts its character in the zone,
        at the slot's place, with its aspect and type."""
        for i, p in enumerate(props[:256]):
            if p.flags != FLAGS_DELETED:
                self.chars[32 * i:32 * i + 4] = (zone << 24 | p.loc).to_bytes(4, 'big')
                self.chars[32 * i + 4] = self.chars[32 * i + 4] & 0x83 | p.r[4] & 0x7C
                t = u16(self.chars, 32 * i + 4) & ~0x3FF | p.type
                self.chars[32 * i + 4:32 * i + 6] = t.to_bytes(2, 'big')

    def place(self, lv, i):
        """A character's slot on the map (flags 0x42), as the engine does."""
        s, c = lv.props[i], self.chars[32 * i:32 * i + 32]
        s.flags = 0x42
        s.loc = int.from_bytes(c[1:4], 'big')
        s.r[4] = s.r[4] & 0x83 | c[4] & 0x7C
        t = u16(s.r, 4) & ~0x3FF | u16(c, 4) & 0x3FF
        s.r[4:6] = t.to_bytes(2, 'big')

    def add_prop(self, c, flags, ptype, aspect=0):
        lv = self.level(self.zone)
        p = Prop(bytes(16))
        p.r[0] = flags
        p.loc = c
        p.r[4:6] = ((aspect & 0x3F) << 10 | ptype).to_bytes(2, 'big')
        lv.props.append(p)                   # a new index: nothing refers to it
        return p

    def set_skill(self, c, name, n):
        name = name.replace('_', ' ')         # "lock_picking": no spaces in run.sh
        if not 0 <= n <= 15:
            sys.exit('skill levels are 0-15')
        lv = self.level(self.zone)
        for i in self.skills(c):
            p = lv.props[i]
            if self.scen.skill_name(p.type).lower() == name.lower():
                p.set_aspect(p.aspect & 0x30 | n)
                return
        known = {self.scen.skill_name(t).lower(): t for t in range(0xC0, 0x100)
                 if 0x1A00 | t in self.scen.arc.entries}
        if name.lower() not in known:
            sys.exit(f'unknown skill {name!r} (skills: {", ".join(sorted(known))})')
        self.add_prop(c, 0x1C, known[name.lower()], 0x10 | n)

    def set_gold(self, c, n):
        lv = self.level(self.zone)
        stacks = [i for i in self.held(c) if lv.props[i].type == OBOLS]
        if stacks:
            p = lv.props[stacks[0]]
        elif n:
            p = self.add_prop(c, 0x10, OBOLS)
        else:
            return
        if n:
            self.scen.set_count(p, n)
        else:
            p.flags = FLAGS_DELETED

    def give(self, c, item, n):
        """n of an item: one stack if it stacks, else n props."""
        t = prop_type(self.scen, item)
        if not self.scen.stacking(t) & 0x03:
            for _ in range(n):
                self.add_prop(c, 0x10, t)
            return
        self.scen.set_count(self.add_prop(c, 0x10, t), n)


def prop_type(scen, item):
    """A prop type from a number or an item name ("flatbread", "obols")."""
    if re.fullmatch(r'\d+|0x[0-9a-fA-F]+', item):
        return int(item, 0)
    want = item.lower().replace('_', ' ')
    names = scen.names
    lasts = [last for last, _ in names.tiles]
    for a in range(8):                        # stacks change aspect (and name)
        for t, first in enumerate(names.prop_tile):
            k = bisect.bisect_left(lasts, first + a)   # Names.tile, faster
            if k < len(lasts) and da.plural(names.tiles[k][1]).lower() in (want, want.rstrip('s')):
                return t
    sys.exit(f'unknown item {item!r}')


def finder_type(path):
    """Give a new saved game the Finder type the game's Open dialog lists
    ('DelP' 'Delv'): the port keeps them in .finderinfo (src/os/files.c)."""
    d, leaf = os.path.split(os.path.abspath(path))
    fi = os.path.join(d, '.finderinfo')
    lines = open(fi, encoding='utf-8').read().splitlines() if os.path.exists(fi) else []
    lines = [l for l in lines if l.split('\t')[0] != leaf]
    lines.append(f'{leaf}\t44656c50 44656c76 0000')
    with open(fi, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


def parse_zone(scen, s):
    if re.fullmatch(r'\d+|0x[0-9a-fA-F]+', s):
        return int(s, 0)
    key = s.lower().replace(' ', '').replace("'", '')
    for z, n in enumerate(da.ZONES):
        if n.lower().replace(' ', '').replace("'", '') == key:
            return z
    sys.exit(f'unknown zone {s!r} (names: {", ".join(da.ZONES)})')

# -------------------------------------------------------------- commands
def show(sv, args):
    sc = sv.scen
    day, h, m = sv.clock()
    ps = sv.play_seconds
    print(f'{sv.path}: {sv.arc.title!r}, player {sv.arc.player!r}')
    print(f'day {day}, {h:02d}:{m:02d} (clock {sv.time:#x}); played {ps // 3600}:{ps // 60 % 60:02d}:{ps % 60:02d}')
    print(f'karma {sv.karma}, difficulty {sv.difficulty}, languages {sv.languages:#x}, '
          f'automap {"on" if sv.automap else "off"}')
    z, x, y = sv.char_loc(HERO)
    print(f'location: {sc.zone_name(z)} (zone {z}) at {x},{y}')
    one = args.opt('--char')
    who = [int(one, 0)] if one else sv.party()
    for c in who:
        f = {k: sv.get(c, k) for k in CHAR_FIELDS}
        cz, cx, cy = sv.char_loc(c)
        print(f'\n#{c} {sv.name(c)}{"" if sv.in_party(c) else " (not in the party)"}: '
              f'{sc.zone_name(cz)} {cx},{cy}')
        print(f'  health {f["hp"]}/{f["maxhp"]}  magic {f["mp"]}/{f["maxmp"]}  '
              f'body {f["body"]} reflex {f["reflex"]} mind {f["mind"]}')
        print(f'  level {f["level"]}  exp {f["exp"]}  training {f["training"]}  '
              f'nutrition {f["nutrition"]}  gold {sv.gold(c)}')
        lv = sv.level(sv.zone)
        sk = [f'{sc.skill_name(lv.props[i].type)} {lv.props[i].aspect & 0xF}'
              for i in sv.skills(c)]
        print('  skills: ' + (', '.join(sk) or '-'))
        def item(i, depth):
            p = lv.props[i]
            n = sc.count(p)
            print(f'  {"  " * depth}{"worn " if p.flags == 0x18 else ""}{sc.prop_name(p)}'
                  f'{f" x{n}" if n > 1 else ""}')
            for k in lv.contents(i):
                item(k, depth + 1)
        inv = [i for i in sv.held(c) if lv.props[i].flags != 0x1C] + \
              [i for i in range(0x100, len(lv.props))   # 11: held, stays in the zone
               if lv.props[i].flags == 0x11 and lv.props[i].loc & 0xFFFF == c]
        print('  inventory:' + ('' if inv else ' -'))
        for i in inv:
            item(i, 1)
    todo = sv.todo()
    print('\nto-do:' + ('' if todo else ' -'))
    for n, done, day, text in todo:
        print(f'  {n:3} {"done" if done else "    "} day {day:<3} {text}')
    setf = [n for n in range(256) if sv.qf_set(n)]
    print(f'\nstory flags set: {" ".join(map(str, setf)) or "-"}; state values: ' +
          (' '.join(f'{i}={v}' for i, v in enumerate(sv.qv) if v) or '-'))
    print(f'monsters active: {len(monsters(sv.chunk(b"Mons")))}, '
          f'heap blocks in use: {heap_used(sv.arc.data(0xF307))}, '
          f'journal pages: {sum(1 for r in sv.arc.ids() if r >> 8 == 0xE0)}')
    print('zones stored: ' + ', '.join(sc.zone_name(r & 0xFF) for r in sv.arc.ids() if r >> 8 == 0x81))
    if args.flag('--props'):
        print()
        print(da.props_text(sv.arc, sc.names, sv.level(sv.zone).recs_bytes()), end='')


def monsters(d):
    """'Mons' entries: kind (b: 9/10 crawler, 11 dragon, 12 octopus, else
    plain), then TActiveMonster::Save: h prop index, h, h; for level
    monsters (index >= 0x100) h and 32 bytes of monster data; b facing and
    4 x h; h queue length, b h h l per queued activity.  Crawlers add h n
    and n x h (their segments), dragons 4 x h, octopuses 8 x h.
    -> [(prop index, bytes)]."""
    out, i = [], 0
    while i < len(d):
        s, kind = i, d[i]
        idx = struct.unpack('>h', d[i + 1:i + 3])[0]
        i += 7
        if idx >= 0x100:
            i += 2 + 0x20
        i += 9
        n = struct.unpack('>h', d[i:i + 2])[0]
        i += 2 + 9 * n
        if kind in (9, 10):
            i += 2 + 2 * struct.unpack('>h', d[i:i + 2])[0]
        elif kind == 11:
            i += 8
        elif kind == 12:
            i += 16
        out.append((idx, d[s:i]))
    return out


def heap_used(d):
    """THeap blocks (THeap::Next, DataChunkSize, RebuildTOC): u32 size,
    u16 handle, a byte whose bits 4-6 are the kind (0 free) and bit 3 adds
    4 bytes; the data follows, padded to 4.  A new game's heap is one free
    block.  -> number of blocks in use."""
    i, n = 0, 0
    while i + 8 <= len(d):
        size, handle, kind = struct.unpack('>IHB', d[i:i + 7])
        if kind >> 4 & 7 and handle:
            n += 1
        i += 8 + (size + 3 & ~3) + (4 if kind & 8 else 0)
    return n


def flag_refs(scen):
    """Script references to story flags: syscalls DC/DD (get/set state value
    QV) and DE/DF (get/set flag QF) with a constant first argument (41 NN
    or 42 NNNN).  A byte scan: a hint of who uses a flag, not a parse."""
    ops = {0xDC: ('QV', 'get'), 0xDD: ('QV', 'set'), 0xDE: ('QF', 'get'), 0xDF: ('QF', 'set')}
    refs = {}
    for rid in scen.arc.ids():
        if rid >> 8 >= 0x80:
            continue
        x = scen.arc.data(rid)
        for i in range(len(x) - 3):
            if x[i] in ops and x[i + 1] in (0x41, 0x42):
                n = x[i + 2] if x[i + 1] == 0x41 else x[i + 2] << 8 | x[i + 3]
                kind, op = ops[x[i]]
                refs.setdefault((kind, n), {}).setdefault(rid, set()).add(op)
    return refs


def res_label(scen, rid):
    p, n = rid >> 8, rid & 0xFF
    if p == 0x18:
        return f'{rid:04X} {scen.char_name(n)}'
    if p in (0x14, 0x15):
        return f'{rid:04X} {scen.zone_name(n)}'
    return f'{rid:04X}'


def flags(sv, args):
    """QF: 256 bits (VM syscalls DE/DF), QV: 32 bytes (DC/DD)."""
    refs = flag_refs(sv.scen) if args.flag('--refs') else {}
    every = args.flag('--all')
    def line(kind, n, val):
        r = refs.get((kind, n), {})
        txt = '; '.join(f'{"/".join(sorted(o))} {res_label(sv.scen, rid)}' for rid, o in sorted(r.items()))
        return f'{kind} {n:3} = {val}' + (f'   ({txt})' if txt else '')
    for n in range(256):
        if every or sv.qf_set(n) or (refs and ('QF', n) in refs):
            print(line('QF', n, sv.qf_set(n)))
    for n in range(32):
        if every or sv.qv[n] or (refs and ('QV', n) in refs):
            print(line('QV', n, sv.qv[n]))


def check(sv, args):
    """Every decoded part must re-encode to the stored bytes, and writing
    without edits must give the file back."""
    ok = True
    for rid in [0x8100 | sv.zone] + [r for r in sv.arc.ids() if r >> 8 == 0x81]:
        sv.level(rid & 0xFF)
    for rid, data in sv.encoded().items():
        stored = sv.resource(rid) or b''
        if rid == 0xF306 or rid >> 8 != 0x81 or rid in sv.arc.entries:
            same = stored == data
            print(f'{rid:04X} {"same" if same else "DIFFERENT"} ({len(data)} bytes)')
            ok &= same
    parts = monsters(sv.chunk(b'Mons'))
    same = b''.join(p for _, p in parts) == sv.chunk(b'Mons')
    print(f'Mons: {len(parts)} monsters, {"same" if same else "DIFFERENT"}')
    ok &= same
    same = sv.arc.rebuilt({}) == sv.arc.buf
    print(f'file: {"identical" if same else "DIFFERENT"} when written unchanged')
    ok &= same
    sys.exit(0 if ok else 1)


def set_(sv, args):
    c = int(args.opt('--char') or str(HERO), 0)
    out = args.opt('-o') or sv.path
    for kv in args.rest:
        if '=' not in kv:
            sys.exit(f'expected KEY=VALUE, got {kv!r}')
        k, v = kv.split('=', 1)
        k = k.lower()
        if k in CHAR_FIELDS:
            sv.put(c, k, int(v, 0))
        elif k == 'pos':
            parts = v.split(',')
            if len(parts) == 2:
                sv.move(c, sv.char_loc(c)[0], int(parts[0], 0), int(parts[1], 0))
            elif len(parts) == 3:
                sv.move(c, parse_zone(sv.scen, parts[0]), int(parts[1], 0), int(parts[2], 0))
            else:
                sys.exit('pos=X,Y or pos=ZONE,X,Y')
        elif k.startswith('skill:'):
            sv.set_skill(c, k[6:], int(v, 0))
        elif k == 'gold':
            sv.set_gold(c, int(v, 0))
        elif k == 'give':
            item, _, n = v.partition(':')
            sv.give(c, item, int(n or '1', 0))
        elif k == 'time':
            hh, mm = (int(x) for x in v.split(':'))
            if not (0 <= hh < 24 and 0 <= mm < 60):
                sys.exit('time=HH:MM, 00:00-23:59')
            sv.time = hh * HOUR + (mm * HOUR + 59) // 60
        elif k == 'day':
            sv.day = int(v, 0)
        elif k == 'karma':
            sv.karma = int(v, 0)
        else:
            sys.exit(f'unknown key {k!r}\n' + __doc__)
    sv.write(out)
    ch = sv.changes()
    print(f'wrote {out}' + (f' ({", ".join(f"{r:04X}" for r in sorted(ch))} changed)' if ch else ' (unchanged)'))


class Args:
    def __init__(self, argv):
        self.rest = list(argv)

    def opt(self, name):
        if name in self.rest:
            i = self.rest.index(name)
            self.rest.pop(i)
            return self.rest.pop(i)
        return None

    def flag(self, name):
        if name in self.rest:
            self.rest.remove(name)
            return True
        return False


def main(argv):
    sys.stdout.reconfigure(errors='backslashreplace')
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd, path, args = argv[0], argv[1], Args(argv[2:])
    fn = {'show': show, 'flags': flags, 'check': check, 'set': set_}.get(cmd)
    if not fn:
        sys.exit(__doc__)
    fn(Save(path), args)


if __name__ == '__main__':
    main(sys.argv[1:])
