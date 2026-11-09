@echo off
rem Starts two_games.ps1 from the install folder (double click).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Zwei Spiele testen.ps1"
if errorlevel 1 pause
