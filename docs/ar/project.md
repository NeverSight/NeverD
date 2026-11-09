**اللغات**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](project.md)

<!-- i18n-source: 9a100b1a0b2a5ec44fc5623ea91ed93b02f978ecbe29cc51a156ea53dcde63ff -->

<div align="center" dir="rtl">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**محرك تحليل وإعادة تجميع صديق للذكاء الاصطناعي — رفع 1:1 مبني على LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; حزمتا SDK للغتين C وPython

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#البناء)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk-والإضافات)

[التوثيق](README.md) · [Android](android.md) · [iOS](ios.md) · [خارطة الطريق](roadmap.md) · [المساهمة](CONTRIBUTING.md)

</div>

---

> يعرض GitHub دائمًا `README.md` الإنجليزي في الصفحة الرئيسية للمستودع. استخدم روابط اللغة أعلاه للنسخ المترجمة.

<!-- i18n-section: overview -->

## نظرة عامة

NeverD محرك تحليل وفك ترجمة للبرامج الأصلية والعقود الذكية مبني على **رفع التعليمات 1:1**. يحمّل **PE** و**ELF** و**Mach-O** وبايت كود **EVM** التقليدي وبرامج Solana **SBF ELF**. تستخدم الأهداف الأصلية [Capstone](https://www.capstone-engine.org/)، بينما يملك EVM وSBF decoders واعية بالإصدار وIR مرحليًا. كل المسارات ذات دلالات مكتوبة يدويًا. تحافظ التعليمات على السلوك في **LLVM IR** و**C** و**Rust لـSBF** و**إعادة بناء Solidity لـEVM** أو **ثنائي أصلي معاد كتابته**.

وضع strict **مفعّل افتراضيًا**. تعليمة بلا lifter ترمي `UnliftedInstruction` بدل التخطي أو التخمين أو إصدار `NOP` صامت.

CLI والمكاملون ووكلاء الذكاء الاصطناعي يستخدمون محركًا واحدًا — **`libneverd`** — عبر **واجهة C خالصة**. لا يربطون Capstone أو LLVM أو C++ الداخلي مباشرة.

راجع أدلة [EVM](evm.md) و[Solana SBF](sbf.md) و[استعادة مصادر تطبيقات الهاتف](mobile.md) و[استعادة مصادر المفسّرات التجريبية](interpreter-recovery.md) للاستخدام والعقود المدعومة والحدود.

<!-- i18n-section: why-neverd -->

## لماذا NeverD؟

- **دلالات 1:1** — lifter مكتوب يدويًا؛ العمليات غير المدعومة ترمي استثناءً في strict الافتراضي
- **صديق لنماذج LLM** — C منظّم وLLVM IR وتحليل JSON عبر واجهة C خالصة، بأخطاء حتمية
- **خط أنابيب واحد، مخارج متعددة** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → ثنائي أصلي معاد كتابته
- **إعادة كتابة الثنائي** — PE / ELF / Mach-O بقفزات section أو overwrite inplace
- **مجموعة أدوات التحليل** — CLI، معلومات تصحيح، توقيعات، إضافات، وتمريرات تشويش اختيارية

<!-- i18n-section: supported-targets -->

## الأهداف المدعومة

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> نُفّذت كل خلية في المصفوفة، لكن عمق اختبارات التكامل يختلف. راجع [مصفوفة تغطية المعمارية](architecture.md#support-and-test-depth). يستخدم Mach-O i386 كائنات `thin` قابلة لإعادة التموضع لأن macOS الحديث لا يستطيع ربط ملفات i386 التنفيذية التاريخية.

**تحليل MIPS ELF تجريبي:** يدعم MIPS32r2/o32 وMIPS64r2/n64 بكلا ترتيبي البايتات مجموعة فرعية محددة بدقة من تعليمات الأعداد الصحيحة لفك التجميع واستعادة CFG وإخراج C وLLVM. إعادة كتابة الشيفرة الأصلية غير مدعومة. راجع [نطاق دعم MIPS ومثال البرنامج الثابت (بالإنجليزية)](../mips.md) للاطلاع على المجموعات الفرعية microMIPS32 وDSP وMIPS32r5 MSA التي تم التحقق منها بشكل منفصل، والقيود المفروضة على إخراج الشيفرة المصدرية.

يدعم بايت كود EVM التقليدي مستقلًا عن الحاويات الأصلية: تمر كل opcodes المخصصة
وعددها 150 من Frontier إلى Fusaka عبر Low/Med/High IR وLLVM `i256` متحقق وC23
`_BitInt(256)` وSolidity. راجع [فك تجميع EVM](evm.md).

تستخدم برامج Solana SBF v0-v4 ELF loader صارماً مخصصاً، وmetadata ISA كاملة
حسب الإصدار، وLow/Med/High IR، وLLVM متحققاً منه، وC11 محمولاً، وRust مستقراً
وآمناً. راجع [فك ترجمة Solana SBF](sbf.md).

<!-- i18n-section: mobile-source-recovery -->

### استعادة مصادر تطبيقات الهاتف

تدعم واجهة سطر الأوامر التجريبية `neverd mobile` المدخلات والمخرجات التالية:

| المنصة | المدخلات | المخرجات |
|--------|----------|----------|
| [Android](android.md) | APK بما فيه multidex، وDEX، وملفات smali أو مجلداتها | شيفرة Java وتقرير JSON |
| [iOS](ios.md) | IPA و`.app` وMach-O ‏(arm64/x86_64) | شيفرة C أصلية ومصادر Objective-C/Swift المدعومة وتقرير تغطية JSON |

تعتمد الاستعادة على أنماط الشيفرة المدعومة؛ راجع [نظرة الهاتف العامة (بالإنجليزية)](../mobile.md) وأدلة المنصات لمعرفة التغطية والحدود.

<!-- i18n-section: cpu-workloads -->

### تنفيذ المعالج وبيئات الضيف

يفصل NeverD خلفيات CPU عن نماذج نظام الضيف للعمليات المحدودة ومكتبات Android الأصلية وبرامج تشغيل Windows x64. يوفر Unicorn تنفيذ x64/ARM64 برمجيًا؛ ويمكن للمضيف ذي ISA المطابقة استخدام KVM على Linux أو WHP على Windows أو HVF على macOS. راجع [تنفيذ CPU وأحمال الضيف](emulation.md) للبيئات والتحقق والحدود.

<!-- i18n-section: how-it-works -->

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

<!-- i18n-section: quick-start -->

## بداية سريعة

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# خط الأنابيب
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# شيفرة زائفة بلغة البرنامج نفسه (C++ أو Rust أو Go أو C)
./build/bin/neverd decompile --language=source -o out.rs rust-binary
./build/bin/neverd decompile --language=cpp -o out.cpp cpp-binary
./build/bin/neverd decompile --language=go --func main.main go-binary

# تحويلات اختيارية لدالة أصلية واحدة (يتجاوزها HighC العادي)
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

# Android: من APK إلى Java (تجريبي)
./build/bin/neverd mobile app.apk -o recovered-android
./build/bin/neverd mobile App.ipa -o recovered-ios

# التحليل
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

تُثبَّت مكتبات التوقيع في `build/bin/signatures/` عند البناء. `sigs --auto` يختار المجموعة حسب الصيغة والمعمارية وعرض البت. إذا ذكر ترويسة Rich في ملف PE إصدار Visual Studio الخاص بالرابط، فإنه يحمّل ملف `vs<year>.pat` لذلك الإصدار فقط إلى جانب الملفات التي لا تتبع أي إصدار. ويختار `--sig-base <dir>` بالطريقة نفسها من شجرة توقيعات أخرى. يُحلَّل ملف الأنماط الذي يبلغ 1 MiB أو أكثر مرة واحدة: تُحفظ وحداته في `neverd/signatures` داخل دليل ذاكرة التخزين المؤقت للمستخدم، وتُعيَّن في الذاكرة عند التحميلات اللاحقة. يحدّد `NEVERD_SIGNATURE_CACHE` دليلًا آخر، وتُعطّل القيمة `off` الذاكرة المؤقتة.

<!-- i18n-section: building -->

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r6
cmake --build build
```

تبني الـCI المعتادة لـNeverD، عند الـpush والـpull request، وحدة LLVM الفرعية من المصدر عن قصد. وعند تشغيل سير عمل `CI` يدويًا، اختر `use_prebuilt_llvm` للتحقق من الحزم المنشورة؛ ولا يُفعَّل الـLLVM الجاهز إلا باختيار `true` يدويًا. وتركه دون اختيار يُبقي المسار نفسه: البناء من المصدر كما في الـCI التلقائية.

تُختار الحزمة المنشورة حسب المضيف الذي يشغّل CMake:

| المضيف | مخرَج الإصدار |
|--------|----------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

يُفحَص كل أرشيف مقابل البصمة المثبتة في `cmake/NeverDLLVMPrebuilt.cmake`، أو ملف `.sha256` المنشور للوسوم التي لا تصفها القيم المثبتة، قبل استخراجه تحت `~/.cache/neverd-llvm/<tag>/<arch>/` أو المسار المحدد بواسطة `NEVERD_LLVM_PREBUILT_CACHE_DIR`. ويجب أن يذكر `BUILDINFO.txt` في الحزمة الافتراضية التزام الوحدة الفرعية LLVM نفسه تمامًا. تستخدم إصدارات macOS/Linux أداة ccache، وتستخدم إصدارات Windows clang-cl أداة sccache مع مخبأ GitHub Actions. تسرّع هذه المخابئ إعادة البناء فقط ولا تُنشر كملفات إصدار.

مراجعة الحزمة الافتراضية هي `neverd-llvm-v23.0.0-r6`. يشكّل وسم Git وهدف الإصدار والتزام المصدر وبصمات الأرشيفات الثلاثة مرجع مصدر ذا إصدار غير قابل للتغيير. تنتقل أدلة البناء التي تحتفظ بالوسم الأساسي القديم أو `neverd-llvm-v23.0.0-r1` أو `neverd-llvm-v23.0.0-r2` أو `neverd-llvm-v23.0.0-r3` أو `neverd-llvm-v23.0.0-r4` أو `neverd-llvm-v23.0.0-r5` تلقائيًا إلى `r6` ما لم تُحدّد قيمة `NEVERD_LLVM_PREBUILT_SHA256` صراحة. يعمل `Prebuilt LLVM Audit` عند push وpull request وكل ست ساعات، ويستدعي `scripts/audit_prebuilt_llvm_release.py` لمقارنة المرجع بإصدار GitHub الحالي وكل ملف تحقق منشور.

إذا تغيّر تفرع LLVM بينما لا يزال LLVM يعلن `23.0.0`، فانشر مراجعة الحزمة التالية `neverd-llvm-v23.0.0-r7` ثم `-r8`، بدل الكتابة فوق إصدار موجود أو اختراع إصدار LLVM باسم `23.0.1`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r7 \
  -f overwrite_existing_assets=false
```

بعد نجاح سير العمل، حدّث الوسم الافتراضي والالتزام المثبت والبصمات الثلاثة معًا في `cmake/NeverDLLVMPrebuilt.cmake`. تستخدم كل مراجعة للحزمة دليل تخزين مؤقت خاصًا بها؛ توجد المراجعة الافتراضية الحالية تحت `.cache/neverd-llvm/neverd-llvm-v23.0.0-r6`. ويفشل الأرشيف القديم أو المعاد نشره قبل الاستخراج. يقتصر `overwrite_existing_assets` على الاستعادة التاريخية ويبقى معطّلًا في سير المراجعات المعتاد.

**المخرجات**

| المسار | الوصف |
|--------|--------|
| `build/bin/neverd` | CLI موحّد |
| `build/bin/neverd-bench` | قياس أداء (JSON) |
| `build/bin/neverd-sigmaker` | مولّد `.pat` من مكتبات ثابتة |
| `build/bin/libneverd.*` | مكتبة المحرك المشتركة |
| `build/bin/sdk/` | جذر include الرسمي لـC SDK؛ استخدم `<neverd/sdk/NeverDCAPI.h>` أو `<neverd/sdk/NeverDPlugin.h>` مع الحفاظ على هيكل `neverd/sdk/` |
| `build/bin/sdk/python/` | حزمة إضافات Python ذات معلومات الأنواع وأمثلة |
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

راجع [اختبار NeverD](testing.md) للاطلاع على الأهداف المركّزة، وتسميات CTest، ومتطلبات fixtures، وشبكة إعادة الكتابة عبر الصيغ.

**خيارات CMake**

| الخيار | الافتراضي | الوصف |
|--------|-----------|--------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | LLVM جاهز لـ CI |
| `NEVERD_BUILD_SHARED` | `ON` | بناء `libneverd` |
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | تضمين دعم إضافات CPython 3.10+ |
| `NEVERD_BUILD_PLUGINS` | `OFF` | إضافات مثال |
| `BUILD_TESTING` | `OFF` | اختبارات وحدات |
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | مجموعة الاختبارات الدلالية المعتمدة على Unicorn (عند `BUILD_TESTING=ON`) |

</details>

<!-- i18n-section: desktop-workbench -->

## بيئة سطح المكتب

تتبع [بيئة سطح المكتب (الإنجليزية)](../gui.md) الاختيارية تخطيط أداة التفكيك التفاعلية الكلاسيكية واختصاراتها، مع عروض للتفكيك والرسم البياني والشيفرة الزائفة وIR والبيانات الست عشرية والقوائم، وسمة على طراز Visual Studio Code، وجميع لغات الواجهة الإحدى عشرة، وقواعد بيانات المشاريع `.nddb`. يعمل التحليل في عملية مستقلة لا تعتمد على Qt؛ وتبقى إصدارات CLI فقط مستقلة. يوضح [سجل التأهيل (الإنجليزية)](../gui-qualification.md) مسارات العمل المدعومة واختبارات المنصات المطلوبة قبل الإصدار.

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### خط الأنابيب

| الأمر | المخرج | الوصف |
|-------|--------|--------|
| `lift` | `.ll` | رفع إلى LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | C أو Solidity لـEVM أو Rust لـSBF عبر `--language` |
| `decompile -llvm` | `.c` | عبر LLVM IR + المحسّن |
| `decompile --devirtualize` | `.c` + JSON اختياري | استعادة تجريبية لمفسّرات x64؛ تتطلب `--func`؛ [العقد والأمثلة](interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | تجريبي: [Android](android.md), [iOS](ios.md) |
| `patch` | ثنائي | إعادة كتابة شفرة الآلة |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-source repl.c --entry replacement --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

يمكن لرقع الشيفرة المصدرية اختيار Clang أو الوضع العادي لـ NeverC أو GCC وفق [ملفات source/object التي تم التحقق منها](../source-patching.md). تحتفظ ملفات الكائنات الأصلية بالشيفرة الآلية التي أنتجها المترجم؛ ويُتحقق من توافق المترجم وبيئة التشغيل وانتقال الاستثناءات لكل ملف تعريف.

إذا افتقد ملف ARM ثنائي 32 بت بيانات وضع ARM/Thumb لدالة، فحدّد وضع الدخول قبل فك الترجمة:

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

كرّر الخيار لنقاط الدخول الأخرى الملتبسة، واستخدم `:arm` عند الحاجة. يفشل التحميل إذا تعارض التصريح مع بيانات ثنائية تم التحقق منها، ولا تؤثر التصريحات الصحيحة إلا في نقطة الدخول المحددة. توفّر واجهة C الإعداد نفسه قبل التحميل عبر `neverd_session_set_arm_function_mode()`.

<details>
<summary><strong>أوامر التحليل</strong></summary>

<br>

| الأمر | الغرض |
|-------|--------|
| `info` / `dashboard` / `headers` | بيانات وصفية ونظرة عامة |
| `funcs` | الدوال المكتشفة |
| `disasm` | تفكيك (`--func` اسم أو hex) |
| `sym-explore` | استكشاف محدود لمسارات LowIR الأصلية (`--func`؛ خرج JSON) |
| `audit` | عيوب عمر كائنات الكومة وقراءات المكدس المحلي غير المهيأ (JSON) |
| `hunt` | تجاوزات النسخ الخطرة مع شواهد رمزية وأدلة إعادة تشغيل إضافية من نوع `process-input-v1` عند توفر خطة كاملة (مخطط JSON v1) |
| `hex` | تفريغ hex عند عنوان |
| `cfg` / `callgraph` | CFG / رسم استدعاء (JSON؛ DOT/SVG اختياري) |
| `xrefs` | مراجع متقاطعة |
| `switches` | جداول القفز لعبارات switch من تحليل البرنامج كاملًا |
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

<!-- i18n-section: sdk-and-plugins -->

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
[أمثلة C API لـEVM](evm.md#c-api).

تستخدم المكتبات المشتركة الأصلية وملفات Python ذات اللاحقة `.py` دورة حياة
الإضافات نفسها. ابنِ المثال الأصلي بـ`-DNEVERD_BUILD_PLUGINS=ON`؛ وراجع
[دليل الإضافات الأصلية](plugins.md) للاطلاع على واصف C الخالص،
والاستدعاءات، وخطوات البناء/الربط، والاكتشاف، وسير عمل CLI، وقيود ABI. دعم
Python مفعّل افتراضيًا ويمكن إزالته بالكامل بواسطة
`-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`؛ ويغطي
[دليل إضافات Python](python-plugins.md) حزمة SDK المعرّفة بأنواع وسير عمل
الحزمة. يستخدم النوعان `<neverd-dir>/plugins` و`~/.neverd/plugins`
و`$NEVERD_PLUGIN_PATH`.

<!-- i18n-section: dependencies -->

## الاعتماديات

| المكوّن | الدور | المصدر |
|---------|--------|--------|
| **LLVM** (fork) | IR، تحسين، توليد شفرة، تشخيص | `third_party/llvm-project` أو جاهز |
| **Capstone** | فك الشفرة | `third_party/capstone` |

تحتفظ مكوّنات الطرف الثالث بتراخيصها.

<!-- i18n-section: contributing -->

## المساهمة

تُدمج المساهمات في فرع **`dev`**. راجع [دليل المساهمة](CONTRIBUTING.md) لإعداد البيئة، وإرشادات Release/Debug، والأسلوب، والاختبارات المركّزة، ومتطلبات pull request. تربط أدلة [المعمارية](architecture.md) و[الاختبار](testing.md) التغييرات الشائعة بالشيفرة وحزم التحقق المناسبة.

<!-- i18n-section: license -->

## الترخيص

[GNU AGPL الإصدار الثالث فقط](../../LICENSE). عند إعادة توزيع شيفرة NeverD الخاضعة للرخصة أو الأعمال المعدّلة، احتفظ بإشعارات حقوق النشر والرخصة وانعدام الضمان، بما فيها نسب المشروع ومصدره في [NOTICE](../../NOTICE). ينطبق ذلك أيضًا على إعادة الاستخدام بمساعدة AI/LLM وتحويلات الشيفرة المبنية على LLVM.

راجع [النسب والاستشهاد](ATTRIBUTION.md) للمتطلبات والنطاق والأمثلة. ولتتبّع المرجع، نوصي بذكر ملف المصدر والإصدار أو الالتزام الدقيق. يوفّر [CITATION.cff](../../CITATION.cff) بيانات الاستشهاد بالبرنامج؛ ولا يحل الاستشهاد وحده محل الالتزام بالرخصة.

تحتفظ مكوّنات LLVM برخصة Apache-2.0 WITH LLVM-exception. يحتفظ Capstone برخصته الخاصة.
