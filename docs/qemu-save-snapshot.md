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

A PC match is a necessary check for using a capture at a selected function
entry, not a complete proof of invocation state. Image identity, runtime
relocation, original SP/return cell, register and memory availability, hidden
CPU state and external environment effects remain the producer's obligations.
In particular, a save with a matching kernel at an unrelated pause cannot
recover the incoming private stack bytes of the two unresolved ordinary
registry functions. No bytes are synthesized and no ordinary parameters are
removed. This tool does not yet import a complete
[V6 continuation context](x64-continuation-source.md).

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
