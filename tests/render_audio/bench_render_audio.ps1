param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$OutDir = (Join-Path $env:TEMP 'nesrecomp_render_audio_bench'),
    [string]$BaselineRef = 'HEAD',
    [string]$GitExe = 'C:\Program Files\Git\cmd\git.exe',
    [string]$GccExe = 'C:\msys64\mingw64\bin\gcc.exe'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

Push-Location -LiteralPath $RepoRoot
try {
    $apuBaseline = Join-Path $OutDir 'apu_baseline.c'
    $ppuBaseline = Join-Path $OutDir 'ppu_renderer_baseline.c'
    Invoke-Checked $GitExe @('show', ("{0}:runner/src/apu.c" -f $BaselineRef)) |
        Set-Content -LiteralPath $apuBaseline -Encoding ascii
    Invoke-Checked $GitExe @('show', ("{0}:runner/src/ppu_renderer.c" -f $BaselineRef)) |
        Set-Content -LiteralPath $ppuBaseline -Encoding ascii

    $common = @(
        '-std=c11',
        '-O2',
        '-DNESRECOMP_TRACE=0',
        '-DNESRECOMP_ENABLE_MODS=0',
        '-I', 'runner\include',
        '-I', 'runner\src'
    )
    $apuRenamesBase = @(
        '-Dapu_init=base_apu_init',
        '-Dapu_write=base_apu_write',
        '-Dapu_read_status=base_apu_read_status',
        '-Dapu_generate=base_apu_generate',
        '-Dapu_clock_cycles=base_apu_clock_cycles',
        '-Dapu_take_dmc_stall=base_apu_take_dmc_stall',
        '-Dapu_irq_asserted=base_apu_irq_asserted',
        '-Dapu_debug_t0=base_apu_debug_t0',
        '-Dapu_get_state_blob=base_apu_get_state_blob',
        '-Dapu_set_state_blob=base_apu_set_state_blob'
    )
    $apuRenamesCand = @(
        '-Dapu_init=cand_apu_init',
        '-Dapu_write=cand_apu_write',
        '-Dapu_read_status=cand_apu_read_status',
        '-Dapu_generate=cand_apu_generate',
        '-Dapu_clock_cycles=cand_apu_clock_cycles',
        '-Dapu_take_dmc_stall=cand_apu_take_dmc_stall',
        '-Dapu_irq_asserted=cand_apu_irq_asserted',
        '-Dapu_debug_t0=cand_apu_debug_t0',
        '-Dapu_get_state_blob=cand_apu_get_state_blob',
        '-Dapu_set_state_blob=cand_apu_set_state_blob'
    )

    $apuBaseObj = Join-Path $OutDir 'apu_base.o'
    $apuCandObj = Join-Path $OutDir 'apu_cand.o'
    $apuBenchExe = Join-Path $OutDir 'bench_apu_period_cache.exe'
    Invoke-Checked $GccExe ($common + $apuRenamesBase + @('-c', $apuBaseline, '-o', $apuBaseObj))
    Invoke-Checked $GccExe ($common + $apuRenamesCand + @('-c', 'runner\src\apu.c', '-o', $apuCandObj))
    Invoke-Checked $GccExe ($common + @('tests\render_audio\bench_apu_period_cache.c', $apuBaseObj, $apuCandObj, '-o', $apuBenchExe))

    $ppuCommon = @(
        '-std=c11',
        '-O2',
        '-DNESRECOMP_TRACE=0',
        '-DNESRECOMP_ENABLE_MODS=0',
        '-I', 'runner\include'
    )
    $ppuBenchBase = Join-Path $OutDir 'bench_ppu_sidecar_base.exe'
    $ppuBenchCand = Join-Path $OutDir 'bench_ppu_sidecar_cand.exe'
    $ppuClassicBase = Join-Path $OutDir 'bench_ppu_classic_only_base.exe'
    $ppuClassicCand = Join-Path $OutDir 'bench_ppu_classic_only_cand.exe'
    Invoke-Checked $GccExe ($ppuCommon + @('tests\render_audio\bench_ppu_sidecar.c', $ppuBaseline, '-o', $ppuBenchBase))
    Invoke-Checked $GccExe ($ppuCommon + @('tests\render_audio\bench_ppu_sidecar.c', 'runner\src\ppu_renderer.c', '-o', $ppuBenchCand))
    Invoke-Checked $GccExe ($ppuCommon + @('tests\render_audio\bench_ppu_classic_only.c', $ppuBaseline, '-o', $ppuClassicBase))
    Invoke-Checked $GccExe ($ppuCommon + @('tests\render_audio\bench_ppu_classic_only.c', 'runner\src\ppu_renderer.c', '-o', $ppuClassicCand))

    Write-Output "APU paired benchmark baseline=$BaselineRef candidate=working-tree compiler=$GccExe config=-O2"
    Invoke-Checked $apuBenchExe @()

    Write-Output "PPU sidecar paired benchmark baseline=$BaselineRef candidate=working-tree compiler=$GccExe config=-O2"
    for ($i = 0; $i -lt 4; $i++) {
        if (($i % 2) -eq 0) {
            Write-Output "PAIR $i BASE"
            Invoke-Checked $ppuBenchBase @()
            Write-Output "PAIR $i CAND"
            Invoke-Checked $ppuBenchCand @()
        } else {
            Write-Output "PAIR $i CAND"
            Invoke-Checked $ppuBenchCand @()
            Write-Output "PAIR $i BASE"
            Invoke-Checked $ppuBenchBase @()
        }
    }

    Write-Output "PPU 256px lower-envelope benchmark baseline=$BaselineRef candidate=working-tree compiler=$GccExe config=-O2"
    for ($i = 0; $i -lt 6; $i++) {
        if (($i % 2) -eq 0) {
            Write-Output "CLASSIC $i BASE"
            Invoke-Checked $ppuClassicBase @()
            Write-Output "CLASSIC $i CAND"
            Invoke-Checked $ppuClassicCand @()
        } else {
            Write-Output "CLASSIC $i CAND"
            Invoke-Checked $ppuClassicCand @()
            Write-Output "CLASSIC $i BASE"
            Invoke-Checked $ppuClassicBase @()
        }
    }
}
finally {
    Pop-Location
}
