**Lingue**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Indice della documentazione](README.md)

# Emulazione dei processi guest

`neverd emulate` esegue un’immagine con un profilo esplicito del sistema operativo guest. Trasporto CPU, parsing dell’immagine, ingresso del processo e servizi OS hanno responsabilità separate. Attivare `NEVERD_ENABLE_CPU_EMULATION=ON`; è incluso anche dall’emulazione dei driver.

Il primo profilo, `linux-elf64-v1`, esegue ELF `ET_EXEC` x64/AArch64 e PIE statici `ET_DYN` autorilocanti a CPL3 o EL0. Carica veri segmenti ELF, crea lo stack iniziale, riprende a quanti e gestisce richieste esplicite di system call Linux. È un modello di processo autonomo, non una distribuzione Linux completa né una promessa di eseguire binari libc arbitrari. Linking dinamico, segnali, thread, filesystem e servizi non supportati falliscono esplicitamente.

<!-- i18n-section: cli-sdk -->

## CLI e SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Host Linux compatibili selezionano KVM e host Windows compatibili WHP; le altre combinazioni ISA host/guest usano Unicorn. Un backend selezionato ma indisponibile è un errore, senza fallback silenzioso. L’immagine ELF usa il modello Linux anche se eseguita su Windows. Vedi [esecuzione CPU](cpu-execution.md) per inventario istruzioni e limiti.

Il CLI emette un singolo report JSON. Exit code 0 per status guest zero, 2 per un altro status, 3 per esecuzione incompleta (inclusi fault e limiti), 1 per setup/API non valido. Lo status guest effettivo è in `exit_status`. L’entry point C aggiuntivo [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) accetta sessione, path non vuoto, profilo esplicito e JSON opzioni facoltativo. Liberare il risultato con `neverd_free_string`; NULL segnala errore di setup descritto da `neverd_last_error`. Fault guest o stop per risorse restituiscono un report. L’immagine d’analisi caricata nella sessione non serve e non viene modificata.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## Opzioni e risultati

Le opzioni sono un oggetto JSON massimo 64 KiB. Campi sconosciuti/null, tipi errati, NUL incorporati e limiti non positivi vengono rifiutati.

| Opzione | Predefinito | Contratto |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` o `whp` |
| `arguments` | Nome input | argv completo incluso argv[0]; vuoto usa il default |
| `environment` | `[]` | Stringhe guest esplicite; non eredita l’ambiente host |
| `instruction_limit` | 100000 | Tentativi d’istruzione ammessi condivisi |
| `event_limit` | 10000 | Eventi syscall, addebitati prima del servizio OS |
| `timeout_microseconds` | 5000000 | Deadline monotona avviata dopo il setup del processo |
| `memory_limit` | 67108864 | Budget di memoria fisica/mappata |
| `stack_size` | 1048576 | Stack allineato alla pagina entro il budget |
| `output_limit` | 1048576 | Byte complessivi catturati da stdout/stderr |
| `instruction_quantum` | 1024 | Intervallo d’ammissione prima di cedere alla runtime |

`schema_version` è 1. Il report contiene profilo, architettura, backend e motivo della scelta, `stop_reason`, `exit_status` nullable, diagnostica, PC di ingresso/corrente, contatori, record dei servizi e ultimo esito CPU tipizzato. Indirizzi, numeri syscall, registri argomento e bit di ritorno sono stringhe esadecimali **senza** `0x`; `stdout_hex`/`stderr_hex` preservano NUL e UTF-8 non valido. Un risultato syscall null significa nessun ritorno modellato (per esempio exit o richiesta non supportata), non zero riuscito.

<!-- i18n-section: linux-semantics -->

## Semantica del profilo Linux

La policy OS riusa gli header di programma già decodificati dal loader ELF. Verifica tag ABI, allineamento segmenti, tabelle degli header mappate e limiti degli indirizzi user. Un piano generico controlla estensioni, permessi, sovrapposizioni e budget prima dell’allocazione e pubblica solo uno spazio privato completo. Conserva prefissi/code delle pagine file, azzera BSS, rispetta i permessi e riserva guard gap dello stack. Layout con pagine sovrapposte e header contraddittori sono rifiutati, non indovinati.

Il PIE statico usa un load bias deterministico di almeno `0x40000000`, aumentato per rispettare l’allineamento `PT_LOAD` maggiore. Segmenti, PC d’ingresso e `AT_PHDR`/`AT_ENTRY` usano lo stesso bias; i program header originali restano invariati e `AT_BASE` vale zero senza interpreter. Il mapping usa i byte originali del file, non i fixup dell’analisi; il guest esegue autonomamente relocation e inizializzazione. Il loader decodifica `PT_DYNAMIC` da record limitati del file originale, senza dipendere dalle section. Se presente, la tabella deve essere leggibile, terminata e contenere al massimo 4096 entry. `PT_INTERP` e tag esterni di dipendenze/filter/audit sono rifiutati; non c’è dynamic linker, risoluzione simboli o esecuzione costruttori.

Lo stack iniziale contiene argc/argv/envp/auxv allineati, PHDR/PHENT/PHNUM, entry, dimensione pagina e identità. PID/TID/UID/GID modellati valgono 1000. `AT_RANDOM` contiene i primi 16 byte dello SHA-256 dell’input per riproducibilità; è una policy deterministica del modello, non entropia crittografica. HWCAP/HWCAP2 sono zero; non esiste vDSO.

Sono implementate `write`, `exit`, `exit_group`, `getpid` e `gettid`, `mmap`, `mprotect`, `munmap`, `brk`, con numeri distinti per [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) e [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). Il ritorno di SYSCALL x64 applica i clobber RCX/R11, RAX e il PC successivo. ARM64 usa x8 per il numero e x0 per il risultato. Le altre chiamate si fermano come `unsupported_service`; non vengono eseguite syscall host.

I template TLS statici `PT_TLS` sono validati come dati del loader: un solo template, estensioni file/memoria limitate, allineamento congruente e byte iniziali leggibili. L’avvio guest alloca e inizializza i blocchi TLS e installa il thread pointer; il modello Linux non inventa un TCB/DTV specifico di libc. Questo permette TLS local-exec generato dal compilatore nei programmi freestanding. TLS dinamico e thread OS restano fuori ambito.

Su x64, `arch_prctl` supporta `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` e `ARCH_GET_GS`. Set accetta una base user anche non mappata; le dereference successive controllano comunque i permessi. Basi kernel restituiscono `EPERM` guest; destinazioni Get non valide restituiscono `EFAULT` senza fault CPU. Le altre operazioni falliscono esplicitamente. ARM64 installa `TPIDR_EL0` con `MSR`; `MRS`, accessi FS/GS e restore del contesto preservano il thread pointer fra quanti e ingressi backend. Non implementa uno scheduler.

I descrittori 1 e 2 sono sink virtuali di byte. `write` convalida pagine user leggibili, restituisce il prefisso leggibile se una pagina successiva non è accessibile ed `EFAULT` se nessun byte è leggibile. Un descrittore errato restituisce `EBADF`; una write di zero byte con descrittore valido non accede al puntatore. Atomicità delle pipe Linux e file non sono modellati. Il limite output ferma prima di pubblicare una scrittura eccedente.

I servizi di memoria anonima condividono spazio del processo e budget fisico con immagine e stack. `mmap` accetta esattamente `MAP_PRIVATE | MAP_ANONYMOUS`, con `PROT_NONE`, `PROT_READ`, `PROT_READ | PROT_WRITE`, `PROT_READ | PROT_EXEC` o RWX leggibile. Rispetta suggerimenti liberi e allineati a pagina; altrimenti cerca spazi da `0x100000000`, poi dall'indirizzo utente minimo, riservando le guardie dello stack. Questa disposizione deterministica non emula ASLR Linux. Le pagine nuove sono indipendenti e azzerate; la rimozione parziale può recuperare pagine non vincolate. Una proiezione CPU o vista del backing ancora conservata può mantenere viva un'allocazione ritirata fino al termine della propria durata.

Le lunghezze sono arrotondate a pagine. `munmap` tollera buchi e rimozioni ripetute; `mprotect` modifica il prefisso mappato prima di restituire `ENOMEM` al primo buco. `PROT_NONE` conserva allocazione e byte, negando l'accesso guest. Il `brk` grezzo restituisce il limite richiesto in caso di successo e quello precedente in caso di errore, non lo zero/meno uno del wrapper libc. Il limite iniziale è la fine immagine allineata a pagina. La crescita rispetta altre mappature e budget; la riduzione conserva i byte della pagina parziale restante. Regole e priorità degli errori seguono i servizi Linux di [mappatura](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c) e [protezione](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c).

Mappature di file, condivise o fisse, crescita verso il basso, pagine enormi, blocco memoria, chiavi di protezione, permessi di sola esecuzione/scrittura e altri flag restano esplicitamente non supportati: arresto prima di pubblicare effetti o inventare un ritorno. Errori ordinari di intervallo, lunghezza e allineamento nel sottoinsieme ammesso restituiscono errori guest e consentono di proseguire. Nessun puntatore o richiesta di mappatura guest viene inoltrato all'OS host.

<a id="windows-pe64-profile"></a>

<!-- i18n-section: windows-pe64 -->

## Profilo Windows PE64

`windows-pe64-v1` aggiunge processi console Windows x64/ARM64 limitati: caricamento PE, PEB/TEB, TLS statico e dinamico, callback di avvio/uscita e modelli Win32 nominativi. Usa il livello CPU indipendentemente dai driver; caricamento DLL/CRT, GUI, SEH utente, thread e compatibilità Windows generale restano incompleti.

La memoria virtuale Windows aggiunge `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` e `FlushInstructionCache` per il processo corrente. Il livello OS gestisce le prenotazioni; `AddressSpace` resta responsabile delle pagine impegnate, dei permessi e della memoria sottostante. I test verificano modifiche al codice, errori di accesso e riutilizzo del budget di memoria.

Le allocazioni private supportano `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT`, `MEM_RELEASE` e `MEM_TOP_DOWN`, con allineamento delle prenotazioni a 64 KiB e pagine da 4 KiB. La sola prenotazione non consuma RAM guest. Un nuovo commit conserva i dati e aggiorna i permessi; il decommit restituisce le singole pagine. La verifica dell’intero intervallo e la preparazione delle allocazioni evitano modifiche parziali in caso di errori ordinari. La query restituisce la struttura x64/ARM64 da 48 byte e raggruppa pagine successive della stessa allocazione. Immagine, ambiente, heap, ingressi API e margini dello stack partecipano al posizionamento; l’identità dello stack coincide con il TEB. Se un `VirtualProtect` riuscito rende di sola lettura la posizione di output dei vecchi permessi, i nuovi permessi restano applicati, i byte di output non cambiano e la chiamata restituisce successo. Una modifica della protezione su pagine non confermate restituisce `ERROR_INVALID_ADDRESS`, scrive `PAGE_NOACCESS` nell’output dei vecchi permessi e lascia invariati i permessi delle pagine.

Le protezioni ammesse sono `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE_READ` e `PAGE_EXECUTE_READWRITE`. Pagine di guardia, sola esecuzione, copia su scrittura, modificatori di cache, pagine grandi, reset/write-watch/segnaposto e modifiche alle mappature interne del modello restano esplicitamente non supportati. Solo le allocazioni virtuali private possono essere decommesse o rilasciate. Non si aggiungono gestione delle eccezioni utente o prove su hardware ARM64 nativo.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

Il modello versionato a thread singolo carica EXE console PE32+ AMD64/ARM64 alla base preferita. Verifica byte originali e dati del loader, conserva permessi utente e intervalli di guardia non mappati dello stack. Rifiuta import sconosciuti, ordinali, associati o ritardati, load configuration/CFG, immagini managed, GUI e ingressi DLL. Verifica le rilocazioni senza cambiare base. I modelli API non sono DLL installate: gli elenchi del loader contengono solo l’EXE e `GetModuleHandleW` accetta soltanto NULL.

GS su x64 e x18 su ARM64 puntano a TEB con limiti dello stack, puntatore a sé, PID/TID, PEB, parametri, LastError e TLS. UTF-8 rigoroso diventa UTF-16; argv segue le regole di quoting Microsoft CRT. I nomi ambiente sono ASCII, i duplicati senza distinzione tra maiuscole sono rifiutati, i valori possono essere Unicode e il blocco ordinato termina con due NUL. Non eredita ambiente o filesystem host. TLS statico copia template, azzera BSS e scrive un indice a 32 bit; TLS dinamico usa slot TEB separati. Attach/detach legge l’array corrente in ordine con budget e scadenza condivisi. Ritorno dall’ingresso e uscita normale eseguono detach; l’uscita ricorsiva durante detach si arresta esplicitamente.

`WindowsProcessServices.def` definisce `ExitProcess`, `RtlExitUserProcess`, handle di output e `WriteFile` sincrono, LastError, ID e pseudo-handle processo/thread, `GetCommandLineW`, allocazione/liberazione/dimensione heap, TLS dinamico e NULL `GetModuleHandleW`. Risolve nomi esatti in `kernel32.dll`, `kernelbase.dll`, `ntdll.dll`. Syscall dirette e gate falsi non selezionano modelli. L’heap appartiene al processo e viene recuperato; l’output mantiene i byte binari. Gli errori Win32 sono distinti da I/O asincrono ed eccezioni utente non implementati. Gli alias rispettano l’azzeramento iniziale del contatore e il vero slot di ritorno.

`windows.native_calls` conserva modulo/funzione, argomenti scalari dichiarati e risultati nullable senza inventare numeri NT. `NeverDWindowsProcessTests` verifica PE reali, TLS del compilatore, modifiche dei callback, heap, alias, metadati non validi, privilegi e budget; `NeverDProcessPublicTests` verifica CLI/C ABI. La CI Windows esegue direttamente lo stesso EXE come riferimento indipendente e richiede i test WHP. La prova runtime ARM64 nativa richiede ancora una macchina adatta.

Se il buffer di ingresso non è vuoto ma non è leggibile, `WriteFile` restituisce `ERROR_INVALID_USER_BUFFER` (1784), azzera il conteggio dei byte scritti e non produce alcun byte.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c).

<!-- i18n-section: verification -->

## Verifica

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# In una build shared-library/CLI:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

I test compilano ingressi ELF autonomi in assembly e C per entrambe le ISA: data/BSS, metadati di avvio reali, errori syscall, output binario, permessi, scritture parziali, servizi non supportati e budget tra quanti. TLS inizializza blocchi allineati indipendenti, BSS e puntatori thread e ne verifica la conservazione; x64 controlla gli errori `arch_prctl` senza perdere la base precedente. I backend non disponibili sono esplicitamente saltati. La suite pubblica confronta report e codici di uscita attraverso ABI C condivisa e CLI. PIE verifica auxv e slot RELA inizialmente nulli prima delle proprie rilocazioni di dati e funzioni. I test di mappatura conservano i fixup scegliendo la fonte di analisi; le tabelle dinamiche coprono sezioni assenti e input malformati o dipendenti. Entrambe le ISA provano allocazione, protezione, buchi, rimappatura, crescita/riduzione dell'heap ed errori gestiti. Vere scritture guest verificano fault dopo protezioni ordinarie e parziali. x64 riscrive codice allo stesso indirizzo tra RW e RX e chiama entrambe le versioni; lo stesso ELF gira nativamente su Linux come oracolo indipendente. I test di memoria coprono esaurimento, recupero e snapshot autorevoli senza trattenere RAM. Cross-compilazione e Unicorn ARM64 non sono evidenza di ARM64 KVM/WHP nativo.
