param([Parameter(Mandatory=$true)][ValidateSet('vulkan','d3d12')][string]$Api,
      [Parameter(Mandatory=$true)][int]$GameId)
$owned=Get-CimInstance Win32_Process -Filter "ProcessId = $GameId"
if (!$owned -or $owned.Name -ne 'king_kong.exe' -or $owned.ExecutablePath -notlike 'F:\KK-native-renderer\runs\task075-*\king_kong.exe') {
  throw 'Capture only a task075 private game PID that this task started.'
}
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class FullOwnShot075 {
[DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr v);
[DllImport("user32.dll")] static extern bool PrintWindow(IntPtr w,IntPtr dc,uint f);
[DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr w,out R r);
[StructLayout(LayoutKind.Sequential)] struct R { public int l,t,r,b; }
public static void Shot(IntPtr w,string file) {
var old=SetThreadDpiAwarenessContext(new IntPtr(-4));
try { R r; if(!GetWindowRect(w,out r)) return;
using(var b=new System.Drawing.Bitmap(r.r-r.l,r.b-r.t)) using(var g=System.Drawing.Graphics.FromImage(b)) {
var dc=g.GetHdc(); bool ok; try {ok=PrintWindow(w,dc,2);} finally {g.ReleaseHdc(dc);}
if(ok)b.Save(file);
}} finally {SetThreadDpiAwarenessContext(old);}
}
}
"@
$dir=Join-Path $PSScriptRoot '../documentary/uipaint2'
while($g=Get-Process -Id $GameId -ErrorAction SilentlyContinue) {
$g.Refresh()
if($g.MainWindowHandle -ne 0){ [FullOwnShot075]::Shot($g.MainWindowHandle,(Join-Path $dir ($Api+'-full-'+(Get-Date -Format HHmmssfff)+'.png'))) }
Start-Sleep -Milliseconds 700
}
