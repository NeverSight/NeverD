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

Il loader nativo collega record del metodo, indirizzo IMP eseguibile e codifica di tipo supportata a posizioni ABI sorgente esplicite. I parametri fissi scalari/puntatori mantengono `self`/`_cmd`, argomenti inutilizzati, banchi interi/flottanti separati e posizioni sullo stack supportate. Reinterpretazione dei bit float/double e conversione numerica sono distinte. I suggerimenti di tipo sono input per la proiezione sorgente, non prove ABI autenticate né autorizzazione a modificare codice eseguibile.

`sources/objc.m` colloca le istruzioni realmente ricostruite nei metodi `@implementation`, mantenendo helper C e chiamate tipizzate necessarie. Le destinazioni richiedono un collegamento sorgente supportato; destinazioni ignote e gruppi di dipendenze incompleti restano non recuperati. Definizioni mancanti, indirizzi non eseguibili, codifiche discordanti, ABI non gestite, decodifica incompleta e IR rifiutato non diventano metodi recuperati solo perché esiste una dichiarazione.

I metadati mantengono identità della superclasse, inizio/dimensione dell’istanza e ivar scalari/puntatori con offset, larghezze e allineamenti verificati. Le dichiarazioni inseriscono padding dove necessario. I metodi che richiedono un layout indisponibile restano non recuperati. Le categorie conservano identità classe/categoria/indirizzo e implementazioni separate; record identici ripetuti nei due inventari contano una volta. Le categorie esterne supportate usano dichiarazioni Foundation esistenti. Gli header esterni sconosciuti sono dipendenze mancanti; non si inventa una classe sostitutiva.

Il recupero richiede anche dichiarazioni di classe complete, definizioni delle classi antenate locali e sorgente in cui i valori siano definiti prima dell’uso su ogni percorso e le uscite raggiungibili abbiano il ritorno richiesto; una superclasse locale vuota riceve una `@implementation` vuota solo se il layout verificato e un inventario completo dimostrano l’assenza di metodi ordinari propri. Se mancano queste prove o un nome è in conflitto con un’importazione Foundation nota, i metodi interessati restano nel denominatore della copertura con le motivazioni, mentre le classi recuperabili indipendentemente continuano a essere emesse; questi controlli dei nomi non coprono tutti i nomi degli SDK, gli SDK iOS o le versioni.

Le chiamate ai Block Objective-C supportate richiedono un’ABI scalare fissa completa, comprendente l’oggetto Block implicito e tutte le posizioni degli argomenti e del risultato. La codifica di runtime `@?` viene ampliata a `id` soltanto nelle dichiarazioni; non fornisce il prototipo di invocazione. I riferimenti ai Block globali mantengono l’identità dell’oggetto condiviso. Le catture scalari sincrone supportate richiedono una prova della memoria nativa delle catture e del flusso di invocazione. Catture che sfuggono al contesto o asincrone, proprietà di oggetti/byref non modellata, funzioni copy/dispose e layout sconosciuti restano non recuperati.

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

Per una sessione con Mach-O già caricato, `neverd_objc_methods_json(session, max_functions)` e `neverd_swift_methods_json(session, signatures_json, max_functions)` restituiscono i rapporti. Zero seleziona tutte le funzioni scoperte. Liberare le stringhe riuscite con `neverd_free_string`; `NULL` indica un errore spiegato nello stato della sessione. Le API non caricano contenitori IPA o `.app`.

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
