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

توسيع NeverD إلى **بايتكود EVM** برفع 1:1 إلى نفس مكدس IR وإخراج C وSolidity وLLVM IR.

### الأهداف

- محمّل EVM · lifter أكواد 1:1 (strict) · مكدس/ذاكرة · JUMP/JUMPI → CFG · تخزين/calldata · C23 وSolidity وLLVM · CLI/C API موحّدة

**الحالة:** اكتمل decode وlifting للـlegacy opcodes من Frontier حتى Fusaka
وتغطيهما اختبارات الانحدار. ما زالت إعادة بناء source مستمرة بتحفظ: لا تُبلغ
selectors وevents والأنواع والمعايير والأسماء وcontrol flow الديناميكي إلا بدليل
كافٍ، ولا تُقدَّم كـsource أصلي أوABI كاملة أوتوافق ERC كامل. تبقى canonical
function selectors وABI variants الخاصة بكل standard وأشكال return الناجحة
منفصلة، فلا يستطيع selector مشترك بين ERCs اختلاق standard أو استعارة return
type غير متوافق. Amsterdam هدف
Review/development صريح opt-in، ويبقى `latest` هو Fusaka. EOFv1/EIP-7692 غير
مجدول وEIP-3540 في حالة Stagnant، فلا يُقدمان كسلوك mainnet نهائي. راجع
[فك تجميع EVM](../evm.ar.md).

### لماذا EVM

- إخلاص للتدقيق · محرك واحد للأصلي والعقود · بلا تخطٍ صامت

---

## 3. إعادة تجميع Solana eBPF (SBF)

برامج **Solana eBPF / SBF** بنفس دلالات strict.

### الأهداف

- محمّل SBF · lifter eBPF/SBF 1:1 · Account/CPI · نفس الأنبوب · API موحّدة

**الحالة:** اكتمل دعم عقود Anza `sbpf` الحالية من v0-v4. يدعم التنفيذ ملفات ELF القديمة ذات sections/relocations وملفات ELF الصارمة المعتمدة على program headers فقط، وقاعدة تعليمات كاملة حسب الإصدار، والتحقق الصارم، ومراحل Low/Med/High IR، ومراقبة syscall/CPI/account، وLLVM متحققًا منه، وC11 محمولًا، وRust مستقرًا وآمنًا، وتكامل CLI/C API، وoracle دلاليًا مستقلًا ومحدودًا للبايتكود الخام. يتتبع v4 المنبع؛ وتظل إمكانية نشره أو تشغيله على cluster معين معتمدة على feature activation لذلك cluster. راجع [تفكيك Solana SBF](../sbf.ar.md).

### لماذا Solana eBPF

- هدف تدقيق مهم · ISA من نوع BPF تناسب MedIR · SDK بلغة C واحدة

---

## 4. تدقيق وصيد أمان الذاكرة

تحليل ثنائي مرفوع بحثًا عن عيوب عمر الكومة (تسرّب، تحرير مزدوج، استخدام بعد التحرير) وفيضانات النسخ الخطرة، بـ JSON منظَّم، مع نموذج محلّل محدود للفيضان المُثبَت. يعمل التحليل على IR المستقل عن الصيغة وعرض الهوية المشترك، لذا **PE وELF وMach-O أهداف متكافئة**، ويعيد استخدام التنفيذ الرمزي ومحلّل المتجهات البتية الداخليين — بلا محلّل خارجي ولا حاوية.

| البند | ملاحظات |
|-------|---------|
| مسار `audit` | آلة حالة الكومة فوق IR + ملخصات الهروب: تسرّب، تحرير مزدوج، استخدام بعد التحرير |
| مسار `hunt` | كتالوج المصارف + مرشح الوسائط المسبق + سعة الوجهة + شاهد المحلّل |
| دليل قابلية الوصول | حالة التحكم من مداخل معروفة مع نقطة ثابتة مستقلة لتحكم المهاجم وشاهد دقيق للجذر/سلسلة الاستدعاءات |
| عقد الهوية | حلّ المصارف حسب الصيغة (IAT لـ PE وPLT لـ ELF وربط dyld لـ Mach-O) ومصادر الأسماء PDB / DWARF / MAP |

**الحالة:** اكتملت Phase 1 لـ PE وELF وMach-O. يتضمن P0 تحليلًا مغلق العالم لدورة حياة الكومة والنسخ الخطر، ودليل schema-v1 إضافيًا مع إعادة تشغيل `process-input-v1` لقيم البيئة الحرفية الدقيقة ولأول استهلاك للدخل القياسي؛ وتبقى أنواع الإدخال الأخرى غير قابلة للإعادة مع سبب. يغطي P1 فيضان المكدس/العالمي والقراءات المحلية غير المهيأة وسلاسل التنسيق. تبقى تأثيرات الاستدعاء المجهولة أو القابلة للتطبيق جزئيًا UNKNOWN. تغطية الأحكام والهوية مثبتة بـ [`unittests/safety`](../../unittests/safety) واختبار الطرف إلى الطرف [`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp) الذي يشغّل على كل مضيف مصفوفة PE/ELF/Mach-O × x86-64/AArch64 الإلزامية. انظر [تدقيق وصيد أمان الذاكرة](../memory-safety.ar.md).

يضيف slice الحالي بين الإجراءات `reachability.status` و
`reachability.attacker_control` إلى schema v1 من دون تغيير `verdict` المستقل.
وهو يبلغ عن جذور `application` أو `image` أو `export`، وسلاسل استدعاء داخلية
دقيقة، وحالات UNKNOWN مغلقة الفشل. تتاح الميزانيتان `max_call_depth` و
`max_summary_iterations` عبر C API وأمري CLI وطريقتي Python. ولذلك فإن
`control_reachable` و`attacker_reachable` مجموعا
قابلية وصول، لا مجموعا أحكام بديلين.

تُنظَّم أسطح التحليل وخطط P2 ضمن حدود ذات إصدار وحالة صريحة:

| الخطة | النطاق | الحالة |
|-------|--------|--------|
| `lowir-concolic-v1` | استكشاف LowIR هجين/concolic وتوليد seed | تجريبي؛ بذور سجلات متحقق منها بإعادة التشغيل على PE/ELF/Mach-O × x86-64/AArch64 |
| `binary-sanitizer-v1` | فحوص وقت تشغيل مدرجة في ثنائي أصلي معاد الكتابة | تجريبي على Darwin: حراس counted-write لكل المواقع أو الرفض، ونشر موثّق create-exclusive أو no-change للمصدر نفسه |
| `process-replay-v1` | إعادة تشغيل أوسع للعملية تشمل argv والملفات والشبكة والقراءات المتكررة بعد `process-input-v1` | حد Phase 0 فقط: تحقق الخطة/المنسق واستعلام fail-closed عن التوافر الأصلي؛ لا يوفر أي مضيف عمليات replay أصلية |

محوّل concolic هو سطح تحليل منفصل، وليس توسيعًا لعقد قبول تقرير أمان Phase 1. يتوفر sanitizer التجريبي عبر `neverd_session_sanitize` و`neverd patch --sanitize=strict` وPython `Session.sanitize`؛ وترفض المضيفات غير Darwin قبل lifting أو تغيير namespace. يوثّق receipt الكامل كائن دليل الوجهة المحتفظ به أثناء المعاملة فقط. ولأن الدليل قد يُعاد تسميته بعد فتحه، فلا يثبت أن pathname الأصلي ظل يشير إلى الكائن أثناء المعاملة أو بعد العودة، ولا يشكل ارتباط مسار دائمًا. يظل `NativeProcessReplayAdapter` حد query/factory من Phase 0 بقدرات كاملة أو معدومة؛ وتعيد كل المضيفات حاليًا جميع القدرات false ولا تعيد جدول عمليات.

---

## 5. تعزيز المحرك والمنتج (مستمر)

| المجال | الاتجاه |
|--------|---------|
| تغطية الـ lifter | سد فجوات الأصلي دون إرخاء strict |
| اختبارات دلالية | توسيع Unicorn / roundtrip |
| ABI الإضافات | صيانة [ABI الإضافات الأصلية](../plugins.ar.md) بوصفه عقد توسعة داخل العملية؛ تبقى قيم Loader وUI بيانات وصفية حتى تتوفر واجهات مضيف صريحة |
| التوثيق / المصفوفة | تحديث README بعد الاختبارات فقط |

---

## الجدول الزمني

تغطي اختبارات الانحدار الصيغ الأصلية وlegacy EVM decode/lifting حتى Fusaka و
Solana SBF وPhase 1 لأمان الذاكرة، بما فيه slice قابلية الوصول الحالي من المداخل
المعروفة. ما زالت إعادة بناء source ‏EVM المحافظة مستمرة. لا تواريخ ملزمة.

| الميزة | الحالة |
|--------|--------|
| اكتمال الصيغ الأصلية (PE ARM*، Mach-O i386) | مكتمل |
| Legacy EVM decode/lifting | مكتمل حتى Fusaka؛ مغطى باختبارات انحدار |
| إعادة بناء source ‏EVM | مستمرة — مسندة بالأدلة ومحافظة |
| إعادة تجميع Solana eBPF (SBF) | مكتمل — v0-v4 وC وRust وLLVM؛ مغطى باختبارات انحدار |
| تدقيق وصيد أمان الذاكرة | اكتملت Phase 1 وslice قابلية الوصول من المداخل المعروفة؛ `lowir-concolic-v1` و`binary-sanitizer-v1` على Darwin تجريبيان؛ ويظل `process-replay-v1` الأصلي غير متاح خلف محوّل Phase 0 بنمط fail-closed |
| تعزيز المحرك والمنتج | مستمر |
