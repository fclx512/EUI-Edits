#requires -Version 7
param([string]$File = '')
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$exePath = Join-Path $repoRoot 'build-win32\visual\neo_editor.exe'
if (-not (Test-Path -LiteralPath $exePath)) { throw 'Build the visual review executable first.' }
if (@(Get-Process neo_editor -ErrorAction SilentlyContinue).Count) { throw '请先保存并关闭当前 EUI-Edits。' }
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $exePath
$start.WorkingDirectory = $repoRoot
$start.UseShellExecute = $false
$start.Environment['NEO_D2D_SOFTWARE'] = '1'
$start.Environment['NEO_WIN32_DC'] = '1'
$start.Environment['NEO_LIVE_RESIZE'] = '1'
if ($File) { $start.ArgumentList.Add((Get-Item -LiteralPath $File).FullName) }
$process = [Diagnostics.Process]::Start($start)
Write-Output "Started EUI-Edits visual review PID=$($process.Id)"
