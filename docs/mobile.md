# Mobile application recovery

`neverd mobile` recovers readable Java from Android APK, DEX, and smali inputs. For iOS IPA, `.app`, and Mach-O inputs, it exports available runtime metadata and uses NeverD's native decompiler to produce C pseudocode. This is an experimental CLI workflow; mobile containers are not accepted by the native C SDK or GUI loader.

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

The output separates C pseudocode under `sources/` from available Objective-C declarations and runtime/symbol metadata under `metadata/`. The selected thin binary is retained as `artifacts/selected.macho`; temporary package contents are removed. `report.json` records the selected architecture, artifact, outputs, recovered function count, and limitations. Include-only output with no recovered function bodies fails. `--metadata-only` skips native decompilation. `--max-func N` limits native function recovery; zero selects all discovered functions.

Objective-C declarations depend on the metadata and pointer encodings present in the binary. Swift symbol names may be available even when Swift source cannot be recovered. Stripping, optimization, unavailable metadata, and unsupported fixups reduce recovery. Neither original Swift source nor original Objective-C method bodies are reconstructed; method implementations are represented by native C pseudocode. Recovered native types and calling conventions are approximate and may need manual corrections. Consult the per-input metadata limitations before interpreting an empty declaration list.

## Limits and failures

`-o` must name a new directory outside any directory input. Existing output is never overwritten. Work is staged and published only after successful recovery and output validation. `--json` prints a versioned report for automation; failures return nonzero and a JSON error when the helper has started.

The defaults are 20,000 entries, 2 GiB of input/extracted or final output data, and 300 seconds per backend process. Set `--max-files`, `--max-bytes`, and `--timeout` to adjust these positive limits. The temporary output work area is also monitored while backends run. Diagnostics are capped at 16 MiB per process. Archives with traversal paths, links, special files, case collisions, or encrypted ZIP entries are rejected. Directory inputs also reject symbolic links and special files.

These limits are robustness controls, not a sandbox for third-party backend code. Backend dependencies run as local processes. Failed staging output is removed; the error includes the bounded backend diagnostic tail for process failures.

## Validation

```sh
cmake --build build --target check-neverd-mobile
python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

The component tests cover unsafe containers, malformed and encrypted native inputs, backend failures and incomplete Java, architecture selection, and output preservation. Tests that invoke the built CLI use `NEVERD_BUILD_DIR`. The real Android smoke runner additionally needs the supported Java backend and JDK; it recovers synthetic smali/DEX/multidex APK inputs and compiles/runs the recovered Java.
