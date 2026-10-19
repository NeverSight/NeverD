# Caller-provided module resources

These files describe inputs owned by the caller; no binaries or PDBs are
distributed here. Copy the configuration beside your own `images` and `symbols`
directories and replace the paths/identities as needed. `ModuleSet` is generic:
an ID is not tied to ntoskrnl, Microsoft, kernel scope or one address space.

`driver-replay.modules.json` prepares the selected kernel reference eagerly and
keeps the other files lazy. Source registration alone does not map a guest
instance. Runtime bindings select scope/address space/base separately. The
driver replay consumer selects `kernel` for its existing kernel-reference
adapter; that role retains its own version/layout requirements.

`driver-replay.scenario.json` is a minimal explicit paging configuration for the
existing 19041 model. A real workload needs its own observed scenario inputs;
the example does not claim to initialize arbitrary driver business state.
The common resource interface has no 19041 restriction. Use the public
`modules` CLI to inspect file/PDB identity, then the
[SDK replay runner](../../../scripts/labs/README.md) or interactive `execution`
CLI to run the chosen driver scenario. Application execution adapters remain
reserved for later implementation.
