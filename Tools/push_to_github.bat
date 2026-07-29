@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0setup_and_push_github.ps1" %*
pause
