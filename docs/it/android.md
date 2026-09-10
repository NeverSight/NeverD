**Lingue**: [English](../android.md) | [简体中文](../zh-CN/android.md) | [繁體中文](../zh-TW/android.md) | [日本語](../ja/android.md) | [한국어](../ko/android.md) | [Français](../fr/android.md) | [Deutsch](../de/android.md) | [Español](../es/android.md) | [Italiano](android.md) | [Русский](../ru/android.md) | [العربية](../ar/android.md)

# Ricostruzione Java per Android

[← Indice della documentazione](README.md)

`neverd mobile` recupera Java leggibile da APK, DEX e smali usando per impostazione predefinita il motore integrato di NeverD. I lettori, implementati in modo indipendente, condividono un modello Dalvik tipizzato e un generatore Java con lavoro limitato. Questa funzione CLI sperimentale non promette parità funzionale con JADX né il recupero completo di qualsiasi APK. I contenitori APK e l’output Java non sono disponibili tramite SDK C nativo, SDK dei plugin Python, caricatore GUI o `neverd decompile --language`.

Il Java prodotto è una ricostruzione del bytecode. Commenti, formattazione, scelte del linguaggio sorgente originale e identificatori rimossi non sono disponibili; anche il bytecode Kotlin produce Java. Un’esecuzione riuscita non dimostra l’equivalenza semantica e non garantisce che ogni metodo possa essere ricompilato. Questo flusso non avvia le applicazioni analizzate.

## Avvio rapido

Dopo aver preparato gli ambienti di esecuzione descritti sotto, scegli una nuova directory di output:

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Apri `recovered-app/sources/` per leggere i file Java e `recovered-app/report.json` per consultare l’inventario degli input e i limiti. Se le classi smali si richiamano tra loro, è preferibile fornire una directory.

## Preparazione degli ambienti di esecuzione

| Componente | Requisito | Selezione |
|------------|-----------|-----------|
| NeverD | Compilare il target `neverd` con una toolchain compatibile con C++20. Il flusso mobile è integrato nella CLI nativa e non richiama un interprete Python. Distribuire l’eseguibile con le librerie native richieste dalla propria compilazione. | `build/bin/neverd` / PATH |

Il motore predefinito è implementato in C++20 e non richiede Python, Java o JADX durante l’esecuzione. Accetta dichiarazioni e operazioni comuni rappresentabili di DEX 035, 037–040 e smali. DEX 041, chiamate dinamiche come `invoke-custom`, alcuni percorsi di inizializzazione, annotazioni semantiche od operazioni sconosciute e identificatori non esprimibili in Java falliscono esplicitamente. Accettare un formato non significa supportarne tutte le istruzioni e dichiarazioni.

La risoluzione dei nomi Java distingue l’intestazione dal corpo della classe e può gestire i casi noti di oscuramento dei nomi nello stesso package; rifiuta esplicitamente i casi in cui mancano dichiarazioni di superclassi o interfacce esterne e non è possibile determinare i tipi o i riferimenti del codice ausiliario Java generato, assumendo soltanto per `java.lang.Object`, anche senza una dichiarazione disponibile, l’assenza di tipi membro ereditabili. I letterali in virgola mobile di smali vengono arrotondati direttamente alla precisione singola o doppia di destinazione, preservando la rappresentazione in bit risultante.

Il motore C++ integrato conserva e verifica i metadati `Signature` supportati di classi, campi e metodi, comprese variabili di tipo, array, wildcard, limiti di tipo e oscuramento dei nomi a livello di metodo. La cancellazione dei tipi deve corrispondere all’identità della dichiarazione DEX originale. `Throws` viene conservato; una gerarchia di eccezioni non dimostrabile viene rifiutata. Ereditarietà generica o sostituzione dei tipi dei membri, rigenerazione dei metodi ponte, chiamate a metodi generici, tipi interni parametrizzati e corrispondenza dei parametri nascosti dei costruttori restano esplicitamente non supportati senza le prove necessarie.

La CI elabora esempi Java 8 propri del progetto con D8 e NeverD, ricompila tutto il Java generato e confronta i metadati `Signature` completi, i risultati della reflection e il comportamento. Queste verifiche non certificano il recupero completo di applicazioni reali.

### Linux e macOS

```sh
cmake --build build --target neverd

./build/bin/neverd mobile app.apk -o recovered-app
```

Né `NEVERD_JADX` né un eseguibile `jadx` nel PATH selezionano il motore esterno: serve un `--jadx PATH` esplicito. Non è previsto alcun ripiego automatico. Racchiudere tra virgolette i percorsi con spazi.

### Windows PowerShell

```powershell
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app
```

Le compilazioni multiconfigurazione possono collocare l’eseguibile in `build/bin/Release/`. Seguire i normali requisiti di distribuzione delle librerie native di quella compilazione.

## Input supportati e confini

| Input | Comportamento | Limite importante |
|-------|---------------|-------------------|
| `.apk` | Convalidare l’intero ZIP, quindi analizzare insieme tutti i `classes.dex`, `classes2.dex` e i successivi DEX numerati nella radice dell’archivio | Solo codice; nessuna decodifica di risorse o manifest |
| `.dex` | Validazione e analisi di DEX 035 o 037–040 tramite il lettore integrato | DEX 041 e dichiarazioni od operazioni non supportate falliscono; rinominare o troncare un file non produce bytecode valido |
| `.smali` | Analizzare la classe fornita | Le classi adiacenti referenziate non vengono caricate implicitamente |
| Directory smali | Raccogliere ricorsivamente i file `.smali` e analizzarli insieme | Includere le classi annidate e le radici smali dipendenti nella directory di input |

Per analizzare un albero di APK decodificato che contiene `smali/` e `smali_classes2/`, passa la loro directory padre comune. Solo i file `.smali` arrivano al backend, ma prima viene convalidato e copiato l’intero albero fornito; anche gli asset voluminosi non pertinenti contribuiscono quindi ai limiti di input. Una directory compatta che contiene solo le radici smali rilevanti riduce il lavoro.

Gli APK suddivisi sono input separati. Ogni APK che contiene DEX può essere elaborato indipendentemente, ma questo comando non unisce un insieme di APK; le parti contenenti solo risorse falliscono perché non hanno DEX nella radice. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat` e `.vdex` non sono accettati come input mobili. Il supporto di alcuni di questi formati nel backend non implica che siano supportati da questo comando NeverD.

Le risorse APK, `AndroidManifest.xml`, gli asset, le librerie JNI/native e il codice scaricato durante l’esecuzione non vengono ricostruiti in Java. Estrai separatamente una libreria nativa `.so` e usa `neverd decompile library.so -o library.c`. Per questo flusso statico, i payload cifrati o protetti da un packer devono essere già disponibili come normali DEX/smali; non vengono eseguiti rimozione del packing, collegamento a dispositivi o aggiramento delle protezioni.

## Opzioni e precedenza

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --timeout=600 --max-files=30000 --max-bytes=4294967296 --json
```

| Opzione | Valore predefinito | Significato |
|---------|-------------------|-------------|
| `-o DIRECTORY` | Obbligatoria | Nuova directory di output esterna a qualsiasi directory di input; non sovrascrivere mai un output esistente |
| `--platform=auto\|android` | `auto` | Selezionare Android esplicitamente o dedurre la piattaforma dall’input |
| `--jadx PATH` | Non impostato: motore integrato | Seleziona esplicitamente l’adattatore di compatibilità JADX installato separatamente; nessuna selezione dall’ambiente né ripiego automatico |
| `--timeout N` | `300` | Budget di tempo positivo per l’analisi integrata; secondi per processo esterno, inclusa la verifica della versione |
| `--max-files N` | `20000` | Limite positivo del numero di voci, comprese le directory create |
| `--max-bytes N` | `2147483648` | Limite positivo in byte per input, dati estratti e output finale |
| `--json` | Disattivato | Stampare il report in JSON anziché come riepilogo per l’utente |

Una selezione `--arch` diversa da quella predefinita, `--artifact`, `--metadata-only` e un `--max-func` diverso da zero appartengono a iOS e vengono rifiutati per Android; `--arch=auto` esplicito è accettato. Non è previsto l’inoltro di opzioni arbitrarie al backend. L’adattatore JADX esplicito isola configurazione, cache e directory temporanee, senza importare impostazioni ambientali del backend o configurazioni dei plugin.

Input, dati estratti e output finale conservano i budget di file e byte. I lettori e il generatore integrati controllano anche lavoro limitato e tempo trascorso. Le aree di lavoro esterne consentono fino al triplo dei budget di voci e byte per ospitare input preparati e risultati intermedi; i log sono limitati a 16 MiB per processo. Sono controlli delle risorse, non una sandbox. Aumentare un limite non disattiva gli altri.

## Struttura dell’output e report JSON

```text
recovered-app/
  sources/                       package e classi Java recuperati
  metadata/android-methods.json  copertura dei metodi del motore integrato
  report.json                    inventario con versione e limiti
```

Gli input temporanei vengono rimossi. Le classi annidate possono condividere il file della classe esterna, quindi il numero di file Java non equivale al numero di classi DEX. I metodi generati possono usare un ciclo di dispatch Java; non eseguono il DEX originale né lo richiamano tramite un ponte a runtime.

Il report integrato include `android_method_recovery`, scritto anche in `metadata/android-methods.json`, e conserva ogni metodo originale. Vale `method_count = recovered_method_count + projected_method_count + declaration_only_method_count + unrecovered_method_count`; un `projected_method_count` assente vale zero e `unrecovered_method_count` resta zero prima della pubblicazione. I metodi originali `native` e `abstract` hanno stato `declaration-only` e non contano come corpi recuperati.

Un sottoinsieme di classi locali con nome e senza catture può essere emesso nel metodo statico contenitore esatto. Sono richiesti un metodo ordinario con tipi scalari, una classe priva di campi che estenda direttamente `Object`, un vero costruttore senza argomenti, metodi di istanza scalari e usi degli oggetti verificati che non escano dall’ambito supportato. Classi anonime, catture, modificatori non supportati e usi non dimostrati continuano a fallire esplicitamente.

I metodi locali e il metodo contenitore ricevono `source-projected`, con `projection_kind: "named-method-local"`; la copertura rimane `partial` anche quando il report generale indica `success`. I nomi binari e i flag di accesso dopo la ricompilazione restano non verificati. Il compilatore Java può scegliere un nome binario diverso, quindi `class_source_bindings` conserva classe originale, metodo contenitore esatto, percorso sorgente e nome locale con `binary_name_status: "unverified"`.

`generated_source_helpers` elenca i metodi aggiuntivi con i tipi esatti `throw-helper`, `constant-helper`, `default-constructor` e `field-initializer`. L’ultimo indica un `<clinit>` generato aggiuntivo, assente dall’inventario dei metodi originali. Queste aggiunte non rientrano nel totale dei metodi originali. Una compilazione riuscita o una singola corrispondenza dei nomi non dimostra un recupero completo. L’esempio abbreviato seguente non contiene metodi proiettati:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {
    "name": "neverd",
    "version": "1",
    "execution": "builtin"
  },
  "input_code_files": [
    "classes.dex",
    "classes2.dex"
  ],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": [
    "sources/example/Main.java",
    "sources/example/Peer.java"
  ],
  "logs": [],
  "android_method_recovery": {
    "schema_version": 1,
    "status": "recovered",
    "class_count": 2,
    "method_count": 6,
    "recovered_method_count": 5,
    "declaration_only_method_count": 1,
    "unrecovered_method_count": 0
  }
}
```

`input_kind` è `apk`, `dex`, `smali` o `smali-directory`. `input_code_files` elenca i nomi del bytecode o i percorsi smali di input, mentre `java_sources` e `logs` sono relativi alla radice dell’output. `source` è il nome base dell’input. I report effettivi includono altri limiti di ricostruzione; conservali quando presenti i risultati ad altri strumenti.

Per l’automazione, controlla il codice di uscita del processo prima di leggere `status` e, quando reindirizzi stdout, salva il report fuori dalla nuova directory di output:

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

La CLI nativa restituisce zero in caso di successo e un valore non nullo in caso di errore. Con `--json`, gli errori gestiti includono `schema_version`, `status: "error"` ed `error`. Parsing degli argomenti, errori di avvio dell’eseguibile o delle librerie native e interruzioni possono essere segnalati soltanto su stderr. Controllare prima lo stato di uscita.

## Gestione degli errori e risoluzione dei problemi

La pubblicazione è transazionale: l’output esistente viene conservato e i risultati temporanei falliti vengono rimossi. Operazioni non supportate, flussi di registri irrisolti, dichiarazioni non rappresentabili, gestione delle eccezioni malformata e budget esauriti fanno fallire il motore integrato anziché pubblicare corpi mancanti. L’adattatore esterno rifiuta anche uscite non nulle, errori di assemblaggio o decompilazione nei log, omissioni di classi duplicate, marcatori di codice incompleto, file Java vuoti e assenza di Java. Un recupero riuscito non prova l’equivalenza semantica.

| Sintomo | Azione |
|---------|--------|
| DEX, istruzione, dichiarazione o inizializzazione non supportati | Leggere il messaggio diagnostico e verificare il sottoinsieme supportato. Usare `--jadx PATH` solo scegliendo deliberatamente l’adattatore separato |
| Input non valido o classe duplicata | Correggere bytecode o insieme di classi; i corpi non supportati non vengono omessi silenziosamente |
| Tempo o budget superato | Ridurre l’input o adattare `--timeout`, `--max-files` e `--max-bytes` alle risorse disponibili |
| Output già esistente | Scegliere una nuova directory di output |

## Adattatore opzionale di compatibilità JADX

`--jadx PATH` seleziona JADX esterno, non l’implementazione integrata. Installare JADX 1.5.6 o successivo con i plugin standard di input DEX/smali e Java 11 o successivo. Ottenere la [distribuzione completa di JADX](https://github.com/skylot/jadx/releases/tag/v1.5.6), conservarne la struttura `bin/` e `lib/` e, in caso di ridistribuzione, le licenze delle dipendenze incluse. Nulla viene scaricato automaticamente. Il report dell’adattatore identifica il motore effettivo `jadx` e la versione rilevata; non dichiara la copertura dei metodi del motore integrato.

Su Windows, indicare il launcher `.bat`/`.cmd` della distribuzione oppure `lib/jadx-*-all.jar`. NeverD individua il JAR e avvia Java direttamente: i percorsi dell’applicazione non passano per una shell di comandi. `JAVA_HOME` o PATH selezionano Java. Le esecuzioni riuscite conservano `logs/jadx-version.log` e `logs/jadx.log`; directory temporanee e log dei tentativi falliti vengono rimossi. Solo un’uscita non nulla del backend include una coda limitata del log. Errori di avvio, timeout e superamenti dei budget hanno messaggi specifici.

```sh
neverd mobile app.apk -o recovered-jadx --jadx /opt/jadx/bin/jadx
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

## Verifica e profondità del supporto

Python serve soltanto agli script di test di sviluppo riportati sotto; il recupero mobile integrato viene eseguito nella CLI nativa C++20.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
```

I test dei componenti e della CLI controllano parsing, contratti di output e pulizia dopo gli errori. Il runner interno di confronto usa un JDK (`java` e `javac`) e D8 per costruire esempi DEX/APK indipendenti, poi compilare ed eseguire il Java recuperato. Sono dipendenze di test, non requisiti per il recupero integrato. Eseguirlo sulla build attuale ed esaminarne i risultati prima di dichiarare verificato un caso. Il runner di compatibilità separato richiede anche JADX e verifica quell’adattatore. Il successo degli esempi non dimostra il recupero completo di qualsiasi applicazione.

Vedere la [panoramica mobile](../mobile.md) per il flusso iOS correlato.
