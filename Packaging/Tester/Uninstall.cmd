@echo off
setlocal
title Metro2033ReduxVR Uninstaller
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-Metro2033ReduxVR.ps1"
exit /b %errorlevel%
