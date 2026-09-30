#!/usr/bin/env python3
"""Rebuild the stage saves in tests/saves/ (see tests/saves/README).

Usage: make_stages.py [STAGE...]      (default: all, in order)

A stage save is made the hybrid way: tools/delv_save.py edits do what is
only bookkeeping (a stronger hero, travel, the clock), and the game plays
for real everything the scenario's scripts decide (fights, conversations,
training), so the story state they set is the game's own.  Each stage starts
from the fixture (tests/fixture.txt, made first) or from an earlier stage,
then runs its steps:
  ('edit', 'KEY=VALUE ...')   tools/delv_save.py set on the saved game
  ('play', LINES)             load the game, run the script lines, save
                              (Cmd-S) and quit; LINES is a list of script
                              lines, where an item ('script', FILE, LAST)
                              stands for FILE's commands up to LAST
A play step fails on the same things as tests/run.sh (crash, stall, failed
expect).  $CYTHERA is the program (default build/cythera); logs and a
screenshot per step go to work/stages/.
"""
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAVES = os.path.join(ROOT, 'tests', 'saves')
LOAD = ['wait 1800', 'click 240 270', 'wait 120', 'key return', 'wait 1500']
SAVE = ['key s cmd', 'wait 300']

def repeat(lines, n):
    return [l for _ in range(n) for l in lines]

# Each stage: (start: None = the fixture, or a stage name, steps).
STAGES = {
    # The opening played for real (the travel test's route: Omen's Test, LandKing
    # Hall, the World, the guard at Odemia's gate: "Rescue kidnapped Ariadne"),
    # then the rescue: Eudoxus and his ruffians at the Abandoned Farmhouse
    # killed, Ariadne freed from the cellar and brought to the gate, which
    # opens.  The hero stands inside Odemia with Ariadne in the party.
    'odemia': (None, [
        ('play', [('script', 'tests/scripts/travel.txt', 'expect Goodbye, stranger')]
                 + ['key return', 'wait 120', 'key return', 'wait 120']),
        # a hero who can win the fight, next to Eudoxus (the ruffians come)
        ('edit', 'pos=AbandonedFarmhouse,20,19 body=30 reflex=30 mind=20 level=8 '
                 'maxhp=250 hp=250 skill:attack=12 skill:defense=12 skill:sword=12 '
                 'skill:shield=10 wear=0x63 wear=106 wear=113 wear=109 wear=138 gold=100'),
        # Eudoxus doesn't fight until attacked: context menu, Attack (he is at
        # (-1,-1) from the hero); then hit whatever attacks until all are dead.
        # His death sets story value 1 to 2 ("rescue" stage).
        ('play', ['wait 60', 'mousedown 438 133', 'wait 90', 'move 458 156', 'wait 10',
                  'mouseup 458 156', 'wait 60'] + repeat(['dclickfoe', 'wait 40'], 120)),
        # Ariadne's schedule puts her in the cellar while story value 1 < 3: let
        # the hour turn so the schedule runs, then go down to her
        ('edit', 'time=09:59 hp=250'),
        ('play', repeat(['key space', 'wait 30'], 40)),
        ('edit', 'pos=FarmhouseCellar,4,5'),
        ('play', ['wait 60', 'dclickchar 53', 'wait 120', 'expect Take me from this place',
                  'type y', 'key return', 'wait 120', 'expect thank you',
                  'key return', 'wait 120', 'key return', 'wait 120']),
        # back at the gate with her: the guard opens it; walk in
        ('edit', 'pos=Odemia,9,32'),
        ('play', ['wait 60', 'dclick 502 165', 'wait 120', 'expect You are safe',
                  'key return', 'wait 120', 'key return', 'wait 120', 'key return', 'wait 120',
                  'key right', 'wait 40', 'key right', 'wait 40', 'key right', 'wait 40']),
        # to her husband Philinus (28,26 at this hour): the quest is done and
        # she leaves the party
        ('edit', 'pos=27,26'),
        ('edit', '--char 53 pos=27,27'),
        ('play', ['wait 60', 'dclickchar 50', 'wait 120', 'expect safe',
                  'keyuntil return 120 30 in your debt', 'key return', 'wait 300',
                  'key return', 'wait 900']),
        ('edit', 'hp=250'),
    ]),
    # From 'odemia': to Lindus, headmaster of the Magisterium in Pnyx (10,44 from
    # 9:00 to 22:00), trained in Mana then Casting, which also gives the
    # grimoire; a scroll of Minor Embrightenment (edited in, as if found) is
    # learnt with it for real.  The hero can cast (magic 22/22).
    'magic': ('odemia', [
        ('edit', 'pos=Pnyx,10,45'),
        ('play', ['wait 60', 'dclickchar 80', 'wait 150',
                  'keyuntil return 120 6 Ways of the Mage', 'type y', 'key return', 'wait 150',
                  'expect What would you like to train on', 'click 188 33', 'wait 150',
                  'keyuntil return 120 10 use it on the scroll', 'key return', 'wait 150',
                  'type trai', 'key return', 'wait 150',
                  'expect What would you like to train on', 'click 188 55', 'wait 150',
                  'key return', 'wait 150', 'key return', 'wait 150',
                  'type bye', 'key return', 'wait 150', 'key return', 'wait 150',
                  'key return', 'wait 150']),
        ('edit', 'give=scroll@2'),
        # character window, Inventory: the scroll (newest) comes first, then the
        # grimoire; use the grimoire on the scroll
        ('play', ['wait 60', 'mousedown 362 404', 'wait 6', 'mouseup 362 404', 'wait 200',
                  'click 60 320', 'wait 100', 'dclick 70 122', 'wait 100',
                  'expect Use grimoire on what', 'click 37 122', 'wait 150',
                  'expect You now know the spell', 'key return', 'wait 100']),
    ]),
}


def script_lines(spec):
    """Script lines; ('script', FILE, LAST) items expand to FILE's commands
    up to LAST (without comments, screenshots and quit)."""
    out = []
    for item in spec:
        if isinstance(item, str):
            out.append(item)
            continue
        _, path, last = item
        for l in open(os.path.join(ROOT, path)):
            l = l.strip()
            if not l or l.startswith('#') or l.startswith('shot ') or l == 'quit':
                continue
            out.append(l)
            if l == last:
                break
        else:
            sys.exit(f'{path}: no line {last!r}')
    return out


def run_game(home, lines, log):
    script = os.path.join(home, 'script.txt')
    with open(script, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    env = dict(os.environ, CYTHERA_FONT_DIR=os.environ.get('CYTHERA_FONT_DIR', 'tests/fonts'))
    exe = os.environ.get('CYTHERA', os.path.join(ROOT, 'build', 'cythera'))
    with open(log, 'w') as f:
        rc = subprocess.call([exe, '--deterministic', '--timeout', '1800', '--sysdir',
                              os.path.join(home, 'System Folder'), '--script', script],
                             cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT)
    bad = [l.rstrip() for l in open(log, encoding='utf-8', errors='replace')
           if any(k in l for k in ('FATAL', 'unimplemented trap', 'stall:', 'EXPECT FAILED'))]
    if rc or bad:
        sys.exit(f'{log}: exit {rc}\n' + '\n'.join(bad[:5]))


def make(name, work, fixture):
    start, steps = STAGES[name]
    home = os.path.join(work, name)
    shutil.rmtree(home, ignore_errors=True)
    shutil.copytree(fixture, home)
    hero = os.path.join(home, 'Saved Games', 'Hero')
    if start:
        shutil.copy(os.path.join(SAVES, start), hero)
        shutil.copy(os.path.join(SAVES, start + '.rsrc'), hero + '.rsrc')
    for n, (kind, arg) in enumerate(steps, 1):
        print(f'{name} {n}/{len(steps)}: {kind}', flush=True)
        if kind == 'edit':
            subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'delv_save.py'),
                                   'set', hero] + arg.split(), cwd=ROOT)
        else:
            shot = f'shot work/stages/{name}_{n}.png'
            run_game(home, LOAD + script_lines(arg) + [shot] + SAVE + ['quit'],
                     os.path.join(work, f'{name}_{n}.log'))
    os.makedirs(SAVES, exist_ok=True)
    shutil.copy(hero, os.path.join(SAVES, name))
    shutil.copy(hero + '.rsrc', os.path.join(SAVES, name + '.rsrc'))
    subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'delv_save.py'),
                           'check', os.path.join(SAVES, name)], cwd=ROOT, stdout=subprocess.DEVNULL)
    print(f'{name}: wrote tests/saves/{name}')


def main(names):
    work = os.path.join(ROOT, 'work', 'stages')
    os.makedirs(work, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='cythera-stages.')
    try:
        fixture = os.path.join(tmp, 'fixture')
        os.makedirs(os.path.join(fixture, 'System Folder'))
        run_game(fixture, [l.strip() for l in open(os.path.join(ROOT, 'tests', 'fixture.txt'))
                           if l.strip() and not l.startswith('#')], os.path.join(work, 'fixture.log'))
        for name in names or list(STAGES):
            if name not in STAGES:
                sys.exit(f'unknown stage {name!r} (stages: {", ".join(STAGES)})')
            make(name, tmp, fixture)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main(sys.argv[1:])
