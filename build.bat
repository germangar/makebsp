@echo off
:: This batch file launches the PowerShell script so you can just double-click it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" ::-release
pause