**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](source-patching.md)

<!-- i18n-source: 1eae101089e730c29ba3fda92db515fe148fe41b758d6f5b69156252591d9b3f -->

[← Documentation Index](README.md)

# تصحيحات المصدر والكائنات الأصلية

يترجم NeverD الدالة البديلة بالمترجم المختار ويثبتها عند مدخل الدالة الأصلية المحدد صراحةً. يتولى المترجم دلالات C/C++، ويتولى NeverD ربط العناوين وإعادة التموضع والتخزين وسجلات فك المكدس والنشر. يحتفظ إصلاح الثنائيات وإعادة كتابة الشيفرة المرفوعة بمساراتهما. لا يعاد بناء بقية البرنامج.

<!-- i18n-section: compiler -->
## اختيار المترجم

اختر `--compiler=clang` أو `--compiler=gcc` أو `--compiler=neverc`. يستطيع Clang وNeverC في الوضع العادي إنتاج LLVM IR؛ وتحافظ صيغة `object` على التعليمات الأصلية المنتجة. تختار CLI الكائنات افتراضياً مع GCC. يختار Python SDK الكائنات افتراضيًا مع GCC أيضًا. لا يحدث انتقال صامت إلى مترجم آخر.

استخدم `--compiler-arg` أو `compiler_args` في SDK لتمرير مسارات التضمين وتعريفات الماكرو وخيارات المصدر المدعومة. تُرفض تغييرات الهدف أو نمط الإخراج ورموز ملفات الاستجابة (`@file`) في جميع مواضع الوسائط، بما فيها القيم المنفصلة للخيارات. تظل مسارات `-I` وتعريفات `-D` العادية مدعومة.

استبدل عناوين الأمثلة بمداخل أصلية مؤكدة. يجب أن يطابق تصريح البديل ABI الأصلي بما فيه اتفاقية الاستدعاء وتخطيط البنى؛ لا يثبت العنوان ذلك. تتطلب C++ الخيار `--language=c++` ومسار Clang++/G++ الصحيح. يفضل مدخل `extern "C"` أو الاسم المزخرف الدقيق. يسمح بدوال مساعدة إضافية. يحدد `--entry` و`--func` البديل صراحةً، ويحترم `--from-c` القديم العنوان أيضاً.

يمكن أيضًا اختيار `artifact="object"` صراحةً.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## الملفات المتحققة والقيود

[NeverC producer: 182fd2c5dc](https://github.com/NeverSight/NeverC/commit/182fd2c5dc)

**NeverC C وDynCode المستضاف**

يقبل NeverC لغة C الخالصة فقط على ELF وPE وMach-O لمعمارَي x64 وARM64. يرفض NeverD الجمع بين NeverC و`--language=c++` قبل تشغيل المترجم، دون ترجمة وسيطة أو تبديل ضمني للمترجم. تستخدم C++ والاستثناءات Clang أو برنامج تشغيل GCC المطابق لملف كائنات ELF المتحقق منه. يمكن لإطارات hosted C تمرير استثناء يرميه برنامج C++ المحيط ويلتقطه بفضل سجلات فك المكدس الفعلية؛ وهذا لا يضيف دعم C++ إلى NeverC.

يستخدم `dyncode`: `--compile-mode=dyncode` وضع `-fdyncode-hosted` ويتطلب مصدرًا ومدخلًا صريحًا واحدًا وكائنًا أصليًا دون ربط بالرموز القائمة. تختار CLI وPython الكائنات لهذا الوضع وGCC. تُفحص البصمتان وكل بايت والمدخل قبل النشر، مع CFI/SEH حقيقية أو سجلات compact كاملة. تُرفض الحالة القابلة للكتابة/TLS وتحويلات syscall/PEB/heap/string الضمنية والاستيرادات غير المحلولة والكود المعاد ترتيبه. تُمرر البيانات وcallbacks كمعاملات.

```sh
neverd patch app --from-source=replacement.cpp --language=c++ \
  --compiler=clang --compiler-path=/path/to/clang++ \
  --entry=replacement --func=0x401140 --patch-report -o app.cpp
neverd patch app --from-source=replacement.c --compiler=neverc \
  --compiler-path=/path/to/neverc --compile-mode=dyncode \
  --entry=replacement --func=0x401140 --patch-report -o app.dyncode
```

يدعم مسار Clang LLVM IR استثناءات Windows ARM64 C++ FH3 مع التحقق من xdata وABI الإطار الأب ومجموعات funclet/FuncInfo. تبقى معالجات اللغة في الكائنات الأصلية غير مدعومة. ينفذ `check_source_patch_cpp_runtime.py` تسعة سيناريوهات على Windows أصلي x64/ARM64 مع فصل أدلة CPU ومفك المكدس في النظام.

| المترجم | الناتج | ملف تنفيذ نظام التشغيل المتحقق |
|---|---|---|
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++؛ Mach-O x64/ARM64 C/C++؛ PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| Clang 22.1.6 | كائن أصلي | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C; Mach-O x64/ARM64 C; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| GCC/G++ 13.3.0 | كائن أصلي | ELF x64/ARM64 C/C++ مع بيئة Itanium متوافقة; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| NeverC 3389.1.5 الوضع العادي | LLVM IR | ELF x64 C؛ Mach-O x64/ARM64 C |
| NeverC 3389.1.5 الوضع العادي | كائن أصلي | ELF x64 C |
| NeverC 3389.1.5 C | LLVM IR / كائن أصلي | ELF / PE / Mach-O x64/ARM64: مصدر C خالص |
| NeverC 3389.1.5 `hosted-v1` | كائن أصلي + حمولة مختومة | ELF / PE / Mach-O x64/ARM64: مصدر C ومرور الاستثناء عبر إطارات C |

يعرض الجدول ملفات تشغيل OS المتحقق منها، ولا يضمن كل إصدار أو خيار للمترجم. تقبل مثبتات ELF وPE وMach-O الأصلية المعماريات الأربع. يتطلب GCC برنامج تشغيل ELF مطابقًا ولا ينتج LLVM IR. تفشل بوضوح عمليات النقل المجهولة وTLS والمنشئات العامة عند التحميل وLTO ورموز common/IFUNC والاستيرادات غير المحلولة. يسجل إيصال JSON المترجم والوسائط والربط والعناوين، ويسجل SHA-256 للكائنات الأصلية أيضًا.

يُستمد حقل `target` في إيصال الكائن الأصلي من ABI للصورة المحمّلة، بما في ذلك أعلام الفاصلة العائمة ARM ELF والتمييز بين منصتَي iOS/watchOS. تتبع الكائنات المترجمة والمقدمة قاعدة الهوية نفسها.

تتطلب مصفوفة CPU نجاح تثبيت المصدر وIR المقدم والكائنات الأصلية في التركيبات الاثنتي عشرة. وتتحقق بصورة منفصلة من ARMv7k watchOS وARMv7 iOS، والحالة القابلة للكتابة／للقراءة فقط، ووسائط الفاصلة العائمة والمكدس للدوال الأصلية، وتنفيذ نقطة الدخول بعد تغيير عنوان التحميل. لا يصادق تنفيذ CPU على محمل OS أو فك مكدس C++ أو أي ABI؛ راجع ملفات OS أعلاه.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

تعني `CPU` تثبيت المصدر／IR／الكائن وإعادة التحميل والتنفيذ الدقيق للدالة. يستنتج ARM ELF حالة EABI5 وARM/Thumb وABI الفاصلة العائمة من الصورة ويسجل فهرس `EHABI` كاملًا ومرتبًا. يحافظ PE على سجلات x64/ARM وعمليات `HIGHLOW`/`DIR64`/`MOV32T`، ويستخدم x86 شيفرة تسجيل المترجم. يميز Mach-O بين watchOS DWARF وiOS `SjLj` حسب النوع الفرعي CPU. تستخدم مداخل ARM/Thumb فروعًا نسبية تتحمل تغيير قاعدة التحميل.

تعتمد الكائنات الأصلية على رموز الدوال محددة الحدود والتعريفات المساعدة／تعريفات الأقسام وسجلات فك المكدس الخاصة بها، دون اختلاق معرفات ملكية MC أو إعادة ترجمة IR. تتطلب حدود دوال COFF أحجامًا مساعدة دقيقة أو أقسامًا لدالة واحدة (`-ffunction-sections`) أو سجلات دوال وقت التشغيل الأصلية بعد فك ترميزها؛ لا يُستخدم الرمز التالي لتخمين الحجم. يحتفظ محول watchOS بإطارات الدوال الورقية للحصول على سجلات حقيقية. لا تشمل ملفات الكائنات المتحقق منها معالجات اللغة COFF/Mach-O الأصلية؛ استخدم مسار IR المتحقق منه لاستثناءات C++. يُرفض فك مكدس ARM الثابت ذو سجل exidx مدمج، ومؤشرات Mach-O المطلقة غير المسجلة، وتخزين RELRO غير المدعوم.

ثُبتت كائنات GCC C ونُفذت أيضًا على ELF x86 وARM32 وARM64 عبر Linux QEMU user mode. يختار المحول ARM/Thumb من الصورة، وتُحاذى سجلات GNU property حسب عرض مؤشر ELF.

<!-- i18n-section: references -->
## استدعاء التنفيذ الأصلي وتوسيع التخطيط والتبعيات

تسمّي الروابط تصريحات خارجية: يتحقق `function` من الدالة الأصلية أو وسيط الاستيراد، ويتطلب `data` تخزيناً قابلاً للكتابة و`const` تصريح IR ثابتاً. تحتاج بيانات الكائن إلى نطاق غير صفري لا يتجاوز حدود التخزين ولا يمنح الكتابة إلى RELRO. استخدم `original` صراحة لاستدعاء الجسم المحفوظ للدالة المستبدلة. يستدعي المدخل العام البديل، بينما ينفذ المدخل المحفوظ بادئة من تعليمات كاملة أُعيد تحديد مواقعها ثم يتابع في الجسم الأصلي. تشترك الشيفرة وIR المقدم والكائنات الأصلية في هذا العقد، بما فيه معاملات الفاصلة العائمة والمكدس وتعدد الاستدعاءات الراجعة.

يتطلب الاستدعاء الراجع حدود دالة ونمط تعليمات وعنوان متابعة وأدلة فك مكدس موثقة. تُرفض قبل النشر التعليمات المجهولة ومراجع PC النسبية غير الآمنة والفروع إلى داخل البادئة المنسوخة وإعادات التموضع غير المدعومة للمحمّل والاستدعاءات المتداخلة والمقدمات ذات المعالجات غير الممثلة. يبقى توفير تصريح مطابق لـABI مسؤولية المستدعي. يسجل الإيصال المداخل المحفوظة في `original_callbacks`.

يمكن لـPE32/PE32+ توسيع جدول رؤوس أقسام ممتلئ بنقل نطاقات الملف وتحديث مؤشرات التصحيح والرموز والشهادات والبيانات الملحقة الممثلة، مع حفظ عناوين RVA الأصلية. يمكن لصور Mach-O x64/ARM64 PIE توفير مساحة الأوامر وفك المكدس عبر تمديد مدقق لبادئة الرأس إذا سمحت حماية page-zero المجاورة ومقطع linkedit الأخير؛ تبقى عناوين الشيفرة والبيانات الأصلية ثابتة. لا يتطلب هذا النمط `-Wl,-headerpad,0x400` أو صفحات مجاورة فارغة لفك المكدس. تُرفض مراجع عنوان الرأس الصريحة وأوامر التحميل المجهولة وملكية الشيفرة/البيانات المختلطة والنطاقات التالفة والتخطيطات غير المدعومة. تُخطط سعتا الرأس وفك المكدس معاً قبل التثبيت.

تبقى الشيفرة الجديدة RX والحالة المتغيرة RW. تضيف طلبات `--dependency` الصريحة دوال أو بيانات عبر بيانات ELF الديناميكية أو واصفات استيراد PE/IAT أو بيانات Mach-O dylib/bind. تبقى هويات الاستيراد وأرقامه وإصداراته الحالية ثابتة، وتحتفظ الخانات ووسطاء الاستدعاء بمعانيها المنفصلة. اختر اسم المكتبة أو مسارها وفق محمّل OS المستهدف. يستخدم المثال اسم مكتبة ELF مشتركة. يجب أن يؤدي غياب المكتبة أو الرمز إلى خطأ حقيقي من المحمّل وقت التنفيذ. تظل TLS وIFUNC وعقود إصدارات الرموز الجديدة وبواني وقت التحميل وإعادات التموضع غير الممثلة غير مدعومة؛ وتتطلب ELF `.data.rel.ro` وCET/IBT عقوداً إضافية. في كائنات COFF الأصلية، صرّح عن استيرادات الدوال والبيانات الجديدة باستخدام `__declspec(dllimport)` كي تحدد إعادات التموضع جدول IAT. تُرفض المراجع الخارجية المباشرة العادية.

```sh
# ELF example: the original target is at 0x401140.
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=original:saved_target=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --dependency=function:extra_function=libpatch_extra.so \
  --dependency=data:extra_state=libpatch_extra.so \
  --patch-report -o app.bound
```

تُضاف خيارات التبعيات إلى نهاية البنية وتُفحص بحسب حجمها، وتسري على الشيفرة وIR المقدم والكائنات الأصلية. يتضمن الإيصال `added_imports`؛ وبعد إعادة تحميل المرشح يعاد فحص جميع الاستدعاءات والاستيرادات وصلاحيات التخزين والبايتات المثبتة. يحفظ الفشل المدخل وأي وجهة موجودة.

<!-- i18n-section: examples -->
## أمثلة قابلة للتشغيل

[أمثلة كاملة لـ Windows/macOS/Linux](../../examples/source-patch/README.md) تتضمن مصادر البرنامج الأصلي والدالة البديلة والمكتبة، وأوامر البناء وإضافة Python SDK تعمل داخل المضيف الحقيقي. شغّلها من جذر المستودع باستخدام بناء NeverD يدعم Python ومجلد إخراج جديد.

```sh
# Linux / macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-native
```

```powershell
# Windows: Visual Studio developer PowerShell
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-native
```

يعمل المساران افتراضيًا؛ أضف `--api cli` أو `--api sdk` لاختيار أحدهما. يُكتشف عنوان الهدف تلقائيًا. عند الإدخال `7` يطبع الأصل `8` وتطبع النسختان المعدلتان `36`. يتحقق المشغّل من التنفيذ والإيصالات وفشل المحمّل الفعلي عند غياب المكتبة، ويحفظ الأوامر الكاملة في `commands.json`.

<!-- i18n-section: exceptions -->
## استثناءات C++

تستخدم ELF بيانات FDE/LSDA للمترجم، وتستخدم Mach-O صيغة compact unwind/DWARF. تقبل PE x64/ARM64 جداول FH3 للمترجم مع بيئة MSVC أصلية متوافقة وهويات أنواع متطابقة. قد تحتاج بيانات type-info إلى روابط صريحة. ترفض FH4 وpersonality المخصصة ومصدر SEH وأوضاع CFG/EHCont/XFG النشطة مع الحفاظ على أعلام الحماية الأصلية.

تُرفض حاليًا البرامج الأصلية ELF x86/x64 التي تتطلب IBT؛ فمثبت المصدر لا يوفر بعد قفزة وسيطة تحافظ على ENDBR.

تغطي حالات التنفيذ العودة العادية وcatch الداخلي واستثناءً جديداً يمسكه المستدعي الأصلي واستثناءً يمر من الدالة الأصلية عبر البديل وتنظيف RAII مرة واحدة وإعادة الرمي وإنهاء noexcept والدوال غير المعدلة. يجب أن يسمح موضع الاستدعاء الأصلي بانتشار الاستثناء. لا يعيد التصحيح مسارات catch التي أزالها المترجم حول دالة ثبت أنها لا ترمي.

<!-- i18n-section: publication -->
## SDK والنشر

واجهة C الإضافية هي `neverd_patch_source` مع `neverd_source_patch_options_v1`؛ وتعيد `neverd_patch_source_report` الإيصال. يقدم إدخال الكائن طوله بالبايت. تبقى `neverd_patch_from_c` القديمة متاحة.

تؤخذ لقطة للمدخلات، ويجهز المرشح بجوار الوجهة، ويعاد تحميله وتدقيق كل مدخل مطلوب قبل النشر الحصري. لا تستبدل الملفات الموجودة ولا أسماء الروابط الرمزية. يؤدي غياب تعيين واحد إلى فشل الطلب كله. يرفض `--mode=inplace`. لا تطبق تحويلات IR للجلسة على الكائنات الأصلية. تتحقق Mach-O من التوقيع وتجدد توقيعه بدقة؛ ولا يحذف توقيع PE بصمت.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="clang", compiler_path="/path/to/clang", artifact="object",
    bindings={
        "saved_target": ("original", original_address),
        "shared_counter": ("data", original_counter_address, 4),
    },
    dependencies={
        "extra_function": ("libpatch_extra.so", "function"),
        "extra_state": ("libpatch_extra.so", "data"),
    },
)
```

<!-- i18n-section: verification -->
## البناء والتحقق

نجحت المهام الست المختارة لأنظمة التشغيل والمعماريات الأصلية بلا إخفاق أو تخطٍّ: 3669 تنفيذ GoogleTest، و1008 ملاحظات لعمليات أصلية (504 وظيفية و504 لاستثناءات C++)، و12 رفضاً فعلياً من المحمّل، و96 صورة و84 إيصال نشر.

| المضيف | المصدر | Harness | مهمة CI | الاختبارات | الملاحظات / الرفض |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

تؤهّل هذه الآثار تحديداً مراجعات المصدر وharness المدرجة أدناه. تخضع مراجعات دمج dev والتوثيق اللاحقة لفحوص محلية وتوثيقية مستقلة. تظل المهام السابقة الفاشلة أو الملغاة إخفاقات. لا تثبت الملاحظات التكميلية التاريخية الـ216 ولا نتائج CPU/التوافق لـPE ARM32 وApple القديمة ذات32بت تغطية عتاد أصلي أو استثناءات C++ للقدرات الجديدة.

يستخدم تأهيل Windows ARM64 مكتبة Unicorn C DLL أصلية لمعمارية ARM64، بُنيت فعلياً من الوحدة الفرعية المثبتة بأدوات LLVM-MinGW المتحقق منها، مع مكتبة استيراد MSVC وتجاوز صريح لهدف CMake. توثّق نسخة البناء المعزولة ثلاثة إصلاحات للمضيف: زوج انتقالات بلا SEH، ورد نداء ARM64 لتخصيص الصفحات مع مزامنة ذاكرة التعليمات المخبأة، وأقنعة قيم فورية 64 بت لـ Windows LLP64. لا تتغير مصادر الوحدة الفرعية أو مراجعتها. هذا يؤهل مضيف الاختبار لمصفوفة CPU ولا يثبت دعم JIT MSVC ARM64 الافتراضي. تُسجّل فحوص استعادة المضيف الخمسة منفصلة عن أدلة التنفيذ الأصلي للعمليات المعدلة واستثناءات C++.

حدد مترجماً متوافقاً عبر `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (جرى اختبار Clang 22.1.6). يصحح البناء المتكامل نسخة LLVM داخل دليل البناء فقط، دون تعديل الوحدة الفرعية أو ذاكرة الحزم المشتركة. قد يرفض خطأ x64 PLT في حزم r3/r4 تصحيح IR؛ تتجنب الكائنات الأصلية هذا المسار MC. يشترط `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` إنتاج PLT الدقيق المصحح.

تحتوي `NeverDSourcePatchTests` على حالات C/API/الكائنات/الاستثناءات الأصلية؛ وتفحص `NeverDSourcePatchMatrixTests` كل التركيبات باستخدام Unicorn. تنفذ `SourcePatchPE` سبعة سيناريوهات C++ عند تحديد `NEVERD_TEST_WINDOWS_CRT_LIB` و`NEVERD_TEST_WINDOWS_UCRT_LIB` و`NEVERD_TEST_WINDOWS_SDK_LIB`. يحتاج المضيف غير Windows أيضاً إلى `NEVERD_TEST_WINDOWS_RUNNER`. يجعل `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` المدخلات الناقصة فشلاً. يشترط سير عمل Windows المخصص التنفيذ الأصلي ويرفض الحالات المتخطاة.

تستخدم أدلة Linux على Mac التطوير Docker ومحمل Linux صريحاً؛ وتستخدم PE بيئة Wine 11. تنفذ Mach-O ARM64 على Mac نفسه. يجب فحص نتائج CI الأصلية لـWindows بشكل منفصل؛ فإعداد المهمة لا يعني نجاح التنفيذ. توجد الأعداد التاريخية وإخفاقات الأساس وحدود المحاكيات في [العقود التفصيلية وسجل التحقق](../source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

تصحح نسخة LLVM أيضاً مراجع بيانات GOT في Mach-O x64؛ تقاس جداول catch ذات النوع في Clang من نهاية حقل إعادة التموضع ذي أربعة بايتات. تزحزحه r3/r4 أربعة بايتات، وترفضه مراجعة ما قبل النشر. تنفذ حالات x64 على Mac عبر Rosetta 2، وحالات ARM64 مباشرةً.

اجتاز التحقق في بيئة Windows x64 الأصلية [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805) جميع الاختبارات المتخصصة وعددها 16 لمسارات PE والكائنات وtrampoline، وجميع اختبارات مصفوفة أهداف CPU وعددها 143، دون تخطي أي حالة. نفّذ اختبار FH3 سبعة سيناريوهات تشغيل C++؛ وتستخدم مصفوفة الأهداف Unicorn. استُخدم Clang 22.1.6 مع MSVC وWindows SDK الأصليين وLLVM المدمج.

يُنشئ سير عمل Windows أيضًا 27 بديلًا بصيغة PE لمعمارية x86 وx64 وARM64. يتحقق `scripts/check_source_patch_pe_runtime.py` من الاستدعاءات غير الورقية وحالة المؤشرات القابلة للتغيير ووسائط الفاصلة العائمة والمكدس عبر IR المصدر والكائنات المترجمة والكائنات المقدمة. يُشغّل كل أصل وكل نتيجة ثلاث مرات، أي 30 ملاحظة تنفيذ لكل معمارية. يعمل x86/x64 على Windows x64، ويعمل ARM64 على `windows-11-arm`. يتطلب التحقق المضيف الأصلي المتوقع وتطابق الإيداع وبصمات الملفات واكتمال جميع الحالات، مع تسجيل المشغلات الخارجية بصورة منفصلة. لا يثبت هذا الملف دعم استثناءات C++ على ARM64.

يحدد `NEVERD_TEST_MATRIX_NEVERC` المترجم الاختياري للمصفوفة. اجتاز NeverC 3389.1.5 جميع الفحوص الـ66 لمعمارية x64/ARM64 مع ELF وPE وMach-O، بما فيها تثبيت IR والكائنات الأصلية وتنفيذ CPU. لا يدعم هذا الإصدار x86 أو ARM32؛ استخدم Clang لهما، أو مشغل GCC متوافقًا مع ELF. عند ترجمة PE باستخدام NeverC، يضيف المحوّل `-fno-autolink` لحل المراجع داخل الصورة الحالية دون ربط CRT آخر. تظل الكائنات المقدمة التي تحتوي عقود ربط `.drectve` مرفوضة.

اجتاز التحقق من عمليات Windows الأصلية [a4a5ae3cb](https://github.com/NeverSight/NeverD/actions/runs/38350079817) جميع الملاحظات الـ90: 30 لكل من x86 وx64 وARM64، عبر IR المصدر والكائنات المترجمة والكائنات المقدمة. نُفّذ x86/x64 على Windows x64، ونُفّذ ARM64 على `windows-11-arm` باستخدام الصور نفسها بعد التحقق من بصماتها. اجتاز التشغيل نفسه أيضًا جميع الفحوص المتخصصة الـ16 لمسارات PE والكائنات وtrampoline، وجميع فحوص مصفوفة Unicorn الـ143، دون تخطي أي حالة. احتفظ اختبار FH3 بسيناريوهات تشغيل C++ x64 السبعة.

يثبت التحقق الأصلي إصدار NeverC عند `182fd2c5dc` مع برامج أصلية مستقلة ومثبت NeverD الحالي. يتطلب `check_source_patch_hosted_pe_runtime.py` عدد 36 مشاهدة لكل معمارية Windows لوسيط C وكائناته وhosted ومرور الاستثناء عبر إطارَي C. ويتطلب `check_source_patch_elf_runtime.py` عدد 120 مشاهدة لكل معمارية Linux لـC والمرور hosted وتسعة سيناريوهات لاستثناءات Clang C++. يجب تطابق الإيصالات والبصمات ومعمارية المضيف الفعلية؛ ولا يعني ذلك تشغيل مترجم NeverC نفسه على مضيف CI. ينفذ Linux x64 وARM64 السيناريوهات التسعة لكل من وسيط Clang وكائنات Clang الأصلية وكائنات GCC الأصلية. ويبقى NeverC مخصصا لـC فقط.

تعد المشاهدات الـ204 لملف الترجمة السابق سجلا تاريخيا ولا تثبت عقد C الخالصة الحالي. تستخدم مسارات NeverC الثلاثة الحالية لغة C، وتختبر Clang C++ FH3 منفصلة. على جهاز Mac التطويري بمعالج Apple Silicon، تستخدم macOS x64 بيئة Rosetta 2. لا تحل نتائج CPU في Unicorn/QEMU محل التحقق الأصلي من النظام وفك المكدس.

تتطلب إعادة تموضع تعليمات AArch64 قيما مضافة صريحة من RELA. ترفض القيم المضافة الضمنية لتعليمات REL قبل النشر، ولا تفسر حقول بتات التعليمات كبيانات عادية. تحتفظ إعادة تموضع بيانات PREL64 المدعومة بالقيم المضافة الموقعة كاملة في REL وRELA.

اجتاز تحقق منتج C [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591930213) جميع مشاهدات العمليات الأصلية البالغ عددها 312:‏ 36 لكل من Windows x64 وARM64، و120 لكل من Linux x64 وARM64. يتحقق Linux بصورة منفصلة من تسعة سيناريوهات استثناءات لكل من وسيط Clang وكائنات Clang الأصلية وكائنات GCC الأصلية؛ وجميع مدخلات NeverC لوسيط IR والكائنات وhosted DynCode مكتوبة بلغة C.

اجتاز تحقق المثبت المعاد بناؤه [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591928600) عدد 19 فحصا لـPE والكائنات، وجميع حالات مصفوفة CPU البالغ عددها 143، و474 مشاهدة لعمليات Windows/Linux الأصلية. نفذت عمليات Windows x86 على مضيف Windows x64، واستخدم x64 وARM64 مضيفين مطابقين. واجتاز [تحقق Linux](https://github.com/NeverSight/NeverD/actions/runs/38585759996) المنفصل الحالات الأصلية الـ108 وثلاثة فحوص LLVM-C وثلاثة فحوص Clang/GCC C++ إضافية، دون إخفاق أو تخط.

أعاد [تأهيل الدمج 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627694097) التحقق من جميع ملاحظات العمليات الـ312 على Windows وLinux الأصليين بمعماريتي x64 وARM64. تطابق الصور النهائية الثلاثون البايتات المؤهلة سابقاً؛ حُدّثت بصمات المصدر وإيصالات النشر الخاصة بـ C فقط وتحقّق منها بصورة مستقلة.

نجح [تأهيل Linux الموسع عند 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38626866257) في 429 فحصاً منفذاً دون فشل أو تجاوز: الحالات الأصلية الـ108، و55 فحص Swift كاملاً، و143 فحصاً لأهداف المعالج، و117 فحص جلسة أصلياً، وثلاثة فحوص LLVM-C وثلاثة فحوص Clang/GCC C++. كما نجحت مجموعة الترقيع الثنائي المحلية الكاملة في الحالات الـ22,961 عند هذا الدمج. تبقى أعداد العمليات الأصلية منفصلة عن تغطية محاكاة المعالج.

اجتاز [تأهيل مُثبّت Windows المعاد بناؤه عند 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627581732) جميع الفحوص المركزة البالغ عددها 182 وعمليات تشغيل البرامج الأصلية البالغ عددها 474، دون إخفاقات أو حالات متجاوزة. تحقّق تدقيق مستقل للمخرجات من تجزئة كل صورة ومعماريتها وسيناريو التشغيل وعدد التكرارات. وبعد دمج إصلاحات ABI وفك المكدس عند `e1cac949f`، اجتازت مجموعة اختبارات الترقيع الثنائي الكاملة جميع الحالات البالغ عددها 22,961 مرة أخرى دون إخفاقات أو حالات متجاوزة. يظل NeverC مخصصًا للغة C الخالصة؛ وتستخدم بدائل C++ المترجم Clang/GCC.

<!-- i18n-section: runtime-coverage -->
## التحقق من عمليات macOS وأهداف 32 بت

يتطلب مسار صور العمليات تشغيلًا أصليًا على macOS ARM64 (`macos-15`) وIntel x64 (`macos-15-intel`). تنفذ كل بنية 96 ملاحظة تشمل تثبيت مصدر وكائنات Clang C، وحالة المؤشرات القابلة للكتابة، ومعاملات الفاصلة العائمة والمكدس، وتسعة سيناريوهات لاستثناءات Clang C++، ومخرجات IR والكائنات وhosted الحالية من NeverC للغة C الخالصة. تُفحص تواقيع Mach-O بصرامة قبل التشغيل، ويجب أن يطابق كود hosted المثبت حمولة المترجم بايتًا ببايت.

يتطلب المسار نفسه 30 ملاحظة Windows x86 على Windows x64، و48 ملاحظة Linux x86 على Linux x64، و96 ملاحظة Linux ARM32 عبر QEMU في نمط المستخدم، موزعة بالتساوي بين ARM وThumb. تشمل اختبارات Linux كائنات GCC المطابقة للهدف. يتكرر كل سيناريو ثلاث مرات. تميز التقارير بين `native` و`compatibility` و`rosetta` و`qemu-user`، وتسجل بنية الهدف وبنية المضيف الفعلية. لا يفي Rosetta بشرط تشغيل Intel الأصلي.

تظل ملفات أهداف CPU الأربعة عشر مغطاة بمصفوفة 283 اختبارًا للمصدر وIR والكائنات. يتوفر حاليًا تحقق CPU لـ PE ARM32 وMach-O i386 وARMv7 iOS وARMv7k watchOS، دون ادعاء تحقق محمل OS أصلي أو آلية فك استثناءات C++. يبقى NeverC مخصصًا للغة C الخالصة على x64 وARM64؛ ويتولى Clang وبرامج تشغيل ELF GCC المطابقة إنتاج أهداف 32 بت. لا يبني المتحقق NeverC ولا يعدله.

ينفذ كل من Windows x64 وARM64 أيضًا 36 ملاحظة لاستثناءات Clang C++ FH3.

تُهيأ مؤشرات Mach-O القابلة للكتابة أثناء التنفيذ؛ وما زالت تهيئة العناوين الثابتة تحتاج إلى تسجيل إعادة تموضع وقت التحميل غير المدعوم، مع اختبار صريح للرفض. لا تُقبل دوال COMDAT في ELF ذات الحجم الصفري إلا إذا كان رمز التوقيع هو الدالة الوحيدة عند بداية قسمها التنفيذي وطابقت CFI القسم كاملًا بدقة. وتظل حالات الإزاحة الداخلية والقسم المشترك والحشو وغياب المجموعة أو CFI مرفوضة.

لـ ELF ARM وThumb ملفا تحقق منفصلان. تتلقى أدوات إنتاج المصدر صراحةً `-marm` أو `-mthumb`، وتحافظ روابط الدوال الأصلية على بت حالة التعليمات. تستخدم عمليات ELF ذات 32 بت صور PIE بإعادة تموضع ديناميكية فعلية. يسجل تشغيل ARM32 عبر QEMU محمل glibc صريحًا `--loader` وsysroot؛ فحساب AUXV في QEMU لا يتبع رؤوس البرنامج المنقولة بالطريقة نفسها التي تتبعها نواة Linux. يتحقق هذا المسار من تحميل glibc والتنفيذ تحت QEMU، ولا يثبت التنفيذ على عتاد ARM32 أو بمحمل نواة أصلي.

بالنسبة إلى صور ELF هذه بعد تطبيق التصحيحات، يتطلب كل من مساري Linux x86 وARM32 عبر `qemu-user` محمل glibc صريحًا `--loader` من sysroot المطابق لبنية الهدف. يستخدم مسار Linux x86 في وضع `compatibility` دعم 32 بت في نواة مضيف Linux x64، ويُسجَّل منفصلًا عن تنفيذ QEMU؛ ولا يثبت التنفيذ على مضيف x86 أصلي ذي 32 بت.

اجتاز [تحقق العمليات](https://github.com/NeverSight/NeverD/actions/runs/38694434082) للإصدار `fcee81c9a5e2f20a71090ade23ae833175f64c59` جميع المشاهدات البالغ عددها 750: منها 576 على مضيفين أصليين بمعمارية مطابقة، و78 في وضع توافق x86، و96 لمعمارية ARM/Thumb عبر وضع المستخدم في QEMU. تحقق تدقيق مستقل من بصمات الصور الـ126، وبيئات التنفيذ، وقائمة السيناريوهات كاملة. اجتازت [إعادة البناء على Windows](https://github.com/NeverSight/NeverD/actions/runs/38694628242) عدد 194 من الاختبارات المركزة و474 مشاهدة لعمليات Windows/Linux. واجتازت [إعادة البناء على Linux](https://github.com/NeverSight/NeverD/actions/runs/38694638358) عدد 444 من الاختبارات، بما فيها جميع الحالات الأصلية البالغ عددها 62 حالة فشل و46 حالة تخطٍ، دون فشل أو تخطٍ. تظل حدود التحقق من العتاد ومحمّلات النظام المذكورة أعلاه سارية. يبقى NeverC مخصصًا للغة C الخالصة ولم يُعدّل مستودعه.

نجح [التحقق الإضافي من ARM/Thumb](https://github.com/gmh5225/test_mac_intel/actions/runs/38750504263) في جميع الملاحظات البالغ عددها 96 (48 لكل من ARM وThumb) للصور الاختبارية الـ32 الخاصة بلغة C من `fcee81c9a5e2f20a71090ade23ae833175f64c59` على `ubuntu-24.04-arm`. حمّلت نواة Linux كل صورة مباشرة عبر `/lib/ld-linux-armhf.so.3`، دون QEMU أو أمر تحميل صريح. هذا وضع `compatibility` على مضيف ARM64، وليس تحققًا على مضيف أصلي من فئة 32 بت. يُسجَّل التزام برنامج الاختبار `3ad17ac9608d6bd9f5a4d44953ec375524e5730b` والتزام الصور كلٌّ على حدة. تحققت عمليات تدقيق مستقلة من بصمات الصور، وإيصالات النشر الثلاثين كاملة، وجميع السيناريوهات والتكرارات، وعدم وجود قواعد مفسّر `binfmt_misc` مطابقة.

أظهرت [فحوص بيئات التشغيل المستضافة المنفصلة](https://github.com/gmh5225/test_mac_intel/actions/runs/38749138719) أن بيئة Windows 11 ARM64 المقدَّمة ترفض برامج ARM32 بالخطأ `WinError 216`. في macOS 15.7.9 مع Xcode 16.4، لم تتمكن حزم SDK المتاحة من ربط برامج macOS i386 أو iOS ARMv7. نجح ربط برنامج watchOS ARMv7k، لكن لم يُثبت تشغيله على جهاز فعلي. نجاح مهام الفحص لا يعني اعتماد هذه المنصات؛ فما زالت تغطية العتاد الأصلي غير مكتملة. لم يُبنَ NeverC ولم يُعدَّل.

نجحت [فحوص Linux الإضافية في Actions الشخصي](https://github.com/gmh5225/test_mac_intel/actions/runs/38773830274) في 96 تشغيلًا لـ ARM/Thumb داخل ضيف ARM32 كامل عبر QEMU TCG، بنواة Linux ذات 32 بت وglibc، إضافة إلى 30 تشغيلًا لـ PE ARM32 عبر Wine داخل الضيف نفسه. تحمّل نواة الضيف صور ELF مباشرة؛ وهذه محاكاة للنظام الكامل تُسجّل منفصلة عن مسار `qemu-user` ذي المحمّل الصريح. يُسجّل تثبيت سجل Wine على طبقة توافق المضيف ARM64 بصورة مستقلة ولا يُحتسب ضمن مرات تشغيل الرقع. يجب أن تنجح تهيئة Wine داخل الضيف وكل عملية تشغيل مقاسة ضمن المهل الأصلية.

نجحت [فحوص توافق Apple](https://github.com/gmh5225/test_mac_intel/actions/runs/38771214358) في 30 تشغيلًا لـ Mach-O i386 عبر Darling على Linux، و60 تشغيلًا لـ ARMv7/ARMv7k عبر touchHLE على macOS Intel. يُشغّل كل أصل وكل بديل ناتج من IR المصدر أو ملف كائن مُترجم أو ملف كائن مُقدّم ثلاث مرات. تحققت مراجعات مستقلة من الصور الـ 72، وسجلات نشر الرقع الـ 66، وعمليات التشغيل الـ 216 في هذه الفحوص الإضافية، بما فيها أحجام نواتج GitHub وبصمات SHA256 والالتزامات الدقيقة وقوائم الحالات الكاملة. تبقى مدخلات Linux مثبتة عند `fcee81c9a5`؛ أُنتجت مدخلات PE عند `a542f97ba` ومدخلات Mach-O عند `8f5f06762`. تُسجّل التزامات أدوات الاختبار منفصلة في عمليات التشغيل المرتبطة.

تتحقق إصلاحات Mach-O من دوال Thumb المختارة بصورة مستقلة عن وصف الصورة العام المختلط ARM/Thumb، وتوحّد مؤشرات تصدير Thumb المثبتة وعناوين شيفرة فك المكدس في ARM32 دون تغيير مؤشرات البيانات أو أطوال النطاقات. اجتاز البناء باستخدام مصادر LLVM المدمجة محليًا جميع فحوص مصفوفة CPU البالغ عددها 283 وفحوص عقود الاستثناءات البالغ عددها 258، دون إخفاق أو تخطٍ. تستخدم اختبارات Mach-O القديمة رابطًا مفتوح المصدر موثقًا وتحجز سعة لبيانات فك المكدس؛ ولـ i386 نقطة بدء بمحاذاة صحيحة ومقطع صفحة صفر بحجم 64 KiB.

تثبت هذه النتائج تنفيذ C ضمن المحاكيات وطبقات التوافق المذكورة. Wine ليس Microsoft Windows، وDarling ليس macOS، وتنفيذ ARMv7k داخل محاكاة iOS عالية المستوى لا يثبت دعم watchOS أو WatchKit أو أجهزة Apple الفعلية. لا يُستنتج منها تحقق من SDK قديم لـ Apple أو عتاد أصلي ذي 32 بت، كما لا تثبت هذه الاختبارات فك مكدس استثناءات C++. تبقى أدلة التشغيل الأصلي والتوافق السابقة منفصلة. لم يُبنَ NeverC ولم يُعدّل.
