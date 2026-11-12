@echo off
rem Builds d3d8.dll (32-bit) with Zig's bundled clang/MinGW toolchain.
rem Requires Zig (winget install zig.zig).
cd /d "%~dp0"
zig c++ -target x86-windows-gnu -std=c++17 -O2 -s -shared -fno-exceptions -fno-rtti -Wno-dll-attribute-on-redeclaration ^
  -o d3d8.dll ^
  src\dllmain.cpp src\config.cpp src\framelimiter.cpp src\fixedtick.cpp src\interp.cpp src\watchdog.cpp src\mousefix.cpp src\preload.cpp src\freecam.cpp src\quicksave.cpp src\phoneskip.cpp src\interior.cpp src\drawdist.cpp src\gameplay.cpp src\exitguard.cpp src\population.cpp ^
  -luser32 -lkernel32
if errorlevel 1 exit /b 1
del /q d3d8.pdb d3d8.lib dllmain.lib 2>nul
echo Built %~dp0d3d8.dll
