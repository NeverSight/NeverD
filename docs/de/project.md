**Sprachen**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<!-- i18n-source: edadf84089252d2d8a80ae25926c8013d54fb4a3fea7fc631f3731016652375c -->

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**Die AI-freundliche Binary-Analyse- und Dekompilations-Engine — 1:1 Lift, auf LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; C- und Python-SDKs

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#bauen)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk-und-plugins)

[Dokumentation](README.md) · [Android](android.md) · [iOS](ios.md) · [Roadmap](roadmap.md) · [Mitwirken](CONTRIBUTING.md)

</div>

---

> GitHub zeigt auf der Repository-Startseite immer die englische `README.md`. Nutzen Sie die Sprachlinks oben für lokalisierte Versionen.

<!-- i18n-section: overview -->

## Überblick

NeverD ist eine Engine für native und Smart-Contract-Analyse/Dekompilation mit **1:1-Instruktionslifting**. Sie lädt **PE**, **ELF**, **Mach-O**, Legacy-**EVM**-Bytecode und Solana-**SBF-ELF**. Native Ziele nutzen [Capstone](https://www.capstone-engine.org/); EVM und SBF besitzen versionsbewusste Decoder und gestufte IR. Alle Pfade verwenden handgeschriebene Semantik. Instruktionen bewahren ihr Verhalten in **LLVM IR**, **C**, **Rust für SBF**, **Solidity-Rekonstruktion für EVM** oder einer **umgeschriebenen nativen Binärdatei**.

Strict-Modus ist **standardmäßig an**. Eine Instruktion ohne Lifter wirft `UnliftedInstruction`, statt zu überspringen, zu raten oder still einen `NOP` auszugeben.

CLI, Integratoren und KI-Agenten nutzen eine Engine — **`libneverd`** — über eine **reine C-API**. Sie linken Capstone, LLVM oder internes C++ nicht direkt.

Verwendung, unterstützte Verträge und Grenzen stehen in den Leitfäden zu [EVM](evm.md), [Solana SBF](sbf.md), [mobiler Quelltextrekonstruktion](mobile.md) und experimenteller [Interpreter-Quelltextrekonstruktion](interpreter-recovery.md).

<!-- i18n-section: why-neverd -->

## Warum NeverD?

- **1:1-Semantik** — handgeschriebene Lifter; nicht unterstützte Opcodes werfen im Standard-Strict-Modus
- **LLM-freundlich** — strukturiertes C, LLVM IR und JSON-Analyse über eine reine C-API mit deterministischen Fehlern
- **Eine Pipeline, mehrere Ausgänge** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → umgeschriebene native Binärdatei
- **Binär-Umschreiben** — PE / ELF / Mach-O mit Section-Trampolinen oder In-Place-Überschreiben
- **Analyse-Werkzeuge** — CLI, Debug-Infos, Signaturen, Plugins und optionale Obfuskations-Pässe

<!-- i18n-section: supported-targets -->

## Unterstützte Ziele

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Jede Zelle der Matrix ist implementiert, die Tiefe der Integrationstests variiert jedoch. Siehe die [Architektur-Abdeckungsmatrix](architecture.md#support-and-test-depth). Mach-O i386 verwendet relocatable `thin`-Objekte, weil modernes macOS historische i386-Executables nicht linken kann.

**Die MIPS-ELF-Analyse ist experimentell:** MIPS32r2/o32 und MIPS64r2/n64 unterstützen in beiden Byte-Reihenfolgen eine strikt begrenzte Teilmenge von Ganzzahlbefehlen für Disassemblierung, CFG-Rekonstruktion sowie C- und LLVM-Ausgabe. Das Umschreiben nativen Codes wird nicht unterstützt. Siehe [MIPS-Abdeckung und Firmware-Beispiel (Englisch)](../mips.md) für die separat geprüften Teilmengen microMIPS32, MIPS16e/e2, DSP und MIPS32r5 MSA sowie deren Einschränkungen bei der Quellcodeausgabe.

Legacy-EVM-Bytecode wird unabhängig von nativen Containern unterstützt: Alle
150 Opcodes von Frontier bis Fusaka führen durch Low/Med/High IR, verifiziertes
LLVM `i256`, C23 `_BitInt(256)` und Solidity. Siehe
[EVM-Dekompilation](evm.md).

Solana SBF v0-v4 ELF-Programme nutzen einen dedizierten Strict-Loader,
vollständige versionierte ISA-Metadaten, Low/Med/High IR, verifiziertes LLVM,
portables C11 und sicheres stabiles Rust. Siehe
[Solana-SBF-Dekompilierung](sbf.md).

<!-- i18n-section: mobile-source-recovery -->

### Mobile Quelltextrekonstruktion

Die experimentelle CLI `neverd mobile` unterstützt folgende Eingaben und Ausgaben:

| Plattform | Eingaben | Ausgaben |
|----------|----------|----------|
| [Android](android.md) | APK einschließlich Multidex, DEX, smali-Dateien oder -Verzeichnisse | Java-Quelltext und JSON-Bericht |
| [iOS](ios.md) | IPA, `.app`, Mach-O (arm64/x86_64) | Natives C, unterstützte Objective-C-/Swift-Quelltexte und JSON-Abdeckungsbericht |

Die Rekonstruktion hängt von unterstützten Codemustern ab; Umfang und Grenzen beschreiben die [Mobile-Übersicht (Englisch)](../mobile.md) und die Plattformanleitungen.

<!-- i18n-section: cpu-workloads -->

### CPU-Ausführung und Gastumgebungen

NeverD trennt CPU-Backends von Gast-OS-Modellen für begrenzte Prozesse, native Android-Bibliotheken und Windows-x64-Treiber. Unicorn bietet x64/ARM64-Softwareausführung; Hosts gleicher ISA können KVM (Linux), WHP (Windows) oder HVF (macOS) verwenden. Unterstützte Umgebungen, Nachweise und Grenzen stehen unter [CPU-Ausführung und Gast-Workloads](emulation.md).

<!-- i18n-section: how-it-works -->

## So funktioniert es

```text
Binary (PE / ELF / Mach-O)
  → Loader + DebugInfo
  → Capstone decode
  → LowIR     architecture-neutral NdOps · CFG
  → MedIR     types · ABI · calls · memory · SSA
       │
       ├─ lift        MedIR → LLVM IR
       ├─ decompile   MedIR → HighIR → C
       │              MedIR → LLVM IR → opt → C   (-llvm)
       └─ patch       MedIR → LLVM IR → codegen → binary

EVM (raw / hex / compiler artifact)
  → Runtime-Normalisierung + Hardfork-bewusster Decode
  → EVM LowIR → EVM Stack-SSA MedIR → rekonstruiertes EVM HighIR
       ├─ lift        → verifiziertes LLVM i256/i512
       └─ decompile   → C23 _BitInt(256) oder Solidity-Rekonstruktion

Solana SBF ELF (v0-v4)
  → versionsbewusster Legacy-/Strict-Loader + Verifier
  → SBF LowIR → normalisiertes MedIR → wiederhergestelltes SBF HighIR
       ├─ lift        → verifizierte LLVM-i64-Runtime-ABI
       └─ decompile   → portables C11 oder sicheres stabiles Rust
```

| Stufe | Rolle |
|-------|------|
| **LowIR** | ~77 `NdOp`-Opcodes + CFG |
| **MedIR** | Typen, Aufrufkonventionen, Speichermodell, SSA |
| **HighIR** | Strukturierter Kontrollfluss (`if` / `while` / `for`) |
| **LLVM** | Optimieren, C ausgeben oder Maschinencode erzeugen |

<!-- i18n-section: quick-start -->

## Schnellstart

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# Pseudocode in der Sprache des Programms (C++, Rust, Go oder C)
./build/bin/neverd decompile --language=source -o out.rs rust-binary
./build/bin/neverd decompile --language=cpp -o out.cpp cpp-binary
./build/bin/neverd decompile --language=go --func main.main go-binary

# Optionale Transformationen einer nativen Funktion (normales HighC überspringt sie)
./build/bin/neverd decompile --func main --symbolic-simplify -o enhanced.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=O3 -o optimized.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=custom \
  --llvm-passes=instcombine,early-cse,simplifycfg,dce -o cleaned.c binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Android: APK zu Java (experimentell)
./build/bin/neverd mobile app.apk -o recovered-android
./build/bin/neverd mobile App.ipa -o recovered-ios

# Analyse
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

Signaturbibliotheken werden zur Build-Zeit nach `build/bin/signatures/` installiert. `sigs --auto` wählt das Set nach Format, Architektur und Bitness. Nennt der Rich-Header einer PE-Datei die Visual-Studio-Version ihres Linkers, lädt es nur deren `vs<year>.pat` neben den Dateien, die zu keiner Version gehören. `--sig-base <dir>` wählt auf dieselbe Weise aus einem anderen Signaturbaum. Eine Musterdatei ab 1 MiB wird nur einmal geparst: Ihre Module werden in `neverd/signatures` im Cache-Verzeichnis des Benutzers abgelegt und bei späteren Ladevorgängen eingeblendet. `NEVERD_SIGNATURE_CACHE` nennt ein anderes Verzeichnis, `off` schaltet den Cache ab.

<!-- i18n-section: building -->

## Bauen

**Voraussetzungen:** CMake ≥ 3.20 · Ninja · C++20-Compiler · Git-Submodules (LLVM-Fork + Capstone)

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Erste Konfiguration baut den LLVM-Fork lokal (oft 30–60 Minuten). Spätere Builds sind inkrementell. Presets: `CMakePresets.json` → `release` / `relwithdebinfo` / `debug`.

<details>
<summary><strong>Vorgefertigtes LLVM · Artefakte · Tests · CMake-Optionen</strong></summary>

<br>

**Vorgefertigtes LLVM**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r6
cmake --build build
```

Die reguläre Push- und Pull-Request-CI von NeverD baut das LLVM-Submodul bewusst aus den Quellen. Beim manuellen Start des `CI`-Workflows validiert `use_prebuilt_llvm` die veröffentlichten Pakete; nur ein manuell gewähltes `true` aktiviert vorgefertigtes LLVM. Bleibt es ungesetzt, gilt derselbe Quellbau-Pfad wie in der automatischen CI.

Welches Paket verwendet wird, ergibt sich aus dem Host, auf dem CMake läuft:

| Host | Release-Asset |
|------|---------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

Jedes Archiv wird vor dem Entpacken nach `~/.cache/neverd-llvm/<tag>/<arch>/` oder `NEVERD_LLVM_PREBUILT_CACHE_DIR` gegen den in `cmake/NeverDLLVMPrebuilt.cmake` fixierten Digest geprüft. Für dort nicht beschriebene Tags gilt die veröffentlichte `.sha256`-Datei. Beim fixierten Standard muss `BUILDINFO.txt` außerdem den exakten LLVM-Submodul-Commit nennen. Release-Builds verwenden ccache unter macOS/Linux sowie sccache mit dem GitHub-Actions-Cache für Windows clang-cl. Compiler-Caches beschleunigen nur Neubuilds und werden nie als Release-Artefakte veröffentlicht.

Standard ist die Paketrevision `neverd-llvm-v23.0.0-r6`. Git-Tag, Release-Ziel, Quell-Commit und die drei Archiv-Digests bilden eine unveränderliche versionierte Quellreferenz. Build-Verzeichnisse mit altem Basistag, `neverd-llvm-v23.0.0-r1`, `neverd-llvm-v23.0.0-r2`, `neverd-llvm-v23.0.0-r3`, `neverd-llvm-v23.0.0-r4` oder `neverd-llvm-v23.0.0-r5` wechseln automatisch zu `r6`, außer bei explizitem `NEVERD_LLVM_PREBUILT_SHA256`. `Prebuilt LLVM Audit` läuft bei Pushes, Pull Requests und alle sechs Stunden. `scripts/audit_prebuilt_llvm_release.py` vergleicht die Quellreferenz mit dem aktuellen GitHub-Release und allen veröffentlichten Prüfsummendateien.

Ändert sich der LLVM-Fork, während LLVM weiter `23.0.0` meldet, veröffentlichen Sie die nächste Paketrevision `neverd-llvm-v23.0.0-r7`, danach `-r8`. Überschreiben Sie weder bestehende Releases noch erfinden Sie die LLVM-Version `23.0.1`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r7 \
  -f overwrite_existing_assets=false
```

Aktualisieren Sie nach Erfolg Standardtag, fixierten Commit und alle drei Digests in `cmake/NeverDLLVMPrebuilt.cmake` gemeinsam. Jede Paketrevision verwendet ein eigenes Cache-Verzeichnis; der aktuelle Standard liegt unter `.cache/neverd-llvm/neverd-llvm-v23.0.0-r6`. Veraltete oder neu veröffentlichte Archive scheitern vor dem Entpacken. `overwrite_existing_assets` dient nur der historischen Wiederherstellung und bleibt im normalen Revisionsablauf aus.

**Artefakte**

| Pfad | Beschreibung |
|------|-------------|
| `build/bin/neverd` | Einheitliche CLI |
| `build/bin/neverd-bench` | Benchmark-Harness (JSON) |
| `build/bin/neverd-sigmaker` | `.pat`-Generator aus statischen Bibliotheken |
| `build/bin/libneverd.*` | Shared Library der Engine |
| `build/bin/sdk/` | Kanonischer Include-Root des C SDK; `<neverd/sdk/NeverDCAPI.h>` oder `<neverd/sdk/NeverDPlugin.h>` unter Beibehaltung der Hierarchie `neverd/sdk/` verwenden |
| `build/bin/sdk/python/` | Typisiertes Python-Pluginpaket und Beispiele |
| `build/bin/signatures/` | Mitgelieferte Signaturbibliotheken |

**Tests**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| Ziel | Beschreibung |
|------|-------------|
| `check-neverd` | Alle Tests |
| `check-neverd-semantic` | Nur semantischer Roundtrip (Unicorn) |

Fokussierte Targets, CTest-Labels, Fixture-Anforderungen und das formatübergreifende Rewrite-Raster finden Sie unter [NeverD testen](testing.md).

**CMake-Optionen**

| Option | Standard | Beschreibung |
|--------|----------|-------------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI-vorgefertigtes LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | `libneverd` bauen |
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | CPython-3.10+-Pluginunterstützung einbetten |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Beispiel-Plugins |
| `BUILD_TESTING` | `OFF` | Unit-Tests |
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | Unicorn-abhängige semantische Testgruppe (bei `BUILD_TESTING=ON`) |

</details>

<!-- i18n-section: desktop-workbench -->

## Desktop-Arbeitsumgebung

Die optionale [Desktop-Arbeitsumgebung (Englisch)](../gui.md) übernimmt Aufteilung und Tastenkürzel des klassischen interaktiven Disassemblers, mit Ansichten für Disassembly, Graph, Pseudocode, IR, Hex und Listen, einem Design im Stil von Visual Studio Code, allen 11 UI-Sprachen und `.nddb`-Projektdatenbanken. Die Analyse läuft in einem separaten Prozess ohne Qt; reine CLI-Builds bleiben unabhängig. Unterstützte Abläufe und noch erforderliche Plattformprüfungen vor einer Veröffentlichung stehen im [Qualifikationsnachweis (Englisch)](../gui-qualification.md).

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### Pipeline

| Befehl | Ausgabe | Beschreibung |
|--------|---------|-------------|
| `lift` | `.ll` | Nach LLVM IR liften |
| `decompile` | `.c` / `.sol` / `.rs` | C, EVM-Solidity oder SBF-Rust über `--language` |
| `decompile -llvm` | `.c` | Über LLVM IR + Optimizer |
| `decompile --devirtualize` | `.c` + optionales JSON | Experimentelle x64-Interpreterrekonstruktion; benötigt `--func`; [Vertrag und Beispiele](interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | Experimentell: [Android](android.md), [iOS](ios.md) |
| `patch` | Binärdatei | Maschinencode umschreiben |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-source repl.c --entry replacement --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

Quelltext-Patches können Clang, den normalen NeverC-Modus oder GCC gemäß den [geprüften Quelltext-/Object-Profilen](../source-patching.md) verwenden. Native Objects bewahren den Maschinencode des Erzeugers; Compiler-/Laufzeitkompatibilität und Ausnahmeweitergabe werden für jedes Profil geprüft.

Fehlen einem 32-Bit-ARM-Binärprogramm die ARM/Thumb-Modusmetadaten einer Funktion, geben Sie den Eintrittsmodus vor dem Dekompilieren an:

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

Wiederholen Sie die Option für weitere mehrdeutige Einstiege, gegebenenfalls mit `:arm`. Widerspricht eine Angabe verifizierten Binärmetadaten, schlägt das Laden fehl. Gültige Angaben betreffen nur den exakten Einstieg. Die C-API bietet dieselbe Einstellung vor dem Laden mit `neverd_session_set_arm_function_mode()`.

<details>
<summary><strong>Analysebefehle</strong></summary>

<br>

| Befehl | Zweck |
|--------|------|
| `info` / `dashboard` / `headers` | Metadaten und Überblick |
| `funcs` | Gefundene Funktionen |
| `disasm` | Disassemblieren (`--func` Name oder Hex) |
| `sym-explore` | Begrenzte Pfaderkundung für natives LowIR (`--func`; JSON-Ausgabe) |
| `audit` | Heap-Lebensdauerfehler und uninitialisierte lokale Stack-Lesezugriffe (JSON) |
| `hunt` | Gefährliche Kopierüberläufe mit symbolischen Zeugen und zusätzlichen `process-input-v1`-Wiedergabenachweisen bei vollständigem Plan (JSON-Schema v1) |
| `hex` | Hex-Dump an einer Adresse |
| `cfg` / `callgraph` | CFG / Callgraph (JSON; DOT/SVG optional) |
| `xrefs` | Querverweise |
| `switches` | Sprungtabellen von switch-Anweisungen aus der Gesamtprogrammanalyse |
| `strings` / `search` | Strings / Byte- oder Textsuche |
| `imports` / `exports` / `symbols` / `relocs` | Tabellen |
| `segments` / `sections` / `entrypoints` | Layout |
| `diff` | Zwei Binärdateien vergleichen (`-a` / `-b`) |
| `sigs` | Signaturen (`--auto`) |
| `rename` / `annotate` / `bookmarks` | Sitzungsannotationen |
| `export` | Ergebnisse exportieren |
| `plugins` | Plugins auflisten oder ausführen |

Die meisten Analysebefehle akzeptieren `--json`.

</details>

<!-- i18n-section: sdk-and-plugins -->

## SDK und Plugins

Integratoren nutzen die **reine C-API** von `libneverd`:

| Header | Rolle |
|--------|------|
| `NeverDCAPI.h` | Sitzung, Lift, Dekompilation, Patch, IR / CFG, Annotationen |
| `NeverDPlugin.h` | Dynamische-Bibliothek-Plugin-ABI |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

Für EVM wählt `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)`
Solidity explizit; `neverd_decompile_all` gibt weiterhin C aus. Siehe die
[EVM-C-API-Beispiele](evm.md#c-api).

Native Shared Libraries und Python-`.py`-Dateien verwenden denselben
Plugin-Lebenszyklus. Bauen Sie das native Beispiel mit
`-DNEVERD_BUILD_PLUGINS=ON`; der [Leitfaden für native Plugins](plugins.md)
beschreibt den reinen C-Deskriptor, Callbacks, Build-/Link-Schritte, Erkennung,
CLI-Ablauf und ABI-Einschränkungen. Die Python-Unterstützung ist standardmäßig
aktiv und lässt sich mit `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` vollständig
entfernen; der [Leitfaden für Python-Plugins](python-plugins.md) behandelt
das typisierte SDK und den Paketablauf. Beide Arten verwenden
`<neverd-dir>/plugins`, `~/.neverd/plugins` und `$NEVERD_PLUGIN_PATH`.

<!-- i18n-section: dependencies -->

## Abhängigkeiten

| Komponente | Rolle | Quelle |
|------------|------|--------|
| **LLVM** (Fork) | IR, Optimierung, Codegen, Diagnostik | `third_party/llvm-project` oder vorgefertigt |
| **Capstone** | Dekodierung | `third_party/capstone` |

Drittanbieter-Komponenten behalten ihre eigenen Lizenzen.

<!-- i18n-section: contributing -->

## Mitwirken

Beiträge werden in den Branch **`dev`** integriert. Einrichtung, Release-/Debug-Anleitungen, Stil, fokussierte Tests und Pull-Request-Anforderungen beschreibt der [Leitfaden zum Mitwirken](CONTRIBUTING.md). Die Leitfäden zu [Architektur](architecture.md) und [Tests](testing.md) ordnen typische Änderungen dem zugehörigen Code und den passenden Validierungssuiten zu.

<!-- i18n-section: license -->

## Lizenz

[GNU AGPL ausschließlich Version 3](../../LICENSE). Bei der Weitergabe erfassten NeverD-Codes oder seiner Bearbeitungen müssen Urheberrechts-, Lizenz- und Gewährleistungsausschlüsse einschließlich Projektattribution und Quelle aus [NOTICE](../../NOTICE) erhalten bleiben. Das gilt auch für KI/LLM-gestützte Wiederverwendung und LLVM-basierte Codeumformungen.

Anforderungen, Umfang und Beispiele stehen unter [Attribution und Zitieren](ATTRIBUTION.md). Für nachvollziehbare Verweise empfehlen wir Quelldatei und exakte Version oder Commit. [CITATION.cff](../../CITATION.cff) stellt Software-Zitiermetadaten bereit; Zitieren ersetzt die Einhaltung der Lizenz nicht.

LLVM-Komponenten behalten ihre Apache-2.0 WITH LLVM-exception-Lizenz. Capstone behält seine eigene Lizenz.
