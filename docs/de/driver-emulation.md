**Sprachen**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← Dokumentationsindex](README.md)

# Emulation von Windows-Treibern

NeverDs optionaler Treiberemulator führt den PE-Einsprungpunkt eines unterstützten
x64-WDM-Treibers aus und kann vor dem Entladen ein ausdrücklich angegebenes
Szenario synchroner Anforderungen durchlaufen. Für die CPU-Ausführung verwendet
er Unicorn, für die begrenzte Windows-Umgebung NeverDs eigenes Modell. Er lädt
den Treiber nicht in den Host-Kernel und leitet Gast-API-Aufrufe nicht an
Betriebssystemdienste des Hosts weiter.

## Bauen und ausführen

Die Funktion muss ausdrücklich aktiviert werden und ist unabhängig von `BUILD_TESTING`:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON
cmake --build build-release --target neverd --parallel 4
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --instruction-limit 100000 > driver-report.json
```

Der Bericht erscheint immer als JSON auf stdout; Diagnosemeldungen zu Anforderungen
und Einrichtung gehen an stderr. Die Standardgrenzen sind 100000 Gastinstruktionen,
64 MiB Gastspeicher, 10000 aufgezeichnete Ereignisse und 5000 Millisekunden.
Das Instruktionslimit muss positiv sein. Ein erschöpftes Budget beendet die
Ausführung und erhält die bis dahin gesammelten Beobachtungen.

| Exitcode | Bedeutung |
|----------|-----------|
| `0` | Initialisierung und jede angeforderte, abgeschlossene Operation waren erfolgreich |
| `1` | Ungültige Eingabe/Optionen, Einrichtungsfehler oder beim Build deaktivierte Funktion |
| `2` | Initialisierung oder eine abgeschlossene Anforderung lieferte einen fehlerhaften `NTSTATUS` |
| `3` | Ausführung endete vor Abschluss des Szenarios, etwa wegen einer nicht unterstützten API, eines Speicherfehlers oder einer Budgetgrenze |

Ein zurückgegebener Fehlerstatus ist eine abgeschlossene Beobachtung dieser
Operation. Eine erfolgreiche Rückkehr beschreibt nur diesen modellierten Lauf;
sie belegt nicht, dass der Treiber unter Windows funktioniert.

## Treiberkompatibilität

Die Kompatibilität hängt vom ausgeführten Codepfad und seinen Abhängigkeiten ab,
nicht von der Dateiendung `.sys`. Die bisherigen Abnahmen decken selbst entwickelte,
freistehende Fixtures und den gepufferten Pfad von Microsofts WDM-Beispiel SIOCTL
ab. Sie belegen keine Kompatibilität mit beliebigen Treibern Dritter.

| Treiberklasse oder Anforderung | Aktueller Umfang | Fehlende Umgebung |
|-------------------------------|------------------|-------------------|
| x64-Software-WDM-Treiber mit den aufgeführten APIs | Begrenzte Initialisierung und ein synchroner Dateilebenszyklus | Jede weitere ausgeführte API benötigt ein definiertes Modell |
| `METHOD_BUFFERED`-IOCTL | Im ausdrücklich angegebenen Anforderungsszenario unterstützt | Mehrere offene Dateien und asynchroner Abschluss sind nicht verfügbar |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT`, `METHOD_NEITHER` | Abgewiesen | MDLs, gesperrte Seiten, Zugriffsprüfung und Lebensdauer von Benutzerpuffern |
| KMDF-/UMDF-Treiber | Nicht unterstützt | Framework-Anbindung, Objekte, Warteschlangen, Callbacks und passende Host-Laufzeit |
| PnP-Bus-, Funktions- oder Filtertreiber | Initialisierung kann innerhalb der API-Teilmenge laufen; Gerätestapel-Lebenszyklus nicht unterstützt | Geräteanbindung, Dispatch an untergeordnete Treiber sowie PnP- und Power-IRPs |
| Speicher-, Netzwerk-, Anzeige-, Dateisystem- und Minifiltertreiber | Subsystemverträge nicht unterstützt | Port-/Klassen-/Miniport-Frameworks, NDIS/WFP, Grafik- oder Dateisystemdienste |
| Treiber mit Worker-Threads, Timern, DPCs, APCs, Warteoperationen oder Abbruch | Nicht unterstützt | Scheduling, IRQL-Wechsel, Synchronisation und asynchrone Zuständigkeit |
| Treiber mit Prozess-/Thread-Callbacks, Handles, Registry-/Dateioperationen oder Kernel-Modulsuche | Außerhalb der aufgeführten APIs nicht unterstützt | Objektmanager, Systemzustand und Erzeuger von Callbacks/Ereignissen |
| Hardware-, DMA-, PCI-, Interrupt- oder Virtualisierungstreiber | Umgebung nicht unterstützt | Gerätemodelle, physischer Speicher, Busse, Interrupts und privilegierter CPU-Zustand |
| x86- oder ARM64-Windows-Treiber | Abgewiesen | Architekturspezifisches Laden, ABI und Ausführungsmodell |
| x64-Image mit CFG, nicht unterstützter Ladekonfiguration, TLS oder anderen abgewiesenen PE-Funktionen | Beim Laden abgewiesen | Explizite Loader-/Laufzeitsemantik für diese Anforderungen |

Ein ungenutzter, nicht unterstützter Import darf gebunden bleiben. Wird eine
nicht unterstützte Operation erreicht, stoppt die Ausführung mit einer Diagnose
und den bisherigen Beobachtungen. Ein erfolgreiches DriverEntry allein belegt
nicht die Unterstützung späterer Dispatch-, Hardware- oder Framework-Pfade.
Die folgende API-Tabelle definiert verbindlich die unterstützte Teilmenge.

## Ausführungsvertrag

Das Profil modelliert einen einzigen, einsträngigen x64-WDM-Lebenszyklus auf
`PASSIVE_LEVEL`. Die Ausführung beginnt am PE-Einsprungpunkt und behält einen
vorhandenen Compiler-Einstiegswrapper bei. DriverEntry muss zur Initialisierung
`STATUS_SUCCESS` zurückgeben; ein anderer erfolgreicher Status oder ein
Pending-Status beendet den Lauf als nicht unterstützter Initialisierungsvertrag.
Ein Fehlerstatus bleibt als abgeschlossenes Initialisierungsergebnis erhalten.
Alle Objekte, Zeichenfolgen, Stacks, Funktionszeiger und Allokationen liegen im
Gastspeicher. Das Modell stellt ein `DRIVER_OBJECT` und einen Registry-Pfad für
den konfigurierten Dienstnamen bereit (Standard: `NeverDDriver`).
Der Adapter verwendet Unicorns virtuellen TLB-Modus, um virtuelle Gastadressen
einschließlich kanonischer hoher Kernel-Adressen ohne künstliche Windows-
Seitentabellen zu erhalten. Der anfängliche RFLAGS-Wert ist `0x202`; das
Softwaregeräteprofil verwendet eine feste Cachezeilengröße von 64 Byte.
Dies sind ausdrückliche Eigenschaften dieses Ausführungsszenarios.

Unbekannte Importe werden an erst bei Nutzung auslösende Traps gebunden. Ein
ungenutzter Import verhindert den Lauf nicht; die Ausführung seines Thunks oder
das Lesen eines nicht modellierten exportierten Datenwerts stoppt mit
`unsupported_api`. Nicht unterstützte CPU-Umgebungseffekte stoppen ebenfalls
ausdrücklich. NeverD ersetzt nicht implementierte Aufrufe nicht durch
Erfolgswerte. Fehlerhafte Images oder nicht unterstützte Ladeanforderungen
scheitern vor Beginn der Ausführung.

Dieses Profil implementiert keinen vollständigen Windows-Kernel, keine
KMDF-Laufzeit, keinen PnP-/Power-Lebenszyklus, keine asynchronen oder ausstehenden
IRPs, keine Direct-/Neither-IOCTLs, keine Interrupts und kein Multithread-Scheduling.
Callbacks laufen nur auf ausdrückliche Anforderung des Szenarios; reine
Initialisierung endet weiterhin nach DriverEntry.

Images verwenden ihre bevorzugte Basisadresse, sofern das Szenario keine gültige
Relokationsadresse auswählt, und müssen ausführbare PE32+-x64-Dateien mit nativem
Subsystem sein. Importe dürfen aus `ntoskrnl.exe` oder `ntkrnlmp.exe` stammen.
Der Ausführungsloader unterstützt validierte x64-`DIR64`-Basisrelokationen und
eine begrenzte Security-Cookie-Ladekonfiguration. Der deterministische Gast-Cookie
wird vor dem Einstiegswrapper initialisiert. CFG und andere nicht modellierte
Ladekonfigurationsfelder, TLS, verzögerte/gebundene Importe, Ordinalimporte und
verwaltete Images werden abgewiesen. Images müssen außerdem strikte Bereichs-
und Ausrichtungsprüfungen bestehen.

Das anfängliche API-Modell besitzt bewusst einen begrenzten Vertrag:

| APIs | Modelliertes Verhalten und Einschränkungen |
|------|-------------------------------------------|
| `RtlInitUnicodeString` | Erstellt eine Gast-`UNICODE_STRING` für eine begrenzte NUL-terminierte Quelle |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Datenallokationen für Pooltypen `0`, `1` und `512`; positive Größe/Tag, passende Tags beim Freigeben, keine Wiederverwendung von Adressen |
| `IoCreateDevice`, `IoDeleteDevice` | Gerätetyp `0x22`, Characteristics `0` oder `0x100`, begrenzte Erweiterungen, ASCII-Namen der Form `\Device\Name` |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` oder `\??\Name` in einem Sitzungsnamensraum, Ziel `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | ASCII-Literaltext und `%%`, höchstens 512 Ausgabebytes; variadische Formatierung stoppt; alle Debuggerfilter aktiviert |
| `IoGetCurrentIrpStackLocation` | Gibt die Stackposition des aktiven modellierten IRP zurück; normale kompilierte WDM-Makros lesen dasselbe Gastfeld |
| `KeGetCurrentIrql` | Gibt `PASSIVE_LEVEL` zurück |
| `IofCompleteRequest`, `IoCompleteRequest` | Schließt das aktive synchrone modellierte IRP mit `IO_NO_INCREMENT` ab; auf ein abgeschlossenes IRP oder seinen Puffer darf nicht erneut zugegriffen werden |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Begrenzte Gastpufferoperationen, höchstens 1 MiB pro Aufruf; Kopier-APIs ohne Überlappungsunterstützung weisen Überlappungen ab |

Der ursprüngliche RegistryPath-Datensatz und sein Puffer verlieren ihre Gültigkeit,
sobald DriverEntry zurückkehrt. Treiber, die die Zeichenfolge später benötigen,
müssen sie während der Initialisierung kopieren.

Die Objekt-/Pool-Arena umfasst 1 MiB. Nicht initialisierte Poolbytes enthalten
deterministisch `0xCD`, freigegebene Poolbytes `0xDD`. Dies beschreibt ein
konkretes Ausführungsszenario. CPU-Zugriffe und modellierte Puffer-APIs weisen
Zugriffe auf freigegebene Poolallokationen, gelöschte Geräte, nicht zugewiesene
Arena-Bytes und opake Objektfelder sowie Schreibzugriffe auf schreibgeschützte
Objektfelder ab. Diese Prüfungen erfassen die Objektlebensdauern dieses Modells;
sie sind keine allgemeine Speichersicherheitsanalyse für Treiber. Unbeschriebene
Dispatch-Tabelleneinträge melden null als „nicht registriert“. Eine
Szenarioanforderung für eine nicht registrierte Major Function wird vom
modellierten Standardhandler mit `STATUS_INVALID_DEVICE_REQUEST` abgeschlossen;
der Fehler bleibt sowohl im Dispatch- als auch im I/O-Status sichtbar. Das
Modell erfindet keine Gastfunktionsadresse für diesen Handler; Gastlesezugriffe
auf einen unbeschriebenen Eintrag bleiben nicht unterstützt. Das ausdrückliche
Registrieren eines Null-Callbacks ist ein Fehler.

## Anforderungsszenarien

Übergeben Sie mit `--scenario` eine JSON-Datei, um Anforderungen und optionales
Entladen auszuwählen:

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --scenario scenario.json > driver-report.json
```

Für einen Treiber, der `\Device\NeverDIO` erstellt und den gepufferten IOCTL
`0x222000` akzeptiert, sieht eine beispielhafte `scenario.json` so aus:

```json
{
  "requests": [
    {"kind": "create", "device": "\\Device\\NeverDIO"},
    {"kind": "ioctl", "code": "0x222000", "input": "00112233", "output_size": 4},
    {"kind": "cleanup"},
    {"kind": "close"}
  ],
  "unload": true
}
```

Gerätename und IOCTL-Code müssen zum Treiber passen. Ohne `device` wird das
einzige aktive Gerät ausgewählt; mehrdeutige Auswahl scheitert. Dieses Modell
verfolgt eine offene Datei und verlangt create, IOCTLs, cleanup und close in
dieser Reihenfolge. Nur `METHOD_BUFFERED`-IOCTLs werden unterstützt. Dispatch
muss jedes IRP synchron abschließen; `STATUS_PENDING`, fehlender Abschluss,
ungültige Ausgabelängen und Zugriffe auf bereits abgeschlossene IRPs führen
ausdrücklich zu Fehlern. Angefordertes Entladen darf keine aktiven Geräte,
symbolischen Links, Poolallokationen oder Dateiobjekte zurücklassen.

Das optionale Wurzelfeld `"load_address": "0x190000000"` fordert eine
Basisverschiebung an; ohne dieses Feld oder mit `"0x0"` gilt die bevorzugte
Adresse. Das Image muss die Relokationsanforderungen erfüllen. Der ursprüngliche
Initialisierungsbefehl und die C-API implizieren kein Szenario.

An der Wurzel sind nur `load_address`, `requests` und `unload` erlaubt.
Anforderungsfelder sind `kind`, optional `device` und ausschließlich für `ioctl`
das erforderliche `code` sowie optional `input` und `output_size`. Unbekannte
oder doppelte Felder werden abgewiesen. `code` akzeptiert eine vorzeichenlose
32-Bit-JSON-Ganzzahl oder eine Hexadezimalzeichenfolge mit `0x`. `input` ist eine
Hexadezimal-Bytezeichenfolge gerader Länge ohne Präfix oder Leerzeichen;
Weglassen bedeutet leere Eingabe. `output_size` ist eine vorzeichenlose
JSON-Ganzzahl; Weglassen bedeutet null. Brüche und Gleitkommaschreibweisen
werden abgewiesen.

Der Szenariotext ist auf 2 MiB begrenzt: höchstens 64 Anforderungen, höchstens
65536 Byte pro Eingabe- oder Ausgabepuffer und höchstens 512 KiB Eingabe- und
Ausgabebytes insgesamt. Instruktions-, Beobachtungs-, Gastspeicher- und Zeitbudgets
gelten für das gesamte Szenario. Die 1-MiB-Arena enthält auch Objekte und
Metadaten; daher kann ein Image den Modellspeicher erschöpfen, bevor die
maximalen Szenariopuffer verbraucht sind.

## Abnahme mit dem Microsoft-Beispiel

Das ausdrücklich auszuführende [Validierungsskript](../../scripts/validate_windows_driver_sample.py)
lädt Microsofts SIOCTL-Quelltext in der im
[Validierungsmanifest](../../unittests/emulation/fixtures/sioctl-validation.json)
festgelegten Revision herunter, prüft SHA-256-Hashes und kompiliert den
unveränderten Quelltext gegen MinGW-w64-DDK-Header. Es bewahrt Lizenz und
Herkunft des Originals, Build-Befehle, Szenario und Berichte im gewählten
Ausgabeverzeichnis auf. Es benötigt Netzwerkzugriff, Clang, `lld-link`, `nm`
und die DDK-Header von MinGW-w64:

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

Mit `--headers` lässt sich ein anderes MinGW-w64-Include-Verzeichnis angeben.
Das Skript erzeugt aus den Abhängigkeiten der kompilierten Objektdatei eine
MS-COFF-Importbibliothek. Die Prüfung führt DriverEntry, create, den gepufferten
IOCTL des Beispiels, cleanup, close und unload aus. Das Originalbeispiel
registriert keinen Cleanup-Handler; deshalb schließt der modellierte
Standardhandler cleanup mit `STATUS_INVALID_DEVICE_REQUEST` (`0xC0000010`) ab.
Der Treiber wird trotzdem geschlossen und entladen, und der erfolgreiche IOCTL
liefert die erwarteten Bytes. Für dieses vollständige Szenario beträgt der
erwartete CLI-Exitcode **2**, und `scenario_success` ist false. Das Skript selbst
ist nur erfolgreich, wenn alle diese Ergebnisse einschließlich des sichtbaren
Cleanup-Fehlers übereinstimmen; es verändert das Beispiel nicht, um dieses
Ergebnis zu verbergen.

## Berichte und SDK

Der JSON-Bericht unterscheidet `stop_reason`, die nullable Felder `nt_status`
und `nt_success`, den PC beim Anhalten und die Instruktionszahl. Er erhält
die vor dem Stopp gesammelten API-Aufrufe und beobachtbaren Zustände,
einschließlich Geräteobjekten und Callback-Adressen des Treibers. Gastadressen
sind Hexadezimalzeichenfolgen, damit JSON-Verbraucher keine 64-Bit-Präzision
verlieren. Das Objekt `configuration` protokolliert Limits und Dienstnamen des
Laufs. Das Profil lautet `wdm-x64-synchronous-v1`. `nt_status` bleibt das
DriverEntry-Ergebnis, während `scenario_success` Initialisierung und
abgeschlossene Anforderungen gemeinsam beschreibt. `phase`, `requests` und
`unload_completed` kennzeichnen die ausgeführten Teile des angeforderten
Lebenszyklus. Jeder API-Aufruf und CPU-Schreibzugriff protokolliert auch seine
Phase (`driver_entry`, `request:N` oder `unload`). Jede Anforderung meldet
Dispatch- und I/O-Status, Abschluss, Informationslänge und zurückgegebene Bytes
in `output_hex`. `preferred_image_base` beschreibt die ursprüngliche PE-Basis.
`security_cookie` ist die Gastadresse des initialisierten Cookies oder `"0x0"`,
wenn keiner erforderlich war. Anforderungsfelder sind `kind`, `device`, `code`,
`irp`, `completed`, `dispatch_status`, `io_status`, `information` und `output_hex`.

`instructions` zählt zugelassene Gastinstruktionsversuche. Eine von der
Ausführungsrichtlinie abgewiesene Instruktion wird nicht gezählt; eine
zugelassene Instruktion, die in der CPU einen Fehler auslöst, wird gezählt.
Synthetischer API-Dispatch und der Rückkehr-Sentinel erhöhen den Zähler nicht.

Jeder `writes`-Eintrag enthält `semantics: "attempted_guest_write"`: Er erfasst
einen CPU-Schreibversuch außerhalb des Stacks, auch wenn dieser anschließend
einen Fehler auslöst oder durch ein Budget gestoppt wird. Er garantiert nicht
den Abschluss des Schreibens und enthält keine Schreibzugriffe der API-Modelle.
Snapshots von Geräte- und Treiberobjekten beschreiben den beim Stopp
beobachteten Zustand.

Binden Sie `neverd/sdk/NeverDCAPIEmulation.h` (oder den C-API-Sammelheader) ein,
erstellen Sie eine Sitzung und rufen Sie
`neverd_emulate_driver_json(session, path, options)` auf. Ein expliziter,
nicht leerer Pfad führt direkt zur strikten Ausführungsvorprüfung, ohne zuvor
über die allgemeine Analyse-API zu laden. Die CLI verwendet diesen Weg.
`NULL` für options wählt die Standardwerte. Explizite
`neverd_driver_options_v1`-Optionen benötigen die exakte `struct_size` und
positive Instruktions-, Speicher-, Ereignis- und Zeitbudgets. Geben Sie das
Ergebnis mit `neverd_free_string` frei.

`NULL` für den Pfad erfordert dagegen eine geladene Sitzung und parst deren
Datei unabhängig von IR-Analyse und funktionsbeschränktem Laden erneut. Beide
Wege erhalten das Sitzungsimage. Halten Sie die Eingabedatei während des Aufrufs
verfügbar und unverändert. Anforderungs-/Einrichtungsfehler geben `NULL` zurück
und setzen `neverd_last_error`; Ausführungsstopps liefern JSON. Die API bleibt
auch in Builds mit deaktivierter Funktion verfügbar und erklärt deren Aktivierung.

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)`
verwendet dieselben v1-Optionen und Eigentumsregeln und ergänzt eine strikte
Szenarioeingabe. Erforderlich ist eine nicht-NULL, NUL-terminierte JSON-Zeichenfolge.
Die ursprüngliche ABI `neverd_emulate_driver_json` bleibt unverändert und auf
Initialisierung beschränkt. Der C++-Parser `driverOptionsFromScenarioJSON`
stellt Aufrufern von `emulateDriver` dieselbe Szenariovalidierung bereit.

Der interne C++-Einsprungpunkt ist `neverd::emulation::emulateDriver` in
`include/neverd/emulation/DriverSession.h`. Formatparsing gehört zum vorhandenen
Loader, Windows-Objekt-/API-Verhalten zu `lib/emulation/windows`, CPU-Zustand
und Ausführung zum Unicorn-Adapter. Adapter und Modell verwenden dieselbe
Gastspeicherschnittstelle. Windows-API-Verhalten gehört nicht in den Unicorn-Fork.
