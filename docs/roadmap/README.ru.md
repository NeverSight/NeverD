**Языки**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← Индекс документации](../README.ru.md)

# Дорожная карта NeverD

Документ описывает основные направления за пределами нативного конвейера PE / ELF / Mach-O. Принципы: **1:1-поднятие**, **strict fail-loud**, общий **четырёхступенчатый IR**.

---

## 1. Завершение нативных форматов

Довести цели, которые loader’ы уже частично узнают.

| Пункт | Заметки |
|-------|---------|
| PE AArch64 | Windows ARM64: unwind/`.pdata`, трамплины, rewrite roundtrip |
| PE ARM32 (Thumb-2) | Windows on ARM — только Thumb |
| Mach-O i386 | Обычные reloc clang; сначала thin objects |

### Принципы

- Не помечать поддержку до форматных тестов
- Не ломать ELF / PE x86 / Mach-O arm64+x64
- Режим инструкций на уровне образа

---

## 2. Декомпиляция байткода EVM

Расширить NeverD на **байткод EVM**: 1:1-поднятие в тот же IR с выводом C, Solidity и LLVM IR.

### Цели

- Loader EVM · 1:1 lifter opcodes (strict) · стек/память · JUMP/JUMPI → CFG · storage/calldata · C23/Solidity/LLVM · единый CLI/C API

**Статус:** Завершено для legacy EVM от Frontier до Fusaka: 150 opcodes,
raw/hex/artifact, runtime extraction, CFG и stack-SSA, strict/relaxed analysis,
backends C23/LLVM/Solidity, CLI/C API и differential tests с Anvil. Host ABI и
ограничения описаны в [декомпиляции EVM](../evm.ru.md).

### Зачем EVM

- Верность для аудита · один движок для натива и контрактов · без тихого пропуска

---

## 3. Декомпиляция Solana eBPF (SBF)

Программы **Solana eBPF / SBF** с той же strict-семантикой.

### Цели

- Loader SBF · 1:1 eBPF/SBF lifter · Account/CPI · тот же конвейер · единый API

**Статус:** Поддержка текущих контрактов Anza `sbpf` v0-v4 завершена. Реализованы устаревшие ELF с секциями/релокациями и строгие ELF только с program headers, полная версионированная база инструкций, строгая верификация, поэтапные Low/Med/High IR, наблюдения syscall/CPI/account, проверенный LLVM, переносимый C11, безопасный стабильный Rust, интеграция CLI/C API и независимый ограниченный семантический oracle для сырого байткода. v4 отслеживается по upstream; возможность развёртывания или выполнения в конкретном кластере по-прежнему зависит от активации функций этого кластера. См. [Декомпиляция Solana SBF](../sbf.ru.md).

### Зачем Solana eBPF

- Важная цель аудита · BPF-ISA подходит MedIR · один C SDK

---

## 4. Укрепление движка и продукта (ongoing)

| Область | Направление |
|---------|-------------|
| Покрытие lifter’ов | Закрывать нативные дыры без ослабления strict |
| Семантические тесты | Расширять Unicorn / roundtrip |
| ABI плагинов | Новые форматы плагинами где уместно |
| Docs / матрица | Обновлять README только после тестов |

---

## Сроки

Нативные форматы и декомпиляции EVM и Solana SBF завершены и покрыты регрессионными тестами. Даты не обещаем.

| Функция | Статус |
|---------|--------|
| Завершение нативных форматов (PE ARM*, Mach-O i386) | Завершено |
| Декомпиляция EVM | Завершено — C, Solidity и LLVM; регрессионное покрытие |
| Декомпиляция Solana eBPF (SBF) | Завершено — v0-v4, C, Rust и LLVM; регрессионное покрытие |
| Укрепление движка и продукта | Постоянно |
