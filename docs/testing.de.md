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
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMAnalyzerTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Hardfork-Metadata, Eingabenormalisierung, CFG/SSA/Recovery, Interpreter-Semantik, differentielle LLVM/C/Solidity-Ausführung und API-Routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFSemanticTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFEmitterTests`, `NeverDSBFIntegrationTests` | v0-v4-Metadaten und ELF-Layouts, strikte Verifikation, CFG/Recovery, unabhängige Raw-Ausführung, LLVM-Verifikation, C-/Rust-Kompilierung und Routing der öffentlichen API |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Rewrite-/Obfuskationsäquivalenz über vier ISAs und drei Objektformate |
| Fokussierte Transformationsdateien in `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Schnell relinkbare Sonden außerhalb des großen Semantikprogramms |
| `unittests/corpus` (Submodul) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests` | Exception- und Runtime-Metadaten aus 305 per Digest fixierten echten Binärdateien, jede mit einem Manifest, das die Untergrenzen ihrer Wiederherstellung nennt |

Die Registrierungsquellen sind
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) und
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) und
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt).

### Das fixierte Binär-Corpus

Jede andere Suite baut selbst, was sie prüft. Das Corpus nicht: Es ist ein
Submodul aus Binärdateien, die echte Toolchains auf Hosts und für Ziele erzeugt
haben, die dieses Repository nicht erreicht. Jede ist per Digest fixiert, und
daneben nennt ein Manifest die Untergrenzen, die ihre Wiederherstellung
überschreiten muss. Nur dort ist eine Aussage darüber, was NeverD etwa aus einem
mit `-O2` gebauten, gestrippten `armv7`-Shared-Object liest, beantwortbar statt
strittig.

Die Suites entstehen nur, wenn der Configure-Schritt angewiesen wurde, nach
ihnen zu suchen — dieses Flag ist also alles, was sie unter Test hält:

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` führt jede Linie aus; `check-neverd-windows-eh-corpus`,
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus` und
`check-neverd-cxx-itanium-eh-corpus` jeweils eine. Alle drei CI-Hosts
konfigurieren mit dem Flag und fahren alle vier Linien: Die Bytes sind überall
identisch, was sie liest jedoch nicht, und ein Corpus-Lauf auf einem Host
beweist nichts über die anderen beiden. `scripts/audit_ci_test_inventory.py`
weist ein Inventar zurück, dem eines der vier Labels fehlt, denn ein Build, der
das Corpus stillschweigend nicht mehr liest, ist eine Regression, die kein Test
fangen kann — der Test ist ja das, was abhandenkam.

Der EVM-Opcode-Audit führt bei jedem Lauf einen flachen `git fetch` des Remote-
`HEAD` aus dem offiziellen
[go-ethereum-Repository](https://github.com/ethereum/go-ethereum) aus und meldet
anschließend den exakt geprüften Commit. Er verwendet den ignorierten Bare-Cache
`build/evm-opcode-audit/go-ethereum.git` erneut, aktualisiert ihn jedoch vor dem
Lesen des geschlossenen Opcode-Inventars und der Byte-Zuordnungen:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

CI führt denselben Live-Audit bei jedem Push und Pull Request, bei manueller
Auslösung und einmal täglich aus. So wird Upstream-Drift auch ohne NeverD-Änderung
erkannt. Wählen Sie für eine Offline- oder historische Reproduktion ausdrücklich
einen vorhandenen Checkout:

```bash
python3 scripts/audit_evm_opcode_metadata.py \
  --geth-root /path/to/go-ethereum
```

Der Audit erlaubt ausschließlich die in `EVMUpstreamOpcodePolicy.def`
benannten Ausschlüsse. Ein Upstream-Opcode, der weder repräsentiert noch
ausdrücklich geprüft wurde, lässt den Befehl fehlschlagen. Parser und
Drift-Diagnosen besitzen unabhängige Python-Unit-Abdeckung in CI:

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

Führen Sie bei Änderungen am EVM-Kontrollfluss zuerst den Fixpunkt- und
Höhendomänenvertrag aus:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Diese Fälle decken blockübergreifende interne Returns, endliche Multi-Target-
Merges, Schleifenkonvergenz und deterministische Kantenreihenfolge,
pfadabhängige Stack-Höhen, beschränktes Widening, korrelationsbedingte
kartesische Over-Approximation, unbekannte Sprünge, präzise ungültige Ziele und
Stackfehler im strikten wie im entspannten Modus ab. Führen Sie anschließend
alle sieben EVM-Binaries und den Upstream-Metadata-Audit aus; CFG-Änderungen
können Emitter und Integration beeinflussen, selbst wenn die lokale
Analyzer-Form korrekt ist.

Führen Sie bei MedIR-/HighIR-Dataflow-Änderungen außerdem die Verträge für
Constant-Phis, Selector, typisierte Operanden, fehlerhafte Graphen und tiefe
Ketten aus:

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

Diese Fälle beweisen gleiche und widersprüchliche zyklische Phis, nicht
benachbarte und blockübergreifende Selector-Ausdrücke, beide Reihenfolgen der
Gleichheitsoperanden, exakte ABI-Breitenprüfungen, typisierte Storage-/Event-/
Calldata-Operanden, deterministische Behandlung fehlerhaften MedIR und einen
iterativen Producer-Walk über 16.384 Werte.

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

### Windows-Ausnahmerekonstruktion

Änderungen an tabellenbasierten Windows-Ausnahmen benötigen sowohl
Repräsentationstests als auch einen Patch-Test mit einer gelinkten PE-Datei.
Der fokussierte Lift-Filter deckt das normalisierte Unwind-/SEH-/C++-Modell,
beschädigte Eingaben, außergewöhnliche CFG-Kanten, HighIR, LLVM-WinEH-Erzeugung,
den Austausch des Ausnahmeverzeichnisses sowie die Rekonstruktion von Guard CF
und Guard EH Continuation ab:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

Das geschützte x64-Assembly-Fixture benötigt Clangs Windows-Target und
`lld-link`; der CMake-Link verwendet `/guard:cf` und `/guard:ehcont`. Ein Skip
wegen eines fehlenden Cross-Linkers ist kein Nachweis für den Final-Image-Pfad.
Ein erfolgreicher Integrationstest beweist, dass das umgeschriebene PE erneut
geladen werden kann und seine Runtime-Function-, Unwind-, Load-Config-, Guard-CF-
und Guard-EH-Continuation-Tabellen sortiert, dateigestützt und auf ausführbare
Ziele beschränkt bleiben.

Das gelinkte FH3-Fixture prüft den nativen C++-Abschluss unabhängig: feste
Zustandstabellen, HighC-Anmerkungen, Erhalt der Personality, erzeugte Catch-Ziele
und den erneut geladenen IP-to-State-Graphen.

Siehe [Windows-Ausnahmerekonstruktion](windows-exception-reconstruction.de.md)
für die Analyse-/Native-Supportmatrix und den Fail-Closed-Patch-Vertrag.

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

### Differentielle EVM-Backends

Interpretertests bilden einen deterministischen 256-Bit-Oracle. Die Emitter-
Suite kompiliert und führt LLVM aus, übersetzt C23 mit Clang für denselben Host-
Harness und deployt bei vorhandenem `solc`, `anvil`, `cast` und `jq` erzeugtes
Solidity lokal. Verglichen werden Status, Storage und Trace-Zähler. Ein separates
Raw-Bytecode-Corpus führt Pre-Fusaka-ALU, Calldata-/Memory-Copy, überlappendes
`MCOPY`, Keccak und Return Data in Anvils nativer EVM aus.

`NeverDEVMOpcodeTests` erzwingt zudem die Metadata-Architektur: 150 Opcodes
roundtrippen zwischen Encoding und typisiertem Wert; Familiengrenzen,
Hardfork-Aliase sowie abgeleitete Stack-/Host-Maxima werden geprüft.

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

# Alle fokussierten EVM-Targets/-Fälle
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMAnalyzerTests NeverDEVMSemanticTests NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

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
| EVM-Loader, Opcode, IR oder Backend | Kleinstes zuständiges `NeverDEVM*Tests`-Target | Alle EVM-Targets plus Kompilierung des generierten C/Solidity |
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

## Aktuelles Solana-SBF-Konformitäts- und Sanitizer-Profil

Diese aktuelle Liste ersetzt die kürzere SBF-Liste oben. Die Source-
Differential-Suite benötigt neben clang auch `rustc`; ein Compiler-Skip ist
fehlende Abdeckung. Das vollständige Aggregat enthält
`NeverDSBFProgramImageTests`, `NeverDSBFMalformedCorpusTests`,
`NeverDSBFISAConformanceTests`, `NeverDSBFUpstreamConformanceTests`,
`NeverDSBFLLVMDifferentialTests` und `NeverDSBFSourceDifferentialTests` sowie
die Metadata-/Loader-/Analyzer-/Semantic-/Emitter-/Integration-Targets. Das
integrierte Profil besteht 145/145 Fälle in 14 Binaries.

Das Sanitizer-Profil wird separat in `build-sbf-asan-ubsan` gebaut. Es besteht
141/141 Core-Fälle in 13 Binaries ohne ASan- oder UBSan-Report; Integration
bleibt im integrierten LLVM-Build, weil dem Prebuilt-Paket der benötigte
fork-only Header fehlt.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```
