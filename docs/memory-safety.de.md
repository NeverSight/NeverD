**Sprachen**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← Dokumentationsindex](README.de.md)

# Speicher-Audit und Hunt

NeverD analysiert ein geladenes Binärfile auf zwei Familien von Speichersicherheitsfehlern und berichtet sie als strukturiertes JSON. Beide Spuren laufen auf dem formatneutralen, gelifteten IR, daher sind **PE/COFF, ELF und Mach-O gleichrangige Erstklassziele** — ein Fund hängt nie an einem format-spezifischen Scanner oder einer Importtabelle.

| Spur | Befehl | Berichtet |
|------|--------|-----------|
| **Audit** | `neverd audit <binary>` | Heap-Lebensdauerfehler: Leak, Double-Free, Use-after-Free |
| **Hunt** | `neverd hunt <binary>` | Gefährliche Copy-Überläufe mit symbolischer Evidenz und Eingabekandidaten; `replayable=false` bis ein Prozess-Eingabeadapter sie auf tatsächliche Bytes abbildet |

Die Engine nutzt NeverDs eigene symbolische Ausführung und den Bitvektor-Solver für Zeugen und Erreichbarkeit. Kein externer Solver, keine VM, kein Container.

---

## Kerninvariante: geschlossen fehlschlagen

Eine nicht geliftete Operation, ein Aufruf, dessen Argumente die ABI-Passage nicht zurückgewinnen konnte, ein unaufgelöstes indirektes Ziel oder ein erschöpftes Budget ergeben **UNKNOWN**, niemals SAFE. Ein Zielpuffer, dessen Kapazität nicht zurückgewonnen werden kann, ist UNKNOWN. Striktes Lifting bleibt unverändert; die Sicherheitsschicht legt nur konservative Urteile darauf.

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

Der Katalog ist eine konfigurierbare Tabelle, keine fest verdrahtete Menge. Jede **Senke** deklariert ihre Schwachstellenklasse, ihre Rolle (copy, format, alloc, free, realloc) und die relevanten Argumentslots (Ziel, Quelle, Länge, Kapazität). Jede **Quelle** benennt einen Anbieter angreiferbeeinflusster Eingabe.

Die eingebauten Einträge stehen in [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) und [`SafetySources.def`](../include/neverd/safety/SafetySources.def); sie decken die übliche C-Laufzeit-Copy-Familie (`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), gehärtete `_chk`-Varianten (explizite Zielschranke), Allokation und Freigabe (`malloc`/`calloc`/`realloc`/`free`, Operator `new`/`delete`) sowie optionale Win32-Heap-APIs. Eingabequellen umfassen POSIX (`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, Programargumente) **und** Win32 (`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`). Ein PE-Hunt ist nicht auf POSIX-Eingaben beschränkt.

Format-spezifische Schreibweisen fallen auf einen Eintrag: führende Unterstriche werden entfernt (`_malloc`, `___strcpy_chk`), gemangled `new`/`delete` über Aliase.

Katalog per Spezifikationsdatei erweitern oder überschreiben:

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 }
] }
```

---

## Hunt: Urteile zu Copy-Überläufen

Für jede Copy-Senke ermittelt der Hunt die Zielkapazität — debug-deklarierte Arraygröße, dann Heap-Allokationsstelle bekannter Größe, dann eine solide Stack-Frame-Schranke — und klassifiziert das Argument, das die Schreiblänge bestimmt, per rückwärtigem SSA-Gang (Spill/Reload über Stackslots):

- **Konstante Länge** innerhalb einer exakten Kapazität ist SAFE. Ein konstanter Überlauf ist nur UNSAFE, wenn die Senke auf einem bestätigten Pfad erreichbar ist; andernfalls bleibt er UNKNOWN.
- **Gehärtete** `_chk`-Kopien tragen eine Laufzeit-Zielschranke. Eine Zurückweisung oder eine nachweislich passende Schranke ist SAFE; ein möglicher Schreibzugriff über das Objekt hinaus ist UNSAFE; eine nicht rekonstruierte oder nicht entscheidende Schranke ist UNKNOWN.
- **Beweisbar beschränkte** Länge (längenrückgebender Aufruf, Maske, Clamp) wird vor dem Solver mit Begründung zurückgezogen. SAFE gilt nur bei exakter Zielgröße; eine reine Regionsobergrenze bleibt UNKNOWN.
- **Angreiferbeeinflusste** Länge bei bekannter Kapazität: Bitvektor-Solver. Ist eine Länge größer als die Kapazität erfüllbar, ist das Urteil UNSAFE; das Solver-Modell wird als symbolische Evidenz mit nicht abspielbaren Kandidaten (`replayable=false`) gemeldet, bis ein Prozess-Eingabeadapter verfügbar ist.
- Alles andere — unbekannte Länge oder unbekannte Kapazität — ist UNKNOWN.

Jede zurückgewonnene Kapazität ist eine **obere Schranke** der wahren Objektgröße, daher ist ein bewiesener Überlauf niemals ein False Positive.

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" }, "candidate_values": [{ "name": "copy_length", "value": "17" }, { "name": "argv[1]", "value": "16 bytes" }], "replayable": false, "symbolic_model": [{ "id": 0, "name": "copy_len", "width": 64, "value_hex": "0x11", "origin": "input" }] }
}
```

---

## False-Positive-Schranken und Umfang

- Kapazität ist exakt oder eine obere Schranke der wahren Objektgröße; UNSAFE spiegelt daher einen echten Überlauf. Reicht ohne exakte Größe die Regionsobergrenze nicht zum Sicherheitsbeweis, lautet das Ergebnis UNKNOWN.
- Eine längenbeschränkte Kopie wird vor dem Solver zurückgezogen und in `skipped` gezählt; exakte Kapazität kann SAFE beweisen, eine Obergrenze allein bleibt UNKNOWN.
- Katalogisierte Wide-Character- und Append-Kopien bleiben UNKNOWN, bis Elementbreite und vorhandene Ziellänge rekonstruiert sind. Out-Parameter-Allokatoren und bedingter `realloc`-Besitz bleiben ebenfalls UNKNOWN, wenn der Handle-Übergang nicht beweisbar ist.
- **P0** (diese Version, alle drei Formate): Senkenkatalog, Argument-Vorfilter, Copy-Überlauf-Hunt, Heap-Lebensdauer-Audit. Jeder Testhost führt sechs eingecheckte PE-, ELF- und Mach-O-Fixtures für x86-64 und AArch64 aus.
- **P1**: Stack-/Global-Überlauf, uninitialisierte Reads, Formatstrings, reichere PDB-Stacktypen, weitere Plattform-Allokatoren.
- **P2**: per Patch eingefügte Laufzeitprüfungen, interprozedurale Angreifererreichbarkeit.
