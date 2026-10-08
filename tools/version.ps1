# Shows or changes the wave-os version (version.h).
#   version.bat            show the current version
#   version.bat 1.6        start the big update 1.6 (no patch suffix yet)
#   version.bat patch      next patch of the current update: v1.6 -> v1.6-001 -> v1.6-002 ...
param([string]$What = '', [string]$Header = '')
$ErrorActionPreference = 'Stop'
$h = if ($Header) { $Header } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'version.h' }
$t = [IO.File]::ReadAllText($h)
if ($t -notmatch '#define WAVE_VERSION "([^"]*)"' ) { throw "version.h has no WAVE_VERSION" }
$ver = $Matches[1]
if ($t -notmatch '#define WAVE_PATCH\s+"([^"]*)"') { throw "version.h has no WAVE_PATCH" }
$patch = $Matches[1]
if ($What -eq '') { "v$ver$patch"; exit 0 }
if ($What -eq 'patch') {
    $n = if ($patch -match '^-(\d{3})$') { [int]$Matches[1] + 1 } else { 1 }
    if ($n -gt 999) { throw "more than 999 patches: start a new update instead" }
    $patch = '-{0:000}' -f $n
} elseif ($What -match '^\d+(\.\d+)*$') {
    $ver = $What; $patch = ''
} else { throw "usage: version.bat [1.6 | patch]" }
$t = [regex]::Replace($t, '#define WAVE_VERSION "[^"]*"', "#define WAVE_VERSION `"$ver`"")
$t = [regex]::Replace($t, '#define WAVE_PATCH\s+"[^"]*"', "#define WAVE_PATCH   `"$patch`"")
[IO.File]::WriteAllText($h, $t)
"now v$ver$patch   (tag it  v$ver$patch-<name>  and name the release file wave-os-v$ver$patch-<name>.kpart)"
