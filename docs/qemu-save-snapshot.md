# Offline QEMU save inspection

[`scripts/qemu_save_snapshot.py`](../scripts/qemu_save_snapshot.py) reads a
completed libvirt save without restoring a VM or executing guest instructions.
It exposes captured CPU/APIC fields and bounded reads of named RAM blocks.
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
python3 -m unittest scripts.tests.test_qemu_save_snapshot scripts.tests.test_qemu_save_x64 scripts.tests.test_qemu_save_memory -v
```

The interpretation references are QEMU 10.2's
[x86 VMState fields and omission predicates](https://github.com/qemu/qemu/blob/v10.2.0/target/i386/machine.c),
[register and flag encodings](https://github.com/qemu/qemu/blob/v10.2.0/target/i386/cpu.h),
and [KVM bank/event conversion](https://github.com/qemu/qemu/blob/v10.2.0/target/i386/kvm/kvm.c).
This adapter is independently authored from those format contracts.

## Supported save format

The reader accepts completed libvirt version 2 saves with raw, uncompressed
QEMU version 3 migration streams, an embedded VMState JSON trailer, section
footers and 4096-byte target pages. Libvirt header byte order is detected.
Device fields, arrays and nested/subsection layouts come from that save's
description; stream versions, subsection headers and serialized sizes must
agree. The captured `cpu`, `cpu_common` and `apic` fields are retained. Other
described devices are traversed without constructing device models.

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

The unit fixtures independently encode register bits, signed/little-endian
fields, arrays, nested/subsection layouts, RAM updates/fills and missing pages.
Negative cases cover invocation mismatch, format/version/extent errors,
truncation, duplicate JSON keys, invalid Unicode and inspection bounds.
Real-save qualification should additionally compare CPU/APIC fields with an
independent offline decoder and selected RAM bytes with existing capture
receipts. Keep saves, raw register/memory output and timing receipts local;
these tests require no guest or generated-program execution.
