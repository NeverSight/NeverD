# WHATWG HTML reference data

Copyright © WHATWG (Apple, Google, Mozilla, Microsoft).

The full upstream [LICENSE](LICENSE) is retained from
[whatwg/html](https://raw.githubusercontent.com/whatwg/html/main/LICENSE),
retrieved on 2026-10-10. It licenses the work under CC BY 4.0 and portions
incorporated into source code under BSD 3-Clause.
The preserved license SHA-256 is
`85dc6f5ccb57a6fe8c33d158f9fc8fc7ee5655a5d3db2cdd131c6a3d0f48a864`.

`unittests/web/fixtures/html/entities.json` preserves the official named
character reference dataset. `lib/web/HTMLNamedReferences.inc` incorporates
that data into C++ source, sorted with the leading ampersand removed and text
encoded as UTF-8 hexadecimal bytes. See the fixture README for the exact hash
and the C++ recorder. NeverD's bounded tokenizer and local-path comparator are
independent implementations, not copied browser parser code.

Enabled web builds stage these notices beside the shared library and in the
SDK, and install them under `share/neverd/licenses/whatwg`.
