@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish.ps1" %*
exit /b %ERRORLEVEL%
