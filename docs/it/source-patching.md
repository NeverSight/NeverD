**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 3ceb56e2abf86f594f437a16b09a1888650cb49d4cf06860711e73458c52c68d -->

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
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++; Mach-O x64/ARM64 C/C++; PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3 |
| Clang 22.1.6 | Oggetto nativo | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C |
| GCC/G++ 13.3.0 | Oggetto nativo | ELF x64/ARM64 C/C++ con runtime Itanium compatibile |
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
## Riferimenti originali e memoria

Le associazioni corrispondono a dichiarazioni esterne: `function` richiede una funzione originale o uno stub import verificato; `data` richiede memoria scrivibile; `const` richiede una dichiarazione IR costante. I dati degli oggetti richiedono un’estensione non nulla. I limiti della memoria non possono essere superati, né RELRO può diventare scrivibile. I callback al corpo originale sostituito sono rifiutati: questo profilo non conserva il prologo in un trampolino.

Il nuovo codice resta RX e lo stato mutabile RW. Si riutilizzano solo import esistenti; gli slot GOT/IAT restano distinti dagli stub chiamabili. Mach-O richiede spazio per comandi di caricamento, per esempio `-Wl,-headerpad,0x400`; PE richiede spazio per intestazioni di sezione. Lo spazio insufficiente provoca un errore. ELF `.data.rel.ro` e le protezioni CET/IBT originali richiedono contratti aggiuntivi e sono attualmente rifiutati.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

La crescita delle tabelle Mach-O richiede anche spazio libero adiacente nel file e nello spazio virtuale; la sola riserva per intestazioni non basta. I fixture verificati distinguono questi requisiti.

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
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## Compilazione e verifica

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

Le 204 osservazioni del vecchio profilo di traduzione sono storiche e non provano l’attuale contratto solo C. Le tre vie NeverC attuali usano C; Clang C++ FH3 viene verificato separatamente. macOS x64 usa Rosetta 2. Unicorn/QEMU non sostituiscono la verifica nativa del sistema e dell’unwinder.