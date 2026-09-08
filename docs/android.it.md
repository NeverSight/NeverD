**Lingue**: [English](android.md) | [简体中文](android.zh-CN.md) | [繁體中文](android.zh-TW.md) | [日本語](android.ja.md) | [한국어](android.ko.md) | [Français](android.fr.md) | [Deutsch](android.de.md) | [Español](android.es.md) | [Italiano](android.it.md) | [Русский](android.ru.md) | [العربية](android.ar.md)

# Ricostruzione Java per Android

[← Indice della documentazione](README.it.md)

`neverd mobile` ricostruisce Java leggibile da input APK, DEX e smali tramite un backend JADX installato separatamente. Il flusso convalida il bytecode, prepara copie di lavoro, analizza insieme le classi correlate, controlla l’output generato e pubblica una directory di sorgenti con un report leggibile dalle applicazioni. È una funzionalità CLI sperimentale. I contenitori APK e l’output Java non sono disponibili tramite l’SDK C nativo, l’SDK dei plugin Python, il loader della GUI o `neverd decompile --language`.

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
| NeverD | Compilare il target `neverd`; distribuire anche la directory `mobile/` accanto all’eseguibile | `build/bin/neverd` o un eseguibile nel PATH |
| Python | Python 3.10 o successivo, indipendente dall’host dei plugin incorporato | `--python`, poi `NEVERD_PYTHON`, poi `python3`/`python` nel PATH |
| Backend Java | JADX 1.5.6 o successivo con i plugin standard per input DEX e smali | `--jadx`, poi `NEVERD_JADX`, poi `jadx` nel PATH |
| Runtime Java | Java 11 o successivo; per la verifica tramite compilazione ed esecuzione serve un JDK | `JAVA_HOME` o Java nel PATH |

NeverD non scarica automaticamente le dipendenze. Procurati la [distribuzione JADX](https://github.com/skylot/jadx/releases/tag/v1.5.6) completa, mantieni la struttura `bin/` e `lib/` e conserva le licenze incluse quando la ridistribuisci. La versione del backend verificata è la 1.5.6; le versioni successive devono rispettare lo stesso contratto CLI. La preparazione delle dipendenze è separata dalla compilazione della pipeline LLVM di NeverD.

### Linux e macOS

```sh
cmake --build build --target neverd
python3 --version
java -version
/opt/jadx/bin/jadx --version

./build/bin/neverd mobile app.apk -o recovered-app \
  --python python3 --jadx /opt/jadx/bin/jadx
```

Per l’uso ripetuto, imposta `NEVERD_JADX=/opt/jadx/bin/jadx` e, se necessario, indica il percorso dell’interprete in `NEVERD_PYTHON`. Se Java non è già disponibile, fai puntare `JAVA_HOME` alla directory di installazione del JDK. I percorsi che contengono spazi vanno racchiusi tra virgolette.

### Windows PowerShell

```powershell
$env:JAVA_HOME = 'C:\Tools\jdk'
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app `
  --python 'C:\Tools\Python\python.exe' `
  --jadx 'C:\Tools\jadx\bin\jadx.bat'
```

Il percorso `.bat`/`.cmd` del backend viene usato per individuare l’unico `lib/jadx-*-all.jar` della distribuzione; NeverD invoca Java direttamente. Puoi anche passare quel JAR a `--jadx`. I percorsi delle applicazioni non vengono mai inseriti in un comando shell. Le build con più configurazioni possono collocare l’eseguibile in `build/bin/Release/`. Spostare solo l’eseguibile senza la sua directory `mobile/` provoca un errore di helper mancante.

## Input supportati e confini

| Input | Comportamento | Limite importante |
|-------|---------------|-------------------|
| `.apk` | Convalidare l’intero ZIP, quindi analizzare insieme tutti i `classes.dex`, `classes2.dex` e i successivi DEX numerati nella radice dell’archivio | Solo codice; nessuna decodifica di risorse o manifest |
| `.dex` | Convalidare la firma DEX e lasciare al backend la decodifica del contenuto | Un file rinominato o troncato non è bytecode valido |
| `.smali` | Analizzare la classe fornita | Le classi adiacenti referenziate non vengono caricate implicitamente |
| Directory smali | Raccogliere ricorsivamente i file `.smali` e analizzarli insieme | Includere le classi annidate e le radici smali dipendenti nella directory di input |

Per analizzare un albero di APK decodificato che contiene `smali/` e `smali_classes2/`, passa la loro directory padre comune. Solo i file `.smali` arrivano al backend, ma prima viene convalidato e copiato l’intero albero fornito; anche gli asset voluminosi non pertinenti contribuiscono quindi ai limiti di input. Una directory compatta che contiene solo le radici smali rilevanti riduce il lavoro.

Gli APK suddivisi sono input separati. Ogni APK che contiene DEX può essere elaborato indipendentemente, ma questo comando non unisce un insieme di APK; le parti contenenti solo risorse falliscono perché non hanno DEX nella radice. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat` e `.vdex` non sono accettati come input mobili. Il supporto di alcuni di questi formati nel backend non implica che siano supportati da questo comando NeverD.

Le risorse APK, `AndroidManifest.xml`, gli asset, le librerie JNI/native e il codice scaricato durante l’esecuzione non vengono ricostruiti in Java. Estrai separatamente una libreria nativa `.so` e usa `neverd decompile library.so -o library.c`. Per questo flusso statico, i payload cifrati o protetti da un packer devono essere già disponibili come normali DEX/smali; non vengono eseguiti rimozione del packing, collegamento a dispositivi o aggiramento delle protezioni.

## Opzioni e precedenza

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --jadx /opt/jadx/bin/jadx --timeout=600 \
  --max-files=30000 --max-bytes=4294967296 --json
```

| Opzione | Valore predefinito | Significato |
|---------|-------------------|-------------|
| `-o DIRECTORY` | Obbligatoria | Nuova directory di output esterna a qualsiasi directory di input; non sovrascrivere mai un output esistente |
| `--platform=auto\|android` | `auto` | Selezionare Android esplicitamente o dedurre la piattaforma dall’input |
| `--jadx PATH` | Ambiente/PATH | Launcher del backend o JAR della distribuzione; l’opzione esplicita ha precedenza |
| `--python PATH` | Ambiente/PATH | Interprete per l’helper incluso; l’opzione esplicita ha precedenza |
| `--timeout N` | `300` | Numero positivo di secondi per processo backend, compreso il rilevamento della versione |
| `--max-files N` | `20000` | Limite positivo del numero di voci, comprese le directory create |
| `--max-bytes N` | `2147483648` | Limite positivo in byte per input, dati estratti e output finale |
| `--json` | Disattivato | Stampare il report in JSON anziché come riepilogo per l’utente |

Un valore di `--arch` diverso da quello predefinito, `--artifact`, `--metadata-only` e un valore non nullo di `--max-func` appartengono a iOS e vengono rifiutati per Android; `--arch=auto` è accettato anche quando viene indicato esplicitamente. Non esiste un inoltro arbitrario di opzioni al backend. Le directory di configurazione, cache e file temporanei del backend sono isolate per ogni esecuzione; impostazioni preesistenti e configurazioni dei plugin non vengono importate nel processo.

I limiti controllano le risorse, ma non costituiscono una sandbox per il processo backend. Anche l’area di lavoro temporanea viene monitorata, con spazio per input, dati estratti e output fino a tre volte i budget configurati di voci e byte. I log sono limitati a 16 MiB per processo. Gli input grandi possono comunque richiedere più heap Java o un timeout maggiore; aumentare un limite non disattiva gli altri.

## Struttura dell’output e report JSON

```text
recovered-app/
  sources/                 package e classi Java ricostruiti
  logs/jadx-version.log    rilevamento della versione del backend
  logs/jadx.log            diagnostica del backend
  report.json              inventario con versione e limiti di ricostruzione
```

Le copie temporanee e le cache del backend vengono rimosse. I nomi esatti dei file Java e il loro numero dipendono dalla ricostruzione del backend; le classi annidate possono condividere il file sorgente della classe esterna. Il numero di sorgenti Java non coincide quindi con quello delle classi DEX.

Un report illustrativo abbreviato:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {"name": "jadx", "version": "1.5.6"},
  "input_code_files": ["classes.dex", "classes2.dex"],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": ["sources/example/Main.java", "sources/example/Peer.java"],
  "logs": ["logs/jadx-version.log", "logs/jadx.log"],
  "limitations": ["Java is reconstructed from bytecode; original comments, formatting, and stripped names cannot be restored."]
}
```

`input_kind` è `apk`, `dex`, `smali` o `smali-directory`. `input_code_files` elenca i nomi del bytecode o i percorsi smali di input, mentre `java_sources` e `logs` sono relativi alla radice dell’output. `source` è il nome base dell’input. I report effettivi includono altri limiti di ricostruzione; conservali quando presenti i risultati ad altri strumenti.

Per l’automazione, controlla il codice di uscita del processo prima di leggere `status` e, quando reindirizzi stdout, salva il report fuori dalla nuova directory di output:

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

Le esecuzioni riuscite dell’helper restituiscono zero. Gli errori di ricostruzione restituiscono un valore non nullo; quando la gestione degli errori dell’helper è attiva, `--json` produce un oggetto di errore contenente `schema_version`, `status: "error"` ed `error`. Il parsing nativo degli argomenti, Python mancante, una versione di Python precedente alla 3.10 o un helper assente possono fallire prima, scrivendo su stderr invece di produrre JSON. Anche un’interruzione può essere segnalata su stderr. Gli strumenti che consumano l’output devono gestire questi casi.

## Gestione degli errori e risoluzione dei problemi

La pubblicazione è transazionale: l’output esistente viene preservato e l’output temporaneo dei tentativi falliti viene rimosso. Codici di uscita non nulli del backend, errori di assemblaggio o decompilazione nei log, classi omesse a causa di duplicati, marcatori espliciti di codice incompleto, file Java vuoti e risultati senza Java fanno tutti fallire il comando. Un successo segnalato dal backend non dimostra da solo la correttezza dei singoli metodi.

| Sintomo | Azione |
|---------|--------|
| Python/helper mancante | Installare o selezionare Python 3.10+ e mantenere la directory `mobile/` accanto a NeverD |
| Backend non eseguibile o versione non supportata | Verificare `--jadx`, la struttura completa della distribuzione, Java e la versione minima del backend |
| Header DEX non valido / nessun DEX nella radice | Controllare il formato effettivo dell’input; usare un APK contenente codice, un DEX ordinario o smali |
| Nessun file smali | Indicare una directory con file `.smali`, non sorgenti Java o un albero contenente solo asset |
| Classe duplicata o ricostruzione parziale | Rimuovere le definizioni duplicate o analizzare separatamente l’insieme di bytecode pertinente; correggere lo smali malformato invece di accettare un risultato incompleto |
| Timeout / limite di byte o voci | Usare un input più piccolo e pertinente oppure aumentare consapevolmente il limite corrispondente |
| Percorso di archivio o link non sicuro | Ricreare un input regolare e portabile, senza nomi con attraversamento di directory, link, file speciali o percorsi in conflitto |
| Output già esistente | Scegliere un’altra directory di output; non riutilizzare quella di una precedente esecuzione riuscita |

Le esecuzioni riuscite conservano i log del backend. Le directory temporanee dei tentativi falliti, compresi i loro log, vengono eliminate. Un codice di uscita non nullo del backend include nell’errore un estratto limitato della parte finale della diagnostica; i timeout e il superamento dei budget di risorse producono messaggi specifici. Per indagini specifiche sul backend, riproduci il problema su un input isolato con la CLI del backend e una directory diagnostica separata. Non dedurre mai il successo dalla sola comparsa di file Java prima di un errore.

## Verifica e profondità del supporto

```sh
cmake --build build --target check-neverd-mobile
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

I primi due comandi verificano i componenti mobili e i contratti della CLI compilata; i requisiti delle fixture specifici della piattaforma possono comportare esclusioni esplicite. Il runner con il backend reale richiede anche un JDK (`java` e `javac`). Costruisce casi con un solo file smali, riferimenti tra classi e classi annidate, DEX e APK realmente multidex, quindi compila ed esegue il Java ricostruito. I casi coprono diramazioni, cicli, array, gestione delle eccezioni, riferimenti a classi, input malformati e omissioni di classi duplicate. Questo fornisce evidenza per tali fixture, non una promessa di ricostruzione completa per applicazioni arbitrarie.

Il [workflow Mobile Decompilation](../.github/workflows/mobile.yml) esegue i test dei componenti su Linux, macOS e Windows con Python 3.10 e 3.13, oltre a un job Linux con un backend Android reale fissato tramite checksum. Consulta la [panoramica mobile](mobile.md) per il flusso iOS separato e i suoi limiti attuali.
