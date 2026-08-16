# LidarScan FIELD-TEST KIT v2 - test 1: COIN-D6 single-line lidar (Windows)
#
# Evolved from tools/tester-kit/capture_d6.ps1 (v1). Same wire behaviour:
#   230400 8N1, DTR cleared, start AA 55 F0 0F, stop AA 55 F5 0A
# What is NEW in v2:
#   - probes every COM port instead of trusting the CH340 friendly name, so a
#     UM982 GPS board (which may ALSO be a CH340) on a second COM port cannot
#     be mistaken for the lidar, and vice versa
#   - writes into the shared Desktop\LIDAR_TEST_RESULT folder and appends to
#     the ONE combined TEST_RESULT.txt
#
# The real verification still happens back at the dev machine with
# verify_capture.py / d6cli --replay; the PASS here is a rate + framing check.

param([int]$Seconds = 30)

$here = $PSScriptRoot
if (-not $here) { $here = Split-Path -Parent $MyInvocation.MyCommand.Definition }
. (Join-Path $here "common.ps1")

$D6_BAUD  = 230400
$D6_START = [byte[]]@(0xAA, 0x55, 0xF0, 0x0F)
$D6_STOP  = [byte[]]@(0xAA, 0x55, 0xF5, 0x0A)

function Invoke-D6Test {
  param([int]$Seconds = 30)

  Show-Banner "TEST 1 of 3 - SPINNING LIDAR (COIN-D6)" "Cyan"
  Write-Host "  This takes about $Seconds seconds and runs by itself."
  Write-Host "  Make sure the small silver/black lidar is plugged in by USB."
  Write-Host ""

  $outDir  = Get-ResultDir
  $binPath = Join-Path $outDir ("d6_" + $Seconds + "s.bin")
  Add-KitLog ("capture file: " + (Split-Path -Leaf $binPath))
  Add-KitLog ("settings: 230400 8N1, DTR cleared, start AA55F00F / stop AA55F50A")

  # --- find the lidar -------------------------------------------------------
  Write-Host "  Looking for the lidar on the computer's ports..." -ForegroundColor Cyan
  $cands = Get-SerialPortCandidates
  if ($cands.Count -eq 0) {
    Add-KitLog "no COM ports present at all"
    Show-Verdict "FAIL" "the computer cannot see ANY plugged-in device"
    Write-Host "  Try these in order:" -ForegroundColor Yellow
    Write-Host "   1. Push the lidar's USB plug in fully. Try a different USB socket."
    Write-Host "   2. Unplug it, count to five, plug it back in."
    Write-Host "   3. Connect the computer to the internet and replug once more"
    Write-Host "      (Windows fetches the driver by itself the first time -"
    Write-Host "      give it a full minute)."
    Write-Host "   4. Run this test again."
    Write-Host ""
    Show-PhotoNote
    Save-KitLog -Title "TEST 1 D6 LIDAR" -Verdict "FAIL"
    return "FAIL"
  }

  Add-KitLog ("COM ports seen: " + (($cands | ForEach-Object { $_.Port + " = " + $_.Name }) -join " | "))
  Write-Host ("  Found " + $cands.Count + " device(s):")
  foreach ($c in $cands) { Write-Host ("    " + $c.Port + "  " + $c.Name) }
  Write-Host ""

  $port = $null
  $portName = $null
  foreach ($c in $cands) {
    Write-Host ("  Checking " + $c.Port + " ...") -NoNewline
    $probe = Read-PortSample -PortName $c.Port -Baud $D6_BAUD -Ms 1800 -Poke $D6_START
    if (-not $probe.Ok) {
      Write-Host " cannot open (in use by another program?)" -ForegroundColor Yellow
      Add-KitLog ("probe " + $c.Port + ": could not open - " + $probe.Error)
      continue
    }
    $d6  = Get-D6Score $probe.Bytes
    $nm  = Get-NmeaScore $probe.Bytes
    Add-KitLog ("probe " + $c.Port + ": " + $probe.Bytes.Length + " bytes, AA55=" + $d6 + ", nmea-starts=" + $nm)
    if ($probe.Bytes.Length -eq 0) {
      Write-Host " silent" -ForegroundColor DarkGray
    } elseif ($d6 -ge 5 -and $d6 -gt $nm) {
      Write-Host " THIS IS THE LIDAR" -ForegroundColor Green
      $portName = $c.Port
      break
    } elseif ($nm -gt 0) {
      Write-Host " this is the GPS, not the lidar" -ForegroundColor DarkGray
    } else {
      Write-Host " some other device" -ForegroundColor DarkGray
    }
  }

  if (-not $portName) {
    Show-Verdict "FAIL" "the lidar did not answer on any port"
    Write-Host "  Most likely causes, in order:" -ForegroundColor Yellow
    Write-Host "   1. The lidar has no power - is its little adapter board plugged in?"
    Write-Host "   2. The USB cable is a CHARGE-ONLY cable. Swap it for a data cable."
    Write-Host "   3. Another program already has the lidar open. Close everything"
    Write-Host "      (especially any lidar viewer software) and run this again."
    Write-Host "   4. The lidar is not spinning. It should spin and may glow"
    Write-Host "      faintly red inside when powered."
    Write-Host ""
    Show-PhotoNote
    Save-KitLog -Title "TEST 1 D6 LIDAR" -Verdict "FAIL"
    return "FAIL"
  }

  Write-Host ""
  Write-Host ("  Using " + $portName) -ForegroundColor Green
  Add-KitLog ("port chosen: " + $portName)

  # --- capture --------------------------------------------------------------
  try {
    $port = New-SerialPort $portName $D6_BAUD
    $port.Open()
  } catch {
    Add-KitLog ("could not re-open " + $portName + ": " + $_.Exception.Message)
    Show-Verdict "FAIL" "another program grabbed the lidar"
    Write-Host "  Close every other window, then run this test again." -ForegroundColor Yellow
    Save-KitLog -Title "TEST 1 D6 LIDAR" -Verdict "FAIL"
    return "FAIL"
  }

  $port.Write($D6_START, 0, 4)
  $fs = [System.IO.File]::Open($binPath, [System.IO.FileMode]::Create)
  $buf = New-Object byte[] 8192
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $total = 0
  $lastShown = -1

  Write-Host ""
  Write-Host "  RECORDING. The lidar should be SPINNING now." -ForegroundColor Cyan
  Write-Host "  Do not touch anything until it finishes." -ForegroundColor Cyan
  Write-Host ""

  while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    try {
      $n = $port.Read($buf, 0, $buf.Length)
      if ($n -gt 0) { $fs.Write($buf, 0, $n); $total += $n }
    } catch [System.TimeoutException] { }
    catch { break }
    $sec = [int]$sw.Elapsed.TotalSeconds
    if ($sec -ne $lastShown) {
      $lastShown = $sec
      Write-Progress-Line ("  {0,3} / {1} seconds   {2,10:N0} bytes received" -f $sec, $Seconds, $total)
    }
  }
  Write-Host ""

  try { $port.Write($D6_STOP, 0, 4); Start-Sleep -Milliseconds 300 } catch { }
  try { $port.Close(); $port.Dispose() } catch { }
  $fs.Close()

  $elapsed = $sw.Elapsed.TotalSeconds
  $rate = 0
  if ($elapsed -gt 0) { $rate = $total / $elapsed }
  Add-KitLog ("bytes captured: " + $total + " in " + ("{0:N1}" -f $elapsed) + " s")
  Add-KitLog ("byte rate: " + ("{0:N0}" -f $rate) + " bytes/s  (healthy is about 11,500)")

  $bytes = [System.IO.File]::ReadAllBytes($binPath)
  $headers = Get-D6Score $bytes
  Add-KitLog ("packet headers (AA 55): " + $headers)
  if ($headers -gt 0) {
    Add-KitLog ("one header every " + ("{0:N0}" -f ($bytes.Length / $headers)) + " bytes (healthy is about 100-140)")
  }

  # PASS heuristics mirror verify_capture.py's quick D6 check.
  $pass = ($rate -ge 8000 -and $rate -le 16000 -and $headers -gt ($Seconds * 30))
  $warn = (-not $pass) -and ($total -gt 20000)

  if ($pass) {
    Show-Verdict "PASS" "THE LIDAR WORKS"
    Write-Host ("  Recorded " + ("{0:N0}" -f $total) + " bytes of real lidar data.") -ForegroundColor Green
    Write-Host ""
    Show-SendBackNote
    Save-KitLog -Title "TEST 1 D6 LIDAR" -Verdict "PASS"
    return "PASS"
  } elseif ($warn) {
    Show-Verdict "WARN" "some data arrived, but less than expected"
    Write-Host "  Please send the result anyway - it is still useful." -ForegroundColor Yellow
    Write-Host "  Also tell us: was the lidar spinning the whole time?" -ForegroundColor Yellow
    Write-Host ""
    Show-SendBackNote
    Save-KitLog -Title "TEST 1 D6 LIDAR" -Verdict "WARN"
    return "WARN"
  } else {
    Show-Verdict "FAIL" "the lidar sent almost nothing"
    Write-Host "  Check its power adapter board and the USB cable, then retry." -ForegroundColor Yellow
    Write-Host ""
    Show-PhotoNote
    Save-KitLog -Title "TEST 1 D6 LIDAR" -Verdict "FAIL"
    return "FAIL"
  }
}

# Run standalone if invoked directly rather than dot-sourced by the menu.
if ($MyInvocation.InvocationName -ne '.') {
  Invoke-D6Test -Seconds $Seconds | Out-Null
  Wait-Enter "Press the ENTER key to close this window"
}
