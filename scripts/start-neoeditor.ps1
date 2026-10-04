param(
    [string]$DocumentPath = '',
    [string]$Exe = '',
    [switch]$LiveResize,
    [switch]$NoStats,
    [string]$StatsFile = ''
)

# 诊断/安全启动器。默认：最新构建 + 关闭实时 resize（安全模式）+ 打开 GPU 计数心跳。
#
# 为什么默认关实时 resize：2026-09-30 取证确认卡死是 Intel 显示内核驱动
# （igdkmdn64.sys，RINGHANG）的 ring 挂死，触发负载是窗口拖拽期间的密集重绘/
# 提交。实时 resize 刷新是已知危险路径，默认关掉可以避免把模态拖拽消息风暴
# 变成提交风暴（代价：拖拽中内容由 DWM 拉伸，松手后刷新）。
# 需要观感时加 -LiveResize 显式打开。
#
# 诊断留痕：默认打开 NEO_GPU_STATS=1 并把心跳写到 -StatsFile
# （缺省放在 exe 同级目录，ASCII 路径）。卡死时该文件最后一行就是现场。
# 不让它在 exe 目录写文件时用 -NoStats。
#
# 注意：Windows 的 GPU 偏好按 exe 完整路径生效。
#   build\Release\neo_editor.exe                 -> 注册表里被钉在独显（GpuPreference=2）
#   build\gpu-intel-20260930\Release\neo_editor.exe -> 无偏好，走核显原生路径
# 用 -Exe 选择要测哪条路径。

$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Exe) {
    $Exe = Join-Path $repoRoot 'build\Release\neo_editor.exe'
}
$Exe = [IO.Path]::GetFullPath($Exe)
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "找不到可执行文件：$Exe"
}

$exeDir = [IO.Path]::GetDirectoryName($Exe)
if (-not $StatsFile) {
    $StatsFile = Join-Path $exeDir 'neo_gpu_stats.log'
}
$StatsFile = [IO.Path]::GetFullPath($StatsFile)

# 已有实例在跑就不会真正启动（单实例会转发并退出），先明确报错。
if (@(Get-Process -Name 'neo_editor' -ErrorAction SilentlyContinue).Count -gt 0) {
    throw '已有 EUI-Edits 在运行：单实例闸门会转发命令行后退出，本次启动不会生效。请先关闭它。'
}

$previousLiveResize = $env:NEO_LIVE_RESIZE
$previousStats = $env:NEO_GPU_STATS
$previousStatsFile = $env:NEO_GPU_STATS_FILE
$previousInterval = $env:NEO_GPU_STATS_INTERVAL_MS
try {
    # 只改本进程子环境，不写 Windows GPU/TDR 偏好与用户设置。
    if ($LiveResize) {
        Remove-Item Env:NEO_LIVE_RESIZE -ErrorAction SilentlyContinue
    } else {
        $env:NEO_LIVE_RESIZE = '0'
    }
    if ($NoStats) {
        Remove-Item Env:NEO_GPU_STATS -ErrorAction SilentlyContinue
        Remove-Item Env:NEO_GPU_STATS_FILE -ErrorAction SilentlyContinue
    } else {
        $env:NEO_GPU_STATS = '1'
        $env:NEO_GPU_STATS_FILE = $StatsFile
        $env:NEO_GPU_STATS_INTERVAL_MS = '500'
    }

    $launch = @{
        FilePath         = $Exe
        WorkingDirectory = $exeDir
        WindowStyle      = 'Normal'
        PassThru         = $true
    }
    if ($DocumentPath) {
        $document = [IO.Path]::GetFullPath($DocumentPath)
        if (-not (Test-Path -LiteralPath $document -PathType Leaf)) {
            throw "文档不存在：$document"
        }
        $launch.ArgumentList = '"' + $document + '"'
    }

    Write-Host ('exe        : ' + $Exe)
    Write-Host ('实时 resize : ' + $(if ($LiveResize) { '开启（实验路径，拖拽中重绘）' } else { '关闭（安全模式：拖拽中不提交帧）' }))
    Write-Host ('GPU 计数    : ' + $(if ($NoStats) { '关闭' } else { "开启，心跳写 $StatsFile" }))
    $process = Start-Process @launch
    Write-Host ('pid        : ' + $process.Id)
} finally {
    $env:NEO_LIVE_RESIZE = $previousLiveResize
    $env:NEO_GPU_STATS = $previousStats
    $env:NEO_GPU_STATS_FILE = $previousStatsFile
    $env:NEO_GPU_STATS_INTERVAL_MS = $previousInterval
}
