**Idiomas**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**El motor de análisis y descompilación AI-friendly — lift 1:1, basado en LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; SDK C puro

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#compilación)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-orange.svg)](#sdk-y-plugins)

[Documentación](../README.es.md) · [Hoja de ruta](../roadmap/README.es.md) · [Contribuir](CONTRIBUTING.es.md)

</div>

---

> GitHub siempre muestra el `README.md` en inglés en la página del repositorio. Use los enlaces de idioma de arriba para las versiones localizadas.

## Resumen

NeverD es un motor de análisis y descompilación nativa y de smart contracts centrado en el **lifting 1:1**. Carga **PE**, **ELF**, **Mach-O**, bytecode legacy **EVM** y programas Solana **SBF ELF**. Los objetivos nativos usan [Capstone](https://www.capstone-engine.org/); EVM y SBF tienen decoders versionados e IR por etapas. Todos los recorridos usan semántica escrita a mano. Las instrucciones preservan su comportamiento en **LLVM IR**, **C**, **Rust para SBF**, **reconstrucción Solidity para EVM** o un **binario nativo reescrito**.

El modo strict está **activado por defecto**. Una instrucción sin lifter lanza `UnliftedInstruction` en lugar de omitir, adivinar o emitir un `NOP` silencioso.

CLI, integradores y agentes de IA usan un solo motor — **`libneverd`** — mediante una **API C pura**. No enlazan Capstone, LLVM ni el C++ interno directamente.

Los formatos de entrada, contratos host y límites se documentan en las guías de [EVM](../evm.es.md) y [Solana SBF](../sbf.es.md).

## ¿Por qué NeverD?

- **Semántica 1:1** — lifters a mano; opcodes no soportados lanzan excepción en modo strict por defecto
- **Compatible con LLM** — C estructurado, LLVM IR y análisis JSON mediante una API C pura, con errores deterministas
- **Un pipeline, varias salidas** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → binario nativo reescrito
- **Reescritura binaria** — PE / ELF / Mach-O con trampolines de sección o sobrescritura inplace
- **Kit de análisis** — CLI, info de depuración, firmas, plugins y pases de ofuscación opcionales

## Objetivos soportados

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Todas las celdas de la matriz están implementadas, pero la profundidad de las pruebas de integración varía. Consulte la [matriz de cobertura de arquitectura](../architecture.es.md#support-and-test-depth). Mach-O i386 usa objetos reubicables `thin` porque macOS moderno no puede enlazar ejecutables i386 históricos.

El bytecode EVM legacy se soporta sin contenedor nativo: los 150 opcodes asignados
de Frontier a Fusaka pasan por Low/Med/High IR, LLVM `i256` verificado, C23
`_BitInt(256)` y Solidity. Consulte [descompilación EVM](../evm.es.md).

Los programas Solana SBF v0-v4 ELF usan un loader strict dedicado, metadatos
ISA versionados completos, Low/Med/High IR, LLVM verificado, C11 portable y
Rust estable y seguro. Consulte [descompilación Solana SBF](../sbf.es.md).

## Cómo funciona

```text
Binary (PE / ELF / Mach-O)
  → Loader + DebugInfo
  → Capstone decode
  → LowIR     architecture-neutral NdOps · CFG
  → MedIR     types · ABI · calls · memory · SSA
       │
       ├─ lift        MedIR → LLVM IR
       ├─ decompile   MedIR → HighIR → C
       │              MedIR → LLVM IR → opt → C   (-llvm)
       └─ patch       MedIR → LLVM IR → codegen → binary

EVM (raw / hex / compiler artifact)
  → normalización runtime + decode sensible al hardfork
  → EVM LowIR → EVM stack-SSA MedIR → EVM HighIR recuperado
       ├─ lift        → LLVM i256/i512 verificado
       └─ decompile   → C23 _BitInt(256) o reconstrucción Solidity

Solana SBF ELF (v0-v4)
  → loader legacy/strict sensible a la versión + verifier
  → SBF LowIR → MedIR normalizado → SBF HighIR recuperado
       ├─ lift        → ABI runtime LLVM i64 verificada
       └─ decompile   → C11 portable o Rust estable y seguro
```

| Etapa | Rol |
|-------|------|
| **LowIR** | ~77 opcodes `NdOp` + CFG |
| **MedIR** | Tipos, convenciones de llamada, modelo de memoria, SSA |
| **HighIR** | Control estructurado (`if` / `while` / `for`) |
| **LLVM** | Optimizar, emitir C o generar código máquina |

## Inicio rápido

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Análisis
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

Las bibliotecas de firmas se instalan en `build/bin/signatures/` en tiempo de compilación. `sigs --auto` elige el conjunto según formato, arquitectura y bitness.

## Compilación

**Requisitos:** CMake ≥ 3.20 · Ninja · compilador C++20 · submódulos Git (fork LLVM + Capstone)

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

La primera configuración compila el fork LLVM localmente (a menudo 30–60 min). Luego, builds incrementales. Presets: `CMakePresets.json` → `release` / `relwithdebinfo` / `debug`.

<details>
<summary><strong>LLVM precompilado · artefactos · pruebas · opciones CMake</strong></summary>

<br>

**LLVM precompilado**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r1
cmake --build build
```

La CI habitual de NeverD, en push y pull request, compila deliberadamente el submódulo LLVM desde las fuentes. Al ejecutar el workflow `CI` manualmente, marque `use_prebuilt_llvm` para validar los paquetes publicados; solo un `true` elegido a mano habilita el LLVM precompilado. Sin marcarlo se mantiene la misma ruta de compilación desde fuentes que en la CI automática.

El paquete publicado se elige según el host que ejecuta CMake:

| Host | Artefacto de release |
|------|----------------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

Cada archivo se coteja con el resumen fijado en `cmake/NeverDLLVMPrebuilt.cmake` —o con el `.sha256` publicado junto a él, para una etiqueta que esos anclajes no describan— antes de extraerlo en `~/.cache/neverd-llvm/<tag>/<arch>/` (o en la ruta que indique `NEVERD_LLVM_PREBUILT_CACHE_DIR`). La compilación de release usa ccache en macOS y Linux; las compilaciones clang-cl de Windows usan sccache con la caché de GitHub Actions como backend. Las cachés del compilador solo aceleran recompilaciones y nunca se publican como artefactos.

La etiqueta de release versiona el paquete de NeverD, mientras que `BUILDINFO.txt` registra el commit exacto del fork de LLVM. Si LLVM sigue informando `23.0.0` pero las fuentes del fork cambiaron, la elección inmutable habitual es una revisión de paquete como `neverd-llvm-v23.0.0-r1` (luego `-r2`), no `23.0.1`, salvo que haya cambiado la propia versión de parche de LLVM. Apunte `NEVERD_LLVM_PREBUILT_TAG` a esa nueva revisión.

Para reparar en el sitio la release mutable `neverd-llvm-v23.0.0`, ejecute el workflow `NeverD LLVM Release` desde la rama `main` de llvm-project y active `overwrite_existing_assets`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0 \
  -f overwrite_existing_assets=true
```

Esto reemplaza los artefactos homónimos pero deliberadamente no mueve la etiqueta Git existente. Actualice en el mismo cambio los resúmenes fijados en `cmake/NeverDLLVMPrebuilt.cmake`: son esos resúmenes, y no la etiqueta, los que nombran la compilación que espera una revisión de NeverD, de modo que un `~/.cache/neverd-llvm/neverd-llvm-v23.0.0/` obsoleto se reemplaza en la siguiente configuración, y un archivo que no coincide con ningún resumen fijado detiene esa configuración con una discrepancia de suma de verificación en lugar de aflorar más tarde como una cabecera que el paquete anterior no traía. Una etiqueta `-rN` nueva evita por completo la reescritura en el sitio. El workflow rechaza el reemplazo accidental mientras la casilla no esté activada, y lo rechaza por completo si GitHub marca la release como inmutable.

**Artefactos**

| Ruta | Descripción |
|------|-------------|
| `build/bin/neverd` | CLI unificada |
| `build/bin/neverd-bench` | Banco de pruebas (JSON) |
| `build/bin/neverd-sigmaker` | Generador `.pat` desde bibliotecas estáticas |
| `build/bin/libneverd.*` | Biblioteca compartida del motor |
| `build/bin/sdk/` | Raíz de includes canónica del C SDK; use `<neverd/sdk/NeverDCAPI.h>` o `<neverd/sdk/NeverDPlugin.h>` conservando la jerarquía `neverd/sdk/` |
| `build/bin/signatures/` | Bibliotecas de firmas |

**Pruebas**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| Objetivo | Descripción |
|----------|-------------|
| `check-neverd` | Todas las pruebas |
| `check-neverd-semantic` | Solo roundtrip semántico (Unicorn) |

Para conocer los objetivos específicos, las etiquetas CTest, los requisitos de fixtures y la matriz de reescritura entre formatos, consulte [Probar NeverD](../testing.es.md).

**Opciones CMake**

| Opción | Predeterminado | Descripción |
|--------|----------------|-------------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | LLVM precompilado CI |
| `NEVERD_BUILD_SHARED` | `ON` | Construir `libneverd` |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Plugins de ejemplo |
| `BUILD_TESTING` | `OFF` | Pruebas unitarias |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### Pipeline

| Comando | Salida | Descripción |
|---------|--------|-------------|
| `lift` | `.ll` | Elevar a LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | C, Solidity EVM o Rust SBF elegido con `--language` |
| `decompile -llvm` | `.c` | Vía LLVM IR + optimizador |
| `patch` | binario | Reescribir código máquina |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>Comandos de análisis</strong></summary>

<br>

| Comando | Propósito |
|---------|------|
| `info` / `dashboard` / `headers` | Metadatos y resumen |
| `funcs` | Funciones descubiertas |
| `disasm` | Desensamblar (`--func` nombre o hex) |
| `sym-explore` | Exploración acotada de rutas LowIR nativas (`--func`; salida JSON) |
| `audit` | Defectos de vida del montón: fuga, doble liberación, uso después de liberar (JSON) |
| `hunt` | Desbordamientos de copias peligrosas con evidencia simbólica y valores candidatos (JSON) |
| `hex` | Volcado hex en una dirección |
| `cfg` / `callgraph` | CFG / grafo de llamadas (JSON; DOT/SVG opcional) |
| `xrefs` | Referencias cruzadas |
| `strings` / `search` | Cadenas / búsqueda de bytes o texto |
| `imports` / `exports` / `symbols` / `relocs` | Tablas |
| `segments` / `sections` / `entrypoints` | Diseño |
| `diff` | Comparar dos binarios (`-a` / `-b`) |
| `sigs` | Firmas (`--auto`) |
| `rename` / `annotate` / `bookmarks` | Anotaciones de sesión |
| `export` | Exportar resultados |
| `plugins` | Listar o ejecutar plugins |

La mayoría de comandos de análisis aceptan `--json`.

</details>

## SDK y plugins

Los integradores usan la **API C pura** de `libneverd`:

| Cabecera | Rol |
|----------|------|
| `NeverDCAPI.h` | Sesión, lift, descompilación, patch, IR / CFG, anotaciones |
| `NeverDPlugin.h` | ABI de plugin en biblioteca dinámica |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

Para EVM, `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)` selecciona
Solidity explícitamente; `neverd_decompile_all` sigue emitiendo C. Consulte los
[ejemplos de API C EVM](../evm.es.md#api-c).

Las bibliotecas compartidas nativas y los archivos `.py` de Python usan el
mismo ciclo de vida de plugins. Compile el ejemplo nativo con
`-DNEVERD_BUILD_PLUGINS=ON`; consulte la
[guía de plugins nativos](../plugins.es.md) para conocer el descriptor en C
puro, los callbacks, los pasos de compilación/enlace, el descubrimiento, el
flujo de la CLI y las restricciones de ABI. Python está habilitado de forma
predeterminada y puede eliminarse por completo con
`-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`; la
[guía de plugins de Python](../python-plugins.es.md) cubre su SDK tipado y el
flujo de empaquetado. Ambos tipos usan `<neverd-dir>/plugins`,
`~/.neverd/plugins` y `$NEVERD_PLUGIN_PATH`.

## Dependencias

| Componente | Rol | Fuente |
|------------|------|--------|
| **LLVM** (fork) | IR, optimización, codegen, diagnósticos | `third_party/llvm-project` o precompilado |
| **Capstone** | Decodificación | `third_party/capstone` |

Los componentes de terceros conservan sus propias licencias.

## Contribuir

Las contribuciones se integran en la rama **`dev`**. Consulte la [guía de contribución](CONTRIBUTING.es.md) para la configuración, las instrucciones de Release/Debug, el estilo, las pruebas específicas y los requisitos de los pull requests. Las guías de [arquitectura](../architecture.es.md) y [pruebas](../testing.es.md) relacionan los cambios habituales con el código y las suites de validación correspondientes.

## Licencia

[AGPL-3.0](../../LICENSE)

Los componentes LLVM conservan su licencia Apache-2.0 WITH LLVM-exception. Capstone conserva su propia licencia.
