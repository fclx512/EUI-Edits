#requires -Version 7
<# Launch the isolated Win32 software-rendering experiment; does not replace the accepted OpenGL executable. #>
param([switch]$Hardware, [switch]$NoLiveResize, [string]$File = '')
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$exePath = Join-Path $repoRoot 'build-win32\Release\neo_editor.exe'
if (-not (Test-Path -LiteralPath $exePath)) { throw 'Build build-win32/Release/neo_editor.exe first; see the experiment report.' }
if (@(Get-Process neo_editor -ErrorAction SilentlyContinue).Count) { throw 'EUI-Edits is already running; close it before testing the other renderer.' }
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $exePath
$start.WorkingDirectory = $repoRoot
$start.UseShellExecute = $false
$start.Environment['NEO_D2D_SOFTWARE'] = $(if ($Hardware) {'0'} else {'1'})
$start.Environment['NEO_WIN32_DC'] = '1'
$start.Environment['NEO_LIVE_RESIZE'] = $(if ($NoLiveResize) {'0'} else {'1'})
if ($File) { $start.ArgumentList.Add((Get-Item -LiteralPath $File).FullName) }
$process = [Diagnostics.Process]::Start($start)
Write-Output "Started Win32 software-rendering experiment PID=$($process.Id), software=$(-not $Hardware), liveResize=$(-not $NoLiveResize)"
