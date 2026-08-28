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

## Budgets, output, and bindings

Hunt exploration and the solver are bounded (`--max-paths`, `--max-steps`,
`--max-loop`, `--solver-conflicts`); budget exhaustion yields UNKNOWN. Both
commands print JSON and honour `-o`. The exit code is `0` for SAFE, `2` for
UNSAFE, and `1` for UNKNOWN or an error.

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

The replay fields are additive. The top-level `schema_version` remains `1`, so
schema-v1 consumers must continue to ignore fields they do not understand.

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
- **P2**: patch-inserted runtime checks, hybrid fuzzing, and broader
  interprocedural attacker reachability.
