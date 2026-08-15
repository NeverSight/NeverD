# NeverD Python Plugin SDK

`neverd-plugin` is the official, typed authoring package for Python plugins
loaded by NeverD. The package is pure Python and imports outside the host, so
editors, type checkers, and unit tests do not need a local NeverD library.

```python
from neverd_plugin import Event, Plugin, Session


@Plugin(name="My Plugin", version="1.0.0", author="Your team")
class MyPlugin:
    def on_init(self, session: Session) -> None:
        print(session.architecture)

    def on_run(self, session: Session, arg: int) -> int:
        return arg

    def on_event(self, event: Event) -> None:
        print(event.type.name)

    def on_term(self) -> None:
        pass
```

Install for authoring with `python -m pip install neverd-plugin`. At runtime,
NeverD stages a matching copy beside `libneverd` and injects a private native
bridge. Host-backed `Session` calls outside that context fail with an actionable
exception instead of loading an arbitrary library.

Plugins can request a bounded symbolic walk of a native LowIR function with
`session.symbolic_explore(address)`. The typed result reports whether the walk
was complete and exact, the path outcomes, resource use, and any unmodelled
operations; pass `include_expressions=True` to include path predicates. Calls
without summaries and stores through unresolved addresses are conservative
approximations, so they make `exact` false and contribute to `unmodelled_ops`.

Proof-gated synthesis and transactional LLVM optimization are also available
without a session:

```python
from neverd_plugin import optimize_llvm_ir, synthesize_expression

rewrite = synthesize_expression("(x >> 4) + ((x >> 2) >> 2)")
optimized = optimize_llvm_ir(llvm_ir, enable_synthesis=True)
```

Every committed synthesis rewrite has an equivalent solver proof. Distinct
search and proof counters explain refusals; malformed proof questions report
`ProofStatus.INVALID` instead of the budget-driven `ProofStatus.UNKNOWN`, and
both fail closed. On the expression APIs,
`exhaustive=True` also removes the native parser's nesting and width policy
ceilings; simplification selects the unlimited MBA work/arity policy, while
synthesis removes search and solver ceilings without changing its grammar.
Physical resources and IR representation bounds still apply. LLVM optimization
runs on a transaction clone and exposes only a verified, committed module. The
older `simplify_expression` API remains MBA-only for ABI compatibility.

The SDK also exposes the current cross-architecture object boundary explicitly:

```python
from neverd_plugin import (
    TranslationObjectFormat,
    translate_x86_64_block_to_aarch64_object,
)

translated = translate_x86_64_block_to_aarch64_object(
    b"\x48\x89\xf8\x48\x83\xc0\x01\xc3",
    entry_pc=0x1000,
    executable_generation=0,
    object_format=TranslationObjectFormat.ELF,
)
object_bytes = translated.object_bytes
```

The example is one member of the published, fail-closed x86-64 v1
scalar-register subset. The subset accepts only canonical encodings without
legacy prefixes: REX.W full-width GPR `MOV`, `ADD`/`SUB`, and
`AND`/`OR`/`XOR` forms over supported register/immediate LowIR shapes.
It also accepts full-width register-only `CMP` `39/3B`, register/immediate `CMP`
`81/7`, `83/7`, and `3D`, full-width register-only `TEST` `85`, and
register/immediate `TEST` `F7/0` and `A9`. Arithmetic forms retain their scalar
flag computations;
logical and `TEST` forms compute their architecturally defined flags while
preserving `AF` in the NeverD state model. Canonical `C3`
`RET` or `C2 iw` `RET imm16` terminates a return block, and direct-relative
`EB cb`/`E9 cd` `JMP` terminates a direct-branch block. The published lowering
schema is 9. Canonical, legacy-prefix-free traditional Jcc comprises `JO`/`JNO`
short `70/71 cb` or near `0F 80/81 cd`, `JB`/`JAE` `72/73 cb` or
`0F 82/83 cd`, `JE`/`JNE` `74/75 cb` or `0F 84/85 cd`, `JBE`/`JA`
`76/77 cb` or `0F 86/87 cd`, `JS`/`JNS` `78/79 cb` or `0F 88/89 cd`,
`JP`/`JNP` `7A/7B cb` or `0F 8A/8B cd`, `JL`/`JGE` `7C/7D cb` or
`0F 8C/8D cd`, and `JLE`/`JG` `7E/7F cb` or `0F 8E/8F cd`.
`JRCXZ`/`JECXZ`/`JCXZ` and `LOOP`/`LOOPE`/`LOOPNE` remain unpublished and
fail closed. Reserved `F7 /1`, ordinary guest-memory operations,
partial-register forms, legacy prefixes, semantically redundant REX extension
bits, any instruction or encoding outside that exact subset, all other control
flow, and unimplemented LowIR fail before object emission.
Successful calls return only audited AArch64 ELF or Mach-O relocatable objects.
The returned bytes and identity strings are Python-owned; native allocations
have already been released. This boundary does not link, load, publish,
dispatch, execute, or debug the object.
Native translation failures raise `TranslationError` carrying a
`TranslationErrorCode`; local argument validation raises `TypeError` or
`ValueError` instead.

Runnable host examples live under `examples/`: `minimal.py`,
`analysis_report.py`, and `semantic_optimizer.py`. The last one exercises both
proof-gated synthesis and the transactional LLVM pipeline through the same API
available to third-party plugins.

Python 3.10 or newer is supported. This package and NeverD are licensed under
the GNU Affero General Public License, version 3 only.
