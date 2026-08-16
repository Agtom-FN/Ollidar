@echo off
title LidarScan equipment test
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\menu.ps1"
if errorlevel 1 (
  echo.
  echo The test program could not start.
  echo Take a photo of this window and send it to us.
  echo.
  pause
)
