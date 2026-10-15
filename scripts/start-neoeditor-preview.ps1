#Requires -Version 7
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$File,
    [switch]$Hardware,
    [switch]$PassThru,
    [string]$ProfileDirectory
)
$ErrorActionPreference = 'Stop'
$exePath = (Get-Item -LiteralPath $Exe).FullName
$filePath = (Get-Item -LiteralPath $File).FullName
if (@(Get-Process neo_editor -ErrorAction SilentlyContinue).Count) {
    throw 'EUI-Edits is already running. Close it normally before starting the isolated preview test.'
}
$profile = if ($ProfileDirectory) { [IO.Path]::GetFullPath($ProfileDirectory) }
    else { Join-Path (Split-Path -Parent $exePath) ('test-profile-' + [Guid]::NewGuid().ToString('N')) }
if (-not (Test-Path -LiteralPath $profile -PathType Container)) {
    if (Test-Path -LiteralPath $profile) { throw 'ProfileDirectory must be a directory.' }
    New-Item -ItemType Directory -Path $profile | Out-Null
}
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $exePath
$start.WorkingDirectory = Split-Path -Parent $exePath
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
foreach ($name in @($start.Environment.Keys | Where-Object { $_ -like 'NEO_*' })) {
    $start.Environment.Remove($name) | Out-Null
}
$start.Environment['APPDATA'] = $profile
$start.Environment['LOCALAPPDATA'] = $profile
$start.Environment['NEO_SINGLE_INSTANCE'] = '0'
$start.Environment['NEO_D2D_SOFTWARE'] = if ($Hardware) { '0' } else { '1' }
$start.Environment['NEO_D2D_TRACE'] = '1'
$start.ArgumentList.Add($filePath)
$exeHash = if ($PassThru) { (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash } else { $null }
$process = [Diagnostics.Process]::Start($start)
$message = "Started preview test PID={0}; mode={1}; profile={2}" -f $process.Id, $(if ($Hardware) { 'hardware-preferred (actual adapter unconfirmed)' } else { 'software DC/GDI' }), $profile
if ($PassThru) {
    Write-Host $message
    [pscustomobject]@{
        Process = $process; Exe = $exePath; Profile = $profile; File = $filePath
        SHA256 = $exeHash
        RenderRequested = if ($Hardware) { 'hardware-preferred' } else { 'software DC/GDI' }
    }
} else { Write-Output $message }
