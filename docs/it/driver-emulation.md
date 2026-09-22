**Lingue**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← Indice della documentazione](README.md)

# Emulazione dei driver Windows

L’emulatore opzionale di driver di NeverD esegue il punto di ingresso PE di un
driver WDM x64 supportato e, facoltativamente, percorre uno scenario esplicito
di richieste sincrone prima di scaricarlo. Usa Unicorn per l’esecuzione della
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
originali autonome e il percorso con buffer dell’esempio WDM SIOCTL di Microsoft.
Non dimostrano la compatibilità con driver arbitrari di terze parti.

| Classe di driver o requisito | Ambito attuale | Ambiente mancante |
|-----------------------------|----------------|-------------------|
| Driver WDM software x64 che usa le API elencate | Inizializzazione limitata e un ciclo di vita sincrono di file | Ogni ulteriore API eseguita deve avere un modello definito |
| IOCTL `METHOD_BUFFERED` | Supportato nello scenario esplicito di richieste | Non sono disponibili più file aperti o il completamento asincrono |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT`, `METHOD_NEITHER` | Rifiutati | MDL, pagine bloccate, verifica degli accessi e durata dei buffer utente |
| Driver KMDF / UMDF | Non supportato | Binding al framework, oggetti, code, callback e runtime host appropriato |
| Driver PnP di bus, funzione o filtro | L’inizializzazione può essere eseguita nel sottoinsieme di API; il ciclo di vita dello stack di dispositivi non è supportato | Collegamento dei dispositivi, dispatch al driver sottostante, IRP PnP e di alimentazione |
| Driver di archiviazione, rete, visualizzazione, file system e minifilter | Contratti dei sottosistemi non supportati | Framework port/class/miniport, NDIS/WFP, servizi grafici o del file system |
| Driver con thread di lavoro, timer, DPC, APC, attese o annullamento | Non supportato | Scheduling, transizioni IRQL, sincronizzazione e responsabilità asincrona delle risorse |
| Driver con callback di processo/thread, handle, operazioni su registro/file o ricerca di moduli kernel | Non supportato al di fuori delle API elencate | Gestore degli oggetti, stato del sistema e produttori di callback/eventi |
| Driver hardware, DMA, PCI, di interrupt o di virtualizzazione | Ambiente non supportato | Modelli di dispositivi, memoria fisica, bus, interrupt e stato CPU privilegiato |
| Driver Windows x86 o ARM64 | Rifiutato | Caricamento, ABI e modello di esecuzione specifici dell’architettura |
| Immagine x64 che richiede CFG, configurazione di caricamento non supportata, TLS o altre funzionalità PE rifiutate | Rifiutata al caricamento | Semantica esplicita del loader e del runtime per tali requisiti |

Un’importazione non supportata ma inutilizzata può restare collegata. Raggiungere
un’operazione non supportata interrompe l’esecuzione con una diagnosi e le
osservazioni raccolte fino a quel momento. Il solo successo di DriverEntry non
dimostra il supporto dei successivi percorsi di dispatch, hardware o framework.
La tabella delle API seguente definisce il sottoinsieme supportato di riferimento.

## Contratto di esecuzione

Il profilo modella un unico ciclo di vita WDM x64 a thread singolo a
`PASSIVE_LEVEL`. L’esecuzione inizia dal punto di ingresso PE, mantenendo il
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

Le importazioni sconosciute vengono collegate a trap attivate solo all’uso.
Un’importazione inutilizzata non impedisce l’esecuzione; eseguire il suo thunk
o leggere un valore di dati esportato non modellato arresta l’esecuzione con
`unsupported_api`. Anche gli effetti non supportati dell’ambiente CPU provocano
un arresto esplicito. NeverD non sostituisce le chiamate non implementate con
valori di successo. Le immagini malformate o i requisiti di caricamento non
supportati falliscono prima dell’esecuzione.

Questo profilo non implementa un kernel Windows completo, un runtime KMDF,
il ciclo di vita PnP/alimentazione, IRP asincroni o pending, IOCTL direct/neither,
interrupt o scheduling multithread. I callback vengono eseguiti solo se
richiesti esplicitamente dallo scenario; la sola inizializzazione continua a
fermarsi dopo DriverEntry.

Le immagini usano la base preferita, salvo che uno scenario scelga un indirizzo
di rilocazione valido, e devono essere eseguibili PE32+ x64 con sottosistema
nativo. Le importazioni possono provenire da `ntoskrnl.exe` o `ntkrnlmp.exe`.
Il loader di esecuzione supporta rilocazioni di base x64 `DIR64` validate e
una configurazione di caricamento limitata per il cookie di sicurezza,
inizializzato prima del wrapper di ingresso con un cookie guest deterministico.
CFG e altri campi di configurazione di caricamento non modellati, TLS,
importazioni ritardate/bound, importazioni per ordinale e immagini gestite
vengono rifiutati. Le immagini devono anche superare controlli rigorosi su
intervalli e allineamento.

Il modello API iniziale ha intenzionalmente un contratto limitato:

| API | Comportamento modellato e restrizioni |
|-----|--------------------------------------|
| `RtlInitUnicodeString` | Costruisce una `UNICODE_STRING` guest per una sorgente limitata terminata da NUL |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Allocazioni di dati per i tipi di pool `0`, `1` e `512`; dimensione/tag positivi, tag corrispondenti nelle liberazioni con tag, nessun riutilizzo degli indirizzi |
| `IoCreateDevice`, `IoDeleteDevice` | Tipo di dispositivo `0x22`, caratteristiche `0` o `0x100`, estensioni limitate, nomi ASCII `\Device\Name` |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` o `\??\Name` in un unico namespace di sessione, con destinazione `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Testo letterale ASCII e `%%`, al massimo 512 byte di output; la formattazione variadica arresta l’esecuzione; tutti i filtri del debugger sono abilitati |
| `IoGetCurrentIrpStackLocation` | Restituisce la posizione nello stack dell’IRP modellato attivo; le normali macro WDM compilate leggono lo stesso campo guest |
| `KeGetCurrentIrql` | Restituisce `PASSIVE_LEVEL` |
| `IofCompleteRequest`, `IoCompleteRequest` | Completa l’IRP sincrono modellato attivo con `IO_NO_INCREMENT`; non si può accedere nuovamente a un IRP completato o al suo buffer |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Operazioni limitate sui buffer guest, al massimo 1 MiB per chiamata; le API di copia senza sovrapposizione rifiutano le sovrapposizioni |

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
Omettere `device` seleziona l’unico dispositivo attivo; una selezione ambigua
fallisce. Il modello segue un solo file aperto e richiede create, IOCTL, cleanup
e close in quest’ordine. Sono supportati solo gli IOCTL `METHOD_BUFFERED`.
Il dispatch deve completare ogni IRP in modo sincrono; restituire `STATUS_PENDING`,
non completare un IRP, fornire lunghezze di output non valide o accedere a un
IRP già completato causa un errore esplicito. Lo scaricamento richiesto non
deve lasciare dispositivi, collegamenti simbolici, allocazioni di pool o oggetti
file attivi.

Il campo radice opzionale `"load_address": "0x190000000"` richiede un cambio di
base; l’omissione o `"0x0"` usa l’indirizzo preferito. L’immagine deve soddisfare
i requisiti di rilocazione. Il comando di inizializzazione e l’API C originali
non implicano alcuno scenario.

Alla radice sono accettati solo `load_address`, `requests` e `unload`. I campi
di richiesta sono `kind`, l’opzionale `device` e, solo per `ioctl`, l’obbligatorio
`code` insieme agli opzionali `input` e `output_size`. I campi sconosciuti o
duplicati vengono rifiutati. `code` accetta un intero JSON senza segno a 32 bit
o una stringa esadecimale con prefisso `0x`. `input` è una stringa di byte
esadecimali di lunghezza pari, senza prefisso o spazi; ometterla significa input
vuoto. `output_size` è un intero JSON senza segno; ometterlo significa zero.
Le frazioni numeriche e le notazioni in virgola mobile vengono rifiutate.

Il testo dello scenario è limitato a 2 MiB, con al massimo 64 richieste, 65536
byte per buffer di input o output e 512 KiB complessivi di byte di input e
output. I budget di istruzioni, osservazioni, memoria guest e tempo si applicano
all’intero scenario. L’arena da 1 MiB contiene anche oggetti e metadati: un’immagine
può quindi esaurire la memoria del modello prima di consumare il massimo dei
buffer dello scenario.

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

Usare `--headers` per una directory include di MinGW-w64 diversa da quella
predefinita. Lo script genera una libreria di importazione MS COFF dalle
dipendenze dell’oggetto compilato. La verifica esegue DriverEntry, create,
l’IOCTL con buffer dell’esempio, cleanup, close e unload. L’esempio originale
non registra un gestore cleanup; il gestore predefinito modellato completa
quindi cleanup con `STATUS_INVALID_DEVICE_REQUEST` (`0xC0000010`). Il driver
viene comunque chiuso e scaricato, e l’IOCTL riuscito restituisce i byte attesi.
Per questo scenario completo, il codice di uscita atteso della CLI è **2** e
`scenario_success` è false. Lo script stesso riesce solo quando tutti questi
risultati coincidono, incluso l’errore visibile di cleanup; non riscrive l’esempio
per nascondere tale risultato.

## Report e SDK

Il report JSON distingue `stop_reason`, i campi che ammettono null `nt_status`
e `nt_success`, il PC di arresto e il numero di istruzioni. Conserva le chiamate
API e lo stato osservabile raccolti prima dell’arresto, inclusi gli oggetti
dispositivo e gli indirizzi dei callback del driver. Gli indirizzi guest sono
stringhe esadecimali, così i consumatori JSON non perdono la precisione a 64 bit.
L’oggetto `configuration` registra i limiti e il nome del servizio dell’esecuzione.
Il profilo è `wdm-x64-synchronous-v1`. `nt_status` rimane il risultato di DriverEntry,
mentre `scenario_success` descrive insieme l’inizializzazione e le richieste
completate. `phase`, `requests` e `unload_completed` identificano le parti
eseguite del ciclo di vita richiesto. Ogni chiamata API e scrittura CPU registra
anche la propria fase (`driver_entry`, `request:N` o `unload`). Ogni richiesta
riporta gli stati di dispatch e I/O, il completamento, la lunghezza delle
informazioni e i byte restituiti in `output_hex`. `preferred_image_base` descrive
la base PE originale. `security_cookie` è l’indirizzo guest del cookie inizializzato,
oppure `"0x0"` se non era necessario. I campi delle richieste sono `kind`,
`device`, `code`, `irp`, `completed`, `dispatch_status`, `io_status`,
`information` e `output_hex`.

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
