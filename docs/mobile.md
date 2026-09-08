# Mobile application recovery

[← Documentation index](README.md) · [Complete Android guide](android.md) · [Complete iOS guide](ios.md)

`neverd mobile` recovers readable Java from Android APK, DEX, and smali inputs. For iOS IPA, `.app`, and Mach-O inputs, it exports native C and reconstructs supported Objective-C method bodies into `.m` source, alongside experimental Swift source and runtime metadata. This is an experimental CLI workflow; mobile containers are not accepted by the native C SDK or GUI loader.

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

The [complete iOS guide](ios.md) documents IPA, `.app`, and Mach-O selection, setup, all CLI options, source schemas, coverage, and verification.

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

Each run selects one executable. `--artifact` is relative to the application bundle for both IPA and `.app`. Fat selection prefers arm64, arm, x86_64, then i386; source-language projection targets arm64/x86_64. Encrypted selected slices are rejected. The experimental native path emits C and supported Objective-C method bodies, including scalar/pointer, floating, mixed, and stack ABI bindings. Runtime class/ivar layouts and separate categories are retained when validated; unresolved layouts, signatures, calls, and other dependencies remain explicit omissions.

Swift recovery classifies demangled signatures, binds them to native entries and ABI locations, and emits supported functions, class methods/initializers, and fixed-layout struct methods as actual `.swift` source. It requires a demangler selected by `--swift-demangle`, `NEVERD_SWIFT_DEMANGLE`, PATH, or macOS `xcrun --find swift-demangle`. Missing automatic tools preserve unclassified coverage; explicit missing tools fail. Generic/resilient, async/throwing, unsupported runtime-generated callable forms, and incomplete source dependency groups remain unrecovered.

Normal output includes `sources/native.c`, optional `sources/objc.m` and `sources/swift.swift`, declarations and runtime metadata, method/signature coverage JSON, logs, `artifacts/selected.macho`, and `report.json`. Generated source does not call the original binary as a recovery bridge. Swift `source_units` groups type declarations and methods; standalone method rows must not be concatenated to rebuild classes. Outer `status: "success"` means output publication, not complete method coverage or semantic equivalence.

`--metadata-only` invokes no backend/demangler and emits no source or method coverage. Its Python Objective-C reader does not resolve chained or relocatable-object pointers; full recovery uses native resolved metadata. Swift raw metadata may retain unsupported references as partial. `--max-func` limits native function recovery and is ignored in metadata-only mode. Missing native function bodies fail a normal run. Temporary unpacked inputs are removed.

Original comments, formatting, removed identifiers, and compilation-lost source structures cannot be reconstructed exactly. Read each method's recovery status and reason, the separate Swift callable/non-callable/unclassified counts, and the documented limitations before using the output.

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

The runner builds its own Objective-C fixture, recovers its method implementations, and links only the recovered `.m` with the same independent calling harness. Its 22-method fixture compares 141 observable results covering integer boundaries, branches, loops, pointer reads/writes, hidden and unused arguments, float/double identity bits, mixed parameters, and stack positions. A current passing run is required before treating a case as verified. Both arm64 and x86_64 and classic/default pointer layouts are attempted; an architecture the host cannot execute is explicitly skipped. The host architecture must execute. This fixture evidence does not establish completeness for arbitrary iOS programs. `NeverDMobileIOSBackend` is registered with CTest on macOS, including the main CI test profile.

The independent Swift recovery runner is `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd`. It recompiles generated Swift and its harness without linking the original binary; unsupported callables and behavior differences are failures. See the [iOS guide](ios.md) for coverage semantics and retained failure evidence.
