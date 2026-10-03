**اللغات**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](project.md)

<!-- i18n-source: 7d9f0c7d3909a8c17cb367decf40b703e1928de2a57b8a83453ce6efb0f0b6c4 -->

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

توثق أدلة [EVM](evm.md) و[Solana SBF](sbf.md) صيغ الإدخال وعقود host والحدود.

تستعيد واجهة CLI التجريبية `neverd mobile app.apk -o recovered-app` شيفرة Java من APK وDEX وsmali وتنتج `report.json`. تستخدم استعادة Android محرك NeverD المدمج بلغة C++20 فقط، ولا تحتاج إلى Python أو Java وقت التشغيل. ضع المسارات التي تحتوي على مسافات بين علامتي اقتباس. راجع [دليل Android](android.md) للمدخلات المدعومة والتقارير وحدود الاستعادة.

يصدر مسار iOS التجريبي `neverd mobile App.ipa -o recovered-ios` شيفرة C أصلية ومصادر Objective-C/Swift مدعومة من IPA أو `.app` أو Mach-O. يحفظ تخطيطات وقت التشغيل ووحدات المصدر وأسباب حذف الطرائق صراحة، ولا يستخدم جسراً إلى الملف الثنائي الأصلي. راجع [دليل iOS](ios.md) للإعداد والتغطية وإعادة التجميع المستقلة.

تستخدم [استعادة مصادر المفسّرات التجريبية](interpreter-recovery.md) الأمر `neverd decompile --devirtualize --func ENTRY` لتخصيص مفسّرات x64 ELF/PE المرتبطة والمدعومة إلى HighC أو LLVMC عبر مسار LowIR/MedIR المشترك. تفصل تلميحات التحكم سياقات مفكّك الترميز دون تثبيت مدخلات التشغيل. يؤدي التحكم غير المحسوم والدلالات غير المدعومة ونفاد الميزانية إلى فشل صريح؛ ولا يثبت هذا الوضع سلامة استبدال الملف الثنائي أو تكافؤ الاستثناءات.

ميزانيات الاستعادة صريحة: تحتفظ `--vm-max-fields` و`--vm-max-refinements` و`--vm-max-queries` بالقيم الافتراضية 16 و16 و4096. يشرح دليل الاستعادة واجهة C المتوافقة v3 وقواعد الفشل.

تتيح الاستعادة أيضًا `--vm-chain-transfers=N` (الافتراضي 0) و`--vm-no-control-discovery`. يحفظ التسلسل الارتباطات الرمزية عبر الانتقالات المثبت أن لها هدفًا واحدًا؛ وعند الحد يعود إلى حدود CFG العادية. يمكن لاستعادة حالة الآلة إعلان إزاحات RSP عند الدخول دون التفاف، غير مفحوصة أثناء التشغيل، عبر `--vm-entry-frame=begin:end`. ترافق الفرضية العددية الدقيقة شيفرة C والتقرير؛ ولا تمنح صلاحية ذاكرة أو برهان تكافؤ.

يقبل الاسترجاع بحالة الآلة الخيار `--vm-entry-alignment=A:R` لتحديد نطاق RSP عند الدخول والتحقق منه صراحةً. يجب أن تكون `A` قوة موجبة للعدد اثنين وأن يكون `R < A`. تعيد القيم الأخرى الحالة 2 قبل الوصول إلى ذاكرة الضيف أو كتابة الحالة. تبقى بتات العنوان العليا حرة، ولا يُفترض أي اصطفاف افتراضيًا. لا يمنح هذا الخيار شهادة تكافؤ أصلي.

يضيف `--vm-external-stores-disjoint-frame` شرطًا صريحًا غير مفحوص أثناء التنفيذ: يجب ألا يتقاطع النطاق الكامل لكل STORE خارجي مع `--vm-entry-frame`. تُحفظ الحقائق الموجودة داخل النطاق فقط. لا يقيّد LOAD أو التداخل بين المؤشرات الخارجية، ويبقى السلوك الافتراضي محافظًا. ترفض واجهات الإثبات الأصلي هذا النطاق.

يمكن للدوال الكبيرة المستعادة التي تتجاوز حد بناء SSA استخدام `--llvm` عبر عقد محدود للتخزين العددي القابل للتغيير. يحتفظ التنفيذ بمعاني المدخلات وقيم الحلقات والقراءات السابقة. تفشل صراحةً الحالات الضمنية غير المدعومة ومعاملات سجلات المتجهات وإعادة تموضع الصورة والتخزين الملتبس وتدفق التحكم غير السليم؛ ويرفض HighC هذا المسار البديل. يظل المصدر خاضعًا لعقد حالة الآلة الحالي ولا يضيف شهادة تكافؤ.

تستنتج واجهة C++ المنفصلة لإثبات الحلقات ثوابت محدودة ورتبًا معجمية للحلقات المتداخلة ثم تعيد فحص التنقيح من الشيفرة الأصلية إلى LowIR. راجع [دليل الاستعادة](interpreter-recovery.md)؛ فهي لا تصادق على C الناتجة.

تركب واجهة C++ المستقلة `checkBinaryLLVMRefinement` فحوصاً جديدة للشيفرة الأصلية وLLVM على منتج LLVM دقيق؛ وتبقى ترجمة C خارج نطاق البرهان.

يتحقق استرداد PE أيضًا من بايتات DIR64 عند الأساس المفضل ويستبعد كتابات الاستيراد؛ ولا يشهد العقد بتكافؤ ASLR أو التهيئة.

ضمن عقد حالة الآلة الصريح، يدعم الاسترداد تقسيمات محدودة لمحاذاة مكدس الدخول وتنظيف `RET imm16` الداخلي. لا يزال التركيب التلقائي لبراهين الشيفرة الأصلية إلى LLVM لهذه التقسيمات غير مكتمل.

يحافظ استرداد `REP MOVS/STOS` المحدود على ترتيب العناصر وسلوك التداخل؛ ولا يزال إثبات التعليمات الأصلية غير مكتمل.

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

يفصل تنفيذ المعالج بين قبول ISA وذاكرة الضيف ونقل المحرك وسياسة نظام الضيف. يتيح `NEVERD_ENABLE_CPU_EMULATION` طبقة x64/ARM64، ويضيف `NEVERD_ENABLE_DRIVER_EMULATION` بيئة Windows WDM/KMDF x64 المحدودة. يشغّل `linux-elf64-v1` عمليات Linux ELF المدعومة. انظر [تنفيذ المعالج](cpu-execution.md) و[محاكاة عمليات الضيف](process-emulation.md) و[محاكاة برامج تشغيل Windows](driver-emulation.md).

يدعم `windows-pe64-v1` عمليات طرفية محدودة لـWindows x64/ARM64 مع PEB/TEB وTLS ثابت وديناميكي و`DllMain` وواجهات Win32 مسماة ورسوم DLL صريحة بلا دورات. تدعم الوحدات استيراد الشيفرة والبيانات بالاسم أو الرقم وDIR64 والتصدير المحال وهويات قوائم المحمّل الفعلية. تستخدم `LoadLibraryA` / `LoadLibraryW` و`FreeLibrary` و`GetProcAddress` دليل الوحدات المضبوط. ما زالت CRT/GUI وSEH للمستخدم والخيوط والتوافق العام مع Windows غير مكتملة، وكذلك أدلة ARM64 الأصلية لـKVM/WHP.

تضيف الذاكرة الافتراضية في Windows دعم `VirtualAlloc` و`VirtualFree` و`VirtualProtect` و`VirtualQuery` و`FlushInstructionCache` للعملية الحالية. تدير طبقة OS الحجوزات، وتبقى `AddressSpace` المرجع للصفحات الملتزم بها والصلاحيات والتخزين الفعلي. تشمل الاختبارات تعديل الشيفرة وأخطاء الوصول وإعادة استخدام ميزانية الذاكرة.

يدعم `driver-strict` / `checked-x64-v1` كلاً من KVM على مضيف Linux x64 المطابق وWHP على Windows x64 المطابق؛ يختار `auto` هذا النقل الأصلي، وتستخدم ISA المختلفة Unicorn. يحتفظ Unicorn الصريح وAPI V1 السابق بالملف البرمجي المحمول. يتحقق التنفيذ الأصلي من العناوين القانونية والآثار قبل الدخول؛ ويفشل العتاد غير المتاح دون تراجع. التعليمات وسلوك OS غير المدعومين أخطاء صريحة. تجتاز CI الأصلية على Windows x64 مع تعطيل Unicorn جميع الفحوص الإلزامية البالغ عددها 359:‏ 131 فحص CPU و224 نتيجة لبرامج التشغيل من 26 صورة مدمجة و46 صورة WDK و40 حالة سيناريو عند العناوين المفضلة والمعاد تموضعها، إضافة إلى أربعة فحوص لحدود SEH ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). لا تزال أدلة التشغيل الأصلي ARM64 ناقصة، ولا تثبت هذه القدرة توافق أي برنامج تشغيل أو Android/Darwin.

يدعم x64 المتحقق `MOVS/STOS/LODS` على RAM العادية و`CLD/STD`، مع الاستئناف والإلغاء وفحص الصفحات لكل عنصر. تبقى البتات العليا الخاصة بالمعالج عند العداد الصفري ومعاملات الأجهزة لـ STOS/LODS خارج العقد.

يدعم x64 المتحقق أيضًا `CMPS/SCAS` على RAM العادية مع `REPE/REPNE`، وأعلام الحساب والنهاية المبكرة والإيقاف لكل عنصر والتعافي من الأعطال. لا تزال مقارنات الأجهزة غير مدعومة.

يوفر `checked-aarch64-v1` و`checked-user-aarch64-v1` مجموعة محدودة من ARM64 FP32/FP64 وSIMD ثابت العرض وحالة FPCR/FPSR والمتجهات الكاملة. يستخدم Linux ARM64 المطابق KVM، ويستخدم Windows ARM64 المطابق WHP، وتستخدم ISA المختلفة Unicorn. ما زال التحقق الأصلي ARM64 مطلوباً؛ ويظل تحميل برامج تشغيل Windows مقتصراً على x64.

تتحقق اختبارات البدء الأصلية لـx64 وARM64 من تنفيذ الحالة الكاملة المحدود مع حق حصري للذاكرة. تملك حزم XSAVE وجداول التخزين المؤقت المرتبطة بـISA جهة مرجعية واحدة؛ وأدلة أحمال ARM64 الأصلية لا تزال غير مكتملة.

تتبع حقول x64 الأصلية `FOP/FIP/FDP` قواعد الحفظ والاستعادة للمضيف: قد يصفر AMD بيانات استثناء x87 غير النشطة. تتحقق مجسات البدء من هذه الحقول باستخدام استثناء معلق غير مقنّع.

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r3
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

مراجعة الحزمة الافتراضية هي `neverd-llvm-v23.0.0-r3`. يشكّل وسم Git وهدف الإصدار والتزام المصدر وبصمات الأرشيفات الثلاثة مرجع مصدر ذا إصدار غير قابل للتغيير. تنتقل أدلة البناء التي تحتفظ بالوسم الأساسي القديم أو `neverd-llvm-v23.0.0-r1` أو `neverd-llvm-v23.0.0-r2` تلقائيًا إلى `r3` ما لم تُحدّد قيمة `NEVERD_LLVM_PREBUILT_SHA256` صراحة. يعمل `Prebuilt LLVM Audit` عند push وpull request وكل ست ساعات، ويستدعي `scripts/audit_prebuilt_llvm_release.py` لمقارنة المرجع بإصدار GitHub الحالي وكل ملف تحقق منشور.

إذا تغيّر تفرع LLVM بينما لا يزال LLVM يعلن `23.0.0`، فانشر مراجعة الحزمة التالية `neverd-llvm-v23.0.0-r4` ثم `-r5`، بدل الكتابة فوق إصدار موجود أو اختراع إصدار LLVM باسم `23.0.1`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r4 \
  -f overwrite_existing_assets=false
```

بعد نجاح سير العمل، حدّث الوسم الافتراضي والالتزام المثبت والبصمات الثلاثة معًا في `cmake/NeverDLLVMPrebuilt.cmake`. تُخزّن الحزمة الجديدة تحت `.cache/neverd-llvm/<tag>`، ويفشل الأرشيف القديم أو المعاد نشره قبل الاستخراج. يقتصر `overwrite_existing_assets` على الاستعادة التاريخية ويبقى معطّلًا في سير المراجعات المعتاد.

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

توفّر [بيئة Qt Quick (الإنجليزية)](../gui.md) الاختيارية عروضًا قابلة للإرساء للتعليمات وCFG والبيانات الست عشرية وC وIR، وجميع لغات الواجهة الإحدى عشرة، وحفظ التعليقات، واتصالات MCP. يعمل التحليل في عملية مستقلة لا تعتمد على Qt؛ وتبقى إصدارات CLI فقط مستقلة. يوضح [سجل التأهيل (الإنجليزية)](../gui-qualification.md) مسارات العمل المدعومة واختبارات المنصات المطلوبة قبل الإصدار.

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
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

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
