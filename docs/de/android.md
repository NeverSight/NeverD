**Sprachen**: [English](../android.md) | [简体中文](../zh-CN/android.md) | [繁體中文](../zh-TW/android.md) | [日本語](../ja/android.md) | [한국어](../ko/android.md) | [Français](../fr/android.md) | [Deutsch](android.md) | [Español](../es/android.md) | [Italiano](../it/android.md) | [Русский](../ru/android.md) | [العربية](../ar/android.md)

# Java-Rekonstruktion für Android

[← Dokumentationsübersicht](README.md)

`neverd mobile` stellt lesbares Java aus APK, DEX und smali standardmäßig mit der integrierten NeverD-Engine wieder her. Eigenständig implementierte Leser teilen sich ein typisiertes Dalvik-Modell und einen Java-Generator mit begrenztem Arbeitsaufwand. Diese experimentelle CLI-Funktion verspricht weder Funktionsgleichheit mit JADX noch die vollständige Wiederherstellung beliebiger APKs. APK-Container und Java-Ausgabe sind nicht über das native C-SDK, Python-Plugin-SDK, den GUI-Loader oder `neverd decompile --language` verfügbar.

Das erzeugte Java ist eine Rekonstruktion des Bytecodes. Ursprüngliche Kommentare, Formatierung, Entscheidungen der ursprünglichen Quellsprache und entfernte Bezeichner sind nicht verfügbar; auch Kotlin-Bytecode ergibt Java. Ein erfolgreicher Lauf beweist keine semantische Gleichwertigkeit und garantiert nicht, dass sich jede Methode erneut kompilieren lässt. Die analysierte Anwendung wird bei diesem Ablauf nicht gestartet.

## Schnellstart

Richten Sie zunächst die unten beschriebenen Laufzeitumgebungen ein und wählen Sie ein neues Ausgabeverzeichnis:

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Unter `recovered-app/sources/` finden Sie die Java-Dateien. `recovered-app/report.json` enthält das Eingabeinventar und die Einschränkungen. Wenn Smali-Klassen aufeinander verweisen, ist die Übergabe eines Verzeichnisses vorzuziehen.

## Laufzeitumgebungen einrichten

| Komponente | Voraussetzung | Auswahl |
|------------|---------------|---------|
| NeverD | Das Ziel `neverd` bauen; auch das danebenliegende Verzeichnis `mobile/` mitliefern | `build/bin/neverd` oder eine ausführbare Datei im PATH |
| Python | Python 3.10 oder neuer, unabhängig vom eingebetteten Plugin-Host | `--python`, danach `NEVERD_PYTHON`, danach `python3`/`python` im PATH |

Die Standard-Engine verwendet ausschließlich die Python-Standardbibliothek; Java und JADX werden zur Ausführung nicht benötigt. Sie verarbeitet darstellbare gewöhnliche Deklarationen und Operationen aus DEX 035, 037–040 und smali. DEX 041, dynamische Aufrufe wie `invoke-custom`, einige Initialisierungspfade, unbekannte semantische Annotationen oder Operationen sowie in Java nicht darstellbare Bezeichner führen ausdrücklich zum Fehler. Die Annahme eines Dateiformats bedeutet nicht, dass alle seine Anweisungen und Deklarationen unterstützt werden.

### Linux und macOS

```sh
cmake --build build --target neverd
python3 --version

./build/bin/neverd mobile app.apk -o recovered-app --python python3
```

Mit `NEVERD_PYTHON` lässt sich der Interpreter für weitere Aufrufe festlegen. Weder `NEVERD_JADX` noch ein `jadx` im PATH wählen die externe Engine aus; dies geschieht ausschließlich durch ein explizites `--jadx PATH`. Es gibt keinen automatischen Rückgriff. Pfade mit Leerzeichen müssen in Anführungszeichen stehen.

### Windows PowerShell

```powershell
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app `
  --python 'C:\Tools\Python\python.exe'
```

Bei Builds mit mehreren Konfigurationen kann die ausführbare Datei unter `build/bin/Release/` liegen. Behalten Sie beim Verschieben das benachbarte Verzeichnis `mobile/` bei; andernfalls wird ein fehlendes Hilfsprogramm gemeldet.

## Unterstützte Eingaben und Grenzen

| Eingabe | Verhalten | Wichtige Grenze |
|---------|-----------|-----------------|
| `.apk` | Das gesamte ZIP validieren und anschließend alle `classes.dex`, `classes2.dex` und weiteren nummerierten DEX-Dateien im Archivwurzelverzeichnis gemeinsam analysieren | Nur Code; keine Dekodierung von Ressourcen oder Manifest |
| `.dex` | DEX 035 oder 037–040 mit dem integrierten Leser prüfen und analysieren | DEX 041 und nicht unterstützte Deklarationen oder Operationen scheitern; umbenannte oder abgeschnittene Dateien sind kein gültiger Bytecode |
| `.smali` | Die übergebene Klasse analysieren | Referenzierte benachbarte Klassen werden nicht implizit geladen |
| Smali-Verzeichnis | `.smali`-Dateien rekursiv sammeln und gemeinsam analysieren | Verschachtelte Klassen und abhängige Smali-Wurzelverzeichnisse in das Eingabeverzeichnis aufnehmen |

Wenn ein dekodierter APK-Verzeichnisbaum sowohl `smali/` als auch `smali_classes2/` enthält, übergeben Sie deren gemeinsames übergeordnetes Verzeichnis. Nur `.smali`-Dateien gelangen zum Backend, doch zuvor wird der gesamte übergebene Baum validiert und kopiert; große, nicht benötigte Assets zählen somit ebenfalls gegen die Eingabegrenzen. Ein kompaktes Verzeichnis mit ausschließlich den relevanten Smali-Wurzelverzeichnissen reduziert den Aufwand.

Split-APKs sind getrennte Eingaben. Jedes APK mit DEX-Inhalt kann einzeln verarbeitet werden, dieser Befehl führt jedoch keinen APK-Satz zusammen. Reine Ressourcen-Splits scheitern, weil sie kein DEX im Archivwurzelverzeichnis enthalten. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat` und `.vdex` werden nicht als mobile Eingaben akzeptiert. Dass das Backend einige dieser Formate unterstützt, bedeutet nicht, dass sie von diesem NeverD-Befehl unterstützt werden.

APK-Ressourcen, `AndroidManifest.xml`, Assets, JNI-/native Bibliotheken und zur Laufzeit heruntergeladener Code werden nicht als Java rekonstruiert. Extrahieren Sie eine native `.so` separat und verwenden Sie `neverd decompile library.so -o library.c`. Verschlüsselte oder gepackte Nutzlasten müssen für diesen statischen Ablauf bereits als gewöhnliches DEX/Smali vorliegen; Entpacken von Schutzschichten, Anbindung an ein Gerät und Umgehung von Schutzmechanismen sind nicht Teil des Ablaufs.

## Optionen und Vorrang

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --timeout=600 --max-files=30000 --max-bytes=4294967296 --json
```

| Option | Standard | Bedeutung |
|--------|----------|-----------|
| `-o DIRECTORY` | Erforderlich | Neues Ausgabeverzeichnis außerhalb einer Verzeichniseingabe; bestehende Ausgabe nie überschreiben |
| `--platform=auto\|android` | `auto` | Android ausdrücklich auswählen oder die Plattform aus der Eingabe ableiten |
| `--jadx PATH` | Nicht gesetzt: integrierte Engine | Den separat installierten JADX-Kompatibilitätsadapter ausdrücklich wählen; keine Auswahl über die Umgebung und kein automatischer Rückgriff |
| `--python PATH` | Umgebung/PATH | Interpreter für den mitgelieferten Helper; eine ausdrückliche Option hat Vorrang |
| `--timeout N` | `300` | Positives Zeitbudget für die integrierte Analyse; bei externen Backends Sekundenlimit je Prozess einschließlich Versionsabfrage |
| `--max-files N` | `20000` | Positive Grenze für Einträge, einschließlich angelegter Verzeichnisse |
| `--max-bytes N` | `2147483648` | Positive Bytegrenze für Eingabe, extrahierte Daten und endgültige Ausgabe |
| `--json` | Aus | Den Bericht als JSON statt als Zusammenfassung für Menschen ausgeben |

Eine vom Standard abweichende `--arch`-Auswahl, `--artifact`, `--metadata-only` und ein von null verschiedenes `--max-func` gehören zu iOS und werden für Android abgelehnt; explizites `--arch=auto` ist zulässig. Beliebige Backend-Optionen werden nicht durchgereicht. Der ausdrücklich gewählte JADX-Adapter isoliert Konfigurations-, Cache- und temporäre Verzeichnisse und übernimmt keine bestehenden Backend- oder Plugin-Einstellungen.

Für Eingaben, entpackte Daten und endgültige Ausgaben gelten weiterhin die Datei- und Bytebudgets. Die integrierten Leser und der Generator prüfen zusätzlich Arbeitsaufwand und verstrichene Zeit. Externe Arbeitsbereiche dürfen für Eingaben und Zwischenergebnisse bis zum Dreifachen der festgelegten Eintrags- und Bytebudgets belegen; Logs sind auf 16 MiB je Prozess begrenzt. Dies sind Ressourcenkontrollen und keine Sandbox. Das Erhöhen eines Limits schaltet andere Grenzen nicht ab.

## Ausgabestruktur und JSON-Bericht

```text
recovered-app/
  sources/                       wiederhergestellte Java-Pakete und Klassen
  metadata/android-methods.json  Methodenabdeckung der integrierten Engine
  report.json                    versioniertes Inventar und Grenzen
```

Temporäre Eingaben werden entfernt. Verschachtelte Klassen können die Quelldatei ihrer äußeren Klasse teilen; die Anzahl der Java-Dateien entspricht daher nicht der DEX-Klassenanzahl. Generierte Methoden können eine Java-Verteilerschleife verwenden. Sie führen weder die ursprüngliche DEX aus noch rufen sie diese über eine Laufzeitbrücke auf.

Der integrierte Bericht enthält `android_method_recovery`; derselbe Inhalt steht in `metadata/android-methods.json`. Vor der Veröffentlichung muss `method_count = recovered_method_count + declaration_only_method_count` gelten und `unrecovered_method_count` null sein. Ursprüngliche `native`- und `abstract`-Methoden erhalten den Status `declaration-only` und zählen nicht als wiederhergestellte Rümpfe. Es folgt ein gekürztes Beispiel; die Abdeckungsdatei enthält auch das Inventar je Methode:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {
    "name": "neverd",
    "version": "1",
    "execution": "builtin"
  },
  "input_code_files": [
    "classes.dex",
    "classes2.dex"
  ],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": [
    "sources/example/Main.java",
    "sources/example/Peer.java"
  ],
  "logs": [],
  "android_method_recovery": {
    "schema_version": 1,
    "status": "recovered",
    "class_count": 2,
    "method_count": 6,
    "recovered_method_count": 5,
    "declaration_only_method_count": 1,
    "unrecovered_method_count": 0
  }
}
```

`input_kind` ist `apk`, `dex`, `smali` oder `smali-directory`. `input_code_files` enthält Namen der eingegebenen Bytecode-Dateien oder Smali-Pfade, während `java_sources` und `logs` relativ zum Ausgabewurzelverzeichnis angegeben werden. `source` ist der Basisname der Eingabe. Tatsächliche Berichte enthalten weitere Rekonstruktionsgrenzen; behalten Sie diese bei, wenn Sie Ergebnisse an andere Werkzeuge weitergeben.

Prüfen Sie bei automatisierter Verarbeitung den Exit-Code des Prozesses, bevor Sie `status` auswerten. Wenn Sie stdout umleiten, speichern Sie den Bericht außerhalb des neuen Ausgabeverzeichnisses:

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

Erfolgreiche Helper-Läufe geben null zurück. Rekonstruktionsfehler liefern einen Wert ungleich null; sobald die Fehlerbehandlung des Helpers aktiv ist, erzeugt `--json` ein Fehlerobjekt mit `schema_version`, `status: "error"` und `error`. Native Argumentprüfung, fehlendes Python, eine Python-Version unter 3.10 oder ein fehlender Helper können schon vorher scheitern und statt JSON auf stderr schreiben. Auch ein Abbruch kann auf stderr gemeldet werden. Aufrufende Werkzeuge müssen diese Fälle berücksichtigen.

## Fehlerbehandlung und Problembehebung

Die Veröffentlichung ist transaktional: Vorhandene Ausgaben bleiben erhalten, fehlgeschlagene temporäre Ergebnisse werden entfernt. Nicht unterstützte Operationen, ungeklärte Registerflüsse, nicht darstellbare Deklarationen, fehlerhafte Ausnahmebehandlung und ausgeschöpfte Budgets lassen die integrierte Ausführung scheitern, statt fehlende Methodenrümpfe zu veröffentlichen. Der externe Adapter lehnt außerdem Fehlercodes, protokollierte Assemblierungs- oder Dekompilierungsfehler, ausgelassene doppelte Klassen, Markierungen unvollständigen Codes, leere Java-Dateien und fehlende Java-Ausgaben ab. Eine erfolgreiche Wiederherstellung beweist keine semantische Gleichheit.

| Symptom | Maßnahme |
|---------|----------|
| Python oder Hilfsprogramm fehlt | Python 3.10+ wählen und das benachbarte `mobile/`-Verzeichnis beibehalten |
| DEX, Anweisung, Deklaration oder Initialisierung nicht unterstützt | Diagnose und unterstützten Umfang prüfen. `--jadx PATH` nur bei bewusster Wahl des separaten Kompatibilitätsadapters verwenden |
| Ungültige Eingabe oder doppelte Klasse | Bytecode oder Klassenmenge korrigieren; nicht unterstützte Rümpfe werden nicht stillschweigend ausgelassen |
| Zeit- oder Arbeitsbudget überschritten | Eingabe verkleinern oder `--timeout`, `--max-files` und `--max-bytes` an verfügbare Ressourcen anpassen |
| Ausgabe bereits vorhanden | Neues Ausgabeverzeichnis wählen |

## Optionaler JADX-Kompatibilitätsadapter

`--jadx PATH` wählt das externe JADX und nicht die integrierte Implementierung. Installieren Sie JADX ab 1.5.6 mit den standardmäßigen DEX/smali-Eingabeplugins sowie Java ab 11. Verwenden Sie die vollständige [JADX-Distribution](https://github.com/skylot/jadx/releases/tag/v1.5.6), behalten Sie die Struktur von `bin/` und `lib/` sowie bei Weitergabe die enthaltenen Lizenzen der Abhängigkeiten bei. Nichts wird automatisch heruntergeladen. Der Adapterbericht nennt die tatsächliche Engine `jadx` und deren erkannte Version; er beansprucht keine Methodenabdeckung der integrierten Engine.

Unter Windows kann der `.bat`/`.cmd`-Starter der Distribution oder `lib/jadx-*-all.jar` angegeben werden. NeverD ermittelt das JAR und startet Java direkt; Anwendungspfade gelangen nicht in eine Befehls-Shell. `JAVA_HOME` oder PATH bestimmt Java. Erfolgreiche Adapterläufe behalten `logs/jadx-version.log` und `logs/jadx.log`; fehlgeschlagene temporäre Verzeichnisse samt Logs werden entfernt. Nur ein Backend-Fehlercode fügt ein begrenztes Log-Ende an die Fehlermeldung an. Startfehler, Zeitüberschreitungen und Budgetverletzungen haben eigene Diagnosen.

```sh
neverd mobile app.apk -o recovered-jadx --jadx /opt/jadx/bin/jadx
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

## Verifikation und Supporttiefe

```sh
cmake --build build --target check-neverd-mobile
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
```

Komponenten- und CLI-Tests prüfen das Parsen, Ausgabeverträge und die Bereinigung bei Fehlern. Der interne Ausführungsvergleich benötigt ein JDK (`java` und `javac`) und D8, um unabhängige DEX/APK-Beispiele zu erstellen und wiederhergestelltes Java zu kompilieren und auszuführen. Dies sind Testabhängigkeiten, keine Laufzeitvoraussetzungen der integrierten Wiederherstellung. Führen Sie ihn mit dem aktuellen Build aus und prüfen Sie die Ergebnisse, bevor Sie einen Fall als verifiziert bezeichnen. Der separate Kompatibilitätstest benötigt zusätzlich JADX und prüft diesen Adapter. Erfolgreiche Beispiele belegen keine vollständige Wiederherstellung beliebiger Anwendungen.

Der zugehörige iOS-Ablauf steht in der [Mobilübersicht](../mobile.md).
