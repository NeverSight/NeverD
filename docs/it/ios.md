**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](../ja/ios.md) | [한국어](../ko/ios.md) | [Français](../fr/ios.md) | [Deutsch](../de/ios.md) | [Español](../es/ios.md) | [Italiano](ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# Recupero del codice nativo e dei sorgenti iOS

[← Indice della documentazione](README.md) · [Panoramica mobile](../mobile.md)

`neverd mobile` accetta IPA, `.app` e Mach-O. Esporta C nativo, metadati runtime e, in via sperimentale, sorgenti Objective-C `.m` e Swift `.swift` per i corpi nativi supportati. Un risultato pubblicato può includere metodi non recuperati: controllare la copertura prima dell’uso. I contenitori mobili appartengono al flusso CLI; l’SDK C nativo carica separatamente il Mach-O selezionato.

La compilazione elimina commenti, formattazione, identificatori e costrutti del linguaggio. Questo flusso ricostruisce una rappresentazione sorgente: non ripristina il testo originale e non certifica un comportamento equivalente per applicazioni arbitrarie. Non avvia l’applicazione analizzata.

## Avvio e dipendenze

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

Compilare il target `neverd` con una toolchain compatibile con C++20. Il flusso mobile viene eseguito nella CLI nativa senza interprete Python. Il demangling delle firme Swift è fornito da `LLVMSwiftDemangle` nel fork LLVM di NeverD. Sia le compilazioni dai sorgenti sia i pacchetti LLVM pubblicati corrispondenti includono questo componente; NeverD non scarica separatamente i sorgenti Swift. Compilare ed eseguire NeverD non richiede l’installazione del compilatore o della toolchain Swift. Restano le dipendenze native come LLVM e Capstone: distribuire le librerie e gli avvisi di licenza richiesti dalla propria compilazione. La compilazione indipendente dei sorgenti Apple generati e i test di comportamento Swift su macOS richiedono, secondo il caso, Apple Clang, SDK e `swiftc`.

Il recupero delle firme Swift usa direttamente i nodi strutturati di `LLVMSwiftDemangle` nel processo C++. Non cerca né avvia eseguibili esterni di demangling o comandi per individuare la toolchain. La precedente opzione per il percorso dell’eseguibile è stata rimossa e la precedente variabile d’ambiente del demangler non viene più letta. `--metadata-only` non esegue né l’esportatore nativo dei sorgenti né il demangling delle firme.

L’inventario delle firme in `metadata/swift-signatures.json` identifica il componente integrato così:

```json
{
  "demangler": {
    "name": "llvm-swift-demangle",
    "execution": "builtin",
    "version": "6.3.3"
  }
}
```

```sh
neverd mobile App.ipa -o recovered-swift --timeout=600 --json
```

## Input e selezione

L’IPA deve contenere esattamente un `Payload/*.app` al primo livello. In IPA e `.app`, `CFBundleExecutable` in `Info.plist` identifica il programma principale. `--artifact` è relativo a quel bundle in entrambi i formati e seleziona un solo eseguibile incorporato, senza analizzare ricorsivamente tutti i framework o le estensioni. L’input Mach-O diretto non accetta `--artifact`.

Per i binari fat, `--arch=auto` preferisce arm64, arm, x86_64, poi i386. Slice assenti o non supportate falliscono esplicitamente. La proiezione sorgente riguarda attualmente arm64/x86_64: selezionare un’altra famiglia non implica supporto Objective-C/Swift. Una slice selezionata con `cryptid != 0` viene rifiutata; fornire un input già decifrato e leggibile. Archivi e directory rifiutano percorsi pericolosi, link simbolici, file speciali e voci in conflitto.

## Opzioni e limiti di risorse

| Opzione | Predefinito | Significato |
|---------|-------------|-------------|
| `-o DIRECTORY` | Obbligatorio | Nuova directory esterna alla directory di input; l’output esistente rimane intatto |
| `--platform=auto\|ios` | `auto` | Rilevare la piattaforma o scegliere iOS |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | Selezionare una slice Mach-O |
| `--artifact PATH` | Programma principale | Percorso eseguibile relativo al bundle |
| `--metadata-only` | Disattivato | Solo metadati, senza recupero sorgenti o invocazione di strumenti |
| `--max-func N` | `0` | Limite di funzioni native; zero significa tutte quelle scoperte; ignorato in modalità metadati |
| `--timeout N` | `300` | Budget totale positivo di analisi in secondi; i processi figli usano il tempo rimanente |
| `--max-files N` | `20000` | Budget positivo di voci; anche l’inventario Swift è limitato |
| `--max-bytes N` | `2147483648` | Budget positivo in byte per input, estrazione e output finale |
| `--json` | Disattivato | Stampare il rapporto versionato come JSON |

L’area di lavoro è monitorata e può usare fino a tre volte i budget di voci/byte per input temporanei e risultati intermedi. I log sono limitati a 16 MiB per processo; il JSON delle firme Swift ha inoltre un limite di 32 MiB. Sono controlli di risorse, non isolamento dei processi. Aumentare il timeout non disabilita gli altri limiti. I metodi esclusi da `--max-func` rimangono non recuperati se presenti nei metadati.

## Sorgenti Objective-C e struttura runtime

Entrambe le modalità di rapporto sorgente includono `source_projection_graph`. I nodi mostrano i corpi nativi tipizzati finali, la diagnostica locale, le `dependencies` native e Block aggregate e il risultato effettivo `closure_closed`. I controlli locali e gli errori propagati dalle dipendenze hanno motivi distinti; i corpi tipizzati mancanti mantengono diagnostica incompleta. Risolvere una chiamata non collegata può introdurre altre dipendenze. Un nodo chiuso ha superato solo la fase delle dipendenze: emissione e controlli del testo restano necessari prima di `recovered`. `native_dependency_graph` rimane un inventario LowIR separato.

Per analisi ripetute della copertura, il comando seguente esegue le stesse analisi e verifiche di pubblicazione di `--format=objc-methods`, ma omette `native_source` e il campo `source` di ogni metodo. Il JSON aggiunge `sources_omitted=true` e conserva il significato completo di identità, stati, diagnostica, firme, riferimenti agli helper condivisi e prove delle dipendenze. I controlli di rendering vengono comunque eseguiti; la modalità completa produce sorgenti compilabili. Il punto di ingresso C corrispondente è `neverd_objc_methods_summary_json(session, max_functions)`; liberare il risultato con `neverd_free_string`.

```sh
neverd export WMF --format=objc-methods-summary -o summary.json
```

Il loader nativo collega record del metodo, indirizzo IMP eseguibile e codifica di tipo supportata a posizioni ABI sorgente esplicite. I parametri fissi scalari/puntatori mantengono `self`/`_cmd`, argomenti inutilizzati, banchi interi/flottanti separati e posizioni sullo stack supportate. Reinterpretazione dei bit float/double e conversione numerica sono distinte. I suggerimenti di tipo sono input per la proiezione sorgente, non prove ABI autenticate né autorizzazione a modificare codice eseguibile.

`sources/objc.m` colloca le istruzioni realmente ricostruite nei metodi `@implementation`, mantenendo helper C e chiamate tipizzate necessarie. Le destinazioni richiedono un collegamento sorgente supportato; destinazioni ignote e gruppi di dipendenze incompleti restano non recuperati. Definizioni mancanti, indirizzi non eseguibili, codifiche discordanti, ABI non gestite, decodifica incompleta e IR rifiutato non diventano metodi recuperati solo perché esiste una dichiarazione.

I parametri delle funzioni ausiliarie native possono acquisire tipi puntatore per la proiezione del sorgente quando i valori di ingresso completi raggiungono, tramite COPY/PHI, argomenti puntatore già associati senza usi scalari in conflitto. Questa inferenza conserva le posizioni ABI fisiche e i tipi IR generici; i corpi e i relativi gruppi completi di dipendenze native devono comunque essere convalidati.

Un helper scalare nativo può restituire un parametro in ingresso osservato se il suo esatto registro ABI copre l’intera larghezza del risultato. Una prova limitata combina ogni percorso di ritorno con l’ingresso iniziale e gli archi di ritorno dei cicli; chiamate e scritture parziali invalidano il valore. Ingressi non dichiarati, parametri segnaposto inutilizzati, marcatori iniziali SSA e PHI non provano gli input. Questa inferenza limitata alla proiezione sorgente conserva le posizioni fisiche e richiede ancora la verifica completa del corpo e delle dipendenze.

Le funzioni native locali possono esporre ingressi interi a larghezza completa in registri ausiliari, inclusi puntatori ai risultati, quando l’analisi degli ingressi osservabili concorda con le letture del LowIR completo. I registri salvati dal chiamante possono poi essere sovrascritti come temporanei; i contesti preservati richiedono ancora l’assenza di scritture nei registri preservati esterni al frame. Le definizioni implicite delle chiamate, anche con versione SSA 0, non sono ingressi; solo i byte esplicitamente preservati vengono tracciati attraverso le chiamate. Chiamanti e definizioni rigenerati condividono le stesse posizioni fisiche. Non vengono dedotti prototipi C/Swift esterni e resta obbligatoria la verifica completa di corpi e dipendenze.

I tipi di callback C fissi conservano le firme di parametri e risultato nelle dichiarazioni e conversioni del sorgente. L’importazione esatta `swift_once` associa un indicatore, un callback `void (*)(void *)` e un contesto, senza risultato. La chiamata al runtime viene mantenuta; ciò non dimostra il recupero del corpo del callback né la proprietà della memoria condivisa di inizializzazione. Le dipendenze incomplete restano non recuperate.

Le importazioni note del runtime Objective-C mantengono associazioni esplicite per argomenti e risultati: retain/release, rilascio automatico, riferimenti forti e deboli, allocazione e setter con firma fissa. Le varianti ARM64 dedicate a un registro leggono quel registro; identità importata e ABI devono coincidere. Le chiamate di lettura, scrittura e rimozione degli oggetti associati conservano oggetto, chiave, valore e criterio della larghezza di un puntatore; il C generato usa l’header pubblico del runtime. I test di ricompilazione macOS confrontano durata degli oggetti, azzeramento dei riferimenti deboli, copia e rimozione con i metodi originali.

Le interrogazioni ottimizzate di classi e selettori di libobjc mantengono le chiamate reali del runtime, inclusa la gestione di nil e delle ridefinizioni personalizzate. Il risultato di un byte conserva le conversioni native del chiamante; non viene sostituito con un controllo ipotizzato della gerarchia delle classi.

Il binding del sorgente costruisce la mappa verificata delle identità degli oggetti classe diretti solo quando un argomento puntatore ha un indirizzo costante. La mappa resta locale a una singola operazione di binding; ogni operazione successiva verifica nuovamente l’immagine corrente, inclusi nomi delle classi, flag delle metaclassi e conflitti di identità.

L’esportazione Objective-C associa anche un insieme fisso di importazioni del runtime Swift con ABI C ordinaria: conteggio dei riferimenti, riferimenti deboli nativi e a oggetti di tipo sconosciuto, metadati degli oggetti e inizio/fine del controllo degli accessi. Il C generato conserva le chiamate e richiede il runtime Swift al collegamento. Restano esclusi gli ingressi con registri specializzati, le convenzioni di chiamata Swift non riconosciute e i simboli Swift arbitrari; identità importate e posizioni scalari devono coincidere esattamente.

Binding separati supportano su arm64 e x86_64 gli import Darwin esatti String → NSString `_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF` e NSString opzionale → String `_$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ`. Entrambi preservano la proprietà e Clang `swiftcall`; il collegamento richiede Swift Foundation e Swift Core. Il bridge inverso trasporta le due parole String restituite come intero senza segno a 128 bit, suddiviso nei registri di ritorno espliciti prima di SSA. È un trasporto di bit, non un layout String recuperato né una ABI generale per aggregati. Le firme sconosciute e le dipendenze di inizializzazione incomplete restano non supportate.

Negli argomenti chiave verificati delle API degli oggetti associati, gli indirizzi esatti nelle stringhe C Mach-O di sola lettura sono ricostruiti come identità condivise. Lo stesso indirizzo originale usa una sola chiave; offset interni diversi restano distinti. L’esportazione mobile unisce automaticamente le funzioni ausiliarie. L’API C ne elenca i nomi in `shared_identity_functions`; collegando le unità dei metodi occorre una sola definizione per funzione. Queste chiavi appartengono al codice ricostruito, non alla memoria di un’immagine originale già caricata. Gli altri usi di indirizzi dell’immagine non associati rimangono una limitazione.

Le importazioni verificate di `os_unfair_lock_lock`, `os_unfair_lock_unlock`, `os_unfair_lock_trylock`, `os_unfair_lock_assert_owner`, `os_unfair_lock_assert_not_owner` chiamano la vera implementazione Darwin tramite `<os/lock.h>`, preservando l’indirizzo del lock, i controlli di proprietà e il risultato booleano. Le varianti sconosciute restano non supportate. I risultati interi conservano solo i bit dichiarati dall’ABI: Darwin arm64 estende i risultati di 8/16 bit a 32 bit secondo il segno; gli altri bit del registro restano sconosciuti. Le larghezze di ritorno dichiarate entrano in SSA prima dell’unione dei rami, evitando che i bit alti inutilizzati nascondano i byte bassi validi.

I record Darwin `__cfstring` verificati si ricostruiscono come oggetti costanti quando importazione della classe, layout, caratteri e fixup sono completi. Si conservano byte ASCII e unità UTF-16, inclusi i NUL incorporati. Gli stessi indirizzi originali condividono un oggetto ricostruito; record diversi restano distinti. Gli helper compaiono in `shared_identity_functions` e richiedono Foundation al collegamento. Questo non autorizza accessi diretti alla memoria non verificati sugli oggetti costanti.

I contatori numerici di una sezione `__DATA,__llvm_prf_cnts` con limiti verificati e priva di puntatori possono usare memoria ricostruita condivisa. Sono preservati i byte iniziali, le letture e scritture sovrapposte da 1–16 byte e gli aggiornamenti tra i file dei metodi. L’esportazione mobile unisce le funzioni ausiliarie; gli utenti dell’API C devono collegare una sola definizione per ogni funzione di `shared_storage_functions`. Questa memoria è indipendente dall’immagine originale e dal relativo runtime di profilazione. Restano non supportati gli indirizzi che sfuggono all’analisi, gli accessi ordinati, le mappature incomplete e le rilocazioni di puntatori. I callback block strumentati possono aggiornare questo spazio nell’immagine senza esporre indirizzi privati; le normali scritture dei contatori sono distinte dalle scritture nel frame di stack privato.

I metadati mantengono identità della superclasse, inizio/dimensione dell’istanza e ivar scalari/puntatori con offset, larghezze e allineamenti verificati. Le dichiarazioni inseriscono padding dove necessario. I metodi che richiedono un layout indisponibile restano non recuperati. Le categorie conservano identità classe/categoria/indirizzo e implementazioni separate; record identici ripetuti nei due inventari contano una volta. Le categorie esterne supportate usano dichiarazioni Foundation esistenti. Gli header esterni sconosciuti sono dipendenze mancanti; non si inventa una classe sostitutiva.

Il recupero richiede anche dichiarazioni di classe complete, definizioni delle classi antenate locali e sorgente in cui i valori siano definiti prima dell’uso su ogni percorso e le uscite raggiungibili abbiano il ritorno richiesto; una superclasse locale vuota riceve una `@implementation` vuota solo se il layout verificato e un inventario completo dimostrano l’assenza di metodi ordinari propri. Se mancano queste prove o un nome è in conflitto con un’importazione Foundation nota, i metodi interessati restano nel denominatore della copertura con le motivazioni, mentre le classi recuperabili indipendentemente continuano a essere emesse; questi controlli dei nomi non coprono tutti i nomi degli SDK, gli SDK iOS o le versioni.

Le chiamate ai Block Objective-C supportate richiedono un’ABI scalare fissa completa, comprendente l’oggetto Block implicito e tutte le posizioni degli argomenti e del risultato. La codifica di runtime `@?` viene ampliata a `id` soltanto nelle dichiarazioni; non fornisce il prototipo di invocazione. I riferimenti ai Block globali mantengono l’identità dell’oggetto condiviso. Le catture scalari sincrone supportate richiedono una prova della memoria nativa delle catture e del flusso di invocazione. Le catture forti copiate tramite una chiamata verificata a `objc_retainBlock` / `_Block_copy` possono uscire dal contesto se la costruzione inizializza la memoria su ogni percorso entrante e i corpi di invocazione, copia e distruzione sono interamente recuperati. Il descrittore generato conserva le ABI degli ausiliari e il layout di proprietà originali. Riferimenti deboli/byref, layout sconosciuti e utilizzatori non dimostrati restano non recuperati. Sono supportati anche i parametri block C dichiarati non sfuggenti dal compilatore quando coincidono importazione esatta, posizione del parametro e ABI completa del callback. Questo contratto di durata non implica memoria di sola lettura. Quando si collegano le unità sorgente della C API, ogni voce di `shared_block_functions` deve avere una sola definizione. L’esportazione mobile unisce le definizioni corrispondenti e rifiuta i conflitti, comprese le differenze nelle funzioni private chiamate.

Le dichiarazioni dei metodi di protocollo vengono lette dai record locali risolti del runtime, inclusi i protocolli ereditati e i metodi obbligatori o facoltativi, di istanza e di classe. Le liste ordinarie e relative condividono il decoder. Tutte le dichiarazioni corrispondenti di classi e protocolli devono concordare prima di assegnare una firma fissa al selettore; record malformati e cicli di ereditarietà non forniscono indicazioni di tipo. I puntatori validi a strutture, unioni e array restano opachi senza dedurne il layout. `objc_metadata.protocols` espone separatamente le dichiarazioni, senza contarle come implementazioni recuperate né stabilire la conformità delle classi. A parità del resto della firma, i risultati interi a 64 bit con e senza segno condividono una rappresentazione binaria senza segno; i conflitti su interi più stretti, virgola mobile, puntatori o argomenti restano rifiutati.

Le chiamate di formato NSString dichiarate dal compilatore possono recuperare argomenti scalari promossi da un oggetto di formato costante verificato. Gli argomenti sequenziali e posizionali devono essere completi e coerenti nei tipi. Darwin arm64 legge gli argomenti variabili da slot di stack di otto byte; x86_64 usa registri interi, registri in virgola mobile e stack. Le chiamate generate conservano ellissi e dispatch dinamico. Formati sconosciuti, scritture del conteggio, long double ed estensioni non supportate vengono rifiutati. La stessa analisi supporta importazioni C dichiarate come `NSLog`, verifica la libreria esportatrice esatta e condivide il prototipo variadico tra chiamate con numeri di argomenti diversi.

Le scritture nel frame di stack privato conservano ogni byte letto in qualsiasi punto della funzione. Con limiti del frame, alias immutabili in ingresso e assenza di fuoriuscita degli indirizzi dimostrati, NeverD può eliminare scritture non lette o accorciare la coda non letta di una scrittura intera. Gli argomenti scalari promossi restano così recuperabili quando solo il riempimento inutilizzato è sconosciuto. Non vengono inventati bit sconosciuti. Accessi ordinati, chiamate non associate, indirizzi ambigui, valori con effetti e budget di analisi esauriti conservano le scritture originali.

I metodi di categoria in collisione conservano le dichiarazioni ABI validate anche quando l’ordine di sostituzione è sconosciuto. Una chiamata dinamica può usare tale ABI solo se tutte le dichiarazioni corrispondenti concordano; le implementazioni ambigue restano escluse dalla scelta del corpo sorgente. Dichiarazioni incompatibili o malformate continuano a bloccare la chiamata.

Le chiamate note a `objc_enumerationMutation` conservano l’argomento oggetto e il percorso successivo, poiché un gestore delle mutazioni installato può ritornare. Le importazioni Darwin esatte di `__stack_chk_guard` legano l’identità dell’oggetto del runtime; letture, confronti e chiamate a `__stack_chk_fail` restano osservabili nel sorgente ricostruito.

Le immagini Darwin collegate a 64 bit che importano Foundation di sistema consultano anche dichiarazioni integrate estratte dal compilatore. Tutte le dichiarazioni del runtime e del framework per un selettore devono essere compatibili; le firme variadiche, gli aggregati non supportati e le differenze tra piattaforme restano senza associazione. Il catalogo fornisce tipi di chiamata, senza determinare la classe del ricevitore né generare corpi di funzione. Per usare NeverD non serve un SDK Apple locale. CoreData dispone di un catalogo separato, attivato dalla sua esatta dipendenza di sistema. Le dichiarazioni appartengono al framework del relativo header pubblico; gli header di dipendenze inclusi indirettamente non attivano altri framework.

Il catalogo delle dichiarazioni C copre anche le esportazioni di CoreGraphics e ImageIO. I puntatori opachi a immagini e colori, i conteggi interi e i risultati in virgola mobile conservano l’ABI dichiarata. Gli alias dei framework pubblici di sistema vengono generati insieme ai dati di esportazione; percorsi privati, versioni diverse e simboli non dichiarati non ottengono un’associazione. Anche le dichiarazioni mobile usano la grammatica dei tipi del caricatore: i puntatori ad aggregati validi rimangono opachi senza ipotizzarne la disposizione.

I grandi letterali Swift immortali possono associare i byte UTF-8 a memoria statica condivisa presso un ponte Foundation verificato. Lunghezza, flag, terminatore, validità UTF-8, immutabilità e importazione esatta devono essere verificati insieme. La rappresentazione marcata e il ponte originali restano invariati; gli zeri interni e altre forme di memorizzazione rimangono non associati.

Gli oggetti NSString costanti convalidati mantengono la loro identità condivisa anche nelle assegnazioni e scritture tramite interi con provenienza da un indirizzo dati completo. Valori scalari, indirizzi incompleti, operazioni numeriche e accessi ai byte interni degli oggetti non ricevono questo collegamento.

I caricamenti scalari ordinari da byte dell’immagine dimostrati immutabili e privi di rilocazione possono diventare costanti che conservano i bit. Sono supportati interi di 1, 2, 4 e 8 byte e valori in virgola mobile di 4 e 8 byte. Memoria scrivibile o ambigua, caricamenti ordinati e usi come indirizzo rimangono non associati; un uso numerico non autorizza usi come puntatore della stessa espressione.

Le chiamate C con parametri fissi usano anche dichiarazioni estratte dal compilatore e informazioni di esportazione e riesportazione dell’SDK. Devono corrispondere la libreria dyld esatta, il simbolo e l’ABI scalare; importazioni deboli, fornitori sconosciuti e prototipi non supportati restano non associati. Il C generato usa identificatori distinti collegati ai simboli originali. Le normali chiamate di sincronizzazione senza tabelle di eccezione conservano chiamate reali ed effetti sulla memoria.

Le letture scalari indicizzate possono usare una tabella di byte immutabili quando l’analisi condivisa del flusso sorgente dimostra un limite superiore senza segno su ogni percorso in ingresso. Condizioni, maschere, larghezze intere native e overflow modulare mantengono la propria semantica; scritture e indirizzi locali che sfuggono invalidano i fatti precedenti. Ogni tabella è limitata a 4.096 voci e 65.536 byte. Alla pubblicazione si ricontrollano limiti, memoria, rilocazioni e ogni uso degli helper. I bit della larghezza di un puntatore che designano una sezione dell’immagine restano ambigui; frammenti scalari più stretti e riempimento dei segmenti non provano l’identità di un puntatore. I byte copiati servono solo a letture scalari e l’indirizzo della tabella non può sfuggire. I test eseguibili confrontano bit interi, bit in virgola mobile incluso lo zero negativo e comportamento fuori intervallo con i metodi originali.

Il catalogo C include le dichiarazioni di `sys/mount.h` con identità di collegamento specifiche per architettura: ARM64 usa `getmntinfo`, mentre x86-64 usa `getmntinfo$INODE64`. Il parametro di uscita resta un puntatore a puntatore opaco; modalità e risultato restano interi con segno a 32 bit, con verifica esatta delle esportazioni di sistema. Non vengono inventati né il layout dei record del filesystem né il contenuto del buffer restituito.

Le associazioni dei dati esterni richiedono dichiarazioni SDK comuni senza TLS e prove esatte delle esportazioni della libreria. Il C generato fa riferimento alla memoria reale del simbolo e conserva gli accessi successivi, inclusa la distinzione tra un puntatore globale e il suo oggetto. Importazioni deboli, identità contrastanti e memorie non supportate restano non associate. Le dichiarazioni dei dati non dimostrano la costruzione o la proprietà dei blocchi. Il catalogo include dati di CoreData, CoreImage, CoreGraphics, ImageIO e CoreSpotlight. Lo storage dei letterali integrati deriva dalla compilazione di raccolte vuote e oggetti booleani per ogni destinazione; sono ammessi solo indirizzi diretti di dati esterni senza TLS, con gli stessi controlli sulle esportazioni. La generazione richiede anche il compilatore Clang tramite `--clang`, oltre a libclang.

Su ARM64, le identità di memoria esterne di `kCIContextPriorityRequestLow` e `kCIContextUseSoftwareRenderer` usano dichiarazioni concordanti degli SDK completi iPhoneOS e arm64 iPhoneSimulator, con un’importazione CoreImage esatta. Le opzioni del contesto conservano i caricamenti effettivi dei puntatori e i valori di esecuzione; il binding non sostituisce le chiavi stringa o il comportamento di rendering.

Su ARM64, anche `UIApplicationDidEnterBackgroundNotification` viene associata al proprio esatto spazio di memoria esterno UIKit. Le dichiarazioni dei due SDK concordano; il caricamento originale del puntatore e l’identità della notifica a runtime restano invariati.

È una ricostruzione limitata del runtime: proprietà e protocolli completi, annotazioni originali di ownership, aggregati arbitrari, code variadiche, corpi dipendenti dalle eccezioni e layout Block/catture non modellati non sono garantiti. La codifica descrive gli argomenti fissi e non prova l’assenza di puntini di sospensione nella dichiarazione originale. I puntatori concatenati sono usati solo negli slot risolti dal loader; gli altri formati mantengono diagnostica.

## Sorgenti Swift e memoria

L’output strutturato del demangler distingue firme invocabili da metadati non invocabili. Prima della proiezione nativa, le firme supportate sono associate a simboli, entry e ABI macchina esplicita del binario selezionato. I receiver Swift seguono la propria ABI, senza sostituirli con gli argomenti nascosti Objective-C. Anche un file di firme fornito dall’utente resta un suggerimento da validare.

L’emettitore sperimentale costruisce funzioni libere, metodi di classe, inizializzatori designati e metodi di struct a layout fisso supportati, incluse determinate forme di receiver mutating. Dichiarazioni di classe/struct e campi memorizzati richiedono metadati di layout recuperati. Le chiamate native sono emesse solo quando dichiarazioni e corpi necessari formano un gruppo completo di dipendenze supportate. Le unità sorgente riuniscono dichiarazioni e metodi, senza un ponte che richiami il binario originale.

I corpi dei getter/setter Swift supportati provengono dall’implementazione nativa e vengono assemblati in proprietà. La memoria privata di supporto conserva il layout dei campi accertato; inizializzatori e altri metodi usano gli stessi nomi di memoria. Una dichiarazione di proprietà o una voce di campo da sola non dimostra il recupero del corpo di un accessor.

I costruttori con allocazione, distruttori/deallocazioni triviali, accessor dei metadati di tipo e ingressi `_modify`/resume supportati possono essere proiettati in un’unità di tipo emessa. Ogni ingresso richiede una prova limitata dell’intero flusso nativo e dei suoi effetti, dipendenze contesto/inizializzatore/proprietà effettivamente recuperate e verifiche della gestione delle eccezioni e dell’IR dei corpi correlati. Le scritture dell’allocatore devono coincidere con l’inizializzatore reale; `_modify` deve associare il campo mutabile esatto e la continuazione. Le chiamate ai metadati di runtime conservano la propria semantica modellata nel tipo recuperato. Questi ingressi sono indicati esplicitamente come proiezioni sorgente del compilatore, non come corpi di normali metodi recuperati separatamente o testo sorgente originale.

Layout generici o resilient, funzioni async/throwing, convenzioni sconosciute, accessor/allocator/thunk non supportati, inizializzazione incompleta e dipendenze native/runtime non collegate rimangono singolarmente `unrecovered`. Un simbolo mangled o nome di tipo nominale non è un metodo recuperato. Simboli rimossi e nodi del demangler non classificati rendono la copertura incompleta o sconosciuta.

## Output e copertura

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

I file del linguaggio sorgente esistono soltanto se può essere emesso codice. `objc.json` contiene classi, categorie, ivar e codifiche grezze; `objc.h` le dichiarazioni supportate. `swift.json` contiene tipi nominali e simboli mangled. JSON di firme/metodi mantengono classificazione, omissioni, motivi e conteggi. I log contengono la diagnostica nativa e quella dell’export Swift nativo quando viene eseguito. Non vengono generati log di ricerca di toolchain Swift esterne o di demangling esterno. I percorsi in `report.json` sono relativi alla sua directory. Il binario selezionato è un artefatto d’analisi, non viene collegato come ponte di recupero al codice generato.

Copie temporanee del pacchetto e JSON intermedi vengono rimossi. Una normale esecuzione senza corpi nativi fallisce anche con metadati disponibili. La modalità metadati produce soltanto slice selezionata, `objc.h`, `objc.json`, `swift.json`, `report.json`; mancano sorgenti e file di firme/copertura, mentre `native_function_count`, `objc_method_recovery`, `swift_method_recovery` sono `null`. Tutte le modalità usano i metadati Objective-C risolti dal loader nativo. I metadati Swift vengono letti dall’immagine nativa entro limiti verificati; fixup, layout rilocabili o riferimenti non supportati mantengono diagnosi di risultati parziali.

Questo rapporto illustrativo abbreviato mostra volutamente un recupero parziale:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

Lo `status: "success"` esterno indica pubblicazione dell’output validato. `recovered`, `partial`, `unrecovered`, `no-methods` descrivono l’inventario scoperto, non equivalenza semantica o completezza del programma originale. Ogni metodo non recuperato ha un motivo. Per Objective-C, `recovered` richiede anche metadati runtime completi. Un inventario vuoto non dimostra che non esistessero metodi.

Una riga Objective-C non recuperata può includere `native_backend: {status, reason, diagnostics}` se un unico risultato del backend corrisponde esattamente alla sua identità runtime. Questo riepilogo facoltativo e limitato nelle dimensioni conserva il risultato intermedio anche quando fallisce un controllo delle dichiarazioni o del layout. Stato, motivo e conteggi principali della riga restano determinanti; l’assenza del riepilogo indica che queste informazioni non erano disponibili.

Il `coverage_status` Swift conta soltanto gli elementi invocabili classificati. Lo `status` Swift complessivo considera anche simboli ignoti e può essere `unclassified`, `unsupported-architecture`, `no-symbols`. I metadati non invocabili compaiono in `non_method_symbols` con `not-callable`, quelli ignoti con `unclassified`. `types`, `type_metadata_count`, `source_type_count` contano separatamente metadati e unità di tipo emesse, senza gonfiare il numero di metodi.

Ogni riga Swift recuperata riporta `source_representation` con valore `native-method-body` oppure `compiler-generated-from-type`. Le proiezioni del compilatore conservano anche `compiler_projection_kind` e `compiler_projection_evidence`. `source_body_method_count` conta i corpi nativi recuperati; `compiler_projection_method_count` conta le proiezioni del compilatore dimostrate. La loro somma è `recovered_method_count`. Gli ingressi del compilatore rimangono nel denominatore `method_count` e conservano l’identità esatta in una sola unità sorgente `type` corrispondente. I soli metadati di tipo o il nome di una dipendenza non aumentano la copertura recuperata. Il JSON nativo in batch include `source` nelle righe del compilatore e nelle unità di tipo; i `source_units` del report mobile conservano soltanto descrizioni senza `source`, mentre il sorgente completo si trova in `sources/swift.swift`.

Il batch Swift descrive `source_units` con `{kind, module, name, source, method_entries, method_identities}`; kind vale `function` o `type`, ogni identità è `{entry, mangled_symbol}`. Simboli diversi possono condividere un’entry mantenendo proiezioni ABI distinte. Ogni identità recuperata deve apparire esattamente una volta, nessuna non recuperata può comparire. `method_entries` deve essere l’esatta proiezione ordinata delle entry di `method_identities`, inclusi indirizzi ripetuti. Identità duplicate identiche non possono essere unite silenziosamente. `source` concatena in ordine il codice delle unità più un carattere di nuova riga ciascuna. Mobile salva il codice aggregato in `sources/swift.swift` e le descrizioni nel JSON di copertura. Il `source` individuale serve all’ispezione; concatenare queste righe non ricostruisce correttamente le classi.

## Export nativi diretti e SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

L’export Swift consuma l’inventario strutturato di firme prodotto dal parser integrato durante una normale esecuzione mobile. Il batch Objective-C include `native_source`, `native_function_count`, `objc_metadata` e, per metodo, sorgente C, nome, tipo di ritorno e parametri. Prima di `.m`, Mobile verifica ulteriormente dichiarazioni, corpi e layout: la copertura finale può essere inferiore a quella del C batch. Un export nativo riuscito può non contenere alcun metodo recuperato.

Ogni metodo include `projection_diagnostics`: gli `items` registrano ostacoli verificabili indipendentemente con `code`, `reason` e prove disponibili su istruzioni, chiamate, valori o dipendenze. `checks_complete: false` indica prerequisiti mancanti o limiti di risorse; un rapporto parziale vuoto non dimostra il recupero. I controlli sono condivisi con l’ammissione e preservano `status`, `reason` e `unbound_call`.

Il `native_dependency_graph` percorre il LowIR finale dalle firme supportate, seguendo chiamate dirette al codice dell’immagine. Conserva indirizzi di chiamante, blocco e istruzione, destinatari condivisi e cicli; quelli indiretti restano null. `inventory_complete` è false se manca una funzione LowIR necessaria o si raggiunge il limite di registrazioni. `targets_complete` richiede inoltre destinatari diretti per tutte le chiamate registrate. Radici non supportate e destinazioni indirette irrisolte sono escluse; non dimostra copertura completa dell’esecuzione nativa né recupero delle fonti dipendenti.

I riferimenti locali a protocolli convalidati usano `objc_getProtocol` per preservare l’identità registrata. L’inventario delle dipendenze `runtime_protocols` include le funzioni native chiamate. Nomi in conflitto, dichiarazioni incomplete, slot importati e rilocazioni irrisolte restano non associati. L’esportazione autonoma segnala la registrazione mancante invece di emettere un corpo che potrebbe ricevere un protocollo nullo.

Il catalogo ABI del runtime Swift deriva da dichiarazioni upstream fissate e conserva la convenzione C o Swift. Puntatori noti e interi senza segno della larghezza di un puntatore formano firme scalari fisse. Le chiamate Swift richiedono un’importazione forte esatta di libswiftCore; le due classi supportate di disponibilità versionata dei metadati non autorizzano importazioni deboli. Registri speciali, rappresentazioni sconosciute, disponibilità non supportate e conflitti restano esclusi. Il compilatore aggiunge a swift_willThrow attributi swiftself/swifterror assenti dal DSL. Effetti e contratti esistenti per callback e byte restano invariati. Generare con `scripts/generate_swift_runtime_declarations.py --input RuntimeFunctions.def --source-sha256 <pinned-hash>`; `--check` verifica il catalogo. Revisione, hash e avvisi di terze parti accompagnano i dati.

Le dichiarazioni fisse del runtime Swift conservano anche risultati di due parole: due puntatori, oppure il puntatore ai metadati e la parola di stato dichiarati. Il livello ABI condiviso assegna entrambi i registri del risultato; la validazione del sorgente ricontrolla ordine, tipi e importazione esatta. Allocazione dei box e richieste di metadati eseguono le vere chiamate del runtime; parametri aggregati, layout sconosciuti e contesti nascosti restano esclusi.

Separatamente, i bridge esatti di valori Foundation osservati dal compilatore per URLRequest, Notification, URL, Data, Date e IndexPath conservano `swiftcall` e l’identità del provider. Risultati indiretti e contesto/self usano `swift_indirect_result` e `swift_context` nei registri dedicati (x8/x20 su arm64 e RAX/R13 su x86_64) senza consumare il banco ordinario degli argomenti interi. Data mantiene il trasporto esplicito di due parole. Sono dichiarazioni dei portatori di chiamata, non layout di valori recuperati né una ABI Swift generale.

L’inferenza dei risultati scalari nativi riconosce l’estrazione completa e immediata delle due parole di una chiamata con ABI del risultato verificata. Identità temporanea, offset dei membri, larghezze e registri fisici devono coincidere. Una funzione ausiliaria può così restituire direttamente il primo membro, mantenendo le regole delle viste dei registri, le sovrascritture delle chiamate e il requisito di un risultato definito su ogni percorso di ritorno.

Il catalogo C fisso conserva anche valori interi a 32 bit e risultati booleani esplicitamente estesi con zeri. Un risultato booleano occupa un intero byte senza segno; ciò non stabilisce le regole di estensione dei parametri booleani. Restano escluse le rappresentazioni più strette sconosciute e le dichiarazioni a 32 bit con convenzione Swift. I test eseguiti verificano conversioni riuscite e fallite, incrementi contati dei riferimenti e distruzione degli oggetti, mantenendo chiamate ed effetti sulla proprietà.

I metadati dei campi distinguono la disposizione dei byte nota dai tipi di linguaggio sconosciuti. Le classi Swift con ABI stabile usano variabili di offset larghe quanto un puntatore anche su arm64; i campi non esposti possono avere una codifica di tipo Objective-C vuota. NeverD conserva questa assenza e verifica offset, dimensioni, allineamento e sovrapposizioni. I corpi C recuperati ottengono gli offset tramite ricerca degli ivar a runtime. Un tipo sconosciuto impedisce ancora dichiarazioni di classe che richiederebbero di inventare un tipo.

L’identità degli ivar Swift con ABI stabile può essere conservata anche quando gli offset vengono inizializzati a runtime in memoria zero-fill. Queste classi usano `ivar_status: "runtime"`; gli offset sconosciuti sono JSON `null` e la dimensione 0 indica una larghezza determinata a runtime. I metodi possono interrogare la classe esistente e i tipi oggetto dichiarati seguono gli slot esatti. Gli offset letterali richiedono ancora una disposizione nota. Ciò non ricostruisce la disposizione di una classe Swift: l’esportazione autonoma continua a rifiutare disposizioni e tipi di campo sconosciuti.

Per una chiamata nativa associata direttamente, l’indirizzo di un offset ivar può essere sostituito da quello di uno scalare locale solo se la funzione chiamata legge il puntatore esattamente una volta all’ingresso, prima di effetti osservabili, senza scriverlo, conservarlo, confrontarlo o usarlo altrimenti. Il valore locale deriva dalla ricerca ivar esistente a runtime; ABI e corpo della funzione restano invariati. Questa prova limitata accetta un flusso di controllo lineare e rifiuta usi incerti o limiti di analisi esauriti. La C API recupera `.cxx_destruct`; la sintassi dei metodi Objective-C autonomi non può ancora emettere questo selettore.

I buffer immutabili vengono ricostruiti solo se il contratto verificato della chiamata importata limita la lettura a una lunghezza non negativa ed esclude scritture, conservazione dei puntatori e confronti di identità degli indirizzi. Byte e lunghezza vengono preservati; gli altri usi dei puntatori restano irrisolti. Il fallimento di asserzione esatto della libreria standard Swift conserva i portatori scalari e di stack derivati dal compilatore, `swiftcall`, l’effetto `noreturn` e l’ortografia del simbolo di collegamento; gli shim diagnostici C mantengono l’ABI C dichiarata e una trap successiva separata. Le stringhe statiche e la memoria dei letterali String immortali vengono copiate solo per questo consumatore terminale autenticato che non conserva il puntatore. I valori String dinamici o posseduti non sono trattati come letterali.

Per una sessione con Mach-O già caricato, `neverd_objc_methods_json(session, max_functions)` e `neverd_swift_methods_json(session, signatures_json, max_functions)` restituiscono i rapporti. Zero seleziona tutte le funzioni scoperte. Liberare le stringhe riuscite con `neverd_free_string`; `NULL` indica un errore spiegato nello stato della sessione. Le API non caricano contenitori IPA o `.app`.

L’inferenza delle dipendenze native Objective-C può restringere da 64 a 32 bit un parametro intero in registro di `NativeAnalysis` dopo il collegamento delle chiamate sorgente. Una prova esaustiva degli usi HighIR deve dimostrare che ogni occorrenza osserva esattamente i quattro byte meno significativi, tramite un’estrazione di byte con offset zero o come argomento intero esatto di una chiamata collegata. La prova è indipendente per ciascun parametro; gli usi a larghezza intera, con offset diverso da zero, non validi o interrotti dal budget conservano la larghezza originale. La funzione ausiliaria raffinata viene sottoposta di nuovo al lifting e deve superare i normali controlli del corpo e della chiusura delle dipendenze; l’ABI di riscrittura non cambia.

I getter di oggetti statici Swift con inizializzazione differita esposti tramite thunk di ingresso Objective-C possono essere proiettati solo quando il simbolo `vgZTo`, la firma del metodo a runtime, il test del predicato, la chiamata `swift_once`, il caricamento dello storage e il risultato di `objc_retainAutoreleaseReturnValue` formano un modello esatto del compilatore. I simboli del predicato `_Wz`, dell’inizializzatore `_WZ` e dello storage `vpZ` devono corrispondere; il terzo registro grezzo può comparire solo come contesto once, mentre l’inizializzatore deve ignorare tale contesto e non avere normali chiamanti diretti. La proiezione ricostruisce il predicato e lo storage dell’oggetto, passa null come contesto irrilevante e mantiene l’inizializzatore come dipendenza verificata. Qualsiasi variazione di forma, simbolo, uso dei parametri o callback rimane non recuperata.

I thunk di costruttori Swift Objective-C con un simbolo `cfcTo` autenticato e la firma di runtime `init` possono anche eliminare un terzo registro di argomento non dichiarato quando la sua unica occorrenza è il contesto di una sola chiamata esatta a `swift_once`. Il predicato `_Wz` e l’inizializzatore `_WZ` devono corrispondere; il callback deve ignorare il contesto e non avere normali chiamanti diretti. La proiezione passa null, deduce solo l’estensione di otto byte dello storage del predicato e conserva tutti gli altri effetti di controllo, memoria e chiamata; restano applicabili i normali controlli sul corpo sorgente e sulla chiusura delle dipendenze.

I callback once ARM64 possono inoltrare un contesto x2 altrimenti inutilizzato a un solo `swift_once` annidato se il simbolo esterno `_WZ` e la coppia interna `_Wz`/`_WZ` sono esatti, nessuno dei callback ha chiamanti diretti ordinari e il callback foglia, tipizzato indipendentemente, ignora il contesto. Lo stesso contratto governa individuazione e proiezione. Questa passa null e conserva ogni istruzione; l’ABI void consente di scartare solo ritorni puri da registri, temporanei o costanti. Letture dello stack, caricamenti e chiamate nel ritorno, oltre alle dipendenze irrisolte, restano rifiutati.

## Verifica e risoluzione dei problemi

Su macOS, le build con `BUILD_TESTING` attivo offrono `check-neverd-mobile-ios`, che esegue tutte e tre le suite di recupero nativo tramite CTest.

Python serve soltanto agli script di test di sviluppo riportati sotto; il recupero mobile integrato viene eseguito nella CLI nativa C++20.

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

Su macOS, gli script Objective-C compilano gli originali, recuperano `.m` e collegano soltanto il sorgente generato a un programma di chiamata indipendente. Lo script scalare verifica limiti interi, rami, cicli, letture/scritture tramite puntatori, parametri impliciti, identità dei bit float/double, parametri misti e argomenti sullo stack. Lo script delle chiamate aggiunge dispatch dei messaggi, ereditarietà, Category, memoria delle variabili d’istanza, funzioni native ausiliarie e invocazione/catture/identità condivisa dei Block. Il corpus delle chiamate richiede 21/21 metodi recuperati e 134/134 risultati indipendenti per ogni variante arm64/x86_64 × classic/default. Eseguire i controlli con la compilazione corrente della CLI nativa.

Lo script Swift rigoroso controlla 22 dichiarazioni utente, tre ingressi getter/setter e nove ingressi invocabili generati dal compilatore; nessuno può scomparire dall’inventario. Ogni variante contiene 858 verifiche indipendenti dei risultati del programma originale. Compila separatamente il `.swift` generato e il programma di chiamata, senza dylib, modulo, bridge originali o dichiarazioni sostitutive scritte a mano. I casi includono chiamate scalari/native, inizializzazione e memoria delle classi, metodi di struct per valore/mutating, parametri floating point e sullo stack, puntatori e cicli. La CLI nativa C++20 deve superare tutte e quattro le varianti arm64/x86_64 × classic/default senza casi saltati: ciascuna deve recuperare 25 corpi nativi e nove proiezioni del compilatore, conservare le 34 identità invocabili e superare 858/858 verifiche sia per gli originali sia per il Swift generato compilato indipendentemente. Questi risultati si limitano al corpus e non garantiscono il recupero di applicazioni arbitrarie o del testo sorgente originale. Lo script rifiuta copertura mancante, errori di compilazione del sorgente e differenze di comportamento.

Queste quattro varianti hanno macOS come destinazione. I due ingressi aggiuntivi del compilatore sono l’inizializzatore del valore vuoto e il relativo accessor dei metadati; le prove native li verificano separatamente e una sola unità sorgente `struct Empty {}` conserva entrambe le identità. Superare questo corpus non convalida un’applicazione iOS reale.

Tutti e tre gli script supportano `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N` e `--work-dir NEW_DIRECTORY`. `--setup-only` convalida gli originali e non verifica il recupero. L’accettazione richiede il completamento di tutte le varianti di architettura e fixup richieste, anche per lo script scalare. Una variante mancante o un’architettura richiesta che l’host non può eseguire causa un fallimento; non è consentito saltare casi. Gli artefatti di errore conservati consentono di distinguere copertura sorgente mancante, errori di compilazione e differenze di comportamento. Consultare l’output corrente prima di dichiarare il supporto verificato.

Il [workflow Mobile Real Applications](../../.github/workflows/mobile-real-apps.yml) usa applicazioni pubbliche con versioni fissate nel [manifest del corpus](../../scripts/mobile_real_apps.json). L’accettazione richiede inventari indipendenti di tutti i Mach-O dei bundle iOS completi e di tutti i DEX degli APK, ricompilazioni indipendenti degli originali e del sorgente generato, e confronti del comportamento. Fasi mancanti, copertura dell’inventario sconosciuta o casi obbligatori assenti fanno fallire la verifica. Le fasi recompile e behavior delle applicazioni reali sono ancora incomplete, quindi resta l’etichetta Experimental. Il superamento dei test di controllo dell’harness non dimostra il successo sulle applicazioni reali.

La pubblicazione è transazionale: scegliere una directory nuova, controllare prima lo stato d’uscita e salvare altrove il JSON rediretto. Gli errori eliminano l’output temporaneo e preservano quello esistente. Un’uscita non nulla del backend include una coda limitata del log. I timeout del backend mantengono il messaggio originale e aggiungono una coda limitata quando è disponibile testo del log già acquisito. I superamenti dei budget mantengono i propri messaggi distinti. La CLI nativa restituisce zero in caso di successo e un valore non nullo in caso di errore. Con `--json`, gli errori gestiti includono `schema_version`, `status: "error"` ed `error`. Parsing degli argomenti, errori di avvio dell’eseguibile o delle librerie native e interruzioni possono essere segnalati soltanto su stderr. Controllare prima lo stato di uscita.

Per slice cifrate fornire input leggibili; per architetture assenti controllare le slice disponibili. Leggere motivi esatti e diagnostica dei metodi omessi. Aumentare `--max-func` aiuta soltanto le funzioni escluse dal limite. Layout, firme, header esterni, eccezioni o ABI mancanti richiedono implementazione o ulteriori metadati validi, non una dichiarazione di recupero completo. Conservare gli avvisi di licenza applicabili distribuendo strumenti o pacchetti generati.

L’inferenza delle funzioni ausiliarie scalari native supporta anche parametri e risultati float/double nei registri. L’analisi condivisa dei byte di ingresso MedIR deve dimostrare che un ingresso vettoriale ampio osserva solo una corsia scalare inferiore. I valori locali CONCAT possono essere ridotti solo se tutte le definizioni hanno la stessa larghezza inferiore, le espressioni superiori eliminate non hanno effetti e ogni uso legge esplicitamente quel prefisso. Chiamate, scritture, posizioni dei rami e bit in virgola mobile diversi da NaN restano invariati; i bit superiori ignoti non vengono inventati.

Una funzione ausiliaria nativa foglia che inoltra a chiamate finali void esterne validate può usare una firma sorgente interna void in assenza di un risultato scalare completo. LowIR deve confermare ogni chiamata e il relativo ritorno sintetico. La prova foglia vieta scritture nei registri preservati, di frame, stack e collegamento; un punto fisso limitato di contaminazione per byte rifiuta inoltre valori derivati dallo stack memorizzati o passati a una chiamata. Restano i controlli di CFG, corpo e dipendenze. Nessun bit di risultato viene fornito: i chiamanti che leggono un risultato ignoto restano non recuperati. Le regressioni verificano distruzione condizionale degli oggetti, risultati indipendenti e rifiuto delle letture di risultati ignoti.

I riepiloghi nativi void supportano anche frame di stack e chiamate ordinarie quando un’analisi LowIR limitata dimostra che ogni uscita ripristina i byte dei registri preservati, il puntatore dello stack e il registro di collegamento. Scritture parziali, estensioni implicite con zeri, memorizzazioni sovrapposte e chiamate invalidano i fatti interessati. Le memorizzazioni tramite indirizzi che non contengono byte derivati dal frame sono disgiunte dal frame privato dell’invocazione; gli alias derivati parzialmente o interamente dal frame, nonché la memorizzazione o la fuga del suo indirizzo, impediscono la prova. L’unica eccezione di prestito del frame è il primo argomento esatto di un `objc_msgSendSuper2` associato alla sorgente: può indicare un oggetto `objc_super` completo di 16 byte, interamente contenuto nel frame allocato, perché tale contratto di runtime lo legge in modo sincrono. I messaggi ordinari, i puntatori incompleti, gli oggetti fuori dal frame e ogni altro argomento del frame restano rifiutati. La vivacità per byte di HighIR tratta il valore di uno slot dello stack come una lettura limitata dei byte di quello slot, mentre prendere o passare il suo indirizzo resta una fuga; gli accessi non validi o sovrapposti restano conservativi. Ciò consente alle memorizzazioni private disgiunte di eliminare solo i byte finali non letti. Dopo un nuovo lifting, i parametri di registri ausiliari senza occorrenze in HighIR possono essere rimossi prima di rieseguire la pipeline. La pulizia HighIR esistente gestisce ancora le memorizzazioni private; i parametri ordinari e gli ingressi effettivamente usati restano invariati.

Il catalogo UIKit arm64 associa anche il risultato oggetto di `observedProgress` e `setProgress:animated:` di `UIProgressView`. Quest’ultimo conserva registri separati per gli argomenti `float` e booleano. Entrambe le dichiarazioni richiedono prove concordanti prodotte dal compilatore per iPhoneOS e arm64 iPhoneSimulator e l’esatto fornitore UIKit di sistema. Le altre architetture restano non supportate.

Le parole dei letterali con tag possono contenere un indirizzo completo dell’immagine da un pool immutabile di stringhe C sotto l’esatto tag del bit più alto. Il binding sorgente ricolloca solo quell’indirizzo provato nel pool condiviso permanente e conserva OR intero, tag e offset interno. Valori scalari o indirizzi parziali simili, provenienza in conflitto, pool modificabili o con rilocazioni e usi diretti come indirizzo di memoria restano rifiutati; non viene dedotto il layout di un oggetto Swift String.

`setProgress:` di `UIProgressView` richiede un ricevitore provato, per esempio il risultato tipizzato di una proprietà conservato da una chiamata ARC di identità esatta. La chiusura verificata di superclassi e protocolli seleziona l’argomento `float`. Senza ricevitore qualificato la chiamata resta ambigua, poiché UIKit e classi locali dichiarano anche setter con argomento oggetto per lo stesso selettore.

Il catalogo UIKit per arm64 registra anche `setTitleColor:forState:` di `UIButton`, con un oggetto e un `UIControlState` senza segno a 64 bit, e il metodo void `invalidateIntrinsicContentSize` di `UIView`. Gli AST completi del dispositivo e del simulatore concordano. L’ereditarietà osservata `UIImageView → UIView` consente alle prove di receiver esistenti di distinguere i setter locali di oggetti dai setter in virgola mobile omonimi di classi non correlate. Restano non supportati receiver sconosciuti, dichiarazioni di sottoclassi in conflitto, fornitori di dichiarazioni padre mancanti e x86_64.

Una ricerca in un dizionario che restituisce `id` senza qualificazione non acquisisce una classe ricevente unendosi a un percorso che restituisce una classe nota tramite `new`. Ogni percorso in ingresso deve conservare la prova del ricevente; anche un argomento puntatore con tipo esplicito non consente di scegliere un setter di oggetti rispetto a una dichiarazione in virgola mobile incompatibile.

Su arm64, `+[NSSet setWithObjects:]` conserva la dichiarazione variadica dell’SDK terminata da nil. Il primo caso supportato richiede un’importazione esatta della classe di piattaforma e uno stub del selettore, oltre a stringhe Objective-C immutabili i cui valori completi di otto byte coincidano su tutti i percorsi entranti. Il recupero si ferma al primo nil certo. Se il primo oggetto è nil, non occorre provare gli argomenti successivi sullo stack e gli slot oltre il terminatore non vengono letti. L’ABI variadica Darwin condivisa mantiene tre parametri fissi e colloca gli altri oggetti e nil sullo stack. La pubblicazione verifica nuovamente dichiarazione, libreria fornitrice, ricevente, selettore e ogni argomento nell’immagine corrente. Oggetti dinamici, scritture mancanti o parziali, frame sfuggiti, ridefinizioni locali e altre architetture restano non supportati. Ogni argomento sullo stack richiede anche una prova lineare nel frame privato al preciso momento del caricamento: le sovrascritture successive non cambiano il valore già letto; scritture sovrapposte, chiamate sconosciute, escape o flussi di controllo impediscono la certificazione.

L’esportazione del sorgente può proiettare un getter super BOOL generato dal compilatore per ciascun ingresso Objective-C solo quando le prove ARM64 attuali e complete delle istruzioni, del CFG e dei salvataggi/ripristini certificano ingresso, corpo condiviso e accessor dei metadati (`CMa`). Individuazione, binding e generazione usano un unico contratto. L’helper conserva la chiamata CMa effettiva e le sue dipendenze chiuse, poi legge la cella SEL registrata a runtime e chiama `objc_msgSendSuper2` con il tipo certificato; selettori diversi mantengono celle distinte. Il loader ricontrolla il linkage locale tramite simboli Mach-O ed export trie LLVM e la corrispondenza esatta delle attuali mappature di segmenti/sezioni. Gli altri chiamanti e l’ABI nativa globale della funzione condivisa restano invariati; prove mancanti o obsolete vengono rifiutate.

Un inizializzatore Swift once arm64 può copiare un oggetto tra slot statici distinti tramite un helper nativo già tipizzato. Un solo contratto verifica tutti i parametri attuali e tutti i percorsi, mantenendo nell’ordine lettura del predicato, eventuale `swift_once`, lettura sorgente, scrittura destinazione, `objc_retain` e ritorno. Accetta quattro parametri di ruolo e un eventuale ingresso inutilizzato; solo l’inferenza nativa rimuove parametri. Scoperta e binding riconvalidano lo stesso contratto, i simboli esatti di callback e memoria, intervalli disgiunti di otto byte e il contesto inutilizzato del callback nell’immagine attuale. ABI, flusso di controllo, effetti di memoria e controlli delle dipendenze restano intatti; alias, accessi parziali o ordinati, usi aggiuntivi e prove obsolete vengono rifiutati.

Le dichiarazioni complete degli SDK iOS per dispositivo e simulatore confermano anche `UIGraphicsBeginImageContextWithOptions(CGSize, BOOL, CGFloat)` come chiamata C fissa con risultato void. Su ARM64, i due campi della dimensione occupano `d0` e `d1`, il booleano usa il byte in `w0` e la scala occupa `d2`. L’esportazione esatta del linker UIKit autentica il fornitore. Il sorgente mantiene la chiamata reale e gli aggiornamenti dei contatori del wrapper; rifiuta fornitori errati, architetture non supportate e dichiarazioni alterate. I wrapper nativi richiedono comunque prove indipendenti dello stato e del valore restituito.

Le stesse prove verificate dell’SDK UIKit associano `CGSizeFromString(NSString *)` al suo risultato `CGSize` composto da due double. L’ABI condivisa conserva sia `d0` sia `d1`, inclusi i risultati distinti ancora utilizzati dopo un’altra chiamata. La verifica dell’importazione corrente e della firma rifiuta fornitori diversi, sostituti scalari, parametri puntatore modificati e portatori del risultato mancanti o scambiati.

La preservazione del frame nativo ARM64 può leggere un argomento in ingresso sullo stack quando la firma di ingresso attualmente inferita ne descrive esplicitamente lo slot scalare allineato di otto byte. Il caricamento LowIR deve corrispondere esattamente all’intero slot. I byte restano valori di ingresso sconosciuti e non dimostrano l’identità di un registro salvato né un indirizzo nel frame privato della funzione chiamata. Scritture, letture parziali, spazi vuoti e slot non dichiarati non ricevono questa autorizzazione. Il corpo sorgente, gli argomenti di ogni chiamante e la chiusura delle dipendenze richiedono ancora convalida.

Le dichiarazioni complete dell’SDK UIKit per dispositivo e simulatore associano `UIAccessibilityPostNotification` a `void(uint32_t, id nullable)`: ARM64 passa la parola di notifica senza segno in `w0` e il puntatore all’oggetto in `x1`. `UIAccessibilityAnnouncementNotification` è memoria esterna `const uint32_t`. L’importazione UIKit esatta ne certifica l’indirizzo; restano la lettura originale di quattro byte e la chiamata reale, anche con argomento nil, senza sostituire il numero della notifica. Provider errati, importazioni deboli e modifiche a larghezza, segno o ritorno vengono rifiutati; le altre dipendenze di memoria inizializzata su richiesta richiedono una prova propria.

Le rilocazioni concatenate generic64 di Mach-O conservano gli otto bit alti codificati sia in `DYLD_CHAINED_PTR_64` sia in `DYLD_CHAINED_PTR_64_OFFSET`. La variante con offset ricostruisce prima la parola completa, poi somma la base preferita dell’immagine verificando l’overflow, secondo [l’esecuzione di dyld](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/common/MachOLoaded.cpp#L771-L792). I byte caricati mantengono quel valore completo a runtime. I controlli di appartenenza dei normali puntatori a codice e dati usano l’intero indirizzo mappato; le stringhe Swift con tag richiedono una prova separata della loro rappresentazione. Gli indirizzi alti validamente mappati restano supportati.

Un lettore separato di valori concatenati immutabili espone l’intera parola risolta a runtime solo per uno slot esatto di rebase Mach-O in memoria immutabile, presente nel file e con proprietario univoco. Rifiuta fixup non risolti, ambigui, importati o sovrapposti. Il valore non conferisce identità di puntatore ordinario né autorizza a copiare byte rilocati: il consumatore sorgente deve dimostrare separatamente rappresentazione e rilocazione, compreso qualsiasi tag di stringa Swift.
