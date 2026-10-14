**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](source-patching.md)

<!-- i18n-source: 3ceb56e2abf86f594f437a16b09a1888650cb49d4cf06860711e73458c52c68d -->

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
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++؛ Mach-O x64/ARM64 C/C++؛ PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3 |
| Clang 22.1.6 | كائن أصلي | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C |
| GCC/G++ 13.3.0 | كائن أصلي | ELF x64/ARM64 C/C++ مع بيئة Itanium متوافقة |
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
## مراجع البرنامج الأصلي والتخزين

تطابق الروابط تصريحات خارجية: يتطلب `function` دالة أصلية أو مدخل استيراد مؤكداً؛ ويتطلب `data` تخزيناً قابلاً للكتابة؛ ويتطلب `const` تصريح IR ثابتاً. تحتاج روابط بيانات الكائن إلى امتداد غير صفري. لا يجوز تجاوز حدود التخزين أو منح الكتابة إلى RELRO. يرفض استدعاء الجسم الأصلي للدالة المستبدلة؛ لا يحتوي هذا الملف على قفزة رد تحفظ مقدمة الدالة.

تبقى الشيفرة الجديدة RX والحالة القابلة للتعديل RW. يعاد استخدام الاستيرادات الموجودة فقط، مع التمييز بين خانات GOT/IAT ومداخل الاستدعاء. يحتاج Mach-O إلى مساحة لأوامر التحميل مثل `-Wl,-headerpad,0x400`؛ ويحتاج PE إلى مساحة لرؤوس الأقسام. يفشل نقص المساحة. تتطلب ELF `.data.rel.ro` وحماية CET/IBT الأصلية عقوداً إضافية وترفض حالياً.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

يتطلب نمو جداول فك المكدس Mach-O أيضاً مساحة ملف ومساحة افتراضية مجاورتين غير مستخدمتين؛ لا تحجزها مساحة الرأس وحدها. تميز العينات المتحققة بين الشرطين.

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
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## البناء والتحقق

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

تعد المشاهدات الـ204 لملف الترجمة السابق سجلا تاريخيا ولا تثبت عقد C الخالصة الحالي. تستخدم مسارات NeverC الثلاثة الحالية لغة C، وتختبر Clang C++ FH3 منفصلة. تستخدم macOS x64 بيئة Rosetta 2. لا تحل نتائج CPU في Unicorn/QEMU محل التحقق الأصلي من النظام وفك المكدس.