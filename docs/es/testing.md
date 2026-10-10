**Idiomas**: [English](../testing.md) | [简体中文](../zh-CN/testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](../ja/testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](../de/testing.md) | [Español](testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← Índice de documentación](README.md)

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

Consulte [CONTRIBUTING.md](CONTRIBUTING.md) para clonación, perfiles de
compilación y LLVM precompilado en macOS.

## Comprobaciones de recuperación de intérpretes

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests \
  NeverDDevirtualizationSourceTests --parallel 4
ctest --test-dir build-release -L '^NeverD(InterpreterSpecialization|DevirtualizationSource)Tests$' \
  --output-on-failure
```

```sh
cmake --build build-release --target NeverDLowIRUndefinedIndependenceTests \
  NeverDOriginalBinaryUndefinedIndependenceTests \
  NeverDX86UndefinedEffectsTests NeverDX86CarryArithmeticFlagTests \
  NeverDX86LogicIdentityTests NeverDX86NoIndexAddressTests --parallel 4
build-release/bin/NeverDLowIRUndefinedIndependenceTests
build-release/bin/NeverDOriginalBinaryUndefinedIndependenceTests \
  --gtest_filter='OriginalBinaryUndefinedIndependence.*'
build-release/bin/NeverDX86UndefinedEffectsTests
build-release/bin/NeverDX86CarryArithmeticFlagTests
build-release/bin/NeverDX86LogicIdentityTests
build-release/bin/NeverDX86NoIndexAddressTests
```

Las pruebas de API cubren valores predeterminados v1/v2/v3, presupuestos explícitos, estructuras truncadas, todos los campos reserved y colas futuras. Las pruebas CLI comprueban agotamiento y recuperación correcta con ambas ABI y motores, rechazan límites decimales inválidos y exigen `--devirtualize`. El agotamiento no debe publicar código ni grafos residuales parciales.

Las pruebas v4 fijan tamaños y relleno de prefijos, rechazan estructuras truncadas y banderas desconocidas, conservan formatos anteriores/futuros y mantienen límites en C sin informe. Ejemplos CLI independientes necesitan encadenamiento para las correlaciones y límites para una comparación de pila sin signo; ambos backends C ejecutan O0/O2 con trampas de comportamiento indefinido. Desactivar el descubrimiento debe cambiar un resultado que dependa de él. El análisis cubre cero, extremos enteros, desbordamientos, rangos incorrectos y requisitos ausentes. Python verifica disposición, banderas, firmas y propiedad de informes de fallo.

`NeverDByteMemoryForwardingTests` cubre últimas escrituras solapadas, ambos órdenes de bytes, anchos múltiplos de ocho hasta i128, valores definidos, instantáneas undef/poison correlacionadas y sobrescrituras parciales. Los casos negativos conservan las lecturas ante alias desconocidos, conversiones de espacio de direcciones, llamadas, accesos ordenados, cambios de vida útil, bytes ausentes, offsets dinámicos o inválidos, ramas y bucles. También prueba PHI con muchas entradas, presupuestos nulos, exactos o agotados y rechazo predeterminado de instantáneas. El LLVM original y reescrito se ejecuta a O0/O2 contra una referencia aritmética independiente; las suites MBA, LLVMC y de fuentes de intérpretes protegen la compatibilidad.

Las regresiones adicionales conservan intrinsics de operaciones puras sobre bits en ambos modos de memoria y órdenes de bytes. Las llamadas ordinarias, operand bundles, convergent, cambios de vida útil, efectos de memoria y traps siguen siendo barreras. El código original y transformado se ejecuta con O0/O2 y traps de comportamiento indefinido frente a una referencia independiente que verifica la dirección numérica del resultado y cada byte del búfer.

Las regresiones de vivacidad por byte cubren lecturas disjuntas, escrituras parcialmente observadas y cobertura mediante varias escrituras. Las direcciones condicionadas incluyen AND/OR/XOR, 32/64 bits, ambos órdenes de bytes, raíces incorrectas, máscaras incompletas, uniones que evitan la comprobación y todos los límites de presupuesto. Los oráculos independientes O0/O2 comprueban ambas ramas, los dieciséis residuos de dirección y cada byte del búfer.

Las regresiones de relaciones de dirección cubren PHI/select de enteros y punteros, ambos órdenes de bytes y anchos de puntero, retornos de bucle estables y variables, undef/freeze, ciclos sin anclaje, presupuestos adyacentes e invalidación por cambios exclusivos de dirección. Los bucles O0/O2 comparan cada retorno y cada byte del búfer con un oráculo independiente por iteración, incluida una dirección móvil que no debe tratarse como constante.

Los casos numéricos verifican lecturas completas y contenidas de un escritor, undef/poison sin instantáneas nuevas, ambos órdenes de bytes, 32/64 bits, offsets negativos modulares, solapamientos parciales, alias entre raíces y allocas, excepciones, bucles, límites adyacentes del presupuesto e invalidación al eliminar solo stores. O0/O2 comparan original y transformación con un oráculo independiente para el retorno y cada byte del búfer con alias.

`NeverDMedMutableSourceTests` y `NeverDLLVMCValueTests` ejecutan a O0/O2 bucles independientes, bloques reordenados, aristas de retorno a la entrada, aritmética de pila en ejecución, lecturas anteriores, uniones, alias parciales, valores booleanos y conteos de bits incluido el cero. Los casos negativos rechazan entradas mal formadas, destinos truncados, portadores ambiguos y presupuestos agotados antes de emitir. Un caso CLI sobre el límite SSA exige LLVMC ejecutable y rechazo explícito de HighC. Las actualizaciones repetidas y las cadenas de expresiones almacenadas entre bloques también comprueban el tamaño y la ejecución del C generado.

Las regresiones adicionales limitan las lecturas y escrituras privadas antes de la promoción LLVM y el tamaño del C. Ejecutan a O0/O2 cadenas aritméticas mixtas largas, bloques SSA reordenados, escrituras de memoria solapadas y retornos cero. Los casos clave también pasan por el pipeline real de optimización LLVM; se verifica reutilizar el emisor tras rechazar una generación de módulo.

Las regresiones de condiciones compuestas ejecutan a O0/O2 conjunciones y disyunciones con igualdad a constantes no nulas, comparaciones sin signo, comparaciones con signo en ambos órdenes, booleanos ampliados y todas las combinaciones de negación. El C debe conservar toda la tabla de verdad sin desreferenciar un operando ausente de comparación con cero. El almacenamiento de direcciones enteras cubre valores de 32/64/128 bits alineados y no alineados. Los arreglos de bytes conservan la alineación explícita y los accesos exactos a la base y parciales, sin asignaciones escalares al arreglo ni alias mediante tipos incompatibles.

`NeverDLowIRRefinementTests` cubre grafos realmente recuperados, bucles finitos de distinta estructura y cero iteraciones, productores dinámicos, elecciones condicionales, vistas de entrada solapadas, copias y derrames correlacionados, evidencia de lectura inmutable en ambos lados, flags de sistema y preservación del retorno. Candidatos erróneos, escrituras adicionales, caminos incompletos o infinitos, evidencia obsoleta, colisiones temporales y presupuestos compartidos agotados deben rechazar el certificado. Las pruebas de independencia siguen rechazando valores arbitrarios observables.

Los casos `CompleteModel`, `CompletedTargetFacts`, `ConditionalImplication` y `PartitionedCoverage` de `NeverDLowIRRefinementTests` usan oráculos exhaustivos independientes para dominios pequeños y comprueban entradas malformadas, cachés obsoletas, enumeraciones incompletas y presupuestos exactos/insuficientes. Las pruebas reales de LowIR y binarios verifican productos condicionales y todas las ramas terminales originales/recuperadas con límites fijos de puertas, rechazando observaciones finales alteradas, dominios ajenos y destinos ausentes. `FiniteValues` distingue fallos de codificación de rechazos por búsqueda, cantidad de valores y presupuesto global.

En el mismo objetivo, `LowIRLoopRefinement.*` y `BinaryLowIRLoopRefinement.*` prueban contadores arbitrarios de 64 bits, rangos lexicográficos anidados, residuos nativos reales, prefijos de entrada, vistas superpuestas y derrames correlacionados. Los controles negativos rechazan cuerpos incorrectos, dominios de entrada reducidos, rangos que no disminuyen, desbordamiento modular sin signo, escrituras previas olvidadas, cortes ausentes, plantillas inválidas y presupuestos compartidos agotados. El éxito de una rama finita no autoriza una inducción incompleta.

`LowIRLoopInference.*` y `BinaryLowIRLoopInference.*` usan contadores, guardados en pila, retornos anticipados, llamadas nativas y banderas empaquetadas escritos independientemente. Cubren ampliación aritmética estrecha y banderas semánticamente iguales con expresiones distintas. Grafos malformados, orígenes ausentes o falsificados, bucles infinitos o con desbordamiento modular y presupuestos agotados no deben producir certificados.

Las regresiones de prefijos cero cubren agrupaciones, anchuras inusuales y todos los pares de bytes, conservando bits desconocidos y no nulos. Bucles de marco independientes comprueban escrituras separadas del valor bajo y los ceros altos en ambos órdenes de bytes, anchuras estrechas, aritmética y relleno incorrectos, límites exactos/insuficientes y el presupuesto separado de consultas de la prueba completa.

Las regresiones con cabecera y bloque de retorno compartidos cubren contadores de 32 bits extendidos con ceros y de 64 bits completos, rangos escalares de paso no unitario, resultados incorrectos, caminos sin progreso o con desbordamiento modular, y presupuestos exactos o agotados entre la búsqueda escalar y por tuplas. `LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`. Otras regresiones de incremento y reinicio exigen convergencia sin desplegar un bit del contador por ronda y rechazan la ausencia de progreso y el desbordamiento modular sin signo. Las regresiones de planificación cubren acumuladores de paso no unitario, ruido de contadores unitarios que pueden desbordarse junto a un rango escalar válido de paso no unitario, y tres contadores cuya tupla válida aparece después de la ventana inicial. Presupuestos exactos y con un intento menos comprueban la continuación determinista sin propuestas repetidas.

`LowIRLoopPlanPairing.*`, en el mismo destino, comprueba registros renombrados, cuerpos aritméticos distintos, instantáneas de prefijo de cada lado, predicados conservados, entradas de memoria compartidas, cortes anidados y presupuestos de prueba independientes. Relaciones ausentes, escrituras incorrectas, temporales mal vinculados, emparejamientos incompletos o límites de metadatos agotados no deben producir certificados.

`LowIRLoopAlignment.*` comprueba bucles ordinarios y rotados con contadores en el marco, escritos de forma independiente: los dos planes predeterminados se prueban por separado, su primer emparejamiento falla y otro corte candidato demuestra la relación. Las regresiones cubren permutaciones de varios cortes, resultados y escrituras incorrectos, registros originales ausentes u obsoletos, witnesses explícitos de valores indefinidos, contadores no decrecientes o con desbordamiento, grafos malformados, consultas acumuladas tras fallos, presupuestos totales exactos y límites agotados. Ningún rechazo debe contener un certificado. Los casos adicionales cubren fases separadas de reinicio y progreso, guardas de salida equivalentes reubicadas que requieren emparejar familias, caché sin repetir inferencia y exceso conjunto de metadatos. Un ciclo independiente posterior verifica la cobertura completa con un límite explícito de 16384 consultas de inferencia. Las familias vacías o duplicadas no usan consultas simbólicas; se rechazan cortes insuficientes. También se comprueban presupuestos globales exactos o con un intento menos, resultados incorrectos, falta de progreso, evidencia original y witness indefinidos. Las regresiones del filtro cubren diamantes aritméticos neutros, confluencias locales o solo en la frontera y un punto de unión alcanzable que puede evitarse hacia una salida o frontera de bucle. Verifican el candidato filtrado aunque la familia original sea duplicada, la reutilización de un plan filtrado antes de un intento completo posterior, límites exactos, inferiores en una unidad o nulos de `MaxCutSelectionWork`, el trabajo fallido acumulado en `CutSelectionWork` y que no se inicie inferencia simbólica tras agotar el trabajo global del grafo. Ambas familias de ramas comprueban la cobertura completa de ciclos; el diamante usa límites explícitos de consultas de inferencia y prueba.

Las regresiones cubren regiones de marco y registro, ambas direcciones, posiciones bajas/intermedias/altas, anchuras inusuales, ambos órdenes de bytes y palabras de marco de tres bytes. Verifican descubrimiento tardío, cambios en bits preservados, falta de progreso y desbordamiento sin guarda, entradas adicionales inválidas, presupuestos exactos/insuficientes y búsqueda con un solo corte.

Las regresiones de fase inicial cubren dos y tres bucles secuenciales que reutilizan una palabra de cuenta atrás, la combinación con fases de bucles anidados, presupuestos exactos o con un intento menos para rangos y consultas, bucles sin progreso y reinicios hacia una fase anterior. Las constantes de fase erróneas del mismo ancho, resultados incorrectos y escrituras de marco incorrectas deben rechazarse sin certificado mediante el comprobador completo; la falta de evidencia original sigue sin admitirse.

`InterpreterMachineStateModel.*` en `NeverDLowIRRefinementTests` usa ejemplos LowIR independientes para comprobar indicadores de entrada sin normalizar, estado separado del RAX invitado, las 17 palabras, subregistros, indicadores empaquetados, rechazo dinámico persistente, escrituras del marco invitado, ambas ramas e inferencia de ciclos seguida de una prueba nueva. Deben fallar las salidas incorrectas, el estado perdido, la memoria modificada, los registros obsoletos, las entradas malformadas y los presupuestos agotados. Las pruebas fuente existentes también ejecutan ambas rutas C con O0/O2; las pruebas del modelo por sí solas no certifican el C compilado.

`NeverDLLVMInterpreterModelTests` compara LLVM independiente con oráculos LowIR de estado completo: anchos, PHI paralelos, switch, memoria invitada, estado separado, condiciones poison, rangos intrínsecos, contratos rechazados y cuatro presupuestos. Comprueba una prueba completa de cuenta regresiva de palabra arbitraria y rechaza un estado cambiado. C independiente compilado en O1/O2 debe cumplir las mismas observaciones. Se valida el modelo admitido; descubrir invariantes automáticamente y demostrar el compilador son obligaciones separadas. Los casos de desplazamiento variable cubren los cuatro anchos, cantidades limitadas por máscara o rama, valores límite y excesivos, indicadores sin desbordamiento y exactos, rechazo estricto de poison y C compilado en O1/O2.

`LLVMGuestAlignment.*` compara cargas y escrituras con oráculos independientes de memoria por bytes: dominios alineados y desalineados, bits altos de dirección libres, alineación predeterminada analizada, anchos parciales, accesos sin uso o sobrescritos, ramas inalcanzables y presupuestos exactos o insuficientes por una unidad. `InterpreterLLVMRefinement.GuestAlignmentRequiresBothFreshPremises` comprueba escrituras nativas en pila, congruencias de entrada coincidentes y efectos de fuente alterados mediante dos comprobaciones nuevas de relación.

`LLVMByteSwap*`, `LLVMScalarByteSwap.*` e `InterpreterLLVMRefinement.ByteSwapRequiresBothFreshPremises` usan referencias independientes de copia de bytes y desplazamiento/máscara para comprobar bytes superiores, valores entre bloques, poison, contratos y presupuestos exactos o una unidad menores, calculados independientemente. Los casos Clang O1/O2 exigen intrinsics reales; pequeños casos nativos de intercambio/BSWAP verifican ambas premisas de nuevo y rechazan valores incorrectos o la falta de puesta a cero de la mitad superior.

`NeverDLLVMScalarEquivalenceTests` cubre dominios completos de bucles, cero iteraciones, intercambios PHI simultáneos, switch, bits altos de entrada, contraejemplos en la última partición, actualizaciones adicionales que producen poison, rangos de retorno, contratos no admitidos y presupuestos exactos, insuficientes por una unidad y cero. Oráculos independientes de doble ancho y desbordamiento cubren extremos funnel y productos con restricciones en cada ancho admitido; C independiente con bucles anidados en O1/O2 comprueba el perfil de entrada del compilador. La suite del modelo de estado también comprueba los extremos. `SymExpr.ConstantWindowSharesActualWorkWithoutRelaxingQueryCeilings` verifica la contabilidad acumulada y los límites locales sin cambios.

`LLVMScalarDecision.*` cubre obligaciones profundas de desplazamiento exacto y extensión, ramas constantes, ambas aristas de retorno del bucle, bits altos conservados, operaciones indefinidas tardías, no terminación, cambios tras una comprobación y presupuestos exactos, cortos y locales. `LLVMScalarDecisionCompiled.DeepOneAndTwoBackedgeOracles` compara recurrencias independientes de una y dos aristas de retorno con un oráculo C sin signo a O0/O2 en 32.768 llamadas. Son pruebas del modelo escalar, no cobertura de ABI nativa ni recuperación de binarios completos.

`LLVMScalarDemand.*` prueba cuerpos de bucle no lineales distintos con presupuesto fijo, conserva las dieciséis particiones y bits altos libres, rechaza errores de salida o definición en la última partición y comprueba presupuestos exactos/insuficientes y límites de nodos. `LLVMScalarDemandCompiled.*` compara ambos con un oráculo aritmético independiente sin signo a O0/O2 en 262.144 llamadas. No elimina operaciones fuente ni restringe el dominio de entrada.

`SymKnownBitsTests` contrasta los hechos con todos los pares de bytes y valores límite de precisión arbitraria: extensiones, sumas sin desbordamiento, raíces distintas y semántica total de desplazamientos. Comprueba presupuestos exactos o inferiores en uno, coste de aciertos, capacidad de caché, contextos separados, profundidad, cantidad de operandos y anchuras no admitidas. `SymExprExtensionTests` verifica constantes altas y desplazamientos completos. Las regresiones de equivalencia mantienen los datos simbólicos al demostrar restricciones de rango, rechazan diferencias en la última partición y poison ejecutado, y comprueban el presupuesto de la prueba completa. `SymMBAExtensionTests` exige una derivación con la verificación por muestras desactivada, comprueba todos los pares de bytes y conserva los límites de signo, acarreo estrecho, complemento y agotamiento del trabajo.

`NeverDLLVMScalarLoopRecoveryTests` cubre prefijos, estados de predecesores, alternativas de cero iteraciones, bucles sobre sí mismos, estados afines, igualdad con desbordamiento modular, poison por actualizaciones extra, bits altos y contratos rechazados. Los presupuestos exactos o reducidos en una unidad comprueban el rechazo atómico. Oráculos aritméticos independientes ejecutan el LLVM original y recuperado a O0/O2 con todas las entradas de control de un byte. No certifican recuperación de ABI nativa ni salida C predeterminada.

Las regresiones cubren PHI de 8/16/32/64 bits, comparaciones y polaridades invertidas, extensiones con y sin signo, iniciales externas distintas, observaciones incompatibles, varios retornos, contraejemplos simbólicos, truncado rechazado, poison/no terminación y rechazo atómico por límites de construcción, prueba, candidatos y transformaciones. Oráculos independientes sin signo comparan LLVM original y recuperado a O0/O2 en 458.752 llamadas con trampas de comportamiento indefinido. Fuente y módulo padre permanecen intactos.

Las pruebas cubren guardas modulares de 8/16/32/64 bits, pasos descendentes, polaridades, cero iteraciones, salidas compartidas, vecinos erróneos equivalentes solo con datos cero, límites inalcanzables, fragmentos grandes, poison, hojas ausentes y presupuestos atómicos. Cuarenta argumentos sin uso no deben desplazar la inicialización útil del prefijo. Oráculos independientes sin signo ejecutan LLVM original y recuperado a O0/O2 en 458.752 llamadas con trampas de comportamiento indefinido.

El mismo objetivo comprueba `recoverLLVMScalarSource`: preparación previa, limpieza sin cambios de bucle, estado sin usar de ancho completo, obligaciones muertas de desbordamiento, desplazamiento exacto, división y assume, efectos no admitidos, presupuestos acumulativos exactos o una unidad menores y continuación acotada. Oráculos aritméticos independientes ejecutan el LLVM original y preparado a O0/O2 para todos los controles de un byte y estados deterministas de ancho completo. La fuente y el módulo padre permanecen intactos tanto al aceptar como al rechazar.

Las pruebas cubren identidades modulares anotadas, expresiones equivalentes en predecesores distintos, valores de rama diferentes, rechazo de desbordamiento/desplazamiento exacto/truncado/extensión originales y presupuestos acumulativos exactos o insuficientes. Oráculos independientes sin signo comparan cuerpos originales y preparados a O0/O2 en 131.072 llamadas con trampas de comportamiento indefinido. Las pruebas semánticas existentes mantienen las reglas de rechazo del pase independiente.

Las regresiones de máscaras cubren operandos conmutados, campos cero, bits altos de entrada conservados, alternativas tras un fallo con datos completos, todas las aristas de retorno, rechazo por vuelta modular/desbordamiento, lotes mayores de 32 y presupuestos exactos o una unidad menores con rechazo atómico. Oráculos aritméticos independientes de LLVM y C a O0/O2 comprueban la composición con la recuperación de anchura. Las consultas reflexivas conservan dominios completos, rechazo de poison/undef y contratos no admitidos, no terminación, límites locales y contabilidad exacta; modificar la misma función invalida el resultado anterior. Son pruebas de LLVM escalar, no certificación de ABI nativa.

Las pruebas de inclusión de máscaras agotan todas las parejas de bytes, cubren máscaras no contiguas y anchuras hasta 128 bits, conservan bits desconocidos/altos y limitan el crecimiento de nodos fuera de los límites. Un bucle simbólico con dos aristas de retorno debe demostrar su recurrencia XOR enmascarada frente a una forma cerrada independiente. Las pruebas de dependencias contabilizan el almacenamiento normalizado del desplazamiento conservando presupuestos exactos/cortos y la alternativa conservadora para valores anchos.

Las regresiones de ancho cubren literales no nulos, firmas intactas, entradas e intrínsecos más anchos, bits altos observables, orden con signo, nuevos desbordamientos y presupuestos exactos/insuficientes. Los mismos candidatos escalares se prueban con triples x86-64, AArch64, AArch64 big-endian y ARM32; es cobertura LLVM, no certificación de ABI nativa. Oráculos aritméticos independientes ejecutan LLVM original/recuperado y C generado con O0/O2 y trampas de comportamiento indefinido en C.

Las regresiones cubren todas las entradas, varias aristas de retorno, datos altos ocultos, poison, intercambios simultáneos, división de lotes, más de 32 variables y presupuestos atómicos. Las pruebas escalares distinguen consultas desconocidas terminadas del agotamiento global y prueban desplazamientos seguros sin enumerar bits de datos. Las pruebas simbólicas agotan valores de byte, máscaras y recuentos conservando bits observables, identidad de origen y recuentos grandes. Las vistas de distinta anchura comparten el valor numérico completo del recuento. No certifican la ABI nativa.

Las regresiones adicionales cubren últimos índices estrechos con desbordamiento, bloques body/latch separados, ambas polaridades de la condición, operandos de igualdad intercambiados, recurrencias unitarias reordenadas o descendentes, diferencias en bits altos en la ruta vacía, poison ejecutado de nuevo y límites incorrectos. Los presupuestos exactos y con una unidad menos de construcción y prueba, junto con el agotamiento de candidatos, mantienen el rechazo atómico. LLVM original y C generado se ejecutan en O0/O2 frente a oráculos aritméticos independientes.

`NeverDLLVMCScalarLoopRecoveryTests` comprueba salida completa y seleccionada por defecto, recuperación tras limpiar retornos, identidad y atributos, llamadores, intrínsecos existentes/nuevos y colisiones, presupuestos compartidos, llamadas con efectos, entradas sin garantía de definición, metadatos, imágenes, direcciones externas de bloques y selecciones ajenas. Oráculos independientes de aritmética y rotación ejecutan C en O0/O2 con trampas de comportamiento indefinido. La aritmética también compara LLVM original compilado aparte para todos los controles de un byte, valores límite y datos deterministas de ancho completo.

`SymSimplifyPredicates.*` también compara las políticas de la fase independiente y del pass completo, el trabajo informado, los presupuestos exactos o insuficientes por una unidad, la fase desactivada y las funciones marcadas como ofuscadas. Las regresiones del código escalar cubren condiciones de parada de bucle codificadas aritméticamente en 8/32/64 bits, el rechazo del desbordamiento con signo y los límites de construcción compartidos entre rondas y funciones. Si la publicación falla, se conserva el IR original; el C emitido se ejecuta en O0/O2 frente al LLVM original compilado de forma independiente y un oráculo aritmético.

`SymKnownBits.*` verifica máscaras sin pérdida y desplazamientos con signo de ida y vuelta mediante aritmética exhaustiva de pares de bytes, incluidos valores negativos, fuentes distintas, bits desconocidos descartados, factores/cantidades incompatibles y desplazamientos excesivos de ancho grande. Hasta 128 bits se mantienen los presupuestos exactos o inferiores por una unidad y la ausencia de crecimiento del DAG. Las pruebas de decisión escalar añaden actualizaciones positivas y negativas en aristas de retorno separadas, rechazo de hechos obsoletos y desbordamientos, bits altos simbólicos y 16 384 llamadas O0/O2 frente a un oráculo sin signo independiente.

`SymKnownBits.*` también comprueba exhaustivamente pares de bytes para el orden de múltiplos y productos entre anchos, rechazando desbordamientos, coeficientes o multiplicidades incorrectos y trasladar desbordamientos estrechos a una palabra mayor. Hasta 128 bits mantiene presupuestos exactos o inferiores por una unidad y un DAG sin crecimiento. Las pruebas escalares demuestran sumas repetidas y multiplicaciones sin enumerar bits de datos, conservan cada obligación de desbordamiento de los bucles y ejecutan 16 384 llamadas O0/O2 frente a un oráculo independiente.

`LLVMScalarAssume*` comprueba dominios completos de bucle, fallos en la última partición, condiciones falsas inalcanzables o alcanzadas, obligaciones acumuladas para todas las entradas de un byte, presupuestos exactos o con una unidad menos, cambios de IR y contratos de llamada no admitidos. Cuatro triples de destino ejercitan el modelo compartido; 8.192 llamadas O0/O2 se comparan con un oráculo independiente sin signo. La suite del modelo de estado comprueba por separado la misma obligación y el rechazo de paquetes de operandos.

`LLVMScalarProjection.*` cubre campos anidados, ventanas, argumentos sin uso conservados, múltiples retornos, aristas de retorno, obligaciones overflow/shift/assume no seleccionadas, fallo en la última partición, no terminación, contratos desconocidos, entradas modificadas y presupuestos exactos/una unidad cortos. Cuatro tripletas ejercitan la semántica compartida. `LLVMScalarProjectionCompiled.*` compara el agregado original mediante un puente de arreglos LLVM y las proyecciones con aritmética sin signo independiente en O0/O2. `SymExpr.RightShift*` agota pares de bytes y comprueba extensión de signo, acarreos, bits altos conservados, recuentos completos y límites de búsqueda.

`LLVMScalarInputs.*` comprueba mapas ordenados de anchos mixtos, interfaces sin argumentos o nombre, demanda de aritmética muerta y assume, resultados alterados, contratos desconocidos, rechazo de empaquetado y presupuestos acumulativos exactos o insuficientes. Las pruebas restauran la firma completa sin fijar entradas omitidas. `LLVMScalarInputsCompiled.*` compara bucles originales y reducidos con un oráculo independiente sin signo a O0/O2 variando todos los argumentos omitidos.

`NeverDLLVMScalarStateProjectionTests` cubre ventanas solapadas/no alineadas, celdas de 8/16/32/64 bits, bucles, máscaras, cambios de fuente, rangos de estado, poison conservado, memoria externa y presupuestos exactos/insuficientes. Cuerpos de memoria y puentes de agregados LLVM realizan 172.032 comparaciones O0/O2 con oráculos independientes; las pruebas escalares se comprueban aparte. `SymKnownBits.*` agota pares de bytes y verifica 128 bits, factores distintos, máscaras ampliadas, sumas con desbordamiento y presupuestos. `LLVMCIntrinsicSemantics.AssumeEvaluatesItsConditionAndRefusesBundles` comprueba evaluación única en O0/O2 y rechazo de bundles.

Las regresiones de metadatos comparan bucles contados con una fórmula independiente en todas las particiones de control y con presupuestos exactos o reducidos en una unidad. Un historial de peeling grande o nulo no oculta resultados incorrectos, no terminación ni poison. Los metadatos mal formados construidos mediante API comprueban el rechazo del importador por separado del análisis de ensamblador LLVM; las pruebas de estado de máquina conservan los efectos de estado y los límites de entrada.

Las regresiones cubren rangos parciales y separados, alias fijos, ambas ramas, cada retorno, lecturas en la primera iteración y escrituras antes de lecturas en bucles. Deben fallar las lecturas antes de escribir, escrituras ausentes o invitadas, alias desconocidos, accesos especiales, rangos fuera del objeto y presupuestos agotados. Un ejemplo C independiente con una palabra de estado solo de salida se compila a O1/O2, conserva los atributos LLVM exactos y supera una nueva prueba compuesta de código nativo a LLVM.

Los tests de cuenta regresiva protegida cubren reintentos tras rechazar la plantilla del cuerpo, una prueba completa en la cabecera para palabras arbitrarias, presupuestos compartidos y rechazo inmediato de violaciones reales del contrato de entrada.

`NeverDInterpreterLLVMRefinementTests` comprueba composiciones nuevas, vínculo exacto texto/función, presupuestos independientes, observaciones completas y dominios fuente ampliados. Cambios de bytes, residuos, resultados, indicadores, estados, escrituras, poison o planes falsos/obsoletos deben impedir el comprobante compuesto. Los contadores de palabra arbitraria requieren ambas premisas inductivas; ejemplos C independientes compilados en O1/O2 prueban el LLVM serializado real. Las regresiones rechazan vueltas ocultas a la entrada y limitan raíces sin copiar procedencia auxiliar.

`InterpreterLLVMRefinement.Preservation*` cubre rangos parciales o solapados, peticiones inválidas, coste de preparación calculado independientemente, alteraciones finales idénticas, guardado/restauración de valores de entrada a través de bucles, evidencia opaca nueva y rechazos tardíos. Reconstruir también el consumidor de API `NeverDPEFixedImageTests`. Comparar por separado resultados, contadores y resúmenes sin petición con la base.

`InterpreterLLVMRefinement.Collection*` comprueba retención y aplazamiento necesarios en pruebas finitas e inductivas, ramas inválidas alcanzables, rechazo tardío de la fuente, conservación de entrada y las cuatro identidades de opciones nativas. Compilar fallos que omitan su transmisión en el módulo de composición para verificar cada opción necesaria.

```sh
cmake --build build-release --target NeverDLLVMCScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMCScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarLoopRecoveryTests --parallel 4
build-release/bin/NeverDLLVMScalarLoopRecoveryTests
cmake --build build-release --target NeverDLLVMScalarEquivalenceTests --parallel 4
build-release/bin/NeverDLLVMScalarEquivalenceTests
cmake --build build-release --target NeverDLLVMScalarResultProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarResultProjectionTests
cmake --build build-release --target NeverDLLVMScalarStateProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarStateProjectionTests
cmake --build build-release --target NeverDLLVMScalarInputProjectionTests --parallel 4
build-release/bin/NeverDLLVMScalarInputProjectionTests
cmake --build build-release --target NeverDLLVMInterpreterModelTests --parallel 4
build-release/bin/NeverDLLVMInterpreterModelTests
cmake --build build-release --target NeverDInterpreterLLVMRefinementTests --parallel 4
build-release/bin/NeverDInterpreterLLVMRefinementTests
```

Las salidas por igualdad en caché de bucles de dos y tres niveles comprueban operandos correlacionados, límites móviles, reinicios de contador y copias alteradas.

Las regresiones de comparaciones en caché cubren igualdad y desigualdad, guardas e inicialización constante, campos descubiertos tras ampliar y bits 7/31/63 en cachés de 1/4/8 bytes. Cambiar solo un bit vecino conservando el bit probado también debe fallar en la comparación completa del estado. Se rechazan pasos nulos, límites móviles, reinicios y presupuestos agotados.

Las regresiones cubren entradas unidas, el primer testigo sin iteraciones, diferencias ocultas de registros/marco, predicados booleanos no canónicos, condiciones de trap nativas, copias a pila correlacionadas y planes inválidos o sin presupuesto. Contadores independientes de dos/tres niveles con salida por igualdad y bytes nativos comprueban límites sin signo, dominios cero/máximo, pasos no unitarios e instrucciones originales incorrectas. La inferencia y la prueba final rechazan resultados incompletos.

Las regresiones independientes con bucles alternativos cubren ambas orientaciones de rama, cuerpos incorrectos, una rama hermana no terminante y el agotamiento de los presupuestos compartidos de búsqueda/prueba. `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

Las regresiones cubren dos y tres niveles anidados, contadores ascendentes y descendentes, fases inferidas y cortes en cuerpos nativos reales. Deben rechazarse dominios de prefijo inaccesibles o disjuntos, cuerpos incorrectos, transiciones infinitas o con desbordamiento circular y presupuestos compartidos agotados. Un testigo de prefijo nunca sustituye la cobertura completa de segmentos.

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

`NeverDLowIRUndefinedIndependenceTests` comprueba la independencia entre dos ejecuciones de grafos LowIR completos y acíclicos. Comparten las entradas ordinarias; cada nuevo valor indefinido por la arquitectura conserva sus correlaciones entre copias, escrituras solapadas, guardados en pila y recargas. Los predicados de control se comprueban antes de asumir condiciones de ruta. Los certificados requieren metadatos de efectos `Complete`, vinculados exactamente a los límites completos de cada instrucción y al resumen de sus operaciones. Se rechazan si faltan pruebas, hay bucles alcanzables, llamadas, alias desconocidos o se agota el presupuesto. El resultado se limita a las observaciones explícitas y al contrato de acceso sin fallos al marco; no demuestra la equivalencia completa del código nativo con C.

El comportamiento siguiente usa el contrato de auditoría estricto predeterminado. `NeverDOriginalBinaryUndefinedIndependenceTests` usa bytes x64 independientes con mapeos fijos para comprobar CALL/RET físicos, destinos de retorno modificados, conjuntos finitos exhaustivos de destinos indirectos y lecturas inmutables. El mismo objetivo verifica la recopilación completa de ramas directas, la vinculación exacta de bytes/efectos/mapeos/evidencias de lectura, la preservación del RSP y la ranura de retorno de entrada al retornar al exterior, y la separación marco/imagen. Instrucciones ausentes o solapadas, ramas no auditadas fuera de las reglas exactas de trampas y proyección con perfil explícito, bucles que no terminan o agotan el presupuesto, enumeración incompleta, perfiles/contratos incompatibles y presupuestos agotados deben rechazarse sin certificado ni código residual. Todas las rutas nativas viables deben terminar. Este control opcional no certifica invariantes de bucle, excepciones, ejecución con CET ni equivalencia de código nativo a C; la recuperación ordinaria sigue separada. El objetivo también comprueba los límites terminales de `INT3`/`UD2` elevados estrictamente y la vinculación de sus bytes completos y resúmenes de operaciones. Un sidecar de salidas indefinidas `Missing` debe seguir siendo `Missing`; solo las trampas demostradas inalcanzables mediante ejecución simbólica pueden figurar en un certificado, mientras que toda ruta viable hacia una trampa debe devolver `ContractViolation` sin certificado ni código residual. No se modela la continuación tras trampas ni la recuperación de excepciones, no se usa `codeFollowsTrap` y el soporte de la API LowIR estática no cambia.

Las regresiones de retorno físico cubren llamadas directas e indirectas que saltan bytes inválidos, continuaciones incorrectas alcanzables, enumeración completa y presupuestos exactos o insuficientes. El refinamiento del estado completo rechaza resultados alterados. El C independiente compilado a O1/O2 debe conservar toda la escritura del marco; un retorno igual no oculta bytes alterados en la posición de retorno.

Las pruebas explícitas de solapamiento nativo cubren saltos x64 reales al interior de operandos inmediatos, los resultados de ambas ramas posibles y entradas de retorno indirectas dentro de instrucciones anteriores. Los proveedores sintéticos comprueban rangos contenidos en ambos órdenes de recopilación, bytes contradictorios en una rama directa no tomada, coherencia entre código y lecturas en ambos órdenes y lecturas del candidato. Los presupuestos exactos e insuficientes cuentan bytes repetidos a través de transferencias indirectas. Cambiar resultados, usar API estáticas o de bucles y aportar evidencia contradictoria debe impedir certificados; modificar la opción o el límite cambia los resúmenes.

Las pruebas de fronteras explícitas cubren RCL, XADD de memoria con LOCK y REP MOVS inalcanzables, contradicciones simbólicas de rutas, ramas controladas por valores arbitrarios y rechazos exactos al llegar por entrada, salto indirecto, CALL y RET. Verifican entradas independientes a sufijos alcanzables, colisiones de direcciones candidato/nativo, pruebas malformadas o parciales, recursos agotados, rechazo de API estáticas/de bucles y las tres capas de resúmenes de refinamiento. Cambiar una instrucción inalcanzable o activar la opción sin fronteras retenidas cambia los resúmenes del certificado. Estas pruebas validan el alcance finito declarado, no la semántica de las instrucciones sin auditar.

Las pruebas cubren todas las combinaciones de indicadores escalares de entrada, máscaras de privilegio, TF/AC en ambas ejecuciones, productores indefinidos distintos, copias correlacionadas, llamadas nativas, estados de ramas hermanas, observación final obligatoria, evidencia malformada y límites de recursos. Todo camino factible de entrada de un bucle finito debe terminar; una rama segura no oculta un camino infinito o truncado. RDSSPD/RDSSPQ cubre los 16 registros generales y ambas anchuras, preservación de bits altos, evidencia `Missing` conservada y rechazo de proyecciones falsificadas. Las pruebas de estado de máquina comparan ambas rutas C en O0/O2 con trampas de comportamiento indefinido frente a un oráculo independiente de indicadores de usuario y comprueban que el fallo de perfil persiste. Las pruebas INCSSPD/INCSSPQ cubren ambas anchuras y todos los registros generales, conservación de límites inalcanzables, trampas factibles tras completar una rama hermana, operandos cero y evidencia de trampa falsificada.

`NeverDX86DecodeDetailTests` cubre las tres rutas, ambos anchos de dirección x64, límites con signo, prefijos obligatorios, disp16 de i386, moffs, entradas truncadas y reutilización sin detalles. Solo se vinculan campos exactos de reubicación; un ancho, posición o valor incorrecto no se vincula. Las pruebas nativas de independencia y refinamiento mantienen todas las escrituras del marco y rechazan indicadores arbitrarios observables y candidatos de desplazamiento modificados.

`NeverDLowUndefinedDigestTests` comprueba vectores SHA-256 independientes, todos los campos almacenados, bits de secuencia con signo, orden, exclusión del relleno y entradas sin modificar. Los casos cubren el crecimiento del búfer integrado, el límite de 199/200 operaciones e intervalos incrementales mayores. `LowIRRefinement.StaleUnusedInputRefusesAcrossDigestStorageBoundaries` comprueba el rechazo real de evidencia obsoleta y su nueva vinculación en ambas rutas. Mantener `InputDigest.*` en `NeverDLiftTests` y recompilar los consumidores afectados tras cambiar la implementación separada. Registrar la cobertura real de sanitizers, ruta portable y equipos; estos microbenchmarks no certifican equivalencia nativa.

```bash
cmake --build build-release --target NeverDLowUndefinedDigestTests --parallel 4
build-release/bin/NeverDLowUndefinedDigestTests
```

`NeverDX86UndefinedEffectsTests` comprueba metadatos de bits indefinidos, flags definidos o preservados y el rechazo de certificados obsoletos. `NeverDX86CarryArithmeticFlagTests` comprueba el acarreo auxiliar de ADC/SBB en las formas de registro y memoria frente a un oráculo aritmético. `NeverDX86LogicIdentityTests` verifica que AND con operandos idénticos siga poniendo a cero los bits 63:32 del registro de 64 bits correspondiente al escribir un destino de 32 bits en modo de 64 bits y preserve los bits no escritos de destinos más estrechos.

`X86RotateUndefinedEffects.*` compara todos los contadores brutos, anchos, solapamientos de CL, alias de byte alto y destinos de memoria con un oráculo aritmético escalar. `X86BitTestUndefinedEffects.*` cubre índices de registro/inmediatos, solapamiento de origen/destino, registros extendidos, indicadores definidos y escrituras en las partes altas. Los controles de metadatos rechazan operandos, codificaciones y formas no admitidas alterados. Las pruebas nativas distinguen lecturas correlacionadas de bits nuevos independientes, verifican presupuestos exactos/insuficientes y rechazan desbordamiento indefinido observable. El refinamiento del estado completo acepta el testigo elegido y rechaza testigos de bits cero o candidatos modificados.

`X86XaddAudit.*` comprueba los 65,536 pares de operandos de byte, los límites de banderas en anchuras mayores, el solapamiento de registros/bytes altos, ambas escrituras, los límites de anchura de byte con REX y la conservación de registros completos mediante un oráculo aritmético sin signo. Las pruebas nativas exigen cero bits arbitrarios nuevos sin perder dependencias previas. Ambos testigos aceptan XADD sin cambios y rechazan alteraciones de la suma, la fuente intercambiada o una bandera definida. El alias `/6` usa la matriz completa de cuentas de desplazamiento; los grupos/ID decodificados alterados se rechazan. Las pruebas de memoria también agotan los pares de bytes y cubren cambios de tamaño de dirección, extensiones, direcciones relativas a IP e i386 de 16 bits, desplazamientos con signo, bytes vecinos y detalles obsoletos. Las pruebas nativas del marco completo conservan dependencias arbitrarias previas y límites exactos/insuficientes; alterar la dirección de escritura viola el contrato del retorno.

`X86DoubleShiftUndefinedEffects.*` verifica cada cuenta de byte a 16/32/64 bits mediante transferencias independientes bit a bit, incluidos alias fuente/destino/CL y guardas exactas. Las pruebas nativas descartan RAX para aislar banderas, distinguen 16 de 17 y mantienen dependencias previas y presupuestos exactos/insuficientes. Las relaciones completas rechazan partes definidas alteradas y testigos nulos; una palabra baja arbitraria no autoriza borrar bits altos definidos. Las formas malformadas no publican pruebas parciales.

`*Deferred*` cubre destinos y continuaciones muertos, código ausente o malformado, guardas simbólicas contradictorias, control arbitrario, testigos y mutaciones del estado completo. Los proveedores sintéticos no consultan sucesores muertos y rechazan metadatos alcanzados malformados. Los presupuestos exactos/insuficientes de instrucciones, operaciones, visitas y consultas, alternativas inválidas viables y bucles infinitos no certifican prefijos. La política cambia los resúmenes; las API estáticas/de bucles rechazan la opción.

`NeverDPEFixedImageTests` usa archivos PE construidos independientemente para comprobar instrucciones reubicadas, datos inmutables, escrituras de importaciones, cabeceras/tablas malformadas, alias y procedencia alterada. Las pruebas de código nativo a LowIR y LLVM exacto aceptan candidatos coincidentes y rechazan resultados, estados o bytes nativos modificados. El agotamiento del presupuesto de preparación conserva su clasificación y permite reintentar con límites ampliados explícitamente; la carga ordinaria también acepta 40000 reubicaciones válidas por encima del presupuesto de análisis predeterminado.

`FrameOffsets.*`, `NativeStackSpecialization.*` y `OriginalBinaryUndefinedIndependence.*` comprueban todos los residuos de alineaciones 2/4/8/16/32, bits altos libres, valores guardados entre llamadas, bucles de cuenta atrás, corrupción por alias, selección errónea, máscaras grandes irrelevantes, ampliaciones necesarias y presupuestos exactos o inferiores en una unidad. Controles nativos separados comprueban alineación condicionada, limpieza interna sin signo, limpieza incorrecta y retornos con prefijos. Estos tests no establecen cobertura automática nativo-a-LLVM para bucles particionados.

Las regresiones nativas cubren 64 lecturas alineadas con el presupuesto de una, presupuesto inferior en uno, direcciones modificadas fuera del marco y la misma dirección bajo predicados distintos tras retornar una ruta. Siguen siendo necesarias las pruebas de mutación de estado, claves y capacidad.

Las regresiones de factibilidad repetida conservan las 130 instrucciones nativas con el presupuesto de consultas de dos instrucciones lineales. Rechazan presupuestos de consultas/instrucciones inferiores en uno, trampas alcanzables tras cambiar ramas o dominios de entrada, agotamiento de puertas del solver y estados candidatos modificados.

Las regresiones prueban 558 combinaciones de ancho, alineación, residuo y sesgo con un presupuesto de puertas insuficiente para restar raíces completas. Cubren fragmentos con fuentes/sesgos distintos, máscaras dispersas sin restringir, acarreos y desbordamientos modulares, máscaras anidadas y agotamiento de nodos/consultas. Las pruebas nativas verifican escrituras mediante punteros parcialmente alineados y rechazan alineación ausente, accesos fuera del marco y valores almacenados alterados en el refinamiento completo.

Las pruebas cubren todos los residuos de 1/2/4/16, dos raíces, bits altos libres, dominios inválidos, constantes contradictorias, límites, huecos de exclusión, preservación y presupuestos exactos/menos uno. Las plantillas inductivas mantienen el predicado inicial. Las pruebas nativas y LLVM nuevas rechazan dominios distintos y cambios de estado; los resúmenes vinculan ambos dominios. Los casos independientes no deducen alineación de una ABI o ejecución. Un C independiente con guarda se compila sin cambios a O1/O2 y se demuestra frente a instrucciones nativas reales para dos residuos; los mismos artefactos deben fallar con otro residuo.

Las regresiones cubren ambos órdenes de bytes, bases de marco altas, campos sobrescritos y solapados, aristas tardías, predecesores ampliados, CALL/RET nativos y presupuestos exactos o insuficientes por una unidad. Ambas rutas C ejecutan los cuatro casos de memoria a O0/O2. Las comprobaciones nativas fijan dos valores del selector por separado; no prueban entradas sin restricciones. Un ejemplo LLVM independiente verifica, incluidas copias PHI y escrituras, que mover la rama falsa antes de la unión compartida no la haga ejecutar después de la verdadera.

Las regresiones cubren particiones más finas, todos los residuos permitidos, distintos bits altos, fallo del último caso y presupuestos exactos o inferiores en una unidad. Ambos backends C se ejecutan en O0/O2 con direcciones invitadas rechazadas inaccesibles y flags inválidos: el estado 2 debe conservar cada byte de estado. Las pruebas del modelo y C/Python verifican disposición v5, propiedad y extensiones antiguas o futuras. Las pruebas nativas rechazan dominios de alineación sin vincular.

`StringTransfer.*` y las regresiones de copia repetida comprueban solapamiento, contador cero, aislamiento de temporales, límites de capacidad y presupuesto e invalidación de punteros. `MachineStringSourceTests.cpp` compara ejecución nativa y ambas rutas C a O0/O2 con un oráculo independiente de todos los registros, indicadores y bytes de pila, para cuatro anchos y ambos sentidos.

`ControlDiscovery.*` y `NativeStackSpecialization.*` cubren condiciones sobre los bits bajos de la raíz, dependencias de los bits altos y de toda la raíz, recorridos incompletos, presupuestos exactos e insuficientes y la conservación de evidencias de direcciones inmutables finitas.

Las pruebas de guardas ante destinos no resueltos cubren una fase inválida inalcanzable, un destino desconocido alcanzable al entrar o tras una arista de retorno, límites exactos de refinamiento y presupuestos de descubrimiento adyacentes con éxito o agotamiento. Un rechazo no publica código residual, orígenes ni testigos de lectura. El trabajo opcional tras una recuperación completa no fija un presupuesto mínimo necesario.

Las regresiones de proyección del marco bajo demanda cubren registros automáticos y existentes, raíces altas y desbordamiento modular, condiciones limitadas al byte bajo, hechos contradictorios o ausentes en aristas hermanas, presupuestos de consultas adyacentes y el resultado unknown del solucionador. Una demanda estrecha no convierte una prueba parcial del puntero en una prueba completa; el rechazo no publica código residual ni testigos.

Las regresiones de subcampos finitos cubren selectores en ambas mitades, datos arbitrarios observables, registros y ranuras de marco, ambos órdenes de bytes, campos estrechos, ampliaciones posteriores del dominio, máscaras disjuntas y hechos ausentes. Los destinos alcanzables que falten, selectores libres, presupuestos compartidos agotados y resultados unknown del solucionador no deben publicar código residual ni testigos. También se cubren el descubrimiento automático con demanda de la palabra completa, las indicaciones manuales sin uso, los datos derivados de la raíz y las ventanas constantes iniciales.

Las pruebas de presupuestos públicos cubren valores predeterminados y máximos de 32 bits, el agotamiento independiente de evaluaciones y descubrimiento, ambas ABI de origen y motores C, y valores CLI mal formados. Las comprobaciones de disposición C/Python cubren v7, la validación heredada y las extensiones futuras ignoradas por v1–v6. El rechazo por presupuesto no publica código ni testigos; el contador de evaluaciones no debe desbordarse al alcanzar su máximo.

Las pruebas de corte opcional comparan bucles directos y anidados con un mismo presupuesto de operaciones, ejecutan los bucles residuales, distinguen modos de decodificación y comprueban que el encadenamiento cero no cambia. Un contraejemplo de correlación debe funcionar por defecto y rechazar la publicación con la opción. Las llamadas nativas repetidas, ranuras de retorno, reproducción de dependencias y predecesores tardíos usan ambas configuraciones. Las pruebas públicas cubren ambas ABI y backends, banderas por defecto y desconocidas, API antiguas que ignoran la extensión y el booleano informado.

`NativeStackSpecialization.NarrowAddressDemandRetainsCompletePointer` comprueba uniones condicionadas de punteros en registros y ranuras del marco, ambos órdenes de bytes, el desbordamiento modular y raíces altas, la restauración de la pila y 120 bytes del marco. Los contraejemplos asociados rechazan punteros corruptos y ejercitan presupuestos exactos o una unidad menores para consultas, operaciones, evaluaciones y refinamientos, además del agotamiento del descubrimiento y los contextos.

Los tests del observador cubren el rechazo anticipado, constantes, proyecciones vacías y la última consulta UNSAT. Las regresiones conservan las lecturas en ejecución tras un contraejemplo, revalidan dominios de direcciones en caché para otra extensión de lectura y rechazan certificados malformados sin publicar pruebas parciales.

Las pruebas de control afín cubren condiciones sobre bits bajos con un presupuesto de ocho consultas, registros y ranuras del marco en ambos órdenes de bytes, desbordamiento modular, observaciones de bytes del marco y restauración de la pila. Una condición contradictoria no literal debe eliminar una rama no admitida; dos raíces con los mismos 32 bits bajos y distintos bits altos deben conservar ambos destinos indirectos.

`FiniteQueryCache.*` comprueba límites de almacenamiento exactos y de una unidad menos, la actualización del orden por aciertos, la expulsión de varios registros de distinto tamaño, las expulsiones repetidas y las consultas con variables renombradas. Los duplicados, fallos de caché, resultados mal formados y candidatos demasiado grandes conservan el orden. Las copias de pruebas devueltas siguen siendo válidas después de expulsar sus entradas.

`ControlStateRecovery.*Marginal*` cubre dominios independientes de destinos y datos de negocio cuyo producto excede el límite conjunto, salidas concretas del programa residual, un predecesor tardío que añade un destino después de ampliar la relación, destinos alcanzables ausentes y el descarte completo de dominios excesivos o incompletamente enumerados. Estos casos originales comprueban la reanudación del análisis ante cambios de dominio y que los fallos no publiquen grafos parciales. Una variante con una ranura del marco comprueba la invalidación por alias tras ampliar la relación, en ambos órdenes de bytes.

`InterpreterTransferChain.*` comprueba valores correlacionados, ramas únicas y dinámicas, predecesores tardíos, límites del marco, rechazo de alias y presupuestos. Las pruebas nativas CALL/RET adicionales verifican apariciones repetidas, bytes del retorno y restauración de la pila. La reproducción de productores y los controles de nativo a LLVM cubren contratos incompatibles, comprobantes cambiados, resultados incorrectos y escrituras de pila ausentes.

`NeverDX86NoIndexAddressTests` comprueba el direccionamiento SIB x86 sin índice con direcciones de 32 y 64 bits: bits de escala ignorados, anchuras de destino, cargas/almacenamientos, metadatos completos de salidas indefinidas, desplazamientos de segmento y procedencia de direcciones. Rechaza seudorregistros como base o índice de anchura incorrecta y conserva los índices R12 reales seleccionados por REX.X. Las pruebas EVEX de difusión y movimientos enmascarados también cubren estas formas, la supresión de accesos inactivos y los metadatos SIB contradictorios.

Las regresiones cubren todos los contadores de ocho bits, combinaciones de indicadores con cuenta cero, ambos modos x86, todos los anchos, alias CL, AH/CH/DH/BH, registros extendidos y memoria. La ejecución simbólica por bytes se contrasta con aritmética repetida bit a bit. Las pruebas relacionales verifican copias y nuevos indicadores, guardados, bucles, cuentas derivadas de valores indefinidos, ramas, formatos inválidos, resúmenes y presupuestos. Las lecturas finitas cubren 1/2/4/8 bytes, selección según entrada, conjuntos unitarios por ruta, testigos completos y límites; rechazan candidatos dependientes, ausentes, modificables, sin respaldo de archivo, reubicados o ilimitados.

Las pruebas del núcleo comprueban la separación de contextos, uniones de punto fijo, bucles dinámicos, registros solapados, invalidación de alias, despacho finito y rechazo sin sustituciones parciales. Las pruebas de código fuente ensamblan máquinas x64 originales de registros, de pila y de direcciones finitas; incluyen campos de control relacionados y un oráculo nativo independiente SysV/Win64. Ambas rutas C se compilan en O0/O2 con trampas de comportamiento indefinido y se comparan con un oráculo sin signo para cálculos, escrituras en memoria y centinelas de salida. Los casos negativos comprueban certificados ausentes y presupuestos insuficientes. La CLI pública y sus informes verifican controles, presupuestos, contadores y rechazos. Se necesitan Clang multicompilación y LLD; ejecutar el ELF original requiere además Linux x64. Una herramienta ausente o un host incompatible significa cobertura omitida, no éxito.

`ControlStateRecovery.LongTransparentLoop*` cubre un bucle de 20 fases escrito de forma independiente, oráculos aritméticos dinámicos, el rechazo de selectores desconocidos y el agotamiento de presupuestos. `LongTransparentPhasesKeepExactBitDemands` comprueba que los bits ajenos del mismo byte del selector sigan siendo datos observables en ejecución sin convertirse en demandas de control. `ProducerClosureChargesWorkBeforeAnotherRestart` comprueba que el descubrimiento inverso y la reevaluación consuman los presupuestos compartidos antes de iniciar un nuevo grafo, sin publicar resultados parciales.

`X86ShiftCarry.*` comprueba el acarreo del desplazamiento aritmético a la derecha
de enteros estrechos, los conteos enmascarados y los destinos y la supresión de
flags APX mediante desplazamientos sucesivos de un bit.
`NarrowArithmeticShiftCarrySurvivesBothSourceBackends` ejecuta ambas salidas C
con O0/O2 y trampas de comportamiento indefinido, cubriendo todos los valores de
byte y conteos originales. `NeverDLLVMCIntrinsicSemanticTests` ejecuta también
min/max de enteros con y sin signo i1/8/16/32/64/128 con O0/O2: resultados
asignados o integrados, orden de los productores y evaluación única. Los anchos
escalares no admitidos y los operandos mal formados deben fallar explícitamente.

## Comprobaciones de control y llamadas en C estructurado

`HighControlFlowSemantics.*` comprueba que mover salidas o bloques finales de bucles conserve las etiquetas referenciadas por otros saltos. Las entradas directas a las salidas iniciales y finales y la sustitución de break ejecutan el C generado con O0/O2 frente a valores de retorno esperados independientes.

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` comprueba que los argumentos de registro obligatorios inferidos conserven las posiciones finales desconocidas. Evaluar un argumento obligatorio o una condición desconocida debe provocar una trampa explícita; los operandos omitidos, nulos o anidados no deben convertirse silenciosamente en cero. Los valores conocidos y los operandos adicionales cuya ausencia de lectura esté demostrada siguen siendo ejecutables. Una trampa es un límite de diagnóstico, no una prueba de equivalencia del comportamiento recuperado.

## Pruebas de ejecución CPU

`NeverDIntegerABITests` compila fixtures originales de Clang para Windows x64, Linux x64 y Linux ARM64. Sus funciones reales de diez argumentos comprueban registros, pila y marcos de llamada. La matriz Unicorn/KVM/WHP marca explícitamente como omitidos los pares host/ISA no disponibles; un skip no es un aprobado. `NeverDExecutionBudgetTests` verifica presupuestos compartidos de continuación, reservas y deadlines absolutos sin depender de pausas temporizadas.

`NeverDCPUEmulationTests` cubre instrucciones ARM64, control, cargas, contextos, alias, invalidación de caché y bucles acotados; el perfil software también ejecuta FP/SIMD y TLS. `NeverDUserExecutionTests` comprueba permisos CPL3/EL0, alias, fallos de protección, contextos y cambio de espacio. `NeverDServiceRequestTests` demuestra que SYSCALL/SVC se interceptan antes del transporte y conservan el estado hasta consumir la solicitud una sola vez; esto prueba el protocolo de transferencia, no un modelo completo de servicios del SO. `NeverDExecutionConfigurationTests` valida resolución compartida, diferencia entre soporte de compilación y sondeo real y rechazo cerrado de opciones. Las pruebas públicas SDK/CLI no requieren el modelo Windows. `NeverDThreadPointerTests` verifica FS-base, `TPIDR_EL0`, restauración de contexto y permisos. `NeverDKvmCancellationTests` usa un invitado x64 no terminante para comprobar interrupción KVM activa, reanudación y estado de señales sin cambios; se omite explícitamente si no hay KVM.

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests NeverDCPUEmulationTests NeverDUserExecutionTests NeverDServiceRequestTests NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget|CPUEmulation|UserExecution|ServiceRequest|ExecutionConfiguration)Tests$' --output-on-failure
```

La falta de CPU ARM64 o hipervisor es cobertura nativa omitida, no un aprobado. Unicorn y la compilación cruzada no prueban ejecución KVM/WHP nativa.

`NeverDParallelExecutionTests` fuerza llamadas solapadas, cancelación de escritores en espera, estados CPU independientes, competencia por alias y transporte privado. `NeverDRunControlTests` verifica dos leases WHP independientes simultáneos y la serialización del caché compartido. `NeverDMMIOAtomicTests` compara dispositivo/RAM para instrucciones x64 atómicas/de actualización y todos los casos ARM64 LSE, ambas observaciones anchas, vistas caducadas, fallos y carreras commit/stop. `KernelMMIOFailure` cubre alias, escrituras idénticas, doble commit, energía, unmap y propietarios destruidos. Las plataformas no disponibles se omiten explícitamente. La barrera del wrapper prueba llamadas concurrentes, no retiro simultáneo de instrucciones hardware. ARM64 KVM/WHP necesita el host correspondiente.

```bash
cmake --build build-cpu --target NeverDParallelExecutionTests NeverDMMIOAtomicTests NeverDRunControlTests --parallel 4
ctest --test-dir build-cpu/unittests/emulation -L '^NeverD(ParallelExecution|MMIOAtomic|RunControl)Tests$' --output-on-failure
```

## Pruebas del perfil de procesos Linux

Las [suites independientes de procesos](process-emulation.md) compilan fixtures ELF reales x64/AArch64. `NeverDLinuxProcessTests` verifica inicio, política de cabeceras, solicitudes de servicio, salida binaria, fallos y límites. `NeverDProcessPublicTests` comprueba C API/CLI sin modificar la imagen de análisis. `NeverDExecutionSessionTests` cubre dos CPU que comparten memoria/presupuesto y consumo exactamente una vez de solicitudes/fallos. `NeverDX64MemoryUpdateTests` comprueba aritmética de memoria, SETcc, BT, XMM/MXCSR, observadores de escritura, límites REP y lecturas preparadas de dispositivos. `DriverBackendParityTests.cpp` ejecuta fixtures WDK originales y reubicadas y compara todo el informe observable con Unicorn; las imágenes/backends ausentes se omiten.

El x64 comprobado admite también las formas heredadas enmascaradas `SS`, `SD`, `PS`, `PD` de `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`. `X64SSEInstructions.def` centraliza anchuras, alineación y admisión. `MaskedSSEArithmeticMatchesIndependentHostExecution` compara registros/RAM con un oráculo CPU anfitrión independiente: cuatro redondeos, FTZ, ceros con signo, subnormales y NaN. `SSEMemoryObserverStopsBeforeResultAndStatusChanges` comprueba la parada antes de los efectos. Las excepciones sin máscara, x87 y AVX siguen excluidos.

`X64PackedIntegerTests.cpp` usa codificaciones originales y 180 resultados fijos de `X64PackedIntegerCases.def`, contrastados de forma independiente con intrinsics del compilador x64 nativo. Los casos de registros y alias RAM al final de página conservan los otros XMM, centinelas enteros, FLAGS, MXCSR y bytes fuente. Las interrupciones/errores de observadores y los fallos de lectura recuperables conservan el estado; la reparación permite un reintento. La desalineación genera `#GP(0)`; MMX, LOCK y MMIO siguen rechazándose antes de callbacks. La CI nativa exige ambos privilegios WHP.

`X64PackedShiftTests.cpp` y los casos originales de `X64PackedShiftCases.def` comparan diez desplazamientos con cálculos escalares independientes e intrinsics SSE2 nativos, usando 16 contadores inmediatos y 21 variables. Cubren alias entre contador y destino, bits altos ignorados, alineación, observadores, fallos recuperables y rechazo de dispositivos. `X64VectorTestSupport.h` comparte las comprobaciones de registros y RAM con las pruebas aritméticas empaquetadas. La CI nativa exige ambos modos de privilegio WHP.

`X64VectorMaskTests.cpp` compara codificaciones originales independientes con extracción escalar e intrinsics SSE nativos: cada bit fuente y las 16 GPR × 16 XMM con ambos valores REX.W. Las instantáneas de todos los registros públicos, la RAM y los observadores verifican la extensión con ceros y la conservación del estado. Las paradas, los fallos de callbacks y las formas no admitidas no publican efectos. La validación nativa KVM/WHP exige ambos modos de privilegio.

`X64ShuffleTests.cpp` usa codificaciones independientes de `X64ShuffleCases.def` y compara la selección escalar de elementos con intrínsecos nativos. Cubre los 256 controles con registros, fuente idéntica y alias al final de página, todas las parejas XMM, el estado público completo de CPU y RAM, paradas y excepciones de observadores, permisos, fallos de alineación y reintentos. MMX, VEX/EVEX, LOCK y los operandos de dispositivos deben rechazarse sin efectos. La aceptación nativa KVM/WHP exige estos casos en ambos niveles de privilegio; las parejas host/ISA no disponibles se omiten explícitamente. Los casos `UNPCKLPS`, `UNPCKHPS`, `UNPCKLPD` y `UNPCKHPD` reutilizan la misma matriz de estado y fallos y los oráculos escalares y nativos independientes. También se comprueba cada destino XMM con una fuente de memoria.

`X64PartialMoveTests.cpp` y `X64PartialMoveCases.def` comparan referencias escalares/nativas independientes de carga y almacenamiento, los 16 registros XMM y bits crudos de NaN/subnormales. Verifican todo el estado CPU y dos páginas RAM ante accesos no alineados, alias, fallos entre páginas, reparación de permisos, paradas/fallos de observadores y reintentos. Bastan ocho bytes al final de página; almacenar no requiere permiso de lectura. Se comprueban aparte alias de registros, formas rechazadas y callbacks de dispositivos; los casos nativos KVM/WHP son obligatorios en ambos privilegios.

`X64IntegerFloatTests.cpp` usa codificaciones independientes de `X64IntegerFloatCases.def`, expectativas `APFloat` e instrucciones nativas con estado FP guardado/restaurado. Comprueba ambos anchos enteros, cuatro redondeos, estado persistente de precisión, FTZ, cada par GPR/XMM y todo el estado CPU/RAM. Fuentes no alineadas, entre páginas o al final de página, reparación de permisos, paradas/fallos y reintentos mantienen rangos exactos. Los casos nativos KVM/WHP son obligatorios en ambos privilegios.

`X64FloatIntegerTests.cpp` comprueba redondeo y truncamiento con codificaciones independientes de `X64FloatIntegerCases.def`, `APFloat` e instrucciones nativas de registro/memoria. Límites con signo, valores intermedios, NaN, infinitos, subnormales, todos los redondeos, estados persistentes y FTZ cubren ambos anchos enteros. Verifica cada par GPR/XMM, todo el estado CPU/RAM, lecturas exactas al final de página, fallos recuperables entre páginas y cancelación/reintento de observadores. Los resultados nativos KVM/WHP en ambos privilegios son obligatorios.

`X64SSEComparisonTests.cpp` usa codificaciones independientes de `X64SSEComparisonCases.def`, orden `APFloat` e instrucciones nativas que guardan/restauran FLAGS y el estado FP del host. Todos los pares de 21 entradas cubren prioridad NaN/desnormal, ceros, infinitos y valores adyacentes. Comprueba cada par XMM y alias, MXCSR persistente, independencia del redondeo, conservación de DF, estado CPU/RAM completo, lecturas exactas de fin de página/entre páginas y cancelación/reintento. Los resultados nativos KVM/WHP en ambos privilegios son obligatorios.

`X64SSEPredicateTests.cpp` usa predicados independientes de `X64SSEPredicateCases.def`, entradas brutas compartidas de `X64SSEComparisonCases.def`, orden `APFloat` e instrucciones nativas originales. Cubre todos los pares, prioridad de excepciones entre canales, canales escalares superiores, alias XMM, estado CPU/RAM completo, lecturas de fin de página/entre páginas, prioridad de alineación y reintentos tras observación/fallo. Las pruebas directas de Capstone cubren todos los bytes de control, ambas sintaxis y API y modos de 32/64 bits. Los resultados nativos KVM/WHP son obligatorios en ambos privilegios; controles reservados, VEX/EVEX y operandos de dispositivos siguen excluidos. Las matrices de valores se dividen por instrucción y predicado; las de redondeo y control, por instrucción. `NativeCPUTests.def` sigue exigiendo todas las combinaciones originales, sin cambiar los plazos del invitado ni los 15 segundos de CTest. La compilación comprueba que cada par instrucción/predicado aparezca exactamente una vez.

`X64SSEPrecisionTests.cpp` combina codificaciones independientes de `X64SSEPrecisionCases.def`, redondeo `APFloat` e instrucciones nativas originales. Los límites usan exponente ilimitado, incluyendo desbordamiento dirigido a valor finito y resultados pequeños redondeados a normales. Cubre ambos signos, cargas NaN, todos los modos de redondeo/FTZ/estado persistente, agregación vectorial, alias XMM, CPU/RAM completos, anchos exactos, prioridad de alineación, fallos de página y cancelación/reintento de observadores. KVM/WHP son obligatorios en ambos privilegios.

`X64PackedFloatTests.cpp` usa codificaciones independientes de `X64PackedFloatCases.def`, expectativas con signo de `APFloat` e instrucciones nativas originales. Comprueba todos los pares de entrada, límites enteros, puntos medios, redondeos, estado persistente y FTZ. Alias, pares XMM, CPU/RAM completos, cada división m64, finales de página, prioridad de alineación y reintentos tras observaciones/fallos cubren ambos privilegios. Todos los casos KVM/WHP son obligatorios.

`X64PackedFloatIntegerTests.cpp` combina codificaciones independientes de `X64PackedFloatIntegerCases.def`, entradas de `X64FloatIntegerCases.def`, `APFloat`/`APSInt` e instrucciones nativas. Todos los pares de 43 entradas cubren límites signed32, vecinos de puntos medios, NaN, infinitos y subnormales. Otras matrices varían independientemente los canales exactos, inexactos, inválidos y subnormales. Se verifican todos los redondeos/FTZ/estados persistentes, alias XMM, CPU/RAM completos, finales de página alineados, prioridad de alineación y reintentos. KVM/WHP son obligatorios en ambos privilegios.

`DAZBackends` amplía con DAZ las matrices de comparación, predicados y conversiones escalares o empaquetadas entre enteros, flotantes y precisiones. `X64DAZTestSupport.h` normaliza las entradas de forma independiente con `APFloat` y comprueba el `MXCSR_MASK` del anfitrión antes de ejecutar instrucciones nativas de referencia. Las pruebas comparan ceros con signo, subnormales, NaN, carriles mixtos, todos los redondeos, FTZ y estados persistentes, y verifican la conservación de los bytes fuente, otros registros, FLAGS y toda la RAM. Los operandos en registros, alias y límites de página mantienen sus observaciones de acceso. KVM/WHP exigen todos los casos DAZ en ambos privilegios y los 17 casos de referencia con instrucciones originales del anfitrión; las ejecuciones portables omiten explícitamente los anfitriones incompatibles. Se conservan los casos sin DAZ y los plazos existentes.

`X64AlignmentTests.cpp` comprueba que los operandos desalineados de instrucciones aligned SSE admitidas notifican un `#GP(0)` recuperable o terminal antes de observadores, permisos o callbacks de dispositivo. Se conserva todo el contexto público de registros x64, PC y RAM. El ajuste al ancho de dirección precede a la suma de FS/GS; reparar la dirección permite reintentar la instrucción original. Pruebas directas KVM/WHP verifican de forma independiente el límite de hardware. Windows ring3 entrega los fallos clasificados `operand_alignment`; otras causas de `#GP` siguen sin admitirse.

`X64SIMDExceptionTests.cpp` omite la admisión checked para verificar el transporte nativo de `#XM` en KVM/WHP con ambos privilegios. Los ocho casos originales de `X64SIMDExceptionCases.def` cubren seis tipos de excepción, incluidos resultados diminutos exactos y desbordamientos exactos con exponente ilimitado. Las formas de registro y RAM conservan todo el estado GPR, XMM, x87, FLAGS, FS/GS y la memoria invitada al fallar, salvo el estado MXCSR especificado. Enmascarar la excepción permite reintentar la instrucción; reparar los operandos conservando los indicadores persistentes verifica que los antiguos no la reactiven. Los contratos públicos checked y de controlador también ejecutan los casos originales de fallo y reintento. `WindowsSIMDExecutionTests.cpp` ejecuta un fallo nativo real, instrucciones VEH/VCH invitadas y continuaciones por salto, enmascarado o reparación de operandos. Verifica por separado controles activos y contexto guardado. Las pruebas negativas de inicio rechazan fallos ausentes, vectores erróneos y destinos modificados sin publicar capacidades.

`check_windows_simd.py` compila un programa Windows x64 original e independiente desde `WindowsSIMDCases.def` y los casos escalares. Sus 6.144 observaciones cubren operandos de registro/RAM, todas las máscaras, indicadores persistentes vacíos o completos y tres continuaciones: saltar, enmascarar y reintentar, o reparar operandos y reintentar sin cambiar las máscaras. Las entradas en ensamblador registran los estados MXCSR y x87 reales de VEH/VCH separados del `CONTEXT` guardado. Se comprueban el PC exacto, la conservación del estado, el contexto reparado y el resultado antes de restaurar el anfitrión. CI conserva registros brutos y hashes de fuentes. `--build-only` solo demuestra compilación. Estas observaciones no habilitan SIMD sin máscara en checked ni demuestran ejecución nativa ARM64.

`WindowsSIMDStatusCases.def` fija las 63 combinaciones no vacías de estados activos observadas en Windows nativo. `WindowsSIMDMappingTests.cpp` verifica códigos y parámetros exactos, rechaza fallos incoherentes y controles inválidos, e inyecta el límite del fallo para comprobar registros, ambos controles de CONTEXT y una continuación enmascarada. El ejecutable original de CI Windows valida estos resultados de forma independiente. La inyección no acredita la entrega de excepciones de Unicorn ni habilita SIMD sin enmascarar en ejecución checked.

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
```

Los backends no disponibles se omiten explícitamente. La compilación cruzada y Unicorn ARM64 no prueban KVM/WHP ARM64 nativo.

## Comprobaciones de emulación de controladores

Active `NEVERD_ENABLE_DRIVER_EMULATION=ON` junto con `BUILD_TESTING=ON` para
compilar la suite de ejecución específica y las comprobaciones de API C/CLI
mediante la biblioteca compartida:

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

Las fixtures ejercitan la inicialización del invitado, retornos satisfactorios
y fallidos, comportamientos no compatibles, fallos de memoria, análisis estricto
de escenarios, ejecución acotada, E/S síncrona con búfer y directa, READ/WRITE,
ciclos de vida independientes de archivos, permisos MDL, exportaciones dinámicas,
argumentos variables del invitado y fallos estructurados de CPU mediante create,
transferencias, cleanup, close y unload. Utilice la [CLI `emulate-driver`](driver-emulation.md)
para verificar el JSON y los códigos de salida del proceso. Las compilaciones
de producción pueden activar esta función con `BUILD_TESTING=OFF`; `libneverd`
no debe requerir una configuración de Unicorn exclusiva de las pruebas.

Las pruebas adicionales cubren MDL de pool no paginado propiedad del controlador, vidas independientes del descriptor y del búfer, estructuras de consulta del registro y búferes cortos, permisos de identificadores, eliminación y fugas, y los 64 bits completos de `information_hex` para IOCTL sin salida. La validación externa también cubre lecturas/escrituras directas síncronas y consultas de estadísticas de Zero.

Las pruebas verifican contextos CPU completos (registros, indicadores, SIMD, FPU, CR8), memoria compartida y rechazo de contextos ajenos o en fallo. Los fixtures compilados `driver_dispatcher.c` ejecutan DPC y trabajo reales, vencimientos, eventos/temporizadores de notificación y sincronización, esperas no alertables `KernelMode` con razón `Executive`, timeout/delay, varias pilas bloqueadas, activación conservada tras set/reset, argumentos y errores de IRQL/vida. Las pruebas de trabajo mantienen cobertura pendiente/finalización, colas, bloqueos y límites comunes. Demuestran el subconjunto descrito, no todo el comportamiento asíncrono de Windows.

`DriverThreadPriorityTests.cpp` ejecuta el controlador original compilado `driver_thread_priority.c` sobre Unicorn/KVM/WHP explícitos con contratos driver y checked. Verifica cambios de prioridad en cola y en espera, despertares por evento/temporizador dentro del cuanto, rotación entre iguales, máscara DISPATCH_LEVEL y avance de temporizadores pese a la inanición inferior. Dos bucles de conteo comparados demuestran que se conserva exactamente el cuanto restante. Las pruebas del modelo cubren ABI con signo, rechazos sin mutación, referencias terminadas, identidad anidada y reutilización de pilas independientes. Los casos nativos son obligatorios en `NativeDriverTests.def`; los transportes locales no disponibles se omiten explícitamente.

`DriverMutexThreadTests.cpp` ejecuta cuatro modos WDK originales de `driver_seh_mutex.def`: recursión en un filtro SEH, adquisición por un filtro o finally excepcional y reanudación de un filtro bloqueado tras la liberación por otro hilo del sistema. Los contratos driver y checked de Unicorn/KVM/WHP cubren imágenes normales/CFG activo, direcciones preferidas/reubicadas y ejecución cooperativa/con cuantos de 1/17 instrucciones. Las pruebas del modelo verifican también la desactivación de APC tras retirar la pila anidada, la liberación por otro hilo y el control del retorno exterior; los casos KVM/WHP son obligatorios en `NativeDriverTests.def`.

`KernelWaitSetTests.cpp` ejecuta dieciséis casos de modelo sin Unicorn: `WaitAll` parcial, primer `WaitAny` listo, índices capturados, limpieza al vencer, objetos/almacenamiento posteriores inválidos, límite de 64, IRQL, hilos terminados retenidos y dos temporizadores síncronos. `DriverMultipleWaitTests.cpp` ejecuta siete modos WDK originales de `driver_wdm_multiple_wait.c` y `DriverMultipleWaitCases.def` sobre Unicorn/KVM/WHP, ambos contratos, imágenes normales/CFG, reubicación y cuantos cooperativos/1/17 instrucciones. Los 30 resultados modelo/nativos son obligatorios en `NativeDriverTests.def`. Las regresiones también cubren la finalización duplicada tras éxito o vencimiento, la alteración del estado capturado y la finalización repetida de un retardo.

`KernelMultipleWait.DispatcherScalarParametersIgnoreUpperRegisterBits` comprueba bits altos contaminados, límites con signo, desbordamiento y conservación del estado del objeto. Los envoltorios de salto final sin prólogo de `DriverMultipleWaitCases.def` ejecutan las mismas llamadas ABI válidas mediante importaciones WDK reales en `driver_wdm_multiple_wait.c`, con CFG activo, reubicación y desalojo por instrucción.

`driver_context_limits.c`: Los límites IRQL proceden de `KernelAPIIRQL.def`; el modelo propietario comprueba las restricciones por argumento. Un DPC no puede llamar al registro ni asignar, liberar o acceder al pool paginado. Las conversiones Unicode de `DbgPrint` requieren `PASSIVE_LEVEL`; la salida ANSI y operaciones no paginadas admitidas funcionan a `DISPATCH_LEVEL`. Las pilas tienen límites: un puntero escapado no puede entrar en la pila de otro worker bloqueado. Los temporizadores armados en la extensión impiden retirar prematuramente el dispositivo. Estas comprobaciones no exponen cambios generales de IRQL.

`KernelDeviceStackTests.cpp` verifica propiedad/conexión independientes, selección del extremo superior, atomicidad del fallo, capacidad, campos opacos, handles, retención de trabajo/solicitudes tras desconectar/eliminar e identidades de archivo/despacho. El original `driver_wdm_stack.c` usa encabezados WDK auténticos y Copy/Skip/SetCompletion inline; las rutas opcionales `NEVERD_WDM_STACK_FIXTURE`/`NEVERD_WDM_STACK_CFG_FIXTURE` seleccionan imágenes normales/CFG activo. `DriverWDMStackTests.cpp` cubre reubicación, estado inferior, orden/indicadores de finalización, propagación pending tardía, workers/DPC, esperas, `STATUS_MORE_PROCESSING_REQUIRED`, retención de MDL directos, finalización anidada y cursores/controles malformados. `DriverScenarioPublicTests.cpp` cubre reenvío C API/CLI y finalización retenida/anidada C API, incluidas imágenes CFG configuradas. Los artefactos ausentes se omiten explícitamente. Esta evidencia Linux solo demuestra pilas del mismo controlador, no PDO/PnP/energía. `KernelIRPStackTests.cpp` verifica cursores contados, prefijos Copy inline completos, posiciones borradas, propagación de estado/pending, MPR y finalización anidada, propietarios de continuaciones y rutas retenidas. READ/WRITE y el ciclo de archivo reales también usan Copy inline.

`DriverPnpScenarioTests.cpp` comprueba la equivalencia estricta entre JSON y la validación nativa, los datos iniciales explícitos, los límites de ID y cantidad, las combinaciones de campos prohibidas, los estados finales del bus y los informes observados que admiten null. `KernelPnpDeviceTests.cpp`, `KernelPnpRequestTests.cpp` y `KernelPnpCompletionTests.cpp` cubren la propiedad del proveedor, éxito/fallo/fugas de AddDevice, IRP iniciales, admisión de archivos, reversión del ciclo de vida, finalización diferida, continuaciones MPR/anidadas/en espera y atomicidad del fallo. El original `driver_wdm_pnp.c`, compilado con WDK auténtico, utiliza las rutas opcionales `NEVERD_WDM_PNP_FIXTURE` / `NEVERD_WDM_PNP_CFG_FIXTURE`. `DriverWDMPnpTests.cpp` ejecuta imágenes normales y con CFG activo tras reubicarlas: AddDevice, E/S de archivos, eliminación ordenada, inicio/eliminación diferidos, fallos de inicio/query y fallos de AddDevice con limpieza o fugas. `DriverScenarioPublicTests.cpp` también cubre un escenario PnP diferido de siete solicitudes mediante C API y CLI, incluidas las imágenes CFG configuradas. Los artefactos ausentes se omiten explícitamente. Las pruebas de ejecución proceden solo de Linux y demuestran únicamente el subconjunto PnP sin recursos documentado.

Las pruebas del esquema V9 verifican la lectura y escritura de los ocho nombres de funciones menores y comparten la validación del estado final con la finalización del ciclo de vida; QueryStop 0x119 se rechaza antes de cargar la imagen. Las comprobaciones ampliadas del modelo y del fixture auténtico cubren reversión de query-stop, cancel-stop, parada/reinicio, eliminación inesperada, fallos de los contratos de éxito exacto, E/S de software durante la parada o eliminación pendiente, rechazo del invitado tras eliminación inesperada, identidad del dispositivo y resultados mixtos de AddDevice. `DriverScenarioPublicTests.cpp` ejecuta una secuencia de 16 solicitudes de parada/reinicio/eliminación inesperada mediante C API y CLI con fixtures normales/con CFG activo, conservando los bytes del IOCTL de software exitoso, el IOCTL fallido devuelto por el invitado tras la eliminación inesperada y cleanup/close/remove finales. La ejecución pública es secuencial: una IRP retenida sin una fuente de finalización disponible no puede esperar a una solicitud posterior del escenario que inicie o limpie el dispositivo. Las restricciones de vaciado previas a Remove son límites del perfil, no una política general de admisión de E/S en Windows. Las pruebas siguen limitadas a Linux.

`DriverPowerScenarioTests.cpp` comprueba los datos estrictos de los paquetes de energía, la equivalencia JSON/nativa, el contexto opaco de 32 bits, los límites de la FIFO de respuestas y los informes independientes de hijos. `KernelPowerRequestTests.cpp` y `KernelPowerCompletionTests.cpp` verifican el diseño real del paquete, los indicadores de ruta, la distinción entre ciclo de vida y notificación por objeto, la coincidencia FIFO, la propiedad del callback terminal, MPR, esperas y límites de liberación. El original `driver_wdm_power.c`, compilado con WDK auténtico, usa las rutas opcionales `NEVERD_WDM_POWER_FIXTURE` / `NEVERD_WDM_POWER_CFG_FIXTURE`. `DriverWDMPowerTests.cpp` cubre la reubicación normal/con CFG activo, Query/Set directos y anidados, finalización independiente diferida, el orden S0 antes de D0, instantáneas de callbacks de cinco argumentos conservadas durante esperas, hijos iniciados por trabajadores, callbacks null, rechazo de query, valores iniciales/FIFO independientes por PDO y fallos explícitos por datos ausentes. `DriverScenarioPublicTests.cpp` añade validación previa de paquetes de energía malformados y una secuencia de suspensión/reanudación con seis solicitudes de escenario y tres hijas mediante C API y CLI. Los artefactos auténticos ausentes se omiten explícitamente; la evidencia de ejecución sigue limitada a Linux y solo acredita el subconjunto documentado de energía paginable y sin recursos.

`KernelUsbIdleTests.cpp` verifica propiedad, D2, préstamo y primera causa; `KernelUsbIdleBridgeTests.cpp` / `KernelUsbIdleReceiptTests.cpp` IRP reales, cancelación, capacidad compuesta y orden de recepción/finalización; `DriverUsbIdleScenarioTests.cpp` entradas/informes. `DriverWdmUsbIdleTests.cpp` usa el verdadero `driver_wdm_usb_idle.c` mediante `NEVERD_WDM_USB_IDLE_FIXTURE` / `NEVERD_WDM_USB_IDLE_CFG_FIXTURE` para idle, D0/D3, despertar, cancelación, rearme/reinicio, funciones independientes/compuestas y rutas PDO/FDO. `DriverWdmUsbIdlePublicTests.cpp` ejecuta el [escenario USB](../examples/driver-wdm-usb-idle-scenario.json) vía C API/CLI, normal/active-CFG y bases preferidas/reubicadas. Faltantes se omiten explícitamente; evidencia solo Linux, sin demostrar soporte USB KMDF.

`KernelFrameworkUsbIdleTests.cpp`, `KernelFrameworkUsbIdleStorageTests.cpp` y `KernelFrameworkUsbIdleBridgeTests.cpp` verifican política, almacenamiento y planificación tipada. El verdadero `driver_kmdf_usb_idle.c` no envía su propio IRP idle. `DriverKMDFUsbIdleTests.cpp` cubre falta de permiso, E/S administrada con D2/D0 diferidos, StopIdle antes/durante callback, fallo arm, Maximum explícito, despertar y grupos. `DriverKMDFUsbIdlePublicTests.cpp` ejecuta el [escenario KMDF USB](../examples/driver-kmdf-usb-idle-scenario.json) con `NEVERD_KMDF_USB_IDLE_FIXTURE` / `NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`, C API/CLI, normal/active-CFG y bases preferidas/reubicadas. Faltantes se omiten explícitamente; pruebas solo Linux. Las pruebas del modelo verifican que agotar las asignaciones tras armar la activación ejecuta la devolución real de desarmado y cancela WAIT_WAKE sin consumir una respuesta D2.

`DriverKMDFUsbPoFxTests.cpp` cubre SystemManaged/WithHint inicial, dos permisos, cancelación D0, D2/D0 retrasados y confirmación F0 por worker real, StopIdle, despertar antes de READ, fallo arm, eliminación y reinicio. `DriverKMDFUsbPoFxPublicTests.cpp` ejecuta el [escenario USB PoFx](../examples/driver-kmdf-usb-pofx-scenario.json) por C API/CLI, ambos modos, normal/active-CFG y bases preferida/reubicada. `DriverKMDFUsbIdleTests.cpp` conserva regresiones de reenvío y verifica READ directo después de D0Entry. `KernelFrameworkRequestTests.cpp` cubre asignación, propiedad caller-context, colas manuales/detenidas e IRQL. Agotamiento y barreras independientes se prueban en el puente del modelo, no en el controlador real. Artefactos ausentes se omiten explícitamente; evidencia de ejecución solo Linux. `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` cubre además el fallo D0Entry real en RemovePending: confirmación Required exacta y quiescencia permiten limpiar sin F0/ActiveCondition. No demuestra fallos SET_POWER generales ni retirada inesperada autónoma.

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: El [escenario WAIT_WAKE nativo](../examples/driver-wdm-wait-wake-scenario.json) usa el fixture WDK real `driver_wdm_wait_wake.c` mediante `NEVERD_WDM_WAIT_WAKE_FIXTURE` / `NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE`. Comprueba envío durante START, argumentos, activación sin D0 implícito, rearme, cancelación, MPR, cancelación DPC seguida de D0 por worker, captura exacta y proveedores independientes. La evidencia normal/active-CFG con bases preferidas/reubicadas es solo Linux; la ausencia de archivos produce omisión explícita.

`KernelPowerCompletionTests.cpp` cubre APC/DPC, fallo atómico de capacidad y reintento, proveedor solo síncrono/diferido con/sin callback, ruta retenida y MPR. El verdadero `DriverWdmWaitWakeTests.cpp` verifica D0 directo desde cancelación DPC, Query/Set APC/DPC, IRQL/CR8 intactos, retorno antes de ejecución PASSIVE y rechazo de WAIT_WAKE elevado. El [escenario elevado](../examples/driver-wdm-elevated-power-scenario.json) pasa por `DriverWdmWaitWakePublicTests.cpp`, C API/CLI, imágenes normales/active-CFG y bases preferidas/reubicadas; evidencia solo Linux.

`KernelRemoveLocksTests.cpp` comprueba identidades independientes de bloqueo y dispositivo, Tags NULL y repetidos, tamaños retail/DBG exactos, vaciado inmediato y diferido, obligaciones tras una adquisición fallida, atomicidad ante fallos, capacidad y retirada del almacenamiento. `KernelRemoveLockBridgeTests.cpp` y `DriverWDMRemoveLockTests.cpp` cubren la inicialización antes de adjuntar, el almacenamiento opaco de la extensión, los límites de IRQL, la liberación tras retirar el paquete, la finalización del proveedor después del vaciado, la disponibilidad en la última liberación antes de que retorne el callback, trabajadores en espera y limpieza tras un fallo de AddDevice. El original `driver_wdm_remove_lock.c`, compilado con WDK auténtico, tiene variantes retail/DBG y normal/con CFG activo mediante las rutas opcionales `NEVERD_WDM_REMOVE_LOCK_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` y `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`. Las pruebas públicas de C API/CLI usan el esquema PnP existente y preservan observaciones distintas de recepción y finalización del bus y del desmontaje final. Los artefactos ausentes se omiten explícitamente; la evidencia de ejecución se limita a Linux y no acredita Driver Verifier completo ni el vaciado general de solicitudes simultáneas.

`DriverResourceScenarioTests.cpp` comprueba datos explícitos JSON/C++, anchos enteros, cantidades, solapamientos físicos y de registros, alineación, ID, bancos vacíos y serialización de configuración. `KernelMMIOTests.cpp`, `KernelMMIOFailureTests.cpp`, `KernelResourceBridgeTests.cpp` y `UnicornMMIOTests.cpp` cubren la propiedad de bancos/mappings, alias, generaciones, vida de listas empaquetadas, tiempos del proveedor, persistencia al reiniciar, acceso tras retirada inesperada o cambios de energía, transacciones exactas CPU/API y atomicidad de errores. El original `driver_wdm_resources.c`, compilado con WDK auténtico, usa `NEVERD_WDM_RESOURCE_FIXTURE` / `NEVERD_WDM_RESOURCE_CFG_FIXTURE`; `DriverWDMResourceTests.cpp` ejecuta accesores escalares y REP reales, reubicación normal/con CFG activo, alias de subrangos, mappings al final de página, STOP/reinicio y accesos inválidos. Las pruebas C API/CLI rechazan datos inválidos antes de cargar la imagen y ejecutan el mismo escenario de 14 solicitudes con salida IOCTL persistente y recuentos exactos de map/unmap. El archivo compartido [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) necesita el protocolo de registros/IOCTL de este fixture. Los artefactos ausentes se omiten explícitamente; la evidencia se limita a Linux y no usa memoria física del anfitrión ni un backend general de dispositivos.

`DriverInterruptScenarioTests.cpp` cubre descriptores raw/traducidos explícitos, asignaciones mixtas y solo de interrupciones, campos/recuentos estrictos, identidad de origen y observaciones BOOLEAN independientes. `KernelInterruptsTests.cpp`, `KernelInterruptBridgeTests.cpp` y `SchedulerInterruptTests.cpp` cubren coincidencia exclusiva de tuplas, tokens opacos, captura de generación/conexión, vida de eventos, campos Ex seleccionados exactos, restauración del bloqueo común/IRQL, propiedad de callbacks, prioridad ISR en el mismo instante y fallo de capacidad antes de modificar estado. `KernelFrameworkRequestTests.cpp` comprueba vistas previas puras de cancelación y capacidad de tokens por lote sin publicar llamadas ni consumir referencias. El original `driver_wdm_interrupts.c` compilado con WDK auténtico usa `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`; `DriverWDMInterruptTests.cpp` ejercita reubicación normal/con CFG activo, ABI heredado de once argumentos, Ex 1/2/4, finalización real ISR→DPC, FALSE en AL bajo, sincronización/bloqueos manuales, PDO independientes, generaciones tras reinicio y hechos hardware inválidos. Las pruebas C API/CLI rechazan declaraciones inválidas antes de cargar la imagen y ejecutan [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json) de siete solicitudes, comprobando los bytes del IOCTL pendiente y las observaciones de entrega separadas. Las imágenes ausentes se omiten explícitamente; la evidencia sigue limitada a Linux y no establece soporte para interrupciones compartidas/de nivel/MSI ni interrupción a nivel de instrucción.

`DriverDMAScenarioTests.cpp` valida capacidades explícitas, dominios lógicos, límites de bytes/recuentos/tiempo, direcciones estrictas y separación de configuración/observaciones. `KernelPhysicalMemoryTests.cpp` y `BackendBackingTests.cpp` verifican límites de asignaciones en páginas compartidas, referencias fijadas, permisos CPU intactos, exclusión de MMIO/reentrada y atomicidad del fallo del intervalo completo; `KernelRequestMDLTests.cpp` verifica PFN de solo lectura y alias de descriptores construidos con las mismas identidades físicas. `KernelDMATests.cpp`, `KernelDMABridgeTests.cpp` y `SchedulerDMATests.cpp` ejercitan bytes RAM reales, llamadas de tablas vinculadas, propiedad FIFO directa/en cola, vidas separadas de callbacks/mappings, fragmentos de página, direcciones incorrectas, prevalidación de liberación, dominios PDO independientes y fallos de generación/energía. El original WDK `driver_wdm_dma.c` usa `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`; `DriverWDMDMATests.cpp` y las pruebas C API/CLI ejecutan punteros reales, almacenamiento common/SG y eventos DMA/interrupción configurados por separado. El ejemplo compartido [driver-dma-scenario.json](../examples/driver-dma-scenario.json) requiere el protocolo del fixture. Los artefactos ausentes se omiten explícitamente; la evidencia se limita a Linux y no establece DMA real del anfitrión, PCI ni un motor general de dispositivos. `pluginsdk/python/tests/test_driver_dma_integration.py` usa la vinculación existente con JSON de propiedad explícita y `NEVERD_TEST_LIBNEVERD` / `NEVERD_TEST_WDM_DMA_FIXTURE` / `NEVERD_TEST_WDM_DMA_CFG_FIXTURE` para comprobar bytes, orden de callbacks y observaciones de fallos.

`KernelSEHTests.cpp` comprueba planes puros de desenrollado, orden de ámbitos, restauración de registros generales no volátiles, pilas acotadas y metadatos explícitamente no admitidos; `KernelExceptionTests.cpp` comprueba aridad exacta de API, estados de 32 bits bajos, excepciones tipadas, límites IRQL y conservación del estado del modelo/CPU. El original WDK `driver_wdm_seh.c`, con `/GS-`, usa las opciones `NEVERD_WDM_SEH_FIXTURE` / `NEVERD_WDM_SEH_CFG_FIXTURE`; `DriverWDMSEHTests.cpp` ejecuta imágenes normales, con CFG activo y reubicadas con excepciones directas y desde auxiliares, manejadores anidados, nuevas excepciones desde manejadores, filtros reales, finally durante el desenrollado, orden de búsqueda, registros estables, continuación de fallos CPU admitidos y restauración completa del CPU. Los filtros anidados y los finally en colisión usan pilas lógicas enlazadas; los demás fallos CPU se rechazan explícitamente. C API/CLI ejecuta [driver-seh-scenario.json](../examples/driver-seh-scenario.json) y verifica resultados API nulos junto con mensajes reales del manejador invitado. `pluginsdk/python/tests/test_driver_seh_integration.py` usa `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_SEH_FIXTURE` y `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`. Las imágenes externas ausentes se omiten explícitamente; la evidencia sigue limitada a Linux y no establece soporte de búferes de usuario ni SEH general.

`KernelDMAChannelTests.cpp`, `KernelDMAChannelBridgeTests.cpp` y `SchedulerDMATests.cpp` comprueban la FIFO de asignaciones mixtas, anchos de retorno, validaciones puras de admisión/liberación, reutilización de registros, fragmentos contiguos, flush completo, instantáneas CurrentIrp y vidas de paquetes/MDL/dispositivos. El original WDK `driver_wdm_dma_channel.c` usa las rutas opcionales `NEVERD_WDM_DMA_CHANNEL_FIXTURE` / `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`; `DriverWDMDMAChannelTests.cpp` ejecuta versiones normal/CFG activo y reubicadas con MapTransfer/FlushAdapterBuffers reales, cuota common/SG/canal compartida, transacciones explícitas, finalización IRQ/DPC, operaciones secuenciales, dos PDO y fallos. C API/CLI ejecuta las siete solicitudes de [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json), incluida una transacción que abarca ambos fragmentos. `pluginsdk/python/tests/test_driver_dma_channel_integration.py` usa `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE` y `NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` sobre la misma interfaz JSON pública. Los artefactos ausentes se omiten explícitamente; la evidencia Linux no establece controladores DMA del sistema ni patrones HAL arbitrarios de mapeo/flush.

`DriverGuardTests.cpp` y cuatro variantes originales de `driver_guard.c` cubren CFG activo/inactivo, cambio de base de carga, ABI check/dispatch y destinos malformados. `KernelFrameworkTests.cpp`, `KernelFrameworkControlTests.cpp`, `KernelFrameworkQueueTests.cpp` y `KernelFrameworkRequestTests.cpp` cubren vinculaciones, creación transaccional de dispositivos, enrutamiento de colas, longitudes lógicas de búferes y el orden de limpieza y los ciclos de vida de IRP y contextos. Los originales `driver_kmdf_lifecycle.c` y `driver_kmdf_control.c` se compilan opcionalmente con cabeceras genuinas WDK 1.33 y se enlazan mediante la biblioteca real `FxDriverEntry`. Configure las rutas de caché de CMake `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` para las imágenes de ciclo de vida y `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` para las imágenes de dispositivo de control normal y con CFG activo. Los artefactos externos ausentes se omiten explícitamente. `DriverKMDFLifecycleTests.cpp`, `DriverKMDFControlTests.cpp` y los casos C API/CLI de `DriverScenarioPublicTests.cpp` cubren callbacks reales, E/S con búfer y directa, finalización pendiente mediante elementos de trabajo, estados de fallo, descarga y ejecución CFG con una base reubicada. La evidencia sigue limitada a Linux y no demuestra compatibilidad completa con KMDF ni con PnP o administración de energía.

Las pruebas de la API de cancelación anterior mantienen la continuación de la API durante cancelación, limpieza anidada y destrucción final; Ex sigue devolviendo cancelación sin callback si ya estaba cancelada. `KernelFrameworkRequestAccessorTests.cpp` y `KernelRequestMDLTests.cpp` comprueban Information compartida de 64 bits, longitud al completar, identidad de cola/IRP original, identificadores de archivo WDF NULL, getters de identificadores conservados, caché MDL con ByteCount de primera dirección, descriptor directo y mapeo diferido, caducidad y rechazo del bypass WDM. Los modos L, M, D y C del fixture de control real ejecutan la API de cancelación anterior, MDL/información con búfer, MDL READ/WRITE directas y acceso tras completar en imágenes normales/con CFG activo.

Las pruebas de cancelación cubren plazos virtuales solo para transferencias y campos de informe, finalización previa, solicitudes ya canceladas, marcado/desmarcado, permisos de finalización según encolado o entrega, espera de callbacks y referencias internas. Las pruebas del planificador verifican por separado orden DPC/cancelación/trabajo, capacidad, identidades separadas y suspensión/reanudación. La cancelación WDM sigue siendo un error explícito del modelo.


## Distribución de pruebas

`add_neverd_unittest` crea un ejecutable GoogleTest y asigna a cada caso
descubierto una etiqueta CTest igual al nombre de ese objetivo ejecutable.

| Área fuente | Objetivo y etiqueta CTest | Cobertura |
|-------------|---------------------------|-----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Invocación de procesos hijo multiplataforma, quoting, redirecciones y códigos de salida |
| `unittests/libc` | `NeverDLibCTests` | Nombres libc conocidos y clasificación |
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Catálogo de sumideros, precedencia de identidad, prefiltro de argumentos, caza de desbordamiento de copia, auditoría de vida del montón y matriz obligatoria de seis celdas PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/loader` | `NeverDRawISATests` | Archivos binarios: identificación del conjunto de instrucciones a partir de los bytes (datos, el propio código de la prueba, código desplazado dos bytes, codificaciones de 32 frente a 64 bits por familia, archivos que empiezan con ceros) y la tabla de vectores Cortex-M. `scripts/validate_isa_model.py --engine build/bin/libneverd.so` comprueba el modelo con 180 programas y bibliotecas reales que nunca vio, descargados por hash; necesita red y no forma parte de CTest |
| `unittests/lift` | `NeverDLiftTests` | Formas LowIR decoder/lifter, etapas IR, loaders, relocations, fixtures de formato, descompilación y flujos patch representativos |
| La mayoría de `unittests/semantic` | `NeverDSemanticTests` | Semántica diferencial de instrucciones, ABI, control de flujo, expresiones C y lift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadatos hardfork, normalización, ambigüedad ABI/firma, CFG/SSA/recuperación, límites decoder exhaustivos e inputs hostiles, hechos proxy/call, semántica del intérprete, diferenciales LLVM/C/Solidity y API pública |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | Metadatos v0-v4 y diseños ELF, comportamiento estricto de verifier/loader, 23 artefactos ELF fijados, oracle oficial independiente, disponibilidad exhaustiva de opcodes, entradas hostiles, CFG/recuperación y diferencias ejecutadas de LLVM/C/Rust |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Equivalencia de reescritura/ofuscación entre cuatro ISA y tres formatos objeto |
| Archivos de transformación enfocados en `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Sondas rápidas de reenlazar separadas del gran binario semántico |
| `unittests/corpus` (submódulo) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests`, `NeverDAdaDEHCorpusTests` | Metadatos de excepciones y de runtime leídos de 545 binarios reales fijados, cada uno declarado en un manifiesto con los mínimos que su recuperación debe superar |

Las fuentes de registro son
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt) y
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt) y
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt) y
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt).

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
`check-neverd-go-eh-corpus`, `check-neverd-cxx-itanium-eh-corpus`,
`check-neverd-objc-eh-corpus` y `check-neverd-ada-d-eh-corpus` ejecutan una cada uno. Los tres hosts de CI
configuran con la opción y corren las seis líneas: los bytes son idénticos en
todas partes, pero lo que los lee no lo es, y una pasada del corpus en un host
no prueba nada sobre los otros dos. `scripts/audit_ci_test_inventory.py` rechaza
un inventario al que le falte cualquiera de las seis etiquetas, porque una
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

Consulte [Reconstrucción de excepciones de Windows](windows-exception-reconstruction.md)
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
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h).
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

El perfil sanitizer se construye por separado en `build-sbf-asan-ubsan`. El
paquete prebuilt fijado por revisión incluye el header fork-only requerido, así
que integration se ejecuta en el mismo perfil ASan/UBSan fail-fast.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
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

## Rendimiento del inventario de clases Android

Compile `NeverDMobileTests` en Release y ejecute su etiqueta antes de medir
`neverd mobile INPUT --list-classes`. Las pruebas del lector cubren metadatos
dispersos, Unicode, referencias inválidas, cuerpos de métodos no admitidos,
sumas de comprobación y presupuestos; las pruebas de archivos distinguen la
extracción completa de las consultas de cargas seleccionadas. Las pruebas CLI
comprueban filtrado por prefijo, alcance JSON, conservación de salidas y
atomicidad ante fallos multidex.

El generador y banco de medición independientes validan el inventario completo
de descriptores de cada proceso antes de aceptar una muestra de tiempo:

```sh
python3 -m unittest scripts.tests.test_benchmark_mobile_inventory -v
python3 scripts/benchmark_mobile_inventory.py \
  --output-dir /tmp/neverd-inventory-benchmark \
  --class-count 6000 --dex-count 3 --code-units 64 \
  --extra-string-bytes 8388608 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

Use un directorio de salida nuevo en cada ejecución. `--generate-only` escribe
los casos de prueba y su manifiesto sin medir tiempos. `--workload` selecciona
tipos de entrada comunes para un `--peer-command 'tool {input} {prefix}'`
opcional; la preparación de entradas queda fuera del comando cronometrado.
Los informes conservan hashes, comandos, todas las muestras de procesos nuevos,
las hipótesis de caché caliente y, en Linux con GNU time, el RSS máximo de los
procesos hijos. Este RSS no es el pico combinado de una herramienta
multiproceso. Los APK sintéticos son contenedores de consulta, no aplicaciones
instalables. La velocidad del inventario no demuestra la velocidad de búsqueda
de referencias ni la calidad de recuperación Java.

En CPU híbridas, fije el banco y sus hijos a la misma CPU permitida (por ejemplo,
`taskset -c 4 python3 ...` en Linux) para no mezclar núcleos de rendimiento y de
eficiencia. El informe registra la afinidad de CPU heredada.

## Rendimiento de referencias de código Android

Las consultas de referencias comparten límites de instrucciones y validación
de código con el lector de recuperación. Ejecute la suite móvil tras cambiar
este límite. Las pruebas del lector cubren tipos de pools de operandos, modos
de búsqueda, pertenencia de métodos y código compartido, valores engañosos en
cargas/inmediatos, entradas malformadas y límites de recursos. Los flujos de
depuración compartidos se comprueban con el marco, la extensión y los parámetros
de cada cuerpo propietario. Mantenga inventarios grandes de miembros y cuerpos
con muchos saltos en la cobertura de límites de almacenamiento; los índices
persistentes y el crecimiento temporal de contenedores tienen vidas distintas.
Compruebe también elementos reordenados y solapados, código compartido con
prototipos incompatibles del mismo ancho, almacenamiento de entrada no alineado
y subcadenas que cruzan límites de bloques de búsqueda. Las optimizaciones de
datos privados del decodificador deben conservar los modelos completos de
recuperación con datos propios y los multiconjuntos de apariciones de referencias,
incluido el comportamiento de fallo ante metadatos de recuperación no admitidos.
Distinga expectativas emitidas de forma independiente de la coincidencia entre
herramientas sobre entradas reales.

El banco independiente de referencias registra las apariciones esperadas al
emitir instrucciones. En cada ejecución medida comprueba identidades completas
de métodos, PC en unidades de código, opcodes, identidades de destino, unidades
UTF-16 y multiplicidad. También comprueba los contadores de cobertura de NeverD
esperados de forma independiente y `code_scan_complete`:

```sh
NEVERD_REFERENCE_TEST_BINARY="$PWD/build-release/bin/neverd" \
  python3 -m unittest scripts.tests.test_benchmark_mobile_references -v
python3 scripts/benchmark_mobile_references.py \
  --output-dir /tmp/neverd-reference-benchmark \
  --class-count 500 --methods-per-class 64 --matching-methods 2 \
  --dex-count 3 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

Use `--kind` y `--workload` para seleccionar casos. `--extra-strings 65536`
ejercita índices de cadenas reales de 32 bits. Los señuelos de cargas están
activados por defecto; `--no-payload-lookalikes` conserva la distribución y
las referencias verdaderas, sustituyendo los valores señuelo para comparaciones
con entradas comunes. Conserve los resultados de corrección y de tiempos.
Una consulta que devuelve referencias falsas de cargas u omite referencias
reales falla la validación y no obtiene una medida de tiempo aceptada.

El `--peer-command` opcional acepta una plantilla argv con `{input}`, `{kind}`
y `{query}`. Adapte explícitamente la sintaxis de consulta cuando otra herramienta
use una semántica diferente y compare multiconjuntos completos de apariciones.
Se conserva su alcance de validación declarado sin atribuirle un análisis
completo del código. Se aplican las mismas condiciones sobre directorios nuevos,
afinidad de CPU, procesos nuevos, caché caliente y RSS que en el banco de
inventario. La prueba unitaria CLI opcional se omite salvo que
`NEVERD_REFERENCE_TEST_BINARY` indique el ejecutable compilado; informe de
esa omisión.

## Evidencia de exportaciones de los SDK móviles

El flujo manual `Mobile SDK Export Evidence` ejecuta `collect_mobile_ios_sdk_declarations.py --exports-only` con los SDK de Xcode fijados. Conserva sin cambios los mapas del enlazador de Foundation, CoreFoundation y UIKit de los SDK iOS para dispositivo y simulador, con destino, versión del SDK, hash de su configuración, tamaño y SHA-256. El recopilador habitual de declaraciones también conserva estos mapas. Los archivos ausentes, vacíos, demasiado grandes o externos al SDK hacen fallar la recopilación, preservando la evidencia ya completada. Los mapas acreditan las exportaciones de símbolos, pero no una ABI de llamada ni la recuperación de un método.

## Evidencia de ABI de cadenas Swift para dispositivos móviles

El flujo manual `Mobile Swift String ABI Evidence` compila pruebas fijas de igualdad y orden en Swift y una prueba C con `swiftcall`, usando Xcode 26.5 para dispositivos iOS y simuladores arm64. `collect_mobile_swift_string_abi.py` conserva código fuente, LLVM IR, ensamblador, identidad del compilador, configuración del SDK y `libswiftCore.tbd`, con sus hashes. Ambos lenguajes deben mostrar la importación exacta de comparación con cinco argumentos y retorno `i1`; C debe ampliar explícitamente ese resultado a un byte. Los destinos o firmas incorrectos, errores y tiempos agotados conservan la evidencia parcial y hacen fallar la recopilación. Esta evidencia no instala declaraciones de ejecución ni acredita la recuperación de métodos. Pruebe el recopilador sin SDK con `python3 -m unittest scripts.tests.test_mobile_swift_string_abi`.

## Simplificación MBA modular

`SymSimplifyFinite.*` cubre dominios completos de dos valores entre 8 y 512 bits, el coste de usos compartidos, todas las anotaciones admitidas que pueden generar poison, lecturas volatile y freeze independientes, undef/poison explícitos, recorridos iterativos profundos, presupuestos y la marca de ofuscación. El IR original y el simplificado se ejecutan a O0/O2 frente a un oráculo independiente para todos los valores de byte y entradas aleatorias de ancho completo. Los tests de objetos traducidos exigen identidades de caché distintas para presupuestos de valores finitos distintos.

Las pruebas de dominios unidos cubren selects anidados, PHI en diamante y ciclos de copia de 8–512 bits; retornos conflictivos, condiciones indefinidas, componentes sin origen, observaciones PHI/freeze independientes y productores anotados conservados; y límites exactos de nodos, aristas y trabajo. Los oráculos O0/O2 recorren todos los pares de bytes y varían operandos de ancho completo en selecciones, uniones y bucles de estado acotados.

Las pruebas de dos valores añaden máscaras de conjunciones anidadas de 8–512 bits, operandos permutados, rechazo de OR/undef, profundidad acotada, contabilización independiente, marca de ofuscación y presupuesto exacto de la primera reescritura. Los oráculos recorren todos los pares de bytes y varían datos ajenos de 64 bits, comparando IR original y simplificado a O0/O2.

`SymSimplifyPredicates.*` enumera exhaustivamente desplazamientos, signos y entradas de cuatro bits, comprueba composición booleana de intervalos y conjuntos desconectados, y ejecuta oráculos independientes de un byte y ancho completo a O0/O2. Cubre anotaciones poison, entradas indefinidas ocultas en uniones, lecturas/freezes independientes, PHI de bucle conservados, rentabilidad de usos compartidos, trabajo acumulado, muchos usos, límites de recursión y marca de ofuscación. Ambas claves de caché distinguen los presupuestos del análisis de predicados.

`SymExpr.*` comprueba ventanas constantes sobre el bit inferior con todas las entradas y máscaras de cuatro bits, valores anchos, estructuras anidadas, recomposición de bytes conocidos y desconocidos y contraejemplos de acarreo. Las regresiones de presupuesto sitúan un nodo ancho en el límite recursivo y rechazan copiar constantes demasiado grandes. Las ventanas desconocidas deben permanecer simbólicas sin ampliar el DAG. `SymState.*` también distingue constantes escalares deducidas de hechos literales de regiones en ambos órdenes de bytes, sin ampliar el DAG ni cambiar las palabras almacenadas completas.

`SymReadability.*` comprueba la escritura de restas y complementos, el coste de operadores asociativos, literales de un bit y anchos, saturación de árboles compartidos, selección de candidatos con presupuesto y equivalencia exhaustiva de tres bits sin muestreo. `SymMBASample.*` compara la verificación estrecha y de precisión arbitraria con el evaluador AP para todos los operadores, asignaciones deterministas y entradas anchas no usadas. Para comparar la calidad entre versiones de la puntuación, vuelva a contar ambas salidas con la misma métrica; los contadores de tamaño del SDK son solo diagnósticos.

## Matriz de pruebas de ARM32 y reenvío de marco

```sh
cmake --build build-release --target NeverDSymbolicTests \
  NeverDSymSimplifyGuardTests NeverDLiftTests NeverDMBASourceTests \
  NeverDHighCStoreForwardingTests NeverDMetadataJSONTests --parallel 4
build-release/bin/NeverDSymbolicTests
build-release/bin/NeverDSymSimplifyGuardTests
build-release/bin/NeverDLiftTests \
  --gtest_filter='HighSymSimplify.*:HighFrameStoreForwarding.*:ELFARM32ModeTest.*'
build-release/bin/NeverDHighCStoreForwardingTests
build-release/bin/NeverDMBASourceTests
build-release/bin/NeverDMetadataJSONTests --gtest_filter='ELFARM32ModeCAPITest.*'
```

La matriz de spills también cubre x86-32 (ELF/COFF/Mach-O), ARM32 (ARM y Thumb en ELF) y AArch64 (ELF/COFF/Mach-O) con ambos backends de C. Las recargas repetidas del marco privado deben reducirse a suma o resta y ejecutarse correctamente con pares de bytes, pares en límites de palabra y palabras aleatorias deterministas en ambos niveles de optimización. Las comprobaciones del AST de Clang examinan funciones completas para detectar operadores MBA restantes sin confundir expresiones válidas de dirección. HighFrameStoreForwarding comprueba anchos, mutaciones locales, escrituras a memoria, alias, solapamientos, memoria ordenada, grafos malformados y límites de expansión. HighCStoreForwarding mantiene vivas las definiciones usadas por valores almacenados en cuatro arquitecturas, incluidas reinterpretaciones flotantes; SymSimplifyGuard comprueba identidad y orden de cargas, volatile/atomic y límites de poison. ELFARM32ModeTest verifica selección ARM/Thumb, normalización de direcciones, metadatos mixtos y evidencia contradictoria. ELFARM32ModeCAPITest comprueba errores explícitos del SDK y la recuperación del decodificador tras recargar Thumb; InstructionMode cubre decodificador, punteros de código, ramas y generación. La ausencia de Clang para otro destino implica omisión, no prueba de compatibilidad.

`HighBoundPrivateFrameCopies.*`, en `NeverDHighControlFlowTests`, comprueba las copias que pasan por posiciones privadas de la pila sin escape tras vincular las ABI de las llamadas, incluidas las ramas, la reutilización de posiciones y la concordancia entre contextos de guarda. El C generado para x64 y AArch64 se ejecuta con `-O0` y `-O2`, con trampas por comportamiento indefinido y comprobaciones aritméticas independientes. Los casos negativos exigen conservar la función original ante escapes, ABI desconocidas, alias de pila ausentes o incoherentes, reasignaciones de parámetros de entrada, accesos superpuestos, memoria ordenada o atómica, sentencias mal formadas, ciclos o agotamiento del presupuesto. Las conversiones ordinarias no deben convertirse en copias PHI.

`HighIntegerSignedness.*` en `NeverDHighControlFlowTests` comprueba el pase tardío que declara cada variable local de registro o temporal con o sin signo según lo que lea la mayoría de sus usos. La aritmética modular, los desplazamientos lógicos y las comparaciones sin signo favorecen sin signo; las comparaciones, divisiones y desplazamientos aritméticos con signo y la extensión de signo favorecen con signo; una local con algún uso no entero conserva sus tipos. El C emitido se ejecuta con `-O0` y `-O2` con trampas de comportamiento indefinido frente a aritmética de referencia independiente, incluida una comparación con signo de una local que pasó a ser sin signo.

`HighValueForward.*` en `NeverDHighControlFlowTests` comprueba cuándo el escritor HighC puede plegar un valor de un solo uso en su uso. Una condición de bucle conserva un valor cuyas variables asigna el bucle, porque un nombre puede denotar varios valores SSA; una ranura de pila releída conserva su valor a través de una escritura en esa ranura y se pliega más allá de una escritura en otra. Una copia conserva su valor cuando su origen se reasigna antes del uso. Cada caso se ejecuta con `-O0` y `-O2` con trampas de comportamiento indefinido.

`HighCIntegerConversion.*` en `NeverDHighControlFlowTests` comprueba las conversiones enteras que el escritor HighC deja a C. Una conversión dentro de un operando que conserva los bytes que conserva una conversión exterior no imprime un cast propio; una asignación a una variable local entera declarada y un return convierten implícitamente, y un literal se escribe como el valor al que convierte, mientras que un puntero conserva su conversión explícita. Las escrituras en memoria convierten como las asignaciones, y un argumento extendido con ceros para un parámetro tipado más ancho conserva su extensión. Cada caso se ejecuta con `-O0` y `-O2` con trampas de comportamiento indefinido frente a una aritmética de referencia.

La proyección de código vuelve a validar las listas de objetos variádicos tras esta limpieza: admite anclas de instrucciones vacías y rechaza efectos ocultos o transferencias de control. La limpieza sincronizada admite una sola vista `int64_t` o `uint64_t` del mismo receptor guardado; sigue rechazando reducciones de ancho, conversiones flotantes, aritmética de direcciones y reasignaciones. Los conjuntos de objetos Foundation y las trazas de desbloqueo normal y excepcional se ejecutan con `-O0` y `-O2`.

## Excepciones síncronas nativas x64

Los `DIV`/`IDIV` checked x64 usan resultados reales del procesador y `#DE`. KVM utiliza una IDT/IST supervisor privada y WHP un mapa explícito; el contexto original y los códigos disponibles se distinguen de los errores de transporte. El SO consume el evento recuperable antes de instalar la continuación. Los controladores Windows traducen la división por cero y el desbordamiento del cociente a `STATUS_INTEGER_DIVIDE_BY_ZERO`, ejecutando filtros SEH, `__finally` y reintentos reales. `NeverDX64ExceptionTests` se compila sin Unicorn; `DriverWDMCPUException` verifica casos WDK originales. Los hosts ARM64 no disponibles se omiten explícitamente.

## Efectos de RAM preparados

`RAMTransaction` conserva únicamente la unión física de las escrituras declaradas de una instrucción, bajo el bloqueo de ejecución. Restaura la RAM original antes de los observadores de resultados; una cancelación, un error de transporte o una excepción del observador no publica RAM ni registros parciales. Tras revertir la RAM, los fallos de CPU conservan el estado arquitectónico de excepción. Las escrituras simples y dobles de ARM64 usan la misma autoridad. x64 ejecuta `XCHG`, `XADD` y `CMPXCHG` de 8/16/32/64 bits, con alineación natural para formas bloqueadas o con bloqueo implícito. `NeverDRAMTransactionTests` compara resultados con la CPU del host y verifica reversión, alias y permisos; omite explícitamente plataformas no disponibles. Los dispositivos y SMP paralelo siguen fuera del contrato; las instantáneas de CPU no revierten RAM ya confirmada.

`CMPXCHG8B` y `CMPXCHG16B` ejecutan sus instrucciones originales con KVM, WHP y checked Unicorn en los perfiles de controlador y usuario. Tanto el éxito como el fallo de la comparación requieren permisos de lectura y escritura; los fallos de acceso se clasifican como escrituras. `CMPXCHG16B` comprueba la alineación de 16 bytes antes de acceder a memoria y comunica `#GP(0)` si no se cumple. Sus dos observaciones comparten una transacción RAM: detenerse o lanzar una excepción en cualquiera impide publicar registros y RAM. `CMPXCHG8B` sin bloqueo puede cruzar páginas; los operandos bloqueados siguen requiriendo alineación natural. `X64WideAtomicTests.cpp` compara resultados originales del host y fallos nativos directos, y verifica alias, prefijos, direccionamiento, reparación y cancelación. Las fixtures originales de controlador Windows y PE ring3 ejecutan ambos anchos; la fixture WDK también ejecuta `_InterlockedCompareExchange128`. El modelo de CPU debe admitir `CMPXCHG16B`.

## Estado x87 completo

`NeverDEmulationArch` posee los contratos ISA, las tablas de páginas y el formato FP compartido por los transportes nativos y Unicorn. Los contextos x64 conservan control, estado, TOP, etiquetas físicas, código de operación, punteros de instrucciones/datos y ocho registros de 80 bits. `FP0`–`FP7` usan `RegisterValue`; el acceso escalar rechaza el truncamiento. `FPTag` es la máscara física de registros no vacíos. `NeverDX64FPTests` comprueba todos los TOP, operaciones exactas frente a FXSAVE/FXRSTOR del host y restauración. Esto no admite instrucciones x87 en checked ni prueba todos los redondeos. Los hosts nativos no disponibles se omiten explícitamente.

`driver-strict` admite KVM en anfitriones Linux x64 compatibles y WHP en Windows x64 compatibles; `auto` elige ese transporte nativo, y las ISA diferentes usan Unicorn. Unicorn explícito y la API V1 conservan el perfil portátil. La ejecución nativa comprueba direcciones canónicas y efectos antes de entrar; hardware no disponible falla sin alternativa. Instrucciones y comportamiento OS no admitidos fallan explícitamente. La CI nativa de Windows x64 con Unicorn desactivado supera las 359 comprobaciones obligatorias: 131 de CPU, 224 resultados de controladores de 26 imágenes integradas, 46 imágenes WDK y 40 casos de escenarios en las direcciones preferidas y reubicadas, más cuatro comprobaciones de límites SEH ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Faltan pruebas nativas ARM64; esto no establece compatibilidad universal de controladores ni de Android/Darwin.

La validación nativa anterior cubre los puntos de entrada declarados de los controladores y los escenarios publicados. Las regresiones detalladas por función y las comprobaciones C API/CLI/Python descritas a continuación conservan evidencia limitada a Linux salvo que se documente su ejecución en Windows; superar el corpus nativo no valida todas las variantes de prueba en Windows.

Consulte el perfil seleccionado con `executionCapabilities(Contract, ISA, Backend)`. `NativeLegacyX64` describe la ejecución nativa de controladores x64. `NeverDNativeDriverTests` valida el corpus existente y también puede ejecutarse en una compilación sin Unicorn.

El workflow CI existente ejecuta todo el directorio de pruebas de emulación antes de los perfiles generales y guarda el inventario, los resultados JUnit y el registro CTest en `emulation-focused`. Un fallo en otro módulo no impide esta ejecución. El hardware no disponible y los controladores opcionales ausentes siguen siendo omisiones explícitas; una ejecución de software o compilación correcta no demuestra ejecución nativa.

En Linux, `NeverDUnicornDeadlineTests` completa el hilo real del temporizador antes de entrar al invitado mediante planificación pthread controlada. Cubre x64, ARM32 y ARM64, exige que la cancelación previa no produzca efectos y verifica un presupuesto independiente en la siguiente ejecución. Usa API públicas sin modificar el estado privado del motor.

`X64StateTransition` en `NeverDX64ExceptionTests` ejecuta lecturas independientes de RAM y de CR8 en la CPU nativa. Alterna bases TLS y privilegio, reanuda tras fallos de división repetidos y cambia TLS después de una entrada cancelada. Tras modificar la transferencia de estado nativa, ejecute su etiqueta CTest junto con las comprobaciones de alias reasignados, contextos CPU, estado FP y resultados de los controladores originales. Un transporte KVM/WHP no disponible sigue siendo una omisión explícita.


`NeverDKvmRunTests` verifica las transferencias prestadas de `KvmRunControl` sin necesitar `/dev/kvm`. `StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` comprueba que preparación, recogida y entrada del host interceptada comparten un hilo, con una sola preparación durante los reintentos interrumpidos. Otros casos cubren fallo de preparación sin entrada, fallo de recogida, parada durante la preparación y cancelación de una entrada activa, seguidos de una ejecución nueva que no puede reutilizar los callbacks antiguos. Mantenga las suites de cancelación real, reversión de RAM, excepciones y controladores originales en la validación. `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers` verifica que varias entradas con un mismo plazo reutilizan el hilo, ejecutan cada transferencia una sola vez y mantienen intactos los paquetes anteriores.

`KvmHandoffPolicy` limita cada espera activa a 8 μs, pasa a espera bloqueante tras dos intentos fallidos consecutivos y vuelve a intentarlo después de 256 intercambios. El llamador y el trabajador se adaptan de forma independiente; el llamador también respeta el plazo y el token de parada originales. Los indicadores atómicos solo orientan la planificación: el mutex sigue protegiendo los paquetes, la vida de los callbacks y la confirmación de cancelación. `NeverDKvmRunTests` comprueba el límite del sondeo improductivo, la recuperación, los cambios de latencia y la cancelación antes de reutilizar paquetes.

KVM x64/ARM64 usa `KvmRunControl` para preparar estado, entrar en `KVM_RUN` y capturarlo en el mismo trabajador vCPU privado. La preparación ocurre una vez incluso con `EINTR`; cancelaciones y fallos de captura impiden publicar. `KvmAArch64Machine.cpp` realiza mantenimiento de traducciones y transferencias escalares/vectoriales completas con un solo plazo de paso. El llamante publica tras confirmación y conserva decodificación ISA, transacciones RAM, política del SO y observadores. Faltan pruebas nativas ARM64.

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` verifica la continuación y las escrituras reales de CPU tras cambios del host en registros generales, los XMM de ambos extremos, MXCSR y el control x87. Los bytes reales de `FXSAVE64` verifican todos los registros físicos de 80 bits, TOP, etiquetas, opcode y punteros tras una entrada detenida; los fallos de división repetidos también invalidan la reutilización. Estas pruebas de máquina no admiten instrucciones x87 adicionales en los perfiles checked.

`NeverDKvmStateTransferTests` inyecta un fallo de lectura de registros o XSAVE tras una ejecución real de KVM y reintenta con la entrada sin cambios. Los resultados independientes de enteros y bytes empaquetados prueban que una recogida fallida no reutiliza el estado nativo ya avanzado. Solo este ejecutable envuelve `ioctl`; los hosts nativos no disponibles se omiten explícitamente.

`NeverDKvmStateTransferTests` prueba en KVM real conjuntos `KVM_CAP_SYNC_REGS` ausentes, individuales y combinados, además de consultas fallidas. `SynchronizedCapturesRemoveOnlySupportedReadIoctls` cuenta lecturas reales y comprueba todo el estado CPU tras pasos consecutivos. `CancelledWarmEntryRequiresFreshSpecialStateOnRetry` exige volver a leer registros especiales después de cancelar. Los fallos de captura, reintentos de enteros/SIMD, reversión de RAM especulativa y prioridad de excepciones usan la misma matriz; la cobertura nativa no disponible se omite explícitamente.

ARM64 comprobado tiene un límite común para todo el estado. `Registers.def` define 39 campos escalares y 32 vectores de 128 bits; `captureAArch64State` prepara todas las lecturas, aplica anchuras y normaliza NZCV antes de publicar una vez. Unicorn, KVM, WHP y HVF transfieren el mismo inventario, incluidos TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR y FPSR. Los adaptadores nativos habilitan FP/SIMD mediante CPACR_EL1. Cualquier lectura fallida o entrada cancelada conserva todo el estado del llamante.

El inicio ARM64 KVM/WHP/HVF ejecuta el programa privado `AArch64MachineProbe.def`: NOP, suma FP32 redondeada hacia infinito positivo y suma SIMD de dos canales. Cada paso compara los 39 campos escalares y 32 vectores, incluidos TLS, NZCV, los bits superiores borrados del resultado y el estado conservado/acumulado FPCR/FPSR. La prueba usa sólo memoria de supervisor y un plazo global. Las pruebas solo acreditan la inicialización acotada. Sigue pendiente la validación de cargas en Linux ARM64 KVM y Windows ARM64 WHP; los resultados nativos de macOS están en la [guía HVF](macos-hvf.md). El programa también incluye la firma y autenticación A/B de direcciones de retorno con las claves desactivadas, y las cuatro formas de BTI en páginas sin protección.

La prueba ejecuta también dos veces `MRS CTR_EL0`, junto con `DC CVAU`, `DSB ISH`, `IC IVAU` e `ISB`, y verifica la geometría estable de caché y el estado completo. Checked EL0/EL1 admite las instrucciones originales, todas las opciones DSB básicas con nombre y solo ISB SY. CTR procede de la CPU virtual elegida y puede variar entre transportes. Los destinos deben ser RAM ordinaria legible con los permisos actuales; se admiten direcciones no alineadas y alias, y se rechazan los demás como no compatibles. El mantenimiento no genera eventos de lectura/escritura de datos. La proyección mantiene coherente la ejecución sin modelar cachés privadas ni SMP de hardware paralelo. `NeverDAArch64CacheTests` verifica estado, finales de página de solo lectura, rechazos, paradas, contextos, presupuestos y actualizaciones de código invitado mediante alias RW/RX que cruzan páginas. Los hosts KVM/WHP no disponibles se omiten explícitamente.

La inicialización nativa x64 de KVM/WHP/HVF ejecuta `X64MachineProbe.def` en páginas supervisor privadas. Un solo plazo cubre NOP, suma FP32 redondeada hacia infinito positivo, suma SIMD de dos vías y lecturas FS/GS y CS/SS/CR8; cada paso compara todos los estados escalares, XMM, x87 físicos y de control. Las sondas x64 y ARM64 requieren el permiso exclusivo de ejecución de la memoria física. `MemoryProjection` posee la identidad del caché (ISA, espacio de direcciones, generación de mappings, privilegio y variante del monitor) y el historial de raíces confirmadas por ISA. Los constructores invalidan antes de reescribir: un reemplazo fallido no reutiliza tablas parciales y los llamantes no suministran raíces obsoletas. Las pruebas solo acreditan la inicialización acotada. Sigue pendiente la validación de cargas en Linux ARM64 KVM y Windows ARM64 WHP; los resultados nativos de macOS están en la [guía HVF](macos-hvf.md).

El decodificador XSAVE compartido distingue el estado SSE inicial estándar y compacto. Con XSTATE_BV[1] a cero, ambos inicializan XMM; el formato estándar sigue leyendo y validando MXCSR, mientras que el compacto lo inicializa. `X64XsaveCases.def` aporta disposiciones independientes y programas XRSTOR originales para el host. `X64XsaveTests.cpp` verifica el rechazo atómico y compara ambos formatos con ejecución real, conservando el estado FP/SSE del llamador. El oráculo se omite explícitamente si la arquitectura o la función de instrucción requerida no está disponible.

`X64FPState.def` declara disposiciones compactas AVX, AVX-512, CET_U/CET_S y AMX, incluida la alineación de componentes a 64 bytes. Los datos presentes deben ser el estado inicial nulo; los componentes ausentes y el relleno no definen estado. Los bits de disposición determinan los desplazamientos; las disposiciones desconocidas, los datos no iniciales o las longitudes incorrectas fallan antes de publicar. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState` e `InitialWideComponentsDoNotHideFPState` cubren paquetes WHP de 872 y 10752 bytes. Este transporte no admite las instrucciones de dichas extensiones.

`WhpXsaveRegisters.def` complementa los paquetes XSAVE completos con registros de control x87/SSE identificados por nombre. El último opcode y los punteros de instrucción/datos se escriben explícitamente y se leen del host. Los campos nulos pueden completarse; los conflictos no nulos o controles comunes incoherentes fallan antes de publicar. `NamedMetadataRestoresOmittedPacketFields` verifica los campos omitidos conservando toda la carga FP.

Los campos nativos `FOP/FIP/FDP` siguen las reglas x87 del host. AMD puede borrarlos sin una excepción pendiente sin máscara; las instantáneas conservan los valores observados. `X64MachineProbe.def` y las pruebas exactas NOP/contexto preparan una excepción pendiente coherente para comparar cada campo válido sin ocultar diferencias. La referencia FXRSTOR64/FXSAVE64 del proceso host verifica ambos estados; los backends nunca sustituyen los resultados del host por metadatos de entrada.

`NativeGuestRAMDistinguishesEntryFromCaptureLoss` compara FP/SSE completo guardado en RAM por FXSAVE64 invitado con la captura XSAVE del host. Ambas API prueban la instalación directa y FXRSTOR64 invitado, con el ajuste predeterminado de guardado de punteros y el admitido por el host seleccionado explícitamente. Distingue entrada, ejecución y captura sin corregir valores; toda discrepancia sigue siendo un fallo. La matriz también cubre una excepción x87 pendiente sin máscara y registra una referencia FXRSTOR64/FXSAVE64 en el proceso host y el fabricante del procesador, para distinguir el guardado condicional de punteros del transporte WHP.

El códec común `encodeX64XsaveState` / `decodeX64XsaveState` posee los paquetes FP/SSE estándar o compactados, rotación TOP física, estado inicial de componentes ausentes y validación atómica. WHP usa API XSAVE completas, prefiriendo `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`, con las API XSAVE anteriores como ruta de compatibilidad. Los antiguos registros x87 individuales no sustituyen paquetes completos. Los componentes extendidos no iniciales, cabeceras malformadas, controles inválidos y capturas truncadas fallan explícitamente. Los errores de mapping WHP conservan HRESULT, GPA y tamaño para el diagnóstico.

`CheckedX64Instructions.def` admite `MUL` sin signo de 8/16/32/64 bits y `CBW/CWDE/CDQE/CWD/CDQ/CQO` mediante el transporte CPU existente. `NeverDX64IntegerTests` usa codificaciones y valores esperados independientes de `X64IntegerCases.def` en ambos niveles de privilegio: conservación parcial de registros, extensión con ceros de 32 bits, ambas mitades del producto, resultados CF/OF definidos y banderas intactas tras la extensión de signo. La multiplicación en RAM ordinaria mantiene la comprobación de permisos de todo el intervalo y los observadores de lectura; un fallo o una parada del observador conserva los registros de salida implícitos y PC. Los operandos de dispositivo siguen sin admitirse. Los casos también ejecutan checked Unicorn; los transportes nativos no disponibles se omiten explícitamente.

`X64BitInstructions.def` admite `BT/BTS/BTR/BTC` sobre registros y RAM ordinaria de 16/32/64 bits. El índice de registro se interpreta con signo al ancho del operando y selecciona una palabra completa; el inmediato permanece en la palabra base. El truncamiento al ancho de dirección precede a la suma de la base FS/GS. El procesador proporciona CF y los valores escritos; `RAMTransaction` mantiene el resultado privado hasta que los observadores lo aceptan. Los permisos se comprueban en todo el intervalo, incluidas páginas asignadas por separado y alias. Las paradas, los fallos de callbacks y los accesos denegados preservan CPU y RAM. LOCK se limita a modificaciones de memoria con alineación natural; MMIO y SMP de hardware paralelo siguen excluidos. `X64BitStringTests.cpp` compara codificaciones independientes con ejecución x64 real y verifica índices negativos, truncamiento, cruces de página, cancelación y formas LOCK inválidas. Véase la [referencia Intel](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`X64StringInstructions.def` define `MOVS/STOS/LODS` sobre RAM ordinaria de 8/16/32/64 bits; `CLD/STD` solo cambia la dirección. Cada elemento REP valida todo el operando antes de observarlo y confirma sus efectos en un límite de reanudación. Un fallo posterior conserva los elementos completados; una parada o excepción del observador deja intacto el elemento actual. FS/GS solo se suma al origen, después de truncar la dirección. AL/AX conserva los bits altos y EAX extiende con ceros. REP con contador cero y direcciones de 32 bits exige bits altos nulos en el contador y, para MOVS/STOS, en las direcciones utilizadas: los procesadores reales difieren en otro caso. REPNE para MOVS/STOS/LODS y los operandos de dispositivo STOS/LODS siguen excluidos. `X64StringTransferTests.cpp` compara anchuras, dirección, solapamientos y contadores cero con instrucciones independientes del host, y comprueba permisos, alias, retorno de direcciones, fallos y reanudación. El controlador WDK original de recursos ejecuta las cuatro anchuras STOS/LODS mediante `driver_resource_strings.def`.

`X64StringInstructions.def` también define `CMPS/SCAS` sobre RAM ordinaria de 8/16/32/64 bits con `REPE/REPNE`. Cada elemento valida todas las lecturas antes de los observadores, actualiza seis indicadores aritméticos y termina en la primera condición de salida. Un fallo de datos restaura los indicadores al inicio del REP ininterrumpido y conserva los cambios de punteros y contador ya completados; una reanudación pública parte del estado CPU publicado. Las paradas y excepciones de observadores no cambian el elemento actual, y una salida anticipada no lee el siguiente. FS/GS solo afecta a la fuente CMPS; SCAS conserva el acumulador y el registro fuente no utilizado. Se excluyen dispositivos y bits altos ambiguos con contador nulo de 32 bits. `X64StringComparisonTests.cpp` compara instrucciones independientes del host, indicadores, dirección, alias, vuelta de direcciones, permisos y recuperación; su prueba de señales Linux x64 captura los registros de un fallo real. El controlador WDK original ejecuta ambas repeticiones condicionales en las cuatro anchuras mediante `driver_resource_strings.def`. Véase la [referencia Intel](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html). El oráculo nativo Linux comprueba fallos antes y después del primer elemento. Distingue la restauración de flags iniciales de Intel de los flags de la última comparación observados en AMD EPYC 7763 bajo Hyper-V ([observaciones nativas](https://github.com/NeverSight/NeverD/actions/runs/37202522130)); un fabricante de CPU desconocido produce un fallo explícito. Los invitados checked restauran los flags iniciales en todos los backends.

Las CPU WHP existentes comparten una partición nativa; su cierre final y recreación usan el mismo bloqueo de registro. `WhpResourceCache.h` reutiliza el VP 0 cooperativo; al cambiar de CPU lógica, primero retira ese VP y sus mapeos. Las CPU paralelas conservan VP y rangos GPA propios. Cada transferencia de registros, operación XSAVE y cancelación se dirige a su VP. x64 conserva las funciones XSAVE predeterminadas del host y verifica la configuración efectiva con `WHvGetPartitionProperty`. La planificación sigue siendo cooperativa por defecto.

`NeverDX64FPTests` comprueba las 79 posiciones de corrupción y ejecuta instrucciones ensambladas independientemente de `X64ProbeCases.def` en transportes nativos, con plazo único y RAM invitada intacta. `NeverDProjectionCacheTests` cubre llamantes, orden ISA, historial de raíces, variantes privilegio/monitor, generaciones, identidad de espacios y reemplazo fallido. `NeverDRunControlTests` incluye `WhpXsaveTests.cpp` para paquetes API nuevos y antiguos, todos los TOP, tamaños y estado intacto tras errores; estas pruebas de protocolo en memoria no prueban WHP nativo. Los transportes nativos no disponibles se omiten explícitamente.

Los diagnósticos XSAVE distinguen consulta de tamaño, preparación local y decodificación del paquete capturado. Conservan el nombre de API, los bytes devueltos, la capacidad y metadatos limitados de cabecera y control, con expectativas independientes en `WhpHostFailureCases.def`, sin imprimir el contenido de los registros invitados. `InvalidInputReportsPreparationWithoutHostMutation` comprueba además que una entrada rechazada no llama al host ni modifica su paquete. El códec ISA compartido sigue siendo la única autoridad de validación.

Los errores del host WHP durante consultas de capacidades, configuración de particiones/CPU virtuales, transferencia de registros/XSAVE y ejecución conservan el HRESULT y el nombre de API declarado en `WhpProtocol.def`; las consultas fallidas mantienen el resultado tipado de no disponibilidad. `WhpHostFailureCases.def` aporta expectativas independientes para errores del host simultáneos a la cancelación y fallos de consulta, instalación y captura de XSAVE moderno y heredado. El CI específico de Windows exige 210 éxitos nativos: 16 casos de mapeo, dos de inicio, diez FP/contexto, siete CPU compartida, ocho enteros y las dos variantes API de `NativeInstallRetainsFPStateBeforeAnyGuestExecution`. Estas comparan FP/SSE completo y metadatos leídos por separado antes de ejecutar código invitado. Las inscripciones ausentes y las pruebas omitidas, deshabilitadas o no ejecutadas hacen fallar la auditoría de evidencia nativa. Las 26 comprobaciones adicionales cubren todos los casos de `X64BitStringTests.cpp` en ambos niveles de privilegio. Windows PE64 exige 67 casos de procesos WHP y once casos de comparación independientes con Windows nativo.

`NeverDMemoryLifecycleTests` se compila independientemente de Unicorn, incluso en configuraciones exclusivamente nativas. Los casos de proyección y dispositivos específicos del software se omiten explícitamente si Unicorn está desactivado; los casos de CPU que comparten RAM en el host correspondiente siguen registrados. `WhpMemoryTests.cpp` aísla la API de memoria nativa mediante 16 casos de `WhpMemoryCases.def`: tamaño de página o proyección, asignaciones compartidas o independientes, bytes sin tocar o residentes y presencia o ausencia del primer procesador virtual. Cada caso mantiene dos propietarios lógicos, alterna repetidamente su partición mapeada, destruye el propietario inactivo y comprueba que el mapeo restante siga funcionando sin recrearlo. Los errores reales conservan HRESULT y hacen fallar la prueba; esta evidencia de la API de memoria no demuestra ejecución de instrucciones.

`X64MachineProbe.def` identifica la instrucción de inicio fallida y todas las diferencias en escalares, TLS, privilegio, controles x87, lanes FP físicos y palabras XMM, con valores esperados y observados. `DiagnosticIdentifiesStepFieldAndBothValues` verifica mensajes esperados independientes. La comparación sigue siendo exacta; el diagnóstico distingue pérdidas de transferencia de problemas de ejecución sin dar por válida una prueba nativa fallida.

`WhpResourceTests.cpp` cubre reutilización, retirada antes del reemplazo, recuperación tras errores y carreras con el plazo o la detención. `LogicalCPUSwitchingRestoresPhysicalFPAndTLS` alterna dos máquinas en ambos modos de privilegio, verifica estados físicos x87/XMM y FS/GS independientes y reanuda la superviviente tras destruir la otra. La CI de Windows exige ambos casos WHP.

`NEVERD_ENABLE_SEMANTIC_TESTS` vale `ON` de forma predeterminada y controla el grupo de `unittests/semantic` y sus destinos agregados. Para compilar pruebas de CPU nativa sin Unicorn, mantenga `BUILD_TESTING=ON` y configure `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` y `NEVERD_EMULATION_BACKEND_UNICORN=OFF`. Las pruebas nativas KVM/WHP siguen disponibles, incluso con Windows ARM64/MSVC y las cabeceras SDK adecuadas. Activar Unicorn en Windows ARM64 aún requiere una cadena ARM64 LLVM-MinGW. Esta separación de compilación no acredita ejecución nativa ARM64.

El CI de CPU nativa inicializa las fuentes Capstone de la revisión fijada y utiliza el paquete LLVM precompilado verificado. Con `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` y el adaptador Unicorn desactivado, sus pruebas de CPU se configuran, compilan y enlazan sin fuentes Unicorn. Tampoco requieren firmas ni el corpus externo. El CI predeterminado conserva el grupo completo de pruebas semánticas.

El perfil manual `native_cpu_only` de `ci.yml` selecciona Windows x64 con `native_cpu_backend=whp` (predeterminado), o Ubuntu x64 con `native_cpu_backend=kvm`. `NativeCPUTests.def` comparte requisitos de CPU/procesos y declara por separado objetivos y casos específicos del transporte. `run_native_cpu_ci.py --require-whp` o `--require-kvm` valida el host, compila todos los objetivos antes de CTest y conserva inventario, JUnit, registros y recuentos de resultados. La ausencia u omisión de casos obligatorios falla aunque CTest termine correctamente. CI desactiva Unicorn; `--with-drivers` exige el mismo corpus en direcciones originales y reubicadas con el transporte elegido. Compilar y sondear la inicialización no demuestra ejecución invitada ni aceptación ARM64. El perfil Ubuntu usa paquetes Clang/LLD 21 firmados por el proyecto original; las declaraciones CR8 de Clang 18/19 son incompatibles con las cabeceras WDK fijadas. La validación nativa Linux usa CMake 4.2.3. `NeverDNativeDriverTests` selecciona `NO_PRETTY_VALUES` para conservar los nombres de casos declarados en CTest, independientemente de la representación diagnóstica de los parámetros.

Antes de compilar, la CI nativa ejecuta `sccache --zero-stats`. Si la prueba falla, se desactivan los lanzadores de C y C++ y se conservan la configuración de los compiladores y todas las pruebas obligatorias. Los errores de configuración o compilación siguen haciendo fallar el trabajo.

La validación KVM exige cancelar una vCPU real que no sale por sí sola y 48 resultados de transferencia de estado de `KvmStateTransferCases.def`, incluidas capturas mediante ioctl y consultas fallidas de capacidades opcionales. Los otros modos de registros sincronizados se ejecutan si el host los admite; en caso contrario se omiten explícitamente. Los nombres estables de parámetros no dependen de números ioctl ni del formato de tuplas. Las pruebas de protocolo complementan la ejecución nativa sin sustituirla.

`native-host-probe.yml` ejecuta el programa independiente `probe_native_host.py` en runners alojados Linux y Windows x64/ARM64. `NativeHostProbe.def` declara el orden de las pruebas de capacidades, creación de VM/vCPU y liberación. Los informes conservan hashes de fuentes/binarios, ISA nativa y cada código de estado. `setup_ready` solo acredita la preparación; no se ejecutan instrucciones invitadas. La ausencia de capacidades de API/dispositivo produce `unavailable`; los errores de compilación, preparación, liberación, tiempo límite o formato de evidencia hacen fallar el trabajo. La disponibilidad ARM64 debe observarse en cada ejecución; esta prueba no acredita cargas ARM64. Ambos flujos Linux usan `prepare_kvm_ci.py` para conceder acceso al dispositivo de caracteres KVM existente solo a la cuenta del runner alojado y registrar su identidad y permisos. Se rechazan máquinas locales o autoalojadas y no se crean dispositivos ausentes.

`windows-alignment-oracle.yml` usa `check_windows_alignment.py` y `WindowsAlignmentCases.def` para recoger 72 observaciones originales de excepciones Windows x64: nueve formas SSE alineadas, cada una con siete casos de direcciones/permisos no alineados y un control alineado en una página inaccesible. Conserva códigos, parámetros, PC del fallo, contextos guardados, salida original y hashes de fuentes/binarios, y comprueba que las entradas y la RAM no cambian. Solo establece el comportamiento del SO; no certifica la ejecución KVM/WHP ni añade compatibilidad SEH.

Con `native_cpu_only=true`, `native_driver_tests=true` activa `NeverDNativeDriverTests` sin Unicorn. Antes de configurar, `build_wdk_driver_fixtures.py` verifica el SHA-256 completo de los paquetes oficiales Microsoft WDK/SDK 10.0.26100.6584 y recompila 48 imágenes de controladores normales/CFG/DBG desde las fuentes originales. `WDKDriverFixtures.def` declara las identidades de los paquetes, los argumentos del compilador y enlazador y las asociaciones de fixtures. Los archivos Microsoft sin modificar y sus licencias permanecen en los directorios locales de compilación/caché; CI solo publica metadatos y registros de compilación. El manifiesto conserva versiones de herramientas, comandos, hashes de fuentes/cabeceras y hashes de las imágenes producidas.

`NativeDriverTests.def` exige 230 resultados WHP de las 115 cargas de `DriverBuiltinImages.def` y `DriverBackendParityCases.def`: 27 imágenes integradas, 48 imágenes WDK y 40 escenarios, en direcciones originales y reubicadas. El inventario obligatorio completo es `5068 CPU + 230 WHP + 25 SEH + 77 scheduling + 30 wait sets + 11 driver UNPACK + 6 clock reads + 4 image memory checks = 5451`. Las 30 comprobaciones de conjuntos de espera incluyen dieciséis casos de modelo portables y catorce casos de controladores nativos originales. `run_native_cpu_ci.py --with-drivers` conserva identidades exactas y pruebas JUnit con Unicorn desactivado. Fixtures requeridos ausentes u omitidos hacen fallar esta validación opcional; las compilaciones ordinarias mantienen opcionales los fixtures externos. Las imágenes fijas conservan el rechazo esperado de reubicación. La ejecución nativa de invitados ARM64 sigue sin verificarse.

`InterruptionRetainsPhaseCauseDeadlineAndLease` inyecta vencimiento, parada y ambos antes de dos instrucciones iniciales distintas. Comprueba la fase exacta, la duración del mensaje propio, el tipo y los bits de causa, un único plazo invariable y la liberación de memoria. Los fallos reales de transporte y las discrepancias de estado siguen diferenciados. La validación inicial x64 nativa tiene un presupuesto de `5 s`; los plazos del invitado y márgenes de paso único no cambian.

`WhpResourcePolicy.def` concede a la creación de recursos WHP x64 y ARM64 un plazo independiente de `30 s` antes de validar la ISA. La configuración síncrona del host se contrasta con ese plazo antes de publicar el recurso. Las pruebas de instrucciones y los plazos normales del invitado conservan sus límites. `WhpResourceTests.cpp` comprueba interrupciones tipadas de inicialización, diagnóstico de causas, descarte tras cancelación, prioridad del error del host y plazos normales sin cambios.

`X64PopFlagsTests.cpp` comprueba ambos privilegios y `driver-strict`: 256 imágenes permitidas con dos estados iniciales, nueve codificaciones, los 64 bits de entrada, alias de solo lectura o ejecutables, fallos entre páginas y reparación, cancelación/error de observadores, rechazo de pilas de dispositivos y límites de instrucciones nativas posteriores. `X64PopFlagsOracle` ejecuta instrucciones originales de forma independiente en x64 y verifica CPL3/IOPL0 y el consumo exacto de pila. `driver_resource_flags.def` hace que el controlador WDK original establezca, borre y restaure indicadores con ambos anchos. Se conserva todo el estado entero/control/x87/SSE; no se admite TF/NT/AC/ID del invitado ni se demuestra ejecución nativa ARM64.

`X64StatusFlagsTests.cpp` cubre `CLC/STC/CMC`, `LAHF/SAHF`, las 256 entradas AH y combinaciones de flags admitidas, todos los REX, estado completo, memoria intacta, paradas/errores del observador, contextos y continuación nativa ADC/escritura. LOCK inválido se rechaza sin efectos. Un oráculo independiente verifica 24 prefijos tras comprobar CPUID. Los controladores WDK originales ejecutan las cinco instrucciones y añaden 22 resultados nativos obligatorios. Los hosts/ISA no disponibles se omiten explícitamente. El perfil Unicorn portátil ejecuta los mismos siete casos; las pruebas directas de la dependencia cubren AH y LOCK en 16/32/64 bits, registros REX explícitos y rechazo por falta de la función en modo largo.

`X64DoubleShiftTests.cpp` cubre todas las cuentas imm8/CL admitidas, registros compartidos/extendidos, flags definidos, observaciones RAM entre páginas, cancelación, fallos de permisos/mapeo/dispositivo, rechazo de LOCK/cuentas indefinidas, contextos y continuación ADC nativa. Un oráculo independiente verifica 5.184 ejecuciones originales y doce sondas WDK cubren registros y RAM. Se añaden 145 resultados nativos obligatorios.

`X64ScalarShiftTests.cpp` comprueba todos los conteos de un byte, ambas entradas de acarreo, operandos nulos, de todos unos y con signo, formas implícitas de uno, alias AH/SPL y del registro contador, el estado completo de CPU, rangos RAM exactos, reversión de observadores, fallos y continuación de contexto. Un oráculo nativo independiente comprueba 65,536 ejecuciones. El controlador de recursos WDK añade 72 sondas originales. La validación KVM/WHP exige 769 resultados para esta familia. Solo un contador enmascarado igual a cero garantiza conservar todos los indicadores. Una rotación completa no nula del anillo de acarreo `RCL/RCR` conserva el operando y CF, pero deja OF indefinido; el oráculo excluye únicamente ese bit indefinido.

`X64LoopTests.cpp` cubre 21 codificaciones originales, desbordamiento de contadores, destinos relativos con signo por encima de 4 GiB, conservación de todo el estado CPU/RAM, cancelación de observadores, instrucciones entre páginas independientes, fallos al leer el destino y restauración del contexto. Las instrucciones incompletas se rechazan antes de ejecutarse; un fallo al obtener el destino conserva el contador y PC de la rama ya ejecutada. El controlador WDK añade 12 sondas originales. KVM/WHP exigen 379 resultados de esta familia en los contratos supervisor, usuario y controlador. El oráculo de instrucciones originales del anfitrión ejecuta hasta 1,008 casos e informa su número. Las ramas AMD tomadas con `66H` apuntan a memoria baja reservada por el sistema anfitrión; esas formas se ejecutan en la matriz invitada con destinos bajos asignados explícitamente. Se verifican por separado los anchos Intel/AMD y la prioridad de REX.W.

`X64BranchTests.cpp` comprueba las 16 condiciones Jcc y JMP relativo, nueve secuencias de prefijos, formas cortas/cercanas, destinos relativos con signo sobre 4 GiB, estado CPU/RAM completo, paradas/errores de observadores, decodificado entre páginas, fallos de lectura en el destino y restauración de contexto. La referencia Intel independiente ejecuta 9,792 instrucciones originales; las formas AMD con destino bajo permanecen en pruebas invitadas. Ambos modelos comprueban bytes completos/truncados y las sondas nativas verifican fallos de publicación. Cuatro sondas WDK originales ejercitan la política de controladores. Cada validación KVM/WHP añade 274 resultados obligatorios. La cobertura del modelo AMD no acredita ejecución nativa AMD o ARM64.

`X64StackTests.cpp` cubre 42 codificaciones en nueve familias: anchos, direcciones, estado completo, orden de observadores, cancelación, permisos, límites de página, alias físicos, reparación de fallos, rechazo de dispositivos y restauración del contexto. Una referencia independiente ejecuta instrucciones originales en el anfitrión; seis sondas WDK prueban la ruta del controlador. Cada puerta nativa KVM/WHP añade 1135 resultados obligatorios. Los transportes no disponibles se omiten explícitamente fuera de su puerta nativa obligatoria.

`X64FrameExitTests.cpp` cubre 14 codificaciones de `LEAVE`, el orden efectivo de prefijos, el direccionamiento RBP completo, todos los registros, alias de solo lectura, fallos y reparación de marcos entre páginas, privilegios, observadores, direcciones inválidas, rechazo de dispositivos y repetición del contexto. Un oráculo independiente del host ejecuta 42 instrucciones originales; cinco sondas WDK comprueban la ejecución de controladores. Cada validación nativa añade 379 resultados obligatorios.

`X64FrameEntryTests.cpp` cubre 14 codificaciones, anidamiento, solapamiento, alias físicos, comprobaciones de solo escritura, fallos entre páginas y reparación, cancelación, privilegios, rechazo de prefijos y repetición de contexto. Oráculos independientes comparan bytes de pila y registros en 882 ejecuciones correctas y 84 fallos en Linux x64. Dos pruebas de transporte inyectado distinguen la reversión por cancelación/error de la publicación por fallo arquitectónico. Seis sondas WDK ejecutan instrucciones originales en el controlador. Se añaden 508 resultados nativos obligatorios para KVM y 507 para WHP.

`DriverSIMDSEHTests.cpp` ejecuta ocho fallos SSE originales con cuatro disposiciones admitidas y un rechazo de modificación x87, ambos contratos nativos, imágenes WDK normales/CFG y dos direcciones. Son obligatorios diez resultados por backend y tres comprobaciones puras de registros SSE del kernel. `driver_seh_simd.def` define casos y modos; las tablas asíncronas de desenrollado cubren la función que falla. El kernel Microsoft 10.0.26100.9549 aporta evidencia independiente: 107,744 clasificaciones y 8,192 restauraciones mediante rutas de instrucciones aisladas. No demuestra ejecución de controladores en un kernel Windows completo; KVM/WHP nativos en ARM64 siguen sin verificar.

Los intervalos C SEH siguen siendo semiabiertos. Un destino válido de `__C_specific_handler` puede estar dentro de su intervalo protegido: [LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608) emite `EndLabel + 1` como límite final. El modelo Windows conserva esos límites y valida por separado que el destino sea ejecutable, pertenezca a la función y coincida con la continuación, también tras reubicarlo. `KernelSEHContinuationCases.def` conserva la disposición del fixture original; `ScopeEndLabelMayOverlapTheHandlerLandingPad` comprueba manejadores constantes y filtros. Las pruebas complementarias verifican el final exclusivo y el rechazo de destinos inválidos sin consumir el estado de despacho. Estas pruebas puras se ejecutan en `NeverDNativeDriverTests` con Unicorn desactivado.

El desenrollado hacia un destino también usa el final original del intervalo: no se sale de un `finally` cuyo intervalo todavía contiene el destino del manejador. `FinallyRespectsRawScopeEndAtHandlerTarget` prueba ambos lados del límite y, en Windows x64, los compara directamente con `ntdll.dll!__C_specific_handler`. NeverD no repara intervalos generados por el compilador. Los builds Clang 20/21 del fixture original devuelven un fallo del invitado en los modos `T` y `J` porque el final desplazado incluye el destino elegido; Clang 23 ejecuta ambas limpiezas. El [cambio LLVM #144745](https://github.com/llvm/llvm-project/pull/144745) elimina el antiguo sesgo `+1`. Estos resultados del compilador se distinguen de los fallos del backend.

El inventario cubre todos los programas C WDM/KMDF originales y las rutas WDK opcionales de CMake. Cada `driver-*-scenario.json` publicado tiene casos normal y CFG en `DriverBackendParityCases.def`; cualquier vínculo ausente entre fuente, compilación o escenario hace fallar las pruebas del inventario. `Original` elimina la dirección impuesta por el escenario y comprueba la base preferida de la imagen; `Rebased` comprueba la base reubicada declarada. El escenario de IRP propiedad del controlador cancela deliberadamente una solicitud hija, por lo que `DriverNativeOutcomes.def` conserva el resultado global fallido esperado tras la limpieza correcta.

```bash
python3 scripts/build_wdk_driver_fixtures.py \
  --output build-driver-fixtures --cache build-driver-packages
cmake -S . -B build-native -G Ninja \
  -C build-driver-fixtures/fixtures.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
```

`NeverDAArch64StateTests` comprueba la corrupción de cada campo escalar y ambas palabras de cada vector, cambios de privilegio, ausencia de ejecución flotante y conservación de diagnósticos de transporte. `NeverDAArch64FPTests` ejecuta `OriginalProgramChecksCompleteStateAndOneDeadline` en ambos privilegios sobre transportes reales con instrucciones ensambladas independientemente en `AArch64ProbeCases.def`. La prueba traslada estas palabras independientes del PC al código invitado sin permitir acceso de usuario a páginas del monitor. Unicorn y las omisiones nativas explícitas no sustituyen evidencia de inicio ARM64 nativo.

`CheckedAArch64Instructions.def` y `AArch64InstructionEffects` admiten en EL0/EL1 aritmética FP32/FP64 básica acotada, comparaciones, movimientos y SIMD de anchura fija. FPCR conserva cuatro redondeos, FZ y DN; FPSR conserva estado acumulado y QC. Los bits no admitidos se rechazan antes de modificar. FP16 aritmético, SVE/SME, excepciones sin máscara, extensiones opcionales y formas no enumeradas fallan explícitamente. No se añade carga de controladores Windows ARM64 ni otro entorno de SO.

`AArch64InstructionEffects` posee los rangos RAM escalares y FP/SIMD individuales o emparejados, hasta 128 bits por operando. El espacio compartido valida cada página antes de entrar; `RAMTransaction` confirma solo escrituras físicas declaradas completas. El observador de 128 bits recibe dos palabras ordenadas de 64 bits antes de los efectos. Las paradas y faltas preservan RAM, vectores y actualización de dirección. El mismo número Xn/Vn es válido; las parejas que envuelven la dirección se rechazan. `NeverDAArch64MemoryTests` usa los independientes `AArch64CrossPageCases.def` y `AArch64VectorMemoryCases.def`.

`NeverDAArch64StateTests` verifica las 71 lecturas del estado completo en ambos privilegios, anchuras, lectores ausentes y reintentos. `NeverDAArch64FPTests` ejecuta instrucciones originales de `AArch64FPCases.def` para todos los canales vectoriales, aritmética empaquetada, FP escalar/vectorial, cuatro redondeos, FZ/DN, FPSR acumulado, contextos y rechazos. `NeverDAArch64MemoryTests` comprueba cada cruce de página, orden/paradas de observadores, permisos, alias y vectores restaurados. `NeverDUnicornStateTransferTests` (`UnicornStateTransferCases.def`, `CapturesDeclaredWidthsWithoutStaleUpperBits`) inyecta cada fallo de lectura escalar/vectorial tras ejecución real. Los transportes nativos ausentes se omiten explícitamente; estas pruebas no sustituyen ejecución ARM64 KVM/WHP nativa.

`NeverDAArch64MemoryTests` cubre 18 formas escalares/por pares con Unicorn, KVM y WHP en ambos niveles de privilegio: todos los desplazamientos entre páginas, signo y anchura, orden de observadores, segunda página denegada o ausente, consumo explícito del fallo y reintento, alias físicos repetidos y restauración de contexto tras reemplazar alias. La anterior denegación de una carga válida entre páginas se reprodujo antes del cambio. Los transportes no disponibles se omiten explícitamente; Unicorn y la compilación cruzada no sustituyen la evidencia nativa ARM64 KVM/WHP.

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`) comprueba los metadatos CFG inactivos con indicadores a cero, los punteros de reserva intactos en ambas direcciones de carga, las ranuras/destinos inválidos y las reubicaciones ausentes. Sus casos seleccionan explícitamente Unicorn/KVM/WHP con `driver-strict` y `checked-x64-v1`; cada backend no disponible se omite por separado. `DriverPublicCLICases.def` selecciona `--backend unicorn` para comparar la CLI con la API C v1 compatible. La selección nativa y `auto` mantienen pruebas públicas independientes y nunca cambian de backend silenciosamente si la API del anfitrión no está disponible.

Unicorn comprobado usa `MachineRunControl`: un único margen cubre mantenimiento ARM64, ejecución invitada y captura completa del estado. `UC_HOOK_CODE` comprueba el token de parada prestado y el plazo al entrar en la instrucción. La llamada síncrona libera el préstamo del hook antes de volver, pero el paso de máquina conserva el control hasta publicar. Unicorn y WHP preparan todo el estado CPU y comprueban el mismo control antes de publicar un paso correcto. WHP crea el margen una vez antes de preparar. Una excepción CPU x64 autenticada prevalece sobre una parada recibida durante la captura. La transacción RAM comprobada descarta escrituras especulativas si se cancela la captura; el contrato de software no restringido no cambia. `MachineInterruptedError` distingue una cancelación confirmada de un fallo del host o de captura. El CPU comprobado común devuelve `Stopped` o `Deadline`, conserva CPU/RAM y permite reintentar; los fallos reales siguen siendo `BackendFailure` incluso con una parada simultánea.

Regresiones de captura: `NeverDUnicornStateTransferTests`, `NeverDUnicornMachineControlTests`: `StopDuringCaptureCannotPublishAndAllowsRetry`, `ExpiredCaptureCannotPublishAndAllowsRetry`, `CompletedStoreCannotPublishCancelledCapture`, `StopDuringCaptureCannotHideRealGuestException`. `UnicornPublicCapture.CancellationKeepsTypedExitStateAndRAMConsistent`; `NeverDKvmStateTransferTests`: `PublicCancellationRetainsStateRAMAndFailurePriority`.

`NeverDUnicornMachineControlTests` usa las instrucciones de almacenamiento originales de `UnicornMachineControlCases.def` en motores x64 y ARM64 reales con ambos privilegios. `RejectedEntryPreservesStateAndRAMAndAllowsRetry` comprueba cancelación antes del paso, parada o vencimiento en la entrada invitada real, conservación del estado completo y RAM, y un almacenamiento posterior correcto. Su envoltorio de entrada exclusivo de pruebas no requiere hipervisor ni demuestra ejecución nativa ARM64/WHP.

`RunDeadline::invoke` rechaza una entrada WHP detenida o vencida antes de llamar al anfitrión, conserva el resultado real durante la cancelación y confirma que terminaron las devoluciones de interrupción antes de liberar el token prestado. KVM y WHP validan el estado privado completamente capturado en el hilo llamante que posee el permiso de ejecución, antes de clasificar una parada o un vencimiento simultáneos. Los errores reales del anfitrión o de captura y las excepciones autenticadas de la CPU x64 mantienen prioridad. Un estado ordinario exitoso permanece privado hasta finalizar las comprobaciones de cancelación; una interrupción confirmada descarta los efectos especulativos de CPU/RAM y permite reintentar. Preparación, ejecución nativa y captura comparten una sola tolerancia por paso. El control es cooperativo y no garantiza un límite estricto de tiempo real.

`NeverDRunControlTests` incluye los tests portables `NativeEntryTests.cpp` y, en Windows con WHP, `WhpEntryControlTests.cpp`. Las devoluciones del anfitrión en memoria comprueban entrada rechazada, reintento, cancelación tardía, errores conservados, prioridad del resultado y vida confirmada de los callbacks sin Hyper-V. `NeverDKvmRunTests` comprueba finalización en el hilo llamante, prioridad de errores y rechazo de reentrada. Los tests reales `NeverDKvmStateTransferTests` ejecutan instrucciones originales de `KvmStateTransferCases.def`; `ActualCPUExceptionOutranksStopDuringCapture` y `PublicCPUExceptionOutranksStopDuringCapture` detienen tras lecturas reales de registros/XSAVE y conservan la excepción de división, contexto original, RAM y recuperación explícita. Los tests portables con ABI de Windows bajo Wine aportan solo evidencia de hilos y control, sin demostrar WHP nativo. Los transportes nativos no disponibles se omiten explícitamente.

`NeverDInstructionFetchTests` ejecuta programas checked x64/ARM64 con Unicorn, KVM y WHP en modos supervisor y usuario. `InstructionFetchCases.def` cubre cambios de operandos, saltos relativos, escrituras del huésped y del anfitrión mediante alias de código, restauración de contexto, revocación de permisos, páginas con almacenamiento independiente, lectura anticipada al final de página, codificaciones inválidas o truncadas y rechazo de entradas recursivas. La CI nativa de Windows exige que pasen todos los casos WHP x64. Las combinaciones anfitrión/ISA no disponibles se omiten explícitamente; ejecutar ARM64 de forma portable no demuestra soporte ARM64 nativo.

`WhpStateTransferTests.cpp` inyecta transferencias para ambas generaciones XSAVE: grupos modificados, captura completa, relleno ignorado, fallos parciales, cancelación, prioridad de excepciones y reemplazo de partición. `ContinuedStepsReuseCapturedRegistersAndFP` cuenta instalaciones evitadas; `PartialTransferFailuresPreserveStateAndForceFullRetry` exige restauración completa. Son controles de protocolo; siguen siendo necesarias las suites nativas FP, transiciones, controladores y ring3.

`CancelledDirectRunPublishesACompleteBoundary` comprueba el estado completo tras confirmar la cancelación de una ejecución directa. `FailedDirectCapturePreservesStateAndForcesFullRetry` exige conservar el estado del llamador si falla la captura de registros, XSAVE o metadatos durante la cancelación y realizar un reintento completo. Ninguna generación de la API puede publicar un prefijo parcial de registros.

`WhpStateTransferCases.def` también cubre cada prefijo parcial de la captura conjunta de 32 registros y conflictos en los siete campos de metadatos. Ambas generaciones XSAVE deben conservar el estado del llamador y exigir una restauración completa al reintentar. La suite comprueba una sola lectura de registros por paso y la recuperación de metadatos omitidos por XSAVE desde esa lectura.

`windows-pe64-v1` admite procesos de consola Windows x64/ARM64 acotados con PEB/TEB, TLS estático y dinámico, `DllMain`, API Win32 con nombre y grafos DLL explícitos sin ciclos. Los módulos admiten código/datos por nombre u ordinal, DIR64, exportaciones reenviadas e identidades reales del cargador. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary` y `GetProcAddress` usan el catálogo configurado. CRT/GUI, SEH de usuario ARM64 basado en marcos, hilos y compatibilidad general de Windows siguen pendientes; falta evidencia nativa ARM64 KVM/WHP.

Los bytes de entrada y las extensiones acumuladas de imagen comparten cada uno `memory_limit`; los mapas del entorno también consumen el presupuesto de imagen. La preparación comparte 65,536 registros, 64 MiB de lecturas, nombres acotados y el plazo total, sin garantía temporal estricta de E/S del host. El fixture original EXE→DLL→DLL comprueba reubicaciones, ordinales, datos, identidad API, `MEM_IMAGE`, listas y attach/detach TLS del EXE. `NeverDWindowsProcessTests` incluye el oráculo Windows nativo; `NeverDPEProgramExportsTests`, metadatos inválidos y presupuestos; `NeverDProcessPublicTests`, paridad C ABI/CLI. Los transportes no disponibles se omiten explícitamente.

`WindowsProcess.ClockServicesUseConsistentUnitsAndPreserveLastError` ejecuta llamadas PE x64/ARM64 originales a `QueryPerformanceFrequency`, al contador monótono, a FILETIME, a ticks cíclicos y al retraso relativo. `WindowsProcess.UnmodeledDelaysStopWithoutClaimingCompletion` comprueba el rechazo antes de completarse de esperas alertables, fechas absolutas positivas e intervalos INT64_MIN. `NativeWindowsOracleRunsTheSameExecutable` también ejecuta directamente en Windows el escenario temporal correcto; ambas regresiones del invitado son obligatorias en KVM/WHP sin Unicorn. El programa original altera explícitamente los bits no usados del registro `BOOLEAN` y usa constantes de tipo de 64 bits, conservando la época e INT64_MIN bajo la ABI de Windows.

`ARM64 native backend build` compila `NeverDEmulationNative` con Unicorn desactivado en `ubuntu-24.04-arm` (KVM) y `windows-11-arm` (WHP), usando las fuentes fijadas de LLVM. `audit_native_backend_build.py` comprueba cada fuente nativa declarada, la definición activa del backend, el comando de compilación, el objeto ARM64 ELF/COFF y sus hashes. `probe_native_host.py` registra la disponibilidad de la configuración del host y la liberación de recursos; las funciones no disponibles se indican explícitamente y los errores de configuración hacen fallar el trabajo. Esto verifica la compilación y la configuración del host; la ejecución del invitado sigue sin verificarse. `NeverDCapstoneCompilerOptions.inc` limita la opción de diagnóstico de calificadores a las compilaciones C con Clang; GCC y MSVC conservan sus propias reglas de advertencia. `native_arm64_only=true` selecciona estas compilaciones de componentes ARM64 y pruebas de configuración sin el perfil completo de aceptación de CPU x64. Las auditorías normalizan las raíces de fuentes y compilación antes de comparar rutas, incluidos los alias Windows 8.3. La tarea de componentes ARM64 habilita explícitamente el backend KVM o WHP seleccionado antes de la auditoría de compilación.

`WindowsTestExecution.def` selecciona la comparación Unicorn ARM64 `WindowsExclusive` para `RUN_SERIAL`. La política CTest evita la competencia con otras cargas invitadas, conservando el plazo original del invitado de 60 s y todas las comprobaciones de resultados, registros, permisos y resúmenes nativos.

`run_native_cpu_methods.py` valida la propiedad booleana `RUN_SERIAL` de `NativeMethodExecution.def` y la conserva en la agrupación de métodos y los contratos entre ejecuciones. Los métodos se ejecutan de uno en uno; un proceso hijo sin terminar impide iniciar el siguiente. Las propiedades desconocidas y los contratos de fragmentos modificados siguen causando un fallo de validación.

`WindowsProcessLifetime` ejecuta TLS y después `DllMain` de las DLL en orden de dependencia, seguido de TLS y entrada EXE, con una CPU y presupuesto comunes. Cada módulo recibe índice TLS y bloque alineado independientes, copiados de la imagen reubicada y enlazada en un área de 64 KiB. El argumento reservado TLS es cero; `DllMain` recibe un valor opaco no nulo al iniciar/terminar el proceso. La salida explícita separa las DLL inicializadas en orden inverso de la lista del cargador y después TLS EXE, incluso antes de inicializar el EXE. `DllMain(FALSE)` de inicio termina con `0xc0000142` sin detach. Fallos y presupuesto agotado no inventan limpieza. El retorno de entrada PE con DLL invitadas requiere terminación de hilo no soportada y se detiene explícitamente. `SizeOfZeroFill` no nulo sigue excluido; se admiten los bytes inicializados a cero de la plantilla TLS real. Las DLL sin entrada reciben TLS attach, pero no notificaciones de detach del proceso.

`WindowsProcessExports` comparte la resolución por nombre/ordinal entre importaciones estáticas y `GetProcAddress`, incluidos código, datos, alias y cadenas de reenvío. Solo los reenvíos iniciales utilizados añaden módulos del catálogo y dependencias de inicialización; los demás no cargan archivos. Los nombres distinguen mayúsculas; un nombre ausente devuelve NULL/error 127, un ordinal consultado directamente que falta (incluidos huecos) NULL/error 182 y un argumento de consulta NULL error 87, y el éxito conserva LastError. Los handles desconocidos siguen sin admitirse. Las entradas API exactas proveedor/nombre se reservan una vez desde el registro acotado. Se verifican las cabeceras PE y los metadatos de exportación actuales de cada imagen, rechazando cambios o bytes ilegibles; las cadenas se limitan a 64 entradas y comparten el presupuesto restante de metadatos y el plazo de ejecución. Un reenvío a un hueco devuelve la base de la imagen destino y conserva LastError; al ordinal cero devuelve el error 87. La base es una dirección de datos y no concede permiso para ejecutar las cabeceras. Los reenvíos en ejecución pueden cargar módulos configurados e inicializarlos antes de devolver el resultado. Sigue sin admitirse modificar la tabla de exportación activa.

`WindowsProcessLoader` carga nombres base DLL ASCII de `windows.modules` y gestiona referencias explícitas, dependencias compartidas y retención inicial. Repetir consultas reenviadas no añade referencias. Cada recarga asigna una generación residente nueva al mismo espacio del catálogo. TLS y `DllMain` usan la misma CPU por debajo de las tramas API suspendidas; restaurar registros conserva escrituras invitadas y usa el retorno actual. Los punteros reservados del attach/detach dinámico son cero. Un attach fallido durante una carga explícita devuelve 1114 después de limpiar, conservando las cargas anidadas independientes que tuvieron éxito. Descargar libera imagen y TLS; recargar restaura los bytes originales. Se rechazan cambios externos en listas del cargador o punteros TLS. Los presupuestos de archivos, imágenes y metadatos son acumulativos incluso tras fallos. Los proveedores del sistema usan la base PE mapeada como handle. Búsqueda de archivos, rutas no ASCII, opciones `LoadLibraryEx`, ciclos y transiciones reentrantes del mismo módulo mientras se inicializa o descarga siguen sin admitirse.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` comparten el bloque actual del entorno invitado en los parámetros del proceso del PEB. Los nombres ASCII no distinguen mayúsculas; los valores son UTF-16. Las modificaciones validan entradas, capacidad y permisos de escritura antes de publicarse. Las instantáneas son independientes de los cambios posteriores y liberan su memoria invitada. El modelo limita el bloque a 64 KiB; cadenas y expansiones están acotadas y comprueban el plazo. Siguen sin admitirse punteros de propiedad desconocida, bloques malformados, páginas de códigos ANSI y búferes de expansión superpuestos. `WindowsEnvironmentTests.cpp` compara fixtures originales x64/ARM64 en los backends disponibles; CI exige un oráculo Windows nativo independiente.

`WindowsProcessHeap` unifica asignación, `HeapReAlloc`, liberación y consulta del tamaño del heap del proceso. El cambio de tamaño conserva los bytes retenidos; `HEAP_ZERO_MEMORY` pone a cero los bytes añadidos y `HEAP_REALLOC_IN_PLACE_ONLY` impide mover el bloque. Un cambio de tamaño fallido conserva el bloque y devuelve NULL con `ERROR_NOT_ENOUGH_MEMORY` (8), como en las observaciones nativas. Las páginas independientes devuelven capacidad al reducir o liberar; el crecimiento preparado y las copias acotadas comprueban el plazo. Heaps personalizados, indicadores de excepciones, propiedad desconocida y rangos inaccesibles detienen la ejecución explícitamente. `WindowsHeapTests.cpp` cubre ambas ISA, movimiento forzado, reutilización del presupuesto y fallos atómicos; CI ejecuta el mismo EXE original en Windows nativo. Los casos PE por lotes usan el límite CTest común de 120 segundos; cada invitado conserva su presupuesto finito. La fixture del heap permite 20 segundos por proceso para comprobar todos los datos en WHP.

`WindowsSystemModules` construye imágenes modelo PE64 acotadas de `ntdll.dll`, `kernelbase.dll` y `kernel32.dll` para ambas ISA. Las consultas ASCII `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW` y `GetProcAddress` comparten sus bases mapeadas; PEB/LDR y `MEM_IMAGE` describen esas mismas imágenes. Importaciones estáticas, búsquedas por nombre y reenvíos invitados usan los mismos puntos de entrada API y resolución de exportaciones. Los proveedores permanecen residentes, sin callbacks invitados de inicialización, y no impiden retornar desde la entrada tras descargar las DLL invitadas ordinarias. Cambiar cabeceras o metadatos de exportación detiene la búsqueda. Los nombres de sistema no modelados y ordinales no nulos se rechazan explícitamente; diferencias solo de mayúsculas en nombres modelados y nombres vacíos devuelven 127, una consulta NULL devuelve 87. Los bytes y direcciones generados son política del modelo; no se reconstruyen diseños por versión de Windows, ordinales nativos ni alias entre proveedores. `WindowsSystemTests.cpp` compara EXE originales x64/ARM64 con Windows nativo e incluye ocho observaciones independientes del retorno del hilo inicial.

`WindowsSectionFixture.inc` comprueba dos vistas independientes, reutilización de handles, lectura tras cerrar, aislamiento tras escribir, desmapeo y conservación del proveedor residente. Los casos negativos cubren espacios, permisos, direcciones fijas y desplazamientos. `WindowsThreadFixture.inc` separa afinidad y ocultación y verifica longitudes y bits superiores. Los ejecutables originales también se ejecutan en el oráculo Windows nativo; Wine sin `KnownDlls` no valida el caso de sección.

`WindowsProcessExceptions` implementa `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler` y `RaiseException` en la misma CPU y presupuesto del proceso. Los manejadores ordenados pueden modificar registros, generar excepciones anidadas, llamar API modeladas, cargar DLL y terminar el proceso. Las infracciones de datos x64/ARM64 y divisiones enteras x64 reanudan tras validar cambios del invitado en `CONTEXT`, conservando registros generales, SIMD y estado FP admitido. Las excepciones de software continúan mediante una instrucción real de retorno del proveedor modelado. Límites: 128 registros retenidos y 16 marcos anidados. Resultados inválidos, punteros modificados, campos no admitidos y excesos fallan explícitamente. SEH/desenrollado ARM64 basado en pila, depuración y fallos de ejecución/guarda siguen sin soporte. `WindowsExceptionTests.cpp` compara EXE/DLL originales con Windows nativo; aún faltan pruebas nativas ARM64 KVM/WHP. Los registros de excepciones de software incluyen `EXCEPTION_SOFTWARE_ORIGINATE` (`0x80`), independientemente de la marca de no continuación del llamador; el ejecutable original de Windows comprueba los valores exactos de las marcas de las excepciones de software y hardware.

`WindowsProcessContext` conserva el origen de cada marco de despacho. Los fallos x64 admitidos de acceso a datos y división exponen RF (`0x10000`) en `CONTEXT.EFlags`; `RaiseException`, incluso con códigos de violación de acceso por software, conserva el contexto actual. El origen se mantiene durante VEH/VCH y la búsqueda/desenrollado SEH. Una continuación válida restaura las banderas lógicas de CPU sin RF; los cambios de RF del invitado se rechazan antes de publicar el estado. Este perfil limitado no modela puntos de interrupción de instrucciones ni RF controlado por el invitado. `WindowsExceptionTests.cpp` comprueba registros, restauración y CPU/RAM intactas tras un rechazo.

Según observaciones nativas independientes, Windows ring3 convierte los fallos checked x64 `operand_alignment` en `STATUS_ACCESS_VIOLATION` con parámetros `[read, UINT64_MAX]`, también para escrituras. La capa CPU proporciona la causa; Windows no la adivina a partir del vector 13 ni vuelve a decodificar la instrucción. `WindowsAlignmentProcessTests.cpp` ejecuta instrucciones PE originales en 72 escenarios de fallo y 9 reintentos tras corregir la dirección (`72 + 9`), comprobando PC, RF, XMM y RAM. Los fallos sin clasificar o incoherentes se rechazan. Los informes de procesos y controladores conservan `cause` y `error_code` hexadecimal, ambos anulables, distinguiendo ausencia y cero. Esta entrega se aplica al perfil de usuario checked x64. Tras cada fallo o reintento con la dirección corregida, el programa exporta la página completa de 4096 bytes; el anfitrión verifica las 81 capturas y los contadores reales sin cambiar el plazo del invitado. Las observaciones de procesos e hilos iniciales nativos usan `CREATE_DEFAULT_ERROR_MODE`: GoogleTest activa el indicador heredado `SEM_NOALIGNMENTFAULTEXCEPT`, que puede hacer que Windows corrija los fallos medidos. El oráculo observa así el comportamiento predeterminado del sistema, independientemente de la política del entorno de pruebas.

`AddVectoredContinueHandler` y `RemoveVectoredContinueHandler` mantienen una lista ordenada independiente y comparten con los manejadores de excepción el límite de 128 registros retenidos. Cuando un manejador vectorizado acepta continuar, los callbacks de continuación reciben el mismo registro modificable y `CONTEXT`. La validación final ocurre después de estos callbacks, incluidas las excepciones anidadas y las notificaciones DLL. No se pueden retirar identificadores mediante la otra familia de manejadores. `WindowsContinuationTests.cpp` compara EXE originales con Windows nativo para orden, terminación anticipada, cambios de registro, reparación del contexto, anidamiento, callbacks del cargador y salida del proceso. La ruta vectorizada probada en Windows x64 permite continuar con `EXCEPTION_NONCONTINUABLE`; esto no demuestra el comportamiento de SEH basado en pila. La ejecución ARM64 nativa sigue sin verificar.

`RtlCaptureContext` está disponible mediante `kernel32.dll` y `ntdll.dll` para x64 y ARM64. Los componentes compartidos `WindowsProcessContext` e `IntegerABI` guardan PC/SP del llamador sin cambiar el estado de CPU ni LastError. Las observaciones nativas de Windows confirman los indicadores x64 `0x10000f`, la conservación de áreas home/depuración/vectores no escritas y los campos históricos de direcciones x87 de 32 bits; ARM64 copia LR a PC y pone a cero X0/LR en el registro. Registros, SIMD y controles flotantes proceden del invitado; los selectores x64 y la máscara de capacidades MXCSR siguen la CPU invitada configurada. Los destinos inválidos, desalineados o parcialmente inaccesibles fallan antes de publicar datos. `WindowsContextTests.cpp` cubre importaciones directas, consultas a proveedores, callbacks VEH, salidas entre páginas y atomicidad de los fallos. `scripts/check_windows_context.py` ejecuta el programa original en Windows x64/ARM64, con un oráculo nativo separado para el estado x87 no vacío. Estas observaciones ARM64 no prueban ejecución nativa KVM/WHP. Restauración de contexto, recorrido de pila y tablas dinámicas de funciones siguen pendientes. `WindowsProcessServices.def` declara restricciones exactas por módulo: la búsqueda en `kernelbase.dll` devuelve `ERROR_PROC_NOT_FOUND` (127), según las observaciones nativas, sin inventar una exportación. [RtlCaptureContext](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlcapturecontext).

`WindowsProcessSEH` usa el `X64SEH` compartido de `os/windows/exception/` (`NeverDEmulationWindowsException`, disponible sin controladores) para x64 `__C_specific_handler` y UNWIND_INFO V1. Tras buscar VEH, admite filtros, finally, transferencia no local, despacho anidado/desenrollado en colisión y marcos EXE/DLL reubicados, preservando GPR/XMM no volátiles. La continuación por filtro ejecuta VCH con el mismo `CONTEXT`. `WindowsSEHTests.cpp` compara 23 escenarios originales con Windows nativo; KVM/WHP/Unicorn comparten la semántica. El presupuesto del proceso cubre la revalidación de generaciones, cabeceras, bytes de desenrollado/ámbitos, regiones del manejador de lenguaje y enlaces IAT. Los metadatos cambiados o imágenes retenidas descargadas fallan explícitamente. SEH ARM64 por marcos, C++ EH, tablas dinámicas, RtlUnwind/NtContinue generales y desenrollado a través de callbacks del cargador/VEH/VCH siguen sin soporte.

Para `EXCEPTION_NONCONTINUABLE`, un filtro x64 que devuelve `EXCEPTION_CONTINUE_EXECUTION` genera `STATUS_NONCONTINUABLE_EXCEPTION` (`0xc0000025`, indicadores `0x81`, registro enlazado nulo) con un contexto nuevo. VEH se ejecuta de nuevo antes de buscar en la pila lógica conservada, manteniendo el orden finally, la identidad de los marcos EXE/DLL y los mismos presupuestos de profundidad y ejecución. Los 23 escenarios nativos incluyen 21 ejecuciones correctas y dos terminaciones: aceptar en VEH/VCH la continuación de esta excepción secundaria la deja sin gestionar incluso tras restaurar el `CONTEXT` original. El modelo informa de un fallo de ejecución. Las direcciones de excepciones de software coinciden con el PC guardado; las direcciones del despachador interno y la disposición de registros son decisiones del modelo. [Windows x64 CI](https://github.com/NeverSight/NeverD/actions/runs/37141166235).

`WindowsDynamicTests.cpp` compara DLL/EXE originales x64/ARM64 con observaciones independientes de Windows nativo: referencias, dependencias compartidas, cargas anidadas, limpieza tras fallos, reenvíos, salida, DLL sin entrada y TLS nuevo al recargar. Otras regresiones rechazan metadatos alterados y punteros de código caducados, mantienen presupuestos acumulativos y dejan incompletos los resultados API interrumpidos. La CI Windows exige el oráculo nativo y casos WHP; compilar para ARM64 o usar Unicorn no prueba ejecución nativa ARM64.

Una biblioteca ausente en cualquier punto de la cadena de reenvío de `GetProcAddress` devuelve 127; un `LoadLibrary` explícito de un módulo ausente del catálogo devuelve 126. El oráculo nativo y cada backend disponible verifican los 41 escenarios declarados. En Windows, el retorno tras descargar todas las DLL se observa 16 veces por variante de DLL. La inicialización fallida mediante un reenvío de `GetProcAddress` también devuelve 127 tras limpiar. Los callbacks de separación del proceso conservan el contenido de la pila del llamador que termina.

`WindowsExportTests.cpp` usa DLL y EXE originales x64/ARM64 para verificar llamadas reenviadas de código/datos/ordinales, alias, consultas durante inicialización, reubicación, mayúsculas, ausencias, LastError, ciclos, destinos no residentes, punteros inválidos y cambios tras consultas correctas. El mismo EXE tiene un oráculo Windows nativo independiente; los casos WHP son obligatorios en CI nativa. Las pruebas C ABI/CLI comparan informes completos. Sigue pendiente la evidencia de hardware ARM64 nativo. Las variantes EXE con y sin tabla de exportación cubren ambos grafos, el orden PEB y detach, y los errores de nombre/ordinal/NULL.

`WindowsLifetimeTests.cpp` compara trazas fijas con procesos Windows nativos independientes y KVM/WHP/Unicorn: salida normal, retorno de entrada, ambos fallos DLL, cuatro salidas tempranas y DLL sin entrada. También verifica fallos de callbacks, presupuestos comunes, campos TLS reubicados y capacidad total. La prueba nativa de retorno conserva el identificador del hilo inicial y verifica 64 veces su código de salida y la secuencia exacta de notificaciones de hilo/proceso. Los hilos hijos restantes se terminan tras la observación; la salida del proceso no se interpreta como retorno de entrada.

`NeverDUnpackTests`, `NeverDUnpackExecutionTests` y `NeverDUnpackPublicTests` cubren la recuperación de imágenes empaquetadas; véase [desempaquetado](unpack.md). `UnpackGeneratedTests.cpp` comprueba las reglas de entrada en x86-64 y ARM64 con un programa que la propia prueba empaqueta. `X64ReturnPrefixTests.cpp` comprueba el retorno cercano de dos bytes en cada transporte y que cualquier otro retorno con prefijo sigue rechazado. `WindowsDeferredTests.cpp` comprueba las entradas opacas y la observación de un proceso detenido; `ExecutionSessionTests.cpp` comprueba las vigilancias de ejecución. `DirectX64Tests.cpp` comprueba vigilancia parcial de páginas, instrucciones entre páginas, reanudación única, servicios, instrucciones inválidas y estado al vencer el tiempo.

`LiveHeapReferencesCannotPublishAnOrdinaryRecoveredImage`, `ExplicitSnapshotsKeepExternalHeapDependenciesVisible` y `ReleasedHeapStateDoesNotBlockRecovery` comparan inicio y entrada compilados independientemente en los backends verificados y directos disponibles. `HeapReferencesInCapturedTLSCannotBeDiscarded` cubre dependencias solo en TLS. Las pruebas públicas exigen conservar los archivos existentes al rechazar y obtener snapshots idénticos en API C y CLI. Las coincidencias son evidencia conservadora, no un certificado de ejecución nativa.

`DirectServiceBindingsRequireAnExplicitSnapshot` cubre enlaces directos usados por primera vez antes y después de la entrada capturada. C API y CLI también verifican que el rechazo conserve la salida existente.

`PrivateHeapDestructionReleasesOnlyOwnedBlocks` comprueba el propietario tras mover bloques, el rechazo entre heaps, los identificadores retirados y la reutilización de capacidad.

`UnpackLibraryTests.cpp` empaqueta DLL x64/ARM64 independientes en el test y comprueba orden de dependencias, callbacks TLS ordinarios/generados, limpieza tras fallos, identidades entrada/anfitrión, acceso al propio archivo, nombres/ordinales/datos/reenvíos y ausencia de autoimportaciones. Windows nativo carga DLL originales y reconstruidas con un EXE separado y llama a exportaciones declaradas; WHP comprobado y directo son obligatorios. `CompletedGeneratedTLSCallsRequireTheAttachABI` rechaza entradas/argumentos cambiados; `GeneratedCallsNeedTheirReturnedStackAtTheContinuation` rechaza una pila de retorno incorrecta. Se verifica desempaquetado, sin desvirtualización.

`ExportObserver` también observa exportaciones ejecutables de dependencias invitadas residentes; los proveedores modelados siguen observándose en el despacho de servicios. Se excluyen exportaciones de la propia entrada. Los cambios de módulos actualizan las paradas y cada reparación exige identidad actual. Los registros respetan el límite de importaciones declarado. Los tests DLL reparan un helper API y otro de dependencia; la carga nativa verifica que no queden direcciones emuladas.

`WrappedEntriesRequireExplicitTransferEvidence` cubre un wrapper DLL que llama a la entrada restaurada con una pila más profunda. El resultado predeterminado sigue siendo `no_entry`; seleccionar la llamada observada con `transfer` reconstruye una DLL cargable. La profundidad sola no distingue entrada e inicializador.

`WindowsDeferred.EarlierTLSCallbackMayGenerateALaterCallback` exige una sección `.gentls` aislada con `IMAGE_SCN_CNT_UNINITIALIZED_DATA`, el tamaño exacto de los búferes declarados y tamaño y puntero de datos sin procesar nulos. `WindowsDeferredCases.def` define el almacenamiento y el ensamblador; la sección `.data` ordinaria permanece separada. Los casos de callback y entrada generados conservan las comprobaciones de rechazo estricto y ejecución diferida en x64/ARM64.

`ExtendedRegistersLoadOrdinaryImportsAgain` ejecuta cargas R8-R15 compactas y con relleno en x64 comprobado y directo. Los registros bajos cubren un byte anterior similar a REX y rutinas de dirección con CALL solo; las llamadas con relleno saltan bytes arbitrarios tras CALL. `ImportCallHelpersCannotDiscardPersistentEffects` exige conservar efectos persistentes. `PERebuildTests.cpp` rechaza pruebas de inicio/resultado ausentes e inicios solapados y conserva el retorno API exacto en ventanas de seis a ocho bytes.

`OpaqueExportCallsAreRepairedBeforeTheExplicitStop` restaura una llamada pura sin omitir una API desconocida. `ExportObservationIncludesTheOpaqueBoundary` cubre exports estáticos, dinámicos y por ordinal sin cambiar ejecución ni registros de servicios. `OpaqueExportObservationPreservesAnUnreadableReturn` exige mantener ausente el retorno desconocido.

`ExportIdentitySurvivesRebindingAndLateResolution` cambia el orden de enlace de exportaciones opacas y resuelve una exportación después de la entrada. Los casos checked/direct x64 exigen la identidad API correcta y conservan la parada explícita unsupported-service.

La memoria virtual de Windows incorpora `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` y `FlushInstructionCache` para el proceso actual. La capa OS administra las reservas; `AddressSpace` mantiene la autoridad sobre páginas confirmadas, permisos y almacenamiento. Las pruebas cubren cambios de código, fallos de acceso y reutilización del presupuesto de memoria.

`WriteProcessMemory` sigue el comportamiento de páginas confirmadas observado en x64/ARM64 para escrituras de hasta 4 KiB en el proceso actual. Conserva la protección de cada región, los prefijos copiados, los recuentos de bytes y LastError, incluidos `ERROR_NOACCESS`, `ERROR_PARTIAL_COPY` y el éxito tras un prefijo RX. `WindowsMemoryWriteTests.cpp` comprueba las 25 parejas de protecciones; `check_windows_memory_write.py` verifica el mismo ejecutable original en CI de Windows nativo. Los destinos sin confirmar siguen explícitamente sin soporte.

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

Las regresiones cubren presupuestos exactos e insuficientes, palabras parciales, ambos órdenes de bytes, identidad de memoria intacta, límites afines completos, sobrescrituras de predecesores tardíos e invalidación predeterminada. C API/CLI comprueban compatibilidad v6 y dominios inválidos. HighC y LLVMC ejecutados en O0/O2 verifican retorno, memoria, pila y estado preservado, sin certificar equivalencia nativa.

Las regresiones cubren fases en registro y marco, ambos órdenes de bytes, ramas inválidas alcanzables, predecesores tardíos, guardas interiores agotadas y límites de descubrimiento adyacentes. Los límites de visitas se prueban con correlaciones aritméticas, bucles anidados, modos, pasos secuenciales, trabajo total y prioridad anterior. El CLI ejecuta ambas rutas C y ABI fuente a O0/O2; C/Python v8 comprueban disposición, campos inválidos y colas futuras ignoradas. Las regresiones también comprueban que grandes selectores finitos ajenos dejan presupuesto para las condiciones nativas y que el último refinamiento permitido se reserva para un productor ya propuesto.

`NeverDLLVMCPhiTests` ejecuta actualizaciones de bucle independientes y con dependencias cruzadas en O0/O2, con cero iteraciones, límites de iteración y valores iniciales aleatorios de ancho completo. Las comprobaciones de legibilidad exigen cero copias locales en actualizaciones independientes y solo la copia necesaria en intercambios compuestos. Los casos existentes de ramas, switch, ramas movidas e intercambio verifican la arista seleccionada y las asignaciones simultáneas.

Los cinco tests independientes de `LLVMCInternalExitRegions` cubren polaridades, cero iteraciones, intercambios PHI paralelos, cuatro combinaciones anidadas de salidas y orden de observaciones. Destinos comunes y continuaciones tempranas verifican una alternativa ejecutable; una región rechazada tras un bucle válido no debe publicar estructura parcial. La salida de módulo y función coincide sin modificar LLVM. LLVM original y C generado se comparan con oráculos sin signo a O0/O2 en 294.912 llamadas, con trampas de comportamiento indefinido para C.

`NeverDLLVMCPhiTests` también comprueba salidas comunes de bucle con pares PHI distintos en los sucesores, llamadas de observación ordenadas, memoria de salida e IR del llamador sin cambios. El C del módulo completo y de una función seleccionada se compara con referencias independientes en O0/O2. Las comparaciones distintas y los predecesores adicionales comprueban el tratamiento conservador.

`NeverDLLVMCPhiTests` verifica varias aristas de retorno con oráculos O0/O2 independientes, observadores ordenados, instantáneas previas a llamadas que modifican memoria, desbordamiento modular estrecho y extensión de signo. Las salidas de módulo y función conservan el IR del llamador. Cubre aristas en conflicto, raíces compartidas, anotaciones poison, operandos indefinidos, desplazamientos variables, intrínsecos restringidos, excepciones y rechazo completo por falta de presupuesto. Las rotaciones solo se agrupan cuando coinciden todas las operaciones entrantes.

`NeverDLLVMCPhiTests` ejecuta regiones escalares estructuradas en O0/O2: bucles anidados con bloques reordenados, rombos, cero iteraciones, desbordamiento modular estrecho, observadores de cabecera, intercambios PHI, valores externos vivos, pasos compartidos y extremos de desplazamientos de embudo. Comprueba IR original intacto, fusión a tres variables locales y alternativas ejecutables para grafos con varias salidas, irreducibles o demasiado grandes. Son casos sintéticos independientes; la salida de código no certifica recuperación nativa.

`NeverDLLVMCValueTests` compara el C de bucles escalares tipados con LLVM compilado directa e independientemente en O0/O2, con trampas de comportamiento indefinido para el C generado. Los valores límite y entradas deterministas de ancho completo cubren multiplicación estrecha y desbordamiento antes del desplazamiento, multiplicación y desplazamiento ampliados, truncamiento booleano, comparación y extensión con signo, precedencia, expresiones condicionales, aritmética booleana, alternativas para operaciones no admitidas y materialización de expresiones profundas. También comprueba que el IR del llamador no cambie y que desaparezcan las conversiones redundantes.

`NeverDLLVMCPhiTests` y `NeverDLLVMCValueTests` cubren contadores de un byte con incremento/decremento y desbordamiento, salidas fusionadas, usos integrados tras el bucle, valores externos vivos e instantáneas PHI. Las comparaciones independientes O0/O2 con trampas de comportamiento indefinido verifican suma compuesta, rechazo de resta invertida, multiplicación estrecha y máscaras booleanas. Las regiones anidadas requieren contadores locales al bucle, un resultado separado y LLVM fuente intacto. Una regresión ejecutable hace coincidir nombres de funciones externas con los identificadores iniciales de resultado y contador, y comprueba las llamadas y sus efectos observables.

`NeverDLLVMCValueTests` comprueba ambas posiciones de la rama neutra en suma, resta y operaciones de bits, bases sin cambios, dependencias en línea del valor anterior, instantáneas de condiciones, pruebas estrechas, ramas no neutras y selecciones compartidas. El C generado se ejecuta frente a LLVM compilado independientemente en O0/O2, con trampas de comportamiento indefinido. `NeverDLLVMCPhiTests` también comprueba instantáneas paralelas y valores iniciales dependientes de ramas que deben conservar el ámbito compartido. El IR del llamador no cambia.

`NeverDUnicornDecodeTests` comprueba los bits EVEX reservados de las formas de registro en modelos CPU AVX-512/APX, así como la prioridad de los fallos de memoria ROUND, la conservación del estado y la reanudación. Una prueba independiente en Linux x64 confirma los fallos de alineación de la codificación clásica y los fallos de página de las formas escalares/VEX. Estas pruebas del motor no amplían las instrucciones admitidas en modo checked ni demuestran ejecución APX nativa.

## Mediciones de CPU ARM64

Use una compilación Release CPU. HVF explícito requiere macOS ARM64 nativo; active Unicorn para comparar software. Se verifica cada resultado. La inicialización se mide aparte; alternar CPU incluye llamadas API y verificaciones intermedias, las demás cargas excluyen preparación y comprobación.

```bash
cmake --build build-cpu --target neverd-cpu-bench --parallel 4
build-cpu/bin/neverd-cpu-bench --backend hvf --samples 7 --warmup 1
build-cpu/bin/neverd-cpu-bench --backend unicorn --samples 7 --warmup 1
```

Conserve el ejecutable de referencia antes de recompilar. Python 3.11+ alterna el orden. Guarde configuración, etiquetas de fuente, hashes binarios y muestras completas; no compile ni ejecute pruebas en paralelo.

```bash
python3 scripts/benchmark_cpu.py \
  --baseline /path/to/before --baseline-label BEFORE_COMMIT \
  --candidate /path/to/after --candidate-label AFTER_COMMIT \
  --pairs 15 --output /path/to/new-comparison.json
```

El conteo de entradas es un diagnóstico separado, incluye sondeos iniciales y añade sobrecarga; descarte sus tiempos. Estas cargas no representan rendimiento de un OS completo ni comparaciones entre ISA.

[Reproducción](../testing.md#reproduce-checked-arm64-cpu-measurements) · [HVF](macos-hvf.md)

`NeverDLLVMPrivateFrameTests` cubre escrituras solapadas, todas las entradas y retornos de bucle, direcciones conservadas, salidas con alias, memoria no inicializada/ordenada/desconocida, metadatos y rechazo atómico con presupuesto exacto o una unidad insuficiente. Oráculos independientes O0/O2 con trampas de comportamiento indefinido comparan retornos completos, objetos externos y bytes del marco. Compilar para x86-64, AArch64, AArch64 de endian grande y ARM32 no demuestra recuperación nativa. Repetir `NeverDByteMemoryForwardingTests` al cambiar las utilidades comunes de direcciones y efectos.

```sh
cmake --build build-release --target NeverDLLVMPrivateFrameTests NeverDByteMemoryForwardingTests --parallel 4
build-release/bin/NeverDLLVMPrivateFrameTests
build-release/bin/NeverDByteMemoryForwardingTests
```

`NeverDByteCellScalarizationTests` cubre palabras solapadas, dos entradas y dos aristas de retorno, accesos anchos y de anchura no potencia de dos, ambos órdenes de bytes, elección de valores y obligaciones poison, sobrescrituras parciales de poison, rechazo de usos y presupuestos exactos/insuficientes entre objetos. La canalización Thin/Deep normal debe eliminar los arreglos residuales. Oráculos O0/O2 independientes comparan los 24 bytes de salida, las guardas y el retorno de 8.192 entradas en tres versiones: 49.152 llamadas con trampas de comportamiento indefinido. Las compilaciones x86-64, AArch64, AArch64 de orden grande y ARM32 son distintas de la cobertura de ejecución nativa. Ejecutar este objetivo junto con los de reenvío de bytes y marcos privados al cambiar contratos de memoria compartidos.

```sh
cmake --build build-release --target NeverDByteCellScalarizationTests --parallel 4
build-release/bin/NeverDByteCellScalarizationTests
```

`AndroidMutexTests.cpp` usa programas independientes O0/O2 y empaquetado normal/APS2/RELR para tres tipos de mutex, varios participantes, nueva contención, liberación recursiva final, errno, eventos, memoria invalidada, interbloqueos y límites acumulados. Los casos se ejecutan en Unicorn y KVM/WHP/HVF disponibles; los demás se omiten explícitamente. No prueban equivalencia con dispositivos Android ni SMP paralelo.

`HighControlFlowSemantics.DeepStableContainersPreserveEveryReturnPath` comprueba 48 niveles de bloques, bucles, switch y excepciones mediante un intérprete independiente, tanto en rutas recorridas como omitidas. Un límite de tiempo amplio detecta recorridos recursivos repetidos. La cobertura corresponde a HighIR estructurado; recuperar los métodos de toda la imagen sigue exigiendo comprobaciones completas e independientes del inventario y las dependencias.

`SwiftOnceSources.EarlyReturnsKeepExactObjCOnceThunkProofs` cubre thunks ARM64/x64 con llamadas retain combinadas o separadas. `EarlyOnceCopyReturnsRequireTheSameCompleteTail` rechaza escrituras modificadas, retain ausentes o reordenados, cargas ordenadas, resultados cambiados y entradas externas. `IgnoredNestedReturnCopiesDoNotObserveOnceContext` comprueba las dos salidas factibles de un callback void y el flujo fuente tras la proyección.

`HighControlFlowSemantics.ReturnTailCopyKeepsTheOuterLabelOwner` compara rutas de entrada y desvío mediante un intérprete independiente cuando una dirección reaparece en un bloque anidado. `ReturnTailCopyIncludesTheFirstChildOfItsLabel` conserva el caso válido del padre y su primer hijo, junto con la asignación. Estas pruebas específicas no sustituyen las comparaciones completas de métodos y dependencias nativas.

`JumpTailCopyKeepsTheOuterLabelOwner` comprueba la misma regla de pertenencia para los tramos de salto.

`EarlyStringGetterReturnsKeepOnlyInertOnceAnchors` comprueba los anclajes vacíos entre el retorno anticipado y la llamada once, y rechaza las llamadas o escrituras intermedias.

`SwiftOnceSources.ObjCThunkRootsShareTheNestedCallbackProof` comprueba la prueba compartida de la raíz y el rechazo cuando una hoja empieza a observar su contexto.

`SourceABI.SwiftPointActionRequiresTwoDoublesAndContext` / `ObjCCallHints.CoreGraphicsPointActionsKeepSwiftFloatingCarriers` comprueba ambas arquitecturas y rechaza proveedores cambiados, importaciones débiles, almacenamiento en conflicto y portadores ABI obsoletos. `HighCSourceCalls.SwiftCoreGraphicsPointActionsKeepCoordinatesContextAndOrder` ejecuta el C generado en O0/O2 con verificadores independientes de portadores Swift y comprueba bits de coordenadas, incluidos cero con signo, subnormales y NaN, identidad del receptor, orden de llamadas y valores de protección. Esto demuestra el ABI de llamada, no la recuperación completa de métodos superiores.

`NativeSourceHints.CGContextCGRectMethodKeepsOrdinaryAndSwiftContextInputs` verifica el árbol completo y los portadores exactos, incluidos miembros privados y firmas rechazadas. `SwiftFieldReceiver.CGRectMethodSelfKeepsItsLogicalParameterIdentity` y `CGRectMethodRejectsChangedEntryAndReceiverParameter` prueban la canalización y la repetición de publicación, rechazando cambios del índice self, la entrada o el tipo de argumento. Los registros del compilador cubren cuatro destinos macOS/Mac Catalyst; la declaración de entrada sigue limitada a arm64. `HighCSourceCalls.SwiftCGRectMethodKeepsContextReceiverAndAllCoordinateBits` ejecuta el C generado a O0/O2 con un oráculo independiente de portadores escalares Swift, verificando los bits de las cuatro coordenadas, punteros context/self distintos, una sola llamada y guardas de almacenamiento.

`NeverDLowInstructionBoundaryTests` ejecuta las pruebas de procedencia LowIR sin construir todos los fixtures de elevación. `BackwardSharedReturnEpilogueKeepsReturnAndCallerFrame` verifica la liberación alineada mediante ADD y LDP posindexado, incluida la restauración del registro de enlace en el llamador; el RET X30 original y la entrada compartida siguen representados de forma independiente. `BackwardSharedReturnEpilogueRejectsChangedReturnAndOwnership` rechaza otro registro de retorno, BR X30, liberación ausente o desalineada, restauraciones estrechas, entradas interiores, ajustes, asignaciones escribibles o ambiguas, entradas reubicables y otros formatos. Decodificar un final compartido no demuestra una ABI nativa: la ausencia de guardados o asignación en el llamador sigue invalidando la prueba de marco existente.

`NeverDOwnInteriorCallTests` cubre llamadas x86 y x86-64 directas a una etiqueta dentro del propio rango de desenrollado de la función, bajo una entrada `.pdata` de Microsoft x64, una FDE DWARF de System V x86-64 y una FDE DWARF de i386. Una llamada hecha solo por la dirección de retorno que apila se eleva como un apilado y un salto, y el C emitido para los casos x86-64 lineal y en bucle se ejecuta con `-O0` y `-O2` bajo AddressSanitizer y trampas de comportamiento indefinido. Un destino cuyos retornos desapilan la propia dirección de retorno de esa llamada sigue siendo una llamada ordinaria; un retorno tras un cambio de pila o por debajo del puntero de pila de entrada se rechaza. Un rango reconstruido a partir de una cadena de registro i386 no delimita el cuerpo, así que su llamada sigue siendo una llamada.

`NeverDSysVCallContractTests` cubre los contratos de llamada x86-64 System V con las formas de `QDomNode::save` y `QDomNode::isDocument` de QtXml. Un llamado directo cuyo resumen lee un registro de argumento recibe el valor del llamador, incluido un `this` entrante que pasa sin modificar; una llamada virtual toma el objeto que un bloque dominante cargó en `RDI`; un método que retorna sin escribir `RAX` en un camino y en los demás solo pasa resultados de llamados es void; y un byte escrito en `AL` antes de una cadena de comparaciones es el valor devuelto en todos los caminos. Los programas emitidos se ejecutan con `-O0` y `-O2` bajo AddressSanitizer y trampas de comportamiento indefinido.

`ObjCCallHints.CIImageAffineValueKeepsProviderAndPhysicalCopyCarrier` comprueba proveedor CoreImage, fábrica CIImage, registro lógico completo de 48 bytes y puntero x2; rechaza proveedores ausentes o incorrectos, x86_64 y declaraciones incompatibles. `ObjCImageValueCopy.OriginalFrameAndCompleteBodyAuthorizePublication` distingue la llamada original de las asignaciones del resultado en la misma dirección máquina. `RejectsChangedCopyCallBodyAndCurrentImage` rechaza 24 alteraciones de comprobantes, argumentos, escrituras, marcos, metadatos, importaciones, llamadas duplicadas e IR guardado, incluso cambios coherentes en MedIR y HighIR a la vez. `GeneratedCExecutesAgainstIndependentPhysicalCopyABI` ejecuta el C generado sin cambios con O0/O2 en ARM64 contra una función que recibe el puntero x2 observado de forma independiente en el compilador; verifica los seis patrones de bits flotantes, identidad de selector y receptor, una evaluación, objeto devuelto, escrituras legales en la copia, entradas sin cambios y guardas de límites. Otros sistemas omiten esta ejecución de la ABI física.

`ObjCCallHints.CurrentMethodEncodingMustAgreeWithCachedDeclaration` rechaza una ABI almacenada que contradiga la codificación actual no vacía o el selector del método; los clientes que solo aportan una declaración conservan su contrato anterior.

`DarwinIndirectRecordCalls.MatrixFrameEffectsRequireExactCurrentContract` cubre los contratos actuales de matrices y transformaciones y 22 alteraciones rechazadas del contrato. `ObjCAffineImageValueCopy.CurrentProducerInitializesThePublishedCopy` demuestra que el resultado SDK llega a la copia CoreImage y a la reconstrucción independiente para publicación. `RejectsWrongProducerFrameAndSavedIR` comprueba doce alteraciones por productor, incluidas escrituras de entrada ausentes, resultados fuera del marco, proveedores o portadores ABI erróneos y reutilización de una entrada Concat consumida. `GeneratedCMatchesOriginalMachineAndSDKResults` ejecuta el C generado sin modificar y las instrucciones ARM64 originales con CoreGraphics nativo en O0/O2 sobre Apple ARM64: 1000 casos por productor comparan los 48 bytes del resultado, ambas entradas, identidad del selector y receptor, una llamada, objetos devueltos, escrituras privadas y guardas. Otros equipos omiten esta prueba nativa del SDK.

`MatrixFrameEffectsRequireExactCurrentContract` también comprueba el consumidor CGRect y sus 22 alteraciones rechazadas. `ObjCAffineImageValueCopy.CGRectInputUsesTheSameCurrentFrameOwner` verifica rotación → préstamo CGRect → reinicialización por rotación → publicación CoreImage. `CGRectBorrowRejectsExpiredInputsAndChangedABI` rechaza ocho cambios en inicialización, límites, importaciones y portadores. `GeneratedCMatchesOriginalMachineAndSDKResults` ejecuta además esta secuencia completa en O0/O2 con 1000 casos frente a instrucciones originales y SDK nativo, comprobando ángulo guardado, los 48 bytes finales, objetos y guardas.

`FrameMetadataAccessorUsesCurrentCatalogAndABI` comprueba la declaración compartida, los dos portadores de respuesta y la publicación actual del witness del frame. `FrameMetadataAccessorRejectsChangedImportAndBytes` rechaza imports débiles, cambios de proveedor/nombre/addend, solicitudes con direcciones privadas, spills parciales, recargas incorrectas y cambios en las llamadas originales. El oracle del ARM64 original y C generado también llama al accesor real de metadatos URL de Foundation: ambas variantes ejecutan 2048 casos en O0/O2, comprobando selección dinámica de witnesses, todos los bytes de salida, conservación de entradas, número de llamadas y guardas. No demuestra asignación dinámica de pila ni efectos de memoria de witnesses.

`AArch64ExclusiveTests.cpp` verifica anchuras, pares, acquire/release, solapamientos, alias, fallos, instantáneas, cancelación o fallo de observadores y competencia entre CPU. `RAMReservationTests.cpp` cubre escrituras idénticas, ABA, reutilización, reversión e interferencias de cadenas y `ENTER` en KVM/Unicorn. Las muestras Windows ARM64 originales ejecutan bucles exclusivos. `scripts/check_aarch64_exclusives.py` observa instrucciones originales y excepciones de alineación en CI Windows ARM64. Estas observaciones nativas no acreditan la ejecución de los backends ARM64 KVM/WHP; los perfiles no disponibles quedan explícitamente omitidos. Los mismos casos cubren Unicorn de software, interferencias entre contratos, escrituras idénticas y ABA, alias ejecutables dentro de una ejecución, y escrituras `DC ZVA` con cancelación por observadores.
 `windows-alignment-oracle.yml` también ejecuta la prueba ARM64: 1.320 observaciones cubren cada desplazamiento no alineado, cuatro secuencias de carga/almacenamiento y memoria escribible, de solo lectura, inaccesible o entre páginas. Conserva el ancho completo de los valores iniciales de registros y las escrituras parciales anteriores a un fallo. `WindowsExclusiveProcessTests.cpp` compara 1.320 observaciones originales de Windows ARM64 con los resúmenes nativos de `WindowsExclusiveNative.def`. Solo normaliza la ubicación del código y los datos; conserva registros, metadatos de excepción y efectos en RAM.

`AArch64AtomicTests.cpp` cubre 168 codificaciones LSE ensambladas de forma independiente, alias, comparaciones con signo, permisos, cancelación, reservas físicas y transferencias NZCV. `scripts/check_aarch64_atomics.py` recopila 1.100 registros originales de Windows ARM64 con resultados, contexto de excepción y huella RAM completos; el analizador rechaza pruebas ausentes o incoherentes. KVM/WHP nativos requieren validación aparte. `WindowsAtomicProcessTests.cpp` verifica el proceso checked; `WindowsAtomicResults.def` conserva los resúmenes de los registros completos.

Los destinos nativos estructuralmente constantes usan directamente el planificador existente con comprobación de viabilidad. Un destino simbólico único conserva el predicado de entrada solo tras una enumeración exhaustiva. Las regresiones verifican 128 transferencias constantes con el presupuesto de consultas de una ejecución lineal y 32 transferencias calculadas con dos consultas de enumeración por transferencia, manteniendo libres los bits altos de dirección y los dominios de las ramas. Se rechazan cambios en el estado completo, falta de alineación, límites de destinos nulos y presupuestos insuficientes de consultas o instrucciones. Siguen siendo necesarias las comprobaciones de rechazo de múltiples destinos y enumeraciones incompletas.

Una rama nativa conserva el dominio de entrada en una arista solo después de completar una prueba UNSAT que excluya la otra. Las pruebas verifican 32 transferencias condicionales en ambas orientaciones con 512 puertas del solucionador, presupuestos exactos y reducidos en una consulta, y puertas agotadas. Deben rechazarse la alineación modificada o ausente, las comparaciones invertidas y los estados finales alterados; siguen siendo necesarias las pruebas de control indefinido arbitrario y de dos aristas factibles.

Las cachés de traducción a bits y el almacenamiento del recorrido registran solo los nodos y variables alcanzados. `NeverDSolverTests` comprueba identificadores altos y dispersos, crecimiento del contexto entre aserciones incrementales, reutilización de bits almacenados, extracción de modelos y cambios de supuestos. Las expresiones anchas no relacionadas quedan sin codificar; se siguen rechazando las infracciones de anchura alcanzadas, las raíces malformadas y los presupuestos de puertas agotados.

`SourceFrameAnalysis.CallStorage*` comprueba llamadas exactas, definiciones entrantes, inicialización, relleno, escapes, límites y ciclos sin conceder aprobación de fuente. `ObjCFrameBlockBorrows.*` comprueba préstamos síncronos limitados por el descriptor y 19 modificaciones de importación, cabecera, ABI y código máquina. El relleno sigue sin probarse y se rechazan campos de propiedad sin inicializar; la construcción, las lecturas capturadas y el cierre del callback conservan sus comprobaciones de publicación independientes.

Las pruebas de publicación de block/copia también cubren dos rangos separados de 48 bytes, solapamiento del descriptor, cambios en los cuerpos de callbacks, instrucciones e IR obsoletas, llamadas separadas del cuerpo y el orden exacto de proyección. `MixedWidthFrameCopiesMeetEveryInitializedByte` y `FrameCoverageCannotHideMissingBytesOrPointerJoins` comprueban escrituras de 8/16 bytes en ambos órdenes de unión, bytes ausentes, invalidación por préstamos modificables e identidades de puntero que sobreviven a sobrescrituras parciales.

Las pruebas de relación de bucles cubren temporales fijos y variables con iteraciones arbitrarias, offsets separados, planes emparejados, rangos parciales y no alineados, ambos órdenes de bytes y rangos de terminación temporales. La composición nativa mantiene el nuevo almacenamiento solo en el candidato. Prefijos ausentes, bytes no declarados o no definidos, proyecciones erróneas, asignaciones omitidas, comportamiento modificado y definiciones incompatibles impiden certificados. Los presupuestos exactos de ejecución, consultas y observaciones pasan; una unidad menos falla. La inferencia no amplía el presupuesto de la prueba posterior. Las duraciones siguen ligadas al resumen, sin establecer una ABI nativa ordinaria.

Las pruebas de bucles nativos cubren la recopilación condicional diferida y las fronteras de rechazo sin auditar con un número arbitrario de iteraciones. Los planes manuales e inferidos vuelven a comprobar todo el dominio de entrada e inducción; ramas inválidas alcanzables, actualizaciones nativas modificadas y presupuestos de consultas o instrucciones agotados rechazan certificados. Se vinculan los bytes modificados de fronteras inalcanzables, se conservan los valores estrictos y el rechazo de planes mal formados, y se verifican ambos testigos, opciones combinadas y rechazos de API estáticas e instrucciones solapadas. El esquema semántico 17 vincula esta admisión; ABI nativa ordinaria y composición de fuente siguen siendo obligaciones separadas.

`ObjCSuperGetterSources` cubre getters CGRect de cuatro portadores, diez mutaciones de publicación rechazadas y llamadores Boolean/CGRect que comparten cuerpo máquina. El oráculo ejecutado con O0 y O2 comprueba bits de retorno exactos (incluidos cero negativo, infinito y carga NaN), identidad de receptor/clase y carga del selector después de la llamada de metadatos. Apple ARM64 ejecuta el thunk original y el C generado; otros equipos ejecutan el C generado con su ABI nativa de registros compuestos.

`LowIRLoopInference` cubre contadores proyectados de 8, 24 y 32 bits con bits superiores iniciales arbitrarios, ambas direcciones, registros, marcos, temporales de función y ambos órdenes de bytes. Las autopruebas completas pasan; se rechazan cambios de resultado, estancamiento, desbordamiento estrecho y saltos sobre salidas por igualdad. Los presupuestos exactos de operaciones, consultas, caminos, candidatos de rango y ensanchamientos pasan; una unidad menos falla. Los límites de operaciones, consultas y observaciones de la prueba final se comprueban aparte.

`ObjCCallHints.SDKRecordData*` comprueba ambos registros externos, todos los desplazamientos double, las dos arquitecturas Darwin y alias del proveedor, además de importaciones modificadas o débiles, bibliotecas ausentes, correcciones incompatibles, almacenamiento escribible y rangos incompletos. `python3 -m unittest scripts.tests.test_generate_darwin_record_data_declarations scripts.tests.test_generate_darwin_data_declarations` comprueba conflictos de perfiles, disposiciones alternativas, tamaños y alineaciones inválidos, TLS y exportaciones por arquitectura. Reproduzca el catálogo con `generate_darwin_record_data_declarations.py`, el SDK y libclang fijados, la ruta de salida y `--check`; esta comprobación de declaraciones no demuestra la inicialización de resultados nativos indirectos ni la recuperación de métodos.

`LowIRLoopInference.ProjectedBounds*` cubre salidas por igualdad con bits superiores arbitrarios en contadores y cotas, campos de 8/24/32 bits, los tres tipos de almacenamiento y ambos órdenes de bytes. Las pruebas rechazan cotas alteradas, estancamiento, salidas saltadas, desbordamientos y cambios de bytes superiores observados. Los presupuestos de inferencia y prueba siguen separados, con comprobaciones exactas y de una unidad menos.

`LowIRLoopInference.LateCounter*` cubre inicialización constante que solo revela campos de 8/24/32 bits tras generalizar: registros, marcos, temporales de función, ambos órdenes de bytes y bits superiores arbitrarios en las cotas. Las salidas por igualdad pasan la prueba completa; se rechazan estancamiento, salidas saltadas, cotas cambiantes y mutaciones de bytes superiores observados. Se comprueban por separado presupuestos exactos y de una unidad menos para inferencia y prueba final.

`LowIRLoopInference.ProjectedComparisonBits*` comprueba igualdad estrecha almacenada en cabeceras de bucle con bits superiores arbitrarios en las cotas: 8/24/32 bits, tres almacenamientos, ambos órdenes de bytes e inicialización constante o con bits superiores. Las pruebas rechazan cambios de caché, bytes superiores observados y cotas, además de estancamiento y salidas saltadas. Se comprueban presupuestos exactos y de una unidad menos por separado para inferencia y prueba final.

`LowIRLoopInference.OrderedComparisonBits*` cubre ambas codificaciones booleanas de salidas de orden sin signo con contadores de 8/24/32 bits, tres almacenamientos y ambos órdenes de bytes. Las pruebas rechazan actualizaciones no terminantes, comparaciones o cotas alteradas y cambios de bytes superiores observados. Los presupuestos exactos y de una unidad menos son independientes. Las fixtures que cambian la codificación vuelven a vincular el resumen de operaciones originales antes de la prueba.

`LowIRLoopInference.MutablePrefixBounds*` cubre cotas derivadas de 8/24/32 bits con bits superiores cambiantes, tres almacenamientos del contador, ambos órdenes de bytes y salidas directas/almacenadas. Comprueba no terminación, cotas móviles por igualdad, cambios observables de bits superiores/caché y presupuestos exactos o de una unidad menos. Una cota móvil de orden sin signo puede terminar al desbordar y cuenta con su propia prueba completa.

`LowIRLoopInference.SharedTemplatesFitIndependentOperationBudgets` prueba contadores parciales anidados dentro de 1.024 operaciones de inferencia y 640 operaciones de prueba independiente. Las expresiones puras idénticas y las lecturas inmutables del prefijo se comparten solo durante una reconstrucción del punto de corte, conservando la identidad de los operandos, los anchos de salida y los espacios de ubicación. Las pruebas observan las palabras completas de contadores y límites, rechazan un cálculo de prefijo modificado y presupuestos con una operación de menos. Siguen siendo obligatorios los casos existentes de registros, marcos, temporales de función, orden de bytes y no terminación.

`LowIRLoopInference.CompletedEntailments*` comprueba el aislamiento de sesiones entre bucles de frame terminantes y no terminantes en ambos órdenes de bytes, el agotamiento del solver/nodos y los presupuestos independientes de prueba. Las regresiones de límites variables verifican el presupuesto lógico exacto y uno inferior en una unidad con aciertos de caché; las pruebas nativas con contextos repetidos verifican la reutilización dependiente del dominio.

`LowIRLoopInference.IncrementalEntailmentsKeepRollingBudgets` comprueba la reutilización del codificador dentro de un mismo dominio de restricciones. Al cambiar de dominio se descarta el codificador; si se agota la capacidad acumulada de puertas, se reintenta una sola vez con un codificador nuevo y se contabiliza otra consulta. Se observan las palabras completas de contadores y límites en ambos órdenes de bytes, con presupuestos de consultas exactos y una unidad menores, rechazo al superar límites de puertas, anchura o búsqueda, y presupuestos independientes para la prueba final.

`LowIRLoopInference.RebuiltCounterLanes*` cubre actualizaciones proyectadas que coinciden exactamente cuando los demás bits del contador se reconstruyen desde el prefijo o cambian de forma independiente. Los casos de registros, marco y temporales de función abarcan ambos órdenes de bytes, contadores de 1/3/4 bytes y salidas directas o almacenadas en caché, con observación completa de contadores, límites y etiquetas. Deben rechazarse la eliminación de una etiqueta alta observada, las actualizaciones no terminantes, la falta de guardas de salida y los presupuestos de inferencia o prueba inferiores en una unidad. La coincidencia estructural solo propone ampliaciones y rangos; siguen siendo obligatorias las pruebas completas de transición y la prueba final. Los presupuestos fijos de operaciones y consultas también cubren contadores proyectados simbólicos sin convertir coincidencias accidentales del prefijo constante en relaciones adicionales.

`LowIRLoopInference.ProjectedCounterCopies*` cubre bucles anidados que copian una franja del contador mediante una palabra con etiqueta independiente antes de incrementarla. Los 144 casos de estado completo abarcan registros, marcos, temporales de función, ambos órdenes de bytes, franjas de 1/3/4 bytes, salidas directas o almacenadas y controles de copia de palabra completa. Las igualdades proyectadas deben cumplirse en las llegadas guardadas y en toda transición entrante; los bits altos siguen siendo independientes. La implicación bajo el dominio de transición puede reconocer recurrencias aditivas entre parámetros distintos. Se siguen rechazando etiquetas altas eliminadas, actualizaciones inválidas, guardas ausentes y presupuestos de inferencia o prueba final reducidos en una unidad.

`LowIRLoopInference.TransferredCounters*` cubre contadores que cambian de ubicación entre puntos de corte mientras otros bucles pueden omitir cada punto. Las transferencias simbólicas exactas de un paso unitario proponen guardas del contador de origen y un rango alternativo con otra ubicación en un punto; todas las guardas y rangos requieren pruebas completas de transición. Los 192 casos abarcan registros, marcos, temporales de función, ambos órdenes de bytes, contadores de 1/3/4/8 bytes, etiquetas independientes y controles sin traslado. Se rechazan cambios de resultados o etiquetas, mapas de rango inválidos, actualizaciones no terminantes, guardas ausentes y presupuestos insuficientes de inferencia o prueba final. El recorrido usa el límite existente de nodos simbólicos; el presupuesto del selector opcional de grafos puede ser cero. Los casos incluyen decrementos hasta cero e incrementos hasta un límite de entrada. Cuando se descubre un contador nuevo, la eliminación de límites y guardas de segmento espera a que se reconstruyan las plantillas de todos los puntos de corte; continúan el ensanchamiento habitual y la prueba independiente.

`LowIRLoopRefinement.GuardedCuts*` y `BinaryLowIRLoopRefinement.GuardedCuts*` cubren PC repetidos, registros/marco/banderas, ambos órdenes de bytes, rutas no seleccionadas finitas o cíclicas, solapamiento, lados erróneos, generalización, testigos de valores indefinidos, metadatos, resúmenes y presupuestos. Pruebas nativas independientes demuestran ambos contextos R10 en el mismo PC y la prioridad de las fronteras sin auditar. La certificación ABI queda separada.

`BinaryLowIRLoopInference.NativeSelectors*` cubre dos contextos de registro, contextos diferenciados solo por el marco, conjunciones de tres dominios, plantillas inseparables, mutaciones de orígenes y del cuerpo nativo, y presupuestos independientes exactos y una unidad menores. Las iteraciones son arbitrarias y no se añaden constantes de entrada.

`NativeSelectorsGeneralize*` comprueba la recuperación automática y las pruebas nativas completas de fases alternas en registros y marco, incluidas máscaras de byte con bits superiores simbólicos. `NativeSelectorState*` cubre rangos/cuerpos incorrectos, corrupción fuera de la máscara, asignaciones inválidas y presupuestos de trabajo/metadatos exactos o una unidad menores. Los planes explícitos válidos también combinan el constructor real con comprobaciones nativas completas antes y después: cortes solapados y disjuntos, procedencia del prefijo original conservada, coste de los recorridos de respaldo y rechazo de desbordamientos temporales. Esta cobertura se distingue de la inferencia automática.

`DarwinIndirectRecordCalls` comprueba el contrato actual de MakeScale y sus 22 mutaciones de importación/ABI, y consume un resultado privado completo de 48 bytes mediante la prueba compartida de copia por valor. Se rechazan rangos desalineados, desplazados, superpuestos o fuera del marco. Quitar el efecto de escritura completa también se rechaza, aunque se conserve toda la ABI de retorno.

`SourceFrameAnalysis.IncomingResultAddressNeedsCompleteEntryIdentity` rechaza diez mutaciones de entrada, portador o escritura y la ausencia de ABI de entrada. `NativeSourceHints.IndirectResultTailCallRetainsExplicitOutputAddress` vuelve a elevar una llamada de cola directa y comprueba el parámetro de salida explícito, seis escrituras y la puerta de publicación.

`NativeSourceHints.FourDoubleCallerDemandNeedsEveryUnchangedCarrier` comprueba las cuatro vías inferiores, escrituras superiores independientes y nueve mutaciones de declaración/control. `FourDoubleReturnRequiresEveryComputedLowLane` rechaza doce casos de resultado incompleto o contrato obsoleto. `DarwinNativeRecordReturns.FourComputedDoublesExecuteAtO0AndO2` compara los 32 bytes del resultado con un oráculo aritmético independiente en 2048 casos por nivel de optimización.

`DarwinIndirectRecordCalls.AffineInvertSnapshotsItsCompleteAliasedInput` ejecuta 2560 casos tanto en O0 como O2, con entradas/salidas iguales, solapadas o separadas. Comprueba los bits de entrada, una llamada, los 48 bytes del resultado y todo el almacenamiento protegido. Es un oráculo de copia física y captura, no ejecución de la máquina original ni del SDK nativo. La prueba de contratos matriciales/afines actuales conserva 22 mutaciones rechazadas por contrato.

`DarwinIndirectRecordCalls.AffineTranslatePreservesScalarBitsAndSnapshotsAliasedInput` ejecuta 2560 casos en cada nivel O0 y O2. Comprueba los patrones de bits de ambos escalares, los seis campos de entrada, una llamada, todos los bytes de salida y el almacenamiento protegido en posiciones iguales, solapadas y separadas. Rechaza cuatro mutaciones del ABI escalar y las 22 mutaciones compartidas de importación/ABI. El sustituto bit a bit comprueba los argumentos físicos y la copia previa; no es un oráculo matemático de traslación ni ejecuta el código máquina original.

`NativeFloatingReturnProof.HFAResultFieldsNeedTheExactCompleteDefinedCall` cubre nueve selecciones de campos y diecisiete mutaciones rechazadas de llamada, portador, ancho, desplazamiento o SSA. `HFAFieldExtractionNeedsADominatingCall` rechaza un productor en una ruta hermana. `NativeSourceHints.HFAFieldTypeRequiresCurrentCallAndFrameProofForPublication` eleva un llamador ARM64 de cinco instrucciones, vuelve a elevar el resultado escalar inferido y comprueba la puerta de fuente; rechaza otro proveedor o la falta de restauración de LR/SP. Comprueba tipos y proyección de fuente, no la ejecución del cuerpo máquina original.

El desalojo explícito en CPU0, el reloj virtual y los límites se describen en [planificación de controladores](driver-scheduling.md).

`SwiftOnceSources.FoldedObjCGetterTailsExecuteOnceAndRetainsAtO0AndO2` pliega colas de retorno ARM64/x64 reales, con retain combinado o separado y predicados separados o integrados, y ejecuta el C emitido con stubs de runtime a O0/O2. Comprueba los bits del resultado, una sola inicialización, el orden de llamadas y un valor almacenado que cambia. `FoldedObjCGetterTailsRevalidateCurrentStorageAndCalls` rechaza cambios de ancho, orden, intrínsecos, almacenamiento, resultado, llamadas e importaciones actuales incluso con un plan guardado. Son pruebas controladas de código y stubs, no ejecución de la máquina WMF original ni del runtime Swift nativo.

`SourceFrameAnalysis.CompleteOutput*` cubre prefijos completos y cortos, uniones de todos los retornos, escrituras de cola SDK, bytes ausentes, escapes de punteros y restricciones de portadores. Los casos del llamador rechazan certificados ausentes o cortos, desalineación, límites del marco y solapamientos con registros guardados, alias, invalidación por escrituras posteriores y valores opacos vivos. `NativeSourceHints.CompleteNativeOutput*` reproduce productores y consumidores ARM64 ensamblados, exige el rango completo de entrada SDK y rechaza pruebas obsoletas de código, CFG, ABI, auditoría, proveedor e instancia de llamada. Son comprobaciones de inicialización de bytes y aceptación del código fuente; no ejecutan el cuerpo máquina original ni certifican un retorno lógico nativo.

`MedCallingConvValueFlow.FPInputsFollowOnlyAuthenticatedCallPrefixes` / `NativeSourceHints.PreservedFPPrefixesRetainAllEntryInputsAfterSDKCalls`: Las regresiones del prefijo comprueban tres lecturas válidas y diecinueve casos rechazados de portadores, prefijos, propiedad de llamadas y valores no utilizados. Un llamador ARM64 ensamblado conserva las cuatro vías double entrantes a través de una llamada SDK; una nueva elevación verifica la ABI completa y la aceptación del código fuente. El método WMF aceptado e inalterado `0x36350` también pasa 2048 casos en O0 y otros tantos en O2 frente a una expresión nativa CoreGraphics independiente de inversión, traslación, normalización, concatenación y aplicación, comprobando los 32 bytes, receptor/selector y protecciones de entrada, con dimensiones cero, negativas, infinitas y NaN. No se ejecuta el cuerpo máquina original de WMF.

`SourceABI.SwiftEntryCapturesAndReturnsTheErrorRegisterOnEveryPath` verifica la captura ARM64/x64, ambos retornos y el rechazo de pruebas de transporte ausentes o modificadas. `NativeSwiftCallsKeepBothResultsAndPostCallErrorBranches` mantiene el resultado, el registro de error actualizado y la condición posterior, con colisiones de nombres privados y ambos órdenes de emisión. El C generado se ejecuta a O0/O2 en el destino Darwin nativo frente a oráculos independientes de éxito y fallo. `SwiftFunctionSymbols.RegularExpressionInitializerRetainsContextAndError` rechaza alias, símbolos completos cambiados, entradas fuera del código y formatos de imagen no admitidos. Estas pruebas no ejecutan el constructor WMF original ni demuestran la recuperación completa de métodos superiores.

`NativeSourceHints.SwiftErrorDeclaration*` vuelve a levantar entradas ARM64/x64 ensambladas tras sustituir una ABI escalar observada por la declaración del compilador. Las auditorías actuales ausentes o incompletas impiden la sustitución; los contratos de fuente explícitos en las opciones, MedIR o HighIR conservan su autoridad. Las pruebas de entrada también rechazan una marca de salida de error ausente en MedIR o un ancho de operando incorrecto antes de convertir a HighIR.

La captura de entrada sigue siendo una raíz de vivacidad cuando todas las rutas sobrescriben el registro de error, incluida la limpieza final de la fuente tras vincular once. `NativeSourceHints.SwiftErrorCallsRequireCurrentDirectNativeProjection` rechaza funciones llamadas ausentes, nulas, modificadas o sin prueba, así como destinos modificados, llamadas indirectas, resultados incompletos, operandos ausentes y efectos incompatibles.


`SwiftFunctionSymbols.RepeatedDeclarationsKeepEveryRecordField` comprueba registros duplicados idénticos y rechaza cambios de nombre, tamaño, procedencia del límite u origen. `NativeSourceHints.SwiftErrorCallResults*` prueba la inferencia automática de llamadores ARM64/x64 y rechaza destinos actuales ausentes, operaciones de máquina alteradas, auditorías obsoletas, ABI incompletos y extracciones de resultados ausentes, estrechas o ajenas. La ejecución del código de entrada también cubre declaraciones opcionales de depuración contradictorias, conservando la convención vinculada y los roles de error y contexto.

Los generadores de witness Swift verifican `CurrentValueSubject: Publisher` y `Range<Bound: Comparable>: RangeExpression` en ARM64/x86-64 para macOS y Mac Catalyst. `scripts.tests.test_generate_swift_witness_contracts` rechaza cambios de entradas genéricas, tipos o miembros de respuesta de metadatos, prototipos, proveedores de exportaciones y flujo incompleto. `ObjCSourceBindings.SwiftWitnessUndefRequiresExactDescriptorContract` y `SwiftWitnessUndefRejectsUnprovedInputAndABI` comprueban ambos descriptores en ambas arquitecturas, con 33 mutaciones por descriptor y arquitectura sobre identidad del runtime/importación, almacenamiento débil o conflictivo, ABI y efectos. Estos catálogos no autorizan disposición ni préstamo del marco.

`scripts.tests.test_generate_swift_data_declarations` comprueba la consulta completa del descriptor `String.Index` y rechaza cambios de celdas simbólicas, bytes o longitudes de receta, flujo de metadatos/caché, ABI del runtime y definiciones duplicadas o ausentes. `ObjCSourceBindings.SwiftRangeIndexDescriptorKeepsItsCompleteRecipe` comprueba el descriptor no inicial en el desplazamiento 3 en ambas arquitecturas; `SwiftRangeIndexDescriptorRejectsStaleIdentityAndRecipe` rechaza 20 mutaciones por arquitectura y vuelve a validar las pistas de direcciones publicadas y la emisión de helpers.

`ObjCSourceBindings.PrivateFramePointerTailRequiresExactStoreOnEveryPath` cubre ciclos de variables objeto tras fusionar PHI y un valor del marco privado dentro del mismo ciclo. El ciclo de objetos conserva el almacenamiento exacto del puntero; los ciclos con marco, las escrituras parciales y las fugas desconocidas rechazan la prueba.

`ObjCCallHints.FoundationGenericNSRangeKeepsSixPointersAndTwoWords` verifica ambas arquitecturas y proveedores y todos los argumentos y resultados; rechaza importaciones débiles, desplazamientos, proveedores ajenos, símbolos obsoletos y efectos de préstamo inventados.

El lector de testigos String en `scripts.tests.test_generate_swift_witness_contracts` verifica el flujo completo, rechaza 28 cambios de almacenamiento, ABI y flujo y siete declaraciones ambiguas, y limita la entrada. Las pruebas de enlace cubren cuatro descriptores en ambas arquitecturas con 33 mutaciones por pareja. La identidad no concede disposición del marco ni préstamo.

`PreparedFiniteKeys.*` comprueba la destrucción del contexto y el renombrado, el orden y los límites de proyección, la invalidación explícita tras mover, los resultados malformados e incompletos, los dominios vacíos o no únicos y los límites exactos de capacidad. Las regresiones existentes de caché y marco también cubren esta ruta.

`SourceABI.SwiftPointTransformKeepsTwoFloatingInputsAndResults` rechaza cambios de registros, disposición, contexto y resultados indirectos en ambas arquitecturas. `SourceABI.SwiftPointForwardingPreservesBothIEEECarriers` ejecuta el código de reenvío con -O0/-O2 y comprueba ceros con signo, subnormales, infinitos y cargas NaN en ambos campos. Las pruebas de declaración aceptan las formas del compilador y argumentos con nombre y rechazan firmas alteradas e identidades ambiguas.

La fixture MainActor comprueba el flujo completo de metadatos fijos y tabla estática, y rechaza cambios de almacenamiento, ABI, extracción e identidad, efectos adicionales, declaraciones ausentes o duplicadas y presupuestos agotados. Ambos catálogos exigen los tres símbolos SDK para cada destino. Las pruebas de enlace cubren los cuatro descriptores en arm64/x64 con 33 mutaciones por descriptor y arquitectura. La prueba de ejecución del anfitrión compara la tabla pública con cinco patrones de bits del argumento de instanciación.

`BitVectorEncodingClone.WatchMigrationAndGrowthOutliveTheSource` amplía tablas y listas tras copiar y destruir el origen, modifica una copia hermana de forma independiente, interrumpe la propagación tras una visita y comprueba los modelos completos reanudados contra las cláusulas originales y relaciones booleanas independientes.

`ObjCCallHints.SwiftPublishedAccessorsKeepOpaqueValueAndAllKeyPaths` verifica ambos accesores en ARM64/x86-64 y los dos proveedores Combine canónicos; rechaza nueve cambios de ABI y ocho de identidad de importación por combinación. La validación SDK independiente ejecuta el C generado de ambas configuraciones fuente con O0/O2 en ARM64 y compara los 24 bytes, las guardas de entrada/salida y dos identidades owner en 128 llamadas. Ocho configuraciones de compilación cruzada cubren ambas arquitecturas en macOS/Mac Catalyst. El oráculo conserva las referencias consumidas por el setter y no concede atajos de préstamo ni propiedad al producto.

`ObjCCallHints.SwiftMainActorSharedKeepsObjectAndMetatypeContext` verifica los portadores completos del resultado y de swiftself en ARM64/x86-64 y rechaza diez mutaciones de ABI y diez de identidad de importación por arquitectura. La validación independiente del SDK ejecuta el C generado sin modificar para ambas configuraciones de arquitectura fuente a O0/O2 en un host ARM64: 128 llamadas conservan la identidad del singleton y del metatipo y equilibran la propiedad de referencias. Ocho configuraciones de compilación cruzada cubren macOS y Mac Catalyst en ambas arquitecturas; la ejecución nativa x86-64 sigue siendo una cobertura independiente.

`BitVectorEncodingClone.RootQueuePreservesDecisionsAcrossGrowthAndBudgets` comprueba variables raíz y pendientes mezcladas, raíces sin decisión, copias de copias, fuentes destruidas, nuevas variables, ambas polaridades predeterminadas, interrupción y reanudación con presupuesto, conflictos y reinicios. Los modelos completos y todos los contadores deben coincidir con una codificación nueva.

`ContextFiniteProofs.*` comprueba el aislamiento y reemplazo de propietarios, los contextos, el movimiento de tokens, predicados exactos y proyecciones ordenadas, nodos añadidos, resultados completos e incompletos, límites de almacenamiento y expulsión LRU. Las pruebas de marco exigen la consulta final de unicidad antes de almacenar y conservan el límite de nodos en los aciertos.

`LinuxPriorityTests.cpp` comprueba el estado explícito de tareas, aislamiento de hilos, observaciones ausentes, JSON inválido, admisión del perfil y efectos del rechazo. Programas independientes x64/AArch64 con llamadas sin envoltorio a O0/O2 verifican los límites nice, la reducción de argumentos a 32 bits, los permisos CAP_SYS_NICE/RLIMIT_NICE y la codificación getpriority del kernel. Ejecute `LinuxPriority.*` y `Backends/LinuxPriorityProcess.*` en `NeverDLinuxProcessTests`; los cambios compartidos de kernel/JSON requieren después las suites completas de procesos Linux, Android nativo y API pública de procesos. Los transportes nativos opcionales ausentes siguen siendo omisiones explícitas.

`LinuxKernelAvailability.*` valida entradas de ausencia explícita y admisión de perfiles. `Backends/LinuxKernelProcess.*` usa programas independientes x64/AArch64 con llamadas sin envoltorio a O0/O2 para verificar ENOSYS antes de validar argumentos y el rechazo de llamadas no especificadas o ajenas. La prueba Android compara SVC sin envoltorio con `syscall` de Bionic, distinguiendo retorno bruto y efectos en errno. Los cambios de disponibilidad requieren estas pruebas focalizadas, las suites completas de procesos Linux y API pública, y las suites Android de syscall, entrada nativa y señales.

`CompletedQueryCache.*` comprueba dominios completos de un byte, cada posición compacta, crecimiento, aislamiento de contexto y propietario, entradas inválidas o incompletas y límites exactos de almacenamiento. Las ramas nativas conservan costes lógicos fijos y presupuestos exactos o una unidad menores aunque las respuestas completas eviten trabajo del motor.


`BinaryLowIRRefinement.NativeTargetDomainsKeepIndependentProjections` / `FrameOffsets.FrameAndJointTargetProjectionsKeepIndependentSearches` comprueban cadenas repetidas de destinos nativos con cambios de rama y escrituras simbólicas de marco, costes lógicos fijos, presupuestos exactos o con una consulta menos, límites de destino inválidos, agotamiento de puertas y observaciones finales incorrectas. Las proyecciones intercaladas de marco y destinos correlacionados conservan también las tuplas completas, el orden de observación y el rechazo de resultados incompletos al reemplazar el predicado.
`FrameOffsets.Cached*` cubre traslaciones con bits altos libres en la raíz, desbordamiento sin signo, cambios en la forma de las sumas, ambos modos de caché, separación de predicados, capacidad cero, rechazos por presupuesto de consultas/nodos y dominios vacíos frente a no únicos. Las consultas iniciales conservan pruebas completas; las traslaciones posteriores pueden usar una prueba ya terminada aunque no quede presupuesto de consultas.

## Contratos de los kernels Android GKI publicados

`AndroidTestExecution.def` asigna a `FiniteRegistryRejectsBeforeSuccessAndCanBeReused` un límite CTest total de 120 segundos y `RUN_SERIAL`. Cada carga conserva su presupuesto de 30 segundos; la ejecución serial evita contención entre pruebas de capacidad. La política cubre las seis variantes O0/O2 y de relocación.

`LinuxPIDFD.*` valida antes de cargar ramas, enums inválidos, conflictos GKI/ausencia y catálogos mal formados, excesivos o contradictorios. `Backends/LinuxPIDFDProcess.*` usa llamadores independientes O0/O2 x64/AArch64 para ocho ramas: flags, asignación compartida, límites, cierre/reuso, catálogo cerrado, errores de no líder y orden escalar/vectorial. Destinos ausentes y no líderes preceden el agotamiento FD; cubre flags de hilos y catálogo vacío con self implícito. Los vectores contrastan longitud negativa temprana y metadatos posteriores inaccesibles, y una extensión original fuera del límite usuario cuya extensión acotada cabe, en pidfds y ambos flujos capturados. Los casos Android `ReleasedGKIProcessDescriptorsShareRawAndBionicOwnership`, `ReleasedGKIVectorImportRetainsRawAndBionicErrors` y `ReleasedGKICatalogueRetainsRawAndBionicLookupErrors` conservan errores brutos, errno Bionic, catálogo y agotamiento en seis perfiles de reubicación. Ejecutar primero estos casos y luego las suites completas Linux proceso, Android native y proceso público. Ejecutan el modelo, no arrancan ocho kernels GKI. Véanse los [contratos GKI publicados](../android-gki-kernels.md).

`ProcessCPUClocksRetainIdentityAndIdleSeparation`, `ProcessCPUClocksKeepMissingObservationBoundaries`, `LinuxClock.ProcessCPUObservationsShareAliasesAndRemainFixedWhileIdle`: Comprueba las ocho revisiones: identidad directa/Bionic, PROF/VIRT/SCHED, 32 bits bajos, destino antes del fallo de puntero, muestras ausentes, alias, CPU no negativa y separación CPU/pared en reposo. `AndroidTimeTests.cpp` comprueba salidas y centinelas; el syscall cooperativo comprueba el TID actual no líder.

`ZeroTimeoutPollRetainsReadinessAndOrderedCopies` verifica llamadas raw O0/O2 de ocho GKI: descriptores vivos, negativos y cerrados, duplicados, reducción de argumentos, orden tiempo/máscara, timespec cero de solo lectura, importación completa antes de disponibilidad y primeros `revents` conservados tras un fallo posterior. `ZeroTimeoutPollKeepsUnobservedBoundaries` mantiene límites desconocidos de núcleo, recursos, máscaras, esperas y disponibilidad. Android `ReleasedGKIZeroTimeoutPollSharesRawAndBionicResults` verifica la tabla y propiedad de errno en seis perfiles de empaquetado.

## Atributos de directorio por lotes acotados

bulk-attributes verifica grupos completos, conjunto de nombres/tipos, guardas de bytes sin usar, low32 FD, palabras bitmap, errores nativos, dup, open independientes, EOF y rewind cero. Modos literal/desconocido solo virtuales. Los modelos cubren stat completo, invalidación, nombres NFD/255 bytes, alias entrada/salida, errores de transporte/presupuesto, movimientos/SWAP/eliminación/reutilización y permiso explícito. Inventario requerido:65 casos por plataforma,195 ARM64 y130 Intel. Solo ARM64 HVF coincidente se verifica localmente. native5s, guest/Python5,000,000us/quantum1024 y public10s no cambian.

## Enlaces físicos Darwin acotados

link sigue el destino simbólico final; linkat flags=0 elige el propio enlace y AT_SYMLINK_FOLLOW su destino. Solo se admiten low32 0/0x40; otros bits bajos dan EINVAL antes de importar. La búsqueda fuente y EPERM de directorios preceden al destino; un destino existente da EEXIST. Se exige permiso de modificación del destino y el mismo dominio de montaje explícito. Alias iniciales y conflictos conocidos de dispositivo, modo o flags siguen excluidos.

Un alias consume entrada y ruta/NUL, sin nuevo inode. Bytes, permisos de atributos, validez de metadatos y reservas de mapping pertenecen al objeto compartido. Las políticas explícitas actualizan enlaces y ctime; sin ellas stat completo es desconocido. Cambiar atributos invalida stat; cambiar contenido invalida observaciones de atributos. Descriptores y últimos mappings retienen costes de nombres retirados; solo costes liberables de inmediato se descuentan. Los subárboles seleccionan identidad y padre exactos; alias externos permanecen y destinos relativos usan el padre seleccionado.

Tras varios nombres F_GETPATH/ATTR_CMN_NAME siguen sin soporte aunque quede uno o ninguno; no se afirma un modelo general de caché APFS. bulk NAME usa la entrada real; rename/SWAP del mismo objeto conserva ambos nombres. Casing EXCL, O_SYMLINK, Intel HVF, iOS físico, ACL, mappings coherentes/señales EOF, dyld, Mach IPC, hilos y frameworks completos siguen pendientes. Este contrato amplía las exclusiones anteriores solo dentro de sus límites.

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
65 mandatory workloads per platform / ARM64 195 / Intel 130
```

bulk-attributes verifica grupos completos, conjunto de nombres/tipos, guardas de bytes sin usar, low32 FD, palabras bitmap, errores nativos, dup, open independientes, EOF y rewind cero. Modos literal/desconocido solo virtuales. Los modelos cubren stat completo, invalidación, nombres NFD/255 bytes, alias entrada/salida, errores de transporte/presupuesto, movimientos/SWAP/eliminación/reutilización y permiso explícito. Inventario requerido:63 casos por plataforma,189 ARM64 y126 Intel. Solo ARM64 HVF coincidente se verifica localmente. native5s, guest/Python5,000,000us/quantum1024 y public10s no cambian.

`MaterializedRuntimePreservesOwnedObjectsOnNativeWindows` verifica ejecución modelada original, restauración, permisos y ejecución nativa Windows de ambas imágenes: reasignación/liberación del heap, punteros interiores codificados, rearme FLS, bloqueos recursivos, LastError y páginas virtuales reservadas, comprometidas y protegidas. `MaterializationRequiresKnownSupportedState` rechaza versión ausente y TLS dinámico. `RuntimeRestorationHasTheSameCAPIAndCLIContract` compara bytes exactos e informes. Las comprobaciones de construcción Linux y observaciones Wine no sustituyen evidencia del ciclo de vida nativo Windows.

`NeverDUnpackDriverTests` cubre entrada, imports del núcleo, recursos retenidos, ABI, exports, planificación, solicitudes y descarga, C API/CLI y suma PE. Los casos obligatorios KVM/WHP e ImageHlp en Windows no prueban una carga nativa en el núcleo. [UNPACK](unpack.md).

## Pruebas del estado opaco

`X86PreservedState.*` comprueba formas escalares nuevas, alias exactos, reinicio estricto y rechazo de bytes/secuencias/versiones obsoletos. `OriginalBinaryUndefinedIndependence.*Opaque*` cubre ramas, llamadas internas, destinos indirectos exhaustivos, perfiles exactos y presupuestos de metadatos exactos/menos uno derivados de decodificación independiente. `BinaryLowIR*.*Opaque*` cubre testigos frente a elecciones indefinidas arbitrarias, múltiples fuentes inductivas, rechazo tardío de rango/presupuesto, preservación escalar desde la entrada real y cambios posteriores de bytes con LowIR idéntico pero resumen distinto. `NativeUndefinedIndependence.*Opaque*` y `NativeStackControl.*FreshMemoryCall*` comprueban interiores de grupos, límites previos al corte, recibos obsoletos y evaluación del destino antes de modificar la pila. Reconstruir consumidores afectados, incluido `NeverDInterpreterLLVMRefinementTests`; informar por separado sanitizers, fallos compilados y pruebas ordinarias.
