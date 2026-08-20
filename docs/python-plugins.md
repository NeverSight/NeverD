**Languages**: [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← Documentation Index](README.md)

# Python plugins

NeverD can load a Python file as a first-class plugin. Python plugins share the
same metadata, lifecycle, ordering, duplicate-name rules, event stream, and
session C ABI as native plugins. The supported authoring package is
`neverd-plugin`; do not import the private `_neverd_plugin` bridge directly.

## Build and runtime requirements

`NEVERD_ENABLE_PYTHON_PLUGINS` defaults to `ON`. Enabled builds require a
CPython 3.10-or-newer interpreter and embedding development library discoverable
by CMake:

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

Set `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` for a native-only `libneverd` with no
CPython link dependency. A Python-enabled build stages the matching package and
examples under `build/bin/sdk/python/`; that directory is also directly
installable with `python3 -m pip install build/bin/sdk/python`.

## Write a plugin

One module declares exactly one decorated class:

```python
from neverd_plugin import Event, Plugin, PluginType, Session


@Plugin(
    name="Analysis Report",
    version="1.0.0",
    author="Your team",
    description="Reports basic information about the loaded binary",
    type=PluginType.PROCESSOR,
)
class AnalysisReport:
    def on_init(self, session: Session) -> int | None:
        print(session.architecture)
        return None

    def on_run(self, session: Session, arg: int) -> int | None:
        print(session.file_path, session.function_count)
        return 0

    def on_event(self, event: Event) -> int | None:
        print(event.type.name)
        return None

    def on_term(self) -> None:
        pass
```

All hooks are optional. `None` means success; an integer result must fit a C
`int`. Metadata versions use strict SemVer. Names must be non-empty UTF-8, and
all metadata is rejected if it contains an embedded NUL.

The repository examples are
[`minimal.py`](../pluginsdk/python/examples/minimal.py) and
[`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py), plus
[`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py)
for the proof-gated optimization APIs.

## Load and inspect plugins

The C API can load a specific `.py` file deterministically or scan a directory:

```c
if (!neverd_plugins_load_file(session, "plugins/report.py")) {
  const char *message = neverd_last_error(session);
  /* log message */
  neverd_free_string(message);
}

neverd_plugins_init(session);
int result = neverd_plugins_run(session, "Analysis Report", 0);
neverd_plugins_term(session);
```

`neverd_plugins_list_json` identifies each entry with `"kind":"python"` or
`"kind":"native"`. Directory discovery is sorted by canonical path and accepts
native libraries and Python files in the same directory. Duplicate canonical
paths and duplicate plugin names are errors.

## Session and event API

`Session` revalidates its host capability before every C call. Its typed surface
includes file/architecture/format metadata, bitness and table counts, function
views, loading and analysis, byte reads, disassembly, decompilation, and common
queries. `session.raw` exposes every declaration in `neverd_plugin.abi` for
advanced operations:

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

### Bounded symbolic path exploration

For native LowIR functions, `session.symbolic_explore` returns typed path
outcomes, block traces, resource use, and optional predicates:

```python
result = session.symbolic_explore(
    0x401000,
    max_paths=64,
    max_steps=1 << 16,
    max_block_visits=3,
    include_expressions=True,
)
if not result.exact:
    print(result.unmodelled_ops)
for path in result.paths:
    print(path.outcome, path.blocks, path.predicate)
```

`complete` is false when a path, step, loop, or unresolved-branch bound stops
the walk. `exact` additionally requires that no operation was conservatively
replaced by unknown state; unsupported LowIR operations, calls without
summaries, and stores through unresolved addresses are counted in
`unmodelled_ops`. EVM and SBF sessions do not expose native LowIR exploration.

### Memory-safety audit and hunt

`session.audit()` and `session.hunt()` return parsed JSON reports (the same
schema as the CLI). They require a lifted native session:

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

EVM and SBF sessions reject these calls.

### Proof-gated synthesis and LLVM optimization

`synthesize_expression` is deliberately separate from the legacy MBA-only
`simplify_expression`. A changed result is exposed only after the solver reports
`ProofStatus.EQUIVALENT`; counterexamples, incomplete proofs, and exhausted
search budgets leave the original expression unchanged while preserving their
distinct outcomes and work counters. `ProofStatus.INVALID` identifies a
malformed proof question and remains distinct from budget-driven
`ProofStatus.UNKNOWN`; both fail closed.

`optimize_llvm_ir` parses textual LLVM IR, optimizes a transaction clone with
NeverD's semantic fixed point and the selected standard LLVM pipeline, verifies
the result, and returns only the committed module:

```python
from neverd_plugin import (
    LLVMOptimizationLevel,
    OptimizationMode,
    ProofStatus,
    optimize_llvm_ir,
    synthesize_expression,
)

rewrite = synthesize_expression(
    "(x >> 4) + ((x >> 2) >> 2)", exhaustive=True
)
if rewrite.changed:
    assert rewrite.proof_status is ProofStatus.EQUIVALENT

module = optimize_llvm_ir(
    llvm_ir,
    mode=OptimizationMode.DEEP,
    llvm_level=LLVMOptimizationLevel.O2,
    enable_synthesis=True,
    exhaustive=True,
)
print(module.output_ir, module.semantic_rewrites, module.proof_queries)
```

Budget fields let production callers bound MBA work and arity, synthesis search
and SAT work, and LLVM convergence. For `simplify_expression`, explicit
`exhaustive=True` selects the unlimited MBA arity/work policy and removes the
native parser's nesting and width policy ceilings. For `synthesize_expression`,
it removes parser, search-work, and SAT ceilings while preserving the caller's
grammar. For `optimize_llvm_ir`, it removes convergence, search-work, and SAT
ceilings. Python adds no further expression limit; memory-safety and IR
representation bounds still apply. The equivalent C entry points are
`neverd_simplify_expr`, `neverd_synthesize_expr`, and `neverd_optimize_llvm_ir`,
with typed result disposers and versioned JSON adapters.

The six immutable event variants are `BINARY_LOADED`, `BINARY_CLOSING`,
`FUNCTION_SELECTED`, `ADDRESS_CHANGED`, `ANALYSIS_DONE`, and `PATCH_APPLIED`.
Payload strings are copied during the callback; fields unrelated to a variant
are `None`.

Never store a `Session` for use after termination. The native capsule is
invalidated before `on_term` starts and before the native session can be freed.
A later call fails with `RuntimeError` rather than dereferencing stale memory.

## Errors, isolation, and trust

Python exceptions never unwind through C++. NeverD captures the full formatted
traceback and exposes it through `neverd_last_error`. Each canonical plugin path
loads under a unique module name; termination removes the module, and a later
reload gets fresh module/class state. CPython is initialized once, the bootstrap
GIL is released, callbacks acquire the GIL on any host thread, and NeverD never
finalizes an interpreter it may share with another component.

Plugins execute arbitrary Python in NeverD's process and can call the complete C
API. Load only trusted files. This is an extension boundary, not a sandbox.

## Authoring, tests, and packages

For editor and type-checker support, install the pure-Python package or point
`PYTHONPATH` at the source tree:

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

The audit requires exact parity between every exported C declaration and its
`ctypes` signature/ownership rule. It also checks output-language values,
CMake/package versions, CI feature flags, action pins, artifact flow, and PyPI
OIDC policy. Native adapter tests are `NeverDPluginRuntimeTests`; embedded
Python tests are `NeverDPythonRuntimeTests` and `NeverDPythonPluginTests`.

The `Python Plugin SDK` workflow builds one wheel and source distribution,
installs both into clean environments, and uploads the verified artifacts.
Publishing runs only for a published GitHub release through the approval-gated
`pypi` environment and Trusted Publishing; no long-lived PyPI token is used.
