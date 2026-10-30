#Requires -Version 7
[CmdletBinding()]
param([string]$EvidenceDirectory, [string]$Python = 'D:/Python312/python.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repo 'scripts/neoeditor-release-common.ps1')
if (-not $EvidenceDirectory) { $EvidenceDirectory = Join-Path $repo ('out/release-script-tests-' + [Guid]::NewGuid().ToString('N')) }
$EvidenceDirectory = Assert-EuiRepositoryPath $EvidenceDirectory $repo 'Test evidence'
if (Test-Path -LiteralPath $EvidenceDirectory) { throw 'Test evidence directory must not exist.' }
New-Item -ItemType Directory -Path $EvidenceDirectory | Out-Null
$pwsh = Join-Path $PSHOME 'pwsh.exe'
$results = @()
# Both Windows workflows call this preflight once before candidate preparation.
# Python checker regressions are separate from the CTest application cases.
$regressionOutput = @(& $Python -B (Join-Path $repo 'tests/python/test_check_i18n.py') -v 2>&1)
$regressionCode = $LASTEXITCODE
$regressionOutput | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'i18n-checker-regressions.log') -Encoding utf8
$results += [ordered]@{ name = 'i18n-checker-regressions'; status = if ($regressionCode -eq 0) { 'PASS' } else { 'FAIL' }; exitCode = $regressionCode }
if ($regressionCode -ne 0) {
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'results.json') -Encoding utf8
    throw "Translation checker regressions failed with exit code $regressionCode. See $EvidenceDirectory/i18n-checker-regressions.log."
}
function Check-Invocation {
    param([string]$Name, [string]$Script, [string[]]$Arguments, [bool]$ShouldPass, [string]$ExpectedText)
    $output = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $repo "scripts/$Script") @Arguments 2>&1)
    $code = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $EvidenceDirectory "$Name.log") -Encoding utf8
    if (($code -eq 0) -ne $ShouldPass -or ($output -join "`n") -notmatch [Regex]::Escape($ExpectedText)) {
        throw "$Name returned unexpected exit/output ($code): $($output -join "`n")"
    }
    $script:results += [ordered]@{ name = $Name; status = 'PASS'; exitCode = $code }
}
function Check-FailedStage {
    param([string]$Name, [int]$InjectedExit, [string]$ExpectedError)
    # An isolated no-checkout clone supplies HEAD/index without compiling or
    # changing the user's repository. Missing tracked files stay in the manifest.
    $fixture = Join-Path $EvidenceDirectory $Name
    & git clone --local --shared --no-checkout $repo $fixture 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare isolated failure fixture.' }
    foreach ($directory in @('scripts', 'cmake', 'tests/tools')) {
        New-Item -ItemType Directory -Path (Join-Path $fixture $directory) | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $repo 'scripts/prepare-neoeditor-release.ps1'), (Join-Path $repo 'scripts/neoeditor-release-common.ps1') -Destination (Join-Path $fixture 'scripts')
    Copy-Item -LiteralPath (Join-Path $repo 'cmake/NeoEditorVersion.cmake') -Destination (Join-Path $fixture 'cmake')
    Copy-Item -LiteralPath (Join-Path $repo '.gitignore') -Destination $fixture
    'raise SystemExit(0)' | Set-Content -LiteralPath (Join-Path $fixture 'scripts/check-docs.py') -Encoding utf8
    @("from pathlib import Path", "Path(__file__).parents[2].joinpath('injected-source-change.txt').write_text('changed')", "raise SystemExit($InjectedExit)") |
        Set-Content -LiteralPath (Join-Path $fixture 'tests/tools/check_i18n.py') -Encoding utf8
    $run = Join-Path $fixture 'out/run'
    $output = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $fixture 'scripts/prepare-neoeditor-release.ps1') `
        -Version $version -Python $Python -RunDirectory $run -PreflightOnly 2>&1)
    $code = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $EvidenceDirectory "$Name.log") -Encoding utf8
    $record = Get-Content -LiteralPath (Join-Path $run 'result.json') -Raw | ConvertFrom-Json
    if ($code -eq 0 -or $record.status -ne 'FAIL' -or $record.error -notmatch [Regex]::Escape($ExpectedError)) { throw "$Name did not fail as expected." }
    if (@($record.stages | Where-Object status -eq 'RUNNING').Count -gt 0) { throw 'Failed stages were left RUNNING.' }
    $i18n = @($record.stages | Where-Object name -eq 'i18n-preflight')[0]
    if ($i18n.exitCode -ne $InjectedExit) { throw 'Native exit code was lost.' }
    if (-not (Test-Path -LiteralPath (Join-Path $run 'source-final.json'))) { throw 'Failure source snapshot was lost.' }
    if (Test-Path -LiteralPath (Join-Path $run 'check-build')) { throw 'Failure continued into compilation.' }
    $lockCheck = [IO.File]::Open((Join-Path $fixture 'out/neoeditor-release.lock'), [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $lockCheck.Dispose()
    $script:results += [ordered]@{ name = $Name; status = 'PASS'; exitCode = $code }
}
function Check-I18nRegressionFailure {
    # Invoke the real CI preflight with a failing Python suite in a separate
    # repository-shaped fixture. It must stop before any release guard runs.
    $fixture = Join-Path $EvidenceDirectory 'i18n-regression-failure'
    foreach ($directory in @('scripts', 'tests/tools', 'tests/python')) {
        New-Item -ItemType Directory -Path (Join-Path $fixture $directory) -Force | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $repo 'scripts/neoeditor-release-common.ps1') -Destination (Join-Path $fixture 'scripts')
    Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $fixture 'tests/tools/test_release_scripts.ps1')
    'print("injected checker regression failure"); raise SystemExit(7)' |
        Set-Content -LiteralPath (Join-Path $fixture 'tests/python/test_check_i18n.py') -Encoding utf8
    $run = Join-Path $fixture 'out/run'
    $output = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $fixture 'tests/tools/test_release_scripts.ps1') `
        -EvidenceDirectory $run -Python $Python 2>&1)
    $code = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'i18n-regression-failure.log') -Encoding utf8
    $record = @(Get-Content -LiteralPath (Join-Path $run 'results.json') -Raw | ConvertFrom-Json)
    if ($code -eq 0 -or $record.Count -ne 1 -or $record[0].status -ne 'FAIL' -or
        $record[0].exitCode -ne 7 -or ($output -join "`n") -notmatch 'regressions failed with exit code 7') {
        throw 'CI preflight did not stop on the failing translation checker suite.'
    }
    if (Test-Path -LiteralPath (Join-Path $run 'occupied')) { throw 'Failed Python regressions continued into release guard checks.' }
    $script:results += [ordered]@{ name = 'i18n-regression-failure'; status = 'PASS'; exitCode = $code }
}
function Check-DocsPreflightFailure {
    # A documentation failure must stop preparation before any build directory.
    $fixture = Join-Path $EvidenceDirectory 'docs-preflight-failure'
    & git clone --local --shared --no-checkout $repo $fixture 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare documentation failure fixture.' }
    foreach ($directory in @('scripts', 'cmake', 'tests/tools')) {
        New-Item -ItemType Directory -Path (Join-Path $fixture $directory) | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $repo 'scripts/prepare-neoeditor-release.ps1'), (Join-Path $repo 'scripts/neoeditor-release-common.ps1') -Destination (Join-Path $fixture 'scripts')
    Copy-Item -LiteralPath (Join-Path $repo 'cmake/NeoEditorVersion.cmake') -Destination (Join-Path $fixture 'cmake')
    Copy-Item -LiteralPath (Join-Path $repo '.gitignore') -Destination $fixture
    'raise SystemExit(0)' | Set-Content -LiteralPath (Join-Path $fixture 'tests/tools/check_i18n.py') -Encoding utf8
    'print("DOC003 broken fixture link"); raise SystemExit(9)' | Set-Content -LiteralPath (Join-Path $fixture 'scripts/check-docs.py') -Encoding utf8
    $run = Join-Path $fixture 'out/run'
    $output = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $fixture 'scripts/prepare-neoeditor-release.ps1') `
        -Version $version -Python $Python -RunDirectory $run 2>&1)
    $code = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'docs-preflight-failure.log') -Encoding utf8
    $record = Get-Content -LiteralPath (Join-Path $run 'result.json') -Raw | ConvertFrom-Json
    $stage = @($record.stages | Where-Object name -eq 'docs-preflight')
    if ($code -eq 0 -or $record.status -ne 'FAIL' -or $stage.Count -ne 1 -or $stage[0].exitCode -ne 9) {
        throw 'Documentation preflight did not retain the failing stage and exit code.'
    }
    if (Test-Path -LiteralPath (Join-Path $run 'check-build')) { throw 'Documentation failure continued into compilation.' }
    $script:results += [ordered]@{ name = 'docs-preflight-failure'; status = 'PASS'; exitCode = $code }
}
function Check-DocsDeliveryFailure {
    param([string]$Name, [string]$ScriptName, [string[]]$ExtraArguments)
    $fixture = Join-Path $EvidenceDirectory $Name
    New-Item -ItemType Directory -Path (Join-Path $fixture 'scripts') | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo "scripts/$ScriptName") -Destination (Join-Path $fixture "scripts/$ScriptName")
    'print("DOC006 pending review"); raise SystemExit(9)' | Set-Content -LiteralPath (Join-Path $fixture 'scripts/check-docs.py') -Encoding utf8
    $output = @(& $pwsh -NoProfile -NonInteractive -File (Join-Path $fixture "scripts/$ScriptName") -Python $Python @ExtraArguments 2>&1)
    $code = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $EvidenceDirectory "$Name.log") -Encoding utf8
    if ($code -eq 0 -or ($output -join "`n") -notmatch 'Documentation checks failed before') {
        throw "$Name did not stop on pending documentation review."
    }
    if (Test-Path -LiteralPath (Join-Path $fixture 'handoff')) { throw 'Pending review created a handoff snapshot.' }
    $script:results += [ordered]@{ name = $Name; status = 'PASS'; exitCode = $code }
}
function Check-BuildRetention {
    param([string]$Name, [bool]$Keep)
    # Only tiny simulated trees in this fixture are created/removed. The real
    # preparation script and receipt run normally; no CMake/MSBuild is invoked.
    $fixture = Join-Path $EvidenceDirectory $Name
    & git clone --local --shared --no-checkout $repo $fixture 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare isolated retention fixture.' }
    foreach ($directory in @('scripts', 'cmake', 'tests/tools')) {
        New-Item -ItemType Directory -Path (Join-Path $fixture $directory) | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $repo 'scripts/prepare-neoeditor-release.ps1'), (Join-Path $repo 'scripts/neoeditor-release-common.ps1') -Destination (Join-Path $fixture 'scripts')
    Copy-Item -LiteralPath (Join-Path $repo 'cmake/NeoEditorVersion.cmake') -Destination (Join-Path $fixture 'cmake')
    Copy-Item -LiteralPath (Join-Path $repo '.gitignore') -Destination $fixture
    'raise SystemExit(0)' | Set-Content -LiteralPath (Join-Path $fixture 'scripts/check-docs.py') -Encoding utf8
    'raise SystemExit(0)' | Set-Content -LiteralPath (Join-Path $fixture 'tests/tools/check_i18n.py') -Encoding utf8
    @'
param([string]$BuildDirectory, [string]$CMake, [string]$Python, [string]$Generator, [int]$Parallel)
New-Item -ItemType Directory -Path $BuildDirectory | Out-Null
'SIMULATED_CHECK_CACHE' | Set-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt')
exit 0
'@ | Set-Content -LiteralPath (Join-Path $fixture 'scripts/check-neoeditor.ps1') -Encoding utf8
    @'
param([string]$Version, [string]$BuildDirectory, [string]$OutputDirectory, [string]$CMake, [string]$Dumpbin, [string]$Generator, [int]$Parallel)
New-Item -ItemType Directory -Path $BuildDirectory, $OutputDirectory | Out-Null
'SIMULATED_PACKAGE_CACHE' | Set-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt')
'SIMULATED_ASSET' | Set-Content -LiteralPath (Join-Path $OutputDirectory "EUI-Edits-$Version-windows-x64.exe")
exit 0
'@ | Set-Content -LiteralPath (Join-Path $fixture 'scripts/package-neoeditor.ps1') -Encoding utf8
    $run = Join-Path $fixture 'out/run'
    $candidate = Join-Path $fixture 'out/candidate'
    $arguments = @('-NoProfile', '-NonInteractive', '-File', (Join-Path $fixture 'scripts/prepare-neoeditor-release.ps1'),
        '-Version', $version, '-Python', $Python, '-RunDirectory', $run, '-OutputDirectory', $candidate)
    if ($Keep) { $arguments += '-KeepBuildDirectories' }
    $output = @(& $pwsh @arguments 2>&1)
    $code = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $EvidenceDirectory "$Name.log") -Encoding utf8
    $record = Get-Content -LiteralPath (Join-Path $run 'result.json') -Raw | ConvertFrom-Json
    if ($code -ne 0 -or $record.status -ne 'READY_FOR_GUI' -or $record.keepBuildDirectories -ne $Keep -or
        $record.gui -ne 'NOT_RUN' -or $record.publication -ne 'NOT_RUN') { throw "$Name did not preserve the expected status/retention boundaries." }
    foreach ($buildName in @('check-build', 'package-build')) {
        if ((Test-Path -LiteralPath (Join-Path $run $buildName)) -ne $Keep) { throw "${Name}: incorrect build retention for $buildName." }
        if (-not (Test-Path -LiteralPath (Join-Path $run "$buildName-evidence/CMakeCache.txt"))) { throw "$Name lost the simulated build evidence." }
    }
    if ($record.sha256 -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $record.executable).Hash) { throw "$Name lost candidate identity." }
    $script:results += [ordered]@{ name = $Name; status = 'PASS'; exitCode = $code }
}
$absentBuild = Join-Path $EvidenceDirectory 'never-build'
$emptyOutput = Join-Path $EvidenceDirectory 'empty-output'
$occupied = Join-Path $EvidenceDirectory 'occupied'
New-Item -ItemType Directory -Path $occupied | Out-Null
$sentinel = Join-Path $occupied 'keep.txt'
'Existing user candidate' | Set-Content -LiteralPath $sentinel
$sentinelHash = (Get-FileHash -LiteralPath $sentinel).Hash
Check-Invocation 'occupied-output' 'package-neoeditor.ps1' @('-BuildDirectory', $absentBuild, '-OutputDirectory', $occupied, '-CMake', 'missing-cmake.exe') $false 'OutputDirectory must be empty'
Check-Invocation 'outside-build' 'check-neoeditor.ps1' @('-BuildDirectory', (Join-Path (Split-Path -Parent $repo) 'foreign-build')) $false 'inside the current repository'
Check-Invocation 'root-build' 'check-neoeditor.ps1' @('-BuildDirectory', $repo) $false 'inside the current repository'
Check-Invocation 'nested-output' 'package-neoeditor.ps1' @('-BuildDirectory', $absentBuild, '-OutputDirectory', (Join-Path $absentBuild 'artifacts')) $false 'separate non-nested'
Check-Invocation 'unsupported-generator' 'check-neoeditor.ps1' @('-BuildDirectory', $absentBuild, '-Generator', 'Ninja') $false 'supports Visual Studio'
$existingBuild = Join-Path $EvidenceDirectory 'existing-build'
New-Item -ItemType Directory -Path $existingBuild | Out-Null
Check-Invocation 'existing-build' 'check-neoeditor.ps1' @('-BuildDirectory', $existingBuild) $false 'new build directory'
$foreignCache = Join-Path $EvidenceDirectory 'foreign-cache'
New-Item -ItemType Directory -Path $foreignCache | Out-Null
'CMAKE_HOME_DIRECTORY:INTERNAL=C:/another-checkout' | Set-Content -LiteralPath (Join-Path $foreignCache 'CMakeCache.txt')
Check-Invocation 'foreign-cache' 'package-neoeditor.ps1' @('-BuildDirectory', $foreignCache, '-OutputDirectory', $emptyOutput, '-SkipBuild', '-PreflightOnly') $false 'another source checkout'
Check-Invocation 'check-tools' 'check-neoeditor.ps1' @('-BuildDirectory', $absentBuild, '-Python', $Python, '-PreflightOnly') $true 'VisualStudio'
Check-Invocation 'package-tools' 'package-neoeditor.ps1' @('-BuildDirectory', $absentBuild, '-OutputDirectory', $emptyOutput, '-PreflightOnly') $true 'Dumpbin'
$versionSource = Get-Content -LiteralPath (Join-Path $repo 'cmake/NeoEditorVersion.cmake') -Raw
if ($versionSource -notmatch 'set\(NEO_EDITOR_VERSION "([^"]+)"') { throw 'Missing source version.' }
$version = $Matches[1]
$prepareRun = Join-Path $EvidenceDirectory 'prepare-preflight'
Check-Invocation 'prepare-preflight' 'prepare-neoeditor-release.ps1' @('-Version', $version, '-Python', $Python, '-RunDirectory', $prepareRun, '-OutputDirectory', $emptyOutput, '-PreflightOnly') $true 'PREFLIGHT_PASS'
$receipt = Get-Content -LiteralPath (Join-Path $prepareRun 'result.json') -Raw | ConvertFrom-Json
if ($receipt.status -ne 'PREFLIGHT_PASS' -or $receipt.gui -ne 'NOT_RUN' -or $receipt.publication -ne 'NOT_RUN') { throw 'Incorrect preflight status boundaries.' }
Check-Invocation 'wrong-version' 'prepare-neoeditor-release.ps1' @('-Version', '65535.65535.65535', '-RunDirectory', (Join-Path $EvidenceDirectory 'wrong-version-run'), '-OutputDirectory', $emptyOutput, '-PreflightOnly') $false 'synchronize the candidate first'
$lock = [IO.File]::Open((Join-Path $repo 'out/neoeditor-release.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    Check-Invocation 'concurrent-preparation' 'prepare-neoeditor-release.ps1' @('-Version', $version, '-RunDirectory', (Join-Path $EvidenceDirectory 'locked-run'), '-OutputDirectory', $emptyOutput, '-PreflightOnly') $false 'FAIL'
} finally { $lock.Dispose() }
Check-FailedStage 'native-stage-failure' 7 'i18n-preflight failed with exit code 7'
Check-FailedStage 'source-drift' 0 'Candidate source or Git state changed'
Check-I18nRegressionFailure
Check-DocsPreflightFailure
Check-DocsDeliveryFailure 'docs-preview-failure' 'check-neoeditor-preview.ps1' @('-Exe', (Join-Path $EvidenceDirectory 'absent.exe'))
Check-DocsDeliveryFailure 'docs-handoff-failure' 'write-handoff-summary.ps1' @('-Repository', (Join-Path $EvidenceDirectory 'docs-handoff-failure'), '-OutputDirectory', (Join-Path $EvidenceDirectory 'docs-handoff-failure/handoff'))
Check-BuildRetention 'default-build-cleanup' $false
Check-BuildRetention 'explicit-build-retention' $true
if (Test-Path -LiteralPath $absentBuild) { throw 'A rejected request created a build tree.' }
if (Test-Path -LiteralPath $emptyOutput) { throw 'Preflight created a candidate output.' }
if ((Get-FileHash -LiteralPath $sentinel).Hash -ne $sentinelHash) { throw 'Existing candidate was changed.' }
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'results.json') -Encoding utf8
Write-Output "Release script checks passed: $($results.Count). Evidence: $EvidenceDirectory"
exit 0
