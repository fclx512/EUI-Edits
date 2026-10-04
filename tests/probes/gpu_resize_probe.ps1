<#
GPU resize 计数实测（B1 验收：真实分配计数 + 安全模式核验）

干什么：用 NEO_GPU_STATS=1 启动被测 exe，程序化做一批**有界**的窗口尺寸变化
（SetWindowPos 改客户区，幅度 ±5 档），结束后收集应用退出时打印的会话汇总。

为什么这样做：计数器的口径是"回调次数 / 真正执行的 live-resize paint / 真实纹理与 FBO
分配"。真实拖拽是模态拖拽循环里的 ~30Hz refresh 回调；SetWindowPos 循环触发同一批
WM_SIZE → GLFW framebuffer 回调，条数可控、可重复，适合做 A/B 对照。它**不能**代替
真实拖拽的观感（那是人工项），但足以回答"安全模式下 live-resize paint 与分配次数是否
显著下降"。

用法：
  pwsh -File tests/probes/gpu_resize_probe.ps1                 # 常规（实时 resize 开启）
  pwsh -File tests/probes/gpu_resize_probe.ps1 -SafeMode       # 安全模式 NEO_LIVE_RESIZE=0
  pwsh -File tests/probes/gpu_resize_probe.ps1 -Doc <文档路径>  # 先加载文档再测
前提：无其它 EUI-Edits 实例在运行（本脚本不替你杀进程），桌面未锁屏。
#>
param(
  [string]$Exe = 'D:\ruanjian\NeoEditor\build\Release\neo_editor.exe',
  [string]$Doc = '',
  [int]$Resizes = 30,
  [switch]$SafeMode
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class NeoWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
  [DllImport("user32.dll", SetLastError=true)] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
}
'@

$existing = @(Get-Process -Name 'neo_editor' -ErrorAction SilentlyContinue)
if ($existing.Count -gt 0) {
  throw ("已有 EUI-Edits 在运行（PID: {0}）—— 探针要求独占，且不会替用户结束进程。" -f (($existing | ForEach-Object { $_.Id }) -join ', '))
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
if ($Doc) { $psi.Arguments = '"' + $Doc + '"' }
$psi.UseShellExecute = $false
$psi.RedirectStandardError = $true
$psi.EnvironmentVariables['NEO_GPU_STATS'] = '1'
if ($SafeMode) { $psi.EnvironmentVariables['NEO_LIVE_RESIZE'] = '0' }

$mode = if ($SafeMode) { '安全模式 NEO_LIVE_RESIZE=0' } else { '常规（实时 resize 开启）' }
$p = [System.Diagnostics.Process]::Start($psi)
$errTask = $p.StandardError.ReadToEndAsync()

try {
  $deadline = (Get-Date).AddSeconds(30)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    if ($p.HasExited) { throw ("被测进程提前退出：ExitCode={0} (0x{1:X8})" -f $p.ExitCode, $p.ExitCode) }
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) { break }
  }
  if ($p.MainWindowHandle -eq [IntPtr]::Zero) { throw '30s 内没等到主窗口' }
  $hwnd = $p.MainWindowHandle

  $rect = New-Object NeoWin+RECT
  [void][NeoWin]::GetClientRect($hwnd, [ref]$rect)
  $baseW = $rect.right
  $baseH = $rect.bottom
  Write-Output ("模式: {0}" -f $mode)
  Write-Output ("exe : {0}" -f $Exe)
  if ($Doc) { Write-Output ("文档: {0}" -f $Doc) }
  Write-Output ("初始客户区: {0}x{1}   计划 resize {2} 次" -f $baseW, $baseH, $Resizes)

  Start-Sleep -Seconds 3   # 让它先画稳（首次 compose / 字体图集）
  for ($i = 0; $i -lt $Resizes; $i++) {
    $w = [Math]::Max(320, $baseW + (($i % 10) - 5) * 12)
    $h = [Math]::Max(240, $baseH + (($i % 6) - 3) * 8)
    # SWP_NOZORDER(0x4) | SWP_NOACTIVATE(0x10)：不抢前台、不改 z 序
    [void][NeoWin]::SetWindowPos($hwnd, [IntPtr]::Zero, 0, 0, $w, $h, 0x0004 -bor 0x0010)
    Start-Sleep -Milliseconds 45
  }
  Start-Sleep -Seconds 2
} finally {
  if (-not $p.HasExited) {
    [void]$p.CloseMainWindow()
    if (-not $p.WaitForExit(15000)) { $p.Kill(); [void]$p.WaitForExit(5000) }
  }
}

$stderr = $errTask.GetAwaiter().GetResult()
if ([string]::IsNullOrWhiteSpace($stderr)) {
  Write-Output '未收到 NEO_GPU_STATS 汇总（进程可能未正常退出）'
} else {
  Write-Output '--- 应用退出时的会话汇总 ---'
  Write-Output $stderr.TrimEnd()
}
