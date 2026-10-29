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
