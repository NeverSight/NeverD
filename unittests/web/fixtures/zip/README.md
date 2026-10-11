# ZIP32 fixture provenance

These two self-authored archives were recorded on macOS arm64 with libarchive
3.8.0 on 2026-10-11, using the adjacent C++ recorder. They contain only the
recorder's inert package, JS, HTML, directory and empty-file strings. No third
party application or target execution is involved. The recorder is independent
of NeverD's ZIP reader and is not built or invoked by product code or tests.
Tests consume the preserved bytes without requiring libarchive.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `libarchive-stored.zip` | 747 | `9ca1c0715639ea30a429c83726f5c1395aeaee3352c43fee972d986023b94091` |
| `libarchive-deflate.zip` | 747 | `437b82da0a97f30d964e0f37ff4cff127e81f8f21e9d4418034a3308778377a1` |

The writer emits Unix metadata, local extras and data descriptors. The deflate
case adds an independent compressed stream, while `ZipFixture.h` supplies
stored-block raw deflate and malformed records in C++. Re-record only into a
fresh directory with the pinned library; compare bytes before replacing a
fixture. Do not infer support for all libarchive ZIP options from these files.

The recorder calls the public [libarchive API](https://www.libarchive.org/);
no upstream source is copied or embedded in NeverD.
