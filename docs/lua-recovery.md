# Lua source recovery

Run `neverd lua` or choose **File → Recover Lua source…**. Both use the same
offline C++ reader. The workbench sends the request to its analysis worker.
Select **Upstream Luau 0.659** for binary output from that compiler. Luau
bytecode has no identifying signature. A filename alone cannot establish its
opcode encoding.

```sh
neverd lua sample.luac
neverd lua sample.luac --json -o inventory.json
neverd lua sample.luac --prototype=1 -o function.lua
neverd lua game.luauc --profile=luau-0.659
```

The default source view selects the entry prototype. The JSON report lists
zero-based prototype IDs, code, diagnostics and navigation. In the workbench,
select a prototype and a bytecode row to jump to the corresponding source
line. **Evidence** shows the header, debug metadata and input SHA-256. Copy and
source export use the selected prototype. Report export includes all prototypes.
File exports create a new file. Ctrl+C in the CLI and **Cancel** in the dialog
request a stop. Closing the dialog releases its worker.

## Coverage

| Input or selected profile | Identification | Layout and instruction decoding | Source emission | Owned validation |
| --- | --- | --- | --- | --- |
| UTF-8 `.lua`, `.luau` | Extension and text check | Byte range of the original file | Exact bytes, including BOM and line endings | Extraction, copy/export and CLI/GUI parity |
| PUC Lua 5.1 | Signature, version 0x51, ABI header | 5.1 prototypes, RK operands and inline closure bindings | Qualified register operations and structured regions | 5.1.5 |
| PUC Lua 5.2 | Signature, version 0x52, ABI header and tail | 5.2 constants, nested prototypes, upvalue descriptors and jumps | Qualified operations including environment access | 5.2.4 |
| PUC Lua 5.3 | Signature, version 0x53 and numeric probes | 5.3 integer/float tags, operands and debug records | Includes integer operators and float literals | 5.3.6 |
| PUC Lua 5.4 | Signature, version 0x54 and numeric probes | Terminating-bit sizes, delta line tables, paired arithmetic | Includes checked metamethod order and lexical close scopes | 5.4.8 |
| PUC Lua 5.5 | Signature, version 0x55 and four representation probes | Aligned code, shared strings, signed integer varints and 5.5 loop slots | Includes qualified ordinary varargs, numeric/generic loops and finalizers | 5.5.0 |
| LuaJIT | ESC-LJ, dump v2 and compatibility flags | Independent child stack, GC/numeric pools, debug records, FR1/FR2 call layout | Qualified LuaJIT operations, closures, calls and loops | Both dump frame layouts at revision `3bfb8fa4ab970b36d42d108ead04e92341c49998` |
| Upstream Luau 0.659 | Explicit profile, bytecode 6, type version 3 | Prototype/string tables, AUX words, capture records and line tables | Qualified Luau operations, calls, closures and loops | Compiler `f8a1e0129dce62c8c07ed5828781438e74452671`, `-O0`, including `-t1` type metadata |
| Other revisions, encodings or embeddings | Retain the available version/profile evidence | Unsupported layouts stop at their boundary | No compatibility assumed | Unqualified |

PUC compiler fixtures use 32-bit C `int`, binary64 numbers and little-endian
instruction words. A synthetic 5.1 test also checks big-endian decoding.
Versions with a separate integer subtype use 64-bit Lua integers. The reader follows the header's byte order and 4/8-byte serialized
`size_t` where present. The fixture matrix records the actual toolchain
configurations. Integral `lua_Number`, nonstandard chunk format numbers and
other numeric representations have an unsupported outcome. The LuaJIT dump
records its frame flag. The owned LuaJIT compiler emitted both frame encodings
on an arm64 GC64 host, where the source comparisons ran. The producer revision
and GC64 runtime configuration
are unavailable from those bytes alone. Luau's selected compiler revision
describes the reader profile. Custom opcode encoders need their own profile.

## Reading the result

`language-recovery-v1` records the input SHA-256, selected profile, header
evidence, root prototype and function inventory. Instruction offsets are file
byte offsets, PCs count serialized words, and source line numbers start at one.
An instruction can cover multiple words, including AUX, capture descriptors or
an arithmetic fallback. Successor PCs refer to instruction boundaries.
Original debug names and line numbers are separate from generated register and
upvalue names. A prototype viewed in isolation can refer to its enclosing
bindings. A chunk with fixed parameters is displayed as a callable function.

The artifact and individual functions have explicit statuses. Plain text
produces `extracted_source`. A fully expressed body produces
`reconstructed_source`. An unresolved instruction, open-result window, capture
lifetime or branch produces `partial_reconstruction` and a diagnostic for that
prototype. Completed siblings and the available bytecode evidence remain
inspectable. This includes unqualified Luau import caches, shared closure
identity, LuaJIT table templates and PUC close scopes
with unresolved lifetimes. Malformed records, unknown profiles, I/O failures,
budget exhaustion and cancellation have distinct outcomes. The CLI exits
nonzero for incomplete recovery and can still export the evidence with `--json`.

The parsers read immutable bytes without invoking a Lua loader, VM or plugin.
One request admits up to 512 KiB of input, 8,192 instruction words and 32,768
inventory/decoding items. Text expansion and nesting have independent limits.
Debug information is optional. No original identifier or producer version is
derived from stripped data.

## Verification

The [owned sources and compiler manifest](../unittests/lua/fixtures) cover
debug/stripped artifacts, short-circuit control, captured writes, nil-bearing
multiple returns, varargs, iteration, integer subtypes, metamethod order and
5.4/5.5 finalization. The behavioral qualifier compiles the owned inputs, checks
the recorded byte hashes and runs the original and recovered **source text**
with each matching interpreter. Submitted bytecode is never executed.
The [reader tests](../unittests/lua/LuaRecoveryTests.cpp) include damaged
operands, missing auxiliary words, every truncated prefix of representative
chunks and cancellation. The [workbench test](../tools/neverd-gui/tests/LuaRecoveryDialogTests.cpp)
compares real CLI and desktop exports.

```sh
python3 scripts/build.py build-release --target NeverDLuaRecoveryTests neverd-gui-lua-tests --parallel 4
ctest --test-dir build-release -R 'LuaRecovery' --output-on-failure
python3 unittests/lua/qualify.py --tools /path/to/fixture-toolchains --neverd build-release/bin/neverd
```

Wire-format references include the [PUC Lua releases](https://www.lua.org/versions.html),
[LuaJIT dump definitions](https://github.com/LuaJIT/LuaJIT/blob/3bfb8fa4ab970b36d42d108ead04e92341c49998/src/lj_bcdump.h)
and [Luau 0.659 bytecode definitions](https://github.com/luau-lang/luau/blob/f8a1e0129dce62c8c07ed5828781438e74452671/Common/include/Luau/Bytecode.h).
