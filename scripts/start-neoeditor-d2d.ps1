#requires -Version 7
<# Launch the Win32 + Direct2D build; does not replace the main OpenGL executable. #>
param([switch]$Hardware, [switch]$LiveResize, [string]$File = '')
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$exePath = Join-Path $repoRoot 'build-win32\Release\neo_editor.exe'
if (-not (Test-Path -LiteralPath $exePath)) { throw 'Build build-win32/Release/neo_editor.exe first (EUI_WINDOW_BACKEND=win32 EUI_RENDER_BACKEND=d2d).' }
if (@(Get-Process neo_editor -ErrorAction SilentlyContinue).Count) { throw 'EUI-Edits is already running; close it before testing the other renderer.' }
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $exePath
$start.WorkingDirectory = $repoRoot
$start.UseShellExecute = $false
$start.Environment['NEO_D2D_SOFTWARE'] = $(if ($Hardware) {'0'} else {'1'})
$start.Environment['NEO_LIVE_RESIZE'] = $(if ($LiveResize) {'1'} else {'0'})
if ($File) { $start.ArgumentList.Add((Get-Item -LiteralPath $File).FullName) }
$process = [Diagnostics.Process]::Start($start)
Write-Output "Started Direct2D build PID=$($process.Id), software=$(-not $Hardware), liveResize=$([bool]$LiveResize)"
