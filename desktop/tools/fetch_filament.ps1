<#
.SYNOPSIS
  Fetch the prebuilt Filament release for Windows/MSVC — the Windows twin of
  tools/fetch_filament.sh.

.DESCRIPTION
  This is a separate script rather than a branch inside the bash one because the
  Windows release tarball is NOT laid out like the mac/linux ones, and the
  difference is not cosmetic:

    mac / linux :  filament/include/...        <- a top-level `filament/` dir
                   filament/lib/<arch>/lib<name>.a
                   filament/bin/matc

    windows     :  include/...                 <- NO top-level dir at all
                   lib/x86_64/md/<name>.lib    <- /MD (release) CRT
                   lib/x86_64/mdd/<name>.lib   <- /MDd (debug) CRT
                   bin/matc.exe

  `tar xzf ... -C third_party/filament` (what the bash script does) therefore
  produces third_party/filament/include on Windows and
  third_party/filament/filament/include everywhere else, and CMakeLists.txt
  would have to grow a platform branch for the difference. Extracting into
  third_party/filament/filament instead makes FILAMENT_DIR identical on all
  three platforms; CMakeLists.txt only has to know about the lib naming.

  The archive is ~790 MB (it ships every sample, both CRT flavours and a large
  docs/ tree), so by default only the three things this build needs are
  extracted: include/, lib/x86_64/ and bin/matc.exe. -Full extracts everything.

.PARAMETER Version
  Filament release tag. PINNED on purpose — see CMakeLists.txt's header and
  spikes/s3-render/REPORT.md §9 caveat 2.
#>
param(
  [string]$Version = "v1.75.0",
  [switch]$Full
)

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dest = Join-Path $here "third_party\filament\filament"
$asset = "filament-$Version-windows.tgz"
$url = "https://github.com/google/filament/releases/download/$Version/$asset"

if (Test-Path (Join-Path $dest "bin\matc.exe")) {
  Write-Host "[filament] already present at $dest"
  exit 0
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null
$tgz = Join-Path $env:TEMP $asset
Write-Host "[filament] fetching $url"
# Invoke-WebRequest's progress bar costs minutes on a ~790 MB file in a
# non-interactive shell; turning it off is a real speedup, not a style choice.
$ProgressPreference = "SilentlyContinue"
Invoke-WebRequest -Uri $url -OutFile $tgz

if ($Full) {
  tar xzf $tgz -C $dest
} else {
  # bsdtar (which is what `tar` is on Windows 10+) takes member paths.
  tar xzf $tgz -C $dest include lib/x86_64 bin/matc.exe
}
if ($LASTEXITCODE -ne 0) { throw "tar failed with $LASTEXITCODE" }
Remove-Item $tgz -Force

if (-not (Test-Path (Join-Path $dest "bin\matc.exe"))) {
  throw "matc.exe missing after extraction — the release layout changed"
}
if (-not (Test-Path (Join-Path $dest "lib\x86_64\md\filament.lib"))) {
  throw "lib/x86_64/md/filament.lib missing after extraction — the release layout changed"
}
Write-Host "[filament] unpacked into $dest"
& (Join-Path $dest "bin\matc.exe") --version | Out-Null
Write-Host "[filament] matc OK"
