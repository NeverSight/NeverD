**Sprachen**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: c5503091e713c63aa7b08a8cbd5fab1b5cc3c8d8950bb7da4fe754dbe5787137 -->

[← Dokumentationsübersicht](README.md)

# Gastprozessumgebungen für macOS und iOS

`lib/emulation/os/darwin/` modelliert begrenzte, eigenständige Mach-O-Prozesse unabhängig vom Host-CPU-Transport. Aktivieren Sie `NEVERD_ENABLE_CPU_EMULATION`; Windows-Treiberemulation ist nicht erforderlich. `macos/` und `ios/` definieren die expliziten Plattformprofile.

| Profil | Mach-O-Plattform | Gast-ISA | OS-Seitengröße |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, Basis-ARM64 | 4 KiB x64; 16 KiB ARM64 |
| `ios-macho64-v1` | iOS-Gerät | Basis-ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, Basis-ARM64 | 4 KiB x64; 16 KiB ARM64 |

Geräte- und Simulatorbilder sind nicht austauschbar; die Gastplattform wird nicht aus dem Host abgeleitet. Gleiche ISA unter macOS kann [HVF](macos-hvf.md) nutzen, sonst wählt `auto` Unicorn. Die CPU-Speichergranularität bleibt 4 KiB. [C-, Python- und CLI-APIs](process-emulation.md) teilen Optionen, Grenzen und Berichte.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## Bild und Prozessstart

`MachOExecutionImage` erhält die Originalbytes ohne Relokationsänderungen der Analyse. Nur dünne Little-Endian-`MH_EXECUTE`-Bilder mit eindeutiger Plattform und Eintrittsstelle werden zugelassen. Bei Universaldateien muss die gewünschte Architektur zuvor ausdrücklich extrahiert werden.

Die gesamte Datei einschließlich Metadaten und nachgestellter Bytes muss vor Parsing und Kopie in `memory_limit` passen. Der Loader liest einen begrenzten privaten Snapshot einer regulären Datei; NUL-Pfade, kurze Lesevorgänge und Größenänderungen werden abgewiesen. Er hält kein lebendes Dateimapping. Dateibudget und abgebildeter Gastspeicher haben getrennte Obergrenzen desselben Werts; Host-Datei-I/O besitzt keine harte Zeitgarantie.

Segmente behalten aktuelle/maximale Rechte und Nullfüllung. `__PAGEZERO` reserviert Adressen ohne große physische Allokation. Datei-/VM-Bereiche, OS-Seitenausrichtung, gerundete Überlappungen, Header-Zuordnung, ausführbarer Eintritt und Budget werden vor Ausführung geprüft. Das Headersegment muss lesbar und ausführbar sein; Schutzseiten und privates Rückkehrtor bleiben reserviert. Die letzte Dateiseite behält Bytes bis Seitengrenze oder EOF, spätere vollständige VM-Seiten werden genullt: [XNU-Loader](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).

`LC_MAIN` erhält `argc`, `argv`, `envp` und den apple-Vektor als vier ganzzahlige Argumente. Der Rückgabewert liefert die unteren acht Bits des Exitstatus. `/usr/lib/dyld` ist nur für diesen importfreien Eintritt zulässig; Host-dyld wird nicht ausgeführt. Ein von null verschiedener `stacksize` wird abgewiesen, weil die Aufruferoption `stack_size` das Budget besitzt.

`LC_UNIXTHREAD` verlangt genau einen vollständigen nativen 64-Bit-Allgemeinregistersatz, in dem nur PC belegt ist. Der Startstack enthält argc, terminierte argv/envp und einen terminierten apple-Vektor mit `executable_path=<input filename>`. Eigene SP/Flags, andere Register, zusätzliche Flavors und widersprüchliche Eintritte sind unzulässig. Hostumgebung und Linux-Hilfsvektor werden nicht übernommen. Grundlage: [dyld-Architektur](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

Externe Dylibs, Imports, Rebases/Chained Fixups, Konstruktoren/Destruktoren, TLS-Sektionen, arm64e/PAC, andere nicht unterstützte CPU-Untertypen, Verschlüsselung und nicht modellierte Ladebefehle scheitern vor Ausführung. PIE ohne Fixups verwendet bevorzugte Adressen, kein ASLR. Signaturblobs sind Metadaten und implementieren weder AMFI noch Entitlement-Regeln.

## Darwin-Dienste

BSD-Aufrufe auf ARM64 nutzen X16, X0–X5 und `svc #0x80`; x64 die BSD-Klasse `0x02000000`, RAX und RDI/RSI/RDX/R10/R8/R9. Erfolg löscht Carry, Fehler setzt Carry und liefert positives errno. ARM64 löscht X1; x64 löscht RDX nur bei Erfolg und erhält es bei Fehlern. SYSCALL-Registeränderungen sind explizit. Der Bericht kennzeichnet BSD-Fehler mit `result` und `error=true`; nicht zurückkehrende oder nicht unterstützte Aufrufe haben in JSON `result: null` und kein `error`-Feld. Grundlage sind XNU-Eintrittspfade für [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) und [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c); Apple-Code wurde nicht übernommen.

Unterstützt werden `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `getgroups`, `mmap`, `mprotect`, `munmap`. PID ist1000 und PPID1; UID/GID sind standardmäßig1000 oder die unten explizit erklärten realen/effektiven IDs. Deskriptoren 1 und 2 erfassen Bytes einschließlich NUL und Nicht-UTF8; geschlossene oder nur lesbare Deskriptoren ergeben EBADF. Bei teilweisem Kopieren bleiben gelesene Bytes erhalten, der nachfolgende Zugriffsfehler bleibt EFAULT. Längen über `INT_MAX` ergeben EINVAL vor Deskriptor-, Zeiger- oder Budgetprüfung: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

Speicherdienste unterstützen private anonyme Datenmappings mit `flags=0x1002`, Deskriptor -1 und Offset null. Längen und nicht feste Adresshinweise werden auf OS-Seiten aufgerundet. Ein belegter Hinweis sucht zunächst aufwärts, dann am Standardort. Historisches rohes mmap mit Länge null liefert null ohne Allokation; `MAP_UNIX03` wird unterstützt und weist Länge null mit EINVAL ab. Unmap/protect verlangen ausgerichtete Adressen. NONE/READ/WRITE sind möglich; WRITE impliziert READ. Jede OS-Seite besitzt ihre physische Allokation: Teil-Unmap gibt Budget frei, neu zugewiesene Seiten sind genullt. Ein protect über eine Lücke oder oberhalb maximaler Rechte lässt den gesamten Bereich unverändert. Referenz: [XNU-VM-Dienste](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Shared-/Fixed-/JIT-Mappings, ausführbare anonyme Mappings, andere Mach-Traps, indirekte Syscalls, Threads, Signale, Hostdateisystem/Netzwerk, dyld, Objective-C/Swift-Laufzeiten und Foundation/UIKit sind ausgeschlossen und stoppen ausdrücklich. Dies ist kein vollständiges Apple-OS und keine iOS-Simulator-Anwendung.

## Validierung

Eigene C-Fixtures werden mit Clang und `ld64.lld` ohne Apple-SDK oder proprietäre Binärdateien erzeugt. Sie decken fünf Plattform-/ISA-Kombinationen, fehlerhafte Mach-O-Datensätze, 4/16-KiB-Seiten und Teilfreigaben bei ausgeschöpftem Budget ab. `NeverDProcessPublicTests` vergleicht C API und CLI; `NEVERD_TEST_LIBNEVERD` und `NEVERD_TEST_DARWIN_FIXTURES` aktivieren dieselben fünf Kombinationen im Python-SDK.

## Explizite Dateien und Deskriptoren

`darwin_files` stellt allen drei Profilen einen geschlossenen Katalog zunächst schreibgeschützter Dateien bereit. Das Pflichtfeld `files` enthält kanonische absolute Gastpfade `path` und hexadezimale `bytes_hex`; optional liefert `stdin_hex` einen endlichen Eingabestrom. Fehlende Eingabe ist unbekannt und stoppt nichtleere Leseversuche, eine leere Zeichenfolge bedeutet EOF. Ohne Katalog stoppt open; ein ausdrücklich leerer Katalog ergibt ENOENT. Hostdateien und Hosteingabe werden nicht verwendet.

Hinzu kommen `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl` sowie die nocancel-Einstiege von read/write/open/close/fcntl/pread. Unterstützt sind O_RDONLY/O_CLOEXEC und F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL. Separate opens haben eigene Positionen, Duplikate teilen die Position mit separaten close-on-exec-Flags; pread ändert sie nicht. Schließen oder Ersetzen von 0/1/2 wirkt auf spätere I/O; duplizierte Ausgaben behalten Senke und Budget.

Grenzen: 256 Dateien, 16 MiB für Pfade/NUL/Dateien/Eingabe zusammen, Pfade unter 1024 Byte und Komponenten bis 255 Byte. `descriptor_limit` ist eine exklusive Grenze von 3–4096, Standard 256; JSON bleibt auf 64 KiB begrenzt. Ungültige Optionen scheitern vor dem Laden. read über INT_MAX ergibt EINVAL vor FD-Prüfung; EOF berührt das Ziel nicht, ungültige Ziele ergeben EFAULT. Teilweise beschreibbare Puffer stoppen vor Kopie oder Positionsänderung. SET/CUR/END-Fehler erhalten die Position. Altes stat und weitere fcntl bleiben ausgeschlossen. Dateien als Pfadvorfahren ergeben ENOTDIR. Dasselbe Objekt läuft als nativer macOS-Vergleich; C/CLI/Python prüfen fünf Gastkombinationen. Das ist kein iOS-Gerätenachweis.

Release-Prüfung vom 2026-10-05: 381 Registrierungen, 177 bestanden, 204 übersprungen, keine Fehler; alle 51/51 ARM64-HVF-Pflichtfälle liefen. Auch sieben native macOS-Programme, 35 öffentliche C/CLI-/Berichtsprüfungen, fünf Python-Gastkombinationen und 66 Prüflauf-Tests bestanden. Die Zahlen überlappen. Für die neuen Dateidienste fehlen native Intel-HVF/KVM/WHP-Nachweise. Intel HVF bleibt unvalidiert, seine Actions bleiben ausgesetzt. iOS-SDK und Gerätevergleich fehlen.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Vorhandene Dateien ändern

Das strikte Boolean `"writable":true` beziehungsweise `DarwinFileOptions::WritableFiles` erlaubt Änderungen innerhalb des Prozesses. Fehlend/false bleibt schreibgeschützt; unbekannte Berechtigung stoppt. Host und übergebene Anfangsdaten bleiben unverändert. write(4/397), pwrite(154/415), truncate(200), ftruncate(201) und O_TRUNC teilen Inhalte; open besitzt eigene Positionen, dup teilt Position und Status. Inhalte überleben den letzten close. Wachstum füllt mit Nullen, Kürzung erhält Positionen, auch bei O_RDONLY|O_TRUNC.

F_SETFL ändert nach nativer Flag-Konvertierung nur O_APPEND|O_NONBLOCK und erhält Zugriff, close-on-exec und FWASWRITTEN. F_GETFL zeigt nach tatsächlich übertragenen Bytes 0x10000, auch bei pwrite und erfasster Ausgabe. pwrite ignoriert append und erhält die Position. INT_MAX wird vor FD geprüft, pwrite mit -1 liefert noch früher EINVAL. INT64_MAX liefert EFBIG vor der Leerprüfung; die Länge wird vor Wahl des EOF gekürzt.

Erfolgreiches ftruncate setzt FWASWRITTEN auch bei gleicher Größe auf der aufgerufenen Beschreibung und deren dup. O_TRUNC setzt es auf der neuen Beschreibung, auch bei O_RDONLY; pfadbasiertes truncate verändert keine vorhandenen Beschreibungsflags.

Teilweise lesbare Eingaben stoppen vor Effekten. Vollständiges EFAULT erhält Bytes, nichtleeres append setzt die Position jedoch auf EOF. Transportfehler übernehmen weder Inhalt noch Position. Ohne `mutation_policy` verwerfen nichtleere Schreibvorgänge, Kürzungen und vollständiges nichtleeres EFAULT die komplette stat-Beobachtung; spätere Abfragen stoppen vor Ausgabe. Leere Writes erhalten sie. 16 MiB zählen Pfade/NUL, Eingabe, Verzeichnisdaten, CWD, aktuelle Inhalte und Referenzen schreibbarer Pfade. Kürzung ersetzt den Speicher und gibt Kapazität frei; Anfangsdaten und ein begrenzter Ersatzpuffer kommen hinzu. Bekannte inode-Aliase und immutable/append-only-Flags werden abgelehnt.

DarwinMemory hält Mapping-Leases bis zum letzten unmap, auch bei PROT_NONE oder geschlossenen FDs; Änderungen bleiben bis dahin gesperrt. Fehler und alte Null-Längen-Mappings behalten keine Lease. Neue Mappings sehen aktuelle Bytes. O_WRONLY mit READ/WRITE ergibt EACCES; PROT_NONE darf später per mprotect lesen/schreiben.

Originale normale/nocancel-Programme vergleichen den nativen Kernel; 4K/16K-Tests sowie C/CLI/Python prüfen fünf Kombinationen. Rechteprüfung, Verzeichnislöschung, Umbenennen zwischen verschiedenen anfänglichen Verzeichnisdomänen, Hardlinks, native Dateisystem-Metadaten, Mapping-Kohärenz und EOF-SIGBUS fehlen weiterhin. Vollständige Umgebung, iOS-Gerät und Intel HVF sind nicht abgenommen; Intel-Actions bleiben ausgesetzt.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Explizite veränderliche Metadaten

Eine Datei kann neben `writable: true` und vollständigen metadata eine mutation_policy angeben; C++ nutzt `DarwinFileOptions::MutationPolicies`. Dies ist ein expliziter virtueller Vertrag für lückenhafte Belegung, keine Ableitung von APFS oder Hostzeit. Ohne Policy bleiben Metadaten nach Änderungen unbekannt.

allocation_unit, mutation_time und seconds/nanoseconds sind Pflichtfelder mit den bestehenden verlustfreien Ganzzahlregeln. Die Einheit ist eine Zweierpotenz von512 Bytes bis16 MiB, unabhängig von block_size und VM-Seiten. Erforderlich sind normale Rechte ohne set-id/sticky, flags=0, link_count=1 und anfänglich dichte Belegung: blocks=ceil(size/allocation_unit)*(allocation_unit/512). Nullbytes beweisen keine Lücken. Policy-Pfadreferenzen zählen zum16-MiB-Limit; die Belegungsbilanz erzeugt kein angenommenes ENOSPC.

Writes belegen alle berührten Einheiten, auch Nullen in Lücken. truncate-Wachstum fügt nur Nullbytes hinzu; Kürzen verwirft Einheiten hinter aufgerundetem EOF und behält die letzte teilweise belegte Einheit. Erneutes Wachstum stellt verworfene Belegung nicht wieder her. Erfolgreiche nichtleere Writes und jedes erfolgreiche truncate, auch bei gleicher Größe oder leerem O_TRUNC, aktualisieren size/blocks und setzen mtime/ctime auf die feste Vorgabe. Andere Felder und Eingaben bleiben erhalten; Lesen erhöht atime nicht. Pfad-stat, unabhängige open, dup und Wiederöffnen teilen denselben Knoten.

Leere Writes, Budget-/Mapping-Ablehnung, abgelehnte Teilzugriffe und Backendfehler erhalten bekannte Metadaten. Vollständiges nichtleeres EFAULT macht sie unbekannt; späterer Erfolg stellt sie nicht wieder her. Fehler beim stat-Ausgeben ändern den Knoten nicht. virtual-file-metadata prüft144 Bytes über fünf Profile und C/CLI/Python: Policy-Test, kein APFS-Gleichheitsnachweis. Native Programme prüfen Flags, Positionen und Fehler separat. Namensraum, native Kohärenz, Mach und dynamische Laufzeit fehlen weiterhin.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```


## Positionierung in Dateien mit Lücken

Mit mutation_policy und bekannter Belegung unterstützt lseek SEEK_HOLE=3 und SEEK_DATA=4 für reguläre Dateien und liest dieselbe Bilanz wie stat. Anfangsdaten sind dicht belegt, auch Nullbytes. Innerhalb einer passenden Einheit gilt der Eingabeoffset, sonst der Anfang der nächsten passenden Einheit. Die letzte Lücke beginnt bei EOF. Negative Werte ergeben EINVAL; ab EOF, auch bei leeren Dateien, oder ohne spätere Daten gilt ENXIO=6. Fehler erhalten den Cursor; Erfolg ändert nur diese Beschreibung und dup. Andere open behalten eigene Cursor, Wiederöffnen sieht aktuelle Belegung. Metadaten, Flags und Bytes bleiben gleich; hohe whence-Bits werden ignoriert.

Ohne Policy, bei Verzeichnissen oder nach vollständigem EFAULT mit unbekannter Belegung bleibt der Aufruf unsupported. Nullwerte und abgelehnte Änderungen begründen keine Belegung. Das eigene sparse-file-seek vergleicht native/Gast-Fehler, geschriebene Bytes, EOF und Beschreibungslaufzeit ohne Annahmen über frühere FS-Extents. virtual-file-metadata prüft genaue Policy-Geometrie getrennt; C/CLI/Python decken fünf Profile ab.

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Namen regulärer Dateien entfernen

`mutable:true` pro Verzeichnis (C++ `MutableDirectories`) erlaubt ausdrücklich Änderungen direkter Namen, unabhängig vom Dateiinhalt `writable`. Ohne Freigabe wird abgebrochen. Bekannte Flags ungleich null, besondere Elternrechte, link_count≠1 des Kindes sowie bekannte Eltern-/Kind-Aliase werden abgewiesen. Identitäten kombinieren stat und Snapshot-Inodes; ausdrücklich verschiedene Geräte bleiben getrennt. Pfade zählen zum bestehenden Budget.

`unlink(10)` / `unlinkat(472)` entfernen vorhandene reguläre Namen. Dateilöschung unterstützt nur die unteren32 Bits 0 oder `0x800`; unbekannte Bits liefern EINVAL vor Pfad/FD, AT_REMOVEDIR nutzt den unten beschriebenen Löschvertrag; DATALESS und SYSTEM_DISCARDED bleiben unmodelliert. Gemeinsame Auflösung: ENOENT, ENOTDIR nach Datei mit `/`, EPERM für normale Verzeichnisse, EISDIR für eine Wurzel nur aus Schrägstrichen, EBUSY bei abschließendem `.`/`..`. Endkomponenten `.`/`..` wurden nativ geprüft.

Alte FD/dup/unabhängige Opens behalten Daten, Position und Flags; F_GETPATH liefert den erfassten alten Pfad. Neue Opens scheitern, implizite Eltern und CWD bleiben. Schreibfreigaben gehören zum Objekt; das aktuelle Bytebudget wird erst nach letztem Deskriptor und letzter Mapping-Range durch close/dup2/nächste Mutation zurückgewonnen. Ursprüngliche Pfadkosten bleiben; Rechteprüfung, Umbenennung zwischen verschiedenen anfänglichen Verzeichnisdomänen und Hardlinks fehlen noch; anfängliche Verzeichnisse verwenden die unten beschriebene explizite Freigabe.

Eltern-stat/readdir/SEEK_END werden für alte/neue FD und Pfade unbekannt und stoppen vor Kopie/Cursoränderung. read/pread bleiben EISDIR; SET/CUR/F_GETPATH/fchdir/relative Auflösung funktionieren weiter. Eine bekannte Richtlinie setzt nur nlink=0 und feste ctime; spätere Schreibvorgänge stellen nlink=1 nicht wieder her. Ohne Richtlinie/nach EFAULT bleiben Metadaten unbekannt. Fehler erhalten den Zustand. `unlinked-file` vergleicht native Namen-/FD-Regeln; Zeit und Invalidierung sind explizite Modellregeln.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Reguläre Dateien erstellen

O_CREAT=0x200 erstellt eine leere Datei direkt unter einem explizit mutable Elternverzeichnis, über normales/nocancel open und openat. Neue Objekte sind beschreibbar; vorhandene behalten ihre WritableFiles-Freigabe. Ein Nur-Lese-FD kann erstellen, aber nicht schreiben. Ohne explizite Erstellungsrichtlinie bleiben stat64 und Sparse-Suche unbekannt. metadata/mutation_policy eines früheren gleichnamigen Objekts werden nie übernommen.

O_EXCL=0x800 mit O_CREAT liefert bei vorhandenen Dateien/Verzeichnissen EEXIST vor Kürzung; allein ist es wirkungslos. Ein vorhandenes Verzeichnis lässt sich mit Nur-Lese-O_CREAT öffnen. Nach der unten beschriebenen openat-Prüfung des ersten Bytes und Verzeichnisses gilt die Reihenfolge: ungültiger Zugriffsmodus, FD-Platz, EINVAL für O_CREAT|O_DIRECTORY, Pfad. Nur die letzte ursprüngliche fehlende Komponente kann entstehen; fehlende Vorfahren und `/`, `//`, `/.`, `/..` am Ende liefern ENOENT. Neues O_CREAT|O_TRUNC setzt FWASWRITTEN nicht, Kürzung vorhandener Dateien dagegen schon.

Nur Einfügen invalidiert Elternbeobachtungen. Gleichnamige alte/neue Objekte behalten getrennte Daten, FD, Metadaten und Mapping-Leases. 256 Einträge umfassen feste ursprüngliche Nicht-Datei-Einträge und lebende Dateien; dynamische kanonische Pfade/NUL und aktuelle Bytes zählen zu 16 MiB. Nach unlink gibt erst der letzte FD/Mapping die dynamischen Kosten frei, ursprüngliche Kosten bleiben. Budgetende oder kanonische Pfade ab 1024 Bytes stoppen ausdrücklich ohne erfundenes ENOSPC oder natives Pfad-errno, ohne Namen/FD zu veröffentlichen. created-file vergleicht natives macOS und fünf Profile; 4K/16K-Tests prüfen Grenzen. Rechteprüfung, Umbenennung zwischen verschiedenen anfänglichen Verzeichnisdomänen, Links und Verzeichnismutation bleiben offen.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Explizite Erstellungsmetadaten und Prozess-umask

Optionales `darwin_files.umask` (C++ `InitialUmask`) gibt unabhängig von Erstellungsrechten die Anfangsmaske von oktal 0 bis 07777 an. `umask(60)` liefert die vorige Maske und speichert die unteren 07777 Bits, ohne Gast-Speicherzugriff oder freien FD. Weglassen bedeutet unbekannt; Host-/Standardwerte werden nicht geraten. Die einmalige Initialisierung und spätere Änderungen betreffen nur künftige Erstellungen, nicht die Eingabe. Im Beispiel entspricht dezimal 18 dem oktalen 0022.

Ohne `namespace_policy`: Optionales `darwin_files.creation_policy` (C++ `CreationPolicy`) liefert vollständige Metadaten neuer Objekte. Das strikte Objekt enthält genau `first_inode`, `block_size`, `generation`, `creation_time`, `mutation_policy`; Zeit und Mutationsrichtlinie nutzen die bestehenden Formate. Erforderlich sind eine explizite umask, mindestens ein mutable Elternverzeichnis und vollständige metadata für jedes freigegebene Elternverzeichnis. block_size liegt in 1..INT32_MAX, generation ist uint32. Die Allokationseinheit ist eine Zweierpotenz von 512 bis 16 MiB, unabhängig von Block-/VM-Seitengröße; Nanosekunden liegen in [0,1000000000). first_inode ist ein positiver uint64 größer als alle stat/Snapshot-inodes, auch anderer Geräte. Dezimalstrings erhalten Werte außerhalb des exakten JSON-Ganzzahlbereichs.

Nur erfolgreiche neue Einfügungen verbrauchen die globale inode-Folge; UINT64_MAX erschöpft sie dauerhaft. close/unlink/Namensreuse/umask/Nachschlagen setzen sie nicht zurück. Exklusiv-, FD-, Pfad-, Eintrags- und Bytebudgetfehler veröffentlichen weder Namen/FD noch Zählerfortschritt; bestehendes O_CREAT verbraucht nichts. Neue stat64-Daten übernehmen device/GID vom direkten Elternverzeichnis, ausgewählte effektive Gast-UID (Standard1000), mode `S_IFREG | (mode & 0777 & ~umask)`, nlink=1 und size/blocks/flags=0. Blockgröße, generation und vier feste Anfangszeiten stammen aus der Richtlinie. Nach Invalidierung des vollständigen Eltern-stat/Eintragsbilds bleiben device/GID nutzbar, ohne den vollständigen Datensatz wiederherzustellen.

Neue Knoten besitzen eigene Metadaten/Allokation und erben nichts vom alten gleichnamigen Objekt. write/truncate/unlink teilen die Richtlinie und erhalten inode/mode/birthtime sowie nlink=0 nach unlink; vollständiges EFAULT bleibt dauerhaft unbekannt. Bestehende Knoten ändern sich nicht rückwirkend. `created-file-metadata` vergleicht native Rechte, Maskenrückgabe, effektive UID, Eltern-Gerät/Gruppe und Lebensdauer in fünf Profilen; `virtual-created-metadata` prüft separat alle 144 Bytes. Native vier Zeiten müssen nicht gleich sein. Feste Zeit/sparse Allokation sind virtuelle Regeln; Rechteprüfung, Identitätswechsel, ACL und natives APFS-Verhalten bleiben offen.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Atomarer Austausch von Dateien und erzeugten Verzeichnissen

RENAME_SWAP=0x2 tauscht mit renameatx_np zwei bestehende reguläre Dateien, zwei lebende prozesserzeugte Verzeichnisse oder eine Datei und ein solches Verzeichnis; RENAME_NOFOLLOW_ANY ist optional. Das explizite Anfangsverzeichnis erklärt mutable:true und swap_rename:true; C++ verwendet DarwinFileOptions::SwapRenameDirectories. Nachfahren erben die Fähigkeit des ursprünglichen Objekts; Namenswiederverwendung überträgt keine alte Erklärung. false oder Auslassen bedeutet unbekannt. Gleiche Devices und Änderungsrechte beweisen keine Fähigkeit; getrennte Anfangsdomänen bleiben ausgeschlossen.

`openat_nocancel`, `fstatat64` und `F_GETPATH=50` nutzen denselben Dateibereich. Pfade und Beobachtungen gehören nach dem Austausch weiter zum jeweiligen Objekt; vollständiger stat bleibt vom Metadatenvertrag abhängig.

Ein fehlendes Ziel, auch mit Endslash, liefert ENOENT vor Quellpunkt/Doppelpunkt, Domäne, Rechten oder Fähigkeit. Anfängliche oder entfernte Verzeichnisoperanden bleiben ausgeschlossen. In der zugelassenen Domäne liefern beide Vorfahr/Nachfahr-Richtungen und Verzeichnis/Kinddatei EINVAL; ein Dateiziel mit Endslash liefert ENOTDIR. Dasselbe Objekt mit gewöhnlicher Komponente bleibt nach Rechteprüfung unverändert, auch ohne Fähigkeitserklärung. Derselbe Quellpunkt benötigt weiterhin die unbekannte Groß-/Kleinschreibungseigenschaft. RENAME_EXCL=0x4 + RENAME_SWAP=0x2 und unbekannte Flags liefern vor Pfadzugriff EINVAL; SECLUDE bleibt ausgeschlossen.

Beide nichtleeren Teilbäume folgen Elternobjekten, einschließlich entfernter Verzeichnisse und durch FD/Mapping gehaltener Dateiwaisen. Der gemischte Austausch bewegt nur die genaue Dateiwurzel; eine alte gleichnamige Waise bleibt bei ihrem Elternobjekt. FD, dup, CWD und Doppelpunkt folgen Objekt und neuem Elternverzeichnis. Nachfahrdateien behalten Bytes, Identität, Metadaten, Schreibrechte, Cursor, Flags und Leases. Bewegte Wurzeln und beide direkten Eltern verwenden die bestehenden Namespace-Regeln; eine konfigurierte Richtlinie aktualisiert die eigene ctime der Wurzeldatei, sonst bleiben vollständige Metadaten unbekannt.

Jede Referenz reserviert Pfad+NUL im festen anfänglichen 16-MiB-Budget. Alle lebenden/gehaltenen Pfade beider Richtungen werden unter 1024 Bytes und dem gemeinsamen Budget geprüft, bevor alte Namen gemeinsam entnommen und neue veröffentlicht werden. Keine Wurzel wird gelöscht; es gibt kein Ersatzguthaben, auch nicht aus ungeöffneten Zieldaten. Die erste Bewegung einer Anfangsdatei erhält dynamische Pfadkosten; Zurücktauschen löscht sie nicht und Wiederholung summiert sie nicht weiter. Kein neuer FD, Eintrag oder Erzeugungs-inode. Ablehnung bewahrt beide Namespaces, Eltern, Cursor, Beobachtungen und Mappings. Das originale SDK-free swapped-directory vergleicht Verzeichnisse und beide gemischten Richtungen auf nativem macOS und fünf C++/C/CLI/Python-Profilen.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Explizite Verzeichnisschnappschüsse

`getdirentries64` (344) liest den optionalen unveränderlichen `contents` eines vorhandenen `directories`-Eintrags; C++ verwendet `DarwinFileOptions::DirectoryContents`. `entries` enthält in expliziter Reihenfolge alle unmittelbaren Kinder einschließlich `.` und `..`. Ohne Schnappschuss bleibt auch ein leeres Verzeichnis unbekannt. Pfade, stat-Daten oder Hostzugriffe werden nicht abgeleitet.

Jeder Eintrag benötigt `name`, eine von null verschiedene `inode`, `type` (0 unbekannt, 4 Verzeichnis, 8 Datei), `next_offset` und `seek_offset`. Typ und Pfad sowie Inodes desselben aufgelösten Pfades in allen Schnappschüssen und Metadaten müssen übereinstimmen. `next_offset` ist innerhalb des Verzeichnisses eindeutig, positiv und <=INT64_MAX; aufsteigende Werte sind nicht nötig. Null setzt zurück. `seek_offset` ist ein gesonderter vorzeichenloser 64-Bit-d_seekoff-Wert, auch mehrfach null. Ganzzahlen verwenden die verlustfreien Dezimalzeichenfolgen von stat.

`contents.minimum_buffer_size` ist ein erforderliches Nutzdatenminimum von 1–128 MiB, auch bei EOF. Optionales `minimum_buffer_size` eines Eintrags (Standard 0) gilt beim Start an dieser Position. Das Beispiel hält APFS-Beobachtungen fest: 64 Bytes für beide ersten Punkteinträge, 1 bei EOF; sonst muss mindestens ein ganzer Datensatz passen. LP64-Datensätze sind achtfach ausgerichtet, Größe `roundUp(25 + nameBytes, 8)`. Insgesamt höchstens 4096 Einträge; deren Bytes zählen zu 16 MiB. Nur durch Metadaten/Schnappschuss deklarierte Vorfahren zählen einmal zur Grenze von 256 Pfaden. JSON bleibt auf 64 KiB begrenzt.

Unabhängige open-Aufrufe haben eigene Cursor, dup teilt sie. Nur null oder angegebene Werte erlauben Fortsetzung; unbekannte Positionen stoppen ausdrücklich. Zurückgegeben wird das größtmögliche Präfix ganzer Datensätze. Länge >=1024 reserviert die letzten vier angeforderten Bytes für EOF (am Ende 1, sonst 0); nur die Nutzdaten werden auf 128 MiB begrenzt. Die Flagadresse behält die ursprüngliche vorzeichenlose Rechnung samt Überlauf. Reihenfolge: Daten, Cursorfortschritt, ursprüngliche Position, Flags. Späteres EFAULT erhält frühere Effekte; EOF überspringt die leere Datenkopie. Eine nur teilweise schreibbare Einzelkopie stoppt vor dieser Kopie, ohne frühere Effekte zurückzunehmen.

`directory-entries` vergleicht Felder, dup/Zurücksetzen, kleine Lesevorgänge, EOF und Kopierreihenfolge mit macOS. Ein separater Test vergleicht sämtliche erfassten nativen Bytes samt langen Namen mit dem SDK-Layout. Feste Cookies bilden dynamische APFS-Generationen nicht nach. Altes `getdirentries` (196), Auflistung nach Änderungen, andere native Backends und physisches iOS bleiben außerhalb dieser Abnahme.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

Auflistungsprüfung (2026-10-05, Release): 498 Darwin-Fälle, 246 bestanden, 252 wegen fehlender Backends übersprungen, keine Fehler; alle 63/63 ARM64-HVF-Pflichtfälle ausgeführt. Elf native macOS-Programme, 40 C/CLI/Berichtsprüfungen ohne Auslassung, fünf Python-Gastkombinationen mit jeweils acht Dateiszenarien und 66 Werkzeugtests bestanden. Die Zahlen überschneiden sich. Belege: `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel-HVF-Actions bleiben ausgesetzt; andere native Backends und physisches iOS sind nicht bestätigt.

## Private Dateimappings

`mmap` unterstützt reguläre Katalogdateien mit `MAP_PRIVATE`: `flags=0x2` oder `0x40002` mit `MAP_UNIX03` und OS-seitenausgerichtetem Offset. Auch kurze Längen behalten alle Dateibytes der Seite; der Rest der letzten EOF-Seite ist null. Private Schreibzugriffe ändern nur dieses Mapping, nicht Datei, weitere Mappings, feste Metadaten oder gemeinsame Dateiposition. Das Mapping überlebt close und FD-Wiederverwendung. Auch schreibgeschützte und PROT_NONE-Mappings erhalten Anfangsdaten; `mprotect` kann Schreiben erlauben.

Überlauf des Dateiendes, UNIX03-Länge null oder nicht ausgerichtete UNIX03-Offsets ergeben EINVAL vor FD-Suche; ungültige FD ergeben EBADF vor Budgetprüfung. Historische Länge null prüft weiterhin den FD. Historische nicht ausgerichtete Offsets, Streams, leere Dateiseiten und vollständige Seiten hinter EOF stoppen vor Allokation. macOS lässt EOF-Mappings zu, erzeugt beim Zugriff aber SIGBUS; das Modell erfindet weder lesbare Nullseiten noch Signalzustellung. Gemeinsame, feste, ausführbare und JIT-Mappings bleiben ausgeschlossen.

`DarwinFiles` löst FD und Bytes auf; `DarwinMemory` verwaltet Platzierung, Rechte, Budget und Rücknahme. Daten stammen nur aus `darwin_files`. Das gemeinsame Programm `file-mapping` prüft Kopien, close, Positionen, Fehler und anonyme Wiederverwendung. Ein separater nativer Vergleich prüft einen Offset ungleich null, jedes Seitenbyte und SIGBUS.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### Prüfung privater Mappings, 2026-10-05

Release Darwin: 438 eindeutige Registrierungen, 210 bestanden, 228 übersprungen, keine Fehler. Alle 57/57 ARM64-HVF-Pflichtfälle und fünf Unicorn-Gastkombinationen liefen. Neun native macOS-Programme, vollständiger Seitenvergleich bei Offset ungleich null und SIGBUS im isolierten Kindprozess bestanden. 36 öffentliche API-/Berichtsprüfungen liefen ohne Auslassung; Python prüfte fünf Kombinationen einschließlich `file-mapping`. Auch 66 Werkzeug- und 38 Herkunftsregressionen bestanden; Zahlen überlappen. Nachweise: `build-hvf-arm64/darwin-mmap-verified-evidence/`. Keine neuen Intel-HVF/KVM/WHP- oder physischen iOS-Nachweise; Intel-HVF-Actions bleiben ausgesetzt.

## Explizite Dateimetadaten

Ein Dateieintrag kann `metadata` enthalten; dann sind alle unten gezeigten Felder erforderlich. Dezimalzeichenfolgen erhalten die volle Ganzzahlbreite; JSON-Zahlen müssen exakte Ganzzahlen innerhalb ±(2^53−1) sein. device ist 32 Bit mit Vorzeichen, mode/link_count sind 16 Bit ohne Vorzeichen, inode 64 Bit ohne Vorzeichen und uid/gid/flags/generation 32 Bit ohne Vorzeichen. size muss der Bytezahl entsprechen; blocks passt in vorzeichenbehaftete 64 Bit, block_size in nichtnegative vorzeichenbehaftete 32 Bit. Zeiten verwenden vorzeichenbehaftete 64-Bit-Sekunden und 0–999999999 Nanosekunden.

`stat64` (338), `fstat64` (339) und `lstat64` (340) liefern auf ARM64/x64 denselben 144-Byte-LP64-Datensatz. Sie teilen die Pfadauflösung mit open, beachten dup/close und ändern weder FD-Belegung noch Cursor. rdev, Füllbytes und Reserven sind null. Eingaben liefern anfängliche Metadaten; die optionale Policy steuert Änderungen. read aktualisiert keine Zeitstempel, mode ändert keine Katalogzugriffsrechte. Fehlende Metadaten, Streamstatus, altes stat, und erweiterte Sicherheit bleiben ausgeschlossen. Pfad-/FD-Fehler gehen dem Ausgabezeiger voraus; teilweise beschreibbare Ausgabe wird vor jedem Schreiben abgelehnt. Native Tests vergleichen alle Bytes einer echten Datei und die SDK-Offsets; dasselbe eigene Programm prüft alle drei Aufrufe.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### Metadatenprüfung und nächste Schritte (2026-10-05)

Mit stat64: 409 eindeutige Registrierungen, 193 bestanden, 216 übersprungen, kein Fehler; alle 54/54 ARM64-HVF-Pflichtfälle liefen, Unicorn deckte fünf Gastkombinationen ab. SDK-/Originaldatensatzvergleich, acht native Programme, 36 API-/Berichtsfälle ohne Auslassung, fünf Python-Kombinationen und 66 Werkzeugtests bestanden; die Zahlen überschneiden sich. Jeder native Fall hat nun eine eigene Ausgabedatei, sodass kürzere Ausgaben keine alten Endbytes behalten. Für die Ergänzungen fehlen native Intel-HVF/KVM/WHP- und physische iOS-Belege.

Als Nächstes: Shared-Mappings und EOF-Seitenfehler, begrenztes Schreiben (EOF-Seiten, Lebensdauer nach close, Fehlerreihenfolge), explizite Zeit-/Systembeobachtungen, nötige Mach-/Thread-Dienste sowie Mach-O-Abhängigkeiten, Rebases/Binds, Initialisierung und TLS. Objective-C/Swift und Foundation/UIKit brauchen ausführbare native Referenzen. Physisches iOS benötigt SDK und Gerät; Intel HVF bleibt unbestätigt, seine Actions bleiben ausgesetzt.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

Die eigene Workload-Prüfung verlangt alle 111 nativen ARM64- beziehungsweise 74 x64-Fälle, einschließlich `LC_MAIN` und `LC_UNIXTHREAD` auf jeder Plattform. Fehlende/übersprungene Pflichtfälle oder fehlendes `ld64.lld` führen zum Fehlschlag.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Linux verwendet `kvm`, Windows `whp`. Der [Darwin-Workflow](../../.github/workflows/darwin-native.yml) prüft beide x64-Transporte ohne Unicorn und erlaubt einzelne Wiederholungsläufe. Die [Kernelreferenz](../../.github/workflows/darwin-kernel-reference.yml) führt dieselben Programme direkt auf beiden macOS-ISAs aus, ohne NeverD/LLVM. `DarwinNativeCases.def` besitzt Modi, Exitstatus und erwartete Bytes. Nur die Hostreferenz bindet libSystem für den echten dyld-Eintritt. Falsche ISA, Rosetta, Timeout oder Abweichungen scheitern. Diese Referenz belegt keinen iOS-Gerätekernel.

## Nachweise und verbleibender Umfang

Stand 2026-10-03; überlappende Zeilen nicht addieren:

| Transport | Quelle | Bestanden | Fehler | Übersprungen | Native Workloads |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

Der [Intel-Lauf](https://github.com/NeverSight/NeverD/actions/runs/37106013999) gleicht 286 CTest-Identitäten und 32 Prozesse mit Original-XML ab. Die 234 übersprungenen Fälle sind 65 deaktivierte Unicorn-Fälle, 39 ARM64-Gäste und 130 andere Hostplattformen. Artefakt `11267489438` hat den verifizierten SHA-256 `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. Auch [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) wurden unabhängig geprüft. Die [Kernelreferenz](https://github.com/NeverSight/NeverD/actions/runs/37064795867) bestand 4/4 Programme auf jeder ISA, mit Status 37, exakter Ausgabe und leerem stderr.

C API/CLI mit Unicorn: 138 bestanden, 156 übersprungen, keine Fehler. Python deckt alle fünf Kombinationen ab. Die Paketengine stimmt mit 18 ARM64-CLI-Berichten überein; 186 Mach-O-Signaturen wurden geprüft. HVF/Unicorn OFF bestand 38 Prüfungen, übersprang 231 und linkte Hypervisor.framework nicht. Das sind Integrationsnachweise, keine zusätzlichen nativen Ausführungen. Die vollständige Intel-CPU-Abnahme bleibt offen; siehe [HVF](macos-hvf.md) und den [ausführlichen Nachweis](../darwin-emulation.md#hosted-native-verification-2026-10-03).

## Explizite Zeitbeobachtungen

`ProcessOptions::DarwinTime` / `darwin_time` liefert feste Beobachtungen für den rohen Aufruf `gettimeofday` (116), einschließlich der dritten Ausgabe `mach_absolute_time`, auf allen Darwin-Profilen. `time_of_day`, `timezone` und `mach_absolute_time` sind jeweils optional: Fehlen bedeutet unbekannt, eine explizite Null ist ein Wert. Ein leeres Objekt erzeugt keine Standarduhr. Das Modell liest keine Hostuhr, leitet keine Zeitzone ab, lässt Zeit nicht fortschreiten und rechnet absolute Ticks nicht um.

Jeder angegebene Datensatz benötigt alle Felder. `seconds` ist vorzeichenlos mit 32 Bit, `microseconds` liegt in [0, 999999], `minutes_west` / `dst_time` sind vorzeichenbehaftet mit 32 Bit, Ticks vorzeichenlos mit 64 Bit. JSON verwendet die gemeinsamen verlustfreien Ganzzahlregeln; außerhalb des sicheren Bereichs sind Dezimalstrings nötig. Unbekannte Felder, ungültige Bereiche und andere Profile werden vor dem Laden abgewiesen.

LP64 `timeval` umfasst 16 Byte: mit Null erweiterte Sekunden bei Offset 0, 32-Bit-Mikrosekunden bei 8 und vier Nullbytes bei 12. Die Zeitzone hat zwei vorzeichenbehaftete 32-Bit-Felder, Ticks acht Byte. Kalender- und Absolutzeit bilden eine gemeinsame erste Messung; alle angeforderten Beobachtungen müssen vor Kopien und Zeigerprüfungen vorhanden sein. Danach folgen timeval, timezone und absolute ticks. Eine fehlende Zeitzone oder ein späterer EFAULT erhält frühere Schreibvorgänge; überlappende Adressen folgen derselben Reihenfolge. Eine nur teilweise beschreibbare Einzelausgabe stoppt vor ihrer Kopie und erhält frühere Kopien. Nur Nullzeiger benötigen keine Konfiguration, selektive Anfragen nur ihre angeforderten Werte.

Das selbst geschriebene Programm `time` prüft natives Verhalten; `time-values` liefert die konfigurierten 32 Byte über C/CLI/Python für alle fünf Gastkombinationen. Ein separates SDK-Orakel vergleicht jedes Byte mit drei Ausgaben eines einzigen nativen Rohaufrufs. Fortschreitende Uhren, Umrechnung, commpage-Zähler, Timer und Mach-Uhrobjekte/IPC bleiben offen, ebenso dyld, Threads, Objective-C/Swift und Foundation/UIKit. Intel HVF Actions bleibt ausgesetzt; native Intel- und physische iOS-Abnahme wird nicht hinzugefügt.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

Zeitprüfung (2026-10-06, Release): 538 Darwin-Fälle, 274 bestanden, 264 wegen fehlender Backends übersprungen, keine Fehler; alle 66/66 vorgeschriebenen ARM64-HVF-Fälle ausgeführt. Die 12 nativen macOS-Programme und der SDK-Bytevergleich eines einzelnen Samples bestehen. C/CLI/Berichte: 43/43 ohne Auslassungen. Python besteht für fünf Gastkombinationen mit exakten Zeitbytes und acht bestehenden Dateimodi. Alle 66 Runner-Tests sowie Übersetzungs-, Fähigkeits- und Formatprüfungen bestehen. Zählungen überschneiden sich. Nachweise: `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/`, `darwin-time-public.xml`.

## Mach-Zeit und Rückgabekonventionen

`darwin_time.timebase` liefert `numerator` und `denominator` als vorzeichenlose 32-Bit-Werte ungleich null. Das Verhältnis bleibt ungekürzt und wird nicht umgerechnet. `mach_timebase_info_trap` mit Index 89 verwendet ARM64 X16=-89 oder x64 RAX=0x01000059. Es schreibt acht Little-Endian-Bytes (Zähler, Nenner) und liefert null, auch bei vollständig ungültiger Ausgabeadresse. Teilweise beschreibbare Ausgaben stoppen vor dem Kopieren; Transportfehler werden weitergegeben. Fehlende Konfiguration stoppt vor der Zeigerprüfung, auch bei null.

ARM64 X16=-3 und X16=-4 liefern alle 64 vorzeichenlosen Bits von `mach_absolute_time` und `mach_continuous_time`. Jeder Aufruf benötigt nur seinen eigenen Wert; explizite null ist gültig. Die entsprechenden nativen x64-Tabelleneinträge lösen EXC_SYSCALL aus und bleiben ununterstützt. Fortschreitende Uhren, commpage, Timer und Mach-Uhrobjekte/IPC fehlen weiterhin.

Die Auflösung verwendet nur die unteren 32 Nummernbits; der Bericht behält alle ursprünglichen 64 Bits. Negative ARM64-Zahlen wählen Mach; x64 verwendet 0x01000000 für Mach und 0x02000000 für BSD. BSD 3/4 bleiben read/write; unbekannte Nummern und fremde Klassen stoppen. Die aufgelöste Bindung bestimmt die Rückgabe: Mach erhält Flags und X1/RDX, BSD behält seine Carry-Regeln. x64 aktualisiert weiterhin RCX/R11. Mach-Berichte enthalten `result`, aber kein `error`, selbst bei gesetztem Eingangs-Carry.

`mach-time` vergleicht Flags, sekundäres Ergebnis, hohe Nummernbits, ungültige Zeiger und BSD-Wechsel mit dem nativen ARM64-Kernel. `mach-timebase-values` prüft exakte Bytes auf fünf Gästen, `mach-clock-values` auf ARM64; das SDK prüft Layout und erfasstes Verhältnis. Intel HVF Actions bleibt ausgesetzt; x64-Software- und Syntaxprüfungen sind keine native Intel- oder physische iOS-Abnahme.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach-Prüfung (2026-10-06, Release): 569 Darwin-Fälle, 293 bestanden, 276 wegen fehlendem Backend übersprungen, null Fehler; alle 69/69 ARM64-HVF-Pflichtfälle ausgeführt. Im letzten Lauf bestanden 13 native Programme und beide Zeit-SDK-Orakel. C/CLI/report: 100/100 ohne Überspringen; Python deckt fünf Gäste ab. Öffentliche Vergleiche laufen getrennt je Plattform und Szenario mit explizitem Gastbudget von 10 Sekunden; Produktvorgaben und Deadline-Regressionen bleiben unverändert. Zahlen überschneiden sich.

Erste native Starts überschritten die bestehende Fünfsekundengrenze: unabhängig gemessene 6.056 Sekunden, danach 0.010 bei Wiederverwendung. Dasselbe Programm bestand anschließend 13 Fälle mit der ursprünglichen Grenze; Fehlerberichte bleiben erhalten. Nach Zeitüberschreitungen unter Hostlast bestand die getrennte serielle Prüfung. Belege: `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`; Arbeitsbaum vor dem Commit. In jener Revision fehlten ARM64 MRS/MSR NZCV im checked-Vertrag; der Test beobachtete die Flags daher mit Ganzzahlbefehlen. Die folgende Änderung schließt diese CPU-Lücke.

## ARM64-Bedingungsflagregister

Der gemeinsame checked-ARM64-Vertrag lässt die exakten Codierungen `MRS Xt, NZCV` und `MSR NZCV, Xt` in EL0/EL1 zu. Lesen liefert nur Bits 31–28; Schreiben übernimmt diese vier Eingabebits und ignoriert alle anderen. Lesen nach `XZR` verwirft das Ergebnis; Schreiben aus `XZR` löscht die Flags, ohne SP zu lesen. Jedes Backend führt die Originalbefehle aus. Host-Setter-Prüfungen und FPCR/FPSR-Grenzen bleiben unverändert; benachbarte, nicht aufgeführte Systemregister bleiben unzulässig.

`NeverDAArch64NZCVTests` vergleicht alle Flagkombinationen mit Hostbefehlen und prüft vollständigen Skalar-/Vektorzustand, Speicher, Registergrenzen, Beobachterstopp/-fehler, Kontextwiederherstellung und gemeinsame Befehlsbudgets. ARM64 `mach-time` verwendet jetzt echte MSR/MRS um SVC und prüft Mach-Erhaltung sowie den Übergang zu BSD. Native HVF-Anforderungen enthalten alle sechs Methoden in beiden Privilegstufen und das Host-Orakel. ARM64 KVM/WHP und physisches iOS bleiben ungeprüft. Beschreibbare Dateien, Systeminformationen, fortschreitende Uhren, Mach IPC/Threads, dyld/Laufzeiten/Frameworks und Geräteabnahme bleiben weitere Umgebungsarbeit.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


Prüfung veränderbarer Dateien (2026-10-06): Release Darwin, 610 Registrierungen, 322 bestanden, 288 wegen fehlendem Backend übersprungen, keine Fehler; ARM64 HVF 72/72 Pflichtfälle ausgeführt. Abschließende gezielte Tests einschließlich neuer EFAULT-Metadatenprüfungen: 102 bestanden, 12 übersprungen von 114. Alle 15 nativen Programme und 111 öffentlichen C/CLI-/Berichtstests bestanden ebenfalls. Zahlen überlappen. Der erste native Lauf fand die fehlende FWASWRITTEN-Behandlung; nach Korrektur bestanden, Fehlerbeleg aufbewahrt. Keine Frist verändert. Vollständige GitHub-CI und iOS-Geräte bleiben separat; Intel-Actions ausgesetzt.

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python überschritt zuerst in drei ARM64-Verzeichnisfällen fünf Sekunden. Unveränderte Argumente bestanden alle zehn neuen Schreibfälle; ein iOS-Fall benötigte 5,005 s Wandzeit bei1,263 s CPU und lief ab. Alle drei isolierten Wiederholungen bestanden mit gleicher Grenze in2,43–3,17 s, jeweils10.941 Instruktionen und Ausgabe65. Last54–70 bei16 logischen CPUs spricht für Scheduling-Druck, garantiert keine Latenz; Erstfehler bleiben erhalten.

Die abschließende unveränderte Python-Methode bestand alle fünf Kombinationen in41,118 s bei weiterhin fünf Sekunden je Prozess. Frühere Fehler und Diagnosen bleiben getrennt erhalten.


Metadatenprüfung (2026-10-06): Release gezielt148=124 bestanden/24 übersprungen. Gesamtes Darwin645=343 bestanden/300 übersprungen/2 bestehende ARM64-HVF-Verzeichnis-Timeouts. Gleicher20-Fälle-Lauf mit ursprünglichen5s:8 bestanden/12 übersprungen, betroffene Fälle3.818/3.949s. Alle75 Pflicht-HVF-Fälle haben erfolgreiche Beobachtungen; erster Fehlerlauf bleibt erhalten. C/CLI/Reports117/117 mit73 Darwin, Python fünf Profile27.359s, nativ15/15, Runner66/66 bestanden. Virtuelle Belegung ist kein APFS-Nachweis. Keine Friständerung; volleCI, Intel, iOS-Gerät und Gesamtumgebung bleiben offen.

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

Sparse-Seek-Prüfung (2026-10-06): Release Darwin umfasst 671 Fälle: 359 bestanden, 312 wegen nicht verfügbarer Backends übersprungen, keine Fehler. Alle 78 verpflichtenden ARM64-HVF-Fälle wurden ausgeführt; Unicorn deckt fünf Gastprofile ab. Gezielte Tests: 123 von 147 bestanden, 24 übersprungen. Alle 16 nativen Programme, 122 C/CLI/report-Prüfungen (78 Darwin-Vergleiche), fünf Python-Profile (12.344 s) und 66 Runner-Tests bestanden. Zahlen überschneiden sich; Zeitlimits und frühere Fehlerprotokolle bleiben erhalten. Nachweise: `build-hvf-arm64/sparse-seek-validation-summary.json`. Die Belegung ist eine explizite virtuelle Richtlinie, keine APFS-Gleichheit. Vollständige CI und physisches iOS bleiben offen; Intel HVF Actions bleibt ausgesetzt.

Unlink-Prüfung (2026-10-06): Release Darwin708 Fälle,384 bestanden,324 wegen fehlender Backends übersprungen, keine Fehler; alle81 ARM64-HVF-Pflichtfälle ausgeführt. Gezielte156:137 bestanden/19 übersprungen. Native17/17, C/CLI/report128/128 (Darwin83), Python fünf Profile16.268s, Runner66/66 bestanden. Unabhängige Entwurfs-/Implementierungsprüfung ohne verbleibende Blocker. Zahlen überlappen, Zeitlimits unverändert, keine Wiederholung nötig. Nachweise: `build-hvf-arm64/unlink-validation-summary.json`. Invalidierung/feste Zeiten sind Modellregeln; vollständiges Dateisystem/Runtime und physisches iOS bleiben offen. Intel HVF Actions ausgesetzt, vollständige CI separat.

### Erstellungsprüfung, 2026-10-06

Release Darwin:748 Fälle,412 bestanden,336 wegen fehlender Backends übersprungen, keine Fehler; alle84 ARM64-HVF-Pflichtfälle ausgeführt. Gezielt162:150 bestanden/12 übersprungen. C/CLI/Berichte133/133 (Darwin88), Python5 Profile9.982s, nativ18/18, Runner66/66 bestanden. ARM64 verweigerte zunächst korrekt die Pointer-Tabellen-Rebases des Tests. Inline-Bytes beheben die Fixture ohne Lockerung des Loaders; ursprüngliche Fehler/Binärdateien bleiben. Inventarerwartung27→28 aktualisiert. Unabhängige Prüfung ohne Blocker, einschließlich Elternbeobachtungen nach Budgetfehler. Zahlen überlappen, Zeitlimits unverändert. Vollständige CI/physisches iOS separat; Intel HVF Actions ausgesetzt.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### Prüfung der Erstellungsmetadaten, 2026-10-06

Release Darwin: 787 Registrierungen, 439 bestanden, 348 wegen nicht verfügbarer Backends übersprungen, keine Fehler; alle 87 ARM64-HVF-Pflichtfälle ausgeführt. Gezielt: 139/151 bestanden, 12 übersprungen. C/CLI/Bericht: 145/145 einschließlich 98 Darwin-Eingabevergleichen; unveränderte Python-Methode mit fünf Profilen in 12.211 Sekunden bestanden. Native Programme 19/19, Prüfrunner 66/66. Unabhängige Prüfung ohne Blocker; zusätzliche Fälle prüfen verschiedene Eltern-device/GID und globale inode-Folge, unlink vor erstem Schreiben sowie umask ohne freien FD/lesbare Eingabe. Zahlen überlappen, Zeitlimits unverändert, keine Fehlerwiederholung nötig. Feste Erstellungs-/Änderungszeit und Allokation bleiben virtuelle Richtlinien. Vollständige GitHub CI und physisches iOS bleiben separat; Intel HVF Actions ausgesetzt.

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### Umbenennung geprüft, 2026-10-06

Release Darwin: 835 Registrierungen,474 bestanden,360 nicht verfügbare Backends, ein bestehender macOS-ARM 64-HVF-Timeout für virtuelle Metadaten (5.087 s). Unveränderte Methode/Argumente und 5 s-Limit erneut geprüft: 8 bestanden,12 übersprungen, betroffene Identität 0.113 s. Alle 90 erforderlichen ARM 64-HVF-Identitäten haben erfolgreiche Beobachtungen über beide Läufe; das vollständige Gate bleibt als fehlgeschlagen dokumentiert. Fokus 42/54 bestanden,12 übersprungen; C/CLI/Bericht 150/150, davon 103 Darwin-Vergleiche; unverändertes Python mit fünf Profilen 18.478 s; native 20/20, Skripte 66/66. Unabhängige Prüfung fand verschachtelte Punkt-Klassifikation: 4 K/16 K vor Korrektur fehlgeschlagen, danach bestanden. Frühere readonly-ftruncate-Testannahme auf EINVAL korrigiert. Fehler und Probeversionen bleiben erhalten, Zählungen überlappen, Fristen unverändert. Vollständige GitHub CI/iOS-Geräte separat; Intel HVF Actions ausgesetzt.

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## Explizite Systembeobachtungen

`ProcessOptions::DarwinSystem` / `darwin_system` liefert feste Beobachtungen für `sysctl(202)` und das rohe `sysctlbyname(274)` in jedem Darwin-Profil. Alle Felder sind optional; fehlende Werte oder nicht aufgeführte Schlüssel bleiben ausdrücklich nicht unterstützt. Es gibt keine Hostabfragen oder abgeleiteten Versions-/Modellvorgaben. Striktes JSON und C++ prüfen Werte und lehnen andere Profile vor dem Laden ab.

`os_revision` ist ein vorzeichenbehafteter 32-Bit-Wert, `cpu_count` liegt bei 1..INT32_MAX, `memory_size` bewahrt 64 vorzeichenlose Bits. `max_files_per_process` liegt bei 0..INT32_MAX und wird als vier Byte großer int kodiert. Die übrigen Skalarfelder sind höchstens 1023 Byte lange (255 für `hostname`) Zeichenketten ohne eingebettetes NUL; explizit leere Werte sind gültig und die Ausgabe enthält das abschließende NUL. Beobachtungen ändern weder Scheduling noch Speicher- oder Deskriptorbudgets.

| JSON-Feld | sysctl-Name | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |
| `max_files_per_process` | `kern.maxfilesperproc` | `1,29` |
| `hostname` | `kern.hostname` | `1,10` |

`hw.pagesize` stammt aus der vorhandenen Gastspeicherrichtlinie: normalerweise acht Bytes, vier bei nicht null Ausgabe und genau vier Bytes Kapazität. Das alte MIB `[6,7]` und `hw.pagesize_compat` liefern immer vier Bytes. Die dynamische numerische OID von `hw.pagesize` bleibt nicht unterstützt. `hw.memsize` wird bei Kapazität vier nur verkürzt, wenn das 64-Bit-Muster einer Vorzeichenerweiterung eines 32-Bit-Werts entspricht; sonst bewahrt ERANGE34 Ausgabe und Länge.

Die MIB-Anzahl verwendet die unteren 32 Bits und muss 2–12 sein; die Namenslänge verwendet 64 Bits und muss kleiner als 1024 sein. Alle angegebenen Bytes werden vor dem ersten NUL und dem Entfernen eines abschließenden Punkts geprüft. Leere Namen ergeben ENOENT; teilweise lesbare Eingaben bleiben nicht unterstützt. Ein nicht null `oldlenp` muss vor Effekten für acht Bytes vollständig les- und schreibbar sein. Native Versuche mit fehlerhaften Längenzeigern kehrten nicht fristgerecht zurück; diese Zeiger bleiben außerhalb des unterstützten Bereichs. Null `oldlenp` bedeutet Kapazität null; null `oldp` fragt nur die Größe ab. Bei Schlüsseln außer `kern.hostname` ergibt ein kurzer Puffer ENOMEM12 ohne Datenänderung und schreibt Länge null. Daten-EFAULT bewahrt die alte Länge. Eingabe und Kapazität werden vor den Daten erfasst; die Länge wird zuletzt kopiert. Aliase und frühere Kopien bleiben bei späteren Transportfehlern erhalten.

`hostname` deklariert die für diesen Guest-Aufrufer sichtbaren Bytes, ohne Host-Abfrage, implizites mobiles `localhost` oder abgeleitete Entitlements. Fehlende Werte bleiben unbekannt; ein explizit leerer Wert liefert ein NUL. Für `kern.hostname` gelingt eine Ausgabe mit positivem, zu kleinem Puffer mit genau dessen Kapazität an Bytes und abschließendem NUL; diese Kapazität wird gemeldet. Kapazität null behält ENOMEM12, Länge null und unveränderte Daten; ein Null-Ausgabezeiger meldet die volle Länge einschließlich NUL. Nur der tatsächliche Ausgabebereich wird geprüft. Teilweise schreibbare Bereiche bleiben ohne veröffentlichte Präfixe nicht unterstützt; native Teilkopien liegen außerhalb des Modells. Dies ergänzt nur die von libc uname/gethostname verwendete Raw-Beobachtung, nicht deren dylib-Imports oder eine vollständige Laufzeit.

newp/newlen beide ungleich0 bedeuten Schreiben. Namen/MIB und vollständige oldlenp-Lese/Schreibvorprüfung bleiben zuerst. Standard oder explizit nicht-root EUID liefert EPERM1 vor Beobachtung/Datenausgabe. EUID0 stoppt kern.osversion / kern.maxfilesperproc / kern.hostname unsupported, da privilegiertes Schreiben unmodelliert bleibt; RUID entscheidet nicht. Andere nativ schreibgeschützte Knoten liefern auch root EPERM1. Neue Länge0 ignoriert Zeiger; kein erfundenes ENOENT für unbekannte Schlüssel/Bäume/dynamische OIDs.

Das eigene Programm `system-info` prüft native macOS- und Gast-ABI; `virtual-system` vergleicht konfigurierte Bytes über C++, C/CLI und Python. Ein separates SDK-Orakel erfasst neun Hostbeobachtungen als explizite Testeingaben und vergleicht benannte und numerische Ausgaben. Das bestätigt weder physisches iOS noch Intel HVF.

```json
{"darwin_system":{"hostname":"guest-node","os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man 3/sysctl.3.html).

### Prüfung der Systemabfragen, 2026-10-06

Release Darwin: 881 Registrierungen, 509 bestanden, 372 wegen nicht verfügbarer Backends übersprungen, keine Fehler; alle 93 erforderlichen ARM64-HVF-Identitäten ausgeführt. Fokus: 37/49 bestanden, 12 übersprungen. C/CLI/Bericht: 163/163, davon 113 Darwin-Vergleiche. Unveränderte Python-Methode: fünf Profile in 15.302 s; native Programme 21/21, Skripte 66/66. Unabhängige Prüfung ohne Blocker; zusätzliche Fehlerprioritäten und SDK-Orakel bestanden. Ein neuer Orakel-Build scheiterte am fehlenden StringExtras-Header und gelang nach Ergänzung; Quelle und Log bleiben erhalten. Native Versuche mit fehlerhaften Längenzeigern bleiben dokumentiert und außerhalb des Vertrags. Nach den Tests wurden nur zwei Dateikopfkommentare bereinigt und erfolgreich neu gebaut. Zählungen überlappen, Fristen bleiben gleich; keine Laufzeitfehler-Nachprüfung nötig. Vollständige GitHub CI und physisches iOS separat; Intel-HVF-Actions ausgesetzt.

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## Vektorielle Datei-E/A und Ausgabenerfassung

`readv`/`writev`, `preadv`/`pwritev` und nocancel teilen die skalare Datei- und Ausgabelogik, ohne neue Optionen oder Hostzugriffe. Ein LP64-iovec enthält Adresse und Länge mit je acht Bytes. Die vorzeichenbehafteten unteren32 Bits von iovcnt müssen1–1024 ergeben. Das ganze Array wird vor der Deskriptorsuche kopiert; Ausgabealiase ändern den Auftrag nicht. Teilweise lesbare Arrays bleiben nicht unterstützt.

Zugriffsrechte und Positionierbarkeit eines Streams werden vor den Längen geprüft. Einzelwerte und Summe müssen in INT64_MAX passen, bei Dateien und Verzeichnissen zusätzlich in INT_MAX. Endliches stdin wird auf verfügbare Bytes begrenzt; Erfassung behält das Ausgabebudget. pwritev lehnt jeden negativen Offset vor dem Array ab; preadv prüft ihn nach Deskriptor und Längen. Leere Elemente ignorieren die Adresse, jedoch nicht Deskriptor-, Typ- und Offsetregeln. Nach EOF werden restliche Elemente nicht berührt. Positionierte Aufrufe erhalten den Cursor, pwritev ignoriert Append. Normales Append kürzt den gesamten Auftrag einmal am ursprünglichen Cursor und wählt erst dann EOF.

Ein späteres vollständig ungültiges Element liefert EFAULT und erhält vorherige Bytes, normalen Cursorfortschritt und FWASWRITTEN nach dem Schreiben mindestens eines Bytes. Ein zugelassener nichtleerer Schreibvorgang mit Datenpuffer-EFAULT invalidiert vollständige Metadaten; Argumentfehler, Modellablehnungen und Backend-Fehler erhalten sie. Ein teilweise schreibbares Leseziel stoppt mit UnsupportedService ohne Kopie dieses Elements; frühere Kopien bleiben. Eine teilweise lesbare Dateischreibquelle bleibt vor allen Dateieffekten nicht unterstützt. Berechtigung, Mapping-Leases und Gesamtspeicherbudget werden vorab geprüft; Backend-Prüf- oder Lesefehler veröffentlichen keine Datei- oder Ausgabebytes.

Erfassung prüft zuerst das gemeinsame stdout/stderr-Budget. Ein Element über der Benutzeradressgrenze liefert keine Bytes, vorherige Elemente bleiben erhalten. Andere lesbare Präfixe werden mit EFAULT erfasst. Skalare Bereichsfehler bleiben vor dem Budget priorisiert. Duplizierte oder umgeleitete Deskriptoren behalten ihr Ziel. Das originale `vectored-io` prüft acht Eingänge auf nativem macOS, fünf Gastkombinationen und C/CLI/Python. Cancellation, Pipes, Threads und physische iOS-Abnahme kommen nicht hinzu.

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### Prüfung der Vektor-E/A, 2026-10-06

Release Darwin:937 Registrierungen,553 bestanden,384 wegen fehlendem Backend übersprungen, keine Fehler; alle96 erforderlichen ARM64-HVF-Identitäten ausgeführt. Gezielt45/57 bestanden,12 übersprungen. C/CLI/Report168/168, darunter118 Darwin-Eingabevergleiche; Python deckt fünf Kombinationen in 20.397s ab. Nativ22/22, Prüfskripte66/66. Die unabhängige Prüfung ergänzte einen Fehlerfall für positioniertes Schreiben mit Lücke und prüft Cursor, tatsächliches EOF, Metadatenverweigerung und exakte Restkapazität. Beim ersten Build verwies ein alter Test noch auf eine entfernte interne Abfrage; er prüft nun echte Ausgaben. Ein optional<bool>-Fehler in der neuen Ereignisassertion meldete acht erfolgreiche Gastläufe als fehlgeschlagen; korrigierte Prüfungen bestanden. Quellen und Logs beider Fehler bleiben erhalten. Zählungen überlappen, Fristen unverändert. Vollständige GitHub-CI und physisches iOS separat; Intel HVF Actions bleibt ausgesetzt.

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## Abfragen zur Dateiexistenz

`access(33)` und `faccessat(466)` prüfen den aktuellen virtuellen Katalog ohne neue Deskriptoren oder Änderungen an Inhalt, Cursor, Flags oder Metadaten. F_OK bestätigt den Namen nach dem bestehenden Suchvertrag. Metadaten gewähren oder entziehen keinen Katalogzugriff; native Suchrechte der Vorfahren, ACL und MAC werden nicht geprüft. Fehlende oder ungültig gewordene stat-Beobachtungen verhindern die Abfrage nicht. Entfernte Namen liefern trotz alter FD oder Mappings ENOENT; Erzeugung, Wiederverwendung und Umbenennung folgen dem aktuellen Namensraum.

Der Modus verwendet die unteren 32 Bits. R/W/X belegt Bits 0–2, erweiterte Rechte 9–21. `(mode & 0x003ffe07) == 0` bedeutet Existenzprüfung; andere Bits einschließlich Vorzeichen werden ohne EINVAL ignoriert. Rechteanforderungen bleiben nach erfolgreicher Suche UnsupportedService, ohne Schlüsse aus Metadaten oder Mutationsfreigaben. Bekannte Pfad-/Deskriptorfehler gehen vor.

Faccessat akzeptiert jede Kombination der unteren Flags AT_EACCESS(0x10), AT_SYMLINK_NOFOLLOW(0x20), AT_SYMLINK_NOFOLLOW_ANY(0x800). Andere liefern EINVAL vor Pfad oder FD, auch ohne Katalog. Reale/effektive Identitäten sind fest; feste Links folgen den Regeln unten. Absolute Pfade ignorieren dirfd, relative behalten CWD-/Verzeichnis-FD-Regeln. nameiat außerhalb AT_FDCWD liest ein Byte, prüft den relativen FD und importiert dann den ganzen String; `/` überspringt FD. Erstes Byte unzugänglich: EFAULT14; unbekannter/Datei-FD: EBADF9/ENOTDIR20 vor späteren Fehlern. Auch ein leerer relativer Pfad prüft den FD: unbekannt EBADF, normale Datei ENOTDIR, sonst ENOENT. Fehlender Katalog und unbekannte Stream-Verzeichnisidentität bleiben nicht unterstützt.

Das originale `file-access` vergleicht beide Aufrufe, ignorierte Bits, Flags und Reihenfolge auf nativem macOS und fünf Gastkombinationen über C++/C/CLI/Python. NOFOLLOW_ANY nutzt einen relativen Verzeichnis-FD, damit Host-Symlinks unter `/tmp` oder `/var` nicht stören. Direkte Tests decken aktuelle Namen, gemischte Bits, FD-Erschöpfung, Metadatenunabhängigkeit und Speicherfehler ab.

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### Prüfung der Dateiexistenz, 2026-10-06

Release Darwin:971 Registrierungen,575 bestanden,396 wegen fehlendem Backend übersprungen, keine Fehler; alle99 erforderlichen ARM64-HVF-Identitäten ausgeführt. Gezielt23/35 bestanden,12 übersprungen, einschließlich14 direkter Fälle. C/CLI/Report173/173, darunter123 Darwin-Eingabevergleiche. Python prüfte fünf Kombinationen in 16.235s; nativ23/23 und Skripte66/66. Unabhängige Plan- und Codeprüfung ohne Blocker. Das anfängliche NOFOLLOW_ANY-Ergebnis durch den Host-Link /tmp sowie der Vergleich mit kanonischem Pfad bleiben erhalten; der gemeinsame Test nutzt einen relativen Verzeichnis-FD. Zählungen überlappen, Fristen bleiben gleich, kein Laufzeitfehler musste erneut geprüft werden. Vollständige GitHub-CI und physisches iOS separat; Intel HVF Actions bleibt ausgesetzt.

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## Verzeichnisse erzeugen und entfernen

`mkdir(136)` und `mkdirat(475)` erzeugen in einem ausdrücklich veränderbaren direkten Elternverzeichnis. Neue Verzeichnisse erben die Namensraumfreigabe, anfängliche behalten eigene Freigaben. Nur bekannte Device/GID-Werte werden geerbt, niemals vollständiges stat, Größe, Zuteilung, Zeiten oder Cookies. Neue Verzeichnisse verdecken alle Beobachtungen alter gleichnamiger Dateien. `creation_policy` gilt weiter nur für reguläre Dateien: Unterdateien verwenden die geerbte Identität und globale Inodefolge; mkdir verbraucht keinen Datei-Inode. Rechteprüfung und native Verzeichnismetadaten bleiben ausgeschlossen.

Der gemeinsame Komponentenlauf erlaubt bei mkdir einen fehlenden letzten Namen mit ausschließlich nachfolgenden Schrägstrichen. Fehlende Vorfahren vor Punkt/Doppelpunkt liefern ENOENT, Dateivorfahren ENOTDIR, bestehende Namen EEXIST. Relative FD/CWD-Regeln, absolute FD-Unabhängigkeit und Vorrang von Stringfehlern bleiben bestehen. Kein freier FD ist nötig; Such-, Freigabe-, Budget- oder Transportfehler veröffentlichen nichts.

`rmdir(137)` und `unlinkat(472)` mit AT_REMOVEDIR(0x80), optional AT_SYMLINK_NOFOLLOW_ANY(0x800), entfernen leere, von diesem Prozess erzeugte Verzeichnisse. Unbekannte untere32 Bits liefern EINVAL vor Eingaben; DATALESS und SYSTEM_DISCARDED bleiben unmodelliert. Bekannte Pfad-/Typ-/Wurzelfehler bleiben; anfängliche Verzeichnisse ohne removable-Freigabe zu entfernen bleibt UnsupportedService. Bei zugelassenen Zielen ergibt ein abschließender Punkt EINVAL, Doppelpunkt aus einem verknüpften Verzeichnis oder nicht leeres Ziel ENOTEMPTY. Verzeichnis-FDs einschließlich dup und CWD behalten das ursprüngliche Objekt und verhindern die Löschung nicht mehr. Bereits entlinkte reguläre Dateien/Mappings zählen nicht als Namen; native Vergleiche bestätigen Inhalt, Inode und letzten F_GETPATH nach Elternlöschung und Wiederverwendung.

Kanonischer Pfad+NUL und ein Eintrag je neuem Verzeichnis zählen zum gemeinsamen16-MiB-/256-Einträge-Budget. Nach Löschung und Freigabe aller Referenzen wird nur dieser Anteil erstattet, nicht verwaiste Dateien/Mappings. Nur Erfolg invalidiert stat/Auflistung des direkten Elternverzeichnisses; vollständige Beobachtungen neuer Verzeichnisse bleiben unbekannt. Das originale `directory-mutations` vergleicht verschachtelte Erzeugung, Umbenennung, unlink, Löschung und Wiederverwendung auf nativem macOS und fünf Gästen über C++/C/CLI/Python.

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### Prüfung der Verzeichnisänderungen, 2026-10-06

Release Darwin:1.017 registriert,609 bestanden,408 wegen fehlendem Backend übersprungen,keine Fehler;102 erforderliche ARM64 HVF ausgeführt. Gezielt58 bestanden,12 übersprungen,einschließlich26 neuer direkter4K/16K-Fälle. C/CLI/Report178/178,darunter128 Darwin;Python fünf Kombinationen in 17.255s,nativ24/24,Skripte66/66. Unabhängige Prüfung von Budget,Namenswiederverwendung,Elternidentität,Leases und Rollback. Eine zusätzliche native Probe nach dem ersten erfolgreichen Lauf zeigte EISDIR für reine Schrägstrich-Wurzeln und EBUSY bei abschließendem Punkt/Doppelpunkt. Gemeinsame Entscheidung und Tests wurden korrigiert;erste Ergebnisse und Quell-/Binärkopien bleiben erhalten. Überlappende Zahlen,unveränderte Fristen,Intel HVF Actions ausgesetzt;vollständige GitHub-CI und physisches iOS separat.

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## Erhaltene Verzeichnisidentitäten

FD/CWD behalten das gelöschte Objekt und seine ursprüngliche Elternkette trotz Namenswiederverwendung. Punkt-Open hat einen unabhängigen Cursor,dup einen gemeinsamen; Doppelpunkt folgt dem ursprünglichen Elternobjekt. Normale Kinder eines gelöschten Verzeichnisses liefern ENOENT. LOOKUP darf erhaltene gelöschte Eltern durchlaufen,Erzeugung/Löschung/Umbenennung dagegen ENOENT. Abschließender Punkt/Doppelpunkt im Umbenennungsziel liefert EINVAL vor diesem Schritt,nach früheren Vorfahrenfehlern. F_GETPATH behält den letzten Pfad; vollständiges stat/Auflisten bleibt unbekannt. Pfad+NUL und ein Eintrag bleiben bis zur letzten FD/CWD/Kindreferenz berechnet. Close/dup2/CWD-Wechsel/Änderungszulassung geben unerreichbare Ketten frei; Anfangskosten und Dateileases bleiben getrennt. `deleted-directories` vergleicht dies nativ und in fünf Gästen. Eine verwaiste Datei mit offener Beschreibung oder Mapping-Lease hält auch ihre Elternverzeichnisse und deren aktuelle Pfad/NUL- und Eintragskosten. Alle Verzeichnis-FDs zu schließen reicht nicht. Nach Dateifreigabe wird die Elternkette zurückgewonnen. Das Verschieben eines lebenden erzeugten Vorfahren aktualisiert F_GETPATH; Namenswiederverwendung überträgt alte Objekte nicht auf den Ersatz.

### Prüfung der Verzeichnislebensdauer, 2026-10-06

Release Darwin1.051 Fälle,631 bestanden,420 nicht verfügbare übersprungen,keine Fehler;105 erforderliche ARM64 HVF ausgeführt. Gezielt98 bestanden/12 übersprungen,anfangs64/64 direkte einschließlich14 neuer. C/CLI/Report183/183,davon133 Darwin;Python fünf Kombinationen 18.691s,nativ25/25,Skripte66/66. Zusätzliche native Umbenennungsprobe korrigiert abschließende Punkt-Reihenfolge;frühere Quellen/Ergebnisse/Snapshots erhalten. Primärer Agent prüfte Belege;abschließende unabhängige Prüfung nicht verfügbar. Zahlen überlappen,Fristen unverändert,vollständigeCI/physisches iOS separat,Intel HVF Actions ausgesetzt.

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## Explizit zugelassene anfängliche Verzeichnisse entfernen

Der strikte Boolesche Eintrag `"removable": true` (C++ `DarwinFileOptions::RemovableDirectories`) deklariert ein gewöhnliches Verzeichnis ohne Mount und mit einer einzigen Namensraumidentität. Er erfordert einen expliziten anfänglichen `directories`-Eintrag außer der Wurzel sowie einen explizit veränderbaren direkten Elternknoten. Bekannte besondere Modi/Flags, Inode-Aliasse einschließlich Verzeichnisschnappschüssen und widersprüchliche bekannte Gerätenummern von Elternknoten und Ziel werden abgelehnt. Gleiche Gerätenummern beweisen allein nicht die Abwesenheit eines Mounts. Weglassen/false bleibt ununterstützt; andere JSON-Typen sind ungültig. Dies ist kein allgemeines Rechte- oder Mountmodell.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

Die Löschung erfordert einen leeren aktuellen Namensraum. Anfängliche implizite Unterverzeichnisse bleiben nach unlink ihrer letzten ursprünglichen Datei bestehen. Erfolg verwirft die vollständigen stat-/Auflistungsbeobachtungen des Objekts und direkten Elternknotens; alte FD/dup/CWD behalten das Objekt und seine ursprüngliche Elternkette. Unveränderliche Eingaben lassen gelöschte Namen nicht wieder erscheinen. Eine neue Datei oder ein neues Verzeichnis gleichen Namens erhält eine eigene Identität ohne alte Metadaten oder Schnappschüsse. Die Eingaben des Aufrufers bleiben unverändert.

Jede removable-Referenz belastet das feste anfängliche 16-MiB-Budget mit Pfad und NUL. Anfängliche Einträge, Pfade, Referenzen und Schnappschüsse bleiben auch nach Löschung und letztem close angerechnet, ebenso ihr Anteil an der Grenze von 256 Einträgen. Neue Objekte verwenden ihre eigene dynamische Abrechnung. Das Originalprogramm `initial-directory-removal` löscht das bereits vorhandene leere Verzeichnis des nativen Tests bei offener Referenz, verwendet den Namen für eine Datei und dann ein Verzeichnis, prüft die alleinige CWD-Referenz und stellt das leere Verzeichnis wieder her. Dasselbe Programm läuft über C++/C/CLI/Python in allen fünf Gastkombinationen.

### Prüfung der Entfernung anfänglicher Verzeichnisse, 2026-10-06

Der abschließende Release-Quellstand gleicht 1.089 Darwin-Tests ab: 657 bestanden, 432 wegen nicht verfügbarem Backend übersprungen, keine Fehler; alle 108 verpflichtenden ARM64-HVF-Fälle wurden ausgeführt. Die gezielte Prüfung bestand 27/39 Fälle bei 12 Auslassungen; auch die zusätzliche Prüfung von Aliasen allein aus Verzeichnisschnappschüssen bestand. Öffentliche C/CLI/Berichte: 191/191; Python: fünf Kombinationen in 76,276 s; eigene native Programme: 26/26; Evidenz-Runner: 66/66. Ein widersprüchlicher Inode/Schnappschuss-Test wurde berichtigt; seine Fehler bleiben erhalten.

Zwei frühere vollständige Läufe hatten einen bzw. drei Zeitüberschreitungen in vorhandenen Datei/Umbenennungsfällen. Ein instrumentierter Lauf reproduzierte eine nach 5,008 s Echtzeit bei 0,171 s Prozess-CPU-Zeit. Vergleiche derselben Methoden und alter Programme bestanden, doch die Latenzursache bleibt ungeklärt; der abschließende Erfolg beweist keine Zeitlimitstabilität. Temporäre Diagnostik wurde entfernt, Programm-Hashes wiederhergestellt und die ursprüngliche Gastgrenze von 5 s beibehalten. Primäre Quell-/Evidenzprüfung abgeschlossen; unabhängige Prüfung nicht verfügbar. Die Zahlen überlappen. Vollständige GitHub-CI, physisches iOS und ausgesetzte Intel-HVF-Actions liegen außerhalb dieser lokalen Abnahme.

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## Exklusives Umbenennen regulärer Dateien

Nach Quell- und Zielauflösung liefert RENAME_EXCL für eine andere vorhandene Datei oder ein Verzeichnis EEXIST, noch vor Mount- und Änderungsprüfungen. Frühere Pfadfehler einschließlich EINVAL bei abschließendem Punkt/Doppelpunkt bleiben vorrangig. Ein fehlendes Ziel nutzt dieselbe begrenzte Transaktion unter Erhalt offener Beschreibungen, Cursor, Flags, Mapping-Leases und konfigurierter Metadatenübergänge. Dasselbe Objekt bleibt ausdrücklich nicht unterstützt: Das native Ergebnis hängt von der Groß-/Kleinschreibung des Dateisystems ab, die exakte Katalogschlüssel nicht belegen. Namensfaltung, anfängliche Verzeichnisquellen und SECLUDE bleiben außerhalb. Das eigene Programm `renamed-file` vergleicht Ablehnung ohne Metadatenänderung und erfolgreiche EXCL|NOFOLLOW_ANY-Verschiebung auf nativem macOS sowie über C++/C/CLI/Python.

Prüfung, 2026-10-06 (Release): 1.097 Darwin-Tests, 665 bestanden, 432 wegen nicht verfügbarem Backend übersprungen, keine Fehler; alle 108 verpflichtenden ARM64-HVF-Fälle ausgeführt. Gezielt: 44 bestanden, 12 ausgelassen, darunter acht neue direkte Fälle. C/CLI/Berichte: 191/191; Python: fünf Kombinationen in 19,241 s; unabhängige Rohaufrufprobe: 26 Prüfungen bestanden. Der erste native Gesamtlauf überschritt bei return das Zeitlimit; die übrigen 25 einschließlich renamed-file bestanden. Drei return-Nachprüfungen derselben unveränderten Binärdatei dauerten 0,014–0,034 s, danach bestanden alle 26 Fälle mit der ursprünglichen 5-s-Grenze. Der erste Fehler bleibt dokumentiert und ungeklärt; auch der vorherige abschließende HVF-Erfolg belegt keine Latenzstabilität. Primäraudit abgeschlossen, unabhängige Prüfung nicht verfügbar. Zahlen überlappen; physisches iOS, vollständige GitHub-CI und ausgesetzte Intel-HVF-Actions sind nicht Teil der lokalen Abnahme.

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.


## Umbenennen zwischen erstellten Verzeichnissen

Ein anfängliches Verzeichnis und alle durch diesen Prozess mit mkdir/mkdirat erstellten Nachfahren teilen eine virtuelle Namensraumdomäne. `rename`, `renameat` und `renameatx_np` dürfen reguläre Dateien zwischen diesen Eltern verschieben; kein neues JSON-Feld ist nötig. Nach Erstellung von `/work/left` und `/work/right` im veränderbaren anfänglichen `/work` sind Bewegungen von `/work/data` in die Kinder und zwischen ihnen möglich. Ein getrennt angegebenes anfängliches `/work/left` bleibt eine eigene Domäne, auch bei gleicher Gerätenummer. Allgemeine Mount-Topologie bleibt unbekannt.

Beide direkten Eltern brauchen Namensraumfreigaben; erstellte Verzeichnisse erben diese und bekannte Geräte-/Gruppenwerte. Dateiidentität, Eigentümer/Gruppe, Schreibfreigabe und Belegung bleiben erhalten. Bekannte Gerätekonflikte werden abgelehnt. Echtes Verschieben invalidiert vollständige stat-/Aufzählungsbeobachtungen beider Eltern. Gelöschte Anfangsverzeichnisse und wiederverwendete Pfade bleiben verschiedene Objekte; alte FD/CWD erhalten keine neue Domäne.

Begrenzte Ersatztransaktion, Mapping-Leases, Pfad/NUL-Kosten und Fehlerpriorität gelten weiter. EXCL liefert bei einem anderen bestehenden Ziel EEXIST vor Domänen-/Freigabeprüfungen. `renamed-file` vergleicht Bewegungen ins erstellte Kind, Ersatz im Anfangselternteil und Rückkehr über C++/C/CLI/Python und natives macOS. Rechteprüfung, Verschieben anfänglicher Verzeichnisse, harte Links, dynamische symbolische Links und native APFS-Metadaten bleiben offen.

### Prüfung über verschiedene Eltern, 2026-10-06

Die abschließende Release-Prüfung glich 1,115 Darwin-Registrierungen ab: 683 bestanden, 432 wegen nicht verfügbarem Backend übersprungen, keine Fehler; alle 108 erforderlichen ARM64-HVF-Fälle wurden ausgeführt. Direkte Prüfungen bestanden 56/56, darunter 18 neue 4K/16K-Fälle. Öffentliche C/CLI/report-Prüfungen bestanden 191/191; die Python-Methode deckte fünf Profile in 22.254s ab. Ursprüngliche native Programme: 26/26; separate Rohaufrufsonde: 34 Prüfungen; Dokumentations-/Funktions-/Nachweisrunner-Skripte: 296/296. Die Zählungen überschneiden sich. Nach der auf MSVC begrenzten CMake-Anpassung blieben die Hashes aller zehn Prüfbinärdateien unverändert.

Die beiden früheren Gesamtläufe bewahren drei und zwei Zeitüberschreitungen in der vorhandenen HVF-Dateimethode. Vergleiche der vollständigen Methode, Arbeitsverzeichnisse und Sitzungen bestanden, klärten aber die Ursache nicht; der abschließende Erfolg belegt keine stabile Latenz. Die Sonde verglich zunächst /tmp mit dem kanonischen /private/tmp; der Pfad des Root-FD korrigierte vier Erwartungen. Das erweiterte native Programm verwendete mkdir(136) zur Bereinigung; rmdir(137) behob exit150. Ursprüngliche Quellen und Fehler bleiben erhalten; die Gastgrenze bleibt 5s.

Vier von der vollständigen Linux-CI gemeldete LP64-Initialisierungslistenkonflikte verwenden jetzt explizite uint64_t-Werte; NeverDJumpTableTests erhält unter MSVC /bigobj. Tatsächliche Linux-/Windows-Kompilierung steht in der CI noch aus. Die frühere Gesamt-CI meldete außerdem getrennte Fehler im Windows-EH-Korpus und beim Abbrechen einer geschlossenen PR. Die Quellen und Nachweise wurden selbst geprüft; eine unabhängige Prüfung, physisches iOS oder die ausgesetzte Intel-HVF-Abnahme werden nicht behauptet.

`build-hvf-arm64/cross-parent-rename-validation-summary.json`, `cross-parent-rename-darwin-synced-evidence/`, `cross-parent-rename-darwin-evidence/`, `cross-parent-rename-darwin-rechecked-evidence/`, `cross-parent-rename-public-synced.xml`, `cross-parent-rename-python-synced.log`, `cross-parent-rename-native-fixed-evidence/`, `cross-parent-rename-probe/`.

## Atomarer Austausch regulärer Dateinamen

RENAME_SWAP=0x2 tauscht durch renameatx_np die Namen zweier vorhandener regulärer Dateien, optional mit RENAME_NOFOLLOW_ANY. Ein explizites Anfangsverzeichnis muss mutable:true und swap_rename:true erklären; C++ verwendet DarwinFileOptions::SwapRenameDirectories. Erzeugte Nachfahren erben die Fähigkeit des ursprünglichen Verzeichnisobjekts. Löschen und Wiederverwenden eines Pfads überträgt keine alte Erklärung. false oder Weglassen bedeutet unbekannt; gleiche Geräte und Namespace-Rechte beweisen keine Unterstützung. Verschiedene Anfangsdomänen bleiben ausgeschlossen.

Beide Pfade verwenden den vorhandenen Komponentenresolver. Ein fehlendes Ziel liefert ENOENT vor Domänen-, Rechte- und Fähigkeitsprüfung. Verzeichnisoperanden bleiben ausdrücklich nicht unterstützt: natives Swap kann Datei und Verzeichnis tauschen, weshalb gewöhnliches EISDIR hier nicht gilt. Derselbe autorisierte Gegenstand ist ein No-op, auch ohne Fähigkeitserklärung. RENAME_EXCL=0x4 + RENAME_SWAP=0x2 und unbekannte Flags liefern vor Pfadzugriff EINVAL; SECLUDE bleibt offen.

Beide Dateien bleiben verknüpft. Eigene Identität, Besitzer/Gruppe, Bytes, Schreibrechte, offene Beschreibungen, Cursor, Flags und Mapping-Leases bleiben erhalten. Konfigurierte virtuelle Regeln ändern jeweils die eigene ctime; fehlende oder ungültige Regeln lassen vollständige Metadaten unbekannt. Tatsächlicher Austausch invalidiert vollständige Metadaten und Enumeration beider Eltern. Er verbraucht kein Erzeugungs-inode, keinen Eintrag und keinen FD; Eingaben bleiben unverändert.

Jede Fähigkeitserklärung reserviert Pfad+NUL im festen anfänglichen 16-MiB-Budget. Die Transaktion prüft beide vollständigen dynamischen Namenskosten vor Veröffentlichung; weiterhin verknüpfte Bytes und Leases liefern keinen Ersatzkredit. Wiederholter Austausch nutzt diese Kosten erneut. Das ursprüngliche renamed-file-Programm tauscht über ein erzeugtes Kind und zurück, prüft beide Objekte und setzt gewöhnlichen Ersatz fort, auf nativem macOS und allen C++/C/CLI/Python-Profilen. Rechteprüfung, Mount-Topologie, Namensfaltung, Verschieben anfänglicher Verzeichnisse bleiben separate Arbeit.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/work","mutable":true,"swap_rename":true}]}}
```

[Apple volume swap capability](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsswaprenaming?changes=__1_2), [XNU rename](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

### Swap-Prüfung, 2026-10-06

Release Darwin: 1.133 Registrierungen, 701 bestanden, 432 wegen nicht verfügbarer Backends übersprungen, keine Fehler; alle 108 erforderlichen ARM64-HVF-Fälle ausgeführt. Direkte Rename-Prüfungen 68/68, darunter 18 neue Options- und 4K/16K-Fälle. Öffentliches C/CLI/Report 192/192; die Python-Methode deckte fünf Profile in 16,153s ab. Native Originalprogramme 26/26, unabhängige Rohaufrufprobe 45 Prüfungen. Zahlen überlappen; Gastfristen bleiben unverändert.

Zwei erste direkte Tests hatten falsche Erwartungen für unbekannte Schreibberechtigung und unbekannte Metadaten nach Mutation; nur Erwartungen wurden korrigiert. Ein erster JSON-Filter wählte null Tests und zählt nicht; der richtige Besitzer und die komplette öffentliche Prüfung bestanden anschließend. Erste Quellen und Ergebnisse bleiben erhalten. Frühere HVF/native Latenz ist ungeklärt; dieser Pass beweist keine Stabilität. Quellen/Evidenz wurden selbst geprüft; keine unabhängige Prüfung, physische iOS-, vollständige GitHub-CI- oder ausgesetzte Intel-HVF-Abnahme wird behauptet.

`build-hvf-arm64/swap-rename-validation-summary.json`, `swap-rename-darwin-evidence/`, `swap-rename-public.xml`, `swap-rename-python.log`, `swap-rename-native-evidence/`, `swap-rename-probe/`.


## Rename vom Prozess erzeugter Verzeichnisse

Gewöhnliches rename, renameat und renameatx_np verschiebt ein lebendes erzeugtes Verzeichnis samt Teilbaum innerhalb einer Anfangsdomäne; beide unmittelbaren Eltern brauchen Berechtigung. Ein fehlendes Verzeichnisziel darf abschließende Slashes haben. Datei als Ziel: ENOTDIR; nichtleeres Verzeichnis: ENOTEMPTY; Verschieben in Nachfahren: EINVAL. Berechtigter gleicher Name bleibt unverändert. EXCL liefert EEXIST für ein anderes vorhandenes Ziel vor Typ, Zyklus, Domäne und Freigabe. Zielfehler kommen vor Quellpunkten. Quellpunkt/Doppelpunkt zum selben Objekt bleibt UnsupportedService, da Dateisystem-Namensfaltung unbekannt ist. Anfängliche/entfernte Quellen, anfängliche Ersatzverzeichnisse, getrennte Domänen, Links, Rechteprüfung und SECLUDE bleiben offen.

Elternobjektketten bestimmen Zugehörigkeit: benannte Kinder, gehaltene entfernte Verzeichnisse und FD/Mapping-gehaltene verwaiste Dateien erhalten neue Pfade. Quell-FD, dup, CWD und Doppelpunkt folgen dem gleichen Objekt und neuen Elternteil. Das ersetzte leere Verzeichnis behält alten Pfad/Elternteil; gewöhnliche Kinder geben ENOENT, Punkt/CWD halten das alte Objekt. Alte Zielwaisen folgen dem erneut verschobenen Ersatz auch bei identischen Pfadtexten nicht.

Kinderbytes, Identität, Metadaten, Schreibrecht, Cursor, FD-Flags und Mappings bleiben erhalten. Quelle und Eltern verlieren vollständige Beobachtungen; erzeugte Verzeichnisse behalten ihre geerbte Autorität für weitere Erstellung/zugelassenes Rename. Kein neuer Eintrag, FD oder inode. Vor Veröffentlichung werden alle lebenden/gehaltenen Schlüssel und Pfade vorbereitet und gegen 1024 Byte samt NUL und 16 MiB geprüft. Nur ein Ziel ohne FD/CWD/gehaltenen Nachfahren liefert einmaligen Rückgewinnungskredit. Ablehnung erhält Namen, Eltern und Beobachtungen. Mapping-Waisen halten auch Kosten entfernter Eltern.

Das eigene SDK-free `renamed-directory` vergleicht natives macOS und fünf Gäste via C++/C/CLI/Python; 4K/16K prüft Wiederverwendung, Rollback, exakte Kapazität, lange Nachfahren, Zielkredit, Elternfreigabe und Eintrags/FD/inode-Erschöpfung.


### 2026-10-06

Finales Release:1,175 Darwin-Registrierungen,731 bestanden,444 nicht verfügbare Backend-Skips,kein Fehler;111 Pflichtfälle ARM64 HVF ausgeführt. Fokussiert32/44 (12 Skips,22 neue4K/16K),finale Grenzen4/4. C/CLI165/165 und Reports32/32 ohne Skips;Python fünf Profile21.759s,nativ27/27,unabhängige Probe79. Unabhängige Prüfung bestätigt den Root-Trennzeichenfix im Programm und die FS-Eigenschaftsgrenze für Quellpunkte desselben Objekts. Erste exit124 und zwei veraltete fehlschlagende Erwartungen samt Quellen/Binärdateien bleiben erhalten;finale vollständige Prüfung bestanden. Zahlen überlappen,Fristen unverändert. Frühere HVF/native Timeouts bleiben ungeklärt,kein Stabilitätsnachweis. Intel HVF Actions ausgesetzt;physisches iOS und vollständige GitHub CI separat.

`build-hvf-arm64/directory-rename-validation-summary.json`, `directory-rename-darwin-final-evidence/`, `directory-rename-public.xml`, `directory-rename-reports.xml`, `directory-rename-python.log`, `directory-rename-native-evidence/`, `directory-rename-probe/`, `directory-rename-initial-fixture/`, `directory-rename-darwin-evidence/failed-source/`.

### Prüfung von Verzeichnis- und gemischten Austauschen, 2026-10-07

Der Release-Darwin-Lauf gleicht 1.219 Registrierungen ab: 763 bestanden, 456 wegen nicht verfügbarer Backends übersprungen, keine Fehler. Alle 114 vorgeschriebenen ARM64-HVF-Fälle liefen. Die gezielte Prüfung besteht 32/44 Fälle mit 12 nicht verfügbaren Fällen, einschließlich aller 24 neuen direkten 4K/16K-Fälle. Der vollständige Dateibereich besteht 317/317; native Originalprogramme 28/28. Eine unabhängige direkte Systemaufrufprobe erfasst 32 erfolgreiche Beobachtungen auf diesem macOS-Dateisystem ohne Groß-/Kleinschreibung. Zahlen überschneiden sich; Gastfristen bleiben unverändert.

Eine unabhängige Plan- und Implementierungsprüfung untersuchte die beidseitige Transaktion, Pfadkosten initialer Dateien, beide gehaltenen Teilbäume, nur durch Mapping gehaltene verwaiste Objekte und genaue Rückerstattungen. Der erste rote Test ließ die ausdrückliche Austauschdeklaration für die Wurzel aus und zählt nicht zur Abnahme. Die korrigierte Ausgangsversion scheiterte in allen sechs ausgewählten Fällen an der alten Verzeichnisverweigerung. Zwei frühe Assertions für gemischte Dateien verwarfen konfigurierte Metadaten fälschlich; jetzt vergleichen sie den gesamten Datensatz mit ausschließlich geändertem ctime. Eine lokale konstante Zeigertabelle erzeugte ARM64-Rebases und sechs Ladeverweigerungen. Vier skalare Assertions ersetzen sie; das korrigierte Programm hat keine klassischen Rebases, und der Loader verweigert weiterhin nicht unterstützte Fixups.

Der erste korrigierte gezielte Lauf bewahrt drei HVF-Zeitüberschreitungen von fünf Sekunden. Einzel- und Drei-Profil-Kontrollen sowie die spätere vollständige Prüfung bestanden. Die Ursache bleibt unbekannt; Verzögerungsstabilität ist damit nicht bewiesen. Ursprüngliche Quellen, Binärdateien, Fehler und Kontrollen bleiben erhalten. Initiale Verzeichnisverschiebungen, getrennte initiale Domänen, Berechtigungs-/Mount-/Großkleinschreibungsdeklarationen, dynamische Abhängigkeiten und Framework-Laufzeit bleiben offen. Physisches iOS, ausgesetztes Intel HVF und vollständige GitHub CI sind gesonderte Abnahmegrenzen.

`build-hvf-arm64/directory-swap-research/`: `darwin-final-evidence/`, `darwin-evidence/`, `focused-v5.xml`, `file-owner-v5.xml`, `native-workload-fixed-evidence/`, `native-probe-v2-results.json`, `fixture-fixups-before/`, `hvf-controls.json`.

Die endgültig gelinkten Binärdateien bestanden die vollständige Darwin-Prüfung erneut. Die eingefrorene dev-Integration 0a9a1d28d erfasst 4.515 Fälle in 20 gemeinsamen Komponenten: 4.489 bestanden, sechs optionale Z3-Fälle und 20 nicht verfügbare Windows-EH-Korpusfälle übersprungen, keine Fehler. C/CLI 170/170 und Berichte 32/32 sind enthalten. Python prüft alle fünf Profile in 30,780s. Alle zwölf nativen mobilen Architektur/Fixup-Varianten stimmen in 4.532 Beobachtungen mit den Originalprogrammen überein; Einzelsitzung und vollständige Swift-Metadaten stimmen 12/12 überein. Der Swift-Witness-Generator reproduziert seinen Katalog nach einer Kommentar-Einrückungskorrektur mit dem dokumentierten SDK/Compiler. Dies ist lokale Release-LLVM-23/Apple-Clang-17-Abnahme, keine Abnahme späterer dev-Stände oder Linux Clang 18.


## Gewöhnliches Verschieben deklarierter anfänglicher Verzeichnisbäume

Das strikte Boolean `"movable": true` an einem expliziten anfänglichen Nicht-Wurzelverzeichnis erlaubt dessen gewöhnliches rename und deklariert den gesamten anfänglichen Teilbaum als gewöhnliche Verzeichnisse ohne Mounts oder Namensaliase. `DarwinFileOptions::MovableDirectories` wird hinter den bisherigen C++-Aggregatfeldern ergänzt. Der direkte Elternknoten muss mutable sein. Fehlend/false bleibt unbekannt; andere JSON-Typen werden abgelehnt. Nachfahren behalten ihre eigenen mutable/removable/movable- und Dateischreibrechte. Dies erlaubt weder anfängliche Verzeichnis-SWAPs noch allgemeine Rechte oder Mounts. Bekannte flags, spezielle Verzeichnismodi, mehrfach verlinkte reguläre Dateien und inode-Aliase aus stat/Snapshots werden abgelehnt. Jede durch die Deklarationen verbundene Domäne darf nur eine bekannte Gerätenummer enthalten, einschließlich benachbarter Teilbäume und Dateien unter Vorfahren ohne stat. Gleiche Nummern verbinden keine andere Domäne.

Objekte behalten Namen, Eltern, ursprüngliche stat/Snapshots und Rechte. Alte Eingabepfade erzeugen verschobene oder entfernte Namen nicht erneut. Ungeöffnete Nachfahren, FD/dup/CWD, Mappings und entfernte Nachfahren folgen ihrem Objekt. Unveränderte Nachfahren behalten stat, Snapshot, Cookie und SEEK_END; verschobene Wurzeln und geänderte Eltern verlieren vollständige Beobachtungen. Namenswiederverwendung übernimmt keine Beobachtungen oder Rechte. Die Anfangseingabe gilt für einen synchronen Aufruf, nicht für einen Austausch während der Ausführung. SWAP bleibt getrennt: direkte swap_rename-Deklaration anfänglicher Objekte, Übernahme vom Elternobjekt bei mkdir, keine Neuberechnung beim Verschieben. Beide SWAP-Eltern benötigen Unterstützung.

Ein leeres anfängliches Ersetzungsziel braucht zusätzlich removable. Vor Veröffentlichung gelten 1023 Byte je Pfad und das gemeinsame 16-MiB-Budget. Jede movable-Referenz reserviert den ursprünglichen Pfad plus NUL dauerhaft, ohne zusätzlichen Eintrag. Anfängliche Verzeichnispfade, Referenzen, Snapshots und Einträge bleiben nach Entfernung reserviert. Dynamisches PathCharge beginnt bei null und berechnet beim Verschieben einmal den aktuellen Pfad jedes verbundenen oder gehaltenen Mitglieds. Nur die vorhandene dynamische Gebühr eines sofort freigebbaren Ziels ist Guthaben; FD/CWD/Kinder/verwaiste Mappings liefern keines. Endgültige Rückgewinnung erstattet einmal. Implizite Anfangsvorfahren zählen nicht zusätzlich zur Grenze von 256 Einträgen; die Rückgewinnung anfänglicher regulärer Dateien bleibt bestehen.

Beispiel:

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/work","mutable":true,"movable":true}]}}
```

initial-directory-move prüft ohne SDK Verschieben, geteilte/unabhängige Cursor, FD-flags, CWD, privates Mapping, Ersetzen und Namenswiederherstellung. Lokale Guest-Profile und physisches iOS sind getrennte Nachweise.

Die Grenze für SWAP von Anfangsverzeichnissen betrifft die Quell- und Zielwurzel des Systemaufrufs. Der Austausch erstellter Vorfahren darf bereits verschobene anfängliche Nachkommen mitnehmen; Objektzustand und dynamische Kosten bleiben erhalten. Die vorherigen Einschränkungen gelten, wenn die erforderlichen Deklarationen fehlen.

Nach der unabhängigen Abschlussprüfung auf ARM64 macOS umfasst das Darwin-Gate 1.264 Tests: 796 bestanden, 468 wegen nicht verfügbarer Backends übersprungen, keine Fehler. Alle 117 verpflichtenden ARM64-HVF-Arbeitslasten wurden ausgeführt. Der Dateibereich besteht 342/342, einschließlich 22 neuer 4K/16K-Fälle und drei Zulassungsprüfungen. CreationPolicy behält Device/GID des verschobenen Elternobjekts; Namenswiederverwendung, Inodefolge und aktuelle umask bleiben getrennt. Bei exakt 16 MiB erzeugen 16 Hin- und Rücktausche keine kumulativen Kosten; das Zurückverschieben gibt nur die tatsächliche Differenz von sechs Bytes frei. Öffentliche C/CLI-Prüfungen: 175/175; Berichte: 33/33; nativer Kernel: 29/29; unabhängige Originalsonde: 19 erfolgreiche Beobachtungen. Fünf Python-Konfigurationen bestehen in 27.865 Sekunden, ebenso 71 reine API-Tests, SDK-Abgleich und 49 Runnerprüfungen. Die Zahlen überschneiden sich. Quellen, Binärdateien, Versuche und Ergebnisse liegen unter build-hvf-arm64/initial-directory-move/ und sind an den Commit gebunden. Physisches iOS, pausiertes Intel HVF, SWAP anfänglicher Wurzeln, Rechte/Mounts/Großschreibung, gemeinsames EOF, Mach/Threads/dyld und Frameworks bleiben eigenständige Lücken.

## Atomarer Austausch deklarierter Anfangsverzeichniswurzeln

Das strikte Boolean exchangeable:true (C++ DarwinFileOptions::ExchangeableDirectories, am Aggregatende) erlaubt nur eine explizite anfängliche Verzeichniswurzel außer / als RENAME_SWAP-Operand. Ihr direkter Anfangselternknoten muss mutable sein. Fehlend/false bleibt ausgeschlossen; andere Typen sind ungültig. Es teilt mit movable die gewöhnliche mountfreie, eindeutig benannte Unterbaumdeklaration sowie Prüfungen für flags/Sondermodi/Aliase/Hardlinks/Geräte im ganzen verbundenen Bereich. Die Vereinigung bestimmt nur Topologie. Jeder Verweis reserviert ursprünglichen Pfad+NUL separat, auch bei gleicher Wurzel, ohne neuen Eintrag. Gerätegleichheit verbindet keine Bereiche.

Gewöhnliche/EXCL-Anfangsquellen brauchen weiterhin movable, gewöhnliche anfängliche Ersatzziele removable. exchangeable erteilt diese Rechte, mutable-Nachkommen, Schreibrechte oder allgemeine Rechte/Mounts nicht. Verschiedene Objekte benötigen zwei tatsächlich mutable Eltern mit jeweils swap_rename. Ein erlaubter gleichnamiger SWAP gewöhnlicher Komponenten prüft Eltern/Geräte, bleibt dann unverändert, ohne erste Gebühr oder Fähigkeit für verschiedene Objekte. Dot/Großschreibung desselben Objekts bleiben unbekannt; fehlendes Ziel und Dot behalten ihre Reihenfolge.

Nichtleere Anfangswurzeln, anfängliche/erstellte Wurzeln und Verzeichnis/Datei werden in beiden Richtungen ausgetauscht. Beide verknüpften und gehaltenen Bäume werden vollständig geprüft; alle bewegten Namen werden vor Veröffentlichung entfernt. Beide Wurzeln bleiben verknüpft; kein Ersatz-/Inhaltskredit und keine neuen FD/Inodes/Einträge. FD/dup/CWD/Cursor, Elternobjekte, Leases und Rechte folgen ihren Objekten; unveränderte Nachkommen behalten stat/Snapshots. Alte gelöschte und neue gleichnamige Objekte bleiben getrennt. Dynamische Pfadkosten beginnen bei null, werden zuerst einmal berechnet, später ersetzt; feste Kosten bleiben reserviert. Pfad-/Budgetfehler erhalten beide Zustände.

Das originale SDK-freie initial-directory-swap tauscht vorhandenes empty und data und stellt sie wieder her, mit Nachkommen, Mapping, CWD, Cursorn, flags und Erstellung nach Verschieben. Die vorherige Annahme f98068c07 ist ein getrennt eingefrorenes Ergebnis gewöhnlicher Verschiebungen; erst diese Deklaration erweitert den anfänglichen Wurzel-SWAP.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/left","exchangeable":true},{"path":"/right","exchangeable":true}]}}
```


Die aktuelle Release-Prüfung auf macOS ARM64 umfasst 1,303 Darwin-Registrierungen: 823 bestanden, 480 wegen fehlender Backends übersprungen, keine Fehler; alle 120 verpflichtenden ARM64-HVF-Fälle wurden ausgeführt. Dateiprüfungen bestehen 361/361 (16 neue 4K/16K-Verhaltensfälle und 3 Zulassungen), C/CLI 180/180, Berichtparser 34/34, ursprüngliche Kernelprogramme 30/30 und die unabhängige Sonde 35 Beobachtungen. Python prüft fünf Konfigurationen in 33.894 Sekunden; 71 reine API-Tests, 49 Inventar-/Referenzeinheiten sowie SDK-, Format-, Fähigkeits-, Herkunfts- und Dokumentprüfungen bestehen. Die Zählungen überlappen.

An der Kapazitätsgrenze erhalten gleiche Namen und 16 Hin-/Rücktausche Objekte und Kosten. Bei sechs benötigten und fünf freien Bytes bewahren beide Ablehnungsrichtungen die Bäume, Cursor und das Erstellungsbudget. Unabhängige Plan- und finale Quellprüfungen sind akzeptiert. Versuche, Quellen, Binärdateien und Ergebnisse liegen eingefroren unter `build-hvf-arm64/initial-directory-swap/` und sind an den Commit gebunden. Ein überlappender Berichtslauf wurde ausgeschlossen und seriell wiederholt; Übersetzungsmarkierungen sind synchronisiert. Zeitgrenzen und Negativkontrollen bleiben unverändert. Rechte, nicht deklarierte Mounts/Großschreibung, gemeinsame Maps/EOF, fortschreitende Uhren, Mach/Threads/dyld/Frameworks bleiben offen; physisches iOS, ausgesetztes Intel HVF und entfernte Merge-CI sind separate Nachweise.

## Explizite schreibgeschützte Ressourcenlimits

`getrlimit(194)` liest in allen fünf Darwin-Gastprofilen `DarwinSystemOptions::ResourceLimits` (`darwin_system.resource_limits`). Ein `DarwinResourceLimit` für Schlüssel 0..8 enthält zwei Little-Endian-uint64 an Offset 0/8, insgesamt 16 Bytes. Es gilt `0 <= current <= maximum <= 9223372036854775807`; Null ist explizit, INT64_MAX bedeutet unendlich. Keine Hostabfrage oder Änderung der FD-, VM-, Speicher- oder Ausführungsbudgets. `setrlimit`, Durchsetzung, Signale und Scheduling fehlen weiterhin.

Striktes JSON erlaubt höchstens neun eindeutige Objekte mit genau `resource`, `current`, `maximum`, als exakte Ganzzahlen oder vorzeichenlose Dezimalstrings. Ungültige Typen, fehlende/fremde Felder, Duplikate, nichtkanonische Schlüssel und vertauschte Grenzen scheitern vor dem Laden. Leere/fehlende Arrays bleiben unbekannt. Konfigurationsschlüssel werden nicht normalisiert.

Nur der Syscall nimmt die unteren 32 Selector-Bits und entfernt `_RLIMIT_POSIX_FLAG=0x1000`. Ungültige Ressourcen liefern vor Speicherzugriff EINVAL; fehlende Beobachtungen bleiben vorher unsupported. Vollständig unbeschreibbarer Speicher liefert EFAULT; teilweise beschreibbare Paare bleiben ohne Schreibeffekt unsupported. Vollständige unaligned/seitengrenzenübergreifende Kopien ändern genau 16 Bytes; Backendfehler bleiben Transportfehler. Der native ARM64-Probe bestand 23 Wert-/Selectorprüfungen und vier separate Fehlerprüfungen; unveränderte Teilpräfixe auf diesem Host sind keine portable Garantie.

```json
{"darwin_system":{"resource_limits":[{"resource":8,"current":256,"maximum":"9223372036854775807"}]}}
```

macOS ARM64 Release: registriert/bestanden/nicht ausgeführt/fehlgeschlagen 1337 / 845 / 492 / 0; alle 123 erforderlichen HVF-Fälle ausgeführt. Zwölf neue 4K/16K-Verhaltensfälle und SDK-Capture-Orakel; C/CLI 190/190, Parser 37/37, native Workloads 31/31; Python über fünf Profile in 71.978 Sekunden, 71 API- und 49 Runnerprüfungen bestanden. SDK-Drift, Format, Fähigkeiten, Provenienz und Dokumentation bestanden; Zahlen überlappen. Quellen, Binärdateien und Versuche sind unter `build-hvf-arm64/resource-limit-observations/` eingefroren und an den Commit gebunden. Der erste Build enthielt einen Test-Enum-Tippfehler; Wiring- und Filterversuche bleiben erhalten, korrigierte Prüfungen liefen seriell ohne gelockerte Grenzen. Durchsetzung, Berechtigungen, Verzeichnisdaten nach Mutation, Shared-Mapping/EOF, fortschreitende Uhren, Mach/Threads/dyld und Frameworks fehlen weiterhin. Physisches iOS, ausgesetztes Intel HVF und Remote-Merge-CI brauchen eigene Abnahme.

## Explizite Ressourcenverbrauchswerte nur zum Lesen

getrusage(117) liest in fünf Darwin-Konfigurationen unabhängig optionale DarwinSystemOptions::ResourceUsageSelf / ResourceUsageChildren. Striktes JSON: darwin_system.resource_usage.self / .children. Jeder DarwinResourceUsage verlangt int64 user_seconds/system_seconds, uint32 user_microseconds/system_microseconds unter1000000 und genau14 int64 counters. Exakte Ganzzahlen oder vorzeichenbehaftete Dezimalstrings erhalten den ganzen Bereich; ungültige Typen/Felder/Mikrosekunden/Längen scheitern vor dem Laden. Fehlender Partner bleibt unbekannt, ohne den angegebenen zu blockieren; explizite Nullwerte sind gültig.

Eine vollständige little-endian Kopie von144 Byte: timeval bei0/16 (Sekunden8, Mikrosekunden4, Nullpadding4), counters ab32 je8. Darwin-Rohwerte und Einheiten bleiben erhalten, ru_maxrss ohne Linux-KiB-Umrechnung. Feste Werte implementieren keine Host-Leistungsmessung, Abrechnung, fork/wait, Planung oder Limitdurchsetzung.

Nur die unteren32 Selectorbits: 0=SELF, -1=CHILDREN; 0x1000 ungültig, kein Entfernen des POSIX-Flags. Ungültig ergibt EINVAL vor Speicherzugriff; fehlend unsupported vor Ausgabe. Vollständige unaligned/Seitenkopie erhält Guards; ganz unschreibbar EFAULT, teilweise unsupported vor jedem Byte; Backendfehler bleiben Transportfehler. SDK erfasst jeden Selector genau einmal und vergleicht keine veränderlichen späteren SELF-Werte. Native Probe13 Prüfungen plus4 unabhängige Fehlerprozesse bei unveränderten5s. Auf diesem Host schrieb partial SELF64 Byte vor EFAULT; daraus folgt keine allgemeine Präfixgarantie.

0..13: `ru_maxrss`, `ru_ixrss`, `ru_idrss`, `ru_isrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`, `ru_inblock`, `ru_oublock`, `ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`, `ru_nvcsw`, `ru_nivcsw`.

```json
{"darwin_system":{"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":0,"system_microseconds":0,"counters":["9223372036854775807",0,0,0,0,0,0,0,0,0,0,0,0,0]}}}}
```

[Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html), [XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [SDK layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

macOS ARM64 Release registriert/bestanden/ohne Ausführung übersprungen/fehlgeschlagen 1371/867/504/0, alle 126 Pflicht-HVF ausgeführt. File361/361, C/CLI 200/200, JSON 40/40, System/SDK 53/53, native 32/32 bestanden. Zwölf neue4K/16K-Fälle plus Zulassung/SDK, Python fünf Konfigurationen 42.865s; API71, runner/reference49 und SDK-drift/Format/Fähigkeiten/Herkunft/Dokumentation bestanden. Zahlen überlappen, unabhängige Prüfung bestanden. Belege unter `build-hvf-arm64/resource-usage-observations/` eingefroren und an Commit gebunden. Erste x64-EINVAL-Assertion auf RDX-Erhalt korrigiert, ARM64 löscht X1; Fehler/leerer Filter erhalten. Runtime/Fristen/Negativkontrollen unverändert. Limits, Rechte, Verzeichnisdaten nach Mutation, shared maps/EOF, Uhren, Mach/thread/dyld, Frameworks unvollständig; physisches iOS, ausgesetztes Intel HVF und Merge-CI separat.

Erster Gesamtgate:866 bestanden, ein bestehender iOS ARM64 HVF rename5s-Timeout,504 skips. Gleiches Binary besteht den Fall in386ms, danach der vollständige serielle Gate. Beide Läufe erhalten; Ursache ungeklärt, keine Latenzgarantie.


## Explizite Identitäten, Gruppen und konsistenter Ersteller

Optionale Credentials enthalten RealUID/EffectiveUID/RealGID/EffectiveGID und unabhängig optionale GroupAccessList und GroupMembershipUID. Ohne Credentials vier Getter1000; explizit0/root gültig, vier skalare IDs/Gruppeneinträge0..INT32_MAX; GroupMembershipUID hat die eigene Spanne unten. Gruppen1..16, erste=EffectiveGID, Reihenfolge/Duplikate erhalten; fehlend unbekannt, keine Host/EGID-Ableitung. darwin_system.credentials verlangt genau real_uid/effective_uid/real_gid/effective_gid, unabhängig optional groups und group_membership_uid. Verlustfreie Ganzzahlen/zentraler Validator verweigern Form/Felder/Bereich/Anzahl/erste Inkonsistenz vor Laden; NichtDarwin ebenso.

getuid24/geteuid25/getgid47/getegid43/getgroups79 haben einen Systembesitzer. Neue reguläre Datei benutzt effektiveUID, device/GID vom direkten Elternteil; rename/gehalteneFD/Namensreuse erhalten Objekt, Inputstat unverändert. Root gewährt keine Schreib/Verzeichnis/ACL-Rechte; setuid/setgid/setgroups und Prozess/Session fehlen.

getgroups interpretiert low32 als signedint: negativ zuerstEINVAL, unbekanntunsupported, bekannte0=count ohne Zeiger, positiver KurzpufferEINVAL vor Speicher; ausreichend einmal4*count little-endian Bytes. 0x1000 positiv, kein POSIXflag entfernen. Vollständige Guards unaligned/über Seiten; ganz unbeschreibbarEFAULT, teilweiseunsupported vor Byte, Backendfehler bleiben Transport. BSD-Fehler erhalten x64RDX/löschen ARM64X1; Erfolg löscht beide sekundären Resultate, Report behält Rohargumente.


~~~json
{"darwin_system":{"credentials":{"real_uid":101,"effective_uid":202,
 "real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}}}
~~~


[Apple getgroups contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getgroups.2.html), [pinned XNU credential/group ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c).

registriert/bestanden/unverfügbare skips/fehlgeschlagen: 1413/897/516/0; ARM64 HVF 129/129. System/SDK72/72, File365/365, Report43/43, C/CLI210/210; guest8/12skip; native33/33; Python5 82.918s; API71, runner/reference49. SDK drift / clang-format22.1.2 / capabilities23 / provenance / docs11+negative3: OK. Independent source review: OK.

21 value +5 native fault,5s; host16groups, positive short capacity: OK. Native partial32byte thenEFAULT: observation only. Counts overlap. `build-hvf-arm64/credential-observations/`.

Erster Lauf8 Fehler (5 Instruktionsbudgets,3 Fünfsekundenfristen),12skips. Zweitseiten-Scan im Fixture auf komplette132Byte am gleichen Seitenübergang begrenzt (64 davor,bis64 Daten,mindestens4 danach); direkter Owner prüft beide ganzen Seiten. Budgets/Argumente/Negativkontrollen unverändert, Quellen/Binaries/beide Läufe erhalten. Transportgeometrie vor Ausführung nach Review korrigiert; Public-Auswahl vergleicht Inhalte. Rechte/ACL, Links, Directorybeobachtungen nach Mutation, shared maps/EOF, Uhren, Mach/thread/dyld/framework offen; physisches iOS, pausiertes IntelHVF, mergeCI separat.

## Deskriptortabelle aus deklarierten Prozess- und Kernelgrenzen

`DarwinSystemOptions::MaxFilesPerProcess` / `darwin_system.max_files_per_process` deklariert optional einen nichtnegativen int. Fehlend bleibt unbekannt, explizite Null ist gültig. `kern.maxfilesperproc` und MIB `[1,29]` lesen unabhängig von Ressourcenlimits dieselben vier Byte. Verlustfreie Ganzzahlprüfung und zentraler Validator verweigern falsche Typen, negative und zu große Werte vor dem Laden; Nicht-Darwin-Profile ebenso.

BSD `getdtablesize(89)` benötigt diese Grenze und `ResourceLimits[8].Current` und liefert das Minimum. Der volle 64-Bit-Current wird vor int-Konvertierung begrenzt: `0x100000001` mit cap=64 liefert64, auch Infinity wird sicher begrenzt. Maximum, Hostwerte, aktive FD-Anzahl und `DescriptorLimit` ersetzen keine Beobachtung. Ein fehlender Partner bleibt auch bei bekanntem Nullwert unsupported. Alle sechs Argumente bleiben unbeachtet, ohne Zugriff auf Userspeicher; vorhandene BSD carry/sekundäre Register gelten. Mach timebase trap89 bleibt getrennt.

Die sysctl-Kopierphasen bleiben erhalten. EUID0-Schreiben stoppt nach Namen/MIB und oldlenp-Vorprüfung, vor Beobachtung/Ausgabe unsupported; nicht-root erhält EPERM. Neuer Zeiger mit Länge Null bleibt Lesen. Limits oder Schreibrechte werden nicht daraus abgeleitet. Das Pflicht-Workload prüft beide cap-Abfragen und niedrige/hohe syscall numbers mit unverändertem `l` /144-Byte-Ausgang; spätere Ablehnung bewahrt bereits ausgegebene Bytes. Der skalare Fixture prüft fehlende Werte, Null, breiten Current und Unabhängigkeit von DescriptorLimit=3.

```json
{"darwin_system":{"max_files_per_process":64,"resource_limits":[{"resource":8,"current":"4294967297","maximum":"9223372036854775807"}]}}
```

[XNU getdtablesize](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU proc_limitgetcur_nofile](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [XNU MIB constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/sysctl.h).

Prüfung: Darwin (registriert/bestanden/nicht verfügbar/fehlgeschlagen) 1441/913/528/0; ARM64 HVF 132/132; System/SDK 80/80; File 365/365; C/CLI 211/211; ProcessReport 45/45; native 33/33; Python 5 (42.254s); API 71; runner 44 + reference 5. clang-format 22.1.2; capabilities/provenance; docs 286 / locales 10 / negative controls 3.

Versuchsverlauf: Die erste Runner-Prüfung scheiterte für ARM64 und x86-64, weil die neue skalare Methode nicht im Pflichtinventar stand. Die Registrierung wurde ergänzt, ohne die Gleichheitsregel aller Methoden abzuschwächen. Das erste Gate mit 129 Pflichtfällen und die Fehler bleiben erhalten; das neue abschließende ARM64-Gate verlangt 132 Fälle.

Zählungen überschneiden sich. `build-hvf-arm64/descriptor-table-observations/` bewahrt die tatsächliche Ausführungsbasis und exakte Quell-/Binär-/Loghashes; frühere Belege bleiben unverändert. Fünf wegwerfbare ARM64-macOS-Kindprozesse,40 Prüfungen, je fünf Sekunden Frist: Current=1048575/cap=245760 ergibt245760; Kinder mit Current=0/1/32/245777 ergeben0/1/32/245760. Eltern- und Systemlimits bleiben unverändert. Physisches iOS, pausiertes Intel HVF und remote merge CI sind separate Abnahmen. Rechte, Verzeichnisbeobachtungen nach Änderung, shared maps/EOF, fortlaufende Uhren, Mach/thread/dyld und Frameworks bleiben offen.

## Explizite Prozessbeobachtungen

`DarwinSystemOptions::ProcessGroupID`, `SessionID` und `ProcessTainted` sind unabhängige optionale Eingaben: JSON `process_group_id`, `session_id`, `process_tainted`. IDs müssen positiv und höchstens INT32_MAX sein; der Kontaminationszustand akzeptiert ausschließlich JSON Boolean `true`/`false`. Fehlend bleibt unbekannt, explizites `false` ist bekannte Null. Kein Wert wird vom Host, PID1000, Anmeldedaten oder einer anderen Beobachtung abgeleitet.

Der rohe Aufruf `getpgrp(81)` liest die Prozessgruppe; `getpgid(151)` und `getsid(310)` verwenden die vorzeichenbehafteten unteren32 Bits von `pid_t`. Null oder die feste eigene PID1000 wählen den eigenen Prozess; auch `0xffffffff000003e8` bezeichnet ihn. Negative untere PID-Werte liefern ESRCH3 vor der Beobachtungsauswahl, bestätigt durch rein lesende native Proben und XNU-Prozesszuweisung/-suche. Unbekannte positive andere Prozesse einschließlich `0x1000` stoppen mit UnsupportedService, ohne geratenes ESRCH oder Flag-Maskierung. Ein fehlender ausgewählter Eigenwert stoppt ebenfalls als nicht unterstützt. `getpgrp` und `issetugid(327)` ignorieren alle Argumente; alle vier Skalarabfragen greifen nicht auf Gastspeicher zu. BSD carry und die Regeln für das zweite Rückgaberegister bleiben bestehen.

`process_tainted` liefert die feste `P_SUGID`-Beobachtung unabhängig davon, ob reale und effektive IDs gleich sind. EUID, Dateieigentum, sysctl-Schreibbefugnis, Entitlements, Führungsrolle und Terminalzustand ändern sich nicht. `setpgid`, `setsid` und Anmeldedatenänderungen bleiben nicht unterstützt. Das originale rein lesende Programm `process-observations` erfasst die eigene PID und prüft hohe Trägerbits, negative Fehler und Rückgabestatus; `virtual-process-observations` gibt konfigurierte Gruppen-/Sitzungs-/Kontaminationsbytes aus. Modellfälle für fremde Prozesse, fehlende Werte und Setter gehören nicht zum nativen Inventar. macOS-Referenzen sind keine Abnahme auf physischer iOS-Hardware.

```json
{"darwin_system":{"process_group_id":7,"session_id":16909060,"process_tainted":false}}
```

[XNU process queries](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c), [XNU service numbers](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/syscalls.master), [XNU PID allocation](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_fork.c), [XNU PID lookup](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_proc.c).


## Expliziter Sitzungsanmeldepuffer

`DarwinSystemOptions::LoginNameBytes` deklariert unabhängig alle 255 rohen Sitzungsbytes (`MAXLOGNAME`). JSON `login_name_hex` verlangt genau 510 ASCII-Hexadezimalziffern, Groß- und Kleinschreibung zulässig. Eingebettete NUL und Nichtnullbytes nach einem Abschluss bleiben gültig. Fehlend bedeutet unbekannt, ein vollständig genullter Puffer ist explizit bekannt. Kurze Namen werden nicht aufgefüllt; Host, Zugangsdaten, Gruppe, Sitzung und Taint liefern keine Vorgaben. Die Beobachtung erteilt keine Anmelde- oder Dateirechte.

`getlogin(49)` nutzt die vorzeichenlosen unteren 32 Längenbits (`u_int`) und kopiert genau min(length,255) Bytes, ohne Textauswertung, zusätzliches NUL oder Ausgabe einer benötigten Größe. Länge null gelingt ohne Beobachtung oder Gastzugriff auch bei ungültigen Zeigern; `0xffffffff00000000` wählt null. Für Nichtnull ist der vollständige Puffer vor Zielprüfungen nötig. Ein völlig unbeschreibbares Ziel liefert EFAULT14; teilweise beschreibbare Bereiche stoppen vor dem Kopieren. Vorprüfungsfehler veröffentlichen keine Bytes. Backend-Schreibfehler werden durch den vorhandenen Kopierbesitzer weitergegeben, ohne allgemeine Rollbackgarantie. BSD carry und zweites Rückgaberegister einschließlich ursprünglichem x64 RDX bei Fehlern bleiben erhalten.

`setlogin(50)` bleibt auch mit explizitem root oder Nullpuffer ununterstützt. Der originale lesende Ablauf `login-buffer` prüft volle Längenträger, Präfixe, Nachbarbytes, Null-Längen-Zeiger und EFAULT; `virtual-login-buffer` gibt die 255 deklarierten Bytes aus. Fehlende Werte und Setter bleiben reine Modelltests. macOS ARM64 belegt weder physisches iOS noch Intel nativ. Das Beispiel deklariert 255 Nullbytes ausdrücklich und leitet keinen leeren Namen ab.

Der Gastprüfer initialisiert vor jedem Kopieren alle 265 Ausgabebytes und prüft drei aufsteigende, disjunkte Bereiche: [0,3), [3,3+n), [3+n,265), wobei n dieselbe begrenzte Kopierlänge ist. Der erste und letzte Bereich prüfen jedes Schutzbyte; der mittlere vergleicht jedes kopierte Byte mit dem ursprünglichen Snapshot. Alle 12 Längenträger, 21 Aufrufe mit Länge null, 42 EFAULT-Aufrufe, Carry-/Sekundärregisterprüfungen und rohen/virtuellen Pfade bleiben unter den bestehenden Ausführungslimits abgedeckt. Die Prüfarbeit sinkt bei gleicher Byteabdeckung und Reihenfolge des ersten Fehlers; die Produktionslaufzeit benötigt gesonderte Messungen.

```json
{"darwin_system":{"login_name_hex":"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}}
```

[XNU getlogin / MAXLOGNAME](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c#L1561).


## Explizite Priorität des aktuellen Prozesses

`DarwinSystemOptions::ProcessNice` / JSON `darwin_system.nice` deklariert einen unabhängigen festen vorzeichenbehafteten nice-Wert von -20 bis 20. Fehlend ist unbekannt; explizite0 und -1 sind bekannt. Der bestehende verlustfreie Ganzzahlparser akzeptiert Ganzzahlen und dezimale Ganzzahlzeichenketten; falsche Typen, Bruchteile, Exponentzeichenketten, Leerzeichen und Werte außerhalb der Grenzen scheitern vor dem Laden. Exakt ganzzahliges numerisches JSON bleibt gültig. Nicht-Darwin-Profile lehnen das Feld ab. Host, Identität, Gruppe/Sitzung, Taint, Login, Ressourcen oder CPU liefern keinen Wert; Planung, Rechte und Ausführungsbudgets ändern sich nicht.

Rohes `getpriority(100)` verwendet die unteren32 Bits des int-Selektors und des vorzeichenlosen `id_t`-Ziels. Ein Ziel über INT32_MAX liefert zuerst EINVAL22. Unbekannte Selektoren einschließlich GPU5/0x1000 und Thread3 mit nichtnull Ziel liefern vor Beobachtungen EINVAL. `PRIO_PROCESS`0 unterstützt nur0 oder die aktuelle PID1000 und fordert danach nice an. Andere positive PIDs bleiben ohne erfundenes ESRCH ununterstützt. Gruppe1, Benutzer2, Thread3/Ziel0 und4,6,7,8 bleiben auch mit verwandten Werten unbekannt. Ein Thread-Ziel nur mit hohen Bits ist kein EINVAL. Vorzeichenerweiterung auf64 Bits liefert für -1 erfolgreich UINT64_MAX mit gelöschtem carry. Kein Gastspeicher, ungenutzte Argumente ignoriert; BSD bewahrt x64 RDX bei Fehlern und löscht es bei Erfolg, ARM64 X1 ist immer0.

`setpriority(96)` bleibt auch mit explizitem root/nice ununterstützt. Das originale nur lesende `process-priority` prüft Eigenziele, sicher ungültige Argumente und Erfolg/Fehler/Erfolg; `virtual-process-priority` gibt die acht deklarierten vorzeichenbehafteten Bytes aus. Fremdprozess-, Aggregat-, Fehlwert- und Setterfälle bleiben Modellfälle. Die neue ARM64-macOS-Sonde bestand191 Prüfungen bei nice0; dieser nichtnegative Wert beweist keine negative Hardwareerweiterung. Dafür gelten die festgelegten signierten Eintrittsdeklarationen und unabhängigen Modellgrenzen. Physisches iOS und natives Intel bleiben separat.

```json
{"darwin_system":{"nice":-1}}
```

[XNU getpriority](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_resource.c), [XNU signed INT entry](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/dev/arm/systemcalls.c).


## Open ohne Linkverfolgung und Verzeichnisvorprüfung

Normale und nocancel-Einträge von `open` / `openat` akzeptieren O_NOFOLLOW=0x100 oder O_NOFOLLOW_ANY=0x20000000 in den vorzeichenlosen unteren32 Bits. Diese Suchflags fehlen in F_GETFL und ändern weder Zugriff, Anhängen, Kürzen, Erzeugen noch FD-eigenes CLOEXEC. Beide zusammen ergeben EINVAL22 nach der FD-Kapazitätsprüfung und vor dem vollständigen Pfadimport; eine volle Tabelle liefert zuerst EMFILE24. Unbekannte Flags bleiben nicht unterstützt.

Bei dirfd ungleich AT_FDCWD liest `openat` genau das erste Pfadbyte vor Zugriffsmodus und FD-Kapazität. Ein unlesbares Byte ergibt EFAULT14. Ein relativer Anfang, auch NUL, prüft zuerst das gehaltene Verzeichnisobjekt: unbekannter FD EBADF9, reguläre Datei ENOTDIR20, unbekannter Stream-vnode-Typ nicht unterstützt. `/` überspringt dirfd; erst danach liest die vorhandene open-Sequenz den vollständigen Pfad. Normales `open` und AT_FDCWD lassen diese Vorprüfung aus, andere nameiat-Pfaddienste prüfen nach Flags und Größen zuerst das erste Byte und den relativen dirfd, dann die vollständige Zeichenfolge. Abgelehnte Aufrufe verbrauchen weder FD noch neuen inode; Transportfehler werden ohne Namensraummutationen weitergereicht.

Das ursprüngliche `file-access` prüft NOFOLLOW_ANY über relative Verzeichnis-FDs und vermeidet native `/var`-/`/tmp`-Linkaliase. Direkte Tests unterscheiden erstes/späteres Byte, Schrägstrich/NUL, Benutzer-/Seitengrenzen, volle Tabellen und gehaltene entfernte Verzeichnisse. Die rohe ARM64-macOS-Sonde bestand30 Fälle im unveränderten Fünfsekundenlimit. Ihre letzte absolute Pfadzeile bei voller Tabelle verwendet trotz altem Namen einen gültigen Verzeichnis-FD. Physisches iOS und natives Intel bleiben getrennt zu prüfen.

[XNU open1at / open1](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU vn_open_auth](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_vnops.c), [XNU open flags / FMASK](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/sys/fcntl.h).

## Feste anfängliche symbolische Links

Die folgenden initialen Namen sind standardmäßig geschützt; der letzte Abschnitt beschreibt explizite Änderungsrechte. `DarwinFileOptions::SymbolicLinks` / `darwin_files.symbolic_links` deklarieren kanonischen absoluten `path`, rohe hexadezimale `target_hex` und optionale eigene `metadata`. Ziele enthalten1..1023 NichtNUL-Bytes, erhalten NichtUTF-8, wiederholte Schrägstriche/Punkte und dürfen fehlen. `files` bleibt auch leer erforderlich. Kollisionen und deklarierte Nachfahren unter Links sind verboten. Pfad/NUL/Ziel teilen256 Einträge/16 MiB. S_IFLNK, size=Ziellänge, DT_LNK=10 und konsistente inode sind erforderlich; CWD muss ein tatsächliches Verzeichnis sein.

Links werden vor Punkten aufgelöst. Relative Ziele beginnen im tatsächlichen Elternobjekt, absolute an der Gastwurzel. Jede Fortsetzung prüft abschließende Schrägstriche neu; verbrauchte Eingabestriche werden nicht übernommen.32 Erweiterungen sind erlaubt, die33. ergibtELOOP62; Ziel+Rest+NUL über1024 Bytes ergibtENAMETOOLONG63. FD/CWD/F_GETPATH/mmap halten das Zielobjekt.

stat64/open/access/truncate/chdir folgen dem letzten Link; lstat64/readlink behalten ihn. O_NOFOLLOW ergibtELOOP, mitO_DIRECTORY zuerstENOTDIR20. O_NOFOLLOW_ANY verweigert erforderliche Auflösung. O_CREAT|O_EXCL ergibtEEXIST17 bei vorhandenem letzten Link, auch fehlendem/zyklischem Ziel. AT0x20/0x800 behalten den letzten Link;0x800 verweigert Zwischen-/Endschrägstrich-Auflösung. Kombination erlaubt. AT_FDONLY ignoriert den Pfad nach Flagprüfung.

readlink(58) nutzt vorzeichenbehaftetes unteres32-Bit-count, readlinkat(473) vollständiges size_t; Rückgabeint. ÜberINT32_MAX kommtEINVAL22 vor Pfad/FD. Kopiert min(count,Ziellänge), ohneNUL; nur dieser Bereich wird geprüft. Länge0 prüft Pfad/Typ und ignoriert dann Ausgabe. Kein Link:EINVAL22; kein schreibbares Byte:EFAULT14; teilweise schreibbar: Stopp vor Kopie. Transport-/Speicherbudgetfehler bleiben Fehler.

Linknamen und rohe Zielbytes bleiben fest. MutableDirectories darf weder die Wurzel noch ein Pfadsegment-Vorfahre eines festen Linknamens sein; /work enthält /workspace/link nicht. Getrennte veränderbare Bereiche dürfen während der Ausführung erzeugte, verschobene, gelöschte oder ersetzte Ziele enthalten. Prüfungen für Eltern, Mounts, Aliase, Flags, SWAP-Unterstützung und Erzeugung gelten weiter; neue Inodes müssen alle Metadaten/Snapshot-Inodes einschließlich geschützter Links übersteigen. Feste WritableFiles/MutationPolicies können das reguläre Ziel ändern. Link-unlink/rename stoppen vor Effekten. Die Laufzeiterzeugung folgt unten; harte Links, ACL und veränderbare initiale Linkkataloge bleiben offen. ARM64-macOS-Probe:189 Beobachtungen/115 vollständige Puffer innerhalb ursprünglicher5s; kein Nachweis für physisches iOS/Intel HVF/vollständiges OS.

Die zusätzlichen60 ARM64 macOS DELETE/RENAME-Kontrollen erfassen vollständige stat-Puffer, den Namensraum vorher/nachher und gehaltene FD/CWD-Identitäten innerhalb der ursprünglichen5s. Abschließende Schrägstriche können einen festen Link auflösen und das Ziel ändern; NOFOLLOW_ANY verweigert nötige Auflösung mit ELOOP. symbolic-link-mutations ohne SDK prüft Erzeugung, fehlende Ziele, Verschieben/Löschen/Ersetzen, gehaltene CWD-Eltern und alle10 ursprünglichen Datei-Bytes vor FD-Schluss sowie alle10 Mapping-Bytes auch danach. Kein Nachweis für physisches iOS oder natives Intel.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"symbolic_links":[{"path":"/link","target_hex":"64617461"}],"working_directory":"/"}}
```

[XNU namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c), [XNU readlink / AT](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU open authorization](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_subr.c).

## Symbolische Links während der Ausführung erzeugen

Rohe `symlink(57)` und `symlinkat(474)` erzeugen prozesslokale Links in freigegebenen veränderbaren Verzeichnissen; Rückgabe ist int. Dirfd verwendet die unteren32 Bits, absolute Zielnamen ignorieren den FD. Der Linkinhalt wird vor dem Zielnamen bis zum ersten NUL importiert:0..1023 rohe Bytes, auch leer, Nicht-UTF8, Punkte und wiederholte Trenner.1024 Bytes ohne NUL ergeben ENAMETOOLONG63, ein früherer Lesefehler EFAULT14. Initiale JSON-Ziele verlangen weiterhin1..1023 Bytes.

Die aktuelle Linktabelle hält tatsächlichen Namen, Elternobjekt und Zielbytes. Bestehende Endnamen ergeben EEXIST17. Verbrauchte End-Slashes können einem hängenden Link folgen und an dessen Zielnamen erzeugen; der alte Link bleibt unverändert. Ein leeres Ziel expandiert mit ENOENT2. Readlink liefert dafür0 ohne Zugriff auf den Ausgabepointer, auch bei positiver Kapazität; Count/Pfad/Typ werden zuerst geprüft.

Name/NUL und Ziel werden einmal im gemeinsamen256-Einträge/16-MiB-Budget berechnet. Ablehnung verändert weder Knoten noch Eltern, FD oder regulären Erzeugungs-Inode. Neue vollständige Linkmetadaten bleiben unbekannt und erben weder Datei-CreationPolicy noch alte Beobachtungen wiederverwendeter Namen. Elternstat/Snapshot werden nach Erzeugung unbekannt. FD/CWD/Mapping behalten alte Objekte bei Zielentfernung/Ersetzung. Rmdir und Verzeichnisersetzung erkennen Linkkinder. Verschieben/SWAP mit geschützten initialen Links auf einer bewegten Seite stoppt vor Effekten. Link/Verzeichnis-Ersetzungen, harte Links, ACL und veränderbare initiale Linkkataloge bleiben offen; Aliase übertragen keine Freigabe des tatsächlichen Elternobjekts.

Die150 nativen ARM64-macOS-Fälle behalten vier Beobachterfehler; zehn zusätzliche Fälle prüfen tatsächliche neue Ziele und leere Links. Das SDK-freie `symbolic-link-creation` prüft beide Aufrufe, Bytes/Puffer, Eltern, Ersetzung und alle zehn alten FD/Mapping-Bytes. Physisches iOS, natives Intel und vollständige OS-Kompatibilität sind damit nicht belegt.

[XNU symlink / symlinkat](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU empty-link expansion](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c).

## Laufzeit-Symbolverknüpfungen entfernen

`unlink(10)` und `unlinkat(472)` entfernen zur Laufzeit erzeugte Links mit Änderungsfreigabe am tatsächlichen Elternobjekt. Initiale feste Links bleiben geschützt. Ein bloßes `AT_SYMLINK_NOFOLLOW_ANY` behält den letzten Link bei und erlaubt dessen Entfernung, auch bei fehlendem, zyklischem oder leerem Ziel. Nötige Zwischen- oder Endslash-Auflösung liefert ELOOP62. Ohne dieses Flag entfernt `a/` bei `a → b → target` nur `b`; `a` und das endgültige Ziel bleiben. Die bisherige Fehlerreihenfolge für Flags, Pfade und dirfd gilt weiter.

Erfolg erstattet einmal einen dynamischen Eintrag sowie aktuellen Pfad/NUL/Zielbytes und verwirft nur die vollständigen stat/Verzeichnisbeobachtungen des tatsächlichen Elternobjekts. Freier FD und Erzeugungs-Inode sind unnötig. Zieldaten, Änderungsrichtlinie, offene Beschreibungen, gemeinsame Cursor, CWD und Mappings behalten ihre Objekte. Namenswiederverwendung belebt den alten Link nicht; Ablehnung verändert weder Modellzustand noch Budget. Vollständige Metadaten zur Laufzeit erzeugter Links bleiben unbekannt. Link/Verzeichnis-Ersetzungen und Verschieben/SWAP Teilbäume mit geschützten initialen Links bleiben ununterstützt.

Die unabhängigen 40 ARM64-macOS-Beobachtungen erfassen unter derselben Fünfsekundenfrist 28 erfolgreiche Entfernungen, 12 Fehler, 17 erneute ENOENT und erhaltene FD/CWD/private Mappings. Sie belegen keine Gast-, physische iOS- oder native Intel-Abnahme. `symbolic-link-unlink` und öffentliche API-Fälle prüfen die Grenzen separat.

## Umbenennen zur Laufzeit erzeugter symbolischer Links

`rename(128)`, `renameat(465)` und `renameatx_np(488)` unterstützen gewöhnliche und `RENAME_EXCL=4` Blattverschiebungen: Link auf freien Namen sowie Link/Link, Link/Datei und Datei/Link-Ersetzung. Beide tatsächlichen Eltern benötigen Änderungsfreigaben in einer nachgewiesenen Mountdomäne; initiale Links bleiben unveränderlich. Der gemeinsame Resolver wählt die tatsächlichen Knoten. Leere, fehlende, zyklische und nicht-UTF8-Zielbytes bleiben erhalten; relative Ziele werden vom neuen Elternobjekt aufgelöst. Bloßes `RENAME_NOFOLLOW_ANY=16` behält den terminalen Link, notwendige Zwischenexpansion liefert ELOOP62. Verschiedene vorhandene EXCL-Ziele liefern EEXIST17; EXCL desselben Objekts bleibt ohne Groß-/Kleinschreibungsvertrag ununterstützt.

Vor der Veröffentlichung wird der neue Pfad/NUL reserviert: maximal1024 Bytes einschließlich NUL. Der vollständige aktuelle Pfad/NUL/Zielbetrag eines ersetzten Laufzeitlinks wird unabhängig von Referent-FD/Mapping genau einmal erstattet. Inhalte/dynamische Pfade ersetzter Dateien bleiben bis zur Freigabe aller Beschreibungen und Mapping-Leases berechnet; nur sofort rückgewinnbare reguläre Ziele liefern Reservierungskredit. Kein zusätzlicher Eintrag, FD oder Dateierzeugungs-Inode wird benötigt. Ablehnung erhält beide Knoten; Erfolg verwirft vollständige stat/Enumerationsbeobachtungen der tatsächlichen Eltern. Vollständige Laufzeitlink-Metadaten bleiben unbekannt. Link/Verzeichnis-Ersetzung, harte Links und Verschieben/SWAP Teilbäume mit geschützten initialen Links bleiben ununterstützt.

Die unabhängigen19 ARM64-macOS-Kontrollen erfassen14 Erfolge und5 EEXIST/ELOOP-Fehler mit unveränderten5-Sekunden-Fristen, Link-Inode/Zielbytes, relativer Neubindung und erhaltenen FD/dup/Cursor/CWD/privaten Mappings. `symbolic-link-rename` ohne SDK und öffentliche SDK/CLI-Prüfungen sind getrennt. Native Referenzen allein belegen weder physisches iOS noch natives Intel oder vollständige OS-Kompatibilität.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Austausch zur Laufzeit erzeugter symbolischer Links

`renameatx_np(488)` unterstützt Link/Link, Link/Datei und Datei/Link mit `RENAME_SWAP=2`, wenn beide tatsächlichen Eltern Änderung und SWAP in derselben nachgewiesenen Mountdomäne freigeben. Identische Namen ergeben einen wirkungslosen Erfolg ohne zusätzliche SWAP-Freigabe. Fehlende Ziele liefern ENOENT2; `SWAP|NOFOLLOW_ANY=18` behält terminale Links und verweigert notwendige Zwischenexpansion mit ELOOP62; `SWAP|EXCL=6` liefert EINVAL22 vor Pfadeingabe. Zielbytes bleiben unverändert; relative Ziele binden an beide neuen Eltern.

Beide Pfad/NUL-Beträge werden vor Veröffentlichung reserviert. Beide Objekte bleiben verknüpft, ohne Ersatzkredit aus Daten oder Mapping-Leases. Initiale Dateien erwerben beim ersten Austausch dynamische Pfadkosten und verwenden sie beim Rücktausch erneut. Eintrag, FD und Erzeugungs-Inode werden nicht verbraucht. Dateiidentität, nlink, Beschreibungen, Cursor, CWD und Mappings bleiben erhalten; vollständige Elternbeobachtungen werden unbekannt. Vollständige Laufzeitlink-Metadaten, tatsächliche Verzeichnis/Link-Paare und Teilbaumverschiebungen/SWAP mit geschützten initialen Links bleiben ununterstützt. Die 22 ARM64-macOS-Kontrollen erfassen 14 Austausche, zwei identische Objekte und sechs Fehler unter den ursprünglichen fünf Sekunden. `symbolic-link-rename` und SDK/CLI/Python prüfen Austausch und Fehler; physisches iOS, natives Intel und vollständiges OS sind nicht belegt.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Teilbäume mit Laufzeitlinks verschieben

Gewöhnliche/EXCL-Verzeichnisverschiebung und SWAP schließen nun Laufzeitlink-Nachfahren ein, auch beim Verzeichnis/Datei-Austausch. Der bestehende Vorgang prüft sämtliche Verzeichnis-, Datei- und Linknamen im gemeinsamen Namensraum und gegen1024 Bytes einschließlich NUL; alle drei Tabellen werden vor Veröffentlichung extrahiert. Zielbytes bleiben unverändert berechnet, nur aktuelle Pfad/NUL-Kosten wechseln. SWAP liefert keinen Ersatzkredit; zusätzlicher Eintrag, FD oder Erzeugungs-Inode entfällt.

Links behalten ihre tatsächlichen Elternobjekte beim Verschieben. Relative Ziele verwenden neue Pfade; Beschreibungen, gemeinsame Cursor, CWD, entfernte Knoten und Mapping-Leases behalten ihre Objekte. Geschützte initiale Links samt Teilbäumen bleiben unveränderlich; Verzeichnis/Link-Wurzelpaare, vollständige Linkmetadaten, harte Links und ACL bleiben offen.25 ARM64-macOS-Kontrollen erfassen fünf Verschiebungen, zehn Austausche, zwei wirkungslose Erfolge und acht Ablehnungen ohne Namensänderung unter den ursprünglichen fünf Sekunden. Elternwechsel prüfen rohe Ziele und relative Neubindung; `symbolic-link-rename` deckt C++/SDK/CLI/Python ab, ohne physisches iOS, natives Intel oder vollständiges OS zu belegen.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Metadaten neu erstellter Links und Verzeichnisse

Optionales `creation_policy.namespace_policy` (C++ `DarwinFileCreationPolicy::Namespace`) ergänzt die fünf Pflichtfelder um ein striktes Objekt mit `symbolic_link_allocation_unit`, `directory_entry_size` und `directory_blocks`. Die Linkeinheit ist eine Zweierpotenz von512 bis16 MiB, die Eintragsgröße positiv bis16 MiB, die Blockzahl uint64 bisINT64_MAX; Dezimalstrings bleiben verlustfrei. Elternmetadaten, explizite umask und neue Inodes bleiben erforderlich. Ohne Erweiterung gelten die früheren unbekannten Link-/Verzeichnismetadaten und der Inodeverbrauch nur für reguläre Dateien.

Erfolgreiche Datei-,symlink- und mkdir-Einfügungen teilen bei Aktivierung eine Inodefolge mit dauerhafter UINT64_MAX-Erschöpfung. Fehler und bestehende Namen verbrauchen nichts. Device/GID stammen vom tatsächlichen Elternobjekt, UID von der effektiven Gastidentität. Links verwenden S_IFLNK plus `0777 & ~umask`, nlink1, rohe Ziellänge einschließlich leer/nichtUTF-8 und auf die Einheit gerundete512-Byte-Blöcke. Verzeichnisse verwenden S_IFDIR plus `mode & 0777 & ~umask`, nlink gleich2 plus allen direkten verknüpften Namen, size gleich nlink mal Eintragsgröße und feste blocks, auch bei gehaltenen entfernten leeren Objekten. Dies ist ein expliziter virtueller Vertrag, keine abgeleitete APFS-Regel.

Anfangszeiten verwenden creation_time. Kindnamensänderungen setzen mtime/ctime neu erstellter Eltern; direkte Verschiebungen setzen nur Objekt-ctime auf mutation_time. Vorfahrenverschiebungen erhalten Nachfahrenmetadaten. Die vollständigen Daten bleiben bei dup/CWD/Ersetzung/SWAP/Entfernung/Namenswiederverwendung am Objekt. Die Erstellungserweiterung allein erhält weder vollständigen stat initialer Eltern noch feste Snapshots; die separaten Richtlinien unten bieten stat und aktuelle Enumeration. ACL, Änderungen initialer Links ohne individuelles Recht und allgemeine Verzeichnis-/Link-Wurzeltransaktionen fehlen weiterhin. `created-namespace-metadata` prüft native gemeinsame Identität/Modus/Eigentümer/Lebensdauer; `virtual-created-namespace-metadata` vergleicht vollständige144-Byte-Konstanten überC++/SDK/CLI/Python. Fokussiert bestanden11 Modell-/Zulassungstests,1 strikterJSON-Test,43 native Workloads mit unveränderten5Sekunden,8 Gastinstanzen（12 nicht verfügbare übersprungen,3 nötigeHVF ausgeführt）und10 öffentliche Fälle. Pointertabellenfehler und korrigierte statischeARM64-Artefakte bleiben erhalten. Physisches iOS und nativesIntel sind unvalidiert.

```json
{"namespace_policy":{"symbolic_link_allocation_unit":512,"directory_entry_size":32,"directory_blocks":7}}
```

## Virtuelle Auflistung nach Namensänderungen

Optionales `directories[].enumeration_policy` aktiviert aktuelle `getdirentries 64`-Ansichten; C++ nutzt `DarwinFileOptions::DirectoryEnumerationPolicies` und `DarwinDirectoryEnumerationPolicy`. Das strikte Objekt verlangt genau `minimum_buffer_size`, `initial_minimum_buffer_size` und `seek_offset`: der erste Wert ist positiv, beide Minima höchstens 128 MiB, seek_offset ein verlustfreies uint 64. Anfangsverzeichnisse brauchen eigene Metadaten mit Inode ungleich null; unveränderliches `contents` ist unvereinbar. Jede Referenz zählt Pfad+NUL einmal. mkdir erbt die Richtlinie des tatsächlichen Elternobjekts; vorhandene Nachkommen behalten ihre eigene Erklärung. Auflistung und Änderungsbefugnis sind unabhängig.

Die virtuelle Reihenfolge lautet `.`, `..`, dann direkt verknüpfte Namen nach vorzeichenlosen Bytes. Inodes stammen aus beobachteten oder erzeugten Objekten; `..` folgt dem gehaltenen tatsächlichen Elternobjekt. Fehlende/null Identitäten stoppen vor Ausgabe. Nach ungültigem vollständigem Anfangs-stat bleibt nur die Inode-Identität verwendbar. Cookies sind lokale Ordnungszahlen ab 1, d_seekoff die erklärte Konstante. dup teilt den Cursor, open bleibt unabhängig. Verbindliche Mitgliedsänderungen und direkte Verschiebungen erfordern Zurücksetzen auf null; Ablehnungen und wirkungslose Vorgänge behalten Cursor. Vorfahrenbewegungen erhalten Nachkommencursor. Versionserschöpfung stoppt ausdrücklich. Gehaltene entfernte leere Verzeichnisse liefern null Datensätze, auch nach Namenswiederverwendung.

Dieser Absatz beschreibt die Vorbereitung nur mit Enumeration, ohne die folgende initiale stat-Richtlinie. Kodierer, ganze Datensätze, Puffergrenzen, Nutzlastgrenze, EOF-Suffix und Daten/Cursor/Position/Flags-Reihenfolge bleiben gemeinsam. Vollständiger Anfangs-Eltern-stat und feste Snapshots werden weiterhin ungültig; ohne Richtlinie bleiben bisherige Ablehnungen. APFS-Generationen werden nicht nachgebildet. Unabhängige ARM 64-Vorbereitung erfasste 25 Ereignisse/16 Ansichten innerhalb unveränderter 5 Sekunden. `directory-enumeration-mutations` prüft native Identitäten und gehaltene Objekte; `virtual-directory-enumeration` vergleicht 160 wörtliche Bytes über Gast, C/CLI und Python. Natives Intel, physisches iOS, ACL, Hardlinks und vollständige OS/Frameworks bleiben ungeprüft oder nicht unterstützt.

## Explizite stat-Änderungen initialer Verzeichnisse

Optionales `directories[].mutation_policy` erhält den vollständigen stat eines zugelassenen initialen Verzeichnisses nach Namensraumänderungen. C++ verwendet `DarwinDirectoryMutationPolicy` und `DarwinFileOptions::DirectoryMutationPolicies`. Das strikte Objekt enthält genau `directory_entry_size` und `mutation_time`. Die Eintragsgröße ist positiv und höchstens 16 MiB; die Zeit verwendet verlustfreie vorzeichenbehaftete 64-Bit-Sekunden und Nanosekunden in [0,1000000000). Eigene vollständige metadata mit einer Inode ungleich null sind erforderlich. Jede Referenz berechnet Pfad plus NUL einmal; die projizierte Größe allokiert keine Dateibytes. Die Richtlinie erteilt keine Namensraum- oder Zugriffsrechte und benötigt keine Erstellungs- oder Enumerationsrichtlinie.

Der vollständige beobachtete Datensatz bleibt bis zur ersten tatsächlich bestätigten Änderung erhalten. Dann werden seine skalaren Felder ohne Allokation in das Verzeichnisobjekt kopiert. Kindnamensänderungen setzen nlink auf zwei plus alle unmittelbar verknüpften Namen jeder Art, size auf nlink mal directory_entry_size und mtime/ctime auf mutation_time. Direkte Verschiebung, SWAP oder Entfernung ändern nur ctime; eine Vorfahrenverschiebung erhält Nachfahrendaten. Ablehnungen und wirkungslose Operationen desselben Objekts ändern nichts. Device, inode, mode, Eigentümer, blocks, Blockgröße, flags, generation, atime und birthtime behalten beobachtete Werte. Dies sind deklarierte virtuelle Regeln, keine Ableitung von APFS-Allokation, Linkzahlen oder Uhren.

Datensatz und Richtlinie folgen dem ursprünglichen Objekt durch dup, gehaltene FD, CWD, Ersetzung, Entfernung und Namenswiederverwendung. Ein neues mkdir-Objekt verwendet die separate Erstellungsrichtlinie, falls angegeben, und erbt keine initiale stat-Richtlinie vom Elternobjekt oder früheren gleichnamigen Objekt. Unveränderliche Snapshots werden nach Änderungen weiterhin unbekannt; eine unabhängige enumeration_policy kann aktuelle Ansichten liefern. Ohne diese stat-Richtlinie bleiben vollständige Metadaten geänderter initialer Verzeichnisse unbekannt.

Eine unabhängige native ARM64-Vorbereitung erhielt 27 geschützte rohe stat-Ansichten und 15 Operationen innerhalb unveränderter fünf Sekunden und prüfte die 144-Byte-SDK-ABI sowie gehaltene Identität ohne Zeit- oder Allokationsverallgemeinerung. Der originale Test `initial-directory-metadata` prüft gemeinsame native Beobachtungen; `virtual-initial-directory-metadata` vergleicht einen vollständigen literalen 144-Byte-Datensatz über Guest, C/CLI und Python. Modelle prüfen auch erste Entfernung, fehlende Änderungsrechte, fehlende Erstellungsrichtlinie und Snapshot-/Enumerationsunabhängigkeit. Natives Intel, physisches iOS, ACL, Hardlinks, Änderungen initialer Links ohne individuelles Recht und vollständige OS/Frameworks sind unvalidiert oder nicht unterstützt.

```json
{"mutation_policy":{"directory_entry_size":17,"mutation_time":{"seconds":-11,"nanoseconds":321}}}
```

## Explizite Änderung initialer symbolischer Links

`symbolic_links[].mutable:true` und C++ `DarwinFileOptions::MutableSymbolicLinks` erlauben Namensraumänderungen des ursprünglichen Objekts; der tatsächliche Elternknoten braucht separat Änderungsrecht. Bekannte flags, spezielle mode-Bits, link_count≠1, Identitätsalias und widersprüchliche Geräte werden abgelehnt. Ohne Recht bleibt der Name geschützt und darf nicht in einem veränderlichen Vorfahrenbereich liegen. Das Recht reserviert eine feste path/NUL-Referenz, ohne Eintrag oder Erstellungs-inode. Initiale Ziele bleiben unverändert.

unlink, gewöhnliches/EXCL rename, Blatt-link/file/link SWAP und deklarierte Teilbaumtransaktionen behalten tatsächliche Eltern-, mount- und SWAP-Bedingungen. Relative Ziele werden vom neuen Elternobjekt aufgelöst; FD/dup, CWD und Mapping-Leases behalten ihre Referenten. Initiale Kosten bleiben nach Löschen/Ersetzen reserviert; die erste Umbenennung braucht einen separaten dynamischen Namen. Nur besessene dynamische Namens/Zielkosten werden zurückgegeben; nur neu erstellte Links zählen dynamisch. Ablehnungen und wirkungslose Operationen ändern nichts.

`symbolic_links[].mutation_policy` verwendet `DarwinSymbolicLinkMutationPolicy` und `DarwinFileOptions::SymbolicLinkMutationPolicies`. Das einzige Pflichtfeld `mutation_time` enthält verlustfreie signed 64-bit Sekunden und Nanosekunden [0, 1000000000); Recht und vollständige nonzero-inode-Beobachtungen sind nötig. Der erste direkte Move/SWAP kopiert Skalare ohne Allokation und ändert nur ctime. Blocks und alle übrigen Werte bleiben beobachtet; Vorfahrenbewegungen/Ablehnungen/No-ops erhalten den Record. Ohne Richtlinie ist vollständiger stat danach unbekannt, aber Enumerations-inode und bekannte Gerätewidersprüche bleiben erhalten. Die path/NUL-Referenz wird einmal fest reserviert. Wiederverwendete Namen/neue symlink erben keine initiale Richtlinie; Erstellung nutzt die separate namespace policy.

Die private ARM64-Vorbereitung bewahrt 14 guarded raw-stat Ansichten, 11 Operationen, unabhängiges 144-byte SDK ABI, compile120s/native5s/drain1s/reap1s und Cleanup. `mutable-initial-links` prüft native Identität, Neubindung und Referenten; `virtual-mutable-initial-links` prüft vollständigen stat in fünf guest-Profilen, C/CLI und Python. Modelle prüfen zwei Seitengrößen, Teilbaum-SWAP, genaue Kosten und entry/inode-Erschöpfung. Intel und physisches iOS sind unvalidiert; hard links, ACL, vollständiges OS/runtime/framework fehlen.

```json
{"mutable":true,"mutation_policy":{"mutation_time":{"seconds":-13,"nanoseconds":456}}}
```

## Verzeichnis- und Symlink-Wurzeltransaktionen

`renameatx_np(RENAME_SWAP)` tauscht ein tatsächliches Verzeichnis und einen Symlink in beiden Richtungen, einschließlich nichtleerer Teilbäume. Anfangsverzeichnisse benötigen `exchangeable`, Anfangslinks `mutable`; beide tatsächlichen Eltern benötigen Änderungs-/SWAP-Rechte innerhalb eines nachgewiesenen Mounts. Geschützte Anfangsnachkommen bleiben ausgeschlossen. Unter bestehenden Verschieberechten ergibt Verzeichnis→Link ENOTDIR20, umgekehrt EISDIR21 und EXCL gegen verschiedene vorhandene Namen EEXIST17. Beide Verzeichnis/Nachkommenlink-Zyklen ergeben vor Änderungen EINVAL22. Der Link selbst wird ausgetauscht; entstehende Selbstreferenz ist zulässig und späteres Folgen ergibt ELOOP62.

Die bestehende Drei-Tabellen-Transaktion prüft Wurzeln, verknüpfte/gehaltene Nachkommen, alle Pfade und dynamischen Speicher vor Veröffentlichung. Feste Anfangsnamen/Ziele/Referenzen, Dateidaten und Mapping-Leases liefern keinen SWAP-Kredit. Erste Umbenennung reserviert einen eigenen dynamischen Pfad/NUL; Wiederholung ersetzt nur dessen alte Kosten. Kein zusätzlicher Eintrag, FD oder Erstellungs-inode wird verbraucht. Zugehörigkeit folgt tatsächlichen Elternobjekten, nicht wiederverwendeter Schreibweise. Rohziele bleiben unverändert, relative Auflösung bindet neu; FD/dup-Cursor, CWD, Referenten und verwaiste Mappings bleiben erhalten. Direkte Wurzeln verwenden eigene stat-Regeln nur für ctime, Vorfahrenbewegung erhält Nachkommen. Ohne Regel bleibt vollständiger stat unbekannt, bekannte inodes dienen weiter der Live-Auflistung; relevante Versionen folgen dem bestehenden Rücksetzvertrag. Keine neuen JSON-Felder/Rechte.

Die originale private ARM64-Vorbereitung bewahrt 36 geschützte 144-byte-stat-Ansichten, 16 raw rename, compile120s/native5s/drain1s/reap1s und bestätigte Prozess-/Verzeichnisbereinigung. `directory-link-roots` und `virtual-directory-link-roots` prüfen Identität und vollständige stat-Konstanten über fünf guest-Profile, C/CLI und Python. Modelle beider Seitengrößen prüfen genaue Budgets, ungeöffnete Überläufe, verwaiste Objekte sowie FD/Eintrag/inode-Erschöpfung. Dieser Abschnitt erweitert frühere Ausschlüsse innerhalb der genannten Rechte. Natives Intel, physisches iOS, Hardlinks, ACLs und vollständiges OS/runtime/framework bleiben separate Lücken.

## Feste pathconf-Abfragen des Kernels

pathconf(191) / fpathconf(192) unterstützen feste XNU-vnode-Werte:15/16/17→1,19/25→0,20/22/23→4096,21→65536,24→255. Diese Angaben zu Links, Allokation, I/O und Transfers aktivieren keine asynchrone Ausführung oder Autorisierung; Seitengröße und Katalogbudgets bestimmen sie nicht.

Vollständige Pfadauflösung mit Link-Verfolgung oder FD-Suche erfolgt vor dem low32-Selektor. CWD und EFAULT/ENOENT/ENOTDIR/ELOOP bleiben erhalten; fehlende/geschlossene FDs liefern EBADF. Gehaltene Datei-/Verzeichnisobjekte bleiben ohne vollständigen stat über dup, Verschieben, Entfernen und Namenswiederverwendung abfragbar. Der native Typ von Eingabe/Ausgabe-Erfassung bleibt unbekannt. BSD-int/Carry/Zweitregister gelten ohne Ausgabekopie oder Änderungen an Cursor, Metadaten/Auflistung und Eintrag/FD/inode. NAME_MAX, Groß-/Kleinschreibung und unbekannte Selektoren bleiben nach der Suche ununterstützt; keine Hostwerte oder geratenen EINVAL.

kernel-pathconf, kernel-pathconf-values und kernel-pathconf-unsupported prüfen gemeinsame Semantik, unabhängige80-Byte-Werte und den Stopp mit erhaltener Ausgabe über Gast/C/CLI/Python. Private ARM64-Vorbereitung:250 Abfragen,249 SDK-Vergleiche,0.262s bei unveränderter5s-Grenze. Natives Intel/physisches iOS/ACL/Hardlinks/vollständige Laufzeiten und Frameworks bleiben ungeprüft oder unimplementiert.

[XNU vn_pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU fpathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_descrip.c).

`pathconf`: `file/` → ENOTDIR; `alias/`, `alias//` → 1 (selector15); `alias/.`, `alias/child` → ENOTDIR.

## Listen fester gemeinsamer Attribute

getattrlist(220), fgetattrlist(228) und getattrlistat(476) fragen elf feste gemeinsame Felder im expliziten Katalog ab: Gerät, Typ, vier Zeiten, Besitzer/Gruppe, vollständiger Modus, Flags und Datei-ID. Die Gültigkeit vollständiger Metadaten wird mit stat64 geteilt; fehlende oder invalidierte Beobachtungen bleiben unbekannt. Typ und leere Auswahl brauchen keinen stat. Wurzel/Mount-NAME, Volume-, Verzeichnis/Datei/Fork-Masken, ACL und unbekannte Optionen bleiben auch mit Rückgabemaske ausdrücklich ununterstützt; keine Hostdaten oder Mountnamen werden abgeleitet.

Pfad/at importieren24 Bytes vor der Suche; FD prüft zuerst low32 FD und nativen Typ. reserved wird ignoriert. Gemeinsame CWD/relative-FD/Link-Auflösung bewahrt native Fehler vor Größe/Bitmap. Little-endian,4-Byte-Ausrichtung, vollständiger st_mode und vorzeichenbehaftete Sekunden; mit Rückgabemaske120 Bytes, sonst100. Kleine Puffer erhalten nur das gewünschte Präfix, melden aber den vollständigen Bedarf. Partieller Zugriff stoppt vor dieser Kopie; signed-uio-Übergröße ergibt EINVAL nach gültiger unterstützter Anfrage. Cursor, Metadaten, Auflistung und Eintrag/FD/inode-Budgets bleiben gleich; dup/Entfernen/Namenswiederverwendung behalten ihre Lebensdauer.

Drei Modi prüfen gemeinsame Semantik, unabhängige konfigurierte Bytes und ununterstütztes ATTR_CMN_EXTENDED_SECURITY mit erhaltener Ausgabe über guest/C/CLI/Python. Private ARM64-Vorbereitung:187 raw-Abfragen/176 SDK-Pfad-FD-Vergleiche; native5s unverändert. SDK15.5 deklariert getattrlistat nicht, raw476 wird getrennt erfasst. Neue guest/Python:5,000,000us/quantum1024; vorhandene öffentliche Tests:10s. Natives Intel, physisches iOS, FS-Fakten, Hardlinks, Rechte/ACL und vollständige Laufzeiten/Frameworks bleiben ungeprüft oder unvollständig.

```text
ATTR_CMN_RETURNED_ATTRS=0x80000000
FSOPT_NOFOLLOW=1, FSOPT_REPORT_FULLSIZE=4
FSOPT_PACK_INVAL_ATTRS=8 (requires ATTR_CMN_RETURNED_ATTRS)
FSOPT_NOFOLLOW_ANY=0x800
common-attributes
common-attributes-values
common-attributes-unsupported
```

[XNU attrlist](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_attrlist.c), [XNU attr.h](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h).


## Explizite Leseabfragen für erweiterte Attribute

getxattr(234), fgetxattr(235), listxattr(240) und flistxattr(241) lesen vollständige geordnete Beobachtungen in darwin_files. Dateien, Verzeichnisse und Links können extended_attributes als strenges Array von {name,bytes_hex} angeben. Fehlende Angaben bleiben unbekannt; [] bedeutet eine bekannte leere Liste. UTF-8-Namen haben 1..127 Bytes und dürfen Schrägstriche enthalten. Werte sind opaque Bytes; Duplikate werden abgewiesen und die Reihenfolge bleibt erhalten. Höchstens 4096 Attribute; Namen, NUL und Werte belasten das bestehende 16-MiB-Budget ohne einen vorhandenen Objektpfad doppelt zu zählen.

Beobachtungen gehören auch nach dup, Verschieben, Entfernen, CWD und Namenswiederverwendung zum Objekt, unabhängig von vollständigem stat und Auflistung. Neue Objekte bleiben unbekannt. Inhaltsänderungen, erfolgreiche Kürzung und unklare nichtleere Kopierfehler machen Attribute unbekannt; eine Ablehnung vor der Kopie erhält sie. Abfragen erhalten Cursor, Eingaben, Metadaten und Objekt/FD/inode-Budgets.

ABI: FD/options/position low32, size full64, BSD user_ssize_t/carry/secondary. NULL ignoriert position. Für nichtleere Werte liefert Pfad nonNULL size0 ERANGE; FD size0 fragt die Länge ab. Nur Pfad-get UINT32_MAX/UINT64_MAX sind ältere Abfragen; FD begrenzt auf INT32_MAX. Kurze positive Listen können vollständige Namen vor ERANGE ausgeben; negative full64 nonNULL-Längen ergeben bei nichtleeren Listen ERANGE. Eine native leere Liste wurde nicht verifiziert: dieser negative Fall bleibt bei deklarierter Leere UnsupportedService. Unzugängliche Ausgaben stoppen vor dem Kopieren. NOFOLLOW1 und NOFOLLOW_ANY64 sind unabhängig;8/16 werden vor Suche, FD1/64 vor FD/Name abgewiesen. CREATE2/REPLACE4 werden ignoriert; SHOWCOMPRESSION32 und unbekannte Bits bleiben ununterstützt. com.apple.system.*, ResourceFork, FinderInfo, decmpfs, Setzen/Löschen, Berechtigungen/ACL und Dateisystemannahmen sind ausgeschlossen.

extended-attributes / extended-attributes-values / extended-attributes-unsupported prüfen den gemeinsamen nativen Ablauf, virtuelle Bytes und unbekannte Beobachtungen mit erhaltener Ausgabe. Unverändert: guest/Python5,000,000us/quantum1024, öffentliche API10s, nativ5s. Private ARM64-Vorbereitung:320 raw/SDK-Vergleiche mit allen288 Schutzbytes und gleichem carry/secondary. Automatisches com.apple.provenance ist eine Beobachtung, kein leerer Standard. Natives Intel/physisches iOS bleiben unverifiziert; vollständiges dyld, Mach IPC, Objective-C/Swift und Frameworks bleiben unvollständig.

Sources: [XNU syscall ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU xattr calls](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ordinary attributes](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_xattr.c), [XNU xattr definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Original code/probes; Apple implementation is not copied.

## Begrenzte Objektnamen

ATTR_CMN_NAME=1 wird über getattrlist220/fgetattrlist228/getattrlistat476 für eindeutig benannte Objekte außerhalb der Wurzel im expliziten Katalog unterstützt. Der Blattname ist gültiges UTF-8 mit1..255 Bytes. Der mit F_GETPATH geteilte tatsächliche Objektpfad behält die letzte verknüpfte Schreibweise über dup, CWD, Verschieben, SWAP, Löschen und Namenswiederverwendung; Aufrufer-Aliase ersetzen sie nicht. Name und Typ brauchen keinen stat; gewählte stat-Felder brauchen vollständige gültige Beobachtungen. Wurzel/Mountnamen, ungültige Namen, Hardlink-/Großkleinschreibungs-Aliase, Normalisierung und Vollpfadattribute bleiben unbekannt.

attrreference_t umfasst8 Bytes vor anderen gemeinsamen Feldern; attr_dataoffset bezieht sich auf die Referenz, attr_length enthält NUL und der Namensbereich wird auf4 Bytes aufgefüllt. Kurze Ausgaben behalten Gesamtlänge und genaue Präfixe einschließlich geteiltem UTF-8. attribute-names / attribute-names-values / attribute-names-unsupported prüfen natives Verhalten, unabhängige Bytes und Wurzelnamen-Abbruch mit erhaltener Ausgabe über guest/C/CLI/Python. ARM64:601 raw-Abfragen,453 vollständige SDK-Schutzpuffervergleiche,384 Präfixprüfungen. SDK15.5 deklariert raw476 nicht. Native5s, guest/Python5,000,000us/quantum1024 und public10s unverändert. Natives Intel, physisches iOS und vollständige Laufzeiten/Frameworks bleiben ungeprüft oder unvollständig.

## Begrenzte Verzeichnisattribute in Gruppen

getattrlistbulk(461) benötigt explizites enumeration_policy.bulk_attributes=true; fehlend oder false gewährt nichts. Dieser virtuelle TYPE-Vertrag liefert direkte aktuelle Kindernamen in vorzeichenloser Bytefolge, ohne Punkteinträge und mit lokalen Ordnungszahlen statt nativen Cookies. Die Berechtigung bleibt am anfänglichen Verzeichnisobjekt über dup, Verschieben, SWAP, Entfernen und Namenswiederverwendung erhalten; neue Verzeichnisse erben sie nicht. minimum_buffer_size, initial_minimum_buffer_size und seek_offset gelten nur für getdirentries64.

NAME|OBJTYPE|RETURNED_ATTRS (0x80000009) ist erforderlich; die elf vorhandenen gemeinsamen Felder benötigen gültige ausgewählte Beobachtungen. Options0/8 sind erlaubt. bulk ignoriert beide16-Bit-Wörter bitmap/reserved unabhängig von normaler attrlist-Prüfung. Ein Attributencoder teilt attrreference_t und stat64-Gültigkeit. Nur vollständige Datensätze werden geliefert:8-Byte-Auffüllung, wenn sie passt, sonst darf die letzte Gruppe4-Byte-Größe haben. Eine zu kleine erste Gruppe ergibt ERANGE ohne Ausgabe- oder Cursoränderung; teilweise zugängliche erforderliche Ausgabe stoppt vor dem Kopieren. Nur tatsächlich gelieferte Bytes müssen schreibbar sein.

dup teilt den Fortschritt, separate open sind unabhängig. Ein abgeschlossener Lauf mit nichtnull Offset behält EOF trotz Namensraumänderung und überspringt nach Anfrageprüfung Größe/Ausgabe. Ein anfänglich leeres offset0 wird erneut geprüft. Null-lseek setzt die Iteration zurück. Änderungen vor EOF, beliebiges seek ungleich null und gemischte getdirentries64/bulk-Iteration stoppen ausdrücklich. NAME-only-Fallback, ERROR-Einträge, Snapshots, ACL/Berechtigungen, Hostreihenfolge und andere mask/options bleiben ununterstützt. bulk-attributes / bulk-attributes-values / bulk-attributes-unsupported prüfen gemeinsames natives Verhalten, virtuelle Literalbytes und unbekannte Auswahl mit erhaltener voriger Ausgabe. Private ARM64-Vorbereitung bestand728 geschützte raw/SDK-Vergleiche. native5s, guest/Python5,000,000us/quantum1024 und public10s bleiben gleich. Intel nativ, physisches iOS und vollständige Laufzeiten/Frameworks bleiben ungeprüft oder unvollständig.

Sources: [XNU bulk ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/man/man2/getattrlistbulk.2), [XNU attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h). Original implementation and probes; no Apple implementation copied.

## Explizit erlaubte Änderungen gewöhnlicher erweiterter Attribute

setxattr(236), fsetxattr(237), removexattr(238) und fremovexattr(239) benötigen eine unabhängige Freigabe für das anfängliche Objekt: C++ `MutableExtendedAttributes`, strikt boolesches JSON `mutable_extended_attributes=true`. Jede freigegebene Datei, jedes Verzeichnis und jeder Link muss eine vollständige Liste gewöhnlicher `extended_attributes` angeben, auch eine bekannt leere Liste. Schreibbarer Inhalt und ein veränderbarer Namensraum erteilen diese Freigabe nicht. Freigaben und Werte gehören dem gehaltenen Objekt auch bei dup, Verschieben, Entfernen und gehaltenen Speicherabbildungen. Neu erstellte Objekte und wiederverwendete Namen beginnen mit unbekannten Attributen. Bekannte Aliase, widersprüchliche Metadatenflags, geschützte Systemattribute, ResourceFork, FinderInfo und Kompressionssemantik bleiben ausgeschlossen.

Ersetzen erhält die Position in der virtuellen Liste, Entfernen löscht den Eintrag und Erstellen hängt ihn an. Diese prozesslokale Reihenfolge wird angegeben, nicht aus APFS abgeleitet. Anfängliche Attributbytes, Anzahlen und Freigabe-Pfadreferenzen bleiben reserviert. Laufzeitwachstum teilt die bestehenden Grenzen von 16 MiB/4096; Entfernen, Inhaltsinvalidierung und endgültige Objektfreigabe geben nur dieses Wachstum frei. Kapazitäts-, Übertragungs- und Fristfehler veröffentlichen keinen Zwischenzustand. Vollständig unlesbare erforderliche Eingabe liefert EFAULT; teilweise lesbare Eingabe stoppt vor Veröffentlichung ausdrücklich als nicht unterstützt. Erfolg invalidiert den vollständigen stat ohne erfundene Zeiten und erhält Identität, Verzeichnisinhalt, Aufzählungsversion/-snapshot und Cursor.

Die Änderungs-ABI nutzt low32 FD/options/position und full64 size. Frühe Prüfungen privilegierter und FD-Link-Optionen kommen vor dem Namensimport, dieser vor der Objektsuche. set lehnt NULL bei einer Länge ungleich null vor zu großer VFS-Eingabe (E2BIG7) ab; die Suche kommt vor der Prüfung gewöhnlicher Namen, position und Konflikte. set importiert den vollständigen Wert vor EEXIST17 für bestehendes CREATE oder ENOATTR93 für fehlendes REPLACE. CREATE und REPLACE zusammen liefern EINVAL; Entfernen ignoriert beide Bits. Größe null liest den Wertzeiger nicht. Andere Flags, unbekannte Berechtigungen und unbeobachtetes Anbieter-Verhalten stoppen ausdrücklich.

xattr-mutations / xattr-mutations-values / xattr-mutations-unsupported prüfen eigene native/Gast-Kontrollen, unabhängige virtuelle Byte-Literale und den Stopp bei fehlender Freigabe mit erhaltener bisheriger Ausgabe. Die private ARM64-Vorbereitung prüfte726 raw/SDK-Aufrufe, vollständige geschützte544-Byte-Beobachtungen und vollständige lesbare Seiten. native5s/compile120s/drain1s/reap1s, guest/Python5,000,000us/quantum1024 und public10s bleiben unverändert. Natives Intel, physisches iOS, dyld, Mach IPC, Threads/Signale, Objective-C/Swift-Laufzeiten und vollständige Frameworks bleiben ungeprüft oder unvollständig.

Primäre ABI-Quellen: [XNU-Systemaufrufdeklarationen](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [xattr-Definitionen](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Eigener Code und eigene Probes; keine Apple-Implementierung kopiert.

## Begrenzte Darwin-Hardlinks

link folgt dem letzten symbolischen Ziel; linkat mit flags=0 wählt das Linkobjekt, AT_SYMLINK_FOLLOW das Ziel. Nur low32 0/0x40 sind zulässig, andere niedrige Bits liefern EINVAL vor dem Import. Quellensuche und Verzeichnis-EPERM gehen dem Zielimport voraus; vorhandene Ziele liefern EEXIST. Das Ziel benötigt Änderungsrecht und dieselbe ausdrücklich festgelegte Mount-Domäne. Anfängliche Identitätsaliasse sowie bekannte Geräte-, Modus- und Flag-Konflikte bleiben unzulässig.

Ein Alias kostet nur Eintrag und Pfad/NUL, keinen neuen inode. Bytes, Attributrechte, Metadatengültigkeit und Mapping-Leases gehören dem gemeinsamen Objekt. Explizite Richtlinien aktualisieren Linkzahl und ctime; ohne sie bleibt vollständiges stat unbekannt. Attributänderungen invalidieren stat, Inhaltsänderungen Attributbeobachtungen. Gehaltene Beschreibungen und letzte Mappings behalten Namenskosten; Ersetzung verrechnet nur sofort freigebbare Kosten. Teilbäume wählen genaue Identitäten und Eltern; externe Aliasnamen bleiben stehen, relative symbolische Ziele verwenden den gewählten Eintragselternteil.

F_GETPATH/ATTR_CMN_NAME bleiben nach mehreren Namen auch bei einem oder keinem Namen unzulässig; kein allgemeines APFS-Cachemodell wird behauptet. bulk NAME stammt vom tatsächlichen Eintrag; gewöhnliches rename/SWAP desselben Objekts behält beide Namen. EXCL-Großschreibung, Intel HVF, physisches iOS, ACL, kohärente Mappings/EOF-Signale, dyld, Mach IPC, Threads und vollständige Frameworks bleiben Lücken. Frühere Ausschlüsse werden nur innerhalb dieses Vertrags erweitert.

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## Begrenzte O_SYMLINK-Deskriptoren

O_SYMLINK=0x00200000 hält das letzte symbolische Linkobjekt auch bei ungültigem Ziel oder Zyklus, für Lesen, Schreiben oder beides. Es gewährt keine Schreibrechte auf Zielinhalte. O_CREAT folgt weiterhin dem Ziel; NOFOLLOW behält ELOOP-Priorität, exklusive Erstellung EEXIST, und O_DIRECTORY liefert für den gehaltenen Link ENOTDIR. Zwischenkomponenten und abschließende Schrägstriche folgen dem bestehenden Resolver und NOFOLLOW_ANY. F_GETFL enthält das Auswahlbit nicht.

Die Beschreibung hält den tatsächlichen LinkNode und die ausgewählte NameIdentity. Dup teilt Statusflags und Cursor, unabhängige Opens besitzen eigene Beschreibungen. Umbenennung, Entfernung, Namenswiederverwendung und Elternentfernung ersetzen das gehaltene Objekt nicht. F_GETPATH/ATTR_CMN_NAME verwenden den ausgewählten eindeutigen Namen; eine Mehrnamenshistorie verweigert vnode-Namensschlüsse dauerhaft, auch ohne verbleibende Namen. Anfangsreservierungen bleiben fest. Dynamische Namen, Ziele, Einträge und Attributzuwächse warten auf ihren tatsächlichen letzten Besitzer. Ein noch gehaltenes letztes Alias kann beim Ersetzen keine Kostengutschrift liefern.

I/O stellt Zielzeichenfolgen niemals als Dateiinhalt bereit. Nach bestehenden Skalar/Vektor-, Zugriffs- und Anzahlprüfungen liefern negative Offsets EINVAL. Lesen an INT64_MAX ergibt null, Schreiben EFBIG; andere zugelassene Offsets ergeben EPERM, auch bei Länge null und vor APPEND oder Datenzugriff. Bestehende frühe Negativprüfungen von pwrite/pwritev bleiben maßgeblich. DATA/HOLE liefert für nichtnegative Positionen ENXIO und für negative EINVAL, ohne Cursoränderung.

Nichtnegatives ftruncate für schreibfähige Beschreibungen und zugelassenes open TRUNC setzen nur WasWritten. Zielbytes, vollständiger stat, xattrs, Cursor, Speicherbudget und inode bleiben unverändert. Nur-Lesen oder negative Länge liefert EINVAL. Zugelassenes F_SETFL ändert APPEND|NONBLOCK und liefert anschließend ENOTTY25; dup sieht die Änderung, unabhängiges open nicht. Unbekannte Argumente stoppen vor Wirkungen.

Festes fpathconf, fgetattrlist und unabhängig deklarierte gewöhnliche FD-xattr-Rechte gelten für das Linkobjekt. Relative Verzeichnis-FDs und fchdir liefern ENOTDIR. truncate stellt durch Attributänderung ungültigen stat nicht wieder her. Ausgerichtete, nicht ausführbare historische private/shared mmap-Auswahlen erreichen das symbolische EINVAL ohne Mapping oder Lease. Gewöhnliches Shared, unbekannte Flags, ausführbarer Schutz und weitere bestehende Grenzen bleiben bestehen. Die18 nativen mmap-Kontrollen umfassen nur length16384, offset0 und protection1/2/3.

Die eigenständig erstellte ARM64-Vorbereitung prüft alle144 geschützten stat-Bytes bei15 Truncate-Fällen, extreme I/O-Offsets/Anzahlen, Sparse-Seek und fehlgeschlagene F_SETFL-Effekte. Das SDK-freie gemeinsame Programm läuft mit O0/O1/O2 und vergleicht den tatsächlichen Eltern-FD-Pfad. Virtuelle Routen vergleichen unabhängige stat/type-Literale oder verweigern Mehrnamensabfragen unter Erhalt der Ausgabe. Natives Intel HVF, physisches iOS, ACL/Rechte, gemappte EOF/Signale, dyld, Mach IPC, Threads und vollständige Laufzeiten/Frameworks bleiben ungeprüft oder unvollständig.

Primäre Interpretation: [passende XNU-Mappinggrenze](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_mman.c). Implementierung und Probes sind original; kein Apple-Implementierungscode wurde kopiert.

```text
O_SYMLINK=0x00200000; ENOTTY=25
open O_RDONLY|O_SYMLINK|O_TRUNC: WasWritten only
LinkNode, NameIdentity, HadMultipleNames, DetachedNames
INT64_MAX read=0 / write=EFBIG; other admitted offsets=EPERM
SEEK_DATA/SEEK_HOLE nonnegative=ENXIO / negative=EINVAL
F_SETFL: APPEND|NONBLOCK effect before ENOTTY; dup shares / independent open separate
symbolic-descriptors
symbolic-descriptors-values
symbolic-descriptors-name-unsupported
SymbolicDescriptor*, SymbolicDescriptorsRetainObjectsAndNativeErrorOrder
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 transport parameters / 15 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## Begrenzter nichtblockierender Deskriptorstatus

O_NONBLOCK=4 ist für reguläre Dateien, Verzeichnisse und O_SYMLINK zugelassen und bleibt in F_GETFL erhalten. Dup teilt Status und Cursor; unabhängiges open behält eigene Beschreibungen. Bestehende Regeln für Zugriff, close-on-exec, WasWritten, Metadaten, Bytes und Cursor gelten weiter. Endliches deklariertes stdin erhält EOF und Zeigerfehler-Reihenfolge; fehlende Eingabe bleibt unbekannt. Ausgabeerfassung erhält Kopierfehler und gemeinsames Budget.

F_SETFL prüft zugelassene low32-Argumente vor Wirkungen, addiert eins gemäß nativer open-Flag-Konvertierung und ändert nur APPEND|NONBLOCK. High32 wird ignoriert; Zugriff und eingehendes WasWritten erteilen keine Rechte und erfinden keine Schreibwirkung. Native Literalwerte3/7/11/15 wählen4/8/12/0. Symbolische Beschreibungen ändern den Status vor ENOTTY25. Unbekannte Flags wie ASYNC0x40 stoppen vor Wirkungen.

Die eigenständige ARM64-macOS-Vorbereitung enthält122 Beobachtungen:16 Anfragen je gültiger Objekt/Zugriff-Kombination, gehaltenes dup, unabhängiges open, Löschen der Bits, echte Schreibvorgänge und CLOEXEC je FD. Das SDK-freie Programm vergleicht mit O0/O1/O2 Bytes, Status, Cursor und rohe BSD-carry/errno-ABI. Gast, C/CLI und Python prüfen auch unbekannte Flags; alle drei ARM64-HVF-Profile müssen tatsächlich laufen. Keine Bereitschaftswartezeiten, Pipes, Netzwerk, kqueue, asynchronen Signale oder Host-I/O. O_EVTONLY-Prozessregeln bleiben unmodelliert; natives Intel HVF, physisches iOS und die vollständige Umgebung bleiben ungeprüft oder unvollständig.

```text
O_NONBLOCK=4; F_SETFL raw low32 mask=0x1000f
requests3/7/11/15 -> APPEND|NONBLOCK status4/8/12/0
nonblocking-descriptors / nonblocking-flags-unsupported
Nonblocking*, NonblockingDescriptorsKeepNativeControlState
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
8 model cases / 20 transport parameters / 10 public cases / 5 Python profiles
67 mandatory workloads per platform / ARM64 201 / Intel 134
```

## Explizite endliche getentropy-Beobachtungen

Raw BSD getentropy500 verwendet die geordnete Beobachtungsfolge `DarwinSystemOptions::EntropyReads`, in JSON `darwin_system.entropy_reads`. Nichtleere Hexzeichenfolgen haben gerade Länge: höchstens256 Einträge mit je1..256 Bytes. Dies sind Modellgrenzen; das JSON-Transportlimit bleibt65536 Bytes. Auslassen bedeutet unbekannt, `[]` ausdrücklich erschöpft. Native und JSON-Eingaben werden vor Änderungen an Image oder Backend geprüft; andere OS-Profile lehnen Darwin-Optionen ab.

Zuerst wird die gesamte64-Bit-Länge geprüft: über256 ergibt EINVAL22 ohne Zugriff oder Verbrauch; null gelingt für jeden Zeiger ohne Eingabe. Nichtnull verlangt anschließend den nächsten Eintrag mit genau passender Länge. Fehlende, erschöpfte oder unpassende Daten stoppen UnsupportedService vor Effekten, auch bei ungültiger Adresse; dies ist die Replay-Zulassung. Erfolg oder vollständig unbeschreibbares EFAULT14 verbraucht genau einen Eintrag. Teilweise beschreibbare Ziele werden vor Kopie und Cursoränderung abgelehnt; Transportfehler schreiten nicht fort. Spätere Rückgaberegisterfehler lassen abgeschlossene Effekte bestehen. Auch dieselben Optionen beginnen bei jedem Lauf am ersten Eintrag.

DarwinEntropy besitzt je Lauf einen eigenen Cursor; Eingabebytes bleiben unverändert. Bestehendes BSD und returnService verwalten beide ISA, Carry und sekundäre Register. Der SDK-freie Replay-Test deckt5 Gastprofile und3 ARM64 HVF-Profile ab; feste Bytes gehören nicht zum nativen deterministischen RNG-Inventar. ARM64 O0/O1/O2-Proben umfassen594 Aufrufe; geänderte Sentinelbytes belegen keine genaue Kopierlänge. Host-RNG, Kryptografiequalität, /dev/random, libc-Imports und Frameworks fehlen. Intel HVF, physisches iOS und vollständige OS-Kompatibilität bleiben ungeprüft oder unvollständig.

```text
BSD getentropy500 / DarwinEntropy / EntropyReads / darwin_system.entropy_reads
full64 size>256 -> EINVAL22; zero ->0; whole EFAULT14 consumes one record
1..256 bytes per record / at most256 records / JSON transport65536 bytes
entropy-replay / entropy-missing / entropy-exhausted / entropy-mismatch / entropy-partial
15 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
68 mandatory workloads per platform / ARM64 204 / Intel 136
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU getentropy ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU generation/copyout boundary](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/dev/random/randomdev.c).

## Explizite Identität des aktuellen Threads

BSD thread_selfid372 liest die unveränderliche optionale Beobachtung `DarwinSystemOptions::ThreadID` / JSON `darwin_system.thread_id`. Jedes uint64-Bitmuster einschließlich null ist bekannt; fehlende Angaben stoppen mit UnsupportedService. Dezimalzeichenketten erhalten64 Bits, JSON-Zahlen müssen exakte Ganzzahlen bis2^53-1 sein. IDs werden nicht aus Host, PID oder Mach-Port abgeleitet. Der Aufruf ohne Argumente ignoriert alle sechs Argumentträger und greift nicht auf Speicher zu. Die vorhandene low32-Auflösung erhält die vollständige rohe Nummer im Ereignis; BSD liefert64 Bits und löscht carry sowie RDX/X1. Mach-Formen bleiben unsupported.

Wiederholte Ausführungen behalten die Beobachtung, getrennte Optionen bleiben unabhängig. Es werden weder IDs vergeben noch Eindeutigkeit, Scheduler-Ereignisidentitäten, Thread-Lebenszyklen, pthread, TLS oder Mach IPC modelliert. Originale ARM64-Proben bei O0/O1/O2 bewahren24 Aufrufe zum Vergleich mit der aktuellen SDK-pthread-ID, beliebigen Argumenten und hohen Nummernbits. Das gemeinsame native Programm vergleicht nur Beziehungen innerhalb eines Prozesses; feste ID-Bytes gehören nicht zum deterministischen nativen Inventar. Intel HVF, physisches iOS und vollständige OS-Kompatibilität bleiben ungeprüft oder unvollständig.

```text
BSD thread_selfid372 / Wide / ThreadID / darwin_system.thread_id
known uint64 including0 / missing -> UnsupportedService / no memory
full64 return / low32 resolution / carry clear / RDX-X1 zero / raw event number
thread-identity / thread-identity-value / thread-identity-missing
4 model cases / 20 transport parameters / 16 public cases / 5 Python profiles
69 mandatory workloads per platform / ARM64 207 / Intel 138 unverified
original ARM64 O0/O1/O2 probes24 / native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU thread_selfid ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [libpthread current-thread owner](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/kern/kern_support.c).

## Explizite Mach-Selbstportbeobachtungen

Die rohen Mach-Aufrufe thread_self_trap27, task_self_trap28 und host_self_trap29 lesen unabhängige optionale uint32-Beobachtungen DarwinSystemOptions::ThreadSelfPort, TaskSelfPort und HostSelfPort über darwin_system.thread_self_port, task_self_port und host_self_port. Jede Abfrage benötigt nur ihr Feld. Fehlen bleibt unbekannt und stoppt UnsupportedService; Null, gleiche Namen und alle32-Bit-Muster sind explizit. Exakte Zahlen und Dezimalstrings bis UINT32_MAX übernehmen nicht die positiven pid_t-Grenzen von process_group_id/session_id.

Der Systembesitzer wandelt den Namen über das vorzeichenbehaftete int32-Kernelergebnis in raw64 um: 0x80000001 wird0xffffffff80000001 und UINT32_MAX wirdUINT64_MAX. Die Mach-Bindung erhält Flags und X1/RDX sowie die x64-RCX/R11-Regeln, ignoriert Argumente und greift nicht auf Speicher zu. Low32-Auflösung erhält die vollständige rohe Nummer im Ereignis. Mach-Abfragen lassen BSD error weg und erzeugen keine Scheduler-ThreadID. Wiederverwendung und unabhängige Optionen ändern die Beobachtungen nicht.

Die ursprüngliche ARM64-O0/O1/O2-Vorbereitung bewahrt432 rohe Beobachtungen, alle16 NZCV, High32-Präfixe, Registerwerte und SDK-Vergleiche. Native bit31-Portnamen wurden nicht beobachtet; hohe Vorzeichenerweiterung beruht auf dem festen XNU-Rückgabevertrag und unabhängigen Literaltests für Modell/Gast/öffentliche API. Das gemeinsame native Programm prüft nur Beziehungen im selben Prozess; virtuelle Namen und fehlende Eingaben sind keine deterministischen nativen Referenzen. Keine Namen/Sendereferenzen, lebenden Rechte, Eindeutigkeit oder IPC/Lebensdauer/Planung werden abgeleitet. Berechtigungen/ACL, Bereitschaftswarten, fortschreitende Uhren, echte Mach-IPC/Threads, dyld/TLS und vollständige Laufzeiten/Frameworks bleiben unvollständig. Native Intel HVF und physisches iOS bleiben ungeprüft.

```json
{"darwin_system":{"thread_self_port":2147483649,"task_self_port":0,"host_self_port":"4294967295"}}
```

```text
Mach thread_self_trap27 / task_self_trap28 / host_self_trap29
ThreadSelfPort / TaskSelfPort / HostSelfPort / uint32 / signed-int32 -> raw64
known0 / missing -> UnsupportedService / no arguments or memory
low32 resolution / complete raw number / flags and RDX-X1 preserved / no BSD error
mach-self-ports / mach-self-port-values / mach-self-port-missing
MachSelfPortsPreserveExplicitBitsAndIndependentRuns
6 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
70 mandatory workloads per platform / ARM64 210 / Intel 140 unverified
original ARM64 O0/O1/O2 probes432 / no native bit31 name observed
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU Mach trap table](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/syscall_sw.c), [self-port name owners](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/ipc_tt.c), [host-port owner](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/ipc_host.c), [ARM64 return](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [x64 return](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/i386/bsd_i386.c).


## Statische Berechtigungsabfragen für gewöhnliche Eigentümer

darwin_files.authorization="static-owner-queries" (DarwinFileAuthorization::StaticOwnerQueries) erklärt eine unveränderliche gewöhnliche lokale Umgebung: keine ACL, MAC, zusätzlichen kauth-Listener, Entitlements oder Umgehungen; beschreibbares, ausführbares, nicht opaques Mount mit Eigentumsprüfung; flags=0 und keine speziellen Modusbits. Beobachtungen allein erteilen keine Rechte. Unterstützt werden access/faccessat-Abfragen.

Eine echte Prüfung braucht explizite darwin_system.credentials und Metadaten des tatsächlichen Objekts. access verwendet real_uid, AT_EACCESS effective_uid für SEARCH und abschließendes R/W/X. Die gewählte UID muss ungleich0 und Eigentümer sein; alle verlangten Owner-Bits sind nötig. Bekannte Ablehnung liefert EACCES13 mit bisherigem BSD-Carry. Unbekannte Identität/Metadaten, gewählte UID0, Nicht-Eigentümer, erweiterte Aktionen und R/W/X eines behaltenen Endlinks stoppen UnsupportedService. Ungewählte UID0 bleibt gültig. Host, UID1000, Gruppen/Andere und Root-Ausnahmen werden nicht abgeleitet.

SEARCH prüft X des Elternobjekts vor Kindersuche, fehlendem Namen, einschlägigen Punkt/Punktpunkt-Schritten und Link-Neustart. F_OK/ignorierte Bits brauchen nur tatsächliches SEARCH. Nur-Slash-Root und an Root begrenztes Punktpunkt brauchen keines; verbrauchte Endslashs fügen keine Endprüfung hinzu. Reihenfolge von Flags, Kopie, relativem dirfd und leerem Namen bleibt. Name255 begrenzt Verfügbarkeit: nach erlaubt SEARCH ist Überschreitung Unsupported, Ablehnung vorher EACCES, unbekannt vorher Stopp; kein geratenes Dateisystem-errno.

Alle anderen Dateirouten einschließlich open/stat/chdir/readlink/Attribute/Enumeration/Namensmutation stoppen vor Effekten. Nur echte Input/Output/Error-Ströme und dup-Aliase behalten I/O, close, dup/dup2, lseek und fcntl; FD0/1/2 als Zahl reicht nicht. Datei-mmap/mappingSource sind geschlossen, anonymer Speicher unabhängig. Gemeinsame C++/JSON-Zulassung weist Änderungs/Erstellungsrechte, bekannte Flags/Spezialbits und inode/device-Aliase zurück; unbekannte Daten und256 Einträge/16MiB bleiben unverändert.

Das Beispiel liefert Root-SEARCH und0400-Eigentümerdatei: Lesenprüfung gelingt, Schreibenprüfung EACCES, open bleibt ununterstützt. ARM64 O0/O1/O2 behält2472 raw/SDK-Paare,2439 unabhängige Literale,33 reine Beobachtungen mit compile120s/native5s. fstatx/filesec bestätigt fehlendes ACL. Der erste NULL/ENOENT-Protokollfehler lag vor Abfragen und bleibt erhalten. Native real/effective-UIDs sind gleich; unterschiedliche Auswahl beruht auf festgelegter Quelle/Modell. Globale Hooks/opaque-Interna sind nicht vollständig beobachtet. owner-queries gehört nicht zu den58 deterministischen nativen Referenzen. Gruppen/Root/ACL/MAC, dynamische Identität, allgemeine vnode-Rechte, Bereitschaft, Uhren, Mach IPC/Threads, dyld/TLS und vollständige Laufzeiten bleiben offen; Intel HVF und physisches iOS ungeprüft.

```json
{"darwin_files":{"authorization":"static-owner-queries","files":[{"path":"/data","bytes_hex":"00","metadata":{"device":7,"inode":2,"mode":33024,"link_count":1,"uid":501,"gid":20,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16832,"link_count":2,"uid":501,"gid":20,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":501,"real_gid":20,"effective_gid":20}}}
```

```text
DarwinFileAuthorization::StaticOwnerQueries / authorization=static-owner-queries
access33 / faccessat466 / real_uid / effective_uid / AT_EACCESS0x10
owner R/W/X / all requested bits / directory SEARCH / EACCES13
no-action root LOOKUP / root-clamped dotdot / consumed terminal separators
unknown credentials-metadata-root-nonowner -> UnsupportedService
all other vnode routes closed / typed standard streams and dup aliases only
anonymous memory independent / file-backed mmap and mappingSource closed
Name255 availability stop after allowed SEARCH / no guessed filesystem errno
owner-queries / owner-query-stop / owner-query-open / owner-query-map
OwnerQueriesKeepPermissionAndUnknownBoundaries
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
original ARM64 O0/O1/O2 pairs2472 / literal2439 / capture-only33
native5s / compile120s / owner-build1200s / guest-Python5,000,000us
```
[XNU access and subject selection](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [real credential copy](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c), [owner authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [pathname SEARCH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c), [cached lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_cache.c), [Libc ACL properties](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/gen/filesec.c), [fstatx ACL absence](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/sys/statx_np.c).

## Statische gewöhnliche Abfragen mit teilweise bekanntem Gruppenwissen

`darwin_files.authorization="static-ordinary-queries"` (DarwinFileAuthorization::StaticOrdinaryQueries) behält die unveränderlichen Mount-, Sicherheits- und Metadatenannahmen des vorigen Abschnitts sowie geschlossene Dateioperationen, Standardstrom-Ausnahmen und unabhängigen anonymen Speicher. static-owner-queries bleibt auf Eigentümer begrenzt. Tatsächliche Prüfungen brauchen ausdrückliche Identitäten, Metadaten und eine ausgewählte UID ungleich0; root, erweiterte Rechte und R/W/X des letzten Links bleiben ausgeschlossen.

Eigentümer verwenden alle angeforderten Eigentümerbits. Für andere Benutzer werden Gruppen-/Sonstigen-Ergebnisse für die gesamte Maske verglichen. Gleiche Ergebnisse erlauben oder verweigern mit EACCES13 ohne Gruppenabfrage; verschiedene Bitmengen können beide verweigern. Andernfalls nutzt ein bekannter Mitgliedsstatus Gruppenbits, bewiesene Nichtmitgliedschaft Sonstigenbits, und unbekannte Mitgliedschaft stoppt vor Suche oder Effekten mit UnsupportedService. Klassen werden nicht vermischt.

credentials.groups ist die geordnete Liste im Kernel-Berechtigungsnachweis, mit EffectiveGID an Stelle0 und erhaltenen Duplikaten, nicht die erweiterte Resolverliste von SDK getgroups. Ausgewählte Primärgruppe und ausdrückliche positive Einträge sind bekannt; fehlende Einträge oder eine ausgelassene Liste beweisen gewöhnlich keine Nichtmitgliedschaft. Stimmen beide UID/GID-Paare überein, bleibt der reale Kontext unverändert. Sonst ersetzt RealGID Stelle0, und die erste zusätzliche RealGID-Stelle erhält den alten EffectiveGID. Ohne Treffer wird die alte Primärgruppe verdrängt und memberd deaktiviert. Dieser bewiesene Schritt oder ausdrücklich ursprüngliches KAUTH_UID_NONE macht zusammen mit einer vollständigen expliziten Liste negative Antworten bekannt. Unterschiedliche UIDs bei gleichen GIDs lösen ihn ebenfalls aus; eine doppelte Primärgruppe kann externe Mitgliedschaft unbekannt lassen. AT_EACCESS verwendet den ursprünglichen effektiven Kontext; die Eingabe bleibt gleich.

Das Beispiel verweigert die reale Abfrage nach Verdrängung von GID20 und erlaubt AT_EACCESS über die bekannte effektive Primärgruppe20. SEARCH folgt aus gleichen Gruppen-/Sonstigen-Ergebnissen; ausgewählte UID0 und Dateiöffnungen bleiben ausgeschlossen.

```json
{"darwin_files":{"authorization":"static-ordinary-queries","files":[{"path":"/data","metadata":{"device":7,"inode":2,"mode":32816,"link_count":1,"uid":700,"gid":20,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}},"bytes_hex":"00"}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16895,"link_count":2,"uid":0,"gid":0,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":502,"real_gid":30,"effective_gid":20,"groups":[20,40]}}}
```

Native schreibgeschützte ARM64-O0/O1/O2-Prüfungen erhalten270 raw/SDK-Paare für fremde Objekte, SEARCH, Fehler, Identität und unabhängiges Fehlen der ACL-Eigenschaft. Die rohe Identitätsliste hat16 Gruppen, die erweiterte SDK-Liste17. Der erste Versuch lehnte diese Länge fälschlich vor Berechtigungsabfragen ab; der Fehler bleibt dokumentiert. Reale/effektive IDs sind gleich: Unterschiede und vollständige Gruppentransformationen beruhen auf festgelegten XNU-Quellen und unabhängigen Modellen, nicht nativer Prüfung aller externen Hooks. Fünf Software- und drei ARM64-HVF-Konfigurationen prüfen echte Gäste, C/CLI/Python sowie unbekannte Identitäten, Metadaten, root und Mitgliedschaft. Das bereitgestellte Modell bleibt außerhalb der58 nativen gemeinsamen Referenzen. Vollständige Gruppenauflösung, root, ACL/MAC, allgemeine vnode-Operationen, dynamische Identitäten, Bereitschaft/Netzwerk, fortschreitende Uhren, Mach IPC/Threads, dyld/TLS und volle Frameworks bleiben offen; natives Intel HVF und physisches iOS sind ungeprüft. Alle Grenzen bleiben gleich.

```text
DarwinFileAuthorization::StaticOrdinaryQueries / authorization=static-ordinary-queries
owner bits / whole-mask group-world outcomes / EACCES13
credentials.groups / in-credential16 / EffectiveGID index0 / duplicates retained
real credential copy / first supplementary match / displacement disables memberd
missing membership usually unknown / original NONE or displaced real plus complete list proves negatives
all41 other file routes and direct/file-backed mappings closed / typed streams only
ordinary-queries / ordinary-query-unknown / ordinary-query-open / ordinary-query-map
OrdinaryQueriesPreserveGroupKnowledgeAndSelectedSearch
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
original ARM64 O0/O1/O2 nonowner pairs270 / raw-groups16 / SDK-extended-groups17
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU ordinary mode authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [real credential and group membership](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c), [raw in-credential getgroups](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c), [SDK extended getgroups](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/sys/getgroups.c).

## Ausdrücklicher ursprünglicher Gruppenmitgliedschaftskontext

Optionales `DarwinCredentials::GroupMembershipUID` / `darwin_system.credentials.group_membership_uid` deklariert den ursprünglichen cr_gmuid unabhängig von vier IDs und groups. Zulässig sind0..INT32_MAX oder genau KAUTH_UID_NONE=4294967195 (0xffffff9b, UINT32_MAX minus100). Der bestehende Decoder nimmt verlustfreie Dezimalzeichenfolgen und exakte numerische Ganzzahlen an; falsche Typen, Brüche, negative und andere unzulässige Werte werden vor Laden verweigert. Der Sentinel bleibt in normalen UID/GID und Gruppeneinträgen unzulässig. Auslassung und andere zulässige UIDs beweisen keine externe Nichtmitgliedschaft und aktivieren keinen Resolver.

Primärgruppe und positive Einträge werden zuerst erkannt. Ursprüngliches NONE plus vollständige explizite Liste beweist fehlende Mitgliedschaft; ohne Liste bleibt sie unbekannt. Die reale Kopie erhält ursprüngliches NONE auch bei erstem zusätzlichen Treffer mit erhaltener alter Primärgruppe; bewiesene Verdrängung deaktiviert ebenfalls externe Auflösung. Skalare Abfragen, raw getgroups, Erstellungseigentum und ältere Modi bleiben gleich. Derselbe Abfragebesitzer wählt Sonstigenrechte und prüft SEARCH vor Kindersuche; andere vnode-Operationen bleiben geschlossen.

Der Aufrufer im Beispiel gehört nicht GID50 an: beide Identitäten dürfen /data lesen, eine Schreibrechteabfrage liefert EACCES13. Ohne group_membership_uid bleiben unterschiedliche Gruppen-/Sonstigenergebnisse ununterstützt.

```json
{"darwin_files":{"authorization":"static-ordinary-queries","files":[{"path":"/data","bytes_hex":"00","metadata":{"device":7,"inode":2,"mode":32772,"link_count":1,"uid":700,"gid":50,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16895,"link_count":2,"uid":0,"gid":0,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":501,"real_gid":20,"effective_gid":20,"groups":[20],"group_membership_uid":4294967195}}}
```

Lokale O0/O1/O2-SDK-Ausführungen prüfen nur Sentinel und vier Byte uid_t, nicht Host-cr_gmuid oder Resolverzustand; sie ersetzen nicht die früheren270 tatsächlichen raw/SDK-Nichteigentümerpaare. ordinary-queries-closed-groups prüft173 Ereignisse, effektive Nichtmitgliedsverweigerungen und SEARCH für fehlende Kinder, Punkt/Doppelpunkt und Links in fünf Software-/drei obligatorischen ARM64-HVF-Profilen sowie C/CLI/Python. Es bleibt außerhalb58 native-common Referenzen. Root, ACL/MAC, vollständige Gruppenauflösung, allgemeine vnode-Autorisierung, dynamische Credentials, Warten/Netzwerk, fortschreitende Uhren, Mach IPC/Threads, dyld/TLS und vollständige Frameworks bleiben offen. Intel HVF/physisches iOS sind ungeprüft; bestehende Fristen bleiben gleich.

```text
GroupMembershipUID / group_membership_uid / original cr_gmuid
0..INT32_MAX or KAUTH_UID_NONE=4294967195 / 0xffffff9b / not UINT32_MAX
positive entries first / original NONE plus complete list proves negatives
omitted list unknown / first-match real copy preserves original NONE
ordinary-queries-closed-groups / 173 events / stdout GN
OrdinaryQueriesUseExplicitMembershipUIDWithoutResolver
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
SDK constant O0/O1/O2 only / prior actual nonowner pairs270 remain separate
58 native-common references unchanged / Intel and physical iOS unverified
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU KAUTH_UID_NONE](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/kauth.h), [XNU credential membership](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c).

## Sofortige Bereitschaft gewöhnlicher Dateien

Raw BSD poll(230) und poll_nocancel(417) unterstützen timeout low int32=0. nfds wird uint32; über OPEN_MAX10240 folgt EINVAL22 vor Zeigern. Positive Anzahlen brauchen explizites darwin_system.resource_limits resource8 Current. Darüber und über FD_SETSIZE1024 folgt EINVAL; kleinere Anzahlen brauchen die ursprüngliche explizite effektive UID, nichtnull ergibt EINVAL, null erlaubt. Die root-Ausnahme ist nur Quell-/Modellevidenz. DescriptorLimit, kern.maxfilesperproc, FD-Zahl und Laufzeitbudgets liefern diese Beobachtung nicht. Null Anzahl berührt keinen Arrayzeiger; anderer timeout bleibt nach Anzahlzulassung unsupported.

Der konstruierte virtuelle reguläre Anbieter erklärt nicht widerrufene Beschreibungen, erfolgreiche gewöhnliche Lese-/Schreibfilterregistrierung und keine MAC/Anbieterverweigerung. Der native vnode-Typ beweist keine Registrierung. Unter dieser Voraussetzung sind IN/RDNORM und OUT/WRBAND auch bei EOF/readonly bereit, ohne Ableitung aus Restbytes, Zugriffsflags, O_NONBLOCK oder Schreibrechten. Numerische FD/Lese- und FD/Schreibschlüssel behalten jeweils die letzte Zeile; verschiedene dup-FDs bleiben getrennt. HUP allein registriert Lesen ohne Bereitschaftsbit. Negative FD/ignorierte Bits ergeben null, geschlossene Registrierungen pro Zeile POLLNVAL32. Live OOB/vnode, Streams/Verzeichnisse/Links werden vor Ausgabe verweigert. Ganze 8-Byte-Eingabe wird aufgenommen; unvollständige Eingabe/komplett unschreibbare Ausgabe ergibt EFAULT14, partielle Ausgabe wird ohne Präfixkopie verweigert. Ganze Ausgabe erhält fd/events und ersetzt revents. Beide statischen Berechtigungsmodi sperren Poll vor Vorprüfung, auch bei null Anzahl; die gemeinsame Sperrliste enthält41 Routen.

SDK-freies immediate-poll umfasst86 Ereignisse, fünf Software-/drei erforderliche ARM64 HVF-Profile sowie C/CLI/Python und eine neue gemeinsame native Referenz, aktuell59 Fälle. Die unabhängige O0/O1/O2 ARM64-Probe hält582 gemischte literale ABI/Fehlerkontrollen und reguläre Anbieterbeobachtungen fest, keine universellen Bereitschaftskonstanten. Frühere58 Belege behalten Quellidentität; Downloadfehler, erster nativer Timeout und ursprüngliche Fristen bleiben erhalten. Intel HVF, physisches iOS, Widerruf/MAC/Registrierungsverweigerung, select, Warten, asynchrone Anbieter, Netzwerk, fortlaufende Uhren, echte Mach IPC/Threads, dyld/TLS und vollständige Frameworks bleiben ungeprüft oder unvollständig.

```text
poll230 / poll_nocancel417 / timeout low int32=0 / nfds uint32
ResourceLimits[8].Current / OPEN_MAX10240 / FD_SETSIZE1024 / explicit original effective UID
constructive regular provider: nonrevoked / successful ordinary filter attachment / no MAC-provider refusal
numeric FD + read/write filter / independent last requested index / distinct dup aliases
IN1 RDNORM64 OUT4 WRBAND256 / HUP16 trigger only / closed registrations POLLNVAL32
negative and ignored-only rows zero / whole input snapshot / whole output / partial output unsupported
EFAULT14 EINVAL22 / no ready prefix on unsupported row / zero count no pointer
static-owner-queries and static-ordinary-queries / all41 other file routes closed before preflight
immediate-poll86 events / five software + three mandatory ARM64 HVF profiles / C CLI Python
59 current native-common cases / prior58 source identity preserved / 582 mixed controls and captures
native Intel and physical iOS unverified / waits select revoked-MAC-provider failures networking unfinished
```

[XNU poll ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/poll.h), [poll registration and copy order](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [vnode registration and regular filters](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).
