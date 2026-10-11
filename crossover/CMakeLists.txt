cmake_minimum_required(VERSION 3.10)
project(crossover C)
set(CMAKE_C_STANDARD 99)

# ---- engine core (portable C99) ----
add_library(core STATIC
    core/lr35902.c core/w65816.c
    core/ppu_gb.c core/ppu_snes.c
    core/gb.c core/snes.c
    core/spc700.c core/apu.c
    core/hub.c)
target_include_directories(core PUBLIC core)

# ---- test runners (headless, no SDL needed) ----
foreach(t test_cpu test_65816 test_ppu test_gb test_hub test_snes_ppu test_snes test_spc700 test_apu test_frontend)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} core)
endforeach()

enable_testing()
add_test(NAME cpu         COMMAND test_cpu)
add_test(NAME cpu65816    COMMAND test_65816)
add_test(NAME gb_ppu      COMMAND test_ppu)
add_test(NAME gb_machine  COMMAND test_gb)
add_test(NAME hub         COMMAND test_hub)
add_test(NAME snes_ppu    COMMAND test_snes_ppu)
add_test(NAME snes_machine COMMAND test_snes)
add_test(NAME spc700      COMMAND test_spc700)
add_test(NAME snes_apu    COMMAND test_apu)
add_test(NAME frontend    COMMAND test_frontend)

# ---- Windows GUI app ----
add_executable(crossover platform/frontend.c platform/main_win.c)
target_link_libraries(crossover core)
if(WIN32 AND USE_SDL2)
    target_compile_definitions(crossover PRIVATE USE_SDL2)
    find_package(SDL2 REQUIRED)
    target_link_libraries(crossover SDL2::SDL2 SDL2::SDL2main)
endif()
