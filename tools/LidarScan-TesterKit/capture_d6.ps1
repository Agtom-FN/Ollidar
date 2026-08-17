# LidarScan tester kit — one-click COIN-D6 capture (Windows)
# Launched by START_TEST.bat. No installs, no typing: finds the lidar's COM
# port, records 30 seconds of raw data, checks it, and leaves ONE folder on
# the Desktop for the tester to send back.
#
# Engineering notes (for the dev, not the tester):
#  - 230400 8N1, DTR cleared (matches S1's d6cli / vendor CH340 behavior)
#  - start AA 55 F0 0F / stop AA 55 F5 0A per the D6 protocol spec
#  - PASS heuristics mirror tools/remote-capture/verify_capture.py's quick
#    check: byte rate ~11.5 KB/s and AA 55 header density; the REAL
#    verification (checksum variant + noise sigma) happens back on the dev
#    machine with verify_capture.py / d6cli --replay.

$ErrorActionPreference = "Stop"
$Seconds = 30

function Banner([string]$text, [string]$color) {
  Write-Host ""
  Write-Host ("=" * 58) -ForegroundColor $color
  Write-Host ("   " + $text) -ForegroundColor $color
  Write-Host ("=" * 58) -ForegroundColor $color
  Write-Host ""
}

function Finish([int]$code) {
  Write-Host ""
  Read-Host "Press the ENTER key to close this window" | Out-Null
  exit $code
}

Banner "LIDAR TEST - please wait, this runs by itself" Cyan
Write-Host "  Test length: $Seconds seconds. Do not unplug anything."
Write-Host ""

# --- output folder on the Desktop ------------------------------------------
$desktop = [Environment]::GetFolderPath("Desktop")
$outDir  = Join-Path $desktop "LIDAR_TEST_RESULT"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$binPath = Join-Path $outDir "bench_d6_30s.bin"
$txtPath = Join-Path $outDir "TEST_RESULT.txt"
$log     = New-Object System.Collections.Generic.List[string]
$log.Add("LidarScan D6 tester capture")
$log.Add("date: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
$log.Add("computer: " + $env:COMPUTERNAME + "  windows: " + [Environment]::OSVersion.VersionString)

# --- find the lidar's COM port ---------------------------------------------
$portName = $null
try {
  $pnp = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -match "CH340|USB-SERIAL|USB Serial" -and $_.Name -match "\(COM\d+\)" }
  if ($pnp) {
    $portName = [regex]::Match(@($pnp)[0].Name, "COM\d+").Value
    $log.Add("device: " + @($pnp)[0].Name)
  }
} catch { }

if (-not $portName) {
  $all = [System.IO.Ports.SerialPort]::GetPortNames()
  $log.Add("no CH340 name matched; COM ports present: " + ($all -join ", "))
  if ($all.Count -eq 1) { $portName = $all[0] }
}

if (-not $portName) {
  Banner "PROBLEM: the computer cannot see the lidar" Red
  Write-Host "  What to do (in this order):" -ForegroundColor Yellow
  Write-Host "   1. Check the lidar's USB cable is pushed in fully."
  Write-Host "   2. Unplug the USB, wait 5 seconds, plug it back in."
  Write-Host "   3. Make sure the computer is connected to the internet,"
  Write-Host "      then unplug and replug once more (Windows fetches the"
  Write-Host "      driver by itself the first time - give it a minute)."
  Write-Host "   4. Run START_TEST again."
  Write-Host "   5. Still failing? Take a PHOTO of this window and send it."
  $log.Add("RESULT: FAIL - no serial device found")
  $log | Set-Content -Path $txtPath
  Finish 1
}

Write-Host ("  Found the lidar on port " + $portName) -ForegroundColor Green
$log.Add("port: " + $portName)

# --- open port, start lidar, capture ---------------------------------------
$port = New-Object System.IO.Ports.SerialPort $portName, 230400, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$port.DtrEnable   = $false
$port.RtsEnable   = $false
$port.ReadTimeout = 200
try {
  $port.Open()
} catch {
  Banner "PROBLEM: another program is using the lidar" Red
  Write-Host "  Close every other window (especially any lidar software),"
  Write-Host "  then run START_TEST again."
  $log.Add("RESULT: FAIL - port busy or blocked: " + $_.Exception.Message)
  $log | Set-Content -Path $txtPath
  Finish 1
}

$start = [byte[]]@(0xAA, 0x55, 0xF0, 0x0F)
$stop  = [byte[]]@(0xAA, 0x55, 0xF5, 0x0A)
$port.Write($start, 0, 4)

$fs   = [System.IO.File]::Open($binPath, [System.IO.FileMode]::Create)
$buf  = New-Object byte[] 4096
$sw   = [System.Diagnostics.Stopwatch]::StartNew()
$total = 0
$lastShown = -1

Write-Host ""
Write-Host "  Recording... the lidar should be SPINNING and may glow red inside." -ForegroundColor Cyan
Write-Host ""

while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
  try {
    $n = $port.Read($buf, 0, $buf.Length)
    if ($n -gt 0) { $fs.Write($buf, 0, $n); $total += $n }
  } catch [System.TimeoutException] { }
  $sec = [int]$sw.Elapsed.TotalSeconds
  if ($sec -ne $lastShown) {
    $lastShown = $sec
    Write-Host ("`r  {0,2} / {1} seconds   {2,7:N0} bytes received " -f $sec, $Seconds, $total) -NoNewline
  }
}
Write-Host ""

try { $port.Write($stop, 0, 4); Start-Sleep -Milliseconds 300 } catch { }
try { $port.Close() } catch { }
$fs.Close()
$log.Add(("bytes captured in {0}s: {1}" -f $Seconds, $total))

# --- quick self-check -------------------------------------------------------
$rate = $total / $Seconds
$log.Add(("byte rate: {0:N0} bytes/s (expected around 11,500)" -f $rate))

# count AA 55 header pairs (the D6 packet header on the wire)
$bytes = [System.IO.File]::ReadAllBytes($binPath)
$headers = 0
for ($i = 0; $i -lt $bytes.Length - 1; $i++) {
  if ($bytes[$i] -eq 0xAA -and $bytes[$i + 1] -eq 0x55) { $headers++ }
}
$log.Add("packet headers seen: " + $headers)

$pass = ($total -gt 200000) -and ($headers -gt 1000)
$warn = (-not $pass) -and ($total -gt 20000)

if ($pass) {
  Banner "SUCCESS - THE TEST WORKED" Green
  Write-Host "  One last step:" -ForegroundColor Yellow
  Write-Host "   Send the folder  LIDAR_TEST_RESULT  (it is on the Desktop)"
  Write-Host "   back the same way you received this test - WhatsApp, WeChat,"
  Write-Host "   or email. It contains 2 small files. That's everything!"
  $log.Add("RESULT: PASS")
} elseif ($warn) {
  Banner "PARTLY WORKED - please send the result anyway" Yellow
  Write-Host "  Some data arrived but less than expected."
  Write-Host "  Send the Desktop folder LIDAR_TEST_RESULT back, and also"
  Write-Host "  mention whether the lidar was spinning during the test."
  $log.Add("RESULT: WARN - low data rate")
} else {
  Banner "PROBLEM: the lidar did not send data" Red
  Write-Host "  Likely causes:" -ForegroundColor Yellow
  Write-Host "   - The lidar has no power (is its adapter board plugged in?)"
  Write-Host "   - Wrong cable or a charge-only USB cable"
  Write-Host "  Try once more, and if it fails again take a PHOTO of this"
  Write-Host "  window and send it together with the Desktop folder."
  $log.Add("RESULT: FAIL - no/near-zero data")
}

$log | Set-Content -Path $txtPath
Finish 0
