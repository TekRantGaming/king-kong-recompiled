param([Parameter(Mandatory=$true)][int]$GameId,
  [ValidateSet('shot','key','text','click','wheel')][string]$Action='shot',
  [string]$Value='', [int]$X=0, [int]$Y=0)
$ErrorActionPreference='Stop'
$game=Get-Process -Id $GameId
if ($game.ProcessName -ne 'king_kong' -or $game.Path -notlike 'F:\KK-native-renderer\runs\task076-*\*\king_kong.exe') {
  throw 'Only this task private game can be controlled or captured'
}
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class OwnWindow076 {
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint key,uint mode);
 [StructLayout(LayoutKind.Sequential)] public struct R {public int l,t,r,b;}
 public static void Shot(IntPtr h,string path) {
  var old=SetThreadDpiAwarenessContext(new IntPtr(-4));
  try { R r; if(!GetWindowRect(h,out r)) throw new Exception("Own window unavailable");
   using(var b=new System.Drawing.Bitmap(r.r-r.l,r.b-r.t)) using(var g=System.Drawing.Graphics.FromImage(b)) {
    var dc=g.GetHdc(); bool ok; try {ok=PrintWindow(h,dc,2);} finally {g.ReleaseHdc(dc);}
    if(!ok) throw new Exception("PrintWindow failed"); b.Save(path);
   }
  } finally {SetThreadDpiAwarenessContext(old);}
 }
}
'@
$game.Refresh(); $hwnd=$game.MainWindowHandle
if (!$hwnd) { throw 'Own HWND not ready' }
switch ($Action) {
 'shot' { [OwnWindow076]::Shot($hwnd,$Value) }
 'key' {
   $key=[int]$Value; $scan=[OwnWindow076]::MapVirtualKey($key,0)
   [void][OwnWindow076]::PostMessage($hwnd,0x100,[IntPtr]$key,[IntPtr](1 -bor ($scan -shl 16)))
   Start-Sleep -Milliseconds 100
   [void][OwnWindow076]::PostMessage($hwnd,0x101,[IntPtr]$key,[IntPtr](1L -bor ($scan -shl 16) -bor 0xC0000000L))
 }
 'text' { foreach ($c in $Value.ToCharArray()) { [void][OwnWindow076]::PostMessage($hwnd,0x102,[IntPtr][int]$c,[IntPtr]1); Start-Sleep -Milliseconds 12 } }
 'click' {
   $pos=[IntPtr](($Y -shl 16) -bor ($X -band 65535))
   [void][OwnWindow076]::PostMessage($hwnd,0x200,[IntPtr]0,$pos)
   Start-Sleep -Milliseconds 100
   [void][OwnWindow076]::PostMessage($hwnd,0x201,[IntPtr]1,$pos)
   Start-Sleep -Milliseconds 100
   [void][OwnWindow076]::PostMessage($hwnd,0x202,[IntPtr]0,$pos)
 }
 'wheel' { [void][OwnWindow076]::PostMessage($hwnd,0x20A,[IntPtr](([int]$Value*120) -shl 16),[IntPtr]0) }
}
