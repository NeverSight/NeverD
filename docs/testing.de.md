**Sprachen**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← Dokumentationsindex](README.de.md)

# NeverD testen

NeverDs Tests beantworten drei verschiedene Fragen: Hat eine Darstellung die
erwartete Form, funktioniert ein vollständiger Pipeline-Pfad für ein binäres
Fixture und bewahrt generierter Code das Verhalten? Wählen Sie die kleinste
Suite, die die Frage der Änderung beantwortet, und führen Sie vor einem
risikoreichen Pull Request das breitere Aggregat aus.

## Test-Build konfigurieren

Tests sind deaktiviert, sofern `BUILD_TESTING` nicht aktiv ist. Release ist die
normale Wahl für die vollständige Suite; Debug erhält Assertions und
Schrittbetrieb, ist aber bewusst unoptimiert und für Decode-Benchmarks nicht
repräsentativ.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

Der vollständige Fixture-Satz benötigt `clang` für zielübergreifende
Kompilierung und die LLVM-Linker (`ld.lld` und `lld-link`) im `PATH`. CMake baut
viele relocatable Fixtures immer und gelinkte ELF-/PE-Fixtures, wenn der
passende Linker vorhanden ist. Ein übersprungener Test, dessen Fixture der Host
nicht kompilieren oder linken kann, ist nicht ausgeführte Abdeckung und kein
bestandener Zielpfad.

Klonen, Build-Profile und vorgefertigtes LLVM unter macOS beschreibt
[CONTRIBUTING.md](i18n/CONTRIBUTING.de.md).

## Testaufteilung

`add_neverd_unittest` erzeugt ein GoogleTest-Programm und weist jedem
gefundenen Fall ein CTest-Label mit dem Namen dieses Executable-Targets zu.

| Quellbereich | Target und CTest-Label | Abdeckung |
|--------------|------------------------|-----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Plattformübergreifende Kindprozesse, Quoting, Umleitungen und Exitcodes |
| `unittests/libc` | `NeverDLibCTests` | Bekannte libc-Namen und Klassifizierung |
| `unittests/lift` | `NeverDLiftTests` | Decoder-/Lifter-LowIR-Formen, IR-Stufen, Loader, Relokationen, Format-Fixtures, Dekompilierung und repräsentative Patch-Flows |
| Die meisten Dateien in `unittests/semantic` | `NeverDSemanticTests` | Differentielle Semantik von Instruktionen, ABI, Kontrollfluss, C-Ausdrücken und Lift/Recompile |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFSemanticTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFEmitterTests`, `NeverDSBFIntegrationTests` | v0-v4-Metadaten und ELF-Layouts, strikte Verifikation, CFG/Recovery, unabhängige Raw-Ausführung, LLVM-Verifikation, C-/Rust-Kompilierung und Routing der öffentlichen API |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Rewrite-/Obfuskationsäquivalenz über vier ISAs und drei Objektformate |
| Fokussierte Transformationsdateien in `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Schnell relinkbare Sonden außerhalb des großen Semantikprogramms |

Die Registrierungsquellen sind
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) und
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt) und
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt).

## Erzeugung der Fixtures

### Lift- und Format-Fixtures

`unittests/lift/CMakeLists.txt` kompiliert C- und Assembly-Quellen während des
Builds für mehrere Ziele. Clang-Triples erzeugen x86-64-, i386-, AArch64- und
ARM32-ELF-Objekte, PE-/COFF-Objekte und gelinkte Images sowie PIC-/No-PIC-
Mach-O-i386-Objekte. Ist LLD verfügbar, werden ausgewählte Objekte außerdem zu
Executables für Patch-Tests gelinkt. `NeverDLiftTests` hängt vom Target
`lift-test-objects` ab; ein normaler Build dieses Testprogramms erneuert somit
die generierten Fixtures.

Die meisten Lift-Tests rufen über `NeverDLiftFixture.h` die gebaute `neverd`-CLI
auf und prüfen LowIR, MedIR, HighIR, LLVM IR, generiertes C oder ein
umgeschriebenes Binary. Für ein fokussiertes manuelles Experiment kann die
Umgebungsvariable `NEVERD` den CLI-Pfad überschreiben; normale CTest-Läufe
verwenden das von CMake eingebettete Executable.

### Differentielle Unicorn-Roundtrips

Das Semantik-Fixture prüft Verhalten statt Textform:

1. Einen kleinen C-/Assembly-Fall schreiben oder LLVM IR erstellen.
2. Mit Clang/LLVM für das angeforderte Ziel kompilieren.
3. Ursprünglichen Maschinencode in Unicorn ausführen und erwarteten Rückgabewert oder anderen Fixture-Zustand erfassen.
4. Mit NeverD laden und liften, LLVM IR ausgeben und das Ergebnis zurück in Maschinencode kompilieren.
5. Regenerierten Code mit gleicher ABI, Eingaben, Speicheranordnung und gleichem CPU-Modell ausführen.
6. Beobachtbare Ergebnisse vergleichen.

Die Hauptimplementierung ist
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h).
Das Patch-Full-Fixture verwendet `Codegen::compileForRewrite`, dasselbe
Rewrite-Backend wie Patch-Operationen, und vergleicht danach Basis- und
Transformationscode über das vollständige 4×3-ISA-/Format-Raster.

Ein deterministischer NeverD-Semantikfehler soll ein fehlgeschlagener Test sein.
Skips sind expliziten externen Fähigkeitsgrenzen vorbehalten; lesen Sie die
Begründung. Eine grüne Zusammenfassung ohne Cross-Linker beweist nicht, dass der
Formatpfad lief.

### Differentielle Solana-SBF-Backends

Die SBF-Metadatentests prüfen jedes Versionsmerkmal, Opcode-Kollisionsgrenzen, Murmur3-Syscall-Hashes, Relokationen, ELF-Machine-, Register- und VM-Adresskonstanten. Loader-Fixtures erzeugen ohne eingebundene Binärdateien sowohl ältere v0-v2-Section-Layouts als auch sectionlose, strikte v3/v4-Program-Header-Layouts.

`NeverDSBFSemanticTests` führt verifizierte Instruktionsbytes direkt aus und verwendet kein MedIR. Eine Änderung oder Beschädigung der normalisierten IR kann daher nicht versehentlich dazu führen, dass Source-Oracle und Backend übereinstimmen. Abgedeckt werden nicht-monotone v2-Semantik, Speicher, Syscalls, interne Call-Frames, Faults, Traces und Ressourcenlimits. LLVM-Module werden verifiziert; generiertes C wird mit Warnungen als Fehler und Rust mit `-D warnings` kompiliert. Tests der öffentlichen API durchlaufen ausgehend von einem generierten strikten SBF-ELF alle IR-Stufen, Disassembly, CFG, Metadaten, LLVM, C und Rust.

## Einmalziele

Benutzerdefinierte Targets bauen ihre Abhängigkeiten und starten dann CTest mit
aus der Host-CPU abgeleiteter Parallelität:

| CMake-Target | Auswahl |
|--------------|---------|
| `check-neverd` | Alle registrierten Tests |
| `check-neverd-semantic` | Nur `NeverDSemanticTests` |
| `check-neverd-sbf` | Alle `NeverDSBF*Tests`-Targets/-Fälle |
| `check-neverd-patch-full` | Nur `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | Nur `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | Nur `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | Nur `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` und `NeverDAvxUpperXformTests` haben derzeit kein
Komfortziel `check-neverd-*`. Bauen und wählen Sie sie wie unten per Label.
`check-neverd-semantic` enthält auch nicht die separaten Transformations- oder
Patch-Full-Programme; verwenden Sie `check-neverd` für das vollständige
Aggregat.

## Inkrementeller CTest-Ablauf

Bauen Sie zuerst das zuständige Executable und wählen Sie dann sein Label. So
vermeiden Sie das Relinken unbeteiligter großer Semantikziele.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# Alle fokussierten Solana-SBF-Targets/-Fälle
cmake --build build-release --target \
  NeverDSBFMetadataTests NeverDSBFLoaderTests NeverDSBFAnalyzerTests \
  NeverDSBFSemanticTests NeverDSBFLLVMEmitterTests NeverDSBFEmitterTests \
  NeverDSBFIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'SBF' --output-on-failure --parallel 4
```

Verwenden Sie einen aus GoogleTest abgeleiteten CTest-Namen für eine einzelne
Regression:

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

Nützliche Selektoren:

| Befehl | Zweck |
|--------|-------|
| `ctest --test-dir build-release -N` | Gefundene Fälle auflisten, ohne sie auszuführen |
| `ctest --test-dir build-release -L '<regex>'` | Testprogramm-Label auswählen |
| `ctest --test-dir build-release -R '<regex>'` | Fallnamen auswählen |
| `ctest --test-dir build-release --output-on-failure` | Diagnosen nur bei Fehlern zeigen |
| `ctest --test-dir build-release --stop-on-failure` | Nach dem ersten Fehler anhalten |
| `ctest --test-dir build-release --parallel 4` | Bis zu vier Fälle parallel ausführen |

GoogleTest-Discovery nutzt `DISCOVERY_MODE PRE_TEST`; das zugehörige
Testprogramm muss vor der CTest-Aufzählung existieren. Fall-Timeouts und separate
Discovery-Timeouts stehen in `cmake/AddNeverD.cmake` und dürfen nur für Suiten
mit gemessenen schweren Fällen erweitert werden.

## Welche Tests ändern sich mit Code?

| Änderungsbereich | Zuerst | Danach erwägen |
|------------------|--------|----------------|
| Architektur-Lifter oder decode | Benannter Fall in `NeverDLiftTests` | Passender ISA-Semantik-Roundtrip |
| LowIR-CFG, Funktionserkennung, Sprungtabellen | Lift-CFG-/Switch-Fälle | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests` oder `NeverDTwoTableXformTests` |
| MedIR, ABI, Flags, Typen, SSA | MedIR-/Aufrufkonventions-Lift-Fälle | ISA-übergreifende `NeverDSemanticTests`-Fälle |
| HighIR oder strukturiertes C | HighIR-/Decompile-Fälle | `NeverDCFGLoopXformTests` und Kompilierungsprüfungen des generierten C |
| PE-/ELF-/Mach-O-Loader oder Eingaberelokation | Passendes Format-Fixture in `unittests/lift` | Alle-Stufen-Lade-/Dekompilationstest der Zelle |
| Rewrite-Codegen oder Ausgaberelokation | `RewriteCodegenRTTests`-Fälle | `NeverDPatchFullTests` und gelinktes Patch-Fixture, falls verfügbar |
| Von Patch genutzte LLVM-IR-Transformation | Fokussiertes Transformationsprogramm | Kombiniertes Pass-Raster `NeverDPatchFullTests` |
| C-API oder CLI | Direkter SDK-/Query-Test und `unittests/semantic/CLIEndToEndTests.cpp` | Relevante Pipeline-/Formatsuite |
| SBF-Loader, ISA, IR oder Backend | Kleinstes zuständiges `NeverDSBF*Tests`-Target | Alle SBF-Targets plus Kompilierung des generierten C/Rust |
| Libc-Erkennung | `NeverDLibCTests` | Semantische Call-/ABI-Fälle bei Verhaltensänderung |
| Prozessausführung oder Quoting | `NeverDTestProcessTests` | Ein betroffener CLI-/Semantikfall je unterstütztem Host |

Tests sollen den Vertrag an der niedrigsten stabilen Grenze ausdrücken. Ein
LowIR-Formtest ist für Lifter-Zuordnung nützlich; ein Semantik-Roundtrip ist
erforderlich, wenn sich zwei plausible IR-Formen unterschiedlich verhalten
könnten. Vermeiden Sie Golden Dumps ganzer Funktionen, wenn eine kleine Opcode-,
CFG- oder Beobachtungszustands-Assertion genügt.

## Beziehung zur CI

Die CI baut Release mit aktivierten Tests unter Linux, macOS und Windows,
prüft das gefundene Inventar und wendet danach plattformspezifische
Label-Ausschlüsse an. Die Profile stehen in `.github/workflows/ci.yml` und
`scripts/audit_ci_test_inventory.py`. Da kein einzelner Matrix-Shard alle
teuren Suiten darstellt, bleibt ein lokales `check-neverd` auf einer Maschine
mit allen Cross-Werkzeugen das klarste vollständige Signal vor dem Merge.
