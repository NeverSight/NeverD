# nlohmann JSON attribution

The worker and native web MCP server compile the header-only JSON library from
the existing pinned [v3.11.3 release](https://github.com/nlohmann/json/releases/tag/v3.11.3).
`LICENSE.MIT` is copied unchanged from that archive. Its SHA-256 is
`86b998c792894ccb911a1cb7994f7a9652894e7a094c0b5e45be2f553f45cf14`.
It retains Niels Lohmann's copyright and the MIT terms.

`tools/common/transport/CMakeLists.txt` owns the archive pin, shared JSON target,
staging and install rule. Redistribute this notice with either executable.
