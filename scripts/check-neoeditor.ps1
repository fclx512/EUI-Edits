#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string]$CMake,
    [string]$Python,
    [string]$Generator,
    [ValidateRange(1, 32)][int]$Parallel = 2
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($env:OS -ne 'Windows_NT') { throw 'EUI-Edits release checks require Windows with MSVC x64.' }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo ('build-neoeditor-check-' + [Guid]::NewGuid().ToString('N')) }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
if (Test-Path -LiteralPath $BuildDirectory) { throw 'Use a new build directory. Existing directories are never changed or deleted.' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not $Generator -and (Test-Path -LiteralPath $vswhere)) {
    $vsVersion = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion) | Select-Object -First 1
    if ($vsVersion -and $vsVersion.StartsWith('18.')) { $Generator = 'Visual Studio 18 2026' }
    elseif ($vsVersion -and $vsVersion.StartsWith('17.')) { $Generator = 'Visual Studio 17 2022' }
}
if (-not $Generator) { throw 'Supported Visual Studio installation not found. Pass -Generator explicitly.' }
if (-not $CMake) {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { $CMake = $command.Source }
    elseif (Test-Path -LiteralPath $vswhere) {
        $CMake = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe') | Select-Object -First 1
    }
}
if (-not $CMake -or -not (Test-Path -LiteralPath $CMake)) { throw 'CMake not found. Pass -CMake with an absolute executable path.' }
$ctest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest)) { throw 'ctest.exe must be next to the selected CMake executable.' }
if (-not $Python) {
    $command = Get-Command python -ErrorAction SilentlyContinue
    if ($command) { $Python = $command.Source }
}
if (-not $Python -or -not (Test-Path -LiteralPath $Python)) { throw 'Python 3 not found. Pass -Python with an absolute executable path.' }

& $Python (Join-Path $repo 'tests/tools/check_i18n.py')
if ($LASTEXITCODE -ne 0) { throw 'EUI-Edits translation checks failed.' }
& $CMake -S $repo -B $BuildDirectory -G $Generator -A x64 `
    '-DEUI_BUILD_APPS=OFF' '-DEUI_BUILD_USER_APPS=OFF' '-DEUI_BUILD_NEOEDITOR_ONLY=OFF' `
    '-DEUI_BUILD_TEST_FIXTURES=ON' '-DEUI_WINDOW_BACKEND=win32' '-DEUI_RENDER_BACKEND=d2d' `
    '-DEUI_DEPS_MODE=bundled' '-DEUI_BUILD_SHARED=OFF' '-DEUI_ENABLE_INSTALL=OFF' `
    '-DEUI_ENABLE_MODULES=OFF' '-DCMAKE_DISABLE_FIND_PACKAGE_CURL=TRUE' `
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>'
if ($LASTEXITCODE -ne 0) { throw 'EUI-Edits check configuration failed.' }

# Bounded correctness gate; performance and interactive acceptance stay separate.
# Registry tests redirect HKCU in their own process; settings use temporary APPDATA.
$targets = @('file_safety', 'document_tabs', 'session_storage', 'text_file_encoding', 'settings_atomic', 'settings_confirmation',
    'i18n', 'menu_commands', 'file_assoc_registration', 'context_menu', 'toast_layout',
    'input_scrollbar', 'pointer_input', 'vault_delete_state', 'file_operations', 'vault_rename',
    'portable_resource_paths', 'theme_loader', 'image_viewport', 'dsl_main_handle', 'win32_input',
    'text_metrics_cache', 'text_atlas_growth', 'text_atlas_overflow', 'session_write_scheduler',
    'document_tab_cache', 'input_viewport_metrics', 'file_check_scheduler', 'vault_scan_scheduler',
    'tab_presentation')
$tests = @($targets) + @('settings_language_zh', 'settings_language_en', 'settings_language_invalid', 'settings_language_legacy')
$filter = '^(' + (($tests | ForEach-Object { [Regex]::Escape($_) }) -join '|') + ')$'
# CTest can otherwise succeed with an accidentally empty or incomplete selection.
$listing = @(& $ctest --test-dir $BuildDirectory -C Release --show-only=json-v1 -R $filter)
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate EUI-Edits checks.' }
$listed = @((($listing -join "`n") | ConvertFrom-Json).tests | ForEach-Object { $_.name })
if ($listed.Count -ne $tests.Count -or @(Compare-Object $tests $listed).Count -ne 0) {
    throw 'EUI-Edits check selection is incomplete; refusing to report a pass.'
}
& $CMake --build $BuildDirectory --config Release --target @targets --parallel $Parallel
if ($LASTEXITCODE -ne 0) { throw 'EUI-Edits check build failed.' }
& $ctest --test-dir $BuildDirectory -C Release -R $filter --output-on-failure --timeout 180
if ($LASTEXITCODE -ne 0) { throw 'EUI-Edits correctness checks failed.' }
Write-Output ("EUI-Edits Win32/Direct2D correctness gate passed: {0} tests. Real-window acceptance remains separate." -f $tests.Count)
