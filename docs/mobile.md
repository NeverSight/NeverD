# Mobile application recovery

[← Documentation index](README.md) · [Complete Android guide](android.md) · [Complete iOS guide](ios.md)

`neverd mobile` recovers readable Java from Android APK, DEX, and smali inputs. For iOS IPA, `.app`, and Mach-O inputs, it exports native C and reconstructs supported Objective-C method bodies into `.m` source, alongside experimental Swift source and runtime metadata. This is an experimental CLI workflow; mobile containers are not accepted by the native C SDK or GUI loader.

## Setup

Build the `neverd` target with C++20 support. The mobile workflow is compiled into the native CLI and does not use a Python interpreter. Distribute the executable with the native libraries required by your build.

Native ZIP handling uses zlib for CRC-32 and DEFLATE. CMake prefers an installed library via `find_package`; otherwise it downloads zlib 1.3.2 with a pinned SHA256 and builds it statically. This mobile ZIP implementation needs no Python helper tools on Windows. Preserve the dependency notices in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

The default engine is implemented in C++20 and needs no Python, Java, or JADX runtime. Only an explicit `--jadx PATH` selects the separately installed compatibility adapter; `NEVERD_JADX` and PATH do not select it automatically, and there is no automatic fallback. The optional adapter requires JADX 1.5.6+ with standard DEX/smali input plugins and Java 11+. Its report names the actual `jadx` engine and version. Installation and dependency licenses remain documented in the [Android guide](android.md#optional-jadx-compatibility-adapter).

## Android

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

All root `classes.dex`, `classes2.dex`, and subsequent numbered DEX files in an APK are analyzed together. A smali directory is searched recursively and all its classes are analyzed in one invocation, including nested and sibling classes. Use a directory when recovering classes that reference each other. A single smali file only supplies that class.

Built-in output contains `sources/`, `metadata/android-methods.json`, and `report.json`, with `backend: {"name": "neverd", "version": "1", "execution": "builtin"}`. The report embeds `android_method_recovery`: `method_count = recovered_method_count + declaration_only_method_count`, and `unrecovered_method_count` must be zero before publication. Original `native`/`abstract` declarations are counted separately from recovered bodies. The explicit external adapter retains its own backend logs. APK resources, manifests, native libraries, and dynamically loaded code are outside this Java path; native libraries can be analyzed separately with `neverd decompile`.

The built-in readers share an independently implemented typed Dalvik model and bounded Java emitter for representable ordinary DEX 035/037–040 and smali code. DEX 041, dynamic calls such as `invoke-custom`, some initialization paths, unknown operations, and identifiers unrepresentable in Java fail explicitly. Generated Java may use a dispatch loop; it does not execute the original DEX or call it through a runtime bridge. Original comments, formatting, and removed names cannot be restored. The experimental engine does not promise JADX feature parity, semantic equivalence, or complete recovery of arbitrary APKs.

## iOS

The [complete iOS guide](ios.md) documents IPA, `.app`, and Mach-O selection, setup, all CLI options, source schemas, coverage, and verification.

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

Each run selects one executable. `--artifact` is relative to the application bundle for both IPA and `.app`. Fat selection prefers arm64, arm, x86_64, then i386; source-language projection targets arm64/x86_64. Encrypted selected slices are rejected. The experimental native path emits C and supported Objective-C method bodies, including scalar/pointer, floating, mixed, and stack ABI bindings. Runtime class/ivar layouts and separate categories are retained when validated; unresolved layouts, signatures, calls, and other dependencies remain explicit omissions.

Swift recovery classifies signatures inside the C++ process using `LLVMSwiftDemangle` from the NeverD LLVM fork. It starts no external demangler or toolchain-discovery command and needs no installed Swift compiler to build or run NeverD. Fork source builds and matching LLVM packages include the component; NeverD does not fetch a separate Swift source dependency. Signature inventory records `demangler: {"name": "llvm-swift-demangle", "execution": "builtin", "version": "6.3.3"}`. Supported signatures are bound to native entries and ABI locations before actual `.swift` functions, class methods/initializers, and fixed-layout struct methods are emitted. Generic/resilient, async/throwing, unsupported runtime-generated callable forms, and incomplete source dependency groups remain unrecovered.

Normal output includes `sources/native.c`, optional `sources/objc.m` and `sources/swift.swift`, declarations and runtime metadata, method/signature coverage JSON, logs, `artifacts/selected.macho`, and `report.json`. There are no external Swift toolchain-discovery or demangling logs. Generated source does not call the original binary as a recovery bridge. Swift `source_units` groups type declarations and methods; standalone method rows must not be concatenated to rebuild classes. Outer `status: "success"` means output publication, not complete method coverage or semantic equivalence.

`--metadata-only` runs neither the native source exporter nor signature demangling and emits no source or method coverage. All modes use the native loader’s resolved Objective-C metadata. Swift metadata uses bounded native-image reads; unsupported fixups, relocatable layouts, or references retain partial diagnostics. `--max-func` limits native function recovery and is ignored in metadata-only mode. Missing native function bodies fail a normal run. Temporary unpacked inputs are removed.

Original comments, formatting, removed identifiers, and compilation-lost source structures cannot be reconstructed exactly. Read each method's recovery status and reason, the separate Swift callable/non-callable/unclassified counts, and the documented limitations before using the output.

## Limits and failures

`-o` must name a new directory outside any directory input. Existing output is never overwritten. Work is staged and published only after successful recovery and output validation. Successful native CLI runs return zero. Recovery failures return nonzero; `--json` reports handled failures with `schema_version`, `status: "error"`, and `error`. Argument parsing, native executable or library startup failures, and interruptions can instead report on stderr. Consumers must inspect the exit status first.

The defaults are 20,000 entries, 2 GiB of input/extracted or final output data, and 300 seconds for built-in Android/iOS analysis or each explicit JADX process. iOS child processes receive the remaining total analysis budget. The built-in reader and emitter also enforce a bounded work budget. Set `--max-files`, `--max-bytes`, and `--timeout` to adjust these positive limits. The temporary work area is monitored while backends run, with up to three times the entry/byte limits to allow staged input and intermediate output to coexist. Diagnostics are capped at 16 MiB per process.

APK staging writes only root `classes.dex`, `classes2.dex`, and subsequent numbered DEX files. Every ZIP member still undergoes header/range checks, decompression, length and CRC validation, and counts toward archive entry and uncompressed-byte limits. Unwritten resources may have distinct case-sensitive names such as `res/-A.xml` and `res/-a.xml`. Exact duplicate ZIP names and file/directory identity conflicts remain errors; portable filesystem case-collision checks apply to members actually written. Full extraction, including IPA input, still rejects such output collisions. Traversal paths, links, special files, and encrypted ZIP entries remain rejected throughout the archive. Directory inputs also reject symbolic links and special files.

These limits are robustness controls, not a sandbox for third-party backend code. Explicit JADX and native source-export commands run as local child processes. Failed staging output is removed. A backend's nonzero exit includes the bounded diagnostic tail; launch failures, timeouts and budget violations have their own error messages.

## Validation

Python is used only by the development test harnesses below; built-in mobile recovery runs in the native C++20 CLI.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

The component tests cover parsing, unsafe containers, backend failures, output cleanup, architecture selection, and output preservation. Tests that invoke the built CLI use `NEVERD_BUILD_DIR`. The internal Android runner uses a JDK (`java` and `javac`) and D8 to build independent DEX/APK fixtures, then compile and execute recovered Java. These are verification dependencies, not built-in recovery requirements. Run against the current build and inspect the result before treating a case as verified. The separate compatibility runner additionally requires JADX; fixture success does not prove arbitrary application recovery.

On macOS with Apple Clang, its SDK and a built NeverD, run the actual Objective-C execution comparison:

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

The runner builds its own Objective-C fixture, recovers its method implementations, and links only the recovered `.m` with the same independent calling harness. Its 22-method fixture compares 141 observable results covering integer boundaries, branches, loops, pointer reads/writes, hidden and unused arguments, float/double identity bits, mixed parameters, and stack positions. A current passing run is required before treating a case as verified. Every requested architecture and fixup variant must complete; the default covers arm64/x86_64 × classic/default. A missing variant or an architecture the host cannot execute is a failure, with no permitted skips. This fixture evidence does not establish completeness for arbitrary iOS programs. `NeverDMobileIOSBackend` is registered with CTest on macOS, including the main CI test profile.

The independent Swift recovery runner is `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd`. It recompiles generated Swift and its harness without linking the original binary; unsupported callables and behavior differences are failures. See the [iOS guide](ios.md) for coverage semantics and retained failure evidence.

The [Mobile Real Applications workflow](../.github/workflows/mobile-real-apps.yml) uses public applications pinned in the [corpus manifest](../scripts/mobile_real_apps.json). Its acceptance gate requires independent inventories of every DEX in each APK and every Mach-O in each complete iOS bundle, independent rebuilding of originals and generated source, and behavior comparisons. Missing stages, unknown inventory coverage, or missing required cases fail the gate. Real-app recompile and behavior stages remain incomplete, so support retains the Experimental label; passing harness guard tests does not establish real-app success.
