#Requires -Version 7
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$File,
    [switch]$Hardware
)
$ErrorActionPreference = 'Stop'
$exePath = (Get-Item -LiteralPath $Exe).FullName
$filePath = (Get-Item -LiteralPath $File).FullName
if (@(Get-Process neo_editor -ErrorAction SilentlyContinue).Count) {
    throw 'EUI-Edits is already running. Close it normally before starting the isolated preview test.'
}
$profile = Join-Path (Split-Path -Parent $exePath) ('test-profile-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $exePath
$start.WorkingDirectory = Split-Path -Parent $exePath
$start.UseShellExecute = $false
foreach ($name in @('NEO_D2D_SOFTWARE', 'NEO_WIN32_DC', 'NEO_LIVE_RESIZE', 'NEO_GPU_STATS', 'NEO_GPU_STATS_FILE', 'NEO_GPU_STATS_INTERVAL_MS', 'NEO_RESIZE_TRACE', 'NEO_TABS_TRACE', 'NEO_D2D_TRACE')) {
    $start.Environment.Remove($name) | Out-Null
}
$start.Environment['APPDATA'] = $profile
$start.Environment['NEO_D2D_SOFTWARE'] = if ($Hardware) { '0' } else { '1' }
$start.Environment['NEO_D2D_TRACE'] = '1'
$start.ArgumentList.Add($filePath)
$process = [Diagnostics.Process]::Start($start)
Write-Output ("Started preview test PID={0}; mode={1}; profile={2}" -f $process.Id, $(if ($Hardware) { 'hardware-preferred (actual adapter unconfirmed)' } else { 'software DC/GDI' }), $profile)
