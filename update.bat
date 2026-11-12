@echo off
setlocal
rem Runs the Lumenveil update script. Double-click this file in the extracted release folder.
if not exist "%~dp0scripts\setup.ps1" (
    echo Extract the whole zip to a folder first, then run this file again.
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup.ps1" -Action update %*
set "code=%errorlevel%"
pause
exit /b %code%
