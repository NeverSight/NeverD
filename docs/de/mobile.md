**Sprachen**: [English](../mobile.md) | [简体中文](../zh-CN/mobile.md) | [繁體中文](../zh-TW/mobile.md) | [日本語](../ja/mobile.md) | [한국어](../ko/mobile.md) | [Français](../fr/mobile.md) | [Deutsch](mobile.md) | [Español](../es/mobile.md) | [Italiano](../it/mobile.md) | [Русский](../ru/mobile.md) | [العربية](../ar/mobile.md)

# Wiederherstellung mobiler Anwendungen

[← Dokumentationsübersicht](README.md) · [Vollständige Android-Anleitung](android.md) · [Vollständige iOS-Anleitung](ios.md)

`neverd mobile` stellt lesbares Java aus Android-APK-, DEX- und smali-Eingaben wieder her. Für iOS-IPA-, `.app`- und Mach-O-Eingaben exportiert es natives C und rekonstruiert unterstützte Objective-C-Methodenrümpfe als `.m`-Quelltext, ergänzt um experimentellen Swift-Quelltext und Laufzeitmetadaten. Dies ist ein experimenteller CLI-Ablauf; das native C-SDK und der GUI-Loader akzeptieren keine mobilen Container.

## Einrichtung

Bauen Sie das Target `neverd` mit C++20-Unterstützung. Der mobile Ablauf ist in die native CLI einkompiliert und verwendet keinen Python-Interpreter. Verteilen Sie die ausführbare Datei zusammen mit den nativen Bibliotheken, die Ihr Build benötigt.

Die native ZIP-Verarbeitung nutzt zlib für CRC-32 und DEFLATE. CMake bevorzugt eine installierte Bibliothek über `find_package`; andernfalls lädt es zlib 1.3.2 mit festgelegter SHA256-Prüfsumme herunter und baut sie statisch. Diese mobile ZIP-Implementierung benötigt unter Windows keine Python-Hilfsprogramme. Behalten Sie die Hinweise zu Abhängigkeiten in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) bei.

Die Standard-Engine ist in C++20 implementiert und benötigt keine Python-, Java- oder JADX-Laufzeit. Nur ein ausdrückliches `--jadx PATH` wählt den separat installierten Kompatibilitätsadapter; `NEVERD_JADX` und PATH wählen ihn nicht automatisch, und es gibt keinen automatischen Rückgriff. Der optionale Adapter benötigt JADX 1.5.6+ mit den standardmäßigen DEX/smali-Eingabeplugins sowie Java 11+. Sein Bericht nennt die tatsächliche Engine `jadx` und deren Version. Installation und Lizenzen der Abhängigkeiten sind weiterhin in der [Android-Anleitung](android.md#optionaler-jadx-kompatibilitätsadapter) dokumentiert.

## Android

Für ein schnelles Klassenverzeichnis ohne Java-Wiederherstellung verwenden Sie
`neverd mobile app.apk --list-classes`, optional mit
`--class-prefix com.example` oder `--json`. Der
[Inventarvertrag](android.md#schnelles-klasseninventar) beschreibt Reihenfolge,
Limits und Validierung ausgewählter Nutzdaten. Der Abfragemodus benötigt kein
Ausgabeverzeichnis; sein optionales `-o` bezeichnet eine neue Datei.

Für direkte Bytecode-Referenzen verwenden Sie
`neverd mobile app.apk --find-refs string --query 'example' --json`.
Der [Vertrag für Referenzabfragen](android.md#abfragen-von-codereferenzen)
beschreibt auch Typ-, Methoden- und Feldoperanden, wörtlichen/exakten Abgleich,
Positionen jedes Vorkommens, UTF-16-Erhaltung und den Umfang der Codevalidierung.
Ohne `--json` liefert diese Operation JSON Lines. Für `-o` gilt ebenfalls das
Verhalten einer neuen Datei.

Die folgenden Beispiele und Ausgaben beschreiben die Wiederherstellung.

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Alle `classes.dex`, `classes2.dex` und nachfolgenden nummerierten DEX-Dateien im Wurzelverzeichnis einer APK werden gemeinsam analysiert. Ein smali-Verzeichnis wird rekursiv durchsucht, und alle seine Klassen werden in einem Aufruf analysiert, einschließlich verschachtelter und benachbarter Klassen. Verwenden Sie ein Verzeichnis, wenn Sie Klassen wiederherstellen, die aufeinander verweisen. Eine einzelne smali-Datei liefert nur diese Klasse.

Die Ausgabe der integrierten Wiederherstellung enthält `sources/`, `metadata/android-methods.json` und `report.json` mit `backend: {"name": "neverd", "version": "1", "execution": "builtin"}`. Der Bericht enthält `android_method_recovery`: `method_count = recovered_method_count + declaration_only_method_count`, und `unrecovered_method_count` muss vor der Veröffentlichung null sein. Ursprüngliche `native`-/`abstract`-Deklarationen werden getrennt von wiederhergestellten Rümpfen gezählt. Der ausdrücklich gewählte externe Adapter behält seine eigenen Backend-Protokolle. APK-Ressourcen, Manifeste, native Bibliotheken und dynamisch geladener Code liegen außerhalb dieses Java-Pfads; native Bibliotheken können separat mit `neverd decompile` analysiert werden.

Die integrierten Wiederherstellungsleser teilen ein eigenständig implementiertes typisiertes Dalvik-Modell und einen Java-Generator mit begrenztem Arbeitsaufwand für darstellbaren gewöhnlichen DEX-035/037–040- und smali-Code. DEX 041, dynamische Aufrufe wie `invoke-custom`, einige Initialisierungspfade, unbekannte Operationen und in Java nicht darstellbare Bezeichner führen ausdrücklich zum Fehler. Generiertes Java kann eine Dispatch-Schleife verwenden; es führt weder den ursprünglichen DEX aus noch ruft es ihn über eine Laufzeitbrücke auf. Ursprüngliche Kommentare, Formatierung und entfernte Namen lassen sich nicht wiederherstellen. Die experimentelle Engine verspricht weder JADX-Funktionsgleichheit noch semantische Äquivalenz oder vollständige Wiederherstellung beliebiger APKs.

## iOS

Die [vollständige iOS-Anleitung](ios.md) beschreibt die Auswahl von IPA, `.app` und Mach-O, Einrichtung, sämtliche CLI-Optionen, Quelltextschemas, Abdeckung und Verifikation.

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

Jeder Lauf wählt eine ausführbare Datei. `--artifact` ist sowohl für IPA als auch für `.app` relativ zum Anwendungsbundle. Bei Fat-Binärdateien wird arm64 vor arm, x86_64 und i386 bevorzugt; die Projektion in Quellsprachen zielt auf arm64/x86_64. Ausgewählte verschlüsselte Slices werden abgelehnt. Der experimentelle native Pfad erzeugt C und unterstützte Objective-C-Methodenrümpfe, einschließlich ABI-Bindungen für Skalare/Zeiger, Gleitkommawerte, gemischte Parameter und Stack-Positionen. Laufzeitlayouts von Klassen/Instanzvariablen und separate Kategorien bleiben nach erfolgreicher Validierung erhalten; nicht aufgelöste Layouts, Signaturen, Aufrufe und sonstige Abhängigkeiten bleiben ausdrücklich als fehlend ausgewiesen.

Die Swift-Wiederherstellung klassifiziert Signaturen innerhalb des C++-Prozesses mit `LLVMSwiftDemangle` aus dem NeverD-LLVM-Fork. Sie startet weder einen externen Demangler noch einen Befehl zur Toolchain-Erkennung und benötigt zum Bauen oder Ausführen von NeverD keinen installierten Swift-Compiler. Quellbuilds des Forks und passende LLVM-Pakete enthalten die Komponente; NeverD lädt keine separate Swift-Quellabhängigkeit. Das Signaturinventar erfasst `demangler: {"name": "llvm-swift-demangle", "execution": "builtin", "version": "6.3.3"}`. Unterstützte Signaturen werden nativen Einstiegspunkten und ABI-Positionen zugeordnet, bevor tatsächliche `.swift`-Funktionen, Klassenmethoden/Initialisierer und Methoden von Strukturen mit festem Layout ausgegeben werden. Generische/resiliente, asynchrone/werfende, nicht unterstützte laufzeitgenerierte aufrufbare Formen und unvollständige Gruppen von Quelltextabhängigkeiten bleiben nicht wiederhergestellt.

Die normale Ausgabe enthält `sources/native.c`, optional `sources/objc.m` und `sources/swift.swift`, Deklarationen und Laufzeitmetadaten, JSON zur Methoden-/Signaturabdeckung, Protokolle, `artifacts/selected.macho` und `report.json`. Es gibt keine Protokolle einer externen Swift-Toolchain-Erkennung oder eines externen Demanglers. Der erzeugte Quelltext ruft das ursprüngliche Binärprogramm nicht als Wiederherstellungsbrücke auf. Swift-`source_units` gruppieren Typdeklarationen und Methoden; einzelne Methodenzeilen dürfen nicht zur Rekonstruktion von Klassen aneinandergehängt werden. Ein äußeres `status: "success"` bedeutet Veröffentlichung der Ausgabe, nicht vollständige Methodenabdeckung oder semantische Äquivalenz.

`--metadata-only` führt weder den nativen Quellexport noch das Demangling von Signaturen aus und erzeugt weder Quelltext noch Methodenabdeckung. Alle Modi verwenden die vom nativen Loader aufgelösten Objective-C-Metadaten. Swift-Metadaten verwenden begrenzte Lesezugriffe auf native Images; nicht unterstützte Fixups, relozierbare Layouts oder Referenzen behalten Teildiagnosen. `--max-func` begrenzt die Wiederherstellung nativer Funktionen und wird im reinen Metadatenmodus ignoriert. Fehlende native Funktionsrümpfe lassen einen normalen Lauf fehlschlagen. Temporär entpackte Eingaben werden entfernt.

Ursprüngliche Kommentare, Formatierung, entfernte Bezeichner und beim Kompilieren verlorene Quelltextstrukturen lassen sich nicht exakt rekonstruieren. Lesen Sie vor Verwendung der Ausgabe den Wiederherstellungsstatus und die Begründung jeder Methode, die getrennten Swift-Zähler für aufrufbare/nicht aufrufbare/nicht klassifizierte Einträge sowie die dokumentierten Grenzen.

## Grenzen und Fehler

Für die Wiederherstellung muss `-o` ein neues Verzeichnis außerhalb jeder Verzeichniseingabe benennen. Abfragemodi akzeptieren stattdessen eine optionale neue Ausgabedatei. Bestehende Ausgaben werden nie überschrieben. Wiederherstellungsarbeit wird zwischengespeichert und erst nach erfolgreicher Wiederherstellung und Ausgabevalidierung veröffentlicht. Abfrageergebnisse werden gepuffert, bis alle ausgewählten DEX-Dateien erfolgreich verarbeitet sind. Erfolgreiche native CLI-Läufe liefern null zurück. Wiederherstellungsfehler liefern einen Wert ungleich null; `--json` meldet behandelte Fehler mit `schema_version`, `status: "error"` und `error`. Fehler beim Parsen von Argumenten, beim Start der nativen ausführbaren Datei oder ihrer Bibliotheken sowie Unterbrechungen können stattdessen auf stderr erscheinen. Verbraucher müssen zuerst den Exit-Status prüfen.

Die Standardwerte sind 20 000 Einträge, 2 GiB Eingabe-/extrahierte Daten oder endgültige Ausgabedaten sowie 300 Sekunden für integrierte Android-/iOS-Analysen oder jeden ausdrücklich gewählten JADX-Prozess. iOS-Kindprozesse erhalten das verbleibende gesamte Analysebudget. Der integrierte Leser und Generator erzwingen außerdem ein begrenztes Arbeitsbudget. Mit `--max-files`, `--max-bytes` und `--timeout` passen Sie diese positiven Limits an. Während Backends laufen, wird der temporäre Arbeitsbereich überwacht; bis zum Dreifachen der Eintrags-/Bytelimits ist zulässig, damit vorbereitete Eingaben und Zwischenausgaben gleichzeitig bestehen können. Diagnosen sind auf 16 MiB je Prozess begrenzt.

Bei der Wiederherstellung schreibt die APK-Vorbereitung nur `classes.dex`, `classes2.dex` und nachfolgende nummerierte DEX-Dateien aus dem Wurzelverzeichnis. Jeder ZIP-Eintrag durchläuft dennoch Header-/Bereichsprüfungen, Dekomprimierung sowie Längen- und CRC-Prüfungen und zählt für die Archivlimits für Einträge und unkomprimierte Bytes. Nicht geschriebene Ressourcen können verschiedene, groß-/kleinschreibungssensitive Namen wie `res/-A.xml` und `res/-a.xml` tragen. Exakt doppelte ZIP-Namen und Datei-/Verzeichnis-Identitätskonflikte bleiben Fehler; portable Prüfungen auf Groß-/Kleinschreibungskollisionen im Dateisystem gelten für tatsächlich geschriebene Einträge. Vollständige Extraktion, einschließlich IPA-Eingaben, lehnt solche Ausgabekollisionen weiterhin ab. Pfadtraversierung, Links, Spezialdateien und verschlüsselte ZIP-Einträge werden im gesamten Archiv abgelehnt. Verzeichniseingaben lehnen ebenfalls symbolische Links und Spezialdateien ab.

Diese Limits dienen der Robustheit und sind keine Sandbox für Backend-Code Dritter. Ausdrücklich gewählte JADX- und native Quellexportbefehle laufen als lokale Kindprozesse. Fehlgeschlagene temporäre Ausgaben werden entfernt. Backend-Exits ungleich null enthalten einen begrenzten Diagnoseausschnitt vom Protokollende. Backend-Zeitüberschreitungen behalten die Timeout-Meldung bei und hängen einen begrenzten Ausschnitt an, wenn erfasster Protokolltext vorliegt. Startfehler und Budgetverletzungen behalten ihre eigenen Fehlermeldungen.

## Verifikation

Python wird nur von den folgenden Entwicklungstestprogrammen verwendet; die integrierte mobile Wiederherstellung läuft in der nativen C++20-CLI.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

Die Komponententests decken Parsing, unsichere Container, Backend-Fehler, Ausgabebereinigung, Architekturauswahl und Ausgabeerhaltung ab. Tests, die die gebaute CLI aufrufen, verwenden `NEVERD_BUILD_DIR`. Das interne Android-Testprogramm nutzt ein JDK (`java` und `javac`) und D8, um unabhängige DEX-/APK-Fixtures zu bauen und anschließend wiederhergestelltes Java zu kompilieren und auszuführen. Dies sind Verifikationsabhängigkeiten, keine Voraussetzungen für die integrierte Wiederherstellung. Führen Sie die Tests mit dem aktuellen Build aus und prüfen Sie das Ergebnis, bevor Sie einen Fall als verifiziert einstufen. Das separate Kompatibilitätstestprogramm benötigt zusätzlich JADX; erfolgreiche Fixtures belegen keine Wiederherstellung beliebiger Anwendungen.

Führen Sie auf macOS mit Apple Clang, seinem SDK und einem gebauten NeverD den tatsächlichen Objective-C-Ausführungsvergleich aus:

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

Das Testprogramm baut sein eigenes Objective-C-Fixture, stellt dessen Methodenimplementierungen wieder her und verknüpft nur die wiederhergestellte `.m`-Datei mit demselben unabhängigen Aufruf-Testprogramm. Sein Fixture mit 22 Methoden vergleicht 141 beobachtbare Ergebnisse für Ganzzahlgrenzen, Verzweigungen, Schleifen, Zeigerlese-/-schreibzugriffe, versteckte und ungenutzte Argumente, bitgenaue float-/double-Identitäten, gemischte Parameter und Stack-Positionen. Bevor ein Fall als verifiziert gilt, ist ein aktueller erfolgreicher Lauf erforderlich. Jede angeforderte Architektur- und Fixup-Variante muss abgeschlossen werden; standardmäßig wird arm64/x86_64 × classic/default abgedeckt. Eine fehlende Variante oder eine vom Host nicht ausführbare Architektur gilt als Fehler; Überspringen ist nicht erlaubt. Diese Fixture-Nachweise begründen keine Vollständigkeit für beliebige iOS-Programme. `NeverDMobileIOSBackend` ist unter macOS bei CTest registriert, einschließlich des Haupt-CI-Testprofils.

Das unabhängige Swift-Wiederherstellungstestprogramm ist `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd`. Es kompiliert generiertes Swift und sein Testprogramm erneut, ohne das ursprüngliche Binärprogramm einzubinden; nicht unterstützte aufrufbare Einträge und Verhaltensabweichungen gelten als Fehler. Die [iOS-Anleitung](ios.md) beschreibt die Abdeckungssemantik und erhaltene Fehlernachweise.

Der [Workflow Mobile Real Applications](../../.github/workflows/mobile-real-apps.yml) nutzt öffentliche Anwendungen, die im [Korpusmanifest](../../scripts/mobile_real_apps.json) festgelegt sind. Seine Abnahme verlangt unabhängige Inventare jedes DEX in jeder APK und jedes Mach-O in jedem vollständigen iOS-Bundle, unabhängiges Neubauen der Originale und generierten Quellen sowie Verhaltensvergleiche. Fehlende Phasen, unbekannte Inventarabdeckung oder fehlende Pflichtfälle lassen die Prüfung fehlschlagen. Die Neukompilierungs- und Verhaltensphasen für reale Apps sind noch unvollständig; die Unterstützung behält deshalb die Einstufung Experimental. Erfolgreiche Schutztests des Testprogramms belegen keinen Erfolg mit realen Apps.

Android-Fälle versuchen, den gesamten inventarisierten generierten Java-Bestand mit javac und D8 zu kompilieren, wobei ausschließlich Android-SDK-Deklarationen verwendet werden. Ursprünglicher Anwendungsbytecode, Implementierungen von Abhängigkeiten und Ersatz-Stubs dürfen fehlende Wiederherstellung nicht ausgleichen. Teilweise Wiederherstellung und Compilerfehler bleiben in den Nachweisen; erfolgreiches Kompilieren erfordert weiterhin unabhängige Verifikation, vollständige APK-Rekonstruktion und ART-Verhaltensvergleich. Das iOS-Orakel gleicht auf Datenträger gespeicherte Objective-C-Methodeneinträge mit der Ausgabe von Apple-Werkzeugen ab und bewahrt Klassen-, Metaklassen-, Kategorie-, Listen- und Ordinalidentitäten. Nicht aufgelöste Zeiger oder ausgelassene Slots lassen das Inventar unbekannt. Separate Inventare von Geräte- und Simulator-SDK-Deklarationen unterstützen Framework-Importe, ohne Header als Nachweis für Instanzlayout oder Verhalten zu behandeln.
