function Get-StageRelativePath([string]$stage, [string]$path) {
  return $path.Substring($stage.Length).TrimStart('\', '/').Replace('\', '/')
}

function Assert-ReleaseArchive([string]$zip, [string]$stage) {
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $expected = @{}
  foreach ($file in Get-ChildItem $stage -Recurse -File) {
    $relative = Get-StageRelativePath $stage $file.FullName
    $expected[$relative] = $file.FullName
  }

  $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
  try {
    $seen = @{}
    foreach ($entry in $archive.Entries) {
      if ($entry.FullName.Contains('\')) {
        throw "release archive contains a non-portable Windows path: $($entry.FullName)"
      }
      $relative = $entry.FullName
      if ([string]::IsNullOrEmpty($entry.Name)) { continue }
      if ($relative.StartsWith('/') -or $relative -match '(^|/)\.\.(/|$)') {
        throw "release archive contains unsafe path: $relative"
      }
      if ($seen.ContainsKey($relative)) {
        throw "release archive contains duplicate path: $relative"
      }
      $seen[$relative] = $true
      if (-not $expected.ContainsKey($relative)) {
        throw "release archive contains unstaged payload: $relative"
      }

      $sha = [System.Security.Cryptography.SHA256]::Create()
      $stream = $entry.Open()
      try {
        $archiveHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
      } finally {
        $stream.Dispose()
        $sha.Dispose()
      }
      $stageHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $expected[$relative]).Hash
      if ($archiveHash -ne $stageHash) {
        throw "release archive content differs from staging: $relative"
      }
    }
    $missing = @($expected.Keys | Where-Object { -not $seen.ContainsKey($_) })
    if ($missing.Count -ne 0) {
      throw "release archive is missing staged payload: $($missing -join ', ')"
    }
  } finally {
    $archive.Dispose()
  }
}


function New-VerifiedReleaseArchive([string]$Stage, [string]$Zip) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $full = [IO.Path]::GetFullPath($Stage).TrimEnd('\') + '\'
    $temporary = $Zip + '.partial-' + [guid]::NewGuid().ToString('N')
    try {
        $archive = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($file in Get-ChildItem -LiteralPath $Stage -Recurse -File | Sort-Object FullName) {
                $relative = Get-StageRelativePath $Stage $file.FullName
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.FullName,$relative,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
            }
        } finally { $archive.Dispose() }
        Assert-ReleaseArchive $temporary $Stage
        if (Test-Path -LiteralPath $Zip) { [IO.File]::Replace($temporary,$Zip,[NullString]::Value) }
        else { [IO.File]::Move($temporary,$Zip) }
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}

function Remove-VerifiedReleaseStage([string]$Stage, [string]$OutputDirectory, [string]$ProjectRoot) {
    $full=[IO.Path]::GetFullPath($Stage)
    $out=[IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
    if ([IO.Path]::GetDirectoryName($full) -ine $out -or
        [IO.Path]::GetFileName($full) -notmatch '^stage-cycle-[0-9a-f]{32}$') { throw "Unexpected release stage: $full" }
    foreach ($ancestor in @($ProjectRoot,$out)) {
        if ([IO.File]::GetAttributes($ancestor) -band [IO.FileAttributes]::ReparsePoint) { throw "Release parent is a reparse point: $ancestor" }
    }
    $pending=New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($full)
    while ($pending.Count) {
        $directory=$pending.Pop()
        if ([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint) { throw "Release stage contains a reparse point: $directory" }
        if ([IO.File]::Exists((Join-Path $directory 'HEAD')) -and [IO.Directory]::Exists((Join-Path $directory 'objects'))) { throw "Release stage contains a nested repository: $directory" }
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($directory)) {
            if ([IO.Path]::GetFileName($entry) -ieq '.git') { throw "Release stage contains a nested repository: $entry" }
            $attributes=[IO.File]::GetAttributes($entry)
            if ($attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Release stage contains a reparse point: $entry" }
            if ($attributes -band [IO.FileAttributes]::Directory) { $pending.Push($entry) }
        }
    }
    Remove-Item -LiteralPath $full -Recurse -ErrorAction Stop
    if (Test-Path -LiteralPath $full) { throw "Release stage was not removed: $full" }
}
