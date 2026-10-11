@echo off
rem Build the crossover for Windows with MinGW-w64 (gcc).
rem   Requires: mingw-w64 (gcc) on PATH.
rem   SDL2:     add -DUSE_SDL2 and point SDL2_INC/SDL2_LIB at your SDL2 install.
rem   Produces: crossover.exe
setlocal
set CORE=core\lr35902.c core\w65816.c core\ppu_gb.c core\ppu_snes.c core\gb.c core\snes.c core\hub.c core\spc700.c core\apu.c
set PLATFORM=platform\frontend.c platform\main_win.c
set CFLAGS=-std=c99 -Wall -Wextra -O2 -mwindows

rem --- uncomment the next two lines for the real windowed SDL2 build ---
rem set SDL2_INC=-I C:\SDL2\include
rem set SDL2_LIB=-L C:\SDL2\lib -lmingw32 -lSDL2main -lSDL2

gcc %CFLAGS% %SDL2_INC% -o crossover.exe %CORE% %PLATFORM% %SDL2_LIB%
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
echo Built crossover.exe
endlocal
