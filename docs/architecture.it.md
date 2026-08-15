**Lingue**: [English](architecture.md) | [简体中文](architecture.zh-CN.md) | [繁體中文](architecture.zh-TW.md) | [日本語](architecture.ja.md) | [한국어](architecture.ko.md) | [Français](architecture.fr.md) | [Deutsch](architecture.de.md) | [Español](architecture.es.md) | [Italiano](architecture.it.md) | [Русский](architecture.ru.md) | [العربية](architecture.ar.md)

[← Indice della documentazione](README.it.md)

# Architettura di NeverD

Questa guida descrive i confini di produzione che un contributor deve conoscere
per modificare NeverD in sicurezza. Copre intenzionalmente solo il codice di
NeverD; i sottomoduli LLVM, Capstone e Unicorn mantengono la propria
architettura interna.

## Confine del sistema

```mermaid
flowchart LR
  CLI["tools/neverd CLI"] --> CAPI["libneverd C API"]
  SDKUser["SDK user or plugin"] --> CAPI
  CAPI --> Session["sdk::Session"]
  Session --> Loader["format loader"]
  Loader --> Image["BinaryImage"]
  Image --> Pipeline["Pipeline"]
  Pipeline --> Low["LowIR"]
  Low --> Med["MedIR"]
  Med --> High["HighIR"]
  High --> HighC["structured C"]
  Med --> LLVM["LLVM IR"]
  LLVM --> LLVMOut["LLVM IR or LLVM-derived C"]
  LLVM --> Codegen["target codegen"]
  Codegen --> Rewriter["PE / ELF / Mach-O rewriter"]
  Rewriter --> Patched["patched binary"]
```

NeverD ha quattro rappresentazioni IR, ma non costituiscono una sequenza
obbligatoria di quattro passaggi. `LowIR -> MedIR` è condiviso. La
decompilazione strutturata usa poi `MedIR -> HighIR -> C`, mentre `lift`,
`decompile --llvm` e `patch` seguono direttamente `MedIR -> LLVM IR`. In
particolare, le modalità patch e lift saltano intenzionalmente HighIR.

La CLI analizza i comandi in `tools/neverd`, crea un `neverd_session_t` e chiama
l’API pubblica di `include/neverd/sdk/NeverDCAPI.h`. Lo stato del motore risiede
in `lib/sdk/SessionImpl.h`; `neverd_session_load` sceglie un loader e costruisce
una `BinaryImage`, mentre le operazioni basate su IR eseguono
`lib/pipeline/Pipeline.cpp` su richiesta. L’eseguibile `neverd` collega
`neverd_shared`; gli archivi dei componenti e le loro dipendenze LLVM/Capstone
sono dettagli privati della libreria condivisa. La CLI usa LLVM
Support per l’interfaccia a riga di comando, ma non aggira la C API per pilotare
il motore.

## Rappresentazioni IR e percorsi

| Rappresentazione | Scopo | Definizioni e trasformazioni principali |
|------------------|-------|-----------------------------------------|
| LowIR | Operazioni `NdOp` indipendenti dall’architettura, basic block, CFG e metadati delle jump table | `include/neverd/ir/low`, `lib/ir/low`, prodotto da `lib/decode` + `lib/lift` |
| MedIR | Tipi, ABI/convenzioni di chiamata, modello memoria/stack, flag, chiamate e flusso simile a SSA | `include/neverd/ir/med`, `lib/ir/med` |
| HighIR | Espressioni e controllo di flusso strutturati per C leggibile | `include/neverd/ir/high`, `lib/ir/high`, emesso da `lib/backend/c/HighC` |
| LLVM IR | Ottimizzazione, C derivato da LLVM, generazione di codice target e input per riscrittura binaria | `lib/backend/llvm`, ottimizzato/orchestrato da `lib/pipeline` |

| Percorso utente | Cammino delle rappresentazioni | Uscita |
|-----------------|-----------------------------|--------|
| Dump Low/Med | Binary -> LowIR, opzionalmente -> MedIR | Testo diagnostico |
| Dump High o `decompile` | Binary -> LowIR -> MedIR -> HighIR | HighIR o C strutturato |
| `lift` | Binary -> LowIR -> MedIR -> LLVM IR | `.ll` |
| `decompile --llvm` | Binary -> LowIR -> MedIR -> LLVM IR | C derivato da LLVM |
| `patch` | Binary -> LowIR -> MedIR -> LLVM IR -> codegen | Binario riscritto |

`lib/pipeline/Pipeline.cpp` è la fonte autorevole per la scelta del percorso.
Mantieni la logica specifica di una rappresentazione nella relativa libreria IR
o backend; il pipeline deve orchestrare i componenti, non assorbirne gli
algoritmi.

## Contratto di traduzione tra architetture

`include/neverd/translate` definisce un livello contrattuale, non un
backend di esecuzione. `GuestState` modella lo stato visibile alla macchina in
modo indipendente dall’architettura per `x86_32`, `x86_64`, `AArch64` e `ARM32`.
La sua serializzazione canonica versione 1 usa campi little-endian a larghezza
fissa, ID di registro stabili, raccolte ordinate e validazione fail-closed;
lo stato persistito non dipende quindi dal layout C++ dell’host.

La baseline wire v1 di `GuestState` è congelata in modo permanente. Ogni stato
esterno a tale baseline deve usare un ID di registro di estensione nell’intervallo
riservato insieme a un nome canonico minuscolo, oppure passare a una nuova
versione wire con un upgrader esplicito; è vietato modificare in-place la
baseline v1.

Per un guest `ARM32`, `ExecutionMode` è la modalità di decodifica autorevole e
deve essere coerente con `CPSR.T`. Il PC memorizzato è sempre l’indirizzo
canonico dell’istruzione con il bit 0 azzerato; la modalità ARM richiede inoltre
l’allineamento alla parola.

Il contratto delle coppie definisce `x86_64 -> AArch64`,
`AArch64 -> x86_64`, `x86_32 -> AArch64/ARM32` e
`ARM32 -> x86_32/x86_64`. `ContractDefined` significa che una richiesta può
essere validata e persistita, non che il codice possa essere tradotto o eseguito.
La policy JIT accetta solo l’host nativo del processo; la policy AOT richiede
un’architettura host e un target triple espliciti; anche una CPU o un insieme di
feature selezionati devono essere espliciti.

`ResolvedHostTarget` trasforma questa selezione in un risultato concreto. La
risoluzione `Native` ricava dal processo triple, CPU e insieme di feature
abilitate o disabilitate. La risoluzione `Explicit` valida e normalizza
architettura, triple, CPU e feature forniti dal chiamante e rifiuta i conflitti.
La sua identità di cache versionata è costruita in ordine di byte deterministico
dagli input target normalizzati e non contiene indirizzi di processo né testo
dipendente dalla locale.

Un `TranslationExit` versionato registra una causa di arresto stabile e il
payload tipizzato corrispondente per syscall, eccezioni o segnali, breakpoint,
istruzioni non supportate, automodifica, budget di risorse, chiamate esterne,
fault di memoria e altre condizioni terminali. I consumer non devono quindi
reinterpretare un intero privo di tipo in base alla causa di arresto.

Salvo il caso `BudgetExhausted` corrispondente, i conteggi di istruzioni, blocks
e codice generato non devono superare il budget non nullo della richiesta.
L’esaurimento di istruzioni e blocks si arresta esattamente al limit. La
dimensione di un oggetto generato è nota solo dopo un codegen indivisibile;
quindi il relativo risultato può indicare `Observed > Limit`. L’oggetto rifiutato
non viene mai collegato, pubblicato o eseguito. Ogni payload `BudgetExhausted`
identifica esattamente il limit richiesto, mai una soglia derivata o privata
dell’implementazione.

Il contratto backend-private `RuntimeControlBlockV1` misura
esattamente 128 byte, è allineato a 8 byte ed è vincolato da magic, version,
size e offset dei campi fissi della v1, campi riservati a zero ed exit tipizzate
coerenti. Non contiene container C++, puntatori host né alias di indirizzi guest.
Non è il layout C++ né il formato wire di `GuestState`; un backend che implementa
questo contratto deve convertire esplicitamente lo stato in questo record.

La superficie fissa di chiamata v1 del codice generato contiene esattamente otto
helper: `nvd_rt_v1_load8_le`, `nvd_rt_v1_load16_le`, `nvd_rt_v1_load32_le`,
`nvd_rt_v1_load64_le`, `nvd_rt_v1_store8_le`, `nvd_rt_v1_store16_le`,
`nvd_rt_v1_store32_le` e `nvd_rt_v1_store64_le`. Nomi, firme e provenienza dei
puntatori devono corrispondere esattamente; un backend collega esplicitamente
questa tabella finita e non ripiega mai sulla risoluzione ambientale dei simboli.
La validazione della generation eseguibile e il polling di budget/cancellazione
sono operazioni riservate al dispatcher fidato; `nvd_rt_v1_validate_generation`
e `nvd_rt_v1_poll` non sono helper del codice generato. Il dispatcher host fidato
possiede anche la selezione dei blocks e non è invocabile dall’IR generato; i
translated blocks restituiscono invece un codice di exit tipizzato. L’IR
generato può leggere direttamente solo lo slot runtime scalar-result dichiarato.

`RuntimeSymbolRegistryV1` realizza questa tabella di helper come registro host
chiuso. La costruzione valida l’intero insieme ABI-v1, i nomi canonici esatti, le
classi degli helper, le firme e, per ogni voce, esattamente un puntatore a
funzione non nullo coerente con la classe. La ricerca accetta solo il nome
esatto, non consulta mai simboli ambientali del processo o del loader dinamico e
fornisce al verifier degli oggetti gli stessi nomi ordinati come allowlist. La
sua identità versionata copre nomi, classi degli helper e forma dell’ABI, ma
esclude deliberatamente gli indirizzi nativi ed è quindi stabile con ASLR.

`RuntimeCodeMemory` possiede storage per codice generato isolato per pagina e
consente solo la pubblicazione unidirezionale `RW -> RX`. La memoria non è mai
scrivibile ed eseguibile allo stesso tempo, non può essere riaperta in scrittura,
controlla i limiti di scritture ed entry point e invalida la cache delle
istruzioni host al momento della pubblicazione. Lo smoke test nativo esegue solo
una breve sequenza di istruzioni host dopo la pubblicazione: dimostra questo
confine di memoria W^X, non un motore di traduzione.

`GuestMemoryRuntime` è isolato dal `GuestState` logico: la costruzione prima
valida lo stato, quindi copia byte e metadati delle regioni in un indice privato
ordinato. Gli indirizzi virtuali guest sono soltanto chiavi di ricerca e non
vengono mai convertiti in puntatori host. Gli accessi scalari controllati
segnalano fault tipizzati per larghezza, allineamento, overflow, assenza di
mapping, attraversamento di regione, permessi, scrittura eseguibile, overflow o
discordanza di generation e violazione di policy. I budget di istruzioni/blocks,
la cancellazione, il tracking della generation e le policy di scrittura del
codice `RejectExecutableWrites`, `InvalidateOnExecutableWrite` e
`ValidateBeforeDispatch` producono anch’essi record tipizzati coerenti invece
di comportamento host implicito.

`TranslationObjectCompilerV1` è il confine verificato da LLVM IR a oggetto.
Valida un modulo di input const, lo clona prima di ogni trasformazione, compone
la semplificazione semantica proof-gated con l’ottimizzazione LLVM da `O0` a
`O3`, convalida di nuovo l’IR finale ed emette oggetti relocatable ELF, COFF o
Mach-O per le quattro architetture host del contratto. Canonicalizza i manifest
esatti dei blocks e dei simboli runtime dopo il mangling del target, controlla
ogni oggetto emesso e restituisce l’identità del registro runtime insieme a
chiavi di cache versionate per richiesta e artefatto. Con un budget di byte
generati diverso da zero, solo un oggetto conforme può passare alla verifica
dell’artefatto. LLVM emette prima in un buffer privato per misurare la dimensione
esatta e indivisibile; un oggetto sovradimensionato viene rifiutato prima della
pubblicazione e dell’audit, mentre la telemetria tipizzata conserva la dimensione
osservata e il limit esatto richiesto. Zero significa nessun limite imposto dal
chiamante. Il compilatore si ferma ai byte relocatable controllati: non li
collega, pubblica, invia al dispatcher o esegue e non fornisce il lowering delle
istruzioni guest.

Il verifier post-codegen controlla gli oggetti relocatable ELF,
COFF e Mach-O come insieme chiuso. Formato e architettura devono corrispondere
esattamente all’host selezionato; i simboli non definiti devono appartenere
esattamente alla allowlist finita degli helper e i simboli dinamici sono vietati.
Le relocations seguono whitelist dirette esplicite con controlli di encoding,
larghezza, allineamento, offset, destinazione caricabile e target definito
nell’oggetto come non-preemptible o helper autorizzato esattamente. Sono
rifiutati W+X, metadati unwind/exception/initializer, TLS, IFUNC, GOT e
l’indirezione PLT ordinaria, relocations dinamiche, definizioni weak/preemptible
o selezionabili, sezioni allocate sconosciute e direttive del linker. La forma
`R_X86_64_PLT32` usata da LLVM per una chiamata ELF x86-64 hidden è ammessa solo
quando la policy v1 dimostra un branch diretto sealed verso l’helper runtime
esatto; non autorizza un percorso PLT o GOT. Gli artefatti ELF `ET_REL` non
possono contenere program header o segmenti. I load command Mach-O seguono una
lista positiva: esattamente un segmento della larghezza corretta e al massimo
una symbol table, dynamic-symbol table, platform-version e un comando
data-in-code, con verifica delle dipendenze. Le opzioni del linker e ogni altro
command vengono rifiutati.

`TranslationObjectRequestV1` è la prima fase pubblica, volutamente ristretta,
che trasforma byte guest in un oggetto su questi contratti. Nel sottoinsieme v1
fail-closed pubblicato per i registri scalari x86-64 accetta soltanto codifiche
canoniche prive di prefissi legacy: forme `MOV`, `ADD`/`SUB` e
`AND`/`OR`/`XOR` con REX.W su GPR a larghezza intera i cui operandi hanno le
forme LowIR registro/immediato supportate. Le forme aritmetiche mantengono i
relativi calcoli dei flag scalari; quelle logiche e `TEST` calcolano i flag
definiti dall’architettura preservando `AF` nel modello di stato NeverD. Lo
schema 9 accetta anche `CMP` register/register a larghezza intera con `39/3B`,
`CMP` register/immediate con `81/7`, `83/7` e `3D`, `TEST` a larghezza intera
register/register con `85` e register/immediate con `F7/0` e `A9`. Le codifiche
canoniche `C3` `RET` e `C2 iw`
`RET imm16` terminano i block di ritorno; le codifiche `JMP` direct-relative
canoniche `EB cb` ed `E9 cd` terminano i block di branch diretto. Lo schema di
lowering pubblicato è 9. I branch Jcc tradizionali, canonici e senza prefisso
legacy sono limitati a: `JO`/`JNO` short `70/71 cb` o near `0F 80/81 cd`;
`JB`/`JAE` con `72/73 cb` o `0F 82/83 cd`; `JE`/`JNE` con `74/75 cb` o
`0F 84/85 cd`; `JBE`/`JA` con `76/77 cb` o `0F 86/87 cd`; `JS`/`JNS` con
`78/79 cb` o `0F 88/89 cd`; `JP`/`JNP` con `7A/7B cb` o `0F 8A/8B cd`;
`JL`/`JGE` con `7C/7D cb` o `0F 8C/8D cd`; e `JLE`/`JG` con `7E/7F cb` o
`0F 8E/8F cd`. `JRCXZ`/`JECXZ`/`JCXZ` e `LOOP`/`LOOPE`/`LOOPNE` restano non
pubblicati e falliscono fail-closed. Anche `F7 /1` riservato, gli operandi di
memoria guest, i registri parziali, i prefissi legacy e i bit di estensione REX
semanticamente ridondanti falliscono fail-closed. Emette
esclusivamente un oggetto relocatable ELF o Mach-O AArch64 little-endian
sottoposto ad audit. Le normali operazioni sulla memoria guest, le forme a
registro parziale, qualsiasi istruzione o codifica al di fuori di questo
sottoinsieme esatto, i flussi di controllo diversi dai ritorni, da questi salti
diretti e dai branch su un singolo flag pubblicati sopra, e ogni operazione
LowIR non implementata dal lowerer vengono rifiutati prima dell’emissione. La
lettura controllata dell’indirizzo di ritorno richiesta da
`RET` fa parte del suo contratto di terminazione e non pubblica un lowering
generale della memoria guest. La richiesta ricostruisce e convalida il
descrittore del block, usa la stessa target machine risolta per lowering ed
emissione dell’oggetto e combina la semplificazione semantica proof-gated con la
pipeline di ottimizzazione `O2` predefinita di LLVM. Questa fase non copre altre
istruzioni x86-64, altre coppie guest/host o la direzione inversa da AArch64 a
x86-64.

L’entry point C pubblico
`neverd_translate_x86_64_block_to_aarch64_object_v1`, il wrapper Python ctypes
`translate_x86_64_block_to_aarch64_object` e il comando
`neverd translate-object` espongono lo stesso confine limitato all’oggetto.
Python usa `TranslationObjectFormat.ELF` o `.MACHO`. Gli errori della traduzione
nativa sollevano una `TranslationError` tipizzata che contiene
`TranslationErrorCode`; la validazione locale degli argomenti solleva invece
`TypeError` o `ValueError`. In caso di successo Python restituisce un risultato
immutabile di sua proprietà. Il risultato C possiede i byte dell’oggetto, le
identità di cache stabili e la telemetria di ottimizzazione; la CLI scrive
soltanto l’oggetto ELF o Mach-O selezionato. Tutte e tre le superfici terminano
prima di linking,
caricamento, dispatch, esecuzione e debugging; non sono interfacce di sessione
di esecuzione.

`verifyTranslationLinkGraphV1` aggiunge un secondo audit indipendente prima di qualsiasi
allocation. Costruisce un grafo LLVM JITLink effimero da un oggetto ELF o Mach-O
AArch64 accettato e verifica target, permessi delle sezioni, manifest dei simboli
block/runtime, chiusura dei simboli esterni, tipi e destinazioni degli edge. Il
grafo viene distrutto dopo aver prodotto il risultato di audit privo di
indirizzi. Superare questo audit non collega, alloca, risolve, carica, pubblica,
invia al dispatcher né esegue codice.

`linkTranslationObjectV1` è il confine separato di linking nativo. Riesegue
l’audit del descrittore fidato, dell’oggetto grezzo e del grafo JITLink prima e
dopo pruning, allocation, risoluzione dei simboli e fixup. I simboli runtime
provengono soltanto dal registro sealed. Una credential del dispatcher lega
l’unica voce del manifest alla relativa session, all’identità del block, al PC
di ingresso guest, alla generazione della cache e all’epoca del codice;
l’invocazione richiede inoltre che il `RIP` guest del runtime corrisponda a tale
ingresso. Dopo la finalizzazione riuscita, pubblica memoria eseguibile con le
permission finali. Unload revoca le nuove invocazioni e attende un’invocazione
attiva prima di liberare l’allocation. L’overload senza credential resta
audit-only e non può invocare.

`NativeTranslationSessionV1` compone questi elementi nel confine sperimentale
di esecuzione C++ da x86-64 ad AArch64 nativo. In un processo ELF o Mach-O
AArch64 little-endian conserva lo stesso runtime di memoria guest controllato e
lo stesso stato guest fisso tra più block di un loop dispatcher
compile-link-validate-invoke-unload. Un salto diretto canonico continua al suo
target statico esatto. Un branch canonico pubblicato su un singolo flag continua
soltanto nel successore taken o fallthrough dichiarato dal manifest del block; il
dispatcher rifiuta qualsiasi altro PC selezionato. Un ritorno termina. I budget
globali per istruzioni, block e byte di oggetto generati restano esatti tra i
block. Quando il guest si arresta con successo, lo stato eseguito e la memoria
autorevole vengono committati insieme. La cancellazione è linearizzata rispetto
a questo commit finale.

Questa è una vertical slice eseguibile, non un traduttore completo. Non copre
ancora normali istruzioni di memoria guest, registri parziali, flusso di
controllo condizionale al di fuori dello slice esatto schema-9 dei Jcc
tradizionali descritto sopra, inclusi `JRCXZ`/`JECXZ`/`JCXZ` e
`LOOP`/`LOOPE`/`LOOPNE`, flusso
di controllo indiretto, call, virgola mobile, SIMD, x87, operazioni atomiche,
istruzioni di sistema, propagazione generale delle eccezioni, cache dei block,
altre coppie guest/host o la direzione inversa da AArch64 a x86-64.
La sessione di esecuzione non ha ancora superfici C, Python, CLI o JSON; il
debugging resta separato e non supportato. Le API oggetto precedenti restano
utilizzabili senza abilitare l’esecuzione nativa.

Il contratto dell’IR generato richiede che ogni translated block soggetto ad
esso sia hidden e non-preemptible e usi il C ABI
`i32 (ptr state, ptr runtime)`. I blocks sono individuabili solo tramite un
registro privato, mai tramite la ricerca dei simboli del processo circostante;
le chiamate dirette tra blocks sono vietate.

L’IR verifier limita inoltre la larghezza degli interi alla larghezza del
registro scalare dell’host, per evitare compiler-runtime libcalls noti introdotti
dalla legalization. Questa verifica è necessaria, ma non sufficiente: ogni
backend di esecuzione che implementa questo contratto deve controllare in modo
esatto i trasferimenti di controllo post-codegen, il `MachineIR` e le relocations
dell’oggetto target rispetto alla stessa runtime-symbol allowlist finita.

I load e store diretti di TranslationIR, insieme ai valori delle private
constants, possono contenere solo un singolo intero scalare non più largo del
registro scalare dell’host. Gli aggregati devono essere scalarizzati prima del
confine del verifier, così un IR compatto non può causare un’espansione non
limitata nel backend.

L’ABI del codice generato è definita solo per interi scalari. Virgola mobile,
SIMD, x87, operazioni atomiche e istruzioni di sistema restano fuori da questo
contratto. Ogni implementazione che seleziona `ProvenSemanticAndLLVM` deve
eseguire la semplificazione semantica di NeverD, subordinata a prova, fino a un
fixed point congiunto con l’ottimizzazione LLVM; la policy non fornisce un
backend di traduzione eseguibile.

## Confini della riscrittura delle eccezioni

Il compact unwind Mach-O dispone di un parser rigoroso del `__unwind_info`
originale, di un parser consapevole dei fixup per i record
`__LD,__compact_unwind` generati, di un merge esatto degli intervalli originali
e generati, di un encoder deterministico per le pagine regolari e di un
installer transazionale della sezione finale. L’installer riscrive in-place una
`__TEXT,__unwind_info` esistente e file-backed solo quando la tabella codificata
rientra nella capacità dichiarata. Rivalida architettura, layout e byte preimage,
azzera la coda inutilizzata, quindi esegue nuovamente il parse del risultato e
ne prova l’equivalenza semantica prima dell’unico commit della transazione Mach-O
esterna. Se la sezione finale è assente, i record compact generati non vengono
installati e la transazione può proseguire solo tramite la chiusura DWARF-FDE
esatta e autenticata descritta sotto; una sezione finale esistente ma
insufficiente o malformata continua a fallire in modalità fail-closed. I record
generati sono autenticati tramite un’associazione esatta,
registrata dal compiler, tra funzione IR sorgente e owner symbol MC di destinazione
(incluse le definizioni private, senza ipotesi su prefissi o mangling), ID di
intervallo opachi e diversi da zero e intervalli di frammento semiaperti esatti.
Ogni FDE generato deve corrispondere esattamente a un solo frammento autenticato;
ogni frammento richiesto deve corrispondere a un solo FDE installato dalla stessa
transazione, salvo che sia coperto da un record compact non-DWARF esatto e
rigorosamente validato. Frammenti adiacenti o disgiunti dello stesso owner di
funzione possono riutilizzare una ricetta sorgente; identità mancanti, duplicate,
dangling, cross-owner o con limiti incoerenti falliscono prima della modifica. Il
nuovo segmento RX viene confermato solo dopo aver provato un `__LINKEDIT` unico e
terminale nel file e nello spazio VM, shift degli offset con aritmetica controllata
e un replay rigoroso del layout finale.

Nel compact unwind ARM32, lo stack adjustment codificato e il layout GPR sono
`Complete`. Anche i selettori di pattern dei registri D da 0 a 3 sono `Complete`;
quelli da 4 a 7 sono `Partial` perché il compact word da solo non prova ogni slot
relativo al CFA allineato a runtime. Una voce `Partial` può conservare le identità
dei registri provate per l’analisi, ma ogni percorso di rewrite la rifiuta
fail-closed. Ogni receipt di installazione EH-frame lega esattamente target
architecture, pointer width e byte order; il binding DWARF compact-unwind rifiuta
ogni target identity del receipt non corrispondente. Manca ancora una prova
native throw/catch su un binario collegato.

La transazione di sezione ARM32 di livello superiore è più ristretta del
decoder compact unwind. Viene abilitata solo quando l’header Mach-O è
esattamente `CPU_SUBTYPE_ARM_V7K` e i bit `N_ARM_THUMB_DEF` della symbol table
originale autenticano positivamente ogni funzione richiesta come codice Thumb.
Il triple esatto `thumbv7k-apple-watchos` e la modalità Thumb rimangono quindi
vincolati per tutta la code generation, i cui requisiti di feature in input non
possono superare il limite Cortex-A7. Funzioni prive di flag o con modalità
sconosciuta, sottotipi generici non-v7k, modalità ARM, target di codice esterno
misti o sconosciuti, l’entry point in-place per ARM Mach-O e il patch ARM Mach-O
da sorgente C falliscono in modalità fail-closed prima di modificare l’output.
Gli input stripped le cui funzioni siano individuabili soltanto tramite
`LC_FUNCTION_STARTS` non sono ancora supportati.

PE, ELF e Mach-O dispongono ciascuno di componenti delle eccezioni specifici del
formato, ma NeverD non pubblica ancora una pipeline di riscrittura end-to-end
per tutti i formati e tutti i tipi di eccezione. Un encoding non supportato o
requisiti di registration/layout non risolti devono fallire prima di modificare
l’output; il supporto parziale esistente non deve essere descritto come chiusura
completa delle eccezioni.

## Mappa dei componenti

Ogni componente è un archivio statico creato da
`add_neverd_component_library`. La tabella elenca le dipendenze NeverD
importanti, non tutte le librerie LLVM e Capstone comuni fornite dall’helper
CMake.

| Directory | Responsabilità | Dipendenze importanti |
|-----------|----------------|-----------------------|
| `lib/loader` | Rilevamento formato, caricamento PE/COFF, ELF e Mach-O, `BinaryImage` normalizzata, scoperta funzioni | API LLVM Object |
| `lib/lift` | Semantica scritta a mano per istruzioni x86/i386, AArch64 e ARM32 | Tipi di dati IR |
| `lib/decode` | Decodifica Capstone/native e dispatch ai lifter di architettura | `NeverDIR`, `NeverDLift` |
| `lib/ir` | Tipi comuni e definizioni/trasformazioni LowIR, MedIR, HighIR e intrinsic | Quattro sottocomponenti IR |
| `lib/pipeline` | Rilevamento funzioni e orchestrazione dei percorsi Low/Med/High/LLVM | IR, decode, lift, backend LLVM, debug info, pass IR |
| `lib/backend/c` | Rendering HighIR-to-C e LLVM-IR-to-C | IR |
| `lib/backend/llvm` | Lowering da MedIR a LLVM | IR |
| `lib/backend/codegen` | Generazione codice target e patch/riscrittura in-place PE/ELF/Mach-O | IR, loader |
| `lib/sdk` | ABI C pubblica, ciclo session, query, persistenza, plugin, punti lift/decompile/patch | Aggrega il motore in `libneverd` |
| `lib/pass` | Pass di offuscamento LLVM IR e runner di pass MIR | IR |
| `lib/debug` | Contesti di debug DWARF, PDB e linker-map | IR |
| `lib/sigs` | Parsing, database e matching delle firme | Loader |
| `lib/libc` | Nomi libc noti e supporto del modello di chiamata | Componente autonomo |
| `lib/support` | Helper condivisi per il caricamento binario | Loader |
| `lib/translate` | Contratti versionati per guest state/policy/exit, runtime ABI fissa, guest memory controllata, audit di IR/oggetti/LinkGraph generati, linking nativo sealed e dispatcher C++ sperimentale da x86-64 ad AArch64 | Contratti IR, LLVM, LLVM Object e JITLink |

Gli header pubblici rispecchiano queste aree sotto `include/neverd`. Evita che
una classe C++ interna diventi accidentalmente parte dell’SDK: le operazioni
esterne stabili appartengono all’header C puro e a uno dei file mirati
`lib/sdk/NeverDCAPI*.cpp`.

## Contratto di lifting strict

`Decoder` e ogni lifter di architettura partono in modalità strict. Se Capstone
può decodificare un’istruzione ma il lifter selezionato non la implementa,
lancia `UnliftedInstruction`. L’eccezione registra indirizzo, mnemonico e
operandi; la semantica non supportata deve quindi fallire visibilmente invece di
essere omessa o ipotizzata.

Il percorso interno non strict emette `NdOp::NOP`, ma è una via di fuga
diagnostica, non un’implementazione accettabile. I test dei contributor e della
CI devono mantenere la modalità strict. Quando si verifica un errore strict:

1. Riproducilo con la fixture specifica dell’architettura più piccola.
2. Aggiungi la semantica mancante in `lib/lift/<ISA>`.
3. Verifica la forma LowIR prevista in `unittests/lift`.
4. Aggiungi un roundtrip differenziale Unicorn in `unittests/semantic` se l’istruzione ha un comportamento osservabile.

Non intercettare `UnliftedInstruction` solo per far proseguire il pipeline. Una
nuova approssimazione intenzionale richiede contratto e test espliciti; non deve
fingersi lifting 1:1.

## Proprietà di formati e ISA

La logica del formato in ingresso e quella di riscrittura in uscita sono
separate intenzionalmente:

| Formato | Caricamento, metadati e relocation di input | Patch e relocation di output |
|---------|---------------------------------------------|------------------------------|
| PE/COFF | `lib/loader/COFF` | `lib/backend/codegen/COFF` |
| ELF | `lib/loader/ELF` | `lib/backend/codegen/ELF` |
| Mach-O | `lib/loader/MachO` | `lib/backend/codegen/MachO` |

I lifter di architettura risiedono in `lib/lift/X86`, `lib/lift/AArch64` e
`lib/lift/ARM`. Le dichiarazioni pubbliche di lifter/register sono in
`include/neverd/lift`. L’emissione LLVM e la generazione di codice specifiche
del target si trovano in `lib/backend/llvm/<ISA>` e
`lib/backend/codegen/CodeGen<ISA>.cpp`.

<a id="support-and-test-depth"></a>

### Supporto e profondità dei test

La matrice di supporto principale indica che ogni cella è implementata. Non
significa che ogni opcode, caso limite ABI, produttore binario o versione del
sistema operativo sia stato testato in modo esaustivo. La modalità strict
fallisce in modo chiuso quando la semantica di un’istruzione è fuori dalla
copertura implementata dal lifter.

Tutte le 12 celle formato-per-architettura hanno copertura semantica del backend
di riscrittura in `unittests/semantic/PatchFullSubstRTTests.cpp`. La profondità
di integrazione è più specifica:

| Formato | x86-64 | i386 | AArch64 | ARM32 |
|---------|--------|------|---------|-------|
| PE/COFF | Fixture collegata | Griglia backend | Fixture collegata | Fixture Thumb collegata |
| ELF | Fixture collegata + roundtrip semantico | Pipeline oggetto + roundtrip semantico | Fixture collegata + roundtrip semantico | Fixture collegata + roundtrip semantico |
| Mach-O | Fixture collegata\* | Pipeline oggetto PIC/no-PIC\* | Fixture collegata\* | Griglia backend |

- Una **fixture collegata** esercita loader/pipeline e patch su un eseguibile
  collegato per programmi rappresentativi.
- Una **pipeline oggetto** esercita caricamento, tutte le fasi IR e
  decompilazione di un oggetto rilocabile, ma non il linking host né
  l’esecuzione del binario patchato.
- Una **griglia backend** compila IR rappresentativo attraverso il percorso
  esatto di generazione per riscrittura e confronta il comportamento in
  Unicorn; non esercita il loader del formato su un eseguibile collegato.
- `*` Le fixture Mach-O collegate dipendono da una toolchain host capace di
  produrre il target. macOS moderno non collega eseguibili i386 storici; si
  usano quindi oggetti thin PIC/no-PIC e la griglia di riscrittura.

Le celle con fixture collegata sono la prova più forte di integrazione
del formato per quei programmi. Le celle pipeline oggetto e griglia backend
hanno solo copertura parziale di integrazione. Nessuna cella è «completamente
testata» senza questa precisazione né pretende copertura esaustiva dell’ISA.

Le prove principali sono
[`PatchFormatTests.cpp`](../unittests/lift/format/PatchFormatTests.cpp) per fixture ELF
e PE collegate,
[`COFFARMFormatTests.cpp`](../unittests/lift/format/COFFARMFormatTests.cpp) per
caricamento/decompilazione Windows ARM,
[`MachOI386RelocationTests.cpp`](../unittests/lift/format/MachOI386RelocationTests.cpp)
per oggetti thin i386,
[`X86_64_PipelineE2ETests.cpp`](../unittests/lift/x86_64/X86_64_PipelineE2ETests.cpp) e
[`AArch64_PipelineE2ETests.cpp`](../unittests/lift/aarch64/AArch64_PipelineE2ETests.cpp)
per Mach-O collegato, e
[`PatchFullSubstRTTests.cpp`](../unittests/semantic/probe/patchfull/PatchFullSubstRTTests.cpp)
per la griglia di 12 celle. Consulta la [guida ai test](testing.it.md).

## Dove intervenire

| Modifica | Punto di partenza | Verifica minima mirata |
|----------|-------------------|------------------------|
| Aggiungere o correggere un’istruzione | File corrispondenti in `lib/lift/X86`, `AArch64` o `ARM`; header pubblico se cambia il dispatch | Test di architettura in `unittests/lift`; roundtrip semantico in `unittests/semantic` |
| Aggiungere un `NdOp` | `include/neverd/ir/NdOps.h`, poi verifica Low-to-Med, emitter/renderer, verifier/emulator e dump | `NeverDLiftTests` + casi pertinenti di `NeverDSemanticTests` |
| Modificare CFG o scoperta funzioni | `lib/ir/low`, `lib/loader/FunctionDiscovery*.cpp`, `lib/pipeline/PipelineFuncDetect.cpp` | Test lift CFG/jump-table e suite di trasformazione semantica mirata |
| Aggiungere relocation input o regola unwind PE | `lib/loader/COFF` | `COFFARMFormatTests` o nuova fixture loader mirata |
| Aggiungere relocation output o regola patch PE | `lib/backend/codegen/COFF` | `PatchFormatTests`, `RewriteCodegenRTTests` e griglia backend PE |
| Modificare comportamento ELF o Mach-O | Directory `lib/loader/<Format>` e/o `lib/backend/codegen/<Format>` corrispondenti | Test del formato più griglia di riscrittura |
| Modificare recupero MedIR/ABI | `lib/ir/med` | Test lift delle convenzioni di chiamata + roundtrip semantici multi-ISA |
| Modificare recupero del controllo strutturato | `lib/ir/high` | `NeverDCFGLoopXformTests` e test C strutturato |
| Aggiungere trasformazione LLVM | `lib/pass/ir`, header pubblico in `include/neverd/pass/ir`, toggle pipeline se esposto | Suite di trasformazione mirata + `NeverDPatchFullTests` se cambia l’output patch |
| Aggiungere operazione C API | `include/neverd/sdk/NeverDCAPI.h`, `lib/sdk/NeverDCAPI*.cpp` mirato, `SessionImpl.h` solo per stato | Test semantici SDK/CLI; preservare `neverd_last_error` e convenzioni di allocazione |
| Aggiungere comando CLI | `tools/neverd/NeverDCLIOptions.cpp`, `NeverDCLI.h`, `NeverDCmd*.cpp` mirato e dispatch in `neverd.cpp` | `unittests/semantic/CLIEndToEndTests.cpp` e smoke test CLI diretto |
| Aggiungere regressione semantica | `unittests/semantic/*Tests.cpp` mirato; registrare il nuovo file in `unittests/semantic/CMakeLists.txt` | Costruire il binario di test e selezionare il caso con `ctest -R` |

Mantieni le modifiche ristrette. I file che definiscono una rappresentazione
possono cambiare con le relative trasformazioni, ma loader, lifter e backend non
correlati non vanno modificati solo per uniformare un refactoring ampio.
