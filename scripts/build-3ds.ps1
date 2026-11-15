# Builds ctr64 (Nintendo 3DS).  Output: dist/3ds/ctr64.3dsx and, if makerom is found, dist/3ds/ctr64.cia
#
# Needs: devkitPro with the 3ds-dev group (devkitARM, libctru, picasso, 3dsxtool, smdhtool) and git.
# devkitPro's CMake toolchain only runs under its bundled msys2 cmake, so the build is driven
# through C:\devkitPro\msys2 (override the location with $env:DEVKITPRO_WIN).
# The core (Mupen64Plus-Next) is cloned at the pinned commit and patched on first use.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dkp = if ($env:DEVKITPRO_WIN) { $env:DEVKITPRO_WIN } else { 'C:\devkitPro' }
$bash = Join-Path $dkp 'msys2\usr\bin\bash.exe'
if (-not (Test-Path $bash)) { throw "devkitPro msys2 not found at $bash (install devkitPro, or set DEVKITPRO_WIN)" }

# 1. the patched core
$core = Join-Path $root 'third_party\mupen64plus-libretro-nx'
if (-not (Test-Path (Join-Path $core 'libretro\libretro.c'))) {
    $commit = (Get-Content (Join-Path $root 'ctr64\patches\BASE_COMMIT') -Raw).Trim()
    git clone https://github.com/libretro/mupen64plus-libretro-nx $core
    if ($LASTEXITCODE) { throw 'git clone failed' }
    git -C $core checkout -q $commit
    if ($LASTEXITCODE) { throw "git checkout $commit failed" }
    git -C $core apply --whitespace=nowarn (Join-Path $root 'ctr64\patches\mupen64plus-libretro-nx.patch')
    if ($LASTEXITCODE) { throw 'applying ctr64/patches/mupen64plus-libretro-nx.patch failed' }
}

# 2. build
$src = (& $bash -lc "cygpath -u '$root'").Trim()
& $bash -lc "cd '$src' && cmake --preset 3ds-release && cmake --build --preset 3ds-release -j8"
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$out = Join-Path $root 'dist\3ds'
$bld = Join-Path $root 'build\3ds\ctr64'
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $bld 'ctr64.3dsx') $out -Force
Copy-Item (Join-Path $bld 'ctr64.elf') $out -Force # for crash.txt addresses (arm-none-eabi-addr2line)

# 3. the CIA (makerom from Project_CTR, on PATH or in devkitPro's tools\bin)
$makerom = (Get-Command makerom -ErrorAction SilentlyContinue).Source
if (-not $makerom -and (Test-Path (Join-Path $dkp 'tools\bin\makerom.exe'))) { $makerom = Join-Path $dkp 'tools\bin\makerom.exe' }
if ($makerom) {
    # title version from CTR64_VERSION in ctr64/CMakeLists.txt: major << 10 | minor << 4 | micro
    $m = Select-String -Path (Join-Path $root 'ctr64\CMakeLists.txt') -Pattern 'set\(CTR64_VERSION "(\d+)\.(\d+)\.(\d+)"\)'
    $g = $m.Matches[0].Groups
    $ver = ([int]$g[1].Value -shl 10) -bor ([int]$g[2].Value -shl 4) -bor [int]$g[3].Value
    & $makerom -f cia -o (Join-Path $out 'ctr64.cia') -elf (Join-Path $bld 'ctr64.elf') `
        -rsf (Join-Path $root 'ctr64\cia\ctr64.rsf') -icon (Join-Path $bld 'ctr64.smdh') `
        -banner (Join-Path $root 'ctr64\cia\banner.bnr') -target t -exefslogo -ver $ver
    if ($LASTEXITCODE) { throw 'makerom failed' }
    Write-Host "Done: $out\ctr64.3dsx, $out\ctr64.cia"
} else {
    Write-Host "Done: $out\ctr64.3dsx (no makerom found: no .cia; see README)"
}
