# 自动化测试辅助（不随程序发布）
Add-Type -AssemblyName System.Drawing
if (-not ('CO.W' -as [type])) {
Add-Type @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
namespace CO {
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, string s);
  [DllImport("user32.dll")] public static extern bool RegisterHotKey(IntPtr h, int id, uint mod, uint vk);
  [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr h, int id);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public static IntPtr Find(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) { var sb = new StringBuilder(64); GetClassName(h, sb, 64);
        if (sb.ToString() == cls) { found = h; return false; } }
      return true; }, IntPtr.Zero);
    return found;
  }
}}
"@
}

function Get-Co($proc) {
    $h = [CO.W]::Find([uint32]$proc.Id, 'ClockOverlay.Host')
    $o = [CO.W]::Find([uint32]$proc.Id, 'ClockOverlay.Overlay')
    $ex = [int64][CO.W]::GetWindowLongPtr($o, -20)
    $r = New-Object CO.W+RECT; [void][CO.W]::GetWindowRect($o, [ref]$r)
    [pscustomobject]@{
        Host = $h; Overlay = $o; Visible = [CO.W]::IsWindowVisible($o)
        Topmost = [bool]($ex -band 0x8); ToolWindow = [bool]($ex -band 0x80)
        NoActivate = [bool]($ex -band 0x8000000); Layered = [bool]($ex -band 0x80000)
        ClickThrough = [bool]($ex -band 0x20)
        Rect = "$($r.L),$($r.T) $($r.R - $r.L)x$($r.B - $r.T)"
        L = $r.L; T = $r.T; W = $r.R - $r.L; H = $r.B - $r.T
    }
}

# 面板控件 ID（与 clock.c 中 enum 一致）
$ID = @{ OK=1; VISIBLE=1001; TOPMOST=1002; CLICKTHRU=1003; LOCK=1004; SHOWMODE=1005;
  TIMEFMT=1006; TIMEPREV=1007; SHOWDATE=1008; DATEFMT=1009; DATEPREV=1010;
  AUTOSTART=1011; OPENCFG=1012; RELOAD=1013; FONT=1014; TEXTCOLOR=1015; OUTLINE=1016;
  OUTLINECOLOR=1017; BGCOLOR=1018; BGOP=1019; BGOP_VAL=1020; CORNER=1021; CORNER_VAL=1022;
  OPACITY=1023; OPACITY_VAL=1024; POS_TL=1025; POS_TR=1026; POS_BL=1027; POS_BR=1028; POS_TC=1029;
  SIZE=1030; SIZE_VAL=1031; FITW=1032; RESETPOS=1033; DEFAULTS=1034; EXIT=1035 }
# 在编辑框型下拉框里“输入”文字并通知面板（WM_SETTEXT + CBN_EDITCHANGE）
function Type-Combo($panel, [int]$id, [string]$text) {
    $cb = Get-Item $panel $id
    [void][CO.W]::SendText($cb, 0x000C, [IntPtr]::Zero, $text)
    [void][CO.W]::SendMessage($panel, 0x0111, [IntPtr]($id -bor (5 -shl 16)), $cb)
}
function Pick-Combo($panel, [int]$id, [int]$index) {
    $cb = Get-Item $panel $id
    [void][CO.W]::SendMessage($cb, 0x014E, [IntPtr]$index, [IntPtr]::Zero)
    [void][CO.W]::SendMessage($panel, 0x0111, [IntPtr]($id -bor (1 -shl 16)), $cb)
}
function Is-Enabled($h) { -not ([int64][CO.W]::GetWindowLongPtr($h, -16) -band 0x08000000) }
function Get-Panel($proc) { [CO.W]::Find([uint32]$proc.Id, 'ClockOverlay.Panel') }
function Get-Item($panel, [int]$id) { [CO.W]::GetDlgItem($panel, $id) }
function Get-Text($h) { $sb = New-Object Text.StringBuilder 256; [void][CO.W]::GetWindowText($h, $sb, 256); $sb.ToString() }
# 点击面板中的按钮 / 复选框（BM_CLICK）
function Click($panel, [int]$id) { [void][CO.W]::SendMessage((Get-Item $panel $id), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) }
# 设置滑块并通知面板（TBM_SETPOS + WM_HSCROLL）
function Set-Slider($panel, [int]$id, [int]$v) {
    $tb = Get-Item $panel $id
    [void][CO.W]::SendMessage($tb, 0x0405, [IntPtr]1, [IntPtr]$v)
    [void][CO.W]::SendMessage($panel, 0x0114, [IntPtr]8, $tb)
}
# 托盘点击（WM_TRAY, WM_LBUTTONUP）
function Tray-Click($co) { [void][CO.W]::SendMessage($co.Host, 0x8001, [IntPtr]::Zero, [IntPtr]0x202) }
function Save-Window($h, $path) {
    $r = New-Object CO.W+RECT; [void][CO.W]::GetWindowRect($h, [ref]$r)
    $bmp = New-Object Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $g.Dispose(); $bmp.Save($path); $bmp.Dispose()
}

function Save-Shot($co, $path, [int]$pad = 16) {
    $w = $co.W + 2 * $pad; $h = $co.H + 2 * $pad
    $bmp = New-Object Drawing.Bitmap $w, $h
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($co.L - $pad, $co.T - $pad, 0, 0, $bmp.Size)
    $g.Dispose(); $bmp.Save($path); $bmp.Dispose()
}
