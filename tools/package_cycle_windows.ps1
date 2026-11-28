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
    [string]$OutputDirectory,
    [string]$ReadmeText,
    [string]$RedistDir,
    [string]$PackageRoot,
    [hashtable]$AdditionalFiles = @{},
    [scriptblock]$StageHook,
    [scriptblock]$ValidateStage,
    [string]$AuthoredModState,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release_windows_common.ps1')
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
$startupDiagnostics = $cache -match '(?m)^NESRECOMP_STARTUP_DIAGNOSTICS:BOOL=ON\r?$'
$bin = if (Test-Path -LiteralPath (Join-Path $build "Release/$Target.exe")) { Join-Path $build 'Release' } else { $build }
& (Join-Path $PSScriptRoot 'stage_windows_runtime.ps1') -BuildDir $build -Target $Target -RedistDir $RedistDir
foreach ($dependency in "$Target.exe",'SDL2.dll','assets') {
    if (-not (Test-Path -LiteralPath (Join-Path $bin $dependency))) { throw "Missing release dependency: $dependency" }
}
$out = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $ProjectRoot 'release' }
New-Item -ItemType Directory -Path $out -Force | Out-Null
$stage = Join-Path $out ('stage-cycle-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    $compiler = Cache-Path 'CMAKE_CXX_COMPILER' (Cache-Path 'CMAKE_C_COMPILER' '')
    $compilerBin = if ($compiler) { Split-Path -Parent $compiler } else { '' }
    $gnuRuntimes = @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')
    $gnu = $compilerBin -and (Test-Path -LiteralPath (Join-Path $compilerBin 'libstdc++-6.dll'))
    foreach ($file in "$Target.exe",'SDL2.dll','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll',
                     'msvcp140_1.dll','msvcp140_2.dll','concrt140.dll','vccorlib140.dll',
                     'msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll',
                     'libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll') {
        $source = Join-Path $bin $file
        if ($gnu -and $file -in $gnuRuntimes -and -not (Test-Path -LiteralPath $source)) {
            $source = Join-Path $compilerBin $file
            if (-not (Test-Path -LiteralPath $source)) { throw "Missing GNU runtime dependency: $file" }
        }
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $stage }
    }
    Copy-Item -LiteralPath (Join-Path $bin 'assets') -Destination (Join-Path $stage 'assets') -Recurse
    if ($startupDiagnostics) {
        Copy-Item -LiteralPath (Join-Path $ProjectRoot 'DIAGNOSTICS.md') -Destination $stage
        Copy-Item -LiteralPath (Join-Path $ProjectRoot 'tools/Collect-Diagnostics.cmd') -Destination $stage
        Copy-Item -LiteralPath (Join-Path $EngineRoot 'tools/Collect-Diagnostics.ps1') -Destination $stage
    }
    $preloaded = if ($PackageRoot) { (Resolve-Path -LiteralPath $PackageRoot).Path } else { Join-Path $ProjectRoot 'mods/preloaded/packages' }
    if (Test-Path -LiteralPath $preloaded) {
        New-Item -ItemType Directory -Path (Join-Path $stage 'mods') | Out-Null
        Copy-Item -LiteralPath $preloaded -Destination (Join-Path $stage 'mods/packages') -Recurse
    }
    # Framework packages the build staged beside the executable (nesrecomp.*,
    # e.g. the default-on replay capture: nesrecomp_cyc_stage_replay_package).
    $built = Join-Path $bin 'mods/packages'
    if (Test-Path -LiteralPath $built) {
        foreach ($package in Get-ChildItem -LiteralPath $built -Directory -Filter 'nesrecomp.*') {
            $destination = Join-Path $stage 'mods/packages'
            New-Item -ItemType Directory -Path $destination -Force | Out-Null
            if (-not (Test-Path -LiteralPath (Join-Path $destination $package.Name))) {
                Copy-Item -LiteralPath $package.FullName -Destination $destination -Recurse
            }
        }
    }
    $licenses = @{
        'NESRecomp-LICENSE.txt' = (Join-Path $EngineRoot 'LICENSE')
        'SDL2-COPYING.txt' = (Join-Path $EngineRoot 'runner/external/SDL2/COPYING.txt')
        'recomp-ui-LICENSE.txt' = (Join-Path $RecompUi 'LICENSE')
        'ImGui-LICENSE.txt' = (Join-Path $RecompUi 'src/third_party/imgui/LICENSE.txt')
    }
    # FetchContent can be overridden independently of the engine checkout.
    # Use the sources that configured this binary, including non-default RB roots.
    foreach ($dependency in @(
        @{ Cache = 'recomp_net_SOURCE_DIR'; Notice = 'recomp-net-LICENSE.txt' },
        @{ Cache = 'retcomm_rbengine_SOURCE_DIR'; Notice = 'retcomm-rbengine-LICENSE.txt' }
    )) {
        $sourceRoot = Cache-Path $dependency.Cache ''
        if ($sourceRoot) { $licenses[$dependency.Notice] = Join-Path $sourceRoot 'LICENSE' }
    }
    foreach ($provider in 'mod','persistence','input','diagnostics') {
        $providerRoot = Cache-Path 'NESRECOMP_SHARED_CORE_ROOT' (Join-Path $EngineRoot 'external/shared-cores')
        $notice = Join-Path $providerRoot "recomp-$provider-core/LICENSE"
        if (Test-Path -LiteralPath $notice) { $licenses["recomp-$provider-core-LICENSE.txt"] = $notice }
    }
    # These are the fonts in recomp-ui's runtime manifest. The notices include
    # their upstream copyright statements and full redistribution licenses.
    foreach ($font in @(
        @{ File = 'LatoLatin-Regular.ttf'; Notice = 'LatoLatin-OFL.txt' },
        @{ File = 'LatoLatin-Bold.ttf'; Notice = 'LatoLatin-OFL.txt' },
        @{ File = 'NotoSansSymbols2-Regular.ttf'; Notice = 'NotoSansSymbols2-OFL.txt' },
        @{ File = 'OpenMoji-black-glyf.ttf'; Notice = 'OpenMoji-LICENSE.txt' }
    )) {
        if (Test-Path -LiteralPath (Join-Path $stage "assets/fonts/$($font.File)")) {
            $licenses[$font.Notice] = Join-Path $PSScriptRoot "release-licenses/$($font.Notice)"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $stage 'libstdc++-6.dll')) {
        $compiler = Cache-Path 'CMAKE_CXX_COMPILER' ''
        if (-not $compiler) { throw 'Missing compiler identity for GNU runtime notices.' }
        $compilerPrefix = Split-Path -Parent (Split-Path -Parent $compiler)
        foreach ($name in 'COPYING.LIB', 'COPYING.RUNTIME', 'COPYING3', 'README') {
            $licenses["GCC-$name.txt"] = Join-Path $compilerPrefix "share/licenses/gcc-libs/$name"
        }
        $licenses['libwinpthread-COPYING.txt'] = Join-Path $compilerPrefix 'share/licenses/libwinpthread/COPYING'
        $sdlNotice = Join-Path $compilerPrefix 'share/licenses/SDL2/LICENSE.txt'
        if (Test-Path -LiteralPath $sdlNotice) { $licenses['SDL2-runtime-LICENSE.txt'] = $sdlNotice }
    }
    # Some local game projects do not yet carry a separate license file.
    # Bundle it when declared; the required framework/dependency notices stay.
    $gameLicense = Join-Path $ProjectRoot 'LICENSE'
    if (Test-Path -LiteralPath $gameLicense) { $licenses['Game-LICENSE.txt'] = $gameLicense }
    New-Item -ItemType Directory -Path (Join-Path $stage 'licenses') | Out-Null
    foreach ($name in $licenses.Keys) {
        if (-not (Test-Path -LiteralPath $licenses[$name])) { throw "Missing license: $($licenses[$name])" }
        Copy-Item -LiteralPath $licenses[$name] -Destination (Join-Path $stage "licenses/$name")
    }
    $readme = @"
$Title - cycle-accurate NESRecomp

No ROM is included. Select your matching original ROM at first launch.
Arrow keys: D-pad. Z: A. X: B. Enter: Start. Backslash: Select.
Escape: menu. Hold Tab: fast-forward. F7: save-state browser. F8: rewind.
Gamepads and remapping are supported; use Controls (config.ini).
Save states are specific to this build.
$GameNotes
"@
    if ($startupDiagnostics) {
        $readme += "`r`nStartup logs and crash dumps are automatic. After a failure, send the diagnostics folder beside the executable. See DIAGNOSTICS.md; Collect-Diagnostics.cmd also captures Windows loader failures.`r`n"
    }
    if ($ReadmeText) { $readme = $ReadmeText }
    [IO.File]::WriteAllText((Join-Path $stage 'README.txt'),$readme,[Text.Encoding]::UTF8)
    foreach ($relative in $AdditionalFiles.Keys) {
        $destination = [IO.Path]::GetFullPath((Join-Path $stage $relative))
        if (-not $destination.StartsWith([IO.Path]::GetFullPath($stage).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Additional file escapes staging.' }
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $AdditionalFiles[$relative] -Destination $destination -Recurse
    }
    if ($AuthoredModState) {
        New-Item -ItemType Directory -Path (Join-Path $stage 'mods') -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $stage 'mods/state.toml'),$AuthoredModState,[Text.Encoding]::ASCII)
    }
    if ($StageHook) { & $StageHook $stage $bin }
    if ($ValidateStage) { & $ValidateStage $stage @($licenses.Keys) }

    $forbidden = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
        $_.Extension -in '.nes','.fds','.rom','.sav','.srm','.state','.cycstate','.log','.dmp','.pdb' -or
        ($_.Name -in 'rom.cfg','config.ini','keybinds.ini','debug.ini','state.toml' -and
         -not ($AuthoredModState -and $_.FullName -eq (Join-Path $stage 'mods/state.toml') -and
                   (Get-Content -LiteralPath $_.FullName -Raw) -ceq $AuthoredModState))
    })
    if ($forbidden.Count) { throw 'Player or ROM data found in release staging.' }
    if (-not $ArchiveName) { $ArchiveName = "$Target-windows-x64.zip" }
    if ([IO.Path]::GetFileName($ArchiveName) -ne $ArchiveName) { throw 'ArchiveName must be a filename.' }
    $zip = Join-Path $out $ArchiveName
    New-VerifiedReleaseArchive $stage $zip
    Write-Output "Created $zip"
} finally {
    Remove-VerifiedReleaseStage $stage $out $ProjectRoot
}
