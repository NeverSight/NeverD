**Languages**: [English](android.md) | [简体中文](zh-CN/android.md) | [繁體中文](zh-TW/android.md) | [日本語](ja/android.md) | [한국어](ko/android.md) | [Français](fr/android.md) | [Deutsch](de/android.md) | [Español](es/android.md) | [Italiano](it/android.md) | [Русский](ru/android.md) | [العربية](ar/android.md)

# Android Java recovery

[← Documentation index](README.md)

`neverd mobile` recovers readable Java from APK, DEX, and smali inputs through a separately installed JADX backend. The workflow validates and stages bytecode, analyzes related classes together, checks the generated output, and publishes a source directory with a machine-readable report. It is an experimental CLI feature. APK containers and Java output are not exposed through the native C SDK, Python plugin SDK, GUI loader, or `neverd decompile --language`.

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
| NeverD | Build the `neverd` target; distribute its sibling `mobile/` directory too | `build/bin/neverd` or an executable on PATH |
| Python | Python 3.10 or newer, independent of the embedded plugin host | `--python`, then `NEVERD_PYTHON`, then `python3`/`python` on PATH |
| Java backend | JADX 1.5.6 or newer with the standard DEX and smali input plugins | `--jadx`, then `NEVERD_JADX`, then `jadx` on PATH |
| Java runtime | Java 11 or newer; a JDK is required for compile/run verification | `JAVA_HOME` or Java on PATH |

NeverD does not download dependencies automatically. Obtain the complete [JADX distribution](https://github.com/skylot/jadx/releases/tag/v1.5.6), keep its `bin/` and `lib/` layout, and retain its included licenses when redistributing it. The tested backend release is 1.5.6; later versions must satisfy the same CLI contract. Dependency setup is separate from building NeverD's LLVM pipeline.

### Linux and macOS

```sh
cmake --build build --target neverd
python3 --version
java -version
/opt/jadx/bin/jadx --version

./build/bin/neverd mobile app.apk -o recovered-app \
  --python python3 --jadx /opt/jadx/bin/jadx
```

For repeated use, set `NEVERD_JADX=/opt/jadx/bin/jadx` and optionally `NEVERD_PYTHON` to an interpreter path. Point `JAVA_HOME` at the JDK installation directory if Java is not already available. Paths containing spaces must be quoted.

### Windows PowerShell

```powershell
$env:JAVA_HOME = 'C:\Tools\jdk'
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app `
  --python 'C:\Tools\Python\python.exe' `
  --jadx 'C:\Tools\jadx\bin\jadx.bat'
```

The backend's `.bat`/`.cmd` path resolves the distribution's single `lib/jadx-*-all.jar`; NeverD invokes Java directly. You can also pass that JAR to `--jadx`. Application paths are never inserted into a command shell. Multi-configuration builds may put the executable under `build/bin/Release/` instead. Moving only the executable without its `mobile/` directory produces a helper-missing error.

## Supported inputs and boundaries

| Input | Behavior | Important boundary |
|-------|----------|--------------------|
| `.apk` | Validate the complete ZIP, then analyze all root `classes.dex`, `classes2.dex`, and later numbered DEX files together | Code only; no resources or manifest decoding |
| `.dex` | Validate the DEX magic and let the backend decode its contents | A renamed or truncated file is not valid bytecode |
| `.smali` | Analyze the supplied class | Referenced sibling classes are not implicitly loaded |
| Smali directory | Recursively collect `.smali` files and analyze them together | Include nested classes and dependent smali roots in the input directory |

To analyze an APK decode tree containing `smali/` and `smali_classes2/`, pass their common directory. Only `.smali` files reach the backend, but the entire supplied tree is validated and copied first; unrelated large assets still count against input limits. A compact directory containing only the relevant smali roots reduces work.

Split APKs are separate inputs. Each APK containing DEX can be processed independently, but this command does not merge an APK set; resource-only splits fail because they contain no root DEX. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat`, and `.vdex` are not accepted mobile inputs. Backend support for some of these formats does not make them supported by this NeverD command.

APK resources, `AndroidManifest.xml`, assets, JNI/native libraries, and code downloaded at runtime are not recovered as Java. Extract a native `.so` separately and use `neverd decompile library.so -o library.c`. Encrypted or packed payloads must already be available as ordinary DEX/smali for this static workflow; no unpacking, device attachment, or protection bypass is performed.

## Options and precedence

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --jadx /opt/jadx/bin/jadx --timeout=600 \
  --max-files=30000 --max-bytes=4294967296 --json
```

| Option | Default | Meaning |
|--------|---------|---------|
| `-o DIRECTORY` | Required | New output directory outside any directory input; never overwrite existing output |
| `--platform=auto\|android` | `auto` | Select Android explicitly or infer the platform from the input |
| `--jadx PATH` | Environment/PATH | Backend launcher or distribution JAR; explicit option takes precedence |
| `--python PATH` | Environment/PATH | Interpreter for the bundled helper; explicit option takes precedence |
| `--timeout N` | `300` | Positive seconds per backend process, including version probing |
| `--max-files N` | `20000` | Positive entry limit, including materialized directories |
| `--max-bytes N` | `2147483648` | Positive input, extracted-data, and final-output byte limit |
| `--json` | Off | Print the report as JSON rather than a human summary |

Non-default `--arch` selection, `--artifact`, `--metadata-only`, and a nonzero `--max-func` belong to iOS and are rejected for Android; explicit `--arch=auto` is accepted. There is no arbitrary backend-option passthrough. Backend config/cache/temp directories are isolated for each run; ambient backend settings and plugin configuration are not imported into the run.

Limits are resource controls, not a sandbox for the backend process. The staging work area is also monitored with room for input, extracted data, and output, up to three times the configured entry/byte budgets. Logs are capped at 16 MiB per process. Large inputs may still need more Java heap or a larger timeout; increasing one limit does not disable the others.

## Output layout and JSON report

```text
recovered-app/
  sources/                 recovered Java packages and classes
  logs/jadx-version.log    backend version probe
  logs/jadx.log            backend diagnostics
  report.json              versioned inventory and recovery limits
```

Temporary copies and backend caches are removed. The exact Java filenames and their count depend on backend reconstruction; nested classes can share an outer-class source file. Java source count is therefore not DEX class count.

A shortened illustrative report:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {"name": "jadx", "version": "1.5.6"},
  "input_code_files": ["classes.dex", "classes2.dex"],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": ["sources/example/Main.java", "sources/example/Peer.java"],
  "logs": ["logs/jadx-version.log", "logs/jadx.log"],
  "limitations": ["Java is reconstructed from bytecode; original comments, formatting, and stripped names cannot be restored."]
}
```

`input_kind` is `apk`, `dex`, `smali`, or `smali-directory`. `input_code_files` lists input bytecode names or smali paths, while `java_sources` and `logs` are relative to the output root. `source` is the input basename. Additional reconstruction limitations are included in real reports; retain them when presenting results to other tools.

For automation, check the process exit code before consuming `status`, and keep the report outside the new output directory when redirecting stdout:

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

Successful helper runs return zero. Recovery failures return nonzero; once the helper starts on a supported interpreter, `--json` produces an error object containing `schema_version`, `status: "error"`, and `error`. Native argument parsing, missing Python, Python older than 3.10, or a missing helper can fail earlier with stderr instead of JSON. Interruption can also report on stderr. Consumers must handle those cases.

## Failure handling and troubleshooting

Publication is transactional: existing output is preserved, and failed staging output is removed. Backend nonzero exits, logged assembly/decompilation errors, duplicate-class omissions, explicit incomplete-code markers, empty Java files, and no-Java results all fail the command. A backend reporting success does not independently prove method-level correctness.

| Symptom | Action |
|---------|--------|
| Python/helper missing | Install/select Python 3.10+ and keep the sibling `mobile/` directory with NeverD |
| Backend cannot execute or version unsupported | Verify `--jadx`, the complete distribution layout, Java, and the minimum backend version |
| Invalid DEX header / no root DEX | Check the actual input format; use a code-containing APK, ordinary DEX, or smali |
| No smali files | Point at a directory containing `.smali` files, not Java source or an assets-only tree |
| Duplicate class or partial recovery | Remove duplicate input definitions or analyze the relevant bytecode set separately; fix malformed smali rather than accepting an incomplete result |
| Timeout / byte or entry limit | Use a smaller relevant input or deliberately increase the corresponding limit |
| Unsafe archive path or link | Recreate a regular, portable input without traversal names, links, special files, or conflicting paths |
| Output already exists | Choose another output directory; do not reuse a previous success directory |

Successful runs retain backend logs. Failed staging directories, including their logs, are deleted; nonzero backend exits include a bounded diagnostic tail in the error. Timeouts and resource-limit failures report their own diagnostic messages. For backend-specific investigation, reproduce on an isolated input with the backend's own CLI and a separate diagnostic directory. Never infer success just because some Java appeared before a failure.

## Verification and support depth

```sh
cmake --build build --target check-neverd-mobile
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

The first two commands exercise the mobile component and built CLI contracts; platform-specific fixture requirements can produce explicit skips. The real backend runner additionally needs a JDK (`java` and `javac`). It constructs single-smali, cross-class/nested smali, DEX, and true multidex APK cases, then compiles and executes the recovered Java. Its cases cover branches, loops, arrays, exception handling, class references, malformed input, and duplicate-class omissions. This is evidence for those fixtures, not a promise of complete recovery for arbitrary applications.

The [Mobile Decompilation workflow](../.github/workflows/mobile.yml) runs component tests on Linux, macOS, and Windows with Python 3.10 and 3.13, plus a checksum-pinned real Android backend job on Linux. See the [mobile overview](mobile.md) for the separate iOS workflow and its current limits.
