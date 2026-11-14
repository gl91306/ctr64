# Puts the release zip together after scripts\build-3ds.ps1:
#   dist/3ds/ctr64-release.zip = ctr64.cia, ctr64.3dsx, README.md, LICENSE,
#   3ds/ctr64/jit/shipped/*.blk (ctr64/jit-shipped) and an empty 3ds/ctr64/roms/
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist\3ds'
foreach ($f in 'ctr64.cia', 'ctr64.3dsx') {
    if (-not (Test-Path (Join-Path $dist $f))) { throw "$f missing in dist\3ds: run scripts\build-3ds.ps1 (with makerom) first" }
}
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ('ctr64-release-' + [guid]::NewGuid())
New-Item -ItemType Directory -Force (Join-Path $stage '3ds\ctr64\jit\shipped') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage '3ds\ctr64\roms') | Out-Null
Copy-Item (Join-Path $dist 'ctr64.cia'), (Join-Path $dist 'ctr64.3dsx') $stage
Copy-Item (Join-Path $root 'README.md'), (Join-Path $root 'LICENSE') $stage
Copy-Item (Join-Path $root 'ctr64\jit-shipped\*.blk') (Join-Path $stage '3ds\ctr64\jit\shipped')
$zip = Join-Path $dist 'ctr64-release.zip'
Remove-Item $zip -ErrorAction SilentlyContinue
# .NET keeps the empty roms folder (Compress-Archive drops empty folders)
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)
Remove-Item -Recurse -Force $stage
Write-Host "Done: $zip"
