[CmdletBinding()]
param(
    [ValidateRange(1, 10000)]
    [int]$Rounds = 20,
    [switch]$Show,
    [switch]$Light,
    [switch]$Hwnd,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$sourcePath = Join-Path $PSScriptRoot 'frozen_backdrop_probe.cpp'
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "Probe source not found: $sourcePath"
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $OutputDirectory = Join-Path (Join-Path $repoRoot 'out') "frozen-backdrop-$stamp"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Output directory already exists; choose a new path: $OutputDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null

$exePath = Join-Path $OutputDirectory 'frozen_backdrop_probe.exe'
$objPath = Join-Path $OutputDirectory 'frozen_backdrop_probe.obj'
$stdoutPath = Join-Path $OutputDirectory 'results.jsonl'
$stderrPath = Join-Path $OutputDirectory 'stderr.txt'

$compiler = Get-Command 'cl.exe' -ErrorAction SilentlyContinue
if ($null -eq $compiler) {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'cl.exe is unavailable and vswhere.exe was not found. Run from an x64 Visual Studio developer shell.'
    }
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vsRoot)) {
        throw 'vswhere could not locate the Visual C++ x64 tools.'
    }
    $devCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devCmd -PathType Leaf)) {
        throw "VsDevCmd.bat not found: $devCmd"
    }
    $setupCommand = 'call "' + $devCmd + '" -no_logo -arch=x64 -host_arch=x64 && set'
    $environmentLines = & $env:ComSpec /d /c $setupCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio environment setup failed with exit code $LASTEXITCODE."
    }
    foreach ($line in $environmentLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
    $compiler = Get-Command 'cl.exe' -ErrorAction SilentlyContinue
    if ($null -eq $compiler) {
        throw 'Visual Studio setup completed, but cl.exe is still unavailable.'
    }
}

$compileArgs = @(
    '/nologo', '/std:c++17', '/EHsc', '/O2', '/W4', '/permissive-',
    '/DUNICODE', '/D_UNICODE',
    $sourcePath, "/Fo$objPath", "/Fe$exePath",
    '/link', 'd2d1.lib', 'dwrite.lib', 'ole32.lib', 'user32.lib', 'gdi32.lib', 'dxguid.lib'
)
& $compiler.Source @compileArgs *> (Join-Path $OutputDirectory 'compile.log')
if ($LASTEXITCODE -ne 0) {
    throw "Probe compilation failed with exit code $LASTEXITCODE. See compile.log in $OutputDirectory."
}

$probeArgs = @('--rounds', $Rounds.ToString())
if ($Show) { $probeArgs += '--show' }
if ($Light) { $probeArgs += '--light' }
if ($Hwnd) { $probeArgs += '--hwnd' } else { $probeArgs += '--dc' }

$metadata = [ordered]@{
    source = $sourcePath
    sourceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourcePath).Hash
    executable = $exePath
    executableSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $exePath).Hash
    rounds = $Rounds
    show = [bool]$Show
    target = if ($Hwnd) { 'D2D1 software HWND render target' } else { 'D2D1 software DC render target (default)' }
    theme = if ($Light) { 'light' } else { 'dark' }
    osVersion = [System.Environment]::OSVersion.VersionString
    compiler = $compiler.Source
}
$metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'run-metadata.json') -Encoding UTF8

& $exePath @probeArgs 1> $stdoutPath 2> $stderrPath
$probeExit = $LASTEXITCODE
if ($probeExit -ne 0) {
    throw "Probe exited with code $probeExit. See results.jsonl and stderr.txt in $OutputDirectory."
}

Write-Output "Results: $stdoutPath"
