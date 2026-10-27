@echo off
rem Double-click this to update an installed Dragon Quest IX with the files in this folder (a newer version you unzipped).
rem It finds the installed game (or asks where it is), copies the new files over it, builds again, and keeps your saves and settings.
cd /d "%~dp0"
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0tools\setup-wizard.ps1" -Update
