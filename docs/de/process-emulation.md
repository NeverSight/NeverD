**Sprachen**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Dokumentationsindex](README.md)

# Gastprozess-Emulation

`neverd emulate` führt ein Image unter einem expliziten Gastbetriebssystemprofil aus. CPU-Transport, Image-Parsing, Prozesseinstieg und OS-Dienste haben getrennte Zuständigkeiten. `NEVERD_ENABLE_CPU_EMULATION=ON` aktiviert die Funktion; Treiberemulation schließt sie ebenfalls ein.

Das erste Profil `linux-elf64-v1` führt x64-/AArch64-ELF-`ET_EXEC` sowie selbstrelokierende statische PIE-`ET_DYN` auf CPL3 beziehungsweise EL0 aus. Es lädt echte ELF-Segmente, erstellt den initialen Stack, setzt die Ausführung in begrenzten Quanten fort und verarbeitet explizite Linux-Systemaufrufe. Dies ist ein freistehendes Prozessmodell, keine vollständige Linux-Distribution und keine Zusage für beliebige libc-Binaries. Dynamisches Linken, Signale, Threads, Dateisysteme und nicht unterstützte Dienste schlagen ausdrücklich fehl.

<!-- i18n-section: cli-sdk -->

## CLI und SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Passende Linux-Hosts wählen KVM, passende Windows-Hosts WHP; andere Host-/Gast-ISA-Kombinationen verwenden Unicorn. Ein nicht verfügbares ausgewähltes Backend ist ein Fehler ohne stillen Fallback. Auch unter Windows bleibt das Gastmodell Linux. Die [CPU-Ausführung](cpu-execution.md) beschreibt Befehlsumfang und Grenzen.

Das CLI schreibt genau einen JSON-Bericht. Rückgabecode 0 bedeutet Gaststatus null, 2 einen anderen Gaststatus, 3 unvollständige Ausführung (einschließlich Fehlern und Limits) und 1 ungültige Einrichtung/API. Der tatsächliche Gaststatus steht in `exit_status`. Der additive C-Einstieg [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) erwartet Sitzung, nicht leeren Eingabepfad, explizites Profil und optionale JSON-Optionen. Ergebnis mit `neverd_free_string` freigeben; NULL bedeutet Setupfehler und `neverd_last_error` liefert Details. Gastfehler oder Ressourcenstopps ergeben trotzdem einen Bericht. Ein geladenes Analyse-Image wird weder benötigt noch verändert.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## Optionen und Ergebnisse

Optionen sind ein JSON-Objekt bis 64 KiB. Unbekannte/null-Felder, falsche Typen, eingebettete NULs und nichtpositive Limits werden abgelehnt.

| Option | Standard | Vertrag |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` oder `whp` |
| `arguments` | Eingabedateiname | Vollständiges argv einschließlich argv[0]; leer wählt den Standard |
| `environment` | `[]` | Explizite Gaststrings; Hostumgebung wird nie übernommen |
| `instruction_limit` | 100000 | Gemeinsame Zahl zugelassener Instruktionsversuche |
| `event_limit` | 10000 | Systemaufrufereignisse, vor der OS-Behandlung belastet |
| `timeout_microseconds` | 5000000 | Monotone Deadline ab Ende des Prozess-Setups |
| `memory_limit` | 67108864 | Budget für physischen/gemappten Speicher |
| `stack_size` | 1048576 | Seitenausgerichteter Stack innerhalb des Budgets |
| `output_limit` | 1048576 | Zusammengefasste stdout/stderr-Bytes |
| `instruction_quantum` | 1024 | Zulassungsintervall bis zur Rückgabe an die Runtime |

`schema_version` ist 1. Der Bericht enthält Profil, Architektur, ausgewähltes Backend samt Grund, `stop_reason`, nullable `exit_status`, Diagnose, Ein-/aktuellen PC, Zähler, Service-Aufzeichnungen und letzten typisierten CPU-Ausgang. Adressen, syscall-Nummern, Registerargumente und rohe Rückgabebits sind Hex-Strings **ohne** `0x`; `stdout_hex`/`stderr_hex` erhalten NUL und ungültiges UTF-8. Ein null syscall-Ergebnis bedeutet keine modellierte Rückgabe (etwa Exit oder nicht unterstützte Anfrage), nicht erfolgreiche Null.

<!-- i18n-section: linux-semantics -->

## Semantik des Linux-Profils

Die OS-Policy verwendet dekodierte Program-Header des vorhandenen ELF-Loaders. Sie prüft ABI-Tags, Segmentausrichtung, gemappte Program-Header-Tabellen und User-Adressgrenzen. Ein Mapping-Plan prüft Bereiche, Rechte, Überlappungen und Budget vor der Allokation und veröffentlicht nur einen vollständig vorbereiteten privaten Adressraum. Dateiseiten-Präfix/-Reste bleiben erhalten, BSS wird genullt, Segmentrechte werden beachtet und Stack-Guard-Lücken reserviert. Überlappende Seitenlayouts und widersprüchliche Header werden abgelehnt statt erraten.

Statische PIEs erhalten einen deterministischen Load Bias von mindestens `0x40000000`, erhöht für größere `PT_LOAD`-Ausrichtung. Gemappte Segmente, Eintritts-PC und `AT_PHDR`/`AT_ENTRY` verwenden denselben Bias; ursprüngliche Program-Header-Werte bleiben unverändert und `AT_BASE` bleibt ohne Interpreter null. Gemappt werden Originaldateibytes, keine Analyse-Pointer-Fixups; der Gast muss Relokationen und Initialisierung selbst ausführen. Der Loader dekodiert `PT_DYNAMIC` aus begrenzten Originaldatei-Datensätzen, unabhängig von Section-Headers. Falls vorhanden, muss die Tabelle lesbar, terminiert und höchstens 4096 Einträge lang sein. `PT_INTERP` sowie externe Abhängigkeits-, Filter- und Audit-Tags werden abgelehnt; es gibt keinen dynamischen Linker, Symbolauflöser oder Konstruktorlauf.

Der initiale Stack enthält ausgerichtetes argc/argv/envp/auxv sowie PHDR/PHENT/PHNUM, Entry, Seitengröße und Identitätswerte. Modellierte PID/TID/UID/GID sind 1000. `AT_RANDOM` enthält für reproduzierbare Ausführung die ersten 16 Bytes des Eingabe-SHA-256; das ist eine deterministische Modellpolicy, keine kryptografische Entropie. HWCAP/HWCAP2 sind null; ein vDSO gibt es nicht.

Implementiert sind `write`, `exit`, `exit_group`, `getpid` und `gettid`, `mmap`, `mprotect`, `munmap`, `brk`, mit getrennten Nummern für [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) und [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). Ein zurückkehrendes x64-SYSCALL setzt RCX/R11-Clobber, RAX und den nächsten PC. ARM64 nutzt x8 als Nummer und x0 als Ergebnis. Unbekannte Aufrufe stoppen als `unsupported_service`; Host-system calls werden nie ausgeführt.

Statische `PT_TLS`-Vorlagen werden als Loader-Fakten validiert: genau eine Vorlage, begrenzte Datei-/Speicherbereiche, passende Ausrichtung und lesbare initialisierte Bytes. Der Gaststart allokiert und initialisiert TLS-Blöcke und setzt den Thread-Pointer; das Linux-Modell erfindet kein libc-spezifisches TCB oder DTV. Compiler-erzeugtes local-exec TLS in freestanding Programmen wird damit unterstützt. Dynamisches TLS und OS-Threads bleiben außerhalb des Umfangs.

Auf x64 unterstützt `arch_prctl` `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` und `ARCH_GET_GS`. Set akzeptiert eine User-Basis auch ohne Mapping; spätere Dereferenzierungen prüfen weiterhin Rechte. Kernel-Basen liefern Gast-`EPERM`, ungültige Get-Ziele Gast-`EFAULT`, ohne CPU-Fault. Andere Operationen schlagen ausdrücklich fehl. ARM64 installiert `TPIDR_EL0` mit `MSR`; `MRS`, FS/GS-Speicherzugriffe und Kontextwiederherstellung erhalten Thread-Pointer über Quanten und Backend-Eintritte. Das implementiert keinen Scheduler.

Deskriptor 1 und 2 sind virtuelle Byte-Senken. `write` prüft lesbare User-Seiten, liefert bei später unzugänglicher Seite das lesbare Präfix und `EFAULT`, wenn kein Byte lesbar ist. Ein falscher Deskriptor ergibt `EBADF`; ein gültiger Null-Byte-write greift nicht auf den Zeiger zu. Linux-Pipe-Atomicität oder Dateien werden nicht modelliert. Das Ausgabelimit stoppt vor Veröffentlichung eines zu großen Schreibvorgangs.

Anonyme Speicherdienste nutzen denselben Prozessadressraum und dasselbe physische Budget wie Image und Stack. `mmap` akzeptiert genau `MAP_PRIVATE | MAP_ANONYMOUS` mit gewöhnlichem `PROT_NONE`, `PROT_READ`, `PROT_READ | PROT_WRITE`, `PROT_READ | PROT_EXEC` oder lesbarem RWX. Freie, seitenausgerichtete Hinweise werden übernommen; sonst beginnt die Lückensuche bei `0x100000000`, danach bei der niedrigsten Benutzeradresse und unter Erhalt der Stack-Schutzbereiche. Das deterministische Verfahren emuliert kein Linux-ASLR. Neue Seiten sind unabhängig und nullinitialisiert; teilweises Entfernen kann nicht fixierte Seiten freigeben. CPU-Projektionen oder gehaltene Backing-Views können entfernte Allokationen bis zum Ende ihrer eigenen Lebensdauer erhalten.

Längen werden auf Seiten aufgerundet. `munmap` toleriert Lücken und wiederholtes Entfernen; `mprotect` ändert das gemappte Präfix und liefert an einer Lücke `ENOMEM`. `PROT_NONE` erhält Allokation und Bytes, verweigert aber Gastzugriffe. Der rohe `brk`-Aufruf liefert bei Erfolg die angeforderte Bytegrenze, sonst die alte Grenze, nicht die Null/Minus-eins-Konvention des libc-Wrappers. Die anfängliche Grenze ist das seitenausgerichtete Image-Ende. Wachstum berücksichtigt andere Mappings und das Budget; Schrumpfen erhält Bytes der verbleibenden Teilseite. Regeln und Fehlerpriorität folgen den Linux-Diensten für [Mapping](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c) und [Schutz](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c).

Datei-, gemeinsame und feste Mappings, abwärts wachsender Speicher, große Seiten, Speichersperren, Schutzschlüssel, reine Ausführungs-/Schreibrechte und weitere Flags sind ausdrücklich nicht unterstützt. Sie stoppen vor veröffentlichten Effekten oder erfundenen Rückgabewerten. Normale Bereichs-, Längen- und Ausrichtungsfehler innerhalb der unterstützten Teilmenge liefern Gastfehler und erlauben die Fortsetzung. Kein Gastzeiger oder Mapping-Auftrag wird an das Host-OS weitergereicht.

<a id="windows-pe64-profile"></a>

<!-- i18n-section: windows-pe64 -->

## Windows-PE64-Profil

`windows-pe64-v1` ergänzt begrenzte Windows-Konsolenprozesse für x64/ARM64: PE-Laden, PEB/TEB, statisches und dynamisches TLS, Start-/Ende-Callbacks und benannte Win32-API-Modelle. Es nutzt die CPU-Schicht ohne Treiberemulation; DLL-/CRT-Laden, GUI, Benutzer-SEH, Threads und allgemeine Windows-Kompatibilität bleiben unvollständig.

Der virtuelle Windows-Speicher ergänzt `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` und `FlushInstructionCache` für den aktuellen Prozess. Die OS-Schicht verwaltet Reservierungen; `AddressSpace` bleibt maßgeblich für zugesicherte Seiten, Zugriffsrechte und deren Speicher. Tests prüfen Codeänderungen, Zugriffsfehler und die Wiederverwendung des Speicherbudgets.

Private Zuweisungen unterstützen `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT`, `MEM_RELEASE` und `MEM_TOP_DOWN` mit 64 KiB Reservierungsausrichtung und 4 KiB Seiten. Reservierungen allein verbrauchen kein Gast-RAM. Erneute Zusicherung erhält die Bytes und aktualisiert Rechte; deren Aufhebung gibt einzelne Seiten frei. Bereichsprüfung und vorbereitete Zuweisungen verhindern Teiländerungen bei gewöhnlichen Fehlern. Die Abfrage liefert die 48 Byte große x64/ARM64-Struktur und fasst nachfolgende Seiten nur innerhalb derselben Zuweisung zusammen. Abbild, Umgebung, Heap, API-Einstiege und Stapelränder werden bei der Platzierung berücksichtigt; die Stapelidentität stimmt mit dem TEB überein. Macht ein erfolgreiches `VirtualProtect` den Ausgabebereich für die alten Rechte schreibgeschützt, bleiben die neuen Rechte wirksam und die Ausgabebytes unverändert; der Aufruf meldet weiterhin Erfolg. Eine Rechteänderung über nicht zugesicherte Seiten liefert `ERROR_INVALID_ADDRESS`, schreibt `PAGE_NOACCESS` in die Ausgabe der alten Rechte und lässt die Seitenrechte unverändert.

Unterstützt werden `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE_READ` und `PAGE_EXECUTE_READWRITE`. Schutzseiten, reine Ausführung, Copy-on-Write, Cacheattribute, große Seiten, reset/write-watch/Platzhalter und Änderungen modellinterner Laufzeitabbildungen bleiben ausdrücklich nicht unterstützt. Nur private virtuelle Zuweisungen können zurückgenommen oder freigegeben werden. Benutzer-Ausnahmebehandlung und native ARM64-Hardwarebelege kommen dadurch nicht hinzu.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

Das versionierte Einthreadmodell lädt AMD64-/ARM64-PE32+-Konsolen-EXEs an ihrer bevorzugten Basis. Originalbytes und Loader-Fakten werden geprüft; Benutzerrechte und ungemappte Stack-Schutzbereiche bleiben erhalten. Unbekannte, ordinale, gebundene oder verzögerte Imports, Load-Config/CFG, verwaltete Images, GUI und DLL-Einstiege werden abgelehnt. Relokationen werden geprüft, aber kein Rebasing durchgeführt. API-Modelle sind keine installierten System-DLLs: Loader-Listen enthalten nur das EXE; `GetModuleHandleW` akzeptiert nur NULL.

x64 GS und ARM64 x18 zeigen auf den TEB mit Stack-Grenzen, Selbstzeiger, PID/TID, PEB, Prozessparametern, LastError und TLS. Striktes UTF-8 wird zu UTF-16; argv wird nach Microsoft-CRT-Regeln zitiert. Umgebungsnamen sind ASCII; Duplikate ohne Beachtung der Großschreibung werden abgelehnt. Werte dürfen Unicode enthalten; der sortierte Block endet doppelt mit NUL. Hostumgebung und Dateisystem werden nicht übernommen. Statisches TLS kopiert Vorlage und Null-BSS und setzt einen 32-Bit-Index; dynamisches TLS verwendet eigene TEB-Slots. Attach/Detach liest die jeweils aktuellen Callback-Einträge in Reihenfolge bei gemeinsamem Zeit- und Ressourcenbudget. Eintrittsreturn und normales Beenden führen Detach aus; rekursives Beenden dabei stoppt explizit.

`WindowsProcessServices.def` definiert `ExitProcess`, `RtlExitUserProcess`, Ausgabehandles und synchrones `WriteFile`, LastError, Prozess-/Thread-IDs und Pseudohandles, `GetCommandLineW`, Heap-Allokation/Freigabe/Größe, dynamisches TLS und NULL `GetModuleHandleW`. Aufgelöst werden exakte Namen aus `kernel32.dll`, `kernelbase.dll` und `ntdll.dll`. Direkte Syscalls und gefälschte Callback-Gates wählen keine Modelle. Heap-Speicher gehört dem Prozess und wird freigegeben; Ausgabe bleibt binär. Win32-Fehler sind von nicht unterstützter asynchroner E/A und Benutzerexception-Verarbeitung getrennt. Aliase berücksichtigen das anfängliche Nullsetzen des Ausgabezählers und den tatsächlichen Rückkehrslot.

`windows.native_calls` bewahrt Modul/Funktion, deklarierte skalare Argumente und nullable Ergebnisse ohne erfundene NT-Nummern. `NeverDWindowsProcessTests` prüft echte PE-Dateien, Compiler-TLS, Callback-Änderungen, Heap, Aliase, fehlerhafte Metadaten, Privilegien und Budgets; `NeverDProcessPublicTests` prüft CLI/C ABI. Windows-CI führt dasselbe EXE als unabhängiges Orakel aus und verlangt WHP-Tests. Native ARM64-Laufzeitnachweise benötigen weiterhin passende Hardware.

Bei einem nicht leeren, nicht lesbaren Eingabepuffer liefert `WriteFile` den Fehler `ERROR_INVALID_USER_BUFFER` (1784), setzt die Anzahl geschriebener Bytes auf null und gibt keine Bytes aus.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c).

<!-- i18n-section: verification -->

## Verifikation

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# In einem Shared-Library-/CLI-Build:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Tests übersetzen eigenständige ELF-Einstiege in Assembler und C für beide ISAs. Sie prüfen data/BSS, Startmetadaten, Systemaufruffehler, Binärausgabe, Berechtigungsfehler, Teilzugriffe, nicht unterstützte Dienste und Budgets über Quanten hinweg. TLS-Fixtures initialisieren unabhängige ausgerichtete Blöcke, BSS und Thread-Zeiger und prüfen deren Erhalt; x64 prüft zusätzlich `arch_prctl`-Fehler ohne Verlust der alten Basis. Nicht verfügbare Backends werden ausdrücklich übersprungen. Die öffentliche Suite prüft gemeinsame C-ABI/CLI-Berichte und Exitcodes. PIE-Fixtures prüfen auxv und anfangs leere RELA-Slots vor eigenen Daten-/Funktionszeiger-Relokationen. Mapping-Tests erhalten Analyse-Fixups bei entsprechend gewählter Bytequelle; dynamische Tabellen decken fehlende Sections sowie fehlerhafte/abhängige Eingaben ab. Beide ISAs testen Allokation, Schutz, Lücken, Remapping, Heap-Wachstum/-Schrumpfen und behandelte Fehler. Echte Gastschreibzugriffe prüfen Fehler nach vollständigen und teilweisen Schutzänderungen. x64 ersetzt Code an derselben Adresse zwischen RW und RX und ruft beide Versionen auf; dasselbe ELF läuft unter Linux nativ als unabhängiges Ergebnis-/Fehlerorakel. Reine Speichertests prüfen Budgeterschöpfung, Rückgewinnung und maßgebliche Mapping-Snapshots ohne RAM festzuhalten. Cross-Compilation und Unicorn ARM64 sind kein Nachweis für natives ARM64 KVM/WHP.
