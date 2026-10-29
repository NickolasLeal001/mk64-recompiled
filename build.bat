@echo off
rem Configures and builds with the Visual Studio 2026 toolchain (MSVC + bundled CMake/Ninja).
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0"
if not exist build\build.ninja (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
)
cmake --build build || exit /b 1
