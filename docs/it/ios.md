**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](../ja/ios.md) | [한국어](../ko/ios.md) | [Français](../fr/ios.md) | [Deutsch](../de/ios.md) | [Español](../es/ios.md) | [Italiano](ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# Recupero del codice nativo e dei sorgenti iOS

[← Indice della documentazione](README.md) · [Panoramica mobile](../mobile.md)

`neverd mobile` accetta IPA, `.app` e Mach-O. Esporta C nativo, metadati runtime e, in via sperimentale, sorgenti Objective-C `.m` e Swift `.swift` per i corpi nativi supportati. Un risultato pubblicato può includere metodi non recuperati: controllare la copertura prima dell’uso. I contenitori mobili appartengono al flusso CLI; l’SDK C nativo carica separatamente il Mach-O selezionato.

La compilazione elimina commenti, formattazione, identificatori e costrutti del linguaggio. Questo flusso ricostruisce una rappresentazione sorgente: non ripristina il testo originale e non certifica un comportamento equivalente per applicazioni arbitrarie. Non avvia l’applicazione analizzata.

## Avvio e dipendenze

```sh
cmake --build build --target neverd
python3 --version
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

Compilare NeverD normalmente e conservare la cartella adiacente `mobile/` distribuendo l’eseguibile. Serve Python 3.10+, selezionato tramite `--python PATH`, poi `NEVERD_PYTHON`, infine `python3`/`python` in PATH. Le dipendenze non vengono scaricate automaticamente. Per compilare indipendentemente i sorgenti Apple generati su macOS servono Apple Clang, SDK e toolchain Swift; questi requisiti sono distinti dall’analisi nativa statica.

Le firme Swift usano, nell’ordine, `--swift-demangle PATH`, `NEVERD_SWIFT_DEMANGLE`, `swift-demangle` in PATH. Su macOS l’ultimo tentativo automatico è `xcrun --find swift-demangle` con tempo limitato. Un percorso esplicito non disponibile causa errore; una ricerca automatica fallita conserva i simboli non classificati con `unavailable`. In assenza di simboli Swift non serve il demangler. `--metadata-only` non invoca né backend nativo né demangler.

```sh
neverd mobile App.ipa -o recovered-swift \
  --python python3 --swift-demangle /path/to/swift-demangle --timeout=600 --json
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
| `--python PATH` | Ambiente/PATH | Interprete Python 3.10+ dell’helper |
| `--swift-demangle PATH` | Ambiente/PATH/toolchain | Demangler delle firme Swift |
| `--timeout N` | `300` | Secondi positivi per processo backend |
| `--max-files N` | `20000` | Budget positivo di voci; anche l’inventario Swift è limitato |
| `--max-bytes N` | `2147483648` | Budget positivo in byte per input, estrazione e output finale |
| `--json` | Disattivato | Stampare il rapporto versionato come JSON |

L’area di lavoro è monitorata e può usare fino a tre volte i budget di voci/byte per input temporanei e risultati intermedi. I log sono limitati a 16 MiB per processo; il JSON delle firme Swift ha inoltre un limite di 32 MiB. Sono controlli di risorse, non isolamento dei processi. Aumentare il timeout non disabilita gli altri limiti. I metodi esclusi da `--max-func` rimangono non recuperati se presenti nei metadati.

## Sorgenti Objective-C e struttura runtime

Il loader nativo collega record del metodo, indirizzo IMP eseguibile e codifica di tipo supportata a posizioni ABI sorgente esplicite. I parametri fissi scalari/puntatori mantengono `self`/`_cmd`, argomenti inutilizzati, banchi interi/flottanti separati e posizioni sullo stack supportate. Reinterpretazione dei bit float/double e conversione numerica sono distinte. I suggerimenti di tipo sono input per la proiezione sorgente, non prove ABI autenticate né autorizzazione a modificare codice eseguibile.

`sources/objc.m` colloca le istruzioni realmente ricostruite nei metodi `@implementation`, mantenendo helper C e chiamate tipizzate necessarie. Le destinazioni richiedono un collegamento sorgente supportato; destinazioni ignote e gruppi di dipendenze incompleti restano non recuperati. Definizioni mancanti, indirizzi non eseguibili, codifiche discordanti, ABI non gestite, decodifica incompleta e IR rifiutato non diventano metodi recuperati solo perché esiste una dichiarazione.

I metadati mantengono identità della superclasse, inizio/dimensione dell’istanza e ivar scalari/puntatori con offset, larghezze e allineamenti verificati. Le dichiarazioni inseriscono padding dove necessario. I metodi che richiedono un layout indisponibile restano non recuperati. Le categorie conservano identità classe/categoria/indirizzo e implementazioni separate; record identici ripetuti nei due inventari contano una volta. Le categorie esterne supportate usano dichiarazioni Foundation esistenti. Gli header esterni sconosciuti sono dipendenze mancanti; non si inventa una classe sostitutiva.

È una ricostruzione limitata del runtime: proprietà e protocolli completi, annotazioni originali di ownership, aggregati arbitrari, code variadiche, corpi dipendenti dalle eccezioni e layout Block/catture non modellati non sono garantiti. La codifica descrive gli argomenti fissi e non prova l’assenza di puntini di sospensione nella dichiarazione originale. I puntatori concatenati sono usati solo negli slot risolti dal loader; gli altri formati mantengono diagnostica.

## Sorgenti Swift e memoria

L’output strutturato del demangler distingue firme invocabili da metadati non invocabili. Prima della proiezione nativa, le firme supportate sono associate a simboli, entry e ABI macchina esplicita del binario selezionato. I receiver Swift seguono la propria ABI, senza sostituirli con gli argomenti nascosti Objective-C. Anche un file di firme fornito dall’utente resta un suggerimento da validare.

L’emettitore sperimentale costruisce funzioni libere, metodi di classe, inizializzatori designati e metodi di struct a layout fisso supportati, incluse determinate forme di receiver mutating. Dichiarazioni di classe/struct e campi memorizzati richiedono metadati di layout recuperati. Le chiamate native sono emesse solo quando dichiarazioni e corpi necessari formano un gruppo completo di dipendenze supportate. Le unità sorgente riuniscono dichiarazioni e metodi, senza un ponte che richiami il binario originale.

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

I file del linguaggio sorgente esistono soltanto se può essere emesso codice. `objc.json` contiene classi, categorie, ivar e codifiche grezze; `objc.h` le dichiarazioni supportate. `swift.json` contiene tipi nominali e simboli mangled. JSON di firme/metodi mantengono classificazione, omissioni, motivi e conteggi. I log comprendono diagnostica nativa e, quando usati, ricerca della toolchain, demangling ed export Swift nativo. I percorsi in `report.json` sono relativi alla sua directory. Il binario selezionato è un artefatto d’analisi, non viene collegato come ponte di recupero al codice generato.

Copie temporanee del pacchetto e JSON intermedi vengono rimossi. Una normale esecuzione senza corpi nativi fallisce anche con metadati disponibili. La modalità metadati produce soltanto slice selezionata, `objc.h`, `objc.json`, `swift.json`, `report.json`; mancano sorgenti e file di firme/copertura, mentre `native_function_count`, `objc_method_recovery`, `swift_method_recovery` sono `null`. Il reader Objective-C Python non risolve puntatori concatenati o di oggetti rilocabili. Il recupero completo usa i metadati Objective-C risolti dal loader nativo. Il reader Swift grezzo può ancora segnalare riferimenti non supportati come parziali.

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
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

Lo `status: "success"` esterno indica pubblicazione dell’output validato. `recovered`, `partial`, `unrecovered`, `no-methods` descrivono l’inventario scoperto, non equivalenza semantica o completezza del programma originale. Ogni metodo non recuperato ha un motivo. Per Objective-C, `recovered` richiede anche metadati runtime completi. Un inventario vuoto non dimostra che non esistessero metodi.

Il `coverage_status` Swift conta soltanto gli elementi invocabili classificati. Lo `status` Swift complessivo considera anche simboli ignoti e può essere `unavailable`, `unclassified`, `unsupported-architecture`, `no-symbols`. I metadati non invocabili compaiono in `non_method_symbols` con `not-callable`, quelli ignoti con `unclassified`. `types`, `type_metadata_count`, `source_type_count` contano separatamente metadati e unità di tipo emesse, senza gonfiare il numero di metodi.

Il batch Swift descrive `source_units` con `{kind, module, name, source, method_entries, method_identities}`; kind vale `function` o `type`, ogni identità è `{entry, mangled_symbol}`. Simboli diversi possono condividere un’entry mantenendo proiezioni ABI distinte. Ogni identità recuperata deve apparire esattamente una volta, nessuna non recuperata può comparire. `method_entries` deve essere l’esatta proiezione ordinata delle entry di `method_identities`, inclusi indirizzi ripetuti. Identità duplicate identiche non possono essere unite silenziosamente. `source` concatena in ordine il codice delle unità più un carattere di nuova riga ciascuna. Mobile salva il codice aggregato in `sources/swift.swift` e le descrizioni nel JSON di copertura. Il `source` individuale serve all’ispezione; concatenare queste righe non ricostruisce correttamente le classi.

## Export nativi diretti e SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

L’export Swift consuma l’inventario strutturato generato da una normale esecuzione mobile con demangler. Il batch Objective-C include `native_source`, `native_function_count`, `objc_metadata` e, per metodo, sorgente C, nome, tipo di ritorno e parametri. Prima di `.m`, Mobile verifica ulteriormente dichiarazioni, corpi e layout: la copertura finale può essere inferiore a quella del C batch. Un export nativo riuscito può non contenere alcun metodo recuperato.

Per una sessione con Mach-O già caricato, `neverd_objc_methods_json(session, max_functions)` e `neverd_swift_methods_json(session, signatures_json, max_functions)` restituiscono i rapporti. Zero seleziona tutte le funzioni scoperte. Liberare le stringhe riuscite con `neverd_free_string`; `NULL` indica un errore spiegato nello stato della sessione. Le API non caricano contenitori IPA o `.app`.

## Verifica e risoluzione dei problemi

```sh
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

Su macOS il runner Objective-C compila i propri esempi originali, recupera `.m` e collega solo il codice generato con un programma chiamante indipendente. Verifica limiti interi, rami, cicli, puntatori, parametri nascosti, identità dei bit float/double, parametri misti e stack. Il runner Swift ricompila `.swift` con il proprio harness senza dylib, modulo, ponte o dichiarazioni sostitutive manuali originali. Controlla scalari/chiamate native, inizializzazione/memoria di classe, metodi di struct per valore/mutating, flottanti, stack, puntatori e cicli. Sono controlli rigorosi che possono esporre limiti: l’esistenza dello script non prova che ogni caso passi con ogni build.

Entrambi gli script supportano `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N`, `--work-dir NEW_DIRECTORY`. `--setup-only` verifica gli originali, non il recupero. Le architetture non eseguibili sull’host sono saltate esplicitamente ove consentito; saltare non equivale a passare. Conservare gli artefatti per distinguere copertura mancante, errori di compilazione e differenze di comportamento. Controllare i risultati attuali prima di dichiarare supporto verificato.

La pubblicazione è transazionale: scegliere una directory nuova, controllare prima lo stato d’uscita e salvare altrove il JSON rediretto. Gli errori eliminano l’output temporaneo e preservano quello esistente. L’uscita non zero del backend include una coda di log limitata; timeout e budget hanno messaggi distinti. Con `--json`, gli errori gestiti dell’helper producono `status: "error"`; parsing, helper/interprete mancante, Python sotto 3.10 e interruzioni possono fallire prima su stderr.

Per slice cifrate fornire input leggibili; per architetture assenti controllare le slice disponibili; per Swift selezionare il demangler effettivo. Leggere motivi esatti e diagnostica dei metodi omessi. Aumentare `--max-func` aiuta soltanto le funzioni escluse dal limite. Layout, firme, header esterni, eccezioni o ABI mancanti richiedono implementazione o ulteriori metadati validi, non una dichiarazione di recupero completo. Conservare gli avvisi di licenza applicabili distribuendo strumenti o pacchetti generati.
