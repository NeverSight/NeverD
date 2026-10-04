**Lingue**: [English](../testing.md) | [简体中文](../zh-CN/testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](../ja/testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](../de/testing.md) | [Español](../es/testing.md) | [Italiano](testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← Indice della documentazione](README.md)

# Testare NeverD

I test di NeverD rispondono a tre domande diverse: una rappresentazione ha la
forma prevista, un percorso completo funziona per una fixture binaria e il
codice generato conserva il comportamento? Scegli la suite minima che risponde
alla domanda della modifica, poi esegui l’aggregato più ampio prima di una pull
request ad alto rischio.

## Configurare una build di test

I test sono disabilitati se non si abilita `BUILD_TESTING`. Release è la scelta
normale per la suite completa; Debug mantiene assertion ed esecuzione
passo-passo, ma è intenzionalmente non ottimizzato e non rappresentativo per i
benchmark di decode.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

Il set completo di fixture richiede `clang` per la compilazione multi-target e
i linker LLVM (`ld.lld` e `lld-link`) nel `PATH`. CMake genera sempre molti
oggetti rilocabili e fixture ELF/PE collegate quando è disponibile il linker
corrispondente. Un test ignorato perché l’host non può compilare o collegare la
fixture è copertura non eseguita, non un superamento per quel target.

Consulta [CONTRIBUTING.md](CONTRIBUTING.md) per clonazione, profili di
build e LLVM precompilato su macOS.

## Verifiche del recupero degli interpreti

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests \
  NeverDDevirtualizationSourceTests --parallel 4
ctest --test-dir build-release -L '^NeverD(InterpreterSpecialization|DevirtualizationSource)Tests$' \
  --output-on-failure
```

```sh
cmake --build build-release --target NeverDLowIRUndefinedIndependenceTests \
  NeverDOriginalBinaryUndefinedIndependenceTests \
  NeverDX86UndefinedEffectsTests NeverDX86CarryArithmeticFlagTests \
  NeverDX86LogicIdentityTests NeverDX86NoIndexAddressTests --parallel 4
build-release/bin/NeverDLowIRUndefinedIndependenceTests
build-release/bin/NeverDOriginalBinaryUndefinedIndependenceTests \
  --gtest_filter='OriginalBinaryUndefinedIndependence.*'
build-release/bin/NeverDX86UndefinedEffectsTests
build-release/bin/NeverDX86CarryArithmeticFlagTests
build-release/bin/NeverDX86LogicIdentityTests
build-release/bin/NeverDX86NoIndexAddressTests
```

I test API coprono valori predefiniti v1/v2/v3, budget espliciti, strutture troncate, tutti i campi reserved e code future. I test CLI verificano esaurimento e recupero riuscito con entrambe le ABI e i backend, rifiutano limiti decimali non validi e richiedono `--devirtualize`. L’esaurimento non deve pubblicare sorgenti o grafi residui parziali.

I test v4 fissano dimensioni e padding dei prefissi, rifiutano strutture troncate e flag sconosciuti, mantengono i formati precedenti/futuri e conservano i limiti nel C anche senza rapporto. Esempi CLI indipendenti richiedono il concatenamento per le correlazioni e i limiti per un confronto di stack senza segno; entrambi i backend C eseguono O0/O2 con trap per comportamento indefinito. Disabilitare la scoperta deve cambiare un risultato che ne dipende. Il parser verifica zero, estremi interi, overflow, intervalli malformati e prerequisiti mancanti. Python verifica layout, flag, firme e proprietà dei rapporti di errore.

`NeverDByteMemoryForwardingTests` copre ultime scritture sovrapposte, entrambi gli ordini dei byte, larghezze multiple di otto fino a i128, valori definiti, snapshot undef/poison correlati e sovrascritture parziali. I casi negativi mantengono le letture con alias sconosciuti, conversioni di spazio di indirizzi, chiamate, accessi ordinati, cambi di durata, byte mancanti, offset dinamici o invalidi, rami e cicli. Verifica anche PHI con molti ingressi, budget nulli/esatti/esauriti e il rifiuto predefinito degli snapshot. LLVM originale e riscritto vengono eseguiti a O0/O2 contro un riferimento aritmetico indipendente; le suite MBA, LLVMC e dei sorgenti degli interpreti proteggono la compatibilità.

Le nuove regressioni mantengono gli intrinsic di operazioni pure sui bit in entrambe le modalità di memoria e gli ordini dei byte. Chiamate ordinarie, operand bundle, convergent, durata degli oggetti, effetti sulla memoria e trap restano barriere. Il codice originale e trasformato viene eseguito a O0/O2 con trap per comportamento indefinito contro un riferimento indipendente che controlla l’indirizzo numerico nel risultato e ogni byte del buffer.

Le regressioni di vitalità dei byte coprono letture disgiunte, scritture parzialmente osservate e copertura mediante più scritture. Gli indirizzi condizionati includono AND/OR/XOR, 32/64 bit, entrambi gli ordini dei byte, radici errate, maschere incomplete, confluenze che aggirano il controllo e tutti i limiti di budget. Gli oracoli indipendenti O0/O2 verificano entrambi i rami, i sedici residui di indirizzo e ogni byte del buffer.

Le regressioni degli indirizzi verificano PHI/select di interi e puntatori, entrambi gli ordini dei byte e le larghezze dei puntatori, ritorni di ciclo stabili e variabili, undef/freeze, cicli senza ancoraggio, budget adiacenti e invalidazione per sole modifiche di indirizzo. I cicli O0/O2 confrontano ogni risultato e byte del buffer con un oracolo indipendente per iterazione, compreso un indirizzo mobile che non va considerato costante.

I test numerici verificano letture complete e contenute da un solo writer, undef/poison senza nuovi snapshot, entrambi gli ordini di byte, 32/64 bit, offset negativi modulari, sovrapposizioni parziali, alias fra radici e alloca, eccezioni, cicli, limiti adiacenti del budget e invalidazione quando si eliminano solo store. O0/O2 confrontano originale e trasformazione con un oracolo indipendente per il risultato e ogni byte del buffer con alias.

`NeverDMedMutableSourceTests` e `NeverDLLVMCValueTests` eseguono a O0/O2 cicli indipendenti, blocchi riordinati, archi di ritorno all’ingresso, aritmetica dello stack a runtime, letture precedenti, confluenze, alias parziali, valori booleani e conteggi di bit incluso lo zero. I casi negativi rifiutano input malformati, destinazioni troncate, portatori ambigui e budget esauriti prima dell’emissione. Un caso CLI oltre il limite SSA richiede LLVMC eseguibile e un rifiuto esplicito da HighC. Gli aggiornamenti ripetuti e le catene di espressioni memorizzate tra blocchi verificano anche la dimensione e l’esecuzione del C generato.

Ulteriori regressioni limitano letture e scritture private prima della promozione LLVM e la dimensione del C. Eseguono a O0/O2 lunghe catene aritmetiche miste, blocchi SSA riordinati, scritture di memoria sovrapposte e ritorni zero. I casi principali attraversano anche la pipeline LLVM effettiva; viene verificato il riuso dell’emettitore dopo una generazione di modulo rifiutata.

Le regressioni delle condizioni composte eseguono a O0/O2 congiunzioni e disgiunzioni con uguaglianza a costanti non nulle, confronti senza segno, confronti con segno in entrambi gli ordini, booleani estesi e tutte le combinazioni di negazione. Il C deve conservare l’intera tabella di verità senza dereferenziare un operando mancante del confronto con zero. Le scritture di indirizzi interi coprono valori a 32/64/128 bit allineati e non allineati. Gli array di byte conservano l’allineamento esplicito e gli accessi esatti alla base e parziali, senza assegnazioni scalari all’array né alias tramite tipi incompatibili.

`NeverDLowIRRefinementTests` copre grafi realmente recuperati, cicli finiti con strutture diverse e zero iterazioni, produttori dinamici, scelte condizionali, viste di ingresso sovrapposte, copie e spill correlati, prove di lettura immutabile su entrambi i lati, flag di sistema e conservazione del ritorno. Candidati errati, scritture aggiuntive, percorsi incompleti o infiniti, prove obsolete, collisioni temporanee e budget condivisi esauriti devono rifiutare il certificato. I test di indipendenza continuano a rifiutare valori arbitrari osservabili.

Nello stesso target, `LowIRLoopRefinement.*` e `BinaryLowIRLoopRefinement.*` coprono conteggi arbitrari a 64 bit, ranghi lessicografici annidati, residui nativi reali, prefissi d’ingresso, viste sovrapposte e salvataggi correlati. I controlli negativi rifiutano corpi errati, domini d’ingresso ristretti, ranghi non decrescenti, ritorno modulare senza segno, scritture precedenti dimenticate, tagli mancanti, modelli malformati e budget condivisi esauriti. Il successo di un ramo finito non autorizza un’induzione incompleta.

`LowIRLoopInference.*` e `BinaryLowIRLoopInference.*` usano contatori, salvataggi sullo stack, ritorni anticipati, chiamate native e flag impacchettati scritti indipendentemente. Coprono ampliamento aritmetico a larghezza ridotta e flag semanticamente uguali con espressioni diverse. Grafi malformati, origini mancanti o falsificate, cicli infiniti o con riavvolgimento e budget esauriti non devono produrre certificati.

Le regressioni con intestazione e blocco di ritorno condivisi coprono contatori a 32 bit estesi con zeri e a 64 bit completi, ranghi scalari a passo non unitario, risultati errati, percorsi senza progresso o con riporto modulare e budget esatti o esauriti fra ricerca scalare e per tuple. `LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`. Ulteriori regressioni di incremento e azzeramento richiedono convergenza senza espandere un bit del contatore per iterazione e rifiutano mancanza di progresso e riporto modulare senza segno. Le regressioni di pianificazione coprono accumulatori a passo non unitario, contatori unitari soggetti a riporto modulare accanto a un rango scalare valido con altro passo, e tre contatori la cui tupla valida compare oltre la finestra iniziale. Budget esatti e ridotti di un tentativo verificano una ripresa deterministica senza proposte ripetute.

`LowIRLoopPlanPairing.*`, nello stesso target, verifica registri rinominati, corpi aritmetici differenti, prefissi specifici di ciascun lato, predicati conservati, ingressi di memoria condivisi, tagli annidati e budget di prova indipendenti. Relazioni mancanti, scritture errate, temporanei mal associati, abbinamenti incompleti e limiti dei metadati esauriti non devono produrre certificati.

`LowIRLoopAlignment.*` verifica cicli ordinari e ruotati con contatori nel frame, scritti indipendentemente: entrambi i piani predefiniti si dimostrano singolarmente, il primo abbinamento fallisce e un altro taglio candidato dimostra la relazione. Le regressioni coprono permutazioni di più tagli, risultati e scritture errati, record originali mancanti o obsoleti, witness espliciti per valori indefiniti, contatori non decrescenti o con riporto modulare, grafi malformati, query cumulative dei tentativi falliti, budget totali esatti e limiti esauriti. Nessun rifiuto deve contenere un certificato. I nuovi casi coprono fasi separate di azzeramento e progresso, guardie d’uscita equivalenti spostate che richiedono abbinamenti tra famiglie, cache senza ripetere inferenze e superamento complessivo dei metadati. Un ciclo indipendente successivo verifica la copertura completa con un limite esplicito di 16384 query d’inferenza. Famiglie vuote o duplicate non usano query simboliche; tagli insufficienti sono respinti. Restano verificati budget globali esatti o ridotti di un tentativo, risultati errati, assenza di progresso, evidenze originali e witness indefiniti. Le regressioni del filtro coprono rombi aritmetici neutri, ricongiungimenti locali o solo alla frontiera e un punto di unione raggiungibile ma aggirabile verso un’uscita o una frontiera del ciclo. Verificano il candidato filtrato anche con famiglia originale duplicata, il riuso di un piano filtrato prima di un tentativo completo successivo, limiti esatti, inferiori di uno o nulli di `MaxCutSelectionWork`, il lavoro fallito cumulativo in `CutSelectionWork` e l’assenza di nuova inferenza simbolica dopo l’esaurimento del lavoro globale sul grafo. Entrambe le famiglie controllano la copertura completa dei cicli; il rombo usa limiti espliciti per query d’inferenza e prova.

Le regressioni coprono porzioni di frame e registro, entrambe le direzioni, posizioni basse/intermedie/alte, larghezze insolite, entrambi gli ordini dei byte e parole di frame di tre byte. Verificano scoperta tardiva, modifiche ai bit preservati, assenza di progresso e riporto senza guardia, ingressi aggiuntivi non validi, budget esatti/insufficienti e ricerca con un singolo taglio.

Le regressioni della fase iniziale coprono due e tre cicli sequenziali che riutilizzano una parola di conto alla rovescia, la combinazione con fasi di cicli annidati, budget esatti o ridotti di un tentativo per ranghi e query, cicli senza progresso e ripristini verso una fase precedente. Costanti di fase errate della stessa larghezza, risultati sbagliati e scritture errate nel frame devono essere respinti senza certificato dal verificatore completo; l’assenza di evidenze originali resta non supportata.

`InterpreterMachineStateModel.*` in `NeverDLowIRRefinementTests` usa esempi LowIR indipendenti per verificare flag di ingresso grezzi, stato distinto dal RAX ospite, tutte le 17 parole, sottoregistri, flag impacchettati, rifiuto dinamico persistente, scritture del frame ospite, entrambi i rami e inferenza ciclica seguita da una nuova prova. Uscite errate, stato perso, memoria modificata, registrazioni obsolete, ingressi malformati e budget esauriti devono fallire. I test sorgente esistenti eseguono anche entrambi i percorsi C a O0/O2; i test del modello da soli non certificano il C compilato.

`NeverDLLVMInterpreterModelTests` confronta LLVM indipendente con oracoli LowIR dello stato completo: larghezze, PHI paralleli, switch, memoria ospite, stato distinto, condizioni poison, intervalli intrinseci, contratti rifiutati e quattro budget. Verifica una prova completa del conto alla rovescia su una parola arbitraria e rifiuta stati alterati. C indipendente compilato a O1/O2 deve rispettare le stesse osservazioni. I test validano il modello ammesso; scoperta automatica degli invarianti e correttezza del compilatore restano separate. I casi di shift variabile coprono tutte e quattro le larghezze, conteggi limitati da maschere o rami, valori limite ed eccessivi, flag senza overflow ed esatti, rifiuto rigoroso del poison e C compilato a O1/O2.

`NeverDLLVMScalarEquivalenceTests` verifica domini completi dei cicli, zero iterazioni, scambi PHI simultanei, switch, bit alti di ingresso, controesempi nell’ultima partizione, aggiornamenti aggiuntivi che producono poison, intervalli di ritorno, contratti non supportati e budget esatti, inferiori di uno o nulli. Oracoli indipendenti di larghezza doppia e overflow coprono gli estremi funnel e i prodotti vincolati per ogni larghezza ammessa; C indipendente con cicli annidati a O1/O2 verifica il profilo di ingresso del compilatore. Anche la suite del modello di stato verifica gli estremi. `SymExpr.ConstantWindowSharesActualWorkWithoutRelaxingQueryCeilings` verifica la contabilità cumulativa e i limiti locali invariati.

`SymKnownBitsTests` verifica i fatti su tutte le coppie di byte e sui valori limite a precisione arbitraria: estensioni, somme senza riavvolgimento, radici distinte e semantica totale degli shift. Copre budget esatti o inferiori di uno, costo degli accessi alla cache, capacità limitata, contesti separati, profondità, numero di operandi e larghezze non supportate. `SymExprExtensionTests` verifica le costanti alte e i conteggi completi degli shift. Le regressioni di equivalenza mantengono i dati simbolici nelle prove di intervallo, rifiutano differenze nell’ultima partizione e poison eseguito, e verificano il budget dell’intera prova. `SymMBAExtensionTests` richiede una derivazione senza verifica a campione, controlla tutte le coppie di byte e conserva i confini di segno, riporto stretto, complemento ed esaurimento del lavoro.

`NeverDLLVMScalarLoopRecoveryTests` copre prefissi, stati dei predecessori, percorsi a zero iterazioni, auto-retroarchi, stati affini, uguaglianza con riavvolgimento modulare, poison da aggiornamenti extra, bit alti e contratti rifiutati. Budget cumulativi esatti o inferiori di un'unità verificano il rifiuto atomico. Oracoli aritmetici indipendenti eseguono LLVM originale e ricostruito a O0/O2 con tutti gli ingressi di controllo a un byte. Non certificano il recupero dell'ABI nativa né l'uscita C predefinita.

`NeverDLLVMCScalarLoopRecoveryTests` verifica uscita completa e selezionata predefinita, recupero dopo la pulizia dei ritorni, identità e attributi, chiamanti, intrinseci esistenti/nuovi e collisioni, budget condivisi, chiamate con effetti, ingressi senza garanzia di definizione, metadati, immagini, indirizzi esterni di blocchi e selezioni estranee. Oracoli indipendenti aritmetici e di rotazione eseguono il C a O0/O2 con trap per comportamento indefinito. L’aritmetica confronta anche LLVM originale compilato separatamente su tutti i controlli a un byte, valori limite e dati deterministici a larghezza piena.

Le regressioni coprono intervalli parziali e separati, alias fissi, entrambi i rami, ogni ritorno, letture alla prima iterazione e scritture prima delle letture nei cicli. Letture prima della scrittura, scritture mancanti o guest, alias sconosciuti, accessi speciali, intervalli fuori oggetto e budget esauriti devono fallire. Un esempio C indipendente con una parola di stato solo in uscita viene compilato a O1/O2, conserva gli attributi LLVM esatti e supera una nuova prova composta dal codice nativo a LLVM.

I test del conto alla rovescia protetto coprono il nuovo tentativo dopo il rifiuto del template del corpo, una prova completa all’intestazione per parole arbitrarie, budget condivisi e rifiuto immediato di violazioni reali del contratto di ingresso.

`NeverDInterpreterLLVMRefinementTests` controlla nuove prove composte, legame esatto testo/funzione, budget indipendenti, osservazioni complete e domini sorgente più ampi. Byte, residui, risultati, flag, stato, scritture, poison e piani errati/obsoleti devono impedire l’attestazione composta. I contatori di parola arbitraria richiedono entrambe le premesse induttive; esempi C indipendenti compilati O1/O2 verificano LLVM serializzato effettivo. Le regressioni rifiutano ritorni nascosti all’ingresso e limitano le radici senza copiare provenienza accessoria.

```sh
cmake --build build-release --target NeverDLLVMCScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMCScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarEquivalenceTests --parallel 4
build-release/bin/NeverDLLVMScalarEquivalenceTests
cmake --build build-release --target NeverDLLVMInterpreterModelTests --parallel 4
build-release/bin/NeverDLLVMInterpreterModelTests
cmake --build build-release --target NeverDInterpreterLLVMRefinementTests --parallel 4
build-release/bin/NeverDInterpreterLLVMRefinementTests
```

Le uscite per uguaglianza memorizzate nei cicli a due e tre livelli verificano operandi correlati, limiti variabili, azzeramenti dei contatori e copie alterate.

Le regressioni dei confronti in cache coprono uguaglianza e disuguaglianza, guardie e inizializzazione costante, campi scoperti dopo l’ampliamento e bit 7/31/63 in cache da 1/4/8 byte. Cambiare solo un bit vicino conservando quello verificato deve fallire nel confronto dell’intero stato. Passi nulli, limiti mobili, azzeramenti e budget esauriti devono essere rifiutati.

Le regressioni coprono ingressi uniti, primo testimone senza iterazioni, differenze nascoste di registri/frame, predicati booleani non canonici, condizioni native di trap, spill correlati e piani invalidi o con budget esaurito. Contatori indipendenti a due/tre livelli con uscita per uguaglianza e byte nativi verificano limiti senza segno, domini zero/massimo, passi non unitari e istruzioni originali errate. Inferenza e prova finale devono rifiutare risultati incompleti.

Regressioni indipendenti con cicli alternativi coprono entrambi gli orientamenti dei rami, corpi errati, un ramo fratello non terminante e l’esaurimento dei budget condivisi di ricerca/prova. `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

Le regressioni coprono due e tre livelli annidati, contatori crescenti e decrescenti, fasi inferite e tagli in corpi nativi reali. Domini di prefisso irraggiungibili o disgiunti, corpi errati, transizioni infinite o con riavvolgimento aritmetico e budget condivisi esauriti devono essere rifiutati. Un testimone di prefisso non sostituisce la copertura completa dei segmenti.

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

`NeverDLowIRUndefinedIndependenceTests` verifica l’indipendenza tra due esecuzioni di grafi LowIR completi e aciclici. Gli ingressi ordinari sono condivisi; ogni nuovo valore non definito dall’architettura mantiene le proprie correlazioni attraverso copie, scritture sovrapposte, salvataggi sullo stack e ricaricamenti. I predicati di controllo sono verificati prima delle assunzioni sul percorso. I certificati richiedono metadati degli effetti `Complete`, associati esattamente ai confini completi di ogni istruzione e al digest delle sue operazioni. Prove mancanti, cicli raggiungibili, chiamate, alias sconosciuti e budget esauriti comportano il rifiuto. Il risultato è limitato dalle osservazioni esplicite e dal contratto di accesso al frame senza errori; non dimostra l’equivalenza completa dal codice nativo a C.

Il comportamento seguente usa il contratto di audit rigoroso predefinito. `NeverDOriginalBinaryUndefinedIndependenceTests` usa byte x64 indipendenti con mappature fisse per verificare CALL/RET fisici, ritorni modificati, insiemi finiti esaustivi di destinazioni indirette e letture immutabili. Lo stesso target verifica raccolta completa dei rami diretti, legame esatto di byte/effetti/mappature/evidenze di lettura, conservazione di RSP e dello slot di ritorno d’ingresso al ritorno esterno e separazione frame/immagine. Istruzioni mancanti o sovrapposte, rami non verificati fuori dalle regole esatte per trap e proiezioni con profilo esplicito, cicli non terminanti o oltre budget, enumerazioni incomplete, profili/contratti incompatibili e budget esauriti devono essere rifiutati senza certificato o codice residuo. Ogni percorso nativo fattibile deve terminare. Il controllo facoltativo non certifica invarianti dei cicli, eccezioni, esecuzione con CET o equivalenza dal codice nativo a C; il recupero ordinario resta separato. Il target verifica anche i limiti terminali di `INT3`/`UD2` sottoposte a lifting rigoroso e il vincolo dei byte completi e dei digest delle operazioni. Un sidecar delle uscite indefinite `Missing` deve restare `Missing`; solo le trap dimostrate irraggiungibili mediante esecuzione simbolica possono comparire in un certificato, mentre ogni percorso fattibile verso una trap deve restituire `ContractViolation` senza certificato né codice residuo. La continuazione dopo le trap e il recupero dalle eccezioni non sono modellati, `codeFollowsTrap` non viene usato e il supporto dell’API LowIR statica rimane invariato.

I test espliciti di sovrapposizione nativa verificano veri salti x64 dentro operandi immediati, i risultati di entrambi i rami possibili e ingressi di ritorno indiretti dentro istruzioni precedenti. I fornitori sintetici verificano intervalli contenuti in entrambi gli ordini di raccolta, byte contraddittori su un ramo diretto non preso, coerenza tra codice e letture nei due ordini e letture del candidato. I budget esatti e insufficienti conteggiano anche i byte ripetuti attraverso trasferimenti indiretti. Risultati modificati, uso di API statiche o per cicli e prove contraddittorie devono impedire certificati; cambiare opzione o limite modifica i digest.

I test dei confini espliciti coprono RCL, XADD in memoria e REP MOVS irraggiungibili, contraddizioni simboliche dei percorsi, rami controllati da valori arbitrari e rifiuti esatti all’arrivo tramite ingresso, salto indiretto, CALL e RET. Verificano accessi indipendenti a suffissi raggiungibili, collisioni degli indirizzi candidato/nativo, prove malformate o parziali, risorse esaurite, rifiuto delle API statiche/dei cicli e tutti e tre i livelli di digest del raffinamento. Modificare un’istruzione irraggiungibile o attivare l’opzione senza confini conservati cambia i digest del certificato. Questi test verificano l’ambito finito dichiarato, non la semantica delle istruzioni non verificate.

I test coprono tutte le combinazioni dei flag scalari iniziali, maschere di privilegio, TF/AC in entrambe le esecuzioni, produttori indefiniti distinti, copie correlate, chiamate native, stato dei rami fratelli, osservazione finale obbligatoria, prove malformate e limiti delle risorse. Ogni percorso di input ammissibile dei cicli finiti deve terminare; un ramo sicuro non nasconde un percorso infinito o troncato. RDSSPD/RDSSPQ verifica i 16 registri generali in entrambe le larghezze, bit alti preservati, prove `Missing` mantenute e proiezioni contraffatte rifiutate. I test dello stato macchina confrontano entrambe le vie C a O0/O2 con trap per comportamento indefinito con un oracolo indipendente dei flag utente e verificano che gli errori del profilo restino registrati. I test INCSSPD/INCSSPQ coprono entrambe le larghezze e tutti i registri generali, conservazione dei limiti irraggiungibili, trap ammissibili dopo un ramo fratello completato, operandi zero e prove di trap contraffatte.

`NeverDX86UndefinedEffectsTests` verifica i metadati dei bit indefiniti, i flag definiti o preservati e il rifiuto dei certificati obsoleti. `NeverDX86CarryArithmeticFlagTests` controlla il riporto ausiliario di ADC/SBB nelle forme registro e memoria con un oracolo aritmetico. `NeverDX86LogicIdentityTests` verifica che AND con operandi identici azzeri ancora i bit 63:32 del registro a 64 bit corrispondente quando scrive una destinazione a 32 bit in modalità a 64 bit, preservando i bit non scritti nelle scritture più strette.

`X86RotateUndefinedEffects.*` confronta tutti i conteggi grezzi, le larghezze, le sovrapposizioni di CL, gli alias del byte alto e le destinazioni in memoria con un oracolo aritmetico scalare. `X86BitTestUndefinedEffects.*` copre indici registro/immediato, sovrapposizione sorgente/destinazione, registri estesi, flag definiti e scritture delle parti alte. I controlli dei metadati rifiutano operandi, codici e forme non supportate alterati. Le prove native distinguono letture correlate da nuovi bit indipendenti, verificano budget esatti/insufficienti e rifiutano overflow indefinito osservabile. Il raffinamento dell’intero stato accetta il testimone scelto e rifiuta testimoni a bit zero o candidati modificati.

`X86XaddAudit.*` verifica tutte le 65,536 coppie di operandi byte, i limiti dei flag per larghezze maggiori, la sovrapposizione di registri/byte alti, entrambe le scritture, i limiti della larghezza byte con REX e la conservazione dei registri completi con un oracolo aritmetico senza segno. Le verifiche native richiedono zero nuovi bit arbitrari senza perdere le dipendenze precedenti. Entrambi i testimoni accettano XADD invariato e rifiutano modifiche alla somma, alla sorgente scambiata o a un flag definito. L’alias `/6` usa la matrice completa dei conteggi di shift; gruppi/ID decodificati alterati vengono rifiutati.

`NeverDPEFixedImageTests` usa file PE costruiti indipendentemente per verificare istruzioni rilocate, dati immutabili, scritture delle importazioni, intestazioni/tabelle malformate, alias e provenienza alterata. Le prove dal codice nativo a LowIR e LLVM esatto accettano candidati corrispondenti e rifiutano risultati, stati o byte nativi modificati. L’esaurimento del budget di preparazione resta distinto e consente un nuovo tentativo con limiti esplicitamente aumentati; il caricamento ordinario accetta anche 40000 rilocazioni valide oltre il budget di analisi predefinito.

`FrameOffsets.*`, `NativeStackSpecialization.*` e `OriginalBinaryUndefinedIndependence.*` verificano tutti i resti degli allineamenti 2/4/8/16/32, bit alti liberi, salvataggi fra chiamate, cicli di conto alla rovescia, corruzione tramite alias, selezioni errate, grandi maschere irrilevanti, ampliamenti necessari e budget esatti o inferiori di uno. Controlli nativi separati verificano allineamento condizionato, pulizia interna senza segno, pulizia errata e ritorni con prefissi. Questi test non stabiliscono la copertura automatica native-to-LLVM dei cicli partizionati.

Le regressioni coprono entrambi gli ordini dei byte, basi del frame alte, campi sovrascritti e sovrapposti, archi successivi, predecessori ampliati, CALL/RET nativi e budget esatti o inferiori di uno. Entrambi i percorsi C eseguono i quattro casi di memoria a O0/O2. Le verifiche native fissano separatamente due valori del selettore; non provano ingressi senza vincoli. Un esempio LLVM indipendente verifica, comprese copie PHI e scritture, che spostare il ramo falso prima della confluenza comune non lo faccia eseguire dopo quello vero.

Le regressioni coprono partizioni più fini, tutti i residui ammessi, diversi bit alti, l’ultimo caso fallito e budget esatti o inferiori di uno. Entrambi i backend C vengono eseguiti a O0/O2 con indirizzi guest rifiutati inaccessibili e flag non validi: lo stato 2 deve preservare ogni byte di stato. I test del modello e C/Python verificano anche layout v5, proprietà e code precedenti o future. Le prove native rifiutano domini di allineamento non vincolati.

`StringTransfer.*` e le regressioni di copia ripetuta verificano sovrapposizione, conteggio zero, isolamento dei temporanei, limiti di capacità e budget e invalidazione dei puntatori. `MachineStringSourceTests.cpp` confronta esecuzione nativa ed entrambi i percorsi C a O0/O2 con un riferimento indipendente per tutti i registri, flag e byte dello stack, per quattro larghezze e due direzioni.

`ControlDiscovery.*` e `NativeStackSpecialization.*` verificano condizioni sui bit bassi della radice, dipendenze dai bit alti e dall’intera radice, visite incomplete, budget di visita esatti e insufficienti e la conservazione delle evidenze di indirizzi immutabili finiti.

I test delle guardie per obiettivi non risolti coprono una fase non valida irraggiungibile, un obiettivo sconosciuto raggiungibile all’ingresso o dopo un arco di ritorno, limiti esatti di raffinamento e budget di scoperta adiacenti con esito positivo o esaurimento. Il rifiuto non pubblica codice residuo, origini o testimoni di lettura. Il lavoro facoltativo dopo il recupero completo non stabilisce il budget minimo necessario.

Le regressioni della proiezione del frame su richiesta coprono registri automatici ed esistenti, radici alte e riavvolgimento modulare, guardie limitate al byte basso, fatti incompatibili o mancanti sugli archi fratelli, budget di query adiacenti e il risultato unknown del risolutore. Una richiesta su pochi bit non trasforma una prova parziale del puntatore in una prova completa; il rifiuto non pubblica codice residuo né testimoni.

Le regressioni dei sottocampi finiti coprono selettori in entrambe le metà, dati arbitrari osservabili, registri e slot del frame, entrambi gli ordini dei byte, campi stretti, successive espansioni del dominio, maschere disgiunte e fatti mancanti. Destinazioni raggiungibili mancanti, selettori liberi, budget di query condivisi esauriti e risultati unknown del risolutore non devono pubblicare codice residuo o testimoni. Sono coperti anche la scoperta automatica con richiesta dell’intera parola, gli indizi manuali inutilizzati, i dati derivati dalla radice e le finestre costanti iniziali.

I test dei budget pubblici coprono valori predefiniti e massimi a 32 bit, esaurimento indipendente delle valutazioni e della scoperta, entrambe le ABI sorgente e i backend C, e valori CLI malformati. I controlli di disposizione C/Python coprono v7, convalida ereditata ed estensioni future ignorate da v1–v6. Un rifiuto per budget non pubblica sorgente né testimoni; il contatore delle valutazioni non deve ritornare a zero al massimo.

I test del taglio opzionale confrontano cicli diretti e annidati con lo stesso budget di operazioni, eseguono i cicli residui, distinguono le modalità di decodifica e verificano che il concatenamento zero resti invariato. Un controesempio di correlazione deve riuscire per impostazione predefinita e rifiutare la pubblicazione con l’opzione. Chiamate native ripetute, slot di ritorno, riproduzione delle dipendenze e predecessori tardivi usano entrambe le impostazioni. I test pubblici coprono entrambe le ABI e i backend, flag predefiniti e sconosciuti, vecchie API che ignorano l’estensione e il booleano riportato.

`NativeStackSpecialization.NarrowAddressDemandRetainsCompletePointer` verifica ricongiungimenti condizionati di puntatori in registri e slot del frame, entrambi gli ordini dei byte, il riavvolgimento modulare e radici alte, il ripristino dello stack e 120 byte del frame. I controesempi associati rifiutano puntatori corrotti e verificano budget esatti o inferiori di una unità per query, operazioni, valutazioni e raffinamenti, oltre all’esaurimento della ricerca e dei contesti.

I test dell’osservatore coprono rifiuto anticipato, costanti, proiezioni vuote e query UNSAT finale. Le regressioni mantengono le letture a runtime dopo un controesempio, riverificano domini in cache per una diversa estensione di lettura e rifiutano certificati malformati senza pubblicare prove parziali.

I test del controllo affine coprono condizioni sui bit bassi con un budget di otto query, registri e slot del frame in entrambi gli ordini dei byte, riporto modulare, osservazioni dei byte del frame e ripristino dello stack. Una condizione contraddittoria non letterale deve eliminare un ramo non supportato; due radici con gli stessi 32 bit bassi e bit alti diversi devono conservare entrambe le destinazioni indirette.

`FiniteQueryCache.*` verifica limiti di memoria esatti e inferiori di un’unità, aggiornamenti dell’ordine tramite accessi riusciti, eliminazione di più record di dimensioni diverse, eliminazioni ripetute e query con variabili rinominate. Duplicati, mancate corrispondenze, risultati malformati e candidati troppo grandi mantengono l’ordine. Le copie delle prove restituite rimangono valide dopo l’eliminazione delle rispettive voci.

`ControlStateRecovery.*Marginal*` verifica domini indipendenti di obiettivi e dati applicativi il cui prodotto supera il limite congiunto, risultati concreti del programma residuo, un predecessore tardivo che aggiunge un obiettivo dopo l’ampliamento, obiettivi raggiungibili mancanti e lo scarto completo di domini troppo grandi o enumerati in modo incompleto. Questi casi originali verificano la ripresa dell’analisi al cambiare dei domini e impediscono di pubblicare grafi parziali dopo un errore. Una variante con uno slot del frame verifica l’invalidazione per alias dopo l’ampliamento della relazione, con entrambi gli ordini dei byte.

`InterpreterTransferChain.*` verifica valori correlati, rami univoci e dinamici, predecessori tardivi, limiti del frame, rifiuto degli alias e budget. Ulteriori test CALL/RET nativi verificano occorrenze ripetute, byte del ritorno e ripristino dello stack. La riproduzione dei produttori e i controlli da nativo a LLVM coprono contratti incompatibili, ricevute modificate, risultati errati e scritture mancanti sullo stack.

`NeverDX86NoIndexAddressTests` verifica l’indirizzamento SIB x86 senza indice con indirizzi a 32 e 64 bit: bit di scala ignorati, larghezze di destinazione, letture/scritture, metadati completi degli output indefiniti, offset di segmento e provenienza degli indirizzi. Rifiuta gli pseudoregistri come base o indice di larghezza errata e conserva gli indici R12 reali selezionati da REX.X. I test EVEX di broadcast e trasferimento mascherato coprono anche queste forme, la soppressione degli accessi inattivi e i metadati SIB incoerenti.

Le regressioni coprono tutti i conteggi grezzi a otto bit, combinazioni dei flag con conteggio zero, entrambi i modi x86, tutte le larghezze, alias CL, AH/CH/DH/BH, registri estesi e memoria. L’esecuzione simbolica per byte è confrontata con aritmetica ripetuta a un bit. I test relazionali verificano copie e nuovi flag, salvataggi, visite dei cicli, conteggi derivati da valori indefiniti, rami, formati non validi, digest e budget. Le letture finite coprono 1/2/4/8 byte, selezione dipendente dall’ingresso, insiemi singoli per percorso, evidenze complete e limiti; rifiutano candidati dipendenti, mancanti, scrivibili, non presenti nel file, rilocati o illimitati.

I test del nucleo verificano separazione dei contesti, ricongiungimenti a punto fisso, cicli dinamici, registri sovrapposti, invalidazione degli alias, dispatch finito e rifiuto senza sostituzioni parziali. I test dei sorgenti assemblano macchine x64 originali a registri, a stack e a indirizzi finiti; includono campi di controllo correlati e un oracolo nativo indipendente SysV/Win64. Entrambi i percorsi C sono compilati in O0/O2 con trap per comportamento indefinito e confrontati con un oracolo senza segno per calcoli, scritture in memoria e sentinelle di uscita. I casi negativi verificano certificati mancanti e budget insufficienti. La CLI pubblica e i rapporti verificano controlli, budget, contatori e rifiuti. Servono Clang con supporto cross-target e LLD; eseguire l’ELF originale richiede anche un host Linux x64. Uno strumento assente o un host incompatibile indica copertura saltata, non successo.

`ControlStateRecovery.LongTransparentLoop*` copre un ciclo indipendente di 20 fasi, oracoli aritmetici dinamici, il rifiuto dei selettori sconosciuti e l’esaurimento dei budget. `LongTransparentPhasesKeepExactBitDemands` verifica che i bit non pertinenti nello stesso byte del selettore restino dati osservabili a runtime senza diventare richieste di controllo. `ProducerClosureChargesWorkBeforeAnotherRestart` verifica che l’individuazione inversa e la rivalutazione consumino i budget condivisi prima dell’avvio di un nuovo grafo, senza pubblicare risultati parziali.

`X86ShiftCarry.*` verifica il riporto degli shift aritmetici a destra stretti,
i conteggi mascherati, le destinazioni APX e la soppressione dei flag usando
shift ripetuti di un bit. `NarrowArithmeticShiftCarrySurvivesBothSourceBackends`
esegue entrambe le uscite C a O0/O2 con trap per comportamento indefinito,
coprendo tutti i valori di byte e i conteggi originali.
`NeverDLLVMCIntrinsicSemanticTests` esegue anche min/max interi con e senza segno
i1/8/16/32/64/128 a O0/O2, verificando risultati assegnati e incorporati,
ordine dei produttori e valutazione singola. Le larghezze scalari non supportate
e gli operandi malformati devono fallire esplicitamente.

## Verifiche di controllo e chiamata nel C strutturato

`HighControlFlowSemantics.*` verifica che spostare uscite o code dei cicli conservi le etichette raggiunte da altri salti. Gli ingressi diretti nelle uscite iniziali e finali e la sostituzione di break eseguono il C generato a O0/O2 confrontandolo con valori di ritorno attesi indipendenti.

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` verifica che gli argomenti di registro obbligatori dedotti mantengano le posizioni finali sconosciute. La valutazione di un argomento obbligatorio o di una condizione sconosciuta deve causare una trap esplicita; gli operandi omessi, nulli o annidati non devono diventare silenziosamente zero. I valori noti e gli operandi aggiuntivi di cui è dimostrata la mancata lettura restano eseguibili. Una trap è un limite diagnostico, non una prova di equivalenza del comportamento ricostruito.

## Test di esecuzione CPU

`NeverDIntegerABITests` compila fixture Clang originali per Windows x64, Linux x64 e Linux ARM64. Funzioni reali a dieci argomenti verificano registri, stack e frame di chiamata. La matrice Unicorn/KVM/WHP indica come skip espliciti le coppie host/ISA non disponibili; uno skip non è un pass. `NeverDExecutionBudgetTests` controlla budget condivisi di continuazione, prenotazioni e deadline assolute senza attese basate sul tempo.

`NeverDCPUEmulationTests` copre istruzioni ARM64, controllo, load, contesti, alias, invalidazione cache e cicli limitati; il profilo software esegue anche FP/SIMD e TLS. `NeverDUserExecutionTests` verifica permessi CPL3/EL0, alias, fault di protezione, contesti e cambio spazio. `NeverDServiceRequestTests` dimostra che SYSCALL/SVC viene intercettato prima del trasporto e mantiene lo stato fino al consumo singolo; è un protocollo di handoff, non un modello completo dei servizi OS. `NeverDExecutionConfigurationTests` verifica risoluzione condivisa, distingue supporto build da probe live e rifiuta chiuso opzioni non supportate. I test pubblici SDK/CLI non richiedono il modello Windows. `NeverDThreadPointerTests` verifica FS-base, `TPIDR_EL0`, restore del contesto e permessi. `NeverDKvmCancellationTests` usa un guest x64 non terminante per verificare interruzione KVM attiva, ripresa e stato dei segnali invariato; senza KVM viene saltato esplicitamente.

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests NeverDCPUEmulationTests NeverDUserExecutionTests NeverDServiceRequestTests NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget|CPUEmulation|UserExecution|ServiceRequest|ExecutionConfiguration)Tests$' --output-on-failure
```

L’assenza di hardware ARM64 o hypervisor è copertura nativa omessa, non un pass. Unicorn e cross-compilazione non provano KVM/WHP nativo.

## Test del profilo processi Linux

Le [suite indipendenti dei processi](process-emulation.md#verifica) compilano vere fixture ELF x64/AArch64. `NeverDLinuxProcessTests` verifica avvio, policy degli header, service request, output binario, fault e limiti. `NeverDProcessPublicTests` controlla C API/CLI senza modificare l’immagine di analisi. `NeverDExecutionSessionTests` copre due CPU con memoria/budget condivisi e consumo exactly-once di richieste/fault. `NeverDX64MemoryUpdateTests` verifica aritmetica memoria, SETcc, BT, XMM/MXCSR, observer di scrittura, confini REP e letture dispositivo preparate. `DriverBackendParityTests.cpp` esegue fixture WDK originali e rilocate e confronta il report osservabile completo con Unicorn; immagini/backend assenti sono skip espliciti.

Il profilo x64 checked ammette anche le forme legacy mascherate `SS`, `SD`, `PS`, `PD` di `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`. `X64SSEInstructions.def` centralizza larghezze, allineamento e ammissione. `MaskedSSEArithmeticMatchesIndependentHostExecution` confronta registri/RAM con un riferimento CPU host indipendente: quattro arrotondamenti, FTZ, zeri con segno, subnormali e NaN. `SSEMemoryObserverStopsBeforeResultAndStatusChanges` verifica l’arresto prima degli effetti. DAZ, eccezioni non mascherate, x87 e AVX restano esclusi.

`X64PackedIntegerTests.cpp` usa codifiche originali e 180 risultati fissi di `X64PackedIntegerCases.def`, confrontati indipendentemente con gli intrinsic del compilatore x64 nativo. I casi con registri e alias RAM a fine pagina preservano gli altri XMM, sentinelle intere, FLAGS, MXCSR e byte sorgente. Arresti/errori degli osservatori e guasti di lettura recuperabili preservano lo stato; la riparazione consente un nuovo tentativo. Il disallineamento genera `#GP(0)`; MMX, LOCK e MMIO restano rifiutati prima dei callback. La CI nativa richiede entrambi i privilegi WHP.

`X64PackedShiftTests.cpp` e i casi originali di `X64PackedShiftCases.def` confrontano dieci scorrimenti con calcoli scalari indipendenti e intrinsics SSE2 nativi, usando 16 conteggi immediati e 21 variabili. Coprono alias tra conteggio e destinazione, bit superiori ignorati, allineamento, osservatori, errori recuperabili e rifiuto dei dispositivi. `X64VectorTestSupport.h` condivide le verifiche dei registri e della RAM con i test aritmetici packed. La CI nativa richiede entrambi i livelli di privilegio WHP.

`X64AlignmentTests.cpp` verifica che gli operandi disallineati delle istruzioni aligned SSE ammesse segnalino un `#GP(0)` recuperabile o terminale prima di osservatori, controlli dei permessi o callback del dispositivo. L’intero contesto pubblico dei registri x64, PC e RAM rimane invariato. Il riporto alla larghezza dell’indirizzo precede l’aggiunta di FS/GS; correggere l’indirizzo consente di ritentare l’istruzione originale. Test diretti KVM/WHP verificano indipendentemente il confine hardware. La consegna Windows dei fault di protezione generale resta fuori dal modello OS.

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
```

I backend non disponibili sono skip espliciti. Cross-compilazione e Unicorn ARM64 non provano KVM/WHP ARM64 nativo.

## Verifiche dell’emulazione dei driver

Abilitare `NEVERD_ENABLE_DRIVER_EMULATION=ON` insieme a `BUILD_TESTING=ON` per
compilare la suite di esecuzione mirata e le verifiche dell’API C/CLI tramite
la libreria condivisa:

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

Le fixture verificano l’inizializzazione guest, ritorni di successo ed errore,
comportamenti non supportati, fault di memoria, parsing rigoroso degli scenari,
esecuzione limitata, I/O sincrono con buffer e diretto, READ/WRITE, cicli di
vita indipendenti dei file, permessi MDL, export dinamici, argomenti variabili
guest e fault CPU strutturati attraverso create, trasferimenti, cleanup, close
e unload. Usare la [CLI `emulate-driver`](driver-emulation.md) per
verificare il JSON e i codici di uscita dei processi. Le build di produzione
possono abilitare questa funzionalità con `BUILD_TESTING=OFF`; `libneverd` non
deve richiedere una configurazione Unicorn riservata ai test.

I test aggiuntivi coprono MDL del pool non paginato appartenenti al driver, durate indipendenti di descrittore e buffer, strutture di interrogazione del registro e buffer corti, diritti degli handle, eliminazioni e perdite, oltre ai 64 bit completi di `information_hex` per gli IOCTL senza output. La convalida esterna include anche letture/scritture dirette sincrone e statistiche di Zero.

I test verificano contesti CPU completi (registri, flag, SIMD, FPU, CR8), memoria condivisa e rifiuto di contesti estranei o in fault. Le fixture compilate `driver_dispatcher.c` eseguono DPC e callback di lavoro reali, scadenze, eventi/timer di notifica e sincronizzazione, attese non alertable `KernelMode` con motivo `Executive`, timeout/delay, più stack bloccati, risvegli conservati dopo set/reset, argomenti e errori IRQL/durata. I test di lavoro mantengono copertura pending/completamento, code, stalli e budget comuni. Provano il sottoinsieme descritto, non tutto l’asincronismo Windows.

`driver_context_limits.c`: I limiti IRQL provengono da `KernelAPIIRQL.def`; il modello responsabile verifica le restrizioni dipendenti dagli argomenti. Un DPC non può chiamare il registro né allocare, liberare o accedere al pool paginato. Le conversioni Unicode di `DbgPrint` richiedono `PASSIVE_LEVEL`; output ANSI e operazioni non paginate supportate restano utilizzabili a `DISPATCH_LEVEL`. Gli stack hanno limiti: un puntatore fuori intervallo non può entrare nello stack di un altro worker bloccato. I timer armati nell’estensione impediscono il rilascio prematuro del dispositivo. Ciò non espone cambi generali di IRQL.

`KernelDeviceStackTests.cpp` verifica proprietà/collegamenti indipendenti, selezione della cima, atomicità degli errori, capacità, campi opachi, handle, riferimenti di lavoro/richiesta oltre scollegamento/eliminazione e identità file/dispatch. L’originale `driver_wdm_stack.c` usa veri header WDK e Copy/Skip/SetCompletion inline; i percorsi facoltativi `NEVERD_WDM_STACK_FIXTURE`/`NEVERD_WDM_STACK_CFG_FIXTURE` scelgono immagini normali/CFG attivo. `DriverWDMStackTests.cpp` copre rilocazione, stato inferiore, ordine/flag di completamento, propagazione pending ritardata, worker/DPC, attese, `STATUS_MORE_PROCESSING_REQUIRED`, MDL diretti mantenuti, completamento annidato e cursori/controlli malformati. `DriverScenarioPublicTests.cpp` copre inoltro C API/CLI e completamento trattenuto/annidato C API, comprese immagini CFG configurate. Gli artefatti mancanti sono saltati esplicitamente. Le prove Linux stabiliscono solo stack dello stesso driver, non supporto PDO/PnP/alimentazione. `KernelIRPStackTests.cpp` verifica cursori contati, prefissi Copy inline completi, posizioni azzerate, propagazione stato/pending, MPR e completamento annidato, proprietari delle continuazioni e percorsi conservati. Anche READ/WRITE e il ciclo dei file reali usano Copy inline.

`DriverPnpScenarioTests.cpp` verifica la corrispondenza rigorosa tra JSON e validazione nativa, i dati iniziali espliciti, i limiti di ID e quantità, le combinazioni di campi vietate, gli stati finali del bus e i rapporti osservati che ammettono null. `KernelPnpDeviceTests.cpp`, `KernelPnpRequestTests.cpp` e `KernelPnpCompletionTests.cpp` coprono proprietà del provider, successo/errore/perdite di AddDevice, IRP iniziali, ammissione dei file, rollback del ciclo di vita, completamento differito, continuazioni MPR/annidate/in attesa e atomicità degli errori. L’originale `driver_wdm_pnp.c`, compilato con il vero WDK, usa i percorsi facoltativi `NEVERD_WDM_PNP_FIXTURE` / `NEVERD_WDM_PNP_CFG_FIXTURE`. `DriverWDMPnpTests.cpp` esegue immagini normali e con CFG attivo dopo rilocazione: AddDevice, I/O dei file, rimozione ordinata, avvio/rimozione differiti, errori di avvio/query ed errori AddDevice con pulizia o perdite. `DriverScenarioPublicTests.cpp` copre anche uno scenario PnP differito di sette richieste tramite C API e CLI, incluse le immagini CFG configurate. Gli artefatti mancanti vengono saltati esplicitamente. Le prove di esecuzione provengono solo da Linux e dimostrano soltanto il sottoinsieme PnP senza risorse documentato.

I test dello schema V9 verificano lettura e riscrittura dei nomi di tutte le otto funzioni minori e condividono la validazione dello stato finale con il completamento del ciclo di vita; QueryStop 0x119 viene rifiutato prima del caricamento dell’immagine. I controlli ampliati del modello e della fixture autentica coprono rollback di query-stop, cancel-stop, arresto/riavvio, rimozione improvvisa, violazioni dei contratti di successo esatto, I/O software durante arresto o rimozione in sospeso, rifiuto del guest dopo rimozione improvvisa, identità del dispositivo e risultati misti di AddDevice. `DriverScenarioPublicTests.cpp` esegue una sequenza di 16 richieste di arresto/riavvio/rimozione improvvisa tramite C API e CLI con fixture normali/CFG attivo, mantenendo i byte dell’IOCTL software riuscito, l’IOCTL fallito restituito dal guest dopo la rimozione improvvisa e cleanup/close/remove finali. L’esecuzione pubblica è seriale: un IRP trattenuto senza una sorgente di completamento attualmente disponibile non può attendere una richiesta successiva dello scenario che avvii o ripulisca il dispositivo. I vincoli di svuotamento prima di Remove sono limiti del profilo, non una politica generale di ammissione I/O di Windows. Le prove restano limitate a Linux.

`DriverPowerScenarioTests.cpp` verifica i dati rigorosi dei pacchetti di alimentazione, la corrispondenza JSON/nativa, il contesto opaco a 32 bit, i limiti delle FIFO di risposta e i rapporti indipendenti delle richieste figlie. `KernelPowerRequestTests.cpp` e `KernelPowerCompletionTests.cpp` controllano il layout reale dei pacchetti, i flag dei percorsi, la distinzione tra ciclo di vita e notifica per oggetto, la corrispondenza FIFO, la proprietà del callback terminale, MPR, attese e confini di rilascio. L’originale `driver_wdm_power.c`, compilato con il vero WDK, usa i percorsi facoltativi `NEVERD_WDM_POWER_FIXTURE` / `NEVERD_WDM_POWER_CFG_FIXTURE`. `DriverWDMPowerTests.cpp` copre la rilocazione normale/con CFG attivo, Query/Set diretti e annidati, il completamento indipendente differito, l’ordine S0 prima di D0, le istantanee dei callback a cinque argomenti mantenute durante le attese, le richieste figlie avviate dai worker, i callback null, il rifiuto di query, i valori iniziali/FIFO indipendenti dei PDO e gli errori espliciti per dati mancanti. `DriverScenarioPublicTests.cpp` aggiunge la verifica preliminare dei pacchetti di alimentazione malformati e una sequenza di sospensione/ripresa con sei richieste di scenario e tre figlie tramite C API e CLI. Gli artefatti autentici mancanti vengono saltati esplicitamente; le prove di esecuzione restano limitate a Linux e attestano soltanto il sottoinsieme documentato di alimentazione paginabile senza risorse.

`KernelUsbIdleTests.cpp` verifica proprietà, D2, prestito e prima causa; `KernelUsbIdleBridgeTests.cpp` / `KernelUsbIdleReceiptTests.cpp` IRP reali, annullamento, capacità composita e ordine ricezione/fine; `DriverUsbIdleScenarioTests.cpp` input/report. `DriverWdmUsbIdleTests.cpp` usa il vero `driver_wdm_usb_idle.c` tramite `NEVERD_WDM_USB_IDLE_FIXTURE` / `NEVERD_WDM_USB_IDLE_CFG_FIXTURE` per idle, D0/D3, risveglio, annullamento, riarmo/riavvio, funzioni indipendenti/composite e route PDO/FDO. `DriverWdmUsbIdlePublicTests.cpp` esegue lo [scenario USB](../examples/driver-wdm-usb-idle-scenario.json) con C API/CLI, normale/active-CFG e basi preferite/rilocate. Artefatti assenti saltati esplicitamente; prove solo Linux, nessuna prova di supporto USB KMDF.

`KernelFrameworkUsbIdleTests.cpp`, `KernelFrameworkUsbIdleStorageTests.cpp` e `KernelFrameworkUsbIdleBridgeTests.cpp` verificano politica, memoria e pianificazione tipizzata. Il vero `driver_kmdf_usb_idle.c` non invia un proprio IRP idle. `DriverKMDFUsbIdleTests.cpp` copre assenza di permesso, I/O gestito con D2/D0 differiti, StopIdle prima/durante callback, arm fallito, Maximum esplicito, risveglio e gruppi. `DriverKMDFUsbIdlePublicTests.cpp` esegue lo [scenario KMDF USB](../examples/driver-kmdf-usb-idle-scenario.json) con `NEVERD_KMDF_USB_IDLE_FIXTURE` / `NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`, C API/CLI, normale/active-CFG e basi preferite/rilocate. Assenze saltate esplicitamente; prove solo Linux. I test del modello verificano che l’esaurimento delle allocazioni dopo l’armamento riuscito esegua il vero callback di disarmo e annulli WAIT_WAKE senza consumare una risposta D2.

`DriverKMDFUsbPoFxTests.cpp` copre SystemManaged/WithHint iniziale, due permessi, annullamento D0, D2/D0 ritardati e conferma F0 da vero worker, StopIdle, risveglio prima di READ, errore arm, rimozione e riavvio. `DriverKMDFUsbPoFxPublicTests.cpp` esegue lo [scenario USB PoFx](../examples/driver-kmdf-usb-pofx-scenario.json) via C API/CLI, entrambi i modi, normale/active-CFG e basi preferita/rilocata. `DriverKMDFUsbIdleTests.cpp` conserva regressioni di inoltro e verifica READ diretto dopo D0Entry. `KernelFrameworkRequestTests.cpp` copre associazioni, proprietà caller-context, code manuali/fermate e IRQL. Esaurimento e vincoli indipendenti sono provati nel ponte del modello, non nel driver reale. Artefatti mancanti saltati esplicitamente; prove di esecuzione solo Linux. `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` copre anche il vero errore D0Entry in RemovePending: conferma Required esatta e quiescenza permettono pulizia senza F0/ActiveCondition. Non prova errori SET_POWER generali o rimozione improvvisa autonoma.

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: Lo [scenario WAIT_WAKE nativo](../examples/driver-wdm-wait-wake-scenario.json) usa il vero fixture WDK `driver_wdm_wait_wake.c` tramite `NEVERD_WDM_WAIT_WAKE_FIXTURE` / `NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE`. Verifica invio durante START, argomenti, risveglio senza D0 implicito, riarmo, annullamento, MPR, annullamento DPC seguito da D0 tramite worker, cattura esatta e provider indipendenti. Le prove normali/active-CFG a base preferita/rilocata sono solo Linux; file assenti comportano skip esplicito.

`KernelPowerCompletionTests.cpp` copre APC/DPC, errore atomico di capacità e nuovo tentativo, provider solo sincrono/differito con/senza callback, route conservata e MPR. Il vero `DriverWdmWaitWakeTests.cpp` verifica D0 diretto dall’annullamento DPC, Query/Set APC/DPC, IRQL/CR8 invariati, ritorno prima dell’esecuzione PASSIVE e rifiuto di WAIT_WAKE elevato. Lo [scenario elevato](../examples/driver-wdm-elevated-power-scenario.json) usa `DriverWdmWaitWakePublicTests.cpp`, C API/CLI, immagini normali/active-CFG e basi preferite/rilocate; prove solo Linux.

`KernelRemoveLocksTests.cpp` verifica identità indipendenti di blocco e dispositivo, Tag NULL e ripetuti, dimensioni retail/DBG esatte, svuotamento immediato e differito, obblighi dopo acquisizioni fallite, atomicità degli errori, capacità e ritiro dello spazio. `KernelRemoveLockBridgeTests.cpp` e `DriverWDMRemoveLockTests.cpp` coprono l’inizializzazione prima del collegamento, lo spazio opaco nell’estensione, i limiti IRQL, il rilascio dopo il ritiro del pacchetto, il completamento del provider dopo lo svuotamento del blocco, la disponibilità all’ultimo rilascio prima del ritorno del callback, i worker in attesa e la pulizia di AddDevice fallito. L’originale `driver_wdm_remove_lock.c`, compilato con il vero WDK, ha varianti retail/DBG e normale/con CFG attivo tramite i percorsi facoltativi `NEVERD_WDM_REMOVE_LOCK_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` e `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`. I test pubblici C API/CLI usano lo schema PnP esistente e conservano osservazioni distinte della ricezione e del completamento del bus e dello smantellamento finale. Gli artefatti mancanti vengono saltati esplicitamente; le prove di esecuzione sono limitate a Linux e non attestano l’intero Driver Verifier né lo svuotamento generale di richieste simultanee.

`DriverResourceScenarioTests.cpp` verifica fatti espliciti JSON/C++, larghezze degli interi, conteggi, sovrapposizioni fisiche/dei registri, allineamento, ID, banchi vuoti e serializzazione della configurazione. `KernelMMIOTests.cpp`, `KernelMMIOFailureTests.cpp`, `KernelResourceBridgeTests.cpp` e `UnicornMMIOTests.cpp` coprono proprietà di banchi/mapping, alias, generazioni, durata delle liste impacchettate, tempi del provider, persistenza al riavvio, accessibilità dopo rimozione improvvisa o cambi di alimentazione, transazioni CPU/API esatte e atomicità degli errori. L’originale `driver_wdm_resources.c`, compilato con il vero WDK, usa `NEVERD_WDM_RESOURCE_FIXTURE` / `NEVERD_WDM_RESOURCE_CFG_FIXTURE`; `DriverWDMResourceTests.cpp` esegue accessori scalari e REP reali, rilocazione normale/con CFG attivo, alias di sottointervalli, mapping a fine pagina, STOP/riavvio e accessi invalidi. I test C API/CLI rifiutano dati invalidi prima di caricare l’immagine ed eseguono lo stesso scenario di 14 richieste con output IOCTL persistente e conteggi map/unmap esatti. Il file condiviso [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) richiede il protocollo registri/IOCTL di questo fixture. Gli artefatti mancanti vengono saltati esplicitamente; le prove sono limitate a Linux e non coinvolgono memoria fisica host o un backend generale di dispositivi.

`DriverInterruptScenarioTests.cpp` copre descrittori raw/tradotti espliciti, assegnazioni miste e solo interrupt, campi/conteggi rigorosi, identità d’origine e osservazioni BOOLEAN indipendenti. `KernelInterruptsTests.cpp`, `KernelInterruptBridgeTests.cpp` e `SchedulerInterruptTests.cpp` coprono corrispondenza esclusiva delle tuple, token opachi, cattura di generazione/connessione, durata degli eventi, campi Ex selezionati esatti, ripristino del lock comune/IRQL, proprietà dei callback, priorità ISR nello stesso istante ed errori di capacità prima delle modifiche. `KernelFrameworkRequestTests.cpp` verifica anteprime pure dell’annullamento e capacità dei token in lotti senza pubblicare chiamate o consumare riferimenti. L’originale `driver_wdm_interrupts.c`, compilato con il vero WDK, usa `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`; `DriverWDMInterruptTests.cpp` esercita rilocazione normale/con CFG attivo, ABI legacy a undici argomenti, versioni Ex 1/2/4, completamento reale ISR→DPC, FALSE nel basso AL, sincronizzazione/lock manuali, PDO indipendenti, generazioni al riavvio e fatti hardware invalidi. I test C API/CLI rifiutano dichiarazioni invalide prima di caricare l’immagine ed eseguono [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json) con sette richieste, verificando i byte dell’IOCTL pending e le osservazioni separate della consegna. Le immagini mancanti sono saltate esplicitamente; le prove restano limitate a Linux e non attestano interrupt condivisi/di livello/MSI o preemption a livello di istruzione.

`DriverDMAScenarioTests.cpp` verifica capacità esplicite, domini logici, limiti di byte/conteggi/tempo, direzioni rigorose e separazione tra configurazione e osservazioni. `KernelPhysicalMemoryTests.cpp` e `BackendBackingTests.cpp` controllano confini di allocazione su pagine condivise, riferimenti trattenuti, permessi CPU invariati, esclusione MMIO/rientranza e atomicità degli errori sull’intero intervallo; `KernelRequestMDLTests.cpp` verifica PFN di sola lettura e alias dei descrittori costruiti sulle stesse identità fisiche. `KernelDMATests.cpp`, `KernelDMABridgeTests.cpp` e `SchedulerDMATests.cpp` esercitano byte RAM effettivi, chiamate di tabelle associate, proprietà FIFO immediata/in coda, durate separate di callback/mapping, frammenti di pagina, direzioni errate, prevalidazione del rilascio, domini PDO indipendenti ed errori di generazione/alimentazione. L’originale WDK `driver_wdm_dma.c` usa `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`; `DriverWDMDMATests.cpp` e le prove C API/CLI eseguono veri puntatori dell’adattatore, memoria common/SG ed eventi DMA/interrupt configurati separatamente. Il condiviso [driver-dma-scenario.json](../examples/driver-dma-scenario.json) richiede il protocollo del fixture. Gli artefatti mancanti sono saltati esplicitamente; l’evidenza è solo Linux e non dimostra DMA reale dell’host, PCI o un motore generico di dispositivi. `pluginsdk/python/tests/test_driver_dma_integration.py` usa il binding JSON con proprietà esplicita già esistente e `NEVERD_TEST_LIBNEVERD` / `NEVERD_TEST_WDM_DMA_FIXTURE` / `NEVERD_TEST_WDM_DMA_CFG_FIXTURE` per verificare byte, ordine dei callback e osservazioni degli errori.

`KernelSEHTests.cpp` verifica piani puri di unwind, ordine degli ambiti, ripristino dei registri generali non volatili, stack limitati e metadati esplicitamente non supportati; `KernelExceptionTests.cpp` verifica arità esatta delle API, stati nei 32 bit bassi, eccezioni tipizzate, limiti IRQL e stato del modello/CPU invariato. Il fixture WDK autentico `/GS-` `driver_wdm_seh.c` usa le opzioni `NEVERD_WDM_SEH_FIXTURE` / `NEVERD_WDM_SEH_CFG_FIXTURE`; `DriverWDMSEHTests.cpp` esegue immagini normali, con CFG attivo e rilocate con eccezioni dirette e da helper, gestori annidati, nuove eccezioni dai gestori, filtri reali, finally durante l’unwind, ordine di ricerca, record stabili, continuazione dei fault CPU supportati e ripristino completo dello stato CPU. I filtri annidati e i finally in collisione usano stack logici collegati; gli altri fault CPU restano esplicitamente rifiutati. C API/CLI esegue [driver-seh-scenario.json](../examples/driver-seh-scenario.json) e verifica risultati API nulli con messaggi effettivi del gestore guest. `pluginsdk/python/tests/test_driver_seh_integration.py` usa `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_SEH_FIXTURE` e `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`. Le immagini esterne mancanti vengono saltate esplicitamente; l’evidenza resta limitata a Linux e non stabilisce supporto per buffer utente o SEH generale.

`KernelDMAChannelTests.cpp`, `KernelDMAChannelBridgeTests.cpp` e `SchedulerDMATests.cpp` verificano FIFO di allocazioni miste, larghezze di ritorno, controlli puri di ammissione/rilascio, riuso dei registri, frammenti contigui, flush completo, snapshot CurrentIrp e durate di pacchetti/MDL/dispositivi. L'originale WDK `driver_wdm_dma_channel.c` usa i percorsi facoltativi `NEVERD_WDM_DMA_CHANNEL_FIXTURE` / `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`; `DriverWDMDMAChannelTests.cpp` esegue driver normal/CFG attivo e rilocati con vere chiamate MapTransfer/FlushAdapterBuffers, quota common/SG/canale condivisa, transazioni esplicite, completamento IRQ/DPC, operazioni sequenziali, due PDO ed errori. C API/CLI esegue le sette richieste di [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json), inclusa una transazione su entrambi i frammenti. `pluginsdk/python/tests/test_driver_dma_channel_integration.py` usa `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE` e `NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` sulla stessa interfaccia JSON pubblica. Artefatti assenti vengono saltati esplicitamente; le prove Linux non dimostrano controller DMA di sistema né schemi HAL arbitrari di mapping/flush.

`DriverGuardTests.cpp` e quattro varianti originali di `driver_guard.c` coprono CFG attivo/inattivo, rilocazione della base, ABI check/dispatch e destinazioni malformate. `KernelFrameworkTests.cpp`, `KernelFrameworkControlTests.cpp`, `KernelFrameworkQueueTests.cpp` e `KernelFrameworkRequestTests.cpp` coprono binding, creazione transazionale dei dispositivi, instradamento delle code, lunghezze logiche dei buffer e l’ordine di pulizia e la durata di IRP e contesti. Gli originali `driver_kmdf_lifecycle.c` e `driver_kmdf_control.c` si compilano facoltativamente con veri header WDK 1.33 e si collegano tramite la vera libreria `FxDriverEntry`. Impostare i percorsi nella cache CMake `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` per le immagini del ciclo di vita e `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` per le immagini dei dispositivi di controllo normali e con CFG attivo. Gli artefatti esterni mancanti vengono esplicitamente saltati. `DriverKMDFLifecycleTests.cpp`, `DriverKMDFControlTests.cpp` e i casi C API/CLI in `DriverScenarioPublicTests.cpp` coprono callback reali, I/O bufferizzato e diretto, completamento pendente tramite elementi di lavoro, stati di errore, scaricamento ed esecuzione CFG con base rilocata. Le prove restano limitate a Linux e non attestano supporto completo per KMDF né per PnP o gestione dell’alimentazione.

I test della vecchia API di annullamento mantengono la continuazione API durante annullamento, pulizia annidata e distruzione finale; Ex continua a restituire annullamento senza callback per richieste già annullate. `KernelFrameworkRequestAccessorTests.cpp` e `KernelRequestMDLTests.cpp` verificano Information condivisa a 64 bit, lunghezza al completamento, identità coda/IRP originale, handle file WDF NULL, getter su handle conservati, cache MDL con ByteCount della prima direzione, descrittore diretto e mapping differito, scadenza e rifiuto del bypass WDM. Le modalità L, M, D e C del fixture di controllo reale eseguono la vecchia API di annullamento, MDL/informazioni bufferizzate, MDL READ/WRITE diretti e accesso dopo completamento in immagini normali/con CFG attivo.

I test di annullamento coprono scadenze virtuali riservate ai trasferimenti e campi dei rapporti, completamento precedente, richieste già annullate, marcatura/rimozione, facoltà di completare secondo accodamento o consegna, attese e riferimenti interni. I test dello scheduler verificano separatamente ordine DPC/annullamento/lavoro, capacità, identità distinte e sospensione/ripresa. L’annullamento WDM rimane un errore esplicito del modello.


## Organizzazione dei test

`add_neverd_unittest` crea un eseguibile GoogleTest e assegna a ogni caso
scoperto una label CTest uguale al nome del target eseguibile.

| Area sorgente | Target e label CTest | Copertura |
|---------------|----------------------|-----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Invocazione di processi figlio multipiattaforma, quoting, redirect e codici di uscita |
| `unittests/libc` | `NeverDLibCTests` | Nomi libc noti e classificazione |
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Catalogo di sink, precedenza di identità, prefiltro degli argomenti, hunt di overflow di copia, audit di vita dell’heap e matrice obbligatoria a sei celle PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/lift` | `NeverDLiftTests` | Forme LowIR decoder/lifter, fasi IR, loader, relocation, fixture di formato, decompilazione e flussi patch rappresentativi |
| La maggior parte di `unittests/semantic` | `NeverDSemanticTests` | Semantica differenziale di istruzioni, ABI, controllo, espressioni C e lift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadata hardfork, normalizzazione, ambiguità ABI/firma, CFG/SSA/recovery, confini decoder esaustivi e input ostili, fatti proxy/call, semantica interpreter, differenziali LLVM/C/Solidity e API pubblica |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | Metadati v0-v4 e layout ELF, comportamento rigoroso di verifier/loader, 23 artefatti ELF fissati, oracle ufficiale indipendente, disponibilità esaustiva degli opcode, input ostili, CFG/recupero e differenze eseguite LLVM/C/Rust |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Equivalenza riscrittura/offuscamento su quattro ISA e tre formati oggetto |
| File di trasformazione mirati in `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Sonde veloci da ricollegare separate dal grande binario semantico |
| `unittests/corpus` (sottomodulo) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests`, `NeverDAdaDEHCorpusTests` | Metadati di eccezioni e runtime letti da 545 binari reali fissati, ciascuno dichiarato in un manifest con le soglie minime che il suo recupero deve superare |

Le fonti autorevoli per la registrazione sono
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt) e
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt) e
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt) e
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt).

### Il corpus binario fissato

Ogni altra suite costruisce ciò che prova; il corpus no: è un sottomodulo di
binari prodotti da toolchain reali, su host e per target che questo repository
non può raggiungere. Ognuno è fissato per digest e accanto un manifest dichiara
le soglie minime che il suo recupero deve superare. È l’unico posto in cui
un’affermazione su ciò che NeverD legge da, poniamo, un oggetto condiviso
`armv7` compilato con `-O2` e privato dei simboli trova una risposta anziché una
discussione.

Le suite vengono costruite solo se al passo di configurazione è stato detto di
cercarle, quindi è quel flag a tenerle sotto test:

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` esegue tutte le linee; `check-neverd-windows-eh-corpus`,
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus`,
`check-neverd-cxx-itanium-eh-corpus`, `check-neverd-objc-eh-corpus` e `check-neverd-ada-d-eh-corpus` ne eseguono
una ciascuno. Tutti e tre gli host di CI configurano con il flag ed eseguono le
sei linee: i byte sono identici ovunque, ma ciò che li legge non lo è, e una
passata del corpus su un host non prova nulla sugli altri due.
`scripts/audit_ci_test_inventory.py` rifiuta un inventario a cui manchi una
delle sei etichette, perché una build che ha smesso in silenzio di leggere il
corpus è una regressione che nessun test può cogliere: il test è proprio ciò che
è sparito.

L’audit live degli opcode EVM si esegue così:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

In locale e in CI il percorso standard forza
`git fetch --depth=1 --force` sull’URL ufficiale
`https://github.com/ethereum/go-ethereum.git` e prova soltanto lo SHA esatto
appena ottenuto dal `HEAD` remoto del branch predefinito, in un worktree
detached. Ogni esecuzione usa un repository bare privato, temporaneo e
dal nome imprevedibile. Mantiene l’authority ref del fetch e il suo SHA esatto
per la vita del worktree detached, poi distrugge entrambi. Non esistono
repository Git persistenti o cache condivisi. `local_docs`, un checkout
esistente e un submodule non sono percorsi d’audit: un pin di submodule
diventerebbe obsoleto proprio quando serve rilevare il drift live.

Ogni comando Git rimuove prima tutti i `GIT_*` ereditati, inclusi
`GIT_CONFIG_*`, e poi installa soltanto valori revisionati.
`GIT_CONFIG_NOSYSTEM` e `GIT_CONFIG_GLOBAL` disabilitano la configurazione
system/global; `GIT_ATTR_NOSYSTEM` e `core.attributesFile` per comando
disabilitano gli attributi system/global, mentre `core.hooksPath` disabilita gli
hook. Configurazione inattesa del repository privato, grafts,
`objects/info/alternates` o `refs/replace` fanno fallire la validazione;
`GIT_NO_REPLACE_OBJECTS` disabilita replacement lookup.

Il probe riflette tutti i bool esportati di
`params.Rules`, chiama `LookupInstructionSet(params.Rules)` e scandisce tutti i
256 slot. `EVMUpstreamOpcodePolicy.def` possiede alias e typed exclusions
storiche/EOF non pianificate; `EVMUpstreamSemanticsPolicy.def` possiede
l’inventario Rules chiuso, mapping dei fork, eccezioni base-stack e famiglie
dynamic-immediate.

CI esegue lo stesso audit live soltanto sui push a `dev`, sulle pull request,
manualmente e ogni giorno. Il probe Go chiama l’API pubblica
`LookupInstructionSet(params.Rules)` per ogni fork mappato.
La CLI pubblica espone soltanto `--manifest-output`; il manifest chiuso usa
`schema 3` e non permette di scegliere source, ref, checkout o toolchain.
`EVMUpstreamOpcodePolicy.def` gestisce alias ed esclusioni storiche/EOF non pianificate
revisionate; l’ortogonale `EVMUpstreamSemanticsPolicy.def` gestisce regole dei
fork ed eccezioni della semantica dello stack. Il manifest chiuso verifica
revisione esatta, attivazione, byte/name, `base_min_stack` e `net_stack_delta`,
e rifiuta field, fork, nomi o byte sconosciuti o duplicati. L’allocazione usa
soltanto `operation.undefined`; `HasCost` serve soltanto da controllo incrociato
del costo perché vale false anche per le operazioni definite a costo zero. Ogni slot
`defined && !HasCost` deve corrispondere esattamente a
`EVM_GETH_ACTIVE_WITHOUT_COST` dal fork dichiarato. Uno slot undefined con costo,
uno defined non revisionato o la perdita del marker falliscono in modo chiuso.
Dichiarazioni mancanti, fuori range o non consumate sintatticamente falliscono
anch’esse: ogni `.def parser` rifiuta una policy `partial`. Un errore CI carica
revisione, manifest e log come artifact. Parser e diagnostic hanno copertura
unit Python indipendente:

`EVMUpstreamSemanticsPolicy.def` assegna ogni campo booleano esportato di
`params.Rules` a un solo `EVM_GETH_RULE_FIELD`: `MappedForkSelector`,
`NoOpcodeAllocation` o `ExcludedSelectorExpectedError`. Il probe abilita ogni
campo isolatamente tramite `LookupInstructionSet`: le prime due categorie
richiedono nil error, la terza error, e ogni fingerprint opcode/stack completo
di 256 slot deve essere `ExpectedFork`. `IsEIP155`, `IsEIP2929`, `IsEIP4762` e
`IsPetersburg` sono ora campi senza allocazione con fingerprint Frontier;
`IsUBT` deve fallire e produrre Cancun.

`EVMUpstreamSemanticsPolicy.def` dichiara le famiglie dinamiche EIP-8024, i tipi
di operazione e i delta stack validi; `EVMEIP8024Immediates.def` possiede
separatamente il decode degli immediate e classifica i 256 byte single/pair.
Con `go -overlay`, l’audit ottiene i veri handler privati `operation.execute` e
percorre una per una le `canonical fork jump tables` e le
`mainnet active/scheduled jump tables`. Registra una famiglia `inactive` e
rifiuta una `partial`. Ogni tabella attiva prova `DUPN`, `SWAPN` ed `EXCHANGE` su tutti gli
immediate (`3x256`) più i `3 missing-operand cases` rispetto alle stesse fonti
dichiarative.

`EVM_HARDFORK_LATEST` ha un solo target canonico. Il closed
`EVMUpstreamForkAliases.def` mappa Prague→Pectra, Osaka e BPO1–BPO5→Fusaka e
Paris/Shanghai/Cancun/Amsterdam/Bogota su se stessi; nomi ignoti falliscono
chiuso. Un `audit_unix_time` registrato guida
`MainnetChainConfig.LatestFork(time)` (deve eguagliare NeverD latest) e il check
alias/probe di `LatestFork(max uint64)`; entrambi gli instruction set sono
confrontati integralmente. Il manifest fissa `authority=official-fresh-fetch`,
URL ufficiale, `HEAD` richiesto e SHA. Il probe usa `GOTOOLCHAIN=local`.

Il probe Go e il controller Python applicano
`input/collection/string hard limits`; input, collection o stringhe
sovradimensionate falliscono in modo chiuso. Per `bounded diagnostic output`,
una visualizzazione troppo lunga include il `digest` completo e un
`explicit truncated marker`. Output e deadline limitati valgono per ogni child;
al superamento viene terminato l’intero `process group`/process tree e vengono
drenate le pipe.

La ricevuta schema 3 corrente registra `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, revisione
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, `Go 1.24.0` locale,
`stack_limit=1024` e `diagnostics=[]`. Copre `21 fork tables` e
`20 Rules probes` con `15 mapped/4 no-op/1 expected-error`. Entrambi i record
`mainnet active/scheduled` riportano `upstream BPO2`, mappato in modo chiuso a
`NeverD Fusaka`. Dei `23 table targets`, soltanto `Amsterdam/Bogota` sono attivi:
`1536 candidate executions` e `6 missing-operand cases`. I
`three handler symbols` coincidono sui due target attivi. Audit Python `67/67` e
`C++ Opcode 10/10` sono passati. Il run macOS reale è riuscito sotto
`sandbox-exec`, con il `go run` finale offline; Linux richiede `bubblewrap`.

Tutte le fasi Go — `go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` e `go run` — attraversano il filesystem sandbox
`capability-root`. Legge solo probe privato, geth fresco, `resolved GOROOT`
validato e le precise root runtime di sistema necessarie, e scrive solo nelle
root isolate dell’ambiente. La rete è concessa soltanto alle fasi dependency
necessarie; il run finale è offline. I test esigono il rifiuto dei sentinels in
`host HOME/workspace` e l’assenza del loro contenuto dall’output. Linux verifica
la stessa policy `bubblewrap` senza `/` broad bind.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

Gli undici target EVM attualmente registrati da CMake sono:

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` esaurisce tutti gli input di due byte per ogni
fork che cambia il decoder, confronta il decode completo e i confini `JUMPDEST`
esatti e passa input ostili deterministici di lunghezza limitata in tutti i fork.

Per modifiche al control flow EVM, esegui prima il contratto di punto fisso e
dominio delle altezze:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Questi casi coprono return tra block, merge finiti multi-target, convergenza,
ordine deterministico, lane dell’intero stack sensibili al percorso,
correlazione preservata, jump sconosciuti, target esattamente non validi, budget
fail-loud e stack fault. `MayReachable` conserva solo un candidato CFG e non
produce fatti certi. Esegui poi tutti gli undici target EVM e l’audit live upstream.

Per modifiche al dataflow MedIR/HighIR, esegui anche i contratti constant-phi,
selector, typed-operand, malformed-graph e deep-chain:

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

Questi casi provano phi ciclici uguali e in conflitto, espressioni selector non
adiacenti e tra block, entrambi gli ordini degli operandi di uguaglianza, check
esatti delle width ABI, operandi tipizzati storage/event/calldata, gestione
deterministica di MedIR malformed e un producer walk iterativo su 16.384 valori.

## Come vengono prodotte le fixture

### Fixture di lift e formato

`unittests/lift/CMakeLists.txt` compila sorgenti C e assembly per più target
durante la build. Le triple Clang producono oggetti ELF x86-64, i386, AArch64 e
ARM32, oggetti e immagini collegate PE/COFF e oggetti Mach-O i386 PIC/no-PIC.
Quando è disponibile LLD, alcuni oggetti vengono anche collegati in eseguibili
per i test patch. `NeverDLiftTests` dipende dal target `lift-test-objects`,
quindi una build normale di quel binario aggiorna le fixture generate.

La maggior parte dei test lift usa `NeverDLiftFixture.h` per invocare la CLI
`neverd` compilata e ispezionare LowIR, MedIR, HighIR, LLVM IR, C generato o un
binario riscritto. La variabile d’ambiente `NEVERD` può sovrascrivere il percorso
della CLI per un esperimento manuale mirato; le normali esecuzioni CTest usano
l’eseguibile incorporato da CMake.

### Fixture di sicurezza della memoria

`unittests/safety/fixtures/binaries` contiene immagini PE, ELF e Mach-O
versionate per x86-64 e AArch64, insieme al PDB o al dSYM che ciascun formato
fornisce e a un MAP del linker per ogni immagine. Il MAP è ciò che una build
spogliata continua a distribuire, quindi ogni cella viene analizzata anche
indicando il MAP in modo esplicito, il che fissa che cosa un risultato può
ancora affermare quando non restano né tipi né righe sorgente.
`NeverDSafetyIntegrationTests` esegue tutte e sei le celle su ogni host; la
configurazione fallisce se manca un’immagine o un file di accompagnamento
richiesto, e la suite non ha alcun percorso di salto legato alla toolchain
dell’host.

I binari equivalenti derivano da un unico file sorgente. Ricostruisci la fixture
smoke nativa dell’host con `make`, oppure rigenera l’intera matrice versionata
con:

```bash
make -C unittests/safety/fixtures matrix
```

La ricetta della matrice richiede i target incrociati Linux e Windows di Clang,
gli strumenti COFF di LLD, entrambe le architetture Darwin e `dsymutil`. I suoi
percorsi di debug vengono rimappati e la registrazione della riga di comando
CodeView è disattivata, così i file di accompagnamento versionati non catturano
il percorso assoluto dell’area di lavoro di chi sviluppa.

### Ricostruzione delle eccezioni Windows

Le modifiche alle eccezioni Windows basate su tabelle richiedono sia test della
rappresentazione sia un test di patch su un PE collegato. Il filtro lift mirato
copre il modello unwind/SEH/C++ normalizzato, gli input corrotti, gli archi CFG
eccezionali, HighIR, la generazione LLVM WinEH, la sostituzione della directory
delle eccezioni e la ricostruzione Guard CF/EH continuation:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

La fixture assembly x64 protetta richiede il target Windows di Clang e
`lld-link`; il link CMake usa `/guard:cf` e `/guard:ehcont`. Uno skip dovuto a
un cross-linker mancante non dimostra il percorso final-image. Un caso di
integrazione riuscito prova che il PE riscritto può essere ricaricato e che le
tabelle runtime-function, unwind, load-config, Guard CF e Guard EH continuation
restano ordinate, presenti nel file e limitate a target eseguibili.

La fixture FH3 collegata copre indipendentemente la chiusura C++ nativa: tabelle
di stato fisse, annotazioni HighC, conservazione della personality, target catch
generati e grafo IP-to-state ricaricato.

Vedere [Ricostruzione delle eccezioni Windows](windows-exception-reconstruction.md)
per la matrice di supporto analisi/nativo e il contratto di patch fail-closed.

### Modelli di eccezioni per linguaggio

Tutto ciò che non è il modello tabellare di Windows sta in un unico target
mirato. `NeverDLanguageEHTests` copre la catena di frame DWARF, l'area dati
specifica del linguaggio di Itanium, ARM EHABI, il compact unwind di Darwin, i
metadati di frame del runtime Go, la macchineria di panic di Rust e i tre
runtime Objective-C:

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

Le tabelle di questa suite sono assemblate byte per byte anziché compilate,
perché la maggior parte delle combinazioni da verificare non viene emessa
insieme da nessuna singola toolchain. Objective-C è il caso più netto: i tre
runtime emettono tutti una LSDA Itanium e differiscono solo per ciò che sta in
uno slot della tabella dei tipi, e quella differenza è totale, non di grado. Lo
slot di Apple indirizza un `objc_typeinfo` i cui primi due campi imitano
deliberatamente `std::type_info`; quello Objective-C++ di GNUstep indirizza una
vera sottoclasse di `std::type_info`; e quello del runtime GNU non è nemmeno un
puntatore, ma la stringa del nome della classe. Applicare la convenzione di un
runtime alla tabella di un altro non fallisce: riporta un nome di classe letto
dal mezzo di qualcos'altro. Per questo il runtime viene stabilito dalla
personality del frame prima di leggere qualsiasi slot.

La stessa suite fissa due distinzioni facili da confondere e sbagliate una volta
confuse. `@catch(id)` e `@catch(...)` sono gestori diversi — il primo accetta
qualsiasi oggetto Objective-C e lascia proseguire un'eccezione estranea — e ogni
runtime li scrive in modo diverso; un decodificatore che riporti entrambi come
catch-all mette un gestore su eccezioni che in realtà sarebbero passate oltre. E
una tabella di call site setjmp/longjmp indicizza i punti di chiamata invece
degli indirizzi: un lettore che non riconosca una delle personality SJLJ non
fallisce, ma inventa intervalli protetti e landing pad che il programma non ha
mai nominato.

Riconoscere quella forma non equivale a rifiutarla. Una voce SJLJ è una coppia
di valori ULEB128 — un selettore di dispatch e uno scostamento di azione — e
quello scostamento significa lì esattamente ciò che significa nella forma a
indirizzi: la catena di azioni, i tipi catturati e le specifiche di eccezione
si leggono quindi tutti da una tabella che non nomina alcun codice. Resta
ignota soltanto la regione che ciascuna voce protegge, perché a enunciarla sono
le scritture che la funzione stessa compie nel proprio slot di call-site, non
qualcosa nella tabella. La suite fissa anche l'unico byte di cui qui non ci si
deve fidare: GCC scrive `DW_EH_PE_uleb128` come codifica delle call-site e LLVM
scrive `DW_EH_PE_udata4`, entrambi emettono poi ULEB128 comunque, e nessuna
personality lo legge mai — quindi non deve leggerlo nemmeno un decodificatore.

L'identità della personality è fissata accanto a questo, perché è ciò che
decide come si legge ognuna delle tabelle qui sopra. GNAT nomina la propria
routine nei tre modi in cui GCC nomina quella di ogni frontend — `_v0`, `_sj0`,
`_seh0` — e su Windows registra un simbolo mentre inoltra a un altro, così
tutte e quattro le grafie devono ricadere su Ada. D ne è l'immagine speculare:
tre compilatori, tre nomi per una sola routine, un unico insieme di tabelle
alle spalle.

### Roundtrip differenziali Unicorn

La fixture semantica verifica il comportamento anziché la forma testuale:

1. Scrivere un piccolo caso C/assembly o costruire LLVM IR.
2. Compilarlo con Clang/LLVM per il target richiesto.
3. Eseguire il codice macchina originale in Unicorn e acquisire il ritorno previsto o altro stato definito dalla fixture.
4. Caricarlo e fare lift con NeverD, emettere LLVM IR e ricompilare il risultato in codice macchina.
5. Eseguire il codice rigenerato con stessa ABI, input, layout di memoria e modello CPU.
6. Confrontare i risultati osservabili.

L’implementazione principale è
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h).
La fixture patch-full usa `Codegen::compileForRewrite`, lo stesso backend di
riscrittura delle operazioni patch, poi confronta codice baseline e trasformato
sull’intera griglia ISA/formato 4×3.

Un errore semantico deterministico di NeverD deve far fallire il test. Riserva
gli skip a limiti espliciti di capacità esterna e leggine il motivo: un riepilogo
verde senza cross-linker non prova che il percorso del formato sia stato
eseguito.

### Backend differenziali EVM

I test interpreter forniscono un oracle deterministico a 256 bit. La suite
emitter compila ed esegue LLVM, abbassa C23 con Clang sullo stesso host harness
e, se sono disponibili `solc`, `anvil`, `cast` e `jq`, deploya Solidity generato
in locale. Confronta status, storage e trace count. Un corpus raw separato esegue
ALU pre-Fusaka, copie calldata/memory, `MCOPY` sovrapposto, Keccak e return data
nell’EVM nativa di Anvil.

I test Low/Med conservano execution lane whole-stack sensibili al path e
l’identità della lane nei phi; esaurire un budget, incluso
`MaxAbstractInstructionTransfers`, è un hard error. Strict rifiuta un opcode
ignoto o inattivo soltanto su una lane provata `Reachable`; `MayReachable` non
produce fatti definitivi. HighIR vincola selector, receive e fallback alla lane
radice e a terminali riusciti. Un selector condiviso non è evidenza indipendente
di standard: solo una `KnownFunctionVariantInfo` dello standard e una forma di
ritorno esatta concordata da tutti i terminali riusciti consentono di scegliere
variante e lista dei ritorni.

L’interpreter esegue il preflight tipizzato dello stack prima di ogni effetto
specifico dell’opcode. `EVMForkSemantics.def` definisce il byte `0x44` come
`DIFFICULTY` prima di Paris e `PREVRANDAO` da Paris. `REVERT`, fault, step limit
ed esaurimento delle risorse ripristinano lo stato della transazione. Un errore
di allocazione è `ExecutionFaultKind::ResourceExhausted`; se non si può creare
lo snapshot d’ingresso, `HasPersistentStateSnapshot` è false e il risultato non
può essere committed.

### Regressioni dei confini pubblici e dei budget EVM

I test delle API pubbliche alterano separatamente
`Code`/`Fork`/`Instructions`/`JumpDestinations` canonici e ogni tabella, range,
ID, lane e riferimento edge di LowIR. `execute` deve restituire `llvm::Error`
prima del lookup delle istruzioni; `lowerToMedIR` deve rifiutare tutto il LowIR
malformed o fuori budget prima di costruire indici o allocare output
proporzionale all’input. Per `lowerToMedIR`, i test impongono validation di
options, risorse e struttura prima del `canonical decode replay` field-by-field
e prima di `lowerCanonicalLowToMedIR`. Il recovery HighIR pubblico replay-verifica
LowIR/MedIR esterni; soltanto `analyze` usa `lowerCanonicalLowToMedIR` e
`recoverCanonicalHighIR` sul proprio IR canonico senza replay ricorsivo o
duplicato, ma con tutti gli HighIR option/resource budgets. L’interpreter prova poi il confine esatto e +1 per ogni
limite di `EVMInterpreterLimits.def`: `MaxSteps` mantiene il suo `StepLimit`;
l’esaurimento di `MaxMemoryBytes`, `MaxTraceEntries`, `MaxLogEntries`,
dell’aggregato `MaxLogDataBytes` o di `MaxPersistentStateEntries` runtime
restituisce `ResourceExhausted` e ripristina gli effetti transazionali. Un
aggregato iniziale `MaxHostReturnDataBytes` o persistent state troppo grande è
un errore API. Anche `MaxCalldataBytes`, l’aggregato
`MaxHostEnvironmentEntries` su `BlockHashes`, `Balances`, `CodeHashes`,
`ExternalCode`, `BlobHashes` e l’aggregato `MaxExternalCodeBytes` sono errori
API. Il `const execute preflight` li rifiuta prima di copiare environment,
snapshot o result. Sono coperti anche view return-data `ArrayRef` e lookup
`lower_bound` su tabella ordinata, senza copia di buffer né PC map.

Test LowIR separati coprono i limiti diagnostic aggregati `MaxLowDiagnostics` e
`MaxLowDiagnosticBytes`: decode lineare e costruzione CFG preaddebitano count/
byte finali esatti e rifiutano zero.
I test di sicurezza HighIR coprono il dominio ordinato per lane
`Any/Exact/Excluded`, match/esclusione dell’uguaglianza, false-edge match e
true-edge mismatch di un `XOR(selector, constant)` grezzo, raffinamento di word
zero/calldata size/call value e condizioni unknown fail-closed. I test al confine
esatto e -1 coprono, da `EVMAnalysisLimits.def`,
`MaxHighDispatchCandidates`, l’aggregato `MaxHighRecoveredArguments`,
`MaxHighDiagnostics`, `MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`,
`MaxHighMemoryTransferCells` e `MaxHighMemoryValueVisits`. Ogni diagnostic
emesso, incluso quello fisso per malformed, deve addebitare count e byte finali
prima dell’allocazione.
I budget diagnostic LowIR e HighIR sono testati separatamente; la root CFG
region predefinita deve addebitare `MaxHighRegionBlockReferences` prima di
reserve o copia dei block PC.
Le regressioni di function scope coprono i back-jump `EQ` e `raw XOR` al
dispatcher condiviso. Verificano che un’altra funzione non contamini
`arguments`, `mutability`, `return shape` o `region`, mantenendo raggiungibili
body condivisi e tail call.
Gli esiti esterni CALL/CREATE sono provati come outcome host non deterministici
su entrambi gli edge CFG precisi, preservando la recovery del fallback ERC-1167.
Una condizione selector illeggibile resta Unknown e non può inventare fatti
fallback o function.

I test CFG derivano `InvalidJumpDestination` da `EVMLowFaultKinds.def` per un
`end-of-code JUMPI`: true certo verso un target invalido non ha coda di successo
ed è un fault certo; false certo ha successo; unknown conserva il possibile
percorso false di successo senza marcare tutta la lane come fault certo.

I test ABI applicano al limite esatto e +1 i confini grammaticali di
`EVMABIParserLimits.def` e quelli di cardinalità/testo delle tabelle pubbliche
di `EVMABITableLimits.def`. Rifiutano inoltre enum kind/standard/evidence
invalidi, metadata incoerente, signature/return non canonici, selector condivisi
marcati erroneamente independent, variant dangling o duplicate e un event-topic
`APInt` di width non-word prima del lookup selector indicizzato o del lookup
topic ordinato.

`NeverDEVMOpcodeTests` impone anche l’architettura metadata: ogni opcode assegnato
fa roundtrip tra encoding e valore tipizzato; vengono testati confini di famiglia,
alias hardfork e massimi stack/host derivati.

### Backend differenziali Solana SBF

I test dei metadati SBF convalidano ogni funzionalità di versione, i confini di collisione degli opcode, gli hash syscall Murmur3, le rilocazioni e le costanti di machine ELF, registro e indirizzo VM. Le fixture del loader generano, senza binari inclusi, sia layout legacy v0-v2 a sezioni sia layout rigorosi v3/v4 senza sezioni e basati sui program header.

`NeverDSBFISAConformanceTests` verifica ogni byte encoding per ciascuna versione
v0-v4 rispetto a un manifest tipizzato sottoposto ad audit indipendente.
`NeverDSBFExternalOracleTests` confronta poi le decisioni di attivazione e di
confine con un processo Anza ufficiale costruito separatamente.
`NeverDSBFUpstreamConformanceTests` assegna un esito esplicito a tutti i 23 ELF
alla revisione Anza fissata.

`NeverDSBFSemanticTests` esegue direttamente byte di istruzioni verificati e non usa MedIR; modificare o corrompere l’IR normalizzato non può quindi far concordare accidentalmente il source oracle con un backend. Copre la semantica v2 non monotona, memoria, syscall, frame di chiamata interni, fault, trace e limiti di risorse. I moduli LLVM vengono verificati; il C generato è compilato con i warning come errori e Rust con `-D warnings`. I test dell’API pubblica attraversano tutti gli stadi IR, disassembly, CFG, metadati, LLVM, C e Rust partendo da un ELF SBF rigoroso generato.

## Target in un solo comando

I target personalizzati compilano le dipendenze e poi eseguono CTest con
parallelismo derivato dalle CPU host:

| Target CMake | Selezione |
|--------------|-----------|
| `check-neverd` | Tutti i test registrati |
| `check-neverd-semantic` | Solo `NeverDSemanticTests` |
| `check-neverd-sbf` | Tutti i target/casi `NeverDSBF*Tests` |
| `check-neverd-patch-full` | Solo `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | Solo `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | Solo `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | Solo `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` e `NeverDAvxUpperXformTests` al momento non hanno un
target di comodità `check-neverd-*`. Compilali e selezionali per label come
mostrato sotto. `check-neverd-semantic` inoltre non include i binari separati di
trasformazione o patch-full; usa `check-neverd` per l’aggregato completo.

## Flusso CTest incrementale

Compila prima l’eseguibile proprietario e poi selezionane la label. Evita così
di ricollegare grandi target semantici non correlati.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# Tutti i target/casi EVM mirati
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Tutti i target/casi Solana SBF mirati
cmake --build build-release --target check-neverd-sbf --parallel 4
```

Usa un nome CTest derivato da GoogleTest per una singola regressione:

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

Selettori utili:

| Comando | Scopo |
|---------|-------|
| `ctest --test-dir build-release -N` | Elencare i casi scoperti senza eseguirli |
| `ctest --test-dir build-release -L '<regex>'` | Selezionare una label di binario test |
| `ctest --test-dir build-release -R '<regex>'` | Selezionare nomi di casi |
| `ctest --test-dir build-release --output-on-failure` | Mostrare diagnostica solo in caso di errore |
| `ctest --test-dir build-release --stop-on-failure` | Fermarsi dopo il primo errore |
| `ctest --test-dir build-release --parallel 4` | Eseguire fino a quattro casi in parallelo |

La discovery GoogleTest usa `DISCOVERY_MODE PRE_TEST`, quindi il binario
corrispondente deve esistere prima che CTest lo enumeri. I timeout per caso e di
discovery separati sono definiti in `cmake/AddNeverD.cmake` e vanno ampliati
solo per suite con casi pesanti misurati.

## Quali test cambiano con il codice?

| Area di modifica | Iniziare da | Poi considerare |
|------------------|-------------|-----------------|
| Lifter di architettura o decode | Caso nominato in `NeverDLiftTests` | Roundtrip semantico dell’ISA corrispondente |
| CFG LowIR, scoperta funzioni, jump table | Casi lift CFG/switch | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests` o `NeverDTwoTableXformTests` |
| MedIR, ABI, flag, tipi, SSA | Casi lift MedIR/convenzione di chiamata | Casi `NeverDSemanticTests` multi-ISA |
| HighIR o C strutturato | Casi HighIR/decompile | `NeverDCFGLoopXformTests` e compilazione del C generato |
| Loader PE/ELF/Mach-O o relocation input | Fixture formato corrispondente in `unittests/lift` | Test caricamento/decompilazione di tutte le fasi per la cella |
| Codegen di riscrittura o relocation output | Casi `RewriteCodegenRTTests` | `NeverDPatchFullTests` e fixture patch collegata se disponibile |
| Trasformazione LLVM IR usata da patch | Binario di trasformazione mirato | Griglia di pass composti `NeverDPatchFullTests` |
| C API o CLI | Test SDK/query diretto e `unittests/semantic/CLIEndToEndTests.cpp` | Suite pipeline/formato pertinente |
| Loader, opcode, IR o backend EVM | Target proprietario `NeverDEVM*Tests` più piccolo | Tutti i target EVM e compilazione del C/Solidity generato |
| Loader, ISA, IR o backend SBF | Target proprietario `NeverDSBF*Tests` più piccolo | Tutti i target SBF e compilazione del C/Rust generato |
| Riconoscimento libc | `NeverDLibCTests` | Casi semantici call/ABI se cambia il comportamento |
| Audit di vita dell’heap o hunt di overflow di copia | `NeverDSafetyTests` | Tutte le sei celle di `NeverDSafetyIntegrationTests` |
| Esecuzione o quoting di processi | `NeverDTestProcessTests` | Un caso CLI/semantico interessato su ogni host supportato |

I test devono esprimere il contratto al confine stabile più basso. Un test della
forma LowIR è utile per attribuire il lifter; serve un roundtrip semantico se due
forme IR plausibili possono comportarsi diversamente. Evita golden dump di
intere funzioni quando basta una piccola assertion su opcode, CFG o stato
osservabile.

## Relazione con la CI

La CI compila Release con test abilitati su Linux, macOS e Windows, controlla
l’inventario scoperto e poi applica esclusioni di label specifiche della
piattaforma. I profili sono in `.github/workflows/ci.yml` e
`scripts/audit_ci_test_inventory.py`. `NeverDSafetyTests` e
`NeverDSafetyIntegrationTests` sono obbligatori su ogni host della matrice;
ogni esecuzione legge le stesse fixture PE, ELF e Mach-O versionate per x86-64
e AArch64. Poiché nessuno shard della matrice rappresenta tutte le suite
costose, un `check-neverd` locale resta il segnale pre-merge completo più chiaro
quando la macchina dispone di tutti gli strumenti cross richiesti.

## Profilo corrente di conformità e sanitizer Solana SBF

Questa lista aggiornata sostituisce la lista SBF abbreviata precedente. La
suite source differential richiede `rustc` oltre a clang; uno skip del compiler
indica coverage mancante. L’aggregato completo include
`NeverDSBFProgramImageTests`, `NeverDSBFMalformedCorpusTests`,
`NeverDSBFISAConformanceTests`, `NeverDSBFUpstreamConformanceTests`,
`NeverDSBFLLVMDifferentialTests` e `NeverDSBFSourceDifferentialTests`, oltre ai
target metadata, loader, analyzer, semantic, emitter e integration. Il profilo
integrato registra target e risultati nominati, non un totale che cambia presto.

Il profilo sanitizer viene costruito separatamente in `build-sbf-asan-ubsan`.
Il package prebuilt fissato per revisione include l’header fork-only richiesto,
quindi integration gira nello stesso profilo ASan/UBSan fail-fast.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

### Snapshot di evidenza SBF fissato (2026-08-24)

La gate fissa Anza `sbpf` a
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave a
`ef210d67f2fabeee1730498188fa78854260c679` e Solana SDK a
`122f32e571ce39face4beffaccea733e37c207fd`. Il manifest ELF ufficiale passa
23/23; `NeverDSBFExternalOracleTests` confronta 1,411 casi opcode/boundary via
`SBFOfficialOracleProtocol.def`, `SBFOfficialVerifierCases.def` e
`SBFOfficialExecutionConstants.def`.
`SBFOfficialELFMutations.def` è il contratto tabellare degli ELF malformati; il
totale variabile non viene fissato.
Separatamente, il `41-case strict ELF differential` esegue l’intera matrice
strict-v3 tramite `verify-elf-batch` ufficiale e NeverD; i suoi 41 casi non
fanno parte del totale 1,411.

La matrice di esecuzione ufficiale aggiuntiva resta separata: esattamente 508
casi attivi `(Version,Opcode)` più 58 casi di confine producono 566 casi di
esecuzione esatta. Non sostituisce né rientra nelle 1,411 probe del verifier o
nel `41-case strict ELF differential`.
`NeverDSBFAgaveConformanceTests` autentica Firedancer test-vectors
`68bb4af40235562e8852fa23d5727e49c2a0b862` e confronta tutti i 1,955 `sol_compat_elf_loader_v1` fixture
del loader (1,399 accettati, 556 rifiutati). Per ogni ELF accettato confronta
`entry_pc`, `text_off`, `text_cnt`, `rodata_hash` e `calldests_hash`. Questa gate non esegue il successivo
instruction verifier.
La Linux Release CI usa `--print-pinned-revision`,
`--print-test-vectors-revision` e `--print-toolchain`, ed esporta
`NEVERD_SBPF_ORACLE` e `NEVERD_AGAVE_CONFORMANCE_ROOT`, rendendo obbligatorie
entrambe le gate esterne. In locale, senza env oracle/corpus esplicito, i casi
vengono scoperti ma possono essere saltati.

`SBF_RUNTIME_VERSION` rende `RuntimeVersionPolicy::ChainProfile` dipendente dal
cluster/slot storico: i feature account ufficiali fanno avanzare l’ISA massimo
da V0 a V1, V2 e V3; oggi resta V3. v4 esplicito usa
`RuntimeVersionPolicy::UpstreamToolchain` per analisi
offline. Il limite corrente di 10 MiB è esattamente `10'485'760` byte; 65,536 è
solo provenance/test storico. `SBFFaultCodes.def` stabilizza i valori degli
execution fault e `SBFSourceStatuses.def` possiede separatamente l’ABI source.

Fixture in scala 10,000 proteggono worklist, function ownership e multi-latch
senza fissare tempi di macchina. Le righe cluster/account/slot consentono un
`RPC activation audit` mentre i test ordinari restano deterministic e offline.

## Prestazioni dell’inventario delle classi Android

Compila `NeverDMobileTests` in Release ed esegui la relativa etichetta prima di
misurare `neverd mobile INPUT --list-classes`. I test del lettore coprono metadati
sparsi, Unicode, riferimenti non validi, corpi di metodo non supportati, checksum
e budget; quelli degli archivi distinguono l’estrazione completa dalle query sui
payload selezionati. I test CLI verificano filtri di prefisso, ambito JSON,
preservazione dell’output e atomicità degli errori multidex.

Il sistema indipendente di fixture e misurazione convalida l’intero inventario
dei descrittori di ogni processo prima di accettare un campione temporale:

```sh
python3 -m unittest scripts.tests.test_benchmark_mobile_inventory -v
python3 scripts/benchmark_mobile_inventory.py \
  --output-dir /tmp/neverd-inventory-benchmark \
  --class-count 6000 --dex-count 3 --code-units 64 \
  --extra-string-bytes 8388608 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

Usa una nuova directory di output per ogni esecuzione. `--generate-only` scrive
fixture e manifest senza misure temporali. `--workload` seleziona i tipi di input
comuni per un eventuale `--peer-command 'tool {input} {prefix}'`; la preparazione
dell’input resta fuori dal comando misurato. I report conservano hash, comandi,
tutti i campioni da nuovi processi, ipotesi sulla cache calda e, su Linux con GNU
time, il massimo RSS del processo figlio. Questo RSS non è il picco combinato di
uno strumento multiprocesso. Gli APK sintetici sono contenitori per query, non
app installabili. La velocità dell’inventario non dimostra né quella della
ricerca dei riferimenti né la qualità del recupero Java.

Sulle CPU ibride, vincola il sistema di misura e i figli che ne ereditano le
impostazioni alla stessa CPU consentita (per esempio `taskset -c 4 python3 ...`
su Linux) per non mescolare core ad alte prestazioni e a basso consumo. Il report
registra l’affinità CPU ereditata.

## Prestazioni dei riferimenti nel codice Android

Le query sui riferimenti condividono i confini delle istruzioni e la convalida
del codice del lettore di recupero. Esegui la suite mobile dopo modifiche a
questo confine. I test del lettore coprono tipi di pool degli operandi, modalità
di corrispondenza, appartenenza dei metodi e codice condiviso, payload/immediati
che imitano riferimenti, input malformati e limiti delle risorse. I flussi di
debug condivisi vengono verificati rispetto a frame, estensione e parametri di
ogni corpo proprietario. Mantieni nella copertura dei limiti di memoria sia
grandi inventari dei membri sia corpi densi di salti; indici persistenti e
crescita dei contenitori temporanei hanno durate diverse.
Controlla anche elementi riordinati e sovrapposti, codice condiviso con prototipi
incompatibili della stessa larghezza, memoria di input non allineata e
corrispondenze di sottostringhe che attraversano i confini dei blocchi di ricerca.
Le ottimizzazioni dei dati privati del decoder devono preservare i modelli di
recupero completi con dati propri e i multinsiemi delle occorrenze dei
riferimenti, incluso il comportamento di errore per metadati di recupero non
supportati. Distingui le aspettative generate indipendentemente dall’accordo
tra strumenti su input reali.

Il sistema indipendente per i riferimenti registra le occorrenze attese mentre
emette le istruzioni. Controlla identità complete dei metodi, PC in unità di
codice, opcode, identità delle destinazioni, unità UTF-16 e molteplicità a ogni
esecuzione misurata. Verifica anche i conteggi di copertura attesi di NeverD,
determinati indipendentemente, e `code_scan_complete`:

```sh
NEVERD_REFERENCE_TEST_BINARY="$PWD/build-release/bin/neverd" \
  python3 -m unittest scripts.tests.test_benchmark_mobile_references -v
python3 scripts/benchmark_mobile_references.py \
  --output-dir /tmp/neverd-reference-benchmark \
  --class-count 500 --methods-per-class 64 --matching-methods 2 \
  --dex-count 3 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

Usa `--kind` e `--workload` per selezionare i casi. `--extra-strings 65536`
esercita veri indici di stringa a 32 bit. I payload esca sono abilitati per
impostazione predefinita; `--no-payload-lookalikes` mantiene disposizione e
riferimenti reali, sostituendo i valori esca nei payload per confronti su input
comuni. Conserva sia i risultati di correttezza sia quelli temporali. Una query
che restituisce falsi riferimenti ai payload o omette riferimenti reali non
supera la convalida e non riceve una misura temporale accettata.

L’opzione facoltativa `--peer-command` accetta un modello argv contenente
`{input}`, `{kind}` e `{query}`. Adatta esplicitamente la sintassi della query
quando un altro strumento usa semantiche diverse e confronta multinsiemi
completi delle occorrenze. Il suo ambito di convalida dichiarato viene conservato
senza affermare una scansione completa del codice. Si applicano le stesse
precisazioni su directory nuove, affinità CPU, processi nuovi, cache calda e RSS
del benchmark dell’inventario. Il test CLI facoltativo viene saltato se
`NEVERD_REFERENCE_TEST_BINARY` non indica l’eseguibile compilato; segnala il test
saltato.

## Evidenze delle esportazioni degli SDK mobili

Il workflow manuale `Mobile SDK Export Evidence` esegue `collect_mobile_ios_sdk_declarations.py --exports-only` con gli SDK Xcode fissati. Conserva senza modifiche le mappe del linker di Foundation, CoreFoundation e UIKit degli SDK iOS per dispositivo e simulatore, con destinazione, versione SDK, hash delle impostazioni SDK, dimensione e SHA-256. Anche il normale raccoglitore di dichiarazioni conserva queste mappe. File mancanti, vuoti, troppo grandi o esterni allo SDK fanno fallire la raccolta, mantenendo le evidenze già completate. Le mappe attestano le esportazioni dei simboli, ma non dimostrano una ABI di chiamata né il recupero di un metodo.

## Prove ABI delle stringhe Swift per dispositivi mobili

Il workflow manuale `Mobile Swift String ABI Evidence` compila sonde Swift fisse per uguaglianza e ordinamento e una sonda C con `swiftcall`, usando Xcode 26.5 per dispositivi iOS e simulatori arm64. `collect_mobile_swift_string_abi.py` conserva sorgenti, LLVM IR, assembly, identità dei compilatori, impostazioni SDK e `libswiftCore.tbd`, con gli hash. Entrambi i linguaggi devono mostrare l’esatta importazione di confronto con cinque argomenti e risultato `i1`; C deve estendere esplicitamente tale risultato a un byte. Destinazioni o firme errate, comandi falliti e timeout conservano le prove parziali e fanno fallire la raccolta. Queste prove non installano dichiarazioni runtime né dimostrano il recupero di metodi. Test senza SDK: `python3 -m unittest scripts.tests.test_mobile_swift_string_abi`.

## Semplificazione MBA modulare

`SymSimplifyFinite.*` verifica domini completi a due valori da 8 a 512 bit, i costi degli usi condivisi, tutte le annotazioni supportate che possono generare poison, letture volatile e freeze indipendenti, undef/poison espliciti, visite iterative profonde, budget e marcatura di offuscamento. IR originale e semplificato vengono eseguiti a O0/O2 contro un oracolo indipendente per tutti i valori di byte e ingressi casuali a larghezza piena. I test degli oggetti tradotti richiedono identità di cache diverse per budget di valori finiti diversi.

`SymSimplifyPredicates.*` enumera esaustivamente offset, segni e ingressi a quattro bit, verifica composizioni booleane di intervalli e insiemi disconnessi ed esegue oracoli indipendenti a un byte e larghezza completa a O0/O2. Copre annotazioni poison, ingressi indefiniti nascosti alle confluenze, letture/freeze indipendenti, PHI di ciclo conservati, convenienza degli usi condivisi, lavoro cumulativo, molti usi, limiti di ricorsione e marca di offuscamento. Entrambe le chiavi della cache distinguono i budget dell’analisi dei predicati.

`SymExpr.*` verifica finestre costanti sopra il bit inferiore con tutte le combinazioni di ingressi e maschere a quattro bit, valori ampi, strutture annidate, ricomposizione di byte noti e ignoti e controesempi di riporto. Le regressioni di budget collocano un nodo ampio al limite di ricorsione e rifiutano copie di costanti troppo grandi. Le finestre ignote devono restare simboliche senza espandere il DAG. `SymState.*` distingue inoltre le costanti scalari dedotte dai fatti letterali delle regioni in entrambi gli ordini dei byte, senza ampliare il DAG né modificare le parole memorizzate complete.

`SymReadability.*` copre la forma di sottrazione e complemento, il costo degli operatori associativi, letterali a un bit e larghi, saturazione degli alberi condivisi, scelta dei candidati con budget ed equivalenza esaustiva a tre bit senza campionamento. `SymMBASample.*` confronta la verifica stretta e a precisione arbitraria con il valutatore AP per tutti gli operatori, assegnazioni deterministiche e input larghi inutilizzati. Per confrontare la qualità tra versioni del punteggio occorre ricontare entrambe le uscite con la stessa metrica; i contatori di dimensione dell’SDK servono solo alla diagnostica.

## Matrice di test ARM32 e propagazione del frame

```sh
cmake --build build-release --target NeverDSymbolicTests \
  NeverDSymSimplifyGuardTests NeverDLiftTests NeverDMBASourceTests \
  NeverDHighCStoreForwardingTests NeverDMetadataJSONTests --parallel 4
build-release/bin/NeverDSymbolicTests
build-release/bin/NeverDSymSimplifyGuardTests
build-release/bin/NeverDLiftTests \
  --gtest_filter='HighSymSimplify.*:HighFrameStoreForwarding.*:ELFARM32ModeTest.*'
build-release/bin/NeverDHighCStoreForwardingTests
build-release/bin/NeverDMBASourceTests
build-release/bin/NeverDMetadataJSONTests --gtest_filter='ELFARM32ModeCAPITest.*'
```

La matrice degli spill copre anche x86-32 (ELF/COFF/Mach-O), ARM32 (ARM e Thumb in ELF) e AArch64 (ELF/COFF/Mach-O) con entrambi i backend C. Ricaricamenti ripetuti del frame privato devono ridursi ad addizioni o sottrazioni ed eseguire correttamente coppie di byte, coppie ai confini di parola e parole casuali deterministiche a entrambi i livelli di ottimizzazione. I controlli AST di Clang esaminano intere funzioni spill per operatori MBA residui, distinguendo le espressioni di indirizzo valide. HighFrameStoreForwarding verifica larghezze, mutazioni locali, scritture alla memoria, alias, sovrapposizioni, memoria ordinata, grafi malformati e limiti di espansione. HighCStoreForwarding mantiene vive le definizioni usate da valori memorizzati su quattro architetture, incluse reinterpretazioni floating; SymSimplifyGuard verifica identità e ordine dei load, volatile/atomic e confini poison. ELFARM32ModeTest verifica selezione ARM/Thumb, normalizzazione degli indirizzi, metadati misti e prove contraddittorie. ELFARM32ModeCAPITest verifica errori SDK espliciti e ripristino del decoder dopo il ricaricamento di Thumb; InstructionMode copre decoder, puntatori al codice, branch e codegen. La mancanza di Clang cross-target significa skip, non prova del formato.

`HighBoundPrivateFrameCopies.*`, in `NeverDHighControlFlowTests`, verifica le copie attraverso slot privati dello stack senza fuga dopo il collegamento delle ABI delle chiamate, inclusi rami, riuso degli slot e accordo tra contesti di guardia. Il C generato per x64 e AArch64 viene eseguito con `-O0` e `-O2`, con trap per comportamento indefinito e confronti aritmetici indipendenti. I casi negativi richiedono di conservare la funzione originale in presenza di fuga di indirizzi, ABI sconosciute, alias dello stack mancanti o incoerenti, riassegnazione degli argomenti iniziali, accessi sovrapposti, memoria ordinata o atomica, istruzioni malformate, cicli o budget esaurito. Le normali conversioni di valore non devono diventare copie PHI.

La proiezione del sorgente riconvalida anche gli elenchi di oggetti variadici dopo questa pulizia: ammette ancoraggi di istruzione vuoti e rifiuta effetti nascosti o trasferimenti di controllo. La pulizia sincronizzata ammette una sola vista `int64_t` o `uint64_t` dello stesso ricevitore salvato; restringimenti, conversioni in virgola mobile, aritmetica degli indirizzi e riassegnazioni restano esclusi. Gli insiemi Foundation e le tracce di sblocco normale ed eccezionale vengono eseguiti a `-O0` e `-O2`.

## Eccezioni sincrone native x64

Le `DIV`/`IDIV` checked x64 usano risultati reali del processore e `#DE`. KVM usa una IDT/IST supervisor privata, WHP una bitmap esplicita; contesto originale e codici disponibili restano distinti dagli errori di trasporto. Il sistema operativo consuma l’evento recuperabile prima di impostare la continuazione. I driver Windows traducono divisione per zero e overflow del quoziente in `STATUS_INTEGER_DIVIDE_BY_ZERO`, eseguendo veri filtri SEH, `__finally` e tentativi successivi. `NeverDX64ExceptionTests` compila senza Unicorn; `DriverWDMCPUException` verifica casi WDK originali. Gli host ARM64 non disponibili sono saltati esplicitamente.

## Effetti RAM preparati

`RAMTransaction` conserva soltanto l’unione fisica delle scritture dichiarate di un’istruzione, sotto il blocco di esecuzione. Ripristina la RAM originale prima degli osservatori dei risultati; annullamento, errore di trasporto o eccezione dell’osservatore non pubblicano RAM o registri parziali. Dopo il ripristino della RAM, gli errori CPU mantengono lo stato architetturale di eccezione. Le scritture singole e doppie ARM64 usano la stessa autorità. x64 esegue `XCHG`, `XADD` e `CMPXCHG` a 8/16/32/64 bit, con allineamento naturale per forme bloccate o implicitamente bloccate. `NeverDRAMTransactionTests` confronta i risultati con la CPU host e verifica ripristino, alias e permessi; le piattaforme indisponibili sono saltate esplicitamente. Dispositivi e SMP parallelo restano esclusi; gli snapshot CPU non ripristinano la RAM già confermata.

## Stato x87 completo

`NeverDEmulationArch` possiede i contratti ISA, le tabelle delle pagine e il formato FP condiviso dai trasporti nativi e Unicorn. I contesti x64 conservano controllo, stato, TOP, tag fisici, opcode, puntatori istruzione/dati e otto registri a 80 bit. `FP0`–`FP7` usano `RegisterValue`; gli accessi scalari rifiutano il troncamento. `FPTag` è la maschera fisica dei registri non vuoti. `NeverDX64FPTests` verifica tutti i TOP, operazioni esatte contro FXSAVE/FXRSTOR dell’host e ripristino. Ciò non ammette istruzioni x87 nel contratto checked e non prova tutti gli arrotondamenti. Gli host nativi non disponibili vengono esplicitamente saltati.

`driver-strict` supporta KVM su host Linux x64 compatibili e WHP su host Windows x64 compatibili; `auto` sceglie quel trasporto nativo, mentre ISA diverse usano Unicorn. Unicorn esplicito e la precedente API V1 mantengono il profilo software portabile. L’esecuzione nativa verifica indirizzi canonici ed effetti prima dell’ingresso; hardware assente produce un errore senza ripiego. Istruzioni e comportamento OS non supportati falliscono esplicitamente. La CI nativa Windows x64 con Unicorn disattivato supera tutti i 359 controlli obbligatori: 131 controlli CPU, 224 risultati di driver da 26 immagini integrate, 46 immagini WDK e 40 casi di scenario alle basi preferite e rilocate, più quattro controlli dei limiti SEH ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Mancano prove native ARM64; non è stabilita la compatibilità universale dei driver o Android/Darwin.

La verifica nativa precedente copre i punti di ingresso dichiarati dei driver e gli scenari pubblicati. Le regressioni dettagliate per funzione e i controlli C API/CLI/Python descritti di seguito mantengono prove limitate a Linux, salvo esecuzione Windows esplicitamente documentata; il superamento del corpus nativo non convalida ogni variante di test su Windows.

Interrogare il profilo selezionato con `executionCapabilities(Contract, ISA, Backend)`. `NativeLegacyX64` descrive l’esecuzione nativa dei driver x64. `NeverDNativeDriverTests` verifica il corpus esistente e può essere eseguito anche in una compilazione senza Unicorn.

Il workflow CI esistente esegue l’intera directory dei test di emulazione prima dei profili generali e conserva inventario, risultati JUnit e log CTest in `emulation-focused`. Un errore in altri moduli non impedisce questa esecuzione. Hardware non disponibile e driver opzionali mancanti rimangono salti espliciti; una compilazione o esecuzione software riuscita non prova l’esecuzione nativa.

Su Linux, `NeverDUnicornDeadlineTests` completa il vero thread del timer prima dell’ingresso nel guest tramite una pianificazione pthread controllata. Per x64, ARM32 e ARM64 verifica che l’annullamento preventivo non produca effetti e che l’esecuzione successiva usi un budget indipendente. Utilizza API pubbliche senza modificare lo stato privato del motore.

`X64StateTransition` in `NeverDX64ExceptionTests` esegue letture RAM indipendenti e letture CR8 sulla CPU nativa. Alterna basi TLS e privilegio, riprende dopo errori di divisione ripetuti e cambia TLS dopo un ingresso annullato. Dopo modifiche al trasferimento di stato nativo, eseguire questa etichetta CTest insieme alle verifiche di rimappatura alias, contesti CPU, stato FP e risultati dei driver originali. Un trasporto KVM/WHP non disponibile resta un salto esplicito.


`NeverDKvmRunTests` verifica i trasferimenti presi in prestito di `KvmRunControl` senza richiedere `/dev/kvm`. `StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` controlla che preparazione, acquisizione e ingresso host intercettato usino lo stesso thread, con una sola preparazione durante i tentativi interrotti. Altri casi coprono errore di preparazione senza ingresso, errore di acquisizione, arresto durante la preparazione e annullamento di un ingresso attivo, seguiti da una nuova esecuzione che non può riutilizzare i vecchi callback. Mantenere nella validazione le suite di annullamento reale, rollback RAM, eccezioni e driver originali. `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` verifica che più ingressi con la stessa scadenza riutilizzino il thread, eseguano ogni trasferimento una volta e lascino invariati i pacchetti precedenti.

`KvmHandoffPolicy` limita ogni attesa attiva a 8 μs, passa all’attesa bloccante dopo due tentativi consecutivi senza esito e riprova dopo 256 scambi. Chiamante e worker si adattano indipendentemente; il chiamante rispetta anche la scadenza e il token di arresto originali. Gli indicatori atomici sono soltanto suggerimenti per la pianificazione: il mutex protegge ancora i pacchetti, la durata dei callback e la conferma dell’annullamento. `NeverDKvmRunTests` verifica il limite delle attese improduttive, il recupero, le variazioni di latenza e l’annullamento prima del riutilizzo dei pacchetti.

KVM x64/ARM64 usa `KvmRunControl` per preparare, entrare in `KVM_RUN` e acquisire lo stato sullo stesso worker vCPU privato. La preparazione avviene una volta anche con `EINTR`; annullamento e lettura fallita impediscono la pubblicazione. `KvmAArch64Machine.cpp` esegue manutenzione delle traduzioni e trasferimenti scalari/vettoriali completi con una sola scadenza di passo. Il chiamante pubblica dopo conferma e mantiene decodifica ISA, transazioni RAM, politica OS e osservatori. Le prove native ARM64 restano mancanti.

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` verifica la continuazione e le scritture CPU reali dopo modifiche host ai registri generali, ai due XMM estremi, a MXCSR e al controllo x87. I byte reali di `FXSAVE64` verificano tutti i registri fisici a 80 bit, TOP, tag, opcode e puntatori dopo un ingresso arrestato; anche errori di divisione ripetuti invalidano il riuso. Questi test macchina non ammettono ulteriori istruzioni x87 nei profili checked.

`NeverDKvmStateTransferTests` inietta un errore di lettura dei registri o di XSAVE dopo una vera esecuzione KVM, quindi riprova con l’ingresso invariato. Risultati indipendenti interi e di byte impacchettati provano che un’acquisizione fallita non riutilizza lo stato nativo già avanzato. Solo questo eseguibile intercetta `ioctl`; gli host nativi non disponibili vengono saltati esplicitamente.

`NeverDKvmStateTransferTests` verifica su KVM reale insiemi `KVM_CAP_SYNC_REGS` assenti, singoli e combinati, oltre alle query fallite. `SynchronizedCapturesRemoveOnlySupportedReadIoctls` conta le letture effettive e verifica tutto lo stato CPU dopo passi consecutivi. `CancelledWarmEntryRequiresFreshSpecialStateOnRetry` richiede una nuova lettura dei registri speciali dopo un annullamento. Errori di cattura, tentativi interi/SIMD, ripristino della RAM speculativa e priorità delle eccezioni usano la stessa matrice; la copertura nativa non disponibile viene saltata esplicitamente.

ARM64 checked usa un unico confine per lo stato completo. `Registers.def` definisce 39 campi scalari e 32 vettori da 128 bit; `captureAArch64State` prepara tutte le letture, applica le larghezze e normalizza NZCV prima di pubblicare una sola volta. Unicorn, KVM, WHP e HVF trasferiscono lo stesso inventario, inclusi TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR e FPSR. Gli adattatori nativi abilitano FP/SIMD tramite CPACR_EL1. Letture fallite e ingressi annullati conservano tutto lo stato del chiamante.

L’avvio ARM64 KVM/WHP/HVF esegue il programma privato `AArch64MachineProbe.def`: NOP, somma FP32 arrotondata verso infinito positivo e somma SIMD a due lane. Ogni passo confronta tutti i 39 campi scalari e 32 vettori, inclusi TLS, NZCV, azzeramento dei bit superiori del risultato e stato FPCR/FPSR conservato/cumulativo. Il probe usa solo memoria del monitor supervisor e una scadenza globale. Le sonde verificano soltanto questa inizializzazione limitata. La validazione dei carichi Linux ARM64 KVM e Windows ARM64 WHP resta da completare; i risultati nativi macOS sono nella [guida HVF](macos-hvf.md).

L'inizializzazione nativa x64 KVM/WHP/HVF esegue `X64MachineProbe.def` in pagine supervisor private. Un'unica scadenza copre NOP, somma FP32 arrotondata verso infinito positivo, somma SIMD a due corsie e letture FS/GS e CS/SS/CR8; ogni passo confronta lo stato completo scalare, XMM, x87 fisico e di controllo. Le sonde x64 e ARM64 richiedono il diritto esclusivo di esecuzione della memoria fisica. `MemoryProjection` possiede l'identità della cache (ISA, spazio di indirizzi, generazione dei mapping, privilegio e variante del monitor) e la cronologia delle radici confermate per ISA. I costruttori invalidano prima di riscrivere: un aggiornamento fallito non riusa tabelle parziali e i chiamanti non forniscono radici obsolete. Le sonde verificano soltanto questa inizializzazione limitata. La validazione dei carichi Linux ARM64 KVM e Windows ARM64 WHP resta da completare; i risultati nativi macOS sono nella [guida HVF](macos-hvf.md).

Il decodificatore XSAVE condiviso distingue lo stato SSE iniziale standard e compatto. Con XSTATE_BV[1] azzerato, entrambi inizializzano XMM; il formato standard legge e valida comunque MXCSR, mentre quello compatto lo inizializza. `X64XsaveCases.def` fornisce disposizioni indipendenti e programmi XRSTOR host originali. `X64XsaveTests.cpp` verifica il rifiuto atomico e confronta entrambi i formati con l’esecuzione reale, preservando lo stato FP/SSE del chiamante. L’oracolo viene saltato esplicitamente se l’architettura o la funzionalità di istruzione richiesta non è disponibile.

`X64FPState.def` dichiara disposizioni compatte AVX, AVX-512, CET_U/CET_S e AMX, incluso l’allineamento dei componenti a 64 byte. I dati presenti devono rappresentare lo stato iniziale nullo; componenti assenti e riempimento non definiscono stato. I bit di disposizione determinano gli offset; disposizioni sconosciute, dati non iniziali o lunghezze errate falliscono prima della pubblicazione. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState` e `InitialWideComponentsDoNotHideFPState` coprono pacchetti WHP di 872 e 10752 byte. Il trasporto non ammette le istruzioni di tali estensioni.

`WhpXsaveRegisters.def` integra i pacchetti XSAVE completi con i registri di controllo x87/SSE nominati. L’ultimo opcode e i puntatori a istruzione/dati vengono scritti esplicitamente e letti dall’host. I campi nulli possono essere completati; conflitti non nulli o controlli comuni incoerenti falliscono prima della pubblicazione. `NamedMetadataRestoresOmittedPacketFields` verifica i campi omessi mantenendo tutti i dati FP.

I campi nativi `FOP/FIP/FDP` seguono le regole x87 dell’host. AMD può azzerarli senza un’eccezione pendente non mascherata; gli snapshot conservano i valori osservati. `X64MachineProbe.def` e i test esatti NOP/contesto impostano un’eccezione pendente coerente per confrontare ogni campo valido senza nascondere differenze. Il riferimento FXRSTOR64/FXSAVE64 nel processo host verifica entrambi gli stati; i backend non sostituiscono mai i risultati dell’host con metadati di ingresso.

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` confronta FP/SSE completo salvato in RAM da FXSAVE64 nel guest con la cattura XSAVE dell’host. Entrambe le API provano installazione diretta e FXRSTOR64 nel guest, con la funzione predefinita di salvataggio puntatori e l’impostazione supportata dall’host selezionata esplicitamente. Distingue ingresso, esecuzione e cattura senza correggere valori; le discrepanze restano errori. La matrice copre anche un’eccezione x87 pendente non mascherata e registra un riferimento FXRSTOR64/FXSAVE64 nel processo host e il produttore della CPU, per distinguere il salvataggio condizionale dei puntatori dal trasporto WHP.

Il codec condiviso `encodeX64XsaveState` / `decodeX64XsaveState` possiede pacchetti FP/SSE standard e compattati, rotazione TOP fisica, stato iniziale dei componenti assenti e validazione atomica. WHP usa le API XSAVE complete, preferendo `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`, con le API XSAVE precedenti come percorso compatibile. I singoli vecchi registri x87 non sostituiscono pacchetti completi. Componenti estesi non iniziali, intestazioni malformate, controlli invalidi e acquisizioni troncate falliscono esplicitamente. Gli errori di mapping WHP conservano HRESULT, GPA e dimensione per la diagnosi.

`CheckedX64Instructions.def` ammette `MUL` senza segno a 8/16/32/64 bit e `CBW/CWDE/CDQE/CWD/CDQ/CQO` tramite il trasporto CPU esistente. `NeverDX64IntegerTests` usa codifiche e valori attesi indipendenti di `X64IntegerCases.def` a entrambi i livelli di privilegio: conservazione parziale dei registri, estensione con zeri a 32 bit, entrambe le metà del prodotto, risultati CF/OF definiti e flag invariati per l’estensione del segno. La moltiplicazione nella RAM ordinaria conserva i controlli dei permessi sull’intero intervallo e gli osservatori di lettura; un errore o un arresto dell’osservatore preserva i registri di uscita impliciti e PC. Gli operandi di dispositivo restano non supportati. I casi vengono eseguiti anche con checked Unicorn; i trasporti nativi non disponibili vengono saltati esplicitamente.

`X64BitInstructions.def` ammette `BT/BTS/BTR/BTC` su registri e RAM ordinaria a 16/32/64 bit. L’indice di registro è interpretato con segno alla larghezza dell’operando e seleziona una parola intera; l’immediato resta nella parola di base. Il troncamento alla larghezza dell’indirizzo precede l’aggiunta della base FS/GS. Il processore fornisce CF e valori scritti; `RAMTransaction` mantiene privato il risultato finché gli osservatori lo accettano. I permessi vengono verificati sull’intero intervallo, incluse pagine allocate separatamente e alias. Arresti, errori dei callback e accessi negati preservano CPU e RAM. LOCK è limitato alle modifiche di memoria con allineamento naturale; MMIO e SMP hardware parallelo restano esclusi. `X64BitStringTests.cpp` confronta codifiche indipendenti con l’esecuzione x64 reale e verifica indici negativi, troncamento, attraversamenti di pagina, annullamento e forme LOCK non valide. Vedere il [riferimento Intel](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`X64StringInstructions.def` definisce `MOVS/STOS/LODS` sulla RAM ordinaria a 8/16/32/64 bit; `CLD/STD` modifica soltanto la direzione. Ogni elemento REP verifica l’intero operando prima dell’osservazione e conferma gli effetti a un confine di ripresa. Un errore successivo conserva gli elementi completati; arresti o eccezioni dei callback lasciano intatto l’elemento corrente. FS/GS si aggiunge soltanto alla sorgente, dopo il troncamento dell’indirizzo. AL/AX conserva i bit alti, mentre EAX estende con zeri. REP con conteggio nullo e indirizzi a 32 bit richiede bit alti nulli nel contatore e, per MOVS/STOS, negli indirizzi utilizzati: altrimenti i processori reali divergono. REPNE per MOVS/STOS/LODS e gli operandi di dispositivo STOS/LODS restano esclusi. `X64StringTransferTests.cpp` confronta larghezze, direzione, sovrapposizioni e conteggi nulli con istruzioni host indipendenti e verifica permessi, alias, riavvolgimento degli indirizzi, errori e ripresa. Il driver WDK originale delle risorse esegue tutte le quattro larghezze STOS/LODS tramite `driver_resource_strings.def`.

`X64StringInstructions.def` definisce anche `CMPS/SCAS` sulla RAM ordinaria a 8/16/32/64 bit con `REPE/REPNE`. Ogni elemento verifica tutte le letture prima degli osservatori, aggiorna sei flag aritmetici e termina alla prima condizione di uscita. Un errore dati ripristina i flag iniziali del REP ininterrotto, conservando puntatori e contatore degli elementi completati; una ripresa pubblica parte dallo stato CPU pubblicato. Arresti ed eccezioni degli osservatori non cambiano l’elemento corrente; l’uscita anticipata non legge il successivo. FS/GS riguarda soltanto la sorgente CMPS; SCAS conserva accumulatore e registro sorgente inutilizzato. Restano esclusi dispositivi e bit alti ambigui a conteggio nullo a 32 bit. `X64StringComparisonTests.cpp` confronta istruzioni host indipendenti, flag, direzione, alias, riavvolgimento, permessi e ripresa; l’oracolo Linux x64 cattura i registri al guasto reale. Il driver WDK originale esegue entrambe le ripetizioni condizionali nelle quattro larghezze tramite `driver_resource_strings.def`. Vedere il [riferimento Intel](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`WhpResourceCache.h` separa lo stato delle CPU logiche dalle partizioni WHP. Il runtime mantiene una partizione nativa attiva e la riutilizza per passi consecutivi della stessa CPU. Il cambio di CPU distrugge la vecchia partizione prima di ricostruire mapping, processore virtuale e stato completo. Le CPU logiche conservano viste `MemoryProjection` indipendenti e la RAM autoritativa. L’acquisizione del lease rispetta annullamento e scadenza corrente; distruggere una CPU inattiva non distrugge la partizione di un’altra CPU. x64 conserva le funzionalità XSAVE predefinite dell’host e verifica la partizione effettiva tramite `WHvGetPartitionProperty`, senza eliminare dipendenze per ridurre la maschera. Questa alternanza cooperativa non offre SMP hardware parallelo.

`NeverDX64FPTests` verifica tutte le 79 posizioni di corruzione ed esegue istruzioni assemblate indipendentemente di `X64ProbeCases.def` sui trasporti nativi, con scadenza unica e RAM guest invariata. `NeverDProjectionCacheTests` copre chiamanti, ordine ISA, cronologia delle radici, varianti privilegio/monitor, generazioni, identità degli spazi e aggiornamento fallito. `NeverDRunControlTests` include `WhpXsaveTests.cpp` per pacchetti API nuovi e precedenti, tutti i TOP, dimensioni e stato invariato dopo errori; i test di protocollo in memoria non provano WHP nativo. I trasporti nativi indisponibili vengono saltati esplicitamente.

Le diagnostiche XSAVE distinguono interrogazione della dimensione, preparazione locale e decodifica del pacchetto acquisito. Conservano nome API, byte restituiti, capacità e metadati limitati di intestazione e controllo, con aspettative indipendenti in `WhpHostFailureCases.def`, senza stampare il contenuto dei registri guest. `InvalidInputReportsPreparationWithoutHostMutation` verifica inoltre che un ingresso rifiutato non chiami l’host né modifichi il suo pacchetto. Il codec ISA condiviso resta l’unica autorità di validazione.

Gli errori host WHP durante le interrogazioni delle capacità, la configurazione di partizioni/CPU virtuali, il trasferimento di registri/XSAVE e l’esecuzione conservano HRESULT e nome API dichiarato in `WhpProtocol.def`; le interrogazioni fallite mantengono il risultato tipizzato di indisponibilità. `WhpHostFailureCases.def` definisce aspettative indipendenti per errori host simultanei all’annullamento e per errori di interrogazione, installazione e acquisizione XSAVE moderni e precedenti. Il CI Windows mirato richiede 210 successi nativi: 16 casi di mappatura, due di avvio, dieci FP/contesto, sette CPU condivisa, otto interi e le due varianti API di `NativeInstallRetainsFPStateBeforeAnyGuestExecution`. Queste confrontano FP/SSE completo e metadati letti separatamente prima di eseguire codice guest. Registrazioni mancanti e test saltati, disabilitati o non eseguiti causano il fallimento della verifica delle prove native. I 26 controlli aggiuntivi coprono tutti i casi di `X64BitStringTests.cpp` ai due livelli di privilegio. Windows PE64 richiede 67 casi di processo WHP e undici confronti indipendenti con Windows nativo.

`NeverDMemoryLifecycleTests` viene compilato indipendentemente da Unicorn, anche nelle configurazioni solo native. I casi software di proiezione e dispositivi vengono saltati esplicitamente quando Unicorn è disabilitato; i casi di CPU che condividono RAM sull’host corrispondente restano registrati. `WhpMemoryTests.cpp` isola l’API di memoria nativa con 16 casi in `WhpMemoryCases.def`: dimensione di pagina o proiezione, allocazioni condivise o indipendenti, byte non toccati o residenti e presenza o assenza del primo processore virtuale. Ogni caso mantiene due proprietari logici, alterna ripetutamente la loro partizione mappata, elimina il proprietario inattivo e verifica che il mapping superstite funzioni senza ricrearlo. Gli errori reali conservano HRESULT e fanno fallire il test; questa evidenza dell’API di memoria non dimostra l’esecuzione di istruzioni.

`X64MachineProbe.def` identifica l’istruzione di avvio fallita e tutte le differenze in scalari, TLS, privilegio, controlli x87, lane FP fisiche e parole XMM, conservando valori attesi e osservati. `DiagnosticIdentifiesStepFieldAndBothValues` verifica messaggi attesi indipendenti. Il confronto resta esatto: la diagnosi distingue una perdita di trasferimento da un problema di esecuzione senza convalidare una verifica nativa fallita.

`WhpResourceTests.cpp` verifica riuso, rilascio prima della sostituzione, recupero dagli errori e gare con scadenza o arresto. `LogicalCPUSwitchingRestoresPhysicalFPAndTLS` alterna due macchine in entrambi i modi di privilegio, verifica stati fisici x87/XMM e FS/GS indipendenti e riprende quella superstite dopo aver distrutto l’altra. La CI Windows richiede entrambi i casi WHP.

`NEVERD_ENABLE_SEMANTIC_TESTS` ha valore predefinito `ON` e controlla il gruppo in `unittests/semantic` e i relativi target aggregati. Per compilare i test CPU nativi senza Unicorn, mantenere `BUILD_TESTING=ON` e impostare `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` e `NEVERD_EMULATION_BACKEND_UNICORN=OFF`. I test nativi KVM/WHP restano disponibili, anche con Windows ARM64/MSVC e gli header SDK appropriati. Abilitare Unicorn su Windows ARM64 richiede ancora una toolchain ARM64 LLVM-MinGW. Questa separazione della compilazione non dimostra l’esecuzione nativa ARM64.

Il CI CPU nativo inizializza i sorgenti Capstone alla revisione fissata e usa il pacchetto LLVM precompilato verificato. Con `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` e l’adattatore Unicorn disabilitato, i test CPU si configurano, compilano e collegano senza sorgenti Unicorn. Non richiedono neppure firme o il corpus esterno. Il CI predefinito mantiene attivo l’intero gruppo di test semantici.

Il `ci.yml` esistente offre la modalità manuale esplicita `native_cpu_only` sul suo runner Windows x64. `NativeCPUTests.def` seleziona gli obiettivi di test; `run_native_cpu_ci.py` li compila prima del CTest filtrato e salva inventario, JUnit, log e riepilogo. I parser CI comuni distinguono superato, fallito, saltato, disabilitato e non eseguito. Ogni caso nativo WHP dichiarato deve essere scoperto ed eseguito; prove native mancanti o saltate fanno fallire il job mirato. La CI predefinita che compila LLVM dai sorgenti resta invariata. Test di protocollo e compilazione non sostituiscono la validazione nativa dei carichi WHP o ARM64.

`native-host-probe.yml` esegue il programma autonomo `probe_native_host.py` sui runner ospitati Linux e Windows x64/ARM64. `NativeHostProbe.def` dichiara l’ordine delle prove di capacità, creazione VM/vCPU e rilascio. I rapporti conservano hash di sorgenti/binari, ISA nativa e ogni codice di stato. `setup_ready` dimostra solo la preparazione; nessuna istruzione guest viene eseguita. Le capacità API/dispositivo mancanti producono `unavailable`; errori di compilazione, preparazione, rilascio, timeout o formato delle prove fanno fallire il job. La disponibilità ARM64 va osservata a ogni esecuzione; questa verifica non convalida i carichi ARM64. Entrambi i workflow Linux usano `prepare_kvm_ci.py` per consentire l’accesso al dispositivo a caratteri KVM esistente solo all’account del runner ospitato, registrandone identità e permessi. Le macchine locali o autogestite vengono rifiutate e i dispositivi mancanti non vengono creati.

`windows-alignment-oracle.yml` usa `check_windows_alignment.py` e `WindowsAlignmentCases.def` per raccogliere 72 osservazioni originali di eccezioni Windows x64: nove forme SSE allineate, ciascuna in sette casi di indirizzi/permessi non allineati e un controllo allineato su una pagina inaccessibile. Conserva codici, parametri, PC del guasto, contesti salvati, output grezzo e hash di sorgenti/binari, verificando che input e RAM restino invariati. Queste osservazioni stabiliscono solo il comportamento del sistema; non certificano l’esecuzione KVM/WHP né aggiungono supporto SEH.

Con `native_cpu_only=true`, `native_driver_tests=true` abilita `NeverDNativeDriverTests` senza Unicorn. Prima della configurazione, `build_wdk_driver_fixtures.py` verifica lo SHA-256 completo dei pacchetti ufficiali Microsoft WDK/SDK 10.0.26100.6584 e ricompila 46 immagini di driver normali/CFG/DBG dai sorgenti originali. `WDKDriverFixtures.def` dichiara identità dei pacchetti, argomenti di compilatore e linker e associazioni delle fixture. I file Microsoft non modificati e le relative licenze restano nelle directory locali di build/cache; la CI carica solo metadati e registri di compilazione. Il manifesto conserva versioni degli strumenti, comandi, hash di sorgenti/header e hash delle immagini prodotte.

`NativeDriverTests.def` richiede 224 esiti WHP per tutti i 112 carichi di `DriverBuiltinImages.def` e `DriverBackendParityCases.def`: 26 immagini integrate, 46 immagini WDK e 40 scenari di richieste, agli indirizzi originali e rilocati. Insieme ai 278 controlli CPU e a quattro regressioni condivise delle continuazioni SEH, sono obbligatori 506 esiti. Le immagini fisse mantengono il rifiuto previsto della rilocazione. Immagini o scenari WDK mancanti o saltati fanno fallire questo job CI facoltativo; nelle normali build locali le fixture esterne restano facoltative. `run_native_cpu_ci.py --with-drivers` registra i target configurati e le prove complete di inventario/JUnit. La compilazione non dimostra esecuzione nativa Windows o ARM64. I comandi seguenti consentono la riproduzione locale; la cache generata può essere caricata anche in una build di emulazione esistente. `278 CPU + 224 WHP + 4 SEH = 506`.

Gli intervalli C SEH restano semiaperti. Una destinazione valida di `__C_specific_handler` può trovarsi nel proprio intervallo protetto: [LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608) emette `EndLabel + 1` come limite finale. Il modello Windows mantiene tali limiti e verifica separatamente eseguibilità, appartenenza alla funzione e identità della continuazione, anche dopo la rilocazione. `KernelSEHContinuationCases.def` conserva il layout della fixture originale; `ScopeEndLabelMayOverlapTheHandlerLandingPad` verifica gestori costanti e filtri. I test complementari verificano il limite esclusivo e il rifiuto di destinazioni non valide senza consumare lo stato di dispatch. Questi controlli puri vengono eseguiti in `NeverDNativeDriverTests` anche con Unicorn disattivato.

Anche l’unwind verso una destinazione usa il limite originale: un `finally` il cui intervallo contiene ancora la destinazione del gestore non viene abbandonato. `FinallyRespectsRawScopeEndAtHandlerTarget` verifica entrambi i lati del limite e, su Windows x64, li confronta direttamente con `ntdll.dll!__C_specific_handler`. NeverD non corregge gli intervalli generati dal compilatore. Le build Clang 20/21 della fixture originale restituiscono un errore guest nelle modalità `T` e `J`, poiché il limite spostato include la destinazione scelta; Clang 23 esegue entrambe le operazioni di pulizia. La [modifica LLVM #144745](https://github.com/llvm/llvm-project/pull/144745) elimina il precedente scostamento `+1`. Questi risultati del compilatore sono distinti dagli errori del backend.

L’inventario include tutti i programmi C WDM/KMDF originali e i percorsi WDK opzionali di CMake. Ogni `driver-*-scenario.json` pubblicato ha casi normali e CFG in `DriverBackendParityCases.def`; collegamenti mancanti fra sorgenti, compilazioni o scenari fanno fallire i test dell’inventario. `Original` rimuove l’indirizzo imposto dallo scenario e verifica la base preferita dell’immagine; `Rebased` verifica la base rilocata dichiarata. Lo scenario con IRP del driver annulla intenzionalmente una richiesta figlia, quindi `DriverNativeOutcomes.def` conserva l’esito complessivo negativo previsto dopo la corretta pulizia.

```bash
python3 scripts/build_wdk_driver_fixtures.py \
  --output build-driver-fixtures --cache build-driver-packages
cmake -S . -B build-native -G Ninja \
  -C build-driver-fixtures/fixtures.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
```

`NeverDAArch64StateTests` verifica la corruzione di ogni campo scalare e di entrambe le parole di ogni vettore, cambi di privilegio, mancata esecuzione floating-point e conservazione delle diagnosi di trasporto. `NeverDAArch64FPTests` esegue `OriginalProgramChecksCompleteStateAndOneDeadline` ai due privilegi su trasporti reali con istruzioni assemblate indipendentemente in `AArch64ProbeCases.def`. Il test sposta queste parole indipendenti dal PC nel codice guest senza concedere accesso user alle pagine del monitor. Unicorn e gli skip nativi espliciti non sostituiscono la prova di avvio ARM64 nativo.

`CheckedAArch64Instructions.def` e `AArch64InstructionEffects` ammettono a EL0/EL1 aritmetica FP32/FP64 di base limitata, confronti, trasferimenti e SIMD a larghezza fissa. FPCR conserva quattro arrotondamenti, FZ e DN; FPSR conserva stato cumulativo e QC. I bit non supportati vengono rifiutati prima delle modifiche. FP16 aritmetico, SVE/SME, eccezioni non mascherate, estensioni opzionali e forme non elencate falliscono esplicitamente. Non aggiunge caricamento di driver Windows ARM64 o altri ambienti OS.

`AArch64InstructionEffects` possiede gli intervalli RAM scalari e FP/SIMD singoli o accoppiati, fino a 128 bit per operando. Lo spazio condiviso verifica ogni pagina prima dell’ingresso; `RAMTransaction` conferma solo scritture fisiche dichiarate complete. L’osservatore da 128 bit riceve due parole ordinate da 64 bit prima degli effetti. Stop e fault conservano RAM, vettori e aggiornamento dell’indirizzo. Lo stesso numero Xn/Vn è valido; le coppie con riavvolgimento dell’indirizzo sono rifiutate. `NeverDAArch64MemoryTests` usa `AArch64CrossPageCases.def` e `AArch64VectorMemoryCases.def` indipendenti.

`NeverDAArch64StateTests` verifica tutte le 71 letture dello stato completo ai due privilegi, larghezze, lettori mancanti e ripresa. `NeverDAArch64FPTests` esegue istruzioni originali di `AArch64FPCases.def`: tutte le lane, aritmetica packed, risultati FP scalari/vettoriali, quattro arrotondamenti, FZ/DN, FPSR cumulativo, contesti e rifiuti. `NeverDAArch64MemoryTests` verifica ogni attraversamento di pagina, ordine/stop degli osservatori, permessi, alias e vettori ripristinati. `NeverDUnicornStateTransferTests` (`UnicornStateTransferCases.def`, `CapturesDeclaredWidthsWithoutStaleUpperBits`) inietta ogni lettura scalare/vettoriale fallita dopo esecuzione reale. I trasporti nativi assenti sono esclusi esplicitamente; queste prove non sostituiscono esecuzione ARM64 KVM/WHP nativa.

`NeverDAArch64MemoryTests` verifica 18 forme scalari/a coppie con Unicorn, KVM e WHP a entrambi i livelli: tutti gli offset tra pagine, segno e larghezza, ordine degli osservatori, seconda pagina negata o assente, consumo esplicito dell’errore e ripetizione, alias fisici ripetuti e ripristino del contesto dopo sostituzione degli alias. Il precedente rifiuto di un caricamento valido tra pagine è stato riprodotto prima della modifica. I trasporti non disponibili sono saltati esplicitamente; Unicorn e cross-compilazione non sostituiscono prove native ARM64 KVM/WHP.

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`) verifica metadati CFG inattivi con flag a zero, puntatori di ripiego invariati a entrambi gli indirizzi di caricamento, slot/destinazioni non validi e rilocazioni mancanti. I casi selezionano esplicitamente Unicorn/KVM/WHP con `driver-strict` e `checked-x64-v1`; i backend non disponibili vengono saltati separatamente. `DriverPublicCLICases.def` seleziona `--backend unicorn` per confrontare la CLI con l’API C v1 compatibile. La selezione nativa e `auto` mantengono test pubblici separati e non cambiano silenziosamente backend quando l’API host non è disponibile.

Unicorn verificato usa `MachineRunControl`: un unico margine copre manutenzione ARM64, esecuzione guest e acquisizione completa dello stato. `UC_HOOK_CODE` controlla il token di arresto preso in prestito e la scadenza all’ingresso dell’istruzione. La chiamata sincrona rilascia il riferimento del hook prima del ritorno, ma il passo macchina conserva il controllo fino alla pubblicazione. Unicorn e WHP preparano tutto lo stato CPU e verificano lo stesso controllo prima di pubblicare un passo riuscito. WHP crea il margine una sola volta prima della preparazione. Un’eccezione CPU x64 autenticata ha precedenza su un arresto ricevuto durante l’acquisizione. La transazione RAM verificata scarta scritture speculative quando l’acquisizione è annullata; il contratto software non limitato resta invariato. `MachineInterruptedError` distingue un annullamento confermato da un errore host o di acquisizione. La CPU verificata condivisa restituisce `Stopped` o `Deadline`, conserva CPU/RAM e consente il tentativo successivo; i veri errori restano `BackendFailure` anche con un arresto simultaneo.

Regressioni di acquisizione: `NeverDUnicornStateTransferTests`, `NeverDUnicornMachineControlTests`: `StopDuringCaptureCannotPublishAndAllowsRetry`, `ExpiredCaptureCannotPublishAndAllowsRetry`, `CompletedStoreCannotPublishCancelledCapture`, `StopDuringCaptureCannotHideRealGuestException`. `UnicornPublicCapture.CancellationKeepsTypedExitStateAndRAMConsistent`; `NeverDKvmStateTransferTests`: `PublicCancellationRetainsStateRAMAndFailurePriority`.

`NeverDUnicornMachineControlTests` usa le istruzioni di scrittura originali di `UnicornMachineControlCases.def` su motori x64 e ARM64 reali con entrambi i privilegi. `RejectedEntryPreservesStateAndRAMAndAllowsRetry` verifica annullamento prima del passo, arresto o scadenza all’ingresso guest effettivo, conservazione dell’intero stato iniziale e della RAM, e una successiva scrittura riuscita. Il wrapper d’ingresso riservato ai test non richiede un hypervisor e non prova l’esecuzione nativa ARM64/WHP.

`RunDeadline::invoke` rifiuta un ingresso WHP già arrestato o scaduto prima della chiamata host, conserva il risultato host effettivo durante l'annullamento e conferma la fine dei callback di interruzione prima di rilasciare il token preso in prestito. KVM e WHP convalidano lo stato privato interamente acquisito sul thread chiamante titolare del diritto di esecuzione, prima di classificare un arresto o una scadenza simultanei. Gli errori effettivi dell'host o dell'acquisizione e le eccezioni CPU x64 autenticate mantengono la priorità. Uno stato ordinario riuscito rimane privato fino alla fine dei controlli di annullamento; un'interruzione confermata scarta gli effetti CPU/RAM speculativi e permette un nuovo tentativo. Preparazione, esecuzione nativa e acquisizione condividono una sola tolleranza per passo. Il controllo è cooperativo e non garantisce un limite rigido di tempo reale.

`NeverDRunControlTests` include i test portabili `NativeEntryTests.cpp` e, su Windows con WHP, `WhpEntryControlTests.cpp`. Callback host in memoria verificano ingresso rifiutato, nuovo tentativo, annullamento tardivo, errori conservati, priorità del risultato e durata confermata dei callback senza Hyper-V. `NeverDKvmRunTests` verifica completamento sul thread chiamante, priorità degli errori e rifiuto del rientro. I test reali `NeverDKvmStateTransferTests` eseguono le istruzioni originali di `KvmStateTransferCases.def`; `ActualCPUExceptionOutranksStopDuringCapture` e `PublicCPUExceptionOutranksStopDuringCapture` arrestano dopo letture effettive di registri/XSAVE e preservano eccezione di divisione, contesto originale, RAM e ripresa esplicita. I test portabili con ABI Windows sotto Wine forniscono solo prove di thread e controllo, non di esecuzione WHP nativa. I trasporti nativi indisponibili restano esplicitamente saltati.

`NeverDInstructionFetchTests` esegue programmi checked x64/ARM64 tramite Unicorn, KVM e WHP in modalità supervisore e utente. `InstructionFetchCases.def` verifica forme degli operandi, salti relativi, scritture guest/host tramite alias del codice, ripristino del contesto, revoca dei permessi, pagine con memoria distinta, lettura anticipata a fine pagina, codifiche non valide o troncate e rifiuto degli ingressi ricorsivi. La CI Windows nativa richiede il successo di tutti i casi WHP x64. Le coppie host/ISA non disponibili sono saltate esplicitamente; l’esecuzione ARM64 portabile non prova il supporto ARM64 nativo.

`WhpStateTransferTests.cpp` inietta trasferimenti per entrambe le generazioni XSAVE: gruppi modificati, cattura completa, padding ignorato, errori parziali, annullamento, priorità delle eccezioni e sostituzione della partizione. `ContinuedStepsReuseCapturedRegistersAndFP` conta le installazioni evitate; `PartialTransferFailuresPreserveStateAndForceFullRetry` impone il ripristino completo. Sono verifiche di protocollo; restano necessarie le suite native FP, transizioni, driver e ring3.

`WhpStateTransferCases.def` verifica anche gli errori dopo ogni prefisso parziale della lettura combinata di 32 registri e i conflitti in tutti i sette campi di metadati. Entrambe le generazioni XSAVE devono conservare lo stato del chiamante e imporre il ripristino completo al nuovo tentativo. La suite verifica una sola lettura di registri per passo e il recupero dei metadati omessi da XSAVE tramite quella lettura.

`windows-pe64-v1` supporta processi console Windows x64/ARM64 limitati con PEB/TEB, TLS statico e dinamico, `DllMain`, API Win32 nominate e grafi DLL espliciti aciclici. I moduli supportano import di codice/dati per nome o ordinale, DIR64, export inoltrati e identità reali del loader. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` e `GetProcAddress` usano il catalogo configurato. CRT/GUI, SEH utente ARM64 basato sui frame, thread e compatibilità Windows generale restano incompleti; mancano prove native ARM64 KVM/WHP.

Byte di input ed estensioni aggregate delle immagini condividono ciascuno `memory_limit`; anche i mapping del runtime consumano il budget immagine. La preparazione condivide 65,536 record, 64 MiB di letture, nomi limitati e la scadenza del lavoro, senza limite temporale rigido per I/O host. Il fixture originale EXE→DLL→DLL verifica rebasing, ordinali, dati condivisi, identità API, `MEM_IMAGE`, liste e attach/detach TLS EXE. `NeverDWindowsProcessTests` include l’oracolo Windows nativo, `NeverDPEProgramExportsTests` metadati invalidi e budget, `NeverDProcessPublicTests` parità C ABI/CLI. I backend indisponibili sono saltati esplicitamente.

`WindowsProcessLifetime` esegue TLS e poi `DllMain` delle DLL in ordine di dipendenza, seguiti da TLS e ingresso EXE, con CPU e budget comuni. Ogni modulo riceve indice TLS e blocco allineato indipendenti, copiati dall’immagine rilocata e collegata in un’area comune di 64 KiB. L’argomento riservato TLS è zero; `DllMain` riceve un valore opaco non nullo all’avvio/uscita del processo. L’uscita esplicita separa le DLL inizializzate nell’ordine inverso della lista del loader e poi TLS EXE, anche prima dell’inizializzazione EXE. `DllMain(FALSE)` iniziale termina con `0xc0000142` senza detach. Errori e budget esauriti non inventano pulizia. Il ritorno dall’ingresso PE con DLL guest richiede terminazione del thread non supportata e si arresta esplicitamente. `SizeOfZeroFill` non nullo resta escluso; i byte inizializzati a zero nel modello TLS effettivo sono supportati. Le DLL senza ingresso ricevono TLS attach, ma nessuna notifica di detach del processo.

`WindowsProcessExports` condivide la risoluzione per nome/ordinale tra import statici e `GetProcAddress`, inclusi codice, dati, alias e catene di inoltro. Solo gli inoltri iniziali usati aggiungono moduli del catalogo e dipendenze di inizializzazione; quelli inutilizzati non caricano file. I nomi distinguono maiuscole; nomi assenti restituiscono NULL/errore 127, ordinali assenti cercati direttamente (inclusi i buchi) NULL/errore 182 e un argomento di query NULL errore 87, il successo conserva LastError. Gli handle sconosciuti restano non supportati. Gli ingressi API esatti fornitore/nome sono riservati una volta dal registro limitato. La risoluzione verifica header PE e metadati export correnti di ogni immagine, rifiuta modifiche o byte illeggibili, limita le catene a 64 elementi e condivide i crediti residui di metadati e la scadenza del processo. Un inoltro a un buco restituisce la base dell’immagine di destinazione e conserva LastError; all’ordinale zero restituisce errore 87. La base è un indirizzo dati e non autorizza l’esecuzione degli header. Gli inoltri a runtime possono caricare moduli configurati e completarli prima di restituire il risultato. La modifica delle tabelle export attive resta non supportata.

`WindowsProcessLoader` carica nomi base DLL ASCII da `windows.modules` e gestisce riferimenti espliciti, dipendenze condivise e mantenimento dei moduli iniziali. Ripetere una query inoltrata non aggiunge riferimenti. Ogni ricaricamento assegna una nuova generazione residente allo stesso slot del catalogo. TLS e `DllMain` usano la stessa CPU sotto i frame API sospesi; il ripristino dei registri conserva le scritture guest e usa il ritorno corrente. I puntatori riservati di attach/detach dinamico sono zero. Un attach fallito durante un caricamento esplicito restituisce 1114 dopo la pulizia, preservando i caricamenti annidati indipendenti riusciti. Lo scaricamento libera immagine e TLS; il ricaricamento ripristina i byte originali. Modifiche esterne a liste loader o puntatori TLS sono rifiutate. I budget di file, immagini e metadati restano cumulativi anche dopo gli errori. I fornitori di sistema usano la base PE mappata come handle di modulo. Ricerca di file, percorsi non ASCII, flag `LoadLibraryEx`, cicli e transizioni rientranti dello stesso modulo in inizializzazione/scaricamento restano non supportati.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` condividono il blocco ambiente corrente del guest nei parametri di processo del PEB. I nomi ASCII ignorano maiuscole e minuscole; i valori sono UTF-16. Le modifiche verificano input, capacità e permessi di scrittura prima della pubblicazione. Le istantanee restano indipendenti dalle modifiche successive e rilasciano la memoria guest. Il modello limita il blocco a 64 KiB; stringhe ed espansioni hanno limiti e controllano la scadenza. Puntatori di proprietà sconosciuta, blocchi malformati, pagine di codice ANSI e buffer di espansione sovrapposti restano non supportati. `WindowsEnvironmentTests.cpp` confronta fixture originali x64/ARM64 sui backend disponibili; la CI richiede un oracolo Windows nativo indipendente.

`WindowsProcessHeap` unifica allocazione, `HeapReAlloc`, rilascio e interrogazione delle dimensioni dello heap del processo. Il ridimensionamento conserva i byte mantenuti; `HEAP_ZERO_MEMORY` azzera quelli aggiunti e `HEAP_REALLOC_IN_PLACE_ONLY` impedisce lo spostamento. Il ridimensionamento fallito conserva il vecchio blocco e restituisce NULL con `ERROR_NOT_ENOUGH_MEMORY` (8), come nelle osservazioni native. Le pagine indipendenti restituiscono capacità durante riduzione e rilascio; crescita preparata e copie limitate verificano la scadenza. Heap personalizzati, flag di eccezione, proprietà sconosciuta e intervalli inaccessibili arrestano esplicitamente l’esecuzione. `WindowsHeapTests.cpp` copre entrambe le ISA, spostamento forzato, riuso del budget e atomicità degli errori; CI esegue lo stesso EXE originale su Windows nativo. I casi PE in batch usano il limite CTest comune di 120 secondi; ogni guest mantiene il proprio budget finito. La fixture dello heap consente 20 secondi per processo per controllare tutti i dati con WHP.

`WindowsSystemModules` costruisce immagini modello PE64 limitate per `ntdll.dll`, `kernelbase.dll` e `kernel32.dll` su entrambe le ISA. Le ricerche ASCII `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` e `GetProcAddress` condividono le basi mappate; PEB/LDR e `MEM_IMAGE` descrivono le stesse immagini. Importazioni statiche, ricerche per nome e inoltri guest usano gli stessi ingressi API e risolutore degli export. I fornitori restano residenti, senza callback guest di inizializzazione, e non impediscono il ritorno dall’ingresso dopo lo scaricamento delle normali DLL guest. Modifiche a intestazioni o metadati di export interrompono la ricerca. Nomi di sistema non modellati e ordinali non nulli arrestano esplicitamente l’esecuzione; differenze di maiuscole nei nomi modellati e nomi vuoti restituiscono 127, una ricerca NULL restituisce 87. Byte e indirizzi generati sono regole del modello; layout delle versioni Windows, ordinali nativi e alias tra fornitori non sono ricostruiti. `WindowsSystemTests.cpp` confronta EXE originali x64/ARM64 con Windows nativo, incluse otto osservazioni indipendenti del ritorno del thread iniziale.

`WindowsProcessExceptions` implementa `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` e `RaiseException` sulla stessa CPU e con il budget del processo. I gestori ordinati possono modificare le registrazioni, sollevare eccezioni annidate, chiamare API modellate, caricare DLL e terminare il processo. Le violazioni di accesso ai dati x64/ARM64 e le divisioni intere x64 riprendono dopo la convalida delle modifiche guest a `CONTEXT`, preservando registri generali, SIMD e stato FP supportato. Le eccezioni software riprendono da una vera istruzione di ritorno nel fornitore modellato. Limiti: 128 registrazioni conservate e 16 frame annidati. Risultati non validi, puntatori modificati, campi non supportati e superamenti falliscono esplicitamente. SEH/unwinding ARM64 basato sullo stack, debugger ed errori di esecuzione/guardia restano esclusi. `WindowsExceptionTests.cpp` confronta EXE/DLL originali con Windows nativo; mancano ancora prove native ARM64 KVM/WHP. I record delle eccezioni software includono `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), indipendentemente dal flag di non continuabilità del chiamante; l’eseguibile Windows originale verifica i valori esatti dei flag delle eccezioni software e hardware.

`AddVectoredContinueHandler` e `RemoveVectoredContinueHandler` gestiscono un elenco ordinato separato e condividono con i gestori delle eccezioni il limite di 128 registrazioni conservate. Quando un gestore vettorizzato accetta la ripresa, i callback di continuazione ricevono lo stesso record modificabile e `CONTEXT`. La convalida finale avviene dopo questi callback, comprese le eccezioni annidate e le notifiche DLL. Gli handle non possono essere rimossi tramite l’altra famiglia di gestori. `WindowsContinuationTests.cpp` confronta EXE originali con Windows nativo per ordine, arresto anticipato, modifiche alle registrazioni, riparazione del contesto, annidamento, callback del caricatore e uscita del processo. Il percorso vettorizzato verificato su Windows x64 consente la ripresa con `EXCEPTION_NONCONTINUABLE`; ciò non dimostra il comportamento SEH basato sullo stack. L’esecuzione ARM64 nativa resta non verificata.

`RtlCaptureContext` è disponibile tramite `kernel32.dll` e `ntdll.dll` per x64 e ARM64. I componenti condivisi `WindowsProcessContext` e `IntegerABI` salvano PC/SP del chiamante senza modificare lo stato CPU o LastError. Le osservazioni native di Windows confermano i flag x64 `0x10000f`, la conservazione delle aree home/debug/vettori non scritte e i campi storici degli indirizzi x87 a 32 bit; ARM64 copia LR in PC e azzera X0/LR nel record. Registri, SIMD e controlli floating point provengono dal guest; selettori x64 e maschera delle capacità MXCSR seguono la CPU guest configurata. Destinazioni non valide, non allineate o parzialmente inaccessibili falliscono prima della scrittura. `WindowsContextTests.cpp` verifica import diretti, ricerche dei provider, callback VEH, output tra pagine e atomicità degli errori. `scripts/check_windows_context.py` esegue il programma originale su Windows x64/ARM64, con un oracolo nativo separato per lo stato x87 non vuoto. Queste osservazioni ARM64 non provano l’esecuzione nativa KVM/WHP. Ripristino del contesto, analisi dello stack e tabelle di funzioni dinamiche restano attività distinte. `WindowsProcessServices.def` dichiara vincoli precisi per modulo: la ricerca in `kernelbase.dll` restituisce `ERROR_PROC_NOT_FOUND` (127), come nelle osservazioni native, senza inventare un export. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` usa lo `X64SEH` condiviso in `os/windows/exception/` (`NeverDEmulationWindowsException`, disponibile senza driver) per x64 `__C_specific_handler` e UNWIND_INFO V1. Dopo la ricerca VEH supporta filtri, finally, trasferimenti non locali, eccezioni annidate/unwinding in collisione e frame EXE/DLL rilocati, preservando GPR/XMM non volatili. La continuazione tramite filtro esegue VCH sullo stesso `CONTEXT`. `WindowsSEHTests.cpp` confronta 23 scenari originali con Windows nativo; KVM/WHP/Unicorn condividono la semantica. Il budget del processo comprende la riconvalida di generazioni, header, byte di unwinding/ambiti, regioni del gestore di linguaggio e associazioni IAT. Metadati modificati o immagini conservate scaricate falliscono esplicitamente. SEH ARM64 basato sui frame, C++ EH, tabelle dinamiche, RtlUnwind/NtContinue generali e unwinding oltre callback del loader/VEH/VCH restano esclusi.

Per `EXCEPTION_NONCONTINUABLE`, un filtro x64 che restituisce `EXCEPTION_CONTINUE_EXECUTION` genera `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, flag `0x81`, record collegato nullo) con un nuovo contesto. VEH viene eseguito nuovamente prima della ricerca nello stack logico conservato, mantenendo l’ordine finally, l’identità dei frame EXE/DLL e gli stessi budget di profondità ed esecuzione. I 23 scenari nativi comprendono 21 esecuzioni riuscite e due terminazioni: accettare in VEH/VCH la continuazione di questa eccezione secondaria la lascia non gestita anche ripristinando il `CONTEXT` originale. Il modello segnala un errore di esecuzione. Gli indirizzi delle eccezioni software coincidono con il PC salvato; indirizzi del dispatcher interno e disposizione dei registri sono scelte del modello. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

`WindowsDynamicTests.cpp` confronta DLL/EXE originali x64/ARM64 con osservazioni Windows native indipendenti: riferimenti, dipendenze condivise, caricamenti annidati, pulizia dopo errori, inoltri, uscita, DLL senza ingresso e TLS nuovo al ricaricamento. Le regressioni rifiutano metadati modificati e puntatori di codice scaduti, mantengono budget cumulativi e risultati API interrotti incompleti. La CI Windows impone oracolo nativo e casi WHP; cross-compilazione e Unicorn ARM64 non provano l’esecuzione ARM64 nativa.

Una libreria assente nella catena di inoltro di `GetProcAddress` restituisce 127; un `LoadLibrary` esplicito per un modulo assente dal catalogo restituisce 126. L’oracolo nativo e ogni backend disponibile verificano tutti i 41 scenari dichiarati. Su Windows, il ritorno dopo lo scaricamento di tutte le DLL viene osservato 16 volte per variante di DLL. Anche un’inizializzazione fallita tramite inoltro di `GetProcAddress` restituisce 127 dopo la pulizia. I callback di distacco del processo preservano il contenuto dello stack del chiamante che termina.

`WindowsExportTests.cpp` usa DLL ed EXE originali x64/ARM64 per verificare chiamate inoltrate a codice/dati/ordinali, alias, query di inizializzazione, rebasing, maiuscole, assenze, LastError, cicli, destinazioni non residenti, puntatori non validi e modifiche dopo query riuscite. Lo stesso EXE ha un oracolo Windows nativo indipendente; i casi WHP sono obbligatori nella CI nativa. I test C ABI/CLI confrontano report completi. Le prove hardware ARM64 native restano da acquisire. Varianti EXE con e senza tabella export coprono entrambi i grafi, l’ordine PEB e detach e gli errori per nome/ordinale/NULL.

`WindowsLifetimeTests.cpp` confronta tracce fisse con processi Windows nativi indipendenti e KVM/WHP/Unicorn: uscita normale, ritorno dall’ingresso, entrambi gli errori DLL, quattro uscite precoci e DLL senza ingresso. Verifica anche errori dei callback, budget comuni, campi TLS rilocati e capacità totale. La prova nativa del ritorno conserva l’handle del thread iniziale e verifica 64 volte il codice di uscita e l’esatta sequenza di notifiche thread/processo. I thread figli restanti vengono terminati dopo l’osservazione; l’uscita del processo non viene interpretata come ritorno dall’ingresso.

La memoria virtuale Windows aggiunge `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` e `FlushInstructionCache` per il processo corrente. Il livello OS gestisce le prenotazioni; `AddressSpace` resta responsabile delle pagine impegnate, dei permessi e della memoria sottostante. I test verificano modifiche al codice, errori di accesso e riutilizzo del budget di memoria.

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

Le regressioni coprono budget esatti e insufficienti, parole parziali, entrambi gli ordini dei byte, identità della memoria intatta, limiti degli slot affini, sovrascritture di predecessori tardivi e invalidazione predefinita. C API/CLI verificano compatibilità v6 e domini non validi. HighC e LLVMC eseguiti in O0/O2 controllano ritorno, memoria, stack e stato preservato, senza certificare equivalenza nativa.

Le regressioni coprono fasi in registri e frame, entrambi gli ordini dei byte, rami invalidi raggiungibili, predecessori tardivi, guardie interne esaurite e limiti di scoperta adiacenti. I test del limite di visite coprono correlazioni aritmetiche, cicli annidati, modalità, passaggi sequenziali, lavoro totale e precedenza precedente. Il CLI esegue entrambi i percorsi C e ABI sorgente a O0/O2; C/Python v8 verificano layout, campi invalidi e code future ignorate. Le regressioni verificano anche che grandi selettori finiti estranei lascino budget alle guardie native e che l’ultimo raffinamento consentito sia riservato a un produttore già proposto.

`NeverDLLVMCPhiTests` esegue aggiornamenti di ciclo indipendenti e con dipendenze incrociate a O0/O2, includendo zero iterazioni, limiti di iterazione e valori iniziali casuali a larghezza completa. Le verifiche di leggibilità richiedono nessuna copia locale per gli aggiornamenti indipendenti e solo quella necessaria per gli scambi composti. I casi esistenti di diramazione, switch, rami spostati e cicli di scambio verificano l’arco scelto e le assegnazioni simultanee.

`NeverDLLVMCPhiTests` verifica anche uscite comuni dai cicli con coppie PHI distinte nei successori, chiamate di osservazione ordinate, memoria di uscita e IR del chiamante invariato. Il C dell’intero modulo e di una funzione selezionata viene confrontato con riferimenti indipendenti a O0/O2. Confronti diversi e predecessori aggiuntivi verificano il trattamento conservativo.

`NeverDLLVMCPhiTests` verifica più archi di ritorno con oracoli O0/O2 indipendenti, osservatori ordinati, istantanee precedenti a chiamate che modificano memoria, riporto modulare su tipi stretti ed estensione del segno. Le uscite di modulo e singola funzione preservano l’IR originale. Copre archi in conflitto, radici condivise, annotazioni poison, operandi indefiniti, shift variabili, intrinseci vincolati, eccezioni e rifiuto completo per budget insufficiente. Le rotazioni vengono riunite solo se tutte le operazioni entranti concordano.

`NeverDLLVMCPhiTests` esegue regioni scalari strutturate a O0/O2: cicli annidati con blocchi riordinati, rombi, zero iterazioni, wrap di interi stretti, osservatori in testa, scambi PHI, valori esterni vivi, passi condivisi ed estremi degli shift a imbuto. Verifica IR originale invariato, fusione in tre variabili locali e ripiego eseguibile per grafi con più uscite, irriducibili o troppo grandi. Sono casi sintetici indipendenti; l’emissione del sorgente non certifica il recupero nativo.

`NeverDLLVMCValueTests` confronta il C dei cicli scalari tipizzati con LLVM compilato direttamente e indipendentemente a O0/O2, attivando trap per il comportamento indefinito nel C generato. Valori limite e input deterministici a larghezza completa coprono moltiplicazione stretta e riporto prima degli shift, moltiplicazione e shift estesi, troncamento booleano, confronti ed estensioni con segno, precedenza, espressioni condizionali, aritmetica booleana, percorsi alternativi per operazioni non supportate ed espressioni profonde materializzate. I test verificano anche IR chiamante invariato e rimozione delle conversioni ridondanti.

`NeverDLLVMCPhiTests` e `NeverDLLVMCValueTests` coprono contatori a un byte con riporto in incremento/decremento, uscite unificate, usi inline dopo il ciclo, valori esterni ancora vivi e copie PHI. Confronti indipendenti O0/O2 con trap per comportamento indefinito verificano somma composta, rifiuto della sottrazione invertita, moltiplicazione stretta e maschere booleane. Le regioni annidate richiedono contatori locali al ciclo, un risultato distinto e LLVM sorgente invariato. Una regressione eseguibile fa coincidere i nomi delle funzioni esterne con quelli iniziali di risultato e contatore, verificando chiamate ed effetti osservabili.

`NeverDLLVMCValueTests` verifica entrambe le posizioni del ramo neutro per somme, sottrazioni e operazioni bit a bit, basi invariate, dipendenze inline dal vecchio valore, istantanee delle condizioni, test stretti, rami non neutri e selezioni condivise. Il C generato viene eseguito a O0/O2 contro LLVM compilato indipendentemente, con trap per comportamento indefinito. `NeverDLLVMCPhiTests` verifica anche le istantanee parallele e le inizializzazioni dipendenti dai rami che devono mantenere l’ambito condiviso. L’IR chiamante resta invariato.

`NeverDUnicornDecodeTests` verifica i bit EVEX riservati delle forme registro sui modelli CPU AVX-512/APX, la priorità dei fault di memoria ROUND, la conservazione dello stato e la ripresa. Una prova indipendente su Linux x64 conferma i fault di allineamento della codifica classica e i fault di pagina delle forme scalari/VEX. Questi test del motore non ampliano le istruzioni ammesse in modalità checked e non dimostrano esecuzione APX nativa.

## Misure CPU ARM64

Usare una build CPU Release. HVF esplicito richiede macOS ARM64 nativo; attivare Unicorn per il confronto software. Ogni risultato viene verificato. L’inizializzazione è misurata separatamente; i cambi di CPU includono API e controlli intermedi, gli altri carichi escludono preparazione e verifica.

```bash
cmake --build build-cpu --target neverd-cpu-bench --parallel 4
build-cpu/bin/neverd-cpu-bench --backend hvf --samples 7 --warmup 1
build-cpu/bin/neverd-cpu-bench --backend unicorn --samples 7 --warmup 1
```

Conservare l’eseguibile di riferimento prima di ricompilare. Python 3.11+ alterna l’ordine. Conservare configurazione, etichette delle sorgenti, hash binari e tutti i campioni; evitare compilazioni e test paralleli.

```bash
python3 scripts/benchmark_cpu.py \
  --baseline /path/to/before --baseline-label BEFORE_COMMIT \
  --candidate /path/to/after --candidate-label AFTER_COMMIT \
  --pairs 15 --output /path/to/new-comparison.json
```

Il conteggio degli ingressi è una diagnosi separata, include sonde iniziali e introduce overhead; i suoi tempi non sono misure prestazionali. I carichi non rappresentano un OS completo né confronti tra ISA.

[Riproduzione](../testing.md#reproduce-checked-arm64-cpu-measurements) · [HVF](macos-hvf.md)
