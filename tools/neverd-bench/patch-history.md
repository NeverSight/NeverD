# Durable patch commit cost

Mode: **PR Review**. Scope: `EnginePatch.cpp`, `ProjectHistory.cpp` and their
worker regressions. SDK byte admission, analysis invalidation and the public
C ABI remain unchanged.

The previous overlay optimization did not reach the limit: durable commits
still rewrote eight sidecar tables and repeatedly copied/encoded full JSON
states. Diagnostic phase timers showed sidecar publication dominating small
commits and command-diff/history work dominating large ones. Diagnostic
instrumentation was removed before the comparative runs.

| Symptom | Source | Consequence | Remedy |
|---|---|---|---|
| Patching rewrites unrelated notes and names | `ProjectHistory::persist` writes all sidecars | Extra atomic replacements, syncs and timestamp churn | Compare normalized table rows; write only changed tables while checking every table for external edits |
| Encoded documents are immediately discarded and recreated | Budget checks and writes serialize independently | Repeated CPU work and temporary allocation | Encode once and use those same bytes for admission and publication |
| A one-byte history command copies both complete overlays | The diff map owns two JSON rows per address | Allocation and copying proportional to the entire overlay | Keep references during comparison and copy only changed rows into the command |
| Already canonical arrays are repeatedly sorted | Normalization unconditionally sorts numeric address strings | Avoidable comparisons and string copies | Check sortedness linearly, sort only unordered input, retain validation and duplicate checks |
| Temporary after-states duplicate journal payloads | Publication copies local state into and out of the journal | Extra full DOM copies and peak memory | Move the temporary state into the journal and then into the committed state; retain the complete journal on failure |

The journal schema, history limits and atomic writer are unchanged. Changed
files still flush both their contents and directory. Every persisted target
is byte-budget checked before the journal is published, including unchanged
tables; recovery can therefore still materialize the complete target state.
Recovery keeps its full before/after conflict checks. Skipping an unchanged
file's write never skips reading or validating that file.

## Controlled measurements

Compared with the already optimized patch implementation on `dev` at
`e4bac38ad97853d86c75cd67d8a107c86c1f1357`. Both workers use the same frozen SDK
(`a0650ccf…`, built from `138a53a9…`) and the same Release/Clang 23/mimalloc
configuration. The SDK's driver/Z3 configuration was not reduced. Other work
ran on this Linux i9-13900H host; no own builds/tests overlapped the campaign.
A/B/B/A order, 15 samples per process, 30 pooled samples per variant. Figures
measure complete worker commits, including history and durable file writes,
with correctness readback and undo outside timing.

| Adjacent rows | Before median | After median | Speedup |
|---:|---:|---:|---:|
| 1 | 20.090 ms | 6.097 ms | 3.29× |
| 64 | 22.019 ms | 7.513 ms | 2.93× |
| 2,048 | 89.985 ms | 55.778 ms | 1.61× |
| 8,192 | 280.082 ms | 179.404 ms | 1.56× |

One warmed 64-row commit was also traced separately: file replacements fell
from **10 to 3**, and `fsync` calls from **20 to 6**. The three remaining files
are the recovery journal, changed patch table and history. No flush was removed
from any file that is written.

Whole-worker peak RSS at 8,192 rows was **190.9 → 179.1 MiB** (median of three
fresh processes, three checked edit/undo pairs each). This includes the SDK and
loaded input; it is not the allocation size of a patch. Timing and memory runs
are separate. Every timing sample, p95, load, binary/source hash and memory
observation is in [the evidence JSON](../../docs/evidence/patch-history-2026-10-25.json).

Reproduce the time comparison with the existing driver and matching snapshots:

```sh
python3 tools/neverd-bench/patch-scaling.py \
  --library /snapshot/bin/libneverd.so --worker /snapshot/bin/neverd-worker \
  --rows --range-bytes --worker-rows 1 64 2048 8192 --samples 15 \
  --output build-bench/patch-history.json
```

Use the same `TMPDIR` filesystem and run baseline/candidate/candidate/baseline
sequentially. The memory probe's complete source is retained in the evidence
JSON; it observes Linux `/proc/<pid>/status` outside timed operations.

## Remaining costs and validation

This is **not an absolute performance limit**. Small commits now perform three
durable file replacements, so storage latency is material. Large commits still
read, parse, validate and serialize complete state/history and publish complete
before/after recovery snapshots. That work remains proportional to total state.
A further large reduction would require an incremental journal or shared state
representation, with migration, compaction, undo and crash-recovery guarantees.
It is an architectural change, not a reason to remove consistency checks or
acknowledge a save before its durable writes finish. Cold input snapshots,
assembly, source compilation, reanalysis and GUI paint are separate boundaries
not measured here.

The final affected worker suite passes 13 groups, including real SDK patching,
source patching, history, local types, data presentation, function edits and
Git collaboration. New transaction tests inject failure before and after each
write of a two-table commit (eight boundaries), check foreign table/history
refusal, retry with newer staged edits, numeric normalization/duplicates and
oversized-history rejection before any publication. Real and mock SDK patch
tests verify unchanged notes keep their bytes/timestamps through edits,
undo/redo and reopen; foreign changes still reject the patch and restore bytes.
The unchanged-file assertion fails against the baseline, as expected.

The broader 56-test worker inventory had 54 passes, one existing failure in
`NeverDWorkerSourceVariableTypes` (missing LLVMC `qword_401000` variable metadata,
reproduced with the baseline and identical SDK), and one optional listing test
without its required private native input. Neither is represented as a pass.


The delivered worker in `build-gui/bin` also passes the 13 affected CTest groups.
The existing GUI with that worker passes the full X11 and private GNOME Wayland
PatchTests inventories: 78 Qt cases each (including setup/cleanup), zero
failures/skips. The GUI and SDK were preserved; the original workspace's GCC
Release worker build is separate from the controlled Clang comparison. Later
dev integration changed no worker or transport/SDK interfaces used by this
change. The focused sources still match the measured source hashes.
