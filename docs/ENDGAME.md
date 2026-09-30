# The end of "Cure Alaric"

What the scenario's scripts need for the winning ending, read from the
scripts (delvmod's ddasm, offline) and played in the port from the stage save
`tests/saves/endgame` (`tools/make_stages.py`, `tests/scripts/win.txt`).
Story values QV and flags QF are the `Char` chunk's ([ANALYSIS.md
§3.1](ANALYSIS.md#31-saved-games)); character flag *n* is bit *n* of the
character's F009 byte 8 (`SetCharacterFlag`, 0F01/0F02).

## Items

| Item | Prop type | State |
|---|---|---|
| Crolna pieces ("glowing crystal", class 1025) | 37 | aspect 0, 1: first pieces; 4: those two merged; 2, 3: the other shards; 6: complete |
| Crolna, purified | 37 | aspect 6, d1 = 1 |
| Distiller (Charax's, at (7,5) in Charax's House, zone 17) | 233 empty, 234 loaded (d1 = 1) | can't be moved |
| Eartheart mushroom (from Sabinate) | 271 | |
| Deep-sea kelp | 270 | |
| Timeflux book | 26 (d1 5) | |

## The chain

1. **Pelagon's shard** (180D Talk, Kosha grotto, zone 23 (13,17) by schedule
   once QV 3 = 3): needs QV 3 = 3, Pelagon's character flag 0 clear, and the
   hero holding crolna aspects 4 and 3. He asks for the shards three times
   (answer `n`), then "Must we take them by force?" (anything but `n`): he
   creates the fourth shard, aspect 2. (Answering `y` to a request, or `n` to
   the last question, gives them up: no cure.)
2. **Combining** (1025 UseOn, crolna on crolna): aspects 0+1 → 4, 4+2 → 5,
   5+3 → 6 ("The fourth part joins onto the first three"); a queued vision of
   Omen follows each stage.
3. **Charax** (184F Talk, char 79): the Timeflux book (26/5) given sets his
   flag 1; with flag 1 and QV 3 ≥ 2 he asks for kelp (flag 2), the kelp (270)
   sets flag 3; then, with the mushroom (271) on the hero, he takes it (flag
   4 set, flag 3 cleared), walks to (7,6) and loads the distiller: 233 → 234,
   d1 = 1 ("...you can't move a distiller without spilling it").
4. **The distiller** (10EA Use, needs the Alchemy skill 210 trained; UseOn):
   loaded (d1 = 1) and used on the complete crolna (aspect/type word 6181 =
   aspect 6, type 37): the distiller empties (233), the crolna's d1 becomes 1,
   and Omen's vision curses the hero ("You've destroyed my master").
5. **Alaric** (1025 UseOn on type 34, the crolna must have aspect 6): Alaric's
   behaviour becomes 150, quest 0 "Cure Alaric" completes, **QV 0 = 1** if the
   crolna was purified (d1 ≠ 0), **2** if not, Alaric's character flag 1 is
   set; he then talks by himself.
6. **Alaric's Talk** (1802): character flag 1 clear → "with a look of
   desperation"; QV 0 = 2 → the bad ending (Pelagon, slides 0242,
   "You have damned Cythera to darkness."); otherwise "with a look of relief
   and calm", Magpie's thanks from Bahoudin, `SpecialView`, `BeginSlideshow`,
   `Slideshow(512, 0x0243[0..1])`, `EndSlideshow`,
   `GameOver("You have saved the land of Cythera from darkness.")`.

QV 3 is the Pelagon/time-flux thread: Berossus (1848) sets it to 2, then 3
(Stentor's tale of seeing Pelagon).

## What the stage save edits and what is played

Edited (tools/delv_save.py): crolna aspects 4 and 3, the mushroom, the
Alchemy skill, QV 3 = 3, Charax's flags 1 and 3, the clock (so Pelagon's
schedule runs), and the travel (Kosha grotto, Charax's House, LandKing Hall).
Played for real: Pelagon's conversation and shard, the two combinations and
Omen's visions, Charax and the distiller, the distiller on the crolna, then
(win.txt) the crolna on Alaric, his conversation, the slideshow and GameOver.
Not covered: the quests that lead there (Catamarca's plague, the crystal for
Lindus and Timon, Maayti, Jinrai, Comana, Stentor's net, Opheltius, the pipes
and House Comana's secret doors, the book, the kelp, Sabinate).
