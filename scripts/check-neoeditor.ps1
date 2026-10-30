#Requires -Version 7
[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string]$CMake,
    [string]$Python,
    [string]$Generator,
    [ValidateRange(1, 32)][int]$Parallel = 2,
    [switch]$PreflightOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'neoeditor-release-common.ps1')
if ($env:OS -ne 'Windows_NT') { throw 'EUI-Edits release checks require Windows with MSVC x64.' }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo ('build-neoeditor-check-' + [Guid]::NewGuid().ToString('N')) }
$BuildDirectory = Assert-EuiRepositoryPath $BuildDirectory $repo 'BuildDirectory'
if (Test-Path -LiteralPath $BuildDirectory) { throw 'Use a new build directory. Existing directories are never changed or deleted.' }

$tools = Get-EuiReleaseTools -CMake $CMake -Generator $Generator -Python $Python -NeedPython
$CMake = $tools.CMake; $ctest = $tools.CTest; $Generator = $tools.Generator; $Python = $tools.Python
if ($PreflightOnly) { $tools; return }

& $Python -B (Join-Path $repo 'scripts/check-docs.py')
if ($LASTEXITCODE -ne 0) { throw 'EUI-Edits documentation checks failed.' }
& $Python -B (Join-Path $repo 'tests/python/test_check_docs.py')
if ($LASTEXITCODE -ne 0) { throw 'EUI-Edits documentation checker regressions failed.' }
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
$targets = @('file_safety', 'document_tabs', 'session_storage', 'text_file_encoding', 'settings_atomic', 'settings_confirmation', 'settings_reveal', 'reveal_state', 'settings_reveal_benchmark', 'hover_transition_benchmark', 'decoration_cadence', 'reveal_d2d_benchmark',
    'vault_watcher', 'save_dialog_protocol', 'font_safety', 'heading_anchor', 'find_search', 'find_actions',
    'i18n', 'menu_commands', 'menu_motion', 'menu_style_benchmark', 'settings_transition', 'control_text_alignment', 'transition_cadence_benchmark', 'file_assoc_registration', 'context_menu', 'frozen_backdrop', 'frozen_menu_benchmark', 'frozen_runtime_benchmark', 'editor_context_menu_lp', 'link_editor_focus', 'toast_layout',
    'input_scrollbar', 'pointer_input', 'vault_delete_state', 'file_operations', 'vault_rename',
    'portable_resource_paths', 'theme_loader', 'theme_workbench', 'theme_workbench_ui', 'input_selection_color', 'image_viewport', 'mindmap_plan', 'mindmap_layout', 'mindmap_preview_state', 'mindmap_preview_ui', 'mindmap_inline_ui', 'mindmap_manual_sample', 'input_line_overlay', 'ime_rect', 'ime_frame_refresh', 'dsl_main_handle', 'win32_input',
    'text_metrics_cache', 'text_metrics_cursor', 'text_glyph_advance_cache', 'text_glyph_cache_budget', 'text_font_resolution_scope', 'text_atlas_growth', 'text_atlas_overflow', 'session_write_scheduler',
    'document_tab_cache', 'input_viewport_metrics', 'file_check_scheduler', 'vault_scan_scheduler',
    'tab_presentation', 'system_appearance', 'window_chrome', 'input_placeholder_refresh', 'input_vertical_alignment', 'layout_numeric',
    'input_horizontal_viewport', 'input_callback_layout', 'input_reflow', 'input_reflow_callback', 'input_reflow_scroll', 'input_reflow_render', 'input_drag_feedback', 'input_pointer_feedback',
    'document_vault_loading', 'virtual_list_wheel', 'win32_event_pump', 'runtime_scroll_sync',
    'main_window_frame_reentry', 'dsl_main_compose', 'dsl_window_resize_throttle', 'runtime_timer', 'd2d_render_policy', 'd2d_render_probe', 'd2d_atlas_capacity',
    'input_model', 'input_navigation', 'input_value_reference', 'input_source_line_count', 'table_selection_background', 'lp_decorations', 'source_decorations', 'decoration_uniform_pages', 'undo_incremental', 'image_attach', 'markdown_writing')
$tests = @($targets) + @('text_italic', 'settings_language_zh', 'settings_language_en', 'settings_language_invalid', 'settings_language_legacy', 'cached_strip_runtime')
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
