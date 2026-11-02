**Idiomas**: [English](../../CONTRIBUTING.md) | [简体中文](../zh-CN/CONTRIBUTING.md) | [繁體中文](../zh-TW/CONTRIBUTING.md) | [日本語](../ja/CONTRIBUTING.md) | [한국어](../ko/CONTRIBUTING.md) | [Français](../fr/CONTRIBUTING.md) | [Deutsch](../de/CONTRIBUTING.md) | [Español](CONTRIBUTING.md) | [Italiano](../it/CONTRIBUTING.md) | [Русский](../ru/CONTRIBUTING.md) | [العربية](../ar/CONTRIBUTING.md)

<!-- i18n-source: af6c34901bacab00e5d1ed68b031f0a48a3ff64ac85ee208695647ec935b9790 -->

# Contribuir a NeverD

NeverD es un proyecto de análisis binario que prioriza la semántica. Una
contribución útil tiene un alcance concreto, hace que el comportamiento no
soportado falle de forma visible e incluye la prueba mínima que demuestra el
contrato modificado.

Conserve los avisos de derechos de autor, licencia y procedencia al incorporar
código existente, incluido el copiado o adaptado con herramientas de IA/LLM.
Siga la [guía de atribución](ATTRIBUTION.md), identifique la fuente original
y la revisión del material reutilizado, y compruebe su licencia antes de enviarlo.
Mantenga los avisos de terceros bajo sus licencias originales.

Antes de editar, lea la [guía de arquitectura](architecture.md). Use la
[guía de pruebas](testing.md) para elegir suites y la
[hoja de ruta](roadmap.md) para consultar el trabajo de producto
planificado.

<!-- i18n-section: prerequisites -->
## Requisitos previos

- Git con soporte para submódulos recursivos
- CMake 3.20 o posterior
- Ninja
- Un compilador C++20
- CPython 3.10+ y archivos de desarrollo para integración si se activan los plugins Python
- Clang y LLD (`ld.lld` y `lld-link`) para el conjunto completo de fixtures
  entre objetivos

Los submódulos recursivos proporcionan los forks de LLVM y Capstone de NeverD,
Unicorn y los datos de firmas. No los sustituya por revisiones arbitrarias del
sistema al validar un cambio.

<!-- i18n-section: clone -->
## Clonar e inicializar

El desarrollo se integra en `dev`, que también es la rama predeterminada del
repositorio. Clónela con todos los submódulos:

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

En un clon existente, sincronice los submódulos antes de la primera compilación
y después de cualquier commit que cambie sus revisiones registradas:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

<!-- i18n-section: build -->
## Elegir un perfil de compilación

| Perfil | Uso | Comportamiento importante |
|--------|-----|--------------------------|
| Release | Desarrollo normal, pruebas completas, benchmarks de decodificación/lift | Optimizado; rendimiento representativo |
| RelWithDebInfo | Perfilado o depuración de rutas críticas optimizadas | Optimizado con símbolos de depuración |
| Debug | Aserciones, ejecución paso a paso, corrección local | Sin optimizar; benchmarks de decodificación deliberadamente mucho más lentos |

Use Release salvo que la tarea necesite específicamente el comportamiento de
Debug:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
python scripts/build.py build-release --parallel
```

De forma predeterminada, la compilación construye `third_party/llvm-project`
como dependencia integrada. La primera compilación suele tardar entre 30 y 60
minutos; las siguientes son incrementales. `CMakePresets.json` también define
los presets de configuración/compilación `release`, `relwithdebinfo` y `debug`,
pero arriba se usan directorios explícitos para dejar visible la activación de
las pruebas.

Para depurar a nivel de código fuente, use un directorio separado en vez de
reconfigurar el árbol Release:

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
python scripts/build.py build-debug --parallel
```

Nunca publique el rendimiento de decodificación o lift obtenido en una
compilación Debug. Use Release para benchmarks, o RelWithDebInfo cuando el
perfilado necesite símbolos.

<!-- i18n-section: allocator -->
### Asignador del proceso

Linux, macOS y Windows MSVC/clang-cl activan `NEVERD_ENABLE_MIMALLOC` por defecto.
Las herramientas, workers de análisis y pruebas creados con el auxiliar CMake común usan
la rama predeterminada del fork mimalloc de NeverSight, siguiendo el modelo anfitrión de NeverC.
Unix lo integra una vez por ejecutable. Windows usa una DLL mimalloc y la correspondiente
`mimalloc-redirect*.dll` de su arquitectura para redirigir el CRT dinámico, incluidos
libneverd y plugins nativos. Ambas se colocan junto a los ejecutables y se instalan en `bin`;
distribuya las dos. Windows exige `/MD` o `/MDd`; con CRT estático desactive mimalloc.
La biblioteca SDK no impone asignador al anfitrión. En Windows ARM64 use compilación ARM64
nativa: una DLL x64 no puede redirigir el CRT ARM64EC de la emulación x64. MinGW conserva
el asignador del sistema. Consulte los [requisitos Windows originales](https://github.com/NeverSight/mimalloc/blob/main3/bin/readme.md).

Use `-DNEVERD_ENABLE_MIMALLOC=OFF` como referencia del sistema o para asignadores externos.
`NEVERD_WORKER_MIMALLOC=OFF` solo desactiva el worker. CMake suprime mimalloc con sanitizers
address, thread, memory y leak, incluidos indicadores combinados o específicos de configuración,
de compilación/enlace. UBSan solo es compatible. Si se añaden sanitizers mediante indicadores
exclusivos de una diana, configure también OFF. En Windows, `MIMALLOC_DISABLE_REDIRECT=1`
permite comparar sin recompilar y `MIMALLOC_VERBOSE=1` informa de la activación.

La entrada admitida, `python scripts/build.py BUILD_DIR`, actualiza los cinco forks NeverSight
desde sus ramas remotas predeterminadas antes de cada compilación. Son submódulos de
`third_party`, como Capstone y Unicorn. El script resuelve las ramas reales, actualiza los
cinco submódulos y exporta fuentes confirmadas a `BUILD_DIR/third_party/<library>/<commit>/src`.
Los cambios locales sin confirmar o divergentes detienen la compilación sin sobrescribirse;
los fallos de red también la detienen. El directorio se bloquea durante actualización,
reconfiguración y compilación; la instantánea dura hasta la siguiente invocación. Las
compilaciones incrementales también adoptan commits nuevos.

`neverd-dependency-inputs.json` registra entradas, commit y SHA-256 de exportación y acompaña
los productos instalados; no certifica un binario. `neverd-build-receipt.json` registra
éxito/fallo solo para objetivos y configuración solicitados. CI conserva ambos. La instalación
rechaza fallos, entradas modificadas y configuraciones no compiladas; CI de producto compila
todos los objetivos instalados antes de probar la instalación. Compare commits antes de unir
resultados multiplataforma: la rama principal puede cambiar entre ejecuciones. `cmake --build`
comprueba vigencia y rechaza instantáneas obsoletas; use la entrada Python para renovarlas.
Su regeneración automática conserva la misma instantánea. `FETCHCONTENT_SOURCE_DIR_NEVERD_*`
permite fixtures locales aisladas, pero la entrada principal lo rechaza. Generadores
multiconfiguración exigen `--config`. Compile mediante la entrada y ejecute pruebas y
`cmake --install` aparte; el objetivo directo `install` se rechaza antes de compilar.

Consulte la [validación del asignador](../testing.md#allocator-validation) para pruebas básicas y comparaciones.

<!-- i18n-section: acceleration -->
### Bibliotecas de aceleración

Las compilaciones normales activan escaneo Highway, validación UTF-8 simdutf, análisis de
transporte simdjson y planificación oneTBB, conservando el asignador del proceso. JSON mantiene
DOM y serialización nlohmann: tramas válidas solo con enteros se copian de la cinta SIMD;
decimales y cero negativo conservan la conversión original. Entradas fuera del dominio simdjson
usan la admisión original; texto externo inválido conserva escapes fieles a los bytes.
Estas bibliotecas y mimalloc proceden de los forks NeverSight de `third_party/forks.json`;
las correcciones están en sus ramas predeterminadas. Se copian licencias actuales y commits
exactos; estas cinco dependencias no se fijan a versiones antiguas.

`NEVERD_ENABLE_HIGHWAY`, `NEVERD_ENABLE_SIMDUTF`, `NEVERD_ENABLE_SIMDJSON` y
`NEVERD_ENABLE_TBB` valen `ON`; cada una admite `OFF` para comparar. oneTBB retiene workers
entre lotes y comprueba sus pilas de 8 MiB; nunca compila/carga `tbbmalloc` ni su proxy.
Los llamantes con pila grande reutilizan espacio disponible; los pequeños reciben un
coordinador nativo. Anfitriones que hacen fork tras el primer uso o descargan el SDK
dinámicamente deben usar hilos nativos (`NEVERD_ENABLE_TBB=OFF`): el planificador retenido
no admite esos ciclos de vida. oneTBB se enlaza estáticamente con el núcleo compartido,
y sus símbolos son privados de ese núcleo. Las llamadas a componentes desde la CLI y
las solicitudes al worker de la GUI usan el mismo planificador; no se distribuye ninguna
DLL ni biblioteca compartida de TBB. Los targets que enlazan `neverd_shared` seleccionan
automáticamente el planificador compartido. Los clientes que acceden al SDK a través
de otro target y también enlazan archivos de componentes deben usar
`neverd_use_shared_task_scheduler` en su target final. Las herramientas de componentes
independientes conservan su propio entorno de ejecución.
Esta integración restringida evita varios planificadores NeverD en un mismo proceso;
el proyecto original sigue considerando las [compilaciones estáticas de oneTBB](https://uxlfoundation.github.io/oneTBB/main/intro/static_linking.html)
como no soportadas y desaconseja su uso general.

Vea la [validación de aceleración](../testing.md#acceleration-libraries) para pruebas nativas, comparaciones y límites.

<!-- i18n-section: prebuilt-llvm -->
### LLVM precompilado en macOS

Quienes contribuyan desde Apple Silicon pueden evitar compilar localmente el
fork de LLVM:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
python scripts/build.py build-release --parallel
```

CMake descarga el paquete de versión configurado por el repositorio, verifica
su suma SHA-256 y reutiliza la caché de usuario extraída en compilaciones
posteriores. El canal precompilado solo admite macOS arm64. Los Mac Intel y las
compilaciones universales deben usar la compilación LLVM local predeterminada.
Las opciones avanzadas, como `NEVERD_LLVM_PREBUILT_TAG`, la URL de espejo, el
directorio de caché y una suma explícita, están documentadas en
`cmake/NeverDLLVMPrebuilt.cmake`.

Las correcciones de LLVM pertenecen al [fork LLVM NeverSight](https://github.com/NeverSight/llvm-project).
Confírmelas y fusiónelas allí; después actualice la revisión del submódulo LLVM de NeverD.
La compilación integrada compila ese código directamente; no añada archivos de parches LLVM ni copias modificadas en el directorio de construcción de NeverD.
Los paquetes precompilados deben reconstruirse desde el fork corregido para validar el mismo comportamiento.

<!-- i18n-section: workflow -->
## Flujo de ramas y pull requests

Parta de un `dev` actualizado y cree una rama temática concreta:

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

Abra pull requests contra `dev`, no contra una supuesta rama de versión.
Mantenga los commits fáciles de revisar: un propósito coherente, sin salida de
compilación generada, sin formateo ajeno al cambio y sin revisiones de
submódulos modificadas salvo que formen parte de la propuesta.

<!-- i18n-section: code-style -->
## Estilo de código

C/C++ siguen los [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html).
[`.clang-format`](../../.clang-format) determina formato y [`.editorconfig`](../../.editorconfig)
codificación, finales de línea y espacios. La versión C++ la fija `CMakeLists.txt`, no la guía
externa. Formatee los archivos modificados:

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

CI usa la versión fijada en [`scripts/check_clang_format.py`](../../scripts/check_clang_format.py);
use la misma para reproducir resultados. El formato no comprueba propiedad de cabeceras ni
literales de políticas. Evite reformateos globales para arreglos puntuales. Siga nombres y
descomposición vecinos, mantenga diferencias de plataforma en loader/lifter/backend y tipos
C++ internos fuera del SDK C puro.

<!-- i18n-section: headers -->
### Organización de archivos y cabeceras

- Los nuevos archivos C++ comienzan con banner LLVM nombre/propósito, `NeverD Decompiler`
  y Doxygen `\file`, como `lib/debug/PDBLoader.cpp`. Conserve avisos de autoría y licencia.
- Use `.h` autocontenidas con dependencias explícitas; la implementación incluye primero su
  interfaz. `.clang-format` ordena después cabeceras privadas vecinas, públicas `neverd/`,
  LLVM/Capstone entre comillas y finalmente entre ángulos. No dependa de orden accidental o inclusiones transitivas.
- Cabeceras nuevas usan guardas `NEVERD_..._H` derivadas de su ruta, por ejemplo
  `NEVERD_SDK_LIMITS_H` para `neverd/sdk/Limits.h`. Incluya suficientes directorios privados/de
  herramientas para evitar colisiones; comente el nombre en `#endif`. Aplique esto al normalizar
  explícitamente cabeceras existentes sin tocar otras ajenas al cambio.
- Interfaces compartidas en `include/neverd/<component>/`; privadas junto a sus fuentes en
  `lib/` o `tools/`. La ubicación no garantiza una API C SDK estable. Dependencias acíclicas;
  modelos/lectores centrales independientes de sesiones/transporte. Algoritmos en su componente,
  sin duplicar semántica CLI/SDK/worker. Véase [arquitectura](architecture.md).
- Use el espacio de nombres propietario y nombres descriptivos: tipos/campos `UpperCamelCase`,
  funciones `lowerCamelCase`, constantes de política `kUpperCamelCase`. Respete interfaces
  existentes, contratos C/Python y convenciones STL. Ningún `using namespace` global en cabeceras.
- Tablas con inclusión múltiple intencionada conservan `.def`/`.inc` y documentan sus macros;
  no son interfaces `.h` independientes y no deben tener guardas que bloqueen expansiones.
  Cambie cabeceras generadas mediante su generador.

Ejemplo de cabecera de política autocontenida:

```cpp
//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include <cstdint>

namespace neverd::sdk {

/// Traversal ceiling for a function without a recorded instruction extent.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
```

<!-- i18n-section: policy -->
### Propiedad de límites, valores predeterminados y configuración

Use `ResourceLimits.h` en directorios de herramientas añadidos directamente a la ruta de búsqueda de cabeceras. En un sistema de archivos que no distingue mayúsculas de minúsculas, un `Limits.h` local puede ocultar la cabecera del sistema `<limits.h>`.

Cada política tiene una definición autoritativa en la cabecera de límites/configuración de su
subsistema, incluso con un solo llamante: topes de entrada/salida, recursión/grafos, presupuestos
de trabajo/iteraciones, umbrales heurísticos, cachés, páginas/lotes, reintentos y tiempos máximos.
Incluye `constexpr`, macros, comparaciones literales, `min`/`clamp`, inicializadores y parámetros predeterminados.

Elija primero la autoridad existente:

| Responsable | Definición |
| --- | --- |
| Núcleo/pipeline compartido | [`include/neverd/Limits.h`](../../include/neverd/Limits.h) |
| SDK | [`include/neverd/sdk/Limits.h`](../../include/neverd/sdk/Limits.h) |
| Web | [`include/neverd/web/Limits.h`](../../include/neverd/web/Limits.h) o definición específica existente |
| Otro componente | Cabecera/tabla existente, o `Limits.h` específico en su capa |
| Solo CLI, worker o GUI | `ResourceLimits.h` o `Config.h` local, sin dependencia del núcleo |
| Opciones del llamante | Modelo propietario `Config.h`, `*Options.h` o `*Configuration.h` |

Mantenga cabeceras ligeras y agrupadas por propósito. Reutilice `*Limits.def` o tablas de
configuración y exponga su vista C++ en una cabecera autocontenida. No duplique políticas
entre `Limits.h`, `Constants.h` y `Config.h`, ni coloque políticas centrales en SDK/herramientas
o todas las políticas independientes en el `Limits.h` raíz.

Escalares C++ usan `inline constexpr` tipado y el espacio existente (`neverd::limits`,
`neverd::sdk`). Nombre propósito/unidad: `kMaxSourceBytes`, `kDefaultPageRows`,
`kRetryDelayMilliseconds`; documente cero/centinelas y límites inusuales. Separe valores por
defecto de máximos duros con significado distinto. Un modelo puede poseer el valor de campo;
consumidores usan modelo, fábrica o constante común. Derive valores relacionados sin repetir literales.

Adaptadores, persistencia, CLI/GUI/worker y Python/MCP reutilizan esa autoridad, incluidos
máximos de esquema y diagnósticos numéricos. Derive contratos multilenguaje de tablas o
enlaces generados; añada control de deriva si generar es inviable. Conserve compatibilidad C
pura. Un límite de vista previa/página más estricto requiere nombre y propietario propio;
igualdad numérica no implica la misma política.

Diseños binarios, ABI/protocolo/instrucciones, rangos enteros y costes de algoritmos de prueba
permanecen en sus modelos semánticos. Estados iniciales, índices, sugerencias de asignación y
entradas de prueba independientes no son automáticamente políticas. No sustituya el límite
esperado independiente por la constante de implementación que debe verificar.

Revise declaraciones y usos literales: guardas multilínea, bucles, macros, valores por defecto,
temporizadores y enlaces. Verifique significado/propiedad de cada política tocada; buscar
`Max*` no basta. Consolidar conserva tipos, valores, dirección de comparación, centinelas y
agotamiento/errores. Separe cambios de política y ejecute las [pruebas afectadas](testing.md).

<!-- i18n-section: artifacts -->
### Documentación y artefactos locales

Markdown conciso y verificable en fuentes, con enlaces internos relativos. Actualice docs
en la misma PR para cambios CLI/API, soporte, opciones y comandos. Sincronice todas las
traducciones de esta guía; revíselas antes de actualizar el SHA-256 `i18n-source`, manteniendo
secciones, ejemplos, API/opciones y destinos. Ejecute `python3 scripts/check_docs_i18n.py`.

Guarde informes de rendimiento locales, trazas y tiempos en directorios ignorados de build
o benchmark-results. Confirme herramientas de medición reutilizables y regresiones, no informes
locales ni tablas de resultados copiadas.

<!-- i18n-section: tests -->
## Ejecutar pruebas

Ejecute todas las pruebas registradas mediante el objetivo agregado:

```bash
python scripts/build.py build-release --parallel
cmake --build build-release --target check-neverd
```

Durante el desarrollo, use el objetivo pertinente más pequeño o una etiqueta
CTest:

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
python scripts/build.py build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

La [guía de pruebas](testing.md) documenta todos los objetivos de
conveniencia, las suites de transformación que solo tienen etiqueta, las
expresiones regulares para una sola prueba, la compilación de fixtures y los
recorridos de ida y vuelta con Unicorn. Si se omite un objetivo por faltar un
compilador cruzado o linker, informe de esa limitación; no describa la ruta no
ejecutada como aprobada.

<!-- i18n-section: python-sdk -->
### SDK de plugins Python

Para cambios del SDK ejecute primero Python puro y controles de deriva, después adaptadores nativos:

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
python scripts/build.py build-release --target \
  NeverDPluginRuntimeTests NeverDPythonRuntimeTests NeverDPythonPluginTests
ctest --test-dir build-release -R 'NeverD(Python|Plugin)' --output-on-failure
```

Declaraciones C públicas, firmas ABI Python, versiones paquete/CMake y política de entrega
forman un contrato revisado conjuntamente; actualícelos en el mismo cambio. Véase la [guía de plugins Python](python-plugins.md).

<!-- i18n-section: review -->
## Lista de control del pull request

Antes de solicitar una revisión:

- Rebase o fusione el `dev` más reciente según el flujo preferido de los
  mantenedores y resuelva deliberadamente los cambios de submódulos.
- Compile los objetivos afectados en Release, o explique por qué hace falta
  otro perfil.
- Ejecute las pruebas de regresión precisas y la suite pertinente más amplia
  que resulte práctica; incluya los comandos exactos y todos los skips en la
  descripción del PR.
- Conserve el lifting estricto: una instrucción no soportada no debe convertirse
  silenciosamente en una operación inferida ni en un `NOP`.
- Añada cobertura semántica para los cambios de comportamiento, no solo
  instantáneas textuales de IR.
- Mantenga fuera del diff la limpieza ajena, los archivos generados y los
  artefactos de compilación locales.
- Revise [cabeceras y propiedad de políticas](#estilo-de-código), guardas/valores numéricos y deriva
  de enlaces; superar el formato no basta.
- Actualice la documentación pública y para contribuidores cuando cambien el
  comportamiento, el soporte, las opciones, los comandos o la propiedad de las
  pruebas.

Para informes sensibles de seguridad que no deban comenzar como un pull request
público, siga [SECURITY.md](../../SECURITY.md).
