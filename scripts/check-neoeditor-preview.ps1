# Read-only identity check before handing over a development preview.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$BuildDirectory,
    [string]$Version
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $PSBoundParameters.ContainsKey('Version')) {
    $source = Get-Content -LiteralPath (Join-Path $repo 'cmake/NeoEditorVersion.cmake') -Raw
    if ($source -notmatch '(?m)^set\(NEO_EDITOR_VERSION "([^"]+)" CACHE STRING ') {
        throw 'Cannot read the default EUI-Edits version.'
    }
    $Version = $Matches[1]
}
if ($Version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$') {
    throw "Invalid EUI-Edits version: $Version"
}
$parts = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
if ($parts | Where-Object { $_ -gt 65535 }) { throw "Version fields exceed PE limits: $Version" }
$number = $parts -join ','
$number += ',0'

$exePath = [IO.Path]::GetFullPath($Exe)
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) { throw "EXE not found: $exePath" }
$pe = (Get-Item -LiteralPath $exePath).VersionInfo
if ($pe.ProductVersion -ne $Version -or $pe.FileVersion -notin @($Version, (($parts -join '.') + '.0')) -or
    $pe.ProductMajorPart -ne $parts[0] -or $pe.ProductMinorPart -ne $parts[1] -or
    $pe.ProductBuildPart -ne $parts[2] -or $pe.ProductPrivatePart -ne 0 -or
    $pe.FileMajorPart -ne $parts[0] -or
    $pe.FileMinorPart -ne $parts[1] -or $pe.FileBuildPart -ne $parts[2] -or
    $pe.FilePrivatePart -ne 0) {
    throw "EXE version mismatch: expected $Version; PE ProductVersion=$($pe.ProductVersion), FileVersion=$($pe.FileVersion)"
}

$buildPath = $null
if ($BuildDirectory) {
    $buildPath = [IO.Path]::GetFullPath($BuildDirectory)
    $cachePath = Join-Path $buildPath 'CMakeCache.txt'
    $headerPath = Join-Path $buildPath 'generated/neoeditor/neo_editor_version_generated.h'
    $rcPath = Join-Path $buildPath 'neo_editor_version.rc'
    foreach ($path in @($cachePath, $headerPath, $rcPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Build identity file missing: $path" }
    }
    $cache = Get-Content -LiteralPath $cachePath -Raw
    if ($cache -notmatch '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=([^\r\n]+)') { throw 'Build cache has no source identity.' }
    $cacheRepo = [IO.Path]::GetFullPath($Matches[1]).TrimEnd('\', '/')
    if (-not $cacheRepo.Equals($repo.TrimEnd('\', '/'), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Build cache belongs to another checkout: $cacheRepo"
    }
    if ($cache -notmatch '(?m)^NEO_EDITOR_VERSION:[^=]+=([^\r\n]+)') {
        throw 'Build cache has no EUI-Edits version.'
    }
    if ($Matches[1] -cne $Version) {
        throw "Build cache version does not match expected $Version."
    }
    $header = Get-Content -LiteralPath $headerPath -Raw
    if ($header -notmatch '(?m)^inline constexpr char kVersion\[\] = "([^"]+)";') {
        throw 'Generated version header has no kVersion.'
    }
    if ($Matches[1] -cne $Version) {
        throw "Generated version header does not match expected $Version."
    }
    $rc = Get-Content -LiteralPath $rcPath -Raw
    foreach ($field in @('FILEVERSION', 'PRODUCTVERSION')) {
        if ($rc -notmatch ('(?m)^\s*' + $field + '\s+' + [regex]::Escape($number) + '\s*$')) {
            throw "Version RC $field does not match expected $number."
        }
        $literal = [regex]::Escape($Version + '\0')
        if ($rc -notmatch ('(?m)^\s*VALUE\s+"' + $field + '",\s*"' + $literal + '"\s*$')) {
            throw "Version RC string $field does not match expected $Version."
        }
    }
}

[pscustomobject]@{
    Status = 'PASS'
    Exe = $exePath
    Sha256 = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
    Version = $Version
    PeProductVersion = $pe.ProductVersion
    PeFileVersion = $pe.FileVersion
    BuildDirectory = $buildPath
}
