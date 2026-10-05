# Archived EUI-NEO workflows

These files are retained as upstream framework references. GitHub Actions does not discover workflows in this directory.

- `ci.yml`: framework Linux/backend matrix, not EUI-Edits Windows acceptance.
- `pages.yml`: deployment of the upstream framework website.
- `release.yml`: framework runtime/SDK releases for bare `v*` tags.

EUI-Edits uses the workflows in `../workflows/`, whose release workflow still recognizes the internal `neoeditor-v*` tag pattern. The public 0.1.0 release uses `v0.1.0`. Archiving the framework CI stops its automatic backend coverage; the framework source and documentation remain available. Restore or adapt these files only when framework CI or website maintenance is intentionally resumed.
