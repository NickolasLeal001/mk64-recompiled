# MK64 Port

A native Windows port of **Mario Kart 64 (USA)**, built by statically recompiling the game's
MIPS code from your own ROM into C and running it on a PC replacement for the N64 libraries.
No emulator runs at play time: the game code is compiled into a regular `.exe`.

The repository contains **no game code or data**. Everything game-specific is generated on your
machine from a ROM you provide, and the ROM is read again at runtime for graphics, sound and
level data.

## What to expect

- The full game: menus, Grand Prix, Time Trials (with ghosts saved to the Controller Pak), VS and
  Battle modes, saved records (EEPROM).
- Higher internal resolution: up to 8x (2560x1920) on the GPU, up to 4x in software.
- Widescreen: 16:9, 16:10, 21:9, 32:9 or any `a:b` ratio, showing more of the track on the sides
  while the HUD stays centered.
- 60 fps display: in-between images are drawn between the game's own 30 fps frames.
- An optional FPS counter (F2).
- Rendering on the GPU through Vulkan (SDL_gpu), with an automatic fallback to the software
  renderer when Vulkan is not available.
- Keyboard and gamepad input, fully remappable in `mk64.ini`.

**Known limitations:**

- Only the USA version of the ROM is supported. SHA-1:
  `579c48e211ae952530ffc8738709f078d5dd215e`
- Windows only. The build scripts use MSVC.
- Only controller 1 is played from the keyboard or a gamepad. Multiplayer modes work, but the
  other players can't be controlled yet.
- The 60 fps in-between images interpolate the 3D scene. The HUD and objects whose vertices the
  game builds on the CPU still move at the game's 30 fps. Particles that appear or disappear in a
  frame are not interpolated.
- In GPU mode the F1 debug status appears in the window title, not over the picture.

## Requirements

| Tool | Notes |
|---|---|
| Windows 10/11, x64 | |
| [Visual Studio 2026](https://visualstudio.microsoft.com/) (Community or Build Tools) | The "Desktop development with C++" workload (MSVC x64). The bundled CMake and Ninja are used. |
| [Python 3](https://www.python.org/) | Must be on `PATH`. It runs the recompiler; no extra packages are needed. |
| [Vulkan SDK](https://vulkan.lunarg.com/) | Optional. Its `glslc` compiles the GPU shaders. Without it, the executable has the software renderer only. |
| Git and internet access | Needed only for the first build, which downloads SDL 3.4.16. |
| A Mario Kart 64 (USA) ROM | In `.z64`, `.v64` or `.n64` byte order. |

## Building

From the project folder:

```bat
compilar.bat C:\path\to\mario_kart_64.z64
```

This runs the whole flow, stopping with a message if any step fails:

1. **Checks the ROM:** byte order, size and SHA-1. A `.v64` or `.n64` file is converted to `.z64`
   order.
2. **Recompiles the game code:** `tools/recomp/analyze.py` finds the functions and
   `tools/recomp/recomp.py` translates them to C in `generated/`.
3. **Builds:** `build.bat` compiles with MSVC, CMake and Ninja. The first time, it also downloads
   and builds SDL3.
4. **Assembles `dist/`:** copies `mk64.exe` and the ROM there, as `rom.z64`.
5. **Tests the executable:** runs it for a few seconds without a window to check that it boots.

A rebuild takes about a minute. The first build takes longer, because SDL is compiled too.

Options:

| Option | Effect |
|---|---|
| `--saida FOLDER` | Put the result in another folder instead of `dist/`. |
| `--sem-rom` | Don't copy the ROM next to the executable. You then pass its path when starting the game. |
| `--sem-teste` | Skip the final test run. |

To rebuild after changing the port's own code (`src/`, `gfx/`, `runtime/`, `audio/`, `native/`),
run `build.bat` alone. It reuses `generated/` and only recompiles what changed.

## Playing

Run `dist\mk64.exe`. It opens `rom.z64` from its own folder, or a ROM path given as the first
argument:

```bat
mk64.exe C:\path\to\mario_kart_64.z64
```

### Default keys

| N64 | Keyboard |
|---|---|
| Analog stick | Arrow keys |
| A / B | X / C |
| Z | Z |
| L / R | A / S |
| Start | Enter |
| C buttons (up, down, left, right) | I / K / J / L |
| D-pad (up, down, left, right) | T / G / F / H |

The first gamepad found also works as controller 1.

| Key | Action |
|---|---|
| F2 | Show or hide the FPS counter |
| F11 | Toggle fullscreen |
| F1 | Debug status |
| Esc | Quit |

### Settings: `mk64.ini`

The first run creates `mk64.ini` next to the executable, with comments. The keys are in
Portuguese. Delete the file to go back to the defaults.

| Key | Default | Meaning |
|---|---|---|
| `escala` | `3` | Window size, as a multiple of 320x240 |
| `tela_cheia` | `0` | Start in fullscreen |
| `filtro` | `nitido` | Scaling filter: `nitido` (sharp) or `suave` (smooth) |
| `resolucao_interna` | `2` | Internal resolution, as a multiple of 320x240: 1 to 4 in software, up to 8 on the GPU |
| `renderizador` | `gpu` | `gpu` (Vulkan) or `cpu` (software); falls back to `cpu` on its own |
| `proporcao` | `4:3` | Aspect ratio: `4:3`, `16:9`, `16:10`, `21:9`, `32:9` or any `a:b` |
| `interpolacao` | `1` | 60 fps display with in-between images |
| `mostrar_fps` | `1` | FPS counter in the corner of the picture |
| `volume` | `100` | Volume, from 0 to 100 |
| `tecla_*` | see above | Keyboard mapping, using SDL key names (e.g. `X`, `Return`, `Left Shift`, `Up`) |

Some settings can also be set by environment variables, which take precedence over the file:
`MK64_RENDERER`, `MK64_RES_SCALE`, `MK64_ASPECT` and `MK64_INTERP`.

### Files the game writes

All of these go next to the executable:

- `mk64.eep`: saved records and settings (the game's EEPROM)
- `mk64.mpk`: Controller Pak data, which holds the Time Trial ghosts
- `mk64_runtime.log`: diagnostics, such as the renderer in use or why the GPU was not available

## How it works

- **Recompiler** (`tools/recomp/`): reads the ROM's code segments and finds every function,
  including jump tables and function pointers. It translates each MIPS R4300 instruction to C,
  using the semantics in `runtime/recomp.h`. The function list is in `analysis/functions.json`,
  and the ROM layout is in `analysis/rom_map.json`.
- **Native libultra** (`runtime/`): threads, message queues, the video interface, DMA, timers,
  EEPROM, controllers and the Controller Pak are reimplemented on the PC. The original library
  functions they replace are listed in `analysis/replaced.json`. Game threads run as fibers on a
  single OS thread.
- **Graphics** (`gfx/`):
  - The game's display lists (F3DEX microcode) are interpreted in `rsp.c`.
  - `rdp.c` draws the original 320x240 frame in software. This is the image the game itself sees.
  - The enlarged display image is drawn by the GPU (`gpu.c` with the shaders in `gfx/shaders/`) or
    by the software renderer.
- **Audio** (`audio/`): the audio microcode's commands are processed in `abi.c`, and SDL plays
  the result.
- **Native rewrites** (`native/`): a few game functions rewritten as readable C. They are listed
  in `analysis/rewritten.json`.

## Tests

These need `build/mk64.exe`, which `build.bat` produces.

```bat
python -I tools\replay.py C:\path\to\rom.z64
```

This plays back the 26 scripted runs in `tests/replays/` without a window and compares a hash of
each captured frame. The runs cover menus, every Time Trial track, Grand Prix, VS and Battle. Some
also check the EEPROM, the Controller Pak or values in RAM. Any difference fails the replay.

```bat
python -I tools\gpu_compare.py C:\path\to\rom.z64
```

This runs the replays with both renderers and compares the GPU's enlarged images with the
software ones.

## Legal

This project includes no Nintendo code, graphics, sound or other data. You need your own copy of
the game. Don't commit ROMs, generated code (`generated/`), save files or screenshots of the
game. `.gitignore` already excludes them.
