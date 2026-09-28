**Lingue**: [English](../interpreter-recovery.md) | [简体中文](../zh-CN/interpreter-recovery.md) | [繁體中文](../zh-TW/interpreter-recovery.md) | [日本語](../ja/interpreter-recovery.md) | [한국어](../ko/interpreter-recovery.md) | [Français](../fr/interpreter-recovery.md) | [Deutsch](../de/interpreter-recovery.md) | [Español](../es/interpreter-recovery.md) | [Italiano](interpreter-recovery.md) | [Русский](../ru/interpreter-recovery.md) | [العربية](../ar/interpreter-recovery.md)

# Recupero del sorgente da interpreti

[← Indice della documentazione](README.md)

La fase sperimentale di specializzazione degli interpreti elimina il dispatch
risolto staticamente da una funzione x64 già collegata, conservandone gli input
a runtime, gli effetti sulla memoria, le diramazioni e i cicli. Usa la semantica
delle istruzioni, senza firme degli handler né tabelle di opcode specifiche di
un particolare sistema di protezione.

```sh
neverd decompile program --func vm_entry --devirtualize \
  --recovery-report recovery.json -o recovered.c
neverd decompile program --func vm_entry --devirtualize \
  --llvm -o recovered-llvm.c
```

`--vm-control` seleziona registri generali interi che distinguono i contesti
dell’interprete. Può essere ripetuto e non fornisce valori concreti. Si può
selezionare, per esempio, un cursore del bytecode il cui valore viene stabilito
dal codice d’ingresso. Un input a runtime usato come contatore deve restare
dinamico. Una separazione insufficiente dei contesti può interrompere il
recupero quando confluiscono valori diversi del cursore; il motore non deve
compensare indovinando una destinazione di dispatch. Per un cursore salvato
sullo stack, `--vm-control-stack=-16:8` seleziona otto byte a RSP d’ingresso meno
16. L’offset è relativo all’ingresso della funzione, non al puntatore dello
stack dopo le successive modifiche.

L’API C è `neverd_devirtualize_source_v1()`, dichiarata in
`neverd/sdk/NeverDCAPIDevirtualize.h`. Esegue una transazione separata senza
modificare la normale cache di decompilazione della sessione. Un errore non
restituisce sorgente, ma può comunque restituire una diagnosi JSON. Entrambe
le stringhe allocate si liberano con `neverd_free_string()`.

## Individuazione automatica dello stato di controllo

La CLI e tutte le versioni delle API C di recupero del sorgente attivano l’individuazione automatica per impostazione predefinita. Nell’API C++ indipendente dal provider, `SpecializationOptions::DiscoverControlState = false` rimane il valore predefinito; il chiamante può impostare `true`. I suggerimenti manuali `--vm-control` e `--vm-control-stack` restano chiavi di contesto facoltative. I normali campi automatici conservano relazioni congiunte di valori finiti e limitati, senza creare chiavi di contesto né fissare gli input a valori campionati. I contatori restano dinamici, salvo quando il raffinamento selettivo della memoria descritto sotto richiede le loro costanti dimostrate.

L’analisi segue le dipendenze irrisolte di controllo e indirizzamento fino agli input strutturati dei registri all’ingresso del nodo e all’origine di creazione degli input di memoria, incluse porzioni ristrette di byte. Uno slot del frame viene proposto solo se tale origine identifica memoria non modificata dall’ingresso del nodo in un intervallo esatto relativo al frame all’ingresso della funzione. Letture successive di valori uguali o inoltrati da una scrittura non creano ulteriori dipendenze di input; i byte sconosciuti creati dopo un’invalidazione della memoria non sono considerati slot d’ingresso. La cronologia delle letture resta disponibile alle altre analisi. Le dipendenze mancanti fanno ripartire l’analisi dall’ingresso della funzione. Ogni relazione conservata richiede ancora una prova completa del dominio finito dei bit che vincola; le scritture con possibili alias invalidano ancora i fatti sulla memoria. Memoria esterna arbitraria e relazioni di valori illimitate non diventano finite.

Le richieste ai produttori conservano le maschere di bit attraverso i confini dei nodi, senza estenderle a byte interi; le operazioni aritmetiche includono comunque, in modo conservativo, il prefisso di bit meno significativi che può propagare riporti o prestiti verso un bit richiesto.

Se dipendenze di memoria già tracciate impediscono ripetutamente una prova di indirizzo esatto, il raffinamento può promuovere le loro costanti in ingresso dimostrate a chiavi di contesto. Non suddivide tuple con più valori in nuovi archi e non aggiunge letture della memoria ospite per scegliere il contesto: provare un dominio finito non garantisce la sicurezza di una lettura aggiuntiva. Stati dinamici o illimitati dipendenti dalla memoria possono quindi ancora arrestare il recupero entro i limiti configurati.

Le richieste a ritroso verso i produttori sono identificate dall’ingresso del nodo nativo, dalla modalità delle istruzioni, dal tipo di campo e dal suo intervallo di byte. Solo un arco il cui successore richiede quel campo ne espande le dipendenze di produzione, anche quando il dominio finito resta troppo impreciso. Ciò può riavviare l’analisi dall’ingresso entro i budget, senza assegnare un unico ruolo globale a un registro fisico riutilizzato. La scoperta delle dipendenze è limitata e incompleta; il recupero senza indicazioni manuali non è garantito per ogni interprete.

Anche la proiezione dei normali campi automatici è limitata alle richieste del nodo di destinazione. Un campo non costante viene incluso nella relazione congiunta di valori finiti di un arco solo se la destinazione lo richiede. I campi manuali e quelli promossi automaticamente a chiavi di contesto continuano a essere proiettati globalmente. I byte costanti noti, i puntatori esatti relativi al frame all’ingresso e i fatti di provenienza vengono conservati indipendentemente dalle richieste. Questo evita che campi non correlati, usati in fasi diverse dei gestori, moltiplichino le combinazioni di valori della relazione. I budget configurati e le prove richieste per le destinazioni di controllo, gli indirizzi di memoria e lo stato al ritorno restano invariati.

Per i normali campi automatici, ogni colonna di una tupla vincola soltanto i bit richiesti e porta la maschera corrispondente. Le unioni conservano solo i bit vincolati su tutti i percorsi in ingresso. Gli altri bit dello stesso byte restano valori di esecuzione: l’intervallo di memorizzazione di un campo non certifica un dominio finito completo per l’intero intervallo. Un byte diventa costante solo quando tutti gli otto bit sono dimostrati costanti.

I campi manuali e quelli promossi a chiavi di contesto continuano a tentare prima, globalmente, la prova di relazioni sull’intera larghezza. Se la prova non è conclusiva, possono conservare una relazione limitata ai bit richiesti; ciò non fornisce mai byte non dimostrati a una chiave di contesto e non aggira un budget globale esaurito.

Al riavvio con un nuovo grafo, i normali intervalli di memorizzazione automatici interamente contenuti in altri campi possono condividere quei campi più ampi. Le posizioni di fase e le richieste di bit originali restano invariate. I campi manuali, quelli di contesto e quelli proposti direttamente da un indirizzo di memoria irrisolto mantengono gli intervalli esatti. `MaxControlFields` limita i campi effettivamente conservati dopo questa normalizzazione. `DiscoveredControlFields` resta cumulativo e può superare il numero di campi attivi; il limite dei campi e i budget globali di lavoro non aumentano.

Un normale campo automatico con un dominio finito completamente dimostrato non aggiunge subito tutti i propri produttori come campi di controllo. L’analisi registra queste dipendenze candidate e le attiva solo se il recupero resta bloccato e il raffinamento immediato non produce candidati. I campi promossi a chiavi di contesto continuano a espandere subito i propri produttori. Visite di scoperta, riavvii e lavoro di prova mantengono i budget cumulativi esistenti.

Un dominio finito completo può anche dimostrare che singoli byte sono costanti mentre varia l’intera parola. Per esempio, il dominio `{0, 0x100}` ha il byte meno significativo costante. Si conservano come costanti soltanto i byte uguali in tutte le tuple enumerate, secondo l’ordine dei byte della destinazione; gli altri restano dinamici. Un’enumerazione parziale, un risultato sconosciuto del risolutore o l’esaurimento del budget di prova non forniscono fatti di questo tipo.

Le prove di domini finiti possono essere riutilizzate all’interno di un’esecuzione del recupero, compresi i riavvii di raffinamento. Una cache limitata confronta l’intero DAG ordinato delle espressioni, a meno di una rinomina coerente delle variabili libere; condivisione delle variabili, larghezze, bit costanti, parametri degli operatori e limite di proiezione restano nella chiave. Si conservano solo domini completi e prove che un dominio supera il limite. Risultati sconosciuti o parziali non vengono memorizzati; in assenza di una voce o con capacità insufficiente si usa la prova ordinaria. Tutti i budget globali restano applicati e `solverQueries` conta le chiamate effettive al risolutore.

Sotto lo stesso predicato, il dominio completo di una sola colonna variabile e un unico valore dimostrato per ciascuna delle altre determinano la relazione congiunta esatta, purché la raggiungibilità sia stabilita. Più colonne variabili richiedono ancora una prova congiunta. Una colonna mascherata che copre tutto il proprio dominio di bit può essere omessa solo dopo aver provato che i bit d’ingresso sono indipendenti dal predicato e da tutte le altre colonne. Input condivisi, risultati sconosciuti o enumerazioni parziali non giustificano mai l’ipotesi di un prodotto cartesiano.

I valori predefiniti sono `MaxControlFields = 16` per campi manuali e automatici complessivamente, `MaxControlRefinements = 16` e `MaxDiscoveryVisits = 65536`. I riavvii condividono budget globali per nodi (inclusi quelli sintetici), operazioni, valutazioni, query e visite di individuazione. `contexts` conta i contesti nativi e, come `evaluatedOperations`, `nodeEvaluations` e `solverQueries`, si accumula tra i tentativi. Contesti per indirizzo, slot attivi di ritorno nativo, campi e tuple restano limiti strutturali per tentativo. Il limite delle query resta 4096. L’esaurimento non pubblica risultati parziali; `residualBlocks` descrive solo il grafo residuo finale.

L’opzione CLI `--vm-max-refinements=N` richiede un intero positivo e ha valore predefinito 16. In C, lo stesso limite si configura tramite `neverd_devirtualize_source_v2()` o `neverd_devirtualize_machine_source_v2()`: inizializzare a zero `neverd_devirtualize_options_v2`, impostare `base.struct_size = sizeof(neverd_devirtualize_options_v2)`, quindi `max_control_refinements` (zero mantiene il valore predefinito 16). Il membro incorporato `base` contiene le opzioni v1; entrambi i membri reserved devono restare zero. I layout e i punti d’ingresso v1 esistenti non cambiano e ignorano le estensioni finali. Restano validi gli altri budget di lavoro e di prova.

Il rapporto JSON aggiunge `discoverControlState`, `maxControlRefinements`, `maxDiscoveryVisits`, `discoveredControlFields`, `discoveredContextFields`, `controlRefinements` e `discoveryVisits` per registrare attivazione, limiti e lavoro di analisi. L’individuazione dei campi, da sola, non prova il successo del recupero.

## Contratto di esecuzione

L’adattatore binario accetta attualmente immagini x64 ELF e PE già collegate,
agli indirizzi in cui sono mappate. Mappature, byte e permessi devono restare
invariati, senza modifiche concorrenti. Il lifting rigoroso su richiesta segue
il codice macchina raggiungibile. Istruzioni non supportate, chiamate,
operazioni opache, accessi alla memoria ordinati, controllo non risolto e
gestione delle eccezioni del linguaggio interrompono il recupero.

Il recupero PE richiede tutti i metadati dell’immagine, comprese le rilocazioni globali e i record delle eccezioni. La CLI li carica prima di applicare `--func`; chi usa l’API C non deve prima limitare la sessione con `neverd_session_restrict_function()`. L’adattatore rifiuta immagini caricate con un insieme ristretto di funzioni, perché i metadati omessi non dimostrano l’assenza di correzioni del loader o archi di eccezione.

Solo intervalli completi di sola lettura, sostenuti dal file e privi di
mappature sovrapposte o correzioni del loader, possono fornire letture costanti
dell’immagine. Tabelle scrivibili, rilocazioni non risolte e istantanee campionate
durante l’esecuzione non provano l’immutabilità. Le rilocazioni COPY e le directory delle eccezioni strutturalmente incomplete
vengono rifiutate. Se la directory PE e gli intervalli delle funzioni sono
completi, un gestore sconosciuto in un’altra funzione non blocca l’analisi
dell’ingresso scelto; raggiungere il codice che copre interrompe il recupero. Il dominio
ammesso richiede ritorni ABI ordinari: l’intervallo di destinazione di ogni
scrittura di origine esterna deve essere disgiunto dallo slot dell’indirizzo
di ritorno all’ingresso. È una precondizione esplicita per chiamante e ambiente,
anche per indirizzi calcolati da interi esterni; l’assenza di provenienza dal
frame dello stack non prova la disgiunzione numerica. Gli indirizzi di scrittura
derivati dal frame devono dimostrare tale disgiunzione e il puntatore dello
stack originale deve essere ripristinato al ritorno. Le informazioni di origine
sopravvivono ai salvataggi sullo stack e alle confluenze; perdere un’espressione
affine non la trasforma in un puntatore esterno. Pivot dello stack, ritorni che
rimuovono gli argomenti nella funzione chiamata e dispatch basato su RET sono
attualmente rifiutati. L’adattatore impone la semantica little-endian di x64.

Il risultato è sorgente e IR per l’analisi. Non dimostra la sicurezza di
rilocazione, unwinding, eccezioni asincrone o sostituzione binaria. La modalità
patch rifiuta questa opzione. Non si garantisce il supporto di ogni interprete
o configurazione di protezione.

Le forme x64 esatte di `PUSHFQ`/`POPFQ` restano nel programma residuo. L’analisi tratta ogni istantanea dei flag della macchina come un valore di runtime sconosciuto; il lifter combina separatamente i flag aritmetici modellati. Anche il ripristino dei flag resta un effetto di runtime. Un indirizzo derivato da flag sconosciuti non può usare il contratto di non sovrapposizione del puntatore esterno con lo slot dell’indirizzo di ritorno; un dispatch derivato senza destinazioni dimostrate finite continua a fallire.

Prima di acquisire l’intera immagine dei flag, ogni flag aritmetico o di direzione modellato deve essere definito nella funzione recuperata. Ogni lettura diretta di un flag richiede inoltre una definizione su tutti i percorsi predecessori raggiungibili, anche se la semplificazione simbolica ne annulla il valore. Altrimenti il recupero viene rifiutato, invece di emettere C con una trappola per un registro sconosciuto.

I temporanei LowIR sono locali a una sola istruzione nativa sollevata. Ogni byte letto deve essere stato definito prima nella stessa istruzione; il riuso di un offset da un’istruzione precedente o l’annullamento algebrico di un valore indefinito non dimostrano la validità del sorgente. Le costanti d’ingresso possono vincolare solo registri fisici.

## Limiti attuali

Gli indirizzi di bytecode dipendenti dall’ingresso e le relazioni fra stati del decodificatore sono supportati solo quando i domini finiti e le correlazioni necessari sono dimostrabili entro i limiti configurati. Questo non dimostra il supporto di qualsiasi schema di decodifica indiretta. Rami e cicli dinamici possono essere recuperati se ogni destinazione di dispatch è dimostrata; la copertura dei normali rami non basta a provarlo. Controllo irrisolto o esaurimento di un budget di prova necessario costituiscono un errore, senza pubblicare sorgenti recuperati o sostituzioni parziali. Chiamate ausiliarie native, confini di eccezione o rientro, codice modificabile e altre architetture restano fuori dal contratto dell’adattatore.

## Implementazione condivisa

`SpecializationProvider` fornisce istruzioni completamente sottoposte a lifting
e prove delle letture immutabili. `NeverDInterpreterSpecialization` usa la
semantica esistente di `SymExec` per valutare parzialmente le operazioni intere
e di controllo. L’adattatore binario gestisce mappature e decodifica; non
implementa un secondo valutatore delle istruzioni.

Per un indirizzo di lettura simbolico con dominio finito, il risolutore di vettori di bit integrato enumera gli indirizzi candidati sotto i vincoli correnti. L’insieme viene accettato solo dopo un risultato UNSAT finale che dimostri l’assenza di altre possibilità, e ogni indirizzo deve avere un certificato completo di lettura immutabile che non provochi errori di memoria. Tale lettura certificata può essere sostituita in LowIR da una cattura dell’indirizzo e una catena esatta di SELECT; le normali letture non certificate restano dinamiche. Un campione di indirizzi non sostituisce mai l’insieme completo. I registri di controllo e gli slot del frame d’ingresso selezionati possono conservare tuple congiunte limitate fra nodi, per esempio la relazione fra cursore e chiave di decodifica. Unioni e allargamenti restano conservativi. Modelli SAT parziali o risultati sconosciuti non provano la completezza dell’insieme di indirizzi o destinazioni. Questo meccanismo non richiede il backend Z3 opzionale.

Un nodo è identificato dal cursore nativo, dalla modalità delle istruzioni e dalle costanti selezionate dei registri di controllo e degli slot del frame d’ingresso. Gli altri fatti a livello di byte si uniscono per intersezione. Quando un fatto in ingresso si indebolisce, il nodo viene valutato nuovamente. Così i cicli del programma restano cicli, senza espandere ogni iterazione osservata. Tutti i valori raggiungibili di una destinazione indiretta devono appartenere a un insieme limitato la cui completezza sia dimostrata; le destinazioni selezionate diventano confronti residui espliciti e archi del CFG.

Operazioni dinamiche e normali letture/scritture restano in LowIR. Costanti scalari, puntatori
affini relativi al frame d’ingresso e byte del frame provati costanti possono
attraversare i nodi; le altre espressioni vengono scartate anziché espanse senza
limite. La memoria del frame usa l’invalidazione conservativa degli alias dello
stato simbolico esistente. Una scrittura tramite un puntatore sconosciuto che
potrebbe sovrapporsi invalida i fatti in conflitto. Non si presume che uno slot
dello stack sia privato né se ne eliminano gli effetti in base a un contratto
di assenza di alias non dimostrato.

Etichette sintetiche uniche delle istruzioni distinguono i contesti clonati.
I confini delle istruzioni originali restano in una mappa d’origine separata;
le certificazioni originali di rilocazioni, eccezioni e tabelle di salto non
vengono copiate sulle nuove occorrenze. Il LowIR recuperato entra nella normale
conversione LowIR-MedIR prima della separazione tra HighC e LLVM, condividendo
la gestione di registri, stack, CFG, SSA e ABI. Il percorso HighC richiede anche
che la verifica MedIR riesca.

Budget per nodi, contesti per indirizzo, operazioni, valutazioni dei nodi e
destinazioni finite limitano l’analisi. Se un budget si esaurisce o la semantica
non è supportata, non viene pubblicata alcuna funzione residua. Un grafo di
controllo completo è distinto dalla corretta emissione del sorgente; l’API
pubblica verifica entrambi i risultati e ne segnala la differenza.

Gli insiemi finiti di indirizzi di lettura, le tuple congiunte di controllo e il numero di campi di controllo hanno limiti espliciti. Un limite globale alle interrogazioni del risolutore e limiti per interrogazione a porte, conflitti, propagazioni e visite ai letterali sorvegliati delimitano il lavoro di prova; il limite ai nodi simbolici delimita la crescita delle espressioni. Il rapporto JSON include questi budget insieme a `solverQueries` e `relationalWidenings`.

## Evidenze e test

Il report JSON locale facoltativo include hash dell’input, controlli scelti,
budget, stato, contatori del lavoro, numero di blocchi residui, posizioni delle
istruzioni originali e byte immutabili usati nel recupero. Contiene informazioni
derivate dall’input e viene scritto solo nel percorso locale richiesto.

I test pubblici usano macchine originali con dispatch tramite registri e stack,
ciascuna con programmi aritmetici, diramazioni con confluenze e cicli a runtime.
Un oracolo indipendente senza segno verifica ritorni, scritture in memoria,
riporti/prestiti e sentinelle di uscita. HighC e LLVMC recuperati vengono compilati
a O0/O2 con trap per comportamento indefinito ed eseguiti contro l’oracolo.
I casi negativi coprono dispatch non risolto o scrivibile, ordine dei byte
incompatibile, metadati delle eccezioni e budget.

Ulteriori fixture originali a indirizzi finiti usano record di sola lettura selezionati dall’ingresso, campi di controllo cursore/chiave correlati e uno stesso handler in posizioni virtuali diverse. Coprono rami con ricongiungimenti e cicli la cui selezione del record dipende dallo stato corrente del programma. L’oracolo nativo indipendente usa le convenzioni SysV e Win64; entrambi i percorsi C recuperati vengono verificati in O0/O2 con trap per comportamento indefinito e sentinelle di uscita. Certificati di lettura mancanti o budget di prova insufficienti non devono pubblicare risultati parziali.

I target di test mirati sono descritti in [testing.md](testing.md).

## Recupero con stato macchina esplicito

`--devirtualize --vm-machine-state` o `neverd_devirtualize_machine_source_v1()` seleziona una ABI separata: un puntatore a 17 parole `uint64_t` allineate (RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8–R15, RFLAGS). Solo lo stato unsigned a 64 bit uguale a zero indica successo. Un valore diverso non annulla le scritture. Lo stato è acquisito prima del pop dell’indirizzo nel RET finale. La sua memoria non deve sovrapporsi a quella guest; sono richieste le mappature originali e un host little-endian a 64 bit.

Il profilo richiede CPL3/IOPL0, shadow stack disattivato, assenza di eventi asincroni ed esecuzione normale senza fault. I flag iniziali devono essere canonici con TF/RF/VM/AC/VIF/VIP a zero; POPFQ deve mantenere TF/AC a zero, verificato dal codice generato. PUSHFQ/POPFQ usano lo stato esplicito; RDSSP conserva la destinazione e INCSSP raggiunto viene rifiutato. Le chiamate near dirette conservano la scrittura reale del ritorno; i RET interni richiedono una destinazione unica dimostrata. I puntatori di frame salvati integralmente si propagano; scritture parziali o alias possibili invalidano i fatti. I metadati di eccezione ammettono solo il percorso normale, senza equivalenza di dispatch o unwinding. Il rapporto registra `sourceABI` e `executionProfile`; l’ABI predefinita mantiene le restrizioni.

L’ABI sorgente ordinaria ricostruisce un frame privato dell’invocazione. Ogni intervallo LOAD/STORE di origine esterna, incluse le destinazioni calcolate, deve essere disgiunto dal frame nativo privato e dalla sua memoria ricostruita nel sorgente: è una precondizione esplicita. La prova condivisa rifiuta indirizzi di frame che sfuggono, risultati o rami che ne dipendono e letture private non inizializzate. L’ABI con stato macchina conserva gli indirizzi guest e non usa questa precondizione sul frame privato.

Se i flag indefiniti influenzano il controllo, gli indirizzi o gli output definiti, serve una prova indipendente di non interferenza. Il rapporto attuale non fornisce tale prova né certifica questo comportamento dipendente dal processore.
