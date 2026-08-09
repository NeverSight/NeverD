**Idiomas**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← Índice de documentación](README.es.md)

# Probar NeverD

Las pruebas de NeverD responden a tres preguntas distintas: si una
representación tiene la forma esperada, si una ruta completa funciona para una
fixture binaria y si el código generado conserva el comportamiento. Elija la
suite más pequeña que responda a la pregunta del cambio y ejecute después el
agregado más amplio antes de un pull request de alto riesgo.

## Configurar una compilación de pruebas

Las pruebas están desactivadas si no se habilita `BUILD_TESTING`. Release es la
opción normal para la suite completa; Debug conserva aserciones y ejecución paso
a paso, pero no está optimizado de forma intencional ni representa los
benchmarks de decodificación.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

El conjunto completo de fixtures necesita `clang` para compilar entre objetivos
y los linkers LLVM (`ld.lld` y `lld-link`) en el `PATH`. CMake genera siempre
muchos objetos reubicables y fixtures ELF/PE enlazadas cuando existe el linker
correspondiente. Una prueba omitida porque el host no puede compilar o enlazar
su fixture es cobertura no ejecutada, no una aprobación de ese objetivo.

Consulte [CONTRIBUTING.md](i18n/CONTRIBUTING.es.md) para clonación, perfiles de
compilación y LLVM precompilado en macOS.

## Distribución de pruebas

`add_neverd_unittest` crea un ejecutable GoogleTest y asigna a cada caso
descubierto una etiqueta CTest igual al nombre de ese objetivo ejecutable.

| Área fuente | Objetivo y etiqueta CTest | Cobertura |
|-------------|---------------------------|-----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Invocación de procesos hijo multiplataforma, quoting, redirecciones y códigos de salida |
| `unittests/libc` | `NeverDLibCTests` | Nombres libc conocidos y clasificación |
| `unittests/lift` | `NeverDLiftTests` | Formas LowIR decoder/lifter, etapas IR, loaders, relocations, fixtures de formato, descompilación y flujos patch representativos |
| La mayoría de `unittests/semantic` | `NeverDSemanticTests` | Semántica diferencial de instrucciones, ABI, control de flujo, expresiones C y lift/recompile |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Equivalencia de reescritura/ofuscación entre cuatro ISA y tres formatos objeto |
| Archivos de transformación enfocados en `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Sondas rápidas de reenlazar separadas del gran binario semántico |

Las fuentes de registro son
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) y
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt).

## Cómo se producen las fixtures

### Fixtures de lift y formato

`unittests/lift/CMakeLists.txt` compila fuentes C y ensamblador entre objetivos
durante la compilación. Los triples Clang producen objetos ELF x86-64, i386,
AArch64 y ARM32, objetos e imágenes enlazadas PE/COFF y objetos Mach-O i386
PIC/no-PIC. Cuando está disponible LLD, también se enlazan objetos seleccionados
como ejecutables para pruebas patch. `NeverDLiftTests` depende del objetivo
`lift-test-objects`, por lo que una compilación normal de ese binario actualiza
sus fixtures generadas.

La mayoría de pruebas lift usan `NeverDLiftFixture.h` para invocar el CLI
`neverd` compilado e inspeccionar LowIR, MedIR, HighIR, LLVM IR, el C generado o
un binario reescrito. La variable de entorno `NEVERD` puede sustituir la ruta
del CLI en un experimento manual enfocado; las ejecuciones CTest normales usan
el ejecutable incrustado por CMake.

### Recorridos diferenciales Unicorn

La fixture semántica prueba comportamiento en vez de forma textual:

1. Escribir un caso pequeño en C/ensamblador o construir LLVM IR.
2. Compilarlo con Clang/LLVM para el objetivo solicitado.
3. Ejecutar el código máquina original en Unicorn y capturar el retorno esperado u otro estado definido por la fixture.
4. Cargarlo y hacer lift con NeverD, emitir LLVM IR y recompilar el resultado a código máquina.
5. Ejecutar el código regenerado con la misma ABI, entradas, disposición de memoria y modelo de CPU.
6. Comparar los resultados observables.

La implementación principal es
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h).
La fixture patch-full usa `Codegen::compileForRewrite`, el mismo backend de
reescritura que las operaciones patch, y compara después código base y
transformado en toda la cuadrícula ISA/formato 4×3.

Un fallo semántico determinista de NeverD debe hacer fallar la prueba. Reserve
los skips para límites explícitos de capacidad externa y lea su motivo: un
resumen verde sin cross-linker no demuestra que se haya ejecutado la ruta del
formato.

## Objetivos de una sola orden

Los objetivos personalizados compilan sus dependencias y ejecutan CTest con
paralelismo derivado de las CPU del host:

| Objetivo CMake | Selección |
|----------------|-----------|
| `check-neverd` | Todas las pruebas registradas |
| `check-neverd-semantic` | Solo `NeverDSemanticTests` |
| `check-neverd-patch-full` | Solo `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | Solo `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | Solo `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | Solo `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
```

`NeverDIndCallXformTests` y `NeverDAvxUpperXformTests` no tienen actualmente un
objetivo de conveniencia `check-neverd-*`. Compílelos y selecciónelos por
etiqueta como se muestra abajo. `check-neverd-semantic` tampoco incluye los
binarios separados de transformación o patch-full; use `check-neverd` para el
agregado completo.

## Flujo CTest incremental

Compile primero el ejecutable propietario y seleccione después su etiqueta.
Así evita reenlazar grandes objetivos semánticos no relacionados.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4
```

Use un nombre CTest derivado de GoogleTest para una sola regresión:

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

Selectores útiles:

| Comando | Propósito |
|---------|-----------|
| `ctest --test-dir build-release -N` | Enumerar casos descubiertos sin ejecutarlos |
| `ctest --test-dir build-release -L '<regex>'` | Seleccionar etiqueta de binario de pruebas |
| `ctest --test-dir build-release -R '<regex>'` | Seleccionar nombres de casos |
| `ctest --test-dir build-release --output-on-failure` | Mostrar diagnósticos solo para fallos |
| `ctest --test-dir build-release --stop-on-failure` | Detener tras el primer fallo |
| `ctest --test-dir build-release --parallel 4` | Ejecutar hasta cuatro casos en paralelo |

El descubrimiento GoogleTest usa `DISCOVERY_MODE PRE_TEST`, por lo que el
binario correspondiente debe existir antes de que CTest lo enumere. Los
timeouts por caso y de descubrimiento independientes están definidos en
`cmake/AddNeverD.cmake` y solo deben ampliarse para suites con casos pesados
medidos.

## ¿Qué pruebas deben cambiar con el código?

| Área de cambio | Empezar por | Considerar después |
|----------------|-------------|--------------------|
| Lifter de arquitectura o decode | Caso nombrado en `NeverDLiftTests` | Recorrido semántico de la ISA correspondiente |
| CFG LowIR, detección de funciones, jump tables | Casos lift CFG/switch | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests` o `NeverDTwoTableXformTests` |
| MedIR, ABI, flags, tipos, SSA | Casos lift MedIR/convención de llamada | Casos `NeverDSemanticTests` entre ISA |
| HighIR o C estructurado | Casos HighIR/decompile | `NeverDCFGLoopXformTests` y compilación del C generado |
| Loader PE/ELF/Mach-O o relocation de entrada | Fixture de formato correspondiente en `unittests/lift` | Prueba de carga/descompilación de todas las etapas para la celda |
| Codegen de reescritura o relocation de salida | Casos `RewriteCodegenRTTests` | `NeverDPatchFullTests` y fixture patch enlazada si existe |
| Transformación LLVM IR usada por patch | Binario de transformación enfocado | Cuadrícula de pases compuestos `NeverDPatchFullTests` |
| C API o CLI | Prueba SDK/query directa y `unittests/semantic/CLIEndToEndTests.cpp` | Suite pipeline/formato pertinente |
| Reconocimiento libc | `NeverDLibCTests` | Casos semánticos call/ABI si cambia el comportamiento |
| Ejecución o quoting de procesos | `NeverDTestProcessTests` | Un caso CLI/semántico afectado en cada host soportado |

Las pruebas deben expresar el contrato en el límite estable más bajo. Una
prueba de forma LowIR sirve para atribuir el lifter; hace falta un recorrido
semántico cuando dos formas IR plausibles podrían comportarse de manera
distinta. Evite volcados golden de funciones completas si basta una aserción
pequeña de opcode, CFG o estado observable.

## Relación con CI

CI compila Release con pruebas habilitadas en Linux, macOS y Windows, audita el
inventario descubierto y después aplica exclusiones de etiquetas específicas
de plataforma. Los perfiles están en `.github/workflows/ci.yml` y
`scripts/audit_ci_test_inventory.py`. Como ningún shard de la matriz representa
todas las suites costosas, un `check-neverd` local sigue siendo la señal previa
a fusión completa más clara si la máquina dispone de todas las herramientas
cruzadas necesarias.
