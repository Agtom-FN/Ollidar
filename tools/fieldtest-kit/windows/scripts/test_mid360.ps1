# LidarScan FIELD-TEST KIT v2 - test 2: Livox Mid-360 (Windows)
#
# What it does:
#   1. checks that some Ethernet adapter has a 192.168.1.x address
#      - if not: prints idiot-proof Windows Settings steps AND offers to set
#        192.168.1.50 / 255.255.255.0 automatically via an elevated netsh
#        (Start-Process -Verb RunAs). Declining is fine and non-fatal.
#   2. pings 192.168.1.100-199 in parallel to find the lidar (its address is
#      192.168.1.1XX where XX comes from the unit's serial number)
#   3. binds the Mid-360 host UDP ports and records every datagram verbatim
#      into a .livoxdump container - byte-identical framing to
#      tools/remote-capture/capture_mid360.py (magic LX360CAP) so the dev-side
#      verify_capture.py reads it with no changes
#   4. live per-port packet counters, then a verdict on the point-port rate
#
# Port sets: Livox's own samples use device 56100/56200/56300/56400/56500 and
# host 56101/56201/56301/56401/56501 (engine/docs/A3-mid360-driver.md "Ports"),
# but tools/remote-capture/capture_mid360.py binds the 561xx00 set. Which one
# a given unit streams to depends on what was pushed into it, so this test
# binds BOTH sets - ten ports - and lets the counters say which is live.
#
# NOTE: this test only LISTENS. A Mid-360 does not discover its host, it is
# TOLD where to stream (SDK2 0x0100 config push). If the unit has never been
# pointed at this PC, run Livox Viewer 2 once to start the stream, QUIT it
# (it holds the ports), then run this test.

param(
  [int]$Seconds = 45,
  [string]$Ports = "56100,56101,56200,56201,56300,56301,56400,56401,56500,56501"
)

$here = $PSScriptRoot
if (-not $here) { $here = Split-Path -Parent $MyInvocation.MyCommand.Definition }
. (Join-Path $here "common.ps1")

$HOST_IP_WANTED = "192.168.1.50"
$NETMASK        = "255.255.255.0"

# ---------------------------------------------------------------------------
# Network setup
# ---------------------------------------------------------------------------

function Get-LidarSubnetAddresses {
  $found = @()
  try {
    $found = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
               Where-Object { $_.IPAddress -like "192.168.1.*" })
  } catch {
    # Very old boxes without the NetTCPIP module: fall back to WMI.
    try {
      $cfgs = @(Get-CimInstance Win32_NetworkAdapterConfiguration -ErrorAction SilentlyContinue |
                Where-Object { $_.IPEnabled -eq $true })
      foreach ($c in $cfgs) {
        foreach ($ip in @($c.IPAddress)) {
          if ($ip -and $ip -like "192.168.1.*") {
            $found += (New-Object PSObject -Property @{ IPAddress = $ip; InterfaceAlias = $c.Description })
          }
        }
      }
    } catch { }
  }
  return $found
}

function Get-WiredAdapters {
  try {
    return @(Get-NetAdapter -Physical -ErrorAction SilentlyContinue |
             Where-Object { $_.InterfaceDescription -notmatch "Wi-?Fi|Wireless|802\.11|Bluetooth|Virtual|Loopback|VPN|TAP|Hyper-V" })
  } catch { return @() }
}

function Show-ManualStaticIpSteps {
  Write-Host ""
  Write-Host "  HOW TO SET THE NETWORK ADDRESS BY HAND (2 minutes):" -ForegroundColor Yellow
  Write-Host "   1. Click the Windows Start button, type:  settings   and press ENTER"
  Write-Host "   2. Click  Network & internet"
  Write-Host "   3. Click  Ethernet"
  Write-Host "   4. Find the line  IP assignment  and click  Edit"
  Write-Host "   5. Change the dropdown from  Automatic (DHCP)  to  Manual"
  Write-Host "   6. Switch  IPv4  to  On"
  Write-Host "   7. Type these EXACTLY:"
  Write-Host ("        IP address    " + $HOST_IP_WANTED) -ForegroundColor White
  Write-Host ("        Subnet mask   " + $NETMASK) -ForegroundColor White
  Write-Host "        Gateway       (leave empty)" -ForegroundColor White
  Write-Host "   8. Click  Save"
  Write-Host "   9. Come back here and run this test again."
  Write-Host ""
  Write-Host "  AFTERWARDS (important): to get normal internet back on that" -ForegroundColor Yellow
  Write-Host "  cable, repeat steps 1-5 and set it back to  Automatic (DHCP)." -ForegroundColor Yellow
  Write-Host ""
}

function Set-StaticIpElevated {
  $adapters = Get-WiredAdapters
  if ($adapters.Count -eq 0) {
    Write-Host "  No wired Ethernet adapter found on this computer." -ForegroundColor Red
    Write-Host "  The Mid-360 needs a cabled Ethernet connection. If you are"
    Write-Host "  using a USB-to-Ethernet adapter, plug it in and try again."
    Add-KitLog "auto-IP: no wired adapter found"
    return $false
  }
  $chosen = $adapters[0]
  if ($adapters.Count -gt 1) {
    Write-Host ""
    Write-Host "  Which network socket is the lidar cable plugged into?" -ForegroundColor Yellow
    for ($i = 0; $i -lt $adapters.Count; $i++) {
      Write-Host ("   [" + ($i + 1) + "] " + $adapters[$i].Name + "  -  " + $adapters[$i].InterfaceDescription +
                  "  (" + $adapters[$i].Status + ")")
    }
    $sel = Read-Host ("Type a number 1-" + $adapters.Count + " and press ENTER")
    $idx = 0
    if ([int]::TryParse($sel, [ref]$idx) -and $idx -ge 1 -and $idx -le $adapters.Count) {
      $chosen = $adapters[$idx - 1]
    } else {
      Write-Host "  Not a valid number - using the first one." -ForegroundColor Yellow
    }
  }

  Write-Host ""
  Write-Host ("  Setting " + $chosen.Name + " to " + $HOST_IP_WANTED + " ...") -ForegroundColor Cyan
  Write-Host "  Windows will show a blue 'Do you want to allow...' box." -ForegroundColor Yellow
  Write-Host "  Click YES. (It only changes this one network socket.)" -ForegroundColor Yellow
  Add-KitLog ("auto-IP: netsh static " + $HOST_IP_WANTED + " on adapter '" + $chosen.Name + "'")

  $argList = @("interface", "ip", "set", "address", ("name=" + $chosen.Name),
               "static", $HOST_IP_WANTED, $NETMASK)
  try {
    $p = Start-Process -FilePath "netsh.exe" -ArgumentList $argList -Verb RunAs -Wait -PassThru
    Add-KitLog ("auto-IP: netsh exit code " + $p.ExitCode)
  } catch {
    Write-Host "  You clicked No, or Windows blocked it." -ForegroundColor Yellow
    Add-KitLog ("auto-IP: declined/blocked - " + $_.Exception.Message)
    return $false
  }

  Start-Sleep -Seconds 3
  $now = Get-LidarSubnetAddresses
  if ($now.Count -gt 0) {
    Write-Host ("  Done. This computer is now " + $now[0].IPAddress) -ForegroundColor Green
    Write-Host ""
    Write-Host "  REMEMBER: after all testing, set that network socket back to" -ForegroundColor Yellow
    Write-Host "  Automatic (DHCP) or that cable will not give you internet." -ForegroundColor Yellow
    Write-Host ("  (Undo command: netsh interface ip set address name=""" + $chosen.Name + """ dhcp)")
    Add-KitLog ("auto-IP: success, host now " + $now[0].IPAddress)
    Add-KitLog ("auto-IP: undo with -> netsh interface ip set address name=""" + $chosen.Name + """ dhcp")
    return $true
  }
  Write-Host "  The address did not take effect." -ForegroundColor Red
  Add-KitLog "auto-IP: address did not take effect"
  return $false
}

function Find-LidarOnSubnet {
  # Parallel ICMP sweep of 192.168.1.100-199; the Mid-360 lives at
  # 192.168.1.1XX (XX from the serial number). ~1.5 s total.
  $hits = @()
  try {
    $tasks = @()
    $ips = @()
    for ($i = 100; $i -le 199; $i++) {
      $ip = "192.168.1." + $i
      $ips += $ip
      $p = New-Object System.Net.NetworkInformation.Ping
      $tasks += $p.SendPingAsync($ip, 900)
    }
    [System.Threading.Tasks.Task]::WaitAll($tasks, 4000) | Out-Null
    for ($i = 0; $i -lt $tasks.Count; $i++) {
      $t = $tasks[$i]
      if ($t.Status -eq [System.Threading.Tasks.TaskStatus]::RanToCompletion -and
          $t.Result -and $t.Result.Status -eq [System.Net.NetworkInformation.IPStatus]::Success) {
        $hits += $ips[$i]
      }
    }
  } catch { }
  return $hits
}

# ---------------------------------------------------------------------------
# The UDP capture engine
#
# Fast path: a tiny C# receiver compiled on the fly with Add-Type (one thread
# per port, no per-datagram PowerShell overhead). A healthy Mid-360 sends
# ~2000 datagrams/s and plain PowerShell cannot keep up with that reliably,
# which would produce a false FAIL.
# Fallback: if Add-Type is unavailable (locked-down machine, blocked TEMP),
# a pure-PowerShell Socket.Select loop, with the log saying so, because its
# numbers may understate the true rate.
# ---------------------------------------------------------------------------

$CSharpReceiver = @'
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;

public class LivoxCap
{
    public int[]  Ports;
    public long[] Pkts;
    public long[] Bytes;
    public string Error = "";

    Socket[]   socks;
    Thread[]   threads;
    FileStream fs;
    readonly object wlock = new object();
    volatile bool running = false;

    public bool Open(string hostIp, int[] ports, string outPath)
    {
        Ports = ports;
        Pkts  = new long[ports.Length];
        Bytes = new long[ports.Length];
        IPAddress addr = IPAddress.Any;
        if (!string.IsNullOrEmpty(hostIp) && hostIp != "0.0.0.0")
        {
            if (!IPAddress.TryParse(hostIp, out addr)) addr = IPAddress.Any;
        }
        socks = new Socket[ports.Length];
        try
        {
            for (int i = 0; i < ports.Length; i++)
            {
                Socket s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
                s.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                try { s.ReceiveBufferSize = 8 * 1024 * 1024; } catch { }
                s.Bind(new IPEndPoint(addr, ports[i]));
                s.ReceiveTimeout = 300;
                socks[i] = s;
            }
        }
        catch (Exception e) { Error = e.Message; CloseSockets(); return false; }

        try { fs = new FileStream(outPath, FileMode.Create, FileAccess.Write, FileShare.Read, 1 << 20); }
        catch (Exception e) { Error = e.Message; CloseSockets(); return false; }

        // .livoxdump header: magic "LX360CAP", u16 version=1, u16 num_ports,
        // then num_ports x u32 LE port numbers.
        byte[] magic = System.Text.Encoding.ASCII.GetBytes("LX360CAP");
        fs.Write(magic, 0, 8);
        fs.Write(BitConverter.GetBytes((ushort)1), 0, 2);
        fs.Write(BitConverter.GetBytes((ushort)ports.Length), 0, 2);
        for (int i = 0; i < ports.Length; i++)
            fs.Write(BitConverter.GetBytes((uint)ports[i]), 0, 4);
        return true;
    }

    public void Start()
    {
        running = true;
        threads = new Thread[socks.Length];
        for (int i = 0; i < socks.Length; i++)
        {
            int idx = i;
            threads[i] = new Thread(delegate() { Worker(idx); });
            threads[i].IsBackground = true;
            threads[i].Start();
        }
    }

    void Worker(int i)
    {
        byte[] buf = new byte[65535];
        byte[] rec = new byte[14];
        EndPoint ep = new IPEndPoint(IPAddress.Any, 0);
        while (running)
        {
            int n;
            try { n = socks[i].ReceiveFrom(buf, 0, buf.Length, SocketFlags.None, ref ep); }
            catch (SocketException) { continue; }
            catch (ObjectDisposedException) { break; }
            catch (Exception) { break; }
            if (n <= 0) continue;
            // record: u64 LE t_ns (unix epoch), u16 LE port_idx, u32 LE len, payload
            long tns = (DateTime.UtcNow.Ticks - 621355968000000000L) * 100L;
            Buffer.BlockCopy(BitConverter.GetBytes(tns), 0, rec, 0, 8);
            Buffer.BlockCopy(BitConverter.GetBytes((ushort)i), 0, rec, 8, 2);
            Buffer.BlockCopy(BitConverter.GetBytes((uint)n), 0, rec, 10, 4);
            lock (wlock)
            {
                if (fs == null) break;
                fs.Write(rec, 0, 14);
                fs.Write(buf, 0, n);
            }
            Interlocked.Increment(ref Pkts[i]);
            Interlocked.Add(ref Bytes[i], n);
        }
    }

    void CloseSockets()
    {
        if (socks == null) return;
        for (int i = 0; i < socks.Length; i++)
        {
            if (socks[i] != null) { try { socks[i].Close(); } catch { } }
        }
    }

    public void Stop()
    {
        running = false;
        CloseSockets();
        if (threads != null)
            foreach (Thread t in threads) { try { t.Join(1000); } catch { } }
        lock (wlock)
        {
            if (fs != null) { try { fs.Flush(); fs.Close(); } catch { } fs = null; }
        }
    }
}
'@

function Invoke-Mid360Capture {
  param(
    [Parameter(Mandatory=$true)][int[]]$PortList,
    [Parameter(Mandatory=$true)][string]$OutPath,
    [Parameter(Mandatory=$true)][int]$Seconds,
    [string]$HostIp = "0.0.0.0"
  )

  $result = New-Object PSObject -Property @{
    Ok = $false; Error = ""; Engine = ""; Elapsed = 0.0
    Pkts = (New-Object 'long[]' $PortList.Count)
    Bytes = (New-Object 'long[]' $PortList.Count)
  }

  $cap = $null
  try {
    # LIDARSCAN_KIT_FORCE_FALLBACK=1 exercises the slow path in the smoke test.
    if ($env:LIDARSCAN_KIT_FORCE_FALLBACK -eq "1") { throw "forced fallback" }
    if (-not ([System.Management.Automation.PSTypeName]'LivoxCap').Type) {
      Add-Type -TypeDefinition $CSharpReceiver -ErrorAction Stop
    }
    $cap = New-Object LivoxCap
    $result.Engine = "fast"
  } catch {
    $result.Engine = "fallback"
    Add-KitLog ("fast receiver unavailable (" + $_.Exception.Message + ") - using the slower PowerShell receiver; packet counts may be understated")
  }

  $sw = [System.Diagnostics.Stopwatch]::StartNew()

  if ($result.Engine -eq "fast") {
    if (-not $cap.Open($HostIp, $PortList, $OutPath)) {
      $result.Error = $cap.Error
      return $result
    }
    $cap.Start()
    $lastShown = -1
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
      Start-Sleep -Milliseconds 250
      $sec = [int]$sw.Elapsed.TotalSeconds
      if ($sec -ne $lastShown) {
        $lastShown = $sec
        $tot = 0
        foreach ($v in $cap.Pkts) { $tot += $v }
        $best = Get-BusiestPort -PortList $PortList -Pkts $cap.Pkts -Elapsed $sw.Elapsed.TotalSeconds
        Write-Progress-Line ("  {0,3} / {1} s   {2,10:N0} packets   busiest port {3} ({4:N0}/s)" -f `
                              $sec, $Seconds, $tot, $best.Port, $best.Rate)
      }
    }
    Write-Host ""
    $cap.Stop()
    $result.Elapsed = $sw.Elapsed.TotalSeconds
    for ($i = 0; $i -lt $PortList.Count; $i++) {
      $result.Pkts[$i]  = $cap.Pkts[$i]
      $result.Bytes[$i] = $cap.Bytes[$i]
    }
    $result.Ok = $true
    return $result
  }

  # ---- fallback: pure PowerShell -------------------------------------------
  $socks = @()
  try {
    foreach ($p in $PortList) {
      $s = New-Object System.Net.Sockets.Socket ([System.Net.Sockets.AddressFamily]::InterNetwork),
                                                ([System.Net.Sockets.SocketType]::Dgram),
                                                ([System.Net.Sockets.ProtocolType]::Udp)
      $s.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket,
                         [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
      try { $s.ReceiveBufferSize = 8388608 } catch { }
      $addr = [System.Net.IPAddress]::Any
      if ($HostIp -and $HostIp -ne "0.0.0.0") { $addr = [System.Net.IPAddress]::Parse($HostIp) }
      $s.Bind((New-Object System.Net.IPEndPoint $addr, $p))
      $socks += $s
    }
  } catch {
    $result.Error = $_.Exception.Message
    foreach ($s in $socks) { try { $s.Close() } catch { } }
    return $result
  }

  $fs = [System.IO.File]::Open($OutPath, [System.IO.FileMode]::Create)
  $bw = New-Object System.IO.BinaryWriter $fs
  $bw.Write([System.Text.Encoding]::ASCII.GetBytes("LX360CAP"))
  $bw.Write([uint16]1)
  $bw.Write([uint16]$PortList.Count)
  foreach ($p in $PortList) { $bw.Write([uint32]$p) }

  $buf = New-Object byte[] 65535
  $anyEp = [System.Net.EndPoint](New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Any), 0)
  $epoch = New-Object DateTime 1970, 1, 1, 0, 0, 0, ([DateTimeKind]::Utc)
  $lastShown = -1
  while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    $check = New-Object System.Collections.ArrayList
    foreach ($s in $socks) { $check.Add($s) | Out-Null }
    [System.Net.Sockets.Socket]::Select($check, $null, $null, 200000)
    foreach ($s in $check) {
      $idx = [array]::IndexOf($socks, $s)
      while ($s.Available -gt 0) {
        $n = $s.ReceiveFrom($buf, [ref]$anyEp)
        if ($n -le 0) { break }
        $tns = [long](([DateTime]::UtcNow - $epoch).Ticks) * 100
        $bw.Write([uint64]$tns)
        $bw.Write([uint16]$idx)
        $bw.Write([uint32]$n)
        $bw.Write($buf, 0, $n)
        $result.Pkts[$idx]++
        $result.Bytes[$idx] += $n
      }
    }
    $sec = [int]$sw.Elapsed.TotalSeconds
    if ($sec -ne $lastShown) {
      $lastShown = $sec
      $tot = 0
      foreach ($v in $result.Pkts) { $tot += $v }
      $best = Get-BusiestPort -PortList $PortList -Pkts $result.Pkts -Elapsed $sw.Elapsed.TotalSeconds
      Write-Progress-Line ("  {0,3} / {1} s   {2,10:N0} packets   busiest port {3} ({4:N0}/s)" -f `
                            $sec, $Seconds, $tot, $best.Port, $best.Rate)
    }
  }
  Write-Host ""
  $result.Elapsed = $sw.Elapsed.TotalSeconds
  try { $bw.Flush(); $bw.Close(); $fs.Close() } catch { }
  foreach ($s in $socks) { try { $s.Close() } catch { } }
  $result.Ok = $true
  return $result
}

function Get-BusiestPort {
  param([int[]]$PortList, $Pkts, [double]$Elapsed = 0)
  $bestI = 0
  for ($i = 0; $i -lt $PortList.Count; $i++) {
    if ($Pkts[$i] -gt $Pkts[$bestI]) { $bestI = $i }
  }
  $rate = 0.0
  if ($Elapsed -gt 0) { $rate = $Pkts[$bestI] / $Elapsed }
  return (New-Object PSObject -Property @{
    Index = $bestI; Port = $PortList[$bestI]; Count = $Pkts[$bestI]; Rate = $rate })
}

# ---------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------

function Invoke-Mid360Test {
  param([int]$Seconds = 45, [string]$Ports = "")

  if (-not $Ports) { $Ports = "56100,56101,56200,56201,56300,56301,56400,56401,56500,56501" }
  $portList = @()
  foreach ($p in $Ports.Split(",")) {
    $t = $p.Trim()
    if ($t) { $portList += [int]$t }
  }

  Show-Banner "TEST 2 of 3 - BIG ROUND LIDAR (Livox Mid-360)" "Cyan"
  Write-Host "  BEFORE YOU START, check all three:" -ForegroundColor Yellow
  Write-Host "   1. The round lidar has its 12 volt power connected and is"
  Write-Host "      making a faint whirring noise."
  Write-Host "   2. Its network cable goes into this computer's network socket"
  Write-Host "      (or into a USB-to-network adapter plugged into this computer)."
  Write-Host "   3. Nothing else, especially Livox Viewer, is open."
  Write-Host ""
  Wait-Enter "When all three are done, press ENTER"

  Add-KitLog ("ports bound: " + ($portList -join ","))

  # --- step 1: is this computer on the lidar's network? ---------------------
  Write-Host ""
  Write-Host "  Checking this computer's network address..." -ForegroundColor Cyan
  $mine = Get-LidarSubnetAddresses
  if ($mine.Count -gt 0) {
    foreach ($m in $mine) {
      Write-Host ("    OK - " + $m.InterfaceAlias + " is " + $m.IPAddress) -ForegroundColor Green
      Add-KitLog ("host address: " + $m.IPAddress + " on " + $m.InterfaceAlias)
    }
  } else {
    Write-Host "    This computer has NO address on the lidar's network." -ForegroundColor Red
    Add-KitLog "host address: none in 192.168.1.x"
    Write-Host ""
    Write-Host "  The lidar talks on addresses that start with  192.168.1." -ForegroundColor Yellow
    Write-Host ("  This computer must be given the address  " + $HOST_IP_WANTED) -ForegroundColor Yellow
    Write-Host ""
    $auto = Read-YesNo "  Shall I set it for you automatically?" $true
    if ($auto) {
      $ok = Set-StaticIpElevated
      if (-not $ok) {
        Show-ManualStaticIpSteps
        Write-Host "  Continuing anyway - the test below will most likely find nothing." -ForegroundColor Yellow
      }
    } else {
      Add-KitLog "auto-IP: tester declined"
      Show-ManualStaticIpSteps
      $again = Read-YesNo "  Have you set it by hand just now?" $false
      if (-not $again) {
        Show-Verdict "FAIL" "network address not set - cannot test this lidar yet"
        Write-Host "  Follow the steps above, then run this test again." -ForegroundColor Yellow
        Write-Host ""
        Show-PhotoNote
        Save-KitLog -Title "TEST 2 MID-360 LIDAR" -Verdict "FAIL"
        return "FAIL"
      }
      $mine = Get-LidarSubnetAddresses
      foreach ($m in $mine) { Add-KitLog ("host address: " + $m.IPAddress + " on " + $m.InterfaceAlias) }
    }
  }

  # --- step 2: can we see the lidar at all? --------------------------------
  Write-Host ""
  Write-Host "  Looking for the lidar on the network (takes a few seconds)..." -ForegroundColor Cyan
  $hits = Find-LidarOnSubnet
  if ($hits.Count -gt 0) {
    Write-Host ("    Found device(s) at: " + ($hits -join ", ")) -ForegroundColor Green
    Add-KitLog ("ping sweep 192.168.1.100-199: replies from " + ($hits -join ", "))
  } else {
    Write-Host "    No reply from any lidar address yet." -ForegroundColor Yellow
    Write-Host "    (Not fatal - some units stay quiet to ping. Carrying on.)"
    Add-KitLog "ping sweep 192.168.1.100-199: no replies"
  }

  # --- step 3: record ------------------------------------------------------
  $outDir  = Get-ResultDir
  $outPath = Join-Path $outDir ("mid360_" + $Seconds + "s.livoxdump")
  Add-KitLog ("capture file: " + (Split-Path -Leaf $outPath))

  Write-Host ""
  Write-Host ("  RECORDING for " + $Seconds + " seconds. Do not touch anything.") -ForegroundColor Cyan
  Write-Host ""

  $cap = Invoke-Mid360Capture -PortList $portList -OutPath $outPath -Seconds $Seconds
  Add-KitLog ("receiver: " + $cap.Engine)

  if (-not $cap.Ok) {
    Add-KitLog ("bind failed: " + $cap.Error)
    Show-Verdict "FAIL" "the computer could not open the lidar's network ports"
    Write-Host "  Almost always this means another program already has them." -ForegroundColor Yellow
    Write-Host "  Close Livox Viewer (and any other lidar software) completely,"
    Write-Host "  then run this test again."
    Write-Host ""
    Show-PhotoNote
    Save-KitLog -Title "TEST 2 MID-360 LIDAR" -Verdict "FAIL"
    return "FAIL"
  }

  # --- step 4: verdict -----------------------------------------------------
  $total = 0
  $totalBytes = 0
  $bestI = 0
  for ($i = 0; $i -lt $portList.Count; $i++) {
    $total += $cap.Pkts[$i]
    $totalBytes += $cap.Bytes[$i]
    if ($cap.Pkts[$i] -gt $cap.Pkts[$bestI]) { $bestI = $i }
  }
  $elapsed = $cap.Elapsed
  if ($elapsed -le 0) { $elapsed = $Seconds }

  Add-KitLog ("duration: " + ("{0:N1}" -f $elapsed) + " s")
  Add-KitLog ("total datagrams: " + $total + "   total bytes: " + $totalBytes)
  $live = @()
  for ($i = 0; $i -lt $portList.Count; $i++) {
    if ($cap.Pkts[$i] -gt 0) {
      $r = $cap.Pkts[$i] / $elapsed
      $live += ("port " + $portList[$i] + ": " + $cap.Pkts[$i] + " pkts (" + ("{0:N0}" -f $r) + "/s), " + $cap.Bytes[$i] + " bytes")
    }
  }
  if ($live.Count -eq 0) { Add-KitLog "no port received anything" }
  else { foreach ($l in $live) { Add-KitLog $l } }

  $pointRate = 0
  if ($elapsed -gt 0) { $pointRate = $cap.Pkts[$bestI] / $elapsed }
  Add-KitLog ("busiest port: " + $portList[$bestI] + " at " + ("{0:N0}" -f $pointRate) +
              " datagrams/s (healthy point stream is above 1,500/s)")

  Write-Host ""
  Write-Host "  ---- what arrived ----"
  for ($i = 0; $i -lt $portList.Count; $i++) {
    if ($cap.Pkts[$i] -gt 0) {
      Write-Host ("    port " + $portList[$i] + " : " + ("{0,9:N0}" -f $cap.Pkts[$i]) + " packets   " +
                  ("{0,7:N0}" -f ($cap.Pkts[$i] / $elapsed)) + " per second") -ForegroundColor Green
    }
  }
  if ($total -eq 0) { Write-Host "    (nothing at all)" -ForegroundColor Red }
  Write-Host ""

  if ($pointRate -ge 1500) {
    Show-Verdict "PASS" "THE BIG LIDAR WORKS"
    Write-Host ("  It sent " + ("{0:N0}" -f $total) + " packets of scan data.") -ForegroundColor Green
    Write-Host ""
    Show-SendBackNote
    Save-KitLog -Title "TEST 2 MID-360 LIDAR" -Verdict "PASS"
    return "PASS"
  } elseif ($total -gt 0) {
    Show-Verdict "WARN" "the lidar is talking, but slower than expected"
    Write-Host "  Data did arrive, so the cable and address are right." -ForegroundColor Yellow
    Write-Host "  It may be sending only status messages and not scan data yet."
    Write-Host "  Please send the result folder anyway - it tells us a lot."
    Write-Host ""
    Show-SendBackNote
    Save-KitLog -Title "TEST 2 MID-360 LIDAR" -Verdict "WARN"
    return "WARN"
  } else {
    Show-Verdict "FAIL" "the big lidar sent nothing"
    Write-Host "  Check these, in order:" -ForegroundColor Yellow
    Write-Host "   1. 12 volt power on the lidar. It should whirr faintly."
    Write-Host "   2. The network cable is clicked into BOTH ends."
    Write-Host "   3. This computer's address is 192.168.1.50 (see above)."
    Write-Host "   4. The lidar has to be TOLD to send to this computer. If it"
    Write-Host "      has never been connected to this PC before: open Livox"
    Write-Host "      Viewer 2 once, let it find the lidar, then CLOSE Viewer"
    Write-Host "      completely and run this test again."
    Write-Host ""
    Show-PhotoNote
    Save-KitLog -Title "TEST 2 MID-360 LIDAR" -Verdict "FAIL"
    return "FAIL"
  }
}

if ($MyInvocation.InvocationName -ne '.') {
  Invoke-Mid360Test -Seconds $Seconds -Ports $Ports | Out-Null
  Wait-Enter "Press the ENTER key to close this window"
}
