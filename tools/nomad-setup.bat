@echo off
rem nomad-setup launcher. Run this from an Administrator prompt for the
rem SD card step - partitioning a disk needs elevation.
setlocal
set "PYTHONPATH=%~dp0;%PYTHONPATH%"
where python >nul 2>nul || (
  echo Python 3 not found. Install it from https://python.org and try again.
  exit /b 1
)
python -m nomad_setup %*
