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
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Senkenkatalog, Identitätsvorrang, Argument-Vorfilter, Copy-Überlauf-Hunt, Heap-Lebensdauer-Audit und die verpflichtende Sechs-Zellen-Matrix PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/lift` | `NeverDLiftTests` | Decoder-/Lifter-LowIR-Formen, IR-Stufen, Loader, Relokationen, Format-Fixtures, Dekompilierung und repräsentative Patch-Flows |
| Die meisten Dateien in `unittests/semantic` | `NeverDSemanticTests` | Differentielle Semantik von Instruktionen, ABI, Kontrollfluss, C-Ausdrücken und Lift/Recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Hardfork-Metadata, Eingabenormalisierung, ABI-/Signatur-Ambiguität, CFG/SSA/Recovery, exhaustive Decoder-Grenzen und feindliche Eingaben, Proxy-/Call-Fakten, Interpreter-Semantik, differentielle LLVM/C/Solidity-Ausführung und API-Routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | v0-v4-Metadaten und ELF-Layouts, striktes Verifier-/Loader-Verhalten, 23 gepinnte ELF-Artefakte, unabhängiges offizielles Oracle, vollständige Opcode-Verfügbarkeit, feindliche Eingaben, CFG/Recovery sowie ausgeführte LLVM-/C-/Rust-Differenzen |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Rewrite-/Obfuskationsäquivalenz über vier ISAs und drei Objektformate |
| Fokussierte Transformationsdateien in `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Schnell relinkbare Sonden außerhalb des großen Semantikprogramms |
| `unittests/corpus` (Submodul) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests` | Exception- und Runtime-Metadaten aus 317 per Digest fixierten echten Binärdateien, jede mit einem Manifest, das die Untergrenzen ihrer Wiederherstellung nennt |

Die Registrierungsquellen sind
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) und
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) und
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt) und
[`unittests/safety/CMakeLists.txt`](../unittests/safety/CMakeLists.txt).

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
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus`,
`check-neverd-cxx-itanium-eh-corpus` und `check-neverd-objc-eh-corpus` jeweils
eine. Alle drei CI-Hosts konfigurieren mit dem Flag und fahren alle fünf Linien:
Die Bytes sind überall identisch, was sie liest jedoch nicht, und ein
Corpus-Lauf auf einem Host beweist nichts über die anderen beiden.
`scripts/audit_ci_test_inventory.py` weist ein Inventar zurück, dem eines der
fünf Labels fehlt, denn ein Build, der das Corpus stillschweigend nicht mehr
liest, ist eine Regression, die kein Test fangen kann — der Test ist ja das, was
abhandenkam.

Der Live-EVM-Opcode-Audit wird so ausgeführt:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

Lokal und in CI erzwingt der Standardpfad
`git fetch --depth=1 --force` von der offiziellen URL
`https://github.com/ethereum/go-ethereum.git` und prüft ausschließlich
den frisch vom Remote-`HEAD` des Default-Branches geholten exakten SHA in einem
detached Worktree. Jeder Lauf verwendet ein unvorhersagbar benanntes privates temporäres
Bare-Repository, hält die vom Fetch gelieferte authority ref und ihren exakten
SHA über die Lebensdauer des detached Worktree und vernichtet danach Repository
und Worktree gemeinsam. Es gibt kein gemeinsam genutztes dauerhaftes
Git-Repository und keinen Cache. `local_docs`, ein vorhandener Checkout
und ein Submodule sind keine Audit-Pfade, denn ein Submodule-Pin wäre gerade
beim Erkennen von Live-Drift veraltet.

Jeder Git-Befehl entfernt zuerst alle geerbten `GIT_*`, einschließlich
`GIT_CONFIG_*`, und setzt danach nur geprüfte Werte. `GIT_CONFIG_NOSYSTEM` und
`GIT_CONFIG_GLOBAL` deaktivieren System-/globale Konfiguration;
`GIT_ATTR_NOSYSTEM` und das befehlslokale `core.attributesFile` deaktivieren
System-/globale Attribute, `core.hooksPath` deaktiviert Hooks. Unerwartete
Konfiguration des privaten Repository, Grafts,
`objects/info/alternates` und `refs/replace` lassen die Prüfung scheitern;
`GIT_NO_REPLACE_OBJECTS` deaktiviert Replacement-Lookup.

Der Probe reflektiert alle exportierten booleschen
Felder von `params.Rules`, ruft `LookupInstructionSet(params.Rules)` auf und
scannt alle 256 Slots. `EVMUpstreamOpcodePolicy.def` besitzt Aliase und typisierte
historische bzw. nicht eingeplante EOF-Ausschlüsse;
`EVMUpstreamSemanticsPolicy.def` besitzt das geschlossene Rules-Inventar,
Fork-Zuordnungen, Base-Stack-Ausnahmen und Dynamic-Immediate-Familien.

CI führt denselben Live-Audit nur bei Pushes nach `dev`, Pull Requests, manueller
Auslösung und täglich aus. Der Go-Probe ruft für jeden abgebildeten Fork die
öffentliche API `LookupInstructionSet(params.Rules)` auf.
Die öffentliche CLI bietet ausschließlich `--manifest-output`; das geschlossene
Manifest verwendet `schema 3`, und Quelle, Ref, Checkout sowie Toolchain sind
nicht wählbar.
`EVMUpstreamOpcodePolicy.def` verwaltet Namensaliase und geprüfte historische/
nicht eingeplante EOF-Ausschlüsse; die orthogonale `EVMUpstreamSemanticsPolicy.def`
verwaltet Forkregeln und Ausnahmen der Stacksemantik. Das geschlossene Manifest
prüft exakte Revision, Aktivierung, Byte/Name, `base_min_stack` und
`net_stack_delta` und lehnt unbekannte oder doppelte Felder, Forks, Namen und
Bytes ab. Die Belegung folgt ausschließlich `operation.undefined`; `HasCost`
ist nur eine Kosten-Gegenprüfung, da definierte Nullkosten-Operationen ebenfalls
false liefern. Jeder Slot `defined && !HasCost` muss ab seinem deklarierten
Aktivierungs-Fork exakt zu `EVM_GETH_ACTIVE_WITHOUT_COST` passen. Ein undefined
Slot mit Kosten, ein ungeprüfter defined Slot oder ein fehlender Marker schlägt
geschlossen fehl. Bei CI-Fehlern werden Revision, Manifest und Log als Artifact
hochgeladen. Fehlende, außerhalb des Wertebereichs liegende oder syntaktisch
nicht verbrauchte Deklarationen schlagen ebenfalls fehl: Jeder `.def parser`
verwirft `partial` Policy-Eingabe. Parser und Drift-Diagnosen besitzen
unabhängige Python-Abdeckung:

`EVMUpstreamSemanticsPolicy.def` ordnet jedes exportierte boolesche
`params.Rules`-Feld mit genau einem `EVM_GETH_RULE_FIELD` den Kategorien
`MappedForkSelector`, `NoOpcodeAllocation` oder
`ExcludedSelectorExpectedError` zu. Der Probe aktiviert jedes Feld einzeln über
`LookupInstructionSet`: Die ersten beiden Kategorien brauchen nil error, die
dritte error, und jeder vollständige 256-Slot-Opcode/Stack-Fingerprint muss
`ExpectedFork` entsprechen. `IsEIP155`, `IsEIP2929`, `IsEIP4762` und
`IsPetersburg` sind aktuell No-Allocation-Felder mit Frontier-Fingerprint;
`IsUBT` muss fehlschlagen und Cancun ergeben.

`EVMUpstreamSemanticsPolicy.def` deklariert die dynamischen EIP-8024-OpCode-
Familien, Operationsarten und gültigen Stack-Deltas;
`EVMEIP8024Immediates.def` besitzt getrennt die Immediate-Decodierung und
klassifiziert alle 256 Bytes der Single-/Pair-Inventare. Per `go -overlay` holt
das Audit die echten privaten `operation.execute`-Handler und prüft die
`canonical fork jump tables` sowie die `mainnet active/scheduled jump tables`
einzeln. Eine `inactive` Familie wird protokolliert, eine unvollständige Familie
ist ein Fehler. Jede aktive Tabelle führt `DUPN`, `SWAPN` und `EXCHANGE` für alle Bytes
(`3x256`) plus `3 missing-operand cases` aus und prüft Akzeptanz, PC-Delta,
Mutation, Underflow und fehlenden Operanden gegen dieselben deklarativen Daten.

`EVM_HARDFORK_LATEST` hat genau ein kanonisches Ziel. Das geschlossene
`EVMUpstreamForkAliases.def` mappt Prague→Pectra, Osaka und BPO1–BPO5→Fusaka
sowie Paris/Shanghai/Cancun/Amsterdam/Bogota auf sich selbst; unbekannte Namen
schlagen geschlossen fehl. Ein protokolliertes `audit_unix_time` steuert
`MainnetChainConfig.LatestFork(time)` (muss NeverD latest entsprechen) und die
Alias-/Probe-Prüfung für `LatestFork(max uint64)`; beide Instruction Sets werden
vollständig verglichen. Das Manifest fixiert `authority=official-fresh-fetch`,
offizielle URL, angefordertes `HEAD` und SHA. Der Probe nutzt
`GOTOOLCHAIN=local`.

Go-Probe und Python-Controller erzwingen `input/collection/string hard limits`;
übergroße Eingaben, Collections oder Strings schlagen geschlossen fehl. Für
`bounded diagnostic output` erhält eine überlange Anzeige den vollständigen
`digest` und einen `explicit truncated marker`. Begrenzte Ausgabe und eine
gemeinsame Frist gelten für jeden Kindprozess; bei Überschreitung wird die
gesamte `process group` beziehungsweise der process tree beendet und geleert.

Der aktuelle schema-3-Beleg enthält `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, Revision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, lokales `Go 1.24.0`,
`stack_limit=1024` und `diagnostics=[]`. Geprüft wurden `21 fork tables` und
`20 Rules probes` mit `15 mapped/4 no-op/1 expected-error`. Beide
`mainnet active/scheduled`-Einträge melden `upstream BPO2`, geschlossen auf
`NeverD Fusaka` gemappt. Von `23 table targets` sind nur `Amsterdam/Bogota`
aktiv; daraus folgen `1536 candidate executions` und
`6 missing-operand cases`. Die `three handler symbols` stimmen an beiden
aktiven Zielen überein. Python-Audit `67/67` und `C++ Opcode 10/10` bestanden.
Der echte macOS-Lauf war unter `sandbox-exec` erfolgreich, das finale `go run`
offline; der Linux-Workflow verlangt `bubblewrap`.

Alle Go-Stufen — `go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` und `go run` — passieren den `capability-root`-Dateisystem-
Sandbox. Er liest nur privaten Probe, frisches geth, validiertes
`resolved GOROOT` und exakt benötigte System-Runtime-Roots und schreibt nur in
isolierte Environment-Roots. Netzwerk erhalten nur notwendige Dependency-
Stufen; der finale Lauf ist offline. Tests verlangen Zugriffsverweigerung für
Sentinels im `host HOME/workspace` und verhindern deren Inhalt in jeder Ausgabe.
Linux prüft dieselbe `bubblewrap`-Policy ohne `/` broad bind.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

Die elf aktuell von CMake registrierten EVM-Testziele sind:

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` prüft für jeden decoder-verändernden Fork alle
Zwei-Byte-Eingaben, vollständigen Decode und exakte `JUMPDEST`-Grenzen sowie
deterministische feindliche Eingaben begrenzter Länge über alle Forks.

Führen Sie bei Änderungen am EVM-Kontrollfluss zuerst den Fixpunkt- und
Höhendomänenvertrag aus:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Diese Fälle decken blockübergreifende interne Returns, endliche Multi-Target-
Merges, Schleifenkonvergenz, deterministische Kantenreihenfolge, pfadsensitive
Whole-Stack-Lanes, erhaltene Korrelation, unbekannte Sprünge, exakt ungültige
Ziele, Fail-loud-Budgets und Stackfehler ab. `MayReachable` bewahrt nur einen
CFG-Kandidaten und erzeugt keine sicheren Fakten. Führen Sie danach alle elf
EVM-Ziele und den Live-Upstream-Audit aus.

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

### Speichersicherheits-Fixtures

`unittests/safety/fixtures/binaries` enthält eingecheckte PE-, ELF- und
Mach-O-Images für x86-64 und AArch64, dazu den PDB- oder dSYM-Begleiter, den das
jeweilige Format liefert, sowie eine Linker-MAP zu jedem Image. Die MAP ist das
Einzige, was ein gestripptes Build noch mitliefert; deshalb wird jede Zelle
zusätzlich mit explizit benannter MAP analysiert, was festschreibt, was ein
Befund noch behaupten darf, sobald weder Typen noch Quellzeilen übrig sind.
`NeverDSafetyIntegrationTests` führt alle sechs Zellen auf jedem Host aus; die
Konfiguration schlägt fehl, wenn ein benötigtes Image oder ein Begleiter fehlt,
und die Suite kennt keinen Übersprungpfad wegen der Host-Toolchain.

Die gleichwertigen Binaries stammen aus einer einzigen Quelldatei. Bauen Sie die
hosteigene Smoke-Fixture mit `make` neu, oder erzeugen Sie die vollständige
eingecheckte Matrix neu mit:

```bash
make -C unittests/safety/fixtures matrix
```

Das Matrix-Rezept benötigt Clangs Linux- und Windows-Cross-Targets, LLDs
COFF-Werkzeuge, beide Darwin-Architekturen und `dsymutil`. Seine Debug-Pfade
werden umgeschrieben und die CodeView-Kommandozeilenaufzeichnung ist
abgeschaltet, damit eingecheckte Begleiter nicht den absoluten Pfad des
Arbeitsbereichs einer Entwicklerin festhalten.

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

### Sprachspezifische Ausnahmemodelle

Alles, was nicht das Windows-Tabellenmodell ist, liegt in einem fokussierten
Target. `NeverDLanguageEHTests` deckt die DWARF-Frame-Kette, den
sprachspezifischen Itanium-Datenbereich, ARM EHABI, Darwin Compact Unwind, die
Frame-Metadaten der Go-Runtime, Rusts Panic-Maschinerie und die drei
Objective-C-Runtimes ab:

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

Die Tabellen dieser Suite werden Byte für Byte zusammengesetzt statt kompiliert,
denn die meisten der geprüften Kombinationen gibt kein einzelnes Toolchain
gemeinsam aus. Objective-C ist der deutlichste Fall: Alle drei Runtimes geben
eine Itanium-LSDA aus und unterscheiden sich nur darin, was in einem Slot der
Typtabelle steht — und dieser Unterschied ist vollständig, nicht graduell.
Apples Slot adressiert ein `objc_typeinfo`, dessen erste zwei Felder
`std::type_info` bewusst nachbilden; GNUsteps Objective-C++-Slot adressiert eine
echte `std::type_info`-Ableitung; und der Slot der GNU-Runtime ist überhaupt kein
Zeiger, sondern die Klassennamenszeichenkette selbst. Die Konvention einer
Runtime auf die Tabelle einer anderen anzuwenden schlägt nicht fehl — es meldet
einen Klassennamen, der mitten aus etwas anderem gelesen wurde. Deshalb wird die
Runtime aus der Personality des Frames bestimmt, bevor irgendein Slot gelesen
wird.

Dieselbe Suite fixiert zwei Unterscheidungen, die leicht zusammenfallen und
deren Zusammenfallen falsch ist. `@catch(id)` und `@catch(...)` sind
verschiedene Handler — der erste nimmt jedes Objective-C-Objekt und lässt eine
fremde Ausnahme daran vorbeiziehen — und jede Runtime schreibt sie anders. Ein
Decoder, der beide als Catch-all meldet, hängt einen Handler an Ausnahmen, die
tatsächlich vorbeigeflogen wären. Und eine setjmp/longjmp-Call-Site-Tabelle
indiziert Aufrufstellen statt Adressen: Ein Leser, der eine der
SJLJ-Personalities nicht erkennt, bricht nicht ab, sondern erfindet geschützte
Bereiche und Landing Pads, die das Programm nie benannt hat.

Diese Form zu erkennen ist nicht dasselbe, wie sie abzulehnen. Ein SJLJ-Eintrag
ist ein Paar von ULEB128-Werten — ein Dispatch-Selektor und ein Action-Offset —
und dieser Offset bedeutet dort genau das, was er in der Adressform bedeutet.
Die Action-Kette, die gefangenen Typen und die Ausnahmespezifikationen lassen
sich damit alle aus einer Tabelle lesen, die überhaupt keinen Code nennt.
Unbekannt bleibt allein der Bereich, den jeder Eintrag schützt, denn was ihn
angibt, sind die Schreibzugriffe der Funktion auf ihren eigenen Call-Site-Slot
und nichts in der Tabelle. Die Suite fixiert außerdem das eine Byte, dem hier
nicht zu trauen ist: GCC schreibt `DW_EH_PE_uleb128` als Call-Site-Kodierung,
LLVM schreibt `DW_EH_PE_udata4`, beide geben danach ohnehin ULEB128 aus, und
keine Personality liest es je — ein Decoder darf es also auch nicht.

Die Identität der Personality wird daneben festgehalten, weil sie entscheidet,
wie jede Tabelle darüber gelesen wird. GNAT benennt seine Routine auf die drei
Arten, auf die GCC die jedes Frontends benennt — `_v0`, `_sj0`, `_seh0` — und
registriert unter Windows das eine Symbol, während es an ein anderes
weiterleitet; alle vier Schreibweisen müssen deshalb bei Ada landen. D ist das
Spiegelbild: drei Compiler, drei Namen für eine Routine, dahinter ein einziger
Satz Tabellen.

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

Low-/Med-Tests bewahren pfadsensitive Whole-Stack-Execution-Lanes und die
Lane-Identität der Phis; erschöpfte Budgets einschließlich
`MaxAbstractInstructionTransfers` sind harte Fehler. Strict lehnt unbekannte oder
inaktive Opcodes nur auf einer bewiesenen `Reachable` Lane ab; `MayReachable`
erzeugt keinen bestimmten Fakt. HighIR beschränkt Selector-, Receive- und
Fallback-Walks auf die Root-Lane und erfolgreiche Endzustände. Ein geteilter
Selector ist kein unabhängiger Standardbeleg: Erst die standardbezogene
`KnownFunctionVariantInfo` und eine über alle erfolgreichen Enden übereinstimmende
exakte Return-Form wählen Variante und Return-Liste.

Der Interpreter führt die typisierte Stack-Vorprüfung vor jedem opcode-eigenen
Effekt aus. `EVMForkSemantics.def` definiert Byte `0x44` vor Paris als
`DIFFICULTY`, ab Paris als `PREVRANDAO`. `REVERT`, Faults, Step-Limit und
Ressourcenerschöpfung rollen Transaktionszustand zurück. Allokationsfehler sind
`ExecutionFaultKind::ResourceExhausted`; kann selbst der Eingabe-Snapshot nicht
entstehen, ist `HasPersistentStateSnapshot` false und ein Commit ausgeschlossen.

### Regressionen für öffentliche EVM-Grenzen und Budgets

Public-API-Tests manipulieren kanonische
`Code`/`Fork`/`Instructions`/`JumpDestinations` und jede LowIR-Tabelle, Range,
ID, Lane und Kantenreferenz unabhängig. `execute` muss vor der
Instruktionssuche `llvm::Error` liefern; `lowerToMedIR` muss vollständig
fehlerhaftes oder überbudgetiertes LowIR vor Indexaufbau oder inputproportionaler
Allokation ablehnen. Für `lowerToMedIR` erzwingen Tests Options-, Ressourcen- und
Strukturprüfung vor einem feldweisen `canonical decode replay` und vor
`lowerCanonicalLowToMedIR`. Öffentliches HighIR-Recovery replay-prüft externe
LowIR/MedIR; nur `analyze` darf für eigenes kanonisches IR
`lowerCanonicalLowToMedIR` und `recoverCanonicalHighIR` ohne rekursives oder
doppeltes Replay nutzen, muss aber alle HighIR option/resource budgets anwenden.
Danach prüfen Interpretertests alle Grenzen aus
`EVMInterpreterLimits.def` am exakten Rand und eins darüber. `MaxSteps` behält
den eigenen `StepLimit`; Erschöpfung von `MaxMemoryBytes`, `MaxTraceEntries`,
`MaxLogEntries`, aggregiertem `MaxLogDataBytes` und laufzeitigem
`MaxPersistentStateEntries` liefert `ResourceExhausted` und rollt
Transaktionseffekte zurück. Zu große initiale Aggregate unter
`MaxHostReturnDataBytes` oder Persistent State sind API-Fehler.
Auch `MaxCalldataBytes`, das Aggregat `MaxHostEnvironmentEntries` über
`BlockHashes`, `Balances`, `CodeHashes`, `ExternalCode`, `BlobHashes` und das
Aggregat `MaxExternalCodeBytes` führen zu API-Fehlern. Der
`const execute preflight` verwirft sie vor Environment-, Snapshot- oder
Result-Kopie. Return-Data-`ArrayRef`-Views und `lower_bound` auf der sortierten
Tabelle werden ohne
Bufferkopie oder PC-Map abgedeckt.

Separate LowIR-Randtests prüfen die aggregierten Diagnoselimits
`MaxLowDiagnostics` und `MaxLowDiagnosticBytes`: Linearer Decode und CFG-Aufbau
belasten exakte Anzahl/finale Bytes vor und Null wird verworfen.
HighIR-Sicherheitstests decken die sortierte Lane-Domäne
`Any/Exact/Excluded`, Equality-Treffer/Ausschluss, beim rohen
`XOR(selector, constant)` den False-Kanten-Treffer und True-Kanten-Nichttreffer,
die Verfeinerung von Nullwort/Calldata-Größe/Call Value und fail-closed
unbekannte Bedingungen ab. Ihre exakten Rand- und Eins-darunter-Tests prüfen aus
`EVMAnalysisLimits.def` `MaxHighDispatchCandidates`, das Aggregat
`MaxHighRecoveredArguments`, `MaxHighDiagnostics`, `MaxHighDiagnosticBytes`,
`MaxHighReferenceVisits`, `MaxHighMemoryTransferCells` und
`MaxHighMemoryValueVisits`. Jede ausgegebene Diagnose, auch die feste
Malformed-Diagnose, muss Anzahl und finale Bytes vor Allokation berechnen.
LowIR- und HighIR-Diagnosebudgets werden unabhängig geprüft; die Default-Root-
CFG-Region muss `MaxHighRegionBlockReferences` vor Reserve oder Block-PC-Kopie
belasten.
Function-Scope-Regressionen prüfen sowohl `EQ`- als auch `raw XOR`-Rücksprünge
in einen gemeinsamen Dispatcher. Dabei dürfen `arguments`, `mutability`,
`return shape` und `region` nicht durch eine andere Funktion verunreinigt
werden; gemeinsame Bodies und Tail Calls bleiben erreichbar.
Externe CALL/CREATE-Ergebnisse werden als nichtdeterministische Host-Outcomes
über beide präzisen CFG-Kanten geprüft, wodurch die ERC-1167-Fallback-Recovery
erhalten bleibt. Eine unlesbare Selector-Bedingung bleibt Unknown und kann keine
Fallback- oder Funktionsfakten erzeugen.

CFG-Tests leiten `InvalidJumpDestination` aus `EVMLowFaultKinds.def` für ein
`end-of-code JUMPI` ab: Sicher true mit ungültigem Ziel hat keinen erfolgreichen
Nachlauf und ist ein definitiver Fehler; sicher false ist erfolgreich; Unknown
behält den möglicherweise erfolgreichen False-Pfad, ohne die ganze Lane als
definitiv fehlerhaft zu markieren.

ABI-Tests prüfen die Grammatikgrenzen aus `EVMABIParserLimits.def` und die
Kardinalitäts-/Textgrenzen öffentlicher Tabellen aus `EVMABITableLimits.def`
am exakten Limit und eins darüber. Sie verwerfen außerdem ungültige
Kind-/Standard-/Evidence-Enums, unpassende Metadaten, nichtkanonische Signaturen
und Return-Listen, fälschlich unabhängige geteilte Selectors, hängende oder
doppelte Varianten sowie einen Event-Topic-`APInt` falscher Wortbreite vor
indizierter Selector- oder sortierter Topic-Suche.

`NeverDEVMOpcodeTests` erzwingt zudem die Metadata-Architektur: Jeder zugewiesene
Opcode roundtrippt zwischen Encoding und typisiertem Wert; Familiengrenzen,
Hardfork-Aliase sowie abgeleitete Stack-/Host-Maxima werden geprüft.

### Differentielle Solana-SBF-Backends

Die SBF-Metadatentests prüfen jedes Versionsmerkmal, Opcode-Kollisionsgrenzen, Murmur3-Syscall-Hashes, Relokationen, ELF-Machine-, Register- und VM-Adresskonstanten. Loader-Fixtures erzeugen ohne eingebundene Binärdateien sowohl ältere v0-v2-Section-Layouts als auch sectionlose, strikte v3/v4-Program-Header-Layouts.

`NeverDSBFISAConformanceTests` prüft für jede Version von v0 bis v4 jedes
Byte-Encoding gegen ein unabhängig auditiertes typisiertes Manifest.
`NeverDSBFExternalOracleTests` vergleicht anschließend Aktivierungs- und
Grenzentscheidungen mit einem separat gebauten offiziellen Anza-Prozess.
`NeverDSBFUpstreamConformanceTests` weist allen 23 ELF-Dateien am gepinnten
Anza-Stand ein explizites Ergebnis zu.

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
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Alle fokussierten Solana-SBF-Targets/-Fälle
cmake --build build-release --target check-neverd-sbf --parallel 4
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
| Heap-Lebensdauer-Audit oder Copy-Überlauf-Hunt | `NeverDSafetyTests` | Alle sechs Zellen in `NeverDSafetyIntegrationTests` |
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
`scripts/audit_ci_test_inventory.py`. `NeverDSafetyTests` und
`NeverDSafetyIntegrationTests` sind auf jedem Matrix-Host Pflicht; jeder Lauf
liest dieselben eingecheckten PE-, ELF- und Mach-O-Fixtures für x86-64 und
AArch64. Da kein einzelner Matrix-Shard alle teuren Suiten darstellt, bleibt
ein lokales `check-neverd` auf einer Maschine mit allen Cross-Werkzeugen das
klarste vollständige Signal vor dem Merge.

## Aktuelles Solana-SBF-Konformitäts- und Sanitizer-Profil

Diese aktuelle Liste ersetzt die kürzere SBF-Liste oben. Die Source-
Differential-Suite benötigt neben clang auch `rustc`; ein Compiler-Skip ist
fehlende Abdeckung. Das vollständige Aggregat enthält
`NeverDSBFProgramImageTests`, `NeverDSBFMalformedCorpusTests`,
`NeverDSBFISAConformanceTests`, `NeverDSBFUpstreamConformanceTests`,
`NeverDSBFLLVMDifferentialTests` und `NeverDSBFSourceDifferentialTests` sowie
die Metadata-/Loader-/Analyzer-/Semantic-/Emitter-/Integration-Targets. Das
integrierte Profil protokolliert benannte Targets und Ergebnisse statt einer
schnell driftenden Summenzahl.

Das Sanitizer-Profil wird separat in `build-sbf-asan-ubsan` gebaut. Die
fokussierten Targets laufen fail-fast ohne ASan- oder UBSan-Report; Integration
bleibt im integrierten LLVM-Build, weil dem Prebuilt-Paket der benötigte
fork-only Header fehlt.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```

### Gepinnter SBF-Evidenzsnapshot (2026-08-24)

Das Gate fixiert Anza `sbpf` auf
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave auf
`ef210d67f2fabeee1730498188fa78854260c679` und das Solana SDK auf
`122f32e571ce39face4beffaccea733e37c207fd`. Das offizielle ELF-Manifest
besteht 23/23; `NeverDSBFExternalOracleTests` vergleicht 1,411
Opcode-/Grenzfälle über `SBFOfficialOracleProtocol.def` und
`SBFOfficialVerifierCases.def` und `SBFOfficialExecutionConstants.def`.
`SBFOfficialELFMutations.def` ist der
tabellengesteuerte Malformed-ELF-Vertrag; seine wechselnde Gesamtzahl wird nicht
fixiert.
Getrennt führt das `41-case strict ELF differential` die vollständige
Strict-v3-Matrix durch offizielles `verify-elf-batch` und NeverD; diese 41 Fälle
gehören nicht zur Summe von 1,411.

Die zusätzliche offizielle Ausführungsmatrix bleibt getrennt: Genau 508 aktive
`(Version,Opcode)`-Fälle plus 58 Grenzfälle ergeben 566 exakte
Ausführungsfälle. Sie ersetzt weder die 1,411 Verifier-Probes noch das
`41-case strict ELF differential` und wird auf keine dieser Summen angerechnet.
`NeverDSBFAgaveConformanceTests` authentifiziert Firedancer test-vectors
`68bb4af40235562e8852fa23d5727e49c2a0b862` und gleicht alle 1,955 `sol_compat_elf_loader_v1` Loader-
Fixtures ab (1,399 akzeptiert, 556 verworfen). Für jedes akzeptierte ELF werden
`entry_pc`, `text_off`, `text_cnt`, `rodata_hash` und `calldests_hash`
verglichen. Dieses Gate führt den späteren Instruction-Verifier nicht aus.
Linux Release CI nutzt `--print-pinned-revision`,
`--print-test-vectors-revision` und `--print-toolchain` und exportiert
`NEVERD_SBPF_ORACLE` sowie `NEVERD_AGAVE_CONFORMANCE_ROOT`; damit sind beide
externen Gates Pflicht. Lokal werden die Fälle ohne explizite Oracle-/Corpus-
Umgebung entdeckt, dürfen aber überspringen.

`SBF_RUNTIME_VERSION` macht `RuntimeVersionPolicy::ChainProfile` historisch
cluster-/slotabhängig: offizielle Feature-Accounts schalten das maximale ISA
von V0 über V1 und V2 auf V3; aktuell bleibt V3. Explizites v4 nutzt
`RuntimeVersionPolicy::UpstreamToolchain` für Offline-
Analyse. Die aktuelle 10-MiB-Grenze ist exakt `10'485'760` Byte; 65,536 ist nur
historische Provenienz/Testdatum. `SBFFaultCodes.def` stabilisiert Execution-
Fault-Werte, `SBFSourceStatuses.def` getrennt die Generated-Source-ABI.

10,000-Skalierungsfixtures schützen Worklist, Function Ownership und
Multi-Latch-Verhalten ohne eine Maschinenzeit zu fixieren. Cluster-/Account-/
Slot-Zeilen ermöglichen einen `RPC activation audit`, während normale Tests
deterministisch und offline bleiben.
