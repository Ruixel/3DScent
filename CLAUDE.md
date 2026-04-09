# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

3DScent is a Nintendo 3DS homebrew port of **Descent** (Parallax Software's 1994 first-person space shooter). It targets the ARM11 (3DS) using devkitARM and the citro3d graphics library.

## Build System

Requires devkitARM toolchain. Set environment variables before building:
```bash
export DEVKITPRO=/path/to/devkitPro
export DEVKITARM=/path/to/devkitPro/devkitARM
```

| Command | Output |
|---|---|
| `make all` | ELF executable (default) |
| `make 3dsx` | Homebrew `.3dsx` (run on modified 3DS) |
| `make cia` | Installable `.cia` cartridge image |
| `make 3ds` | Encrypted `.3ds` cartridge format |
| `make citra` | Build `.3dsx` and launch in Citra emulator |
| `make release` | All release formats (`.zip`, `.cia`, `.3ds`) |
| `make clean` | Remove `build/` and `output/` directories |

Output goes to `output/`. Build intermediates go to `build/`.

There is no test framework — compilation and manual testing in Citra emulator serve as validation.

## Architecture

### Source Module Layout

```
source/
  main/      # Game logic (~74k LOC) — all major subsystems
  3d/        # 3D math, polygon rendering, clipping
  2d/        # 2D graphics primitives, fonts, bitmaps
  vecmat/    # Vector/matrix math (fixed-point)
  fix/       # Fixed-point arithmetic + lookup tables
  bios/      # Platform HAL: input (joy/key/mouse), timer, 3DS-specific
  cfile/     # Custom file I/O abstraction
  iff/       # IFF file format parser (asset loading)
  mem/       # Memory allocation wrappers
  misc/      # Utilities: strings, errors, file helpers
  includes/  # Global type definitions (TYPES.h, ndsw.h)
```

### Key Files

- `source/main/INFERNO.c` — Main entry point
- `source/main/GAMESEQ.c` — Game state machine (menu → loading → gameplay → endgame)
- `source/main/GAME.c` — Core game loop
- `source/main/RENDER.c` — Rendering pipeline
- `source/main/PIGGY.c` / `PAGING.c` — Asset loading and paging (HOG files)
- `source/vecmat/VECMAT.h` — 3D math primitives
- `source/bios/ndsfunc.c` — 3DS platform-specific code
- `source/fix/FIX.h` — Fixed-point type definitions (all 3D math uses `fix` not `float`)

### Data & Assets

- `romfs/` — ROM filesystem mounted at runtime (game data files go here)
- `data/` — Binary data compiled into the executable
- `resources/` — 3DS metadata: `AppInfo`, `icon.png`, `banner.png`, `audio.wav`, `template.rsf`

### Shader Pipeline

GPU vertex shaders are written in `.v.pica` (PICA200 assembly), compiled by `picasso` into `.shbin` during the build, then linked as binary objects.

### Fixed-Point Math

**All 3D/physics calculations use fixed-point arithmetic** (`fix` type from `source/fix/FIX.h`), not floating-point. This is critical for performance on the ARM11 without a hardware FPU. Trig functions use precomputed lookup tables in `TABLES.c`.

### Game Loop Flow

```
Input (CONTROLS/JOY/KEY)
  → Physics (PHYSICS.c)
  → Collision (COLLIDE.c, FVI.c)
  → AI (AI.c, AIPATH.c)
  → State update (weapons, effects, objects)
  → Render (RENDER.c → 3D pipeline → 2D output)
  → Audio (DIGI.c, SONGS.c)
```

### Compiler Flags

- Architecture: `-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft`
- Optimization: `-O2 -fomit-frame-pointer -ffunction-sections`
- Defines: `-DARM11 -D_3DS`
- Libraries: `citro3d`, `ctru`, `libm`
