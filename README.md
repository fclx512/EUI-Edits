# EUI-Edits

[简体中文](README.zh-CN.md) · English

EUI-Edits is a native Windows notepad built around low resource use and a modern UI. Alongside simple text editing, it offers Obsidian-style live Markdown rendering and in-place editing: formatting follows your input, while reading and editing stay in the same document.

**0.1.0 release candidate** · Windows x64 · C++ · Win32 / Direct2D · Apache-2.0

## See it in use

![Markdown live rendering and in-place editing in the light theme](docs/screenshots/markdown-light.png)

![JSON editing and the document library in the dark theme](docs/screenshots/code-dark.png)

Five small, matching document icons for Markdown, text, code, data, and other files:

<p align="center">
  <img src="apps/neo_editor/assets/icons/md.svg" width="56" alt="Markdown document icon">
  &nbsp;&nbsp;
  <img src="apps/neo_editor/assets/icons/txt.svg" width="56" alt="Text document icon">
  &nbsp;&nbsp;
  <img src="apps/neo_editor/assets/icons/code.svg" width="56" alt="Code document icon">
  &nbsp;&nbsp;
  <img src="apps/neo_editor/assets/icons/data.svg" width="56" alt="Data document icon">
  &nbsp;&nbsp;
  <img src="apps/neo_editor/assets/icons/file.svg" width="56" alt="Generic file icon">
</p>

<details>
<summary>More screenshots: appearance and file associations</summary>

![English appearance settings](docs/screenshots/settings-en.png)

![File association settings](docs/screenshots/associations-zh.png)

</details>

The screenshots show the 0.1.0 candidate on Windows 11 with sample documents and isolated settings.

## The editing experience

- **Markdown, rendered as you write.** Common headings, emphasis, links, lists, task lists, blockquotes, code blocks, and tables update in place. The active editing region exposes the Markdown source, so reading and editing stay in the same document.
- **A local document library.** Browse the full file list by default, including hidden and extensionless files. The library reads file and directory metadata; it does not load unopened file contents into the editor.
- **Text files, simply edited.** Open Markdown, plain text, source code, and data files. Recognized formats get basic syntax coloring; other text remains plain text.

The interface is built with native C++ on Win32 and Direct2D. Low resource use is a design goal: the editor redraws on demand and does not embed a WebView. The everyday essentials—open, save, find and replace, undo and redo, line numbers, and word wrap—are kept close at hand. In image preview, use the wheel to zoom and middle-drag to pan; left-click or press `Esc` to close it.

EUI-Edits focuses on writing and local files, not knowledge-base organization or IDE features such as code completion and debugging. It does not run scripts.

## Get started

The 0.1.0 candidate is not yet publicly available. The local candidate ships as a single `EUI-Edits-0.1.0-windows-x64.exe`, with no installer or external resource directory. Run it and open a document with **File → Open** or `Ctrl+O`.

Settings and recovery drafts are stored in `%APPDATA%\EUI-Edits`; this is an install-free application, not a portable-settings mode. On first use, TXT and Markdown are preselected in file associations; other types, including BAT, CMD, PowerShell, and Python, are optional. Applying a selection adds EUI-Edits to Windows Open With; it does not change the Windows default app, which you choose separately in Windows Settings.

| Shortcut | Action |
| --- | --- |
| `Ctrl+N` / `Ctrl+O` | New / Open |
| `Ctrl+S` / `Ctrl+Shift+S` | Save / Save As |
| `Ctrl+F` / `Ctrl+H` | Find / Replace |
| `Ctrl+G` / `Ctrl+Shift+G` | Next / Previous search result (while Find is open) |
| `F3` / `Shift+F3` | Open Find / Next or Previous result |
| `F2` | Rename the focused item in the document library |
| `Ctrl+,` | Settings |

## Development

For the repository's Windows translation and correctness checks, run [`scripts/check-neoeditor.ps1`](scripts/check-neoeditor.ps1).

## Scope

The release configuration targets **Windows 10/11 x64**. The 0.1.0 candidate has been checked on Windows 11; Windows 10 has not received equivalent machine testing. The executable is currently unsigned.

- Files are limited to **64 MiB** by default. Suspected binary files and content that cannot be decoded reliably are rejected. UTF-8, UTF-16, and Windows ANSI encodings are supported.
- Markdown focuses on common syntax. Formulas, remote images, and presentation mode are outside the current scope.
- The first document-library scan is synchronous, so very large folders may take time to enumerate.

## License and credits

EUI-Edits is built on [EUI-NEO](https://github.com/sudoevolve/EUI-NEO). The framework source is retained in this repository; the framework's upstream documentation lives in the [EUI-NEO repository](https://github.com/sudoevolve/EUI-NEO).

The project follows the repository's [Apache-2.0 license](LICENSE). The application icon is a redrawn project asset with a dedicated 16px design. Third-party libraries and fonts retain their respective licenses and attribution; see [Third-party notices](apps/neo_editor/THIRD-PARTY-NOTICES.md) and [LICENSES](apps/neo_editor/LICENSES). EUI-Edits uses the work of the FreeType Team for font handling.

The complete license and attribution texts are also embedded in the executable and can be viewed and exported from **Settings → About**.
