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
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Catálogo de sumideros, precedencia de identidad, prefiltro de argumentos, caza de desbordamiento de copia, auditoría de vida del montón y matriz obligatoria de seis celdas PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/lift` | `NeverDLiftTests` | Formas LowIR decoder/lifter, etapas IR, loaders, relocations, fixtures de formato, descompilación y flujos patch representativos |
| La mayoría de `unittests/semantic` | `NeverDSemanticTests` | Semántica diferencial de instrucciones, ABI, control de flujo, expresiones C y lift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadatos hardfork, normalización, ambigüedad ABI/firma, CFG/SSA/recuperación, límites decoder exhaustivos e inputs hostiles, hechos proxy/call, semántica del intérprete, diferenciales LLVM/C/Solidity y API pública |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | Metadatos v0-v4 y diseños ELF, comportamiento estricto de verifier/loader, 23 artefactos ELF fijados, oracle oficial independiente, disponibilidad exhaustiva de opcodes, entradas hostiles, CFG/recuperación y diferencias ejecutadas de LLVM/C/Rust |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Equivalencia de reescritura/ofuscación entre cuatro ISA y tres formatos objeto |
| Archivos de transformación enfocados en `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Sondas rápidas de reenlazar separadas del gran binario semántico |
| `unittests/corpus` (submódulo) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests` | Metadatos de excepciones y de runtime leídos de 317 binarios reales fijados, cada uno declarado en un manifiesto con los mínimos que su recuperación debe superar |

Las fuentes de registro son
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) y
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) y
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt) y
[`unittests/safety/CMakeLists.txt`](../unittests/safety/CMakeLists.txt).

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
`check-neverd-go-eh-corpus`, `check-neverd-cxx-itanium-eh-corpus` y
`check-neverd-objc-eh-corpus` ejecutan una cada uno. Los tres hosts de CI
configuran con la opción y corren las cinco líneas: los bytes son idénticos en
todas partes, pero lo que los lee no lo es, y una pasada del corpus en un host
no prueba nada sobre los otros dos. `scripts/audit_ci_test_inventory.py` rechaza
un inventario al que le falte cualquiera de las cinco etiquetas, porque una
compilación que dejó de leer el corpus en silencio es una regresión que ningún
test puede atrapar: el test es justamente lo que desapareció.

La auditoría live de opcodes EVM se ejecuta así:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

Tanto en local como en CI, la ruta estándar fuerza
`git fetch --depth=1 --force` sobre la URL oficial
`https://github.com/ethereum/go-ethereum.git` y sólo prueba el SHA exacto
recién obtenido del `HEAD` remoto de la rama por defecto, en un worktree
detached. Cada ejecución usa un repositorio bare privado, temporal y de nombre
impredecible. Conserva el authority ref del fetch y su SHA exacto durante
la vida del worktree detached, y después destruye ambos. No existe repositorio
Git persistente ni caché compartida. `local_docs`, un checkout existente y un
submodule no son rutas de auditoría, pues un pin de submodule quedaría obsoleto
justo cuando toca detectar drift live.

Cada comando Git elimina primero todos los `GIT_*` heredados, incluidos
`GIT_CONFIG_*`, y después instala sólo valores auditados. `GIT_CONFIG_NOSYSTEM`
y `GIT_CONFIG_GLOBAL` desactivan la configuración system/global;
`GIT_ATTR_NOSYSTEM` y `core.attributesFile` por comando desactivan los atributos
system/global, y `core.hooksPath` desactiva hooks. Configuración inesperada del repositorio privado, grafts,
`objects/info/alternates` o `refs/replace` hacen fallar la validación;
`GIT_NO_REPLACE_OBJECTS` desactiva replacement lookup.

La sonda refleja todos los bool exportados de `params.Rules`, llama a
`LookupInstructionSet(params.Rules)` y recorre los 256 slots.
`EVMUpstreamOpcodePolicy.def` posee aliases y exclusiones tipadas
históricas/EOF sin programar; `EVMUpstreamSemanticsPolicy.def` posee el
inventario Rules cerrado, mappings de forks, excepciones base-stack y familias
dynamic-immediate.

CI sólo ejecuta esta auditoría live en push a `dev`, pull requests, activación
manual y el horario diario. La sonda Go llama a la API pública
`LookupInstructionSet(params.Rules)` para cada fork mapeado.
La CLI pública sólo expone `--manifest-output`; el manifest cerrado usa
`schema 3` y no permite elegir fuente, ref, checkout ni toolchain.
`EVMUpstreamOpcodePolicy.def` mantiene alias y exclusiones históricas/EOF sin programar
revisadas; el ortogonal `EVMUpstreamSemanticsPolicy.def` mantiene reglas de fork
y excepciones de semántica de pila. El manifest cerrado comprueba revisión
exacta, activación, byte/name, `base_min_stack` y `net_stack_delta`, y rechaza
campos, forks, nombres o bytes desconocidos o duplicados. La asignación se decide
sólo con `operation.undefined`; `HasCost` sólo sirve de comprobación cruzada del
coste porque también vale false para operaciones definidas de coste cero. Cada slot
`defined && !HasCost` debe coincidir exactamente con
`EVM_GETH_ACTIVE_WITHOUT_COST` desde su fork declarado. Un slot undefined con
coste, uno defined sin revisar o la pérdida del marcador fallan de forma cerrada.
Declaraciones ausentes, fuera de rango o no consumidas sintácticamente también
fallan: cada `.def parser` rechaza una política `partial`. Un fallo CI publica
revisión, manifest y log como artifact. Parser y diagnósticos tienen cobertura
unitaria Python independiente:

`EVMUpstreamSemanticsPolicy.def` asigna cada campo booleano exportado de
`params.Rules` a un único `EVM_GETH_RULE_FIELD`: `MappedForkSelector`,
`NoOpcodeAllocation` o `ExcludedSelectorExpectedError`. El probe activa cada
campo aislado mediante `LookupInstructionSet`: las dos primeras categorías
exigen nil error, la tercera error, y cada fingerprint opcode/stack completo de
256 slots debe ser `ExpectedFork`. `IsEIP155`, `IsEIP2929`, `IsEIP4762` e
`IsPetersburg` son ahora campos sin asignación con fingerprint Frontier;
`IsUBT` debe fallar y dar Cancun.

`EVMUpstreamSemanticsPolicy.def` declara las familias dinámicas EIP-8024, las
clases de operación y los deltas de pila válidos;
`EVMEIP8024Immediates.def` posee por separado el decode de immediates y clasifica
los 256 bytes single/pair. Con `go -overlay`, la auditoría obtiene los handlers
privados reales `operation.execute` y recorre tabla por tabla las
`canonical fork jump tables` y las `mainnet active/scheduled jump tables`.
Registra explícitamente una familia `inactive` y rechaza una `partial`. Cada
tabla activa prueba `DUPN`, `SWAPN` y `EXCHANGE` con todos los immediates (`3x256`) y
los `3 missing-operand cases`, contrastando aceptación, PC, mutación, underflow
y operando ausente con las mismas fuentes declarativas.

`EVM_HARDFORK_LATEST` tiene un solo target canónico. El cerrado
`EVMUpstreamForkAliases.def` mapea Prague→Pectra, Osaka y BPO1–BPO5→Fusaka, y
Paris/Shanghai/Cancun/Amsterdam/Bogota a sí mismos; nombres desconocidos fallan
cerrado. Un `audit_unix_time` registrado dirige
`MainnetChainConfig.LatestFork(time)` (debe igualar NeverD latest) y el chequeo
alias/probe de `LatestFork(max uint64)`; ambos instruction sets se comparan
completos. El manifest fija `authority=official-fresh-fetch`, URL oficial,
`HEAD` solicitado y SHA. El probe usa `GOTOOLCHAIN=local`.

La sonda Go y el controlador Python aplican
`input/collection/string hard limits`; entradas, colecciones o cadenas
sobredimensionadas fallan de forma cerrada. Para `bounded diagnostic output`,
una visualización demasiado larga incluye el `digest` completo y un
`explicit truncated marker`. Cada proceso hijo tiene salida y plazo acotados;
al excederlos se mata todo el `process group`/process tree y se drenan sus pipes.

El recibo schema 3 actual registra `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, revisión
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, `Go 1.24.0` local,
`stack_limit=1024` y `diagnostics=[]`. Cubre `21 fork tables` y
`20 Rules probes` con `15 mapped/4 no-op/1 expected-error`. Ambos registros
`mainnet active/scheduled` informan `upstream BPO2`, mapeado de forma cerrada a
`NeverD Fusaka`. De `23 table targets`, sólo `Amsterdam/Bogota` están activos:
`1536 candidate executions` y `6 missing-operand cases`. Los
`three handler symbols` coinciden en los dos targets activos. El audit Python
pasó `67/67` y `C++ Opcode 10/10`. El run real de macOS tuvo éxito bajo
`sandbox-exec`, con el `go run` final sin red; Linux impone `bubblewrap`.

Todas las etapas Go —`go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` y `go run`— pasan por el sandbox de filesystem
`capability-root`. Lee sólo el probe privado, geth fresco, el `resolved GOROOT`
validado y las raíces exactas de runtime del sistema, y escribe sólo en raíces
aisladas de entorno. La red se concede sólo a las etapas de dependencias que la
necesitan; el run final está offline. Los tests exigen denegar los sentinels de
`host HOME/workspace` y que su contenido no aparezca en output. Linux prueba la
misma política `bubblewrap` sin `/` broad bind.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

Los once targets EVM actualmente registrados por CMake son:

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` agota todas las entradas de dos bytes por cada
fork que cambia el decoder, compara el decode completo y los límites `JUMPDEST`
exactos y pasa inputs hostiles deterministas de longitud acotada por todos los forks.

Para cambios de control de flujo EVM, ejecute primero el contrato de punto fijo
y dominio de alturas:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Estos casos cubren retornos entre bloques, uniones finitas con varios destinos,
convergencia, orden determinista, lanes de pila completa sensibles al camino,
correlación preservada, saltos desconocidos, destinos exactamente inválidos,
presupuestos fail-loud y fallos de pila. `MayReachable` conserva sólo un candidato
de CFG y no produce hechos ciertos. Ejecute después los once targets EVM y la
auditoría live upstream.

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

### Fixtures de seguridad de memoria

`unittests/safety/fixtures/binaries` contiene imágenes PE, ELF y Mach-O
versionadas para x86-64 y AArch64, junto con el PDB o el dSYM que aporta cada
formato y un MAP del enlazador por cada imagen. El MAP es lo único que sigue
entregando una compilación despojada, así que cada celda se analiza también
nombrando el MAP de forma explícita, lo que fija qué puede afirmar un hallazgo
cuando ya no quedan tipos ni líneas de código fuente.
`NeverDSafetyIntegrationTests` ejecuta las seis celdas en cada host; la
configuración falla si falta cualquier imagen o acompañante requerido, y la
suite no tiene ninguna vía de omisión ligada a la cadena de herramientas del
host.

Los binarios equivalentes provienen de un único archivo fuente. Reconstruya la
fixture smoke nativa del host con `make`, o regenere la matriz completa
versionada con:

```bash
make -C unittests/safety/fixtures matrix
```

La receta de la matriz necesita los destinos cruzados de Linux y Windows de
Clang, las herramientas COFF de LLD, ambas arquitecturas Darwin y `dsymutil`.
Sus rutas de depuración se reasignan y el registro de la línea de comandos de
CodeView queda desactivado, de modo que los acompañantes versionados no capturen
la ruta absoluta del espacio de trabajo de una persona desarrolladora.

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

Las pruebas Low/Med preservan execution lanes whole-stack sensibles al path y la
identidad de lane de los phi; agotar un presupuesto, incluido
`MaxAbstractInstructionTransfers`, es un error duro. Strict sólo rechaza un
opcode desconocido o inactivo en una lane probada `Reachable`; `MayReachable` no
produce hechos definitivos. HighIR restringe selector, receive y fallback a la
lane raíz y a terminales exitosos. Un selector compartido no es evidencia
independiente de estándar: sólo una `KnownFunctionVariantInfo` del estándar y
una forma de retorno exacta acordada por todos los terminales exitosos permiten
elegir variante y lista de retornos.

El intérprete hace preflight tipado de pila antes de cualquier efecto específico
del opcode. `EVMForkSemantics.def` define el byte `0x44` como `DIFFICULTY` antes
de Paris y `PREVRANDAO` desde Paris. `REVERT`, faults, step limit y agotamiento
de recursos revierten el estado transaccional. Un fallo de asignación es
`ExecutionFaultKind::ResourceExhausted`; si no puede crearse el snapshot de
entrada, `HasPersistentStateSnapshot` es false y el resultado no puede commit.

### Regresiones de límites públicos y presupuestos EVM

Los tests de API pública alteran por separado
`Code`/`Fork`/`Instructions`/`JumpDestinations` canónicos y cada tabla, rango,
ID, lane y referencia de arista de LowIR. `execute` debe devolver `llvm::Error`
antes del lookup de instrucciones, y `lowerToMedIR` debe rechazar todo LowIR
malformado o fuera de presupuesto antes de construir índices o asignar output
proporcional al input. Para `lowerToMedIR`, los tests exigen validar options,
recursos y estructura antes del `canonical decode replay` campo a campo y antes
de `lowerCanonicalLowToMedIR`. El recovery HighIR público replay-comprueba
LowIR/MedIR externos; sólo `analyze` usa `lowerCanonicalLowToMedIR` y
`recoverCanonicalHighIR` sobre su IR canónico sin replay recursivo o duplicado,
pero con todos los HighIR option/resource budgets. El intérprete prueba después el borde exacto y +1 para
todos los límites de `EVMInterpreterLimits.def`: `MaxSteps` mantiene su
`StepLimit`; agotar `MaxMemoryBytes`, `MaxTraceEntries`, `MaxLogEntries`, el
agregado `MaxLogDataBytes` o `MaxPersistentStateEntries` en runtime devuelve
`ResourceExhausted` y revierte los efectos transaccionales. Un agregado inicial
`MaxHostReturnDataBytes` o estado persistente demasiado grande es error de API.
También lo son `MaxCalldataBytes`, el agregado `MaxHostEnvironmentEntries` sobre
`BlockHashes`, `Balances`, `CodeHashes`, `ExternalCode`, `BlobHashes` y el
agregado `MaxExternalCodeBytes`. El `const execute preflight` los rechaza antes
de copiar environment, snapshot o result. Se cubren views `ArrayRef` de return data y lookup `lower_bound` sobre
tabla ordenada, sin copia de buffer ni mapa de PC.

Tests LowIR separados cubren los límites agregados de diagnóstico
`MaxLowDiagnostics` y `MaxLowDiagnosticBytes`: decode lineal y construcción CFG
precargan número/bytes finales exactos y se rechaza cero.
Los tests de seguridad HighIR cubren el dominio ordenado por lane
`Any/Exact/Excluded`, match/exclusión de igualdad, match en arista false y
mismatch en arista true de `XOR(selector, constant)` crudo, refinamiento de word
cero/calldata size/call value y condiciones unknown fail-closed. Sus pruebas de
borde exacto y -1 cubren, desde `EVMAnalysisLimits.def`,
`MaxHighDispatchCandidates`, el agregado `MaxHighRecoveredArguments`,
`MaxHighDiagnostics`, `MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`,
`MaxHighMemoryTransferCells` y `MaxHighMemoryValueVisits`. Todo diagnóstico
emitido, incluido el fijo de malformación, debe cargar número y bytes finales
antes de asignar memoria.
Los budgets de diagnóstico LowIR/HighIR se prueban por separado; la región CFG
raíz predeterminada debe cargar `MaxHighRegionBlockReferences` antes de reserve o
copiar PC de bloques.
Las regresiones de function scope cubren back-jumps `EQ` y `raw XOR` al
dispatcher compartido. Verifican que otra función no contamine `arguments`,
`mutability`, `return shape` ni `region`, y que bodies compartidos y tail calls
sigan siendo alcanzables.
Los resultados externos CALL/CREATE se prueban como outcomes host no
deterministas por ambas aristas CFG precisas, preservando la recuperación del
fallback ERC-1167. Una condición selector ilegible sigue Unknown y no puede
inventar hechos fallback o function.

Los tests CFG derivan `InvalidJumpDestination` de `EVMLowFaultKinds.def` para un
`end-of-code JUMPI`: true definitivo con destino inválido carece de cola exitosa
y es fallo definitivo; false definitivo tiene éxito; unknown conserva la posible
ruta false exitosa sin marcar toda la lane como fallo definitivo.

Los tests ABI aplican en el límite exacto y +1 los bordes gramaticales de
`EVMABIParserLimits.def` y los bordes de cardinalidad/texto de tabla pública de
`EVMABITableLimits.def`. También rechazan enums kind/standard/evidence inválidos,
metadata discordante, firmas/returns no canónicos, selectors compartidos
marcados erróneamente independent, variantes colgantes o duplicadas y un
event-topic `APInt` con ancho distinto de word antes del lookup indexado de
selector o el lookup ordenado de topic.

`NeverDEVMOpcodeTests` impone la arquitectura metadata: cada opcode asignado hace
roundtrip entre encoding y valor tipado; se prueban límites de familias, aliases
hardfork y máximos stack/host derivados.

### Backends diferenciales de Solana SBF

Las pruebas de metadatos SBF validan cada función de versión, los límites de colisión de opcodes, los hash syscall Murmur3, las reubicaciones y las constantes de machine ELF, registro y dirección VM. Las fixtures del loader generan, sin binarios vendorizados, tanto diseños legacy con secciones v0-v2 como diseños estrictos v3/v4 sin secciones y basados en program headers.

`NeverDSBFISAConformanceTests` comprueba cada byte encoding de cada versión
v0-v4 contra un manifest tipado auditado de forma independiente.
`NeverDSBFExternalOracleTests` compara después las decisiones de activación y
de límites con un proceso oficial de Anza construido por separado.
`NeverDSBFUpstreamConformanceTests` asigna un resultado explícito a los 23 ELF
en la revisión fijada de Anza.

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
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Todos los targets/casos específicos de Solana SBF
cmake --build build-release --target check-neverd-sbf --parallel 4
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
| Auditoría de vida del montón o caza de desbordamiento de copia | `NeverDSafetyTests` | Las seis celdas de `NeverDSafetyIntegrationTests` |
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
`scripts/audit_ci_test_inventory.py`. `NeverDSafetyTests` y
`NeverDSafetyIntegrationTests` son obligatorios en cada host de la matriz; cada
ejecución lee las mismas fixtures versionadas PE, ELF y Mach-O para x86-64 y
AArch64. Como ningún shard de la matriz representa todas las suites costosas,
un `check-neverd` local sigue siendo la señal previa a fusión completa más clara
si la máquina dispone de todas las herramientas cruzadas necesarias.

## Perfil actual de conformidad y sanitizers de Solana SBF

Esta lista actual sustituye la lista SBF abreviada anterior. La suite source
differential requiere `rustc` además de clang; omitir el compilador significa
coverage ausente. El agregado completo incluye `NeverDSBFProgramImageTests`,
`NeverDSBFMalformedCorpusTests`, `NeverDSBFISAConformanceTests`,
`NeverDSBFUpstreamConformanceTests`, `NeverDSBFLLVMDifferentialTests` y
`NeverDSBFSourceDifferentialTests`, junto con los targets de metadata, loader,
analyzer, semantic, emitter e integration. El perfil integrado registra targets
y resultados nombrados, no un total que cambia con frecuencia.

El perfil sanitizer se construye por separado en `build-sbf-asan-ubsan`. Los
targets enfocados se ejecutan fail-fast sin informes ASan o UBSan; integration
permanece en la build LLVM integrada porque al paquete prebuilt le falta el
header fork-only requerido.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```

### Instantánea de evidencia SBF fijada (2026-08-24)

La gate fija Anza `sbpf` en
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave en
`ef210d67f2fabeee1730498188fa78854260c679` y Solana SDK en
`122f32e571ce39face4beffaccea733e37c207fd`. El manifest ELF oficial pasa
23/23; `NeverDSBFExternalOracleTests` contrasta 1,411 casos opcode/boundary
mediante `SBFOfficialOracleProtocol.def`, `SBFOfficialVerifierCases.def` y
`SBFOfficialExecutionConstants.def`.
`SBFOfficialELFMutations.def` es el contrato tabulado de ELF malformado; no se
fija su total cambiante.
Por separado, el `41-case strict ELF differential` ejecuta toda la matriz
strict-v3 mediante `verify-elf-batch` oficial y NeverD; sus 41 casos no forman
parte del total 1,411.

La matriz oficial adicional de ejecución se mantiene separada: exactamente 508
casos activos `(Version,Opcode)` más 58 casos de límite suman 566 casos de
ejecución exacta. No sustituye ni se contabiliza dentro de las 1,411 probes del
verifier ni del `41-case strict ELF differential`.
`NeverDSBFAgaveConformanceTests` autentica Firedancer test-vectors
`68bb4af40235562e8852fa23d5727e49c2a0b862` y contrasta los 1,955 `sol_compat_elf_loader_v1` fixtures del
loader (1,399 aceptados, 556 rechazados). Para cada ELF aceptado contrasta
`entry_pc`, `text_off`, `text_cnt`, `rodata_hash` y `calldests_hash`. Esta gate no ejecuta el verifier de
instrucciones posterior.
Linux Release CI usa `--print-pinned-revision`,
`--print-test-vectors-revision` y `--print-toolchain`, y exporta
`NEVERD_SBPF_ORACLE` y `NEVERD_AGAVE_CONFORMANCE_ROOT`, por lo que ambas gates
externas son obligatorias. En local, sin env explícito de oracle/corpus, los
casos se descubren pero pueden omitirse.

`SBF_RUNTIME_VERSION` hace que `RuntimeVersionPolicy::ChainProfile` dependa del
cluster/slot histórico: las feature accounts oficiales avanzan el ISA máximo
de V0 a V1, V2 y V3; hoy sigue en V3. v4 explícito usa
`RuntimeVersionPolicy::UpstreamToolchain` para análisis
offline. El límite actual de 10 MiB es `10'485'760` bytes exactos; 65,536 sólo
es provenance/test histórico. `SBFFaultCodes.def` estabiliza los valores de
execution fault y `SBFSourceStatuses.def` conserva aparte el ABI del source.

Fixtures de escala 10,000 protegen worklist, function ownership y multi-latch
sin fijar tiempo de máquina. Las filas cluster/account/slot permiten un
`RPC activation audit` mientras las pruebas normales siguen deterministic y
offline.
