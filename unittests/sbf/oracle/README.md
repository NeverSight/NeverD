# Official Anza sbpf process oracle

This helper is the process boundary for NeverD's upstream SBF release gate. It
is compiled inside a detached worktree of the exact `SBPFMain` object ID in
`SBFUpstreamSources.def`, using that checkout's `Cargo.lock`. The helper links
Anza's loader, `RequisiteVerifier`, and interpreter; it never links NeverD.

Build it with:

```sh
python3 scripts/build_sbf_official_oracle.py \
  --sbpf-root /path/to/anza-sbpf \
  --output /absolute/path/to/neverd-sbpf-official-oracle
```

Then run the complete gate with both inputs explicit:

```sh
NEVERD_SBPF_ORACLE=/absolute/path/to/neverd-sbpf-official-oracle \
NEVERD_SBPF_ROOT=/path/to/anza-sbpf \
ctest --test-dir build -L NeverDSBFExternalOracleTests --output-on-failure
```

Only the pinned-corpus test needs `NEVERD_SBPF_ROOT`; generated ELF, verifier,
and execution tests depend solely on `NEVERD_SBPF_ORACLE`. Without an input, a
test skips only when that input is relevant to it. Setting
`NEVERD_REQUIRE_SBPF_ORACLE=1` turns any missing required executable or corpus
into a test failure. The binary's `--version` record embeds the source object
ID, and the C++ test refuses an oracle built from any other revision.

The upstream CLI is not used as the protocol. It represents verifier rejection
with a panic exit and does not register the `log` syscall required by the
published corpus. The dedicated helper reports semantic rejection separately
from infrastructure failure and supports both ELF probes and raw text execution
from a byte file or hex string. Raw execution records include the normalized
status, result, and official instruction count.

The verifier release gate uses one `verify-batch` process for all 1,411 probes:
all 256 opcode bytes under each of v0 through v4 (1,280 records), plus 131
table-driven boundary records for LDDW continuations, both register nibbles,
division, shifts, endian widths, branch bounds, and each version's CALLX field.
The helper calls the pinned crate's `RequisiteVerifier` directly and emits one
indexed acceptance record per input. NeverD compares those records both with
its independently audited opcode manifest and with strict analysis of the same
bytes. The C++ version array and Rust parser are both generated from
`SBFVersions.def`; a new concrete row therefore expands the matrix, or fails
the pinned Rust build if upstream has no matching enum variant. Protocol
tokens are generated for Rust from
`SBFOfficialOracleProtocol.def`, so the process boundary has one string/status
authority shared with the C++ consumer.

The strict-v3 ELF compatibility gate similarly sends all 41 deterministic
fixtures through one `verify-elf-batch` process. Its table-driven matrix starts
from one accepted sectionless ELF and covers identification and ELF header
fields, program-header table bounds, segment types/flags/file and memory
ranges, entry-point bounds and alignment, and table/segment overlap. It also
records the two upstream compatibility choices that `e_type` and `p_align` are
ignored. Every response is keyed by its request index. A nonzero helper exit,
unexpected output, an out-of-range or duplicate index, an unknown status, or a
missing record is an infrastructure failure rather than a semantic rejection.

The upstream corpus currently exercises ELF v0 and v3. Three deterministic
minimal ELF fixtures independently add accepted v1, v2, and v4 header/loader
paths, each probed by the official process and NeverD before the verifier-only
matrix runs.

The legacy-ELF batch adds seven deterministic v0 cases for non-allocatable
`.text` and `.rodata`, unknown allocatable `SHT_PROGBITS` and `SHT_NOBITS`
sections, already-based rodata, and the upstream ordering that processes a
malformed dynamic relocation table before rejecting a misaligned entry point.

At the audited revision, the checkout's unqualified `stable` toolchain was
Rust 1.91.1 locally and failed to compile `Layout::dangling_ptr`. Rust 1.97.1
builds the unchanged checkout successfully. The `OfficialOracleRust` row in
`SBFUpstreamSources.def` is the build script's toolchain authority; no
compatibility patch or semantic shim is applied.
