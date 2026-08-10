**Sprachen**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**Die AI-freundliche Binary-Analyse- und Dekompilations-Engine — 1:1 Lift, auf LLVM**

PE · ELF · Mach-O · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · SBF &nbsp;|&nbsp; Reine C-API

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#bauen)
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O%20%7C%20SBF-informational.svg)](#unterstützte-ziele)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM%20%7C%20SBF-orange.svg)](#unterstützte-ziele)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-lightgrey.svg)](#sdk-und-plugins)

[Dokumentation](../README.de.md) · [Roadmap](../roadmap/README.de.md) · [Mitwirken](CONTRIBUTING.de.md)

</div>

---

> GitHub zeigt auf der Repository-Startseite immer die englische `README.md`. Nutzen Sie die Sprachlinks oben für lokalisierte Versionen.

## Überblick

NeverD ist eine Engine zur Analyse und Dekompilation nativer sowie Smart-Contract-Binärdateien mit Fokus auf **1:1-Instruktionslifting**. Sie lädt **PE**, **ELF**, **Mach-O** und Solana-**SBF-ELF**-Programme. Native Ziele dekodieren mit [Capstone](https://www.capstone-engine.org/); SBF nutzt einen dedizierten versionsbewussten Decoder und gestufte IR. Alle Pfade verwenden handgeschriebene Semantik statt Näherungsübersetzung. Unterstützte Instruktionen bewahren ihr beobachtbares Verhalten in **LLVM IR**, **strukturiertem C**, **sicherem stabilem Rust für SBF** oder bei nativen Zielen in einer **umgeschriebenen Binärdatei**.

Strict-Modus ist **standardmäßig an**. Eine Instruktion ohne Lifter wirft `UnliftedInstruction`, statt zu überspringen, zu raten oder still einen `NOP` auszugeben.

CLI, Integratoren und KI-Agenten nutzen eine Engine — **`libneverd`** — über eine **reine C-API**. Sie linken Capstone, LLVM oder internes C++ nicht direkt.

Solana-SBF-Dekompilierung ist jetzt verfügbar; siehe den [SBF-Leitfaden](../sbf.de.md). Weitere Ziele und Härtung werden in der [Roadmap](../roadmap/README.de.md) verfolgt.

## Warum NeverD?

- **1:1-Semantik** — handgeschriebene Lifter; nicht unterstützte Opcodes werfen im Standard-Strict-Modus
- **LLM-freundlich** — strukturiertes C, LLVM IR und JSON-Analyse über eine reine C-API mit deterministischen Fehlern
- **Eine Pipeline, mehrere Ausgänge** — `lift` → LLVM IR · `decompile` → C/Rust · `patch` → umgeschriebene native Binärdatei
- **Binär-Umschreiben** — PE / ELF / Mach-O mit Section-Trampolinen oder In-Place-Überschreiben
- **Analyse-Werkzeuge** — CLI, Debug-Infos, Signaturen, Plugins und optionale Obfuskations-Pässe

## Unterstützte Ziele

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Jede Zelle der Matrix ist implementiert, die Tiefe der Integrationstests variiert jedoch. Siehe die [Architektur-Abdeckungsmatrix](../architecture.de.md#support-and-test-depth). Mach-O i386 verwendet relocatable `thin`-Objekte, weil modernes macOS historische i386-Executables nicht linken kann.

Solana SBF v0-v4 ELF-Programme nutzen einen dedizierten Strict-Loader,
vollständige versionierte ISA-Metadaten, Low/Med/High IR, verifiziertes LLVM,
portables C11 und sicheres stabiles Rust. Siehe
[Solana-SBF-Dekompilierung](../sbf.de.md).

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

## Schnellstart

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Analyse
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sigs --auto binary
```

Signaturbibliotheken werden zur Build-Zeit nach `build/bin/signatures/` installiert. `sigs --auto` wählt das Set nach Format, Architektur und Bitness.

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

**Artefakte**

| Pfad | Beschreibung |
|------|-------------|
| `build/bin/neverd` | Einheitliche CLI |
| `build/bin/neverd-bench` | Benchmark-Harness (JSON) |
| `build/bin/neverd-sigmaker` | `.pat`-Generator aus statischen Bibliotheken |
| `build/bin/libneverd.*` | Shared Library der Engine |
| `build/bin/sdk/` | `NeverDCAPI.h`, `NeverDPlugin.h` |
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

Fokussierte Targets, CTest-Labels, Fixture-Anforderungen und das formatübergreifende Rewrite-Raster finden Sie unter [NeverD testen](../testing.de.md).

**CMake-Optionen**

| Option | Standard | Beschreibung |
|--------|----------|-------------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI-vorgefertigtes LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | `libneverd` bauen |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Beispiel-Plugins |
| `BUILD_TESTING` | `OFF` | Unit-Tests |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### Pipeline

| Befehl | Ausgabe | Beschreibung |
|--------|---------|-------------|
| `lift` | `.ll` | Nach LLVM IR liften |
| `decompile` | `.c` / `.rs` | C oder SBF-Rust über `--language` |
| `decompile -llvm` | `.c` | Über LLVM IR + Optimizer |
| `patch` | Binärdatei | Maschinencode umschreiben |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>Analysebefehle</strong></summary>

<br>

| Befehl | Zweck |
|--------|------|
| `info` / `dashboard` / `headers` | Metadaten und Überblick |
| `funcs` | Gefundene Funktionen |
| `disasm` | Disassemblieren (`--func` Name oder Hex) |
| `hex` | Hex-Dump an einer Adresse |
| `cfg` / `callgraph` | CFG / Callgraph (JSON; DOT/SVG optional) |
| `xrefs` | Querverweise |
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

Beispiel-Plugin mit `-DNEVERD_BUILD_PLUGINS=ON` bauen. Ladepfade: `<neverd-dir>/plugins`, `~/.neverd/plugins`, `$NEVERD_PLUGIN_PATH`.

## Abhängigkeiten

| Komponente | Rolle | Quelle |
|------------|------|--------|
| **LLVM** (Fork) | IR, Optimierung, Codegen, Diagnostik | `third_party/llvm-project` oder vorgefertigt |
| **Capstone** | Dekodierung | `third_party/capstone` |

Drittanbieter-Komponenten behalten ihre eigenen Lizenzen.

## Mitwirken

Beiträge werden in den Branch **`dev`** integriert. Einrichtung, Release-/Debug-Anleitungen, Stil, fokussierte Tests und Pull-Request-Anforderungen beschreibt der [Leitfaden zum Mitwirken](CONTRIBUTING.de.md). Die Leitfäden zu [Architektur](../architecture.de.md) und [Tests](../testing.de.md) ordnen typische Änderungen dem zugehörigen Code und den passenden Validierungssuiten zu.

## Lizenz

[AGPL-3.0](../../LICENSE)

LLVM-Komponenten behalten ihre Apache-2.0 WITH LLVM-exception-Lizenz. Capstone behält seine eigene Lizenz.
