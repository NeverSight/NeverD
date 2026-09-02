**Sprachen**: [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← Dokumentationsindex](README.de.md)

# Python-Plugins

NeverD kann eine Python-Datei als vollwertiges Plugin laden. Python-Plugins verwenden dieselben Metadaten, denselben Lebenszyklus, dieselbe Reihenfolge, dieselben Regeln für doppelte Namen, denselben Event-Stream und dieselbe Session-C-ABI wie native Plugins. Das unterstützte Entwicklungspaket ist `neverd-plugin`; importieren Sie die private Brücke `_neverd_plugin` nicht direkt.

## Build- und Laufzeitanforderungen

`NEVERD_ENABLE_PYTHON_PLUGINS` ist standardmäßig `ON`. Ein aktivierter Build benötigt einen von CMake auffindbaren CPython-Interpreter ab Version 3.10 sowie dessen Entwicklungsbibliothek zum Einbetten:

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

Mit `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` entsteht eine rein native `libneverd` ohne CPython-Link-Abhängigkeit. Ein Build mit Python legt das passende Paket und die Beispiele unter `build/bin/sdk/python/` ab; dieses Verzeichnis lässt sich auch direkt mit `python3 -m pip install build/bin/sdk/python` installieren.

## Ein Plugin schreiben

Ein Modul deklariert genau eine dekorierte Klasse:

```python
from neverd_plugin import Event, Plugin, PluginType, Session


@Plugin(
    name="Analysis Report",
    version="1.0.0",
    author="Your team",
    description="Reports basic information about the loaded binary",
    type=PluginType.PROCESSOR,
)
class AnalysisReport:
    def on_init(self, session: Session) -> int | None:
        print(session.architecture)
        return None

    def on_run(self, session: Session, arg: int) -> int | None:
        print(session.file_path, session.function_count)
        return 0

    def on_event(self, event: Event) -> int | None:
        print(event.type.name)
        return None

    def on_term(self) -> None:
        pass
```

Alle Hooks sind optional. `None` bedeutet Erfolg; ein ganzzahliges Ergebnis muss in einen C-`int` passen. Metadatenversionen verwenden striktes SemVer. Namen müssen nicht leere UTF-8-Zeichenketten sein; Metadaten mit eingebettetem NUL werden vollständig abgelehnt.

Beispiele im Repository sind [`minimal.py`](../pluginsdk/python/examples/minimal.py), [`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py) und [`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py) für die beweisgebundenen Optimierungs-APIs.

## Plugins laden und prüfen

Die C-API kann eine bestimmte `.py`-Datei deterministisch laden oder ein Verzeichnis durchsuchen:

```c
if (!neverd_plugins_load_file(session, "plugins/report.py")) {
  const char *message = neverd_last_error(session);
  /* log message */
  neverd_free_string(message);
}

neverd_plugins_init(session);
int result = neverd_plugins_run(session, "Analysis Report", 0);
neverd_plugins_term(session);
```

`neverd_plugins_list_json` kennzeichnet jeden Eintrag mit `"kind":"python"` oder `"kind":"native"`. Die Verzeichniserkennung wird nach kanonischem Pfad sortiert und akzeptiert native Bibliotheken und Python-Dateien im selben Verzeichnis. Doppelte kanonische Pfade und doppelte Plugin-Namen sind Fehler.

## Session- und Event-API

`Session` prüft ihre Host-Fähigkeit vor jedem C-Aufruf erneut. Die typisierte Oberfläche umfasst Datei-, Architektur- und Formatmetadaten, Bitbreite und Tabellenzähler, Funktionsansichten, Laden und Analyse, Byte-Lesezugriffe, Disassemblierung, Dekompilierung und häufige Abfragen. Für fortgeschrittene Operationen stellt `session.raw` jede Deklaration aus `neverd_plugin.abi` bereit:

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

### Begrenzte symbolische Pfaderkundung

Für native LowIR-Funktionen liefert `session.symbolic_explore` typisierte Pfadergebnisse, Basic-Block-Verläufe, den Ressourcenverbrauch und optional Pfadprädikate:

```python
result = session.symbolic_explore(
    0x401000,
    max_paths=64,
    max_steps=1 << 16,
    max_block_visits=3,
    include_expressions=True,
)
if not result.exact:
    print(result.unmodelled_ops)
for path in result.paths:
    print(path.outcome, path.blocks, path.predicate)
```

`complete` ist false, wenn eine Grenze für Pfade, Schritte, Schleifenbesuche oder nicht aufgelöste Verzweigungen die Erkundung beendet. `exact` setzt zusätzlich voraus, dass keine Operation konservativ durch einen unbekannten Zustand ersetzt wurde; nicht unterstützte LowIR-Operationen, Aufrufe ohne Zusammenfassung und Speicherzugriffe über nicht aufgelöste Adressen werden in `unmodelled_ops` gezählt. EVM- und SBF-Sessions bieten keine native LowIR-Erkundung.

### Verifizierte konkolische LowIR-Zweigumkehrungen

`session.lowir_concolic` verfolgt einen nativen LowIR-Pfad aus expliziten Bytebereichen des Eingaberegisters und liefert nur Solver-Kandidaten, die eine neue Ausführung am selben Kontrollentscheidungsvorkommen bestätigt:

```python
from neverd_plugin import ConcolicRegisterSeed

report = session.lowir_concolic(
    0x401000,
    [ConcolicRegisterSeed(offset=56, bytes=4, value=0)],
)
for flip in report.flips:
    if flip.candidate_id is not None:
        print(report.candidates[flip.candidate_id].seed)
```

Der Register-Offset ist ein Byte-Offset in NeverDs Registerdatei, kein nativer Zeiger oder eine Registernummer. Der Bericht ist stets nicht vollständig; UNSAT, Solver-Grenzen sowie Projektions- oder Replay-Ablehnungen bleiben typisierte Flip-Ergebnisse statt Ausnahmen.

### Speicher-Audit und Hunt

`session.audit()` und `session.hunt()` liefern geparste JSON-Berichte (dasselbe Schema wie die CLI). Sie brauchen eine geliftete native Session:

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

EVM- und SBF-Sessions weisen diese Aufrufe zurück.

Die sechs unveränderlichen Event-Varianten heißen `BINARY_LOADED`, `BINARY_CLOSING`, `FUNCTION_SELECTED`, `ADDRESS_CHANGED`, `ANALYSIS_DONE` und `PATCH_APPLIED`. Payload-Zeichenketten werden während des Callbacks kopiert; für die jeweilige Variante irrelevante Felder sind `None`.

Speichern Sie niemals eine `Session`, um sie nach der Terminierung weiterzuverwenden. Die native Capsule wird vor Beginn von `on_term` und vor einer möglichen Freigabe der nativen Session ungültig gemacht. Ein späterer Aufruf schlägt mit `RuntimeError` fehl, statt veralteten Speicher zu dereferenzieren.

### Strikte Veröffentlichung des Binär-Sanitizers

`session.sanitize()` führt die experimentelle Alles-oder-Ablehnen-Transaktion `binary-sanitizer-v1` aus und gibt erst nach Prüfung eines vollständigen, konsistenten authentisierten Receipts ein eingefrorenes `SanitizeResult` zurück. Nicht-Darwin-Hosts lehnen vor Lifting, Guard-Erzeugung, Kandidatenerstellung oder Namespace-Änderung ab. `PUBLISH_INDETERMINATE` und `PUBLISHED_INCOMPLETE` sind Fehler und bedeuten, dass das Ziel bereits existieren kann und vor Verwendung oder Wiederholung geprüft werden muss.

Ein vollständiger Darwin-Receipt authentisiert nur das während der Transaktion gehaltene Zielverzeichnisobjekt. Da dieses nach dem Öffnen umbenannt werden kann, garantiert der Receipt weder während noch nach der Transaktion die Bindung des ursprünglichen Pfads und ist keine dauerhafte, unabhängig nachprüfbare Pfadbindung. Späteres erneutes Öffnen erfordert einen externen Anker und erneute Authentisierung. Python bietet derzeit kein natives Whole-Process-Replay; `NativeProcessReplayAdapter` ist nur eine fail-closed Phase-0-C++-Verfügbarkeits-/Factory-Grenze, auf der alle Hosts alle Fähigkeiten false und keine Operationstabelle melden.

### Beweisgesicherte Synthese und LLVM-Optimierung

`synthesize_expression` ist von der aus ABI-Gründen erhaltenen, reinen
MBA-Funktion `simplify_expression` getrennt. Eine Umschreibung wird nur bei
`ProofStatus.EQUIVALENT` übernommen. Gegenbeispiele, unvollständige Beweise und
erschöpfte Suchbudgets lassen den ursprünglichen Ausdruck unverändert und
melden Ergebnis sowie Such- und Beweisaufwand getrennt.
`ProofStatus.INVALID` kennzeichnet eine fehlerhafte Beweisfrage und bleibt vom
budgetbedingten `ProofStatus.UNKNOWN` getrennt; beide lehnen die Umschreibung
fehlersicher ab.

`optimize_llvm_ir` kombiniert NeverDs semantischen Fixpunkt und die gewählte
LLVM-Standardpipeline auf einer Transaktionskopie und liefert nur das
verifizierte, übernommene Modul:

```python
from neverd_plugin import (
    LLVMOptimizationLevel,
    OptimizationMode,
    ProofStatus,
    optimize_llvm_ir,
    synthesize_expression,
)

rewrite = synthesize_expression(
    "(x >> 4) + ((x >> 2) >> 2)", exhaustive=True
)
if rewrite.changed:
    assert rewrite.proof_status is ProofStatus.EQUIVALENT

module = optimize_llvm_ir(
    llvm_ir,
    mode=OptimizationMode.DEEP,
    llvm_level=LLVMOptimizationLevel.O2,
    enable_synthesis=True,
    exhaustive=True,
)
print(module.output_ir, module.semantic_rewrites, module.proof_queries)
```

Produktionsaufrufe können MBA-Arbeit und -Stelligkeit, Synthesesuche und
SAT-Arbeit sowie LLVM-Konvergenz getrennt begrenzen. Bei
`simplify_expression` wählt `exhaustive=True` die unbegrenzte MBA-Richtlinie
für Stelligkeit und Arbeit und entfernt die Verschachtelungs- und
Bitbreitenlimits des nativen Parsers. Bei `synthesize_expression` entfernt es
Parser-, Sucharbeits- und SAT-Limits, während die vom Aufrufer beschriebene
Grammatik erhalten bleibt; bei `optimize_llvm_ir` entfernt es Konvergenz-,
Sucharbeits- und SAT-Limits. Python fügt keine weitere Ausdrucksgrenze hinzu;
Speichersicherheits- und IR-Darstellungsgrenzen gelten weiterhin. Die
C-Einstiegspunkte heißen `neverd_simplify_expr`, `neverd_synthesize_expr` und
`neverd_optimize_llvm_ir` und besitzen typisierte Freigabefunktionen sowie
versionierte JSON-Adapter.

## Fehler, Isolation und Vertrauen

Python-Ausnahmen werden niemals durch C++ hindurch abgewickelt. NeverD erfasst den vollständig formatierten Traceback und stellt ihn über `neverd_last_error` bereit. Jeder kanonische Plugin-Pfad wird unter einem eindeutigen Modulnamen geladen; bei der Terminierung wird das Modul entfernt, sodass ein späteres Neuladen frischen Modul- und Klassenzustand erhält. CPython wird einmal initialisiert, die anfängliche GIL wird freigegeben, und Callbacks erwerben die GIL auf jedem Host-Thread. NeverD finalisiert keinen Interpreter, den es möglicherweise mit einer anderen Komponente teilt.

Plugins führen beliebigen Python-Code im NeverD-Prozess aus und können die vollständige C-API aufrufen. Laden Sie nur vertrauenswürdige Dateien. Dies ist eine Erweiterungsgrenze, keine Sandbox.

## Entwicklung, Tests und Pakete

Für Editor- und Typprüfungsunterstützung installieren Sie das reine Python-Paket oder nehmen den Quellbaum in `PYTHONPATH` auf:

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

Das Audit verlangt exakte Übereinstimmung zwischen jeder exportierten C-Deklaration und ihrer `ctypes`-Signatur sowie Besitzregel. Es prüft außerdem Ausgabesprachenwerte, CMake-/Paketversionen, CI-Feature-Flags, festgeschriebene Action-Versionen, den Artefaktfluss und die PyPI-OIDC-Richtlinie. Die Tests des nativen Adapters heißen `NeverDPluginRuntimeTests`; die eingebetteten Python-Tests heißen `NeverDPythonRuntimeTests` und `NeverDPythonPluginTests`.

Der Workflow `Python Plugin SDK` erstellt ein Wheel und eine Quelldistribution, installiert beide in sauberen Umgebungen und lädt die geprüften Artefakte hoch. Die Veröffentlichung erfolgt nur für ein veröffentlichtes GitHub Release über das genehmigungspflichtige Environment `pypi` und Trusted Publishing; ein langlebiges PyPI-Token wird nicht verwendet.
