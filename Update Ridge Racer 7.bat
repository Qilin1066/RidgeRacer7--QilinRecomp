@echo off
rem Update Ridge Racer 7: double-click. Gets the latest code from GitHub,
rem rebuilds what changed from your copy, and starts the game.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\build-ridgeracer7.ps1" -Pull -Run
if errorlevel 1 (
    echo.
    echo The update stopped. The message above says where; copy it for Claude.
    pause
)
