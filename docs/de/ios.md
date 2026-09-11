**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](../ja/ios.md) | [한국어](../ko/ios.md) | [Français](../fr/ios.md) | [Deutsch](ios.md) | [Español](../es/ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# iOS: nativen Code und Quelltext rekonstruieren

[← Dokumentationsindex](README.md) · [Mobile Übersicht](../mobile.md)

`neverd mobile` verarbeitet IPA, `.app` und Mach-O. Es exportiert natives C, Laufzeitmetadaten sowie experimentellen Objective-C-Quelltext (`.m`) und Swift-Quelltext (`.swift`) für unterstützte native Funktionskörper. Veröffentlichte Ergebnisse können nicht rekonstruierte Methoden enthalten; prüfen Sie vor der Verwendung den Abdeckungsbericht. Mobile Container gehören zum CLI-Ablauf; das native C-SDK lädt die ausgewählte Mach-O-Datei separat.

Beim Kompilieren gehen Kommentare, Formatierung, Bezeichner und Sprachkonstrukte verloren. Dieser Ablauf rekonstruiert eine Quelltextdarstellung, nicht den ursprünglichen Text, und beweist keine Verhaltensäquivalenz beliebiger Anwendungen. Die analysierte Anwendung wird dabei nicht gestartet.

## Einstieg und Abhängigkeiten

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

Bauen Sie das Ziel `neverd` mit einer C++20-fähigen Toolchain. Der mobile Ablauf läuft in der nativen CLI ohne Python-Interpreter. Swift-Signaturen werden mit `LLVMSwiftDemangle` aus dem NeverD-LLVM-Fork demangelt. Sowohl Quellcode-Builds als auch dazu passende veröffentlichte LLVM-Pakete enthalten diese Komponente; NeverD lädt Swift-Quellen nicht als zusätzliche Abhängigkeit herunter. Zum Bauen und Ausführen von NeverD muss kein Swift-Compiler und keine Swift-Toolchain installiert sein. Native Abhängigkeiten wie LLVM und Capstone bleiben bestehen; verteilen Sie die benötigten Bibliotheken und Lizenzhinweise. Zum unabhängigen Kompilieren erzeugter Apple-Quelltexte und für Swift-Verhaltenstests unter macOS werden je nach Aufgabe Apple Clang, das SDK und `swiftc` benötigt.

Die Swift-Signaturrekonstruktion verwendet die strukturierten Knoten von `LLVMSwiftDemangle` direkt im C++-Prozess. Sie sucht oder startet weder ein externes Demangling-Programm noch Befehle zur Toolchainsuche. Die frühere Option für den Programmpfad wurde entfernt, und die frühere Demangler-Umgebungsvariable wird nicht mehr gelesen. `--metadata-only` führt weder den nativen Quellexporter noch Signatur-Demangling aus.

Das Signaturinventar in `metadata/swift-signatures.json` kennzeichnet die eingebaute Komponente wie folgt:

```json
{
  "demangler": {
    "name": "llvm-swift-demangle",
    "execution": "builtin",
    "version": "6.3.3"
  }
}
```

```sh
neverd mobile App.ipa -o recovered-swift --timeout=600 --json
```

## Eingaben und Auswahl

Eine IPA muss genau ein oberstes `Payload/*.app` enthalten. Für IPA und `.app` bestimmt `CFBundleExecutable` in `Info.plist` das Hauptprogramm. `--artifact` ist in beiden Formaten relativ zu diesem Anwendungsverzeichnis und wählt genau eine eingebettete ausführbare Datei; Frameworks und Erweiterungen werden nicht alle rekursiv analysiert. Für reine Mach-O-Eingaben ist `--artifact` unzulässig.

Bei Fat-Binärdateien bevorzugt `--arch=auto` arm64, arm, x86_64 und dann i386. Fehlende oder nicht unterstützte Slices führen zu einem ausdrücklichen Fehler. Die Quellsprachenprojektion zielt derzeit auf arm64/x86_64; eine andere Architekturauswahl bedeutet keine Objective-C-/Swift-Unterstützung. Ein ausgewählter Slice mit `cryptid != 0` wird abgelehnt; die Eingabe muss bereits entschlüsselt und lesbar sein. Archive und Verzeichnisse dürfen keine unsicheren Pfade, symbolischen Links, Spezialdateien oder kollidierenden Einträge enthalten.

## Optionen und Ressourcenlimits

| Option | Standard | Bedeutung |
|--------|----------|-----------|
| `-o DIRECTORY` | Erforderlich | Neues Ausgabeverzeichnis außerhalb einer Verzeichniseingabe; vorhandene Ausgabe bleibt erhalten |
| `--platform=auto\|ios` | `auto` | Plattform erkennen oder iOS wählen |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | Einen Mach-O-Slice auswählen |
| `--artifact PATH` | Hauptprogramm | Ausführbarer Pfad relativ zur Anwendung |
| `--metadata-only` | Aus | Nur Metadaten, ohne Quelltextrekonstruktion oder Werkzeugaufruf |
| `--max-func N` | `0` | Limit nativer Funktionen; null bedeutet alle gefundenen; im Metadatenmodus ignoriert |
| `--timeout N` | `300` | Positives Gesamtzeitbudget der Analyse in Sekunden; Kindprozesse verwenden das verbleibende Budget |
| `--max-files N` | `20000` | Positives Eintragsbudget; auch das Swift-Symbolinventar ist begrenzt |
| `--max-bytes N` | `2147483648` | Positives Bytebudget für Eingabe, entpackte Daten und endgültige Ausgabe |
| `--json` | Aus | Versionierten Bericht als JSON ausgeben |

Der Arbeitsbereich wird überwacht und darf für bereitgestellte Eingaben und Zwischenprodukte bis zum Dreifachen der Eintrags-/Bytebudgets wachsen. Pro Prozess sind Logs auf 16 MiB begrenzt, Swift-Signatur-JSON zusätzlich auf 32 MiB. Dies sind Ressourcenkontrollen, keine Prozessisolierung. Ein größeres Timeout hebt andere Grenzen nicht auf. Durch `--max-func` ausgeschlossene Methoden bleiben als nicht rekonstruiert sichtbar, sofern das Metadateninventar sie enthält.

## Objective-C-Quelltext und Laufzeitstruktur

Der native Loader verbindet Methodenrecord, ausführbare IMP-Adresse und unterstützte Typkodierung mit expliziten Quell-ABI-Positionen. Feste Skalar-/Zeigerbindungen erhalten versteckte `self`/`_cmd`, ungenutzte Argumente, getrennte Ganzzahl-/Gleitkommaregister und unterstützte Stackpositionen. Die Bit-Neuinterpretation von float/double ist von numerischer Konvertierung getrennt. Typhinweise dienen ausschließlich der Quelltextprojektion; sie sind weder authentifizierte ABI-Nachweise noch eine Erlaubnis zum Patchen ausführbaren Codes.

`sources/objc.m` enthält tatsächlich rekonstruierte Anweisungen in `@implementation`-Methoden sowie erforderliche C-Hilfsfunktionen und typgebundene Aufrufe. Aufrufziele benötigen eine unterstützte Quellbindung; unbekannte Ziele und unvollständige Abhängigkeitsgruppen bleiben nicht rekonstruiert. Fehlende Definitionen, ungültige Codeadressen, widersprüchliche Kodierungen, nicht unterstützte ABI-Abbildungen, unvollständiges Decoding und abgelehnte IR werden nicht allein aufgrund einer Deklaration als rekonstruiert gewertet.

Klassenmetadaten erhalten die Oberklasse, Instanzbeginn/-größe und skalare bzw. Zeiger-Ivars mit geprüften Offsets, Breiten und Ausrichtungen. Deklarationen ergänzen nötiges Padding. Methoden mit erforderlichem, aber unbekanntem Instanzlayout bleiben nicht rekonstruiert. Kategorien behalten eigene Klassen-/Kategorie-/Adressidentitäten und Implementierungen; identische Records in Klassen- und Kategorieinventaren zählen einmal. Unterstützte externe Kategorien verwenden vorhandene Foundation-Klassendeklarationen. Unbekannte externe Header werden als fehlende Abhängigkeit gemeldet; es wird kein Ersatzlayout erfunden.

Die Wiederherstellung setzt außerdem vollständige Klassendeklarationen, Definitionen lokaler Vorfahrenklassen und erzeugten Quelltext voraus, in dem Werte auf jedem Pfad vor ihrer Verwendung definiert sind und erreichbare Ausgänge die erforderliche Rückgabe enthalten; eine leere lokale Oberklasse erhält nur dann eine leere `@implementation`, wenn geprüftes Layout und vollständiges Methodeninventar belegen, dass sie keine eigenen gewöhnlichen Methoden besitzt. Fehlen diese Nachweise oder kollidiert ein Name mit einem bekannten Foundation-Import, bleiben betroffene Methoden mit Begründung im Nenner der Abdeckung, während unabhängig rekonstruierbare Klassen weiter ausgegeben werden; diese Namensprüfungen erfassen nicht sämtliche SDK-Namen, iOS-SDKs oder Versionen.

Unterstützte Objective-C-Block-Aufrufe benötigen eine vollständige feste skalare Aufruf-ABI einschließlich des verborgenen Block-Objekts sowie aller Argument- und Rückgabepositionen. Die Laufzeitkodierung `@?` wird nur in Deklarationen zu `id` erweitert; sie liefert keinen Aufrufprototyp. Referenzen auf globale Blocks bewahren die gemeinsame Objektidentität. Unterstützte synchrone skalare Captures erfordern einen Nachweis des nativen Capture-Speichers und Aufrufflusses. Entweichende oder asynchrone Captures, nicht modellierte Objekt-/byref-Eigentumsregeln, copy/dispose-Hilfsfunktionen und unbekannte Layouts bleiben nicht wiederhergestellt.

Die Rekonstruktion von Laufzeitinformationen ist begrenzt: vollständige Properties, Protokolle, ursprüngliche Ownership-Annotationen, beliebige Aggregate, variadische Endargumente, ausnahmeabhängige Körper und nicht modellierte Block-/Capture-Layouts werden nicht versprochen. Laufzeitkodierungen beschreiben feste Argumente und können fehlende Auslassungspunkte im Original nicht beweisen. Chained Pointer werden nur für vom nativen Loader aufgelöste Slots verwendet; andere Formate behalten Diagnosen.

## Swift-Quelltext und Speicherlayout

Strukturierte Demangler-Ausgaben trennen aufrufbare Signaturen von nicht aufrufbaren Metadaten. Unterstützte Signaturen werden vor der Körperprojektion an Symbole, Einsprungadressen und explizite Maschinen-ABI der gewählten Binärdatei gebunden. Swift-Empfänger folgen der Swift-ABI; versteckte Objective-C-Argumente ersetzen sie nicht. Auch eine vom Nutzer gelieferte Signaturdatei bleibt ein zu validierender Hinweis.

Der experimentelle Emitter erzeugt unterstützte freie Funktionen, Klassenmethoden, designierte Initialisierer und Methoden von Strukturen mit festem Layout, einschließlich unterstützter mutierender Empfängerformen. Klassen-/Strukturdeklarationen und gespeicherte Felder benötigen rekonstruierte Layoutmetadaten. Native Aufrufe erscheinen nur, wenn benötigte Deklarationen und Körper eine vollständige unterstützte Abhängigkeitsgruppe bilden. Quelltexteinheiten enthalten Deklarationen und Methoden gemeinsam, statt über eine Brücke die Originalbinärdatei aufzurufen.

Die Rümpfe unterstützter Swift-Getter und -Setter stammen aus der nativen Implementierung und werden zu Properties zusammengesetzt. Privater Hintergrundspeicher bewahrt das nachgewiesene Feldlayout; Initialisierer und andere Methoden verwenden dieselben Speichernamen. Eine Property-Deklaration oder ein Feldeintrag allein belegt keinen wiederhergestellten Accessor-Rumpf.

Unterstützte allokierende Konstruktoren, triviale Destruktoren/Freigaben, Typmetadaten-Accessors und `_modify`/resume-Einstiegspunkte können auf eine ausgegebene Typeinheit abgebildet werden. Jeder Eintrag benötigt einen begrenzten Nachweis des gesamten nativen Ablaufs und seiner Effekte, tatsächlich wiederhergestellte Kontext-/Initialisierer-/Property-Abhängigkeiten sowie Prüfungen der Ausnahmebehandlung und IR aller beteiligten Rümpfe. Die Schreiboperationen des Allokators müssen zum tatsächlichen Initialisierer passen; `_modify` muss das genaue veränderbare Feld und die Fortsetzung binden. Laufzeitaufrufe für Metadaten behalten ihre modellierte Semantik innerhalb des wiederhergestellten Typs. Diese Einträge werden ausdrücklich als Compiler-Quellcodeprojektionen ausgewiesen und stellen keine separat wiederhergestellten gewöhnlichen Methodenrümpfe oder den ursprünglichen Quelltext dar.

Generische oder resiliente Layouts, async-/throwing-Funktionen, unbekannte Aufrufkonventionen, nicht unterstützte Accessors/Allocators/Thunks, unvollständige Initialisierung und ungebundene native oder Laufzeitabhängigkeiten bleiben einzeln `unrecovered`. Ein Mangling-Symbol oder nominaler Typname allein ist keine rekonstruierte Methode. Entfernte Symbole und unklassifizierte Demangler-Knoten machen die Abdeckung unvollständig oder unbekannt.

## Ausgabe und Abdeckung

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

Quellsprachendateien entstehen nur bei möglicher Ausgabe. `objc.json` enthält Klassen, Kategorien, Ivars und rohe Methodenkodierungen, `objc.h` die unterstützten Deklarationen. `swift.json` enthält nominale Typmetadaten und Mangling-Symbole. Signatur-/Methoden-JSON bewahren Klassifizierung, Auslassungen, Gründe und Zähler. Logs enthalten native Diagnosen sowie Diagnosen des nativen Swift-Exports, sofern dieser ausgeführt wird. Logs einer externen Swift-Toolchainsuche oder eines externen Demanglers entstehen nicht. Ausgabepfade in `report.json` sind relativ zu dessen Verzeichnis. Der ausgewählte Binärcode ist ein Analyseartefakt und wird nicht als Wiederherstellungsbrücke in generierten Quelltext gelinkt.

Temporäre Paketkopien und Backend-Zwischenberichte werden gelöscht. Ohne native Funktionskörper schlägt ein normaler Lauf auch bei vorhandenen Metadaten fehl. Der reine Metadatenmodus erzeugt nur den ausgewählten Slice, `objc.h`, `objc.json`, `swift.json` und `report.json`; Quelltext- und Methoden-/Signaturdateien fehlen, und `native_function_count`, `objc_method_recovery`, `swift_method_recovery` sind `null`. Alle Modi verwenden die vom nativen Loader aufgelösten Objective-C-Metadaten. Swift-Metadaten werden durch begrenzte Zugriffe auf das native Abbild gelesen; nicht unterstützte Fixups, relokierbare Layouts oder Referenzen behalten Diagnosen für Teilergebnisse.

Dieser verkürzte Beispielbericht zeigt bewusst eine teilweise Rekonstruktion:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

Äußeres `status: "success"` bedeutet, dass geprüfte Ausgabe veröffentlicht wurde. `recovered`, `partial`, `unrecovered` und `no-methods` beschreiben das gefundene Inventar, weder Semantikgleichheit noch Vollständigkeit des Originalprogramms. Jede nicht rekonstruierte Methode hat einen Grund. Objective-C-Status `recovered` verlangt zusätzlich vollständige Laufzeitmetadaten. Ein leeres Inventar beweist nicht, dass keine Methoden existierten.

Swift-`coverage_status` zählt nur klassifizierte aufrufbare Einträge. Der gesamte Swift-`status` berücksichtigt unbekannte Symbole und kann auch `unclassified`, `unsupported-architecture` oder `no-symbols` sein. Nicht aufrufbare Metadaten stehen in `non_method_symbols` als `not-callable`, unbekannte Symbole als `unclassified`. `types`, `type_metadata_count` und `source_type_count` zählen Typmetadaten bzw. ausgegebene Typeinheiten getrennt und dürfen Methodenzahlen nicht erhöhen.

Jede wiederhergestellte Swift-Zeile enthält `source_representation` mit `native-method-body` oder `compiler-generated-from-type`. Compiler-Projektionen behalten außerdem `compiler_projection_kind` und `compiler_projection_evidence`. `source_body_method_count` zählt wiederhergestellte native Methodenrümpfe, `compiler_projection_method_count` nachgewiesene Compiler-Projektionen. Ihre Summe entspricht `recovered_method_count`. Compiler-Einstiegspunkte bleiben im Nenner `method_count` und behalten ihre genaue Identität in genau einer zugehörigen `type`-Quelleinheit. Typmetadaten oder der Name einer Abhängigkeit allein erhöhen die Wiederherstellungsabdeckung nicht. Das native Batch-JSON enthält `source` in Compiler-Zeilen und Typeinheiten; die mobilen `source_units` behalten nur Beschreibungen ohne `source`, und der vollständige Quellcode steht in `sources/swift.swift`.

Im nativen Swift-Bericht enthält `source_units` die Felder `{kind, module, name, source, method_entries, method_identities}`; kind ist `function` oder `type`, jede Identität `{entry, mangled_symbol}`. Verschiedene Symbole dürfen denselben Einsprung mit unterschiedlichen ABI-Projektionen teilen. Jede rekonstruierte Identität muss genau einmal vorkommen, keine nicht rekonstruierte darf enthalten sein. `method_entries` entspricht exakt der geordneten Einsprungprojektion von `method_identities`, auch mit wiederholten Adressen. Identische doppelte Identitäten dürfen nicht still zusammengeführt werden. Batch-`source` verkettet die Einheitsquelltexte mit je einem Zeilenumbruch. Mobile speichert die Gesamtausgabe in `sources/swift.swift` und Einheitsbeschreibungen im Abdeckungs-JSON. Einzelne Methoden-`source`-Felder dienen der Inspektion; ihre Verkettung erzeugt keine korrekten Klassendeklarationen.

## Direkter nativer Export und SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

Der Swift-Export verarbeitet das strukturierte Signaturinventar, das der eingebaute Signaturparser bei einem normalen Mobile-Lauf erzeugt. Objective-C-Batch-JSON enthält `native_source`, `native_function_count`, `objc_metadata` sowie C-Quelltext, Funktionsname, Rückgabetyp und Parameter pro Methode. Vor `.m` prüft Mobile Deklarationen, Körper und Layouts zusätzlich; seine endgültige Abdeckung kann deshalb enger sein als die C-Batch-Abdeckung. Auch ein erfolgreicher nativer Export kann keine rekonstruierte Methode enthalten.

Für eine Sitzung mit geladener Mach-O-Datei liefern `neverd_objc_methods_json(session, max_functions)` und `neverd_swift_methods_json(session, signatures_json, max_functions)` die Berichte. Null wählt alle gefundenen Funktionen. Erfolgreiche Strings mit `neverd_free_string` freigeben; `NULL` bedeutet Fehler, erklärt durch den Sitzungsfehler. Diese APIs laden keine IPA- oder `.app`-Container.

## Verifikation und Fehlerdiagnose

Unter macOS bieten Builds mit aktiviertem `BUILD_TESTING` das Ziel `check-neverd-mobile-ios`, das alle drei nativen Wiederherstellungstestsuiten über CTest ausführt.

Python dient nur den folgenden Entwicklungstests; die integrierte mobile Wiederherstellung läuft in der nativen C++20-CLI.

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

Unter macOS kompilieren die Objective-C-Prüfskripte die Originale, stellen `.m` wieder her und linken ausschließlich den erzeugten Quellcode mit einem unabhängigen Aufrufprogramm. Das Skalarskript prüft Ganzzahlgrenzen, Verzweigungen, Schleifen, Zeigerzugriffe, verborgene Parameter, float/double-Bitidentität, gemischte Parameter und Stack-Argumente. Das Aufrufskript ergänzt Nachrichtenverteilung, Vererbung, Category, Instanzvariablenspeicher, native Hilfsfunktionen sowie Block-Aufrufe, Captures und gemeinsame Identität. Der Aufrufkorpus erfordert 21/21 wiederhergestellte Methoden und 134/134 unabhängige Sollwerte je Variante arm64/x86_64 × classic/default. Führen Sie die Prüfung mit dem aktuellen nativen CLI-Build aus.

Das strenge Swift-Prüfskript kontrolliert 22 Benutzerdeklarationen, drei Getter-/Setter-Einstiegspunkte und sieben vom Compiler erzeugte aufrufbare Einträge; keiner darf aus dem Inventar verschwinden. Pro Variante werden 855 unabhängige Sollwerte des Originalprogramms geprüft. Erzeugtes `.swift` und Aufrufprogramm werden unabhängig kompiliert, ohne ursprüngliche dylib, Modul, Bridge oder handgeschriebene Ersatzdeklarationen. Die Fälle umfassen skalare/native Aufrufe, Klasseninitialisierung und -speicher, Strukturmethoden mit Wertübergabe/mutating, Gleitkomma- und Stack-Argumente, Zeiger und Schleifen. Die native C++20-CLI muss alle vier Varianten arm64/x86_64 × classic/default ohne übersprungene Fälle bestehen: jeweils 25 native Methodenrümpfe und sieben Compiler-Projektionen wiederherstellen, alle 32 aufrufbaren Identitäten bewahren und für Originale sowie unabhängig kompilierten erzeugten Swift-Code 855/855 Sollwertprüfungen erfüllen. Diese Ergebnisse gelten nur für diesen Korpus und garantieren keine Wiederherstellung beliebiger Anwendungen oder des ursprünglichen Quelltexts. Das Skript weist fehlende Abdeckung, Quellcode-Kompilierfehler und Verhaltensabweichungen zurück.

Alle drei Skripte unterstützen `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N` und `--work-dir NEW_DIRECTORY`. `--setup-only` prüft die Originale und nicht die Wiederherstellung. Für die Abnahme müssen alle angeforderten Architektur- und Fixup-Varianten vollständig durchlaufen werden, auch im Skalartest. Eine fehlende Variante oder eine auf dem Host nicht ausführbare angeforderte Architektur führt zum Fehlschlag; Überspringen ist nicht erlaubt. Gesicherte Fehlerartefakte helfen, fehlende Quellcodeabdeckung, Kompilierfehler und Verhaltensabweichungen zu unterscheiden. Prüfen Sie die aktuellen Ergebnisse, bevor Sie Unterstützung als verifiziert bezeichnen.

Der [Workflow Mobile Real Applications](../../.github/workflows/mobile-real-apps.yml) verwendet öffentliche Anwendungen mit festgelegten Versionen aus dem [Korpusmanifest](../../scripts/mobile_real_apps.json). Die Abnahme verlangt unabhängige Inventare aller Mach-O-Dateien vollständiger iOS-Bundles und aller DEX-Dateien der APKs, getrennte Neubauten der Originale und des erzeugten Quellcodes sowie Verhaltensvergleiche. Fehlende Stufen, unbekannte Inventarabdeckung oder fehlende Pflichtfälle lassen die Abnahme scheitern. Die Stufen recompile und behavior für reale Anwendungen sind noch unvollständig; daher bleibt die Kennzeichnung Experimental bestehen. Bestandene Schutztests des Testgerüsts sind kein Nachweis einer erfolgreichen Prüfung realer Anwendungen.

Die Veröffentlichung ist transaktional: neues Verzeichnis wählen, zuerst den Exitstatus prüfen und umgeleitetes JSON außerhalb speichern. Fehler entfernen temporäre Ausgabe und erhalten vorhandene Ergebnisse. Backend-Exits mit einem Wert ungleich null enthalten ein begrenztes Log-Ende. Bei Backend-Zeitüberschreitungen bleibt die ursprüngliche Timeoutmeldung erhalten; ein begrenztes Log-Ende wird angefügt, wenn bereits erfasster Logtext verfügbar ist. Budgetverletzungen behalten ihre eigenen Meldungen. Die native CLI liefert bei Erfolg null und bei Wiederherstellungsfehlern einen Wert ungleich null. Mit `--json` enthalten behandelte Fehler `schema_version`, `status: "error"` und `error`. Argumentprüfung, Startfehler nativer Programme oder Bibliotheken und Unterbrechungen können ausschließlich auf stderr erscheinen. Prüfen Sie zuerst den Exitstatus.

Bei verschlüsselten Slices lesbare Eingaben liefern und bei fehlender Architektur verfügbare Slices prüfen. Für ausgelassene Methoden genaue Gründe und Metadatendiagnosen lesen. Ein größeres `--max-func` hilft nur bei durch das Limit ausgeschlossenen Funktionen. Fehlende Layouts, Signaturen, externe Header, Ausnahme- oder ABI-Unterstützung benötigen Implementierung oder zusätzliche gültige Metadaten, keine Behauptung vollständiger Wiederherstellung. Anwendbare Lizenzhinweise von Abhängigkeiten bei der Weitergabe erhalten.
