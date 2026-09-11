**Languages**: [English](android.md) | [简体中文](zh-CN/android.md) | [繁體中文](zh-TW/android.md) | [日本語](ja/android.md) | [한국어](ko/android.md) | [Français](fr/android.md) | [Deutsch](de/android.md) | [Español](es/android.md) | [Italiano](it/android.md) | [Русский](ru/android.md) | [العربية](ar/android.md)

# Android Java recovery

[← Documentation index](README.md)

`neverd mobile` recovers readable Java from APK, DEX, and smali using NeverD’s built-in engine by default. Independently implemented readers share a typed Dalvik model and a bounded Java emitter. This experimental CLI feature does not claim feature parity with JADX or complete recovery of arbitrary APKs. APK containers and Java output are not exposed through the native C SDK, Python plugin SDK, GUI loader, or `neverd decompile --language`.

Recovered Java is a reconstruction of bytecode. Original comments, formatting, source-language choices, and removed identifiers are unavailable; Kotlin bytecode also produces Java. A successful run does not prove semantic equivalence or guarantee that every method recompiles. No analyzed application is launched by this workflow.

## Quick start

After preparing the runtimes below, choose a new output directory:

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Open `recovered-app/sources/` to read the Java files and `recovered-app/report.json` to inspect the input inventory and limitations. A directory input is preferable when smali classes refer to each other.

## Runtime setup

| Component | Requirement | Selection |
|-----------|-------------|-----------|
| NeverD | Build the `neverd` target with C++20 support. The mobile workflow is compiled into the native CLI and does not use a Python interpreter. Distribute the executable with the native libraries required by your build. | `build/bin/neverd` / PATH |

The default engine is implemented in C++20 and needs no Python, Java, or JADX runtime. It accepts representable ordinary declarations and operations from DEX 035 and 037–040 and from smali. DEX 041, dynamic calls such as `invoke-custom`, some initialization paths, unknown semantic annotations or operations, and identifiers that Java cannot represent fail explicitly. File-format acceptance does not mean every instruction or declaration in that format is supported.

Java name binding distinguishes class headers from class bodies and can resolve known same-package shadowing; it explicitly rejects cases where missing external superclass or interface declarations prevent it from establishing type or generated Java helper bindings, and only `java.lang.Object` is assumed to contribute no inherited member types without a supplied declaration. Smali floating-point literals are rounded directly to their target single or double precision, preserving the resulting bit patterns.

The built-in C++ engine preserves and validates supported class, field, and method `Signature` metadata, including type variables, arrays, wildcards, bounds, and method-level shadowing. Erasure must match the original DEX declaration identity. `Throws` is retained; an unproven exception hierarchy is rejected. Generic inheritance or member substitution, bridge regeneration, generic method invocation, parameterized inner types, and hidden constructor parameter mapping remain explicitly unsupported where the required proof is unavailable.

The empty, runtime-visible Java 8 `@java.lang.Deprecated` marker is preserved on classes, fields, methods and constructors. Parameter annotations, annotated static initializers, other visibility levels and element values such as `since` or `forRemoval` are rejected. CI separately compares the classfile `Deprecated` attribute and runtime annotations after recompilation, including unannotated controls and generated helpers; helpers remain outside the original method count.

The built-in engine also preserves runtime-visible `@Retention`, `@Target`, `@Documented` and `@Inherited` on supported annotation declarations, emitting a real `@interface`. This subset has no fields, methods, type parameters or nested declarations. It permits top-level annotations and static member annotations with proven names, access and enclosing identity; local or anonymous scopes are rejected.

Empty marker applications are supported on class, interface and annotation declarations only when the same analyzed class set contains an accessible, matching marker definition. Its Retention and Target must permit the actual application. A `SOURCE` declaration can be reconstructed, but a persisted application is rejected; `CLASS` or absent Retention requires DEX build visibility (`0`), and `RUNTIME` requires runtime visibility (`1`). Missing Retention remains distinct from explicit `CLASS`; absent Target remains distinct from an empty array, and target array order is preserved. Target values follow Java 8; newer values such as `MODULE` and `RECORD_COMPONENT` are rejected. `@Inherited` is preserved without copying inherited applications onto subclass declarations.

Annotation element declarations and defaults, nonempty custom applications, custom markers on fields/methods/parameters, external annotation definitions, repeatable annotation containers and `kotlin.Metadata` remain unsupported. A marker definition is emitted as a class declaration without element or helper methods, and does not increase the recovered-method count.

CI uses owned Java 8 fixtures through D8 and NeverD, recompiles all generated Java, and compares complete `Signature` metadata, reflection results, and behavior. These fixture checks do not qualify real applications for complete recovery.

### Linux and macOS

```sh
cmake --build build --target neverd

./build/bin/neverd mobile app.apk -o recovered-app
```

`NEVERD_JADX` and a `jadx` executable on PATH do not select the external engine; only an explicit `--jadx PATH` does. There is no automatic fallback. Quote paths containing spaces.

### Windows PowerShell

```powershell
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app
```

Multi-configuration builds may put the executable under `build/bin/Release/`. Follow the normal native-library deployment requirements for that build.

## Supported inputs and boundaries

| Input | Behavior | Important boundary |
|-------|----------|--------------------|
| `.apk` | Validate the complete ZIP, then analyze all root `classes.dex`, `classes2.dex`, and later numbered DEX files together | Code only; no resources or manifest decoding |
| `.dex` | Validate and parse DEX 035 or 037–040 with the built-in reader | DEX 041 and unsupported declarations or operations fail; renamed or truncated files are not valid bytecode |
| `.smali` | Analyze the supplied class | Referenced sibling classes are not implicitly loaded |
| Smali directory | Recursively collect `.smali` files and analyze them together | Include nested classes and dependent smali roots in the input directory |

To analyze an APK decode tree containing `smali/` and `smali_classes2/`, pass their common directory. Only `.smali` files reach the backend, but the entire supplied tree is validated and copied first; unrelated large assets still count against input limits. A compact directory containing only the relevant smali roots reduces work.

Split APKs are separate inputs. Each APK containing DEX can be processed independently, but this command does not merge an APK set; resource-only splits fail because they contain no root DEX. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat`, and `.vdex` are not accepted mobile inputs. Backend support for some of these formats does not make them supported by this NeverD command.

APK resources, `AndroidManifest.xml`, assets, JNI/native libraries, and code downloaded at runtime are not recovered as Java. Extract a native `.so` separately and use `neverd decompile library.so -o library.c`. Encrypted or packed payloads must already be available as ordinary DEX/smali for this static workflow; no unpacking, device attachment, or protection bypass is performed.

## Options and precedence

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --timeout=600 --max-files=30000 --max-bytes=4294967296 --json
```

| Option | Default | Meaning |
|--------|---------|---------|
| `-o DIRECTORY` | Required | New output directory outside any directory input; never overwrite existing output |
| `--platform=auto\|android` | `auto` | Select Android explicitly or infer the platform from the input |
| `--jadx PATH` | Not set: built-in engine | Explicitly select the separately installed JADX compatibility adapter; no environment-based selection or automatic fallback |
| `--timeout N` | `300` | Positive analysis-time budget for the built-in engine; positive seconds per external backend process, including version probing |
| `--max-files N` | `20000` | Positive entry limit, including materialized directories |
| `--max-bytes N` | `2147483648` | Positive input, extracted-data, and final-output byte limit |
| `--json` | Off | Print the report as JSON rather than a human summary |

Non-default `--arch` selection, `--artifact`, `--metadata-only`, and a nonzero `--max-func` belong to iOS and are rejected for Android; explicit `--arch=auto` is accepted. There is no arbitrary backend-option passthrough. The explicit JADX adapter isolates config/cache/temp directories and does not import ambient backend settings or plugin configuration.

Input, extraction, and final output retain the configured file and byte budgets. The built-in reader and emitter also enforce bounded work and elapsed-time checks. External backend work areas allow up to three times the configured entry/byte budgets for staged inputs and intermediate output; logs are capped at 16 MiB per process. These are resource controls, not a sandbox. Raising one limit does not disable the others.

## Output layout and JSON report

```text
recovered-app/
  sources/                       recovered Java packages and classes
  metadata/android-methods.json  built-in method coverage
  report.json                    versioned inventory and recovery limits
```

Temporary inputs are removed. Nested classes can share an outer-class source file, so Java source count is not DEX class count. Generated methods may use a Java dispatch loop; they do not execute the original DEX or use a runtime bridge to it.

The built-in report embeds `android_method_recovery`, also written to `metadata/android-methods.json`. Every original method remains in the inventory. Its invariant is `method_count = recovered_method_count + projected_method_count + declaration_only_method_count + unrecovered_method_count`; an absent `projected_method_count` means zero, and `unrecovered_method_count` remains zero before publication. Original `native` and `abstract` methods have status `declaration-only` and are not counted as recovered bodies.

A bounded subset of named, noncapturing local classes can be emitted inside their exact enclosing static method. This requires an ordinary scalar method, a fieldless class directly extending `Object`, a real no-argument constructor, scalar instance methods, and verified object uses that do not escape the supported scope. Anonymous classes, captures, unsupported modifiers, and unproved uses still fail explicitly.

For these exports, the local methods and their enclosing method are `source-projected`, with `projection_kind: "named-method-local"`; coverage is `partial` even when the outer pipeline report says `success`. The recompiled binary names and access flags remain unverified. Java compilation can choose a different local-class binary name, so `class_source_bindings` preserves the original class, enclosing method, source path and local name with `binary_name_status: "unverified"`.

`generated_source_helpers` lists extra methods with the exact kinds `throw-helper`, `constant-helper`, `default-constructor`, and `field-initializer`. The last kind identifies an additional generated `<clinit>` that was absent from the original method inventory. These extras do not count toward the original-method denominator. Compilation or one matching binary name does not upgrade this status to complete recovery. The following shortened example has no projected methods:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {
    "name": "neverd",
    "version": "1",
    "execution": "builtin"
  },
  "input_code_files": [
    "classes.dex",
    "classes2.dex"
  ],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": [
    "sources/example/Main.java",
    "sources/example/Peer.java"
  ],
  "logs": [],
  "android_method_recovery": {
    "schema_version": 1,
    "status": "recovered",
    "class_count": 2,
    "method_count": 6,
    "recovered_method_count": 5,
    "declaration_only_method_count": 1,
    "unrecovered_method_count": 0
  }
}
```

`input_kind` is `apk`, `dex`, `smali`, or `smali-directory`. `input_code_files` lists input bytecode names or smali paths, while `java_sources` and `logs` are relative to the output root. `source` is the input basename. Additional reconstruction limitations are included in real reports; retain them when presenting results to other tools.

For automation, check the process exit code before consuming `status`, and keep the report outside the new output directory when redirecting stdout:

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

Successful native CLI runs return zero. Recovery failures return nonzero; `--json` reports handled failures with `schema_version`, `status: "error"`, and `error`. Argument parsing, native executable or library startup failures, and interruptions can instead report on stderr. Consumers must inspect the exit status first.

## Failure handling and troubleshooting

Publication is transactional: existing output is preserved, and failed staging output is removed. Unsupported operations, unresolved register flows, unrepresentable declarations, malformed exception handling, and exhausted budgets fail the built-in run instead of publishing missing method bodies. The external adapter also rejects nonzero exits, logged assembly/decompilation errors, duplicate-class omissions, incomplete-code markers, empty Java files, and missing Java output. Recovery success is not a proof of semantic equivalence.

| Symptom | Action |
|---------|--------|
| Unsupported DEX, instruction, declaration, or initialization | Read the explicit diagnostic; check the supported subset. Use `--jadx PATH` only if you deliberately choose the separate compatibility adapter |
| Invalid input or duplicate class | Correct the supplied bytecode/class set; unsupported bodies are not silently omitted |
| Timeout or budget exceeded | Narrow the input or adjust `--timeout`, `--max-files`, and `--max-bytes` within available resources |
| Output already exists | Choose a new output directory |

## Optional JADX compatibility adapter

`--jadx PATH` selects external JADX, not the built-in implementation. Install JADX 1.5.6 or newer with standard DEX/smali input plugins and Java 11 or newer. Obtain the complete [JADX distribution](https://github.com/skylot/jadx/releases/tag/v1.5.6), retain its `bin/` and `lib/` layout and included dependency licenses when redistributing it. Nothing is downloaded automatically. The adapter report identifies the actual `jadx` engine and detected version; it does not claim built-in method coverage.

On Windows, pass the distribution’s `.bat`/`.cmd` launcher or `lib/jadx-*-all.jar`. NeverD resolves the distribution JAR and invokes Java directly; application paths do not enter a command shell. `JAVA_HOME` or PATH selects Java. Successful adapter runs keep `logs/jadx-version.log` and `logs/jadx.log`; failed staging directories and logs are removed. Only a nonzero backend exit includes a bounded log tail. Launch failures, timeouts, and budget violations have their own diagnostics.

```sh
neverd mobile app.apk -o recovered-jadx --jadx /opt/jadx/bin/jadx
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

## Verification and support depth

Python is used only by the development test harnesses below; built-in mobile recovery runs in the native C++20 CLI.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
```

The component and CLI tests check parsing, output contracts, and failure cleanup. The internal execution runner uses a JDK (`java` and `javac`) and D8 to build independent DEX/APK fixtures and compile/run recovered Java; these are test dependencies, not requirements for built-in recovery. Run it against the current build and inspect its results before claiming a case is verified. The separate compatibility runner additionally requires JADX and exercises that adapter. Fixture success does not establish complete recovery of arbitrary applications.

See the [mobile overview](mobile.md) for the related iOS workflow.
