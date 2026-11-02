# Reproducible function-matching diagnostics

[`scripts/benchmark_binary_diff.py`](../scripts/benchmark_binary_diff.py) builds
owned x64 ELF inputs and scores retained function associations against authored
identities. The three development fixtures cover stripped function reordering,
the same reordering with a changed bound, and duplicate local symbols in reversed
object order. They are small regression diagnostics. They do not establish
held-out quality, competitive speed or statistical superiority.

## Generate and run

Provide explicit compiler, ELF linker and strip executables. The generator
records their versions and hashes, all commands, original C, object files,
unstripped binaries and linker maps. The compiler targets x86-64 Linux at O2;
the binaries have no C runtime and are analyzed, never executed.

```bash
python3 scripts/benchmark_binary_diff.py generate \
  --clang /path/to/clang --linker /path/to/ld.lld --strip /path/to/llvm-strip \
  --output-dir benchmark-results/diff-corpus
python3 scripts/benchmark_binary_diff.py run \
  --corpus benchmark-results/diff-corpus/manifest.json \
  --neverd /path/to/frozen/bin/neverd \
  --library /path/to/frozen/bin/libneverd.so \
  --output-dir benchmark-results/diff-neverd
```

The runner currently requires Linux. It starts a fresh CLI for each case, selects
the explicit shared-library directory, records CLI/library hashes and version,
and retains stdout, stderr, exit status, report and evaluation. Only input ELF
paths are passed to the matcher. The separate `truth` directories and corpus
manifest are evaluator inputs. Freeze the executable and library throughout the
run. CLI exits 0, 2 or 3 are admitted only when the saved native comparison says
`state: complete` and the process has no timeout/cancellation/error. Exit 3 may
report incomplete native bodies despite completed function matching. Native
body coverage and discovery completeness remain separate evaluation fields;
neither is promoted to complete by a matching score. Output directories and result
files must be new, so a rerun cannot overwrite earlier evidence.

On Linux with `/usr/bin/time`, each stage retains user/system CPU seconds,
elapsed seconds and `gnu-time-child-maxrss-v1` maximum RSS in KiB, plus the
measurement executable's hash. RSS is the largest waited-for child's resident
high-water mark, not the simultaneous sum of a process tree. Without that tool,
resources remain null. Timeouts and cancellation retain their failed stages.
These are end-to-end diagnostic observations on the current host. Shared builds
or other load prevent competitive latency interpretation; do not compare them
with another tool's matching-only time.

## Run IDA, BinExport and BinDiff

[`scripts/benchmark_binary_diff_bindiff.py`](../scripts/benchmark_binary_diff_bindiff.py)
automates the Linux end-to-end track using existing, licensed IDA and explicitly
selected BinExport/BinDiff installations. It does not install products or change
their license configuration. Supply the complete BinDiff configuration file:

```bash
python3 scripts/benchmark_binary_diff_bindiff.py \
  --corpus benchmark-results/diff-corpus/manifest.json \
  --ida /path/to/idat --binexport-plugin /path/to/binexport_ida.so \
  --bindiff /path/to/bindiff --config /path/to/bindiff-config.json \
  --output-dir benchmark-results/diff-bindiff
```

Each case starts two fresh IDA databases, waits for automatic analysis, exports
their graphs and invokes BinDiff. The exporter binds its graph bytes to IDA's
native input SHA-256 and version. Admission also checks BinDiff's own ordered
input identities. A zero process exit alone cannot certify a completed export.
The runner retains the databases, graph exports, logs, commands, configuration,
copies of the selected binaries and scripts, hashes, stage outcomes and evaluation.
Later source edits do not invalidate those retained copies. Timeout or a failed
stage remains an incomplete run; the timeout terminates only that stage's owned
process group. Ctrl-C also stops that group and records cancellation before
exiting with code 130. The default per-stage deadline is 600 seconds and `--timeout`
overrides it. Existing output directories are refused.

Exit 0 means every case had an exact mapping, 1 means all pipelines completed
but at least one mapping differed, and 2 means invalid input or an incomplete
pipeline. A completed competitor run with wrong matches is still useful
evidence and must not be retried until its score improves. Stage times are
diagnostics, not a matching-only or peak-memory benchmark. Pin the surrounding
product installations and dependencies separately for release qualification;
the runner's hashes do not inventory every shared library or installed plugin.

## Run Ghidra Auto Version Tracking

[`scripts/benchmark_binary_diff_ghidra.py`](../scripts/benchmark_binary_diff_ghidra.py)
creates a new local project for every case and imports both inputs under distinct
names, including when their original basenames are identical:

```bash
python3 scripts/benchmark_binary_diff_ghidra.py \
  --corpus benchmark-results/diff-corpus/manifest.json \
  --ghidra /path/to/ghidra --java-home /path/to/jdk-21 \
  --output-dir benchmark-results/diff-ghidra
```

The runner checks an input-bound completion receipt after each import. A native
analysis timeout is rejected even when headless processing exits successfully.
It then invokes Ghidra's Auto Version Tracking task, observes its completion
status, saves the session and exports its associations. A partial session or a
successful export alone cannot complete the pipeline. The task's status retains
any markup-application warnings; scoring covers accepted function associations.
The qualified profile matches the bundled Ghidra 12.1.3 headless script defaults.
Its three overrides of task defaults are explicit in
[`RunNeverDVersionTracking.java`](../tools/neverd-bench/RunNeverDVersionTracking.java).
Other Ghidra releases need their own runtime/profile qualification.

`--cpus` defaults to 2, `--analysis-timeout` to 300 seconds per input, and
`--timeout` to 600 seconds per process. The retained project, copied input bytes,
analysis/task receipts, scripts, selected runtime files and native SHA-256
identities support inspection and independent re-evaluation. Installed Ghidra
extensions, preferences and the surrounding JDK remain part of the separately
pinned environment.

## Run IDA and Diaphora

[`scripts/benchmark_binary_diff_diaphora.py`](../scripts/benchmark_binary_diff_diaphora.py)
starts fresh IDA databases and invokes the selected Diaphora exporter and matcher
APIs with explicit completion checks:

```bash
python3 scripts/benchmark_binary_diff_diaphora.py \
  --corpus benchmark-results/diff-corpus/manifest.json \
  --ida /path/to/idat --diaphora /path/to/diaphora-source \
  --python /path/to/venv/bin/python \
  --python-path /path/to/venv/lib/python3.12/site-packages \
  --output-dir benchmark-results/diff-diaphora
```

The default profile includes library/thunk functions, decompiler output and
microcode. It requires an available licensed decompiler. Use `--no-decompiler
--no-microcode` for an explicitly different assembly-only profile, or
`--no-include-library-thunks` to exclude those functions. `--python-path` is
repeatable; select dependency directories compatible with IDA's embedded Python.
Inherited `DIAPHORA_*`, `PYTHONHOME` and `PYTHONPATH` settings do not silently
change the selected run. Other Diaphora options come from its archived source
configuration. `--cpus` defaults to 2 and `--timeout` to 600 seconds per process.

The runner executes a retained copy of the Diaphora source tree, including its
configuration, data and license notices. Both export receipts bind native IDA
SHA-256 identities to database bytes; admission also verifies the databases'
native MD5 and version fields. Matching must return success, preserve the
export bytes and finish without unhandled heuristic-thread exceptions. The
adapter retains those exceptions and refuses to save a successful result even
when Diaphora's main matching call returns success. Older receipts without this
thread check require separate failure-log review. Its receipt binds the result
to those ordered exports, and the
result database's own configuration must agree. The `diaphora-native` evaluator
rechecks this entire chain without requiring the original absolute database
locations to remain live. The format version is recorded separately from the
source hash; Diaphora tag 3.5.0 uses internal format version 3.4.

Both runners use the same timeout/cancellation handling and exit-code meanings
as the BinDiff runner. Existing results are preserved, incomplete runs are not
scored, and completed mismatches are retained. These are Linux end-to-end
diagnostics; they do not qualify shared-input fairness or competitive timing.

## Evaluate another retained result

The evaluator accepts a versioned run receipt for one corpus case:

```json
{
  "schema": "neverd.benchmark.diff.run.v1",
  "case": "stripped-reorder",
  "format": "bindiff",
  "track": "end-to-end",
  "completed": true,
  "tool": {"name": "BinDiff", "version": "exact installed version and revision"},
  "input_sha256": ["primary SHA-256", "secondary SHA-256"],
  "result": {"path": "before_vs_after.BinDiff", "sha256": "result SHA-256"},
  "artifacts": [
    {"path": "invocation.json", "sha256": "invocation receipt SHA-256"}
  ]
}
```

Paths in a receipt are relative to that receipt; corpus input paths are relative
to its manifest. Record commands, configuration, exporter/disassembler versions,
dependencies, export databases, analysis logs and completion evidence in hashed
artifacts. `completed` is the producer's recorded outcome, not an inference from
the existence of a result file. Pin those inputs when creating the receipt; do
not reconstruct a stronger provenance claim than the original run retained.

```bash
python3 scripts/benchmark_binary_diff.py evaluate \
  --corpus benchmark-results/diff-corpus/manifest.json \
  --run benchmark-results/bindiff/run.json \
  --output benchmark-results/bindiff/evaluation.json
```

| Format | Scored associations | Input binding |
| --- | --- | --- |
| `neverd` | `matches` in a complete `neverd.bindiff` schema 3 or 4 report | Native ordered SHA-256 plus receipt |
| `bindiff` | SQLite `function` rows; orientation comes from `metadata.file1/file2` | Native SHA-1 or SHA-256 plus receipt SHA-256 |
| `diaphora-native` | Completed match receipt referencing SQLite `best`/`partial` rows and both exports | Native IDA SHA-256, native export MD5, ordered result configuration and retained export/result hashes |
| `diaphora` | SQLite `results` rows in `best` or `partial`; hex addresses | Receipt; retained export databases belong in its artifacts |
| `ghidra-vt` | Accepted function associations exported by `ExportNeverDVTMatches.java` | Native ordered SHA-256 plus receipt |
| `ghidra` | Accepted function associations in the `neverd.baseline.ghidra-vt-smoke.v1` JSON export | Receipt; retained import/VT invocation evidence belongs in its artifacts |

Diaphora's `unreliable` and `multimatch` rows and Ghidra's available candidates,
data matches and rejected associations are not promoted to accepted function
pairs. Repeated Ghidra correlator evidence is deduplicated by address pair.
Conflicting pairs remain visible and are scored; they never overwrite each other.
Ghidra addresses must use the `ram` space. Other spaces need a qualified adapter.
These readers match the retained BinDiff 8, Diaphora tag 3.5.0 (internal format
version 3.4), and Ghidra 12.1.3 diagnostic outputs. They do not launch or install
those products. The Ghidra input is the recorded export format, not an arbitrary
VT database. Use the provided
[`ExportNeverDVTMatches.java`](../tools/neverd-bench/ExportNeverDVTMatches.java)
inside an existing completed Ghidra VT project:

```bash
/path/to/ghidra/support/analyzeHeadless /path/to/projects project-name \
  -process after.elf -noanalysis \
  -scriptPath /path/to/NeverD/tools/neverd-bench \
  -postScript ExportNeverDVTMatches.java /session-name /path/to/new-matches.json
```

The script reads the existing association decisions and native program SHA-256
identities, checks cancellation, and requires a new output file. Select
`ghidra-vt` in its run receipt. It does not create a VT session or accept
candidates. The legacy `ghidra` reader remains available for earlier receipts
without native hashes. The automated runners above add fresh import, correlation
and completion checks; exporting an existing session does not establish those
earlier stages retroactively.

Use `end-to-end` when each product performs its own import and analysis. A
`common-input` receipt describes a separately prepared, qualified shared-input
experiment. Merely selecting that string does not make different disassemblers'
inventories equivalent. Never combine those tracks into one leaderboard.

## Real source releases with partial truth

[`scripts/benchmark_binary_diff_corpus.py`](../scripts/benchmark_binary_diff_corpus.py)
prepares real ELF release pairs separately from the matching tools. Install its
optional, pinned dependency in a tooling environment:

```bash
python3 -m pip install --require-hashes -r scripts/requirements-binary-diff-bench.txt
python3 scripts/benchmark_binary_diff_corpus.py \
  --spec benchmark-results/source-spec.json --strip /usr/bin/strip \
  --output-dir benchmark-results/source-corpus
```

The source specification uses schema `neverd.benchmark.diff.sources.v1` and a
`cases` array. Each case has `id`, `category`, `primary` and `secondary`. Each
side declares `project`, `revision`, `license`, `source_url`, `source_root`, a
`binary` artifact and nonempty `provenance` artifacts. Paths resolve relative to
the specification; every artifact has `path` and `sha256`. Retain official
source archives, license files, compiler identity, configuration/build commands
and logs as provenance. Build with debug information and `-gdwarf-4`; only the
preparation tool receives these debug binaries and source roots.

The current profile admits x86-64/AArch64 ELF executables and shared objects
whose lowest-address load segment starts at file offset zero. It requires
DWARF 4 and native function symbols. Identity is the source-root-relative
compilation unit, declaration file and linkage name (or stated source name).
Only unique, concrete, contiguous bodies starting at a unique native function
entry are eligible. Aliases, overlapping ranges, entries inside fragments,
compiler clones and ambiguous identities are excluded with counts and reasons.
Declaration file indexes use their owning compilation unit. Abstract-origin
chains retain the original definition's source unit, including when GCC LTO
places the concrete body in an artificial unit. A specification's declaration
unit does not replace that definition unit; same-named local functions from
different source units remain distinct. Cyclic references fail, and missing
source origins remain excluded. Code must lie in executable allocated sections and file-backed
executable load segments. Unsupported layouts fail rather than guessing a base.

For PGO inputs, retain the instrumented build commands, training inputs and
round-trip results, collected profile files and final profile-use build logs.
Keep configure probes outside the profiling build: their reused temporary
names do not identify stable program units. Require matching profile data for
the actual compared units, and check that the generated configuration stays
the same. A successful build or training run is not a matching-quality result.
Treat opposite comparison directions and multiple builds of one project as
correlated cases, not independent projects or release pairs.

Preparation copies the debug binaries and provenance into `truth/`, emits
`inputs/primary.elf` and `inputs/secondary.elf` with static symbols/debug data
removed, and checks that allocated section bytes/metadata, ELF runtime identity
and program headers survive stripping. Dynamic symbols remain. Only these
stripped paths are supplied to a matcher. Shared source identities become
paired labels; an absent optimized body never proves addition or removal.
All exclusions and unpaired bodies remain in retained inventories.

The output is corpus schema `neverd.benchmark.diff.corpus.v2`, with explicit
`truth_complete: false`, `truth_kind: unique-source-bodies-v1`, input image bases
and hashed truth/preparation artifacts. The four runners above accept this
end-to-end profile. A common-input track is refused until an equivalent native
representation is separately qualified. Source identity establishes function
correspondence, not unchanged behavior, a vulnerability fix or semantic equality.

Native inventories determine address-space translation: NeverD's saved snapshot,
Ghidra's RAM function inventory, Diaphora's function database and BinExport2's
actual CallGraph vertices. BinExport may reconstruct entries absent from IDA's
earlier function list; its graph is the authority for BinDiff. Graph node types
and the earlier IDA count are reported separately. Imported graph nodes remain
visible and labelled-endpoint contradictions are still scored. Ghidra synthetic
EXTERNAL-to-EXTERNAL associations are retained in the native export but excluded
from RAM-body scoring; mixed or unknown spaces are refused. No translation is
inferred from successful matches or truth labels.

Freeze the complete harness, executables, libraries and selected external-tool
configuration before a run. Evaluations bind the loaded evaluator components,
not later edits to their source files. Preserve failed receipts when correcting
an adapter; a new evaluation must bind the original native result, all original
evidence and the new frozen evaluator. Completing native analysis, recovering
valid scoring and passing a quality gate are distinct outcomes.

## Freeze and check a numeric qualification

[`benchmark_binary_diff_qualification.py`](../scripts/benchmark_binary_diff_qualification.py)
binds a protocol, partial-truth corpus, candidate executable/library and harness
before the run's output directory exists. Freeze the complete scripts directory
before invoking it. The protocol uses schema
`neverd.benchmark.diff.heldout-protocol.v1`, `track: end-to-end`, a written `scope`,
the exact ordered `case_ids`, and these explicit gates and runtime settings:

```json
{
  "gates": {
    "minimum_labelled_pairs_per_case": 100,
    "minimum_conditional_precision_per_case": 0.995,
    "minimum_truth_recall_per_case": 0.9,
    "minimum_macro_f1": 0.95,
    "maximum_receipt_failures": 0,
    "maximum_timeouts": 0
  },
  "runtime": {"threads": 1, "stage_timeout_seconds": 1200}
}
```

Select the gates before inspecting qualification outcomes; the example is not
a universal product-quality requirement. The current profile requires zero
receipt failures and timeouts. It rejects missing/unknown gates, changed case
inventories and undefined precision. The plan preserves hashes, not authenticated
timestamps or proof that the dataset was never previously used for tuning.

```bash
python3 scripts/benchmark_binary_diff_qualification.py freeze \
  --protocol benchmark-results/protocol.json \
  --corpus benchmark-results/heldout/manifest.json \
  --neverd /path/to/frozen/neverd --library /path/to/frozen/libneverd.so \
  --plan benchmark-results/qualification-plan.json \
  --output-dir benchmark-results/qualification-run
python3 scripts/benchmark_binary_diff_qualification.py run \
  --plan benchmark-results/qualification-plan.json
python3 scripts/benchmark_binary_diff_qualification.py evaluate \
  --plan benchmark-results/qualification-plan.json \
  --output benchmark-results/qualification-replay.json
```

Every frozen case is reevaluated from its native report and input-bound run
receipt. Missing, failed or timed-out cases remain failures and contribute zero
to the macro-F1 numerator; they cannot disappear from its denominator. Aggregate
score files cannot replace native evidence. The runtime version probe uses the
same stage deadline and retains its receipt, including startup failures. Replay
checks both stages against the frozen deadline. The deadline bounds the process
wait; measured wall time also includes process startup, polling and cleanup.
The output retains individual
metrics, unscored denominators, failed receipts, macro F1 and the gates. Exit 0
means those declared gates passed, 1 means qualification failed, 2 means the
plan or invocation could not be admitted, and 130 means interruption. Replays
write a new file. Existing plans, runs and evaluations are never overwritten.
Passing covers only the written partial function-mapping scope; other advanced
capabilities, unlabelled functions and competitive/statistical claims need their
own evidence.

## Truth and metrics

Schema v1 supplies complete one-to-one function truth, with unique IDs and
addresses. A corpus must contain at least one case; case IDs must be unique
single directory names containing only ASCII letters, digits, dots, underscores
and hyphens, starting with a letter or digit. Invalid corpus declarations are
rejected before starting the runtime or creating output directories. A removed
function has a null secondary address; an added function
has a null primary address. Matching either to an unrelated function is a false
positive. Aliases at one address, split/merge regions and semantic equivalence
are outside these function-pair metrics.

`correct` counts accepted pairs in the truth. `wrong` counts other accepted
pairs. `missed` counts truth pairs absent from accepted results. A wrong pair
can also miss a true pair, so wrong and missed are not disjoint function counts.
Precision is correct / accepted pairs; recall is correct / expected pairs.
A zero denominator produces null, not a perfect score. `exact_mapping` requires
both zero wrong and zero missed pairs. The evaluator returns 0 for an exact
mapping, 1 for a scored mismatch and 2 for invalid input or an incomplete run.
For v2 partial truth, a pair touching either labelled endpoint is scorable:
it is correct if it equals the labelled pair and wrong otherwise. A pair with
two unlabelled endpoints is `unscored`, never silently counted correct or wrong.
Precision is conditional on these scorable accepted pairs; recall uses all
labelled pairs. Evaluations expose accepted/scorable/unscored denominators,
scored fraction, F1 and labelled discovery misses. `exact_mapping` stays null;
`scored_mapping_exact` describes only the labels. Zero-denominator precision or
recall remains null. F1 is zero when no pair is correct and there are accepted
or expected pairs; with neither it remains null.

Raw addresses, denominators, repeated evidence count and source hashes remain
in the evaluation for inspection. Freeze independent project/compiler holdouts
and numerical gates before opening their scores. Developing against a project
consumes it as development evidence; it cannot subsequently become a holdout
for the same candidate. A few release pairs do not establish statistical or
market-wide superiority.

Run the independent metric and adapter regressions with:

```bash
python3 -m unittest scripts.tests.test_benchmark_binary_diff \
  scripts.tests.test_benchmark_binary_diff_bindiff \
  scripts.tests.test_benchmark_binary_diff_external \
  scripts.tests.test_benchmark_binary_diff_binexport \
  scripts.tests.test_benchmark_binary_diff_corpus -v
```

Keep generated corpora and results in ignored benchmark directories. Expand
coverage with licensed, independently labeled and held-out applications before
setting competitive gates; see [the acceptance ledger](binary-diff-acceptance.md).
