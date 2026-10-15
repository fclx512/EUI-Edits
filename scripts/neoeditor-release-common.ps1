#Requires -Version 7
# Shared guards for local and CI preparation; no build or publication here.
function Assert-EuiRepositoryPath {
    param([string]$Path, [string]$Repository, [string]$Label)
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $root = [IO.Path]::GetFullPath($Repository).TrimEnd('\', '/')
    if (-not $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must be inside the current repository and separate from its root."
    }
    $ancestor = $full
    while ($ancestor -and $ancestor -ne $root) {
        if (Test-Path -LiteralPath $ancestor) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "$Label must not traverse a junction or symbolic link: $ancestor"
            }
        }
        $ancestor = Split-Path -Parent $ancestor
    }
    return $full
}

function Assert-EuiEmptyOutput {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) {
        if (-not (Test-Path -LiteralPath $Path -PathType Container) -or
            @(Get-ChildItem -LiteralPath $Path -Force).Count -gt 0) {
            throw 'OutputDirectory must be empty or absent. Existing files are never removed or overwritten.'
        }
    }
}

function Get-EuiReleaseTools {
    param([string]$CMake, [string]$Generator, [string]$Python, [string]$Dumpbin,
        [switch]$NeedPython, [switch]$NeedDumpbin)
    if ($env:OS -ne 'Windows_NT') { throw 'EUI-Edits release preparation requires Windows with MSVC x64.' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsArgs = @('-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64')
    if ($Generator) {
        if ($Generator -eq 'Visual Studio 17 2022') { $vsArgs += @('-version', '[17.0,18.0)') }
        elseif ($Generator -eq 'Visual Studio 18 2026') { $vsArgs += @('-version', '[18.0,19.0)') }
        else { throw 'Release preparation supports Visual Studio 17 2022 or Visual Studio 18 2026 only.' }
    }
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere not found; install Visual Studio with MSVC x64 tools.' }
    $installation = @(& $vswhere @vsArgs -property installationPath) | Select-Object -First 1
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'The selected Visual Studio installation with MSVC x64 tools was not found.' }
    if (-not $Generator) {
        $vsVersion = @(& $vswhere @vsArgs -property installationVersion) | Select-Object -First 1
        if ($vsVersion -like '18.*') { $Generator = 'Visual Studio 18 2026' }
        elseif ($vsVersion -like '17.*') { $Generator = 'Visual Studio 17 2022' }
        else { throw 'Unsupported Visual Studio version.' }
    }
    if (-not $CMake) {
        $command = Get-Command cmake -ErrorAction SilentlyContinue
        if ($command) { $CMake = $command.Source }
        else { $CMake = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' }
    }
    if (-not (Test-Path -LiteralPath $CMake -PathType Leaf)) { throw 'CMake not found. Pass -CMake with an absolute executable path.' }
    $CMake = (Get-Item -LiteralPath $CMake).FullName
    $help = @(& $CMake --help)
    if ($LASTEXITCODE -ne 0 -or ($help -join "`n") -notmatch [Regex]::Escape($Generator)) {
        throw 'The selected CMake does not support the selected Visual Studio generator.'
    }
    $ctest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
    if ($NeedPython) {
        if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) { throw 'ctest.exe must be next to the selected CMake executable.' }
        if (-not $Python) {
            $command = Get-Command python -ErrorAction SilentlyContinue
            if ($command) { $Python = $command.Source }
        }
        if (-not $Python -or -not (Test-Path -LiteralPath $Python -PathType Leaf)) { throw 'Python 3 not found. Pass -Python with an absolute executable path.' }
        $Python = (Get-Item -LiteralPath $Python).FullName
        $pythonVersion = @(& $Python --version 2>&1)
        if ($LASTEXITCODE -ne 0 -or ($pythonVersion -join '') -notmatch '^Python 3\.') { throw 'Python 3 is required.' }
    }
    if ($NeedDumpbin) {
        if (-not $Dumpbin) {
            $Dumpbin = @(& $vswhere @vsArgs -find 'VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe') | Select-Object -Last 1
        }
        if (-not $Dumpbin -or -not (Test-Path -LiteralPath $Dumpbin -PathType Leaf)) { throw 'dumpbin not found. Pass -Dumpbin with an absolute executable path.' }
        $Dumpbin = (Get-Item -LiteralPath $Dumpbin).FullName
    }
    return [pscustomobject]@{ CMake = $CMake; CTest = $ctest; Generator = $Generator; Python = $Python; Dumpbin = $Dumpbin; VisualStudio = $installation }
}
