**Sprachen**: [English](../interpreter-recovery.md) | [简体中文](../zh-CN/interpreter-recovery.md) | [繁體中文](../zh-TW/interpreter-recovery.md) | [日本語](../ja/interpreter-recovery.md) | [한국어](../ko/interpreter-recovery.md) | [Français](../fr/interpreter-recovery.md) | [Deutsch](interpreter-recovery.md) | [Español](../es/interpreter-recovery.md) | [Italiano](../it/interpreter-recovery.md) | [Русский](../ru/interpreter-recovery.md) | [العربية](../ar/interpreter-recovery.md)

# Quelltextrekonstruktion aus Interpretern

[← Dokumentationsübersicht](README.md)

Die experimentelle Interpreter-Spezialisierung entfernt statisch aufgelöste
Dispatch-Vorgänge aus einer fertig gelinkten x64-Funktion. Laufzeiteingaben,
Speichereffekte, Verzweigungen und Schleifen bleiben erhalten. Grundlage ist
die Instruktionssemantik, nicht die Erkennung von Handler-Signaturen oder die
Opcode-Tabelle eines bestimmten Schutzprogramms.

```sh
neverd decompile program --func vm_entry --devirtualize \
  --recovery-report recovery.json -o recovered.c
neverd decompile program --func vm_entry --devirtualize \
  --llvm -o recovered-llvm.c
```

`--vm-control` wählt vollständige allgemeine Register, die Interpreter-Kontexte
unterscheiden. Die Option darf mehrfach vorkommen und gibt keine konkreten
Werte vor. Wählen Sie beispielsweise einen Bytecode-Zeiger, dessen Wert der
Einstiegscode festlegt. Eine als Zähler verwendete Laufzeiteingabe muss dynamisch
bleiben. Fehlende Kontexttrennung kann die Rekonstruktion stoppen, wenn
verschiedene Zeigerwerte zusammentreffen; die Engine darf dies nicht durch
Erraten eines Dispatch-Ziels ausgleichen. Für einen auf den Stack ausgelagerten
Zeiger wählt `--vm-control-stack=-16:8` acht Bytes bei Eintritts-RSP minus 16.
Der Offset bezieht sich auf den Funktionseintritt, nicht auf den inzwischen
veränderten Stackpointer.

Die C-API heißt `neverd_devirtualize_source_v1()` und ist in
`neverd/sdk/NeverDCAPIDevirtualize.h` deklariert. Sie führt eine separate
Transaktion aus, ohne den gewöhnlichen Dekompilationscache der Sitzung zu
verändern. Bei einem Fehler wird kein Quelltext zurückgegeben; eine
JSON-Diagnose kann dennoch vorliegen. Beide zugewiesenen Zeichenketten werden
mit `neverd_free_string()` freigegeben.

## Automatische Erkennung des Steuerzustands

Die CLI und alle Versionen der C-APIs zur Quelltextwiederherstellung aktivieren die automatische Erkennung standardmäßig. In der anbieterunabhängigen C++-API gilt weiterhin `SpecializationOptions::DiscoverControlState = false`; Aufrufer können `true` setzen. Manuelle Hinweise mit `--vm-control` und `--vm-control-stack` bleiben optionale Kontextschlüssel. Gewöhnliche automatisch erkannte Felder bewahren begrenzte gemeinsame endliche Werterelationen, ohne Kontextschlüssel anzulegen oder Laufzeiteingaben auf Stichprobenwerte festzulegen. Zähler bleiben dynamisch, sofern die folgende selektive Speicherverfeinerung ihre bewiesenen Konstanten nicht benötigt.

Die Erkennung verfolgt ungelöste Steuer- und Adressabhängigkeiten zu strukturierten Registereingaben am Knoteneingang und zum Entstehungsursprung von Speichereingaben, einschließlich schmaler Bytebereiche. Ein Frame-Slot wird nur vorgeschlagen, wenn dieser Ursprung seit Knoteneintritt unveränderten Speicher in einem exakten Bereich relativ zum Frame am Funktionseintritt bezeichnet. Spätere Lesezugriffe auf gleiche oder von Schreibzugriffen weitergereichte Werte erzeugen keine zusätzlichen Eingabeabhängigkeiten; nach einer Speicherinvalidierung neu entstandene unbekannte Bytes gelten nicht als Eingangsslots. Die bisherigen Leseaufzeichnungen bleiben für andere Analysen verfügbar. Fehlende Abhängigkeiten führen zu einer erneuten Analyse ab dem Funktionseintritt. Jede beibehaltene Relation benötigt weiterhin einen vollständigen Nachweis des endlichen Wertebereichs der von ihr eingeschränkten Bits; mögliche Alias-Schreibzugriffe verwerfen weiterhin Speicherfakten. Beliebiger externer Speicher und unbegrenzte Werterelationen werden dadurch nicht endlich.

Produzentenanforderungen bewahren Bitmasken über Knotengrenzen hinweg, ohne sie auf ganze Bytes zu erweitern; arithmetische Operationen berücksichtigen weiterhin konservativ das Präfix der niederwertigen Bits, die einen Übertrag bei Addition oder Subtraktion in ein angefordertes Bit bewirken können.

Verhindern bereits verfolgte Speicherabhängigkeiten wiederholt einen exakten Adressnachweis, kann die Verfeinerung ihre bewiesenen eingehenden Konstanten zusätzlich als Kontextschlüssel verwenden. Sie teilt keine mehrwertigen Tupel in neue Kanten auf und fügt keine Gast-Speicherlesezugriffe zur Kontextauswahl hinzu: Ein endlicher Wertebereich beweist nicht, dass ein zusätzlicher Zugriff sicher ist. Dynamische oder unbegrenzte speicherabhängige Zustände können die Wiederherstellung daher weiterhin innerhalb der gesetzten Grenzen stoppen.

Rückwärts gerichtete Anforderungen an Produzenten sind durch den nativen Knoteneingang, den Instruktionsmodus sowie Feldart und Bytebereich bestimmt. Nur eine Kante, deren Nachfolger das Feld benötigt, verfolgt dessen Produzentenabhängigkeiten weiter, auch bei endlichen, aber noch zu ungenauen Wertebereichen. Dies kann begrenzte Neustarts ab dem Eingang auslösen, ohne einem wiederverwendeten physischen Register eine einzige globale Rolle zuzuweisen. Die Abhängigkeitserkennung ist begrenzt und unvollständig; eine Wiederherstellung ohne manuelle Hinweise ist nicht für jeden Interpreter garantiert.

Die Projektion gewöhnlicher automatischer Felder ist ebenfalls auf die Anforderungen des Zielknotens beschränkt. Ein nicht konstantes Feld wird nur dann in die gemeinsame endliche Werterelation einer Kante aufgenommen, wenn deren Ziel es benötigt. Manuelle Felder und automatisch zu Kontextschlüsseln erhobene Felder werden weiterhin global projiziert. Bekannte konstante Bytes, exakte Zeiger relativ zum Frame am Funktionseintritt und Herkunftsfakten bleiben unabhängig von diesen Anforderungen erhalten. So vervielfachen unabhängige Felder aus unterschiedlichen Handlerphasen nicht die Wertekombinationen der Relation. Die konfigurierten Budgets und die erforderlichen Nachweise für Kontrollflussziele, Speicheradressen und den Rückkehrzustand bleiben unverändert.

Bei gewöhnlichen automatischen Feldern schränkt jede Tupelspalte nur die angeforderten Bits ein und trägt die zugehörige Maske. Zusammenführungen behalten nur Bits, die auf allen eingehenden Pfaden eingeschränkt sind. Andere Bits desselben Bytes bleiben Laufzeitwerte: Der Speicherbereich eines Feldes bescheinigt keinen vollständig bewiesenen endlichen Wertebereich für den gesamten Bereich. Ein Byte wird erst dann konstant, wenn alle acht Bits als konstant bewiesen sind.

Manuelle Felder und zu Kontextschlüsseln erhobene Felder versuchen weiterhin zunächst global, Relationen über ihre volle Breite zu beweisen. Bleibt dieser Nachweis ohne Ergebnis, können sie stattdessen eine Relation für die angeforderten Bits behalten. Dabei werden weder unbewiesene Bytes für einen Kontextschlüssel bereitgestellt noch erschöpfte globale Budgets umgangen.

Beim Neustart mit einem neuen Graphen können gewöhnliche automatische Speicherbereiche, die vollständig in anderen Feldern liegen, diese breiteren Felder mitbenutzen. Die ursprünglichen Phasenpositionen und Bit-Anforderungen bleiben unverändert. Manuelle Felder, Kontextfelder und direkt von einer ungelösten Speicheradresse vorgeschlagene Felder behalten ihre exakten Bereiche. `MaxControlFields` begrenzt die nach dieser Normalisierung tatsächlich beibehaltenen Felder. `DiscoveredControlFields` bleibt kumulativ und kann die aktive Anzahl übersteigen; das Feldlimit und die globalen Arbeitsbudgets werden nicht erhöht.

Ein gewöhnliches automatisches Feld mit vollständig bewiesenem endlichem Wertebereich nimmt nicht sofort alle seine Produzenten als Kontrollfelder auf. Die Erkennung merkt sich diese Abhängigkeitskandidaten und aktiviert sie nur, wenn die Wiederherstellung weiterhin blockiert ist und die unmittelbare Verfeinerung keine Kandidaten liefert. Zu Kontextschlüsseln erhobene Felder verfolgen ihre Produzenten weiterhin sofort. Erkennungsbesuche, Neustarts und Beweisarbeit behalten ihre bisherigen kumulativen Budgets.

Ein vollständig bewiesener endlicher Wertebereich kann auch einzelne Bytes als konstant nachweisen, obwohl das ganze Wort variiert. Beispielsweise hat der Wertebereich `{0, 0x100}` ein konstantes niederwertigstes Byte. Nur Bytes, die in jedem aufgezählten Tupel gleich sind, werden gemäß der Byte-Reihenfolge des Ziels als konstant erhalten; andere Bytes bleiben dynamisch. Teilweise Aufzählungen, unbekannte Solver-Ergebnisse oder erschöpfte Beweisbudgets liefern keine solchen Fakten.

Endliche Wertebereichsnachweise können innerhalb eines Wiederherstellungslaufs einschließlich seiner Verfeinerungsneustarts wiederverwendet werden. Ein begrenzter Cache vergleicht den vollständigen geordneten Ausdrucks-DAG bis auf konsistente Umbenennung freier Variablen; gemeinsame Variablen, Breiten, Konstantenbits, Operatorparameter und das Projektionslimit bleiben Teil des Schlüssels. Gespeichert werden nur vollständige Wertebereiche und Nachweise, dass ein Bereich sein Limit überschreitet. Unbekannte oder teilweise Ergebnisse werden nicht gespeichert; bei fehlendem Eintrag oder unzureichender Kapazität gilt der normale Beweisweg. Alle globalen Budgets bleiben wirksam, und `solverQueries` zählt tatsächliche Solver-Aufrufe.

Unter demselben Prädikat bestimmen der vollständige Wertebereich einer einzigen variierenden Spalte und bewiesene Einzelwerte aller übrigen Spalten die exakte gemeinsame Relation, sofern Erreichbarkeit nachgewiesen ist. Mehrere variierende Spalten benötigen weiterhin einen gemeinsamen Nachweis. Eine maskierte Spalte mit vollständigem Bitwertebereich darf nur entfallen, wenn ihre Eingabebits nachweislich vom Prädikat und allen anderen Spalten unabhängig sind. Gemeinsame Eingaben, unbekannte Ergebnisse und teilweise Aufzählungen rechtfertigen niemals die Annahme eines kartesischen Produkts.

Standardwerte sind `MaxControlFields = 16` für manuelle und automatische Felder zusammen, `MaxControlRefinements = 16` und `MaxDiscoveryVisits = 65536`. Neustarts teilen globale Budgets für Knoten (einschließlich synthetischer), Operationen, Auswertungen, Solver-Abfragen und Erkennungsbesuche. `contexts` zählt native Kontexte und wird wie `evaluatedOperations`, `nodeEvaluations` und `solverQueries` über alle Versuche summiert. Kontexte pro Adresse, aktive native Rückkehrslots, Felder und Tupel bleiben strukturelle Grenzen je Versuch. Das Abfragelimit bleibt 4096. Bei Erschöpfung wird kein Teilergebnis veröffentlicht; `residualBlocks` beschreibt nur den abschließenden Restgraphen.

Die CLI-Option `--vm-max-refinements=N` verlangt eine positive ganze Zahl und hat den Standardwert 16. C-Aufrufer können dieselbe Grenze über `neverd_devirtualize_source_v2()` oder `neverd_devirtualize_machine_source_v2()` setzen: `neverd_devirtualize_options_v2` mit Nullen initialisieren, `base.struct_size = sizeof(neverd_devirtualize_options_v2)` und anschließend `max_control_refinements` setzen (null behält den Standardwert 16). Das eingebettete `base` enthält die v1-Optionen; beide reserved-Mitglieder müssen null bleiben. Bestehende v1-Layouts und Einstiegspunkte bleiben unverändert und ignorieren angehängte Erweiterungen. Andere Arbeits- und Beweisbudgets gelten weiterhin.

Der JSON-Bericht ergänzt `discoverControlState`, `maxControlRefinements`, `maxDiscoveryVisits`, `discoveredControlFields`, `discoveredContextFields`, `controlRefinements` und `discoveryVisits` für Aktivierung, Grenzen und Analyseaufwand. Die Erkennung von Feldern allein beweist keine erfolgreiche Wiederherstellung.

## Ausführungsvertrag

Der Binäradapter akzeptiert derzeit fertig gelinkte x64-ELF- und PE-Abbilder an
ihren gemappten Adressen. Mappings, Bytes und Berechtigungen müssen unverändert
bleiben; gleichzeitige Änderungen sind ausgeschlossen. Striktes Lifting bei
Bedarf folgt dem erreichbaren Maschinencode. Nicht unterstützte Instruktionen,
Aufrufe, undurchsichtige Operationen, geordnete Speicherzugriffe, nicht
aufgelöster Kontrollfluss und sprachabhängige Ausnahmebehandlung stoppen die
Rekonstruktion.

Die PE-Rekonstruktion erfordert vollständige Abbildmetadaten einschließlich aller Relokationen und Ausnahmeeinträge. Die CLI lädt sie vor der Anwendung von `--func`; C-API-Aufrufer dürfen die Sitzung nicht zuvor mit `neverd_session_restrict_function()` einschränken. Der Binäradapter lehnt Abbilder ab, die mit einer eingeschränkten Funktionsmenge geladen wurden: Ausgelassene Metadaten beweisen weder die Abwesenheit von Fixups noch die von Ausnahmekanten.

Nur vollständige, dateigestützte und schreibgeschützte Bereiche ohne
überlappende Mappings oder Loader-Fixups dürfen konstante Abbildlesevorgänge
begründen. Schreibbare Tabellen, nicht aufgelöste Relokationen und punktuelle
Laufzeitschnappschüsse belegen keine Unveränderlichkeit. COPY-Relokationen und strukturell unvollständige Ausnahmedirektoren werden
abgelehnt. Ist das PE-Verzeichnis samt Funktionsbereichen vollständig, hindert
ein unbekannter Handler in einer anderen Funktion die Analyse nicht; erreicht
die Wiederherstellung dessen Codebereich, wird sie abgelehnt. Der Gültigkeitsbereich
verlangt normale ABI-Rückgaben: Der Zielbereich jedes Schreibzugriffs externen
Ursprungs muss vom Speicherplatz der Eintritts-Rücksprungadresse getrennt sein.
Dies ist eine ausdrückliche Voraussetzung an Aufrufer und Umgebung, auch bei
Adressen, die aus externen Ganzzahlen berechnet werden. Fehlende Herkunft aus
dem Stackframe beweist keine numerische Überschneidungsfreiheit. Aus dem Frame
abgeleitete Schreibadressen müssen diese Trennung nachweisen; bei der Rückgabe
muss der ursprüngliche Stackpointer wiederhergestellt sein. Herkunftsinformation
übersteht Auslagerungen auf den Stack und Zusammenführungen. Der Verlust eines
affinen Ausdrucks macht daraus keinen externen Zeiger. Stack-Pivots,
Rücksprünge mit Bereinigung der Argumente durch den Aufgerufenen und RET-basierter
Dispatch werden derzeit abgelehnt. Der Binäradapter erzwingt die
Little-Endian-Semantik von x64.

Die Ausgabe umfasst Quelltext und IR für die Analyse. Sie weist keine Sicherheit
für Relokation, Stack-Unwinding, asynchrone Ausnahmen oder binären Ersatz nach.
Der Patch-Modus lehnt diese Option ab. Eine Unterstützung aller Interpreter
oder Schutzkonfigurationen wird nicht zugesichert.

Die exakten x64-Formen von `PUSHFQ`/`POPFQ` bleiben im Restprogramm erhalten. Die Analyse behandelt jeden Schnappschuss der Maschinenflags als unbekannten Laufzeitwert; der Lifter fügt die separat modellierten arithmetischen Flags hinzu. Auch das Wiederherstellen der Flags bleibt ein Laufzeiteffekt. Für eine aus unbekannten Flags abgeleitete Adresse gilt der Vertrag zur Überschneidungsfreiheit externer Zeiger mit der Rücksprungadresse nicht; ein daraus abgeleiteter Dispatch ohne nachweislich endliche Ziele schlägt weiterhin fehl.

Vor einem vollständigen Flags-Schnappschuss muss jedes modellierte arithmetische Flag und das Richtungsflag innerhalb der rekonstruierten Funktion definiert sein. Auch ein direkter Flag-Lesezugriff braucht auf jedem erreichbaren Vorgängerpfad eine Definition, selbst wenn die symbolische Vereinfachung seinen Wert aufhebt. Andernfalls wird die Rekonstruktion abgelehnt, statt C mit einer Falle für ein unbekanntes Register auszugeben.

LowIR-Temporärwerte gelten nur innerhalb einer gehobenen nativen Anweisung. Jedes gelesene Byte muss zuvor in derselben Anweisung definiert worden sein; ein wiederverwendeter Offset aus einer früheren Anweisung oder ein algebraisch aufgehobener undefinierter Wert ist kein Beleg für gültigen Quellcode. Eintrittskonstanten dürfen nur physische Register binden.

## Aktuelle Grenzen

Eingabeabhängige Bytecode-Adressen und Beziehungen zwischen Decoder-Zuständen werden nur unterstützt, wenn die erforderlichen endlichen Wertebereiche und Korrelationen innerhalb der konfigurierten Grenzen nachweisbar sind. Daraus folgt keine Unterstützung beliebiger indirekter Decodierschemata. Dynamische Verzweigungen und Schleifen können rekonstruiert werden, wenn jedes Dispatch-Ziel bewiesen ist; gewöhnliche Zweigabdeckung genügt dafür nicht. Nicht aufgelöster Kontrollfluss und ausgeschöpfte erforderliche Beweisbudgets sind Fehler: Es wird weder rekonstruierter Quelltext noch ein teilweiser Ersatz veröffentlicht. Native Hilfsaufrufe, Ausnahme- und Wiedereintrittsgrenzen, veränderlicher Code und andere Architekturen bleiben außerhalb des Ausführungsvertrags.

## Gemeinsame Implementierung

`SpecializationProvider` liefert vollständig geliftete Instruktionen und
Nachweise für unveränderliche Lesezugriffe. `NeverDInterpreterSpecialization`
verwendet die vorhandene `SymExec`-Semantik zur partiellen Auswertung von
Ganzzahl- und Kontrolloperationen. Der Binäradapter verantwortet Mappings und
Instruktionsdecodierung; er implementiert keinen zweiten Instruktionsauswerter.

Bei einer symbolischen Leseadresse mit endlichem Wertebereich zählt der integrierte Bitvektor-Solver unter den aktuellen Bedingungen mögliche Adressen auf. Die Menge wird erst akzeptiert, wenn ein abschließendes UNSAT beweist, dass keine weitere Adresse möglich ist, und für jede Adresse ein vollständiger Nachweis eines unveränderlichen, nicht fehlschlagenden Lesezugriffs vorliegt. Ein solcher Zugriff kann in LowIR durch die einmalige Erfassung der Adresse und eine exakte SELECT-Kette ersetzt werden; gewöhnliche nicht zertifizierte Lesezugriffe bleiben dynamisch. Stichproben ersetzen niemals die vollständige Menge. Ausgewählte Kontrollregister und Eintritts-Frame-Slots können begrenzte gemeinsame Wertetupel über Knotengrenzen bewahren, etwa die Beziehung zwischen Cursor und Decodierschlüssel. Zusammenführungen und Erweiterungen bleiben konservativ. Einzelne SAT-Modelle oder unbekannte Solver-Ergebnisse beweisen keine vollständige Adress- oder Zielmenge. Der optionale Z3-Backend wird dafür nicht benötigt.

Ein Knoten wird durch seinen nativen Cursor, den Instruktionsmodus und die ausgewählten konstanten Kontrollregister und Eintritts-Frame-Slots bestimmt. Weitere byteweise Fakten werden durch Schnittmengenbildung zusammengeführt. Schwächt sich ein eingehender Fakt ab, wird der Knoten erneut ausgewertet. So bleiben Programmschleifen als Schleifen erhalten, statt jede beobachtete Iteration einzeln zu entfalten. Alle erreichbaren Werte eines indirekten Ziels müssen zu einer begrenzten, nachweislich vollständigen Menge gehören. Die ausgewählten Ziele werden zu expliziten Vergleichen im Restprogramm und zu CFG-Kanten.

Dynamische Operationen sowie gewöhnliche Lese- und Schreibzugriffe verbleiben
in LowIR. Skalare Konstanten, affine Zeiger relativ zum Eintrittsframe und nachweislich
konstante Frame-Bytes dürfen Knotengrenzen überschreiten. Andere Ausdrücke
werden verworfen, statt sie unbegrenzt zu erweitern. Frame-Speicher verwendet
die konservative Alias-Invalidierung des vorhandenen symbolischen Zustands.
Schreiben über einen unbekannten, möglicherweise überlappenden Zeiger
invalidiert widersprüchliche Fakten. Dabei wird weder ein Stack-Slot als privat
angenommen noch seine Wirkung aufgrund eines unbewiesenen No-Alias-Vertrags
entfernt.

Eindeutige synthetische Instruktionslabels unterscheiden geklonte Kontexte. Die
ursprünglichen Instruktionsgrenzen bleiben in einer separaten Herkunftstabelle;
originale Relokations-, Ausnahme- oder Sprungtabellennachweise werden nicht auf
neue Vorkommen übertragen. Das rekonstruierte LowIR durchläuft vor der
Aufteilung in HighC und LLVM die gewöhnliche LowIR-zu-MedIR-Konvertierung.
Register-, Stack-, CFG-, SSA- und ABI-Behandlung bleiben dadurch gemeinsam.
Der HighC-Pfad erfordert zusätzlich eine erfolgreiche MedIR-Verifikation.

Budgets begrenzen Knoten, Kontexte pro Adresse, Operationen, Knotenauswertungen
und endliche Zielmengen. Bei Budgetüberschreitung oder nicht unterstützter
Semantik wird keine Restfunktion veröffentlicht. Ein vollständiger
Kontrollflussgraph ist von erfolgreicher Quelltextausgabe zu unterscheiden;
die öffentliche API prüft beide Ergebnisse und meldet sie getrennt.

Endliche Leseadressmengen, gemeinsame Kontrolltupel und die Anzahl der Kontrollfelder haben eigene Grenzen. Ein globales Limit für Solver-Abfragen sowie Limits pro Abfrage für Gatter, Konflikte, Propagationen und Besuche überwachter Literale begrenzen den Beweisaufwand; ein Limit für symbolische Knoten begrenzt das Ausdruckswachstum. Der JSON-Bericht enthält diese Budgets sowie `solverQueries` und `relationalWidenings`.

## Nachweise und Tests

Der optionale lokale JSON-Bericht enthält den Eingabehash, gewählte
Kontrollgrößen, Budgets, Status, Arbeitszähler, die Anzahl verbleibender Blöcke,
ursprüngliche Instruktionsorte und die bei der Rekonstruktion verwendeten
unveränderlichen Bytes. Er enthält aus der Eingabe abgeleitete Informationen
und wird ausschließlich an den angeforderten lokalen Pfad geschrieben.

Die öffentlichen Tests verwenden eigenständig entwickelte Maschinen mit
Register- und Stack-Dispatch, jeweils mit Arithmetik, Verzweigungen samt
Zusammenführung und Laufzeitschleifen. Ein unabhängiges vorzeichenloses Oracle
prüft Rückgabewerte, Speicherzugriffe, Überträge und Borrows sowie
Ausgabeschutzwerte. Rekonstruierte HighC- und LLVMC-Quellen werden mit O0/O2 und
Traps für undefiniertes Verhalten kompiliert und gegen dieses Oracle ausgeführt.
Negativfälle prüfen nicht aufgelösten und schreibbaren Dispatch, inkompatible
Byte-Reihenfolge, Ausnahmemetadaten und Budgets.

Weitere eigenständige Fixtures mit endlichen Adressen verwenden eingabeabhängig gewählte schreibgeschützte Datensätze, zusammenhängende Cursor-/Schlüsselfelder und denselben Handler an verschiedenen virtuellen Positionen. Sie decken Verzweigungen mit Zusammenführung und Schleifen ab, deren Datensatzauswahl vom aktuellen Programmzustand abhängt. Ihr unabhängiges natives Oracle verwendet SysV und Win64; beide rekonstruierten C-Pfade werden unter O0/O2 mit Fallen für undefiniertes Verhalten und Ausgabewächtern geprüft. Fehlende Lesezertifikate oder unzureichende Beweisbudgets dürfen kein Teilergebnis veröffentlichen.

Gezielte Testziele stehen in [testing.md](testing.md).

## Wiederherstellung mit explizitem Maschinenzustand

`--devirtualize --vm-machine-state` oder `neverd_devirtualize_machine_source_v1()` wählt eine eigene Quell-ABI: einen Zeiger auf 17 ausgerichtete `uint64_t`-Wörter (RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8 bis R15, RFLAGS). Nur der vorzeichenlose 64-Bit-Status null bedeutet Erfolg. Ein anderer Status macht Speicherzugriffe nicht rückgängig. Der Zustand gilt unmittelbar vor dem Adress-Pop des letzten RET. Sein Speicher darf Gastspeicher nicht überlappen; erforderlich sind ursprüngliche Abbildungen und ein 64-Bit-Little-Endian-Host.

Das Profil setzt CPL3/IOPL0, deaktivierten Schattenstack, keine asynchronen Ereignisse und normale fehlerfreie Ausführung voraus. Eingangsflags müssen kanonisch mit TF/RF/VM/AC/VIF/VIP null sein; POPFQ muss TF/AC null lassen. Der erzeugte Code prüft dies. PUSHFQ/POPFQ verwenden den expliziten Zustand; RDSSP erhält das Zielregister, erreichtes INCSSP wird abgelehnt. Direkte Near-Aufrufe behalten den echten Rückadressenspeicherzugriff; interne RET erfordern ein bewiesenes eindeutiges Ziel. Vollständig gespeicherte Rahmenzeiger bleiben erhalten; Teilzugriffe oder mögliche Aliase verwerfen Fakten. Ausnahmemetadaten erlauben nur den normalen Pfad, keine Ausnahme- oder Unwind-Äquivalenz. Der Bericht enthält `sourceABI` und `executionProfile`; die Standard-ABI bleibt streng.

Die normale Quell-ABI rekonstruiert einen aufrufprivaten Rahmen. Alle extern abgeleiteten LOAD/STORE-Bereiche einschließlich berechneter Adressen müssen vom nativen privaten Rahmen und seinem rekonstruierten Quellspeicher getrennt sein; dies ist eine ausdrückliche Vorbedingung. Der gemeinsame Beweis lehnt entweichende Rahmenadressen, davon abhängige Ergebnisse oder Verzweigungen und uninitialisierte private Lesezugriffe ab. Die Maschinenzustands-ABI behält ursprüngliche Gastadressen und verwendet diese Rahmenvorbedingung nicht.

Wenn undefinierte Flags den Kontrollfluss, Adressen oder definierte Ausgaben beeinflussen, ist ein separater Nichtinterferenznachweis erforderlich. Der aktuelle Bericht liefert diesen Nachweis nicht und bestätigt kein solches prozessorabhängiges Verhalten.
