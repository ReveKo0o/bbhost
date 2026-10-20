# gpumem.ps1 NAME SAMPLES INTERVAL: a process's GPU memory as Task Manager
# counts it (dedicated = VRAM, shared = system RAM the GPU maps), its working
# set and CPU seconds; one line per sample. Used by tools/win_perf.sh.
param([string]$Name = "bbhost", [int]$Samples = 1, [int]$Interval = 5)
for ($i = 0; $i -lt $Samples; $i++) {
  $p = Get-Process -Name $Name -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { Write-Output "$(Get-Date -Format HH:mm:ss) no $Name"; Start-Sleep $Interval; continue }
  $id = $p.Id
  $c = Get-Counter "\GPU Process Memory(pid_${id}_*)\Dedicated Usage","\GPU Process Memory(pid_${id}_*)\Shared Usage","\GPU Process Memory(pid_${id}_*)\Total Committed" -ErrorAction SilentlyContinue
  $d = 0; $s = 0; $t = 0
  foreach ($x in $c.CounterSamples) {
    if ($x.Path -like "*dedicated usage") { $d += $x.CookedValue }
    elseif ($x.Path -like "*shared usage") { $s += $x.CookedValue }
    else { $t += $x.CookedValue }
  }
  Write-Output ("{0} pid {1} dedicated {2} MiB shared {3} MiB committed {4} MiB; working set {5} MiB; cpu {6:N0} s" -f (Get-Date -Format HH:mm:ss), $id, [math]::Round($d/1MB), [math]::Round($s/1MB), [math]::Round($t/1MB), [math]::Round($p.WorkingSet64 / 1MB), $p.CPU)
  if ($i -lt $Samples - 1) { Start-Sleep $Interval }
}
