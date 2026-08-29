**Idiomas**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← Índice de documentación](README.es.md)

# Plugins nativos

Los plugins nativos de NeverD son bibliotecas compartidas de confianza que se
cargan en el proceso anfitrión. Usan las declaraciones en C puro de
`neverd/sdk/NeverDPlugin.h` y llaman a la API C pública de
`neverd/sdk/NeverDCAPI.h`. Use la
[guía de plugins de Python](python-plugins.es.md) cuando resulte más apropiado
desarrollar en Python dentro del mismo proceso.

## Compatibilidad y límite de confianza

El descriptor actual `neverd_plugin_t` no contiene un campo de versión de ABI
ni de tamaño de estructura. Compile el plugin con las cabeceras preparadas por
la revisión exacta de NeverD que vaya a cargarlo y vuelva a compilarlo cada vez
que se actualice NeverD. El plugin y el anfitrión también deben usar el mismo
sistema operativo y arquitectura, además de toolchains compatibles con la ABI.

Los plugins nativos son código arbitrario dentro del proceso. NeverD no los
ejecuta en un sandbox, no aísla sus fallos ni restringe su acceso a la sesión o
al proceso anfitrión. Cargue únicamente plugins en los que confíe.

## Descriptor y callbacks

Cada biblioteca exporta un único símbolo de datos llamado exactamente
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

`NEVERD_PLUGIN_EXPORT` se expande a `__declspec(dllexport)` en Windows y a la
visibilidad predeterminada de ELF/Mach-O en los demás sistemas. Mantenga el
código fuente en C o dé al descriptor enlace C de forma explícita si no se
puede evitar una implementación en C++.

`Name` no puede estar vacío y debe ser único en cada anfitrión. Al cargar, el
anfitrión toma una copia de las cuatro cadenas de metadatos. `Version`, `Author`
y `Description` pueden estar vacíos. Los cuatro valores de tipo son únicamente
metadatos de clasificación:

| Valor | Significado actual |
|-------|--------------------|
| `NEVERD_PLUGIN_GENERIC` | Etiqueta de extensión general |
| `NEVERD_PLUGIN_LOADER` | Etiqueta de cargador; no registra un cargador de binarios |
| `NEVERD_PLUGIN_PROCESSOR` | Etiqueta de análisis/procesamiento; no programa trabajo |
| `NEVERD_PLUGIN_UI` | Etiqueta de UI; NeverD no proporciona un anfitrión GUI para plugins nativos |

Todos los punteros de callback son opcionales. Las llamadas son directas y
síncronas en el hilo del llamador del anfitrión.

| Callback | Contrato |
|----------|----------|
| `Init(session)` | Devuelve `0` si tiene éxito. Un resultado distinto de cero registra un error; no se llamará a `Term` tras esa inicialización fallida. |
| `Term()` | Solo se llama durante la finalización después de un `Init` correcto. A continuación se descarga la biblioteca. |
| `Run(session, arg)` | Realiza el trabajo del plugin y devuelve un resultado entero. La CLI pasa `0`; la API C que lo integra puede elegir otro argumento. Si falta el callback, devuelve `-1`. |
| `Event(event)` | Gestiona un evento despachado por el anfitrión. Devuelve `0` si tiene éxito; un resultado distinto de cero registra un error. Si falta el callback, no se hace nada. |

El orden normal de integración es cargar, inicializar, ejecutar o despachar
eventos y, por último, finalizar. La API C no impone ese orden para `Run` ni
`Event`, por lo que el anfitrión que integra el plugin es responsable de su
ciclo de vida.

## Los eventos los despacha el anfitrión

`neverd_event_t` contiene uno de seis valores de evento:

| Evento | Carga en `event->Data` |
|--------|-------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | Sin carga |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | Sin carga |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

Ni las API de sesión ni la herramienta de línea de comandos `neverd` emiten
eventos automáticamente. Un anfitrión integrado construye y despacha el evento:

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

El evento y las cadenas de su carga se prestan hasta que regresa el callback;
no los libere ni los conserve. El identificador de sesión pertenece al
anfitrión: un plugin no debe destruirlo ni usarlo después de que termine el
anfitrión. Los resultados asignados por la API C de NeverD pertenecen a NeverD
y deben liberarse con `neverd_free_string()`. NeverD no garantiza el acceso
concurrente a callbacks o sesiones: los anfitriones integrados deben serializar
las llamadas del ciclo de vida, de ejecución y de eventos, salvo que
proporcionen su propia sincronización segura.

## Compilar el ejemplo incluido

Active los ejemplos explícitamente; el valor predeterminado sigue siendo
`NEVERD_BUILD_PLUGINS=OFF`:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

Con un generador de configuración única, los artefactos relevantes son:

| Artefacto | Ruta |
|-----------|------|
| CLI | `build/bin/neverd` (`neverd.exe` en Windows) |
| Biblioteca anfitriona | `build/bin/libneverd.so`, `build/bin/libneverd.dylib` o `build/bin/neverd.dll` |
| Cabeceras preparadas | `build/bin/sdk/neverd/sdk/` |
| Ejemplo | `build/bin/plugins/example_plugin.so`, `.dylib` o `.dll` |

Una compilación multiconfiguración coloca los mismos artefactos de ejecución
bajo la configuración seleccionada, por ejemplo `build/bin/Release/`, con el
plugin en `build/bin/Release/plugins/` y las cabeceras en
`build/bin/Release/sdk/`:

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## Compilar un plugin independiente

NeverD no instala actualmente su SDK ni proporciona `NeverDConfig.cmake`, por
lo que no existe un `find_package(NeverD)` admitido. Haga que una compilación
independiente use las cabeceras preparadas y una biblioteca de enlace explícita
procedente de la compilación exacta del anfitrión:

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

Configure con `NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk` (o el directorio
`sdk` específico de la configuración). En Linux/macOS,
`NEVERD_LINK_LIBRARY` es el `.so`/`.dylib` correspondiente. En Windows es la
biblioteca de importación `neverd.lib` generada por el generador, mientras que
el `neverd.dll` correspondiente debe permanecer junto al ejecutable anfitrión.
Las ubicaciones de las bibliotecas de importación dependen del generador, así
que pase el archivo real de forma explícita.

## Descubrimiento y recorrido por la CLI

La CLI examina los directorios en este orden; prevalecen las primeras rutas
canónicas y los primeros nombres de plugin:

1. `plugins` junto al ejecutable `neverd` en ejecución.
2. `$HOME/.neverd/plugins` (se usa `HOME` cuando no está vacío; en Windows, el directorio de perfil nativo es la alternativa).
3. Cada entrada no vacía de `NEVERD_PLUGIN_PATH`, en orden.
4. El directorio proporcionado mediante `--plugin-dir`.

`NEVERD_PLUGIN_PATH` usa `:` para separar entradas en Linux/macOS y `;` en
Windows. Los directorios canónicamente equivalentes se examinan una sola vez.
Para las bibliotecas nativas solo se tiene en cuenta el sufijo del anfitrión:
`.so` en Linux, `.dylib` en macOS y `.dll` en Windows. Las compilaciones con
Python habilitado también examinan archivos `.py`.

El ejemplo del árbol de compilación ya se encuentra en el directorio hermano
del ejecutable:

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

Para una compilación multiconfiguración, sustituya `build/bin` por
`build/bin/Release`. Para instalar una copia para un ejecutable de NeverD que
no tenga ya el mismo plugin a su lado, use el comando de la plataforma
anfitriona y ejecute después ese binario con las mismas opciones `--list` y
`--run`:

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

Los directorios opcionales ausentes junto al ejecutable o en el perfil se
ignoran. Un plugin defectuoso en un directorio opcional genera una advertencia
y el examen continúa. Todos los directorios indicados por `NEVERD_PLUGIN_PATH`
y `--plugin-dir` son obligatorios: un directorio ausente o un candidato
rechazado hace que la CLI termine con un código distinto de cero. Se rechazan
los archivos canónicos duplicados, los nombres de plugin duplicados, la falta
de una exportación `neverd_plugin` y los tipos de descriptor no válidos. Una
llamada directa a `neverd_plugins_load_file` también rechaza archivos con un
sufijo no admitido.

Los anfitriones integrados deben comprobar tanto los resultados de gestión de
plugins como `neverd_last_error(session)`. `neverd_plugins_load_file` devuelve
`1` o `0`; `neverd_plugins_load_dir` devuelve el número cargado y puede informar
de candidatos rechazados incluso después de un éxito parcial.
`neverd_plugins_run` devuelve el resultado del plugin y usa `-1` cuando el
plugin no existe o no se puede ejecutar. Libere con `neverd_free_string()`
todas las cadenas de error o JSON devueltas por la API C.
