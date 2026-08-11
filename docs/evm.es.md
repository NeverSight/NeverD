**Idiomas**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# Descompilación EVM

[← Índice de documentación](README.es.md)

NeverD carga bytecode tradicional de Ethereum Virtual Machine, construye LowIR
de 256 bits, MedIR SSA de pila y HighIR recuperado, y emite LLVM IR, C23 o
Solidity. El análisis estricto es el predeterminado: un opcode sin asignar o
inactivo para el hardfork elegido genera un error en su PC exacto.

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
y extrae la porción runtime copiada. Un campo con sólo `0x` opcional se considera
vacío, por lo que un runtime vacío no oculta un fallback de creación útil. El mapa
CBOR Solidity final sólo se elimina si longitud, marcador y una clave `solc`,
`ipfs` o Swarm conocida son válidos.

Hex mal formado, dígitos impares, placeholders de linker sin resolver, artefactos
multi-contrato ambiguos, límites de metadata inválidos o código vacío producen
errores accionables. `BytecodeLoadOptions::ArtifactContract` selecciona
`Contract` o `path/File.sol:Contract`. Un nombre no cualificado se rechaza si
varios archivos lo definen, evitando que el orden del artefacto elija mal.

EVM está registrado en el loader central, no oculto tras un plugin backend. CLI,
API C, desensamblador, CFG y consultas IR reciben así la misma imagen normalizada
y las mismas opciones.

## Hardforks y opcodes

Se cubren los 150 opcodes legacy asignados de Frontier a Fusaka, incluidos
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
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2) y figura
como [eliminado de Osaka y sin fecha](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md).
NeverD no trata la propuesta retirada como comportamiento mainnet.

El modo strict rechaza bytes desconocidos o inactivos. `--evm-relaxed` los
conserva en LowIR y diagnósticos, pero los backends fallan al ejecutarlos; nunca
se convierten silenciosamente en NOP.

## Arquitectura de metadata al estilo LLVM

La metadata EVM mantenida a mano sigue el patrón `.def` multi-incluido de LLVM:

- `EVMOpcodes.def` es la única fuente para 150 opcodes finalizados y cuatro de
  desarrollo opt-in. Encoding, cambios pop/push reales, tipo de inmediato,
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
  exclusiones históricas/retiradas deliberadas;
  `scripts/audit_evm_opcode_metadata.py` rechaza deriva de bytes y cualquier
  constante upstream nueva que no haya sido revisada.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` y
  `OutputLanguages.def` generan enums ordenados, parsers, nombres, opciones CLI
  y valores ABI C. `EVMConstants.h` centraliza anchos, límites y nombres.
- `EVMCalls.def` describe las cuatro instrucciones que llaman a otro programa y
  el retículo de procedencias de una dirección de callee. Un único flag por
  registro, si un operando de value se sitúa entre el callee y la ventana de
  argumentos, deriva todas las posiciones posteriores, y la tabla se valida
  contra la base de datos de opcodes para que la derivación no se desvíe de los
  pops declarados.
- `EVMPrecompiles.def` es el diccionario de direcciones en las que responde el
  propio protocolo, cada una con el fork que la reservó. El gas está ausente a
  propósito: el coste de una precompile es función de su entrada y se ha
  re-tarifado sin que cambien la dirección ni la operación.
- `EVMRecoveredFacts.def` posee las grafías de los vocabularios de hechos
  recuperados, de modo que un nombre que llega a la salida vive en un solo
  lugar y no en un `switch` del que puede quedar fuera un nuevo enumerador.
  `EVMKnownSignatures.def` hace lo mismo con los tres roles de una firma.
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

  `AnalyzeOptions::MaxAbstractValuesPerSlot` limita cada conjunto finito; al
  superarlo, el slot se amplía a `Unknown`. `MaxStackHeightVariants` limita las
  alturas dependientes del camino en un bloque y, al superarse, produce un error
  explícito de límite de análisis en lugar de truncar el CFG. Ambos límites
  rechazan cero. Los valores finitos creados por una operación cartesiana tras
  una unión de pila no relacional se marcan como sobreaproximaciones: los
  candidatos inválidos se diagnostican, pero no hacen que el análisis estricto
  rechace bytecode sólo por perder la correlación entre slots. Los destinos
  inválidos precisos siguen fallando en el PC exacto del salto. En modo relajado,
  los fallos de pila se diagnostican y terminan sólo el camino abstracto afectado;
  no se inventa un fallthrough imposible tras el fallo.
- **EVM MedIR** representa cada valor de pila como SSA de 256 bits y conecta
  todos los phi de unión antes de ejecutar una worklist dispersa y determinista
  de constantes. Su lattice privado es `Uninitialized`, una `Constant` exacta u
  `Overdefined`: las constantes iguales se propagan entre bloques y ciclos phi
  anclados, mientras que un ciclo conflictivo o dependiente de runtime no puede
  inventar una constante. La worklist comprueba los ID def-use y usa el mismo
  evaluador ALU de `Semantics.h` que el intérprete. MedIR también conserva el
  efecto semántico primario y, de forma ortogonal, el acceso a memoria EVM
  `none/read/write/readwrite`, al estado de nivel fuente y al call-value. En este
  límite, una pila LowIR polimórfica se alinea conservadoramente por el tope; los
  slots ausentes en algún camino se vuelven valores desconocidos explícitos y un
  diagnóstico determinista registra la pérdida de precisión.
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
  fuerza una recuperación `nonpayable` conservadora. Hasta que MedIR tenga
  memory SSA, la recuperación del payload de custom errors y la de la llamada
  saliente son las únicas heurísticas de ventana de instrucciones acotada; los nombres y tipos recuperados siguen
  siendo explícitamente heurísticos.

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
