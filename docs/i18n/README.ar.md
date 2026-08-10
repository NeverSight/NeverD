**اللغات**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center" dir="rtl">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**محرك تحليل وإعادة تجميع صديق للذكاء الاصطناعي — رفع 1:1 مبني على LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; SDK بلغة C خالصة

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#البناء)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-blue.svg)](#sdk-والإضافات)
<br>
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O%20%7C%20EVM%20%7C%20SBF-informational.svg)](#الأهداف-المدعومة)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM%20%7C%20EVM256%20%7C%20SBF-orange.svg)](#الأهداف-المدعومة)

[التوثيق](../README.ar.md) · [خارطة الطريق](../roadmap/README.ar.md) · [المساهمة](CONTRIBUTING.ar.md)

</div>

---

> يعرض GitHub دائمًا `README.md` الإنجليزي في الصفحة الرئيسية للمستودع. استخدم روابط اللغة أعلاه للنسخ المترجمة.

## نظرة عامة

NeverD محرك تحليل وفك ترجمة للبرامج الأصلية والعقود الذكية مبني على **رفع التعليمات 1:1**. يحمّل **PE** و**ELF** و**Mach-O** وبايت كود **EVM** التقليدي وبرامج Solana **SBF ELF**. تستخدم الأهداف الأصلية [Capstone](https://www.capstone-engine.org/)، بينما يملك EVM وSBF decoders واعية بالإصدار وIR مرحليًا. كل المسارات ذات دلالات مكتوبة يدويًا. تحافظ التعليمات على السلوك في **LLVM IR** و**C** و**Rust لـSBF** و**إعادة بناء Solidity لـEVM** أو **ثنائي أصلي معاد كتابته**.

وضع strict **مفعّل افتراضيًا**. تعليمة بلا lifter ترمي `UnliftedInstruction` بدل التخطي أو التخمين أو إصدار `NOP` صامت.

CLI والمكاملون ووكلاء الذكاء الاصطناعي يستخدمون محركًا واحدًا — **`libneverd`** — عبر **واجهة C خالصة**. لا يربطون Capstone أو LLVM أو C++ الداخلي مباشرة.

توثق أدلة [EVM](../evm.ar.md) و[Solana SBF](../sbf.ar.md) صيغ الإدخال وعقود host والحدود.

## لماذا NeverD؟

- **دلالات 1:1** — lifter مكتوب يدويًا؛ العمليات غير المدعومة ترمي استثناءً في strict الافتراضي
- **صديق لنماذج LLM** — C منظّم وLLVM IR وتحليل JSON عبر واجهة C خالصة، بأخطاء حتمية
- **خط أنابيب واحد، مخارج متعددة** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → ثنائي أصلي معاد كتابته
- **إعادة كتابة الثنائي** — PE / ELF / Mach-O بقفزات section أو overwrite inplace
- **مجموعة أدوات التحليل** — CLI، معلومات تصحيح، توقيعات، إضافات، وتمريرات تشويش اختيارية

## الأهداف المدعومة

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> نُفّذت كل خلية في المصفوفة، لكن عمق اختبارات التكامل يختلف. راجع [مصفوفة تغطية المعمارية](../architecture.ar.md#support-and-test-depth). يستخدم Mach-O i386 كائنات `thin` قابلة لإعادة التموضع لأن macOS الحديث لا يستطيع ربط ملفات i386 التنفيذية التاريخية.

يدعم بايت كود EVM التقليدي مستقلًا عن الحاويات الأصلية: تمر كل opcodes المخصصة
وعددها 150 من Frontier إلى Fusaka عبر Low/Med/High IR وLLVM `i256` متحقق وC23
`_BitInt(256)` وSolidity. راجع [فك تجميع EVM](../evm.ar.md).

تستخدم برامج Solana SBF v0-v4 ELF loader صارماً مخصصاً، وmetadata ISA كاملة
حسب الإصدار، وLow/Med/High IR، وLLVM متحققاً منه، وC11 محمولاً، وRust مستقراً
وآمناً. راجع [فك ترجمة Solana SBF](../sbf.ar.md).

## كيف يعمل

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
  → تطبيع runtime + decode واعٍ بالـhardfork
  → EVM LowIR → EVM stack-SSA MedIR → EVM HighIR مستعاد
       ├─ lift        → LLVM i256/i512 متحقق منه
       └─ decompile   → C23 _BitInt(256) أو إعادة بناء Solidity

Solana SBF ELF (v0-v4)
  → loader legacy/strict واعٍ بالإصدار + verifier
  → SBF LowIR → MedIR مطبّع → SBF HighIR مستعاد
       ├─ lift        → LLVM i64 runtime ABI متحقق منه
       └─ decompile   → C11 محمول أو Rust مستقر وآمن
```

| المرحلة | الدور |
|---------|--------|
| **LowIR** | نحو 77 من `NdOp` + CFG |
| **MedIR** | الأنواع، اتفاقيات الاستدعاء، نموذج الذاكرة، SSA |
| **HighIR** | تدفق تحكم منظّم (`if` / `while` / `for`) |
| **LLVM** | تحسين، إخراج C، أو توليد شفرة آلة |

## بداية سريعة

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# خط الأنابيب
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

# التحليل
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sigs --auto binary
```

تُثبَّت مكتبات التوقيع في `build/bin/signatures/` عند البناء. `sigs --auto` يختار المجموعة حسب الصيغة والمعمارية وعرض البت.

## البناء

**المتطلبات:** CMake ≥ 3.20 · Ninja · مترجم C++20 · Git submodule (LLVM fork + Capstone)

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

التكوين الأول يبني fork LLVM محليًا (غالبًا 30–60 دقيقة). البناءات اللاحقة تراكمية. الإعدادات المسبقة: `CMakePresets.json` → `release` / `relwithdebinfo` / `debug`.

<details>
<summary><strong>LLVM جاهز · المخرجات · الاختبارات · خيارات CMake</strong></summary>

<br>

**LLVM جاهز**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

**المخرجات**

| المسار | الوصف |
|--------|--------|
| `build/bin/neverd` | CLI موحّد |
| `build/bin/neverd-bench` | قياس أداء (JSON) |
| `build/bin/neverd-sigmaker` | مولّد `.pat` من مكتبات ثابتة |
| `build/bin/libneverd.*` | مكتبة المحرك المشتركة |
| `build/bin/sdk/` | `NeverDCAPI.h`، `NeverDPlugin.h` |
| `build/bin/signatures/` | مكتبات التوقيع المضمّنة |

**الاختبارات**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| الهدف | الوصف |
|-------|--------|
| `check-neverd` | كل الاختبارات |
| `check-neverd-semantic` | roundtrip دلالي فقط (Unicorn) |

راجع [اختبار NeverD](../testing.ar.md) للاطلاع على الأهداف المركّزة، وتسميات CTest، ومتطلبات fixtures، وشبكة إعادة الكتابة عبر الصيغ.

**خيارات CMake**

| الخيار | الافتراضي | الوصف |
|--------|-----------|--------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | LLVM جاهز لـ CI |
| `NEVERD_BUILD_SHARED` | `ON` | بناء `libneverd` |
| `NEVERD_BUILD_PLUGINS` | `OFF` | إضافات مثال |
| `BUILD_TESTING` | `OFF` | اختبارات وحدات |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### خط الأنابيب

| الأمر | المخرج | الوصف |
|-------|--------|--------|
| `lift` | `.ll` | رفع إلى LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | C أو Solidity لـEVM أو Rust لـSBF عبر `--language` |
| `decompile -llvm` | `.c` | عبر LLVM IR + المحسّن |
| `patch` | ثنائي | إعادة كتابة شفرة الآلة |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>أوامر التحليل</strong></summary>

<br>

| الأمر | الغرض |
|-------|--------|
| `info` / `dashboard` / `headers` | بيانات وصفية ونظرة عامة |
| `funcs` | الدوال المكتشفة |
| `disasm` | تفكيك (`--func` اسم أو hex) |
| `hex` | تفريغ hex عند عنوان |
| `cfg` / `callgraph` | CFG / رسم استدعاء (JSON؛ DOT/SVG اختياري) |
| `xrefs` | مراجع متقاطعة |
| `strings` / `search` | سلاسل / بحث بايت أو نص |
| `imports` / `exports` / `symbols` / `relocs` | جداول |
| `segments` / `sections` / `entrypoints` | التخطيط |
| `diff` | مقارنة ثنائيين (`-a` / `-b`) |
| `sigs` | توقيعات (`--auto`) |
| `rename` / `annotate` / `bookmarks` | تعليقات الجلسة |
| `export` | تصدير النتائج |
| `plugins` | سرد أو تشغيل الإضافات |

معظم أوامر التحليل تقبل `--json`.

</details>

## SDK والإضافات

يستخدم المكاملون **واجهة C الخالصة** من `libneverd`:

| الرأس | الدور |
|-------|--------|
| `NeverDCAPI.h` | جلسة، رفع، إعادة تجميع، patch، IR / CFG، تعليقات |
| `NeverDPlugin.h` | ABI إضافات كمكتبة ديناميكية |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

يختار `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)` Solidity صراحة
لـEVM، بينما يواصل `neverd_decompile_all` إخراج C. راجع
[أمثلة C API لـEVM](../evm.ar.md#c-api).

ابنِ إضافة المثال بـ`-DNEVERD_BUILD_PLUGINS=ON`. مسارات التحميل: `<neverd-dir>/plugins`، `~/.neverd/plugins`، `$NEVERD_PLUGIN_PATH`.

## الاعتماديات

| المكوّن | الدور | المصدر |
|---------|--------|--------|
| **LLVM** (fork) | IR، تحسين، توليد شفرة، تشخيص | `third_party/llvm-project` أو جاهز |
| **Capstone** | فك الشفرة | `third_party/capstone` |

تحتفظ مكوّنات الطرف الثالث بتراخيصها.

## المساهمة

تُدمج المساهمات في فرع **`dev`**. راجع [دليل المساهمة](CONTRIBUTING.ar.md) لإعداد البيئة، وإرشادات Release/Debug، والأسلوب، والاختبارات المركّزة، ومتطلبات pull request. تربط أدلة [المعمارية](../architecture.ar.md) و[الاختبار](../testing.ar.md) التغييرات الشائعة بالشيفرة وحزم التحقق المناسبة.

## الترخيص

[AGPL-3.0](../../LICENSE)

تحتفظ مكوّنات LLVM برخصة Apache-2.0 WITH LLVM-exception. يحتفظ Capstone برخصته الخاصة.
