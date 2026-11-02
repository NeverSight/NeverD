# NeverD agent guide

NeverD is a semantics-first binary analysis and decompilation project built on
C++20 and LLVM, with a Qt workbench and C/Python interfaces.

## Task index

Read the guidance matching the task. For large guides, search headings and
relevant symbols with `rg -n`, then read the surrounding section.

| Task | Read first |
| --- | --- |
| Implement or review code | [Implementation principles](docs/development-principles.md), especially validation and speculative defenses |
| Locate code or change shared semantics, ISA/format behavior, or pipeline ownership | [Where to edit](docs/architecture.md#where-to-edit) and the relevant architecture section |
| Configure/build or change dependencies | [Build profiles](CONTRIBUTING.md#choose-a-build-profile) and [dependency refresh behavior](CONTRIBUTING.md#process-allocator) |
| Choose tests, targets, labels, or fixtures | [Test layout](docs/testing.md#test-layout) and [incremental CTest workflow](docs/testing.md#incremental-ctest-workflow) |
| Change C/C++ code, headers, limits, or defaults | [Code style](CONTRIBUTING.md#code-style), including header layout and policy ownership |
| Change a hot path, container, index, parallel loop, cache or threshold; claim or review a speedup | [Performance skill](.agents/skills/neverd-performance/SKILL.md) |
| Change CLI behavior or usage | [CLI usage](README.md#cli) |
| Change GUI, SDK/plugins, or another subsystem | The matching guide in the [documentation index](docs/README.md) |
| Prepare a PR, reuse external code, or update documentation | [Contributing](CONTRIBUTING.md) and [attribution](ATTRIBUTION.md) |
| Prioritize or scope planned product work | [Roadmap](docs/roadmap.md); not required for ordinary fixes |

## Repository map

- `include/neverd/`, `lib/`: interfaces and implementations; loader, decode/lift,
  IR, analysis/passes, pipeline, backends, emulation, and SDK.
- `tools/`: CLI, GUI, workers, and MCP adapters. `plugins/`, `pluginsdk/`:
  plugin implementations and authoring interfaces.
- `unittests/`, `scripts/`: tests, fixtures, build entry point, and checks.
- `third_party/`: dependencies; inspect only when the task involves them.

## Always apply

- Fail clearly on malformed input and unsupported instructions. Never guess
  semantics or substitute a `NOP`. Keep each semantic decision in one owning
  layer and reuse it across architectures, formats, and adapters.
- Validate at the owning boundary; trust established internal guarantees.
  Avoid speculative defensive machinery and premature abstractions. Follow the
  [implementation principles](docs/development-principles.md) for detailed rules.
- Preserve unrelated changes and diagnostic artifacts. Avoid unrelated
  formatting and submodule revision changes.
- Make changes to LLVM itself in the [NeverSight LLVM fork](https://github.com/NeverSight/llvm-project).
  Commit, push and merge them there before updating NeverD's LLVM submodule.
  Do not add LLVM patch files or build-directory source overlays to NeverD.

## Build, verify, finish

Use Release unless the task needs another profile. Use `scripts/build.py` for
compilation; it refreshes the NeverSight dependency forks as documented above.
For an existing Release test build, a lift/loader check looks like:

```bash
python3 scripts/build.py build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
git diff --check
```

Choose the actual target/filter from the testing guide. Run the smallest check
that can disprove the change, fix failures caused by the work, and rerun affected
checks. Broaden coverage for shared semantics, ISA/format changes, or PR review.
Local builds and fixtures are disposable and have no production access.

For implementation requests, continue through verification without asking after
each step. Stop when the user requested diagnosis/review only, or when completion
needs a product decision or authority not already granted. Report the changes,
checks, and unavailable/skipped coverage; update affected documentation.

Keep this entry point short. Put detailed rules in the linked owning document;
add directory-local `AGENTS.md` files only for distinct local requirements.
