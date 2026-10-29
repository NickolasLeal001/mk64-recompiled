cmake_minimum_required(VERSION 3.24)
project(mk64_port C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(MSVC)
  # Portable C (fopen, strncat...) instead of the MSVC-only *_s variants.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

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
  file(GLOB NATIVE_REWRITES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/native/*.c)
  add_library(recomp_game STATIC ${RECOMP_GENERATED} ${NATIVE_REWRITES} runtime/recomp_ops.c runtime/recomp_lookup.c)
  target_include_directories(recomp_game PUBLIC runtime generated)
  if(MSVC)
    # Generated code: unreferenced labels (C4102) and constant conditions are expected.
    target_compile_options(recomp_game PRIVATE /bigobj /W3 /wd4102 /wd4127 /wd4146)
  endif()
  # The game: recompiled code + native libultra runtime + SDL host.
  add_executable(mk64 src/main.c src/input.c src/config.c runtime/ultra.c runtime/io.c gfx/rsp.c gfx/rdp.c audio/abi.c)
  target_include_directories(mk64 PRIVATE runtime generated src gfx audio)
  target_link_libraries(mk64 PRIVATE recomp_game rom SDL3::SDL3-static)
  if(MSVC)
    # /Ob2: RelWithDebInfo defaults to /Ob1, which leaves the renderer's per-pixel helpers
    # (static, not marked inline) as real calls.
    target_compile_options(mk64 PRIVATE /W4 /Ob2)
  endif()

  add_executable(recomp_link_check tests/recomp_link_check.c runtime/recomp_stubs.c)
  target_link_libraries(recomp_link_check PRIVATE recomp_game)
  add_executable(behavior_test tests/behavior_test.c)
  target_link_libraries(behavior_test PRIVATE recomp_game rom)

  add_executable(recomp_diff tests/recomp_diff.c tests/interp.c)
  target_link_libraries(recomp_diff PRIVATE recomp_game rom)
  if(MSVC)
    # C4611: setjmp is intentional; C4702: code after the longjmp-ing hooks is unreachable by design.
    target_compile_options(recomp_diff PRIVATE /W4 /wd4611 /wd4702)
  endif()
endif()

if(MSVC)
  target_compile_options(rom PRIVATE /W4)
endif()

# Tests run without SDL so they work headless.
enable_testing()
add_executable(test_rom tests/test_rom.c)
target_link_libraries(test_rom PRIVATE rom)
add_test(NAME sha1_vectors COMMAND test_rom)
