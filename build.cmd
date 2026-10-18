@echo off
rem Run in the x64 Native Tools Command Prompt for Visual Studio 2022 or newer.
rem The Desktop development with C++ workload and a Windows SDK are required.
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
 echo Open the x64 Native Tools Command Prompt, then run build.cmd.
 exit /b 1
)
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT trainer.cpp user32.lib gdi32.lib /Fe:AtomicHeartAmmo_v6.exe /link /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1
echo Build completed: AtomicHeartAmmo_v6.exe
