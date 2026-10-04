param(
    [switch]$Show,
    [switch]$Intel,
    [switch]$dGPU,
    [switch]$Clear,
    [string]$Restore = '',
    [string]$BackupPath = '',
    [string]$ExePath = ''
)

# 读取/切换 Windows 的 per-app GPU 偏好（HKCU\Software\Microsoft\DirectX\UserGpuPreferences）。
#
# 背景：该键按 exe 完整路径生效。仓库里 build\Release\neo_editor.exe 被钉在
# GpuPreference=2（独显，旧实验遗留），于是双击启动时应用渲染在 NVIDIA 独显上，
# 而内屏（2560x1600@240Hz）由 Intel 核显驱动 —— 每帧都要跨适配器搬运，而
# 2026-09-30 取证确认卡死的是 Intel 显示内核驱动 igdkmdn64.sys（RINGHANG）。
# 用无偏好的路径启动则走核显原生路径（见 build\gpu-intel-20260930\）。
#
# 本脚本只改当前用户的偏好键，可逆：任何改动前先把现有值写进备份文件，
# 用 -Restore <备份文件> 还原。不碰 HKLM、不碰 TDR 设置。
#
# 用法：
#   pwsh -File tests\probes\gpu_pref.ps1 -Show
#   pwsh -File tests\probes\gpu_pref.ps1 -Clear        # 还原成"让 Windows 决定"
#   pwsh -File tests\probes\gpu_pref.ps1 -Intel        # GpuPreference=1 省电/核显
#   pwsh -File tests\probes\gpu_pref.ps1 -Restore <备份文件>

$ErrorActionPreference = 'Stop'

$key = 'HKCU:\Software\Microsoft\DirectX\UserGpuPreferences'
# 本文件在 tests\probes\ 下，仓库根在上两级。
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $ExePath) {
    $ExePath = Join-Path $repoRoot 'build\Release\neo_editor.exe'
}
$ExePath = [IO.Path]::GetFullPath($ExePath)

function Get-PrefValues {
    if (-not (Test-Path $key)) { return @{} }
    $item = Get-Item -LiteralPath $key
    $map = @{}
    foreach ($name in $item.GetValueNames()) { $map[$name] = $item.GetValue($name) }
    return $map
}

function Show-Prefs($map) {
    Write-Host ('[' + $key + ']')
    if ($map.Count -eq 0) { Write-Host '  (empty)'; return }
    foreach ($name in ($map.Keys | Sort-Object)) { Write-Host ('  ' + $name + '  =  ' + $map[$name]) }
}

$current = Get-PrefValues

if ($Show -or (-not ($Intel -or $dGPU -or $Clear -or $Restore))) {
    Show-Prefs $current
    Write-Host ''
    Write-Host ('目标路径 : ' + $ExePath)
    Write-Host ('当前值   : ' + $(if ($current.ContainsKey($ExePath)) { $current[$ExePath] } else { '(无偏好)' }))
    return
}

if ($Restore) {
    $Restore = [IO.Path]::GetFullPath($Restore)
    if (-not (Test-Path -LiteralPath $Restore -PathType Leaf)) { throw "备份文件不存在：$Restore" }
    foreach ($line in Get-Content -LiteralPath $Restore) {
        if ($line -match '^\s*#') { continue }
        if ($line -notmatch '^(.*?)\t(.*)$') { continue }
        $name = $Matches[1]; $value = $Matches[2]
        if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
        if ($value -eq '') {
            Remove-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue
        } else {
            New-ItemProperty -Path $key -Name $name -Value $value -PropertyType String -Force | Out-Null
        }
        Write-Host ('restored: ' + $name + ' = ' + $value)
    }
    Write-Host ''
    Show-Prefs (Get-PrefValues)
    return
}

if (-not $BackupPath) {
    $BackupPath = Join-Path $repoRoot ('build\p0-gpupref-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')
}
$BackupPath = [IO.Path]::GetFullPath($BackupPath)
$backupDir = [IO.Path]::GetDirectoryName($BackupPath)
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
$lines = @('# HKCU\Software\Microsoft\DirectX\UserGpuPreferences backup')
foreach ($name in ($current.Keys | Sort-Object)) { $lines += ($name + "`t" + $current[$name]) }
Set-Content -LiteralPath $BackupPath -Value $lines -Encoding UTF8
Write-Host ('backup   : ' + $BackupPath)

if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
if ($Clear) {
    Remove-ItemProperty -Path $key -Name $ExePath -ErrorAction SilentlyContinue
    Write-Host ('cleared  : ' + $ExePath)
} else {
    $value = if ($Intel) { 'GpuPreference=1;' } else { 'GpuPreference=2;' }
    New-ItemProperty -Path $key -Name $ExePath -Value $value -PropertyType String -Force | Out-Null
    Write-Host ('set      : ' + $ExePath + ' = ' + $value)
}

Write-Host ''
Show-Prefs (Get-PrefValues)
Write-Host ''
Write-Host ('还原命令 : pwsh -File tests\probes\gpu_pref.ps1 -Restore ' + $BackupPath)
