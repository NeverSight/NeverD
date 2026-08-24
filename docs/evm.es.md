**Idiomas**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# Descompilación EVM

[← Índice de documentación](README.es.md)

NeverD carga bytecode tradicional de Ethereum Virtual Machine, construye LowIR
de 256 bits, MedIR SSA de pila y HighIR recuperado, y emite LLVM IR, C23 o
Solidity. El análisis estricto es el predeterminado, pero la EVM legacy no valida
la imagen completa de antemano: sólo se rechaza en su PC exacto un opcode no
asignado o inactivo cuando una lane de ejecución definitivamente `Reachable`
demuestra que se alcanza. Los bytes muertos y candidatos CFG sólo
`MayReachable` no se convierten en errores strict.

Las salidas Solidity y C son reconstrucciones semánticas. Conservan el orden de
opcodes, la aritmética de 256 bits, las comprobaciones de pila y el control de
flujo validado, pero no afirman reproducir el código fuente, nombres o tipos originales.

## Inicio rápido

```bash
# LLVM IR verificado con valores i256/i512.
./build/bin/neverd lift contract.evm -o contract.ll

# Inspeccionar cada fase de análisis EVM.
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# Emitir C23 o Solidity.
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Elegir opcodes históricos o conservar los desconocidos como nodos de fallo.
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

`disasm`, `cfg` y las consultas Low/Med/High/LLVM de la API C también aceptan
EVM. La reescritura binaria EVM se rechaza explícitamente; `patch` sigue siendo nativo.

## Entradas aceptadas

| Entrada | Reconocimiento y normalización |
|---------|--------------------------------|
| Bytes crudos | `.raw`, `.evmraw` o contenido binario con extensión EVM explícita |
| Texto hexadecimal | `0x` opcional, espacios ASCII arbitrarios, `.evm`, `.hex`, `.bin`, `.bytecode`; también se detecta hex sin extensión tras validarlo |
| Artefacto de compilador | `.json` con `deployedBytecode`, `runtimeBytecode` o `bytecode` en raíz o bajo `evm`; también JSON estándar solc `contracts → file → contract → evm` |

El bytecode runtime/desplegado tiene prioridad sobre el de creación. Si sólo hay
creation code, NeverD reconoce wrappers constantes y acotados `CODECOPY`/`RETURN`
y extrae la porción runtime copiada. El recorrido del constructor usa el mismo
decodificador de instrucción única que el decoder real, bajo el hardfork
analizado, de modo que un byte que es dato en un fork y opcode en otro no puede
mover la frontera. Un campo `deployedBytecode` o `runtimeBytecode` presente es
autoritativo: un `0x` explícito se acepta como runtime vacío que se detiene de
forma natural y bloquea deliberadamente el fallback al bytecode de creación.
Sólo un campo ausente permite probar el candidato siguiente; el hex ausente o
formado sólo por espacios, sin prefijo explícito, se rechaza. Una entrada raw
explícita también puede estar vacía.

### Trailers de compilador

`EVMMetadataFields.def` tabula ambos formatos de trailer. Solidity escribe un
mapa CBOR cuyos dos bytes finales cuentan sólo el mapa; `vyper` escribe un
arreglo CBOR que termina en ese mapa, y cuyos dos bytes finales cuentan todo el
footer, incluidos ellos mismos. Leer un encuadre como si fuera el otro no falla
de forma ruidosa —cae dos bytes más allá y elimina dos bytes de código real—, así
que se intentan ambos y una entrada que no coincide con ninguno se deja intacta.

El trailer se lee dos veces: una sobre la entrada tal cual y otra sobre el código
runtime que queda tras desenvolver un wrapper de despliegue. Vyper trasladó su
trailer al initcode y deja el código runtime sin ninguno, así que un lector que
sólo mire después de desenvolver informa de un build desconocido para un contrato
que se había nombrado a sí mismo. Un footer de secuencia declara además la
longitud del código runtime, las de las secciones de datos y la de los
immutables, que acotan el código devuelto sin ejecutar el constructor.

### Contenedores que no son instrucciones

`EVMBytecodeContainers.def` clasifica la entrada antes de cualquier
decodificación. Desde que EIP-3541 hizo indesplegable `0xEF`, un `0xEF` inicial
promete que los bytes no son instrucciones:

| Contenedor | Marcador | Tratamiento |
|------------|----------|-------------|
| legacy | — | se decodifica como instrucciones |
| delegación (`eip-7702`) | `0xef0100` y exactamente 23 bytes | informa de la cuenta destino; el análisis se detiene |
| eof (`eip-3540`) | `0xef00` | rechazado; ningún fork lo ha activado |

Los veinte bytes de un indicador de delegación son una dirección, no código.
Decodificarlos leería la dirección como opcodes y produciría un grafo de flujo de
control de una cuenta, así que `info` informa del destino y el análisis se niega
indicando el motivo. La negativa distingue los dos casos: antes de Pectra el
marcador aún no está asignado, y desde Pectra el código runtime del destino
simplemente falta. Un marcador con cualquier otra longitud es entrada mal formada
y no una variante del contenedor, y sigue tratándose como instrucciones para que
el decoder pueda nombrar el byte que no pudo leer.

Hex mal formado, dígitos impares, placeholders de linker sin resolver, artefactos
multi-contrato ambiguos, límites de metadata inválidos y hex ausente o en blanco
producen errores accionables. Un raw vacío explícito o runtime `0x` sigue siendo
un programa vacío válido. `BytecodeLoadOptions::ArtifactContract` selecciona
`Contract` o `path/File.sol:Contract`. Un nombre no cualificado se rechaza si
varios archivos lo definen, evitando que el orden del artefacto elija mal.

EVM está registrado en el loader central, no oculto tras un plugin backend. CLI,
API C, desensamblador, CFG y consultas IR reciben así la misma imagen normalizada
y las mismas opciones.

## Hardforks y opcodes

Se cubren todos los opcodes legacy asignados de Frontier a Fusaka, incluidos
`PUSH0`, almacenamiento transitorio, `MCOPY`, opcodes blob y `CLZ`. `latest`
selecciona Fusaka por defecto.

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, amsterdam, bogota, latest
```

Se aceptan `dao`, variantes con guion bajo, `merge`, `prague` y `osaka`.
Actualmente `latest` y `osaka` resuelven a la revisión canónica `fusaka`.

`latest` significa la última revisión mainnet finalizada implementada, no la
cabeza de desarrollo de Ethereum. [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/)
está previsto para Q4 2026; las instrucciones aún en Review
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) y
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) sólo se activan
con `--evm-hardfork=amsterdam` (o `bogota`) y quedan fuera de `latest` hasta su
finalización. En EIP-8024 sólo se consume un inmediato válido; un candidato
inválido sigue siendo la siguiente instrucción.

EOF se retiró en
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2).
EOFv1/EIP-7692 no está programado y la propuesta de contenedor
[EIP-3540](https://eips.ethereum.org/EIPS/eip-3540) está Stagnant. El antiguo
repositorio `execution-spec-tests` está archivado y sus pruebas mantenidas se
trasladaron a
[execution-specs](https://github.com/ethereum/execution-specs/tree/master/tests).
NeverD no presenta un contenedor EOF experimental como comportamiento mainnet.

El modo strict rechaza un byte desconocido o fork-inactive sólo cuando una lane
de estado definitivamente `Reachable` demuestra que se ejecuta.
`--evm-relaxed` lo conserva como fault prefix tipado y diagnóstico, pero los
backends fallan al alcanzarlo; nunca se convierte en NOP.

## Arquitectura de metadata al estilo LLVM

La metadata EVM mantenida a mano sigue el patrón `.def` multi-incluido de LLVM:

- `EVMOpcodes.def` es la única fuente para todos los opcodes legacy finalizados
  y de desarrollo opt-in. Encoding, cambios pop/push reales, tipo de inmediato,
  clase, fork de activación, efecto principal,
  acceso ortogonal a memoria, estado, call-value y terminación viven en cada
  registro; no hay valores predeterminados silenciosos.
- `EVMMemoryAccesses.def`, `EVMStateAccesses.def` y
  `EVMCallValueAccesses.def` definen dominios cerrados y tipados. `CALL` puede ser
  llamada externa y lectura/escritura de memoria; `EXTCODECOPY`, lectura de
  contexto y escritura de memoria. Estado usa la lattice
  `None/Read/Write/Unknown`. Payability es independiente: `CALLVALUE` normalmente
  implica `payable`; sólo se suprime si el analizador demuestra el guard canónico
  `ISZERO(CALLVALUE)` y que la rama no cero acaba en `REVERT`.
- `EVMImmediateKinds.def` define los datos PUSH de ancho fijo y los encodings
  single/pair condicionales de EIP-8024; `EVMDecodeStatuses.def` posee el
  vocabulario estable expuesto por LowIR y el desensamblado.
  `EVMUpstreamOpcodePolicy.def` registra el alias de nombre de go-ethereum y las
  exclusiones históricas y de EOF sin programar deliberadas;
  `scripts/audit_evm_opcode_metadata.py` rechaza deriva de bytes y cualquier
  constante upstream nueva que no haya sido revisada.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` y
  `OutputLanguages.def` generan enums ordenados, parsers, nombres, opciones CLI
  y valores ABI C. `EVMAnalysisLimits.def`, `EVMInterpreterLimits.def`,
  `EVMABIParserLimits.def` y `EVMABITableLimits.def` declaran los límites por
  etapa del análisis, intérprete, parser y tablas públicas. `EVMConstants.h`
  centraliza los anchos de protocolo y nombres internos estables, y materializa
  desde `EVMAnalysisLimits.def` los valores predeterminados del análisis y los
  nombres de opciones diagnósticas; los headers del intérprete y ABI
  materializan los límites de sus propias tablas.
- `EVMCalls.def` describe las cuatro instrucciones que llaman a otro programa y
  el retículo de procedencias de una dirección de callee. Un único flag por
  registro, si un operando de value se sitúa entre el callee y la ventana de
  argumentos, deriva todas las posiciones posteriores, y la tabla se valida
  contra la base de datos de opcodes para que la derivación no se desvíe de los
  pops declarados.
- `EVMPrecompiles.def` es el diccionario de direcciones en las que responde el
  propio protocolo, cada una con el fork que la reservó y la propuesta que la
  programó. `P256VERIFY` en `0x100` se atribuye a `eip-7951`, la propuesta Final
  que la reservó en mainnet con Fusaka; la propuesta de rollup de la que procede
  su interfaz nunca llegó a programarla. El gas está ausente a propósito: el
  coste de una precompile es función de su entrada y se ha re-tarifado sin que
  cambien la dirección ni la operación.
- `EVMMetadataFields.def` y `EVMBytecodeContainers.def` describen qué es una
  entrada antes de decodificarla: los dos encuadres de trailer de compilador y
  los contenedores cuyos bytes no son instrucciones en absoluto.
- `EVMRecoveredFacts.def` posee las grafías de los vocabularios de hechos
  recuperados, de modo que un nombre que llega a la salida vive en un solo
  lugar y no en un `switch` del que puede quedar fuera un nuevo enumerador.
  `EVMKnownSignatures.def` guarda una sola vez el spelling y selector canónicos
  de cada función y separa en `KnownFunctionVariantInfo` por estándar las listas
  de retorno y el rol de evidencia independent/non-independent. Así un spelling
  compartido por ERC-20/ERC-721 sigue siendo un único candidato invocable, sin
  demostrar por sí solo ningún estándar ni heredar el retorno del primer
  variant. Eventos y custom errors conservan records tipados distintos.
- `Semantics.h` contiene el evaluador ALU escalar independiente del target.
  Interpreter y constant folding comparten el mismo `APInt` comprobado; los
  lowerings LLVM/C/Solidity siguen explícitos y fail-loud.

El decoder es la frontera de bytes. Identidad asignada y activación por fork se
separan: relaxed conserva nombre, fork de introducción y ancho inmediato de un
opcode inactivo, con semántica conservadora que falla. Así un inmediato inactivo
no desplaza límites posteriores. Análisis, intérprete y emitters usan `Opcode`
generado y consultas de metadata; encoding crudo sólo reaparece en ABI de trace
y host callbacks. Las 17 entradas de `SWAP16` y los 7 argumentos host máximos
son límites separados derivados en compilación.

`OpcodeInfo` no puede construirse medio válido y su nombre es
`llvm::StringLiteral`. El validador compile-time rechaza encodings duplicados,
propiedades desconocidas, contratos ALU, inconsistencias effect/state,
familias PUSH/DUP/SWAP/LOG, terminadores y resultados host inválidos. Sólo una
factory explícita crea metadata unknown conservadora.

Los `.def` son bases escritas a mano como
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def).
`.inc` se reserva para fragmentos realmente generados, por ejemplo por TableGen.
Los registros declarativos ricos viven en `.td` y
[TableGen](https://llvm.org/docs/TableGen/ProgRef.html) genera `.inc`. NeverD no
tiene aún ese paso para EVM, por lo que un `.inc` sin generador sólo fingiría ser
generado. El C++ sigue las [normas LLVM](https://llvm.org/docs/CodingStandards.html),
ADT/strings LLVM en límites y switches semánticos exhaustivos.

Añadir un opcode exige un registro `EVM_OPCODE` completo, semántica escalar,
lowerings explícitos y pruebas focalizadas. Añadir un hardfork exige un
`EVM_HARDFORK` ordenado y aliases. API tipada, lookup, validación, clasificación
y CLI crecen sin tablas paralelas.

## Modelo de análisis

- **EVM LowIR** conserva PC, encoding, estado tipado del inmediato y operandos
  decodificados de profundidad de pila (incluidos el relleno con ceros a la
  derecha de PUSH y la regla de consumo condicional de EIP-8024), bloques,
  aristas predecesoras/sucesoras, destinos `JUMPDEST` validados, alcanzabilidad y
  dominios de altura de pila. La recuperación del CFG es un punto fijo
  determinista de todo el programa: se propaga por cada slot una colección
  finita y acotada de valores de 256 bits, y se conserva una pila abstracta por
  cada altura concreta. Las constantes transportadas por bloques de llamada y
  retorno internos, las permutaciones de pila, `PC`/`CODESIZE` y las operaciones
  escalares de ALU pueden resolver así uno o varios destinos concretos. Un
  destino realmente desconocido queda como arista indirecta explícita.

  En un back-edge, un slot loop-carried que cambia se sobreaproxima semánticamente
  a `Top` para que converja el punto fijo; esta abstracción de recurrencia es
  independiente de los recursos. Instrucciones, bloques, estados, valores,
  pilas, lanes, edges, actualizaciones de worklist y transferencias
  instrucción×lane tienen presupuestos con nombre, incluidos
  `MaxAbstractValuesPerSlot`, `MaxStackHeightVariants` y
  `MaxAbstractInstructionTransfers`. Cero o agotarlos es un error duro antes
  de insertar, nunca `emergency widening` ni truncamiento silencioso.

  `EVMLowFaultKinds.def::InvalidJumpDestination` conserva sensibilidad de ruta
  en un `end-of-code JUMPI`: una condición definitivamente true con destino
  inválido no tiene cola exitosa y registra un fallo definitivo; una condición
  definitivamente false tiene éxito. Unknown conserva sólo su posible ruta
  false exitosa y no marca erróneamente toda la lane como fallo definitivo.
- **EVM MedIR** representa cada valor de pila como SSA de 256 bits y conecta
  todos los phi de unión antes de ejecutar una worklist dispersa y determinista
  de constantes. Su lattice privado es `Uninitialized`, una `Constant` exacta u
  `Overdefined`: las constantes iguales se propagan entre bloques y ciclos phi
  anclados, mientras que un ciclo conflictivo o dependiente de runtime no puede
  inventar una constante. La worklist comprueba los ID def-use; valores, state
  lanes, entradas de pila, operaciones, referencias operation-lane, entradas phi
  y actualizaciones de worklist tienen límites independientes. Usa el mismo
  evaluador ALU de `Semantics.h` que el intérprete. MedIR también conserva el
  efecto semántico primario y, de forma ortogonal, el acceso a memoria EVM
  `none/read/write/readwrite`, al estado de nivel fuente y al call-value. Cada
  lane de pila completa del LowIR conserva una lane SSA de ejecución distinta y
  los phi identifican su lane fuente; no se alinean pilas incompatibles por su
  altura máxima.
- **EVM HighIR** recupera selectors del dispatcher de Solidity, palabras
  probables de calldata y retorno, mutability, slots constantes de storage,
  hechos LOG/event, hechos de revert y regiones function/CFG. Un índice de
  productores comprobado y un recorrido iterativo y memoizado recuperan hechos
  desde operandos tipados de MedIR, no desde la distancia entre instrucciones:
  las comparaciones de selector pueden cruzar bloques y phis, usar cualquier
  orden de operandos de `EQ` y conservar una máscara derivada de 32 bits; offsets
  de argumentos, claves de storage, topic0 de eventos, guards non-payable/receive
  y tamaños de retorno exactos de 32 bytes usan sus entradas semánticas. El
  recorrido está estructuralmente acotado por el grafo MedIR y trata expresiones
  malformadas, mixtas o cíclicas como desconocidas. Los destinos conflictivos de
  un mismo selector se diagnostican y omiten. Payability permanece independiente
  del lattice de acceso a estado, y un salto dinámico alcanzable no resuelto
  fuerza una recuperación `nonpayable` conservadora. El dataflow de memoria
  byte a byte sigue writes de offset constante entre bloques, compone overlap/kill
  e invalida conocimiento ante un write dinámico o desconocido. Las pruebas de
  payload cubren hoy selector y bytes Panic conocidos. Para una declaración de
  custom error conocida, el emitter Solidity conserva tipos de parámetro
  canónicos; no afirma recuperar cada valor de argumento runtime.

  El descubrimiento de selectors parte sólo de la lane raíz y sigue las aristas
  no coincidentes del dispatcher; un test parecido a selector dentro de un
  handler no se eleva a función pública. Receive y fallback también están
  restringidos a la raíz y exigen un terminal exitoso definitivamente alcanzable:
  revert, fault, un handler de calldata vacío non-payable o una ruta meramente
  posible no prueban esas entradas. El uso incompatible de calldata descarta un
  candidato canónico y un selector compartido no aporta evidencia independiente
  de estándar. Sólo suficientes selectors independientes compatibles o evidencia
  fuerte de topic/arity exactos, slot de storage o proxy permiten reconocer el
  estándar y elegir su variant. La lista de retornos estáticos se emite sólo si
  todos los terminales exitosos definitivamente alcanzables acuerdan el número
  exacto de bytes ABI; transferencias no resueltas, formas conflictivas o un
  mismatch fallan de forma cerrada. Revert y fault no son retornos exitosos. Los
  demás hechos siguen siendo candidatos sustentados por evidencia.

  HighIR limita por separado funciones, visitas de lane/operación, referencias
  de bloques de región, solicitudes y bytes de memoria, celdas de estado y
  actualizaciones de worklist. El punto fijo de memoria sólo consume lanes
  ejecutadas definitivamente alcanzables, hace meet por consenso de bytes y
  devuelve error duro al agotar presupuesto, sin truncar hechos.

  HighIR registra además la mitad saliente de la interfaz: cada `CALL`,
  `CALLCODE`, `DELEGATECALL` y `STATICCALL`, con la procedencia del callee, la
  dirección reservada que nombra cuando el fork analizado reserva una, el
  selector que la llamada coloca al inicio del calldata del callee y el valor
  transferido cuando es constante. `CREATE` y `CREATE2` quedan excluidos porque
  ejecutan código que aún no tiene dirección, así que no hay callee que
  recuperar.

  Una firma saliente recuperada nunca se suma a los estándares a los que el
  programa responde. Enviar `transfer(address,uint256)` dice que el programa usa
  un token, no que lo sea, y confundir ambas cosas reportaría todo router y todo
  vault como ERC-20. Una llamada delegante se reporta además como hecho de
  proxy, porque es el único miembro de la familia cuyo callee se ejecuta contra
  el storage de este mismo programa.

  La búsqueda de precompiles se filtra por el fork analizado, no por el más
  nuevo que exista. Llamar a la dirección de una precompile que introduce un
  fork posterior alcanza una cuenta sin código, tiene éxito y no devuelve nada,
  de modo que nombrarla reportaría una operación que el programa demostrablemente
  no realizó.
- **LLVM** emite una máquina `i32 @evm_execute(ptr)` limpia para el verifier, con
  pila comprobada de 1024 palabras `i256`, intermedios `i512`, división signed
  protegida, shifts saturados, `BYTE`/`SIGNEXTEND`/`CLZ` exactos y switches válidos.

El intérprete determinista es el oracle semántico. LLVM/C se compilan y comparan;
Solidity se despliega en Anvil y se compara por storage y traces. Un corpus raw
pre-Fusaka también se ejecuta en la EVM nativa de Anvil para validar de forma
independiente ALU, copia calldata, `MCOPY` solapado, expansión de memoria, Keccak
y returndata. Los operandos de cuenta se enmascaran a 160 bits según la
[especificación](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py),
se validan anchos de entorno y `BLOCKHASH` respeta 256 bloques. El buffer EIP-211
se separa de la salida final: sólo `RETURN`/`REVERT` rellenan
`ExecutionResult::ReturnData`; CREATE/CREATE2 siguen la misma regla.

Antes de cualquier efecto específico de un opcode, el intérprete preflight
comprueba la altura requerida tipada, los pops y la altura retenida más los
pushes; un underflow u overflow no puede ejecutar media instrucción.
`EVMForkSemantics.def` selecciona el byte `0x44`: `DIFFICULTY` antes de Paris y
`PREVRANDAO` desde Paris. `REVERT`, faults semánticos, el step limit y el
agotamiento de recursos por allocation/length restauran storage, transient
storage, logs y efectos de selfdestruct al snapshot de entrada, conservando
diagnósticos de frame y bytes explícitos de revert. El fallo de asignación se
marca `ExecutionFaultKind::ResourceExhausted` sin asignar un string de error; si
ni siquiera se pudo crear el snapshot, `HasPersistentStateSnapshot` es false y
el resultado nunca es committable.

### Límites públicos de IR y recursos

La función pública `execute` comprueba primero que
`Code`/`Fork`/`Instructions`/`JumpDestinations` formen LowIR canónico. Por ello,
un fork alterado, un registro de instrucción falsificado, un encoding
incoherente o una tabla de destinos incorrecta devuelve `llvm::Error` antes de
que el intérprete indexe la tabla de instrucciones. El `lowerToMedIR` público
valida, en orden, options, recursos y estructura; después un
`canonical decode replay` decodifica `Low.Code` con su fork/strictness y compara
cada campo LowIR. Sólo entonces puede llamar a `lowerCanonicalLowToMedIR`, crear
índices o asignar output proporcional al caller. El `recoverHighIR` público
replay-valida de igual forma LowIR/MedIR externos. Los caminos privados
`lowerCanonicalLowToMedIR` y `recoverCanonicalHighIR` son sólo para IR propiedad
de `analyze`: omiten únicamente el replay redundante no recursivo y siguen
aplicando todos los HighIR option/resource budgets.

La prueba del dispatcher conserva por `MedStateLane` un dominio selector
ordenado `Any/Exact/Excluded`. Los joins unen conjuntos Exact, intersectan las
exclusiones Excluded y restan un conjunto Exact de una exclusión cofinita; al
ensancharse un dominio se vuelve a visitar la lane. Una igualdad sólo registra
el candidato de la arista true si el selector sigue permitido, y lo excluye en
la arista false. Un `XOR(selector, constant)` crudo registra la arista cero/false
como match cuando todos los sucesores canónicos nombran la misma entrada; este
fallthrough no tiene que apuntar a `JUMPDEST`. La arista no-cero/true es el
mismatch y excluye ese selector; `ISZERO` convierte la misma expresión en una
igualdad. Selector word, calldata word cero, calldata size y call value guard
se refinan por arista. Una condición unknown detiene la prueba en vez de seguir
un branch meramente posible.

Una vez reconocido un candidato de función, el recorrido de su scope continúa
con su `exact singleton selector`. Si la función salta de nuevo al dispatcher
compartido, `SelectorEquality`, `XOR` crudo y `SelectorWord` sólo toman el
`definite edge` coherente con el selector ya emparejado. Los predicados Unknown
o ajenos conservan prudentemente todos los `definite edges`. No se usa la
heurística de excluir otros entry blocks: el flujo legítimo
`shared body/tail-call` permanece en el scope de la función.

Los resultados externos de CALL/CREATE son distintos: el resultado del host es
realmente no determinista, por lo que el análisis explora ambas aristas CFG
precisas. Así conserva la recuperación del fallback ERC-1167 sin usar como
evidencia una condición selector ilegible; un dispatcher verdaderamente Unknown
sigue fallando de forma cerrada.

`EVMAnalysisLimits.def` da al decoder lineal y al constructor CFG un único
presupuesto agregado de diagnósticos LowIR mediante `MaxLowDiagnostics` y
`MaxLowDiagnosticBytes`. Ambos caminos precargan el número y los bytes finales
exactos y rechazan un límite cero. Los presupuestos de diagnóstico LowIR y
HighIR permanecen independientes. La misma tabla carga
`MaxHighDispatchCandidates`, el
agregado global `MaxHighRecoveredArguments`, `MaxHighDiagnostics` y
`MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`,
`MaxHighMemoryTransferCells` y `MaxHighMemoryValueVisits`. Los registros de
candidato y argumento recuperado se precargan antes de insertarlos en cualquier
contenedor de destino o de asignar su nombre/tipo. Cada diagnóstico de salida
HighIR carga número y bytes finales del mensaje antes de construirse o copiarse,
incluido el diagnóstico fijo de IR malformado; agotar el presupuesto devuelve
su error duro con nombre, sin omitir silenciosamente diagnósticos ni hechos.
La región CFG raíz predeterminada carga `MaxHighRegionBlockReferences` antes de
reservar o copiar su lista de PC de bloques.

`EVMABIParserLimits.def` limita nesting de tuples, nodos de tipo y dimensiones
de array agregadas. `EVMABITableLimits.def` limita cardinalidad y texto agregado
de las tablas públicas de firmas/variantes. La validación pública aplica esos
límites antes de parsear o hashear, y luego rechaza enums inválidos, metadata de
kind, estándares, roles de evidencia de selector, tipos no canónicos, hashes
derivados, memberships y colisiones. El lookup de selector de producción está
indexado, el de eventos usa una tabla ordenada por topic y las APIs de topic
comprueban que un `APInt` mida exactamente una palabra EVM antes de comparar u
ordenar.

`EVMInterpreterLimits.def` declara `MaxSteps`, `MaxMemoryBytes`,
`MaxTraceEntries`, `MaxLogEntries`, el agregado `MaxLogDataBytes`, el agregado
`MaxHostReturnDataBytes`, `MaxCalldataBytes`, el agregado
`MaxHostEnvironmentEntries`, el agregado `MaxExternalCodeBytes` y
`MaxPersistentStateEntries`. El agregado de entradas host incluye `BlockHashes`,
`Balances`, `CodeHashes`, `ExternalCode` y `BlobHashes`; el límite de bytes
incluye todos los cuerpos `ExternalCode`. `MaxSteps` conserva el
resultado explícito `StepLimit`. El crecimiento runtime de memory, trace, logs,
datos de log y nuevas claves de estado persistente se precarga; superar esos
límites devuelve `ResourceExhausted` y revierte estado persistente, logs y
efectos selfdestruct. Un agregado inicial de host return data o un mapa de estado
persistente demasiado grande es, en cambio, un error de API de `execute`. El
intérprete mantiene host return data como views `ArrayRef` y usa `lower_bound`
sobre la tabla de instrucciones ordenada y ya validada, sin copiar buffers ni
reconstruir un mapa de PC por ejecución. El `const execute preflight` valida
programa y límites host antes de copiar environment, snapshot o result.

### Auditoría diferencial live de go-ethereum

La auditoría local estándar y CI fuerzan en cada ejecución
`git fetch --depth=1 --force` del `HEAD` remoto de la rama por defecto oficial
`https://github.com/ethereum/go-ethereum.git`. Cada ejecución crea un repositorio
bare privado, temporal y de nombre
impredecible; no existe repositorio Git persistente ni caché compartida. Sólo el
authority ref devuelto por ese fetch y su SHA exacto resuelto eligen la revisión.
Se comunica el SHA y se prueba en un worktree temporal detached; después se
destruyen juntos el repositorio de autoridad y el worktree.
Ni `local_docs`, ni un checkout existente, ni un submodule son rutas de
auditoría; un submodule fijado quedaría obsoleto justo cuando se necesita
detectar drift vivo.

Cada comando Git elimina primero todos los `GIT_*` heredados, incluidos
`GIT_CONFIG_*`, y después instala sólo valores auditados. `GIT_CONFIG_NOSYSTEM`
y `GIT_CONFIG_GLOBAL` desactivan la configuración system/global;
`GIT_ATTR_NOSYSTEM` y `core.attributesFile` por comando desactivan los atributos
system/global, y `core.hooksPath` desactiva los hooks. El repositorio privado
rechaza configuración local inesperada,
grafts, `objects/info/alternates` y `refs/replace`; `GIT_NO_REPLACE_OBJECTS`
desactiva además los reemplazos. Toda desviación falla de forma cerrada.

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

La CLI pública sólo expone `--manifest-output`; fuente, ref y toolchain no son
seleccionables. Su manifest cerrado usa `schema 3`. La sonda Go refleja todo el
inventario booleano exportado de `params.Rules`, llama
a `LookupInstructionSet(params.Rules)` por cada fork mapeado y examina los 256
slots de byte. La asignación se decide sólo con `operation.undefined` de geth;
`HasCost` es únicamente una comprobación cruzada de coste, pues también devuelve
false para operaciones definidas de coste cero. Cada slot
`defined && !HasCost` debe coincidir exactamente con
`EVM_GETH_ACTIVE_WITHOUT_COST` desde su fork de activación declarado; un slot
indefinido con coste, uno definido sin revisar o la desaparición upstream del
marcador fallan de forma cerrada. Cada tabla también compara `base_min_stack` y
`net_stack_delta`. Campos y registros desconocidos, duplicados,
ausentes, fuera de rango o sin analizar fallan. Cada `.def parser` rechaza
también texto con aspecto de macro que no haya consumido, en vez de aceptar una
política `partial`. `EVMUpstreamOpcodePolicy.def`
mantiene aliases y exclusiones históricas/EOF sin programar tipadas y valida sus
invariantes overlap/inactive. El ortogonal `EVMUpstreamSemanticsPolicy.def`
mantiene el inventario cerrado reflejado de `params.Rules`, el mapeo de forks,
excepciones base-stack y familias dynamic-immediate. CI se ejecuta en push a `dev`, pull request, activación
manual y planificación diaria; un fallo publica como artifact la revisión
exacta, el manifest y el log.

En concreto, `EVMUpstreamSemanticsPolicy.def` asigna cada campo booleano
exportado de `params.Rules` a una sola entrada `EVM_GETH_RULE_FIELD` de tipo
`MappedForkSelector`, `NoOpcodeAllocation` o
`ExcludedSelectorExpectedError`. La auditoría activa cada campo por separado y
llama a `LookupInstructionSet`: las dos primeras categorías exigen nil error y
la tercera error; el fingerprint completo de opcode/stack de 256 slots siempre
debe ser `ExpectedFork`. Los campos sin asignación `IsEIP155`, `IsEIP2929`,
`IsEIP4762` e `IsPetersburg` dan Frontier; `IsUBT` debe fallar y dar el
fingerprint Cancun.

`EVMUpstreamSemanticsPolicy.def` declara los opcodes de cada familia dinámica
EIP-8024, su clase de operación y el delta de pila válido;
`EVMEIP8024Immediates.def` sigue siendo la autoridad separada de decodificación
de immediates y clasifica todos los valores single/pair. Mediante `go -overlay`,
la auditoría obtiene los handlers privados reales `operation.execute` y cubre,
tabla por tabla, las `canonical fork jump tables` y las
`mainnet active/scheduled jump tables`. Una familia `inactive` se registra de
forma explícita y una familia `partial` es un error. En cada tabla activa se
ejecutan las tres operaciones declaradas para todos los immediates (`3x256`) y
los `3 missing-operand cases`. Se comprueban aceptación, delta de PC,
operandos/mutación derivados de marcadores, underflow exacto y comportamiento
`0x00` ausente contra las mismas políticas declarativas, sin duplicar la fórmula.

`EVM_HARDFORK_LATEST` tiene un único destino canónico. El mapa cerrado
`EVMUpstreamForkAliases.def` lleva Prague a Pectra, Osaka y BPO1 hasta BPO5 a
Fusaka; Paris, Shanghai, Cancun, Amsterdam y Bogota son identidades. Un nombre
nuevo desconocido falla de forma cerrada. Cada auditoría fija y registra un
`audit_unix_time`, exige que `MainnetChainConfig.LatestFork(time)` corresponda al
latest de NeverD y que `LatestFork(max uint64)` esté en el inventario de alias
con su fork canónico ya probado; ambas tablas de instrucciones se comparan por
completo. El manifest guarda `authority=official-fresh-fetch`, URL oficial,
`HEAD` solicitado y SHA resuelto. El probe fija `GOTOOLCHAIN=local`.

Go y Python aplican `input/collection/string hard limits` antes de materializar
metadatos hostiles; entradas, colecciones o cadenas sobredimensionadas fallan de
forma cerrada. Para `bounded diagnostic output`, una visualización demasiado
larga lleva el `digest` del contenido completo y un `explicit truncated marker`.
Cada proceso hijo tiene salida y plazo acotados; una infracción mata todo el
`process group`/process tree y drena sus pipes.

El recibo live schema 3 actual registra `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, revisión
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, `Go 1.24.0` local,
`stack_limit=1024` y `diagnostics=[]`. Compara `21 fork tables` y
`20 Rules probes`, clasificados como `15 mapped/4 no-op/1 expected-error`. Ambos
registros `mainnet active/scheduled` nombran `upstream BPO2`, que el alias cerrado
mapea a `NeverD Fusaka`. EIP-8024 cubre `23 table targets`; sólo
`Amsterdam/Bogota` están activos, con `1536 candidate executions` y
`6 missing-operand cases`. Los `three handler symbols` coinciden en ambos
targets activos. Cerraron en verde el audit Python `67/67` y
`C++ Opcode 10/10`. En macOS el audit real terminó en `sandbox-exec` con el
`go run` final sin red; el workflow Linux exige `bubblewrap`.

Todas las fases Go —`go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` y `go run`— se ejecutan en un sandbox de filesystem
`capability-root`. Sólo pueden leerse el probe privado, geth fresco, el
`resolved GOROOT` validado y las raíces exactas de runtime del sistema; sólo las
raíces aisladas del entorno son escribibles. La red se concede únicamente a
las fases de dependencias que la necesitan y el run final queda offline. Los
sentinels de `host HOME/workspace` se rechazan y su contenido no puede aparecer
en output. Linux replica la política con `bubblewrap` sin `/` broad bind.

`NeverDEVMDecoderPropertyTests` agota todas las entradas de dos bytes en cada
fork que cambia el decoder, compara el decode completo y los límites `JUMPDEST`
exactos, y pasa cadenas de bytes hostiles deterministas de longitud acotada por
todos los forks.

Las lanes de ruta LowIR/MedIR conservan correlaciones y `MayReachable` sólo ofrece
candidatos CFG. Selector, receive, fallback, forma de retorno y memoria byte a
byte de HighIR consumen únicamente lanes ejecutadas definitivamente alcanzables.
Los selectors compartidos se separan de `KnownFunctionVariantInfo` por estándar,
y el retorno debe superar la comprobación de todos los terminales exitosos. Todo
agotamiento de presupuesto falla ruidosamente, sin emergency widening ni
truncamiento silencioso.

## Contrato C generado

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

Las operaciones ambientales usan la ABI siguiente. `a0` es la cima original,
los argumentos sin usar son cero y el retorno es el primer valor apilado. El
trace hook se llama antes de cada instrucción.

```c
evm_word neverd_evm_host_op(
    struct neverd_evm_env *environment, uint8_t opcode,
    evm_word a0, evm_word a1, evm_word a2, evm_word a3,
    evm_word a4, evm_word a5, evm_word a6);
void neverd_evm_trace(
    struct neverd_evm_env *environment, uint64_t pc, uint8_t opcode);
```

```bash
clang -std=c2x -ffreestanding -c contract.c
```

El frontend debe aceptar `_BitInt` de al menos 512 bits. Apple Clang para Darwin
aún no lo hace; en macOS use un target no-Darwin capaz o la salida LLVM.

## Contrato Solidity generado

La salida combina declaraciones function/storage/event/error por selector con
una máquina PC/pila exacta. Un slot constante se emite como
`recovered_storage_slot_3 = uint256(0x3)`, nunca como variable secuencial que
invente el layout.

El contrato es deliberadamente `abstract`. Sobrescriba `_evmHost` para efectos
ambientales; `_evmTrace` es virtual y emite `EVMTrace` por defecto.

```bash
solc --bin contract.sol
```

## API C

```c
neverd_session_t session = neverd_session_create();
neverd_evm_set_hardfork(session, "cancun");
neverd_evm_set_strict(session, 1);
if (!neverd_session_load(session, "contract.evm") ||
    !neverd_session_analyze(session)) {
  /* inspect neverd_last_error(session) */
}
const char *solidity = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_SOLIDITY, 0, 0);
const char *c = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_C, 0, 0);
neverd_free_string(solidity);
neverd_free_string(c);
neverd_session_destroy(session);
```

`neverd_decompile_all` conserva compatibilidad y emite C. Las nuevas entradas
son `neverd_session_bitness`, `neverd_evm_set_strict`,
`neverd_evm_set_hardfork` y `neverd_decompile_all_ex`. Solidity para nativos,
la ruta LLVM-to-C antigua para EVM y el roundtrip de objeto nativo para EVM se
rechazan de forma explícita, nunca se ignoran.

## Limitaciones explícitas

- Sólo bytecode legacy; aún no se decodifican contenedores EOF.
- Amsterdam/Bogota son objetivos de desarrollo explícitos; `latest` permanece
  en Fusaka finalizado hasta que se finalicen los opcodes previstos.
- Sin RPC, descubrimiento de estado, gas/reembolsos ni ejecución de precompiles.
- La extracción de creation reconoce wrappers estáticos, no una transacción completa.
- Los saltos dinámicos quedan indirectos salvo prueba constante acotada.
- Tipos ABI, nombres, mappings, events y errores personalizados son best-effort.
- La ejecución autónoma de efectos requiere hooks host C/Solidity.
