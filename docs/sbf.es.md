**Idiomas**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Descompilación Solana SBF

[← Índice de documentación](README.es.md)

NeverD carga artefactos desplegables de Solana como programas SBF de primera
clase y expone toda la ruta mediante CLI y `libneverd`:

```text
SBF ELF
  → loader ELF y verifier sensibles a la versión
  → LowIR sin pérdidas + CFG
  → MedIR normalizado + hechos de registros
  → funciones, syscalls, observaciones CPI/account y regiones recuperadas
       ├─ LLVM IR verificado
       ├─ C11 portable
       └─ Rust estable y seguro
```

La implementación sigue la VM Anza `sbpf` actual, no eBPF Linux genérico. Los
metadata de version, opcode, syscall, relocation y protocolo viven en bases
`.def` bajo `include/neverd/sbf/`; loaders y backends consumen tablas tipadas
generadas sin duplicar encodings ni nombres.

## Entrada y versiones VM soportadas

La entrada es un programa Solana ELF64 little-endian (`.so`).

| SBF | Layout ELF | Machine ID | Comportamiento ISA importante | Estado |
|-----|------------|------------|-------------------------------|--------|
| v0 | secciones/relocations legacy | `EM_BPF`, `EM_SBPF` | frames fijos con huecos virtuales, LDDW, opcodes memory legacy | legacy |
| v1 | secciones/relocations legacy | `EM_BPF`, `EM_SBPF` | stack frames ajustados manualmente | legacy |
| v2 | secciones/relocations legacy | `EM_BPF`, `EM_SBPF` | aritmética PQR, encodings memory movidos, resta immediate intercambiada, CALLX por registro fuente | legacy, no monótono |
| v3 | program headers strict, sin relocation dinámica | `EM_BPF` | syscalls/calls estáticos, JMP32, CALLX por destino, bytecode en `0x100000000`, rodata en cero | formato actual del toolchain desplegado |
| v4 | program headers strict, sin relocation dinámica | `EM_BPF` | ISA v3 y contrato de memory mapping alineado | upstream `sbpf` actual; varía por cluster |

Los cambios v2 no pasan intencionadamente a v3. Los feature checks son explícitos,
no suposiciones `version >= N`. Strict, por defecto, rechaza headers, rangos o
alineaciones mal formados, secciones legacy writable no soportadas, continuations,
registros, escrituras frame-pointer o branches inválidos y opcodes inactivos,
indicando slot y dirección virtual.

El toolchain actual usa `cargo build-sbf`. Los programas v3+ son Rust y el
toolchain C upstream no apunta a v3; esto no limita a NeverD: toda entrada
aceptada puede emitirse como C o Rust.

- [Programas Solana](https://solana.com/docs/core/programs)
- [Ejecución](https://solana.com/docs/core/programs/program-execution)
- [Referencia syscall](https://solana.com/docs/core/programs/syscall-reference)
- [VM Anza sbpf](https://github.com/anza-xyz/sbpf)
- [Changelog Agave](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

## CLI

```bash
neverd info program.so
neverd headers --json program.so

neverd lift --dump-low program.so
neverd lift --dump-med program.so
neverd lift --dump-high program.so

neverd lift -o program.ll program.so
neverd decompile --language=c -o program.c program.so
neverd decompile --language=rust -o program.rs program.so

neverd lift --sbf-version=v2 program.so
neverd lift --sbf-relaxed --dump-low program.so
```

`--sbf-version=auto|v0|v1|v2|v3|v4` sólo cambia semántica tras validar el layout
detectado. Sirve para fixtures dañadas o de investigación, no para reinterpretar
un archivo no fiable bajo otro estándar de empaquetado.

## Análisis y recuperación

LowIR conserva encoding de ocho bytes, campos crudos, continuations LDDW, calls
resueltos, hashes syscall, bloques, aristas, alcanzabilidad y diagnósticos. MedIR
normaliza encodings por versión a operaciones tipadas de 32/64 bits, extensions
explícitas, aritmética protegida, anchos memory y clases de call. El dataflow de
registros sigue constantes y direcciones stack/rodata.

HighIR recupera funciones entry/internal, aristas directas, nombres syscall
oficiales, strings, loops naturales, condicionales reducibles y observaciones
Solana conservadoras. `sol_invoke_signed_rust`/`sol_invoke_signed_c` son CPI; la
memoria basada en input register es acceso account/input. No inventa tipos
Anchor ni layouts de account sin IDL.

C y Rust comparten una pasada de estructuración neutral. Emite `if`/`if-else` y
`while`/`loop` cuando hay una representación reducible única; calls internos,
CALLX y flujo irreducible conservan el dispatcher PC exacto.

La base syscall cubre logs, memoria, PDA, SHA-256/Keccak/Blake3, Poseidon,
secp256k1, curvas/alt-bn128, exponenciación modular, CPI, return data, sibling
instructions, compute units y sysvars como epoch rewards. Las relocations
`R_BPF_64_64`, `R_BPF_64_RELATIVE`, `R_BPF_64_32` se centralizan. Text relocation,
ambas mitades LDDW y la clave CALL Murmur3 oficial se aplican antes de decodificar.
Si `R_BPF_64_32` ya fue aplicado y eliminado, la clave registry se recalcula
desde symbols y slots para recuperar calls internos.

## Recuperación del programa Solana

Sobre el modelo de máquina SBF, NeverD informa de lo que un programa significa
como programa Solana. Cada hecho registrado lleva la evidencia que lo produjo, y
lo que los bytes no deciden queda sin fijar en lugar de adivinarse.

| Recuperado | Evidencia |
|------------|-----------|
| Direcciones base58 en datos de solo lectura | coincidencia en `SBFKnownAddresses.def`, o una constante que el código materializa |
| La dirección propia declarada | un `sol_memcmp_` de exactamente una longitud de clave contra una constante de solo lectura |
| Despacho de instrucciones Anchor | una comparación de 64 bits cuya constante iguala un discriminator SHA-256 con namespace |
| Destinos de CPI | el registro de instruction alcanzable desde el argumento del invoke |
| La operación que selecciona una llamada | un selector listado en `SBFProgramInstructions.def`, o un discriminator Anchor inicial |
| Semillas de una dirección derivada | el arreglo de descriptores de semilla alcanzable desde el argumento de derivación |
| Lecturas y escrituras de campos de cuenta | un load o store cuya dirección cae demostrablemente en el input serializado |

El loader pasa un argumento, el búfer de input serializado en la base de la
región de input, así que la propagación de constantes desde ese estado inicial da
campos de cuenta con nombre en vez de offsets crudos. `SBFAccountLayout.def`
guarda la serialización oficial; se comprueba que sus campos fijos cubran su
extensión sin hueco.

Anchor deriva un discriminator hasheando `<namespace>:<name>` con SHA-256 y
conservando los ocho primeros bytes, lo cual es unidireccional. Por eso NeverD
solo confirma candidatos: `SBFAnchorNames.def` es un diccionario de nombres
recurrentes y `--sbf-idl` aporta el IDL propio del programa, que tiene
precedencia. Una comparación de 64 bits solo se llama discriminator cuando al
menos una de ellas resuelve a un nombre.

`SBFKnownAddresses.def` registra direcciones de protocolo y de programas
canónicos. Cada entrada debe decodificar a exactamente 32 bytes, algo que la
suite de pruebas exige. La recuperación también necesita la ABI de syscall:
SBPFv3 mapea los datos de solo lectura en la dirección cero, de modo que un
argumento de longitud y una dirección de datos baja son el mismo número.
`SBFSyscalls.def` registra por ello qué registros de argumento llevan una
dirección VM, y solo se siguen esos.

Los dos syscalls de invocación describen la misma instruction con dos
estructuras distintas, y `SBFCPIABI.def` conserva ambos diseños, indexados por el
syscall que los elige. Leer uno con los desplazamientos del otro no falla:
informa en silencio la primera cuenta como programa invocado.
`SBFProgramInstructions.def` nombra después la operación que se pidió a un
programa canónico a partir del selector que publica su propia interfaz: un índice
de variante bincode para los programas system, stake, lookup-table y
upgradeable-loader, y un byte inicial para los programas de token, incluido el
rango de extensiones de Token-2022 sobre la numeración que comparte con el
programa de token original. Un selector no listado se informa como su número.

### Memoria de trabajo y ventanas de syscall

Un programa casi nunca entrega una constante al runtime. Ensambla un arreglo de
semillas, una instruction serializada y su carga útil en su propio frame o en su
heap, y pasa un puntero. Leer solo la imagen cargada mostraría el puntero y nada
de lo que direcciona; por eso la recuperación mantiene un modelo con precisión de
byte de la memoria que solo este programa puede escribir, acotado por
`kMaxModeledScratchBytes`.

Dos hechos deciden qué sobrevive a una llamada. `SBFSyscalls.def` dice qué
registros de argumento llevan una dirección VM; `SBFSyscallMemory.def` dice qué
hace el runtime a través de ellos, como lectura o escritura con una extensión
`Fixed`, `Counted` u `Opaque`. Un syscall sin ventana de escritura no puede
cambiar ningún byte del llamador, así que todo lo probado antes de `sol_log_`
sigue probado después. Una escritura acotada por un argumento de longitud
invalida exactamente esa ventana. Una escritura `Opaque` invalida su dirección
base y todo lo que está por encima, porque un búfer nunca se extiende por debajo
de donde empieza ni cruza el límite de una región VM. El resumen de efectos de
`SBFSyscalls.def` y la tabla de ventanas se validan entre sí en ambos sentidos,
de modo que ninguno puede desviarse solo.

`sol_memcpy_`, `sol_memmove_` y `sol_memset_` se siguen en lugar de solo
invalidarse: con destino, longitud y origen probados, los bytes de destino pasan
a conocerse. Eso es lo que recupera la operación que invoca un programa Anchor,
ya que su carga útil se copia en su lugar en vez de mapearse.

Una llamada a una función que este análisis no ha descrito se supone que escribe
en todo lo que pueda alcanzar. El llamado corre en un frame propio, así que una
llamada cuyos registros de argumento demostrablemente no direccionan memoria de
trabajo deja el modelo intacto; cualquier otra cosa lo descarta.
`sol_invoke_signed_rust` y `sol_invoke_signed_c` escriben datos de cuenta y no la
memoria del llamador, de modo que dos invocaciones ensambladas en un mismo bloque
quedan ambas legibles.

El modelo es un análisis «must» hacia adelante sobre el CFG intraprocedural: un
byte sobrevive hasta un bloque solo cuando todo camino que llega a él escribió el
mismo valor. Las aristas de llamada no se siguen, porque un llamado no hereda
nada del frame de su llamador. Los programas con más de
`kMaxScratchFlowBlocks` bloques conservan la recuperación por bloque y solo
pierden los hechos que cruzan un límite de bloque.

`SBFLints.def` cataloga observaciones de programa completo: falta de comprobación
de signer u owner, un destino de invocación no constante, un syscall obsoleto o
tras feature gate, y una versión SBPF que SIMD-0500 dejará de aceptar para
despliegue. Cada una lleva severidad y confianza, y ningún lint cambia la
semántica decodificada. Nada de esta capa contacta con la red.

## Contrato runtime LLVM generado

LLVM nunca trata una dirección VM como puntero host. Las declaraciones checked
load/store/syscall devuelven status `i32`; load/syscall escriben `i64` por un
puntero de salida. Todo status no cero salta a un bloque de fallo SBF. El módulo
pasa `llvm::verifyModule` antes de salir.

## Contrato host C generado

```c
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;
```

`width` está en bits; un retorno host no cero es un status SBF explícito. Se
representan registros, return PC, r6-r9 preservados, frame pointer, direcciones
VM, fallos de división, PQR ancho y wrapping shifts. Sólo se emiten helpers
usados, por lo que pasa `clang -Wall -Wextra -Werror`.

## Contrato host Rust generado

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

La salida es Rust stable seguro sin punteros crudos. El entry point es genérico
sobre el trait y usa arrays seguros de tamaño fijo. Las pruebas compilan con
`rustc --edition=2021 -D warnings`.

## API C

Tras cargar SBF siguen disponibles funciones de sesión, disassembly, dumps IR,
CFG/call graph JSON, sections, symbols, relocations, strings y headers. Rust se
elige con el enum de lenguaje añadido sin romper ABI.

```c
neverd_session_t session = neverd_session_create();
neverd_sbf_set_strict(session, 1);
neverd_sbf_set_version(session, "auto");
const char *rust = neverd_decompile_all_ex(
    session, "program.so", NEVERD_OUTPUT_RUST, 0, 0);
/* consume rust, then: */
neverd_free_string(rust);
neverd_session_destroy(session);
```

## Verificación y límites

`unittests/sbf/` cubre invariantes metadata, loaders v0-v4, verifier strict,
CFG/recuperación, LLVM verificado, compilación C/Rust sin warnings, intérprete
raw independiente de MedIR y API C. Una fixture conditional+loop se ejecuta en
ambos lenguajes contra el oracle raw; el corpus ELF `sbpf` oficial también se usa
localmente sin incorporar binarios de terceros.

- Rewriting SBF y object-code roundtrip se rechazan explícitamente.
- Anchor IDL/tipos y RPC/accounts live quedan fuera del loader.
- Syscalls y VM memory del source generado pasan por un host contract; no es un
  runtime Solana autónomo.
- Relaxed sólo sirve para inspección; nunca asigna semántica adivinada.

## Base de conformidad actual (2026-08-10)

Después de aplicar las relocations, un único `ProgramImage` inmutable y
direccionado por VM es la fuente de verdad para decoder, interpreter,
recuperación de strings y backends LLVM/C/Rust. No existen copias separadas de
text o rodata que puedan divergir de la semántica del loader.

Los registros cerrados viven en `SBFVersions.def`, `SBFOpcodes.def`,
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFProtocolLimits.def`,
`SBFSyscalls.def`, `SBFSyscallMemory.def`, `SBFCPIABI.def`,
`SBFProgramInstructions.def` y
`SBFUpstreamSources.def`. Los diagnósticos y nombres de bloque LLVM de un solo
uso permanecen locales, siguiendo la convención real de LLVM.

`SBFProtocolLimits.def` registra el valor histórico de 65.536 instrucciones y
el límite actual de 10 MiB para account data; NeverD deriva de este último su
límite conservador de decodificación.

En strict v3/v4, los program headers acotados forman el contrato de runtime;
section y symbol tables son enriquecimiento de debug opcional y no invalidan
una imagen válida si faltan o están dañados. Legacy v0-v2 combina `.text`,
`.rodata`, `.data.rel.ro` y `.eh_frame`; `R_BPF_64_64`,
`R_BPF_64_RELATIVE` y `R_BPF_64_32` se aplican exactamente una vez antes de
inmutabilizar la imagen.

| Evidencia | Resultado auditado |
|-----------|--------------------|
| Manifest ELF oficial | 20/20 artefactos de `sbpf/tests/elfs` |
| Matriz ISA | los 256 encodings para v0-v4, 1,280 celdas, más límites del verifier |
| Ejecución diferencial | oracle de bytes raw frente a LLVM ORC, C11 y Rust stable, incluidos memory/fault/syscall trace |
| Agregado integrado | 124/124 casos en 13 binarios de prueba |
| ASan + UBSan | 121/121 casos core en 12 binarios sin informes |

La auditoría fija Anza `sbpf` en
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` y Agave en
`cae40aa610fdbdb313209bc1eec737079eb59688`. Para actualizarla, revise
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` y
`SBFUpstreamSources.def`, y ejecute:

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

La comparación mostró que `sol-azy` falla con el ELF strict actual y conserva
un nodo CFG legacy indefinido; `solana-data-reverser` se centra en account data,
`SolDragon` marca el análisis como WIP y `bn-ebpf-solana` requiere Binary Ninja.
Por ello, `sbpf` y Agave oficiales siguen siendo la autoridad semántica.
