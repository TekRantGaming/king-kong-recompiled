param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    # Keys to tap in order: Win32 virtual-key names (Return, Space, Left, Up, Escape, Back, A..Z, D0..D9).
    [Parameter(Mandatory = $true)][string[]]$Keys,
    [int]$HoldMs = 120,
    [int]$GapMs = 400
)
Add-Type -AssemblyName System.Windows.Forms
# Dev helper: taps keys in a process's main window (used to drive the game for screenshots).
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WinKeys {
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public static void Key(ushort vk, bool up) {
        var i = new INPUT[1];
        i[0].type = 1;
        i[0].ki.wVk = vk;
        i[0].ki.wScan = (ushort)MapVirtualKey(vk, 0);
        uint flags = 0x0008;  // KEYEVENTF_SCANCODE
        if (vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28) flags |= 0x0001;  // extended (arrows)
        if (up) flags |= 0x0002;
        i[0].ki.dwFlags = flags;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
$h = (Get-Process -Id $ProcessId).MainWindowHandle
[WinKeys]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 200
foreach ($k in $Keys) {
    $vk = [UInt16][System.Windows.Forms.Keys]::Parse([System.Windows.Forms.Keys], $k)
    [WinKeys]::Key($vk, $false); Start-Sleep -Milliseconds $HoldMs
    [WinKeys]::Key($vk, $true); Start-Sleep -Milliseconds $GapMs
}
