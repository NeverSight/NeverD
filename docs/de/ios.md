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

Beide Quellberichtmodi enthalten `source_projection_graph`. Seine Knoten zeigen die endgültig typisierten nativen Funktionskörper, lokale Diagnosen, zusammengeführte native/Block-`dependencies` und das produktive Ergebnis `closure_closed`. Lokale Prüfungen und fortgepflanzte Abhängigkeitsfehler haben getrennte Gründe; fehlende typisierte Körper besitzen unvollständige Diagnosen. Das Binden eines bislang ungebundenen Aufrufs kann weitere Abhängigkeiten sichtbar machen. Ein geschlossener Knoten hat nur die Abhängigkeitsphase bestanden: Erst Emission und Quelltextprüfung machen eine Methode zu `recovered`. `native_dependency_graph` bleibt davon getrennt und inventarisiert LowIR-Aufrufe.

Für wiederholte Deckungsanalysen führt der folgende Befehl dieselben Analyse- und Veröffentlichungsprüfungen wie `--format=objc-methods` aus, lässt aber `native_source` und das `source`-Feld jeder Methode weg. Das JSON ergänzt `sources_omitted=true`; Methodenidentitäten, Zustände, Diagnosen, Signaturen, gemeinsame Hilfsreferenzen und Abhängigkeitsbelege behalten ihre vollständige Bedeutung. Rendering-Prüfungen laufen weiterhin. Der vollständige Modus liefert kompilierbare Quellen. Der entsprechende C-Einstieg ist `neverd_objc_methods_summary_json(session, max_functions)`; das Ergebnis wird mit `neverd_free_string` freigegeben.

```sh
neverd export WMF --format=objc-methods-summary -o summary.json
```

Der native Loader verbindet Methodenrecord, ausführbare IMP-Adresse und unterstützte Typkodierung mit expliziten Quell-ABI-Positionen. Feste Skalar-/Zeigerbindungen erhalten versteckte `self`/`_cmd`, ungenutzte Argumente, getrennte Ganzzahl-/Gleitkommaregister und unterstützte Stackpositionen. Die Bit-Neuinterpretation von float/double ist von numerischer Konvertierung getrennt. Typhinweise dienen ausschließlich der Quelltextprojektion; sie sind weder authentifizierte ABI-Nachweise noch eine Erlaubnis zum Patchen ausführbaren Codes.

`sources/objc.m` enthält tatsächlich rekonstruierte Anweisungen in `@implementation`-Methoden sowie erforderliche C-Hilfsfunktionen und typgebundene Aufrufe. Aufrufziele benötigen eine unterstützte Quellbindung; unbekannte Ziele und unvollständige Abhängigkeitsgruppen bleiben nicht rekonstruiert. Fehlende Definitionen, ungültige Codeadressen, widersprüchliche Kodierungen, nicht unterstützte ABI-Abbildungen, unvollständiges Decoding und abgelehnte IR werden nicht allein aufgrund einer Deklaration als rekonstruiert gewertet.

Parameter nativer Hilfsfunktionen können für die Quellcodeprojektion Zeigertypen erhalten, wenn vollständige Eingangswerte über COPY/PHI bereits gebundene Zeigerargumente erreichen und keine widersprüchliche skalare Verwendung vorliegt. Physische ABI-Positionen und generische IR-Typen bleiben erhalten; Funktionskörper und ihre vollständigen nativen Abhängigkeitsgruppen müssen weiterhin validiert werden.

Ein nativer skalarer Helfer darf einen beobachteten Eingangsparameter zurückgeben, wenn dessen genaues ABI-Register die gesamte Ergebnisbreite abdeckt. Eine begrenzte Analyse verbindet alle Rückgabepfade mit dem ersten Eintritt und den Schleifenrückkanten; Aufrufe und partielle Schreibzugriffe verwerfen den Wert. Nicht deklarierte Eingangswerte, unbenutzte Parameterplatzhalter, SSA-Anfangsmarkierungen und PHIs beweisen keine Eingaben. Diese ausschließlich für die Quellprojektion verwendete Inferenz erhält die physischen Positionen und verlangt weiterhin vollständige Prüfung von Funktionskörper und Abhängigkeiten.

Lokale native Hilfsfunktionen können vollständige Ganzzahleingaben in zusätzlichen Registern einschließlich Ergebniszeigern als explizite Parameter übernehmen, wenn Eingangsbedarfsanalyse und vollständige LowIR-Lesezugriffe übereinstimmen. Vom Aufrufer gesicherte Register dürfen danach als temporäre Register dienen; erhaltene Kontextregister erfordern weiterhin, dass keine erhaltenen Register außerhalb des Frames beschrieben werden. Implizite Aufrufdefinitionen sind auch mit SSA-Version 0 keine Eingangswerte; nur ausdrücklich erhaltene Bytes lassen sich über Aufrufe verfolgen. Neu erzeugte Aufrufer und Definitionen verwenden dieselben physischen Eingangspositionen. Externe C-/Swift-Prototypen werden nicht abgeleitet; vollständige Funktionskörper und Abhängigkeitsabschlüsse bleiben erforderlich.

Feste C-Callback-Typen behalten ihre Parameter- und Rückgabesignaturen in Quelldeklarationen und Typumwandlungen. Der exakte Import `swift_once` bindet eine Initialisierungsmarke, einen Callback `void (*)(void *)` und einen Kontext ohne Rückgabewert. Der Runtime-Aufruf bleibt erhalten; dies beweist weder die Wiederherstellung des Callback-Rumpfs noch die Eigentümerschaft des gemeinsamen Initialisierungsspeichers. Unvollständige Abhängigkeiten bleiben nicht wiederhergestellt.

Bekannte Importe der Objective-C-Laufzeit erhalten explizite Argument- und Rückgabebindungen: retain/release, automatische Freigabe, starke und schwache Referenzen, Objektallokation und Setter mit fester Signatur. Registerspezifische ARM64-Varianten lesen das angegebene Register; Importidentität und ABI müssen übereinstimmen. Aufrufe zum Lesen, Setzen und Entfernen assoziierter Objekte bewahren Objekt, Schlüssel, Wert und die Richtlinie in Zeigerbreite. Der erzeugte C-Code nutzt den öffentlichen Laufzeit-Header. Rekompilierungstests unter macOS vergleichen Lebensdauer, Nullsetzen schwacher Referenzen, Kopieren und Entfernen mit den Originalmethoden.

Die optimierten Klassen- und Selektorabfragen von libobjc behalten ihre tatsächlichen Laufzeitaufrufe einschließlich nil-Behandlung und eigener Überschreibungen. Das Byte-Ergebnis erhält die nativen Umwandlungen des Aufrufers; es wird nicht durch einen angenommenen Test der Klassenhierarchie ersetzt.

Der Objective-C-Export bindet außerdem eine feste Auswahl von Swift-Runtime-Importen mit gewöhnlicher C-ABI: Referenzzählung, native und typunbekannte schwache Objektreferenzen, Objektmetadaten sowie Beginn und Ende der Zugriffsprüfung. Das erzeugte C erhält diese Aufrufe und benötigt beim Linken die Swift-Runtime. Registerabhängige Einstiegspunkte, unbekannte Swift-Aufrufkonventionen und beliebige Swift-Symbole bleiben ausgeschlossen; Importidentitäten und skalare Übergabeorte müssen exakt übereinstimmen.

Separate Bindungen unterstützen auf arm64 und x86_64 den exakten Darwin-Import String → NSString `_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF` sowie optionales NSString → String `_$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ`. Beide erhalten die Besitzsemantik und Clang `swiftcall`; zum Linken sind Swift Foundation und Swift Core erforderlich. Die Rückrichtung transportiert beide zurückgegebenen String-Wörter als vorzeichenlose 128-Bit-Ganzzahl und zerlegt sie vor SSA in explizite Rückgaberegister. Dies beschreibt nur den Bittransport, kein rekonstruiertes String-Layout oder allgemeines Aggregat-ABI. Unbekannte Signaturen und unvollständige Initialisierungsabhängigkeiten bleiben nicht unterstützt.

Bei bestätigten Schlüsselargumenten der API für assoziierte Objekte lassen sich genaue Adressen im schreibgeschützten Mach-O-C-String-Bereich als gemeinsame Schlüsselidentitäten rekonstruieren. Gleiche Originaladressen teilen einen Schlüssel; unterschiedliche innere Offsets bleiben getrennt. Der mobile Export führt die Hilfsfunktionen automatisch zusammen. Die C-API nennt sie in `shared_identity_functions`; beim Linken der Methoden darf jede Hilfsfunktion nur einmal definiert sein. Die Schlüssel gehören zum neu erzeugten Code und verweisen nicht auf ein bereits geladenes Originalabbild. Andere ungebundene Abbildadressen bleiben eine Einschränkung.

Verifizierte Importe für `os_unfair_lock_lock`, `os_unfair_lock_unlock`, `os_unfair_lock_trylock`, `os_unfair_lock_assert_owner`, `os_unfair_lock_assert_not_owner` rufen über `<os/lock.h>` die echte Darwin-Implementierung auf und erhalten die Sperradresse, Eigentümerprüfungen und das boolesche Ergebnis. Unbekannte Varianten bleiben ununterstützt. Ganzzahlige Aufrufergebnisse erhalten nur die deklarierten ABI-Bits: Darwin arm64 erweitert 8/16-Bit-Ergebnisse entsprechend ihrer Vorzeichenbehandlung auf 32 Bit; die übrigen Registerbits bleiben unbekannt. Deklarierte Rückgabebreiten gelangen vor dem Zusammenführen von Zweigen in SSA, damit ungenutzte obere Bits gültige untere Bytes nicht verdecken.

Verifizierte Darwin-`__cfstring`-Datensätze lassen sich bei vollständigem Klassenimport, Layout, Zeichenspeicher und Fixups als konstante Objekte rekonstruieren. ASCII-Bytes und UTF-16-Codeeinheiten einschließlich eingebetteter NUL bleiben erhalten. Gleiche Ursprungsadressen teilen ein rekonstruiertes Objekt; verschiedene Datensätze bleiben getrennt. Die Hilfsfunktionen stehen in `shared_identity_functions` und benötigen Foundation beim Linken. Ungeprüfte direkte Speicherzugriffe auf konstante Objekte werden dadurch nicht erlaubt.

Numerische Profilzähler in einem vollständig begrenzten, zeigerfreien Abschnitt `__DATA,__llvm_prf_cnts` können gemeinsamen rekonstruierten Speicher verwenden. Anfangsbytes, überlappende Lese- und Schreibzugriffe von 1–16 Byte sowie Änderungen bleiben über Methodendateien hinweg erhalten. Der mobile Export führt die Hilfsfunktionen zusammen; Nutzer der C-API müssen jede Definition aus `shared_storage_functions` genau einmal linken. Dieser Speicher ist unabhängig vom Originalabbild und dessen Profiling-Laufzeit. Entweichende Adressen, geordnete Zugriffe, unvollständige Abbildungen und Zeigerrelokationen bleiben ununterstützt. Instrumentierte Block-Callbacks dürfen diesen Bildspeicher aktualisieren, ohne private Adressen offenzulegen; gewöhnliche Zählerschreibzugriffe gelten nicht als Schreibzugriffe auf den privaten Stackframe.

Klassenmetadaten erhalten die Oberklasse, Instanzbeginn/-größe und skalare bzw. Zeiger-Ivars mit geprüften Offsets, Breiten und Ausrichtungen. Deklarationen ergänzen nötiges Padding. Methoden mit erforderlichem, aber unbekanntem Instanzlayout bleiben nicht rekonstruiert. Kategorien behalten eigene Klassen-/Kategorie-/Adressidentitäten und Implementierungen; identische Records in Klassen- und Kategorieinventaren zählen einmal. Unterstützte externe Kategorien verwenden vorhandene Foundation-Klassendeklarationen. Unbekannte externe Header werden als fehlende Abhängigkeit gemeldet; es wird kein Ersatzlayout erfunden.

Die Wiederherstellung setzt außerdem vollständige Klassendeklarationen, Definitionen lokaler Vorfahrenklassen und erzeugten Quelltext voraus, in dem Werte auf jedem Pfad vor ihrer Verwendung definiert sind und erreichbare Ausgänge die erforderliche Rückgabe enthalten; eine leere lokale Oberklasse erhält nur dann eine leere `@implementation`, wenn geprüftes Layout und vollständiges Methodeninventar belegen, dass sie keine eigenen gewöhnlichen Methoden besitzt. Fehlen diese Nachweise oder kollidiert ein Name mit einem bekannten Foundation-Import, bleiben betroffene Methoden mit Begründung im Nenner der Abdeckung, während unabhängig rekonstruierbare Klassen weiter ausgegeben werden; diese Namensprüfungen erfassen nicht sämtliche SDK-Namen, iOS-SDKs oder Versionen.

Unterstützte Objective-C-Block-Aufrufe benötigen eine vollständige feste skalare Aufruf-ABI einschließlich des verborgenen Block-Objekts sowie aller Argument- und Rückgabepositionen. Die Laufzeitkodierung `@?` wird nur in Deklarationen zu `id` erweitert; sie liefert keinen Aufrufprototyp. Referenzen auf globale Blocks bewahren die gemeinsame Objektidentität. Unterstützte synchrone skalare Captures erfordern einen Nachweis des nativen Capture-Speichers und Aufrufflusses. Starke Objektreferenzen, die ein verifizierter Aufruf von `objc_retainBlock` / `_Block_copy` kopiert, dürfen entkommen, wenn die Konstruktion ihren Speicher auf jedem erreichenden Pfad initialisiert und Aufruf-, Kopier- und Freigabefunktionen vollständig rekonstruiert sind. Der erzeugte Deskriptor bewahrt die ursprünglichen Helfer-ABIs und das Besitzlayout. Schwache/byref-Referenzen, unbekannte Layouts und nicht nachgewiesene Verbraucher bleiben unrekonstruiert. Vom Compiler als nicht entkommend deklarierte C-Block-Parameter werden ebenfalls unterstützt, wenn der genaue Import, die Parameterposition und die vollständige Callback-ABI übereinstimmen. Dieser Lebensdauervertrag bedeutet keinen schreibgeschützten Speicher. Beim gemeinsamen Linken von C-API-Quelltexteinheiten darf jeder Eintrag in `shared_block_functions` nur einmal definiert sein. Der mobile Export vereinigt übereinstimmende Definitionen und weist Konflikte einschließlich unterschiedlicher privater Aufrufziele zurück.

Protokollmethoden werden aus aufgelösten lokalen Laufzeitdaten gelesen, einschließlich geerbter Protokolle sowie erforderlicher und optionaler Instanz- und Klassenmethoden. Gewöhnliche und relative Listen nutzen denselben Decoder. Alle passenden Klassen- und Protokolldeklarationen müssen übereinstimmen, bevor ein Selektor eine feste Aufrufsignatur erhält; fehlerhafte Datensätze und Vererbungszyklen liefern keine Typhinweise. Syntaktisch gültige Zeiger auf Strukturen, Unions und Arrays bleiben opak, ohne Layoutannahmen. `objc_metadata.protocols` zeigt Deklarationen separat; sie zählen nicht als rekonstruierte Implementierungen und belegen keine Protokollkonformität einer Klasse. Bei ansonsten identischen Signaturen nutzen vorzeichenbehaftete und vorzeichenlose 64-Bit-Ergebnisse einen vorzeichenlosen Bitträger; Konflikte bei schmaleren Ganzzahlen, Gleitkommazahlen, Zeigern oder Argumenttypen bleiben unzulässig.

Bei vom Compiler deklarierten NSString-Formataufrufen lassen sich die promovierten skalaren Argumente aus einem geprüften konstanten Formatobjekt ableiten. Sequenzielle und positionale Argumente müssen vollständig und typkonsistent sein. Darwin arm64 liest variable Argumente aus acht Byte großen Stackplätzen; x86_64 verwendet Ganzzahl- und Gleitkommaregister sowie den Stack. Erzeugte Aufrufe behalten Ellipse und dynamische Auflösung. Unbekannte Formate, Zählwertschreibzugriffe, long double und nicht unterstützte Erweiterungen werden abgewiesen. Dieselbe Formatanalyse unterstützt deklarierte C-Importe wie `NSLog`, prüft den genauen Bibliotheksexport und teilt einen variadischen Prototyp zwischen Aufrufen mit unterschiedlicher Argumentanzahl.

Schreibzugriffe auf den privaten Stackframe erhalten jedes Byte, das irgendwo in der Funktion gelesen wird. Bei nachgewiesenen Framegrenzen, unveränderlichen Aliasen am Eintritt und nicht entweichenden Adressen kann NeverD ungelesene Schreibzugriffe entfernen oder das ungelesene Ende eines Ganzzahlzugriffs kürzen. Promovierte skalare Argumente bleiben dadurch rekonstruierbar, wenn nur ungenutzte Füllbytes unbekannt sind. Unbekannte Bits werden nicht ergänzt. Geordnete Zugriffe, ungebundene Aufrufe, mehrdeutige Adressen, Werte mit Seiteneffekten und ausgeschöpfte Analysebudgets erhalten die ursprünglichen Schreibzugriffe.

Kollidierende Kategoriemethoden behalten validierte ABI-Deklarationen auch bei unbekannter Überschreibungsreihenfolge. Ein dynamischer Aufruf darf dieses ABI nur nutzen, wenn alle passenden Deklarationen übereinstimmen; mehrdeutige Implementierungen bleiben von der Auswahl eines Quellcodekörpers ausgeschlossen. Widersprüchliche oder fehlerhafte Deklarationen verhindern den Aufruf weiterhin.

Bekannte Aufrufe von `objc_enumerationMutation` behalten das Objektargument und den anschließenden Ausführungspfad, da ein installierter Änderungshandler zurückkehren kann. Exakte Darwin-Importe von `__stack_chk_guard` binden die Identität des Laufzeitobjekts; Lesezugriffe, Vergleiche und Aufrufe von `__stack_chk_fail` bleiben im rekonstruierten Quelltext beobachtbar.

Gelinkte 64-Bit-Darwin-Abbilder mit einem Import der Systembibliothek Foundation verwenden auch integrierte, vom Compiler extrahierte Framework-Deklarationen. Alle Laufzeit- und Framework-Deklarationen eines Selektors müssen übereinstimmen; variadische Signaturen, nicht unterstützte Aggregate und Abweichungen zwischen Plattformen bleiben ungebunden. Der Katalog liefert nur Aufruftypen, keine Empfängerklassen oder Funktionsrümpfe. Für die Nutzung von NeverD ist kein lokales Apple SDK erforderlich. CoreData besitzt einen eigenen Katalog, der nur durch die exakte Systemframework-Abhängigkeit aktiviert wird. Deklarationen gehören zum Framework ihres öffentlichen Headers; indirekt eingebundene Header aktivieren kein anderes Framework.

Der C-Deklarationskatalog erfasst auch Exporte von CoreGraphics und ImageIO. Undurchsichtige Bild- und Farbzeiger, ganzzahlige Anzahlen und Gleitkommaergebnisse behalten ihr deklariertes ABI. Öffentliche Systemframework-Aliase werden zusammen mit Exportfakten erzeugt; private Pfade, andere Frameworkversionen und nicht deklarierte Symbole erhalten keine Bindung. Mobile-Deklarationen verwenden ebenfalls die Typgrammatik des Loaders: syntaktisch gültige Aggregatzeiger bleiben opak, ohne ein Layout anzunehmen.

Große unsterbliche Swift-Stringliterale können ihre UTF-8-Bytes an einer nachgewiesenen Foundation-Brücke in gemeinsamem statischem Speicher ablegen. Länge, Flags, Terminator, gültiges UTF-8, unveränderlicher Speicher und exakter Import müssen übereinstimmen. Die ursprüngliche markierte Darstellung und der Brückenaufruf bleiben erhalten; eingebettete Nullbytes und andere Speicherformen bleiben ungebunden.

Validierte konstante NSString-Objekte behalten ihre gemeinsame Identität auch bei Zuweisungen und Speicherzugriffen über Ganzzahlwerte mit nachgewiesener vollständiger Datenadresse. Skalare Konstanten, unvollständige Adressen, numerische Operationen und Zugriffe auf interne Objektbytes erhalten diese Bindung nicht.

Gewöhnliche skalare Lesezugriffe auf nachweislich unveränderliche, nicht relocierte Image-Bytes können zu bitgetreuen Konstanten werden. Unterstützt werden Ganzzahlen mit 1, 2, 4 und 8 Byte sowie Gleitkommawerte mit 4 und 8 Byte. Schreibbarer oder mehrdeutiger Speicher, geordnete Lesezugriffe und Adressverwendungen bleiben ungebunden; ein numerisches Vorkommen legitimiert keine Zeigerverwendung desselben Ausdrucks.

C-Aufrufe mit festen Parametern verwenden zusätzlich compilerabgeleitete Deklarationen und SDK-Exportinformationen einschließlich Reexports. Die konkrete dyld-Bibliothek, das Symbol und die skalare ABI müssen übereinstimmen; schwache Imports, unbekannte Anbieter und nicht unterstützte Prototypen bleiben ungebunden. Generiertes C verwendet eigene Bezeichner mit den ursprünglichen Linkersymbolen. Gewöhnliche Synchronisationsaufrufe ohne sprachspezifische Ausnahmetabellen behalten ihre Aufrufe und Speicherwirkungen.

Indizierte skalare Lesezugriffe können eine unveränderliche Bytetabelle verwenden, wenn die gemeinsame Quellflussanalyse auf jedem eintreffenden Pfad eine vorzeichenlose Obergrenze beweist. Bedingungen, Masken, native Ganzzahlbreiten und modularer Überlauf behalten ihre Bedeutung; Schreibzugriffe und entkommene lokale Adressen verwerfen frühere Fakten. Jede Tabelle ist auf 4.096 Einträge und 65.536 Bytes begrenzt. Vor der Ausgabe werden Grenzen, Speicher, Relokationen und jede Hilfsfunktionsverwendung erneut geprüft. Werte in voller Zeigerbreite, die eine Abbildsektion bezeichnen, bleiben mehrdeutig; schmalere Skalarteile und Segmentfüllbytes allein beweisen keine Zeigeridentität. Kopierte Bytes dienen nur skalaren Lesezugriffen; Tabellenadressen dürfen nicht entkommen. Ausführbare Regressionstests vergleichen Ganzzahlbits, Gleitkommabits einschließlich negativer Null und Verhalten außerhalb der Grenzen mit den ursprünglichen Methoden.

Der C-Katalog enthält Deklarationen aus `sys/mount.h` mit architekturspezifischen Linkersymbolen: ARM64 verwendet `getmntinfo`, x86-64 dagegen `getmntinfo$INODE64`. Der Ausgabeparameter bleibt ein Zeiger auf einen undurchsichtigen Zeiger; Modus und Rückgabewert bleiben vorzeichenbehaftete 32-Bit-Ganzzahlen. Die Systemexporte werden genau geprüft. Weder das Layout der Dateisystemeinträge noch der Inhalt des zurückgegebenen Puffers wird angenommen.

Externe Datenbindungen erfordern gemeinsame SDK-Deklarationen ohne TLS und genaue Exportnachweise der Bibliothek. Generiertes C referenziert den tatsächlichen Symbolspeicher und erhält nachfolgende Speicherzugriffe, einschließlich der Unterscheidung zwischen einem globalen Zeiger und seinem Ziel. Schwache Imports, widersprüchliche Identitäten und nicht unterstützter Speicher bleiben ungebunden. Datendeklarationen beweisen weder die Konstruktion noch die Eigentumsverhältnisse von Blöcken. Der Katalog umfasst Daten aus CoreData, CoreImage, CoreGraphics, ImageIO und CoreSpotlight. Für eingebaute Literale werden leere Sammlungen und boolesche Objekte für jedes Ziel kompiliert; nur direkte Adressen externer Daten ohne TLS bestehen dieselben Exportprüfungen. Die Katalogerzeugung benötigt neben libclang auch den über `--clang` angegebenen Clang-Compiler.

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

Ein nicht rekonstruierter Objective-C-Methodeneintrag kann `native_backend: {status, reason, diagnostics}` enthalten, wenn genau ein Backend-Ergebnis vollständig zur Laufzeitidentität passt. Diese optionale, größenbegrenzte Zusammenfassung bewahrt das Zwischenergebnis auch bei fehlgeschlagenen Deklarations- oder Layoutprüfungen. Maßgeblich bleiben der primäre Status, der Grund und die Wiederherstellungszähler; fehlt die Zusammenfassung, war dieser Nachweis nicht verfügbar.

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

Jede Methode enthält `projection_diagnostics`: `items` erfassen unabhängig prüfbare Hindernisse mit `code`, `reason` und verfügbaren Belegen zu Anweisung, Aufruf, Wert oder Abhängigkeit. `checks_complete: false` kennzeichnet fehlende Voraussetzungen oder Ressourcenlimits; ein leerer Teilbericht belegt keine Wiederherstellung. Dieselben Prüfungen dienen der Zulassung; `status`, `reason` und `unbound_call` bleiben erhalten.

Der `native_dependency_graph` verfolgt im endgültigen LowIR direkte Aufrufe in den Image-Code ab Methoden mit unterstützter Signatur. Er erhält Aufrufer-, Block- und Anweisungsadressen, gemeinsame Ziele und Zyklen; indirekte Ziele bleiben null. Fehlende benötigte LowIR-Funktionen oder das Aufzeichnungslimit setzen `inventory_complete` auf false. `targets_complete` verlangt zusätzlich direkte Ziele für alle erfassten Aufrufe. Nicht unterstützte Methodenwurzeln und unaufgelöste indirekte Ziele sind ausgeschlossen; vollständige native Ausführungsabdeckung oder Wiederherstellung abhängiger Quellen werden nicht bewiesen.

Validierte lokale Protokollreferenzen verwenden `objc_getProtocol`, um die Identität registrierter Protokolle zu erhalten. Die Abhängigkeitsliste `runtime_protocols` enthält auch native Aufrufziele. Namenskonflikte, unvollständige Deklarationen, importierte Slots und ungelöste Fixups bleiben ungebunden. Der eigenständige Export meldet fehlende Protokollregistrierung, statt einen Körper auszugeben, der ein Nullprotokoll erhalten könnte.

Der Swift-Laufzeit-ABI-Katalog wird aus festgelegten Upstream-Deklarationen erzeugt und bewahrt die C- oder Swift-Aufrufkonvention. Bekannte Zeiger und vorzeichenlose Werte in Zeigerbreite bilden feste skalare Signaturen. Swift-Aufrufe benötigen einen exakten starken libswiftCore-Import; auch die zwei unterstützten versionierten Metadaten-Verfügbarkeitsklassen erlauben keine schwachen Importe. Besondere Parameterregister, unbekannte Darstellungen, nicht unterstützte Verfügbarkeiten und Konflikte bleiben ausgeschlossen. Für swift_willThrow ergänzt der Compiler swiftself/swifterror-Attribute, die der Deklarations-DSL fehlen. Effekte und bestehende Callback-/Byteverträge bleiben erhalten. Erzeugen mit `scripts/generate_swift_runtime_declarations.py --input RuntimeFunctions.def --source-sha256 <pinned-hash>`; `--check` prüft den Katalog. Revision, Hash und Fremdhinweise begleiten die Fakten.

Feste Swift-Laufzeitdeklarationen erhalten auch Ergebnisse aus zwei Wörtern: zwei Zeiger oder den deklarierten Metadatenzeiger mit Zustandswort. Die gemeinsame ABI-Schicht weist beide Ergebnisregister zu; die Quellvalidierung prüft Reihenfolge, Typen und exakten Import erneut. Box-Allokation und Metadatenabfragen führen weiterhin echte Laufzeitaufrufe aus. Aggregatparameter, unbekannte Layouts und versteckte Kontexte bleiben ununterstützt.

Unabhängig davon bewahren exakt vom Compiler beobachtete Foundation-Wertbrücken für URLRequest, Notification, URL, Data, Date und IndexPath `swiftcall` und die Anbieteridentität. Indirekte Ergebnisse und Kontext/Self verwenden `swift_indirect_result` und `swift_context` in ihren festen Registern (x8/x20 auf arm64, RAX/R13 auf x86_64), ohne die normale Ganzzahlargumentbank zu belegen. Data behält seinen expliziten Zweiworttransport. Dies sind Deklarationen der Aufrufträger, keine rekonstruierten Wertlayouts und keine allgemeine Swift-ABI.

Die native Skalar-Rückgabeanalyse erkennt die vollständige, unmittelbar folgende Extraktion beider Wörter eines Aufrufs mit validiertem Ergebnis-ABI. Identität des temporären Werts, Feldversätze, Breiten und physische Register müssen übereinstimmen. Hilfsfunktionen können damit das erste Feld direkt zurückgeben; Registeransichten, Aufrufüberschreibungen und die Pflicht zu einem definierten Ergebnis auf jedem Rückgabepfad bleiben erhalten.

Der Katalog fester C-Laufzeitaufrufe erhält auch 32-Bit-Ganzzahlwerte und explizit mit Nullen erweiterte boolesche Ergebnisse. Ein boolesches Ergebnis belegt ein vollständiges vorzeichenloses Byte; daraus folgen keine Erweiterungsregeln für boolesche Parameter. Schmalere unbekannte Darstellungen und 32-Bit-Deklarationen mit Swift-Aufrufkonvention bleiben ausgeschlossen. Laufzeittests prüfen erfolgreiche und fehlgeschlagene Typumwandlungen, gezählte Referenzerhöhungen und Objektzerstörung; Aufrufe und Besitzwirkungen bleiben erhalten.

Metadaten gespeicherter Felder unterscheiden bekannte Byte-Layouts von unbekannten Sprachtypen. Swift-Klassen mit stabiler ABI verwenden auch auf arm64 Feld-Offset-Variablen in Zeigerbreite; nicht exponierte Felder können eine leere Objective-C-Typkodierung haben. NeverD behält diese fehlende Typinformation bei und prüft Offsets, Größen, Ausrichtung und Überlappungen. Wiederhergestellte C-Funktionskörper ermitteln die Offsets über die Ivar-Suche zur Laufzeit. Ein unbekannter Feldtyp verhindert weiterhin eine Klassendeklaration, die einen erfundenen Typ erfordern würde.

Die Identität stabiler Swift-Ivars bleibt auch bei zur Laufzeit initialisierten Offset-Slots in Zero-Fill-Speicher erhalten. Solche Klassen tragen `ivar_status: "runtime"`; unbekannte Offsets erscheinen als JSON `null`, Feldgröße 0 bezeichnet eine erst zur Laufzeit bekannte Breite. Methodenkörper können Offsets an der bestehenden Laufzeitklasse abfragen; deklarierte Objekttypen lassen sich über exakte Offset-Slots verfolgen. Literale Offsets erfordern weiterhin ein bekanntes Layout. Ein Swift-Klassenlayout wird damit nicht rekonstruiert: Der eigenständige Klassenexport lehnt unbekannte Layouts und Feldtypen weiterhin ab.

Bei direkt gebundenen nativen Aufrufen darf die Adresse eines ivar-Offsets durch die Adresse eines lokalen Skalars ersetzt werden, wenn die aufgerufene Funktion den Zeiger genau einmal am Eintritt vor beobachtbaren Effekten liest und ihn sonst weder schreibt, speichert, vergleicht noch verwendet. Der lokale Wert stammt aus der bestehenden ivar-Laufzeitabfrage; ABI und Funktionskörper bleiben erhalten. Der begrenzte Nachweis akzeptiert nur geradlinigen Kontrollfluss und verwirft unklare Verwendungen oder überschrittene Analysegrenzen. Die C API kann `.cxx_destruct` wiederherstellen; die eigenständige Objective-C-Methodensyntax kann diesen Selektor weiterhin nicht ausgeben.

Unveränderliche Bytepuffer werden nur rekonstruiert, wenn der geprüfte Vertrag des importierten Aufrufs den Lesezugriff auf eine nichtnegative Länge begrenzt und Schreibzugriffe, das Speichern von Zeigern und Vergleiche der Adressidentität ausschließt. Bytes und Länge bleiben erhalten; andere Zeigerverwendungen bleiben unaufgelöst. Der exakt erkannte Assertionsfehler der Swift-Standardbibliothek bewahrt seine vom Compiler abgeleiteten skalaren und Stack-Träger, `swiftcall`, den `noreturn`-Effekt und die Linkerschreibweise; die C-Meldungsfunktionen behalten ihre deklarierte C-ABI und eine separate nachfolgende Trap-Operation. Statische Strings und der Speicher unsterblicher String-Literale werden nur für diesen authentifizierten, nicht speichernden terminalen Verbraucher kopiert. Dynamische oder besessene String-Werte gelten nicht als Literale.

Für eine Sitzung mit geladener Mach-O-Datei liefern `neverd_objc_methods_json(session, max_functions)` und `neverd_swift_methods_json(session, signatures_json, max_functions)` die Berichte. Null wählt alle gefundenen Funktionen. Erfolgreiche Strings mit `neverd_free_string` freigeben; `NULL` bedeutet Fehler, erklärt durch den Sitzungsfehler. Diese APIs laden keine IPA- oder `.app`-Container.

Die Inferenz nativer Objective-C-Abhängigkeiten darf einen 64-Bit-Ganzzahlregisterparameter aus `NativeAnalysis` nach dem Binden der Quellaufrufe weiter auf 32 Bit verengen. Ein vollständiger HighIR-Verwendungsnachweis muss zeigen, dass jedes Vorkommen exakt die unteren vier Bytes beobachtet, entweder durch einen Byte-Auszug mit Offset null oder als exakt gebundenes ganzzahliges Aufrufargument. Der Nachweis erfolgt unabhängig je Parameter; vollbreite, von null verschiedene, fehlerhafte oder durch Budgetende unvollständige Verwendungen behalten die ursprüngliche Breite. Die verfeinerte Hilfsfunktion wird erneut geliftet und muss die üblichen Rumpf- und Abhängigkeitsabschlussprüfungen bestehen; die Rewrite-ABI bleibt unverändert.

Über Objective-C-Einstiegsthunks bereitgestellte, verzögert initialisierte statische Swift-Objektgetter können nur projiziert werden, wenn das `vgZTo`-Symbol, die Laufzeit-Methodensignatur, der Prädikattest, der `swift_once`-Aufruf, das Laden des Speichers und das Ergebnis von `objc_retainAutoreleaseReturnValue` einem exakten Compilermuster entsprechen. Die Symbole für Prädikat (`_Wz`), Initialisierer (`_WZ`) und Speicher (`vpZ`) müssen zusammenpassen; das rohe dritte Register darf nur als Once-Kontext auftreten, und der Initialisierer muss diesen Kontext ignorieren und darf keinen gewöhnlichen direkten Aufrufer haben. Die Projektion baut Prädikat und Objektspeicher neu auf, übergibt null als irrelevanten Kontext und behält den Initialisierer als geprüfte Abhängigkeit bei. Jede Abweichung bei Form, Symbolen, Parameternutzung oder Callback bleibt nicht wiederhergestellt.

Swift-Objective-C-Konstruktorthunks mit authentifiziertem `cfcTo`-Symbol und der Laufzeitsignatur `init` können ebenfalls ein nicht deklariertes drittes Argumentregister entfernen, wenn dessen einziges Vorkommen der Kontext eines einzelnen exakt geprüften `swift_once`-Aufrufs ist. Prädikat `_Wz` und Initialisierer `_WZ` müssen zusammenpassen; der Callback muss den Kontext ignorieren und darf keinen gewöhnlichen direkten Aufrufer haben. Die Projektion übergibt null, leitet nur den acht Byte großen Speicherbereich des Prädikats ab und erhält alle übrigen Kontrollfluss-, Speicher- und Aufrufeffekte; die üblichen Prüfungen des Quellrumpfs und des Abhängigkeitsabschlusses gelten weiterhin.

ARM64-Once-Callbacks dürfen einen sonst ungenutzten x2-Kontext an einen einzelnen verschachtelten `swift_once` weitergeben, wenn das äußere `_WZ`-Symbol und das innere `_Wz`/`_WZ`-Paar exakt stimmen, beide Callbacks keine gewöhnlichen direkten Aufrufer haben und der unabhängig typisierte Blatt-Callback seinen Kontext ignoriert. Erkennung und Projektion verwenden denselben Vertrag. Die Projektion übergibt null und erhält jede Anweisung; die void-Callback-ABI erlaubt nur das Verwerfen reiner Register-, Temporär- oder Konstantenrückgaben. Stack-Lesezugriffe, Ladeoperationen und Aufrufe in Rückgabeausdrücken sowie offene Abhängigkeiten bleiben ausgeschlossen.

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

Das strenge Swift-Prüfskript kontrolliert 22 Benutzerdeklarationen, drei Getter-/Setter-Einstiegspunkte und neun vom Compiler erzeugte aufrufbare Einträge; keiner darf aus dem Inventar verschwinden. Pro Variante werden 858 unabhängige Sollwerte des Originalprogramms geprüft. Erzeugtes `.swift` und Aufrufprogramm werden unabhängig kompiliert, ohne ursprüngliche dylib, Modul, Bridge oder handgeschriebene Ersatzdeklarationen. Die Fälle umfassen skalare/native Aufrufe, Klasseninitialisierung und -speicher, Strukturmethoden mit Wertübergabe/mutating, Gleitkomma- und Stack-Argumente, Zeiger und Schleifen. Die native C++20-CLI muss alle vier Varianten arm64/x86_64 × classic/default ohne übersprungene Fälle bestehen: jeweils 25 native Methodenrümpfe und neun Compiler-Projektionen wiederherstellen, alle 34 aufrufbaren Identitäten bewahren und für Originale sowie unabhängig kompilierten erzeugten Swift-Code 858/858 Sollwertprüfungen erfüllen. Diese Ergebnisse gelten nur für diesen Korpus und garantieren keine Wiederherstellung beliebiger Anwendungen oder des ursprünglichen Quelltexts. Das Skript weist fehlende Abdeckung, Quellcode-Kompilierfehler und Verhaltensabweichungen zurück.

Diese vier Varianten zielen auf macOS. Die beiden zusätzlichen Compiler-Einträge sind der Initialisierer des leeren Werttyps und sein Metadatenzugriff; native Nachweise prüfen sie getrennt, und eine gemeinsame Quelleinheit `struct Empty {}` bewahrt beide Identitäten. Das Bestehen dieses Korpus qualifiziert keine reale iOS-Anwendung.

Alle drei Skripte unterstützen `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N` und `--work-dir NEW_DIRECTORY`. `--setup-only` prüft die Originale und nicht die Wiederherstellung. Für die Abnahme müssen alle angeforderten Architektur- und Fixup-Varianten vollständig durchlaufen werden, auch im Skalartest. Eine fehlende Variante oder eine auf dem Host nicht ausführbare angeforderte Architektur führt zum Fehlschlag; Überspringen ist nicht erlaubt. Gesicherte Fehlerartefakte helfen, fehlende Quellcodeabdeckung, Kompilierfehler und Verhaltensabweichungen zu unterscheiden. Prüfen Sie die aktuellen Ergebnisse, bevor Sie Unterstützung als verifiziert bezeichnen.

Der [Workflow Mobile Real Applications](../../.github/workflows/mobile-real-apps.yml) verwendet öffentliche Anwendungen mit festgelegten Versionen aus dem [Korpusmanifest](../../scripts/mobile_real_apps.json). Die Abnahme verlangt unabhängige Inventare aller Mach-O-Dateien vollständiger iOS-Bundles und aller DEX-Dateien der APKs, getrennte Neubauten der Originale und des erzeugten Quellcodes sowie Verhaltensvergleiche. Fehlende Stufen, unbekannte Inventarabdeckung oder fehlende Pflichtfälle lassen die Abnahme scheitern. Die Stufen recompile und behavior für reale Anwendungen sind noch unvollständig; daher bleibt die Kennzeichnung Experimental bestehen. Bestandene Schutztests des Testgerüsts sind kein Nachweis einer erfolgreichen Prüfung realer Anwendungen.

Die Veröffentlichung ist transaktional: neues Verzeichnis wählen, zuerst den Exitstatus prüfen und umgeleitetes JSON außerhalb speichern. Fehler entfernen temporäre Ausgabe und erhalten vorhandene Ergebnisse. Backend-Exits mit einem Wert ungleich null enthalten ein begrenztes Log-Ende. Bei Backend-Zeitüberschreitungen bleibt die ursprüngliche Timeoutmeldung erhalten; ein begrenztes Log-Ende wird angefügt, wenn bereits erfasster Logtext verfügbar ist. Budgetverletzungen behalten ihre eigenen Meldungen. Die native CLI liefert bei Erfolg null und bei Wiederherstellungsfehlern einen Wert ungleich null. Mit `--json` enthalten behandelte Fehler `schema_version`, `status: "error"` und `error`. Argumentprüfung, Startfehler nativer Programme oder Bibliotheken und Unterbrechungen können ausschließlich auf stderr erscheinen. Prüfen Sie zuerst den Exitstatus.

Bei verschlüsselten Slices lesbare Eingaben liefern und bei fehlender Architektur verfügbare Slices prüfen. Für ausgelassene Methoden genaue Gründe und Metadatendiagnosen lesen. Ein größeres `--max-func` hilft nur bei durch das Limit ausgeschlossenen Funktionen. Fehlende Layouts, Signaturen, externe Header, Ausnahme- oder ABI-Unterstützung benötigen Implementierung oder zusätzliche gültige Metadaten, keine Behauptung vollständiger Wiederherstellung. Anwendbare Lizenzhinweise von Abhängigkeiten bei der Weitergabe erhalten.

Die Inferenz nativer skalarer Hilfsfunktionen unterstützt auch float/double-Registerparameter und Rückgaben. Die gemeinsame MedIR-Analyse der Eingangsbytes muss beweisen, dass ein breiter Vektoreingang nur eine untere skalare Lane beobachtet. Lokale CONCAT-Werte dürfen nur verkleinert werden, wenn alle Definitionen dieselbe untere Breite haben, verworfene obere Ausdrücke keine Effekte besitzen und jede Verwendung ausdrücklich dieses Präfix liest. Aufrufe, Speicherzugriffe, Verzweigungspositionen und Gleitkommabits außer NaN bleiben erhalten; unbekannte obere Bits werden nicht ergänzt.

Eine native Blatt-Hilfsfunktion mit validierten externen void-Endaufrufen darf ohne vollständiges skalares Ergebnis eine interne void-Quellsignatur erhalten. LowIR muss jeden Aufruf und dessen synthetische Rückkehr bestätigen. Der Blattnachweis verbietet Schreibzugriffe auf erhaltene, Frame-, Stack- und Linkregister; ein begrenzter Byte-Taint-Fixpunkt verwirft außerdem gespeicherte oder an Aufrufe übergebene stackabgeleitete Werte. CFG-, Rumpf- und Abhängigkeitsprüfungen bleiben bestehen. Ergebnisbits werden nicht bereitgestellt: Aufrufer mit unbekannten Ergebnislesezugriffen bleiben unrekonstruiert. Laufzeittests prüfen bedingte Objektzerstörung, unabhängige Aufrufergebnisse und die Ablehnung unbekannter Ergebnislesezugriffe.

Native void-Zusammenfassungen unterstützen auch Stackframes und gewöhnliche Aufrufe, wenn eine begrenzte LowIR-Analyse an jedem Ausgang die Wiederherstellung der erhaltenen Registerbytes, des Stackpointers und des Linkregisters beweist. Teilzugriffe, implizite Nullerweiterungen, überlappende Speicherzugriffe und Aufrufe entkräften betroffene Fakten. Speicherzugriffe über Adressen, die nachweislich keine vom Frame abgeleiteten Bytes enthalten, sind vom privaten Aufrufframe getrennt; teilweise oder vollständig vom Frame abgeleitete Aliase sowie gespeicherte oder entkommende Frameadressen führen zur Ablehnung. Die einzige Ausnahme für einen ausgeliehenen Frame ist das exakte erste Argument eines quellgebundenen `objc_msgSendSuper2`: Es darf ein vollständiges 16-Byte-Objekt `objc_super` bezeichnen, das vollständig im reservierten Frame liegt, weil dieser Laufzeitvertrag es synchron liest. Gewöhnliche Nachrichten, unvollständige Zeiger, Objekte außerhalb des Frames und alle anderen Frameargumente bleiben abgelehnt. Die HighIR-Byte-Lebendigkeit behandelt einen Stackslot-Wert als begrenztes Lesen der Bytes dieses Slots; das Bilden oder Übergeben seiner Adresse bleibt dagegen ein Entkommen. Fehlerhafte oder überlappende Zugriffe werden weiterhin konservativ behandelt. So können getrennte private Speicherungen nur ungelesene Suffixbytes verwerfen. Nach erneutem Lifting können Hilfsregisterparameter ohne jedes Vorkommen im HighIR entfernt und die Pipeline erneut ausgeführt werden. Private Speicherzugriffe werden weiterhin von der bestehenden HighIR-Bereinigung entfernt; reguläre Parameter und tatsächliche Eingaben bleiben erhalten.

Der arm64-UIKit-Katalog bindet auch das Objektergebnis von `observedProgress` und `setProgress:animated:` aus `UIProgressView`. Letzteres bewahrt getrennte Register für das `float`- und das boolesche Argument. Beide Deklarationen erfordern übereinstimmende Compilerbelege für iPhoneOS und arm64 iPhoneSimulator sowie den exakten UIKit-Systemanbieter. Andere Architekturen bleiben nicht unterstützt.

Markierte Literalwörter dürfen eine vollständige Bildadresse aus einem unveränderlichen C-String-Pool unter dem exakten höchsten Tag-Bit enthalten. Die Quellbindung verlagert nur diese nachgewiesene Adresse in den bestehenden permanenten gemeinsamen Pool und erhält das ganzzahlige OR, das Tag und den inneren Offset. Ähnliche Skalare oder Teiladressen, widersprüchliche Herkunft, veränderliche oder relocierte Pools sowie direkte Speicheradressnutzung bleiben abgelehnt; daraus wird kein Swift-String-Objektlayout abgeleitet.

`setProgress:` von `UIProgressView` erfordert einen nachgewiesenen Empfänger, etwa ein typisiertes Eigenschaftsergebnis, das ein exakter ARC-Identitätsaufruf bewahrt. Die geprüfte vollständige Oberklassen- und Protokollkette wählt das `float`-Argument. Ohne qualifizierten Empfänger bleibt der Aufruf mehrdeutig, da UIKit und lokale Klassen auch Setter mit Objektargument unter diesem Selektor deklarieren.

Der UIKit-Katalog für arm64 erfasst außerdem `setTitleColor:forState:` von `UIButton` mit einem Objektargument und einem vorzeichenlosen 64-Bit-`UIControlState` sowie die void-Methode `invalidateIntrinsicContentSize` von `UIView`. Die vollständigen Geräte- und Simulator-ASTs stimmen überein. Die nachgewiesene Vererbung `UIImageView → UIView` ermöglicht bestehenden Receiver-Nachweisen, lokale Objekt-Setter von gleichnamigen Gleitkomma-Settern anderer Klassen zu unterscheiden. Unbekannte Receiver, widersprüchliche Unterklassendeklarationen, fehlende Anbieter von Elterndeklarationen und x86_64 bleiben ununterstützt.
