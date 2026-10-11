crossover — Windows build (x86-64)
==================================

FILES
    crossover.exe       the emulator (174 KB, needs SDL2.dll next to it)
    SDL2.dll            SDL2 2.32.10 runtime — put it in the SAME folder
    SDL2-LICENSE.txt    zlib licence for SDL2
    crossover-source.tar.gz   the full C99 source tree

TO RUN
    1. Download crossover.exe and SDL2.dll.
    2. Put both files in the same folder.
    3. Double-click crossover.exe.
       A console window opens for diagnostics; the game window opens with it.

    crossover.exe --gb    game.gb    run a Game Boy ROM
    crossover.exe --snes  game.smc   run a SNES ROM (LoROM or HiROM, .smc OK)

    With no ROM it shows the shared hub world.  Esc quits.

CONTROLS
    Keyboard  Arrows=D-pad  Z/J=A  X/K=B  A=Y  S=X  Q=L  W=R
              Shift=Select  Enter=Start

    Game pad  Any SDL2 game controller, hot-plugged.  For a DualSense Edge:
              Cross=B  Circle=A  Triangle=X  Square=Y   (SNES diamond layout)
              D-pad or left/right stick = directions
              L1/R1 and L2/R2 = L/R shoulders
              Share/Mic = Select   Options/PS = Start

    The DualSense Edge (Sony VID $054C, PID $0DF2, USB and Bluetooth) is
    detected and named on connect.  Requires SDL2 >= 2.26.5 for the Edge;
    this build uses SDL2 2.32.10.

KNOWN LIMITS (see the source README for the full list)
    - The window follows SDL2 conventions; the build has not been
      run interactively against real hardware in this release.
    - SNES: no ExHiROM and no enhancement chips (Super FX, SA-1, DSP-1).
      Super Mario World boots to gameplay rendering (verified headlessly:
      APU handshake, Mode 1 BG layers, sprites); full interactive
      play-testing is still pending.
    - Audio and input paths are unit-tested headlessly (304 checks).

BUILDING IT YOURSELF
    See build_windows.bat in the source tree, or:

      x86_64-w64-mingw32-gcc -std=c99 -O2 -DUSE_SDL2 \
        -I SDL2-2.32.10/x86_64-w64-mingw32/include -I core -I platform \
        core/*.c platform/frontend.c platform/main_win.c -o crossover.exe \
        -L SDL2-2.32.10/x86_64-w64-mingw32/lib \
        -lmingw32 -lSDL2main -lSDL2 \
        -lgdi32 -luser32 -limm32 -lole32 -lwinmm -lsetupapi -lhid \
        -lcfgmgr32 -lversion -lshell32 -mwindows

    On Linux/macOS:  make test     (headless, no SDL2 needed)

    SDL2 MinGW SDK: https://libsdl.org/release/SDL2-devel-2.32.10-mingw.tar.gz
