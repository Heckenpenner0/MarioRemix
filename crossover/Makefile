# Host Makefile (Linux/macOS/MinGW) - builds + runs the full core test suite.
CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
CORE     = core/lr35902.c core/w65816.c core/hub.c core/ppu_gb.c core/ppu_snes.c core/gb.c core/snes.c core/spc700.c core/apu.c
TESTS    = tests/test_cpu tests/test_65816 tests/test_ppu tests/test_gb tests/test_hub tests/test_snes_ppu tests/test_snes tests/test_spc700 tests/test_apu tests/test_frontend

.PHONY: all test clean
all: $(TESTS)

tests/test_cpu: core/lr35902.c tests/test_cpu.c core/lr35902.h
	$(CC) $(CFLAGS) -o $@ core/lr35902.c tests/test_cpu.c

tests/test_65816: core/w65816.c tests/test_65816.c core/w65816.h
	$(CC) $(CFLAGS) -o $@ core/w65816.c tests/test_65816.c

tests/test_ppu: core/ppu_gb.c tests/test_ppu.c core/ppu_gb.h
	$(CC) $(CFLAGS) -o $@ core/ppu_gb.c tests/test_ppu.c

tests/test_gb: core/lr35902.c core/ppu_gb.c core/gb.c tests/test_gb.c core/gb.h
	$(CC) $(CFLAGS) -o $@ core/lr35902.c core/ppu_gb.c core/gb.c tests/test_gb.c
	$(CC) $(CFLAGS) -o $@ core/ppu_gb.c tests/test_ppu.c

tests/test_hub: $(CORE) tests/test_hub.c core/hub.h
	$(CC) $(CFLAGS) -o $@ $(CORE) tests/test_hub.c

tests/test_snes_ppu: core/ppu_snes.c tests/test_snes_ppu.c core/ppu_snes.h
	$(CC) $(CFLAGS) -o $@ core/ppu_snes.c tests/test_snes_ppu.c

tests/test_snes: core/w65816.c core/ppu_snes.c core/snes.c core/spc700.c core/apu.c tests/test_snes.c core/snes.h
	$(CC) $(CFLAGS) -o $@ core/w65816.c core/ppu_snes.c core/snes.c core/spc700.c core/apu.c tests/test_snes.c

tests/test_spc700: core/spc700.c tests/test_spc700.c core/spc700.h
	$(CC) $(CFLAGS) -o $@ core/spc700.c tests/test_spc700.c

tests/test_apu: core/spc700.c core/apu.c tests/test_apu.c core/apu.h
	$(CC) $(CFLAGS) -o $@ core/spc700.c core/apu.c tests/test_apu.c

tests/test_frontend: $(CORE) platform/frontend.c tests/test_frontend.c platform/frontend.h
	$(CC) $(CFLAGS) -I core -I platform -o $@ $(CORE) platform/frontend.c tests/test_frontend.c

test: $(TESTS)
	@echo "== LR35902 (GB) =="  && ./tests/test_cpu
	@echo "== 65816 (SNES) =="  && ./tests/test_65816
	@echo "== GB PPU =="         && ./tests/test_ppu
	@echo "== GB machine =="    && ./tests/test_gb
	@echo "== hub crossover ==" && ./tests/test_hub
	@echo "== SNES PPU =="      && ./tests/test_snes_ppu
	@echo "== SNES machine =="  && ./tests/test_snes
	@echo "== SPC700 (APU CPU) ==" && ./tests/test_spc700
	@echo "== SNES APU =="      && ./tests/test_apu
	@echo "== frontend =="      && ./tests/test_frontend

clean:
	rm -f $(TESTS)
