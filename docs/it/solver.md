**Lingue**: [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](../es/solver.md) | [Italiano](solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← Indice della documentazione](README.md)

# Backend di prova bitvector

Per impostazione predefinita NeverD usa il solver bitvector integrato. La derivazione MBA esatta resta indipendente da un solver generale. La sintesi di espressioni accetta un candidato solo dopo una prova di equivalenza; controesempi o query inconclusive mantengono l’espressione originale.

Il circuito di moltiplicazione integrato rimuove i bit bassi noti come zero dai due fattori, costruisce il prodotto rimanente e ripristina la parola di uscita completa. Anche l’adattatore Z3 opzionale codifica i prodotti con coefficiente costante pari alla larghezza esatta che osservano. Entrambi mantengono le query di uguaglianza a larghezza completa, i modelli di ingresso indipendenti e i limiti di risorse esistenti. L’ottimizzazione del sorgente in CLI e GUI condivide il circuito integrato senza un’opzione aggiuntiva.

<!-- i18n-section: builtin-comparisons -->

## Circuiti di confronto integrati

I confronti più larghi di otto bit confrontano prima le metà alte e usano quelle basse quando le prime sono uguali. I piccoli blocchi usano i riporti della sottrazione. Il codificatore condiviso può riutilizzare le porte del prefisso alto quando un aggiornamento parziale di registro cambia solo i bit bassi. I confronti con segno continuano a invertire entrambi i bit di segno. La semantica delle espressioni e i limiti delle risorse restano invariati; l’esaurimento restituisce ancora `Unknown`.

`BitBlaster.WidePredicatesAgreeWithTheEvaluator` confronta i predicati con e senza segno con il valutatore delle espressioni a larghezze selezionate da 8 a 256 bit, comprese larghezze dispari, valori adiacenti ai confini e bit significativi oltre i 64 bit. Esclude anche risultati errati. I test dei contatori parziali verificano query complete, modelli di controesempio ed esaurimento del budget di porte. Le regressioni native con confronto memorizzato provano entrambi gli ordini di confronto e aggiornamento osservando tutti i registri e flag, e rifiutano un corpo originale del ciclo modificato.

<!-- i18n-section: pristine-encoding -->

## Copie della codifica prima della ricerca

`BitVectorSolver::cloneEncoding()` copia una codifica completa prima di qualsiasi tentativo di ricerca SAT. Restituisce null dopo una ricerca o un errore di codifica. Ogni copia possiede clausole mutabili, propagazione alla radice, porte e corrispondenze dei bit indipendenti; conserva ordine delle variabili, conteggio delle porte e impostazioni del risolutore. Il contesto deve sopravvivere a entrambi i risolutori; quello originale può essere modificato o distrutto indipendentemente.

Il motore SAT conserva quattro elementi di sorveglianza in ogni lista di letterale; le liste più lunghe crescono dinamicamente. Questo evita allocazioni separate delle liste corte durante costruzione, copia prima della ricerca e distruzione. Ordine di propagazione, clausole, proprietà indipendente e tutti i limiti di lavoro restano invariati.

<!-- i18n-section: context-finite-proofs -->

## Prove completate nello stesso contesto

Il verificatore nativo di indipendenza lega la cache dei domini finiti al contesto simbolico effettivo, al quale si aggiungono soltanto nodi. Le chiavi compatte conservano il predicato esatto, le proiezioni ordinate e il limite dei valori. Si riutilizzano solo domini numerici completi o non unicità dimostrata; ogni mancata corrispondenza richiede ancora l’enumerazione completa e la prova finale di esclusione. Il contesto e il significato dei nodi esistenti devono restare stabili per tutta la vita della cache. I token preparati non passano a un altro proprietario né a un sostituto allo stesso indirizzo. Resta il limite di memoria in parole. Gli altri utenti mantengono chiavi strutturali e riutilizzo tramite rinomina delle variabili.

<!-- i18n-section: z3-build -->

## Build Z3 facoltativa

Attivando Z3, CMake scarica tramite `FetchContent` la revisione sorgente fissata 4.13.3 e crea una libreria statica con NeverD; non serve installare Z3 nel sistema:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
python scripts/build.py build-release --parallel 4 --target neverd \
  NeverDSolverTests NeverDSymbolicTests neverd-solver-bench
```

Il default è `NEVERD_Z3_PROVIDER=FETCH`. La prima configurazione scarica i sorgenti; le build successive riusano `_deps`. Non vengono buildati CLI, test, esempi, documentazione né binding di Z3. I generatori Python usano l’interprete già richiesto da NeverD. Per una libreria installata scegliere `-DNEVERD_Z3_PROVIDER=SYSTEM` e, facoltativamente, `-DZ3_ROOT=/path/to/prefix`; se mancano i file di sviluppo fallisce senza cambiare provider in silenzio. Per build offline fornire una copia locale con `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3`.

Con `NEVERD_ENABLE_Z3=OFF` (default), NeverD non scarica, cerca o linka Z3. Una richiesta runtime esplicita di un backend non disponibile fallisce senza fallback.

<!-- i18n-section: synthesis -->

## Sintesi di espressioni con prova

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

`--solver=builtin` è il default. La selezione richiede `--synthesize`; il semplificatore MBA normale non invoca Z3. Il timeout Z3 si applica a ogni verifica, esclusa la traduzione dell’espressione. La cancellazione è cooperativa, non un limite wall-clock rigido. Zero seleziona 1000 ms; `--exhaustive` rimuove il limite. Limiti SAT di conflitti, propagazione e visite ai watch riguardano solo il backend integrato e sono rifiutati con Z3. I controlli Z3 incrementano il contatore delle query di prova; i contatori SAT integrati restano a zero.

La C API aggiunge `solver_backend` e `solver_timeout_ms` a `neverd_synthesize_options`. I reader limitati per dimensione mantengono il backend integrato per i vecchi client. `neverd_solver_backend_available()` indica la capacità della build; Python espone la scelta tramite `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)`. Attualmente vale solo per la sintesi di espressioni. Esecuzione concolic, analisi di sicurezza e default dell’ottimizzazione IR mantengono le policy esistenti. Gli utenti interni possono fornire il verifier Z3 attraverso il proof callback del semplificatore semantico.

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

<!-- i18n-section: checks -->

## Controlli indipendenti ed esportazione query

Con Z3 abilitato, `NeverDSolverTests` include valutazione indipendente delle espressioni e confronti tra backend. Le espressioni di riferimento sono costruite direttamente, così un bug nei builder di NeverD non può semplificare entrambi i lati del test. Le build senza Z3 saltano esplicitamente gli oracle ma verificano ancora il contratto del backend indisponibile.

I test dei prodotti scalati confrontano il circuito integrato con una moltiplicazione lunga indipendente e con l’aritmetica intera fino a 257 bit. I test Z3 dimostrano la concordanza con una codifica indipendente a larghezza completa e verificano i bit alti di ingresso e un bit più significativo di uscita volutamente errato.

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

Ogni riga non commentata è `original ; candidate`; senza punto e virgola il candidato è il risultato del semplificatore MBA. Lo strumento riporta verdetti, replay del modello e tempi (include creazione sessione, traduzione e soluzione; esclude parsing, semplificazione MBA, export e teardown). Ogni ripetizione usa un solver nuovo. I modelli SAT devono riprodurre la differenza nell’evaluator. Verdetti decisivi opposti o query/modelli non validi fanno fallire il run; `unknown` viene registrato ma non prova equivalenza.

L’SMT-LIB esportato include il DAG codificato equivalente, asserzioni permanenti e assunzioni dell’ultima query; replay con `z3 query-N.smt2`. Registrare limiti di risorse e versioni: i backend usano unità di budget diverse, quindi il confronto è tra workload limitati e non tra lavoro identico. Le prove bitvector usano la semantica totale a larghezza fissa del linguaggio. Eccezioni macchina, effetti di memoria e LLVM poison restano responsabilità dei confini di lifting e traduzione; una prova d’espressione non li certifica.

Una copia della codifica non ancora esplorata rimuove dalla propria coda decisionale le variabili assegnate alla radice e ricostruisce l’ordine rigoroso dello heap per attività e indice. Assegnazioni e clausole restano intatte; i fatti alla radice sopravvivono a ogni backtracking. Proprietà della sorgente, decisioni effettive, modelli completi e tutti i budget di ricerca restano invariati.

Il DAG esportato in SMT-LIB è una codifica semanticamente equivalente e può contenere il quoziente esatto descritto sopra; la struttura può differire dal DAG originale.
