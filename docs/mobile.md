# Mobile application recovery

[← Documentation index](README.md) · [Complete Android guide](android.md)

`neverd mobile` recovers readable Java from Android APK, DEX, and smali inputs. For iOS IPA, `.app`, and Mach-O inputs, it exports native C and reconstructs supported Objective-C method bodies into `.m` source, alongside runtime metadata. This is an experimental CLI workflow; mobile containers are not accepted by the native C SDK or GUI loader.

## Setup

Build the `neverd` target normally. Keep the generated `mobile/` directory beside the executable when moving or distributing the build. Python 3.10 or newer must be on PATH; select another interpreter with `--python PATH` or `NEVERD_PYTHON`.

Android additionally requires a separately installed JADX 1.5.6 or newer, its standard DEX/smali input plugins, and Java 11 or newer. Set `--jadx PATH` or `NEVERD_JADX`, or put `jadx` on PATH. On Windows, supply the distribution's full `jadx.bat` path or its `lib/jadx-*-all.jar`; NeverD launches the JAR through Java without passing application paths through a command shell. `JAVA_HOME` selects Java for JAR launchers. No dependencies are downloaded automatically. Installation and dependency licenses are available in the [JADX distribution documentation](https://github.com/skylot/jadx#download).

## Android

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java --jadx /opt/jadx/bin/jadx
```

All root `classes.dex`, `classes2.dex`, and subsequent numbered DEX files in an APK are analyzed together. A smali directory is searched recursively and all its classes are analyzed in one invocation, including nested and sibling classes. Use a directory when recovering classes that reference each other. A single smali file only supplies that class.

The output contains `sources/`, backend logs under `logs/`, and `report.json` with the backend version, input code files, source paths, and recovery limitations. APK resources, manifests, native libraries, and dynamically loaded code are outside this Java recovery path. Native libraries can be extracted separately and analyzed with `neverd decompile`.

Original comments, formatting, and removed names are unavailable. Backend errors, explicit incomplete-code markers, and missing Java output fail the command. Successful output remains reconstructed source: it is not proof of semantic equivalence or a guarantee that every recovered method recompiles.

## iOS

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-ios --arch arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
neverd mobile App.ipa -o quick-analysis --max-func 20 --timeout 600
```

IPA input must contain one top-level `Payload/*.app`. The main executable is resolved through `Info.plist`'s `CFBundleExecutable`. Use `--artifact` to select another executable relative to that application bundle, such as an embedded framework. Each command analyzes one selected native artifact.

Fat binaries are selected deterministically: `auto` prefers arm64, then arm, x86_64, and i386. Use `--arch` to request a particular family. An absent or unsupported slice fails explicitly. A selected Mach-O slice with a nonzero encryption identifier is rejected; supply an already decrypted analysis input.

Normal recovery produces the following files:

| Path | Content |
|------|---------|
| `sources/native.c` | Native C analysis output for recovered functions |
| `sources/objc.m` | Actual reconstructed bodies of supported Objective-C methods; absent when none can be emitted |
| `metadata/objc.h`, `metadata/objc.json` | Declarations, classes, selectors, type encodings, implementation addresses and metadata limitations |
| `metadata/objc-methods.json` | Per-method body coverage, including omitted methods, reasons and native diagnostics |
| `metadata/swift.json` | Available nominal types and mangled Swift symbols |
| `artifacts/selected.macho` | Selected thin binary |
| `logs/native.log` | Native backend diagnostics |
| `report.json` | Aggregate recovery report and relative output paths |

Temporary package contents and the intermediate batch source report are removed. Normal recovery with no recovered native function bodies fails, even if metadata is available. `--metadata-only` retains the selected binary, the three metadata/declaration files and `report.json`; it does not produce `sources/`, `metadata/objc-methods.json` or `logs/native.log`. Its report sets `native_function_count` and `objc_method_recovery` to `null`.

The Python Objective-C metadata reader used by `--metadata-only` does not resolve chained pointers or relocatable-object pointers. Full native analysis instead uses the native loader's Objective-C metadata, including individually resolved chained pointer slots. Swift metadata uses the Python reader in both modes, so unsupported indirect type references remain partial. `--max-func N` limits native function recovery; zero selects all discovered functions. Methods excluded by a limit remain visible as unrecovered when their metadata is available. This option has no effect with `--metadata-only`.

### Objective-C method bodies

The native loader ties each readable method record to an executable implementation address. Supported type encodings supply fixed parameter positions, signedness, widths and return type to source projection. The hidden receiver and selector are retained even when unused, as are unused explicit parameters. `sources/objc.m` contains the recovered statements inside `@implementation` methods, with typed bindings and the C helpers required by those statements. It does not insert empty bodies to inflate coverage.

The current method-body path supports arm64 and x86_64, void returns, integer and ordinary pointer signatures, and fixed parameters that fit in integer argument registers (eight on arm64, six on x86_64, including the two hidden arguments). It handles valid absolute and small relative method records. Chained-pointer metadata is used only where the loader resolved the actual slot. Invalid pointers, conflicting declarations, unsupported encodings, incomplete decoding, failed IR verification, and degraded source output do not become recovered methods.

Floating-point or aggregate signatures, stack-passed parameters, variadic tails, blocks, exceptions, and methods needing unbound native or dynamic calls are not currently projected as complete Objective-C bodies. Runtime encodings describe fixed parameters and cannot prove the absence of an ellipsis. Property/protocol/category reconstruction and original instance-variable layouts are also unavailable. A method accessing instance storage may require additional layout reconstruction before rebuilding a class. Native C remains analysis output for those cases.

Inspect `report.json`'s `objc_method_recovery` or the identical `metadata/objc-methods.json` before consuming `.m` output. The fields `method_count`, `recovered_method_count`, `unrecovered_method_count`, and `methods` describe the discovered method inventory. Each unrecovered entry carries a reason; `diagnostics` preserves available native method diagnostics, including uncertainty about runtime type hints.

| Coverage status | Meaning |
|-----------------|---------|
| `recovered` | Every inventoried method has an emitted body and Objective-C metadata status is `recovered` |
| `partial` | At least one body is emitted, but other methods are omitted or metadata is incomplete |
| `unrecovered` | The inventory contains methods, but none has an emitted body |
| `no-methods` | The inventory is empty; this does not establish that the original program had no methods |

Overall `status: "success"` means output publication succeeded, including when method coverage is partial or no Objective-C bodies are available. Even `recovered` is source-generation coverage, not a semantic equivalence certificate.

For a raw, already readable Mach-O, the same batch export is also available independently of package handling:

```sh
neverd export executable --format=objc-methods -o method-sources.json
neverd export executable --format=objc-methods --max-func=20 -o limited-sources.json
```

The batch JSON contains `native_source`, `native_function_count`, `objc_metadata`, `methods`, `pointer_size`, and limitations. Each recovered method carries its exact generated C function name, source, return type and parameter list. Batch `recovered_method_count` counts native C projections; the mobile `.m` renderer can reject additional methods when validating identifiers, declarations and function bodies. Use the mobile coverage report for the final `.m` inventory. Batch `status: "success"` means the report was generated, including when no methods were recovered.

The native C API `neverd_objc_methods_json(session, max_functions)` returns this JSON for a loaded Mach-O; free its result with `neverd_free_string`. Zero selects all discovered functions. Failure for a valid session returns `NULL` with the session error set. This API does not load IPA or `.app` containers.

### Recovery limits

Swift source-level method bodies are not reconstructed; Swift symbols and nominal type names are metadata only. Stripping, optimization, unavailable metadata and unsupported fixups reduce recovery. Original comments, formatting, removed identifiers and source constructs lost during compilation cannot be restored exactly. Outside supported runtime signatures, native types and calling conventions remain approximate. Rebuilding arbitrary applications requires dependencies, instance layouts and runtime behavior that a method listing alone cannot supply.

## Limits and failures

`-o` must name a new directory outside any directory input. Existing output is never overwritten. Work is staged and published only after successful recovery and output validation. `--json` prints a versioned report for automation; handled recovery failures return nonzero and a JSON error. Startup failures, missing helpers/interpreters, Python older than 3.10, argument parsing errors and interruptions can instead report plain text on stderr.

The defaults are 20,000 entries, 2 GiB of input/extracted or final output data, and 300 seconds per backend process. Set `--max-files`, `--max-bytes`, and `--timeout` to adjust these positive limits. The temporary work area is monitored while backends run, with up to three times the entry/byte limits to allow staged input and intermediate output to coexist. Diagnostics are capped at 16 MiB per process. Archives with traversal paths, links, special files, case collisions, or encrypted ZIP entries are rejected. Directory inputs also reject symbolic links and special files.

These limits are robustness controls, not a sandbox for third-party backend code. Backend dependencies run as local processes. Failed staging output is removed. A backend's nonzero exit includes the bounded diagnostic tail; launch failures, timeouts and budget violations have their own error messages.

## Validation

```sh
cmake --build build --target check-neverd-mobile
python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

The component tests cover unsafe containers, malformed and encrypted native inputs, backend failures and incomplete Java, architecture selection, and output preservation. Tests that invoke the built CLI use `NEVERD_BUILD_DIR`. The real Android smoke runner additionally needs the supported Java backend and JDK; it recovers synthetic smali/DEX/multidex APK inputs and compiles/runs the recovered Java.

On macOS with Apple Clang, its SDK and a built NeverD, run the actual Objective-C execution comparison:

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

The runner builds its own Objective-C fixture, recovers its method implementations, and links only the recovered `.m` with the same independent calling harness. It compares 85 observable results covering constants, signed and unsigned boundaries, branches, loops, pointer reads/writes, hidden arguments and unused parameter positions. Both arm64 and x86_64 and classic/default pointer layouts are attempted; an architecture the host cannot execute is explicitly skipped. The host architecture must execute. This fixture evidence does not establish completeness for arbitrary iOS programs. `NeverDMobileIOSBackend` is registered with CTest on macOS, including the main CI test profile.
