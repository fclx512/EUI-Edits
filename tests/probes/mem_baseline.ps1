<#
EUI-Edits 常驻内存基线实测（2026-09-29 修订版，建议用 pwsh 7 运行）

口径：WorkingSetSize = 工作集（任务管理器“内存(活动专用工作集)”之外的整体工作集）
      PrivateUsage   = 私有提交 / Private Bytes（任务管理器“提交大小”列），**不是**“专用工作集”
隔离：每次运行创建唯一目录；每个场景使用独立的 APPDATA 子目录。
      注意 Local\EUI-Edits.SingleInstance 互斥名不受 APPDATA 影响，故启动前强制检查单实例。
用法：pwsh -File tests/probes/mem_baseline.ps1 [-KeepArtifacts]
      默认在结束后清理本轮唯一目录；-KeepArtifacts 保留样本与结果文件用于复核。
前置：被测 exe 已构建；无正在运行的 EUI-Edits（本脚本绝不自动结束用户进程）。
#>
param(
  [switch]$KeepArtifacts,
  # 被测 exe。默认主构建；给其它路径可做同脚本 A/B（例如对比上一版构建或便携包）。
  [string]$Exe = 'D:\ruanjian\NeoEditor\build\Release\neo_editor.exe'
)

$ErrorActionPreference = 'Stop'
$exe  = $Exe
$base = 'D:\ruanjian\NeoEditor\build\p0-probe'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$root = Join-Path $base ("mem-probe-$stamp-$PID")

if (-not (Test-Path -LiteralPath $exe)) {
  throw "被测 exe 不存在：$exe（先构建 build/Release 的 neo_editor 目标）"
}

# ---- 前置检查：独占运行环境 ----
function Assert-NoForeignInstance {
  $existing = @(Get-Process -Name 'neo_editor' -ErrorAction SilentlyContinue)
  if ($existing.Count -gt 0) {
    $ids = ($existing | ForEach-Object { $_.Id }) -join ', '
    throw "检测到已运行的 EUI-Edits 进程（PID: $ids）。探针要求独占运行，且不会自动结束用户进程；请先手动关闭这些窗口后重试。"
  }
  $mutexHeld = $false
  try {
    $m = [System.Threading.Mutex]::OpenExisting('Local\EUI-Edits.SingleInstance')
    $mutexHeld = $true
    $m.Dispose()
  } catch [System.Threading.WaitHandleCannotBeOpenedException] {
    $mutexHeld = $false
  } catch {
    Write-Warning "单实例互斥探测异常（按未占用继续）：$($_.Exception.Message)"
  }
  if ($mutexHeld) {
    throw "单实例互斥 Local\EUI-Edits.SingleInstance 已被占用（可能残留实例）。请确认后再运行。"
  }
  # 残留转发文件会让被测进程打开“不是本轮样本”的文档，必须排除。
  $forward = Join-Path ([System.IO.Path]::GetTempPath()) 'EUI-Edits.next-open'
  if (Test-Path -LiteralPath $forward) {
    throw "存在残留转发文件：$forward。它会劫持被测进程的启动文档；请确认无 EUI-Edits 实例后删除该文件再重试。"
  }
}
Assert-NoForeignInstance

New-Item -ItemType Directory -Path $root -Force | Out-Null

Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public class PM{
 [StructLayout(LayoutKind.Sequential)]
 public struct COUNTERS{public uint cb;public uint PageFaultCount;
  public UIntPtr PeakWorkingSetSize;public UIntPtr WorkingSetSize;
  public UIntPtr QuotaPeakPagedPoolUsage;public UIntPtr QuotaPagedPoolUsage;
  public UIntPtr QuotaPeakNonPagedPoolUsage;public UIntPtr QuotaNonPagedPoolUsage;
  public UIntPtr PagefileUsage;public UIntPtr PeakPagefileUsage;public UIntPtr PrivateUsage;}
 [DllImport("psapi.dll",SetLastError=true)]
 public static extern bool GetProcessMemoryInfo(IntPtr h,out COUNTERS c,uint cb);
 [DllImport("kernel32.dll")]public static extern IntPtr OpenProcess(uint a,bool inh,int id);
 [DllImport("kernel32.dll")]public static extern bool CloseHandle(IntPtr h);
 [DllImport("user32.dll",SetLastError=true)]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
}
'@

# cb 必须显式赋值，否则 GetProcessMemoryInfo 以 ERROR_INVALID_PARAMETER 失败；
# 返回值为 false 时整轮无效（旧版忽略了返回值，可能把全零当数据）。
function Get-Mem([int]$procId, [string]$where) {
  $h = [PM]::OpenProcess(0x1000, $false, $procId)
  if ($h -eq [IntPtr]::Zero) {
    throw "${where}: OpenProcess($procId) 失败，Win32Error=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
  }
  try {
    $c = New-Object PM+COUNTERS
    $c.cb = [uint32][Runtime.InteropServices.Marshal]::SizeOf([type][PM+COUNTERS])
    $ok = [PM]::GetProcessMemoryInfo($h, [ref]$c, [uint32]$c.cb)
    if (-not $ok) {
      throw "${where}: GetProcessMemoryInfo($procId) 失败，Win32Error=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
  } finally {
    [PM]::CloseHandle($h) | Out-Null
  }
  if ($c.WorkingSetSize.ToUInt64() -eq 0) {
    throw "${where}: 工作集读数全零，本轮结果无效"
  }
  [pscustomobject]@{
    WorkingSetMB    = [math]::Round($c.WorkingSetSize.ToUInt64() / 1MB, 1)
    PrivateCommitMB = [math]::Round($c.PrivateUsage.ToUInt64() / 1MB, 1)
  }
}

# 生成混合中英文 + 标题 + 代码块的 Markdown 标本文档
function New-Doc([string]$path, [long]$targetBytes) {
  $block = @"
## 测试标题 Section

这是一段混合中英文的正文段落，用来撑起文档体积，包含中文标点、English words、
以及行内代码 \`inline_code\` 和 [链接](https://example.com/path)。第二行继续补充文字
使单段约 200 字节以上，接近真实笔记密度。数字 1234567890 与标点、、。！？混合出现。

- 列表项 alpha：中文内容 + english tail
- 列表项 beta：图片语法 ![img](_assets/pic.png) 保持原样

\`\`\`rust
fn sample(n: u64) -> u64 { n.wrapping_mul(0x9E3779B97F4A7C15) }
\`\`\`

"@
  $sw = [System.IO.StreamWriter]::new($path, $false, [System.Text.UTF8Encoding]::new($false))
  $written = 0L
  $i = 0
  while ($written -lt $targetBytes) {
    $sw.Write(('# H1 第 {0} 章' -f $i)); $sw.Write("`r`n")
    $sw.Write($block)
    $written = $sw.BaseStream.Position
    $i++
  }
  $sw.Dispose()
}

$doc1 = Join-Path $root 'doc-1mb.md'
$doc8 = Join-Path $root 'doc-8mb.md'
New-Doc $doc1 1MB
New-Doc $doc8 8MB
Write-Output ("docs: {0} ({1:N0} bytes), {2} ({3:N0} bytes)" -f $doc1, (Get-Item $doc1).Length, $doc8, (Get-Item $doc8).Length)

function Run-Scenario([string]$label, [string]$slug, [string]$docPath) {
  # slug 必须是 ASCII：这个目录会作为 APPDATA 交给被测进程。中文路径曾让应用启动
  # 即崩（getenv 的 GBK 字节被当 UTF-8 → u8path 抛异常；已在 text_file 侧修掉），
  # 这里继续只用 ASCII —— 各轮数据可比，也不依赖那个修复仍生效。
  $appdata = Join-Path $root ('appdata-' + $slug)
  New-Item -ItemType Directory -Path $appdata -Force | Out-Null
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $exe
  if ($docPath) { $psi.Arguments = '"' + $docPath + '"' }
  $psi.UseShellExecute = $false
  $psi.EnvironmentVariables['APPDATA'] = $appdata
  $p = [System.Diagnostics.Process]::Start($psi)
  try {
    # 启动核对：进程未提前退出（防被已有实例转发后空跑）+ 主窗口属于该 PID
    $deadline = (Get-Date).AddSeconds(20)
    do {
      Start-Sleep -Milliseconds 250
      if ($p.HasExited) {
        throw "场景 $label：进程启动后立即退出（ExitCode=$($p.ExitCode)），疑似被已有实例转发，本轮无效"
      }
      $p.Refresh()
    } while ($p.MainWindowHandle -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
    if ($p.MainWindowHandle -eq [IntPtr]::Zero) {
      throw "场景 $label：20s 内未出现主窗口，本轮无效"
    }
    $owner = [uint32]0
    [PM]::GetWindowThreadProcessId($p.MainWindowHandle, [ref]$owner) | Out-Null
    if ($owner -ne $p.Id) {
      throw "场景 $label：主窗口归属 PID $owner ≠ 启动 PID $($p.Id)，本轮无效"
    }

    Start-Sleep -Seconds 12   # 窗口出现后 12s：启动 + 首次 compose + recovery 节流窗口
    $m1 = Get-Mem $p.Id "$label/12s"
    Start-Sleep -Seconds 8    # 稳定性第二采样
    $m2 = Get-Mem $p.Id "$label/20s"
    $p.Refresh()
    $managedWS = [math]::Round($p.WorkingSet64 / 1MB, 1)   # 交叉核对：托管 API 与 psapi 工作集
  } finally {
    if (-not $p.HasExited) {
      $p.CloseMainWindow() | Out-Null
      if (-not $p.WaitForExit(8000)) {
        Stop-Process -Id $p.Id -Force
        $p.WaitForExit(4000) | Out-Null
      }
    }
    Start-Sleep -Seconds 2
  }
  if ([math]::Abs($managedWS - $m1.WorkingSetMB) -gt [math]::Max(5, $m1.WorkingSetMB * 0.05)) {
    Write-Warning "场景 $label：托管工作集($managedWS MB) 与 psapi($($m1.WorkingSetMB) MB) 差异偏大，请复查口径"
  }
  [pscustomobject]@{
    场景 = $label; PID = $p.Id
    工作集MB_12s = $m1.WorkingSetMB; 私有提交MB_12s = $m1.PrivateCommitMB
    工作集MB_20s = $m2.WorkingSetMB; 私有提交MB_20s = $m2.PrivateCommitMB
    托管工作集MB_20s = $managedWS
  }
}

$results = @()
Write-Output '== 空载（隔离 APPDATA，无恢复文档） =='
$results += Run-Scenario '空载' 'empty' $null
Write-Output '== 打开 1MB 文档（加载，未滚动） =='
$results += Run-Scenario '1MB' '1mb' $doc1
Write-Output '== 打开 8MB 文档（加载，未滚动） =='
$results += Run-Scenario '8MB' '8mb' $doc8

$table = $results | Format-Table -AutoSize | Out-String -Width 200
Write-Output $table
Write-Output ("口径：工作集=WorkingSetSize；私有提交=PrivateUsage（提交大小，非专用工作集）。本轮目录：{0}" -f $root)

$report = Join-Path $root 'membaseline-result.txt'
$table | Set-Content -LiteralPath $report -Encoding utf8
Write-Output ("结果已写入 {0}" -f $report)

# ---- 清理本轮唯一目录：先做绝对路径边界校验，边界不明一律不删 ----
if (-not $KeepArtifacts) {
  $full = [System.IO.Path]::GetFullPath($root)
  $baseFull = [System.IO.Path]::GetFullPath($base)
  $leaf = Split-Path -Leaf $full
  $insideBase = $full.StartsWith($baseFull + [System.IO.Path]::DirectorySeparatorChar,
                                 [System.StringComparison]::OrdinalIgnoreCase)
  if ($full -eq $baseFull -or -not $insideBase -or -not $leaf.StartsWith('mem-probe-')) {
    Write-Warning "跳过清理：路径边界校验未通过（$full）"
  } else {
    Remove-Item -LiteralPath $full -Recurse -Force
    Write-Output '本轮目录已清理（需要保留请加 -KeepArtifacts）'
  }
} else {
  Write-Output '保留本轮目录（-KeepArtifacts）'
}
Write-Output 'DONE'
