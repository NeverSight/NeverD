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

Runnable host examples live under `examples/`: `minimal.py`,
`analysis_report.py`, and `semantic_optimizer.py`. The last one exercises both
proof-gated synthesis and the transactional LLVM pipeline through the same API
available to third-party plugins.

Python 3.10 or newer is supported. This package and NeverD are licensed under
the GNU Affero General Public License, version 3 only.
