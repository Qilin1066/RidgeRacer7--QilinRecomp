@echo off
rem Analyse game: double-click. Builds the qilin tool (a minute or two the
rem first time) and reads your copy of Ridge Racer 7 with it. The results
rem go to analysis-out\ in this folder, which is never committed.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\analyse-game.ps1" %*
if errorlevel 1 (
    echo.
    echo It stopped. The message above says where; copy it for Claude.
)
pause
