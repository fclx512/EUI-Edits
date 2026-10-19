# EUI-Edits

[简体中文](README.md) · English

EUI-Edits is a lightweight native text and Markdown editor for Windows, suited to everyday notes, document writing, and simple edits to source code and configuration files. Markdown is rendered live and edited in place, so formatting follows your input without switching between source and preview.

Built with C++, Win32, and Direct2D, it redraws on demand and embeds no WebView. Its borderless window brings menus, tabs, and window controls into one top bar. The install-free release is a single EXE.

## Interface preview

Light writing view: a borderless top bar, Markdown tools, and live rendering.

![Markdown writing interface in the 0.2.0 light theme](docs/screenshots/markdown-light.png)

Dark code view: multiple tabs, line numbers, and basic syntax coloring.

![Code editing interface in the 0.2.0 dark theme](docs/screenshots/code-dark.png)

Theme workspace: preview colors on a sample and generate neutral, soft, or vivid palettes from a seed color.

![Theme workspace and local palette generation in 0.2.0](docs/screenshots/theme-workbench.png)

The application icon and document icons for Markdown, text, code, data, and other files share one design:

![Application icon (top) and the five document icons (bottom)](docs/screenshots/icons-overview.png)

## The editing experience

- **Markdown as you write.** Headings, lists, blockquotes, code blocks, and tables appear directly in the document; the active editing region shows source text. The toolbar inserts common formatting, tasks can be checked, and local images open a preview with zoom and pan.
- **Writing and navigation.** Typewriter mode follows the current input line. Focus mode hides interface elements and dims unselected non-current lines while retaining mode and window controls. The right-side outline jumps to headings; local Markdown and wiki links support headings in the same or another file.
- **Draggable tabs.** Drag tabs to reorder them. Each tab preserves its caret, selection, undo history, folds, and library view. Opening the same file locates its existing tab; launching the app again forwards file requests to the running window.
- **A library that loads on demand.** Browse a folder in the sidebar, including hidden and extensionless files. Opened files are marked with a dot. Entries load in batches one directory level at a time; expanding a subfolder loads its entries, and opening a file loads its contents. `F2` renames items.
- **Editing across text formats.** Open Markdown, plain text, source code, and data files. Recognized formats receive basic syntax coloring; "Edit → Syntax mode…" changes the presentation manually. Find and replace can be limited to a selection, with one-step undo for Replace all. Word wrap and line numbers support everyday editing.
- **Saving and recovery.** Saving preserves the original encoding, BOM, and line endings, checks for external changes, and verifies the written content. A failed save retains the unsaved state and recovery data. Unsaved content is copied periodically for recovery; after an abnormal exit, it can return as separate drafts for review and manual saving.

## Markdown support

| Syntax | Current behavior |
| --- | --- |
| Headings (`#` and underline style), paragraphs | Live rendering and in-place editing |
| Bold, italic, inline code, strikethrough, underline (`_.._`) | Supported |
| Links, wiki links (`[[...]]`) | Clickable navigation |
| Ordered / unordered lists, tasks, blockquotes | Supported; tasks are clickable, blockquotes nest up to four levels |
| Fenced code blocks, tables, dividers, frontmatter | Supported |
| Local images on their own line | Rendered; click to preview |
| Inline math `$...$`, HTML | Math uses code styling; HTML stays as source and is not executed |
| Formula typesetting, remote images, highlights, comments, footnotes, callouts, tags | Not yet supported; source remains editable |

Markdown covers common writing syntax and some extensions; full Obsidian compatibility is not guaranteed.

## Getting started

Choose a version on the [releases page](https://github.com/fclx512/EUI-Edits/releases), download its `EUI-Edits-<version>-windows-x64.exe`, put it where you plan to keep it, and run it directly. No resource folder needs unpacking. This README describes the 0.2.0 source; available downloads are listed on the releases page. The unsigned app supports Windows 10/11 x64 and has been tested on both systems. Appearance and behavior may vary with your system, fonts, and display settings.

**Changes in 0.2.0:** Version 0.2.0 adds a borderless window with draggable tabs, a theme workspace with local palette generation, Markdown tools, Typewriter and Focus modes, a right-side outline, selection-scoped find and replace, and heading-link navigation. It also strengthens save verification and recovery protection, and loads library folders on demand. See the [release notes](apps/neo_editor/RELEASE-NOTES.md) for more changes.

### Common actions

| Action | Shortcut / method |
| --- | --- |
| New text / open file | `Ctrl+N` / `Ctrl+O`; files can also be dropped into the window |
| New Markdown | "File → New → Markdown" |
| Save / Save As | `Ctrl+S` / `Ctrl+Shift+S` |
| Close current tab | `Ctrl+W`; modified content prompts for save, discard, or cancel |
| Next / previous tab | `Ctrl+Tab` / `Ctrl+Shift+Tab` |
| Go to tab | `Ctrl+1`–`Ctrl+8` select the matching tab, `Ctrl+9` the last |
| Find / replace | `Ctrl+F` / `Ctrl+H`; enable selection scope in find options |
| Next / previous match | With the find bar open: `F3` / `Shift+F3`, or `Ctrl+G` / `Ctrl+Shift+G` |
| Open library / rename | "File → Open folder…"; focus a sidebar item and press `F2` |
| Fold / unfold current section | `Ctrl+Shift+J` |
| Typewriter / Focus | Buttons at the right of the Markdown toolbar; plain text uses floating writing buttons |
| Show / hide outline | "Outline" button at the right of the Markdown toolbar |
| Settings | Top-bar "Settings", "View → Settings…", or `Ctrl+,` |

A new document's first save chooses its name and location. For garbled text, choose "File → Reopen with encoding"; use "File → Convert to UTF-8" when conversion is needed. Unrepresentable characters produce a warning, and failed or cancelled saves retain the document content.

Typewriter and Focus work in Markdown and plain text; their preferences last for the current session. Focus hides interface elements without entering system fullscreen. Selection-scoped find uses literal text matching. If edits, undo, or reload invalidate the original range, EUI-Edits warns and stops selection-scoped matching to prevent replacement at stale positions; choose a new range or switch to document scope to continue.

In image preview, use the wheel to zoom, middle-drag to pan, and left-click or `Esc` to close.

### Settings, recovery, and file associations

Settings live in the current Windows user's profile and do not move with the EXE:

| Location | Contents |
| --- | --- |
| `%APPDATA%\EUI-Edits\settings.ini` | Appearance, fonts, layout, recent files, and other preferences |
| `%APPDATA%\EUI-Edits\session` | Tab sessions and recovery copies of unsaved content |
| `%APPDATA%\EUI-Edits\themes` | Local themes in the theme library |

After an abnormal exit, recovered content opens as separate drafts without automatically overwriting original files. A normal exit clears the recovery session after save or discard confirmations, so all tabs do not automatically reopen next time. Recovery copies are periodic, not per-keystroke saves; save important work yourself.

"Settings → Files & system" registers Open-with entries for the current user, initially selecting TXT and Markdown. It shows selected, pending-add, and pending-remove counts; "Select recommended" excludes HTML, Shell, batch, and PowerShell files. Choose the default app separately in Windows. Moving or renaming the EXE requires registering associations again. Script files are edited as text; their code is not executed.

## Appearance and personalization

Most settings take effect immediately and persist across launches:

| Option | Choices |
| --- | --- |
| Language and theme | 简体中文 / English; follow the system theme or choose light / dark |
| Fonts | Separate body, code, and UI fonts; import `.ttf`, `.otf`, or `.ttc`; code fonts are checked for monospace |
| Sizes and scaling | Body size 12–32, UI size 12–18; extra scaling of 80%–200% on top of system DPI |
| Reading layout | Line numbers, status bar, word wrap; readable width narrows and centers the content |
| Library and animations | Simple / library view; hover and press animations can be disabled |

The status bar shows file location, syntax mode, encoding, line endings, line count, and character count, with controls for syntax mode and word wrap; some information collapses in narrow windows. Fonts come from your machine or imports; the release ships no font files.

**The local theme workspace is a beta feature.** Open "Settings → Appearance → Theme library" to view a sample on the left and edit colors or typography on the right. Each light/dark side has 26 colors. It supports local palette generation, per-color locks, undo/redo, comparison with the baseline, and saving a new theme. Typography controls include line spacing, paragraph spacing, and heading scales.

JSON or CSS imports are reviewed before creating a draft; files can also be placed in the `themes` folder above. Saved themes persist across launches; unsaved drafts and their undo history do not.

For Obsidian themes, select `manifest.json` with a sibling `theme.css`, or select CSS directly. Import maps recognizable colors only, never executes CSS, and does not promise the original layout or plugin styles. Built-in colors remain available.

## Resource use

The UI redraws on demand. Tab switches reuse layout and highlighting where possible, with less-used derived data released when cache budgets are exceeded. The library scans metadata without loading every document. Disabling interaction animations removes hover and press transition frames.

The following are **historical idle memory figures for 0.1.4 on Windows 11**, in MiB:

| Scenario | Total working set | Private working set | Private commit |
| --- | ---: | ---: | ---: |
| Empty document, one tab | 46.0 | 15.9 | 21.7 |
| Three small Markdown tabs, 2,580 bytes total, idle after loading each | 55.9 | 21.9 | 28.3 |
| 9 MiB plain text, 73,728 file lines | 118.0 | 87.8 | 101.9 |
| 9 MiB Markdown, same line count, with a list item, emphasis, and inline code on every line | 216.6 | 186.2 | 207.0 |

These figures are for reference only, not measurements of 0.2.0 or memory ceilings. Actual use varies with the system, document content, settings, and editing activity, and may differ from the table. The three metrics cannot be added together.

## Current scope

EUI-Edits focuses on local text editing. It provides no knowledge-base management, code completion, build/debug tools, script execution, or presentation mode.

| Area | Support and limits |
| --- | --- |
| Files | Default open limit: **64 MiB**; oversized files, suspected binaries, and unreliable decoding are rejected |
| Encodings | UTF-8, UTF-16 LE / BE, Windows ANSI code pages; saves preserve encoding, BOM, and LF / CRLF characteristics |
| Library | Loads one level at a time in batches of up to 500 entries; expanding a subfolder loads its entries. Filtering covers loaded entries only, and automatic watching covers changes to direct children of the root. Does not recurse into symbolic-link or junction targets |
| Recovery | Up to **256 tab entries**; recovery-body read limits of **256 MiB per file**, **1 GiB total**; damaged manifests or missing bodies are retained and reported, and recovery may be unavailable |

## Development

Building requires Windows, Visual Studio 2022 / 2026 or Build Tools with the C++ desktop workload and Windows SDK, a CMake version supporting that generator, PowerShell 7, and Python 3. Dependency sources are included in the repository.

Run from the repository root:

```powershell
# Run checks
pwsh -NoProfile -File ./scripts/check-neoeditor.ps1

# Build a Release x64 single EXE
pwsh -NoProfile -File ./scripts/package-neoeditor.ps1 -Version 0.2.0
```

The output is `out/euiedits-0.2.0-single-exe/EUI-Edits-0.2.0-windows-x64.exe`. Source folders, CMake targets, and script names retain the internal `neo_editor` identifier.

See the [release build guide](docs/NeoEditor-发布构建说明-2026-10-02.md) for more build options and the [documentation index](docs/目录说明.md) for maintenance and release guidance (Chinese).

## License and credits

EUI-Edits is built on [EUI-NEO](https://github.com/sudoevolve/EUI-NEO), whose source remains in this repository, under the [Apache-2.0 license](LICENSE). Application and document icons are project-drawn assets. Third-party libraries retain their licenses and attribution; see the [third-party notices](apps/neo_editor/THIRD-PARTY-NOTICES.md) and [LICENSES](apps/neo_editor/LICENSES). Font handling uses the work of the FreeType Team.

Complete license and attribution texts are embedded in the EXE and available in "Settings → About" for viewing and export.
