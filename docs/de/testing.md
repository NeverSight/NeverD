**Sprachen**: [English](../testing.md) | [简体中文](../zh-CN/testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](../ja/testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

`NeverDLLVMCValueTests` kompiliert und führt den umgewandelten Varargs-Aufruf mit einer separaten Windows-x64-ABI-Funktion bei O0/O2 und beiden Typbeibehaltungsoptionen aus. Native x64-Hosts prüfen den Wert mit va_arg; andere Architekturen überspringen diese Ausführung ausdrücklich.

[← Dokumentationsindex](README.md)

# NeverD testen

`PEFunctionAddresses.*` prüft x64-PE-Blattfunktionen ohne Stackframe, Eigentümerschaft und Instruktionsgrenzen, abgewiesene Präfixe und schmale Operanden, Ausgänge, Abbruch und gleiche Ergebnisse mit 1/2/4/16 Workern. Signaturtests decken alle 16 rel32-Bedingungen in beiden Richtungen, explizite Einstiegsbereiche und Quellrelokationen ab. Python-Tests bewahren Alternativen und gemeinsam notwendige Stellen, prüfen jeden ursprünglichen Gegenbeleg und vermeiden doppelte Ansprüche.

`DriverKernelSEH.V2*`, `COFFExceptionParser.*V2*` and `DriverSEHV2Execution.*` cover normalized V2 tables, partial restoration, malformed metadata and original linked driver exceptions. Run `NeverDNativeDriverTests`, `NeverDDriverEmulationTests`, `NeverDWindowsProcessTests` and `NeverDCOFFExceptionV2Tests`; unavailable host backends remain explicit skips.

`DriverMSRExecution.*` und `DriverMSRDeclaration.*` prüfen explizite MSR-Werte, vollständige Aufteilung, Zustandserhalt, Wiederholungen, exakte Budgets, fehlende/ungültige Angaben und fehlende Speicherrechte auf verfügbaren Backends. `KernelMSRRead.*` erhält ungenutzte Abhängigkeiten. Native Treiber-, Treiberemulations- und Kernelsnapshot-Suiten ausführen; nicht verfügbare WHP/HVF-Läufe sind übersprungen, nicht nativ qualifiziert.

NeverDs Tests beantworten drei verschiedene Fragen: Hat eine Darstellung die
erwartete Form, funktioniert ein vollständiger Pipeline-Pfad für ein binäres
Fixture und bewahrt generierter Code das Verhalten? Wählen Sie die kleinste
Suite, die die Frage der Änderung beantwortet, und führen Sie vor einem
risikoreichen Pull Request das breitere Aggregat aus.

`PipelineOutcome.ResolvedCalls*` prüft Registergruppen, call/invoke-Identität, Zusammenfügen und Aufteilen von Stackbytes, schmale Werte in ihren eigenen Plätzen sowie die Ablehnung fehlender Bytes. `MedCallABI.AnIndirectCallRecordsTheSlotOfEachNarrowStackValue` prüft die aufgezeichneten Offsets. `MedCallABI.CallbackTableAddressDoesNotReplaceTheActualFirstArgument` deckt x64, ARM32 und AArch64 für ELF, PE und Mach-O ab. `LowInstructionBoundary.AArch64ImportedCopyNeedsExactLengthSourceOwnerAndAllPaths` prüft alle drei Formate und verwirft überschriebene Längen, beschreibbare oder zu große Bereiche, gleichnamige lokale Funktionen, falsche Quellregister, alternative Einstiege und verschiebbare Objektdateien. C-Differenztests kompilieren die Referenz separat mit O0 und den erzeugten C-Code mit O2, damit ein fehlerhaft optimierter Rekurrenzwert des Hostcompilers die Referenz nicht verfälscht. Der AArch64-FMA-Test fordert `-mfma` nur auf x86-Hosts an.

## Tests des klassischen GS-Zustands

`NeverDX86LegacyGSRecoveryTests` prüft universelle Nachbedingungen von Austausch/Doppelaustausch, vollständige segmentierte Adressen, Aliase, Erhaltung von FS/Speicher/Flags, MXCSR-Koexistenz, fehlerhafte Zustände und Temporärplätze, atomare Ablehnung, erhaltene Effekte und Budgets. `PEFixedImageTest.RawLegacyGS*` prüft native Zulassung und Belegbindung. Maschinenzustands- und Native-/LLVM-Verfeinerungstests lehnen nicht unterstützte Quellverträge auch bei ungenutzten Deklarationen ab. `LegacyGSExecutionPolicy.RemainsUnmodeled` erhält die konkrete Ablehnung. Dies sind symbolische und Richtlinientests, keine native Ausführung privilegierter Befehle.

## Prüfungen der MXCSR-Wiederherstellung

`InterpreterSpecialization.MXCSR*` prüft beliebige Anfangszustände, exakte Speicherwerte, reservierte Bits, DAZ, Aliase, ungültige Formen, Knotenprojektion und Budgetgrenzen. `PEFixedImageTest.RawMXCSR*` prüft Legacy/VEX-Bytes und maskengebundene Nachweise. Maschinenzustands- und native/LLVM-Verfeinerungstests verhindern, dass ungenutzte Fähigkeiten die Schnittstellen erweitern.

## Tests für segmentierte Schreibzugriffe

`InterpreterSpecialization.SegmentedWrite*` prüft unabhängige Byte-Speicherergebnisse, alle Skalarbreiten, Adressüberlauf, Frame-Aliase, überschriebene Zeigerherkunft, knotenübergreifende Invalidierung und genaue oder zu kleine Budgets. `SegmentedWritesAndReadsShareLinearMemory` deckt gleiche und verschiedene Basisadressen ab. `PEFixedImageTest.RawSegmented*` prüft native Bytes, die Ablehnung von Rückkehr-Aliasen und geänderte Fähigkeitsnachweise. Bestehende Kompatibilitätswerte bleiben erhalten. Die Tests für Spezialisierung, feste PE-Abbilder, Maschinenzustand und native/LLVM-Verfeinerung bauen; Ablehnungsprüfungen erfassen auch ungenutzte Schreibfähigkeiten.

## Konkrete Eingangsspeicher-Seeds

`NeverDSymbolicTests`, `NeverDConcolicTests` und `NeverDConcolicIntegrationTests` ausführen. `LowIRConcolicMemory.*` und symbolische Tests prüfen beide Byteordnungen, Alias-Stores, getrennte Bänke, fehlende/ungültige Bereiche, Reset und verspätete Seeds, Budgetgrenzen, Epochen, Modellbreiten, Speicher-Replay und Deduplizierung. Die bisherige native Format-/Architekturmatrix bleibt erhalten.

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
[CONTRIBUTING.md](CONTRIBUTING.md).

## Prüfungen der Interpreter-Rekonstruktion

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests \
  NeverDDevirtualizationSourceTests --parallel 4
ctest --test-dir build-release -L '^NeverD(InterpreterSpecialization|DevirtualizationSource)Tests$' \
  --output-on-failure
```

```sh
cmake --build build-release --target NeverDLowIRUndefinedIndependenceTests \
  NeverDOriginalBinaryUndefinedIndependenceTests \
  NeverDX86UndefinedEffectsTests NeverDX86CarryArithmeticFlagTests \
  NeverDX86LogicIdentityTests NeverDX86NoIndexAddressTests --parallel 4
build-release/bin/NeverDLowIRUndefinedIndependenceTests
build-release/bin/NeverDOriginalBinaryUndefinedIndependenceTests \
  --gtest_filter='OriginalBinaryUndefinedIndependence.*'
build-release/bin/NeverDX86UndefinedEffectsTests
build-release/bin/NeverDX86CarryArithmeticFlagTests
build-release/bin/NeverDX86LogicIdentityTests
build-release/bin/NeverDX86NoIndexAddressTests
```

API-Tests prüfen v1/v2/v3-Standardwerte, explizite Budgets, verkürzte Strukturen, alle reserved-Felder und künftige Anhänge. CLI-Tests prüfen Feld-/Abfrageerschöpfung und erfolgreiche Wiederherstellung für beide ABIs und Backends, lehnen ungültige Dezimalgrenzen ab und verlangen `--devirtualize`. Erschöpfte Budgets dürfen weder Quellcode noch partielle Restgraphen veröffentlichen.

v4-Tests fixieren Präfixgrößen und Padding, lehnen abgeschnittene Strukturen und unbekannte Flags ab, erhalten alte/zukünftige Anhänge und behalten Grenzen auch ohne Bericht im C. Unabhängige CLI-Beispiele benötigen Verkettung für Korrelationen und Grenzen für einen vorzeichenlosen Stackvergleich; beide C-Backends laufen bei O0/O2 mit Fallen für undefiniertes Verhalten. Das Abschalten der Erkennung muss ein davon abhängiges Ergebnis ändern. Parserprüfungen erfassen Null, Extremwerte, Überlauf, fehlerhafte Bereiche und fehlende Voraussetzungen. Python prüft Layout, Flags, Signaturen und den Besitz von Fehlerberichten.

`NeverDByteMemoryForwardingTests` prüft überlappende letzte Schreibzugriffe, beide Byteordnungen, durch acht teilbare Breiten bis i128, definierte Werte, korrelierte undef/poison-Snapshots und partielle Überschreibungen. Negativfälle erhalten Loads bei unbekannten Aliasen, Adressraumkonvertierungen, Aufrufen, geordneten Zugriffen, Lebensdaueränderungen, fehlenden Bytes, dynamischen oder ungültigen Offsets, Verzweigungen und Schleifen. Auch PHIs mit vielen Eingängen, Null-, exakte und erschöpfte Budgets sowie die standardmäßige Snapshot-Ablehnung werden geprüft. Originales und umgeschriebenes LLVM laufen mit O0/O2 gegen eine unabhängige arithmetische Referenz; bestehende MBA-, LLVMC- und Interpreter-Quelltests sichern die Kompatibilität.

Zusätzliche Regressionen erhalten reine Bit-Intrinsics in beiden Speichermodi und Byteordnungen. Gewöhnliche Aufrufe, Operand-Bundles, convergent, Lebensdaueränderungen, Speichereffekte und Traps bleiben Barrieren. Original und umgeschriebener Code laufen mit O0/O2 und Traps für undefiniertes Verhalten gegen eine unabhängige Referenz, die die numerische Adresse im Rückgabewert und jedes Pufferbyte prüft.

Die Byte-Lebendigkeitsregressionen prüfen disjunkte Reads, teilweise beobachtete Stores und gemeinsame Überdeckung durch mehrere Stores. Adressbedingungen umfassen AND/OR/XOR, 32/64 Bit, beide Byteordnungen, falsche Wurzeln, unvollständige Masken, umgehende Zusammenführungen und alle Budgetgrenzen. Unabhängige O0/O2-Orakel prüfen beide Zweige, alle sechzehn Adressreste und jedes Pufferbyte.

Die Adressregressionen prüfen Integer- und Pointer-PHI/select, beide Byteordnungen und Pointerbreiten, stabile und veränderliche Rückkanten, undef/freeze, Zyklen ohne Eingangsanker, benachbarte Arbeits- und Ausgabebudgets sowie Invalidierung bei reinen Adressänderungen. O0/O2-Schleifen vergleichen Rückgaben und jedes Pufferbyte mit einer unabhängigen Referenz pro Iteration, einschließlich einer wandernden Adresse, die nicht als konstant gelten darf.

Die numerischen Tests prüfen ganze und enthaltene Loads eines Schreibers, undef/poison ohne neue Snapshots, beide Byteordnungen, 32/64 Bit, modulare negative Offsets, partielle Überdeckung, Aliase zwischen Wurzeln und Allocas, werfende Aufrufe, Schleifen, benachbarte Budgetgrenzen und Analyseinvalidierung bei reiner Store-Löschung. O0/O2 vergleichen Original und Transformation mit einer unabhängigen Referenz für Rückgabewert und jedes Byte des aliasierenden Puffers.

`NeverDMedMutableSourceTests` und `NeverDLLVMCValueTests` führen unabhängige Schleifen, umgeordnete Blöcke, Rückkanten zum Einstieg, Laufzeit-Stackarithmetik, frühere Lesezugriffe, Verzweigungszusammenführungen, partielle Aliase, Wahrheitswerte und Bitzählungen einschließlich Null bei O0/O2 aus. Negativfälle weisen fehlerhafte Eingaben, abgeschnittene Ziele, mehrdeutige Träger und erschöpfte Budgets vor der Ausgabe zurück. Ein CLI-Fall oberhalb der SSA-Grenze verlangt ausführbares LLVMC und eine ausdrückliche Ablehnung durch HighC. Wiederholte Aktualisierungen und blockübergreifende Ketten gespeicherter Ausdrücke prüfen außerdem die Größe und Ausführung der C-Ausgabe.

Zusätzliche Regressionstests begrenzen private Lese- und Schreibzugriffe vor der LLVM-Promotion sowie die Größe der C-Ausgabe. Lange gemischte Rechenketten, umgeordnete SSA-Blöcke, überlappende Speicherzugriffe und Null-Rückgaben werden bei O0/O2 ausgeführt. Wichtige Fälle durchlaufen auch die tatsächliche LLVM-Optimierung; die Wiederverwendung des Emitters nach abgelehnter Modulerzeugung wird geprüft.

Regressionstests für zusammengesetzte Bedingungen prüfen bei O0/O2 Konjunktionen und Disjunktionen mit Gleichheit zu Konstanten ungleich null, vorzeichenlosen Vergleichen, vorzeichenbehafteten Vergleichen in beiden Operandenreihenfolgen, verbreiterten Booleschen Eingaben und allen Negationskombinationen. Die C-Ausgabe muss die gesamte Wahrheitstabelle erhalten und darf keinen fehlenden Nullvergleichsoperanden dereferenzieren. Ganzzahlige Adressspeicherungen prüfen ausgerichtete und unausgerichtete 32/64/128-Bit-Träger. Byte-Speicherarrays behalten explizite Ausrichtung sowie genaue Basis- und Teilzugriffe bei, ohne skalare Array-Zuweisungen oder Aliasing durch inkompatible Typen.

`NeverDLowIRRefinementTests` prüft echte rekonstruierte Graphen, unterschiedlich strukturierte endliche Schleifen einschließlich null Iterationen, dynamische Erzeuger, bedingte Wahlen, überlappende Eingaben, korrelierte Kopien und Spills, beidseitige unveränderliche Lesebelege sowie Systemflags und Rückkehrslot-Erhaltung. Falsche Kandidaten, zusätzliche Schreibzugriffe, unvollständige oder unendliche Pfade, veraltete Belege, Scratch-Kollisionen und erschöpfte gemeinsame Budgets müssen Zertifikate verweigern. Die bisherigen Unabhängigkeitstests lehnen beobachtbare beliebige Werte weiterhin ab.

`CompleteModel`, `CompletedTargetFacts`, `ConditionalImplication` und `PartitionedCoverage` in `NeverDLowIRRefinementTests` verwenden unabhängige vollständige Orakel für kleine Eingabebereiche und prüfen fehlerhafte Eingaben, veraltete Caches, unvollständige Aufzählungen sowie exakte/zu knappe Budgets. Echte LowIR- und Binärtests prüfen bedingte Produkte und alle terminalen Zweige der ursprünglichen/rekonstruierten Graphen bei festen Gattergrenzen; geänderte Endbeobachtungen, fremde Eingabebereiche und fehlende Ziele werden abgelehnt. `FiniteValues` unterscheidet Kodierungsfehler von Ablehnungen durch Suche, Wertezahl oder Gesamtbudget.

`LowIRLoopRefinement.*` und `BinaryLowIRLoopRefinement.*` im selben Ziel prüfen beliebige 64-Bit-Zähler, verschachtelte lexikografische Ränge, echte native Restprogramme, Eingangsvorlagen, überlappende Ansichten und korrelierte Auslagerungen. Negativtests verwerfen falsche Schleifenkörper, eingeschränkte Eingangsbereiche, nicht sinkende Ränge, vorzeichenlosen Umlauf, vergessene frühere Schreibzugriffe, fehlende Schnittpunkte, ungültige Vorlagen und erschöpfte gemeinsame Budgets. Ein erfolgreicher endlicher Geschwisterpfad legitimiert keinen unvollständigen Induktionsbeweis.

`LowIRLoopInference.*` und `BinaryLowIRLoopInference.*` verwenden unabhängig geschriebene Zähler, Stack-Ablagen, frühe Rückgaben, native Aufrufe und gepackte Flags. Sie prüfen schmale arithmetische Erweiterung und semantisch gleiche Flags mit unterschiedlichen Ausdrücken. Fehlerhafte Graphen, fehlende oder gefälschte Ursprünge, endlose oder umlaufende Schleifen und erschöpfte Budgets dürfen kein Zertifikat erzeugen.

Nullpräfix-Regressionen prüfen Gruppierung, ungewöhnliche Breiten und alle Bytepaare unter Erhalt unbekannter und von null verschiedener Bits. Unabhängige Frame-Schleifen prüfen getrennte Schreibzugriffe auf niedrige Werte und hohe Nullbits in beiden Bytefolgen, schmale Breiten, falsche Arithmetik und Füllbits, exakte/zu kleine Inferenzbudgets und das getrennte Abfragebudget des vollständigen Beweises.

Regressionen für gemeinsame Schleifenköpfe und Rücksprungblöcke prüfen nullerweiterte 32-Bit- und volle 64-Bit-Zähler, skalare Ränge mit anderen Schrittweiten, falsche Ergebnisse, stagnierende und umlaufende Pfade sowie exakte oder erschöpfte Budgets über skalare und Tupelsuche hinweg. `LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`. Zusätzliche Inkrement-/Reset-Regressionen verlangen Konvergenz ohne bitweises Entfalten des Zählers pro Runde und lehnen fehlenden Fortschritt sowie vorzeichenlosen Überlauf ab. Scheduling-Regressionen prüfen Akkumulatoren mit anderen Schrittweiten, umlaufende Einheitszähler neben einem gültigen skalaren Rang mit anderer Schrittweite sowie drei Zähler, deren gültiges Tupel erst nach dem frühen Fenster erscheint. Exakte und um einen Versuch reduzierte Rangbudgets prüfen deterministische Fortsetzung ohne wiederholte Vorschläge.

`LowIRLoopPlanPairing.*` im selben Ziel prüft umbenannte Register, unterschiedliche Rechenkörper, seitenspezifische Präfixzustände, erhaltene Prädikate, gemeinsame Speicherrahmeneingaben, verschachtelte Schnittpunkte und unabhängige Beweisbudgets. Fehlende Beziehungen, falsche Schreibzugriffe, ungültige Temporärbindungen, unvollständige Zuordnungen und erschöpfte Metadatengrenzen dürfen kein Zertifikat erzeugen.

`LowIRLoopAlignment.*` prüft unabhängig geschriebene gewöhnliche und rotierte Schleifen mit Frame-Zählern: Beide Standardpläne beweisen sich einzeln, ihre erste Paarung scheitert, ein anderer Kandidatenschnittpunkt beweist die Relation. Regressionen decken mehrere Schnittpunktpermutationen, falsche Ergebnisse und Frame-Schreibzugriffe, fehlende oder veraltete Originalaufzeichnungen, explizite Witnesses für undefinierte Werte, nicht fallende oder überlaufende Zähler, fehlerhafte Graphen, kumulierte Abfragen fehlgeschlagener Versuche, exakte Gesamtbudgets und erschöpfte Suchlimits ab. Ablehnungen dürfen kein Zertifikat enthalten. Weitere Fälle prüfen getrennte Rücksetz- und Fortschrittsphasen, äquivalent verschobene Austrittswächter mit familienübergreifender Paarung, Caches ohne erneute Inferenz und gemeinsame Metadatenüberschreitung. Ein nachfolgender unabhängiger Zyklus prüft vollständige Abdeckung mit ausdrücklich 16384 Inferenzabfragen. Leere und doppelte Familien benötigen keine symbolischen Abfragen; zu wenige Schnitte werden abgelehnt. Exakte und um einen Versuch reduzierte Gesamtbudgets, falsche Ergebnisse, fehlender Fortschritt, ursprüngliche Evidenz und undefinierte Witness-Werte bleiben geprüft. Filterregressionen prüfen neutrale arithmetische Rauten, lokale Zusammenläufe gegenüber Zusammenläufen nur an Grenzen sowie erreichbare Zusammenläufe mit Umgehung zu einem Austritt oder einer Schleifengrenze. Geprüft werden gefilterte Kandidaten trotz doppelter Originalfamilie, Wiederverwendung eines zuvor erfolgreichen Filterplans vor späterer vollständiger Inferenz, exaktes, um eins zu kleines und null gesetztes `MaxCutSelectionWork`, kumuliertes fehlgeschlagenes `CutSelectionWork` und keine symbolische Inferenz nach Erschöpfung der globalen Grapharbeit. Beide Verzweigungsfamilien prüfen vollständige Zyklusabdeckung; die Rautenrelation verwendet explizite Inferenz- und Beweisabfragelimits.

`LowIRLoopAlignment.DirectRanks*` soll umbenannte Register-/Frame-Speicherorte, kompatible Frame-Vereinigungen, Konflikte an beiden Endpunkten sowie die konservative Ablehnung konstanter oder berechneter Ränge und tatsächlicher 8- gegenüber 4-Byte-Frame-Eingaben prüfen. Das Abrechnungsorakel muss optionale Durchlauf-, Vergleichs- und Metadatenkosten unabhängig aus den öffentlichen Selbstrelationsplänen ableiten, bevor exakte und um eine Einheit kleinere Grenzen geprüft werden; die Summe einer erfolgreichen Suche darf nicht als Erwartungswert kopiert werden. Duplikate müssen Versuche und Arbeit, aber keine neuen Prüferabfragen verbrauchen; nach dem optionalen Limit muss eine spätere gewöhnliche Paarung noch beweisen können. Falsche Ergebnisse, geänderte Frames oder erhaltene Zustände, fehlender Fortschritt und veraltete Originalevidenz müssen bei vollständigen Beobachtungen und unverändertem gewähltem Witness abgelehnt bleiben. `InterpreterLLVMRefinement.PreservationRestoresEntryAcrossLoopCutpoints` soll den Quellplan ohne manuelle Schnittadressen oder Rangbindungen durch die öffentliche Suche finden, neue Modelle vorbereiten und beide nativen und LLVM-Prämissen im zusammengesetzten Prüfer erneut prüfen. Dies behauptet weder einen Testlauferfolg noch eine gewöhnliche ABI-Zertifizierung.

`LowIRLoopAlignment.PlanCutCap*` prüft Null-, zu kleine und exakte Plangrenzen, null symbolische Anfragen für zu große Familien, vollständige Zyklusabdeckung, fehlerhafte Graphen und spätere gefilterte Beweise in beiden Richtungen. Eigenständige Inferenz bleibt kompatibel. `LLVMModel.RejectedBodyTemplateDoesNotHideAValidGuardedHeader` prüft außerdem, dass eine Ausrichtungsgrenze von einem Schnitt mehrere Inferenzversuche erlaubt. Falsche Ergebnisse dürfen weiterhin kein Zertifikat erhalten.

Die Regressionen prüfen Frame- und Registerteilbereiche, beide Richtungen, untere/mittlere/obere Positionen, ungewöhnliche Breiten, beide Byteordnungen und drei Byte breite Frame-Wörter. Abgedeckt sind späte Erkennung, Änderungen erhaltener Bits, fehlender Fortschritt und ungesichertes Überlaufen, ungültige zusätzliche Einstiege, exakte/zu kleine Budgets sowie die bisherige Einzelschnittsuche.

Regressionen der vorderen Phase prüfen zwei und drei aufeinanderfolgende Schleifen mit wiederverwendetem Countdown-Wort, die Kombination mit verschachtelten Schleifenphasen, exakte und um einen Versuch zu kleine Rang- und Abfragebudgets, Schleifen ohne Fortschritt und Rücksetzungen zu einer früheren Phase. Fehlerhafte Phasenkonstanten gleicher Breite, falsche Ergebnisse und Frame-Schreibzugriffe müssen vom vollständigen Prüfer ohne Zertifikat abgelehnt werden; fehlende Originalevidenz bleibt nicht unterstützt.

`InterpreterMachineStateModel.*` in `NeverDLowIRRefinementTests` prüft mit unabhängigen LowIR-Beispielen rohe Eintrittsflags, Status getrennt vom Gast-RAX, alle 17 Zustandswörter, Teilregister, gepackte Flags, bleibende dynamische Ablehnung, Gast-Rahmenschreibzugriffe, beide Zweige und Schleifeninferenz mit anschließendem neuem Beweis. Falsche Ausgaben, verlorener Status, geänderter Speicher, veraltete Instruktionsdatensätze, fehlerhafte Eingaben und erschöpfte Erzeugungsbudgets müssen scheitern. Bestehende Quelltexttests führen beide C-Wege unter O0/O2 aus; Modelltests allein zertifizieren keinen kompilierten C-Code.

`NeverDLLVMInterpreterModelTests` vergleicht unabhängig geschriebenes LLVM mit LowIR-Referenzen für den gesamten Zustand: Breiten, parallele PHIs, switch, Gastspeicher, separater Status, Poison-Bedingungen, Intrinsic-Bereiche, abgelehnte Verträge und vier Budgets. Ein vollständiger Beweis für beliebige Wort-Countdowns wird geprüft, veränderter Status abgelehnt. Unabhängiges C muss nach O1/O2-Kompilierung dieselben Beobachtungen erfüllen. Die Tests prüfen das unterstützte Modell; automatische Invariantenfindung und Compilerkorrektheit bleiben separat. Die variablen Schiebetests decken alle vier Breiten, durch Masken oder Verzweigungen begrenzte Schiebeweiten, Grenzwerte und zu große Werte, Überlaufverbote und Exaktheitsflags, strikte Poison-Ablehnung sowie mit O1/O2 kompiliertes C ab.

`LLVMGuestAlignment.*` vergleicht Laden und Speichern mit unabhängigen Byte-Speicherreferenzen: ausgerichtete und falsch ausgerichtete Domänen, freie hohe Adressbits, geparste Standardausrichtung, Teilbreiten, ungenutzte oder überschriebene Zugriffe, unerreichbare Zweige und exakte sowie um eins zu kleine Konstruktionsbudgets. `InterpreterLLVMRefinement.GuestAlignmentRequiresBothFreshPremises` prüft native Stack-Schreibzugriffe, passende Eingangskongruenzen und geänderte Quelleffekte durch zwei neu geprüfte Relationen.

`LLVMByteSwap*`, `LLVMScalarByteSwap.*` und `InterpreterLLVMRefinement.ByteSwapRequiresBothFreshPremises` prüfen unabhängige Bytekopier- und Schiebe/Masken-Referenzen, obere Bytes, blockübergreifende Werte, Poison, Aufrufverträge sowie unabhängig gezählte exakte und um eins zu kleine Budgets. Clang-O1/O2-Fälle müssen echte Intrinsics enthalten; kleine native Austausch-/BSWAP-Fälle prüfen beide Prämissen neu und lehnen falsche Werte oder fehlendes Nullsetzen des oberen Wortes ab.

`NeverDLLVMScalarEquivalenceTests` prüft vollständige Schleifendomänen, null Iterationen, gleichzeitige PHI-Tausche, switch, hohe Eingabebits, Gegenbeispiele in der letzten Partition, zusätzliche poison-erzeugende Updates, Rückgabebereiche, nicht unterstützte Verträge sowie exakte, um eins zu kleine und Nullbudgets. Unabhängige Referenzen für doppelte Breite und Überlauf prüfen Funnel-Endpunkte und bewachte Produkte aller unterstützten Wortbreiten; unabhängig geschriebene verschachtelte C-Schleifen bei O1/O2 prüfen das Compiler-Eingabeprofil. Die Zustandsmodellsuite prüft ebenfalls die Endpunkte. `SymExpr.ConstantWindowSharesActualWorkWithoutRelaxingQueryCeilings` prüft kumulative Abrechnung und unveränderte lokale Limits.

`LLVMScalarDecision.*` prüft tiefe Exact-Shift-/Erweiterungsbedingungen, konstante Verzweigungen, beide Schleifenrückkanten, erhaltene hohe Datenbits, späte undefinierte Operationen, Nichtterminierung, Änderungen nach einer Prüfung sowie exakte, um eins zu kleine und lokale Budgets. `LLVMScalarDecisionCompiled.DeepOneAndTwoBackedgeOracles` vergleicht unabhängig verfasste Rekurrenzen mit einer oder zwei Rückkanten bei O0/O2 in 32.768 Aufrufen mit einem vorzeichenlosen C-Orakel. Das sind Prüfungen des Skalarmodells, keine Abdeckung der nativen ABI oder der Wiederherstellung ganzer Binärdateien.

`LLVMScalarDemand.*` beweist getrennte nichtlineare Schleifenkörper mit festem Arbeitsbudget, behält alle 16 Kontrollpartitionen und freien hohen Eingabebits, lehnt Ausgabe- und Definiertheitsfehler der letzten Partition ab und prüft genaue/zu kleine Budgets sowie Knotengrenzen. `LLVMScalarDemandCompiled.*` vergleicht beide Körper bei O0/O2 in 262.144 Aufrufen mit einem unabhängigen vorzeichenlosen Rechenorakel. Quelloperationen und Eingabedomäne bleiben erhalten.

`SymKnownBitsTests` prüft Fakten anhand aller Byte-Eingabepaare und beliebig breiter Grenzwerte: Erweiterungsidentitäten, nicht überlaufende Summen, verschiedene Wurzeln und vollständig definierte Schiebesemantik. Geprüft werden exakte und um eins zu kleine Budgets, berechnete Cache-Treffer, Speichergrenzen, getrennte Kontexte, Tiefe, Operandenzahl und nicht unterstützte Breiten. `SymExprExtensionTests` prüft hohe Konstantenbits und vollständige Schiebewerte. Skalare Äquivalenztests erhalten symbolische Daten bei Bereichsbeweisen, lehnen Unterschiede in der letzten Partition und ausgeführtes poison ab und prüfen das Budget des gesamten Beweises. `SymMBAExtensionTests` verlangt eine Herleitung ohne Stichprobenprüfung, prüft alle Byte-Eingabepaare und erhält die Grenzen von Vorzeichen, schmalem Übertrag, Bitkomplement und erschöpftem Arbeitsbudget.

`NeverDLLVMScalarLoopRecoveryTests` prüft Präfixe und Vorgängerzustände, Nulliterationspfade, Selbst-Rückkanten, affine Zustände, Gleichheit bei Überlauf, Poison durch zusätzliche Updates, hohe Datenbits und abgelehnte Eingabeverträge. Exakte und um eins gekürzte kumulative Budgets prüfen atomare Ablehnung. Unabhängige arithmetische Orakel führen ursprüngliches und rekonstruiertes LLVM bei O0/O2 für sämtliche Byte-Steuereingaben aus. Das bestätigt weder native ABI-Rekonstruktion noch standardmäßige C-Ausgabe.

Prädikatsregressionen prüfen 8/16/32/64-Bit-PHIs, vertauschte Vergleiche und Zweigpolaritäten, Erweiterungen mit und ohne Vorzeichen, verschiedene Eingangsinitialwerte, widersprüchliche Beobachtungen, mehrere Rückkanten, symbolische Gegenbeispiele, abgelehnte Kürzung, ursprüngliches Poison/Nichtterminierung und atomare Ablehnung an Konstruktions-, Beweis-, Kandidaten- und Transformationsgrenzen. Unabhängige vorzeichenlose Orakel vergleichen Original und Ergebnis bei O0/O2 über 458.752 Aufrufe mit Fallen für undefiniertes Verhalten. Quelle und Elternmodul bleiben unverändert.

Regressionen prüfen modulare Guards mit 8/16/32/64 Bit, negative Schritte, beide Polaritäten, null Iterationen, gemeinsame Austritte, nur bei Nulldaten passende falsche Nachbargrenzen, unerreichbare Grenzen, übergroße Ausschnitte, Poison, fehlende Blätter und atomare Budgets. Vierzig ungenutzte Argumente dürfen den benötigten Präfix-Anfangswert nicht verdrängen. Unabhängige vorzeichenlose Orakel führen Original und Ergebnis bei O0/O2 über 458.752 Aufrufe mit Fallen für undefiniertes Verhalten aus.

Dasselbe Ziel prüft auch `recoverLLVMScalarSource`: Vorbereitung vor der Suche, reine Bereinigung, unbenutzten Zustand voller Breite, tote Überlauf-, Exact-Shift-, Divisions- und Assume-Pflichten, nicht unterstützte Effekte, exakte und um eins zu kleine kumulative Budgets sowie begrenzte Fortsetzung. Unabhängige arithmetische Orakel führen ursprüngliches und vorbereitetes LLVM bei O0/O2 für alle Byte-Steuerwerte und deterministische Zustände voller Breite aus. Quelle und Elternmodul bleiben bei Erfolg und Ablehnung unverändert.

Guard-Tests prüfen annotierte modulare Identitäten, äquivalente Ausdrücke verschiedener Vorgänger, unterschiedliche Zweigwerte, Ablehnung ursprünglicher Überläufe/exakter Shifts/Verkürzungen/Erweiterungen sowie genaue und zu kleine Gesamtbudgets. Unabhängige vorzeichenlose Orakel vergleichen Original und Vorschlag bei O0/O2 über 131.072 Aufrufe mit Fallen für undefiniertes Verhalten. Bestehende Semantiktests decken weiterhin die Ablehnungsregeln des eigenständigen Passes ab.

Maskentests prüfen vertauschte Operanden, Nullfelder, erhaltene hohe Eingabebits, Alternativen nach gescheitertem Vollbeweis, alle Rückkanten, abgelehntes Wraparound/Überlaufen, mehr als 32 Vorschläge sowie exakte und um eins zu kleine atomare Budgets. Unabhängige LLVM- und C-Arithmetikorakel bei O0/O2 prüfen das Zusammenspiel mit der Breitenrekonstruktion. Selbstabfragen behalten vollständige Steuerdomänen, Ablehnung von poison/undef und nicht unterstützten Verträgen, Nichtterminierung, lokale Grenzen und genaue Arbeitszählung bei. Änderungen am selben Funktionsobjekt machen frühere Ergebnisse ungültig. Dies ist skalare LLVM-Abdeckung, keine Zertifizierung der nativen ABI.

Maskeneinschlusstests prüfen alle Byte-Eingabepaare, nicht zusammenhängende Masken, Breiten bis 128 Bit, erhaltene unbekannte/hohe Bits und begrenztes Knotenwachstum jenseits der Term- und Breitengrenzen. Eine symbolische Schleife mit zwei Rückkanten muss ihre maskierte XOR-Rekurrenz gegen eine unabhängige geschlossene Form beweisen. Abhängigkeitstests zählen die normalisierte Speicherung von Schiebezählern und behalten exakte/knappe Budgets sowie den konservativen Rückfall für breite Werte bei.

Breitentests prüfen von null verschiedene Literale, unveränderte Signaturen, breitere Eingaben und Intrinsics, beobachtbare obere Bits, vorzeichenbehaftete Ordnung, neue Überläufe und exakte/knappe Budgets. Dieselben skalaren Kandidaten laufen mit x86-64-, AArch64-, Big-Endian-AArch64- und ARM32-Triples; dies ist LLVM-Abdeckung, keine native ABI-Zertifizierung. Unabhängige arithmetische Orakel für originales/rekonstruiertes LLVM und erzeugtes C laufen mit O0/O2 und C-UB-Traps.

Regressionen prüfen alle externen Eingänge, mehrere Rückkanten, verborgene hohe Datenbits, Poison, parallele Vertauschungen, Gruppenteilung, über 32 Zustandsvariablen und atomare Budgets. Skalare Beweise unterscheiden abgeschlossene unbekannte Anfragen von globaler Erschöpfung und beweisen sichere Verschiebungen ohne Datenbit-Aufzählung. Symbolische Tests erschöpfen Bytewerte, Masken und Zähler unter Erhalt beobachtbarer Bits, Quellidentität und großer Zähler. Verschiebungsansichten unterschiedlicher Breite teilen den vollständigen numerischen Zähler. Diese Prüfungen zertifizieren keine native ABI.

Zusätzliche Schleifentests decken schmale überlaufende Endindizes, getrennte Body-/Latch-Blöcke, beide Bedingungsrichtungen, vertauschte Gleichheitsoperanden, umgeordnete und absteigende Einheitsrekurrenzen, Unterschiede hoher Datenbits im leeren Pfad, neu ausgeführtes Poison und falsche Grenzen ab. Exakte und um eine Einheit gekürzte Konstruktions- und Beweisbudgets sowie erschöpfte Kandidatenbudgets erhalten die atomare Ablehnung. Original-LLVM und erzeugtes C laufen bei O0/O2 gegen unabhängige arithmetische Orakel.

`NeverDLLVMCScalarLoopRecoveryTests` prüft vollständige und ausgewählte Standardausgabe, weitere Rekonstruktion nach Rückgabebereinigung, Identität und Attribute, Aufrufer, bestehende/neue Intrinsics und Konflikte, gemeinsame Budgets, Aufrufe mit Effekten, fehlende Eingabedefiniertheit, Metadaten, Images, externe Blockadressen und fremde Funktionsauswahl. Unabhängige Rechen- und Rotationsorakel führen das erzeugte C bei O0/O2 mit Fallen für undefiniertes Verhalten aus. Die Arithmetik wird zusätzlich für sämtliche Byte-Steuerwerte, Grenzwerte und deterministische Vollbreitendaten mit unabhängig kompiliertem Original-LLVM verglichen.

`SymSimplifyPredicates.*` vergleicht außerdem die Regeln der eigenständigen Phase und des vollständigen Passes, gemeldeten Aufwand, exakte und um eine Einheit zu kleine Budgets, eine deaktivierte Phase sowie mit Obfuskationsmarkierung versehene Funktionen. Regressionen für skalaren Quelltext prüfen arithmetisch codierte Schleifenabbrüche mit 8/32/64 Bit, die Ablehnung vorzeichenbehafteter Überläufe und gemeinsame Konstruktionsgrenzen über Runden und Funktionen hinweg. Scheitert die Veröffentlichung, bleibt das ursprüngliche IR erhalten. Das erzeugte C läuft bei O0/O2 gegen unabhängig kompiliertes Original-LLVM und ein arithmetisches Orakel.

`SymKnownBits.*` prüft verlustfreie Masken und vorzeichenbehaftete Hin- und Rückverschiebungen anhand vollständiger Bytepaare, einschließlich negativer Werte, anderer Quellen, entfernter unbekannter Bits, abweichender Faktoren/Schiebewerte und breiter Überschiebungen. Bis 128 Bit werden exakte und um eine Einheit zu kleine Budgets sowie unveränderte DAG-Größe geprüft. Skalare Entscheidungstests ergänzen positive und negative Aktualisierungen auf getrennten Rückkanten, die Ablehnung veralteter Fakten und von Überläufen, symbolische hohe Datenbits sowie 16.384 O0/O2-Aufrufe gegen ein unabhängiges vorzeichenloses Orakel.

`SymKnownBits.*` prüft außerdem alle Bytepaare für die Ordnung von Vielfachen und Produkte unterschiedlicher Breite. Überlauf, falsche Koeffizienten oder Faktorhäufigkeiten sowie das Verlegen schmaler Überläufe in breitere Wörter werden abgelehnt. Bis 128 Bit bleiben exakte und um eine Einheit zu kleine Budgets sowie unveränderte DAG-Größe geprüft. Skalare Tests beweisen wiederholte Addition und Multiplikation ohne Aufzählung der Datenbits, erhalten alle Überlaufpflichten der Schleifen und führen 16.384 O0/O2-Aufrufe gegen ein unabhängiges Orakel aus.

`LLVMScalarAssume*` prüft vollständige Schleifenbereiche, Fehler in der letzten Partition, unerreichbare und erreichte falsche Bedingungen, kumulierte Definiertheit für alle Byte-Eingaben, exakte und um eins zu kleine Budgets, IR-Änderungen und nicht unterstützte Aufrufverträge. Vier Ziel-Triples prüfen das gemeinsame Modell; 8.192 O0/O2-Aufrufe werden mit einem unabhängigen vorzeichenlosen Orakel verglichen. Die Zustandsmodell-Suite prüft dieselbe Verpflichtung und die Ablehnung von Operanden-Bundles separat.

`LLVMScalarProjection.*` prüft verschachtelte Felder, Bitfenster, erhaltene ungenutzte Argumente, mehrere Rückgaben, Rückkanten, nicht ausgewählte overflow/shift/assume-Bedingungen, Fehler der letzten Partition, Nichtterminierung, unbekannte Verträge, geänderte Eingaben und exakte/um eins zu kleine Budgets. Vier Zieltripel testen die gemeinsame Semantik. `LLVMScalarProjectionCompiled.*` vergleicht das Originalaggregat über eine LLVM-Array-Brücke und projizierte Fenster mit unabhängiger vorzeichenloser Arithmetik bei O0/O2. `SymExpr.RightShift*` erschöpft Bytepaare und prüft Vorzeichenerweiterung, Überträge, erhaltene hohe Bits, vollständige Zähler und Suchgrenzen.

`LLVMScalarInputs.*` prüft geordnete Zuordnungen gemischter Breiten, parameterlose und unbenannte Schnittstellen, Eingabebedarf toter Arithmetik und von assume, veränderte Ausgaben, unbekannte Verträge, Verpackungsablehnung und genaue beziehungsweise zu kleine Gesamtbudgets. Beweise stellen die vollständige Signatur wieder her, ohne ausgelassene Eingaben festzulegen. `LLVMScalarInputsCompiled.*` vergleicht ursprüngliche und reduzierte Schleifen bei O0/O2 mit einem unabhängigen vorzeichenlosen Orakel und variiert alle ausgelassenen Argumente.

`NeverDLLVMScalarStateProjectionTests` prüft überlappende/unausgerichtete Fenster, 8/16/32/64-Bit-Zellen, Schleifen, Eingangsmasken, Quelländerungen, Statusbereiche, erhaltenes Poison, externen Speicher und exakte/knappe Budgets. Speicheroriginale und LLVM-Aggregatbrücken führen 172.032 O0/O2-Vergleiche mit unabhängigen Byte-/Arithmetikorakeln aus; skalare Beweise bleiben getrennt. `SymKnownBits.*` erschöpft Bytepaare und prüft 128 Bit, andere Faktoren, erweiterte Masken, überlaufende Summen und Budgets. `LLVMCIntrinsicSemantics.AssumeEvaluatesItsConditionAndRefusesBundles` prüft einmalige Auswertung bei O0/O2 und Bundle-Ablehnung.

Schleifenmetadaten-Regressionen vergleichen Zählschleifen mit einer unabhängigen Formel über alle Kontrollpartitionen bei exakten und um eins zu kleinen Budgets. Große oder nullwertige Peeling-Zähler können falsche Ergebnisse, Nichtterminierung oder poison nicht verdecken. Per API erzeugte fehlerhafte Metadaten prüfen die Importer-Ablehnung getrennt vom LLVM-Assembly-Parser; Maschinenzustandstests erhalten Zustandseffekte und Eingabegrenzen.

Initialisierungsregressionen prüfen partielle und getrennte Bytebereiche, feste Aliasse, beide Zweige, jede Rückkehr, Lesezugriffe der ersten Iteration und Schreiben vor Lesen in Schleifen. Lesen vor Schreiben, fehlende Schreibzugriffe, Gastspeicherungen, unbekannte Aliasse, spezielle Speicherzugriffe, Bereiche außerhalb des Objekts und erschöpfte Budgets müssen scheitern. Ein unabhängiges C-Beispiel mit einem nur geschriebenen Zustandswort wird mit O1/O2 kompiliert, behält die exakten LLVM-Attribute und besteht eine neue kombinierte Prüfung von nativem Code zu LLVM.

Geschützte Countdown-Tests prüfen den nächsten Versuch nach einem verworfenen Rumpf-Template, einen vollständigen Kopfbeweis für beliebige Worteingaben, gemeinsame Schnittpunkt-/Anfragebudgets und die sofortige Ablehnung echter Eingangsvertragsverletzungen.

`NeverDInterpreterLLVMRefinementTests` prüft neue Gesamtbeweise, exakte Text-/Funktionsbindung, unabhängige Budgets, vollständige Beobachtungen und größere Quellbereiche. Geänderte Bytes, Restprogramme, Ergebnisse, Flags, Status, Frame-Schreibzugriffe, Poison und falsche/veraltete Schleifenpläne müssen den Gesamtnachweis verhindern. Beliebige Wortzähler erfordern beide induktiven Voraussetzungen; unabhängige C-Beispiele mit O1/O2 prüfen tatsächlichen serialisierten LLVM-Input. Zustandsmodelltests lehnen versteckte Einstieg-Rückkanten ab und begrenzen Wurzeln ohne Kopie zusätzlicher Herkunftsdaten.

`InterpreterLLVMRefinement.Preservation*` prüft Teilbereiche und Überlappungen, ungültige Anfragen, unabhängig berechneten Vorbereitungsaufwand, identische Endwertänderungen, Sicherung/Wiederherstellung von Eintrittswerten über Schleifen, neue opake Nachweise und späte Ablehnung. Auch den API-Verbraucher `NeverDPEFixedImageTests` neu bauen. Ergebnisse, Zähler und Digests bei ausgelassener Anfrage separat mit der Basis vergleichen.

`InterpreterLLVMRefinement.Collection*` prüft notwendige Beibehaltung und Verzögerung in endlichen und induktiven Beweisen, erreichbare ungültige Zweige, späte Quellablehnung, Eintrittserhaltung und alle vier nativen Optionsidentitäten. Kompilierte Auslassungsfehler im Kompositionsmodul prüfen die Weitergabe jeder benötigten Option.

```sh
cmake --build build-release --target NeverDLLVMCScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMCScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarEquivalenceTests --parallel 4
build-release/bin/NeverDLLVMScalarEquivalenceTests
cmake --build build-release --target NeverDLLVMScalarResultProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarResultProjectionTests
cmake --build build-release --target NeverDLLVMScalarStateProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarStateProjectionTests
cmake --build build-release --target NeverDLLVMScalarInputProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarInputProjectionTests
cmake --build build-release --target NeverDLLVMInterpreterModelTests --parallel 4
build-release/bin/NeverDLLVMInterpreterModelTests
cmake --build build-release --target NeverDInterpreterLLVMRefinementTests --parallel 4
build-release/bin/NeverDInterpreterLLVMRefinementTests
```

Zwischengespeicherte Gleichheitsbedingungen in zwei- und dreifach verschachtelten Schleifen prüfen korrelierte Operanden, veränderliche Grenzen, Zählerrücksetzungen und beschädigte Kopien.

Regressionen für Vergleichscaches prüfen Gleichheit und Ungleichheit, Guards und konstante Initialisierung, erst nach Erweiterung entdeckte Felder sowie Bits 7/31/63 in 1/4/8-Byte-Caches. Auch ein geändertes Nachbarbit bei unverändertem Prüfbit muss am vollständigen Zustandsvergleich scheitern. Nullschritte, bewegliche Grenzen, Rücksetzungen und erschöpfte Budgets müssen abgewiesen werden.

Regressionen prüfen zusammengeführte Eingänge, erste Zeugen ohne Iteration, verborgene Register-/Frame-Unterschiede, nichtkanonische boolesche Prädikate, native Trap-Bedingungen, korrelierte Spills sowie ungültige oder ausgeschöpfte Pläne. Unabhängige zwei-/dreistufige Gleichheitszähler und native Bytes prüfen vorzeichenlose Grenzen, Null-/Maximalbereiche, andere Schrittweiten und falsche Originalbefehle. Inferenz und Abschlussbeweis müssen unvollständige Ergebnisse ablehnen.

Unabhängige Regressionen für alternative Schleifen prüfen beide Verzweigungsrichtungen, falsche Schleifenrümpfe, einen nicht terminierenden Nachbarzweig sowie erschöpfte gemeinsame Such-/Beweisbudgets. `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

Die Regressionen prüfen zwei und drei verschachtelte Ebenen, auf- und absteigende Zähler, abgeleitete Phasen und Schnittpunkte in echten nativen Schleifenrümpfen. Unerreichbare oder disjunkte Präfixbereiche, falsche Rümpfe, endlose oder überlaufende Übergänge und erschöpfte gemeinsame Such-/Beweisbudgets müssen abgelehnt werden. Präfixzeugen ersetzen keine vollständige Segmentabdeckung.

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

`NeverDLowIRUndefinedIndependenceTests` prüft die Unabhängigkeit zweier Ausführungen vollständiger azyklischer LowIR-Graphen. Gewöhnliche Eingaben am Eintritt sind gemeinsam; neu erzeugte, architektonisch undefinierte Werte behalten ihre Korrelationen über Kopien, überlappende Schreibzugriffe, Stack-Sicherungen und erneutes Laden hinweg. Kontrollprädikate werden vor den Pfadannahmen geprüft. Zertifikate verlangen `Complete`-Effektmetadaten, die exakt an die vollständigen Grenzen jeder Instruktion und den Digest ihrer Operationen gebunden sind. Fehlende Belege, erreichbare Schleifen, Aufrufe, unbekannte Aliase und erschöpfte Budgets führen zur Ablehnung. Explizite Beobachtungen und ein Vertrag für fehlerfreie Frame-Zugriffe begrenzen das Ergebnis; es beweist keine vollständige Äquivalenz von nativem Code zu C.

`FreshChoiceProjection.*` gehört zu `NeverDInterpreterSpecializationTests`. Unabhängige symbolische Kontexte vergleichen die genaue Knotenreihenfolge mit der gewöhnlichen Substitution; Byte-Eingabeorakel prüfen projizierte Werte und erhaltene Wahlwerte. Regressionen decken Kontext- und Blattzulassung, Null- und exakte Kapazitäten, Rückfall sowie den gesamten Traversierungsaufwand gemeinsam genutzter inkrementeller Ketten ab. Zeitschwellen werden nicht verwendet.

Das folgende Verhalten verwendet den standardmäßigen strengen Prüfvertrag. `NeverDOriginalBinaryUndefinedIndependenceTests` prüft mit unabhängigen x64-Bytes in festen Abbildungen physische CALL/RET-Semantik, veränderte Rücksprungziele, vollständige endliche indirekte Zielmengen und unveränderliche Lesezugriffe. Dasselbe Ziel prüft vollständige direkte Verzweigungen, genaue Bindung von Bytes/Effekten/Abbildungen/Lesebelegen, Erhaltung von Eintritts-RSP und Rückadressenslot am äußeren Rücksprung sowie Frame/Image-Trennung. Fehlende oder überlappende Instruktionen, ungeprüfte Zweige außerhalb der exakten Trap- und expliziten Profilprojektionsregeln, nicht terminierende oder budgetüberschreitende Schleifen, unvollständige Zielmengen, Profil-/Vertragskonflikte und erschöpfte Budgets müssen ohne Zertifikat oder Restcode abgelehnt werden. Alle möglichen nativen Pfade müssen enden. Diese optionale Schranke zertifiziert keine Schleifeninvarianten, Ausnahmebehandlung, CET-Ausführung oder native-zu-C-Äquivalenz; gewöhnliche Wiederherstellung bleibt getrennt. Das Ziel prüft außerdem strikt geliftete `INT3`/`UD2`-Endgrenzen und die Bindung ihrer vollständigen Bytes und Operations-Digests. Ein `Missing`-Sidecar für undefinierte Ausgaben muss `Missing` bleiben; nur symbolisch nachweisbar unerreichbare Traps dürfen in einem Zertifikat erscheinen, während jeder mögliche Trap-Pfad `ContractViolation` ohne Zertifikat oder Restcode liefern muss. Eine Fortsetzung nach Traps und eine Wiederaufnahme nach Ausnahmen werden nicht modelliert, `codeFollowsTrap` wird nicht verwendet und die statische LowIR-API bleibt unverändert.

Regressionen physischer Rücksprünge prüfen direkte und indirekte Aufrufe über ungültige Inline-Bytes, erreichbare fehlerhafte Fortsetzungen, vollständige Zielmengen sowie exakte und knappe Budgets. Die Relation des vollständigen Zustands verwirft geänderte Ergebnisse. Unabhängiger C-Code muss auch nach O1/O2 die gesamte Frame-Schreibwirkung erhalten; gleiche Rückgabewerte verdecken kein geändertes Byte im Rücksprungslot.

Explizite Tests für native Überlappung prüfen echte x64-Sprünge in Immediate-Operanden, beide möglichen Zweigergebnisse und indirekte Rücksprungeinstiege innerhalb vorheriger Befehle. Synthetische Anbieter prüfen enthaltene Bereiche in beiden Erfassungsreihenfolgen, widersprüchliche Bytes auf einem nicht genommenen direkten Zweig, Code-/Leseübereinstimmung in beiden Reihenfolgen sowie Kandidatenzugriffe. Exakte und zu kleine Byte-Budgets zählen wiederholte Bytes auch über indirekte Transfers hinweg. Geänderte Ergebnisse, statische oder Schleifen-API-Nutzung und widersprüchliche Nachweise müssen Zertifikate verhindern; Änderungen der Option oder Grenze verändern die Hashes.

Tests der expliziten Grenzen prüfen unerreichbares RCL, LOCK-Speicher-XADD und REP MOVS, symbolische Pfadwidersprüche, von beliebigen Werten gesteuerte Verzweigungen sowie genaue Ablehnungen bei Eintritt, indirektem Sprung, CALL und RET. Sie prüfen unabhängige Zugänge zu erreichbaren Suffixen, gleiche Kandidaten-/Native-Adressen, fehlerhafte oder unvollständige Belege, erschöpfte Ressourcen, die Ablehnung statischer/Schleifen-APIs und alle drei Digest-Ebenen der Verfeinerung. Änderungen unerreichbarer Befehle oder der Option ohne vorhandene Grenzen ändern die Zertifikat-Digests. Dies prüft den deklarierten endlichen Beweisumfang, nicht die Semantik ungeprüfter Befehle.

Tests gepackter Flags prüfen alle skalaren Eingangsflagkombinationen, Privilegmasken, TF/AC in beiden Ausführungen, getrennte undefinierte Erzeuger, korrelierte Kopien, native Aufrufe, Geschwisterpfade, verpflichtende Endzustandsbeobachtung, fehlerhafte Belege und Ressourcenlimits. Alle möglichen Eingabepfade endlicher Schleifen müssen terminieren; ein sicherer Zweig verdeckt keinen unendlichen oder abgeschnittenen Pfad. RDSSPD/RDSSPQ prüft alle 16 allgemeinen Register in beiden Breiten, erhaltene obere Bits, unveränderte `Missing`-Belege und abgelehnte gefälschte Projektionen. Maschinenzustandstests vergleichen beide C-Wege bei O0/O2 mit Fallen für undefiniertes Verhalten gegen ein unabhängiges Benutzermodus-Flagorakel und prüfen dauerhaft gespeicherte Profilfehler. INCSSPD/INCSSPQ-Tests prüfen beide Breiten und alle allgemeinen Register, erhaltene unerreichbare Grenzen, ausführbare Traps nach einem abgeschlossenen Geschwisterpfad, Nulloperanden und gefälschte Trap-Belege.

`NeverDX86DecodeDetailTests` prüft alle drei Dekodierwege, beide x64-Adressbreiten, vorzeichenbehaftete Grenzwerte, Pflichtpräfixe, echte i386-disp16, moffs, abgeschnittene Eingaben und Wiederverwendung ohne Details. Nur genaue Relokationsfelder werden gebunden; falsche Breiten, Positionen oder Werte nicht. Native Unabhängigkeits- und Verfeinerungstests erhalten sämtliche Frame-Schreibzugriffe und verweigern beobachtete beliebige Flags sowie einen veränderten Schiebekandidaten.

`NeverDLowUndefinedDigestTests` prüft unabhängige SHA-256-Vektoren, jedes gespeicherte Feld, vorzeichenbehaftete Sequenzbits, Reihenfolge, ausgeschlossene Füllbytes und unveränderte Eingaben. Die Fälle decken das Wachstum des eingebetteten Puffers, die Grenze bei 199/200 Operationen und größere inkrementelle Bereiche ab. `LowIRRefinement.StaleUnusedInputRefusesAcrossDigestStorageBoundaries` prüft tatsächliche Ablehnung veralteter Evidenz und erneute Bindung auf beiden Pfaden. `InputDigest.*` in `NeverDLiftTests` beibehalten und betroffene Aufrufer nach Änderungen der ausgelagerten Implementierung neu bauen. Tatsächliche Sanitizer-, portable Pfad- und Host-Abdeckung angeben; diese Mikrobenchmarks zertifizieren keine native Äquivalenz.

```bash
cmake --build build-release --target NeverDLowUndefinedDigestTests --parallel 4
build-release/bin/NeverDLowUndefinedDigestTests
```

`NeverDX86UndefinedEffectsTests` prüft Metadaten undefinierter Bits, definierte oder erhaltene Flags und die Ablehnung veralteter Zertifikate. `NeverDX86CarryArithmeticFlagTests` prüft den Hilfsübertrag von ADC/SBB für Register- und Speicherformen anhand eines arithmetischen Orakels. `NeverDX86LogicIdentityTests` prüft, dass AND mit identischen Operanden im 64-Bit-Modus beim Schreiben eines 32-Bit-Ziels weiterhin die Bits 63:32 des zugehörigen 64-Bit-Registers löscht und bei schmaleren Schreibzugriffen die ungeschriebenen Bits erhält.

`X86RotateUndefinedEffects.*` prüft alle Rohzähler, Operandenbreiten, CL-Überlappungen, obere Byte-Aliasse und Speicherziele gegen ein skalares Rechenorakel. `X86BitTestUndefinedEffects.*` deckt Register-/Immediate-Indizes, Quell-/Zielüberlappungen, erweiterte Register, definierte Flags und Schreibzugriffe auf obere Registerteile ab. Metadatenkontrollen weisen veränderte Operanden, Kodierungen und nicht unterstützte Formen zurück. Native Beweise unterscheiden korrelierte Lesezugriffe von unabhängigen beliebigen Flags, prüfen genaue/zu kleine Erzeugerbudgets und verweigern beobachtbaren undefinierten Überlauf. Die vollständige Zustandsverfeinerung akzeptiert den gewählten Zeugen und lehnt Nullbit-Zeugen oder veränderte Kandidaten ab.

`X86XaddAudit.*` prüft alle 65,536 Byte-Operandenpaare, Flag-Grenzen größerer Breiten, Register-/High-Byte-Überlappungen, beide Rückschreibungen, REX-Bytebreitengrenzen und den Erhalt ganzer Register anhand vorzeichenloser Arithmetik. Native Prüfungen verlangen keine neuen beliebigen Bits, ohne frühere Abhängigkeiten zu verlieren. Beide Zeugen akzeptieren unverändertes XADD; manipulierte Summen, ausgetauschte Quellen oder definierte Flags werden abgelehnt. Der `/6`-Alias durchläuft die vollständige Schiebezählmatrix; veränderte Gruppen/dekodierte IDs müssen als semantisch inkonsistent abgelehnt werden. Speichertests prüfen zusätzlich alle Byte-Paare, Adressgrößenwechsel, Erweiterungen, IP-relative und i386-16-Bit-Adressierung, vorzeichenbehaftete Displacements, Nachbarbytes und veraltete Details. Native Prüfungen des ganzen Frames erhalten frühere beliebige Abhängigkeiten und exakte/zu kleine Budgets; eine veränderte Speicheradresse verletzt den Rückkehrslot-Vertrag.

`X86DoubleShiftUndefinedEffects.*` prüft jeden Byte-Zähler bei 16/32/64 Bit mit unabhängigen Einzelbitübertragungen, einschließlich Quell-/Ziel-/CL-Aliasen und exakten Bedingungen. Native Prüfungen verwerfen RAX zur Flag-Isolation, unterscheiden 16 von 17 und erhalten frühere Abhängigkeiten sowie exakte/zu kleine Budgets. Vollzustandsrelationen lehnen geänderte definierte Bereiche und Nullbit-Zeugen ab; ein beliebiges unteres Wort erlaubt kein Löschen definierter oberer Bits. Fehlerhafte Formen veröffentlichen keine Teilbelege.

`*Deferred*` prüft tote Sprungziele und Fallthroughs, fehlenden oder fehlerhaften Code, symbolische Widersprüche, beliebige Steuerung, Zeugen und vollständige Zustandsänderungen. Synthetische Provider laden keine unerreichbaren Nachfolger und lehnen erreichte fehlerhafte Metadaten ab. Exakte/zu kleine Befehls-, Operations-, Besuchs- und Abfragebudgets, erreichbare ungültige Alternativen und Endlosschleifen zertifizieren keine Präfixe. Richtlinien ändern Zertifikatsdigests; statische und Schleifen-APIs lehnen die Option ab.

`NeverDPEFixedImageTests` prüft mit unabhängig erstellten PE-Dateien relokierte Instruktionen, unveränderliche Daten, Import-Schreibbereiche, fehlerhafte Header/Tabellen, Aliase und geänderte Herkunftsdaten. Beweise von nativen Instruktionen zu LowIR und exaktem LLVM akzeptieren passende Kandidaten und lehnen geänderte Ergebnisse, Statuswerte oder native Bytes ab. Ein erschöpftes Vorbereitungsbudget bleibt separat erkennbar und erlaubt eine Wiederholung mit ausdrücklich erhöhten Grenzen; normales Laden akzeptiert auch 40000 gültige Relokationen oberhalb des Standardbudgets der Analyse.

`FrameOffsets.*`, `NativeStackSpecialization.*` und `OriginalBinaryUndefinedIndependence.*` prüfen alle Reste für Ausrichtungen 2/4/8/16/32, freie obere Bits, aufrufübergreifende Spills, Countdown-Schleifen, Aliasbeschädigung, falsche Verzweigung, irrelevante große Masken, notwendige Partitionserweiterungen sowie exakte und um eins zu kleine Budgets. Separate native Kontrollen prüfen bedingte Ausrichtung, interne vorzeichenlose Rückkehrbereinigung, falsche Bereinigung und präfixbehaftete Rückkehr. Diese Tests belegen keine automatische native LLVM-Beweisabdeckung partitionierter Schleifen.

Native Regressionen prüfen 64 ausgerichtete Lesezugriffe mit dem Abfragebudget eines Zugriffs, ein um eins zu kleines Budget, geänderte Adressen außerhalb des Frames und gleiche Adressen unter verschiedenen Prädikaten nach Rückkehr eines Pfads. Zustandsmutation sowie Cache-Schlüssel und Kapazität bleiben geprüft.

Regressionen zur wiederholten Erreichbarkeit behalten alle 130 nativen Instruktionen mit dem Abfragebudget einer geraden Folge aus zwei Instruktionen. Um eins zu kleine Abfrage-/Instruktionsbudgets, erreichbare Traps bei geänderten Zweigen oder Eingangsdomänen, erschöpfte Solver-Gatter und veränderte Kandidatenzustände werden abgelehnt.

Frameoffset-Regressionen prüfen 558 Kombinationen aus Teilbreite, Ausrichtung, Rest und Bias mit einem für Ganzwurzel-Subtraktion unzureichenden Gatterbudget. Abweichende Quellen/Biaswerte, freie dünn besetzte Masken, modulare Überträge und Überläufe, verschachtelte Masken sowie Knoten-/Abfrageerschöpfung werden geprüft. Native Tests bestätigen Stores über teilweise ausgerichtete Zeiger und lehnen fehlende Ausrichtung, Zugriffe außerhalb des Frames sowie veränderte Storewerte in vollständigen Zustandsrelationen ab.

Tests prüfen alle Reste bei 1/2/4/16, zwei Wurzeln, freie höhere Bits, ungültige Domänen, widersprüchliche Konstanten, Grenzen, Ausschlusslücken, Erhaltung und exakte/um eins verkürzte Abfragebudgets. Induktionsvorlagen behalten das Eintrittsprädikat. Frische native und LLVM-Beweise verweigern abweichende Domänen und geänderten Status; Digests binden beide Domänen. Die unabhängigen Fälle leiten keine Ausrichtung aus ABI oder Ausführung ab. Unabhängiges C mit Eintrittsprüfung wird unverändert bei O1/O2 für zwei Reste gegen echte native Instruktionen bewiesen; dieselben Artefakte müssen bei einem anderen Rest scheitern.

Registerfall-Regressionen prüfen beide Byteordnungen, hohe Frame-Basen, überschriebene und überlappende Felder, spätere Kanten, verbreiterte Vorgänger, natives CALL/RET sowie exakte und um eins zu kleine Budgets. Beide C-Wege führen alle vier Speicherfälle mit O0/O2 aus. Native Verfeinerungsprüfungen binden zwei Selektorwerte getrennt; sie beweisen keine unbeschränkten Eingaben. Ein eigenes LLVM-Beispiel prüft inklusive PHI-Kopien und Stores, dass ein vor den gemeinsamen Join verschobener falscher Arm nicht nach dem wahren Arm ausgeführt wird.

Die Regressionen prüfen feinere Partitionen, alle erlaubten Reste, unterschiedliche hohe Adressbits, einen Fehler im letzten Fall sowie genaue und um eins zu kleine Budgets. Beide C-Backends laufen mit O0/O2 und unzugänglichen abgewiesenen Gastadressen sowie ungültigen Flags: Status 2 muss alle Zustandsbytes erhalten. Modell- und C/Python-Tests prüfen zudem v5-Layout, Eigentum und alte oder zukünftige Anhänge. Native Beweise lehnen ungebundene Ausrichtungsbereiche ab.

`StringTransfer.*` und Kopierregressionen prüfen Überlappung, Anzahl null, temporäre Isolation, Kapazitäts- und Budgetgrenzen sowie Zeigerinvalidierung. `MachineStringSourceTests.cpp` vergleicht native Ausführung und beide C-Wege bei O0/O2 mit unabhängigen Erwartungen für alle Register, Flags und Stackbytes, für vier Breiten und beide Richtungen.

`ControlDiscovery.*` und `NativeStackSpecialization.*` prüfen Bedingungen über untere Wurzelbits, Abhängigkeiten von oberen Bits und der gesamten Wurzel, unvollständige Durchläufe, exakt ausreichende und zu kleine Durchlaufbudgets sowie den Erhalt endlicher Nachweise unveränderlicher Adressen.

Guard-Tests für unaufgelöste Ziele prüfen eine unerreichbare ungültige Phase, unbekannte Ziele am Eintritt oder nach einer Rückkante, exakte Verfeinerungslimits sowie benachbarte erfolgreiche und erschöpfte Entdeckungsbudgets. Eine Ablehnung veröffentlicht weder Restcode noch Herkunfts- oder Lesezeugen. Optionale Entdeckungsarbeit nach vollständiger Wiederherstellung ist keine Untergrenze des nötigen Budgets.

Regressionen für angeforderte Frame-Projektionen prüfen automatische und vorhandene Register, hohe Wurzeladressen und modularen Überlauf, nur das untere Byte einschränkende Bedingungen, widersprüchliche oder fehlende Fakten auf Geschwisterkanten, benachbarte Abfragebudgets und Solver-unknown. Eine schmale Bitanforderung macht aus einem Teilbeweis keinen Beweis für den ganzen Zeiger; bei Ablehnung werden weder Restcode noch Nachweise veröffentlicht.

Die Teilfeldregressionen prüfen Selektoren in beiden Hälften, beobachtbare beliebige Nutzdaten, Register und Frame-Slots, beide Byte-Reihenfolgen, schmale Felder, spätere Wertebereichserweiterungen, disjunkte Masken und fehlende Fakten. Fehlende erreichbare Ziele, freie Selektoren, erschöpfte gemeinsame Abfragebudgets und unknown-Ergebnisse des Solvers dürfen weder Restcode noch Nachweise veröffentlichen. Außerdem werden automatische Erkennung bei Anforderungen an das ganze Wort, ungenutzte manuelle Hinweise, wurzelabhängige Daten und führende konstante Fenster geprüft.

Tests der öffentlichen Arbeitsbudgets prüfen Standardwerte und 32-Bit-Maxima, getrennte Erschöpfung von Auswertung und Erkennung, beide Quell-ABIs und C-Backends sowie ungültige CLI-Werte. C/Python-Layoutprüfungen decken v7, geerbte Validierung und das Ignorieren späterer Anhänge durch v1–v6 ab. Budgetablehnungen veröffentlichen weder Quellcode noch Nachweise; der Auswertungszähler darf am Maximum nicht überlaufen.

Tests der optionalen Kettengrenze vergleichen einfache und verschachtelte Schleifen bei gleichem Operationsbudget, führen die Restschleifen aus, unterscheiden Dekodiermodi und prüfen das unveränderte Verhalten ohne Verkettung. Ein Korrelationsgegenbeispiel muss standardmäßig erfolgreich sein und mit Option die Veröffentlichung verweigern. Wiederholte native Aufrufe/Rückkehrslots, Abhängigkeits-Replay und späte Vorgänger laufen mit beiden Einstellungen. Öffentliche Tests prüfen beide Quell-ABIs/Backends, Standard- und unbekannte Flags, alte APIs mit ignorierter Erweiterung und den gemeldeten booleschen Wert.

`NativeStackSpecialization.NarrowAddressDemandRetainsCompletePointer` prüft bedingte Zeigerzusammenführungen in Registern und Frame-Slots, beide Bytefolgen, modularen Überlauf und hohe Wurzeladressen, Stack-Wiederherstellung und 120 Frame-Bytes. Zugehörige Gegenbeispiele weisen beschädigte Zeiger zurück und prüfen exakt ausreichende sowie um eins zu kleine Abfrage-, Operations-, Auswertungs- und Verfeinerungsbudgets sowie erschöpfte Such- und Kontextgrenzen.

Beobachtertests prüfen frühen Abbruch, Konstanten, leere Projektionen und die abschließende UNSAT-Abfrage. Leseregressionen behalten Laufzeitzugriffe nach einem Gegenbeispiel bei, prüfen zwischengespeicherte Adressmengen bei anderer Lesebreite erneut und weisen fehlerhafte Zertifikate ohne Veröffentlichung von Teilbelegen zurück.

Tests für affine Steuerwerte prüfen Bedingungen über niedrige Bits mit einem Budget von acht Solver-Abfragen, Register und Frame-Slots in beiden Byte-Reihenfolgen, modularen Überlauf, beobachtete Frame-Bytes und den wiederhergestellten Stack. Eine widersprüchliche Bedingung ohne literales false muss einen nicht unterstützten Zweig ausschließen; zwei Wurzelwerte mit gleichen unteren 32 Bits und unterschiedlichen oberen Bits müssen beide indirekten Ziele erhalten.

`FiniteQueryCache.*` prüft genau ausreichende und um eine Einheit zu kleine Speichergrenzen, die Aktualisierung durch Treffer, die Verdrängung mehrerer unterschiedlich großer Einträge, wiederholte Verdrängung und Abfragen mit umbenannten Variablen. Doppelte Speicherungen, Fehltreffer, fehlerhafte Ergebnisse und übergroße Kandidaten ändern die Reihenfolge nicht. Bereits zurückgegebene Beweiskopien bleiben nach der Verdrängung ihrer Cache-Einträge gültig.

`ControlStateRecovery.*Marginal*` prüft unabhängige Ziel- und Geschäftsdatenbereiche, deren Produkt die gemeinsame Grenze überschreitet, konkrete Ergebnisse des Restprogramms, ein später hinzugefügtes Ziel nach Vergröberung der Relation, fehlende erreichbare Ziele sowie das vollständige Verwerfen übergroßer oder unvollständig aufgezählter Bereiche. Die eigenständig erstellten Fixtures prüfen erneute Analyse bei Bereichsänderungen und verhindern die Veröffentlichung partieller Graphen nach Fehlern. Eine Frame-Slot-Variante prüft die Aliasinvalidierung nach Vergröberung der gemeinsamen Relation für beide Byte-Reihenfolgen.

`InterpreterTransferChain.*` prüft korrelierte Werte, eindeutige und dynamische Zweige, spätere Vorgänger, Frame-Grenzen, Aliasablehnung und Budgets. Zusätzliche native CALL/RET-Tests prüfen wiederholte Vorkommen, Rückkehrslot-Bytes und Stack-Wiederherstellung. Produzentenwiederholung und Native-zu-LLVM-Kontrollen decken Vertragsabweichungen, geänderte Nachweise, falsche Ergebnisse und fehlende Stack-Schreibzugriffe ab.

`NeverDX86NoIndexAddressTests` prüft x86-SIB-Adressierung ohne Index bei 32 und 64 Bit Adressbreite: ignorierte Skalierungsbits, Zielbreiten, Laden/Speichern, vollständige Metadaten undefinierter Ausgaben, Segmentoffsets und Adressherkunft. Pseudoregister als Basis oder Index falscher Breite werden abgewiesen; durch REX.X ausgewählte echte R12-Indizes bleiben erhalten. EVEX-Tests für Broadcast und maskierte Transfers prüfen diese Formen, unterdrückte inaktive Speicherzugriffe und widersprüchliche SIB-Metadaten.

Schiebetests decken alle rohen 8-Bit-Zähler, Flagkombinationen bei null, beide x86-Modi, alle Breiten, CL-Aliase, AH/CH/DH/BH, erweiterte Register und Speicher ab. Bytegenaue symbolische Ausführung wird mit wiederholter Ein-Bit-Arithmetik verglichen. Relationale Tests prüfen kopierte und neue Flags, Speicherablagen, Schleifenbesuche, undefinierte Zählerquellen, Zweige, ungültige Formen, Digests und Budgets. Endliche unveränderliche Lesezugriffe prüfen 1/2/4/8 Bytes, eingabeabhängige Auswahl, pfadabhängige Einzeladressen, vollständige Belege und Grenzen; abhängige, fehlende, beschreibbare, nicht dateigestützte, relokierte und unbeschränkte Kandidaten werden abgelehnt.

Die Kerntests prüfen Kontexttrennung, Fixpunkt-Zusammenführungen, dynamische Schleifen, überlappende Register, Alias-Invalidierung, endlichen Dispatch und Ablehnung ohne teilweisen Ersatz. Die Quelltexttests assemblieren eigenständige x64-Maschinen mit Registern, Stack und endlichen Adressen; sie umfassen zusammenhängende Kontrollfelder und ein unabhängiges natives SysV-/Win64-Oracle. Beide C-Pfade werden unter O0/O2 mit Fallen für undefiniertes Verhalten kompiliert und mit einem vorzeichenlosen Oracle für Berechnungen, Speicherzugriffe und Ausgabewächter verglichen. Negative Fälle prüfen fehlende Zertifikate und unzureichende Budgets. Die öffentliche CLI und ihre Berichte prüfen Kontrollfelder, Budgets, Zähler und Ablehnungen. Benötigt werden Clang mit Cross-Target-Unterstützung und LLD; die Ausführung des ursprünglichen ELF erfordert außerdem einen x64-Linux-Host. Fehlende Werkzeuge oder ein ungeeigneter Host bedeuten übersprungene Abdeckung, keinen Erfolg.

`ControlStateRecovery.LongTransparentLoop*` prüft eine unabhängig entwickelte Schleife mit 20 Phasen, dynamische arithmetische Orakel, die Ablehnung unbekannter Selektoren und Budgeterschöpfung. `LongTransparentPhasesKeepExactBitDemands` prüft, dass unabhängige Bits im Byte des Selektors beobachtbare Laufzeitdaten bleiben und nicht zu Kontrollanforderungen werden. `ProducerClosureChargesWorkBeforeAnotherRestart` prüft, dass Rückwärtserkennung und erneute Auswertung bereits vor dem Start eines neuen Graphen gemeinsame Budgets verbrauchen und kein Teilergebnis veröffentlichen.

`X86ShiftCarry.*` prüft das Carry schmaler arithmetischer Rechtsschiebeoperationen,
maskierte Zähler sowie APX-Zielregister und Flag-Unterdrückung anhand wiederholter
Ein-Bit-Schritte. `NarrowArithmeticShiftCarrySurvivesBothSourceBackends` führt beide
rekonstruierten C-Ausgaben bei O0/O2 mit Fallen für undefiniertes Verhalten aus,
einschließlich aller Bytewerte und rohen Schiebezähler.
`NeverDLLVMCIntrinsicSemanticTests` führt vorzeichenbehaftetes und vorzeichenloses
Integer-Min/Max für i1/8/16/32/64/128 bei O0/O2 aus und prüft zugewiesene und
eingebettete Ergebnisse, Erzeugerreihenfolge und einmalige Auswertung. Nicht
unterstützte skalare Breiten und fehlerhafte Operanden müssen explizit scheitern.

## Funktionsgebundener x64-x87-Kontext

`NeverDX86X87ConvertStateTests` vergleicht Originalbytes mit nativem LLVM, HighC und LLVMC bei default/NoOpt und C O0/O2. Seine Fixtures erzwingen den eigenen Kontext auf jedem Host über `BinaryImage::X87ContextOverride`; die Integer-Lade-, Vorzeichen- und x87-FPU-Suiten prüfen zusätzlich die typisierte System-V-Projektion. Geprüft werden Konvertierungen, acht Rechenformen, Flags, MMX/EMMS, volle/leere Stacks, Aufrufe, Wrapper, Einstiegsschleifen und unabhängige Einstiege. Vier FLDENV/FRSTOR-Formate prüfen ausstehende Ausnahmen, Tags und gespeicherte Werte; soweit anwendbar alle vier RC, drei definierten PC und 64 Ausnahmemasken. Low/Med/High-Verträge lehnen falsche Formen, gefälschte Handles und abgeschnittene Selektoren ab. Schmale Rückgaben erhalten Aufrufeffekte und verweigern benötigte unbekannte Bits. Das native Orakel benötigt x64 und Clang; vollständige Befehls-, Plattform-, CPU-, Fehlerprioritäts- oder typisierte Gleitkomma-Import-ABI-Abdeckung wird nicht behauptet.

## Kontrollfluss und Aufrufe in strukturiertem C prüfen

`HighControlFlowSemantics.*` prüft, dass beim Verschieben von Schleifenausstiegen oder nachfolgenden Blöcken die von anderen Sprüngen verwendeten Labels erhalten bleiben. Direkte Einstiege in Ausstiege am Schleifenanfang und -ende sowie der Ersatz von break werden im erzeugten C mit O0/O2 gegen unabhängige Rückgabewerte geprüft.

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` prüft, dass erkannte erforderliche Registerargumente auch unbekannte Positionen am Ende behalten. Das Auswerten eines unbekannten erforderlichen Arguments oder einer unbekannten Bedingung muss explizit eine Trap auslösen; ausgelassene, nullwertige und verschachtelte Operanden dürfen nicht stillschweigend zu null werden. Bekannte Werte und nachweislich ungelesene zusätzliche Operanden bleiben ausführbar. Eine Trap ist eine Diagnosegrenze und kein Beweis für gleichwertiges rekonstruiertes Verhalten.

## Objektspeicher in LLVM C

`NeverDLLVMCAllocaTests` führt ursprüngliches LLVM und erzeugtes C bei O0/O2 mit Fallen für undefiniertes Verhalten aus. Beobachtet werden vollständige Bereiche, gepolsterte Schrittweite, 16/32/64-Byte-Ausrichtung, unabhängige gleichzeitige/Schleifen-Allokationen und einmalige Auswertung der Anzahl. Normale und typerhaltende C-Wege behalten Speicher; nicht unterstützte oder überlaufende Verträge und stack save/restore scheitern vor DCE. Ursprüngliche Stack-Adressen und Ausnahmeframe-Identität werden nicht zertifiziert.

Dasselbe Ziel vergleicht die Ausrichtung statischer Skalare, Zeiger, Arrays und gepackter Strukturen bis 128 Byte, globale Ausrichtung, den vollständigen Speicher nullaufgefüllter Zeichenketten und die globale Adressidentität in Initialisierungen, Rückgaben, Aufrufen, Auswahlen und Zeiger-Ganzzahl-Konvertierungen. Geprüft werden soweit anwendbar normale/typerhaltende und vollständige/ausgewählte Funktionsausgaben.

## Prüfungen der CPU-Ausführung

`NeverDIntegerABITests` baut originale Clang-Fixtures für Windows x64, Linux x64 und Linux ARM64. Funktionen mit zehn Argumenten prüfen echte Register-/Stackparameter und ausgeglichene Aufrufrahmen. Die Unicorn/KVM/WHP-Matrix überspringt nicht verfügbare Host-/ISA-Paare ausdrücklich; ein Skip ist kein Pass. `NeverDExecutionBudgetTests` prüft gemeinsame Fortsetzungsbudgets, Reservierungen und absolute Deadlines ohne timingabhängiges Warten.

`NeverDCPUEmulationTests` deckt ARM64-Befehle, Kontrollfluss, Loads, CPU-Kontexte, Aliase, Cache-Invalidierung und begrenzte Schleifen ab; das Softwareprofil führt zusätzlich FP/SIMD und TLS aus. `NeverDUserExecutionTests` prüft CPL3/EL0-Seitenrechte, Aliase, Schutzfehler, Kontexte und Adressraumwechsel. `NeverDServiceRequestTests` belegt, dass SYSCALL/SVC vor dem Transport abgefangen werden und Zustand sowie Exactly-once-Verbrauch erhalten bleiben. Das ist ein Übergabeprotokoll, kein vollständiges OS-Dienstemodell. `NeverDExecutionConfigurationTests` prüft gemeinsame Auflösung, Build-Unterstützung gegenüber Live-Probe und fail-closed Konfigurationen. Öffentliche SDK/CLI-Tests benötigen kein Windows-Modell. `NeverDThreadPointerTests` prüft FS-Basis, `TPIDR_EL0`, Kontextwiederherstellung und Rechte. `NeverDKvmCancellationTests` nutzt einen nicht endenden x64-Gast, um aktive KVM-Unterbrechung, Wiederaufnahme und unveränderten Signalzustand zu prüfen; ohne KVM wird ausdrücklich übersprungen.

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests NeverDCPUEmulationTests NeverDUserExecutionTests NeverDServiceRequestTests NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget|CPUEmulation|UserExecution|ServiceRequest|ExecutionConfiguration)Tests$' --output-on-failure
```

Fehlende ARM64-Hardware oder Hypervisoren bedeuten ausgelassene native Abdeckung, keinen Erfolg. Unicorn und Cross-Compilation belegen keine native KVM/WHP-Ausführung.

`NeverDParallelExecutionTests` erzwingt überlappende Prozessoraufrufe, Abbruch wartender Schreiber, unabhängige CPU-Zustände, Alias-Konkurrenz und private Übertragung. `NeverDRunControlTests` prüft zwei gleichzeitige unabhängige WHP-Leases und den gemeinsamen seriellen Cache. `NeverDMMIOAtomicTests` vergleicht RAM/Gerät für originale x64-Atomic-/Update-Befehle und alle ARM64-LSE-Fälle, beide breiten Beobachtungen, veraltete Vorschauen, Fehler und Commit/Stop-Rennen. `KernelMMIOFailure` prüft Aliase, identische Schreibwerte, doppelte Commits, Stromwechsel, Unmap und freigegebene Besitzer. Fehlende Plattformen werden ausdrücklich übersprungen. Das Wrapper-Rendezvous beweist gleichzeitige Aufrufe, nicht gleichzeitigen Hardware-Retirement. ARM64-KVM/WHP benötigt passende Hosts.

```bash
cmake --build build-cpu --target NeverDParallelExecutionTests NeverDMMIOAtomicTests NeverDRunControlTests --parallel 4
ctest --test-dir build-cpu/unittests/emulation -L '^NeverD(ParallelExecution|MMIOAtomic|RunControl)Tests$' --output-on-failure
```

## Tests des Linux-Prozessprofils

Die unabhängigen [Prozesstests](process-emulation.md#verifikation) kompilieren echte x64-/AArch64-ELF-Fixtures. `NeverDLinuxProcessTests` prüft Start, Program-Header-Policy, Service-Fortsetzungen, Binärausgabe, Gastfehler und Ressourcenstopps. `NeverDProcessPublicTests` testet C-API/CLI ohne Änderung des Analyse-Images. `NeverDExecutionSessionTests` prüft zwei CPUs mit gemeinsamem Speicher/Budget sowie Exactly-once-Verbrauch von Requests/Fehlern. `NeverDX64MemoryUpdateTests` prüft Speicherarithmetik, SETcc, BT, XMM/MXCSR, Schreibbeobachter, REP-Grenzen und vorbereitete Geräte-Lesezugriffe. `DriverBackendParityTests.cpp` führt originale und relokierte WDK-Fixtures aus und vergleicht den vollständigen beobachtbaren Bericht mit Unicorn; fehlende Images/Backends werden übersprungen.

Geprüftes x64 erlaubt auch maskierte Legacy-Formen `SS`, `SD`, `PS`, `PD` von `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`. `X64SSEInstructions.def` verwaltet Breiten, Ausrichtung und Zulassung zentral. `MaskedSSEArithmeticMatchesIndependentHostExecution` vergleicht Register/RAM mit einem unabhängigen Host-CPU-Orakel: vier Rundungsmodi, FTZ, vorzeichenbehaftete Nullen, Subnormalzahlen und NaNs. `SSEMemoryObserverStopsBeforeResultAndStatusChanges` prüft den Stopp vor Effekten. Unmaskierte Ausnahmen, x87 und AVX bleiben ausgeschlossen.

`X64PackedIntegerTests.cpp` prüft eigene Kodierungen und 180 feste Ergebnisse aus `X64PackedIntegerCases.def` unabhängig gegen native x64-Compiler-Intrinsics. Registerfälle und RAM-Aliase am Seitenende bewahren andere XMM, Ganzzahlprüfwerte, FLAGS, MXCSR und Quellbytes. Beobachterabbrüche/-fehler und behebbare Lesefehler bewahren den Zustand; nach Reparatur folgt ein erneuter Versuch. Fehlausrichtung löst `#GP(0)` aus; MMX, LOCK und MMIO werden weiterhin vor Callbacks abgewiesen. Native CI verlangt beide WHP-Privilegstufen.

`X64PackedShiftTests.cpp` und die eigenen Fälle in `X64PackedShiftCases.def` vergleichen zehn Schiebeoperationen mit unabhängigen skalaren Berechnungen und nativen SSE2-Intrinsics bei 16 unmittelbaren und 21 variablen Zählern. Sie prüfen Zähler-/Ziel-Aliase, ignorierte hohe Bits, Ausrichtung, Beobachter, behebbare Fehler und Geräteablehnung. `X64VectorTestSupport.h` teilt die Register- und RAM-Prüfungen mit gepackten Arithmetiktests. Beide WHP-Privilegmodi sind in nativer CI verpflichtend.

`X64VectorMaskTests.cpp` vergleicht unabhängige Rohkodierungen mit skalarer Bitextraktion und nativen SSE-Intrinsics: jedes Quellbit und alle 16 GPR × 16 XMM mit beiden REX.W-Werten. Vollständige öffentliche Registerzustände, RAM und Datenbeobachter prüfen Nullerweiterung und Zustandserhalt. Befehlsstopps, Callback-Fehler und nicht unterstützte Formen dürfen keine Effekte veröffentlichen. Die native KVM/WHP-Abnahme verlangt beide Privilegstufen.

`X64ShuffleTests.cpp` nutzt unabhängige Kodierungen aus `X64ShuffleCases.def` und vergleicht skalare Lane-Auswahl mit nativen Intrinsics. Geprüft werden alle 256 Steuerwerte mit Registern, identischer Quelle und Alias am Seitenende, sämtliche XMM-Paare, vollständiger öffentlicher CPU-Zustand und RAM, Beobachterstopps und -ausnahmen, Rechte, Ausrichtungsfehler und Wiederholungen. MMX, VEX/EVEX, LOCK und Geräteoperanden müssen ohne Wirkung abgewiesen werden. Die native KVM/WHP-Abnahme verlangt diese Fälle auf beiden Privilegstufen; nicht verfügbare Host/ISA-Paare werden ausdrücklich übersprungen. Die Fälle `UNPCKLPS`, `UNPCKHPS`, `UNPCKLPD` und `UNPCKHPD` verwenden dieselbe Zustands- und Fehlermatrix sowie unabhängige skalare und native Vergleiche. Jede XMM-Zielposition wird auch mit einer Speicherquelle geprüft.

`X64PartialMoveTests.cpp` und `X64PartialMoveCases.def` vergleichen unabhängige skalare/native Lade- und Speicherreferenzen, alle 16 XMM-Register und rohe NaN-/Subnormal-Bits. Vollständiger CPU-Zustand und zwei RAM-Seiten werden bei nicht ausgerichteten Zugriffen, Aliasen, Seitengrenzfehlern, reparierten Rechten, Beobachterstopps/-fehlern und Wiederholungen geprüft. Am Seitenende reichen acht Bytes; Speichern benötigt kein Leserecht. Registeraliase, abgelehnte Formen und Geräte-Callbacks werden separat geprüft; native KVM/WHP-Fälle sind auf beiden Privilegstufen verpflichtend.

`X64IntegerFloatTests.cpp` nutzt unabhängige Kodierungen aus `X64IntegerFloatCases.def`, `APFloat`-Erwartungen und native Befehle mit gesichertem/wiederhergestelltem FP-Zustand. Geprüft werden beide Ganzzahlbreiten, vier Rundungsmodi, haftender Präzisionsstatus, FTZ, jedes GPR/XMM-Paar und vollständiger CPU/RAM-Erhalt. Nicht ausgerichtete, seitenübergreifende und am Seitenende liegende Quellen, reparierte Rechte, Beobachterstopps/-fehler und Wiederholungen behalten exakte Zugriffsbereiche. Native KVM/WHP-Fälle sind auf beiden Privilegstufen verpflichtend.

`X64FloatIntegerTests.cpp` prüft Rundung und Abschneiden mit unabhängigen Kodierungen aus `X64FloatIntegerCases.def`, `APFloat` und nativen Register-/Speicherbefehlen. Vorzeichengrenzen, Halbwerte, NaNs, Unendlichkeiten, subnormale Werte, alle Rundungsmodi, bleibende Statusbits und FTZ decken beide Ganzzahlbreiten ab. Jedes GPR/XMM-Paar, der gesamte CPU/RAM-Zustand, exakte Lesezugriffe am Seitenende, behebbare seitenübergreifende Fehler und Beobachterabbruch mit Wiederholung werden geprüft. Native KVM/WHP-Ergebnisse auf beiden Privilegstufen sind verpflichtend.

`X64SSEComparisonTests.cpp` nutzt unabhängige Kodierungen aus `X64SSEComparisonCases.def`, `APFloat`-Ordnung und native Befehle mit gesicherten/wiederhergestellten Host-FLAGS und FP-Zustand. Alle Paare aus 21 Rohwerten decken NaN-/Denormal-Priorität, Nullen, Unendlichkeiten und Nachbarwerte ab. Geprüft werden alle XMM-Paare und Aliase, bleibendes MXCSR, Rundungsunabhängigkeit, DF-Erhalt, vollständiger CPU/RAM-Zustand, exakte Seitenend-/Seitengrenzlesezugriffe und Beobachterabbruch mit Wiederholung. Native KVM/WHP-Ergebnisse auf beiden Privilegstufen sind verpflichtend.

`X64SSEPredicateTests.cpp` nutzt unabhängige Prädikate aus `X64SSEPredicateCases.def`, gemeinsame Rohwerte aus `X64SSEComparisonCases.def`, `APFloat`-Ordnung und originale native Befehle. Geprüft werden alle Eingabepaare, Ausnahmepriorität gemischter Lanes, obere skalare Lanes, XMM-Aliase, vollständiger CPU/RAM-Zustand, Seitenend-/Seitengrenzlesezugriffe, Ausrichtungspriorität und Wiederholung nach Beobachterabbruch/Fehler. Direkte Capstone-Prüfungen decken alle Steuerbytes, beide Syntaxformen und APIs sowie 32/64-Bit-Modi ab. Native KVM/WHP-Ergebnisse sind auf beiden Privilegstufen Pflicht; reservierte Werte, VEX/EVEX und Geräteoperanden bleiben ausgeschlossen. Wertematrizen werden nach Befehl und Prädikat, Rundungs- und Steuermatrizen nach Befehl aufgeteilt. `NativeCPUTests.def` verlangt weiterhin alle ursprünglichen Kombinationen; Gastfristen und die 15-Sekunden-Frist von CTest bleiben unverändert. Übersetzungszeitprüfungen verlangen jedes Befehl/Prädikat-Paar genau einmal.

`X64SSEPrecisionTests.cpp` verbindet unabhängige Kodierungen aus `X64SSEPrecisionCases.def`, `APFloat`-Präzisionsrundung und originale native Befehle. Bereichsprüfungen runden mit unbeschränktem Exponenten, einschließlich gerichtetem endlichem Überlauf und winzigen Ergebnissen, die zu normalen Zahlen runden. Geprüft werden beide Vorzeichen, NaN-Nutzdaten, alle Rundungs-/FTZ-/Statusvarianten, Lane-Aggregation, XMM-Aliase, vollständiger CPU/RAM-Zustand, exakte Quellbreiten, Ausrichtungspriorität, Seitenfehler und Beobachterabbruch/Wiederholung. KVM/WHP sind auf beiden Privilegstufen Pflicht.

`X64PackedFloatTests.cpp` nutzt unabhängige Kodierungen aus `X64PackedFloatCases.def`, vorzeichenbehaftete `APFloat`-Erwartungen und originale native Befehle. Alle Eingabepaare, Ganzzahlgrenzen, Rundungsmittelpunkte, Rundungsmodi, Status und FTZ werden geprüft. Aliase, alle XMM-Paare, vollständiger CPU/RAM-Zustand, jede m64-Seitenteilung, Seitenenden, Ausrichtungspriorität und Beobachter-/Fehlerwiederholungen decken beide Privilegstufen ab. Alle KVM/WHP-Fälle sind Pflicht.

`X64PackedFloatIntegerTests.cpp` verbindet unabhängige Kodierungen aus `X64PackedFloatIntegerCases.def`, Eingaben aus `X64FloatIntegerCases.def`, `APFloat`/`APSInt` und originale native Befehle. Alle Paare aus 43 Roheingaben prüfen signed32-Grenzen, Nachbarn von Rundungsmittelpunkten, NaN, Unendlichkeiten und Subnormale. Separate Matrizen variieren exakte, ungenaue, ungültige und subnormale Lanes unabhängig. Alle Rundungs-/FTZ-/Statusvarianten, XMM-Aliase, CPU/RAM-Zustände, ausgerichtete Seitenenden, Ausrichtungspriorität und Beobachter-/Fehlerwiederholungen werden geprüft. KVM/WHP sind auf beiden Privilegstufen Pflicht.

`DAZBackends` erweitert die Matrizen für Vergleiche, Prädikate sowie skalare und gepackte Ganzzahl-, Gleitkomma- und Präzisionskonvertierungen um aktiviertes DAZ. `X64DAZTestSupport.h` normalisiert Eingaben unabhängig mit `APFloat` und prüft vor nativen Referenzbefehlen die `MXCSR_MASK` des Hosts. Die Tests vergleichen vorzeichenbehaftete Nullen, subnormale Werte, NaNs, gemischte Lanes, alle Rundungsmodi, FTZ und persistente Statusbits; Quellbytes, andere Register, FLAGS und der gesamte RAM werden auf Erhaltung geprüft. Register-, Alias- und Seitengrenzoperanden behalten ihre Zugriffsbeobachtungen. KVM/WHP verlangen alle DAZ-Fälle auf beiden Privilegstufen und die 17 Referenzfälle mit Originalbefehlen des Hosts; portable Läufe überspringen ungeeignete Hosts ausdrücklich. Bestehende Fälle ohne DAZ und Zeitlimits bleiben erhalten.

`X64AlignmentTests.cpp` prüft, dass fehlausgerichtete Operanden zugelassener aligned-SSE-Befehle einen behebbaren oder terminalen `#GP(0)` vor Datenbeobachtern, Rechteprüfungen oder Geräte-Callbacks melden. Der gesamte öffentliche x64-Registerkontext, PC und RAM bleiben erhalten. Der Adressbreitenumlauf erfolgt vor der Addition von FS/GS; nach Adresskorrektur wird derselbe Befehl wiederholt. Direkte KVM/WHP-Maschinentests prüfen die Hardwaregrenze unabhängig. Windows ring3 stellt klassifizierte `operand_alignment`-Fehler zu; andere Ursachen für `#GP` bleiben ununterstützt.

`X64SIMDExceptionTests.cpp` umgeht die checked-Zulassung und prüft den nativen `#XM`-Transport von KVM/WHP auf beiden Privilegstufen. Acht eigene Fälle in `X64SIMDExceptionCases.def` decken sechs Ausnahmetypen ab, einschließlich exakter winziger Ergebnisse und eines bei unbegrenztem Exponenten exakten Überlaufs. Register- und RAM-Formen bewahren bei Fehlern den gesamten GPR-, XMM-, x87-, FLAGS-, FS/GS- und Gastspeicherzustand bis auf den vorgeschriebenen MXCSR-Status. Das Maskieren erlaubt die Wiederholung des Originalbefehls; reparierte Operanden bei erhaltenen Statusbits prüfen, dass alte Flags keine neue Ausnahme auslösen. Auch öffentliche checked- und Treiberverträge führen diese ursprünglichen Fehler- und Wiederholungsfälle aus. `WindowsSIMDExecutionTests.cpp` führt einen echten nativen Fehler, Gast-VEH/VCH-Befehle und Fortsetzungen durch Überspringen, Maskieren oder Operandenreparatur aus. Aktive Steuerzustände und gespeicherter Kontext werden getrennt geprüft. Negative Starttests verweigern fehlende Fehler, falsche Vektoren und veränderte Ziele ohne Veröffentlichung von Fähigkeiten.

`check_windows_simd.py` erstellt aus `WindowsSIMDCases.def` und den skalaren Fällen ein unabhängiges originales Windows-x64-Programm. Seine 6.144 Beobachtungen decken Register/RAM-Operanden, alle Ausnahmemasken, gelöschte oder vollständig gesetzte Statusflags und drei Fortsetzungen ab: Überspringen, Maskieren und Wiederholen oder Operanden reparieren und bei unveränderten Masken wiederholen. Assemblereinstiege erfassen die tatsächlichen MXCSR- und x87-Zustände von VEH/VCH getrennt vom gespeicherten `CONTEXT`. Geprüft werden Fehler-PC, Zustandserhalt, reparierter Kontext und Ergebnis vor der Wiederherstellung des Hosts. Die CI bewahrt Rohdaten und Quellhashes auf. `--build-only` belegt nur die Kompilierung. Diese Beobachtungen aktivieren kein unmaskiertes checked-SIMD und belegen keine native ARM64-Ausführung.

`WindowsSIMDStatusCases.def` fixiert alle 63 nicht leeren Kombinationen aktiver Statusbits aus nativem Windows. `WindowsSIMDMappingTests.cpp` prüft genaue Codes und Parameter, weist inkonsistente Fehler und ungültige Steuerwerte zurück und injiziert die Fehlergrenze zur Prüfung der Datensätze, beider CONTEXT-Steuerwerte und einer maskierten Fortsetzung. Das originale Windows-CI-Programm prüft die fixierten Ergebnisse unabhängig. Der injizierte Test belegt keine Unicorn-Ausnahmezustellung und aktiviert kein unmaskiertes SIMD in checked-Ausführung.

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
```

Nicht verfügbare Backends werden übersprungen. Cross-Compilation und Unicorn ARM64 sind kein Nachweis für natives KVM/WHP.

## Prüfungen der Treiberemulation

Aktivieren Sie `NEVERD_ENABLE_DRIVER_EMULATION=ON` zusammen mit `BUILD_TESTING=ON`,
um die gezielte Ausführungssuite und die Prüfungen der öffentlichen C-API/CLI
über die Shared Library zu bauen:

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

Fixtures prüfen Gastinitialisierung, zurückgegebenen Erfolg und Fehler,
nicht unterstütztes Verhalten, Speicherfehler, striktes Szenarioparsing,
begrenzte Ausführung, synchrone gepufferte/direkte I/O, READ/WRITE, unabhängige
Dateilebensdauern, MDL-Rechte, dynamische Exportauflösung, Gast-Varargs und
strukturierte CPU-Fehler über create, Transfers, cleanup, close und unload. Verwenden Sie die [`emulate-driver`-CLI](driver-emulation.md),
um JSON und Prozess-Exitcodes zu prüfen. Produktionsbuilds dürfen diese
Funktion mit `BUILD_TESTING=OFF` aktivieren; `libneverd` darf keine ausschließlich
für Tests bestimmte Unicorn-Konfiguration benötigen.

Zusätzliche Tests prüfen treibereigene MDLs für nicht auslagerbaren Pool, unabhängige Lebenszeiten von Deskriptor und Puffer, Registry-Abfragelayouts und kurze Puffer, Handle-Rechte, Löschung und Lecks sowie die vollständigen 64 Bit von `information_hex` bei IOCTLs ohne Ausgabe. Die externe Validierung umfasst auch synchrone direkte Lese-/Schreibzugriffe und Statistikabfragen von Zero.

Backend-Tests prüfen vollständige CPU-Kontexte (Register, Flags, SIMD, FPU, CR8), gemeinsamen Speicher und das Zurückweisen fremder oder fehlerhafter Kontexte. Kompilierte `driver_dispatcher.c`-Fixtures führen echte DPC-/Work-Item-Callbacks aus und prüfen Timergrenzen, Benachrichtigungs-/Synchronisationsobjekte, nicht alertable `KernelMode`-Warten mit Grund `Executive`, Timeout/Delay, mehrere blockierte Stacks, nach Set/Reset erhaltenes Aufwecken, Argumente und IRQL-/Lebensdauerfehler. Work-Item-Tests behalten Pending/Abschluss-, Warteschlangen-, Stau- und Budgetprüfungen. Dies belegt den beschriebenen Teilumfang, keine vollständige asynchrone Windows-Unterstützung.

`DriverThreadPriorityTests.cpp` führt den originalen kompilierten Treiber `driver_thread_priority.c` explizit auf Unicorn/KVM/WHP im driver- und checked-Vertrag aus. Die Fälle prüfen Prioritätsänderungen bereiter und wartender Threads, Ereignis-/Timerwecken innerhalb eines Quantums, Rotation gleicher Priorität, DISPATCH_LEVEL-Maskierung und Timerfortschritt trotz verhungernder niedriger Threads. Zwei verglichene Zählschleifen belegen das exakt erhaltene Restquantum. Modelltests prüfen vorzeichenbehaftete ABI-Werte, unveränderten Zustand bei Ablehnung, beendete Referenzen, verschachtelte Identität und Stackwiederverwendung unabhängiger Callbacks. Native Fälle sind in `NativeDriverTests.def` verpflichtend; fehlende lokale Transporte bleiben explizite Skips.

`DriverMutexThreadTests.cpp` führt vier originale WDK-Modi aus `driver_seh_mutex.def` aus: Rekursion in einem SEH-Filter, Erwerb durch Filter oder außergewöhnliches finally und Fortsetzung eines blockierten Filters nach Mutex-Freigabe durch einen anderen Systemthread. Unicorn/KVM/WHP prüfen driver- und checked-Verträge mit normalen/aktiven CFG-Abbildern, bevorzugten/verschobenen Adressen sowie kooperativen Quanten und Quanten von 1/17 Instruktionen. Modelltests prüfen zudem APC-Unterdrückung nach Entfernen des verschachtelten Stacks, fremde Freigabe und die äußerste Rückkehr; KVM/WHP-Fälle sind in `NativeDriverTests.def` verpflichtend.

`KernelWaitSetTests.cpp` führt sechzehn Modellfälle ohne Unicorn aus: partielles `WaitAll`, erstes bereites `WaitAny`, gespeicherte Indizes, Freigabe bei Zeitablauf, ungültige spätere Objekte/Speicher, 64er-Grenze, IRQL, erhaltene beendete Threads und zwei Synchronisationstimer. `DriverMultipleWaitTests.cpp` führt sieben eigene WDK-Modi aus `driver_wdm_multiple_wait.c` und `DriverMultipleWaitCases.def` auf Unicorn/KVM/WHP aus, mit beiden Treiberverträgen, normalen/CFG-Images, Relokation und kooperativen/1/17-Instruktionsquanten. Die 30 Modell-/Nativ-Ergebnisse sind in `NativeDriverTests.def` verpflichtend. Regressionen prüfen außerdem doppelte Abschlüsse nach Erfolg oder Zeitablauf, veränderte erfasste Zustände und wiederholte Abschlüsse einer Verzögerung.

`KernelMultipleWait.DispatcherScalarParametersIgnoreUpperRegisterBits` prüft belegte obere Bits, vorzeichenbehaftete Grenzen, Überläufe und unveränderte Objektzustände. Die nackten Tail-Call-Wrapper in `DriverMultipleWaitCases.def` führen dieselben gültigen ABI-Aufrufe über echte WDK-Imports in `driver_wdm_multiple_wait.c` aus, einschließlich aktivem CFG, Relokation und Präemption auf Befehlsebene.

`driver_context_limits.c`: IRQL-Obergrenzen stehen in `KernelAPIIRQL.def`; argumentabhängige Regeln prüft das zuständige Modell. DPCs dürfen weder Registry-APIs aufrufen noch paged Pool allokieren, freigeben oder darauf zugreifen. Unicode-Konvertierungen von `DbgPrint` erfordern `PASSIVE_LEVEL`; unterstützte ANSI-Ausgabe und nonpaged Operationen bleiben auf `DISPATCH_LEVEL` nutzbar. Callback-Stacks sind begrenzt: Ein ausbrechender Stackpointer darf nicht den Stack eines anderen blockierten Workers erreichen. Aktive Timer in der Geräteerweiterung verhindern vorzeitige Gerätefreigabe. Allgemeine IRQL-Wechsel werden dadurch nicht bereitgestellt.

`KernelDeviceStackTests.cpp` prüft getrennte Eigentums-/Anbindungsgraphen, oberstes Ziel, Fehleratomizität, Kapazität, opake Felder, Handle-Zähler, Arbeits-/Anforderungsreferenzen über Trennen/Löschen sowie Datei- gegen Dispatch-Identität. Das originale `driver_wdm_stack.c` verwendet echte WDK-Header und Inline-Copy/Skip/SetCompletion; optionale Pfade `NEVERD_WDM_STACK_FIXTURE`/`NEVERD_WDM_STACK_CFG_FIXTURE` wählen normale/aktive-CFG-Bilder. `DriverWDMStackTests.cpp` prüft Relokation, unteren Status, Completion-Reihenfolge/Flags, späte pending-Weitergabe, Worker/DPCs, Warten, `STATUS_MORE_PROCESSING_REQUIRED`, direkte MDL-Lebensdauer, verschachtelten Abschluss und fehlerhafte Cursor/Steuerung. `DriverScenarioPublicTests.cpp` prüft C-API/CLI-Weiterleitung und zurückgehaltene/verschachtelte C-API-Abschlüsse, auch mit konfiguriertem CFG. Fehlende Artefakte werden sichtbar übersprungen. Linux-Nachweise belegen nur Stapel desselben Treibers, keine PDO/PnP/Power-Unterstützung. `KernelIRPStackTests.cpp` prüft gezählte Cursor, vollständige Inline-Copy-Präfixe, geleerte Positionen, Status-/pending-Weitergabe, MPR und verschachtelten Abschluss, Fortsetzungseigentümer und gehaltene Routen. Echte READ/WRITE- und Datei-Lebenszyklen verwenden ebenfalls Inline-Copy.

`DriverPnpScenarioTests.cpp` prüft die strikte Übereinstimmung von JSON und nativer Validierung, explizite Anfangsdaten, ID-/Anzahlgrenzen, unzulässige Feldkombinationen, endgültige Busstatus und nullable Beobachtungsberichte. `KernelPnpDeviceTests.cpp`, `KernelPnpRequestTests.cpp` und `KernelPnpCompletionTests.cpp` prüfen Providereigentum, AddDevice-Erfolg/-Fehler/-Leaks, initiale IRPs, Dateizulassung, Lebenszyklus-Rollback, verzögerten Abschluss, MPR/verschachtelte/wartende Fortsetzungen und Fehleratomizität. Das originale, mit echtem WDK erstellte `driver_wdm_pnp.c` verwendet die optionalen Pfade `NEVERD_WDM_PNP_FIXTURE` / `NEVERD_WDM_PNP_CFG_FIXTURE`. `DriverWDMPnpTests.cpp` prüft normale und aktive-CFG-Abbilder nach Relokation: AddDevice, Datei-I/O, geordnetes Entfernen, verzögertes Starten/Entfernen, Fehler bei Start/Query sowie bereinigte und leckende AddDevice-Fehler. `DriverScenarioPublicTests.cpp` prüft außerdem ein verzögertes PnP-Szenario mit sieben Anforderungen über C-API und CLI, einschließlich konfigurierter CFG-Abbilder. Fehlende Artefakte werden ausdrücklich übersprungen. Die Ausführungsnachweise stammen ausschließlich von Linux und belegen nur den dokumentierten ressourcenfreien PnP-Teilumfang.

Die V9-Schematests prüfen das Einlesen und Ausgeben aller acht Minor-Schreibweisen und teilen die Prüfung des endgültigen Status mit dem Lebenszyklusabschluss; QueryStop 0x119 wird vor dem Laden des Abbilds abgelehnt. Erweiterte Modell- und echte Fixture-Prüfungen decken QueryStop-Rollback, CancelStop, Stopp/Neustart, überraschendes Entfernen, Verstöße gegen exakte Erfolgsstatus, Software-I/O im gestoppten oder RemovePending-Zustand, Gastablehnung nach überraschendem Entfernen, Geräteidentität und gemischte AddDevice-Ergebnisse ab. `DriverScenarioPublicTests.cpp` führt über C-API und CLI eine Folge aus 16 Stopp-/Neustart-/Surprise-Anforderungen mit normalen/aktiven-CFG-Fixtures aus und erhält erfolgreiche Software-IOCTL-Bytes, den vom Gast gelieferten IOCTL-Fehler nach überraschendem Entfernen sowie abschließendes Cleanup/Close/Remove. Die öffentliche Ausführung ist seriell: Ein zurückgehaltenes IRP ohne derzeit verfügbare Abschlussquelle kann nicht auf eine spätere Szenarioanforderung warten, die das Gerät startet oder bereinigt. Die Remove-Drain-Regeln sind Profilgrenzen, keine allgemeine Windows-Regel zur I/O-Zulassung. Die Nachweise bleiben auf Linux beschränkt.

`DriverPowerScenarioTests.cpp` prüft strikte Power-Paketdaten, JSON/native Übereinstimmung, den opaken 32-Bit-Kontext, Antwort-FIFO-Grenzen und unabhängige Kindberichte. `KernelPowerRequestTests.cpp` und `KernelPowerCompletionTests.cpp` prüfen das tatsächliche Paketlayout, Routenflags, Lebenszyklus gegenüber objektbezogener Benachrichtigung, FIFO-Abgleich, Eigentum abschließender Callbacks, MPR, Wartephasen und Freigabegrenzen. Das originale, mit echtem WDK erstellte `driver_wdm_power.c` verwendet die optionalen Pfade `NEVERD_WDM_POWER_FIXTURE` / `NEVERD_WDM_POWER_CFG_FIXTURE`. `DriverWDMPowerTests.cpp` prüft Relokation normaler und aktiver-CFG-Abbilder, direkte und verschachtelte Query/Set-Anforderungen, verzögerten unabhängigen Abschluss, S0-vor-D0-Reihenfolge, Snapshots in Callbacks mit fünf Argumenten über Wartephasen, von Workern erzeugte Kinder, null-Callbacks, abgelehnte Queries, unabhängige PDO-Anfangswerte/FIFOs und explizite Fehler bei fehlenden Fakten. `DriverScenarioPublicTests.cpp` ergänzt die Vorprüfung fehlerhafter Power-Pakete und eine Schlaf-/Aufwachfolge mit sechs Szenario- und drei Kindanforderungen über C-API und CLI. Fehlende echte Artefakte werden ausdrücklich übersprungen; die Ausführungsnachweise bleiben auf Linux beschränkt und belegen nur den dokumentierten auslagerbaren, ressourcenfreien Power-Teilumfang.

`KernelUsbIdleTests.cpp` prüft Eigentum, D2, Ausleihe und erste Ursache; `KernelUsbIdleBridgeTests.cpp` / `KernelUsbIdleReceiptTests.cpp` echte IRPs, Abbruch, Composite-Kapazität und Eingangs-/Abschlussreihenfolge; `DriverUsbIdleScenarioTests.cpp` Eingaben/Berichte. `DriverWdmUsbIdleTests.cpp` nutzt echtes `driver_wdm_usb_idle.c` über `NEVERD_WDM_USB_IDLE_FIXTURE` / `NEVERD_WDM_USB_IDLE_CFG_FIXTURE` für Idle, D0/D3, Wake, Abbruch, Rearm/Neustart, unabhängige/Composite-Funktionen und PDO/FDO-Routen. `DriverWdmUsbIdlePublicTests.cpp` führt das [USB-Szenario](../examples/driver-wdm-usb-idle-scenario.json) per C API/CLI, normal/active-CFG und bevorzugt/verschoben aus. Fehlende Artefakte werden explizit übersprungen; Nachweise nur Linux, kein KMDF-USB-Nachweis.

`KernelFrameworkUsbIdleTests.cpp`, `KernelFrameworkUsbIdleStorageTests.cpp` und `KernelFrameworkUsbIdleBridgeTests.cpp` prüfen Richtlinie, Speicherung und typisierte Planung. Echtes `driver_kmdf_usb_idle.c` sendet selbst keinen Idle-IRP. `DriverKMDFUsbIdleTests.cpp` deckt fehlende Erlaubnis, verwaltete E/A mit verzögertem D2/D0, StopIdle vor/während Callback, Arm-Fehler, explizites Maximum, Wake und Composite ab. `DriverKMDFUsbIdlePublicTests.cpp` führt das [KMDF-USB-Szenario](../examples/driver-kmdf-usb-idle-scenario.json) mit `NEVERD_KMDF_USB_IDLE_FIXTURE` / `NEVERD_KMDF_USB_IDLE_CFG_FIXTURE` über C API/CLI, normal/active-CFG und bevorzugt/verschoben aus. Fehlende Artefakte werden ausdrücklich übersprungen; Laufzeitnachweise nur Linux. Modelltests prüfen, dass eine erschöpfte Speicherzuweisung nach erfolgreichem Wake-Arm-Callback den echten Disarm-Callback ausführt und WAIT_WAKE abbricht, ohne eine D2-Antwort zu verbrauchen.

`DriverKMDFUsbPoFxTests.cpp` prüft initiales SystemManaged/WithHint, beide Freigaben, Abbruch in D0, verzögertes D2/D0 und echte F0-Worker-Bestätigung, StopIdle, Wake vor READ, Arm-Fehler, Entfernung und Neustart. `DriverKMDFUsbPoFxPublicTests.cpp` führt das [USB-PoFx-Szenario](../examples/driver-kmdf-usb-pofx-scenario.json) über C API/CLI, beide Modi, normal/active-CFG und bevorzugte/verschobene Basis aus. `DriverKMDFUsbIdleTests.cpp` behält Weiterleitungsregressionen und prüft direktes READ nach D0Entry. `KernelFrameworkRequestTests.cpp` prüft Zuordnungen, Caller-Context-Besitz, manuelle/gestoppte Queues und IRQL. Speichererschöpfung und getrennte Sperren belegen Modellbrückentests, nicht der echte Treiber. Fehlende Artefakte werden explizit übersprungen; Laufzeitbelege gelten nur für Linux. `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` prüft auch echten D0Entry-Fehler bei RemovePending: exakte Required-Bestätigung und Quieszenz erlauben Bereinigung ohne F0/ActiveCondition. Allgemeine SET_POWER-Fehler oder autonome Surprise Removal sind damit nicht belegt.

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: Das [native WAIT_WAKE-Szenario](../examples/driver-wdm-wait-wake-scenario.json) verwendet das echte WDK-Fixture `driver_wdm_wait_wake.c` über `NEVERD_WDM_WAIT_WAKE_FIXTURE` / `NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE`. Geprüft werden START-Aufruf, Argumente, Wake ohne implizites D0, Neuanforderung, Abbruch, MPR, DPC-Abbruch mit Worker-D0, genaue Erfassung und getrennte Anbieter. Normale/active-CFG-Ausführung an bevorzugter/verschobener Basis ist nur unter Linux belegt; fehlende Dateien werden ausdrücklich übersprungen.

`KernelPowerCompletionTests.cpp` prüft APC/DPC, atomare Kapazitätsfehler mit Wiederholung, Anbieter allein synchron/verzögert mit/ohne Callback, gehaltene Route und MPR. Das echte `DriverWdmWaitWakeTests.cpp` prüft direktes D0 im DPC-Abbruch, APC/DPC Query/Set, unveränderte IRQL/CR8, Rückkehr vor PASSIVE-Ausführung und abgelehnte erhöhte WAIT_WAKE. Das [erhöhte Szenario](../examples/driver-wdm-elevated-power-scenario.json) läuft über `DriverWdmWaitWakePublicTests.cpp`, C API/CLI, normale/active-CFG-Images und bevorzugte/verschobene Basis; nur Linux-Nachweise.

`KernelRemoveLocksTests.cpp` prüft unabhängige Lock-/Geräteidentität, NULL- und wiederholte Tags, exakte Retail-/DBG-Größen, sofortigen und verzögerten Drain, Verpflichtungen nach fehlgeschlagenem Acquire, Fehleratomizität, Kapazität und Speicherfreigabe. `KernelRemoveLockBridgeTests.cpp` und `DriverWDMRemoveLockTests.cpp` decken Initialisierung vor dem Anhängen, opaken Erweiterungsspeicher, IRQL-Grenzen, Freigabe nach Paketende, Providerabschluss nach Lock-Drain, Bereitschaft durch die letzte Freigabe vor Callback-Rückkehr, wartende Worker und bereinigte AddDevice-Fehler ab. Das originale, mit echtem WDK erstellte `driver_wdm_remove_lock.c` wird als Retail-/DBG- sowie normale/aktive-CFG-Variante über die optionalen Pfade `NEVERD_WDM_REMOVE_LOCK_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` und `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` gebaut. Öffentliche C-API-/CLI-Tests verwenden das bestehende PnP-Schema und bewahren getrennte Beobachtungen von Busempfang, Busabschluss und endgültigem Abbau. Fehlende Artefakte werden ausdrücklich übersprungen; die Ausführungsnachweise sind auf Linux beschränkt und belegen weder vollständigen Driver Verifier noch allgemeines Leeren gleichzeitiger Anforderungen.

`DriverResourceScenarioTests.cpp` prüft explizite JSON-/native Fakten, Ganzzahlbreiten, Mengen, physische/Registerüberschneidungen, Ausrichtung, IDs, leere Banken und Konfigurationsserialisierung. `KernelMMIOTests.cpp`, `KernelMMIOFailureTests.cpp`, `KernelResourceBridgeTests.cpp` und `UnicornMMIOTests.cpp` decken Bank-/Mapping-Eigentum, Aliase, Generationen, Lebensdauer gepackter Listen, Providerzeitpunkte, Werterhalt beim Neustart, Surprise-/Energiezugänglichkeit, exakte CPU-/API-Transaktionen und Fehleratomizität ab. Das originale WDK-Fixture `driver_wdm_resources.c` verwendet `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`. `DriverWDMResourceTests.cpp` führt echte skalare und REP-Zugriffe, normale/aktive-CFG-Rebasierung, Teilbereichsaliase, Mappings am Seitenende, STOP/Neustart und ungültige Zugriffe aus. C API/CLI lehnen ungültige Fakten vor dem Laden ab und führen dasselbe Neustartszenario mit 14 Anforderungen, persistenten IOCTL-Ausgaben und exakten Map-/Unmap-Zahlen aus. Die gemeinsame [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) benötigt das Register-/IOCTL-Protokoll dieses Fixtures. Fehlende Artefakte werden ausdrücklich übersprungen; die Nachweise sind auf Linux begrenzt. Weder physischer Hostspeicher noch ein allgemeines Gerätebackend werden ausgeführt.

`DriverDMAScenarioTests.cpp` prüft explizite Fähigkeiten, logische Adressräume, Byte-/Anzahl-/Zeitgrenzen, strikte Ereignisrichtungen sowie getrennte Konfiguration und Beobachtungen. `KernelPhysicalMemoryTests.cpp` und `BackendBackingTests.cpp` prüfen Zuweisungsgrenzen innerhalb gemeinsamer Seiten, feste Bindungen, unveränderte CPU-Rechte, Ausschluss von MMIO/Wiedereintritt und atomare Validierungsfehler für vollständige Bereiche. `KernelRequestMDLTests.cpp` prüft Aliase aufgebauter Deskriptoren gegen dieselben physischen Identitäten. `KernelDMATests.cpp`, `KernelDMABridgeTests.cpp` und `SchedulerDMATests.cpp` testen echte RAM-Bytes, adaptergebundene Tabellenaufrufe, Inline-/FIFO-Eigentümerschaft, getrennte Callback-/Abbildungslebensdauern, Seitenfragmente, falsche Richtungen, Freigabevorprüfung, unabhängige PDO-Adressräume und Epochen-/Energiefehler. Der eigenständig geschriebene WDK-Treiber `driver_wdm_dma.c` nutzt `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`; `DriverWDMDMATests.cpp` sowie C-API-/CLI-Tests führen echte Adapterzeiger, gemeinsame/SG-Speicherung und separat konfigurierte DMA-/Interruptereignisse aus. Das gemeinsame [driver-dma-scenario.json](../examples/driver-dma-scenario.json) verlangt dessen Fixture-Protokoll. Fehlende Artefakte werden ausdrücklich übersprungen. Die Linux-Ausführungsnachweise belegen weder echtes Host-DMA noch PCI oder eine allgemeine Geräte-Engine. `pluginsdk/python/tests/test_driver_dma_integration.py` nutzt die bestehende JSON-Bindung mit eigener Speicherverwaltung über `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_FIXTURE` und `NEVERD_TEST_WDM_DMA_CFG_FIXTURE` und prüft Bytes, Callback-Reihenfolge und gemeldete Fehler.

`KernelSEHTests.cpp` prüft reine Unwindpläne, Bereichsreihenfolge, Wiederherstellung nichtflüchtiger GPRs, begrenzte Stacks und explizit unmodellierte Metadaten. `KernelExceptionTests.cpp` prüft exakte API-Argumentanzahlen, Statuswerte der unteren 32 Bit, typisierte Ausnahmen, IRQL-Grenzen und unveränderten Modell-/CPU-Zustand. Der mit echtem WDK und `/GS-` gebaute `driver_wdm_seh.c` verwendet optional `NEVERD_WDM_SEH_FIXTURE` / `NEVERD_WDM_SEH_CFG_FIXTURE`. `DriverWDMSEHTests.cpp` führt normale, aktive CFG- und umbasierte Abbilder mit direktem Auslösen und Auslösen aus Hilfsfunktionen, verschachtelten Handlern, erneutem Auslösen, echten Filtern, finally beim Unwind, Suchreihenfolge, stabilen Ausnahmedatensätzen, unterstützter CPU-Fehlerfortsetzung und vollständiger CPU-Zustandswiederherstellung aus. Verschachtelte Filter und kollidierte finally nutzen verknüpfte logische Stacks; andere CPU-Fehler bleiben ausdrücklich abgewiesen. C-API/CLI führen [driver-seh-scenario.json](../examples/driver-seh-scenario.json) aus und prüfen null-API-Ergebnisse sowie tatsächliche Gast-Handlermeldungen. `pluginsdk/python/tests/test_driver_seh_integration.py` verwendet `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_SEH_FIXTURE` und `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`. Fehlende externe Abbilder werden ausdrücklich übersprungen; Linux-Nachweise belegen weder Benutzerpuffer noch allgemeine SEH-Unterstützung.

`KernelDMAChannelTests.cpp`, `KernelDMAChannelBridgeTests.cpp` und die gemeinsam verwendeten `SchedulerDMATests.cpp` prüfen gemischte Zuweisungs-FIFOs, Callback-Rückgabebreiten, reine Zulassungs-/Freigabeprüfungen, Registerwiederverwendung, zusammenhängende Seitenfragmente, Flush der gesamten Operation, CurrentIrp-Schnappschüsse und Paket-/MDL-/Gerätelebensdauern. Der eigenständig geschriebene und mit echtem WDK gebaute `driver_wdm_dma_channel.c` nutzt optional `NEVERD_WDM_DMA_CHANNEL_FIXTURE` / `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`. `DriverWDMDMAChannelTests.cpp` führt normale, aktive CFG- und umbasierte Treiber mit echten MapTransfer-/FlushAdapterBuffers-Aufrufen, gemeinsamem Puffer-/SG-/Kanalkontingent, expliziten Gerätetransaktionen, IRQ/DPC-Abschluss, aufeinanderfolgenden Operationen, zwei PDOs und Fehlerfällen aus. C-API/CLI führen das sieben Anfragen umfassende [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json) aus, einschließlich einer einzelnen Transaktion über beide abgebildeten Fragmente. `pluginsdk/python/tests/test_driver_dma_channel_integration.py` verwendet `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE` und `NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` für dieselbe öffentliche JSON-Schnittstelle. Fehlende Artefakte werden ausdrücklich übersprungen; Linux-Nachweise belegen weder System-DMA-Controller noch beliebige HAL-Abbildungs-/Flush-Muster.

`DriverInterruptScenarioTests.cpp` prüft explizite Raw-/übersetzte Deskriptoren, gemischte und reine Interruptzuweisungen, strikte Ereignisfelder/-grenzen, Quellidentität und unabhängige BOOLEAN-Beobachtungen. `KernelInterruptsTests.cpp`, `KernelInterruptBridgeTests.cpp` und `SchedulerInterruptTests.cpp` prüfen exakte exklusive Tupel, opake Token, erfasste Generation/Verbindung, Ereignislaufzeit, ausgewählte Ex-Felder, gemeinsame Sperre und IRQL-Wiederherstellung, Callback-Eigentum, ISR-Vorrang bei gleicher Zeit und Kapazitätsfehler vor Zustandsänderungen. `KernelFrameworkRequestTests.cpp` prüft reine Abbruchvorschauen und Tokenkapazität ganzer Gruppen, ohne Aufrufe zu veröffentlichen oder Referenzen zu verbrauchen. Das originale WDK-Fixture `driver_wdm_interrupts.c` nutzt `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`. `DriverWDMInterruptTests.cpp` testet normale/aktive-CFG-Rebasierung, die ältere ABI mit elf Argumenten, Ex-Versionen 1/2/4, echten ISR→DPC-Abschluss, FALSE im niedrigen AL, Synchronisation/manuelle Sperren, unabhängige PDOs, Neustartgenerationen und ungültige Hardwarefakten. C-API-/CLI-Tests lehnen ungültige Deklarationen vor dem Laden ab und führen [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json) mit sieben Anforderungen aus; dabei prüfen sie Bytes des wartenden IOCTL und separate Zustellungsbeobachtungen. Fehlende Images werden explizit übersprungen. Ausführungsnachweise bleiben auf Linux beschränkt und belegen keine gemeinsam genutzten/Pegel-/MSI-Interrupts oder Befehlspräemption.

`DriverGuardTests.cpp` und vier originale Varianten von `driver_guard.c` prüfen aktives/inaktives CFG, das Verschieben der Ladebasis, Check-/Dispatch-ABI und fehlerhafte Ziele. `KernelFrameworkTests.cpp`, `KernelFrameworkControlTests.cpp`, `KernelFrameworkQueueTests.cpp` und `KernelFrameworkRequestTests.cpp` prüfen Bindungen, transaktionale Geräteerstellung, Queue-Routing, logische Pufferlängen sowie die Cleanup-Reihenfolge und die Lebensdauer von IRPs und Kontexten. Die originalen Fixtures `driver_kmdf_lifecycle.c` und `driver_kmdf_control.c` werden optional mit echten WDK-1.33-Headern kompiliert und über die echte `FxDriverEntry`-Bibliothek gelinkt. Setzen Sie die CMake-Cache-Pfade `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` für Lifecycle-Images und `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` für normale Steuergeräte-Images bzw. solche mit aktivem CFG. Fehlende externe Artefakte werden ausdrücklich übersprungen. `DriverKMDFLifecycleTests.cpp`, `DriverKMDFControlTests.cpp` und die C-API-/CLI-Fälle in `DriverScenarioPublicTests.cpp` prüfen tatsächliche Callbacks, gepufferte/direkte E/A, den Abschluss ausstehender Anforderungen durch Work Items, Fehlerstatus, Unload und CFG-Ausführung mit verschobener Ladebasis. Die Nachweise bleiben auf Linux beschränkt und belegen weder vollständige KMDF- noch PnP-/Energieverwaltungsunterstützung.

Legacy-Abbruchtests halten die API-Fortsetzung über Abbruch, verschachteltes Cleanup und endgültige Zerstörung offen; Ex liefert für bereits abgebrochene Anforderungen weiterhin den Status ohne Callback. `KernelFrameworkRequestAccessorTests.cpp` und `KernelRequestMDLTests.cpp` prüfen gemeinsame 64-Bit-Information, Längenprüfung beim Abschluss, ursprüngliche Queue-/IRP-Identität, NULL-WDF-Dateihandles, Getter bei erhaltenem Handle, gepufferten MDL-Cache und ByteCount der ersten Richtung, direkte Deskriptoridentität und verzögertes Mapping, Abschlussfreigabe und abgewiesene WDM-Umgehung. Die echten Steuergeräte-Fixture-Modi L, M, D und C führen Legacy-Abbruch, gepufferte MDL/Information, direkte READ-/WRITE-MDL und Zugriff nach Abschluss in normalen und aktiven CFG-Images aus.

Abbruchtests prüfen virtuelle Fristen ausschließlich für Transfers, Berichtsfelder, vorherigen Abschluss, bereits abgebrochene Anforderungen, Markieren/Entfernen, Abschlussrechte bei Einreihung gegenüber Zustellung, Callback-Warten und interne Referenzen. Scheduler-Tests prüfen unabhängig DPC-/Abbruch-/Work-Item-Reihenfolge, Kapazität, getrennte Identitäten und Suspendieren/Fortsetzen. WDM-Abbruch bleibt ein ausdrücklicher Modellfehler.


## Testaufteilung

`add_neverd_unittest` erzeugt ein GoogleTest-Programm und weist jedem
gefundenen Fall ein CTest-Label mit dem Namen dieses Executable-Targets zu.

| Quellbereich | Target und CTest-Label | Abdeckung |
|--------------|------------------------|-----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Plattformübergreifende Kindprozesse, Quoting, Umleitungen und Exitcodes |
| `unittests/libc` | `NeverDLibCTests` | Bekannte libc-Namen und Klassifizierung |
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Senkenkatalog, Identitätsvorrang, Argument-Vorfilter, Copy-Überlauf-Hunt, Heap-Lebensdauer-Audit und die verpflichtende Sechs-Zellen-Matrix PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/loader` | `NeverDRawISATests` | Binärdateien: Erkennung des Befehlssatzes aus den Bytes (Daten, der eigene Code des Tests, um zwei Bytes versetzter Code, 32- gegenüber 64-Bit-Kodierungen je Familie, Dateien, die mit Nullen beginnen) und die Cortex-M-Vektortabelle. `scripts/validate_isa_model.py --engine build/bin/libneverd.so` prüft das Modell an 180 echten Programmen und Bibliotheken, die es nie gesehen hat, per Hash heruntergeladen; es braucht das Netz und gehört nicht zu CTest |
| `unittests/lift` | `NeverDLiftTests` | Decoder-/Lifter-LowIR-Formen, IR-Stufen, Loader, Relokationen, Format-Fixtures, Dekompilierung und repräsentative Patch-Flows |
| Die meisten Dateien in `unittests/semantic` | `NeverDSemanticTests` | Differentielle Semantik von Instruktionen, ABI, Kontrollfluss, C-Ausdrücken und Lift/Recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Hardfork-Metadata, Eingabenormalisierung, ABI-/Signatur-Ambiguität, CFG/SSA/Recovery, exhaustive Decoder-Grenzen und feindliche Eingaben, Proxy-/Call-Fakten, Interpreter-Semantik, differentielle LLVM/C/Solidity-Ausführung und API-Routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | v0-v4-Metadaten und ELF-Layouts, striktes Verifier-/Loader-Verhalten, 23 gepinnte ELF-Artefakte, unabhängiges offizielles Oracle, vollständige Opcode-Verfügbarkeit, feindliche Eingaben, CFG/Recovery sowie ausgeführte LLVM-/C-/Rust-Differenzen |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Rewrite-/Obfuskationsäquivalenz über vier ISAs und drei Objektformate |
| Fokussierte Transformationsdateien in `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Schnell relinkbare Sonden außerhalb des großen Semantikprogramms |
| `unittests/corpus` (Submodul) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests`, `NeverDAdaDEHCorpusTests` | Exception- und Runtime-Metadaten aus 545 per Digest fixierten echten Binärdateien, jede mit einem Manifest, das die Untergrenzen ihrer Wiederherstellung nennt |

Die Registrierungsquellen sind
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt) und
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt) und
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt) und
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt).

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
`check-neverd-cxx-itanium-eh-corpus`, `check-neverd-objc-eh-corpus` und `check-neverd-ada-d-eh-corpus` jeweils
eine. Alle drei CI-Hosts konfigurieren mit dem Flag und fahren alle sechs Linien:
Die Bytes sind überall identisch, was sie liest jedoch nicht, und ein
Corpus-Lauf auf einem Host beweist nichts über die anderen beiden.
`scripts/audit_ci_test_inventory.py` weist ein Inventar zurück, dem eines der
sechs Labels fehlt, denn ein Build, der das Corpus stillschweigend nicht mehr
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

Siehe [Windows-Ausnahmerekonstruktion](windows-exception-reconstruction.md)
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
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h).
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

Das Sanitizer-Profil wird separat in `build-sbf-asan-ubsan` gebaut. Das
gepinnte, revisionsgebundene Prebuilt-Paket enthält den benötigten fork-only
Header; daher läuft auch Integration im selben fail-fast ASan/UBSan-Profil.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
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

## Leistung des Android-Klasseninventars

Bauen Sie `NeverDMobileTests` in Release und führen Sie sein Label aus, bevor
Sie `neverd mobile INPUT --list-classes` messen. Lesertests decken spärliche
Metadaten, Unicode, ungültige Referenzen, nicht unterstützte Methodenrümpfe,
Prüfsummen und Budgets ab; Archivtests unterscheiden vollständige Extraktion
von Abfragen ausgewählter Nutzdaten. CLI-Tests prüfen Präfixfilterung,
JSON-Prüfumfang, Erhaltung bestehender Ausgaben und atomare Fehler bei Multidex.

Der unabhängige Fixture- und Messrahmen validiert das vollständige
Deskriptorinventar jedes Prozesses, bevor er einen Zeitmesswert akzeptiert:

```sh
python3 -m unittest scripts.tests.test_benchmark_mobile_inventory -v
python3 scripts/benchmark_mobile_inventory.py \
  --output-dir /tmp/neverd-inventory-benchmark \
  --class-count 6000 --dex-count 3 --code-units 64 \
  --extra-string-bytes 8388608 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

Verwenden Sie für jeden Lauf ein neues Ausgabeverzeichnis. `--generate-only`
schreibt Fixtures und Manifest ohne Zeitmessung. `--workload` wählt gemeinsame
Eingabearten für ein optionales `--peer-command 'tool {input} {prefix}'`;
die Eingabevorbereitung liegt außerhalb des gemessenen Befehls. Berichte
bewahren Hashes, Befehle, sämtliche Messwerte aus neuen Prozessen,
Warm-Cache-Annahmen und unter Linux mit GNU time den maximalen RSS der
Kindprozesse auf. Dieser RSS ist nicht die kombinierte Spitze eines Werkzeugs
mit mehreren Prozessen. Synthetische APKs sind Abfragecontainer, keine
installierbaren Apps. Inventargeschwindigkeit belegt weder die Geschwindigkeit
der Referenzsuche noch die Qualität der Java-Rekonstruktion.

Binden Sie auf Hybrid-CPUs den Messrahmen und seine Kindprozesse an dieselbe
zulässige CPU (unter Linux etwa `taskset -c 4 python3 ...`), um Performance-
und Effizienzkerne nicht zu vermischen. Der Bericht erfasst die geerbte CPU-Affinität.

## Leistung von Android-Codereferenzen

Referenzabfragen teilen die Instruktionsgrenzen und Codevalidierung des
Rekonstruktionslesers. Führen Sie nach Änderungen an dieser Grenze die mobile
Testsuite aus. Lesertests decken Operanden-Pool-Arten, Abgleichmodi,
Methodenzuordnung und gemeinsam genutzten Code, täuschende Payload-/Immediate-Werte,
fehlerhafte Eingaben und Ressourcenlimits ab. Gemeinsam genutzte Debug-Ströme
werden gegen Rahmen, Ausdehnung und Parameter jedes zugehörigen Rumpfs geprüft.
Behalten Sie große Member-Inventare und verzweigungsreiche Rümpfe in den Tests
für Speicherlimits bei; dauerhafte Indizes und temporäres Containerwachstum
haben unterschiedliche Lebensdauern.
Prüfen Sie auch umgeordnete und überlappende Elemente, gemeinsamen Code mit
inkompatiblen Prototypen gleicher Breite, nicht ausgerichteten Eingabespeicher
und Teilzeichenfolgen über Suchblockgrenzen hinweg. Leistungsänderungen an
privaten Decoderdaten müssen vollständige Rekonstruktionsmodelle mit eigenen
Daten und Multimengen der Referenzvorkommen bewahren, einschließlich des
Fehlerverhaltens bei nicht unterstützten Rekonstruktionsmetadaten. Unterscheiden
Sie unabhängig erzeugte Erwartungen von Übereinstimmung mehrerer Werkzeuge
bei realen Eingaben.

Der unabhängige Referenzmessrahmen erfasst erwartete Vorkommen beim Erzeugen
der Instruktionen. Er prüft bei jedem gemessenen Lauf vollständige
Methodenidentitäten, PCs in Codeeinheiten, Opcodes, Zielidentitäten,
UTF-16-Einheiten und Häufigkeiten. Er prüft außerdem die unabhängig erwarteten
Abdeckungszähler von NeverD und `code_scan_complete`:

```sh
NEVERD_REFERENCE_TEST_BINARY="$PWD/build-release/bin/neverd" \
  python3 -m unittest scripts.tests.test_benchmark_mobile_references -v
python3 scripts/benchmark_mobile_references.py \
  --output-dir /tmp/neverd-reference-benchmark \
  --class-count 500 --methods-per-class 64 --matching-methods 2 \
  --dex-count 3 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

Wählen Sie Fälle mit `--kind` und `--workload`. `--extra-strings 65536` testet
echte 32-Bit-Zeichenkettenindizes. Payload-Köder sind standardmäßig aktiv;
`--no-payload-lookalikes` bewahrt Anordnung und echte Referenzen, ersetzt aber
die Köderwerte für Vergleiche mit gemeinsamen Eingaben. Bewahren Sie sowohl
Korrektheits- als auch Zeitergebnisse auf. Liefert eine Abfrage falsche
Payload-Referenzen oder fehlen echte Referenzen, schlägt die Validierung fehl
und es wird kein Zeitmesswert akzeptiert.

Ein optionales `--peer-command` akzeptiert eine argv-Vorlage mit `{input}`,
`{kind}` und `{query}`. Passen Sie die Abfragesyntax ausdrücklich an, wenn ein
anderes Werkzeug andere Semantik verwendet, und vergleichen Sie vollständige
Multimengen von Vorkommen. Sein angegebener Validierungsumfang bleibt erhalten,
ohne ihm einen vollständigen Codescan zuzuschreiben. Dieselben Einschränkungen
bezüglich neuer Verzeichnisse, CPU-Affinität, neuer Prozesse, warmem Cache und
RSS wie beim Inventarbenchmark gelten. Der optionale CLI-Unittest wird
übersprungen, solange `NEVERD_REFERENCE_TEST_BINARY` nicht das gebaute Programm
bezeichnet; melden Sie diesen übersprungenen Test.

## Exportnachweise der mobilen SDKs

Der manuelle Workflow `Mobile SDK Export Evidence` führt `collect_mobile_ios_sdk_declarations.py --exports-only` mit den festgelegten Xcode-SDKs aus. Er bewahrt die Linkerdateien von Foundation, CoreFoundation und UIKit für iOS-Geräte und Simulatoren unverändert auf, einschließlich Ziel, SDK-Version, Hash der SDK-Einstellungen, Dateigröße und SHA-256. Der reguläre Deklarationssammler bewahrt diese Dateien ebenfalls auf. Fehlende, leere, zu große oder außerhalb des SDK liegende Dateien lassen die Erfassung scheitern; bereits erfasste Nachweise bleiben erhalten. Die Linkerdateien belegen Symbolexporte, jedoch keine Aufruf-ABI oder erfolgreiche Methodenwiederherstellung.

## ABI-Nachweise für mobile Swift-Strings

Der manuelle Workflow `Mobile Swift String ABI Evidence` kompiliert feste Swift-Proben für Gleichheit und Ordnung sowie eine C-Probe mit `swiftcall` unter Xcode 26.5 für arm64-iOS-Geräte und Simulatoren. `collect_mobile_swift_string_abi.py` bewahrt Quellcode, LLVM IR, Assembler, Compileridentität, SDK-Einstellungen und `libswiftCore.tbd` samt Hashes auf. Beide Sprachen müssen den exakten Vergleichsimport mit fünf Argumenten und dem Ergebnis `i1` zeigen; C muss dieses Ergebnis ausdrücklich auf ein Byte erweitern. Falsche Ziele, geänderte Signaturen, Befehlsfehler und Zeitüberschreitungen erhalten Teilergebnisse und lassen die Erfassung scheitern. Diese Compilernachweise installieren keine Laufzeitdeklaration und belegen keine Methodenwiederherstellung. Test ohne SDK: `python3 -m unittest scripts.tests.test_mobile_swift_string_abi`.

## Modulare MBA-Vereinfachung

`SymSimplifyFinite.*` prüft vollständige Bereiche mit zwei Werten von 8 bis 512 Bit, die Kosten geteilter Nutzungen, alle unterstützten poison-erzeugenden Annotationen, unabhängige volatile-Lesezugriffe und freeze-Werte, explizites undef/poison, tiefe iterative Durchläufe, Budgets und die Verschleierungsmarkierung. Ursprüngliche und vereinfachte IR laufen bei O0/O2 gegen eine unabhängige Referenz für alle Byte-Eingaben und zufällige Eingaben voller Breite. Übersetzungsobjekttests verlangen unterschiedliche Cache-Identitäten für unterschiedliche Budgets endlicher Werte.

Merge-Domänentests prüfen verschachtelte Selects, rautenförmige PHIs und Kopierzyklen mit 8–512 Bit; widersprüchliche Rückkanten, undefinierte Bedingungen, Komponenten ohne Ursprung, unabhängige PHI/freeze-Beobachtungen und erhaltene markierte Erzeuger; außerdem genaue Knoten-, Kanten- und Arbeitsgrenzen. O0/O2-Laufzeitorakel prüfen alle Byte-Paare und variieren Operanden voller Breite für Auswahl, Zusammenführung und begrenzte Zustandschleifen.

Die Zweiwerttests prüfen zusätzlich verschachtelte Konjunktionsmasken mit 8–512 Bit, vertauschte Operanden, OR/undef-Ablehnung, begrenzte tiefe Suche, eigenständige Arbeitszählung und Obfuskationsmarkierung sowie die genaue Budgetgrenze der ersten Änderung. Laufzeitorakel prüfen sämtliche Byte-Eingabepaare und variieren unabhängige 64-Bit-Daten; ursprüngliches und vereinfachtes IR werden bei O0/O2 verglichen.

`SymSimplifyPredicates.*` prüft Vierbit-Offsets, Vorzeichen und Eingaben vollständig, testet boolesche Intervallverknüpfungen sowie getrennte Mengen und führt unabhängige Byte- und Vollbreiten-Orakel bei O0/O2 aus. Abgedeckt sind poison-Annotationen, verborgene undef-Eingaben an Zusammenführungen, unabhängige Lese-/freeze-Werte, erhaltene Schleifen-PHIs, gemeinsame Nutzungen, kumulatives Budget, hoher Fan-out, Rekursionsgrenzen und Obfuskationsmarkierung. Beide Cache-Schlüssel unterscheiden Prädikatanalysebudgets.

`SymExpr.*` prüft konstante Ausschnitte oberhalb des niedrigsten Bits mit sämtlichen Vierbit-Eingaben und Masken sowie breiten Werten, verschachtelten Strukturen, gemischter Byte-Rekonstruktion und Übertragsgegenbeispielen. Budgettests platzieren einen breiten Knoten an der Rekursionsgrenze und verweigern das Kopieren übergroßer Konstanten. Unbekannte Ausschnitte müssen symbolisch bleiben, ohne den Ausdrucks-DAG zu vergrößern. `SymState.*` unterscheidet außerdem für beide Byte-Reihenfolgen abgeleitete skalare Konstanten von literalen Regionskonstanten, ohne den DAG zu vergrößern oder gespeicherte ganze Wörter zu verändern.

`SymReadability.*` prüft die Darstellung von Subtraktion und Komplement, Kosten assoziativer Operatoren, Ein-Bit- und breite Literale, Sättigung gemeinsamer Bäume, budgetierte Kandidatenwahl sowie vollständige Drei-Bit-Äquivalenz ohne Stichproben. `SymMBASample.*` vergleicht schmale und beliebig genaue Prüfung mit dem AP-Auswerter, einschließlich aller Operatoren, deterministischer Belegungen und ungenutzter breiter Eingaben. Für Qualitätsvergleiche über Wertungsversionen hinweg müssen beide Ausgaben mit demselben Maß neu gezählt werden; die versionsabhängigen SDK-Größenzähler dienen nur der Diagnose.

## ARM32- und Frame-Weitergabe-Testmatrix

```sh
cmake --build build-release --target NeverDSymbolicTests \
  NeverDSymSimplifyGuardTests NeverDLiftTests NeverDMBASourceTests \
  NeverDHighCStoreForwardingTests NeverDMetadataJSONTests --parallel 4
build-release/bin/NeverDSymbolicTests
build-release/bin/NeverDSymSimplifyGuardTests
build-release/bin/NeverDLiftTests \
  --gtest_filter='HighSymSimplify.*:HighFrameStoreForwarding.*:ELFARM32ModeTest.*'
build-release/bin/NeverDHighCStoreForwardingTests
build-release/bin/NeverDMBASourceTests
build-release/bin/NeverDMetadataJSONTests --gtest_filter='ELFARM32ModeCAPITest.*'
```

Die Frame-Spill-Matrix umfasst außerdem x86-32 (ELF/COFF/Mach-O), ARM32 (ARM- und Thumb-ELF) und AArch64 (ELF/COFF/Mach-O) mit beiden C-Backends. Wiederholte private Frame-Loads müssen zu Addition oder Subtraktion werden und für Bytepaare, Wortgrenzen und deterministische Zufallswörter auf beiden Optimierungsstufen korrekt laufen. Clang-AST-Prüfungen untersuchen ganze Spill-Funktionen auf verbleibende MBA-Operatoren und unterscheiden gültige Adressausdrücke. HighFrameStoreForwarding prüft exakte Zugriffsbreiten, lokale Änderungen, Memory-Home-Schreibzugriffe, Aliase, Überlappungen, geordneten Speicher, fehlerhafte Graphen und Expansionsgrenzen. HighCStoreForwarding hält Store-Wert-Definitionen über vier Architekturen einschließlich Float-Reinterpretationen lebendig; SymSimplifyGuard prüft Load-Identität und -Reihenfolge, volatile/atomic und Poison-Grenzen. ELFARM32ModeTest prüft ARM/Thumb-Auswahl, Adressnormalisierung, gemischte Metadaten und widersprüchliche Belege. ELFARM32ModeCAPITest prüft explizite SDK-Fehler und Decoder-Wiederherstellung nach erneutem Laden von Thumb; InstructionMode deckt Decoder-, Codepointer-, Branch- und Codegen-Grenzen ab. Fehlendes Cross-Target-Clang ist ein Skip und kein Nachweis für das Format.

`HighBoundPrivateFrameCopies.*` in `NeverDHighControlFlowTests` prüft Kopien über private Stack-Slots, deren Adressen nach Bindung der Aufruf-ABI nicht nach außen gelangen. Abgedeckt sind Verzweigungen, wiederverwendete Slots und übereinstimmende Fakten in verschiedenen Bedingungskontexten. Der für x64 und AArch64 erzeugte C-Code läuft mit `-O0` und `-O2`, Fallen für undefiniertes Verhalten und unabhängigen arithmetischen Prüfungen. Gegenbeispiele verlangen die unveränderte Funktion bei entweichenden Frame-Adressen, unbekannten Aufruf-ABIs, fehlenden oder widersprüchlichen Frame-Aliasen, neu zugewiesenen Eingangsparametern, überlappenden Zugriffen, geordnetem oder atomarem Speicher, fehlerhaften Anweisungen, Zyklen und erschöpften Budgets. Gewöhnliche Wertkonvertierungen dürfen keine PHI-Kopien werden.

`HighIntegerSignedness.*` in `NeverDHighControlFlowTests` prüft den späten Durchlauf, der jede Register- oder temporäre lokale Variable danach vorzeichenbehaftet oder vorzeichenlos deklariert, was die meisten ihrer Verwendungen lesen. Umlaufende Arithmetik, logische Verschiebungen und vorzeichenlose Vergleiche sprechen für vorzeichenlos, vorzeichenbehaftete Vergleiche, Division, arithmetische Verschiebungen und Vorzeichenerweiterung für vorzeichenbehaftet; eine Variable mit einer nicht ganzzahligen Verwendung behält ihre Typen. Das erzeugte C läuft mit `-O0` und `-O2` unter Traps für undefiniertes Verhalten gegen unabhängige Referenzarithmetik, einschließlich eines vorzeichenbehafteten Vergleichs einer vorzeichenlos gewordenen Variable.

`HighValueForward.*` in `NeverDHighControlFlowTests` prüft, wann der HighC-Schreiber einen einmal verwendeten Wert in seine Verwendung falten darf. Eine Schleifenbedingung behält einen Wert, dessen Variablen die Schleife zuweist, da ein Name mehrere SSA-Werte bezeichnen kann; ein erneut gelesener Stapelplatz behält seinen Wert über eine Speicherung in diesen Platz hinweg und faltet an einer Speicherung in einen anderen Platz vorbei. Eine Kopie behält ihren Wert, wenn ihre Quelle vor der Verwendung neu zugewiesen wird. Jeder Fall läuft mit `-O0` und `-O2` unter Traps für undefiniertes Verhalten.

`HighCIntegerConversion.*` in `NeverDHighControlFlowTests` prüft die Ganzzahlkonvertierungen, die der HighC-Schreiber C überlässt. Eine Konvertierung innerhalb eines Operanden, die die Bytes behält, die eine äußere Konvertierung behält, erhält keinen eigenen Cast; eine Zuweisung an eine deklarierte ganzzahlige lokale Variable und ein Return konvertieren implizit, und ein Literal wird als der Wert geschrieben, in den es konvertiert, während ein Zeiger seine explizite Konvertierung behält. Speicherungen konvertieren wie Zuweisungen, und ein nullerweitertes Argument für einen typisierten breiteren Parameter behält seine Erweiterung. Jeder Fall läuft mit `-O0` und `-O2` unter Traps für undefiniertes Verhalten gegen Referenzarithmetik.

Die Quellprojektion prüft variadische Objektlisten auch nach dieser Bereinigung erneut: Leere Instruktionsanker sind zulässig, versteckte Effekte und Kontrolltransfers nicht. Die Synchronisationsbereinigung akzeptiert eine einzelne `int64_t`- oder `uint64_t`-Sicht desselben gespeicherten Empfängers; Verengungen, Gleitkommakonvertierungen, Adressarithmetik und erneute Zuweisungen bleiben ausgeschlossen. Foundation-Objektmengen sowie normale und ausnahmebedingte Entsperrabläufe werden mit `-O0` und `-O2` ausgeführt.

## Native synchrone x64-Ausnahmen

Checked x64 führt `DIV`/`IDIV` mit echten Prozessorergebnissen und `#DE` aus. KVM nutzt eine private Supervisor-IDT/IST, WHP eine explizite Ausnahme-Bitmap; ursprünglicher Kontext und verfügbare Fehlercodes bleiben von Transportfehlern getrennt. Das OS konsumiert das wiederaufnehmbare Ereignis vor dem Setzen einer Fortsetzung. Windows-Treiber behandeln Nulldivision und Quotientenüberlauf als `STATUS_INTEGER_DIVIDE_BY_ZERO`, mit echten SEH-Filtern, `__finally` und Wiederholung. `NeverDX64ExceptionTests` baut ohne Unicorn; `DriverWDMCPUException` prüft originale WDK-Fälle. Nicht verfügbare ARM64-Hosts werden explizit übersprungen.

## Gestufte RAM-Effekte

`RAMTransaction` erfasst unter der physischen Ausführungslease nur die vereinigten deklarierten Schreibbereiche einer Instruktion. Vor Ergebnisbeobachtern wird der ursprüngliche RAM wiederhergestellt; Abbruch, Transportfehler und Beobachterausnahmen veröffentlichen weder Teilwrites noch Register. Prozessorfehler behalten nach RAM-Rollback ihren architektonischen Ausnahmestatus. ARM64-Einzel- und Paarstores verwenden dieselbe Instanz. x64 führt `XCHG`, `XADD` und `CMPXCHG` mit 8/16/32/64 Bit aus; gesperrte und implizit gesperrte Formen erfordern natürliche Ausrichtung. `NeverDRAMTransactionTests` vergleicht Ergebnisse mit der Host-CPU und prüft Rollback, Aliase und Rechte; fehlende Plattformen werden ausdrücklich übersprungen. Geräte und paralleles SMP bleiben ausgeschlossen. CPU-Snapshots setzen bereits bestätigten RAM nicht zurück.

`CMPXCHG8B` und `CMPXCHG16B` führen ihre Originalbefehle mit KVM, WHP und checked Unicorn in Treiber- und Benutzerprofilen aus. Erfolgreiche und fehlgeschlagene Vergleiche benötigen Lese- und Schreibrechte; Fehler werden als Schreibzugriffe eingeordnet. `CMPXCHG16B` prüft vor dem Speicherzugriff die 16-Byte-Ausrichtung und meldet bei einem Verstoß `#GP(0)`. Beide Ergebnisbeobachtungen gehören zu einer RAM-Transaktion: Ein Stopp oder eine Ausnahme in einem Callback veröffentlicht weder Register- noch RAM-Änderungen. Unverriegeltes `CMPXCHG8B` darf Seitengrenzen überschreiten; verriegelte Operanden erfordern weiterhin natürliche Ausrichtung. `X64WideAtomicTests.cpp` vergleicht originale Host-Ergebnisse und direkte native Fehler und prüft Aliase, Präfixe, Adressierung, Reparatur und Abbruch. Eigene Windows-Treiber- und Ring3-PE-Fixtures führen beide Breiten aus; die WDK-Fixture führt außerdem `_InterlockedCompareExchange128` aus. Das CPU-Modell muss `CMPXCHG16B` unterstützen.

## Vollständiger x87-Zustand

`NeverDEmulationArch` besitzt ISA-Verträge, Seitentabellen und das FP-Layout, das native Transporte und Unicorn gemeinsam nutzen. x64-Kontexte erhalten Steuerung, Status, TOP, physische Tags, Opcode, Befehls-/Datenzeiger und acht 80-Bit-Register. `FP0`–`FP7` verwenden `RegisterValue`; skalare Zugriffe lehnen eine Kürzung ab. `FPTag` ist die physische Maske nicht leerer Register. `NeverDX64FPTests` prüft alle TOP-Werte, exakte Operationen gegen Host-FXSAVE/FXRSTOR und die Wiederherstellung. Dies lässt keine x87-Befehle im checked-Vertrag zu und beweist nicht sämtliche Rundungssemantik. Fehlende native Hosts werden ausdrücklich übersprungen.

`driver-strict` unterstützt KVM auf passenden Linux-x64-Hosts und WHP auf passenden Windows-x64-Hosts; `auto` wählt diesen nativen Transport, unterschiedliche ISAs verwenden Unicorn. Explizites Unicorn und die bisherige V1-API behalten das portable Softwareprofil. Native Ausführung prüft kanonische Adressen und Effekte vor dem Eintritt; fehlende Hardware führt ohne Rückfall zum Fehler. Nicht unterstützte Instruktionen und OS-Verhalten bleiben explizite Fehler. Die native Windows-x64-CI besteht bei deaktiviertem Unicorn alle 359 Pflichtprüfungen: 131 CPU-Prüfungen, 224 Treiberergebnisse aus 26 eingebauten Images, 46 WDK-Images und 40 Szenariofällen an bevorzugten und verschobenen Adressen sowie vier SEH-Grenzprüfungen ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Native ARM64-Nachweise fehlen weiterhin; allgemeine Treiber- oder Android/Darwin-Kompatibilität ist damit nicht belegt.

Die obige native Prüfung umfasst die deklarierten Treibereinstiegspunkte und veröffentlichten Szenarien. Die unten beschriebenen detaillierten Funktionstests sowie C-API-/CLI-/Python-Prüfungen bleiben auf Linux-Nachweise beschränkt, sofern keine Windows-Ausführung ausdrücklich belegt ist. Ein erfolgreicher nativer Korpus belegt nicht jede Testvariante unter Windows.

Das ausgewählte Profil lässt sich mit `executionCapabilities(Contract, ISA, Backend)` abfragen. `NativeLegacyX64` beschreibt die native Ausführung von x64-Treibern. `NeverDNativeDriverTests` prüft den vorhandenen Treiberkorpus und kann auch in einem Build ohne Unicorn laufen.

Der bestehende CI-Workflow führt vor den allgemeinen Profilen das gesamte Emulationstestverzeichnis aus und speichert Inventar, JUnit-Ergebnisse und CTest-Protokoll in `emulation-focused`. Fehler anderer Module verhindern diesen Lauf nicht. Fehlende Hardware und optionale Treiberdateien bleiben ausdrücklich übersprungene Tests; erfolgreiche Softwareausführung oder Kompilierung belegt keine native Ausführung.

Unter Linux lässt `NeverDUnicornDeadlineTests` den tatsächlichen Timer-Thread durch gesteuerte pthread-Abläufe vor dem Gasteintritt fertig werden. Für x64, ARM32 und ARM64 prüft der Test, dass eine vorherige Abbruchanforderung keine Gasteffekte erzeugt und der nächste Lauf ein eigenes Budget verwendet. Er nutzt öffentliche APIs und verändert keinen privaten Engine-Zustand.

`X64StateTransition` in `NeverDX64ExceptionTests` führt unabhängige RAM-Lesezugriffe und CR8-Lesezugriffe auf der nativen CPU aus. TLS-Basen und Privileg wechseln, die Ausführung wird nach wiederholten Divisionsfehlern fortgesetzt, und TLS ändert sich nach abgebrochenem Eintritt. Nach Änderungen der nativen Zustandsübertragung sind dieses CTest-Label sowie Alias-Neuzuordnung, CPU-Kontexte, FP-Zustand und Ergebnisvergleiche der ursprünglichen Treiber zu prüfen. Nicht verfügbare KVM/WHP-Transporte bleiben ausdrücklich übersprungen.


`NeverDKvmRunTests` prüft die ausgeliehenen Übertragungen in `KvmRunControl` ohne `/dev/kvm`. `StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` prüft, dass Vorbereitung, Erfassung und abgefangener Host-Eintritt denselben Thread nutzen und die Vorbereitung trotz unterbrochener Wiederholungen nur einmal erfolgt. Weitere Fälle prüfen Vorbereitung ohne Eintritt bei Fehler, Erfassungsfehler, Stopp während der Vorbereitung und Abbruch eines aktiven Eintritts; ein anschließender Lauf darf die alten Callbacks nicht wiederverwenden. Die Tests für echten Abbruch, RAM-Rücknahme, Ausnahmen und ursprüngliche Treiber bleiben Teil der Validierung. `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` prüft, dass mehrere Eintritte mit derselben Frist den Thread wiederverwenden, jeden Transfer einmal ausführen und frühere Zustandspakete unverändert lassen.

`KvmHandoffPolicy` begrenzt jedes aktive Warten auf 8 μs, wechselt nach zwei erfolglosen Versuchen zum blockierenden Warten und versucht es nach 256 Übergaben erneut. Aufrufer und Worker passen sich unabhängig an; der Aufrufer beachtet auch die ursprüngliche Frist und das Stopptoken. Atomare Bereitschaftsflags sind nur Hinweise für die Ablaufplanung: Pakete, Callback-Lebensdauer und Abbruchbestätigung bleiben durch den Mutex geschützt. `NeverDKvmRunTests` prüft begrenztes erfolgloses Polling, Erholung, wechselnde Latenzen und Abbruchbestätigung vor der Wiederverwendung von Paketen.

KVM x64/ARM64 verwendet `KvmRunControl` für Vorbereitung, `KVM_RUN` und Zustandserfassung auf demselben privaten vCPU-Worker. Auch bei `EINTR` erfolgt die Vorbereitung einmal; Abbruch oder Lesefehler verhindern Veröffentlichung. `KvmAArch64Machine.cpp` führt Übersetzungspflege und vollständige Skalar-/Vektortransfers unter einer gemeinsamen Schrittfrist aus. Der Aufrufer veröffentlicht nach Bestätigung; ISA-Decodierung, RAM-Transaktionen, OS-Politik und Beobachter bleiben bei ihm. Native ARM64-Laufzeitnachweise stehen aus.

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` prüft fortgesetzte Ausführung und echte CPU-Schreibzugriffe nach Host-Änderungen an allgemeinen Registern, erstem und letztem XMM-Register, MXCSR und x87-Steuerwort. Echte `FXSAVE64`-Bytes prüfen alle physischen 80-Bit-Register, TOP, Tags, Opcode und Zeiger nach einem gestoppten Eintritt; wiederholte Divisionsfehler verwerfen die Wiederverwendung ebenfalls. Diese Maschinentests erlauben keine zusätzlichen x87-Instruktionen in checked-Profilen.

`NeverDKvmStateTransferTests` injiziert nach echter KVM-Ausführung einen Register- oder XSAVE-Lesefehler und wiederholt mit unveränderter Eingabe. Unabhängige Ganzzahl- und gepackte Byte-Ergebnisse belegen, dass fehlgeschlagene Erfassung keinen bereits fortgeschrittenen nativen Zustand wiederverwendet. Nur diese Testdatei umhüllt `ioctl`; nicht verfügbare native Hosts werden ausdrücklich übersprungen.

`NeverDKvmStateTransferTests` prüft auf echtem KVM fehlende, einzelne und kombinierte `KVM_CAP_SYNC_REGS`-Sätze sowie Abfragefehler. `SynchronizedCapturesRemoveOnlySupportedReadIoctls` zählt tatsächliche Leseaufrufe und prüft den vollständigen CPU-Zustand nach aufeinanderfolgenden Schritten. `CancelledWarmEntryRequiresFreshSpecialStateOnRetry` verlangt nach einem Abbruch ein erneutes Lesen der Spezialregister. Erfassungsfehler, Integer/SIMD-Wiederholungen, Rücknahme spekulativer RAM-Schreibvorgänge und Ausnahmepriorität durchlaufen dieselbe Matrix; nicht verfügbare native Abdeckung wird ausdrücklich übersprungen.

Geprüftes ARM64 besitzt eine gemeinsame Grenze für den vollständigen Zustand. `Registers.def` definiert 39 skalare Felder und 32 Vektoren mit 128 Bit; `captureAArch64State` sammelt alle Werte, wendet Bitbreiten an und normalisiert NZCV vor einer einzigen Veröffentlichung. Unicorn, KVM, WHP und HVF übertragen denselben Bestand einschließlich TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR und FPSR. Native Adapter aktivieren FP/SIMD über CPACR_EL1. Fehlgeschlagene Lesevorgänge und abgebrochene Eintritte erhalten den gesamten Aufruferzustand.

Der ARM64-KVM/WHP/HVF-Start führt das private Programm `AArch64MachineProbe.def` aus: NOP, FP32-Addition mit Rundung gegen positive Unendlichkeit und SIMD-Addition auf zwei Spuren. Jeder Schritt vergleicht alle 39 skalaren Felder und 32 Vektoren, einschließlich TLS, NZCV, gelöschter oberer Ergebnisbits und erhaltenem/kumulativem FPCR/FPSR-Zustand. Die Probe verwendet nur Supervisor-Monitorspeicher und eine gemeinsame Gesamtfrist. Die Proben belegen nur die begrenzte Initialisierung. Die Workload-Validierung für Linux ARM64 KVM und Windows ARM64 WHP steht noch aus; native macOS-Ergebnisse sind im [HVF-Leitfaden](macos-hvf.md) dokumentiert. Das Programm umfasst außerdem die A/B-Signierung und Authentifizierung von Rücksprungadressen bei deaktivierten Schlüsseln sowie alle vier BTI-Formen auf ungeschützten Seiten.

Die Probe führt außerdem zweimal `MRS CTR_EL0` sowie `DC CVAU`, `DSB ISH`, `IC IVAU` und `ISB` aus und prüft stabile Cachegeometrie und vollständigen Zustand. Checked EL0/EL1 erlaubt die Originalbefehle, alle benannten Basisoptionen von DSB und nur ISB SY. CTR stammt von der gewählten virtuellen CPU und kann je Transport abweichen. Cacheziele müssen mit den aktuellen Rechten lesbares normales RAM bezeichnen; unaligned Adressen und Aliase sind erlaubt, andere Ziele werden als nicht unterstützt abgewiesen. Wartung erzeugt keine Datenlese- oder Schreibereignisse. Die Projektion hält die Befehlsausführung kohärent, modelliert aber weder private Cacheinhalte noch paralleles Hardware-SMP. `NeverDAArch64CacheTests` prüft Zustand, schreibgeschützte Seitenenden, Ablehnungen, Stopps, Kontexte, Budgets und Gastcodeänderungen über seitenübergreifende RW/RX-Aliase. Nicht verfügbare KVM/WHP-Hosts werden ausdrücklich übersprungen.

Die native x64-Initialisierung von KVM/WHP/HVF führt `X64MachineProbe.def` in privaten Supervisor-Seiten aus. Eine gemeinsame Frist umfasst NOP, FP32-Addition mit Rundung gegen positive Unendlichkeit, SIMD-Addition mit zwei Lanes sowie FS/GS- und CS/SS/CR8-Lesezugriffe; jeder Schritt vergleicht den vollständigen Skalar-, XMM-, physischen x87- und Steuerzustand. x64- und ARM64-Proben benötigen das exklusive Ausführungsrecht des physischen Speichers. `MemoryProjection` besitzt die Cache-Identität (ISA, Adressraum, Mapping-Generation, Privileg und Monitorvariante) und den Verlauf bestätigter Wurzeln je ISA. Vor dem Überschreiben wird der Cache ungültig: fehlgeschlagener Ersatz darf keine teilweise geschriebenen Tabellen wiederverwenden, und Aufrufer liefern keine veralteten Wurzeln. Die Proben belegen nur die begrenzte Initialisierung. Die Workload-Validierung für Linux ARM64 KVM und Windows ARM64 WHP steht noch aus; native macOS-Ergebnisse sind im [HVF-Leitfaden](macos-hvf.md) dokumentiert.

Der gemeinsame XSAVE-Decoder unterscheidet den initialen SSE-Zustand im Standard- und Kompaktformat. Bei gelöschtem XSTATE_BV[1] initialisieren beide XMM; das Standardformat liest und prüft weiterhin MXCSR, das Kompaktformat initialisiert MXCSR. `X64XsaveCases.def` enthält unabhängige Datenlayouts und eigene XRSTOR-Hostprogramme. `X64XsaveTests.cpp` prüft atomare Ablehnung und vergleicht beide Formate mit tatsächlicher Hostausführung unter Erhalt des FP/SSE-Zustands des Aufrufers. Fehlen die Hostarchitektur oder die benötigte Befehlsfunktion, wird der Hostvergleich ausdrücklich übersprungen.

`X64FPState.def` deklariert kompakte Transportlayouts für AVX, AVX-512, CET_U/CET_S und AMX einschließlich der Komponentenausrichtung auf 64 Byte. Vorhandene Erweiterungsdaten müssen dem Null-Initialzustand entsprechen; fehlende Komponenten und Füllbytes definieren keinen Zustand. Layoutbits bestimmen Offsets; unbekannte Layouts, nicht initiale Daten und falsche Längen scheitern vor der Veröffentlichung. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState` und `InitialWideComponentsDoNotHideFPState` prüfen WHP-Pakete mit 872 und 10752 Byte. Der Transport erlaubt keine Ausführung dieser Erweiterungsbefehle.

`WhpXsaveRegisters.def` ergänzt vollständige XSAVE-Pakete durch benannte x87/SSE-Steuerregister. Letzter Opcode sowie Instruktions- und Datenzeiger werden explizit geschrieben und vom Host gelesen. Leere Paketfelder können ergänzt werden; widersprüchliche Nichtnullwerte oder gemeinsame Steuerfelder führen vor der Veröffentlichung zum Fehler. `NamedMetadataRestoresOmittedPacketFields` prüft fehlende Felder unter Erhalt der gesamten FP-Nutzdaten.

Native `FOP/FIP/FDP` folgen den x87-Sicherungsregeln des Hosts. AMD darf diese Felder ohne ausstehende unmaskierte Ausnahme löschen; Snapshots behalten die beobachteten Werte. `X64MachineProbe.def` und exakte NOP/Kontexttests setzen einen konsistenten ausstehenden Ausnahmezustand, damit jedes Feld gültig bleibt und ohne ausgeblendete Unterschiede verglichen wird. Die FXRSTOR64/FXSAVE64-Referenz im Hostprozess prüft beide Zustände; Backends ersetzen Hostergebnisse niemals durch Eingabemetadaten.

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` vergleicht den durch FXSAVE64 im Gast-RAM gespeicherten vollständigen FP/SSE-Zustand mit der XSAVE-Erfassung des Hosts. Beide APIs prüfen direkte Installation und FXRSTOR64 im Gast mit der Standardfunktion zur Zeigerspeicherung und der expliziten vom Host unterstützten Einstellung. Eintritt, Ausführung und Erfassung bleiben unterscheidbar; Werte werden nicht korrigiert, Abweichungen bleiben Fehler. Die Matrix erfasst auch eine ausstehende unmaskierte x87-Ausnahme und protokolliert eine FXRSTOR64/FXSAVE64-Referenz im Hostprozess samt Prozessorhersteller, um bedingte Zeigerspeicherung vom WHP-Transport zu unterscheiden.

Der gemeinsame Codec `encodeX64XsaveState` / `decodeX64XsaveState` besitzt Standard- und komprimierte FP/SSE-Pakete, die physische TOP-Rotation, Initialzustände fehlender Komponenten und atomare Prüfung. WHP verwendet vollständige XSAVE-APIs, bevorzugt `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`, mit älteren XSAVE-APIs als Kompatibilitätsweg. Einzelne ältere x87-Register ersetzen kein vollständiges Paket. Nicht initiale Erweiterungskomponenten, fehlerhafte Header, ungültige Steuerwerte und verkürzte Erfassungen scheitern ausdrücklich. WHP-Mappingfehler behalten HRESULT, GPA und Größe für die Diagnose.

`CheckedX64Instructions.def` erlaubt vorzeichenloses `MUL` mit 8/16/32/64 Bit und `CBW/CWDE/CDQE/CWD/CDQ/CQO` über den vorhandenen CPU-Transport. `NeverDX64IntegerTests` verwendet unabhängige Kodierungen und Erwartungswerte aus `X64IntegerCases.def` auf beiden Privilegstufen: Erhalt unveränderter Registerteile, 32-Bit-Nullerweiterung, beide Produkthälften, definierte CF/OF-Ergebnisse und unveränderte Flags bei Vorzeichenerweiterung. Multiplikation in gewöhnlichem RAM behält Rechteprüfungen für den gesamten Zugriffsbereich und Lese-Beobachter bei; ein Fehler oder Beobachterstopp erhält implizite Ausgaberegister und PC. Geräteoperanden bleiben nicht unterstützt. Die Fälle laufen auch mit checked Unicorn; nicht verfügbare native Transporte werden ausdrücklich übersprungen.

Die sieben Fälle `X64Integer.BitScan*` prüfen mit unabhängigen Kodierungen Breiten, Nullquellen, Registerüberlappung, definierte Flags, seitenübergreifende Lesezugriffe, Beobachterstopps, Speicherfehler und die Ablehnung von LOCK/Geräteoperanden. Undefinierte Ausgaben erhalten keine portablen Sollwerte. Die Tests laufen auf beiden Privilegstufen; die native KVM/WHP-Prüfung verlangt alle vierzehn Ergebnisse des passenden Hosts. Nicht verfügbare Plattformen werden ausdrücklich übersprungen.

`X64BitInstructions.def` erlaubt `BT/BTS/BTR/BTC` für Register und gewöhnlichen RAM mit 16/32/64 Bit. Ein Registerindex wird in Operandenbreite vorzeichenbehaftet ausgewertet und wählt ein ganzes Wort; ein unmittelbarer Index bleibt im Basiswort. Die Kürzung auf die Adressbreite erfolgt vor dem Addieren der FS/GS-Basis. Der Prozessor liefert CF und Schreibwerte; `RAMTransaction` hält das Ergebnis bis zur Annahme durch die Beobachter zurück. Berechtigungen werden über den gesamten Bereich einschließlich separater Seiten und Aliase geprüft. Stopps, Callback-Fehler und verweigerte Zugriffe erhalten CPU und RAM. LOCK ist auf natürlich ausgerichtete Speicheränderungen beschränkt; MMIO und paralleles Hardware-SMP bleiben ausgeschlossen. `X64BitStringTests.cpp` vergleicht unabhängige Kodierungen mit echter x64-Ausführung und prüft negative Indizes, Breitenkürzung, Seitengrenzen, Abbruch und ungültige LOCK-Formen. Siehe die [Intel-Befehlsreferenz](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`X64StringInstructions.def` definiert `MOVS/STOS/LODS` für normalen RAM mit 8/16/32/64 Bit; `CLD/STD` ändert nur das Richtungsflag. Jedes REP-Element prüft den gesamten Operanden vor Beobachteraufrufen und wird an einer Wiederaufnahmegrenze übernommen. Spätere Fehler erhalten fertige Elemente; Abbruch oder Callback-Ausnahme lässt das aktuelle Element unverändert. FS/GS wird erst nach Begrenzung der Adressbreite und nur zur Quelle addiert. AL/AX erhält obere Bits, EAX erweitert mit Nullen. Bei REP mit Zähler null und 32-Bit-Adressen müssen die oberen Zählerbits und für MOVS/STOS die oberen Bits der beteiligten Adressregister null sein: Reale Prozessoren liefern sonst unterschiedliche Ergebnisse. REPNE für MOVS/STOS/LODS und STOS/LODS-Geräteoperanden bleiben ausgeschlossen. `X64StringTransferTests.cpp` vergleicht Breiten, Richtung, Überlappung und Nullzähler mit unabhängigen Hostbefehlen und prüft Rechte, Aliasse, Adressumlauf, Fehler und Wiederaufnahme. Der originale WDK-Ressourcentreiber führt über `driver_resource_strings.def` alle vier STOS/LODS-Breiten aus.

`X64StringInstructions.def` definiert außerdem `CMPS/SCAS` auf normalem RAM mit 8/16/32/64 Bit und `REPE/REPNE`. Jedes Element prüft alle Leseoperanden vor Beobachtern, aktualisiert sechs arithmetische Flags und endet bei der ersten Abbruchbedingung. Ein Datenfehler stellt die Flags vom Beginn dieses ununterbrochenen REP wieder her; abgeschlossene Zeiger- und Zähleränderungen bleiben erhalten. Ein öffentlicher Wiedereinstieg beginnt mit dem veröffentlichten CPU-Zustand. Stopps und Beobachterausnahmen verändern das aktuelle Element nicht; nach vorzeitigem Ende wird das nächste Element nicht gelesen. FS/GS betrifft nur die CMPS-Quelle; SCAS bewahrt Akkumulator und ungenutztes Quellregister. Geräteoperanden und mehrdeutige obere Bits bei inaktivem 32-Bit-Zähler bleiben ausgeschlossen. `X64StringComparisonTests.cpp` vergleicht unabhängige Hostbefehle, Flags, Richtung, Aliase, Adressumlauf, Rechte und Wiederaufnahme; ein Linux-x64-Signaltest erfasst echte Fehlerregister. Der originale WDK-Ressourcentreiber führt beide bedingten Wiederholungen in allen vier Breiten über `driver_resource_strings.def` aus. Siehe [Intel-Referenz](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html). Das native Linux-Orakel prüft Fehler vor und nach dem ersten Element. Es unterscheidet Intels Wiederherstellung der Anfangsflags von den auf AMD EPYC 7763 unter Hyper-V beobachteten Flags des letzten Vergleichs ([native Beobachtungen](https://github.com/NeverSight/NeverD/actions/runs/37202522130)); unbekannte CPU-Hersteller führen zu einem ausdrücklichen Fehler. Checked-Gäste stellen auf allen Backends die Anfangsflags wieder her.

Bestehende WHP-CPUs teilen eine native Partition; endgültiges Schließen und erneutes Erstellen verwenden dieselbe Registersperre. `WhpResourceCache.h` verwendet den kooperativen VP 0 erneut; vor einem Wechsel der logischen CPU werden dieser VP und seine Abbildungen entfernt. Parallele CPUs behalten eigene VPs und GPA-Bereiche. Registerübertragungen, XSAVE und Abbruchanforderungen richten sich an den jeweiligen VP. x64 behält die standardmäßigen XSAVE-Funktionen des Hosts bei und prüft die tatsächliche Konfiguration mit `WHvGetPartitionProperty`. Die Ablaufplanung bleibt standardmäßig kooperativ.

`NeverDX64FPTests` prüft alle 79 Korruptionspositionen und führt unabhängig assemblierte `X64ProbeCases.def`-Befehle nativ mit gemeinsamer Frist und unverändertem Gast-RAM aus. `NeverDProjectionCacheTests` prüft Aufruferwechsel, ISA-Reihenfolge, Wurzelverlauf, Privileg-/Monitorvarianten, Mapping-Generationen, Adressraumidentität und fehlgeschlagenen Ersatz. `NeverDRunControlTests` enthält `WhpXsaveTests.cpp` für moderne und ältere API-Pakete, jeden TOP, Größengrenzen und unveränderten Zustand bei Fehlern; diese Speicher-Protokolltests sind kein nativer WHP-Nachweis. Nicht verfügbare native Transporte werden ausdrücklich übersprungen.

XSAVE-Diagnosen unterscheiden Größenabfrage, lokale Vorbereitung und Dekodierung erfasster Pakete. API-Name, zurückgegebene Bytezahl, Kapazität und begrenzte Header-/Steuerdaten bleiben erhalten; unabhängige Erwartungen stehen in `WhpHostFailureCases.def`. Gastregisterinhalte werden nicht ausgegeben. `InvalidInputReportsPreparationWithoutHostMutation` prüft außerdem, dass ungültige Eingaben weder den Host aufrufen noch dessen Paket ändern. Der gemeinsame ISA-Codec bleibt allein für die Validierung zuständig.

WHP-Hostfehler bei Fähigkeitsabfragen, Partitions-/CPU-Einrichtung, Register-/XSAVE-Übertragung und Ausführung behalten den HRESULT und den in `WhpProtocol.def` deklarierten API-Namen; fehlgeschlagene Fähigkeitsabfragen behalten das typisierte Nichtverfügbarkeitsergebnis. `WhpHostFailureCases.def` enthält unabhängige Erwartungen für Hostfehler bei gleichzeitigem Abbruch sowie für Abfrage-, Installations- und Erfassungsfehler der modernen und älteren XSAVE-APIs. Die Windows-Spezial-CI verlangt 210 native Erfolge: 16 Mapping-, zwei Start-, zehn FP/Kontext-, sieben gemeinsame CPU- und acht Integer-Fälle sowie beide API-Varianten von `NativeInstallRetainsFPStateBeforeAnyGuestExecution`. Letztere vergleichen vollständiges FP/SSE und separat gelesene Metadaten vor der Gastausführung. Fehlende Registrierungen, übersprungene, deaktivierte oder nicht ausgeführte Tests lassen die Prüfung nativer Nachweise fehlschlagen. Die zusätzlichen 26 Prüfungen umfassen alle Fälle aus `X64BitStringTests.cpp` auf beiden Privilegstufen. Windows PE64 verlangt 67 WHP-Prozessfälle und elf unabhängige native Windows-Vergleichsfälle.

`NeverDMemoryLifecycleTests` wird unabhängig von Unicorn gebaut, auch in rein nativen Konfigurationen. Software-spezifische Projektions- und Gerätefälle werden bei deaktiviertem Unicorn ausdrücklich übersprungen; Tests gemeinsam genutzter CPUs auf dem passenden Host bleiben registriert. `WhpMemoryTests.cpp` isoliert die native Speicher-API mit 16 Fällen aus `WhpMemoryCases.def`: Seiten- oder Projektionsgröße, gemeinsame oder getrennte Allokationen, unberührte oder residente Bytes und ein vorhandener oder fehlender erster virtueller Prozessor. Jeder Fall hält zwei logische Eigentümer am Leben, wechselt ihre abgebildete Partition mehrfach, gibt den inaktiven Eigentümer frei und prüft die weitere Nutzung der verbleibenden Abbildung ohne Neuerstellung. Echte Abbildungsfehler behalten ihren HRESULT und lassen den Test scheitern; dies belegt die Speicher-API, keine Instruktionsausführung.

`X64MachineProbe.def` benennt die fehlgeschlagene Startinstruktion und sämtliche Abweichungen bei Skalaren, TLS, Privileg, x87-Steuerfeldern, physischen FP-Lanes und XMM-Wörtern samt Soll- und Istwerten. `DiagnosticIdentifiesStepFieldAndBothValues` prüft unabhängige erwartete Meldungen. Der Zustandsvergleich bleibt exakt; die Diagnose unterscheidet Übertragungsverlust von Ausführungsfehlern und erklärt einen fehlgeschlagenen nativen Test nicht zum Erfolg.

`WhpResourceTests.cpp` prüft Wiederverwendung, Freigabe vor Ersatz, Fehlererholung und Wettläufe mit Frist oder Stopp. `LogicalCPUSwitchingRestoresPhysicalFPAndTLS` führt zwei Maschinen abwechselnd in beiden Privilegmodi aus, prüft unabhängige physische x87/XMM- und FS/GS-Zustände und setzt die verbleibende Maschine nach Freigabe der anderen fort. Windows-CI verlangt beide WHP-Privilegfälle.

`NEVERD_ENABLE_SEMANTIC_TESTS` ist standardmäßig `ON` und steuert die Testgruppe in `unittests/semantic` samt ihren Sammelzielen. Für native CPU-Tests ohne Unicorn bleibt `BUILD_TESTING=ON` aktiv; zusätzlich werden `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` und `NEVERD_EMULATION_BACKEND_UNICORN=OFF` gesetzt. Native KVM/WHP-Tests bleiben damit verfügbar, auch unter Windows ARM64/MSVC mit geeigneten SDK-Headern. Unicorn unter Windows ARM64 benötigt weiterhin eine ARM64-LLVM-MinGW-Toolchain. Diese Trennung des Builds belegt noch keine native ARM64-Ausführung.

Der native CPU-CI-Checkout initialisiert Capstone-Quellen der festgelegten Revision und verwendet das geprüfte LLVM-Paket. Mit `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` und deaktiviertem Unicorn-Adapter benötigen Konfiguration, Build und Linken der CPU-Tests keine Unicorn-Quellen. Signaturen und der externe Korpus werden ebenfalls nicht benötigt. Die Standard-CI aktiviert weiterhin die vollständige semantische Testgruppe.

Das manuelle Profil `native_cpu_only` in `ci.yml` wählt mit `native_cpu_backend=whp` Windows x64 (Standard) oder mit `native_cpu_backend=kvm` Ubuntu x64. `NativeCPUTests.def` teilt CPU-/Prozessanforderungen und deklariert transportspezifische Ziele und Fälle getrennt. `run_native_cpu_ci.py --require-whp` oder `--require-kvm` prüft den Host, baut alle Ziele vor CTest und speichert Inventar, JUnit, Protokolle und Ergebniszahlen. Fehlende oder übersprungene Pflichtfälle führen auch bei erfolgreichem CTest zum Fehler. CI deaktiviert Unicorn; `--with-drivers` verlangt denselben Korpus an Original- und verschobenen Adressen auf dem gewählten Transport. Kompilierung und Initialisierungsproben belegen weder Gastausführung noch ARM64-Abnahme. Das Ubuntu-Profil verwendet signierte Clang/LLD-21-Pakete des Upstream-Projekts; die CR8-Deklarationen von Clang 18/19 widersprechen den festgelegten WDK-Headern. Die native Linux-Abnahme verwendet CMake 4.2.3. `NeverDNativeDriverTests` setzt `NO_PRETTY_VALUES`, damit CTest die deklarierten Fallnamen unabhängig von der diagnostischen Parameterdarstellung beibehält.

Vor dem Build prüft die native CI den Cache mit `sccache --zero-stats`. Bei einem fehlgeschlagenen Test werden beide Compiler-Launcher entfernt; Compiler-Konfiguration und Pflichtinventar bleiben erhalten. Konfigurations- und Buildfehler lassen den Job weiterhin fehlschlagen.

Die KVM-Abnahme verlangt den Abbruch einer echten vCPU ohne selbstständigen Ausstieg und 48 Zustandstransferergebnisse aus `KvmStateTransferCases.def`, einschließlich ioctl-Erfassung und fehlgeschlagener Abfragen optionaler Fähigkeiten. Weitere synchronisierte Registermodi laufen bei Hostunterstützung und werden andernfalls ausdrücklich übersprungen. Stabile Parameternamen sind unabhängig von ioctl-Nummern und Tupelformatierung. Protokolltests ergänzen native Ausführung, ersetzen sie aber nicht.

`native-host-probe.yml` führt das eigenständige `probe_native_host.py` auf gehosteten Linux- und Windows-Runnern für x64/ARM64 aus. `NativeHostProbe.def` legt die Reihenfolge der Nachweise für Fähigkeiten, VM/vCPU-Erstellung und Freigabe fest. Berichte enthalten Quell-/Binärdatei-Hashes, die native Host-ISA und alle Host-Statuscodes. `setup_ready` belegt nur die Einrichtung; Gastbefehle werden nicht ausgeführt. Fehlende API-/Gerätefähigkeiten ergeben `unavailable`; Fehler beim Bauen, Einrichten oder Freigeben sowie Zeitüberschreitungen und ungültige Nachweise lassen den Job scheitern. ARM64-Verfügbarkeit muss pro Lauf beobachtet werden; dieser Test belegt keine ARM64-Workload-Abnahme. Beide Linux-Workflows gewähren mit `prepare_kvm_ci.py` nur dem aktuellen gehosteten Runner-Konto Zugriff auf das vorhandene KVM-Zeichengerät und protokollieren Identität und Rechte. Lokale und selbst gehostete Systeme werden abgewiesen; fehlende Geräte werden nicht erstellt.

`windows-alignment-oracle.yml` erfasst mit `check_windows_alignment.py` und `WindowsAlignmentCases.def` 72 eigenständige Beobachtungen von Windows-x64-Ausnahmen: neun ausgerichtete SSE-Formen in je sieben Fällen mit nicht ausgerichteten Adressen/Zugriffsrechten sowie einem ausgerichteten Kontrollfall auf einer unzugänglichen Seite. Ausnahmecodes, Parameter, Fehler-PCs, gespeicherte Kontexte, Rohausgaben und Quell-/Binärhashes bleiben erhalten; Eingaben und RAM müssen unverändert bleiben. Dies belegt nur das OS-Verhalten, weder KVM/WHP-Ausführung noch zusätzliche SEH-Unterstützung.

Mit `native_cpu_only=true` aktiviert `native_driver_tests=true` die `NeverDNativeDriverTests` ohne Unicorn. Vor der Konfiguration prüft `build_wdk_driver_fixtures.py` den vollständigen SHA-256 der offiziellen Microsoft-Pakete WDK/SDK 10.0.26100.6584 und baut 68 normale/CFG/DBG-Treiberimages aus den Originalquellen. `WDKDriverFixtures.def` deklariert Paketidentitäten, Compiler- und Linkerargumente sowie Fixture-Zuordnungen. Unveränderte Microsoft-Dateien und ihre Lizenzen bleiben in den lokalen Build-/Cache-Verzeichnissen; CI lädt nur Build-Metadaten und Protokolle hoch. Das Manifest enthält Werkzeugversionen, Befehle, Quell-/Header-Hashes und Hashes der erzeugten Images.

`NativeDriverTests.def` verlangt 270 WHP-Ergebnisse aus allen 135 Workloads in `DriverBuiltinImages.def` und `DriverBackendParityCases.def`: 27 eingebaute Images, 68 WDK-Images und 40 Anfrageszenarien an ursprünglichen und verschobenen Adressen. Das vollständige Pflichtinventar lautet `5085 CPU + 270 WHP + 32 SEH + 77 scheduling + 30 wait sets + 29 driver UNPACK + 6 clock reads + 4 image memory checks + 6 CPUID + 7 kernel snapshots + 10 pool captures + 4 DriverEntry caller checks + 126 driver environment checks + 116 driver API checks = 5802`. Die 30 Wartemengenprüfungen umfassen sechzehn portable Modellfälle und vierzehn eigene native Treiberfälle. `run_native_cpu_ci.py --with-drivers` bewahrt genaue Identitäten und JUnit-Nachweise bei deaktiviertem Unicorn. Fehlende oder übersprungene Pflicht-Fixtures lassen diese optionale Prüfung scheitern; gewöhnliche Builds halten externe Fixtures optional. Feste Images behalten ihre erwartete Relokationsablehnung. Native ARM64-Gastausführung bleibt ungeprüft.

Das WDK-Inventar baut auch `driver_device_queries.c`, `driver_file_objects.c` und `driver_file_handles.c` als Normal- und CFG-Varianten. `DriverDeviceQueryTests.cpp` und `DriverFileObjectTests.cpp` verwenden dieselben Prüfungen wie portable Aufrufer für gehaltene Referenzen, fehlgeschlagenes CREATE, Prozessidentität, letzten Handle-CLEANUP und verzögertes CLOSE bei beiden Verträgen, Basen und Schedulingarten. Alle drei Familien sind native Pflichtprüfungen; das Inventar entdeckt `NEVERD_WDK_FIXTURE`-Quellen unabhängig und erkennt fehlende WDK-Builds.

`InterruptionRetainsPhaseCauseDeadlineAndLease` injiziert Fristablauf, Stopp und beides vor zwei unterschiedlichen Startinstruktionen. Geprüft werden genaue Phasendiagnose, eigene Nachrichtenlebensdauer, Fehlertyp und Ursachenbits, eine unveränderte gemeinsame Frist sowie freigegebener Speicherbesitz. Echte Transportfehler und Zustandsabweichungen bleiben getrennt. Das native x64-Startvalidierungsbudget beträgt `5 s`; normale Gastfristen und Einzelschrittzulagen bleiben unverändert.

`WhpResourcePolicy.def` gewährt der WHP-Ressourcenerstellung auf x64 und ARM64 vor der ISA-Validierung eine eigene Frist von `30 s`. Nach synchroner Hosteinrichtung wird die Frist vor Freigabe der Ressource geprüft. Instruktionsprüfungen und normale Gastfristen behalten ihre Grenzen. `WhpResourceTests.cpp` prüft typisierte Initialisierungsunterbrechungen, Ursachendiagnosen, Freigabe nach Abbruch, Vorrang von Hostfehlern und unveränderte normale Ausführungsfristen.

`X64PopFlagsTests.cpp` prüft beide Privilegien und `driver-strict`: 256 erlaubte Flagbilder mit zwei Anfangszuständen, neun Codierungen, alle 64 Eingabebits, schreibgeschützte/ausführbare Aliase, seitenübergreifende Fehler und Reparatur, Beobachterabbruch/-fehler, abgelehnte Gerätestacks und folgende native Instruktionsgrenzen. `X64PopFlagsOracle` führt Originalinstruktionen unabhängig auf x64-Hosts aus und prüft CPL3/IOPL0 sowie den genauen Stackverbrauch. `driver_resource_flags.def` lässt den ursprünglichen WDK-Ressourcentreiber Flags mit beiden Operandenbreiten setzen, löschen und wiederherstellen. Vollständiger Integer-/Kontroll-/x87-/SSE-Zustand bleibt erhalten; Gast-TF/NT/AC/ID und native ARM64-Ausführung sind dadurch nicht belegt.

`X64StatusFlagsTests.cpp` prüft `CLC/STC/CMC`, `LAHF/SAHF`, alle 256 AH-Eingaben und zugelassenen Flagkombinationen, alle REX-Präfixe, vollständigen CPU-Zustand, unveränderten Speicher, Beobachterstopps/-fehler, Kontexte und native ADC/Speicher-Fortsetzung. Ungültiges LOCK wird ohne Wirkung abgewiesen. Ein unabhängiges Host-Orakel prüft nach CPUID-Kontrolle 24 Präfixfolgen. Originale WDK-Ressourcentreiber führen alle fünf Befehle aus; 22 native Ergebnisse werden verpflichtend. Nicht verfügbare Host/ISA-Fälle bleiben explizite Skips. Das portable Unicorn-Profil führt dieselben sieben Fälle aus; direkte Abhängigkeitstests prüfen AH und LOCK in 16/32/64 Bit, explizite REX-Register und die Ablehnung bei fehlender Long-Mode-Funktion.

`X64DoubleShiftTests.cpp` prüft alle zugelassenen imm8/CL-Zählwerte, überlappende/erweiterte Register, definierte Flags, seitenübergreifende RAM-Beobachtungen, Abbruch, Rechte-/Mapping-/Gerätefehler, LOCK/undefinierte Zählwerte, Kontexte und native ADC-Fortsetzung. Ein unabhängiges Host-Orakel prüft 5.184 Originalausführungen; zwölf WDK-Proben decken Register und RAM ab. Die native Prüfung erhält 145 weitere Pflichtresultate.

`X64ScalarShiftTests.cpp` prüft alle Byte-Zählwerte, beide Carry-Eingaben, Null-, Eins- und vorzeichenbehaftete Bitmuster, implizite Einerschritte, AH/SPL- und Zählregister-Aliasse, den vollständigen CPU-Zustand, genaue RAM-Bereiche, Beobachter-Rollback, Fehler und Kontextfortsetzung. Ein unabhängiges natives Orakel prüft 65,536 Ausführungen. Der WDK-Ressourcentreiber ergänzt 72 eigene Proben. Die KVM/WHP-Prüfung verlangt 769 Ergebnisse für diese Familie. Nur ein maskierter Zählwert von null garantiert den Erhalt aller Flags. Eine vollständige `RCL/RCR`-Drehung durch den Carry-Ring mit einem Zählwert ungleich null erhält Operand und CF, lässt OF aber undefiniert; das Orakel klammert nur dieses undefinierte Bit aus.

`X64LoopTests.cpp` prüft 21 eigene Kodierungen, Zählerüberläufe, vorzeichenbehaftete relative Ziele oberhalb von 4 GiB, den Erhalt des vollständigen CPU/RAM-Zustands, Beobachterabbruch, Befehle über getrennte Seiten, Fehler beim Zielfetch und Kontextwiederherstellung. Unvollständige Befehle werden vor der Ausführung abgewiesen; ein Fehler beim Zielfetch erhält Zähler und PC des ausgeführten Sprungs. Der WDK-Ressourcentreiber ergänzt 12 eigene Proben. KVM/WHP verlangen 379 Ergebnisse dieser Familie für Supervisor-, Benutzer- und Treiberverträge. Das Orakel mit ursprünglichen Hostbefehlen führt bis zu 1,008 Fälle aus und meldet deren Anzahl. Genommene AMD-Sprünge mit `66H` zielen auf vom Hostsystem reservierten niedrigen Speicher; diese Formen laufen daher in der Gastmatrix mit explizit eingeblendeten niedrigen Zielen. Intel-/AMD-Zielbreiten und der Vorrang von REX.W werden getrennt geprüft.

`X64BranchTests.cpp` prüft alle 16 Jcc-Bedingungen und relative JMP, neun Präfixfolgen, kurze/nahe Formen, vorzeichenbehaftete relative Ziele oberhalb von 4 GiB, den gesamten CPU/RAM-Zustand, Beobachterstopps/-fehler, Seitengrenzen, Fehler beim Zielfetch und Kontextwiederherstellung. Die unabhängige Intel-Hostreferenz führt 9,792 Originalinstruktionen aus; AMD-Formen mit niedrigem Ziel bleiben in Gasttests. Beide Decodermodelle prüfen vollständige/abgeschnittene Bytes, native Proben die fehlgeschlagene Veröffentlichung. Vier eigene WDK-Ressourcenproben prüfen die Treiberrichtlinie. Jede KVM/WHP-Prüfung erhält 274 verpflichtende Ergebnisse. Das AMD-Softwaremodell belegt keine native AMD- oder ARM64-Ausführung.

`X64StackTests.cpp` prüft 42 Kodierungen in neun Gruppen: Breiten, Adressen, Gesamtzustand, Beobachterreihenfolge, Abbruch, Rechte, Seitengrenzen, physische Aliase, Fehlerbehebung, Geräteablehnung und Kontextwiederherstellung. Eine unabhängige Hostreferenz führt Originalinstruktionen aus; sechs WDK-Ressourcenproben prüfen den Treiberpfad. Jede native KVM/WHP-Prüfung erhält 1135 verpflichtende Ergebnisse. Nicht verfügbare Ausführungswege werden außerhalb ihrer verpflichtenden nativen Prüfung ausdrücklich übersprungen.

`X64FrameExitTests.cpp` prüft 14 `LEAVE`-Kodierungen, wirksame Präfixreihenfolge, vollständige RBP-Adressierung, alle Register, schreibgeschützte Aliase, seitenübergreifende Rahmenfehler und deren Behebung, Privilegien, Beobachter, ungültige Adressen, Geräteablehnung und Kontextwiederholung. Ein unabhängiges Host-Orakel führt 42 Originalbefehle aus; fünf WDK-Ressourcenproben prüfen die Treiberausführung. Beide nativen Prüfungen erhalten jeweils 379 zusätzliche Pflichtresultate.

`X64FrameEntryTests.cpp` prüft 14 Kodierungen, Verschachtelung, Überlappung, physische Aliase, reine Schreibprüfungen, Seitengrenzfehler und Reparatur, Beobachterabbruch, Rechte, abgewiesene Präfixe und Kontextwiederholung. Unabhängige Host-Orakel vergleichen Stackbytes und Register bei 882 erfolgreichen Ausführungen sowie 84 Fehlerfällen unter Linux x64. Zwei Tests mit injiziertem Transport unterscheiden Abbruch-/Fehler-Rollback von der Veröffentlichung architektonischer Fehler. Sechs WDK-Ressourcenproben führen die Originalinstruktionen im Treiber aus. Die nativen Prüfungen erhalten 508 zusätzliche Pflichtresultate für KVM und 507 für WHP.

`DriverSIMDSEHTests.cpp` führt acht originale SSE-Fehler mit vier unterstützten Dispositionen und einer abgewiesenen x87-Änderung, beiden nativen Verträgen, normalen/CFG-WDK-Images und zwei Ladeadressen aus. Zehn Ergebnisse je Backend und drei reine Prüfungen der Kernel-SSE-Datensätze sind verpflichtend. `driver_seh_simd.def` definiert Fälle und Modi; asynchrone Unwind-Tabellen beschreiben die fehlerauslösende Hilfsfunktion. Microsoft-Kernel 10.0.26100.9549 liefert unabhängige Nachweise: 107,744 Klassifizierungen und 8,192 Wiederherstellungen durch isolierte Instruktionspfade. Dies ist kein Treiberlauf in einem vollständigen Windows-Kernel; natives ARM64 KVM/WHP bleibt ungeprüft.

C-SEH-Bereiche bleiben halboffen. Ein gültiges Sprungziel von `__C_specific_handler` darf innerhalb seines geschützten Bereichs liegen: [LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608) erzeugt `EndLabel + 1` als Endadresse. Das Windows-Modell erhält diese Grenzen und prüft Ausführbarkeit, Funktionszugehörigkeit und Fortsetzungsidentität getrennt, auch nach Relokation. `KernelSEHContinuationCases.def` bewahrt das Layout des Original-Fixtures; `ScopeEndLabelMayOverlapTheHandlerLandingPad` prüft konstante Handler und Filter. Weitere Tests sichern das exklusive Ende und die Ablehnung ungültiger Ziele, ohne den Dispatch-Zustand zu verbrauchen. Diese reinen Modellprüfungen laufen in `NeverDNativeDriverTests` auch bei deaktiviertem Unicorn.

Auch das Ziel-Unwind verwendet das unveränderte Bereichsende: Ein `finally`, dessen geschützter Bereich das Handlerziel noch enthält, wird nicht verlassen. `FinallyRespectsRawScopeEndAtHandlerTarget` prüft beide Seiten dieser Grenze und vergleicht sie unter Windows x64 direkt mit `ntdll.dll!__C_specific_handler`. NeverD repariert keine vom Compiler erzeugten Bereiche. Clang-20/21-Builds des Original-Fixtures liefern in den Modi `T` und `J` einen Gastfehler, weil die verschobene Endadresse das ausgewählte Ziel enthält; Clang 23 führt beide Aufräumroutinen aus. [LLVM-Änderung #144745](https://github.com/llvm/llvm-project/pull/144745) entfernt den alten `+1`-Versatz. Diese compilerabhängigen Ergebnisse werden von Backendfehlern unterschieden.

Das Build-Inventar erfasst alle eigenen WDM/KMDF-C-Fixtures und alle optionalen WDK-CMake-Pfade. Für jede veröffentlichte `driver-*-scenario.json` enthält `DriverBackendParityCases.def` normale und CFG-Fälle; fehlende Quell-, Build- oder Szenariozuordnungen lassen die Inventartests scheitern. `Original` entfernt die Adressvorgabe des Szenarios und prüft die bevorzugte Bildbasis; `Rebased` prüft die deklarierte verschobene Basis. Das Szenario mit treibereigenen IRPs bricht absichtlich eine untergeordnete Anforderung ab; `DriverNativeOutcomes.def` behält deshalb nach erfolgreicher Bereinigung das erwartete negative Gesamtergebnis bei.

```bash
python3 scripts/build_wdk_driver_fixtures.py \
  --output build-driver-fixtures --cache build-driver-packages
cmake -S . -B build-native -G Ninja \
  -C build-driver-fixtures/fixtures.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
```

`NeverDAArch64StateTests` prüft Schäden an jedem skalaren Feld und beiden Wörtern jedes Vektors, Privilegänderungen, fehlende Gleitkommaausführung und erhaltene Transportdiagnosen. `NeverDAArch64FPTests` führt `OriginalProgramChecksCompleteStateAndOneDeadline` mit unabhängig assemblierten `AArch64ProbeCases.def`-Befehlen auf echten Transporten in beiden Privilegien aus. Der Test verlagert diese PC-unabhängigen Befehle in Gastcode, ohne Benutzern Zugriff auf Monitorseiten zu geben. Unicorn-Ausführung und ausdrückliche native Skips ersetzen keinen nativen ARM64-Startnachweis.

`CheckedAArch64Instructions.def` und `AArch64InstructionEffects` erlauben in EL0/EL1 begrenzte Basis-FP32/FP64-Arithmetik, Vergleiche, Transfers und SIMD fester Breite. FPCR erhält vier Rundungsmodi, FZ und DN; FPSR erhält kumulative Zustände und QC. Nicht unterstützte Bits werden vor Änderungen abgelehnt. FP16-Arithmetik, SVE/SME, unmaskierte Ausnahmen, optionale Erweiterungen und nicht gelistete Formen schlagen ausdrücklich fehl. Windows-ARM64-Treiberladen und weitere OS-Umgebungen werden nicht hinzugefügt.

`AArch64InstructionEffects` besitzt skalare und FP/SIMD-RAM-Bereiche für einzelne und gepaarte Operanden bis 128 Bit. Der gemeinsame Adressraum prüft jede Seite vor Eintritt; `RAMTransaction` übernimmt nur vollständige deklarierte physische Schreibbereiche. Ein 128-Bit-Schreibbeobachter erhält vor Effekten zwei geordnete 64-Bit-Wörter. Stops und Fehler erhalten RAM, Vektoren und Writeback. Gleiche Xn/Vn-Nummern sind gültig; umlaufende Paarbereiche werden abgelehnt. `NeverDAArch64MemoryTests` verwendet unabhängige `AArch64CrossPageCases.def` und `AArch64VectorMemoryCases.def`.

`NeverDAArch64StateTests` prüft alle 71 Lesepositionen des vollständigen Zustands in beiden Privilegien, Breiten, fehlende Leser und Wiederholung. `NeverDAArch64FPTests` führt Originalbefehle aus `AArch64FPCases.def` für alle Vektorlane, gepackte Arithmetik, skalare/Vektor-FP-Ergebnisse, vier Rundungen, FZ/DN, kumulatives FPSR, Kontexte und Ablehnungen aus. `NeverDAArch64MemoryTests` prüft jeden Seitenübergang, Beobachterreihenfolge und Stops, Rechte, Aliase und wiederhergestellte Vektoren. `NeverDUnicornStateTransferTests` (`UnicornStateTransferCases.def`, `CapturesDeclaredWidthsWithoutStaleUpperBits`) injiziert jeden Skalar-/Vektorlesefehler nach echter Ausführung. Fehlende native Transporte werden ausdrücklich übersprungen; native ARM64-KVM/WHP-Nachweise werden dadurch nicht ersetzt.

`NeverDAArch64MemoryTests` prüft 18 Skalar-/Paarformen mit Unicorn, KVM und WHP bei beiden Privilegstufen: jeden Seitenübergang, Vorzeichen und Breite, Beobachterreihenfolge, gesperrte/fehlende zweite Seiten, explizite Fehlerübernahme und Wiederholung, wiederholte physische Aliase und Kontextwiederherstellung nach Aliasersetzung. Die frühere Ablehnung eines gültigen seitenübergreifenden Loads wurde vor der Änderung reproduziert. Nicht verfügbare Transporte werden ausdrücklich übersprungen; Unicorn-Prüfung und Cross-Compilation ersetzen keinen nativen ARM64-KVM/WHP-Nachweis.

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`) prüft inaktive CFG-Metadaten mit Nullflags, unveränderte Rückfallzeiger an beiden Ladeadressen, ungültige Slots/Ziele und fehlende Relokationen. Die Ausführungstests wählen Unicorn/KVM/WHP ausdrücklich mit `driver-strict` und `checked-x64-v1`; nicht verfügbare Backends werden einzeln übersprungen. `DriverPublicCLICases.def` wählt `--backend unicorn` für CLI-Vergleiche mit der kompatiblen C-API v1. Native und `auto`-Auswahl behalten separate öffentliche Tests und wechseln bei fehlender Host-API niemals stillschweigend das Backend.

Geprüftes Unicorn verwendet `MachineRunControl`: Ein Zeitrahmen umfasst ARM64-Pflege, Gastausführung und vollständige Zustandserfassung. `UC_HOOK_CODE` prüft das geliehene Stopptoken und die Frist am Instruktionseingang. Der synchrone Engine-Aufruf gibt die Hook-Referenz vor seiner Rückkehr frei; der Maschinenschritt behält die Kontrolle bis zur Veröffentlichung. Unicorn und WHP erfassen den vollständigen CPU-Zustand zunächst privat und prüfen dieselbe Kontrolle vor Veröffentlichung eines erfolgreichen Schritts. WHP legt den Zeitrahmen einmal vor der Vorbereitung fest. Eine authentifizierte x64-CPU-Ausnahme hat Vorrang vor einem während der Erfassung eintreffenden Stop. Die geprüfte RAM-Transaktion verwirft bei abgebrochener Erfassung spekulative Schreibzugriffe; der uneingeschränkte Softwarevertrag bleibt unverändert. `MachineInterruptedError` trennt bestätigten Abbruch von Host- oder Erfassungsfehlern. Die gemeinsame geprüfte CPU liefert `Stopped` oder `Deadline`, erhält CPU/RAM und erlaubt einen erneuten Versuch; echte Fehler bleiben auch bei gleichzeitigem Stop `BackendFailure`.

Regressionen zur Zustandserfassung: `NeverDUnicornStateTransferTests`, `NeverDUnicornMachineControlTests`: `StopDuringCaptureCannotPublishAndAllowsRetry`, `ExpiredCaptureCannotPublishAndAllowsRetry`, `CompletedStoreCannotPublishCancelledCapture`, `StopDuringCaptureCannotHideRealGuestException`. `UnicornPublicCapture.CancellationKeepsTypedExitStateAndRAMConsistent`; `NeverDKvmStateTransferTests`: `PublicCancellationRetainsStateRAMAndFailurePriority`.

`NeverDUnicornMachineControlTests` verwendet die originalen Speicherinstruktionen aus `UnicornMachineControlCases.def` auf echten x64- und ARM64-Engines in beiden Privilegstufen. `RejectedEntryPreservesStateAndRAMAndAllowsRetry` prüft Abbruch vor dem Schritt, Stopp oder Fristablauf am tatsächlichen Gasteintritt, unveränderten vollständigen Eingangszustand und RAM sowie einen anschließenden erfolgreichen Speichervorgang. Sein Test-Wrapper benötigt keinen Hypervisor und belegt keine native ARM64/WHP-Ausführung.

`RunDeadline::invoke` weist einen bereits gestoppten oder abgelaufenen WHP-Eintritt vor dem Hostaufruf zurück, bewahrt bei Abbruch das tatsächliche Hostergebnis und bestätigt das Ende der Unterbrechungsrückrufe vor Freigabe des geliehenen Stopptokens. KVM und WHP prüfen das vollständig erfasste private Zustandspaket im aufrufenden Thread mit Ausführungsrecht, bevor sie einen gleichzeitigen Stopp oder Fristablauf einordnen. Tatsächliche Host- und Erfassungsfehler sowie authentifizierte x64-CPU-Ausnahmen behalten Vorrang. Ein gewöhnlicher Erfolgszustand bleibt bis zum Ende der Abbruchprüfung privat; eine bestätigte Unterbrechung verwirft spekulative CPU/RAM-Effekte und erlaubt einen neuen Versuch. Vorbereitung, native Ausführung und Erfassung teilen sich eine einzige Schrittfrist. Die Abbruchkontrolle ist kooperativ und garantiert keine harte Echtzeitgrenze.

`NeverDRunControlTests` enthält die portablen `NativeEntryTests.cpp` sowie unter Windows mit WHP die `WhpEntryControlTests.cpp`. Hostrückrufe im Speicher prüfen ohne Hyper-V abgelehnten Eintritt, Wiederholung, späten Abbruch, erhaltene Fehler, Ergebnispriorität und bestätigte Rückruflebensdauer. `NeverDKvmRunTests` prüft Abschluss im aufrufenden Thread, Fehlerpriorität und abgelehnte Wiedereintritte. Die echten `NeverDKvmStateTransferTests` führen Originalbefehle aus `KvmStateTransferCases.def` aus; `ActualCPUExceptionOutranksStopDuringCapture` und `PublicCPUExceptionOutranksStopDuringCapture` stoppen nach tatsächlichem Register-/XSAVE-Lesen und erhalten Divisionsausnahme, Ursprungskontext, RAM und explizite Wiederaufnahme. Portable Tests mit Windows-ABI unter Wine belegen nur Thread- und Kontrollverhalten, keine native WHP-Ausführung. Nicht verfügbare native Transporte werden ausdrücklich übersprungen.

`NeverDInstructionFetchTests` führt checked-Programme für x64/ARM64 mit Unicorn, KVM und WHP im Supervisor- und Benutzermodus aus. `InstructionFetchCases.def` prüft wechselnde Operandenformen, relative Sprünge, Gast- und Hostschreibzugriffe über Codealiase, Kontextwiederherstellung, Rechteentzug, getrennten Seitenspeicher, Vorauslesen am Seitenende, ungültige oder abgeschnittene Kodierungen und die Ablehnung rekursiver Aufrufe. Die native Windows-CI verlangt den Erfolg aller x64-WHP-Fälle. Nicht verfügbare Host/ISA-Paare werden ausdrücklich übersprungen; portable ARM64-Ausführung belegt keinen nativen ARM64-Support.

`RepeatedOperandsSurviveCodeWorkingSetEviction` wiederholt Prüfungen der Operanden, Speicher-Hooks und relativen Sprünge vor und nach einer größeren Codemenge. Die geprüfte Ausführung verwendet begrenzte, selbst gehaltene Capstone-Metadaten nur bei übereinstimmenden aktuellen Befehlsbytes erneut. Jeder Schritt liest aktuelle Bytes und prüft Ausführungsrechte; vollständige Operandendetails und Decoder-Aliase bleiben bei Treffern erhalten.

`WhpStateTransferTests.cpp` injiziert Übertragungen für beide XSAVE-API-Generationen: geänderte Gruppen, vollständige Erfassung, ignorierte Füllbytes, Teilfehler, Abbruch, Ausnahmepriorität und Partitionswechsel. `ContinuedStepsReuseCapturedRegistersAndFP` zählt vermiedene Installationen; `PartialTransferFailuresPreserveStateAndForceFullRetry` verlangt vollständige Wiederherstellung. Diese Protokollprüfungen ersetzen keine nativen FP-, Zustandswechsel-, Treiber- oder ring3-Tests.

`CancelledDirectRunPublishesACompleteBoundary` prüft den vollständigen Zustand bei bestätigtem Abbruch einer direkten Ausführung. `FailedDirectCapturePreservesStateAndForcesFullRetry` verlangt bei fehlgeschlagener Register-, XSAVE- oder Metadatenerfassung während des Abbruchs einen unveränderten Aufruferzustand und einen vollständigen Wiederholungsversuch. Keine API-Generation darf einen unvollständigen Registerzustand veröffentlichen.

`WhpStateTransferCases.def` prüft außerdem Fehler nach jedem Teilpräfix der gemeinsamen Erfassung von 32 Registern sowie Konflikte in allen sieben Metadatenfeldern. Beide XSAVE-API-Generationen müssen den Aufruferzustand bewahren und beim erneuten Versuch vollständig wiederherstellen. Die Tests prüfen einen Registerleseaufruf pro Schritt und die Ergänzung ausgelassener XSAVE-Metadaten aus diesem Aufruf.

`windows-pe64-v1` unterstützt begrenzte Windows-x64/ARM64-Konsolenprozesse mit PEB/TEB, statischem und dynamischem TLS, `DllMain`, benannten Win32-APIs und expliziten azyklischen DLL-Graphen. Gastmodule unterstützen Code-/Datenimporte nach Name oder Ordinal, DIR64, weitergeleitete Exports und echte Loader-Listen. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` und `GetProcAddress` verwenden den konfigurierten Katalog. CRT/GUI, ARM64-Frame-basiertes Benutzer-SEH, Threads und allgemeine Windows-Kompatibilität bleiben unvollständig; native ARM64-KVM/WHP-Belege fehlen weiterhin.

Eingabebytes und gesamte Image-Ausdehnungen teilen jeweils `memory_limit`; Laufzeit-Mappings zählen zum Image-Budget. Vorbereitung teilt 65,536 Datensätze, 64 MiB Metadatenlesezugriffe, begrenzte Namen und die Arbeitsfrist, ohne harte Host-E/A-Zeitgarantie. Das eigene EXE→DLL→DLL-Fixture prüft Rebasing, Ordinale, gemeinsame Daten, API-Identität, `MEM_IMAGE`, Listen und EXE-TLS-Attach/Detach. `NeverDWindowsProcessTests` enthält das native Windows-Orakel, `NeverDPEProgramExportsTests` ungültige Metadaten und Budgets, `NeverDProcessPublicTests` C-ABI/CLI-Parität. Nicht verfügbare Backends werden explizit übersprungen.

`WindowsProcess.ClockServicesUseConsistentUnitsAndPreserveLastError` führt originale x64/ARM64-PE-Aufrufe für `QueryPerformanceFrequency`, monotonen Zähler, FILETIME, umlaufende Ticks und relative Wartezeit aus. `WindowsProcess.UnmodeledDelaysStopWithoutClaimingCompletion` prüft die Ablehnung alarmierbarer Wartezeiten, positiver absoluter Zeitpunkte und des INT64_MIN-Intervalls vor dem Abschluss. `NativeWindowsOracleRunsTheSameExecutable` führt auch das erfolgreiche Uhrenszenario direkt unter Windows aus; beide Gastregressionen sind für die KVM/WHP-Abnahme ohne Unicorn verpflichtend. Das Originalprogramm belegt ungenutzte `BOOLEAN`-Registerbits ausdrücklich mit Fremdwerten und verwendet typisierte 64-Bit-Konstanten, damit Epoche und INT64_MIN unter der Windows-ABI erhalten bleiben.

`ARM64 native backend build` kompiliert `NeverDEmulationNative` mit deaktiviertem Unicorn auf `ubuntu-24.04-arm` (KVM) und `windows-11-arm` (WHP) aus den festgelegten LLVM-Quellen. `audit_native_backend_build.py` prüft jede deklarierte native Quelldatei, die aktive Backend-Definition, den Kompilierbefehl, das ARM64-ELF/COFF-Objekt und die Hashwerte. `probe_native_host.py` protokolliert die Verfügbarkeit der Host-Einrichtung und die Ressourcenfreigabe; fehlende Funktionen werden ausdrücklich gemeldet und Einrichtungsfehler lassen den Job fehlschlagen. Damit sind Kompilierung und Host-Einrichtung geprüft; die Gastausführung bleibt unbestätigt. `NeverDCapstoneCompilerOptions.inc` beschränkt die Qualifizierer-Diagnoseoption auf C-Kompilierungen mit Clang; GCC und MSVC behalten ihre eigenen Warnregeln. `native_arm64_only=true` wählt diese ARM64-Komponenten-Builds und Einrichtungsprüfungen ohne das vollständige x64-CPU-Abnahmeprofil. Build-Prüfungen normalisieren Quell- und Build-Verzeichnisse vor dem Pfadvergleich, einschließlich Windows-8.3-Aliasnamen. Der ARM64-Komponentenjob aktiviert vor der Build-Prüfung ausdrücklich das ausgewählte KVM- oder WHP-Backend.

`WindowsTestExecution.def` setzt den Unicorn-ARM64-Vergleich `WindowsExclusive` auf `RUN_SERIAL`. Die CTest-Regel vermeidet Ressourcenkonkurrenz mit anderen Gastaufgaben und erhält die ursprüngliche Gastfrist von 60 s sowie sämtliche Ergebnis-, Register-, Berechtigungs- und nativen Hashprüfungen.

`run_native_cpu_methods.py` prüft die boolesche Eigenschaft `RUN_SERIAL` aus `NativeMethodExecution.def` und erhält sie in Methodengruppen und Ausführungsverträgen über mehrere Läufe hinweg. Methoden laufen nacheinander; ein nicht beendeter Kindprozess verhindert den Start der nächsten Methode. Unbekannte Eigenschaften und veränderte Shard-Verträge lassen die Prüfung weiterhin fehlschlagen.

`WindowsProcessLifetime` führt DLL-TLS und danach `DllMain` in Abhängigkeitsreihenfolge aus, anschließend EXE-TLS und Einstieg, auf einer CPU mit gemeinsamem Budget. Jedes Modul erhält einen eigenen TLS-Index und ausgerichteten Block aus dem relokierten, verknüpften Image im gemeinsamen 64-KiB-Bereich. Das reservierte TLS-Argument ist null; `DllMain` erhält bei Start/Prozessende einen undurchsichtigen Nicht-NULL-Wert. Explizites Prozessende trennt fertig initialisierte DLLs in umgekehrter Loaderlisten-Reihenfolge und danach EXE-TLS, auch vor dessen Initialisierung. Start-`DllMain(FALSE)` beendet mit `0xc0000142` ohne Detach. Fehler und erschöpfte Budgets erfinden keine Bereinigung. PE-Einstiegsreturn mit Gast-DLLs benötigt nicht unterstütztes Thread-Ende und stoppt explizit. Ein von null verschiedenes `SizeOfZeroFill` bleibt ausgeschlossen; Nullbytes im tatsächlichen TLS-Template sind unterstützt. DLLs ohne Einstieg erhalten TLS-Attach, aber keine Prozess-Detach-Benachrichtigung.

`WindowsProcessExports` löst statische Imports und `GetProcAddress` über dieselben Namens-/Ordinalidentitäten auf, einschließlich Code, Daten, Aliasen und Weiterleitungsketten. Nur tatsächlich verwendete Startweiterleitungen ergänzen Katalogmodule und Initialisierungsabhängigkeiten; unbenutzte laden keine Dateien. Namen unterscheiden Groß-/Kleinschreibung; fehlende Namen liefern NULL/Fehler 127, direkt abgefragte fehlende Ordinale einschließlich Lücken NULL/Fehler 182 und ein NULL-Abfrageargument Fehler 87, Erfolg erhält LastError. Unbekannte Modulhandles bleiben ununterstützt. Exakte Anbieter-/Namens-API-Einstiege werden einmal aus dem begrenzten Register reserviert. Die Auflösung prüft aktuelle PE-Header und Exportmetadaten jedes Abbilds, lehnt Änderungen oder unlesbare Bytes ab, begrenzt Ketten auf 64 Einträge und teilt verbleibende Metadatenbudgets und die Ausführungsfrist. Eine Weiterleitung auf eine Lücke liefert die Zielbildbasis und erhält LastError; Ordinal null liefert Fehler 87. Die Basis ist eine Datenadresse und erteilt keine Ausführungsrechte für Image-Header. Laufzeitweiterleitungen können konfigurierte Module laden und vor Rückgabe des Ergebnisses initialisieren. Änderungen aktiver Exporttabellen bleiben ununterstützt.

`WindowsProcessLoader` lädt ASCII-DLL-Basisnamen aus `windows.modules` und verwaltet explizite Referenzen, gemeinsame Abhängigkeiten und den Erhalt von Startmodulen. Wiederholte Weiterleitungsabfragen erhöhen die Referenzzahl nicht. Beim erneuten Laden erhält ein Katalogplatz eine neue residente Generation. TLS und `DllMain` laufen auf derselben CPU unterhalb angehaltener API-Stackframes; die Registerwiederherstellung erhält Gastspeicheränderungen und verwendet die aktuelle Rücksprungadresse. Reservierte Zeiger bei dynamischem Attach/Detach sind null. Fehlgeschlagenes Attach beim expliziten Laden liefert nach Bereinigung Fehler 1114, behält aber erfolgreiche unabhängige verschachtelte Ladevorgänge. Entladen gibt Abbild und TLS frei; erneutes Laden stellt Originalbytes her. Fremde Änderungen an Loader-Listen oder TLS-Zeigern werden abgewiesen. Datei-, Abbild- und Metadatenbudgets bleiben auch nach Fehlern kumulativ. Systemanbieter verwenden ihre eingeblendeten PE-Basen als Modulhandles. Dateisuche, Nicht-ASCII-Pfade, `LoadLibraryEx`-Flags, Importzyklen und reentrante Übergänge desselben gerade initialisierten oder entladenen Moduls bleiben ununterstützt.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` verwenden denselben aktuellen Gastumgebungsblock in den PEB-Prozessparametern. ASCII-Namen werden ohne Beachtung der Großschreibung verglichen; Werte sind UTF-16. Änderungen prüfen Eingaben, Kapazität und Schreibrechte vor der Veröffentlichung. Momentaufnahmen bleiben unabhängig von späteren Änderungen und geben ihren Gastspeicher beim Freigeben zurück. Das Modell begrenzt den Block auf 64 KiB; Zeichenketten und Ersetzungen sind begrenzt und prüfen die Ausführungsfrist. Unbekannter Zeigerbesitz, fehlerhafte Blöcke, ANSI-Codepages und überlappende Ersetzungspuffer bleiben ununterstützt. `WindowsEnvironmentTests.cpp` vergleicht eigene x64/ARM64-Fixtures auf verfügbaren Backends; CI verlangt ein unabhängiges natives Windows-Orakel.

`WindowsProcessHeap` verwaltet Allokation, `HeapReAlloc`, Freigabe und Größenabfragen des Prozessheaps gemeinsam. Größenänderungen erhalten die verbleibenden Bytes; `HEAP_ZERO_MEMORY` löscht hinzugefügte Bytes, `HEAP_REALLOC_IN_PLACE_ONLY` verbietet Verschiebungen. Fehlgeschlagene Größenänderungen erhalten den alten Block und liefern NULL mit `ERROR_NOT_ENOUGH_MEMORY` (8), entsprechend den nativen Beobachtungen. Separate Seiten geben bei Verkleinerung und Freigabe Kapazität zurück; vorbereitetes Wachstum und begrenzte Kopien prüfen die Ausführungsfrist. Eigene Heaps, Ausnahmeflags, unbekannter Besitz und unzugängliche Kopier- oder Löschbereiche stoppen ausdrücklich. `WindowsHeapTests.cpp` prüft beide ISAs, erzwungene Verschiebung, Budgetwiederverwendung und Fehleratomarität; CI führt dieselbe eigene EXE auch auf nativem Windows aus. PE-Sammeltests nutzen das gemeinsame CTest-Zeitlimit von 120 Sekunden; jeder Gast behält sein eigenes endliches Budget. Die Heap-Fixture erlaubt pro Prozess 20 Sekunden für vollständige Datenprüfungen mit WHP.

`WindowsSystemModules` erzeugt begrenzte PE64-Modellabbilder für `ntdll.dll`, `kernelbase.dll` und `kernel32.dll` auf beiden ISAs. ASCII-Abfragen über `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` und `GetProcAddress` teilen deren eingeblendete Basisadressen; PEB/LDR und `MEM_IMAGE` beschreiben dieselben Abbilder. Statische Importe, Namensabfragen und Gastweiterleitungen nutzen dieselben API-Gates und Exportauflösung. Anbieter bleiben fest resident, haben keine Gastinitialisierungs-Callbacks und verhindern nach Entladen gewöhnlicher Gast-DLLs keine Rückkehr vom Einstiegspunkt. Geänderte Header oder Exportmetadaten stoppen die Suche. Unmodellierte Systemexportnamen und von null verschiedene Systemordinale stoppen ausdrücklich; reine Schreibungsabweichungen modellierter Namen sowie leere Namen liefern Fehler 127, eine NULL-Abfrage liefert 87. Erzeugte Bytes und Adressen sind Modellregeln; Windows-Versionslayouts, native Ordinale und anbieterübergreifende Aliase werden nicht rekonstruiert. `WindowsSystemTests.cpp` vergleicht eigene x64/ARM64-EXEs mit nativem Windows und beobachtet acht Rückgaben des Anfangsthreads unabhängig.

`WindowsSectionFixture.inc` prüft zwei unabhängige Ansichten, Handle-Wiederverwendung, Lesen nach dem Schließen, Isolation nach Schreibzugriff, Aufheben der Abbildung und Erhalt des residenten Anbieters. Negative Fälle betreffen Namensräume, Rechte, feste Adressen und Offsets. `WindowsThreadFixture.inc` trennt Affinität und Verbergungszustand und prüft exakte Längen sowie obere Argumentbits. Originalprogramme laufen auch im nativen Windows-Orakel; Wine ohne `KnownDlls` kann den Abschnittsfall nicht validieren.

`WindowsProcessExceptions` implementiert `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` und `RaiseException` auf derselben CPU mit dem Prozessbudget. Geordnete Handler dürfen Registrierungen ändern, verschachtelte Ausnahmen auslösen, modellierte APIs aufrufen, DLLs laden und den Prozess beenden. Datenzugriffsverletzungen auf x64/ARM64 und x64-Ganzzahldivisionsfehler können nach Prüfung der Gaständerungen an `CONTEXT` fortgesetzt werden; allgemeine Register, SIMD und unterstützter FP-Zustand bleiben erhalten. Softwareausnahmen kehren über eine echte Rücksprunganweisung im modellierten Anbieter zurück. Grenzen sind 128 aufbewahrte Registrierungen und 16 verschachtelte Frames. Ungültige Ergebnisse, geänderte Ausnahmezeiger, nicht unterstützte Felder und Grenzüberschreitungen scheitern ausdrücklich. Stackbasiertes ARM64-SEH/Unwinding, Debugger-Zustellung und Ausführungs-/Schutzseitenfehler bleiben offen. `WindowsExceptionTests.cpp` vergleicht eigene EXE/DLL-Szenarien mit nativem Windows; native ARM64-KVM/WHP-Belege fehlen weiterhin. Datensätze für Softwareausnahmen tragen `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), unabhängig vom durch den Aufrufer gesetzten Flag für nicht fortsetzbare Ausnahmen; die ursprüngliche Windows-Programmdatei prüft die exakten Flagwerte für Software- und Hardwareausnahmen.

`WindowsProcessContext` behält den Ursprung jedes Dispatch-Frames bei. Unterstützte x64-Datenzugriffs- und Divisionsfehler setzen RF (`0x10000`) in `CONTEXT.EFlags`; `RaiseException` behält auch bei Software-Zugriffsverletzungen den aktuellen Kontext. Der Ursprung bleibt während VEH/VCH und SEH-Suche/Unwinding erhalten. Eine gültige Fortsetzung stellt logische CPU-Flags ohne RF wieder her; RF-Änderungen des Gasts werden vor der Zustandsübernahme abgewiesen. Dieses begrenzte Profil modelliert weder Befehls-Breakpoints noch gastgesteuertes RF. `WindowsExceptionTests.cpp` prüft gespeicherte Daten, Wiederherstellung und unveränderte CPU/RAM bei Ablehnung.

Nach unabhängigen nativen Beobachtungen bildet Windows ring3 checked-x64-Fehler mit `operand_alignment` auf `STATUS_ACCESS_VIOLATION` mit `[read, UINT64_MAX]` ab, auch bei Schreibbefehlen. Die CPU-Schicht liefert die Ursache; Windows errät sie nicht aus Vektor 13 und dekodiert den Befehl nicht erneut. `WindowsAlignmentProcessTests.cpp` führt originale PE-Befehle in 72 Fehlerszenarien und 9 Wiederholungen nach Adresskorrektur (`72 + 9`) aus und prüft PC, RF, XMM und RAM. Unklassifizierte oder widersprüchliche Fehler werden abgelehnt. Prozess- und Treiberberichte bewahren nullable `cause` und hexadezimales `error_code`; fehlende Werte bleiben vom Zahlenwert Null verschieden. Diese Zustellung gilt für das checked-x64-Benutzerprofil. Nach jedem Fehler oder Wiederholungsversuch mit korrigierter Adresse exportiert das Testprogramm die vollständige 4096-Byte-Seite; der Host prüft alle 81 Speicherabbilder und die tatsächlichen Abschlusszähler bei unverändertem Zeitlimit des Gasts. Native Prozess- und Anfangsthread-Beobachtungen verwenden `CREATE_DEFAULT_ERROR_MODE`: GoogleTest aktiviert das vererbte Flag `SEM_NOALIGNMENTFAULTEXCEPT`, durch das Windows die gemessenen Fehler automatisch beheben kann. Das Orakel beobachtet daher das Standardverhalten des Systems unabhängig von der Richtlinie des Testprogramms.

`AddVectoredContinueHandler` und `RemoveVectoredContinueHandler` verwalten eine eigene geordnete Liste; beide Handlerfamilien teilen die Grenze von 128 aufbewahrten Registrierungen. Akzeptiert ein vektorisierter Ausnahmehandler die Fortsetzung, sehen die Fortsetzungshandler denselben veränderbaren Ausnahmedatensatz und `CONTEXT`. Die abschließende Kontextprüfung erfolgt nach diesen Rückrufen, einschließlich verschachtelter Ausnahmen und DLL-Benachrichtigungen. Handles lassen sich nicht über die andere Handlerfamilie entfernen. `WindowsContinuationTests.cpp` vergleicht eigene EXE-Szenarien für Reihenfolge, vorzeitiges Ende, Listenänderungen, Kontextreparatur, Verschachtelung, Loader-Rückrufe und Prozessende mit nativem Windows. Der geprüfte Windows-x64-Vektorpfad erlaubt die Fortsetzung mit `EXCEPTION_NONCONTINUABLE`; dies belegt kein stackbasiertes SEH-Verhalten. Native ARM64-Ausführung bleibt ungeprüft.

`RtlCaptureContext` ist über `kernel32.dll` und `ntdll.dll` für x64 und ARM64 verfügbar. Das gemeinsame `WindowsProcessContext` und `IntegerABI` speichern PC/SP des Aufrufers, ohne CPU-Zustand oder LastError zu ändern. Native Windows-Beobachtungen bestätigen x64-Flags `0x10000f`, unveränderte nicht beschriebene Home-/Debug-/Vektorbereiche und die klassischen 32-Bit-x87-Adressfelder; ARM64 übernimmt LR als PC und setzt X0/LR im Datensatz auf null. Register, SIMD und Gleitkommasteuerung stammen vom Gast; x64-Selektoren und MXCSR-Fähigkeitsmaske folgen der konfigurierten Gast-CPU. Ungültige, falsch ausgerichtete oder teilweise unzugängliche Ziele scheitern vor dem Schreiben. `WindowsContextTests.cpp` prüft direkte Importe, Anbieterabfragen, VEH-Rückrufe, seitenübergreifende Ausgaben und atomare Fehler. `scripts/check_windows_context.py` führt das Originalprogramm unter Windows x64/ARM64 aus und prüft nicht leeren x87-Zustand mit einem eigenen nativen Orakel. Diese ARM64-API-Beobachtungen belegen keine native KVM/WHP-Ausführung. Kontextwiederherstellung, Stack-Walking und dynamische Funktionstabellen bleiben getrennte Aufgaben. `WindowsProcessServices.def` legt genaue Modulbeschränkungen fest: Die Suche in `kernelbase.dll` liefert entsprechend den nativen Beobachtungen `ERROR_PROC_NOT_FOUND` (127), ohne einen zusätzlichen Export zu erfinden. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` nutzt das gemeinsame `X64SEH` unter `os/windows/exception/` (`NeverDEmulationWindowsException`, auch ohne Treiber) für x64 `__C_specific_handler` und UNWIND_INFO V1. Nach der VEH-Suche folgen Filter, finally-Aufrufe, nichtlokale Handlerübergänge, verschachtelte/kollidierte Abwicklungen und verschobene EXE/DLL-Frames; nichtflüchtige GPR/XMM bleiben erhalten. Bei Filterfortsetzung läuft VCH mit demselben `CONTEXT`. `WindowsSEHTests.cpp` vergleicht 23 eigene Szenarien mit nativem Windows; KVM/WHP/Unicorn teilen die Semantik. Das Prozessbudget umfasst erneute Prüfungen von Image-Generationen, Headern, Unwind-/Scope-Bytes, Codebereichen der Sprachhandler und IAT-Bindungen. Veränderte Metadaten oder entladene beibehaltene Images scheitern ausdrücklich. ARM64-Frame-SEH, C++ EH, dynamische Funktionstabellen, allgemeines RtlUnwind/NtContinue und Abwicklung über Loader-/VEH-/VCH-Callback-Grenzen bleiben ununterstützt.

Bei `EXCEPTION_NONCONTINUABLE` löst ein x64-Filter mit Rückgabe `EXCEPTION_CONTINUE_EXECUTION` eine `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, Flags `0x81`, verknüpfter Datensatz null) mit neuem Kontext aus. VEH läuft erneut vor der Suche im erhaltenen logischen Stack; finally-Reihenfolge, EXE/DLL-Frame-Identität sowie Tiefen- und Ausführungsbudgets bleiben erhalten. Die 23 nativen Szenarien umfassen 21 erfolgreiche Ausführungen und zwei Beendigungen: Eine in VEH/VCH akzeptierte Fortsetzung dieser sekundären Ausnahme bleibt auch nach Wiederherstellung des ursprünglichen `CONTEXT` unbehandelt. Das Modell meldet einen Laufzeitfehler. Software-Ausnahmeadressen entsprechen dem gespeicherten PC; interne Dispatcher-Adressen und Registerlayouts sind Modellvorgaben. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

`WindowsDynamicTests.cpp` vergleicht originale x64/ARM64-DLLs und EXEs mit unabhängigen nativen Windows-Beobachtungen: Referenzen, gemeinsame Abhängigkeiten, verschachteltes Laden, Fehlerbereinigung, Weiterleitungen, Prozessende, DLLs ohne Einstieg und frisches TLS beim Neuladen. Zusätzliche Tests lehnen manipulierte Loader-Metadaten und veraltete Codezeiger ab, erhalten kumulative Budgets und lassen unterbrochene API-Ergebnisse unvollständig. Windows-CI verlangt natives Orakel und WHP-Fälle; Cross-Kompilierung und Unicorn ARM64 belegen keine native ARM64-Ausführung.

Eine fehlende Bibliothek in der Weiterleitungskette von `GetProcAddress` ergibt Fehler 127; ein explizites `LoadLibrary` für ein fehlendes Katalogmodul ergibt 126. Das native Orakel und jedes verfügbare Backend prüfen alle 41 deklarierten Ladeszenarien. Unter Windows wird die Rückkehr nach dem Entladen aller DLLs je DLL-Variante 16-mal beobachtet. Fehlgeschlagene Initialisierung über eine `GetProcAddress`-Weiterleitung liefert nach Bereinigung ebenfalls 127. Prozess-Detach-Callbacks erhalten den Stackinhalt des beendenden Aufrufers.

`WindowsExportTests.cpp` prüft mit ursprünglichen x64/ARM64-DLLs und einem EXE weitergeleitete Code-/Daten-/Ordinalaufrufe, Aliase, Initialisierungsabfragen, Rebasing, Groß-/Kleinschreibung, fehlende Exports, LastError, Zyklen, nicht residente Ziele, ungültige Zeiger und Metadatenänderungen nach erfolgreichen Abfragen. Dasselbe EXE dient einem unabhängigen nativen Windows-Orakel; WHP-Fälle sind in nativer CI Pflicht. C-ABI/CLI-Tests vergleichen vollständige Berichte. Native ARM64-Hardwarebelege stehen noch aus. EXE-Varianten mit und ohne Exporttabelle prüfen beide Graphen, PEB- und Detach-Reihenfolge sowie Fehler für Namen, Ordinale und NULL.

`WindowsLifetimeTests.cpp` vergleicht feste Traces mit unabhängigen nativen Windows-Prozessen und KVM/WHP/Unicorn: normales Ende, Einstiegsreturn, beide DLL-Fehler, vier frühe Enden und DLLs ohne Einstieg. Hinzu kommen Callback-Fehler, gemeinsame Budgets, relokierte TLS-Felder und Gesamtkapazität. Die native Return-Prüfung behält das Handle des Anfangsthreads und prüft 64-mal dessen Exitcode sowie die genaue Thread-/Prozess-Benachrichtigungsfolge. Verbleibende Kindthreads werden nach der Beobachtung beendet; der Prozess-Exitcode gilt nicht als Einstiegsrückgabe.

`NeverDUnpackTests`, `NeverDUnpackExecutionTests` und `NeverDUnpackPublicTests` decken die Wiederherstellung gepackter Abbilder ab; siehe [Entpacken](unpack.md). `UnpackGeneratedTests.cpp` prüft die Einstiegsregeln auf x86-64 und ARM64 mit einem Programm, das der Test selbst packt. `X64ReturnPrefixTests.cpp` prüft den Zwei-Byte-Nahrücksprung auf jedem Transport und dass jeder andere Rücksprung mit Präfix abgelehnt bleibt. `WindowsDeferredTests.cpp` prüft opake Einstiege und die Beobachtung angehaltener Prozesse; `ExecutionSessionTests.cpp` prüft Ausführungsüberwachungen. `DirectX64Tests.cpp` prüft Teilseitenüberwachung, seitenübergreifenden Befehlsabruf, einmalige Fortsetzung, Dienste, ungültige Befehle und den Zustand beim Zeitablauf.

`DirectX64.UnobservedStoresInvalidatePhysicalWriteWitnesses` prüft geänderte, gleichwertige und wiederhergestellte Schreibwerte ohne Schreibüberwachung. Direkte Ausführung entwertet alte physische Schreibverläufe konservativ, auch bei ungültigen Befehlen und Zeitlimits. Im Stillstand kann ein neuer Verlauf entstehen; Ausführungsbeobachtung und Benachrichtigungen über bestätigte Schreibvorgänge bleiben erhalten.

`LiveHeapReferencesCannotPublishAnOrdinaryRecoveredImage`, `ExplicitSnapshotsKeepExternalHeapDependenciesVisible` und `ReleasedHeapStateDoesNotBlockRecovery` vergleichen unabhängig kompilierten Start und Einstieg auf verfügbaren geprüften und direkten Backends. `HeapReferencesInCapturedTLSCannotBeDiscarded` prüft reine TLS-Abhängigkeiten. Öffentliche Tests verlangen, dass Ablehnung vorhandene Dateien erhält und explizite Snapshots in C-API und CLI übereinstimmen. Adresstreffer sind konservative Belege, keine Bestätigung nativer Ausführung.

`DirectServiceBindingsRequireAnExplicitSnapshot` prüft erstmals vor und nach dem erfassten Einstieg verwendete direkte Dienstbindungen. C API und CLI prüfen zusätzlich, dass eine Ablehnung vorhandene Ausgaben erhält.

`PrivateHeapDestructionReleasesOnlyOwnedBlocks` prüft Eigentümerschaft nach Verschiebungen, die Ablehnung fremder Heaps, entwertete Handles und wieder verfügbare Heapkapazität.

`UnpackLibraryTests.cpp` packt unabhängige x64/ARM64-DLLs im Test und prüft Abhängigkeitsreihenfolge, normale/generierte TLS-Callbacks, Fehlerbereinigung, Eingabe-/Hostidentität, eigenen Dateizugriff, Namen/Ordinale/Daten/Weiterleitungen und fehlende Selbstimporte. Natives Windows lädt Originale und rekonstruierte DLLs über eine separate EXE und ruft deklarierte Exporte auf; geprüfte und direkte WHP-Fälle sind Pflicht. `CompletedGeneratedTLSCallsRequireTheAttachABI` verwirft geänderte Eintritte/Argumente; `GeneratedCallsNeedTheirReturnedStackAtTheContinuation` verwirft falsche Rücksprungstapel. Dies prüft Entpacken ohne Devirtualisierung.

`ExportObserver` beobachtet auch ausführbare Exporte residenter Gastabhängigkeiten; modellierte Anbieter bleiben an der Dienstverteilung beobachtet. Eigene Eingabe-Exporte sind ausgeschlossen. Moduländerungen erneuern die Haltepunkte; jede Reparatur verlangt die aktuelle Exportidentität. Aufzeichnungen bleiben innerhalb des deklarierten Importlimits. DLL-Tests reparieren System-API- und Gastabhängigkeitshelfer; natives Laden prüft, dass keine emulierte Adresse übrig bleibt.

`WrappedEntriesRequireExplicitTransferEvidence` prüft einen DLL-Wrapper, der den wiederhergestellten Eintritt auf tieferem Stapel aufruft. Standard bleibt `no_entry`; die Wahl des beobachteten Aufrufs mit `transfer` erzeugt eine ladbare DLL. Ein tieferer Aufruf allein unterscheidet Eintritt und Initialisierer nicht.

`WindowsDeferred.EarlierTLSCallbackMayGenerateALaterCallback` verlangt eine isolierte `.gentls`-Sektion mit `IMAGE_SCN_CNT_UNINITIALIZED_DATA`, exakt dem deklarierten Pufferumfang sowie Rohdatengröße und Rohdatenzeiger null. `WindowsDeferredCases.def` definiert Speicher und Assembler; gewöhnliches `.data` bleibt getrennt. Generierte Callbacks und Einsprungpunkte behalten die Prüfungen für strikte Ablehnung und verzögerte Ausführung auf x64/ARM64.

`ExtendedRegistersLoadOrdinaryImportsAgain` prüft kompakte und aufgefüllte R8-R15-Ladeformen mit geprüfter und direkter x64-Ausführung. Niedrige Register decken ein vorheriges REX-ähnliches Byte und Adresshilfen nur mit CALL ab; aufgefüllte Aufrufe überspringen beliebige Bytes nach CALL. `ImportCallHelpersCannotDiscardPersistentEffects` verlangt erhaltene dauerhafte Effekte. `PERebuildTests.cpp` lehnt fehlende Start-/Ergebnisnachweise und überlappende Starts ab und erhält die genaue API-Rücksprungadresse für sechs bis acht Bytes.

`OpaqueExportCallsAreRepairedBeforeTheExplicitStop` stellt einen reinen Aufruf ohne Umgehung einer unbekannten API wieder her. `ExportObservationIncludesTheOpaqueBoundary` prüft statische, dynamische und ordinale Exporte bei unveränderter Ausführung und Dienstprotokollierung. `OpaqueExportObservationPreservesAnUnreadableReturn` verlangt, dass fehlende Rückkehrdaten fehlen bleiben.

`ExportIdentitySurvivesRebindingAndLateResolution` ändert die Bindungsreihenfolge opaker Exporte und löst einen Export nach dem Einstieg auf. Die checked/direct-x64-Fälle verlangen die richtige API-Identität und behalten den expliziten unsupported-service-Stopp bei.

Der virtuelle Windows-Speicher ergänzt `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` und `FlushInstructionCache` für den aktuellen Prozess. Die OS-Schicht verwaltet Reservierungen; `AddressSpace` bleibt maßgeblich für zugesicherte Seiten, Zugriffsrechte und deren Speicher. Tests prüfen Codeänderungen, Zugriffsfehler und die Wiederverwendung des Speicherbudgets.

`WriteProcessMemory` folgt bei Schreibzugriffen von höchstens 4 KiB auf den aktuellen Prozess dem auf x64/ARM64 beobachteten Verhalten zugesicherter Seiten. Der Schutz jedes Bereichs, kopierte Präfixe, Bytezahlen und LastError bleiben erhalten, einschließlich `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY` und Erfolg nach einem RX-Präfix. `WindowsMemoryWriteTests.cpp` prüft alle 25 Schutzpaare; `check_windows_memory_write.py` überprüft dieselbe selbst erstellte ausführbare Datei in nativer Windows-CI. Nicht zugesicherte Zielbereiche bleiben ausdrücklich nicht unterstützt.

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

Regressionen prüfen exakte und zu kleine Budgets, Teilwörter, beide Byteordnungen, unberührte Speicheridentitäten, vollständige affine Slotgrenzen, spätes Überschreiben und Standardinvalidierung. C API/CLI prüfen v6-Kompatibilität und ungültige Bereiche. HighC und LLVMC laufen mit O0/O2 und prüfen Rückgabe, Speicher, Stack und erhaltenen Zustand; dies ist kein natives Äquivalenzzertifikat.

Regressionen prüfen Register- und Rahmenphasen, beide Bytefolgen, erreichbare ungültige Arme, spätere Vorgänger, ausgeschöpfte innere Guards und benachbarte Suchgrenzen. Besuchsgrenzen-Tests prüfen arithmetische Beziehungen, verschachtelte Schleifen, Dekodiermodi, sequenzielle Übergänge, Gesamtbudget und Legacy-Vorrang. Der CLI führt beide C-Wege und Quell-ABIs mit O0/O2 aus; C/Python v8 prüfen Layouts, ungültige Felder und ignorierte zukünftige Endfelder. Regressionen prüfen außerdem, dass hinter großen unabhängigen endlichen Selektoren Suchbudget für native Guards bleibt und die letzte zulässige Verfeinerung einem bereits nominierten Produzenten zugutekommt.

`NeverDLLVMCPhiTests` führt unabhängige und voneinander abhängige Schleifenaktualisierungen bei O0/O2 aus, einschließlich null Iterationen, Iterationsgrenzen und zufälligen Startwerten voller Bitbreite. Lesbarkeitsprüfungen verlangen keine lokalen Kopien für unabhängige Updates und nur die nötige Kopie bei zusammengesetzten Vertauschungen. Bestehende Tests für Verzweigungen, switch, verschobene Zweige und Vertauschungsschleifen prüfen weiterhin die gewählte Kante und gleichzeitige Zuweisungen.

Fünf unabhängige Tests in `LLVMCInternalExitRegions` prüfen beide Polaritäten, null Iterationen, parallele PHI-Vertauschungen, alle vier Verschachtelungen von Kopf-/Innenaustritten und die Beobachtungsreihenfolge. Gemeinsame Ziele und frühe Fortsetzungen prüfen ausführbaren Rückfall; eine spätere unzulässige Region darf trotz vorheriger gültiger Schleife keine Teilstruktur veröffentlichen. Modul- und Einzelfunktionsausgabe stimmen überein und verändern LLVM nicht. Original-LLVM und erzeugtes C werden bei O0/O2 über 294.912 Aufrufe mit unabhängigen vorzeichenlosen Orakeln verglichen, C mit Fallen für undefiniertes Verhalten.

`NeverDLLVMCPhiTests` prüft außerdem gemeinsame Schleifenaustritte mit verschiedenen Nachfolger-PHI-Paaren, geordneten Beobachteraufrufen, Ausgabespeicher und unveränderter Aufrufer-IR. C für das ganze Modul und einzelne Funktionen wird unter O0/O2 mit unabhängigen Referenzen verglichen. Unterschiedliche Austrittsvergleiche und zusätzliche Vorgänger prüfen die konservative Behandlung.

`NeverDLLVMCPhiTests` prüft die Zusammenfassung bei mehreren Rückkanten mit unabhängigen O0/O2-Orakeln, geordneten Beobachtern, Wertschnappschüssen vor speicherändernden Aufrufen, schmalem modularen Überlauf und Vorzeichenerweiterung. Modul- und Einzelfunktionsausgabe erhalten das ursprüngliche IR. Geprüft werden widersprüchliche Kanten, geteilte Wurzeln, poison-Annotationen, undefinierte Operanden, variable Schiebeweiten, eingeschränkte Intrinsics, Ausnahmefunktionen und vollständige Ablehnung bei Budgetmangel. Rotationen werden nur bei Übereinstimmung aller Eingangsoperationen zusammengefasst.

`NeverDLLVMCPhiTests` führt strukturierte skalare Regionen bei O0/O2 aus: verschachtelte Schleifen mit umgeordneten Blöcken, Diamanten, null Iterationen, schmale modulare Überläufe, Beobachter im Schleifenkopf, PHI-Tausch, lebendige äußere Werte, gemeinsame Schritte und Funnel-Shift-Grenzen. Geprüft werden unveränderte Quell-IR, Zusammenfassung auf drei lokale Variablen sowie ausführbare Rückfälle für mehrfache Ausgänge, irreduzible und zu große Graphen. Die synthetischen Fälle sind unabhängig; Quelltextausgabe zertifiziert keine native Wiederherstellung.

`NeverDLLVMCValueTests` vergleicht typisierten skalaren Schleifen-C-Code bei O0/O2 mit unabhängig direkt kompiliertem LLVM; undefiniertes Verhalten im erzeugten C löst einen Trap aus. Grenzwerte und deterministische Eingaben voller Breite prüfen schmale Multiplikation, Überlauf vor Schiebeoperationen, verbreiterte Multiplikation und Rechtsverschiebung, Boolesche Kürzung, vorzeichenbehaftete Vergleiche und Erweiterungen, Operatorrang, bedingte Ausdrücke, Boolesche Arithmetik, Fallbacks und materialisierte tiefe Ausdrücke. Die Tests prüfen außerdem unverändertes Aufrufer-IR und das Entfernen redundanter Casts.

`NeverDLLVMCPhiTests` und `NeverDLLVMCValueTests` prüfen Byte-Zähler mit Überlauf beim Inkrementieren/Dekrementieren, zusammengefasste Ausgangswerte, eingebettete Verwendungen nach der Schleife, lebende äußere Werte und PHI-Momentaufnahmen. Unabhängige O0/O2-Vergleiche mit Traps für undefiniertes Verhalten prüfen zusammengesetzte Addition, Ablehnung umgekehrter Subtraktion, schmale Multiplikation und Boolesche Masken. Verschachtelte Regionen verlangen schleifenlokale Zähler, einen getrennten Ergebnisträger und unverändertes Quell-LLVM. Ein ausführbarer Namenstest lässt externe Funktionen mit den zuerst erzeugten Ergebnis- und Zählernamen kollidieren und prüft Aufrufe und beobachtbare Effekte.

`NeverDLLVMCValueTests` prüft beide Positionen neutraler select-Zweige bei Addition, Subtraktion und Bitoperationen, unveränderte Basen, inline gelesene Altwerte, materialisierte Bedingungsschnappschüsse, schmale Wahrheitstests, nichtneutrale Zweige und gemeinsam genutzte select-Werte. Der erzeugte C-Code wird mit Fallen für undefiniertes Verhalten bei O0/O2 gegen unabhängig kompiliertes LLVM ausgeführt. `NeverDLLVMCPhiTests` prüft zusätzlich parallele Altwertschnappschüsse und zweigabhängige Initialisierungen im gemeinsamen Gültigkeitsbereich. Die aufrufende IR bleibt unverändert.

`NeverDUnicornDecodeTests` prüft reservierte EVEX-Bits der Registerformen auf AVX-512/APX-CPU-Modellen sowie die Priorität von ROUND-Speicherfehlern, Zustandserhaltung und Fortsetzung. Ein unabhängiges Linux-x64-Hostprogramm bestätigt Ausrichtungsfehler der klassischen Kodierung und Seitenfehler der skalaren/VEX-Formen. Diese Engine-Tests erweitern weder die im checked-Modus zugelassenen Befehle noch belegen sie native APX-Ausführung.

## ARM64-CPU-Leistungsmessung

Verwenden Sie einen Release-CPU-Build. Explizites HVF benötigt natives ARM64-macOS; für den Softwarevergleich muss Unicorn aktiviert sein. Jeder Ergebniswert wird geprüft. Initialisierung wird separat gemessen; CPU-Wechsel enthalten API-Aufrufe und Prüfungen, andere Ausführungslasten schließen Einrichtung und Prüfung aus.

```bash
cmake --build build-cpu --target neverd-cpu-bench --parallel 4
build-cpu/bin/neverd-cpu-bench --backend hvf --samples 7 --warmup 1
build-cpu/bin/neverd-cpu-bench --backend unicorn --samples 7 --warmup 1
```

Sichern Sie die Ausgangsdatei vor dem Neubau. Python 3.11+ misst mit wechselnder Reihenfolge. Bewahren Sie Konfiguration, Quelllabels, Binärhashes und alle Stichproben auf; während der Messung keine parallelen Builds oder Tests.

```bash
python3 scripts/benchmark_cpu.py \
  --baseline /path/to/before --baseline-label BEFORE_COMMIT \
  --candidate /path/to/after --candidate-label AFTER_COMMIT \
  --pairs 15 --output /path/to/new-comparison.json
```

Die separate Eintrittszählung beeinflusst Laufzeiten und enthält Startproben; ihre Zeiten sind keine Leistungsdaten. Diese Lasten belegen weder vollständigen OS-Durchsatz noch architekturübergreifende Leistung.

[Reproduktion](../testing.md#reproduce-checked-arm64-cpu-measurements) · [HVF](macos-hvf.md)

`NeverDLLVMPrivateFrameTests` prüft überlappende Schreibzugriffe, alle Eintrittspfade und Rückkanten, erhaltene Adressen, aliasierende Ausgaben, uninitialisierten/geordneten/unbekannten Speicher, Metadaten sowie atomare Ablehnung bei exaktem und um eins zu kleinem Budget. Unabhängige O0/O2-Orakel mit Traps für undefiniertes Verhalten vergleichen vollständige Rückgaben, externe Objekte und Frame-Bytes. Kompilierungen für x86-64, AArch64, Big-Endian-AArch64 und ARM32 belegen keine native Wiederherstellung dieser Architekturen. Bei Änderungen gemeinsamer Adress-/Effekthelfer auch `NeverDByteMemoryForwardingTests` ausführen.

```sh
cmake --build build-release --target NeverDLLVMPrivateFrameTests NeverDByteMemoryForwardingTests --parallel 4
build-release/bin/NeverDLLVMPrivateFrameTests
build-release/bin/NeverDByteMemoryForwardingTests
```

`NeverDByteCellScalarizationTests` prüft überlappende Wörter, zwei Eintrittspfade und zwei Rückkanten, breite und nicht-zweierpotenzige Zugriffe, beide Byte-Reihenfolgen, erhaltene Speicherwertwahl und Poison-Pflichten, partielles Überschreiben von Poison, vollständige Verwendungsprüfung sowie exakte/zu kleine Budgets über mehrere Objekte. Die normale Thin/Deep-Pipeline muss verbleibende Arrays entfernen. Unabhängige O0/O2-Orakel vergleichen alle 24 Ausgabebytes, umgebende Schutzbytes und Rückgabewerte für 8.192 Eingaben in drei Varianten: 49.152 Aufrufe mit Traps für undefiniertes Verhalten. Kompilierungen für x86-64, AArch64, Big-Endian-AArch64 und ARM32 sind von nativer Ausführungsabdeckung getrennt. Bei Änderungen gemeinsamer Speicherverträge diese Tests zusammen mit Byte-Weiterleitungs- und privaten Frame-Tests ausführen.

```sh
cmake --build build-release --target NeverDByteCellScalarizationTests --parallel 4
build-release/bin/NeverDByteCellScalarizationTests
```

`AndroidMutexTests.cpp` prüft mit unabhängigen O0/O2-Programmen und normaler/APS2/RELR-Packung drei Mutexarten, mehrere Wartende, erneute Konkurrenz, letzte rekursive Freigabe, errno, Ereignisidentität, ungültigen Speicher, Deadlocks und kumulierte Befehlsgrenzen. Unicorn und verfügbare KVM/WHP/HVF führen dieselben Fälle aus; nicht verfügbare Backends werden ausdrücklich übersprungen. Dies belegt keine Äquivalenz zu Android-Geräten oder parallelem SMP.

`HighControlFlowSemantics.DeepStableContainersPreserveEveryReturnPath` prüft 48 Ebenen von Block-, Schleifen-, switch- und Ausnahmeblöcken mit einem unabhängigen Interpreter auf betretenen und umgangenen Pfaden. Eine großzügige Laufzeitgrenze erkennt wiederholte rekursive Traversierung. Dies deckt strukturiertes HighIR ab; die Methodenwiederherstellung des gesamten Abbilds erfordert weiterhin eigene vollständige Inventar- und Abhängigkeitsprüfungen.

`SwiftOnceSources.EarlyReturnsKeepExactObjCOnceThunkProofs` prüft ARM64/x64-Thunks mit kombinierten oder getrennten retain-Aufrufen. `EarlyOnceCopyReturnsRequireTheSameCompleteTail` lehnt geänderte Schreibziele, fehlende oder umgeordnete retain-Aufrufe, geordnete Ladevorgänge, geänderte Ergebnisse und externe Einstiege ab. `IgnoredNestedReturnCopiesDoNotObserveOnceContext` prüft beide ausführbaren Ausgänge eines void-Callbacks und den Quellfluss nach der Projektion.

`HighControlFlowSemantics.ReturnTailCopyKeepsTheOuterLabelOwner` vergleicht Eintritts- und Umgehungspfade mit einem unabhängigen Interpreter, wenn eine Adresse in einem verschachtelten Block erneut vorkommt. `ReturnTailCopyIncludesTheFirstChildOfItsLabel` erhält den gültigen Eltern-/Erstkindfall samt Zuweisung. Diese gezielten Prüfungen ersetzen keine vollständigen Methoden- und nativen Abhängigkeitsvergleiche.

`JumpTailCopyKeepsTheOuterLabelOwner` prüft dieselbe Eigentumsregel für Sprungendstücke.

`EarlyStringGetterReturnsKeepOnlyInertOnceAnchors` prüft leere Anker zwischen früher Rückgabe und once-Aufruf und lehnt dazwischenliegende Aufrufe oder Schreibvorgänge ab.

`SwiftOnceSources.ObjCThunkRootsShareTheNestedCallbackProof` prüft den gemeinsamen Wurzelbeweis und die Ablehnung, sobald ein Blatt seinen Kontext beobachtet.

`SourceABI.SwiftPointActionRequiresTwoDoublesAndContext` / `ObjCCallHints.CoreGraphicsPointActionsKeepSwiftFloatingCarriers` prüft beide Architekturen und weist geänderte Anbieter, schwache Importe, widersprüchlichen Speicher und veraltete ABI-Träger zurück. `HighCSourceCalls.SwiftCoreGraphicsPointActionsKeepCoordinatesContextAndOrder` führt generiertes C bei O0/O2 mit unabhängigen Swift-Trägerprüfungen aus und kontrolliert Koordinatenbits einschließlich vorzeichenbehafteter Null, Subnormalzahlen und NaNs, Empfängeridentität, Aufrufreihenfolge und Schutzwerte. Dies belegt die Aufruf-ABI, nicht die vollständige Wiederherstellung übergeordneter Methoden.

`NativeSourceHints.CGContextCGRectMethodKeepsOrdinaryAndSwiftContextInputs` prüft den vollständigen Methodenbaum und die genauen Träger einschließlich privater Mitglieder und abgelehnter Signaturen. `SwiftFieldReceiver.CGRectMethodSelfKeepsItsLogicalParameterIdentity` und `CGRectMethodRejectsChangedEntryAndReceiverParameter` prüfen Pipeline und Veröffentlichungswiederholung und lehnen Änderungen an self-Index, Einstieg oder Argumenttyp ab. Compilerprotokolle decken vier macOS-/Mac-Catalyst-Ziele ab; die Einstiegsdeklaration bleibt auf arm64 begrenzt. `HighCSourceCalls.SwiftCGRectMethodKeepsContextReceiverAndAllCoordinateBits` führt erzeugtes C bei O0/O2 mit einer unabhängigen skalaren Swift-Referenz aus und prüft alle vier Koordinaten-Bitmuster, getrennte context/self-Zeiger, einen einzigen Aufruf und Speicherschutzwerte.

`NeverDLowInstructionBoundaryTests` führt die LowIR-Provenienztests ohne sämtliche Fixtures des gesamten Lift-Tests aus. `BackwardSharedReturnEpilogueKeepsReturnAndCallerFrame` prüft ausgerichtete Stapelfreigaben durch ADD und postindiziertes LDP, einschließlich der Wiederherstellung des Link-Registers im Aufrufer; das ursprüngliche RET X30 und der gemeinsame Einstieg bleiben unabhängig dargestellt. `BackwardSharedReturnEpilogueRejectsChangedReturnAndOwnership` lehnt andere Rückkehrregister, BR X30, fehlende oder falsch ausgerichtete Freigaben, schmale Wiederherstellungen, innere Einstiege, Fixups, beschreibbare oder mehrdeutige Zuordnungen, relozierbare Eingaben und andere Formate ab. Das Dekodieren eines gemeinsamen Endes beweist keine native ABI: fehlende Sicherungen oder Allokationen im Aufrufer lassen den bestehenden Frame-Nachweis weiterhin scheitern.

`NeverDOwnInteriorCallTests` deckt direkte x86- und x86-64-Aufrufe einer Marke im eigenen Unwind-Bereich der Funktion ab, unter einem Microsoft-x64-`.pdata`-Eintrag, einer System-V-x86-64-DWARF-FDE und einer i386-DWARF-FDE. Ein Aufruf, der nur der gepushten Rücksprungadresse wegen erfolgt, wird als Push und Sprung angehoben, und das erzeugte C für die geradlinigen und schleifenden x86-64-Fälle läuft mit `-O0` und `-O2` unter AddressSanitizer und Traps für undefiniertes Verhalten. Ein Ziel, dessen Rücksprünge genau die Rücksprungadresse dieses Aufrufs entnehmen, bleibt ein gewöhnlicher Aufruf; ein Rücksprung nach einem Stackwechsel oder unterhalb des Eintritts-Stackzeigers wird abgelehnt. Ein aus einer i386-Registrierungskette rekonstruierter Bereich begrenzt den Rumpf nicht, daher bleibt sein Aufruf ein Aufruf.

Vor der Einstufung als gewöhnliche Unterroutine weist die Prüfung des Rücksprungadressplatzes überlappende, unbekannte und atomare Schreibzugriffe sowie Aufrufe ohne nachgewiesene Speicherwirkung zurück. PE-, ELF- und Mach-O-Fälle prüfen teilweise beschädigte Rücksprungadressen und verbreiterte i386-Speicheradressen. Private Schreibzugriffe, verworfene Rücksprungadressen und terminierende Zweige bleiben zulässig.

Gemischte Ausgänge einer einzelnen internen Aufrufstelle bewahren beide Verhaltensweisen: Ein geprüfter naher RET ohne Operanden entnimmt seinen unbeschädigten Rücksprung-PC und setzt an der exakten Fortsetzung fort; ein Ausgang, der diesen PC verwirft, behält seinen ursprünglichen Kontrollfluss. `MixedExitsResumeOnlyTheMatchingInvocation` deckt PE/ELF/Mach-O x64 und ELF i386 ab, prüft optimiertes und nicht optimiertes LLVM und führt x64-C mit Sanitizern aus. Weitere Tests prüfen wiederholte Aufrufe und lehnen mehrere Aufrufstellen mit gemeinsamem RET, unabhängige oder Ausnahme-Einstiege, durch die Fortsetzung entdeckte Vorgänger, offene indirekte Ausgänge, beschädigte PCs, Rücksprünge mit unmittelbarer Stackbereinigung und Sprachruntime-Metadaten ab. Jeder neu aufgebaute CFG muss den Aufruf erneut beweisen; gemeinsame SEH-Callback-Rücksprünge sind damit nicht freigegeben. Nichtlokale Ausgänge beweisen außerdem den Rücksprungslot des Aufrufers separat: Der äußere RET muss den ursprünglichen Eintritts-PC am exakten Eintritts-SP lesen. Übermäßige Stackfreigabe und Schreibzugriffe, die diesen PC beschädigen oder aliasieren können, werden abgelehnt; disjunkte private Schreibzugriffe bleiben gültig.

`NeverDSysVCallContractTests` prüft x86-64-System-V-Aufrufverträge an den Formen von QtXmls `QDomNode::save` und `QDomNode::isDocument`. Ein direkter Aufgerufener, dessen Zusammenfassung ein Argumentregister liest, erhält den Wert des Aufrufers, auch ein unverändert durchgereichtes eingehendes `this`; ein virtueller Aufruf übernimmt das Objekt, das ein dominierender Block in `RDI` geladen hat; eine Methode, die auf einem Pfad ohne Schreiben von `RAX` zurückkehrt und auf den anderen nur Ergebnisse von Aufgerufenen weitergibt, ist void; und ein Byte, das vor einer Vergleichskette nach `AL` geschrieben wird, ist auf jedem Pfad der Rückgabewert. Die erzeugten Programme laufen mit `-O0` und `-O2` unter AddressSanitizer und Traps für undefiniertes Verhalten.

`ObjCCallHints.CIImageAffineValueKeepsProviderAndPhysicalCopyCarrier` prüft CoreImage-Anbieter, CIImage-Factory, vollständigen logischen 48-Byte-Record und x2-Zeiger; fehlende oder falsche Anbieter, x86_64 und widersprüchliche Deklarationen werden abgelehnt. `ObjCImageValueCopy.OriginalFrameAndCompleteBodyAuthorizePublication` trennt den ursprünglichen Aufruf von Ergebniszuweisungen an derselben Maschinenadresse. `RejectsChangedCopyCallBodyAndCurrentImage` verwirft 24 Änderungen an Belegen, Argumenten, Stores, Frames, Metadaten, Imports, doppelten Aufrufen und gespeichertem IR, einschließlich übereinstimmender Änderungen in MedIR und HighIR. `GeneratedCExecutesAgainstIndependentPhysicalCopyABI` führt unverändertes erzeugtes C mit O0/O2 auf ARM64 gegen eine Funktion mit dem unabhängig beim Compiler beobachteten x2-Zeiger aus; geprüft werden alle sechs Gleitkomma-Bitmuster, Selektor-/Empfängeridentität, eine Auswertung, Rückgabeobjekt, zulässige Kopieschreibzugriffe, unveränderte Eingaben und Speichergrenzen. Andere Hosts überspringen diesen physischen ABI-Ausführungstest.

`ObjCCallHints.CurrentMethodEncodingMustAgreeWithCachedDeclaration` lehnt eine gecachte ABI ab, die der aktuellen nichtleeren Methodenkodierung oder dem Selektor widerspricht; reine Deklarationsclients behalten ihren bisherigen Vertrag.

`DarwinIndirectRecordCalls.MatrixFrameEffectsRequireExactCurrentContract` deckt die aktuellen Matrix-/Affine-Verträge und 22 abgelehnte Vertragsänderungen ab. `ObjCAffineImageValueCopy.CurrentProducerInitializesThePublishedCopy` belegt den Weg des SDK-Ergebnisses zur CoreImage-Kopie und die unabhängige Veröffentlichungsprüfung. `RejectsWrongProducerFrameAndSavedIR` prüft zwölf Änderungen je Erzeuger, darunter fehlende Eingabeschreibvorgänge, Ergebnisse außerhalb des Frames, falsche Anbieter oder ABI-Träger und die Wiederverwendung einer verbrauchten Concat-Eingabe. `GeneratedCMatchesOriginalMachineAndSDKResults` führt unverändertes generiertes C und die ursprünglichen ARM64-Instruktionswörter mit nativem CoreGraphics bei O0/O2 auf Apple ARM64 aus: 1000 Fälle je Erzeuger vergleichen alle 48 Ergebnisbytes, beide Eingaben, Selektor-/Empfängeridentität, einen Aufruf, Rückgabeobjekte, private Schreibzugriffe und Schutzwerte. Andere Hosts überspringen diesen nativen SDK-Test.

`MatrixFrameEffectsRequireExactCurrentContract` prüft auch den CGRect-Verbraucher und dessen 22 abgelehnte Änderungen. `ObjCAffineImageValueCopy.CGRectInputUsesTheSameCurrentFrameOwner` prüft Rotation → CGRect-Borrow → erneute Rotation → CoreImage-Veröffentlichung. `CGRectBorrowRejectsExpiredInputsAndChangedABI` lehnt acht Änderungen an Initialisierung, Grenzen, Importen und Trägern ab. `GeneratedCMatchesOriginalMachineAndSDKResults` führt zusätzlich diese vollständige Folge bei O0/O2 mit 1000 Fällen gegen ursprüngliche Instruktionswörter und das native SDK aus; gesicherter Winkel, alle 48 Ergebnisbytes, Objekte und Schutzwerte werden verglichen.

`FrameMetadataAccessorUsesCurrentCatalogAndABI` prüft die gemeinsame Metadatendeklaration, beide Antwortregister und die aktuelle Veröffentlichung des Frame-Witness. `FrameMetadataAccessorRejectsChangedImportAndBytes` verweigert schwache Imports, geänderte Provider/Namen/Addends, private Adressanfragen, partielle Spills, falsche Reloads und geänderte Originalaufrufe. Das Oracle für Original-ARM64 und erzeugtes C ruft auch den echten Foundation-URL-Metadatenzugriff auf: Beide Varianten führen bei O0/O2 jeweils 2048 Fälle aus und prüfen dynamische Witness-Auswahl, sämtliche Ergebnisbytes, erhaltene Eingaben, Aufrufzahlen und Schutzwerte. Dynamische Stack-Allokation und Witness-Speichereffekte werden dadurch nicht bewiesen.

`AArch64ExclusiveTests.cpp` prüft Breiten, Paare, Acquire/Release, Registerüberlappung, Aliase, Fehler, Snapshots, Abbruch oder Fehler von Beobachtern sowie konkurrierende CPUs. `RAMReservationTests.cpp` prüft identische Schreibwerte, ABA, Wiederverwendung, Rollback sowie String- und `ENTER`-Interferenz auf KVM/Unicorn. Originale Windows-ARM64-Prozessdateien führen exklusive Schleifen aus. `scripts/check_aarch64_exclusives.py` erfasst Originalbefehle und Ausrichtungsfehler auf Windows-ARM64-CI. Diese nativen Beobachtungen belegen keine Ausführung der ARM64-KVM/WHP-Backends; nicht verfügbare Profile werden ausdrücklich übersprungen. Dieselben Fälle prüfen Software-Unicorn, vertragsübergreifende Zugriffe, gleichwertige und ABA-Schreibvorgänge, ausführbare Aliase innerhalb eines Laufs sowie `DC ZVA` und den Abbruch durch Beobachter.
 `windows-alignment-oracle.yml` führt auch den ARM64-Probelauf aus: 1.320 Beobachtungen decken jeden nicht ausgerichteten Offset, vier Lade-/Speicherfolgen sowie schreibbaren, schreibgeschützten, unzugänglichen und seitenübergreifenden Speicher ab. Registerstartwerte behalten ihre volle Bitbreite; vor einem Fehler abgeschlossene Teilzugriffe werden aufgezeichnet. `WindowsExclusiveProcessTests.cpp` gleicht 1.320 ursprüngliche Windows-ARM64-Beobachtungen mit den nativen Prüfsummen in `WindowsExclusiveNative.def` ab. Nur Code- und Datenadressen werden normalisiert; Register, Ausnahmemetadaten und RAM-Effekte bleiben erhalten.

`AArch64AtomicTests.cpp` prüft 168 unabhängig assemblierte LSE-Kodierungen, Registeraliase, vorzeichenbehaftete Vergleiche, Rechte, Abbruch, physische Reservierungen und NZCV-Transfers. `scripts/check_aarch64_atomics.py` sammelt 1.100 originale Windows-ARM64-Datensätze mit vollständigen Ergebnissen, Ausnahmekontexten und RAM-Bereichen; Parsertests weisen fehlende oder widersprüchliche Belege zurück. Native KVM/WHP-Ausführung muss gesondert validiert werden. `WindowsAtomicProcessTests.cpp` prüft den Checked-Prozess; `WindowsAtomicResults.def` enthält Prüfsummen der vollständigen Datensätze.

Strukturell konstante native Ziele verwenden direkt die bestehende Planung mit Machbarkeitsprüfung. Ein symbolisches Einzelziel behält das eingehende Prädikat erst nach vollständiger Aufzählung bei. Regressionen prüfen 128 konstante Übertragungen mit dem Abfragebudget geradliniger Ausführung und 32 berechnete Übertragungen mit zwei Aufzählungsabfragen pro Übertragung; freie obere Adressbits und Zweigdomänen bleiben erhalten. Geänderte Gesamtzustände, fehlende Ausrichtung, ein Ziellimit von null und zu kleine Abfrage- oder Befehlsbudgets werden abgelehnt. Bestehende Ablehnungstests für mehrere Ziele und unvollständige Aufzählung bleiben erforderlich.

Ein nativer Zweig behält die eingehende Domäne auf einer Kante erst bei, nachdem ein abgeschlossener UNSAT-Beweis die andere Kante ausschließt. Tests prüfen 32 bedingte Übertragungen in beiden Richtungen mit 512 Solver-Gattern, genaue und um eins verkürzte Abfragebudgets sowie erschöpfte Gatter. Geänderte oder fehlende Ausrichtung, umgekehrte Vergleiche und geänderte Endzustände müssen abgelehnt werden; bestehende Tests für beliebige undefinierte Steuerwerte und zwei mögliche Kanten bleiben erforderlich.

Bit-Blast-Caches und Traversierungsspeicher erfassen nur erreichte Ausdrucksknoten und Variablen. `NeverDSolverTests` prüft hohe, dünn verteilte Kennungen, Kontextwachstum zwischen inkrementellen Zusicherungen, die Wiederverwendung gespeicherter Bits, Modellextraktion und wechselnde Annahmen. Unbeteiligte breite Ausdrücke bleiben unkodiert; erreichte Breitenverstöße, fehlerhafte Wurzeln und erschöpfte Gatterbudgets werden weiterhin abgelehnt.

`SourceFrameAnalysis.CallStorage*` prüft Aufrufstellen, ankommende Definitionen, Initialisierung, Padding, Entweichen, Grenzen und Zyklen ohne Quellfreigabe. `ObjCFrameBlockBorrows.*` prüft deskriptorbegrenzte synchrone Ausleihen sowie 19 Änderungen an Importen, Headern, ABI und Maschinenbefehlen. Padding bleibt unbewiesen; fehlende Initialisierung von Besitzfeldern wird abgelehnt. Blockaufbau, Capture-Lesezugriffe und Callback-Abschluss bleiben eigene Veröffentlichungsprüfungen.

Die Tests zur Veröffentlichung von block und Kopie prüfen außerdem zwei getrennte 48-Byte-Bereiche, überlappende Deskriptoren, geänderte Callback-Rümpfe, veraltete Maschinenbefehle und IR, vom Rumpf getrennte Aufrufstellen sowie die genaue Projektionsreihenfolge. `MixedWidthFrameCopiesMeetEveryInitializedByte` und `FrameCoverageCannotHideMissingBytesOrPointerJoins` prüfen 8/16-Byte-Schreibzugriffe in beiden Zusammenführungsreihenfolgen, fehlende Bytes, Ungültigkeit nach beschreibbarer Ausleihe und nach teilweisem Überschreiben erhaltene Zeigeridentitäten.

Die Schleifenrelationstests prüfen feste und veränderliche Funktionstemporärwerte bei beliebig vielen Iterationen, getrennte Offsets, gepaarte Pläne, teilweise und nicht ausgerichtete Bereiche, beide Byteordnungen und temporäre Rangwerte. Native Komposition hält neuen Speicher auf Kandidatenseite. Fehlende Präfixe, nicht deklarierte oder undefinierte Bytes, falsche Projektionen, fehlende Zuweisungen, geändertes Programmverhalten und widersprüchliche Definitionen verhindern Zertifikate. Exakte Ausführungs-, Abfrage- und Beobachtungsbudgets bestehen; um eins kleinere scheitern. Inferenz erhöht kein späteres Beweisbudget. Lebensdauern bleiben an den Digest gebunden; eine gewöhnliche native ABI wird nicht nachgewiesen.

Native Schleifenrelationstests prüfen verzögerte bedingte Sammlung und nicht auditierte Ablehnungsgrenzen für beliebig viele Iterationen. Manuelle und abgeleitete Pläne prüfen den gesamten Eintritts- und Induktionsbereich erneut; erreichbare ungültige Zweige, geänderte native Aktualisierungen und erschöpfte Abfrage- oder Befehlsbudgets verweigern Zertifikate. Tests binden geänderte unerreichbare Grenzbytes, bewahren strikte Vorgaben und die Ablehnung fehlerhafter Pläne, prüfen beide Zeugen und kombinierte Optionen sowie die Ablehnung statischer APIs und überlappender Befehle. Semantikschema 17 bindet diese Zulassung; gewöhnliche native ABI und Quellkomposition bleiben getrennte Pflichten.

`ObjCSuperGetterSources` prüft CGRect-Getter mit vier Trägern, zehn zurückgewiesene Veröffentlichungsänderungen und gemischte Boolean/CGRect-Aufrufer desselben Maschinenkörpers. Das Ausführungsorakel prüft unter O0 und O2 exakte Ergebnisbits (einschließlich negativer Null, Unendlichkeit und NaN-Nutzlast), Empfänger-/Klassenidentität und das Laden des Selektors nach dem Metadatenaufruf. Apple ARM64 führt den ursprünglichen Compiler-Thunk und erzeugtes C aus; andere Hosts führen erzeugtes C mit ihrer nativen Record-ABI aus.

`LowIRLoopInference` prüft projizierte 8-, 24- und 32-Bit-Zähler mit beliebigen anfänglichen oberen Bits, beide Richtungen, Register, Frames, Funktionstemporäre und beide Byte-Reihenfolgen. Vollständige Selbstbeweise bestehen; geänderte Ergebnisse, Stillstand, schmale Überläufe und übersprungene Gleichheitsausstiege werden abgelehnt. Exakte Budgets für Operationen, Abfragen, Pfade, Rangkandidaten und Aufweitungen bestehen, eine Einheit weniger scheitert. Die Operations-, Abfrage- und Beobachtungsbudgets des abschließenden Beweises werden getrennt geprüft.

`ObjCCallHints.SDKRecordData*` prüft beide externen Datensätze, alle Double-Offsets, beide Darwin-Architekturen und Anbieter-Aliase sowie geänderte und schwache Importe, fehlende Bibliotheken, widersprüchliche Fixups, schreibbaren Speicher und unvollständige Bereiche. `python3 -m unittest scripts.tests.test_generate_darwin_record_data_declarations scripts.tests.test_generate_darwin_data_declarations` prüft Profilkonflikte, alternative Layouts, ungültige Größen/Ausrichtung, TLS und architekturspezifische Exporte. Der Katalog lässt sich mit `generate_darwin_record_data_declarations.py`, dem festgelegten SDK und libclang, Ausgabepfad und `--check` reproduzieren; diese Deklarationsprüfung beweist weder die Initialisierung indirekter nativer Ergebnisse noch eine Methodenrekonstruktion.

`LowIRLoopInference.ProjectedBounds*` prüft Gleichheitsausstiege mit beliebigen oberen Bits von Zähler und Grenze, 8/24/32-Bit-Felder, alle drei Speicherarten und beide Byte-Reihenfolgen. Die Beweise lehnen geänderte Grenzen, Stillstand, übersprungene Ausstiege, Überläufe und Änderungen beobachteter oberer Eingabebytes ab. Inferenz und Beweis behalten getrennte Budgets mit exakten und um eins verkürzten Grenzen.

`LowIRLoopInference.LateCounter*` prüft konstante Initialisierung, bei der 8/24/32-Bit-Felder erst nach der Verallgemeinerung sichtbar werden: Register, Frames, Funktionstemporäre, beide Byte-Reihenfolgen und beliebige obere Grenzbits. Gleichheitsausstiege bestehen den vollständigen Beweis; Stillstand, übersprungene Ausstiege, wechselnde Grenzen und Änderungen beobachteter oberer Bytes werden abgelehnt. Exakte und um eins verkürzte Budgets werden für Inferenz und Abschlussbeweis getrennt geprüft.

`LowIRLoopInference.ProjectedComparisonBits*` prüft gespeicherte Teilwortgleichheit am Schleifenkopf mit beliebigen oberen Grenzbits: 8/24/32 Bit, drei Speicherarten, beide Byte-Reihenfolgen und konstante oder mit oberen Bits ergänzte Initialisierung. Die Beweise lehnen geänderte Cachewerte, beobachtete obere Bytes, wechselnde Grenzen, Stillstand und übersprungene Ausstiege ab. Exakte und um eins verkürzte Budgets werden für Inferenz und Abschlussbeweis getrennt geprüft.

`LowIRLoopInference.OrderedComparisonBits*` prüft beide booleschen Kodierungen vorzeichenloser Ordnungsausstiege mit 8/24/32-Bit-Zählern, drei Speicherarten und beiden Byte-Reihenfolgen. Vollständige Beweise lehnen nicht terminierende Updates, geänderte Vergleiche oder Grenzen und Änderungen beobachteter oberer Bytes ab. Exakte und um eins verkürzte Budgets bleiben getrennt. Bei geänderter Kodierung wird vor dem Beweis der Digest der Originaloperationen neu gebunden.

`LowIRLoopInference.MutablePrefixBounds*` prüft abgeleitete 8/24/32-Bit-Grenzen mit veränderlichen oberen Bits, drei Zählerspeicherarten, beide Byte-Reihenfolgen und direkte/gespeicherte Ausstiege. Nichtterminierung, bewegte Gleichheitsgrenzen, beobachtete obere Bits und Vergleichsspeicher sowie exakte und um eins verkürzte Budgets werden geprüft. Eine bewegte vorzeichenlose Ordnungsgrenze kann beim Überlauf terminieren; ein eigener Regressionstest beweist dies vollständig.

`LowIRLoopInference.SharedTemplatesFitIndependentOperationBudgets` beweist verschachtelte Teilzähler mit höchstens 1.024 Inferenzoperationen und 640 unabhängigen Beweisoperationen. Identische reine Ausdrücke und unveränderliche Präfixlesezugriffe werden nur innerhalb einer Schnittpunktrekonstruktion geteilt; Operandenidentität, Ausgabebreiten und Positionsräume bleiben erhalten. Die Tests beobachten vollständige Zähler- und Grenzwörter und weisen veränderte Präfixberechnungen sowie um eine Operation zu kleine Budgets zurück. Bestehende Fälle für Register, Stackframes, temporäre Funktionswerte, Byte-Reihenfolgen und Nichtterminierung bleiben erforderlich.

`LowIRLoopInference.CompletedEntailments*` prüft die Sitzungsisolation zwischen terminierenden und nichtterminierenden Frame-Schleifen in beiden Byte-Reihenfolgen, Solver-/Knotenlimits und unabhängige Beweisbudgets. Regressionen mit veränderlichen Grenzen testen das exakte logische Anfragebudget und ein um eins zu kleines Budget trotz Cache-Treffern; native Beweise mit wiederholten Kontexten prüfen die domänenabhängige Wiederverwendung.

`LowIRLoopInference.IncrementalEntailmentsKeepRollingBudgets` prüft die Wiederverwendung des Encoders innerhalb eines Bedingungsbereichs. Ein Bereichswechsel verwirft den Encoder; bei ausgeschöpfter kumulierter Gatterkapazität erfolgt genau ein neuer Versuch mit einem frischen Encoder, der eine weitere Abfrage verbraucht. Vollständige Zähler- und Grenzwörter bleiben in beiden Bytefolgen beobachtet. Geprüft werden exakte und um eine Abfrage zu kleine Budgets, die Ablehnung bei Gatter-, Breiten- oder Suchgrenzen sowie unabhängige Budgets für den abschließenden Beweis.

`LowIRLoopInference.RebuiltCounterLanes*` prüft exakt erkannte projizierte Aktualisierungen, wenn andere Zählerbits aus Präfixwerten rekonstruiert oder unabhängig geändert werden. Register, Frame und Funktions-Temporärwerte werden in beiden Bytefolgen mit 1/3/4-Byte-Zählern und direkten oder zwischengespeicherten Ausstiegsbedingungen getestet; Zähler, Grenzen und Tags bleiben vollständig beobachtet. Fehlende beobachtete obere Tags, nicht terminierende Updates, fehlende Ausstiegsguards und um eine Einheit zu kleine Inferenz- oder Beweisbudgets werden weiterhin abgelehnt. Strukturelle Rekurrenzerkennung schlägt nur Erweiterungen und Ränge vor; vollständige Übergangs- und Abschlussbeweise bleiben erforderlich. Feste Operations- und Abfragebudgets prüfen außerdem symbolische projizierte Zähler, ohne zufällige Übereinstimmungen im konstanten Präfix in zusätzliche Relationen zu erweitern.

`LowIRLoopInference.ProjectedCounterCopies*` prüft verschachtelte Schleifen, die vor dem Inkrementieren eine Zählerspur über ein Wort mit eigenständigem Tag kopieren. Die 144 Fälle mit vollständigem Zustand umfassen Register, Frames, Funktionstemporäre, beide Byte-Reihenfolgen, Spuren mit 1/3/4 Bytes, direkte/gepufferte Austritte und Ganzwortkopien als Vergleich. Projizierte Gleichheiten müssen für gespeicherte Ankünfte und jeden eingehenden Übergang gelten; obere Bits bleiben unabhängig. Implikationsbeweise im Übergangsbereich können additive Rekurrenzen über verschiedene Parameter erkennen. Verlorene obere Tags, ungültige Updates, fehlende Schutzbedingungen und um eins zu kleine Inferenz- oder Abschlussbeweisbudgets werden weiterhin abgelehnt.

`LowIRTransferredCounterState.*` und `LowIRLoopInference.TransferredCountersKeepProofBoundaries` prüfen Zähler, deren Speicherort zwischen Schnittpunkten wechselt, während andere Schleifen einzelne Schnittpunkte umgehen können. Exakte symbolische Einheitsschritte liefern Vorschläge für Quellzählerbedingungen und einen Ersatzrang mit einem abweichenden Ort an einem Schnittpunkt; sämtliche Bedingungen und Ränge benötigen vollständige Übergangsbeweise. Die 192 Zustandsfälle umfassen Register, Frames, Funktionstemporäre, beide Byte-Reihenfolgen, 1/3/4/8-Byte-Zähler, unabhängige Tags und ortsfeste Kontrollen. Veränderte Ergebnisse oder Tags, falsche Rangzuordnungen, nicht terminierende Updates, fehlende Bedingungen und zu kleine Inferenz- oder Abschlussbeweisbudgets werden abgelehnt. Die Zähleranalyse nutzt das bestehende Symbolknotenbudget; optionale Graphselektoren dürfen ein Budget von null haben. Die Fälle decken das Herunterzählen bis null und das Hochzählen bis zu einer Eingabegrenze ab. Bei neu entdeckten Zählern werden Grenzen und Teilwortbedingungen erst nach dem Neuaufbau aller Schnittpunktvorlagen entfernt; die übrige Erweiterung und der unabhängige Beweis laufen weiter. Die vollständige Zustandsmatrix besteht aus zwölf benannten Gruppen für Speicherort, Richtung und Übertragung mit jeweils sechzehn Kombinationen; CTest meldet und begrenzt jede Gruppe separat.

`LowIRLoopRefinement.GuardedCuts*` und `BinaryLowIRLoopRefinement.GuardedCuts*` prüfen wiederholte PCs, Register/Frame/Systemflags, beide Byte-Reihenfolgen, nicht ausgewählte endliche und zyklische Pfade, Überlappung, falsche Seiten, Präfixgeneralisierung, Zeugen undefinierter Werte, Metadaten, Digests und Budgets. Unabhängige native Tests beweisen beide R10-Kontexte am selben Schleifen-PC und den Vorrang ungeprüfter Grenzen. Gewöhnliche ABI-Zertifizierung bleibt getrennt.

`BinaryLowIRLoopInference.NativeSelectors*` prüft zwei Registerkontexte, allein durch den Frame trennbare Kontexte, Konjunktionen für drei Domänen, untrennbare Vorlagen, veränderte Ursprünge und native Schleifen sowie unabhängige exakte und um eins verkürzte Budgets. Iterationszahlen sind beliebig; Eingangskonstanten werden nicht ergänzt.

`NativeSelectorsGeneralize*` prüft automatische Rekonstruktion und vollständige native Beweise für wechselnde Register- und Framephasen, einschließlich Bytemasken mit symbolischen oberen Bits. `NativeSelectorState*` prüft falsche Ränge/Schleifenkörper, Änderungen außerhalb der Maske, ungültige Zuweisungen sowie exakte und um eins reduzierte Arbeits-/Metadatenbudgets. Gültige explizite Pläne verbinden den tatsächlichen Konstruktor mit vollständigen nativen Prüfungen vorher und nachher: überlappende und disjunkte Schnittpunkte, erhaltene Originalpräfix-Herkunft, berechnete Rückfallscans und abgelehnte temporäre Überläufe. Diese Prüfungen sind von automatischer Inferenz zu unterscheiden.

`DarwinIndirectRecordCalls` prüft den aktuellen MakeScale-Vertrag und seine 22 Import-/ABI-Mutationen und verwendet anschließend ein vollständiges privates Ergebnis von 48 Bytes mit dem gemeinsamen Beweis für Wertkopien. Nicht ausgerichtete, verschobene, überlappende oder außerhalb des Rahmens liegende Ergebnisbereiche werden abgelehnt. Auch das Entfernen des sicheren Schreibeffekts führt trotz vollständiger Rückgabe-ABI zur Ablehnung.

`SourceFrameAnalysis.IncomingResultAddressNeedsCompleteEntryIdentity` lehnt zehn Eingangs-, Carrier- oder Schreibmutationen und eine fehlende Eingangs-ABI ab. `NativeSourceHints.IndirectResultTailCallRetainsExplicitOutputAddress` liftet einen direkten Tail-Aufruf erneut und prüft den expliziten Ausgabeparameter, sechs Writes und das Veröffentlichungsgate.

`NativeSourceHints.FourDoubleCallerDemandNeedsEveryUnchangedCarrier` prüft vier untere Lanes, unabhängige obere Writes und neun Deklarations-/Kontrollmutationen. `FourDoubleReturnRequiresEveryComputedLowLane` lehnt zwölf Fälle unvollständiger Ergebnisse oder veralteter Verträge ab. `DarwinNativeRecordReturns.FourComputedDoublesExecuteAtO0AndO2` vergleicht alle 32 Ergebnisbytes bei 2048 Fällen je Optimierungsstufe mit einem unabhängigen arithmetischen Orakel.

`DarwinIndirectRecordCalls.AffineInvertSnapshotsItsCompleteAliasedInput` führt bei O0/O2 jeweils 2560 Fälle mit identischen, überlappenden und getrennten Ein-/Ausgabebereichen aus. Geprüft werden Eingabebits, ein Aufruf, alle 48 Ergebnisbytes und der gesamte geschützte Speicher. Dies ist ein Orakel für physische Kopien und Snapshots, keine Ausführung der Originalmaschine oder des nativen SDK. Der aktuelle Matrix-/Affine-Vertragstest behält 22 abgelehnte Mutationen je Vertrag.

`DarwinIndirectRecordCalls.AffineTranslatePreservesScalarBitsAndSnapshotsAliasedInput` führt je 2560 Fälle bei O0 und O2 aus. Geprüft werden beide Skalar-Bitmuster, alle sechs Eingabefelder, ein Aufruf, sämtliche Ausgabebytes und der geschützte Speicher bei gleichen, überlappenden und getrennten Positionen. Vier Skalar-ABI-Mutationen und die gemeinsamen 22 Import-/ABI-Mutationen werden abgelehnt. Der bitweise Stub prüft physische Argumente und die Eingabekopie; er ist kein mathematisches Translationsorakel und führt keine ursprünglichen Maschinenbefehle aus.

`NativeFloatingReturnProof.HFAResultFieldsNeedTheExactCompleteDefinedCall` deckt neun Feldauswahlen und siebzehn abgelehnte Aufruf-, Träger-, Breiten-, Offset- oder SSA-Mutationen ab. `HFAFieldExtractionNeedsADominatingCall` lehnt einen Produzenten auf einem Geschwisterpfad ab. `NativeSourceHints.HFAFieldTypeRequiresCurrentCallAndFrameProofForPublication` liftet einen ARM64-Aufrufer mit fünf Befehlen, liftet das abgeleitete skalare Ergebnis erneut und prüft die Source-Grenze; ein falscher Anbieter oder fehlende LR/SP-Wiederherstellung wird abgelehnt. Dies prüft Quelltyp und Projektion, nicht die Ausführung des ursprünglichen Maschinenkörpers.

Explizite CPU0-Präemption, virtuelle Zeit und Grenzen beschreibt [Treiber-Scheduling](driver-scheduling.md).

`SwiftOnceSources.FoldedObjCGetterTailsExecuteOnceAndRetainsAtO0AndO2` faltet echte ARM64/x64-Rückgabetails mit kombinierten oder getrennten Retain-Aufrufen und separaten oder eingebetteten Prädikaten und führt das erzeugte C mit Laufzeit-Stubs bei O0/O2 aus. Geprüft werden Ergebnisbits, einmalige Initialisierung, Aufrufreihenfolge und ein geänderter Cachewert. `FoldedObjCGetterTailsRevalidateCurrentStorageAndCalls` lehnt trotz gespeichertem Plan Mutationen an Breite, Reihenfolge, Intrinsics, Speicher, Ergebnis, Aufrufen und aktuellen Imports ab. Dies sind kontrollierte Source-/Stub-Prüfungen, keine Ausführung der ursprünglichen WMF-Maschine oder nativen Swift-Laufzeit.

`SourceFrameAnalysis.CompleteOutput*` prüft vollständige und kürzere Präfixe, Zusammenführungen aller Rückkehrpfade, SDK-Tail-Schreibzugriffe, fehlende Bytes, Zeigerflucht und Trägergrenzen. Aufruferfälle weisen fehlende oder kurze Zertifikate, Fehlausrichtung, Frame-Grenzen und Überlappung gespeicherter Register, Aliase, spätere Schreibinvalidierung und lebende opake Werte zurück. `NativeSourceHints.CompleteNativeOutput*` spielt assemblierte ARM64-Erzeuger und Verbraucher erneut ab, verlangt den vollständigen SDK-Eingabebereich und verweigert veraltete Code-, CFG-, ABI-, Audit-, Provider- und Aufrufbelege. Dies prüft Byteinitialisierung und Quellcodefreigaben, ohne den ursprünglichen Maschinenkörper auszuführen oder eine native logische Rückgabe zu zertifizieren.

`MedCallingConvValueFlow.FPInputsFollowOnlyAuthenticatedCallPrefixes` / `NativeSourceHints.PreservedFPPrefixesRetainAllEntryInputsAfterSDKCalls`: Die Präfixregressionen prüfen drei gültige Lesezugriffe und neunzehn abgelehnte Träger-, Präfix-, Aufrufzuordnungs- und unbenutzte Wertfälle. Ein assemblierter ARM64-Aufrufer erhält alle vier eingehenden double-Lanes über einen SDK-Aufruf; erneutes Lifting prüft vollständige Parameter-ABI und Quellcodefreigabe. Die unveränderte akzeptierte WMF-Methode `0x36350` besteht zudem jeweils 2048 Fälle bei O0 und O2 gegen einen unabhängigen nativen CoreGraphics-Ausdruck für Spiegelung, Translation, Normalisierung, Verkettung und Anwendung: alle 32 Ergebnisbytes, Empfänger/Selektor und Eingabeschutz, einschließlich Null-, negativer, unendlicher und NaN-Abmessungen. Der ursprüngliche WMF-Maschinenkörper wird nicht ausgeführt.

`SourceABI.SwiftEntryCapturesAndReturnsTheErrorRegisterOnEveryPath` prüft die ARM64/x64-Eingangserfassung, beide Rückkehrpfade und die Ablehnung fehlender oder veränderter Transportnachweise. `NativeSwiftCallsKeepBothResultsAndPostCallErrorBranches` erhält Aufrufergebnis, aktualisiertes Fehlerregister und nachfolgende Bedingung, einschließlich privater Namenskollisionen und beider Ausgabereihenfolgen. Der erzeugte C-Code läuft bei O0/O2 auf dem nativen Darwin-Ziel gegen unabhängige Erfolgs-/Fehlerorakel. `SwiftFunctionSymbols.RegularExpressionInitializerRetainsContextAndError` lehnt Aliase, veränderte vollständige Symbole, Nicht-Code-Eingänge und nicht unterstützte Abbildformate ab. Diese Tests führen den ursprünglichen WMF-Konstruktor nicht aus und beweisen keine vollständige Wiederherstellung der aufrufenden Methoden.

`NativeSourceHints.SwiftErrorDeclaration*` liftet assemblierte ARM64/x64-Einstiege erneut, nachdem eine beobachtete skalare ABI durch die Compilerdeklaration ersetzt wurde. Fehlende oder unvollständige aktuelle Prüfungen verhindern den Ersatz; explizite Quellverträge in Optionen, MedIR oder HighIR behalten ihre Priorität. Die Einstiegstests weisen außerdem eine fehlende MedIR-Markierung für die Fehlerausgabe oder eine falsche Operandenbreite vor der HighIR-Konvertierung zurück.

Die Eingangserfassung bleibt eine Liveness-Wurzel, wenn jeder Pfad das Fehlerregister überschreibt, auch bei der abschließenden Quellbereinigung nach der once-Bindung. `NativeSourceHints.SwiftErrorCallsRequireCurrentDirectNativeProjection` weist fehlende, nullwertige, geänderte oder unbewiesene Zielroutinen sowie geänderte Ziele, indirekte Aufrufe, unvollständige Ergebnisse, fehlende Operanden und unvereinbare Effekte zurück.


`SwiftFunctionSymbols.RepeatedDeclarationsKeepEveryRecordField` prüft identische doppelte Datensätze und lehnt geänderte Namen, Größen, Grenzherkunft oder Ursprünge ab. `NativeSourceHints.SwiftErrorCallResults*` prüft die automatische ARM64/x64-Aufruferanalyse und lehnt fehlende aktuelle Ziele, geänderte Maschinenoperationen, veraltete Audits, unvollständige ABIs sowie fehlende, schmalere oder unabhängige Ergebnisextraktionen ab. Die Ausführung des Eingangscodes deckt auch widersprüchliche optionale Debugdeklarationen ab und behält die gebundene Konvention sowie Fehler- und Kontextrollen bei.

Die Swift-Witness-Generatoren prüfen `CurrentValueSubject: Publisher` und `Range<Bound: Comparable>: RangeExpression` auf ARM64/x86-64 für macOS und Mac Catalyst. `scripts.tests.test_generate_swift_witness_contracts` lehnt geänderte generische Eingaben, Metadatenantworttypen oder Mitglieder, Prototypen, Exportanbieter und unvollständigen Datenfluss ab. `ObjCSourceBindings.SwiftWitnessUndefRequiresExactDescriptorContract` und `SwiftWitnessUndefRejectsUnprovedInputAndABI` prüfen beide Deskriptoren auf beiden Architekturen mit jeweils 33 Mutationen für Runtime-/Importidentität, schwachen oder widersprüchlichen Speicher, ABI und Effekte. Die Kataloge gewähren weder Frame-Layout noch einen Ausleihvertrag.

`scripts.tests.test_generate_swift_data_declarations` prüft die vollständige `String.Index`-Deskriptorabfrage und lehnt Änderungen an symbolischen Zellen, Rezeptbytes oder -längen, Metadaten-/Cache-Fluss und Runtime-ABI sowie doppelte oder fehlende Definitionen ab. `ObjCSourceBindings.SwiftRangeIndexDescriptorKeepsItsCompleteRecipe` prüft den nicht führenden Deskriptor bei Offset 3 auf beiden Architekturen. `SwiftRangeIndexDescriptorRejectsStaleIdentityAndRecipe` lehnt jeweils 20 Mutationen ab und validiert zuvor veröffentlichte Adresshinweise und Helper-Ausgabe erneut.

`ObjCSourceBindings.PrivateFramePointerTailRequiresExactStoreOnEveryPath` prüft zyklische Objektvariablen nach PHI-Zusammenführung sowie einen privaten Frame-Wert im selben Zyklus. Der Objektzyklus erhält einen exakten Pointer-Spill; ein Frame-Wert, teilweise Überschreibungen und unbekannte Frame-Escapes verwerfen ihn.

`ObjCCallHints.FoundationGenericNSRangeKeepsSixPointersAndTwoWords` prüft beide Architekturen und Anbieter sowie alle Argument- und Ergebnisregister und verwirft schwache Imports, Addenden, fremde Anbieter, veraltete Symbole und erfundene Borrowing-Effekte.

Der String-Witness-Leser in `scripts.tests.test_generate_swift_witness_contracts` prüft vollständigen Cache- und Abfragefluss, verwirft 28 Speicher-, ABI- und Flussänderungen sowie sieben mehrdeutige Deklarationen und erzwingt das Eingabebudget. Die Bindungstests decken vier Deskriptoren auf beiden Architekturen mit je 33 Änderungen ab. Die Identität gewährt kein Frame-Layout oder Borrowing.

`PreparedFiniteKeys.*` prüft Kontextzerstörung und Umbenennung, Projektionsreihenfolge und Grenzen, explizite Ungültigkeit nach Verschiebung, fehlerhafte und unvollständige Ergebnisse, leere und nicht eindeutige Wertebereiche sowie exakte Kapazitätsgrenzen. Bestehende Cache- und Frame-Regressionen decken diesen Pfad ebenfalls ab.

`SourceABI.SwiftPointTransformKeepsTwoFloatingInputsAndResults` verwirft veränderte Register, Layouts, Kontextrollen und indirekte Ergebnisse auf beiden Architekturen. `SourceABI.SwiftPointForwardingPreservesBothIEEECarriers` führt den weitergeleiteten Quelltext mit -O0/-O2 aus und prüft vorzeichenbehaftete Nullen, Subnormale, Unendlichkeiten und NaN-Nutzlasten beider Felder. Deklarationstests akzeptieren Compilerformen und benannte Argumente und verwerfen geänderte Signaturen und mehrdeutige Identitäten.

Die MainActor-Fixture prüft den vollständigen Ablauf für feste Metadaten und statische Tabellen. Änderungen an Speicher, ABI, Metadatenextraktion, Tabellenidentität, zusätzliche Effekte, fehlende oder doppelte Deklarationen und überschrittene Eingabebudgets werden abgelehnt. Beide Kataloge verlangen alle drei SDK-Exporte für jedes Ziel. Die Witness-Bindungstests decken alle vier Deskriptoren auf arm64/x64 mit jeweils 33 Mutationen pro Architektur ab. Der Laufzeittest auf dem Host vergleicht die öffentliche Tabelle für fünf Bitmuster des Instanziierungsarguments.

`BitVectorEncodingClone.WatchMigrationAndGrowthOutliveTheSource` vergrößert Tabellen und gemeinsame Watch-Listen nach Kopie und Zerstörung der Quelle, verändert eine Geschwisterkopie unabhängig, unterbricht nach einem Watch-Besuch und prüft fortgesetzte vollständige Modelle gegen Originalklauseln und unabhängige boolesche Beziehungen.

`ObjCCallHints.SwiftPublishedAccessorsKeepOpaqueValueAndAllKeyPaths` prüft beide Zugriffe auf ARM64/x86-64 mit beiden kanonischen Combine-Anbietern und verwirft pro Kombination neun ABI- und acht Importidentitätsmutationen. Die unabhängige SDK-Prüfung führt generiertes C beider Quellarchitekturkonfigurationen mit O0/O2 auf ARM64 aus und vergleicht bei 128 Aufrufen alle 24 Nutzdatenbytes, Eingabe-/Ausgabewächter und zwei Owner-Identitäten. Acht Cross-Compile-Konfigurationen prüfen beide Architekturen auf macOS/Mac Catalyst. Das Laufzeitorakel bewahrt die konsumierten Setter-Referenzen und gewährt dem Produkt keine Abkürzung bei Borrowing oder Besitz.

`ObjCCallHints.SwiftMainActorSharedKeepsObjectAndMetatypeContext` prüft die vollständigen Ergebnis- und swiftself-Träger auf ARM64/x86-64 und weist je Architektur zehn ABI- sowie zehn Importidentitätsmutationen zurück. Die unabhängige SDK-Prüfung führt unveränderten generierten C-Code für beide Quellarchitekturen mit O0/O2 auf einem ARM64-Host aus: 128 Aufrufe erhalten Singleton- und Metatypidentität bei ausgeglichenem Referenzbesitz. Acht Cross-Compile-Konfigurationen decken macOS und Mac Catalyst auf beiden Architekturen ab; native x86-64-Ausführung bleibt gesonderte Abdeckung.

`BitVectorEncodingClone.RootQueuePreservesDecisionsAcrossGrowthAndBudgets` prüft gemischte Wurzel- und unentschiedene Variablen, Wurzeln ohne Entscheidungsstatus, das Kopieren einer Kopie, zerstörte Quellen, späteres Variablenwachstum, beide Standardpolaritäten, begrenzte Unterbrechung und Fortsetzung, Konflikte und Neustarts. Vollständige Modelle und alle Suchzähler müssen mit einer frischen Kodierung übereinstimmen.

`ContextFiniteProofs.*` prüft Kontext- und Besitzerisolation, Besitzerersatz, Token-Verschiebung, genaue Prädikate und Projektionsreihenfolge, Knotenwachstum, vollständige und unvollständige Ergebnisse, Speichergrenzen und LRU-Verdrängung. Frame-Tests verlangen die letzte Eindeutigkeitsabfrage vor dem Speichern und erhalten die Knotengrenze bei Cache-Treffern.

`LinuxPriorityTests.cpp` prüft expliziten Task-Zustand, Threadisolation, fehlende Beobachtungen, fehlerhaftes JSON, Profilzulassung und die Auswirkungen einer Ablehnung. Unabhängige rohe x64-/AArch64-Aufrufer prüfen bei O0/O2 die Nice-Begrenzung, die Verengung von Systemaufrufargumenten auf 32 Bit, CAP_SYS_NICE-/RLIMIT_NICE-Berechtigungsgrenzen und die Kernelkodierung von getpriority. In `NeverDLinuxProcessTests` sind `LinuxPriority.*` und `Backends/LinuxPriorityProcess.*` auszuführen; bei gemeinsamen Kernel-/JSON-Änderungen folgen die vollständigen Linux-Prozess-, nativen Android- und öffentlichen Prozesstests. Fehlende optionale native Transporte bleiben explizite Übersprünge.

`LinuxKernelAvailability.*` prüft explizite Abwesenheitseingaben und Profilzulassung. `Backends/LinuxKernelProcess.*` prüft mit unabhängigen rohen x64-/AArch64-Aufrufern bei O0/O2 ENOSYS vor der Argumentprüfung sowie die weiterhin geltende Ablehnung nicht angegebener oder anderer Aufrufe. Die Android-syscall-Fixture vergleicht rohe SVC mit Bionics `syscall` und unterscheidet rohe Rückgaben von errno-Effekten. Bei Verfügbarkeitsänderungen sind diese fokussierten Tests, sämtliche Linux-Prozess- und öffentlichen Prozesstests sowie Android-syscall-, native Einstiegs- und Signaltests auszuführen.

`CompletedQueryCache.*` prüft vollständige Byte-Domänen, alle gepackten Plätze, Wachstum, Kontext- und Besitzerisolation, ungültige und unvollständige Eingaben sowie exakte Speichergrenzen. Native Verzweigungstests behalten feste logische Abfragekosten und exakte beziehungsweise um eins zu kleine Budgets, auch wenn vollständige Antworten Backend-Arbeit vermeiden.


`BinaryLowIRRefinement.NativeTargetDomainsKeepIndependentProjections` / `FrameOffsets.FrameAndJointTargetProjectionsKeepIndependentSearches` prüfen wiederholte native Zielketten mit Zweigwechseln und symbolischen Frame-Schreibzugriffen, feste logische Kosten, exakte und um eine Anfrage zu kleine Budgets, ungültige Zielgrenzen, erschöpfte Gate-Budgets und falsche Endbeobachtungen. Abwechselnde Frame- und korrelierte Zielprojektionen bewahren auch beim Prädikatwechsel vollständige Tupel, Beobachterreihenfolge und die Ablehnung unvollständiger Ergebnisse.
`FrameOffsets.Cached*` prüft Adressverschiebungen mit beliebigen hohen Bits des Eingangsbezugs, vorzeichenlosen Überlauf, veränderte Summenformen, beide Cache-Modi, getrennte Prädikate, Nullkapazität, Ablehnungen wegen Anfrage-/Knotenbudgets sowie leere und nicht eindeutige Wertebereiche. Erste Anfragen behalten vollständige Solver-Beweise; spätere Verschiebungen können einen abgeschlossenen Beweis auch ohne verbleibendes Anfragebudget nutzen.

## Verträge veröffentlichter Android-GKI-Kernel

`AndroidTestExecution.def` gibt `FiniteRegistryRejectsBeforeSuccessAndCanBeReused` einen gesamten CTest-Zeitrahmen von 120 Sekunden und `RUN_SERIAL`. Beide Workloads behalten jeweils ihre endliche Laufzeitgrenze von 30 Sekunden; die Serialisierung verhindert Konkurrenz zwischen Kapazitätstests. Alle sechs O0/O2- und Relokationsvarianten verwenden diese Regel.

`LinuxPIDFD.*` prüft vor dem Laden Zweige, ungültige Enums, GKI/Abwesenheitskonflikte und fehlerhafte, übermäßige oder widersprüchliche Task-Kataloge. `Backends/LinuxPIDFDProcess.*` nutzt unabhängige O0/O2-x64/AArch64-Aufrufer für acht Zweige: Flags, gemeinsame Vergabe, Grenzen, close/Wiederverwendung, geschlossene Suche, Nichtführerfehler und Skalar/Vektorreihenfolge. Fehlende Ziele und Nichtführer werden vor FD-Erschöpfung geprüft; Threadflags und leerer Katalog mit implizitem self sind enthalten. Vektorfälle vergleichen frühe negative Längen mit später unzugänglichen Metadaten und Originalspannen jenseits der Benutzergrenze, deren gekappte Spanne passt, für pidfds und beide Ausgabestreams. Android-Fälle `ReleasedGKIProcessDescriptorsShareRawAndBionicOwnership`, `ReleasedGKIVectorImportRetainsRawAndBionicErrors` und `ReleasedGKICatalogueRetainsRawAndBionicLookupErrors` erhalten rohe Fehler, Bionic-errno, Katalog und Erschöpfung in sechs Relokationsprofilen. Danach vollständige Linux-Prozess-, Android-native- und öffentliche Prozesssuites ausführen. Diese Tests betreiben das Modell, nicht acht GKI-Kernel. Siehe [veröffentlichte GKI-Verträge](../android-gki-kernels.md).

`ProcessCPUClocksRetainIdentityAndIdleSeparation`, `ProcessCPUClocksKeepMissingObservationBoundaries`, `LinuxClock.ProcessCPUObservationsShareAliasesAndRemainFixedWhileIdle`: Prüft alle acht Pins: rohe/Bionic-Identität, PROF/VIRT/SCHED, untere 32 Bits, Zielprüfung vor Zeigerfehlern, fehlende Werte, Aliase, nichtnegative CPU-Sekunden und Trennung von CPU und Wanduhr-Leerlauf. `AndroidTimeTests.cpp` prüft Ausgaben und Wächter, der kooperative Syscall die aktuelle Nichtführer-TID.

`ZeroTimeoutPollRetainsReadinessAndOrderedCopies` prüft O0/O2-Rohaufrufe für acht GKI-Versionen: lebende, negative und geschlossene Deskriptoren, Duplikatzählung, Argumentverengung, Zeitlimit-/Maskenreihenfolge, schreibgeschützte Null-timespecs, vollständigen Metadatenimport vor Bereitschaft sowie frühere `revents` bei späteren Schreibfehlern. `ZeroTimeoutPollKeepsUnobservedBoundaries` erhält unbekannte Kernel, Grenzen, Masken, Warte- und Bereitschaftszustände. Androids `ReleasedGKIZeroTimeoutPollSharesRawAndBionicResults` prüft gemeinsame Tabelle und errno-Besitz in sechs Verpackungsprofilen.

## Begrenzte Verzeichnisattribute in Gruppen

bulk-attributes prüft ganze Gruppen, Namen/Typen als Menge, Schutz unbenutzter Bytes, low32 FD, bitmap-Wörter, native Fehler, dup, unabhängige open, EOF und Null-rewind. Literal-/Unbekannt-Modi sind nur virtuell. Modelle decken vollständiges stat, Invalidierung, NFD/255-Byte-Namen, Ein-/Ausgabealias, Transport-/Budgetfehler, Verschieben/SWAP/Entfernen/Wiederverwenden und explizite Rechte ab. Pflichtinventar:65 Fälle pro Plattform,195 ARM64 und130 Intel. Lokal wurde nur passendes ARM64 HVF geprüft. native5s, guest/Python5,000,000us/quantum1024 und public10s bleiben gleich.

## Begrenzte Darwin-Hardlinks

link folgt dem letzten symbolischen Ziel; linkat mit flags=0 wählt das Linkobjekt, AT_SYMLINK_FOLLOW das Ziel. Nur low32 0/0x40 sind zulässig, andere niedrige Bits liefern EINVAL vor dem Import. Quellensuche und Verzeichnis-EPERM gehen dem Zielimport voraus; vorhandene Ziele liefern EEXIST. Das Ziel benötigt Änderungsrecht und dieselbe ausdrücklich festgelegte Mount-Domäne. Anfängliche Identitätsaliasse sowie bekannte Geräte-, Modus- und Flag-Konflikte bleiben unzulässig.

Ein Alias kostet nur Eintrag und Pfad/NUL, keinen neuen inode. Bytes, Attributrechte, Metadatengültigkeit und Mapping-Leases gehören dem gemeinsamen Objekt. Explizite Richtlinien aktualisieren Linkzahl und ctime; ohne sie bleibt vollständiges stat unbekannt. Attributänderungen invalidieren stat, Inhaltsänderungen Attributbeobachtungen. Gehaltene Beschreibungen und letzte Mappings behalten Namenskosten; Ersetzung verrechnet nur sofort freigebbare Kosten. Teilbäume wählen genaue Identitäten und Eltern; externe Aliasnamen bleiben stehen, relative symbolische Ziele verwenden den gewählten Eintragselternteil.

F_GETPATH/ATTR_CMN_NAME bleiben nach mehreren Namen auch bei einem oder keinem Namen unzulässig; kein allgemeines APFS-Cachemodell wird behauptet. bulk NAME stammt vom tatsächlichen Eintrag; gewöhnliches rename/SWAP desselben Objekts behält beide Namen. EXCL-Großschreibung, O_SYMLINK, Intel HVF, physisches iOS, ACL, kohärente Mappings/EOF-Signale, dyld, Mach IPC, Threads und vollständige Frameworks bleiben Lücken. Frühere Ausschlüsse werden nur innerhalb dieses Vertrags erweitert.

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
65 mandatory workloads per platform / ARM64 195 / Intel 130
```

bulk-attributes prüft ganze Gruppen, Namen/Typen als Menge, Schutz unbenutzter Bytes, low32 FD, bitmap-Wörter, native Fehler, dup, unabhängige open, EOF und Null-rewind. Literal-/Unbekannt-Modi sind nur virtuell. Modelle decken vollständiges stat, Invalidierung, NFD/255-Byte-Namen, Ein-/Ausgabealias, Transport-/Budgetfehler, Verschieben/SWAP/Entfernen/Wiederverwenden und explizite Rechte ab. Pflichtinventar:63 Fälle pro Plattform,189 ARM64 und126 Intel. Lokal wurde nur passendes ARM64 HVF geprüft. native5s, guest/Python5,000,000us/quantum1024 und public10s bleiben gleich.

`MaterializedRuntimePreservesOwnedObjectsOnNativeWindows` prüft ursprüngliche Modellausführung, Wiederherstellung, Sektionsrechte und native Windows-Ausführung beider Abbilder: Heap-Neuzuweisung/Freigabe, kodierte innere Zeiger, FLS-Neuaktivierung, rekursive Sperren, LastError und virtuelle Reservierung, Zusicherung und Schutz. `MaterializationRequiresKnownSupportedState` lehnt fehlende Versionen und dynamisches TLS ab. `RuntimeRestorationHasTheSameCAPIAndCLIContract` vergleicht genaue Bytes und Berichte. Linux-Konstruktionsprüfungen und Wine-Beobachtungen ersetzen keine nativen Windows-Lebensdauernachweise.

`NeverDUnpackDriverTests` prüft Einstieg, Kernelimports, verbleibende Ressourcen, ABI, Exporte, Ablaufplanung, Anforderungen und Entladen, C API/CLI sowie PE-Prüfsummen. Pflichtfälle für KVM/WHP und ImageHlp unter Windows belegen keinen nativen Kernelladevorgang. [UNPACK](unpack.md).

`NeverDKernelSnapshotTests` prüft diagnostische Objektabbilder nach terminalen Speicherfehlern und nicht unterstützten Befehlen. Bestätigte Geräte- und Dispatch-Zustände bleiben lesbar, ohne den ersten Fehler zu löschen oder weitere CPU-Zugriffe zu erlauben. Beschädigte Eigentümerbeziehungen, zyklische Gerätelisten, ungültige Treibererweiterungen und teilweise initialisierte Dispatch-Zeiger werden weiterhin abgelehnt. Die native KVM/WHP-Abnahme verlangt die Fälle des jeweiligen Backends.

`KernelUnpackStateTests.cpp` ergänzt `NeverDKernelSnapshotTests` um neun portable Fälle für Pool-Allokationsverträge, unabhängige Inhaltskopien, freigegebene Lebenszeiten, fehlenden Speicher und verbleibende Abhängigkeiten. Fälle mit terminalen Fehlern erfassen auch lebende Pools. `UnpackDriver.StoppedViewOwnsLivePoolContentsAndContracts` prüft den öffentlichen Beobachterzustand nach dem Ende der Sitzung. Diese Fälle sind im nativen Treiber-Gate verpflichtend und zertifizieren keine Wiederherstellung im nativen Kernel.

Die Schreibbeobachter-Regressionen prüfen Sichtbarkeit vor der nächsten Anweisung, genaue Anweisungszählung ohne Schreibprotokoll und die Weitergabe von Beobachterfehlern. Auf passenden KVM/WHP-Backends sind legacy und checked verpflichtend.

`UnpackDriver.CommittedWriteWatchesSuspendBeforeTheNextInstruction`; `UnpackDriver.CommittedWriteObserverFailuresRemainAPIErrors`.

## Prüfungen opaker Zustände

`X86PreservedState.*` prüft frische skalare Formen, genaue Aliase, striktes Zurücksetzen und Ablehnung veralteter Bytes/Folgen/Versionen. `OriginalBinaryUndefinedIndependence.*Opaque*` deckt Zweige, interne Aufrufe, vollständige indirekte Ziele, genaue Profile und unabhängig dekodierte exakte/um eins zu kleine Metadatenbudgets ab. `BinaryLowIR*.*Opaque*` prüft Zeugen gegenüber beliebigen undefinierten Entscheidungen, mehrere induktive Quellen, späte Rang-/Budgetfehler, skalare Erhaltung vom echten Eintritt und spätere Quellbytes mit identischem LowIR, aber verändertem Ausführungsdigest. `NativeUndefinedIndependence.*Opaque*` und `NativeStackControl.*FreshMemoryCall*` prüfen Gruppeninnere, Grenzen vor Schnittpunkten, veraltete Belege und Zielauswertung vor Stackänderung. Betroffene Verbraucher einschließlich `NeverDInterpreterLLVMRefinementTests` neu bauen; Sanitizer und kompilierte Fehlerinjektionen getrennt von normalen Tests berichten.

`scripts/check_windows_nonlocal_finally.py` kompiliert und startet originale MSVC-x86/x64-Programme nativ mit O0/O2 und ein- bzw. ausgeschaltetem GS. Sechs Fälle unterscheiden normale finally-Aufrufe, Aufrufe beim Stack-Unwinding, bedingte Sprünge zur Elternfunktion und return-Anweisungen, die das Unwinding beenden. Wiederholungen prüfen lokale Schutzbereiche und die x86-Registrierungskette; eine absichtlich geänderte Bereinigungsfolge muss an den ursprünglichen Erwartungen scheitern. Der Windows-EH-Fixture-Job speichert Quell- und Image-Hashes, Compilerausgaben, Linker-Maps und Laufzeitbeobachtungen. In der passenden MSVC-Umgebung `--arch x86` oder `--arch x64` und `--output` angeben. Dies liefert eine Referenz des Original-Runtimes, keinen Nachweis, dass NeverD diese nichtlokalen Übergänge unterstützt. Vier neue Fälle lösen nach der Elternfortsetzung sofort eine zweite Ausnahme aus; zwei prüfen einen geschützten Zähler des Aufrufers. Die x86-Kette wird nach jedem Aufruf und am Fortsetzungspunkt geprüft. Schema 2 ist erforderlich; alte reine Abschlussprüfungen reichen nicht.

`scripts/check_windows_native_seh_resume.py` prüft separat ein experimentelles Windows-x64-LLVM/CRT-Protokoll bei O0/O2. `_local_unwind` erhält den physischen SP und die adressierbare Elternfortsetzung, getrennt vom Quellrahmen. MASM prüft acht erhaltene GPRs, XMM6–XMM15, SP und Schutzwerte sowie ausgeführte bzw. übersprungene Aufrufsuffixe und eine zweite Ausnahme. Werkzeugversionen und Hashes werden gespeichert. Erfolg erlaubt keine NeverD-Rekonstruktion und löst weder PE32 noch entkommene Speicheraliasse, beliebige Verschachtelung oder beide C-Ausgaben. Mit `--output` in der nativen x64-MSVC-Umgebung ausführen. Die Workflow-Eingabe `windows_eh_protocol_only` führt diese nativen Experimente ohne NeverD-Neubau aus; sie ersetzt keine Engine-Regressionen.

`scripts/check_windows_native_seh_bridge.py` Die feste x64-Assembler-Brücke besitzt Fortsetzungslabels und Unwind und ruft optimiertes LLVM über normale ABI-Einstiege auf. O0/O2 prüfen je acht Pfade in 256 Aufrufen, rohe Bytes, tatsächliche Codes, Zähler und dauerhafte Fehler sowie sechs beschädigte Kontrollen. Quellhashes, finales unwind/load-config und tatsächliche CRT-Anbieter werden gespeichert. Speicherautorität, beliebige Verschachtelung, Nebenläufigkeit, EHCONT/CET und Produktionsrekonstruktion bleiben unbewiesen.

`RegistrationTryLevel.EH4Publication*` und `EH4CookieStoresMustOwnDistinctPrivateBytes` prüfen direkte PE32-EH4-Frames mit ADD/SUB-Allokation, gesicherten Registern, per PUSH abgelegten Cookies und disp8/disp32-Speicherungen. Geänderte Quellen oder Offsets, überlappende Cookies und Schreibzugriffe auf gesicherte Register oder Laufzeitverwaltungsdaten müssen abgelehnt werden; die gemeinsame LowIR prüft Werte und Lebensdauer unabhängig.

`PipelineOutcome.SourceEHRefusalsAgreeAcrossSingleAndShardedEmission` prüft die Quellcodeprojektion mit und ohne Optimierung für PE32, ELF und Mach-O auf x86, x64 und AArch64. LLVM-Ausgabe einzelner Funktionen und in Teilmodulen behält denselben ausdrücklich unvollständigen nativen EH-Vertrag bei; binäres Umschreiben lehnt ihn weiterhin ab.

`RegistrationCallNoReturn` weist geänderte Befehlsgrenzen, Ziele, Operationsbereiche, bedingte Aufrufe und falsche Architekturen oder Formate zurück. Der PE32-Test mit ausgerichtetem Stack prüft unabhängig belegte terminale Aufrufe sowie normale Folgekanten, deren Entfernung einen aktuellen Nachweis erfordert. Ungültige Speichernachweise erzeugen keine bereits entfernte Kante neu.

`SafeSEHReference.*` prüft unveränderliche SafeSEH-Mitgliedschaft, fehlerhaften oder mehrdeutigen Speicher, Relokationen, andere Architekturen/Formate und die Deklarationsreihenfolge über Shards hinweg. `PipelineOutcome.NativeFinallyInputsRetainSourceLLVM` prüft über `NEVERD_NONLOCAL_FINALLY_INPUTS` 32 LLVM-Kombinationen nativer MSVC-Fixtures für x86/x64, O0/O2 und GS mit und ohne Quelloptimierung. LLVM und aufgelöste Aufrufsignaturen werden geprüft; nicht unterstützte native x86-EH-Rekonstruktion muss weiterhin scheitern. Der gezielte Windows-EH-Job lädt das native Fixture-Artefakt und führt die Matrix aus; ohne Eingabeverzeichnis wird der Test ausdrücklich übersprungen.

`RegistrationPrologueSaves.*` und `PipelineOptNone.*` prüfen Quellspeicher, geschützte Callback-Argumente und Selektoren sowie die Optimierung gewöhnlichen Codes. Die MSVC-Matrix bestätigt den x86-Export `/Od` (`/GS-`, `/GS`) `nonlocal_finally_return` mit unabhängiger COFF-Prüfung. `check_windows_finally_rewrite.py` vergleicht rohe und optimierte IR in beiden Patch-Modi an zwei erzwungenen Basisadressen: 24 Ausführungen, 1536 Aufrufe, Stack- und Kettenprüfungen und eine Negativkontrolle. Die EXE-DLL-Anpassung ändert nur Header, verlangt ein TLS-freies Eingabebild und erhält den Code; ein äußerer SEH-Beobachter ermöglicht die Ausnahmesuche. Andere MSVC-Formen bleiben Analyse- und Ablehnungstests.

`RegistrationEH4Wrapper.*` prüft beide Basisadressen, veränderliche und unveränderliche Laufzeitdaten, MXCSR-Verzweigungen, Definitionen auf allen Pfaden, Rahmenisolation, überlappende Anweisungen und Speicher sowie exakte PE-Relokationen. Beide MSVC-Profile enthalten eine unabhängige Negativkontrolle.

`LocalUnwindInsideCheckedSEHHandlerCarriesTheParentFrame` prüft lokale Unwind-Aufrufe aus einem nachgewiesenen Windows-x64-SEH-Handler sowie gemeinsam genutzten gewöhnlichen/Ausnahme-Einstiegen. Gegenbeispiele umfassen falsche Unwind-Allokation, instabilen oder partiellen SP, unabhängige Wurzeln, unvollständige Eingaben, fehlende oder gefälschte Ausnahme-Kanten, erschöpftes Budget, unbekannten Stack-Speicher, fremde Relokationen und nicht vertrauenswürdige Laufzeiten. LowIR-Nachweis und Modulzertifikat gehen in die SSA-Prüfung ein; gewöhnliche Rückgabefortsetzungen behalten eigene Einstiegsregeln.

`OrdinaryInternalReturnsRetainTheParentsReturnPC` und `OrdinaryInternalWritesCanUseTheParentsPrivateAllocation` prüfen beschädigte Eltern-Rücksprungadressen und getrennte lokale Schreibzugriffe auf PE, ELF und Mach-O x64 sowie ELF i386. Die erhaltene Rücksprungadresse des Aufgerufenen allein erlaubt keinen gewöhnlichen Aufruf.

`LLVMFrameSlotProof.RepeatedClosedQueriesDoNotRepeatProofWork` begrenzt wiederholte positive und negative Abfragen auf x86, x64, ARM und AArch64. Kontexttests erhalten Ablehnungen für gebundene Werte, Eintritts-SP, Breite und Nullbudget und verwerfen Antworten nach vorläufigen oder neu aufgebauten Erreichbarkeitsgraphen und Funktionswechseln.

`RegistrationState.LocalUnwindRetiresLevelsAfterCheckedFinallyEffects` prüft auch frühe Abbrüche unterhalb einer Grenze, Sentinel-Ausgänge, übersprungene Zielindizes und unterschiedliche Endzustände beim Zusammenführen. Fehlende finally-Rümpfe werden beim tatsächlichen Durchlaufen weiter abgewiesen; übersprungene Bereinigungen liefern keinen Frame-Schreibeffekt.

`RegistrationState.LocalUnwindRetiresLevelsAfterCheckedFinallyEffects` prüft auch konstante Schranken außerhalb der Tabelle, die obere Hälfte vorzeichenloser Werte und -2. Ausstiege ohne Iteration behalten die ursprüngliche Ebene und die Fakten zum Rahmenspeicher; dynamische Schranken und ungültige aktuelle Ebenen werden weiterhin abgelehnt.

`RegistrationState.MalformedSEHScopeGraphsPublishNoStateFacts` prüft falsche Sentinels, Zyklen, Vorwärts-/Bereichsfehler, ungültige Anfangszustände und Handler-/Filterrollen trotz vollständigem Parserstatus. Kontrollfälle mit aktivem Anfangsbereich und leerer Tabelle erhalten gültige EH3/EH4-Sentinels. Zu breite Handler- und Filteradressen werden ebenfalls ohne Veröffentlichung von Fakten abgelehnt.

Der Solver für Quell-Registrierungszustände verwendet dieselbe Prüfung der Verwaltungsbereiche für Objektleihen, die Konstruktion von Catch-Objekten und Local-finally-Schreibzugriffe. `RegistrationState.SEHObjectBorrowsExcludeSavedStackAndExceptionPointers` und `SEHObjectWritesCannotBorrowWholeOrPartialCookieCells` prüfen EH3/EH4-Lesezugriffe, Schreibzugriffe und teilweise Überlappungen mit SavedESP, Ausnahmezeigern und Cookie-Slots. `RegistrationCallback.RealignedCatchObjectsCannotReplaceRuntimeAnchors` schützt den gespeicherten Eintrittsrahmen unabhängig von späteren Lesezugriffen. Benachbarte gewöhnliche Objekte bleiben zulässig.

`RAMReservationTests.cpp` prüft öffentliche Schreibnachweise bei gleichen Werten und ABA, gehaltenen Bereichen, Adresswiederverwendung, gemeinsamen Mappings, Rollback, Abbruch, bestätigten Zugriffen vor Fehlern und opaker Ausführung. `UnpackDriver.RelocationFactsRequireUninterruptedLoaderWrites` prüft eigene Ladefakten an bevorzugten, niedrigeren und Kernel-Adressen: Wiederhergestellte Bytes stellen die Herkunft nicht wieder her. `DriverGuardExecution.SessionFixupsInvalidateOriginalLoaderFacts` unterscheidet aktive CFG-Korrekturen von erhaltenen inaktiven Rückfallzeigern.

`UnpackDriver.SnapshotPreservesOnlyUnmodifiedLoaderRelocationFacts` prüft PE-Ausgabe, einen unabhängigen DIR64-Leser, Modell-Rebasierung und den Ausschluss umgeschriebener Debug-Einträge. `UnpackDriver.SnapshotRelocationFactsBindSourceAndFormatWrites` lehnt falsche Quelle, Basis oder Reihenfolge ab und schließt Formatierungsersetzungen auch bei gleichen Bytes aus. Das prüft Diagnosemetadaten, keine native Wiederherstellung.

`OnlyCanonicalWaitPreservesLocalFinallyFrameEffects` prüft neun intrinsic-Formen, den Schreibeffekt des finally im Elternframe, den wiederhergestellten Argument-ESP, die Entfernung von registration und die erhaltenen Ausnahme-Einstiege. Nur das kanonische FWAIT besteht; die acht fehlerhaften oder anderen intrinsic-Formen werden weiterhin abgelehnt.

`NeverDNativeFinallyInputTests` enthält die MSVC-Eingabematrix als eigenes Testziel. `NeverDPipelineOutcomeTests` bleibt auf jeder CI-Plattform verpflichtend und hängt nicht von optionalen nativen Dateien ab. Der Windows-EH-Job baut und prüft das Eingabeziel mit seinen verifizierten Images. macOS-CI wählt Homebrew Clang 22 ausdrücklich für Quelltext-Ersetzungstests, unabhängig vom NeverD-Buildcompiler. Nicht unterstützte Stack-Probe-Verfahren werden weiterhin abgelehnt.

MSVC-Testziele verwenden `/bigobj` für die vielen COFF-Sektionen aus GoogleTest- und LLVM-Template-Instanziierungen. Andere Compilerprofile bleiben unverändert. Konfigurationstests prüfen den erzeugten Compileraufruf; native Windows-CI prüft weiterhin Kompilierung und Ausführung.

Die geprüfte Ablehnung doppelter Verschiebungen meldet den maskierten Zähler und die Operandenbreite, wenn Ziel und Flags undefiniert sind. `X64DoubleShift` prüft, dass typisierte Ausgänge und die bisherige Fehler-API den Grund ohne Register- oder RAM-Änderungen und ohne Speicherbeobachtungen erhalten; der Treibertest prüft denselben Grund vor der Szenarioausführung.

## Quellcode- und native Objektpatches

Das aktuelle Inventar `unittests/Inputs/source-patch-expansion.json` umfasst 283 CPU-Matrixfälle, 14 Layout-, 16 Rückruf-, 265 Ausnahmevertragsfälle und die genaue hostspezifische C-API-Liste. `scripts/qualify_source_patch_expansion.py` verlangt dieses vollständige Inventar ohne Fehler oder übersprungene Fälle an einem exakten, sauberen Quellcommit. `scripts/check_source_patch_expansion_runtime.py` führt Rückruf-, Abhängigkeits- und kombinierte IR-/Objektabbilder separat über den nativen OS-Loader aus: pro Host 84 Funktions-, 84 Ausnahmebeobachtungen und zwei Kontrollen für fehlende Bibliotheken/Symbole. Geprüft werden echte alte/neue Bibliotheken, Gleitkomma-/Stackargumente, wiederholte Originalaufrufe, genau einmalige Bereinigung, Loadermetadaten und Veröffentlichungs-Hashes. CPU-, Kompatibilitäts- und native OS-Prozessnachweise bleiben getrennt.

Die sechs ausgewählten nativen OS/ISA-Jobs bestanden ohne Fehler oder Überspringen: 3669 GoogleTest-Ausführungen, 1008 native Prozessbeobachtungen (504 funktionale und 504 C++-Ausnahmebeobachtungen), 12 echte Loader-Ablehnungen, 96 Images und 84 Veröffentlichungsbelege.

| Host | Quelle | Harness | CI-Job | Tests | Beobachtungen / Ablehnungen |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

Die Artefakte qualifizieren genau die unten genannten Quell- und Harness-Commits. Spätere dev-Integrations- und Dokumentationscommits haben eigene lokale und Dokumentationsprüfungen. Frühere fehlgeschlagene oder abgebrochene Jobs bleiben Fehlschläge. Die historischen 216 ergänzenden Beobachtungen sowie PE ARM32 und ältere 32-Bit-Apple-CPU-/Kompatibilitätsergebnisse belegen keine neue native Hardware- oder C++-Ausnahmeabdeckung.

Die Windows-ARM64-Qualifikation nutzt eine native ARM64-Unicorn-C-DLL, tatsächlich aus dem festgelegten Submodul mit verifiziertem LLVM-MinGW gebaut, eine MSVC-Importbibliothek und eine explizite CMake-Zielersetzung. Die isolierte Build-Kopie authentifiziert drei Hostkorrekturen: das Sprungfunktionspaar ohne SEH, einen ARM64-Seiten-Commit-Callback mit Befehls-Cache-Synchronisierung und 64-Bit-Sofortwertmasken für Windows LLP64. Submodulquellen und Revision bleiben unverändert. Dies qualifiziert den Testhost für die CPU-Matrix; standardmäßige MSVC-ARM64-JIT-Unterstützung folgt daraus nicht. Die fünf Host-Wiederherstellungsprüfungen werden von nativen Patchprozess- und C++-Ausnahmenachweisen getrennt geführt.

[source-patching.md](source-patching.md): `CPU` — source / IR / object, 283 checks.

`NeverDSourcePatchTests` · `NeverDSourcePatchMatrixTests` · `SourcePatchPE` · `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` · `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1`.

`sourcePatchTargetTriple` bestimmt ARM-Modus, ELF-Fließkomma-ABI und Mach-O-CPU-Untertyp anhand des Images. `COFFPatchObject` und `MachOPatchObject` prüfen native Symbole, Hilfs／Abschnittsdefinitionen und Unwind-Daten ohne erfundene MC-IDs. Format-Installer registrieren die tatsächlichen Tabellen. Die 283 CPU-Prüfungen verlangen Quellcode／IR／Objekte, vier Architekturen, drei Formate, Zustand, Fließkomma／Stackargumente und zwei von null verschiedene Basisverschiebungen. Alte 32-Bit-Mach-O-Container behalten echten Compiler-Code, bestätigen jedoch kein OS.

[Quellcode- und native Objektpatches](source-patching.md)

```sh
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

`OriginalBinaryUndefinedIndependence.KernelPackedFlags*` prüft IF unter CPL0 mit CPL3-Gegenkontrolle, Profilbindung und Digest-Trennung, TF/AC/IOPL-Verweigerung, obligatorische endgültige IF-Abhängigkeit auch ohne gewöhnliche Beobachtungen, Wiederherstellung und exakte/eine Operation zu kurze Budgets. `InterpreterMachineStateModel.GenerationRefusesUnsupportedOrMalformedInput` verhindert außerdem die stille Auswahl der Benutzer-Quell-ABI. Produktions-Admission undefinierter Befehle und Treiber-Recovery-Guards bleiben unverändert.

`NeverDX86X87IntegerLoadTests` vergleicht originale `FILD m16/m32/m64` mit Codegen, HighC und LLVMC unter default/NoOpt und C O0/O2. Binary80, Schutzbytes, Steuerwort und definiertes C1 decken alle RC, gültigen PC und Ausnahmemasken ab. Konkrete Prüfungen führen sämtliche 16-Bit-Eingaben und Zweierpotenzgrenzen breiter Ganzzahlen aus. Reine Konvertierungen erhalten FP-Zustand und Füllbytes. Negative Kontrollen prüfen ausstehende/unbekannte Ausnahmen, genaue Speicherbereiche, fehlerhafte Metadaten sowie leere Zielplätze auf allen Zweigen, ausgeglichenen/unausgeglichenen Schleifen und nach unbekannten Aufrufen. Ein Funktionseintritt beginnt mit dem leeren Registerstapel der Aufrufkonvention und undefinierten Slotwerten; unbekannte/belegte Ziele an anderer Stelle verhindern vollständige Funktionspublikation. Native Oracles benötigen x64 GCC/Clang. Vollständiger x87 TOP/Tag/Status, Stapelüberlaufemulation, native Fehlerpriorität und native Linux/macOS/x86-32-Ausführung sind nicht zertifiziert. `NeverDLLVMCIntrinsicSemanticTests` führt zusätzlich skalares `llvm.abs` bis 64 Bit unter C O0/O2 mit Fallen für undefiniertes Verhalten aus, einschließlich definiertem vorzeichenbehaftetem Minimum und einmaliger Auswertung.

```sh
cmake --build build-release --target NeverDX86X87IntegerLoadTests NeverDLLVMCIntrinsicSemanticTests
ctest --test-dir build-release -L '^(NeverDX86X87IntegerLoadTests|NeverDLLVMCIntrinsicSemanticTests)$' --output-on-failure
```

Die geprüfte KVM-x64-Ausführung übergibt die vollständige Befehlsschleife mit `withExecutionOwner` an den privaten vCPU-Thread. Zulassung, RAM-Transaktionen, Hooks und Budgets bleiben in dieser Schleife; jeder native Eintritt behält seine Kontrolle und Ausnahmepriorität. Der Aufrufer überwacht aktive Eintritte und wartet auf die bestätigte Abbruchbeendigung. CPU-Hooks dürfen nicht vom lokalen Zustand des aufrufenden Threads abhängen. Die Fertigstellung eines eigenständigen nativen Eintritts bleibt beim Aufrufer. `ExecutionOwnerKeepsAdmissionAndTransfersOnOneThread` und `OwnedLoopDeadlineAcknowledgesAnActualNonExitingVCPU` prüfen Zuständigkeit und echten Abbruch; diese Änderung zertifiziert keinen wiederhergestellten Treiber.

Der Windows-Workflow für Quellpatches führt außerdem `scripts/check_source_patch_pe_runtime.py` aus: drei Originale und 27 Ersetzungen für x86/x64/ARM64, danach 30 Prozessbeobachtungen je Architektur. x86/x64 verwenden Windows x64, ARM64 verwendet `windows-11-arm`. Commit, PE-Architektur, Dateihashes und vollständiges Inventar müssen übereinstimmen. Dieses C-Profil prüft keine ARM64-C++-Exceptions. `NEVERD_TEST_MATRIX_NEVERC=/path/to/neverc` wählt den normalen NeverC-Modus. Die 66 Prüfungen mit 3389.1.5 decken x64/ARM64 mit ELF, PE und Mach-O ab; x86/ARM32 sind nicht unterstützt und es gibt keinen automatischen Compilerwechsel. Der PE-Adapter verwendet `-fno-autolink`; externe `.drectve`-Objekte bleiben abgelehnt.

Die Matrix vergleicht auch die `target`-Belege kompilierter und bereitgestellter nativer Objekte mit einem unabhängigen ABI-Verzeichnis. Nach den fehlerhaften Eingaben müssen die Tests das unveränderte Objekt als gültige Kontrolle installieren und ausführen.

Die optionale Matrix `NeverCProfiles/*` prüft 30 Fälle in sechs x64/ARM64-Profilen: C-IR, C-Objekte, Hosted-C-DynCode, nicht eingebettete Hilfsfunktionen, Rekursion und C++-Ablehnung. `SourcePatch.NeverCRejectsCppBeforeStartingTheCompiler` belegt die Ablehnung ohne installierten NeverC. `SourcePatchException.NeverC*` führt C-Ergebnisse und einen Ausnahme-Callback durch zwei Hosted-C-Rahmen aus; Clang-C++-Ausnahmen werden getrennt geprüft. Skalare Swift-Konvertierungen in Registern und Speicher prüfen Ergebnisbits und MXCSR in beiden Optimierungsstufen. Der mobile Parser akzeptiert nur das genaue Rundungs-Pragma-Präfix des C-Backends und lehnt bedingte Deklarationen sowie veränderte Direktiven weiter ab.

```sh
NEVERD_TEST_MATRIX_NEVERC=/path/to/neverc build-source-full/bin/NeverDSourcePatchMatrixTests --gtest_filter="NeverCProfiles/*"
```

Die native Qualifikation verwendet NeverC `182fd2c5dc`, unabhängige Originale und den aktuellen NeverD-Installierer. `check_source_patch_hosted_pe_runtime.py` verlangt je Windows-Architektur 36 Beobachtungen für C-IR/Objekte/Hosted-Ergebnisse und Ausnahmeweitergabe durch zwei C-Rahmen. `check_source_patch_elf_runtime.py` verlangt je Linux-Architektur 120 Beobachtungen für C, Hosted-Weitergabe und neun Clang-C++-Ausnahmeszenarien. Belege, Hashes und tatsächliche Hostarchitektur müssen übereinstimmen; NeverC selbst wird damit nicht auf dem CI-Host ausgeführt. Unter Linux x64 und ARM64 laufen die neun Ausnahmeszenarien jeweils mit Clang-IR, nativen Clang-Objekten und nativen GCC-Objekten. NeverC bleibt auf C beschränkt.

Die 204 Beobachtungen des früheren Übersetzungsprofils sind historische Belege und weisen den aktuellen C-Vertrag nicht nach. Die drei aktuellen NeverC-Wege verwenden C; Clang C++ FH3 wird getrennt geprüft. macOS x64 nutzt Rosetta 2. Unicorn/QEMU ersetzen keine nativen Betriebssystem- und Unwinder-Prüfungen.

`InterpreterSpecialization.HighAddress*` und `BinaryLowIRRefinement.HighAddress*` prüfen hohe Adressen, überlappende Instruktionsbereiche, CALL-Rückkehrbytes, Stapelwiederherstellung, Daten mit Label-Werten, ungültigen oder fehlenden Code sowie genaue und zu knappe Budgets. Ein unabhängiger Byte-Interpreter und ein verändertes Rückkehrwort als Gegenprobe unterscheiden Laufzeitwerte von Labels.


`NeverDRawEnvironmentRequirementTests` prüft LowIR-Kopien, gültige und fehlerhafte Formen, strikte und nichtstrikte Emulation, symbolisches step/run, Low-to-Med, direkte C/LLVM-Ausgabe, Maschinenzustandswrapper und erneute Spezialisierung. Positive normale Kontrollen und unerreichbare Marker prüfen statische Unabhängigkeit, beide Refinement-Seiten und das Refinement vom ursprünglichen Binärprogramm zum Kandidaten; abgelehnte Beweise veröffentlichen kein Zertifikat.

`PEFixedImageTest.RawCode*` prüft beide Adresshälften, echte Rücksprungwörter, vollständige Register und Bytespeicher, unbekannte veränderliche Daten, vollständige Importlesezugriffe, fehlenden Code, Bildausschluss, ungültige und unbenutzte Deklarationen, Verfeinerung, explizite Aliasbedingungen und Belegänderungen. Exakte und um eins verkürzte Budgets umfassen viele Abbildungen, mehrere Bindungen und terminale Ausgänge. `PipelineRefusesUnusedRawEnvironmentDomains` prüft die Quellcode-Ablehnung vor der Rekonstruktion.

`RawCodeNativeReturn*` prüft zwei physische Rücksprungwörter, eine mögliche Alias-Lücke, eine verworfene unbewiesene Zelle, schmale Adressen, sämtliche Zustandsänderungen und genaue Beweisbudgets. `SymStorePreservation.Additional*` und `SymStorePreservation.AbsentAdditional*` prüfen Bereichsvereinigungen in beiden Byte-Reihenfolgen, neue Speicherepochen, fehlende Bytes, ungültige Bereiche und unveränderten Zustand bei Budgetmangel.

`FrameOffsets.*Normalization*` prüft exakte Identitäten, zusätzliche Eingaben, Wurzelidentität, unerfüllbare Prädikate ohne gespeicherten Zeugen, Standardverhalten, Struktur- und Messkosten sowie Ablehnung nach Überschreiten des Knotenlimits. `FrameNormalizationPreservesStoresAndNativeReturns` und `RawCodeFrameNormalization*` prüfen ursprüngliche Speichereffekte und physische Rücksprungwörter mit unabhängigen Byte-Orakeln, beide anwendbaren Byte-Reihenfolgen, exakte oder zu kleine Operationsbudgets und veränderte Belege. `FrameNormalizationStrategyBindsIndependentProof` prüft beide binären Beweisidentitäten.

`NormalizationRetryReusesOriginalTranslatedQuery` · `IncompleteNormalizationRetryNeverSuppliesCachedEvidence` · `NormalizationTheoremsNeverPopulateFiniteDomains`.

`NormalizationFeasibilityCachesExactTranslatedTheorems` · `NormalizationFeasibilityRejectsIncompleteAndUnrelatedProofs` · `NormalizationFeasibilityRetainsStorageAndWorkLimits`.

`MachineExitRetReadsAliasedBytesBeforePhysicalPop` · `MachineExitCallCapturesTargetBeforeAliasingPush` · `MachineExitJumpRetainsDynamicAndOriginalDirectTargets` · `MachineExitKeepsInternalCallsAndReturnProofs` · `MachineExitRequiresExactDomainBytesAndTransferKind` · `MachineExitIntegrityBudgetsAndOrdinaryConsumers` · `MachineExitProvesLiveInternalReturnCells`.

`ConstantAddressesReuseOnlyCompletedPredicateProof` · `ConstantPredicateReuseStillRequiresEachReadCertificate` · `ConstantPredicateReusePreservesInfeasibleControlEdges`.

`NormalizationChargesOnlyTheUncachedReadabilityPrefix` / `ReadabilityPrefixInspectionPreservesLazyAndCopiedCaches` prüft, dass zwischengespeicherte Historie nicht erneut berechnet wird, angehängte breite Konstanten weiterhin Zulassungskosten haben, knappe Arbeits- und Knotenbudgets ablehnen und kopierte Lesbarkeitscaches unabhängig bleiben.

## Native Lift-Qualifikation

Der Workflow `Lift native qualification` prüft die zuvor nicht qualifizierten 108 Fälle (62 Fehler und 46 übersprungene Tests). Er installiert Clang 23 oder neuer, GNU GCC und die i386-Entwicklungsbibliotheken auf einem nativen Linux-x86-64-Runner und initialisiert den festgelegten Binärkorpus. `unittests/Inputs/lift-qualification.json` erhält jede ursprüngliche Testidentität. `scripts/run_lift_qualification.py` weist fehlende, doppelte, deaktivierte, übersprungene oder fehlgeschlagene Fälle sowie einen Prozessrückgabewert ungleich null zurück. `qualification.json` protokolliert einzelne Erfolge, Quellcode- und Korpusrevisionen, Compilerversionen und Binärhashes.

Das Profil führt native i386/x86-64-Kontrollen, erweiterte x87-Präzision sowie generierte Swift-SSE-Prüfungen für Ergebnis, Zustand und unmaskierte Ausnahmen bei beiden Optimierungsstufen aus. Die AArch64-Paritätsprüfung dekodiert den echten `.text` einer festgelegten Rust-Laufzeit. Zielübergreifendes Dekodieren belegt keine native Ausführung auf diesem Ziel; dafür dienen die getrennten nativen Prozessjobs für Windows/Linux x64/ARM64 und die lokalen macOS-Prüfungen.

```bash
python3 scripts/run_lift_qualification.py \
  --binary build-release/bin/NeverDLiftTests \
  --neverd build-release/bin/neverd \
  --output build-release/lift-qualification
```

Das native Profil verlangt außerdem die LLVM-C-Prüfungen breiter Ganzzahlkonstanten und ihrer Speicherzugriffe sowie die native Ausführung synthetisierter i386-Tabellen. Swift-Signalbeobachter verwenden unabhängigen Rohspeicher und prüfen bei beiden Optimierungsstufen positive Kontrollen sowie getrennte negative Kontrollen für vorzeitige Zustands- und Ergebnisschreibzugriffe. Das Lesen einer aktiven Swift-`inout`-Variablen im Signalhandler würde eine Exklusivitätsverletzung auslösen und die Ausnahmebeobachtung ungültig machen.

`NeverDFPParameterMaskTests` führt die gemeinsame ABI-Suite und `FPParameterMask.*` aus, erhält verwendete Bytes, frühere Stores, mehrdeutige Definitionen und unmaskierte Rückgaben und verwirft nur vollständig gelöschte oder ersetzte Bytes. Es prüft D0-Transport, D16-D31-Arbeitsregister und beide FP-Rückgabepfade. `NeverDARM32ObjectImmediateTests` führt fünf ARM32-Host-C-Differentialtests separat aus; Windows-EH-CI nutzt Clang 18 einschließlich von Clang 23 abweichender NEON-zip/mask-Formen. Direkte/indirekte CALLs ohne rekonstruierte ABI-Operanden erhalten explizit vorbereitete FP-Register.

Der native Linux-x64-Job verlangt außerdem drei C++-Ausnahmeprüfungen: Clang-IR, native Clang-Objekte und native GCC-Objekte. XML und Prozessstatus werden getrennt geprüft; Fehler oder übersprungene Fälle lassen die Qualifikation scheitern. Diese C++-Eingaben verwenden NeverC nicht.

Der native Qualifizierungsjob baut die festgelegte LLVM-Fork-Revision aus den Quellen, einschließlich der Bereichskorrektur für x64-PLT/AArch64-Spezifizierer. Das vorkompilierte r4-Paket allein qualifiziert den Clang-IR-Ausnahmepfad nicht.

## Darwin dylib Importoffsets

`StartupDylibAddendsPreserveDataAndTLSAliasing` prüft +8/-4, Änderungen des TLS-Zeigers und frischen Prozesszustand. C, CLI und echtes Python prüfen alle Profile. Das Pflichtinventar umfasst 101 Methoden je Profil und 303 passende ARM64-Fälle; der gemeinsame native Katalog bleibt 82. Nicht verfügbare Hardware zählt nicht als native Ausführung.

[Darwin](../darwin-emulation.md#explicit-startup-dylibs-and-main-thread-tls)

## Ordinary Darwin whole-file lock coverage

`DarwinAdvisoryLockOptions` and `DarwinAdvisoryLockTest` check complete unique initial objects, shared byte budgets, both page sizes, low carriers, action priority, readonly EX, reentry, failed upgrades, downgrade, missing versus empty providers, FD/kind ordering, duplicate/fcntl/dup2 lifetime, descriptor reuse, hardlinks, rename, held zero-name unlink and creation refusal. Real VM mappings and retained Contents cannot keep a closed description’s lock; mode/ownership/data/xattr changes keep independent authority and metadata validity. A contended blocking request stops before model mutation. Strict C/CLI/Python and the original SDK-free77-record workload cover five software and three matching HVF profiles, including empty-provider boundaries. Native-common64 and mandatory79 per platform require all237 matching ARM64 native identities to execute successfully. The720 original ARM64 O0/O1/O2 literal observations comprise522 action and198 namespace/descriptor/VM-lifetime controls. Intel native, physical iOS, external peers and wait lifecycle remain separate unverified coverage. Native5s, guest/Python5,000,000us/quantum1024 and public10s remain unchanged.


## Abdeckung der Synchronisierung gewöhnlicher Darwin-Dateien

`DarwinSynchronizationOptions` and `DarwinSynchronizationTest` check complete initial grants, shared byte budgets, 4KiB/16KiB pages, three upper carriers, readonly/writeonly/readwrite descriptions, missing versus empty providers, FD/kind ordering, dup/fcntl/dup2 and FD reuse, runtime links/rename/held zero-name unlink, recreated-object refusal, real private mappings and retained Contents. Write/truncate, mode/ownership, invalid stat/xattr observations and advisory locks retain their independent state. Both immutable query domains close all57 other logical routes. The original SDK-free68-record workload and C/CLI/Python preserve20 guarded full stat observations and separate main40/stdout S across five software and three matching ARM64 HVF profiles. Native-common68 and mandatory83 per platform require all249 matching ARM64 identities to execute successfully. Original O0/O1/O2 native captures contain504 literal observations with guarded176-byte stat records, cursors, raw/libc paths and three high carriers. Directory/pipe captures do not expand the ordinary provider contract. Native5s, guest/Python5,000,000us/quantum1024 and public10s are unchanged. Native Intel, physical iOS, durability, full cancellation, provider failures and asynchronous waits remain unverified or unsupported.


## Ordinary Darwin data synchronization coverage

`DarwinDataSynchronizationOptions` and `DarwinDataSynchronizationTest` execute complete initial admission/shared budgets at4KiB/16KiB, low FD carriers, readonly/writeonly/readwrite, FD/kind/provider ordering, missing/empty environments, dup/fcntl/dup2/reuse, runtime names/held zero-name/recreation, real VM and retained Contents, committed write/truncate records/cursors, invalid stat/xattrs, mode/ownership and locks. The existing `DarwinSynchronizationTest.DataGrantCannotSupplyFileCompletionProvider` and new `FileGrantCannotSupplyDataCompletionProvider` cases execute file-only/data-only/both/empty providers. Neither purpose restores observations or supplies the other's grant. Both query domains close all57 other logical routes. SDK-free41 records/11 guarded full stat observations/main41/stdout D use five software and three matching HVF profiles plus C/CLI/Python. Native-common68/mandatory83 require all249 matching ARM64 identities to actually pass. Original O0/O1/O2 controls retain252 literal observations with176-byte guarded native stat/cursors. Native5s, guest/Python5,000,000us/quantum1024 and public10s remain unchanged. Native Intel, physical iOS, physical durability, general cancellation, provider failures and asynchronous waits remain unverified or unsupported.


## Created ordinary Darwin completion coverage

DarwinCreatedSynchronizationOptions and both4096/16384-page DarwinCreatedSynchronizationTest cases execute complete policy prerequisites, independent purposes/defaults, initial/creation separation, readonly/dirty data, carriers, literal created metadata, write/truncate state, runtime links/rename/held removal/recreation, dup/fcntl/dup2/reuse, real VM retention, failed descriptor/entry/byte/inode admission and unchanged unrelated authorities. CreatedRegularSynchronizationKeepsIndependentProvidersAndState is mandatory on every available matching ARM64 transport. ProcessReport strict flag/policy cases run in NeverDLinuxProcessTests; C/CLI and the complete Python method preserve147 records/33 guarded full stat observations/main42/C through five software profiles. Common68/mandatory83 per platform require all249 matching ARM64 identities to actually pass. The702 original O0/O1/O2 controls observe each fresh unflushed target itself. The SDK-free first native failure is retained: raw created F_GETFL65538 exposed an invalid hard-coded2; corrected checks capture the initial flags and verify access bits and preservation. The first model process failure is also retained: its existing FileWasWritten0x10000 owner produced post-write65538. C++/C/CLI/Python expectations include that existing write history and preserve whole flags; no new status-bit representation is introduced. Native5s, guest/Python5000000us/quantum1024 and public10s remain unchanged. Intel native, physical iOS, physical durability, full cancellation and provider/wait lifecycle stay unverified or unsupported.


## Created ordinary Darwin advisory lock coverage

DarwinCreatedAdvisoryLockOptions and both4096/16384-page DarwinCreatedAdvisoryLockTest cases cover complete policy prerequisites, shared byte budgets, independent defaults/initial objects, existing O_CREAT and replacements, persistent status on initial/created descriptions, failed/unsupported actions, F_SETFL history roundtrip, carriers, complete dirty state, dup/fcntl/dup2/FD reuse, runtime names, actual mapping and final-description release, failed FD/entry/byte/inode admission, directory/link exclusion and independent authorities/unknown observations. CreatedRegularAdvisoryLocksKeepProviderAndDescriptionStatus is mandatory on available matching ARM64 transports. ProcessReport strict tests run in NeverDLinuxProcessTests; C/CLI and the complete Python method verify131 literal records/twelve guarded stat observations/main43/L through five software profiles. Native-common68/mandatory83 require all249 matching ARM64 identities to actually pass. Existing SymbolicDescriptorTruncateAndControlShareOnlyDescriptions runs both pages with ENOTTY25 for recognized history, unchanged actual history/stat/cursors, and genuine unsupported flags. Opaque stream requests return zero without acquiring history. Separate162 native symbolic/pipe controls prove these exact results. Original486 flock observations plus72 fcntl checks at O0/O1/O2 prove persistent FWASLOCKED0x4000, original bounds, whole176-byte stat/content/cursors and real mapping lifetime. Retain the first native assumption failure and repair the existing status owner; success sets history, unlock keeps it, and errors/refused waits do not manufacture it. Skips remain unexecuted; full timing stability, Intel native, physical iOS, external peers and wait/provider lifecycle remain unverified.


## Created ordinary Darwin attribute coverage

Neue reguläre Dateien erhalten getrennte Modus- und Eigentümerrechte nur durch ausdrückliche Deklaration. Beide Schalter sind standardmäßig false; true erfordert eine vollständige Erstellungspolitik. Die Wiederverwendung eines Namens überträgt keine bestehenden Rechte. Erfolgreiche Änderungen verwenden mutation_time als virtuelle ctime. Auch Anfragen ohne Eigentümer- und Gruppenänderung benötigen den Provider und erhalten jedes Byte. Unbekannte Metadaten oder Gruppenmitgliedschaften bleiben nicht unterstützt.

DarwinCreatedAttributeOptions and DarwinCreatedAttributeTest register three admission/default cases and thirteen cases on each of 4096/16384-byte pages: 29 identities. They check independent flags/initial times, existing O_CREAT, fresh replacements, dirty bytes/cursors/status, three high carriers and all mode/owner routes, readonly/duplicate/independent descriptions, inactive ownership and active equal IDs, errors/unknown security or groups, names/held unlink, private mappings, failed FD/entry/byte/inode admission, directory/link exclusions, independent authorities and invalidated metadata. CreatedRegularAttributesKeepIndependentProvidersAndObjectState is mandatory on available matching ARM64 transports. Strict JSON runs in NeverDLinuxProcessTests. C/CLI and one whole Python SDK method check five software profiles: 235 authored common services/main44/stdout A/137 guarded stat observations and nine virtual services/main45/320 independent literal guarded bytes. Process and SDK success cases each use two fresh common and virtual runs. Virtual GID31 explicitly declares effective groups20/31; real GID31 alone proves nothing. Common native69 and mandatory84 per platform require all252 matching ARM64 identities to pass. The complete Darwin owner must discover3098 identities (2090 runnable/1008 unavailable), with broad C++ owners requiring1808 passes; actual receipts determine qualification. Fresh O0/O1/O2 native controls record450 mode and720 ownership observations at native5s with whole176-byte guarded stat records. Native timestamps are not literal virtual ctime; SDK-free native execution proves main44/A without a captured service trace. Preparation/compile failures and older HVF time-limit failures remain evidence. Whole-method revalidation, if required, is an explicit aggregate without a timing-stability claim. Guest/Python5000000us/quantum1024 and original bounds stay unchanged. Intel native, physical iOS, full frameworks, root privilege, general DAC and host provider lifecycle remain unqualified.

The first process expectation failure is retained:136 assertions omitted upper raw syscall or FD carrier bits. Fresh expectations derive the complete raw carriers from the unchanged workload; optional FD expectations also check literal UINT64_MAX instead of conflating it with an absent check. Runtime decisions, scalar/errno results, guarded records and timing bounds are unchanged.

The complete-owner first round retained two x64 stack-budget failures. Compiler inlining reserved13688 bytes on every main branch, so the unchanged four-page stack test selected an unmapped page below SP and could not release budget. Only the two new workload helpers now use noinline; main reserves56 bytes, and all eight runnable original budget cases pass without a larger budget. Later guest time-limit failures remain retained. Five fixed-length loops in the shared stat helpers use compile-time full unrolling: every original guard, mask, scalar/error check, all137 complete observations and all235 raw calls remain. The runtime implementation, guest5s, method/owner bounds and scheduling policy remain unchanged. Failure diagnostics retain mode/repeat, instructions, PC and completed service count. Isolated entitled CLI completion does not establish timing stability or prove the cause of another host timeout.


## Ordinary Darwin full and barrier completion coverage

Explizite Provider für vollständige Synchronisierung und Barrieren schließen Vorgänge auf regulären Dateien unabhängig ab. Anfangsrechte und Rechte für neu erstellte Objekte sind getrennt; Erstellungsflags sind standardmäßig false und true erfordert eine vollständige Erstellungspolitik. Der Abschluss erhält alle Modellbeobachtungen und ignoriert das ungenutzte Argument. Barrieren werden direkt unterstützt, ohne Mount-Hochstufung oder Rückfall. Beide Deklarationen leeren keinen Host-Speicher und belegen keine physische Dauerhaftigkeit.

DarwinFullSynchronizationOptions and DarwinFullSynchronizationTest register three admission cases and twelve parameterized cases over4096/16384-byte pages and commands51/85:51 identities. They cover initial/created defaults and policy prerequisites, shared reference budgets, access modes, raw upper carriers, ignored invalid pointers, bad/closed FDs and kind/scope ordering, missing/empty/other-purpose grants, duplicate/fcntl/dup2/reuse, links/rename/held removal/recreation, real VM retention, dirty bytes/truncate/mode/ownership/locks/xattrs, invalid metadata, directory/link exclusion and failed admission. FullAndBarrierSynchronizationKeepIndependentObjectState is mandatory on each available matching ARM64 transport. The SDK-free common workload has148 authored services,38 guarded full stat observations and separate main46/stdout F. Its native execution establishes46/F without a captured service trace. Virtual-only creation uses ten services, separate main47 and320 independent literal guarded bytes; this is not a native timestamp oracle. Process and Python success paths use two fresh common and virtual runs across five software profiles; C/CLI checks strict independent success and missing/empty providers. New strict JSON cases run in NeverDLinuxProcessTests. Common native70 and mandatory85 per platform require all255 matching ARM64 native identities to execute successfully. The complete Darwin inventory is3169 identities; expected2149 runnable and1020 unavailable identities must be reconciled against actual execution receipts. Broad public/Linux/service/budget/safety/integration owners and nineteen complete Python methods are separate acceptance checks. Fresh O0/O1/O2 native controls record216 raw calls and648 guarded176-byte whole-stat comparisons, including known native pipe EBADF9, low carriers and ignored arguments. Those controls establish observed completions, not whether a native mount used direct barriers or promotion, and not physical durability. Retain initial test-only failures: write access had no explicit write grant, and exhausted virtual inode admission is unsupported rather than a guessed ENOSPC28. Original guest/Python5000000us/quantum1024, native5s, public10s, registered method and owner bounds remain unchanged. Older stack and HVF timing failures remain evidence. Native Intel, physical iOS, default-fatal child cleanup, provider/wait lifecycle and complete Apple frameworks remain unqualified.


## Explicit ordinary timestamp mutations

Explizite Zeitänderungsprovider autorisieren reguläre Dateien unabhängig. Anfangsrechte und Rechte erfolgreich neu erstellter regulärer Dateien sind getrennt. Das Erstellungsflag ist standardmäßig false; true erfordert eine vollständige Erstellungsrichtlinie. NULL-Anfragen verwenden nur eine separat deklarierte aktuelle Zeitprobe. Die Speicherung angeforderter Zeiten und das feste virtuelle ctime sind getrennte Beobachtungen. Hostuhr und Hostdateisystem werden nicht abgefragt; die vollständige macOS/iOS-Framework-Umgebung ist weiterhin unvollständig.

DarwinTimestampMutationOptions and DarwinTimestampMutationTest cover initial/created grants, shared catalogue limits, explicit and NULL requests, signed microseconds and seconds saturation, nonnegative birth boundaries, request/error ordering, effective ownership and known/unknown group permissions, whole guarded stat records, dirty bytes, offsets, descriptor flags, lock/xattr independence, unknown metadata, hardlinks/rename/held unlink, failed creation and real VM retention. C++ process, strict JSON, C/CLI and one whole Python method cover five software profiles and three required matching ARM64 HVF profiles. Earlier batch counts are historical observations; the timestamp batch required71 common workloads and86 mandatory methods per platform. Every discovered matching ARM64 native identity must actually pass for complete owner acceptance. Native captures are evidence on their test filesystem, not a universal storage rule. Intel native, physical iOS, general setattr lifecycle, dyld/Mach IPC/ObjC/Swift/Foundation/UIKit and full OS execution remain unqualified.

## Explicit filesystem observations

Explizite Dateisystem-Snapshots liefern unabhängig Kapazität, Typ und Mountnamen für reguläre Dateien und Verzeichnisse. Nullwerte und leere Namen sind gültig; das Hostdateisystem wird nicht abgefragt. Bindungen bleiben beim tatsächlichen Objekt über duplizierte Deskriptoren, Verschieben und Löschen bei offenem Deskriptor erhalten; Ersatznamen erben sie nicht. Neue reguläre Dateien erhalten den gebundenen Snapshot ihres tatsächlichen Elternverzeichnisses nur mit explizitem Erstellungsflag und vollständiger Erstellungsrichtlinie. Die Dateiumgebungsabfragen sind nutzbar; die vollständige macOS/iOS-Framework-Umgebung bleibt unvollständig.

DarwinFileSystemOptions and DarwinFileSystemTest cover exact wide/signed fields, UTF8/empty/maximum names, full guarded records, low carriers, missing/empty/other-purpose declarations, path/FD/output order, partial output without effects, independent target metadata and access flags, effective known/unknown group search, root/special/unknown ancestors, implicit directories and followed links, CWD, separate opens/dup/fcntl, same-ID noninheritance, hardlinks/rename/held unlink/reuse, explicit/default/unbound creation, directory moves/default-created-directory exclusion, dirty bytes/locks/xattrs and real VM retention. C++ process, strict JSON, C/CLI and one whole Python method execute both authored common and independent virtual modes across five thin profiles; all three matching ARM64 HVF identities are mandatory. Full owner acceptance reconciles discovered identities and actually passed native requirements. All prior timing and test-input failures are retained in immutable receipts. Source/products are pinned and heavy work is serial under the existing acceptance lock. Old batch totals remain historical; current native registration is72 common workloads and87 mandatory methods per platform. Native Intel, physical iOS, default-fatal child cleanup, provider wait lifecycle and complete Apple frameworks remain unqualified.

`DarwinCreatedAttributesPublic.PreserveProvidersAcrossCAndCLI` registriert fünf Plattformfälle einzeln und behält alle 180 C-API/CLI-Ausführungen bei der bisherigen Grenze von 30 Sekunden je Fall bei. `DarwinCreatedRegularAttributesRejectMalformedProvidersBeforeLoading` behält die zehn ungültigen Eingaben bei. Mobile Regressionen prüfen den unveränderten Text und weisen geänderte Direktiven zurück. Alle zwölf Varianten für Skalare, Aufrufe/Blocks und Swift müssen neu kompiliert und ausgeführt werden; danach folgt `test_mobile_ios_single_session.py`.

## Declared mount enumeration

Die deklarierte Mountliste liefert geordnete Beobachtungen und explizite Aktualisierungen. Jeder Prozess besitzt einen eigenen Cache; doppelte Einträge bleiben erhalten und eine leere Liste unterscheidet sich von einer unbekannten. Diese Dateisystemabfrage ist nun nutzbar, während dyld, Mach IPC und die vollständigen Objective-C/Swift- und Foundation/UIKit-Umgebungen unvollständig bleiben.

DarwinMountEnumerationOptions and DarwinMountEnumerationTest exercise both 4KiB and 16KiB pages, the shared entry/byte budgets, exact independent record bytes, duplicate order, signed and high size carriers, unknown versus empty tables, NULL and short outputs, flag precedence, partial records, late missing refresh and complete faults, earlier committed effects, cache reset and independent statfs/fstatfs observations. Strict JSON tests reach both nested snapshot parsers and preserve the separate 64KiB envelope. The SDK-free common workload checks count, truncation, flags, guarded records and native error ABI; the virtual workload emits 19609 independently authored bytes across three snapshots plus M. C++ process, C/CLI and Python exercise five thin profiles and process reuse, with three matching ARM64 HVF identities mandatory. The new common native registration is73 and required method registration88 per matching platform. Native Intel hardware, physical iOS, default-fatal child cleanup and full Apple frameworks remain unqualified.

## Swift scalar instruction completion

Wiederhergestelltes x86_64-Swift unterstützt skalare ADD/SUB/MUL/DIV für Register und Speicher, vorzeichenbehaftete 32/64-Bit-Ganzzahlkonvertierung nach Float32/Float64 sowie Float32/Float64 in beide Richtungen einschließlich VEX. Numerische Bits und vollständiger ein-/ausgehender MXCSR bleiben gekoppelt. Gepackte Operationen, SQRT/MIN/MAX, FMA, gesondertes Runden und Näherungen werden weiterhin abgelehnt; die vollständige macOS/iOS-Framework-Umgebung bleibt unvollständig.

Gemischte alte/generische und alte/VEX-Formen prüfen einen Helfer je Zahlen-/Zustandsvertrag. Unabhängige ursprüngliche SSE-Beobachter und Literalwerte vergleichen Onone/O für Rundung, klebende Flags, DAZ/FTZ, Subnormale, vorzeichenbehaftete Nullen, NaNs und alle skalaren Konvertierungen. Nicht maskierte Arithmetik-/Konvertierungstraps erfordern natives Linux x64 oder Intel-macOS und unveränderte Ergebnis-/Zustandswächter; ARM64 überspringt sie ausdrücklich, da Rosetta Ausnahmen maskiert.

## Explicit ordinary directory modes

Explizite Mode-Provider unterstützen gewöhnliche Verzeichnisse und Dateien. Berechtigungen für anfängliche und neu erstellte Verzeichnisse bleiben unabhängig von Namensänderungen und Kindobjekten. Die vollständigen Umgebungen für dyld, Mach IPC, Objective-C/Swift und Foundation/UIKit sowie Tests auf physischen iOS-Geräten bleiben offen.

DarwinModeTest covers both4KiB and16KiB pages with complete independent stat records, changed/equal permissions, strict authority, alias admission, independent initial/created grants, high carriers, errors, cursors, direct moves/exchanges and both namespace/mode orders. The focused61 checks passed. Earlier complete-owner rounds retained two obsolete directory-target expectations and intermittent legacy mount-enumeration/created-attribute HVF guest timeouts. Final required method revalidation uses the original complete method, unchanged source and binary hashes, and original method/guest deadlines; earlier failures remain evidence. DirectoryModesPreserveIndependentObjectAndNamespaceState is mandatory on all three available matching ARM64 HVF profiles. Five thin software profiles also pass process, C/CLI and Python, including fresh runs in a reused public session. Native common inventory74 and mandatory inventory89 require267 matching ARM64 executions; the complete Darwin owner reconciled3419 identities with2351 passed and1068 unavailable hardware identities across four disjoint serial shards plus full-method revalidation of retained guest timeouts. This is a composite qualification, not a single unsharded timing-stability claim. A retained HVF run reached the unchanged guest5-second limit during complete stat verification under high observed host load; load is contextual evidence, not a proven sole cause. Equivalent word comparisons and fully unrolled fixed-length fixture loops retain all33 records, complete fields and guards, raw calls, errors and independent virtual times. The qualified fixture reduces checked guest instruction work without changing runtime semantics, scheduling or deadlines. Earlier invalid-fixture, registration-controller and build-target errors remain retained. The first captured-dev Lift rebuild reached its original1200-second deadline; the incomplete attempt is retained and the final incremental build is separately qualified under the same bound. Intel hardware, physical iOS and the complete Apple framework environment remain unqualified.

## Explicit ordinary directory timestamps

Explizite Zeitstempelanbieter unterstützen utimes/futimes für unabhängig freigegebene anfängliche und neu erstellte Verzeichnisse. Namensraum-, Modus- und Eigentumsrechte bleiben unabhängig. Vollständige Apple-Laufzeiten und Frameworks sowie physische iOS-Prüfungen sind noch unvollständig.

The97 initial private/strict JSON checks passed before route addition. Directory timestamp cases cover4KiB/16KiB, full literal stat144 with guards, initial/created grants, all time/namespace orders, aliases, effective owner/nonowner decisions, dup/cursors, direct move/exchange, held removal/reuse and permanent invalidation. Five thin software process/C/CLI/Python profiles and all three matching ARM64 HVF profiles passed with fresh executions; foreign hardware is unavailable coverage. The complete Darwin owner reconciles3469 identities with2389 passed and1080 unavailable, including all270 mandatory matching ARM64 executions, across four disjoint serial bounded shards. This is not a single unsharded timing-stability claim. Shared public/Linux/service/budget/safety owners passed1845 cases, with existing timestamp and filesystem Python regressions. The native common inventory is75 and mandatory inventory is90 per matching platform. The preparation controller's ignored-plan assertion, one isolated test's retained movable capability without mutable parent, and a stale regular-object diagnostic expectation are preserved failures corrected in fresh epochs. Native5s, compiler120s, component1200s/parallel2, original method bounds, guest5000000us/quantum1024, public30s and other established limits remain unchanged. Physical Intel/iOS, nonowner/special native security and complete Apple runtime/framework execution remain unqualified.

## Explicit ordinary directory ownership

Explizite Eigentumsanbieter unterstützen chown/fchown/fchownat für unabhängig freigegebene anfängliche und neue gewöhnliche Verzeichnisse. Gruppenänderungen aktualisieren auch das tatsächliche Elternobjekt für neue Kinder. Namensraum-, Modus- und Zeitstempelrechte bleiben unabhängig. Vollständige Apple-Laufzeiten und Frameworks sowie physische iOS-Prüfungen sind noch unvollständig.

Initial focused private and strict JSON checks passed103 identities before route addition. New directory cases cover4KiB/16KiB, complete literal stat144 with guards, independent initial/created grants, actual current parent group inheritance for all3routes and all3child kinds, aliases, active/inactive IDs, authorization and group knowledge, high carriers, errors, dup/cursors, moves/exchanges, held removal/reuse and permanent invalidation. Five software process/C/CLI/Python profiles and all three matching ARM64 HVF profiles passed fresh executions. The complete Darwin owner reconciles3511 identities with2419 passed and1092 hardware-unavailable, including all273 mandatory matching ARM64 executions, across four disjoint serial bounded shards plus37 fresh isolated replays of the initial guest-time-limit failures. Every replay passed on the same source/products and original bounds. This is not a clean full-round or unsharded timing-stability claim. Shared public/Linux/service/budget/safety owners passed1852 cases, together with affected Python attribute/filesystem regressions. Native common inventory is76 and mandatory inventory91 per matching platform. The earlier complete Darwin round retains guest-time-limit failures during high observed host load; the final complete-inventory qualification uses unchanged source, products and bounds, combining the original disjoint shards with focused replays. Native5s, compiler120s, component1200s/parallel2, original method bounds, guest5000000us/quantum1024 and public30s remain unchanged. The new negative-membership fixture correction and a guest-deadline failure from redundant end-to-end Cartesian snapshots are retained in immutable epochs; the final workload keeps independent full typing proof and unchanged bounds. Physical Intel/iOS, native nonowner/special security and complete Apple runtimes/frameworks remain unqualified.

## Symbolic ownership regression

`DarwinSymbolicOwnershipTestData.h` supplies independently authored complete stat144 records with two8-byte guards. `DarwinOwnershipTests.cpp` covers exact initial/created authority, effective credentials and known/unknown groups, complete-record preservation/commit, descriptor versus SEARCH error order, flags/carriers, dangling/cyclic targets, alias/dup/move/exchange/held-unlink/reuse and permanent metadata invalidation. Strict JSON checks preserve purpose-specific Booleans, defaults, creation prerequisites and lossless timestamp import.

The SDK-free `darwin_process.c` ownership modes use59/60 authored common calls and67/68 literal calls, return58/W and emit1281 bytes in each literal mode. The eight full guarded records distinguish original link, unchanged/changed target, two-name/renamed/held link and fresh replacement. Process tests repeat positives from fresh state and retain early/late unsupported outcomes with exact raw service/scalar argument checks. Native catalogue seeding explicitly creates a symbolic input and a separate binary target. Original owned nonroot O0/O1/O2 probes retain318 observations, and common initial/created native workloads execute at all three levels. Common native checks capture no service trace and assert no fixed native ctime.

`SymbolicOwnershipKeepsVnodeAndTargetGrantsSeparate` is required on every matching native platform through `scripts/NativeDarwinTests.def`; inventory now contains94 methods per platform and282 matching ARM64 identities. `DarwinNativeCases.def` contains81 common cases. Execute all five software profiles, all three matching ARM64 HVF identities and C/CLI/Python routes; skips do not count as native acceptance. Preserve guest5000000us, quantum1024, native5s, compiler120s, component1200s/parallel2 and public30s limits. Full Darwin qualification uses the existing disjoint serial method shards, with any bounded unchanged-source recovery reported as a composite. Intel hardware, physical iOS and complete Apple runtimes remain unqualified.

## Filesystem snapshots for created directories

Ein unabhängiges Erstellungsflag bindet neue Verzeichnisse an den deklarierten Dateisystem-Snapshot ihres tatsächlichen Elternobjekts, sofern vorhanden. Offene Deskriptoren behalten die Bindung nach Verschieben und Löschen. Vollständige Apple-Laufzeiten und physische iOS-Prüfungen bleiben unvollständig.

Initial focused private and strict JSON checks passed125 identities before route addition. Final cases cover both page sizes/routes, full2200 guarded literal records, independent flags, zero versus unbound parents, initial/symbolic exclusion, actual parent binding after moves/exchanges, child moves, held rmdir/name reuse, failed/exhausted-inode creation, descriptor flags, full stat preservation, group changes and permanent metadata invalidation without path SEARCH repair. Five software process/C/CLI/Python profiles and three matching ARM64 HVF profiles pass. C API successes repeat in reused sessions; CLI reports are compared completely.

Owned nonroot ARM64 O0/O1/O2 probes passed102 observations with complete2168-byte records and guards, primary/secondary/carry, high carriers, error order, CWD, child kinds, moves and held deletion/name reuse. Common native main56/D passes with33 authored calls; virtual-only35 calls emit4400 independent literal guarded bytes plus D. Native comparisons exclude only five dynamic uint64 counters; no native service trace or universal capacity is claimed.

The complete Darwin owner reconciles3565 identities: 2461 passed, 1104 hardware-unavailable, zero failures; all276 mandatory matching ARM64 identities actually pass in four disjoint serial bounded shards. This is not an unsharded timing-stability claim. Shared public/Linux/services/budget/safety owners pass1858 C++ cases. All20 products rebuilt; inventories77 common native and92 mandatory per matching platform verified. Original fixture/discovery failures stay immutable: explicit creation/swap/empty-provider prerequisites and a regular-only stat helper were corrected, without changing production semantics or the33/35-call sequence. Native5s, compiler120s, component1200s/parallel2, guest5000000us/quantum1024, public30s and original method bounds remain unchanged. Apple runtimes/frameworks, Intel native and physical iOS remain unfinished.

## Filesystem snapshots for symbolic vnodes

Explizite Snapshots unterstützen symbolische Deskriptoren mit fehlenden oder zyklischen Zielen. Pfadabfragen folgen weiterhin dem Ziel. Neue Links verwenden ein unabhängiges Flag für die Bindung an das tatsächliche Elternobjekt. Vollständige Apple-Umgebungen und physisches iOS bleiben ungeprüft.

Initial private/strict JSON checks passed166 identities before route addition. Final coverage uses both4KiB/16KiB pages and both path/FD routes: full2200-byte literal guarded snapshots, link-versus-target declarations with identical IDs, unknown/empty/zero snapshots, dangling/cyclic/nonUTF8 targets, complete namespace prerequisites and all8 independent creation-flag combinations, no target metadata/stat/credential dependence, permanent invalidation, dup/flags/target bytes, moves/exchanged parents, held unlink/replacement, failed creation/inode exhaustion and exact reference budgets/partial copyout.

Owned nonroot ARM64 O0/O1/O2 native probes passed114 observations, full2168 payloads/guards, raw primary/secondary/carry, high carriers, readonly/dup, target errors, parent moves, held deletion/name reuse and unchanged stat/flags/cursor. The common initial32 and created33 authored services pass main57/Y natively; virtual35 emits6600 independent literal bytes plus Y, separating link owner17 from target/replacement owner4294967294. Native comparisons mask only five dynamic uint64 counters and capture no native service trace. Five software process/C API/CLI/Python profiles and three matching ARM64 HVF profiles pass fresh executions; C API successes reuse sessions and CLI reports compare completely.

Complete Darwin composite: 3624 registered, 2508 passed, 1116 hardware-unavailable, zero remaining failures; all279 required matching ARM64 identities actually pass in four disjoint serial bounded shards plus1 fresh isolated replay of the original resource-usage HVF guest-time-limit failure. The initial full round and all failures remain retained. Identical source/products and unchanged limits qualify the composite only; no clean full-round or unsharded timing-stability claim. Shared public/Linux/services/budget/safety owners pass1866 C++ cases. All20 products rebuilt; common native79/mandatory93 per matching platform independently asserted. Earlier compile enum typo, mutable-parent fixture omissions and failed repair matching/controller syntax attempts remain immutable; no production semantics were weakened to pass them. Native5s/compiler120s/component1200s parallel2/guest5000000us quantum1024/public30s and original method bounds are unchanged. Intel native, physical iOS and complete Apple runtimes/frameworks remain unqualified.

`LLVMCValues.NativeBlockAddressesCannotBecomeZeroConstants` weist Blockadressen in Rückgaben, Integer-Konvertierungen, Adressarithmetik und verschachtelten Tabellen zurück, sowohl im ganzen Modul als auch bei einer ausgewählten Funktion. Die Matrix umfasst 32/64 Bit, beide Byte-Reihenfolgen und ELF/COFF/Mach-O. Nicht unterstützte Symbolkonstanten werden ebenfalls ausdrücklich abgelehnt und nicht zu Nullzeigern. Dies ist ein Ablehnungsvertrag, keine portable native Fortsetzung.

`X86_64_PipelineE2E.DecompileC_InteriorCodeAddressRefusesWithoutReplacingOutput` CLI-Ablehnungen für Modul und Funktion werden bei beiden Optimierungen geprüft, ohne bestehende Ausgaben zu überschreiben. Andere atomare Funktionen müssen C ausgeben. Untere Adressbits bleiben nicht unterstützt; LLVM-Verifikation beweist diese Semantik nicht.


### Deklariertes physisches Speicherinventar

`KernelPhysicalInventoryTests` prüft Einheiten, Terminator, unabhängige Allokationen, nebenwirkungsfreie Ablehnung, Deklarationskopie, Erfassungsabhängigkeiten, Pins, physische Identität und Mangel. `DriverPhysicalInventoryTests` prüft strikten Transport sowie vollständigen normalen/verschobenen Lebenszyklus, dynamische Suche und Lesen nach Freigabe auf Unicorn/KVM/WHP. Der UNPACK-Test mit ungenutzter Deklaration bleibt ablehnend. Je nativem Backend sind 13 Fälle Pflicht; lokal nicht verfügbare Plattformausführung bleibt übersprungen.

### Explizite Abfrage des Kernel-VA-Schattenstatus

`KernelVaShadowQueryTests` prüft unabhängige Flags, Längen, Seitengrenzen, ULONG-Kürzung, beide Ausgaben, Überlappung, Rechte, Aliase, Objekte, Kontext, IRQL und Abhängigkeiten. `DriverVaShadowQueryTests` prüft strikten Transport und relokalisierte Unicorn/KVM/WHP-Lebenszyklen mit importierten/dynamischen Aufrufen. Ungenutzte Deklarationen verhindern weiterhin UNPACK-Wiederherstellung. Je nativem Backend sind 19 Identitäten Pflicht; nicht verfügbare Ausführung wird explizit übersprungen. Bestehende Versionstests prüfen den gemeinsamen Pufferprüfer.

## Qualifikation von Quellcode-Patches nach der Integration

Die native Linux-Qualifikation führt zusätzlich den gesamten `HighSwiftEmitter`-Testbestand aus: Register- und Speicherkonvertierungen, gemeinsame Legacy/VEX-Helfer, Vergleiche mit ursprünglichem SSE und unmaskierte Ausnahmen. Verwenden Sie LLVM aus dem korrigierten Quellcode: r4 kann i386-PIC-GOT-Anker falsch kodieren; eine ungültige Ankerdistanz verhindert nun die Veröffentlichung. Zehn Linux/ELF-spezifische `SessionCAPITest`-Fälle bleiben unter macOS und Windows ausdrücklich übersprungen. Die Prüfung gleicht Namen, Eigentümer-Labels und ursprüngliche Gründe exakt ab, verlangt weiterhin die native Linux-Ausführung und zählt diese Fälle nicht als bestanden. Der native No-Return-Test berücksichtigt Mach-O-Symbolnamen und die rekonstruierte Argumentbreite.

### Begrenzte Erfassung des Kernel-Aufruferkontexts

`KernelContextCaptureTests` prüft sämtliche Bytes, Guards, Stack, Flags, Aliase, Überlappung, Objekte, Rechte, IRQL und injizierte Fehler. `DriverContextCaptureTests` prüft lebende GPR/FPCW/MXCSR/XMM, relozierte Eintritts-/Anfrage-/Unload-Zyklen und Abhängigkeiten. Zwölf Bytevergleiche haben ein explizites Testbudget von 400000 Instruktionen; separate Collector-Fälle variieren FPCW. Alle 17 Identitäten pro nativem Backend sind Pflicht; nicht verfügbares WHP bleibt unter Linux ausdrücklich übersprungen. Bestehende x64/ARM64-Benutzertests sichern das alte Verhalten.

### Symbolische Regionskoordinaten über null hinweg

`SymRegionWrap.*` prüft beide Byte-Reihenfolgen, vollständige und partielle Zugriffe über den relativen Nullpunkt, Eingabeherkünfte ohne Umlauf, kopierte Epochen unbekannter Werte, exakt ausreichende und unzureichende Erhaltungsbudgets sowie die Ablehnung absoluter/skalarer Bereichsüberläufe. `InterpreterSpecialization.NativeReturnCellProofKeepsStoreAcrossRelativeZero` stellt die symbolische Adressform `base - 1` sicher und prüft mit einem unabhängigen Byte-Oracle die vollständigen STORE-Effekte, nativen Rücksprungadresswörter und den wiederhergestellten Stackzustand. Ohne Trennungsbeweis oder Eingangsausrichtung muss die Spezialisierung weiterhin ablehnen.

### Vergleich und Austausch im sequenziellen Raw-Modell

`SymAtomicCmpxchg.*` und `InterpreterSpecialization.AtomicCmpxchg*` decken vollständige Bytewirkungen, beide Byteordnungen, Herkunft des alten Werts, Ein-/Ausgabe-Aliase, native Rückkehrzellen, Invalidierung affiner Spills über Zusammenführungen, erhaltene geordnete Vorkommen, strikte Zulassung sowie exakte und zu kleine Budgets ab. Das unabhängige Byte-Orakel modelliert nur sequenzielle Werte. Quelltextkapselungs-Kontrollen verlangen die Ablehnung geordneter Speicherzugriffe für initialisierte, strukturell gültige Graphen. Unabhängige LowIR-Kontrollen binden zusätzlich vollständige Sidecars und verlangen ein Ergebnis ohne Zertifikat. Native Binärbeweise können früher scheitern, weil für LOCK CMPXCHG keine vollständige Prüfung architektonisch undefinierter Ausgaben vorliegt. Raw-Belegprüfungen binden Speicherordnung, Adressraum und neuen Operanden. Vor einer Supportaussage sind die zuständigen Tests auszuführen; Testdefinitionen allein sind kein Ausführungsnachweis.

`InterpreterSpecialization.WideIntegerConcat*` prüft beide Byte-Reihenfolgen, obere/untere Wörter, Konstanten, überlappende Ausgaben, Steuerung über das obere Wort, ungültige Formen, exakte/zu kleine Budgets und unbekannte Speicherherkunft. Strikt dekodiertes `PSHUFD` prüft alle 256 Selektoren mit gleichen/verschiedenen Zielen gegen unabhängige 32-Bit-Wortindizes. `BinaryLowIRRefinement.WideIntegerConcatCompletesOnlyRawRecovery` behält die Ablehnungen nativer Beweise und beider Quelllayouts bei.

`InterpreterSpecialization.Timestamp*` prüft vollständige Werte, gelöschte obere Hälften, unabhängige/wiederholte Lesezugriffe, erhaltene Register/Flags/Speicher/Segmentbasen, exakte/zu kleine Budgets, fehlerhafte Bündel und unbekannte Adressherkunft mit unabhängigen Wert- und Zustandsorakeln. `BinaryLowIRRefinement.TimestampCompletesOnlyRawRecovery` prüft rohe Fertigstellung und fortgesetzte native/Quelllayout-Ablehnung. Bestehende `X86_64_Intrinsics`-Uhrfälle prüfen Quellausgabe und sämtliche Bits eines einzelnen Lesezugriffs; `DriverTimestamp` prüft separat die gemeinsame Unicorn/KVM/WHP-Uhrpolitik, Zustandserhalt, Budgets und Überlauf. Plattformbedingtes Überspringen belegt keine Backend-Ausführung.

`KernelObjectTests.cpp` prüft bekannte IDs, Typ-/Rechte-/Modusfehler, Handleinformationen, fehlerhafte Ausgaben ohne Referenzverlust, Unload und Stackwiederverwendung. `DriverObjectTests.cpp` führt das originale `driver_objects.c` mit verfügbaren Unicorn/KVM/WHP/HVF, Legacy/CheckedX64, bevorzugten/rebasierten Adressen und kooperativem/17-Instruktions-Scheduling aus. `NativeDriverTests.def` verlangt KVM/WHP-Ergebnisse; fehlende oder übersprungene Ausführung bleibt eine Lücke. ACLs, allgemeines Prozessende und nicht implementierte Objektklassen sind damit nicht nachgewiesen.

`KernelDataExportTests.cpp` prüft Identitäten, Verfügbarkeit, Thunkkapazität, Schreibschutz/NX, undeklarierte Debuggerbytes und Recovery. `DriverDataExportTests.cpp` führt den originalen `driver_data.c` mit echten DATA-Imports auf verfügbaren Backends, beiden Verträgen und Ladebasen aus, einschließlich Entladen und verbotenen Schreib-/Ausführungszugriffen. Szenario-, C-ABI-, CLI- und Python-Tests prüfen boolesche Deklarationen und Berichte. `NativeDriverTests.def` verlangt native Ergebnisse; nicht verfügbare Backends bleiben Lücken.

`KernelNamespaceTests.cpp` prüft Typen, relative Namen/Großschreibung, Größen/Kopierreihenfolge, Endbytes, Rechte, Prozessbereich, IRQL, fehlerhafte Ausgaben und gehaltene Löschung. `DriverNamespaceTests.cpp` führt das originale `driver_namespace.c` auf verfügbaren Unicorn/KVM/WHP/HVF mit beiden Verträgen, Ladeadressen und kooperativer/17-Instruktionsplanung aus. `NativeDriverTests.def` verlangt native Ergebnisse. Fehlende Backends und Informations-/Sicherheitsklassen bleiben Abdeckungslücken.

`KernelRegistryTests.cpp` prüft getrennte Öffnungen, Typreferenzen, kopierte Handles, Prozessbereich, generische Rechte, gelöschte Identitäten, kurze/ungerade Namenspuffer, überlappende Ausgaben und Fehler-/Kapazitätsgrenzen. `DriverRegistryObjectTests.cpp` führt das Original `fixtures/driver_registry_objects.c` auf verfügbaren Backends, beiden Verträgen und Ladeadressen mit kooperativer und 17-Instruktions-Planung aus. Ein absichtliches Zeigerleck prüft die Entladeverweigerung; native Abnahme verlangt alle vier Vertrags-/Adresskombinationen.

`KernelDeviceStackTests.cpp` prüft aktuelle Nachbarn, geliehene und gehaltene Ergebnisse, Löschung von Eingabe oder Nachbar, Freigabe der letzten Referenz, ungültige oder beschädigte Eingaben und IRQL-Ablehnung vor dem Referenzieren. Das eigene `fixtures/driver_device_queries.c` läuft über `DriverDeviceQueryTests.cpp` auf verfügbaren Backends mit beiden Verträgen und Ladeadressen sowie kooperativer und präemptiver Planung mit 17 Instruktionen. Es prüft Speicherhaltung nach detach/delete und verweigertes Entladen bei einer absichtlich verlorenen Referenz; alle vier nativen Vertrags-/Adresskombinationen sind erforderlich.

KernelFileObjectTests und KernelObjectTests prüfen Typen, Nullausgabe bei Typfehlern, generische Rechte, Prozess- und Gesamtzähler, Callback-Neuöffnungsverbot, Wiederholung nach ungültiger Route, Prozessende, wiederholten Cleanup und ausstehenden CLOSE. KernelPnpRequestTests erhält verzögerte Providerantworten; KernelFrameworkFileTests prüft Prozesskontext und LOCK_CONTROL. driver_file_objects.c behält Zeigerprüfungen. Das originale driver_file_handles.c prüft synchrone/ausstehende CLEANUP/CLOSE, Systemworker, Nummernwiederverwendung und Lecks bei kooperativer Ausführung und 17-Befehls-Planung. Das echte WDK driver_kmdf_file_handles.c prüft Systemprozess, wiederhergestellten Aufrufer und Cleanup ohne Callback; NEVERD_KMDF_FILE_HANDLES_FIXTURE und optional NEVERD_KMDF_FILE_HANDLES_CFG_FIXTURE konfigurieren. DriverFileObjectTests nutzt verfügbare Backends, beide Verträge und Basen. Fehlende WDK-Artefakte und Backends werden sichtbar übersprungen. Dateisystemoperationen benötigen eigene Abdeckung. KMDF-CLEANUP/CLOSE aus dem Szenario bewahren Prozess, Thread und PreviousMode des Aufrufers; Modus F schließt ein gehaltenes Handle im CLEANUP einer anderen Datei und prüft den wiederhergestellten Kontext.

`KernelFileQueryTests.cpp` prüft ausgeliehene/native Pakete, sofortige/ausstehende Veröffentlichung, IOSB-Aliase und Padding, Worker-Abschluss, Lebensdauer, lokale Klassen, Handle-Bereich und IRQL/APC-Ablehnung. `KernelFileCreateTests.cpp` und `DriverFileSystemTests.cpp` prüfen sechs Metadatenklassen und Namensgrenzen. driver_file_queries.c und driver_filesystem_provider.c laufen über verfügbare Backends, Verträge, Basisadressen und kooperatives/17-Instruktionen-Scheduling. Echte normale/CFG-Bilder nutzen NEVERD_WDM_FILE_QUERIES_FIXTURE, NEVERD_WDM_FILE_QUERIES_CFG_FIXTURE, NEVERD_WDM_FILESYSTEM_FIXTURE und NEVERD_WDM_FILESYSTEM_CFG_FIXTURE. C API/CLI/Python prüfen Quellen, vollständige Ausgaben und behandelte Fehler. Begrenzte Originalkernelproben umfassen 156 Erzeuger- und 18 Abschlussfälle mit expliziten Allokator-, Dispatch- und Benachrichtigungsgrenzen, ohne vollständiges Dateisystem oder allgemeine APC zu bestätigen. Fehlende Hardware/Bilder bleiben sichtbare Skips.

`KernelIRPStack.ReadWriteCompletionPreservesStoredFilePosition` prüft eine unabhängige 64-Bit-Position für synchrone/asynchrone Dateien, sofortigen/ausstehenden Abschluss, Erfolg/Warnung/Fehler und Längen null/ungleich null. Die echten normalen/CFG-WDK-Images von `driver_wdm_stack.c` lesen die Position zwischen READ, WRITE und Cleanup erneut.

Die Regressionen prüfen Ausgabegrenzen, NULL, verschachtelte Wiederherstellung, ungültigen IRQL und Importidentität. Das originale `driver_processors.c` führt Einstieg, Worker und DPC mit verschiedenen Backends, Verträgen, Basisadressen und Zeitplanungen aus. `NEVERD_WDM_PROCESSORS_FIXTURE` und `NEVERD_WDM_PROCESSORS_CFG_FIXTURE` ergänzen echte normale/CFG-WDK-Images für Inlineaufrufe und dynamische Exporte; fehlende Backends werden explizit übersprungen.

Die Regressionen prüfen volle Wertebreite, unterstützte Basen, Kapazitätsgrenzen, die Reihenfolge bei ungültigen Basen, Ausgabealiasing und Fehler am Abschlusszeichen. Das eigene `driver_int64_strings.c` prüft Importe und dynamische Auflösung bei Einstieg, Arbeitsroutine und Entladen sowie die Ablehnung bei erhöhtem IRQL. Die optionalen `NEVERD_WDM_INT64_STRINGS_FIXTURE` und `NEVERD_WDM_INT64_STRINGS_CFG_FIXTURE` prüfen echte WDK-Abbilder mit und ohne CFG über verfügbare Backends, Verträge, Ladeadressen und Zeitplanung.


`DriverFileSystemTests.cpp` / `KernelFileSystemTests.cpp` / `DriverFileSystemExecutionTests.cpp` / `DriverScenarioPublic` / `test_driver_filesystem_integration.py`: Das optionale `filesystem` deklariert Sitzungsvolumes vor DriverEntry. Ohne das Feld ist die Verfügbarkeit unbekannt; `[]` bedeutet keine Anbieter. Jedes Volume benötigt eine deklarierte Wurzel. `ZwOpenFile` sowie FILE_OPEN mit `ZwCreateFile` und `IoCreateFile` öffnen vorhandene Knoten über echte CREATE-Anfragen. Namen, Gastfilter, Freigaben, Handles und Referenzen nutzen gemeinsame Eigentümer, ohne Hostdateien abzufragen. Erstellen, Überschreiben, Löschen, Metadatenabfragen, Daten-I/O, Auflistungen und Steueroperationen bleiben ununterstützt. `NEVERD_WDM_FILESYSTEM_FIXTURE`, `NEVERD_WDM_FILESYSTEM_CFG_FIXTURE`, `NEVERD_TEST_DRIVER_FILESYSTEM_FIXTURE`, `NEVERD_TEST_LIBNEVERD`. `UnusedFileSystemDeclarationCannotAuthorizeRuntimeRecovery`: `configuration.filesystem` behält die Eingabe; `filesystem` meldet unabhängigen Zustand mit `device_object` und `end_of_file`, auch nach Ausführungs- oder Projektionsfehlern. `devices` enthält nur Gastgeräte. Sitzungsvolumes dürfen das Entladen überleben, Gastreferenzen, Handles, Anbindungen und IRPs müssen enden. Jede Deklaration, auch `[]`, verhindert ausführbare UNPACK-Wiederherstellung. Altes Unicorn kann abgewiesene Schreibzugriffe vor dem Stopp ausführen; der Projektionsfehler bleibt diagnostiziert.

`KernelFileCreateTests.cpp` prüft Paketlebensdauer, Freigabekonflikte, gehaltene relative Wurzeln, expliziten Abbruch, Fehler/Aliasausgaben und Entladen. `DriverFileSystemExecutionTests.cpp` führt Original- und echte WDK-Normal/CFG-Aufrufer für beide Verträge, Basen und verfügbare Backends mit beiden Schedulingarten aus. C API/CLI/Python erhalten dieselben Identitäten. Fehlende Backends bleiben Skips; Ereignis-/APC-Grenzen der Proben beweisen keinen vollständigen Kernel.

`KernelFileTransfers` prüft Pakete, spezielle Offsets, Append-Handles, Länge null, Ausgabealiase, sofortige/ausstehende Zustände, Worker-Benachrichtigung und Ablehnung vor Veröffentlichung. `OriginalBufferedNativeFileTransfers` / `GenuineWdkBufferedNativeFileTransfers` führen importierte und dynamisch aufgelöste Aufrufe über Backends, Verträge, Ladeadressen und Zeitplanung aus. Echte WDK-Normal-/CFG-Images verwenden `NEVERD_WDM_FILE_TRANSFERS_FIXTURE` / `NEVERD_WDM_FILE_TRANSFERS_CFG_FIXTURE`; nicht verfügbare Backends werden ausdrücklich übersprungen.

CRT-Regressionen prüfen alle Bytes/UTF-16-Einheiten, Argumentbreiten, unveränderte schreibgeschützte Puffer, Zähler/Kapazität, Kürzung, NULL-Reihenfolge, sequenzielle Aliase, Teilfehler, freigegebenen Speicher und Scan-Grenzen. Das Original `driver_crt_strings.c` und echte WDK-Images über `NEVERD_WDM_CRT_STRINGS_FIXTURE`/`NEVERD_WDM_CRT_STRINGS_CFG_FIXTURE` testen Imports und dynamische Auflösung in Eintritt, Worker und Unload, mit passivem/Dispatch-IRQL und beiden Scheduling-Modi. Das native Gate verlangt beide Verträge und Ladebasen; nicht verfügbare Backends bleiben explizite Skips.

Speicherregressionen unterscheiden vollständiges Löschen und unveränderte Fehlerausgabe, Länge null, volle Breite, Überlappung, ausgelagerten/freigegebenen/schreibgeschützten Speicher und Grenzen. Headerproben prüfen vorzeichenlose DOS-Offsets, 64-Bit-Ergebnisse, Adressgrenzen, reine Signaturannahme und Fehlerreihenfolge. Eigene und echte WDK-CRT-Fixtures führen Import- und dynamische Aufrufe in Eintritt, Worker und Entladen aus. Für das in den fixierten WDK-Headern fehlende `RtlImageNtHeader` dient eine lokale ABI-Deklaration; PE-Typen und Importbibliotheken stammen aus dem echten Kit. Die fixierte WDK-ntoskrnl.lib bindet memcpy_s als Gastcode ein. Die eigene Fixture prüft den direkten Import; beide prüfen dynamische Exportaufrufe.

`KernelFileTransfers` prüft Flush-Rechte, pufferlose Pakete für alle Geräteübertragungsarten, vollständige Information-Werte, Veröffentlichung/Freigabe vor Dispatch-Rückkehr, Worker-/Guarded-Benachrichtigung und Ablehnung asynchroner/paralleler Aufrufe. Die bestehenden originalen und echten WDK-Fixtures führen importierte und dynamisch aufgelöste Flushes mit Benutzer-IOSB und neun Fertigstellungsfällen aus.

`KernelFileTransfers` prüft Steuerpakete, ULONG-Kürzung, Null-Längen-Zeiger, kopierte schreibgeschützte Eingaben, IOSB-Aliase, getrennte Ein-/Ausgabegrenzen, Verzögerung und Aufruferbenachrichtigung. Bestehende originale und echte WDK-Transfer-Fixtures testen beide APIs über Import und dynamische Suche mit vier Pufferformen und neun Abschlussmodi.

Regressionen prüfen Ausgabealiasing, schmale Argumente, ungerade Längen, erneut gelesene Deskriptoren, Überlappung, Allokationsfehler, NLS-Ablehnung, Teilfehler, Lebensdauer und APC_LEVEL. Zeittests prüfen Subsekunden an den Epochengrenzen, ULONG-Grenzen und Ein-/Ausgabealiasing. Eigene und echte WDK-CRT-Treiber rufen alle drei Exporte statisch und dynamisch beim Eintritt, im Worker und beim Entladen auf; der Eintritt prüft auch APC_LEVEL.

Regressionen durchlaufen ASCII und den NLS-unabhängigen breiten Bereich mit hohen Trägerbits und prüfen Ablehnung, Argumentzahl und APC_LEVEL. Eigene und echte WDK-CRT-Treiber prüfen alle drei statischen und dynamischen Aufrufe bei Eintritt, Worker und Entladen. Das wint_t aus WDK-ctype.h bleibt 16 Bit breit; erwartete Werte stammen nicht aus dem Host-CRT.

Debug-Regressionen prüfen die short-Regeln aller drei Eingänge, Lesen an Gastgrenzen, wachsende Puffer, eingebettete NUL, freigegebene oder ausgelagerte Eingaben und IRQL. Originale und echte WDK-CRT-Fixtures führen statische und dynamische Aufrufe in Einstieg, Worker und Entladen mit beiden Ablaufmodi, Verträgen und Ladebasen aus. Nicht verfügbare Backends bleiben ausdrückliche Überspringungen.

Regressionen prüfen gehaltene/veraltete Threads, Anforderer-/Worker-/angeheftete Kontexte, Modulgrenzen und Lücken, Ausgabebreite, Lebensdauer, Rechte und IRQL. Originale und echte WDK-CRT-Fixtures führen statische/dynamische KernelMode-Abfragen aus; Datei-Fixtures prüfen auch UserMode-Aufrufer. Beide WDK-Familien enthalten Normal/CFG. Zwei fehlende Deklarationen verwenden lokale ABIs mit echten Kit-Typen. Nicht verfügbare Backends bleiben übersprungen.

Die alten MDL-Aufrufe werden mit echten statischen und dynamischen WDK-Aufrufen, angehängten Workern, eigenen Abbild-Aliasen, Modusverengung, IRQL, Prozesswechseln, abgelaufenen Deskriptoren, doppelten Kernelabbildungen und injiziertem Budgetmangel geprüft. Kernelmangel erhält das MDL und meldet den nicht modellierten Bugcheck; Benutzermangel bleibt abfangbar. Originale Wrapper-Proben prüfen nur Argumentweitergabe und Rückgabebreite, nicht den injizierten Mapper.

`KernelFileQueries.BaseDevice*` prüft die Basis unter zwei Filtern, unveränderte Referenzen, ignorierte Erweiterungszeiger, Erstellungs-/Bereinigungs-/Schließzustände, beschädigte Datensätze und IRQL. Eigene und echte WDK-Fixtures in Normal-/CFG-Varianten führen statische und dynamische Abfragen in der bestehenden Matrix aus Backends, Relokation, Planung und Aufruferlebensdauer aus.

`KernelDevicePointer.*` prüft Referenzen, Ausgabeüberlappung, fehlgeschlagene und informative Öffnungen, Thread-/APC-Wiederherstellung nach wartendem CREATE/CLEANUP, Argumentbreiten, Freigabe, Schreibreihenfolge bei Ausgabefehlern, Zulassung und Entladen. Original- und WDK-Fixture ergänzen sofortige Ausführung, CREATE/CLEANUP durch Worker sowie bis zum Entladen gehaltene Zeiger und dynamische Auflösung. `KernelFileSystemExecution.*OpensExistingFiles` prüft beide Ablaufmodelle, Verträge und Ladebasen; nicht verfügbare Backends werden ausdrücklich übersprungen.

`KernelMDLUserMapping.ProcessProbe*` prüft Argumentbreiten, Normalisierung, IRQL, lebende Prozesse, Threadidentität während der Prüfung, verschachtelte Wiederherstellung, Wiederholung nach Fehlern und Speicherknappheit beim Anhängen/Wiederherstellen. Das echte WDK-Fixture führt statische/dynamische Aufrufe, einen Ausnahmehandler nach dem Lösen, Aufrufe im angehängten selben Prozess, gemeinsame physische Seiten und vollständige Sperrfreigabe aus. Proben des Originalwrappers und seiner Terminierungsfunktion belegen Kontrollfluss zwischen ausdrücklich injizierten Hilfsaufrufen, nicht das gesamte Kernelverhalten.

`KernelObjects.FilterVersion*` prüft exakte Argumentzahl, vom Aufrufer unabhängige Ergebnisse und die IRQL-Grenze auslagerbaren Codes. Eigene und echte WDK-CRT-Fixtures verwenden statische Imports und dynamische Auflösung in Einstieg, Worker und Entladen, beim Einstieg auch auf APC_LEVEL. Proben der beiden Originalinstruktionen prüfen das vollständige Rückgaberegister und fehlende Datenspeichereffekte ohne injizierte Hilfsfunktionen.

`KernelPhysicalRAM.AddressQueries*` und `KernelMDLUserMapping.PhysicalQuery*` prüfen Aliase, Adressraumidentität, VA-Wiederverwendung, Grenzen, vollständige PFNs, IRQL, Seitensperren und aufgehobene Abbildungen. Der echte WDK-Testtreiber vergleicht statische und dynamische `MmGetPhysicalAddress`-Aufrufe mit MDL-PFNs für schreibgeschützte Benutzeransichten, Systemansichten auf DPC-Ebene und Neuabbildungen bei fortbestehendem gesperrtem Alias.

`KernelMDLUserMapping.PrimaryQuery*` prüft exakte Bytes, benachbarte Eigentümer auf einer Seite, vollständige Adressen, IRQL, Freigabe, Aliase, ersetzte Abbildungen, Leseschutz und nicht unterstützte primäre Projektionen. Der echte WDK-Testtreiber führt statische und dynamische `MmGetVirtualForPhysical`-Aufrufe aus, einschließlich einer Systemansicht auf DPC-Ebene und einer Benutzer-VA-Neuzuordnung mit fortbestehendem gesperrtem Alias.

`DriverKernelModel.StackQuery*` / `KernelMDLUserMapping.StackQuery*` prüft Aktivierung, Freigabe, verschachtelte Wiederherstellung, vollständige Grenzen, IRQL, Prozessanbindung, Ausgabeüberlappung und Fehlerreihenfolge High vor Low. Original- und echte WDK-CRT-Fixtures rufen beide APIs statisch und dynamisch bei Einstieg, Worker und Entladen auf, prüfen eine lokale Variable und fragen Grenzen bei DISPATCH_LEVEL ab.

`KernelMDLChain.CreateMdl*` und `KernelMDLUserMapping.CreateMdl*` prüfen Breiten, erhaltene Bytes, leere/überlaufende Metadaten, Pool-/Stacklebensdauer, Sperren, Aliase, fehlgeschlagene Freigaben, partielle Wiederverwendung, IRP-Ablehnung und IRQL. Der echte WDK-Treiber prüft statische/dynamische Erstellung, eigenen Pool, inneren Pool und Stackspeicher, physische Identität und vollständiges Entladen. Originalproben trennen Feldschreibzugriffe, Flagübersetzung und injizierte Allokations-/Freigabegrenzen.

`KernelMDLChain.AdvanceMdl*` und `KernelMDLUserMapping.AdvanceMdl*` prüfen wiederholte Seitengrenzen, PFN-Endmarkierung, Endablehnung, Argumentbreite, vollständige Abbildungen, fehlgeschlagenen Abbau, Rechte und Lebensdauer. Echtes WDK führt statische/dynamische Aufrufe und beide Abbauwege aus. Originalproben trennen vollständige Routine, Wiederherstellungshelfer und Unmap-Wrapper mit injizierten Abbildungstyp-/PTE-Freigabegrenzen; die OS-Seitendatenbank wird nicht ausgeführt.

## Klassische ausgerichtete Vektortransfers

`NeverDX86LegacyAlignedMoveTests` prüft die Registererhaltung von MOVDQA, MOVAPS und MOVAPD sowie die 16-Byte-Ausrichtung vor Speicherzugriffen, einschließlich Überlauf bei begrenzter Adressbreite und expliziter FS/GS-Basen. Nicht ausgerichtete Zugriffe müssen Zielregister und Speicher unverändert lassen. Fehlende Segmentbasen werden weiterhin abgelehnt. Dies prüft Lifting und Emulator; die exakte symbolische Wiederherstellung muss die erhaltene Ausrichtungsbedingung gesondert beweisen.

```sh
cmake --build build-release --target NeverDX86LegacyAlignedMoveTests
ctest --test-dir build-release -L '^NeverDX86LegacyAlignedMoveTests$' --output-on-failure
```

`InterpreterSpecialization.Alignment*` und `PEFixedImageTest.RawAlign*` prüfen unabhängige Restwertbeweise, ungültige Eingaben, genaue Budgets, erhaltene native Prüfungen und manipulierte Nachweise. Audits klassischer Verschiebeoperationen binden den gesamten Operationsbereich einschließlich Ausrichtungsprüfung, bescheinigen aber nicht den Erhalt opaker Zustandsbanken.

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests NeverDPEFixedImageTests NeverDX86UndefinedEffectsTests
ctest --test-dir build-release -L "^(NeverDInterpreterSpecializationTests|NeverDPEFixedImageTests|NeverDX86UndefinedEffectsTests)$" --output-on-failure
```

`NeverDX86NativeStateAuditTests` prüft native Bytes und Decoderdetails für kanonisches x64 REP MOVSQ und klassisches LDMXCSR streng. Die Nachweise undefinierter Ausgaben binden den vollständigen Operationsbereich. Unabhängige Orakel für vollständigen Zustand und geordnetes Kopieren prüfen Überlappung, Nullanzahl, Adressformen, fehlende Bytes und reservierte Bits. Die bisherigen Beweise für Anzahl, Richtung, Speicher, wirksame MXCSR-Maske und Gültigkeit bleiben erforderlich. Dieses Audit bescheinigt weder Wiederaufnahme nach einem Teilfehler noch Erhalt opaker Zustände.

```sh
cmake --build build-release --target NeverDX86NativeStateAuditTests
ctest --test-dir build-release -L "^NeverDX86NativeStateAuditTests$" --output-on-failure
```

`NeverDX86ScalarMoveAuditTests` prüft die strenge Zulassung klassischer MOVSS/MOVSD anhand der Originalbytes und vollständigen Decoderdetails. Unabhängige Byte-Orakel prüfen Registermischung, Nullerweiterung bei Speicherladungen, skalare Schreibzugriffe, alle Ausrichtungsreste, 16/32/64-Bit-Adressierung und vollständigen Erhalt von Registern, Flags und MXCSR. Fehlende Speicherbytes oder Schreibzugriffe auf schreibgeschützten Speicher dürfen keine architektonischen Ausgaben übernehmen. String-, VEX/EVEX-, segmentierte und fehlerhafte Formen bleiben abgelehnt; die Nachweise binden alle Operationen, ohne den Erhalt opaker Zustände zu bescheinigen.

```sh
cmake --build build-release --target NeverDX86ScalarMoveAuditTests NeverDX86LegacyAlignedMoveTests NeverDX86UndefinedEffectsTests
ctest --test-dir build-release -L "^(NeverDX86ScalarMoveAuditTests|NeverDX86LegacyAlignedMoveTests|NeverDX86UndefinedEffectsTests)$" --output-on-failure
```

`SymStorePreservation.Independent*`, `RepeatedRegionsFormAUnionAndChargeEveryScan`, `EmptyIndependentRegionsKeepOriginalWorkAccounting` und `ExecutorUsesIndependentRegionsForOnlyOneStore` prüfen einen STORE mit zwei oder drei unabhängigen aktuellen Wurzeln, beide Byteordnungen, Teilbereiche über den relativen Nullpunkt, überlappende Deklarationen, ungeschützte Lücken und unbekannte Generationen nach dem Schreiben. Exakte und um eine Einheit zu kleine Budgets prüfen die vollständigen Kosten; fehlende Regionen dürfen nicht materialisiert werden. Ungültige oder außerhalb liegende Referenzen, schmale oder konstante Basen, leere oder umgekehrte Intervalle sowie die eigene Basis des Schreibzugriffs werden ohne Zustandsänderung abgelehnt. Leere `AdditionalRegions` erhalten die bisherigen Kosten der Einzelwurzel und von `AdditionalRanges`. Der Ausführer prüft das einzelne Ereignis, benachbarte Bytes derselben Region und die Invalidierung durch einen späteren gewöhnlichen Schreibzugriff. `SymAtomicCmpxchg.IndependentRegionsShareTheWritePhaseAndItsBudget` prüft passende und abweichende Vergleiche, exakte und unzureichende Budgets, zwei geschützte Wurzeln, die Ausgabe des alten Werts und den bestehenden konservativen Ablehnungspfad.

`NeverDSymConditionalStoreTests` prüft bedingte Effekte auf aktuelle Bytes unabhängiger Wurzeln, 128-Bit-Schreibvorgänge, beide Byte-Reihenfolgen, Teilüberlappungen, den Umlauf relativer symbolischer Koordinaten und absoluten Speicher. Geprüft werden außerdem Nachbarbytes der eigenen Bank, aktuelle gegenüber historischen Fakten, unbekannte Epochen nach dem Schreiben, nicht unterstützte Eingaben sowie exakte und um eine Einheit zu kleine Budgets. Ausführungsfälle umfassen STORE mit bestehender Segmentbasisbehandlung, Erfolg und Ablehnung strikter CMPXCHG, verzögerte Veröffentlichung von Old, havoc und Invalid bei anderen Operationen. Die Standardverhalten gewöhnlicher Schreibvorgänge und Erhaltungsverträge bleiben unverändert. Die Tests validieren begrenzte Speichersemantik, keine Zugriffsrechte oder vollständige Programmrekonstruktion.


`AbsoluteMemoryFactsTests.inc` prüft absolute Ziele über Knoten, widersprüchliche Zusammenführungen, teilweise und unbekannte Alias-Schreibzugriffe sowie Stack-Trennung für beide Bytefolgen mit und ohne Verkettung. Ein unabhängiger Byte-Executor prüft Effekte; fehlende Ausschlüsse und knappe Budgets müssen ablehnen. `SymStorePreservation.Absolute*` und `SymState.AbsoluteProjections*` prüfen koexistierende Banken, hohe Adressen, reine begrenzte Projektionen, unbekannte Epochen und transaktionale Ablehnung. `NeverDInterpreterSpecializationTests`, `NeverDSymbolicIntegerIntrinsicTests`, `NeverDSymMemoryKnowledgeTests` und danach native Unabhängigkeit sowie LowIR-Verfeinerung ausführen.

Regressionen für absoluten Speicher prüfen begrenzte zusammenhängende und verstreute Gruppen, vollständige Schreib-/Zellgrenzen sowie konkrete Alias-Gegenbeispiele an beiden Frame- und Ausschlussgrenzen. Geordnete und umgekehrte Bereichsvereinigungen müssen dieselben Zellen erhalten, Lücken verwerfen und bei einer fehlenden Arbeitseinheit den Zustand unverändert lassen.

## Regressionstests für endliche RAM-Lesezugriffe

`LowIRReadTrace.*` prüft gemeinsame Eingaben, beide Byte-Reihenfolgen, Änderungen von Adresse/Breite/Reihenfolge/Anzahl, alle Zweige, teilweise Überlappung früherer Frame-Schreibzugriffe, undefinierte Werte, unabhängige wiederholte Zugriffe, exakte/zu kleine Budgets und Schleifenverweigerung. `OriginalBinaryUndefinedIndependence.OrdinaryReadTracePreservesNativeMemoryInputs` prüft ursprüngliche native Unabhängigkeit, native LowIR-Verfeinerung und die Ablehnung eines geänderten Zeigers. `NeverDLowIRRefinementTests`, `NeverDLowIRUndefinedIndependenceTests`, `NeverDOriginalBinaryUndefinedIndependenceTests`, `NeverDPEFixedImageTests` und `NeverDInterpreterSpecializationTests` ausführen. Diese Beweise allein qualifizieren weder kompiliertes C noch konkrete CPU-Backends.

## Regressionstests nativer skalarer Abfragen

`NativeQueryTrace.*` prüft unabhängige Antworten, vier Nullerweiterungen, Änderungen von Selektoren/Anzahl/Reihenfolge, undefinierte Selektoren, verschachtelte Lesezugriffe, alle Zweige, erhaltenen Zustand, Codierungs-/Detailaudits, fehlerhafte Bündel, Budgets, Digests und statische/induktive Ablehnung. `NativeScalarQueryDescriptor.*` prüft generische Beschreibungsgrenzen und tatsächliche Belegbindung.

`NeverDOriginalBinaryUndefinedIndependenceTests`, `NeverDInterpreterSpecializationTests`, `NeverDLowIRRefinementTests`, `NeverDLowIRUndefinedIndependenceTests`, `NeverDX86NativeStateAuditTests`, `NeverDX86LegacyGSRecoveryTests`, `NeverDPEFixedImageTests`.

Die Regressionen prüfen die exakte Erhaltung von CC, F1 und CD 03, die Ablehnung gefälschter Metadaten, Regionsausgänge ohne Nachfolger sowie exakte und zu kleine Operationsbudgets. Ein gewöhnlich bereitgestellter Trap darf nicht zu einem RETURN in derselben oder der folgenden Instruktion weiterlaufen. Der Originalbeweis umfasst auch UD2: Unerreichbare Traps behalten das Missing-Audit; abweichende Ausnahmeidentität, Bytes oder Audits werden abgelehnt. Erreichbare Traps verhindern den Originalbeweis und die Verfeinerung gegen einen gewöhnlich zurückkehrenden Kandidaten.

`NeverDInterpreterSpecializationTests`, `NeverDPEFixedImageTests`, `NeverDOriginalBinaryUndefinedIndependenceTests`, `NeverDLowIRRefinementTests`.

`NativeStackControl.TrapExit*` · `InterpreterSpecialization.TrapIntrinsicsCannotFallThroughWithoutAnAuthenticatedExit` · `PEFixedImageTest.MachineTrapExitEndsRegionAtDeclaredTrap` · `PEFixedImageTest.UndeclaredTrapRemainsUnsupported` · `PEFixedImageTest.MachineTrapExitRejectsNonTrapInstruction` · `NativeUndefinedIndependence.UntakenTrapStillRequiresExactEvidence`.

Diese Prüfungen qualifizieren deklarierte Regionen, nicht die Ausnahmebehandlung oder Quell-/Native-Äquivalenz.


`PEFixedImageTest.EntryObject*`, `InterpreterSpecialization.EntryObject*`, `FrameOffsets.DisjointRelativeRangesKeepEqualRootsAndBoundaryBytes`.

Die Eingangsobjektregressionen prüfen symbolische Kopien, Sicherungen und kopierte Slots, Laufzeitschleifen, verlorene Bindungen und Zusammenführungen, Objektaliasse, vollständige breite/atomare/segmentierte Schreibzugriffe, relative Grenzen, ungültige Deklarationen, manipulierte Belege sowie exakte und unzureichende Vorbereitungs- und Operationsbudgets sowie erschöpfte Knotenbudgets. Unabhängige Byte-/Registerorakel prüfen erhaltene Effekte. Die zuständigen Suites und raw-Konsumentenablehnungen gemeinsam ausführen; dies liefert kein Zertifikat für ganze Funktionen, kompiliertes C oder eine gewöhnliche ABI.

`NeverDInterpreterSpecializationTests`, `NeverDPEFixedImageTests`, `NeverDRawEnvironmentRequirementTests`, `NeverDOriginalBinaryUndefinedIndependenceTests`, `NeverDLowIRRefinementTests`.

Führen Sie `NeverDObjCSourceCallTests` (`SourceFrameAnalysis.*`) und `NeverDCSourcePresentationTests` für Byte-Herkunft, Pfadübereinstimmung, überschriebene oder entwichene Speicherbereiche, unveränderliche Image-Bereiche, Budgets, Kodierungen und Metadatengrenzen aus. `NeverDWorkerStackText` kompiliert öffentliche x64/ARM64-PE-Fälle über beide C-Pfade, einschließlich unbekannter Überschreibungen. Clang und lld-link sind erforderlich; native Windows-Ausführung wird separat geprüft.

<!-- i18n-section: interactive-byte-patches -->
## Zuständigkeit für interaktive Patches

`NeverDBytePatchTests` prüft die Byteüberlagerung, die Ablehnung von Relokationen, PC-relative Assemblierung sowie PE-Signatur und Prüfsumme. `NeverDWorkerPatch` prüft Vorschau, veraltete Eingaben, Rückgängig und Wiederholen, gemeinsame Wiederherstellung, schreibgeschützte Replikate, erneutes Öffnen und exklusiven Export.

`NeverDWorkerNativePatch` wiederholt diese Prüfungen mit dem echten SDK. `NeverDWorkerSourceBytePatch` prüft unter Linux Quelltextnachweise, isolierte Vorschauen, Stackframe-Symbole und den nativen Funktionsersatz in C.

`NeverDGuiPatchKeys` prüft alle zweiten Tasten über die Tastenkürzel-Verarbeitung von `QWindow`, einschließlich ladender Menüs, Abbruch und Editoreingabe. `NeverDGuiNativePatchKeys` prüft echte SDK-Bytes, Hex, schwebende Fenster sowie Rückgängig, Wiederholen und Wiederherstellen. Derselbe Test unter X11 prüft auch die native Menüverarbeitung.

```sh
cmake --build build-gui --target neverd-gui-patch-tests
ctest --test-dir build-gui -R 'NeverDGui(Native)?PatchKeys$' --output-on-failure
NEVERD_PATCH_NATIVE_WORKER="$PWD/build-gui/bin/neverd-worker" \
  python3 tools/neverd-gui/tests/run_wayland_docking.py \
  build-gui/bin/neverd-gui-patch-tests
ctest --test-dir build-release/unittests/patch --output-on-failure
ctest --test-dir build-gui -R 'NeverDWorker(Patch|NativePatch|SourceBytePatch)$' --output-on-failure
PYTHONPATH=pluginsdk/python NEVERD_TEST_LIBNEVERD="$PWD/build-release/bin/libneverd.so" \
  NEVERD_TEST_CLI="$PWD/build-release/bin/neverd" \
  python3 -m unittest discover -s pluginsdk/python/tests -p test_byte_patch_integration.py -v
QT_QPA_PLATFORM=offscreen build-gui/bin/neverd-gui-unit-tests patchShortcutsDoNotTakeHexRaysKeys patchMenuUsesEngineChoicesAndFixedKeys cancelledPatchMenuDiscardsItsPendingKey
QT_QPA_PLATFORM=offscreen NEVERD_PATCH_NATIVE_WORKER="$PWD/build-gui/bin/neverd-worker" \
  build-gui/bin/neverd-gui-tests nativePatchValueAndLargePreview
```
