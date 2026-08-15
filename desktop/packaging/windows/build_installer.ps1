<#
.SYNOPSIS
  Build LidarScan for Windows x64 and produce the NSIS installer.

.DESCRIPTION
  The Windows twin of tools/package_macos.sh. Configure + build, run
  windeployqt into a staging tree, then makensis.

  !! NEVER EXECUTED !!
    This repo's only host is macOS (desktop/NOTES.md §6 "Windows and Linux are
    not built, let alone run"). This script is staged for the first Windows CI
    run — packaging/ci/windows-installer.yml.snippet is the job that will run
    it. Nothing below has been observed working; the NSIS script it invokes HAS
    been syntax-checked (see packaging/README.md).

  Prerequisites on the machine that runs it:
    * Visual Studio 2022 Build Tools (or clang-cl) — the engine's own
      CMakePresets.json has windows-msvc-x64 / windows-clangcl-x64 presets
    * Qt 6.11.1 msvc2022_64 with the SerialPort module, and its bin\ on PATH
      (aqtinstall works here too: aqt install-qt windows desktop 6.11.1
       win64_msvc2022_64 -m qtserialport)
    * Filament v1.75.0 Windows release unpacked into desktop\third_party\filament
      (the Windows tarball DOES ship the only arch Windows needs, so there is
       no equivalent of the macOS universal problem here)
    * NSIS 3 (makensis.exe on PATH)

  THE OPEN RISK, stated plainly: NOTES.md §3.1 marks the Windows renderer
  UNVERIFIED. NativeSurface_win.cpp (HWND → Vulkan swapchain) has never been
  compiled by anyone, gl_PointSize on Vulkan may clamp to 1.0, and the render
  clock is a plain timer rather than a DXGI waitable object. Producing an
  installer is not the same as producing a working app — the first Windows run
  should be a manual smoke test, not a release.

.PARAMETER Version
  Version string stamped into the installer. Defaults to 0.1.0.
#>
param(
  [string]$Version = "0.1.0",
  [string]$QtRoot  = $env:QT_ROOT,
  [string]$BuildDir = "build-win64",
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$Root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)  # desktop/
$Stage = Join-Path $PSScriptRoot "staging"

function Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Yellow }

if (-not $QtRoot) { throw "Set QT_ROOT (e.g. C:\Qt\6.11.1\msvc2022_64) or pass -QtRoot" }
$windeployqt = Join-Path $QtRoot "bin\windeployqt.exe"
if (-not (Test-Path $windeployqt)) { throw "windeployqt not found at $windeployqt" }

if (-not $SkipBuild) {
  Step "configure + build (x64, Release)"
  cmake -S $Root -B (Join-Path $Root $BuildDir) -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_PREFIX_PATH="$QtRoot"
  if ($LASTEXITCODE -ne 0) { throw "configure failed" }
  cmake --build (Join-Path $Root $BuildDir) --parallel
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$exe = Join-Path $Root "$BuildDir\lidarscan.exe"
if (-not (Test-Path $exe)) { throw "$exe not produced" }

Step "staging + windeployqt"
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Path $Stage | Out-Null
Copy-Item $exe $Stage
# points.filamat sits next to the exe on Windows — ViewportWindow.cpp looks in
# applicationDirPath() first, which is exactly where this puts it (the
# Contents/Resources fallback it also has is macOS-only).
Copy-Item (Join-Path $Root "$BuildDir\points.filamat") $Stage

# --release: no debug DLLs. --no-translations keeps the payload small (the app
# is English-only today). --compiler-runtime pulls in the MSVC redistributable
# DLLs so the installer does not need a separate VC++ redist step.
& $windeployqt --release --no-translations --compiler-runtime `
    --dir $Stage (Join-Path $Stage "lidarscan.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

Write-Host "staged files:"
Get-ChildItem -Recurse $Stage | Select-Object -ExpandProperty FullName |
  ForEach-Object { "    " + $_.Substring($Stage.Length + 1) }

Step "license file"
# NSIS's MUI_PAGE_LICENSE needs a real file. Qt is LGPLv3-dynamic (Tech Spec
# §1) and Filament is Apache-2.0; the shipped text has to say so.
Copy-Item (Join-Path $PSScriptRoot "LICENSE.txt") $Stage -ErrorAction SilentlyContinue

Step "makensis"
Push-Location $PSScriptRoot
try {
  makensis /DVERSION=$Version /DSTAGE_DIR=staging lidarscan.nsi
  if ($LASTEXITCODE -ne 0) { throw "makensis failed" }
} finally { Pop-Location }

$setup = Join-Path $PSScriptRoot "LidarScan-$Version-x64-setup.exe"
Step "result"
Get-Item $setup | Format-List Name, Length, LastWriteTime

Write-Host @"

NOT SIGNED. A shipping installer must be Authenticode-signed or SmartScreen
will warn every user for the first few thousand downloads:

    signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ^
        /f cert.pfx /p <password> "$setup"

Sign lidarscan.exe BEFORE makensis runs as well — signing only the installer
leaves an unsigned binary on disk after installation.
"@ -ForegroundColor DarkYellow
