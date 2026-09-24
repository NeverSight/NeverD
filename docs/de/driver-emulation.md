**Sprachen**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← Dokumentationsindex](README.md)

# Emulation von Windows-Treibern

NeverDs optionaler Treiberemulator führt den PE-Einsprungpunkt eines unterstützten
x64-WDM-Treibers aus und kann vor dem Entladen ein ausdrücklich angegebenes
Szenario serieller Anforderungen durchlaufen. Für die CPU-Ausführung verwendet
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
Das Instruktionslimit muss positiv sein. Ein erschöpftes Ausführungsbudget beendet die Ausführung und erhält die bisherigen Beobachtungen. Einzelne Allokations-APIs verwenden jedoch ihre dokumentierten Rückgabewerte bei Platzmangel; MMIO-Mappingknappheit liefert beispielsweise NULL.

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
freistehende Fixtures sowie die gepufferten, In-Direct- und Out-Direct-Pfade von
Microsofts WDM-Beispiel SIOCTL einschließlich seines Builds mit Debugausgaben
sowie Pavel Yosifovichs öffentliches Beispiel Zero für direkte READ/WRITE und
Statistiken ab.
Sie belegen keine Kompatibilität mit beliebigen Treibern Dritter.

| Treiberklasse oder Anforderung | Aktueller Umfang | Fehlende Umgebung |
|-------------------------------|------------------|-------------------|
| x64-Software-WDM-Treiber mit den aufgeführten APIs | Begrenzte x64-WDM-Initialisierung, serielle buffered/direct Anforderungen, Work Items, Timer, DPCs, Ereignisse und Warten, Berichte und Grenzen | Jede weitere ausgeführte API benötigt ein definiertes Modell |
| `METHOD_BUFFERED`-IOCTL | Serielle buffered/direct E/A mit Abschluss durch Work Item oder DPC | Nur die unten genannten APIs; keine gleichzeitigen öffentlichen Szenarioeinreichungen oder allgemeine WDM-Anforderungsabbrüche |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | Anforderungseigene MDLs, Systemabbildungen, gemeinsame physische Seitenidentitäten und SG-DMA | Benutzerabbildungen und weitere DMA-Schnittstellen |
| Treiberzugewiesene MDLs | Eigenständige Deskriptoren für Nonpaged Pool oder eine Benutzerzuweisung mit gemeinsamen physischen Seiten | Keine IRP-Zuordnung, MDL-Ketten oder beliebigen Prozesse |
| READ/WRITE | Serielle buffered/direct/neither E/A mit Abschluss durch Work Item oder DPC | Nur die unten genannten APIs; keine gleichzeitigen Szenario-IRPs, allgemeine WDM-Anforderungsabbrüche oder implizite Dateiposition |
| WDM `METHOD_NEITHER` | Getrennte Benutzerpuffer, Zugriffsprüfung, MDL-Sperren, synthetische Prozesskennungen, Entzug von VA oder Prozessende nach Dispatch und begrenzter Abbruch | Kein allgemeines Prozess-Attachment und keine beliebigen Benutzerabbildungen oder Wiederabbildungen |
| KMDF-1.33-Nicht-PnP-Treiber | Bindung, Objekte/Kontexte, benannte Steuergeräte, sequenzielle oder parallele Standardwarteschlangen mit endlichem oder unbegrenztem Limit sowie gepufferte/direkte/neither Anforderungen mit ausgeführten Callbacks | Keine PnP-Geräte, allgemeine Warteschlangenplanung, Klassenerweiterungen oder UMDF |
| PnP-Bus-, Funktions- oder Filtertreiber | Explizite PDOs ohne Ressourcen oder mit fester Registerbank, Gast-AddDevice und acht übliche PnP-Lebenszyklusfunktionen | Weitere PnP-Vorgänge, allgemeine Power-Policy, weitere Hardware-/Ressourcenmodelle und allgemeines KMDF-PnP |
| Speicher-, Netzwerk-, Anzeige-, Dateisystem- und Minifiltertreiber | Subsystemverträge nicht unterstützt | Port-/Klassen-/Miniport-Frameworks, NDIS/WFP, Grafik- oder Dateisystemdienste |
| Work Items, Timer, DPCs, Ereignisse und Warten | Der aktuelle IRQL ist bei Dispatch und Work Items `PASSIVE_LEVEL`, bei DPCs `DISPATCH_LEVEL` | Nur die unten genannten APIs; keine gleichzeitigen öffentlichen Szenarioeinreichungen oder allgemeine WDM-Anforderungsabbrüche |
| Registry-Operationen über die aufgeführten Zw-APIs | Expliziter Sitzungsbaum und Rechte pro Handle | ACLs, Privilegien, alternative Ansichten und Persistenz |
| Treiber mit Prozess-/Thread-Callbacks, anderen Handles, Dateioperationen oder Kernel-Modulsuche | Außerhalb der aufgeführten APIs nicht unterstützt | Objektmanager, Systemzustand und Erzeuger von Callbacks/Ereignissen |
| Hardware-, DMA-, PCI-, Interrupt- oder Virtualisierungstreiber | Explizite Speicherregisterbank, MMIO, exklusive Latched-Interruptcallbacks und begrenzte kohärente gemeinsame Puffer/SG-/Kanal-DMA unterstützt | Weitere Gerätemodelle, beliebiger physischer RAM, PCI, Ports, andere Interruptmodi, weitere DMA-Schnittstellen und privilegierter CPU-Zustand |
| x86- oder ARM64-Windows-Treiber | Abgewiesen | Architekturspezifisches Laden, ABI und Ausführungsmodell |
| x64-CFG | Validierte Zieltabellen und Check-/Dispatch-Aufrufe; inaktive Instrumentierung behält Gast-Fallbacks | XFG, Exportunterdrückung, unmodellierte Ladekonfiguration und TLS bleiben abgewiesen |

Ein ungenutzter, nicht unterstützter Import darf gebunden bleiben. Wird eine
nicht unterstützte Operation erreicht, stoppt die Ausführung mit einer Diagnose
und den bisherigen Beobachtungen. Ein erfolgreiches DriverEntry allein belegt
nicht die Unterstützung späterer Dispatch-, Hardware- oder Framework-Pfade.
Die folgende API-Tabelle definiert verbindlich die unterstützte Teilmenge.

## Ausführungsvertrag

Das Profil modelliert einen x64-WDM-Lebenszyklus auf CPU0 mit deterministischem kooperativem Scheduling. Die Ausführung beginnt am PE-Einsprungpunkt und behält einen
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
Inline-Lesezugriffe auf CR8 unter x64 sehen ebenfalls `PASSIVE_LEVEL` / `DISPATCH_LEVEL`;
CR8-Schreibzugriffe und andere Kontrollregisteroperationen bleiben nicht unterstützt.

Unbekannte Importe werden an erst bei Nutzung auslösende Traps gebunden. Ein
ungenutzter Import verhindert den Lauf nicht; die Ausführung seines Thunks oder
das Lesen eines nicht modellierten exportierten Datenwerts stoppt mit
`unsupported_api`. Nicht unterstützte CPU-Umgebungseffekte stoppen ebenfalls
ausdrücklich. NeverD ersetzt nicht implementierte Aufrufe nicht durch
Erfolgswerte. Fehlerhafte Images oder nicht unterstützte Ladeanforderungen
scheitern vor Beginn der Ausführung.

`DelayedWorkQueue`-Work-Items laufen auf `PASSIVE_LEVEL`, Gast-DPC-Callbacks mit den vier vorgesehenen Argumenten auf `DISPATCH_LEVEL`. CPU0 plant deterministisch und kooperativ an Aufrufrückkehr und blockierenden Warteoperationen. Relative, absolute und periodische Timer verwenden virtuelle Zeit; ohne ausführbaren Frame wird bis zur nächsten Timer-, Warte- oder Abbruchfrist fortgeschritten. Benachrichtigungs- und Synchronisationsereignisse/-timer behalten ihre unterschiedlichen Signalverbrauchsregeln. Jeder Callback besitzt einen eigenen Gaststack; mehrere blockierte Frames behalten lokale Variablen und vollständige CPU-Kontexte bei gemeinsamem Gastspeicher. Win64 übergibt die ersten vier Argumente in Registern, weitere auf dem Stack. Anforderungen bleiben seriell: Ein Dispatch mit Pending-Markierung muss `STATUS_PENDING` liefern und vor der nächsten Anforderung abschließen. Fehlt ein verfügbarer Erzeuger für eine ausstehende Anforderung oder unbegrenztes Warten, stoppt ein Stau mit `model_error`. Befehls-, Speicher-, Beobachtungs- und Echtzeitbudgets bleiben gemeinsam.

Das begrenzte Modell ist keine vollständige asynchrone Windows-Umgebung. Alertable-/User-Mode-Warten, APCs, allgemeiner WDM-Anforderungsabbruch, gleichzeitige öffentliche Szenarioeinreichungen, UMDF, allgemeines KMDF-PnP-Geräte und allgemeine Warteschlangenplanung, vollständiges PnP/Power, allgemeine Hardwaremodelle, weitere DMA-Schnittstellen und andere Interruptmodi bleiben unmodelliert. Reine Initialisierung führt explizit eingereihte Callbacks aus, erzeugt aber keine Anforderungen oder implizites Entladen.

Ein Work Item wird vor Beginn seines Callbacks aus der Warteschlange entfernt; der Callback darf sein eigenes Item freigeben. Freigabe eingereihter Items, doppelte Einreihung, veraltete Objekte und Callback-Ziele außerhalb ausführbaren Gastspeichers scheitern ausdrücklich. Die Gerätereferenz bleibt bis zur Rückkehr erhalten. Entladen verlangt die Freigabe aller Work Items und den Abschluss eingereihter Arbeit. CPU-Kontexte enthalten allgemeine Register, SIMD-, FPU- und Steuerzustand; Gastspeicher bleibt gemeinsam, und fehlerhafte CPUs lassen sich durch Kontextwiederherstellung nicht fortsetzen.
Löschen wird aufgeschoben, solange Dateiobjekte oder eingereihte/laufende Work-Item-Referenzen bestehen. Work-Item-Allokation liefert NULL bei erschöpfter Objektarena.

Images verwenden ihre bevorzugte Basisadresse, sofern das Szenario keine gültige
Relokationsadresse auswählt, und müssen ausführbare PE32+-x64-Dateien mit nativem
Subsystem sein. Importe dürfen aus `ntoskrnl.exe`, `ntkrnlmp.exe` oder `WDFLDR.SYS` stammen.
Der Ausführungsloader unterstützt validierte x64-`DIR64`-Basisrelokationen und
eine begrenzte Security-Cookie-Ladekonfiguration. Der deterministische Gast-Cookie
wird vor dem Einstiegswrapper initialisiert. Andere nicht modellierte
Ladekonfigurationsfelder, TLS, verzögerte/gebundene Importe, Ordinalimporte und
verwaltete Images werden abgewiesen. Images müssen außerdem strikte Bereichs-
und Ausrichtungsprüfungen bestehen.

Aktives Control Flow Guard (CFG) validiert PE-Flags, Zeigerplätze und sortierte ausführbare Ziele. Check-/Dispatch-Helfer erlauben nur deklarierte Image-Einstiege oder registrierte API-Thunks, erhalten den Win64-Aufrufzustand und weisen andere Ziele ab. Instrumentierung ohne aktives CFG erhält die ursprünglichen Gast-Fallback-Zeiger. Aktives XFG, Exportunterdrückung und weitere unmodellierte Richtlinien bleiben abgewiesen; ausführbarer Speicher allein macht eine Adresse nicht zum gültigen Ziel.

WDM-Stapel können mehrere Geräteobjekte desselben Gasttreibers enthalten. `IoAttachDeviceToDeviceStack` hängt eine isolierte Quelle über das aktuelle oberste Zielgerät und gibt dieses bisherige oberste Gerät zurück. Dabei werden `StackSize` und `AlignmentRequirement` gesetzt; `NextDevice` und Pufferflags werden nicht übernommen. `IoDetachDevice` erhält das gespeicherte untere Gerät und erfordert `PASSIVE_LEVEL`; Anbinden ist bis `DISPATCH_LEVEL` zulässig. Öffnen eines benannten unteren Geräts dispatcht zum aktuellen obersten Gerät, während `FILE_OBJECT.DeviceObject` und Bericht die benannte Identität behalten. READ/WRITE verwendet dessen obere Pufferflags. Die gespeicherte Anforderungsroute hält alle Geräte bis zur Dispatch-Rückkehr auch nach Trennen/Löschen am Leben; interne Referenzen erhöhen nicht den Zähler offener Handles `ReferenceCount`.

`IofCallDriver` und der Helfer `IoCallDriver` rufen das genaue Ziel auf dieser Route auf. Echte Inline-Helfer `IoCopyCurrentIrpStackLocationToNext`, `IoSkipCurrentIrpStackLocation` und `IoSetCompletionRoutine` bearbeiten das ursprüngliche Gast-IRP; Cursor, Anzahl und Steuerflags werden geprüft. Der untere Dispatch liefert seinen tatsächlichen Status unabhängig von `IoStatus` und Completion-Rückgaben. Die Abwicklung verschiebt den Cursor, wählt Callbacks nach Erfolg/Fehler/Abbruch und propagiert pending nach oben. Ein aufgerufener Completion-Callback ist selbst für diese Weitergabe verantwortlich, auch nach einer früheren Dispatch-Rückgabe `STATUS_PENDING`. `STATUS_MORE_PROCESSING_REQUIRED` hält die Abwicklung mit gültigem IRP, MDLs und Puffern an; ein späterer Abschluss setzt sie fort. Verschachtelter Abschluss erfordert das äußere Stop-Ergebnis; endgültige Abwicklung gibt Speicher genau einmal frei. Fortsetzungen mit Subsystem-Eigentümer erhalten verschachtelte WDM/WDF-Aufrufrahmen und geerbten IRQL. Jede verbrauchte untere Stackposition wird vor dem oberen Completion-Callback geleert.

allgemeine Power-Policy, treiberseitig erzeugte IRPs, weitere PnP-Minorfunktionen und weitere Hardware-/Ressourcenmodelle bleiben ununterstützt. WDF-Zielweiterleitung außerhalb des direkten FDO/PDO-Dateilebenszyklus, Anbinden an Stapel mit aktiven Dateien oder Callbacks, Trennen einer mittleren Schicht, Änderung der weitergeleiteten Major-Funktion und Ziele außerhalb der gespeicherten Route scheitern ausdrücklich. Das optionale echte WDK-Fixture `driver_wdm_stack.c` verwendet `NEVERD_WDM_STACK_FIXTURE` und `NEVERD_WDM_STACK_CFG_FIXTURE`. Native und C-API/CLI-Tests umfassen Relokation; fehlende Artefakte werden sichtbar übersprungen. Ausführungsnachweise gelten nur für Linux.

Ein Szenario kann bis zu 64 `pnp_devices` explizit konfigurieren. Jeder Eintrag benötigt `id`, `bus: "resource_free"` oder `bus: "register_bank"`, `initial_device_power: "D0"` und `initial_system_power: "working"`; fehlende Angaben werden nicht geraten. IDs sind 1–64 ASCII-Bytes lang und unterscheiden Groß-/Kleinschreibung: zuerst ein alphanumerisches Zeichen, danach nur solche Zeichen oder `_`, `-`, `.`. Normale Anfragen können ein konfiguriertes `device_id` statt `device` auswählen, jedoch nicht beides. `kind: "pnp"` benötigt `device_id`, `minor` und `bus_completion`; unterstützt sind `start`, `query_remove`, `cancel_remove`, `remove`, `query_stop`, `stop`, `cancel_stop` und `surprise_removal`. `bus_completion.status` ist eine erforderliche 32-Bit-Zahl oder Hex-Zeichenfolge. Optionales `delay_100ns` ist eine nichtnegative Ganzzahl bis INT64_MAX, gemessen ab tatsächlichem Empfang beim Provider. `STATUS_PENDING` ist kein Endstatus; stop/cancel-stop/surprise-removal/cancel-remove/remove erfordern exakt `STATUS_SUCCESS` (0). Datei-, Transfer- und Abbruchfelder werden bei PnP auch mit Wert null/0 abgelehnt. Dieselbe Vorprüfung gilt im C++-API.

```json
{
  "pnp_devices": [
    {"id": "sensor0", "bus": "resource_free", "initial_device_power": "D0", "initial_system_power": "working"}
  ],
  "requests": [
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0", "delay_100ns": 10}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "surprise_removal", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "remove", "bus_completion": {"status": "0x0"}}
  ],
  "unload": true
}
```

Nach erfolgreichem DriverEntry läuft `AddDevice` einmal je konfiguriertem PDO; der Provider besitzt ein separates `DRIVER_OBJECT`, das der Gast weder löschen noch imitieren darf. PnP-IRPs sind `KernelMode`, dateifrei mit START-Ressourcen gemäß Buskonfiguration, anfänglich `STATUS_NOT_SUPPORTED`. Busantworten werden erst bei tatsächlicher Weiterleitung zum PDO verwendet; verzögerter Abschluss nutzt die gemeinsame virtuelle Uhr und Completion-Fortsetzungen. Der endgültige obere Abschluss entscheidet unabhängig vom Busstatus über Zustandsübernahme oder Rollback. PnP-Erfolg setzt tatsächlichen Providerabschluss voraus; frühe START/QUERY_STOP/QUERY_REMOVE-Fehler können ohne Busbeobachtung bleiben. Normales Remove aus Started benötigt erfolgreiche Query, geschlossene Dateien, abgearbeitete frühere Anfragen und Gast-Detach/Delete. Geräte-/Dateiidentität bleibt nach Detach erhalten. Sauberer AddDevice-Fehler entfernt nur den Provider; neue Gastgeräte-Leaks, auch abgetrennte, verursachen `model_error`. Vor Unload müssen alle Provider entfernt sein. Weitere PnP-Funktionen, allgemeine Power-Policy, weitere Hardware-/Ressourcenmodelle und allgemeines KMDF-PnP fehlen.

`configuration.pnp_devices` erhält die Anfangskonfiguration. Beobachtete `pnp_devices` enthalten `id`, `pdo`, nullable `add_device_status`, aktuelles `attached`, `pnp_state`, `provider_present`; nach Remove ist `attached` false. AddDevice-Phasen heißen `add_device:<ID>`; Fehler beeinflussen `scenario_success`, nicht DriverEntrys `nt_status`. Anfragen ergänzen nullable `device_id` und `pnp`; PnP verwendet `file: null`. Das `pnp`-Objekt enthält `minor`, `state_before`, `state_after`, nullable `bus_status`, `bus_received_at_100ns`, `bus_completed_at_100ns`. Konfigurierter Status wird erst beim echten Busabschluss beobachtbar; Empfangszeit bleibt unabhängig. Bestehende Feldtypen ändern sich nicht.

Normale CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE erreichen den echten Gast-Dispatch, solange das Gerät außerhalb Removing/Removed existiert. Stopped, StopPending, RemovePending oder der Energiezustand erzeugen keinen Modellfehlerstatus; der Treiber entscheidet selbst über Software-I/O, Ablehnung oder Zurückhalten. Der öffentliche Ablauf bleibt seriell: Eine gehaltene IRP ohne aktuell verfügbaren Produzenten kann nicht durch einen späteren Start-/Cleanup-Szenarioschritt gelöst werden und endet als festgefahrenes `model_error`. Geschlossene Dateien und abgearbeitete frühere Anfragen vor Remove sind Profilgrenzen. `query_stop` mit abschließendem `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119) wird bei Vorprüfung und Gastabschluss abgelehnt, da es eine unmodellierte Ressourcen-Neuabfrage fordert; siehe [Microsofts QUERY_STOP-Vertrag](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device). Stop/Restart und Surprise-Removal ergänzen keine Ressourcen-Neuverteilung, allgemeine Power-Policy oder allgemeines KMDF-PnP.

Ressourcenfreies PnP verwendet das originale echte WDK-Fixture `driver_wdm_pnp.c`, optionale `NEVERD_WDM_PNP_FIXTURE`/`NEVERD_WDM_PNP_CFG_FIXTURE` und native sowie C-API/CLI-Tests. Fehlende Artefakte werden explizit übersprungen; Ausführungsnachweise bleiben Linux-spezifisch.

Der synthetische `bus: "register_bank"` ergänzt einen PDO um explizite, feste Speicherressourcen. Das Array `resources` ist bei einer nicht leeren `interrupts`-Liste optional; zusammen müssen beide Listen mindestens eine Ressource enthalten. Jeder Speichereintrag enthält `id`, `raw_start`, `translated_start`, `length` und `registers`; jedes Register benötigt `offset`, `width`, `access` (`read_only` oder `read_write`) und einen anfänglichen `value`. `DriverResources.h`／`DriverResources.def` definieren den gemeinsamen C++-/JSON-Vertrag. Ressourcen-IDs folgen den begrenzten ASCII-Kennungsregeln und sind innerhalb eines PDO eindeutig. Die Grenzen liegen bei 8 Ressourcen pro PDO／32 insgesamt,256 Registern pro Ressource／4096 insgesamt und 1–1048576 Bytes pro Ressource. Unterstützt werden nur natürlich ausgerichtete Zugriffe von exakt 1／2／4 Bytes; Werte müssen in diese Breite passen. Beide physischen Intervalle müssen überlauffrei sein. Raw-Bereiche dürfen sich innerhalb eines PDO nicht überschneiden, übersetzte Bereiche global nicht. Leere `registers` erklären die gesamte Bank für unzugänglich. Adressen und Anfangswerte sind deklarierte Fakten, weder Hosthardware noch implizit nullgefüllter Speicher. `resource_free` behält das ausgelassene Ressourceninventar und null START-Zeiger.

START erhält getrennte, schreibgeschützte Raw- und übersetzte `CM_RESOURCE_LIST`-Allokationen mit korrespondierenden Memory-Deskriptoren in derselben Reihenfolge: ein vollständiger Deskriptor, Internal-Schnittstelle, Bus 0, Version/Revision 1, DeviceExclusive-Freigabe und READ_WRITE-Bereichsflags. Registerbezogenes RO bleibt unabhängig. Ein erfolgreicher unterer START stellt die Zuweisung vor den oberen Abschlusscallbacks bereit. Jeder START aus NotStarted/Stopped erzeugt eine neue Ressourcengeneration mit derselben festen Zuweisung. Registerwerte werden einmal pro PDO initialisiert und bleiben über Unmap, STOP und Neustart erhalten. Bei fehlgeschlagenem START und erfolgreichem STOP/REMOVE muss der Treiber seine Mappings vor dem endgültigen IRP-Abschluss entfernen; das Modell bereinigt sie nicht automatisch. Surprise-Removal verhindert sofort neue Mappings und Registerzugriffe, erlaubt aber das Entfernen vorhandener Mappings. Der tatsächlich erfolgreiche Geräte-SET-Abschluss beim Provider ändert die Hardwarezugänglichkeit: D 3 sperrt Zugriffe, D 0 erlaubt sie nur mit verfügbarer Zuweisung. D 3 erlaubt weiterhin Mappings ohne Registerzugriff; Energieänderungen verwerfen weder Mappings noch Werte.

`MmMapIoSpace` unterstützt NonCached; `MmMapIoSpaceEx` unterstützt PAGE_NOCACHE zusammen mit PAGE_READONLY oder PAGE_READWRITE. Beide akzeptieren nur deklarierte übersetzte Teilbereiche innerhalb einer Zuweisung und erhalten deren Seitenoffset. Aliase teilen eine Bank, behalten unabhängige Rechte und benötigen für `MmUnmapIoSpace` die ursprüngliche Basis und exakte Länge. Erschöpfte Mapping-/Adressfenster oder konfigurierte Speicherbudgets liefern NULL; Backendfehler bleiben explizite Fehler. Aufgehobene Adressen werden durch spätere Mappings nicht wieder gültig. Skalare und echte REP-Registerpufferbefehle durchlaufen CPU-MMIO-Prüfungen. Lücken, falsche Breiten, Fehlausrichtung, RO-Schreibzugriffe, Mappinggrenzen und Ausführung werden vor Registereffekten abgelehnt. Speicher-APIs dürfen eine natürlich ausgerichtete Transaktion von exakt 1／2／4 Bytes auf ein einzelnes Register ausführen. Größere Sammelbereiche mit MMIO werden ausdrücklich abgelehnt und nicht automatisch in Registertransaktionen aufgeteilt. Dies implementiert weder beliebigen physischen RAM noch Ressourcen-Neuverteilung, Ports, andere Interruptmodi, weitere DMA-Schnittstellen oder allgemeines Hardwareverhalten.

Das ausführbare [Registerbank-Szenario](../examples/driver-register-bank-scenario.json) verwendet das originale, mit echtem WDK erstellte `driver_wdm_resources.c` über die optionalen `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`. Es führt 14 Anforderungen mit verzögertem START, Datei-I/O, STOP, Neustart und Entfernung aus und beobachtet erhaltene Werte über das IOCTL des Treibers. `configuration.pnp_devices[].resources` hält Anfangsfakten mit verlustfreien hexadezimalen physischen Adressen fest; es ist kein zweiter Zustandsbericht der Bank. C API und Python verwenden weiterhin `scenario_json` und das unveränderte `neverd_driver_options_v 1`. Fehlende echte Artefakte werden ausdrücklich übersprungen; die bisherigen Ausführungsnachweise stammen nur von Linux.

Derselbe `register_bank`-Provider kann `interrupts` allein oder zusammen mit `resources` deklarieren; mindestens eine Liste muss nicht leer sein. `resource_free` lehnt ein explizites Feld `interrupts` einschließlich `[]` ab. Jeder Interrupt benötigt `id`, `raw_vector`, `raw_level`, `raw_affinity`, `translated_vector`, `translated_level`, `translated_affinity`, `mode: "latched"` und `share: "device_exclusive"`. `DriverInterrupts.h` / `DriverInterrupts.def` besitzen Typen, Feldnamen und Grenzen: 8 Interrupts pro PDO / 32 insgesamt, eindeutige begrenzte ASCII-IDs je PDO, global exklusive übersetzte Vektoren, Raw-Level 0–65535 und übersetzter DIRQL 3–12. Vektoren behalten alle 32 Bits; eine Beziehung zwischen Vektor und IRQL wird nicht geraten. Beide Affinitätsmasken müssen 1 sein: CPU0 und Gruppe0 sind explizite Providerfakten. Raw- und übersetzte Werte sind unabhängig. Speicherdeskriptoren behalten ihre Reihenfolge, gefolgt von geordneten Interruptdeskriptoren in derselben `CM_RESOURCE_LIST`; diese verwenden Typ 2, DeviceExclusive und LATCHED. Dieses synthetische flankengetriggerte Profil bildet keine gemeinsam genutzten PCI-Pegelleitungen ab.

READ-/WRITE-/IOCTL-Anforderungen dürfen `interrupt_events` mit jeweils explizitem `after_100ns`, `device_id` und `interrupt_id` deklarieren. Andere Anforderungsarten lehnen das Feld auch dann ab, wenn es leer ist. Zulässig sind höchstens 64 Ereignisse je Anforderung / 1024 insgesamt und nichtnegative Verzögerungen bis INT64_MAX. Die erfolgreiche Einreichung verankert die Verzögerung und erfasst einen bereits verbundenen Interrupt samt PDO-Ressourcengeneration. Ereignisse sind unabhängige externe Leitungsimpulse: Der Abschluss des Quell-IRP verwirft sie nicht, und Registerschreibzugriffe implizieren kein Enable-, Status- oder Bestätigungsverhalten. Zeit schreitet nur im Leerlauf fort; fällige Ereignisse laufen an der nächsten unterstützten Callback-Grenze. `after_100ns: 0` verspricht daher weder Befehlspräemption noch Zustellung vor dem ersten Gastbefehl. Gleichzeitige Erzeuger werden gemeinsam auf Kapazität geprüft; der Provider veröffentlicht Hardwarezustand vor der Zulässigkeitsprüfung, und ISR-Callbacks gehen DPCs/Workern vor. Eine verlorene Verbindung, veraltete/nicht verfügbare Generation oder physisches D3 zeichnet `undelivered_reason` auf und stoppt als `model_error`; das Ereignis wird weder neu gebunden noch still verzögert.

`IoConnectInterrupt` verwendet seine tatsächlichen elf Argumente und die exakte übersetzte Zuweisung. `IoConnectInterruptEx` unterstützt FullySpecified (1), LineBased (2, eine zugewiesene Leitung am expliziten PDO) und FullySpecifiedGroup (4, Gruppe 0); `IoDisconnectInterruptEx` verlangt passende Version und Kontext. Registrierung und Trennung erfordern PASSIVE_LEVEL. Unterstützt werden nur eine private Interruptsperre, exklusiver Latched-Modus, CPU0/Gruppe0 und keine Sicherung des Gleitkommazustands. Der Synchronisations-IRQL muss dem zugewiesenen DIRQL entsprechen; bei LineBased wählt Null diesen Wert. `KINTERRUPT` ist opak, seine Adresse wird nie wiederverwendet. Die echte ISR erhält `(Interrupt, ServiceContext)` und gibt BOOLEAN in AL zurück; `FALSE` bedeutet nicht beansprucht, keinen NTSTATUS-Fehler. `KeSynchronizeExecution` führt den echten Callback mit einem Argument unter derselben Sperre auf DIRQL aus, gibt dessen BOOLEAN zurück und stellt IRQL/CR8 des Aufrufers wieder her. `KeAcquireInterruptSpinLock` / `KeReleaseInterruptSpinLock` erzwingen nichtrekursiven Besitz, ursprüngliche Ausführung und gespeicherten IRQL; gehaltene Sperren dürfen keine Callback-Rückkehr überdauern. Warten im Interrupt, vom Aufrufer bereitgestellte gemeinsame Sperren, gemeinsam genutzte/Pegel-/MSI-/passive Interrupts und Befehlspräemption bleiben unmodelliert. Fehlgeschlagener START und erfolgreicher STOP/REMOVE verlangen die Trennung vor dem endgültigen Abschluss; obere Abschlusscallbacks können zuvor abbauen, und Disconnect verwirft keinen eingereihten DPC stillschweigend.

Berichte trennen Deklarationen von Beobachtungen. `configuration.pnp_devices[].interrupts` erhält die Ressourcen; `configuration.interrupt_events` flacht Eingabeereignisse mit nullbasiertem `source_request_index` (Index der konfigurierten Anforderung) und `event_index` ab. Die obersten `interrupts`-Zeilen ergänzen `device_id`, `interrupt_id`, `epoch`, absolutes `due_at_100ns` sowie nullable `occurred_at_100ns`, `delivered_at_100ns`, `returned_at_100ns`, `interrupt_object`, `return_value`, `claimed` und `undelivered_reason`. `claimed` folgt ausschließlich dem tatsächlich zurückgegebenen niedrigen Byte; Zeitstempel sind Beobachtungen, keine erfundenen Callback-Ergebnisse. Für `scenario_success` muss jedes konfigurierte Ereignis ohne Nichtzustellungsfehler zurückkehren; eine nicht beanspruchende ISR bleibt gültig. DPC-Effekte sind durch tatsächlichen Anforderungsabschluss, API-Aufrufe und Nachrichten sichtbar. Das ausführbare [Interruptszenario](../examples/driver-interrupt-scenario.json) nutzt das originale WDK-Fixture `driver_wdm_interrupts.c` mit optionalem `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`: Sieben Anforderungen umfassen verzögerten START, ein durch ISR→DPC abgeschlossenes wartendes IOCTL, Dateibereinigung/-schließung und Entfernung. Die bestehende C-/Python-Grenze `scenario_json` und das Layout `neverd_driver_options_v1` bleiben unverändert. Fehlende echte Artefakte werden explizit übersprungen; Ausführungsnachweise gelten nur für Linux.

`DriverDMA.h` / `DriverDMA.def` ergänzen einen `register_bank`-PDO um ein optionales `dma`-Objekt neben seinen Speicher-/Interruptzuweisungen. DMA allein ersetzt keine dieser Ressourcenlisten. Alle sieben Felder sind explizit: `address_bits` (32 oder 64), `maximum_length` (1–1048576 Bytes), `map_registers` (1–256), `alignment` (Zweierpotenz von 1–4096), `logical_base` (ungleich null, seitenbündig), `logical_length` (seitenbündig, 4096–1073741824 Bytes) und der boolesche Wert `scatter_gather`. Der überlauffreie Adressbereich muss zur Adressbreite passen. Jeder PDO besitzt einen unabhängigen logischen Adressraum; gleiche Adressen verschiedener Geräte bilden daher keine Aliase. Übersetzte MMIO-Ressourcen dürfen den reservierten Modell-RAM-Bereich `[0x1000000000, 0x1000100000)` nicht überlappen. Diese Angaben beschreiben einen kohärenten synthetischen Busmaster, weder physischen Hostspeicher noch ein PCI-Gerät.

`IoGetDmaAdapter` akzeptiert die historischen Felder von `DEVICE_DESCRIPTION` Version 0/1 für einen Internal-Busmaster und stellt einen `DMA_ADAPTER` der Version 1 mit seiner tatsächlichen 104-Byte-Tabelle `DMA_OPERATIONS` bereit. Versionsabfragen 2/3 liefern NULL, ohne das Ende einer neueren Beschreibung zu lesen. Jede indirekte Methode ist an genau diesen gültigen Adapter gebunden, unabhängig von Kernelimports. Implementiert sind `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `GetScatterGatherList`, `PutScatterGatherList`, `PutDmaAdapter`, `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers` und `FreeMapRegisters`. `FreeAdapterChannel` und `ReadDmaCounter` bleiben namentlich ausgewiesene Fehlerstellen, da dieses Profil keinen untergeordneten/System-DMA-Controller modelliert. Zuweisen/Freigeben gemeinsamer Puffer sowie Abfragen der Ausrichtung verlangen PASSIVE_LEVEL, Get/PutScatterGatherList verlangen DISPATCH_LEVEL. Die Adapterfreigabe erlaubt IRQL bis einschließlich DISPATCH_LEVEL. x64 ignoriert `CacheEnabled`. Abfragen nicht unterstützter Versionen, unvereinbare deklarierte Fähigkeiten und dokumentierter Ressourcenmangel bei der Zuweisung liefern NULL. Ungültige oder unmodellierte Schnittstellenauswahlen und Backendfehler bleiben explizite Fehler.

`AllocateAdapterChannel` verlangt DISPATCH_LEVEL und reserviert ein opakes, von NULL verschiedenes Mapregister-Token. Gemeinsame Puffer, SG-Listen und Kanalreservierungen teilen sich dasselbe Kontingent und dieselbe FIFO pro PDO. Erfolg nimmt einen echten sofortigen oder wartenden `AdapterControl` an; eine zu große angeforderte Anzahl liefert `STATUS_INSUFFICIENT_RESOURCES` ohne Callback. Pro Gastgerät ist ein noch nicht beendeter Zuweisungscallback erlaubt. AllocateAdapterChannel aus AdapterControl wird auch über einen verschachtelten Callback zurückgewiesen. Die vier Callback-Argumente enthalten den tatsächlichen Wert von `DEVICE_OBJECT.CurrentIrp` zum Registrierungszeitpunkt. Genau dieses acht Byte große Feld ist auf Gastgeräten schreibbar; zugelassen sind null oder ein gültiger, durch das Gerät gerouteter IRP. Ein wartender Callback hält das Paket bis zum Eintritt fest; danach darf er es abschließen. Das Profil besitzt weiterhin kein StartIo, daher bleibt das unbenutzte IRP-Argument des separaten SG-Callbacks NULL.

`AdapterControl` liefert eine 32-Bit-`IO_ALLOCATION_ACTION`; hohe RAX-Bits werden ignoriert, und die Aktion ersetzt niemals `STATUS_SUCCESS` von AllocateAdapterChannel. `DeallocateObject` gibt unbenutzte oder vollständig geflushte Register bei der Callback-Rückkehr frei. `DeallocateObjectKeepRegisters` hält sie bis zu FreeMapRegisters mit exakt passendem Adapter, Token und ursprünglicher Anzahl. `KeepObject` erfordert einen unmodellierten Systemcontroller und scheitert ausdrücklich. Die neu zugestellte Zuweisung kann vor Callback-Rückkehr noch nicht als behaltene Zuweisung freigegeben werden; eine andere zuvor behaltene Zuweisung darf gemäß ihrem eigenen Vertrag freigegeben werden. Callback-Identität, behaltene Register und aktiv abgebildete Bytes besitzen getrennte Lebensdauern, die vor Adapter- oder Geräteabbau geprüft werden.

`MapTransfer` und `FlushAdapterBuffers` erlauben IRQL bis einschließlich DISPATCH_LEVEL; Kanalzuweisung und Registerfreigabe verlangen DISPATCH_LEVEL. MapTransfer erhält eine MDL-relative Position, liest und aktualisiert eine echte ULONG-Länge und gibt die logische Adresse als Wert zurück. Das begrenzte SG-Profil liefert pro Aufruf ein Fragment einer zugrunde liegenden Seite. Unmittelbar anschließende Positionen im selben MDL und in derselben Richtung erweitern eine Operation. Ohne SG wird der gesamte angeforderte Bereich in einem Aufruf abgebildet, sofern die reservierte Anzahl genügt; die Länge wird nicht gekürzt. Die erste Abbildung reserviert einen nicht wiederverwendeten logischen Bereich in Größe der Registerreservierung; andere Zuweisungen dürfen sich zeitlich dazwischenschieben, ohne ihn zu überlappen. Alle Fragmente teilen sich eine wachsende physische Bereichsbindung. Gerätetransaktionen dürfen die gesamte aktuell abgebildete Operation überspannen. CPU-Zugriff, MDL-Freigabe und Abschlüsse, die gebundenen Speicher stilllegen, bleiben bis zum gemeinsamen Flush gesperrt. Flush muss die Anfangsposition, den MDL, die Richtung und die gesamte tatsächlich abgebildete Länge treffen. Er löst die abgebildeten Bytes, nicht die Register; das behaltene Token kann daher eine weitere Operation tragen. Teil-Flushes, Operationen über verschiedene MDLs und weitere MapTransfer-Muster liegen außerhalb dieses Profils und werden nicht pauschal als auf jedem Windows-System ungültig eingestuft.

`KeFlushIoBuffers` prüft einen gültigen gesperrten/nicht auslagerbaren MDL. Die modellierte Plattform ist kohärent: Für beide Werte von ReadOperation und DmaOperation ist keine separate Cache-Kopie nötig. Der Aufruf gibt keine DMA-Eigentümerschaft frei und ersetzt FlushAdapterBuffers nicht. Das [Kanalszenario](../examples/driver-dma-channel-scenario.json) führt den Originaltreiber `driver_wdm_dma_channel.c` mit zwei MapTransfer-Aufrufen, einer seitenübergreifenden Gerätetransaktion, separat deklariertem IRQ/DPC, gemeinsamem Flush und exakter Registerfreigabe aus. Echte Normal-/CFG-Abbilder verwenden `NEVERD_WDM_DMA_CHANNEL_FIXTURE` und `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`.

`KernelPhysicalMemory` weist bestehendem RAM höchstens 256 physische Modellseiten von jeweils 4096 Bytes zu. Virtuelle CPU-Adressen, physische Seitenidentitäten und logische Geräteadressen sind getrennt. PFN-Arrays aufgebauter MDLs geben diese gemeinsamen Identitäten schreibgeschützt frei; nicht aufgebaute Deskriptoren besitzen keine nutzbaren PFNs. Kleine benachbarte Zuweisungen können einen PFN teilen, behalten aber eigene Bytebereiche und Lebensdauern. Gemeinsame Puffer, Poolspeicher und Anforderungspuffer verwenden dieselben bereits von `GuestMemory` gehaltenen Bytes ohne zweite DMA-Kopie. Eine aktive SG-Abbildung hält ihren exakten Datenbereich und Deskriptor fest. Abschluss, Pool-/MDL-Freigabe und Abbau weisen noch aktive Abhängigkeiten vor dem Ungültigmachen zurück. Das Aufheben einer direkten MDL-Abbildung entzieht nur die CPU-Systemabbildung; DMA erreicht weiterhin deren gesperrten RAM. `DmaWritable` hält den Schreibsperrvertrag getrennt von CPU-Abbildungsrechten fest: Geräteschreibzugriffe benötigen direktes READ/OUT_DIRECT oder beschreibbaren nicht ausgelagerten Speicher. WRITE/IN_DIRECT erhalten diese Berechtigung nicht allein durch eine beschreibbare CPU-Abbildung.

`GetScatterGatherList` validiert CurrentVa/Length anhand des ursprünglichen MDL-Bereichs und erzeugt logische Seitenfragmente über dessen bestehendem RAM. Freie Mapregister erlauben den echten void-Callback `AdapterListControl` mit vier Argumenten noch vor der API-Rückkehr inline auszuführen. Andernfalls werden Daten/Deskriptor gehalten und ein Callback in der PDO-FIFO bis zur Ressourcenfreigabe reserviert. Dieses Profil besitzt keine StartIo-Eigentümerschaft; das zweite IRP-Argument ist daher NULL. Die Callback-Rückkehr gibt keine Abbildung frei. CPU-Zugriff auf einen aktiven SG-Datenbereich verlangt zuvor Put; ein noch auf Mapregister wartender Callback hat dem Gerät noch keine Hoheit über diese Bytes gegeben. `PutScatterGatherList` darf im Callback laufen. Danach darf der Treiber die Anforderung abschließen und den letzten Adapter freigeben, während Callback-Fortsetzung und Gerätereferenz bis zur Rückkehr erhalten bleiben. Die Freigabe gemeinsamer Puffer verlangt den ursprünglichen Adapter, die Länge, die logische Adresse und die CPU-Adresse. Logische Adressen werden während der Sitzung auch nach Neustarts niemals wiederverwendet. Ressourcenwarten ohne echten Erzeuger meldet ausdrücklich Stillstand und erfindet weder Abschluss noch Frist.

Nur READ/WRITE/IOCTL-Anforderungen akzeptieren `dma_events`. Jedes Ereignis verlangt `after_100ns`, `device_id`, `logical_address`, `direction` und `length`. `write_memory` verlangt zusätzlich `data_hex` mit exakt passender Länge; `read_memory` weist dieses Feld zurück. Richtungen gelten aus Gerätesicht. Die Grenzen sind 64 Ereignisse pro Anforderung, insgesamt 1024, insgesamt 16 MiB Transaktionsdaten und 1 MiB pro Transaktion; die Verzögerung liegt zwischen null und INT64_MAX. Die Einreichung erfasst die aktuell zugewiesene PDO-Epoche und den virtuellen Zeitanker, verlangt aber keine Abbildung, die erst der folgende Dispatch anlegen wird. Bei Zustellung werden der vollständige aktive logische Bereich und die Richtung aufgelöst; physisches D0 und alle zugrunde liegenden Bytes werden vor jeder Transaktionswirkung geprüft. Der Abschluss des Quell-IRP hebt ein Ereignis nicht auf. Fehlende/freigegebene Abbildungen, veraltete Epochen, überraschendes Entfernen oder D3 zeichnen einen Fehler auf und stoppen. Ereignisse binden sich nicht neu und erfinden weder Interrupt noch Registerprotokoll oder IRP-Abschluss. An derselben Ausführungsgrenze veröffentlicht zuerst der Provider den Hardwarezustand, danach folgen DMA-Bytes und anschließend unabhängig deklarierte Interruptimpulse. Die Zeitsteuerung bleibt kooperativ, ohne Präemption auf Befehlsebene.

Berichte enthalten `configuration.pnp_devices[].dma` und das zusammengefasste `configuration.dma_events`. Zeilen unter `dma_transfers` identifizieren `source_request_index`, `event_index`, `device_id`, `epoch`, `logical_address`, `direction`, `length` und `due_at_100ns`; `occurred_at_100ns`, `completed_at_100ns`, `mapping`, `adapter` und `failure_reason` können null sein. `data_hex` enthält tatsächlich übertragene Bytes. Für `scenario_success` muss jede deklarierte Transaktion ohne Fehler enden. Das [DMA-Szenario](../examples/driver-dma-scenario.json) nutzt den eigenständig geschriebenen und mit echtem WDK gebauten `driver_wdm_dma.c` über die optionalen `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`. Es greift über echte Adapterzeiger auf einen gemeinsamen Puffer zu und schließt über separat deklarierte ISR→DPC-Ausführung ab. C/Python verwenden weiterhin `scenario_json`, ohne `neverd_driver_options_v1` zu ändern. Fehlende Artefakte werden ausdrücklich übersprungen; Ausführungsnachweise gelten nur für Linux. Untergeordnete Controller, V2/V3-Methoden, Hardware-Deskriptor-Engines, allgemeines KMDF-DMA und weitere Gerätemodelle bleiben unmodelliert.

WDM-Remove-Locks verwenden die echten Exporte `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx`, `IoReleaseRemoveLockEx` und `IoReleaseRemoveLockAndWaitEx`; WDK-Namen ohne Ex sind Makros. Eigentümer ist das genaue DEVICE_OBJECT, dessen Erweiterung die vollständig ausgerichtete Ablage enthält, unabhängig von PDO-Zustand oder Tag-Form. Initialisierung vor der Anbindung ist erlaubt. Retail 32/DBG 120 Bytes benötigen den passenden separaten Größenparameter; die gesamte registrierte Ablage ist opak. NULL und wiederholte Tags werden je Lock gezählt und nie dereferenziert, daher bleibt Release nach IRP-Abschluss gültig. Initialisierung/AndWait verlangen `PASSIVE_LEVEL`, Acquire/Release erlauben `DISPATCH_LEVEL`.

AndWait schließt die Zulassung, gibt eine passende Erwerbung frei und suspendiert den echten Gastrahmen bis alle übrigen freigegeben sind. Späteres Acquire liefert `STATUS_DELETE_PENDING` ohne Freigabepflicht. Die letzte Freigabe setzt Bereitschaft vor Callback-Rückkehr; ein Worker darf danach auf ein Ereignis des fortgesetzten REMOVE warten. Kein synthetischer Callback, Timeout oder Erfolg ohne Produzent wird erzeugt. Erforderlich sind eine zugeordnete aktive REMOVE-Route mit dem Eigentümer und tatsächlicher Providerempfang (`bus_received_at_100ns` darf 0 sein), kein unterer Abschluss. Ein unterer Treiber, der vor Providerempfang einreiht, liegt außerhalb dieses Profils; es ist keine vollständige Prüfung von OutsideRemoveDevice oder Driver Verifier. Dateien und frühere Anforderungen müssen vor REMOVE abgeschlossen sein, freigebende Callbacks dürfen noch laufen. Die Route bleibt durch Warten, Trennen/Löschen und unteren Pending-Abschluss bis zur Rückkehr aller übrigen Rahmen erhalten.

Unbekannte/falsch große Ablage, unpassendes Release, doppeltes Drain, Neuinitialisierung oder Löschen mit Erwerbungen beziehungsweise unverbrauchtem Drain-Warten schlagen vor Änderung fehl. Sauberer AddDevice-Fehler darf ein initialisiertes ungenutztes Lock löschen. Locks ersetzen keine echten Geräte-/Work-Item-Referenzen und werden erst beim tatsächlichen Freigeben der Erweiterung deregistriert. Debug-Daten aktivieren keine Verifier-Zeit-/High-Water-Regeln. Die echte `driver_wdm_remove_lock.c` verwendet `NEVERD_WDM_REMOVE_LOCK_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`. Fehlende Artefakte werden explizit übersprungen; Native- und C-API/CLI-Nachweise gelten nur für Linux, nicht für einen vollständigen Remove-Manager oder allgemeines paralleles I/O-Drain.

WDM-Power-Anforderungen verwenden `kind: "power"` mit einer konfigurierten `device_id`. Jedes Paket benötigt ausdrücklich `minor` (`query`/`set`), `power_type` (`device`/`system`), `power_state` (`D0`/`D3` oder `working`/`sleeping3`), `power_action` (`none`/`sleep`), `system_context` (32-Bit-Ganzzahl oder Hex-String) und `bus_completion`. System Query nach Working ist ununterstützt; Datei-, Transfer- und Abbruchfelder sind verboten. Der gesamte `system_context` bleibt ein opaker Paketwert und bestimmt weder Elternanforderungen noch Ruhezustand/Schnellstart. Routen benötigen `DO_POWER_PAGABLE` ohne `DO_POWER_INRUSH`; Power-Dispatch und `PoRequestPowerIrp` laufen auf `PASSIVE_LEVEL`. `PoCallDriver` leitet dieselbe verwaltete Power-IRP weiter; `PoStartNextPowerIrp` folgt Vista+ ohne zusätzlichen Serialisierungshandshake. Allgemeine Power-Policy, WAIT_WAKE, andere Zustände/Aktionen, Herunterfahren/Ruhezustand, Einschaltstrom, nicht auslagerbare Routen, allgemeine Hardwaremodelle und allgemeines KMDF-PnP bleiben ausgeschlossen.

Jeder `pnp_devices`-Eintrag kann `initial_reported_device_power: "D0"` oder `"D3"` unabhängig von den erforderlichen anfänglichen Lebenszykluswerten D0/working angeben. PDO und jedes erstmals zugeordnete Gast-DEVICE_OBJECT haben separate Benachrichtigungszustände; `PoSetPowerState` liefert und aktualisiert nur den bisherigen Wert dieses Geräts. Ohne expliziten Startwert schlägt der Aufruf fehl, statt D0 anzunehmen. Optionales `requested_device_power` enthält device-Vorlagen mit denselben sechs Pflichtangaben, insgesamt höchstens 64 für alle PDOs. Nur ein echter `PoRequestPowerIrp` mit passendem PDO, Minor und Ziel verbraucht den FIFO-Kopf dieses PDO. Fehlende/unpassende Einträge schlagen fehl; ungenutzte erzeugen keine Anforderung, der Callback-Kontext bestimmt keinen Elternteil. Jedes Kind hat eine eigene IRP und Berichtszeile mit `origin: "PoRequestPowerIrp"` und nullbasiertem `response_index`; Szenariozeilen nutzen `origin: "scenario"` und null als Index. Synchrone Kinder können den fünfargumentigen void-Callback vor der API-Rückgabe `STATUS_PENDING` ausführen. Callbacks dürfen warten; System S0 darf vor dem unabhängigen D0-Kind enden. Der IO_STATUS_BLOCK-Snapshot gilt bis zur Callback-Rückkehr.

Berichte ergänzen nullable `power`; Power-Zeilen haben `file: null`. `power` enthält Paketwerte, `device_state_before`/`device_state_after`, `system_state_before`/`system_state_after`, nullable `requested_device_object` und tatsächliche `bus_status`/`bus_received_at_100ns`/`bus_completed_at_100ns`. PnP-Endzustände ergänzen `device_power`/`system_power`, lebende Geräte nullable `reported_device_power`. `scenario_success` zählt nur Szenariozeilen gegen die Konfiguration, verlangt aber erfolgreiche Abschlüsse aller tatsächlichen Kinder und Szenarioanforderungen; ungenutzte Vorlagen sind kein Fehler. Der echte `driver_wdm_power.c` verwendet `NEVERD_WDM_POWER_FIXTURE`/`NEVERD_WDM_POWER_CFG_FIXTURE` für normale/active-CFG Native- und C-API/CLI-Abdeckung. Fehlende Artefakte werden ausdrücklich übersprungen; Ausführungsnachweise gelten nur für Linux.

Das [vollständige Power-Szenario](../examples/driver-power-scenario.json) führt die echte Fixture mit `--scenario` durch Start, Systemabfrage/Schlaf/Rückkehr und Entfernung, mit drei expliziten Kindantworten.

KMDF 1.33 verwendet die genaue ABI 1.33.0: 458 Funktionsplätze besitzen stabile Gastidentitäten; die folgenden 83 APIs haben Ausführungssemantik. `WdfVersionBind` und `WdfVersionUnbind` verwalten Gastbindungen rund um den echten WDK-Wrapper `FxDriverEntry`. `WdfGetDriver` liest die öffentlichen Treiberglobals. Nicht-PnP-Treiber, generische Objekte, Steuergeräte, Warteschlangen und eingehende Anforderungen verwenden typisierte Kontexte, Referenzzähler und ausgeführte Cleanup-/Destroy-/Unload-Callbacks. Alle modellierten Framework-Aufrufe und Callbacks erfordern derzeit `PASSIVE_LEVEL`; neue Referenzen nach abgeschlossenem Cleanup liegen weiterhin außerhalb dieses Profils. Unmodellierte Funktionsplätze, `WdfLdrQueryInterface`, Klassenerweiterungen und UMDF stoppen explizit.

Steuergeräte benötigen einen kopierten Namen aus druckbaren ASCII-Zeichen und exakt die SDDL `D:P(A;;GA;;;WD)`. Sie gewährt allgemeinen Zugriff, ohne ein Aufrufertoken zu erfinden; andere Sicherheitsdeskriptoren, unbenannte Geräte und automatische Namen werden nicht unterstützt. Die Geräteinitialisierung besitzt ein WDM-Gerät. Anforderungen können es im bestehenden Sitzungsnamensraum über die symbolischen Link-Aliasse `\DosDevices\Name` oder `\??\Name` auswählen; der Bericht behält den kanonischen Gerätenamen bei. Erfolgreiche Erstellung verbraucht das Initialisierungsobjekt und leert dessen Zeiger, fehlgeschlagene Erstellung setzt den teilweise entstandenen Gerätebesitz zurück. `WdfControlFinishInitializing` gibt die I/O-Zustellung frei. Das Löschen entfernt Gerät und Links nur, wenn modellierte Dateien, Arbeitselemente und Anforderungen dies erlauben; Abbruch oder Abarbeitung ausstehender Anforderungen während des Löschens werden nicht unterstützt.

Die 96 Byte große Struktur `WDF_IO_QUEUE_CONFIG` unterstützt manuelle, sequenzielle und begrenzte oder unbegrenzte parallele Standard- und Nichtstandardwarteschlangen mit expliziter passiver Ausführung und ohne Framework-Synchronisierung. Warteschlangen von Steuergeräten unterliegen keiner Energieverwaltung. Spezifische READ-/WRITE-/IOCTL-Callbacks haben Vorrang vor dem Standardcallback. Angenommene Warteschlangenanforderungen liefern auch bei synchronem Abschluss `STATUS_PENDING`; das Rückgaberegister eines void-Callbacks schließt seine Anforderung nicht ab. Verzögerter Abschluss nutzt den vorhandenen Scheduler. Ohne Handler wird mit `STATUS_INVALID_DEVICE_REQUEST` abgeschlossen; READ/WRITE mit Länge null wird ohne Zustellung abgeschlossen, sofern diese nicht aktiviert ist. Das Standarddateipaket schließt CREATE/CLEANUP/CLOSE erfolgreich mit Information=0 ab. Nicht standardmäßige manuelle Warteschlangen nehmen Anforderungen über `WdfRequestForwardToIoQueue` auf; `WdfIoQueueRetrieveNextRequest` gibt sie in FIFO-Reihenfolge zurück. Wird eine Anforderung vorher abgebrochen, entfernt und beendet das Framework sie mit `STATUS_CANCELLED`. Automatische Nichtstandardwarteschlangen stellen weitergeleitete Anforderungen über ihre eigenen Callbacks zu; manuelle Standardwarteschlangen behalten eingehende Anforderungen bis zur Entnahme. `WdfIoQueueRetrieveNextRequest` funktioniert für manuelle und sequenzielle Warteschlangen; parallele liefern `STATUS_INVALID_DEVICE_STATE`. Ohne passenden Callback im automatischen Ziel wird der eingereihte Auftrag nach Freiwerden eines Präsentationsplatzes mit `STATUS_INVALID_DEVICE_REQUEST` abgeschlossen. Weitergehendes PnP- und Energieverhalten bleibt außerhalb dieses Profils. `WdfRequestRequeue` legt eine entnommene Anforderung wieder an den Anfang derselben manuellen Warteschlange. `NumberOfPresentedRequests` begrenzt die gleichzeitig zugestellten Anforderungen; weitere warten bis zum Abschluss oder Abbruch einer zugestellten Anforderung. Eine sequenzielle Standardwarteschlange nimmt weitere eingehende Anforderungen an, während eine Anforderung präsentiert ist; diese warten in FIFO-Reihenfolge auf einen freien Platz und können vor der Zustellung abgebrochen werden. `WdfIoQueueStop` unterbindet die Zustellung, nimmt aber weiterhin Anforderungen an. `WdfIoQueueStart` liefert wartende Anforderungen aus, und `WdfIoQueueGetState` meldet die Anzahl wartender und zugestellter Anforderungen. Eine Abfrage während des Stopps liefert `STATUS_WDF_PAUSED`; Ein Stop-Abschlusscallback erhält den angegebenen Kontext, sobald alle bereits zugestellten Anforderungen abgeschlossen oder aus der Queue entfernt sind; wartende Anforderungen verzögern ihn nicht. Eine zweite Registrierung während eines ausstehenden Callbacks wird abgelehnt.

`WdfIoQueueReadyNotify` registriert einen `EvtIoQueueState`-Callback für eine manuelle Warteschlange. Bei `PASSIVE_LEVEL` erhält er `(WDFQUEUE, WDFCONTEXT)`, wenn die Zahl eingereihter Anforderungen von null auf einen positiven Wert wechselt, auch wenn der Treiber bereits entnommene Anforderungen noch besitzt. Eine bereits gefüllte Warteschlange kann sofort nach Registrierung melden; eine gestoppte wartet bis `WdfIoQueueStart`. Doppelte Registrierung oder Abmeldung vor dem Stoppen liefert `STATUS_INVALID_DEVICE_REQUEST`. Nach `WdfIoQueueStop` meldet NULL den Callback ab.

`WdfIoQueueFindRequest` durchsucht eine manuelle Warteschlange, ohne dem Treiber die Anforderung zur Bearbeitung zu übergeben. Bei Erfolg wird eine Referenz hinzugefügt, die der Treiber mit `WdfObjectDereference` freigibt. `WdfIoQueueRetrieveFoundRequest` überträgt die Bearbeitung einer noch eingereihten Anforderung; nach einer Entfernung durch Abbruch folgt `STATUS_NOT_FOUND`. Optionale Anforderungsparameter nutzen dasselbe Layout wie `WdfRequestGetParameters`. Ein aktives Framework-Dateiobjekt filtert `WdfIoQueueFindRequest`. `WdfIoQueueRetrieveRequestByFileObject` entnimmt die nächste passende Anforderung aus einer manuellen oder sequenziellen Warteschlange und lässt die Ausgabe ohne Treffer unverändert.

Ein konfigurierter `EvtIoCanceledOnQueue` erhält `(WDFQUEUE, WDFREQUEST)` nur für eine Anforderung, die der Treiber bereits erhalten und danach weitergeleitet oder erneut eingereiht hat, oder die ein Aufrufkontext-Callback ausdrücklich eingereiht hat. Eine nie an den Treiber gelieferte Anforderung beendet das Framework mit `STATUS_CANCELLED` ohne diesen Callback. Die Benachrichtigung überträgt die Anforderung zurück an den Treiber; er muss sie im Callback oder später abschließen und darf sie nicht erneut einreihen. Purge und Zustandsabschluss warten auf die Rückkehr des Callbacks und den Abschluss der treibereigenen Anforderung.

`WdfIoQueueDrain` lehnt neue Anforderungen mit `STATUS_INVALID_DEVICE_STATE` ab und liefert bereits eingereihte Anforderungen weiter. Der Abschlussrückruf erfolgt, wenn keine Anforderungen eingereiht oder vom Treiber gehalten werden. Weiterleitung liefert `STATUS_WDF_BUSY`; `WdfIoQueueStart` stellt die Annahme wieder her.

`WdfIoQueuePurge` bricht noch nicht gelieferte Anforderungen mit `STATUS_CANCELLED` ab und fordert für bereits gelieferte, als abbrechbar markierte Anforderungen den Abbruch auf dem ursprünglichen IRP an. Die Bereinigung erfolgt vor der IRP-Freigabe. Der Zustandsrückruf wartet auf eingereihte und vom Treiber gehaltene Anforderungen sowie aktive Abbruchrückrufe. Nicht abbrechbare Anforderungen beendet weiterhin der Treiber.

`WdfIoQueueStopSynchronously`, `WdfIoQueueDrainSynchronously` und `WdfIoQueuePurgeSynchronously` halten den Gastaufruf auf `PASSIVE_LEVEL` an, bis die betreffenden Anforderungen beendet sind. Stop nimmt weiter an, hält die Zustellung an und wartet auf bereits zugestellte Anforderungen. Drain lehnt neue Anforderungen ab, stellt wartende zu und wartet auf beide Gruppen. Purge bricht wartende und als abbrechbar markierte Anforderungen ab und wartet auf die Rückkehr der Abbruch-Callbacks. Ohne Quelle für einen Abschluss wird ein Modellfehler wegen Stillstands gemeldet.

`WdfIoQueueStopAndPurge` und `WdfIoQueueStopAndPurgeSynchronously` brechen bereits eingereihte sowie als abbrechbar markierte, vom Treiber gehaltene Anforderungen ab. Neue Anforderungen werden weiter angenommen, aber erst nach `WdfIoQueueStart` zugestellt. Der asynchrone Zustands-Callback und die synchrone Warteoperation enden nach den ursprünglichen Anforderungen und ihren Abbruch-Callbacks; später eingehende Anforderungen bleiben eingereiht und verzögern die Benachrichtigung nicht. Beide Stop-Varianten aktivieren nach Drain oder Purge die Annahme erneut.

Anforderungsparameter verwenden das 40-Byte-Layout `WDF_REQUEST_PARAMETERS`. Eingabe-/Ausgabezugriffe liefern logische Längen und erhalten Pufferaliasse sowie vorhandene MDL-Abbildungen für direkte I/O; die Eingabe direkter IOCTLs bleibt gepuffert. Falsche Richtungen und zu kleine Puffer liefern dokumentierte Statuswerte. Der Abschluss führt das Cleanup bei noch gültigen Puffern aus, schließt das IRP ab und gibt anforderungsgebundene Seitensperren frei; untergeordnete Objekte und die Anforderung werden danach zerstört, sobald Referenzen dies erlauben. Neue Puffer- und Parameterzugriffe werden ab Beginn des Abschlusses abgewiesen; bereits erhaltene Pufferzeiger bleiben während des Cleanups nutzbar. Eine externe Objektreferenz erhält den Kontext, aber keinen Zugriff auf das abgeschlossene IRP.

Anforderungsabbruch wird für die oben beschriebenen Steuergerätewarteschlangen modelliert. Ist der Abbruch bereits erfolgt, liefert `WdfRequestMarkCancelableEx` ohne Callback-Aufruf `STATUS_CANCELLED`. Ein erfolgreiches `WdfRequestUnmarkCancelable` entfernt den Callback; ein späterer Abbruch vermerkt nur noch den Abbruchzustand. `WdfRequestIsCanceled` liest diesen Zustand bei einer lebenden, nicht als abbrechbar markierten Anforderung. Nach erfolgreicher Markierung setzt der Abschluss erfolgreiches Entfernen der Markierung oder den Beginn der Abbruchcallback-Zustellung voraus; bloße Einreihung reicht nicht. Nach Zustellungsbeginn können Callback und Work Item den Abschluss koordinieren, auch wenn der Callback wartet. Eine separate interne Referenz hält die Anforderung bis zur Callback-Rückkehr. Beim Abschluss wird das IRP dennoch zuerst ungültig; auch die abschließende Destroy-Fortsetzung darf warten. DPCs haben Vorrang, danach folgen Abbruchcallbacks in FIFO-Reihenfolge und gewöhnliche Work Items. Abbruchcallbacks werden auch vor der Wiederaufnahme bereiter passiver Wartekontexte zugestellt.

Bei bereits abgebrochenen Anforderungen führt das alte void-API `WdfRequestMarkCancelable` vor seiner Rückkehr einen synchronen Gast-Abbruchcallback aus. Diese untergeordnete Fortsetzung kann warten, über verschachteltes Cleanup abschließen und vor Wiederaufnahme des API die endgültige Zerstörung ausführen. Späterer Abbruch nach der Registrierung nutzt den oben beschriebenen Scheduler-Pfad. Dies bildet den öffentlichen Quellcode bei `PASSIVE_LEVEL` und `WdfSynchronizationScopeNone` ab; [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) verlangt für Treiber ohne automatische Synchronisierung Ex. Die Ausführung ist Kompatibilitätsverhalten, keine Empfehlung für dieses API in dieser Konfiguration.

`WdfRequestGetInformation` und `WdfRequestSetInformation` teilen das originale 64-Bit-Feld `IRP.IoStatus.Information`, einschließlich direkter Gastschreibzugriffe. Set weist nur zu; die Transferlänge wird beim Abschluss geprüft. `WdfRequestCompleteWithInformation` schreibt dasselbe Feld vor dem Cleanup; Änderungen über ein zuvor gespeichertes IRP im Cleanup bestimmen die abschließende Information, obwohl GetInformation dann bereits 0 liefert. `WdfRequestGetIoQueue` liefert die Ursprungswarteschlange. Mit der Standarddateikonfiguration liefert `WdfRequestGetFileObject` NULL und erfindet kein WDF-Dateiobjekt aus dem WDM FILE_OBJECT. `WdfRequestWdmGetIrp` liefert dasselbe IRP; Gastaufrufe von `IoCompleteRequest`/`IofCompleteRequest` dürfen den WDF-Abschluss nicht umgehen. Solange der Handle während oder nach Abschluss gültig bleibt, liefern GetInformation/GetIoQueue den Wert 0; MDL-Abruf setzt einen gültigen Ausgabeplatz zuerst auf NULL und liefert dann `STATUS_INTERNAL_ERROR`. SetInformation/GetFileObject/WdmGetIrp bleiben dann abgewiesen. Bestehende Puffer- und Parameterzugriffsregeln ändern sich nicht.

`WdfRequestRetrieveInputWdmMdl` und `WdfRequestRetrieveOutputWdmMdl` beschreiben bei Bedarf den vorhandenen SystemBuffer für gepufferte WRITE-Eingabe, READ-Ausgabe und IOCTL-Ein-/Ausgabe. Jeder Abruf prüft zuerst gültige Richtung und positive Länge, dann verwendet er den einen gecachten Deskriptor der Anforderung. Der erste Erfolg legt ByteCount fest, auch bei anderer logischer Länge der Gegenrichtung. `MmGetSystemAddressForMdlSafe` liefert dafür die ursprüngliche VA; zusätzliches Mapping, Unmapping und Treiberfreigabe werden abgewiesen. Direkte READ-/IOCTL-Ausgabe und WRITE-Eingabe liefern dagegen das vorhandene `IRP.MdlAddress`, ohne durch den Abruf ein Mapping zu erzeugen. Direkte IOCTL-Eingabe nutzt den SystemBuffer-Cache. Deskriptoren, IRP und Puffer werden beim Abschluss ungültig. Interne Abbruch- und externe Referenzen erhalten nur den WDF-Kontext. Die WDF-MDL-Abfrage für `METHOD_NEITHER` bleibt unmodelliert; anforderungsgebundenes WDFMEMORY nutzt eine separate Abbildung gesperrter Benutzerseiten. PFN-Arrays aufgebauter Deskriptoren geben Modellidentitäten schreibgeschützt frei; PFN-Zugriffe nicht aufgebauter Deskriptoren werden abgewiesen.

Bei gepufferten, direkten und neither KMDF-Anforderungen läuft `WdfDeviceInitSetIoInCallerContextCallback` vor der Warteschlange im Prozess des Antragstellers auf `PASSIVE_LEVEL`. Der Callback muss die Anforderung abschließen oder sie einmal mit `WdfDeviceEnqueueRequest` in die Standardwarteschlange geben. Für `METHOD_NEITHER`-IOCTL und neither READ/WRITE liefern `WdfRequestRetrieveUnsafeUserInputBuffer` und `WdfRequestRetrieveUnsafeUserOutputBuffer` die ursprünglichen Benutzeradressen nur in diesem Callback. `WdfRequestProbeAndLockUserBufferForRead` und `WdfRequestProbeAndLockUserBufferForWrite` prüfen Seitenrechte und sperren anforderungsgebundene Seiten. `WdfMemoryGetBuffer` liefert einen Systemalias, der auch im Queue-Callback außerhalb des Antragstellerkontexts gültig bleibt. Beim Abschluss werden Sperren und Aliasse freigegeben. Eingebettete Benutzerzeiger und beliebige Benutzermappings bleiben unmodelliert.

`WdfRequestRetrieveInputMemory` und `WdfRequestRetrieveOutputMemory` stellen anforderungsgebundene WDFMEMORY-Sichten auf vorhandene gepufferte oder direkte E/A-Puffer bereit. Wiederholter Abruf derselben Richtung behält das Handle; `WdfMemoryGetBuffer` liefert den ursprünglichen Puffer und seine logische Länge. Länge null oder falsche Richtung ergibt einen WDF-Fehlerstatus; neither-E/A benötigt weiterhin Prüfung und Seitensperre im Aufruferkontext. Die geliehenen Sichten fügen keine MDL-Sperre hinzu und verfallen beim Abschluss der Anforderung.

Ein begrenztes KMDF-PnP-Profil unterstützt ein direktes FDO/PDO-Paar. `EvtDriverDeviceAdd` erhält einen Framework-Initializer; `WdfDeviceCreate` erzeugt das FDO, und `WdfFdoInitWdmGetPhysicalDevice` sowie `WdfDeviceWdmGetPhysicalDevice` liefern die PDO-Identität. Nach fehlgeschlagenem AddDevice oder abgeschlossenem Remove führt das Framework Cleanup-/Destroy-Callbacks aus und löscht das Gerät. PnP-Queues unterstützen `WdfUseDefault` und `WdfTrue` für die Energieverwaltung; `WdfFalse` bleibt ohne automatische Verwaltung. `WdfDeviceInitSetPnpPowerEventCallbacks` unterstützt `EvtDevicePrepareHardware`, `EvtDeviceD0Entry`, `EvtDeviceD0Exit` und `EvtDeviceReleaseHardware`. PrepareHardware erhält vor D0Entry zwei unterschiedliche Ressourcenlisten, die bei Geräten ohne Ressourcen leer sind; Bei leeren Listen liefert `WdfCmResourceListGetCount` null und `WdfCmResourceListGetDescriptor` NULL. Ein Fehler in PrepareHardware oder D0Entry wird zum START-Status; ReleaseHardware läuft auch nach einem solchen Fehler. Bei STOP oder Entfernen aus D0 läuft D0Exit vor ReleaseHardware und dem IRP-Abschluss. Konfigurierte `register_bank`-Geräte zeigen schreibgeschützte `CM_PARTIAL_RESOURCE_DESCRIPTOR`-Einträge bis ReleaseHardware; übersetzten Speicher kann der Treiber mit `MmMapIoSpace` abbilden und muss ihn vor dem Abschluss von STOP oder Remove freigeben. Andere Ressourcentypen und allgemeine Power-Policy fehlen. `WdfIoQueuePnpHeld` zeigt die Lieferpause vor D0 und nach dem Verlassen von D0; Vor dem Verlassen von D0 erhält jede vom Treiber gehaltene Anforderung einer energieverwalteten Queue `EvtIoStop`: Der Treiber kann sie abschließen, `WdfRequestStopAcknowledge` aufrufen oder eine bereits geplante Verarbeitung abschließen lassen. Ohne registriertes `EvtIoStop` wartet das Framework ebenfalls auf alle zugestellten Anforderungen. TRUE stellt sie für die erneute Zustellung nach dem Neustart zurück; FALSE behält den Besitz beim Treiber und ruft nach D0-Eintritt `EvtIoResume` auf. Bereits eingereihte Anforderungen warten bis zum Neustart. Nur wenn keine Abschlussquelle mehr vorhanden ist, endet das Warten mit einem angehaltenen `model_error`. Das WDK-Fixture `driver_kmdf_pnp.c` nutzt `NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE`.

Modellierte KMDF-APIs: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfWdmDeviceGetWdfDeviceHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetDeviceType`, `WdfDeviceInitSetExclusive`, `WdfDeviceInitSetFileObjectConfig`, `WdfDeviceInitSetIoType`, `WdfDeviceInitSetIoInCallerContextCallback`, `WdfDeviceInitSetPnpPowerEventCallbacks`, `WdfCmResourceListGetCount`, `WdfCmResourceListGetDescriptor`, `WdfFdoInitWdmGetPhysicalDevice`, `WdfFdoInitSetFilter`, `WdfDeviceCreate`, `WdfDeviceEnqueueRequest`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfDeviceWdmGetAttachedDevice`, `WdfDeviceWdmGetPhysicalDevice`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfDeviceGetDriver`, `WdfDeviceGetIoTarget`, `WdfIoQueueGetDevice`, `WdfIoQueueGetState`, `WdfIoQueueStop`, `WdfIoQueueStopSynchronously`, `WdfIoQueueStart`, `WdfIoQueueDrain`, `WdfIoQueueDrainSynchronously`, `WdfIoQueuePurge`, `WdfIoQueuePurgeSynchronously`, `WdfIoQueueStopAndPurge`, `WdfIoQueueStopAndPurgeSynchronously`, `WdfIoQueueReadyNotify`, `WdfIoQueueRetrieveNextRequest`, `WdfIoQueueRetrieveRequestByFileObject`, `WdfIoQueueFindRequest`, `WdfIoQueueRetrieveFoundRequest`, `WdfRequestForwardToIoQueue`, `WdfRequestRequeue`, `WdfRequestStopAcknowledge`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestSend`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputMemory`, `WdfRequestRetrieveOutputMemory`, `WdfRequestRetrieveUnsafeUserInputBuffer`, `WdfRequestRetrieveUnsafeUserOutputBuffer`, `WdfRequestProbeAndLockUserBufferForRead`, `WdfRequestProbeAndLockUserBufferForWrite`, `WdfMemoryGetBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfFileObjectGetFileName`, `WdfFileObjectGetFlags`, `WdfFileObjectGetDevice`, `WdfFileObjectWdmGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

Für ein PnP-FDO speichert `WdfDeviceInitSetDeviceType` den angegebenen 32-Bit-Typ im WDM-`DEVICE_OBJECT`; ohne Aufruf bleibt `FILE_DEVICE_UNKNOWN` der Standardwert. Vom Gerätetyp abhängige E/A-Prioritätsanhebungen werden nicht modelliert.

`WdfDeviceInitSetExclusive` setzt `DO_EXCLUSIVE` auf dem aus dem Initialisierer erzeugten WDM-Gerät. Ein benanntes Steuergerät weist einen zweiten unabhängigen Öffnungsversuch zurück, bis die erste Datei geschlossen ist. Bei einem PnP-FDO macht dieses Flag allein weder den benannten PDO noch den gesamten Gerätestapel exklusiv; INF-gesteuerte PDO-Exklusivität liegt außerhalb dieses Profils.

`WdfDeviceInitSetFileObjectConfig` registriert vor der Geräteerstellung `EvtDeviceFileCreate`, `EvtFileCleanup` und `EvtFileClose` und kopiert optionale Kontextattribute. Dieses Profil unterstützt `WdfFileObjectNotRequired`, `WdfFileObjectWdfCanUseFsContext`, `WdfFileObjectWdfCanUseFsContext2` und `WdfFileObjectWdfCannotUseFsContexts`. Der ausgewählte WDM-Kontextplatz hält das WDF-Handle bis zum fehlgeschlagenen CREATE oder CLOSE und muss anfangs leer sein. Mit `WdfFileObjectCanBeOptional` liefert `WdfRequestGetFileObject` für I/O ohne passendes WDM-Dateiobjekt NULL. CREATE, CLEANUP und CLOSE benötigen weiterhin ein WDM-Dateiobjekt; `WdfFdoInitSetFilter` aktiviert die Standardweiterleitung für Filter; `WdfTrue` aktiviert und `WdfFalse` deaktiviert sie ausdrücklich. Für die Weiterleitung von Dateianforderungen sind eine direkte FDO/PDO-Route und ein explizites synchrones `bus_completion` nötig. CLEANUP- und CLOSE-Rückrufe laufen vor der Weitergabe an das untere Gerät. Ein CREATE-Rückruf mit `WdfFileObjectNotRequired` kann das geräteeigene lokale Ziel über `WdfDeviceGetIoTarget` beziehen und die ursprüngliche Anforderung mit `WdfRequestSend` ausschließlich mit `WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET` weiterleiten. Das beibehaltene PDO verbraucht die explizite synchrone Busantwort und übernimmt den endgültigen Abschluss. CREATE-Rückrufe mit Framework-Dateiobjekt und andere Sendeoptionen bleiben ununterstützt. WDF-Dateihandle und WDM-`FILE_OBJECT` bleiben getrennte Identitäten, die über `WdfRequestGetFileObject`, `WdfFileObjectGetDevice` und `WdfFileObjectWdmGetFileObject` abfragbar sind. Ein fehlgeschlagenes CREATE löscht das WDF-Dateiobjekt ohne Datei-Cleanup- oder Close-Rückruf; nach erfolgreichem Öffnen laufen CLEANUP und CLOSE vor der Kontextbereinigung und Zerstörung.

Die optionale echte WDK-Prüfung kompiliert `driver_kmdf_lifecycle.c` und `driver_kmdf_control.c` separat mit der wirklichen KMDF-Einstiegsbibliothek. `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` wählen Lebenszyklus-Images; `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` wählen normale beziehungsweise aktive CFG-Images für Steuergeräte. Fehlende externe Artefakte werden explizit übersprungen. Native und C-API-/CLI-Abdeckung ist unter [Tests](testing.md) beschrieben. Die Ausführungsnachweise sind derzeit auf Linux-Hosts begrenzt.

Das anfängliche API-Modell besitzt bewusst einen begrenzten Vertrag:

| APIs | Modelliertes Verhalten und Einschränkungen |
|------|-------------------------------------------|
| `RtlInitUnicodeString` | Erstellt eine Gast-`UNICODE_STRING` für eine begrenzte NUL-terminierte Quelle |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | Längengezähltes UTF-16-Kopieren und Vergleich unter Beachtung der Groß-/Kleinschreibung; Vergleich ohne Beachtung der Groß-/Kleinschreibung benötigt eine Windows-Tabelle zur Groß-/Kleinschreibung und stoppt |
| `ExRaiseStatus`, `ExRaiseAccessViolation`, `ExRaiseDatatypeMisalignment` | Lösen eine Gastausnahme für unterstützte konstante C-`__except`-Handler aus; keine normale API-Rückkehr, Filter/finally und Wiederherstellung nach CPU-Fehlern bleiben unmodelliert |
| `ExAllocatePool2` | Auslagerbare/nicht auslagerbare NX-Allokationen, standardmäßig genullt; Flags für nicht initialisierte und cacheausgerichtete Allokationen modelliert; ungültige erforderliche Flags liefern NULL, Quota-/ausführbare Pools und ausgelöste Allokationsausnahmen stoppen |
| `MmGetSystemRoutineAddress` | Löst einen längengezählten Gastnamen über den gemeinsamen Exportkatalog auf |
| `MmMapIoSpace`, `MmMapIoSpaceEx`, `MmUnmapIoSpace` | Deklarierte übersetzte Teilbereiche; NonCached RO/RW, gemeinsame Aliase und exaktes Unmap; kein beliebiger physischer Speicher |
| `IoConnectInterrupt`, `IoDisconnectInterrupt`, `IoConnectInterruptEx`, `IoDisconnectInterruptEx` | Exakt zugewiesene exklusive Latched-Leitung, ältere ABI und Ex-Versionen 1/2/4 auf PASSIVE_LEVEL; opake Verbindung und genaue Generationslaufzeit |
| `KeSynchronizeExecution`, `KeAcquireInterruptSpinLock`, `KeReleaseInterruptSpinLock` | Echter BOOLEAN-Synchronisationscallback und dieselbe nichtrekursive Sperre auf zugewiesenem DIRQL; IRQL und Besitz des ursprünglichen Aufrufers wiederhergestellt |
| `IoGetDmaAdapter` | Explizite Internal-Busmaster-Beschreibung Version 0/1 und gebundene Operationstabelle Version 1 bei PASSIVE_LEVEL; neuere Versionsabfragen liefern NULL |
| `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `PutDmaAdapter` | Adaptertabellenmethoden über gemeinsamem kohärentem RAM, exakte Zuweisungsidentität und unabhängige Adapterlebensdauer |
| `GetScatterGatherList`, `PutScatterGatherList` | Tabellenmethoden bei DISPATCH_LEVEL; echter Inline- oder ressourcenwartender Callback, festgehaltene MDL-Ansicht und explizite Abbildungsfreigabe |
| `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers`, `FreeMapRegisters` | Übersetzte Busmaster-Kanalcallbacks, gemeinsames Registerkontingent, zusammenhängende MDL-Fragmente, gemeinsamer Flush und exakte Freigabe behaltener Register |
| `KeFlushIoBuffers` | Kohärenter CPU-Cache-Flush über einen gültigen gesperrten/nicht auslagerbaren MDL, ohne DMA-Abbildungen freizugeben |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | Expliziter sitzungslokaler Registry-Baum, Rechte pro Handle, Abfragen mit exakten Ausgabegrößen und Lebensdauer nach dem Löschen; siehe Registry-Szenarien |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | Anforderungseigene MDLs mit gecachten KernelMode-Abbildungen und Rechten; MDLs für nicht auslagerbaren Pool verwenden über den sicheren Helfer die ursprüngliche Abbildung |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | Eigenständige Deskriptoren; vollständiger Bereich innerhalb einer gültigen Allokation im nicht auslagerbaren Pool; unabhängige Lebensdauer von Deskriptor und Puffer; ohne IRP-Zuordnung, Ketten oder Kontingente |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Datenallokationen für Pooltypen `0`, `1` und `512`; positive Größe/Tag, passende Tags beim Freigeben, keine Wiederverwendung von Adressen |
| `IoCreateDevice`, `IoDeleteDevice` | Gerätetyp `0x22`, Characteristics `0` oder `0x100`, begrenzte Erweiterungen, ASCII-Namen der Form `\Device\Name` |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | Anbindung desselben Treibers; gibt vorheriges oberstes Gerät zurück, Trennen erhält gespeichertes unteres Gerät; obige Topologie-/Lebensdauergrenzen gelten |
| `IofCallDriver`, `IoCallDriver` | Genaues Ziel auf gehaltener Route; geprüfter Gastcursor und separater unterer NTSTATUS |
| `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx` | Genauer Eigentümer in Erweiterung, opake32/120 Bytes, NULL/wiederholte Tags |
| `IoReleaseRemoveLockEx`, `IoReleaseRemoveLockAndWaitEx` | Passendes Release und fortsetzbares REMOVE-Warten nach Providerempfang |
| `PoCallDriver`, `PoStartNextPowerIrp` | Verwaltete Power-IRP weiterleiten; Vista+ ohne zusätzlichen Handshake |
| `PoSetPowerState`, `PoRequestPowerIrp` | Separate Gerätebenachrichtigung und echte Kinder aus expliziten PDO-FIFOs; Grenzen oben |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` oder `\??\Name` in einem Sitzungsnamensraum, Ziel `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Geprüfte variadische Win64-Formatierung, höchstens 512 Ausgabebytes; alle Debuggerfilter aktiviert |
| `IoGetCurrentIrpStackLocation` | Gibt die Stackposition des aktiven modellierten IRP zurück; normale kompilierte WDM-Makros lesen dasselbe Gastfeld |
| `KeGetCurrentIrql` | Liest den aktuellen IRQL/CR8 einschließlich expliziter Anhebungen und Wiederherstellungen; Dispatch und Work Items beginnen bei `PASSIVE_LEVEL`, DPCs bei `DISPATCH_LEVEL` |
| `KfRaiseIrql`, `KeLowerIrql` | Echte x64-WDK-Importe zum Anheben/Senken; gespeicherte IRQL-Werte müssen auf derselben Ausführung in LIFO-Reihenfolge vor Rückkehr oder Unterbrechung wiederhergestellt werden. CR8-Lesezugriffe sehen jede Änderung; Unterbrechung auf Befehlsebene wird nicht simuliert. |
| `KeInitializeSpinLock`, `KeAcquireSpinLockRaiseToDpc`, `KeReleaseSpinLock`, `KeAcquireSpinLockAtDpcLevel`, `KeReleaseSpinLockFromDpcLevel`, `KeTryToAcquireSpinLockAtDpcLevel` | Residente, ausgerichtete Executive-Spinlocks auf CPU0; Eigentümer, passendes Freigabeverfahren und Wiederherstellung des IRQL werden geprüft. Bei Konkurrenz stoppt ein blockierender Erwerb ausdrücklich. |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | Geräteeigene undurchsichtige Work Items; nur `DelayedWorkQueue`, Gerät und Kontext werden auf `PASSIVE_LEVEL` übergeben; eingereihte Einträge dürfen nicht freigegeben werden |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | Opaker DPC, vier Gastargumente, `DISPATCH_LEVEL`, Duplikat-/Entfernungsregeln und Wichtigkeit; nur Ziel CPU0 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | Benachrichtigungs-/Synchronisationstimer, relative/absolute 100-ns-Fristen, Millisekundenperioden, Neusetzen/Abbruch und Signalabfrage in virtueller Zeit |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | Benachrichtigungs-/Synchronisationsereignisse mit unterschiedlichem Signalverbrauch; `KeSetEvent` nur mit Increment=0 und Wait=FALSE |
| `KeInitializeSemaphore`, `KeReleaseSemaphore`, `KeReadStateSemaphore` | Residenter Zählsemaphor mit positiver Grenze; jedes erfolgreiche Warten verbraucht eine Einheit. Freigabe mit Increment=0 und Wait=FALSE; Überschreitung löst `STATUS_SEMAPHORE_LIMIT_EXCEEDED` aus. |
| `KeInitializeMutex`, `KeReleaseMutex`, `KeReadStateMutex` | Residenter KMUTEX mit rekursivem Besitz durch eine Ausführung; KeReleaseMutex liefert den vorherigen vorzeichenbehafteten Signalzustand zurück, verlangt Besitzer und passenden DISPATCH_LEVEL-Kontext und akzeptiert nur Wait=FALSE. Gehaltener Besitz verhindert Rückkehr, Neuinitialisierung und Freigabe des Speichers. Die Freigabe durch einen anderen Besitzer löst `STATUS_MUTANT_NOT_OWNED` aus. |
| `PsCreateSystemThread`, `PsTerminateSystemThread`, `ObReferenceObjectByHandle`, `ObfDereferenceObject`, `ZwClose` | Begrenzte Systemprozess-Threads bei PASSIVE_LEVEL. Kernel-Handle und Referenz auf das opake Threadobjekt haben getrennte Lebensdauern; PsTerminateSystemThread beendet die Gastausführung ohne Rückkehr und signalisiert das wartbare Objekt. APCs, Prioritäten und typisierte Objektreferenzen sind nicht modelliert. |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeEnterGuardedRegion`, `KeLeaveGuardedRegion`, `KeAreApcsDisabled`, `KeAreAllApcsDisabled` | Verschachtelter APC-Sperrzustand je Thread. Kritische Bereiche und ein gehaltener KMUTEX sperren normale APCs; geschützte Bereiche und IRQL >= APC_LEVEL sperren alle. Systemthreads starten in einem kritischen Bereich. Unpaariges Verlassen und Rückkehr mit offenem Bereich schlagen fehl; APC-Zustellung ist nicht modelliert. |
| `KeWaitForSingleObject` | Ein initialisiertes Ereignis, Timer, Semaphor oder Mutex; nicht alertable `KernelMode`, Grund `Executive`; Null-Polling, endliche relative/absolute oder unbegrenzte Wartezeit; Nichtnull-/unbegrenztes Warten erfordert IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | Nicht alertable relative/absolute `KernelMode`-Verzögerung bei IRQL <= APC_LEVEL; Gastframe wird nach virtuellem Zeitfortschritt fortgesetzt |
| `IoMarkIrpPending` | Markiert das aktive lebende IRP; der entsprechende Schreibzugriff des WDM-Makros auf das Stack-Control-Feld wird ebenfalls modelliert; Dispatch muss `STATUS_PENDING` zurückgeben |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT`; führt Completion-Abwicklung mit Stop/Fortsetzung aus und gibt IRP/MDL/Puffer erst an der Endgrenze frei |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Begrenzte Gastpufferoperationen, höchstens 1 MiB pro Aufruf; Kopier-APIs ohne Überlappungsunterstützung weisen Überlappungen ab |

IRQL-Obergrenzen stehen in `KernelAPIIRQL.def`; argumentabhängige Regeln prüft das zuständige Modell. DPCs dürfen weder Registry-APIs aufrufen noch paged Pool allokieren, freigeben oder darauf zugreifen. Unicode-Konvertierungen von `DbgPrint` erfordern `PASSIVE_LEVEL`; unterstützte ANSI-Ausgabe und nonpaged Operationen bleiben auf `DISPATCH_LEVEL` nutzbar. Callback-Stacks sind begrenzt: Ein ausbrechender Stackpointer darf nicht den Stack eines anderen blockierten Workers erreichen. Aktive Timer in der Geräteerweiterung verhindern vorzeitige Gerätefreigabe.

Der Timerablauf erfüllt registrierte Warteoperationen, bevor ein DPC den Timer zurücksetzen oder neu starten kann. Eingereihte DPCs laufen vor dem Fortsetzen aufgeweckter `PASSIVE_LEVEL`-Frames. Enthält Anforderungsspeicher noch einen eingereihten DPC, verweigert der IRP-Abschluss die Freigabe vor Abschluss und Pufferinvalidierung.

Die Formatierung von `DbgPrint` unterstützt Ganzzahlen `d/i/u/o/x/X`, Zeiger
`p`, Text `s/c`, `%%`, längengezähltes Unicode `wZ/lZ`, breite Zeichenfolgen
`ls/ws`, Flags, Breite/Präzision einschließlich `*` sowie Windows-Längenmodifikatoren
für Ganzzahlen. Es werden höchstens 32 variable Argumente und 1024 Formatbytes
gelesen. Breite und Präzision sind auf 512 begrenzt. Gleitkomma, `%n`, unbekannte
Kombinationen und Nicht-ASCII-Textkonvertierungen stoppen ausdrücklich; das Modell
errät keine Windows-Codepage und ruft kein Host-printf mit Gastdaten auf.

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

Gerätename und IOCTL-Code müssen zum Treiber passen. Bei create wird ohne
`device` das einzige aktive Gerät ausgewählt; mehrdeutige Auswahl scheitert.
Spätere Anforderungen verwenden das Gerät ihrer Datei, sofern kein expliziter,
passender Name angegeben ist. Das optionale `file` ist eine vorzeichenlose
32-Bit-Szenarioidentität, standardmäßig null. Jede Identität besitzt ein eigenes
FILE_OBJECT und FsContext und verlangt create, Transfers, cleanup und close in
dieser Reihenfolge. Anforderungen unabhängiger Dateien dürfen ineinander
verschachtelt werden. Exklusive Geräte weisen ein zweites Öffnen ab. Diese
Identitäten stehen für Dateiobjekte, nicht für duplizierte Handles. Gepufferte
und beide direkten IOCTL-Methoden werden unterstützt. Dispatch muss synchron abschließen oder den oben beschriebenen Vertrag für ausstehende IRPs mit Callbacks erfüllen. Ungültige Ausgabelängen und Zugriffe auf abgeschlossene IRPs führen ausdrücklich zu Fehlern. Angefordertes Entladen darf keine aktiven Geräte, symbolischen Links,
Poolallokationen oder Dateiobjekte zurücklassen.

Das optionale Wurzelfeld `"load_address": "0x190000000"` fordert eine
Basisverschiebung an; ohne dieses Feld oder mit `"0x0"` gilt die bevorzugte
Adresse. Das Image muss die Relokationsanforderungen erfüllen. Der ursprüngliche
Initialisierungsbefehl und die C-API implizieren kein Szenario.

An der Wurzel sind nur `load_address`, `requests`, `unload`, `kernel_exports`, `registry` und `pnp_devices`
erlaubt. Gewöhnliche Dateianforderungen akzeptieren `kind`, optional `device` oder `device_id` (gegenseitig exklusiv) und optional `file`. IOCTLs benötigen `code` und akzeptieren `input`, `output_size` und
`direct_input`. Ein `read` akzeptiert `output_size` und `byte_offset`; ein
`write` akzeptiert `input` und `byte_offset`. Offsets sind standardmäßig null,
akzeptieren Ganzzahlen oder Hexadezimalzeichenfolgen und müssen in einen
nichtnegativen vorzeichenbehafteten 64-Bit-Wert passen. Lebenszyklusanforderungen
weisen Transferfelder ab. Unbekannte oder doppelte Felder werden abgewiesen.
`code` akzeptiert eine vorzeichenlose 32-Bit-JSON-Ganzzahl oder eine
Hexadezimalzeichenfolge mit `0x`. `input` ist eine Hexadezimal-Bytezeichenfolge
gerader Länge ohne Präfix oder Leerzeichen; Weglassen bedeutet leere Eingabe.
`output_size` ist eine vorzeichenlose JSON-Ganzzahl; Weglassen bedeutet null.
Brüche und Gleitkommaschreibweisen werden abgewiesen.

Nur READ-/WRITE-/IOCTL-Anforderungen akzeptieren das optionale Feld `cancel_after_100ns`, eine JSON-Ganzzahl zwischen 0 und `INT64_MAX` (9223372036854775807). Die Verzögerung zählt ab Anforderungsübergabe in virtuellen 100-ns-Einheiten, nicht in Echtzeit. Bei KMDF löst Null den Abbruch nach Framework-Routing und vor dem Gast-I/O-Callback aus; hat das Routing bereits abgeschlossen, gewinnt der Abschluss. Bei WDM gilt Null nach der Rückkehr aus dem Dispatch. Bei positiven Verzögerungen schreitet die Zeit nur dann zu Timer-, Warte- oder Abbruchfristen fort, wenn kein Callback oder Ausführungskontext bereit ist. Ein ausstehender WDM-IRP ruft seine registrierte Abbruchroutine auf `DISPATCH_LEVEL` mit gehaltenem Abbruch-Spinlock auf; vor Abschluss muss sie ihn mit `Irp->CancelIrql` freigeben. Allgemeiner Warteschlangen- und PnP-Abbruch bleibt unmodelliert. Jeder Anforderungsbericht enthält `cancel_requested_at_100ns`: die tatsächliche absolute virtuelle Abbruchzeit oder null, wenn kein Abbruch erfolgte, auch bei vorherigem Abschluss. Eine Abbruchanforderung allein schließt kein IRP ab und legt seinen Endstatus nicht fest.
`IoSetCancelRoutine`, `IoAcquireCancelSpinLock`, `IoReleaseCancelSpinLock` und `IoCancelIrp` verwenden denselben IRP-Zustand und dieselbe Abbruch-Sperre; `IoCancelIrp` ruft eine registrierte Routine synchron auf und meldet, ob sie ausgeführt wurde.

Das optionale boolesche Feld `user_unmap_after_dispatch` entzieht bei einem nichtleeren WDM-Neither-Transfer den ursprünglichen Benutzeradressen nach der Dispatch-Rückkehr und vor geplanter Arbeit oder einem Abbruch den Zugriff. Gesperrte MDL-Seiten und ihre Systemaliase bleiben bis zum Entsperren nutzbar; rohe Benutzerzeiger und neue Sperren schlagen fehl. Bei entzogenem Ausgabezugriff bleibt `output_hex` leer. Wiederverwendung und beliebige Zeitpunkte der Freigabe sind nicht modelliert.

`requestor_process_id` bezeichnet einen synthetischen Anforderungsprozess (Standard 4096, Bereich 5 bis `UINT32_MAX`). `IoGetRequestorProcessId` liefert seine Kennung für eine aktive IRP; `PsGetCurrentProcessId` liefert sie beim direkten Dispatch oder 4 im modellierten System-Worker. Ein Prozesswechsel sperrt fremde ursprüngliche Benutzeradressen, nicht aber gesperrte MDL-Systemaliasse. `requestor_exit_after_dispatch` sperrt nach der Dispatch-Rückkehr alle ursprünglichen Benutzeradressen dieses Prozesses und lehnt neue I/O ab; explizite CLEANUP/CLOSE bleiben möglich, ohne automatischen Abbruch oder Handle-Rundown.

Ein System-Worker kann mit `IoGetRequestorProcess` das opake Prozessobjekt einer aktiven IRP erhalten, mit `KeStackAttachProcess` und beschreibbarem Kernel-`KAPC_STATE` vorübergehend auf deren ursprüngliche Benutzeradressen zugreifen und mit demselben Zustand `KeUnstackDetachProcess` aufrufen. `IoGetCurrentProcess` und `PsGetProcessId` zeigen den angehängten Prozess; `PsGetCurrentProcessId` bleibt 4, die Kennung des Worker-Erzeugers. Beendete Prozesse, abgeschlossene IRPs, ein falscher Zustand sowie Warten oder IRP-Abschluss während des Anhängens schlagen ausdrücklich fehl.

Bei direkten IOCTLs initialisiert `input` den ersten Systempuffer, während
`direct_input` den separaten, durch die MDL beschriebenen zweiten Puffer
initialisiert und bis `output_size` mit Nullen ergänzt wird. `METHOD_IN_DIRECT`
erfordert Lesezugriff; daraus folgt keine schreibgeschützte Systemabbildung.
Beide Methoden verwenden lesbare/schreibbare Szenariopuffer. `MdlMappingNoWrite`
entfernt Schreibrechte, `MdlMappingNoExecute` Ausführungsrechte der Abbildung.
Das Aufheben der Abbildung entzieht die System-VA; erneutes Abbilden erhält
dieselben gesperrten Daten. Abschluss beendet die Gültigkeit von MDL und Abbildung.
Die von WDM-Makros verwendeten öffentlichen MDL-Felder sind modelliert;
Prozessfelder, PFNs nicht aufgebauter Deskriptoren, selbst erstellte MDLs, Benutzerabbildungen und direkter
Zugriff über den rohen UserBuffer werden abgewiesen. Ein direkter Puffer der
Länge null hat eine Null-MDL.

`IoAllocateMdl` allokiert eigenständige Metadaten für einen nicht leeren, nicht überlaufenden Puffer von höchstens 1 MiB; es prüft oder sperrt diesen Puffer nicht. `Irp` muss NULL sein, `SecondaryBuffer` und `ChargeQuota` müssen FALSE sein. Bei erschöpfter Arena wird NULL zurückgegeben. `MmBuildMdlForNonPagedPool` verlangt, dass der gesamte beschriebene Bereich innerhalb einer einzigen gültigen Allokation im nicht auslagerbaren Pool liegt. Der sichere Helfer und normale WDM-Makros verwenden die ursprüngliche Adresse; Aliase und bestehende Rechte bleiben auch bei neuen Schreib-/Ausführungsverboten erhalten. Zusätzliche Systemabbildungen und das Aufheben der Abbildung werden abgelehnt. `IoFreeMdl` beendet nur die Lebensdauer des Deskriptors; der Poolpuffer hat eine eigene Lebensdauer. Beide Freigabereihenfolgen sind zulässig, solange freigegebener Speicher anschließend nicht verwendet wird. Alle modellierten MDL-Felder und aufgebauten PFN-Arrays sind schreibgeschützt; Prozessfelder, nicht aufgebaute PFNs, Ketten und manuelle Feldänderungen bleiben unmodelliert. Beim Entladen müssen alle treibereigenen Deskriptoren freigegeben sein.

Bei jeder IOCTL-Methode mit einem `output_size` ungleich null darf `Information`
den Wert `output_size` nicht überschreiten, auch wenn der Eingabepuffer größer
ist. Ohne Ausgabepuffer darf `Information` ein IOCTL-spezifisches 64-Bit-Ergebnis
enthalten; es werden keine Ausgabebytes kopiert. Der Bericht erhält den exakten
Wert in `information_hex`.

Für READ/WRITE wählt `DO_BUFFERED_IO` oder `DO_DIRECT_IO` den gepufferten oder
direkten Transfer. Fehlen beide Flags, steht die ursprüngliche Benutzeradresse
nur in `IRP.UserBuffer`: WRITE verwendet Eingabe, READ Ausgabe. SystemBuffer
oder MDL entstehen nicht automatisch. Der Treiber muss die Adresse im
Aufruferkontext prüfen und verwenden oder die Seiten vor verzögerter Arbeit
sperren. `user_input_access` gilt für neither WRITE und `user_output_access`
für neither READ. Explizite Rechte bei gepufferten/direkten READ/WRITE oder
widersprüchliche Flags stoppen. Information wird gegen die Transferlänge
geprüft; Schreiboperationen liefern eine Anzahl, Leseoperationen Bytes.

`kernel_exports` ordnet Routinenamen explizite Verfügbarkeits-Boolesche Werte
zu, etwa `"kernel_exports": {"OptionalRoutine": false}`. Modellierte Exporte und
statische Importe erhalten stabile Adressen, die auch `MmGetSystemRoutineAddress`
verwendet. Ein ausdrücklich fehlender Export wird zu NULL aufgelöst und kann
keinen statischen Import erfüllen. Ein als vorhanden deklarierter Export ohne
API-Modell wird an einen erst bei Nutzung auslösenden Trap gebunden. Ein
unbekannter dynamischer Name stoppt mit einer Diagnose zur nicht festgelegten
Verfügbarkeit; aus fehlender Implementierung wird nie Abwesenheit abgeleitet.
Namen bestehen aus begrenztem druckbarem ASCII; die Auflösung beachtet
Groß-/Kleinschreibung. Der Katalog ist eine konkrete Szenarioeigenschaft und
behauptet keine Übereinstimmung mit jeder Windows-Version.
`IoMarkIrpPending`, `IoGetCurrentIrpStackLocation` und `MmGetSystemAddressForMdlSafe` sind modellierte Hilfsfunktionen aus WDM-Headern; dadurch werden sie nicht standardmäßig als Exporte deklariert, weshalb ihre Exportverfügbarkeit einen statischen Import oder eine explizite `kernel_exports`-Deklaration erfordert.

Der Szenariotext ist auf 2 MiB begrenzt: höchstens 64 Anforderungen, höchstens
65536 Byte pro Eingabe- oder Ausgabepuffer und höchstens 512 KiB angeforderte
Bytes insgesamt, einschließlich des Inhalts von `direct_input`. Instruktions-,
Beobachtungs-, Gastspeicher- und Zeitbudgets gelten für das gesamte Szenario.
Die 1-MiB-Arena enthält auch Objekte und Metadaten; daher kann ein Image den
Modellspeicher erschöpfen, bevor die maximalen Szenariopuffer verbraucht sind.

## Registry-Szenarien

Das optionale Array `registry` definiert einen konkreten, auf die Sitzung
begrenzten Registry-Baum. Jeder Schlüssel hat ein erforderliches `path` und ein
optionales Array `values`; jeder Wert enthält `name`, den vorzeichenlosen
Ganzzahltyp `type` und hexadezimale `data`. Ein leerer Wertname bezeichnet den
Standardwert. Ein DWORD-Wert lautet beispielsweise
`{"name":"Mode","type":4,"data":"01000000"}`. Wertbytes bleiben unverändert;
das Modell ergänzt keine Zeichenfolgenabschlüsse und expandiert keine
Umgebungsvariablen.

Pfade müssen absolute ASCII-Pfade unter `\Registry\Machine` oder
`\Registry\User` sein. Übergeordnete Schlüssel werden implizit angelegt.
Schlüssel- und Wertidentitäten werden nach ASCII-Regeln ohne Beachtung der
Groß-/Kleinschreibung verglichen; Nicht-ASCII-Namen und doppelte Identitäten
werden abgewiesen. Ohne dieses Array bleibt die Registry-Verfügbarkeit
unbestimmt und Registry-Aufrufe stoppen. `"registry": []` beschreibt ausdrücklich
einen leeren Namensraum. Schlüssel, Werte, Daten der Host-Registry oder
Dienstkonfigurationen werden nicht aus dem Treiber abgeleitet.

`ZwOpenKey` und `ZwCreateKey` liefern unabhängige opake Handles mit eigenen
Zugriffsprüfungen für Abfragen, Setzen von Werten, Anlegen von Unterschlüsseln
und Löschen. Der konfigurierte Baum gewährt die unterstützten
`KEY_ALL_ACCESS`-Bits einschließlich der üblichen Masken `KEY_READ` und
`KEY_WRITE`. Er ist ein ausdrücklich zugänglicher Testbaum ohne Windows-ACLs
oder Privilegienprüfung. Generische Rechte, `MAXIMUM_ALLOWED`, alternative
Registry-Ansichten, eigene Sicherheitsdeskriptoren, Klassen und symbolische
Verknüpfungen werden nicht unterstützt. Relatives Anlegen erfordert ein Handle
des direkten Elternschlüssels mit `KEY_CREATE_SUB_KEY`. Eingabeschlüssel sind
nicht flüchtig; neu angelegte Schlüssel dürfen flüchtig sein. Ein nicht
flüchtiger Unterschlüssel eines flüchtigen Schlüssels wird abgewiesen. Neustarts
und dauerhafte Speicherung auf Datenträgern werden nicht modelliert.

`ZwQueryValueKey` implementiert die Informationsklassen Basic, Full und Partial
sowie deren definierte Align64-Varianten mit exakten Längen, ausgerichteten
Daten, Teilausgaben und unterschiedlichen Ergebnissen für
`STATUS_BUFFER_TOO_SMALL` und `STATUS_BUFFER_OVERFLOW`. `ZwSetValueKey` und
`ZwDeleteValueKey` ändern nur den Baum dieser Sitzung. `ZwDeleteKey` weist einen
Schlüssel mit aktiven Unterschlüsseln ab; Handles eines gelöschten Schlüssels
liefern bis zum Schließen `STATUS_KEY_DELETED`. `ZwClose` gibt ein Handle
unabhängig vom Schlüssel frei. Angefordertes Entladen scheitert, solange
Registry-Handles offen bleiben.

Die Grenzen sind 256 Schlüssel einschließlich übergeordneter Schlüssel,
insgesamt 1024 Werte, 65536 Bytes pro Wert, insgesamt 512 KiB Wertdaten,
1024 ASCII-Bytes pro Schlüsselpfad, 256 Bytes pro Wertname und 256 gleichzeitig
offene Handles. Anlegen und Ändern unterliegen denselben Grenzen wie die
Szenariovorprüfung. `configuration.registry` im Bericht erhält die ursprüngliche
Eingabe; `registry` enthält die am Ende aktiven Schlüsselpfade und Werte,
einschließlich der vor einem Stopp beobachteten Änderungen. Ein nicht
festgelegter Registry-Zustand wird als null ausgegeben. Flüchtigkeit und
Handle-Identitäten sind nicht Teil dieses Wert-Snapshots.

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
MS-COFF-Importbibliothek. Die Prüfung führt separate gepufferte, In-Direct- und
Out-Direct-Szenarien über DriverEntry, create, IOCTL, cleanup, close und unload
aus. Ergänzen Sie `--debug` und wählen Sie ein separates Ausgabeverzeichnis,
um mit `DBG=1` zu kompilieren und Gast-Logmeldungen zu prüfen. Das Originalbeispiel
registriert keinen Cleanup-Handler; deshalb schließt der modellierte
Standardhandler cleanup mit `STATUS_INVALID_DEVICE_REQUEST` (`0xC0000010`) ab.
Der Treiber wird trotzdem geschlossen und entladen, und der erfolgreiche IOCTL
liefert die erwarteten Bytes. Für dieses vollständige Szenario beträgt der
erwartete CLI-Exitcode **2**, und `scenario_success` ist false. Das Skript selbst
ist nur erfolgreich, wenn alle diese Ergebnisse einschließlich des sichtbaren
Cleanup-Fehlers übereinstimmen; es verändert das Beispiel nicht, um dieses
Ergebnis zu verbergen.

## Abnahmeprüfung mit dem Beispiel Zero

Das zusätzliche [Zero-Validierungsskript](../../scripts/validate_zero_driver_sample.py)
übersetzt Pavel Yosifovichs unverändertes öffentliches WDM-Beispiel Zero anhand
der Revision und Hashwerte in
[seinem Manifest](../../unittests/emulation/fixtures/zero-validation.json).
Führen Sie `python3 scripts/validate_zero_driver_sample.py` mit denselben
Werkzeugvoraussetzungen aus. Quelltext, MIT-Lizenz, Befehle, Szenario und Bericht
werden standardmäßig unter `build-release/driver-validation/zero` aufbewahrt.
Neun Anforderungen prüfen direkte Lesezugriffe über Seitengrenzen hinweg,
Schreibmengen, atomare Gaststatistiken und einen gepufferten Statistik-IOCTL.
Der Fehler beim Lesen mit Länge null und der fehlende CLEANUP-Handler des
Beispiels bleiben sichtbar; der erwartete CLI-Exitcode ist 2, bei erfolgreichem
Schließen und Entladen. Das Validierungsskript ist nur erfolgreich, wenn diese
exakten Ergebnisse und sämtliche Ausgabebytes übereinstimmen.

## Berichte und SDK

Der JSON-Bericht unterscheidet `stop_reason`, die nullable Felder `nt_status`
und `nt_success`, den PC beim Anhalten und die Instruktionszahl. Er erhält
die vor dem Stopp gesammelten API-Aufrufe und beobachtbaren Zustände,
einschließlich Geräteobjekten und Callback-Adressen des Treibers. Gastadressen
sind Hexadezimalzeichenfolgen, damit JSON-Verbraucher keine 64-Bit-Präzision
verlieren. Das Objekt `configuration` protokolliert Limits, Dienstnamen und
`kernel_exports`-Überschreibungen sowie die `registry`-Eingabe des Laufs. Das
Profil lautet `wdm-x64-scheduled-v65`. `nt_status` bleibt das
DriverEntry-Ergebnis, während `scenario_success` Initialisierung und
abgeschlossene Anforderungen gemeinsam beschreibt. `phase`, `requests` und
`unload_completed` kennzeichnen die ausgeführten Teile des angeforderten
Lebenszyklus. Jeder API-Aufruf und CPU-Schreibzugriff protokolliert auch seine
Phase (`driver_entry`, `add_device:<ID>`, `request:N`, `callback:N` oder `unload`). Jede Anforderung meldet
Dispatch- und I/O-Status, Abschluss, den Information-Wert und zurückgegebene Bytes
in `output_hex`. `preferred_image_base` beschreibt die ursprüngliche PE-Basis.
`security_cookie` ist die Gastadresse des initialisierten Cookies oder `"0x0"`,
wenn keiner erforderlich war. Anforderungsfelder sind `kind`, `device`, `device_id`, `pnp`, `file`, `requestor_process_id`, `byte_offset`, `code`,
`irp`, `completed`, `cancel_requested_at_100ns`, `dispatch_status`, `io_status`, `information`,
`information_hex` und `output_hex`. `information` behält seine Zahlenform;
`information_hex` ist eine Hexadezimalzeichenfolge mit dem Präfix `0x`, die jedes
Bit des vorzeichenlosen 64-Bit-Ergebnisses exakt erhält. Verwenden Sie
`information_hex`, wenn der JSON-Verbraucher keine exakten 64-Bit-Ganzzahlen
erhält, insbesondere bei IOCTLs ohne Ausgabepuffer.

Work-Item-Beobachtungen tragen die Phase `callback:N`. Bei ausstehenden Anforderungen bleibt `dispatch_status` auf `STATUS_PENDING`; der endgültige Abschlussstatus steht getrennt in `io_status` und bestimmt den Beitrag zu `scenario_success`.

`ExRaiseStatus` übergibt die unteren 32 Bit des NTSTATUS an den Gast-Ausnahmehandler; `ExRaiseAccessViolation` und `ExRaiseDatatypeMisalignment` lösen `STATUS_ACCESS_VIOLATION` beziehungsweise `STATUS_DATATYPE_MISALIGNMENT` aus. Das Profil folgt den einzelnen Microsoft-DDI-Seiten: ExRaiseStatus erlaubt `APC_LEVEL`, die beiden parameterlosen Routinen verlangen `PASSIVE_LEVEL`. Einige WDK-SAL-Annotationen erlauben APC_LEVEL für diese Wrapper; dieses Profil behält die strengere dokumentierte Grenze bei. Ein auslösender Aufruf behält `result: null` und vermerkt den Code in `detail`; er meldet niemals eine erfolgreiche API-Rückkehr.

Die Ausnahmezustellung verwendet die dekodierten x64-Unwindtabellen der Version 1 und konstante `EXCEPTION_EXECUTE_HANDLER`-Bereiche von `__C_specific_handler`. Sie führt den tatsächlichen Gast-Handlercode aus, unterstützt das Abwickeln gewöhnlicher Hilfsfunktionsframes, stellt gesicherte nichtflüchtige Allzweckregister wieder her und wahrt die Stackgrenze der laufenden Ausführung. `GetExceptionCode()` liefert den ausgelösten Code. Ein Handler darf eine weitere Ausnahme in einen umgebenden unterstützten Bereich auslösen. Angetroffene Filterfunktionen, `__finally`, GS-/C++-Persönlichkeiten, verkettete oder unvollständige Metadaten, Prologabwicklung und XMM-Wiederherstellung scheitern ausdrücklich. Eine nicht behandelte API-Ausnahme stoppt mit `model_error`; CPU-Speicher-, Interrupt- und ungültige Instruktionsfehler bleiben terminal.

Der eigenständig geschriebene Testtreiber `driver_wdm_seh.c` verwendet echte WDK-Header und `/GS-`. `NEVERD_WDM_SEH_FIXTURE` und `NEVERD_WDM_SEH_CFG_FIXTURE` konfigurieren normale beziehungsweise aktive CFG-Abbilder. Das Beispiel [driver-seh-scenario.json](../examples/driver-seh-scenario.json) lädt das Abbild an einer anderen Basisadresse, behandelt eine API-Ausnahme in DriverEntry und entlädt es. Der separate WDM-Pfad für METHOD_NEITHER unterstützt Benutzerpufferprüfungen, gesperrte MDLs und abfangbare Speicherfehler.

Das nullable Objekt `fault` erhält den ersten Backend-Fehler. Seine Felder
`kind`, `pc`, das nullable `address`, `size`, `access` und `interrupt` unterscheiden
nicht abgebildeten oder geschützten Speicher, ungültige Bereiche, ungültige
Instruktionen und CPU-Ausnahmen. Adressen verwenden Hexadezimalzeichenfolgen,
Größen und Interruptvektoren Ganzzahlen. Beobachtungslesezugriffe können den
ursprünglichen Fehler nicht ersetzen. Ein fehlerhaft angehaltenes Backend kann
nicht fortgesetzt werden; dieser Datensatz ermöglicht keine Gast-SEH-Behandlung dieser Backendfehler.

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

Beim WDM-`METHOD_NEITHER` zeigen `Type3InputBuffer` und `IRP.UserBuffer` auf getrennte Benutzerzuweisungen. `ProbeForRead` prüft Bereich und Ausrichtung ohne Seitenzugriff; `ProbeForWrite` berührt jede Seite. `ExGetPreviousMode` meldet den Anforderungsmodus. `MmProbeAndLockPages` sperrt Seiten einer Benutzerzuweisung, `MmGetSystemAddressForMdlSafe` liefert ein gemeinsames Alias und `MmUnlockPages` hebt Sperre und Alias auf. Beliebige Prozesskontexte sind nicht modelliert.

Ein WDM-`METHOD_NEITHER` IOCTL-Auftrag mit nichtleerem Puffer kann `user_input_access` und `user_output_access` unabhängig auf `read_write` (Standard), `read_only` oder `no_access` setzen. Für gepufferte/direkte Methoden und leere Puffer werden die Felder abgewiesen; `no_access` behält den Zeiger, sperrt aber den Seitenzugriff.
Der Bericht `configuration.user_page_access` enthält nur explizite Zugriffsangaben mit einem bei null beginnenden `source_request_index`; ausgelassene Richtungen verwenden `read_write`.

## Begrenzte parallele WDM-Anforderungen

Eine WDM-READ/WRITE/IOCTL-Anforderung oder eine Anforderung an eine unbegrenzte parallele KMDF-Standardwarteschlange kann `defer_callback_drain: true` setzen. Nur wenn der Dispatch `STATUS_PENDING` zurückgibt und die IRP weiter aussteht, wird die nächste Anforderung vor den Rückrufen eingereicht. Nach der nächsten Anforderung ohne dieses Feld werden die Rückrufe ausgeführt und der Stapel abgeschlossen; beim letzten markierten Eintrag geschieht dies am Szenarioende. Überlappende Anforderungen können verschiedene Dateiobjekte oder dasselbe ausdrücklich asynchron geöffnete Dateiobjekt verwenden. Überlappungen auf synchronen Dateien, beliebige Präemption und externe Anforderungen werden nicht unterstützt.

## Asynchrones Dateiobjekt

Nur eine CREATE-Anforderung darf das boolesche Feld `asynchronous_file: true` setzen; fehlend oder false bedeutet synchrones Öffnen. Beim asynchronen Öffnen wird `FO_SYNCHRONOUS_IO` im Gast-`FILE_OBJECT` gelöscht und für spätere Datei-IRPs kein `IRP_SYNCHRONOUS_API` gesetzt. READ/WRITE/IOCTL auf derselben asynchronen Datei dürfen sich nur bei ausdrücklich verzögerter Rückrufverarbeitung überlappen. CLEANUP/CLOSE warten auf Abschluss und Finalisierung aller früheren Übertragungen. Eine implizite Dateiposition wird nicht geführt; `byte_offset` gilt je Anforderung und ist standardmäßig null. Andere Anforderungstypen lehnen das Feld auch mit false ab.
