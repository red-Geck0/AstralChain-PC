@echo off
rem Test build only: switches between the builds with and without function-lifter. Close the game and launcher first.
cd /d "%~dp0"
".venv\Scripts\python.exe" switch_build.py
pause
