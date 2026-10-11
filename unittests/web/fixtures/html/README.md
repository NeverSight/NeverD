# HTML named character reference fixture

Source: [WHATWG entities.json](https://html.spec.whatwg.org/entities.json),
retrieved 2026-10-10. Copyright © WHATWG (Apple, Google, Mozilla, Microsoft).
The upstream [license](../../../../LICENSES/whatwg/LICENSE) is preserved.

The unmodified JSON contains 2,231 names and is 145,897 bytes long. SHA-256:
`d741d877ac77c4194c4ad526b5b4a19aef8dfe411ab840a466891cdbb9f362e6`.
The content hash pins this snapshot independently of the moving specification.

`RecordReferences.cpp` is an optional C++ development tool. It checks the exact
input hash, sorts the keys, strips the initial ampersand and encodes the
replacement UTF-8 bytes as C++ string literals. Production and tests consume
the checked-in table directly and never run this recorder or access a network.

```sh
cmake --build build-release --target neverd-web-html-reference-record
build-release/bin/neverd-web-html-reference-record \
  unittests/web/fixtures/html/entities.json lib/web/HTMLNamedReferences.inc
```

C++ tests check every name's replacement and independently decode the fixture's
UTF-8 text against its code point array. Additional markup is self-authored
inert test input; no browser or JavaScript runtime is used as a test oracle.
