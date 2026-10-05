# Shared cycle build for game repositories. Does not launch the game.
param(
    [Parameter(Mandatory)][string]$ProjectRoot,
    [Parameter(Mandatory)][string]$Rom,
    [string]$BuildDir = 'build_release',
    [string]$EngineRoot,
    [string]$RecompUi,
    [string[]]$CMakeArgs = @()
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$Rom = (Resolve-Path -LiteralPath $Rom).Path
if (-not $EngineRoot) { $EngineRoot = Split-Path -Parent $PSScriptRoot }
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $ProjectRoot $BuildDir }
$cmake = Join-Path $env:ProgramFiles 'CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake was not found at $cmake" }
function Invoke-CycleBuild([string[]]$ToolArgs) {
    $quoted = foreach ($value in $ToolArgs) {
        '"' + [regex]::Replace([regex]::Replace($value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $cmake
    $info.Arguments = $quoted -join ' '
    $info.WorkingDirectory = $ProjectRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        $null = $process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        Write-Output $stdout.GetAwaiter().GetResult()
        Write-Output $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode) { throw "CMake failed ($($process.ExitCode))" }
    } finally { $process.Dispose() }
}
$configure = @('-S',$ProjectRoot,'-B',$build,'-G','Visual Studio 17 2022','-A','x64',
    "-DNESRECOMP_ROOT=$EngineRoot",'-DNESRECOMP_BACKEND=cycle',"-DNESRECOMP_ROM=$Rom",'-DNESRECOMP_ENABLE_TRACE:BOOL=OFF')
if ($RecompUi) { $configure += "-DNESRECOMP_RECOMP_UI=$RecompUi" }
Invoke-CycleBuild ($configure + $CMakeArgs)
Invoke-CycleBuild @('--build',$build,'--config','Release','--parallel','4')
