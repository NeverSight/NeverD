**Languages**: [English](ios.md) | [简体中文](zh-CN/ios.md) | [繁體中文](zh-TW/ios.md) | [日本語](ja/ios.md) | [한국어](ko/ios.md) | [Français](fr/ios.md) | [Deutsch](de/ios.md) | [Español](es/ios.md) | [Italiano](it/ios.md) | [Русский](ru/ios.md) | [العربية](ar/ios.md)

# iOS native and source recovery

[← Documentation index](README.md) · [Mobile overview](mobile.md)

`neverd mobile` accepts IPA, `.app`, and Mach-O inputs. It exports native C, runtime metadata, and experimental Objective-C `.m` and Swift `.swift` source for supported native bodies. A published result can contain unrecovered methods; inspect the coverage report before using the source. Mobile containers are a CLI workflow; the native C SDK accepts the selected Mach-O separately.

Compilation removes comments, formatting, identifiers, and source constructs. This workflow reconstructs source representations; it cannot restore the original text or certify equivalent behavior for an arbitrary application. It does not launch the analyzed application.

## Start and dependencies

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

Build the `neverd` target with C++20 support. The mobile workflow runs in the native CLI without a Python interpreter. Swift signature demangling comes from `LLVMSwiftDemangle` in the NeverD LLVM fork; both source builds and matching published LLVM packages include this component. NeverD does not fetch a separate Swift source dependency. Building and running NeverD require no installed Swift compiler or toolchain. LLVM, Capstone, and other native library dependencies still apply; distribute the libraries and license notices required by your build. Independently compiling generated Apple-language source and running the Swift behavior tests on macOS require Apple Clang, the SDK, and `swiftc` as appropriate.

Swift signature recovery consumes structured nodes from `LLVMSwiftDemangle` directly inside the C++ process. It does not locate or start an external demangling executable or toolchain discovery command. The former executable-path option is removed, and the former demangler environment variable is not read. `--metadata-only` invokes neither the native source exporter nor signature demangling.

The signature inventory in `metadata/swift-signatures.json` records the built-in component as:

```json
{
  "demangler": {
    "name": "llvm-swift-demangle",
    "execution": "builtin",
    "version": "6.3.3"
  }
}
```

```sh
neverd mobile App.ipa -o recovered-swift --timeout=600 --json
```

## Inputs and selection

IPA must contain exactly one top-level `Payload/*.app`. `.app` and IPA main executables are selected through `Info.plist`'s `CFBundleExecutable`. `--artifact` is relative to that application bundle in both formats; it selects one embedded executable, without recursively analyzing every framework or extension. Raw Mach-O input does not accept `--artifact`.

For fat binaries, `--arch=auto` prefers arm64, arm, x86_64, then i386. Missing or unsupported slices fail explicitly. Source-language projection currently targets arm64 and x86_64; selecting another family does not imply Objective-C/Swift source support. A selected slice with `cryptid != 0` is rejected. Provide an already decrypted, readable analysis input. Archives and directory inputs reject unsafe paths, symbolic links, special files, and conflicting entries.

## Options and resource limits

| Option | Default | Meaning |
|--------|---------|---------|
| `-o DIRECTORY` | Required | New output directory outside a directory input; existing output is preserved |
| `--platform=auto\|ios` | `auto` | Infer the input platform or select iOS |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | Select one Mach-O slice |
| `--artifact PATH` | Main executable | Application-relative executable path |
| `--metadata-only` | Off | Read metadata without source recovery or tool invocation |
| `--max-func N` | `0` | Native function limit; zero means all discovered functions; ignored in metadata-only mode |
| `--timeout N` | `300` | Positive total analysis budget in seconds; child processes use the remaining budget |
| `--max-files N` | `20000` | Positive entry budget; Swift symbol inventory is also bounded |
| `--max-bytes N` | `2147483648` | Positive input, extracted-data, and final-output byte budget |
| `--json` | Off | Print the versioned report as JSON |

The work area is monitored with up to three times the configured entry/byte budgets for staged input and intermediate output. Logs are limited to 16 MiB per process; Swift signature JSON is additionally limited to 32 MiB. Limits are resource controls, not process isolation. Increasing a timeout does not disable byte or entry limits. Methods excluded by `--max-func` remain unrecovered when present in the metadata inventory.

## Objective-C source and runtime structure

The native loader binds a runtime method record, executable IMP address, and supported type encoding to explicit source ABI locations. Fixed scalar/pointer bindings include hidden `self`/`_cmd`, unused arguments, separate integer/floating register banks, and supported stack positions. Float/double bit reinterpretation is distinct from numeric conversion. Type hints are source-projection inputs, not authenticated ABI evidence or permission to patch executable code.

`sources/objc.m` places actual recovered statements in `@implementation` bodies and preserves required C helpers and typed calls. Eligible call targets must have a supported source binding; unknown targets and incomplete dependency groups remain unrecovered. Missing definitions, invalid executable addresses, conflicting encodings, unsupported ABI mappings, incomplete decoding, and rejected IR cannot become recovered methods merely because a declaration is available.

Class metadata retains superclass identity, instance start/size, and scalar/pointer ivars with checked offsets, widths, and alignments. Declarations use padding where necessary. A method requiring unavailable instance layout remains unrecovered. Categories retain separate class/category/address identities and separate implementations; identical entries repeated in class/category inventories are counted once. External categories use an existing Foundation class declaration where supported. Unknown external class headers are reported as missing dependencies; no substitute class layout is invented.

Recovery also requires complete class declarations, definitions for local ancestor classes, and emitted source in which values are defined on every path before use and reachable exits have the required return; an empty local superclass receives an empty `@implementation` only when verified layout and a complete method inventory prove that it has no ordinary methods of its own. If this evidence is missing, or a name conflicts with a known Foundation import, affected methods remain in the coverage denominator with reasons while independent recoverable classes continue to be emitted; these name checks do not cover every SDK name, iOS SDK, or version.

Supported Objective-C Block calls use a complete fixed scalar invocation ABI, including the hidden Block object and every argument and return carrier. The runtime encoding `@?` is widened to `id` only in declarations; it does not supply an invocation prototype. Global Block references preserve shared object identity. Supported synchronous scalar captures require proven native capture storage and invocation flow. Escaping or asynchronous captures, unmodeled object/byref ownership, copy/dispose helpers, and unknown layouts remain unrecovered.

This is a limited reconstruction of runtime information. Complete properties, protocols, original ownership annotations, arbitrary aggregates, variadic tails, exception-dependent bodies, and unmodelled Block/capture layouts are not promised. Runtime encoding describes fixed arguments and cannot prove that the original declaration had no variadic tail. Chained pointers are used only where the native loader resolved the relevant slots; unresolved formats retain diagnostics.

## Swift source and storage

Structured demangler output classifies callable signatures separately from non-callable metadata. Supported signatures are bound to the selected binary's symbols, entries, and explicit machine ABI before native body projection. Swift receivers use Swift ABI rules; Objective-C hidden arguments are not substituted. A user-supplied signature file is a hint that still requires validation.

The experimental emitter can construct supported free functions, class methods, designated initializers, and fixed-layout struct methods, including supported mutating receiver forms. Class/struct declarations and stored fields require recovered layout metadata. Native calls are emitted only when the required source declarations and bodies form a complete supported dependency group. Recovered source units contain those declarations and methods together, rather than a bridge that calls the original binary.

Supported Swift getter and setter bodies come from the native implementation and are assembled into properties. Private backing storage preserves the established field layout; initializers and other methods use the same storage names. A property declaration or field record alone cannot establish a recovered accessor body.

Supported allocating constructors, trivial destructors/deallocators, type metadata accessors, and `_modify`/resume entries can project into an emitted type unit. Each requires a bounded proof of the complete native flow and effects, actual recovered context/initializer/property dependencies, and exception-handling and IR audits of the related bodies. Allocator writes must match the actual initializing constructor; `_modify` must bind the exact mutable field and continuation. Runtime metadata calls retain their modeled semantics within the recovered type. These entries are explicitly reported as compiler source projections; they do not constitute separately recovered ordinary method bodies or original source text.

Generic or resilient layouts, async/throwing functions, unknown calling conventions, unsupported accessors/allocators/thunks, incomplete initialization, and unbound native or runtime dependencies remain individually `unrecovered`. A mangled symbol or nominal type name alone is not a recovered method. Stripped symbols and unclassified demangler nodes make coverage incomplete or unknown.

## Output and coverage

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

Source-language files exist only when source can be emitted. `objc.json` contains classes, categories, ivars, and raw method encodings; `objc.h` contains supported declarations. `swift.json` contains nominal-type metadata and mangled symbols. Signature and method JSON files preserve classification, omissions, reasons, and counts. Logs contain native diagnostics and native Swift export diagnostics when that export runs. There are no external Swift toolchain-discovery or demangling logs. Output paths in `report.json` are relative to its directory. The selected binary is an analysis artifact; generated source does not link it as a recovery bridge.

Temporary package copies and intermediate backend JSON are removed. A normal run with no native function bodies fails even when metadata exists. Metadata-only output contains the selected artifact, `objc.h`, `objc.json`, `swift.json`, and `report.json`; source directories and method-coverage/signature files are absent, and `native_function_count`, `objc_method_recovery`, and `swift_method_recovery` are `null`. All modes use the native loader’s resolved Objective-C metadata. Swift metadata uses bounded native-image reads; unsupported fixups, relocatable layouts, or references retain partial diagnostics.

A shortened illustrative report deliberately shows partial recovery:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

Outer `status: "success"` means validated output was published. Method coverage `recovered`, `partial`, `unrecovered`, or `no-methods` describes the discovered inventory, not semantic equivalence or original-program completeness. Every unrecovered method has a reason. Objective-C `recovered` additionally requires complete runtime metadata. An empty inventory cannot prove there were no methods.

An unrecovered Objective-C row may include `native_backend: {status, reason, diagnostics}` when a unique backend result exactly matches its runtime identity. This bounded optional summary preserves the backend's intermediate result even when a declaration or layout check fails. The row's primary status, reason and recovery counts remain authoritative; an absent summary means the evidence was unavailable.

Swift `coverage_status` counts classified callables only. Overall Swift `status` also accounts for unknown symbols and can be `unclassified`, `unsupported-architecture`, or `no-symbols`. Non-callable metadata is listed under `non_method_symbols` with `not-callable`; unknown symbols use `unclassified`. `types`, `type_metadata_count`, and `source_type_count` count type metadata/emitted type units independently and must not inflate method counts.

Each recovered Swift row reports `source_representation` as `native-method-body` or `compiler-generated-from-type`. Compiler projections also retain `compiler_projection_kind` and `compiler_projection_evidence`. `source_body_method_count` counts recovered native method bodies; `compiler_projection_method_count` counts proven compiler projections. Their sum equals `recovered_method_count`. Compiler entries remain in the `method_count` denominator and retain their exact identity in one corresponding `type` source unit. Neither type metadata nor a dependency name alone increases recovered coverage. Native batch JSON includes `source` on compiler rows and type units; mobile `source_units` retain descriptions without `source`, and the complete source is in `sources/swift.swift`.

Native Swift batch `source_units` records `{kind, module, name, source, method_entries, method_identities}`; kind is `function` or `type`. Each identity is `{entry, mangled_symbol}`. Different symbols may share one entry and retain distinct ABI projections; every recovered identity must appear exactly once and no unrecovered identity may appear. `method_entries` must equal the ordered entry projection of `method_identities`, including repeated addresses. Repeated identical identities cannot be silently merged. Batch `source` equals the ordered concatenation of each unit’s source plus a newline. Mobile retains aggregate source in `sources/swift.swift` and unit descriptions in coverage JSON. Standalone method `source` is for inspection; concatenating those rows does not reconstruct class declarations correctly.

## Direct native exports and SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

Swift export consumes the structured signature inventory produced by the built-in signature parser during a normal mobile run. Objective-C batch JSON includes `native_source`, `native_function_count`, `objc_metadata`, and per-method C source, function name, return type, and parameters. Mobile performs further declaration/body/layout checks before generating `.m`, so its final method coverage can be narrower than batch C coverage. A successful native export can contain no recovered methods.

For an already loaded Mach-O session, `neverd_objc_methods_json(session, max_functions)` and `neverd_swift_methods_json(session, signatures_json, max_functions)` return the corresponding reports. Zero selects all discovered functions. Free successful strings with `neverd_free_string`; `NULL` indicates failure and the session error explains it. These APIs do not load IPA or `.app` containers.

## Verification and troubleshooting

On macOS, builds with `BUILD_TESTING` enabled provide `check-neverd-mobile-ios`, which runs all three native recovery suites through CTest.

Python is used only by the development test harnesses below; built-in mobile recovery runs in the native C++20 CLI.

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

On macOS, the Objective-C runners compile originals, recover `.m`, and link only generated source with an independent calling harness. The scalar runner covers integer boundaries, branches, loops, pointer reads/writes, hidden parameters, float/double identity bits, mixed parameters, and stack arguments. The calls runner adds message dispatch, inheritance, categories, ivar storage, native helpers, and Block invocation/captures/shared identity. The calls corpus requires 21/21 recovered methods and 134/134 independent expected results for each arm64/x86_64 × classic/default variant. Run these checks against the current native CLI build.

The strict Swift runner checks 22 user declarations, three getter/setter entries, and nine compiler-generated callable entries; none may disappear from the inventory. Each variant has 858 original-program oracle checks. It independently compiles generated `.swift` and its harness, without the original dylib, module, bridge, or handcrafted replacement declarations. Cases include scalar/native calls, class initialization/storage, struct value/mutating methods, floating and stack parameters, pointers, and loops. The native C++20 CLI must pass all four arm64/x86_64 × classic/default variants without skips: each must recover 25 native method bodies plus nine compiler projections, preserve all 34 callable identities, and match 858/858 oracle checks for both originals and independently compiled generated Swift. These results are limited to this corpus and do not guarantee recovery of arbitrary applications or original source text. The runner rejects missing coverage, source compilation failures, and behavior mismatches.

These four variants target macOS. The two additional compiler entries are the empty value initializer and its metadata accessor; native proofs validate them separately, and one `struct Empty {}` source unit retains both identities. Passing this corpus does not qualify a real iOS application.

All three scripts support `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N`, and `--work-dir NEW_DIRECTORY`. `--setup-only` validates originals and does not test recovery. Acceptance requires every requested architecture and fixup variant to complete, including the scalar runner; a missing variant or an architecture the host cannot execute fails the run. Skips are not permitted. Use retained failure artifacts to distinguish missing source coverage, compilation errors, and behavior mismatches. Check current test output before claiming verified support.

The [Mobile Real Applications workflow](../.github/workflows/mobile-real-apps.yml) uses public applications pinned in the [corpus manifest](../scripts/mobile_real_apps.json). Its gate requires independent inventories of complete iOS bundles and APK DEX sets, independent rebuilding of originals and generated source, and behavior comparisons. Missing stages, unknown inventory coverage, or missing required cases fail the gate. Real-app recompile and behavior stages remain incomplete, so support retains the Experimental label; passing harness guard tests does not establish real-app success.

Publication is transactional: choose a new output directory, inspect process exit status first, and keep redirected JSON outside that directory. Failures remove staged output and preserve existing output. Nonzero backend exits include a bounded log tail. Backend timeouts preserve the timeout message and append a bounded tail when captured log text is available; budget failures retain their own diagnostics. Successful native CLI runs return zero. Recovery failures return nonzero; `--json` reports handled failures with `schema_version`, `status: "error"`, and `error`. Argument parsing, native executable or library startup failures, and interruptions can instead report on stderr. Consumers must inspect the exit status first.

For an encrypted slice, supply readable input; for an absent architecture, inspect available slices; for omitted methods, read their exact reasons and metadata diagnostics. Increasing `--max-func` helps only functions excluded by the limit. Missing layouts, signatures, external headers, exception support, or unsupported ABI behavior require implementation or additional valid metadata, not a claim of complete recovery. Preserve applicable dependency license notices when distributing tools or generated packages.
