@echo off
rem Build directo con MSVC (correr desde "x64 Native Tools Command Prompt")
cl /nologo /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   main.cpp audio.cpp tray.cpp icons.cpp autostart.cpp ^
   /Fe:MuteMic.exe /link /SUBSYSTEM:WINDOWS ^
   ole32.lib shell32.lib user32.lib gdi32.lib advapi32.lib
if %errorlevel% neq 0 (echo BUILD FAILED & exit /b 1)
echo OK: MuteMic.exe
