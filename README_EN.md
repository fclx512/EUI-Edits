# EUI-Edits

[简体中文](README.md) · English

EUI-Edits is a native Windows notepad built around low resource use and a modern UI. Alongside simple text editing, it offers Obsidian-style live Markdown rendering and in-place editing: formatting follows your input, and reading and editing stay in the same view.

## Interface preview

![Live Markdown rendering and in-place editing in the light theme](docs/screenshots/markdown-light.png)

![Multi-tab editing with syntax coloring in the dark theme](docs/screenshots/code-dark.png)

The application icon and the document icons for Markdown, text, code, data, and other files share one coordinated, hand-drawn design:

![Application icon (top) and the five document icons (bottom)](docs/screenshots/icons-overview.png)

## The editing experience

- **Markdown, rendered as you write.** Common headings, emphasis, links, lists, task lists, blockquotes, code blocks, and tables update in place. The editing region shows the Markdown source, so reading and editing never leave the document.
- **Multi-tab editing.** Keep multiple documents open at once; each tab independently preserves its caret, selection, undo history, folds, and document-library view. After an abnormal exit, unsaved drafts are restored tab by tab and never silently overwrite the original files.
- **A local document library.** Lists every file in the chosen folder, including hidden and extensionless files; opened documents are marked with a dot, and `F2` renames the focused item. Scanning runs in the background and refreshes automatically when the folder changes. It reads file and directory metadata only—never the contents of unopened files.
- **Simple editing for all kinds of text.** Open Markdown, plain text, source code, and data files. Recognized formats get basic syntax coloring, and "Syntax mode…" switches how the current document is presented; other text stays plain.

The interface is built with C++, Win32, and Direct2D, redraws on demand, embeds no WebView, and runs as a single instance—opening a file again forwards it to the running window.

## Markdown syntax support

| Syntax | Support |
| --- | --- |
| Headings (`#` and underline style), paragraphs | ✅ Rendered live, edited in place |
| Bold, italic, inline code, strikethrough, underline (`_.._`) | ✅ |
| Links and wiki links (`[[...]]`) | ✅ Clickable navigation |
| Ordered / unordered lists, blockquotes (up to four levels) | ✅ |
| Task lists | ✅ Click to check off |
| Fenced code blocks, tables, dividers, frontmatter | ✅ |
| Local images (on their own line) | ✅ Rendered; click to open the preview |
| Inline math `$...$` | ⚠️ Presented like code; no formula typesetting |
| HTML | ⚠️ Not executed; kept as source text |
| Formula typesetting (LaTeX), remote images | ❌ Out of scope; the source stays editable |
| Highlights `==...==`, comments `%%...%%`, footnotes, callouts, tags | ❌ Not yet supported; shown as plain text |

## Low resource use

EUI-Edits is built with C++, Win32, and Direct2D, refreshes the UI on demand, and embeds no browser or WebView. The low footprint comes from a few design choices:

- **On-demand redraws.** Typing, scrolling, window changes, and interaction feedback drive updates; with feedback animations turned off, hover and press states no longer produce transition frames.
- **Tab caches are reused.** When switching tabs and neither content nor presentation conditions changed, layout and highlighting results are reused; caches are kept within a budget, and exceeding it releases the least-recently-used derived data while documents, carets, and undo state stay intact.
- **Directory scanning and content reading are separate.** The document library scans file and directory metadata in the background, and multiple tabs can share one folder's scan result; content loads only when a file is opened—the whole folder is never read into the editor.

The following working-set reference values were recorded for the earlier 0.1.0 candidate (Windows 11, read after a cold start had settled):

| Scenario | Process working set |
| --- | --- |
| Started with an empty document | about 50–54 MB |
| One 9 MB text document open | about 68 MB |

These are reference readings from the existing candidate and have not been re-measured against the program pending release. Actual usage varies with document type, line count, Markdown structure, local images, the number of open tabs, fonts, and the rendering environment; plain text and an equally sized Markdown document can also lay out at different costs. These readings are not a fixed memory ceiling.

## Appearance and personalization

Open Settings from "View → Settings…" or `Ctrl+,`. Appearance, editing, files, and system options are configured separately; most adjustments take effect immediately and are saved for the next launch.

| Option | How it works |
| --- | --- |
| Interface language | Follow the system or fix to 简体中文 / English; the interface language is set separately from a document's syntax coloring. |
| Light / dark theme | Follow system, light, or dark. First launch follows the Windows app theme; while following, system light/dark changes apply to the program live. |
| Fonts | "Appearance → Choose fonts" sets body, code, and UI fonts separately. Body covers editing and Markdown reading, code covers code blocks and inline code, and UI covers menus, the sidebar, and the status bar. |
| Font sizes | Editor size 12–32 and UI size 12–18 adjust independently; "View → Font size" also switches the editor size quickly. |
| UI scaling | An extra 80%–200% on top of the Windows display scale. 100% means no extra scaling while still honoring system DPI. |
| Reading and editing layout | Show or hide line numbers and the status bar, toggle word wrap; "Readable line width" narrows and centers the content column, handy for reading Markdown in wide windows. |
| Library layout | "View → Layout" switches between the simple view and the library view with a file sidebar. |
| Interaction animations | Turn hover/press feedback animations on or off. First launch uses the Windows animation preference; the in-app choice is remembered afterwards. |

The font list comes from the local machine, and "Import font" accepts `.ttf`, `.otf`, or `.ttc` files; code fonts are checked for monospace. The program uses system fonts and hand-drawn UI icons, so the release package ships no font files.

The status bar shows the current file name and location, the document's syntax mode, encoding, line endings, line count, and character count; clicking the syntax mode switches how the document is presented, and word wrap has its own toggle. When the window is narrow, some items collapse to fit.

**Local theme import is still a beta feature.** Under "Appearance → Theme file (beta) → Theme library…", pick a local JSON or CSS file, or drop themes into `%APPDATA%\EUI-Edits\themes` and apply them from the library. Obsidian themes can be imported via their `manifest.json` (with `theme.css` in the same folder) or by selecting the CSS directly. Import maps recognizable color variables only; it never executes CSS and does not promise to reproduce the original theme's layout or plugin styles. The first entry in the library restores the built-in colors.

## Getting started

### Opening and saving documents

The release ships as a single `EUI-Edits-<version>-windows-x64.exe` with no installer or resource folder to unpack. Put the program where you plan to keep it and run it directly; if a `.exe.sha256` is provided alongside, use it to verify the file.

1. **New:** choose "File → New → Text / Markdown". `Ctrl+N` creates a plain-text document; a new tab has no file on disk yet, and the first save picks its name and location.
2. **Open:** use "File → Open…" or `Ctrl+O`, or drag files into the window. Documents open in tabs; opening the same file again locates its existing tab, and unsaved content in other tabs is preserved.
3. **Save:** `Ctrl+S` saves the current tab, `Ctrl+Shift+S` is Save As. Existing files keep the encoding, BOM, and line endings they were read with; if the encoding cannot represent newly typed characters, an error is shown and the original file is left untouched.
4. **Close:** `Ctrl+W` closes the current tab. Closing a modified tab or exiting the program asks whether to save, discard, or cancel; a failed save or a cancelled prompt never discards the content outright.

If text shows up garbled, use "File → Reopen with encoding" to pick the right encoding. When files change in other programs, EUI-Edits checks for conflicts during the save flow so stale content never silently overwrites the newer file on disk.

The program runs as a single instance by default. Launching it again with a file hands the request to the existing window, which opens or locates the document there.

### Browsing files and common actions

Choose "File → Open folder…" to pick a directory, or switch to the library view from "View → Layout". The file sidebar lists the folder structure and its files; content loads only when a file is opened. Press `F2` on a focused file or folder in the sidebar to rename it. Opened documents are marked with a dot, and the current document gets an additional selection highlight.

| Action | Shortcut / method |
| --- | --- |
| New text / open file | `Ctrl+N` / `Ctrl+O` |
| Save / Save As | `Ctrl+S` / `Ctrl+Shift+S` |
| Close current tab | `Ctrl+W` |
| Next / previous tab | `Ctrl+Tab` / `Ctrl+Shift+Tab` |
| Go to tab | `Ctrl+1`–`Ctrl+8` pick the matching tab, `Ctrl+9` the last |
| Find / replace | `Ctrl+F` / `Ctrl+H` |
| Next / previous match | With the find bar open, `F3` / `Shift+F3`, or `Ctrl+G` / `Ctrl+Shift+G` |
| Rename sidebar item | Focus the item in the library list and press `F2` |
| Open settings | `Ctrl+,` |

Click a local image inside Markdown to preview it: wheel to zoom, middle-drag to pan, left-click or `Esc` to close.

### Settings and recovery drafts

The program is install-free, but settings live in the current Windows user's profile directory and do not travel with the EXE:

| Location | Contents |
| --- | --- |
| `%APPDATA%\EUI-Edits\settings.ini` | Preferences: appearance, fonts, layout, recent files, and more |
| `%APPDATA%\EUI-Edits\session` | Tab sessions and recovery copies of unsaved content |
| `%APPDATA%\EUI-Edits\themes` | Local themes listed by the theme library |

Unsaved content is written to recovery copies periodically. After an abnormal exit, the next launch can restore them as separate drafts; the recovery flow never overwrites the original files automatically—review the content and save manually. A normal exit clears the session once saves or discard confirmations complete, so the same set of tabs will not reopen next time. Recovery copies are not instant per-keystroke saves; for important work, still press `Ctrl+S` yourself.

### File associations

Under "Settings → Files & system", choose the file types to add to Windows' "Open with". Before any association is registered, TXT and Markdown are preselected; other types, including BAT, CMD, PowerShell, and Python, can be checked as needed.

Applying the selection only registers Open-with entries for the current user; it never changes the Windows default app for you. To make EUI-Edits the double-click default, choose it in Windows Settings or the file's "Open with" dialog. Association records point at the EXE's location, so moving or renaming the program requires registering again. Script files are edited as text—the program never executes their code.

## Development

### Build environment

The commands below build the Windows x64 Win32 / Direct2D version. The application code lives in `apps/neo_editor`; the build entry point is the root [`CMakeLists.txt`](CMakeLists.txt), and the target is `neo_editor`.

| Tool or component | Requirement and purpose |
| --- | --- |
| Windows | Building the x64 binaries requires Windows; the scripts below target MSVC. |
| Visual Studio / Build Tools | VS 2022 or VS 2026 with the "Desktop development with C++" workload and the MSVC x64 tools. |
| Windows SDK | Windows 10 or 11 SDK, providing Win32, Direct2D, and other system libraries plus the resource compiler `rc.exe`. |
| CMake | **3.21+** for the VS 2022 generator; **4.2+** for the VS 2026 generator. The CMake bundled with Visual Studio works. |
| PowerShell | The check and packaging scripts require **5.1 or later**; PowerShell 7 also works. |
| Python 3 | Needed for the development checks; the translation check uses only the standard library. The app build and single-EXE packaging themselves do not depend on Python. |
| `dumpbin.exe` | Required by the single-EXE packaging script; ships with the MSVC tools and checks architecture and DLL dependencies. |

The project uses **C++17 / C99**. Third-party dependency sources are kept in-repo under `3rd/`, including FreeType, zlib, libpng, MD4C, and yyjson. The instructions below use `bundled` mode, which builds from the in-repo sources—no separate installation or network downloads at configure time. Keep the full `3rd/`, `assets/`, and app license directories.

The root project declares a minimum CMake of 3.14, but the Visual Studio generators above have higher requirements; pick versions per the table when building. Generator version notes: [VS 2022](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2017%202022.html) and [VS 2026](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html) in the CMake documentation.

### Building the app

Open PowerShell at the repository root and make sure `cmake` is on PATH; alternatively use the Visual Studio Developer PowerShell, or add CMake's `bin` directory to PATH. The example below uses VS 2022 and configures only the editor and its link dependencies, with the same Win32 / Direct2D backend and static MSVC runtime as the standalone release configuration:

```powershell
cmake -S . -B build-editor -G "Visual Studio 17 2022" -A x64 `
  -DEUI_BUILD_NEOEDITOR_ONLY=ON `
  -DEUI_WINDOW_BACKEND=win32 -DEUI_RENDER_BACKEND=d2d `
  -DEUI_DEPS_MODE=bundled -DEUI_BUILD_SHARED=OFF `
  -DEUI_ENABLE_INSTALL=OFF -DEUI_ENABLE_MODULES=OFF `
  -DEUI_BUILD_TEST_FIXTURES=OFF `
  -DCMAKE_DISABLE_FIND_PACKAGE_CURL=TRUE `
  '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>'

cmake --build build-editor --config Release --target neo_editor --parallel 2
.\build-editor\Release\neo_editor.exe
```

With VS 2026, switch the generator to `Visual Studio 18 2026` and use a CMake that supports it. The output is `build-editor\Release\neo_editor.exe`; for a debug build, replace `Release` with `Debug` in the build command and find the output in the matching `Debug` directory.

The backends are selected explicitly because the root project's defaults pull in the framework's default backend and configure other examples. `EUI_BUILD_NEOEDITOR_ONLY=ON` builds only this app; `EUI_BUILD_SHARED=OFF` statically links the framework, and the MSVC runtime option statically links the CRT. Do not treat an ordinary local build with other settings as a verified standalone release package.

### Running the development checks

From the repository root:

```powershell
.\scripts\check-neoeditor.ps1
```

The script runs the translation check first, then configures, builds, and executes the selected Win32 / Direct2D correctness tests in a fresh build directory, covering file and save safety, encodings, tab state, recovery storage, settings, file associations, theme loading, and cache scheduling.

The script locates Visual Studio, CMake, and Python on PATH automatically; pass `-Generator`, `-CMake`, or `-Python` to specify them explicitly when lookup fails. `-BuildDirectory` may point to an output directory, but it must not exist yet—the script never clears or reuses an existing build directory. Performance measurement and real-window interaction acceptance are separate efforts; this script does not launch the app's UI.

### Producing the single-EXE release package

After the checks pass, use [`scripts/package-neoeditor.ps1`](scripts/package-neoeditor.ps1) to build and verify the release files:

```powershell
.\scripts\package-neoeditor.ps1 -Version 0.1.0
```

The script builds only the editor's Release x64 configuration, statically links the framework and MSVC runtime, embeds the icon and the full license texts, and verifies the PE version, system DLL dependencies, absence of development-directory paths, and license export from an empty-settings run. The default output is:

```text
out/euiedits-0.1.0-single-exe/
  EUI-Edits-0.1.0-windows-x64.exe
  EUI-Edits-0.1.0-windows-x64.exe.sha256
```

`-BuildDirectory` selects a fresh build directory, and `-OutputDirectory` selects an output directory that does not exist or is empty; pass `-CMake`, `-Dumpbin`, or `-Generator` when tool lookup fails. The script never overwrites previous release files, pushes the repository, or publishes a release. Packaging verification is not the same as full GUI acceptance: a freshly packaged EXE still needs its real open, edit, save, recovery, and exit flows checked.

## Current scope

EUI-Edits targets browsing and editing of local text, Markdown, source code, and configuration files. It provides no knowledge-base management, code completion, build/debug tooling, or script execution. Markdown coverage is detailed in the syntax table above.

| Area | Current support and limits |
| --- | --- |
| Platform | The release configuration targets **Windows 10/11 x64**. The 0.1.0 candidate has been checked on Windows 11; Windows 10 has not received equivalent on-machine testing. The program is currently unsigned. |
| File size | The default open limit is **64 MiB**. Oversized files, suspected binaries, and files that cannot be decoded reliably raise an error instead of loading as text. |
| Encodings and line endings | UTF-8, UTF-16 LE / BE, and Windows ANSI code pages are supported. Text is processed in a unified form internally and written back with the file's original encoding, BOM, and LF / CRLF characteristics; unrepresentable characters are never silently replaced. |
| Markdown | Covers common syntax and part of the extensions; no LaTeX formula typesetting, no remote images, and no presentation mode. HTML is kept as source, not executed as a web page. |
| Library scanning | Scans directory and file metadata, including hidden and extensionless files; file contents are never pre-loaded. The scan does not recurse into the targets of symbolic links or directory junctions, to avoid escaping the root or loops. |
| Recovery capacity | Recovery records hold up to **256 tab entries**; recovery bodies are additionally bounded by per-file **256 MiB** and total **1 GiB** protection limits. These are recovery-storage boundaries, not recommended everyday document sizes. |
| Recovery failures | A damaged manifest or missing body is kept as-is and reported instead of being overwritten automatically, preserving evidence for diagnosis; recovery may be unavailable in that case, but manual saving still works. |
| Custom themes | Local theme import is a beta feature; color mapping and compatibility are limited and not yet at the intended reliability and fidelity. |

## License and credits

EUI-Edits is built on [EUI-NEO](https://github.com/sudoevolve/EUI-NEO). The framework source is retained in this repository; the framework's upstream documentation lives in the [EUI-NEO repository](https://github.com/sudoevolve/EUI-NEO).

The project follows the repository's [Apache-2.0 license](LICENSE). The application icon is a redrawn project asset with a dedicated 16px design. Third-party libraries and fonts keep their respective licenses and attribution; see the [third-party notices](apps/neo_editor/THIRD-PARTY-NOTICES.md) and [LICENSES](apps/neo_editor/LICENSES). EUI-Edits uses the work of the FreeType Team for font handling.

The complete license and attribution texts are also embedded in the executable and can be viewed and exported from "Settings → About".
