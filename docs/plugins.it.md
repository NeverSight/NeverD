**Lingue**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← Indice della documentazione](README.it.md)

# Plugin nativi

I plugin nativi di NeverD sono librerie condivise fidate caricate nel processo
host. Usano le dichiarazioni C pure in `neverd/sdk/NeverDPlugin.h` e chiamano
l’API C pubblica in `neverd/sdk/NeverDCAPI.h`. Usa la
[guida ai plugin Python](python-plugins.it.md) quando è più appropriato
sviluppare in Python all’interno dello stesso processo.

## Compatibilità e confine di fiducia

L’attuale descrittore `neverd_plugin_t` non contiene un campo per la versione
dell’ABI né per la dimensione della struttura. Compila il plugin con gli header
preparati dalla revisione esatta di NeverD che lo caricherà e ricompilalo a ogni
aggiornamento di NeverD. Plugin e host devono inoltre usare lo stesso sistema
operativo e la stessa architettura, oltre a toolchain compatibili con l’ABI.

I plugin nativi sono codice arbitrario eseguito all’interno del processo.
NeverD non li esegue in una sandbox, non isola i crash e non limita il loro
accesso alla sessione o al processo host. Carica solo plugin di cui ti fidi.

## Descrittore e callback

Ogni libreria esporta un solo simbolo dati chiamato esattamente
`neverd_plugin`:

```c
#include "neverd/sdk/NeverDPlugin.h"

static int on_init(neverd_session_t session) {
  (void)session;
  return 0;
}

static void on_term(void) {}

static int on_run(neverd_session_t session, int arg) {
  (void)session;
  return arg;
}

static int on_event(const neverd_event_t *event) {
  if (event && event->Type == NEVERD_EVT_BINARY_LOADED) {
    const char *path = event->Data.BinaryLoaded.Path; /* borrowed */
    (void)path;
  }
  return 0;
}

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = "My Plugin",
    .Version = "1.0.0",
    .Author = "Your Name",
    .Description = "A native NeverD extension",
    .Type = NEVERD_PLUGIN_GENERIC,
    .Init = on_init,
    .Term = on_term,
    .Run = on_run,
    .Event = on_event,
};
```

`NEVERD_PLUGIN_EXPORT` si espande in `__declspec(dllexport)` su Windows e nella
visibilità ELF/Mach-O predefinita altrove. Mantieni il sorgente in C oppure
assegna esplicitamente al descrittore il linkage C se un’implementazione C++ è
inevitabile.

`Name` deve essere non vuoto e univoco all’interno di un host. Durante il
caricamento, l’host copia tutte e quattro le stringhe di metadati. `Version`,
`Author` e `Description` possono essere vuoti. I quattro valori di tipo sono
esclusivamente metadati di classificazione:

| Valore | Significato attuale |
|--------|---------------------|
| `NEVERD_PLUGIN_GENERIC` | Etichetta per un’estensione generica |
| `NEVERD_PLUGIN_LOADER` | Etichetta loader; non registra un loader di binari |
| `NEVERD_PLUGIN_PROCESSOR` | Etichetta di analisi/elaborazione; non pianifica lavoro |
| `NEVERD_PLUGIN_UI` | Etichetta UI; NeverD non fornisce un host GUI per plugin nativi |

Tutti i puntatori a callback sono opzionali. Le chiamate sono dirette e
sincrone sul thread del chiamante host.

| Callback | Contratto |
|----------|-----------|
| `Init(session)` | Restituisce `0` in caso di successo. Un risultato diverso da zero registra un errore; `Term` non viene chiamato dopo tale inizializzazione fallita. |
| `Term()` | Viene chiamato durante la terminazione solo dopo un `Init` riuscito. La libreria viene quindi scaricata. |
| `Run(session, arg)` | Esegue il lavoro del plugin e restituisce un risultato intero. La CLI passa `0`; l’API C che integra il plugin può scegliere un altro argomento. Una callback assente restituisce `-1`. |
| `Event(event)` | Gestisce un evento inviato dall’host. Restituisce `0` in caso di successo; un risultato diverso da zero registra un errore. Una callback assente non esegue alcuna operazione. |

L’ordine di integrazione normale è caricamento, inizializzazione, esecuzione o
invio di eventi, quindi terminazione. L’API C non impone questo ordine a `Run` o
`Event`, quindi il ciclo di vita è responsabilità dell’host che integra il
plugin.

## Gli eventi sono inviati dall’host

`neverd_event_t` contiene uno dei sei valori di evento:

| Evento | Payload in `event->Data` |
|--------|--------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | Nessun payload |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | Nessun payload |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

Gli eventi non vengono emessi automaticamente dalle API di sessione né dallo
strumento a riga di comando `neverd`. Un host di integrazione costruisce e invia
l’evento:

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

L’evento e le stringhe del payload sono presi in prestito fino al ritorno della
callback; non liberarli né conservarli. L’handle di sessione appartiene all’host:
un plugin non deve distruggerlo né usarlo dopo la terminazione dell’host. I
risultati allocati dall’API C di NeverD appartengono a NeverD e devono essere
liberati con `neverd_free_string()`. NeverD non garantisce l’accesso concorrente
alle callback o alla sessione: gli host di integrazione devono serializzare le
chiamate del ciclo di vita, di esecuzione e degli eventi, salvo che forniscano
una propria sincronizzazione sicura.

## Compilare l’esempio incluso

Abilita esplicitamente gli esempi; il valore predefinito resta
`NEVERD_BUILD_PLUGINS=OFF`:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

Per un generatore a configurazione singola, gli artefatti rilevanti sono:

| Artefatto | Percorso |
|-----------|----------|
| CLI | `build/bin/neverd` (`neverd.exe` su Windows) |
| Libreria host | `build/bin/libneverd.so`, `build/bin/libneverd.dylib` o `build/bin/neverd.dll` |
| Header preparati | `build/bin/sdk/neverd/sdk/` |
| Esempio | `build/bin/plugins/example_plugin.so`, `.dylib` o `.dll` |

Una build multiconfigurazione colloca gli stessi artefatti di runtime sotto la
configurazione selezionata, per esempio `build/bin/Release/`, con il plugin in
`build/bin/Release/plugins/` e gli header in `build/bin/Release/sdk/`:

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## Compilare un plugin autonomo

NeverD attualmente non installa il proprio SDK né fornisce un
`NeverDConfig.cmake`, quindi non esiste un `find_package(NeverD)` supportato.
Indirizza una build autonoma agli header preparati e a una libreria di link
esplicita proveniente dalla build esatta dell’host:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_neverd_plugin LANGUAGES C)

set(NEVERD_SDK_ROOT "" CACHE PATH
    "Directory containing neverd/sdk/NeverDPlugin.h")
set(NEVERD_LINK_LIBRARY "" CACHE FILEPATH
    "libneverd.so, libneverd.dylib, or the Windows neverd.lib import library")

if(NOT EXISTS "${NEVERD_SDK_ROOT}/neverd/sdk/NeverDPlugin.h")
  message(FATAL_ERROR "Set NEVERD_SDK_ROOT to the staged NeverD SDK")
endif()
if(NOT EXISTS "${NEVERD_LINK_LIBRARY}")
  message(FATAL_ERROR "Set NEVERD_LINK_LIBRARY to the matching libneverd")
endif()

add_library(my_plugin SHARED my_plugin.c)
target_include_directories(my_plugin PRIVATE "${NEVERD_SDK_ROOT}")
target_link_libraries(my_plugin PRIVATE "${NEVERD_LINK_LIBRARY}")
set_target_properties(my_plugin PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  PREFIX "")
```

Configura con `NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk` (o la directory
`sdk` specifica della configurazione). Su Linux/macOS,
`NEVERD_LINK_LIBRARY` è il file `.so`/`.dylib` corrispondente. Su Windows è la
libreria di importazione `neverd.lib` prodotta dal generatore, mentre il file
`neverd.dll` corrispondente deve restare accanto all’eseguibile host. I percorsi
delle librerie di importazione dipendono dal generatore, quindi passa
esplicitamente il file effettivo.

## Rilevamento e guida alla CLI

La CLI esamina le directory in questo ordine; prevalgono i primi percorsi
canonici e i primi nomi dei plugin:

1. `plugins` accanto all’eseguibile `neverd` in esecuzione.
2. `$HOME/.neverd/plugins` (`HOME` viene usata quando non è vuota; su Windows, la directory del profilo nativa è il fallback).
3. Ogni voce non vuota di `NEVERD_PLUGIN_PATH`, nell’ordine.
4. La directory fornita da `--plugin-dir`.

`NEVERD_PLUGIN_PATH` usa `:` tra le voci su Linux/macOS e `;` su Windows. Le
directory canonicamente equivalenti vengono esaminate una sola volta. Per le
librerie native viene considerato solo il suffisso dell’host: `.so` su Linux,
`.dylib` su macOS e `.dll` su Windows. Le build con Python abilitato esaminano
anche i file `.py`.

L’esempio nell’albero di build si trova già nella directory adiacente a quella
dell’eseguibile:

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

Per una build multiconfigurazione, sostituisci `build/bin` con
`build/bin/Release`. Per installare una copia destinata a un eseguibile NeverD
che non abbia già lo stesso plugin accanto, usa il comando per la piattaforma
host, quindi esegui quel binario con le stesse opzioni `--list` e `--run`:

```bash
# Linux
mkdir -p "$HOME/.neverd/plugins"
cp build/bin/plugins/example_plugin.so "$HOME/.neverd/plugins/"

# macOS
mkdir -p "$HOME/.neverd/plugins"
cp build/bin/plugins/example_plugin.dylib "$HOME/.neverd/plugins/"

# Windows PowerShell
New-Item -ItemType Directory -Force "$HOME/.neverd/plugins"
Copy-Item build/bin/plugins/example_plugin.dll "$HOME/.neverd/plugins/"
```

Le directory opzionali mancanti accanto all’eseguibile o nel profilo vengono
ignorate. Un plugin malformato in una directory opzionale produce un avviso e
la scansione continua. Ogni directory indicata da `NEVERD_PLUGIN_PATH` e
`--plugin-dir` è obbligatoria: una directory mancante o un candidato rifiutato
fa terminare la CLI con un codice diverso da zero. Vengono rifiutati i file
canonici duplicati, i nomi di plugin duplicati, l’assenza dell’export
`neverd_plugin` e i tipi di descrittore non validi. Anche una chiamata diretta a
`neverd_plugins_load_file` rifiuta un file con un suffisso non supportato.

Gli host di integrazione devono controllare sia i risultati della gestione dei
plugin sia `neverd_last_error(session)`. `neverd_plugins_load_file` restituisce
`1` o `0`; `neverd_plugins_load_dir` restituisce il numero caricato e può
segnalare candidati rifiutati anche dopo un successo parziale.
`neverd_plugins_run` restituisce il risultato del plugin e usa `-1` quando il
plugin è assente o non può essere eseguito. Libera con `neverd_free_string()`
ogni stringa di errore o JSON restituita dall’API C.
