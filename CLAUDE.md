# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

Hand-authored software renderer in C + SDL3. A learning project — the user is writing the graphics core by hand to get good at low-level C.

## Rules to always conform to

- **Do not modify the renderer core.** `src/renderer.c`, `src/projection.c`, `src/wireframe.c`, and their headers in `include/` are the user's learning surface. They iterate on those files themselves. Peripheral work (event handling, test scenes, tooling, docs, `issues.md`) is fair game.
- **Explain before you write.** When the user asks about a concept (projection math, Bresenham, memory), lead with intuition; let them write the C themselves unless they ask you to.
- **Commit hygiene.** No `Co-Authored-By` trailer. One logical unit per commit; never bundle unrelated changes.
- **Keep `issues.md` current** when the renderer changes. Format: open issues only, each entry as *what breaks / why / fix path*.
- **Architecture lives in `ARCHITECTURE.md`.** Read it before making structural claims; suggest updates to it when the design shifts.

## Build

```sh
cmake -S . -B build     # first time
./build.sh              # rebuild
./build.sh yes          # rebuild and launch (SDL_VIDEODRIVER=x11)
```

C23, links against system SDL3 and libm. No test suite is wired up.
