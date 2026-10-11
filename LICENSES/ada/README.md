# Embedded Ada URL parser

Ada 4.0.0, commit `b12a893a45809da8103bb4f1e2f6f5ee13f9100b`:
[upstream source](https://github.com/ada-url/ada/tree/b12a893a45809da8103bb4f1e2f6f5ee13f9100b).

The codeload archive SHA-256 is
`1eb38d21c35e162ccc6c71e8fdd9a5b418eb359f2b39f1c50bf3836a5949b623`.
`cmake/NeverDWebURL.cmake` compiles the unchanged `src/ada.cpp` and its included
C++ sources and headers, including the already generated Ada IDNA tables.
NeverD selects the MIT license; both upstream license files are preserved.
The Ada IDNA MIT notice and Unicode data license are also included.

No upstream generation script, executable, binding, benchmark, test runner or
URLPattern backend is built or invoked. URL parsing runs in process with no
network or filesystem access. NeverD's bounds, import-map processing and
captured-file association are separate original C++ implementations.

The supplemental notices were retrieved on 2026-10-10 from
[Ada IDNA](https://github.com/ada-url/idna/blob/main/LICENSE-MIT) and
[Unicode](https://www.unicode.org/license.txt). They accompany the unchanged
generated material already included in the pinned Ada source archive.
