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
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFSyscalls.def` y
`SBFUpstreamSources.def`. Los diagnósticos y nombres de bloque LLVM de un solo
uso permanecen locales, siguiendo la convención real de LLVM.

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
| Agregado integrado | 104/104 casos en 13 binarios de prueba |
| ASan + UBSan | 101/101 casos core en 12 binarios sin informes |

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
