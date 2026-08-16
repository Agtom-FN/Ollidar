# LidarScan FIELD-TEST KIT v2 - main menu (Windows)
# Launched by START_TEST.bat. Nothing to install, nothing to type except a
# single digit.

# $kitDir, not $here: the dot-sourced scripts below each assign $here at
# script scope and would overwrite it mid-list.
$kitDir = $PSScriptRoot
if (-not $kitDir) { $kitDir = Split-Path -Parent $MyInvocation.MyCommand.Definition }
. (Join-Path $kitDir "common.ps1")
. (Join-Path $kitDir "test_d6.ps1")
. (Join-Path $kitDir "test_mid360.ps1")
. (Join-Path $kitDir "test_um982.ps1")

$D6_SECONDS     = 30
$MID360_SECONDS = 45
$UM982_SECONDS  = 90

try { $Host.UI.RawUI.WindowTitle = "LidarScan field test" } catch { }

function Show-Menu {
  Clear-Host
  Write-Host ""
  Write-Host "  ============================================================" -ForegroundColor Cyan
  Write-Host "      L I D A R S C A N   -   E Q U I P M E N T   T E S T" -ForegroundColor Cyan
  Write-Host "  ============================================================" -ForegroundColor Cyan
  Write-Host ""
  Write-Host "   Type a number and press ENTER."
  Write-Host ""
  Write-Host "     1   Small spinning lidar        (about half a minute)" -ForegroundColor White
  Write-Host "     2   Big round lidar             (about one minute)" -ForegroundColor White
  Write-Host "     3   GPS receiver                (about two minutes)" -ForegroundColor White
  Write-Host ""
  Write-Host "     4   TEST EVERYTHING  <-- pick this if unsure" -ForegroundColor Green
  Write-Host ""
  Write-Host "     5   Show what has been tested so far"
  Write-Host "     Q   Finish and close" -ForegroundColor Yellow
  Write-Host ""
  if ($Global:KitResults.Count -gt 0) {
    Write-Host "   Done so far:" -ForegroundColor DarkGray
    foreach ($r in $Global:KitResults) { Write-Host ("     " + $r) -ForegroundColor DarkGray }
    Write-Host ""
  }
}

function Show-Summary {
  Show-Banner "RESULTS SO FAR" "Cyan"
  if ($Global:KitResults.Count -eq 0) {
    Write-Host "  Nothing has been tested yet."
    Write-Host ""
    return
  }
  foreach ($r in $Global:KitResults) {
    $color = "Green"
    if ($r -match "WARN") { $color = "Yellow" }
    if ($r -match "FAIL") { $color = "Red" }
    Write-Host ("   " + $r) -ForegroundColor $color
  }
  Write-Host ""
  Write-Host ("  All of it is saved in:  " + (Get-ResultDir))
  Write-Host ""
  Show-SendBackNote
}

function Invoke-EverythingTest {
  Show-Banner "TESTING EVERYTHING - THREE TESTS, ONE AFTER THE OTHER" "Cyan"
  Write-Host "  Roughly four minutes in total. Each test explains itself."
  Write-Host "  If one device is not connected, that test will say so and the"
  Write-Host "  next one still runs."
  Write-Host ""
  Wait-Enter "Press ENTER to begin"

  Invoke-D6Test     -Seconds $D6_SECONDS     | Out-Null
  Wait-Enter "Press ENTER for the next test"
  Invoke-Mid360Test -Seconds $MID360_SECONDS | Out-Null
  Wait-Enter "Press ENTER for the next test"
  Invoke-Um982Test  -Seconds $UM982_SECONDS  | Out-Null

  Show-Summary
}

# ---------------------------------------------------------------------------

Show-Banner "WELCOME - THIS PROGRAM TESTS THE EQUIPMENT FOR YOU" "Cyan"
Write-Host "  Everything it finds is saved into one folder on your Desktop"
Write-Host "  called  LIDAR_TEST_RESULT  which you send back to us at the end."
Write-Host ""
Write-Host ("  Folder: " + (Get-ResultDir))
Write-Host ""
Wait-Enter "Press ENTER to see the menu"

while ($true) {
  Show-Menu
  $choice = Read-Host "   Your choice"
  if ($null -eq $choice) { $choice = "" }
  switch ($choice.Trim().ToUpper()) {
    "1" { Invoke-D6Test     -Seconds $D6_SECONDS     | Out-Null; Wait-Enter }
    "2" { Invoke-Mid360Test -Seconds $MID360_SECONDS | Out-Null; Wait-Enter }
    "3" { Invoke-Um982Test  -Seconds $UM982_SECONDS  | Out-Null; Wait-Enter }
    "4" { Invoke-EverythingTest; Wait-Enter }
    "5" { Show-Summary; Wait-Enter }
    "Q" {
      Show-Banner "FINISHED - ONE LAST STEP" "Green"
      Show-Summary
      Write-Host ("  Send this whole folder back:  " + (Get-ResultDir)) -ForegroundColor Yellow
      Write-Host ""
      Wait-Enter "Press ENTER to close this window"
      exit 0
    }
    default {
      Write-Host ""
      Write-Host "   Please type 1, 2, 3, 4, 5 or Q and press ENTER." -ForegroundColor Yellow
      Start-Sleep -Seconds 2
    }
  }
}
