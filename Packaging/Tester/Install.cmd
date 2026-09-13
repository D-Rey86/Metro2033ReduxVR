@echo off
setlocal
title Metro2033ReduxVR Installer
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Metro2033ReduxVR.ps1"
exit /b %errorlevel%
