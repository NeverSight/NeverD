# NeverD Daily Progress

## Current snapshot — 2026-10-17

- Review source pinned at **01:00 UTC / 09:00 Asia/Shanghai** to dev [3264a443](https://github.com/NeverSight/NeverD/commit/3264a443fdbcae22b1641a474e6538f65d3b44ec).
- Incoming [206fe6f1…3264a443](https://github.com/NeverSight/NeverD/compare/206fe6f1a4ceec63aee93637238c24ce7579b7a1...3264a443fdbcae22b1641a474e6538f65d3b44ec): **408 commits / 892 changed paths** (261 added, 629 modified, 2 removed). Commit pagination was 100 + 100 + 100 + 100 + 8. Both recursive Git trees were complete; their non-directory entries provide the path inventory without the comparison endpoint's 300-file limit. Inventory is not source-review coverage or delivered-feature acceptance.
- **Two statically confirmed defects repaired**: compressed-debug expansion padding in draft [#1212](https://github.com/NeverSight/NeverD/pull/1212), source [f2364df4](https://github.com/NeverSight/NeverD/commit/f2364df414921d033192dc21f43b88a6e96a011c) and unexecuted regression [8d53af79](https://github.com/NeverSight/NeverD/commit/8d53af79a37df4ab5cee283d1d7b8aac91812917); source-patch ABI audit mapping in draft [#1214](https://github.com/NeverSight/NeverD/pull/1214), [14b03974](https://github.com/NeverSight/NeverD/commit/14b03974e16007f9e8fd16183764198a2f679349). Source/test changes total **3 files, +37/-0** across the two focused branches. Remote full-content readbacks and commit diffs matched the intended edits.
- Strict static-only work: source, diffs, callers, interfaces, configuration and existing records. **No builds, tests, benchmarks, project execution, repository scripts, formatters, dynamic analysis, manual CI dispatch or reruns.** Regression source was authored, not executed. Automatically triggered CI is separate external evidence.
- Prior [#1109](https://github.com/NeverSight/NeverD/pull/1109) merged externally at October 16 03:44:47 UTC as [f8e0488d](https://github.com/NeverSight/NeverD/commit/f8e0488d29db7b46c181fa3c6d83be2e60b74edd). Current Python type-conversion ownership declaration and retained file-operation validation remain present; presence is not fresh execution validation.
- **Concurrent development is explicit:** dev advanced first to [30661538](https://github.com/NeverSight/NeverD/commit/30661538b2e01d96848a0176a8edaff5101cc0c5), 13 additional commits / 102 paths after the pinned head, then to [a35311af](https://github.com/NeverSight/NeverD/commit/a35311afeafbad097e44b56da9051f9ff0c4218d). The broad post-pin source-patch integration was not audited. Its newly failing ABI audit received the separate bounded review and fix described below, based on a35311af. At 01:07 UTC, the compressed-section source, regression file and PROGRESS blobs on dev remained unchanged from the pinned versions. The two PRs avoid importing unrelated integration work into the earlier debug fix.

## Activity and review queue

At the initial 01:01 UTC collection: **54 ordinary open issues / 1 open PR**, versus yesterday's publication snapshot of 50 / 5. Since **October 16 01:08 UTC**, the initial collection contained **4 issues opened / 0 closed; 98 PRs opened / 102 merged / 0 closed without merge**.

At **01:07 UTC**, the all-state PR refresh contained **3 open PRs**, **101 openings / 103 merges / 0 closed without merge** in the same interval. The increase comprises today's two draft repair PRs and external [#1213](https://github.com/NeverSight/NeverD/pull/1213), which opened and merged during review. Ordinary issue count remains 54. Counts are point-in-time activity, including merges and documentation, not a count of qualified product features. Report-only changes are excluded from implementation progress.

- New ordinary issues: [#1118](https://github.com/NeverSight/NeverD/issues/1118) TILE-Gx, [#1128](https://github.com/NeverSight/NeverD/issues/1128) Dart/Flutter, [#1129](https://github.com/NeverSight/NeverD/issues/1129) Squirrel/AngelScript/Pawn, and [#1189](https://github.com/NeverSight/NeverD/issues/1189) an x86 lifting memory-growth report. The first three are planning. #1189 has no replies at observation; its cause is unconfirmed. No linked third-party binary was downloaded or executed, and today's debug repair is not asserted to resolve it.
- All five PRs pending at yesterday's publication merged externally. Current implementation themes include native debug-file ranking/discovery [#1161](https://github.com/NeverSight/NeverD/pull/1161), broader type/code rendering, Windows driver ownership, Darwin/Android runtime and PE32 unwind work. Only the scope below received source review.
- [#1211](https://github.com/NeverSight/NeverD/pull/1211), head **69fc0cdb196dd801c7d6c47d0f5580c3d4bdcffd**, is non-draft. It updates decimal expectations after hexadecimal rendering; the PR's claimed local test totals are author-reported. No requested reviewers, submitted reviews, comments or inline review threads were returned. Its exact-head CI was incomplete, so merge readiness is not established.
- [#1212](https://github.com/NeverSight/NeverD/pull/1212), source/test head **8d53af79a37df4ab5cee283d1d7b8aac91812917**, is draft. No requested reviewers, submitted reviews or inline threads were returned. The API reported mergeable but unstable; that is not approval or a full CI pass.
- [#1214](https://github.com/NeverSight/NeverD/pull/1214), head **14b03974e16007f9e8fd16183764198a2f679349**, is draft. Review and exact-head validation remain outstanding. This isolated checker fix depends on the later source-patch API already present in its dev base.

## Bounded static review and fixes

Read root AGENTS.md, CONTRIBUTING.md, SECURITY.md, the relevant architecture/testing contracts, and debug fixture/build configuration. Complete pinned-tree discovery found no nested AGENTS.md. Dev was reported unprotected and repository rulesets were empty; CONTRIBUTING nevertheless requires a focused topic branch and PR against dev, which both fixes follow. No protection, dependencies, issue fields, merges or deployment were changed.

### Compressed debug section expansion

**Confirmed:** [lib/debug/CompressedSections.cpp](lib/debug/CompressedSections.cpp), original expansion block at lines 80–100, counted decoded bodies against the shared expansion budget, but separately grew the output for section alignment. A compressed section whose alignment needs excessive padding could therefore cause output allocation beyond that budget.

**Repair:** [f2364df4](https://github.com/NeverSight/NeverD/commit/f2364df414921d033192dc21f43b88a6e96a011c) adds six lines: refuse non-monotonic or container-unrepresentable growth, require padding to fit the remaining expansion budget, and charge accepted padding to the same total before resizing. No compressor, dependency, public option or ordinary layout policy was changed. This is a resource-bound correctness finding; no host exploitability or memory-corruption conclusion is claimed.

**Source-only regression:** [8d53af79](https://github.com/NeverSight/NeverD/commit/8d53af79a37df4ab5cee283d1d7b8aac91812917) adds a focused refusal assertion using the existing ELF64 zlib companion fixture, whose producer recipe was read only. The existing normal-expansion case remains unchanged. Source/test aggregate is two files, +33/-0. The new case and the existing suite have not been run by this review.

Full-file source/interface inspection:
- include/neverd/debug/CompressedSections.h, DebugFileIdentity.h, DebugInfoDiscovery.h and MapFile.h
- lib/debug/CompressedSections.cpp, DebugFileIdentity.cpp and DebugInfoDiscovery.cpp
- lib/debug/map/MapFile.cpp and LLDMapLoader.cpp
- lib/sdk/capi/NeverDCAPIDebugFiles.cpp
- lib/debug/CMakeLists.txt, unittests/debug/CMakeLists.txt and unittests/debug/fixtures/Makefile (configuration/source only)

Selected caller, contract and fixture ranges:
- DWARFLoader.cpp 295–635: ownership, object/slice selection, expansion callers, snapshot and companion trust
- DebugInfoDiscoveryTests.cpp 1–100 and 1710–2209: discovery, MAP ranking/mismatch, companion identity, compressed DWARF and named-file cases
- Limits.h 27–44; architecture.md 76–123; testing.md 126–151
- SafetyIntegrationTests.cpp 699–715: explicit native-debug decline for the MAP-only case; current abi.py 1115–1125 and KernelModelIO.cpp 1524–1544 to reconcile prior repairs
- .github/workflows/ci.yml 1–160: triggers, concurrency and platform setup
- Pinned LLVM submodule 06788fb1, llvm/Support/MathExtras.h 463–489: alignTo's documented modulo arithmetic; read-only dependency inspection

Observed invariants include named-file error propagation, native debug precedence over a named MAP supplement, companion names/lines remaining separate from authenticated object extents, reloading against the original image bytes before publishing a selected debug file, and one shared C++ discovery owner for frontend callers. No additional defect was proven in these inspected paths.

### Post-pin Python source-patch ABI audit

The new integrated C API and ctypes options both append the dependency pointer/count and agree on the dependency structure's library/symbol/kind fields. However, scripts/check_python_plugin_sdk.py:308–337 did not map that pointer spelling and did not audit the dependency structure itself. types.get therefore yields None for a valid field, guaranteeing a false mismatch.

[14b03974](https://github.com/NeverSight/NeverD/commit/14b03974e16007f9e8fd16183764198a2f679349) adds the missing pointer mapping and structure inventory entry, exactly **one file, +4/-0**. The public ABI, ctypes layout and native source-patch behavior are unchanged. Read source at dev 30661538: check_python_plugin_sdk.py imports/parser and 290–350, abi.py 767–813, NeverDCAPIPatch.h 210–275, scripts/tests/test_check_python_plugin_sdk.py 1–80. Fresh target-blob checks, full remote readback and commit-diff inspection verified publication. The existing standalone-audit regression source was inspected, not executed.

### Unconfirmed reports and uncovered scope

#1189's observed memory growth is a user report, not a confirmed leak diagnosis and not linked by evidence to today's compressed-debug repair. No other defect is claimed.

Most of the 892 incoming paths and the post-pin 102-path integration remain unaudited. Exclusions include full DWARF DIE/type semantics, every MAP dialect parser, whole-session concurrent mutation behavior, all architecture/loader/SEH/driver/runtime changes, complete source-patch relocation/dependency/callback semantics, native fixtures and application corpus, GUI interactions, LLVM internals beyond the cited helper, and source outside the explicit ranges. No claim is made about build/link success, runtime correctness, formatting compliance, native acceptance, memory benchmarks or release readiness.

## Existing CI evidence and limits

All rows are existing external runs; none was dispatched or rerun by this review.

| Commit and observation | Existing evidence |
|---|---|
| Pinned dev **3264a443**, 01:01 UTC | [CI 38987873181](https://github.com/NeverSight/NeverD/actions/runs/38987873181) in progress; [LLVM Style 38987873341](https://github.com/NeverSight/NeverD/actions/runs/38987873341) success; [Mobile Decompilation 38987873489](https://github.com/NeverSight/NeverD/actions/runs/38987873489) queued; Mobile Real Applications skipped. 26 checks: 6 success, 15 skipped, 4 in progress, 1 queued. |
| Later dev **30661538**, 01:05 UTC | [Python Plugin SDK 38989232035](https://github.com/NeverSight/NeverD/actions/runs/38989232035) had a failed Python 3.11 job while other jobs were pending; [CI 38989231982](https://github.com/NeverSight/NeverD/actions/runs/38989231982) queued. Seven workflows; 33 checks: 1 failure, 15 skipped, 4 in progress, 13 queued. |
| PR **#1211 / 69fc0cdb**, 01:05 UTC | [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/38988837828) success, [CI](https://github.com/NeverSight/NeverD/actions/runs/38988837824) in progress, [Mobile](https://github.com/NeverSight/NeverD/actions/runs/38988837849) queued. 17 checks: 4 success, 6 skipped, 5 in progress, 2 queued. |
| PR **#1212 / 8d53af79**, 01:05 UTC | [CI 38989294380](https://github.com/NeverSight/NeverD/actions/runs/38989294380), [Mobile 38989294331](https://github.com/NeverSight/NeverD/actions/runs/38989294331), [LLVM Style 38989294444](https://github.com/NeverSight/NeverD/actions/runs/38989294444) queued. 17 checks: 6 skipped, 11 queued. This is the source/test head, not the subsequent documentation-only head. |
| PR **#1214 / 14b03974**, 01:07 UTC | [Python Plugin SDK 38989475752](https://github.com/NeverSight/NeverD/actions/runs/38989475752), [CI 38989475838](https://github.com/NeverSight/NeverD/actions/runs/38989475838), [Mobile 38989475764](https://github.com/NeverSight/NeverD/actions/runs/38989475764) and [LLVM Style 38989475655](https://github.com/NeverSight/NeverD/actions/runs/38989475655) queued at workflow level. 22 checks: 6 skipped, 15 queued, 1 in progress. Workflow/job aggregation can differ temporarily. |

The [Python 3.11 log at 30661538](https://github.com/NeverSight/NeverD/actions/runs/38989232035/job/117267639145) identifies the exact source-patch options audit mismatch described above. Its first suite reports 213 tests with 113 skips and no failures; the separate audit suite then reports one failure among 10 cases. These are results of that existing external run, not this review's execution and not proof of the new fix. Native-EH and other skipped checks remain skipped. Neither older successes nor mergeability establish current acceptance. Later dev a35311af and the final documentation commit are not qualified by the older samples.

## Suggested next priorities

1. **Review and qualify the two bounded repairs.** Dependencies: maintainer review and exact-head automatic or separately authorized validation for #1212 and #1214. Acceptance: compressed-debug normal/refusal cases and the standalone SDK audit have explicit results on identified commits; failures/skips remain itemized. Do not transfer a pre-fix result to the repair.
2. **Triage #1189's x86 lifting memory growth with bounded ownership evidence.** Dependencies: an approved, safely obtained reproducer and an explicitly authorized execution environment if dynamic evidence becomes necessary. Static next step: trace retained loader/lift/pipeline objects and the latest incoming changes without assuming a leak cause. Acceptance: identify a specific owning path and bounded criterion, or retain an explicit unresolved report; today's allocation guard alone does not close it.
3. **Reconcile current integration and shared frontend acceptance.** Dependencies: current #1211 results and exact source/host evidence for the post-pin source-patch integration. Acceptance: record one identified integration head with terminal required results; retain shared first-party C++ semantic ownership across CLI/SDK/GUI and make unsupported/dependency-limited profiles explicit. Feature issue openings and documentation commits do not count as shipped capability.

## Daily log — 2026-10-17

- Inventoried 408 commits / 892 paths since the prior reviewed dev; reviewed the bounded debug discovery, compressed-section and selected frontend contracts above.
- Published one six-line allocation-budget repair plus unexecuted regression in draft #1212, and a separate four-line SDK audit repair for the concurrent later integration in draft #1214.
- Reconciled the external merge of prior #1109, four new ordinary issues, and the initial/final PR snapshots with explicit observation times.
- Read the existing later-dev Python audit failure and distinguished it from both new PR heads and pending/skipped native checks.
- Added this English snapshot and daily entry, preserving the complete prior document and human edits verbatim below. Documentation-only work is not implementation progress.

## Collection and preservation limits

- Initial open-issue endpoint returned 55 records (54 ordinary plus one PR), then empty. Updated-since pagination was 100 + 16 + 0. Initial and final all-state PR collections each read two 100-record pages, reaching before the cutoff; interval counts are complete for those snapshots.
- Each sampled workflow/check collection returned its full declared total; second pages were empty. These are exact-head samples, not all historical CI or external status systems.
- Counts can change after observation. The newly opened repair PRs are included only in the 01:07 publication queue, not the initial baseline.
- Latest dev and target-branch PROGRESS were read in full immediately before replacement and matched; previous text is retained unchanged. The new documentation is proposed in #1212 and is not asserted to be merged.

<details>
<summary>Previous snapshots (preserved verbatim)</summary>

# NeverD Daily Progress

## Current snapshot — 2026-10-16

- Source pinned to dev [206fe6f1](https://github.com/NeverSight/NeverD/commit/206fe6f1a4ceec63aee93637238c24ce7579b7a1), verified unchanged at **01:08 UTC / 09:08 Asia/Shanghai**. Issue/PR activity was collected at 01:02–01:03 UTC; exact-head CI samples below retain their own observation times.
- Incoming range [3273beda…206fe6f1](https://github.com/NeverSight/NeverD/compare/3273bedaff24518dd40bc06febbf76a8dd47abb5...206fe6f1a4ceec63aee93637238c24ce7579b7a1): **423 commits / 1,402 changed paths** (632 added, 717 modified, 53 removed). Commits were paginated 100 + 100 + 100 + 100 + 23. Both recursive Git trees were complete; comparing non-directory entries avoids the comparison API's 300-file cap. This is inventory, not source-review coverage or completed product work.
- **Two statically proven correctness defects repaired** in draft [#1109](https://github.com/NeverSight/NeverD/pull/1109): a missing Python ABI declaration and retained file-operation access validation. Source commits [4339a1fb](https://github.com/NeverSight/NeverD/commit/4339a1fb53051a82377d2c1825a02cd80d1d7a7b) / [d9747a4d](https://github.com/NeverSight/NeverD/commit/d9747a4d6f052bbdd723f7075d4d0881a841fd93), plus unexecuted regression-source commit [464b1d72](https://github.com/NeverSight/NeverD/commit/464b1d72f01266e5ea1a1e92d0da3f446fe6365b), total **3 files, +13/-1**. Remote full-content readbacks and aggregate diff matched the intended edits.
- Strict static-only scope: source, diffs, callers, interfaces, configuration and existing records. **No build, test, benchmark, project execution, repository script, formatter, dynamic analysis, manual CI dispatch or rerun.** Automatically triggered checks are external evidence and were not performed by this review.
- Yesterday's [#987](https://github.com/NeverSight/NeverD/pull/987) merged externally at October 15 01:30:28 UTC as [1d13910f](https://github.com/NeverSight/NeverD/commit/1d13910f6905a7d2ea35754bee241e93ff590385). Its read-placement guard remains in the current source; that presence is not new runtime validation. No relevant open daily-review PR remained to reuse. Report-only changes are excluded from implementation progress.

## Activity and review queue

Before today's publication: **50 ordinary open issues / 4 open PRs**, versus yesterday's publication snapshot of 26 / 1. Since **October 15 01:11 UTC**, **29 issues opened / 5 closed; 92 PRs opened / 89 merged / 0 closed without merge**. These are activity counts including merges and documentation, not a count of independently qualified features. Today's #1109 adds one draft PR, making the observed publication queue 50 issues / 5 PRs.

- The 29 new issues within [#1021](https://github.com/NeverSight/NeverD/issues/1021)–[#1053](https://github.com/NeverSight/NeverD/issues/1053) describe game/language, model-artifact, binary-format, Android and iOS roadmaps. They are **planning, not implementation delivery**.
- [#714](https://github.com/NeverSight/NeverD/issues/714), [#715](https://github.com/NeverSight/NeverD/issues/715), [#716](https://github.com/NeverSight/NeverD/issues/716), [#717](https://github.com/NeverSight/NeverD/issues/717), and [#718](https://github.com/NeverSight/NeverD/issues/718) closed. Closure alone does not establish full acceptance; their broad JavaScript/web scope was not audited here.
- Merged implementation examples include Windows file-provider and lifetime work [#1098](https://github.com/NeverSight/NeverD/pull/1098), [#1101](https://github.com/NeverSight/NeverD/pull/1101), [#1105](https://github.com/NeverSight/NeverD/pull/1105), [#1107](https://github.com/NeverSight/NeverD/pull/1107), local/header types [#1104](https://github.com/NeverSight/NeverD/pull/1104), and PE32 nonlocal unwind [#1106](https://github.com/NeverSight/NeverD/pull/1106). Only the bounded paths below received source review.

Open PR evidence at 01:03 UTC, before #1109:

| PR | Exact head | State and existing check sample |
|---|---|---|
| [#1108](https://github.com/NeverSight/NeverD/pull/1108) | a6126ed776b4eb88a9283836a95050bfb381c58f | Draft; 2 success, 10 skipped, 3 in progress, 9 queued |
| [#1103](https://github.com/NeverSight/NeverD/pull/1103) | a23c299f2ad01ee608be0b0a6358ffabe01cc6b4 | Non-draft; 7 success, 6 skipped, 4 in progress |
| [#1063](https://github.com/NeverSight/NeverD/pull/1063) | 5066e6b130d514494a817ef8fdb6531511bdf3f6 | Draft; 4 success, 5 failure, 6 skipped, 4 in progress, 3 queued |
| [#1007](https://github.com/NeverSight/NeverD/pull/1007) | b8187a3e36c630ef160d129ec6caec9a43b89de7 | Draft; 7 success, 4 failure, 6 skipped |

All four had no requested reviewers, submitted reviews or inline review threads returned. Each exact-head check response returned its complete total (24, 17, 22 and 17 respectively). Failed causes in those PR samples were not exhaustively diagnosed. PR descriptions' local/Wine/test totals remain **author-reported**; they are not executions performed here. #1063 explicitly retains current r6 qualification and older failed full-CI limitations; #1108 retains ongoing broad qualification. None of these samples establishes merge readiness.

Statistics coverage: open issues returned 54 records (50 ordinary plus 4 PRs), then empty; open PRs returned 4, then empty. Updated-since issue pagination was 100 + 34 + 0. Two all-state PR pages of 100 reach October 12, before the cutoff, and cover the 93 PRs updated in this interval. Counts can change after observation.

## Bounded static review and fixes

Read root AGENTS.md, CONTRIBUTING.md and SECURITY.md, relevant architecture/testing contracts and Python SDK guidance. The complete current tree contains only root AGENTS.md. Dev reports unprotected and repository rulesets were empty; the contribution guide still calls for a topic branch and PR against dev, which this update follows. No protection, dependency, issue, merge or deployment mutation was performed.

### Python type-conversion ABI

The public [NeverDCAPILocalTypes.h](include/neverd/sdk/NeverDCAPILocalTypes.h) declares a non-session `const char *` argument and an owned-string result for `neverd_types_convert`. Its implementation allocates the response, but the Python declaration table omitted that function. [ffi.py](pluginsdk/python/neverd_plugin/ffi.py) requires an inventory entry before binding, and its owned-string path retains and releases the original pointer.

The six-line declaration in [4339a1fb](https://github.com/NeverSight/NeverD/commit/4339a1fb53051a82377d2c1825a02cd80d1d7a7b) matches that signature and ownership. Existing inventory/ownership audit source covers the contract; no new native conversion semantics or public option was introduced.

Inspected: full public local-types header and ffi.py; abi.py declaration/type-binding sections (913–1095 and the relevant scalar-type mappings); NeverDCAPILocalTypes.cpp conversion implementation (650–723); test_abi.py inventory test (576–587); check_python_plugin_sdk.py public-header traversal and ownership audit (1–91, 214–253); Python-plugin ownership/API guidance and the architecture's local-types boundary. These are bounded ranges, not a full audit of type parsing or every Python declaration.

### Windows file-provider and retained-packet lifecycle

The minimal condition change in [d9747a4d](https://github.com/NeverSight/NeverD/commit/d9747a4d6f052bbdd723f7075d4d0881a841fd93) keeps existing access validation active for retained producer-owned file-operation packets. [464b1d72](https://github.com/NeverSight/NeverD/commit/464b1d72f01266e5ea1a1e92d0da3f446fe6365b) adds six assertion lines to the existing lifecycle fixture, preserving allowed reads and final retirement checks. This is a model-correctness/admission repair; host exploitability has not been established. Detailed triggering analysis is retained in the private report. The regression source was authored and inspected, **not executed**.

Full-file inspection:
- include/neverd/emulation/DriverFileSystem.h and DriverFileSystemLimits.def
- lib/emulation/os/windows/driver/DriverFileSystemJSON.cpp
- lib/emulation/os/windows/kernel/KernelFileSystem.h/.cpp, KernelModelFileCreate.cpp, KernelModelFileObjects.cpp, KernelModelFileSystem.cpp, KernelModelFrameworkRequests.cpp
- unittests/emulation/KernelFileCreateTests.cpp

Selected caller/interface/configuration/test ranges:
- KernelModel.h 1241–1362; KernelModel.cpp 740–981; KernelModelIO.cpp 430–622, 690–925, 929–1375, 1380–1571
- KernelModelObjects.cpp 1–225; KernelModelNamespace.cpp 78–290; KernelModelIRPStack.cpp 86–265 and 617–618; KernelModelWaits.cpp 201–236; KernelCalls.cpp 448–470
- KernelAPIs.def 22–25; KernelAPIIRQL.def 259–262; driver/DriverSession.cpp 392–515, 1440–1465, 1670–1735
- DriverFileSystemTests.cpp 376–612; DriverFileObjectTests.cpp 1–343; emulation/CMakeLists.txt 1354–1388; DriverBackendParityCases.def 1–86
- scripts/WDKDriverFixtures.def 75–95 and NativeDriverTests.def 1–150, read only
- docs/architecture.md 6689–6733 and docs/testing.md 7101–7125, 7149–7161
- Metadata and changed-file inventories for #1098/#1101/#1105/#1107; selected #1105 IO/framework patches

Sharing ownership, cleanup release, failed-CREATE cancellation requirements, relative-root retention and final-reference close already have explicit checks in this head. No additional defect is established in those inspected paths. Also re-read the current COFFRegistrationSEHJumpArguments.cpp placement-guard section (85–103) to reconcile yesterday's merged repair; this is not a renewed full PE32 audit.

### Excluded scope

Most of the 1,402 changed paths were not audited. Exclusions include complete query-I/O/device-topology/allocator/physical-memory internals, full native inventories and fixture binaries, broad SDK/CLI transport integration, type-parser semantics beyond the ABI boundary, LLVM/submodule internals, broad PE32/SEH/Darwin/Swift and JavaScript/web changes, localized documentation and the new roadmap's future implementations. Static review does not establish Windows-kernel equivalence, runtime behavior, native platform acceptance, build/link success, formatting compliance or release readiness.

## Existing CI evidence and limits

At **01:02 UTC**, exact dev **206fe6f1** had four workflows:
- [CI 38829888855](https://github.com/NeverSight/NeverD/actions/runs/38829888855): failure.
- [LLVM Style 38829888871](https://github.com/NeverSight/NeverD/actions/runs/38829888871): success.
- [Mobile Decompilation 38829888823](https://github.com/NeverSight/NeverD/actions/runs/38829888823): queued at workflow level; Linux job success, macOS in progress, Windows queued.
- [Mobile Real Applications 38829909481](https://github.com/NeverSight/NeverD/actions/runs/38829909481): skipped.

Exact-head checks: **26 = 6 success + 3 failure + 15 skipped + 1 in progress + 1 queued**. Both workflow and check collections had empty second pages. Several native-EH gates were skipped; ARM64 backend build successes do not establish native execution.

The Linux, macOS and Windows main-CI logs all identify the same missing `neverd_types_convert` ABI inventory entry. The failed step is named "Verify Debug and Release target flags", but the terminating failure is the Python inventory assertion, not evidence of wrong target flags. Each existing log reports 206 Python tests, one failure and 106 skips. These are external run results at the old dev head, not validation of the new fix.

At **01:08 UTC**, automatic PR workflows for source/regression head **464b1d72** were [CI](https://github.com/NeverSight/NeverD/actions/runs/38833415718), [Python Plugin SDK](https://github.com/NeverSight/NeverD/actions/runs/38833415728) and [Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/38833416009) queued, [Source patch Windows runtime](https://github.com/NeverSight/NeverD/actions/runs/38833415724) pending, and [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/38833415727) in progress. They do not establish a pass, nor qualify the later documentation-only head. No workflow was dispatched or rerun manually.

## Suggested next priorities

1. **Review and qualify the Python binding repair in #1109.** Dependency: the exact-head automatic SDK/platform results and maintainer review. Acceptance: inventory/signature/owned-string checks complete on the identified commit; all failed or skipped gates remain itemized. An older green run or current queued run is insufficient.
2. **Qualify retained file-operation permissions and lifecycle.** Dependency: independent authorized test execution and familiarity with producer/completion ownership. Acceptance: the added read/write assertions, retained cancel-open coverage and existing final-retirement cases have exact commit/platform results. No host-security or whole-driver correctness claim follows from this bounded repair.
3. **Separate integration gates from the expanded roadmap.** Dependency: current heads/reviews for #1007/#1063/#1103/#1108 and acceptance evidence for the closed web issues. Acceptance: choose one bounded remaining item with a dependency and observable criterion; record native/skipped evidence precisely. The 29 new planning issues must not count as shipped work.

## Daily log — 2026-10-16

- Inventoried 423 incoming commits / 1,402 paths and completed the bounded Python ABI plus Windows file-lifetime review above.
- Published two minimal correctness repairs and unexecuted regression assertions in draft #1109; verified exact remote file contents and the three-file +13/-1 aggregate source/test diff.
- Reconciled yesterday's external #987 merge, 29 new/five closed issues, 92 opened/89 merged PRs, and four pre-existing review queues.
- Diagnosed the three old-dev CI failures to the missing ABI inventory entry using existing logs; did not run or rerun validation.
- Added this English snapshot and daily entry, preserving the complete previous text and human edits verbatim below. Documentation-only work is not business implementation progress.

<details>
<summary>Previous snapshots (preserved verbatim)</summary>

# NeverD Daily Progress

## Current snapshot — 2026-10-15

- Cutoff: **2026-10-15 01:11 UTC / 09:11 Asia/Shanghai**. Source review pinned to dev [3273beda](https://github.com/NeverSight/NeverD/commit/3273bedaff24518dd40bc06febbf76a8dd47abb5); today's isolated fix is in draft [#987](https://github.com/NeverSight/NeverD/pull/987).
- Incoming [841dc099…3273beda](https://github.com/NeverSight/NeverD/compare/841dc099a973dcf2196bcbc5acb9d4710400de7b...3273bedaff24518dd40bc06febbf76a8dd47abb5): **248 commits / 715 changed file paths** (246 added, 469 modified, none removed). Commit pagination was 100 + 100 + 48. Complete endpoint Git trees, excluding directories, replaced the comparison API's 300-file cap. This is an inventory, not a claim that every path received line-by-line review.
- **One confirmed source-read placement defect repaired**, with remote source readback and exact diff inspection. Source commits [7032d2f9](https://github.com/NeverSight/NeverD/commit/7032d2f96948792e76c7670cdc5d6d9effd8b7ec) and [c9025b27](https://github.com/NeverSight/NeverD/commit/c9025b279e2ed8fa21249652ba36f9bce21286c6) total one file, +13/-0; the second only wraps the guard. No execution evidence is claimed.
- Strictly source/diff/caller/interface/configuration and existing-record inspection. **No build, test, benchmark, project execution, repository script, formatter, dynamic analysis, manual CI dispatch or rerun.** Automatic checks caused by publishing are external results, not validation performed by this review.
- Prior report [#956](https://github.com/NeverSight/NeverD/pull/956) merged externally at October 14 01:03:25 UTC as [55d5f368](https://github.com/NeverSight/NeverD/commit/55d5f3688f8de5e0ced29026545968d6840deafa). It was closed before this review, so a new dedicated draft PR was required. Report-only work is excluded from business progress.
- The previous download fixes 0da5403b and 6d3c11e1 remain present in unchanged source blobs. Their presence is not new validation of the filesystem/network paths.

## Activity and review queue

**26 ordinary open issues / no pre-existing open PRs**, versus 29 / 20 in the previous snapshot. All 20 previously pending PRs merged externally. Today's new draft #987 makes the publication queue **26 issues / 1 draft PR**, with no requested reviewers or submitted reviews at 01:09 UTC.

Since October 14 01:01 UTC, before today's new draft: **29 PRs opened / 49 merged / 0 closed without merge**. These include report #956; excluding it gives **28 opened / 48 merged**. Today's #987 adds one opening and no merge. No merge was performed or requested by this review.

Two ordinary issues opened and five closed: new [#963](https://github.com/NeverSight/NeverD/issues/963) and [#973](https://github.com/NeverSight/NeverD/issues/973) are already closed; [#858](https://github.com/NeverSight/NeverD/issues/858), [#946](https://github.com/NeverSight/NeverD/issues/946), and [#947](https://github.com/NeverSight/NeverD/issues/947) also closed. Related author changes include [#958](https://github.com/NeverSight/NeverD/pull/958), [#974](https://github.com/NeverSight/NeverD/pull/974), and [#981](https://github.com/NeverSight/NeverD/pull/981). Issue closure is not proof of every acceptance criterion: the #858 discussion and [#966](https://github.com/NeverSight/NeverD/pull/966) still recorded a failing reconstruction path, followed by the source-only approach in [#969](https://github.com/NeverSight/NeverD/pull/969).

Incoming implementation themes include Darwin/Swift integration [#962](https://github.com/NeverSight/NeverD/pull/962), kernel counted-string/runtime models [#976](https://github.com/NeverSight/NeverD/pull/976) / [#983](https://github.com/NeverSight/NeverD/pull/983), ABI repairs #974/#981, and PE32 registration-state/nonlocal-transfer work [#984](https://github.com/NeverSight/NeverD/pull/984)–[#986](https://github.com/NeverSight/NeverD/pull/986). Their test descriptions are author reports unless tied to an exact recorded workflow below.

Statistics coverage: pre-publication open issues returned 26 then an empty page, and open PRs zero then empty. After #987, open issues returned 27 records (26 ordinary plus this PR), open PRs one, and both second pages were empty. Updated-since issue pagination returned 54 then empty; the 100 most recently updated all-state PR records extend before the cutoff and cover the interval. Counts may change after the stated snapshot.

## Bounded static review

Read root AGENTS.md and CONTRIBUTING.md before source work, the relevant architecture/testing contracts and roadmap. Complete tree discovery found no nested AGENTS.md. Execution instructions yield to the explicit static-only constraint. No usable connected authorized computer or saved coding environment was available; connector-based static inspection and minimal edits were used. Dev is reported unprotected and repository rulesets returned empty, but contribution guidance still requires a topic branch and PR against dev. No issue fields, dependencies, security settings or deployment were changed.

### PE32 registration-state and native SEH transfer

Full-file inspection at the pinned SHA covered:

- include/neverd/backend/llvm/X86RegistrationSEHJump.h
- lib/backend/llvm/X86/RegistrationSEHJump.cpp, RegistrationSEHJumpStack.cpp, RegistrationSEHJumpCallbackFrame.cpp
- lib/backend/llvm/X86/MedLLVMRegistrationSEHJump.cpp and .h, MedLLVMRegistrationSEHJumpCallback.cpp, MedLLVMRegistrationSEHProof.cpp, MedLLVMNativeRegistration.cpp
- lib/ir/low/X86/RegistrationStateJump.cpp
- lib/backend/codegen/COFF/COFFRegistrationSEHJump.cpp and .h, COFFRegistrationSEHJumpArguments.cpp, COFFRegistrationSEHJumpRuntime.cpp, COFFRegistrationSEHNullAccess.cpp
- lib/backend/codegen/COFF/COFFRegistrationFrameProof.cpp, COFFRegistrationIR.cpp, COFFRegistrationIRProof.cpp and .h, COFFNativeEHProvenance.h
- unittests/lift/eh/RegistrationSEHJumpNativeTests.cpp, RegistrationSEHNullAccessTests.cpp, RegistrationSEHFrameProofTests.cpp, fixtures/registration_jump.s

Patch-level inspection additionally covered #984–#986 state receipts and publication/invalidation, caller ABI, conditional-callee invalidation, capture/solver interfaces, shared COFF copy-edge extraction and C++ consumer, incoming-frame setup, emitter/PipelineOptimize integration, callback planning/private-stack interfaces, WindowsRegistrationFrame, MedCFGSimplify, SEH exception integration, focused fixture changes and reconstruction/testing documentation. A patch review is not a full-file audit of these additional consumers.

**Confirmed and fixed:** COFFRegistrationSEHJumpArguments.cpp, original lines 74–84, did not completely bind an accepted resume-value read to the canonical generated call sequence. The minimal added guard enforces that placement while permitting the single independently authenticated protected-invoke provenance anchor. Detailed trigger analysis is retained for the private report rather than publishing adversarial construction details.

Static proof includes the emitter order in MedLLVMRegistrationSEHJump.cpp:209–222, old-call removal in MedLLVMNativeRegistration.cpp:625–632, protected-invoke anchor insertion at 674–695, independent anchor authentication in COFFRegistrationIR.cpp:328–334 and 419–432, and next() debug/pseudo skipping in COFFRegistrationIRProof.h:24–28. The repair adds no new dependency or accepted lowering form. Remote full-file readback matched the intended content and the base-to-fix diff contains exactly the one source file, +13/-0. It has **not** been compiled or exercised; a focused regression and platform qualification remain outstanding.

**Separate unresolved question:** ABI attribute admission needs a broader caller/normalization trace. This is not a second confirmed bug, has no patch, and must not be treated as an exploitability or correctness conclusion.

### Kernel runtime counted strings and pool ownership

Read complete current KernelModelRuntime.cpp/.h, KernelRuntimeCalls.cpp, KernelRuntimeStringAPIs.def, KernelRuntimeLimits.def, KernelDebugFormats.def, KernelDebugLengths.def and the driver_strings.c fixture. Read relevant ranges of KernelCalls.cpp (1–245 and 396–415), KernelPoolCalls.cpp, KernelMemoryCalls.cpp, KernelModel.cpp (669–815), KernelModel.h runtime/pool declarations, KernelUnpackState.cpp pool inventory, and the KernelAPIs.def/KernelAPIIRQL.def string-table includes.

Reviewed KernelRuntimeTests.cpp ranges 1–185, 365–778, 837–1047 and 1118–1167, plus the implementation hunks introducing counted helpers and conversion release. Traced arity/IRQL table dispatch, the 1 MiB buffer budget, length/capacity/terminator handling, ASCII/case refusal, guest integer widths, integer parsing, and paged-pool allocation/free ownership. No additional statically proven defect was found in this bounded sample. Fixture source inspection is not a test result.

### Explicitly uncovered

No whole-repository assurance is implied. Uncovered or only partially sampled: the complete source runtime authentication/capture/buffer implementation, broad solver, loader/scope-table parser, callback outliner/private-stack analyzer beyond selected patches and consumers, LLVM fork machine-state pass and code generation, CRT behavior, most application corpus/SDK/CLI paths, full kernel object/IRQL/concurrency behavior, broad Darwin/Swift changes, complete tests outside the listed files, and most of the 715 changed paths. This review does not establish whole-application PE32 reconstruction correctness.

## Existing CI evidence and limits

At **01:09 UTC**, exact dev source **3273beda** had six workflow records:
- [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/38662488121): success, updated 00:55:34 UTC.
- [Lift native qualification](https://github.com/NeverSight/NeverD/actions/runs/38662488130): success, updated 01:03:15 UTC.
- [CI](https://github.com/NeverSight/NeverD/actions/runs/38662488122): in progress.
- [Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/38662488178): queued.
- [Source patch Windows runtime](https://github.com/NeverSight/NeverD/actions/runs/38662488116): pending.
- [Mobile Real Applications](https://github.com/NeverSight/NeverD/actions/runs/38662559673): skipped.

A successful workflow name does not establish that every platform-specific job ran. Earlier exact-head check inspection included skipped native-EH gates. Dev is not recorded as fully green.

For PR #986's distinct source head **96dbb4635c4fad4f828ba981aac992ad2203e4cf**, existing [focused CI 38657310788](https://github.com/NeverSight/NeverD/actions/runs/38657310788) completed successfully at 00:48:55 UTC. Its event is workflow_dispatch and it predates this review; this review did not dispatch or rerun it. Other PR workflows include cancelled/skipped records after merge, and author-reported Wine/test totals are not independent checks performed here.

The previous snapshot's dev **841dc099** now has three cancelled workflows and one skipped workflow, not a successful completed baseline.

For this review's source-fix head **c9025b27**, automatic PR checks at 01:09 UTC were [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/38664057832) and [Source patch Windows runtime](https://github.com/NeverSight/NeverD/actions/runs/38664057831) in progress, with [CI](https://github.com/NeverSight/NeverD/actions/runs/38664057812) and [Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/38664057800) queued. These are not terminal passing results and do not certify the later report-only head. No local or remote validation was initiated manually.

## Suggested next priorities

1. **Review the bounded SEH placement repair in #987.** Dependency: a maintainer familiar with the protected-invoke provenance contract and an independently authorized validation environment. Acceptance: focused positive/negative regression coverage, unchanged canonical lowering acceptance, exact tested commit/platform evidence, and a human review decision. Draft status and unexecuted regression coverage currently block merge-readiness claims.
2. **Close PE32/native-EH evidence gaps.** Dependency: exact-head CI results/logs and the current reconstruction acceptance contract, including the unresolved history around #858/#966/#969. Acceptance: map source change, focused case, platform and terminal result individually; distinguish pending/skipped/cancelled gates from tested acceptance. No further feature completion should be inferred merely from closure or a merge.
3. **Rotate the next bounded audit into remaining ownership/admission boundaries.** Dependency: stable source head and full runtime/callback/ABI-normalization caller inventory. Acceptance: trace one selected end-to-end ownership or ABI invariant, record explicit files and uncovered edges, and patch only a statically proven defect. Kernel conversion/pool error paths and the unconfirmed ABI question remain candidates, not scheduled commitments.

## Daily log — 2026-10-15

- Enumerated 248 incoming commits and 715 changed paths; selected the new PE32 transfer chain and kernel runtime strings/pool lifecycle for bounded source review.
- Reconciled two new/five closed issues and 29 opened/49 merged PRs before today's draft, including the separately identified report-only activity.
- Published one minimal source guard in new draft #987 after verifying that prior report #956 had already merged; no merge action was taken.
- Recorded exact dev/PR CI snapshots and explicit missing validation rather than treating author reports or in-progress checks as completed validation.
- Added this English snapshot and daily entry. Prior snapshots and human edits are preserved verbatim below.

<details>
<summary>Previous snapshots (preserved verbatim)</summary>

# NeverD Daily Progress

## Current snapshot — 2026-10-14

- Cutoff: **2026-10-14 01:01 UTC / 09:01 Asia/Shanghai**. Bounded review pinned to [c1ccbe23](https://github.com/NeverSight/NeverD/commit/c1ccbe2322d2fbcd47beeacc3a667530d304bb1d); the only later delta through current dev [841dc099](https://github.com/NeverSight/NeverD/commit/841dc099a973dcf2196bcbc5acb9d4710400de7b) is today's two fixes and their externally performed merge.
- Incoming [587227e5…c1ccbe23](https://github.com/NeverSight/NeverD/compare/587227e57dcf7855d317e62e264dd343edb5cfd2...c1ccbe2322d2fbcd47beeacc3a667530d304bb1d): **186 commits / 666 changed file paths** (263 added, 403 modified, none removed). Complete endpoint trees replaced the comparison API's 300-file cap; directory objects are excluded. Commit pagination was 100 + 86. Through 841dc099 there are 189 commits and the same 666 paths.
- Report-only e8575ea4 and its merge 40632e25 are reporting activity, not product delivery. Commit/path counts include merges, fixtures and prose; they are inventory, not completion percentages.
- **Two confirmed small I/O defects fixed**, remote-verified in [#955](https://github.com/NeverSight/NeverD/pull/955). It was created as a draft and merged externally at 01:00:26 UTC; this review did not perform or request the merge. This separate documentation branch follows the contribution workflow against dev.
- Strict static-only source/diff/caller/interface/configuration and existing-CI inspection. **No project execution, build, test, benchmark, repository script, dynamic analysis, manual CI dispatch or rerun.** No dependency/security changes, issue-field changes or deployments.

## Activity and review queue

**29 ordinary open issues / 20 pre-existing open PRs**, versus yesterday's 28 / 16. New [#946](https://github.com/NeverSight/NeverD/issues/946) and [#947](https://github.com/NeverSight/NeverD/issues/947) report distinct Windows-EH crash paths; these are author reports, not reproduced here. [#859](https://github.com/NeverSight/NeverD/issues/859) closed after #920 merged. Closure is not independent verification of every acceptance criterion; #858 remains open.

Between October 13 01:51 UTC and the pre-publication snapshot: **37 PRs opened, 32 merged, one closed without merge (#943)**. These include yesterday's report #916, so excluding that reporting PR gives 36 opened / 31 merged / one closed. Today's subsequently merged repair #955 adds one opening and one merge; the 20-item pre-existing queue remains unchanged. This new report is excluded. Previously open #910 and #912 merged; six new open proposals (#926, #933, #939, #941, #948, #951) account for the net increase.

Open issues returned 49 records (29 issues plus 20 PRs), followed by an empty page; open PRs returned 20 then empty. The changed-since issue collection returned 43 then empty, and the 100 most recently updated PR records extend before the cutoff. All 20 open PRs had no submitted reviews, no inline review comments and no requested reviewers at 00:58 UTC. The repository-wide issue-comment collection since the cutoff was empty. No reviewer readiness is inferred from an empty queue.

## Bounded static review

Read root AGENTS.md and CONTRIBUTING.md, the relevant PDB/system boundaries in architecture guidance and testing guidance. The complete tree contains only the root AGENTS.md. Build/test/format instructions yield to the explicit static-only constraint. Environment discovery found no usable connected authorized computer or saved coding environment, so connector-based source reads and minimal edits were used.

### PDB symbol-store ownership and download error paths

Read complete current files:
- include/neverd/debug/SymbolStore.h and SymbolStore.def; lib/debug/SymbolStore.cpp
- lib/debug/DebugInfoDiscovery.cpp
- tools/neverd/NeverDSymbolDownload.h, NeverDSymbolDownload.cpp, NeverDSymbolTransportCurl.cpp, NeverDSymbolTransportWinHTTP.cpp and SymbolDownloadABI.def
- tools/neverd-gui/app/SymbolDownload.cpp and MicrosoftSymbols.cpp
- unittests/debug/SymbolStoreTests.cpp

Also read the CLI download introduction's registration/build/caller patches and current neverd.cpp lines 125–190. Traced explicit download opt-in, no-debug/manual-PDB gating, unique CodeView identity admission, portable basename handling, GUID/age key construction, URL segment escaping, ordered local candidates and strict post-download engine identity checking. The engine remains separate from frontend transport.

Reviewed GUI serial/epoch/request guards, modal deferral, cancellation, bounded read/transfer behavior, temporary-file ownership, compressed-file admission and publication. Existing fixture sources cover path ordering, cache/server syntax, ambiguous identity refusal and URL escaping; no fixture executed. No universal safety claim is made for the complete network/filesystem surface.

### Confirmed fixes

1. **Lost save-failure diagnostics**, tools/neverd/NeverDSymbolDownload.cpp, original lines 118–123. Trigger: renaming a downloaded partial file fails, then cleanup runs. Both calls use the same error_code; successful removal clears it and the report can say “cannot save …: Success”, or report cleanup's unrelated failure. [0da5403b](https://github.com/NeverSight/NeverD/commit/0da5403b25b77c9b728e6bf44297e61e95535245) captures the original message before removal (+2/-1). Cleanup and fallback control flow are unchanged. Static evidence: exact data flow and remote diff/readback; no filesystem failure was induced.
2. **Unobserved Windows close/flush failure**, tools/neverd/NeverDSymbolTransportWinHTTP.cpp, final return after line 152. Trigger: buffered writes appear successful but stream close fails. Previously the function returned HTTP success before destructor-driven close, losing that result; its caller could proceed to rename and later misclassify the bad file as an identity mismatch. [6d3c11e1](https://github.com/NeverSight/NeverD/commit/6d3c11e15af802c937d27f2cc3e2867c087ed3a2) explicitly closes and checks the stream before success (+3/-0), matching the existing curl transport. Static review confirms failure returns Status 0 through the existing caller path.

Fresh branch file reads supplied both current blob SHAs; both exact committed files and diff hunks were read back. [c1ccbe23…841dc099](https://github.com/NeverSight/NeverD/compare/c1ccbe2322d2fbcd47beeacc3a667530d304bb1d...841dc099a973dcf2196bcbc5acb9d4710400de7b) contains only those two files (+5/-1). No new fixture was executed or runtime regression pass claimed. Acceptance still needs controlled rename/cleanup and delayed-write/close failures on the supported platforms.

**Uncovered / open questions:** most of the 666 changed paths, EH crash root causes, whole PDB parser validation, all GUI consumers, shared-cache concurrent downloads, complete platform transport/version compatibility, native code generation, hosted DynCode integration and large-source web/resource extraction. Those broader paths are not certified by this bounded review. No speculative fix is claimed for them. Recent source work includes native hosted-C++ and Chromium/WebView2 resources; issue openness does not mean all implementation is absent, and author qualification claims are not our execution results.

## Existing CI evidence

At **01:01 UTC**, integrated dev **841dc099** has three newly automatic workflows: [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/38493336602) in progress, [CI](https://github.com/NeverSight/NeverD/actions/runs/38493336597) pending, [Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/38493336604) pending. They do not establish acceptance of either fix.

At 01:00 UTC, earlier reviewed **c1ccbe23** had LLVM Style success and CI/Mobile Decompilation in progress. At 00:57 UTC its 16 exact-head checks were 5 success, 5 in progress, 1 queued and 5 skipped; native Windows-EH gates were skipped. Legacy statuses were empty. Yesterday's **587227e5** ended with one style success, two cancelled workflows and one skipped mobile-real-applications workflow, not full green.

At 01:00 UTC, repair PR head **6d3c11e1** had seven automatic run records: three pending, one queued, one in progress and two cancelled. These include superseded/cancellation automation; cancelled runs are not failures or passes. All workflow/check results above fit 100-entry responses. No manually initiated run exists from this review.

Pre-existing PR exact-head checks at **00:58 UTC** (failed causes not exhaustively diagnosed):

| PR | Exact head | Existing check counts |
|---|---|---|
| [#951](https://github.com/NeverSight/NeverD/pull/951) | `09093108731ca50910f335737a84317078520768` | No exact-head checks returned |
| [#948](https://github.com/NeverSight/NeverD/pull/948) | `99c5108cc2837bec31079211b98617d2511d190e` | No exact-head checks returned |
| [#941](https://github.com/NeverSight/NeverD/pull/941) | `5cc8d41dde44500bb358e66ddd13ed5433f4dcc5` | 6 skipped, 13 success, 3 failure, 1 cancelled |
| [#939](https://github.com/NeverSight/NeverD/pull/939) | `535638d32c60f6f07196338aa3b81764897aa834` | 5 skipped, 4 failure, 6 success, 1 cancelled |
| [#933](https://github.com/NeverSight/NeverD/pull/933) | `26e07b5be04d8c2b0e93f69105978625ad85ab44` | 5 skipped, 7 success, 4 failure |
| [#926](https://github.com/NeverSight/NeverD/pull/926) | `3b45c955dc7a10d717b7b7689863de870213117c` | 6 skipped, 12 success, 4 failure, 1 cancelled |
| [#906](https://github.com/NeverSight/NeverD/pull/906) | `f79b8144ab7ccd706592e2d2fbf1c68fc16e1ed4` | 5 skipped, 13 success, 4 failure |
| [#895](https://github.com/NeverSight/NeverD/pull/895) | `42c039454fd5721cd3fa30337849ae564f6222d4` | 5 skipped, 13 success, 4 failure |
| [#889](https://github.com/NeverSight/NeverD/pull/889) | `00e232132b474cadbfd0e1c96c5e0854a82adca6` | 5 skipped, 12 success, 4 failure, 1 cancelled |
| [#882](https://github.com/NeverSight/NeverD/pull/882) | `014da8476c9806806cf2d70dcc42de60fbf5f3e7` | 5 skipped, 12 success, 4 failure, 1 cancelled |
| [#866](https://github.com/NeverSight/NeverD/pull/866) | `29429abb8d6c9e988eb4ecb909320c01cadb142d` | 5 skipped, 12 success, 5 failure |
| [#855](https://github.com/NeverSight/NeverD/pull/855) | `639f2c4fa2a3f7db745d0147ece5a933b48e684c` | 5 skipped, 12 success, 5 failure |
| [#850](https://github.com/NeverSight/NeverD/pull/850) | `6916e39381529576ac6deb667e2ed0219d2ecc4f` | 5 skipped, 12 success, 5 failure |
| [#847](https://github.com/NeverSight/NeverD/pull/847) | `eedb586ddbea23661eb56e5e474de14ac25b2754` | 5 skipped, 12 success, 5 failure |
| [#845](https://github.com/NeverSight/NeverD/pull/845) | `e1026072f9dc4f89ee2eee969c8ad32433d473f6` | 5 skipped, 12 success, 5 failure |
| [#842](https://github.com/NeverSight/NeverD/pull/842) | `31af683a3e2ebb08ddf8abbc2c171688ddff140b` | 5 skipped, 12 success, 5 failure |
| [#839](https://github.com/NeverSight/NeverD/pull/839) | `2d33959271e3a2c4379e67d6b850cfc483b6e09e` | 5 skipped, 12 success, 5 failure |
| [#838](https://github.com/NeverSight/NeverD/pull/838) | `e5272285f87bc37692c26727b33f7aff55b8793b` | No exact-head checks returned |
| [#834](https://github.com/NeverSight/NeverD/pull/834) | `895037760df5cb5f6c2a4ce2a4e6728b1e05a113` | 4 skipped, 4 failure, 6 success, 1 cancelled |
| [#829](https://github.com/NeverSight/NeverD/pull/829) | `64dd74afcbec93c50427e985a497ffb2a993381f` | 9 skipped, 6 failure, 8 success |

Three heads (#838, #948, #951) have missing exact-head check evidence, not green. Every other sampled pre-existing PR head has failed checks. All supplied author-local testing remains author-reported unless an exact workflow result is separately linked. The later documentation commit's checks are separate from source qualification.

## Suggested priorities

1. **Qualify the merged download I/O fixes.** Dependencies: exact-head automatic platform results and fault-injection coverage. Acceptance: rename error survives successful or failed cleanup; delayed close failure returns transport failure and does not publish a partial file. Retain the no-build/no-test limitation until actual evidence exists.
2. **Diagnose #946/#947 independently and review EH dependencies.** Dependencies: current source/call relationships and the original report's exact corpus/revision, plus #933/#838/#829 overlap. Acceptance: a bounded proven root cause or explicit unresolved boundary; no guessed semantics or inferred success from #859 closure.
3. **Reduce the 20-PR evidence/review backlog.** Dependencies: independent reviews of the Darwin stack, #939 forwarding changes and #941 Swift state, and exact-head checks for #838/#948/#951. Acceptance: each selected proposal has a reviewed contract and relevant platform evidence; integrated dev reaches explicit terminal results with skipped gates retained. Priorities are suggestions, without assigned owners or delivery dates.

## Daily log — 2026-10-14

- Inventoried 186 incoming commits / 666 paths, then verified today's two-file, three-commit integrated delta.
- Completed the bounded symbol-store, CLI/GUI download ownership and I/O review.
- Published two minimal statically proven repairs in draft #955; recorded its external merge without taking a merge action.
- Reconciled two new issues, one closure, the 20-PR review queue and exact-head CI limitations.
- Added this English snapshot and daily record; preserved prior history and human edits verbatim below.

<details>
<summary>Previous snapshots (preserved verbatim)</summary>

# NeverD Daily Progress

## Current snapshot — 2026-10-13

- Cutoff: **2026-10-13 01:51 UTC / 09:51 Asia/Shanghai**. Detailed source review was pinned at [41141c57](https://github.com/NeverSight/NeverD/commit/41141c57c4c5eaddacc68b53f145d7534e5e6f52); five later commits through current dev [587227e5](https://github.com/NeverSight/NeverD/commit/587227e57dcf7855d317e62e264dd343edb5cfd2) received patch-level inspection.
- Incoming range: [c3df0112…587227e5](https://github.com/NeverSight/NeverD/compare/c3df0112296b75feed71917ef01c51342762a03b...587227e57dcf7855d317e62e264dd343edb5cfd2), **347 commits / 646 changed file paths** (151 added, 494 modified, 1 removed). Both endpoint trees were complete. Commit subjects were enumerated in 100 + 100 + 100 + 42, then 5 later commits; the initial comparison's 300-file cap was replaced by full-tree path accounting.
- **No new statically proven source defect in this bounded sample; no source or test change by this review.** Counts include merges and documentation and are inventory, not business completion. Report-only commit e5b77246 and merge e9f78f8b are excluded from implementation claims. Documentation-only work is not credited as product progress.
- Method: source/diff/caller/interface/configuration and existing-CI reads only. **No build, test, benchmark, project execution, repository script, dynamic analysis, manual CI trigger or rerun.** Static fixture inspection and author-reported results are not checks performed here.
- Root AGENTS.md, CONTRIBUTING.md, architecture/testing and roadmap guidance were read. The complete tree contains no nested AGENTS.md. The static-only instruction overrides execution guidance. Contribution guidance calls for a topic branch and a PR against dev; this report follows that route. No issue fields, dependencies, security settings, merge or deployment were changed.

## Activity and review queue

**28 ordinary open issues / 16 pre-existing open PRs**, versus 26 / 10 in the October 12 snapshot. New ordinary issues [#858](https://github.com/NeverSight/NeverD/issues/858) and [#859](https://github.com/NeverSight/NeverD/issues/859) report x64 local-unwind and PE32 registration-state limitations. No ordinary issue closed. Issue [#825](https://github.com/NeverSight/NeverD/issues/825) remains planned triage work.

Since October 12 01:01 UTC: **59 PRs opened / 53 merged / 0 closed without merge**, including report [#856](https://github.com/NeverSight/NeverD/pull/856), now merged. Previously pending #849 and #841 merged; eight new open PRs (#855, #866, #882, #889, #895, #906, #910, #912) account for the net increase. These totals include documentation PRs and are not a count of delivered product features. Today's report is excluded.

The open issue collection was exhausted at 44 records (28 issues plus 16 PRs) with an empty second page; the open PR collection has 16 plus an empty second page. Updated-since issues returned 64 plus an empty page. The latest 100 all-state PR records extend back before the prior cutoff, fully covering the activity interval. A fresh 01:50–01:51 read retained those counts.

All sixteen PRs returned no submitted reviews, inline comments or discussion comments at 01:02 UTC and have no requested reviewers. PR descriptions are author reports, not this review's validation. Exact heads remained unchanged at the final inventory. The Darwin progression continues through created-file providers, timestamps and filesystem snapshots. #910 (x87 context) and #912 (LLVM C allocation lifetime/count) are pending source proposals, not merged fixes.

## Bounded static coverage

### HighC navigation position recording

Read complete CSourceRecorder.cpp/.h, every production and fixture hunk of [1def6b36](https://github.com/NeverSight/NeverD/commit/1def6b36), and the current caller bodies in HighCEmitter.cpp (lines 3447–3580), HighCFuncWriter.cpp (6455–6514) and HighCStmtWriter.cpp (327–348).

Traced no-library-evidence admission, ordinary-output byte positions, nested/body buffer relocation, deferred declarations, suppressed statements, overflow and bounds refusal, and fallback to the independent marker proof. An ordinary render/output failure is not retried as an optional mapping failure. Library recognitions continue through independent recordings. Fixture sources exercise nested spans, marker-like user text, empty evidence, invalid offsets and render/output exceptions; none were executed. No measured performance claim is made.

### Access-width cache ownership

Read complete include/neverd/DataAccessWidths.h and lib/sdk/capi/InstructionFlow.cpp, SessionImpl.h's reset/navigation/invalidation owners (409–623), and all source/fixture changes in [c8559240](https://github.com/NeverSight/NeverD/commit/c8559240).

The cache compares exact function entry/size pairs instead of a hash. Navigation retains immutable image facts but discards emitted sources; changed extents force a rescan and source retirement. Edits, reload/policy and signature-generation changes use the invalidation path. Collection sorts decoded accesses before the width model, preserves disagreement as unknown, and includes authenticated outlying chunks. Read source cases for navigation, changed extents, conflicting widths, edits and same-address image replacement. Exhaustive mutation-path and all-architecture decoding proof remains outside this sample.

### Passive report and local readable export

Read complete SessionPassiveReport.cpp, interfaces/report/{Compose,Publication,Shapes,Source}.cpp and PassiveReportTests.cpp. Traced explicit committed selections, source/binding/navigation identity, imported-byte hash checks, aggregate parse/work/record/metadata limits, serialization before cache insertion and bounded pagination. Publication distinguishes static inference, imported observations and candidate correlation; type summaries do not expose field names/values or claim server schema, optionality, permission enforcement or authenticated protocol identity.

Read complete SessionReadableExport.cpp and ExportDirectory.h, plus selected ReadableExportSDKTests.cpp assertions. Export uses retained immutable bytes, a selected source goal and hash verification; readable text is emitted only for the same-parser-tree outcome. The writer uses a new private destination, fixed filenames, exclusive/no-follow file creation, read-back hashes and no-clobber manifest publication. Windows export remains unavailable; partial files may remain after I/O failure. Did not fully audit RecoveryParser, every filesystem race/durability case, or all GUI/CLI consumers.

### Late five-commit delta

Read all 16 changed-file patches from 41141c57 to 587227e5: first-written mask definitions, System V entry-read parameter selection, bounded known Linux syscall arity, ELF GOT inclusion in fixture images and double-load indirect-call spelling. The shared constant-register and ELF-OS helpers plus added fixture assertions were inspected. This was patch-level coverage, not a complete proof of the affected call-graph/ABI/emitter subsystems.

**Uncovered:** most of the 646 changed paths; full x87/native recovery, source patching, debug/PDB persistence, parallel HighIR/KVM ownership, desktop bridge extraction, every platform, packaging and regression execution. New web acceptance documents record substantial Tauri/Wails and passive-workflow implementation; the older “wholly pending” description is stale, but their qualification receipts remain author evidence unless separately verified. Open #714–#718 is not by itself evidence that nothing is implemented or that every acceptance gate is complete. No speculative repair was made.

## Existing CI, with exact revisions

At **01:50 UTC**, current dev **587227e5** has **4 workflows: 1 success, 1 in progress, 1 queued, 1 skipped**; **24 checks: 5 success, 4 in progress, 2 queued, 13 skipped**. Legacy status contexts are empty. Pages fit their 100-entry limits.

- [CI](https://github.com/NeverSight/NeverD/actions/runs/38333135382): in progress
- [Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/38333135349): queued
- [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/38333135346): success
- [Mobile Real Applications](https://github.com/NeverSight/NeverD/actions/runs/38333150649): skipped

Earlier reviewed **41141c57** had a confirmed failing [macOS mobile job](https://github.com/NeverSight/NeverD/actions/runs/38324681111/job/115060795895). Existing logs show conditional/mutating preprocessor directives refused in native export, unsupported Swift x86 floating-point state shapes, and incomplete single-session qualification (8/12). Native C++ build/readers passed in that job, while Objective-C scalar, Swift and parity gates failed. Those are prior-head CI observations, not an established cause or a failure attached to 587227e5. Current-head queued/running checks do not prove the failures resolved. The October 12 c3df0112 baseline ended with three cancelled workflows and one skipped workflow.

PR exact-head check snapshot at **01:02 UTC**:

| PR | Exact head | Existing checks |
|---|---|---|
| [#912](https://github.com/NeverSight/NeverD/pull/912) | `8dfd0aef0e5fde42827da7e34d717886d9bef2e6` | 4 skipped, 6 success, 4 failure, 1 cancelled |
| [#910](https://github.com/NeverSight/NeverD/pull/910) | `734e77023330897af58c3a7ce1e914cfe6465cf4` | 4 skipped, 4 failure, 7 success |
| [#906](https://github.com/NeverSight/NeverD/pull/906) | `f6460d703c391375027a3702361d3c04e73d8ef3` | 5 skipped, 12 success, 4 failure, 1 cancelled |
| [#895](https://github.com/NeverSight/NeverD/pull/895) | `42c039454fd5721cd3fa30337849ae564f6222d4` | 5 skipped, 13 success, 4 failure |
| [#889](https://github.com/NeverSight/NeverD/pull/889) | `00e232132b474cadbfd0e1c96c5e0854a82adca6` | 5 skipped, 12 success, 4 failure, 1 cancelled |
| [#882](https://github.com/NeverSight/NeverD/pull/882) | `014da8476c9806806cf2d70dcc42de60fbf5f3e7` | 5 skipped, 12 success, 4 failure, 1 cancelled |
| [#866](https://github.com/NeverSight/NeverD/pull/866) | `29429abb8d6c9e988eb4ecb909320c01cadb142d` | 5 skipped, 12 success, 5 failure |
| [#855](https://github.com/NeverSight/NeverD/pull/855) | `639f2c4fa2a3f7db745d0147ece5a933b48e684c` | 5 skipped, 12 success, 5 failure |
| [#850](https://github.com/NeverSight/NeverD/pull/850) | `6916e39381529576ac6deb667e2ed0219d2ecc4f` | 5 skipped, 12 success, 5 failure |
| [#847](https://github.com/NeverSight/NeverD/pull/847) | `eedb586ddbea23661eb56e5e474de14ac25b2754` | 5 skipped, 12 success, 5 failure |
| [#845](https://github.com/NeverSight/NeverD/pull/845) | `e1026072f9dc4f89ee2eee969c8ad32433d473f6` | 5 skipped, 12 success, 5 failure |
| [#842](https://github.com/NeverSight/NeverD/pull/842) | `31af683a3e2ebb08ddf8abbc2c171688ddff140b` | 5 skipped, 12 success, 5 failure |
| [#839](https://github.com/NeverSight/NeverD/pull/839) | `2d33959271e3a2c4379e67d6b850cfc483b6e09e` | 5 skipped, 12 success, 5 failure |
| [#838](https://github.com/NeverSight/NeverD/pull/838) | `e5272285f87bc37692c26727b33f7aff55b8793b` | No exact-head checks returned |
| [#834](https://github.com/NeverSight/NeverD/pull/834) | `895037760df5cb5f6c2a4ce2a4e6728b1e05a113` | 4 skipped, 4 failure, 6 success, 1 cancelled |
| [#829](https://github.com/NeverSight/NeverD/pull/829) | `64dd74afcbec93c50427e985a497ffb2a993381f` | 9 skipped, 6 failure, 8 success |

No-check #838 is missing evidence, not green. All other sampled PR heads have failed checks; not every cause was diagnosed. Automatically created documentation checks belong to this later report commit, not source qualification.

## Suggested priorities

1. **Establish an integrated exact-head platform result.** Dependency: existing dev CI/mobile runs and diagnosis of the earlier native-export/Swift failures. Acceptance: explicit terminal results for required stages on one integrated SHA, with skipped native-host gates retained; no green claim from style or setup jobs.
2. **Review the sixteen-PR queue by dependency and evidence.** Dependency: independent Darwin-stack review, x87/LLVM-C lifetime review and current-head failure diagnosis; #838 needs exact-head checks. Acceptance: each proposed change has a reviewed bounded contract and relevant platform evidence before any merge decision.
3. **Resolve the two new EH acceptance gaps without widening unproven behavior.** Dependency: source-bound frame/registration facts for #858/#859 and the related #829/#838 proposals. Acceptance: exact failing input/ownership condition maps to a minimal reviewed fix or explicit supported-boundary decision. Issue status is not changed by this report.

## Daily log — 2026-10-13

- Inventoried the incoming 347 commits and 646 paths; documentation activity is not counted as product delivery.
- Reviewed navigation recording, access-width cache ownership, selected passive report/export boundaries and the late five-commit patch set.
- No newly proven source defect or source/test patch in this repository.
- Reconciled the merged prior report, two new issues, sixteen-PR queue and exact-head CI evidence.
- Added this English snapshot and daily record; preserved all prior history and human text verbatim below.

<details>
<summary>Previous snapshots (preserved verbatim)</summary>

# NeverD Daily Progress

Last verified: **2026-10-12 09:01 Asia/Shanghai (UTC+08:00) / 01:01 UTC**.

Bounded static review and existing GitHub evidence only. This report is proposed against dev on a topic branch; no merge or release readiness is asserted. Priorities are suggestions.

## Snapshot and changes

- Pinned dev: [c3df0112](https://github.com/NeverSight/NeverD/commit/c3df0112296b75feed71917ef01c51342762a03b); previous reviewed source: [e4236cb9](https://github.com/NeverSight/NeverD/commit/e4236cb903f87f0a6e4a63a13dfc578193f5bafa).
- [Comparison](https://github.com/NeverSight/NeverD/compare/e4236cb903f87f0a6e4a63a13dfc578193f5bafa...c3df0112296b75feed71917ef01c51342762a03b): **347 commits / 1,119 changed file paths** (467 added, 651 modified, 1 removed). Excluding report commit 2ce39a98, its merge e442f57e and PROGRESS.md gives **345 non-report commits / 1,118 paths**. Merge commits, fixtures and documentation are included; these are not completion metrics.
- Activity window: October 11 01:04 through October 12 01:01 UTC. **26 ordinary open issues**, up one: [#825](https://github.com/NeverSight/NeverD/issues/825) mixed-ISA triage/entropy exploration. No ordinary issue closed. An issue publication is not implementation.
- **10 pre-existing open PRs**, down from 17. **34 PRs opened / 39 merged / 2 closed without merge** in the interval. Report #820 contributes one opening/merge; product counts are **33 opened / 38 merged / 2 closed without merge**. Today's report is excluded.
- All 17 PRs pending in the previous report and report [#820](https://github.com/NeverSight/NeverD/pull/820) have merged. Closed without merge: [#832](https://github.com/NeverSight/NeverD/pull/832) and [#851](https://github.com/NeverSight/NeverD/pull/851); closure alone is not delivery.
- No new statically proven defect in the sample below; no speculative source patch.

## Static review and evidence

Read root AGENTS.md and CONTRIBUTING.md, architecture/testing guidance relevant to source-map and package-review ownership, and roadmap context. The complete current tree has only the root AGENTS.md. Static-only instructions override build/test/formatter guidance: no code, build, test, benchmark, repository script or dynamic analysis was executed, and no CI was manually triggered or rerun.

### Offline advisory matching

Read complete current files:
- lib/web/packages/advisories/SemVer.cpp, Match.cpp and Reader.cpp
- lib/web/SessionPackageAdvisories.cpp
- lib/web/packages/evidence/Admission.h
- unittests/web/PackageAdvisoryTests.cpp

Traced strict SemVer grammar, arbitrary-width decimal ordering, prerelease precedence, ignored build metadata for precedence versus exact enumerated-version matching, sorted introduced/fixed/last-affected events, limit unions, unsupported selector coverage and matching declared npm identities. Work, retained metadata, selector and result ceilings are charged before publication. Session imports construct and serialize privately; descriptor insertion is rolled back if advisory insertion fails.

Unsupported selectors prevent a negative coverage conclusion. Missing package declarations, conflicting identities and malformed versions remain unknown. Withdrawal, source/as-of times and supplied severity are separate declaration fields; a version match deliberately does not prove current vulnerability or exploitability. The fixture source covers those distinctions, malformed/colliding ranges, large components, aggregate work and private-output canaries; it was not executed.

Limits: no whole-OSV/schema compatibility, live feed freshness, publisher authentication, installed dependency identity or reachability claim. Did not exhaustively audit every declaration reader or package resolver.

### Immutable analyst decisions and journal restoration

Read complete:
- include/neverd/web/PackageAssessment.h
- lib/web/packages/assessment/Disposition.cpp and Metadata.cpp
- lib/web/SessionPackageAssessments.cpp and SessionPackageReviewRestore.cpp
- unittests/web/PackageReviewTests.cpp
- journal restore/tampering/reset cases in PackageAssessmentSDKTests.cpp
- docs/web-package-review-journal.md

The authoritative disposition owner validates the complete identity/parent/index/finding chain. Restoration requires the retained history as an exact prefix, computes charges rather than trusting imported sizes, and checks the current expected head. Only a valid suffix consumes the shared 256-event/256-KiB capacity; exact current-head replay consumes none. Unlike append's last-request replay, restore does not accept an old parent head.

The session holds its mutex, checks revision and owner, constructs a candidate and serializes its response before replacing state and charging counters. The parser uses a fixed bounded schema with no arbitrary notes or paths. Source fixtures cover a late altered decision, foreign assessment, invalid disposition, duplicate keys, oversized input, shortened history and reset/replay. These are inspected tests, not passing runs.

The metadata composer keeps selected advisory/version relationships, feed times and unauthenticated attestation/digest declarations separate from analyst assertions. Hash-chain consistency does not authenticate an analyst or make an accepted-risk/false-positive assertion correct. Qt filesystem persistence, every GUI transition and every composer input combination were outside detailed review.

### HighC source recording optimization

Read all production/interface changes and added fixture source in [c3aef8bc](https://github.com/NeverSight/NeverD/commit/c3aef8bc810eb5a1660145253355cb6261acb9bd), complete CSourceRecorder.cpp/.h, and HighCEmitter.cpp lines 3070–3230.

When library recognitions are absent, definition and instruction anchors can share one private render. The constructor refuses combined mode with actual library evidence before clearing existing maps. Publication still requires removing markers to reproduce ordinary text exactly. A failed combined render falls back to the existing independent recordings. Output publication remains outside the mapping exception handler, so a failed output is not retried as a mapping failure. LLVM C retains its independent route.

Added fixtures cover marker-like UTF-8 text, empty versus actual recognition catalogs, changed/throwing private renders and single-attempt output failure. No execution or performance measurement was performed. This does not establish global emitter correctness or measured speedup.

### Prior Windows collision

[#818](https://github.com/NeverSight/NeverD/pull/818) is merged. Current cmake/hermes-parser/llvh/CMakeLists.txt gives LLVHSupport a Windows-private HandleAbort name. The previously pending fix is now integrated, but today's exact-dev Windows build is queued at sampling: no successful link/build is inferred from the source change. No duplicate repair was created.

## Existing CI and review snapshot

Sampled **00:59–01:00 UTC**, exact dev **c3df0112**:
- [CI](https://github.com/NeverSight/NeverD/actions/runs/38189647328): queued.
- [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/38189647348): queued.
- [Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/38189647352): queued.
- [Mobile Real Applications](https://github.com/NeverSight/NeverD/actions/runs/38189650851): skipped.
- Exact-head checks: **2 success, 1 in progress, 8 queued, 13 skipped**. The two successful jobs are individual backend/caller-context checks, not an overall build pass.

Pre-existing PRs, sampled around 01:00 UTC:

| PR | Exact head | Check snapshot |
|---|---|---|
| [#850](https://github.com/NeverSight/NeverD/pull/850) | `6916e39381529576ac6deb667e2ed0219d2ecc4f` | 4 skipped, 6 success, 1 failure, 2 in_progress, 7 queued |
| [#849](https://github.com/NeverSight/NeverD/pull/849) | `3447781f71608628a4527a39b57c448dec7c0cc9` | 4 skipped, 2 queued, 5 success, 3 failure, 1 in_progress |
| [#847](https://github.com/NeverSight/NeverD/pull/847) | `eedb586ddbea23661eb56e5e474de14ac25b2754` | 4 skipped, 10 success, 2 queued, 3 in_progress, 1 failure |
| [#845](https://github.com/NeverSight/NeverD/pull/845) | `e1026072f9dc4f89ee2eee969c8ad32433d473f6` | 5 skipped, 12 success, 2 in_progress, 3 failure |
| [#842](https://github.com/NeverSight/NeverD/pull/842) | `31af683a3e2ebb08ddf8abbc2c171688ddff140b` | 5 skipped, 12 success, 5 failure |
| [#841](https://github.com/NeverSight/NeverD/pull/841) | `01b3372ac244456dde0f4209a2d9c9a0b84c0717` | 8 skipped, 9 queued, 4 success, 3 in_progress |
| [#839](https://github.com/NeverSight/NeverD/pull/839) | `2d33959271e3a2c4379e67d6b850cfc483b6e09e` | 5 skipped, 12 success, 5 failure |
| [#838](https://github.com/NeverSight/NeverD/pull/838) | `70fcda5f18dd4da20542384a492a640be5e95ae3` | 11 skipped, 2 failure, 4 success, 6 queued, 1 in_progress |
| [#834](https://github.com/NeverSight/NeverD/pull/834) | `895037760df5cb5f6c2a4ce2a4e6728b1e05a113` | 4 skipped, 4 failure, 6 success, 1 cancelled |
| [#829](https://github.com/NeverSight/NeverD/pull/829) | `64dd74afcbec93c50427e985a497ffb2a993381f` | 1 in_progress, 8 skipped, 6 success, 7 queued |

All ten returned empty review-submission, inline-comment and conversation-comment collections and no requested reviewers. They need independent review; not every failing check has a diagnosed cause. PR body validation remains author-reported, often tied to earlier local revisions, and does not replace current-head results.

Darwin #839 → #842 → #845 → #847 → #850 is an explicitly stacked progression through mode, ownership, advisory locks, file synchronization and data synchronization. #829 PE32 objects and #838 SEH frame work need their own runtime-evidence gates; #841 is a separate driver-write observation track. These relationships do not authorize merging.

## Capability changes and limits

The current [web support matrix](docs/web-support-matrix.md) documents substantial source progress beyond yesterday: Node SEA/nexe/pkg profiles, ZIP/NW.js/VSIX metadata, bounded reversible primitive/rename/Boolean transforms, native C++ MCP, advisory/attestation/assessment records and a Qt package-review workflow.

It also records eight captured Claude Code 2.1.296 packages across six target layouts and supplied Linux libc variants. Those are **author-recorded local qualification results**, not runs reproduced here; host qualification remains distinct from target artifact extraction. Windows input capture, Tauri/Wails, full runtime module resolution, publisher authentication and complete #714–#718 acceptance remain gaps. Do not carry yesterday's “MCP/SEA/pkg/nexe wholly pending” description forward.

## Suggested priorities

1. **Qualify the integrated dev baseline, including the Windows link repair.** Dependency: completion and diagnosis of exact-head platform checks. Acceptance: explicit successful required build/test stages on the integrated SHA, with skipped/native-host gaps preserved. No current whole-dev green result.
2. **Review the ten-PR queue by dependency and evidence.** Dependency: independent review of the Darwin stack, PE32/SEH and driver tracks, plus exact-current-head failures. Acceptance: each proposed change has a reviewed scope and corresponding platform evidence; stale local validation and closed-without-merge PRs are not counted as completion.
3. **Close bounded web acceptance gaps before broad support claims.** Dependency: host input/runtime packaging, explicit profile/version fixtures, and remaining GUI selections. Acceptance: map #714–#718 criteria to exact implemented/qualified slices; retain Tauri/Wails, runtime semantics and trust/authentication limits. #825 stays a separate planned triage workstream.

## Daily log — 2026-10-12

- Inventoried 347 commits and 1,119 paths; excluded report-only activity from product counts.
- Reviewed advisory matching, immutable decision/journal restoration and the HighC combined-source recording change.
- Found no new statically proven bug in this sample; no source fix or test was authored.
- Updated the prior Windows blocker and PR queue to their actual merged/current states.
- Preserved prior snapshots and manual content verbatim. No issue, label, assignee, dependency, security setting, workflow, merge or deployment was changed.

## Coverage and counting limits

Compare pages were 100 + 100 + 100 + 47 + 0 commits. The capped 300-file comparison was supplemented with complete recursive trees (7,395 and 7,891 entries, neither truncated); only non-tree paths were counted, with renames represented by removed/added paths. Open issue/PR and updated-issue second pages were empty. The latest 100 closed PRs reach October 9, before this interval; merged_at distinguishes merge from closure. Dev CI/check second pages were empty; each PR check response matched its returned total.

Most changed native CPU/EH/emulation, patching, GUI, JS/container and symbolic paths were not deeply reviewed. No whole-repository audit, reproduced qualification, build/test pass, benchmark, or release-readiness claim. GitHub metadata can change after the snapshot.

<details>
<summary>Previous snapshots and manual content (preserved verbatim)</summary>

# NeverD Daily Progress

Last verified: **2026-10-11 09:04 Asia/Shanghai (UTC+08:00) / 01:04 UTC**.

Bounded static review and point-in-time GitHub evidence only. This report is proposed on a topic branch against dev, not merged. Priorities are suggestions, not delivery commitments.

## Snapshot and changes

- Pinned dev source: [e4236cb9](https://github.com/NeverSight/NeverD/commit/e4236cb903f87f0a6e4a63a13dfc578193f5bafa). Previous reviewed source: [ee6e0f54](https://github.com/NeverSight/NeverD/commit/ee6e0f54140eade0a29868331ea8bb78f164d92c).
- [Comparison](https://github.com/NeverSight/NeverD/compare/ee6e0f54140eade0a29868331ea8bb78f164d92c...e4236cb903f87f0a6e4a63a13dfc578193f5bafa): **302 commits / 1,158 changed paths** (552 added, 601 modified, 5 removed). Excluding report commit 6ee561ab, its report-only merge ca8b3a0d and PROGRESS.md leaves **300 non-report commits / 1,157 paths**. These include merges, tests and docs, not completion metrics.
- Activity window: October 10 01:07 through October 11 01:04 UTC. **25 ordinary open issues**, unchanged; no ordinary issues newly opened/closed in the returned activity interval.
- **17 pre-existing open PRs**, up from four. **61 opened / 48 merged / zero closed without merge** during the interval. Report-only #759 contributes one opening and merge, so non-report counts are **60 opened / 47 merged**. Today's report PR is excluded.
- Previous #728, #744, #751, #758 and report [#759](https://github.com/NeverSight/NeverD/pull/759) are merged. Do not carry their old pending states forward.
- Web work has advanced beyond proposals: source now contains JS evidence analysis, Bun/ASAR extraction, npm archive/SRI and passive interface/stream readers. Issues #714–#718 remain open; implemented slices do not satisfy every acceptance criterion.
- No new independently proven defect in the reviewed source sample; no speculative source patch. A confirmed existing Windows link failure already has a pending repair in #818, described below.

## Static review scope

Read root AGENTS.md, CONTRIBUTING.md, architecture/testing guidance and roadmap. The full current tree contains no nested AGENTS.md. The user-required static-only constraint overrides build/test/formatter instructions. No project, test, compiler, repository script, benchmark or dynamic analysis was executed; CI was only read.

### 1. New package archive and SRI evidence

Read complete current files:
- [PackageArchive.cpp](https://github.com/NeverSight/NeverD/blob/e4236cb903f87f0a6e4a63a13dfc578193f5bafa/lib/web/packages/PackageArchive.cpp), PackageArchive.h and SessionPackageArchive.cpp.
- [PackageIntegrity.cpp](https://github.com/NeverSight/NeverD/blob/e4236cb903f87f0a6e4a63a13dfc578193f5bafa/lib/web/packages/PackageIntegrity.cpp), SessionPackageIntegrity.cpp, shared BlobStore.cpp and PathPolicy.cpp.
- Complete PackageArchiveTests.cpp and PackageIntegrityTests.cpp source.

Traced single-gzip end/CRC handling, bounded expanded storage, tar framing/checksum/termination, PAX per-member lifetime, file/type/name admission, case-folded parent collisions, metadata-only links and the session's aggregate archive allowance. Derived bytes remain private before validation/publication; members use immutable slices. SRI uses original selected bytes, strongest supported digest candidates and explicit missing/invalid/unsupported outcomes. A match is not publisher authentication or a safety verdict.

Limitations: this did not independently validate the SHA-512 compression implementation, every archive dialect, all SDK consumers or full package dependency resolution. Test vectors and optional real Claude Code fixtures were read, not run. Windows input capture is explicitly unavailable; ICU/runtime packaging and host qualification remain material dependencies.

### 2. Passive stream records and reviewed publication

Read complete [Framing.cpp](https://github.com/NeverSight/NeverD/blob/e4236cb903f87f0a6e4a63a13dfc578193f5bafa/lib/web/streams/Framing.cpp), Records.cpp, Relations.cpp, SessionStreams.cpp, Streams.h, JsonReader.cpp, SessionInternal.h, StreamTests.cpp and StreamSDKTests.cpp.

Traced SSE blank-line dispatch versus unfinished EOF, inherited ID-buffer state, JSON preflight and cumulative malformed-input work, exact original numeric ID tokens, typed/session/direction joins, duplicate/coverage-gap refusal, and bounded private identity storage. Stream publication consumes a preview token, checks revision/hash and retains metadata-only pages. Unselected logs are not automatically treated as MCP. The retained tests cover malformed/ambiguous records, redaction canaries, stale revisions and consumed tokens; none were executed.

Limitations: this is recorded candidate correlation, not full MCP schema/negotiation validation, capture authentication, source execution or live networking. No broad JS parser, runtime semantics, ASAR/Bun container or package-diff audit is claimed.

### 3. Loop inference cut admission and authoritative checking

Read all production/interface/test hunks in [edadf472](https://github.com/NeverSight/NeverD/commit/edadf4722fb8e2678a2e7731bd1ce90d1afb08a1), complete LowIRLoopAlignment.cpp and LowIRLoopInference.h, and LowIRUndefinedIndependence.cpp lines 5930–6105.

The alignment caller now passes MaxCuts and eligible cuts before symbolic inference. Complete CFG/cycle validation precedes the cut-count rejection; ordinary single-cut attempts remain separately bounded. Oversized families stop before symbolic work. Pairing still invokes the independent refinement checker, shares solver/search budgets, and treats rank bindings as opt-in proposals. Tests explicitly cover zero/one cut caps, malformed CFGs, incomplete cycle coverage, filtered-family budgets and rejected changed arithmetic. This is source review, not independent solver soundness or a test pass.

### 4. Windows integration failure and pending repair

Exact-dev [Windows job](https://github.com/NeverSight/NeverD/actions/runs/38092716664/job/114332267685) fails linking neverd.dll:
- LLVHSupport.lib(Signals.cpp.obj): LNK2005 HandleAbort already defined in LLVMSupport.lib(Signals.cpp.obj).
- LNK1169 follows. The observed failure is a DLL-link failure, not a completed test-suite failure.

Read current cmake/hermes-parser/llvh/CMakeLists.txt, lib/web/CMakeLists.txt and lib/sdk/CMakeLists.txt. [Draft #818](https://github.com/NeverSight/NeverD/pull/818) at 62d7a7dc already adds a Windows-only source compile definition, HandleAbort=NeverDLLVHHandleAbort, scoped to LLVHSupport's Signals.cpp. The exact eight-line patch was inspected. It is a pending integration/qualification candidate, not a repair already on dev. No duplicate patch or merge was made; the rest of that PR was not exhaustively reviewed.

## Web capability distinction

The [support matrix](https://github.com/NeverSight/NeverD/blob/e4236cb903f87f0a6e4a63a13dfc578193f5bafa/docs/web-support-matrix.md) and [Claude Code qualification](https://github.com/NeverSight/NeverD/blob/e4236cb903f87f0a6e4a63a13dfc578193f5bafa/docs/web-claude-code-qualification.md) report offline recovery of the pinned Claude Code 2.1.296 Linux x64 artifact, with 2,589 declared Bun modules and 2,345 decoded JS modules, plus npm-original/standalone equality. These are **author-recorded local results**, not execution performed by this review.

The inspected archive fixture pins original size/hash and retained member equality. It does not independently reproduce the recorded run. No original TypeScript, missing source maps, native/JSC-cache decompilation, arbitrary-version coverage or all-host support is established. Tauri/Wails/SEA/pkg/nexe, NW.js/VSIX qualification, semantic transforms and C++ MCP transport remain declared gaps. Keep those distinctions when updating #714–#718 acceptance; no issue state was changed.

## Existing CI and PR review snapshot

Sampled October 11 **01:02–01:03 UTC**, scoped to the exact heads below.

### dev e4236cb9

- [CI](https://github.com/NeverSight/NeverD/actions/runs/38092716664): in progress; Windows main job failed as above, Linux/macOS main jobs running.
- [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/38092716688): success.
- [Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/38092716728): queued overall; Ubuntu job succeeded, Windows/macOS queued.
- [Mobile Real Applications](https://github.com/NeverSight/NeverD/actions/runs/38092734506): skipped, not qualified.
- **4 workflow records:** 1 success, 1 in progress, 1 queued, 1 skipped.
- **24 exact-head checks:** 6 success, 1 failure, 2 in progress, 2 queued, 13 skipped. No whole-dev green result.

### Pre-existing open PRs

| PR | Exact sampled head | Check conclusions/statuses |
| --- | --- | --- |
| [#819](https://github.com/NeverSight/NeverD/pull/819) | `b43c1e53ad8650b7824919b3353e57d6f27fb75b` | 4 skipped, 12 queued, 3 success, 1 in_progress |
| [#818](https://github.com/NeverSight/NeverD/pull/818) (draft) | `62d7a7dc457b136efdeae13b717274f073a49372` | 4 skipped, 11 queued |
| [#817](https://github.com/NeverSight/NeverD/pull/817) | `82221f18ee6656849b67c80df0c4ce5508d49749` | 4 skipped, 6 success, 2 in_progress, 1 failure, 7 queued |
| [#815](https://github.com/NeverSight/NeverD/pull/815) (draft) | `67fbae1003fea5a76ebc9ddd3036e144b0329e66` | 5 queued, 8 skipped, 6 success, 1 failure, 1 in_progress |
| [#814](https://github.com/NeverSight/NeverD/pull/814) | `fdaef6f00be36e1a5e7d159e45efd195f2d0f3a5` | 4 skipped, 9 queued, 6 success, 1 in_progress |
| [#813](https://github.com/NeverSight/NeverD/pull/813) (draft) | `b4965d97c9263c2ccb76ba03c687f68283a782d0` | 3 in_progress, 8 skipped, 7 success, 2 queued, 1 failure |
| [#812](https://github.com/NeverSight/NeverD/pull/812) | `ae8f6ac51dd5191f83258a10dc1b553177fada2e` | 9 success, 8 skipped, 3 failure, 2 queued |
| [#811](https://github.com/NeverSight/NeverD/pull/811) | `7e8e1c3cef060f4fb16ccc7ba326737c129896a5` | 4 skipped, 9 success, 2 failure, 1 in_progress, 4 queued |
| [#808](https://github.com/NeverSight/NeverD/pull/808) (draft) | `26e1b6ff115034e7416d91dd3faa312abcba9cf4` | 8 skipped, 13 success, 2 in_progress, 3 failure, 6 queued |
| [#806](https://github.com/NeverSight/NeverD/pull/806) (draft) | `f3a5160fdacd5b52b92226834dd0b2dc2f1d17fb` | 8 skipped, 12 success, 3 failure, 1 in_progress |
| [#804](https://github.com/NeverSight/NeverD/pull/804) (draft) | `f9590bcae78e6338df93e28d585e7d7434c0c4d5` | 8 skipped, 11 success, 3 failure, 2 cancelled |
| [#801](https://github.com/NeverSight/NeverD/pull/801) (draft) | `4e228e7061bf7dc30ea0f8be91b05fa5fb1cefb7` | 4 skipped, 6 success, 5 failure |
| [#800](https://github.com/NeverSight/NeverD/pull/800) (draft) | `1278e456f0f5801a6d05f93df80c77ab8046c99a` | 4 skipped, 7 success, 4 failure |
| [#799](https://github.com/NeverSight/NeverD/pull/799) (draft) | `f726ebf1a05da11341c0b232ff32d47468c9766a` | 4 skipped, 5 failure, 6 success |
| [#798](https://github.com/NeverSight/NeverD/pull/798) (draft) | `db023526af879393475d8d24a34ba954d3349ace` | 4 skipped, 6 success, 5 failure |
| [#797](https://github.com/NeverSight/NeverD/pull/797) (draft) | `e64634719969d82502432ac9cbfa39a0cf465eef` | 4 skipped, 7 success, 4 failure |
| [#794](https://github.com/NeverSight/NeverD/pull/794) | `804c06ef1e464a18b11b1072e6383c4edc272652` | 4 skipped, 6 success, 4 failure, 1 in_progress |

All 17 PRs returned empty review-submission, inline-comment and conversation-comment collections, and no requested reviewers. Independent review remains pending. Check counts fit returned totals; no complete diagnosis of every PR failure is claimed. Local validation paragraphs in PR bodies remain author reports.

Dependency chains in PR descriptions require deliberate review: Darwin #811 → #814 → #817 → #819; EH #812/#813 → #815 → #818; driver #804/#806/#808; floating-state #797/#798/#799/#800 → #801. #794 explicitly retains a failing HighC exception-context case in its own reported broad run. These are review/planning dependencies, not permission to merge.

## Suggested priorities

1. **Clear the integrated Windows link blocker using the existing #818 repair.** Dependency: review the scoped LLVH symbol change and its EH stack; acceptance: exact integrated revision links neverd.dll and reaches required subsequent gates, without suppressing duplicate-symbol diagnostics. This task does not merge it.
2. **Reduce the 17-PR review queue by dependency chain.** Dependency: independent reviews, reconciled bases and exact-head platform evidence. Acceptance: distinguish failures, running/queued, skipped and author reports; retain the unresolved #794 exception-context limitation. No delivery dates are assumed.
3. **Qualify the newly integrated web evidence slice before claiming broad coverage.** Dependency: pinned artifact/profile identities, Windows capture/packaging work, original-byte/SRI and passive-publication contracts. Acceptance: executable evidence on each claimed host and version, plus explicit refusals for unsupported targets. The macOS-local Claude Code result does not close #714–#718.

## Daily log — 2026-10-11

- Inventoried 302 commits and 1,158 paths, excluding the prior report from substantive counts.
- Reviewed new package archive/SRI, stream/redaction/correlation and loop-cut boundaries, plus focused fixture source.
- Confirmed the existing Windows DLL collision from exact-head logs and located the already-published #818 repair; no redundant source change.
- Recorded 25 issues / 17 pre-existing PRs and the merged state of the prior queue/report.
- Updated this English snapshot while preserving all earlier content verbatim. No issue, label, owner, dependency, security setting, CI configuration, merge or deployment changed.

## Coverage and counting limits

Commit comparison pages were **100 + 100 + 100 + 2 + 0**. GitHub's capped 300-file diff was supplemented with complete old/current recursive trees (**6,819 / 7,395 entries**, neither truncated); renames count as removed/added paths. Open issues/PRs and updated issues exhausted at empty second pages. The latest 100 closed PRs extend to October 8, before this activity interval; merged_at separates merge from closure. CI/check second pages were empty. The sampled metadata can change after the cutoff.

Most of the 1,157 non-report changed paths remain outside detailed review, including broad EH, Darwin, CPU, GUI, emitter and web parser/container changes. No full-repository scan, build/test pass or release-readiness claim. Publication and remote readback are recorded in the delivery summary.

<details>
<summary>Previous snapshots and manual content (preserved verbatim)</summary>

# NeverD Daily Progress

Last verified: **2026-10-10 09:07 Asia/Shanghai (UTC+08:00) / 01:07 UTC**.

This is a bounded static review and a point-in-time GitHub snapshot, not a whole-repository audit or execution result. Priorities are suggestions, not delivery commitments. This topic-branch report is a proposal against dev, not a merge.

## Current snapshot and changes

- Pinned dev source: [ee6e0f54](https://github.com/NeverSight/NeverD/commit/ee6e0f54140eade0a29868331ea8bb78f164d92c). Previous reviewed source: [f242ee88](https://github.com/NeverSight/NeverD/commit/f242ee8897fd7172fb79046653bb6b555b54cab2). The intervening d2028756 integration, explicitly outside yesterday's review, is included in today's inventory but not exhaustively audited.
- [Comparison](https://github.com/NeverSight/NeverD/compare/f242ee8897fd7172fb79046653bb6b555b54cab2...ee6e0f54140eade0a29868331ea8bb78f164d92c): **293 commits / 669 changed paths**: 180 added, 486 modified and 3 removed. Excluding yesterday's two report commits aa03a653/8763c218 and their documentation-only merge 93b4b9d8, plus PROGRESS.md, leaves **290 non-report commits / 668 paths**. These include source, tests, documentation and merges; they are not implementation-completion metrics.
- Activity window: **October 9 01:10 UTC through October 10 01:07 UTC**, with collections sampled 01:04–01:06 UTC.
- **25 ordinary open issues**, up five; no ordinary issue closed in this interval. New [#714](https://github.com/NeverSight/NeverD/issues/714) JavaScript/bundles, [#715](https://github.com/NeverSight/NeverD/issues/715) offline Node package triage, [#716](https://github.com/NeverSight/NeverD/issues/716) desktop extraction, [#717](https://github.com/NeverSight/NeverD/issues/717) passive interface/protocol mapping and [#718](https://github.com/NeverSight/NeverD/issues/718) Bun/native JS CLI extraction are planning work, not implemented capability.
- **4 pre-existing open PRs**, versus two at yesterday's snapshot: #728, #744, #751 and draft #758. **51 PRs opened / 49 merged / 0 closed without merge** in the interval. Report-only #703 accounts for one opening and one merge; excluding it gives 50 openings / 48 merges. Today's report proposal is excluded from these pre-publication counts.
- Yesterday's [#689](https://github.com/NeverSight/NeverD/pull/689), [#702](https://github.com/NeverSight/NeverD/pull/702) and report [#703](https://github.com/NeverSight/NeverD/pull/703) are merged. Do not keep their old pending-review state.
- **No new independently proven defect in today's bounded NeverD source sample; no speculative code change.** The author added loop-prefix selection repair and FLS cleanup/retained-state handling, reviewed below.

## Static review scope and evidence

Read root AGENTS.md and CONTRIBUTING.md, relevant architecture/testing guidance and the roadmap. The complete pinned tree contains no nested AGENTS.md. Contribution guidance requires a topic branch against dev. The static-only constraint overrides build/test/formatter instructions: no project code, build, test, benchmark, repository script, dynamic analysis or manual CI action was executed.

### Reviewed source

1. **Multi-cut loop inference:** complete a580a0bf patch, including 173 added LowIRRefinementTests.cpp lines; current LowIRUndefinedIndependence.cpp **551–623, 3250–3338, 3470–3530, 4420–4485 and 5601–5713**, tracing cutpoint production, candidate reruns, transition state and the independent final checker.
   - inferMultiple now runs the complete entry segment before choosing first-arrival prefix templates. Separate bounded searches only fill missing nested cuts.
   - Cutpoint indices originate from the validated plan traversal. Candidate return lists are cleared for each run; transition starts are reset before the initial traversal.
   - Inference remains a proposal. The final checker replays original/candidate entry segments, establishes missing paired prefixes and checks every inductive segment. The new fixtures include reordered/padded arms, changed arithmetic, zero progress, changed entry values and independently bounded inference/proof budgets.
   - This is source evidence, not proof that the new tests pass or a whole-solver soundness claim.

2. **FLS callbacks and process termination:** complete production changes in fb5515e0 and 96906f99; current WindowsProcess.cpp **300–500 and 625–950**, Services.cpp's dynamic-local inventory, complete/exitCleanup and FlsAlloc/Free/GetValue/SetValue implementations, and full WindowsProcessLifetime.cpp. Also inspected changed FLSCleanup/ServiceOutcome/lifetime declarations.
   - Cleanup and loader work share the bounded continuation stack. Return handling completes the FLS record before restoring caller state; the allocated index cannot be reused while its callback is active. Recursive release of that same slot refuses explicitly.
   - Normal process exit performs current-fiber cleanup before DLL/TLS detach; startup failure remains separate. Completion clears the fiber value but retains the process-wide index. Later-index allocations can participate in the same bounded traversal.
   - Loader operations during exit cleanup are refused before Modules.begin can acquire/release references. Nested ordinary cleanup resumes the saved active call and return gate.
   - Inspected added WindowsLifetimeTests/WindowsSystemTests, FLS exit/leaf fixture sources and changed UnpackGeneratedTests/fixture source for nested callbacks, same-slot recursion, observer identity and event limits; none were run.
   - Other fibers/threads, all exception interactions, lower-index allocations during exit and undocumented Windows teardown behavior were not independently qualified. No speculative broadening of the declared model was made.

3. **Retained dynamic TLS/FLS and unpacking:** all interface/production hunks in dac53695, including ProcessObserver.h, Unpack.h/Unpack.def, UnpackJSON.cpp, and current Unpack.cpp **110–240** and ProcessTransfer.cpp **375–450**.
   - Allocated zero-valued TLS slots and nonzero unallocated TEB cells both retain state; FLS uses allocated-slot count. Absent inventory stays distinct from zero.
   - Capture propagates inventory errors instead of inventing emptiness. Normal reconstruction refuses unknown or live state; explicitly requested snapshots retain diagnostics and are labeled snapshots.
   - This does not reconstruct dynamic slot ownership or certify other thread/fiber state. Full container rebuilds, tail-helper behavior and native execution are outside this review.

**Coverage limits:** most of the 668 non-report changed paths remain unaudited, including broad GUI, ABI, loader, floating-point, Darwin, native backend and emitter changes. No full review of open PR implementations is claimed. Author-local test counts and benchmark timings are not our measurements. Feature issues #655/#656/#714–#718 were not implemented or mutated.

## Existing CI and review snapshot

Read-only sampling **October 10 01:05–01:06 UTC**. Each status belongs to its explicit source/head; passing PR checks do not validate dev.

### dev ee6e0f54

- [CI](https://github.com/NeverSight/NeverD/actions/runs/38003677498): in progress; Linux/macOS/Windows main jobs still running.
- [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/38003677512): success.
- [Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/38003677507): **failure**. Ubuntu and Windows succeeded; macOS failed at Swift source recovery.
- [macOS log](https://github.com/NeverSight/NeverD/actions/runs/38003677507/job/114067460307) independently shows 366 mobile tests passing and arm64 classic/default Swift variants completing, followed by **x86_64 classic/default failure**: doubleAdd, floatAdd, mixed, stackFloats and stackMixed lack an ordinary direct source binding. Only 2/4 Swift variants completed. Negative-control messages earlier in the log are not additional real failures.
- [Mobile Real Applications 38008461137](https://github.com/NeverSight/NeverD/actions/runs/38008461137): in progress with its upstream-regression gate failed; an earlier same-head run 38003688030 is skipped. Neither is a passed qualification.
- Five workflow records: **1 success, 1 failure, 2 in progress, 1 skipped**. 26 exact-head check records: **9 success, 2 failure, 5 in progress, 10 skipped**. Main-platform completion and downstream qualification remain unresolved.

### Open PR heads

| PR / exact head | Observed check summary | Review / dependency limits |
| --- | --- | --- |
| [#728](https://github.com/NeverSight/NeverD/pull/728), 396d371074093e329ff69bd76d10540f18c2b0f3 | 19: 14 success, 2 failure, 1 running, 2 skipped | Mergeable/unstable. Mobile all three hosts pass; main Linux/macOS fail, Windows runs. Its Swift repair is a pending integration candidate, not dev success. |
| [#744](https://github.com/NeverSight/NeverD/pull/744), 92090e9a4dbfc4cec64db197179f66810947c79e | 16: 5 success, 7 failure, 4 skipped | Conflicting/dirty. Published-package audit and all three main/mobile platform jobs fail. Description says new LLVM release publication and successful live audit are still pending; old-head Wine/native results do not qualify this head. |
| [#751](https://github.com/NeverSight/NeverD/pull/751), 44944e90fe4d7c2d6501f9952cb6541a203ab155 | 12: 7 success, 3 failure, 1 running, 1 skipped | Mergeability unknown at read. Main Linux/macOS and mobile macOS fail; Windows main runs. Extensive local test/timing claims remain author reports, including explicitly retained failures. |
| [#758](https://github.com/NeverSight/NeverD/pull/758), 82943cfc06d2ef6e4058ddbac46dbbe4dea6a327 | 19: 14 success, 3 running, 2 skipped | Draft, mergeable/unstable; depends on #728. Author explicitly reports the complete Darwin gate has not passed (three timeouts in its last reported epoch). Mobile all three hosts pass, main jobs run. |

All four returned review submissions, inline review comments and conversation comment collections were empty, with no requested reviewers. Pending independent review and unresolved CI/dependencies remain; metadata mergeability does not mean ready to merge. Full root causes of every PR check failure were not established.

## Suggested priorities

1. **Close the integrated Swift recovery gap without duplicating pending repairs.** Dependency: #728/#758 source review and exact-head qualification. Acceptance: the selected integrated head completes all four unchanged Swift variants and required mobile gates; separate ARM64 success from x86_64 failure and keep original bounds.
2. **Resolve the PR integration/qualification queue.** Dependency: #744 conflict resolution and authorized LLVM release/audit; #751/#728 failed-platform diagnostics; #758 prerequisite and Darwin timeout evidence. Acceptance: independent review and terminal exact-head required CI, with inherited failures, unavailable native targets and author reports explicitly separated. This daily task neither merges nor publishes an LLVM release.
3. **Sequence the new web-analysis plans behind explicit evidence contracts.** Dependency: #714–#718 extraction/provenance/artifact APIs and existing #655/#656 collaboration/adapter boundaries. Acceptance: choose a pinned offline corpus and a small extraction-to-analysis slice with version/hash identity, unsupported outcomes and bounded resources; keep passive protocol mapping separate from active requests. Planning is not implementation or a delivery-date commitment.

## Daily log — 2026-10-10

- Inventoried 293 commits and 669 paths; removed pure report content/merge from substantive counts.
- Reviewed loop-prefix selection, FLS continuation/exit and retained dynamic TLS/FLS boundaries plus focused regression source. No newly proven NeverD defect or source patch.
- Recorded 25 ordinary open issues, four pre-existing PRs, five new feature proposals and the actual merged state of yesterday's PRs.
- Diagnosed exact-dev mobile Swift failure from the existing log; distinguished pending PR mobile success from dev and retained other CI/review gaps.
- Updated only this English tracker proposal; preserved prior content verbatim. No dependency/security change, issue mutation, merge, deployment or active CI action.

## Counting and publication limits

The compare commit pages were **100 + 100 + 93 + 0**. GitHub's 300-file comparison cap was replaced by comparison of complete recursive trees: **6,641 and 6,819 entries**, neither truncated. Renames count as removed/added paths. Open issues, updated issues and open PRs exhausted at an empty second page. The latest 100 closed PRs extend back to October 8 03:29 UTC, before the activity window; merged_at distinguishes merges from non-merge closures. Check totals fit their 100-item pages; dev check/workflow second pages were empty. External/historical checks outside the sampled collections are not exhaustively covered.

No usable authorized engineering environment was available in the environment catalog; this static task used GitHub connector reads and authorized report publication. No execution verification was attempted. Readback and publication receipt are supplied in the delivery summary.

<details>
<summary>Previous snapshots and manual content (preserved verbatim)</summary>

# NeverD Daily Progress

Last verified: **2026-10-09 09:10 Asia/Shanghai (UTC+08:00)** / **2026-10-09 01:10 UTC**

This is a bounded static source review and a point-in-time snapshot of existing GitHub evidence. Priorities are suggestions, not delivery commitments. All previous tracker content is preserved below. This topic-branch report is a proposal against dev; publication does not mean it has been merged.

## Current snapshot

- Pinned source: [f242ee88](https://github.com/NeverSight/NeverD/commit/f242ee8897fd7172fb79046653bb6b555b54cab2), still dev at the final pre-publication read. Previous reviewed source: [4320d15e](https://github.com/NeverSight/NeverD/commit/4320d15ed24b52c4e631836e440f340b2d5c45a5).
- Activity window: October 8 02:11 UTC through October 9 01:10 UTC. Collections were sampled 01:06–01:09 UTC; later changes are outside this snapshot.
- **20 ordinary open issues**, up two: [#655 collaboration](https://github.com/NeverSight/NeverD/issues/655) and [#656 hzqst/reagent integration](https://github.com/NeverSight/NeverD/issues/656). No ordinary issue closed in the window.
- **2 pre-existing open PRs**: draft [#689](https://github.com/NeverSight/NeverD/pull/689) and non-draft [#702](https://github.com/NeverSight/NeverD/pull/702). This report's new draft is excluded from that count.
- **54 PRs merged**, **0 closed without merge**, and **53 PRs opened** during the window. Yesterday's #643, #645 and [#648](https://github.com/NeverSight/NeverD/pull/648) have merged.
- Source comparison inventory: **354 commits, 574 changed paths** (120 added, 454 modified, zero removed), not an exhaustive review count. [The comparison](https://github.com/NeverSight/NeverD/compare/4320d15ed24b52c4e631836e440f340b2d5c45a5...f242ee8897fd7172fb79046653bb6b555b54cab2) includes broad GUI, ABI, loader, emulation, symbolic and backend changes.
- Exclude pure report commit [6bdec2e2](https://github.com/NeverSight/NeverD/commit/6bdec2e28e47031824a0ed2388e7ef8a42d76f2a) and PROGRESS.md from substantive advancement: **353 non-report-only commits / 573 other paths**. These remain mixed source/docs/tests/merge inventory, not a business-progress metric. #648 includes a real fixture repair, so its entire merge is not excluded.
- **No new statically confirmed defect in the bounded sample; no speculative code changes.** Yesterday's optional-fixture repair is present in dev.

## Changes since the previous snapshot

- #648 merged October 8 03:29:05 UTC. The independent empty defaults for NEVERD_WDM_MULTIPLE_WAIT_FIXTURE and NEVERD_WDM_MULTIPLE_WAIT_CFG_FIXTURE are present in [DriverBackendParityCases.def:7–12](https://github.com/NeverSight/NeverD/blob/f242ee8897fd7172fb79046653bb6b555b54cab2/unittests/emulation/DriverBackendParityCases.def#L7-L12). This closes the unmerged-source status of that repair; no new build acceptance is inferred.
- [#701](https://github.com/NeverSight/NeverD/pull/701) adds complete conditional-domain retries, witness validation, retained singleton target facts and terminal partition obligations. This is today's focused correctness review.
- Merged source also includes [#684](https://github.com/NeverSight/NeverD/pull/684) CNF lookup changes, [#697](https://github.com/NeverSight/NeverD/pull/697) deferred scalar extraction, [#677](https://github.com/NeverSight/NeverD/pull/677) Darwin namespace transactions, [#698](https://github.com/NeverSight/NeverD/pull/698) Windows/i386 import contracts, [#699](https://github.com/NeverSight/NeverD/pull/699) placement-aware loading, and [#700](https://github.com/NeverSight/NeverD/pull/700) idle-analysis GUI visibility. Their complete implementations are outside this sample.
- #655 (enhancement, epic) and #656 (enhancement, plugins) are separate open, unassigned proposals without milestones or comments. #655 owns multi-session task isolation and reviewed shared publication, alongside #102/#108/#95. #656 targets the **hzqst/reagent** fork and can start with a read-only, capability-aware adapter before shared writes. Their acceptance checklists are future work, not delivered capability. No feature implementation or issue mutation was performed here.

## Bounded static review

Read root AGENTS.md and CONTRIBUTING.md; the full recursive tree contains no nested AGENTS.md. Read the relevant architecture/testing sections and roadmap. Contribution guidance requires focused topic branches against dev, so this report uses a draft PR even though the branch endpoint reports dev unprotected. The explicit static-only instruction takes precedence over repository requests to build, format and execute tests.

### Exact source coverage

At f242ee88, reviewed:

- Full lib/analysis/core/CompleteModel.{h,cpp}, CompletedTargetFacts.{h,cpp}, ConditionalImplication.{h,cpp}, DomainCoverage.{h,cpp}, and ProofNode.h.
- LowIRUndefinedIndependence.cpp:632–896 and 1556–1589, tracing cumulative node/query charging, mode-specific retries and indirect-target publication.
- FiniteValues.cpp:286–448, tracing pristine-domain cloning, model-required joint enumeration, encoding-error separation and final blocking UNSAT.
- CompleteModelTests.cpp, CompletedTargetFactsTests.cpp, ConditionalImplicationTests.cpp and DomainCoverageTests.cpp under unittests/devirtualization. Test source was inspected, not executed.
- BitVectorSolver.h assertion/clone/model contracts and BitVectorSolver.cpp:141–270, including failed-encoding propagation into check().
- The repaired optional-fixture defaults in DriverBackendParityCases.def.

### Findings and static evidence

1. **Concrete witnesses remain complete-assignment evidence.** CompleteModel validates reachable topological operands and operator widths before evaluation, requires every referenced variable at exactly its declared width, and refuses absent model values. Its production caller only upgrades an unresolved feasibility/path-pair query to SAT after the original whole predicate is Satisfied. Refuted or incomplete witnesses do not produce UNSAT. Source tests separately cover missing variables, width mismatches, malformed operands, resource boundaries and incompatible partial models.

2. **Completed target facts require complete enumeration.** Storage is bound to the actual context and admits only Complete single-tuple/single-value results. Premise reuse requires the exact stored condition or every conjunct under the current domain; unrelated or weaker domains do not qualify. Additive reduction removes matching occurrences one at a time, preserves modular constants and remaining multiplicity, and still enumerates the reduced expression under the entire original predicate with unchanged query/node limits. Final blocking UNSAT is required before the nonconstant enumeration result becomes a fact. The inspected caller schedules only completed target sets.

3. **Conditional normalization keeps proof obligations.** Preparation validates operand topology and widths, derives equivalences under the domain, and retains reconstruction definitions. Cache identity includes the context, exact domain, all currently declared solver-policy fields and preparation limits. Reuse stores pristine encoding, not a previous goal's answer; every new goal is rewritten and proved with an independent encoding clone. Unknown compound goals split into bounded batches, and every term must be proved before success. The caller supplies the shared cumulative query allowance. Failed assertTrue calls poison subsequent check results, so ignoring their Boolean return at these call sites does not turn partial encoding into a successful proof.

4. **Terminal coverage never assumes observed arms are exhaustive.** Domain factors are removed only when syntactically present or separately proved implied; unresolved factors stay in the complete final question. Partition selection first proves selector values exhaustive under the current domain, then requires every child group, including the last, to prove its whole residual coverage. Unknown/Invalid refuses completion. Operand lists are copied before expression construction, and post-callback node checks preserve the caller's ceiling.

**Result:** No independently confirmed new defect in these paths. No runtime, performance, formal end-to-end soundness or full-repository claim is made. Deeper SAT/CNF internals, all symbolic rebuild/evaluator semantics, every proof-request constructor and all native callers remain unaudited here.

### Questions not established as defects

- PR #702's author reports pre-existing standalone i386 wide-return inference, indirect/LLVM return typing and mov-style outgoing-stack-slot limitations. Those claims identify useful follow-up source areas, but this review did not reproduce or independently establish their triggers; no guessed repair was made.
- Test bodies specify malformed-input and resource-boundary rejection. They do not establish that these tests pass on the combined dev head, nor that all malformed graphs are covered.

## Existing CI and review evidence

Read-only sampling at October 9 **01:08–01:09 UTC**. No workflow was dispatched or rerun.

### Pinned dev f242ee88

- [CI](https://github.com/NeverSight/NeverD/actions/runs/37865019546) was in progress.
- [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/37865019555) succeeded.
- [Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/37865019547) had a queued workflow-level state while its job records included Ubuntu success, macOS running and Windows queued. These are distinct sampled API states, not a claim that all jobs were queued.
- 12 exact-head check records: **6 success, 4 in progress, 1 queued, 1 skipped, 0 failure** at that sample. Main Linux/macOS/Windows were still running. The ARM64 KVM/WHP build checks and both Windows caller-context checks succeeded; native CPU execution was skipped.
- Incomplete evidence is not acceptance. Yesterday's failed or unfinished statuses are not transplanted onto this head; neither are passing PR checks.

### Open PRs

- **#689**, head 5df09503086d12626dc0fe6c4d612ac07bd349a5: draft, metadata mergeable/unstable. 19 checks: **13 success, 4 in progress, 2 skipped**. Main Linux/macOS/Windows and mobile Windows were running; mobile Linux/macOS succeeded. Its extensive clean-head Darwin/HVF/local acceptance numbers are **author-reported**, not independently executed or validated by this review. Native Intel/physical iOS gaps are explicitly acknowledged by the author.
- **#702**, head 66b80c0bed26bd44e4752df7bac37da7b71eb186: non-draft, metadata mergeable/unstable. 12 checks: **1 success, 4 in progress, 6 queued, 1 skipped**. Its local corpus timings, pass/fail comparisons, changed skip behavior and stopped long whole-image emission are **author reports**, not review measurements or complete acceptance.
- Both PRs had **zero submitted reviews, inline comments and conversation comments**, and no requested reviewers in the returned complete collections. Pending review and unfinished exact-head CI remain blockers; neither PR was merged or edited by this review.

## Suggested next priorities

1. **Review the current integration candidate with complete evidence.** Dependencies: #689/#702 owners, independent source review and existing CI. Acceptance: a named head has terminal platform results; author-local runs, skipped cases, inherited failures and actual supported/native coverage are itemized separately. For #702, inspect skip changes and the remaining i386/LLVM contract gaps rather than treating an unchanged failure count as proof of correctness.

2. **Keep #701's proof retry boundary fail-closed.** Dependencies: the complete-domain helper contracts and unchanged cumulative limits. Acceptance: source review plus separately authorized normal CI cover stale context/domain/policy identities, missing final branches/terms, wrong-width or missing model variables, incomplete enumeration and exact/short budgets on the candidate head. This task performs static review only.

3. **Plan the adapter and collaboration milestones separately.** Dependencies: #656 targets hzqst/reagent and reuses #108; #655 shares publication/persistence/type contracts with #102/#95; #580 owns measured concurrency/latency qualification. Acceptance: first define a pinned read-only adapter contract with honest capabilities, paging/chunks and owned/attached lifecycle; separately define two-session isolated work and conflict-safe publication. No shared-writer safety or parallel-speedup claim follows merely from MCP connectivity.

## Daily log

### 2026-10-09 — Complete-domain proof and cache boundaries

- Enumerated 354 comparison commits and 574 changed paths; separated the prior report-only content commit/path from substantive work.
- Reviewed the exact source/configuration/test-source scope above; found no new statically proven bug and made no speculative code repair.
- Verified #648 merged and the fixture defaults are present; recorded #655/#656 as independent planning work.
- Captured 20 open ordinary issues, two pre-existing open PRs, 54 merges, zero unmerged closures, and their exact-head CI/review limitations.
- Prepared only this English tracker update through the documented topic-branch/draft-PR workflow, retaining prior text verbatim. No issue state, label, assignee, dependency, security permission or workflow configuration was changed.
- No build, test, benchmark, formatter, project execution, repository script, dynamic analysis, workflow dispatch or CI rerun was performed.

## Coverage and counting limits

The commit comparison was paginated **100 + 100 + 100 + 54 + 0**. Its first-page 300-file cap was not treated as a complete inventory: complete recursive trees (**6,517 and 6,641 entries**, neither truncated) were compared by non-directory path/blob identity. Renames would count as removed/added paths. The changed-path count is not the number of audited files.

Open and updated issue/PR collections each had an explicitly empty second page. Closed PR metadata returned the latest 100 updated entries, extending to October 6 19:27 UTC, before the activity window; merged_at and closed_at were used to distinguish merges from other closures. Counts are repository activity, not implementation completion. #648's report content commit is excluded from substantive commits, but its real fixture fix remains. Today's report-only proposal is excluded from pre-publication counts.

Both open PRs' review/comment collections were empty. The exact dev workflows/checks and both PR check collections had empty second pages. CI is a changing point-in-time observation; no exhaustive historical/external-check claim is made. Most of the 573 non-tracker changed paths, the full backend/GUI/ABI/Darwin/unpack changes and all runtime/benchmark behavior remain outside this bounded review.

## Post-snapshot publication check — 2026-10-09 01:12 UTC

During report publication, [#702](https://github.com/NeverSight/NeverD/pull/702) merged at 2026-10-09T01:10:50Z. Dev advanced to [d2028756](https://github.com/NeverSight/NeverD/commit/d2028756c25560931d364a93801908f8a7c79354). That new source is **outside the pinned static review and CI snapshot above**; the 01:10 counts remain historical. Its merge adds one to the recorded merge count and removes #702 from the pending-review priorities. Apply priority 1 to review/qualification of the integrated candidate instead of waiting to merge #702.

This documentation-only proposal is [draft #703](https://github.com/NeverSight/NeverD/pull/703). Remote readback of initial report commit [aa03a653](https://github.com/NeverSight/NeverD/commit/aa03a653cb02a458356cc125357cbdd1ed51f204) verified only PROGRESS.md changed (108 additions, zero deletions) and every prior character remained. Dev's tracker blob was unchanged at the publication recheck, and #703 was mergeable/unstable. An automatically triggered [LLVM Style workflow](https://github.com/NeverSight/NeverD/actions/runs/37868585705) for that initial documentation head was queued, not manually dispatched; it is not validation of the newer integrated dev source. No merge was performed by this review.

## Previous snapshot (preserved)

<details>
<summary>2026-10-08 snapshot and all earlier history</summary>

# NeverD Daily Progress

Last verified: **2026-10-08 10:11 Asia/Shanghai (UTC+08:00)** / **2026-10-08 02:11 UTC**

This is a bounded static source review and a snapshot of existing GitHub evidence. Priorities are suggestions, not delivery commitments. The complete previous tracker is preserved below.

## Current snapshot

- Reviewed dev: [4320d15e](https://github.com/NeverSight/NeverD/commit/4320d15ed24b52c4e631836e440f340b2d5c45a5).
- Previous source baseline: [db18af34](https://github.com/NeverSight/NeverD/commit/db18af34bbac577289a83896c142eebaa4ac108d).
- Activity window: October 7 01:01:56 UTC through October 8 02:11 UTC. Before this report's proposal: **18 ordinary open issues**, unchanged; **2 open PRs**, #643 and #645, replacing yesterday's #589 and #605. No ordinary issue was updated, opened or closed in the window.
- **38 PRs merged**, including yesterday's actual HighC repair #610; **0 PRs closed without merge** in the window. There were 37 newly opened PRs in the window. The new [draft #648](https://github.com/NeverSight/NeverD/pull/648) is additional tracking/repair work and is excluded from those pre-publication counts.
- Raw comparison inventory: **280 commits**, **862 changed paths** (298 added, 59 removed, 505 modified). One changed gitlink is third_party/capstone. These are inventory counts, not reviewed-file or implementation-only counts.
- The pure report commit [550608ad](https://github.com/NeverSight/NeverD/commit/550608adac98c28597b36272aedccdad3df2adff) and PROGRESS.md are excluded from substantive advancement: 279 non-report-only commits and 861 other paths remain. #610 also contains a real bug fix, so its entire merge must not be discarded as report-only.
- **One statically confirmed optional-fixture compilation defect repaired**, in [786f9d57](https://github.com/NeverSight/NeverD/commit/786f9d57fb89e4bc54221aab5fddaa178096a504), proposed through draft #648; no merge performed.

## Changes since the prior snapshot

- [#610](https://github.com/NeverSight/NeverD/pull/610), the captured HighC load repair, merged October 7 13:29 UTC. [#605](https://github.com/NeverSight/NeverD/pull/605) finite-proof reuse and [#589](https://github.com/NeverSight/NeverD/pull/589) Darwin observations also merged. Their former unmerged status is resolved; this does not establish combined-head runtime acceptance.
- [#614](https://github.com/NeverSight/NeverD/pull/614) added completed model-free query reuse; [#646](https://github.com/NeverSight/NeverD/pull/646) added bounded full-query ordering retries. Those changed boundaries are the focused correctness sample below.
- [#622](https://github.com/NeverSight/NeverD/pull/622) added Windows multiple-object waits, including the optional fixture catalogue that exposed today's compiler defect.
- Other delivered source includes GUI/listing/string work, [#611](https://github.com/NeverSight/NeverD/pull/611) unpack import-tail repair, [#633](https://github.com/NeverSight/NeverD/pull/633) ARM64 backend build coverage, and [#641](https://github.com/NeverSight/NeverD/pull/641) Windows process writes. Their full implementations were not audited here.

## Bounded static review and repair

Read root AGENTS.md, CONTRIBUTING.md, relevant architecture/testing guidance, and the complete tree inventory; no nested AGENTS.md exists in this tree. The user's static-only restriction overrides instructions to build or run tests. Followed the topic-branch/draft-PR contribution workflow despite dev being reported unprotected.

### Optional WDM multiple-wait fixture compilation

**Trigger:** Configure without either optional genuine WDK multiple-wait fixture, as allowed by CMake.

**Evidence and cause:**
- [CMakeLists.txt:530–533,838–861](https://github.com/NeverSight/NeverD/blob/4320d15ed24b52c4e631836e440f340b2d5c45a5/unittests/emulation/CMakeLists.txt#L838-L861) declares optional paths and emits compiler definitions only for existing files. Lines 1263–1302 forward those definitions to NeverDNativeDriverTests.
- [DriverBackendParityCases.def:6–13](https://github.com/NeverSight/NeverD/blob/4320d15ed24b52c4e631836e440f340b2d5c45a5/unittests/emulation/DriverBackendParityCases.def#L6-L13) used NEVERD_WDM_MULTIPLE_WAIT_FIXTURE and NEVERD_WDM_MULTIPLE_WAIT_CFG_FIXTURE unconditionally without the empty defaults used by other optional entries.
- DriverNativeExecutionTests.cpp:21–24 and DriverBackendParityTests.cpp:20–23 expand catalogue paths into constant character arrays. Missing macros therefore become undeclared identifiers, rather than optional unavailable fixtures.
- Existing exact-head [macOS CI](https://github.com/NeverSight/NeverD/actions/runs/37712812675/job/113102816237) independently reports both undeclared identifiers at October 8 01:59:30 UTC. That is a pre-test compile failure.

**Minimal repair:** Six lines add independent guarded empty-string defaults in the shared catalogue. Supplied compiler definitions remain unchanged; both workload registrations remain. The two consuming suites already check is_regular_file and report unavailable fixtures with GTEST_SKIP. DriverMultipleWaitTests.cpp uses its separate compile-time guards and does not include this catalogue; its execution gating is untouched.

**Static verification:** Re-read the target branch's latest blob before writing, then read the remote commit and exact six-line diff. No dependency, workflow, permission, production behavior or test budget was changed. Existing open #643 and #645 touch neither repaired file nor this fixture contract; no related unmerged proposal existed to reuse. This source-level repair is not a claim that builds or tests now pass. No new regression executable was added or run; the existing two consuming targets exercise the formerly uncompilable configuration when normal CI builds them.

### Completed answers and ordered full-query retry

Reviewed full lib/analysis/core/CompletedQueryCache.h and OrderedQuery.h; LowIRUndefinedIndependence.cpp:341–392,549–627,745–800,2369–2420; full CompletedQueryCacheTests.cpp and OrderedQueryTests.cpp; BitVectorSolver.h assertion/check contracts and BitVectorSolver.cpp:145–250; the changed regression/target registration hunks in #614/#646.

- CompletedQueryCache validates the actual context address, nonzero/in-range one-bit references and node ceiling before access. Packed shifts range from 0 to 62; geometric allocation remains bounded, value-initialized and retains previous words. Conflicting completed answers and Unknown/Invalid stores are refused.
- Checker construction binds caches after its actual session-or-owned context is selected. Node limits precede cache hits; non-immediate hits retain the logical query charge. Unknown/Invalid answers fail before insertion, while checker-local ownership prevents carrying evidence into a new independent checker.
- Ordered retry first asks the complete original predicate. It retries only a search-Unknown without an encoding error and an original direct nonconstant Boolean conjunct, with at most 64 operands inspected. The retry charges the same caller allowance and asserts both the preferred conjunct and the entire original predicate; it cannot accept a partial condition. Solver options remain fixed except model construction is disabled.
- Source tests specify packed-slot/context/budget isolation, conflicting stores, complete SAT/UNSAT versus unknown results, exact retry accounting and malformed/oversized shape refusal. These are descriptions of test code, not results produced by this review.

**Result:** No second statically proven correctness defect in this bounded sample. Solver internals, all native refinement paths and complete symbolic-context mutation invariants are not exhaustively audited. No speculative repair was made.

## Existing CI and review evidence

Sampled October 8 02:09 UTC, pinned dev 4320d15e:
- Four workflows: main CI and Mobile Decompilation still in progress; LLVM Style failed; Mobile Real Applications skipped.
- 21 exact-head checks: **5 success, 3 failure, 3 in progress, 10 skipped**.
- Main macOS failed on the two missing optional fixture macros described above. Main Linux and Windows remained running.
- [LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/37712812679) failed clang-format checks in the changed HighIR/C source range. The existing [format evidence artifact](https://github.com/NeverSight/NeverD/actions/runs/37712812679/artifacts/11522591295) is available. Formatting was not run or changed by this review.
- [Mobile macOS](https://github.com/NeverSight/NeverD/actions/runs/37712812668/job/113102718100) failed with an incomplete Objective-C scalar execution matrix: 2/4 completed, x86_64-classic and x86_64-default missing. This log establishes incomplete acceptance, not the underlying root cause. Mobile Ubuntu succeeded; Windows remained running.
- Both ARM64 native-backend **build** jobs (KVM and WHP) succeeded, as did both Windows caller-context jobs. Build coverage is not native runtime acceptance. All nine real-application qualification checks were skipped.

Open proposals, sampled October 8 02:09–02:11 UTC:
- [#643](https://github.com/NeverSight/NeverD/pull/643), head 953c369601605f3f9661489a26ccd8057dc3f1b6, non-draft: 12 checks, 8 success, 3 failures (main Linux/macOS/Windows), 1 skipped. Metadata mergeability was unknown; do not call it conflicting solely from that response.
- [#645](https://github.com/NeverSight/NeverD/pull/645), head 92f6fdc50eed2d2bd70c5da272647e3c98e000ff, non-draft: 19 checks, 14 success, 3 failures (main Linux/macOS/Windows), 2 skipped. It depends on #643; metadata reported mergeable but unstable. All three mobile jobs passed on this PR head, which cannot be transferred to dev.
- Both had no submitted reviews, inline review comments, conversation comments or requested reviewers in the returned collections. Their local/native validation claims remain author reports; their complete code and failed main-job causes were not audited here.

## Suggested next priorities

1. **Review the optional-fixture repair and complete exact-head compile evidence.** Dependency: draft #648 and the existing CI pipeline. Acceptance: both catalogue consumers compile with neither/one/both fixtures configured; supplied fixture paths retain their values and absent fixtures remain explicit skips. No execution is authorized as part of this static task.
2. **Resolve current integration blockers without weakening coverage.** Dependencies: current HighIR formatting ownership, missing Objective-C x86_64 evidence, #643 before #645. Acceptance: a named candidate has terminal platform results, complete intended mobile matrices and actual application qualification; skips/running jobs/local claims are not counted as acceptance.
3. **Keep performance planning correctness-qualified.** Dependency: [#580](https://github.com/NeverSight/NeverD/issues/580), pinned corpus and one coherent Release candidate. Acceptance: load-to-final-output latency, process-tree memory and scaling include completion denominators and raw source/hardware identities. New caches/retries are not evidence of measured improvement.

## Daily log

### 2026-10-08 — Optional-fixture compile repair and complete-query boundaries

- Inventoried 280 commits and 862 paths; excluded the pure report-only commit/path from substantive advancement. Reviewed the bounded files/ranges above, not the entire diff or repository.
- Committed the statically proved six-line optional-fixture default fix as 786f9d57; proposed it in draft #648. No merge, issue-status, label or assignee change.
- Reconciled yesterday's #610/#589/#605 merges; 18 ordinary issues remain open. Captured the two pre-existing open Darwin proposals separately from this report's draft.
- Read existing CI results only. No build, test, benchmark, project execution, repository script, formatter, dynamic analysis, workflow dispatch or rerun was performed.
- Preserved the previous tracker verbatim below; this proposal does not imply PROGRESS.md has already changed on dev.

## Coverage and counting limits

The compare endpoint was paginated 100 + 100 + 80 + 0 commits. Its capped 300-file list was not used as a complete file inventory: complete recursive trees (6,278 and 6,517 entries, neither truncated) were compared by path/blob identity; renames count as deletion/addition. Open issues/PRs and updated activity were each followed by empty pages. The updated collection contained 41 PR records and no ordinary issues; merged/closed timestamps were reconciled with closed-PR metadata (100 newest updated records, extending earlier than the window), not inferred from closed_at alone. Open-PR review/comment collections were empty. Exact dev workflow/check collections and both PR check collections had explicitly empty second pages. No claim is made about all external checks, all historical CI or full source coverage. The broad remaining 861 non-tracker changed paths were not all audited; only the explicitly listed source/configuration ranges were.

## Previous snapshot (preserved)

<details>
<summary>2026-10-07 snapshot and all earlier history</summary>

# NeverD Daily Progress

Last verified: **2026-10-07 09:01:56 Asia/Shanghai (UTC+08:00)** / **2026-10-07 01:01:56 UTC**

This is a bounded, point-in-time source and existing-evidence review. The [roadmap](docs/roadmap.md), [architecture](docs/architecture.md), [testing guide](docs/testing.md) and [contribution guidance](CONTRIBUTING.md) remain authoritative. No completion percentage or release-readiness claim is implied.

## Current snapshot

Source review is pinned to [db18af34](https://github.com/NeverSight/NeverD/commit/db18af34bbac577289a83896c142eebaa4ac108d), observed October 7 00:57 UTC. The previous reviewed source was [6f885ebb](https://github.com/NeverSight/NeverD/commit/6f885ebb7fda032077838950be03e31b90ffa593). Activity below runs from the previous report publication, October 6 01:06:21 UTC, through today's collection and excludes this new proposal.

| Measure | Verified state |
| --- | --- |
| Open ordinary issues | 18, up one; 14 epics, no milestones, only #12 assigned |
| Open PRs | 2: #589 and #605; yesterday's #525 and #565 merged |
| PRs merged in activity window | 40 including report-only #568; 39 after that exclusion |
| Closed PRs without merge | 0 |
| Ordinary issue activity | #580 opened; no other ordinary issue updated |
| Raw source inventory | 198 distinct commits; 557 changed file paths |
| Path classification | 385 modified, 170 added, 2 removed; no submodule revision changes |
| Report-only exclusion | 196 commits and 556 paths, excluding #568's content commit, merge commit and PROGRESS.md |
| Confirmed defects | One HighC forwarding miscompile fixed below; four-site Darwin compile blocker already repaired in open #589 |
| New code fixes from this review | 1 minimal HighC repair plus one focused regression, statically inspected only |
| Qualification | Current dev has observed failures and unfinished coverage; no complete green gate |

The [source comparison](https://github.com/NeverSight/NeverD/compare/6f885ebb7fda032077838950be03e31b90ffa593...db18af34bbac577289a83896c142eebaa4ac108d) was enumerated as 100 + 98 commits, then an empty third page. GitHub's comparison file list caps at 300. Path counts therefore use complete recursive trees of 6,099 and 6,278 entries, neither truncated, excluding directories and comparing path/blob identity; renames count as old/new paths. This inventory is not an exhaustive review count or a count of implementation-only commits.

[#568](https://github.com/NeverSight/NeverD/pull/568) merged at October 6 05:42:05 UTC. Both [6e2272a1](https://github.com/NeverSight/NeverD/commit/6e2272a15ec7b2bacbfc0d5ed305927ff526ce95) and its [36e8142c merge](https://github.com/NeverSight/NeverD/commit/36e8142c2d1fd5d45a326abbf9b3d304f677a670) change only PROGRESS.md and are excluded from substantive advancement. The entire current 135,547-character tracker, including manual edits and earlier history, is preserved below.

### Changes since the previous snapshot

- Parallel checked CPUs/MMIO atomicity [#565](https://github.com/NeverSight/NeverD/pull/565) and bounded Darwin services [#525](https://github.com/NeverSight/NeverD/pull/525) merged. Darwin removal/rename [#581](https://github.com/NeverSight/NeverD/pull/581), Windows preemption [#582](https://github.com/NeverSight/NeverD/pull/582) and priorities [#602](https://github.com/NeverSight/NeverD/pull/602) followed. Their broad concurrency/OS semantics are outside today's bounded audit.
- Linux trailing-slash and filesystem query contracts landed in [#570](https://github.com/NeverSight/NeverD/pull/570) and [#572](https://github.com/NeverSight/NeverD/pull/572). Process-observation unpacking [#579](https://github.com/NeverSight/NeverD/pull/579), direct x64 execution [#594](https://github.com/NeverSight/NeverD/pull/594), and execution watches [#597](https://github.com/NeverSight/NeverD/pull/597) are delivered source, not independently established runtime coverage.
- Inference and solver work includes predicate retention [#590](https://github.com/NeverSight/NeverD/pull/590), branch-local encodings [#592](https://github.com/NeverSight/NeverD/pull/592), pristine finite-domain encodings [#598](https://github.com/NeverSight/NeverD/pull/598), prepared keys [#599](https://github.com/NeverSight/NeverD/pull/599), inline SAT watches [#600](https://github.com/NeverSight/NeverD/pull/600), root queues [#601](https://github.com/NeverSight/NeverD/pull/601) and reference bounds [#604](https://github.com/NeverSight/NeverD/pull/604).
- ARM64 tail-frame checks [#606](https://github.com/NeverSight/NeverD/pull/606), scalar C declarations/literals [#607](https://github.com/NeverSight/NeverD/pull/607), and shared absolute dispatch [#608](https://github.com/NeverSight/NeverD/pull/608) merged. HighIR forwarding was tightened at the pinned head. Individual author corpus/semantic results cannot establish this combined head's qualification.
- [Issue #580](https://github.com/NeverSight/NeverD/issues/580) requests real end-to-end latency, memory and scaling baselines. It remains open, unassigned and without comments. Its measurement contract is future work, not a benchmark result.

## Bounded static review

**Mode:** Source, interfaces, relevant callers, declared regressions and existing CI only. No project execution, build, test, benchmark, repository script, formatter/linter or dynamic analyzer was run. No workflow was dispatched or rerun. No CI-skip marker is added; ordinary automatic checks may run.

### Finite projection, prepared keys and frame proofs

Reviewed the full `lib/analysis/core/{FiniteValues.h,FiniteValues.cpp,FiniteQueryCache.h,FiniteQueryCache.cpp,FrameOffsets.cpp}`; focused clone contracts in `include/neverd/solver/BitVectorSolver.h:150–168`, `lib/solver/bv/BitVectorSolver.cpp:141–154` and `lib/solver/sat/SatSolver.cpp:340–350`; and regression bodies in `FiniteValuesTests.cpp:21–92,650–747`, `FiniteQueryCacheTests.cpp:399–506`, and `FrameOffsetsTests.cpp:393–434` under `unittests/devirtualization`.

- [Finite projection validation](https://github.com/NeverSight/NeverD/blob/db18af34bbac577289a83896c142eebaa4ac108d/lib/analysis/core/FiniteValues.cpp#L312-L325) checks nonzero/in-range references and widths before constant fast paths or node access. The existing #604 repair is present. Invalid input cannot invoke the observer, spend a solver query or create nodes through these entry points.
- `FiniteDomainEncoding` fixes the context/settings, forces model construction and stores only a pristine predicate encoding. Each projection/search mutates an independent solver copy; replacing the predicate releases the old template. Clone eligibility rejects searched or failed encoding state. Full internal SAT/watch-list ownership was not audited.
- Enumeration requests explicit joint model variables, refuses missing values and observer abandonment, and still requires the final UNSAT query. Budget/unknown failures return no partial tuples. Encoding reuse is distinct from reuse of a completed mathematical domain.
- [Prepared keys](https://github.com/NeverSight/NeverD/blob/db18af34bbac577289a83896c142eebaa4ac108d/lib/analysis/core/FiniteQueryCache.h#L35-L65) own complete serialized keys and projection widths without retaining context references. Moves invalidate the source token. Serialization retains predicate, projection order, limit, widths, operator payloads, constant bits and consistent variable sharing. Only Complete/TooManyValues results are admitted; malformed, oversized and incomplete entries are refused before eviction.
- `FrameOffsets` prepares once, looks up and stores the same token. Its symbolic addend rewrite preserves full modular dependence on the root/predicate under a shared 16-visit budget. Singleton proof, node checks and infeasible/nonunique/invalid/budget distinctions remain explicit; this does not independently prove reachability or memory accessibility.
- Declared regressions cover invalid predicate/projection references before fast paths; fresh/copy tuple and observer equivalence, exact/one-short budgets, predicate replacement and failed-projection isolation; prepared-key context destruction, renaming, moves, storage limits, projection order and empty/nonunique domains. These tests were read, not executed.

**Result:** No additional statically proven defect in this boundary; no speculative code change. This is not a proof of all recovery callers, the complete solver or native refinement.

### Captured HighC loads crossing memory assignments

**Confirmed and fixed:** `collectValueForward` recognizes Store statements and assignments to scalar/stack variables, but omitted the supported Assign-to-Load memory-write representation. For `store(slot, x); t1 = load(slot); assign(load(slot), 5); return t1`, it could replace the captured value with a fresh load after the overwrite, returning 5 instead of x. This is established by the source/control flow, not by executing the example.

- [The old clobber check](https://github.com/NeverSight/NeverD/blob/db18af34bbac577289a83896c142eebaa4ac108d/lib/backend/c/HighC/HighCFuncWriter.cpp#L3331-L3353) lacks this write form. [Statement emission](https://github.com/NeverSight/NeverD/blob/db18af34bbac577289a83896c142eebaa4ac108d/lib/backend/c/HighC/HighCStmtWriter.cpp#L448-L535) and existing `HighCStoreForwardingTests.cpp:651–674` explicitly support it.
- Earlier store forwarding refuses Assign-to-Load as an immutable-slot sequence; liveness/frame-write cleanup retain the overwrite and observed load. A pure-load candidate bypasses the later adjacency gate. The issue also exists in parent `fc969e8cf77f1371c2443000c76c0cddf4111093`; the pinned head's scalar-copy repair does not cover it.
- [Fix a6468b80](https://github.com/NeverSight/NeverD/commit/a6468b80f37d24146fd5a40a08eaf7be987ad58f) classifies both statement forms through the existing address/overlap check, using the assignment destination's access width. It preserves the existing alias policy and changes no shared IR or LLVMC behavior.
- Added `HighValueForward.SlotLoadStaysBeforeAnAssignmentToItsSlot` to the already-registered `NeverDHighControlFlowTests`. It checks the retained temporary and declares O0/O2 emitted-C behavior for inputs 7 and 9 using the existing helper. Neither emitter nor regression was run.
- #589 and #605 touch neither changed file; there is no existing unmerged related HighC proposal to reuse.

Reviewed ranges under `lib/backend/c`: `HighC/HighCFuncWriter.cpp:324–435,946–1115,2330–2585,2826–3673,4720–4869`; `HighC/HighCExprWriter.cpp:84–154,1040–1090,2470–2520,3176–3204,3351–3364,3914–4055`; `HighC/HighCStmtWriter.cpp:282–555,3195–3344`; `HighC/HighCEmitter.cpp:2117–2245`; `pass/HighC/HighCStoreForwarding.cpp:34–210`, `HighCDeadStoreAnalysis.cpp:201–525` and `HighCVoidAnalysis.cpp:316–346`. Also reviewed HighIR statement/expression declarations, relevant HighCWriter/HighCPasses declarations, all `HighValueForwardTests.cpp`, `HighCStoreForwardingTests.cpp:516–735`, target registration and the parent's forwarding range.

**Limits:** No second HighC defect or corresponding LLVMC defect was established. Other forwarding passes, address-taken scalar effects, complete alias analysis and end-to-end native equivalence are not exhaustively proven.

### Darwin regression compilation blocker

[Existing Linux CI](https://github.com/NeverSight/NeverD/actions/runs/37548781094/job/112559177441) reports conflicting initializer-list element types at `unittests/emulation/DarwinFileTests.cpp:1076` and `DarwinVectorTests.cpp:152,162,271`. Static inspection confirms the cause: braced range initializers mix `uint64_t`/UINT64_MAX, which this Linux compiler defines as unsigned long, with ULL expressions, which are unsigned long long. The declared range variable does not select the initializer-list's element type. This is a compile blocker before the tests run, not evidence of failing Darwin runtime semantics.

All four type corrections already exist in [#589](https://github.com/NeverSight/NeverD/pull/589), head `dbb93b662f41b71b34ac76ac81833a977d04e66c`: explicit `uint64_t` operands in [DarwinFileTests.cpp](https://github.com/NeverSight/NeverD/blob/dbb93b662f41b71b34ac76ac81833a977d04e66c/unittests/emulation/DarwinFileTests.cpp#L3560) and [DarwinVectorTests.cpp](https://github.com/NeverSight/NeverD/blob/dbb93b662f41b71b34ac76ac81833a977d04e66c/unittests/emulation/DarwinVectorTests.cpp#L148-L166). That branch is unmerged. Its existing exact-head Linux default-target build passed before later tests failed, supporting the compile repair on that PR only. No duplicate repair was created; this does not establish a completed dev build.

### Performance measurement boundary

Re-read `lib/sdk/capi/NeverDCAPIBench.cpp:58–140` at the pinned source; the blob is unchanged from the previous review. Loading still precedes `T0Total`, and LowIR, MedIR, HighIR and LLVM still use separate pipeline runs. Their total is not one load-to-final-output operation. [#580](https://github.com/NeverSight/NeverD/issues/580) remains the relevant acceptance task: preserve completion denominators and strict semantics while measuring production workflows. No latency, memory, scaling improvement or regression is inferred from source inspection.

## Existing CI and review evidence

### Exact reviewed dev head

At **October 7 01:01:56 UTC**, `db18af34bbac577289a83896c142eebaa4ac108d` had **four all-event workflows**: main CI and Mobile Decompilation running, LLVM Style successful, Mobile Real Applications skipped. **19 checks:** four successful, two failed, three running, ten skipped. Workflow and check collections used 100/page and explicitly empty second pages. Zero legacy commit statuses were returned; their empty aggregate “pending” is not an extra running check.

- [Main Linux](https://github.com/NeverSight/NeverD/actions/runs/37548781094/job/112559177441) failed its default-target build at 00:53:16 UTC on the four Darwin initializer-list type errors above. Tests did not run in this leg.
- [Mobile macOS](https://github.com/NeverSight/NeverD/actions/runs/37548781055/job/112559251785) failed at 00:15:02 UTC: Objective-C scalar, calls/Blocks and Swift source recovery rejected conditional or mutating preprocessor directives. Subsequent single-session parity had missing output evidence. No runtime root cause or speculative fix is inferred.
- Ubuntu mobile and both Windows caller-context jobs passed. Main macOS/Windows and mobile Windows were still running. A partial mobile pass does not establish integrated/native aggregate success.
- [Real applications](https://github.com/NeverSight/NeverD/actions/runs/37548847020): all nine jobs, including release qualification, skipped. No actual current-head application-recovery/reconstruction/behavior qualification was obtained.

### Open proposals and blockers

**[#589](https://github.com/NeverSight/NeverD/pull/589)**, head `dbb93b662f41b71b34ac76ac81833a977d04e66c`: open, non-draft, mergeable at the metadata sample. Four workflows: main/mobile running, style cancelled, Python SDK queued. Fifteen checks: eight success, one failure, one cancelled, one skipped, three running, one queued.

- [Linux default-target build](https://github.com/NeverSight/NeverD/actions/runs/37550844856/job/112565885493) passed, but the job later failed at 01:00:50 UTC. Existing focused mobile-native artifact `11454033213` selected/reported 522 tests: 521 passed, one failed. `MachOInteriorCodePointerCFG.IndexedAbsoluteRootsKeepOwnershipAndRelayRejections` expected INDIR_CALL at `MachOPointerRelocationBoundaryTests.cpp:7854` and did not find it in six indexed cases.
- Existing Linux CTest artifact `11453418469` selected 69,734 registrations, reported four (two pass, two fail for the same Mach-O test under two registrations), and left 69,730 without results. Missing registrations are not passes or independent defect counts.
- [Mobile macOS](https://github.com/NeverSight/NeverD/actions/runs/37550844770/job/112565803383) passed at 00:37:08 UTC. Its log verifies scalar 22 methods/141 matching results, calls 21/134, and Swift 25 native bodies plus nine projections/858 checks for each variant, plus all 12 single-session cases. These are this PR's observed results, not transferable to dev.
- Ubuntu mobile, Windows caller-context and Python 3.10–3.13 passed; Python 3.14 remained queued. Main macOS/Windows and mobile Windows were incomplete.
- The description's local 1,413 Darwin registrations (897 pass, 516 unavailable-backend skips), 210 C/CLI and 33 native workloads are author-reported. It explicitly leaves physical iOS, Intel HVF and complete remote merge CI separate. Overlapping counts are not added.

**[#605](https://github.com/NeverSight/NeverD/pull/605)**, head `afd6776df99001537a237f257c5bf3390e3a17bf`: draft, mergeable false at the metadata sample. All three workflows terminal: style success, main CI and Mobile Decompilation failed. Ten checks: five success, four failure, one skipped.

- [Linux](https://github.com/NeverSight/NeverD/actions/runs/37533655568/job/112509025738) has the same four Darwin compile errors.
- [Windows](https://github.com/NeverSight/NeverD/actions/runs/37533655568/job/112509025826) failed compiling `JumpTableEnhancedTests.cpp` with C1128: object section count exceeds the limit; the compiler suggests /bigobj. This older PR head's build configuration was not patched speculatively.
- [Mobile macOS](https://github.com/NeverSight/NeverD/actions/runs/37533655538/job/112509026241) has the same unsupported-preprocessor recovery failures. [Main macOS](https://github.com/NeverSight/NeverD/actions/runs/37533655568/job/112509025925) artifact `11450905902` reports 688 of 71,915 selected tests: 685 pass, three fail (NeverDMobileIOSBackend, NeverDMobileIOSCallsBackend, NeverDMobileSwiftBackend), 71,227 missing.
- The description's 2,016 local passes, six optional Z3 skips, 60 native outcomes, 16 proof-statistics pairs and 45 ASan cases remain author-reported. Its “no CI wait required” statement is not evidence of remote acceptance.

Both open PRs have zero conversation comments, submitted reviews and inline comments in the sampled REST collections. No independent GitHub approval is inferred. Their complete code changes are outside today's source review except the four #589 compiler corrections.

### Previous blockers reconciled

- Previous source `6f885ebb` now has main/mobile workflows cancelled, style success and applications skipped. It is not a clean passing baseline.
- At historical `253e5519`, [Markor official release](https://github.com/NeverSight/NeverD/actions/runs/37249288624/job/111577673147) failed October 5 01:16:38 UTC. Existing artifact `11320698349` records 10,713 classes, 67,111 methods and 63,750 bodies in the independent inventory; NeverD rejected RestrictTo on RemoteActionCompatParcelizer and published no output. Recompilation and original/reconstructed ART behavior remained incomplete.
- That historical run's [release qualification](https://github.com/NeverSight/NeverD/actions/runs/37249288624/job/111582901351) had zero qualified cases, seven received and 15 missing, plus incomplete independent holdouts/toolchain variation. These are historical failures, not claims about the new source's runtime behavior.
- No fresh result established resolution of the prior Android finalizer timeout, complete Intel HVF acceptance or complete current-head application qualification. Preserve those gates as open instead of carrying forward a green assumption.

## Suggested next priorities

### 1. Close current-candidate compile and qualification gaps

**Dependency:** One exact candidate incorporating reviewed repairs; completed intended platform/application evidence. #589 has the narrow Linux compile repair but also a demonstrated Mach-O regression failure; #605 additionally has failing remote checks and mergeability false.

**Next action:** Reconcile the pending Darwin type repair, Mach-O INDIR_CALL expectation and mobile preprocessor rejection with their actual owners. Then inspect terminal existing results and missing registrations without weakening strict failure or shortening coverage.

**Acceptance:** Identified source compiles on intended platforms; required test registrations have terminal outcomes, all failures are resolved or explicitly blocking, and actual application recovery/reconstruction/behavior completes. Local receipts, skips and cancelled jobs remain separately labelled. This review initiates no run or merge.

### 2. Review and validate the HighC captured-load repair

**Dependency:** Review [a6468b80](https://github.com/NeverSight/NeverD/commit/a6468b80f37d24146fd5a40a08eaf7be987ad58f) and its new regression in the existing target; unrelated dev build failures currently obstruct broader qualification.

**Next action:** Review statement-write classification and the preserved overlap policy. When execution is separately authorized, run the focused HighValueForward cases and relevant HighC memory/control-flow coverage on the actual final revision.

**Acceptance:** The captured value survives Assign-to-Load overwrites at O0/O2; existing disjoint-slot forwarding and scalar/loop controls still hold. Compilation/runtime/formatting remain unverified until real results exist.

### 3. Establish correctness-qualified end-to-end performance evidence

**Dependency:** [#580](https://github.com/NeverSight/NeverD/issues/580)'s measurement contract, a pinned corpus and one coherent Release candidate.

**Next action:** Prepare the load-to-final-output and persistent-session baseline design with completion denominators, process-tree memory, repeated-query growth, cache states and tail latency. Treat solver micro-optimizations as candidates until measured in that workflow.

**Acceptance:** Reproducible raw measurements and source/hardware/corpus identities demonstrate real latency, memory and scaling without reduced semantic coverage or omitted failures. Unavailable metrics are explicit; this review performs no benchmarks.

## Daily log

### 2026-10-07 — HighC write barriers, finite proofs and qualification failures

- Enumerated 198 commits and 557 changed paths; after the two pure report commits/path, 196 commits and 556 paths remain. Recorded 40 merges (39 excluding report-only #568), two open PRs and 18 ordinary issues; #580 is the only ordinary issue activity.
- Found and committed one statically demonstrated HighC captured-load miscompile repair plus a focused unexecuted regression. Reviewed finite projection validation, pristine encoding and prepared-key/frame ownership without finding another proven defect.
- Confirmed four Darwin compiler failures already repaired in unmerged #589, avoiding a duplicate patch. Distinguished that PR's passing Linux build/macOS mobile checks from its later Mach-O test failure and unfinished aggregate.
- Recorded pinned-dev failed/running/skipped CI, #605's failed/conflicting state and historical application/finalizer/native-acceptance limits.
- Preserved the complete prior English tracker verbatim. Only the bug-fix source/test and PROGRESS.md are changed; no other documentation, dependencies, security settings, issues, merge or deployment changed.
- No project execution, build, test, benchmark, script, formatter/linter, manual workflow dispatch or rerun.

## Collection, publication and verification limits

- Open issues: 20 records (18 ordinary + two PRs), empty page 2. Open PRs: two, empty page 2. Activity since October 6 01:06:21 UTC: 43 records (42 PRs + #580), empty page 2. The update-sorted 100-PR page crosses the boundary and independently accounts for all 42 PR records: 40 merged, two open, zero closed without merge.
- Root AGENTS.md, CONTRIBUTING.md, relevant architecture/testing/roadmap and repository review guidance were read. Complete recursive trees contain no nested AGENTS.md.
- Existing workflow artifacts/logs were read only. Check-annotation endpoints were unavailable through the connector; #605 main macOS full log retrieval failed twice, so its existing CTest artifact supplied the failures. No full historical CI census was attempted.
- The prior history blob is `a3694489fd9b85b6f6f2bb36e596b2681541aa9d`. Latest dev and affected file blobs were re-read before writes. Code commit `a6468b80f37d24146fd5a40a08eaf7be987ad58f` has exactly two changed files (26 added lines, three removed); its remote diff was inspected. The report is a separate documentation commit on the same focused branch, proposed as a draft PR.
- The report/repair branch and its automatic CI are different from the reviewed dev and the two existing PR heads. Publishing a patch is not runtime validation or permission to merge.
- The remainder of the 557-path inventory, all solver/watch-list internals, broad emulation/unpack/native/SDK/GUI surfaces, native acceptance and other forwarding passes are not exhaustively reviewed. Static review establishes neither race freedom, performance, complete semantic equivalence nor release readiness. This is one daily sample, not continuous monitoring.

## Previous snapshots (preserved)

<details>
<summary>Complete October 6 tracker and earlier history, preserved verbatim</summary>

# NeverD Daily Progress

Last verified: **2026-10-06 09:03 Asia/Shanghai (UTC+08:00)** / **2026-10-06 01:03 UTC**

This is a point-in-time source and existing-evidence review. The [roadmap](docs/roadmap.md), [architecture](docs/architecture.md), [testing guide](docs/testing.md) and [contribution guidance](CONTRIBUTING.md) remain authoritative. No completion percentage or release-readiness claim is implied.

## Current snapshot

Source review is pinned to [6f885ebb](https://github.com/NeverSight/NeverD/commit/6f885ebb7fda032077838950be03e31b90ffa593), observed at October 6 00:59 UTC. The prior reviewed source was [253e5519](https://github.com/NeverSight/NeverD/commit/253e551961b18a784971244882e36f60eff64b96). Activity counts run from the previous publication sample, October 5 01:20 UTC, to today's collection; they precede this report-only proposal.

| Measure | Verified state |
| --- | --- |
| Open ordinary issues | 17, unchanged; 14 epics, no milestones, only #12 assigned |
| Open PRs | 2: #525 and #565, up from zero before yesterday's proposal |
| PRs merged in the activity window | 78: #487–494, #496–524, #526–564 and #566–567 |
| Closed PRs without merge | 0 |
| Ordinary issue activity | #495 was opened and closed; no other ordinary issue updated |
| Raw source inventory | 338 distinct commits; 744 changed file/submodule paths |
| Path classification | 513 modified, 229 added, 2 removed |
| Report-only exclusion | 337 commits and 743 paths after excluding the one PROGRESS-only commit/path; these are not all implementation commits |
| New statically proven code defects / fixes | 0 / 0 in the bounded scope |
| Existing documentation discrepancy | The Android guide still describes all NULL handle lookups as ESRCH; current getTID returns -1 |

The [source comparison](https://github.com/NeverSight/NeverD/compare/253e551961b18a784971244882e36f60eff64b96...6f885ebb7fda032077838950be03e31b90ffa593) was enumerated as 100 + 100 + 100 + 38 commits, then an empty fifth page. GitHub's comparison file list stops at 300, so the path count comes from complete recursive trees of 5,871 and 6,099 entries, neither truncated, excluding directories and comparing path/blob identities. Rename pairs count as old/new paths. Three changed submodules (`signatures`, Capstone and Unicorn) are inventoried, not audited internally.

[7e68c9f8](https://github.com/NeverSight/NeverD/commit/7e68c9f8aea965b41b303ac180ce8d0f1ffb62a6) changes only PROGRESS.md and is excluded from implementation advancement. Its parent proposal [#487](https://github.com/NeverSight/NeverD/pull/487) also contained three real corrections, so its mixed-purpose merge is retained rather than falsely counted as a report-only merge. The inventory is not an exhaustive review count.

### Changes since the previous snapshot

- Yesterday's [#487](https://github.com/NeverSight/NeverD/pull/487) merged at October 5 03:26:15 UTC as [6f798326](https://github.com/NeverSight/NeverD/commit/6f7983261cfc546d26bdc06100210cc29c55cfa1). Its null TID and typed allocation-failure corrections survive the Bionic/thread dispatch refactor. The current dev PROGRESS blob exactly matches the published October 5 content; all 113,981 characters of that history are preserved below.
- Linux/Android gained shared bounded memory files, status/existence/pathname queries, explicit signal actions and opt-in relative sleep/virtual clocks ([#497](https://github.com/NeverSight/NeverD/pull/497), [#507](https://github.com/NeverSight/NeverD/pull/507), [#545](https://github.com/NeverSight/NeverD/pull/545), [#563](https://github.com/NeverSight/NeverD/pull/563), [#566](https://github.com/NeverSight/NeverD/pull/566)). File/status and sleep ownership were reviewed below; signal delivery and the complete Android surface were not.
- Native/checked instruction coverage added x64 flags, shifts, frame operations and wide compare-exchange, plus ARM64 exclusives and LSE atomics ([#510](https://github.com/NeverSight/NeverD/pull/510), [#534](https://github.com/NeverSight/NeverD/pull/534), [#538](https://github.com/NeverSight/NeverD/pull/538), [#543](https://github.com/NeverSight/NeverD/pull/543), [#552](https://github.com/NeverSight/NeverD/pull/552)). These changes are inventoried, not independently certified here.
- Loop inference gained projected counters/bounds, guarded repeated-PC contexts and completed-query reuse ([#555](https://github.com/NeverSight/NeverD/pull/555)–[#561](https://github.com/NeverSight/NeverD/pull/561), [#564](https://github.com/NeverSight/NeverD/pull/564), [#567](https://github.com/NeverSight/NeverD/pull/567)). Today's focused review covers the final entailment-cache boundary.
- Library recognition/presentation [#537](https://github.com/NeverSight/NeverD/pull/537) and its validation follow-up [#547](https://github.com/NeverSight/NeverD/pull/547) merged; [issue #495](https://github.com/NeverSight/NeverD/issues/495) closed at October 5 15:54:19 UTC. This is delivered recognition/presentation work, not proof of all library behavior or completion of the older epics.
- ARM64 HVF research retained additional correctness regressions but rejected the proposed watchdog/completion performance changes. The [published guide](docs/macos-hvf.md#request-completion-research-2026-10-05) reports final source `a63d57e6a`: CPU 1,434 passed / 12,693 skipped with 29/29 required; separate Darwin 65 passed / 221 skipped with 39/39 required. These are published local evidence, not artifacts independently re-audited today; overlapping inventories must not be added or transferred to this head. Intel complete acceptance remains open.

## Bounded static review

**Mode:** Source, interfaces, callers, declared regressions, configuration and already-existing CI only. No project execution, build, test, benchmark, linter, formatter, repository script or dynamic analyzer was run. No workflow was dispatched or rerun. This publication contains no CI-skip marker; ordinary automatic checks may run.

### Linux file/status and Android sleep boundaries

Full source reads:
- `lib/emulation/os/linux/kernel/{LinuxFiles.h,LinuxFiles.cpp,LinuxFilePaths.cpp,LinuxFileOptions.cpp,LinuxFileIO.cpp,LinuxFileStatus.cpp,LinuxUserMemory.cpp,LinuxTime.h,LinuxTime.cpp,LinuxClock.cpp,LinuxSleep.cpp,LinuxServices.cpp}`
- `lib/emulation/os/linux/android/{AndroidThreads.cpp,AndroidThreadCalls.cpp,AndroidThreadWaits.cpp,AndroidKernelCalls.cpp}`
- `unittests/emulation/{AndroidFileStatusAtTests.cpp,AndroidSleepTests.cpp,LinuxClockTests.cpp}`
- Focused current `AndroidFinalizerTests.cpp:222–243`, architecture/testing contracts and the Android guide's NULL-handle wording

Static conclusions:
- One `LinuxFiles` instance owns descriptors and immutable catalogue observations. Path import stops at the first NUL and bounds addresses before reads. Catalogue validation rejects file/ancestor conflicts; status imports its pathname before output, so overlapping pathname/output is handled deliberately. Absolute paths ignore dirfd, empty-path descriptor queries use the same serializer, and unmodeled relative paths, CWD/directory metadata and version-specific flags fail explicitly.
- `LinuxUserMemory` distinguishes wholly inaccessible output from mixed access without guessing native partial-copy bytes. File reads retain completed page prefixes and cursor updates; original address/signed extent checks precede EOF/clamping. Metadata is explicit rather than fabricated from contents.
- `LinuxSleep` copies both request fields before value checks, preserves the successful remaining-time pointer and retains the consumed deadline. `LinuxClock::advanceTo` checks every supplied epoch for overflow before committing elapsed time.
- `GuestThreads::schedule` advances only with no runnable thread, choosing the earliest sleep deadline. Typed waits preserve request/event attribution; completion updates the original native-call or raw-service event. Bionic alone converts errno. The reviewed source does not introduce host sleeps or host filesystem access.
- The existing `getTID(NULL)` returns `UINT32_MAX`; child mapping converts only `GuestMemoryLimitError` to EAGAIN before identity/output publication. No duplicate fix is needed.

Declared regressions cover direct/variadic/raw ABI routes, output/path aliases, missing metadata, low-word arguments, dynamic-provider lifetime, shared cursor state, consumed sleep inputs, deadline order, TLS/errno/context retention, joins/mutexes/once, budget stops and overflow. Native comparisons explicitly skip unavailable transports. They were read, not executed.

**Result:** No new statically proven defect in this scope; no speculative code patch. The [Android guide](https://github.com/NeverSight/NeverD/blob/6f885ebb7fda032077838950be03e31b90ffa593/docs/android-native-emulation.md#L528) still says NULL lookup returns ESRCH, which conflicts with the narrow TID exception. This known documentation discrepancy remains unchanged because only this tracker is authorized for documentation maintenance. No stable return contract is inferred for other malformed native handles.

### Completed loop entailments

Reviewed `lib/analysis/core/LowIRUndefinedIndependence.cpp` (`Checker::chargeQuery/query`, session binding, `LoopPlanInference::entails/run`, template/native-selector consumers and independent `runRefinement`), the budget contract in `include/neverd/analysis/LowIRRefinement.h`, stable interned references in `include/neverd/symbolic/SymExpr.h`, and the changed regression assertions/CMake ownership from [#567](https://github.com/NeverSight/NeverD/pull/567).

- The [cache key](https://github.com/NeverSight/NeverD/blob/6f885ebb7fda032077838950be03e31b90ffa593/lib/analysis/core/LowIRUndefinedIndependence.cpp#L3026-L3040) is the complete domain AND NOT fact, scoped to one inference's append-only symbolic context; a fact cannot be reused under a different domain by itself.
- The hit path checks node limits and [charges the shared logical-query budget](https://github.com/NeverSight/NeverD/blob/6f885ebb7fda032077838950be03e31b90ffa593/lib/analysis/core/LowIRUndefinedIndependence.cpp#L544-L568) before recording a hit. Misses use a fresh checker. Unknown/invalid answers are rejected before insertion.
- [Final refinement](https://github.com/NeverSight/NeverD/blob/6f885ebb7fda032077838950be03e31b90ffa593/lib/analysis/core/LowIRUndefinedIndependence.cpp#L2827-L2835) starts a separate session: inference results/budgets do not fund the proof.
- `CompletedEntailmentsStayInInferenceSession`, `CompletedEntailmentsKeepUnknownAndNodeLimits`, `MutablePrefixBoundsKeepIndependentBudgets` and `NativeSelectorsProveRepeatedContexts` declare session, nontermination, exact/one-short, node/query exhaustion and context-domain checks in `NeverDLowIRRefinementTests`. Author-reported passes are not independently executed evidence.

**Result:** No statically provable new cache defect; no fix proposed. This is a bounded review of cache ownership/budgets, not a proof of the complete solver, all loop inference or native refinement.

## Existing CI and review evidence

### Exact reviewed head

At **October 6 01:03:15 UTC**, `6f885ebb7fda032077838950be03e31b90ffa593` had **four all-event workflows** (one successful, one skipped, one running, one queued) and **19 checks: four successful, ten skipped, four running, one queued, zero failed**. Both collections used 100/page and an explicitly empty second page.

- [Main CI 37396172858](https://github.com/NeverSight/NeverD/actions/runs/37396172858): all three main platform legs were still at the Debug/Release-target preflight; their configure/build/native test stages had not run. Both Windows caller-context checks passed; the optional Native CPU job skipped.
- [Mobile Decompilation 37396172710](https://github.com/NeverSight/NeverD/actions/runs/37396172710): overall queued; Linux passed, macOS running, Windows queued. The [Linux job](https://github.com/NeverSight/NeverD/actions/runs/37396172710/job/112052630164) independently reports 366/366 CTest tests and separate 197-test/240-test Python suites passing. This is narrower mobile/prebuilt coverage, not the integrated-LLVM/native aggregate.
- [LLVM Style 37396172784](https://github.com/NeverSight/NeverD/actions/runs/37396172784): passed.
- [Mobile Real Applications 37396204608](https://github.com/NeverSight/NeverD/actions/runs/37396204608): all nine jobs skipped, including qualification; its graph skips cancelled upstream producers. It supplies no current-head application-recovery result.

Pending, skipped and partial results cannot establish a complete green gate.

### Prior failures reconciled

- **Android finalizers:** the previous O0 registry-capacity timeout is not verified resolved on this head. The unchanged current regression still makes two separate emulation calls, each with a 30-second allowance. No runtime cause is inferred and no timeout changed.
- **External compiler IR:** merged [#547](https://github.com/NeverSight/NeverD/pull/547) reports compatible host-compiler serialization and successful focused/replay checks. Its own account separates an earlier failing aggregate from later repair runs. This is delivered remediation and author-reported validation, not an independently observed final-revision green aggregate.
- **Real applications:** the [October 5 official Markor job](https://github.com/NeverSight/NeverD/actions/runs/37249288624/job/111577673147) retains failed complete-APK recovery with reconstruction/behavior incomplete. Yesterday's artifact-level RestrictTo diagnosis is preserved below; today's stdout did not independently expose that detail. No new pinned-head real-application pass was found.
- The former [main CI 37247600637](https://github.com/NeverSight/NeverD/actions/runs/37247600637) and [application run 37249288624](https://github.com/NeverSight/NeverD/actions/runs/37249288624), both at `253e5519`, now conclude cancelled overall. Their previously observed individual failures remain failures; cancellation is not a retroactive pass.
- A later [CI 37354456744](https://github.com/NeverSight/NeverD/actions/runs/37354456744), at `2b937f3192dce3d76bdabb7c18a2b56d4232f533`, failed all three main legs before build on 149 benchmark-provenance findings. Merged repairs `ac78f59` and `f053fbb` are present in today's ancestry. Independently, newer ancestor `ac8ae03` passed Linux preflight/configure in [job 112045285352](https://github.com/NeverSight/NeverD/actions/runs/37393906966/job/112045285352), before build cancellation. This supports the historical preflight repair, not complete current-head qualification.

No complete historical workflow census was attempted today; these are targeted prior-blocker checks and an exhaustive exact-head census. The old Linux job-log fetch was unavailable, so the previous preserved artifact diagnosis was not independently repeated.

### Open work awaiting review

- [#565](https://github.com/NeverSight/NeverD/pull/565), head `bfb56d8f932b40df4935fed2bf0d17ff1f67ac15`: parallel checked CPUs and transactional MMIO atomics. At 01:02–01:03 UTC, 14 checks: seven successful, two skipped, five running; an already-existing [native WHP run](https://github.com/NeverSight/NeverD/actions/runs/37394631264) was running. The author reports KVM validation; full current-head completion and ARM64 native coverage remain distinct. This large open PR is outside today's source review.
- [#525](https://github.com/NeverSight/NeverD/pull/525), head `d05bd3fb772c983cfe20f42e8e005aa15aad2841`: explicit Darwin file/directory/mapping/clock services. No exact-head workflow/check runs were returned. Its author records a full local Darwin gate with one HVF virtual-metadata timeout and a later unchanged-bound focused recheck; these must not be flattened into one all-green full gate. Its broad mutable filesystem/OS contract is outside today's source review.
- Both PRs have no submitted reviews, inline review comments or conversation comments in the sampled 100/page endpoints. No independent approval is inferred. No reviewer was assigned and no PR state changed.

## Suggested next priorities

### 1. Establish one complete current-candidate qualification

**Dependency:** Finish existing automatic CI and application producer/consumer chains for one identified source. Historical cancellation, optional Native CPU skips and separate replay receipts leave gaps.

**Next action:** Inspect terminal results, then isolate any remaining finalizer cost/budget, host-compiler IR or real-application coverage failures without weakening strict rejection. Keep already-delivered provenance/owner repairs separate from unresolved behavior.

**Acceptance:** One exact candidate has complete intended Linux/macOS/Windows execution and actual completed application recovery/reconstruction/behavior evidence, with required tests executed and skips/missing outcomes explicitly accounted for. No test or dispatch is initiated by this review.

### 2. Review the two open integration boundaries

**Dependency:** Stable heads and complete existing evidence for #565 and #525; platform-specific native results cannot substitute for each other.

**Next action:** Review CPU/provider atomicity and ownership in #565, and Darwin namespace/content/mapping/metadata lifetimes in #525; reconcile each pending/failed full gate with focused rechecks before integration.

**Acceptance:** Review conclusions and exact-head integration evidence preserve failure atomicity, bounded policy, complete required native outcomes and explicit unavailable-platform limits. No merge or deployment is authorized by this tracker.

### 3. Keep native acceptance and public contracts evidence-aligned

**Dependency:** Original Intel fresh-Executor recovery, complete CPU inventory and independent Darwin evidence on one coherent candidate remain required; diagnostic controls and ARM64 results cannot satisfy them.

**Next action:** Reconcile the published native evidence without promoting incomplete prefixes or rejected performance candidates. Separately request the narrow Android guide wording correction through an authorized documentation change.

**Acceptance:** Intel's original recovery and complete inventories have terminal outcomes with process retirement/source identity; otherwise the gate remains open. Android documentation states the null-TID exception accurately without guessing other malformed-handle contracts.

## Daily log

### 2026-10-06 — File/time boundaries and inference-cache review

- Enumerated 338 commits and 744 changed paths; separated the single pure report commit/path from substantive advancement. Reconciled 78 merges, two open PRs and issue #495's opening/closure, with 17 ordinary issues still open.
- Confirmed yesterday's merged Android corrections survive the source refactor. Completed the bounded file/status/sleep and loop-entailment cache reviews above; found no additional statically proven defect and made no speculative code change.
- Recorded exact-head pending CI, independently observed Linux mobile success, existing application skips and targeted historical blocker/remediation evidence.
- Preserved all previous tracker text, including manual edits/history; only this English PROGRESS.md is changed.
- No execution, build, test, benchmark, repository script, formatter/linter, CI trigger/rerun, issue mutation, dependency/security change, merge or deployment.

## Collection, publication and verification limits

- Open issue collection: 19 records (17 ordinary issues + two PRs), then empty page 2. Separate open PR collection: two records, then empty page 2.
- Activity collection since October 5 01:20 UTC: 81 records (80 PRs + issue #495), then empty page 2. The 100 PRs ordered by update cross that boundary and include all 80 in-window PR records: 78 merged, two open, none closed without merge. Older complete PR history was not enumerated.
- Review submissions and inline comments were checked for #487, #525, #547, #563, #565, #566 and #567; all first pages were empty. Conversation comments were additionally checked for the two open PRs. Unsampled discussions are outside coverage.
- Root AGENTS.md, CONTRIBUTING.md, relevant architecture/testing/roadmap sections and applicable debugging guidance were read. The complete tree has no nested AGENTS.md. GitHub reports dev unprotected and no rulesets; no settings were changed.
- Publication uses a fresh focused topic branch and draft PR because yesterday's related #487 is merged; the two existing open PRs are unrelated. Latest dev/target file blobs are re-read before mutation, and remote content/commit/diff are checked after publication. The existing history blob is `5ebfc33554c723d17f2b07032d02b358f790b2a7`.
- The report's source/CI snapshot remains the pinned source above. Its subsequent documentation commit is a different head with separate checks; publishing it does not validate the reviewed source or imply permission to merge.
- Remaining 744-path inventory, full solver/loop/scalar proof machinery, library-recognition semantic correctness, all x64/ARM64 atomic instructions, broader OS/SDK/GUI surfaces, native backend internals, open PR source and submodule internals are not exhaustively reviewed.
- Static inspection does not establish compilation, formatting, runtime equivalence, race freedom, latency, full ISA/OS coverage or release readiness. This is one bounded daily sample, not continuous monitoring.

## Previous snapshots (preserved)

<details>
<summary>Complete October 5 tracker and earlier history, preserved verbatim</summary>

# NeverD Daily Progress

Last verified: **2026-10-05 09:20 Asia/Shanghai (UTC+08:00)** / **2026-10-05 01:20 UTC**

This point-in-time tracker separates delivered implementation, bounded static review and observed execution evidence. The [roadmap](docs/roadmap.md), [architecture](docs/architecture.md), [testing guide](docs/testing.md) and [contribution guidance](CONTRIBUTING.md) remain authoritative. Suggested priorities are acceptance work, not deadlines or a completion percentage.

## Current snapshot

Source review is pinned to [253e5519](https://github.com/NeverSight/NeverD/commit/253e551961b18a784971244882e36f60eff64b96), observed at October 5 00:59 UTC. Issue/PR counts below precede this proposal.

| Measure | Verified state |
| --- | --- |
| Open ordinary issues | 17, unchanged; 14 epics, no milestones, only #12 assigned |
| Open PRs | 0, down from the three open after yesterday's publication |
| PRs merged since October 4 01:05 UTC | 92: #394, #395 and #397–486 |
| Progress-only PR in that window | #397; excluded from implementation advancement |
| PR closures without merge / ordinary issue updates or closures | 0 / 0 |
| Previous source snapshot | [00f44615](https://github.com/NeverSight/NeverD/commit/00f446154f33ad569b3262c66233767bb368e7a7) |
| Raw change inventory | 400 commits; 613 file/submodule paths: 344 modified, 236 added, 33 removed |
| Excluding yesterday's progress-only commit and merge | 398 commits and 612 paths; these still include other documentation, tests and merges |
| New statically proven defects | 2 Android return/error mismatches and 1 CI expected-owner mismatch, corrected in this proposal |
| Other newly proven defects in the bounded audit | 0 |

The [comparison](https://github.com/NeverSight/NeverD/compare/00f446154f33ad569b3262c66233767bb368e7a7...253e551961b18a784971244882e36f60eff64b96) was enumerated as four pages of 100 commits and an empty fifth page, with 400 distinct SHAs. GitHub caps comparison file lists at 300, so the 613-path inventory compares complete recursive trees of 5,661 and 5,871 entries, neither truncated. Directory entries are excluded; rename pairs count as old/new paths. The two changed submodule pointers are inventoried, not audited internally. Neither 613 paths nor 398 commits is a complete source-review count.

The source interval also includes #396, merged at October 4 01:01:05 UTC and already recorded in yesterday's publication note; it precedes today's PR activity window. Counts based on commit ancestry and counts based on merge time are intentionally distinct.

### Changes since yesterday

- [#397](https://github.com/NeverSight/NeverD/pull/397) merged at October 4 03:32:46 UTC as [2598893f](https://github.com/NeverSight/NeverD/commit/2598893f7e94534f87b97e4c437d34ebd407080c). The current dev tracker exactly matches its published blob; its complete text and all earlier human edits/history are preserved below.
- Yesterday's pending aggregate-value/ABI [#394](https://github.com/NeverSight/NeverD/pull/394) and Android default-symbol-scope [#395](https://github.com/NeverSight/NeverD/pull/395) are now merged. They are no longer open-PR blockers.
- Android gained guest once callbacks, mutexes, explicit clocks and file-backed inputs, bounded formatting, finalizers, thread attributes, cooperative threads and once waiters. The scheduler/callback boundary was reviewed; the whole Android implementation was not.
- Checked x64 gained packed integer/shift/shuffle/interleave/conversion operations, DAZ controls, native SIMD exception continuations and PAUSE. The latest capability, fault-to-Windows and checked execution boundaries were reviewed below; all newly admitted opcode semantics were not independently proved.
- Shared scalar equivalence/loop recovery, initialized private-frame projection, byte-cell scalarization, architecture separation and Swift/Objective-C recovery advanced. These were inventoried, not comprehensively audited.
- Exact-head Mobile Decompilation and LLVM Style pass. Main CI now has a failed Linux job, and real-application qualification has a failed Markor case; other jobs remain unfinished.
- New Intel diagnostic evidence includes independently inspected 1,000-repetition successes under specific retained-session/vCPU controls. Complete Intel CPU acceptance remains explicitly unverified.

## Bounded static review

**Mode:** Source, diffs, interfaces, callers, test declarations, configuration and already-existing CI evidence only. No project, build, test, benchmark, linter, formatter, repository script or dynamic analyzer was run. No workflow was manually dispatched or rerun. New commits contain no CI-skip marker; ordinary automatic checks may run.

### Confirmed correction: null pthread_gettid_np result

[Fix b43f50d6](https://github.com/NeverSight/NeverD/commit/b43f50d67a07d747739745410422a0285eeb0311) corrects one production expression in `GuestThreads::invoke`. The shared null-handle branch returned positive ESRCH (3) before the TID-specific path, so a caller testing for -1 could interpret a failure as a positive thread ID.

The pinned [Bionic implementation](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_gettid_np.cpp#L32-L35) returns -1 for a null lookup. Its [handle lookup](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_internal.cpp#L116-L129) explicitly permits NULL at the selected API boundary while retaining fatal handling of invalid non-null handles. This is an independently implemented API correction, not copied Bionic code.

The fix returns `UINT32_MAX` only for the null TID query, matching the project's existing zero-extended narrow-negative representation and presenting signed -1 to the C `pid_t` consumer. It preserves join/detach errors and the errno path. Direct and dynamically resolved imports converge through `AndroidBionic.cpp` into this owner; result publication writes the same result register without changing errno.

Added regression source in `AndroidThreadTests.cpp` and `fixtures/android_threads.c` checks both routes, signed -1, low-32-bit `0xffffffff`, errno sentinel preservation and provider attribution. Existing O0/O2 and three relocation-packing fixture registrations include the new entry without a build-file change.

**Verification:** Remote commit/file readback matches the intended three-file patch. No tests, fixture compilation or formatter were run. The standalone guide's broad NULL-lookup wording is known to need a narrow correction, but that file is outside this proposal's authorized scope and was not changed.

### Confirmed correction: fragmented thread-memory refusal

[Fix 7877e950](https://github.com/NeverSight/NeverD/commit/7877e950fc840638fe92ff64ddfc7a073e43d32a) translates only `GuestMemoryLimitError` from initial child-region mapping into EAGAIN. Previously, the thread model checked total unmapped capacity but propagated a contiguous-allocation shortage as a terminal runtime failure.

The static chain is `GuestThreads::create` → `AddressSpace::map` → `PhysicalMemory::allocate`. Enough total free bytes do not guarantee one free span large enough for stack, guard and TLS. The allocator already reports this condition using the specific capacity-error type, and Linux memory services already distinguish that type from generic mapping failures. The former Android propagation reached the runner's `RuntimeFailure` path instead of its documented allocation refusal.

The correction leaves every non-capacity error unchanged and precedes guard changes, child TLS/context creation, output-handle publication and identity insertion. A new model-level regression in `AndroidThreadTests.cpp` constructs deterministic 4 KiB-owner fragmentation, explicitly checks the failed contiguous-allocation precondition, and checks EAGAIN with unchanged handle, errno, mapped/allocated bytes, mapping generation and entry-only identity. A separate occupied-slot control requires the generic mapping error to keep propagating. It avoids guest timing, fixture-size and scheduling assumptions.

**Verification:** The two-file remote diff and full file readback match the intended patch. Header declarations, transitive component linkage and the existing test target were inspected. Neither compilation nor the new regression was executed.

### Confirmed correction: mobile-native expected-owner drift

[Fix 717c7928](https://github.com/NeverSight/NeverD/commit/717c79281a171854112323a7dca9d4d95965d6ec) changes exactly eleven expected-identity lines in `.github/workflows/ci.yml`, adding `NeverDJumpTableTests` alongside the retained `NeverDLiftTests` owner.

The same three JumpTable sources are registered in both targets in [CMake](https://github.com/NeverSight/NeverD/blob/253e551961b18a784971244882e36f60eff64b96/unittests/lift/CMakeLists.txt#L995); [the shared registration helper](https://github.com/NeverSight/NeverD/blob/253e551961b18a784971244882e36f60eff64b96/cmake/AddNeverD.cmake#L194) supplies target labels. Preserved Linux and macOS discovery/execution data agree on every one of these eleven names and owner sets. The tests themselves passed **522/522** and **577/577**, respectively; their mobile-native audits failed solely because the expected owner tuples omitted the newer target.

Test names, required counts, full-label equality, assertions, timeouts, workflow triggers, permissions and concurrency policy are unchanged. This repairs expected metadata without weakening the audit. Remote readback and the one-file 11-addition/11-deletion diff were verified; no workflow was dispatched or rerun.

### Android cooperative threads, once, mutex and finalizer boundaries

Full production-source reads:
- `lib/emulation/os/linux/android/{AndroidThreads.cpp,AndroidThreads.h,AndroidThreads.def,AndroidThreadAttributes.cpp,AndroidThreadAttributes.def,AndroidMutex.cpp,AndroidMutex.def,AndroidFinalizers.cpp,AndroidOnce.def,AndroidInternal.h,AndroidNative.cpp,AndroidSymbols.def,AndroidDiagnostics.def}`
- `lib/emulation/runtime/ProcessAndroidJSON.cpp`
- `include/neverd/emulation/{AndroidNative.h,ProcessCall.h}`

Focused callers/interfaces: `AndroidBionic.cpp` TLS/errno and dynamic-provider helpers, once completion (249–292), thread dispatch (294–322) and kernel wrappers (491–520); Linux `LinuxServices.cpp`, `LinuxServiceABI.cpp`, `LinuxMemory.cpp`, `LinuxKernel.h` and `LinuxValues.def`; `ExecutionSession.cpp` continuation/quantum accounting; `IntegerABI.cpp` call preparation; `AddressSpace.cpp:113–193`; the complete `PhysicalMemory.cpp`; public `ExecutionSession.h` and `ProcessSession.h`.

Test/fixture reads: `AndroidThreadTests.cpp` and `fixtures/android_threads.c`; the Android finalizer, mutex and thread-attribute test/fixture pairs; `AndroidNativeTests.cpp:101–269` and `fixtures/android_once.c`; fixture compilation/packing registration in `AndroidFixtures.cmake`; `ProcessPublicTests.cpp:885–923`; Python `test_process_integration.py:19–67`. Relevant changed-file inventories for #400, #405, #431, #435, #439 and #458 were read.

The audit traced saved CPU/TLS/errno ownership, the shared instruction budget, suspended service completion, join retirement, once-owner/waiter wakeup validation, guest destructor callbacks, mutex-owner identity and direct/dynamic import convergence. Two return/error-boundary defects were established above. No additional confirmed once-wait, mutex-owner, finalizer-order or callback-continuation defect emerged.

**Separate uncertainty:** `pthread_getattr_np(NULL)` is a malformed-input boundary whose pinned native implementation directly dereferences the handle. That does not establish a stable API return contract or justify guessing a new modeled result. It is not part of these fixes. Copied scheduling attributes under inherited policy match the inspected Bionic behavior and were not promoted to a defect.

CPU backend context implementations and ELF-linker internals were not deeply re-audited; their Android consumers were traced. No native Android equivalence, SMP correctness, runtime timing, executed regression or full C/Python surface verification is claimed.

### x64 PAUSE, SIMD capability and Windows continuation boundary

Full-file reads:
- `lib/emulation/core/CheckedBackend.cpp`
- `lib/emulation/arch/x86_64/{CheckedX64Backend.cpp,CheckedX64Backend.h,X64Machine.h,X64MachineProbe.cpp,X64MachineProbe.h,X64MachineProbe.def}`
- `lib/emulation/runtime/{BackendRegistry.cpp,ExecutionProfiles.def}`
- `lib/emulation/os/windows/exception/{X64SIMDException.cpp,X64SIMDException.h,X64SIMDException.def}`
- `lib/emulation/os/windows/process/WindowsProcessExceptions.cpp`
- `unittests/emulation/{X64PauseTests.cpp,X64PauseCases.def,X64SIMDExceptionTests.cpp,X64ProbeExecutionTests.cpp,WindowsSIMDMappingTests.cpp,WindowsSIMDExecutionTests.cpp,WindowsSIMDExecutionCases.def}`

Focused reads: `ExecutionConfiguration.cpp:1-218`; `WindowsProcessContext.cpp:176-341`; public `CPU.h` and `ExecutionConfiguration.def` changes; `CheckedX64Instructions.def`, `NativeCPUTests.def` and test-target registration changes; KVM/WHP startup call-site changes. Production patches in [#482](https://github.com/NeverSight/NeverD/pull/482), [#485](https://github.com/NeverSight/NeverD/pull/485) and the code/test changes in [#486](https://github.com/NeverSight/NeverD/pull/486) were read.

Static conclusions:
- PAUSE is admitted through the existing checked single-instruction boundary. The shared loop offers a pre-effect observer stop, checks the same absolute deadline, and only publishes staged CPU/RAM effects after successful retirement. PAUSE is not a guest scheduler or a latency guarantee.
- Precise unmasked SIMD capability is qualified by backend, ISA and execution contract. Only the declared x64 KVM/WHP contracts add it; checked Unicorn and HVF do not acquire it from writable MXCSR bits.
- The KVM/WHP factory requests an authentic SIMD-fault startup probe plus masked and repaired-operand retries before publishing the discovered MXCSR mask. Probe state comparisons cover the full machine packet; a probe still does not certify every instruction or workload.
- A genuine machine exception retains architectural CPU status while speculative RAM is discarded. Windows classification requires an authenticated SIMD fault and consistent retained MXCSR; sticky status by itself does not invent a fault. Saved-context restoration validates both MXCSR copies, capability, reserved fields, executable PC and stack before restoring.
- Read regression sources describe full-state PAUSE stops/resumption, invalid LOCK refusal, real checked/native SIMD fault state, guest VEH/VCH execution, context repair and old sticky-status preservation. These are source assertions, not newly executed results.

No additional statically proven defect was established in this x64 scope. Broad native backend lifetime/state-transfer implementations, all SIMD opcode semantics and hardware behavior remain outside this bounded audit.
## Existing CI and native evidence

### Exact reviewed source

At **October 5 01:20:30 UTC**, `253e551961b18a784971244882e36f60eff64b96` had **five workflows** and **30 checks: ten successful, ten skipped, five running and five failed**. This supersedes the earlier 01:04 sample (24 checks, zero failed).
- [Main CI 37247600637](https://github.com/NeverSight/NeverD/actions/runs/37247600637): still running overall, with [Linux job 111568535902](https://github.com/NeverSight/NeverD/actions/runs/37247600637/job/111568535902) failed. macOS/Windows jobs remain running; macOS already has a failed mobile-native step. Both Windows caller-context checks passed; the optional manually selected Native CPU job skipped.
- [Mobile Decompilation 37247600629](https://github.com/NeverSight/NeverD/actions/runs/37247600629): successful on Linux, macOS and Windows.
- [LLVM Style 37247600645](https://github.com/NeverSight/NeverD/actions/runs/37247600645): successful.
- [Mobile Real Applications 37249288624](https://github.com/NeverSight/NeverD/actions/runs/37249288624): running, with four failed Android checks: Markor official release, Gradle release and Gradle debug, plus calculator official release. Only the official Markor failure was diagnosed below; the three later failure causes were not audited before this snapshot.
- [Earlier same-head real-application consumer 37247613656](https://github.com/NeverSight/NeverD/actions/runs/37247613656): skipped.

Legacy status contexts are empty. Their aggregate `pending` value is not an extra failed check. Running workflows with failed jobs, partial successes and skipped checks do not establish full integration or current native acceptance.

The complete dev Actions **creation** window October 4 01:05 UTC through October 5 01:00 UTC contains **501 distinct runs**, enumerated as 100 + 100 + 100 + 100 + 100 + 1 and matching the API total. At collection: **85 successful, 292 cancelled, 116 skipped, six failed and two running**. Its **118 main-CI runs are 117 cancelled and one running; zero completed main-CI successes**. The mobile-fixture subset is two successful, 115 cancelled and one failed; real-application consumers are 116 skipped, two cancelled and one running. Cancellation is an evidence gap, not proof of a code defect. This window does not include every older run that happened to finish within it.

### Newly observed current-head failures

The failed Linux main-CI job and still-running macOS job have independently inspected preserved evidence. Original ZIP SHA-256 values matched GitHub metadata for Linux mobile-native artifact `11320976133`, macOS mobile-native `11320292520`, Linux emulation `11320706657` and Linux full-profile `11320282451`. The full Linux job-log tool failed twice; these original artifacts supplied the diagnosis.

- **Mobile-native owner drift:** Linux **522/522** and macOS **577/577** selected/reported/started tests passed, but eleven expected label sets were stale. Fix 717c7928 above corrects only those metadata expectations; it is not yet verified by a new run.
- **Android finalizer timeout:** The Linux emulation profile has 16,456 registrations: **7,514 passed, 8,939 skipped and three failed**. All O0 packaging variants of `AndroidFinalizers.FiniteRegistryRejectsBeforeSuccessAndCanBeReused` hit the outer 30-second CTest timeout; O2 variants pass. The [fixture](https://github.com/NeverSight/NeverD/blob/253e551961b18a784971244882e36f60eff64b96/unittests/emulation/AndroidFinalizerTests.cpp#L222) makes two emulation calls, each admitting 30 seconds, inside a 30-second test target. This exposes a possible outer/inner budget conflict but does not establish the runtime-cost root cause. No timeout was increased.
- **External LLVM-IR oracle compatibility:** `LLVMScalarDecisionCompiled.DeepOneAndTwoBackedgeOracles` fails when the external compiler rejects `trunc nuw nsw i64 %shifted to i32` with “expected type.” The [fixture](https://github.com/NeverSight/NeverD/blob/253e551961b18a784971244882e36f60eff64b96/unittests/devirtualization/LLVMScalarDecisionTests.cpp#L31) also uses `zext nneg`, and sends that modern IR unchanged to discovered `NEVERD_TEST_CLANG`. The configured external-tool contract has no matching syntax/version check. In-process proof success does not establish external-oracle compilation. The full profile reports **1,628 of 52,395 selected: 1,563 passed, one failed, 64 skipped, 50,767 missing after early stop**. Missing tests are unexecuted. Semantic flags were not removed to silence the failure.
- **Real application coverage:** [Official Markor job 111577673147](https://github.com/NeverSight/NeverD/actions/runs/37249288624/job/111577673147), exact consumer `253e5519`, exits one because the DEX loader rejects an unsupported AndroidX `RestrictTo` annotation on `RemoteActionCompatParcelizer`. Original artifact `11320698349` was digest-verified. Markor 2.16.1 / source `f33eb6a8dfb210f27bfe7294430d4d39b795bd0f` independently inventories **two DEX files, 10,713 classes, 67,111 methods and 63,750 bodies**; recovery fails, and recompilation/behavior are incomplete. Inventory success is not recovery success, and strict annotation admission was not weakened.

The timeout, external compiler contract and application-support gaps remain unresolved. The three later application failures named in the current check snapshot need their own diagnosis.

### Historical failures already addressed

All six failed runs in that window were diagnosed from their original job logs:
- [Windows mobile 37169700823](https://github.com/NeverSight/NeverD/actions/runs/37169700823/job/111339984308), source `bc05c078`: MSVC C1128 while compiling `NeverDCAPIObjC.cpp`, requiring `/bigobj`. [5a10380b](https://github.com/NeverSight/NeverD/commit/5a10380bc45fa840a9e88a2e2c5328e6e6804c7c) added the MSVC-only source property; it remains in [current CMake lines 50–55](https://github.com/NeverSight/NeverD/blob/253e551961b18a784971244882e36f60eff64b96/lib/sdk/CMakeLists.txt#L50-L55). Current Windows mobile success corroborates recovery for that scope.
- Five LLVM Style failures: [37205055158](https://github.com/NeverSight/NeverD/actions/runs/37205055158) (`HvfExecutor.cpp`), [37212958633](https://github.com/NeverSight/NeverD/actions/runs/37212958633) (`HvfExecutorTests.cpp`), [37222710688](https://github.com/NeverSight/NeverD/actions/runs/37222710688) (`X86Lifter.cpp`), [37228760523](https://github.com/NeverSight/NeverD/actions/runs/37228760523) (`X64PackedFloatCases.def`) and [37230834147](https://github.com/NeverSight/NeverD/actions/runs/37230834147) (`X64PackedFloatIntegerCases.def`). The pinned head's style check passes.

These are historical results, not six current-head failures. No duplicate fix or formatting sweep was made.

### Intel diagnostic progress, with production acceptance still open

The [public pinned HVF guide](https://github.com/NeverSight/NeverD/blob/253e551961b18a784971244882e36f60eff64b96/docs/macos-hvf.md#L213) explicitly keeps the complete Intel CPU inventory unverified. This public tracker references only the repository's published technical summary:

- The guide records **1,000/1,000 retained-session recovery iterations on both Intel host images**, with successful exit and retirement records. This establishes the admission-budget correction for that diagnostic, not the fresh-Executor production gate. [Published summary](https://github.com/NeverSight/NeverD/blob/253e551961b18a784971244882e36f60eff64b96/docs/macos-hvf.md#L435)
- A later vCPU-recreation control records **1,000/1,000** on one image; its counterpart preserves **460 completed / 461 started**, followed by an uploader interruption rather than a completed native result. The public guide does not identify a root cause. [Published summary](https://github.com/NeverSight/NeverD/blob/253e551961b18a784971244882e36f60eff64b96/docs/macos-hvf.md#L443)
- The later control omitting an extra host kick records lost runner communication and **778/779** and **300/301 completed/started** prefixes. Neither has a final native result or retirement record; the last marker does not locate the fault. [Published summary](https://github.com/NeverSight/NeverD/blob/253e551961b18a784971244882e36f60eff64b96/docs/macos-hvf.md#L447)

These published diagnostic outcomes advance the picture beyond yesterday's prefix but do not certify today's head. This update does not reproduce personal-repository evidence links, controller identifiers or raw host/crash details.

Yesterday's independently verified historical **26/26 Intel Darwin** success and **252 complete / 253 started** recovery prefix remain valid for their own NeverD runs, preserved below. Incomplete runs remain incomplete.

**ARM64 HVF, documented local evidence:** The public guide records clean source `389bebfdda31a0db19facc7ab8ca5461a8c8c1bc` with CPU 2,546 passed / 4,710 skipped / zero failed and all 23 required native cases; separate Darwin 130 passed / 156 skipped / zero failed and all 39 required cases. Those local artifacts were not independently accessed today. CPU and Darwin totals overlap and must not be added; neither transfers to today's head or native ARM64 KVM/WHP.

## Today's top priorities

### 1. Review and validate the three focused corrections

**Dependency:** The focused proposal must pass its existing automatic workflow and matching guest regression matrix; source inspection alone is insufficient.

**Next action:** Review the narrow Android return/error ownership changes, regression declarations and eleven corrected JTE owner tuples. Reconcile the known outdated standalone Android documentation before treating its blanket NULL-handle statement as authoritative.

**Acceptance:** Direct and dynamic null TID queries report signed -1 with errno preserved; fragmented-memory thread creation returns EAGAIN without publishing an identity or output handle, preserving the capacity/generic-error boundary. Existing invalid-handle, generic-error and thread-lifetime behaviors remain intact. No execution or merge is initiated by this review.

### 2. Resolve remaining CI blockers and obtain complete qualification

**Dependency:** The finalizer timeout, external-compiler IR compatibility and Markor annotation-coverage blockers remain. One identified source needs complete terminal evidence.

**Next action:** Diagnose the O0 finalizer cost/budget boundary and define a compatible external-oracle compiler contract without deleting semantic proof coverage. Preserve strict annotation rejection while investigating the Markor coverage gap. Inspect already-authorized automatic results with exact source and producer/consumer identities. Map verified Android/x64 delivery to [#104](https://github.com/NeverSight/NeverD/issues/104), and mobile acceptance to [#101](https://github.com/NeverSight/NeverD/issues/101), without closing either by inference.

**Acceptance:** Intended Linux, macOS and Windows profiles complete with audited required execution. Real-application qualification provides actual completed results rather than a skipped consumer. Historical mobile/style recovery and a passing subset do not substitute for full integration.

### 3. Close Intel production acceptance with complete original evidence

**Dependency:** A coherent clean candidate and matching native evidence; diagnostic reuse controls are not the production gate.

**Next action:** Reconcile original fresh-Executor recovery, the complete CPU inventory and independent Darwin results on the same candidate. Keep assertion failure, uploader crash, runner loss and cancellation distinct.

**Acceptance:** Original fresh-Executor recovery completes 1,000 repetitions with final retirement, the complete native CPU inventory executes every required outcome, and the independent Darwin gate passes with exact source/attempt identity. Retained-session or vCPU-only controls, partial prefixes and unrelated-host results cannot satisfy this gate.

## Daily log

### 2026-10-05 — Android error-boundary corrections and native evidence refresh

- Enumerated 400 commits and 613 raw paths; separated yesterday's progress-only commit/merge and reconciled 92 PR merges, 17 unchanged ordinary issues and zero open PRs before this proposal.
- Statically reviewed the bounded Android scheduler/callback and x64 checked/SIMD/Windows-continuation paths above.
- Submitted minimal corrections for two proven Android return/error-boundary defects with regression source, plus the eleven-entry CI expected-owner correction. No regression or project workload was executed.
- Confirmed exact-head mobile/style success, new main-CI/Markor failures and remaining running work; diagnosed six historical failures without duplicating delivered fixes. Retained finalizer timeout and external-compiler compatibility as separate unresolved blockers.
- Reconciled the public HVF guide's new 1,000-repetition diagnostic successes while retaining fresh-Executor/full-inventory acceptance and incomplete-run gaps.
- Preserved the complete October 4 tracker and earlier history. The standalone Android guide's narrow correction remains outside this proposal's authorized file scope; its blanket NULL-lookup statement is known to be outdated.
- No dependency or security-setting changes, issue mutation, manual CI dispatch/rerun, merge or deployment. New commit messages are English and contain no skip marker.

## Collection and publication limits

- Open issues: 17 records and an empty second page; separate open PR list empty. Updated issue/PR collection: 92 PRs and an empty second page, no ordinary issue. The first 100 PRs ordered by update cross the time boundary and contain every in-window record; older complete PR history was not enumerated.
- Review submissions, inline comments and conversation comments were sampled for #394, #395, #397, #439, #458, #482, #485 and #486. Each endpoint returned an empty first page (100/page). No independent approval is inferred, and unsampled PR discussions were not audited.
- Exact-head workflow/check collections included all event types/all checks; the early five/24-record collections had empty second pages. The later publication sample returned five workflows and 30 checks, both below 100/page. The complete dev creation-window inventory and its limits are stated above. Not every historical successful log or artifact was audited.
- Root AGENTS.md, CONTRIBUTING.md, relevant architecture/testing/roadmap sections and applicable repository debugging guidance were read. The complete tree contains no nested AGENTS.md. GitHub reports dev unprotected and an empty ruleset collection; no settings were changed.
- The publication follows a focused topic branch and draft PR against dev. Head/file blobs are re-read before mutation; expected remote contents and diffs are checked after each commit. Remote publication is not passing runtime verification or permission to merge.
- The other changed source paths, all scalar/loop/LLVM proof machinery, Swift/Objective-C recovery, broader loader/ABI changes, native backend lifetime internals and submodule internals remain outside this bounded audit.
- Static analysis does not establish compilation, formatting, runtime behavior, race freedom, complete ISA coverage, overall product completion or release readiness. This is a point-in-time snapshot, not a claim of continuous monitoring.

## Publication observation

At 01:20 UTC, dev still points to the reviewed `253e5519`, and its PROGRESS.md remains blob `f07bf1e27c203670103795f589aaf58c3096d9a2`. The three focused corrections are on `dot/daily-static-review-2026-10-05`: [b43f50d6](https://github.com/NeverSight/NeverD/commit/b43f50d67a07d747739745410422a0285eeb0311), [7877e950](https://github.com/NeverSight/NeverD/commit/7877e950fc840638fe92ff64ddfc7a073e43d32a) and [717c7928](https://github.com/NeverSight/NeverD/commit/717c79281a171854112323a7dca9d4d95965d6ec). They are proposed, not merged. Commit/file/diff readback succeeded. A new-head passing result is not claimed; the final progress commit and draft PR are publication steps, not runtime validation.

## Previous snapshots (preserved)

<details>
<summary>October 4 tracker with complete October 3, October 2, October 1 and September 30 history</summary>

# NeverD Daily Progress

Last verified: **2026-10-04 09:05 Asia/Shanghai (UTC+08:00)** / **2026-10-04 01:05 UTC**

This point-in-time tracker separates delivered implementation, static review and observed execution evidence. The [roadmap](docs/roadmap.md), [architecture](docs/architecture.md), [testing guide](docs/testing.md) and [contribution guidance](CONTRIBUTING.md) remain authoritative. Suggested priorities are acceptance work, not deadlines or a project completion percentage.

## Current snapshot

Source review is pinned to [00f44615](https://github.com/NeverSight/NeverD/commit/00f446154f33ad569b3262c66233767bb368e7a7), observed at 00:58 UTC. Counts below use that observation unless explicitly updated.

| Measure | Verified state |
| --- | --- |
| Open ordinary issues | 17, unchanged |
| Open PRs at initial observation | 3: #394, #395, #396; up from zero before yesterday's proposal |
| PRs merged since October 3 01:12 UTC, through 00:58 UTC | 31: #363–393; #363 is progress-only documentation, not product advancement |
| In-window PR closures without merge | 0 |
| Ordinary issue updates / closures in the window | 0 / 0 |
| Previous source snapshot | [99340b58](https://github.com/NeverSight/NeverD/commit/99340b58657e3907588a33b363435f79b74b86fe) |
| Change inventory | 226 commits; 895 raw file/submodule paths: 395 modified, 327 added, 173 removed |
| New statically proven defects in reviewed scope | 0 |
| Proposed changes from this review | PROGRESS.md only; no duplicate code fix |

The [comparison](https://github.com/NeverSight/NeverD/compare/99340b58657e3907588a33b363435f79b74b86fe...00f446154f33ad569b3262c66233767bb368e7a7) was enumerated across 100 + 100 + 26 commits and an empty fourth page. GitHub caps comparison file lists at 300; the inventory instead compares complete recursive trees of 5,496 and 5,661 entries, neither truncated. Renames count as old/new paths in this raw tree inventory, so 895 is not a count of independent behavioral changes or fully reviewed files.

### Changes since yesterday

- Yesterday's [#363](https://github.com/NeverSight/NeverD/pull/363) merged at October 3 02:57:41 UTC as [988a6f01](https://github.com/NeverSight/NeverD/commit/988a6f01115bc91269595c047ce32e3e259c65cc). Subsequent human/repository editing corrected the historical Windows policy path to its new `driver/` location. That correction and the entire existing tracker are preserved below.
- Windows process capabilities advanced through runtime DLL loading, Unicode environment APIs, heap reallocation, modeled system DLLs, VEH/VCH continuations, shared x64 SEH and caller-context capture: [#368](https://github.com/NeverSight/NeverD/pull/368), [#369](https://github.com/NeverSight/NeverD/pull/369), [#371](https://github.com/NeverSight/NeverD/pull/371), [#375–379](https://github.com/NeverSight/NeverD/pull/379), [#381](https://github.com/NeverSight/NeverD/pull/381), [#386](https://github.com/NeverSight/NeverD/pull/386) and [#387](https://github.com/NeverSight/NeverD/pull/387). Their complete implementation is outside today's bounded audit.
- [#383](https://github.com/NeverSight/NeverD/pull/383) added synchronous external C/Python decoder callbacks to the shared bytecode pipeline; [#384](https://github.com/NeverSight/NeverD/pull/384) reconciled capability declarations.
- [#391](https://github.com/NeverSight/NeverD/pull/391) separated Linux/Darwin kernel contracts from process startup. [#392](https://github.com/NeverSight/NeverD/pull/392) added Linux/Bionic vectored output and corrected scalar zero-write address validation.
- Numeric memory, finite-value and modular predicate simplification, Swift/Objective-C recovery and LLVM C emission also advanced. These changes were inventoried, not comprehensively audited.
- Intel HVF now has independently inspected Darwin success, but complete Intel CPU acceptance remains open. The later 1,000-repetition recovery run failed after runner communication loss; preserved progress does not prove the requested total.

## Bounded static review

**Mode:** Source, diffs, interfaces, callers, test declarations, configuration and already-existing CI evidence only. No project, build, test, benchmark, linter, formatter, repository script or dynamic analyzer was run. No CI workflow was dispatched or rerun.

### External decoder callback and ownership boundary

Full source/interface reads:
- `include/neverd/{analysis/BytecodeDecoder.h,pipeline/BytecodeRecovery.h,sdk/NeverDCAPIBytecode.h}`
- `lib/analysis/bytecode/{BytecodeDecoder.cpp,BytecodeProfile.cpp}`
- `lib/pipeline/BytecodeRecovery.cpp`
- `lib/sdk/capi/NeverDCAPIBytecode.cpp`
- `pluginsdk/python/neverd_plugin/bytecode.py`
- `unittests/devirtualization/BytecodeCAPITests.cpp`
- `pluginsdk/python/tests/{test_bytecode.py,test_bytecode_integration.py}`

Focused reads: Python `abi.py` callback/function declarations; `ffi.py::owned_string`; `BytecodeDecoderTests.cpp` source/call/loop and floating-policy sections (lines 601–996); callback contract in `docs/bytecode-profiles.md`; the external-bytecode capability and expected-public-surface entries. The decoder and pipeline patches in #383 were also read.

Static conclusions:
- Input windows end at the declared function boundary and are capped at 4,096 bytes. Callback recipes pass the same operand-width, storage-range, temporary-definedness, operation and CFG checks as static profiles.
- Reply state is invocation-local. A repeated, missing, null, empty or oversized reply fails; accepted JSON is copied before the callback returns. The sink's return value means copied, not semantically validated.
- Python retains the trampoline through the native call, catches callback exceptions, avoids re-entering the decoder after an exception and re-raises after the native response is freed by `owned_string`.
- The graph and input limits do not preempt trusted callback code. Stable caller context and synchronous lifetime are explicit contracts, not sandbox guarantees. State-C output is not proof of equivalence to an unknown interpreter.
- Existing tests describe independent reply-buffer reuse, error ownership, 64-bit PCs, reentrancy/concurrency and both C routes. They were read, not executed today.

### Linux/Bionic output boundary

Full source reads: `lib/emulation/os/linux/kernel/{LinuxOutput.cpp,LinuxKernel.h,LinuxServices.cpp}`, `lib/emulation/os/linux/LinuxValues.def`, `lib/emulation/os/linux/android/{AndroidBionic.cpp,AndroidKernelServices.def}`, `unittests/emulation/LinuxOutputNativeTests.cpp`, and `unittests/emulation/fixtures/{LinuxOutputCases.def,linux_output.c}`. The #392 LinuxOutput patch was also read.

Focused caller/test reads: `LinuxProcessTests.cpp` scalar output cases and vectored registration/budget sections (lines 252–259, 281–289, 301–339); `AndroidNativeTests.cpp` vectored output/errno and budget sections (128–150), plus scalar-output and request-limit assertions.

Static conclusions:
- Descriptor lookup narrows to 32 bits before checking supported sinks; vector counts are bounded at 1,024. Descriptor metadata and signed lengths are imported before payload publication.
- Address extents are validated before payload access. The one-vector transfer cap and multi-vector original-extent checks are deliberately distinct.
- Whole-call output budgeting covers both streams before publishing that call. A later payload fault retains an earlier readable prefix; metadata errors publish no payload.
- Bionic alone translates negative Linux results to `-1` and errno; raw services retain negative errno. The scalar zero-length path still checks user-address domain, whereas zero vectors ignore the table pointer.
- The original Linux fixture compares regular-file redirects, not pipe atomicity. Native ARM64 backend coverage and complete process semantics cannot be inferred from these source checks.

No new defect in these paths was established strongly enough for an automatic correction. The known Android default-scope/flag work is already proposed in #395; it is not duplicated here.

### Native evidence collectors and acceptance boundaries

Reviewed at the pinned source:
- `scripts/run_native_cpu_ci.py:30–286`, `run_native_cpu_methods.py:26–244`, `audit_hvf_shards.py:40–164`, `prepare_hvf_batches.py:26–89`
- `scripts/diagnose_hvf_methods.py:32–258`, `diagnose_hvf_recovery.py:21–159`
- `.github/actions/hvf-intel-diagnostic/{run.cjs,active-sample.cjs}`, `.github/actions/hvf-intel-recovery/run.cjs`
- `.github/workflows/{hvf.yml,hvf-intel-recovery.yml}`, `.github/actions/hvf-cpu-batches/action.yml`
- `scripts/{NativeHVFTests.def,NativeDarwinTests.def}`
- Associated `scripts/tests/test_{run_native_cpu_methods,audit_hvf_shards,prepare_hvf_batches,diagnose_hvf_methods,diagnose_hvf_recovery}.py`, both actions' `run.test.cjs` and diagnostic `active-sample.test.cjs`

The audit checks exact command and CTest-property contracts, clean source/host/profile identity, whole-method shard membership, complete disjoint result union, required native outcomes, deadline and child-retirement records. The repetition path requires consecutive exact RUN/OK/PASSED records and final retirement. Partial plans/progress, missing shards, required skips and timed-out children cannot satisfy full acceptance. No new proven collector defect was found.

**Separate uncertainty:** Direct-child completion is recorded; absence of every possible descendant is not independently established. No concrete present failure path was demonstrated. Static inspection cannot diagnose the hosted runner loss.

## Existing CI and native evidence

### Exact source snapshot

At 01:04 UTC, the pinned `00f44615` had **10 checks: five successful, four cancelled and one skipped**:
- [Main CI 37165654041](https://github.com/NeverSight/NeverD/actions/runs/37165654041): cancelled. Linux/macOS/Windows integration jobs cancelled; both Windows caller-context jobs succeeded; optional native WHP skipped.
- [Mobile Decompilation 37165654036](https://github.com/NeverSight/NeverD/actions/runs/37165654036): cancelled. Ubuntu/macOS succeeded; Windows cancelled.
- [LLVM Style 37165654017](https://github.com/NeverSight/NeverD/actions/runs/37165654017): succeeded.

Legacy status contexts are empty. These partial successes do not establish full integration. Both CI/mobile workflow headers explicitly enable `cancel-in-progress`; no workflow policy was changed.

The dev Actions creation window **October 3 01:12 UTC to October 4 00:58 UTC** contains **461 runs**, fully enumerated as 100 + 100 + 100 + 100 + 61 and an empty sixth page. At collection, its **99 main-CI runs were 96 cancelled, two failed and one in progress**; that last run later cancelled as recorded above. No green completed main-CI run was observed in the window. The real-application collection had 96 skipped, two cancelled and one queued run; that queued run belongs to earlier source `a4492d7b`, not today's pinned head.

### Already-corrected main-CI failures

The two failed main-CI runs, [37144486804](https://github.com/NeverSight/NeverD/actions/runs/37144486804) and [37147321022](https://github.com/NeverSight/NeverD/actions/runs/37147321022), report the same capability-manifest failure in their inspected Linux logs. The workflow step is named “Verify Debug and Release target flags,” but the failing assertion was `test_repository_manifest_is_honest_and_executable`: the old unparameterized BytecodeCAPI test filter and missing C/JSON/Python callback declarations.

[10198de5](https://github.com/NeverSight/NeverD/commit/10198de584f7b3e6f549f0ec0e09bba4939e2ca5) synchronized the merged declarations; [#384](https://github.com/NeverSight/NeverD/pull/384) added the expanded evidence. Today's capability entry and expected-surface assertions retain these corrections. They are existing delivered fixes, not new fixes from this review, and do not prove current full CI success.

### Native progress and remaining gaps

**Intel Darwin, independently inspected:** [Run 37106013999](https://github.com/NeverSight/NeverD/actions/runs/37106013999), source `8dcc74c59da303176801b99747a60339161b824b`, succeeded. Artifact `11267489438` was downloaded and its SHA-256 matched `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. All 32 original XML/status/mapping sets reconcile: **286 unique results, 52 passed, 234 skipped, zero failed; all 26 required x64 HVF workloads passed**, including the original DarwinNative reference. All 32 processes recorded exit zero, no timeout and retirement. Existing logs additionally confirm ten transport cases, 100 recovery repetitions and isolated CR8 success. This is historical bounded Darwin/transport evidence, not full Intel CPU acceptance or today's head.

**Intel recovery, independently inspected incomplete evidence:** [Run 37159724276](https://github.com/NeverSight/NeverD/actions/runs/37159724276) ended in failure at October 3 23:41:30 UTC. Controller `e4a8169e` tested source `bd284894c60427cf4e6a60e661a1fa0df8a070f5`. Only the plan and eleven progress artifacts survive; there is no final retirement result and the job-log endpoint returns 404. The last artifact `11286787441` was downloaded and matched its server SHA-256. Its untruncated original log proves **252 complete consecutive repetitions and the start of 253**, without a failure/skip in that preserved prefix. It does not prove iteration 253's outcome or the requested 1,000 repetitions. Repository documentation attributes the failure to lost runner communication; that annotation itself was not independently retrieved here. No root cause is inferred.

**ARM64 HVF, maintainer-reported:** Current [HVF documentation](docs/macos-hvf.md) reports clean source `e4a8169e` with 7,125 CPU registrations, 882 passed, 6,243 inapplicable skips, zero failed and 16/16 required native cases; independent Darwin profile 65 passed, 221 skipped and 39/39 required cases, plus 1,000 recovery repetitions and twelve transport cases. These are local evidence reported by the maintainer, not local logs independently accessed today. CPU and Darwin totals overlap and must not be added.

The complete Intel CPU gate, a complete uninterrupted current integration profile and native ARM64 KVM/WHP evidence remain distinct acceptance gaps.

## Today's top priorities

### 1. Close complete Intel CPU acceptance without overstating diagnostic progress

**Dependency:** A matching native Intel execution with durable final evidence; the current preserved recovery prefix is insufficient.

**Next action:** Reconcile already-authorized full inventory/shard evidence against the exact tested source and attempt. Preserve controller/source distinctions and investigate missing final evidence separately from guest semantics.

**Acceptance:** All sixteen shards from one coherent clean source/attempt form the exact full inventory with every required native outcome passing and retirement recorded, or an equivalent complete unsharded gate. A transport success, partial shard or 252-of-1,000 recovery prefix does not qualify. This review initiates no execution.

### 2. Obtain uninterrupted cross-platform integration and real-application qualification

**Dependency:** The capability-manifest corrections are present; one identified source still needs terminal complete evidence.

**Next action:** Inspect eventual authorized automatic results without transferring success between commits. Keep real-application producer/consumer identities separate from mobile fixture results.

**Acceptance:** Linux, macOS and Windows complete their intended audited integration profiles for the same identified source, with required cases executed; real-application qualification supplies actual completed evidence instead of skipped/queued consumers.

### 3. Review pending public boundary changes and map evidence to open criteria

**Dependency:** Draft [#394](https://github.com/NeverSight/NeverD/pull/394) (LLVM C aggregate values/ABI) and [#395](https://github.com/NeverSight/NeverD/pull/395) (Android default symbol scopes) need review and exact-head evidence.

**Next action:** Review aggregate interoperability/rejection cases and explicit scope/residency/flag contracts before counting them delivered. Relate implemented Windows/emulator and Swift/Objective-C work to [#104](https://github.com/NeverSight/NeverD/issues/104) and [#101](https://github.com/NeverSight/NeverD/issues/101), preserving remaining unsupported cases.

**Acceptance:** Each selected criterion has an implementation/evidence link or explicit gap; draft work is not counted as merged acceptance. Seventeen issues remain open, including fourteen epics; no milestones, and only #12 is assigned. No issue is closed by inference from merged PRs.

## Daily log

### 2026-10-04 — Callback/output static review and native acceptance reconciliation

- Enumerated 226 commits and 895 raw changed file/submodule paths since the previous pinned head.
- Reviewed callback lifetime/validation, Linux/Bionic output boundaries and native evidence collectors with the exact bounded scope above; found no new proven defect warranting a code correction.
- Counted 31 merged PRs by the initial observation, separating progress-only #363; ordinary issue count stayed 17.
- Independently reconciled historical Intel Darwin success and the incomplete 252-repetition recovery prefix; kept reported local ARM64 results separate.
- Diagnosed two historical main-CI failures as already-corrected capability drift. The pinned main CI and mobile workflows later cancelled.
- Preserved the complete existing tracker and its later Windows policy-path correction.
- Documentation-only topic-branch/draft-PR proposal. No tests, builds, repository scripts, manual CI, merge, deployment, dependency or security-setting changes. The English commit uses `[skip ci]`; independently configured automatic checks may still occur.

## Publication observation and limits

At 01:05 UTC, dev had advanced to [064b01cc](https://github.com/NeverSight/NeverD/commit/064b01ccadb1aa87788e47719f01e462353300a8) through [#396](https://github.com/NeverSight/NeverD/pull/396), merged at 01:01:05 UTC. It factors shared LLVM C exit tests. This later change is outside the pinned 226-commit inventory and source review. Open PRs fell to **two** and merged PRs since yesterday became **32**, including progress-only #363. The publication branch starts from the re-read current dev; PROGRESS.md was unchanged from the initially read blob.

Collection limits:
- Open issue/PR collection: twenty records (seventeen ordinary issues, three PRs), then empty second page. Updated collection: 34 PRs, no ordinary issue, then empty second page.
- The first 100 most recently updated PRs crossed the tracking boundary and cover all in-window records; older complete PR history was not enumerated.
- Exact-head workflows/checks: three/ten records, both followed by empty second pages. Six selected PRs (#363, #383, #392, #394–396) returned no submitted reviews, review threads or comments; this is not independent approval.
- Two failed-run job collections and selected Linux logs were inspected. Not every historical log, native artifact or PR diff was audited. Artifact verification read data only and did not execute repository workloads.
- AGENTS.md and CONTRIBUTING.md were read first; relevant architecture/testing/roadmap sections and repository guidance were consulted. Only the root AGENTS.md exists in the full tree.
- GitHub reports dev unprotected and an empty repository ruleset collection. The topic-branch/draft-PR workflow is still followed; no protection settings were changed.
- Broad Windows SEH/context/module semantics, Swift/Objective-C identity recovery, numeric optimization, all other changed paths and unmerged #394/#395 code remain outside today's bounded source audit. Static review cannot establish compilation, runtime behavior, race freedom, total ISA coverage or release readiness.

## Previous snapshots (preserved)

<details>
<summary>October 3 tracker with the complete October 2, October 1 and September 30 history</summary>

# NeverD Daily Progress

Last verified: **2026-10-03 09:12 Asia/Shanghai (UTC+08:00)** / **2026-10-03 01:12 UTC**

This is a point-in-time daily issue/PR and static-review tracker. The [roadmap](docs/roadmap.md), [architecture](docs/architecture.md), [testing guide](docs/testing.md) and [contribution guidance](CONTRIBUTING.md) remain authoritative. Priorities below are proposed acceptance work, not assigned deadlines or an overall completion percentage.

## Current snapshot

Source review is pinned to the observed dev commit. Issue/PR counts precede this documentation proposal.

| Measure | Verified state |
| --- | --- |
| Open issues | 17; unchanged |
| Open pull requests | 0; unchanged before this proposal |
| PRs merged since 2026-10-02 01:11 UTC | 35: #328–362 |
| PRs closed without merge in that window | 0 |
| Ordinary issues updated/closed in the window | 0 / 0 |
| Observed dev commit | [99340b58](https://github.com/NeverSight/NeverD/commit/99340b58657e3907588a33b363435f79b74b86fe) |
| Previous observed dev commit | [d87f27d2](https://github.com/NeverSight/NeverD/commit/d87f27d29ecb097d1fa8483006797dc4765c3972) |
| Change inventory | 262 commits; 567 changed file/submodule paths, including 175 added and zero deleted |
| New statically confirmed defects | 0 in the bounded scope below |
| Proposed code fixes today | None; documentation-only update |

The [comparison](https://github.com/NeverSight/NeverD/compare/d87f27d29ecb097d1fa8483006797dc4765c3972...99340b58657e3907588a33b363435f79b74b86fe) was read across 100 + 100 + 62 commit records and an empty fourth page. GitHub's comparison limits its file list to 300 paths; the 567-path inventory instead compares the complete recursive Git trees (5,313 and 5,496 entries, neither truncated), excluding directories. Enumeration is not a claim that every changed path was audited.

### Changes since the previous snapshot

- Yesterday's [PR #328](https://github.com/NeverSight/NeverD/pull/328) merged at 2026-10-02 01:29:42 UTC as [3c1f637f](https://github.com/NeverSight/NeverD/commit/3c1f637f514a9e6902a3ccc02690565d08408efb). The canonical WDK path assertion is present on the inspected dev tree. Its merged PROGRESS.md exactly matches the previous tracker preserved below.
- [#329](https://github.com/NeverSight/NeverD/pull/329) expanded complete WDK corpus/SEH acceptance. Later x64 bit-string, string transfer and string comparison changes landed in [#336](https://github.com/NeverSight/NeverD/pull/336), [#338](https://github.com/NeverSight/NeverD/pull/338) and [#342](https://github.com/NeverSight/NeverD/pull/342).
- Windows process memory, module graphs, lifetimes and exports landed through [#346](https://github.com/NeverSight/NeverD/pull/346), [#350](https://github.com/NeverSight/NeverD/pull/350), [#352](https://github.com/NeverSight/NeverD/pull/352), [#355](https://github.com/NeverSight/NeverD/pull/355) and [#357](https://github.com/NeverSight/NeverD/pull/357). These changes enlarge the current Windows native requirement to 404 outcomes.
- Native macOS HVF and Darwin environments were integrated. The latest [99340b58](https://github.com/NeverSight/NeverD/commit/99340b58657e3907588a33b363435f79b74b86fe) writes and captures Intel RIP/RFLAGS directly through VMCS after cancellation recovery. Its actual Intel full gate is still running.
- Interpreter recovery domains, bounded source generation, exported bytecode recovery, Swift/Objective-C binding and additional lifting changes were inventoried, not comprehensively source-reviewed today.

## Static review

**Mode:** Read-only source, diffs, caller/test contracts, configuration and existing GitHub CI evidence. No repository program, build, test, script, linter or formatter was executed. No workflow was manually dispatched or rerun. No new defect was established strongly enough to justify an automatic code change.

### HVF cancellation, register transfer and callers

HVF source coverage and findings are recorded below. Source invariants are not runtime acceptance.

The source/test audit covers **38 distinct artifacts** in total, including the native-evidence and historical-failure sections below; four workflow files were additionally inspected.

**Full HVF/adjacent source and test reads:**
- `lib/emulation/backends/hvf/{HvfExecutor.cpp,HvfExecutor.h,HvfX64Machine.cpp,HvfX64Registers.def}`
- `lib/emulation/backends/RunDeadline.h` and `lib/emulation/core/MachineRunControl.h`
- `lib/emulation/arch/x86_64/{X64Machine.cpp,X64Machine.h,X64MachineProbe.cpp,X64MachineProbe.def,X64OperandRegisters.def,CheckedX64Instructions.def}`
- `lib/emulation/os/windows/driver/WindowsX64ExecutionPolicy.cpp` and `include/neverd/emulation/X64Registers.def`
- `unittests/emulation/{HvfExecutorTests.cpp,HvfTests.cpp,HvfTestPolicy.h,X64StateTransitionTests.cpp,MachineRunControlTests.cpp,RunControlTests.cpp,NativeEntryTests.cpp}`
- `scripts/NativeHVFTests.def`, also included in the evidence inventory review below

**Focused adjacent sections:** `CheckedX64Backend.cpp` register validation, checked admission, native step/error handling and RAM/CPU publication (principally lines 191–204 and 265–590); `unittests/emulation/CMakeLists.txt` registrations for `NeverDHvfTests`, `NeverDX64ExceptionTests` and `NeverDRunControlTests`. The native-evidence script was also read in full below.

- Intel RIP/RFLAGS prepare and capture use the VMCS boundary on each step; their register-API entries are removed. Capture writes a staged next packet, so a failed field read does not publish partially captured caller state.
- Unsolicited Intel IRQ exits retry within one deadline invocation, preserving native state and the cancellation generation. Native errors return immediately. The watchdog is disarmed and acknowledged before cancelled vCPU teardown/recreation.
- Every Intel vCPU creation, including recovery, binds managed `IA32_KERNEL_GS_BASE`, denies guest MSR access and initializes the private value. CR8 completion accepts only authenticated MOV-from-CR8 qualifications and validates privilege, GPR, instruction length and value before modifying state.
- The checked caller discards speculative RAM on machine failure. Ordinary cancellation/capture failure does not publish staged CPU state; authenticated exceptions retain their precedence.
- The cancellation regression covers deadlines, stop tokens, unsolicited interrupt followed by stop, a real native return, retry at another RIP and completion-error precedence. After recreation it asserts RIP/RFLAGS/AX, not the complete state packet.
- The separate CR8 regression covers all 16 destination GPRs, priorities 0–15, supervisor success, user #GP(0), RF and complete unchanged-state comparisons. It belongs to `NeverDX64ExceptionTests`, outside the 10-case Intel transport-only target. Startup full-state probing occurs at machine creation, not after every cancelled vCPU recreation.

**Remaining uncertainty:** Only matching-host execution can establish that Apple's framework preserves the intended native state after recreation. A green transport subset cannot replace the full CPU/Darwin gate or establish complete post-cancellation state coverage.

### Native-evidence enforcement and current inventories

Read in full: [run_native_cpu_ci.py](scripts/run_native_cpu_ci.py), [test_run_native_cpu_ci.py](scripts/tests/test_run_native_cpu_ci.py), [NativeCPUTests.def](scripts/NativeCPUTests.def), [NativeDriverTests.def](scripts/NativeDriverTests.def), [NativeHVFTests.def](scripts/NativeHVFTests.def), [NativeDarwinTests.def](scripts/NativeDarwinTests.def), [WhpMemoryCases.def](unittests/emulation/WhpMemoryCases.def), [DriverBuiltinImages.def](unittests/emulation/DriverBuiltinImages.def), [DriverBackendParityCases.def](unittests/emulation/DriverBackendParityCases.def), and [test_build_wdk_driver_fixtures.py](scripts/tests/test_build_wdk_driver_fixtures.py).

Also reviewed the shared `TestRecord` / `parse_inventory` boundary in [audit_ci_test_inventory.py](scripts/audit_ci_test_inventory.py), and JUnit label, status, identity and count parsing in [audit_ci_test_results.py](scripts/audit_ci_test_results.py).

- Required host-specific test names are selected by explicit ARM64/x64 identity; an unknown architecture is rejected.
- Native profiles reject missing registrations and non-passing required outcomes. JUnit infrastructure not-run, explicit skips, failures and disabled cases remain distinct. Outcome reconciliation retains test name and owner-label identity.
- The transport-only HVF profile is an explicit subset of the full inventory; the Darwin profile expands every declared workload across each matching guest platform.
- Static text inventory counts reconcile: 160 explicit CPU names + 16 WHP mapping cases = 176 CPU outcomes; 26 built-in images + 46 WDK images + 40 scenarios at two bases = 224 driver outcomes; four SEH cases bring the current Windows total to **404**.
- These are declaration counts, not newly executed results. Existing regression source covers missing owners, missing results, skipped mandatory hardware cases, wrong owner identity, malformed host selections and deleted Darwin workload requirements.

### Historical failure reconciliation

Read both complete current shared-event headers: [AndroidNative.h](include/neverd/emulation/AndroidNative.h) and [ProcessCall.h](include/neverd/emulation/ProcessCall.h), the recovery-surface expectations in [test_check_capabilities.py](scripts/tests/test_check_capabilities.py), and the corresponding corrective commit patches.

- The two older main-CI failures in the “Verify Debug and Release target flags” step actually failed `test_repository_manifest_is_honest_and_executable`, owing to missing recovery-v4 expectations. [c4010c33](https://github.com/NeverSight/NeverD/commit/c4010c33e96851fc2ff170a0c06d6483ab9fad51) supplies those expectations; the inspected file retains them and later APIs. This was not evidence of incorrect Debug flags.
- The third older main-CI failure was a duplicate `NativeCallEvent` definition while compiling Linux services. [c3493d72](https://github.com/NeverSight/NeverD/commit/c3493d723135b0e4e7bfdbf3a810bfd562bb86c2) removes the Android duplicate and retains Library/Symbol in the shared header. The inspected source contains that correction.
- These already-delivered fixes were not duplicated. No claim is made that current full integration has passed.

Relevant architecture/testing/HVF guidance, the complete HVF workflow, and CI/mobile/style trigger and concurrency sections were also inspected. The rest of the 567 changed paths, broader Windows loader/export semantics, Darwin syscall semantics, Objective-C/Swift source recovery and interpreter/refinement work remain outside this bounded audit.

## Existing CI evidence

**Exact-head snapshot: 2026-10-03 01:12 UTC**, for `99340b58657e3907588a33b363435f79b74b86fe`.

| Workflow | Observed state | Evidence |
| --- | --- | --- |
| CI | In progress; all three platform jobs at their configuration/script-verification step; optional WHP job skipped | [37084475387](https://github.com/NeverSight/NeverD/actions/runs/37084475387) |
| HVF hosted-intel / full | In progress at the required transport step; full CPU and Darwin stages not reached | [37084520059](https://github.com/NeverSight/NeverD/actions/runs/37084520059) |
| Mobile Decompilation | Workflow API reports queued; Ubuntu job succeeded, macOS in progress, Windows queued | [37084475394](https://github.com/NeverSight/NeverD/actions/runs/37084475394) |
| Mobile Real Applications | Skipped | [37084500511](https://github.com/NeverSight/NeverD/actions/runs/37084500511) |
| LLVM Style | Success | [37084475343](https://github.com/NeverSight/NeverD/actions/runs/37084475343) |

Exact-head check runs: **18 total: 2 successful, 10 skipped, 5 in progress and 1 queued; zero failed at this snapshot**. Legacy statuses are empty; their combined `pending` state does not establish failure or success. The existing manually initiated Intel workflow was observed only; this review did not initiate it.

The dev Actions collection created from **2026-10-02 01:11 UTC through 2026-10-03 01:05 UTC** contains **597 runs**, across six non-empty pages (100 + 100 + 100 + 100 + 100 + 97) and an empty seventh page. Its **113 main CI runs comprise 109 cancelled, three failed and one in progress**, with no completed green main CI run in that query. The three failures above were diagnosed from their Linux logs and corresponding source corrections. Yesterday's tracked main CI and Mobile Real Applications later cancelled; yesterday's mobile-fixture success stays scoped to yesterday's commit.

### Native progress since yesterday

**Windows WDK/CPU:** The [native WHP job](https://github.com/NeverSight/NeverD/actions/runs/36981864458/job/110758081823) at `9d4c130c2f11d95a2f80f1055dfdbb34de07715c` reports **1,121 passed, 1,667 skipped, zero failed/disabled/not-run**, no missing/unexpected identities, and **all 359 required outcomes executed**. This confirms substantial progress beyond yesterday's 197-outcome blocked gate. It does not validate the newer 404-outcome inventory or today's head.

**Darwin on x64 KVM/WHP:** The [existing run](https://github.com/NeverSight/NeverD/actions/runs/37062839703) at `36e11ca8a3d80aecf585d3328018839ce7fdb989` succeeded on both hosts. Both inspected job logs record **51 passed, 235 skipped, zero failed**, with **all 26 required Darwin workloads executed**. This is Darwin guest-contract evidence on Linux/Windows native transports, not Intel macOS HVF acceptance. The separate [native macOS kernel reference](https://github.com/NeverSight/NeverD/actions/runs/37064795867) succeeded on both x86_64 and arm64 at `e727d3eab7086063bb392444bd55014ac48d43c3`; only its job/step metadata was checked here.

**Intel HVF:** The older [transport job](https://github.com/NeverSight/NeverD/actions/runs/37083061831/job/111087568902) at `48042a5e90e0977585114de092e423cd64b7f95f` had **9 passed / 1 failed / no skips**. The exact failure was the first `Prepare(RetryPC)` after cancellation in `NativeIntelCancellationAndCompletionFailureAllowRetry`, reporting an unexpected VM exit. Full CPU and Darwin stages were skipped. The new VMCS correction is on today's head; its existing full workflow remains incomplete.

**Apple Silicon HVF:** [macos-hvf.md](docs/macos-hvf.md) reports a clean-source full gate at `48042a5e` with 841 passed, 5,942 inapplicable skips and all 16 required outcomes, plus the 12-case transport subset. Those local results are maintainer-reported documentation, not logs independently accessed in this review. They must not be described as absent native ARM64 evidence, but also must not be transferred to Intel HVF or ARM64 KVM/WHP.

## Today's top priorities

### 1. Establish complete Intel HVF acceptance after recovery correction

**Status:** The previous gate failed after cancellation; the exact-head full workflow is still at transport validation.

**Next action:** Review its eventual transport, full CPU and Darwin outcomes against the same source identity. Include the all-GPR CR8/CPL3 regression and distinguish the retry test's limited RIP/RFLAGS/AX assertions from complete state coverage.

**Acceptance:** The identified commit passes every required transport case, the complete full-profile CPU gate and all matching Darwin workloads, with no missing/skipped required tests. Probe-only or transport-only success is insufficient. No manual execution is part of this static review.

### 2. Obtain uninterrupted three-platform integration and real-application evidence

**Status:** Current main CI is incomplete; the bounded dev window has 109 cancellations and no green main CI. Current mobile fixtures are incomplete and real-application qualification is skipped.

**Next action:** Reconcile existing terminal outcomes after the source fixes already present. Keep any superseding commit separate. Record real-application producer/consumer identity and actual execution instead of carrying forward a historical green fixture result.

**Acceptance:** Linux, macOS and Windows complete the intended integration profile for one identified commit, with audited test execution. Real-application qualification supplies actual evidence; a skipped consumer does not satisfy acceptance.

### 3. Reconcile expanded native gates and open issue criteria

**Status:** Historical Windows evidence passes 359 required outcomes; the current inventory requires 404. Seventeen issues remain open, including 14 epics; none has a milestone, and only [#12](https://github.com/NeverSight/NeverD/issues/12) is assigned.

**Next action:** Evaluate authorized existing/native evidence for the expanded Windows process/module/export requirements. Map delivered work to [#104](https://github.com/NeverSight/NeverD/issues/104), [#101](https://github.com/NeverSight/NeverD/issues/101) and [#4](https://github.com/NeverSight/NeverD/issues/4), separating implementation, verified platform scope and remaining gaps. The historical #12 report was not re-audited today.

**Acceptance:** Each selected criterion has an implementation/evidence link or an explicit gap; current Windows acceptance executes all 404 mandatory outcomes at an identified commit. Matching-host results remain distinct, and no issue is closed solely because related PRs merged.

## Daily log

### 2026-10-03 — HVF static audit and native-evidence reconciliation

- Inventoried 262 commits and 567 changed file/submodule paths since the previous source snapshot; reviewed the bounded HVF, native-evidence and historical-failure scope above.
- Found no new statically proven defect requiring a code change. Yesterday's WDK correction is merged.
- Reconciled 35 merged PRs, no unmerged closures, 17 open issues and no open PR before this proposal.
- Verified historical Windows 359-outcome success and x64 KVM/WHP Darwin 26-workload success, retaining the current 404-outcome and Intel HVF acceptance gaps.
- Diagnosed three older main-CI failures and verified their source corrections already exist. Current integration remains incomplete.
- Preserved the complete October 2 tracker, including October 1 and September 30 history, below.
- This English documentation-only proposal uses a topic branch and draft PR. No build, test, repository script, manual workflow trigger, merge, deployment, dependency revision or security-setting change was performed. The commit uses `[skip ci]`; independently managed automatic checks may still occur.

## Tracking conventions and limits

- Open issue/PR collection returned 17 ordinary issues and no PRs, followed by an empty second page; a separate open-PR collection was empty. Updated issue/PR records returned 35 PRs and an empty second page, with no ordinary issue.
- The first 100 most recently updated PRs extend past the tracking boundary and include all 35 in-window records. Older complete PR history was not enumerated.
- Exact-head workflow/check collections returned five/18 records and empty second pages. Main CI and current HVF job collections returned four/one records with empty second pages. Selected historical job collections were small; not every historic log was inspected.
- The separate dev manual-event collection returned 34 records and an empty second page. The historical WHP success was followed directly from repository evidence and is separate from the dev-bound Actions count.
- PRs #328, #329, #357 and #362 returned no submitted reviews, inline review threads or conversation comments. This is not independent approval.
- GitHub reports dev unprotected and an empty repository ruleset collection. The topic-branch/PR contribution workflow is still followed; no protection was changed.
- PROGRESS.md was compared with yesterday's merged version and re-read before writing; preserve this full history and future human edits.
- Pending, skipped, cancelled, failed, maintainer-reported and independently inspected results remain distinct. Static review does not establish compilation, runtime behavior, race freedom, complete ISA coverage or release readiness.

## Publication-time observation

At 2026-10-03 01:16 UTC, dev had advanced to [eee92640](https://github.com/NeverSight/NeverD/commit/eee926404c3a7ee91d46e9e84fa04d47f9543107), which separates and time-bounds Intel transport compilation/execution in the HVF workflow. Its one-file patch was read, and PROGRESS.md was unchanged. This later commit is outside the pinned 262-commit/567-path inventory and source snapshot above. The existing Intel run for `99340b58` was still in progress; no terminal acceptance is inferred.

## Previous snapshots (preserved)

<details>
<summary>2026-10-02 tracker, priorities, evidence and earlier history</summary>

# NeverD Daily Progress

Last verified: **2026-10-02 09:11 Asia/Shanghai (UTC+08:00)** / **2026-10-02 01:11 UTC**

This is a point-in-time daily issue/PR and static-review tracker. The [roadmap](docs/roadmap.md), [architecture](docs/architecture.md), [testing guide](docs/testing.md), and [contribution guidance](CONTRIBUTING.md) remain authoritative. Priorities are proposals, not assigned deadlines or a completion percentage.

## Current snapshot

The issue/PR snapshot precedes today's review PR. Source inspection is pinned to the observed dev commit, not to a moving branch.

| Measure | Verified state |
| --- | --- |
| Open issues | 17; unchanged |
| Open pull requests | 0; unchanged before this review PR |
| PRs merged since 2026-10-01 01:02 UTC | 41: #285–319 and #321, #323–327 |
| PRs closed without merge in that window | 2: #320 and #322 |
| Ordinary issues updated/closed in the window | 0 / 0 |
| Observed dev commit | [d87f27d2](https://github.com/NeverSight/NeverD/commit/d87f27d29ecb097d1fa8483006797dc4765c3972) |
| Previous observed dev commit | [6eb2e5c6](https://github.com/NeverSight/NeverD/commit/6eb2e5c6423f7b2977568fe070c7cd334f503572) |
| Change inventory | 266 commits; 521 changed file/submodule paths |
| Bounded source review | 31 source, test and build artifacts, with sections listed below |
| New confirmed defects | 1 test-portability defect; one-line correction committed |
| New production-code defects established | 0 in the sampled scope |

The [comparison](https://github.com/NeverSight/NeverD/compare/6eb2e5c6423f7b2977568fe070c7cd334f503572...d87f27d29ecb097d1fa8483006797dc4765c3972) was read across three commit pages (100 + 100 + 66), followed by an empty page. GitHub limits the comparison's changed-file list to 300 paths; the 521-path inventory therefore comes from comparing complete recursive Git trees (5,154 and 5,313 entries, neither truncated). Enumeration is not a claim that every changed path was code-reviewed.

### Changes since the previous snapshot

- Yesterday's [tracking PR #285](https://github.com/NeverSight/NeverD/pull/285) merged at 2026-10-01 03:53:57 UTC. Its PROGRESS.md content is identical to the version on the inspected dev tree; the complete previous tracker is preserved below.
- [#319](https://github.com/NeverSight/NeverD/pull/319) centralized width-aware absent-SIB-index handling across scalar address construction, metadata auditing and EVEX validation.
- [#308](https://github.com/NeverSight/NeverD/pull/308), [#311](https://github.com/NeverSight/NeverD/pull/311), [#313](https://github.com/NeverSight/NeverD/pull/313) and [#316](https://github.com/NeverSight/NeverD/pull/316) refined native x64 state ownership, XSAVE handling and physical x87 transition evidence.
- [#317](https://github.com/NeverSight/NeverD/pull/317) expanded the native driver gate from the older 97 required CPU/driver outcomes to 197, including the reproducible WDK corpus. Its PR description's queued run has since failed before native execution; today's correction addresses the observed test assertion.
- Other delivered work includes Android ARM64 native environments, external bytecode profiles, preferred-base PE evidence, loop refinement, bounded frame transfers and Objective-C/Swift recovery. These were inventoried, not comprehensively audited.

## Static review

**Mode:** Source, diff, caller, configuration and existing CI-log inspection only. No repository program, build, test, linter, formatter or script was executed. No workflow was manually dispatched or rerun.

### Confirmed defect and correction

**Test-portability defect:** [test_build_wdk_driver_fixtures.py](scripts/tests/test_build_wdk_driver_fixtures.py), `test_cmake_paths_preserve_spaces_and_reject_list_or_code_expansion`, compared the original path spelling with a cache entry that deliberately uses `Path.resolve()`.

The historical [Windows WHP job](https://github.com/NeverSight/NeverD/actions/runs/36899322120/job/110494224787), at commit `9944433cc4593a59fe899f90c4a532306a4f9ed6`, failed this exact assertion: the temporary path used the short Windows user-directory spelling while the emitted path used its resolved long spelling. The script suite reported 25 tests with one failure. The subsequent native build/verification step was skipped. This is not evidence of an XSAVE or WHP execution failure.

The same raw-path assertion was still present at today's pinned dev commit. [Fix 9b5b23ef](https://github.com/NeverSight/NeverD/commit/9b5b23efa35c577cdb22f6cd18eef7d73ac19e76) changes only the expected path to `image.resolve().as_posix()`. It retains the quoted-space assertion and the separate invalid-character rejection checks. Production canonicalization and validation are unchanged.

**Verification:** Independently checked the producer/test contract and historical log, then remotely read back the fixed file and commit diff. The commit changes one line in one test file. The fix has not been executed or validated by a new Windows run; it is proposed on today's topic branch and is not merged.

### Source coverage

The following 31 artifacts were inspected at `d87f27d2`. Whole-file reads are distinguished from selected sections.

**x86 SIB address semantics: 10 artifacts**

- [X86LiftDetail.h](lib/lift/X86/X86LiftDetail.h): shared `isNoSibIndex` declaration and relevant PR patch
- [X86Lifter.cpp](lib/lift/X86/X86Lifter.cpp): `isNoSibIndex`, `computeEA`, memory read/store callers, undefined-output memory audit, unmapped-register rejection and final sidecar publication
- [X86LiftSIMDMemory.cpp](lib/lift/X86/X86LiftSIMDMemory.cpp): ordinary memory validation, raw SIB/tail checks, EVEX/VEX3 adapters and masked memory-load construction
- [X86LiftSIMDMove.cpp](lib/lift/X86/X86LiftSIMDMove.cpp): masked memory-operand validation and full-vector move caller, including address construction and mask handling
- [X86Regs.cpp](lib/lift/X86/X86Regs.cpp): general-register mapping and invalid-register fallback
- [Decoder.cpp](lib/decode/Decoder.cpp): `liftToLow` dispatch and undefined-effects initialization
- [X86Lifter.h](include/neverd/lift/X86Lifter.h): shared memory-intrinsic/address helpers
- [X86_64_NoIndexAddressTests.cpp](unittests/lift/x86_64/X86_64_NoIndexAddressTests.cpp): all nine regression cases
- [X86_64_EVEXMemoryBroadcastTests.cpp](unittests/lift/x86_64/X86_64_EVEXMemoryBroadcastTests.cpp): absent-index masked-broadcast/move and contradictory-metadata cases, plus their helpers
- [unittests/lift/CMakeLists.txt](unittests/lift/CMakeLists.txt): dedicated no-index test-target registration

**XSAVE/WHP state transfer: 19 artifacts, read in full**

- [X64FPState.cpp](lib/emulation/arch/x86_64/X64FPState.cpp), [.h](lib/emulation/arch/x86_64/X64FPState.h), and [.def](lib/emulation/arch/x86_64/X64FPState.def)
- [X64Machine.h](lib/emulation/arch/x86_64/X64Machine.h), [X64MachineProbe.cpp](lib/emulation/arch/x86_64/X64MachineProbe.cpp), and [X64MachineProbe.def](lib/emulation/arch/x86_64/X64MachineProbe.def)
- [WhpXsaveState.h](lib/emulation/backends/whp/WhpXsaveState.h), [WhpXsaveRegisters.def](lib/emulation/backends/whp/WhpXsaveRegisters.def), [WhpProtocol.def](lib/emulation/backends/whp/WhpProtocol.def), and [WhpMachine.cpp](lib/emulation/backends/whp/WhpMachine.cpp)
- [X64XsaveTests.cpp](unittests/emulation/X64XsaveTests.cpp), [X64XsaveCases.def](unittests/emulation/X64XsaveCases.def), [WhpXsaveTests.cpp](unittests/emulation/WhpXsaveTests.cpp), and [WhpHostFailureCases.def](unittests/emulation/WhpHostFailureCases.def)
- [X64FPStateTests.cpp](unittests/emulation/X64FPStateTests.cpp), [X64FPCases.def](unittests/emulation/X64FPCases.def), and [X64MachineProbeTests.cpp](unittests/emulation/X64MachineProbeTests.cpp)
- [X64StateTransitionTests.cpp](unittests/emulation/X64StateTransitionTests.cpp) and [X64StateTransitionCases.def](unittests/emulation/X64StateTransitionCases.def)

**WDK assertion and producer: 2 artifacts, read in full**

- [build_wdk_driver_fixtures.py](scripts/build_wdk_driver_fixtures.py): especially `cache_entry`, its build caller and publication
- [test_build_wdk_driver_fixtures.py](scripts/tests/test_build_wdk_driver_fixtures.py): especially path spelling, quoted spaces and pre-resolution rejection tests

Repository guidance, relevant architecture/testing sections, roadmap hardening scope, CI/mobile trigger and concurrency definitions, and the native CI configuration step were also inspected.

### Findings in the production-code sample

No new production-code correctness defect was established strongly enough for an automatic fix.

- Absent SIB indices are width-specific; they do not contribute a scaled register term. Effective addresses retain 32-bit wrapping/zero-extension and separate FS/GS offsets from ordinary address provenance. Real R12 indices remain ordinary mapped registers. Raw EVEX tail validation checks the encoded index extension before accepting absent-index aliases.
- Invalid pseudo-register bases or wrong-width pseudo-indices become unmapped register operands and are rejected transactionally in strict lifting. Existing tests cover address widths, all redundant scale encodings, loads/stores, relocation ownership, undefined sidecars, masked accesses and malformed metadata. These are descriptions of test source, not new test results.
- Standard initial-SSE XSAVE packets retain and validate MXCSR; compacted initial-SSE packets reset it. Both clear XMM lanes. The codec stages the next state before publication and rejects unsupported layout bits, inconsistent lengths and truncated extension storage.
- WHP capture stages XSAVE decode, named metadata reads, consistency checks and supplemented-state validation before publishing. The machine caller stages complete state around the transfer. x87 transition/cancellation fixtures retain exact physical-state assertions.

**Limits:** The remaining changed paths, Objective-C/Swift pipeline, Android environment, general loop/refinement work, resource-cache concurrency, ARM64 transport and PE refactor were not source-audited in this pass. Static inspection does not establish runtime behavior, compilation/linking, formatting, race freedom, release readiness or complete ISA coverage.

## Existing CI evidence

**Exact-head CI snapshot: 2026-10-02 01:10 UTC**, for `d87f27d29ecb097d1fa8483006797dc4765c3972`.

| Workflow | Observed state | Evidence |
| --- | --- | --- |
| CI | In progress; Linux, macOS and Windows building; optional native WHP job skipped | [36947225176](https://github.com/NeverSight/NeverD/actions/runs/36947225176) |
| Mobile Decompilation | Success on Ubuntu, macOS and Windows | [36947225342](https://github.com/NeverSight/NeverD/actions/runs/36947225342) |
| Mobile Real Applications | Pending | [36949181960](https://github.com/NeverSight/NeverD/actions/runs/36949181960) |
| Push on dev | In progress | [36947225134](https://github.com/NeverSight/NeverD/actions/runs/36947225134) |
| LLVM Style | Success | [36947225307](https://github.com/NeverSight/NeverD/actions/runs/36947225307) |
| Code Quality: Push on dev | Success | [36947225199](https://github.com/NeverSight/NeverD/actions/runs/36947225199) |
| Prebuilt LLVM Audit | Success | [36947225314](https://github.com/NeverSight/NeverD/actions/runs/36947225314) |
| EVM Upstream Audit | Success | [36947225350](https://github.com/NeverSight/NeverD/actions/runs/36947225350) |

Exact-head check runs: **16 total: 11 successful, 1 skipped, 4 in progress, 0 failed**. Legacy commit statuses are empty; their combined `pending` state alone is not a failure or an all-checks pass.

The dev Actions collection created from **2026-10-01 01:02 UTC through 2026-10-02 01:03 UTC** contained **929 runs**, across ten pages (nine of 100, one of 29). Its **115 main CI runs comprise 114 cancelled and one in progress**; no completed green dev main CI run was observed in that window. This is an integration-evidence gap, not proof of a code failure. Yesterday's tracked [main CI](https://github.com/NeverSight/NeverD/actions/runs/36795886427) and [mobile run](https://github.com/NeverSight/NeverD/actions/runs/36795886438) both subsequently cancelled.

### Native evidence must retain its scope

The older [WHP run 36894495012](https://github.com/NeverSight/NeverD/actions/runs/36894495012), at `e7f205ab4ecb5a91646e8ea9ee8dd6b74ecb230a`, succeeded. Its [job log](https://github.com/NeverSight/NeverD/actions/runs/36894495012/job/110477966198) records **803 passed, 1,522 skipped, zero failed/missing/not-run**, and all **97 required native CPU/driver outcomes executed**. This is real earlier Windows evidence, not current-head or expanded-corpus acceptance.

The later [197-outcome WDK/native run](https://github.com/NeverSight/NeverD/actions/runs/36899322120) failed the path assertion before native verification. Today's one-line fix addresses that blocker but does not prove the expanded run will pass. The optional native job is skipped on today's ordinary dev push. Native ARM64 runtime evidence remains explicitly unavailable in the reviewed documentation.

## Today's top priorities

### 1. Obtain uninterrupted exact-head integration evidence

**Status:** Mobile fixtures now pass on the inspected head; the three-platform main CI is still building. The preceding 114 dev main CI runs in the collection were cancelled.

**Next action:** Inspect the existing main CI's terminal platform/test outcomes, retaining exact commit identities and all skip/missing-test distinctions. If development supersedes the run, record the new integration gap rather than transferring a previous success.

**Acceptance:** All three platform jobs finish for one identified integration commit, with required test execution audited. No manual dispatch/rerun is included in this static-only review.

### 2. Validate the expanded native WDK gate after the path-test correction

**Status:** The older 97-outcome native gate is green; the newer 197-outcome gate stopped before CPU verification. The canonical-path assertion correction is committed on today's review branch, unexecuted.

**Next action:** Review the one-line test fix and later evaluate an authorized Windows native CPU/driver run for a commit containing it. Preserve the original canonicalization and invalid-path protections.

**Acceptance:** Configuration passes, all 197 required native outcomes execute, and failed, missing or skipped required cases remain failures. The observed older 97-outcome run and portable XSAVE source coverage do not satisfy this larger gate.

### 3. Finish mobile real-application qualification and reconcile issue criteria

**Status:** Exact-head Mobile Decompilation is green; Mobile Real Applications is pending. The 17 open issues retain unchanged planning metadata: 14 epics, no milestones, and only [#12](https://github.com/NeverSight/NeverD/issues/12) assigned.

**Next action:** Record the real-application producer/consumer identities and actual result. Map merged implementation and current evidence to [#101](https://github.com/NeverSight/NeverD/issues/101), [#104](https://github.com/NeverSight/NeverD/issues/104) and [#4](https://github.com/NeverSight/NeverD/issues/4), distinguishing delivered code from outstanding acceptance. The historical Windows report in #12 was not re-audited today.

**Acceptance:** Selected criteria have an implementation/evidence link or an explicit gap and a bounded next owner/action. A merged PR, stale unchecked issue or skipped consumer alone does not establish completion.

## Daily log

### 2026-10-02 — Static review, Windows path assertion fix and evidence refresh

- Inventoried 266 commits and 521 changed paths; reviewed the bounded 31-artifact scope above.
- Corrected one statically confirmed test-portability defect in [9b5b23ef](https://github.com/NeverSight/NeverD/commit/9b5b23efa35c577cdb22f6cd18eef7d73ac19e76). No production-code change was justified.
- Recorded 41 PR merges, two unmerged closures, 17 open issues and no open PR before today's publication.
- Confirmed current-head three-platform mobile fixture success and the still-incomplete main CI/real-application result.
- Distinguished earlier 97-outcome WHP success from the later expanded 197-outcome configuration failure.
- Preserved the complete October 1 tracker, including the September 30 history, below. Yesterday's #285 is merged; today's focused topic-branch update requires its own draft PR.
- No repository code, builds, tests, scripts, linters or formatters ran; no manual CI trigger, merge, deployment, dependency revision or security-setting change was performed. Commits use English messages with `[skip ci]`; automatically triggered GitHub checks can still occur independently.

## Tracking conventions and limits

- Source, issue and CI states are point-in-time observations, not continuous monitoring. New review PRs are excluded from pre-publication counts.
- Open issues returned 17 records and an empty second page; the separate open-PR query was empty. Updated issue/PR records returned 43 PRs and an empty second page, with no ordinary issues.
- The recent PR collection's first 100 updated records spans past the tracking boundary. All 43 in-window PRs are present; the older complete PR history was not enumerated.
- Exact-head workflows returned eight records and an empty second page; check runs returned 16 and an empty second page. Main/mobile job collections returned four/three records, each followed by an empty page.
- The separate manual-event collection through 01:10 UTC returned 29 existing runs and an empty second page. This review read it only; no run was created. Only the two native jobs discussed above were examined in log detail.
- Selected PRs #285, #308, #311, #313, #316 and #319 returned no submitted reviews, inline review threads or conversation comments. This is not an independent approval.
- GitHub reports dev unprotected and an empty repository ruleset collection. No protection/security setting was changed. The contribution guide's topic-branch/PR workflow was followed.
- PROGRESS.md was checked against yesterday's merged content and re-read before replacement. Preserve history and any human edits on later refreshes.
- Remote file/diff verification proves publication of the proposed correction, not passing validation or permission to merge.

## Previous snapshots (preserved)

<details>
<summary>2026-10-01 tracker, priorities, evidence and earlier history</summary>

# NeverD Daily Progress

Last verified: **2026-10-01 09:02 Asia/Shanghai (UTC+08:00)** / **2026-10-01 01:02 UTC**

This is a point-in-time daily issue/PR and static-review tracker. The [roadmap](docs/roadmap.md), [architecture](docs/architecture.md), [testing guide](docs/testing.md), and [contribution guidance](CONTRIBUTING.md) remain authoritative. Priorities are proposals, not assigned deadlines or a completion percentage.

## Current snapshot

Source snapshot is taken before opening the documentation-only daily-tracker draft PR.

| Measure | Verified state |
| --- | --- |
| Open issues | 17 |
| Open pull requests | 0 |
| PRs merged since 2026-09-30 03:51 UTC | 19: #220 and #267–284 |
| Issues closed in the same window | 0 |
| Observed dev commit | [6eb2e5c6](https://github.com/NeverSight/NeverD/commit/6eb2e5c6423f7b2977568fe070c7cd334f503572) |
| Previous observed dev commit | [4097148c](https://github.com/NeverSight/NeverD/commit/4097148c8de508f35bab1ad237ba0776c3a3dde6) |
| Change inventory | 160 commits; 410 changed file/submodule paths |
| New confirmed defects in today's sampled code | 0; no production-code change proposed |

The [comparison](https://github.com/NeverSight/NeverD/compare/4097148c8de508f35bab1ad237ba0776c3a3dde6...6eb2e5c6423f7b2977568fe070c7cd334f503572) was read across two commit pages (100 + 60). Its changed-file response stops at 300 paths, so the 410-path inventory instead comes from comparing the two complete recursive Git trees; neither tree was truncated. Commit enumeration and path inventory are not claims that every change was code-reviewed.

### Changes since the previous snapshot

- [#220](https://github.com/NeverSight/NeverD/pull/220) is now merged. Its former draft state and failed historical head are no longer current open-PR blockers.
- All 18 subsequently created PRs, [#267](https://github.com/NeverSight/NeverD/pull/267) through [#284](https://github.com/NeverSight/NeverD/pull/284), are also merged.
- [#283](https://github.com/NeverSight/NeverD/pull/283) consolidated failure-atomic ARM64 native integer-state capture.
- [#284](https://github.com/NeverSight/NeverD/pull/284) corrected MMIO test callback ownership across standard-library implementations.
- Recent work also includes KVM thread/state reuse, native execution profiles, loop refinement, mobile recovery and documentation fixes. Those broader features were inventoried, not comprehensively audited today.

## Static review

**Mode:** Static source and diff inspection only. No repository program, build, test, linter, formatter or script was executed. Existing GitHub Actions evidence was read without dispatching or rerunning workflows.

**Primary change:** [4f189d5e](https://github.com/NeverSight/NeverD/commit/4f189d5e630d207f6e4bfac803ff6c679a3c0973), inspected in the current dev tree, with related callers and rollback code. **Secondary change:** [bfb9a527](https://github.com/NeverSight/NeverD/commit/bfb9a527b0bbd154b81fbec92a489a664848ae0c), the MMIO fixture-ownership fix.

### Source coverage

The bounded review covered these 18 code, test and build artifacts. Relevant sections, rather than entire files, are identified explicitly.

- [AArch64GeneralState.cpp](lib/emulation/arch/aarch64/AArch64GeneralState.cpp), [.h](lib/emulation/arch/aarch64/AArch64GeneralState.h), and [.def](lib/emulation/arch/aarch64/AArch64GeneralState.def)
- [AArch64Machine.h](lib/emulation/arch/aarch64/AArch64Machine.h) and [AArch64Machine.def](lib/emulation/arch/aarch64/AArch64Machine.def)
- [Registers.h](include/neverd/emulation/Registers.h) and ARM64 inventory/ordering in [Registers.def](include/neverd/emulation/Registers.def)
- [KvmAArch64Machine.cpp](lib/emulation/backends/kvm/KvmAArch64Machine.cpp) and [WhpAArch64Machine.cpp](lib/emulation/backends/whp/WhpAArch64Machine.cpp)
- [CheckedAArch64Backend.cpp](lib/emulation/arch/aarch64/CheckedAArch64Backend.cpp)
- [RAMTransaction.h](lib/emulation/core/RAMTransaction.h) and [RAMTransaction.cpp](lib/emulation/core/RAMTransaction.cpp)
- [AArch64GeneralStateTests.cpp](unittests/emulation/AArch64GeneralStateTests.cpp) and [AArch64GeneralStateCases.def](unittests/emulation/AArch64GeneralStateCases.def)
- [lib/emulation/CMakeLists.txt](lib/emulation/CMakeLists.txt) and the ARM64 test-target stanza in [unittests/emulation/CMakeLists.txt](unittests/emulation/CMakeLists.txt)
- The `SharedDeviceRetirementReleasesCallbacksBeforeCPUsResume` case in [MemoryLifecycleTests.cpp](unittests/emulation/MemoryLifecycleTests.cpp) and `ExactUnmapRetiresCallbacksAndReturnsBudget` in [UnicornMMIOTests.cpp](unittests/emulation/UnicornMMIOTests.cpp)

Repository guidance, relevant architecture/testing sections, and workflow trigger/concurrency definitions were also inspected.

### Findings and evidence

No new correctness defect was established strongly enough to justify an automatic code fix in this scope.

- The ARM64 helper captures X0–X30, SP, PC, NZCV and TPIDR_EL0 into a copy, returns before publishing on any failed read, masks NZCV, then assigns the complete state. The register ordering and both native adapters agree with the 35-entry inventory.
- KVM raw reads and WHP captured-value indexes feed that same helper. The checked caller uses a separate next CPU state and publishes it only after the RAM transaction commits. The transaction destructor restores original RAM when native execution exits with an error before staging.
- The portable test source covers every register-read failure position, missing readers, retry, NZCV masking, both privilege modes, and preservation of vectors and untransferred registers. This describes test coverage, not a test result from this review.
- The MMIO regression fixtures now destroy caller-owned callback objects before testing mapping retirement. Their weak-reference assertions therefore do not depend on a moved-from callback container releasing its captures.

**Limitations:** The remaining changed paths, complete Objective-C/Swift pipeline, x64 state-reuse implementation and full issue backlog were not audited at source level. Static inspection does not establish native ARM64 KVM/WHP runtime behavior, cancellation interleavings, build/link success, formatting compliance or release readiness. Native ARM64 evidence remains explicitly outstanding in the project's documentation.

## Existing CI evidence

**CI snapshot:** 2026-10-01 01:01 UTC. Every row below is associated with observed dev head `6eb2e5c6423f7b2977568fe070c7cd334f503572`.

| Workflow | Observed state | Evidence |
| --- | --- | --- |
| CI | In progress; Linux, macOS and Windows jobs running | [36795886427](https://github.com/NeverSight/NeverD/actions/runs/36795886427) |
| Mobile Decompilation | Workflow API queued; Windows job succeeded, Ubuntu running, macOS queued | [36795886438](https://github.com/NeverSight/NeverD/actions/runs/36795886438) |
| EVM Upstream Audit | Queued | [36795886326](https://github.com/NeverSight/NeverD/actions/runs/36795886326) |
| Push on dev | Queued | [36795885475](https://github.com/NeverSight/NeverD/actions/runs/36795885475) |
| LLVM Style | Success | [36795886420](https://github.com/NeverSight/NeverD/actions/runs/36795886420) |
| Code Quality: Push on dev | Success | [36795885522](https://github.com/NeverSight/NeverD/actions/runs/36795885522) |
| Prebuilt LLVM Audit | Success | [36795886538](https://github.com/NeverSight/NeverD/actions/runs/36795886538) |
| Mobile Real Applications | Skipped | [36795891920](https://github.com/NeverSight/NeverD/actions/runs/36795891920) |

Exact-head check runs: **24 total: 6 successful, 9 skipped, 4 in progress, 5 queued, 0 failed**. Workflow and job/check states are reported separately because their APIs can show different aggregate states. Legacy commit statuses are empty; the combined status's `pending` value alone is neither a failure nor a full pass.

The complete dev Actions collection created since 2026-09-30 03:51 UTC contained **503 runs**, including **62 main CI runs: 61 cancelled and the current run in progress**. There was no completed green main CI run in that window. Cancellation is not a code failure, but leaves a substantial integration-evidence gap.

Mobile Decompilation in the same window: 54 cancelled, 5 failed, 2 successful and 1 queued. The latest older success was [36769822465](https://github.com/NeverSight/NeverD/actions/runs/36769822465), completed at 2026-09-30 20:19:17 UTC on `ac550f17d77d7e1f5e76fdf7b3509e8a7cea801d`. It does not establish current-head acceptance. Historical Objective-C x86_64 fixture failures and the old #220 failures must not be relabeled as failures of today's head.

## Today's top priorities

### 1. Establish an uninterrupted exact-head integration result

**Status:** Current main CI is still running; all other main CI runs in the tracking window were cancelled.

**Next action:** Inspect the existing run's final Linux/macOS/Windows results and preserve its commit identity. If subsequent development supersedes it, explicitly record that the replacement still needs full integration evidence. No workflow dispatch or rerun is part of this static-only review.

**Acceptance:** All three platform jobs reach terminal states for one identified current integration commit; failures and skipped suites are itemized. Pending or cancelled work is never counted as passing.

### 2. Reconcile mobile acceptance against the merged current tree

**Status:** #220 merged at 2026-09-30 04:39:17 UTC as [2bbd8019](https://github.com/NeverSight/NeverD/commit/2bbd80190a72d1749f5b64c3c19cf4eddec41813). Current Mobile Decompilation is not complete; Mobile Real Applications is skipped.

**Next action:** Use exact-head workflow evidence for Objective-C scalar/calls, Blocks and single-session qualification. Compare any remaining failures with the current source; do not reopen historical fixes merely because the previous tracker recorded a failed PR head. The PR's WMF recovery metrics remain author-reported evidence.

**Acceptance:** Required mobile fixture cases have explicit current-commit results, and real-application qualification has a recorded producer/consumer identity and actual result. A skipped consumer or an older green run does not satisfy this gate.

### 3. Reconcile native execution acceptance and issue planning

**Status:** Portable ARM64 capture tests are present, but native ARM64 KVM/WHP execution evidence is still outstanding. The 17 open issues have unchanged planning metadata; 14 are epics and only [#12](https://github.com/NeverSight/NeverD/issues/12) is assigned.

**Next action:** Map delivered work to [#104](https://github.com/NeverSight/NeverD/issues/104) and related acceptance criteria; separate portable static/test coverage from native transport evidence. Retain [#4](https://github.com/NeverSight/NeverD/issues/4) release readiness and #12's remaining Windows report as open until verified evidence resolves their specific criteria.

**Acceptance:** Each chosen criterion has an implementation/test link or an explicit gap, native transport claims name the actual platform and result, and the next bounded task has an owner and verification requirement. No issue is closed merely because a related implementation PR merged.

## Daily log

### 2026-10-01 — Static review and integration-evidence refresh

- Inventoried 160 commits and 410 changed paths since the previous observed dev commit; reviewed the bounded 18-artifact ARM64 capture/rollback and MMIO test scope above.
- Found no new statically proven defect in the sampled scope; no production-code fix is included.
- Reconciled #220 and #267–284 as 19 merges; 17 issues remain open and no PR was open at the source snapshot.
- Recorded current-head CI separately from historical failures and successes, including the 61 cancelled main CI runs and outstanding current integration result.
- Preserved the complete prior tracker text below. Followed the contribution guide's topic-branch/draft-PR workflow for this documentation-only update; no merge was performed.
- No builds, tests, repository scripts, linters, formatters, manual CI triggers, dependency revisions or security settings were changed. The documentation commit uses `[skip ci]`.

## Tracking conventions and limits

- Source data and CI are point-in-time observations, not continuous monitoring.
- Issues and PRs were paginated separately: 17 open issues and zero open PRs, followed by empty pages. The changed issue/PR collection returned 82 PR records and an empty second page; no ordinary issue updated in the window.
- Exact-head workflows returned 8 records and an empty second page; check runs returned 24 records and an empty second page; legacy statuses were empty. Main CI job pagination returned 3 jobs and then an empty page.
- The dev-window Actions collection was read across six pages (100 + 100 + 100 + 100 + 100 + 3). Every count above is bounded by that query and snapshot.
- The branch metadata reports `dev` unprotected and the repository ruleset collection is empty. The connector does not support the branch-rules endpoint; no protection setting was changed or bypassed.
- Newly created tracking PRs are excluded from the pre-publication source snapshot above. This update is proposed for `dev`; its presence in a draft branch does not mean it has been merged.
- Preserve history and human edits on subsequent refreshes; avoid treating an unchanged historical snapshot as current status.

## Previous snapshot (preserved)

<details>
<summary>2026-09-30 snapshot, priorities, evidence and initial daily log</summary>

# NeverD Daily Progress

Last verified: **2026-09-30 11:51 Asia/Shanghai (UTC+08:00)** / **2026-09-30 03:51 UTC**

This is the daily issue/PR execution tracker. The [roadmap](docs/roadmap.md)
remains the long-term product plan; [architecture](docs/architecture.md) and
[contribution guidance](CONTRIBUTING.md) remain authoritative for implementation.
Priorities below are proposed from current blockers and dependencies, not assigned
deadlines or an overall completion percentage.

## Current snapshot

| Measure | Verified state |
| --- | --- |
| Open issues | 17 |
| Open pull requests | 1: [#220](https://github.com/NeverSight/NeverD/pull/220), draft |
| PRs merged since 2026-09-29 00:00 UTC | 48 |
| Issues closed in the same window | 0 |
| Issue planning metadata | No milestones or explicit priority labels on the 17 open issues; 14 are labeled epic |
| Issue ownership | Only [#12](https://github.com/NeverSight/NeverD/issues/12) has an assignee: NeverSightAI |
| Observed dev commit | [4097148c](https://github.com/NeverSight/NeverD/commit/4097148c8de508f35bab1ad237ba0776c3a3dde6) |

Counts come from GitHub issue search with explicit issue/PR and state filters.
Each query returned `incomplete_results=false`, with all results within the
100-item page. The activity window is not a rolling 24-hour window.

### Active PR: #220

[Recover WMF Objective-C source with verified callbacks and Swift storage](https://github.com/NeverSight/NeverD/pull/220)

- Head: `c4d57f9ea95ad43da5382a9e03e4006d83416a30`; base branch: `dev`
- Open, draft, not merged; GitHub reports mergeable, which does not establish
  test readiness or permission to merge
- No submitted reviews, inline review threads, or requested reviewers were
  returned at this snapshot
- **Author-reported validation:** WMF recovery 4029/5046 methods, 47 gains and
  zero losses versus the PR base; latest focused suites report 692 Objective-C
  source tests and 19 metadata JSON tests passing
- Those reported results have not been independently rerun for this tracker

Verified Actions results associated with that PR head:

| Workflow | Result | Evidence |
| --- | --- | --- |
| CI | Failure | [Run 36662051074](https://github.com/NeverSight/NeverD/actions/runs/36662051074) |
| LLVM Style | Failure | [Run 36662051100](https://github.com/NeverSight/NeverD/actions/runs/36662051100) |
| Mobile Decompilation | Failure | [Run 36662051058](https://github.com/NeverSight/NeverD/actions/runs/36662051058) |
| Prebuilt LLVM Audit | Success | [Run 36662051118](https://github.com/NeverSight/NeverD/actions/runs/36662051118) |
| EVM Upstream Audit | Success | [Run 36662051077](https://github.com/NeverSight/NeverD/actions/runs/36662051077) |

### Default-branch status is different

At observed dev commit `4097148c`, LLVM Style and Python Plugin SDK succeeded,
but Mobile Decompilation failed. Main CI, Push on dev, and Mobile Real
Applications were still in progress. Do not transfer success or failure from
one commit to another.

- [dev LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/36663627355):
  success
- [dev Python Plugin SDK](https://github.com/NeverSight/NeverD/actions/runs/36663627301):
  success
- [dev Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/36663627368):
  failure; its failure cause has not been compared with the PR run
- [dev CI](https://github.com/NeverSight/NeverD/actions/runs/36663627393):
  in progress
- [dev Mobile Real Applications](https://github.com/NeverSight/NeverD/actions/runs/36664902687):
  in progress

## Today's top priorities

### 1. Reconcile PR #220 with current dev and clear integration gates

**Status:** Blocked by failed checks on the observed PR head.

The three main CI platform jobs failed in the step named
"Verify Debug and Release target flags". The inspected Linux log identifies
the actual terminating failure as the Python ABI inventory missing
`neverd_devirtualize_source_v3` and
`neverd_devirtualize_machine_source_v3`; this is not evidence that Debug
compiler flags themselves are wrong. [Linux job](https://github.com/NeverSight/NeverD/actions/runs/36662051074/job/109718732885)

LLVM Style reports clang-format 22.1.2 violations. Its
[proposed-formatting artifact](https://github.com/NeverSight/NeverD/actions/runs/36662051100/artifacts/11074612923)
is available. A later dev commit already addressed several dev formatting
violations, and dev Python Plugin SDK is green, so compare against current dev
before duplicating fixes or attributing them to #220.

**Next action:** Determine which failures remain on an updated PR comparison;
address only the remaining ABI/formatting discrepancies.

**Acceptance:**
- Current PR head and corresponding workflow runs are recorded
- Python ABI inventory and LLVM Style pass on that exact head
- All three main CI platform jobs complete with explicit pass/skip evidence

### 2. Close the Objective-C cross-architecture acceptance gaps

**Status:** PR #220 Mobile Decompilation is failing.

The [macOS job](https://github.com/NeverSight/NeverD/actions/runs/36662051058/job/109718733004)
reports:
- Scalar fixture, x86_64-default: 20/22 methods recovered
- Calls fixture, x86_64-classic: 18/21 methods recovered
- Single-session qualification: 11/12
- Swift source acceptance passed in that job

**Next action:** Compare the failing fixture identities and diagnostics with
current dev, resolve remaining source/metadata gaps, and preserve strict
fail-closed behavior. Do not treat the larger WMF author-reported metric as
proof these independent acceptance gates pass.

**Dependencies:** Current-head integration evidence from priority 1 and access
to the supported macOS/x86_64 acceptance environment.

**Acceptance:**
- Requested scalar and calls fixture matrices meet their complete-recovery gates
- Single-session qualification completes all 12 cases
- Mobile workflow succeeds on the exact PR head, or a documented unsupported
  case is explicitly reviewed rather than counted as a pass
- Review readiness is assessed only after the draft's remaining scope is clear

### 3. Reconcile epics with delivered work and select the next bounded task

**Status:** Planning metadata needs evidence reconciliation.

[Apple analysis #101](https://github.com/NeverSight/NeverD/issues/101) and
[IR emulator #104](https://github.com/NeverSight/NeverD/issues/104) remain open,
while related implementation PRs have landed. Unchecked historical criteria
are not sufficient evidence that every feature is still absent.

**Next action:** Map each acceptance criterion to merged changes, current
documentation, and tests; distinguish completed, partial, and still-blocked
scope before changing issue status.

**Release candidate:** [#4 release pipeline](https://github.com/NeverSight/NeverD/issues/4)
depends on or complements [#1 CI](https://github.com/NeverSight/NeverD/issues/1)
and [#3 prebuilt LLVM](https://github.com/NeverSight/NeverD/issues/3), both closed
as completed. Closure alone does not establish current three-platform packaging
readiness; verify artifacts and build consumers before planning a dry run.

**Small-task candidate:** [#12's last Windows feedback](https://github.com/NeverSight/NeverD/issues/12#issuecomment-5295808194)
reports executable/library PDB filename collisions after an earlier Debug flag
fix. Verify whether that specific issue remains before implementing another fix.

**Acceptance:**
- Each selected epic criterion has a linked implementation/test or an explicit gap
- The next task has a bounded deliverable, dependency, and validation command
- No issue is marked complete solely because a related PR merged

## Recent delivered changes

These PRs were returned by the merged-PR query. Validation descriptions inside
them are author reports unless separately confirmed by linked workflow results.

- [#266](https://github.com/NeverSight/NeverD/pull/266): acknowledge cancellation
  of active KVM vCPU entries
- [#265](https://github.com/NeverSight/NeverD/pull/265): prevent sibling loops
  from starving reachable entry-prefix witnesses
- [#264](https://github.com/NeverSight/NeverD/pull/264): execute static PIE from
  original ELF bytes with explicit load bias
- [#263](https://github.com/NeverSight/NeverD/pull/263): expose interpreter
  recovery field and query budgets
- [#262](https://github.com/NeverSight/NeverD/pull/262): support compiler-generated
  static TLS across checked CPU backends

## Daily log

### 2026-09-30 — Initial baseline

- Recorded 17 open issues, one draft PR, and 48 merged PRs since September 29
  00:00 UTC; no issues closed in that window
- Identified three failed workflows on #220's observed head and recorded
  concrete ABI, formatting, and Objective-C acceptance evidence
- Distinguished later dev results: formatting and Python SDK are green;
  Mobile Decompilation is failing and other validation is still running
- Proposed three priorities with acceptance criteria; no code changes,
  workflow reruns, issue edits, or merge actions are part of this entry

## Tracking conventions and limits

- Refresh the snapshot and priorities, then append a dated daily-log entry;
  preserve prior entries and human edits
- Record exact commits and workflow URLs; pending, skipped, unavailable, and
  failed checks must remain distinct
- Compare changes against the previous recorded snapshot. Keep issue closure,
  merged implementation, and verified product acceptance separate
- PR workflow lookup covered the returned first page of PR-triggered runs.
  The separate dev Actions query returned nine runs for its exact SHA.
  This does not establish branch-protection requirements, every external check,
  or overall release readiness
- GitHub search and Actions may change after this timestamp. This document is a
  point-in-time record, not a claim of continuous monitoring or a committed
  delivery schedule

</details>

</details>

</details>

</details>

</details>

</details>

</details>

</details>

</details>

</details>

</details>

</details>

</details>

</details>


</details>


</details>

</details>
