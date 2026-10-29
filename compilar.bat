@echo off
rem Builds the port from the user's ROM: compilar.bat <rom> [--saida PASTA] [--sem-rom] [--sem-teste]
rem (see tools\compilar.py). The result goes to dist\ by default.
if "%~1"=="" (
  echo Uso: compilar.bat caminho\da\rom.z64 [--saida PASTA] [--sem-rom] [--sem-teste]
  exit /b 1
)
where python >nul 2>nul || (
  echo ERRO: Python 3 nao encontrado no PATH.
  exit /b 1
)
python -I "%~dp0tools\compilar.py" %*
