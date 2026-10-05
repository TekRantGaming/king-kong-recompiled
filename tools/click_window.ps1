param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    # Click position relative to the window's top-left corner, in physical pixels.
    [Parameter(Mandatory = $true)][int]$X,
    [Parameter(Mandatory = $true)][int]$Y
)
# Dev helper: left-click inside a process's main window (used to drive the launcher in tests).
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WinClick {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[WinClick]::SetProcessDPIAware() | Out-Null
$h = (Get-Process -Id $ProcessId).MainWindowHandle
[WinClick]::SetForegroundWindow($h) | Out-Null
$r = New-Object WinClick+RECT
[WinClick]::GetWindowRect($h, [ref]$r) | Out-Null
[WinClick]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null
Start-Sleep -Milliseconds 250
[WinClick]::mouse_event(0x02, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 120
[WinClick]::mouse_event(0x04, 0, 0, 0, [UIntPtr]::Zero)
