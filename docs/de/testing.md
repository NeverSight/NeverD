**Sprachen**: [English](../testing.md) | [简体中文](../zh-CN/testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](../ja/testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← Dokumentationsindex](README.md)

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

Die Adressregressionen prüfen Integer- und Pointer-PHI/select, beide Byteordnungen und Pointerbreiten, stabile und veränderliche Rückkanten, undef/freeze, Zyklen ohne Eingangsanker, benachbarte Arbeits- und Ausgabebudgets sowie Invalidierung bei reinen Adressänderungen. O0/O2-Schleifen vergleichen Rückgaben und jedes Pufferbyte mit einer unabhängigen Referenz pro Iteration, einschließlich einer wandernden Adresse, die nicht als konstant gelten darf.

Die numerischen Tests prüfen ganze und enthaltene Loads eines Schreibers, undef/poison ohne neue Snapshots, beide Byteordnungen, 32/64 Bit, modulare negative Offsets, partielle Überdeckung, Aliase zwischen Wurzeln und Allocas, werfende Aufrufe, Schleifen, benachbarte Budgetgrenzen und Analyseinvalidierung bei reiner Store-Löschung. O0/O2 vergleichen Original und Transformation mit einer unabhängigen Referenz für Rückgabewert und jedes Byte des aliasierenden Puffers.

`NeverDMedMutableSourceTests` und `NeverDLLVMCValueTests` führen unabhängige Schleifen, umgeordnete Blöcke, Rückkanten zum Einstieg, Laufzeit-Stackarithmetik, frühere Lesezugriffe, Verzweigungszusammenführungen, partielle Aliase, Wahrheitswerte und Bitzählungen einschließlich Null bei O0/O2 aus. Negativfälle weisen fehlerhafte Eingaben, abgeschnittene Ziele, mehrdeutige Träger und erschöpfte Budgets vor der Ausgabe zurück. Ein CLI-Fall oberhalb der SSA-Grenze verlangt ausführbares LLVMC und eine ausdrückliche Ablehnung durch HighC. Wiederholte Aktualisierungen und blockübergreifende Ketten gespeicherter Ausdrücke prüfen außerdem die Größe und Ausführung der C-Ausgabe.

Zusätzliche Regressionstests begrenzen private Lese- und Schreibzugriffe vor der LLVM-Promotion sowie die Größe der C-Ausgabe. Lange gemischte Rechenketten, umgeordnete SSA-Blöcke, überlappende Speicherzugriffe und Null-Rückgaben werden bei O0/O2 ausgeführt. Wichtige Fälle durchlaufen auch die tatsächliche LLVM-Optimierung; die Wiederverwendung des Emitters nach abgelehnter Modulerzeugung wird geprüft.

Regressionstests für zusammengesetzte Bedingungen prüfen bei O0/O2 Konjunktionen und Disjunktionen mit Gleichheit zu Konstanten ungleich null, vorzeichenlosen Vergleichen, vorzeichenbehafteten Vergleichen in beiden Operandenreihenfolgen, verbreiterten Booleschen Eingaben und allen Negationskombinationen. Die C-Ausgabe muss die gesamte Wahrheitstabelle erhalten und darf keinen fehlenden Nullvergleichsoperanden dereferenzieren. Ganzzahlige Adressspeicherungen prüfen ausgerichtete und unausgerichtete 32/64/128-Bit-Träger. Byte-Speicherarrays behalten explizite Ausrichtung sowie genaue Basis- und Teilzugriffe bei, ohne skalare Array-Zuweisungen oder Aliasing durch inkompatible Typen.

`NeverDLowIRRefinementTests` prüft echte rekonstruierte Graphen, unterschiedlich strukturierte endliche Schleifen einschließlich null Iterationen, dynamische Erzeuger, bedingte Wahlen, überlappende Eingaben, korrelierte Kopien und Spills, beidseitige unveränderliche Lesebelege sowie Systemflags und Rückkehrslot-Erhaltung. Falsche Kandidaten, zusätzliche Schreibzugriffe, unvollständige oder unendliche Pfade, veraltete Belege, Scratch-Kollisionen und erschöpfte gemeinsame Budgets müssen Zertifikate verweigern. Die bisherigen Unabhängigkeitstests lehnen beobachtbare beliebige Werte weiterhin ab.

`LowIRLoopRefinement.*` und `BinaryLowIRLoopRefinement.*` im selben Ziel prüfen beliebige 64-Bit-Zähler, verschachtelte lexikografische Ränge, echte native Restprogramme, Eingangsvorlagen, überlappende Ansichten und korrelierte Auslagerungen. Negativtests verwerfen falsche Schleifenkörper, eingeschränkte Eingangsbereiche, nicht sinkende Ränge, vorzeichenlosen Umlauf, vergessene frühere Schreibzugriffe, fehlende Schnittpunkte, ungültige Vorlagen und erschöpfte gemeinsame Budgets. Ein erfolgreicher endlicher Geschwisterpfad legitimiert keinen unvollständigen Induktionsbeweis.

`LowIRLoopInference.*` und `BinaryLowIRLoopInference.*` verwenden unabhängig geschriebene Zähler, Stack-Ablagen, frühe Rückgaben, native Aufrufe und gepackte Flags. Sie prüfen schmale arithmetische Erweiterung und semantisch gleiche Flags mit unterschiedlichen Ausdrücken. Fehlerhafte Graphen, fehlende oder gefälschte Ursprünge, endlose oder umlaufende Schleifen und erschöpfte Budgets dürfen kein Zertifikat erzeugen.

Regressionen für gemeinsame Schleifenköpfe und Rücksprungblöcke prüfen nullerweiterte 32-Bit- und volle 64-Bit-Zähler, skalare Ränge mit anderen Schrittweiten, falsche Ergebnisse, stagnierende und umlaufende Pfade sowie exakte oder erschöpfte Budgets über skalare und Tupelsuche hinweg. `LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`. Zusätzliche Inkrement-/Reset-Regressionen verlangen Konvergenz ohne bitweises Entfalten des Zählers pro Runde und lehnen fehlenden Fortschritt sowie vorzeichenlosen Überlauf ab. Scheduling-Regressionen prüfen Akkumulatoren mit anderen Schrittweiten, umlaufende Einheitszähler neben einem gültigen skalaren Rang mit anderer Schrittweite sowie drei Zähler, deren gültiges Tupel erst nach dem frühen Fenster erscheint. Exakte und um einen Versuch reduzierte Rangbudgets prüfen deterministische Fortsetzung ohne wiederholte Vorschläge.

`LowIRLoopPlanPairing.*` im selben Ziel prüft umbenannte Register, unterschiedliche Rechenkörper, seitenspezifische Präfixzustände, erhaltene Prädikate, gemeinsame Speicherrahmeneingaben, verschachtelte Schnittpunkte und unabhängige Beweisbudgets. Fehlende Beziehungen, falsche Schreibzugriffe, ungültige Temporärbindungen, unvollständige Zuordnungen und erschöpfte Metadatengrenzen dürfen kein Zertifikat erzeugen.

`LowIRLoopAlignment.*` prüft unabhängig geschriebene gewöhnliche und rotierte Schleifen mit Frame-Zählern: Beide Standardpläne beweisen sich einzeln, ihre erste Paarung scheitert, ein anderer Kandidatenschnittpunkt beweist die Relation. Regressionen decken mehrere Schnittpunktpermutationen, falsche Ergebnisse und Frame-Schreibzugriffe, fehlende oder veraltete Originalaufzeichnungen, explizite Witnesses für undefinierte Werte, nicht fallende oder überlaufende Zähler, fehlerhafte Graphen, kumulierte Abfragen fehlgeschlagener Versuche, exakte Gesamtbudgets und erschöpfte Suchlimits ab. Ablehnungen dürfen kein Zertifikat enthalten. Weitere Fälle prüfen getrennte Rücksetz- und Fortschrittsphasen, äquivalent verschobene Austrittswächter mit familienübergreifender Paarung, Caches ohne erneute Inferenz und gemeinsame Metadatenüberschreitung. Ein nachfolgender unabhängiger Zyklus prüft vollständige Abdeckung mit ausdrücklich 16384 Inferenzabfragen. Leere und doppelte Familien benötigen keine symbolischen Abfragen; zu wenige Schnitte werden abgelehnt. Exakte und um einen Versuch reduzierte Gesamtbudgets, falsche Ergebnisse, fehlender Fortschritt, ursprüngliche Evidenz und undefinierte Witness-Werte bleiben geprüft. Filterregressionen prüfen neutrale arithmetische Rauten, lokale Zusammenläufe gegenüber Zusammenläufen nur an Grenzen sowie erreichbare Zusammenläufe mit Umgehung zu einem Austritt oder einer Schleifengrenze. Geprüft werden gefilterte Kandidaten trotz doppelter Originalfamilie, Wiederverwendung eines zuvor erfolgreichen Filterplans vor späterer vollständiger Inferenz, exaktes, um eins zu kleines und null gesetztes `MaxCutSelectionWork`, kumuliertes fehlgeschlagenes `CutSelectionWork` und keine symbolische Inferenz nach Erschöpfung der globalen Grapharbeit. Beide Verzweigungsfamilien prüfen vollständige Zyklusabdeckung; die Rautenrelation verwendet explizite Inferenz- und Beweisabfragelimits.

Die Regressionen prüfen Frame- und Registerteilbereiche, beide Richtungen, untere/mittlere/obere Positionen, ungewöhnliche Breiten, beide Byteordnungen und drei Byte breite Frame-Wörter. Abgedeckt sind späte Erkennung, Änderungen erhaltener Bits, fehlender Fortschritt und ungesichertes Überlaufen, ungültige zusätzliche Einstiege, exakte/zu kleine Budgets sowie die bisherige Einzelschnittsuche.

Regressionen der vorderen Phase prüfen zwei und drei aufeinanderfolgende Schleifen mit wiederverwendetem Countdown-Wort, die Kombination mit verschachtelten Schleifenphasen, exakte und um einen Versuch zu kleine Rang- und Abfragebudgets, Schleifen ohne Fortschritt und Rücksetzungen zu einer früheren Phase. Fehlerhafte Phasenkonstanten gleicher Breite, falsche Ergebnisse und Frame-Schreibzugriffe müssen vom vollständigen Prüfer ohne Zertifikat abgelehnt werden; fehlende Originalevidenz bleibt nicht unterstützt.

`InterpreterMachineStateModel.*` in `NeverDLowIRRefinementTests` prüft mit unabhängigen LowIR-Beispielen rohe Eintrittsflags, Status getrennt vom Gast-RAX, alle 17 Zustandswörter, Teilregister, gepackte Flags, bleibende dynamische Ablehnung, Gast-Rahmenschreibzugriffe, beide Zweige und Schleifeninferenz mit anschließendem neuem Beweis. Falsche Ausgaben, verlorener Status, geänderter Speicher, veraltete Instruktionsdatensätze, fehlerhafte Eingaben und erschöpfte Erzeugungsbudgets müssen scheitern. Bestehende Quelltexttests führen beide C-Wege unter O0/O2 aus; Modelltests allein zertifizieren keinen kompilierten C-Code.

`NeverDLLVMInterpreterModelTests` vergleicht unabhängig geschriebenes LLVM mit LowIR-Referenzen für den gesamten Zustand: Breiten, parallele PHIs, switch, Gastspeicher, separater Status, Poison-Bedingungen, Intrinsic-Bereiche, abgelehnte Verträge und vier Budgets. Ein vollständiger Beweis für beliebige Wort-Countdowns wird geprüft, veränderter Status abgelehnt. Unabhängiges C muss nach O1/O2-Kompilierung dieselben Beobachtungen erfüllen. Die Tests prüfen das unterstützte Modell; automatische Invariantenfindung und Compilerkorrektheit bleiben separat. Die variablen Schiebetests decken alle vier Breiten, durch Masken oder Verzweigungen begrenzte Schiebeweiten, Grenzwerte und zu große Werte, Überlaufverbote und Exaktheitsflags, strikte Poison-Ablehnung sowie mit O1/O2 kompiliertes C ab.

Initialisierungsregressionen prüfen partielle und getrennte Bytebereiche, feste Aliasse, beide Zweige, jede Rückkehr, Lesezugriffe der ersten Iteration und Schreiben vor Lesen in Schleifen. Lesen vor Schreiben, fehlende Schreibzugriffe, Gastspeicherungen, unbekannte Aliasse, spezielle Speicherzugriffe, Bereiche außerhalb des Objekts und erschöpfte Budgets müssen scheitern. Ein unabhängiges C-Beispiel mit einem nur geschriebenen Zustandswort wird mit O1/O2 kompiliert, behält die exakten LLVM-Attribute und besteht eine neue kombinierte Prüfung von nativem Code zu LLVM.

Geschützte Countdown-Tests prüfen den nächsten Versuch nach einem verworfenen Rumpf-Template, einen vollständigen Kopfbeweis für beliebige Worteingaben, gemeinsame Schnittpunkt-/Anfragebudgets und die sofortige Ablehnung echter Eingangsvertragsverletzungen.

`NeverDInterpreterLLVMRefinementTests` prüft neue Gesamtbeweise, exakte Text-/Funktionsbindung, unabhängige Budgets, vollständige Beobachtungen und größere Quellbereiche. Geänderte Bytes, Restprogramme, Ergebnisse, Flags, Status, Frame-Schreibzugriffe, Poison und falsche/veraltete Schleifenpläne müssen den Gesamtnachweis verhindern. Beliebige Wortzähler erfordern beide induktiven Voraussetzungen; unabhängige C-Beispiele mit O1/O2 prüfen tatsächlichen serialisierten LLVM-Input. Zustandsmodelltests lehnen versteckte Einstieg-Rückkanten ab und begrenzen Wurzeln ohne Kopie zusätzlicher Herkunftsdaten.

```sh
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

Das folgende Verhalten verwendet den standardmäßigen strengen Prüfvertrag. `NeverDOriginalBinaryUndefinedIndependenceTests` prüft mit unabhängigen x64-Bytes in festen Abbildungen physische CALL/RET-Semantik, veränderte Rücksprungziele, vollständige endliche indirekte Zielmengen und unveränderliche Lesezugriffe. Dasselbe Ziel prüft vollständige direkte Verzweigungen, genaue Bindung von Bytes/Effekten/Abbildungen/Lesebelegen, Erhaltung von Eintritts-RSP und Rückadressenslot am äußeren Rücksprung sowie Frame/Image-Trennung. Fehlende oder überlappende Instruktionen, ungeprüfte Zweige außerhalb der exakten Trap- und expliziten Profilprojektionsregeln, nicht terminierende oder budgetüberschreitende Schleifen, unvollständige Zielmengen, Profil-/Vertragskonflikte und erschöpfte Budgets müssen ohne Zertifikat oder Restcode abgelehnt werden. Alle möglichen nativen Pfade müssen enden. Diese optionale Schranke zertifiziert keine Schleifeninvarianten, Ausnahmebehandlung, CET-Ausführung oder native-zu-C-Äquivalenz; gewöhnliche Wiederherstellung bleibt getrennt. Das Ziel prüft außerdem strikt geliftete `INT3`/`UD2`-Endgrenzen und die Bindung ihrer vollständigen Bytes und Operations-Digests. Ein `Missing`-Sidecar für undefinierte Ausgaben muss `Missing` bleiben; nur symbolisch nachweisbar unerreichbare Traps dürfen in einem Zertifikat erscheinen, während jeder mögliche Trap-Pfad `ContractViolation` ohne Zertifikat oder Restcode liefern muss. Eine Fortsetzung nach Traps und eine Wiederaufnahme nach Ausnahmen werden nicht modelliert, `codeFollowsTrap` wird nicht verwendet und die statische LowIR-API bleibt unverändert.

Explizite Tests für native Überlappung prüfen echte x64-Sprünge in Immediate-Operanden, beide möglichen Zweigergebnisse und indirekte Rücksprungeinstiege innerhalb vorheriger Befehle. Synthetische Anbieter prüfen enthaltene Bereiche in beiden Erfassungsreihenfolgen, widersprüchliche Bytes auf einem nicht genommenen direkten Zweig, Code-/Leseübereinstimmung in beiden Reihenfolgen sowie Kandidatenzugriffe. Exakte und zu kleine Byte-Budgets zählen wiederholte Bytes auch über indirekte Transfers hinweg. Geänderte Ergebnisse, statische oder Schleifen-API-Nutzung und widersprüchliche Nachweise müssen Zertifikate verhindern; Änderungen der Option oder Grenze verändern die Hashes.

Tests der expliziten Grenzen prüfen unerreichbares RCL, Speicher-XADD und REP MOVS, symbolische Pfadwidersprüche, von beliebigen Werten gesteuerte Verzweigungen sowie genaue Ablehnungen bei Eintritt, indirektem Sprung, CALL und RET. Sie prüfen unabhängige Zugänge zu erreichbaren Suffixen, gleiche Kandidaten-/Native-Adressen, fehlerhafte oder unvollständige Belege, erschöpfte Ressourcen, die Ablehnung statischer/Schleifen-APIs und alle drei Digest-Ebenen der Verfeinerung. Änderungen unerreichbarer Befehle oder der Option ohne vorhandene Grenzen ändern die Zertifikat-Digests. Dies prüft den deklarierten endlichen Beweisumfang, nicht die Semantik ungeprüfter Befehle.

Tests gepackter Flags prüfen alle skalaren Eingangsflagkombinationen, Privilegmasken, TF/AC in beiden Ausführungen, getrennte undefinierte Erzeuger, korrelierte Kopien, native Aufrufe, Geschwisterpfade, verpflichtende Endzustandsbeobachtung, fehlerhafte Belege und Ressourcenlimits. Alle möglichen Eingabepfade endlicher Schleifen müssen terminieren; ein sicherer Zweig verdeckt keinen unendlichen oder abgeschnittenen Pfad. RDSSPD/RDSSPQ prüft alle 16 allgemeinen Register in beiden Breiten, erhaltene obere Bits, unveränderte `Missing`-Belege und abgelehnte gefälschte Projektionen. Maschinenzustandstests vergleichen beide C-Wege bei O0/O2 mit Fallen für undefiniertes Verhalten gegen ein unabhängiges Benutzermodus-Flagorakel und prüfen dauerhaft gespeicherte Profilfehler. INCSSPD/INCSSPQ-Tests prüfen beide Breiten und alle allgemeinen Register, erhaltene unerreichbare Grenzen, ausführbare Traps nach einem abgeschlossenen Geschwisterpfad, Nulloperanden und gefälschte Trap-Belege.

`NeverDX86UndefinedEffectsTests` prüft Metadaten undefinierter Bits, definierte oder erhaltene Flags und die Ablehnung veralteter Zertifikate. `NeverDX86CarryArithmeticFlagTests` prüft den Hilfsübertrag von ADC/SBB für Register- und Speicherformen anhand eines arithmetischen Orakels. `NeverDX86LogicIdentityTests` prüft, dass AND mit identischen Operanden im 64-Bit-Modus beim Schreiben eines 32-Bit-Ziels weiterhin die Bits 63:32 des zugehörigen 64-Bit-Registers löscht und bei schmaleren Schreibzugriffen die ungeschriebenen Bits erhält.

`X86RotateUndefinedEffects.*` prüft alle Rohzähler, Operandenbreiten, CL-Überlappungen, obere Byte-Aliasse und Speicherziele gegen ein skalares Rechenorakel. `X86BitTestUndefinedEffects.*` deckt Register-/Immediate-Indizes, Quell-/Zielüberlappungen, erweiterte Register, definierte Flags und Schreibzugriffe auf obere Registerteile ab. Metadatenkontrollen weisen veränderte Operanden, Kodierungen und nicht unterstützte Formen zurück. Native Beweise unterscheiden korrelierte Lesezugriffe von unabhängigen beliebigen Flags, prüfen genaue/zu kleine Erzeugerbudgets und verweigern beobachtbaren undefinierten Überlauf. Die vollständige Zustandsverfeinerung akzeptiert den gewählten Zeugen und lehnt Nullbit-Zeugen oder veränderte Kandidaten ab.

`X86XaddAudit.*` prüft alle 65,536 Byte-Operandenpaare, Flag-Grenzen größerer Breiten, Register-/High-Byte-Überlappungen, beide Rückschreibungen, REX-Bytebreitengrenzen und den Erhalt ganzer Register anhand vorzeichenloser Arithmetik. Native Prüfungen verlangen keine neuen beliebigen Bits, ohne frühere Abhängigkeiten zu verlieren. Beide Zeugen akzeptieren unverändertes XADD; manipulierte Summen, ausgetauschte Quellen oder definierte Flags werden abgelehnt. Der `/6`-Alias durchläuft die vollständige Schiebezählmatrix; veränderte Gruppen/dekodierte IDs müssen als semantisch inkonsistent abgelehnt werden.

`NeverDPEFixedImageTests` prüft mit unabhängig erstellten PE-Dateien relokierte Instruktionen, unveränderliche Daten, Import-Schreibbereiche, fehlerhafte Header/Tabellen, Aliase und geänderte Herkunftsdaten. Beweise von nativen Instruktionen zu LowIR und exaktem LLVM akzeptieren passende Kandidaten und lehnen geänderte Ergebnisse, Statuswerte oder native Bytes ab. Ein erschöpftes Vorbereitungsbudget bleibt separat erkennbar und erlaubt eine Wiederholung mit ausdrücklich erhöhten Grenzen; normales Laden akzeptiert auch 40000 gültige Relokationen oberhalb des Standardbudgets der Analyse.

`FrameOffsets.*`, `NativeStackSpecialization.*` und `OriginalBinaryUndefinedIndependence.*` prüfen alle Reste für Ausrichtungen 2/4/8/16/32, freie obere Bits, aufrufübergreifende Spills, Countdown-Schleifen, Aliasbeschädigung, falsche Verzweigung, irrelevante große Masken, notwendige Partitionserweiterungen sowie exakte und um eins zu kleine Budgets. Separate native Kontrollen prüfen bedingte Ausrichtung, interne vorzeichenlose Rückkehrbereinigung, falsche Bereinigung und präfixbehaftete Rückkehr. Diese Tests belegen keine automatische native LLVM-Beweisabdeckung partitionierter Schleifen.

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

## Kontrollfluss und Aufrufe in strukturiertem C prüfen

`HighControlFlowSemantics.*` prüft, dass beim Verschieben von Schleifenausstiegen oder nachfolgenden Blöcken die von anderen Sprüngen verwendeten Labels erhalten bleiben. Direkte Einstiege in Ausstiege am Schleifenanfang und -ende sowie der Ersatz von break werden im erzeugten C mit O0/O2 gegen unabhängige Rückgabewerte geprüft.

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` prüft, dass erkannte erforderliche Registerargumente auch unbekannte Positionen am Ende behalten. Das Auswerten eines unbekannten erforderlichen Arguments oder einer unbekannten Bedingung muss explizit eine Trap auslösen; ausgelassene, nullwertige und verschachtelte Operanden dürfen nicht stillschweigend zu null werden. Bekannte Werte und nachweislich ungelesene zusätzliche Operanden bleiben ausführbar. Eine Trap ist eine Diagnosegrenze und kein Beweis für gleichwertiges rekonstruiertes Verhalten.

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

## Tests des Linux-Prozessprofils

Die unabhängigen [Prozesstests](process-emulation.md#verifikation) kompilieren echte x64-/AArch64-ELF-Fixtures. `NeverDLinuxProcessTests` prüft Start, Program-Header-Policy, Service-Fortsetzungen, Binärausgabe, Gastfehler und Ressourcenstopps. `NeverDProcessPublicTests` testet C-API/CLI ohne Änderung des Analyse-Images. `NeverDExecutionSessionTests` prüft zwei CPUs mit gemeinsamem Speicher/Budget sowie Exactly-once-Verbrauch von Requests/Fehlern. `NeverDX64MemoryUpdateTests` prüft Speicherarithmetik, SETcc, BT, XMM/MXCSR, Schreibbeobachter, REP-Grenzen und vorbereitete Geräte-Lesezugriffe. `DriverBackendParityTests.cpp` führt originale und relokierte WDK-Fixtures aus und vergleicht den vollständigen beobachtbaren Bericht mit Unicorn; fehlende Images/Backends werden übersprungen.

Geprüftes x64 erlaubt auch maskierte Legacy-Formen `SS`, `SD`, `PS`, `PD` von `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`. `X64SSEInstructions.def` verwaltet Breiten, Ausrichtung und Zulassung zentral. `MaskedSSEArithmeticMatchesIndependentHostExecution` vergleicht Register/RAM mit einem unabhängigen Host-CPU-Orakel: vier Rundungsmodi, FTZ, vorzeichenbehaftete Nullen, Subnormalzahlen und NaNs. `SSEMemoryObserverStopsBeforeResultAndStatusChanges` prüft den Stopp vor Effekten. DAZ, unmaskierte Ausnahmen, x87 und AVX bleiben ausgeschlossen.

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

Die Quellprojektion prüft variadische Objektlisten auch nach dieser Bereinigung erneut: Leere Instruktionsanker sind zulässig, versteckte Effekte und Kontrolltransfers nicht. Die Synchronisationsbereinigung akzeptiert eine einzelne `int64_t`- oder `uint64_t`-Sicht desselben gespeicherten Empfängers; Verengungen, Gleitkommakonvertierungen, Adressarithmetik und erneute Zuweisungen bleiben ausgeschlossen. Foundation-Objektmengen sowie normale und ausnahmebedingte Entsperrabläufe werden mit `-O0` und `-O2` ausgeführt.

## Native synchrone x64-Ausnahmen

Checked x64 führt `DIV`/`IDIV` mit echten Prozessorergebnissen und `#DE` aus. KVM nutzt eine private Supervisor-IDT/IST, WHP eine explizite Ausnahme-Bitmap; ursprünglicher Kontext und verfügbare Fehlercodes bleiben von Transportfehlern getrennt. Das OS konsumiert das wiederaufnehmbare Ereignis vor dem Setzen einer Fortsetzung. Windows-Treiber behandeln Nulldivision und Quotientenüberlauf als `STATUS_INTEGER_DIVIDE_BY_ZERO`, mit echten SEH-Filtern, `__finally` und Wiederholung. `NeverDX64ExceptionTests` baut ohne Unicorn; `DriverWDMCPUException` prüft originale WDK-Fälle. Nicht verfügbare ARM64-Hosts werden explizit übersprungen.

## Gestufte RAM-Effekte

`RAMTransaction` erfasst unter der physischen Ausführungslease nur die vereinigten deklarierten Schreibbereiche einer Instruktion. Vor Ergebnisbeobachtern wird der ursprüngliche RAM wiederhergestellt; Abbruch, Transportfehler und Beobachterausnahmen veröffentlichen weder Teilwrites noch Register. Prozessorfehler behalten nach RAM-Rollback ihren architektonischen Ausnahmestatus. ARM64-Einzel- und Paarstores verwenden dieselbe Instanz. x64 führt `XCHG`, `XADD` und `CMPXCHG` mit 8/16/32/64 Bit aus; gesperrte und implizit gesperrte Formen erfordern natürliche Ausrichtung. `NeverDRAMTransactionTests` vergleicht Ergebnisse mit der Host-CPU und prüft Rollback, Aliase und Rechte; fehlende Plattformen werden ausdrücklich übersprungen. Geräte und paralleles SMP bleiben ausgeschlossen. CPU-Snapshots setzen bereits bestätigten RAM nicht zurück.

## Vollständiger x87-Zustand

`NeverDEmulationArch` besitzt ISA-Verträge, Seitentabellen und das FP-Layout, das native Transporte und Unicorn gemeinsam nutzen. x64-Kontexte erhalten Steuerung, Status, TOP, physische Tags, Opcode, Befehls-/Datenzeiger und acht 80-Bit-Register. `FP0`–`FP7` verwenden `RegisterValue`; skalare Zugriffe lehnen eine Kürzung ab. `FPTag` ist die physische Maske nicht leerer Register. `NeverDX64FPTests` prüft alle TOP-Werte, exakte Operationen gegen Host-FXSAVE/FXRSTOR und die Wiederherstellung. Dies lässt keine x87-Befehle im checked-Vertrag zu und beweist nicht sämtliche Rundungssemantik. Fehlende native Hosts werden ausdrücklich übersprungen.

`driver-strict` unterstützt KVM auf passenden Linux-x64-Hosts und WHP auf passenden Windows-x64-Hosts; `auto` wählt diesen nativen Transport, unterschiedliche ISAs verwenden Unicorn. Explizites Unicorn und die bisherige V1-API behalten das portable Softwareprofil. Native Ausführung prüft kanonische Adressen und Effekte vor dem Eintritt; fehlende Hardware führt ohne Rückfall zum Fehler. Nicht unterstützte Instruktionen und OS-Verhalten bleiben explizite Fehler. Die native Windows-x64-CI besteht bei deaktiviertem Unicorn alle 359 Pflichtprüfungen: 131 CPU-Prüfungen, 224 Treiberergebnisse aus 26 eingebauten Images, 46 WDK-Images und 40 Szenariofällen an bevorzugten und verschobenen Adressen sowie vier SEH-Grenzprüfungen ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Native ARM64-Nachweise fehlen weiterhin; allgemeine Treiber- oder Android/Darwin-Kompatibilität ist damit nicht belegt.

Die obige native Prüfung umfasst die deklarierten Treibereinstiegspunkte und veröffentlichten Szenarien. Die unten beschriebenen detaillierten Funktionstests sowie C-API-/CLI-/Python-Prüfungen bleiben auf Linux-Nachweise beschränkt, sofern keine Windows-Ausführung ausdrücklich belegt ist. Ein erfolgreicher nativer Korpus belegt nicht jede Testvariante unter Windows.

Das ausgewählte Profil lässt sich mit `executionCapabilities(Contract, ISA, Backend)` abfragen. `NativeLegacyX64` beschreibt die native Ausführung von x64-Treibern. `NeverDNativeDriverTests` prüft den vorhandenen Treiberkorpus und kann auch in einem Build ohne Unicorn laufen.

Der bestehende CI-Workflow führt vor den allgemeinen Profilen das gesamte Emulationstestverzeichnis aus und speichert Inventar, JUnit-Ergebnisse und CTest-Protokoll in `emulation-focused`. Fehler anderer Module verhindern diesen Lauf nicht. Fehlende Hardware und optionale Treiberdateien bleiben ausdrücklich übersprungene Tests; erfolgreiche Softwareausführung oder Kompilierung belegt keine native Ausführung.

Unter Linux lässt `NeverDUnicornDeadlineTests` den tatsächlichen Timer-Thread durch gesteuerte pthread-Abläufe vor dem Gasteintritt fertig werden. Für x64, ARM32 und ARM64 prüft der Test, dass eine vorherige Abbruchanforderung keine Gasteffekte erzeugt und der nächste Lauf ein eigenes Budget verwendet. Er nutzt öffentliche APIs und verändert keinen privaten Engine-Zustand.

`X64StateTransition` in `NeverDX64ExceptionTests` führt unabhängige RAM-Lesezugriffe und CR8-Lesezugriffe auf der nativen CPU aus. TLS-Basen und Privileg wechseln, die Ausführung wird nach wiederholten Divisionsfehlern fortgesetzt, und TLS ändert sich nach abgebrochenem Eintritt. Nach Änderungen der nativen Zustandsübertragung sind dieses CTest-Label sowie Alias-Neuzuordnung, CPU-Kontexte, FP-Zustand und Ergebnisvergleiche der ursprünglichen Treiber zu prüfen. Nicht verfügbare KVM/WHP-Transporte bleiben ausdrücklich übersprungen.


`NeverDKvmRunTests` prüft die ausgeliehenen Übertragungen in `KvmRunControl` ohne `/dev/kvm`. `StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` prüft, dass Vorbereitung, Erfassung und abgefangener Host-Eintritt denselben Thread nutzen und die Vorbereitung trotz unterbrochener Wiederholungen nur einmal erfolgt. Weitere Fälle prüfen Vorbereitung ohne Eintritt bei Fehler, Erfassungsfehler, Stopp während der Vorbereitung und Abbruch eines aktiven Eintritts; ein anschließender Lauf darf die alten Callbacks nicht wiederverwenden. Die Tests für echten Abbruch, RAM-Rücknahme, Ausnahmen und ursprüngliche Treiber bleiben Teil der Validierung. `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` prüft, dass mehrere Eintritte mit derselben Frist den Thread wiederverwenden, jeden Transfer einmal ausführen und frühere Zustandspakete unverändert lassen.

KVM x64/ARM64 verwendet `KvmRunControl` für Vorbereitung, `KVM_RUN` und Zustandserfassung auf demselben privaten vCPU-Worker. Auch bei `EINTR` erfolgt die Vorbereitung einmal; Abbruch oder Lesefehler verhindern Veröffentlichung. `KvmAArch64Machine.cpp` führt Übersetzungspflege und vollständige Skalar-/Vektortransfers unter einer gemeinsamen Schrittfrist aus. Der Aufrufer veröffentlicht nach Bestätigung; ISA-Decodierung, RAM-Transaktionen, OS-Politik und Beobachter bleiben bei ihm. Native ARM64-Laufzeitnachweise stehen aus.

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` prüft fortgesetzte Ausführung und echte CPU-Schreibzugriffe nach Host-Änderungen an allgemeinen Registern, erstem und letztem XMM-Register, MXCSR und x87-Steuerwort. Echte `FXSAVE64`-Bytes prüfen alle physischen 80-Bit-Register, TOP, Tags, Opcode und Zeiger nach einem gestoppten Eintritt; wiederholte Divisionsfehler verwerfen die Wiederverwendung ebenfalls. Diese Maschinentests erlauben keine zusätzlichen x87-Instruktionen in checked-Profilen.

`NeverDKvmStateTransferTests` injiziert nach echter KVM-Ausführung einen Register- oder XSAVE-Lesefehler und wiederholt mit unveränderter Eingabe. Unabhängige Ganzzahl- und gepackte Byte-Ergebnisse belegen, dass fehlgeschlagene Erfassung keinen bereits fortgeschrittenen nativen Zustand wiederverwendet. Nur diese Testdatei umhüllt `ioctl`; nicht verfügbare native Hosts werden ausdrücklich übersprungen.

Geprüftes ARM64 besitzt eine gemeinsame Grenze für den vollständigen Zustand. `Registers.def` definiert 39 skalare Felder und 32 Vektoren mit 128 Bit; `captureAArch64State` sammelt alle Werte, wendet Bitbreiten an und normalisiert NZCV vor einer einzigen Veröffentlichung. Unicorn, KVM, WHP und HVF übertragen denselben Bestand einschließlich TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR und FPSR. Native Adapter aktivieren FP/SIMD über CPACR_EL1. Fehlgeschlagene Lesevorgänge und abgebrochene Eintritte erhalten den gesamten Aufruferzustand.

Der ARM64-KVM/WHP/HVF-Start führt das private Programm `AArch64MachineProbe.def` aus: NOP, FP32-Addition mit Rundung gegen positive Unendlichkeit und SIMD-Addition auf zwei Spuren. Jeder Schritt vergleicht alle 39 skalaren Felder und 32 Vektoren, einschließlich TLS, NZCV, gelöschter oberer Ergebnisbits und erhaltenem/kumulativem FPCR/FPSR-Zustand. Die Probe verwendet nur Supervisor-Monitorspeicher und eine gemeinsame Gesamtfrist. Die Proben belegen nur die begrenzte Initialisierung. Die Workload-Validierung für Linux ARM64 KVM und Windows ARM64 WHP steht noch aus; native macOS-Ergebnisse sind im [HVF-Leitfaden](macos-hvf.md) dokumentiert.

Die native x64-Initialisierung von KVM/WHP/HVF führt `X64MachineProbe.def` in privaten Supervisor-Seiten aus. Eine gemeinsame Frist umfasst NOP, FP32-Addition mit Rundung gegen positive Unendlichkeit, SIMD-Addition mit zwei Lanes sowie FS/GS- und CS/SS/CR8-Lesezugriffe; jeder Schritt vergleicht den vollständigen Skalar-, XMM-, physischen x87- und Steuerzustand. x64- und ARM64-Proben benötigen das exklusive Ausführungsrecht des physischen Speichers. `MemoryProjection` besitzt die Cache-Identität (ISA, Adressraum, Mapping-Generation, Privileg und Monitorvariante) und den Verlauf bestätigter Wurzeln je ISA. Vor dem Überschreiben wird der Cache ungültig: fehlgeschlagener Ersatz darf keine teilweise geschriebenen Tabellen wiederverwenden, und Aufrufer liefern keine veralteten Wurzeln. Die Proben belegen nur die begrenzte Initialisierung. Die Workload-Validierung für Linux ARM64 KVM und Windows ARM64 WHP steht noch aus; native macOS-Ergebnisse sind im [HVF-Leitfaden](macos-hvf.md) dokumentiert.

Der gemeinsame XSAVE-Decoder unterscheidet den initialen SSE-Zustand im Standard- und Kompaktformat. Bei gelöschtem XSTATE_BV[1] initialisieren beide XMM; das Standardformat liest und prüft weiterhin MXCSR, das Kompaktformat initialisiert MXCSR. `X64XsaveCases.def` enthält unabhängige Datenlayouts und eigene XRSTOR-Hostprogramme. `X64XsaveTests.cpp` prüft atomare Ablehnung und vergleicht beide Formate mit tatsächlicher Hostausführung unter Erhalt des FP/SSE-Zustands des Aufrufers. Fehlen die Hostarchitektur oder die benötigte Befehlsfunktion, wird der Hostvergleich ausdrücklich übersprungen.

`X64FPState.def` deklariert kompakte Transportlayouts für AVX, AVX-512, CET_U/CET_S und AMX einschließlich der Komponentenausrichtung auf 64 Byte. Vorhandene Erweiterungsdaten müssen dem Null-Initialzustand entsprechen; fehlende Komponenten und Füllbytes definieren keinen Zustand. Layoutbits bestimmen Offsets; unbekannte Layouts, nicht initiale Daten und falsche Längen scheitern vor der Veröffentlichung. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState` und `InitialWideComponentsDoNotHideFPState` prüfen WHP-Pakete mit 872 und 10752 Byte. Der Transport erlaubt keine Ausführung dieser Erweiterungsbefehle.

`WhpXsaveRegisters.def` ergänzt vollständige XSAVE-Pakete durch benannte x87/SSE-Steuerregister. Letzter Opcode sowie Instruktions- und Datenzeiger werden explizit geschrieben und vom Host gelesen. Leere Paketfelder können ergänzt werden; widersprüchliche Nichtnullwerte oder gemeinsame Steuerfelder führen vor der Veröffentlichung zum Fehler. `NamedMetadataRestoresOmittedPacketFields` prüft fehlende Felder unter Erhalt der gesamten FP-Nutzdaten.

Native `FOP/FIP/FDP` folgen den x87-Sicherungsregeln des Hosts. AMD darf diese Felder ohne ausstehende unmaskierte Ausnahme löschen; Snapshots behalten die beobachteten Werte. `X64MachineProbe.def` und exakte NOP/Kontexttests setzen einen konsistenten ausstehenden Ausnahmezustand, damit jedes Feld gültig bleibt und ohne ausgeblendete Unterschiede verglichen wird. Die FXRSTOR64/FXSAVE64-Referenz im Hostprozess prüft beide Zustände; Backends ersetzen Hostergebnisse niemals durch Eingabemetadaten.

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` vergleicht den durch FXSAVE64 im Gast-RAM gespeicherten vollständigen FP/SSE-Zustand mit der XSAVE-Erfassung des Hosts. Beide APIs prüfen direkte Installation und FXRSTOR64 im Gast mit der Standardfunktion zur Zeigerspeicherung und der expliziten vom Host unterstützten Einstellung. Eintritt, Ausführung und Erfassung bleiben unterscheidbar; Werte werden nicht korrigiert, Abweichungen bleiben Fehler. Die Matrix erfasst auch eine ausstehende unmaskierte x87-Ausnahme und protokolliert eine FXRSTOR64/FXSAVE64-Referenz im Hostprozess samt Prozessorhersteller, um bedingte Zeigerspeicherung vom WHP-Transport zu unterscheiden.

Der gemeinsame Codec `encodeX64XsaveState` / `decodeX64XsaveState` besitzt Standard- und komprimierte FP/SSE-Pakete, die physische TOP-Rotation, Initialzustände fehlender Komponenten und atomare Prüfung. WHP verwendet vollständige XSAVE-APIs, bevorzugt `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`, mit älteren XSAVE-APIs als Kompatibilitätsweg. Einzelne ältere x87-Register ersetzen kein vollständiges Paket. Nicht initiale Erweiterungskomponenten, fehlerhafte Header, ungültige Steuerwerte und verkürzte Erfassungen scheitern ausdrücklich. WHP-Mappingfehler behalten HRESULT, GPA und Größe für die Diagnose.

`CheckedX64Instructions.def` erlaubt vorzeichenloses `MUL` mit 8/16/32/64 Bit und `CBW/CWDE/CDQE/CWD/CDQ/CQO` über den vorhandenen CPU-Transport. `NeverDX64IntegerTests` verwendet unabhängige Kodierungen und Erwartungswerte aus `X64IntegerCases.def` auf beiden Privilegstufen: Erhalt unveränderter Registerteile, 32-Bit-Nullerweiterung, beide Produkthälften, definierte CF/OF-Ergebnisse und unveränderte Flags bei Vorzeichenerweiterung. Multiplikation in gewöhnlichem RAM behält Rechteprüfungen für den gesamten Zugriffsbereich und Lese-Beobachter bei; ein Fehler oder Beobachterstopp erhält implizite Ausgaberegister und PC. Geräteoperanden bleiben nicht unterstützt. Die Fälle laufen auch mit checked Unicorn; nicht verfügbare native Transporte werden ausdrücklich übersprungen.

`X64BitInstructions.def` erlaubt `BT/BTS/BTR/BTC` für Register und gewöhnlichen RAM mit 16/32/64 Bit. Ein Registerindex wird in Operandenbreite vorzeichenbehaftet ausgewertet und wählt ein ganzes Wort; ein unmittelbarer Index bleibt im Basiswort. Die Kürzung auf die Adressbreite erfolgt vor dem Addieren der FS/GS-Basis. Der Prozessor liefert CF und Schreibwerte; `RAMTransaction` hält das Ergebnis bis zur Annahme durch die Beobachter zurück. Berechtigungen werden über den gesamten Bereich einschließlich separater Seiten und Aliase geprüft. Stopps, Callback-Fehler und verweigerte Zugriffe erhalten CPU und RAM. LOCK ist auf natürlich ausgerichtete Speicheränderungen beschränkt; MMIO und paralleles Hardware-SMP bleiben ausgeschlossen. `X64BitStringTests.cpp` vergleicht unabhängige Kodierungen mit echter x64-Ausführung und prüft negative Indizes, Breitenkürzung, Seitengrenzen, Abbruch und ungültige LOCK-Formen. Siehe die [Intel-Befehlsreferenz](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`X64StringInstructions.def` definiert `MOVS/STOS/LODS` für normalen RAM mit 8/16/32/64 Bit; `CLD/STD` ändert nur das Richtungsflag. Jedes REP-Element prüft den gesamten Operanden vor Beobachteraufrufen und wird an einer Wiederaufnahmegrenze übernommen. Spätere Fehler erhalten fertige Elemente; Abbruch oder Callback-Ausnahme lässt das aktuelle Element unverändert. FS/GS wird erst nach Begrenzung der Adressbreite und nur zur Quelle addiert. AL/AX erhält obere Bits, EAX erweitert mit Nullen. Bei REP mit Zähler null und 32-Bit-Adressen müssen die oberen Zählerbits und für MOVS/STOS die oberen Bits der beteiligten Adressregister null sein: Reale Prozessoren liefern sonst unterschiedliche Ergebnisse. REPNE für MOVS/STOS/LODS und STOS/LODS-Geräteoperanden bleiben ausgeschlossen. `X64StringTransferTests.cpp` vergleicht Breiten, Richtung, Überlappung und Nullzähler mit unabhängigen Hostbefehlen und prüft Rechte, Aliasse, Adressumlauf, Fehler und Wiederaufnahme. Der originale WDK-Ressourcentreiber führt über `driver_resource_strings.def` alle vier STOS/LODS-Breiten aus.

`X64StringInstructions.def` definiert außerdem `CMPS/SCAS` auf normalem RAM mit 8/16/32/64 Bit und `REPE/REPNE`. Jedes Element prüft alle Leseoperanden vor Beobachtern, aktualisiert sechs arithmetische Flags und endet bei der ersten Abbruchbedingung. Ein Datenfehler stellt die Flags vom Beginn dieses ununterbrochenen REP wieder her; abgeschlossene Zeiger- und Zähleränderungen bleiben erhalten. Ein öffentlicher Wiedereinstieg beginnt mit dem veröffentlichten CPU-Zustand. Stopps und Beobachterausnahmen verändern das aktuelle Element nicht; nach vorzeitigem Ende wird das nächste Element nicht gelesen. FS/GS betrifft nur die CMPS-Quelle; SCAS bewahrt Akkumulator und ungenutztes Quellregister. Geräteoperanden und mehrdeutige obere Bits bei inaktivem 32-Bit-Zähler bleiben ausgeschlossen. `X64StringComparisonTests.cpp` vergleicht unabhängige Hostbefehle, Flags, Richtung, Aliase, Adressumlauf, Rechte und Wiederaufnahme; ein Linux-x64-Signaltest erfasst echte Fehlerregister. Der originale WDK-Ressourcentreiber führt beide bedingten Wiederholungen in allen vier Breiten über `driver_resource_strings.def` aus. Siehe [Intel-Referenz](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`WhpResourceCache.h` trennt den Zustand logischer CPUs von WHP-Partitionen. Die Laufzeit hält eine aktive native Partition und verwendet sie für aufeinanderfolgende Schritte derselben CPU wieder. Ein CPU-Wechsel zerstört die alte Partition, bevor Abbildungen, virtueller Prozessor und vollständiger Zustand neu aufgebaut werden. Logische CPUs behalten unabhängige `MemoryProjection`-Sichten und den maßgeblichen RAM. Der Erwerb der Lease beachtet Abbruch und laufende Frist; die Freigabe einer inaktiven CPU zerstört keine fremde Partition. x64 erhält die voreingestellte XSAVE-Funktionskombination des Hosts und prüft die wirksame Partition mit `WHvGetPartitionProperty`, ohne abhängige Funktionen für eine kleinere Maske zu löschen. Der kooperative CPU-Wechsel bietet kein paralleles Hardware-SMP.

`NeverDX64FPTests` prüft alle 79 Korruptionspositionen und führt unabhängig assemblierte `X64ProbeCases.def`-Befehle nativ mit gemeinsamer Frist und unverändertem Gast-RAM aus. `NeverDProjectionCacheTests` prüft Aufruferwechsel, ISA-Reihenfolge, Wurzelverlauf, Privileg-/Monitorvarianten, Mapping-Generationen, Adressraumidentität und fehlgeschlagenen Ersatz. `NeverDRunControlTests` enthält `WhpXsaveTests.cpp` für moderne und ältere API-Pakete, jeden TOP, Größengrenzen und unveränderten Zustand bei Fehlern; diese Speicher-Protokolltests sind kein nativer WHP-Nachweis. Nicht verfügbare native Transporte werden ausdrücklich übersprungen.

XSAVE-Diagnosen unterscheiden Größenabfrage, lokale Vorbereitung und Dekodierung erfasster Pakete. API-Name, zurückgegebene Bytezahl, Kapazität und begrenzte Header-/Steuerdaten bleiben erhalten; unabhängige Erwartungen stehen in `WhpHostFailureCases.def`. Gastregisterinhalte werden nicht ausgegeben. `InvalidInputReportsPreparationWithoutHostMutation` prüft außerdem, dass ungültige Eingaben weder den Host aufrufen noch dessen Paket ändern. Der gemeinsame ISA-Codec bleibt allein für die Validierung zuständig.

WHP-Hostfehler bei Fähigkeitsabfragen, Partitions-/CPU-Einrichtung, Register-/XSAVE-Übertragung und Ausführung behalten den HRESULT und den in `WhpProtocol.def` deklarierten API-Namen; fehlgeschlagene Fähigkeitsabfragen behalten das typisierte Nichtverfügbarkeitsergebnis. `WhpHostFailureCases.def` enthält unabhängige Erwartungen für Hostfehler bei gleichzeitigem Abbruch sowie für Abfrage-, Installations- und Erfassungsfehler der modernen und älteren XSAVE-APIs. Die Windows-Spezial-CI verlangt 206 native Erfolge: 16 Mapping-, zwei Start-, zehn FP/Kontext-, sieben gemeinsame CPU- und acht Integer-Fälle sowie beide API-Varianten von `NativeInstallRetainsFPStateBeforeAnyGuestExecution`. Letztere vergleichen vollständiges FP/SSE und separat gelesene Metadaten vor der Gastausführung. Fehlende Registrierungen, übersprungene, deaktivierte oder nicht ausgeführte Tests lassen die Prüfung nativer Nachweise fehlschlagen. Die zusätzlichen 26 Prüfungen umfassen alle Fälle aus `X64BitStringTests.cpp` auf beiden Privilegstufen. Windows PE64 verlangt 64 WHP-Prozessfälle und zehn unabhängige native Windows-Vergleichsfälle.

`NeverDMemoryLifecycleTests` wird unabhängig von Unicorn gebaut, auch in rein nativen Konfigurationen. Software-spezifische Projektions- und Gerätefälle werden bei deaktiviertem Unicorn ausdrücklich übersprungen; Tests gemeinsam genutzter CPUs auf dem passenden Host bleiben registriert. `WhpMemoryTests.cpp` isoliert die native Speicher-API mit 16 Fällen aus `WhpMemoryCases.def`: Seiten- oder Projektionsgröße, gemeinsame oder getrennte Allokationen, unberührte oder residente Bytes und ein vorhandener oder fehlender erster virtueller Prozessor. Jeder Fall hält zwei logische Eigentümer am Leben, wechselt ihre abgebildete Partition mehrfach, gibt den inaktiven Eigentümer frei und prüft die weitere Nutzung der verbleibenden Abbildung ohne Neuerstellung. Echte Abbildungsfehler behalten ihren HRESULT und lassen den Test scheitern; dies belegt die Speicher-API, keine Instruktionsausführung.

`X64MachineProbe.def` benennt die fehlgeschlagene Startinstruktion und sämtliche Abweichungen bei Skalaren, TLS, Privileg, x87-Steuerfeldern, physischen FP-Lanes und XMM-Wörtern samt Soll- und Istwerten. `DiagnosticIdentifiesStepFieldAndBothValues` prüft unabhängige erwartete Meldungen. Der Zustandsvergleich bleibt exakt; die Diagnose unterscheidet Übertragungsverlust von Ausführungsfehlern und erklärt einen fehlgeschlagenen nativen Test nicht zum Erfolg.

`WhpResourceTests.cpp` prüft Wiederverwendung, Freigabe vor Ersatz, Fehlererholung und Wettläufe mit Frist oder Stopp. `LogicalCPUSwitchingRestoresPhysicalFPAndTLS` führt zwei Maschinen abwechselnd in beiden Privilegmodi aus, prüft unabhängige physische x87/XMM- und FS/GS-Zustände und setzt die verbleibende Maschine nach Freigabe der anderen fort. Windows-CI verlangt beide WHP-Privilegfälle.

`NEVERD_ENABLE_SEMANTIC_TESTS` ist standardmäßig `ON` und steuert die Testgruppe in `unittests/semantic` samt ihren Sammelzielen. Für native CPU-Tests ohne Unicorn bleibt `BUILD_TESTING=ON` aktiv; zusätzlich werden `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` und `NEVERD_EMULATION_BACKEND_UNICORN=OFF` gesetzt. Native KVM/WHP-Tests bleiben damit verfügbar, auch unter Windows ARM64/MSVC mit geeigneten SDK-Headern. Unicorn unter Windows ARM64 benötigt weiterhin eine ARM64-LLVM-MinGW-Toolchain. Diese Trennung des Builds belegt noch keine native ARM64-Ausführung.

Der native CPU-CI-Checkout initialisiert Capstone-Quellen der festgelegten Revision und verwendet das geprüfte LLVM-Paket. Mit `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` und deaktiviertem Unicorn-Adapter benötigen Konfiguration, Build und Linken der CPU-Tests keine Unicorn-Quellen. Signaturen und der externe Korpus werden ebenfalls nicht benötigt. Die Standard-CI aktiviert weiterhin die vollständige semantische Testgruppe.

Die vorhandene `ci.yml` bietet auf ihrem Windows-x64-Runner den ausdrücklich gewählten manuellen Modus `native_cpu_only`. `NativeCPUTests.def` wählt elf Testziele; `run_native_cpu_ci.py` baut sie vor dem gefilterten CTest und speichert Inventar, JUnit, Protokolle und Zusammenfassung. Gemeinsame CI-Parser unterscheiden bestanden, fehlgeschlagen, übersprungen, deaktiviert und nicht ausgeführt. Jeder deklarierte native WHP-Abbildungsfall muss entdeckt und ausgeführt werden; fehlende oder übersprungene native Evidenz lässt den gezielten Job scheitern. Die standardmäßige CI mit LLVM-Quellbau bleibt unverändert. Protokolltests und Kompilierung ersetzen keine native WHP- oder ARM64-Workload-Validierung.

Mit `native_cpu_only=true` aktiviert `native_driver_tests=true` die `NeverDNativeDriverTests` ohne Unicorn. Vor der Konfiguration prüft `build_wdk_driver_fixtures.py` den vollständigen SHA-256 der offiziellen Microsoft-Pakete WDK/SDK 10.0.26100.6584 und baut 46 normale/CFG/DBG-Treiberimages aus den Originalquellen. `WDKDriverFixtures.def` deklariert Paketidentitäten, Compiler- und Linkerargumente sowie Fixture-Zuordnungen. Unveränderte Microsoft-Dateien und ihre Lizenzen bleiben in den lokalen Build-/Cache-Verzeichnissen; CI lädt nur Build-Metadaten und Protokolle hoch. Das Manifest enthält Werkzeugversionen, Befehle, Quell-/Header-Hashes und Hashes der erzeugten Images.

`NativeDriverTests.def` verlangt 224 WHP-Ergebnisse für alle 112 Workloads aus `DriverBuiltinImages.def` und `DriverBackendParityCases.def`: 26 eingebaute Images, 46 WDK-Images und 40 Anfrageszenarien, jeweils an ursprünglicher und verschobener Adresse. Zusammen mit 206 CPU-Prüfungen und vier gemeinsamen SEH-Fortsetzungsprüfungen sind 434 Ergebnisse verpflichtend. Feste Images müssen weiterhin die erwartete Relokationsablehnung melden. Fehlende oder übersprungene WDK-Images/Szenarien lassen diesen optionalen CI-Job scheitern; in normalen lokalen Builds bleiben externe Fixtures optional. `run_native_cpu_ci.py --with-drivers` protokolliert konfigurierte Testziele und vollständige Inventar-/JUnit-Nachweise. Der Image-Build belegt keine native Windows- oder ARM64-Ausführung. Die folgenden Befehle reproduzieren den Build lokal; der erzeugte Cache lässt sich auch in einen bestehenden Emulationsbuild laden. `206 CPU + 224 WHP + 4 SEH = 434`.

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

`windows-pe64-v1` unterstützt begrenzte Windows-x64/ARM64-Konsolenprozesse mit PEB/TEB, statischem und dynamischem TLS, `DllMain`, benannten Win32-APIs und expliziten azyklischen DLL-Graphen. Gastmodule unterstützen Code-/Datenimporte nach Name oder Ordinal, DIR64, weitergeleitete Exports und echte Loader-Listen. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` und `GetProcAddress` verwenden den konfigurierten Katalog. CRT/GUI, Frame-basiertes Benutzer-SEH, Threads und allgemeine Windows-Kompatibilität bleiben unvollständig; native ARM64-KVM/WHP-Belege fehlen weiterhin.

Eingabebytes und gesamte Image-Ausdehnungen teilen jeweils `memory_limit`; Laufzeit-Mappings zählen zum Image-Budget. Vorbereitung teilt 65,536 Datensätze, 64 MiB Metadatenlesezugriffe, begrenzte Namen und die Arbeitsfrist, ohne harte Host-E/A-Zeitgarantie. Das eigene EXE→DLL→DLL-Fixture prüft Rebasing, Ordinale, gemeinsame Daten, API-Identität, `MEM_IMAGE`, Listen und EXE-TLS-Attach/Detach. `NeverDWindowsProcessTests` enthält das native Windows-Orakel, `NeverDPEProgramExportsTests` ungültige Metadaten und Budgets, `NeverDProcessPublicTests` C-ABI/CLI-Parität. Nicht verfügbare Backends werden explizit übersprungen.

`WindowsProcessLifetime` führt DLL-TLS und danach `DllMain` in Abhängigkeitsreihenfolge aus, anschließend EXE-TLS und Einstieg, auf einer CPU mit gemeinsamem Budget. Jedes Modul erhält einen eigenen TLS-Index und ausgerichteten Block aus dem relokierten, verknüpften Image im gemeinsamen 64-KiB-Bereich. Das reservierte TLS-Argument ist null; `DllMain` erhält bei Start/Prozessende einen undurchsichtigen Nicht-NULL-Wert. Explizites Prozessende trennt fertig initialisierte DLLs in umgekehrter Loaderlisten-Reihenfolge und danach EXE-TLS, auch vor dessen Initialisierung. Start-`DllMain(FALSE)` beendet mit `0xc0000142` ohne Detach. Fehler und erschöpfte Budgets erfinden keine Bereinigung. PE-Einstiegsreturn mit Gast-DLLs benötigt nicht unterstütztes Thread-Ende und stoppt explizit. Ein von null verschiedenes `SizeOfZeroFill` bleibt ausgeschlossen; Nullbytes im tatsächlichen TLS-Template sind unterstützt. DLLs ohne Einstieg erhalten TLS-Attach, aber keine Prozess-Detach-Benachrichtigung.

`WindowsProcessExports` löst statische Imports und `GetProcAddress` über dieselben Namens-/Ordinalidentitäten auf, einschließlich Code, Daten, Aliasen und Weiterleitungsketten. Nur tatsächlich verwendete Startweiterleitungen ergänzen Katalogmodule und Initialisierungsabhängigkeiten; unbenutzte laden keine Dateien. Namen unterscheiden Groß-/Kleinschreibung; fehlende Namen liefern NULL/Fehler 127, direkt abgefragte fehlende Ordinale einschließlich Lücken NULL/Fehler 182 und ein NULL-Abfrageargument Fehler 87, Erfolg erhält LastError. Unbekannte Modulhandles bleiben ununterstützt. Exakte Anbieter-/Namens-API-Einstiege werden einmal aus dem begrenzten Register reserviert. Die Auflösung prüft aktuelle PE-Header und Exportmetadaten jedes Abbilds, lehnt Änderungen oder unlesbare Bytes ab, begrenzt Ketten auf 64 Einträge und teilt verbleibende Metadatenbudgets und die Ausführungsfrist. Eine Weiterleitung auf eine Lücke liefert die Zielbildbasis und erhält LastError; Ordinal null liefert Fehler 87. Die Basis ist eine Datenadresse und erteilt keine Ausführungsrechte für Image-Header. Laufzeitweiterleitungen können konfigurierte Module laden und vor Rückgabe des Ergebnisses initialisieren. Änderungen aktiver Exporttabellen bleiben ununterstützt.

`WindowsProcessLoader` lädt ASCII-DLL-Basisnamen aus `windows.modules` und verwaltet explizite Referenzen, gemeinsame Abhängigkeiten und den Erhalt von Startmodulen. Wiederholte Weiterleitungsabfragen erhöhen die Referenzzahl nicht. Beim erneuten Laden erhält ein Katalogplatz eine neue residente Generation. TLS und `DllMain` laufen auf derselben CPU unterhalb angehaltener API-Stackframes; die Registerwiederherstellung erhält Gastspeicheränderungen und verwendet die aktuelle Rücksprungadresse. Reservierte Zeiger bei dynamischem Attach/Detach sind null. Fehlgeschlagenes Attach beim expliziten Laden liefert nach Bereinigung Fehler 1114, behält aber erfolgreiche unabhängige verschachtelte Ladevorgänge. Entladen gibt Abbild und TLS frei; erneutes Laden stellt Originalbytes her. Fremde Änderungen an Loader-Listen oder TLS-Zeigern werden abgewiesen. Datei-, Abbild- und Metadatenbudgets bleiben auch nach Fehlern kumulativ. Systemanbieter verwenden ihre eingeblendeten PE-Basen als Modulhandles. Dateisuche, Nicht-ASCII-Pfade, `LoadLibraryEx`-Flags, Importzyklen und reentrante Übergänge desselben gerade initialisierten oder entladenen Moduls bleiben ununterstützt.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` verwenden denselben aktuellen Gastumgebungsblock in den PEB-Prozessparametern. ASCII-Namen werden ohne Beachtung der Großschreibung verglichen; Werte sind UTF-16. Änderungen prüfen Eingaben, Kapazität und Schreibrechte vor der Veröffentlichung. Momentaufnahmen bleiben unabhängig von späteren Änderungen und geben ihren Gastspeicher beim Freigeben zurück. Das Modell begrenzt den Block auf 64 KiB; Zeichenketten und Ersetzungen sind begrenzt und prüfen die Ausführungsfrist. Unbekannter Zeigerbesitz, fehlerhafte Blöcke, ANSI-Codepages und überlappende Ersetzungspuffer bleiben ununterstützt. `WindowsEnvironmentTests.cpp` vergleicht eigene x64/ARM64-Fixtures auf verfügbaren Backends; CI verlangt ein unabhängiges natives Windows-Orakel.

`WindowsProcessHeap` verwaltet Allokation, `HeapReAlloc`, Freigabe und Größenabfragen des Prozessheaps gemeinsam. Größenänderungen erhalten die verbleibenden Bytes; `HEAP_ZERO_MEMORY` löscht hinzugefügte Bytes, `HEAP_REALLOC_IN_PLACE_ONLY` verbietet Verschiebungen. Fehlgeschlagene Größenänderungen erhalten den alten Block und liefern NULL mit `ERROR_NOT_ENOUGH_MEMORY` (8), entsprechend den nativen Beobachtungen. Separate Seiten geben bei Verkleinerung und Freigabe Kapazität zurück; vorbereitetes Wachstum und begrenzte Kopien prüfen die Ausführungsfrist. Eigene Heaps, Ausnahmeflags, unbekannter Besitz und unzugängliche Kopier- oder Löschbereiche stoppen ausdrücklich. `WindowsHeapTests.cpp` prüft beide ISAs, erzwungene Verschiebung, Budgetwiederverwendung und Fehleratomarität; CI führt dieselbe eigene EXE auch auf nativem Windows aus. PE-Sammeltests nutzen das gemeinsame CTest-Zeitlimit von 120 Sekunden; jeder Gast behält sein eigenes endliches Budget. Die Heap-Fixture erlaubt pro Prozess 20 Sekunden für vollständige Datenprüfungen mit WHP.

`WindowsSystemModules` erzeugt begrenzte PE64-Modellabbilder für `ntdll.dll`, `kernelbase.dll` und `kernel32.dll` auf beiden ISAs. ASCII-Abfragen über `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` und `GetProcAddress` teilen deren eingeblendete Basisadressen; PEB/LDR und `MEM_IMAGE` beschreiben dieselben Abbilder. Statische Importe, Namensabfragen und Gastweiterleitungen nutzen dieselben API-Gates und Exportauflösung. Anbieter bleiben fest resident, haben keine Gastinitialisierungs-Callbacks und verhindern nach Entladen gewöhnlicher Gast-DLLs keine Rückkehr vom Einstiegspunkt. Geänderte Header oder Exportmetadaten stoppen die Suche. Unmodellierte Systemexportnamen und von null verschiedene Systemordinale stoppen ausdrücklich; reine Schreibungsabweichungen modellierter Namen sowie leere Namen liefern Fehler 127, eine NULL-Abfrage liefert 87. Erzeugte Bytes und Adressen sind Modellregeln; Windows-Versionslayouts, native Ordinale und anbieterübergreifende Aliase werden nicht rekonstruiert. `WindowsSystemTests.cpp` vergleicht eigene x64/ARM64-EXEs mit nativem Windows und beobachtet acht Rückgaben des Anfangsthreads unabhängig.

`WindowsProcessExceptions` implementiert `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` und `RaiseException` auf derselben CPU mit dem Prozessbudget. Geordnete Handler dürfen Registrierungen ändern, verschachtelte Ausnahmen auslösen, modellierte APIs aufrufen, DLLs laden und den Prozess beenden. Datenzugriffsverletzungen auf x64/ARM64 und x64-Ganzzahldivisionsfehler können nach Prüfung der Gaständerungen an `CONTEXT` fortgesetzt werden; allgemeine Register, SIMD und unterstützter FP-Zustand bleiben erhalten. Softwareausnahmen kehren über eine echte Rücksprunganweisung im modellierten Anbieter zurück. Grenzen sind 128 aufbewahrte Registrierungen und 16 verschachtelte Frames. Ungültige Ergebnisse, geänderte Ausnahmezeiger, nicht unterstützte Felder und Grenzüberschreitungen scheitern ausdrücklich. Stackbasiertes SEH/Unwinding, Debugger-Zustellung und Ausführungs-/Schutzseitenfehler bleiben offen. `WindowsExceptionTests.cpp` vergleicht eigene EXE/DLL-Szenarien mit nativem Windows; native ARM64-KVM/WHP-Belege fehlen weiterhin. Datensätze für Softwareausnahmen tragen `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), unabhängig vom durch den Aufrufer gesetzten Flag für nicht fortsetzbare Ausnahmen; die ursprüngliche Windows-Programmdatei prüft die exakten Flagwerte für Software- und Hardwareausnahmen.

`AddVectoredContinueHandler` und `RemoveVectoredContinueHandler` verwalten eine eigene geordnete Liste; beide Handlerfamilien teilen die Grenze von 128 aufbewahrten Registrierungen. Akzeptiert ein vektorisierter Ausnahmehandler die Fortsetzung, sehen die Fortsetzungshandler denselben veränderbaren Ausnahmedatensatz und `CONTEXT`. Die abschließende Kontextprüfung erfolgt nach diesen Rückrufen, einschließlich verschachtelter Ausnahmen und DLL-Benachrichtigungen. Handles lassen sich nicht über die andere Handlerfamilie entfernen. `WindowsContinuationTests.cpp` vergleicht eigene EXE-Szenarien für Reihenfolge, vorzeitiges Ende, Listenänderungen, Kontextreparatur, Verschachtelung, Loader-Rückrufe und Prozessende mit nativem Windows. Der geprüfte Windows-x64-Vektorpfad erlaubt die Fortsetzung mit `EXCEPTION_NONCONTINUABLE`; dies belegt kein stackbasiertes SEH-Verhalten. Native ARM64-Ausführung bleibt ungeprüft.

`WindowsDynamicTests.cpp` vergleicht originale x64/ARM64-DLLs und EXEs mit unabhängigen nativen Windows-Beobachtungen: Referenzen, gemeinsame Abhängigkeiten, verschachteltes Laden, Fehlerbereinigung, Weiterleitungen, Prozessende, DLLs ohne Einstieg und frisches TLS beim Neuladen. Zusätzliche Tests lehnen manipulierte Loader-Metadaten und veraltete Codezeiger ab, erhalten kumulative Budgets und lassen unterbrochene API-Ergebnisse unvollständig. Windows-CI verlangt natives Orakel und WHP-Fälle; Cross-Kompilierung und Unicorn ARM64 belegen keine native ARM64-Ausführung.

Eine fehlende Bibliothek in der Weiterleitungskette von `GetProcAddress` ergibt Fehler 127; ein explizites `LoadLibrary` für ein fehlendes Katalogmodul ergibt 126. Das native Orakel und jedes verfügbare Backend prüfen alle 41 deklarierten Ladeszenarien. Unter Windows wird die Rückkehr nach dem Entladen aller DLLs je DLL-Variante 16-mal beobachtet. Fehlgeschlagene Initialisierung über eine `GetProcAddress`-Weiterleitung liefert nach Bereinigung ebenfalls 127. Prozess-Detach-Callbacks erhalten den Stackinhalt des beendenden Aufrufers.

`WindowsExportTests.cpp` prüft mit ursprünglichen x64/ARM64-DLLs und einem EXE weitergeleitete Code-/Daten-/Ordinalaufrufe, Aliase, Initialisierungsabfragen, Rebasing, Groß-/Kleinschreibung, fehlende Exports, LastError, Zyklen, nicht residente Ziele, ungültige Zeiger und Metadatenänderungen nach erfolgreichen Abfragen. Dasselbe EXE dient einem unabhängigen nativen Windows-Orakel; WHP-Fälle sind in nativer CI Pflicht. C-ABI/CLI-Tests vergleichen vollständige Berichte. Native ARM64-Hardwarebelege stehen noch aus. EXE-Varianten mit und ohne Exporttabelle prüfen beide Graphen, PEB- und Detach-Reihenfolge sowie Fehler für Namen, Ordinale und NULL.

`WindowsLifetimeTests.cpp` vergleicht feste Traces mit unabhängigen nativen Windows-Prozessen und KVM/WHP/Unicorn: normales Ende, Einstiegsreturn, beide DLL-Fehler, vier frühe Enden und DLLs ohne Einstieg. Hinzu kommen Callback-Fehler, gemeinsame Budgets, relokierte TLS-Felder und Gesamtkapazität. Die native Return-Prüfung behält das Handle des Anfangsthreads und prüft 64-mal dessen Exitcode sowie die genaue Thread-/Prozess-Benachrichtigungsfolge. Verbleibende Kindthreads werden nach der Beobachtung beendet; der Prozess-Exitcode gilt nicht als Einstiegsrückgabe.

Der virtuelle Windows-Speicher ergänzt `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` und `FlushInstructionCache` für den aktuellen Prozess. Die OS-Schicht verwaltet Reservierungen; `AddressSpace` bleibt maßgeblich für zugesicherte Seiten, Zugriffsrechte und deren Speicher. Tests prüfen Codeänderungen, Zugriffsfehler und die Wiederverwendung des Speicherbudgets.

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

Regressionen prüfen exakte und zu kleine Budgets, Teilwörter, beide Byteordnungen, unberührte Speicheridentitäten, vollständige affine Slotgrenzen, spätes Überschreiben und Standardinvalidierung. C API/CLI prüfen v6-Kompatibilität und ungültige Bereiche. HighC und LLVMC laufen mit O0/O2 und prüfen Rückgabe, Speicher, Stack und erhaltenen Zustand; dies ist kein natives Äquivalenzzertifikat.

Regressionen prüfen Register- und Rahmenphasen, beide Bytefolgen, erreichbare ungültige Arme, spätere Vorgänger, ausgeschöpfte innere Guards und benachbarte Suchgrenzen. Besuchsgrenzen-Tests prüfen arithmetische Beziehungen, verschachtelte Schleifen, Dekodiermodi, sequenzielle Übergänge, Gesamtbudget und Legacy-Vorrang. Der CLI führt beide C-Wege und Quell-ABIs mit O0/O2 aus; C/Python v8 prüfen Layouts, ungültige Felder und ignorierte zukünftige Endfelder. Regressionen prüfen außerdem, dass hinter großen unabhängigen endlichen Selektoren Suchbudget für native Guards bleibt und die letzte zulässige Verfeinerung einem bereits nominierten Produzenten zugutekommt.
