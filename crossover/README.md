# crossover

A native, personal **crossover** engine: one shared hub/engine that hosts the
game worlds of two handheld-era games behind a single native Windows app.
Portable C99, no external dependencies for the core; SDL2 for the windowed
graphics/input layer (optional).

The design is a single native PC program with a shared overworld/hub plus two
game "worlds" driven by faithful CPU + video cores, so both worlds run side by
side and content can move between them. ROMs are supplied by you at runtime;
the engine ships no game content.

## Layout

    core/       CPU + video cores and the shared engine (portable C99)
      lr35902.c/.h   Game Boy CPU (Sharp LR35902)          - GB world
      ppu_gb.c/.h    Game Boy PPU (bg/window/sprites)      - GB video
      gb.c/.h        full GB machine: CPU + PPU + bus + MBC + joypad
      w65816.c/.h    SNES CPU (WDC 65816, 8/16-bit M/X, all 256 opcodes)
      ppu_snes.c/.h  SNES PPU (BG modes incl. Mode 7, CGRAM, OAM sprites)
      snes.c/.h      full SNES machine: 65816 + PPU + APU + cartridge bus
                     (LoROM/HiROM + SRAM) + DMA/HDMA + NMI/H-V IRQ + joypad
                     (dot-based beam loop)
      spc700.c/.h    SNES audio CPU (Sony SPC700, full instruction set)
      apu.c/.h       SNES audio subsystem: 64KB ARAM + IPL boot ROM +
                     $2140-$2143 ports + DSP (8 voices, BRR, ADSR, echo)
      hub.c/.h       shared hub: hosts both machines, entities, tile map
    platform/   OS front-end + shared fixed-step loop
      frontend.c/.h  view switching (HUB / GB / SNES), ROM load, input map
      main_win.c     Windows/SDL2 entry + headless entry
    tests/      headless test suites (run anywhere with gcc)

## Stage status

- [x] Stage 1 - scaffold + **LR35902 (GB) CPU core**
- [x] Stage 2 - **fixed-step frontend loop** (SDL2 + headless backends)
- [x] Stage 3 - **65816 (SNES) CPU core** (8/16-bit M/X, block ops, indirect modes)
- [x] Stage 4 - **GB PPU** + full GB machine (bus/MBC) 
- [x] Stage 5 - **SNES PPU** + full SNES machine (LoROM bus)
- [x] Stage 6 - shared hub + **live-machine frontend** (load a ROM, play it)
- [x] Stage 7 - **controller input** for both machines (GB JOYP, SNES $4016/$4218)
- [x] Stage 8 - **Mode 7** affine BG + **general DMA** + **HDMA** + **vblank NMI**
- [x] Stage 9 - **color math + windowing**, **offset-per-tile**, **dot-based H/V
                timing** ($2137/$213C/$213D, $4212, H/V IRQ comparators)
- [x] Stage 10 - **SNES audio**: SPC700 CPU, IPL boot upload, DSP voices,
                 frame audio out (32 kHz stereo)
- [x] Stage 11 - **full 65816 instruction set**, **LoROM + HiROM** cartridge
                 mapping, **cartridge SRAM**, copier-header stripping
- [x] Stage 12 - **game pad support** (SDL2 game controller API, hot-plug,
                 DualSense Edge detection and SNES-style mapping)

## Build & test (any host with gcc)

    make test          # builds + runs every suite (10 suites, headless)

Individual suites: `tests/test_cpu`, `test_65816`, `test_ppu`, `test_gb`,
`test_hub`, `test_snes_ppu`, `test_snes`, `test_spc700`, `test_apu`,
`test_frontend`.  All are clean under `-Wall -Wextra -Werror`.

## Run (headless, no GUI needed)

    ./crossover_headless [--gb rom.gb] [--snes rom.smc] [frames]

With no ROM it renders the shared hub. `--gb`/`--snes` load a ROM and switch
to that machine's live PPU output.

## Build for Windows

*With MinGW-w64 (gcc):*

    build_windows.bat              # produces crossover.exe (headless/hub)
                                   # add -DUSE_SDL2 + SDL2 paths for the window

*Or with CMake (MSVC or MinGW):*

    cmake -B build
    cmake --build build
    ctest --test-dir build

The core is dependency-free. The SDL2 window is optional; define `USE_SDL2`
and link SDL2 to enable it (see `build_windows.bat`).

## Controls (windowed build)

Keyboard:

    Arrows = D-pad   Z/J = A   X/K = B   A/S = Y/X   Q/W = L/R
    Shift = Select   Enter = Start   Esc = quit

Game pad — any SDL2 *game controller*, hot-plugged (`SDL_CONTROLLERDEVICEADDED`
/ `REMOVED`). The SNES diamond keeps its physical positions on a PS-style pad,
where SDL maps Cross->A, Circle->B, Triangle->Y, Square->X:

    SNES A (right) = Circle    SNES B (bottom) = Cross
    SNES X (top)   = Triangle  SNES Y (left)   = Square
    L / R          = L1 / R1, or L2 / R2 past a half-press
    Select / Start = Share (or the mic button) / Options (or the PS button)
    D-pad          = D-pad, or either stick past a ~8000 deadzone

**DualSense Edge**: detected by Sony VID `$054C` / PID `$0DF2` (the same pair
over USB and Bluetooth; the original DualSense is `$0CE6`) with a name fallback
for backends that hide VID/PID, and named on connect. Edge support needs SDL2
**>= 2.26.5** (the first release with an Edge mapping); this project is
verified against **SDL2 2.32.10**.

## Windows build

`build_windows.bat` covers the MinGW build. The exact command that produced the
published binary (MinGW-w64 cross-compiling from Linux, SDL2 2.32.10 MinGW SDK):

    x86_64-w64-mingw32-gcc -std=c99 -O2 -DUSE_SDL2 \
      -I SDL2-2.32.10/x86_64-w64-mingw32/include -I core -I platform \
      core/*.c platform/frontend.c platform/main_win.c -o crossover.exe \
      -L SDL2-2.32.10/x86_64-w64-mingw32/lib \
      -lmingw32 -lSDL2main -lSDL2 \
      -lgdi32 -luser32 -limm32 -lole32 -lwinmm -lsetupapi -lhid \
      -lcfgmgr32 -lversion -lshell32 -mwindows

SDL2 MinGW SDK: <https://libsdl.org/release/SDL2-devel-2.32.10-mingw.tar.gz>
Runtime DLLs needed when linking dynamically: `gdi32 user32 imm32 ole32 winmm
setupapi hid cfgmgr32 version shell32`.

The `USE_SDL2` sources compile clean under `-Wall -Wextra -Werror` against the
real SDL2 2.32.10 headers. The headless build needs no SDL2 at all (`make test`).

## Status / limits

- CPU cores, PPUs, machines, hub, frontend and input are **implemented and
  unit-tested** (see `make test`).
- The SNES PPU covers BG modes 0-7 including **Mode 7** affine, **offset-per-tile**
  (modes 2/3/4/6), **window masking** ($2123-$212B/$212E/$212F), **color math**
  ($2130-$2132: add/subtract, halve, fixed colour, clip modes), CGRAM palettes
  and OAM sprites.
- The SNES machine implements **general DMA** (modes 0-9), **HDMA** (direct +
  indirect, repeat/non-repeat), **vblank NMI**, and a **dot-based beam loop**
  (262 lines x 341 dots) that drives `$2137`/`$213C`/`$213D` beam counters,
  `$4212` HVBJOY and the H/V IRQ comparators (`$4209`/`$420A`).
- The 65816 core implements **all 256 opcodes**, verified by a decode test that
  checks every opcode against the documented operand count from the WDC matrix
  (stack-relative `($00,S)` and `($00,S),Y`, long indirect `[$00]`/`[$00],Y`,
  `PHK`/`PEA`/`PEI`/`PER`, `STZ` indexed, `JMP (abs,X)`, `JML [abs]`).
- The cartridge bus understands **LoROM and HiROM**, selected automatically from
  the internal header's map-mode byte (`$7FC0+$15` / `$FFC0+$15`, trusted only
  when the checksum and its complement agree), and strips a 512-byte `.smc`
  copier header when the file size says there is one.  **Cartridge SRAM** is
  mapped at `$20-$3F:$6000-$7FFF` (mirrored at `$A0-$BF`) with its size taken
  from the header.
- **Audio**: an SPC700 CPU core with the full instruction set, the 64KB ARAM and
  the IPL boot ROM, the `$2140-$2143` CPU ports (so the real boot upload
  handshake works), and a DSP with 8 voices, BRR sample decoding, ADSR/GAIN
  envelopes, per-voice pitch and the echo/FIR buffer.  Each frame yields ~534
  stereo frames at 32000 Hz, which the SDL2 build queues to the audio device.
- Still **not implemented**: ExHiROM, enhancement chips (Super FX, SA-1, DSP-1
  ...), cycle-exact memory-access penalties, and exact per-dot PPU rendering
  (the beam loop times interrupts and counters, but a line is drawn in one go).
  Reads of unmapped addresses return `$00` rather than true open bus.
- ROMs are loaded at runtime from your own files; no game content is included.
