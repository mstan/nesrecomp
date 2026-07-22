@echo off
REM Build nesref.exe from this directory.
REM
REM SDL2: reuses the copy already bundled with nesrecomp at
REM   ../../runner/external/SDL2  (include + lib/x64), so nesref builds in-tree
REM   with no extra download. SDL2.dll is copied next to the exe afterwards.
REM
REM At RUNTIME supply a libretro NES core DLL as argv[1] (e.g. nestopia_libretro.dll
REM or fceumm_libretro.dll). Cores are licensed separately and are NOT committed.
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo Visual Studio 2022 C++ build tools not found.
    exit /b 1
)
call "%VCVARS%"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d %~dp0
set SDL=..\..\runner\external\SDL2
cl /nologo /EHsc /O2 /MD frontend.cpp /I "%SDL%\include" /I "..\..\runner\src" /Fe:nesref.exe ^
   /link /SUBSYSTEM:CONSOLE "%SDL%\lib\x64\SDL2.lib" shell32.lib user32.lib ws2_32.lib
if errorlevel 1 exit /b %ERRORLEVEL%
if exist "%SDL%\lib\x64\SDL2.dll" copy /Y "%SDL%\lib\x64\SDL2.dll" SDL2.dll >nul
echo nesref.exe built successfully.
