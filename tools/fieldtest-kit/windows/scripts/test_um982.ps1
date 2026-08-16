# LidarScan FIELD-TEST KIT v2 - test 3: Unicore UM982 RTK GNSS (Windows)
#
# The UM982 is a dual-antenna RTK receiver. On an evaluation board its UART
# is bridged to USB by a CH340 or a CP2102/CP2105 (check the chip marking on
# the board). Factory default output is NMEA 0183 at 115200 8N1; some boards
# ship at 460800. Because the COIN-D6 lidar may ALSO be a CH340, this test
# does not trust friendly names: it probes every COM port at every candidate
# baud rate and picks whichever one actually emits NMEA.
#
# IMPORTANT FOR THE VERDICT: this is a BENCH test with NO correction service
# (no NTRIP, no base station). A plain "SINGLE" fix - GGA quality 1 - is a
# PASS. RTK Fixed/Float only appear once corrections are fed in, which is a
# later test, not this one.
#
# Everything received is written to the file verbatim, including Unicore's
# proprietary sentences (#UNIHEADINGA and friends, which carry an 8-hex CRC32
# rather than the NMEA 2-hex XOR). They are counted and reported, never
# treated as corrupt.

param(
  [int]$Seconds = 90,
  [int]$Baud = 115200,
  [string]$Port = ""
)

$here = $PSScriptRoot
if (-not $here) { $here = Split-Path -Parent $MyInvocation.MyCommand.Definition }
. (Join-Path $here "common.ps1")

# 115200 is the documented default; 460800 is the common alternative on
# UM982 eval boards. The rest are long shots kept for a quiet board.
$UM982_BAUDS = @(115200, 460800, 9600, 38400, 230400)

function Find-Um982Port {
  param([int]$PreferredBaud = 115200)

  $cands = Get-SerialPortCandidates
  if ($cands.Count -eq 0) { return $null }

  Write-Host ("  Found " + $cands.Count + " device(s):")
  foreach ($c in $cands) { Write-Host ("    " + $c.Port + "  " + $c.Name) }
  Add-KitLog ("COM ports seen: " + (($cands | ForEach-Object { $_.Port + " = " + $_.Name }) -join " | "))
  Write-Host ""

  $bauds = @($PreferredBaud)
  foreach ($b in $UM982_BAUDS) { if ($b -ne $PreferredBaud) { $bauds += $b } }

  foreach ($c in $cands) {
    foreach ($b in $bauds) {
      Write-Host ("  Checking " + $c.Port + " at " + $b + " ...") -NoNewline
      $probe = Read-PortSample -PortName $c.Port -Baud $b -Ms 2500
      if (-not $probe.Ok) {
        Write-Host " cannot open (in use?)" -ForegroundColor Yellow
        Add-KitLog ("probe " + $c.Port + "@" + $b + ": could not open - " + $probe.Error)
        break
      }
      $text = [System.Text.Encoding]::ASCII.GetString($probe.Bytes)
      $nmea = ([regex]::Matches($text, "\$G[A-Z]{4},")).Count
      $uni  = ([regex]::Matches($text, "#[A-Z]{4,}[A-Z0-9]*,")).Count
      $d6   = Get-D6Score $probe.Bytes
      Add-KitLog ("probe " + $c.Port + "@" + $b + ": " + $probe.Bytes.Length +
                  " bytes, nmea=" + $nmea + ", unicore=" + $uni + ", AA55=" + $d6)
      if ($nmea -ge 2 -or $uni -ge 2) {
        Write-Host " THIS IS THE GPS" -ForegroundColor Green
        return (New-Object PSObject -Property @{ Port = $c.Port; Baud = $b; Name = $c.Name })
      }
      if ($d6 -ge 5) {
        Write-Host " this is the spinning lidar, not the GPS" -ForegroundColor DarkGray
        break
      }
      if ($probe.Bytes.Length -eq 0) { Write-Host " silent" -ForegroundColor DarkGray }
      else { Write-Host " noise" -ForegroundColor DarkGray }
    }
  }
  return $null
}

# ---------------------------------------------------------------------------
# NMEA stream statistics. Pulled out of the capture loop so it can be smoke
# tested against synthetic data without any hardware (see
# tools/fieldtest-kit/tests/).
# ---------------------------------------------------------------------------

function New-NmeaStats {
  return (New-Object PSObject -Property @{
    Sentences = 0        # NMEA 0183 '$' sentences
    CsOk      = 0
    CsBad     = 0
    Unicore   = 0        # Unicore proprietary '#...' ASCII logs (CRC32 tail)
    Heading   = 0        # any heading-bearing sentence, NMEA or proprietary
    BinarySync= 0        # Unicore binary log sync seen as text (AA 44 ...)
    Talkers   = @{}
    FixHist   = @{}
    LastFix   = "-"
    LastSats  = "-"
    BestFix   = -1
    MaxSats   = 0
  })
}

function Add-NmeaLine {
  param([Parameter(Mandatory=$true)]$Stats, [string]$Line)
  if (-not $Line) { return }
  $ln = $Line.Trim()
  if (-not $ln) { return }

  if ($ln[0] -eq '#') {
    # Unicore proprietary ASCII log. Its tail is an 8-hex CRC32, NOT the
    # NMEA 2-hex XOR, so it must never be counted as a bad checksum.
    $Stats.Unicore++
    if ($ln -match "HEADING|HDT|HPR") { $Stats.Heading++ }
    $idm = [regex]::Match($ln, "^#([A-Za-z0-9]+)")
    if ($idm.Success) {
      $k = $idm.Groups[1].Value
      $Stats.Talkers[$k] = [int]$Stats.Talkers[$k] + 1
    }
    return
  }
  if ($ln[0] -ne '$') { return }

  $Stats.Sentences++
  $ck = Test-NmeaChecksum $ln
  if ($ck -eq $true) { $Stats.CsOk++ } elseif ($ck -eq $false) { $Stats.CsBad++ }

  $star = $ln.IndexOf('*')
  $body = $ln
  if ($star -ge 0) { $body = $ln.Substring(0, $star) }
  $f = $body.Split(",")
  $sid = ""
  if ($f[0].Length -gt 1) { $sid = $f[0].Substring(1) }
  if ($sid) { $Stats.Talkers[$sid] = [int]$Stats.Talkers[$sid] + 1 }
  if ($sid -match "HDT|HDG|HPR|HEADING|THS") { $Stats.Heading++ }

  if ($sid.EndsWith("GGA") -and $f.Count -gt 7) {
    $q = $f[6]
    if ($q -eq "") { $q = "0" }
    $Stats.FixHist[$q] = [int]$Stats.FixHist[$q] + 1
    $Stats.LastFix = $q
    $qi = 0
    if ([int]::TryParse($q, [ref]$qi)) { if ($qi -gt $Stats.BestFix) { $Stats.BestFix = $qi } }
    $Stats.LastSats = $f[7]
    $si = 0
    if ([int]::TryParse($f[7], [ref]$si)) { if ($si -gt $Stats.MaxSats) { $Stats.MaxSats = $si } }
  }
}

# Returns PASS / WARN / FAIL for a completed UM982 bench capture.
# Deliberately: a SINGLE fix (GGA quality 1) with no corrections is a PASS.
function Get-Um982Verdict {
  param([Parameter(Mandatory=$true)]$Stats, [double]$Elapsed)
  if ($Elapsed -le 0) { $Elapsed = 1 }
  if ($Stats.Sentences -eq 0 -and $Stats.Unicore -eq 0) { return "FAIL" }
  $rate = $Stats.Sentences / $Elapsed
  $csTotal = $Stats.CsOk + $Stats.CsBad
  $csPct = 100.0
  if ($csTotal -gt 0) { $csPct = 100.0 * $Stats.CsOk / $csTotal }
  $streamOk = ($rate -ge 3.0)
  $csOkFlag = ($csTotal -eq 0) -or ($csPct -ge 99.0)
  if ($streamOk -and $csOkFlag -and $Stats.BestFix -ge 1) { return "PASS" }
  return "WARN"
}

function Invoke-Um982Test {
  param([int]$Seconds = 90, [int]$Baud = 115200, [string]$Port = "")

  Show-Banner "TEST 3 of 3 - GPS / RTK RECEIVER (Unicore UM982)" "Cyan"
  Write-Host "  BEFORE YOU START, check all three:" -ForegroundColor Yellow
  Write-Host "   1. At least ONE antenna is screwed onto the GPS board."
  Write-Host "      The socket marked ANT1 (or ANT/MAIN) is the one that"
  Write-Host "      matters. Finger tight is enough - do not force it."
  Write-Host "   2. The antenna is OUTSIDE, or on a windowsill with a clear"
  Write-Host "      view of the sky. Indoors in the middle of a room it will"
  Write-Host "      never find satellites."
  Write-Host "   3. The GPS board's USB cable is plugged into this computer."
  Write-Host ""
  Write-Host "  The first time a GPS is switched on in a new place it can take" -ForegroundColor Yellow
  Write-Host "  1-2 minutes to find satellites. That is normal." -ForegroundColor Yellow
  Write-Host ""
  Wait-Enter "When all three are done, press ENTER"

  Add-KitLog ("requested duration: " + $Seconds + " s, preferred baud: " + $Baud)

  Write-Host ""
  Write-Host "  Looking for the GPS receiver..." -ForegroundColor Cyan
  $target = $null
  if ($Port) {
    $target = New-Object PSObject -Property @{ Port = $Port; Baud = $Baud; Name = "(chosen by hand)" }
    Add-KitLog ("port forced by parameter: " + $Port + "@" + $Baud)
  } else {
    $target = Find-Um982Port -PreferredBaud $Baud
  }

  if (-not $target) {
    Show-Verdict "FAIL" "no GPS receiver found on this computer"
    Write-Host "  Try these in order:" -ForegroundColor Yellow
    Write-Host "   1. Push the GPS board's USB plug in fully; try another socket."
    Write-Host "   2. Is a light on the GPS board lit? If not it has no power."
    Write-Host "   3. Unplug it, count to five, plug it back in, wait a minute"
    Write-Host "      (Windows may be installing its driver), and retry."
    Write-Host "   4. If Windows never shows the board, it needs a USB driver."
    Write-Host "      Look at the small chip on the board next to the USB socket:"
    Write-Host "        marked CH340  -> Windows 11 has it built in; on Windows 10"
    Write-Host "                          connect to the internet and replug."
    Write-Host "        marked CP2102 -> tell us, we will send you the driver link."
    Write-Host "   5. Close any other GPS software (u-center, UPrecise) and retry."
    Write-Host ""
    Show-PhotoNote
    Save-KitLog -Title "TEST 3 GPS UM982" -Verdict "FAIL"
    return "FAIL"
  }

  Write-Host ""
  Write-Host ("  Using " + $target.Port + " at " + $target.Baud + " baud") -ForegroundColor Green
  Add-KitLog ("port chosen: " + $target.Port + " @ " + $target.Baud + " baud 8N1  (" + $target.Name + ")")

  $outDir  = Get-ResultDir
  $outPath = Join-Path $outDir ("um982_" + $Seconds + "s.nmea")
  Add-KitLog ("capture file: " + (Split-Path -Leaf $outPath))

  $sp = $null
  try {
    $sp = New-SerialPort $target.Port $target.Baud
    $sp.Open()
  } catch {
    Add-KitLog ("could not open port: " + $_.Exception.Message)
    Show-Verdict "FAIL" "another program is using the GPS"
    Write-Host "  Close every other window and run this test again." -ForegroundColor Yellow
    Save-KitLog -Title "TEST 3 GPS UM982" -Verdict "FAIL"
    return "FAIL"
  }

  $fs = [System.IO.File]::Open($outPath, [System.IO.FileMode]::Create)
  $buf = New-Object byte[] 8192
  $sw = [System.Diagnostics.Stopwatch]::StartNew()

  $partial      = ""
  $totalBytes   = 0
  $st           = New-NmeaStats
  $lastShown    = -1

  Write-Host ""
  Write-Host ("  LISTENING for " + $Seconds + " seconds. Leave everything alone.") -ForegroundColor Cyan
  Write-Host "  The line below updates every second:" -ForegroundColor Cyan
  Write-Host ""

  while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    try {
      $n = $sp.Read($buf, 0, $buf.Length)
      if ($n -gt 0) {
        $fs.Write($buf, 0, $n)
        $totalBytes += $n
        $partial += [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
        # Keep the tail (an unfinished line) and parse whole lines only.
        $lines = $partial -split "`r`n|`n|`r"
        $partial = $lines[$lines.Count - 1]
        for ($li = 0; $li -lt $lines.Count - 1; $li++) {
          Add-NmeaLine -Stats $st -Line $lines[$li]
        }
      }
    } catch [System.TimeoutException] { }
    catch { break }

    $sec = [int]$sw.Elapsed.TotalSeconds
    if ($sec -ne $lastShown) {
      $lastShown = $sec
      $fixTxt = Get-FixName $st.LastFix
      if ($st.LastFix -eq "-") { $fixTxt = "waiting..." }
      Write-Progress-Line ("  {0,3}/{1}s  sentences {2,6}  satellites {3,3}  fix: {4}" -f `
                            $sec, $Seconds, $st.Sentences, $st.LastSats, $fixTxt)
    }
  }
  Write-Host ""

  try { $sp.Close(); $sp.Dispose() } catch { }
  $fs.Close()

  $elapsed = $sw.Elapsed.TotalSeconds
  if ($elapsed -le 0) { $elapsed = 1 }
  $sentRate = $st.Sentences / $elapsed
  $csTotal = $st.CsOk + $st.CsBad
  $csPct = 0.0
  if ($csTotal -gt 0) { $csPct = 100.0 * $st.CsOk / $csTotal }
  $bestFixTxt = Get-FixName ([string]$st.BestFix)

  Add-KitLog ("duration: " + ("{0:N1}" -f $elapsed) + " s, bytes: " + $totalBytes)
  Add-KitLog ("NMEA sentences: " + $st.Sentences + "  (" + ("{0:N1}" -f $sentRate) + " per second)")
  Add-KitLog ("checksums: " + $st.CsOk + " good, " + $st.CsBad + " bad  (" + ("{0:N2}" -f $csPct) + "% good)")
  Add-KitLog ("Unicore proprietary lines (#...): " + $st.Unicore + "   heading-type sentences: " + $st.Heading)
  if ($st.Talkers.Count -gt 0) {
    $top = ($st.Talkers.GetEnumerator() | Sort-Object -Property Value -Descending |
            Select-Object -First 12 | ForEach-Object { $_.Key + "=" + $_.Value }) -join ", "
    Add-KitLog ("sentence types: " + $top)
  }
  if ($st.FixHist.Count -gt 0) {
    $fh = ($st.FixHist.GetEnumerator() | Sort-Object -Property Name |
           ForEach-Object { (Get-FixName $_.Key) + " x" + $_.Value }) -join " | "
    Add-KitLog ("fix histogram: " + $fh)
  } else {
    Add-KitLog "fix histogram: no GGA sentences seen"
  }
  Add-KitLog ("best fix reached: " + $bestFixTxt + ", most satellites: " + $st.MaxSats)
  Add-KitLog "note: no correction service (NTRIP/base) was used, so SINGLE is the expected best result here"

  Write-Host ""
  Write-Host "  ---- what the GPS said ----"
  Write-Host ("    sentences per second : " + ("{0:N1}" -f $sentRate))
  Write-Host ("    checksums good       : " + ("{0:N2}" -f $csPct) + "%")
  Write-Host ("    most satellites seen : " + $st.MaxSats)
  Write-Host ("    best fix reached     : " + $bestFixTxt)
  if ($st.Unicore -gt 0) {
    Write-Host ("    Unicore extra lines  : " + $st.Unicore + " (good - that is the dual-antenna data)")
  }
  Write-Host ""

  $verdict = Get-Um982Verdict -Stats $st -Elapsed $elapsed

  if ($verdict -eq "FAIL") {
    Show-Verdict "FAIL" "the GPS sent nothing we could read"
    Write-Host "  The port opened but no GPS text arrived at all." -ForegroundColor Yellow
    Write-Host "  It is probably set to a different speed. Tell us that and we"
    Write-Host "  will send you a version set to the right speed."
    Write-Host ""
    Show-PhotoNote
    Save-KitLog -Title "TEST 3 GPS UM982" -Verdict "FAIL"
    return "FAIL"
  }

  if ($verdict -eq "PASS") {
    Show-Verdict "PASS" "THE GPS WORKS"
    Write-Host ("  It is locked onto " + $st.MaxSats + " satellites and knows where it is.") -ForegroundColor Green
    Write-Host "  (It says SINGLE, not RTK. That is exactly right for this test -" -ForegroundColor Green
    Write-Host "   centimetre RTK needs a correction service we are not using yet.)" -ForegroundColor Green
    Write-Host ""
    Show-SendBackNote
    Save-KitLog -Title "TEST 3 GPS UM982" -Verdict "PASS"
    return "PASS"
  }

  # WARN: work out which of the two WARN stories to tell.
  $csOkFlag = ($csTotal -eq 0) -or ($csPct -ge 99.0)
  if ($csOkFlag -and $sentRate -ge 3.0) {
    Show-Verdict "WARN" "the GPS is alive but has not found satellites yet"
    Write-Host "  The receiver itself is working perfectly - it is talking to" -ForegroundColor Yellow
    Write-Host "  the computer and every message is clean. It just cannot see" -ForegroundColor Yellow
    Write-Host "  enough sky." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  To fix: put the antenna outdoors, or right against a window,"
    Write-Host "  wait two minutes, and run this test once more."
    Write-Host "  Send the result folder either way."
  } else {
    Show-Verdict "WARN" "the GPS data arrived damaged or too slowly"
    Write-Host "  Some messages arrived but they are garbled or sparse, which" -ForegroundColor Yellow
    Write-Host "  usually means the speed setting is not quite right or the USB" -ForegroundColor Yellow
    Write-Host "  cable is poor. Try a different USB cable and run it again." -ForegroundColor Yellow
    Write-Host "  Send the result folder either way - we can read the raw file."
  }
  Write-Host ""
  Show-SendBackNote
  Save-KitLog -Title "TEST 3 GPS UM982" -Verdict "WARN"
  return "WARN"
}

if ($MyInvocation.InvocationName -ne '.') {
  Invoke-Um982Test -Seconds $Seconds -Baud $Baud -Port $Port | Out-Null
  Wait-Enter "Press the ENTER key to close this window"
}
