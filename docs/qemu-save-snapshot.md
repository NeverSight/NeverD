# Offline QEMU save inspection

[`scripts/qemu_save_snapshot.py`](../scripts/qemu_save_snapshot.py) reads a
completed libvirt save without restoring a VM or executing guest instructions.
It exposes captured CPU/APIC fields, selected device state and bounded reads
of named RAM blocks.
Use it when an existing save contains native inputs that a binary image alone
cannot provide. It is a separate inspection tool; ordinary decompilation and
generated continuation drivers do not invoke it.

```sh
python3 scripts/qemu_save_snapshot.py paused.save > snapshot.json
python3 scripts/qemu_save_snapshot.py paused.save --cpu 0 \
  --expect-pc 0xffff800000001000 --ram pc.ram 0x2000 64
python3 -m unittest scripts.tests.test_qemu_save_snapshot -v
```

The second command's addresses are illustrative. `--expect-pc` compares the
selected CPU's **captured** `env.eip` field (RIP in 64-bit code, an offset in
segmented code). A mismatch returns exit
status 2 without publishing JSON; it never changes the PC or constructs a
function invocation. CPU numbers are migration instance IDs, not section IDs
or positions in the stream. Native integer fields use hex strings in JSON to
retain all 64 bits. Uninterpreted fields use `raw_hex`; the tool does not infer
device semantics from opaque bytes.

The Python API retains decoded integers and byte strings:

```python
from scripts.qemu_save_snapshot import QemuSaveSnapshot

with QemuSaveSnapshot("paused.save") as snapshot:
    snapshot.expect_pc(0, captured_pc)
    original_bytes = snapshot.read_ram("pc.ram", block_offset, 8)
```

Keep the save immutable for the lifetime of the reader. It opens the input
read-only, writes no output files and never treats RAM-block names as paths.
Normal filesystem permissions apply. It requires only Python's standard
library, not an installed QEMU or libvirt service.

## Selected device evidence

[`scripts/qemu_save_devices.py`](../scripts/qemu_save_devices.py) lists the
described non-RAM sections and optionally decodes selected devices:

```sh
python3 -m scripts.qemu_save_devices paused.save
python3 -m scripts.qemu_save_devices paused.save \
  --device '0000:00:00.0/mch' 0 --device ioapic 0
```

Select identities from that save's inventory; the example names are not
defaults. Selection uses the exact migration name and instance ID, never the
VMState layout name, section ID or list position. Repeated selectors return
one record in first-request order. An absent or invalid selection returns
status 2 without publishing a partial result.

Inventory rows retain migration identity, section ID, VMState name/version
and serialized byte count. Selected records additionally contain `fields`
and direct `subsection_versions`. Field integers use hex strings in JSON;
unknown field types remain `raw_hex`. QEMU's `int32 le` means a **less than or
equal restore check**, not little-endian storage: like `int32 equal`, its
wire bytes are big-endian. The reader preserves the saved integer, without
running the restore check against an invented current device version. See
QEMU's [VMState field serialization](https://github.com/qemu/qemu/blob/v10.2.0/migration/vmstate-types.c).

```python
with QemuSaveSnapshot("paused.save") as snapshot:
    identities = snapshot.device_inventory()
    mch = snapshot.read_device("0000:00:00.0/mch", 0)
    original_pci_config = mch["fields"]["parent_obj"]["config[0]"]
```

Construction validates all sections and retains their layouts and file
offsets. CPU/APIC/common fields keep their existing eager decoding; other
device fields are decoded only when requested. Inventory needs no payload
read. Each `read_device` performs an exact-key lookup and decodes only that
section, without rebuilding the RAM index. It returns a fresh record from
the original save, including when the caller has mutated a public CPU
dictionary or an earlier result. Retain returned records explicitly for reuse.
All layouts share the existing aggregate state and metadata limits.

The reader runs no device model or post-load callback. These fields can
support chipset and interrupt-controller inspection, but do not establish
effective physical mappings, device behavior or a complete execution
environment. Device state cannot recover an uncaptured function invocation.

## Address and invocation boundaries

`--ram` and `read_ram` take **RAM-block offsets**. These are not guest physical
or virtual addresses: a machine can split `pc.ram` around a physical address
hole, and aliases/devices have separate mappings. A consumer must supply and
validate the machine's physical mappings and, for virtual addresses, the saved
page tables. The reader performs neither mapping nor permission simulation.
The physical-memory adapter below accepts an explicit mapping for offline
observations; it does not discover that mapping from the RAM inventory.

A PC match is a necessary check for using a capture at a selected function
entry, not a complete proof of invocation state. Image identity, runtime
relocation, original SP/return cell, register and memory availability, hidden
CPU state and external environment effects remain the producer's obligations.
In particular, a save with a matching kernel at an unrelated pause cannot
recover the incoming private stack bytes of the two unresolved ordinary
registry functions. No bytes are synthesized and no ordinary parameters are
removed. The companion adapter below projects the captured V6 register banks;
it does not import a complete [continuation environment](x64-continuation-source.md).

## Explicit physical-memory observations

[`scripts/qemu_save_memory.py`](../scripts/qemu_save_memory.py) reads bounded
physical intervals through a producer-supplied, flattened map. Each interval
binds a physical start and size to a named RAM block and its original offset.
The producer must establish the **effective read mapping for the captured
address space**, resolving overlays and excluding MMIO, holes and unavailable
regions. Partial maps are allowed; unmapped addresses always fail. A RAM-block
inventory alone cannot establish physical addresses or device behavior.

For example, a map containing one independently established RAM fragment has
this shape (the addresses are illustrative, not a default Q35 map):

```json
{
  "schema": 1,
  "machine": "pc-q35-10.2",
  "ram_blocks": {"pc.ram": "0x100000000"},
  "regions": [
    {"physical_start": "0x12345000", "size": "0x1000",
     "ram_block": "pc.ram", "block_offset": "0x12345000"}
  ]
}
```

The machine must match the save and every referenced block needs its exact
captured size binding. These checks catch obvious mismatches; they do not
authenticate topology, save identity or the producer's mapping. Bind the map
to the same capture and address-space view as the register/environment data.
In particular, Q35's below/above-4G split is insufficient: VGA/ROM, PAM,
SMRAM/TSEG and device mappings can override RAM. See QEMU's
[memory regions and aliases](https://github.com/qemu/qemu/blob/v10.2.0/docs/devel/memory.rst),
[Q35 RAM split](https://github.com/qemu/qemu/blob/v10.2.0/hw/i386/pc_q35.c),
and [chipset overlays](https://github.com/qemu/qemu/blob/v10.2.0/hw/pci-host/q35.c).
No machine-specific mapping is guessed by this adapter.

Map extents and offsets accept unsigned JSON integers or `0x`-prefixed hex
strings with at most 16 digits. Booleans, floats, unknown fields, duplicate
keys, zero-size regions, physical overlaps and overflowing/out-of-block
extents fail. Distinct physical intervals may alias the same saved bytes.
Boundaries can be byte-aligned; a read may cross adjacent regions and RAM
blocks. Missing saved pages remain unavailable even inside declared regions.
An empty read checks its uint64 address and requires no mapped byte.

```sh
python3 -m scripts.qemu_save_memory paused.save --map physical-map.json \
  --physical 0x12345000 64
```

Successful JSON includes the normalized supplied mapping and the complete read.
A failed observation returns status 2 with no JSON or partial bytes. The Python
API reuses the same open snapshot as register-bank projection:

```python
from scripts.qemu_save_memory import QemuPhysicalMemory

with QemuSaveSnapshot("paused.save") as snapshot:
    snapshot.expect_pc(cpu_instance, captured_pc)
    memory = QemuPhysicalMemory.from_file(snapshot, "physical-map.json")
    original_bytes = memory.read_physical(physical_address, 8)
```

`QemuPhysicalMemory(snapshot, description)` also accepts a decoded map. The
validated map is detached from caller mutation. Construction sorts its regions
once; a read uses one binary search and visits only the touched fragments and
saved pages. It borrows the existing RAM index without copying or caching RAM.
`SnapshotLimits` owns the JSON-byte, region-count and total-read bounds. Keep
the snapshot open and its save immutable throughout the view's lifetime.

This is a read-only observation adapter, not a mutable execution provider or a
page walker. It does not update page-table A/D bits, simulate faults or certify
CPU/device access permissions. Ordinary decompilation does not import it, and
it cannot recover an invocation that was not captured.

## Q35 main-RAM consistency

[`scripts/qemu_save_q35.py`](../scripts/qemu_save_q35.py) checks a supplied
partial main-RAM map against the selected save's MCH configuration. It catches
wrong original block offsets, RAM split/gap errors and maps that expose TSEG
or PCI Express configuration MMIO as ordinary RAM. Use the explicit
`qemu-10.2-q35-main-ram` profile with a `pc-q35-10.2` save, MCH v1/PCI v2 and
the matching Intel MCH identity. Select the MCH by its exact migration name
and instance, as in device inspection above.

The producer must bind the original main RAM block, below-4G RAM size,
above-4G RAM base, whether the machine created SMM ranges and its
`extended-tseg-mbytes` property. These machine properties are **not all saved
in MCH VMState**. Neither a machine name nor the RAM inventory authenticates
them. In particular, the guest-writable PCI TSEG query field is not an
authoritative copy of the extended size; the check uses the explicit property
binding. Do not copy these example values to an unrelated capture:

```sh
python3 -m scripts.qemu_save_q35 paused.save --map physical-map.json \
  --profile qemu-10.2-q35-main-ram --mch '0000:00:00.0/mch' 0 \
  --ram-block pc.ram --below-4g 0x80000000 --above-4g-start 0x100000000 \
  --smm-ranges on --extended-tseg-mib 64 --physical 0x12345000 64
```

All supplied regions are checked before the optional read. A rejected region
is an error, never silently removed or shortened. TSEG uses its captured
enable bits and 1/2/8 MiB or bound extended size. PCIEXBAR uses this QEMU
version's masks and length encoding. A reserved length leaves QEMU's previous
mapping unchanged, so even a disabled register cannot certify the address
space; the check rejects it as unknown history.

Only direct main RAM outside the first MiB is admitted. Low legacy RAM,
PAM/ROM/VGA, SMBASE, SMM views and other aliases are outside this profile.
Other device overlays, CPU-local APIC mappings, machine-property provenance,
paging and CPU access permissions remain the producer's obligations.
Success means **MCH consistency for the supplied regions**, not a complete
physical-topology or execution-environment certificate. The generic physical
map adapter remains available for independently established other mappings.

```python
from scripts.qemu_save_q35 import PROFILE, check_main_ram

with QemuSaveSnapshot("paused.save") as snapshot:
    memory = QemuPhysicalMemory.from_file(snapshot, "physical-map.json")
    evidence = check_main_ram(
        snapshot, memory, profile=PROFILE, mch_name=mch_name, mch_instance=mch_instance,
        ram_block=main_ram_block, below_4g=original_low_ram_bytes,
        above_4g_start=original_high_ram_base, smm_ranges=original_smm_ranges,
        extended_tseg_mib=original_extended_tseg_mib)
    original_bytes = memory.read_physical(physical_address, 8)
```

The map and device evidence must share the same open, immutable snapshot.
Checking reads one selected MCH record and visits each supplied region once,
without observing RAM contents. Subsequent reads use the existing map and
page indices, with no extra check or cache. JSON includes the explicit
bindings, selected MCH/configuration digest, excluded ranges, normalized map
and optional complete read. Failure publishes no partial result.

The independently authored interpretation follows QEMU 10.2's
[MCH mapping rules](https://github.com/qemu/qemu/blob/v10.2.0/hw/pci-host/q35.c),
[register encodings](https://github.com/qemu/qemu/blob/v10.2.0/include/hw/pci-host/q35.h),
[RAM split](https://github.com/qemu/qemu/blob/v10.2.0/hw/i386/pc_q35.c) and
[RAM aliases](https://github.com/qemu/qemu/blob/v10.2.0/hw/i386/pc.c).
No device callback or guest instruction is executed.

## V6 register-bank projection

[`scripts/qemu_save_x64.py`](../scripts/qemu_save_x64.py) converts the captured
banks to the existing `neverd_x64_state_v6` fields. It is a separate offline
adapter, with no SDK/decompiler work or generated instruction-loop checks.
It requires an explicitly bound QEMU 10.2 KVM/x64 interpretation profile,
CPU v12/APIC v3 layouts, captured 64-bit code and an exact PC match:

```sh
python3 -m scripts.qemu_save_x64 paused.save \
  --profile qemu-10.2-kvm-x64 --cpu 0 --apic 0 \
  --expect-pc 0xffff800000001000 --undefined 0 0 0 0 0 0 0 0
```

The producer supplies the QEMU profile and the CPU's associated APIC instance;
matching migration instance numbers alone do not prove that association.
The profile is an interpretation contract, not automatic authentication of the
saving executable's version. The address above is illustrative. CS.L and
EFER.LMA must identify 64-bit code; a segmented `env.eip` cannot become a linear
V6 PC. No PC/SP adjustment or relocation is performed.

The adapter preserves all 16 GPRs in x86 encoding order, flags, CR2, STAR,
MXCSR, KernelGSBase, segment caches and descriptor tables. CR8 comes from the
bound APIC's TPR priority class. FS/GS bases retain their single V6 owner.
Segment attributes use the V6 encoding and limits remain effective byte
limits. Unusable bits follow QEMU's KVM restoration-equivalent representation
of the present flag; ignored cache bytes remain intact. NMI blocking comes
from the saved masking bit, independently of pending/injected NMI state.

Mandatory XMM/YMM lanes must be present even when XCR0 disables their use. An
AVX-512 subsection must use version 1 and supply both its upper-lane and high
register arrays. Under the selected QEMU format, omission of this subsection
proves those banks zero because any nonzero lane, high register or opmask causes
it to be emitted. This conditional format rule does not permit filling missing
mandatory fields with zero. The raw reader now retains direct device
`subsection_versions` alongside its decoded fields.

`--undefined` explicitly supplies the eight architectural arbitrary-effect
choices. They are not captured inputs or replacements for missing stack bytes.
Choosing zeros selects one permitted set of choices for the first dynamic
instruction; the continuation contract still governs subsequent choices.
Only reserved V6 storage is otherwise initialized to zero.

JSON output contains projected fields, their producer bindings and the original
CPU/APIC/common state as `environment`. That environment retains CR0/CR3/CR4,
debug and other banks, opmasks, pending events and halted state. Projection
does not certify that these inputs are supported by an execution provider.
It does not wake a halted CPU, consume pending events, supply physical mappings
or run a page walk. A consumer still needs the original RAM/return-cell lifetime,
page-table/features binding and complete external effects.

`--c-initializer` emits a constant `neverd_captured_state_v6` bank image. Include
the selected source export's V6 declaration before it. Field names use that
declaration directly; the adapter does not maintain a competing table of byte
offsets. Keep the JSON environment when composing this image with a provider;
the C initializer alone is not a complete runnable context. With Python, a
single indexed save can provide both outputs:

```python
from scripts.qemu_save_x64 import PROFILE, project_v6

with QemuSaveSnapshot("paused.save") as snapshot:
    projection = project_v6(snapshot, profile=PROFILE, cpu=cpu_instance,
                            apic=associated_apic_instance, expected_pc=captured_pc,
                            undefined=explicit_choices)
    register_and_environment_evidence = projection.summary()
    c_bank_initializer = projection.c_initializer()
```

Projection touches only the selected saved banks, with fixed-size vector work;
it neither rebuilds the RAM index nor copies guest RAM. The independent byte
fixtures cover vector lane order and omission, explicit choices, descriptor
encoding, unusable-bit order, separate NMI masking/pending state, layout/version
errors and rejection of an unrelated entry PC:

```sh
python3 -m unittest scripts.tests.test_qemu_save_snapshot \
  scripts.tests.test_qemu_save_x64 scripts.tests.test_qemu_save_memory \
  scripts.tests.test_qemu_save_devices scripts.tests.test_qemu_save_q35 \
  scripts.tests.test_qemu_save_controls scripts.tests.test_qemu_save_events -v
```

The interpretation references are QEMU 10.2's
[x86 VMState fields and omission predicates](https://github.com/qemu/qemu/blob/v10.2.0/target/i386/machine.c),
[register and flag encodings](https://github.com/qemu/qemu/blob/v10.2.0/target/i386/cpu.h),
and [KVM bank/event conversion](https://github.com/qemu/qemu/blob/v10.2.0/target/i386/kvm/kvm.c).

## Paging and debug control banks

[`scripts/qemu_save_controls.py`](../scripts/qemu_save_controls.py) projects
saved controls into the existing `X86PagingState` and `X64PagedDebugState`
fields. It shares the register adapter's exact CPU, producer version and
64-bit PC-coordinate checks, and requires captured enabled A20. It reads no
RAM, walks no pages and creates no execution context.

CR0/CR3/CR4, EFER, RFLAGS and DR0–DR3/DR6/DR7 retain their complete saved bits.
CPU capabilities such as physical-address width are not inferred from active
control bits or the host. The existing paging and debug runtime validators
remain authoritative for mode, feature and execution-profile admission.

The selected QEMU version's omission rules differ by bank:

- An absent `cpu/pkru` or `cpu/pkrs` means zero. Present v1 subsections require
  the complete 32-bit field, even when the corresponding CR4 feature is off.
- An absent `cpu/pdptrs` means its cached bank is invalid, not that its four
  entries are zero. Bind that inactive bank explicitly with `--missing-pdpte`.
  A present v1 bank is retained and cannot be overridden. Reading current CR3
  memory would not reconstruct the previously loaded registers.
- IA32_DEBUGCTL and pending data-breakpoint causes are not serialized by this
  profile. `--debugctl` and `--pending-data` are required producer inputs, with
  provenance distinct from captured fields. Passing zero is an explicit
  binding, not a recovered observation.

For example, after independently establishing the unsaved inputs:

```sh
python3 -m scripts.qemu_save_controls paused.save \
  --profile qemu-10.2-kvm-x64 --cpu 0 --expect-pc 0xfffff80000123456 \
  --debugctl 0 --pending-data 0 --missing-pdpte 0 0 0 0 \
  --cpp-initializer > captured-controls.inc
```

Without `--cpp-initializer`, JSON includes each field's origin, exact CPU/PC
bindings, and raw CPU/common evidence. Failures publish no partial result.
The C++20 output uses the current public header types and assigns fields by
name in `constexpr` initializers; it duplicates no layout offsets and needs
no initialization work at runtime. Copy these bank images into the embedding's
owned state and bind its feature set, debug profile and physical RAM through
the existing [paged execution API](x86-paged-execution.md). This internal
embedding API is not a stable C SDK runtime constructor.

Pending/injected events, halted state, APIC priority and instruction shadows
are not consumed by this projection. In particular, the selected QEMU KVM
event-copy path does not preserve `interrupt.shadow`; a clear serialized
`hflags` inhibition bit is not evidence that both STI and MOV-SS shadows were
clear. These require an independent producer binding. Bank transport is not
an executable capture, a complete environment import, a permission check or
recovery of an uncaptured function invocation.

The interpretation follows the same QEMU
[CPU VMState fields and omission predicates](https://github.com/qemu/qemu/blob/v10.2.0/target/i386/machine.c)
and [KVM register/event conversion](https://github.com/qemu/qemu/blob/v10.2.0/target/i386/kvm/kvm.c)
referenced above. No QEMU post-load callback or guest instruction runs.
This adapter is independently authored from those format contracts.

## CPU event boundary evidence

[`scripts/qemu_save_events.py`](../scripts/qemu_save_events.py) interprets saved
exception, interrupt, NMI and CPU activity state for the same explicit
`qemu-10.2-kvm-x64` profile. It shares the CPU v12/APIC v3 and exact 64-bit
PC checks with the register adapter and requires matching CPU-common v1.
The producer still supplies the CPU/APIC association. For an independently
identified capture and association:

```sh
python3 -m scripts.qemu_save_events paused.save \
  --profile qemu-10.2-kvm-x64 --cpu 0 --apic 0 \
  --expect-pc 0xfffff80000123456
```

The address is illustrative. JSON retains the original CPU/common/APIC
records and separates the following interpretations:

- A valid exception number becomes **injected on QEMU post-load** when both
  phase flags are clear, including when `cpu/exception_info` was omitted.
  The projection marks that origin explicitly. Injected does not mean already
  handled: the producer must preserve any reinjection obligation.
- Pending exceptions and their saved payloads remain separate from injected
  exceptions. QEMU pre-save can fold a non-nested pending exception into
  injected state, applying its payload to CR2 or DR6. The projection describes
  restoration-equivalent state; it cannot recover that lost phase distinction.
  An omitted payload remains `null`, not a fabricated zero.
- Error-code presence must match its v1 subsection. Signed wire values retain
  their full 32-bit pattern. An old error code with exception number `-1`
  remains inactive data and does not create an exception.
- Injected interrupt vectors retain their software-interrupt flag. NMI
  injected, pending and blocked values stay independent. Triple-fault and
  crash omissions mean zero under their respective QEMU predicates.
- Halted/MP state, SIPI vector, CPU interrupt requests, SMM/nested-mode flags
  and APIC IRR/ISR/TMR, TPR, base and spurious-vector register remain visible.
  A blocked APIC request is retained. No priority selection, wakeup,
  acknowledgement or event delivery occurs.

`interruptibility.STIBlocked`, `SSBlocked` and `NMIBlockedBySTI` are `null`
unless independently supplied with `--sti-blocked`, `--ss-blocked` and
`--nmi-blocked-by-sti`. Each supplied bit has `producer-bound` provenance;
unavailable inputs have `not-saved` provenance. These flags also exist as
optional Python keyword arguments to `project_events`. Clear saved `hflags`
does not fill them. Captured NMI masking remains a separate fact and agrees
with the V6 bank projection. The existing scheduler owns execution admission
and shadow lifetime; this inspector does not duplicate those rules.

The result is event evidence, **not a selected `neverd_x64_event_v1` or a
runnable scheduler context**. Exception numbers alone do not determine
fault/trap restart semantics. APIC/PIC/device arbitration, in-flight delivery,
external interrupt sources and the original invocation still require their
own evidence and implementation. No C initializer, synthetic queue, callback
or implicit event polling is emitted. Missing mandatory fields, inconsistent
subsections or invalid bindings fail without partial JSON output.

Conversion accesses only the selected CPU/common/APIC records in the existing
reader. It reads no RAM, decodes no additional device and rebuilds no page
index. Ordinary decompilation and generated instruction loops do not call it.
The independently authored interpretation follows QEMU 10.2's
[CPU save/post-load and omission rules](https://github.com/qemu/qemu/blob/v10.2.0/target/i386/machine.c),
[KVM event conversion](https://github.com/qemu/qemu/blob/v10.2.0/target/i386/kvm/kvm.c),
[CPU-common fields](https://github.com/qemu/qemu/blob/v10.2.0/hw/core/cpu-system.c)
and [APIC fields](https://github.com/qemu/qemu/blob/v10.2.0/hw/intc/apic_common.c).
It executes none of their callbacks or guest instructions.

## Supported save format

The reader accepts completed libvirt version 2 saves with raw, uncompressed
QEMU version 3 migration streams, an embedded VMState JSON trailer, section
footers and 4096-byte target pages. Libvirt header byte order is detected.
Device fields, arrays and nested/subsection layouts come from that save's
description; stream versions, subsection headers and serialized sizes must
agree. The captured `cpu`, `cpu_common` and `apic` fields are retained. Other
described devices are validated and indexed for optional reads without
constructing device models.

RAM version 4 supports block inventories, full page records, single-byte fill
records and continuation records across sections. Later records replace earlier
ones, including zero fills. An unsaved page is absent, never implicit zero.
Reads crossing an absent page or a block boundary fail. The only accepted
migration command is the empty `SWITCHOVER_START` notification. Compressed
saves, XBZRLE/compressed RAM, postcopy, external/mapped RAM, migration
configuration extensions and undescribed sections are unsupported and fail
explicitly. This is an inspection profile, not a universal migration decoder
or a certificate that QEMU can restore the save.

The RAM index takes eight bytes per target page; page contents stay in the
input file. Construction makes one pass over record headers and skips page
payloads. Each subsequent read looks up only its requested pages. There is
no full RAM copy, persistent cache or work added to the SDK/CLI decompiler.
[`SnapshotLimits`](../scripts/qemu_save_snapshot.py) owns the metadata, expanded
device-state, nesting, total RAM and individual read bounds. API consumers can
choose an explicit limits object for larger offline inspections.

## Format references and validation

This is an independently authored reader, not a vendored migration script.
Format references are the
[libvirt save header](https://github.com/libvirt/libvirt/blob/v12.0.0/src/qemu/qemu_saveimage.h),
QEMU 10.2's [stream records](https://github.com/qemu/qemu/blob/v10.2.0/migration/savevm.h),
[RAM records](https://github.com/qemu/qemu/blob/v10.2.0/migration/ram.c),
and [VMState serialization](https://github.com/qemu/qemu/blob/v10.2.0/migration/vmstate.c).
QEMU documents its own offline inspector in
[migration best practices](https://www.qemu.org/docs/master/devel/migration/best-practices.html).

The unit fixtures independently encode register bits, signed and comparison
integer fields, arrays, nested/subsection layouts, RAM updates/fills and missing
pages. Device fixtures cover exact identities, lazy field decoding, raw PCI
bytes, interleaved RAM reads, mutation isolation and aggregate limits.
Negative cases cover invocation mismatch, format/version/extent errors,
truncation, duplicate JSON keys, invalid Unicode and inspection bounds.
Real-save qualification should additionally compare CPU/APIC/device fields with an
independent offline decoder and selected RAM bytes with existing capture
receipts. Keep saves, raw register/memory output and timing receipts local;
these tests require no guest or generated-program execution.
