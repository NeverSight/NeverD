# NeverD worker

`neverd-worker` owns one local NeverD C ABI Session and serves bounded, framed
JSON requests over stdin/stdout. It does not link Qt or LLVM itself. Native engine
logs go to stderr. The binary under analysis is never executed or modified.

Build without Qt against a matching shared engine:

```sh
cmake -S tools/neverd-worker -B build-worker -G Ninja \
  -DNEVERD_ENGINE_LIBRARY=/absolute/path/to/libneverd.dylib -DBUILD_TESTING=ON
cmake --build build-worker
ctest --test-dir build-worker --output-on-failure
```

Windows also needs `NEVERD_ENGINE_IMPLIB`. See [PROTOCOL.md](PROTOCOL.md) for the
exact transport, request schemas, limits, cancellation and persistence contracts.

- Metadata, function/string tables, disassembly, bytes, C/IR and references use
  bounded pages. Every instruction address crosses IPC as a hexadecimal string.
- `cfg_summary` builds an indexed graph snapshot up to 20,000 nodes and 100,000
  edges. `cfg_viewport` returns at most 256 nodes and 512 edges with deterministic
  positions and explicit truncation. The older `cfg` preview alone retains its
  500-node/2,000-edge limit. The existing engine still constructs a whole CFG
  result; its 32 MiB adapter limit can reject unusually large block text.
- Native Low/Med IR pages include exact retained instruction anchors when the
  engine exports `neverd_ir_view_json`. Headers and synthetic operations remain
  unmapped. C/High/LLVM and VM mappings are explicitly unsupported; older engines
  report the missing capability while keeping text browsing available. Anchors
  are not a complete set of contributing instructions after transformations.
- Annotations remain staged until Save. Rename history is saved immediately and
  refuses unsaved notes. Undo/redo, input identity, a recovery journal and shared
  OS writer locks protect user edits. Reload discards staged annotations and
  reads the current sidecars. New CLI/C ABI writers share the lock; older binaries
  and external editors require the additional foreign-state checks.
- Declarative contribution manifests register bounded read-only query templates;
  they cannot execute scripts, QML, arbitrary operations or edits.

Tests include framed transport, cancellation, 10k-node graph paging and culling,
history recovery/conflict refusal, and real-engine smoke tests. Native mapping
tests explicitly skip engines predating the additive API. A fixture engine is
only for CI/tests and must not be packaged as the production engine.
