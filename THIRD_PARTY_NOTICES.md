# Third-Party Notices

## Capstone

NeverD links the Capstone fork at the exact submodule revision recorded by its
source commit. The Windows Web product includes the original license set and
contributor credits from `third_party/capstone/LICENSES` and `CREDITS.TXT` under
`share/neverd/licenses/capstone`; the product receipt records that source
revision. The fork's original license and component notices remain applicable.

## nlohmann JSON

The native C++ worker and offline web MCP transport use the header-only
nlohmann JSON 3.11.3 library. Both use the existing pinned release archive,
SHA-256 `d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d`.
The unchanged [MIT notice](LICENSES/nlohmann-json/LICENSE.MIT) is staged
beside both executables and installed under `share/neverd/licenses`.
No external JSON executable or language runtime is invoked.

## mimalloc

The 64-bit Linux worker embeds the unchanged mimalloc 3.5.3 allocator from
[Microsoft Research](https://github.com/microsoft/mimalloc/tree/v3.5.3), under
its [MIT license](LICENSES/mimalloc/LICENSE). The source archive is pinned by
SHA-256 `3b4a15153a59905995f7070296ed604bb5ccc00cabb8b93446931aff77224d47`.
Its notice is staged beside the worker and installed under
`share/neverd/licenses/mimalloc`. The allocator is embedded in the executable;
the SDK continues to use its host's allocator. `NEVERD_WORKER_MIMALLOC=OFF`
selects the platform allocator. Allocation-sanitizer builds use that setting.
The build preserves the existing minimum CPU instruction set.

## Ada URL parser

Offline import-map analysis embeds the unchanged C++ Ada 4.0.0 URL parser at
`b12a893a45809da8103bb4f1e2f6f5ee13f9100b`. NeverD selects its MIT license and
preserves the Ada, included Ada IDNA and Unicode notices under
[`LICENSES/ada`](LICENSES/ada/README.md). The build pins the archive hash and
compiles source directly; no upstream generator, tool or JavaScript runtime
is invoked. URLPattern and its regex backend are excluded.

## ASAR format reference and preserved fixtures

The independent C++ reader follows the Pickle, filesystem and integrity
format written by `electron/asar` at commit
`e4fb057678562b7b6170699a046d983ae6d31cb8`. Five unchanged test archives and one
independent original member from that commit are retained as inert test data.
No upstream JavaScript/TypeScript implementation or external ASAR executable
is run or embedded. The original MIT notice is preserved in
[`LICENSES/asar/LICENSE.md`](LICENSES/asar/LICENSE.md); exact paths and hashes
are recorded in the [fixture manifest](unittests/web/fixtures/asar/README.md).
The source commit is a development tree, not an authenticated writer release.

## ICU archive path policy

The optional ASAR and package tar readers link native ICU4C 77.1 common/data libraries for
NFC normalization and full case folding, with Unicode 16.0 runtime checks.
The full upstream Unicode License V3 and bundled third-party notices are
preserved in [`LICENSES/icu/LICENSE`](LICENSES/icu/LICENSE), matching
[`release-77-1/LICENSE`](https://github.com/unicode-org/icu/blob/release-77-1/LICENSE).
NeverD calls the library in process; no ICU command-line utility is invoked.
ASAR and enabled ICU notices are staged beside libneverd and inside the SDK,
and installed under `share/neverd/licenses`. ICU runtime packaging and other
host qualification remain separate release requirements.

## Hermes JavaScript parser

The optional native JavaScript analysis feature embeds the parser, AST and
support libraries from Meta Hermes at revision
`602befee340da188ea1560cde7a9cc33e6b180dc`. It does not link the Hermes VM or
invoke JavaScript tools. Primitive Number formatting also uses the embedded
support/dtoa library. The archive is pinned by SHA-256
`6c615757374850ccb99991c4536aaf86ac0973661b1ae18aac45d2ca85143153`.
See the [upstream source](https://github.com/facebook/hermes/tree/602befee340da188ea1560cde7a9cc33e6b180dc)
and preserved [license set](LICENSES/hermes).

Hermes is Copyright (c) Meta Platforms, Inc. and affiliates, under the MIT
license. Its LLVH and regular-expression support retain LLVM-derived licenses;
its dtoa dependency retains David M. Gay and Lucent Technologies' notices.
These libraries contain their original C/C++ sources. NeverD's new parser
integration, analysis and adapter implementation is C++.

`lib/web/hermes-overlay/hermes/Support/Allocator.h` preserves the upstream
header and adds an allocation-budget hook, dated 2026-10-10. The custom CMake
integration also generates a private copy of `JSParserImpl.cpp` with one
source-location fix: async-arrow rest parameters retain the original spread
node's parser-owned source range. It requires the pinned code to match exactly
once, retains upstream notices, and does not modify the fetched sources.
The 2026-10-11 C++ extension adds resource-declaration grammar, preserves the
`using`/`await using` ESTree kinds and validates their names/async contexts in
the private semantic-validator copy. Its private lexer header also restores
newline state during lookahead backtracking. These edits are recorded with
exact-match anchors in `cmake/hermes-parser/Patches.cmake`; all copies retain
their upstream notices. NeverD's added grammar is in
`lib/web/HermesResourceDeclarations.inc`. The parser profile is
`hermes-602befee-js-v3`.
The integration selects the parser dependency graph and excludes upstream VM,
tool and test targets. Enabled builds stage the license set beside libneverd,
inside the SDK and under the installed `share/neverd/licenses/hermes`.

## Z3

The optional solver backend (`NEVERD_ENABLE_Z3`) uses Z3 under the MIT license.
The default FetchContent provider builds the unmodified 4.13.3 sources at
revision `54d30f26f72ce62f5dcb5a5258f632f84858714f`; a system-library provider is
also available. The original copyright and license are preserved in
[LICENSES/Z3.txt](LICENSES/Z3.txt) and staged under `licenses/z3` beside enabled
binaries and in the SDK. See the [upstream source](https://github.com/Z3Prover/z3/tree/54d30f26f72ce62f5dcb5a5258f632f84858714f).

## Unicorn Engine

The optional Windows driver emulator (`NEVERD_ENABLE_DRIVER_EMULATION`) links
the repository's pinned `third_party/unicorn` CPU engine. Semantic tests also
use this dependency. Unicorn is based on QEMU; its README identifies the
engine's license as GPLv2. Component-specific notices and licenses remain in
the original source files.

See [Unicorn's license](third_party/unicorn/COPYING),
[authors](third_party/unicorn/AUTHORS.TXT),
[credits](third_party/unicorn/CREDITS.TXT),
[third-party notices](third_party/unicorn/THIRD_PARTY_NOTICES.md), and
[QEMU's licensing description](third_party/unicorn/qemu/LICENSE).
Enabled builds stage these notices and the dependency's GPL/LGPL texts under
`licenses/unicorn` beside the binaries and in the staged SDK. NeverD's project
license does not replace these dependency notices.

The pinned NeverSight fork includes the original 2026-09-30 exception-delivery
fix by NeverD contributors: x86 interrupt hooks acknowledge the in-flight
exception before resumption, preventing unrelated later faults from becoming
double faults. The change and original regression tests are preserved in
[commit cf40fa2c](https://github.com/NeverSight/unicorn/commit/cf40fa2c3ccdd90ab7cbb4ff060d9e07921aa3ae).
The modified QEMU file retains its original LGPL notice.

The fork also includes the original 2026-09-30 SSE denormal-status fix by NeverD
contributors. The x86 arithmetic helpers report MXCSR.DE for subnormal inputs
without changing the shared SoftFloat model for other architectures, and retain
NaN, divide-by-zero and negative-square-root priority. Original regressions and
the dated modification notice are preserved in
[commit 16c0b3fd](https://github.com/NeverSight/unicorn/commit/16c0b3fd9486b598287ad79553768ed44805e217).
The modified SSE helper retains its original LGPL notice. The instruction
semantics were checked against the
[Intel floating-point reference](https://www.intel.com/content/www/us/en/developer/articles/technical/floating-point-reference-sheet-for-intel-architecture.html)
and independently executed host instructions; no reference implementation was
copied into NeverD.

The original 2026-10-04 SSE MIN/MAX DAZ correction by NeverD contributors is
preserved in [commit 16676651](https://github.com/NeverSight/unicorn/commit/16676651a4655bd486ae483f329e0550be09863b).
The x86 helpers normalize the selected subnormal payload to signed zero when
DAZ is enabled, retaining source-order, NaN and sticky-status behavior. Original
unit fixtures are in `tests/unit/x86_sse_minmax_daz.def`; the modified helper
retains its original LGPL notice and a dated change notice. Shared SoftFloat
code is unchanged.

The fork includes the original 2026-09-30 pre-entry cancellation fix by NeverD
contributors. The TCG entry boundary honors an engine stop even when CPU startup
has reset its exit flag. Original x64, ARM32 and ARM64 regressions control only
thread scheduling and public engine APIs, and check zero guest effects and an
independent subsequent run. The original QEMU notice and dated modification
notice are preserved in
[commit 9cbcf76a](https://github.com/NeverSight/unicorn/commit/9cbcf76a1eb74d200dca6a9d6cee3200e47e9743).


The fork also includes the original 2026-10-04 bounded single-step fix by NeverD
contributors, pinned at [commit df88be77](https://github.com/NeverSight/unicorn/commit/df88be772cfdffae7b1b930a47171d7c063f70dd).
A count-one run uses a one-instruction translation block and stops before
fetching a successor. Original public-API tests cover page tails, branches,
self-loops, ARM64 MMU successor faults, fault retention and code-store commitment on x64/ARM32/ARM64 as
applicable. The QEMU copyright/license and dated modification notice remain
in the modified source file.

The original 2026-10-04 EVEX register-prefix correction is preserved in
[commit 10d315e0](https://github.com/NeverSight/unicorn/commit/10d315e01526b2fb370a8646b13f4246b9b71694).
APX extends the U field only for memory indexing; register forms retain their
reserved-bit check. The decoder retains its original QEMU license notice.
The [Intel APX specification](https://cdrdv2.intel.com/v1/dl/getContent/784266)
and independent Linux x64 observations also distinguish legacy ROUNDPS
alignment faults from scalar/VEX page faults in the original regression tests.

## Swift runtime ABI declarations

`lib/loader/Swift/SwiftRuntimeDeclarations.inc` derives fixed C and Swift ABI declarations
from Swift's `include/swift/Runtime/RuntimeFunctions.def`, revision
`9215272a4725957dfabdd14e1ca76c0dfa4a3003`.

Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors.
The applicable Apache 2.0 license with Runtime Library Exception is included
in [LICENSES/Swift-Runtime-ABI.txt](LICENSES/Swift-Runtime-ABI.txt).
NeverD's generator extracts pointer, size, 32-bit integer and explicitly
zero-extended Boolean result carriers, together with explicit
nonreturning declarations, as recorded in the generated file. No Swift
runtime implementation is included. Two-word Swift results retain their
declared members. The metadata response layout comes from
[`IRGenModule.cpp:295-298`](https://github.com/swiftlang/swift/blob/9215272a4725957dfabdd14e1ca76c0dfa4a3003/lib/IRGen/IRGenModule.cpp#L295)
and [`Metadata.h:99-113`](https://github.com/swiftlang/swift/blob/9215272a4725957dfabdd14e1ca76c0dfa4a3003/include/swift/ABI/Metadata.h#L99)
at the same revision. The fixed Swift subset excludes the custom
parameter attributes assigned to `swift_willThrow` by
[`IRGenModule.cpp`](https://github.com/swiftlang/swift/blob/9215272a4725957dfabdd14e1ca76c0dfa4a3003/lib/IRGen/IRGenModule.cpp#L1127).

## richprint @comp.id table

`lib/loader/COFF/RichCompIds.inc` derives the tool kind, Visual Studio year
and description of each @comp.id record from richprint's
[`comp_id.txt`](https://github.com/dishather/richprint/blob/49f2dc93504db4a669c968fa80eb9b34591cb557/comp_id.txt),
and `lib/loader/COFF/RichHeader.cpp` decodes the Rich header as
[`richprint.cpp`](https://github.com/dishather/richprint/blob/49f2dc93504db4a669c968fa80eb9b34591cb557/richprint.cpp)
does, at revision `49f2dc93504db4a669c968fa80eb9b34591cb557`.
Copyright (c) 2015-2024 dishather. The original BSD 2-Clause license is
preserved in [LICENSES/richprint.txt](LICENSES/richprint.txt).
`scripts/generate_rich_comp_ids.py` regenerates the table from that file; the
decoder adds a check of the header's checksum. Adapted on 2026-09-28.

## WHATWG Encoding Standard indexes

`lib/support/TextEncodingIndexes.inc` holds the pointer-to-code-point indexes
of the [WHATWG Encoding Standard](https://encoding.spec.whatwg.org/) (the
`index-*.txt` files of 2024-09-18: GB18030 with its ranges, Big5, JIS X 0208,
EUC-KR and fifteen single-byte encodings), and `lib/support/CommonCharacters.inc`
the common Han and Hangul characters derived from them, as
`scripts/generate_text_encoding_tables.py` writes them; the script checks each
index's published identifier. Copyright © WHATWG (Apple, Google, Mozilla,
Microsoft). The standard is licensed under the Creative Commons Attribution 4.0
International License, and the portions incorporated into source code under
the BSD 3-Clause License; both are preserved in
[LICENSES/WHATWG-Encoding.txt](LICENSES/WHATWG-Encoding.txt).
`lib/support/TextCodec.cpp` implements the standard's decoders for these
encodings. Generated on 2026-10-06.

## Windows type libraries

`lib/types/headers/WindowsKernelX64.h` and `WindowsKernelX86.h` hold the
types and prototypes that `<ntifs.h>`, `<ntddk.h>` and `<wdm.h>` declare for
x64 and x86 drivers of Windows 10 2004, and `lib/types/headers/WindowsUserX64.h`,
`WindowsUserX86.h` and `WindowsUserARM64.h` those that `<windows.h>`, the
headers of the libraries programs most often call and phnt's native API
declare for x64, x86 and ARM64 programs of Windows 11 24H2, as
`scripts/generate_windows_types.py` renders them from the headers of
mingw-w64 13.0.0 (the Ubuntu package mingw-w64-common 13.0.0-2ubuntu1) and,
for the native API, of phnt (below). The
DDK and PSDK headers they read that come from ReactOS (contributors include
Amine Khaldi and Timo Kreuzer) state that they are not copyrighted and
offered for use in the public domain; `ddk/csq.h` (Copyright 2004 Vizzini)
states the same, and the SDK headers of the mingw-w64 runtime package that
say so state that they are placed in the public domain. The other mingw-w64
runtime headers they read (`basetsd.h`, `guiddef.h`, `winnt.h`, `winbase.h`,
`winuser.h` and others), and those WIDL generates from its IDL files, are
Copyright 2009-2016 the mingw-w64 project under the Zope Public License 2.1.
The notices and the license are preserved in
[LICENSES/mingw-w64-headers.txt](LICENSES/mingw-w64-headers.txt). The
generated files are changed from the headers -- NeverD reads their
declarations and renders them again -- and say so with their date. The C
runtime's, winpthreads' and the compiler's own headers, some under the GPL
or the LGPL, are left out, and so are the SDK headers mingw-w64 takes from
Wine under the LGPL (`msxml.h`, `propsys.h`, `objectarray.h`,
`structuredquerycondition.h`, `knownfolders.h`, `devpkey.h`) with the
declarations that name their types. Generated on 2026-10-16.

The user mode libraries' native API -- the `Nt`, `Zw`, `Rtl` and `Ldr`
routines ntdll exports and the records they take, the PEB and the TEB among
them -- is rendered from the headers of
[phnt](https://github.com/winsiderss/phnt) at commit
53fbbdc5b5d2b08761db1c7b26bfa8c820924356, Copyright (c) 2022 Winsider
Seminars & Solutions, Inc., under the MIT License, which is preserved in
[LICENSES/phnt.txt](LICENSES/phnt.txt). The generated files are changed
from them as from mingw-w64's headers. phnt's `ntintsafe.h` and
`ntstrsafe.h`, which are Microsoft's, are left out;
`scripts/windows_types` holds the few macros and types phnt takes from
Microsoft's SDK that mingw-w64 lacks. Generated on 2026-10-17.

## FPREM anti-emulation regression

The FPREM regression in
`unittests/semantic/x86/X86_X87TranscendentalRTTests.cpp` adapts the operand
construction and status-check sequence from
[`fprem-anti-emu.asm`](https://github.com/gmh5225/fprem-anti-emulation/blob/f49ff009e062af2a23c5c5eec91649520ca605f0/fprem-anti-emu.asm).
Copyright (c) 2026 Packmad. The original MIT license is preserved in
[LICENSES/FPREM-Anti-Emulation.txt](LICENSES/FPREM-Anti-Emulation.txt).
The sequence was adapted into a callable C inline-assembly round-trip test
on 2026-09-26.

## webpack bundle profile and fixture

The C++ bundle recognizer's fixed runtime shape and the inert documented
CommonJS fixture use webpack material from commit
`6c9f912af2dfbb3e0e1a2a3ecdc3881c96363432` (repository tag `v5.99.9`).
Copyright JS Foundation and other contributors; MIT license preserved in
[`LICENSES/webpack/LICENSE`](LICENSES/webpack/LICENSE).
The original README and the fixture derivation/hashes are retained in
`unittests/web/fixtures`. No webpack JavaScript implementation is executed or
shipped as an analyzer. A repository tag does not authenticate the documented
sample's actual producer version; that manifest field remains unknown.
Enabled builds stage the license beside libneverd and in the SDK, and install
it under `share/neverd/licenses/webpack`.

## Node SEA layout reference and generated corpus

The independent C++ SEA reader follows Node.js 22.15.0 commit
`b009466555c360513b8012ce549f716501090ee5` serialization and bundled postject
resource-location contracts. Node/V8/postject code is not linked or invoked by
NeverD. Small trusted fixture outputs include opaque V8 code-cache data; the
runtime snapshot and full executables remain outside the repository.
The upstream [license and notices](LICENSES/node/LICENSE) are preserved and
shipped with the enabled web backend. See the [profile](docs/web-sea-profile.md)
and [fixture provenance](unittests/web/fixtures/sea/README.md).

## Bun layout reference and generated corpus

The independent C++ Bun reader follows the ELF/Mach-O/PE, standalone graph and serialized
source-map data formats from `oven-sh/bun` commit
`744846f844374847c902b5e7fd59b4342a51ef99` (`bun-v1.4.2`). The self-authored
fixture graph data may contain JavaScript scaffolding emitted by that compiler.
The upstream [LICENSE.md](LICENSES/bun/LICENSE.md), which states that Bun
itself is MIT-licensed and lists its runtime dependencies, is preserved.
No Bun runtime, Rust implementation or JavaScriptCore library is linked or
redistributed by this feature. Full compiler-produced native fixtures remain
outside the repository; the checked-in corpus preserves graph data only.
See [the profile and pinned sources](docs/web-bun-profile.md) and
[fixture provenance](unittests/web/fixtures/bun/README.md).
Enabled web builds ship the reference notice beside the binaries, in the SDK
and under `share/neverd/licenses/bun`, including parser-disabled builds.
Serialized-source decoding prefers the existing LLVM Support native Zstd
backend. If LLVM omits Zstd, the optional exact 1.5.7 static SDK supplies the
in-process decoder under the preserved [BSD license](LICENSES/zstd/LICENSE).
The [dependency notice](LICENSES/zstd/README.md) is staged beside the binaries,
in the SDK and under `share/neverd/licenses/zstd`. Both paths share NeverD's
independent C++ envelope/budget preflight against the
[Zstandard format](https://github.com/facebook/zstd/blob/v1.5.7/doc/zstd_compression_format.md).
No Zstd implementation source is copied into NeverD, and no external tool is
called.

## WHATWG HTML named character references

`lib/web/HTMLNamedReferences.inc` incorporates the official WHATWG
[named character reference dataset](https://html.spec.whatwg.org/entities.json),
retrieved 2026-10-10, into a sorted C++ lookup table. The exact snapshot, hash
and C++ transformation are documented in
[`unittests/web/fixtures/html/README.md`](unittests/web/fixtures/html/README.md).
Copyright © WHATWG (Apple, Google, Mozilla, Microsoft). The full upstream
[license](LICENSES/whatwg/LICENSE) is retained: CC BY 4.0 for the work and
BSD 3-Clause for portions incorporated into source code. Enabled web builds
stage these notices with the library and SDK and install them under
`share/neverd/licenses/whatwg`. No browser parser or target JavaScript is
executed by this implementation.

## zlib

Offline package gzip admission links the installed native zlib library when
available, using only its in-process stream decoder and checksum validation.
No zlib executable or upstream implementation source is copied into the web
component. Gzip capability is unavailable when this dependency is omitted;
plain tar and original SRI verification remain independent.

Mobile ZIP extraction links zlib for DEFLATE and CRC-32. CMake uses an installed
library when available, or builds the unchanged, hash-pinned zlib 1.3.2 source
archive as a static dependency. Its copyright and license remain in the source
distribution. See the [zlib license](https://zlib.net/zlib_license.html) and
[source releases](https://zlib.net/fossils/).
The Windows Web product includes the unchanged 1.3.2 notice under
`share/neverd/licenses/zlib`; its exact source archive and hash are recorded
in the product dependency manifest.

The test fixture `unittests/lift/aarch64/test_jumptable_inflate_gcc_a64.s` is
the AArch64 assembly GCC 15.2.0 produces for `inflate()` from the unchanged
zlib 1.3.1 `inflate.c`, kept with only that function and the data it reads.
The fixture retains zlib's copyright and license notice. It is assembled into a
test object and is not shipped.

## Intel x86 approximation reference implementations

The following files incorporate publicly released x86 approximation reference
implementations:

- `lib/ir/low/X86ApproxReference.c`
- `lib/ir/low/X86ApproxReference28.c`

Copyright (c) 2015, Intel Corporation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
- Neither the name of Intel Corporation nor the names of its contributors may
  be used to endorse or promote products derived from this software without
  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The original x64 FP-state transport and test implementations were checked
against the [Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
[Linux KVM API](https://docs.kernel.org/virt/kvm/api.html),
[WHP register ABI](https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/funcs/whvvirtualprocessordatatypes),
and the pinned Unicorn register ABI. The implementation retains full 80-bit
lanes, physical abridged tags and FXSAVE64 logical stack rotation. No code from
QEMU or Wine was copied into this implementation.
