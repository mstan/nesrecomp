param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,

    [Parameter(Mandatory = $true)]
    [string]$Target,

    [switch]$FastValidation,

    [string]$BuildDirName = 'build-perf'
)

$ErrorActionPreference = 'Stop'

# This helper is intentionally conservative: title translation units are very
# large, and running more than one compiler job has made the validation host
# unstable. Child processes inherit this priority class.
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass =
    [System.Diagnostics.ProcessPriorityClass]::BelowNormal

$sourcePath = (Resolve-Path -LiteralPath $SourceDir).Path
$gitMarker = Join-Path $sourcePath '.git'
if (-not (Test-Path -LiteralPath $gitMarker -PathType Leaf)) {
    throw "Refusing to build a normal checkout; SourceDir must be a linked git worktree: $sourcePath"
}

$cmake = 'C:\msys64\mingw64\bin\cmake.exe'
$ninja = 'C:\msys64\mingw64\bin\ninja.exe'
$ccache = 'C:\msys64\mingw64\bin\ccache.exe'
$cCompiler = 'C:\msys64\mingw64\bin\gcc.exe'
$cxxCompiler = 'C:\msys64\mingw64\bin\g++.exe'
$buildDir = Join-Path $sourcePath $BuildDirName
$logPath = Join-Path $sourcePath 'perf-build.log'
$statusPath = Join-Path $sourcePath 'perf-build.status'

$env:CCACHE_MAXSIZE = '20G'

$profile = if ($FastValidation) { 'fast-validation' } else { 'release' }
"CONFIGURING $Target ($profile)" | Set-Content -LiteralPath $statusPath
$configureArgs = @(
    '-S', $sourcePath,
    '-B', $buildDir,
    '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_C_COMPILER=$cCompiler",
    "-DCMAKE_CXX_COMPILER=$cxxCompiler",
    "-DCMAKE_C_COMPILER_LAUNCHER=$ccache",
    "-DCMAKE_CXX_COMPILER_LAUNCHER=$ccache",
    '-DCMAKE_EXE_LINKER_FLAGS=',
    '-DNESRECOMP_ENABLE_TRACE=OFF',
    '-DNESRECOMP_ENABLE_STACK_TRACKING=OFF',
    '-DNESRECOMP_ENABLE_POSTMORTEM_RINGS=OFF'
)
if ($FastValidation) {
    # Public-title regression checks exercise deterministic behavior rather
    # than benchmark throughput. Avoid spending tens of minutes optimizing
    # each title's enormous generated guest TU; production -O3 is already
    # covered by the engine suite and representative title builds.
    $configureArgs += '-DCMAKE_C_FLAGS_RELEASE=-O0 -DNDEBUG'
    $configureArgs += '-DCMAKE_CXX_FLAGS_RELEASE=-O0 -DNDEBUG'
}

$ErrorActionPreference = 'Continue'
& $cmake @configureArgs `
    2>&1 | Tee-Object -LiteralPath $logPath
$configureExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($configureExit -ne 0) {
    "CONFIGURE_FAILED $configureExit" | Set-Content -LiteralPath $statusPath
    exit $configureExit
}

"BUILDING $Target ($profile)" | Set-Content -LiteralPath $statusPath
$ErrorActionPreference = 'Continue'
& $cmake --build $buildDir --target $Target -- -j1 `
    2>&1 | Tee-Object -FilePath $logPath -Append
$buildExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($buildExit -ne 0) {
    "BUILD_FAILED $buildExit" | Set-Content -LiteralPath $statusPath
    exit $buildExit
}

$exePath = Join-Path $buildDir ($Target + '.exe')
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    "MISSING_EXE $exePath" | Set-Content -LiteralPath $statusPath
    exit 3
}

"PASS $exePath" | Set-Content -LiteralPath $statusPath
