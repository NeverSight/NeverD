# Preserved ASAR interoperability fixtures

These files are unchanged data from
[`electron/asar` commit e4fb057678562b7b6170699a046d983ae6d31cb8](https://github.com/electron/asar/tree/e4fb057678562b7b6170699a046d983ae6d31cb8),
retrieved on 2026-10-10. The upstream MIT license is preserved in
[`LICENSES/asar/LICENSE.md`](../../../../LICENSES/asar/LICENSE.md).
The source tree describes its package as a development version: neither this
commit nor layout compatibility authenticates a published producer version.
No upstream script, runtime or archive tool was run to read or generate these
fixtures. All new boundary fixtures and tests are C++.

| Local file | Upstream path | SHA-256 |
|---|---|---|
| `packthis.asar` | `test/expected/packthis.asar` | `9f6a1857d060c5f03b3e33504ff5db5010e54b035b7b85ce44cb53742b29c2c2` |
| `packthis-unpack.asar` | `test/expected/packthis-unpack.asar` | `50da7f9d94f3d2da4983c4482639a4e7b6125dd29d4706530ee2f59569e85b47` |
| `all-unpacked.asar` | `test/expected/packthis-all-unpacked.asar` | `a35ffaa85641c6be846d8715f4e5e34a6b0b268717e1a8aa46db3e40ee7f943f` |
| `unicode.asar` | `test/expected/packthis-unicode-path.asar` | `93cf302c522c2307a6f23a873acf05d60810d5cdaeb4d64ec1b95fe3c29a50a9` |
| `prototype.asar` | `test/expected/packthis-object-prototype.asar` | `cbfc2c29b4e661c69f1aaf0f4192d2cc266a6a16cd81faf215a3c5a561a6b593` |
| `unpacked-file.bin` | `test/input/packthis/dir2/file2.png` | `cc402b796dc92b2b1f3a6d09515003d8400e63d8acaffc967e49c0cf015fcffe` |

`AsarTests.cpp` verifies these archive hashes, available members' whole/block
integrity, the original Unicode path and exact bytes against the independent
`unpacked-file.bin`. The same original member is explicitly supplied in a
captured unpacked directory; a competing host-side companion is ignored.
Synthetic C++ fixtures cover structures absent from this preserved corpus and
corrupt variants. Synthetic writer/reader agreement is not substituted for the
upstream interoperability checks.
