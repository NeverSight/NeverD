**Sprachen**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**Die AI-freundliche Binary-Analyse- und Dekompilations-Engine — 1:1 Lift, auf LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; Reine C-API

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#bauen)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-orange.svg)](#sdk-und-plugins)

[Dokumentation](../README.de.md) · [Roadmap](../roadmap/README.de.md) · [Mitwirken](CONTRIBUTING.de.md)

</div>

---

> GitHub zeigt auf der Repository-Startseite immer die englische `README.md`. Nutzen Sie die Sprachlinks oben für lokalisierte Versionen.

## Überblick

NeverD ist eine Engine für native und Smart-Contract-Analyse/Dekompilation mit **1:1-Instruktionslifting**. Sie lädt **PE**, **ELF**, **Mach-O**, Legacy-**EVM**-Bytecode und Solana-**SBF-ELF**. Native Ziele nutzen [Capstone](https://www.capstone-engine.org/); EVM und SBF besitzen versionsbewusste Decoder und gestufte IR. Alle Pfade verwenden handgeschriebene Semantik. Instruktionen bewahren ihr Verhalten in **LLVM IR**, **C**, **Rust für SBF**, **Solidity-Rekonstruktion für EVM** oder einer **umgeschriebenen nativen Binärdatei**.

Strict-Modus ist **standardmäßig an**. Eine Instruktion ohne Lifter wirft `UnliftedInstruction`, statt zu überspringen, zu raten oder still einen `NOP` auszugeben.

CLI, Integratoren und KI-Agenten nutzen eine Engine — **`libneverd`** — über eine **reine C-API**. Sie linken Capstone, LLVM oder internes C++ nicht direkt.

Eingabeformate, Host-Verträge und Grenzen stehen in den Leitfäden für [EVM](../evm.de.md) und [Solana SBF](../sbf.de.md).

## Warum NeverD?

- **1:1-Semantik** — handgeschriebene Lifter; nicht unterstützte Opcodes werfen im Standard-Strict-Modus
- **LLM-freundlich** — strukturiertes C, LLVM IR und JSON-Analyse über eine reine C-API mit deterministischen Fehlern
- **Eine Pipeline, mehrere Ausgänge** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → umgeschriebene native Binärdatei
- **Binär-Umschreiben** — PE / ELF / Mach-O mit Section-Trampolinen oder In-Place-Überschreiben
- **Analyse-Werkzeuge** — CLI, Debug-Infos, Signaturen, Plugins und optionale Obfuskations-Pässe

## Unterstützte Ziele

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Jede Zelle der Matrix ist implementiert, die Tiefe der Integrationstests variiert jedoch. Siehe die [Architektur-Abdeckungsmatrix](../architecture.de.md#support-and-test-depth). Mach-O i386 verwendet relocatable `thin`-Objekte, weil modernes macOS historische i386-Executables nicht linken kann.

Legacy-EVM-Bytecode wird unabhängig von nativen Containern unterstützt: Alle
150 Opcodes von Frontier bis Fusaka führen durch Low/Med/High IR, verifiziertes
LLVM `i256`, C23 `_BitInt(256)` und Solidity. Siehe
[EVM-Dekompilation](../evm.de.md).

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

## Schnellstart

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Analyse
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
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

Die reguläre Push- und Pull-Request-CI von NeverD baut das LLVM-Submodul bewusst aus den Quellen. Beim manuellen Start des `CI`-Workflows validiert `use_prebuilt_llvm` die veröffentlichten Pakete; nur ein manuell gewähltes `true` aktiviert vorgefertigtes LLVM. Bleibt es ungesetzt, gilt derselbe Quellbau-Pfad wie in der automatischen CI.

Welches Paket verwendet wird, ergibt sich aus dem Host, auf dem CMake läuft:

| Host | Release-Asset |
|------|---------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

Jedes Archiv wird gegen den in `cmake/NeverDLLVMPrebuilt.cmake` fixierten Digest geprüft — oder gegen die daneben veröffentlichte `.sha256`, falls die Pins den Tag nicht beschreiben —, bevor es unter `~/.cache/neverd-llvm/<tag>/<arch>/` entpackt wird (oder unter dem Pfad aus `NEVERD_LLVM_PREBUILT_CACHE_DIR`). Der Release-Build nutzt ccache auf macOS und Linux; Windows-clang-cl-Builds nutzen sccache mit dem GitHub-Actions-Cache als Backend. Compiler-Caches beschleunigen nur Rebuilds und werden nie als Release-Assets veröffentlicht.

Der Release-Tag versioniert das NeverD-Paket, während `BUILDINFO.txt` den genauen Commit des LLVM-Forks festhält. Meldet LLVM weiterhin `23.0.0`, obwohl sich die Fork-Quellen geändert haben, ist die übliche unveränderliche Wahl eine Paketrevision wie `neverd-llvm-v23.0.0-r1` (dann `-r2`) — nicht `23.0.1`, solange sich nicht LLVMs eigene Patch-Version geändert hat. Richten Sie `NEVERD_LLVM_PREBUILT_TAG` auf diese neue Revision.

Um das bestehende veränderliche Release `neverd-llvm-v23.0.0` an Ort und Stelle zu reparieren, starten Sie den Workflow `NeverD LLVM Release` vom `main`-Branch von llvm-project und aktivieren Sie `overwrite_existing_assets`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0 \
  -f overwrite_existing_assets=true
```

Das ersetzt gleichnamige Assets, verschiebt den bestehenden Git-Tag aber bewusst nicht. Aktualisieren Sie im selben Zug die in `cmake/NeverDLLVMPrebuilt.cmake` fixierten Digests: Diese Digests, nicht der Tag, benennen den Build, den eine NeverD-Revision erwartet. Ein veraltetes `~/.cache/neverd-llvm/neverd-llvm-v23.0.0/` wird dadurch beim nächsten Konfigurieren ersetzt, und ein Archiv, das zu keinem fixierten Digest passt, hält dieses Konfigurieren mit einer Prüfsummenabweichung an, statt später als ein Header aufzutauchen, den das ältere Paket nicht enthielt. Ein neuer `-rN`-Tag vermeidet das Überschreiben ganz. Der Workflow lehnt versehentliches Ersetzen ab, solange die Checkbox nicht gesetzt ist, und lehnt es vollständig ab, wenn GitHub das Release als unveränderlich markiert.

**Artefakte**

| Pfad | Beschreibung |
|------|-------------|
| `build/bin/neverd` | Einheitliche CLI |
| `build/bin/neverd-bench` | Benchmark-Harness (JSON) |
| `build/bin/neverd-sigmaker` | `.pat`-Generator aus statischen Bibliotheken |
| `build/bin/libneverd.*` | Shared Library der Engine |
| `build/bin/sdk/` | Kanonischer Include-Root des C SDK; `<neverd/sdk/NeverDCAPI.h>` oder `<neverd/sdk/NeverDPlugin.h>` unter Beibehaltung der Hierarchie `neverd/sdk/` verwenden |
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
| `decompile` | `.c` / `.sol` / `.rs` | C, EVM-Solidity oder SBF-Rust über `--language` |
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
| `sym-explore` | Begrenzte Pfaderkundung für natives LowIR (`--func`; JSON-Ausgabe) |
| `audit` | Heap-Lebensdauerfehler: Leak, Double-Free, Use-after-Free (JSON) |
| `hunt` | Gefährliche Copy-Überläufe mit symbolischer Evidenz und Kandidatenwerten (JSON) |
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

Für EVM wählt `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)`
Solidity explizit; `neverd_decompile_all` gibt weiterhin C aus. Siehe die
[EVM-C-API-Beispiele](../evm.de.md#c-api).

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
