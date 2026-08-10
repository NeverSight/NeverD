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

Extender NeverD al **bytecode EVM** con lifting 1:1 a la misma pila IR y salida C, Solidity y LLVM IR.

### Objetivos

- Loader EVM · lifter 1:1 (strict) · pila/memoria · JUMP/JUMPI → CFG · storage/calldata · C23/Solidity/LLVM · CLI/C API unificados

**Estado:** Completo para EVM legacy de Frontier a Fusaka: 150 opcodes, entradas
raw/hex/artifact, extracción runtime, CFG y stack-SSA, análisis strict/relaxed,
backends C23/LLVM/Solidity, CLI/C API y diferenciales contra Anvil. Consulte
[descompilación EVM](../evm.es.md) para la ABI host y los límites.

### Por qué EVM

- Fidelidad para auditoría · un motor para nativo y contratos · sin omisiones silenciosas

---

## 3. Descompilación Solana eBPF (SBF)

Programas **Solana eBPF / SBF** con la misma semántica strict.

### Objetivos

- Loader SBF · lifter eBPF/SBF 1:1 · Account/CPI · mismo pipeline · API unificada

**Estado:** La compatibilidad con los contratos actuales de Anza `sbpf` v0-v4 está completa. La implementación admite ELF heredados con secciones/reubicaciones y ELF estrictos basados solo en program headers, una base de instrucciones versionada completa, verificación estricta, IR Low/Med/High por etapas, observaciones de syscall/CPI/account, LLVM verificado, C11 portable, Rust estable y seguro, integración CLI/C API y un oracle semántico independiente y acotado para bytecode sin procesar. v4 sigue el upstream; que pueda desplegarse o ejecutarse en un clúster concreto sigue dependiendo de la activación de funcionalidades de ese clúster. Consulta [Descompilación de Solana SBF](../sbf.es.md).

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

Los formatos nativos y las descompilaciones EVM y Solana SBF están terminados y cubiertos por regresión. Sin fechas comprometidas.

| Función | Estado |
|---------|--------|
| Completitud formatos nativos (PE ARM*, Mach-O i386) | Completa |
| Descompilación EVM | Completa — C, Solidity y LLVM; cubierta por regresión |
| Descompilación Solana eBPF (SBF) | Completa — v0-v4, C, Rust y LLVM; cubierta por regresión |
| Endurecimiento motor y producto | Continuo |
