**Languages**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← Documentation Index](README.md)

# Memory-Safety Audit & Hunt

NeverD analyses a loaded binary for two families of memory-safety defect and
reports them as structured JSON. Both run on the format-neutral lifted IR, so
**PE/COFF, ELF, and Mach-O are first-class, co-equal targets** — a finding is
never gated behind one format's scanner or import table.

| Track | Command | Reports |
|-------|---------|---------|
| **Audit** | `neverd audit <binary>` | Heap-object lifetime defects: leak, double free, use after free |
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
3. `pdb` / `dwarf` / `map` — a debug symbol, by loader kind
4. `export` / `symbol` — an image export or symbol-table entry
5. `sig` — a signature-database match
6. `synthetic` — a placeholder minted for an unnamed routine

A statically-linked `memcpy` named by DWARF reports `dwarf`; an imported
`memcpy` reports `import` on every format. A signature match never displaces a
name a debugger or import table already stated.

---

## Sink & source catalog

The catalog is a configurable table, not a hard-coded set. Each **sink** entry
declares its weakness class, its role (copy, format, alloc, free, realloc), and
the argument slots that matter (destination, source, length, capacity). Each
**source** entry names a provider of attacker-influenced input. The built-in
rows live in [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) and
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

Extend or override the catalog with a specification file:

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 }
  ],
  "sources": [
    { "name": "my_read", "out": 1, "return_tainted": true },
    { "name": "my_scan", "out": -1, "return_tainted": false }
  ]
}
```

`out` identifies a buffer written by the source. `return_tainted` separately
states whether the returned pointer or scalar carries attacker-controlled
input; when omitted, the legacy `out == -1` return-source convention applies.
This keeps status/count returns distinct from buffer content.

Destination-only input routines are not inferred to be unbounded merely
because they also appear in the source catalog. A `gets`-like custom routine
must opt in explicitly with `"unbounded": true`; contradictory source/length
fields are rejected transactionally.

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
  verdict is UNSAFE and the solver's model is reported as the concrete witness.
- Anything else — unknown length or unknown capacity — is UNKNOWN.

Every recovered capacity is an **upper bound** on the true object size, so a
proven overflow is never a false positive.

### Formatted input

For `scanf`/`fscanf` and their versioned spellings, a readable constant format
maps each non-suppressed conversion to its actual variadic output argument.
Unbounded `%s`/`%[` outputs taint later string uses; numeric and character
outputs taint values loaded from the written object, but not the output-pointer
value itself. `sscanf` propagates those effects only when its input string is
already attacker-influenced. Suppressed conversions, excess arguments,
position-dependent or unsupported formats, bounded text fields, and `%n`
remain UNKNOWN instead of being guessed.

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
commands print JSON and honour `-o`. The exit code is `0` for a clean run, `2`
when an unsafe finding is present, and `1` on error.

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "17 bytes" } }
}
```

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
- **P0** (this release, all three formats): the sink catalog, the argument
  prefilter, copy-overflow hunt, and the heap-lifetime audit. Mandatory
  checked-in fixtures cover PE, ELF, and Mach-O on both x86-64 and AArch64 on
  every test host.
- **P1**: local-stack uninitialised-read and format-string checks are available;
  stack/global overflow, richer PDB stack types, and more platform allocators
  remain incremental coverage areas.
- **P2**: patch-inserted runtime checks, interprocedural attacker-reachability.
