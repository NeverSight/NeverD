**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 1eae101089e730c29ba3fda92db515fe148fe41b758d6f5b69156252591d9b3f -->

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
| Clang 22.1.x | IR LLVM | ELF x64/ARM64 C/C++; Mach-O x64/ARM64 C/C++; PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| Clang 22.1.6 | Objeto nativo | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C; Mach-O x64/ARM64 C; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| GCC/G++ 13.3.0 | Objeto nativo | ELF x64/ARM64 C/C++ con runtime Itanium compatible; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
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
## Llamadas al original, ampliación de disposición y dependencias

Los enlaces nombran declaraciones externas: `function` autentica una función original o stub de importación, `data` exige almacenamiento escribible y `const` una declaración IR constante. Los datos de objeto requieren extensión no nula, sin cruzar límites ni conceder escritura a RELRO. Use explícitamente `original` para llamar al cuerpo guardado de la función sustituida. La entrada pública sigue llamando al reemplazo; la guardada ejecuta un prefijo reubicado de instrucciones completas y continúa en el original. Fuente, IR suministrado y objetos nativos comparten el contrato, incluidos argumentos flotantes/en pila y múltiples callbacks.

Un callback requiere límites de función, modo de instrucciones, continuación y pruebas de desenrollado autenticados. Se rechazan antes de publicar las instrucciones desconocidas, referencias PC-relativas inseguras, saltos al prefijo copiado, reubicaciones de carga no compatibles, callbacks superpuestos y prólogos con manejadores no modelados. El llamador debe proporcionar una declaración ABI coincidente. El recibo registra las entradas guardadas en `original_callbacks`.

PE32/PE32+ puede ampliar una tabla de cabeceras de sección llena moviendo rangos del archivo y actualizando los punteros modelados de depuración, símbolos, certificados y overlay, conservando los RVA originales. Mach-O x64/ARM64 PIE puede obtener espacio para comandos y desenrollado mediante una extensión verificada del prefijo de cabecera si lo permiten la guarda page-zero adyacente y el segmento linkedit final; las direcciones de código/datos originales no cambian. Este perfil no exige `-Wl,-headerpad,0x400` ni páginas libres adyacentes para desenrollado. Referencias explícitas a la cabecera, comandos desconocidos, propiedad mixta de código/datos, rangos malformados y disposiciones no compatibles se rechazan. Cabeceras y desenrollado se planifican juntos antes de instalar.

El código nuevo sigue RX y el estado mutable RW. Las solicitudes explícitas `--dependency` añaden funciones/datos mediante metadatos dinámicos ELF, descriptores de importación/IAT PE o metadatos dylib/bind Mach-O. Se conservan identidades, ordinales y versiones existentes, distinguiendo slots y stubs invocables. Elija el nombre/ruta esperado por el cargador del OS destino. El ejemplo usa un nombre de biblioteca compartida ELF. La ausencia de biblioteca o símbolo debe producir un error real del cargador al ejecutar. TLS, IFUNC, nuevos contratos de versión, constructores al cargar y reubicaciones no modeladas siguen sin soporte; ELF `.data.rel.ro` y CET/IBT necesitan contratos adicionales. En objetos COFF nativos, declare los nuevos imports de funciones/datos con `__declspec(dllimport)` para que sus reubicaciones identifiquen la IAT. Se rechazan las referencias externas directas ordinarias.

```sh
# ELF example: the original target is at 0x401140.
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=original:saved_target=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --dependency=function:extra_function=libpatch_extra.so \
  --dependency=data:extra_state=libpatch_extra.so \
  --patch-report -o app.bound
```

Las opciones de dependencia, añadidas al final y comprobadas según el tamaño de estructura, sirven para fuente, IR suministrado y objetos nativos. El recibo incluye `added_imports`; al recargar el candidato se vuelven a comprobar callbacks, importaciones, permisos y bytes instalados. Un fallo conserva la entrada y cualquier destino existente.

<!-- i18n-section: examples -->
## Ejemplos ejecutables

[Ejemplos completos para Windows/macOS/Linux](../../examples/source-patch/README.md) incluyen el código del programa, el reemplazo y la biblioteca, comandos de compilación y un plugin real del SDK de Python alojado en NeverD. Ejecute desde la raíz del repositorio con un build de NeverD con Python y un directorio de salida nuevo.

```sh
# Linux / macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-native
```

```powershell
# Windows: Visual Studio developer PowerShell
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-native
```

Se ejecutan ambas rutas por defecto; añada `--api cli` o `--api sdk` para elegir una. La dirección de destino se detecta automáticamente. Con `7`, el original imprime `8` y ambos parches imprimen `36`. El controlador comprueba ejecuciones, recibos y fallos reales del cargador sin la biblioteca; los comandos completos se guardan en `commands.json`.

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
    compiler="clang", compiler_path="/path/to/clang", artifact="object",
    bindings={
        "saved_target": ("original", original_address),
        "shared_counter": ("data", original_counter_address, 4),
    },
    dependencies={
        "extra_function": ("libpatch_extra.so", "function"),
        "extra_state": ("libpatch_extra.so", "data"),
    },
)
```

<!-- i18n-section: verification -->
## Compilación y verificación

Los seis trabajos nativos OS/ISA seleccionados pasaron sin fallos ni omisiones: 3669 ejecuciones GoogleTest, 1008 observaciones de procesos nativos (504 funcionales y 504 de excepciones C++), 12 rechazos reales del cargador, 96 imágenes y 84 recibos de publicación.

| Host | Fuente | Harness | Trabajo CI | Pruebas | Observaciones / rechazos |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

Los artefactos validan exactamente los commits de fuente y harness indicados abajo. Los commits posteriores de integración dev y documentación tienen controles locales y documentales separados. Los fallos o cancelaciones anteriores siguen siendo fallos. Las 216 observaciones suplementarias históricas, PE ARM32 y los resultados CPU/compatibilidad de Apple antiguo de 32 bits no prueban nueva cobertura de hardware nativo ni de excepciones C++.

La validación Windows ARM64 usa una DLL C Unicorn ARM64 nativa, compilada realmente desde el submódulo fijado con LLVM-MinGW verificado, una biblioteca de importación MSVC y una sustitución explícita del destino CMake. La copia de compilación aislada autentica tres correcciones del host: el par de saltos sin SEH, un callback ARM64 de compromiso de páginas con sincronización de caché de instrucciones y máscaras inmediatas de 64 bits para Windows LLP64. Las fuentes y revisión del submódulo no cambian. Esto valida el host de la matriz CPU, sin establecer soporte predeterminado para JIT MSVC ARM64. Los cinco controles de recuperación del host se registran aparte de las pruebas de procesos parcheados nativos y excepciones C++.

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

Las 204 observaciones del antiguo perfil de traducción son históricas y no prueban el contrato actual de C puro. Las tres rutas NeverC actuales usan C; Clang C++ FH3 se valida por separado. En el Mac de desarrollo con Apple Silicon, macOS x64 usa Rosetta 2. Unicorn/QEMU no sustituyen la validación nativa del sistema y del desenrollador.

Las reubicaciones de instrucciones AArch64 requieren sumandos RELA explícitos. Los sumandos REL implícitos de instrucciones se rechazan antes de publicar; sus campos de bits no se interpretan como datos ordinarios. Las reubicaciones de datos PREL64 admitidas conservan los sumandos completos con signo tanto en REL como en RELA.

La validación del productor C [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591930213) superó las 312 observaciones de procesos nativos: 36 en cada arquitectura Windows x64 y ARM64, y 120 en cada arquitectura Linux x64 y ARM64. Linux verifica por separado nueve escenarios de excepciones para Clang IR, objetos nativos Clang y objetos nativos GCC; las entradas NeverC de IR, objetos y hosted DynCode son todas C.

La validación del instalador recompilado [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591928600) superó 19 comprobaciones PE/objeto, los 143 casos de la matriz CPU y 474 observaciones de procesos nativos Windows/Linux. Los procesos Windows x86 se ejecutaron en Windows x64; x64 y ARM64 usaron hosts correspondientes. La [validación Linux](https://github.com/NeverSight/NeverD/actions/runs/38585759996) separada superó los 108 casos originales más tres comprobaciones LLVM-C y tres Clang/GCC C++, sin fallos ni omisiones.

La [validación de integración 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627694097) volvió a verificar las 312 observaciones de procesos en Windows/Linux nativos x64 y ARM64. Las 30 imágenes finales coinciden con los bytes ya validados; se actualizaron y comprobaron de forma independiente los hashes del código y los comprobantes de publicación exclusivos de C.

La [validación ampliada de Linux en 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38626866257) superó 429 comprobaciones ejecutadas, sin fallos ni omisiones: los 108 casos originales, 55 de Swift completo, 143 de objetivos CPU, 117 de sesiones nativas, tres de LLVM-C y tres de C++ con Clang/GCC. La batería local completa de parches binarios también superó sus 22.961 casos en esta integración. Los procesos nativos y la cobertura con emulación CPU se contabilizan por separado.

La [validación del instalador de Windows recompilado en 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627581732) superó las 182 comprobaciones específicas y 474 ejecuciones de procesos nativos, sin fallos ni omisiones. Una auditoría independiente de los artefactos verificó cada hash de imagen, arquitectura, escenario y repetición. Tras integrar los cambios de ABI y desenrollado en `e1cac949f`, la batería completa de parches binarios volvió a superar sus 22,961 casos, sin fallos ni omisiones. NeverC sigue limitado a C puro; las sustituciones C++ utilizan Clang/GCC.

<!-- i18n-section: runtime-coverage -->
## Verificación de procesos macOS y de 32 bits

El flujo de imágenes exige ejecución nativa en macOS ARM64 (`macos-15`) e Intel x64 (`macos-15-intel`). Cada arquitectura ejecuta 96 observaciones que cubren fuentes/objetos C de Clang, estado modificable mediante punteros, argumentos flotantes/en pila, nueve escenarios de excepciones C++ de Clang y las salidas IR/objeto/hosted del NeverC de C puro existente. Las firmas Mach-O se verifican estrictamente antes de ejecutar; el código hosted instalado debe coincidir byte por byte con la carga del compilador.

El mismo flujo exige 30 observaciones Windows x86 en Windows x64, 48 Linux x86 en Linux x64 y 96 Linux ARM32 mediante QEMU en modo usuario, repartidas por igual entre ARM y Thumb. Las pruebas Linux incluyen objetos GCC correspondientes al destino. Cada escenario se repite tres veces. Los informes distinguen `native`, `compatibility`, `rosetta` y `qemu-user`, y registran tanto la arquitectura de destino como la real del anfitrión. Rosetta no satisface un requisito Intel nativo.

Los 14 perfiles CPU siguen cubiertos por la matriz de 283 pruebas de fuente/IR/objeto. PE ARM32, Mach-O i386, ARMv7 iOS y ARMv7k watchOS tienen actualmente verificación CPU, sin afirmar verificación del cargador OS nativo ni del desenrollador C++. NeverC sigue limitado a C puro en x64/ARM64; Clang y los controladores ELF GCC correspondientes producen los destinos de 32 bits. El verificador no compila ni modifica NeverC.

Windows x64 y ARM64 ejecutan también 36 observaciones Clang C++ FH3 cada uno.

Los punteros modificables Mach-O se inicializan durante la ejecución; las direcciones estáticas siguen necesitando un registro de reubicación de carga no admitido y tienen una prueba explícita de rechazo. Las funciones COMDAT ELF de tamaño cero solo se aceptan si la firma es la única función al inicio de su sección ejecutable y la CFI coincide exactamente con toda la sección. Los casos con desplazamiento interior, sección compartida, relleno, sin grupo o sin CFI siguen rechazándose.

ELF ARM y Thumb tienen perfiles distintos. Los productores reciben explícitamente `-marm` o `-mthumb`, y los enlaces de funciones nativas conservan el bit de estado de instrucciones original. Los procesos ELF de 32 bits usan PIE con reubicaciones dinámicas reales. ARM32 QEMU registra un `--loader` glibc explícito y un sysroot: su cálculo AUXV no sigue las cabeceras trasladadas como el núcleo Linux. Esta ruta verifica carga glibc y ejecución bajo QEMU, sin acreditar hardware ARM32 ni el cargador nativo del núcleo.

Para estas imágenes ELF parcheadas, las rutas `qemu-user` de Linux x86 y ARM32 requieren un `--loader` glibc explícito del sysroot correspondiente a la arquitectura de destino. Linux x86 con `compatibility` usa el soporte de 32 bits del núcleo del anfitrión Linux x64 y se registra por separado de la ejecución QEMU; no acredita ejecución en un anfitrión x86 nativo de 32 bits.

La [validación de procesos](https://github.com/NeverSight/NeverD/actions/runs/38694434082) de `fcee81c9a5e2f20a71090ade23ae833175f64c59` superó las 750 observaciones: 576 en hosts nativos de la misma arquitectura, 78 en modo de compatibilidad x86 y 96 ARM/Thumb mediante QEMU en modo usuario. Una auditoría independiente verificó los hashes de las 126 imágenes, los entornos de ejecución y todos los escenarios. La [recompilación Windows](https://github.com/NeverSight/NeverD/actions/runs/38694628242) superó 194 comprobaciones específicas y 474 observaciones de procesos Windows/Linux. La [recompilación Linux](https://github.com/NeverSight/NeverD/actions/runs/38694638358) superó 444 comprobaciones, incluidos los 62 fallos y 46 casos omitidos originales, sin fallos ni omisiones. Siguen vigentes los límites de hardware y cargador indicados arriba. NeverC continúa limitado a C puro y su repositorio no se modificó.

La [validación adicional de ARM/Thumb](https://github.com/gmh5225/test_mac_intel/actions/runs/38750504263) superó las 96 observaciones (48 ARM y 48 Thumb) de las 32 imágenes de prueba C de `fcee81c9a5e2f20a71090ade23ae833175f64c59` en `ubuntu-24.04-arm`. El kernel de Linux cargó cada imagen directamente mediante `/lib/ld-linux-armhf.so.3`, sin QEMU ni un comando explícito del cargador. Se trata de `compatibility` en un host ARM64, no de un host nativo de 32 bits. El commit del programa de pruebas `3ad17ac9608d6bd9f5a4d44953ec375524e5730b` y el de las imágenes se registran por separado. Auditorías independientes verificaron los hashes de las imágenes, los 30 comprobantes de publicación, todos los escenarios y repeticiones, y la ausencia de reglas de intérprete `binfmt_misc` coincidentes.

Las [pruebas separadas de los runners alojados](https://github.com/gmh5225/test_mac_intel/actions/runs/38749138719) mostraron que el runner Windows 11 ARM64 proporcionado rechaza programas ARM32 con `WinError 216`. En macOS 15.7.9 con Xcode 16.4, los SDK disponibles no pudieron enlazar programas macOS i386 ni iOS ARMv7. Un programa watchOS ARMv7k se enlazó correctamente, pero no se acreditó su ejecución en un dispositivo físico. El éxito de estas comprobaciones no valida esas plataformas; su cobertura de hardware nativo sigue pendiente. NeverC no se compiló ni modificó.

Las [comprobaciones Linux adicionales en Actions personal](https://github.com/gmh5225/test_mac_intel/actions/runs/38773830274) superaron 96 ejecuciones ARM/Thumb dentro de un invitado ARM32 completo en QEMU TCG, con kernel Linux de 32 bits y glibc, más 30 ejecuciones PE ARM32 mediante Wine en ese mismo invitado. El kernel invitado carga directamente las imágenes ELF; esta emulación de sistema se distingue de la ruta `qemu-user` con cargador explícito. La instalación del registro de Wine en la capa de compatibilidad del anfitrión ARM64 se registra por separado y no cuenta como ejecución de parches. La inicialización de Wine en el invitado y cada ejecución medida deben completarse dentro de los plazos originales.

Las [comprobaciones de compatibilidad Apple](https://github.com/gmh5225/test_mac_intel/actions/runs/38771214358) superaron 30 ejecuciones Mach-O i386 con Darling en Linux y 60 ejecuciones ARMv7/ARMv7k con touchHLE en macOS Intel. Cada original y cada sustitución desde IR fuente, objeto compilado u objeto proporcionado se ejecutan tres veces. Las auditorías independientes verificaron las 72 imágenes, 66 recibos de publicación y 216 ejecuciones de estas pruebas adicionales, incluidos los tamaños de artefactos GitHub, SHA256, commits exactos e inventarios completos. Las entradas Linux siguen fijadas a `fcee81c9a5`; las PE se generaron en `a542f97ba` y las Mach-O en `8f5f06762`. Los commits del sistema de pruebas se registran por separado en las ejecuciones enlazadas.

Las correcciones Mach-O autentican las funciones Thumb seleccionadas independientemente de la clasificación ARM/Thumb mixta de la imagen, normalizan los punteros de exportación Thumb acreditados y las direcciones de código de desenrollado ARM32 sin cambiar los punteros de datos ni las longitudes de los rangos. La compilación con fuentes LLVM integradas superó localmente las 283 comprobaciones de la matriz CPU y las 258 de contratos de excepción, sin fallos ni omisiones. Las pruebas Mach-O antiguas usan un enlazador de código abierto documentado y reservan espacio de desenrollado; i386 tiene una entrada de arranque correctamente alineada y un segmento de página cero de 64 KiB.

Estos resultados demuestran ejecución de C en los emuladores y capas de compatibilidad indicados. Wine no es Microsoft Windows, Darling no es macOS y la ejecución ARMv7k dentro de la HLE de iOS no valida watchOS, WatchKit ni dispositivos Apple. No se deduce cobertura de SDK Apple antiguos ni de hardware nativo de 32 bits; estas pruebas tampoco validan el desenrollado de excepciones C++. La evidencia nativa y de compatibilidad anterior permanece separada. NeverC no se compiló ni se modificó.
