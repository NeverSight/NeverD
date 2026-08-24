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

Un número de versión no es en sí una especificación, por lo que
`SBFVersionFeatures.def` contiene los cambios de comportamiento y la tabla de
versiones los compone. Cada registro lleva la propuesta SIMD que aceptó el
cambio y el predicado que `anza-xyz/sbpf` expone para la misma pregunta, porque
varias propuestas aterrizan en una versión y una propuesta cambia varias cosas
sin relación: SIMD-0173 mueve las clases de instrucciones de memoria y retira
`lddw`, mientras que SIMD-0174 añade de forma independiente la clase PQR en la
misma versión. Registrar la propuesta en la característica y no en la versión
es lo que mantiene una versión recuperada trazable hasta el documento que la
decidió, y es la razón de que las dos reglas de `callx` sean características
distintas: SIMD-0173 lee el registro fuente y SIMD-0377 el de destino.

Los cambios v2 no pasan intencionadamente a v3. Los feature checks son explícitos,
no suposiciones `version >= N`. Strict, por defecto, rechaza headers, rangos o
alineaciones mal formados, secciones legacy writable no soportadas, continuations,
registros, escrituras frame-pointer o branches inválidos y opcodes inactivos,
indicando slot y dirección virtual.

## El runtime del que habla una descripción

La versión de la ISA viene del archivo. Casi nada más viene de ahí. Qué syscalls
resuelven depende de la cadena y del slot; en qué bytes cae un campo de cuenta
depende del loader que posee el programa; si el entrypoint recibe un segundo
argumento depende de un interruptor que acciona la cadena; y si un programa puede
desplegarse es una pregunta distinta de si se ejecuta. Un único conmutador de
versión no puede expresar nada de eso, así que estos son ejes separados con
tablas separadas.

`SBFRuntimeFeatures.def` registra clusters, propósitos y las gates que cambian lo
que NeverD informa, cada una con su identificador de runtime, la cuenta feature
cuyo estado registra la activación y el slot en el que cada cluster la activó.
Una cuenta pending puede existir sin activar su gate. Una gate sin
fila para un cluster no se ha activado allí. `simd-0321` está activa en todos los
clusters; `simd-0449` y el syscall SHA-512 lo están en testnet y devnet y no en
mainnet, que es exactamente por lo que un programa que funciona en devnet falla
en mainnet.

En la revisión fijada de Agave, la gate
`syscall_parameter_address_restrictions` (`simd-0459`) endurece el contrato de
dirección VM y alineación de los parámetros de syscall y CPI; el estado RPC
finalizado registra su activación en los slots 429,840,000 de mainnet,
407,468,256 de testnet y 462,240,000 de devnet. La gate
`account_data_direct_mapping` sustituye la copia de datos de cuenta en el búfer
de entrada por regiones de memoria con respaldo directo cuando se usa el
espacio de direcciones ajustado; no está activa en mainnet y se activa en los
slots 408,332,256 de testnet y 463,968,000 de devnet. Ninguna gate crea una ABI
de cuenta nueva ni cambia los offsets lógicos de ABIv0/ABIv1: el loader
propietario sigue eligiendo la serialización y NeverD registra ambas como
metadatos de topología del runtime.

Los bits de feature siguen siendo append-only. Como el snapshot observable ya
supera 32 bits, `RuntimeFeatureMask` es el único tipo `uint64_t` de almacenamiento
y host ABI. `RuntimeFeatureDisposition` distingue un `RuntimeBranch` vivo de un
El ancho del ABI v2 queda congelado y no se amplía in-place; más de 64 bits requieren v3 o una representación multiword, nunca cambiar el ancho de v2.
`FoldedBranch` cuyo lado activo es incondicional en la revisión fijada, aunque su
lado antiguo sigue importando en slots históricos. Activaciones del RPC
finalizado (`—` significa no activado):

| gate | domain / disposition | mainnet | testnet | devnet |
|------|----------------------|---------|---------|--------|
| `disable_deploy_of_alloc_free_syscall` | `ProgramAdmission` / `FoldedBranch` | 209,088,008 | 195,356,264 | 224,208,000 |
| `enable_bpf_loader_set_authority_checked_ix` | `LoaderManagement` / `RuntimeBranch` | 251,424,000 | 247,628,260 | 255,744,000 |
| `remove_bpf_loader_incorrect_program_id` | `LoaderManagement` / `FoldedBranch` | 237,168,000 | 224,300,256 | 247,104,000 |
| `simplify_alt_bn128_syscall_error_codes` | `SyscallSemantics` / `FoldedBranch` | 274,320,000 | 278,300,256 | 308,448,000 |
| `abort_on_invalid_curve` | `SyscallSemantics` / `RuntimeBranch` | 311,904,000 | 300,764,256 | 342,576,000 |
| `deplete_cu_meter_on_vm_failure` | `VMFaultPolicy` / `RuntimeBranch` | 327,888,000 | 319,340,257 | 364,176,000 |
| `fix_alt_bn128_multiplication_input_length` | `SyscallSemantics` / `FoldedBranch` | 361,152,000 | 346,988,256 | 397,440,000 |
| `raise_cpi_nesting_limit_to_8` | `CPIExecution` / `RuntimeBranch` | — | — | — |
| `increase_cpi_account_info_limit` | `CPIExecution` / `FoldedBranch` | 403,056,000 | 385,868,256 | 435,456,000 |
| `poseidon_enforce_padding` | `SyscallSemantics` / `FoldedBranch` | 406,080,000 | 385,868,256 | 438,048,000 |
| `fix_alt_bn128_pairing_length_check` | `SyscallSemantics` / `FoldedBranch` | 406,944,000 | 385,868,256 | 438,480,000 |
| `alt_bn128_little_endian` | `SyscallSemantics` / `RuntimeBranch` | 425,088,000 | 406,604,256 | 456,192,000 |
| `enable_alt_bn128_g2_syscalls` | `SyscallSemantics` / `RuntimeBranch` | 425,520,000 | 406,604,256 | 457,056,000 |
| `loader_v3_minimum_extend_program_size` | `LoaderManagement` / `RuntimeBranch` | 432,864,000 | 416,540,256 | 470,880,000 |

Este alcance no pretende cubrir todo el `FeatureSnapshot` de Agave. NeverD sólo
incluye gates de loader, verifier, VM, entry/input, syscall e infraestructura
CPI cuando cambian directamente el decoding o el host contract emitido. El
scheduling de transacciones, fees, consenso, verificación de precompile a nivel
de transacción y la semántica de negocio de un `CPI target built-in` pertenecen
al `external runtime`; añadir sus bits sin implementar esos built-ins anunciaría
una capacidad inexistente.

`SBFLoaders.def` registra propiedad y serialización. Desplegar y ejecutar dejaron
de ser la misma respuesta hace años: `loader-v1` y `loader-v2` rechazan toda
instrucción de gestión que se les envía y siguen ejecutando los programas que ya
poseen, y por eso su serialización todavía tiene que poder leerse.

| Loader | Serialización | Despliega | Ejecuta |
|--------|---------------|-----------|---------|
| loader-v1 | `abi-v0` | no | sí |
| loader-v2 | `abi-v1` | no | sí |
| loader-v3 | `abi-v1` | sí | sí |
| loader-v4 | `abi-v1` | no | no (built-in retirado) |

`SBFAccountLayout.def` sitúa cada campo de cuenta bajo cada serialización. Las
dos no difieren sólo en el relleno: ordenan los campos de forma distinta, de modo
que en el offset tres la forma no alineada tiene el primer byte de la dirección
de la cuenta y la alineada tiene su flag de ejecutable, y nada en el valor
anuncia cuál se leyó. Una cuenta repetida ocupa además un byte en `abi-v0` y ocho
en `abi-v1`, lo que desalinea un recorrido por las entradas y no un único campo.

Que una llamada resuelva son tres preguntas y no una, así que
`SBFSyscallLifecycle.def` guarda cuán asentada está la firma publicada y
`SBFSyscallRegistration.def` guarda el resto: en qué registry aparece un syscall,
qué gate lo gobierna y hacia dónde apunta esa gate. La dirección importa porque
una gate puede quitar algo con la misma facilidad con la que lo añade —activar
`disable_fees_sysvar` es lo que eliminó el syscall del sysvar de fees— y leer una
gate que quita como si añadiera invierte la respuesta para todos los clusters a
la vez. `sol_alloc_free_` sigue registrado para ejecución a ambos lados del
límite. Deployment lo registraba antes de
`disable_deploy_of_alloc_free_syscall` y lo rechaza desde el slot de activación
específico de cada cluster. La revisión de Agave fijada ha plegado el lado activo
de deployment en la construcción del registry; NeverD conserva la gate para que
un perfil histórico obtenga la respuesta anterior a la activación.

En un runtime que ha activado `simd-0321`, el entrypoint recibe además la
dirección de los datos de la instrucción en `r2`. NeverD lo modela como una clase
de valor propia y no como una constante, porque dónde cae depende de las cuentas:
inventar una dirección permitiría informar de una carga a través de ella como
campo de cuenta con nombre. Antes de la activación el registro llega a cero, y un
programa que lo lee lee un cero. Por eso los entry points generados en LLVM, C y
Rust toman el búfer de input y los datos de la instrucción: un callable al que no
se le puede dar el segundo no puede reproducir un programa que lo lee.

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

# Indicar de qué runtime habla la respuesta. Nada de esto está en el archivo
# del programa.
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

`--sbf-cluster`, `--sbf-slot`, `--sbf-loader` y `--sbf-purpose` seleccionan el
perfil de runtime. Los valores por defecto describen mainnet-beta tal como está,
bajo `loader-v3`, para un programa ya desplegado. Preguntar en cambio por el
despliegue informa de los syscalls que mantendrían un programa fuera de la cadena
aunque la cadena lo siguiera ejecutando.

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
| Direcciones base58 en datos de solo lectura | coincidencia en `SBFKnownAddresses.def` y `SBFAnchorNamespaces.def`, o una constante que el código materializa |
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

La recuperación de scratch es bajo demanda: el fixed point de scratch de Solana
CPI/PDA solo se construye cuando existe un `scratch consumer` real; los programas
sin él omiten el `whole-CFG fixed point`. `SBFAnalysisLimits.def` define la
`analysis policy` del host, no `protocol limits`: `MaxModeledScratchBytes` permite
1,024 bytes por `program point`, y `ScratchFlowRetainedByteBudget` es una
`logical retained estimate` de 8,388,608 bytes. Al superar el presupuesto, la
recuperación hace widening explícito a `ScratchRecoveryPrecision::BlockLocal`.
Solo se descartan `cross-block must-facts`; `block-local replay` sigue siendo `sound`
y todavía puede recuperar `same-block stores`.
El printer emite de forma estable la línea `recovery scratch-precision=block-local`,
y widening nunca devuelve `half-converged must-facts`.

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

Solo un syscall de runtime resuelto puede conservar scratch, y únicamente según
sus ventanas de escritura auditadas. Toda llamada interna, indirecta o no
resuelta borra los bytes modelados, incluso cuando ningún argumento actual
apunta a scratch, porque un puntero escapado antes o un alias global aún puede
permitir que el llamado los modifique. `sol_invoke_signed_rust` y
`sol_invoke_signed_c` escriben datos de cuenta y no la memoria del llamador, de
modo que dos invocaciones ensambladas en un mismo bloque quedan ambas legibles.

El modelo es un análisis «must» hacia adelante sobre el CFG intraprocedural: un
byte sobrevive hasta un bloque solo cuando todo camino que llega a él escribió el
mismo valor. Las aristas de llamada no se siguen, porque un llamado no hereda
nada del frame de su llamador. Su worklist de dependencias no tiene una salida
de precisión por número de bloques; un gate Release opcional ejercita el límite
completo de 10 MiB y `1,310,720` instrucciones.

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
#include <stdint.h>

typedef enum neverd_sbf_status {
  NEVERD_SBF_OK = 0,
  NEVERD_SBF_INVALID_INSTRUCTION = 1,
  NEVERD_SBF_MEMORY_ACCESS = 2,
  NEVERD_SBF_DIVIDE_BY_ZERO = 3,
  NEVERD_SBF_DIVIDE_OVERFLOW = 4,
  NEVERD_SBF_CALL_DEPTH = 5,
  NEVERD_SBF_UNKNOWN_SYSCALL = 6,
  NEVERD_SBF_UNKNOWN_FUNCTION = 7,
  NEVERD_SBF_EXECUTION_OVERRUN = 8,
} neverd_sbf_status;
/* v2 is fixed-width: values 0..8 reuse the legacy constants above. */
typedef uint32_t neverd_sbf_status_v2;
enum {
  NEVERD_SBF_INVALID_REGISTER = 9,
  NEVERD_SBF_INVALID_BRANCH = 10,
};
typedef uint64_t neverd_sbf_runtime_feature_mask;
typedef struct neverd_sbf_runtime_features {
  neverd_sbf_runtime_feature_mask bits;
} neverd_sbf_runtime_features;

/* Generated feature constants have the form NEVERD_SBF_RUNTIME_FEATURE_<Name>. */
typedef struct neverd_sbf_syscall_invocation {
  uint32_t hash;
  uint64_t arguments[5];
  neverd_sbf_runtime_features runtime_features;
} neverd_sbf_syscall_invocation;

/* v1 is the exact legacy four-field ABI. */
/* All callback fields return int, including the v2 callback. */
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  /* Legacy syscall callback: hash, five arguments, output value. */
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;

/* The v1 entrypoint reads only the four fields above. */
neverd_sbf_status neverd_sbf_program(
    neverd_sbf_environment *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);

/* v2 is a distinct ABI: the old layout is embedded and never extended in place. */
typedef struct neverd_sbf_environment_v2 {
  neverd_sbf_environment base;
  /* NULL callback falls back to base.syscall. */
  int (*syscall_with_features)(
      void *, const neverd_sbf_syscall_invocation *, uint64_t *result);
  /* NULL selects the program snapshot; a pointer to zero is an explicit empty snapshot. */
  const neverd_sbf_runtime_features *runtime_features;
} neverd_sbf_environment_v2;

neverd_sbf_status_v2 neverd_sbf_program_v2(
    neverd_sbf_environment_v2 *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);
```

`width` se expresa en bits. Cada callback C generado devuelve `int`, incluido
`syscall_with_features`. En el entrypoint v1 `neverd_sbf_program`, cero significa
éxito; cualquier retorno distinto de cero de `load` o `store` se normaliza a
`NEVERD_SBF_MEMORY_ACCESS`, y cualquier retorno distinto de cero de `syscall` a
`NEVERD_SBF_UNKNOWN_SYSCALL`; los contratos son `v1-load-store-nonzero` y
`v1-syscall-nonzero`; v1 no propaga un status exacto del callback.
Los fallos internos `InvalidRegister` e `InvalidBranch` también se normalizan a
`NEVERD_SBF_INVALID_INSTRUCTION` (`internal-invalid-instruction`).
El entrypoint v2 `neverd_sbf_program_v2` es la ruta de status exacto: un valor de
callback reconocido de `neverd_sbf_status_v2`, incluidos 9 y 10, se conserva como
fallo gestionado (`v2-exact-status`). El entrypoint v2 también conserva los fallos internos
`InvalidRegister` e `InvalidBranch` como 9 y 10. Un valor de callback desconocido
usa el fallback específico de la operación generado (`operation-specific-fallback`). Si
`syscall_with_features` es nulo, vuelve a `base.syscall`; su callback también devuelve `int`
(`feature-aware-null-base-syscall`).
El struct y entrypoint v1 siguen siendo compatibles con hosts legacy. Usa el
entrypoint v2 separado para recibir `syscall_with_features` y el snapshot de
runtime features resuelto. El código generado representa registros, return PCs,
r6-r9 callee-saved, frame pointers, direcciones VM, fallos de división, operaciones
PQR anchas y wrapping shifts. Solo se emiten helpers realmente usados, por lo que
la salida mínima pasa `clang -Wall -Wextra -Werror`.

## Contrato host Rust generado

```rust
// The v1 source contract remains Result-based.
pub enum SbfError {
    InvalidInstruction, MemoryAccess, DivideByZero, DivideOverflow,
    CallDepth, UnknownSyscall, UnknownFunction, ExecutionOverrun,
}

#[repr(u32)]
#[non_exhaustive]
pub enum SbfErrorV2 {
    InvalidInstruction = 0, MemoryAccess = 1, DivideByZero = 2,
    DivideOverflow = 3, CallDepth = 4, UnknownSyscall = 5,
    UnknownFunction = 6, ExecutionOverrun = 7, InvalidRegister = 8,
    InvalidBranch = 9,
}

pub struct SbfRuntimeFeatures { bits: u64 }
impl SbfRuntimeFeatures {
    pub const fn from_bits(bits: u64) -> Self { Self { bits } }
    pub const fn bits(self) -> u64 { self.bits }
    pub const fn contains(self, feature: Self) -> bool {
        (self.bits & feature.bits) != 0
    }
}

pub struct SbfSyscallInvocation {
    pub hash: u32,
    pub args: [u64; 5],
    pub runtime_features: SbfRuntimeFeatures,
}

pub enum SbfSyscallOutcomeV2 {
    Unregistered,
    Returned(u64),
    Fault(SbfErrorV2),
}

pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}

pub trait SbfEnvironmentV2 {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfErrorV2>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfErrorV2>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfErrorV2> {
        let _ = (hash, args);
        Err(SbfErrorV2::UnknownSyscall)
    }
    fn syscall_outcome(&mut self, hash: u32, args: [u64; 5])
        -> SbfSyscallOutcomeV2 {
        match self.syscall(hash, args) {
            Ok(value) => SbfSyscallOutcomeV2::Returned(value),
            Err(SbfErrorV2::UnknownSyscall) => SbfSyscallOutcomeV2::Unregistered,
            Err(error) => SbfSyscallOutcomeV2::Fault(error),
        }
    }
    // Some(SbfRuntimeFeatures::from_bits(0)) is an explicit empty snapshot.
    fn runtime_features(&self) -> Option<SbfRuntimeFeatures> { None }
    fn syscall_with_features(
        &mut self, invocation: SbfSyscallInvocation
    ) -> SbfSyscallOutcomeV2 {
        self.syscall_outcome(invocation.hash, invocation.args)
    }
}

pub fn neverd_sbf_program<E: SbfEnvironment>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfError> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated program body")
}
pub fn neverd_sbf_program_v2<E: SbfEnvironmentV2>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfErrorV2> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated v2 program body")
}
```

El entrypoint antiguo `neverd_sbf_program` y `SbfEnvironment` forman el
`v1-result-abi`; sus métodos host usan `Result`. Un
`Some(SbfRuntimeFeatures::from_bits(0))` es el marcador
`explicit-empty-snapshot` y se distingue de `None`. `syscall_outcome` es el
`result-host-bridge` desde el método host basado en Result hasta
`SbfSyscallOutcomeV2`. Como `SbfErrorV2` lleva `#[non_exhaustive]`, los llamadores
deben usar un `non-exhaustive-wildcard` (`_`) al hacer match.

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
/* De qué runtime habla la respuesta. Los valores por defecto describen
   mainnet-beta tal como está, bajo loader-v3, para un programa ya desplegado. */
neverd_sbf_set_cluster(session, "devnet");
neverd_sbf_set_slot(session, 474768000);
neverd_sbf_set_loader(session, "loader-v3");
neverd_sbf_set_purpose(session, "deployment");
/* Optional: name Anchor handlers from the program's own IDL. */
neverd_sbf_set_idl(session, idl_json);
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

## Base de conformidad actual (2026-08-24)

Después de aplicar las relocations, un único `ProgramImage` inmutable y
direccionado por VM es la fuente de verdad para decoder, interpreter,
recuperación de strings y backends LLVM/C/Rust. No existen copias separadas de
text o rodata que puedan divergir de la semántica del loader.

Los registros cerrados viven en `SBFVersions.def`, `SBFOpcodes.def`,
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFVersionFeatures.def`, `SBFProtocolLimits.def`,
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
| Manifest ELF oficial | 23/23 artefactos de `sbpf/tests/elfs` |
| Oracle oficial | `NeverDSBFExternalOracleTests` contrasta 1,411 casos de opcode/límite con el verifier fijado |
| Ejecución diferencial | oracle de bytes raw frente a LLVM ORC, C11 y Rust stable, incluidos memory/fault/syscall trace |
| Agregado integrado | `check-neverd-sbf` ejecuta todas las suites registradas; no se fija un total que cambia con frecuencia |
| ASan + UBSan | los targets enfocados se ejecutan fail-fast sin informes; no se fija un total que cambia con frecuencia |

La auditoría fija Anza `sbpf` en
`2510663bb8d894e8e3094be351e4bb4b604f1f84` y Agave en
`ef210d67f2fabeee1730498188fa78854260c679`. Para actualizarla, revise
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def` y
`SBFUpstreamSources.def`, y ejecute:

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

La comparación mostró que `sol-azy` falla con el ELF strict actual y conserva
un nodo CFG legacy indefinido; `solana-data-reverser` se centra en account data,
`SolDragon` marca el análisis como WIP y `bn-ebpf-solana` requiere Binary Ninja.
Por ello, `sbpf` y Agave oficiales siguen siendo la autoridad semántica.

## Contrato de evidencia auditado el 2026-08-24

`SBFUpstreamSources.def` fija la auditoría en Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679` y Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`. El manifest oficial pasa 23/23;
`NeverDSBFExternalOracleTests` contrasta 1,411 casos opcode/boundary con el
verifier oficial independiente mediante `SBFOfficialOracleProtocol.def` y
`SBFOfficialVerifierCases.def` y `SBFOfficialExecutionConstants.def`. Los ELF dañados proceden de
`SBFOfficialELFMutations.def` y de un corpus tabulado; no se congela un total
que cambia con frecuencia.
Por separado, el `diferencial ELF estricto de 41 casos` ejecuta toda la matriz
strict-v3 mediante el proceso oficial `verify-elf-batch` y NeverD; esos 41 casos
no forman parte del total 1,411.

La matriz oficial adicional de ejecución (`additional execution matrix`) es
independiente: contiene exactamente 508 casos activos `(Version,Opcode)` y 58
casos de frontera, es decir, 566 casos de ejecución exactos. No sustituye ni
se cuenta dentro de los 1,411 `verifier probes` ni del diferencial ELF estricto
de ELF estricto con 41 casos.

`NeverDSBFAgaveConformanceTests` también autentica la revisión de Firedancer
test-vectors `68bb4af40235562e8852fa23d5727e49c2a0b862` y contrasta los 1,955 fixtures
`sol_compat_elf_loader_v1` (1,399 aceptados y 556 rechazados). Para cada ELF
aceptado compara además `entry_pc`, `text_off`, `text_cnt`, `rodata_hash` y
`calldests_hash`. Esta gate comprueba deliberadamente sólo el loader y no
ejecuta el verifier de instrucciones posterior, para mantener separadas las
dos etapas de Agave.

El perfil chain predeterminado sigue siendo fiel a Agave: las filas
`SBF_RUNTIME_VERSION` calculan por cluster/slot histórico el ISA máximo y lo
hacen avanzar de V0 a V1, V2 y V3 al activarse las feature accounts oficiales;
el máximo actual sigue en V3. Usa `RuntimeVersionPolicy::ChainProfile`. Sólo
`--sbf-version=v4` explícito elige
`RuntimeVersionPolicy::UpstreamToolchain` para análisis offline según el `sbpf`
fijado, sin afirmar que v4 esté activado on-chain. El límite actual de 10 MiB
es exactamente `10'485'760` bytes; 65,536 se conserva sólo como provenance/test
histórico y no se aplica en ejecución.

Los registros `.def` tipados son la autoridad para features, syscalls, faults
y source ABI: `SBFSyscallRegistration.def`, `SBFValidationRules.def`, `SBFFaultCodes.def`,
`SBFSourceStatuses.def`, `SBFArgumentRegisters.def` y `SBFEdgeKinds.def`.
`SBFFaultCodes.def` fija los valores execution-fault y
`SBFSourceStatuses.def` posee por separado el ABI del source generado. El
loader es `raw-first`: corrige relative CALL, aplica una sola vez las raw
relocations en orden ELF ordinal y conserva el orden de error text identity,
CALL, relocation, entrypoint y read-only layout. El mapeo file/VM es gap-aware
y nunca inventa bytes en los huecos.

CFG y dataflow son por función: un call edge no es predecessor local, una
shared tail queda ambigua y todos los latches de un bucle forman una sola
región multi-latch. Worklist y ownership se prueban con 10,000 funciones,
bloques en orden inverso y conditional latches, sin adivinar tiempo de máquina.

El call graph público de SBF usa `callgraph-budget=fail-closed`: los límites
tipados de input, provenance, node, edge, element y `CallGraphOutputByteBudget`
hacen que el JSON sea exacto o vacío. Al agotarse devuelve
`{"nodes":[],"edges":[]}`, establece `neverd_last_error()` y nunca publica una
relación parcial.

Cada fila de activación guarda cluster, feature account y slot; un
`RPC activation audit` puede compararla con un nodo vivo manteniendo offline el
análisis normal. La comparación incluye Blueshift, `qedsvm` (pruebas Lean de
rutas seleccionadas, pero su ELF loader actual sólo acepta V0),
`leanprover-solanalib`, `sol-azy`, `bn-ebpf-solana` y Ghidra/SolDragon.
`ezBPF` se declara deprecated en
`88829078a6d7682a2baed0d696d500401c263750` y remite a Blueshift; es un
predecesor archivado con un único mapa byte-to-enum, no un decoder consciente
de versiones para moved-memory, JMP32 y la matriz v0-v4 actual. En esta
auditoría, los pins de comparación son Blueshift
`704e40f7aa82446555b19d9ffbc0a6e18a35480f`, `qedsvm`
`99bd5ede85374adc7fc5c835c2432ecf4e123fd1` y `leanprover-solanalib`
`6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec`; las cuatro herramientas locales
están fijadas como `sol-azy` `362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`,
`solana-data-reverser` `bf90923adec984a61ca0437e9d341360ac1b11ee`, `SolDragon`
`002b98677a5e595a773af6607b77210f5ea71db7` y `bn-ebpf-solana`
`c3fe0de45d37eb68dcb08f2498c6e1f986056572`.
instantánea, NeverD tiene la evidencia reproducible más fuerte que encontramos
entre los decompiladores SBF generales públicos auditados; es una afirmación
comparativa acotada, no un «número uno mundial» absoluto.

La auditoría pública añade `r2ghidra-solana` fijado en
`eca0b8e2d307e00991e289b8f9b0f45743819f1b`, con UX de Ghidra C-like y
`C-like-pdg` para cuentas, Anchor, strings y syscalls; su CI pasó en el HEAD
fijado, pero la suite específica de Solana está comentada y el smoke de CI sólo
decompila `/bin/ls`. La reproducción directa confirma que el
`relative_call_sbpfv0.so` oficial de V0 produce C razonable, mientras el
`relative_call.so` oficial de V3 falla en `pdg`; el resultado es reproducible.
`radare2-solana`, fijado en
`292d845681be377cadc9959a74c2cadeb6e7f412`, amplía SIMD-0173/0174 exclusivos de
V2 a `>=V2` y por tanto a V3/V4, aunque el `program.rs` oficial los declara sólo
V2. `SBPF-3-1`, fijado en `0e602c93007faa96bccb8e1e12040954ff108b6f`, sólo tiene
2/2 pruebas cargo triviales y no CI; la detección de versión es un placeholder
none/V0, el decoder de opcode por high-nibble es incorrecto y el salto usa imm
en vez de off. Los ELF relative_call de V0 y V3 producen el mismo pseudocódigo
erróneo. La ventaja de NeverD es la evidencia oficial reproducible de loader,
verifier, runtime y process-oracle V0–V4, sin negar la UX ni la salida C de estas
herramientas.

`SBFComparisonTools.def` es la única autoridad para los nombres visibles y las
revisiones completas de las herramientas comparadas. El barrido público final,
delimitado, añadió estas conclusiones:

- `blastrock/Solana-eBPF-for-Ghidra`, fijado en
  `c3ad719004726fe924dbed901eca2744ad82c85d`, ofrece UX real de Ghidra P-code,
  pero un único modelo SLEIGH sin versión fija CALLX a `dst` y mezcla opcodes
  legacy/current. No tiene pruebas reales ni CI, y al source predeterminado le
  falta una clase de constantes de relocation que referencia.
- `SolEmu-Ghidra`, fijado en `6520af2ff104d5adbec24632ba3afa3bef0da529`,
  hereda ese decoder idéntico y añade UX de emulador alrededor de comportamiento
  CPI, criptográfico y ZK explícitamente simulado o placeholder; tampoco tiene
  pruebas reales ni CI. `Ghidra_sBPF`, fijado en
  `907bd4476432ca83bb2352686ad1ccafdb38504c`, permite elegir v1-v3 manualmente,
  pero acumula encodings exclusivos de V2 dentro de V3, sin selección automática
  V0/V4, pruebas ni CI.
- `solana-ebpf-ida-processor`, fijado en
  `aacd215907266190ed9c6c1b408ca9309f92ecdd`, es una UI útil de IDA para
  desensamblado/relocations, no un source lifter; su mapa mixto siempre lee CALLX
  de `imm` y carece de pruebas y CI. `solana-bpf-reverse`, fijado en
  `39479a3bddb8cb866ee499266a76a1b54069b222`, genera informes heurísticos y
  esqueletos Rust con TODO a partir de layouts hard-coded; la ejecución dio
  9 pass, 2 fail y 1 skip, sin CI.
- `solens`, fijado en `22defa1c8f4118dacd42f5c291f1ac31609fc0e5`, es un
  desensamblador terminal sólo V2 con 0 pruebas y sin CI. `sbpf-decompiler`,
  fijado en `37b8bc0edc7ce347abee466f5f974e900c1948df`, sólo implementa por ahora
  tres líneas de `Hello, world!`, con 0 pruebas y sin CI.
- `sbpf-eye`, fijado en `5277a52aeb58e50b6ff8f9020414334765369b49`, se declara
  TUI lightweight WIP de instrucciones/CFG: pasan 3 pruebas, pero no tiene IR
  semántico, emisor de source ni CI. `svm_bytecode_analyzer`, fijado en
  `12aa236db8964e6be661e38131c2dc81588cf19c`, es un analizador disassembler/CFG,
  no un lifter; decodifica mal los bytes de registro/offset y su ejecución dio
  17 pass y 1 fail, sin CI.
- `giraffexiu/Solana-eBPF-for-Ghidra`, fijado en
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07`, es un snapshot de un commit del
  mismo linaje Ghidra, sin semántica de versión, pruebas ni CI adicionales.
  `CertSBF`, fijado en `bb93a97cf0c64d119d08ec851e8e820315beb59e`, es una
  valiosa formalización Isabelle/HOL de semántica rBPF antigua, no un decompilador
  source V0-V4 actual de programa completo.

Estos hallazgos sólo refuerzan la evidencia comparativa en el snapshot público
delimitado; no son una conclusión absoluta sobre herramientas futuras o privadas.

La auditoría final de RPC del 2026-08-24 coincidió exactamente: 38 feature
accounts y 89 activation rows; mainnet en el slot 441305159, testnet en 433055669
y devnet en 487238699. La cuenta vacía pendiente y propiedad del sistema
(`VirtualAddressSpaceAdjustments` en mainnet) no estaba activada. No se fija
ninguna URL RPC en la documentación.

Linux Release CI lee los pins exactos con `--print-pinned-revision`,
`--print-test-vectors-revision` y `--print-toolchain`, autentica el oracle y el
corpus sparse, y exporta `NEVERD_SBPF_ORACLE` y
`NEVERD_AGAVE_CONFORMANCE_ROOT`; por ello ambas pruebas externas son
obligatorias. Una ejecución local normal sin env explícito de oracle/corpus
descubre los casos, pero puede omitirlos.
