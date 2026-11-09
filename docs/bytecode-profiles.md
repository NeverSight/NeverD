# External bytecode profiles

`neverd-bytecode` recovers source from a caller-specified instruction language.
It accepts raw instruction bytes, a JSON encoding profile, and explicit function
ranges. The decoder lives in `lib/analysis/bytecode`; it lowers to ordinary LowIR
and uses the existing MedIR, HighC and LLVM-to-C backends. Profiles and input
containers are supplied separately. There is no built-in dialect, proprietary
opcode table, container signature or host-service registry.

The command-line tool, public C API and Python SDK use one recovery pipeline
in `lib/pipeline/BytecodeRecovery.cpp`. The decoder remains the authority for
encoding and CFG validation; the pipeline owns state lowering and source-route
selection. Plugins supply their own container reader and rules.

```sh
cmake --build build-release --target neverd-bytecode NeverDBytecodeAnalysisTests
build-release/bin/neverd-bytecode program.bin --profile language.json \
  --functions functions.json --check
build-release/bin/neverd-bytecode program.bin --profile language.json \
  --functions functions.json --bindings imports.json -o recovered.c
build-release/bin/neverd-bytecode program.bin --profile language.json \
  --functions functions.json --bindings imports.json --llvm -o recovered-llvm.c
build-release/bin/neverd-bytecode program.bin --profile language.json \
  --functions functions.json --bindings imports.json --llvm --optimize \
  -o recovered-optimized.c
```

`--base` sets the logical address of the first input byte (default zero).
`--check` validates reachable decoding and control flow; it does not claim
semantic equivalence or require call implementations. Source output uses an
atomic replacement after complete conversion. An unknown encoding, malformed
operand, ambiguous match, overlapping instruction, unresolved indirect branch
or unbound source call is an error. Unsupported behavior never becomes a NOP.

Straight-line instructions share a basic block; branch targets and call
continuations start new blocks. State lowering forwards exact register views
within each block, invalidating overlapping views on partial writes and all
views on potentially aliasing guest stores. It preserves state writes visible
to guest memory accesses and calls, without assuming a disjoint state buffer.
Whole state writes may be removed when overwritten before a memory observer;
different-width state loads also count as observers.

`--llvm --optimize` applies the existing thin LLVM optimization policy before
C emission, simplifying scalar expressions and control flow while retaining
function boundaries. It does not infer a new function ABI or drop observable
register state. The default LLVM route only promotes temporary storage.
`--optimize` without `--llvm`, or together with `--check`, is rejected. All
transformed modules must pass verification; unsupported C projection still
fails before replacing the output file.

## Encoding schema

The following independently constructed language writes a 16-bit immediate to
the low halfword of its only 64-bit register, then returns:

```json
{
  "version": 1,
  "register_bytes": 8,
  "temporary_bytes": 64,
  "byte_order": "little",
  "encodings": [
    {
      "size": 3,
      "match": [{"offset": 0, "value": "0xb4"}],
      "operations": [{
        "op": "COPY",
        "output": {"space": "reg", "size": 2, "value": {"addend": 0}},
        "inputs": [{"space": "const", "size": 2,
                    "value": {"offset": 1, "bytes": 2}}]
      }]
    },
    {
      "size": 1,
      "match": [{"offset": 0, "value": "0xe7"}],
      "operations": [{"op": "RETURN", "inputs": []}]
    }
  ]
}
```

Input bytes `b4 34 12 e7` with this function declaration produce
`uint64_t example(void *state)`:

```json
[{"entry": 0, "end": 4, "name": "example"}]
```

The caller supplies at least eight bytes of state. The function replaces bytes
zero and one with `34 12`, preserves the remaining bytes and returns zero.
Function ranges are half-open, disjoint logical intervals. Names must be unique
C identifiers; C keywords and reserved leading underscores are rejected.
Only reachable instruction bytes count toward reported coverage. Unvisited
bytes are not assumed to be padding or to belong to another function.

Every match tests `(instruction[offset] & mask) == value`; the default mask is
255. All tests in an encoding must hold. Multiple matching encodings are errors,
including a complete short match with a potentially matching truncated longer
encoding. Instruction and operand reads stay inside the declared encoding.

Operands have a `space` (`reg`, `temp` or `const`), byte `size` (1, 2, 4 or 8)
and `value`. The value expression is:

```text
index = (read(offset, bytes, byte_order) >> shift) & mask
selected = lookup[index] if lookup is present, otherwise index
value = (selected * scale + addend + (pc_relative ? pc : 0)) mod 2^bits
```

| Field | Default | Meaning |
|---|---|---|
| `offset` | 0 | Byte offset within this instruction |
| `bytes` | 0 | Read width; zero supplies zero without reading |
| `shift` | 0 | Unsigned right shift before masking |
| `mask` | All 64 bits | Field bit mask |
| `lookup` | Absent | Exact finite mapping; an invalid index fails |
| `scale` | 1 | Unsigned multiplier |
| `addend` | 0 | Unsigned additive constant |
| `bits` | 64 | Final modular width, from 1 to 64 |
| `pc_relative` | false | Include this instruction's logical address |

Integers can be unsigned JSON numbers or strings such as `"0xffff"`. Negative
offsets use modular bit patterns and an explicit `bits` width. Signed arithmetic
and sign extension are LowIR operations; field extraction is unsigned.
Register and temporary offsets cannot overflow or truncate into a valid bank.
Only constants can be PC relative. Unknown JSON keys are rejected.

## Semantic contract

Register operands identify byte ranges. Writes preserve bytes outside the exact
output range, including narrow and overlapping writes. Temporary operands are
typed instruction-local values; a read requires a preceding definition with the
same offset and width. Overlapping writes invalidate prior temporary views.
Use `SUBBYTES` and `CONCAT` to express slicing explicitly.

An operation names a supported scalar LowIR opcode and an `inputs` array, with
`output` only for operations producing a value. Arity and widths are checked.
Profiles must specify exceptional arithmetic behavior themselves: division by
zero, signed overflow and oversized shifts cannot be inferred from an opcode
name. For example, a language that masks shift counts must emit the mask.

`BRANCH` takes an 8-byte constant target; `COND_BR` takes a target and a one-byte
condition. `CALL` takes an 8-byte constant target and has no output in the raw
bytecode model. Control transfers must be the last operation of their encoding.
`RETURN` explicitly denotes a language-level return. A native link register or
stack convention is not inferred from that operation.

Source recovery binds registers to a caller-owned, little-endian state buffer.
Instruction-field byte order is independent. Guest LOAD/STORE addresses remain
runtime pointer values and must designate accessible host memory. Generated C
is an offline source projection, not an emulator with mapped guest addresses.
The input is never executed by the recovery tool.

`--unaligned-pointers` requests direct scalar pointer accesses from either C
route. The emitted typedefs use Clang/GCC `aligned(1)` and `may_alias`
attributes, so an unaligned state buffer and overlapping integer/float views
retain their byte access semantics. These are host addresses under the same
contract above; the option does not translate guest virtual addresses or infer
original source types. The default keeps portable byte-copy accesses. Atomic,
segmented, partial-width and other unsupported carrier shapes retain their
existing exact access paths. The shared C emitter option is
`CEmitterOptions::UseUnalignedPointers`.

Normal fields, arrays and pointer expressions require proven object and type
information on either native architecture. ARM64 does not inherently require
memory helper calls. The explicit bytecode state ABI alone does not establish
original object types or alignment, so this API keeps portable copy helpers by
default; the optional compiler-specific spelling does not supply that proof.

Every bound callee has the same contract:

```c
uint64_t callee(void *state);
```

A callee may modify all state and guest memory. Zero continues at the following
bytecode instruction; a nonzero status returns immediately through every caller.
The state pointer is captured once on entry, even for a loop back to the first
instruction. Imported calls require explicit declarations:

```json
[{"address": 12288, "name": "example_callback"}]
```

`--with-context` selects a second, opaque parameter for every recovered function
and bound call:

```c
uint64_t callee(void *state, void *context);
```

The caller supplies the context when executing the generated C. Its pointer is
captured once on entry and passed unchanged through internal calls and external
bindings, including loops back to the entry. The callback owns the meaning of
its contents, which may change or alias state or guest memory; the lowering
assumes no disjointness. A null context is passed through without inspection.
Independent or reentrant invocations may use different contexts without a global
variable or a reserved state slot. Status propagation is unchanged. All bound
implementations must use the selected ABI; the tool neither provides their
bodies nor manages context lifetime. This runtime parameter is separate from
the decoder callback context used while recovering source.

Bindings supply names and signatures, not implementations. Register-indirect
calls must first be resolved by the producer or explicitly modeled through a
bound dispatcher with a defined state contract; guest integers are never cast
automatically to host function pointers. Native callbacks, suspension/resume
behavior, pointer relocation and original high-level argument recovery remain
the producer's responsibility.

## Reconstructing calls with explicit source contracts

`--llvm --optimize --source-contracts contracts.json` additionally exposes an
isolated register bank and replaces statically resolved gateway calls with typed
source calls. The C request and both Python recovery functions accept the same
`source_contracts` object. This option cannot be combined with observation
annotations: a sampled name or return value does not establish a call contract.

The producer must assert that state addresses cannot escape, guest/context
memory is disjoint from the bank, and callbacks cannot retain bank addresses.
For each declared import, a normally returning gateway must return **zero
status**, perform the listed fixed bank writes before the imported operation,
and write its result to the declared destination afterwards. Selector, arguments
and result-channel descriptors are captured on entry, before the fixed writes. Other bank bytes
are preserved. The imported operation may modify arbitrary guest memory and
may fail to return. Nonzero statuses from unresolved gateways and internal
state calls still propagate unchanged. An API's error result is separate from
the gateway's status.

For example, a caller-defined 64-byte bank can describe one service as follows:

```json
{
  "schemaVersion": 1,
  "state_bytes": 64,
  "isolated_bank": true,
  "zero_status_gateways": true,
  "gateways": [{
    "name": "runtime_service",
    "selector": {"offset": 32, "bytes": 4},
    "argument_count": {"offset": 56, "bytes": 4},
    "arguments": [{"offset": 8, "bytes": 8}],
    "result_offset": {"offset": 40, "bytes": 8},
    "result_bytes": {"offset": 48, "bytes": 8},
    "writes": [{"slot": {"offset": 56, "bytes": 8}, "value": 0}],
    "imports": [{"selector": 7, "name": "example_query",
                 "parameters": ["u64"], "result": "i32"}]
  }]
}
```

`result_offset` and `result_bytes` identify slots containing the destination
byte offset and write width, not the destination itself. Each slot has a byte
width of 1, 2, 4 or 8. All accesses must fit the declared bank. A missing
`selector` means selector zero; a missing `argument_count` means the number of
argument slots. The result offset and width must be constants
proven at the actual call site by the shared optimizer. A dynamic argument count
gets a runtime comparison: the matching arm uses the imported call and the other
arm retains the original gateway and status. The selector must be proven at the
site or checked against an advisory candidate as described below. Unlisted selectors,
incompatible constant counts and invalid destinations keep the original state
gateway. A signed result is extended before the exact-width
bank write; a void result writes zero by default. Supported source types are
`u8`, `i8`, `u16`, `i16`, `u32`, `i32`, `u64`, `i64`, `pointer`, and result-only
`void`. Fixed arguments are narrowed to their declared widths; signedness is
preserved in C declarations, calls and result extension. Exceptional call edges,
observable state addresses and unbounded bank addressing are unsupported.
This route currently requires integral 64-bit pointers and indices.

For a dynamic selector, NeverD may follow bounded dominating bank stores and
integer expressions to select one declared import as a candidate. Intervening
calls and writes may have changed that value; the old writer is not a constant
or preservation fact. The actual captured selector is compared with the
candidate's service number before using the imported call. Other values retain
the original gateway, status and bank effects. Candidate discovery adds no
memory reads and uses the target byte order for partial-width transport. Without
a declared candidate, the gateway remains unchanged. PHI and select arms can
supply an advisory candidate, including a loop's initial service number; this
does not establish agreement of the arms or stability across iterations.

With `localize_bank: true`, NeverD also computes conservative byte write sets
for exact in-module state callees, including recursive call cycles. A bounded
CFG analysis forwards unchanged, exactly typed state loads and removes a store
only when the same value already occupies the complete destination. Partial
writes invalidate every overlapping cached value; conflicting predecessors,
unknown callbacks and interposable callees prevent propagation. Complete bank
contents remain observable at calls and returns. This is not dead-return-state
elimination or recovery of an ordinary parameter ABI.

A gateway may additionally declare `"fallback_writes": {}`. This is a stronger
producer assertion covering **every fallback path**, including unrecognized
selectors, arity failures and nonzero statuses: only the fixed `writes` slots
and the result channel captured on entry may change in the bank. Reads may
still observe every bank byte, and guest-memory effects remain unrestricted.
NeverD uses this bound only where the captured result offset and width are
statically proven within the bank. Without the assertion, or without that
proof, a fallback can clobber every bank byte.

For gateways whose fallback always writes a fixed result width, use
`"fallback_writes": {"result_bytes": 8}`. The width must be 1, 2, 4 or 8;
it overrides only the fallback's captured width descriptor. It neither changes
the imported operation's result behavior nor manufactures a result value.
Unknown keys and malformed assertions are rejected. Successful trace samples
or ordinary import contracts do not establish this stronger fallback bound.

A void import may declare `"void_result": "unspecified"` when its gateway
captures physical return-register bits that the native void operation does not
define. NeverD omits that capture only after proving that every destination byte
is overwritten before any possible observer on every normal-return path. The
proof follows branches and switches with zero gateway status, accepts partial
stores only when they cover the whole destination, and stops at unknown memory
accesses, calls, exits, cycles or its analysis budget. Failure to prove this
retains the original gateway; its failure paths and bank writes are unchanged.
This policy does not permit replacing observed bits with zero. The explicit
`"zero"` policy retains the default behavior. `void_result` is invalid on a
non-void import.

A void import may instead declare `"void_result_argument": 0` when its gateway
copies a required argument into the result channel. The zero-based index names
the captured bank operand, before fixed writes, native type narrowing or string
normalization. Its raw bits are zero-extended or truncated to the result width;
the native void call and its guest effects still execute. The operand must be
required, with an existing bank slot. This assertion cannot be combined with
`void_result` or a non-void result. It does not infer a return value from the
native function name or its physical return register.

An import may provide `defaults`, an array of unsigned integer bit patterns for
a suffix of `parameters`. For example, `parameters: ["u32"]` and
`defaults: [0]` accept either zero operands (passing zero) or one supplied
operand. A known count must be between the required prefix length and the full
parameter count; a dynamic count gets the same runtime bounds check and
original-gateway fallback. Defaults are selected from the captured count before
fixed bank writes. Each bit pattern must fit its declared type, including signed
types: an `i8` default of `254` means `-2`. Pointer defaults must be zero.
Defaults cannot be combined with variadic imports or optional by-value strings.
These values are explicit caller assertions; NeverD does not infer a missing
argument's value from the callee name.

The required prefix must fit the gateway's `arguments`. Defaults may supply
additional trailing parameters without bank slots; the accepted count is capped
by the number of available slots. This represents explicit wrapper padding,
without reading nonexistent operands.

To forward a target held in an argument, replace `name` with `target_argument`,
the zero-based index of a required `pointer` in `parameters`. For example,
`parameters: ["pointer", "u64", "u64"]`, `target_argument: 0` and `defaults: [0]`
with two bank argument slots calls the captured target with the second slot
and a trailing zero. The count still includes the target operand. Other
parameters keep their order; the target must use an eight-byte slot. Null
targets, invalid counts and selector mismatches use the original gateway.
The contract asserts that nonnull targets support this fixed C calling ABI and
the declared effects. It does not identify a symbol or prove arbitrary pointers
callable. This mode cannot use `name`, `pointer_source`, a pointer gateway,
variadics, a defaulted target or a string contract on the target. It introduces
no guest-memory read to discover the callee.

An import may set `pass_context: true` to prepend the gateway's opaque context
pointer to its native call. With `--with-context`, a declaration such as
`{"selector": 7, "name": "receiver_query", "parameters": ["i16"],
"result": "i16", "pass_context": true}` emits a call with the signature
`int16_t receiver_query(void *context, int16_t value)`. The gateway must have
exactly two pointer arguments. Without that ABI, recovery rejects the contract
before changing the module. The default is false; non-boolean values are errors.

The receiver is the original call operand, including null, and is never read
or replaced by a global or observed value. The producer must define the
receiver-aware binding and its effects. No object layout or original method
identity is inferred. `parameters`, `defaults`, `variadic_fixed`,
`target_argument`, argument counts and string indices still refer to the bank
operands; the added pointer consumes no bank slot. Named, pointer-candidate and
forwarded-target calls share this rule. Context may alias guest memory under
the isolated-bank contract, so its effects remain ordinary call effects.

Scalar candidate selection can follow an earlier guest store when its address
has the same syntactic root or reloads its base from the same bank slot. This
is an advisory source for a runtime comparison, including after intervening
calls. Changed bases, overwritten bytes and different selectors still use the
captured value and original fallback; no guest load is introduced or removed
by this search. Unrelated roots, unsupported expressions or an exhausted work
budget keep the gateway.

Gateway `indirect_arguments` lists argument indices whose slots contain byte
offsets into the bank. Each points to an eight-byte value captured before fixed
writes. Unknown or out-of-bounds offsets preserve the original gateway; the
pass never introduces a speculative bank dereference. `excluded_arguments`
accepts `{ "argument": 0, "value": 18446744073709551615 }` entries for sentinel
values with additional gateway behavior. Matching exclusions take the original
call, including its full status and effects.

Optional `images` on the root supplies up to 16 candidate guest images:

```json
"images": [{"base": {"offset": 0, "bytes": 8},
            "readable": true, "bytes": "616263392b29325a"}]
```

`bytes` is hexadecimal, at most 65536 bytes per image. `readable: true` asserts
that the current base denotes a live, initialized, readable extent of that
size, disjoint from the bank. It does **not** assert that its contents are
immutable. A gateway may declare strings using argument and image indices:

```json
"strings": [{"argument": 0, "image": 0, "by_value": true,
             "mode": {"offset": 56, "bytes": 8},
             "xor_key": {"offset": 64, "bytes": 8}, "encoded_mode": 1}]
```

The string pointer can itself be an indirect argument. The optimizer must prove
its offset within the current image and the key. With both `mode` and `xor_key`
present, a nonzero full-width key and matching mode select bytewise XOR with
the low key byte, including the encoded NUL terminator. Omit both slots for
ordinary NUL strings. Decoding is bounded to 512 bytes including the terminator.
`by_value: true` asserts that the imported operation only reads the string's
content, does not retain or observe its address, and that gateway normalization
and scratch allocation have no other observable effects. These are explicit
producer obligations, not facts inferred from a successful observation.

NeverD decodes candidate bytes internally and emits a literal in the actual
imported call. Runtime checks compare the current bytes **including the
terminator** and the applicable mode before using it. Changed bytes, mode or
excluded arguments take the original gateway; the candidate never replaces
unverified memory. Memory comparisons occur only after scalar guards succeed.
No platform API name, sample string or image address is built into this pass.

A gateway that calls a function pointer may replace `selector` with an
eight-byte `function_pointer` bank slot. Each import then replaces its numeric
`selector` with `pointer_source: {"image": 0, "offset": 16}`, identifying a
candidate pointer slot in a declared image. The names and complete callable
signatures are explicit producer input; the candidate image's pointer bytes
are not used as addresses. Source slots must fit eight bytes in the image and
be unique within a gateway.

The core follows complete pointer loads and bounded dominating bank stores to
select a candidate, including advisory transport across opaque calls. This
does not prove that the cached pointer or bank value is unchanged. It compares
the captured current target to the declared imported function's actual address,
checks the argument count, and retains the original gateway for null, different
or unresolved targets. No speculative image read is introduced. A changed cache
after capture cannot make the pass call a different function. As with direct
imports, the generated C requires the declared native symbols when linked.

An import may set `variadic_fixed` to the positive number of fixed parameters
in its C declaration. `parameters` still describes every supplied operand,
including the promoted types of trailing arguments. For example, parameters
`["i32", "u64", "pointer"]` with `variadic_fixed: 2` declare a two-parameter
variadic function and supply one pointer vararg. The core guards the complete
argument count; it does not infer omitted or unknown varargs. LLVM receives the
variadic signature, and call-site scalar hints make C preserve each trailing
operand's width, signedness and pointer type, including small 64-bit constants.
Fixed integer arguments and imported function addresses preserve their declared
types too, so overloads in platform C headers cannot redirect the call or make
its address ambiguous. Narrow fixed parameters are supported; trailing integer
varargs must already use their promoted 32- or 64-bit types. Unpromoted 8- or
16-bit trailing declarations are rejected.
Floating-point and aggregate varargs remain unsupported by this source contract.
These models do not recover arbitrary encryption or establish a resolver's ABI.

A bounded refinement exposes narrow bank reads at the original wide-load point
when only low bytes feed a selector. Both byte orders keep the same snapshot,
including intervening writes. Unknown upper bytes are retained for wider uses.
The shared optimizer remains responsible for forwarding and simplification.
Explicit signed import types are preserved in C declarations through source
spelling attributes, without adding machine-ABI extension attributes.

The function's public state/context ABI and all bank bytes at returns and
unknown call boundaries remain intact. Scoped bank/guest alias facts, ordinary LLVM
scalar optimizations and direct call results can remove intermediate register
traffic and reduce flag tests to comparisons. Internal state functions retain
their boundaries; this does not infer their original parameters or object types.
The output header and C/Python reports count `reconstructed_calls` and
`unresolved_contract_calls`, with `guarded_contract_calls` identifying the
subset of reconstructed calls that need a count, argument or string guard. The unresolved count covers only unresolved calls to
**declared** gateways, not every remaining opaque call in the program.

The format-independent `StateSourceRecovery` owner accepts the same layout and
signatures for LLVM state functions on ELF or Windows x64 targets. The bytecode
adapter supplies validation and orchestration only. Native PE import/string
recovery is a separate producer; this option does not infer imports from a PE
or make runtime observations unconditional.

## C and Python plugin API

Include `neverd/sdk/NeverDCAPI.h` and call:

```c
const char *report = neverd_bytecode_recover_json_v1(
    code, code_size, request_json, request_size);
/* Parse report, including its ok field; null is an allocation failure. */
neverd_free_string(report);
```

No loaded native image or session is required. Both input pointers are borrowed
only for this synchronous call. The JSON length is explicit and does not include
a terminating NUL. Each input is limited to 64 MiB. The strict request is:

```json
{
  "schemaVersion": 1,
  "profile": {"version": 1, "register_bytes": 8, "byte_order": "little", "encodings": []},
  "functions": [{"entry": 0, "end": 4, "name": "example"}],
  "bindings": [],
  "base": 0,
  "output": "highc",
  "optimize": false,
  "unaligned_pointers": false,
  "with_context": false
}
```

Replace the empty `encodings` placeholder with a nonempty profile such as the
example above. Only `schemaVersion`, `profile` and `functions` are mandatory.
`output` is `check`, `highc` (default) or `llvmc`; optimization requires `llvmc`.
`with_context` is a boolean, default false, selecting the source contract above.
Unknown fields and wrong field types fail. Function declarations, bindings and
whole-request instruction/operation budgets are validated by the same engine
used by the CLI, including binding declarations supplied in check mode.

Every non-null response is owned, including errors. Success contains
`schemaVersion: 1`, `ok: true`, `functions`, `blocks`, `decoded_instructions`,
`decoded_bytes`, `input_bytes`, `scope`, `with_context`, `source`,
`mapped_observations`, and `unmapped_observations`. The boolean
`with_context` reports the selected ABI even in check mode, without claiming
source conversion or execution was verified. Scope is `cfg` for check
mode, whose source is empty, or `state-c` for source emission. Failure contains
`ok: false` and `error`, without partial source. Counts cover reachable bytes;
unvisited input is still unclassified.

Python plugins can use the typed wrapper:

```python
from neverd_plugin import recover_bytecode

result = recover_bytecode(
    b"\xb4\x34\x12\xe7", profile,
    [{"entry": 0, "end": 4, "name": "example"}],
    output="llvmc", optimize=True,
)
print(result.source)
```

Here `profile` is the complete encoding example above. The wrapper owns and
releases native responses and raises `NeverDError` for recovery failures. For
an explicitly loaded library outside the plugin host, pass `api=HostAPI(library)`.
Both Python recovery functions accept `with_context=True`; their result records
the selected ABI. They do not accept or retain the future runtime context. An
older engine can still serve the default unary ABI, but must explicitly confirm
the context option before the wrapper returns a result requesting that ABI.
See the runnable [C plugin](../plugins/bytecode/bytecode_plugin.c) and
[Python plugin](../pluginsdk/python/examples/external_bytecode.py).

### Per-instruction decoder callbacks

When selecting an encoding needs computation or external context, C and Python
plugins can supply a synchronous decoder instead of a static encoding table.
The request uses `layout` in place of `profile`:

```json
{"schemaVersion":1,
 "layout":{"version":1,"register_bytes":8,"byte_order":"little"},
 "functions":[{"entry":0,"end":1,"name":"example"}],
 "output":"highc"}
```

Layout accepts `version`, `register_bytes`, `byte_order` and optional
`temporary_bytes`; `encodings` is forbidden. All other request, result, call
binding and state-ABI contracts remain the same. The CLI continues to accept
static profiles; dynamic callbacks are available through the library APIs.

```c
static void decode(void *context, const unsigned char *bytes, size_t size,
                   uint64_t pc, ND_BytecodeInstructionSinkV1 reply,
                   void *reply_context) {
  const unsigned key = *(const unsigned *)context;
  const char *json = size && (bytes[0] ^ (unsigned char)(pc + key)) == 0xe7
      ? "{\"size\":1,\"operations\":[{\"op\":\"RETURN\",\"inputs\":[]}]}"
      : "{\"error\":\"unknown instruction\"}";
  reply(reply_context, json, strlen(json));
}

/* code and request must describe this example language and its function range. */
const char *report = neverd_bytecode_recover_decoder_json_v1(
    code, code_size, request_json, request_size, decode, &key);
/* Inspect report's ok/error fields before using its source. */
neverd_free_string(report);
```

Include `<string.h>` for `strlen`. The callback receives the logical PC and
up to 4,096 borrowed bytes from that PC, truncated at the declared function end.
The window bound avoids copying a whole function for each instruction in Python.
It replies exactly
once, before returning, with `{size, operations}` using the existing operand
grammar and **no `match` field**, or with `{error: "diagnostic"}`. The sink
copies the explicit-length JSON before returning; NUL termination is unnecessary.
It returns 1 for accepted bytes and 0 for invalid, repeated or oversized replies.
Acceptance means copied, not semantically validated. A callback reply is limited
to 64 MiB, an instruction to 4,096 bytes and 4,096 operations. Missing replies,
any rejected reply, invalid JSON and unsupported semantics fail the recovery
without partial source. The sink, its context and the input pointer must not
escape the invocation. No exception may cross the C callback boundary.

The decoder and its context are borrowed for the recovery call, invoked on the
calling thread and never retained afterward. Separate calls have independent
reply state and can be nested. A caller sharing its own mutable plugin context
across threads is responsible for synchronizing that context. Results must
depend only on bytes, PC and stable caller context. CFG discovery order is not
execution order: rolling keys or other path-dependent decoding state must first
be resolved by the producer. The callback does not add a path-state interpreter.
Instruction/operation budgets bound recovered graphs; they cannot preempt
trusted plugin code inside a callback.

The Python wrapper copies the byte window and handles reply/result ownership:

```python
from neverd_plugin import recover_bytecode_with_decoder

key = 37

def decode(data: bytes, pc: int) -> dict:
    opcode = data[0] ^ ((pc + key) & 255)
    if opcode == 0xb4 and len(data) >= 3:
        return {"size": 3, "operations": [{
            "op": "COPY", "output": {"space": "reg", "size": 2, "value": {}},
            "inputs": [{"space": "const", "size": 2,
                        "value": {"addend": int.from_bytes(data[1:3], "little")}}]
        }]}
    if opcode == 0xe7:
        return {"size": 1, "operations": [{"op": "RETURN", "inputs": []}]}
    raise ValueError(f"unknown or truncated instruction at {pc:#x}")

result = recover_bytecode_with_decoder(
    bytes([0xb4 ^ key, 0x34, 0x12, 0xe7 ^ (3 + key)]),
    {"version": 1, "register_bytes": 8, "byte_order": "little"},
    [{"entry": 0, "end": 4, "name": "example"}], decode,
    output="llvmc", optimize=True,
)
```

The wrapper catches Python exceptions inside the trampoline, reports a decode
failure, releases the native response, then re-raises the original exception.
It never invokes that decoder again after an exception. Returning an explicit
error mapping instead produces `NeverDError`. Non-mappings, nonfinite JSON and
oversized replies fail. A closure may carry stable private context; neither the
callable nor the closure is registered globally or retained by recovery.

These APIs do not infer indirect targets, recover an original source ABI, or
implement unknown host services. A profile or callback is a supplied semantic
specification, not proof that it matches an arbitrary interpreter.
The current source route retains the CLI's AArch64 state carrier and floating
conversion policy. Engine-specific policies must be modeled explicitly in the
supplied operations. Input execution and host-service modeling are separate
from source recovery; native and Python plugins remain trusted host code.

The C++ entry points are `readBytecodeProfile`, `BytecodeDecoder::create`,
`readBytecodeLayout`, `readBytecodeEncoding`, `BytecodeDecoder::createExternal`,
`BytecodeDecoder::decode`, `BytecodeDecoder::function` and
`lowerBytecodeState`. The tool demonstrates source ABI binding, call recovery,
external LLVM declarations and both C routes. Default resource bounds include
64 MiB per input file, 65,536 encodings, 4,096 bytes per instruction, 65,536 bytes
per state/temporary bank, and per-function instruction/operation budgets.

Profiles are semantic specifications. Successful decoding, C compilation and
even execution on selected inputs do not prove equivalence to a separate native
interpreter. Native instruction boundaries and rewrite authorization are never
created by this path.

## Bounded symbolic observations

`neverd-bytecode-observe` runs decoded LowIR through the shared `SymExec` engine
and uses `BitVectorSolver` for branch feasibility. It contains no built-in
instruction dialect, service registry, library inventory, or string table:

```sh
neverd-bytecode-observe code.bin profile.json functions.json inputs.json observations.json
neverd-bytecode-observe --annotate-c source.c observations.json annotated.c
```

The legacy `--annotate-c` mode adds function comments to ordinary `uint64_t` state-function
C output. It leaves function bodies unchanged. Reported calls and strings are
observations under the supplied contracts, not a reconstruction of every path.
Comments name abstracted calls; consult JSON for budgets, path constraints and
stop reasons. Time or path exhaustion does not prove a function is understood.

To associate evidence with the actual emitted calls and stored resolver
results, pass a fresh report back into recovery:

```sh
neverd-bytecode code.bin --profile profile.json --functions functions.json \
  --bindings bindings.json --llvm --optimize --observations observations.json \
  -o observed.c
```

The CLI and recovery JSON request accept the optional `observations` report.
Its input and canonical profile SHA-256 must match. Each function/address/target
is checked against the current call IR; stores additionally carry the original
instruction operation index. The shared source recorder follows those exact
occurrences through optimization into HighC or LLVMC. Deleted or ambiguous
occurrences stay unmapped, with counts in the response and output header. It
never matches generated variable names or attaches a missing call to a whole
function. `--check` and callback decoders reject this option.

Both routes print conditional API names, observed arguments and strings at the
corresponding expressions/statements. LLVMC also names call-status and stored
pointer values, for example `observed_dlsym_example_status` and
`observed_example_value`. These names preserve the state ABI: the former is
the gateway status, not the resolver's returned pointer. No observed string
becomes an unconditional literal operand, no external gateway is replaced by
a guessed native prototype, and no pointer is replaced with a host address.
Removing comments and changing local identifiers back leaves the executable
operations intact. This is a symbol-aware source view, not original-source
reconstruction or a proof that every path has the observed target.

An explicit resolver contract can set `result_identity` to `"symbol"` or
`"module"`. It requires `continuation: "register_effects"`,
`memory_effect: "none"`, an observed `string`, and a fresh 4- or 8-byte result.
The result can use `result_register` directly (native register ABIs) or
`result_offset_register` (state gateways); they are mutually exclusive.
`result_value` cannot be combined with identity. The symbolic value remains
unknown and nullable. Complete register/memory forwarding retains its identity;
arithmetic, partial overwrites, unknown aliases and havoc do not confer it on
other values. `callee_register` and optional `callee_bytes` observe a later
indirect target. Optional `module_register` (or `module_offset_register` for an
indirect register offset) records a previously resolved
module handle. Reports include `result_identity`, `resolved_target`, and
`symbol_flows`, separately from the program's actual numeric values.

The underlying identity and source-evidence models contain no bytecode dialect,
architecture, resolver name, OS service table, or sample string. Native PE/COFF
producers can use the same model for runtime symbol lookup. Existing
loader-authenticated imports retain their separate native ABI machinery;
conditional runtime observations do not acquire that authority. Windows/ELF
source tests exercise the shared projection, not execution of a Windows process.

The input object accepts `registers` (`offset`, `bytes`, `value`), `memory`
(`address`, `path`), optional selected `entries`, and bounded `max_steps`,
`max_paths`, `max_visits`, `max_milliseconds`. Untouched state remains symbolic. `calls` identifies
external targets and describes how to observe them:

```json
{
  "registers": [{"offset": 0, "bytes": 8, "value": 4096}],
  "memory": [{"address": 4096, "path": "initial-memory.bin"}],
  "calls": [{
    "target": 65536,
    "name": "host_gateway",
    "selector_register": 8,
    "selector_bytes": 4,
    "selector_names": {"0x12": "example_query"},
    "argument_count_register": 16,
    "argument_count_bytes": 4,
    "argument_registers": [24, 32]
  }]
}
```

An external call stops by default. `continuation: "havoc"` uses the shared
engine's conservative memory invalidation. `preserve_registers` may explicitly
retain byte ranges; `result_offset_register` and `result_width_register` identify
an indirect state result, replaced with a fresh symbolic value. Optional
`register_writes` apply explicit constant state updates after the call.

`continuation: "register_effects"` requires `memory_effect: "none"`: a complete
caller-supplied assertion that existing bytecode-visible memory is unchanged.
It preserves the state except for the declared result and constant writes. The
result is fresh unless `result_value` supplies an explicit scenario value;
`result_bytes` defaults to eight, or `result_width_register` selects its width.
Do not apply this contract to arbitrary APIs, allocation failures, callbacks or
writes without the corresponding evidence. `selector_cases` can select a full
call descriptor by the known hexadecimal selector. `dispatch_register` follows
a declared bytecode function through an explicit indirect gateway; unresolved
targets stop.

An optional `string` descriptor names `pointer_register` (or
`pointer_offset_register`), and optional `mode_register`, `encoded_mode`,
`key_register`. This describes an input protocol's byte-XOR string argument;
it is not a search over guessed decryption keys. Only bounded, concrete,
NUL-terminated ASCII values are reported. Ordinary argument pointers and
register writes are also inspected for concrete strings. When live memory is
unknown, an address into the original seed may still be reported separately as
an `initial_image_reference` or `initial_string_reference`; this is a static
reference and does not assert unchanged runtime contents. Unknown memory is
never filled from a dictionary or another decompiler.

The driver bounds paths, steps, repeated PCs, solver gates/watch visits and
expression nodes, and checks a per-function time budget between instructions; large
expression displays retain a symbolic reference instead of expanding their
printed tree. Callers running untrusted or large workloads should also impose
an outer wall-clock limit per function. Solver `unknown` retains a possible
path and records that its constraints were not proved satisfiable.

With CPU emulation enabled, `neverd-process-probe` is a thin command-line entry
to the existing process SDK:

```sh
neverd-process-probe observation.so android-aarch64-api28-v1 options.json runtime.json
```

See [Android native emulation](android-native-emulation.md) for explicit guest
memory, library scopes and supported services. A shared object compiled from
emitted bytecode C still needs an input-ABI adapter for its external gateways.
Running it does not establish equivalence to an unavailable original runtime.

For a fully specified memory gateway, `memory_copy`, `memory_fill`, and
`memory_havoc` continuations describe exactly one bounded writable range.
`destination_register` and `size_register` select its pointer and byte count;
copy/fill also require `source_register` (a source pointer or the fill byte).
Copy snapshots source bytes before writing, so this model has memmove semantics;
use `require_disjoint: true` for memcpy to require proved non-overlapping
ranges before continuing. Symbolic addresses may qualify when the shared
bitvector solver proves non-overlap under the path constraints, including
offsets from a common unknown base. A satisfiable overlap or solver `unknown`
stops the path. Counts must be concrete and at most 4096 bytes;
unresolved ranges stop. The shared
`SymExec` call-effect interface applies writes and conservative alias handling.
The state bank is preserved except for the declared result and register writes.
`return_destination: true` selects the destination as the result; otherwise it
remains symbolic. These are explicit complete call contracts, not automatic
recognition of external implementations or proof of pointer validity.
