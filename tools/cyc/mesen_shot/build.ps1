<#
    build.ps1 - build mesen_shot against a MesenCE checkout (Windows, MSVC).

        tools\cyc\mesen_shot\build.ps1 <MesenCE checkout> [out-dir]

    Builds MesenCE's native static libraries first (Core, Utilities, SevenZip,
    Lua, Windows) via the InteropDLL project, then compiles mesen_shot.cpp and
    links it against them. The .NET UI is never built.
#>
param(
    [Parameter(Mandatory = $true)][string]$MesenRoot,
    [string]$OutDir = "build/mesen_shot"
)

$ErrorActionPreference = "Stop"
$MesenRoot = (Resolve-Path $MesenRoot).Path
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found; install Visual Studio Build Tools" }
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vs) { throw "no Visual Studio installation with MSBuild found" }

Write-Host "==> building MesenCE native libraries"
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$MesenRoot\Mesen.sln" `
    /t:InteropDLL /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "MesenCE build failed" }

$libDir = Join-Path $MesenRoot "bin\win-x64\Release"
if (-not (Test-Path (Join-Path $libDir "Core.lib"))) { throw "Core.lib not found in $libDir" }

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$OutDir = (Resolve-Path $OutDir).Path

# Import the MSVC environment once, then compile and link in it.
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmd = @"
call "$vcvars" >nul
cl /nologo /std:c++17 /EHsc /O2 /MT /DNDEBUG /DWIN32 /D_CRT_SECURE_NO_WARNINGS ^
   /I"$MesenRoot" /I"$MesenRoot\Core" ^
   "$here\mesen_shot.cpp" ^
   /Fo"$OutDir\\" /Fe"$OutDir\mesen_shot.exe" ^
   /link /LIBPATH:"$libDir" Core.lib Utilities.lib SevenZip.lib Lua.lib Windows.lib ^
   dinput8.lib Xinput9_1_0.lib d3d11.lib d3dcompiler.lib dxguid.lib winmm.lib comctl32.lib ^
   kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ^
   ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib ws2_32.lib
"@
$bat = Join-Path $env:TEMP "mesen_shot_build.bat"
$log = Join-Path $env:TEMP "mesen_shot_build.log"
Set-Content -Path $bat -Value $cmd -Encoding ascii
Write-Host "==> compiling mesen_shot"
# vcvars64.bat prints a harmless 'vswhere.exe is not recognized' line on some
# Build Tools installs; keep its noise in the log and judge by the exit code.
& cmd.exe /c "`"$bat`" > `"$log`" 2>&1"
if ($LASTEXITCODE -ne 0) {
    Get-Content $log | Select-Object -Last 25
    throw "mesen_shot build failed (full log: $log)"
}
Write-Host "==> $OutDir\mesen_shot.exe"
