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

**Estado:** El decode y lifting de opcodes legacy de Frontier a Fusaka están
completos y cubiertos por regresión. La reconstrucción fuente continúa de forma
conservadora: selectors, events, tipos, estándares, nombres y control dinámico
sólo se informan con evidencia suficiente, nunca como fuente original, ABI
completa o conformidad ERC total. Los selectors canónicos de función, las
variantes ABI por estándar y las formas de retorno exitosas permanecen separadas:
un selector ERC compartido no puede inventar un estándar ni tomar un tipo de
retorno incompatible. Amsterdam es un target Review/development
opt-in; `latest` sigue siendo Fusaka. EOFv1/EIP-7692 no está programado y
EIP-3540 está Stagnant, por lo que ninguno se presenta como mainnet final.
Consulte [descompilación EVM](../evm.es.md).

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

## 4. Auditoría y caza de seguridad de memoria

Analizar un binario levantado en busca de defectos de vida del montón (fuga, doble liberación, uso después de liberar) y desbordamientos de copias peligrosas, en JSON estructurado, con un modelo acotado del solver para un desbordamiento demostrado. El análisis corre sobre el IR independiente del formato y la vista de identidad compartida, de modo que **PE, ELF y Mach-O son objetivos equivalentes**, y reutiliza la ejecución simbólica y el solver de vectores de bits internos — sin solver externo ni contenedor.

| Elemento | Notas |
|----------|--------|
| Pista `audit` | Máquina de estados del montón sobre IR + resúmenes de escape: fuga, doble liberación, uso después de liberar |
| Pista `hunt` | Catálogo de sumideros + prefiltro de argumentos + capacidad de destino + testigo del solver |
| Contrato de identidad | Resolución de sumideros por formato (IAT PE, PLT ELF, bind dyld Mach-O) y fuentes de nombres PDB / DWARF / MAP |

**Estado:** Phase 1 está implementada para PE, ELF y Mach-O. P0 incluye análisis de mundo cerrado para el ciclo de vida del heap y copias peligrosas, además de evidencia aditiva del esquema v1 con reproducción `process-input-v1` para valores literales exactos del entorno y el primer consumo de entrada estándar; los demás tipos siguen sin ser reproducibles e incluyen el motivo. P1 cubre desbordamientos de pila/global, lecturas locales no inicializadas y cadenas de formato. Los efectos de llamada desconocidos o parcialmente aplicables permanecen UNKNOWN. La cobertura de veredictos e identidad queda fijada por [`unittests/safety`](../../unittests/safety) y el extremo a extremo [`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp), que ejecuta en cada host la matriz obligatoria PE/ELF/Mach-O × x86-64/AArch64. Véase [Auditoría y caza de seguridad de memoria](../memory-safety.es.md). Las comprobaciones binarias, el fuzzing híbrido y la alcanzabilidad interprocedimental más amplia de P2 quedan como trabajo posterior de la hoja de ruta, fuera de la aceptación de Phase 1.

---

## 5. Endurecimiento del motor y producto (continuo)

| Área | Dirección |
|------|-----------|
| Cobertura del lifter | Cerrar huecos nativos sin relajar strict |
| Pruebas semánticas | Ampliar Unicorn / roundtrip |
| ABI de plugins | Mantener la [ABI de plugins nativos](../plugins.es.md) como contrato de extensión dentro del proceso; los valores Loader y UI siguen siendo metadatos hasta que existan API de host explícitas |
| Docs / matriz | Actualizar README solo tras tests |

---

## Calendario

Los formatos nativos, el decode/lifting EVM legacy hasta Fusaka, Solana SBF y la
seguridad de memoria Phase 1 están cubiertos por regresión. La reconstrucción fuente
EVM conservadora sigue en curso. Sin fechas comprometidas.

| Función | Estado |
|---------|--------|
| Completitud formatos nativos (PE ARM*, Mach-O i386) | Completa |
| Decode/lifting EVM legacy | Completo hasta Fusaka; cubierto por regresión |
| Reconstrucción fuente EVM | En curso — basada en evidencia y conservadora |
| Descompilación Solana eBPF (SBF) | Completa — v0-v4, C, Rust y LLVM; cubierta por regresión |
| Auditoría y caza de seguridad de memoria | Phase 1 completa — análisis P0/P1, evidencia de reproducción y matriz nativa de formatos/arquitecturas presentes; P2 planificada |
| Endurecimiento motor y producto | Continuo |
