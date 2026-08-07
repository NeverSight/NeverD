**اللغات**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← فهرس التوثيق](../README.ar.md)

# خارطة طريق NeverD

تلخّص هذه الوثيقة الاتجاهات الرئيسية بعد خط أنابيب PE / ELF / Mach-O الأصلي. المبادئ: **رفع 1:1**، **فشل صريح (strict)**، و**IR من أربع مراحل**.

---

## 1. اكتمال الصيغ الأصلية

إكمال الأهداف التي يتعرّف عليها الـ loader جزئيًا.

| البند | ملاحظات |
|-------|---------|
| PE AArch64 | Windows ARM64: unwind/`.pdata`، trampolines، rewrite |
| PE ARM32 (Thumb-2) | Windows on ARM بوضع Thumb فقط |
| Mach-O i386 | إعادات تموضع clang الشائعة؛ thin objects أولًا |

### المبادئ

- لا تُعلَّم كمدعومة قبل اختبارات الصيغة
- لا تُكسر ELF / PE x86 / Mach-O arm64+x64
- وضع التعليمات على مستوى الصورة

---

## 2. إعادة تجميع بايتكود EVM

توسيع NeverD إلى **بايتكود EVM** برفع 1:1 إلى نفس مكدس IR.

### الأهداف

- محمّل EVM · lifter أكواد 1:1 (strict) · مكدس/ذاكرة · JUMP/JUMPI → CFG · تخزين/calldata · HighIR/LLVM-C · CLI/C API موحّدة

### لماذا EVM

- إخلاص للتدقيق · محرك واحد للأصلي والعقود · بلا تخطٍ صامت

---

## 3. إعادة تجميع Solana eBPF (SBF)

برامج **Solana eBPF / SBF** بنفس دلالات strict.

### الأهداف

- محمّل SBF · lifter eBPF/SBF 1:1 · Account/CPI · نفس الأنبوب · API موحّدة

### لماذا Solana eBPF

- هدف تدقيق مهم · ISA من نوع BPF تناسب MedIR · SDK بلغة C واحدة

---

## 4. تعزيز المحرك والمنتج (مستمر)

| المجال | الاتجاه |
|--------|---------|
| تغطية الـ lifter | سد فجوات الأصلي دون إرخاء strict |
| اختبارات دلالية | توسيع Unicorn / roundtrip |
| ABI الإضافات | صيغ جديدة كإضافات عند الملاءمة |
| التوثيق / المصفوفة | تحديث README بعد الاختبارات فقط |

---

## الجدول الزمني

بحث / تصميم. لا تواريخ ملزمة.

| الميزة | الحالة |
|--------|--------|
| اكتمال الصيغ الأصلية (PE ARM*، Mach-O i386) | تصميم / تنفيذ مبكر |
| إعادة تجميع EVM | بحث / تصميم |
| إعادة تجميع Solana eBPF (SBF) | بحث / تصميم |
| تعزيز المحرك والمنتج | مستمر |

