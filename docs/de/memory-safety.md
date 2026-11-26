**Sprachen**: [English](../memory-safety.md) | [简体中文](../zh-CN/memory-safety.md) | [繁體中文](../zh-TW/memory-safety.md) | [日本語](../ja/memory-safety.md) | [한국어](../ko/memory-safety.md) | [Français](../fr/memory-safety.md) | [Deutsch](memory-safety.md) | [Español](../es/memory-safety.md) | [Italiano](../it/memory-safety.md) | [Русский](../ru/memory-safety.md) | [العربية](../ar/memory-safety.md)

[← Dokumentationsindex](README.md)

# Speicher-Audit und Hunt

Mit `neverd scan <binary>` laufen beide Analysen. Mit `--analyzer=audit` oder `--analyzer=hunt` lässt sich eine einzelne Analyse auswählen. [Leitfaden zur gemeinsamen Prüfung (Englisch)](../memory-safety-scan.md).

NeverD analysiert ein geladenes Binärfile auf zwei Familien von Speichersicherheitsfehlern und berichtet sie als strukturiertes JSON. Beide Spuren laufen auf dem formatneutralen, gelifteten IR, daher sind **PE/COFF, ELF und Mach-O gleichrangige Erstklassziele** — ein Fund hängt nie an einem format-spezifischen Scanner oder einer Importtabelle.

| Spur | Befehl | Berichtet |
|------|--------|-----------|
| **Audit** | `neverd audit <binary>` | Heap-Lebensdauerfehler und uninitialisierte lokale Stack-Reads |
| **Hunt** | `neverd hunt <binary>` | Gefährliche Copy-Überläufe mit symbolischer Evidenz und Eingabekandidaten; `replayable=true` nur mit einem vollständigen `process-input-v1`-Plan |

Die Engine nutzt NeverDs eigene symbolische Ausführung und den Bitvektor-Solver für Zeugen und Erreichbarkeit. Kein externer Solver, keine VM, kein Container.

---

<!-- i18n-section: core-invariant -->

## Kerninvariante: geschlossen fehlschlagen

Eine nicht geliftete Operation, ein Aufruf, dessen Argumente die ABI-Passage nicht zurückgewinnen konnte, ein unaufgelöstes indirektes Ziel oder ein erschöpftes Budget ergeben **UNKNOWN**, niemals SAFE. Ein Zielpuffer, dessen Kapazität nicht zurückgewonnen werden kann, ist UNKNOWN. Striktes Lifting bleibt unverändert; die Sicherheitsschicht legt nur konservative Urteile darauf.

Aufrufeffekte folgen einer Closed-World-Semantik: Eine Zusammenfassung gilt nur, wenn ihre Vorbedingungen und alle relevanten Effekte bekannt sind. Ein unbekannter Effekt oder eine nur teilweise anwendbare Zusammenfassung bleibt UNKNOWN; die Lücke wird weder als wirkungslos noch als erfolgreicher Aufruf angenommen.

---

<!-- i18n-section: identity -->

## Identitätsvertrag je Format

Beide Spuren brauchen die Lift-Pipeline (sie stellt Argumente pro Aufruf wieder her). Callee-Namen kommen aus derselben Identitätsansicht wie der Rest von NeverD. Die Reihenfolge der Debug-Entdeckung bleibt gleich.

| Format | Debug (nach Vorrang) | Import- / Thunk-Auflösung |
|--------|----------------------|---------------------------|
| **PE/COFF** | `--pdb`, Debug-Verzeichnis oder benachbartes `.pdb`, dann MSVC `/MAP` | IAT-Slots und `__imp_`-Thunks, ordinale Imports |
| **ELF** | DWARF im Image, getrenntes `*.debug`, dann GNU/LLD-MAP | PLT-Stubs auf den Importnamen aufgelöst |
| **Mach-O** | DWARF im Image, benachbartes `.dSYM`, dann ld64 `-map` | dyld-Bind / indirekte Symbolslots und Stub-Helfer |

`--pdb` / `--map` benennen eine maßgebliche Begleitdatei: Lesefehler ist ein Fehler, kein stiller Fallback. `--no-debug` liest auf jedem Format nur das Image.

PDB-Prozedursignaturen dienen dazu, wertliefernde Allokatoren von `void`-Freigabefunktionen zu unterscheiden. Die umfassende Wiederherstellung lokaler und Stack-Typen aus einem PDB bleibt begrenzt; lässt sich keine exakte Objektgröße bestimmen, fällt die Jagd auf das Rahmen- bzw. Allokationsmodell zurück und meldet UNKNOWN, statt eine Größe zu erfinden.

<!-- i18n-section: name-precedence -->

### Vorrang von `name_source`

Jeder Fund trägt ein `name_source`, das die Herkunft des Callee-Namens nach diesem Vorrang beschreibt:

1. `rename` — eine vom Aufrufer gesetzte Umbenennung
2. `import` — ein IAT- (PE), PLT- (ELF) oder dyld-Bind-/Stub-Eintrag (Mach-O)
3. `export` / `symbol` — ein bereits vom Image angegebener Export oder Symboltabelleneintrag
4. `pdb` / `dwarf` / `map` — ein Debug-Symbol, das einen Platzhalter festlegt oder mit dem angegebenen Namen übereinstimmt
5. `sig` — ein Signaturtreffer
6. `synthetic` — Platzhalter für eine namenlose Routine

Ein statisch gelinktes, von DWARF benanntes `memcpy` berichtet `dwarf`; ein importiertes `memcpy` berichtet auf jedem Format `import`. Ein Signaturtreffer verdrängt niemals einen Namen, den Debugger oder Importtabelle bereits genannt haben.

---

<!-- i18n-section: catalog -->

## Senken- und Quellenkatalog

Der Katalog ist eine konfigurierbare Tabelle, keine fest verdrahtete Menge. Jede **Senke** deklariert ihre Schwachstellenklasse, ihre Rolle (copy, format, alloc, free, realloc) und die relevanten Argumentslots (Ziel, Quelle, Länge, Kapazität). Eine JSON-Senke der Art copy oder format liefert zusätzlich einen ausführbaren Aufrufeffekt. Jede **Quelle** benennt einen Anbieter angreiferbeeinflusster Eingabe.

Die eingebauten Einträge stehen in [`SafetySinks.def`](../../include/neverd/safety/SafetySinks.def) und [`SafetySources.def`](../../include/neverd/safety/SafetySources.def); sie decken die übliche C-Laufzeit-Copy-Familie (`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), gehärtete `_chk`-Varianten (explizite Zielschranke), Allokation und Freigabe (`malloc`/`calloc`/`realloc`/`free`, Operator `new`/`delete`) sowie optionale Win32-Heap-APIs. Eingabequellen umfassen POSIX (`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, Programargumente) **und** Win32 (`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`). Ein PE-Hunt ist nicht auf POSIX-Eingaben beschränkt.

Format-spezifische Schreibweisen fallen auf einen Eintrag: führende Unterstriche werden entfernt (`_malloc`, `___strcpy_chk`), gemangled `new`/`delete` über Aliase.

Fehlt `effect` bei einer JSON-Senke der Art copy oder format, wird ihre Anwendbarkeit aus dem höchsten referenzierten Argumentslot abgeleitet. Eine copy-Senke verlangt dann genau diese Argumentzahl; eine format-Senke akzeptiert Aufrufe von dieser Mindestzahl bis zur variadischen Obergrenze. Ein optionales `effect`-Objekt kann mit `min_arity` und `max_arity` (oder `"variadic"`) ausdrücklich einen akzeptierten Argumentbereich festlegen, einschließlich zusätzlicher Wrapper-Argumente über die abgeleitete exakte copy-Arity hinaus; `min_arity` muss mindestens dem höchsten referenzierten Rollenslot plus eins entsprechen, während `formats` und `abis` die Anwendbarkeit einschränken. Stimmen Argumentzahl, Objektformat oder ABI des Aufrufs nicht überein, gilt keine Zusammenfassung und das Closed-World-Ergebnis bleibt UNKNOWN.

Katalog per Spezifikationsdatei erweitern oder überschreiben:

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 },
    { "name": "my_format", "kind": "format", "dst": 0, "fmt": 2,
      "effect": { "min_arity": 3, "max_arity": "variadic",
                  "formats": ["elf"], "abis": ["sysv"] } }
  ],
  "sources": [
    { "name": "my_read", "out": 1, "return_tainted": true }
  ]
}
```

Bei einer benutzerdefinierten Quelle sind `out` und `return_tainted` ausschließlich Metadaten für die Erkennung. Sie begründen keine ausführbaren Speicher-, Rückgabewert- oder Taint-Effekte. Dem aktuellen Quellschema fehlen die dafür nötigen typisierten Erfolgs-, Mutations-, Format- und ABI-Verträge; eine Analyse, die von einem solchen benutzerdefinierten Quelleffekt abhängt, bleibt daher UNKNOWN. Eingebaute Quellen sind davon nicht betroffen: Ihre typisierten, auf Anwendbarkeit geprüften Deskriptoren liefern weiterhin ausführbare Effekte.

Eine unbegrenzte benutzerdefinierte Senke nur mit Zielargument wird nicht aus einem gleichnamigen Quelleintrag abgeleitet. Eine `gets`-artige benutzerdefinierte Senke muss `"unbounded": true` ausdrücklich setzen; derselbe Name im Quellenkatalog verleiht ihr keinen ausführbaren Effekt, und widersprüchliche Quell-/Längenfelder werden transaktional abgewiesen.

---

<!-- i18n-section: copy-overflow -->

## Hunt: Urteile zu Copy-Überläufen

Für jede Copy-Senke ermittelt der Hunt die Zielkapazität — debug-deklarierte Arraygröße, dann Heap-Allokationsstelle bekannter Größe, dann eine solide Stack-Frame-Schranke — und klassifiziert das Argument, das die Schreiblänge bestimmt, per rückwärtigem SSA-Gang (Spill/Reload über Stackslots):

- **Konstante Länge** innerhalb einer exakten Kapazität ist SAFE. Ein konstanter Überlauf ist nur UNSAFE, wenn die Senke auf einem bestätigten Pfad erreichbar ist; andernfalls bleibt er UNKNOWN.
- **Gehärtete** `_chk`-Kopien tragen eine Laufzeit-Zielschranke. Eine Zurückweisung oder eine nachweislich passende Schranke ist SAFE; ein möglicher Schreibzugriff über das Objekt hinaus ist UNSAFE; eine nicht rekonstruierte oder nicht entscheidende Schranke ist UNKNOWN.
- **Beweisbar beschränkte** Länge (längenrückgebender Aufruf, Maske, Clamp) wird vor dem Solver mit Begründung zurückgezogen. SAFE gilt nur bei exakter Zielgröße; eine reine Regionsobergrenze bleibt UNKNOWN.
- **Angreiferbeeinflusste** Länge bei bekannter Kapazität: Bitvektor-Solver. Ist eine Länge größer als die Kapazität erfüllbar, ist das Urteil UNSAFE. Kandidaten sind nur mit einem vollständigen `process-input-v1`-Plan abspielbar: zunächst exakte literale Umgebungswerte und höchstens die vom ersten unterstützten `read(0)`-Familienaufruf zurückgegebenen Standardeingabe-Bytes. argv-, Datei-, Netzwerk-, benutzerdefinierte oder mehrdeutige Eingaben bleiben mit Begründung nicht abspielbar.
- Alles andere — unbekannte Länge oder unbekannte Kapazität — ist UNKNOWN.

Jede zurückgewonnene Kapazität ist eine **obere Schranke** der wahren Objektgröße, daher ist ein bewiesener Überlauf niemals ein False Positive.

<!-- i18n-section: formatted-input -->

### Formatierte Eingabe

Für `scanf`/`fscanf` und ihre versionierten Schreibweisen ordnet ein lesbares konstantes Format jede nicht unterdrückte Konvertierung ihrem tatsächlichen variadischen Ausgabeargument zu. Unbegrenzte `%s`/`%[`-Ausgaben markieren spätere Stringverwendungen als Taint; numerische und Zeichenausgaben markieren Werte als Taint, die aus dem geschriebenen Objekt geladen werden, nicht aber den Wert des Ausgabezeigers selbst. `sscanf` propagiert diese Effekte nur, wenn seine Eingabezeichenfolge bereits vom Angreifer beeinflusst ist. Begrenzte Textausgaben wie `%Ns`/`%N[` propagieren Taint zusammen mit einer `MaxBytes`-Ausdehnung einschließlich Terminator; Wide-Character-Varianten berechnen diese Byteausdehnung mit der plattformspezifischen `wchar_t`-Breite. Unterdrückte Konvertierungen, überzählige Argumente, positionsabhängige oder nicht unterstützte Formate und `%n` bleiben UNKNOWN, statt erraten zu werden.

<!-- i18n-section: formatted-output -->

### Formatierte Ausgabe

`snprintf`/`vsnprintf` behalten ihre maximale Schreiblänge; gehärtete `_chk`-Varianten zusätzlich die vom Compiler gelieferte Zielobjektgröße. Beide Grenzen sind unabhängig. Ein vertrauenswürdiges konstantes Format nur aus Literalbytes und `%%` hat eine genaue Ausgabelänge einschließlich NUL, geprüft mit demselben Heap-/Stack-Zielmodell wie Kopiersenken. Passt die maximale Schreiblänge in die genaue Kapazität, gilt SAFE; ein zu großes Literal ergibt UNSAFE. Wertkonvertierungen bleiben bis zum Beweis ihrer Ausgabegrößen UNKNOWN. UNSAFE mit hoher Konfidenz braucht außerdem einen LowIR-Pfadbeweis der Erreichbarkeit. Angreiferkontrollierte Formate werden unabhängig von Kürzung als `format_string` gemeldet.

---

<!-- i18n-section: heap-lifetime -->

## Audit: Urteile zur Heap-Lebensdauer

Für jede Allokation verfolgt das Audit den Handle im Kontrollflussgraphen, einschließlich Stack-Spill/Reload, und wendet eine Escape-Zusammenfassung an (zurückgegeben, über eine Nicht-Stack-Adresse gespeichert oder an einen opaken Callee übergeben):

- **Leak** — der Handle wird weder freigegeben noch darf er entkommen.
- **Double-Free** — eine zweite Freigabe ist nach einer ersten auf einem Pfad erreichbar.
- **Use-after-Free** — eine Dereferenzierung oder opake Nutzung ist nach einer Freigabe erreichbar.

Allokations- und Freigabe-**Wrapper** werden über funktionsweise Escape-Zusammenfassungen erkannt, sodass ein `malloc`/`free`-Forwarder den Fehler nicht verdeckt. Freigaben auf einander ausschließenden Zweigen gelten nicht als Double-Free.

Der Heap-Automat gibt zunächst eine Kandidaten-Ereignisfolge aus (Allokation, Freigabe, Verwendung oder Austritt per Rückgabe). Ein zweiter Durchlauf muss diese Folge auf einem symbolischen LowIR-Pfad nachspielen und dessen Pfadprädikat als erfüllbar nachweisen, bevor der Befund ein UNSAFE mit HOHER Konfidenz wird. Fehlendes LowIR, undurchsichtige Operationen, Aufrufe ohne Zusammenfassung, Unsicherheit des Solvers und Explorationsgrenzen stufen den Kandidaten auf UNKNOWN herab. Konservativer May-Alias-Speicher-Havoc wird getrennt verfolgt, damit gewöhnliche Schreibzugriffe auf den Stapelrahmen einen ansonsten exakten Erreichbarkeitsnachweis nicht entwerten.

---

<!-- i18n-section: stack-initialization -->

## Audit: Initialisierung lokaler Stack-Slots

Das Audit verfolgt vollständige Schreibzugriffe vor Lesezugriffen auf lokale Slots unter dem Eintritts-Stackpointer. Ein Lesen ohne mögliche vorherige Initialisierung wird als `uninitialized_read` gemeldet. Erst ein LowIR-Pfad-Replay der Erreichbarkeit erlaubt UNSAFE mit hoher Konfidenz. Bedingte Initialisierung, Teilzugriffe, entkommene Slot-Adressen und unklare Definitionen bleiben UNKNOWN. Aufrufereigene Argument-Slots am Eintritts-Stackpointer oder darüber sind ausgenommen.

---

<!-- i18n-section: reachability -->

## Interprozedurale Erreichbarkeit ab bekannten Einstiegspunkten

Jeder Befund enthält drei unabhängige Aussagen, die nicht miteinander
gleichgesetzt werden dürfen:

| Feld | Frage | Werte |
|------|-------|-------|
| `verdict` | Was beweist die lokale Sicherheitsanalyse über die Operation? | `SAFE`, `UNSAFE`, `UNKNOWN` |
| `reachability.status` | Liegt die enthaltende Funktion auf einem rekonstruierten Kontrollpfad ab einem bekannten nativen Einstieg? | `REACHABLE`, `UNREACHABLE`, `UNKNOWN` |
| `reachability.attacker_control` | Was beweist der Argument-Slice über den Einfluss eines Angreifers an diesem Befund? | `TAINTED`, `BOUNDED`, `UNKNOWN` |

Die Erreichbarkeit ist additive Evidenz: Sie ändert weder den `verdict` eines
Befunds noch das Gesamturteil oder den CLI-Exit-Code. Daher kann ein lokal
bewiesener Überlauf `verdict=UNSAFE` und zugleich
`reachability.status=UNREACHABLE` tragen. Verbraucher, die einen ausführbaren
Angriffspfad verlangen, müssen beide Felder prüfen.

Die Wurzeln sind erkannte Anwendungseinstiege (`application`, etwa `main` oder
`WinMain`), der Image-Einstieg (`image`) und exportierte Routinen (`export`).
Wenn dieselbe Funktion mehrere Identitäten hat, gilt die deterministische
Reihenfolge `application`, `image`, `export`. `reachability.entry` enthält
`va`, `name` und `kind`. Für einen erreichbaren Nicht-Wurzel-Befund enthält
`call_chain` außerdem einen kürzesten deterministischen Pfad aus exakten
internen Kanten mit `caller_va`, Aufrufstelle `call_va`, `callee_va` und dem
`kind` `direct` oder `indirect`.

`UNREACHABLE` wird nur bei vorhandener Wurzel, vollständigem internen
Aufrufinventar und nicht erschöpfter Tiefengrenze ausgegeben. Bei einer nicht
anderweitig positiv erreichten Funktion verhindern fehlende Wurzeln, doppelte
oder mehrdeutige Funktionsidentitäten, inkonsistente CFG-/Aufrufdaten,
unaufgelöste ausführbare interne Ziele und erschöpfte Tiefe einen negativen
Nachweis und ergeben `reachability.status=UNKNOWN`, gegebenenfalls mit `reason`
und `budget_hit`.
Eine unbekannte ABI, nicht passende Argumentbreite, nur variadischer Slot,
unvollständiger Slice sowie erschöpfte Tiefen- oder Zusammenfassungsbudgets
lassen auch jede noch unbelegte Angreifersteuerung UNKNOWN; bereits bewiesene
Fakten bleiben gültig und es wird keine Propagation erfunden.

Die Reportzähler zählen Befunde, nicht Funktionen oder Pfade.
`control_reachable` zählt `status=REACHABLE`; `attacker_reachable` ist die
Teilmenge mit zusätzlichem `attacker_control=TAINTED`.
`reachability_unknown` und `unreachable` zählen die übrigen Kontrollzustände.
Sie sind von `safe`, `unsafe` und `unknown` für Urteile getrennt.

---

<!-- i18n-section: budgets -->

## Budgets, Ausgabe und Bindings

Hunt-Exploration und Solver sind begrenzt (`--max-paths`, `--max-steps`, `--max-loop`, `--solver-conflicts`). Interprozedural begrenzt `max_call_depth` die Zahl interner Aufrufkanten eines bekannten Einstiegspfads; `max_summary_iterations` begrenzt die Fixpunktrunden der Angreifersteuerung. Die Standardwerte sind 64 Kanten beziehungsweise die effektive Tiefengrenze plus eine Runde. Erschöpfung schlägt wie oben beschrieben geschlossen fehl. Ist `max_call_depth` erschöpft, kann eine noch nicht erreichte Funktion `status=UNKNOWN` bleiben; eine Erschöpfung von `max_summary_iterations` löscht den strukturellen Zeugen nicht, sodass `status=REACHABLE` mit `attacker_control=UNKNOWN` und `budget_hit=true` zusammen auftreten kann. Beide Befehle drucken JSON und respektieren `-o`. Der Exit-Code ist `0` für SAFE, `2` für UNSAFE und `1` für UNKNOWN oder einen Fehler.

Null wählt auf jeder öffentlichen Oberfläche den Engine-Standard:

| Oberfläche | Kontrolltiefe | Angreifer-Zusammenfassung |
|------------|---------------|---------------------------|
| CLI (`audit` und `hunt`) | `--max-call-depth <n>` | `--max-summary-iterations <n>` |
| C (`neverd_safety_options`) | `max_call_depth` | `max_summary_iterations` |
| Python (`Session.audit()` / `Session.hunt()`) | `max_call_depth=<n>` | `max_summary_iterations=<n>` |

C-Aufrufer initialisieren `neverd_safety_options` mit Null und setzen
`struct_size=sizeof(neverd_safety_options)`; ältere Strukturgrößen behalten die
Standardwerte. Python validiert beide Werte als vorzeichenlose 32-Bit-Zahlen.

Dieselben Analysen stehen über die C-API (`neverd_session_audit_json` / `neverd_session_hunt_json` mit versioniertem `neverd_safety_options`) und das Python-SDK (`Session.audit()` / `Session.hunt()`) zur Verfügung.

<!-- i18n-section: finding-schema -->

### Fundschema

```json
{
  "class": "buffer_overflow",
  "function": "parse_header",
  "name": "strcpy",
  "name_source": "import",
  "call_va": "0x11a4",
  "source": "reader.c:42",
  "sink": "strcpy",
  "arg_index": 1,
  "flow": "TAINTED",
  "verdict": "UNSAFE",
  "confidence": "HIGH",
  "capacity": 16,
  "capacity_kind": "exact",
  "corroboration": "path predicate and overflow are jointly satisfiable",
  "reachability": {
    "status": "REACHABLE",
    "attacker_control": "TAINTED",
    "budget_hit": false,
    "entry": { "va": "0x1000", "name": "main", "kind": "application" },
    "call_chain": [
      { "caller_va": "0x1000", "call_va": "0x1080",
        "callee_va": "0x1100", "kind": "direct" }
    ]
  },
  "evidence": {
    "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" },
    "candidate_values": [
      { "name": "copy_length", "value": "17" },
      { "name": "argv[1]", "value": "16 bytes" }
    ],
    "replayable": false,
    "replay": {
      "adapter": "process-input-v1",
      "reason": "argv input is not supported by process-input-v1"
    },
    "symbolic_model": [
      { "id": 0, "name": "copy_len", "width": 64,
        "value_hex": "0x11", "origin": "input" }
    ]
  }
}
```

`replayable` ist abgeleitete Evidenz, kein eigenständiges Versprechen: Es ist nur wahr, wenn `replay` einen vollständigen Eingabeplan für den Adapter `process-input-v1` enthält. Der Plan hält exakte Umgebungsbytes, gegebenenfalls die Bytefolge des ersten unterstützten `read(0)`-Familienaufrufs sowie Bindungen von Solver-Zuweisungs-IDs an diese Eingaben fest; andernfalls erklärt `replay.reason` den Grund. Replay- und Erreichbarkeitsfelder sind additiv; die oberste `schema_version` bleibt `1`.

---

<!-- i18n-section: strict-publication -->

## Strikte Laufzeitprüfungen und authentifizierte Veröffentlichung

`binary-sanitizer-v1` ist eine separate experimentelle Änderungstransaktion; Audit-/Hunt-Urteile bleiben unverändert. Ein abgeschlossener strikter Hunt muss jeden Befund einem genauen Vorkommen eines gezählten Schreibzugriffs mit exakter Restkapazität und passenden Compiler-Aufrufstellenmetadaten zuordnen. Nicht unterstützte Befunde, veraltete/mehrdeutige Identität, unvollständiges Lifting, erschöpfte Budgets, Generierungsfehler und Ziel-/Signaturgrenzen lehnen die gesamte Transaktion ab. Es gibt keinen Teilerfolg. Relokierbare Objekte, dynamische Bibliotheken, universelle Mach-O-Dateien und geschützte Mach-O-Bundle-Mitglieder werden ebenfalls abgelehnt.

Öffentliche Einstiege sind C `neverd_session_sanitize`, CLI `neverd patch --sanitize=strict` und Python `Session.sanitize`. C-Aufrufer müssen für authentifizierte Veröffentlichung zuerst `neverd_sanitize_publication_abi_version()` prüfen. Alle verlangen einen vollständigen, konsistenten publication receipt v1. Nicht-Darwin-Hosts authentifizieren und normalisieren die Eingabe, liefern dann `UNSUPPORTED_TARGET` vor Lifting, Guard-Erzeugung, Kandidatenerstellung oder Namensraumänderung.

Darwin kennt zwei Erfolgsformen. `CREATE_EXCLUSIVE` veröffentlicht einen neuen Kandidaten im selben Verzeichnis mit einer atomaren Nicht-Ersetzen-Operation an ein fehlendes Ziel. Der Beleg bestätigt Namensraumatomarität, exklusive Erstellung und tatsächliche Operandenbindung, aber weder Ersetzungs-CAS noch Absturzdauerhaftigkeit. `NO_CHANGE` ist schreibgeschützt: Ein leerer Plan authentifiziert das gehaltene geladene Quellobjekt erneut und meldet `NOT_PUBLISHED`, ohne Veröffentlichungs- oder Operandenbindungsgarantien. Ein Guard-Plan darf die geladene Quelle nicht als Ausgabe verwenden. Ein anderes bestehendes Ziel wird nie ersetzt; ein neuer Pfad ist erforderlich. Unbestimmte Veröffentlichung oder unvollständiger Abschlussbeleg bedeuten Fehler; das Ziel kann existieren und muss vor Nutzung, Wiederholung oder Löschung geprüft werden.

Quellbytes werden mit dem externen Sitzungsdigest verglichen und über einen gehaltenen Deskriptor beobachtet. Quelle und Zielverzeichnis bleiben während der Transaktion verankert. Metadaten und stabile Identität sind zeitpunktbezogene Beobachtungen, kein Unverändertheitsbeweis seit dem ursprünglichen Laden. Ein geöffnetes Verzeichnis kann umbenannt werden: Der Beleg authentifiziert das gehaltene Objekt, nicht die fortlaufende Bindung des ursprünglichen Pfads während oder nach der Transaktion. Er ist keine selbstständig überprüfbare dauerhafte Pfadbindung; späteres Öffnen verlangt einen externen Anker und erneute Authentifizierung. Darwin rename verwendet einen Kandidatenpfad statt eines gehaltenen Deskriptors. Deshalb muss das Verzeichnis der effektiven uid gehören, ohne erweiterte ACL oder Gruppen-/Fremdschreibrechte auf einem Volume mit bestätigtem normalem POSIX-Eigentum. Diese Bedingungen werden vor Erstellung und Veröffentlichung erneut geprüft. Gleich privilegierte Prozesse, root/DAC-Umgehung und feindliche Dateisysteme/Server sind ausgeschlossen; Kernel, VFS und Dateisystem müssen ihre gemeldete Semantik einhalten.

---

<!-- i18n-section: native-replay -->

## Nativer Prozess-Replay: nur Phase 0

Der plattformneutrale `process-replay-v1`-Plan und sein Koordinator definieren Beweise für eine künftige authentifizierte Ausführung. Derzeit kann kein Host sie ausführen. `NativeProcessReplayAdapter` ist nur eine C++-Verfügbarkeits-/Factory-Grenze, keine C-, Python-, CLI- oder JSON-Ausführungsschnittstelle.

Die unverändernde Abfrage prüft vollständigen Plan, Ausführungsgrenzen, absoluten Executable-Locator und eindeutige physische Aufrufstellen, ohne Zielöffnung, Backend-Callback oder Prozessstart. Der Locator ist künftige Objektsuche und darf nach Authentifizierung keine pfadbasierte Ausführungsautorität werden. Fehlt eine Garantie für Isolation, Identität, Eingabevermittlung, Vorkommensattestierung, Grenzen oder Bereinigung, bleiben `Available` und alle Fähigkeiten false; die Factory liefert einen Fehler ohne Operationstabelle.

Das gilt für alle Hosts. Linux benötigt noch vertrauenswürdige statische ELF-Instrumentierung, dauerhafte Überwachung, vollständige Isolation, Grenzen und Laufzeitattestierung. Unterstützte öffentliche macOS-Mechanismen liefern nicht gemeinsam Ausführung gehaltener Objekte, Sandbox beliebiger Ziele vor deren Code und race-freie Prozessbaumisolation. Andere Plattformen sind ebenfalls nicht unterstützt. Unit-Test-Operationstabellen prüfen den Koordinatorvertrag, nicht native Verfügbarkeit.

---

<!-- i18n-section: scope -->

## False-Positive-Schranken und Umfang

- Kapazität ist exakt oder eine obere Schranke der wahren Objektgröße; UNSAFE spiegelt daher einen echten Überlauf. Reicht ohne exakte Größe die Regionsobergrenze nicht zum Sicherheitsbeweis, lautet das Ergebnis UNKNOWN.
- Eine längenbeschränkte Kopie wird vor dem Solver zurückgezogen und in `skipped` gezählt; exakte Kapazität kann SAFE beweisen, eine Obergrenze allein bleibt UNKNOWN.
- Katalogisierte Wide-Character- und Append-Kopien bleiben UNKNOWN, bis Elementbreite und vorhandene Ziellänge rekonstruiert sind. Out-Parameter-Allokatoren und bedingter `realloc`-Besitz bleiben ebenfalls UNKNOWN, wenn der Handle-Übergang nicht beweisbar ist.
- **Phase 1**: Diese Version bietet für alle drei Formate Senkenkatalog, Argumentvorfilter, Stack-/Global-Kopierüberlaufprüfung, Heap-Lebensdaueraudit, uninitialisierte lokale Stack-Lesezugriffe und Formatstringprüfungen. Jeder Testhost muss die eingecheckten PE-, ELF- und Mach-O-Fixtures für x86-64 und AArch64 ausführen.
- Ausführlichere PDB-Stacktypen und weitere Plattformallokatoren bleiben Erweiterungen; ohne genaue Zusammenfassung gilt UNKNOWN.
- Der aktuelle Slice deckt bekannte Einstiegspunkte, strukturelle interprozedurale Erreichbarkeit und monotone Angreifer-Parameterpropagation ab. Der separate experimentelle Adapter `lowir-concolic-v1` liefert jetzt replay-verifizierte, register-gesäte Branch-Flips auf der verpflichtenden nativen Format-/Architekturmatrix; er bleibt nicht erschöpfend und verändert keine Sicherheitsurteile. Das experimentelle `binary-sanitizer-v1` bietet auf Darwin nun Counted-Write-Guards nach dem Alles-oder-Ablehnen-Prinzip und authentisierte Veröffentlichung; sein Receipt authentisiert das während der Transaktion gehaltene Verzeichnisobjekt, nicht eine dauerhaft nachprüfbare Bindung des ursprünglichen Pfads. Das breitere `process-replay-v1` besitzt weiterhin nur eine fail-closed Phase-0-Grenze für Plan, Koordinator und Verfügbarkeit; kein Host führt derzeit natives Replay aus.
