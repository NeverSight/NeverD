**Lingue**: [English](../../CONTRIBUTING.md) | [简体中文](../zh-CN/CONTRIBUTING.md) | [繁體中文](../zh-TW/CONTRIBUTING.md) | [日本語](../ja/CONTRIBUTING.md) | [한국어](../ko/CONTRIBUTING.md) | [Français](../fr/CONTRIBUTING.md) | [Deutsch](../de/CONTRIBUTING.md) | [Español](../es/CONTRIBUTING.md) | [Italiano](CONTRIBUTING.md) | [Русский](../ru/CONTRIBUTING.md) | [العربية](../ar/CONTRIBUTING.md)

<!-- i18n-source: af6c34901bacab00e5d1ed68b031f0a48a3ff64ac85ee208695647ec935b9790 -->

# Contribuire a NeverD

NeverD è un progetto di analisi binaria che mette la semantica al primo posto.
Un contributo utile è mirato, fa fallire in modo esplicito i comportamenti non
supportati e include il test minimo che dimostra il contratto modificato.

Conserva gli avvisi di copyright, licenza e fonte quando incorpori codice
esistente, incluso il codice copiato o adattato con strumenti di IA/LLM.
Segui la [guida all’attribuzione](ATTRIBUTION.md), indica la fonte e la
revisione originali del materiale riutilizzato e verificane la licenza prima
di inviare il contributo. Mantieni gli avvisi di terzi sotto le loro licenze
originali.

Prima di apportare modifiche, leggi la
[guida all’architettura](architecture.md). Usa la
[guida ai test](testing.md) per scegliere la suite e la
[roadmap](roadmap.md) per il lavoro di prodotto pianificato.

<!-- i18n-section: prerequisites -->
## Prerequisiti

- Git con supporto per i sottomoduli ricorsivi
- CMake 3.20 o successivo
- Ninja
- Un compilatore C++20
- CPython 3.10+ con file di sviluppo per l’incorporamento, se i plugin Python sono abilitati
- Clang e LLD (`ld.lld` e `lld-link`) per il set completo di fixture
  multi-target

I sottomoduli ricorsivi forniscono i fork LLVM e Capstone di NeverD, Unicorn e
i dati delle firme. Durante la convalida di una modifica, non sostituirli con
revisioni di sistema arbitrarie.

<!-- i18n-section: clone -->
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

<!-- i18n-section: build -->
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
python scripts/build.py build-release --parallel
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
python scripts/build.py build-debug --parallel
```

Non pubblicare mai dati di throughput di decode o lift ottenuti da una build
Debug. Usa Release per i benchmark, oppure RelWithDebInfo quando la profilazione
richiede i simboli.

<!-- i18n-section: allocator -->
### Allocatore del processo

Le build Linux, macOS e Windows MSVC/clang-cl abilitano `NEVERD_ENABLE_MIMALLOC` per
impostazione predefinita. Strumenti, worker di analisi e test creati dall’helper CMake comune
usano il ramo predefinito del fork mimalloc NeverSight, seguendo il modello host di NeverC.
Unix lo incorpora una volta per eseguibile. Windows usa una DLL mimalloc e la corrispondente
`mimalloc-redirect*.dll` della stessa architettura per il CRT dinamico, inclusi libneverd e
plugin nativi. Entrambe affiancano gli eseguibili e sono installate in `bin`: distribuirle
insieme. Windows richiede `/MD` o `/MDd`; disabilitare mimalloc con CRT statico. L’SDK non
impone un allocatore all’applicazione ospitante. Su Windows ARM64 usare build ARM64 native:
una DLL x64 non può reindirizzare il CRT ARM64EC dell’emulazione x64. MinGW conserva
l’allocatore di sistema. Vedere i [requisiti Windows upstream](https://github.com/NeverSight/mimalloc/blob/main3/bin/readme.md).

Usare `-DNEVERD_ENABLE_MIMALLOC=OFF` per confronti con il sistema o allocatori esterni.
`NEVERD_WORKER_MIMALLOC=OFF` disabilita solo il worker. CMake sopprime mimalloc con sanitizer
address, thread, memory e leak, inclusi flag combinati o specifici della configurazione per
compilatore/linker. UBSan da solo resta compatibile. Con flag sanitizer specifici del target,
impostare anche OFF. Windows permette confronti senza ricompilare con
`MIMALLOC_DISABLE_REDIRECT=1`; `MIMALLOC_VERBOSE=1` segnala l’attivazione.

L’ingresso supportato, `python scripts/build.py BUILD_DIR`, aggiorna tutti e cinque i fork
NeverSight dai rami remoti predefiniti prima di ogni build. Sono sottomoduli di `third_party`,
come Capstone e Unicorn. Lo script risolve i nomi effettivi dei rami, aggiorna i cinque
sottomoduli ed esporta i sorgenti committati in `BUILD_DIR/third_party/<library>/<commit>/src`.
Modifiche locali non committate o divergenti fermano la build senza sovrascrittura; anche
errori di rete la fermano. La directory resta bloccata durante aggiornamento, configurazione
e compilazione; lo snapshot dura fino all’invocazione successiva. Anche le build incrementali acquisiscono nuovi commit.

`neverd-dependency-inputs.json` registra input di configurazione, commit e SHA-256 degli export,
e accompagna i prodotti installati; non certifica il binario. `neverd-build-receipt.json`
registra esito solo per target e configurazione richiesti. CI conserva entrambi. L’installazione
rifiuta build fallite, input modificati e configurazioni non compilate; CI prodotto compila
tutti i target installati prima di verificare l’installazione. Confrontare commit prima di
qualificare insieme piattaforme diverse: la mainline può cambiare fra esecuzioni.
`cmake --build` controlla la freschezza e rifiuta snapshot obsoleti; usare l’ingresso Python
per aggiornarli. La rigenerazione automatica conserva lo stesso snapshot.
`FETCHCONTENT_SOURCE_DIR_NEVERD_*` consente fixture locali isolate, ma l’ingresso mainline
lo rifiuta. I generatori multi-config richiedono `--config`. Compilare tramite l’ingresso,
poi eseguire test e `cmake --install` separatamente; il target diretto `install` viene rifiutato prima della compilazione.

Vedere [validazione dell’allocatore](../testing.md#allocator-validation) per smoke test e confronti prestazionali.

<!-- i18n-section: acceleration -->
### Librerie di accelerazione

Le build normali abilitano scansione byte Highway, validazione UTF-8 simdutf, parsing di
trasporto simdjson e scheduling oneTBB, mantenendo l’allocatore del processo. JSON conserva
DOM e serializzazione nlohmann: frame validi di soli interi vengono copiati dal tape SIMD;
decimali e zero negativo mantengono la conversione originale. Input fuori dominio simdjson
usano l’ammissione originale; testo esterno invalido conserva escape fedeli ai byte.
Queste librerie e mimalloc provengono dai fork NeverSight in `third_party/forks.json`, con
correzioni sui rami predefiniti. La build copia licenze correnti e registra commit esatti,
senza fissare le cinque dipendenze a vecchie release.

`NEVERD_ENABLE_HIGHWAY`, `NEVERD_ENABLE_SIMDUTF`, `NEVERD_ENABLE_SIMDJSON` e
`NEVERD_ENABLE_TBB` valgono `ON`; ciascuna ammette `OFF` per confronti. oneTBB conserva worker
fra batch, controlla stack da 8 MiB e non compila/carica `tbbmalloc` né il proxy. I chiamanti
con stack grande riusano il margine disponibile; gli altri ricevono un coordinatore nativo.
Host che eseguono fork dopo il primo uso o scaricano dinamicamente l’SDK devono usare thread
nativi (`NEVERD_ENABLE_TBB=OFF`): questi cicli di vita non sono supportati dallo scheduler
persistente. oneTBB viene collegato staticamente al nucleo condiviso e i suoi simboli
restano privati a quel nucleo. Le chiamate ai componenti dalla CLI e le richieste al worker
della GUI usano lo stesso scheduler; non viene distribuita alcuna DLL o libreria condivisa
TBB. I target che collegano `neverd_shared` selezionano automaticamente lo scheduler
condiviso. I client che accedono all’SDK tramite un altro target e collegano anche archivi
di componenti devono usare `neverd_use_shared_task_scheduler` sul target finale.
Gli strumenti di componenti autonomi mantengono il proprio runtime.
Questa integrazione vincolata evita più scheduler NeverD nello stesso processo;
il progetto upstream continua a considerare le [build statiche di oneTBB](https://uxlfoundation.github.io/oneTBB/main/intro/static_linking.html)
non supportate e ne sconsiglia l’uso generale.

Vedere [validazione delle accelerazioni](../testing.md#acceleration-libraries) per test nativi, confronti e limiti di compatibilità.

<!-- i18n-section: prebuilt-llvm -->
### LLVM precompilato su macOS

Chi contribuisce da Apple Silicon può evitare di compilare localmente il fork
LLVM:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
python scripts/build.py build-release --parallel
```

CMake scarica il pacchetto di release configurato dal repository, ne verifica
il checksum SHA-256 e riutilizza la cache utente estratta nelle build
successive. Il canale precompilato supporta solo macOS arm64. I Mac Intel e le
build universali devono usare la build LLVM locale predefinita. Le
personalizzazioni avanzate, come `NEVERD_LLVM_PREBUILT_TAG`, URL del mirror,
directory della cache e checksum esplicito, sono documentate in
`cmake/NeverDLLVMPrebuilt.cmake`.

Le correzioni LLVM appartengono al [fork LLVM NeverSight](https://github.com/NeverSight/llvm-project).
Eseguirne commit e merge nel fork, poi aggiornare la revisione del sottomodulo LLVM di NeverD.
La compilazione integrata compila direttamente questi sorgenti; non aggiungere file patch LLVM o copie modificate nella directory di build di NeverD.
I pacchetti precompilati devono essere ricostruiti dal fork corretto per qualificare lo stesso comportamento.

<!-- i18n-section: workflow -->
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

<!-- i18n-section: code-style -->
## Stile del codice

C/C++ seguono gli [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html).
[`.clang-format`](../../.clang-format) governa il formato e [`.editorconfig`](../../.editorconfig)
codifica, terminatori e spazi. La versione C++ è in `CMakeLists.txt`, non nella guida upstream.
Formattare i file modificati:

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

CI usa la versione fissata in [`scripts/check_clang_format.py`](../../scripts/check_clang_format.py);
usarla per riprodurre i risultati. Il formatter non verifica responsabilità degli header né
letterali di policy. Non riformattare tutto il repository per correzioni locali. Seguire nomi
e decomposizione circostanti, mantenere comportamento piattaforma ai confini loader/lifter/backend
e tipi C++ interni fuori dall’SDK C puro.

<!-- i18n-section: headers -->
### Struttura di file e header

- Nuovi file C++ iniziano con banner LLVM nome/scopo, `NeverD Decompiler` e Doxygen `\file`,
  come `lib/debug/PDBLoader.cpp`. Preservare avvisi di copyright e licenza.
- Usare header `.h` autonomi con dipendenze esplicite; l’implementazione include per prima
  l’interfaccia corrispondente. `.clang-format` ordina poi header privati vicini, pubblici
  `neverd/`, LLVM/Capstone fra virgolette, quindi fra parentesi angolari. Non affidarsi a ordine accidentale o inclusioni transitive.
- Nuovi header usano guardie `NEVERD_..._H` derivate dal percorso: `NEVERD_SDK_LIMITS_H`
  per `neverd/sdk/Limits.h`. Includere directory sufficienti per unicità di header privati/tool;
  commentare il nome presso `#endif`. Applicare quando si uniforma esplicitamente un header
  esistente, senza toccare quelli estranei alla modifica.
- Interfacce condivise in `include/neverd/<component>/`, header privati accanto ai sorgenti
  in `lib/` o `tools/`. La posizione non garantisce API C SDK stabile. Dipendenze acicliche,
  modelli/reader centrali indipendenti da sessioni e trasporto. Algoritmi nel componente
  proprietario, senza duplicare semantica CLI/SDK/worker. Vedere [architettura](architecture.md).
- Namespace proprietario e nomi chiari: tipi/campi `UpperCamelCase`, funzioni `lowerCamelCase`,
  costanti di policy `kUpperCamelCase`. Conservare stile delle interfacce esistenti, contratti
  C/Python e convenzioni STL. Nessuna direttiva generale `using namespace` negli header.
- Tabelle intenzionalmente multi-incluse mantengono `.def`/`.inc` e documentano il contratto
  macro; non sono interfacce `.h` autonome e non hanno guardie che impediscono espansioni.
  Modificare header generati tramite il produttore.

Esempio di header di policy autonomo:

```cpp
//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include <cstdint>

namespace neverd::sdk {

/// Traversal ceiling for a function without a recorded instruction extent.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
```

<!-- i18n-section: policy -->
### Responsabilità di limiti, valori predefiniti e configurazione

Usare `ResourceLimits.h` nelle directory dei tool aggiunte direttamente al percorso di ricerca degli header. Su un filesystem che non distingue maiuscole e minuscole, un `Limits.h` locale può nascondere l’header di sistema `<limits.h>`.

Ogni policy ha una sola definizione autorevole nell’header limiti/configurazione del proprio
sottosistema, anche con un solo chiamante: limiti I/O, ricorsione/grafi, budget di lavoro/iterazioni,
soglie euristiche, cache, pagine/batch, tentativi e timeout. Vale per `constexpr`, macro,
confronti letterali, `min`/`clamp`, inizializzatori e parametri predefiniti.

Scegliere prima l’autorità esistente:

| Responsabile | Definizione |
| --- | --- |
| Core/pipeline condiviso | [`include/neverd/Limits.h`](../../include/neverd/Limits.h) |
| SDK | [`include/neverd/sdk/Limits.h`](../../include/neverd/sdk/Limits.h) |
| Web | [`include/neverd/web/Limits.h`](../../include/neverd/web/Limits.h) o definizione specifica esistente |
| Altra libreria | Header/tabella esistente, altrimenti `Limits.h` mirato nello strato proprietario |
| Solo CLI, worker o GUI | `ResourceLimits.h` o `Config.h` del tool; il core non ne dipende |
| Opzioni del chiamante | Modello proprietario `Config.h`, `*Options.h` o `*Configuration.h` |

Header leggeri, raggruppati per scopo. Riutilizzare `*Limits.def` o tabelle di configurazione,
esponendo una vista C++ autonoma. Nessuna duplicazione fra `Limits.h`, `Constants.h`, `Config.h`;
policy core fuori da SDK/tool, policy di moduli indipendenti non tutte nel `Limits.h` radice.

Scalari C++ come `inline constexpr` tipizzati nel namespace esistente (`neverd::limits`,
`neverd::sdk`). Nomi con scopo/unità: `kMaxSourceBytes`, `kDefaultPageRows`,
`kRetryDelayMilliseconds`; documentare zero/sentinelle e limiti insoliti. Separare default e
massimo rigido se diversi. Un modello opzioni può possedere il default; consumatori usano
modello, factory o costante comune. Derivare valori correlati senza ripetere letterali.

Adapter, persistenza, CLI/GUI/worker e Python/MCP riusano l’autorità, inclusi massimi di schema
e diagnostiche numeriche. Contratti multilingua da tabelle o binding generati, altrimenti
controllo di deriva; conservare compatibilità C pura. Un limite anteprima/pagina più severo
ha nome e responsabilità propri; numeri uguali non implicano policy comune.

Layout binari, ABI/protocolli/istruzioni, intervalli interi e costi di algoritmi di prova
restano nei modelli semantici. Stati iniziali, indici, suggerimenti di allocazione e input
di test indipendenti non sono automaticamente policy. Non sostituire il limite atteso
indipendente con la costante implementativa che il test verifica.

Revisionare dichiarazioni e letterali: guardie multilinea, cicli, macro, default, timer e
binding. Verificare significato/proprietà di ogni policy toccata; cercare `Max*` non basta.
Consolidare preserva tipi, valori, direzione dei confronti, sentinelle ed esaurimento/errori.
Separare cambi di policy ed eseguire i [test interessati](testing.md).

<!-- i18n-section: artifacts -->
### Documentazione e artefatti locali

Markdown conciso e verificabile nei sorgenti, con link relativi interni. Aggiornare docs nella
stessa PR per CLI/API, supporto, flag e test. Sincronizzare tutte le lingue della guida;
revisionare prima di aggiornare SHA-256 `i18n-source`. Mantenere sezioni, esempi, API/opzioni
e destinazioni coerenti. Eseguire `python3 scripts/check_docs_i18n.py` per link e parità.

Rapporti prestazionali locali, trace grezze e tempi restano in directory ignorate build o
benchmark-results. Committare strumenti riutilizzabili e test di regressione, non report locali o tabelle copiate.

<!-- i18n-section: tests -->
## Esecuzione dei test

Esegui tutti i test registrati tramite il target aggregato:

```bash
python scripts/build.py build-release --parallel
cmake --build build-release --target check-neverd
```

Durante lo sviluppo usa il target pertinente più piccolo o una label CTest:

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
python scripts/build.py build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

La [guida ai test](testing.md) documenta tutti i target di comodità, le
suite di trasformazione selezionabili solo tramite label, le espressioni
regolari per un singolo test, la compilazione delle fixture e i roundtrip
Unicorn. Se un target viene ignorato perché manca un cross-compiler o un linker,
segnala la limitazione; non descrivere il percorso non eseguito come superato.

<!-- i18n-section: python-sdk -->
### SDK dei plugin Python

Per modifiche SDK eseguire prima suite Python puro e deriva, poi test adapter nativi:

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
python scripts/build.py build-release --target \
  NeverDPluginRuntimeTests NeverDPythonRuntimeTests NeverDPythonPluginTests
ctest --test-dir build-release -R 'NeverD(Python|Plugin)' --output-on-failure
```

Dichiarazioni C pubbliche, firme ABI Python, versioni pacchetto/CMake e policy di consegna
formano un contratto revisionato insieme; aggiornarli nella stessa modifica. Vedere la [guida ai plugin Python](python-plugins.md).

<!-- i18n-section: review -->
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
- Controllare [header e responsabilità delle policy](#stile-del-codice), guardie/default numerici
  e deriva dei binding; il solo formatter non basta.
- Aggiorna la documentazione pubblica e per i contributor quando cambiano
  comportamento, supporto, flag, comandi o responsabilità dei test.

Per segnalazioni sensibili alla sicurezza che non devono iniziare come pull
request pubblica, segui [SECURITY.md](../../SECURITY.md).
