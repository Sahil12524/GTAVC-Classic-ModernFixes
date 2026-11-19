@echo off
rem VC Mod Manager: optional enhancement mods for VC Modern Fix (see VCModManager.ps1).
rem Put this file and VCModManager.ps1 next to gta-vc.exe and double-click it.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0VCModManager.ps1" %*
if "%~1"=="" pause
