# keep_focus.ps1 -Name bbhost -Seconds 120: brings the process's window to the
# foreground every second for the run (tools/perf_pass_win.sh with
# PERF_FULLSCREEN=1). A test started from another program's console gets its
# window behind that program, and bbhost then takes no input - and AMD's
# driver resolves no occlusion query (KyoPS4x #220) - while it is unfocused.
param([string]$Name = "bbhost", [int]$Seconds = 120)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Fg {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
$shell = New-Object -ComObject WScript.Shell
for ($i = 0; $i -lt $Seconds; $i++) {
  $p = Get-Process -Name $Name -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
  if ($p -and [Fg]::GetForegroundWindow() -ne $p.MainWindowHandle) {
    # An Alt tap lets a background process take the foreground (the lock is
    # lifted for the process that last saw input).
    [Fg]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
    [Fg]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [void][Fg]::ShowWindow($p.MainWindowHandle, 9)
    [void][Fg]::SetForegroundWindow($p.MainWindowHandle)
    [void]$shell.AppActivate($p.Id)
  }
  Start-Sleep 1
}
