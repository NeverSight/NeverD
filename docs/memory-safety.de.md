**Sprachen**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← Dokumentationsindex](README.de.md)

# Speicher-Audit und Hunt

NeverD analysiert ein geladenes Binärfile auf zwei Familien von Speichersicherheitsfehlern und berichtet sie als strukturiertes JSON. Beide Spuren laufen auf dem formatneutralen, gelifteten IR, daher sind **PE/COFF, ELF und Mach-O gleichrangige Erstklassziele** — ein Fund hängt nie an einem format-spezifischen Scanner oder einer Importtabelle.

| Spur | Befehl | Berichtet |
|------|--------|-----------|
| **Audit** | `neverd audit <binary>` | Heap-Lebensdauerfehler und uninitialisierte lokale Stack-Reads |
| **Hunt** | `neverd hunt <binary>` | Gefährliche Copy-Überläufe mit symbolischer Evidenz und Eingabekandidaten; `replayable=true` nur mit einem vollständigen `process-input-v1`-Plan |

Die Engine nutzt NeverDs eigene symbolische Ausführung und den Bitvektor-Solver für Zeugen und Erreichbarkeit. Kein externer Solver, keine VM, kein Container.

---

## Kerninvariante: geschlossen fehlschlagen

Eine nicht geliftete Operation, ein Aufruf, dessen Argumente die ABI-Passage nicht zurückgewinnen konnte, ein unaufgelöstes indirektes Ziel oder ein erschöpftes Budget ergeben **UNKNOWN**, niemals SAFE. Ein Zielpuffer, dessen Kapazität nicht zurückgewonnen werden kann, ist UNKNOWN. Striktes Lifting bleibt unverändert; die Sicherheitsschicht legt nur konservative Urteile darauf.

Aufrufeffekte folgen einer Closed-World-Semantik: Eine Zusammenfassung gilt nur, wenn ihre Vorbedingungen und alle relevanten Effekte bekannt sind. Ein unbekannter Effekt oder eine nur teilweise anwendbare Zusammenfassung bleibt UNKNOWN; die Lücke wird weder als wirkungslos noch als erfolgreicher Aufruf angenommen.

---

## Identitätsvertrag je Format

Beide Spuren brauchen die Lift-Pipeline (sie stellt Argumente pro Aufruf wieder her). Callee-Namen kommen aus derselben Identitätsansicht wie der Rest von NeverD. Die Reihenfolge der Debug-Entdeckung bleibt gleich.

| Format | Debug (nach Vorrang) | Import- / Thunk-Auflösung |
|--------|----------------------|---------------------------|
| **PE/COFF** | `--pdb`, Debug-Verzeichnis oder benachbartes `.pdb`, dann MSVC `/MAP` | IAT-Slots und `__imp_`-Thunks, ordinale Imports |
| **ELF** | DWARF im Image, getrenntes `*.debug`, dann GNU/LLD-MAP | PLT-Stubs auf den Importnamen aufgelöst |
| **Mach-O** | DWARF im Image, benachbartes `.dSYM`, dann ld64 `-map` | dyld-Bind / indirekte Symbolslots und Stub-Helfer |

`--pdb` / `--map` benennen eine maßgebliche Begleitdatei: Lesefehler ist ein Fehler, kein stiller Fallback. `--no-debug` liest auf jedem Format nur das Image.

PDB-Prozedursignaturen dienen dazu, wertliefernde Allokatoren von `void`-Freigabefunktionen zu unterscheiden. Die umfassende Wiederherstellung lokaler und Stack-Typen aus einem PDB bleibt begrenzt; lässt sich keine exakte Objektgröße bestimmen, fällt die Jagd auf das Rahmen- bzw. Allokationsmodell zurück und meldet UNKNOWN, statt eine Größe zu erfinden.

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

## Senken- und Quellenkatalog

Der Katalog ist eine konfigurierbare Tabelle, keine fest verdrahtete Menge. Jede **Senke** deklariert ihre Schwachstellenklasse, ihre Rolle (copy, format, alloc, free, realloc) und die relevanten Argumentslots (Ziel, Quelle, Länge, Kapazität). Eine JSON-Senke der Art copy oder format liefert zusätzlich einen ausführbaren Aufrufeffekt. Jede **Quelle** benennt einen Anbieter angreiferbeeinflusster Eingabe.

Die eingebauten Einträge stehen in [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) und [`SafetySources.def`](../include/neverd/safety/SafetySources.def); sie decken die übliche C-Laufzeit-Copy-Familie (`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), gehärtete `_chk`-Varianten (explizite Zielschranke), Allokation und Freigabe (`malloc`/`calloc`/`realloc`/`free`, Operator `new`/`delete`) sowie optionale Win32-Heap-APIs. Eingabequellen umfassen POSIX (`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, Programargumente) **und** Win32 (`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`). Ein PE-Hunt ist nicht auf POSIX-Eingaben beschränkt.

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

## Hunt: Urteile zu Copy-Überläufen

Für jede Copy-Senke ermittelt der Hunt die Zielkapazität — debug-deklarierte Arraygröße, dann Heap-Allokationsstelle bekannter Größe, dann eine solide Stack-Frame-Schranke — und klassifiziert das Argument, das die Schreiblänge bestimmt, per rückwärtigem SSA-Gang (Spill/Reload über Stackslots):

- **Konstante Länge** innerhalb einer exakten Kapazität ist SAFE. Ein konstanter Überlauf ist nur UNSAFE, wenn die Senke auf einem bestätigten Pfad erreichbar ist; andernfalls bleibt er UNKNOWN.
- **Gehärtete** `_chk`-Kopien tragen eine Laufzeit-Zielschranke. Eine Zurückweisung oder eine nachweislich passende Schranke ist SAFE; ein möglicher Schreibzugriff über das Objekt hinaus ist UNSAFE; eine nicht rekonstruierte oder nicht entscheidende Schranke ist UNKNOWN.
- **Beweisbar beschränkte** Länge (längenrückgebender Aufruf, Maske, Clamp) wird vor dem Solver mit Begründung zurückgezogen. SAFE gilt nur bei exakter Zielgröße; eine reine Regionsobergrenze bleibt UNKNOWN.
- **Angreiferbeeinflusste** Länge bei bekannter Kapazität: Bitvektor-Solver. Ist eine Länge größer als die Kapazität erfüllbar, ist das Urteil UNSAFE. Kandidaten sind nur mit einem vollständigen `process-input-v1`-Plan abspielbar: zunächst exakte literale Umgebungswerte und höchstens die vom ersten unterstützten `read(0)`-Familienaufruf zurückgegebenen Standardeingabe-Bytes. argv-, Datei-, Netzwerk-, benutzerdefinierte oder mehrdeutige Eingaben bleiben mit Begründung nicht abspielbar.
- Alles andere — unbekannte Länge oder unbekannte Kapazität — ist UNKNOWN.

Jede zurückgewonnene Kapazität ist eine **obere Schranke** der wahren Objektgröße, daher ist ein bewiesener Überlauf niemals ein False Positive.

### Formatierte Eingabe

Für `scanf`/`fscanf` und ihre versionierten Schreibweisen ordnet ein lesbares konstantes Format jede nicht unterdrückte Konvertierung ihrem tatsächlichen variadischen Ausgabeargument zu. Unbegrenzte `%s`/`%[`-Ausgaben markieren spätere Stringverwendungen als Taint; numerische und Zeichenausgaben markieren Werte als Taint, die aus dem geschriebenen Objekt geladen werden, nicht aber den Wert des Ausgabezeigers selbst. `sscanf` propagiert diese Effekte nur, wenn seine Eingabezeichenfolge bereits vom Angreifer beeinflusst ist. Begrenzte Textausgaben wie `%Ns`/`%N[` propagieren Taint zusammen mit einer `MaxBytes`-Ausdehnung einschließlich Terminator; Wide-Character-Varianten berechnen diese Byteausdehnung mit der plattformspezifischen `wchar_t`-Breite. Unterdrückte Konvertierungen, überzählige Argumente, positionsabhängige oder nicht unterstützte Formate und `%n` bleiben UNKNOWN, statt erraten zu werden.

---

## Audit: Urteile zur Heap-Lebensdauer

Für jede Allokation verfolgt das Audit den Handle im Kontrollflussgraphen, einschließlich Stack-Spill/Reload, und wendet eine Escape-Zusammenfassung an (zurückgegeben, über eine Nicht-Stack-Adresse gespeichert oder an einen opaken Callee übergeben):

- **Leak** — der Handle wird weder freigegeben noch darf er entkommen.
- **Double-Free** — eine zweite Freigabe ist nach einer ersten auf einem Pfad erreichbar.
- **Use-after-Free** — eine Dereferenzierung oder opake Nutzung ist nach einer Freigabe erreichbar.

Allokations- und Freigabe-**Wrapper** werden über funktionsweise Escape-Zusammenfassungen erkannt, sodass ein `malloc`/`free`-Forwarder den Fehler nicht verdeckt. Freigaben auf einander ausschließenden Zweigen gelten nicht als Double-Free.

Der Heap-Automat gibt zunächst eine Kandidaten-Ereignisfolge aus (Allokation, Freigabe, Verwendung oder Austritt per Rückgabe). Ein zweiter Durchlauf muss diese Folge auf einem symbolischen LowIR-Pfad nachspielen und dessen Pfadprädikat als erfüllbar nachweisen, bevor der Befund ein UNSAFE mit HOHER Konfidenz wird. Fehlendes LowIR, undurchsichtige Operationen, Aufrufe ohne Zusammenfassung, Unsicherheit des Solvers und Explorationsgrenzen stufen den Kandidaten auf UNKNOWN herab. Konservativer May-Alias-Speicher-Havoc wird getrennt verfolgt, damit gewöhnliche Schreibzugriffe auf den Stapelrahmen einen ansonsten exakten Erreichbarkeitsnachweis nicht entwerten.

---

## Budgets, Ausgabe und Bindings

Hunt-Exploration und Solver sind begrenzt (`--max-paths`, `--max-steps`, `--max-loop`, `--solver-conflicts`); Budgeterschöpfung ergibt UNKNOWN. Beide Befehle drucken JSON und respektieren `-o`. Der Exit-Code ist `0` für SAFE, `2` für UNSAFE und `1` für UNKNOWN oder einen Fehler.

Dieselben Analysen stehen über die C-API (`neverd_session_audit_json` / `neverd_session_hunt_json` mit versioniertem `neverd_safety_options`) und das Python-SDK (`Session.audit()` / `Session.hunt()`) zur Verfügung.

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" }, "candidate_values": [{ "name": "copy_length", "value": "17" }, { "name": "argv[1]", "value": "16 bytes" }], "replayable": false, "replay": { "adapter": "process-input-v1", "reason": "argv input is not supported by process-input-v1" }, "symbolic_model": [{ "id": 0, "name": "copy_len", "width": 64, "value_hex": "0x11", "origin": "input" }] }
}
```

`replayable` ist abgeleitete Evidenz, kein eigenständiges Versprechen: Es ist nur wahr, wenn `replay` einen vollständigen Eingabeplan für den Adapter `process-input-v1` enthält. Der Plan hält exakte Umgebungsbytes, gegebenenfalls die Bytefolge des ersten unterstützten `read(0)`-Familienaufrufs sowie Bindungen von Solver-Zuweisungs-IDs an diese Eingaben fest; andernfalls erklärt `replay.reason` den Grund. Die Felder sind additiv; die oberste `schema_version` bleibt `1`.

---

## False-Positive-Schranken und Umfang

- Kapazität ist exakt oder eine obere Schranke der wahren Objektgröße; UNSAFE spiegelt daher einen echten Überlauf. Reicht ohne exakte Größe die Regionsobergrenze nicht zum Sicherheitsbeweis, lautet das Ergebnis UNKNOWN.
- Eine längenbeschränkte Kopie wird vor dem Solver zurückgezogen und in `skipped` gezählt; exakte Kapazität kann SAFE beweisen, eine Obergrenze allein bleibt UNKNOWN.
- Katalogisierte Wide-Character- und Append-Kopien bleiben UNKNOWN, bis Elementbreite und vorhandene Ziellänge rekonstruiert sind. Out-Parameter-Allokatoren und bedingter `realloc`-Besitz bleiben ebenfalls UNKNOWN, wenn der Handle-Übergang nicht beweisbar ist.
- **P0** (diese Version, alle drei Formate): Senkenkatalog, Argument-Vorfilter, Copy-Überlauf-Hunt, Heap-Lebensdauer-Audit. Jeder Testhost führt sechs eingecheckte PE-, ELF- und Mach-O-Fixtures für x86-64 und AArch64 aus.
- **P1**: Stack-/Global-Überlauf, uninitialisierte lokale Reads und Formatstring-Prüfungen sind verfügbar; reichere PDB-Stacktypen und weitere Plattform-Allokatoren bleiben inkrementelle Abdeckung, fehlende exakte Zusammenfassungen bleiben UNKNOWN.
- **P2**: per Patch eingefügte Laufzeitprüfungen, interprozedurale Angreifererreichbarkeit.
