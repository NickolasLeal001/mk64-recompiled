@echo off
rem Configures and builds with the Visual Studio 2026 toolchain (MSVC + bundled CMake/Ninja).
rem With MK64_ROM set to the user's ROM, first regenerates analysis/functions.json and the
rem recompiled C in generated\ (never committed: it is derived from the ROM).
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (
  echo ERRO: Visual Studio com C++ ^(MSVC x64^) nao encontrado.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0"
if defined MK64_ROM (
  python -I tools\recomp\analyze.py "%MK64_ROM%" || exit /b 1
  python -I tools\recomp\recomp.py "%MK64_ROM%" generated || exit /b 1
)
if not exist build\build.ninja (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
)
cmake --build build || exit /b 1
