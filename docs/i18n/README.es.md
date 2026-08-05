**Idiomas**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**El motor de análisis y descompilación AI-friendly — lift 1:1, basado en LLVM**

PE · ELF · Mach-O &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 &nbsp;|&nbsp; SDK C puro

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#compilación)
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O-informational.svg)](#objetivos-soportados)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM-orange.svg)](#objetivos-soportados)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-lightgrey.svg)](#sdk-y-plugins)

[Documentación](../README.es.md) · [Hoja de ruta](../roadmap/README.es.md) · [Contribuir](#contribuir)

</div>

---

> GitHub siempre muestra el `README.md` en inglés en la página del repositorio. Use los enlaces de idioma de arriba para las versiones localizadas.

## Resumen

NeverD es un motor de análisis y descompilación de binarios nativos centrado en el **lifting de instrucciones 1:1**. Carga **PE**, **ELF** y **Mach-O**, decodifica con [Capstone](https://www.capstone-engine.org/) y eleva mediante un pipeline IR de cuatro etapas con **semántica escrita a mano** — no una traducción aproximada. El objetivo es **100 % de fidelidad semántica**: las instrucciones soportadas conservan su comportamiento observable completo en **LLVM IR**, **C estructurado** o un **binario reescrito**.

El modo strict está **activado por defecto**. Una instrucción sin lifter lanza `UnliftedInstruction` en lugar de omitir, adivinar o emitir un `NOP` silencioso.

CLI, integradores y agentes de IA usan un solo motor — **`libneverd`** — mediante una **API C pura**. No enlazan Capstone, LLVM ni el C++ interno directamente.

Las próximas versiones añadirán descompilación [EVM](../roadmap/README.es.md#2-descompilación-de-bytecode-evm) y [Solana eBPF / SBF](../roadmap/README.es.md#3-descompilación-solana-ebpf-sbf) en la misma pila IR — ver la [hoja de ruta](../roadmap/README.es.md).

## ¿Por qué NeverD?

- **Semántica 1:1** — lifters a mano; opcodes no soportados lanzan excepción en modo strict por defecto
- **Compatible con LLM** — C estructurado, LLVM IR y análisis JSON mediante una API C pura, con errores deterministas
- **Un pipeline, tres salidas** — `lift` → LLVM IR · `decompile` → C · `patch` → binario reescrito
- **Reescritura binaria** — PE / ELF / Mach-O con trampolines de sección o sobrescritura inplace
- **Kit de análisis** — CLI, info de depuración, firmas, plugins y pases de ofuscación opcionales

## Objetivos soportados

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> La cobertura de integración de Mach-O i386 usa objetos `thin` reubicables y pruebas del backend de reescritura de ejecutables; el host macOS actual no puede enlazar ejecutables i386 históricos.

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

# Análisis
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

**Artefactos**

| Ruta | Descripción |
|------|-------------|
| `build/bin/neverd` | CLI unificada |
| `build/bin/neverd-bench` | Banco de pruebas (JSON) |
| `build/bin/neverd-sigmaker` | Generador `.pat` desde bibliotecas estáticas |
| `build/bin/libneverd.*` | Biblioteca compartida del motor |
| `build/bin/sdk/` | `NeverDCAPI.h`, `NeverDPlugin.h` |
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
| `decompile` | `.c` | C estructurado (HighIR) |
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

Compile el plugin de ejemplo con `-DNEVERD_BUILD_PLUGINS=ON`. Rutas de carga: `<neverd-dir>/plugins`, `~/.neverd/plugins`, `$NEVERD_PLUGIN_PATH`.

## Dependencias

| Componente | Rol | Fuente |
|------------|------|--------|
| **LLVM** (fork) | IR, optimización, codegen, diagnósticos | `third_party/llvm-project` o precompilado |
| **Capstone** | Decodificación | `third_party/capstone` |

Los componentes de terceros conservan sus propias licencias.

## Contribuir

El estilo sigue las convenciones de LLVM (`.clang-format`).

El desarrollo se realiza en la rama **`dev`** (rama predeterminada en GitHub).

```bash
git clone -b dev https://github.com/NeverSight/NeverD.git
cd NeverD
git submodule update --init --recursive
```

## Licencia

[AGPL-3.0](../../LICENSE)

Los componentes LLVM conservan su licencia Apache-2.0 WITH LLVM-exception. Capstone conserva su propia licencia.
