# macOS native CPU execution (HVF)

NeverD uses Apple's [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor)
as the macOS counterpart to KVM and WHP. `--backend hvf` selects it explicitly;
`auto` selects HVF for a matching native host/guest ISA and native-capable
contract. The unrestricted `software-cpu-v1` profile continues to use Unicorn. An Apple Silicon build
executes ARM64, and an Intel build executes x86-64. Cross-ISA `auto` selection
continues to use Unicorn. HVF rejects a translated executable under Rosetta;
run the native arm64 NeverD build on Apple Silicon.

The transport requires macOS 11 or later and hardware virtualization. This API
baseline does not lower the minimum OS required by other build dependencies.
A selected native backend reports unavailable hardware or entitlement failures
without silently falling back. [Virtualization.framework](https://developer.apple.com/documentation/virtualization)
is a higher-level whole-VM API; NeverD needs the vCPU, register, mapping and
exception controls supplied by Hypervisor.framework.

## Build and signing

Enable `NEVERD_ENABLE_CPU_EMULATION=ON` (or driver emulation).
`NEVERD_EMULATION_BACKEND_HVF` defaults to `ON` and only links the framework on
macOS. `OFF` retains the `hvf` vocabulary and reports `build_disabled` through
the capability API.

The **process executable** needs `com.apple.security.hypervisor`. Signing only
`libneverd.dylib` is insufficient. CMake signs the CLI, worker and emulation test
executables using `resources/macos/neverd-hypervisor.entitlements` after linking.
`NEVERD_HVF_SIGN_IDENTITY` defaults to ad-hoc (`-`) and can select an existing
signing identity. Packaging reapplies the entitlement to the worker after
Mach-O dependency repair, preserves it during recursive signing, and verifies
it in the final bundle. See Apple's [entitlement documentation](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hypervisor).

An embedding application is responsible for its own executable's entitlement.
NeverD does not re-sign the caller's Python interpreter or another installed
application. Capability queries distinguish the build from live initialization;
use `cpu-capabilities --configuration=JSON --probe-host` to check the actual process.

Standalone worker/desktop builds also sign the worker by default because an
imported engine does not expose its build flags to CMake. Set
`NEVERD_WORKER_SIGN_HVF=OFF` only when that executable does not need HVF.

## Ownership and execution

`backends/hvf/HvfExecutor` owns one process VM and one native vCPU, created,
used and destroyed on a dedicated thread. Logical NeverD CPUs share that
executor and serialize entry. Switching CPUs unmaps the old physical backing
before mapping the new owner's two regions. An inactive CPU's destruction
cannot retire another CPU's mappings. Bindings detach synchronously before
releasing their projection and physical RAM. Failed partial registration is
rolled back; an unmap failure retires the VM before backing can be released.
An unrecoverable native teardown failure stops the process instead of retaining
dangling guest mappings.

Host mappings use the host page size (including 16 KiB on Apple Silicon).
NeverD's architectural page tables and guest budget remain 4 KiB. The ISA layer
owns instruction admission, page permissions, CPU state, memory transactions,
service traps and guest OS behavior. Native worker actions cannot call guest
observers or acquire the caller's core memory locks.

ARM64 explicitly single-steps the immutable TLB/I-cache maintenance sequence
before the admitted instruction. Debug exceptions routed to EL2 cannot be
masked using the guest's `PSTATE.D`. All scalar, TLS and FP/SIMD state uses the
existing ARM64 capture boundary and startup probe. Intel uses negotiated VMCS
controls, monitor-trap stepping, TLB invalidation, complete FP/SSE XSAVE packets
and authenticated exception exits. RIP/RFLAGS are installed and captured
directly through VMCS, including after vCPU recreation. CR0/CR4 honor both framework writable masks
and hardware fixed bits; host-required bits are hidden by the guest read
shadows. CR8 uses explicit VMX access exits and an architecture-owned read
completion: the host TPR API can disagree with the guest's actual value.
Only authenticated MOV-from-CR8 exits are completed; other control-register
accesses fail. The checked instruction inventory is unchanged. Each Intel
vCPU creation, including cancellation recovery, binds and
initializes a private managed `IA32_KERNEL_GS_BASE`. Guest MSR access stays
trapped, and unsupported MSR/SWAPGS instructions remain outside the checked ISA. Both architectures run the existing complete
state startup probe before exposing a CPU.

Queue admission observes the borrowed stop token and original deadline. One
native-step allowance covers preparation, maintenance, entry and capture.
`RunDeadline` acknowledges outstanding interrupts before returning. Cancelled
native entry recreates the vCPU so a late kick cannot affect the next task.
Unsolicited Intel host-interrupt exits retry the same native state under the
same cancellation generation, without reporting an instruction completion.
Host/capture failures and authenticated x64 exceptions retain priority over a
concurrent stop; ordinary cancelled state is not published. This is cooperative
cancellation, not a hard real-time deadline.

## Validation

On a native Mac, configure a Release build and run the dedicated evidence gate:

The host needs CMake, Ninja, Python 3, Clang, `ld.lld`, `lld-link`, `ld64.lld`
and `codesign`.
The three LLVM linkers build the authored ELF, PE and Mach-O fixtures; Xcode's linker
does not replace them. Put these tools on `PATH` for the manual CI workflow.
Missing native fixtures fail required HVF runs instead of silently skipping.

```sh
cmake -S . -B build-hvf -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_SHARED=OFF -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DNEVERD_ENABLE_CPU_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
python3 scripts/run_native_cpu_ci.py --build build-hvf \
  --evidence build-hvf/native-evidence --require-hvf
```

Apple Silicon can additionally use `-DNEVERD_LLVM_PREBUILT=ON`; Intel uses the
repository's pinned LLVM source build. The gate records the CTest inventory,
every test result, skipped foreign transports, source revision and a summary.
Required HVF tests must execute successfully; an all-skipped run cannot pass.
The required inventory includes three native Darwin startup/service fixtures on
ARM64 (macOS, iOS device and Simulator), or two on Intel (macOS and Simulator).
Missing registrations and skips fail the gate. Darwin CTest names retain their
exact GoogleTest identities instead of address-bearing parameter dumps.
`.github/workflows/hvf.yml` exposes the same manual gate for dedicated native
`self-hosted, macOS, ARM64/X64, hvf` runners. Its `hosted-intel` selection tries
GitHub's `macos-15-intel` runner. Both choices first compile and sign
`scripts/probe_hvf_host.c` and require actual VM/vCPU creation and teardown
before preparing LLVM. This availability probe does not execute guest code.
A hosted runner that denies HVF fails at that boundary; its label does not
establish virtualization support. After the CPU gate, the workflow also
requires every matching Darwin workload. Dedicated runners are not assumed
to be provisioned.

GitHub describes nested virtualization on hosted runners as experimental and
does not guarantee its stability, performance or compatibility. See its
[hosted-runner policy](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners).
Keep a dedicated native Mac path for repeatable acceptance. This platform
limitation does not identify the cause of an individual stalled run.

The workflow first builds `NeverDHvfTests`, whose dependency boundary is LLVM
Support and the decoder, and requires native instruction execution before the
full process/LLVM dependency build. `validation=transport` stops after this
diagnostic profile; `full` remains the default and also requires both complete
gates. `validation=darwin` builds the Darwin owner and requires every matching
native workload independently of the complete CPU gate. The transport profile
reuses the full inventory's HVF requirements and
records `hvf_transport_only=true`; it is not full CPU/process acceptance.
On Intel, the workflow additionally builds `NeverDX64ExceptionTests` and runs
its CR8 state/privilege regression before the large dependency build. The full
CPU gate owns the complete state/exception suite, without a duplicate preflight.
It uploads transport results and the state inventory before execution, then
preserves the isolated CR8 result before the complete CPU gate.
Artifacts include the run attempt so reruns retain their own evidence.
Full-inventory compilation has its own step and log. `test_parallel` selects
four CTest processes by default or one for a serialized comparison, while
compilation remains parallel. The result summary records the selected value,
host OS/kernel description and logical CPU count.
It can also run locally with `--require-hvf --hvf-transport-only` on
`scripts/run_native_cpu_ci.py`. `validation=probe` runs only the VM/vCPU
availability check without LLVM; it cannot establish instruction execution
or NeverD transport correctness.

Coverage includes complete register/FP state, both guest privilege levels,
page permissions and cross-page memory, aliases and saved contexts, live probes
alongside another CPU, multi-CPU isolation, owner-thread routing, partial map
rollback, queue cancellation, native loop interruption and retry. Both ISAs have
required raw loop interruption and completion-error fixtures. These must observe
an actual native return; cancellation before entry cannot satisfy the loop test.
The full profile also requires the Intel CR8 all-register and privilege regression.
The transport profile requires 12 ARM64 or 10 Intel cases, and the full profile
requires 16 or 14 respectively. Intel compilation alone does not establish Intel
runtime correctness; its native gate remains required.

Hardware-backed execution is not automatically faster for NeverD's checked
instruction-by-instruction contract. Startup, dispatch, state transfer and
maintenance cost must be included when comparing it with the same checked
Unicorn contract. No throughput improvement is promised by backend selection.

### Implementation validation, 2026-10-02 to 2026-10-03

On an Apple M4 Max, macOS 15.6.1, SDK 15.5, Release: the focused CPU/process/C API
suite passed 2,304 tests with zero failures (4,205 inapplicable platform, ISA or
backend parameter cases skipped). The expanded no-Unicorn gate covers 19
owners and passed 753 tests, including all nine required HVF cases, with 5,652
inapplicable or disabled parameter cases skipped. The disabled-HVF configuration passed both configuration tests and
skipped its five hardware cases. Packaging and evidence-script unit suites
passed 10 and 17 tests respectively.

A separate build with both testing and Unicorn disabled built the signed CLI,
initialized HVF and executed the ARM64 ELF process fixture. Capability and
Python SDK audits passed, along with 92 native-evidence/CI audit unit tests.
The expanded gate also covers Linux/Windows processes, integer ABI, sessions
and budgets. Removing a process test executable's entitlement verified that
ordinary runs skip but required native runs fail.

The subsequent [Darwin process environment](darwin-emulation.md) extension
expands that no-Unicorn gate to 20 owners. At `36e11ca8a`, 834 passed, zero
failed and 5,903 skipped, with all 13 required ARM64 cases executed (nine HVF
checks, three Darwin platform fixtures and one original host-kernel reference).
This includes 65 Darwin checks and all 39 required ARM64 process workloads for
macOS, iOS device and iOS Simulator guest contracts. Darwin's ARM64 OS pages
are 16 KiB even though the shared CPU mapping granule remains 4 KiB.

A final check found that the raw cancellation fixture could execute a stale
`HVC` when it rewrote its loop without guest instruction-cache maintenance.
Using separate immutable loop and retry programs passed 1,000 repetitions with
eight concurrent processes, followed by all 36 HVF/configuration/public API
smoke tests. The production ARM64 adapter retains guest cache maintenance.

Live CLI probes verified native selection, explicit foreign-ISA rejection,
the software-contract selection, and a `device_access` diagnostic when a copy
of the executable was signed without the entitlement. A standalone worker and
the complete Qt 6.11.1 bundle passed signature checks across 186 Mach-O images.
The packaged Cocoa GUI smoke and worker EVM load/disassembly passed. A signed
probe loaded the bundle's engine and executed HVF initialization and the ARM64
ELF fixture. This bundle's dependencies require macOS 15.0. Three backend translation units also
compiled against the macOS x86-64 SDK at that stage. Native Intel transport
results are recorded below; complete CPU/process acceptance is separate.

An alternating seven-sample microbenchmark, after warmup and excluding CPU
creation, executed the same checked ARM64 loop on both backends: two setup
instructions plus 1,000 `ADD/SUBS/B.NE` iterations, 3,002 guest instructions per
run. Final registers and PC were checked. Median elapsed time was 73.9 ms for
Unicorn and 95.1 ms for HVF (about 29% longer). This short integer loop provides
no evidence of a speedup and is not representative of every workload.

An extended seven-sample benchmark covered initialization, integer branches,
ordinary RAM, TLS/calls, alternating CPUs and a Linux process. It ran while
the shared host's load average was approximately 30 and observed large latency
ranges; these results are not a stable throughput claim. Direct entry counting
confirmed six native entries per ordinary ARM64 instruction (five maintenance
steps and one guest step). Registers, PC, RAM and instruction counts matched
the independent fixtures; the Linux process's normalized reports matched
Unicorn for normal exit, memory fault, unknown service and instruction-budget
stop. See the [detailed measurements](zh-CN/macos-hvf.md). Quiet-host performance
measurement and the Intel Mac HVF gate remain outstanding; no self-hosted runner
is currently configured for the repository. The focused Darwin gate has since
passed on both Linux KVM and Windows WHP with Unicorn disabled: each passed all
26 required x64 process cases, with 51 checks passed and zero failures overall.
See the [hosted execution evidence](darwin-emulation.md#hosted-native-verification-2026-10-03)
for the exact commits and scope.

At clean source `48042a5e90e0977585114de092e423cd64b7f95f`, the Apple Silicon full
gate passed 841 checks with zero failures, 5,942 inapplicable cases skipped and
all 16 required outcomes executed, including the strengthened interruption
inventory. Its 12-case transport subset also passed locally with no skips.
The complete evidence is in `build-hvf-native/hvf-cr8-full-arm-evidence/`.

Intel diagnosis removed writes to the framework-owned VMCS link pointer,
preserved framework VM-exit controls and applied hardware CR0/CR4 fixed bits.
An [independent 64-bit program](https://github.com/NeverSight/NeverD/actions/runs/37076076219)
then established that the hosted runner could execute through QEMU HVF.
The [isolated MSR experiment](https://github.com/NeverSight/NeverD/actions/runs/37078094659)
identified `IA32_KERNEL_GS_BASE`: enabling that context alone restored execution
and MTF stepping; the other eleven individual MSRs did not. The retained
VM-instruction error 12 was also present on successful runs, so it does not
identify the failed entry's cause. Production now initializes the managed
kernel-GS context on each vCPU creation.

The [isolated CR8 experiment](https://github.com/NeverSight/NeverD/actions/runs/37082402190)
at `e61928b8c` then reproduced a separate TPR synchronization defect: guest
writes and reads agreed at priorities 0, 1, 3 and 15, while host TPR readback
remained zero; host TPR/APIC writes did not produce the requested nonzero guest value.
Production `48042a5e9` uses authenticated CR8 read exits and retries unrelated
host IRQ exits. Added native cases exercise all sixteen destination registers,
CPL3 general-protection faults and unsolicited interrupts before cancellation.
The initial native gate exposed zero VMCS RFLAGS on the first retry after
cancellation recreated the vCPU. `99340b586` moved RIP/RFLAGS installation and
capture to VMCS. The [native transport gate](https://github.com/NeverSight/NeverD/actions/runs/37086427775)
at clean source `19a5f63a239bf1afac892f6907a72f62e8e998c1` then passed all ten
required cases with zero skips. Separate one-process and cancellation checks
also passed; all three interruption modes retained RFLAGS `0x202` through retry.
The artifact SHA-256 was verified against GitHub metadata. This establishes
transport recovery, not yet the complete Intel CPU and Darwin profiles.
The full gate at `9319c880d` passed 100 interruption/recovery repetitions and
transport, but did not return complete CPU/Darwin results; cancellation was
requested after more than 100 minutes in its combined build/test step. The
complete gate still needs all fourteen Intel requirements and broader CPU
coverage to finish successfully. Stalled and
cancelled runs are not acceptance evidence. Temporary instruction probes, API interposers and phase
tracing are removed; actual NeverD tests own ongoing acceptance.

At clean source `9319c880d78e93f5cb8a7a9360778084934de258`, the ARM64 transport
subset again passed all twelve cases without skips, and the native loop
interruption/retry test passed 100 repetitions without failures or skips.

The [Intel checkpoint run](https://github.com/NeverSight/NeverD/actions/runs/37090528761)
at clean source `e2a91ff057df563eb19183a045a3ad6446cb9af1` repeated all 100
interruption/recovery cycles and passed the ten-case transport gate without
skips. The isolated CR8 regression also passed: all sixteen destinations,
CPL3 protection faults, resume-flag behavior and complete state preservation.
Its separate JUnit result and the transport artifact were downloaded and their
SHA-256 digests verified. The state owner compiled and registered 952 cases;
these checkpoints do not establish completion of that larger suite.

At clean source `5251cc68591dc343c954c8b7a9b57fa5cb9190e8`, the
[state-transition family](https://github.com/NeverSight/NeverD/actions/runs/37091386132)
passed all five HVF cases in one process, with ten foreign-transport cases
skipped. Its verified artifact covers TLS/privilege, CR8, cancellation and
complete x87/SSE state across host changes, faults and stops. The job later
stopped reporting during artifact upload. A subsequent serialized transport
and state run reached the raw exception group without returning its result.
These observations do not identify a particular instruction failure or prove
a test-process concurrency defect. Later isolated results are recorded below;
the complete CPU/Darwin gate remains separate.

At clean source `908a830e6e3f1bbae6bc7ed7e534f3b13aeb1c1e`, both focused jobs
completed successfully. The [raw-fault job](https://github.com/NeverSight/NeverD/actions/runs/37094333371)
passed all 120 native exception cases, covering ten fault types at both
privilege levels, repeated recovery, mapping changes and private-gateway
integrity. The [division job](https://github.com/NeverSight/NeverD/actions/runs/37094335126)
passed all 128 native public-CPU division cases, including normal results,
terminal traps, explicit recovery and observer stops. Each also passed all five
state-transition cases, ten transport cases and 100 interruption/recovery
repetitions. Shared checks overlap and must not be added as independent cases.
Both artifacts were downloaded, SHA-256 verified and their individual XML
results inspected. Temporary isolation workflow inputs and steps are removed.
The [complete Intel gate](https://github.com/NeverSight/NeverD/actions/runs/37095689345)
at `4c6a12b913123d0555f067035527fe29f856f3b9` compiled all twenty owners, but
returned no CPU result after more than thirty minutes and was cancelled.
Its completed job still provided no downloadable execution log. The build
artifact's SHA-256 was verified; this is not full CPU acceptance. The
[independent Darwin gate](https://github.com/NeverSight/NeverD/actions/runs/37097301977)
at `4cbb729389df9485c9a9699c63c0be3a48862794` separately requires all 26 native
Intel workloads. A temporary
[per-owner diagnosis](https://github.com/NeverSight/NeverD/actions/runs/37098336208)
records each complete CPU owner's inventory, CTest exit code and XML before
continuing. Both ongoing runs use serialized tests; the emulation implementation
and tests match the successful focused runs. Neither has yet returned complete
acceptance evidence.

After integrating the subsequent `dev` changes, clean source
`f4bf8dde5cbc33d18ce053cb0722a54047e5a2d0` again passed all twelve ARM64
transport cases with no skips. The independent Darwin gate passed 65 checks,
failed none and skipped 221 inapplicable cases; all 39 required native workloads
executed. The new host/parallel summary fields were also verified in actual
Apple Silicon and Intel transport artifacts. Local evidence is in
`build-hvf-native/hvf-final-dev-transport-evidence/` and
`build-hvf-native/hvf-final-dev-darwin-evidence/`.

The subsequent integration pass repaired the test SDK's missing
`neverd_session_set_load_progress` and made the shared worker test client wait
for terminal responses while retaining progress for assertions. All eight
standalone worker checks passed, including three real-engine integrations.
The complete fixture-backed Qt/IPC/MCP suite passed all 19 checks on macOS.
The [desktop GUI workflow](https://github.com/NeverSight/NeverD/actions/runs/37053518872)
also passed on macOS, Windows and Ubuntu at commit `e078b129c`. This verifies
the test fixture and worker transport repair across all three desktop hosts.
