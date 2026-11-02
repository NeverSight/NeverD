# SQLite

The shared NDDB reader embeds the unchanged SQLite 3.54.1 amalgamation from
<https://sqlite.org/2026/sqlite-amalgamation-3540100.zip>.

- SHA-256: `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9`
- Upstream SHA3-256: `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`
- Source and public-domain dedication: <https://sqlite.org/copyright.html>

The original source notice is retained in the downloaded amalgamation:

> The author disclaims copyright to this source code. In place of
> a legal notice, here is a blessing:
>
> May you do good and not evil.
> May you find forgiveness for yourself and forgive others.
> May you share freely, never taking more than you give.

NeverD builds it statically with loadable extensions disabled. The desktop's
Qt SQL writer continues to use Qt's SQLite driver and its distributed notices.
