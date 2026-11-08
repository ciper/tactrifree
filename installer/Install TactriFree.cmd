@echo off
rem Install TactriFree: starts TactriFree-Setup.ps1 in 64-bit PowerShell as Administrator (Windows asks for permission).
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%PS%" -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -FilePath '%PS%' -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0TactriFree-Setup.ps1\"'"
