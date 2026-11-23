@echo off
rem Play Ridge Racer 7: double-click. Starts the game; the first time (or if
rem it isn't built yet) it builds it first from your copy, which takes a
rem long while. (The game reaches its races but isn't finished:
rem docs/runtime.md has the state.)
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\build-ridgeracer7.ps1" -Play %*
if errorlevel 1 (
    echo.
    echo The build stopped. The message above says where; copy it for Claude.
    pause
)
