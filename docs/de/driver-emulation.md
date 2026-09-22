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
freistehende Fixtures sowie die gepufferten, In-Direct- und Out-Direct-Pfade von
Microsofts WDM-Beispiel SIOCTL einschließlich seines Builds mit Debugausgaben
sowie Pavel Yosifovichs öffentliches Beispiel Zero für direkte READ/WRITE und
Statistiken ab.
Sie belegen keine Kompatibilität mit beliebigen Treibern Dritter.

| Treiberklasse oder Anforderung | Aktueller Umfang | Fehlende Umgebung |
|-------------------------------|------------------|-------------------|
| x64-Software-WDM-Treiber mit den aufgeführten APIs | Begrenzte x64-WDM-Initialisierung, serielle buffered/direct Anforderungen, Work Items, Timer, DPCs, Ereignisse und Warten, Berichte und Grenzen | Jede weitere ausgeführte API benötigt ein definiertes Modell |
| `METHOD_BUFFERED`-IOCTL | Serielle buffered/direct E/A mit Abschluss durch Work Item oder DPC | Nur die unten genannten APIs; keine gleichzeitigen IRPs oder WDM-Anforderungsabbrüche |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | Anforderungseigene MDLs und Systemabbildungen | physische Seitenidentitäten, DMA und Benutzerabbildungen |
| Vom Treiber allokierte MDLs | Eigenständige Deskriptoren für modellierten nicht auslagerbaren Pool mit ursprünglichen Pufferadressen | IRP-Zuordnung, MDL-Ketten, Prüfen/Sperren, physische Seiten und Benutzerabbildungen |
| READ/WRITE | Serielle buffered/direct E/A mit Abschluss durch Work Item oder DPC | Nur die unten genannten APIs; keine gleichzeitigen IRPs oder WDM-Anforderungsabbrüche; `METHOD_NEITHER` und implizite Dateiposition |
| `METHOD_NEITHER` | Abgewiesen | Benutzeradressraumkontext, Zugriffsprüfung und Gast-Ausnahmebehandlung |
| KMDF-1.33-Nicht-PnP-Treiber | Bindung, Objekte/Kontexte, benannte Steuergeräte, sequenzielle Standardwarteschlangen sowie gepufferte/direkte Anforderungen mit ausgeführten Callbacks | Keine PnP-Geräte, allgemeine Warteschlangenplanung, Klassenerweiterungen oder UMDF |
| PnP-Bus-, Funktions- oder Filtertreiber | Explizite ressourcenfreie PDOs, Gast-AddDevice und vier PnP-Lebenszyklusfunktionen | Weitere PnP-Vorgänge, Power-IRPs, Hardware/Ressourcen und KMDF-PnP |
| Speicher-, Netzwerk-, Anzeige-, Dateisystem- und Minifiltertreiber | Subsystemverträge nicht unterstützt | Port-/Klassen-/Miniport-Frameworks, NDIS/WFP, Grafik- oder Dateisystemdienste |
| Work Items, Timer, DPCs, Ereignisse und Warten | Der aktuelle IRQL ist bei Dispatch und Work Items `PASSIVE_LEVEL`, bei DPCs `DISPATCH_LEVEL` | Nur die unten genannten APIs; keine gleichzeitigen IRPs oder WDM-Anforderungsabbrüche |
| Registry-Operationen über die aufgeführten Zw-APIs | Expliziter Sitzungsbaum und Rechte pro Handle | ACLs, Privilegien, alternative Ansichten und Persistenz |
| Treiber mit Prozess-/Thread-Callbacks, anderen Handles, Dateioperationen oder Kernel-Modulsuche | Außerhalb der aufgeführten APIs nicht unterstützt | Objektmanager, Systemzustand und Erzeuger von Callbacks/Ereignissen |
| Hardware-, DMA-, PCI-, Interrupt- oder Virtualisierungstreiber | Umgebung nicht unterstützt | Gerätemodelle, physischer Speicher, Busse, Interrupts und privilegierter CPU-Zustand |
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

Das begrenzte Modell ist keine vollständige asynchrone Windows-Umgebung. Alertable-/User-Mode-Warten, Systemthreads, APCs, WDM-Anforderungsabbruch, Spinlocks, gleichzeitige IRPs, allgemeine IRQL-Wechsel, `METHOD_NEITHER`, UMDF, KMDF-PnP-Geräte und allgemeine Warteschlangenplanung, vollständiges PnP/Power, Hardware, DMA und Interrupts bleiben unmodelliert. Reine Initialisierung führt explizit eingereihte Callbacks aus, erzeugt aber keine Anforderungen oder implizites Entladen.

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

Power-IRPs, treiberseitig erzeugte IRPs, weitere PnP-Minorfunktionen und Hardware-/Ressourcenmodelle bleiben ununterstützt. WDF-Anbindung/Weiterleitung, Anbinden an Stapel mit aktiven Dateien oder Callbacks, Trennen einer mittleren Schicht, Änderung der weitergeleiteten Major-Funktion und Ziele außerhalb der gespeicherten Route scheitern ausdrücklich. Das optionale echte WDK-Fixture `driver_wdm_stack.c` verwendet `NEVERD_WDM_STACK_FIXTURE` und `NEVERD_WDM_STACK_CFG_FIXTURE`. Native und C-API/CLI-Tests umfassen Relokation; fehlende Artefakte werden sichtbar übersprungen. Ausführungsnachweise gelten nur für Linux.

Ein Szenario kann bis zu 64 `pnp_devices` explizit konfigurieren. Jeder Eintrag benötigt `id`, `bus: "resource_free"`, `initial_device_power: "D0"` und `initial_system_power: "working"`; fehlende Angaben werden nicht geraten. IDs sind 1–64 ASCII-Bytes lang und unterscheiden Groß-/Kleinschreibung: zuerst ein alphanumerisches Zeichen, danach nur solche Zeichen oder `_`, `-`, `.`. Normale Anfragen können ein konfiguriertes `device_id` statt `device` auswählen, jedoch nicht beides. `kind: "pnp"` benötigt `device_id`, `minor` und `bus_completion`; unterstützt sind `start`, `query_remove`, `cancel_remove`, `remove`. `bus_completion.status` ist eine erforderliche 32-Bit-Zahl oder Hex-Zeichenfolge. Optionales `delay_100ns` ist eine nichtnegative Ganzzahl bis INT64_MAX, gemessen ab tatsächlichem Empfang beim Provider. `STATUS_PENDING` ist kein Endstatus; cancel-remove/remove erfordern exakt `STATUS_SUCCESS` (0). Datei-, Transfer- und Abbruchfelder werden bei PnP auch mit Wert null/0 abgelehnt. Dieselbe Vorprüfung gilt im C++-API.

```json
{
  "pnp_devices": [
    {"id": "sensor0", "bus": "resource_free", "initial_device_power": "D0", "initial_system_power": "working"}
  ],
  "requests": [
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0", "delay_100ns": 10}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "remove", "bus_completion": {"status": "0x0"}}
  ],
  "unload": true
}
```

Nach erfolgreichem DriverEntry läuft `AddDevice` einmal je konfiguriertem PDO; der Provider besitzt ein separates `DRIVER_OBJECT`, das der Gast weder löschen noch imitieren darf. PnP-IRPs sind `KernelMode`, datei- und ressourcenfrei, anfänglich `STATUS_NOT_SUPPORTED`. Busantworten werden erst bei tatsächlicher Weiterleitung zum PDO verwendet; verzögerter Abschluss nutzt die gemeinsame virtuelle Uhr und Completion-Fortsetzungen. Der endgültige obere Abschluss entscheidet unabhängig vom Busstatus über Zustandsübernahme oder Rollback. PnP-Erfolg setzt tatsächlichen Providerabschluss voraus; frühe START/QUERY_REMOVE-Fehler können ohne Busbeobachtung bleiben. CREATE/Transfers erfordern Started/D0/Working; cleanup/close bleiben nach query-remove möglich. Normales Remove aus Started benötigt erfolgreiche Query, geschlossene Dateien, abgearbeitete frühere Anfragen/Callbacks und Gast-Detach/Delete. Geräte-/Dateiidentität bleibt nach Detach erhalten. Sauberer AddDevice-Fehler entfernt nur den Provider; neue Gastgeräte-Leaks, auch abgetrennte, verursachen `model_error`. Vor Unload müssen alle Provider entfernt sein. Weitere PnP-Funktionen, Power-IRPs, Hardware/Ressourcen und KMDF-PnP fehlen.

`configuration.pnp_devices` erhält die Anfangskonfiguration. Beobachtete `pnp_devices` enthalten `id`, `pdo`, nullable `add_device_status`, aktuelles `attached`, `pnp_state`, `provider_present`; nach Remove ist `attached` false. AddDevice-Phasen heißen `add_device:<ID>`; Fehler beeinflussen `scenario_success`, nicht DriverEntrys `nt_status`. Anfragen ergänzen nullable `device_id` und `pnp`; PnP verwendet `file: null`. Das `pnp`-Objekt enthält `minor`, `state_before`, `state_after`, nullable `bus_status`, `bus_received_at_100ns`, `bus_completed_at_100ns`. Konfigurierter Status wird erst beim echten Busabschluss beobachtbar; Empfangszeit bleibt unabhängig. Bestehende Feldtypen ändern sich nicht.

Ressourcenfreies PnP verwendet das originale echte WDK-Fixture `driver_wdm_pnp.c`, optionale `NEVERD_WDM_PNP_FIXTURE`/`NEVERD_WDM_PNP_CFG_FIXTURE` und native sowie C-API/CLI-Tests. Fehlende Artefakte werden explizit übersprungen; Ausführungsnachweise bleiben Linux-spezifisch.

KMDF 1.33 verwendet die genaue ABI 1.33.0: 458 Funktionsplätze besitzen stabile Gastidentitäten; die folgenden 38 APIs haben Ausführungssemantik. `WdfVersionBind` und `WdfVersionUnbind` verwalten Gastbindungen rund um den echten WDK-Wrapper `FxDriverEntry`. `WdfGetDriver` liest die öffentlichen Treiberglobals. Nicht-PnP-Treiber, generische Objekte, Steuergeräte, Warteschlangen und eingehende Anforderungen verwenden typisierte Kontexte, Referenzzähler und ausgeführte Cleanup-/Destroy-/Unload-Callbacks. Alle modellierten Framework-Aufrufe und Callbacks erfordern derzeit `PASSIVE_LEVEL`; neue Referenzen nach abgeschlossenem Cleanup liegen weiterhin außerhalb dieses Profils. Unmodellierte Funktionsplätze, `WdfLdrQueryInterface`, Klassenerweiterungen und UMDF stoppen explizit.

Steuergeräte benötigen einen kopierten Namen aus druckbaren ASCII-Zeichen und exakt die SDDL `D:P(A;;GA;;;WD)`. Sie gewährt allgemeinen Zugriff, ohne ein Aufrufertoken zu erfinden; andere Sicherheitsdeskriptoren, unbenannte Geräte und automatische Namen werden nicht unterstützt. Die Geräteinitialisierung besitzt ein WDM-Gerät. Anforderungen können es im bestehenden Sitzungsnamensraum über die symbolischen Link-Aliasse `\DosDevices\Name` oder `\??\Name` auswählen; der Bericht behält den kanonischen Gerätenamen bei. Erfolgreiche Erstellung verbraucht das Initialisierungsobjekt und leert dessen Zeiger, fehlgeschlagene Erstellung setzt den teilweise entstandenen Gerätebesitz zurück. `WdfControlFinishInitializing` gibt die I/O-Zustellung frei. Das Löschen entfernt Gerät und Links nur, wenn modellierte Dateien, Arbeitselemente und Anforderungen dies erlauben; Abbruch oder Abarbeitung ausstehender Anforderungen während des Löschens werden nicht unterstützt.

Die 96 Byte große Struktur `WDF_IO_QUEUE_CONFIG` unterstützt eine sequenzielle Standardwarteschlange mit expliziter passiver Ausführung und ohne Framework-Synchronisierung. Warteschlangen von Steuergeräten unterliegen keiner Energieverwaltung. Spezifische READ-/WRITE-/IOCTL-Callbacks haben Vorrang vor dem Standardcallback. Angenommene Warteschlangenanforderungen liefern auch bei synchronem Abschluss `STATUS_PENDING`; das Rückgaberegister eines void-Callbacks schließt seine Anforderung nicht ab. Verzögerter Abschluss nutzt den vorhandenen Scheduler. Ohne Handler wird mit `STATUS_INVALID_DEVICE_REQUEST` abgeschlossen; READ/WRITE mit Länge null wird ohne Zustellung abgeschlossen, sofern diese nicht aktiviert ist. Das Standarddateipaket schließt CREATE/CLEANUP/CLOSE erfolgreich mit Information=0 ab. Parallele/manuelle Warteschlangen, Dateicallbacks, PnP-Geräte und vollständiges PnP/Power bleiben unmodelliert.

Anforderungsparameter verwenden das 40-Byte-Layout `WDF_REQUEST_PARAMETERS`. Eingabe-/Ausgabezugriffe liefern logische Längen und erhalten Pufferaliasse sowie vorhandene MDL-Abbildungen für direkte I/O; die Eingabe direkter IOCTLs bleibt gepuffert. Falsche Richtungen und zu kleine Puffer liefern dokumentierte Statuswerte. Der Abschluss führt Anforderungs-Cleanup und die Zerstörung untergeordneter Objekte aus, bevor IRP/Puffer ungültig werden; anschließend wird die Anforderung zerstört, sobald ihre Referenzen dies erlauben. Neue Puffer- und Parameterzugriffe werden ab Beginn des Abschlusses abgewiesen; bereits erhaltene Pufferzeiger bleiben während des Cleanups nutzbar. Eine externe Objektreferenz erhält den Kontext, aber keinen Zugriff auf das abgeschlossene IRP. Benutzerseitiges `METHOD_NEITHER` erfordert weiterhin unimplementierte Aufruferkontext-, Probe- und Sperrunterstützung.

Anforderungsabbruch wird für die oben beschriebenen Steuergerätewarteschlangen modelliert. Ist der Abbruch bereits erfolgt, liefert `WdfRequestMarkCancelableEx` ohne Callback-Aufruf `STATUS_CANCELLED`. Ein erfolgreiches `WdfRequestUnmarkCancelable` entfernt den Callback; ein späterer Abbruch vermerkt nur noch den Abbruchzustand. `WdfRequestIsCanceled` liest diesen Zustand bei einer lebenden, nicht als abbrechbar markierten Anforderung. Nach erfolgreicher Markierung setzt der Abschluss erfolgreiches Entfernen der Markierung oder den Beginn der Abbruchcallback-Zustellung voraus; bloße Einreihung reicht nicht. Nach Zustellungsbeginn können Callback und Work Item den Abschluss koordinieren, auch wenn der Callback wartet. Eine separate interne Referenz hält die Anforderung bis zur Callback-Rückkehr. Beim Abschluss wird das IRP dennoch zuerst ungültig; auch die abschließende Destroy-Fortsetzung darf warten. DPCs haben Vorrang, danach folgen Abbruchcallbacks in FIFO-Reihenfolge und gewöhnliche Work Items. Abbruchcallbacks werden auch vor der Wiederaufnahme bereiter passiver Wartekontexte zugestellt.

Bei bereits abgebrochenen Anforderungen führt das alte void-API `WdfRequestMarkCancelable` vor seiner Rückkehr einen synchronen Gast-Abbruchcallback aus. Diese untergeordnete Fortsetzung kann warten, über verschachteltes Cleanup abschließen und vor Wiederaufnahme des API die endgültige Zerstörung ausführen. Späterer Abbruch nach der Registrierung nutzt den oben beschriebenen Scheduler-Pfad. Dies bildet den öffentlichen Quellcode bei `PASSIVE_LEVEL` und `WdfSynchronizationScopeNone` ab; [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) verlangt für Treiber ohne automatische Synchronisierung Ex. Die Ausführung ist Kompatibilitätsverhalten, keine Empfehlung für dieses API in dieser Konfiguration.

`WdfRequestGetInformation` und `WdfRequestSetInformation` teilen das originale 64-Bit-Feld `IRP.IoStatus.Information`, einschließlich direkter Gastschreibzugriffe. Set weist nur zu; die Transferlänge wird beim Abschluss geprüft. `WdfRequestCompleteWithInformation` schreibt dasselbe Feld vor dem Cleanup; Änderungen über ein zuvor gespeichertes IRP im Cleanup bestimmen die abschließende Information, obwohl GetInformation dann bereits 0 liefert. `WdfRequestGetIoQueue` liefert die Ursprungswarteschlange. Mit der Standarddateikonfiguration liefert `WdfRequestGetFileObject` NULL und erfindet kein WDF-Dateiobjekt aus dem WDM FILE_OBJECT. `WdfRequestWdmGetIrp` liefert dasselbe IRP; Gastaufrufe von `IoCompleteRequest`/`IofCompleteRequest` dürfen den WDF-Abschluss nicht umgehen. Solange der Handle während oder nach Abschluss gültig bleibt, liefern GetInformation/GetIoQueue den Wert 0; MDL-Abruf setzt einen gültigen Ausgabeplatz zuerst auf NULL und liefert dann `STATUS_INTERNAL_ERROR`. SetInformation/GetFileObject/WdmGetIrp bleiben dann abgewiesen. Bestehende Puffer- und Parameterzugriffsregeln ändern sich nicht.

`WdfRequestRetrieveInputWdmMdl` und `WdfRequestRetrieveOutputWdmMdl` beschreiben bei Bedarf den vorhandenen SystemBuffer für gepufferte WRITE-Eingabe, READ-Ausgabe und IOCTL-Ein-/Ausgabe. Jeder Abruf prüft zuerst gültige Richtung und positive Länge, dann verwendet er den einen gecachten Deskriptor der Anforderung. Der erste Erfolg legt ByteCount fest, auch bei anderer logischer Länge der Gegenrichtung. `MmGetSystemAddressForMdlSafe` liefert dafür die ursprüngliche VA; zusätzliches Mapping, Unmapping und Treiberfreigabe werden abgewiesen. Direkte READ-/IOCTL-Ausgabe und WRITE-Eingabe liefern dagegen das vorhandene `IRP.MdlAddress`, ohne durch den Abruf ein Mapping zu erzeugen. Direkte IOCTL-Eingabe nutzt den SystemBuffer-Cache. Deskriptoren, IRP und Puffer werden beim Abschluss ungültig. Interne Abbruch- und externe Referenzen erhalten nur den WDF-Kontext. `METHOD_NEITHER` und physischer PFN-Zugriff bleiben unmodelliert.

Modellierte KMDF-APIs: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetIoType`, `WdfDeviceCreate`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfIoQueueGetDevice`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

Die optionale echte WDK-Prüfung kompiliert `driver_kmdf_lifecycle.c` und `driver_kmdf_control.c` separat mit der wirklichen KMDF-Einstiegsbibliothek. `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` wählen Lebenszyklus-Images; `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` wählen normale beziehungsweise aktive CFG-Images für Steuergeräte. Fehlende externe Artefakte werden explizit übersprungen. Native und C-API-/CLI-Abdeckung ist unter [Tests](testing.md) beschrieben. Die Ausführungsnachweise sind derzeit auf Linux-Hosts begrenzt.

Das anfängliche API-Modell besitzt bewusst einen begrenzten Vertrag:

| APIs | Modelliertes Verhalten und Einschränkungen |
|------|-------------------------------------------|
| `RtlInitUnicodeString` | Erstellt eine Gast-`UNICODE_STRING` für eine begrenzte NUL-terminierte Quelle |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | Längengezähltes UTF-16-Kopieren und Vergleich unter Beachtung der Groß-/Kleinschreibung; Vergleich ohne Beachtung der Groß-/Kleinschreibung benötigt eine Windows-Tabelle zur Groß-/Kleinschreibung und stoppt |
| `ExAllocatePool2` | Auslagerbare/nicht auslagerbare NX-Allokationen, standardmäßig genullt; Flags für nicht initialisierte und cacheausgerichtete Allokationen modelliert; ungültige erforderliche Flags liefern NULL, Quota-/ausführbare Pools und ausgelöste Allokationsausnahmen stoppen |
| `MmGetSystemRoutineAddress` | Löst einen längengezählten Gastnamen über den gemeinsamen Exportkatalog auf |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | Expliziter sitzungslokaler Registry-Baum, Rechte pro Handle, Abfragen mit exakten Ausgabegrößen und Lebensdauer nach dem Löschen; siehe Registry-Szenarien |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | Anforderungseigene MDLs mit gecachten KernelMode-Abbildungen und Rechten; MDLs für nicht auslagerbaren Pool verwenden über den sicheren Helfer die ursprüngliche Abbildung |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | Eigenständige Deskriptoren; vollständiger Bereich innerhalb einer gültigen Allokation im nicht auslagerbaren Pool; unabhängige Lebensdauer von Deskriptor und Puffer; ohne IRP-Zuordnung, Ketten oder Kontingente |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Datenallokationen für Pooltypen `0`, `1` und `512`; positive Größe/Tag, passende Tags beim Freigeben, keine Wiederverwendung von Adressen |
| `IoCreateDevice`, `IoDeleteDevice` | Gerätetyp `0x22`, Characteristics `0` oder `0x100`, begrenzte Erweiterungen, ASCII-Namen der Form `\Device\Name` |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | Anbindung desselben Treibers; gibt vorheriges oberstes Gerät zurück, Trennen erhält gespeichertes unteres Gerät; obige Topologie-/Lebensdauergrenzen gelten |
| `IofCallDriver`, `IoCallDriver` | Genaues Ziel auf gehaltener Route; geprüfter Gastcursor und separater unterer NTSTATUS |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` oder `\??\Name` in einem Sitzungsnamensraum, Ziel `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Geprüfte variadische Win64-Formatierung, höchstens 512 Ausgabebytes; alle Debuggerfilter aktiviert |
| `IoGetCurrentIrpStackLocation` | Gibt die Stackposition des aktiven modellierten IRP zurück; normale kompilierte WDM-Makros lesen dasselbe Gastfeld |
| `KeGetCurrentIrql` | Der aktuelle IRQL ist bei Dispatch und Work Items `PASSIVE_LEVEL`, bei DPCs `DISPATCH_LEVEL` |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | Geräteeigene undurchsichtige Work Items; nur `DelayedWorkQueue`, Gerät und Kontext werden auf `PASSIVE_LEVEL` übergeben; eingereihte Einträge dürfen nicht freigegeben werden |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | Opaker DPC, vier Gastargumente, `DISPATCH_LEVEL`, Duplikat-/Entfernungsregeln und Wichtigkeit; nur Ziel CPU0 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | Benachrichtigungs-/Synchronisationstimer, relative/absolute 100-ns-Fristen, Millisekundenperioden, Neusetzen/Abbruch und Signalabfrage in virtueller Zeit |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | Benachrichtigungs-/Synchronisationsereignisse mit unterschiedlichem Signalverbrauch; `KeSetEvent` nur mit Increment=0 und Wait=FALSE |
| `KeWaitForSingleObject` | Ein initialisiertes Ereignis oder Timer; nicht alertable `KernelMode`, Grund `Executive`; Null-Polling, endliche relative/absolute oder unbegrenzte Wartezeit; Nichtnull-/unbegrenztes Warten erfordert IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | Nicht alertable relative/absolute `KernelMode`-Verzögerung bei IRQL <= APC_LEVEL; Gastframe wird nach virtuellem Zeitfortschritt fortgesetzt |
| `IoMarkIrpPending` | Markiert das aktive lebende IRP; der entsprechende Schreibzugriff des WDM-Makros auf das Stack-Control-Feld wird ebenfalls modelliert; Dispatch muss `STATUS_PENDING` zurückgeben |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT`; führt Completion-Abwicklung mit Stop/Fortsetzung aus und gibt IRP/MDL/Puffer erst an der Endgrenze frei |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Begrenzte Gastpufferoperationen, höchstens 1 MiB pro Aufruf; Kopier-APIs ohne Überlappungsunterstützung weisen Überlappungen ab |

IRQL-Obergrenzen stehen in `KernelAPIIRQL.def`; argumentabhängige Regeln prüft das zuständige Modell. DPCs dürfen weder Registry-APIs aufrufen noch paged Pool allokieren, freigeben oder darauf zugreifen. Unicode-Konvertierungen von `DbgPrint` erfordern `PASSIVE_LEVEL`; unterstützte ANSI-Ausgabe und nonpaged Operationen bleiben auf `DISPATCH_LEVEL` nutzbar. Callback-Stacks sind begrenzt: Ein ausbrechender Stackpointer darf nicht den Stack eines anderen blockierten Workers erreichen. Aktive Timer in der Geräteerweiterung verhindern vorzeitige Gerätefreigabe. Allgemeine IRQL-Wechsel werden dadurch nicht bereitgestellt.

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
erlaubt. Nicht-PnP-Anforderungen akzeptieren `kind`, optional `device` oder `device_id` (gegenseitig exklusiv) und optional `file`. IOCTLs benötigen `code` und akzeptieren `input`, `output_size` und
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

Nur READ-/WRITE-/IOCTL-Anforderungen akzeptieren das optionale Feld `cancel_after_100ns`, eine JSON-Ganzzahl zwischen 0 und `INT64_MAX` (9223372036854775807). Die Verzögerung zählt ab Anforderungsübergabe in virtuellen 100-ns-Einheiten, nicht in Echtzeit. Null löst den Abbruch nach Framework-Routing und vor dem Gast-I/O-Callback aus; hat das Routing bereits abgeschlossen, gewinnt der Abschluss. Bei positiven Verzögerungen schreitet die Zeit nur dann zu Timer-, Warte- oder Abbruchfristen fort, wenn kein Callback oder Ausführungskontext bereit ist. Konfigurierter WDM-Abbruch stoppt mit `model_error`; allgemeiner Warteschlangen- und PnP-Abbruch bleibt unmodelliert. Jeder Anforderungsbericht enthält `cancel_requested_at_100ns`: die tatsächliche absolute virtuelle Abbruchzeit oder null, wenn kein Abbruch erfolgte, auch bei vorherigem Abschluss. Eine Abbruchanforderung allein schließt kein IRP ab und legt seinen Endstatus nicht fest.

Bei direkten IOCTLs initialisiert `input` den ersten Systempuffer, während
`direct_input` den separaten, durch die MDL beschriebenen zweiten Puffer
initialisiert und bis `output_size` mit Nullen ergänzt wird. `METHOD_IN_DIRECT`
erfordert Lesezugriff; daraus folgt keine schreibgeschützte Systemabbildung.
Beide Methoden verwenden lesbare/schreibbare Szenariopuffer. `MdlMappingNoWrite`
entfernt Schreibrechte, `MdlMappingNoExecute` Ausführungsrechte der Abbildung.
Das Aufheben der Abbildung entzieht die System-VA; erneutes Abbilden erhält
dieselben gesperrten Daten. Abschluss beendet die Gültigkeit von MDL und Abbildung.
Die von WDM-Makros verwendeten öffentlichen MDL-Felder sind modelliert;
Prozess-/PFN-Felder, selbst erstellte MDLs, Benutzerabbildungen und direkter
Zugriff über den rohen UserBuffer werden abgewiesen. Ein direkter Puffer der
Länge null hat eine Null-MDL.

`IoAllocateMdl` allokiert eigenständige Metadaten für einen nicht leeren, nicht überlaufenden Puffer von höchstens 1 MiB; es prüft oder sperrt diesen Puffer nicht. `Irp` muss NULL sein, `SecondaryBuffer` und `ChargeQuota` müssen FALSE sein. Bei erschöpfter Arena wird NULL zurückgegeben. `MmBuildMdlForNonPagedPool` verlangt, dass der gesamte beschriebene Bereich innerhalb einer einzigen gültigen Allokation im nicht auslagerbaren Pool liegt. Der sichere Helfer und normale WDM-Makros verwenden die ursprüngliche Adresse; Aliase und bestehende Rechte bleiben auch bei neuen Schreib-/Ausführungsverboten erhalten. Zusätzliche Systemabbildungen und das Aufheben der Abbildung werden abgelehnt. `IoFreeMdl` beendet nur die Lebensdauer des Deskriptors; der Poolpuffer hat eine eigene Lebensdauer. Beide Freigabereihenfolgen sind zulässig, solange freigegebener Speicher anschließend nicht verwendet wird. Alle modellierten MDL-Felder sind schreibgeschützt; Prozess-/PFN-Zugriffe, Ketten und manuelle Feldänderungen bleiben unmodelliert. Beim Entladen müssen alle treibereigenen Deskriptoren freigegeben sein.

Bei jeder IOCTL-Methode mit einem `output_size` ungleich null darf `Information`
den Wert `output_size` nicht überschreiten, auch wenn der Eingabepuffer größer
ist. Ohne Ausgabepuffer darf `Information` ein IOCTL-spezifisches 64-Bit-Ergebnis
enthalten; es werden keine Ausgabebytes kopiert. Der Bericht erhält den exakten
Wert in `information_hex`.

Für READ/WRITE wählt `DO_BUFFERED_IO` oder `DO_DIRECT_IO` die Transfermethode.
Neither-I/O oder widersprüchliche Flags stoppen. Information wird gegen die
Transferlänge geprüft; Schreiboperationen liefern eine Anzahl, Leseoperationen Bytes.

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
Profil lautet `wdm-x64-scheduled-v8`. `nt_status` bleibt das
DriverEntry-Ergebnis, während `scenario_success` Initialisierung und
abgeschlossene Anforderungen gemeinsam beschreibt. `phase`, `requests` und
`unload_completed` kennzeichnen die ausgeführten Teile des angeforderten
Lebenszyklus. Jeder API-Aufruf und CPU-Schreibzugriff protokolliert auch seine
Phase (`driver_entry`, `add_device:<ID>`, `request:N`, `callback:N` oder `unload`). Jede Anforderung meldet
Dispatch- und I/O-Status, Abschluss, den Information-Wert und zurückgegebene Bytes
in `output_hex`. `preferred_image_base` beschreibt die ursprüngliche PE-Basis.
`security_cookie` ist die Gastadresse des initialisierten Cookies oder `"0x0"`,
wenn keiner erforderlich war. Anforderungsfelder sind `kind`, `device`, `device_id`, `pnp`, `file`, `byte_offset`, `code`,
`irp`, `completed`, `cancel_requested_at_100ns`, `dispatch_status`, `io_status`, `information`,
`information_hex` und `output_hex`. `information` behält seine Zahlenform;
`information_hex` ist eine Hexadezimalzeichenfolge mit dem Präfix `0x`, die jedes
Bit des vorzeichenlosen 64-Bit-Ergebnisses exakt erhält. Verwenden Sie
`information_hex`, wenn der JSON-Verbraucher keine exakten 64-Bit-Ganzzahlen
erhält, insbesondere bei IOCTLs ohne Ausgabepuffer.

Work-Item-Beobachtungen tragen die Phase `callback:N`. Bei ausstehenden Anforderungen bleibt `dispatch_status` auf `STATUS_PENDING`; der endgültige Abschlussstatus steht getrennt in `io_status` und bestimmt den Beitrag zu `scenario_success`.

Das nullable Objekt `fault` erhält den ersten Backend-Fehler. Seine Felder
`kind`, `pc`, das nullable `address`, `size`, `access` und `interrupt` unterscheiden
nicht abgebildeten oder geschützten Speicher, ungültige Bereiche, ungültige
Instruktionen und CPU-Ausnahmen. Adressen verwenden Hexadezimalzeichenfolgen,
Größen und Interruptvektoren Ganzzahlen. Beobachtungslesezugriffe können den
ursprünglichen Fehler nicht ersetzen. Ein fehlerhaft angehaltenes Backend kann
nicht fortgesetzt werden; dieser Datensatz impliziert keine Gast-SEH-Behandlung.

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
