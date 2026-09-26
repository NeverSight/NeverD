# Third-Party Notices

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

## FPREM anti-emulation regression

The FPREM regression in
`unittests/semantic/x86/X86_X87TranscendentalRTTests.cpp` adapts the operand
construction and status-check sequence from
[`fprem-anti-emu.asm`](https://github.com/gmh5225/fprem-anti-emulation/blob/f49ff009e062af2a23c5c5eec91649520ca605f0/fprem-anti-emu.asm).
Copyright (c) 2026 Packmad. The original MIT license is preserved in
[LICENSES/FPREM-Anti-Emulation.txt](LICENSES/FPREM-Anti-Emulation.txt).
The sequence was adapted into a callable C inline-assembly round-trip test
on 2026-09-26.

## zlib

Mobile ZIP extraction links zlib for DEFLATE and CRC-32. CMake uses an installed
library when available, or builds the unchanged, hash-pinned zlib 1.3.2 source
archive as a static dependency. Its copyright and license remain in the source
distribution. See the [zlib license](https://zlib.net/zlib_license.html) and
[source releases](https://zlib.net/fossils/).

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
