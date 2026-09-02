**Languages**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← Documentation Index](README.md)

# Memory-Safety Audit & Hunt

NeverD analyses a loaded binary for two families of memory-safety defect and
reports them as structured JSON. Both run on the format-neutral lifted IR, so
**PE/COFF, ELF, and Mach-O are first-class, co-equal targets** — a finding is
never gated behind one format's scanner or import table.

| Track | Command | Reports |
|-------|---------|---------|
| **Audit** | `neverd audit <binary>` | Heap-object lifetime defects and uninitialized local stack reads |
| **Hunt** | `neverd hunt <binary>` | Copy/formatting overflows and uncontrolled format strings |

The engine reuses NeverD's in-house symbolic execution and bitvector solver for
witnesses and reachability; there is no external solver, VM, or container
dependency.

---

## Core invariant: fail closed

An unlifted operation, an unsummarised call, a call whose arguments the ABI pass
could not recover, an unresolved indirect target, or an exhausted budget yields
**UNKNOWN**, never SAFE. A destination whose capacity cannot be recovered is
UNKNOWN. Strict lifting stays as-is; the safety layer only ever adds
conservative verdicts on top of it.

Call effects use closed-world semantics: a summary applies only when its
preconditions and all relevant effects are known. An unknown effect or a
summary that is only partially applicable keeps the result UNKNOWN; the
analysis never fills the gap with an assumed no-op or successful call.

---

## Identity contract per format

Both tracks require the lift pipeline (it recovers per-call arguments), and both
name every finding's callee through the same identity view the rest of NeverD
uses. Debug discovery precedence is unchanged:

| Format | Debug (in precedence order) | Import / thunk resolution |
|--------|------------------------------|---------------------------|
| **PE/COFF** | `--pdb`, debug-directory or sibling `.pdb`, then MSVC `/MAP` | IAT slot and `__imp_` thunks, ordinal imports |
| **ELF** | in-image DWARF, split `*.debug`, then GNU/LLD MAP | PLT stubs resolved to the imported name |
| **Mach-O** | in-image DWARF, adjacent `.dSYM`, then ld64 `-map` | dyld bind / indirect-symbol slots and stub helpers |

`--pdb` / `--map` name an authoritative companion file: failing to read it is an
error, not a silent fallback. `--no-debug` reads the image alone on every
format.

PDB procedure signatures are used to distinguish value-returning allocators
from `void` release functions. Rich PDB local/stack type recovery remains
limited; when it cannot establish an exact object size, Hunt falls through to
the frame/allocation model and reports UNKNOWN rather than inventing a size.

### Name-source precedence

Every finding carries a `name_source` describing where its callee name was
established, chosen by this precedence:

1. `rename` — a caller-supplied rename
2. `import` — an IAT (PE), PLT (ELF), or dyld-bind / stub (Mach-O) entry
3. `export` / `symbol` — an already-stated image export, symbol-table entry,
   or other non-placeholder image name
4. `pdb` / `dwarf` / `map` — a debug symbol that establishes a placeholder or
   agrees with the image's stated name, by loader kind
5. `sig` — a signature-database match
6. `synthetic` — a placeholder minted for an unnamed routine

A statically-linked `memcpy` named only by DWARF reports `dwarf`; an imported
`memcpy` reports `import` on every format. A companion never replaces a
different non-placeholder name already stated by the image, and a signature
match never displaces any stated identity.

---

## Sink & source catalog

The catalog is a configurable table, not a hard-coded set. Each **sink** entry
declares its weakness class, its role (copy, format, alloc, free, realloc), and
the argument slots that matter (destination, source, length, capacity). A JSON
copy or format sink also supplies an executable call effect. Each **source**
entry names a provider of attacker-influenced input. The built-in rows live in
[`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) and
[`SafetySources.def`](../include/neverd/safety/SafetySources.def): the C runtime
copy family (`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), their
fortified `_chk` variants, the allocation/release family
(`malloc`/`calloc`/`realloc`/`free`, operator `new`/`delete`), optional Win32
heap APIs, POSIX inputs (`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`,
program arguments), and Win32 inputs (`GetCommandLineA/W`, `ReadFile`,
`GetEnvironmentVariable*`).

Per-format spellings fold onto one entry: leading underscores are stripped
(`_malloc`, `___strcpy_chk`) and mangled operator new/delete are matched through
aliases.

When `effect` is omitted from a JSON copy or format sink, applicability is
derived from the highest referenced argument slot. A copy then requires that
exact arity; a format sink accepts calls from that minimum arity through the
variadic maximum. An optional `effect` object may explicitly set an accepted
arity range with `min_arity` and `max_arity` (or `"variadic"`), including extra
wrapper arguments beyond the inferred exact copy arity; `min_arity` must be at
least the highest referenced role slot plus one, while `formats` and `abis`
restrict applicability. If the call's
arity, object format, or ABI does not match, no summary applies and the
closed-world result remains UNKNOWN.

Extend or override the catalog with a specification file:

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 },
    { "name": "my_format", "kind": "format", "dst": 0, "fmt": 2,
      "effect": { "min_arity": 3, "max_arity": "variadic",
                  "formats": ["elf"], "abis": ["sysv"] } }
  ],
  "sources": [
    { "name": "my_read", "out": 1, "return_tainted": true }
  ]
}
```

For a custom source, `out` and `return_tainted` are discovery metadata only.
They do not establish executable memory, return-value, or taint effects. The
current source schema lacks the typed success, mutation, format, and ABI
contracts needed for those semantics, so analysis that depends on a custom
source effect remains UNKNOWN. Built-in sources are unaffected: their typed,
applicability-checked descriptors continue to provide executable effects.

An unbounded destination-only custom sink is not inferred from a matching
source entry. A `gets`-like custom sink must opt in explicitly with
`"unbounded": true`; adding the same name to the source catalog does not grant
an executable effect, and contradictory source/length fields are rejected
transactionally.

---

## Hunt: copy-overflow verdicts

For each copy sink the hunt resolves the destination capacity — a debug-declared
array size, a heap allocation site with a known size, a sized writable global
symbol, or a sound stack/section/segment upper bound — and classifies the
argument that decides the write length by a backward SSA walk (following
spill/reload through stack slots):

- **Constant length** within an exact capacity is SAFE. A constant overflow is
  UNSAFE only when the sink is reachable on a corroborated path; otherwise it
  remains UNKNOWN.
- **Fortified** `_chk` copies carry a runtime destination bound. Rejection, or
  a bound proven to fit the recovered object, is SAFE; a feasible write beyond
  the object is UNSAFE; an unrecovered or inconclusive bound is UNKNOWN.
- **Provably bounded** length (a length-returning call, a mask, a clamp) is
  retired before solving, recording why. It is SAFE only when the destination
  size is exact; against a containing-region upper bound it remains UNKNOWN.
- **Attacker-influenced** length with a known capacity is checked with the
  bitvector solver: if a length greater than the capacity is feasible the
  verdict is UNSAFE and the full solver model is reported. Candidate input
  values are replayable only when the whole witness maps to a complete
  `process-input-v1` plan. Initially that means exact literal environment
  values and, at most, the bytes returned by the first supported
  `read(0)`-family standard-input consumption. argv, file, network, custom, or
  ambiguous input remains
  non-replayable and carries the reason no complete plan could be formed.
- Anything else — unknown length or unknown capacity — is UNKNOWN.

Every recovered capacity is an **upper bound** on the true object size, so a
proven overflow is never a false positive.

### Formatted input

For `scanf`/`fscanf` and their versioned spellings, a readable constant format
maps each non-suppressed conversion to its actual variadic output argument.
Unbounded `%s`/`%[` outputs taint later string uses; numeric and character
outputs taint values loaded from the written object, but not the output-pointer
value itself. `sscanf` propagates those effects only when its input string is
already attacker-influenced. Bounded text outputs such as `%Ns`/`%N[` propagate
taint together with a `MaxBytes` extent that includes the terminator;
wide-character variants compute that byte extent using the platform's
`wchar_t` width. Suppressed conversions, excess arguments, position-dependent
or unsupported formats, and `%n` remain UNKNOWN instead of being guessed.

### Formatted output

Formatting calls keep their two independent bounds: `snprintf`/`vsnprintf`
carry a maximum write length, while fortified `_chk` variants also carry the
compiler-provided destination-object size. A trusted constant format containing
only literal bytes and `%%` has an exact output extent, including its terminating
NUL, which is checked against the same heap/stack destination model as copy
sinks. A bounded `snprintf` write is SAFE when its maximum write fits an exact
destination; a literal output larger than the destination is UNSAFE. Formats
with value conversions remain UNKNOWN until the converted argument extents are
proved. As with copy sinks, an overflow becomes high-confidence UNSAFE only
after LowIR path replay proves the formatting call reachable. An
attacker-controlled format string is reported separately as `format_string`,
regardless of destination truncation.

---

## Audit: heap-lifetime verdicts

For each allocation the audit tracks the handle across the control-flow graph,
including through stack spill/reload, and applies an escape summary
(returned, stored through a non-stack address, or handed to an opaque callee):

- **Leak** — the handle is neither released nor allowed to escape.
- **Double free** — a second release is reachable after a first on a path.
- **Use after free** — a dereference or modelled non-release use is reachable
  after a release.

Allocation and release **wrappers** are recognised through per-function escape
summaries, so a `malloc`/`free` forwarder does not hide the defect. Releases on
mutually-exclusive branches are not reported as a double free.

The heap state machine first emits a candidate event sequence (allocation,
release, use, or returned exit). A second pass must replay that sequence on a
symbolic LowIR path and prove its path predicate satisfiable before the finding
is HIGH-confidence UNSAFE. Missing LowIR, opaque operations, unsummarised calls,
solver uncertainty, and exploration limits downgrade the candidate to UNKNOWN.
Conservative may-alias memory havoc is tracked separately, so ordinary stack
frame stores do not invalidate otherwise exact reachability evidence.

---

## Audit: local stack initialization

The audit also follows full-width stores reaching loads from local frame slots
below the function-entry stack pointer. A load with no possible prior
initialization is reported as `uninitialized_read`; LowIR path replay must
confirm that the load is reachable before it becomes HIGH-confidence UNSAFE.
Conditional initialization, partial writes, escaped slot addresses, and other
uncertain definitions remain UNKNOWN. Caller-owned argument slots at or above
the entry stack pointer are excluded from this check.

---

## Known-entry interprocedural reachability

Every finding carries three independent statements. They answer different
questions and must not be collapsed into one another:

| Field | Question | Values |
|-------|----------|--------|
| `verdict` | What did the local safety analysis prove about the operation? | `SAFE`, `UNSAFE`, `UNKNOWN` |
| `reachability.status` | Is the containing function on a recovered control path from a known native entry? | `REACHABLE`, `UNREACHABLE`, `UNKNOWN` |
| `reachability.attacker_control` | What did the argument slice prove about attacker influence at this finding? | `TAINTED`, `BOUNDED`, `UNKNOWN` |

Reachability is additive evidence and does not rewrite a finding's `verdict`,
the report's aggregate `verdict`, or the CLI exit code. For example, a locally
proved overflow can remain `verdict=UNSAFE` while
`reachability.status=UNREACHABLE`; consumers that require an executable attack
path should test both the verdict and the reachability fields.

The interprocedural pass starts from recovered native roots. An `application`
root is a recognized application entry such as `main` or `WinMain`; an `image`
root is the entry recorded by the loaded image; and an `export` root is an
exported routine. If one function has more than one root identity, the
deterministic preference is `application`, then `image`, then `export`.
`reachability.entry` records that root's `va`, `name`, and `kind`. A reachable
non-root also carries a shortest deterministic `call_chain`; each exact internal
edge records `caller_va`, the call-site `call_va`, `callee_va`, and whether its
`kind` is `direct` or `indirect`.

`UNREACHABLE` is emitted only when at least one root exists, the internal call
inventory is complete, the call-depth budget was not exhausted, and no root can
reach the function. For a function not otherwise positively reached, missing
roots, duplicate or ambiguous function identities, inconsistent CFG/call
inventories, unresolved executable internal targets, and call-depth exhaustion
prevent an `UNREACHABLE` proof and produce `reachability.status=UNKNOWN`, with
`reason` and `budget_hit` where applicable. The attacker-control fixed point
likewise does not invent propagation across an unknown ABI, mismatched argument
width, variadic-only slot, incomplete slice, depth boundary, or exhausted
summary budget; any flow not already proved stays `UNKNOWN`, while facts proved
before a later budget boundary remain valid.

The report-level counters count findings, not functions or distinct paths.
`control_reachable` counts findings with `status=REACHABLE`;
`attacker_reachable` is the subset that also has
`attacker_control=TAINTED`. `reachability_unknown` and `unreachable` count the
other two control statuses. These counters are separate from `safe`, `unsafe`,
and `unknown`, which tally verdicts.

---

## Budgets, output, and bindings

Hunt exploration and the solver are bounded (`--max-paths`, `--max-steps`,
`--max-loop`, `--solver-conflicts`). Interprocedural work has two independent
limits: `max_call_depth` is the maximum number of internal call edges in a
known-entry witness, while `max_summary_iterations` bounds attacker-control
fixed-point rounds. Their defaults are 64 edges and the effective call-depth
limit plus one round, respectively. Budget exhaustion fails closed as described
above. Exhausting `max_call_depth` can leave a not-yet-reached function's
`status=UNKNOWN`; exhausting `max_summary_iterations` does not erase a
structural witness, so `status=REACHABLE` can coexist with
`attacker_control=UNKNOWN` and `budget_hit=true`. Both commands print JSON and
honour `-o`. The exit code is `0` for SAFE, `2` for UNSAFE, and `1` for UNKNOWN
or an error.

The same two limits are exposed at every public entry point; zero selects the
engine default:

| Surface | Control-depth option | Attacker-summary option |
|---------|----------------------|-------------------------|
| CLI (`audit` and `hunt`) | `--max-call-depth <n>` | `--max-summary-iterations <n>` |
| C (`neverd_safety_options`) | `max_call_depth` | `max_summary_iterations` |
| Python (`Session.audit()` / `Session.hunt()`) | `max_call_depth=<n>` | `max_summary_iterations=<n>` |

For C callers, zero-initialize `neverd_safety_options`, set `struct_size` to
`sizeof(neverd_safety_options)`, and then set either appended field as needed;
older structure sizes retain the defaults. Python validates both values as
unsigned 32-bit integers before calling the C API.

The same analyses are available through the C API
(`neverd_session_audit_json` / `neverd_session_hunt_json` with a versioned
`neverd_safety_options`) and the Python SDK (`Session.audit()` /
`Session.hunt()`).

### Finding schema

```json
{
  "class": "buffer_overflow",
  "function": "parse_header",
  "name": "strcpy",
  "name_source": "import",
  "call_va": "0x11a4",
  "source": "reader.c:42",
  "sink": "strcpy",
  "arg_index": 1,
  "flow": "TAINTED",
  "verdict": "UNSAFE",
  "confidence": "HIGH",
  "capacity": 16,
  "capacity_kind": "exact",
  "corroboration": "path predicate and overflow are jointly satisfiable",
  "reachability": {
    "status": "REACHABLE",
    "attacker_control": "TAINTED",
    "budget_hit": false,
    "entry": { "va": "0x1000", "name": "main", "kind": "application" },
    "call_chain": [
      { "caller_va": "0x1000", "call_va": "0x1080",
        "callee_va": "0x1100", "kind": "direct" }
    ]
  },
  "evidence": {
    "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" },
    "candidate_values": [
      { "name": "copy_length", "value": "17" },
      { "name": "argv[1]", "value": "16 bytes" }
    ],
    "replayable": false,
    "replay": {
      "adapter": "process-input-v1",
      "reason": "argv input is not supported by process-input-v1"
    },
    "symbolic_model": [
      { "id": 0, "name": "copy_len", "width": 64,
        "value_hex": "0x11", "origin": "input" }
    ]
  }
}
```

`replayable` is derived evidence, not an independent promise: it is `true` if
and only if `replay` contains a complete input plan for the
`process-input-v1` adapter. The plan records exact environment bytes, the first
supported `read(0)`-family standard-input byte sequence when used, and bindings
from solver assignment IDs to those inputs. Unsupported argv, file, network,
custom, and ambiguous origins
leave `replayable=false`; `replay.reason` explains why no exact plan exists.
Candidate values remain useful symbolic evidence but are not a runnable
reproducer.

The replay and reachability fields are additive. The top-level `schema_version`
remains `1`, so schema-v1 consumers must continue to ignore fields they do not
understand.

---

## Strict runtime guards and authenticated publication

`binary-sanitizer-v1` is a separate experimental mutation transaction; it does
not change an audit or hunt verdict. A completed strict hunt must map every
finding to one exact counted-write occurrence with exact remaining capacity and
matching compiler-emitted call-site metadata. Any unsupported finding, stale or
ambiguous identity, incomplete lift, exhausted budget, guard-generation error,
or target/signature gate rejects the whole transaction. There is no partial
success. Relocatable objects, dynamic libraries, universal Mach-O images, and
guarded Mach-O bundle members are among the inputs that fail closed.

The public entry points are the C `neverd_session_sanitize` transaction, the
`neverd patch --sanitize=strict` CLI mode, and Python `Session.sanitize`. C
callers that require authenticated publication must first probe
`neverd_sanitize_publication_abi_version()` before invoking the mutation entry
point. All three surfaces accept success only after publication receipt v1 is
complete and internally coherent.

On non-Darwin hosts the entry point authenticates and normalizes its input but
then returns `UNSUPPORTED_TARGET` before lifting, guard generation, candidate
creation, or namespace mutation. On Darwin there are only two successful
namespace dispositions:

- `CREATE_EXCLUSIVE` publishes a newly created same-directory candidate to an
  absent destination with one atomic no-replace operation. Its receipt states
  namespace atomicity, destination create-exclusivity, and the actual operand
  binding, but does not claim replacement compare-and-swap or crash durability.
- `NO_CHANGE` is read-only. It is available when an empty guard plan names the
  held loaded-source object, authenticates that object again, reports
  `NOT_PUBLISHED`, and claims neither publication guarantees nor an operand
  binding. A guarded plan cannot use the loaded source as its output.

A distinct existing destination is never replaced: publication v1 has no
replacement CAS, so callers must choose a new path. An indeterminate publish or
a destination created without a complete final receipt is a failed transaction,
not degraded success; the destination may exist and must be inspected before
use, retry, or deletion.

The trust boundary is deliberately narrower than the pathname wording. Source
bytes are matched to the session's external digest and then observed through a
held descriptor, while the source and destination-directory objects remain
anchored for the transaction. Source metadata and stable object identity are
transaction-time observations, not proof that they were unchanged since the
session originally loaded the image. The held destination-directory object may
be renamed after it is opened: a complete receipt authenticates publication in
that held object, but does not prove that the original destination pathname
continued to name it during or after the transaction. The public receipt is an
attestation summary, not a self-contained durable path binding; a caller that
later reopens a pathname must retain an external anchor and authenticate the
reopened object again. Darwin's rename primitive names a source
path rather than a held candidate descriptor, so create-exclusive publication
also requires a destination directory owned by the effective uid, without an
extended ACL or group/other write access, on a volume that attests normal POSIX
ownership. Those facts are reauthenticated before candidate creation and
publish. The operand-binding claim excludes an equally privileged process,
root/DAC-bypass authority, and an adversarial filesystem or server; it assumes
the kernel, VFS, and mounted filesystem honor their reported semantics.

---

## Native process replay: phase 0 only

The platform-neutral `process-replay-v1` plan and executor coordinator define
what a future authenticated run would have to prove, but no current host can
perform that native execution. `NativeProcessReplayAdapter` phase 0 is a C++
availability/factory boundary, not a C, Python, CLI, or JSON execution surface.

Its availability query is mutation-free: it validates the complete plan,
execution limits, absolute executable locator, and unique physical call-site
map without opening a target, invoking a backend callback, or launching a
process. The locator is only a future object locator and must never become a
post-authentication pathname execution authority. Availability is
all-or-nothing across the complete executor capability record. If any required
confinement, identity, input-interposition, occurrence-attestation, limit, or
cleanup guarantee is unavailable, `Available` remains false, every capability
remains false, and the factory returns an error without an operations table.

That is the result on every host today. Linux reports an incomplete backend
until trusted static-ELF instrumentation, a persistent supervisor, complete
containment, limits, and runtime attestation exist. macOS is unsupported because
its supported public primitives cannot supply held-object execution, an
arbitrary-target sandbox installed before target code, and race-free
process-tree containment. Other platforms are unsupported. Unit-test operation tables
exercise the coordinator contract but are not evidence of native availability.

---

## False-positive bounds & scope

- Capacity is always exact or an upper bound, so an UNSAFE verdict reflects a
  real overflow. A too-small buffer whose exact declared size is unavailable is
  UNKNOWN when a containing-region bound is insufficient to prove safety.
- A length-bounded copy is retired before solving and counted in `skipped`;
  exact capacity can establish SAFE, while an upper-bound-only capacity remains
  UNKNOWN.
- Catalogued wide-character and append copies remain UNKNOWN until element
  width and the existing destination extent are recovered. Out-parameter
  allocators and conditional `realloc` ownership also remain UNKNOWN when the
  handle transition cannot be proved.
- **Phase 1** (this release, all three formats): the sink catalog, argument
  prefilter, stack/global copy-overflow hunt, heap-lifetime audit, local-stack
  uninitialised-read checks, and format-string checks. Mandatory checked-in
  fixtures cover PE, ELF, and Mach-O on both x86-64 and AArch64 on every test
  host.
- Richer PDB stack types and additional platform allocators remain incremental
  coverage areas; absence of an exact summary stays UNKNOWN.
- The current known-entry slice covers structural interprocedural reachability
  and monotone attacker-parameter propagation. The separate experimental
  `lowir-concolic-v1` adapter now provides replay-verified, register-seeded
  branch flips on the mandatory native format/architecture matrix; it remains
  non-exhaustive and does not change safety verdicts. The separate experimental
  `binary-sanitizer-v1` now supplies strict counted-write guards and
  authenticated publication on Darwin within the limits above. Broader native
  `process-replay-v1` execution remains unavailable behind its fail-closed
  phase-0 adapter, including input kinds and repeated consumptions beyond
  today's `process-input-v1` evidence.
