Source: https://github.com/material-foundation/material-color-utilities
Commit: dd62982dfdefdfc9232ce8d76e122fb1c6d188ae
Retrieved: 2026-10-18
License: Apache-2.0 (LICENSE)
C++ sources excluding upstream tests. Local changes are documented below.

Local MSVC portability changes: replace C compound literals with C++ aggregate initialization; use existing kPi instead of M_PI; replace HexFromArgb Abseil formatting with std::ostringstream (no algorithm change). Only 14 required .cc sources are built; image quantization is not linked.
