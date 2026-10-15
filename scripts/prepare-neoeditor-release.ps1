#Requires -Version 7
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$')]
    [string]$Version,
    [string]$RunDirectory,
    [string]$OutputDirectory,
    [string]$CMake,
    [string]$Python,
    [string]$Dumpbin,
    [string]$Generator,
    [ValidateRange(1, 32)][int]$Parallel = 2,
    [switch]$PreflightOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'neoeditor-release-common.ps1')
if (-not $RunDirectory) {
    $RunDirectory = Join-Path $repo ('out/release-prepare-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
}
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'out/stable-candidate' }
$RunDirectory = Assert-EuiRepositoryPath $RunDirectory $repo 'RunDirectory'
$OutputDirectory = Assert-EuiRepositoryPath $OutputDirectory $repo 'OutputDirectory'
if (Test-Path -LiteralPath $RunDirectory) { throw 'RunDirectory must not exist; use a new evidence directory.' }
if ($OutputDirectory -eq $RunDirectory -or
    $OutputDirectory.StartsWith($RunDirectory + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $RunDirectory.StartsWith($OutputDirectory + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'RunDirectory and OutputDirectory must be separate non-nested directories.'
}
Assert-EuiEmptyOutput $OutputDirectory
New-Item -ItemType Directory -Path $RunDirectory | Out-Null
$checkBuild = Join-Path $RunDirectory 'check-build'
$packageBuild = Join-Path $RunDirectory 'package-build'
$receipt = [ordered]@{
    repository = $repo; version = $Version; started = (Get-Date).ToString('o')
    parallel = $Parallel; output = $OutputDirectory; status = 'RUNNING'; error = $null
    stages = @(); evidenceWarnings = @(); gui = 'NOT_RUN'; publication = 'NOT_RUN'
}
function Save-Receipt {
    $receipt | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $RunDirectory 'result.json') -Encoding utf8
}
function Get-SourceSnapshot {
    $head = @(& git -C $repo rev-parse HEAD)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read repository HEAD.' }
    $status = @(& git -C $repo status --short --untracked-files=all)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read repository status.' }
    # -z avoids Git quoting Unicode paths and delimiters in file names.
    $names = @(& git -C $repo ls-files -z --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate candidate files.' }
    $files = [ordered]@{}
    foreach ($name in @(($names -join "`n").Split([char]0) | Where-Object { $_ } | Sort-Object -Unique)) {
        $path = Join-Path $repo $name
        $files[$name] = if (Test-Path -LiteralPath $path -PathType Leaf) {
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        } else { 'MISSING' }
    }
    return [ordered]@{ head = $head[0]; status = $status; files = $files }
}
function Assert-SourceUnchanged {
    param([string]$Stage)
    $after = Get-SourceSnapshot
    $after | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $RunDirectory "source-after-$Stage.json") -Encoding utf8
    if (($before | ConvertTo-Json -Depth 5 -Compress) -cne ($after | ConvertTo-Json -Depth 5 -Compress)) {
        throw "Candidate source or Git state changed during $Stage; discard the pass and freeze a new candidate."
    }
}
function Invoke-Stage {
    param([string]$Name, [string]$Executable, [string[]]$Arguments)
    $stage = [ordered]@{ name = $Name; command = $Executable; arguments = $Arguments; started = (Get-Date).ToString('o'); status = 'RUNNING'; exitCode = $null }
    $receipt.stages += $stage
    Save-Receipt
    try {
        & $Executable @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $RunDirectory "$Name.log") | Out-Host
        $stage.exitCode = $LASTEXITCODE
        $stage.status = if ($LASTEXITCODE -eq 0) { 'PASS' } else { 'FAIL' }
        if ($stage.exitCode -ne 0) { throw "$Name failed with exit code $($stage.exitCode). See $RunDirectory/$Name.log." }
    } catch {
        $stage.status = 'FAIL'
        $stage.error = $_.Exception.Message
        throw
    } finally {
        $stage.finished = (Get-Date).ToString('o')
        Save-Receipt
    }
}
function Preserve-BuildEvidence {
    param([string]$Build)
    if (-not (Test-Path -LiteralPath $Build)) { return }
    $saved = Join-Path $RunDirectory ((Split-Path -Leaf $Build) + '-evidence')
    New-Item -ItemType Directory -Path $saved -Force | Out-Null
    foreach ($name in @('CMakeCache.txt', 'CMakeFiles/CMakeConfigureLog.yaml', 'Testing/Temporary/LastTest.log', 'Testing/Temporary/LastTestsFailed.log', 'CTestTestfile.cmake')) {
        $path = Join-Path $Build $name
        if (Test-Path -LiteralPath $path -PathType Leaf) { Copy-Item -LiteralPath $path -Destination $saved }
    }
}
$lock = $null
$previousDebugPath = $env:MSBUILDDEBUGPATH
try {
    $lockPath = Join-Path $repo 'out/neoeditor-release.lock'
    New-Item -ItemType Directory -Path (Split-Path -Parent $lockPath) -Force | Out-Null
    $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $versionSource = Get-Content -LiteralPath (Join-Path $repo 'cmake/NeoEditorVersion.cmake') -Raw
    if ($versionSource -notmatch 'set\(NEO_EDITOR_VERSION "([^"]+)"' -or $Matches[1] -ne $Version) {
        throw 'Requested version differs from cmake/NeoEditorVersion.cmake; synchronize the candidate first.'
    }
    $before = Get-SourceSnapshot
    $before | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $RunDirectory 'source-before.json') -Encoding utf8
    $tools = Get-EuiReleaseTools -CMake $CMake -Generator $Generator -Python $Python -Dumpbin $Dumpbin -NeedPython -NeedDumpbin
    $tools | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $RunDirectory 'tools.json') -Encoding utf8
    Invoke-Stage 'diff-check' 'git' @('-C', $repo, '-c', 'core.whitespace=cr-at-eol', 'diff', '--check', 'HEAD')
    Invoke-Stage 'i18n-preflight' $tools.Python @((Join-Path $repo 'tests/tools/check_i18n.py'))
    Assert-SourceUnchanged 'preflight'
    if ($PreflightOnly) {
        $receipt.status = 'PREFLIGHT_PASS'
    } else {
        $env:MSBUILDDEBUGPATH = Join-Path $RunDirectory 'msbuild-diagnostics'
        New-Item -ItemType Directory -Path $env:MSBUILDDEBUGPATH | Out-Null
        $pwsh = Join-Path $PSHOME 'pwsh.exe'
        Invoke-Stage 'correctness' $pwsh @('-NoProfile', '-NonInteractive', '-File', (Join-Path $repo 'scripts/check-neoeditor.ps1'),
            '-BuildDirectory', $checkBuild, '-CMake', $tools.CMake, '-Python', $tools.Python, '-Generator', $tools.Generator, '-Parallel', "$Parallel")
        Assert-SourceUnchanged 'correctness'
        Invoke-Stage 'package' $pwsh @('-NoProfile', '-NonInteractive', '-File', (Join-Path $repo 'scripts/package-neoeditor.ps1'),
            '-Version', $Version, '-BuildDirectory', $packageBuild, '-OutputDirectory', $OutputDirectory,
            '-CMake', $tools.CMake, '-Dumpbin', $tools.Dumpbin, '-Generator', $tools.Generator, '-Parallel', "$Parallel")
        Assert-SourceUnchanged 'package'
        $exe = Join-Path $OutputDirectory "EUI-Edits-$Version-windows-x64.exe"
        $receipt.executable = $exe
        $receipt.sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
        $receipt.status = 'READY_FOR_GUI'
        foreach ($build in @($checkBuild, $packageBuild)) {
            Preserve-BuildEvidence $build
            $safeBuild = Assert-EuiRepositoryPath $build $RunDirectory 'Owned build tree'
            Remove-Item -LiteralPath $safeBuild -Recurse -Force
        }
    }
} catch {
    $receipt.status = 'FAIL'
    $receipt.error = $_.Exception.Message
    throw
} finally {
    try {
        try {
            Get-SourceSnapshot | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $RunDirectory 'source-final.json') -Encoding utf8
        } catch { $receipt.evidenceWarnings += "Final source snapshot: $($_.Exception.Message)" }
        foreach ($build in @($checkBuild, $packageBuild)) {
            try { Preserve-BuildEvidence $build }
            catch { $receipt.evidenceWarnings += "Build evidence: $($_.Exception.Message)" }
        }
    } finally {
        $env:MSBUILDDEBUGPATH = $previousDebugPath
        if ($lock) { $lock.Dispose() }
    }
    $receipt.finished = (Get-Date).ToString('o')
    Save-Receipt
    Write-Output "Preparation status: $($receipt.status). Evidence: $RunDirectory"
}
