**Lingue**: [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← Indice della documentazione](README.it.md)

# Plugin Python

NeverD può caricare un file Python come plugin di prima classe. I plugin Python condividono con quelli nativi gli stessi metadati, ciclo di vita, ordinamento, regole sui nomi duplicati, flusso di eventi e ABI C della sessione. Il pacchetto supportato per lo sviluppo è `neverd-plugin`; non importare direttamente il bridge privato `_neverd_plugin`.

## Requisiti di build e runtime

`NEVERD_ENABLE_PYTHON_PLUGINS` è `ON` per impostazione predefinita. Una build abilitata richiede un interprete CPython 3.10 o successivo e la relativa libreria di sviluppo per l’embedding, rilevabili da CMake:

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

Impostare `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` per ottenere una `libneverd` esclusivamente nativa, senza dipendenza di link da CPython. Una build con Python copia il pacchetto corrispondente e gli esempi in `build/bin/sdk/python/`; la directory è installabile direttamente anche con `python3 -m pip install build/bin/sdk/python`.

## Scrivere un plugin

Ogni modulo dichiara esattamente una classe decorata:

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

Tutti gli hook sono facoltativi. `None` indica il successo; un risultato intero deve rientrare in un `int` C. Le versioni dei metadati usano SemVer rigoroso. I nomi devono essere stringhe UTF-8 non vuote e qualsiasi metadato con un NUL incorporato viene rifiutato.

Gli esempi nel repository sono [`minimal.py`](../pluginsdk/python/examples/minimal.py), [`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py) e [`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py), che mostra le API di ottimizzazione vincolate alla prova.

## Caricare e ispezionare i plugin

L’API C può caricare in modo deterministico uno specifico file `.py` oppure esaminare una directory:

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

`neverd_plugins_list_json` identifica ogni voce con `"kind":"python"` o `"kind":"native"`. La ricerca nelle directory è ordinata per percorso canonico e accetta librerie native e file Python nella stessa directory. Percorsi canonici duplicati e nomi di plugin duplicati sono errori.

## API di sessione ed eventi

`Session` riconvalida le capacità dell’host prima di ogni chiamata C. La sua interfaccia tipizzata include metadati di file, architettura e formato, bitness e conteggi delle tabelle, viste delle funzioni, caricamento e analisi, lettura di byte, disassemblaggio, decompilazione e query comuni. Per le operazioni avanzate, `session.raw` espone ogni dichiarazione in `neverd_plugin.abi`:

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

### Esplorazione simbolica limitata dei percorsi

Per le funzioni LowIR native, `session.symbolic_explore` restituisce risultati di percorso tipizzati, tracce dei blocchi di base, utilizzo delle risorse e predicati di percorso facoltativi:

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

`complete` è false quando un limite di percorsi, passi, visite del ciclo o rami non risolti interrompe l’esplorazione. `exact` richiede inoltre che nessuna operazione sia stata sostituita in modo conservativo da uno stato sconosciuto; le operazioni LowIR non supportate, le chiamate senza riepilogo e le scritture tramite indirizzi non risolti vengono conteggiate in `unmodelled_ops`. Le sessioni EVM e SBF non espongono l’esplorazione LowIR nativa.

### Audit e hunt di sicurezza della memoria

`session.audit()` e `session.hunt()` restituiscono report JSON analizzati (lo stesso schema del CLI). Richiedono una sessione nativa sollevata:

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

Le sessioni EVM e SBF rifiutano queste chiamate.

Le sei varianti di evento immutabili sono `BINARY_LOADED`, `BINARY_CLOSING`, `FUNCTION_SELECTED`, `ADDRESS_CHANGED`, `ANALYSIS_DONE` e `PATCH_APPLIED`. Le stringhe del payload vengono copiate durante il callback; i campi non pertinenti alla variante sono `None`.

Non conservare mai una `Session` per usarla dopo la terminazione. La capsule nativa viene invalidata prima dell’inizio di `on_term` e prima che la sessione nativa possa essere liberata. Una chiamata successiva fallisce con `RuntimeError` invece di dereferenziare memoria obsoleta.

### Sintesi vincolata dalla prova e ottimizzazione LLVM

`synthesize_expression` è separata da `simplify_expression`, mantenuta per la
compatibilità ABI e limitata alle MBA. Una riscrittura viene confermata solo se
il solver restituisce `ProofStatus.EQUIVALENT`. Controesempi, prove incomplete e
budget di ricerca esauriti conservano l’espressione originale e riportano
separatamente esito e lavoro di ricerca e prova.
`ProofStatus.INVALID` identifica una richiesta di prova malformata e resta
distinto da `ProofStatus.UNKNOWN`, dovuto al budget; entrambi rifiutano la
riscrittura in modo fail-closed.

`optimize_llvm_ir` combina il punto fisso semantico di NeverD e la pipeline LLVM
standard selezionata su una copia transazionale, restituendo solo il modulo
verificato e confermato:

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

I client di produzione possono limitare separatamente lavoro e arità MBA,
ricerca di sintesi e lavoro SAT, e convergenza LLVM. Per
`simplify_expression`, `exhaustive=True` seleziona la politica MBA senza limiti
di arità e lavoro e rimuove i limiti di annidamento e larghezza del parser
nativo. Per `synthesize_expression`, rimuove i limiti del parser, del lavoro di
ricerca e di SAT mantenendo la grammatica specificata dal chiamante; per
`optimize_llvm_ir`, rimuove i limiti di convergenza, ricerca e SAT. Python non
aggiunge ulteriori limiti alle espressioni; restano validi i limiti di sicurezza
della memoria e di rappresentazione IR. Gli ingressi C sono
`neverd_simplify_expr`, `neverd_synthesize_expr` e `neverd_optimize_llvm_ir`,
con funzioni di rilascio tipizzate e adattatori JSON versionati.

## Errori, isolamento e attendibilità

Le eccezioni Python non attraversano mai C++ durante l’unwinding. NeverD acquisisce il traceback completo e formattato e lo espone tramite `neverd_last_error`. Ogni percorso canonico di plugin viene caricato con un nome di modulo univoco; alla terminazione il modulo viene rimosso e un caricamento successivo ottiene un nuovo stato di modulo e classe. CPython viene inizializzato una sola volta, il GIL di bootstrap viene rilasciato e i callback acquisiscono il GIL su qualsiasi thread host. NeverD non finalizza mai un interprete che potrebbe condividere con un altro componente.

I plugin eseguono Python arbitrario nel processo NeverD e possono chiamare l’intera API C. Caricare solo file attendibili. Questo è un confine di estensione, non una sandbox.

## Sviluppo, test e pacchetti

Per il supporto dell’editor e del type checker, installare il pacchetto Python puro oppure inserire l’albero dei sorgenti in `PYTHONPATH`:

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

L’audit richiede la corrispondenza esatta fra ogni dichiarazione C esportata e la relativa firma `ctypes` e regola di proprietà. Controlla inoltre i valori del linguaggio di output, le versioni CMake e del pacchetto, i feature flag della CI, le versioni fissate delle Action, il flusso degli artefatti e la policy OIDC di PyPI. I test dell’adattatore nativo sono `NeverDPluginRuntimeTests`; quelli di Python integrato sono `NeverDPythonRuntimeTests` e `NeverDPythonPluginTests`.

Il workflow `Python Plugin SDK` crea un wheel e una distribuzione sorgente, installa entrambi in ambienti puliti e carica gli artefatti verificati. La pubblicazione avviene solo per una GitHub Release pubblicata, tramite l’environment `pypi` protetto da approvazione e Trusted Publishing; non viene usato alcun token PyPI a lunga durata.
