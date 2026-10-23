@echo off
rem Dragon Quest IX for PC: turns your own DQIX ROM into the PC game (the USA or European version).
rem Double-click this. The first time it installs the free MSYS2 build tools
rem (a few minutes, needs the internet); after that it only rebuilds.
rem The result goes in the "game" folder next to this file.
setlocal
cd /d "%~dp0"
title Dragon Quest IX PC - build

rem ---- the ROM: the first .nds in the "rom" folder, or ask for it
set "ROM="
for %%f in ("%~dp0rom\*.nds") do if not defined ROM set "ROM=%%~ff"
if defined ROM goto have_rom
rem the ROM chosen last time (updates build without asking again)
if exist "%~dp0rom\last-rom.txt" set /p ROM=<"%~dp0rom\last-rom.txt"
if defined ROM if exist "%ROM%" goto have_rom
set "ROM="
echo Choose your Dragon Quest IX ROM in the window that opens...
for /f "usebackq delims=" %%i in (`powershell -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; $d.Filter = 'DS ROM (*.nds)|*.nds'; $d.Title = 'Choose your Dragon Quest IX ROM'; if ($d.ShowDialog() -eq 'OK') { $d.FileName }"`) do set "ROM=%%i"
if defined ROM goto have_rom
echo No ROM chosen. Put your ROM in the "rom" folder (or choose it) and run this again.
if not defined DQIX_NOPAUSE pause
exit /b 1

:have_rom
echo ROM: "%ROM%"
if not exist "%~dp0rom" mkdir "%~dp0rom"
>"%~dp0rom\last-rom.txt" echo %ROM%

rem ---- the build tools (MSYS2)
set "MSYSROOT=C:\msys64"
if exist "%MSYSROOT%\usr\bin\bash.exe" goto have_msys
echo Installing MSYS2 (one time, a few minutes)...
winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements
if exist "%MSYSROOT%\usr\bin\bash.exe" goto have_msys
echo MSYS2 could not be installed automatically. Install it from https://www.msys2.org
echo (default folder C:\msys64), then run this again.
if not defined DQIX_NOPAUSE pause
exit /b 1

:have_msys
echo Checking the build tools (the first time downloads them)...
"%MSYSROOT%\usr\bin\bash.exe" -lc "pacman -Sy --noconfirm --needed make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-yaml"

rem ---- build
set MSYSTEM=UCRT64
set CHERE_INVOKING=1
set "DQIX_ROM=%ROM%"
"%MSYSROOT%\usr\bin\bash.exe" -lc "./tools/build-game.sh \"$(cygpath -u \"$DQIX_ROM\")\""
if errorlevel 1 goto failed
echo.
echo Finished. Play with game\dqix.exe (you can make a shortcut to it).
if not defined DQIX_NOPAUSE pause
exit /b 0

:failed
echo.
echo BUILD FAILED. The text above says why.
if not defined DQIX_NOPAUSE pause
exit /b 1
