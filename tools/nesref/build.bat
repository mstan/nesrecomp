@echo off
REM Build nesref.exe from this directory.
REM
REM SDL2: reuses the copy already bundled with nesrecomp at
REM   ../../runner/external/SDL2  (include + lib/x64), so nesref builds in-tree
REM   with no extra download. SDL2.dll is copied next to the exe afterwards.
REM
REM At RUNTIME supply a libretro NES core DLL as argv[1] (e.g. nestopia_libretro.dll
REM or fceumm_libretro.dll). Cores are licensed separately and are NOT committed.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d %~dp0
set SDL=..\..\runner\external\SDL2
cl /nologo /EHsc /O2 /MD frontend.cpp /I "%SDL%\include" /Fe:nesref.exe ^
   /link /SUBSYSTEM:CONSOLE "%SDL%\lib\x64\SDL2.lib" shell32.lib user32.lib ws2_32.lib
echo BUILD_EXIT=%ERRORLEVEL%
if exist "%SDL%\lib\x64\SDL2.dll" copy /Y "%SDL%\lib\x64\SDL2.dll" SDL2.dll >nul
