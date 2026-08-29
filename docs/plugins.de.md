**Sprachen**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← Dokumentationsindex](README.de.md)

# Native Plugins

Native NeverD-Plugins sind vertrauenswürdige Shared Libraries, die in den
Hostprozess geladen werden. Sie verwenden die reinen C-Deklarationen in
`neverd/sdk/NeverDPlugin.h` und rufen die öffentliche C-API in
`neverd/sdk/NeverDCAPI.h` auf. Verwenden Sie den
[Leitfaden für Python-Plugins](python-plugins.de.md), wenn prozesslokale
Entwicklung in Python besser geeignet ist.

## Kompatibilität und Vertrauensgrenze

Der aktuelle Deskriptor `neverd_plugin_t` besitzt weder ein ABI-Versions- noch
ein Strukturgrößenfeld. Bauen Sie ein Plugin mit den bereitgestellten Headern
exakt der NeverD-Revision, die es laden wird, und bauen Sie das Plugin nach
jeder NeverD-Aktualisierung neu. Plugin und Host müssen außerdem dasselbe
Betriebssystem und dieselbe Architektur sowie ABI-kompatible Toolchains
verwenden.

Native Plugins sind beliebiger Code innerhalb des Prozesses. NeverD führt sie
nicht in einer Sandbox aus, isoliert keine Abstürze und beschränkt ihren Zugriff
auf die Sitzung oder den Hostprozess nicht. Laden Sie nur Plugins, denen Sie
vertrauen.

## Deskriptor und Callbacks

Jede Bibliothek exportiert genau ein Datensymbol namens `neverd_plugin`:

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

`NEVERD_PLUGIN_EXPORT` wird unter Windows zu `__declspec(dllexport)` und auf
anderen Systemen zur Standardsichtbarkeit für ELF/Mach-O erweitert. Belassen Sie
den Quelltext in C oder geben Sie dem Deskriptor ausdrücklich C-Linkage, falls
eine C++-Implementierung unvermeidbar ist.

`Name` darf nicht leer sein und muss innerhalb eines Hosts eindeutig sein. Der
Host kopiert beim Laden alle vier Metadaten-Strings. `Version`, `Author` und
`Description` dürfen leer sein. Die vier Typwerte sind ausschließlich
Klassifikationsmetadaten:

| Wert | Heutige Bedeutung |
|------|-------------------|
| `NEVERD_PLUGIN_GENERIC` | Bezeichnung für eine allgemeine Erweiterung |
| `NEVERD_PLUGIN_LOADER` | Loader-Bezeichnung; registriert keinen Binärloader |
| `NEVERD_PLUGIN_PROCESSOR` | Analyse-/Verarbeitungsbezeichnung; plant keine Arbeit ein |
| `NEVERD_PLUGIN_UI` | UI-Bezeichnung; NeverD stellt keinen GUI-Host für native Plugins bereit |

Alle Callback-Zeiger sind optional. Aufrufe erfolgen direkt und synchron auf
dem Thread des Host-Aufrufers.

| Callback | Vertrag |
|----------|---------|
| `Init(session)` | Bei Erfolg `0` zurückgeben. Ein Ergebnis ungleich null zeichnet einen Fehler auf; `Term` wird nach dieser fehlgeschlagenen Initialisierung nicht aufgerufen. |
| `Term()` | Wird beim Beenden nur nach einem erfolgreichen `Init` aufgerufen. Anschließend wird die Bibliothek entladen. |
| `Run(session, arg)` | Plugin-Arbeit ausführen und ein ganzzahliges Ergebnis zurückgeben. Die CLI übergibt `0`; die einbettende C-API kann ein anderes Argument wählen. Ein fehlender Callback gibt `-1` zurück. |
| `Event(event)` | Ein vom Host ausgelöstes Ereignis verarbeiten. Bei Erfolg `0` zurückgeben; ein Ergebnis ungleich null zeichnet einen Fehler auf. Ein fehlender Callback bewirkt nichts. |

Die normale Einbettungsreihenfolge lautet: laden, initialisieren, ausführen oder
Ereignisse auslösen und schließlich beenden. Die C-API erzwingt diese Reihenfolge
für `Run` oder `Event` nicht; der einbettende Host ist daher für den Lebenszyklus
verantwortlich.

## Ereignisse werden vom Host ausgelöst

`neverd_event_t` enthält einen von sechs Ereigniswerten:

| Ereignis | Nutzdaten in `event->Data` |
|----------|----------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | Keine Nutzdaten |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | Keine Nutzdaten |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

Ereignisse werden weder von Sitzungs-APIs noch vom Kommandozeilenwerkzeug
`neverd` automatisch ausgelöst. Ein einbettender Host erstellt das Ereignis und
löst es aus:

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

Das Ereignis und die Nutzdaten-Strings sind bis zur Rückkehr des Callbacks nur
ausgeliehen; geben Sie sie nicht frei und bewahren Sie sie nicht auf. Das
Session-Handle gehört dem Host: Ein Plugin darf es weder zerstören noch nach
dem Beenden des Hosts verwenden. Von der NeverD-C-API reservierte Ergebnisse
gehören NeverD und müssen mit `neverd_free_string()` freigegeben werden. NeverD
garantiert keinen parallelen Zugriff auf Callbacks oder Sitzungen: Einbettende
Hosts sollten Lebenszyklus-, Run- und Ereignisaufrufe serialisieren, sofern sie
keine eigene sichere Synchronisierung bereitstellen.

## Mitgeliefertes Beispiel bauen

Aktivieren Sie die Beispiele ausdrücklich; der Standard bleibt
`NEVERD_BUILD_PLUGINS=OFF`:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

Bei einem Single-Configuration-Generator sind folgende Artefakte relevant:

| Artefakt | Pfad |
|----------|------|
| CLI | `build/bin/neverd` (`neverd.exe` unter Windows) |
| Hostbibliothek | `build/bin/libneverd.so`, `build/bin/libneverd.dylib` oder `build/bin/neverd.dll` |
| Bereitgestellte Header | `build/bin/sdk/neverd/sdk/` |
| Beispiel | `build/bin/plugins/example_plugin.so`, `.dylib` oder `.dll` |

Ein Multi-Configuration-Build legt dieselben Laufzeitartefakte unter der
gewählten Konfiguration ab, zum Beispiel `build/bin/Release/`, mit dem Plugin in
`build/bin/Release/plugins/` und den Headern in `build/bin/Release/sdk/`:

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## Eigenständiges Plugin bauen

NeverD installiert sein SDK derzeit nicht und stellt keine
`NeverDConfig.cmake` bereit; daher gibt es kein unterstütztes
`find_package(NeverD)`. Verweisen Sie einen eigenständigen Build auf die
bereitgestellten Header und eine explizite Linkbibliothek aus exakt demselben
Host-Build:

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

Konfigurieren Sie mit `NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk` (oder dem
konfigurationsspezifischen `sdk`-Verzeichnis). Unter Linux/macOS ist
`NEVERD_LINK_LIBRARY` die passende `.so`/`.dylib`. Unter Windows ist es die vom
Generator erzeugte Importbibliothek `neverd.lib`, während die passende
`neverd.dll` beim ausführbaren Hostprogramm verbleiben muss. Die Speicherorte
von Importbibliotheken hängen vom Generator ab; übergeben Sie daher ausdrücklich
die tatsächlich erzeugte Datei.

## Erkennung und CLI-Rundgang

Die CLI durchsucht Verzeichnisse in dieser Reihenfolge; frühere kanonische Pfade
und Plugin-Namen gewinnen:

1. `plugins` neben dem laufenden ausführbaren Programm `neverd`.
2. `$HOME/.neverd/plugins` (`HOME` wird verwendet, wenn es nicht leer ist; unter Windows dient andernfalls das native Profilverzeichnis als Fallback).
3. Jeder nicht leere Eintrag in `NEVERD_PLUGIN_PATH`, in der angegebenen Reihenfolge.
4. Das durch `--plugin-dir` angegebene Verzeichnis.

`NEVERD_PLUGIN_PATH` trennt Einträge unter Linux/macOS mit `:` und unter Windows
mit `;`. Kanonisch äquivalente Verzeichnisse werden einmal durchsucht. Bei
nativen Bibliotheken wird nur die Host-Endung berücksichtigt: `.so` unter Linux,
`.dylib` unter macOS und `.dll` unter Windows. Builds mit aktivierter
Python-Unterstützung durchsuchen auch `.py`-Dateien.

Das Beispiel im Build-Baum befindet sich bereits im Nachbarverzeichnis des
ausführbaren Programms:

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

Ersetzen Sie bei einem Multi-Configuration-Build `build/bin` durch
`build/bin/Release`. Um eine Kopie für ein ausführbares NeverD-Programm zu
installieren, neben dem sich nicht bereits dasselbe Plugin befindet, verwenden
Sie den Befehl für die Hostplattform. Führen Sie dieses Programm anschließend
mit denselben Optionen `--list` und `--run` aus:

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

Fehlende optionale Nachbar- oder Home-Verzeichnisse werden ignoriert. Ein
fehlerhaftes Plugin in einem optionalen Verzeichnis erzeugt eine Warnung und
die Suche wird fortgesetzt. Jedes durch `NEVERD_PLUGIN_PATH` oder
`--plugin-dir` benannte Verzeichnis ist erforderlich: Ein fehlendes Verzeichnis
oder ein abgelehnter Kandidat lässt die CLI mit einem Wert ungleich null enden.
Doppelte kanonische Dateien, doppelte Plugin-Namen, ein fehlender Export
`neverd_plugin` und ungültige Deskriptortypen werden abgelehnt. Ein direkter
Aufruf von `neverd_plugins_load_file` lehnt außerdem Dateien mit nicht
unterstützter Endung ab.

Einbettende Hosts sollten sowohl die Ergebnisse der Plugin-Verwaltung als auch
`neverd_last_error(session)` prüfen. `neverd_plugins_load_file` gibt `1` oder
`0` zurück; `neverd_plugins_load_dir` gibt die Anzahl geladener Plugins zurück
und kann selbst nach einem Teilerfolg abgelehnte Kandidaten melden.
`neverd_plugins_run` gibt das Plugin-Ergebnis zurück und verwendet `-1`, wenn
das Plugin fehlt oder nicht ausgeführt werden kann. Geben Sie jeden von der
C-API zurückgegebenen Fehler- oder JSON-String mit `neverd_free_string()` frei.
