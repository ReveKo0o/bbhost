# perfmon.ps1 -Name bbhost -Seconds 60 [-Out file]: a process's GPU use as
# Windows Task Manager counts it, once a second - the 3D, compute and copy
# engines' utilisation (summed over the process's engines of each type), its
# dedicated (VRAM) and shared (system RAM the GPU maps) memory, working set and
# CPU. One line a second:
#   t=12 pid=4242 3d=41.2 compute=3.1 copy=0.4 dedicated=2310 shared=5402 ws=6120 cpu=212.5
# (memory in MiB, cpu in % of one core over the second; t in seconds since the
# process started). Waits up to -WaitStart seconds for the process to appear;
# follows the one of that name with the largest working set. Used by
# tools/perf_pass_win.sh; works for any process (shadps4 too).
param([string]$Name = "bbhost", [int]$Seconds = 60, [int]$WaitStart = 600, [string]$Out = "")
$ErrorActionPreference = "SilentlyContinue"
function Pick { Get-Process -Name $Name | Sort-Object WorkingSet64 -Descending | Select-Object -First 1 }
$p = $null
for ($i = 0; $i -lt $WaitStart -and -not $p; $i++) {
  $p = Pick
  if (-not $p) { Start-Sleep 1 }
}
if (-not $p) { Write-Output "perfmon: no process $Name"; exit 1 }
$prev = @{}
for ($s = 0; $s -lt $Seconds; $s++) {
  $p = Pick
  if (-not $p) { break }
  $id = $p.Id
  $eng = Get-Counter -Counter "\GPU Engine(pid_${id}_*)\Utilization Percentage" -SampleInterval 1 -MaxSamples 1
  $mem = Get-Counter -Counter "\GPU Process Memory(pid_${id}_*)\Dedicated Usage","\GPU Process Memory(pid_${id}_*)\Shared Usage"
  $e3d = 0.0; $ecomp = 0.0; $ecopy = 0.0; $ded = 0.0; $sh = 0.0
  if ($eng) {
    foreach ($x in $eng.CounterSamples) {
      $inst = $x.InstanceName
      if ($inst -like "*engtype_3d*") { $e3d += $x.CookedValue }
      elseif ($inst -like "*engtype_compute*") { $ecomp += $x.CookedValue }
      elseif ($inst -like "*engtype_copy*") { $ecopy += $x.CookedValue }
    }
  }
  if ($mem) {
    foreach ($x in $mem.CounterSamples) {
      if ($x.Path -like "*dedicated usage") { $ded += $x.CookedValue }
      elseif ($x.Path -like "*shared usage") { $sh += $x.CookedValue }
    }
  }
  $now = Get-Date
  $cpuMs = $p.TotalProcessorTime.TotalMilliseconds
  $cpu = 0.0
  if ($prev.ContainsKey($id)) {
    $dt = ($now - $prev[$id][1]).TotalMilliseconds
    if ($dt -gt 0) { $cpu = 100.0 * ($cpuMs - $prev[$id][0]) / $dt }
  }
  $prev[$id] = @($cpuMs, $now)
  $age = [math]::Round(($now - $p.StartTime).TotalSeconds, 1)
  $line = "t={0} pid={1} 3d={2:N1} compute={3:N1} copy={4:N1} dedicated={5} shared={6} ws={7} cpu={8:N1}" -f $age, $id, $e3d, $ecomp, $ecopy, [math]::Round($ded/1MB), [math]::Round($sh/1MB), [math]::Round($p.WorkingSet64/1MB), $cpu
  if ($Out) { Add-Content -Path $Out -Value $line } else { Write-Output $line }
}
