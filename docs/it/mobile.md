**Lingue**: [English](../mobile.md) | [简体中文](../zh-CN/mobile.md) | [繁體中文](../zh-TW/mobile.md) | [日本語](../ja/mobile.md) | [한국어](../ko/mobile.md) | [Français](../fr/mobile.md) | [Deutsch](../de/mobile.md) | [Español](../es/mobile.md) | [Italiano](mobile.md) | [Русский](../ru/mobile.md) | [العربية](../ar/mobile.md)

# Recupero delle applicazioni mobili

[← Indice della documentazione](README.md) · [Guida Android completa](android.md) · [Guida iOS completa](ios.md)

`neverd mobile` recupera Java leggibile da input Android APK, DEX e smali. Per input iOS IPA, `.app` e Mach-O, esporta C nativo e ricostruisce i corpi dei metodi Objective-C supportati in sorgenti `.m`, insieme a sorgenti Swift sperimentali e metadati runtime. Questo è un flusso CLI sperimentale; i contenitori mobili non sono accettati dall’SDK C nativo o dal caricatore GUI.

## Preparazione

Compila il target `neverd` con supporto C++20. Il flusso mobile è compilato nella CLI nativa e non usa un interprete Python. Distribuisci l’eseguibile con le librerie native richieste dalla tua build.

La gestione ZIP nativa usa zlib per CRC-32 e DEFLATE. CMake preferisce una libreria installata tramite `find_package`; altrimenti scarica zlib 1.3.2 con SHA256 fissato e la compila staticamente. Questa implementazione ZIP mobile non richiede strumenti ausiliari Python su Windows. Conserva gli avvisi sulle dipendenze in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).

Il motore predefinito è implementato in C++20 e non richiede runtime Python, Java o JADX. Solo un `--jadx PATH` esplicito seleziona l’adattatore di compatibilità installato separatamente; `NEVERD_JADX` e PATH non lo selezionano automaticamente e non esiste un ripiego automatico. L’adattatore facoltativo richiede JADX 1.5.6+ con i plugin standard per input DEX/smali e Java 11+. Il suo report indica il motore `jadx` effettivo e la versione. Installazione e licenze delle dipendenze restano documentate nella [guida Android](android.md#adattatore-opzionale-di-compatibilità-jadx).

## Android

Per un elenco rapido delle classi senza recupero Java, usa
`neverd mobile app.apk --list-classes`, eventualmente con
`--class-prefix com.example` o `--json`. Il
[contratto dell’inventario](android.md#inventario-rapido-delle-classi) descrive
ordinamento, limiti e convalida dei payload selezionati. La modalità query non
richiede una directory di output; il suo `-o` facoltativo indica un nuovo file.

Per i riferimenti diretti nel bytecode, usa
`neverd mobile app.apk --find-refs string --query 'example' --json`.
Il [contratto delle query sui riferimenti](android.md#query-sui-riferimenti-nel-codice)
copre anche operandi di tipo, metodo e campo, corrispondenza letterale/esatta,
posizioni di ogni occorrenza, preservazione UTF-16 e ambito della convalida del
codice. Senza `--json`, questa operazione emette JSON Lines. Condivide il
comportamento di `-o` che richiede un nuovo file.

Gli esempi e l’output seguenti descrivono il recupero.

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Tutti i file `classes.dex`, `classes2.dex` e i successivi DEX numerati nella radice di un APK vengono analizzati insieme. Una directory smali viene esplorata ricorsivamente e tutte le sue classi vengono analizzate in una sola invocazione, incluse classi annidate e adiacenti. Usa una directory per recuperare classi che si riferiscono tra loro. Un singolo file smali fornisce solo quella classe.

L’output del recupero integrato contiene `sources/`, `metadata/android-methods.json` e `report.json`, con `backend: {"name": "neverd", "version": "1", "execution": "builtin"}`. Il report incorpora `android_method_recovery`: `method_count = recovered_method_count + declaration_only_method_count`, e `unrecovered_method_count` deve essere zero prima della pubblicazione. Le dichiarazioni originali `native`/`abstract` vengono contate separatamente dai corpi recuperati. L’adattatore esterno esplicito conserva i propri log del backend. Risorse APK, manifest, librerie native e codice caricato dinamicamente sono esclusi da questo percorso Java; le librerie native possono essere analizzate separatamente con `neverd decompile`.

I lettori di recupero integrati condividono un modello Dalvik tipizzato implementato indipendentemente e un generatore Java con lavoro limitato per codice ordinario rappresentabile DEX 035/037–040 e smali. DEX 041, chiamate dinamiche come `invoke-custom`, alcuni percorsi di inizializzazione, operazioni sconosciute e identificatori non rappresentabili in Java falliscono esplicitamente. Il Java generato può usare un ciclo di dispatch; non esegue il DEX originale né lo chiama attraverso un ponte runtime. Commenti originali, formattazione e nomi rimossi non possono essere ripristinati. Il motore sperimentale non promette parità funzionale con JADX, equivalenza semantica o recupero completo di APK arbitrari.

## iOS

La [guida iOS completa](ios.md) documenta la selezione di IPA, `.app` e Mach-O, la preparazione, tutte le opzioni CLI, gli schemi dei sorgenti, la copertura e la verifica.

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

Ogni esecuzione seleziona un eseguibile. `--artifact` è relativo al bundle dell’applicazione sia per IPA sia per `.app`. La selezione da binari fat preferisce arm64, arm, x86_64 e poi i386; la proiezione nel linguaggio sorgente si rivolge ad arm64/x86_64. Le slice selezionate cifrate vengono rifiutate. Il percorso nativo sperimentale emette C e i corpi dei metodi Objective-C supportati, inclusi binding ABI scalari/puntatore, in virgola mobile, misti e sullo stack. Le disposizioni runtime di classi/ivar e le categorie separate vengono conservate quando convalidate; disposizioni, firme, chiamate e altre dipendenze irrisolte restano omissioni esplicite.

Il recupero Swift classifica le firme nel processo C++ usando `LLVMSwiftDemangle` del fork LLVM di NeverD. Non avvia demangler esterni o comandi di ricerca della toolchain e non richiede un compilatore Swift installato per compilare o eseguire NeverD. Le build dai sorgenti del fork e i pacchetti LLVM corrispondenti includono il componente; NeverD non scarica una dipendenza sorgente Swift separata. L’inventario delle firme registra `demangler: {"name": "llvm-swift-demangle", "execution": "builtin", "version": "6.3.3"}`. Le firme supportate vengono associate a ingressi nativi e posizioni ABI prima di emettere vere funzioni `.swift`, metodi/inizializzatori di classe e metodi di strutture a disposizione fissa. Forme generiche/resilienti, asincrone/con eccezioni, forme chiamabili generate a runtime non supportate e gruppi incompleti di dipendenze sorgente restano non recuperati.

L’output normale include `sources/native.c`, gli eventuali `sources/objc.m` e `sources/swift.swift`, dichiarazioni e metadati runtime, JSON di copertura di metodi/firme, log, `artifacts/selected.macho` e `report.json`. Non ci sono log di ricerca di toolchain Swift esterne o di demangling esterno. Il sorgente generato non chiama il binario originale come ponte di recupero. Swift `source_units` raggruppa dichiarazioni di tipi e metodi; le righe dei singoli metodi non devono essere concatenate per ricostruire le classi. Il campo esterno `status: "success"` significa pubblicazione dell’output, non copertura completa dei metodi o equivalenza semantica.

`--metadata-only` non esegue né l’esportatore di sorgenti nativi né il demangling delle firme e non emette sorgenti o copertura dei metodi. Tutte le modalità usano i metadati Objective-C risolti dal caricatore nativo. I metadati Swift usano letture limitate dell’immagine nativa; fixup non supportati, disposizioni rilocabili o riferimenti conservano diagnostica parziale. `--max-func` limita il recupero delle funzioni native ed è ignorato in modalità solo metadati. Corpi di funzioni native mancanti fanno fallire un’esecuzione normale. Gli input temporanei estratti vengono rimossi.

Commenti originali, formattazione, identificatori rimossi e strutture del sorgente perse in compilazione non possono essere ricostruiti esattamente. Prima di usare l’output, leggi lo stato e il motivo del recupero di ogni metodo, i conteggi separati Swift di elementi chiamabili/non chiamabili/non classificati e i limiti documentati.

## Limiti ed errori

Per il recupero, `-o` deve indicare una nuova directory esterna a qualsiasi directory di input. Le modalità query accettano invece un nuovo file di output facoltativo. L’output esistente non viene mai sovrascritto. Il lavoro di recupero viene preparato in un’area temporanea e pubblicato solo dopo il successo del recupero e della convalida dell’output. I risultati delle query vengono accumulati finché tutti i DEX selezionati non superano i controlli. Le esecuzioni riuscite della CLI nativa restituiscono zero. Gli errori di recupero restituiscono un valore diverso da zero; `--json` riporta gli errori gestiti con `schema_version`, `status: "error"` e `error`. Errori di analisi degli argomenti, avvio dell’eseguibile nativo o delle librerie e interruzioni possono invece essere riportati su stderr. Chi usa il risultato deve controllare prima lo stato di uscita.

I valori predefiniti sono 20.000 voci, 2 GiB di dati di input/estratti o di output finale e 300 secondi per l’analisi integrata Android/iOS o per ogni processo JADX esplicito. I processi figli iOS ricevono il budget di analisi totale rimanente. Il lettore e il generatore integrati applicano anche un budget di lavoro limitato. Imposta `--max-files`, `--max-bytes` e `--timeout` per modificare questi limiti positivi. L’area di lavoro temporanea viene monitorata durante l’esecuzione dei backend, con fino a tre volte i limiti di voci/byte per consentire la coesistenza di input preparati e output intermedi. La diagnostica è limitata a 16 MiB per processo.

Durante il recupero, la preparazione degli APK scrive solo `classes.dex`, `classes2.dex` e i successivi DEX numerati nella radice. Ogni membro ZIP è comunque sottoposto a controlli di header/intervallo, decompressione e convalida di lunghezza e CRC, e contribuisce ai limiti di voci e byte non compressi dell’archivio. Le risorse non scritte possono avere nomi distinti per maiuscole e minuscole come `res/-A.xml` e `res/-a.xml`. Nomi ZIP esattamente duplicati e conflitti di identità tra file e directory restano errori; i controlli portabili delle collisioni tra maiuscole/minuscole del filesystem si applicano ai membri effettivamente scritti. L’estrazione completa, compreso l’input IPA, continua a rifiutare tali collisioni di output. Percorsi di attraversamento, collegamenti, file speciali e voci ZIP cifrate restano rifiutati in tutto l’archivio. Anche gli input directory rifiutano collegamenti simbolici e file speciali.

Questi limiti sono controlli di robustezza, non una sandbox per codice backend di terze parti. I comandi JADX espliciti e di esportazione dei sorgenti nativi vengono eseguiti come processi figli locali. L’output temporaneo di un’operazione fallita viene rimosso. Le uscite non nulle dei backend includono una coda diagnostica limitata. I timeout dei backend conservano il messaggio di timeout e aggiungono una coda limitata quando è disponibile testo di log acquisito. Errori di avvio e violazioni dei budget mantengono i propri messaggi di errore.

## Validazione

Python viene usato solo dai sistemi di test di sviluppo seguenti; il recupero mobile integrato viene eseguito nella CLI nativa C++20.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

I test dei componenti coprono analisi, contenitori non sicuri, errori dei backend, pulizia dell’output, selezione dell’architettura e preservazione dell’output. I test che invocano la CLI compilata usano `NEVERD_BUILD_DIR`. Il runner Android interno usa un JDK (`java` e `javac`) e D8 per creare fixture DEX/APK indipendenti, quindi compilare ed eseguire il Java recuperato. Queste sono dipendenze di verifica, non requisiti del recupero integrato. Esegui i test con la build corrente e ispeziona il risultato prima di considerare verificato un caso. Il runner di compatibilità separato richiede anche JADX; il successo delle fixture non dimostra il recupero di applicazioni arbitrarie.

Su macOS con Apple Clang, il relativo SDK e una build di NeverD, esegui il confronto reale dell’esecuzione Objective-C:

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

Il runner compila la propria fixture Objective-C, ne recupera le implementazioni dei metodi e collega soltanto il `.m` recuperato con lo stesso sistema di chiamata indipendente. La sua fixture di 22 metodi confronta 141 risultati osservabili che coprono limiti degli interi, rami, cicli, letture/scritture tramite puntatori, argomenti nascosti e inutilizzati, bit di identità di float/double, parametri misti e posizioni sullo stack. È necessaria un’esecuzione corrente riuscita prima di considerare verificato un caso. Ogni architettura e variante di fixup richiesta deve completarsi; per impostazione predefinita vengono coperte arm64/x86_64 × classic/default. Una variante mancante o un’architettura che l’host non può eseguire è un errore; non sono consentiti test saltati. Queste evidenze della fixture non dimostrano completezza per programmi iOS arbitrari. `NeverDMobileIOSBackend` è registrato in CTest su macOS, incluso il profilo di test CI principale.

Il runner indipendente di recupero Swift è `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd`. Ricompila lo Swift generato e il suo sistema di test senza collegare il binario originale; elementi chiamabili non supportati e differenze di comportamento sono errori. Consulta la [guida iOS](ios.md) per la semantica della copertura e le evidenze degli errori conservate.

Il [workflow Mobile Real Applications](../../.github/workflows/mobile-real-apps.yml) usa applicazioni pubbliche fissate nel [manifest del corpus](../../scripts/mobile_real_apps.json). Il suo controllo di accettazione richiede inventari indipendenti di ogni DEX in ciascun APK e di ogni Mach-O in ciascun bundle iOS completo, ricompilazione indipendente degli originali e del sorgente generato e confronti del comportamento. Fasi mancanti, copertura dell’inventario sconosciuta o casi obbligatori mancanti fanno fallire il controllo. Le fasi di ricompilazione e confronto del comportamento delle applicazioni reali restano incomplete, quindi il supporto mantiene l’etichetta Experimental; superare i test dei controlli del sistema di verifica non dimostra il successo sulle applicazioni reali.

I casi Android tentano di compilare l’intero insieme inventariato di Java generato con javac e D8, usando solo dichiarazioni dell’SDK Android. Il bytecode originale dell’applicazione, le implementazioni delle dipendenze e gli stub sostitutivi non possono colmare il recupero mancante. Il recupero parziale e gli errori del compilatore restano nelle evidenze; una compilazione riuscita richiede ancora verifica indipendente, ricostruzione completa dell’APK e confronto del comportamento ART. L’oracolo iOS riconcilia i record dei metodi Objective-C su disco con l’output degli strumenti Apple, preservando identità di classe, metaclasse, categoria, lista e posizione ordinale. Puntatori irrisolti o slot omessi lasciano sconosciuto l’inventario. Inventari separati delle dichiarazioni SDK per dispositivo e simulatore supportano il lavoro sulle importazioni dei framework senza trattare gli header come prova della disposizione delle istanze o del comportamento.
