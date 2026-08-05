**اللغات**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center" dir="rtl">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**محرك تحليل وإعادة تجميع صديق للذكاء الاصطناعي — رفع 1:1 مبني على LLVM**

PE · ELF · Mach-O &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 &nbsp;|&nbsp; SDK بلغة C خالصة

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#البناء)
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O-informational.svg)](#الأهداف-المدعومة)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM-orange.svg)](#الأهداف-المدعومة)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-lightgrey.svg)](#sdk-والإضافات)

[التوثيق](../README.ar.md) · [خارطة الطريق](../roadmap/README.ar.md) · [المساهمة](#المساهمة)

</div>

---

> يعرض GitHub دائمًا `README.md` الإنجليزي في الصفحة الرئيسية للمستودع. استخدم روابط اللغة أعلاه للنسخ المترجمة.

## نظرة عامة

NeverD محرك تحليل وإعادة تجميع للثنائيات الأصلية مبني حول **رفع التعليمات 1:1**. يحمّل **PE** و**ELF** و**Mach-O**، ويفكّ الشفرة بـ[Capstone](https://www.capstone-engine.org/)، ويرفع عبر خط أنابيب IR من أربع مراحل بـ**دلالات مكتوبة يدويًا** — وليس ترجمة تقريبية. الهدف **وفاء دلالي 100%**: التعليمات المدعومة تحافظ على سلوكها القابل للرصد كاملًا في **LLVM IR** أو **C منظّم** أو **ثنائي معاد كتابته**.

وضع strict **مفعّل افتراضيًا**. تعليمة بلا lifter ترمي `UnliftedInstruction` بدل التخطي أو التخمين أو إصدار `NOP` صامت.

CLI والمكاملون ووكلاء الذكاء الاصطناعي يستخدمون محركًا واحدًا — **`libneverd`** — عبر **واجهة C خالصة**. لا يربطون Capstone أو LLVM أو C++ الداخلي مباشرة.

ستضيف الإصدارات القادمة إعادة تجميع [EVM](../roadmap/README.ar.md#2-إعادة-تجميع-بايتكود-evm) و[Solana eBPF / SBF](../roadmap/README.ar.md#3-إعادة-تجميع-solana-ebpf-sbf) على نفس مكدس IR — انظر [خارطة الطريق](../roadmap/README.ar.md).

## لماذا NeverD؟

- **دلالات 1:1** — lifter مكتوب يدويًا؛ العمليات غير المدعومة ترمي استثناءً في strict الافتراضي
- **صديق لنماذج LLM** — C منظّم وLLVM IR وتحليل JSON عبر واجهة C خالصة، بأخطاء حتمية
- **خط أنابيب واحد، ثلاث مخارج** — `lift` → LLVM IR · `decompile` → C · `patch` → ثنائي معاد كتابته
- **إعادة كتابة الثنائي** — PE / ELF / Mach-O بقفزات section أو overwrite inplace
- **مجموعة أدوات التحليل** — CLI، معلومات تصحيح، توقيعات، إضافات، وتمريرات تشويش اختيارية

## الأهداف المدعومة

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> تستخدم تغطية تكامل Mach-O i386 كائنات `thin` قابلة لإعادة التموضع إلى جانب اختبارات خلفية إعادة كتابة الملفات التنفيذية؛ لا يستطيع مضيف macOS الحالي ربط ملفات i386 التنفيذية التاريخية.

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
| `decompile` | `.c` | C منظّم (HighIR) |
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

ابنِ إضافة المثال بـ`-DNEVERD_BUILD_PLUGINS=ON`. مسارات التحميل: `<neverd-dir>/plugins`، `~/.neverd/plugins`، `$NEVERD_PLUGIN_PATH`.

## الاعتماديات

| المكوّن | الدور | المصدر |
|---------|--------|--------|
| **LLVM** (fork) | IR، تحسين، توليد شفرة، تشخيص | `third_party/llvm-project` أو جاهز |
| **Capstone** | فك الشفرة | `third_party/capstone` |

تحتفظ مكوّنات الطرف الثالث بتراخيصها.

## المساهمة

الأسلوب يتبع عرف LLVM (`.clang-format`).

يُجرى التطوير على فرع **`dev`** (الفرع الافتراضي على GitHub).

```bash
git clone -b dev https://github.com/NeverSight/NeverD.git
cd NeverD
git submodule update --init --recursive
```

## الترخيص

[AGPL-3.0](../../LICENSE)

تحتفظ مكوّنات LLVM برخصة Apache-2.0 WITH LLVM-exception. يحتفظ Capstone برخصته الخاصة.
