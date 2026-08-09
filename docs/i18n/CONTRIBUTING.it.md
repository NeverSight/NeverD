**Lingue**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md) | [繁體中文](CONTRIBUTING.zh-TW.md) | [日本語](CONTRIBUTING.ja.md) | [한국어](CONTRIBUTING.ko.md) | [Français](CONTRIBUTING.fr.md) | [Deutsch](CONTRIBUTING.de.md) | [Español](CONTRIBUTING.es.md) | [Italiano](CONTRIBUTING.it.md) | [Русский](CONTRIBUTING.ru.md) | [العربية](CONTRIBUTING.ar.md)

# Contribuire a NeverD

NeverD è un progetto di analisi binaria che mette la semantica al primo posto.
Un contributo utile è mirato, fa fallire in modo esplicito i comportamenti non
supportati e include il test minimo che dimostra il contratto modificato.

Prima di apportare modifiche, leggi la
[guida all’architettura](../architecture.it.md). Usa la
[guida ai test](../testing.it.md) per scegliere la suite e la
[roadmap](../roadmap/README.it.md) per il lavoro di prodotto pianificato.

## Prerequisiti

- Git con supporto per i sottomoduli ricorsivi
- CMake 3.20 o successivo
- Ninja
- Un compilatore C++20
- Clang e LLD (`ld.lld` e `lld-link`) per il set completo di fixture
  multi-target

I sottomoduli ricorsivi forniscono i fork LLVM e Capstone di NeverD, Unicorn e
i dati delle firme. Durante la convalida di una modifica, non sostituirli con
revisioni di sistema arbitrarie.

## Clonazione e inizializzazione

Lo sviluppo viene integrato in `dev`, che è anche il branch predefinito del
repository. Clonalo con tutti i sottomoduli:

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

In un clone esistente, sincronizza i sottomoduli prima della prima build e dopo
ogni commit che modifica le revisioni registrate:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

## Scelta del profilo di build

| Profilo | Utilizzo | Comportamento importante |
|---------|----------|--------------------------|
| Release | Sviluppo normale, test completi, benchmark di decode/lift | Ottimizzato; throughput rappresentativo |
| RelWithDebInfo | Profilazione o debug di percorsi critici ottimizzati | Ottimizzato con simboli di debug |
| Debug | Assertion, esecuzione passo-passo, correttezza locale | Non ottimizzato; benchmark di decode volutamente molto più lenti |

Usa Release a meno che l’attività non richieda espressamente il comportamento
Debug:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel
```

Per impostazione predefinita, la build compila `third_party/llvm-project` come
dipendenza integrata. La prima build richiede in genere 30–60 minuti; le
successive sono incrementali. `CMakePresets.json` definisce anche i preset di
configurazione/build `release`, `relwithdebinfo` e `debug`, ma sopra vengono
usate directory esplicite per rendere visibile l’abilitazione dei test.

Per il debug a livello sorgente usa una directory separata, invece di
riconfigurare l’albero Release:

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build-debug --parallel
```

Non pubblicare mai dati di throughput di decode o lift ottenuti da una build
Debug. Usa Release per i benchmark, oppure RelWithDebInfo quando la profilazione
richiede i simboli.

### LLVM precompilato su macOS

Chi contribuisce da Apple Silicon può evitare di compilare localmente il fork
LLVM:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

CMake scarica il pacchetto di release configurato dal repository, ne verifica
il checksum SHA-256 e riutilizza la cache utente estratta nelle build
successive. Il canale precompilato supporta solo macOS arm64. I Mac Intel e le
build universali devono usare la build LLVM locale predefinita. Le
personalizzazioni avanzate, come `NEVERD_LLVM_PREBUILT_TAG`, URL del mirror,
directory della cache e checksum esplicito, sono documentate in
`cmake/NeverDLLVMPrebuilt.cmake`.

## Flusso di branch e pull request

Parti da un `dev` aggiornato e crea un branch tematico mirato:

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

Apri le pull request verso `dev`, non verso un presunto branch di release.
Mantieni i commit facili da esaminare: un unico scopo coerente, nessun output di
build generato, nessuna formattazione estranea e nessuna revisione di
sottomodulo modificata, salvo che faccia parte della proposta.

## Stile del codice

C e C++ seguono le convenzioni LLVM, con `.clang-format` come autorità di
formattazione del repository. Formatta solo i file modificati:

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

Non riformattare l’intero repository per una correzione mirata. Segui i modelli
di denominazione e scomposizione circostanti, mantieni il comportamento
specifico della piattaforma al confine loader/lifter/backend pertinente e non
esporre tipi C++ interni attraverso l’SDK C puro.

Il Markdown deve essere conciso e verificabile dal sorgente. Usa link relativi
per i file del repository e aggiorna la documentazione nella stessa pull
request quando cambiano il comportamento CLI, le API pubbliche, le dichiarazioni
di supporto, i flag di build o i comandi di test.

## Esecuzione dei test

Esegui tutti i test registrati tramite il target aggregato:

```bash
cmake --build build-release --target check-neverd
```

Durante lo sviluppo usa il target pertinente più piccolo o una label CTest:

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

La [guida ai test](../testing.it.md) documenta tutti i target di comodità, le
suite di trasformazione selezionabili solo tramite label, le espressioni
regolari per un singolo test, la compilazione delle fixture e i roundtrip
Unicorn. Se un target viene ignorato perché manca un cross-compiler o un linker,
segnala la limitazione; non descrivere il percorso non eseguito come superato.

## Checklist della pull request

Prima di richiedere una revisione:

- Esegui rebase o merge dell’ultimo `dev` secondo il flusso preferito dai
  maintainer e risolvi deliberatamente le modifiche ai sottomoduli.
- Compila i target interessati in Release, oppure spiega perché serve un altro
  profilo.
- Esegui i test di regressione mirati e la suite pertinente più ampia
  concretamente possibile; includi i comandi esatti e tutti gli skip nella
  descrizione della PR.
- Mantieni il lifting strict: un’istruzione non supportata non deve diventare
  silenziosamente un’operazione ipotizzata o un `NOP`.
- Aggiungi copertura semantica per i cambiamenti di comportamento, non solo
  snapshot testuali dell’IR.
- Tieni fuori dal diff pulizie estranee, file generati e artefatti di build
  locali.
- Aggiorna la documentazione pubblica e per i contributor quando cambiano
  comportamento, supporto, flag, comandi o responsabilità dei test.

Per segnalazioni sensibili alla sicurezza che non devono iniziare come pull
request pubblica, segui [SECURITY.md](../../SECURITY.md).
