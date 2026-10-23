@echo off
rem Double-click this to install Dragon Quest IX for PC: a window walks you through it.
rem (Same as BUILD.bat, with a window, shortcuts and an uninstaller.)
cd /d "%~dp0"
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0tools\setup-wizard.ps1"
