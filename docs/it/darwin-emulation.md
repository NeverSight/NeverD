**Lingue**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: c5503091e713c63aa7b08a8cbd5fab1b5cc3c8d8950bb7da4fe754dbe5787137 -->

[← Indice della documentazione](README.md)

# Ambienti di processo guest macOS e iOS

`lib/emulation/os/darwin/` modella processi Mach-O autonomi con limiti espliciti, separati dal trasporto CPU host. Attivare `NEVERD_ENABLE_CPU_EMULATION`; l’emulazione dei driver Windows non è necessaria. `macos/` e `ios/` definiscono profili di piattaforma espliciti.

| Profilo | Piattaforma Mach-O | ISA guest | Pagina OS |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, ARM64 di base | 4 KiB x64; 16 KiB ARM64 |
| `ios-macho64-v1` | dispositivo iOS | ARM64 di base | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, ARM64 di base | 4 KiB x64; 16 KiB ARM64 |

Un binario per dispositivo non è un’immagine simulatore; il profilo guest non viene dedotto dall’host. Con ISA coincidenti macOS può usare [HVF](macos-hvf.md), altrimenti `auto` usa Unicorn. La granularità CPU rimane 4 KiB. Le [API C, Python e CLI](process-emulation.md) condividono opzioni, limiti e rapporti.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## Immagine e avvio

`MachOExecutionImage` mantiene i byte originali senza patch di rilocazione dell’analisi. Sono ammesse soltanto immagini thin little-endian `MH_EXECUTE` con piattaforma e ingresso univoci. Un’immagine universale richiede l’estrazione esplicita della slice desiderata.

L’intero file, inclusi metadati e byte finali, deve rispettare `memory_limit` prima dell’analisi o copia. Il loader legge uno snapshot privato e limitato di un file regolare; rifiuta percorsi con NUL, letture incomplete e variazioni di dimensione. Non mantiene un mapping vivo del file. File e memoria guest hanno tetti separati dello stesso valore; l’I/O host non ha una garanzia temporale rigida.

I segmenti mantengono permessi correnti/massimi e riempimento a zero. `__PAGEZERO` riserva indirizzi senza allocarne l’intera estensione. Sono verificati intervalli file/VM, allineamenti OS, sovrapposizioni arrotondate, proprietà dell’header, ingresso eseguibile e budget. Il segmento dell’header deve essere leggibile ed eseguibile; pagine di guardia e ingresso privato di ritorno restano riservati. L’ultima pagina del file conserva byte fino al confine pagina o EOF; le pagine VM complete successive sono azzerate secondo il [loader XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).

`LC_MAIN` riceve `argc`, `argv`, `envp` e il vettore apple come quattro argomenti interi; il ritorno produce gli otto bit bassi dello stato d’uscita. `/usr/lib/dyld` è ammesso solo per questo passaggio d’ingresso senza import, senza eseguire dyld host. Un `stacksize` diverso da zero è rifiutato: il budget appartiene all’opzione `stack_size` del chiamante.

`LC_UNIXTHREAD` richiede un solo record completo dei registri generali nativi a 64 bit, con il solo PC valorizzato. Lo stack contiene argc, argv/envp terminati e un vettore apple terminato con `executable_path=<input filename>`. SP/flag personalizzati, altri registri, flavor aggiuntivi e ingressi conflittuali sono rifiutati. Non sono ereditati ambiente host o vettore ausiliario Linux. Riferimento: [architettura dyld](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

Dylib esterne, import, rebases/chained fixups, costruttori/distruttori, sezioni TLS, arm64e/PAC, sottotipi CPU non supportati, payload cifrati e comandi non modellati falliscono prima dell’esecuzione. PIE senza fixup usa gli indirizzi preferiti, senza ASLR. Le firme sono metadati, non un modello AMFI o una politica di entitlements.

## Servizi Darwin

Le chiamate BSD su ARM64 usano X16, X0–X5 e `svc #0x80`; x64 usa la classe BSD `0x02000000`, RAX e RDI/RSI/RDX/R10/R8/R9. Il successo azzera carry; l’errore lo imposta e restituisce errno positivo. ARM64 azzera X1; x64 azzera RDX al successo e lo preserva in errore. Le modifiche ai registri di SYSCALL sono esplicite. Il rapporto usa `result` e `error=true` per errori BSD; richieste senza ritorno o non supportate usano `result: null` in JSON e omettono `error`. Le regole seguono XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) e [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c), senza incorporare codice Apple.

Servizi: `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `getgroups`, `mmap`, `mprotect`, `munmap`. PID vale1000 e PPID1; UID/GID sono1000 per default o gli ID reali/effettivi distinti dichiarati sotto. I descrittori 1 e 2 catturano byte, inclusi NUL e non UTF8; quelli chiusi o di sola lettura restituiscono EBADF. Una copia parziale conserva i byte già letti ma il guasto successivo rimane EFAULT. Una lunghezza oltre `INT_MAX` produce EINVAL prima di controllare descrittore, puntatore o budget: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

La memoria supporta mapping privati anonimi di dati con `flags=0x1002`, descrittore -1 e offset zero. Lunghezze e suggerimenti non fissi sono arrotondati verso l’alto alla pagina OS. Un suggerimento occupato cerca prima verso indirizzi superiori, poi torna al posizionamento predefinito. Il mmap storico grezzo con lunghezza zero restituisce zero senza allocare; `MAP_UNIX03` è supportato e rifiuta lunghezza zero con EINVAL. Unmap/protect richiedono indirizzi allineati. NONE/READ/WRITE sono supportati e WRITE implica READ. Ogni pagina OS possiede la sua memoria fisica: un unmap parziale libera budget e nuove pagine sono azzerate. Un protect attraverso un buco o oltre i diritti massimi lascia invariato l’intero intervallo. Fonte: [servizi VM XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Mapping condivisi/fissi/JIT o anonimi eseguibili, altri trap Mach, syscall indirette, thread, segnali, file host/rete, dyld, runtime Objective-C/Swift e Foundation/UIKit sono esclusi e arrestano esplicitamente l’esecuzione. Questo non è un intero OS Apple né l’applicazione iOS Simulator.

## Verifica

Le fixture C originali sono generate con Clang e `ld64.lld`, senza SDK Apple o binari proprietari. Coprono cinque combinazioni piattaforma/ISA, record Mach-O malformati, pagine 4/16 KiB e rilascio parziale a budget pieno. `NeverDProcessPublicTests` confronta C API/CLI; `NEVERD_TEST_LIBNEVERD` e `NEVERD_TEST_DARWIN_FIXTURES` abilitano le stesse cinque combinazioni in Python.

## File e descrittori espliciti

`darwin_files` offre ai tre profili un catalogo chiuso di file inizialmente di sola lettura. Il campo obbligatorio `files` contiene `path` guest assoluti canonici e `bytes_hex` esadecimali. `stdin_hex` opzionale fornisce input finito: l’assenza significa sconosciuto e arresta letture non vuote, una stringa vuota indica EOF. Senza catalogo open si arresta; un catalogo esplicitamente vuoto restituisce ENOENT. Non si consultano file o input host.

Si aggiungono `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl` e gli ingressi nocancel di read/write/open/close/fcntl/pread. Sono supportati O_RDONLY/O_CLOEXEC e F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL. Aperture separate hanno posizioni indipendenti; dup condivide la posizione ma mantiene flag close-on-exec distinti. pread non cambia posizione. Chiudere o sostituire 0/1/2 modifica l’I/O successivo; gli output duplicati conservano destinazione e budget.

Limiti: 256 file, 16 MiB complessivi per percorsi/NUL/file/input, percorsi sotto 1024 byte e componenti fino a 255. `descriptor_limit` è un limite esclusivo 3–4096, predefinito 256; JSON resta limitato a 64 KiB. Opzioni invalide falliscono prima del caricamento. read oltre INT_MAX restituisce EINVAL prima del controllo FD; EOF non tocca la destinazione e un indirizzo invalido dà EFAULT. Buffer parzialmente scrivibili arrestano prima di copia o avanzamento. Gli errori SET/CUR/END conservano la posizione. Stat precedente e altri fcntl restano esclusi. Un file come antenato dà ENOTDIR. Lo stesso oggetto viene confrontato con macOS nativo; C/CLI/Python coprono cinque combinazioni guest, non dispositivi iOS.

Verifica Release del 2026-10-05: 381 registrazioni, 177 passate, 204 saltate, zero errori e 51/51 casi ARM64 HVF obbligatori eseguiti. Passano anche sette programmi macOS nativi, 35 test pubblici C/CLI/report, cinque combinazioni Python e 66 test del verificatore. I conteggi si sovrappongono. Mancano prove native Intel HVF/KVM/WHP per i nuovi servizi; Intel HVF resta non validato e le sue Actions sospese. Mancano SDK iOS e confronto su dispositivo.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Modificare file esistenti

Il booleano rigoroso `"writable":true` oppure `DarwinFileOptions::WritableFiles` autorizza modifiche locali al processo. Assente/false mantiene sola lettura; un’autorizzazione ignota arresta il servizio. Host e dati iniziali restano invariati. write(4/397), pwrite(154/415), truncate(200), ftruncate(201) e O_TRUNC condividono contenuti; open mantiene posizioni indipendenti, dup condivide posizione e stato, e i contenuti sopravvivono all’ultimo close. L’estensione aggiunge zeri; il troncamento conserva le posizioni, anche O_RDONLY|O_TRUNC.

F_SETFL cambia solo O_APPEND|O_NONBLOCK dopo conversione nativa e conserva accesso, close-on-exec e FWASWRITTEN. F_GETFL espone 0x10000 dopo byte effettivamente trasferiti, inclusi pwrite e output catturato. pwrite ignora append e conserva la posizione. INT_MAX viene controllato prima del FD; pwrite a -1 restituisce EINVAL ancora prima. INT64_MAX dà EFBIG prima del caso vuoto; la lunghezza viene ridotta prima di scegliere EOF.

ftruncate riuscito, anche a parità di dimensione, imposta FWASWRITTEN sulla descrizione chiamata e sui suoi dup. O_TRUNC lo imposta sulla nuova descrizione, anche O_RDONLY; truncate per percorso non cambia quelle esistenti.

Input parzialmente leggibile si arresta prima degli effetti. EFAULT completo conserva i byte, ma append non vuoto sposta la posizione a EOF. Errori del trasporto non confermano contenuti o posizione. Senza `mutation_policy`, scritture non vuote, troncamenti ed EFAULT completi non vuoti invalidano l’intera osservazione stat; le query successive si arrestano prima della copia. Una scrittura vuota la conserva. I 16 MiB contano percorsi/NUL, input, record, CWD, contenuti attuali e riferimenti ai percorsi scrivibili. Ridurre sostituisce il backing e libera capacità; input originale e un buffer di sostituzione limitato sono aggiuntivi. Alias inode noti e flag immutable/append-only sono rifiutati.

DarwinMemory trattiene lease fino all’ultimo unmap, inclusi PROT_NONE e FD chiusi; le modifiche si arrestano finché esistono. Errori e vecchi mmap di lunghezza zero non trattengono lease. Le nuove mappe vedono i byte attuali. O_WRONLY con READ/WRITE dà EACCES; PROT_NONE può acquisire lettura/scrittura tramite mprotect.

Programmi originali normali/nocancel confrontano il kernel nativo; test 4K/16K e C/CLI/Python coprono cinque combinazioni. Controllo dei permessi, rimozione di directory, rinomina tra domini di directory iniziali distinti, hard link, metadati del file system nativo, coerenza delle mappe e SIGBUS EOF restano incompleti. Ambiente completo, iOS fisico e Intel HVF non sono validati; Actions Intel resta sospeso.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Metadati mutabili espliciti

Un file può aggiungere mutation_policy accanto a `writable: true` e metadata completi; C++ usa `DarwinFileOptions::MutationPolicies`. È un contratto virtuale esplicito di allocazione sparsa, senza dedurre APFS o leggere l’orologio host. In sua assenza i metadati dopo modifica restano ignoti.

allocation_unit, mutation_time e seconds/nanoseconds sono obbligatori e seguono le regole intere senza perdita. L’unità è una potenza di due fra512 byte e16 MiB, indipendente da block_size e pagine VM. Servono permessi ordinari senza set-id/sticky, flags=0, link_count=1 e allocazione iniziale densa: blocks=ceil(size/allocation_unit)*(allocation_unit/512). Gli zeri non implicano buchi. Il riferimento al percorso conta nei16 MiB logici; il registro di allocazione non inventa ENOSPC.

La scrittura alloca ogni unità toccata, anche zeri nei buchi. truncate in crescita aggiunge zeri senza allocare; riducendo scarta le unità oltre EOF arrotondato in alto e conserva l’ultima parziale. Ricrescere non recupera le allocazioni scartate. Scritture non vuote riuscite e ogni truncate riuscito, anche di uguale dimensione o O_TRUNC vuoto, aggiornano size/blocks e impostano mtime/ctime al tempo fisso. Altri campi e input restano uguali; read non avanza atime. Stat per percorso, open indipendenti, dup e riapertura condividono il nodo.

Scritture vuote, rifiuti di budget/mappe, input parziale rifiutato ed errori backend conservano lo stato. EFAULT completo non vuoto lo rende ignoto; successi successivi non lo ricostruiscono. La copia stat fallita non cambia il nodo. virtual-file-metadata verifica144 byte su cinque profili e C/CLI/Python: test della politica, non equivalenza APFS. I programmi nativi verificano separatamente flag, posizioni ed errori. Namespace, coerenza nativa, Mach e caricamento dinamico restano incompleti.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```


## Posizionamento nei file sparsi

Con mutation_policy e allocazione nota, lseek accetta SEEK_HOLE=3 e SEEK_DATA=4 sui file regolari usando lo stesso registro di stat. L’input iniziale è denso anche nei byte zero. In un’unità del tipo richiesto restituisce l’offset dato, altrimenti l’inizio della successiva unità corrispondente. Il buco terminale parte da EOF. Valori negativi danno EINVAL; a/oltre EOF, anche nel file vuoto, o senza dati successivi danno ENXIO=6. Gli errori conservano il cursore; il successo cambia solo la descrizione e i suoi dup. Altri open hanno cursori indipendenti e riaprire vede l’allocazione attuale. Metadati, flag e byte non cambiano; i bit alti di whence sono ignorati.

Senza politica, per directory o dopo EFAULT completo con allocazione ignota il servizio resta escluso. Zeri e modifiche rifiutate non implicano allocazione. sparse-file-seek confronta errori, byte scritti, EOF e vita delle descrizioni native/guest senza assumere precedenti confini del FS. virtual-file-metadata controlla separatamente la geometria esatta della politica; C/CLI/Python coprono cinque profili.

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Rimozione dei nomi di file regolari

`mutable:true` per directory (C++ `MutableDirectories`) autorizza i cambiamenti dei nomi immediati, indipendentemente da `writable`. Senza autorizzazione il modello si ferma. Rifiuta flags noti non nulli, permessi speciali del padre, link_count≠1 del figlio e alias noti padre/figlio; unisce stat e inode degli snapshot, distinguendo dispositivi esplicitamente diversi. I percorsi consumano il budget esistente.

`unlink(10)` / `unlinkat(472)` rimuovono nomi regolari esistenti. la rimozione dei file accetta solo i32 bit bassi 0 o `0x800`; bit sconosciuti danno EINVAL prima di percorso/FD, AT_REMOVEDIR usa il contratto limitato sotto; DATALESS e SYSTEM_DISCARDED restano esclusi. Risoluzione comune: ENOENT, ENOTDIR per file seguito da `/`, EPERM per directory ordinaria, EISDIR per radice composta solo da barre, EBUSY con `.`/`..` finali. I suffissi `.`/`..` sono verificati nativamente.

FD/dup/aperture indipendenti esistenti mantengono dati, cursori e flags; F_GETPATH conserva il vecchio percorso acquisito. Le nuove aperture falliscono, genitori impliciti e CWD restano. Il permesso di scrittura appartiene all’oggetto; close/dup2/modifica successiva recuperano i byte correnti solo dopo l’ultimo descrittore e mapping. I costi iniziali dei percorsi restano; controllo dei permessi, rinomina tra domini di directory iniziali distinti e hard link sono ancora incompleti; le directory iniziali usano l’autorizzazione esplicita descritta sotto.

stat/readdir/SEEK_END del padre diventano sconosciuti per ogni FD/percorso e si fermano prima di copia/cursore. read/pread restano EISDIR; SET/CUR/F_GETPATH/fchdir/risoluzione relativa continuano. La politica nota imposta nlink=0 e ctime fisso; scritture successive non ripristinano nlink=1. Senza politica/dopo EFAULT i metadati restano ignoti. Errori preservano lo stato. `unlinked-file` confronta regole native nome/FD; tempi e invalidazione sono regole esplicite del modello.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Creazione di file regolari

O_CREAT=0x200 crea un file vuoto nel genitore diretto esplicitamente mutable, tramite open/openat normale o nocancel. Il nuovo oggetto è scrivibile; quelli esistenti mantengono WritableFiles. Un FD di sola lettura può creare ma non scrivere. Senza politica di creazione esplicita, stat64 e ricerca sparsa restano sconosciuti. metadata/mutation_policy del vecchio omonimo non vengono mai ereditati.

O_EXCL=0x800 con O_CREAT restituisce EEXIST per file/directory esistenti prima del troncamento; da solo non ha effetto. O_CREAT di sola lettura apre directory esistenti. Dopo il controllo del primo byte e della directory in openat descritto sotto, l’ordine è: accesso invalido, disponibilità FD, EINVAL per O_CREAT|O_DIRECTORY, percorso. Si crea solo l’ultimo componente originale mancante; antenati assenti e suffissi `/`, `//`, `/.`, `/..` danno ENOENT. Nuovo O_CREAT|O_TRUNC non imposta FWASWRITTEN, mentre il troncamento esistente sì.

Solo l’inserimento invalida le osservazioni del genitore. Vecchi/nuovi omonimi mantengono dati, FD, metadati e mapping indipendenti. Le 256 voci includono elementi iniziali non file e oggetti vivi; percorsi canonici/NUL dinamici e byte correnti contano nei 16 MiB. Dopo unlink, l’ultimo FD/mapping libera i costi dinamici; quelli iniziali restano. Budget esaurito o percorso canonico di almeno 1024 byte arrestano esplicitamente senza inventare ENOSPC o errno nativo né pubblicare nome/FD. created-file confronta macOS nativo e cinque profili; test 4K/16K verificano i limiti. Controllo dei permessi, rinomina tra domini di directory iniziali distinti, link e mutazione directory restano aperti.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Metadati di creazione espliciti e umask del processo

L’opzione `darwin_files.umask` (C++ `InitialUmask`) dichiara la maschera iniziale da 0 a 07777 ottale, indipendentemente dal permesso di creazione. `umask(60)` restituisce la precedente e salva i bit bassi 07777, senza memoria guest o FD libero. L’omissione significa sconosciuta, senza dedurre valori host o predefiniti. Si inizializza una volta e le modifiche riguardano solo le creazioni future, senza cambiare l’input. L’esempio usa 18 decimale, cioè 0022 ottale.

Senza `namespace_policy`: L’opzione `darwin_files.creation_policy` (C++ `CreationPolicy`) fornisce metadati completi ai nuovi oggetti. L’oggetto rigoroso contiene esattamente `first_inode`, `block_size`, `generation`, `creation_time`, `mutation_policy`; tempi e politica di modifica usano i formati esistenti. Richiede umask esplicita, almeno un genitore mutable e metadata completi per ogni genitore autorizzato. block_size è 1..INT32_MAX, generation è uint32; l’unità di allocazione è una potenza di due tra 512 e 16 MiB, indipendente dal blocco/pagina VM, e i nanosecondi sono in [0,1000000000). first_inode è uint64 positivo maggiore di tutti gli inode stat/snapshot, anche di altri dispositivi. Stringhe decimali preservano gli interi oltre l’intervallo esatto JSON.

Solo l’inserimento riuscito di un nuovo oggetto consuma la sequenza globale inode. UINT64_MAX la esaurisce definitivamente; close/unlink/riuso del nome/umask/ricerche non la reinizializzano. Rifiuti per esclusività, FD, percorso, voci o byte non pubblicano nome/FD né incrementano il contatore; O_CREAT esistente non consuma nulla. Il nuovo stat64 eredita device/GID dal genitore diretto, UID effettivo guest selezionato (default1000), mode `S_IFREG | (mode & 0777 & ~umask)`, nlink=1 e size/blocks/flags=0. Blocco, generation e quattro tempi iniziali fissi provengono dalla politica. Dopo l’invalidazione di stat/enumerazione completi del genitore, device/GID restano utilizzabili senza ripristinare l’intero record.

Ogni nodo possiede metadati/allocazione propri, senza ereditare il vecchio omonimo. write/truncate/unlink condividono la politica e preservano inode/mode/birthtime e nlink=0 dopo unlink; EFAULT completo lascia lo stato permanentemente sconosciuto. Nessun effetto retroattivo sui nodi esistenti. `created-file-metadata` confronta permessi, vecchia maschera, UID effettivo, dispositivo/gruppo genitore e durata nativa in cinque profili; `virtual-created-metadata` confronta separatamente tutti i 144 byte. I quattro tempi nativi possono differire. Tempo fisso/allocazione sparsa sono regole virtuali; controllo permessi, cambio credenziali, ACL e APFS nativo restano da implementare.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Scambio atomico di file e directory create

RENAME_SWAP=0x2 scambia tramite renameatx_np due file regolari esistenti, due directory vive create dal processo, oppure un file e una di queste directory; RENAME_NOFOLLOW_ANY è facoltativo. La directory iniziale esplicita dichiara mutable:true e swap_rename:true; C++ usa DarwinFileOptions::SwapRenameDirectories. I discendenti ereditano la capacità dell’oggetto originale; riutilizzare nomi eliminati non trasferisce dichiarazioni. false o omissione significa ignoto. Device uguali e diritti di modifica non provano il supporto; domini iniziali distinti restano esclusi.

`openat_nocancel`, `fstatat64` e `F_GETPATH=50` usano lo stesso componente file. Dopo lo scambio, percorsi e osservazioni restano dei rispettivi oggetti; la disponibilità dello stat completo segue il contratto dei metadati.

Una destinazione assente, anche con slash finale, dà ENOENT prima di punto/doppio punto sorgente, dominio, autorità o capacità. Operandi directory iniziali o rimossi restano esclusi. Nel dominio ammesso, entrambi gli ordini antenato/discendente e directory/file figlio danno EINVAL; una destinazione file con slash finale dà ENOTDIR. Lo stesso oggetto con componente ordinario non cambia dopo l’autorizzazione, anche senza capacità dichiarata. Il punto sorgente dello stesso oggetto richiede ancora la sensibilità alle maiuscole ignota. RENAME_EXCL=0x4 + RENAME_SWAP=0x2 e flags ignoti danno EINVAL prima dei percorsi; SECLUDE resta escluso.

Entrambi i sottoalberi non vuoti seguono oggetti genitore, inclusi directory rimosse e file orfani trattenuti da FD/mapping. Lo scambio misto muove solo l’esatta radice file; un vecchio orfano omonimo conserva il proprio genitore. FD, dup, CWD e doppio punto seguono oggetti e nuovi genitori. Byte, identità, metadati, diritti, cursori, flags e lease dei file discendenti restano intatti. Radici mosse e genitori immediati applicano le regole namespace esistenti; una policy configurata aggiorna il ctime proprio del file radice, senza policy i metadati completi restano ignoti.

Ogni riferimento riserva percorso+NUL nei 16 MiB iniziali fissi. Tutti i percorsi collegati/trattenuti in entrambe le direzioni sono verificati sotto 1024 byte e il budget condiviso prima di ritirare insieme i vecchi nomi e pubblicare. Nessuna radice è eliminata e non c’è credito di sostituzione, neppure dai byte di una destinazione non aperta. Il primo spostamento di un file iniziale acquisisce costo dinamico; il ritorno non lo azzera e la ripetizione non lo accumula. Nessun nuovo FD, elemento o inode di creazione. Il rifiuto conserva entrambi i namespace, genitori, cursori, osservazioni e mapping. L’originale SDK-free swapped-directory confronta directory e entrambi gli ordini misti su macOS nativo e cinque profili C++/C/CLI/Python.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Istantanee esplicite delle directory

`getdirentries64` (344) enumera il `contents` immutabile facoltativo di una voce `directories` esistente; C++ usa `DarwinFileOptions::DirectoryContents`. `entries` include in ordine esplicito tutti i figli diretti, `.` e `..`. Senza istantanea anche una directory vuota resta sconosciuta. Non crea percorsi o stat e non consulta l’host.

Ogni voce richiede `name`, `inode` non nullo, `type` (0 sconosciuto, 4 directory, 8 file), `next_offset` e `seek_offset`. Tipo e percorso concordano; gli inode dello stesso percorso risolto concordano tra istantanee e metadati. `next_offset` è positivo, unico nella directory e <=INT64_MAX, senza obbligo di crescere; zero riavvolge. `seek_offset` è l’osservazione d_seekoff distinta, a 64 bit senza segno; sono ammessi zeri ripetuti. Gli interi usano le stringhe decimali senza perdita di stat.

`contents.minimum_buffer_size` richiede un minimo di payload di 1–128 MiB, EOF incluso. Il `minimum_buffer_size` facoltativo della voce (predefinito 0) vincola la lettura che parte lì. L’esempio osserva APFS: 64 byte per i due punti iniziali, 1 a EOF; altrove deve entrare un record intero. LP64 allinea a otto byte, dimensione `roundUp(25 + nameBytes, 8)`. Massimo 4096 voci complessive; i byte dei record contano nei 16 MiB. Gli antenati dichiarati solo da metadati/istantanea contano una volta nei 256 percorsi. JSON resta limitato a 64 KiB.

Open indipendenti hanno cursori separati, dup li condivide. La ripresa accetta solo zero o cookie forniti; posizioni sconosciute arrestano esplicitamente. Ogni chiamata restituisce il massimo prefisso di record interi. Lunghezza >=1024 riserva gli ultimi quattro byte richiesti a EOF (1 alla fine, altrimenti 0); solo il payload è limitato a 128 MiB. L’indirizzo conserva l’aritmetica originale senza segno, overflow compreso. Ordine: dati, avanzamento, posizione precedente, flag. EFAULT successivi mantengono gli effetti precedenti; EOF omette la copia vuota. Una singola copia parzialmente scrivibile si arresta prima di quella copia, conservando gli effetti anteriori.

`directory-entries` confronta campi, dup/riavvolgimento, letture piccole, EOF e ordine delle copie con macOS. Un test separato confronta tutti i byte nativi catturati, nomi lunghi compresi, con il layout SDK. I cookie fissi non riproducono le generazioni dinamiche APFS. Il vecchio `getdirentries` (196), enumerazione dopo modifiche, altri backend nativi e iOS fisico restano fuori da questa verifica.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

Verifica enumerazione (2026-10-05, Release): 498 casi Darwin, 246 superati, 252 saltati per backend indisponibile, zero errori; eseguiti tutti i 63/63 casi ARM64 HVF obbligatori. Superati 11 programmi macOS nativi, 40 controlli C/CLI/report senza salti, cinque combinazioni Python con otto scenari di file ciascuna e 66 test degli strumenti. Conteggi sovrapposti. Prove: `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel HVF Actions resta sospeso; altri backend nativi e iOS fisico non sono verificati.

## Mapping privati di file

`mmap` accetta file regolari del catalogo con `MAP_PRIVATE`: `flags=0x2` o `0x40002` con `MAP_UNIX03`, con offset allineato alla pagina OS. Anche una richiesta breve mantiene tutti i byte del file nella pagina; il resto dell’ultima pagina EOF è zero. La scrittura privata modifica solo quel mapping, non file, altri mapping, metadati fissi o posizione condivisa. Il mapping sopravvive a close e al riuso del FD. Anche sola lettura e PROT_NONE ricevono i byte iniziali; `mprotect` può abilitare la scrittura.

Overflow della fine del file, lunghezza UNIX03 zero e offset UNIX03 non allineato danno EINVAL prima della ricerca FD; un FD invalido dà EBADF prima del budget. Anche la lunghezza storica zero controlla il FD. Offset storici non allineati, stream, file vuoti e pagine interamente oltre EOF arrestano prima dell’allocazione. macOS permette questi mapping EOF ma l’accesso genera SIGBUS; il modello non inventa pagine zero leggibili o consegna di segnali. Mapping condivisi, fissi, eseguibili e JIT restano esclusi.

`DarwinFiles` risolve FD e byte; `DarwinMemory` gestisce posizionamento, diritti, budget e rollback. I dati arrivano solo da `darwin_files`. Il programma comune `file-mapping` verifica copie, close, posizioni, errori e riuso anonimo; un confronto nativo separato verifica offset non nullo, intera pagina e SIGBUS.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### Verifica dei mapping privati, 2026-10-05

Release Darwin: 438 registrazioni uniche, 210 superate, 228 saltate, zero errori; eseguiti tutti i 57/57 requisiti ARM64 HVF e cinque combinazioni Unicorn. Superati nove programmi macOS nativi, confronto dell’intera pagina con offset non nullo e SIGBUS in un figlio isolato. I 36 controlli API/report non hanno omissioni; Python copre cinque combinazioni con `file-mapping`, e passano 66 test degli strumenti e 38 della provenienza. I conteggi si sovrappongono. Evidenze: `build-hvf-arm64/darwin-mmap-verified-evidence/`. Nessuna nuova prova Intel HVF/KVM/WHP o iOS fisico; Intel HVF Actions resta sospeso.

## Metadati espliciti dei file

Una voce può aggiungere `metadata`; tutti i campi seguenti sono obbligatori. Le stringhe decimali mantengono l’intera larghezza; i numeri JSON sono interi esatti entro ±(2^53−1). device è a 32 bit con segno, mode/link_count a 16 senza segno, inode a 64 senza segno e uid/gid/flags/generation a 32 senza segno. size deve corrispondere ai byte; blocks rientra nei 64 bit con segno e block_size nei 32 bit con segno non negativi. I tempi usano secondi a 64 bit con segno e 0–999999999 nanosecondi.

`stat64` (338), `fstat64` (339) e `lstat64` (340) restituiscono lo stesso record LP64 di 144 byte su ARM64/x64. Condividono la risoluzione di open e rispettano dup/close senza allocare FD né cambiare cursori. rdev, padding e campi riservati sono zero. Gli input forniscono i metadati iniziali e la politica facoltativa governa le modifiche; read non aggiorna i tempi e mode non cambia l’accesso al catalogo. Metadati mancanti, flussi, stat precedente, e sicurezza estesa restano esclusi. Gli errori di percorso/FD precedono il puntatore di uscita; le uscite parzialmente accessibili sono rifiutate prima della scrittura. Il test nativo confronta tutti i byte di un file reale e gli offset SDK; lo stesso programma originale verifica le tre chiamate.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### Verifica dei metadati e passi successivi (2026-10-05)

Con stat64: 409 registrazioni uniche, 193 passate, 216 saltate, zero errori; eseguiti 54/54 casi ARM64 HVF obbligatori e cinque combinazioni Unicorn. Passano il confronto SDK/record reale, otto programmi nativi, 36 casi API/report senza salti, cinque combinazioni Python e 66 test degli strumenti; i conteggi si sovrappongono. Ogni caso nativo usa il proprio file di uscita, evitando byte residui dopo uscite più brevi. Mancano prove native Intel HVF/KVM/WHP o iOS fisico per queste aggiunte.

Seguono mapping condivisi e fault EOF, scritture limitate (pagine EOF, durata dopo close, ordine degli errori), osservazioni esplicite tempo/sistema, servizi Mach/thread necessari e dipendenze Mach-O, rebases/binds, inizializzatori e TLS. Objective-C/Swift e Foundation/UIKit richiedono programmi nativi di riferimento. iOS fisico necessita SDK e dispositivo; Intel HVF resta non verificato e le sue Actions sospese.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

La verifica autonoma richiede tutti i 111 casi nativi ARM64 o 74 x64, compresi `LC_MAIN` e `LC_UNIXTHREAD` su ogni piattaforma. Casi obbligatori mancanti/saltati o assenza di `ld64.lld` causano errore.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Usare `kvm` su Linux o `whp` su Windows. Il [workflow Darwin](../../.github/workflows/darwin-native.yml) esegue entrambi i trasporti x64 senza Unicorn e permette ripetizioni separate. Il [riferimento kernel](../../.github/workflows/darwin-kernel-reference.yml) esegue i programmi direttamente su entrambe le ISA macOS, senza NeverD/LLVM. `DarwinNativeCases.def` definisce modi, stati e byte previsti. Solo il riferimento host collega libSystem per il vero ingresso dyld. ISA errata, Rosetta, timeout o divergenze fanno fallire il controllo; non è prova del kernel su dispositivo iOS.

## Evidenze e ambito residuo

Risultati al 2026-10-03; le righe sovrapposte non si sommano:

| Trasporto | Sorgente | Superati | Falliti | Saltati | Carichi nativi |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

Il [run Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) riconcilia 286 identità CTest e 32 processi con XML originale. I 234 casi saltati comprendono 65 casi Unicorn disabilitati, 39 guest ARM64 e 130 altre piattaforme host. L’artefatto `11267489438` ha SHA-256 verificato `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. Anche [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) sono stati controllati indipendentemente. Il [riferimento kernel](https://github.com/NeverSight/NeverD/actions/runs/37064795867) supera 4/4 programmi per ISA, con stato 37, output esatto e stderr vuoto.

C API/CLI con Unicorn: 138 superati, 156 saltati, nessun errore. Python copre tutte le cinque combinazioni; il motore nel pacchetto coincide con 18 rapporti CLI ARM64 e 186 immagini Mach-O superano i controlli di firma. HVF/Unicorn OFF supera 38 controlli, ne salta 231 e non collega Hypervisor.framework. Sono prove d’integrazione, non ulteriori esecuzioni native. La CPU Intel completa resta da validare; vedere [HVF](macos-hvf.md) e il [registro dettagliato](../darwin-emulation.md#hosted-native-verification-2026-10-03).

## Osservazioni temporali esplicite

`ProcessOptions::DarwinTime` / `darwin_time` fornisce osservazioni fisse alla chiamata diretta `gettimeofday` (116), inclusa la terza uscita `mach_absolute_time`, su tutti i profili Darwin. `time_of_day`, `timezone` e `mach_absolute_time` sono facoltativi: l’assenza significa sconosciuto, lo zero esplicito è un valore. Un oggetto vuoto non crea orologi predefiniti. Il modello non legge l’orologio host, non deduce il fuso, non fa avanzare il tempo e non converte i tick assoluti.

Ogni record fornito richiede tutti i membri. `seconds` è senza segno a 32 bit, `microseconds` appartiene a [0, 999999], `minutes_west` / `dst_time` hanno segno a 32 bit, i tick sono senza segno a 64 bit. JSON segue le regole intere senza perdita; fuori dall’intervallo sicuro servono stringhe decimali. Campi sconosciuti, intervalli errati e profili non Darwin sono rifiutati prima del caricamento.

Il `timeval` LP64 occupa 16 byte: secondi estesi con zero a offset 0, microsecondi a 32 bit a 8 e quattro byte zero a 12. Il fuso ha due campi con segno a 32 bit, i tick otto byte. Tempo civile e assoluto costituiscono un unico campione iniziale: tutte le osservazioni richieste devono esistere prima delle copie e dei controlli dei puntatori. Seguono timeval, timezone e absolute ticks. Fuso mancante o EFAULT successivo conservano le scritture precedenti; gli alias seguono lo stesso ordine. Un’uscita singola parzialmente scrivibile causa arresto prima della sua copia, conservando quelle precedenti. Tutti i puntatori nulli riescono senza configurazione; le richieste selettive richiedono solo i valori domandati.

Il programma originale `time` verifica il comportamento nativo; `time-values` emette i 32 byte configurati tramite C/CLI/Python nelle cinque combinazioni guest. Un oracolo SDK confronta ogni byte con tre uscite di una sola chiamata nativa diretta. Restano esclusi orologi in avanzamento, conversione, contatori commpage, timer e oggetti orologio Mach/IPC, oltre al lavoro su dyld, thread, Objective-C/Swift e Foundation/UIKit. Intel HVF Actions resta sospeso; non si aggiunge accettazione nativa Intel o iOS fisico.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

Verifica temporale (2026-10-06, Release): 538 casi Darwin, 274 superati, 264 saltati per backend indisponibile, nessun errore; eseguiti tutti i 66/66 casi ARM64 HVF obbligatori. Passano i 12 programmi macOS nativi e il confronto SDK di un solo campione. C/CLI/report: 43/43 senza salti. Python passa su cinque combinazioni, inclusi i byte temporali esatti e gli otto modi di file esistenti. Passano 66 test degli strumenti, localizzazione, capacità e formato. Conteggi sovrapposti. Prove: `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/`, `darwin-time-public.xml`.

## Tempo Mach e convenzioni di ritorno

`darwin_time.timebase` fornisce `numerator` e `denominator`, interi senza segno a 32 bit diversi da zero. Il rapporto resta esatto, senza riduzione o conversione. `mach_timebase_info_trap`, indice 89, usa ARM64 X16=-89 o x64 RAX=0x01000059. Scrive otto byte little-endian (numeratore, denominatore) e restituisce zero anche con un indirizzo di uscita completamente invalido. Un’uscita parzialmente scrivibile arresta prima della copia; gli errori del trasporto si propagano. La configurazione assente arresta prima del controllo del puntatore, anche nullo.

ARM64 X16=-3 e X16=-4 restituiscono tutti i 64 bit senza segno di `mach_absolute_time` e `mach_continuous_time`. Ogni chiamata richiede solo il proprio valore; zero esplicito è valido. Le corrispondenti voci native x64 generano EXC_SYSCALL e non sono supportate. Restano esclusi avanzamento degli orologi, commpage, timer e oggetti orologio Mach/IPC.

La risoluzione usa i 32 bit bassi del numero; il rapporto conserva i 64 originali. I negativi ARM64 selezionano Mach; x64 usa 0x01000000 per Mach e 0x02000000 per BSD. BSD 3/4 restano read/write; numeri sconosciuti e classi estranee arrestano. La voce risolta determina il ritorno: Mach preserva flag e X1/RDX, BSD mantiene le regole carry; x64 aggiorna ancora RCX/R11. I rapporti Mach contengono `result` e omettono `error`, anche con carry iniziale attivo.

`mach-time` confronta flag, risultato secondario, bit alti, puntatori invalidi e transizioni BSD con il kernel ARM64 nativo. `mach-timebase-values` verifica byte esatti su cinque guest, `mach-clock-values` su ARM64; l’SDK verifica layout e rapporto osservato. Intel HVF Actions resta sospeso; test software e sintattici x64 non costituiscono accettazione Intel nativa o iOS fisica.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Validazione Mach (2026-10-06, Release): 569 casi Darwin, 293 superati, 276 saltati per backend indisponibile, zero errori; eseguiti tutti i 69/69 obbligatori ARM64 HVF. Il passaggio finale supera 13 programmi nativi e due oracoli temporali SDK. C/CLI/report: 100/100 senza salti; Python copre cinque guest. I confronti pubblici sono separati per piattaforma e scenario con budget guest esplicito di 10 secondi; valori predefiniti e regressioni sulle scadenze restano invariati. I conteggi si sovrappongono.

I primi avvii nativi superavano il limite esistente di 5 secondi: misura indipendente di 6.056 secondi e 0.010 al riuso. Lo stesso binario ha poi superato 13 casi col limite originale; i fallimenti sono conservati. La verifica seriale separata supera i precedenti timeout sotto carico host. Prove: `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`, dall’albero prima del commit. In quella revisione ARM64 MRS/MSR NZCV erano fuori dal contratto checked; il test osservava i flag con istruzioni intere. La modifica seguente colma questa lacuna CPU.

## Registro dei flag di condizione ARM64

Il contratto ARM64 checked condiviso ammette le codifiche esatte `MRS Xt, NZCV` e `MSR NZCV, Xt` a EL0/EL1. Le letture restituiscono solo i bit 31–28; le scritture selezionano quei quattro bit in ingresso e ignorano gli altri. Leggere verso `XZR` scarta il risultato; scrivere da `XZR` azzera i flag senza leggere SP. Ogni backend esegue le istruzioni originali. La validazione del setter host e i limiti FPCR/FPSR non cambiano; i registri di sistema vicini non elencati restano non supportati.

`NeverDAArch64NZCVTests` confronta tutte le combinazioni con le istruzioni host e verifica stato scalare/vettoriale completo, memoria, registri limite, arresto/errore degli osservatori, ripristino del contesto e budget condivisi. ARM64 `mach-time` usa ora veri MSR/MRS attorno a SVC per controllare conservazione Mach e transizione a BSD. I requisiti HVF nativi includono i sei metodi a entrambi i privilegi e l’oracolo host. ARM64 KVM/WHP e iOS fisico restano non validati. Restano file scrivibili, informazioni di sistema, clock che avanzano, Mach IPC/thread, dyld/runtime/framework e accettazione su dispositivo.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


Validazione dei file modificabili (2026-10-06): Release Darwin, 610 registrazioni, 322 passate, 288 saltate per backend indisponibile, zero errori; 72/72 requisiti ARM64 HVF eseguiti. Verifica finale con nuove asserzioni EFAULT/metadati: 102 passate, 12 saltate su 114. Passano anche tutti i 15 programmi nativi e 111 test pubblici C/CLI/report. I conteggi si sovrappongono. Il primo tentativo nativo ha individuato FWASWRITTEN, corretto prima dei successi; l’errore è conservato. Nessuna scadenza cambiata. CI GitHub completa e iOS fisico restano separati; Actions Intel sospeso.

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python ha inizialmente superato cinque secondi in tre casi directory ARM64. A parità di argomenti passano tutti i dieci nuovi casi scrivibili; un caso iOS scade a5,005 s reali con1,263 s CPU. Le tre ripetizioni isolate passano con lo stesso limite in2,43–3,17 s,10.941 istruzioni e output65. Carico54–70 su16 CPU logiche suggerisce pressione dello scheduler, senza garanzie di latenza; errori iniziali conservati.

Il metodo Python finale invariato ha superato le cinque combinazioni in41,118 s, mantenendo cinque secondi per processo e separati errori/diagnostica precedenti.


Verifica metadati (2026-10-06): Release mirato148=124 riusciti/24 saltati. Darwin completo645=343 riusciti/300 saltati/2 timeout nei test directory ARM64 HVF esistenti. Ripetizione identica20=8 riusciti/12 saltati, casi interessati3.818/3.949s entro il limite originale5s. Tutte75 le identità HVF richieste hanno osservazioni riuscite; il primo fallimento resta conservato. C/CLI/report117/117 con73 Darwin, Python cinque profili27.359s, nativo15/15, runner66/66 riusciti. Allocazione virtuale, non prova APFS. Nessun limite cambiato; CI completa, Intel, iOS fisico e ambiente completo restano aperti.

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

Verifica del posizionamento sparse (2026-10-06): Release Darwin, 671 casi, 359 superati, 312 saltati per backend indisponibili, nessun errore. Eseguiti tutti i 78 casi ARM64 HVF obbligatori; Unicorn copre cinque profili. Test mirati: 123 superati su 147, 24 saltati. Superati 16 programmi nativi, 122 controlli C/CLI/report (78 confronti Darwin), cinque profili Python (12.344 s) e 66 test del runner. Conteggi sovrapposti, scadenze invariate ed errori precedenti conservati. Evidenze: `build-hvf-arm64/sparse-seek-validation-summary.json`. La geometria segue una politica virtuale esplicita, non equivale ad APFS. CI completa e iOS fisico restano da verificare; Intel HVF Actions resta sospeso.

Verifica unlink (2026-10-06): Release Darwin708 casi,384 superati,324 saltati per backend indisponibili, nessun errore;81 ARM64 HVF obbligatori eseguiti. Mirati156:137 superati/19 saltati. Nativi17/17, C/CLI/report128/128 (Darwin83), Python cinque profili16.268s, runner66/66 superati. Revisione indipendente senza blocchi residui. Conteggi sovrapposti, scadenze invariate, nessuna ripetizione necessaria. Evidenze: `build-hvf-arm64/unlink-validation-summary.json`. Invalidazione/tempi fissi sono regole del modello; file system/runtime completo e iOS fisico restano da verificare. Intel HVF Actions sospeso; CI completa separata.

### Verifica creazione, 2026-10-06

Release Darwin:748 casi,412 passati,336 saltati per backend assenti, nessun errore;84 obbligatori ARM64 HVF eseguiti. Mirati162:150 passati/12 saltati. C/CLI/report133/133 (Darwin88), Python5 profili9.982s, nativi18/18, runner66/66 passati. ARM64 inizialmente rifiutava correttamente i rebase della tabella di puntatori del test; byte inline li eliminano senza ampliare il loader. Fallimenti/binari iniziali conservati; inventario atteso27→28. Revisione indipendente senza blocchi, compresa conservazione del genitore dopo budget insufficiente. Conteggi sovrapposti, limiti temporali invariati. CI completa/iOS fisico separati; Actions Intel HVF sospeso.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### Verifica dei metadati di creazione, 2026-10-06

Release Darwin: 787 registrazioni, 439 superate, 348 saltate per backend indisponibili, zero errori; tutti gli 87 ARM64 HVF obbligatori eseguiti. Mirati: 139/151 superati, 12 saltati. C/CLI/report: 145/145, inclusi 98 confronti input Darwin; metodo Python invariato, cinque profili in 12.211 secondi. Nativi 19/19 e verificatori 66/66 superati. Revisione indipendente senza blocchi; nuovi casi per device/GID di genitori distinti e inode globale, unlink prima della scrittura, umask senza FD libero/input utilizzabile. Conteggi sovrapposti, scadenze invariate, nessuna ripetizione per errore. Tempi fissi di creazione/modifica e allocazione restano politiche virtuali. GitHub CI completa e iOS fisico separati; Intel HVF Actions sospeso.

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### Verifica della rinomina, 2026-10-06

Release Darwin: 835 registrazioni,474 superate,360 saltate e un timeout preesistente dei metadati virtuali macOS ARM 64 HVF (5.087 s). Ricontrollo con stessi argomenti e limite 5 s: 8 superate,12 saltate; identità interessata 0.113 s. Tutte 90 identità ARM 64 HVF obbligatorie hanno osservazioni riuscite tra i due avvii; il gate completo resta registrato come fallito. Mirati 42/54 superati,12 saltati; C/CLI/report 150/150, inclusi 103 confronti Darwin; Python invariato, cinque profili 18.478 s; nativi 20/20, script 66/66. Revisione indipendente ha corretto punti annidati: regressione 4 K/16 K fallita prima e riuscita dopo. Precedente aspettativa ftruncate sola lettura corretta a EINVAL. Errori e versioni delle sonde conservati, conteggi sovrapposti, scadenze invariate. GitHub CI completa e iOS fisico separati; Intel HVF Actions sospeso.

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## Osservazioni esplicite del sistema

`ProcessOptions::DarwinSystem` / `darwin_system` fornisce osservazioni fisse a `sysctl(202)` e al `sysctlbyname(274)` diretto in ogni profilo Darwin. Ogni campo è facoltativo; valori assenti o chiavi non elencate restano non supportati. Nessuna query all’host o versione/modello implicito. La validazione rigorosa JSON e C++ rifiuta valori errati e profili non Darwin prima del caricamento.

`os_revision` è un valore con segno a 32 bit; `cpu_count` vale 1..INT32_MAX; `memory_size` conserva 64 bit senza segno; `max_files_per_process` vale 0..INT32_MAX e usa un int di quattro byte. Gli altri campi scalari sono stringhe di massimo 1023 byte (255 per `hostname`) senza NUL interno; una stringa vuota esplicita è valida e il risultato include il NUL finale. Le osservazioni non modificano scheduling o budget di memoria e descrittori.

| Campo JSON | Nome sysctl | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |
| `max_files_per_process` | `kern.maxfilesperproc` | `1,29` |
| `hostname` | `kern.hostname` | `1,10` |

`hw.pagesize` deriva dalla politica di memoria guest esistente: normalmente otto byte, quattro con output non nullo e capacità esattamente quattro. Il MIB storico `[6,7]` e `hw.pagesize_compat` restituiscono sempre quattro byte. L’OID numerico dinamico di `hw.pagesize` non è supportato. Anche `hw.memsize` si restringe con capacità quattro solo se il suo schema a 64 bit è l’estensione del segno di un intero a 32 bit; altrimenti ERANGE34 preserva output e lunghezza.

Il conteggio MIB usa i 32 bit inferiori e deve essere 2–12; la lunghezza del nome usa 64 bit e deve essere inferiore a 1024. Tutti i byte sono verificati prima di interpretare il primo NUL e rimuovere un punto finale. Il nome vuoto restituisce ENOENT; l’input parzialmente leggibile resta non supportato. `oldlenp` non nullo richiede otto byte interamente leggibili e scrivibili prima degli effetti. Le prove native con puntatori di lunghezza errati non sono tornate entro il limite: restano esplicitamente fuori ambito. `oldlenp` nullo significa capacità zero; `oldp` nullo richiede solo la dimensione. Per chiavi diverse da `kern.hostname`, un buffer corto restituisce ENOMEM12, lascia i dati intatti e scrive lunghezza zero. EFAULT sui dati conserva la lunghezza precedente. Input e capacità sono acquisiti prima dei dati e la lunghezza è copiata per ultima, preservando alias e copie già completate in caso di successivo errore di trasporto.

`hostname` dichiara i byte visibili a questo chiamante guest, senza interrogare l’host, assumere `localhost` sui dispositivi mobili o dedurre entitlement. L’assenza resta ignota; una stringa esplicitamente vuota restituisce un NUL. Per `kern.hostname`, un’uscita non nulla con capacità positiva insufficiente ha successo con esattamente tale numero di byte, terminati da NUL, e riporta tale capacità. La capacità zero conserva ENOMEM12, lunghezza zero e dati invariati; l’uscita nulla riporta la lunghezza completa con NUL. Si verifica solo l’intervallo effettivo. Un intervallo parzialmente scrivibile resta non supportato senza pubblicare prefissi; le copie native parziali sono fuori modello. Si aggiunge solo l’osservazione raw usata da libc uname/gethostname, non i loro import dylib o un runtime completo.

newp/newlen non nulli significano scrittura. Prima restano lettura nome/MIB e verifica completa lettura/scrittura oldlenp. EUID default o esplicito non-root dà EPERM1 prima di osservazione/output dati. EUID0 ferma kern.osversion / kern.maxfilesperproc / kern.hostname unsupported perché la scrittura privilegiata non è modellata; RUID non decide. Altri nodi nativi sola lettura mantengono EPERM1 anche root. Nuova lunghezza0 ignora puntatore; nessun ENOENT inventato per chiavi/alberi/OID dinamici sconosciuti.

Il programma originale `system-info` verifica ABI nativa macOS e guest; `virtual-system` confronta byte configurati tramite C++, C/CLI e Python. Un oracolo SDK acquisisce nove osservazioni host come input espliciti del test e confronta output per nome e numero. Non certifica iOS fisico o Intel HVF.

```json
{"darwin_system":{"hostname":"guest-node","os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man 3/sysctl.3.html).

### Verifica delle query di sistema, 2026-10-06

Release Darwin: 881 registrazioni, 509 superate, 372 saltate per backend indisponibile, nessun errore; eseguite tutte le 93 identità ARM64 HVF obbligatorie. Mirate: 37/49 superate, 12 saltate. C/CLI/report: 163/163, inclusi 113 confronti Darwin. Metodo Python invariato: cinque profili in 15.302 s; programmi nativi 21/21 e script 66/66. Revisione indipendente senza blocchi; priorità aggiuntive e oracolo SDK superati. Una compilazione del nuovo oracolo è fallita per StringExtras mancante, poi è riuscita aggiungendolo; codice e log preservati. Le sonde native con lunghezza non valida restano conservate e fuori contratto. Dopo i test sono stati sistemati solo due commenti iniziali, con ricompilazione riuscita. Conteggi sovrapposti, limiti invariati, nessuna ripetizione per errori runtime necessaria. GitHub CI completa e iOS fisico separati; Intel HVF Actions sospeso.

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## I/O vettoriale dei file e cattura

`readv`/`writev`, `preadv`/`pwritev` e nocancel condividono la logica scalare di file e cattura, senza nuove opzioni o accessi host. Ogni iovec LP64 contiene indirizzo e lunghezza di otto byte. I32 bit bassi con segno di iovcnt devono essere1–1024. L’intero array viene copiato prima della ricerca del descrittore, quindi gli alias di output non cambiano la richiesta. Un array parzialmente leggibile resta non supportato.

Permessi e posizionabilità dello stream precedono le lunghezze. Ogni valore e la somma devono rientrare in INT64_MAX; file e directory richiedono inoltre una somma <= INT_MAX. Lo stdin finito si limita ai byte disponibili e la cattura mantiene il proprio budget. pwritev rifiuta ogni offset negativo prima dell’array; preadv lo verifica dopo descrittore e lunghezze. Gli elementi vuoti ignorano l’indirizzo, mantenendo i controlli di descrittore, tipo e offset. Dopo EOF non si accede alla coda. Le chiamate posizionate conservano il cursore e pwritev ignora append. L’append ordinario riduce l’intera richiesta una sola volta secondo il cursore iniziale, poi sceglie EOF.

Un elemento successivo totalmente invalido restituisce EFAULT mantenendo byte precedenti, avanzamento del cursore normale e FWASWRITTEN dopo aver scritto almeno un byte. Una scrittura non vuota ammessa che restituisce EFAULT per un buffer dati invalida i metadati completi; errori di argomento, rifiuti del modello e guasti backend li conservano. Una destinazione di lettura parzialmente scrivibile arresta con UnsupportedService senza copiare quell’elemento, conservando le copie precedenti. Una sorgente file parzialmente leggibile viene rifiutata prima di ogni effetto sul file. Autorizzazione, lease dei mapping e budget totale precedono la scrittura; errori di verifica o lettura del backend non pubblicano byte di file o cattura.

La cattura verifica prima il budget comune stdout/stderr. Un elemento che oltrepassa il limite degli indirizzi utente non contribuisce byte; quelli precedenti restano. Altri prefissi leggibili vengono catturati con EFAULT. La priorità scalare dell’errore di intervallo sul budget resta invariata. Descrittori duplicati o reindirizzati mantengono la destinazione. L’originale `vectored-io` verifica otto ingressi su macOS nativo, cinque combinazioni guest e C/CLI/Python. Non aggiunge cancellazione, pipe, thread o accettazione iOS fisica.

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### Verifica I/O vettoriale, 2026-10-06

Release Darwin:937 registrazioni,553 superate,384 saltate per backend indisponibile, zero errori; tutte96 le identità ARM64 HVF richieste eseguite. Mirati45/57 superati,12 saltati. C/CLI/report168/168, inclusi118 confronti Darwin; Python copre cinque combinazioni in 20.397s. Nativi22/22, script66/66. La revisione indipendente ha aggiunto un errore di scrittura posizionata sparsa, verificando cursore, EOF reale, rifiuto dei metadati e capacità residua esatta. La prima compilazione usava ancora una query interna rimossa in un vecchio test, ora sostituita dal controllo dell’output reale. Un errore optional<bool> nella nuova asserzione degli eventi ha segnalato otto esecuzioni guest riuscite come fallite; dopo la correzione tutti i controlli interessati sono passati. Fonti e log di entrambi gli errori conservati. Conteggi sovrapposti, scadenze immutate. GitHub CI completa e iOS fisico separati; Intel HVF Actions sospeso.

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## Query di esistenza dei file

`access(33)` e `faccessat(466)` interrogano il catalogo virtuale corrente senza allocare descrittori o cambiare contenuto, cursori, flag e metadati. F_OK conferma il nome secondo il contratto di percorso esistente. I metadati non concedono o revocano accesso al catalogo; non vengono verificati i permessi nativi di ricerca degli antenati, ACL o MAC. Osservazioni stat mancanti o invalidate non impediscono la query. I nomi rimossi restituiscono ENOENT anche se vecchi FD o mapping mantengono l’oggetto; creazione, riuso e rinomina seguono lo spazio corrente.

Il modo usa i 32 bit bassi. R/W/X occupa i bit 0–2, i diritti estesi 9–21. `(mode & 0x003ffe07) == 0` indica esistenza; gli altri bit, incluso il segno, vengono ignorati senza EINVAL. Le richieste di permessi restano UnsupportedService dopo una ricerca riuscita, senza dedurli da metadati o concessioni di modifica. Gli errori noti di percorso/descrittore vengono prima.

Faccessat accetta qualsiasi combinazione dei flag bassi AT_EACCESS(0x10), AT_SYMLINK_NOFOLLOW(0x20), AT_SYMLINK_NOFOLLOW_ANY(0x800). Altri flag danno EINVAL prima di percorso o FD, anche senza catalogo. Le identità reale/effettiva sono fisse; i link fissi seguono le regole sotto. I percorsi assoluti ignorano dirfd; quelli relativi mantengono le regole CWD/FD di directory. nameiat fuori AT_FDCWD legge un byte, controlla il FD relativo e importa la stringa; `/` ignora FD. Primo byte inaccessibile: EFAULT14; FD sconosciuto/file: EBADF9/ENOTDIR20 prima di errori successivi. Anche il percorso relativo vuoto verifica il FD: sconosciuto EBADF, file normale ENOTDIR, altrimenti ENOENT. Catalogo assente e identità directory dello stream sconosciuta restano non supportati.

L’originale `file-access` confronta entrambe le chiamate, bit ignorati, flag e ordine su macOS nativo e cinque guest tramite C++/C/CLI/Python. NOFOLLOW_ANY usa un FD relativo per evitare i link host `/tmp` o `/var`. Test diretti coprono nomi vivi, bit misti, FD esauriti, indipendenza dei metadati ed errori di memoria.

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### Verifica dell’esistenza dei file, 2026-10-06

Release Darwin:971 registrazioni,575 superate,396 saltate per backend indisponibile, zero errori; tutte99 le identità ARM64 HVF richieste eseguite. Mirati23/35 superati,12 saltati, inclusi14 diretti. C/CLI/report173/173, inclusi123 confronti Darwin. Python ha verificato cinque combinazioni in 16.235s; nativi23/23 e script66/66. Revisione indipendente di progetto e codice senza blocchi. Conservati il risultato NOFOLLOW_ANY iniziale dovuto al link host /tmp e il confronto con percorso canonico; il programma comune usa un FD di directory relativo. Conteggi sovrapposti, scadenze immutate, nessun errore di esecuzione da riprovare. GitHub CI completa e iOS fisico separati; Intel HVF Actions sospeso.

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## Creare e rimuovere directory

`mkdir(136)` e `mkdirat(475)` creano in un genitore diretto esplicitamente modificabile. Le nuove directory ereditano l’autorità sui nomi; quelle iniziali mantengono autorizzazioni proprie. Ereditano soltanto device/GID noti, mai stat completo, dimensione, allocazione, tempi o cookie. Una nuova directory nasconde tutte le osservazioni del precedente file omonimo. `creation_policy` resta per file regolari: i discendenti usano l’identità ereditata e la sequenza globale inode; mkdir non consuma inode di file. Controllo dei permessi e metadati nativi delle directory restano esclusi.

Il percorso comune consente a mkdir un ultimo nome assente seguito solo da barre. Antenato mancante prima di punto/doppio punto dà ENOENT, file antenato ENOTDIR, nome esistente EEXIST. Restano FD/CWD relativi, indipendenza dal FD per percorsi assoluti e priorità degli errori di stringa. Non serve un FD libero; rifiuti di ricerca, autorità, budget o trasporto non pubblicano cambiamenti.

`rmdir(137)` e `unlinkat(472)` con AT_REMOVEDIR(0x80), anche AT_SYMLINK_NOFOLLOW_ANY(0x800), rimuovono directory vuote create dal processo. Bit bassi32 sconosciuti danno EINVAL prima degli input; DATALESS e SYSTEM_DISCARDED restano esclusi. Restano errori noti di percorso/tipo/radice; rimuovere directory iniziali senza autorizzazione removable resta UnsupportedService. Sulle directory ammesse, punto finale dà EINVAL, doppio punto da directory collegata o destinazione non vuota ENOTEMPTY. FD di directory, dup e CWD mantengono l’oggetto originale e non impediscono più la rimozione. File regolari già unlink e mapping non contano come nomi: confronti nativi mantengono byte, inode e ultimo F_GETPATH dopo rimozione/riuso del genitore.

Ogni percorso canonico+NUL e una voce usano il budget comune16 MiB/256 voci. La rimozione seguita dal rilascio di tutti i riferimenti rimborsa solo questo costo, mantenendo file orfani/mapping. Solo il successo invalida stat/enumerazione del genitore; osservazioni complete delle nuove directory restano sconosciute. L’originale `directory-mutations` confronta creazione annidata, rinomina, unlink, rimozione e riuso su macOS nativo e cinque guest tramite C++/C/CLI/Python. Un file orfano mantenuto da una descrizione o lease di mapping trattiene anche le directory genitrici e i costi correnti percorso/NUL e voce, anche dopo la chiusura dei loro FD. Il recupero del file precede la catena delle directory. Spostare un antenato creato vivo aggiorna F_GETPATH; il riuso del nome non assegna i vecchi oggetti al sostituto.

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### Verifica delle modifiche alle directory, 2026-10-06

Release Darwin:1.017 registrazioni,609 superate,408 saltate per backend indisponibile,zero errori;102 ARM64 HVF obbligatori eseguiti. Mirati58 superati,12 saltati,inclusi26 nuovi diretti4K/16K. C/CLI/report178/178,inclusi128 Darwin;Python cinque combinazioni in 17.255s,nativi24/24,script66/66. Revisione indipendente di budget,riuso nomi,identità genitore,lease e rollback. Una sonda nativa dopo il primo successo ha rilevato EISDIR per radice solo di barre ed EBUSY con punto/doppio punto finale;decisione comune e test nativo/guest corretti. Risultati iniziali e copie sorgente/binarie conservati. Conteggi sovrapposti,scadenze immutate,Intel HVF Actions sospeso;GitHub CI completa e iOS fisico separati.

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## Identità delle directory trattenute

FD/CWD mantengono l’oggetto eliminato e la catena originale dei genitori anche con nomi riutilizzati. Open del punto ha cursore indipendente,dup condiviso;doppio punto segue il genitore originale. I figli ordinari di directory eliminata danno ENOENT. LOOKUP attraversa genitori eliminati trattenuti,mentre creazione/rimozione/rinomina danno ENOENT. Punto/doppio punto finale della rinomina dà EINVAL prima di quel componente,dopo errori precedenti. F_GETPATH conserva l’ultimo percorso;stat/enumerazione completi restano ignoti. Percorso+NUL e una voce restano conteggiati fino all’ultimo FD/CWD/figlio trattenuto. Close/dup2/cambio CWD/ammissione modifica recuperano catene irraggiungibili;costi iniziali e lease dei file separati. `deleted-directories` confronta nativo e cinque guest. Un file orfano mantenuto da una descrizione o lease di mapping trattiene anche le directory genitrici e i costi correnti percorso/NUL e voce, anche dopo la chiusura dei loro FD. Il recupero del file precede la catena delle directory. Spostare un antenato creato vivo aggiorna F_GETPATH; il riuso del nome non assegna i vecchi oggetti al sostituto.

### Verifica della durata delle directory, 2026-10-06

Release Darwin1.051 casi,631 superati,420 indisponibili saltati,zero errori;105 ARM64 HVF obbligatori eseguiti. Mirati98 superati/12 saltati,diretti iniziali64/64 con14 nuovi. C/CLI/report183/183,133 Darwin;Python cinque combinazioni 18.691s,nativi25/25,script66/66. Sonda nativa aggiuntiva corregge l’ordine del punto finale di rinomina;prime fonti/risultati/copie conservate. Agente principale ha verificato le prove;revisione indipendente finale indisponibile. Conteggi sovrapposti,scadenze immutate,CI completa/iOS fisico separati,Intel HVF Actions sospeso.

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## Rimuovere directory iniziali ammesse esplicitamente

Il Boolean rigoroso `"removable": true` di una voce directory (C++ `DarwinFileOptions::RemovableDirectories`) dichiara una directory ordinaria, non un mount, con una sola identità nello spazio dei nomi. Deve essere una voce iniziale esplicita di `directories`, diversa dalla radice, con genitore immediato esplicitamente modificabile. Si rifiutano modi/flag speciali noti, alias inode (incluse le istantanee) e numeri di dispositivo noti incompatibili tra genitore e destinazione. Numeri uguali da soli non dimostrano l’assenza di mount. Omissione/false restano non supportati; altri tipi JSON sono invalidi. Non fornisce un modello generale di permessi o mount.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

La rimozione richiede uno spazio dei nomi corrente vuoto. Le sottodirectory iniziali implicite restano dopo unlink del loro ultimo file originale. Il successo invalida le osservazioni complete stat/enumerazione dell’oggetto e del genitore immediato; i vecchi FD/dup/CWD mantengono oggetto e catena originale dei genitori. Gli input immutabili non fanno riapparire nomi rimossi. Un nuovo file o directory con lo stesso nome ha identità separata e non recupera vecchi metadati o istantanee. Gli input del chiamante restano invariati.

Ogni riferimento removable aggiunge percorso e NUL al costo iniziale fisso di 16 MiB. Voci, percorsi, riferimenti e istantanee iniziali rimangono conteggiati dopo rimozione e ultima chiusura, incluso il loro posto nel limite di 256 voci. I nuovi oggetti mantengono la propria contabilità dinamica. Il programma originale `initial-directory-removal` rimuove la directory vuota preesistente del test nativo mentre è aperta, riusa il nome per un file e poi una directory, verifica la conservazione tramite il solo CWD e ripristina la directory vuota. Lo stesso programma viene eseguito tramite C++/C/CLI/Python nelle cinque combinazioni guest.

### Verifica della rimozione delle directory iniziali, 2026-10-06

Le sorgenti finali Release hanno riconciliato 1.089 test Darwin: 657 superati, 432 saltati per backend non disponibile, nessun errore; tutti i 108 casi ARM64 HVF obbligatori sono stati eseguiti. I controlli mirati hanno superato 27/39 casi con 12 esclusioni; è passato anche il controllo aggiuntivo degli alias derivati soltanto dagli snapshot. C/CLI/report pubblici: 191/191; Python: cinque combinazioni in 76,276 s; programmi nativi originali: 26/26; esecutori delle evidenze: 66/66. Un input inode/snapshot incoerente nel test è stato corretto, conservando i risultati falliti.

Due esecuzioni complete precedenti avevano uno e tre timeout nei casi esistenti di file/rinomina. Un’esecuzione strumentata ne ha riprodotto uno con 5,008 s reali e 0,171 s di CPU del processo. I confronti dello stesso metodo e dei programmi precedenti sono passati, ma la causa della latenza resta irrisolta; il successo finale non dimostra stabilità dei tempi. La diagnostica temporanea è stata rimossa, gli hash dei programmi ripristinati e il limite originale del guest di 5 s mantenuto. Audit principale di sorgenti/evidenze completato; revisione indipendente non disponibile. I conteggi si sovrappongono. CI GitHub completa, iOS fisico e Actions Intel HVF sospese restano fuori da questa accettazione locale.

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## Rinomina esclusiva dei file regolari

Dopo la risoluzione di origine e destinazione, RENAME_EXCL restituisce EEXIST per un altro file o directory esistente prima dei controlli di mount e mutazione. Restano prioritari gli errori precedenti del percorso, incluso EINVAL per punto/doppio punto finale. Una destinazione assente usa la stessa transazione limitata, conservando descrizioni aperte, cursori, flag, lease dei mapping e transizioni dei metadati configurate. Lo stesso oggetto resta esplicitamente non supportato: il risultato nativo dipende dalla distinzione maiuscole/minuscole del filesystem, non dimostrata dalle chiavi esatte. Normalizzazione del caso, sorgenti directory iniziali e SECLUDE restano esclusi. Il programma originale `renamed-file` confronta rifiuto senza cambiamenti dei metadati e successo EXCL|NOFOLLOW_ANY su macOS nativo e C++/C/CLI/Python.

Verifica, 2026-10-06 (Release): 1.097 test Darwin, 665 superati, 432 saltati per backend indisponibile, nessun errore; eseguiti tutti i 108 casi ARM64 HVF obbligatori. Mirati: 44 superati, 12 saltati, inclusi otto nuovi casi diretti. C/CLI/report: 191/191; Python: cinque combinazioni in 19,241 s; sonda indipendente: 26 controlli superati. Il primo tentativo nativo ha superato il tempo in return; gli altri 25, incluso renamed-file, sono passati. Tre ricontrolli return sullo stesso binario invariato hanno richiesto 0,014–0,034 s, poi tutti i 26 casi sono passati con il limite originale di 5 s. Il primo errore resta conservato e inspiegato; questi risultati e il precedente successo HVF non provano stabilità della latenza. Audit principale completato, revisione indipendente non disponibile. Conteggi sovrapposti; iOS fisico, CI GitHub completa e Actions Intel HVF sospese fuori dall’accettazione locale.

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.


## Rinomina tra directory create

Una directory iniziale e tutti i discendenti creati da questo processo con mkdir/mkdirat condividono un dominio di nomi virtuale. `rename`, `renameat` e `renameatx_np` spostano file regolari tra questi genitori senza un nuovo campo JSON. Dopo aver creato `/work/left` e `/work/right` sotto `/work` iniziale modificabile, `/work/data` può passare ai figli e tra essi. Un `/work/left` iniziale dichiarato separatamente resta un altro dominio, anche con lo stesso device. La topologia generale dei mount resta ignota.

Entrambi i genitori diretti richiedono autorizzazione; le directory create la ereditano con device/GID noti. Identità, proprietario/gruppo, diritto di scrittura e allocazione del file restano invariati. Device discordanti vengono rifiutati. Uno spostamento invalida stat/elenco completi di entrambi i genitori. Directory iniziali rimosse e percorsi riutilizzati restano oggetti distinti; vecchi FD/CWD non ricevono il dominio sostitutivo.

Restano transazione limitata, lease dei mapping, costi percorso/NUL e precedenza degli errori. EXCL con destinazione esistente distinta dà EEXIST prima di dominio/autorizzazione. `renamed-file` confronta spostamento nel figlio creato, sostituzione nel genitore iniziale e ritorno via C++/C/CLI/Python e macOS nativo. Permessi, spostamento di directory iniziali, link fisici, link simbolici dinamici e metadati APFS nativi restano aperti.

### Verifica tra directory padre, 2026-10-06

La verifica Release finale ha riconciliato 1,115 registrazioni Darwin: 683 superate, 432 saltate per backend non disponibile, nessun errore; eseguiti tutti i 108 casi ARM64 HVF obbligatori. I controlli diretti hanno superato 56/56, inclusi 18 nuovi casi 4K/16K. C/CLI/report pubblico: 191/191; il metodo Python ha coperto cinque profili in 22.254s. Programmi nativi originali: 26/26; sonda separata di chiamate grezze: 34 controlli; script di documentazione/capacità/esecuzione prove: 296/296. I conteggi si sovrappongono. Gli hash dei dieci binari di verifica sono rimasti invariati dopo la modifica CMake limitata a MSVC.

Le due verifiche complete precedenti conservano tre e due timeout nel metodo HVF di file esistente. I confronti del metodo completo, della directory di lavoro e della sessione sono passati senza stabilire la causa; il successo finale non dimostra stabilità della latenza. La sonda confrontava inizialmente /tmp con /private/tmp canonico; ricavare il percorso dal FD radice ha corretto quattro aspettative. Il programma nativo esteso usava mkdir(136) per la pulizia; rmdir(137) ha corretto exit150. Sorgenti ed errori iniziali sono conservati e il limite ospite rimane 5s.

Quattro conflitti LP64 nelle liste di inizializzazione rilevati dalla CI Linux completa ora usano valori uint64_t espliciti; NeverDJumpTableTests riceve /bigobj sotto MSVC. La compilazione effettiva Linux/Windows attende la CI. La precedente CI completa segnalava anche errori separati del corpus Windows EH e dell’annullamento di una PR chiusa. Completata l’autorevisione di sorgenti/prove; non si rivendicano revisione indipendente, iOS fisico o verifica Intel HVF sospesa.

`build-hvf-arm64/cross-parent-rename-validation-summary.json`, `cross-parent-rename-darwin-synced-evidence/`, `cross-parent-rename-darwin-evidence/`, `cross-parent-rename-darwin-rechecked-evidence/`, `cross-parent-rename-public-synced.xml`, `cross-parent-rename-python-synced.log`, `cross-parent-rename-native-fixed-evidence/`, `cross-parent-rename-probe/`.

## Scambio atomico di nomi di file regolari

RENAME_SWAP=0x2 scambia i nomi di due file regolari esistenti tramite renameatx_np, facoltativamente con RENAME_NOFOLLOW_ANY. Una directory iniziale esplicita deve dichiarare mutable:true e swap_rename:true; C++ usa DarwinFileOptions::SwapRenameDirectories. I discendenti creati ereditano la capacità dell'oggetto originale. Eliminare e riutilizzare un percorso non trasferisce la vecchia dichiarazione. false o omissione lascia la capacità ignota; device uguali e autorità sul namespace non dimostrano supporto. Domini iniziali distinti restano esclusi.

Entrambi i percorsi usano il resolver esistente. Un target assente dà ENOENT prima di dominio, autorità e capacità. Operandi directory restano esplicitamente non supportati: swap nativo può scambiare file e directory, quindi EISDIR del rename ordinario non si applica. Lo stesso oggetto autorizzato è un no-op anche senza dichiarazione della capacità. RENAME_EXCL=0x4 + RENAME_SWAP=0x2 e flags ignoti danno EINVAL prima dei percorsi; SECLUDE resta aperto.

Entrambi i file restano collegati. Identità, proprietario/gruppo, byte, autorizzazione di scrittura, descrizioni, cursori, flags e lease dei mapping propri restano invariati. Le politiche virtuali configurate aggiornano ciascuna ctime; politiche assenti o invalidate lasciano i metadati completi ignoti. Uno scambio reale invalida metadati ed enumerazione completi di entrambi i genitori. Non consuma inode di creazione, voce o FD; l'input del chiamante resta invariato.

Ogni riferimento di capacità riserva percorso+NUL nel budget iniziale fisso di 16 MiB. La transazione verifica entrambi i costi dinamici completi prima della pubblicazione; byte e lease ancora collegati non offrono credito di sostituzione. Scambi ripetuti riutilizzano tali costi. Il programma originale renamed-file scambia verso un figlio creato e torna, verifica entrambi gli oggetti e continua la sostituzione ordinaria su macOS nativo e tutti i profili C++/C/CLI/Python. Permessi, topologia dei mount, normalizzazione del caso, spostamento di directory iniziali restano lavori separati.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/work","mutable":true,"swap_rename":true}]}}
```

[Apple volume swap capability](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsswaprenaming?changes=__1_2), [XNU rename](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

### Verifica swap, 2026-10-06

Release Darwin: 1.133 registrazioni, 701 passate, 432 saltate per backend assente, nessun errore; tutti i 108 casi ARM64 HVF obbligatori eseguiti. Controlli diretti rename 68/68, inclusi 18 nuovi casi di opzioni e 4K/16K. C/CLI/report pubblico 192/192; il metodo Python ha coperto cinque profili in 16,153s. Programmi nativi originali 26/26, sonda indipendente di chiamate grezze 45 controlli. Conteggi sovrapposti; limiti guest invariati.

Due test diretti iniziali avevano aspettative errate per autorità di scrittura ignota e metadati ignoti dopo mutazione; sono state corrette solo le aspettative. Un primo filtro JSON selezionava zero test e non conta; successivamente proprietario reale e suite pubblica completa sono passati. Fonti e risultati iniziali restano preservati. La latenza HVF/nativa precedente è ancora inspiegata; il passaggio non prova stabilità. Autorevisione di fonti/evidenze completata; nessuna revisione indipendente, accettazione iOS fisico, CI GitHub completa o Intel HVF sospeso rivendicata.

`build-hvf-arm64/swap-rename-validation-summary.json`, `swap-rename-darwin-evidence/`, `swap-rename-public.xml`, `swap-rename-python.log`, `swap-rename-native-evidence/`, `swap-rename-probe/`.


## Rename di directory create dal processo

rename, renameat e renameatx_np ordinari spostano una directory creata viva e il sottoalbero entro un dominio iniziale, con autorizzazione di entrambi i genitori immediati. Destinazione assente con slash finali ammessa; file regolare: ENOTDIR; directory non vuota: ENOTEMPTY; spostamento in discendente: EINVAL. Il medesimo nome autorizzato non cambia stato. EXCL dà EEXIST per altra destinazione esistente prima di tipo, ciclo, dominio o autorità. Gli errori di ricerca destinazione precedono i punti sorgente. Punto/doppio punto sorgente verso lo stesso oggetto resta UnsupportedService perché il caso del filesystem è ignoto. Sorgenti iniziali/rimosse, destinazioni directory iniziali, domini distinti, link, permessi e SECLUDE restano esclusi.

L’appartenenza segue oggetti genitori: figli nominati, directory rimosse mantenute e orfani FD/mapping aggiornano percorsi. FD, dup, CWD e doppio punto seguono lo stesso oggetto sorgente e il nuovo genitore. La destinazione vuota sostituita conserva vecchio percorso e genitore; figli ordinari ENOENT, punti/CWD mantengono il vecchio oggetto. I suoi orfani non seguono un altro spostamento del sostituto, anche con testo identico.

Byte, identità, metadati, autorità di scrittura, cursori, flag FD e mapping dei figli restano. Sorgente e genitori invalidano osservazioni complete; le directory create conservano autorità ereditata per creazioni/rename ammessi. Nessuna nuova voce, FD o inode. Prima della pubblicazione si preparano tutte le chiavi e i percorsi vivi/mantenuti entro 1024 byte con NUL e 16 MiB. Solo la destinazione senza FD/CWD/discendenti mantenuti finanzia credito, recuperato una volta. Un rifiuto mantiene nomi, genitori e osservazioni; il solo mapping trattiene costi dei genitori rimossi.

L’originale SDK-free `renamed-directory` confronta macOS nativo e cinque guest via C++/C/CLI/Python; 4K/16K verifica riuso, rollback, capacità esatta, discendenti lunghi, crediti, recupero genitori ed esaurimento voce/FD/inode.


### 2026-10-06

Release finale:1,175 registrazioni Darwin,731 superate,444 skip backend indisponibile,zero errori;111 obbligatori ARM64 HVF eseguiti. Mirato32/44 (12 skip,22 nuovi4K/16K),confini finali4/4. C/CLI165/165 e report32/32 senza skip;Python cinque profili21.759s,nativo27/27,sonda indipendente79. La revisione indipendente conferma le correzioni del separatore root del programma e del confine proprietà FS per punti sorgente allo stesso oggetto. Conservati exit124 iniziali,due aspettative obsolete fallite e sorgenti/binari;verifica completa finale superata. Conteggi sovrapposti,limiti temporali invariati. Vecchi timeout HVF/nativi irrisolti,nessuna prova di stabilità. Intel HVF Actions sospeso;iOS fisico e GitHub CI completo separati.

`build-hvf-arm64/directory-rename-validation-summary.json`, `directory-rename-darwin-final-evidence/`, `directory-rename-public.xml`, `directory-rename-reports.xml`, `directory-rename-python.log`, `directory-rename-native-evidence/`, `directory-rename-probe/`, `directory-rename-initial-fixture/`, `directory-rename-darwin-evidence/failed-source/`.

### Verifica degli scambi di directory e misti, 2026-10-07

La verifica Release Darwin riconcilia 1.219 registrazioni: 763 superate, 456 saltate per backend non disponibili, zero errori. Tutti i 114 casi ARM64 HVF obbligatori sono stati eseguiti. La verifica mirata supera 32/44 casi con 12 indisponibili, inclusi tutti i 24 nuovi casi diretti 4K/16K; il componente file completo supera 317/317. I programmi nativi originali superano 28/28 e una sonda indipendente di chiamate grezze registra 32 osservazioni riuscite su questo filesystem macOS senza distinzione tra maiuscole e minuscole. I conteggi si sovrappongono e i limiti temporali ospiti restano invariati.

Una revisione indipendente del piano e del codice ha verificato la transazione bidirezionale, i costi dei file iniziali, entrambi i sottoalberi conservati, gli orfani trattenuti solo dal mapping e i rimborsi esatti. Il primo test rosso ometteva la dichiarazione esplicita di scambio della radice ed è escluso dall’accettazione; la base corretta falliva nei sei casi selezionati sul precedente rifiuto delle directory. Due asserzioni iniziali per file misti scartavano erroneamente metadati configurati; ora confrontano il record completo cambiando solo ctime. Una tabella locale di puntatori costanti introduceva rebases ARM64 e sei rifiuti di caricamento. Quattro asserzioni scalari la sostituiscono: il programma corretto non ha rebases classici e il loader mantiene il rifiuto dei fixups non supportati.

La prima esecuzione mirata corretta conserva tre timeout HVF di cinque secondi. I controlli individuali, su tre profili e la verifica completa successiva sono passati; la causa resta sconosciuta e ciò non prova stabilità della latenza. Fonti, binari, errori e controlli iniziali sono conservati. Restano incompleti spostamenti di directory iniziali, domini iniziali distinti, dichiarazioni di permessi/mount/maiuscole, dipendenze dinamiche e runtime dei framework. iOS fisico, Intel HVF sospeso e GitHub CI completo sono confini di accettazione separati.

`build-hvf-arm64/directory-swap-research/`: `darwin-final-evidence/`, `darwin-evidence/`, `focused-v5.xml`, `file-owner-v5.xml`, `native-workload-fixed-evidence/`, `native-probe-v2-results.json`, `fixture-fixups-before/`, `hvf-controls.json`.

I binari finali collegati hanno ripetuto con successo la verifica Darwin completa. L’integrazione fissata di dev 0a9a1d28d registra 4.515 casi su 20 componenti condivisi: 4.489 superati, sei Z3 facoltativi e 20 del corpus Windows EH indisponibile saltati, zero errori. Include C/CLI 170/170 e report 32/32. Python copre i cinque profili in 30,780s. Tutte le 12 varianti mobili native architettura/fixup corrispondono a 4.532 osservazioni dei programmi originali; sessione unica e metadati Swift completi corrispondono 12/12. Il generatore di witness Swift riproduce il catalogo con SDK/compilatore registrati dopo la correzione dell’indentazione di un commento. È accettazione locale Release LLVM 23/Apple Clang 17, non di dev successivo o Linux Clang 18.


## Spostamento ordinario dei sottoalberi iniziali dichiarati

Il Boolean rigoroso `"movable": true` su una directory iniziale esplicita diversa dalla radice autorizza il suo rename ordinario e dichiara l’intero sottoalbero iniziale composto da directory ordinarie senza mount o alias di nome. `DarwinFileOptions::MovableDirectories` viene aggiunto dopo i membri aggregati C++ esistenti. Il genitore immediato deve essere mutable. Assenza/false mantiene l’incertezza; altri tipi JSON sono rifiutati. I discendenti conservano i propri diritti mutable/removable/movable e di scrittura. Non autorizza SWAP di directory iniziali, permessi generali o mount. Si rifiutano flags noti, modalità speciali delle directory, file ordinari con più link e alias inode da stat/istantanee. Ogni dominio connesso dalle dichiarazioni può avere al massimo un dispositivo noto, inclusi sottoalberi fratelli e file sotto un antenato senza stat. L’uguaglianza dei dispositivi non connette altri domini.

Gli oggetti conservano nomi, genitori, stat/istantanee e diritti originali. I vecchi percorsi d’ingresso non ricreano nomi spostati o rimossi. Discendenti non aperti, FD/dup/CWD, mapping e discendenti rimossi seguono i loro oggetti. I discendenti immutati conservano stat, istantanee, cookie e SEEK_END; radice spostata e genitori modificati perdono le osservazioni complete. Riutilizzare un nome non eredita osservazioni o diritti. L’input iniziale descrive una singola esecuzione sincrona, senza API di sostituzione durante l’esecuzione. SWAP resta separato: dichiarazione diretta swap_rename degli oggetti iniziali, copia dal genitore a mkdir e conservazione durante lo spostamento. Entrambi i genitori SWAP devono avere supporto dichiarato.

Sostituire una destinazione iniziale vuota richiede anche removable. Prima della pubblicazione si verificano 1023 byte per percorso e il budget comune di 16 MiB. Ogni riferimento movable riserva il percorso originale più NUL senza un’altra voce. Percorsi, riferimenti, istantanee e voci delle directory iniziali restano riservati dopo la rimozione. PathCharge dinamico parte da zero e addebita una volta il percorso corrente di ogni membro collegato o trattenuto. Solo il costo dinamico esistente di un obiettivo subito liberabile fornisce credito; FD/CWD/figli/mapping orfani trattenuti non lo forniscono. Il recupero finale rimborsa una volta. Gli antenati iniziali impliciti non aumentano il limite di 256 voci; il recupero dei file ordinari iniziali è invariato.

Esempio:

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/work","mutable":true,"movable":true}]}}
```

initial-directory-move verifica senza SDK spostamento, cursori condivisi/indipendenti, flags FD, CWD, mapping privato, sostituzione e ripristino dei nomi. Profili guest locali e iOS fisico hanno verifiche separate.

Il limite sullo SWAP iniziale riguarda gli oggetti radice sorgente e destinazione della chiamata. Lo scambio di antenati creati può trasportare discendenti iniziali già spostati, mantenendone lo stato e i costi dinamici. Le restrizioni precedenti si applicano in assenza delle dichiarazioni richieste.

Dopo la revisione indipendente finale su macOS ARM64, il gate Darwin registra 1.264 test: 796 superati, 468 saltati per backend indisponibile, zero errori. Tutti i 117 carichi ARM64 HVF obbligatori sono stati eseguiti. Il sottoinsieme file passa 342/342, con 22 nuovi casi 4K/16K e tre controlli di ammissione. CreationPolicy conserva Device/GID del padre spostato; riuso del nome, sequenza inode e umask corrente restano distinti. Al limite esatto di 16 MiB, 16 scambi avanti/indietro non accumulano costi; il ritorno libera solo la differenza effettiva di sei byte. C/CLI pubblico: 175/175; report: 33/33; kernel nativo: 29/29; sonda originale indipendente: 19 osservazioni riuscite. Le cinque configurazioni Python passano in 27.865 secondi; API pura 71, deriva SDK e runner 49 passano anche. I conteggi si sovrappongono. Sorgenti, binari, tentativi e risultati sono conservati in build-hvf-arm64/initial-directory-move/ e legati al commit. iOS fisico, Intel HVF sospeso, SWAP delle radici iniziali, permessi/mount/case, EOF condiviso, Mach/thread/dyld e framework restano lavori separati.

## Scambio atomico delle radici iniziali dichiarate

Il Boolean rigoroso exchangeable:true (C++ DarwinFileOptions::ExchangeableDirectories, in coda all’aggregato) autorizza solo una radice iniziale esplicita diversa da / come operando RENAME_SWAP. Il padre iniziale diretto deve essere mutable. Assenza/false restano esclusi; altri tipi sono invalidi. Condivide con movable il sottoalbero ordinario senza mount e con nomi unici, più controlli flags/modi speciali/alias/hard link/dispositivi dell’intero componente. L’unione definisce soltanto la topologia. Ogni riferimento riserva percorso originale+NUL separatamente, anche sulla stessa radice, senza altra voce. Dispositivi uguali non uniscono domini.

La sorgente iniziale normale/EXCL richiede ancora movable e la sostituzione iniziale normale removable. exchangeable non concede questi diritti, mutable ai discendenti, scrittura o permessi/mount generali. Oggetti distinti richiedono entrambi i padri reali mutable e con swap_rename indipendente. SWAP omonimo autorizzato di componente normale verifica padri/dispositivi e non modifica nulla, senza prima carica né capacità per oggetti distinti. Dot/case del medesimo oggetto restano sconosciuti; ordine di destinazione assente e dot invariato.

Radici iniziali non vuote, iniziali/create e directory/file si scambiano in entrambe le direzioni. Entrambi gli alberi collegati e trattenuti sono interamente verificati, poi tutti i nomi rimossi prima della pubblicazione. Le radici restano collegate: nessun credito per sostituzione/contenuto né nuovo FD/inode/voce. FD/dup/CWD/cursori, oggetti padre, lease e diritti seguono i propri oggetti; discendenti invariati conservano stat/istantanee. Vecchi oggetti eliminati e nuovi omonimi restano separati. Costi dinamici iniziano a zero, sono addebitati una volta e poi sostituiti; i fissi restano riservati. Errori di percorso/budget preservano entrambi gli stati.

Il carico originale senza SDK initial-directory-swap scambia empty e data preesistenti e li ripristina, controllando discendenti, mapping, CWD, cursori, flags e creazione dopo spostamento. L’accettazione precedente f98068c07 è un distinto risultato normale congelato; solo questa dichiarazione estende SWAP delle radici iniziali.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/left","exchangeable":true},{"path":"/right","exchangeable":true}]}}
```


La verifica Release su macOS ARM64 registra 1,303 casi Darwin: 823 superati, 480 saltati per backend indisponibili, zero errori; tutti i 120 casi ARM64 HVF obbligatori sono eseguiti. File 361/361 (16 nuovi casi 4K/16K e 3 ammissioni), C/CLI 180/180, parser dei rapporti 34/34, programmi kernel originali 30/30 e sonda indipendente 35 osservazioni passano. Python copre cinque configurazioni in 33.894 secondi; passano 71 test API puri, 49 unità inventario/riferimento e verifiche SDK, formato, capacità, provenienza e documentazione. I conteggi si sovrappongono.

Alla capacità esatta, lo stesso nome e 16 scambi di andata e ritorno conservano oggetti e costi. Se servono sei byte ma ne restano cinque, entrambi i rifiuti conservano alberi, cursori e budget di creazione. Le revisioni indipendenti del piano e del codice finale sono approvate. Tentativi, sorgenti, binari e risultati sono congelati in `build-hvf-arm64/initial-directory-swap/` e legati al commit. Un rapporto eseguito con sovrapposizione è escluso e ripetuto in serie; i marcatori di traduzione sono sincronizzati. Nessun limite temporale o controllo negativo è allentato. Permessi, mount/maiuscole non dichiarati, mappe condivise/EOF, orologi progressivi, Mach/thread/dyld/framework restano incompleti; iOS fisico, Intel HVF sospeso e CI remota di merge sono verifiche separate.

## Osservazioni esplicite dei limiti, in sola lettura

`getrlimit(194)` legge `DarwinSystemOptions::ResourceLimits` (`darwin_system.resource_limits`) nei cinque profili Darwin. Ogni `DarwinResourceLimit`, chiave 0..8, contiene due uint64 little-endian agli offset 0/8: 16 byte. Vale `0 <= current <= maximum <= 9223372036854775807`; zero è esplicito e INT64_MAX indica infinito. Nessuna query al host o modifica dei budget FD, VM, storage o esecuzione. `setrlimit`, applicazione dei limiti, segnali e scheduling restano incompleti.

JSON rigoroso ammette al massimo nove chiavi uniche con esattamente `resource`, `current`, `maximum`, interi esatti o stringhe decimali senza segno. Tipi/campi errati, duplicati, chiavi non canoniche e limiti invertiti falliscono prima del caricamento. Array vuoti/omessi sono ignoti; le chiavi configurate non vengono normalizzate.

Solo la syscall usa i 32 bit bassi del selettore e rimuove `_RLIMIT_POSIX_FLAG=0x1000`. Risorse invalide restituiscono EINVAL prima dell’accesso; osservazioni mancanti fermano come unsupported prima dell’output. Output totalmente non scrivibile restituisce EFAULT; coppie parzialmente scrivibili sono rifiutate senza scrivere. Copie complete non allineate o tra pagine cambiano solo 16 byte; errori backend restano di trasporto. La sonda ARM64 nativa ha passato 23 verifiche di valori/selettori e quattro errori separati; il prefisso parziale invariato su questo host non è una garanzia portabile.

```json
{"darwin_system":{"resource_limits":[{"resource":8,"current":256,"maximum":"9223372036854775807"}]}}
```

macOS ARM64 Release: registrati/passati/omessi non eseguiti/falliti 1337 / 845 / 492 / 0; tutti i 123 HVF richiesti eseguiti. Dodici nuovi casi 4K/16K e oracolo SDK; C/CLI 190/190, parser 37/37, carichi nativi 31/31; Python su cinque profili in 71.978 secondi, 71 verifiche API e 49 runner passate. Passano drift SDK, formato, capacità, provenienza e documentazione; i conteggi si sovrappongono. Evidenze congelate in `build-hvf-arm64/resource-limit-observations/` e legate al commit. Primo errore di build da enum di test, tentativi di collegamento/filtro conservati; validazione corretta seriale senza allentare scadenze o controlli. Applicazione dei limiti, permessi, directory dopo mutazione, mappe condivise/EOF, orologi, Mach/thread/dyld e framework restano da fare. iOS fisico, Intel HVF sospeso e CI remota richiedono accettazione separata.

## Osservazioni esplicite del consumo di risorse in sola lettura

getrusage(117) legge DarwinSystemOptions::ResourceUsageSelf / ResourceUsageChildren opzionali e indipendenti nelle cinque configurazioni Darwin. JSON rigoroso: darwin_system.resource_usage.self / .children. Ogni DarwinResourceUsage richiede int64 user_seconds/system_seconds, uint32 user_microseconds/system_microseconds inferiori a1000000 e quattordici counters int64. Interi esatti o stringhe decimali con segno conservano l’intervallo completo; tipi/campi/microsecondi/lunghezze errati falliscono prima del caricamento. Il compagno assente resta ignoto senza bloccare quello fornito; zero esplicito valido.

Una copia completa little-endian di144 byte: timeval a0/16 (secondi8, microsecondi4, padding zero4), counters da32 ciascuno8. Valori/unità Darwin originali, ru_maxrss senza conversione Linux KiB. Dati fissi senza misura host, contabilità, fork/wait, scheduling o applicazione limiti.

Solo32 bit bassi: 0=SELF, -1=CHILDREN; 0x1000 non valido e nessuna rimozione POSIX flag. Non valido EINVAL prima della memoria, assente unsupported prima dell’output. Copia completa disallineata/interpagina conserva guardie; interamente non scrivibile EFAULT, parziale unsupported prima di ogni byte; errori backend restano di trasporto. SDK cattura una volta per selettore, senza confrontare successivi SELF variabili. Probe nativo13 controlli e4 processi di errore indipendenti con limite5s invariato. Qui partial SELF scrive64 byte prima di EFAULT; nessuna garanzia generale del prefisso.

0..13: `ru_maxrss`, `ru_ixrss`, `ru_idrss`, `ru_isrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`, `ru_inblock`, `ru_oublock`, `ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`, `ru_nvcsw`, `ru_nivcsw`.

```json
{"darwin_system":{"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":0,"system_microseconds":0,"counters":["9223372036854775807",0,0,0,0,0,0,0,0,0,0,0,0,0]}}}}
```

[Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html), [XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [SDK layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

macOS ARM64 Release registrati/superati/skip senza esecuzione/falliti 1371/867/504/0; 126 HVF obbligatori eseguiti. File361/361, C/CLI 200/200, JSON 40/40, System/SDK 53/53, native 32/32 superati. Dodici nuovi4K/16K e ammissione/SDK; Python cinque configurazioni 42.865s, API71, runner/reference49 e controlli SDK drift/formato/capacità/provenienza/documentazione superati. Conteggi sovrapposti, revisione indipendente superata. Prove congelate in `build-hvf-arm64/resource-usage-observations/` e associate al commit. Prima asserzione x64 EINVAL corretta per mantenere RDX, ARM64 azzera X1; errore/filtro vuoto conservati. Runtime/scadenze/controlli negativi invariati. Limiti, permessi, directory dopo mutazione, shared maps/EOF, orologi, Mach/thread/dyld, framework incompleti; iOS fisico, Intel HVF sospeso e CI merge separati.

Primo gate:866 superati, un timeout5s del rename iOS ARM64 HVF esistente,504 skip. Stesso binario supera il caso in386ms, poi passa il gate completo seriale. Entrambi conservati; causa ignota, nessuna garanzia di latenza.


## Credenziali esplicite, gruppi e proprietario coerente

Credentials opzionale contiene RealUID/EffectiveUID/RealGID/EffectiveGID e GroupAccessList e GroupMembershipUID opzionali indipendenti. Omissione mantiene quattro getter1000; zero/root esplicito valido, quattro ID scalari/voci gruppo0..INT32_MAX; GroupMembershipUID ha intervallo separato sotto. Gruppi1..16, primo=EffectiveGID, ordine/duplicati preservati; assenza sconosciuta, nessuna inferenza host/EGID. darwin_system.credentials richiede esattamente real_uid/effective_uid/real_gid/effective_gid, groups e group_membership_uid opzionali indipendenti. Interi senza perdita/validatore centrale rifiutano forma/campi/intervallo/numero/primo incoerente prima del caricamento; profili nonDarwin rifiutati.

getuid24/geteuid25/getgid47/getegid43/getgroups79 condividono un proprietario system. Nuovo file regolare usa UID effettivo, device/GID del genitore diretto; rename/FD mantenuti/riuso nome preservano oggetto, stat input immutato. Root non concede scrittura/mutazione/ACL; setuid/setgid/setgroups e processo/sessione assenti.

getgroups capacità=low32 int con segno: negativo primaEINVAL, sconosciutounsupported, zero noto=count senza puntatore, positivo cortoEINVAL prima memoria; sufficiente copia una volta4*count byte little-endian. 0x1000 positivo, nessun POSIXflag rimosso. Guard completi nonallineati/tra pagine; tutto nonscrivibileEFAULT, parzialeunsupported prima byte, backend rimane trasporto. ErrorBSD mantiene x64RDX/azzera ARM64X1; successo azzera entrambi secondari, report conserva argomenti grezzi.


~~~json
{"darwin_system":{"credentials":{"real_uid":101,"effective_uid":202,
 "real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}}}
~~~


[Apple getgroups contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getgroups.2.html), [pinned XNU credential/group ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c).

registrati/superati/skip indisponibili/falliti: 1413/897/516/0; ARM64 HVF 129/129. System/SDK72/72, File365/365, Report43/43, C/CLI210/210; guest8/12skip; native33/33; Python5 82.918s; API71, runner/reference49. SDK drift / clang-format22.1.2 / capabilities23 / provenance / docs11+negative3: OK. Independent source review: OK.

21 value +5 native fault,5s; host16groups, positive short capacity: OK. Native partial32byte thenEFAULT: observation only. Counts overlap. `build-hvf-arm64/credential-observations/`.

Primo run8 fallimenti (5 budget istruzioni,3 scadenze5s),12skip. Scansione due pagine del fixture limitata alla finestra completa132byte sullo stesso confine (64prima,fino64dati,almeno4dopo); owner diretto verifica ancora due pagine intere. Budget/argomenti/controlli negativi invariati, sorgenti/binari/entrambi run conservati. Geometria trasporto corretta prima esecuzione su review; selezione pubblica confronta contenuti. Permessi/ACL,link,osservazioni directory dopo mutazione,shared maps/EOF,orologi,Mach/thread/dyld/framework incompleti; iOS fisico,IntelHVF sospeso,mergeCI separati.

## Tabella dei descrittori dai limiti dichiarati

`DarwinSystemOptions::MaxFilesPerProcess` / `darwin_system.max_files_per_process` dichiara un int opzionale non negativo. Assenza significa ignoto, zero esplicito è valido. `kern.maxfilesperproc` e MIB `[1,29]` leggono gli stessi quattro byte indipendentemente dai limiti di risorse. Parsing intero senza perdita e validazione centrale rifiutano tipi errati, negativi e valori eccessivi prima del caricamento; i profili non Darwin rifiutano la configurazione.

BSD `getdtablesize(89)` richiede il cap e `ResourceLimits[8].Current`, restituendo il minimo. Limita l’intero Current a64 bit prima della conversione int: `0x100000001` con cap=64 restituisce64, anche infinito è limitato. Maximum, host, numero FD attivi e `DescriptorLimit` non sostituiscono osservazioni. Un valore mancante resta unsupported anche se l’altro è zero. Ignora sei argomenti, non accede alla memoria utente e conserva carry/registro secondario BSD. Mach timebase trap89 resta distinto.

Le fasi sysctl restano. Una vera scrittura EUID0 si arresta unsupported dopo nome/MIB e oldlenp, prima di osservazione/output; non-root riceve EPERM. Un nuovo puntatore con lunghezza zero resta lettura. Nessuna applicazione dei limiti o autorità di scrittura viene dedotta. Il workload obbligatorio confronta entrambi i cap e syscall numbers bassi/alti mantenendo `l` /144 byte; rifiuti successivi conservano byte già emessi. Il fixture scalare verifica assenze, zero, Current ampio e indipendenza da DescriptorLimit=3.

```json
{"darwin_system":{"max_files_per_process":64,"resource_limits":[{"resource":8,"current":"4294967297","maximum":"9223372036854775807"}]}}
```

[XNU getdtablesize](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU proc_limitgetcur_nofile](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [XNU MIB constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/sysctl.h).

Verifica: Darwin (registrati/superati/non disponibili/falliti) 1441/913/528/0; ARM64 HVF 132/132; System/SDK 80/80; File 365/365; C/CLI 211/211; ProcessReport 45/45; native 33/33; Python 5 (42.254s); API 71; runner 44 + reference 5. clang-format 22.1.2; capabilities/provenance; docs 286 / locales 10 / negative controls 3.

Cronologia: il primo controllo runner è fallito negli inventari ARM64 e x86-64 perché mancava la registrazione obbligatoria del nuovo metodo scalare. È stata aggiunta mantenendo la regola di uguaglianza di tutti i metodi. Il primo controllo con 129 casi obbligatori e gli errori restano conservati; il controllo finale ARM64 ne richiede 132.

I conteggi si sovrappongono. `build-hvf-arm64/descriptor-table-observations/` conserva baseline reale e hash esatti di sorgenti/binari/log; prove precedenti immutate. Cinque figli usa e getta ARM64 macOS,40 controlli, cinque secondi ciascuno: Current=1048575/cap=245760 restituisce245760; figli Current=0/1/32/245777 restituiscono0/1/32/245760. Limiti del genitore e del sistema immutati. iOS fisico, Intel HVF sospeso e remote merge CI restano separati. Permessi, osservazioni directory dopo mutazione, shared maps/EOF, orologi avanzati, Mach/thread/dyld e framework restano incompleti.

## Osservazioni esplicite del processo

`DarwinSystemOptions::ProcessGroupID`, `SessionID` e `ProcessTainted` sono input opzionali indipendenti: JSON `process_group_id`, `session_id`, `process_tainted`. Gli ID devono essere positivi e non superare INT32_MAX; lo stato contaminato accetta solo Boolean JSON `true`/`false`. Un’omissione resta sconosciuta, mentre `false` esplicito è zero noto. Nessun valore deriva da host, PID1000, credenziali o altre osservazioni.

La chiamata grezza `getpgrp(81)` legge il gruppo; `getpgid(151)` e `getsid(310)` usano i32 bit bassi con segno di `pid_t`, con zero o il PID1000 corrente fisso per il processo stesso. Anche `0xffffffff000003e8` indica sé stesso. Un PID basso negativo restituisce ESRCH3 prima della ricerca dell’osservazione, confermato da sonde native di sola lettura e dall’allocazione/ricerca XNU. Un altro processo positivo sconosciuto, incluso `0x1000`, arresta con UnsupportedService senza inventare ESRCH o applicare maschere di flag. Anche un valore proprio selezionato mancante arresta come non supportato. `getpgrp` e `issetugid(327)` ignorano tutti gli argomenti; le quattro query scalari non accedono alla memoria guest. Restano le regole BSD carry e del secondo registro di ritorno.

`process_tainted` fornisce l’osservazione fissa `P_SUGID`, indipendente dall’uguaglianza degli ID reali/effettivi. Non cambia EUID, proprietà dei file, autorità di scrittura sysctl, entitlement, leadership o stato del terminale. `setpgid`, `setsid` e la mutazione delle credenziali restano non supportati. Il programma originale di sola lettura `process-observations` cattura il proprio PID e verifica bit alti, errori negativi e ritorni; `virtual-process-observations` emette i byte configurati di gruppo/sessione/contaminazione. I casi del modello per altri processi, valori assenti e setter sono esclusi dall’inventario nativo. I riferimenti macOS non certificano dispositivi iOS fisici.

```json
{"darwin_system":{"process_group_id":7,"session_id":16909060,"process_tainted":false}}
```

[XNU process queries](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c), [XNU service numbers](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/syscalls.master), [XNU PID allocation](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_fork.c), [XNU PID lookup](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_proc.c).


## Buffer esplicito di accesso della sessione

`DarwinSystemOptions::LoginNameBytes` dichiara indipendentemente tutti i 255 byte grezzi di sessione (`MAXLOGNAME`). JSON `login_name_hex` richiede esattamente 510 cifre esadecimali ASCII, maiuscole o minuscole. NUL interni e byte non nulli dopo un terminatore sono validi. Assente significa sconosciuto; un record interamente zero è esplicito. Non riempie nomi brevi né deduce dati da host, credenziali, gruppo, sessione o contaminazione. Non concede diritti di accesso o file.

`getlogin(49)` usa i 32 bit bassi senza segno della lunghezza (`u_int`) e copia esattamente min(length,255) byte, senza interpretare stringhe, aggiungere NUL o restituire una dimensione richiesta. Lunghezza zero riesce senza osservazioni o memoria ospite anche con puntatori invalidi; `0xffffffff00000000` seleziona zero. Una richiesta non nulla esige il buffer completo prima dei controlli del destinatario. Un indirizzo totalmente non scrivibile restituisce EFAULT14; intervalli parziali si fermano prima della copia. Gli errori preventivi non pubblicano byte. Gli errori di scrittura del backend passano dal livello esistente senza garanzia generale di rollback. Restano BSD carry e secondo registro, compreso RDX x64 originale in errore.

`setlogin(50)` resta non supportato anche con root esplicito o buffer nullo. Il programma originale in sola lettura `login-buffer` controlla lunghezze complete, prefissi, byte adiacenti, puntatori con lunghezza zero ed EFAULT; `virtual-login-buffer` emette i 255 byte dichiarati. Valori mancanti e setter sono casi solo del modello. La referenza macOS ARM64 non convalida iOS fisico o Intel nativo. L’esempio dichiara 255 zeri senza inferire un nome vuoto.

Il verificatore guest inizializza tutti i 265 byte di uscita prima di ogni copia e controlla tre intervalli crescenti e disgiunti: [0,3), [3,3+n), [3+n,265), dove n resta la stessa lunghezza di copia limitata. Il primo e l’ultimo controllano ogni byte di guardia; quello centrale confronta ogni byte copiato con lo snapshot originale. Restano coperte le 12 lunghezze complete, 21 chiamate a lunghezza zero, 42 chiamate EFAULT, verifiche carry/registro secondario e percorsi grezzi/virtuali con i limiti esistenti. Si riduce il lavoro del verificatore mantenendo copertura e ordine del primo errore; le prestazioni di produzione richiedono misure separate.

```json
{"darwin_system":{"login_name_hex":"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}}
```

[XNU getlogin / MAXLOGNAME](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c#L1561).


## Priorità esplicita del processo corrente

`DarwinSystemOptions::ProcessNice` / JSON `darwin_system.nice` dichiara un nice fisso indipendente con segno da -20 a 20. Assente significa sconosciuto; zero e -1 espliciti sono noti. Il parser intero senza perdita accetta numeri interi e stringhe decimali intere; tipi errati, frazioni, stringhe esponenziali, spazi e valori fuori intervallo falliscono prima del caricamento. JSON numerico esattamente intero resta valido. I profili non Darwin rifiutano il campo. Non deriva da host, credenziali, gruppo/sessione, contaminazione, login, risorse o CPU; non cambia scheduling, permessi o budget.

`getpriority(100)` usa i32 bit bassi del selettore int e del destinatario `id_t` senza segno. Un destinatario oltre INT32_MAX restituisce prima EINVAL22. Selettori sconosciuti, inclusi GPU5/0x1000, e thread3 con destinatario basso non nullo danno EINVAL prima delle osservazioni. `PRIO_PROCESS`0 accetta zero o PID1000 corrente, poi richiede nice. Altri PID positivi restano non supportati senza ESRCH inventato. Gruppo1, utente2, thread3/destinatario0 ed estensioni4,6,7,8 restano non supportati anche con valori correlati. Solo bit alti del destinatario thread indicano stato ignoto, non EINVAL. Il risultato estende il segno a64 bit: -1 è UINT64_MAX con successo e carry azzerato. Nessuna memoria ospite, argomenti inutilizzati ignorati; BSD conserva RDX x64 in errore e lo azzera in successo, ARM64 X1 è zero in entrambi.

`setpriority(96)` resta non supportato anche con root/nice espliciti. L’originale in sola lettura `process-priority` verifica destinatari propri, errori certi e transizioni successo/errore/successo; `virtual-process-priority` emette gli otto byte con segno dichiarati. Altri processi, aggregati, assenze e setter sono casi del modello. La nuova sonda ARM64 macOS ha superato191 controlli con nice0; il campione non negativo non dimostra estensione negativa hardware, coperta dalle dichiarazioni firmate della versione fissata e da limiti indipendenti del modello. iOS fisico e Intel nativo restano separati.

```json
{"darwin_system":{"nice":-1}}
```

[XNU getpriority](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_resource.c), [XNU signed INT entry](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/dev/arm/systemcalls.c).


## Apertura senza seguire link e controllo della directory

Le entrate ordinarie e nocancel `open` / `openat` accettano O_NOFOLLOW=0x100 oppure O_NOFOLLOW_ANY=0x20000000 nei32 bit bassi senza segno. Questi flag di ricerca non compaiono in F_GETFL e non modificano accesso, append, troncamento, creazione o CLOEXEC del singolo FD. Insieme danno EINVAL22 dopo l’ammissione della capacità FD, prima di importare il percorso completo; una tabella piena dà prima EMFILE24. I flag sconosciuti restano non supportati.

Per dirfd diverso da AT_FDCWD, `openat` importa solo il primo byte prima di modalità di accesso e capacità FD. Un byte inaccessibile dà EFAULT14. Un prefisso relativo, anche NUL, controlla prima l’oggetto directory trattenuto: FD sconosciuto EBADF9, file regolare ENOTDIR20, tipo vnode di stream ignoto non supportato. `/` salta dirfd; poi la sequenza open esistente importa l’intero percorso. `open` ordinario e AT_FDCWD omettono questa fase, gli altri servizi nameiat verificano, dopo flag e dimensioni, il primo byte e il dirfd relativo prima della stringa completa. I rifiuti non consumano FD o nuovi inode; gli errori di trasporto si propagano senza mutazioni del namespace.

L’originale `file-access` usa FD relativi di directory per NOFOLLOW_ANY, evitando alias nativi `/var` e `/tmp`. I test diretti distinguono primo byte e successivi, slash/NUL, limiti utente/pagina, FD esauriti e directory rimosse trattenute. La sonda grezza ARM64 macOS ha superato30 casi con il limite invariato di cinque secondi; l’ultima riga assoluta con tabella piena usa davvero un FD di directory valido nonostante la vecchia etichetta. iOS fisico e Intel nativo restano da verificare separatamente.

[XNU open1at / open1](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU vn_open_auth](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_vnops.c), [XNU open flags / FMASK](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/sys/fcntl.h).

## Link simbolici iniziali fissi

I nomi iniziali seguenti sono protetti per impostazione predefinita; l’ultima sezione descrive i permessi espliciti di modifica. `DarwinFileOptions::SymbolicLinks` / `darwin_files.symbolic_links` dichiarano `path` assoluto canonico, `target_hex` esadecimale grezzo e `metadata` opzionali del link stesso. Destinazioni di1..1023 byte nonNUL conservano nonUTF-8, barre ripetute e punti; possono mancare. `files` resta richiesto anche vuoto. Collisioni e discendenti dichiarati sotto link sono rifiutati. Percorso/NUL/destinazione condividono256 voci/16 MiB. S_IFLNK, size=lunghezza, DT_LNK=10 e inode coerente richiesti; CWD deve essere directory reale.

I link si espandono prima dei punti: relativi dal genitore effettivo, assoluti dalla radice guest. Ogni ripresa rivaluta barre finali senza ereditare quelle consumate. Consentite32 espansioni, la33 dàELOOP62; destinazione+suffisso+NUL oltre1024 byte dàENAMETOOLONG63. FD/CWD/F_GETPATH/mmap mantengono l’oggetto risolto.

stat64/open/access/truncate/chdir seguono il link finale; lstat64/readlink lo mantengono. O_NOFOLLOW dàELOOP, conO_DIRECTORY primaENOTDIR20. O_NOFOLLOW_ANY rifiuta espansioni richieste. O_CREAT|O_EXCL dàEEXIST17 per link finale esistente, anche rotto/ciclico. AT0x20/0x800 mantengono il finale;0x800 rifiuta anche espansioni intermedie/barre finali. Combinabili. AT_FDONLY ignora il percorso dopo verifica flag.

readlink(58) usa count firmato basso32; readlinkat(473) size_t completo; ritornoint. OltreINT32_MAX:EINVAL22 prima di percorso/FD. Copia min(count,lunghezza), senzaNUL, verificando solo il prefisso reale. Lunghezza0 verifica percorso/tipo poi ignora output. Non link:EINVAL22; nessun byte scrivibile:EFAULT14; prefisso parziale: arresto prima della copia. Errori trasporto/budget si propagano.

I nomi dei link e i byte grezzi delle destinazioni restano fissi. MutableDirectories non può essere la radice o un antenato per segmenti di un link fisso; /work non contiene /workspace/link. Domini mutabili separati possono contenere destinazioni create, spostate, eliminate o sostituite durante l’esecuzione. Restano i controlli di genitori, mount, alias, flag, supporto SWAP e creazione; i nuovi inode devono superare tutti quelli di metadati/snapshot, inclusi i link protetti. WritableFiles/MutationPolicies fissi possono modificare il file risolto. Unlink/rename del link conservato si fermano prima di effetti. La creazione durante l’esecuzione è descritta sotto; hard link,ACL e cataloghi iniziali mutabili restano esclusi. Sonda ARM64 macOS:189 osservazioni/115 buffer completi nei5s originali; non prova iOS fisico/Intel HVF/OS completo.

I60 controlli ARM64 macOS DELETE/RENAME aggiuntivi registrano buffer stat completi, namespace prima/dopo e identità FD/CWD entro i5s originali. Le barre finali possono espandere un link fisso e modificarne la destinazione; NOFOLLOW_ANY rifiuta l’espansione necessaria con ELOOP. symbolic-link-mutations senza SDK verifica creazione, destinazioni mancanti, spostamento/rimozione/sostituzione, genitori CWD conservati e tutti i10 byte originali del file prima della chiusura dei FD, verificando ancora i10 byte del mapping dopo la chiusura. Non prova iOS fisico o Intel nativo.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"symbolic_links":[{"path":"/link","target_hex":"64617461"}],"working_directory":"/"}}
```

[XNU namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c), [XNU readlink / AT](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU open authorization](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_subr.c).

## Creazione di link simbolici durante l’esecuzione

`symlink(57)` e `symlinkat(474)` creano link locali al processo nelle directory mutabili autorizzate e restituiscono int. Dirfd usa i32 bit bassi; il nome assoluto ignora il FD. La destinazione del link si importa prima del nuovo nome fino al primo NUL:0..1023 byte grezzi, anche vuoti, non UTF8, punti e separatori ripetuti.1024 byte senza NUL danno ENAMETOOLONG63; un errore precedente dà EFAULT14. Le destinazioni JSON iniziali richiedono ancora1..1023 byte.

La tabella corrente conserva nome effettivo, genitore e byte. Un terminale esistente dà EEXIST17. Le barre finali consumate possono seguire un link pendente e creare presso il suo obiettivo, lasciando invariato il vecchio link. Espandere una destinazione vuota dà ENOENT2. Readlink vuoto restituisce0 senza toccare il puntatore di uscita anche con capacità positiva; count/percorso/tipo sono verificati prima.

Nome/NUL e obiettivo sono conteggiati una volta nel limite comune256 voci/16 MiB. Un rifiuto non cambia nodi, genitori, FD o inode di creazione file. I metadati completi nuovi restano ignoti, senza ereditare CreationPolicy del file o vecchie osservazioni del nome riusato. Stat/snapshot del genitore diventa ignoto dopo creazione. FD/CWD/mapping mantengono gli oggetti dopo rimozione/sostituzione dell’obiettivo. Rmdir e sostituzione directory rilevano figli link. Movimento/SWAP con link iniziali protetti in uno dei lati spostati si ferma prima di effetti. sostituzioni link/directory, hard link, ACL e cataloghi iniziali mutabili restano esclusi; un alias non trasferisce autorità al genitore effettivo.

I150 casi ARM64 macOS conservano quattro errori dell’osservatore; dieci controlli separati verificano il nuovo oggetto effettivo e i limiti vuoti. Il programma senza SDK `symbolic-link-creation` verifica entrambi gli ingressi, byte/buffer, genitori, sostituzione e tutti i dieci byte di FD/mapping precedenti. Non dimostra iOS fisico, Intel nativo o compatibilità OS completa.

[XNU symlink / symlinkat](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU empty-link expansion](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c).

## Rimozione dei link simbolici creati durante l’esecuzione

`unlink(10)` e `unlinkat(472)` rimuovono i link creati durante l’esecuzione se il genitore effettivo autorizza modifiche; i link iniziali fissi restano protetti. Il solo `AT_SYMLINK_NOFOLLOW_ANY` conserva il link finale e ne consente la rimozione anche con obiettivo assente, ciclico o vuoto. Un’espansione intermedia o dovuta alla barra finale restituisce ELOOP62. Senza flag, rimuovere `a/` in `a → b → target` elimina soltanto `b`, mantenendo `a` e l’obiettivo finale. L’ordine degli errori di flag, percorso e dirfd resta invariato.

Il successo restituisce una voce dinamica e il costo corrente di percorso/NUL/obiettivo una sola volta, invalidando solo le osservazioni complete stat/enumerazione del genitore reale. Non servono FD libero o inode di creazione. Dati, politica di modifica, descrizioni aperte, cursori condivisi, CWD e mapping mantengono gli oggetti. Il riuso del nome non ripristina il vecchio link; i rifiuti non cambiano stato o budget. I metadati completi dei link creati durante l’esecuzione restano sconosciuti. sostituzioni link/directory e movimento/SWAP di sottoalberi contenenti link iniziali protetti restano esclusi.

Le 40 osservazioni ARM64 macOS indipendenti registrano 28 rimozioni, 12 errori, 17 ENOENT al secondo tentativo e conservazione di FD/CWD/mapping privati nel limite invariato di cinque secondi. Non certificano guest, iOS fisico o Intel nativo. `symbolic-link-unlink` e i casi API pubblici verificano separatamente questi confini.

## Rinominare link simbolici creati durante l’esecuzione

`rename(128)`, `renameat(465)` e `renameatx_np(488)` supportano spostamenti ordinari e `RENAME_EXCL=4`: link a nome libero, link/link, link/file e file/link. Entrambi i genitori reali devono autorizzare modifiche nello stesso dominio di mount accertato; i link iniziali restano immutabili. Il risolutore condiviso sceglie i nodi effettivi. Conserva byte di destinazioni vuote, mancanti, cicliche e non UTF-8; i target relativi si risolvono dal nuovo genitore. `RENAME_NOFOLLOW_ANY=16` da solo mantiene il link finale, mentre l’espansione intermedia necessaria restituisce ELOOP62. Destinazioni EXCL esistenti distinte danno EEXIST17; EXCL sullo stesso oggetto resta escluso senza contratto di distinzione maiuscole/minuscole.

Il nuovo costo percorso/NUL viene riservato prima della pubblicazione, con limite1024 byte incluso NUL. Sostituire un link di esecuzione restituisce una volta tutto il costo corrente percorso/NUL/target, indipendentemente dai FD o mapping del referente. Contenuti/percorsi dinamici dei file sostituiti restano conteggiati finché ogni descrizione e lease di mapping è liberato; solo un file recuperabile subito fornisce credito di prenotazione. Non servono voce aggiuntiva, FD o inode di creazione file. Il rifiuto mantiene entrambi i nodi; il successo invalida le osservazioni complete stat/enumerazione dei genitori reali. I metadati completi del link restano ignoti. sostituzioni link/directory, hard link e movimento/SWAP di sottoalberi con link iniziali protetti restano esclusi.

I19 controlli indipendenti ARM64 macOS registrano14 successi e5 errori EEXIST/ELOOP con scadenze originali di5 secondi, inode/byte del link, nuovo legame relativo e FD/dup/cursori/CWD/mapping privati mantenuti. `symbolic-link-rename` senza SDK e verifiche pubbliche SDK/CLI sono separati. La sola osservazione nativa non prova iOS fisico, Intel nativo o compatibilità OS completa.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Scambiare link simbolici creati durante l’esecuzione

`renameatx_np(488)` supporta link/link, link/file e file/link con `RENAME_SWAP=2` se entrambi i genitori reali autorizzano modifica e SWAP nello stesso dominio di mount accertato. Lo stesso nome riesce senza effetti né ulteriore dichiarazione SWAP. Il target assente dà ENOENT2; `SWAP|NOFOLLOW_ANY=18` mantiene i link finali e rifiuta espansioni intermedie con ELOOP62; `SWAP|EXCL=6` dà EINVAL22 prima delle letture dei percorsi. I byte restano immutati e i target relativi usano entrambi i nuovi genitori.

Si riservano entrambi i percorsi/NUL prima della pubblicazione. Entrambi gli oggetti restano collegati, senza credito di sostituzione da dati o lease dei mapping. Il file iniziale acquisisce un costo dinamico al primo scambio e lo riusa al ritorno. Non consuma voce, FD o inode di creazione. Identità, nlink, descrizioni, cursori, CWD e mapping sono mantenuti; le osservazioni complete dei genitori diventano ignote. Metadati completi dei link, coppie directory/link e movimento/SWAP di sottoalberi con link iniziali protetti restano esclusi. I 22 controlli ARM64 macOS registrano 14 scambi, due successi sullo stesso oggetto e sei errori entro i cinque secondi originali. `symbolic-link-rename` e SDK/CLI/Python verificano scambi ed errori, senza provare iOS fisico, Intel nativo o OS completo.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Spostare alberi con link creati durante l’esecuzione

Gli spostamenti ordinari/EXCL e SWAP di directory includono i link discendenti di runtime, anche nello scambio directory/file. La transazione verifica tutti i nomi di directory, file e link e il limite1024 byte NUL incluso, poi estrae tutte e tre le tabelle prima di pubblicare. I byte del target restano invariati e contabilizzati; cambia solo il percorso/NUL corrente. SWAP non offre credito di sostituzione né consuma nuova voce, FD o inode.

I link conservano i genitori reali spostati e risolvono target relativi dai nuovi percorsi. Descrizioni, cursori, CWD, nodi rimossi e lease dei mapping mantengono i propri oggetti. Link iniziali protetti e relativi alberi, coppie radice directory/link, metadati completi dei link, hard link e ACL restano esclusi.25 controlli ARM64 macOS registrano cinque spostamenti, dieci scambi, due successi senza effetti e otto rifiuti senza cambiare nomi nei cinque secondi originali. I controlli tra genitori verificano byte intatti e nuova risoluzione relativa; `symbolic-link-rename` copre C++/SDK/CLI/Python senza provare iOS fisico, Intel nativo o OS completo.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Metadati dei nuovi link e delle directory

L’opzione `creation_policy.namespace_policy` (C++ `DarwinFileCreationPolicy::Namespace`) aggiunge ai cinque campi obbligatori un oggetto rigoroso con `symbolic_link_allocation_unit`, `directory_entry_size`, `directory_blocks`. L’unità link è una potenza di due512..16 MiB, la dimensione voce positiva fino16 MiB, i blocchi uint64 finoINT64_MAX; le stringhe decimali restano esatte. Metadati dei genitori, umask iniziale e nuovi inode sono ancora necessari. Senza estensione rimangono i limiti precedenti di metadati sconosciuti e consumo inode solo per file regolari.

Inserimenti riusciti di file,symlink,mkdir condividono una sequenza, esaurita definitivamente aUINT64_MAX; errori e nomi esistenti non consumano inode. Device/GID appartengono al genitore reale, UID all’identità effettiva ospite. Il link usa S_IFLNK con `0777 & ~umask`, nlink1, size dei byte grezzi anche vuoti/nonUTF-8 e blocchi512byte arrotondati all’unità dichiarata. La directory usa S_IFDIR con `mode & 0777 & ~umask`, nlink2 più tutti i nomi diretti collegati, size=nlink per dimensione voce e blocks fissi, anche per directory vuote rimosse ma mantenute. È un contratto virtuale esplicito, non una regola APFS dedotta.

I tempi iniziali usano creation_time. Cambiare nomi figli aggiorna mtime/ctime dei genitori creati; gli spostamenti diretti aggiornano solo ctime con mutation_time. Gli spostamenti degli antenati preservano i discendenti. I record completi restano sull’oggetto durante dup/CWD/sostituzione/SWAP/rimozione/riuso. La sola estensione di creazione non conserva lo stat iniziale completo o gli snapshot fissi; le politiche separate sotto forniscono stat ed enumerazione corrente. ACL, modifiche di link iniziali senza permesso individuale e transazioni generali radice directory/link restano assenti. `created-namespace-metadata` controlla osservazioni native comuni; `virtual-created-namespace-metadata` confronta144byte costanti completi viaC++/SDK/CLI/Python. Verifiche mirate superate:11 modello/ammissione,1JSON rigoroso,43 workload nativi nei5secondi originali,8 ospiti（12 saltati per backend indisponibile,3HVF richiesti eseguiti）e10 casi pubblici. Conservati errore della tabella puntatori e correzione staticaARM64. iOS fisico e Intel nativo restano non verificati.

```json
{"namespace_policy":{"symbolic_link_allocation_unit":512,"directory_entry_size":32,"directory_blocks":7}}
```

## Enumerazione virtuale dopo modifiche dei nomi

Il campo facoltativo `directories[].enumeration_policy` abilita viste correnti tramite `getdirentries 64`; C++ usa `DarwinFileOptions::DirectoryEnumerationPolicies` e `DarwinDirectoryEnumerationPolicy`. L'oggetto rigoroso richiede `minimum_buffer_size`, `initial_minimum_buffer_size` e `seek_offset`: il primo positivo, entrambi i minimi fino a 128 MiB e seek_offset uint 64 senza perdita. La directory iniziale richiede propri metadati con inode non nullo; non può avere anche `contents` immutabile. Ogni riferimento conta percorso+NUL una volta. mkdir eredita la politica del genitore reale; i discendenti esistenti conservano le proprie dichiarazioni. Enumerazione e autorità di modifica sono indipendenti.

L'ordine virtuale è `.`, `..`, poi nomi collegati direttamente ordinati per byte senza segno. Gli inode appartengono a oggetti osservati o creati; `..` segue il genitore reale mantenuto. Identità figlio/genitore assente o nulla arresta prima dell'output. Solo inode resta utilizzabile dopo invalidazione del completo stat iniziale. I cookie sono ordinali locali da 1 e d_seekoff la costante dichiarata. dup condivide il cursore, open è indipendente. Modifiche confermate e spostamenti diretti richiedono riavvolgimento a zero; rifiuti e operazioni senza effetti lo conservano. Spostare un antenato conserva cursori discendenti. Esaurire le versioni arresta esplicitamente. Directory vuote eliminate e trattenute emettono zero record anche dopo riuso del nome.

Questo paragrafo documenta la preparazione con la sola enumerazione, senza la politica stat iniziale seguente. Restano comuni codifica, record interi, minimi, limite del payload, suffisso EOF ed effetti dati/cursore/posizione/flag. Stat completo del genitore iniziale e snapshot fissi restano invalidati; omettere la politica mantiene i rifiuti precedenti. Non riproduce generazioni APFS. La preparazione ARM 64 indipendente ha registrato 25 eventi/16 viste entro 5 secondi invariati. `directory-enumeration-mutations` verifica identità native e oggetti trattenuti; `virtual-directory-enumeration` confronta 160 byte letterali tramite guest, C/CLI e Python. Intel nativo, iOS fisico, ACL, hard link e OS/framework completi restano non verificati o non supportati.

## Mutazione esplicita dello stat delle directory iniziali

L’opzione `directories[].mutation_policy` conserva lo stat completo di una directory iniziale ammessa dopo modifiche dello spazio dei nomi. C++ usa `DarwinDirectoryMutationPolicy` e `DarwinFileOptions::DirectoryMutationPolicies`. L’oggetto rigoroso contiene esattamente `directory_entry_size` e `mutation_time`. La dimensione è positiva e al massimo 16 MiB; il tempo usa secondi con segno a 64 bit senza perdita e nanosecondi in [0,1000000000). Servono metadata complete proprie con inode non nullo. Ogni riferimento conta una volta percorso e NUL; la dimensione proiettata non alloca byte di file. La politica non concede autorità di modifica o permessi e non richiede politiche di creazione o enumerazione.

Il record osservato completo resta invariato fino alla prima modifica realmente confermata, che copia i campi scalari nell’oggetto senza allocazione. Le modifiche ai nomi figli impostano nlink a due più tutti i nomi direttamente collegati di ogni tipo, size a nlink per directory_entry_size e mtime/ctime a mutation_time. Spostamenti diretti, SWAP o rimozione cambiano solo ctime; spostare un antenato conserva i discendenti. Rifiuti e operazioni senza effetto sullo stesso oggetto non cambiano nulla. Device, inode, mode, proprietario, blocks, dimensione del blocco, flags, generation, atime e birthtime conservano i valori osservati. Sono regole virtuali dichiarate, non deduzioni su allocazione, numero di link o orologio APFS.

Record e politica seguono l’oggetto originale attraverso dup, FD conservati, CWD, sostituzione, rimozione e riuso del nome. Un nuovo mkdir usa la politica di creazione separata, se fornita, e non eredita lo stat iniziale dal genitore o dal vecchio oggetto omonimo. Gli snapshot immutabili diventano ancora sconosciuti dopo modifiche; una enumeration_policy indipendente può fornire viste correnti. Senza questa politica stat, le metadata complete delle directory iniziali modificate restano sconosciute.

Una preparazione nativa ARM64 indipendente ha conservato 27 viste stat grezze protette e 15 operazioni entro i cinque secondi invariati, verificando ABI SDK di 144 byte e identità conservata senza generalizzare tempi o allocazione. Il test originale `initial-directory-metadata` verifica osservazioni native comuni; `virtual-initial-directory-metadata` confronta un record letterale completo di 144 byte tramite guest, C/CLI e Python. I modelli coprono anche prima rimozione, assenza di autorità, omissione di creazione e indipendenza snapshot/enumerazione. Intel nativo, iOS fisico, ACL, hard link, modifiche di link iniziali senza permesso individuale e OS/framework completi restano non verificati o non supportati.

```json
{"mutation_policy":{"directory_entry_size":17,"mutation_time":{"seconds":-11,"nanoseconds":321}}}
```

## Mutazione esplicita dei link simbolici iniziali

`symbolic_links[].mutable:true` e C++ `DarwinFileOptions::MutableSymbolicLinks` autorizzano il nome dell’oggetto iniziale, con padre reale mutabile separatamente. Flags noti, mode speciali, link_count≠1, alias e dispositivi contraddittori sono rifiutati. Senza dichiarazione il nome resta protetto ed escluso dai domini antenati mutabili. Ogni permesso riserva un riferimento path/NUL fisso, senza voce o inode di creazione. I target iniziali restano immutabili.

unlink, rename ordinario/EXCL, SWAP foglia link/file/link e transazioni dei sottoalberi dichiarati mantengono condizioni padre reale, mount e SWAP. Target relativi si risolvono dal nuovo padre; FD/dup, CWD e lease di mapping conservano i referenti. I costi iniziali restano riservati dopo rimozione/sostituzione; il primo cambio riserva un nome dinamico separato. Si restituiscono solo costi dinamici posseduti; solo nuovi link sono voci dinamiche. Rifiuti e no-op non pubblicano cambiamenti.

`symbolic_links[].mutation_policy` usa `DarwinSymbolicLinkMutationPolicy` e `DarwinFileOptions::SymbolicLinkMutationPolicies`. L’unico campo rigoroso `mutation_time` richiede secondi signed 64-bit senza perdita e nanosecondi [0, 1000000000), permesso e osservazioni complete con inode non zero. Il primo move/SWAP diretto copia scalari senza allocazione e modifica soltanto ctime. Blocks e altri campi, movimenti antenati, rifiuti e no-op restano conservati. Senza policy stat completo diventa sconosciuto, ma inode di enumerazione e contraddizioni note di dispositivo restano. Il riferimento path/NUL è fisso; nomi riusati e nuovi symlink non ereditano record/policy iniziali e usano la namespace policy di creazione distinta.

La preparazione ARM64 privata conserva 14 viste guarded raw-stat, 11 operazioni, ABI SDK indipendente di 144 byte, limiti compile120s/native5s/drain1s/reap1s e pulizia. `mutable-initial-links` verifica identità, risoluzione e referenti nativi; `virtual-mutable-initial-links` verifica stat completo in cinque guest, C/CLI e Python. Modelli coprono due pagine, SWAP dei sottoalberi, costi precisi ed esaurimento entry/inode. Intel e iOS fisico non sono validati; hard link, ACL, OS/runtime/framework completi restano assenti.

```json
{"mutable":true,"mutation_policy":{"mutation_time":{"seconds":-13,"nanoseconds":456}}}
```

## Transazioni radice di directory e link simbolico

`renameatx_np(RENAME_SWAP)` scambia directory reale e link in entrambi gli ordini, inclusi sottoalberi non vuoti. La directory iniziale richiede `exchangeable`, il link iniziale `mutable` e i genitori reali autorizzazioni di modifica/SWAP nello stesso mount accertato. I discendenti iniziali protetti restano esclusi. Con le autorizzazioni ordinarie esistenti, directory→link restituisce ENOTDIR20, il contrario EISDIR21 e EXCL verso nomi distinti esistenti EEXIST17. Entrambi i cicli directory/link discendente danno EINVAL22 prima degli effetti. Si scambia il link stesso: una nuova autoreferenza può risultare da un successo e seguirla dopo restituisce ELOOP62.

La transazione delle tre tabelle verifica radici, discendenti collegati o trattenuti, percorsi completi e spazio dinamico prima della pubblicazione. Nomi/target/riferimenti iniziali, contenuti e lease dei mapping non offrono credito SWAP. La prima nuova chiave riserva un percorso/NUL dinamico indipendente; ripetere sostituisce una volta il costo precedente. Nessuna voce, FD o inode di creazione aggiuntivi. L’appartenenza segue i genitori reali, senza annettere vecchi orfani solo per nomi riutilizzati. Target grezzi invariati, risoluzione relativa sul nuovo genitore; FD/dup, cursori, CWD, referenti e mapping sopravvivono. Le radici dirette applicano proprie politiche stat solo a ctime, gli antenati preservano i discendenti. Senza politica stat completo è ignoto, ma inode resta per l’enumerazione corrente; le versioni seguono il contratto di riavvolgimento a zero esistente. Nessun nuovo campo JSON o permesso.

La preparazione ARM64 originale conserva 36 viste stat protette di 144 byte, 16 raw rename, compile120s/native5s/drain1s/reap1s e recupero/pulizia confermati. `directory-link-roots` e `virtual-directory-link-roots` verificano identità e stat completo costante su cinque guest, C/CLI e Python. I modelli di due pagine coprono budget esatti, overflow senza apertura, orfani ed esaurimento FD/voce/inode. Questa sezione estende le esclusioni precedenti entro tali permessi. Intel nativo, iOS fisico, hard link, ACL e OS/runtime/framework completi restano attività separate.

## Query pathconf fisse del kernel

pathconf(191) / fpathconf(192) supportano le costanti vnode XNU:15/16/17→1,19/25→0,20/22/23→4096,21→65536,24→255. Le osservazioni su link, allocazione, I/O e trasferimenti non abilitano esecuzione asincrona o autorizzazione; non derivano da pagine o limiti del catalogo.

La risoluzione completa del percorso con link o ricerca FD precede il selettore low32. CWD ed EFAULT/ENOENT/ENOTDIR/ELOOP restano invariati; FD assente/chiuso dà EBADF. Gli oggetti file/directory mantenuti restano interrogabili senza stat completo dopo dup, spostamento, eliminazione e riuso dei nomi. Il tipo nativo dei flussi catturati resta sconosciuto. Si conserva BSD int/carry/registro secondario senza copiare output, modificare cursori, metadati/enumerazione o riservare voci/FD/inode. NAME_MAX, proprietà della distinzione maiuscole e selettori sconosciuti restano non supportati dopo ricerca, senza valori host o EINVAL ipotizzati.

kernel-pathconf, kernel-pathconf-values e kernel-pathconf-unsupported verificano semantica comune,80 byte letterali indipendenti e arresto con output precedente preservato via guest/C/CLI/Python. Preparazione ARM64 privata:250 query,249 confronti SDK,0.262s con limite5s invariato. Intel nativo/iOS fisico/ACL/hard link/runtime e framework completi restano non verificati o implementati.

[XNU vn_pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU fpathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_descrip.c).

`pathconf`: `file/` → ENOTDIR; `alias/`, `alias//` → 1 (selector15); `alias/.`, `alias/child` → ENOTDIR.

## Liste di attributi comuni fissi

getattrlist(220), fgetattrlist(228) e getattrlistat(476) interrogano undici campi comuni del catalogo esplicito: dispositivo, tipo, quattro tempi, proprietario/gruppo, modo completo, flag e ID. Condividono con stat64 la validità del record completo; osservazioni mancanti o invalidate restano ignote. Tipo e selezione vuota non richiedono stat. NAME di radice/mount, volume, maschere directory/file/fork, ACL e opzioni ignote restano esplicitamente non supportati anche con maschera restituita; nessun dato host o nome di mount viene dedotto.

Percorso/at importano24 byte prima della ricerca; FD verifica prima low32 FD e tipo nativo. reserved è ignorato. CWD/FD relativo/link condividono gli errori nativi prima di dimensione/bitmap. Little-endian, allineamento4, st_mode completo e secondi con segno; con maschera120 byte, senza100. Un buffer corto riceve solo il prefisso richiesto ma riporta la dimensione completa. Accesso parziale si ferma prima della copia; dimensione fuori signed-uio dà EINVAL dopo richiesta valida supportata. Cursori, metadati, enumerazione e budget voce/FD/inode restano invariati; durata dup/rimozione/riuso nome preservata.

Tre modalità verificano comportamento comune, byte configurati indipendenti e ATTR_CMN_EXTENDED_SECURITY non supportato con output conservato via guest/C/CLI/Python. Preparazione ARM64 privata:187 query raw/176 confronti SDK percorso-FD, native5s invariato. SDK15.5 non dichiara getattrlistat; raw476 separato. Nuovi guest/Python:5,000,000us/quantum1024; test pubblici esistenti:10s. Intel nativo, iOS fisico, dati FS, hard link, permessi/ACL e runtime/framework completi restano incompleti o non verificati.

```text
ATTR_CMN_RETURNED_ATTRS=0x80000000
FSOPT_NOFOLLOW=1, FSOPT_REPORT_FULLSIZE=4
FSOPT_PACK_INVAL_ATTRS=8 (requires ATTR_CMN_RETURNED_ATTRS)
FSOPT_NOFOLLOW_ANY=0x800
common-attributes
common-attributes-values
common-attributes-unsupported
```

[XNU attrlist](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_attrlist.c), [XNU attr.h](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h).


## Lettura esplicita degli attributi estesi

getxattr(234), fgetxattr(235), listxattr(240) e flistxattr(241) leggono osservazioni ordinarie complete e ordinate da darwin_files. File, directory e link possono dichiarare extended_attributes come array rigoroso di {name,bytes_hex}. L’omissione resta sconosciuta; [] dichiara una lista vuota nota. I nomi UTF-8 di 1..127 byte ammettono slash, i valori sono byte opachi, i duplicati sono rifiutati e l’ordine è conservato. Massimo 4096 attributi. Nomi, NUL e valori rientrano nel budget esistente di 16 MiB senza contare due volte il percorso esistente.

Le osservazioni appartengono agli oggetti dopo dup, spostamenti, rimozione, CWD e riuso dei nomi, indipendentemente da stat completo ed enumerazione. I nuovi oggetti restano sconosciuti. Scritture, troncamento riuscito e fallimenti ambigui di copie non vuote invalidano gli attributi; un rifiuto prima della copia li conserva. Le query mantengono cursori, input, metadati e budget di oggetti/FD/inode.

ABI: FD/options/position low32, size full64 e BSD user_ssize_t/carry/secondary. NULL ignora position. Per valori non vuoti, percorso nonNULL size0 restituisce ERANGE; FD size0 interroga la lunghezza. Solo UINT32_MAX/UINT64_MAX del get per percorso sono query storiche; FD limita a INT32_MAX. Liste positive corte pubblicano nomi interi prima di ERANGE; lunghezze negative full64 nonNULL restituiscono ERANGE per liste non vuote. Nessuna lista vuota nativa è stata verificata: quel caso negativo con vuoto dichiarato resta UnsupportedService. L’output inaccessibile si arresta prima della copia. NOFOLLOW1 e NOFOLLOW_ANY64 sono indipendenti;8/16 sono rifiutati prima della ricerca, FD1/64 prima di FD/nome. CREATE2/REPLACE4 sono ignorati; SHOWCOMPRESSION32 e bit ignoti restano non supportati. com.apple.system.*, ResourceFork, FinderInfo, decmpfs, impostazione/rimozione, permessi/ACL e deduzioni del filesystem sono esclusi.

extended-attributes / extended-attributes-values / extended-attributes-unsupported verificano programma nativo condiviso, byte virtuali e arresto ignoto conservando l’output. Invariati: guest/Python5,000,000us/quantum1024, API pubblica10s, nativo5s. Preparazione privata ARM64:320 confronti raw/SDK con tutti288 byte di guardia e carry/secondary uguali. com.apple.provenance automatico è un’osservazione, non un vuoto predefinito. Intel nativo/iOS fisico restano non verificati; dyld, Mach IPC, Objective-C/Swift e framework completi restano incompleti.

Sources: [XNU syscall ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU xattr calls](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ordinary attributes](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_xattr.c), [XNU xattr definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Original code/probes; Apple implementation is not copied.

## Nomi di oggetti limitati

ATTR_CMN_NAME=1 è ammesso tramite getattrlist220/fgetattrlist228/getattrlistat476 per oggetti non radice con nome univoco nel catalogo esplicito. Il nome finale è UTF-8 valido di1..255 byte. Il percorso reale condiviso con F_GETPATH conserva l’ultima grafia collegata dopo dup, CWD, spostamenti, SWAP, rimozione e riuso; gli alias del chiamante non la sostituiscono. Nome e tipo non richiedono stat; i campi stat scelti richiedono osservazioni complete valide. Etichette radice/mount, nomi invalidi, alias hard link o maiuscole, normalizzazione e attributi di percorso completo restano ignoti.

attrreference_t occupa8 byte prima degli altri campi; attr_dataoffset è relativo al riferimento, attr_length include NUL e l’area finale è completata a4 byte. Output brevi mantengono lunghezza totale e prefissi esatti, compreso UTF-8 parziale. attribute-names / attribute-names-values / attribute-names-unsupported verificano comportamento nativo, byte indipendenti e arresto sul nome radice conservando output via guest/C/CLI/Python. ARM64:601 query raw,453 confronti SDK dell’intero buffer protetto,384 prefissi. SDK15.5 non dichiara raw476. Native5s, guest/Python5,000,000us/quantum1024 e public10s invariati. Intel nativo, iOS fisico e runtime/framework completi restano incompleti o non verificati.

## Attributi di directory in gruppi limitati

getattrlistbulk(461) richiede enumeration_policy.bulk_attributes=true esplicito; omissione o false non concede diritti. Questo contratto TYPE virtuale restituisce i nomi attuali dei figli diretti in ordine di byte senza segno, senza voci punto e con ordinali locali anziché cookie nativi. Il diritto rimane nell’oggetto iniziale attraverso dup, spostamenti, SWAP, rimozione e riuso del nome; le nuove directory non lo ereditano. minimum_buffer_size, initial_minimum_buffer_size e seek_offset valgono solo per getdirentries64.

NAME|OBJTYPE|RETURNED_ATTRS (0x80000009) è obbligatorio; gli undici campi comuni esistenti richiedono osservazioni selezionate valide. Options0/8 sono ammesse. bulk ignora entrambe le parole bitmap/reserved a16 bit indipendentemente dalla verifica attrlist normale. Un codificatore condivide attrreference_t e validità stat64. Restituisce solo record completi: riempimento a8 byte se possibile, altrimenti dimensione a4 byte per l’ultimo gruppo. Primo gruppo troppo grande: ERANGE senza cambiare output o cursore. Output richiesto parzialmente accessibile: arresto prima della copia. Solo i byte restituiti richiedono memoria scrivibile.

dup condivide l’avanzamento; open separati sono indipendenti. Un percorso completato non nullo mantiene EOF dopo modifiche dello spazio dei nomi e salta dimensione/output dopo la verifica della richiesta. Una directory inizialmente vuota a offset0 viene ricontrollata. lseek zero ripristina l’iterazione. Modifiche prima di EOF, seek arbitrario non nullo e mescolanza getdirentries64/bulk arrestano esplicitamente. NAME-only, voci ERROR, snapshot, ACL/permessi, ordine host e altre mask/options restano non supportati. bulk-attributes / bulk-attributes-values / bulk-attributes-unsupported verificano comportamento nativo comune, byte virtuali letterali e selezione sconosciuta mantenendo output precedente. Preparazione ARM64 privata:728 confronti raw/SDK protetti riusciti. native5s, guest/Python5,000,000us/quantum1024 e public10s invariati. Intel nativo, iOS fisico e ambienti completi restano non verificati o incompleti.

Sources: [XNU bulk ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/man/man2/getattrlistbulk.2), [XNU attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h). Original implementation and probes; no Apple implementation copied.

## Modifiche esplicitamente autorizzate degli attributi estesi ordinari

setxattr(236), fsetxattr(237), removexattr(238) e fremovexattr(239) richiedono un’autorizzazione indipendente sull’oggetto iniziale: C++ `MutableExtendedAttributes`, booleano JSON rigoroso `mutable_extended_attributes=true`. Ogni file, directory o collegamento autorizzato deve dichiarare una lista completa di `extended_attributes` ordinari, anche una lista nota vuota. Le autorizzazioni per contenuto scrivibile e spazio dei nomi modificabile non la concedono. Autorizzazioni e valori seguono l’oggetto mantenuto attraverso dup, spostamento, rimozione e mantenimento delle mappature. Gli oggetti creati e i nomi riutilizzati iniziano con attributi sconosciuti. Alias noti, flag di metadati incompatibili, attributi di sistema protetti, ResourceFork, FinderInfo e semantica di compressione restano esclusi.

La sostituzione mantiene la posizione nella lista virtuale, la rimozione elimina la voce e la creazione la aggiunge in fondo. L’ordine è dichiarato nel processo, senza dedurre quello APFS. Byte e conteggi iniziali e riferimenti ai percorsi di autorizzazione rimangono riservati. L’eccesso a runtime condivide il limite esistente di 16 MiB/4096; rimozione, invalidazione del contenuto e rilascio finale recuperano solo l’eccesso. Errori di capacità, trasporto o scadenza non pubblicano stati temporanei. Un input necessario interamente illeggibile restituisce EFAULT; un input parzialmente leggibile si arresta esplicitamente prima della pubblicazione. Il successo invalida lo stat completo senza inventare tempi, preservando identità, membri della directory, versione/istantanea di enumerazione e cursori.

L’ABI usa low32 FD/options/position e full64 size. I controlli anticipati delle opzioni privilegiate e dei collegamenti FD precedono l’importazione del nome, che precede la ricerca dell’oggetto. set rifiuta NULL con lunghezza non nulla prima di un input VFS eccessivo (E2BIG7); la ricerca precede la convalida di nome ordinario, position e conflitti. set importa il valore completo prima di EEXIST17 per CREATE esistente o ENOATTR93 per REPLACE assente. CREATE e REPLACE insieme restituiscono EINVAL; le rimozioni ignorano entrambi i bit. La dimensione zero non legge il puntatore del valore. Altri flag, permessi sconosciuti e comportamenti del fornitore non osservati si arrestano esplicitamente.

xattr-mutations / xattr-mutations-values / xattr-mutations-unsupported verificano controlli nativi/guest originali, byte virtuali letterali indipendenti e un arresto per autorizzazione mancante che preserva l’output precedente. La preparazione privata ARM64 ha verificato726 chiamate raw/SDK, osservazioni protette complete di544 byte e intere pagine leggibili. native5s/compile120s/drain1s/reap1s, guest/Python5,000,000us/quantum1024 e public10s restano invariati. Intel nativo, iOS fisico, dyld, Mach IPC, thread/segnali, runtime Objective-C/Swift e framework completi restano non verificati o incompleti.

Riferimenti ABI primari: [dichiarazioni delle chiamate XNU](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [definizioni xattr](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Codice e sonde originali; nessuna copia dell’implementazione Apple.

## Collegamenti fisici Darwin limitati

link segue il target simbolico finale; linkat flags=0 seleziona il collegamento stesso, AT_SYMLINK_FOLLOW il target. Sono ammessi solo low32 0/0x40; gli altri bit bassi danno EINVAL prima dell’importazione. Ricerca sorgente ed EPERM per directory precedono la destinazione; una destinazione esistente dà EEXIST. Occorrono autorizzazione della destinazione e lo stesso dominio di mount esplicito. Alias iniziali e conflitti noti di dispositivo, modalità o flag restano esclusi.

Un alias consuma voce e percorso/NUL, senza nuovo inode. Byte, autorizzazioni degli attributi, validità dei metadati e lease dei mapping appartengono all’oggetto condiviso. Le politiche esplicite aggiornano link e ctime; senza di esse stat completo è ignoto. Modificare attributi invalida stat; modificare contenuto invalida le osservazioni degli attributi. Descrizioni e ultimi mapping mantengono i costi dei nomi rimossi; la sostituzione accredita solo costi immediatamente liberabili. I sottoalberi selezionano identità e genitori esatti; alias esterni restano fermi e target relativi usano il genitore selezionato.

Dopo più nomi F_GETPATH/ATTR_CMN_NAME restano non supportati anche con uno o zero nomi; nessun modello generale della cache APFS. bulk NAME usa la voce reale; rename/SWAP dello stesso oggetto conserva entrambi i nomi. Casing EXCL, Intel HVF, iOS fisico, ACL, mapping coerenti/segnali EOF, dyld, Mach IPC, thread e framework completi restano lacune. Le precedenti esclusioni sono estese solo entro questo contratto.

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## Descrittori O_SYMLINK limitati

O_SYMLINK=0x00200000 conserva l'ultimo oggetto simbolico, anche con destinazione mancante o ciclo, in lettura, scrittura o entrambe. Non concede scrittura sul contenuto della destinazione. O_CREAT segue ancora il destinatario; NOFOLLOW mantiene la priorità ELOOP, la creazione esclusiva EEXIST, e O_DIRECTORY restituisce ENOTDIR per il link conservato. Componenti intermedi e barre finali usano il risolutore e NOFOLLOW_ANY esistenti. F_GETFL omette il bit di selezione.

La descrizione conserva LinkNode e NameIdentity effettivi. Dup condivide flag e cursore; open indipendenti hanno descrizioni proprie. Rinomina, rimozione, riutilizzo del nome e rimozione del padre non sostituiscono l'oggetto detenuto. F_GETPATH/ATTR_CMN_NAME usano il nome univoco selezionato; la precedente presenza di più nomi impedisce permanentemente l'inferenza vnode, anche senza nomi superstiti. Le prenotazioni iniziali restano fisse; nomi, destinazioni, voci e crescita dinamica degli attributi attendono l'ultimo proprietario reale. La sostituzione non può accreditare l'ultimo alias ancora detenuto.

L'I/O non espone la stringa della destinazione come contenuto. Dopo i controlli esistenti di importazione scalare/vettoriale, accesso e quantità, gli offset negativi danno EINVAL. A INT64_MAX la lettura restituisce zero, la scrittura EFBIG; gli altri offset ammessi danno EPERM anche a lunghezza zero, prima di APPEND o accesso ai dati. Restano le regole negative anticipate di pwrite/pwritev. DATA/HOLE dà ENXIO per posizioni non negative, EINVAL per negative, senza spostare il cursore.

ftruncate non negativo su descrizioni scrivibili e open TRUNC ammesso impostano solo WasWritten. Destinazione, stat completo, xattrs, cursore, budget e inode restano invariati. Sola lettura o lunghezza negativa dà EINVAL. F_SETFL ammesso modifica APPEND|NONBLOCK prima di restituire ENOTTY25; dup vede l'effetto, open indipendenti no. Gli argomenti sconosciuti arrestano prima degli effetti.

fpathconf fisso, fgetattrlist e autorità ordinaria FD-xattr dichiarata separatamente operano sul link. FD di directory relativo e fchdir danno ENOTDIR. truncate non ripristina stat invalidato da una modifica degli attributi. mmap legacy private/shared allineato e non eseguibile raggiunge EINVAL per tipo simbolico, senza mapping o lease. Shared ordinario, flag sconosciuti, protezione eseguibile e altri limiti esistenti restano invariati. I18 controlli nativi mmap coprono solo length16384, offset0 e protection1/2/3.

La preparazione ARM64 originale confronta144 byte stat protetti per15 troncamenti, offset/quantità estremi, seek sparso ed effetti di F_SETFL fallito. Il programma comune senza SDK esegue O0/O1/O2 e confronta il percorso effettivo del FD padre. Le rotte virtuali confrontano un letterale stat/type indipendente oppure rifiutano esplicitamente più nomi conservando l'output. Intel HVF nativo, iOS fisico, ACL/permessi, EOF/segnali mappati, dyld, Mach IPC, thread e ambienti/framework completi restano non verificati o incompleti.

Fonte primaria: [confine mmap XNU corrispondente](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_mman.c). Implementazione e sonde originali; nessuna implementazione Apple copiata.

```text
O_SYMLINK=0x00200000; ENOTTY=25
open O_RDONLY|O_SYMLINK|O_TRUNC: WasWritten only
LinkNode, NameIdentity, HadMultipleNames, DetachedNames
INT64_MAX read=0 / write=EFBIG; other admitted offsets=EPERM
SEEK_DATA/SEEK_HOLE nonnegative=ENXIO / negative=EINVAL
F_SETFL: APPEND|NONBLOCK effect before ENOTTY; dup shares / independent open separate
symbolic-descriptors
symbolic-descriptors-values
symbolic-descriptors-name-unsupported
SymbolicDescriptor*, SymbolicDescriptorsRetainObjectsAndNativeErrorOrder
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 transport parameters / 15 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## Stato non bloccante limitato dei descrittori

O_NONBLOCK=4 è ammesso per file ordinari, directory e O_SYMLINK e conservato da F_GETFL. Dup condivide stato e cursore; open indipendenti mantengono descrizioni separate. Restano le regole di accesso, close-on-exec, WasWritten, metadati, byte e cursore. Stdin finito dichiarato conserva EOF e ordine degli errori di puntatore; input omesso resta sconosciuto. La cattura conserva errori di copia e budget condiviso.

F_SETFL valida gli argomenti low32 prima degli effetti, aggiunge uno secondo la conversione nativa dei flag open e modifica solo APPEND|NONBLOCK. High32 è ignorato; accesso e WasWritten in ingresso non concedono autorizzazioni né inventano scritture. I valori nativi letterali3/7/11/15 selezionano4/8/12/0. Le descrizioni simboliche cambiano stato prima di ENOTTY25. Flag sconosciuti come ASYNC0x40 arrestano prima degli effetti.

La preparazione ARM64 macOS originale contiene122 osservazioni:16 richieste per combinazione valida oggetto/accesso, dup mantenuto, open indipendente, cancellazione, scritture reali e CLOEXEC per FD. Il programma comune senza SDK confronta con O0/O1/O2 byte, stato, cursore e ABI BSD grezza carry/errno. Guest, C/CLI e Python verificano anche il rifiuto sconosciuto; i tre profili ARM64 HVF richiedono esecuzione reale. Nessuna attesa di disponibilità, pipe, rete, kqueue, segnali asincroni o I/O host. O_EVTONLY resta non supportato; Intel HVF nativo, iOS fisico e ambiente completo restano non verificati o incompleti.

```text
O_NONBLOCK=4; F_SETFL raw low32 mask=0x1000f
requests3/7/11/15 -> APPEND|NONBLOCK status4/8/12/0
nonblocking-descriptors / nonblocking-flags-unsupported
Nonblocking*, NonblockingDescriptorsKeepNativeControlState
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
8 model cases / 20 transport parameters / 10 public cases / 5 Python profiles
67 mandatory workloads per platform / ARM64 201 / Intel 134
```

## Osservazioni getentropy esplicite e finite

BSD getentropy500 usa la coda ordinata `DarwinSystemOptions::EntropyReads`, con JSON `darwin_system.entropy_reads`. Stringhe esadecimali non vuote e di lunghezza pari: massimo256 record di1..256 byte. Sono limiti del modello; il trasporto JSON mantiene65536 byte. L’omissione è sconosciuta; `[]` è esplicitamente esaurita. La convalida nativa/JSON precede modifiche a immagine o backend; altri profili OS rifiutano le opzioni Darwin.

Si controlla prima l’intera lunghezza64 bit: oltre256 restituisce EINVAL22 senza accesso o consumo; zero riesce con ogni puntatore senza dati. Richieste non nulle ammettono il successivo record di lunghezza esatta. Dati mancanti, esauriti o incompatibili fermano UnsupportedService prima degli effetti, anche con indirizzo invalido: è l’ordine di ammissione del replay. Successo o EFAULT14 interamente inaccessibile consuma un record. Destinazioni parzialmente scrivibili sono rifiutate prima di copia o avanzamento; errori di trasporto non avanzano. Un errore successivo dei registri di ritorno conserva effetti completati. Ogni esecuzione ricomincia dal primo record anche con le stesse opzioni.

DarwinEntropy possiede un cursore per esecuzione e byte immutabili. BSD e returnService esistenti gestiscono entrambe le ISA, carry e registri secondari. Il programma senza SDK copre5 profili ospiti e3 ARM64 HVF; i byte fissi non entrano nell’inventario RNG nativo deterministico. Le sonde ARM64 O0/O1/O2 coprono594 chiamate; byte sentinella cambiati non indicano lunghezza copiata esatta. Nessun RNG host, qualità crittografica, /dev/random, import libc o framework. Intel HVF, iOS fisico e compatibilità OS completa restano non verificati o incompleti.

```text
BSD getentropy500 / DarwinEntropy / EntropyReads / darwin_system.entropy_reads
full64 size>256 -> EINVAL22; zero ->0; whole EFAULT14 consumes one record
1..256 bytes per record / at most256 records / JSON transport65536 bytes
entropy-replay / entropy-missing / entropy-exhausted / entropy-mismatch / entropy-partial
15 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
68 mandatory workloads per platform / ARM64 204 / Intel 136
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU getentropy ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU generation/copyout boundary](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/dev/random/randomdev.c).

## Identità esplicita del thread corrente

BSD thread_selfid372 legge l’osservazione immutabile opzionale `DarwinSystemOptions::ThreadID` / JSON `darwin_system.thread_id`. Ogni schema uint64, compreso zero, è noto; l’omissione arresta con UnsupportedService. Le stringhe decimali mantengono64 bit e i numeri JSON richiedono interi esatti fino a2^53-1. Nessun ID viene dedotto da host, PID o porta Mach. La chiamata senza argomenti ignora i sei valori e non accede alla memoria. La risoluzione low32 esistente conserva il numero originale completo negli eventi; il ritorno BSD mantiene64 bit e azzera carry e RDX/X1. Le forme Mach restano non supportate.

Le esecuzioni ripetute conservano l’osservazione; opzioni separate restano indipendenti. Non assegna ID, garantisce unicità o crea identità degli eventi dello scheduler, né implementa ciclo di vita, pthread, TLS o Mach IPC. Le sonde ARM64 originali O0/O1/O2 conservano24 chiamate confrontate con il pthread corrente del SDK, argomenti arbitrari e bit alti del numero. Il programma nativo comune confronta solo relazioni nello stesso processo; i byte di ID dichiarati sono esclusi dall’inventario nativo deterministico. Intel HVF, iOS fisico e compatibilità OS completa restano non verificati o incompleti.

```text
BSD thread_selfid372 / Wide / ThreadID / darwin_system.thread_id
known uint64 including0 / missing -> UnsupportedService / no memory
full64 return / low32 resolution / carry clear / RDX-X1 zero / raw event number
thread-identity / thread-identity-value / thread-identity-missing
4 model cases / 20 transport parameters / 16 public cases / 5 Python profiles
69 mandatory workloads per platform / ARM64 207 / Intel 138 unverified
original ARM64 O0/O1/O2 probes24 / native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU thread_selfid ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [libpthread current-thread owner](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/kern/kern_support.c).

## Osservazioni esplicite delle porte Mach proprie

Le chiamate Mach grezze thread_self_trap27, task_self_trap28 e host_self_trap29 leggono osservazioni uint32 facoltative indipendenti: DarwinSystemOptions::ThreadSelfPort, TaskSelfPort e HostSelfPort tramite darwin_system.thread_self_port, task_self_port e host_self_port. Ogni richiesta necessita solo del suo campo. L’assenza resta ignota e arresta UnsupportedService; zero, nomi uguali e tutti i pattern32 bit sono espliciti. Accetta interi esatti o stringhe decimali fino a UINT32_MAX senza ereditare i limiti pid_t positivi di process_group_id/session_id.

Il proprietario di sistema converte il nome tramite il risultato int32 con segno del kernel in raw64: 0x80000001 diventa0xffffffff80000001 e UINT32_MAX diventaUINT64_MAX. Il binding Mach conserva flag e X1/RDX, mantiene le sovrascritture x64 RCX/R11, ignora gli argomenti e non accede alla memoria. La risoluzione low32 conserva il numero grezzo completo. Gli eventi Mach omettono BSD error e non generano ThreadID di scheduling. Riutilizzo e opzioni indipendenti lasciano immutate le osservazioni.

La preparazione ARM64 O0/O1/O2 conserva432 osservazioni, tutte16 NZCV, prefissi high32, registri inizializzati e confronti SDK. Nessun nome nativo con bit31 è stato osservato; l’estensione alta con segno deriva dal contratto XNU fissato e da test letterali indipendenti modello/guest/API. Il programma nativo comune confronta solo relazioni nel medesimo processo; nomi virtuali e assenze non sono riferimenti nativi deterministici. Non alloca nomi/riferimenti di invio né autentica diritti vivi, unicità o IPC/durata/scheduling. Permessi/ACL, attese di disponibilità, orologi progressivi, veri Mach IPC/thread, dyld/TLS e runtime/framework completi restano incompleti. Intel HVF nativo e iOS fisico restano non verificati.

```json
{"darwin_system":{"thread_self_port":2147483649,"task_self_port":0,"host_self_port":"4294967295"}}
```

```text
Mach thread_self_trap27 / task_self_trap28 / host_self_trap29
ThreadSelfPort / TaskSelfPort / HostSelfPort / uint32 / signed-int32 -> raw64
known0 / missing -> UnsupportedService / no arguments or memory
low32 resolution / complete raw number / flags and RDX-X1 preserved / no BSD error
mach-self-ports / mach-self-port-values / mach-self-port-missing
MachSelfPortsPreserveExplicitBitsAndIndependentRuns
6 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
70 mandatory workloads per platform / ARM64 210 / Intel 140 unverified
original ARM64 O0/O1/O2 probes432 / no native bit31 name observed
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU Mach trap table](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/syscall_sw.c), [self-port name owners](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/ipc_tt.c), [host-port owner](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/ipc_host.c), [ARM64 return](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [x64 return](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/i386/bsd_i386.c).


## Query statiche dei permessi del proprietario ordinario

darwin_files.authorization="static-owner-queries" (DarwinFileAuthorization::StaticOwnerQueries) dichiara un ambiente locale ordinario immutabile: nessuna ACL, MAC, listener kauth aggiuntivo, entitlement o aggiramento; mount scrivibile, eseguibile e non opaque con proprietà attiva; flags=0 e nessun bit speciale. Le sole osservazioni non autorizzano. La dichiarazione abilita query access/faccessat.

Ogni controllo richiede darwin_system.credentials esplicito e metadati dell’oggetto reale. access usa real_uid; AT_EACCESS usa effective_uid per SEARCH e R/W/X finale. UID selezionato non zero e proprietario, tutti i bit richiesti presenti. Rifiuto noto restituisce EACCES13 con carry BSD esistente. Identità/metadati ignoti, UID0 selezionato, non proprietario, azioni estese e R/W/X di link terminale trattenuto fermano UnsupportedService; UID0 non selezionato resta valido. Nessuna inferenza di host, UID1000, gruppi/altri o eccezione root.

SEARCH usa X del padre prima di cercare figli, nomi assenti, punti pertinenti e riavvio link. F_OK/bit ignorati richiedono solo SEARCH reale; root con soli slash e dotdot limitato al root non lo richiedono. Slash finali consumati non aggiungono SEARCH finale. Ordine flags/copia/dirfd relativo/nome vuoto invariato. Name255 è limite di disponibilità: dopo SEARCH consentito eccedenza Unsupported, rifiuto prima EACCES, ignoto prima arresto; nessun errno filesystem inventato.

Ogni altra operazione, inclusi open/stat/chdir/readlink/attributi/enumerazione/mutazioni, si ferma prima degli effetti. Solo veri stream Input/Output/Error e alias dup mantengono I/O, close, dup/dup2, lseek e fcntl; i numeri FD0/1/2 non bastano. mmap file/mappingSource chiusi, memoria anonima indipendente. Ammissione C++/JSON condivisa rifiuta diritti/politiche di mutazione/creazione, flags/bit speciali noti e alias inode/device; ignoti e256 voci/16MiB invariati.

L’esempio dichiara SEARCH root e file0400: lettura consentita, scrittura EACCES, open non supportato. ARM64 O0/O1/O2 conserva2472 coppie raw/SDK,2439 letterali indipendenti,33 sole osservazioni con compile120s/native5s. fstatx/filesec verifica ACL assente; primo NULL/ENOENT era errore di protocollo prima delle query, conservato. UID real/effective nativi uguali; selezione distinta usa fonte/modello fissati. Hook globali/interni opaque non completamente verificati. owner-queries escluso da58 riferimenti nativi deterministici. Gruppi/root/ACL/MAC, identità dinamiche, autorizzazione vnode generale, attese, orologi, Mach IPC/thread, dyld/TLS e runtime completi restano aperti; Intel HVF e iOS fisico non verificati.

```json
{"darwin_files":{"authorization":"static-owner-queries","files":[{"path":"/data","bytes_hex":"00","metadata":{"device":7,"inode":2,"mode":33024,"link_count":1,"uid":501,"gid":20,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16832,"link_count":2,"uid":501,"gid":20,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":501,"real_gid":20,"effective_gid":20}}}
```

```text
DarwinFileAuthorization::StaticOwnerQueries / authorization=static-owner-queries
access33 / faccessat466 / real_uid / effective_uid / AT_EACCESS0x10
owner R/W/X / all requested bits / directory SEARCH / EACCES13
no-action root LOOKUP / root-clamped dotdot / consumed terminal separators
unknown credentials-metadata-root-nonowner -> UnsupportedService
all other vnode routes closed / typed standard streams and dup aliases only
anonymous memory independent / file-backed mmap and mappingSource closed
Name255 availability stop after allowed SEARCH / no guessed filesystem errno
owner-queries / owner-query-stop / owner-query-open / owner-query-map
OwnerQueriesKeepPermissionAndUnknownBoundaries
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
original ARM64 O0/O1/O2 pairs2472 / literal2439 / capture-only33
native5s / compile120s / owner-build1200s / guest-Python5,000,000us
```
[XNU access and subject selection](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [real credential copy](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c), [owner authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [pathname SEARCH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c), [cached lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_cache.c), [Libc ACL properties](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/gen/filesec.c), [fstatx ACL absence](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/sys/statx_np.c).

## Query ordinarie statiche con gruppi parzialmente noti

`darwin_files.authorization="static-ordinary-queries"` (DarwinFileAuthorization::StaticOrdinaryQueries) mantiene le ipotesi immutabili di mount, sicurezza e metadati, le operazioni chiuse, le eccezioni dei flussi standard e la memoria anonima indipendente della sezione precedente. static-owner-queries resta limitato al proprietario. I controlli effettivi richiedono credenziali e metadati espliciti e UID selezionato diverso da0; root, diritti estesi e R/W/X sul collegamento finale restano esclusi.

Il proprietario usa tutti i bit richiesti della propria classe. Per altri utenti si confrontano i risultati gruppo/altri per l'intera maschera: risultati uguali permettono o restituiscono EACCES13 senza consultare gruppi; bit diversi possono entrambi negare. Altrimenti un membro noto usa gruppo, un non membro dimostrato usa altri e appartenenza ignota arresta UnsupportedService prima di ricerca o effetti. Le classi non si combinano.

credentials.groups è la lista ordinata nelle credenziali del kernel, con EffectiveGID in posizione0 e duplicati conservati, distinta dalla lista estesa del risolutore SDK getgroups. Il gruppo primario selezionato e le presenze esplicite sono noti; un'assenza o lista omessa generalmente non dimostra la non appartenenza. Se entrambe le coppie UID/GID coincidono il contesto reale resta invariato. Altrimenti RealGID sostituisce posizione0 e il vecchio EffectiveGID sostituisce la prima corrispondenza supplementare RealGID. Senza corrispondenza il vecchio primario è rimosso e memberd disabilitato. Questo cambiamento dimostrato o KAUTH_UID_NONE originale esplicito, con una lista completa esplicita, rende note risposte negative. UID diversi con GID uguali lo attivano comunque; un primario duplicato può lasciare ignota l'appartenenza esterna. AT_EACCESS usa il contesto effettivo originale; l'input non cambia.

L'esempio nega la query reale dopo la rimozione di GID20 e accetta AT_EACCESS tramite il primario effettivo20 noto. SEARCH deriva dall'accordo gruppo/altri; non autorizza UID0 selezionato né apertura di file.

```json
{"darwin_files":{"authorization":"static-ordinary-queries","files":[{"path":"/data","metadata":{"device":7,"inode":2,"mode":32816,"link_count":1,"uid":700,"gid":20,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}},"bytes_hex":"00"}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16895,"link_count":2,"uid":0,"gid":0,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":502,"real_gid":30,"effective_gid":20,"groups":[20,40]}}}
```

I controlli ARM64 nativi O0/O1/O2 in sola lettura conservano270 coppie raw/SDK per oggetti altrui, SEARCH, errori, identità e assenza indipendente della proprietà ACL. La lista grezza delle credenziali ha16 gruppi, quella estesa SDK17. Il primo tentativo ha rifiutato erroneamente la lunghezza prima delle query dei permessi; il fallimento resta conservato. ID reali/effettivi coincidono: differenze e trasformazioni complete dipendono da XNU fissato e modelli indipendenti, non da prove native di tutti i controlli esterni. Cinque configurazioni software e tre ARM64 HVF verificano ospiti reali, C/CLI/Python e identità, metadati, root e appartenenze ignoti. Il modello fornito resta fuori dalle58 referenze native comuni. Risoluzione completa gruppi, root, ACL/MAC, vnode generale, identità dinamiche, disponibilità/rete, orologi progressivi, Mach IPC/thread, dyld/TLS e framework completi restano incompleti; Intel HVF nativo e iOS fisico non verificati. Scadenze invariate.

```text
DarwinFileAuthorization::StaticOrdinaryQueries / authorization=static-ordinary-queries
owner bits / whole-mask group-world outcomes / EACCES13
credentials.groups / in-credential16 / EffectiveGID index0 / duplicates retained
real credential copy / first supplementary match / displacement disables memberd
missing membership usually unknown / original NONE or displaced real plus complete list proves negatives
all41 other file routes and direct/file-backed mappings closed / typed streams only
ordinary-queries / ordinary-query-unknown / ordinary-query-open / ordinary-query-map
OrdinaryQueriesPreserveGroupKnowledgeAndSelectedSearch
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
original ARM64 O0/O1/O2 nonowner pairs270 / raw-groups16 / SDK-extended-groups17
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU ordinary mode authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_subr.c), [real credential and group membership](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c), [raw in-credential getgroups](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c), [SDK extended getgroups](https://github.com/apple-oss-distributions/Libc/blob/Libc-1698.140.3/sys/getgroups.c).

## Contesto originale esplicito di appartenenza ai gruppi

`DarwinCredentials::GroupMembershipUID` / `darwin_system.credentials.group_membership_uid` opzionale dichiara cr_gmuid originale, indipendente dai quattro ID e groups. Ammette0..INT32_MAX o esattamente KAUTH_UID_NONE=4294967195 (0xffffff9b, UINT32_MAX meno100). Usa il decoder esistente per stringhe decimali senza perdita e interi numerici esatti; tipi errati, frazioni, negativi e altri valori fuori intervallo sono rifiutati prima del caricamento. Il sentinella resta vietato in UID/GID e gruppi ordinari. Omissione e altri UID validi non provano non appartenenza esterna né abilitano un risolutore.

Primario e voci positive sono noti per primi. NONE originale con lista completa esplicita prova la non appartenenza di una voce assente; senza lista resta ignota. La copia reale conserva NONE originale anche quando la prima corrispondenza supplementare mantiene il vecchio primario; una rimozione provata disabilita pure la risoluzione esterna. Query scalari, raw getgroups, proprietà di creazione e modi precedenti restano invariati. Lo stesso proprietario di query sceglie i permessi degli altri e verifica SEARCH prima dei figli; altre operazioni vnode restano chiuse.

Il chiamante nell’esempio non appartiene a GID50: entrambe le identità leggono /data, ma la query di scrittura restituisce EACCES13. Senza group_membership_uid risultati diversi gruppo/altri restano non supportati.

```json
{"darwin_files":{"authorization":"static-ordinary-queries","files":[{"path":"/data","bytes_hex":"00","metadata":{"device":7,"inode":2,"mode":32772,"link_count":1,"uid":700,"gid":50,"size":1,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"directories":[{"path":"/","metadata":{"device":7,"inode":1,"mode":16895,"link_count":2,"uid":0,"gid":0,"size":0,"block_size":4096,"blocks":0,"flags":0,"generation":0,"access_time":{"seconds":0,"nanoseconds":0},"modification_time":{"seconds":0,"nanoseconds":0},"change_time":{"seconds":0,"nanoseconds":0},"birth_time":{"seconds":0,"nanoseconds":0}}}],"working_directory":"/"},"darwin_system":{"credentials":{"real_uid":501,"effective_uid":501,"real_gid":20,"effective_gid":20,"groups":[20],"group_membership_uid":4294967195}}}
```

Esecuzioni SDK locali O0/O1/O2 verificano solo il sentinella e uid_t di quattro byte, non cr_gmuid del sistema o il risolutore; non sostituiscono le270 coppie raw/SDK reali precedenti. ordinary-queries-closed-groups controlla173 eventi, rifiuti effettivi di non membri e SEARCH di figli mancanti, punto/doppio punto e link in cinque profili software/tre ARM64 HVF obbligatori e C/CLI/Python. Resta fuori dalle58 referenze native-common. Root, ACL/MAC, risoluzione completa, autorizzazione vnode generale, credenziali dinamiche, attese/rete, orologi avanzanti, Mach IPC/thread, dyld/TLS e framework completi restano incompleti. Intel HVF/iOS fisico non sono verificati; i termini originali non cambiano.

```text
GroupMembershipUID / group_membership_uid / original cr_gmuid
0..INT32_MAX or KAUTH_UID_NONE=4294967195 / 0xffffff9b / not UINT32_MAX
positive entries first / original NONE plus complete list proves negatives
omitted list unknown / first-match real copy preserves original NONE
ordinary-queries-closed-groups / 173 events / stdout GN
OrdinaryQueriesUseExplicitMembershipUIDWithoutResolver
73 mandatory workloads per platform / ARM64 219 / Intel 146 unverified
SDK constant O0/O1/O2 only / prior actual nonowner pairs270 remain separate
58 native-common references unchanged / Intel and physical iOS unverified
native5s / compile120s / guest-Python5,000,000us / quantum1024 / public10s
```

[XNU KAUTH_UID_NONE](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/kauth.h), [XNU credential membership](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_credential.c).

## Prontezza immediata dei file ordinari

BSD raw poll(230) e poll_nocancel(417) accettano timeout low int32=0. nfds è uint32; oltre OPEN_MAX10240 EINVAL22 precede i puntatori. Numeri positivi richiedono darwin_system.resource_limits resource8 Current esplicito. Oltre Current e FD_SETSIZE1024 si ottiene EINVAL; altrimenti serve UID effettivo originale esplicito, nonzero dà EINVAL, zero consente. L’eccezione root ha solo prove sorgente/modello. DescriptorLimit, kern.maxfilesperproc, valore FD e budget non danno tale osservazione. Zero non tocca l’array; timeout nonzero resta escluso dopo l’ammissione del numero.

Il fornitore regolare virtuale costruito dichiara descrizioni non revocate, registrazione riuscita dei filtri ordinari lettura/scrittura e nessun rifiuto MAC/fornitore. Il tipo vnode nativo non prova registrazione. Con tale premessa IN/RDNORM e OUT/WRBAND sono pronti anche a EOF/open readonly, senza dedurre da byte restanti, flag, O_NONBLOCK o diritti di scrittura. Chiavi FD numerico/lettura e FD/scrittura conservano l’ultima riga separatamente; FD dup distinti restano separati. HUP solo registra lettura senza bit pronto. FD negativo/bit ignorati danno zero, registrazioni chiuse POLLNVAL32 per riga. OOB/vnode attivi, stream/directory/link sono rifiutati prima dell’output. Si acquisisce tutta l’entrata di record8 byte; entrata incompleta/output tutto non scrivibile dà EFAULT14, output parziale è rifiutato senza prefisso. Copia intera conserva fd/events e sostituisce revents. Entrambi i modi statici chiudono Poll prima del controllo, anche zero; elenco condiviso41 percorsi.

immediate-poll senza SDK copre86 eventi, cinque profili software/tre ARM64 HVF obbligatori, C/CLI/Python e un nuovo riferimento nativo comune, inventario attuale59. La sonda indipendente O0/O1/O2 ARM64 conserva582 controlli letterali ABI/errori e catture del fornitore regolare miste, non costanti universali. Le vecchie prove58 conservano identità; download fallito, primo timeout nativo e termini originali restano preservati. Intel HVF, iOS fisico, revoca/MAC/rifiuto registrazione, select, attese, fornitori asincroni, rete, orologi avanzanti, veri Mach IPC/thread, dyld/TLS e framework completi restano non verificati o incompleti.

```text
poll230 / poll_nocancel417 / timeout low int32=0 / nfds uint32
ResourceLimits[8].Current / OPEN_MAX10240 / FD_SETSIZE1024 / explicit original effective UID
constructive regular provider: nonrevoked / successful ordinary filter attachment / no MAC-provider refusal
numeric FD + read/write filter / independent last requested index / distinct dup aliases
IN1 RDNORM64 OUT4 WRBAND256 / HUP16 trigger only / closed registrations POLLNVAL32
negative and ignored-only rows zero / whole input snapshot / whole output / partial output unsupported
EFAULT14 EINVAL22 / no ready prefix on unsupported row / zero count no pointer
static-owner-queries and static-ordinary-queries / all41 other file routes closed before preflight
immediate-poll86 events / five software + three mandatory ARM64 HVF profiles / C CLI Python
59 current native-common cases / prior58 source identity preserved / 582 mixed controls and captures
native Intel and physical iOS unverified / waits select revoked-MAC-provider failures networking unfinished
```

[XNU poll ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/poll.h), [poll registration and copy order](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [vnode registration and regular filters](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).
