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
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMAnalyzerTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadatos hardfork, normalización de entrada, CFG/SSA/recuperación, semántica del intérprete, ejecución diferencial LLVM/C/Solidity y API pública |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFSemanticTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFEmitterTests`, `NeverDSBFIntegrationTests` | Metadatos v0-v4 y diseños ELF, verificación estricta, CFG/recuperación, ejecución raw independiente, verificación LLVM, compilación C/Rust y enrutamiento de la API pública |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Equivalencia de reescritura/ofuscación entre cuatro ISA y tres formatos objeto |
| Archivos de transformación enfocados en `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Sondas rápidas de reenlazar separadas del gran binario semántico |
| `unittests/corpus` (submódulo) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests` | Metadatos de excepciones y de runtime leídos de 305 binarios reales fijados, cada uno declarado en un manifiesto con los mínimos que su recuperación debe superar |

Las fuentes de registro son
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) y
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) y
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt).

### El corpus binario fijado

Cualquier otra suite construye lo que prueba; el corpus no: es un submódulo de
binarios que produjeron cadenas de herramientas reales, en hosts y para destinos
que este repositorio no alcanza. Cada uno está fijado por digest y junto a él un
manifiesto declara los mínimos que su recuperación debe superar. Es el único
lugar donde una afirmación sobre lo que NeverD lee de, por ejemplo, un objeto
compartido `armv7` compilado con `-O2` y sin símbolos tiene respuesta en vez de
discusión.

Las suites solo se construyen cuando al paso de configuración se le indica que
las busque, así que esa opción es todo lo que las mantiene bajo prueba:

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` ejecuta todas las líneas;
`check-neverd-windows-eh-corpus`, `check-neverd-rust-eh-corpus`,
`check-neverd-go-eh-corpus` y `check-neverd-cxx-itanium-eh-corpus` ejecutan una
cada uno. Los tres hosts de CI configuran con la opción y corren las cuatro
líneas: los bytes son idénticos en todas partes, pero lo que los lee no lo es, y
una pasada del corpus en un host no prueba nada sobre los otros dos.
`scripts/audit_ci_test_inventory.py` rechaza un inventario al que le falte
cualquiera de las cuatro etiquetas, porque una compilación que dejó de leer el
corpus en silencio es una regresión que ningún test puede atrapar: el test es
justamente lo que desapareció.

La auditoría de opcodes EVM hace en cada ejecución un `git fetch` superficial
del `HEAD` remoto del
[repositorio oficial go-ethereum](https://github.com/ethereum/go-ethereum) y
después informa del commit exacto auditado. Reutiliza la caché bare ignorada en
`build/evm-opcode-audit/go-ethereum.git`, pero la actualiza antes de leer el
inventario cerrado de opcodes y sus asignaciones de bytes:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

CI ejecuta la misma auditoría live en cada push y pull request, por activación
manual y una vez al día; así detecta deriva upstream aunque NeverD no cambie.
Para una reproducción offline o histórica, seleccione explícitamente un
checkout existente:

```bash
python3 scripts/audit_evm_opcode_metadata.py \
  --geth-root /path/to/go-ethereum
```

La auditoría sólo permite las exclusiones nombradas en
`EVMUpstreamOpcodePolicy.def`; cualquier opcode upstream que no esté
representado o revisado de forma explícita hace fallar el comando. El parser y
los diagnósticos de deriva tienen cobertura unitaria Python independiente en
CI, ejecutable con:

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

Para cambios de control de flujo EVM, ejecute primero el contrato de punto fijo
y dominio de alturas:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Estos casos cubren retornos internos entre bloques, uniones finitas con varios
destinos, convergencia de bucles y orden determinista de aristas, alturas de
pila dependientes del camino, widening acotado, sobreaproximación cartesiana
inducida por correlación, saltos desconocidos, destinos inválidos precisos y
fallos de pila en modos estricto y relajado. Después ejecute los siete binarios
EVM y la auditoría de metadata upstream; los cambios del CFG pueden afectar al
emitter y a integración aunque la forma local del analizador sea correcta.

Para cambios de dataflow en MedIR/HighIR, ejecute también los contratos de phi
constante, selector, operandos tipados, grafo malformado y cadena profunda:

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

Estos casos prueban phis cíclicos iguales y conflictivos, expresiones de
selector no adyacentes y entre bloques, ambos órdenes de operandos de igualdad,
comprobaciones exactas de ancho ABI, operandos tipados de storage/event/calldata,
tratamiento determinista de MedIR malformado y un recorrido iterativo de 16.384
valores productores.

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

### Reconstrucción de excepciones de Windows

Los cambios de excepciones tabulares de Windows necesitan tanto pruebas de
representación como una prueba de patch sobre un PE enlazado. El filtro
específico de lift cubre el modelo normalizado de unwind/SEH/C++, las entradas
corruptas, las aristas excepcionales del CFG, HighIR, la generación LLVM WinEH,
el reemplazo del directorio de excepciones y la reconstrucción de Guard CF/EH
continuation:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

La fixture ensamblador x64 protegida requiere el objetivo Windows de Clang y
`lld-link`; su enlace CMake usa `/guard:cf` y `/guard:ehcont`. Un skip por falta
del cross-linker no demuestra el camino de imagen final. Un caso de integración
correcto demuestra que el PE reescrito puede volver a cargarse y que sus tablas
runtime-function, unwind, load-config, Guard CF y Guard EH continuation siguen
ordenadas, respaldadas por el archivo y limitadas a objetivos ejecutables.

La fixture FH3 enlazada cubre de forma independiente el cierre C++ nativo:
tablas de estado fijas, anotaciones HighC, conservación de la personality,
objetivos catch generados y el grafo IP-to-state recargado.

Consulte [Reconstrucción de excepciones de Windows](windows-exception-reconstruction.es.md)
para la matriz de soporte de análisis/nativo y el contrato de patch fail-closed.

### Modelos de excepciones por lenguaje

Todo lo que no es el modelo tabular de Windows vive en un único objetivo
enfocado. `NeverDLanguageEHTests` cubre la cadena de frames DWARF, el área de
datos específica del lenguaje de Itanium, ARM EHABI, el compact unwind de
Darwin, los metadatos de frame del runtime de Go, la maquinaria de pánico de
Rust y los tres runtimes de Objective-C:

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

Las tablas de esta suite se ensamblan byte a byte en lugar de compilarse, porque
la mayoría de las combinaciones que se quieren probar no las emite junta ninguna
cadena de herramientas. Objective-C es el caso más claro: los tres runtimes
emiten una LSDA de Itanium y solo difieren en qué contiene una ranura de la
tabla de tipos, y esa diferencia es total, no de grado. La ranura de Apple
direcciona un `objc_typeinfo` cuyos dos primeros campos imitan deliberadamente a
`std::type_info`; la de Objective-C++ de GNUstep direcciona una subclase real de
`std::type_info`; y la del runtime GNU no es siquiera un puntero, sino la propia
cadena con el nombre de la clase. Aplicar la convención de un runtime a la tabla
de otro no falla: informa de un nombre de clase leído desde la mitad de otra
cosa. Por eso el runtime se establece a partir de la personality del frame antes
de leer ninguna ranura.

La misma suite fija dos distinciones fáciles de colapsar y erróneas al hacerlo.
`@catch(id)` y `@catch(...)` son manejadores distintos —el primero acepta
cualquier objeto Objective-C y deja que una excepción ajena siga de largo— y
cada runtime los escribe de otra forma; un decodificador que informe de ambos
como catch-all pone un manejador sobre excepciones que de hecho habrían pasado
de largo. Y una tabla de sitios de llamada setjmp/longjmp indexa sitios de
llamada en vez de direcciones: un lector que no reconozca alguna de las
personalities SJLJ no falla, sino que inventa rangos protegidos y landing pads
que el programa nunca nombró.

Reconocer esa forma no es lo mismo que rechazarla. Una entrada SJLJ es un par
de valores ULEB128 — un selector de despacho y un desplazamiento de acción — y
ese desplazamiento significa allí exactamente lo que significa en la forma
direccionada, de modo que la cadena de acciones, los tipos capturados y las
especificaciones de excepción se leen todos de una tabla que no nombra código
alguno. Lo único que queda desconocido es la región que guarda cada entrada,
porque quien la enuncia son las escrituras que la propia función hace en su
ranura de call-site, y no nada de la tabla. La suite también fija el byte del
que aquí no hay que fiarse: GCC escribe `DW_EH_PE_uleb128` como codificación de
call-site y LLVM escribe `DW_EH_PE_udata4`, ambos emiten después ULEB128 de
todos modos, y ninguna personality lo lee jamás; un decodificador tampoco debe
hacerlo.

La identidad de la personality queda fijada junto a esto, porque es la que
decide cómo se lee cada tabla de arriba. GNAT nombra su rutina de las tres
maneras en que GCC nombra la de cada frontend — `_v0`, `_sj0`, `_seh0` — y en
Windows registra un símbolo mientras reenvía a otro, así que las cuatro grafías
tienen que acabar en Ada. D es la imagen inversa: tres compiladores, tres
nombres para una sola rutina, un único juego de tablas detrás.

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

### Backends diferenciales EVM

Las pruebas del intérprete proporcionan un oracle determinista de 256 bits. La
suite emitter compila y ejecuta LLVM, baja C23 con Clang al mismo host harness y,
si están `solc`, `anvil`, `cast` y `jq`, despliega Solidity generado localmente.
Compara status, storage y contadores de trace. Un corpus raw separado ejecuta
ALU pre-Fusaka, copias calldata/memory, `MCOPY` solapado, Keccak y return data en
la EVM nativa de Anvil.

`NeverDEVMOpcodeTests` impone la arquitectura metadata: 150 opcodes hacen
roundtrip entre encoding y valor tipado; se prueban límites de familias, aliases
hardfork y máximos stack/host derivados.

### Backends diferenciales de Solana SBF

Las pruebas de metadatos SBF validan cada función de versión, los límites de colisión de opcodes, los hash syscall Murmur3, las reubicaciones y las constantes de machine ELF, registro y dirección VM. Las fixtures del loader generan, sin binarios vendorizados, tanto diseños legacy con secciones v0-v2 como diseños estrictos v3/v4 sin secciones y basados en program headers.

`NeverDSBFSemanticTests` ejecuta directamente bytes de instrucciones verificados y no consume MedIR, de modo que cambiar o corromper el IR normalizado no puede hacer que el oracle de origen coincida accidentalmente con un backend. Cubre la semántica v2 no monótona, memoria, syscalls, frames de llamadas internas, faults, traces y límites de recursos. Los módulos LLVM se verifican; el C generado se compila tratando los warnings como errores y Rust con `-D warnings`. Las pruebas de la API pública recorren todas las etapas IR, desensamblado, CFG, metadatos, LLVM, C y Rust desde un ELF SBF estricto generado.

## Objetivos de una sola orden

Los objetivos personalizados compilan sus dependencias y ejecutan CTest con
paralelismo derivado de las CPU del host:

| Objetivo CMake | Selección |
|----------------|-----------|
| `check-neverd` | Todas las pruebas registradas |
| `check-neverd-semantic` | Solo `NeverDSemanticTests` |
| `check-neverd-sbf` | Todos los targets/casos `NeverDSBF*Tests` |
| `check-neverd-patch-full` | Solo `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | Solo `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | Solo `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | Solo `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
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

# Todos los targets/casos específicos de EVM
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMAnalyzerTests NeverDEVMSemanticTests NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Todos los targets/casos específicos de Solana SBF
cmake --build build-release --target \
  NeverDSBFMetadataTests NeverDSBFLoaderTests NeverDSBFAnalyzerTests \
  NeverDSBFSemanticTests NeverDSBFLLVMEmitterTests NeverDSBFEmitterTests \
  NeverDSBFIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'SBF' --output-on-failure --parallel 4
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
| Loader, opcode, IR o backend EVM | Menor target propietario `NeverDEVM*Tests` | Todos los targets EVM y compilación del C/Solidity generado |
| Loader, ISA, IR o backend SBF | Menor target propietario `NeverDSBF*Tests` | Todos los targets SBF y compilación del C/Rust generado |
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

## Perfil actual de conformidad y sanitizers de Solana SBF

Esta lista actual sustituye la lista SBF abreviada anterior. La suite source
differential requiere `rustc` además de clang; omitir el compilador significa
coverage ausente. El agregado completo incluye `NeverDSBFProgramImageTests`,
`NeverDSBFMalformedCorpusTests`, `NeverDSBFISAConformanceTests`,
`NeverDSBFUpstreamConformanceTests`, `NeverDSBFLLVMDifferentialTests` y
`NeverDSBFSourceDifferentialTests`, junto con los targets de metadata, loader,
analyzer, semantic, emitter e integration. El perfil integrado supera 145/145
casos en 14 binarios.

El perfil sanitizer se construye por separado en `build-sbf-asan-ubsan`.
Supera 141/141 casos core en 13 binarios sin informes ASan o UBSan; integration
permanece en la build LLVM integrada porque al paquete prebuilt le falta el
header fork-only requerido.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```
