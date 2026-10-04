# Markdown follow-up probe review

`tests/probes/md_followup.py` is a real-window evidence collector for the next Markdown and interaction review. It creates a throwaway vault with `followup.md`, a `sub/child.md`, and `other.md`, then points a fresh temporary `APPDATA` at that vault. It refuses to launch while another EUI-Edits instance or the single-instance mutex is present. It closes only its own window with `WM_CLOSE`; it does not terminate a process if normal close times out.

Run it only on an unlocked desktop with no EUI-Edits already open:

```powershell
D:/Python312/python.exe -B tests/probes/md_followup.py --exe build-win32/Release/neo_editor.exe --out build-win32/evidence/md-followup/final-light --theme 1 --scale 1
```

The probe saves client screenshots and `conditions.json` with executable/sample hashes, process id, reported window DPI, effective UI scale, pixel measurements, and any failure. A failure attempts to save `failure-current-window.png`. Review the captures before treating any coordinate-driven interaction as evidence; it was calibrated on this machine at 125% system DPI, with light/100% and dark/150% application scales.

The sample begins with plain Chinese/English text, a fenced block containing a blank line and a whitespace-only line, three levels of unordered list, two unresolved relative links, and YAML-like body text between thematic breaks. This text is deliberately outside document frontmatter; actual frontmatter blank-row semantics are covered by lp_decorations. A 150-line C++ fence follows and is long enough to cross the viewport. The capture sequence records the initial list/frontmatter view, link hover at 0/50/150 ms, a press-drag away from a link, an editor context menu opened near the right edge, menu hover at 0/50/150 ms, a click on the expected disabled Cut/Copy row, Escape and outside dismissal, three scrolled code views, and directory/other-row states in the temporary vault.

Pixel measurements intentionally avoid OCR. Link and menu hover metrics compare sampled RGB values in bounded regions. Code continuity checks a trailing band matching the initial code-background RGB and also reports a low-contrast column measurement; inspect the corresponding code screenshot to confirm the column crosses the code decoration rather than empty editor background. Small connected components in the editor gutter are only approximate unordered-marker candidates; the count can include other tiny dark shapes or miss antialiased/light-theme markers. The JSON check makes this limitation visible instead of claiming a semantic marker assertion.

The current coordinate anchors assume the normal 34-logical-pixel menu bar, an editor inset of 18 logical pixels, an initial 24-logical-pixel editor line pitch, the default 264-logical-pixel vault width, and a 26-logical-pixel vault row. UI scale and the client DPI are applied to those anchors. If `01-first-page.png` or `14-vault-directory-selected.png` shows different placement, adjust the documented coordinates and rerun on the target machine. The right-click menu clamp and hover rectangles are estimates; `06-editor-menu-edge.png` through `10-menu-disabled-click.png` are the review evidence for their actual placement and behavior.

Source alone is not acceptance evidence. Current results, executable hash, screenshots and limitations are recorded in docs/NeoEditor-MD交互与字体安全验收-2026-10-01.md; historical exploratory runs must not replace its final captures.
