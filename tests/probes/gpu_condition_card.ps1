<#
EUI-Edits GPU 条件卡采集（B1 验收项，建议 pwsh 7 运行）

用途：GPU 相关实测（resize 计数、安全模式核验、Vulkan 对照）前先跑一次，把
      “哪套构建 / 哪个 exe / 哪个驱动与显卡 / 显示器与 DPI / 事件时间窗”
      固定成文字证据，避免事后无法归因。对应实施指引 §2.2 第 1 条。

用法：pwsh -File tests/probes/gpu_condition_card.ps1 [-Exe <路径>]
输出：stdout + build/p0-probe/conditions/gpu-condition-<时间戳>.txt（不自动清理）

说明：真实 GL_RENDERER/GL_VERSION 字符串需要活的 GL 上下文，当前应用不打印
      （指引 §2.1 已记录该缺口），故本卡以“驱动版本 + ICD DLL 与注册项”为准，
      并列出每应用 GPU 首选项（UserGpuPreferences），这是双显卡机器上最容易
      被忽略、又直接决定走核显还是独显的状态。
#>
param(
  [string]$Exe = 'D:\ruanjian\NeoEditor\build\Release\neo_editor.exe',
  [string]$Repo = 'D:\ruanjian\NeoEditor'
)

$ErrorActionPreference = 'Continue'   # 采集脚本尽量不因单项失败中断

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class CardNative {
  [DllImport("user32.dll")] public static extern uint GetDpiForSystem();
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
}
'@

# 必须在任何尺寸/DPI 查询之前声明 DPI 感知：否则 GetSystemMetrics 返回虚拟化后的
# 逻辑像素、GetDpiForSystem 恒为 96，条件卡会记下错误的缩放率（本机真实为 125%）。
$dpiAware = [CardNative]::SetProcessDpiAwarenessContext([IntPtr](-4))

$lines = New-Object System.Collections.Generic.List[string]
function Add-Line([string]$text) { $lines.Add($text) | Out-Null }

$now = Get-Date
Add-Line "=== EUI-Edits GPU 条件卡 ==="
Add-Line ("采集时间(本地): {0}  (UTC: {1})" -f $now.ToString('yyyy-MM-dd HH:mm:ss zzz'), $now.ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss'))
Add-Line ""

Add-Line "--- 构建 ---"
$head = (git -C $Repo rev-parse HEAD 2>$null)
$branch = (git -C $Repo rev-parse --abbrev-ref HEAD 2>$null)
$dirty = @(git -C $Repo status --porcelain 2>$null).Count
Add-Line ("git HEAD: {0}" -f $head)
Add-Line ("git branch: {0}   工作树改动条目数: {1}{2}" -f $branch, $dirty, $(if ($dirty -gt 0) { '（dirty：结果需标注）' } else { '' }))

Add-Line ""
Add-Line "--- 被测 exe ---"
if (Test-Path -LiteralPath $Exe) {
  $item = Get-Item -LiteralPath $Exe
  $hash = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
  Add-Line ("路径: {0}" -f $item.FullName)
  Add-Line ("大小: {0:N0} bytes   写入时间: {1}" -f $item.Length, $item.LastWriteTime)
  Add-Line ("SHA256: {0}" -f $hash)
} else {
  Add-Line ("路径: {0}  <不存在>" -f $Exe)
}

Add-Line ""
Add-Line "--- 环境变量 ---"
foreach ($name in @('NEO_LIVE_RESIZE', 'NEO_GPU_STATS', 'NEO_PERF_GUARD', 'NEO_PERF_BASELINE_FILE')) {
  $value = [Environment]::GetEnvironmentVariable($name)
  if ($null -eq $value) {
    Add-Line ("{0}: <未设置>" -f $name)
  } else {
    Add-Line ("{0}: '{1}'" -f $name, $value)
  }
}

Add-Line ""
Add-Line "--- 显示适配器 ---"
$adapters = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue)
if ($adapters.Count -eq 0) { Add-Line '<未能枚举适配器>' }
foreach ($a in $adapters) {
  Add-Line ("Name: {0}" -f $a.Name)
  Add-Line ("  驱动版本: {0}   驱动日期: {1}   状态: {2}" -f $a.DriverVersion, $a.DriverDate, $a.Status)
  Add-Line ("  当前模式: {0}x{1} @ {2} Hz   显存: {3:N0} bytes" -f `
      $a.CurrentHorizontalResolution, $a.CurrentVerticalResolution, $a.CurrentRefreshRate, $a.AdapterRAM)
}

Add-Line ""
Add-Line "--- 每应用 GPU 首选项（UserGpuPreferences；1=省电/核显 2=高性能/独显）---"
$gpuPrefKey = 'HKCU:\SOFTWARE\Microsoft\DirectX\UserGpuPreferences'
if (Test-Path $gpuPrefKey) {
  $props = (Get-Item $gpuPrefKey).Property
  $matched = $props | Where-Object { $_ -like '*neo*' -or $_ -like '*EUI-Edits*' }
  if ($matched) {
    foreach ($p in $matched) { Add-Line ("{0} = {1}" -f $p, (Get-ItemProperty $gpuPrefKey).$p) }
  } else {
    Add-Line '(neo_editor 未有显式首选项：由系统按电源策略决定核显/独显)'
  }
  Add-Line ("（该键共 {0} 条应用级首选项）" -f @($props).Count)
} else {
  Add-Line '<无 UserGpuPreferences 键>'
}

Add-Line ""
Add-Line "--- Vulkan ---"
$vulkanDll = Join-Path $env:SystemRoot 'System32\vulkan-1.dll'
if (Test-Path $vulkanDll) {
  Add-Line ("loader: {0}  版本 {1}" -f $vulkanDll, (Get-Item $vulkanDll).VersionInfo.FileVersion)
} else {
  Add-Line 'loader: System32\vulkan-1.dll <未安装>'
}
$icdKeys = @(
  'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers',
  'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers',
  'HKCU:\SOFTWARE\Khronos\Vulkan\Drivers'
)
$icdFound = $false
foreach ($key in $icdKeys) {
  if (-not (Test-Path $key)) { continue }
  $icdFound = $true
  Add-Line ("ICD 注册项: {0}" -f $key)
  $props = (Get-Item $key).Property
  foreach ($jsonPath in $props) {
    $enabled = (Get-ItemProperty $key).$jsonPath
    if ($enabled -ne 0) { continue }   # 0 = 启用，非 0 = 显式禁用
    Add-Line ("  ICD JSON: {0}" -f $jsonPath)
    if (Test-Path -LiteralPath $jsonPath) {
      try {
        $icd = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
        $lib = $icd.ICD.library_path
        $libFull = $lib
        if ($lib -and -not [System.IO.Path]::IsPathRooted($lib)) {
          $libFull = Join-Path (Split-Path -Parent $jsonPath) $lib
        }
        if ($libFull -and (Test-Path -LiteralPath $libFull)) {
          Add-Line ("    library: {0}  版本 {1}" -f $libFull, (Get-Item $libFull).VersionInfo.FileVersion)
        } else {
          Add-Line ("    library: {0}  <文件缺失>" -f $libFull)
        }
        Add-Line ("    apiVersion: {0}" -f $icd.ICD.api_version)
      } catch {
        Add-Line ("    <ICD JSON 解析失败: {0}>" -f $_.Exception.Message)
      }
    } else {
      Add-Line '    <JSON 文件缺失>'
    }
  }
}
if (-not $icdFound) { Add-Line 'ICD 注册项: <未找到 Khronos\Vulkan\Drivers（Vulkan 可能仅由其它位置的 ICD 提供）>' }

Add-Line ""
Add-Line "--- OpenGL ICD（厂商驱动模块）---"
$glKey = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers'
if (Test-Path $glKey) {
  $children = @(Get-ChildItem $glKey -ErrorAction SilentlyContinue)
  if ($children.Count -eq 0) { Add-Line ("{0}: 存在但无子项" -f $glKey) }
  foreach ($child in $children) {
    $dllName = (Get-ItemProperty $child.PSPath -ErrorAction SilentlyContinue).Dll
    Add-Line ("{0} -> Dll={1}" -f $child.PSChildName, $dllName)
  }
} else {
  Add-Line ("{0}: <不存在>" -f $glKey)
}
foreach ($candidate in @('ig9icd64.dll', 'ig7icd64.dll', 'nvoglv64.dll', 'atio6axx.dll', 'amdvlk64.dll')) {
  $candidatePath = Join-Path $env:SystemRoot ("System32\" + $candidate)
  if (Test-Path $candidatePath) {
    Add-Line ("{0}: 存在，版本 {1}" -f $candidate, (Get-Item $candidatePath).VersionInfo.FileVersion)
  } else {
    Add-Line ("{0}: 不在 System32（可能装在 DriverStore，或该厂商 ICD 未安装）" -f $candidate)
  }
}

Add-Line ""
Add-Line "--- 显示 / DPI ---"
Add-Line ("DPI 感知已声明(PMv2): {0}   若为 False 则下列像素/DPI 值被系统虚拟化，不可信" -f $dpiAware)
$systemDpi = [CardNative]::GetDpiForSystem()
Add-Line ("系统 DPI: {0}  (约 {1}% 缩放)" -f $systemDpi, [math]::Round($systemDpi / 96 * 100))
Add-Line ("SM_CMONITORS 显示器数: {0}   主屏尺寸: {1}x{2}" -f `
    [CardNative]::GetSystemMetrics(80), [CardNative]::GetSystemMetrics(0), [CardNative]::GetSystemMetrics(1))
Get-CimInstance Win32_DesktopMonitor -ErrorAction SilentlyContinue |
  ForEach-Object { Add-Line ("显示器 {0}: {1}x{2}" -f $_.DeviceID, $_.ScreenWidth, $_.ScreenHeight) }

Add-Line ""
Add-Line "--- 系统 ---"
$os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
if ($os) {
  Add-Line ("OS: {0} (Build {1})   物理内存: {2:N1} GB" -f $os.Caption, $os.BuildNumber, ($os.TotalVisibleMemorySize / 1MB))
}
Add-Line ("逻辑处理器: {0}" -f [Environment]::ProcessorCount)

Add-Line ""
Add-Line "--- 说明 ---"
Add-Line "真实 GL_RENDERER/GL_VERSION 需活 GL 上下文，当前应用不打印；如需该值请在应用侧加诊断输出。"
Add-Line "同一时间窗内的多次测量请共用一个条件卡文件（记录时间窗起点即可）。"

$text = ($lines -join [Environment]::NewLine) + [Environment]::NewLine
Write-Output $text

$outDir = Join-Path $Repo 'build\p0-probe\conditions'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$outFile = Join-Path $outDir ("gpu-condition-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
[System.IO.File]::WriteAllText($outFile, $text, (New-Object System.Text.UTF8Encoding($true)))
Write-Output ("条件卡已写入: {0}" -f $outFile)
