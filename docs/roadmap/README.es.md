**Idiomas**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← Índice de documentación](../README.es.md)

# Hoja de ruta de NeverD

Este documento resume las direcciones principales más allá del pipeline nativo PE / ELF / Mach-O. Principios: **elevación 1:1**, **fallo estricto**, **IR de cuatro etapas**.

---

## 1. Completitud de formatos nativos

Cerrar objetivos que los loaders ya reconocen parcialmente.

| Ítem | Notas |
|-------|--------|
| PE AArch64 | Windows ARM64: unwind/`.pdata`, trampolines, roundtrip rewrite |
| PE ARM32 (Thumb-2) | Windows on ARM es solo Thumb |
| Mach-O i386 | Relocs clang comunes; primero thin objects |

### Principios

- No marcar soportado hasta tests de formato
- No romper ELF / PE x86 / Mach-O arm64+x64
- Modo de instrucción a nivel de imagen

---

## 2. Descompilación de bytecode EVM

Extender NeverD al **bytecode EVM** con elevación 1:1 a la misma pila IR.

### Objetivos

- Loader EVM · lifter 1:1 (strict) · pila/memoria · JUMP/JUMPI → CFG · storage/calldata · HighIR/LLVM-C · CLI/C API unificados

### Por qué EVM

- Fidelidad para auditoría · un motor para nativo y contratos · sin omisiones silenciosas

---

## 3. Descompilación Solana eBPF (SBF)

Programas **Solana eBPF / SBF** con la misma semántica strict.

### Objetivos

- Loader SBF · lifter eBPF/SBF 1:1 · Account/CPI · mismo pipeline · API unificada

### Por qué Solana eBPF

- Objetivo de auditoría clave · ISA tipo BPF encaja en MedIR · un solo SDK C

---

## 4. Endurecimiento del motor y producto (continuo)

| Área | Dirección |
|------|-----------|
| Cobertura del lifter | Cerrar huecos nativos sin relajar strict |
| Pruebas semánticas | Ampliar Unicorn / roundtrip |
| ABI de plugins | Nuevos formatos como plugins si encaja |
| Docs / matriz | Actualizar README solo tras tests |

---

## Calendario

Investigación / diseño. Sin fechas comprometidas.

| Función | Estado |
|---------|--------|
| Completitud formatos nativos (PE ARM*, Mach-O i386) | Diseño / implementación temprana |
| Descompilación EVM | Investigación / diseño |
| Descompilación Solana eBPF (SBF) | Investigación / diseño |
| Endurecimiento motor y producto | Continuo |

