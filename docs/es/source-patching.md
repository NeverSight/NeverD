**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 3ceb56e2abf86f594f437a16b09a1888650cb49d4cf06860711e73458c52c68d -->

[← Documentation Index](README.md)

# Parches de código fuente y objetos nativos

NeverD compila una función de reemplazo con el compilador elegido y la instala en una entrada original explícita. El compilador define la semántica C/C++; NeverD gestiona direcciones, reubicaciones, almacenamiento, desenrollado y publicación. La reparación binaria y la reescritura del código elevado conservan sus propias canalizaciones. No se recompila el resto del programa.

<!-- i18n-section: compiler -->
## Elección del compilador

Elija `--compiler=clang`, `--compiler=gcc` o `--compiler=neverc`. Clang y el modo ordinario de NeverC pueden producir IR LLVM; `object` conserva las instrucciones nativas. La CLI selecciona objetos por defecto para GCC. El SDK Python también selecciona objetos para GCC. Nunca se cambia de compilador silenciosamente.

Use `--compiler-arg` o `compiler_args` del SDK para las rutas de inclusión, las definiciones de macros y las opciones de código fuente admitidas. Se rechazan los cambios de destino o modo de salida y los tokens de archivos de respuesta (`@file`) en cualquier posición, incluso como valores separados de una opción. Las rutas `-I` y las definiciones `-D` normales siguen admitidas.

Sustituya las direcciones de ejemplo por entradas originales verificadas. La declaración debe coincidir con la ABI original, incluidas la convención de llamada y la disposición de estructuras; una dirección no demuestra esa coincidencia. C++ requiere `--language=c++` y el ejecutable Clang++/G++ adecuado. Use una entrada `extern "C"` o el nombre decorado exacto. Se permiten funciones auxiliares. `--entry` y `--func` seleccionan el reemplazo; el antiguo `--from-c` también respeta la dirección.

También puede elegir `artifact="object"` explícitamente.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## Perfiles verificados y límites

[NeverC producer: 182fd2c5dc](https://github.com/NeverSight/NeverC/commit/182fd2c5dc)

**NeverC C y DynCode alojado**

NeverC solo acepta C puro en ELF, PE y Mach-O x64/ARM64. NeverD rechaza NeverC con `--language=c++` antes de iniciar el compilador, sin traducción ni cambio implícito de compilador. C++ y sus excepciones usan Clang o un controlador GCC compatible con el perfil de objetos ELF verificado. Los marcos C hosted pueden transmitir una excepción lanzada y capturada por el programa C++ que los rodea mediante registros reales de desenrollado; esto no añade C++ a NeverC.

`dyncode`: `--compile-mode=dyncode` usa `-fdyncode-hosted`: fuente, una entrada explícita, objeto nativo y sin enlaces originales. CLI y Python eligen objetos por defecto para este modo y GCC. Se verifican ambos hashes, cada byte enlazado y la entrada antes de publicar, usando CFI/SEH reales o registros compactos completos. Estado modificable/TLS, conversiones implícitas syscall/PEB/heap/string, imports no resueltos y código reordenado se rechazan. Los parámetros pueden llevar datos y callbacks.

```sh
neverd patch app --from-source=replacement.cpp --language=c++ \
  --compiler=clang --compiler-path=/path/to/clang++ \
  --entry=replacement --func=0x401140 --patch-report -o app.cpp
neverd patch app --from-source=replacement.c --compiler=neverc \
  --compiler-path=/path/to/neverc --compile-mode=dyncode \
  --entry=replacement --func=0x401140 --patch-report -o app.dyncode
```

Clang LLVM IR admite Windows ARM64 C++ FH3 y valida xdata, ABI del marco padre y grupos funclet/FuncInfo. Los manejadores de lenguaje de objetos nativos siguen fuera. `check_source_patch_cpp_runtime.py` ejecuta nueve escenarios en Windows nativo x64/ARM64, distinguiendo pruebas CPU y desenrollado OS.

| Compilador | Artefacto | Perfil de ejecución del SO verificado |
|---|---|---|
| Clang 22.1.6 | IR LLVM | ELF x64/ARM64 C/C++; Mach-O x64/ARM64 C/C++; PE x86/x64/ARM64 C; PE x64 C++ FH3 |
| Clang 22.1.6 | Objeto nativo | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C |
| GCC/G++ 13.3.0 | Objeto nativo | ELF x64/ARM64 C/C++ con runtime Itanium compatible |
| NeverC 3389.1.5 modo ordinario | IR LLVM | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 modo ordinario | Objeto nativo | ELF x64 C |
| NeverC 3389.1.5 C | LLVM IR / objeto nativo | ELF / PE / Mach-O x64/ARM64: fuente C pura |
| NeverC 3389.1.5 `hosted-v1` | Objeto nativo + carga sellada | ELF / PE / Mach-O x64/ARM64: fuente C y propagación a través de marcos C |

La tabla describe perfiles OS comprobados, sin garantizar todas las versiones u opciones. Los instaladores nativos ELF, PE y Mach-O aceptan las cuatro arquitecturas. GCC necesita un controlador ELF compatible y no produce LLVM IR. Las reubicaciones desconocidas, TLS, constructores globales durante la carga, LTO, símbolos common/IFUNC e importaciones sin resolver fallan explícitamente. El recibo JSON conserva compilador, argumentos, enlaces y direcciones; los objetos nativos incluyen SHA-256.

El campo `target` del recibo de un objeto nativo se obtiene de la ABI de la imagen cargada, incluidos los indicadores de coma flotante ARM ELF y la distinción iOS/watchOS. Los objetos compilados y suministrados siguen la misma regla de identidad.

La matriz CPU exige instalar correctamente código fuente, IR externo y objetos nativos en las doce combinaciones. Comprueba por separado ARMv7k watchOS y ARMv7 iOS, estado modificable／constante, argumentos flotantes y de pila de funciones originales y entradas tras cambiar la dirección de carga. La ejecución CPU no certifica el cargador OS, el desenrollador C++ ni cualquier ABI; véanse los perfiles OS anteriores.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

`CPU` significa instalación fuente／IR／objeto, recarga y ejecución exacta. ARM ELF obtiene EABI5, estado ARM/Thumb y ABI flotante de la imagen y registra un índice `EHABI` completo y ordenado. PE conserva filas x64/ARM y reubicaciones `HIGHLOW`/`DIR64`/`MOV32T`; x86 usa código de registro del compilador. Mach-O distingue watchOS DWARF e iOS `SjLj` por subtipo CPU. Los saltos de entrada ARM/Thumb son relativos y sobreviven al cambio de base.

Los objetos nativos usan sus símbolos de función acotados, definiciones auxiliares／de sección y registros de desenrollado, sin inventar identificadores MC ni recompilar IR. Los límites de funciones COFF requieren tamaños auxiliares exactos, secciones de una sola función (`-ffunction-sections`) o registros nativos de funciones de ejecución decodificados; nunca se deduce el tamaño del siguiente símbolo. El adaptador watchOS conserva marcos de funciones hoja para obtener registros reales. Los manejadores de lenguaje COFF/Mach-O nativos quedan fuera del perfil de objetos comprobado; las excepciones C++ usan la ruta IR verificada. Se rechazan desenrolladores ARM estáticos con registro exidx fijado, punteros absolutos Mach-O sin registrar y almacenamiento RELRO no soportado.

Los objetos C de GCC también se instalaron y ejecutaron en ELF x86, ARM32 y ARM64 mediante Linux QEMU user mode. El adaptador elige ARM/Thumb desde la imagen y las propiedades GNU se alinean según el ancho de puntero ELF.

<!-- i18n-section: references -->
## Referencias originales y almacenamiento

Los enlaces corresponden a declaraciones externas: `function` requiere una función original o stub de importación verificado; `data` requiere almacenamiento escribible; `const` requiere una declaración IR constante. Los datos de objetos necesitan una extensión distinta de cero. Los rangos no pueden cruzar límites ni conceder escritura en RELRO. Se rechazan callbacks al cuerpo original reemplazado: este perfil no tiene un trampolín que guarde el prólogo.

El código nuevo permanece RX y el estado mutable RW. Solo se reutilizan importaciones existentes; los slots GOT/IAT y los stubs invocables se distinguen. Mach-O necesita espacio para comandos de carga, por ejemplo `-Wl,-headerpad,0x400`; PE necesita espacio para cabeceras de sección. La falta de espacio falla. ELF `.data.rel.ro` y las protecciones CET/IBT originales requieren contratos adicionales y se rechazan por ahora.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

El crecimiento de tablas Mach-O necesita también espacio libre adyacente en el archivo y en memoria virtual; la reserva de cabeceras no lo proporciona. Las entradas verificadas distinguen ambos requisitos.

<!-- i18n-section: exceptions -->
## Excepciones C++

ELF usa FDE/LSDA del compilador; Mach-O usa compact unwind/DWARF. PE x64/ARM64 admite tablas FH3 con un runtime MSVC existente compatible e identidades de tipos coherentes. Los datos type-info pueden necesitar enlaces explícitos. FH4, personalities personalizadas, código SEH y modos CFG/EHCont/XFG activos se rechazan; se conservan las protecciones originales.

Los originales ELF x86/x64 que exigen IBT se rechazan actualmente: el instalador de código fuente todavía no ofrece un trampolín que conserve ENDBR.

Los casos ejecutados cubren retorno normal, catch interno, una excepción nueva capturada por el llamador original, excepciones desde un llamado original atravesando el reemplazo, limpieza RAII exactamente una vez, relanzamiento, terminación noexcept y funciones intactas. El llamador original debe permitir ya la propagación. Un parche no restaura catches eliminados alrededor de una función probada sin excepciones.

<!-- i18n-section: publication -->
## SDK y publicación

La API C aditiva es `neverd_patch_source` con `neverd_source_patch_options_v1`; `neverd_patch_source_report` devuelve el recibo. La entrada de objetos indica su longitud en bytes. La API anterior `neverd_patch_from_c` sigue disponible.

Se fijan las entradas y se prepara el candidato junto al destino, se vuelve a cargar y se comprueba cada entrada solicitada antes de publicarlo de forma exclusiva. No se sobrescriben archivos ni alias de enlaces simbólicos existentes. Un mapeo ausente hace fallar toda la petición. `--mode=inplace` se rechaza. Las transformaciones IR de sesión no se aplican a objetos nativos. Mach-O verifica y renueva firmas estrictamente; las firmas PE no se eliminan en silencio.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## Compilación y verificación

Elija un compilador compatible con `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (Clang 22.1.6 comprobado). La compilación integrada corrige una copia LLVM en el directorio de construcción sin modificar el submódulo ni la caché compartida. El fallo PLT x64 de r3/r4 puede rechazar un parche IR; los objetos nativos evitan esa ruta MC. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` exige la generación PLT exacta corregida.

`NeverDSourcePatchTests` contiene casos nativos C/API/objetos/excepciones; `NeverDSourcePatchMatrixTests` comprueba todas las combinaciones con Unicorn. `SourcePatchPE` ejecuta siete escenarios C++ con `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` y `NEVERD_TEST_WINDOWS_SDK_LIB`. Fuera de Windows requiere además `NEVERD_TEST_WINDOWS_RUNNER`. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` convierte las entradas ausentes en fallo. El workflow dedicado de Windows exige ejecución nativa y rechaza casos omitidos.

Las pruebas Linux en el Mac de desarrollo usan Docker y un cargador Linux explícito; PE usa Wine 11. Mach-O ARM64 se ejecuta en el Mac. Los resultados de CI Windows nativa deben comprobarse aparte: configurar un trabajo no implica que haya pasado. Las cifras históricas, fallos de referencia y límites de emuladores figuran en [los contratos detallados y el registro de validación](../source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

La copia LLVM también corrige referencias GOT de datos Mach-O x64: la tabla catch tipada de Clang es relativa al final del campo de reubicación de cuatro bytes. r3/r4 la desplaza cuatro bytes; la comprobación previa a publicar rechaza ese resultado. En el Mac, x64 usa Rosetta 2 y ARM64 se ejecuta directamente.

La validación nativa de Windows x64 [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805) superó las 16 pruebas específicas de PE/objetos/trampoline y las 143 pruebas de la matriz de destinos CPU, sin omisiones. El caso FH3 ejecutó siete escenarios de C++; la matriz utiliza Unicorn. Esta ejecución utilizó Clang 22.1.6 con MSVC y Windows SDK nativos, además de LLVM integrado.

El flujo de Windows también genera 27 reemplazos PE para x86, x64 y ARM64. `scripts/check_source_patch_pe_runtime.py` comprueba llamadas no hoja, estado mutable de punteros y argumentos de coma flotante o de pila mediante IR de fuente, objetos compilados y objetos suministrados. Ejecuta cada original y cada resultado tres veces: 30 observaciones de procesos por arquitectura. x86/x64 se ejecutan en Windows x64; ARM64, en `windows-11-arm`. El verificador exige el anfitrión nativo previsto, el mismo commit, resúmenes de archivos coincidentes y todos los casos; los ejecutores externos se registran por separado. Este perfil no demuestra compatibilidad con excepciones C++ de ARM64.

`NEVERD_TEST_MATRIX_NEVERC` selecciona el compilador opcional de la matriz. NeverC 3389.1.5 superó las 66 comprobaciones x64/ARM64 con ELF, PE y Mach-O, incluidas la instalación IR/objeto nativo y la ejecución CPU. Esta versión no admite x86 ni ARM32; utilice Clang, o un controlador GCC compatible para ELF. Para compilar PE con NeverC, el adaptador añade `-fno-autolink`: resuelve referencias en la imagen existente sin enlazar otro CRT. Los objetos suministrados con contratos de enlace `.drectve` siguen rechazándose.

La validación de procesos nativos de Windows [a4a5ae3cb](https://github.com/NeverSight/NeverD/actions/runs/38350079817) superó las 90 observaciones: 30 para cada arquitectura x86, x64 y ARM64, con IR de fuente, objetos compilados y objetos suministrados. x86/x64 se ejecutaron en Windows x64; ARM64, en `windows-11-arm`, con las mismas imágenes verificadas mediante sus hashes. La misma ejecución superó las 16 pruebas específicas de PE/objetos/trampoline y las 143 pruebas de la matriz Unicorn, sin omisiones. El caso FH3 mantuvo sus siete escenarios de ejecución de C++ x64.

La validación nativa fija NeverC en `182fd2c5dc`, con originales independientes y el instalador NeverD actual. `check_source_patch_hosted_pe_runtime.py` exige 36 observaciones por arquitectura Windows para IR/objetos/hosted C y propagación a través de dos marcos C. `check_source_patch_elf_runtime.py` exige 120 por arquitectura Linux para C, propagación hosted y nueve escenarios C++ de Clang. Recibos, resúmenes y arquitectura real deben coincidir; esto no afirma que el propio NeverC se ejecutara en el host CI. Linux x64 y ARM64 ejecutan los nueve escenarios para cada vía: IR Clang, objetos nativos Clang y objetos nativos GCC. NeverC sigue limitado a C.

Las 204 observaciones del antiguo perfil de traducción son históricas y no prueban el contrato actual de C puro. Las tres rutas NeverC actuales usan C; Clang C++ FH3 se valida por separado. macOS x64 usa Rosetta 2. Unicorn/QEMU no sustituyen la validación nativa del sistema y del desenrollador.