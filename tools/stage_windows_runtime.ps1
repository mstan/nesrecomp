param([Parameter(Mandatory)][string]$BuildDir, [Parameter(Mandatory)][string]$Target, [string]$RedistDir)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$bin = if (Test-Path -LiteralPath (Join-Path $build "Release/$Target.exe")) { Join-Path $build 'Release' } else { $build }
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
# Other toolchains carry their own runtimes. An explicit redist path opts in.
$msvc = $cache -match '(?im)^CMAKE_(?:C|CXX)_COMPILER:[^=]+=.*[/\\]cl\.exe\r?$' -or
        $cache -match '(?m)^CMAKE_GENERATOR:INTERNAL=Visual Studio'
$compilerFiles = @()
$cmakeFiles = Join-Path $build 'CMakeFiles'
if (Test-Path -LiteralPath $cmakeFiles) {
    foreach ($version in @(Get-ChildItem -LiteralPath $cmakeFiles -Directory)) {
        $compilerFile = Join-Path $version.FullName 'CMakeCXXCompiler.cmake'
        if (Test-Path -LiteralPath $compilerFile) {
            $compilerFiles += $compilerFile
            if ((Get-Content -LiteralPath $compilerFile -Raw) -match 'set\(CMAKE_CXX_(?:COMPILER_ID|SIMULATE_ID) "MSVC"\)') { $msvc = $true }
        }
    }
}
if (-not $RedistDir -and -not $msvc) { return }
$required = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
if (-not $RedistDir -and @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $bin $_)) }).Count -eq 0) { return }

# Read the configured compiler's install directory; never crawl a drive or
# copy runtime DLLs from System32. Accept an explicit licensed redist directory.
$candidates = @()
if ($RedistDir) { $candidates += $RedistDir }
if ($env:VCToolsRedistDir) { $candidates += Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT' }
foreach ($compilerFile in $compilerFiles) {
    $text = Get-Content -LiteralPath $compilerFile -Raw
    $match = [regex]::Match($text, 'set\(CMAKE_CXX_COMPILER "(.+?/VC)/Tools/MSVC/')
    if (-not $match.Success) { continue }
    $redistRoot = Join-Path $match.Groups[1].Value 'Redist/MSVC'
    if (-not (Test-Path -LiteralPath $redistRoot)) { continue }
    foreach ($redist in @(Get-ChildItem -LiteralPath $redistRoot -Directory | Sort-Object Name -Descending)) {
        $candidates += Join-Path $redist.FullName 'x64/Microsoft.VC143.CRT'
    }
}
foreach ($candidate in $candidates) {
    $missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $candidate $_)) })
    if ($missing.Count) { continue }
    foreach ($dll in $required) { Copy-Item -LiteralPath (Join-Path $candidate $dll) -Destination (Join-Path $bin $dll) }
    Write-Output "Staged x64 VC runtimes from $candidate"
    return
}
throw 'Missing licensed x64 Visual C++ redistributables. Supply -RedistDir with Microsoft.VC143.CRT.'
