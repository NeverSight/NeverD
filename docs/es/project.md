**Idiomas**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<!-- i18n-source: 9a100b1a0b2a5ec44fc5623ea91ed93b02f978ecbe29cc51a156ea53dcde63ff -->

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**El motor de análisis y descompilación AI-friendly — lift 1:1, basado en LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; SDK de C + Python

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#compilación)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk-y-plugins)

[Documentación](README.md) · [Android](android.md) · [iOS](ios.md) · [Hoja de ruta](roadmap.md) · [Contribuir](CONTRIBUTING.md)

</div>

---

> GitHub siempre muestra el `README.md` en inglés en la página del repositorio. Use los enlaces de idioma de arriba para las versiones localizadas.

<!-- i18n-section: overview -->

## Resumen

NeverD es un motor de análisis y descompilación nativa y de smart contracts centrado en el **lifting 1:1**. Carga **PE**, **ELF**, **Mach-O**, bytecode legacy **EVM** y programas Solana **SBF ELF**. Los objetivos nativos usan [Capstone](https://www.capstone-engine.org/); EVM y SBF tienen decoders versionados e IR por etapas. Todos los recorridos usan semántica escrita a mano. Las instrucciones preservan su comportamiento en **LLVM IR**, **C**, **Rust para SBF**, **reconstrucción Solidity para EVM** o un **binario nativo reescrito**.

El modo strict está **activado por defecto**. Una instrucción sin lifter lanza `UnliftedInstruction` en lugar de omitir, adivinar o emitir un `NOP` silencioso.

CLI, integradores y agentes de IA usan un solo motor — **`libneverd`** — mediante una **API C pura**. No enlazan Capstone, LLVM ni el C++ interno directamente.

Consulta las guías de [EVM](evm.md), [Solana SBF](sbf.md), [recuperación de código móvil](mobile.md) y [recuperación experimental de intérpretes](interpreter-recovery.md) para conocer el uso, los contratos admitidos y las limitaciones.

<!-- i18n-section: why-neverd -->

## ¿Por qué NeverD?

- **Semántica 1:1** — lifters a mano; opcodes no soportados lanzan excepción en modo strict por defecto
- **Compatible con LLM** — C estructurado, LLVM IR y análisis JSON mediante una API C pura, con errores deterministas
- **Un pipeline, varias salidas** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → binario nativo reescrito
- **Reescritura binaria** — PE / ELF / Mach-O con trampolines de sección o sobrescritura inplace
- **Kit de análisis** — CLI, info de depuración, firmas, plugins y pases de ofuscación opcionales

<!-- i18n-section: supported-targets -->

## Objetivos soportados

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Todas las celdas de la matriz están implementadas, pero la profundidad de las pruebas de integración varía. Consulte la [matriz de cobertura de arquitectura](architecture.md#support-and-test-depth). Mach-O i386 usa objetos reubicables `thin` porque macOS moderno no puede enlazar ejecutables i386 históricos.

**El análisis ELF de MIPS es experimental:** MIPS32r2/o32 y MIPS64r2/n64, en ambos órdenes de bytes, admiten un subconjunto estricto de instrucciones enteras para desensamblado, recuperación del CFG y salida en C y LLVM. No se admite la reescritura de código nativo. Consulta [la cobertura de MIPS y el ejemplo de firmware (inglés)](../mips.md) para conocer los subconjuntos microMIPS32, DSP y MIPS32r5 MSA validados por separado y sus restricciones de salida de código fuente.

El bytecode EVM legacy se soporta sin contenedor nativo: los 150 opcodes asignados
de Frontier a Fusaka pasan por Low/Med/High IR, LLVM `i256` verificado, C23
`_BitInt(256)` y Solidity. Consulte [descompilación EVM](evm.md).

Los programas Solana SBF v0-v4 ELF usan un loader strict dedicado, metadatos
ISA versionados completos, Low/Med/High IR, LLVM verificado, C11 portable y
Rust estable y seguro. Consulte [descompilación Solana SBF](sbf.md).

<!-- i18n-section: mobile-source-recovery -->

### Recuperación de código móvil

La CLI experimental `neverd mobile` admite las siguientes entradas y salidas:

| Plataforma | Entradas | Salidas |
|------------|----------|---------|
| [Android](android.md) | APK, incluido multidex, DEX, archivos o directorios smali | Código Java e informe JSON |
| [iOS](ios.md) | IPA, `.app`, Mach-O (arm64/x86_64) | C nativo, código Objective-C/Swift compatible e informe de cobertura JSON |

La recuperación depende de los patrones de código compatibles; consulte la [introducción móvil (inglés)](../mobile.md) y las guías de plataforma para conocer la cobertura y los límites.

<!-- i18n-section: cpu-workloads -->

### Ejecución CPU y entornos invitados

NeverD separa los backends de CPU de los modelos de SO invitado para procesos acotados, bibliotecas nativas Android y controladores Windows x64. Unicorn ejecuta x64/ARM64 por software; los hosts de la misma ISA pueden usar KVM (Linux), WHP (Windows) o HVF (macOS). Consulta [Ejecución de CPU y cargas invitadas](emulation.md) para entornos, validación y límites.

<!-- i18n-section: how-it-works -->

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

<!-- i18n-section: quick-start -->

## Inicio rápido

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# Pseudocódigo en el lenguaje del propio programa (C++, Rust, Go o C)
./build/bin/neverd decompile --language=source -o out.rs rust-binary
./build/bin/neverd decompile --language=cpp -o out.cpp cpp-binary
./build/bin/neverd decompile --language=go --func main.main go-binary

# Transformaciones opcionales de una función nativa (HighC ordinario las omite)
./build/bin/neverd decompile --func main --symbolic-simplify -o enhanced.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=O3 -o optimized.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=custom \
  --llvm-passes=instcombine,early-cse,simplifycfg,dce -o cleaned.c binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Android: de APK a Java (experimental)
./build/bin/neverd mobile app.apk -o recovered-android
./build/bin/neverd mobile App.ipa -o recovered-ios

# Análisis
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

Las bibliotecas de firmas se instalan en `build/bin/signatures/` en tiempo de compilación. `sigs --auto` elige el conjunto según formato, arquitectura y bitness. Si la cabecera Rich de un archivo PE indica la versión de Visual Studio de su enlazador, solo carga el `vs<year>.pat` de esa versión, además de los archivos que no pertenecen a ninguna versión. `--sig-base <dir>` elige de la misma forma desde otro árbol de firmas. Un archivo de patrones de 1 MiB o más se analiza una sola vez: sus módulos se guardan en `neverd/signatures`, dentro del directorio de caché del usuario, y se mapean en las cargas siguientes. `NEVERD_SIGNATURE_CACHE` indica otro directorio, y `off` desactiva la caché.

<!-- i18n-section: building -->

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r6
cmake --build build
```

La CI habitual de NeverD, en push y pull request, compila deliberadamente el submódulo LLVM desde las fuentes. Al ejecutar el workflow `CI` manualmente, marque `use_prebuilt_llvm` para validar los paquetes publicados; solo un `true` elegido a mano habilita el LLVM precompilado. Sin marcarlo se mantiene la misma ruta de compilación desde fuentes que en la CI automática.

El paquete publicado se elige según el host que ejecuta CMake:

| Host | Artefacto de release |
|------|----------------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

Cada archivo se verifica con el resumen fijado en `cmake/NeverDLLVMPrebuilt.cmake`, o con su `.sha256` publicado para etiquetas no descritas allí, antes de extraerlo en `~/.cache/neverd-llvm/<tag>/<arch>/` o `NEVERD_LLVM_PREBUILT_CACHE_DIR`. Para la versión predeterminada, `BUILDINFO.txt` también debe identificar el commit exacto del submódulo LLVM. La publicación usa ccache en macOS/Linux y sccache con la caché de GitHub Actions para clang-cl en Windows. Las cachés solo aceleran recompilaciones y nunca se publican como artefactos.

La revisión predeterminada es `neverd-llvm-v23.0.0-r6`. La etiqueta Git, el destino de publicación, el commit de origen y los tres resúmenes de archivo forman una referencia de origen versionada e inmutable. Los directorios que conservan la etiqueta base antigua, `neverd-llvm-v23.0.0-r1`, `neverd-llvm-v23.0.0-r2`, `neverd-llvm-v23.0.0-r3`, `neverd-llvm-v23.0.0-r4` o `neverd-llvm-v23.0.0-r5` migran automáticamente a `r6`, salvo que se configure `NEVERD_LLVM_PREBUILT_SHA256`. `Prebuilt LLVM Audit` se ejecuta en pushes, pull requests y cada seis horas; llama a `scripts/audit_prebuilt_llvm_release.py` para comparar la referencia con la publicación actual de GitHub y cada archivo de suma de comprobación.

Si cambia el fork de LLVM pero LLVM sigue indicando `23.0.0`, publique la siguiente revisión de paquete, `neverd-llvm-v23.0.0-r7` y luego `-r8`, sin sobrescribir publicaciones ni inventar la versión LLVM `23.0.1`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r7 \
  -f overwrite_existing_assets=false
```

Tras completarse el flujo, actualice juntos la etiqueta predeterminada, el commit fijado y los tres resúmenes en `cmake/NeverDLLVMPrebuilt.cmake`. Cada revisión del paquete usa su propio directorio de caché; la revisión predeterminada actual se guarda en `.cache/neverd-llvm/neverd-llvm-v23.0.0-r6`. Un archivo obsoleto o republicado falla antes de extraerse. `overwrite_existing_assets` solo sirve para recuperación histórica y permanece desactivado en el flujo normal.

**Artefactos**

| Ruta | Descripción |
|------|-------------|
| `build/bin/neverd` | CLI unificada |
| `build/bin/neverd-bench` | Banco de pruebas (JSON) |
| `build/bin/neverd-sigmaker` | Generador `.pat` desde bibliotecas estáticas |
| `build/bin/libneverd.*` | Biblioteca compartida del motor |
| `build/bin/sdk/` | Raíz de includes canónica del C SDK; use `<neverd/sdk/NeverDCAPI.h>` o `<neverd/sdk/NeverDPlugin.h>` conservando la jerarquía `neverd/sdk/` |
| `build/bin/sdk/python/` | Paquete tipado de plugins Python y ejemplos |
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

Para conocer los objetivos específicos, las etiquetas CTest, los requisitos de fixtures y la matriz de reescritura entre formatos, consulte [Probar NeverD](testing.md).

**Opciones CMake**

| Opción | Predeterminado | Descripción |
|--------|----------------|-------------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | LLVM precompilado CI |
| `NEVERD_BUILD_SHARED` | `ON` | Construir `libneverd` |
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | Integrar compatibilidad con plugins CPython 3.10+ |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Plugins de ejemplo |
| `BUILD_TESTING` | `OFF` | Pruebas unitarias |
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | Grupo de pruebas semánticas dependiente de Unicorn (con `BUILD_TESTING=ON`) |

</details>

<!-- i18n-section: desktop-workbench -->

## Entorno de escritorio

El [entorno de escritorio (inglés)](../gui.md) opcional sigue la disposición y los atajos del desensamblador interactivo clásico, con vistas de desensamblado, grafo, pseudocódigo, IR, hexadecimal y listas, un tema al estilo de Visual Studio Code, los 11 idiomas de interfaz y bases de datos de proyecto `.nddb`. El análisis se ejecuta en un proceso separado sin Qt; las compilaciones solo de CLI siguen siendo independientes. Consulte el [registro de validación (inglés)](../gui-qualification.md) para los flujos compatibles y las verificaciones de plataforma pendientes antes de publicar.

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### Pipeline

| Comando | Salida | Descripción |
|---------|--------|-------------|
| `lift` | `.ll` | Elevar a LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | C, Solidity EVM o Rust SBF elegido con `--language` |
| `decompile -llvm` | `.c` | Vía LLVM IR + optimizador |
| `decompile --devirtualize` | `.c` + JSON opcional | Recuperación experimental de intérpretes x64; requiere `--func`; [contrato y ejemplos](interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | Experimental: [Android](android.md), [iOS](ios.md) |
| `patch` | binario | Reescribir código máquina |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-source repl.c --entry replacement --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

Los parches de código fuente pueden usar Clang, el modo ordinario de NeverC o GCC según los [perfiles source/object verificados](../source-patching.md). Los objetos nativos conservan el código máquina del productor; la compatibilidad del compilador y del runtime y la propagación de excepciones se verifican por perfil.

Si un binario ARM de 32 bits omite los metadatos del modo ARM/Thumb de una función, declare el modo de entrada antes de descompilar:

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

Repita la opción para otras entradas ambiguas, usando `:arm` cuando corresponda. Una declaración incompatible con metadatos binarios verificados impide la carga; las declaraciones válidas solo afectan a la entrada exacta. La API C expone el mismo ajuste previo con `neverd_session_set_arm_function_mode()`.

<details>
<summary><strong>Comandos de análisis</strong></summary>

<br>

| Comando | Propósito |
|---------|------|
| `info` / `dashboard` / `headers` | Metadatos y resumen |
| `funcs` | Funciones descubiertas |
| `disasm` | Desensamblar (`--func` nombre o hex) |
| `sym-explore` | Exploración acotada de rutas LowIR nativas (`--func`; salida JSON) |
| `audit` | Defectos del ciclo de vida del heap y lecturas de pila local sin inicializar (JSON) |
| `hunt` | Desbordamientos de copias peligrosas con testigos simbólicos y pruebas adicionales de reproducción `process-input-v1` cuando existe un plan completo (esquema JSON v1) |
| `hex` | Volcado hex en una dirección |
| `cfg` / `callgraph` | CFG / grafo de llamadas (JSON; DOT/SVG opcional) |
| `xrefs` | Referencias cruzadas |
| `switches` | Tablas de saltos de switch del análisis del programa completo |
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

<!-- i18n-section: sdk-and-plugins -->

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
[ejemplos de API C EVM](evm.md#c-api).

Las bibliotecas compartidas nativas y los archivos `.py` de Python usan el
mismo ciclo de vida de plugins. Compile el ejemplo nativo con
`-DNEVERD_BUILD_PLUGINS=ON`; consulte la
[guía de plugins nativos](plugins.md) para conocer el descriptor en C
puro, los callbacks, los pasos de compilación/enlace, el descubrimiento, el
flujo de la CLI y las restricciones de ABI. Python está habilitado de forma
predeterminada y puede eliminarse por completo con
`-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`; la
[guía de plugins de Python](python-plugins.md) cubre su SDK tipado y el
flujo de empaquetado. Ambos tipos usan `<neverd-dir>/plugins`,
`~/.neverd/plugins` y `$NEVERD_PLUGIN_PATH`.

<!-- i18n-section: dependencies -->

## Dependencias

| Componente | Rol | Fuente |
|------------|------|--------|
| **LLVM** (fork) | IR, optimización, codegen, diagnósticos | `third_party/llvm-project` o precompilado |
| **Capstone** | Decodificación | `third_party/capstone` |

Los componentes de terceros conservan sus propias licencias.

<!-- i18n-section: contributing -->

## Contribuir

Las contribuciones se integran en la rama **`dev`**. Consulte la [guía de contribución](CONTRIBUTING.md) para la configuración, las instrucciones de Release/Debug, el estilo, las pruebas específicas y los requisitos de los pull requests. Las guías de [arquitectura](architecture.md) y [pruebas](testing.md) relacionan los cambios habituales con el código y las suites de validación correspondientes.

<!-- i18n-section: license -->

## Licencia

[GNU AGPL solo versión 3](../../LICENSE). Al redistribuir código NeverD cubierto o adaptaciones, conserve los avisos de derechos de autor, licencia y exención de garantía, incluida la atribución y procedencia del proyecto en [NOTICE](../../NOTICE). También se aplica a la reutilización asistida por IA/LLM y a las transformaciones basadas en LLVM.

Consulte [Atribución y citas](ATTRIBUTION.md) para requisitos, alcance y ejemplos. Para facilitar la trazabilidad, recomendamos citar el archivo fuente y la versión o commit exactos. [CITATION.cff](../../CITATION.cff) contiene metadatos de cita del software; citar no sustituye el cumplimiento de la licencia.

Los componentes LLVM conservan su licencia Apache-2.0 WITH LLVM-exception. Capstone conserva su propia licencia.
