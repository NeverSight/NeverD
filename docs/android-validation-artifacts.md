# Android validation artifacts

The CI shard runner retires each profile's generated classes, DEX, dependency
JARs, recovered sources and detailed logs after its own validation completes.
It keeps one compact result per profile: the command, exit status, a compressed
file hash inventory, the fixture verdict and the final 16 KiB of diagnostics.
Repeated attempts reuse that profile's slot. Existing profile commands and JDK
selection remain unchanged; the driver supplies an owned work directory.

Local multi-step investigations use `scripts/mobile_android_driver.py` with a
private configuration and request. Do not create numbered copies of SDKs,
workspaces, or complete verification archives for each retry. Keep configuration
and sensitive input paths outside Git. For example:

```json
{
  "state_root": "/work/android-evidence",
  "work_root": "/scratch/android-work",
  "cwd": "/work/NeverD",
  "slots": ["owned-source", "owned-sdk", "probe", "source-audit", "sdk-stage"],
  "sdk_aliases": ["platform-declarations", "optional-provider"],
  "output_parents": ["/work/private-validation"]
}
```

A request identifies a fixed slot, immutable input files and the actual command.
`{work}` expands to that slot's work directory. Fixture runners can continue to
use their existing `--work-dir` interface:

```json
{
  "slot": "owned-source",
  "command": ["python3", "scripts/test_mobile_android_internal.py", "--work-dir", "{work}/output"],
  "inputs": [],
  "readers": ["encoding-check"],
  "timeout_seconds": 900
}
```

Add the fixture's required CLI and toolchain arguments to the command, and add
input `path`/`sha256` records when they must stay frozen across the run. Installed
JDKs, SDKs, the current incremental build and original inputs remain references;
the store does not copy or delete them. Run and release the completed reader with:

```bash
python3 scripts/mobile_android_driver.py --config /work/android-driver.json run /work/request.json
python3 scripts/mobile_android_driver.py --config /work/android-driver.json release \
  --slot owned-source --reader encoding-check
```

Register readers **before** production. `readers` retain both successful and
failed outputs, for example a red-case encoding comparison. `success_readers`
retain only successful outputs, for example a full-source compilation audit.
Python consumers use `ArtifactStore.reader()` while reading and release the
registered reader after retaining its necessary verdict. A slot with a pending
reader cannot be overwritten. An abandoned success-only producer does not pin
failed work. Use `output_directories` for a frozen producer's required output
paths: each must be absent or empty, lie under a configured parent, and not
overlap inputs or another live output. This also owns native staging files
inside an explicitly registered output parent, without scanning other folders.

SDK downloads use the `download --alias NAME --url URL --sha256 SHA256` command.
Identical bytes reuse the existing cache file. A staging request can `publish`
an owned result using `{"alias": "platform-declarations", "file": "output/api.jar"}`;
publication happens after the command and input checks pass, before workspace
retirement. `retire_sdk_aliases` drops consumed raw downloads. Cache replacement
or retirement keeps bytes referenced by active jobs and pending readers until
those readers finish. This is storage management; the owning metadata checks
still decide whether a declaration library is compatible.

Private full-package investigations can configure the `probe` adapter with a
frozen command template, qualified classpath manifest, output root and receipt
root. `probe --number N --cli-receipt PATH` checks the completed CLI build and
original inputs. It preserves the actual command and the existing auditor's
required output paths. Successful outputs remain registered for
`full-source-audit`; failed native staging is retired. The receipt consumer also
recovers after interruption. Only current compatibility receipts and the current
and previous compact comparison records belong to this adapter; it does not
adopt historical receipts or weaken the source/identity acceptance gates.

Success, failure, launch errors, timeouts and normal interruption use the same
retirement path. POSIX locks and child process groups keep active producers
safe. `SIGKILL` cannot run a finalizer: the next store startup recovers abandoned
jobs, while preserving any surviving child and explicit pending readers. Run
`recover` with the same configuration to perform that startup check directly.
Use a single state root for these producers, not competing cleanup scripts.

The inventory records apparent byte sizes, not disk space reclaimed. A scratch
volume can be memory-backed; its retired bytes are not SSD savings. Historical
recovery archives, unrelated directories and unregistered source files are not
adopted or pruned. To retain full diagnostic material for a concrete follow-up,
register a reader; the default compact result intentionally does not preserve
every log body or another complete source tree.

The ownership, repeated retry, cache reuse, signal, orphan recovery and actual
CI/local entry points are exercised without large fixtures:

```bash
python3 -m unittest -v scripts.tests.test_mobile_android_artifacts scripts.tests.test_mobile_ci_shards
```
