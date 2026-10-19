# Windows emulation reference resources

This directory owns the format and examples for versioned Windows reference
packs. Emulator implementation remains under `lib/emulation/os/windows`.
Local binary/PDB payloads live in the ignored `packs/` directory or an external
directory chosen by the caller.

```text
windows/
  schema/environment.schema.json
  examples/19041-x64-reference.json
  packs/19041-x64-reference/
    manifest.json
    images/ntoskrnl.exe
    symbols/ntkrnlmp.pdb
    reference.scenario.json
```

The manifest uses paths relative to the pack root and records each module's
own image/PDB hashes and CodeView identity. The directory name is a readable
label. A label or the kernel's PDB identity alone does not identify all modules
of a larger environment. Keep the matched files together and preserve exact
hashes when adding modules. The schema describes resource inventory only; it
does not grant execution support or create loaded modules.

New persistent executions register caller-selected binaries and optional PDBs
through the format-independent `ModuleSet` interface. Use a modules JSON file
with `neverd_module_set_from_file`, then select `image_module` and optional
`kernel_module` in a `windows-driver` execution configuration. The catalogue
also supports user-built modules; it is not tied to these reference packs.
Resource preparation follows the configured eager/lazy policy. Additional
Native module instances require explicit execution bindings; registering a
pack's files does not map or initialize them. Supported missing resources can
be added and supplied to a retained execution through the public C API or
interactive CLI. See [runtime modules](../../../docs/runtime-modules.md) and
the [payload-free examples](../../modules/examples/README.md).

The legacy explicit `kernel_image` and `kernel_pdb` scenario paths remain
compatible and use the same shared file/PDB preparation. For that form, resolve
the selected paths against the pack root and supply the modeled Windows version
in the scenario. The local `reference.scenario.json` is a generated convenience
file with absolute paths; regenerate it after moving the pack. A resource pack
manifest is not a modules configuration or a driver scenario. There is no
automatic download, host-image fallback or user-data-directory search.

The separate runtime `service_images` scenario field can supply explicitly
chosen guest child drivers. Their ordinary PE imports, entry/unload routines
and device state use the existing session. This does not load reference-pack
modules automatically. See [driver emulation](../../../docs/driver-emulation.md)
for the supported lifecycle.

The installed resource path can be designed independently later. This local
layout requires no executable working directory or source-tree dependency in
the kernel model. PE/PDB validation remains in the shared image/PDB loaders;
the inventory schema does not replace it.

The earlier `lib/emulation/os/windows/bin` input copy remains available for
historical validation records. New local runs should use explicit module inputs;
payloads may stay in this ignored resource layer or any caller-selected directory.
The optional local `19041-subsystem-reference` pack contains independently
matched `ndis.sys`, `fltmgr.sys` and `netio.sys` images/PDBs for subsystem
contract analysis. Each module retains its own hashes and CodeView GUID/DBI
age. Public PDB Info stream write ages may be higher after symbol publishing;
the linked DBI age is compared with the PE, as in NeverD's shared PDB loader.
These files are analysis inputs and are not automatically mapped or given
execution models by their presence in a pack.
See [driver emulation](../../../docs/driver-emulation.md) for the runtime
contracts. Local validation reports and reference payloads are not distributed
with the public module examples.
