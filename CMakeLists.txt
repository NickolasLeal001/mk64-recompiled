cmake_minimum_required(VERSION 3.24)
project(mk64_port C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.16
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

add_library(rom STATIC src/rom.c src/sha1.c)
target_include_directories(rom PUBLIC src)

# Recompiled game code. generated/ is produced from the user's ROM by tools/recomp (see build.bat)
# and is never committed.
file(GLOB RECOMP_GENERATED CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/*.c)
if(RECOMP_GENERATED)
  add_library(recomp_game STATIC ${RECOMP_GENERATED} runtime/recomp_ops.c runtime/recomp_lookup.c)
  target_include_directories(recomp_game PUBLIC runtime generated)
  if(MSVC)
    # Generated code: unreferenced labels (C4102) and constant conditions are expected.
    target_compile_options(recomp_game PRIVATE /bigobj /W3 /wd4102 /wd4127 /wd4146)
  endif()
  add_executable(recomp_link_check tests/recomp_link_check.c runtime/recomp_stubs.c)
  target_link_libraries(recomp_link_check PRIVATE recomp_game)
  add_executable(recomp_diff tests/recomp_diff.c tests/interp.c)
  target_link_libraries(recomp_diff PRIVATE recomp_game rom)
  if(MSVC)
    # C4611: setjmp is intentional; C4702: code after the longjmp-ing hooks is unreachable by design.
    target_compile_options(recomp_diff PRIVATE /W4 /wd4611 /wd4702)
  endif()
endif()

add_executable(mk64 src/main.c)
target_link_libraries(mk64 PRIVATE rom SDL3::SDL3-static)
if(MSVC)
  target_compile_options(rom PRIVATE /W4)
  target_compile_options(mk64 PRIVATE /W4)
endif()

# Tests run without SDL so they work headless.
enable_testing()
add_executable(test_rom tests/test_rom.c)
target_link_libraries(test_rom PRIVATE rom)
add_test(NAME sha1_vectors COMMAND test_rom)
