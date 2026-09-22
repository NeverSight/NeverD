**Lingue**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← Indice della documentazione](README.md)

# Emulazione dei driver Windows

L’emulatore opzionale di driver di NeverD esegue il punto di ingresso PE di un
driver WDM x64 supportato e, facoltativamente, percorre uno scenario esplicito
di richieste seriali prima di scaricarlo. Usa Unicorn per l’esecuzione della
CPU e il modello circoscritto dell’ambiente Windows proprio di NeverD. Non
carica il driver nel kernel dell’host e non inoltra le chiamate API del guest
ai servizi del sistema operativo host.

## Compilazione ed esecuzione

La funzionalità richiede un’attivazione esplicita ed è indipendente da `BUILD_TESTING`:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON
cmake --build build-release --target neverd --parallel 4
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --instruction-limit 100000 > driver-report.json
```

Il report viene sempre emesso come JSON su stdout; i messaggi diagnostici delle
richieste e della preparazione vanno su stderr. I limiti predefiniti sono 100000
istruzioni guest, 64 MiB di memoria guest, 10000 eventi registrati e 5000
millisecondi. Il limite di istruzioni deve essere positivo. L’esaurimento di
un budget interrompe l’esecuzione conservando le osservazioni parziali.

| Codice di uscita | Significato |
|-----------------|-------------|
| `0` | L’inizializzazione e tutte le operazioni richieste e completate sono riuscite |
| `1` | Input/opzioni non validi, errore di preparazione o funzionalità disabilitata in compilazione |
| `2` | L’inizializzazione o una richiesta completata ha restituito un `NTSTATUS` di errore |
| `3` | L’esecuzione si è fermata prima di completare lo scenario, ad esempio per un’API non supportata, un fault o un limite di budget |

Uno stato di errore restituito costituisce un’osservazione completata di
quell’operazione. Un ritorno riuscito descrive solo questa esecuzione modellata;
non dimostra che il driver funzioni in Windows.

## Compatibilità dei driver

La compatibilità dipende dal percorso di codice eseguito e dalle sue dipendenze,
non dall’estensione `.sys`. Le attuali evidenze di accettazione coprono fixture
originali autonome e i percorsi con buffer, in-direct e out-direct dell’esempio
WDM SIOCTL di Microsoft, inclusa la build con log di debug, e le letture/scritture
dirette dell’esempio pubblico Zero di Pavel Yosifovich. Non dimostrano la
compatibilità con driver arbitrari di terze parti.

| Classe di driver o requisito | Ambito attuale | Ambiente mancante |
|-----------------------------|----------------|-------------------|
| Driver WDM software x64 che usa le API elencate | Inizializzazione WDM x64 limitata, richieste seriali buffered/direct, lavoro, timer, DPC, eventi e attese, report e limiti | Ogni ulteriore API eseguita richiede un modello definito |
| IOCTL `METHOD_BUFFERED` | I/O buffered/direct seriale con completamento da lavoro o DPC | Solo le API seguenti; nessun IRP concorrente o annullamento di richiesta |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | MDL di proprietà delle richieste e mapping di sistema | identità delle pagine fisiche, DMA e mapping utente |
| MDL allocati dal driver | Descrittori autonomi del pool non paginato modellato, con indirizzi originali condivisi | Associazione IRP, catene MDL, verifica/blocco, pagine fisiche e mapping utente |
| READ/WRITE | I/O buffered/direct seriale con completamento da lavoro o DPC | Solo le API seguenti; nessun IRP concorrente o annullamento di richiesta; `METHOD_NEITHER` e posizione implicita del file |
| `METHOD_NEITHER` | Rifiutato | Contesto degli indirizzi utente, verifica degli accessi e gestione delle eccezioni guest |
| Driver KMDF 1.33 non PnP | Binding di versione, driver e oggetti generici, contesti tipizzati, riferimenti e callback di pulizia/scaricamento eseguiti | Dispositivi, code, richieste, estensioni di classe KMDF e UMDF restano esclusi |
| Driver PnP di bus, funzione o filtro | L’inizializzazione può essere eseguita nel sottoinsieme di API; il ciclo di vita dello stack di dispositivi non è supportato | Collegamento dei dispositivi, dispatch al driver sottostante, IRP PnP e di alimentazione |
| Driver di archiviazione, rete, visualizzazione, file system e minifilter | Contratti dei sottosistemi non supportati | Framework port/class/miniport, NDIS/WFP, servizi grafici o del file system |
| Lavoro, timer, DPC, eventi e attese | L’IRQL corrente è `PASSIVE_LEVEL` per dispatch e lavoro, `DISPATCH_LEVEL` per i DPC | Solo le API seguenti; nessun IRP concorrente o annullamento di richiesta |
| Driver con callback di processo/thread, handle, operazioni su registro/file o ricerca di moduli kernel | Sono supportate le operazioni di registro elencate su un albero configurato esplicitamente; le altre operazioni non sono supportate | Gestore degli oggetti generale, operazioni sui file, stato del sistema e produttori di callback/eventi |
| Driver hardware, DMA, PCI, di interrupt o di virtualizzazione | Ambiente non supportato | Modelli di dispositivi, memoria fisica, bus, interrupt e stato CPU privilegiato |
| Driver Windows x86 o ARM64 | Rifiutato | Caricamento, ABI e modello di esecuzione specifici dell’architettura |
| CFG x64 | Tabelle di destinazioni validate e chiamate check/dispatch; la strumentazione inattiva mantiene i fallback guest | XFG, soppressione degli export, configurazione non modellata e TLS restano rifiutati |

Un’importazione non supportata ma inutilizzata può restare collegata. Raggiungere
un’operazione non supportata interrompe l’esecuzione con una diagnosi e le
osservazioni raccolte fino a quel momento. Il solo successo di DriverEntry non
dimostra il supporto dei successivi percorsi di dispatch, hardware o framework.
La tabella delle API seguente definisce il sottoinsieme supportato di riferimento.

## Contratto di esecuzione

Il profilo modella un ciclo WDM x64 su CPU0 con scheduling cooperativo deterministico. L’esecuzione inizia dal punto di ingresso PE, mantenendo il
wrapper di ingresso del compilatore quando presente. DriverEntry deve restituire
`STATUS_SUCCESS` per inizializzare il driver; uno stato di successo diverso da
zero o uno stato pending interrompe l’esecuzione come contratto di
inizializzazione non supportato. Uno stato di errore viene conservato come
risultato di inizializzazione completato. Tutti gli oggetti, le stringhe, gli
stack, i puntatori a funzione e le allocazioni risiedono nella memoria guest.
Il modello fornisce un `DRIVER_OBJECT` e un percorso del registro per il nome
del servizio configurato (`NeverDDriver` per impostazione predefinita).
L’adattatore usa la modalità TLB virtuale di Unicorn per preservare gli indirizzi
virtuali guest, inclusi gli indirizzi kernel canonici alti, senza sintetizzare
tabelle delle pagine Windows. Il valore iniziale di RFLAGS è `0x202`; il profilo
del dispositivo software usa una linea di cache fissa di 64 byte. Queste sono
proprietà esplicite dello scenario di esecuzione.
Le letture inline x64 di CR8 osservano lo stesso `PASSIVE_LEVEL` / `DISPATCH_LEVEL`; le scritture
di CR8 e le altre operazioni sui registri di controllo restano non supportate.

Le importazioni sconosciute vengono collegate a trap attivate solo all’uso.
Un’importazione inutilizzata non impedisce l’esecuzione; eseguire il suo thunk
o leggere un valore di dati esportato non modellato arresta l’esecuzione con
`unsupported_api`. Anche gli effetti non supportati dell’ambiente CPU provocano
un arresto esplicito. NeverD non sostituisce le chiamate non implementate con
valori di successo. Le immagini malformate o i requisiti di caricamento non
supportati falliscono prima dell’esecuzione.

Gli elementi `DelayedWorkQueue` eseguono a `PASSIVE_LEVEL`, i callback DPC guest a `DISPATCH_LEVEL` con i quattro argomenti previsti. CPU0 usa scheduling cooperativo deterministico ai ritorni delle chiamate e alle attese bloccanti. Timer relativi, assoluti e periodici usano tempo virtuale che avanza alla prossima scadenza di timer o attesa quando nessun frame può eseguire. Eventi/timer di notifica e sincronizzazione mantengono regole distinte di consumo del segnale. Ogni callback ha uno stack guest separato; più frame bloccati conservano variabili locali e contesti CPU completi con memoria condivisa. Win64 passa i primi quattro argomenti nei registri, gli altri sullo stack. Le richieste restano seriali: il dispatch che marca un IRP pendente deve restituire `STATUS_PENDING` e completarlo prima della richiesta successiva. Senza produttore disponibile, richieste pendenti o attese infinite si arrestano con `model_error` per stallo. I budget di istruzioni, memoria, osservazioni e tempo reale restano condivisi.

Il modello limitato non offre tutto l’asincronismo Windows. Attese alertable o utente, thread di sistema, APC, annullamento delle richieste, spinlock, IRP concorrenti, cambi generali di IRQL, `METHOD_NEITHER`, UMDF e contratti KMDF di dispositivi/code/richieste, PnP/alimentazione completo, hardware, DMA e interrupt restano non supportati. L’inizializzazione sola esegue callback esplicitamente accodati senza creare richieste o scaricamenti impliciti.

L’elemento esce dalla coda prima dell’inizio del callback, che può liberare il proprio elemento. Liberare elementi ancora in coda, accodarli due volte, usare oggetti scaduti o destinazioni fuori dalla memoria guest eseguibile causa errori espliciti. Il riferimento al dispositivo resta fino al ritorno del callback. Lo scaricamento richiede la liberazione di tutti gli elementi e il completamento del lavoro in coda. I contesti CPU conservano registri generali, SIMD, FPU e stato di controllo; la memoria guest resta condivisa e un contesto salvato non consente di riprendere una CPU in errore.
L’eliminazione viene rinviata finché restano oggetti file o riferimenti di lavoro in coda/in esecuzione. L’allocazione degli elementi restituisce NULL quando l’arena oggetti è esaurita.

Le immagini usano la base preferita, salvo che uno scenario scelga un indirizzo
di rilocazione valido, e devono essere eseguibili PE32+ x64 con sottosistema
nativo. Le importazioni possono provenire da `ntoskrnl.exe`, `ntkrnlmp.exe` o `WDFLDR.SYS`.
Il loader di esecuzione supporta rilocazioni di base x64 `DIR64` validate e
una configurazione di caricamento limitata per il cookie di sicurezza,
inizializzato prima del wrapper di ingresso con un cookie guest deterministico.
Altri campi di configurazione di caricamento non modellati, TLS,
importazioni ritardate/bound, importazioni per ordinale e immagini gestite
vengono rifiutati. Le immagini devono anche superare controlli rigorosi su
intervalli e allineamento.

Control Flow Guard (CFG) attivo valida flag PE, slot dei puntatori e destinazioni eseguibili ordinate. Gli helper check/dispatch ammettono solo ingressi dichiarati dell’immagine o thunk API registrati, preservano lo stato Win64 e rifiutano le altre destinazioni. La strumentazione senza CFG attivo mantiene i puntatori di fallback guest originali. XFG attivo, soppressione degli export e altre politiche non modellate restano rifiutati; una memoria eseguibile non rende da sola valido un indirizzo.

KMDF è limitato all’ABI esatta 1.33.0: 458 slot hanno identità guest stabili, ma solo le 11 API seguenti hanno semantica di esecuzione. `WdfVersionBind` e `WdfVersionUnbind` validano e gestiscono il binding guest. Il vero wrapper WDK `FxDriverEntry` resta il punto di ingresso; `WdfGetDriver` legge l’handle modellato dai globals pubblici. Driver non PnP e oggetti generici supportano contesti tipizzati azzerati, riferimenti, proprietà del genitore e callback guest annidati di pulizia/distruzione/scaricamento con budget condivisi. Queste operazioni richiedono attualmente `PASSIVE_LEVEL`. Dispositivi, code, richieste, estensioni di classe, UMDF e PnP/alimentazione completo restano esclusi. Slot non modellati, `WdfLdrQueryInterface` e binding di classi arrestano esplicitamente l’esecuzione; la presenza in tabella non implica implementazione. I callback degli oggetti vengono eseguiti solo a `PASSIVE_LEVEL`; acquisire nuovi riferimenti dopo il completamento della pulizia resta fuori da questo profilo.

API KMDF modellate: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`.

La validazione WDK facoltativa compila separatamente `driver_kmdf_lifecycle.c` con la vera libreria di ingresso KMDF. `NEVERD_KMDF_FIXTURE` e `NEVERD_KMDF_CFG_FIXTURE` indicano immagini normale e CFG attivo; gli artefatti esterni mancanti producono skip espliciti. Le [verifiche](testing.md) descrivono i casi nativi e C API/CLI. Le prove attuali sono limitate a host Linux.

Il modello API iniziale ha intenzionalmente un contratto limitato:

| API | Comportamento modellato e restrizioni |
|-----|--------------------------------------|
| `RtlInitUnicodeString` | Costruisce una `UNICODE_STRING` guest per una sorgente limitata terminata da NUL |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | Copia UTF-16 a lunghezza esplicita e confronto sensibile alle maiuscole; il confronto che ignora le maiuscole richiede una tabella Windows e arresta l’esecuzione |
| `ExAllocatePool2` | Allocazioni NX paginabili/non paginabili, azzerate per impostazione predefinita; sono modellati i flag di memoria non inizializzata e allineamento alla cache; flag obbligatori non validi restituiscono NULL; pool con quote/eseguibili ed eccezioni di allocazione arrestano l’esecuzione |
| `MmGetSystemRoutineAddress` | Risolve un nome guest a lunghezza esplicita tramite l’inventario condiviso degli export |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | Albero di registro esplicito e locale alla sessione, handle indipendenti, diritti per handle, interrogazioni con lunghezze esatte e modifiche osservabili; nessun accesso al registro host né valutazione delle ACL Windows |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | MDL delle richieste con mapping KernelMode in cache e permessi; gli MDL del pool non paginato riutilizzano il mapping originale tramite la macro sicura |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | Descrittori autonomi con l’intero intervallo in una singola allocazione attiva del pool non paginato; durate indipendenti di descrittore e buffer; senza IRP, catene o quote |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Allocazioni di dati per i tipi di pool `0`, `1` e `512`; dimensione/tag positivi, tag corrispondenti nelle liberazioni con tag, nessun riutilizzo degli indirizzi |
| `IoCreateDevice`, `IoDeleteDevice` | Tipo di dispositivo `0x22`, caratteristiche `0` o `0x100`, estensioni limitate, nomi ASCII `\Device\Name` |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` o `\??\Name` in un unico namespace di sessione, con destinazione `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Formattazione variadica Win64 verificata, al massimo 512 byte di output; tutti i filtri del debugger abilitati |
| `IoGetCurrentIrpStackLocation` | Restituisce la posizione nello stack dell’IRP modellato attivo; le normali macro WDM compilate leggono lo stesso campo guest |
| `KeGetCurrentIrql` | L’IRQL corrente è `PASSIVE_LEVEL` per dispatch e lavoro, `DISPATCH_LEVEL` per i DPC |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | Elementi opachi del dispositivo; solo `DelayedWorkQueue`, dispositivo e contesto passati a `PASSIVE_LEVEL`; vietato liberare elementi ancora in coda |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | DPC opaco, quattro argomenti guest, `DISPATCH_LEVEL`, duplicati/rimozione e importanza; solo destinazione CPU0 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | Timer di notifica/sincronizzazione; scadenze relative/assolute in 100 ns, periodi in millisecondi, riarmo/annullamento e segnali nel tempo virtuale |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | Eventi di notifica/sincronizzazione con consumo distinto; `KeSetEvent` accetta solo Increment=0 e Wait=FALSE |
| `KeWaitForSingleObject` | Un evento o timer inizializzato; `KernelMode` non alertable, motivo `Executive`; polling zero, attesa finita relativa/assoluta o infinita; attesa non nulla/infinita richiede IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | Ritardo relativo/assoluto `KernelMode` non alertable con IRQL <= APC_LEVEL; riprende il frame guest dopo l’avanzamento del tempo virtuale |
| `IoMarkIrpPending` | Marca l’IRP attivo; è modellata anche la scrittura equivalente della macro WDM nel controllo dello stack; il dispatch deve restituire `STATUS_PENDING` |
| `IofCompleteRequest`, `IoCompleteRequest` | Completa l’IRP modellato attivo, sincrono o pendente con `IO_NO_INCREMENT`; non si può accedere nuovamente a un IRP completato o al suo buffer |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Operazioni limitate sui buffer guest, al massimo 1 MiB per chiamata; le API di copia senza sovrapposizione rifiutano le sovrapposizioni |

I limiti IRQL provengono da `KernelAPIIRQL.def`; il modello responsabile verifica le restrizioni dipendenti dagli argomenti. Un DPC non può chiamare il registro né allocare, liberare o accedere al pool paginato. Le conversioni Unicode di `DbgPrint` richiedono `PASSIVE_LEVEL`; output ANSI e operazioni non paginate supportate restano utilizzabili a `DISPATCH_LEVEL`. Gli stack hanno limiti: un puntatore fuori intervallo non può entrare nello stack di un altro worker bloccato. I timer armati nell’estensione impediscono il rilascio prematuro del dispositivo. Ciò non espone cambi generali di IRQL.

La scadenza soddisfa le attese registrate prima che un DPC reimposti o riarmi il timer. I DPC accodati precedono la ripresa dei frame `PASSIVE_LEVEL` risvegliati. Se la memoria della richiesta contiene ancora un DPC accodato, il completamento IRP ne rifiuta il rilascio prima di completare e invalidare il buffer.

La formattazione di `DbgPrint` supporta interi `d/i/u/o/x/X`, puntatori `p`,
testo `s/c`, `%%`, Unicode a lunghezza esplicita `wZ/lZ`, stringhe wide `ls/ws`,
flag, larghezza/precisione incluso `*` e modificatori Windows della lunghezza
degli interi. Vengono letti al massimo 32 argomenti variabili e 1024 byte di
formato. Larghezza e precisione sono limitate a 512. Virgola mobile, `%n`,
combinazioni sconosciute e conversioni di testo non ASCII arrestano esplicitamente
l’esecuzione; il modello non ipotizza una code page Windows e non invoca il
printf dell’host con dati guest.

La struttura RegistryPath originale e il suo buffer scadono al ritorno di
DriverEntry. I driver che necessitano della stringa in seguito devono copiarla
durante l’inizializzazione.

L’arena degli oggetti/pool è di 1 MiB. I byte di pool non inizializzati hanno
contenuto deterministico `0xCD`; quelli liberati contengono `0xDD`. Si tratta
di uno scenario di esecuzione concreto. Gli accessi CPU e le API modellate sui
buffer rifiutano allocazioni di pool liberate, dispositivi eliminati, byte
non allocati nell’arena, campi opachi degli oggetti e scritture nei campi degli
oggetti di sola lettura. Questi controlli coprono la durata degli oggetti del
modello; non costituiscono un’analisi generale della sicurezza della memoria
dei driver. Le voci non scritte della tabella di dispatch riportano zero come
«non registrato». Una richiesta dello scenario per una major function non
registrata viene completata dal gestore predefinito modellato con
`STATUS_INVALID_DEVICE_REQUEST`; l’errore rimane visibile negli stati sia di
dispatch sia di I/O. Il modello non inventa un indirizzo di funzione guest per
tale gestore e le letture guest di una voce non scritta restano non supportate.
Registrare esplicitamente un callback nullo è un errore.

## Scenari di richieste

Passare un file JSON con `--scenario` per selezionare le richieste e l’eventuale
scaricamento:

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --scenario scenario.json > driver-report.json
```

Per un driver che crea `\Device\NeverDIO` e accetta l’IOCTL con buffer
`0x222000`, un esempio di `scenario.json` è:

```json
{
  "requests": [
    {"kind": "create", "device": "\\Device\\NeverDIO"},
    {"kind": "ioctl", "code": "0x222000", "input": "00112233", "output_size": 4},
    {"kind": "cleanup"},
    {"kind": "close"}
  ],
  "unload": true
}
```

Il nome del dispositivo e il codice IOCTL devono corrispondere al driver.
In create, omettere `device` seleziona l’unico dispositivo attivo; una selezione
ambigua fallisce. Le richieste successive usano il dispositivo del proprio file,
a meno che non sia specificato un nome esplicito corrispondente. Il campo
facoltativo `file` è un’identità di scenario senza segno a 32 bit, pari a zero
per impostazione predefinita. Ogni identità possiede FILE_OBJECT e FsContext
propri e richiede create, trasferimenti, cleanup e close in quest’ordine.
Le richieste di file indipendenti possono essere intercalate. I dispositivi
esclusivi rifiutano una seconda apertura. Queste identità rappresentano oggetti
file, non handle duplicati. Sono supportati il metodo IOCTL con buffer ed
entrambi i metodi diretti. Il dispatch deve completare in modo sincrono o rispettare il contratto di completamento pendente tramite callback sopra descritto. Lunghezze di output non valide e accessi a IRP completati causano errori espliciti. L’unload richiesto
non deve lasciare dispositivi, link simbolici, allocazioni di pool o oggetti
file attivi.

Il campo radice opzionale `"load_address": "0x190000000"` richiede un cambio di
base; l’omissione o `"0x0"` usa l’indirizzo preferito. L’immagine deve soddisfare
i requisiti di rilocazione. Il comando di inizializzazione e l’API C originali
non implicano alcuno scenario.

Alla radice sono accettati solo `load_address`, `requests`, `unload`,
`kernel_exports` e `registry`. Tutte le richieste accettano `kind`, un `device` facoltativo e
un `file` facoltativo. Gli IOCTL richiedono `code` e accettano `input`,
`output_size` e `direct_input`. Un `read` accetta `output_size` e `byte_offset`;
un `write` accetta `input` e `byte_offset`. Gli offset sono zero per impostazione
predefinita, accettano interi o stringhe esadecimali e devono rientrare in un
valore con segno a 64 bit non negativo. Le richieste del ciclo di vita rifiutano
i campi di trasferimento. I campi sconosciuti o duplicati sono rifiutati.
`code` accetta un intero JSON senza segno a 32 bit o una stringa esadecimale `0x`.
`input` è una stringa esadecimale di byte di lunghezza pari senza prefisso né
spazi; l’omissione indica un input vuoto. `output_size` è un intero JSON senza
segno; l’omissione indica zero. Frazioni numeriche e notazioni in virgola mobile
sono rifiutate.

Per gli IOCTL diretti, `input` inizializza il primo buffer di sistema,
mentre `direct_input` inizializza il secondo buffer separato descritto dall’MDL,
riempito con zeri fino a `output_size`. `METHOD_IN_DIRECT` richiede accesso in
lettura; non implica un mapping di sistema di sola lettura. Entrambi i metodi
usano buffer di scenario leggibili/scrivibili. `MdlMappingNoWrite` rimuove
l’accesso in scrittura dal mapping e `MdlMappingNoExecute` rimuove l’accesso in
esecuzione. L’unmap revoca l’indirizzo virtuale di sistema; un nuovo mapping
conserva gli stessi dati bloccati. Il completamento invalida l’MDL e il mapping.
Sono modellati i campi pubblici dell’MDL usati dalle macro WDM; i campi
processo/PFN, gli MDL costruiti manualmente, i mapping utente e l’accesso diretto
tramite UserBuffer grezzo sono rifiutati. Un buffer diretto di lunghezza zero
ha un MDL nullo.

`IoAllocateMdl` alloca metadati autonomi per un buffer non vuoto, senza overflow e di massimo 1 MiB; non verifica né blocca il buffer. `Irp` deve essere NULL, mentre `SecondaryBuffer` e `ChargeQuota` devono essere FALSE. Se l’arena è esaurita restituisce NULL. `MmBuildMdlForNonPagedPool` richiede che l’intero intervallo appartenga a una sola allocazione attiva del pool non paginato. La macro sicura e le normali macro WDM riutilizzano l’indirizzo originale, conservando alias e permessi esistenti anche quando vengono aggiunti flag che vietano scrittura o esecuzione. Ulteriori mapping di sistema e l’annullamento del mapping sono rifiutati. `IoFreeMdl` invalida solo il descrittore; il buffer del pool ha una durata indipendente. Entrambi gli ordini di rilascio sono supportati purché la memoria liberata non venga poi usata. Tutti i campi MDL modellati sono di sola lettura; accesso a processo/PFN, catene e modifiche manuali restano non supportati. Lo scaricamento deve rilasciare tutti i descrittori del driver.

Per ogni metodo IOCTL con `output_size` diverso da zero, `Information` non deve superare `output_size`, anche quando il buffer di input è più grande. Senza un buffer di output, `Information` può contenere un risultato a 64 bit definito dal driver; non viene interpretato come numero di byte da copiare. Il report conserva il valore numerico e aggiunge `information_hex`, una stringa esadecimale esatta per i lettori JSON che non rappresentano tutti gli interi a 64 bit.

Per READ/WRITE, `DO_BUFFERED_IO` o `DO_DIRECT_IO` seleziona il metodo di
trasferimento. L’assenza di entrambi i flag o il loro conflitto arresta
l’esecuzione. Information viene verificato rispetto alla lunghezza di
trasferimento; le scritture restituiscono un conteggio e le letture restituiscono
byte.

`kernel_exports` associa i nomi delle routine a booleani di disponibilità
espliciti, ad esempio `"kernel_exports": {"OptionalRoutine": false}`. Gli export
modellati e gli import statici ricevono indirizzi stabili condivisi con
`MmGetSystemRoutineAddress`. Un export esplicitamente assente si risolve in NULL
e non può soddisfare un import statico. Un export dichiarato presente senza un
modello API si risolve in una trap attivata alla chiamata. Un nome dinamico
sconosciuto arresta l’esecuzione con una diagnostica di disponibilità non
specificata; l’assenza non viene mai dedotta dalla mancanza di implementazione.
I nomi sono ASCII stampabile di lunghezza limitata e la risoluzione distingue
le maiuscole. L’inventario è una proprietà concreta dello scenario, senza
pretendere di corrispondere a ogni versione di Windows.
`IoMarkIrpPending`, `IoGetCurrentIrpStackLocation` e `MmGetSystemAddressForMdlSafe` sono funzioni ausiliarie modellate degli header WDM; ciò non le dichiara esportate per impostazione predefinita, quindi la loro disponibilità come export richiede un import statico o una dichiarazione esplicita in `kernel_exports`.

Il testo dello scenario è limitato a 2 MiB, con al massimo 64 richieste,
65536 byte per buffer di input o output e 512 KiB di byte richiesti complessivi,
incluso il contenuto di `direct_input`. I budget di istruzioni, osservazioni,
memoria guest e tempo si applicano all’intero scenario. L’arena da 1 MiB ospita
anche oggetti e metadati, quindi un’immagine può esaurire la memoria del modello
prima di consumare la dimensione massima dei buffer dello scenario.

## Scenari del registro

L’array facoltativo `registry` definisce un albero di registro concreto, locale
alla sessione. Ogni chiave ha un `path` obbligatorio e un array `values`
facoltativo; ogni valore contiene `name`, un intero senza segno `type` e `data`
esadecimale. Un nome vuoto seleziona il valore predefinito. Un esempio di valore
DWORD è `{"name":"Mode","type":4,"data":"01000000"}`. I byte sono conservati
esattamente; il modello non corregge i terminatori delle stringhe né espande le
variabili d’ambiente.

I percorsi devono essere assoluti, in ASCII, sotto `\\Registry\\Machine` o
`\\Registry\\User`. Le chiavi antenate vengono create implicitamente. Le identità
di chiavi e valori vengono confrontate senza distinzione tra maiuscole e
minuscole secondo le regole ASCII; nomi non ASCII e identità duplicate vengono
rifiutati. Se `registry` è omesso, la disponibilità del registro resta indefinita
e le chiamate al registro arrestano l’esecuzione. `"registry": []` descrive
esplicitamente un namespace vuoto. Nessuna chiave, valore, dato del registro
host o configurazione del servizio viene dedotta dal driver.

`ZwOpenKey` e `ZwCreateKey` restituiscono handle opachi indipendenti, con
controlli di accesso per handle su interrogazione, scrittura, creazione di
sottochiavi ed eliminazione. L’albero configurato concede i bit supportati di
`KEY_ALL_ACCESS`, comprese le normali maschere `KEY_READ` e `KEY_WRITE`. È un
albero di prova esplicitamente accessibile, senza ACL Windows né valutazione
dei privilegi. Diritti generici, `MAXIMUM_ALLOWED`, viste alternative del
registro, descrittori di sicurezza personalizzati, classi e collegamenti
simbolici non sono supportati. La creazione relativa richiede un handle del
genitore diretto con `KEY_CREATE_SUB_KEY`. Le chiavi di input sono non volatili;
le nuove chiavi possono essere volatili, ma una sottochiave non volatile di una
chiave volatile viene rifiutata. Non è modellato il riavvio né la persistenza
su disco.

`ZwQueryValueKey` implementa le classi di informazioni Basic, Full, Partial e
le rispettive varianti Align64 definite, con lunghezze esatte, dati allineati,
output parziale e risultati distinti `STATUS_BUFFER_TOO_SMALL` e
`STATUS_BUFFER_OVERFLOW`. `ZwSetValueKey` e `ZwDeleteValueKey` modificano solo
l’albero della sessione. `ZwDeleteKey` rifiuta una chiave con sottochiavi attive;
gli handle di una chiave eliminata restituiscono `STATUS_KEY_DELETED` finché
non vengono chiusi. `ZwClose` rilascia un handle indipendentemente dalla
chiave; l’unload richiesto fallisce se restano handle del registro aperti.

I limiti sono 256 chiavi comprese le antenate, 1024 valori totali, 65536 byte
per valore, 512 KiB di dati complessivi, 1024 byte ASCII per percorso di chiave,
256 byte per nome di valore e 256 handle aperti contemporaneamente. Creazione
e modifiche applicano gli stessi limiti della verifica preliminare dello
scenario. Nel report, `configuration.registry` conserva l’input originale;
`registry` elenca i percorsi e i valori delle chiavi attive finali, comprese
le modifiche osservate prima di un arresto. Uno stato del registro non
specificato è riportato come null. Volatilità e identità degli handle non
fanno parte di questa istantanea dei valori.

## Verifica di accettazione con l’esempio Microsoft

Lo [script di validazione](../../scripts/validate_windows_driver_sample.py),
da eseguire esplicitamente, scarica il sorgente SIOCTL di Microsoft alla revisione
fissata nel [manifest di validazione](../../unittests/emulation/fixtures/sioctl-validation.json),
verifica gli hash SHA-256 e compila il sorgente non modificato con gli header
DDK di MinGW-w64. Conserva licenza e provenienza originali, comandi di compilazione,
scenario e report nella directory di output scelta. Richiede accesso alla rete,
Clang, `lld-link`, `nm` e gli header DDK di MinGW-w64:

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

Usare `--headers` per una directory di inclusione MinGW-w64 diversa da quella
predefinita. Lo script genera una libreria di import MS COFF dalle dipendenze
dell’oggetto compilato. La verifica esegue scenari con buffer, in-direct e
out-direct separati attraverso DriverEntry, create, IOCTL, cleanup, close e
unload. Aggiungere `--debug` e scegliere una directory di output separata per
compilare con `DBG=1` e verificare i messaggi di log guest. L’esempio upstream
non registra un gestore cleanup; il gestore predefinito modellato completa
quindi cleanup con `STATUS_INVALID_DEVICE_REQUEST` (`0xC0000010`). Il driver
esegue comunque close e unload e l’IOCTL riuscito restituisce i byte attesi.
Per questo scenario completo, il codice di uscita atteso della CLI è **2** e
`scenario_success` è false. Lo script stesso riesce solo quando tutti questi
risultati corrispondono, incluso l’errore visibile di cleanup; non riscrive
l’esempio per nascondere quel risultato.

## Verifica di accettazione con l’esempio Zero

Lo [script di verifica Zero](../../scripts/validate_zero_driver_sample.py)
compila l’esempio pubblico Zero WDM di Pavel Yosifovich senza modificarlo,
usando la revisione e gli hash del [manifesto](../../unittests/emulation/fixtures/zero-validation.json).
Eseguire `python3 scripts/validate_zero_driver_sample.py` con gli stessi
requisiti di toolchain. Sorgenti, licenza MIT, comandi, scenario e report vengono
conservati per impostazione predefinita in `build-release/driver-validation/zero`.
Nove richieste verificano letture dirette attraverso i confini di pagina,
conteggi di scrittura, statistiche atomiche nel guest e un IOCTL con buffer
per le statistiche. Il fallimento della lettura di lunghezza zero e l’assenza
del gestore CLEANUP nell’esempio restano visibili; il codice di uscita atteso
del CLI è 2, con close e unload completati correttamente. Lo script riesce
solo quando questi risultati e tutti i byte di output coincidono esattamente.

## Report e SDK

Il report JSON distingue `stop_reason`, i campi che ammettono null `nt_status`
e `nt_success`, il PC di arresto e il numero di istruzioni. Conserva le chiamate
API e lo stato osservabile raccolti prima dell’arresto, inclusi gli oggetti
dispositivo e gli indirizzi dei callback del driver. Gli indirizzi guest sono
stringhe esadecimali, così i consumatori JSON non perdono la precisione a 64 bit.
L’oggetto `configuration` registra i limiti, il nome del servizio e le
sostituzioni `kernel_exports` e l’input `registry` dell’esecuzione.
Il profilo è `wdm-x64-scheduled-v3`. `nt_status` rimane il risultato di DriverEntry,
mentre `scenario_success` descrive insieme l’inizializzazione e le richieste
completate. `phase`, `requests` e `unload_completed` identificano le parti
eseguite del ciclo di vita richiesto. Ogni chiamata API e scrittura CPU registra
anche la propria fase (`driver_entry`, `request:N`, `callback:N` o `unload`). Ogni richiesta
riporta gli stati di dispatch e I/O, il completamento, la lunghezza delle
informazioni e i byte restituiti in `output_hex`. `preferred_image_base` descrive
la base PE originale. `security_cookie` è l’indirizzo guest del cookie inizializzato,
oppure `"0x0"` se non era necessario. I campi delle richieste sono `kind`, `device`, `file`, `byte_offset`, `code`, `irp`, `completed`, `dispatch_status`, `io_status`,
`information`, `information_hex` e `output_hex`. Il campo numerico `information`
resta un intero JSON decimale esatto; `information_hex` conserva gli stessi bit
anche per i client che leggono i numeri JSON con precisione limitata a 53 bit.

Le osservazioni del lavoro usano la fase `callback:N`. Le richieste pendenti mantengono `STATUS_PENDING` in `dispatch_status`; lo stato finale è riportato separatamente in `io_status` e determina il contributo a `scenario_success`.

L’oggetto nullable `fault` conserva il primo fault del backend. I suoi campi
`kind`, `pc`, `address` nullable, `size`, `access` e `interrupt` distinguono
memoria non mappata o protetta, intervalli non validi, istruzioni non valide ed
eccezioni CPU. Gli indirizzi usano stringhe esadecimali; le dimensioni e i
vettori di interrupt usano interi. Le letture di osservazione non possono
sostituire il fault originale. Un backend in fault non può riprendere
l’esecuzione e questo record non implica una gestione SEH guest.

`instructions` conta i tentativi ammessi di istruzioni guest. Un’istruzione
rifiutata dalla politica di esecuzione non viene conteggiata; un’istruzione
ammessa che provoca un fault nella CPU viene conteggiata. Il dispatch sintetico
delle API e la sentinella di ritorno non incrementano questo contatore.

Ogni voce di `writes` ha `semantics: "attempted_guest_write"`: registra un
tentativo di scrittura CPU fuori dallo stack, anche se successivamente provoca
un fault o viene fermato da un budget. Non garantisce che la scrittura sia stata
completata e non include le scritture effettuate dai modelli API. Le istantanee
degli oggetti dispositivo e driver descrivono lo stato osservato all’arresto.

Includere `neverd/sdk/NeverDCAPIEmulation.h` (o l’header generale dell’API C),
creare una sessione e chiamare `neverd_emulate_driver_json(session, path, options)`.
Un percorso esplicito non vuoto entra direttamente nella verifica rigorosa
precedente all’esecuzione, senza prima caricare tramite l’API di analisi
generale. La CLI usa questo percorso. Passare `NULL` per options seleziona i
valori predefiniti. Le opzioni esplicite `neverd_driver_options_v1` richiedono
l’esatta `struct_size` e budget positivi di istruzioni, memoria, eventi e tempo.
Liberare il risultato con `neverd_free_string`.

Passare `NULL` come percorso richiede invece una sessione caricata e analizza
nuovamente il suo file indipendentemente dall’analisi IR e dal caricamento
limitato a determinate funzioni. Entrambi i percorsi preservano l’immagine della
sessione. Mantenere il file di input disponibile e invariato per tutta la durata
della chiamata. Gli errori di richiesta/preparazione restituiscono `NULL` e
impostano `neverd_last_error`; gli arresti dell’esecuzione restituiscono JSON.
L’API rimane disponibile nelle build con la funzionalità disabilitata e indica
come abilitarla.

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)`
usa le stesse opzioni v1 e regole di proprietà, aggiungendo un input di scenario
rigoroso. È richiesta una stringa JSON non NULL terminata da NUL. L’ABI originale
`neverd_emulate_driver_json` rimane invariata e limitata all’inizializzazione.
Il parser C++ `driverOptionsFromScenarioJSON` fornisce la stessa validazione
dello scenario a chi chiama `emulateDriver`.

Il punto di ingresso C++ interno è `neverd::emulation::emulateDriver` in
`include/neverd/emulation/DriverSession.h`. L’analisi del formato appartiene al
loader esistente; il comportamento degli oggetti/API Windows appartiene a
`lib/emulation/windows`; lo stato CPU e l’esecuzione appartengono all’adattatore
Unicorn. L’adattatore e il modello usano la stessa interfaccia di memoria guest.
Nessun comportamento di API Windows appartiene al fork di Unicorn.
