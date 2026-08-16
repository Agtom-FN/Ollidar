# LidarScan FIELD-TEST KIT v2 - shared helpers (Windows)
# Dot-sourced by menu.ps1 and by each test_*.ps1.
#
# Targets Windows PowerShell 5.1 (what `powershell.exe` is on Windows 10/11).
# Do NOT use PowerShell 7-only syntax here (no ?: ternary, no ??, no
# -Parallel): the launcher deliberately uses the in-box 5.1 so the tester
# never has to install anything.

Set-StrictMode -Off
$ErrorActionPreference = "Stop"

$Global:KitVersion = "2.0"

# Idempotent: every test_*.ps1 dot-sources this file too, and re-running the
# initialisers would throw away results already collected in this session.
if (-not $Global:KitCommonLoaded) {
  $Global:KitLog       = New-Object System.Collections.Generic.List[string]
  $Global:KitResults   = New-Object System.Collections.Generic.List[string]
  $Global:KitResultDir = $null
  $Global:KitCommonLoaded = $true
}

# ---------------------------------------------------------------------------
# Result folder + combined result file
# ---------------------------------------------------------------------------

function Get-ResultDir {
  if (-not $Global:KitResultDir) {
    $desktop = [Environment]::GetFolderPath("Desktop")
    if (-not $desktop) { $desktop = Join-Path $env:USERPROFILE "Desktop" }
    $dir = Join-Path $desktop "LIDAR_TEST_RESULT"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $Global:KitResultDir = $dir
  }
  return $Global:KitResultDir
}

function Get-ResultFile {
  return (Join-Path (Get-ResultDir) "TEST_RESULT.txt")
}

function Add-KitLog([string]$line) {
  $Global:KitLog.Add($line) | Out-Null
}

# Writes one clearly delimited block into the ONE combined TEST_RESULT.txt.
function Save-KitLog {
  param(
    [Parameter(Mandatory=$true)][string]$Title,
    [Parameter(Mandatory=$true)][ValidateSet("PASS","WARN","FAIL")][string]$Verdict
  )
  $bar = "=" * 66
  $block = New-Object System.Collections.Generic.List[string]
  $block.Add("") | Out-Null
  $block.Add($bar) | Out-Null
  $block.Add(("  " + $Title)) | Out-Null
  $block.Add(("  run at " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss") +
              "   kit v" + $Global:KitVersion)) | Out-Null
  $block.Add(("  computer " + $env:COMPUTERNAME + "   " +
              [Environment]::OSVersion.VersionString +
              "   PS " + $PSVersionTable.PSVersion.ToString())) | Out-Null
  $block.Add(("-" * 66)) | Out-Null
  foreach ($l in $Global:KitLog) { $block.Add("  " + $l) | Out-Null }
  $block.Add(("-" * 66)) | Out-Null
  $block.Add(("  RESULT: " + $Verdict)) | Out-Null
  $block.Add($bar) | Out-Null

  $file = Get-ResultFile
  if (-not (Test-Path $file)) {
    $head = @(
      "LidarScan FIELD TEST RESULTS",
      "Send this whole folder back to the LidarScan team.",
      ""
    )
    $head | Set-Content -Path $file -Encoding ASCII
  }
  $block | Add-Content -Path $file -Encoding ASCII

  $Global:KitResults.Add(("{0,-22} {1}" -f $Title, $Verdict)) | Out-Null
  $Global:KitLog.Clear()
}

# ---------------------------------------------------------------------------
# Screen output - big, plain, photographable
# ---------------------------------------------------------------------------

function Show-Banner([string]$text, [string]$color) {
  if (-not $color) { $color = "Cyan" }
  Write-Host ""
  Write-Host ("=" * 62) -ForegroundColor $color
  Write-Host ("   " + $text) -ForegroundColor $color
  Write-Host ("=" * 62) -ForegroundColor $color
  Write-Host ""
}

function Show-Verdict([string]$verdict, [string]$headline) {
  Write-Host ""
  if ($verdict -eq "PASS") {
    Show-Banner ("SUCCESS - " + $headline) "Green"
  } elseif ($verdict -eq "WARN") {
    Show-Banner ("PARTLY WORKED - " + $headline) "Yellow"
  } else {
    Show-Banner ("PROBLEM - " + $headline) "Red"
  }
}

function Show-SendBackNote {
  Write-Host "  WHAT TO DO NOW:" -ForegroundColor Yellow
  Write-Host "   Send the folder  LIDAR_TEST_RESULT  (it is on your Desktop)"
  Write-Host "   back the same way you received this test - WhatsApp, WeChat"
  Write-Host "   or email. Send the WHOLE folder, all the files in it."
  Write-Host ""
}

function Show-PhotoNote {
  Write-Host "  If you are stuck: take a PHOTO of this whole window with your" -ForegroundColor Yellow
  Write-Host "  phone and send the photo. That is always useful to us." -ForegroundColor Yellow
  Write-Host ""
}

function Wait-Enter([string]$prompt) {
  if (-not $prompt) { $prompt = "Press the ENTER key to continue" }
  Write-Host ""
  Read-Host $prompt | Out-Null
}

function Read-YesNo([string]$question, [bool]$defaultYes) {
  $suffix = " [y/n]"
  if ($defaultYes) { $suffix = " [Y/n]" } else { $suffix = " [y/N]" }
  while ($true) {
    $a = Read-Host ($question + $suffix)
    if (-not $a -or $a.Trim() -eq "") { return $defaultYes }
    $a = $a.Trim().ToLower()
    if ($a -eq "y" -or $a -eq "yes") { return $true }
    if ($a -eq "n" -or $a -eq "no")  { return $false }
    Write-Host "  Please type y or n." -ForegroundColor Yellow
  }
}

# Countdown/progress line that overwrites itself.
function Write-Progress-Line([string]$text) {
  $pad = $text
  if ($pad.Length -lt 78) { $pad = $pad + (" " * (78 - $pad.Length)) }
  Write-Host ("`r" + $pad) -NoNewline
}

# ---------------------------------------------------------------------------
# Serial port discovery (shared by the D6 and UM982 tests)
# ---------------------------------------------------------------------------

function Get-SerialPortCandidates {
  $seen = @{}
  $out  = New-Object System.Collections.Generic.List[object]
  try {
    $pnp = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -match "\(COM\d+\)" })
    foreach ($d in $pnp) {
      $m = [regex]::Match($d.Name, "COM\d+")
      if ($m.Success -and -not $seen.ContainsKey($m.Value)) {
        $seen[$m.Value] = $true
        $out.Add((New-Object PSObject -Property @{ Port = $m.Value; Name = $d.Name })) | Out-Null
      }
    }
  } catch { }
  try {
    foreach ($p in [System.IO.Ports.SerialPort]::GetPortNames()) {
      if (-not $seen.ContainsKey($p)) {
        $seen[$p] = $true
        $out.Add((New-Object PSObject -Property @{ Port = $p; Name = ($p + " (no device name)") })) | Out-Null
      }
    }
  } catch { }
  # numeric sort: COM3 before COM10
  return @($out | Sort-Object { [int]([regex]::Match($_.Port, "\d+").Value) })
}

function New-SerialPort([string]$portName, [int]$baud) {
  $sp = New-Object System.IO.Ports.SerialPort $portName, $baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
  # CH340 boards commonly wire DTR to a reset/enable line; the vendor SDK
  # clears it after opening, and so do we (matches d6cli and capture_d6.py).
  $sp.DtrEnable   = $false
  $sp.RtsEnable   = $false
  $sp.ReadTimeout = 250
  $sp.ReadBufferSize = 262144
  return $sp
}

# Reads up to $ms milliseconds of whatever a port emits. Returns a byte[].
# Used by both device probes to tell a D6 (binary AA 55 frames) apart from a
# UM982 (ASCII '$'/'#' sentences) without asking the tester anything.
function Read-PortSample {
  param(
    [Parameter(Mandatory=$true)][string]$PortName,
    [Parameter(Mandatory=$true)][int]$Baud,
    [int]$Ms = 2000,
    [byte[]]$Poke = $null
  )
  $result = New-Object PSObject -Property @{ Ok = $false; Bytes = (New-Object byte[] 0); Error = "" }
  $sp = $null
  try {
    $sp = New-SerialPort $PortName $Baud
    $sp.Open()
  } catch {
    $result.Error = $_.Exception.Message
    if ($sp) { try { $sp.Dispose() } catch { } }
    return $result
  }
  try {
    try { $sp.DiscardInBuffer() } catch { }
    if ($Poke -and $Poke.Length -gt 0) { $sp.Write($Poke, 0, $Poke.Length) }
    $ms = New-Object System.IO.MemoryStream
    $buf = New-Object byte[] 8192
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalMilliseconds -lt $Ms) {
      try {
        $n = $sp.Read($buf, 0, $buf.Length)
        if ($n -gt 0) { $ms.Write($buf, 0, $n) }
      } catch [System.TimeoutException] { }
      catch { break }
    }
    $result.Bytes = $ms.ToArray()
    $result.Ok = $true
  } finally {
    try { $sp.Close() } catch { }
    try { $sp.Dispose() } catch { }
  }
  return $result
}

function Get-D6Score([byte[]]$bytes) {
  # number of AA 55 header pairs; the D6 packet header on the wire
  $c = 0
  for ($i = 0; $i -lt $bytes.Length - 1; $i++) {
    if ($bytes[$i] -eq 0xAA -and $bytes[$i + 1] -eq 0x55) { $c++ }
  }
  return $c
}

function Get-NmeaScore([byte[]]$bytes) {
  # number of '$' or '#' line starts - NMEA 0183 and Unicore ASCII logs
  $c = 0
  for ($i = 0; $i -lt $bytes.Length; $i++) {
    if ($bytes[$i] -eq 0x24 -or $bytes[$i] -eq 0x23) { $c++ }
  }
  return $c
}

# ---------------------------------------------------------------------------
# NMEA helpers (used live by the UM982 test)
# ---------------------------------------------------------------------------

function Test-NmeaChecksum([string]$sentence) {
  # Returns $true / $false / $null (not checksummable).
  # NMEA 0183 = '$' + body + '*' + 2 hex XOR. Unicore ASCII logs start with
  # '#' and end with an 8-hex CRC32, which is NOT this - handled by caller.
  if (-not $sentence -or $sentence.Length -lt 4) { return $null }
  if ($sentence[0] -ne '$' -and $sentence[0] -ne '!') { return $null }
  $star = $sentence.LastIndexOf('*')
  if ($star -lt 1 -or ($star + 3) -gt $sentence.Length) { return $null }
  $claimed = $sentence.Substring($star + 1, 2)
  $val = 0
  if (-not [int]::TryParse($claimed, [System.Globalization.NumberStyles]::HexNumber,
                            [System.Globalization.CultureInfo]::InvariantCulture, [ref]$val)) { return $null }
  $computed = 0
  for ($i = 1; $i -lt $star; $i++) { $computed = $computed -bxor [int][char]$sentence[$i] }
  return ($computed -eq $val)
}

function Get-FixName([string]$q) {
  switch ($q) {
    "-1" { return "none reported" }
    "0" { return "NO FIX (needs sky view)" }
    "1" { return "SINGLE (normal GPS)" }
    "2" { return "DGPS" }
    "3" { return "PPS" }
    "4" { return "RTK FIXED (centimetre)" }
    "5" { return "RTK FLOAT (decimetre)" }
    "6" { return "dead reckoning" }
    "7" { return "manual" }
    "8" { return "simulated" }
    default { return ("quality " + $q) }
  }
}
