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
fusaka, latest
```

Se aceptan `dao`, variantes con guion bajo, `merge`, `prague` y `osaka`.
Actualmente `latest` y `osaka` resuelven a la revisión canónica `fusaka`.

`latest` significa la última revisión mainnet finalizada implementada, no la
cabeza de desarrollo de Ethereum. [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/)
está previsto para Q4 2026; las instrucciones aún en Review
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) y
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) quedan fuera hasta
su finalización. El byte inmediato de EIP-8024 tiene reglas de máscara
`JUMPDEST` distintas de `PUSH`.

EOF se retiró en
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2) y figura
como [eliminado de Osaka y sin fecha](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md).
NeverD no trata la propuesta retirada como comportamiento mainnet.

El modo strict rechaza bytes desconocidos o inactivos. `--evm-relaxed` los
conserva en LowIR y diagnósticos, pero los backends fallan al ejecutarlos; nunca
se convierten silenciosamente en NOP.

## Arquitectura de metadata al estilo LLVM

La metadata EVM mantenida a mano sigue el patrón `.def` multi-incluido de LLVM:

- `EVMOpcodes.def` es la única fuente para los 150 opcodes. Encoding, contrato
  completo de pila, ancho inmediato, clase, fork de activación, efecto principal,
  acceso ortogonal a memoria, estado, call-value y terminación viven en cada
  registro; no hay valores predeterminados silenciosos.
- `EVMMemoryAccesses.def`, `EVMStateAccesses.def` y
  `EVMCallValueAccesses.def` definen dominios cerrados y tipados. `CALL` puede ser
  llamada externa y lectura/escritura de memoria; `EXTCODECOPY`, lectura de
  contexto y escritura de memoria. Estado usa la lattice
  `None/Read/Write/Unknown`. Payability es independiente: `CALLVALUE` normalmente
  implica `payable`; sólo se suprime si el analizador demuestra el guard canónico
  `ISZERO(CALLVALUE)` y que la rama no cero acaba en `REVERT`.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` y
  `OutputLanguages.def` generan enums ordenados, parsers, nombres, opciones CLI
  y valores ABI C. `EVMConstants.h` centraliza anchos, límites y nombres.
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

- **LowIR** conserva PC, encoding, inmediatos PUSH con ceros a la derecha si se
  truncan, bloques, aristas, `JUMPDEST` validados, alcanzabilidad y altura de pila.
- **MedIR** representa la pila como SSA de 256 bits, crea phi, pliega operaciones
  puras y mantiene efecto, memoria, estado y call-value de forma ortogonal.
- **HighIR** recupera selectors, palabras calldata/return probables, mutability,
  slots constantes, events, reverts y regiones function/CFG best-effort. Payability
  y state lattice son independientes. Un salto dinámico alcanzable no resuelto
  une a `Unknown` y fuerza Solidity a `nonpayable`; selectors conflictivos se
  diagnostican y omiten.
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
- Los opcodes Amsterdam en Review están desactivados; `latest` es Fusaka final.
- Sin RPC, descubrimiento de estado, gas/reembolsos ni ejecución de precompiles.
- La extracción de creation reconoce wrappers estáticos, no una transacción completa.
- Los saltos dinámicos quedan indirectos salvo prueba constante acotada.
- Tipos ABI, nombres, mappings, events y errores personalizados son best-effort.
- La ejecución autónoma de efectos requiere hooks host C/Solidity.
