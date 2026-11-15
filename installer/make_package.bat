@echo off
rem IN2: double-click to make the installer package from the latest build.
rem Passes any arguments on, e.g.  make_package.bat -NoReShade
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make_package.ps1" %*
pause
