@echo off
rem One-click PC build of Dragon Quest IX. Installs the free MSYS2 toolchain the first time
rem (C compiler, SDL2, Python), then builds dqix.exe into ..\build\pc. Safe to run again after updates.
setlocal
cd /d "%~dp0"
set MSYSROOT=C:\msys64
if not exist "%MSYSROOT%\usr\bin\bash.exe" (
  echo Installing MSYS2 ^(one time, a few minutes^)...
  winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements
)
if not exist "%MSYSROOT%\usr\bin\bash.exe" (
  echo MSYS2 was not installed. Get it from https://www.msys2.org ^(default folder C:\msys64^) and run this again.
  pause
  exit /b 1
)
echo Installing build tools ^(first time only^)...
"%MSYSROOT%\usr\bin\bash.exe" -lc "pacman -Sy --noconfirm --needed make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-yaml"
set MSYSTEM=UCRT64
set CHERE_INVOKING=1
"%MSYSROOT%\usr\bin\bash.exe" -lc "./build-windows.sh"
if errorlevel 1 (echo BUILD FAILED - the text above says why.) else (echo Build finished: ..\build\pc\play.bat)
pause
