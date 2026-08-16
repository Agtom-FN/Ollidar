# Smoke tests for the Windows field-test kit's decision logic.
#
# Runs on macOS/Linux pwsh as well as Windows PowerShell: it deliberately
# exercises only the platform-independent parts - the NMEA parser, the
# verdict functions, the result-file writer and the Mid-360 UDP receiver
# (both the compiled-C# fast path and the pure-PowerShell fallback).
#
# What it CANNOT test without hardware: serial-port enumeration and opening
# (Get-CimInstance Win32_PnPEntity, System.IO.Ports), Get-NetIPAddress /
# Get-NetAdapter / netsh elevation. Those paths are Windows-only and are
# reviewed, not executed, here.
#
# Usage:  pwsh -NoProfile -File tests/smoke_windows.ps1

$ErrorActionPreference = "Stop"
$testDir = $PSScriptRoot
if (-not $testDir) { $testDir = Split-Path -Parent $MyInvocation.MyCommand.Definition }
$kitRoot = Split-Path -Parent $testDir
$scripts = Join-Path $kitRoot "windows/scripts"

# NOTE: the dot-sourced test scripts each assign $here at script scope, which
# would clobber a variable of that name here - hence $testDir/$kitRoot.

$script:Failures = 0
$script:Checks   = 0

function Check([string]$name, [bool]$ok, [string]$detail) {
  $script:Checks++
  if ($ok) {
    Write-Host ("  PASS  " + $name + $(if ($detail) { "  [" + $detail + "]" } else { "" })) -ForegroundColor Green
  } else {
    $script:Failures++
    Write-Host ("  FAIL  " + $name + "  [" + $detail + "]") -ForegroundColor Red
  }
}

# Point the kit at a scratch "Desktop" so we never touch the real one.
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("kitsmoke_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

. (Join-Path $scripts "common.ps1")
. (Join-Path $scripts "test_um982.ps1")
. (Join-Path $scripts "test_mid360.ps1")

$Global:KitResultDir = $scratch   # override Get-ResultDir's Desktop lookup

Write-Host ""
Write-Host "=== 1. NMEA checksum ===" -ForegroundColor Cyan
Check "valid NMEA checksum accepted" ((Test-NmeaChecksum '$GNGGA,123400.00,2229.9042,N,11410.5533,E,1,09,0.9,31.4,M,-2.3,M,,*5D') -eq $true) ""
Check "bad NMEA checksum rejected"   ((Test-NmeaChecksum '$GNGGA,123400.00,2229.9042,N,11410.5533,E,1,09,0.9,31.4,M,-2.3,M,,*00') -eq $false) ""
Check "no-checksum line returns null" ($null -eq (Test-NmeaChecksum '$GNGGA,123400.00')) ""
Check "non-sentence returns null"     ($null -eq (Test-NmeaChecksum 'hello')) ""

Write-Host ""
Write-Host "=== 2. fix names ===" -ForegroundColor Cyan
Check "quality 1 = SINGLE"     ((Get-FixName "1") -match "SINGLE") (Get-FixName "1")
Check "quality 4 = RTK FIXED"  ((Get-FixName "4") -match "RTK FIXED") (Get-FixName "4")
Check "quality 0 = NO FIX"     ((Get-FixName "0") -match "NO FIX") (Get-FixName "0")
Check "no GGA yet = none"      ((Get-FixName "-1") -match "none") (Get-FixName "-1")

Write-Host ""
Write-Host "=== 3. UM982 stream stats over synthetic data ===" -ForegroundColor Cyan
$py = "python3"
$gen = Join-Path $testDir "make_synthetic.py"

function Read-Stats([string]$file) {
  $st = New-NmeaStats
  foreach ($ln in [System.IO.File]::ReadAllLines($file)) { Add-NmeaLine -Stats $st -Line $ln }
  return $st
}

# (a) healthy single-fix capture, 90 s
$f1 = Join-Path $scratch "um982_90s.nmea"
& $py $gen um982 $f1 --seconds 90 --fix 1 | Out-Null
$s1 = Read-Stats $f1
Check "sentence count"      ($s1.Sentences -eq 540) ("got " + $s1.Sentences + " expected 540 (6 NMEA/s x 90)")
Check "all checksums good"  ($s1.CsBad -eq 0 -and $s1.CsOk -eq 540) ("ok=" + $s1.CsOk + " bad=" + $s1.CsBad)
Check "Unicore logs counted, not called corrupt" ($s1.Unicore -eq 90) ("unicore=" + $s1.Unicore)
Check "heading sentences seen" ($s1.Heading -ge 90) ("heading=" + $s1.Heading)
Check "best fix = 1 (SINGLE)" ($s1.BestFix -eq 1) ("bestfix=" + $s1.BestFix)
Check "satellites parsed"   ($s1.MaxSats -ge 9) ("maxsats=" + $s1.MaxSats)
Check "GGA histogram has only quality 1" ($s1.FixHist.Count -eq 1 -and $s1.FixHist["1"] -eq 90) (($s1.FixHist.Keys | Sort-Object) -join ",")
Check "SINGLE fix with no corrections is a PASS" ((Get-Um982Verdict -Stats $s1 -Elapsed 90) -eq "PASS") (Get-Um982Verdict -Stats $s1 -Elapsed 90)

# (b) receiver alive but no sky view -> WARN, never FAIL
$f2 = Join-Path $scratch "um982_nofix_90s.nmea"
& $py $gen um982 $f2 --seconds 90 --fix 0 | Out-Null
$s2 = Read-Stats $f2
Check "no-fix stream still parses" ($s2.Sentences -eq 540) ("sentences=" + $s2.Sentences)
Check "no-fix histogram is quality 0" ($s2.FixHist["0"] -eq 90) ("hist0=" + $s2.FixHist["0"])
Check "no-fix verdict is WARN not FAIL" ((Get-Um982Verdict -Stats $s2 -Elapsed 90) -eq "WARN") (Get-Um982Verdict -Stats $s2 -Elapsed 90)

# (c) corrupted stream -> WARN
$f3 = Join-Path $scratch "um982_corrupt_90s.nmea"
& $py $gen um982 $f3 --seconds 90 --fix 1 --corrupt 2 | Out-Null
$s3 = Read-Stats $f3
Check "corrupt sentences detected" ($s3.CsBad -eq 45) ("bad=" + $s3.CsBad)
Check "corrupt stream verdict is WARN" ((Get-Um982Verdict -Stats $s3 -Elapsed 90) -eq "WARN") (Get-Um982Verdict -Stats $s3 -Elapsed 90)

# (d) silence -> FAIL
$s4 = New-NmeaStats
Check "silence verdict is FAIL" ((Get-Um982Verdict -Stats $s4 -Elapsed 90) -eq "FAIL") (Get-Um982Verdict -Stats $s4 -Elapsed 90)

Write-Host ""
Write-Host "=== 4. combined TEST_RESULT.txt writer ===" -ForegroundColor Cyan
Add-KitLog "smoke: first line"
Add-KitLog "smoke: second line"
Save-KitLog -Title "TEST 9 SMOKE" -Verdict "PASS"
Add-KitLog "smoke: another test"
Save-KitLog -Title "TEST 8 SMOKE" -Verdict "WARN"
$rf = Get-ResultFile
$txt = Get-Content $rf -Raw
Check "result file created"      (Test-Path $rf) $rf
Check "two blocks appended"      (([regex]::Matches($txt, "RESULT: ")).Count -eq 2) ("blocks=" + ([regex]::Matches($txt, "RESULT: ")).Count)
Check "PASS block present"       ($txt -match "TEST 9 SMOKE") ""
Check "WARN block present"       ($txt -match "RESULT: WARN") ""
Check "menu summary accumulated" ($Global:KitResults.Count -eq 2) ("results=" + $Global:KitResults.Count)

Write-Host ""
Write-Host "=== 5. Mid-360 UDP receiver + .livoxdump container ===" -ForegroundColor Cyan
$port = 56100
$dump = Join-Path $scratch "mid360_5s.livoxdump"
$sender = Start-Process -FilePath $py -ArgumentList @($gen, "mid360", "--port", "$port", "--seconds", "6", "--rate", "2000") -PassThru -NoNewWindow
Start-Sleep -Milliseconds 400
$cap = Invoke-Mid360Capture -PortList @($port, 56200) -OutPath $dump -Seconds 4 -HostIp "127.0.0.1"
try { $sender.WaitForExit(8000) | Out-Null } catch { }
Check "capture reported ok"   ($cap.Ok) ($cap.Error)
Check "receiver engine"       ($cap.Engine -in @("fast", "fallback")) $cap.Engine
Check "datagrams received"    ($cap.Pkts[0] -gt 1000) ("port56100=" + $cap.Pkts[0])
Check "quiet port stays zero" ($cap.Pkts[1] -eq 0) ("port56200=" + $cap.Pkts[1])
$rate = $cap.Pkts[0] / $cap.Elapsed
Check "healthy-rate threshold (>1500/s) reached" ($rate -ge 1500) ("{0:N0}/s" -f $rate)

# container header must be byte-identical to capture_mid360.py's
$fsBytes = [System.IO.File]::ReadAllBytes($dump)
$magic = [System.Text.Encoding]::ASCII.GetString($fsBytes, 0, 8)
$ver   = [BitConverter]::ToUInt16($fsBytes, 8)
$np    = [BitConverter]::ToUInt16($fsBytes, 10)
$p0    = [BitConverter]::ToUInt32($fsBytes, 12)
$p1    = [BitConverter]::ToUInt32($fsBytes, 16)
Check "magic LX360CAP" ($magic -eq "LX360CAP") $magic
Check "version 1"      ($ver -eq 1) "$ver"
Check "num_ports 2"    ($np -eq 2) "$np"
Check "port table"     ($p0 -eq 56100 -and $p1 -eq 56200) "$p0,$p1"

Write-Host ""
Write-Host "=== 6. dev-side verify_capture.py reads what the kit wrote ===" -ForegroundColor Cyan
$verify = Join-Path (Split-Path -Parent $kitRoot) "remote-capture/verify_capture.py"
if (Test-Path $verify) {
  $vout = & $py $verify $dump 2>&1 | Out-String
  Check "verify_capture.py parsed the .livoxdump" ($vout -match "container header OK") ($vout -split "`n" | Select-Object -First 1)
  Check "verify_capture.py saw the packets" ($vout -match "port 56100: [\d,]+ pkts") ""
  $vout2 = & $py $verify $f1 2>&1 | Out-String
  Check "verify_capture.py verdicts the NMEA file" ($vout2 -match "OVERALL") (($vout2 -split "`n" | Where-Object { $_ -match "OVERALL" }) -join "")
} else {
  Write-Host "  SKIP  verify_capture.py not found at $verify" -ForegroundColor Yellow
}

Write-Host ""
Write-Host ("Scratch dir: " + $scratch)
Write-Host ""
if ($script:Failures -eq 0) {
  Write-Host ("ALL " + $script:Checks + " CHECKS PASSED") -ForegroundColor Green
  exit 0
} else {
  Write-Host ($script:Failures.ToString() + " of " + $script:Checks + " CHECKS FAILED") -ForegroundColor Red
  exit 1
}
