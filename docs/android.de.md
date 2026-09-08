**Sprachen**: [English](android.md) | [简体中文](android.zh-CN.md) | [繁體中文](android.zh-TW.md) | [日本語](android.ja.md) | [한국어](android.ko.md) | [Français](android.fr.md) | [Deutsch](android.de.md) | [Español](android.es.md) | [Italiano](android.it.md) | [Русский](android.ru.md) | [العربية](android.ar.md)

# Java-Rekonstruktion für Android

[← Dokumentationsübersicht](README.de.md)

`neverd mobile` rekonstruiert lesbares Java aus APK-, DEX- und Smali-Eingaben mithilfe eines separat installierten JADX-Backends. Der Ablauf validiert die Bytecode-Eingaben, legt Arbeitskopien an, analysiert zusammengehörige Klassen gemeinsam, prüft die erzeugte Ausgabe und veröffentlicht ein Quellverzeichnis mit einem maschinenlesbaren Bericht. Dies ist eine experimentelle CLI-Funktion. APK-Container und Java-Ausgabe sind weder über das native C-SDK, das Python-Plugin-SDK, den GUI-Loader noch über `neverd decompile --language` verfügbar.

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
| Java-Backend | JADX 1.5.6 oder neuer mit den standardmäßigen DEX- und Smali-Eingabeplugins | `--jadx`, danach `NEVERD_JADX`, danach `jadx` im PATH |
| Java-Laufzeit | Java 11 oder neuer; für die Prüfung durch Kompilieren und Ausführen ist ein JDK erforderlich | `JAVA_HOME` oder Java im PATH |

NeverD lädt Abhängigkeiten nicht automatisch herunter. Beschaffen Sie die vollständige [JADX-Distribution](https://github.com/skylot/jadx/releases/tag/v1.5.6), behalten Sie deren Struktur mit `bin/` und `lib/` bei und bewahren Sie bei einer Weitergabe die enthaltenen Lizenzen. Getestet wurde Backend-Version 1.5.6; spätere Versionen müssen denselben CLI-Vertrag erfüllen. Das Einrichten der Abhängigkeiten ist vom Bau der LLVM-Pipeline von NeverD getrennt.

### Linux und macOS

```sh
cmake --build build --target neverd
python3 --version
java -version
/opt/jadx/bin/jadx --version

./build/bin/neverd mobile app.apk -o recovered-app \
  --python python3 --jadx /opt/jadx/bin/jadx
```

Für die wiederholte Nutzung können Sie `NEVERD_JADX=/opt/jadx/bin/jadx` setzen und optional mit `NEVERD_PYTHON` einen Interpreterpfad angeben. Falls Java noch nicht verfügbar ist, muss `JAVA_HOME` auf das Installationsverzeichnis des JDK zeigen. Pfade mit Leerzeichen müssen in Anführungszeichen stehen.

### Windows PowerShell

```powershell
$env:JAVA_HOME = 'C:\Tools\jdk'
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app `
  --python 'C:\Tools\Python\python.exe' `
  --jadx 'C:\Tools\jadx\bin\jadx.bat'
```

Aus dem `.bat`-/`.cmd`-Pfad des Backends wird die einzige Datei `lib/jadx-*-all.jar` der Distribution ermittelt; NeverD ruft Java direkt auf. Sie können dieses JAR auch direkt an `--jadx` übergeben. Anwendungspfade werden nie in einen Shell-Befehl eingesetzt. Bei Builds mit mehreren Konfigurationen kann die ausführbare Datei stattdessen unter `build/bin/Release/` liegen. Wird nur die ausführbare Datei ohne ihr Verzeichnis `mobile/` verschoben, meldet NeverD einen fehlenden Helper.

## Unterstützte Eingaben und Grenzen

| Eingabe | Verhalten | Wichtige Grenze |
|---------|-----------|-----------------|
| `.apk` | Das gesamte ZIP validieren und anschließend alle `classes.dex`, `classes2.dex` und weiteren nummerierten DEX-Dateien im Archivwurzelverzeichnis gemeinsam analysieren | Nur Code; keine Dekodierung von Ressourcen oder Manifest |
| `.dex` | DEX-Magic prüfen und den Inhalt durch das Backend dekodieren lassen | Eine umbenannte oder abgeschnittene Datei ist kein gültiger Bytecode |
| `.smali` | Die übergebene Klasse analysieren | Referenzierte benachbarte Klassen werden nicht implizit geladen |
| Smali-Verzeichnis | `.smali`-Dateien rekursiv sammeln und gemeinsam analysieren | Verschachtelte Klassen und abhängige Smali-Wurzelverzeichnisse in das Eingabeverzeichnis aufnehmen |

Wenn ein dekodierter APK-Verzeichnisbaum sowohl `smali/` als auch `smali_classes2/` enthält, übergeben Sie deren gemeinsames übergeordnetes Verzeichnis. Nur `.smali`-Dateien gelangen zum Backend, doch zuvor wird der gesamte übergebene Baum validiert und kopiert; große, nicht benötigte Assets zählen somit ebenfalls gegen die Eingabegrenzen. Ein kompaktes Verzeichnis mit ausschließlich den relevanten Smali-Wurzelverzeichnissen reduziert den Aufwand.

Split-APKs sind getrennte Eingaben. Jedes APK mit DEX-Inhalt kann einzeln verarbeitet werden, dieser Befehl führt jedoch keinen APK-Satz zusammen. Reine Ressourcen-Splits scheitern, weil sie kein DEX im Archivwurzelverzeichnis enthalten. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat` und `.vdex` werden nicht als mobile Eingaben akzeptiert. Dass das Backend einige dieser Formate unterstützt, bedeutet nicht, dass sie von diesem NeverD-Befehl unterstützt werden.

APK-Ressourcen, `AndroidManifest.xml`, Assets, JNI-/native Bibliotheken und zur Laufzeit heruntergeladener Code werden nicht als Java rekonstruiert. Extrahieren Sie eine native `.so` separat und verwenden Sie `neverd decompile library.so -o library.c`. Verschlüsselte oder gepackte Nutzlasten müssen für diesen statischen Ablauf bereits als gewöhnliches DEX/Smali vorliegen; Entpacken von Schutzschichten, Anbindung an ein Gerät und Umgehung von Schutzmechanismen sind nicht Teil des Ablaufs.

## Optionen und Vorrang

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --jadx /opt/jadx/bin/jadx --timeout=600 \
  --max-files=30000 --max-bytes=4294967296 --json
```

| Option | Standard | Bedeutung |
|--------|----------|-----------|
| `-o DIRECTORY` | Erforderlich | Neues Ausgabeverzeichnis außerhalb einer Verzeichniseingabe; bestehende Ausgabe nie überschreiben |
| `--platform=auto\|android` | `auto` | Android ausdrücklich auswählen oder die Plattform aus der Eingabe ableiten |
| `--jadx PATH` | Umgebung/PATH | Backend-Starter oder Distributions-JAR; eine ausdrückliche Option hat Vorrang |
| `--python PATH` | Umgebung/PATH | Interpreter für den mitgelieferten Helper; eine ausdrückliche Option hat Vorrang |
| `--timeout N` | `300` | Positive Anzahl Sekunden je Backend-Prozess, einschließlich der Versionsabfrage |
| `--max-files N` | `20000` | Positive Grenze für Einträge, einschließlich angelegter Verzeichnisse |
| `--max-bytes N` | `2147483648` | Positive Bytegrenze für Eingabe, extrahierte Daten und endgültige Ausgabe |
| `--json` | Aus | Den Bericht als JSON statt als Zusammenfassung für Menschen ausgeben |

Ein vom Standard abweichendes `--arch`, `--artifact`, `--metadata-only` und ein von null abweichendes `--max-func` gehören zu iOS und werden für Android abgewiesen; ein ausdrücklich angegebenes `--arch=auto` ist zulässig. Es gibt keine beliebige Durchleitung von Backend-Optionen. Konfigurations-, Cache- und temporäre Verzeichnisse des Backends werden für jeden Lauf isoliert; vorhandene Backend-Einstellungen und Plugin-Konfigurationen werden nicht in den Lauf übernommen.

Die Grenzen steuern den Ressourcenverbrauch; sie sind keine Sandbox für den Backend-Prozess. Auch das Arbeitsverzeichnis wird überwacht. Es bietet Platz für Eingabe, extrahierte Daten und Ausgabe bis zum Dreifachen der konfigurierten Eintrags- und Bytebudgets. Logs sind auf 16 MiB je Prozess begrenzt. Große Eingaben benötigen möglicherweise trotzdem mehr Java-Heap oder ein längeres Timeout; das Erhöhen einer Grenze hebt die anderen nicht auf.

## Ausgabestruktur und JSON-Bericht

```text
recovered-app/
  sources/                 rekonstruierte Java-Pakete und -Klassen
  logs/jadx-version.log    Versionsabfrage des Backends
  logs/jadx.log            Backend-Diagnosen
  report.json              versioniertes Inventar und Rekonstruktionsgrenzen
```

Temporäre Kopien und Backend-Caches werden entfernt. Die genauen Java-Dateinamen und ihre Anzahl hängen von der Rekonstruktion durch das Backend ab; verschachtelte Klassen können dieselbe Quelldatei wie ihre äußere Klasse nutzen. Die Anzahl der Java-Quelldateien entspricht deshalb nicht der Anzahl der DEX-Klassen.

Ein gekürzter Beispielbericht:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {"name": "jadx", "version": "1.5.6"},
  "input_code_files": ["classes.dex", "classes2.dex"],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": ["sources/example/Main.java", "sources/example/Peer.java"],
  "logs": ["logs/jadx-version.log", "logs/jadx.log"],
  "limitations": ["Java is reconstructed from bytecode; original comments, formatting, and stripped names cannot be restored."]
}
```

`input_kind` ist `apk`, `dex`, `smali` oder `smali-directory`. `input_code_files` enthält Namen der eingegebenen Bytecode-Dateien oder Smali-Pfade, während `java_sources` und `logs` relativ zum Ausgabewurzelverzeichnis angegeben werden. `source` ist der Basisname der Eingabe. Tatsächliche Berichte enthalten weitere Rekonstruktionsgrenzen; behalten Sie diese bei, wenn Sie Ergebnisse an andere Werkzeuge weitergeben.

Prüfen Sie bei automatisierter Verarbeitung den Exit-Code des Prozesses, bevor Sie `status` auswerten. Wenn Sie stdout umleiten, speichern Sie den Bericht außerhalb des neuen Ausgabeverzeichnisses:

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

Erfolgreiche Helper-Läufe geben null zurück. Rekonstruktionsfehler liefern einen Wert ungleich null; sobald die Fehlerbehandlung des Helpers aktiv ist, erzeugt `--json` ein Fehlerobjekt mit `schema_version`, `status: "error"` und `error`. Native Argumentprüfung, fehlendes Python, eine Python-Version unter 3.10 oder ein fehlender Helper können schon vorher scheitern und statt JSON auf stderr schreiben. Auch ein Abbruch kann auf stderr gemeldet werden. Aufrufende Werkzeuge müssen diese Fälle berücksichtigen.

## Fehlerbehandlung und Problembehebung

Die Veröffentlichung erfolgt transaktional: Bestehende Ausgabe bleibt erhalten, und Arbeitsdaten fehlgeschlagener Läufe werden entfernt. Backend-Exit-Codes ungleich null, protokollierte Assemblierungs- oder Dekompilierungsfehler, wegen Duplikaten ausgelassene Klassen, ausdrückliche Markierungen unvollständigen Codes, leere Java-Dateien und Ergebnisse ohne Java lassen den Befehl fehlschlagen. Eine Erfolgsmeldung des Backends beweist für sich genommen keine Korrektheit auf Methodenebene.

| Symptom | Maßnahme |
|---------|----------|
| Python/Helper fehlt | Python 3.10+ installieren oder auswählen und das danebenliegende Verzeichnis `mobile/` zusammen mit NeverD aufbewahren |
| Backend lässt sich nicht ausführen oder Version wird nicht unterstützt | `--jadx`, die vollständige Distributionsstruktur, Java und die Mindestversion des Backends prüfen |
| Ungültiger DEX-Header / kein DEX im Archivwurzelverzeichnis | Tatsächliches Eingabeformat prüfen; ein APK mit Code, gewöhnliches DEX oder Smali verwenden |
| Keine Smali-Dateien | Ein Verzeichnis mit `.smali`-Dateien angeben, keinen Java-Quellcode und keinen reinen Asset-Baum |
| Doppelte Klasse oder unvollständige Rekonstruktion | Doppelte Eingabedefinitionen entfernen oder den relevanten Bytecode-Satz getrennt analysieren; fehlerhaftes Smali korrigieren, statt ein unvollständiges Ergebnis zu akzeptieren |
| Timeout / Byte- oder Eintragsgrenze | Eine kleinere relevante Eingabe nutzen oder gezielt die entsprechende Grenze erhöhen |
| Unsicherer Archivpfad oder Link | Eine reguläre, portable Eingabe ohne Traversal-Namen, Links, Spezialdateien oder widersprüchliche Pfade neu erstellen |
| Ausgabe existiert bereits | Ein anderes Ausgabeverzeichnis wählen; das Verzeichnis eines früheren erfolgreichen Laufs nicht wiederverwenden |

Erfolgreiche Läufe behalten die Backend-Logs. Arbeitsverzeichnisse fehlgeschlagener Läufe werden einschließlich ihrer Logs gelöscht. Bei einem Backend-Exit-Code ungleich null enthält die Fehlermeldung einen begrenzten Ausschnitt vom Ende der Diagnosen; Timeouts und überschrittene Ressourcenbudgets werden mit eigenen Meldungen angegeben. Für eine backend-spezifische Untersuchung reproduzieren Sie das Problem mit einer isolierten Eingabe, der eigenen CLI des Backends und einem separaten Diagnoseverzeichnis. Leiten Sie niemals Erfolg allein daraus ab, dass vor einem Fehler bereits etwas Java erzeugt wurde.

## Verifikation und Supporttiefe

```sh
cmake --build build --target check-neverd-mobile
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

Die ersten beiden Befehle prüfen die mobilen Komponenten und die Verträge der gebauten CLI; plattformspezifische Anforderungen an Fixtures können zu ausdrücklich gemeldeten Überspringungen führen. Der Testlauf mit dem echten Backend benötigt zusätzlich ein JDK (`java` und `javac`). Er erstellt Fälle mit einzelnem Smali, klassenübergreifendem und verschachteltem Smali, DEX sowie echten Multidex-APKs und kompiliert und führt anschließend das rekonstruierte Java aus. Die Fälle decken Verzweigungen, Schleifen, Arrays, Ausnahmebehandlung, Klassenreferenzen, fehlerhafte Eingaben und wegen Duplikaten ausgelassene Klassen ab. Das belegt diese Fixtures, verspricht aber keine vollständige Rekonstruktion beliebiger Anwendungen.

Der [Workflow Mobile Decompilation](../.github/workflows/mobile.yml) führt Komponententests unter Linux, macOS und Windows mit Python 3.10 und 3.13 aus, außerdem unter Linux einen Lauf mit einem per Prüfsumme festgelegten echten Android-Backend. Die [mobile Übersicht](mobile.md) beschreibt den getrennten iOS-Ablauf und seine aktuellen Grenzen.
