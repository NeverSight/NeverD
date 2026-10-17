**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 1eae101089e730c29ba3fda92db515fe148fe41b758d6f5b69156252591d9b3f -->

[← Documentation Index](README.md)

# Patch da sorgenti e oggetti nativi

NeverD compila una funzione sostitutiva con il compilatore scelto e la installa in un ingresso originale esplicito. Il compilatore definisce la semantica C/C++; NeverD gestisce indirizzi, rilocazioni, memoria, record di unwinding e pubblicazione. La riparazione binaria e la riscrittura del codice sollevato mantengono le proprie pipeline. Il resto del programma non viene ricompilato.

<!-- i18n-section: compiler -->
## Scelta del compilatore

Scegliere `--compiler=clang`, `--compiler=gcc` o `--compiler=neverc`. Clang e NeverC in modalità ordinaria possono produrre LLVM IR; `object` conserva le istruzioni native. La CLI sceglie gli oggetti per GCC per impostazione predefinita. Il SDK Python sceglie oggetti anche per GCC. Non viene mai usato implicitamente un altro compilatore.

Usare `--compiler-arg` o `compiler_args` dell’SDK per i percorsi di inclusione, le definizioni di macro e le opzioni sorgente supportate. Le modifiche al target o alla modalità di output e i token dei file di risposta (`@file`) vengono rifiutati in ogni posizione, anche come valori separati delle opzioni. I normali percorsi `-I` e le definizioni `-D` restano supportati.

Sostituire gli indirizzi di esempio con ingressi originali verificati. La dichiarazione deve corrispondere all’ABI originale, incluse convenzione di chiamata e disposizione delle strutture; un indirizzo non lo dimostra. C++ richiede `--language=c++` e l’eseguibile Clang++/G++ corretto. Preferire un ingresso `extern "C"` o indicare il nome decorato esatto. Sono consentite funzioni ausiliarie. `--entry` e `--func` selezionano esplicitamente la sostituzione; anche il vecchio `--from-c` rispetta l’indirizzo.

È anche possibile scegliere esplicitamente `artifact="object"`.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## Profili verificati e limiti

[NeverC producer: 182fd2c5dc](https://github.com/NeverSight/NeverC/commit/182fd2c5dc)

**NeverC C e DynCode ospitato**

NeverC accetta solo C puro su ELF, PE e Mach-O x64/ARM64. NeverD rifiuta NeverC con `--language=c++` prima di avviare il compilatore, senza traduzione o cambio implicito del compilatore. C++ ed eccezioni usano Clang oppure un driver GCC compatibile con il profilo oggetto ELF verificato. I frame C hosted possono trasmettere un’eccezione generata e catturata dal programma C++ circostante mediante autentici dati di unwinding; questo non aggiunge C++ a NeverC.

`dyncode`: `--compile-mode=dyncode` usa `-fdyncode-hosted`: sorgente, un ingresso esplicito, oggetto nativo e nessuna associazione originale. CLI e Python scelgono oggetti per questo modo e GCC. Prima della pubblicazione verificano hash, ogni byte collegato e l’ingresso, usando vere CFI/SEH o record compact completi. Stato modificabile/TLS, conversioni implicite syscall/PEB/heap/string, import irrisolti e codice riordinato sono rifiutati. Dati e callback possono essere parametri.

```sh
neverd patch app --from-source=replacement.cpp --language=c++ \
  --compiler=clang --compiler-path=/path/to/clang++ \
  --entry=replacement --func=0x401140 --patch-report -o app.cpp
neverd patch app --from-source=replacement.c --compiler=neverc \
  --compiler-path=/path/to/neverc --compile-mode=dyncode \
  --entry=replacement --func=0x401140 --patch-report -o app.dyncode
```

Clang LLVM IR ammette Windows ARM64 C++ FH3 e verifica xdata, ABI del frame padre e gruppi funclet/FuncInfo. Gli handler di linguaggio degli oggetti restano esclusi. `check_source_patch_cpp_runtime.py` esegue nove scenari su Windows nativo x64/ARM64; evidenze CPU e unwinder OS restano distinte.

| Compilatore | Artefatto | Profilo runtime OS verificato |
|---|---|---|
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++; Mach-O x64/ARM64 C/C++; PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| Clang 22.1.6 | Oggetto nativo | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C; Mach-O x64/ARM64 C; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| GCC/G++ 13.3.0 | Oggetto nativo | ELF x64/ARM64 C/C++ con runtime Itanium compatibile; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| NeverC 3389.1.5 modalità ordinaria | LLVM IR | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 modalità ordinaria | Oggetto nativo | ELF x64 C |
| NeverC 3389.1.5 C | LLVM IR / oggetto nativo | ELF / PE / Mach-O x64/ARM64: sorgente C puro |
| NeverC 3389.1.5 `hosted-v1` | Oggetto nativo + payload sigillato | ELF / PE / Mach-O x64/ARM64: sorgente C e propagazione attraverso frame C |

La tabella descrive profili OS verificati, senza garantire ogni versione o opzione. Gli installatori nativi ELF, PE e Mach-O accettano tutte le quattro architetture. GCC richiede un driver ELF compatibile e non produce LLVM IR. Rilocazioni sconosciute, TLS, costruttori globali al caricamento, LTO, simboli common/IFUNC e importazioni irrisolte falliscono esplicitamente. La ricevuta JSON registra compilatore, argomenti, associazioni e indirizzi; gli oggetti nativi includono SHA-256.

Il campo `target` della ricevuta di un oggetto nativo deriva dall’ABI dell’immagine caricata, inclusi i flag in virgola mobile ARM ELF e la distinzione iOS/watchOS. Gli oggetti compilati e forniti seguono la stessa regola di identità.

La matrice CPU richiede l’installazione riuscita di sorgente, IR esterno e oggetti nativi in tutte le dodici combinazioni. Verifica separatamente ARMv7k watchOS e ARMv7 iOS, dati modificabili／costanti, argomenti in virgola mobile e sullo stack di funzioni originali e ingressi dopo il cambio dell’indirizzo di caricamento. L’esecuzione CPU non certifica loader OS, unwinder C++ o ABI arbitrarie; usare i profili OS sopra.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

`CPU` significa installazione sorgente／IR／oggetto, ricaricamento ed esecuzione esatta. ARM ELF ricava EABI5, stato ARM/Thumb e ABI floating point dall’immagine e registra un indice `EHABI` completo e ordinato. PE conserva le righe x64/ARM e le rilocazioni `HIGHLOW`/`DIR64`/`MOV32T`; x86 usa il codice di registrazione del compilatore. Mach-O distingue watchOS DWARF e iOS `SjLj` secondo il sottotipo CPU. I salti d’ingresso ARM/Thumb sono relativi e restano validi dopo il cambio di base.

Gli oggetti nativi usano simboli di funzione delimitati, definizioni ausiliarie／di sezione e dati unwind propri, senza inventare ID MC né ricompilare IR. I limiti delle funzioni COFF richiedono dimensioni ausiliarie esatte, sezioni con una sola funzione (`-ffunction-sections`) o record nativi di funzioni runtime decodificati; il simbolo successivo non serve mai a dedurre una dimensione. L’adattatore watchOS mantiene i frame delle funzioni foglia per ottenere registrazioni reali. Gli handler di linguaggio COFF/Mach-O nativi non rientrano nel profilo oggetti verificato; per le eccezioni C++ usare il percorso IR verificato. Sono rifiutati unwinder ARM statici con registro exidx fissato, puntatori assoluti Mach-O non registrati e memoria RELRO non supportata.

Gli oggetti C di GCC sono stati installati ed eseguiti anche su ELF x86, ARM32 e ARM64 tramite Linux QEMU user mode. L’adattatore sceglie ARM/Thumb dall’immagine e allinea le proprietà GNU alla larghezza dei puntatori ELF.

<!-- i18n-section: references -->
## Richiami all'originale, espansione del layout e dipendenze

I binding nominano dichiarazioni esterne: `function` autentica una funzione originale o stub d'importazione, `data` richiede memoria scrivibile e `const` una dichiarazione IR costante. I dati oggetto richiedono un'estensione non nulla, senza oltrepassare confini o concedere scrittura a RELRO. Usare esplicitamente `original` per chiamare il corpo salvato della funzione sostituita. L'ingresso pubblico continua a chiamare la sostituzione; quello salvato esegue un prefisso rilocato di istruzioni complete e prosegue nell'originale. Sorgente, IR fornito e oggetti nativi condividono il contratto, inclusi argomenti floating-point/sullo stack e callback multipli.

Un callback richiede limiti di funzione, modalità delle istruzioni, continuazione e prove di unwinding autenticati. Istruzioni sconosciute, riferimenti PC-relativi non sicuri, salti nel prefisso copiato, rilocazioni del loader non supportate, callback sovrapposti e prologhi con gestori non modellati sono rifiutati prima della pubblicazione. La dichiarazione deve comunque rispettare l'ABI originale. La ricevuta registra gli ingressi salvati in `original_callbacks`.

PE32/PE32+ può ampliare una tabella piena di intestazioni di sezione spostando intervalli del file e aggiornando i puntatori modellati di debug, simboli, certificati e overlay, preservando gli RVA originali. Mach-O x64/ARM64 PIE può ottenere spazio per comandi e unwinding tramite un'estensione verificata del prefisso dell'intestazione se lo consentono la guardia page-zero adiacente e il segmento linkedit finale; gli indirizzi originali di codice/dati rimangono fissi. Questo profilo non richiede `-Wl,-headerpad,0x400` né pagine libere adiacenti per unwinding. Riferimenti espliciti all'intestazione, comandi sconosciuti, proprietà mista codice/dati, intervalli malformati e layout non supportati sono rifiutati. Capacità di intestazioni e unwinding sono pianificate insieme prima dell'installazione.

Il nuovo codice resta RX e lo stato mutabile RW. Richieste esplicite `--dependency` aggiungono funzioni/dati tramite metadati dinamici ELF, descrittori d'importazione/IAT PE o metadati dylib/bind Mach-O. Identità, ordinali e versioni esistenti restano stabili; slot e stub invocabili mantengono ruoli distinti. Scegliere nome/percorso richiesto dal loader dell'OS destinazione. L'esempio usa il nome di una libreria condivisa ELF. Librerie o simboli mancanti devono causare veri errori del loader durante l'esecuzione. TLS, IFUNC, nuovi contratti di versione, costruttori al caricamento e rilocazioni non modellate restano esclusi; ELF `.data.rel.ro` e CET/IBT richiedono ulteriori contratti. Negli oggetti COFF nativi, dichiarare i nuovi import di funzioni/dati con `__declspec(dllimport)` affinché le rilocazioni identifichino la IAT. I normali riferimenti esterni diretti sono rifiutati.

```sh
# ELF example: the original target is at 0x401140.
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=original:saved_target=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --dependency=function:extra_function=libpatch_extra.so \
  --dependency=data:extra_state=libpatch_extra.so \
  --patch-report -o app.bound
```

Le opzioni di dipendenza, aggiunte in coda e verificate secondo la dimensione della struttura, valgono per sorgenti, IR fornito e oggetti nativi. La ricevuta include `added_imports`; dopo il ricaricamento si ricontrollano callback, importazioni, permessi e byte installati. Un errore preserva l'ingresso e ogni destinazione esistente.

<!-- i18n-section: examples -->
## Esempi eseguibili

[Esempi completi per Windows/macOS/Linux](../../examples/source-patch/README.md) includono sorgenti del programma, della sostituzione e della libreria, comandi di compilazione e un vero plugin Python SDK ospitato in NeverD. Eseguire dalla radice del repository con una build di NeverD con Python e una nuova directory di output.

```sh
# Linux / macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-native
```

```powershell
# Windows: Visual Studio developer PowerShell
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-native
```

Per impostazione predefinita vengono eseguiti entrambi i percorsi; aggiungere `--api cli` o `--api sdk` per sceglierne uno. L’indirizzo di destinazione viene rilevato automaticamente. Con `7`, l’originale stampa `8` ed entrambe le patch stampano `36`. Il driver verifica esecuzioni, ricevute ed errori reali del loader senza libreria; i comandi completi sono salvati in `commands.json`.

<!-- i18n-section: exceptions -->
## Eccezioni C++

ELF usa FDE/LSDA del compilatore; Mach-O usa compact unwind/DWARF. PE x64/ARM64 ammette tabelle FH3 con runtime MSVC esistente compatibile e identità dei tipi coerenti. I dati type-info possono richiedere associazioni esplicite. FH4, personality personalizzate, sorgenti SEH e modalità CFG/EHCont/XFG attive sono rifiutati; le protezioni originali restano invariate.

Gli originali ELF x86/x64 che richiedono IBT vengono attualmente rifiutati: l’installatore sorgente non offre ancora un trampoline che preservi ENDBR.

I casi runtime coprono ritorno normale, catch interno, una nuova eccezione catturata dal chiamante originale, un’eccezione dall’originale attraverso la sostituzione, pulizia RAII esattamente una volta, rilancio, terminazione noexcept e funzioni intatte. Il punto di chiamata originale deve già consentire la propagazione. Una patch non ripristina catch eliminati attorno a una funzione dimostrata non generare eccezioni.

<!-- i18n-section: publication -->
## SDK e pubblicazione

L’API C aggiuntiva è `neverd_patch_source` con `neverd_source_patch_options_v1`; `neverd_patch_source_report` restituisce la ricevuta. L’input oggetto specifica la lunghezza in byte. La vecchia API `neverd_patch_from_c` resta disponibile.

Gli input vengono fissati. Il candidato viene preparato accanto alla destinazione, ricaricato e verificato per ogni ingresso prima della pubblicazione esclusiva. File esistenti e alias simbolici non vengono sovrascritti. Una sola mappatura assente fa fallire l’intera richiesta. `--mode=inplace` è rifiutato. Le trasformazioni IR della sessione non si applicano agli oggetti nativi. Mach-O verifica e rinnova rigorosamente le firme; le firme PE non vengono eliminate silenziosamente.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="clang", compiler_path="/path/to/clang", artifact="object",
    bindings={
        "saved_target": ("original", original_address),
        "shared_counter": ("data", original_counter_address, 4),
    },
    dependencies={
        "extra_function": ("libpatch_extra.so", "function"),
        "extra_state": ("libpatch_extra.so", "data"),
    },
)
```

<!-- i18n-section: verification -->
## Compilazione e verifica

I sei job nativi OS/ISA selezionati sono passati senza errori né casi saltati: 3669 esecuzioni GoogleTest, 1008 osservazioni di processi nativi (504 funzionali e 504 di eccezioni C++), 12 rifiuti reali del loader, 96 immagini e 84 ricevute di pubblicazione.

| Host | Sorgente | Harness | Job CI | Test | Osservazioni / rifiuti |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

Gli artefatti qualificano esattamente i commit sorgente e harness elencati sotto. I successivi commit di integrazione dev e documentazione hanno controlli locali e documentali separati. I precedenti job falliti o annullati restano fallimenti. Le 216 osservazioni supplementari storiche, PE ARM32 e i risultati CPU/compatibilità Apple precedenti a 32 bit non dimostrano nuova copertura hardware nativa o delle eccezioni C++.

La qualificazione Windows ARM64 usa una DLL C Unicorn ARM64 nativa, compilata realmente dal sottomodulo fissato con LLVM-MinGW verificato, una libreria di importazione MSVC e una sostituzione esplicita del target CMake. La copia di build isolata autentica tre correzioni dell'host: coppia di salti senza SEH, callback ARM64 di commit delle pagine con sincronizzazione della cache istruzioni e maschere immediate a 64 bit per Windows LLP64. Sorgenti e revisione del sottomodulo restano invariati. Questo qualifica l'host della matrice CPU, senza dimostrare il supporto JIT MSVC ARM64 predefinito. I cinque controlli di ripristino dell'host restano separati dalle prove dei processi patchati nativi e delle eccezioni C++.

Scegliere un compilatore compatibile con `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (Clang 22.1.6 verificato). La compilazione integrata corregge solo una copia LLVM nel percorso di build, senza modificare sottomodulo o cache condivisa. Il difetto PLT x64 dei pacchetti r3/r4 può rifiutare una patch IR; gli oggetti nativi evitano questo percorso MC. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` richiede la produzione PLT esatta e corretta.

`NeverDSourcePatchTests` contiene casi nativi C/API/oggetti/eccezioni; `NeverDSourcePatchMatrixTests` verifica tutte le combinazioni con Unicorn. `SourcePatchPE` esegue sette scenari C++ con `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` e `NEVERD_TEST_WINDOWS_SDK_LIB`. Su host non Windows richiede anche `NEVERD_TEST_WINDOWS_RUNNER`. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` considera un errore gli input mancanti. Il workflow Windows dedicato richiede esecuzione nativa e rifiuta casi saltati.

Le prove Linux sul Mac di sviluppo usano Docker e un loader Linux esplicito; PE usa Wine 11. Mach-O ARM64 viene eseguito sul Mac stesso. I risultati CI Windows nativi vanno controllati separatamente: configurare un job non equivale a superarlo. Conteggi storici, errori baseline e limiti degli emulatori sono nei [contratti dettagliati e nel registro di convalida](../source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

La copia LLVM corregge anche i riferimenti dati GOT Mach-O x64: la tabella catch tipizzata di Clang è relativa alla fine della parola di rilocazione di quattro byte. r3/r4 la sposta di quattro byte; la verifica prima della pubblicazione rifiuta il risultato. Sul Mac, x64 usa Rosetta 2 e ARM64 viene eseguito direttamente.

La verifica nativa Windows x64 [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805) ha superato tutti i 16 test mirati PE/oggetti/trampoline e i 143 test della matrice dei target CPU, senza casi saltati. Il test FH3 ha eseguito sette scenari C++; la matrice usa Unicorn. Questa esecuzione ha usato Clang 22.1.6 con MSVC e Windows SDK nativi e LLVM integrato.

Il workflow Windows prepara anche 27 sostituzioni PE per x86, x64 e ARM64. `scripts/check_source_patch_pe_runtime.py` verifica chiamate non foglia, stato mutabile dei puntatori e argomenti in virgola mobile o sullo stack tramite IR sorgente, oggetti compilati e oggetti forniti. Esegue ogni originale e ogni risultato tre volte: 30 osservazioni di processo per architettura. x86/x64 vengono eseguiti su Windows x64; ARM64 su `windows-11-arm`. Il verificatore richiede l’host nativo previsto, commit e digest dei file corrispondenti e tutti i casi; gli esecutori esterni sono registrati separatamente. Questo profilo non dimostra il supporto delle eccezioni C++ ARM64.

`NEVERD_TEST_MATRIX_NEVERC` seleziona il compilatore facoltativo della matrice. NeverC 3389.1.5 ha superato tutti i 66 controlli x64/ARM64 per ELF, PE e Mach-O, compresa l’installazione IR/oggetto nativo e l’esecuzione CPU. Questa versione non supporta x86 o ARM32; usare Clang, oppure un driver GCC compatibile per ELF. Per compilare PE con NeverC, l’adattatore aggiunge `-fno-autolink`: i riferimenti vengono risolti nell’immagine esistente senza collegare un altro CRT. Gli oggetti forniti con contratti di collegamento `.drectve` restano rifiutati.

La verifica dei processi Windows nativi [a4a5ae3cb](https://github.com/NeverSight/NeverD/actions/runs/38350079817) ha superato tutte le 90 osservazioni: 30 per ciascuna architettura x86, x64 e ARM64, tramite IR sorgente, oggetti compilati e oggetti forniti. x86/x64 sono stati eseguiti su Windows x64; ARM64 su `windows-11-arm`, usando le stesse immagini verificate tramite hash. La stessa esecuzione ha superato anche i 16 test mirati PE/oggetti/trampoline e i 143 test della matrice Unicorn, senza casi saltati. Il test FH3 ha mantenuto i suoi sette scenari C++ x64.

La qualifica nativa fissa NeverC a `182fd2c5dc`, con originali indipendenti e l’installatore NeverD attuale. `check_source_patch_hosted_pe_runtime.py` richiede 36 osservazioni per architettura Windows per IR/oggetti/hosted C e propagazione attraverso due frame C. `check_source_patch_elf_runtime.py` ne richiede 120 per architettura Linux per C, propagazione hosted e nove scenari C++ Clang. Ricevute, hash e architettura reale devono coincidere; non si dichiara che NeverC stesso sia stato eseguito sull’host CI. Linux x64 e ARM64 eseguono i nove scenari per ciascuna via: IR Clang, oggetti nativi Clang e oggetti nativi GCC. NeverC resta limitato a C.

Le 204 osservazioni del vecchio profilo di traduzione sono storiche e non provano l’attuale contratto solo C. Le tre vie NeverC attuali usano C; Clang C++ FH3 viene verificato separatamente. Sul Mac di sviluppo Apple Silicon, macOS x64 usa Rosetta 2. Unicorn/QEMU non sostituiscono la verifica nativa del sistema e dell’unwinder.

Le rilocazioni delle istruzioni AArch64 richiedono addendi RELA espliciti. Gli addendi REL impliciti delle istruzioni vengono rifiutati prima della pubblicazione; i campi di bit non vengono interpretati come dati ordinari. Le rilocazioni dati PREL64 supportate mantengono gli addendi completi con segno sia in REL sia in RELA.

La verifica del produttore C [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591930213) ha superato tutte le 312 osservazioni di processi nativi: 36 per ciascuna architettura Windows x64 e ARM64 e 120 per ciascuna architettura Linux x64 e ARM64. Linux verifica separatamente nove scenari di eccezioni per Clang IR, oggetti nativi Clang e oggetti nativi GCC; tutti gli ingressi NeverC per IR, oggetti e hosted DynCode sono C.

La verifica dell’installatore ricompilato [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591928600) ha superato 19 controlli PE/oggetto, tutti i 143 casi della matrice CPU e 474 osservazioni di processi nativi Windows/Linux. I processi Windows x86 sono stati eseguiti su Windows x64; x64 e ARM64 hanno usato host corrispondenti. La [verifica Linux](https://github.com/NeverSight/NeverD/actions/runs/38585759996) separata ha superato i 108 casi originali più tre controlli LLVM-C e tre Clang/GCC C++, senza errori né casi saltati.

La [verifica di integrazione 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627694097) ha riconfermato tutte le 312 osservazioni di processo su Windows/Linux nativi x64 e ARM64. Le 30 immagini finali corrispondono ai byte già verificati; gli hash dei sorgenti e le ricevute di pubblicazione solo C sono stati aggiornati e controllati indipendentemente.

La [verifica Linux estesa su 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38626866257) ha superato 429 controlli eseguiti, senza errori né casi saltati: i 108 originali, 55 controlli Swift completi, 143 dei target CPU, 117 delle sessioni native, tre LLVM-C e tre C++ Clang/GCC. Anche l’intera suite locale di patch binarie ha superato tutti i 22.961 casi su questa integrazione. I processi nativi e la copertura con emulazione CPU restano conteggiati separatamente.

La [verifica dell’installer Windows ricompilato a 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627581732) ha superato tutti i 182 controlli mirati e le 474 esecuzioni di processi nativi, senza errori né test saltati. Un controllo indipendente degli artefatti ha verificato ogni hash delle immagini, architettura, scenario e ripetizione. Dopo l’integrazione ABI/unwind a `e1cac949f`, la suite completa di patch binarie ha nuovamente superato tutti i 22,961 casi, senza errori né test saltati. NeverC resta limitato al C puro; le sostituzioni C++ usano Clang/GCC.

<!-- i18n-section: runtime-coverage -->
## Verifica dei processi macOS e a 32 bit

Il workflow delle immagini richiede esecuzione nativa su macOS ARM64 (`macos-15`) e Intel x64 (`macos-15-intel`). Ogni architettura esegue 96 osservazioni su sorgenti/oggetti C Clang, stato modificabile tramite puntatori, argomenti floating point/sullo stack, nove scenari di eccezioni C++ Clang e output IR/oggetto/hosted del NeverC C puro esistente. Le firme Mach-O vengono verificate rigorosamente prima dell’esecuzione; il codice hosted installato deve coincidere byte per byte con il payload del compilatore.

Lo stesso workflow richiede 30 osservazioni Windows x86 su Windows x64, 48 Linux x86 su Linux x64 e 96 Linux ARM32 tramite QEMU in modalità utente, divise equamente tra ARM e Thumb. Le verifiche Linux includono gli oggetti GCC adatti al target. Ogni scenario viene ripetuto tre volte. I rapporti distinguono `native`, `compatibility`, `rosetta` e `qemu-user`, registrando architettura target e host effettivo. Rosetta non soddisfa il requisito di esecuzione Intel nativa.

Tutti i 14 profili CPU restano coperti dalla matrice di 283 verifiche sorgente/IR/oggetto. PE ARM32, Mach-O i386, ARMv7 iOS e ARMv7k watchOS hanno attualmente verifica CPU, senza attestare il caricatore OS nativo o l’unwinder C++. NeverC resta limitato al C puro su x64/ARM64; Clang e i driver ELF GCC corrispondenti producono gli output a 32 bit. Il verificatore non compila né modifica NeverC.

Windows x64 e ARM64 eseguono inoltre 36 osservazioni Clang C++ FH3 ciascuno.

I puntatori modificabili Mach-O sono inizializzati durante l’esecuzione; gli inizializzatori di indirizzi statici richiedono ancora una registrazione delle rilocazioni al caricamento non supportata e hanno un test esplicito di rifiuto. Le funzioni COMDAT ELF di dimensione zero sono accettate solo se la firma è l’unica funzione all’inizio della sezione eseguibile e la CFI coincide esattamente con l’intera sezione. Offset interni, sezioni condivise, padding, gruppi assenti e CFI assente restano rifiutati.

ELF ARM e Thumb hanno profili distinti. I produttori ricevono esplicitamente `-marm` o `-mthumb` e i binding nativi mantengono il bit di stato delle istruzioni originale. I processi ELF a 32 bit usano PIE con rilocazioni dinamiche reali. L’esecuzione ARM32 QEMU registra un `--loader` glibc esplicito e un sysroot: il suo calcolo AUXV non segue gli header spostati come il kernel Linux. Questa via verifica caricamento glibc ed esecuzione sotto QEMU, senza attestare hardware ARM32 o il caricatore nativo del kernel.

Per queste immagini ELF con patch, le vie `qemu-user` di Linux x86 e ARM32 richiedono entrambe un `--loader` glibc esplicito dal sysroot corrispondente all’architettura di destinazione. Linux x86 in modalità `compatibility` usa il supporto a 32 bit del kernel dell’host Linux x64 e viene registrato separatamente dall’esecuzione QEMU; non attesta l’esecuzione su un host x86 nativo a 32 bit.

La [verifica dei processi](https://github.com/NeverSight/NeverD/actions/runs/38694434082) di `fcee81c9a5e2f20a71090ade23ae833175f64c59` ha superato tutte le 750 osservazioni: 576 su host nativi della stessa architettura, 78 in modalità di compatibilità x86 e 96 ARM/Thumb con QEMU in modalità utente. Un controllo indipendente degli artefatti ha verificato gli hash di tutte le 126 immagini, gli ambienti di esecuzione e tutti gli scenari. La [ricompilazione Windows](https://github.com/NeverSight/NeverD/actions/runs/38694628242) ha superato 194 controlli mirati e 474 osservazioni di processi Windows/Linux. La [ricompilazione Linux](https://github.com/NeverSight/NeverD/actions/runs/38694638358) ha superato 444 controlli, compresi tutti i 62 errori e i 46 casi saltati originari, senza errori né omissioni. Restano validi i limiti hardware e del caricatore indicati sopra. NeverC rimane dedicato al C puro e il suo repository non è stato modificato.

La [validazione aggiuntiva ARM/Thumb](https://github.com/gmh5225/test_mac_intel/actions/runs/38750504263) ha superato tutte le 96 osservazioni (48 ARM, 48 Thumb) delle 32 immagini di test C da `fcee81c9a5e2f20a71090ade23ae833175f64c59` su `ubuntu-24.04-arm`. Il kernel Linux ha caricato direttamente ogni immagine tramite `/lib/ld-linux-armhf.so.3`, senza QEMU né un comando esplicito del caricatore. Si tratta di `compatibility` su un host ARM64, non di un host nativo a 32 bit. Il commit del programma di test `3ad17ac9608d6bd9f5a4d44953ec375524e5730b` e quello delle immagini vengono registrati separatamente. Audit indipendenti hanno verificato gli hash delle immagini, tutte le 30 ricevute di pubblicazione, tutti gli scenari e le ripetizioni e l’assenza di regole dell’interprete `binfmt_misc` corrispondenti.

Le [verifiche separate dei runner ospitati](https://github.com/gmh5225/test_mac_intel/actions/runs/38749138719) hanno rilevato che il runner Windows 11 ARM64 fornito rifiuta i programmi ARM32 con `WinError 216`. Su macOS 15.7.9 con Xcode 16.4, gli SDK disponibili non hanno consentito di collegare programmi macOS i386 o iOS ARMv7. Il collegamento di un programma watchOS ARMv7k è riuscito, ma non è stata accertata l’esecuzione su un dispositivo fisico. Il successo delle verifiche non convalida queste piattaforme; la copertura hardware nativa resta incompleta. NeverC non è stato compilato né modificato.

Le [verifiche Linux aggiuntive nelle Actions personali](https://github.com/gmh5225/test_mac_intel/actions/runs/38773830274) hanno superato 96 esecuzioni ARM/Thumb in un sistema guest ARM32 completo sotto QEMU TCG, con kernel Linux a 32 bit e glibc, oltre a 30 esecuzioni PE ARM32 tramite Wine nello stesso guest. Il kernel guest carica direttamente le immagini ELF; questa emulazione di sistema è distinta dal percorso `qemu-user` con loader esplicito. L'installazione del registro Wine nel livello di compatibilità dell'host ARM64 è registrata separatamente e non conta come esecuzione di patch. L'inizializzazione di Wine nel guest e ogni esecuzione misurata devono riuscire entro i limiti di tempo originali.

Le [verifiche di compatibilità Apple](https://github.com/gmh5225/test_mac_intel/actions/runs/38771214358) hanno superato 30 esecuzioni Mach-O i386 con Darling su Linux e 60 esecuzioni ARMv7/ARMv7k con touchHLE su macOS Intel. Ogni originale e ogni sostituzione da IR sorgente, oggetto compilato o oggetto fornito vengono eseguiti tre volte. Gli audit indipendenti hanno verificato tutte le 72 immagini, 66 ricevute di pubblicazione e 216 esecuzioni di queste prove aggiuntive, comprese dimensioni degli artefatti GitHub, SHA256, commit esatti e inventari completi. Gli input Linux restano fissati a `fcee81c9a5`; quelli PE sono stati generati in `a542f97ba` e quelli Mach-O in `8f5f06762`. I commit degli script di verifica sono registrati separatamente nelle esecuzioni collegate.

Le correzioni Mach-O autenticano le funzioni Thumb selezionate indipendentemente dal riepilogo ARM/Thumb misto dell'immagine, normalizzano i puntatori di esportazione Thumb comprovati e gli indirizzi di codice di unwinding ARM32 senza modificare puntatori ai dati o lunghezze degli intervalli. La build con sorgenti LLVM integrati ha superato localmente tutti i 283 controlli della matrice CPU e i 258 controlli dei contratti di eccezione, senza errori né casi saltati. Le prove Mach-O legacy usano un linker open source documentato e riservano spazio per l'unwinding; i386 dispone di un ingresso di avvio correttamente allineato e di un segmento page zero di 64 KiB.

Questi risultati attestano l'esecuzione C negli emulatori e livelli di compatibilità indicati. Wine non è Microsoft Windows, Darling non è macOS e l'esecuzione ARMv7k nella HLE iOS non qualifica watchOS, WatchKit o dispositivi Apple. Non viene dedotta alcuna copertura di vecchi SDK Apple o hardware nativo a 32 bit; queste prove non qualificano neppure l'unwinding delle eccezioni C++. Le precedenti evidenze native e di compatibilità restano separate. NeverC non è stato compilato né modificato.
