Let's port the game Cythera, by Ambrosia Software, to modern OSes.

I own a copy of the game. I bought it as a child and played it. Now it's not playable without emulating classic MacOS, which is of dubious legality itself. This is a personal, private project for me to play my favorite childhood game again. Ambrosia SW itself does not exist anymore and the game is not available commercially anymore.

The game used classic MacOS system calls a lot, and file formats and "resources" typical to classic MacOS as well. We only have compiled binaries, no source.

Some work has been done by others to work on the game data format:
https://github.com/BryceSchroeder/delvmod

My objective is simple:

- reverse engineer the game's data and code
- study how feasible it is to keep parts of it intact, and replace only the system calls
- or: recode the whole thing (without specification, only based on the compiled binary and the system calls)
- use a portable langauge and library, like C or Go and SDL, for instance
- the final deliverable is a portable, modern codebase that is able to run the game with 1-1 feature match with the original. you may, if necessary, have an offline data conversion step, so that the modern game reads from modernized data.

Again: we don't have specs, it would be too much work to specify a 1-1 correspondance, we have only the compiled binary and data, plus the reverse engineering work done before (see link above).

Analyze the given archive `Cythera_Installer.sit` (StuffIt archive), which is my personal copy of the game, study it, and plan the entire porting process.

Write a details technical analysis as you go, as well as a plan for yourself to follow.

Proceed directly to following your plan. Commit and push between each significant chunk of work. Work without human intervention until your goal is accomplished.
