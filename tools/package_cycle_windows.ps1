# Package a cycle game without player data or ROMs. No publishing.
param(
    [Parameter(Mandatory)][string]$ProjectRoot,
    [Parameter(Mandatory)][string]$Target,
    [Parameter(Mandatory)][string]$Title,
    [string]$BuildDir = 'build_release',
    [string]$ArchiveName,
    [string]$Rom,
    [string]$EngineRoot,
    [string]$RecompUi,
    [string[]]$CMakeArgs = @(),
    [string]$GameNotes = '',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $ProjectRoot $BuildDir }
if (-not $SkipBuild) {
    if (-not $Rom) { throw 'Supply -Rom with the original ROM path.' }
    & (Join-Path $PSScriptRoot 'build_cycle_windows.ps1') -ProjectRoot $ProjectRoot -Rom $Rom -BuildDir $build -EngineRoot $EngineRoot -RecompUi $RecompUi -CMakeArgs $CMakeArgs
}
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
if ($cache -notmatch '(?m)^NESRECOMP_BACKEND:STRING=cycle\r?$') { throw 'Refusing to package a legacy build.' }
if ($cache -notmatch '(?m)^NESRECOMP_ENABLE_TRACE:BOOL=OFF\r?$') { throw 'Refusing to package a diagnostic build.' }
function Cache-Path([string]$Name, [string]$Fallback) {
    $match = [regex]::Match($cache, '(?m)^'+[regex]::Escape($Name)+':[^=]+=(.+)\r?$')
    if ($match.Success) { return $match.Groups[1].Value.TrimEnd("`r") }
    return $Fallback
}
$EngineRoot = Cache-Path 'NESRECOMP_ROOT' (Join-Path $ProjectRoot 'nesrecomp')
$RecompUi = Cache-Path 'NESRECOMP_RECOMP_UI' (Join-Path $ProjectRoot 'recomp-ui')
$bin = if (Test-Path -LiteralPath (Join-Path $build "Release/$Target.exe")) { Join-Path $build 'Release' } else { $build }
foreach ($dependency in "$Target.exe",'SDL2.dll','assets') {
    if (-not (Test-Path -LiteralPath (Join-Path $bin $dependency))) { throw "Missing release dependency: $dependency" }
}
$out = Join-Path $ProjectRoot 'release'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$stage = Join-Path $out ('stage-cycle-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    foreach ($file in "$Target.exe",'SDL2.dll','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll') {
        $source = Join-Path $bin $file
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $stage }
    }
    Copy-Item -LiteralPath (Join-Path $bin 'assets') -Destination (Join-Path $stage 'assets') -Recurse
    $preloaded = Join-Path $ProjectRoot 'mods/preloaded/packages'
    if (Test-Path -LiteralPath $preloaded) {
        New-Item -ItemType Directory -Path (Join-Path $stage 'mods') | Out-Null
        Copy-Item -LiteralPath $preloaded -Destination (Join-Path $stage 'mods/packages') -Recurse
    }
    $licenses = @{
        'Game-LICENSE.txt' = (Join-Path $ProjectRoot 'LICENSE')
        'NESRecomp-LICENSE.txt' = (Join-Path $EngineRoot 'LICENSE')
        'SDL2-COPYING.txt' = (Join-Path $EngineRoot 'runner/external/SDL2/COPYING.txt')
        'ImGui-LICENSE.txt' = (Join-Path $RecompUi 'src/third_party/imgui/LICENSE.txt')
    }
    New-Item -ItemType Directory -Path (Join-Path $stage 'licenses') | Out-Null
    foreach ($name in $licenses.Keys) {
        if (-not (Test-Path -LiteralPath $licenses[$name])) { throw "Missing license: $($licenses[$name])" }
        Copy-Item -LiteralPath $licenses[$name] -Destination (Join-Path $stage "licenses/$name")
    }
    $readme = @"
$Title - cycle-accurate NESRecomp

No ROM is included. Select your matching original ROM at first launch.
Arrow keys: D-pad. Z: A. X: B. Enter: Start. Backslash: Select.
Escape: menu. Hold Tab: fast-forward. F8/F9: save/load cycle state.
Gamepads and remapping are supported; use Controls (config.ini).
Legacy binary save states require the retained legacy backend.
$GameNotes
"@
    [IO.File]::WriteAllText((Join-Path $stage 'README.txt'),$readme,[Text.Encoding]::UTF8)
    $forbidden = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
        $_.Extension -in '.nes','.fds','.sav','.srm','.state','.cycstate','.log' -or
        $_.Name -in 'rom.cfg','config.ini','keybinds.ini','debug.ini','state.toml'
    })
    if ($forbidden.Count) { throw 'Player or ROM data found in release staging.' }
    if (-not $ArchiveName) { $ArchiveName = "$Target-windows-x64.zip" }
    if ([IO.Path]::GetFileName($ArchiveName) -ne $ArchiveName) { throw 'ArchiveName must be a filename.' }
    $zip = Join-Path $out $ArchiveName
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    Write-Output "Created $zip"
} finally {
    $absolute = [IO.Path]::GetFullPath($stage)
    $prefix = [IO.Path]::GetFullPath($out).TrimEnd('\')+'\'
    if (-not $absolute.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe staging cleanup path.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
