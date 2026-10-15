#Requires -Version 7
[CmdletBinding()]
param(
    [ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$')]
    [string]$Version,
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [string]$CMake,
    [string]$Dumpbin,
    [string]$Generator,
    [switch]$SkipBuild,
    [ValidateRange(1, 32)][int]$Parallel = 2,
    [switch]$PreflightOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'neoeditor-release-common.ps1')
if (-not $Version) {
    $versionSource = Get-Content -LiteralPath (Join-Path $repo 'cmake/NeoEditorVersion.cmake') -Raw
    if ($versionSource -notmatch 'set\(NEO_EDITOR_VERSION "([^"]+)"') { throw 'Cannot read the default product version.' }
    $Version = $Matches[1]
}
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo ('build-neoeditor-package-' + [Guid]::NewGuid().ToString('N')) }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo ("out/euiedits-$Version-single-exe") }
$BuildDirectory = Assert-EuiRepositoryPath $BuildDirectory $repo 'BuildDirectory'
$OutputDirectory = Assert-EuiRepositoryPath $OutputDirectory $repo 'OutputDirectory'
if ($OutputDirectory -eq $BuildDirectory -or
    $OutputDirectory.StartsWith($BuildDirectory + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $BuildDirectory.StartsWith($OutputDirectory + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory and OutputDirectory must be separate non-nested directories.'
}
Assert-EuiEmptyOutput $OutputDirectory
if ($env:OS -ne 'Windows_NT') { throw 'EUI-Edits packaging requires Windows with MSVC x64.' }
if ($BuildDirectory -eq $repo) { throw 'BuildDirectory must be separate from the source directory.' }
if (-not $SkipBuild -and (Test-Path -LiteralPath $BuildDirectory)) {
    throw 'Use a new, empty build directory. Existing directories are never reset or deleted.'
}
$tools = Get-EuiReleaseTools -CMake $CMake -Generator $Generator -Dumpbin $Dumpbin -NeedDumpbin
$CMake = $tools.CMake; $Generator = $tools.Generator; $Dumpbin = $tools.Dumpbin
if ($SkipBuild) {
    $existingCache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw
    if ($existingCache -notmatch '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=([^\r\n]+)') { throw 'Build cache has no source directory identity.' }
    $cacheSource = [IO.Path]::GetFullPath($Matches[1]).TrimEnd('\', '/')
    if (-not $cacheSource.Equals($repo.TrimEnd('\', '/'), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Build cache belongs to another source checkout.'
    }
}
if ($PreflightOnly) { $tools; return }
if (-not $SkipBuild) {
    & $CMake -S $repo -B $BuildDirectory -G $Generator -A x64 `
        '-DEUI_BUILD_NEOEDITOR_ONLY=ON' '-DEUI_WINDOW_BACKEND=win32' '-DEUI_RENDER_BACKEND=d2d' `
        '-DEUI_DEPS_MODE=bundled' '-DEUI_BUILD_SHARED=OFF' '-DEUI_ENABLE_INSTALL=OFF' `
        '-DEUI_ENABLE_MODULES=OFF' '-DEUI_BUILD_TEST_FIXTURES=OFF' `
        '-DCMAKE_DISABLE_FIND_PACKAGE_CURL=TRUE' '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>' `
        "-DNEO_EDITOR_VERSION=$Version"
    if ($LASTEXITCODE -ne 0) { throw 'EUI-Edits CMake configuration failed.' }
    & $CMake --build $BuildDirectory --config Release --target neo_editor --parallel $Parallel
    if ($LASTEXITCODE -ne 0) { throw 'EUI-Edits Release build failed.' }
}
$cache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw
$required = @{
    NEO_EDITOR_VERSION = $Version; EUI_BUILD_NEOEDITOR_ONLY = 'ON'; EUI_WINDOW_BACKEND = 'win32';
    EUI_RENDER_BACKEND = 'd2d'; EUI_BUILD_SHARED = 'OFF'; CMAKE_GENERATOR_PLATFORM = 'x64';
    EUI_DEPS_MODE = 'bundled'; CMAKE_DISABLE_FIND_PACKAGE_CURL = 'TRUE';
    EUI_ENABLE_INSTALL = 'OFF'; EUI_ENABLE_MODULES = 'OFF'; EUI_BUILD_TEST_FIXTURES = 'OFF';
    CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreaded$<$<CONFIG:Debug>:Debug>'
}
foreach ($entry in $required.GetEnumerator()) {
    if ($cache -notmatch ('(?m)^' + [Regex]::Escape($entry.Key) + ':[^=]+=' + [Regex]::Escape($entry.Value) + '\r?$')) {
        throw ('Build cache does not match the package configuration: ' + $entry.Key)
    }
}
$exe = Join-Path $BuildDirectory 'Release/neo_editor.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Release/neo_editor.exe missing.' }
$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
if ($info.ProductName -ne 'EUI-Edits' -or $info.ProductVersion -ne $Version -or $info.FileVersion -ne $Version) {
    throw 'Executable PE product/file version does not match the requested package version.'
}
$headers = @(& $Dumpbin /HEADERS $exe)
if ($LASTEXITCODE -ne 0 -or ($headers -join "`n") -notmatch '(?im)^\s*8664 machine') { throw 'The executable must be PE x64.' }
$imports = @(& $Dumpbin /DEPENDENTS $exe)
if ($LASTEXITCODE -ne 0) { throw 'dumpbin dependency inspection failed.' }
$dlls = @($imports | ForEach-Object { if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $Matches[1] } })
if (-not $dlls.Count) { throw 'No PE imports detected; cannot verify runtime dependencies.' }
$systemDlls = @('kernel32.dll','user32.dll','gdi32.dll','advapi32.dll','shell32.dll','ole32.dll','oleaut32.dll',
    'comdlg32.dll','comctl32.dll','shlwapi.dll','winmm.dll','imm32.dll','urlmon.dll','pdh.dll',
    'd2d1.dll','dwrite.dll','dwmapi.dll','windowscodecs.dll','uxtheme.dll','winhttp.dll',
    'ws2_32.dll','bcrypt.dll','crypt32.dll','secur32.dll','version.dll','setupapi.dll','ntdll.dll')
foreach ($dll in $dlls) {
    if ($dll -notin $systemDlls -and $dll -notmatch '^api-ms-win-') {
        throw ('Unexpected runtime dependency (MSVC CRT and third-party DLLs must be static): ' + $dll)
    }
}
# Explicit source-path audit; /pathmap is applied by the standalone CMake option.
$binary = [IO.File]::ReadAllBytes($exe)
$utf8Binary = [Text.Encoding]::UTF8.GetString($binary)
$utf16Binary = [Text.Encoding]::Unicode.GetString($binary)
foreach ($path in @($repo, $BuildDirectory)) {
    foreach ($candidate in @($path, $path.Replace('\','/'))) {
        # ASCII decoding loses Chinese source paths. Audit UTF-8 narrow strings
        # and UTF-16 wide strings, including casing differences on Windows.
        if ($utf8Binary.IndexOf($candidate, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $utf16Binary.IndexOf($candidate, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw 'Executable contains a development source/build path.'
        }
    }
}
$name = "EUI-Edits-$Version-windows-x64.exe"
$publishedExe = Join-Path $OutputDirectory $name
$shaFile = $publishedExe + '.sha256'
if (Test-Path -LiteralPath $OutputDirectory) {
    $existingOutput = @(Get-ChildItem -LiteralPath $OutputDirectory -Force)
    if ($existingOutput.Count -gt 0) { throw 'OutputDirectory must be empty. Existing files are never removed or overwritten.' }
} else {
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
}

# Prove the binary carries a complete readable license bundle:
# copy only the EXE into a brand-new empty directory and export notices from it.
$verifyDirectory = Join-Path $BuildDirectory ('single-exe-verify-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $verifyDirectory | Out-Null
$verifyExe = Join-Path $verifyDirectory $name
$verifyLicense = Join-Path $verifyDirectory 'EUI-Edits-LICENSES.txt'
Copy-Item -LiteralPath $exe -Destination $verifyExe
$licenseArguments = '--export-licenses "' + $verifyLicense + '"'
$licenseExportProcess = Start-Process -FilePath $verifyExe -ArgumentList $licenseArguments `
    -Wait -PassThru -WindowStyle Hidden
if ($licenseExportProcess.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $verifyLicense)) {
    throw 'The standalone executable could not export its embedded license bundle from an empty directory.'
}
if (Test-Path -LiteralPath (Join-Path $verifyDirectory 'assets')) {
    throw 'The standalone verification directory unexpectedly contains an assets fallback.'
}
$licenseText = Get-Content -LiteralPath $verifyLicense -Raw -Encoding UTF8
# 2026-10-04 字体解耦后不再内嵌任何字体（FA 及其 SIL 许可已移除），
# 许可包只含 Apache-2.0、第三方声明与 vendored 库许可。
foreach ($requiredText in @('Apache License', 'FREETYPE LICENSES',
                            'Copyright (c) 2017 Serge Zaitsev', 'Copyright (c) 2020 YaoYuan')) {
    if (-not $licenseText.Contains($requiredText)) {
        throw ('Embedded license bundle is incomplete: ' + $requiredText)
    }
}

Copy-Item -LiteralPath $exe -Destination $publishedExe
$exeHash = (Get-FileHash -LiteralPath $publishedExe -Algorithm SHA256).Hash.ToLowerInvariant()
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($shaFile, ($exeHash + '  ' + [IO.Path]::GetFileName($publishedExe) + "`n"), $utf8)
$publishedHash = (Get-FileHash -LiteralPath $publishedExe -Algorithm SHA256).Hash.ToLowerInvariant()
if ($publishedHash -ne $exeHash) { throw 'Published executable hash changed after copying.' }
Write-Output "Executable: $publishedExe"
Write-Output "SHA256 file: $shaFile"
Write-Output "SHA256: $exeHash"
Write-Output 'Standalone EXE, embedded resources, and license export verified from an empty directory. A real GUI run remains a separate acceptance step.'
