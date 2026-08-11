**اللغات**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# إعادة بناء استثناءات Windows

[← فهرس التوثيق](README.ar.md)

يحمل NeverD معلومات استثناءات Windows المعتمدة على الجداول عبر التحميل وlift
وإزالة الترجمة وإعادة كتابة الملف الثنائي. تمثل metadata الاستثناء جزءًا من
العقد التنفيذي للدالة؛ وتُرفض إعادة الكتابة عندما يتعذر إثبات اتساق الشفرة
المولدة وrecords الخاصة بـruntime-function وجداول اللغة وجداول الحماية.

يميز هذا المستند ثلاثة مستويات للدعم:

- **التحليل**: فك التمثيل الأصلي إلى records طبيعية ومفحوصة وإتاحتها لـIR pipeline.
- **إزالة الترجمة**: تحويل المناطق المحمية القابلة للاختزال إلى عقد استثناء
  HighIR صريحة، مع إبقاء annotations أصلية حتمية للأشكال الأخرى.
- **إعادة البناء الأصلية**: يستطيع patch mode طلب عقد استثناء بديل كامل من LLVM
  وتثبيته في PE النهائي.

لا يعني دعم التحليل دعم إعادة البناء الأصلية.

## مصفوفة الدعم

| الشكل الأصلي | Lift والتحليل | الخرج عالي المستوى | Patch mode |
|--------------|---------------|---------------------|------------|
| x64 unwind v1/v2 | Records وعمليات وسلاسل وبيانات handler ومصدر كاملة ومفحوصة | ملخص frame/unwind ومناطق لغة منظمة عند الإمكان | Records primary كاملة؛ تستبدل `.pdata`/`.xdata` المولدة الإغلاق القديم |
| x64 unwind v3/APX | Payload v3 وepilog وحساب عمليات مخصص | Annotations v3 صريحة | تحليل فقط؛ تُرفض الدالة المتأثرة |
| ARM32/ARM64 packed unwind | نطاقات وحقول packed وهوية primary/fragment | ملخص frame/unwind | فقط record primary كامل بلا language handler أو fragment مستقل العنوان |
| ARM32/ARM64 unpacked unwind | Header/extent لـxdata مفحوصان، وربط handler وfragments | ملخص frame/unwind | فقط record primary كامل بلا language handler أو fragment مستقل العنوان |
| `__C_specific_handler` | Scopes وfilters وأهداف finally وhandlers وcontinuations | `__try`/`__except`/`__finally` عند القابلية للاختزال، وإلا annotation | إعادة بناء x64 أصلية لرسوم scope الكاملة القابلة للتمثيل |
| `__CxxFrameHandler3` | Unwind/try maps وcatches وoffsets وcontinuations وIP-to-state | الفترات القابلة للاختزال كـC++ HighIR مع type annotations متوافقة مع C | إعادة بناء x64 للمجموعة الضيقة verifier-clean أدناه |
| `__CxxFrameHandler4` | فك متغير محدود إلى رسم C++ المشترك | HighIR نفسه مع مصدر FH4 | تحليل فقط؛ تُرفض الدالة المتأثرة |
| `__GSHandlerCheck_SEH/EH/EH4` | Personality مغلفة ومصدر GS cookie مفحوص | رسم اللغة الأساسي وannotation للwrapper | تحليل فقط؛ رفض بلا downgrade |
| x86 registration-chain EH | منفصل عن EH الجدولي | Annotation لشكل غير مدعوم | لا يعاد بناؤه |

لا يُعامل record malformed كأنه كامل. يفيد الفك الجزئي في الفحص، لكنه لا يسمح
بالتوليد الأصلي. إذا ظل header لـARM xdata يثبت نطاق fragment تنفيذيًا محدودًا
رغم تلف unwind body، يبقى النطاق متاحًا للتفكيك، لكن record يبقى malformed وغير
قابل للpatch.

## النموذج الطبيعي

يمتلك `BinaryImage` كائن `ExceptionInfo`. يحتوي كل `ExceptionFunction` على:

- نطاق شفرة نصف مفتوح مفحوص؛
- هوية primary أو chained أو fragment؛
- encoding unwind أصلي ومصدر runtime/unwind دقيق؛
- عمليات وepilog طبيعية مع operands معتمة للعمليات غير المفهومة؛
- هوية personality دقيقة وبيانات handler؛
- scopes SEH وخرائط حالة C++ وبيانات GS cookie اختيارية؛
- حالة `Complete` أو `Partial` أو `Malformed` وتشخيص حتمي.

لا يكشف loader مؤشرات ملف خام. تُحفظ RVA الأصلية للتشخيص والاستبدال، بينما
يستخدم مستهلكو IR عناوين ونطاقات محققة فقط.

يسمح فهرس الصورة بتداخل records chained/fragment ويعيد الدالة الأكثر تحديدًا.
تخفض الأدلة أو النطاقات أو المؤشرات أو العدادات أو الانتقالات أو الأعداد المضغوطة
أو السلاسل التالفة، ونفاد decode budget، حالة التحليل المناسبة.

تطبق الحدود لكل جدول وعلى رسم الدالة كله. لا يؤدي تكرار handler map في try entries
كثيرة إلى تجاوز الميزانية الإجمالية. تشكل records FH3 ذات `FuncInfo` وpersonality
المشتركين مجموعة محدودة؛ فتقبل catch funclets التابعة لها لا عناوين runtime غريبة.

## عقد IR

تمر metadata الاستثناء عبر كل التمثيلات دون تغيير معنى CFG العادي:

- يقسم LowIR الكتل عند حدود النطاقات والحالات وfilters وhandlers وcleanup وcontinuation.
- تبقى successors/predecessors الاستثنائية منفصلة عن الحواف العادية.
- يحتفظ MedIR بالوصف الطبيعي والحواف الاستثنائية الثابتة.
- يميز HighIR بين `SEHTry` و`CxxTry` ويحفظ VA وtype descriptors وadjectives
  وoffsets وactions وstates وcontinuations.

يتعامل HighIR structurer بتحفظ: لا ينقل إلا slice متصلة تقع بالكامل داخل منطقة
كاملة، ويعالج التداخل من الداخل. تبقى المناطق المتقاطعة والرسوم الجزئية والحدود
الملتبسة وfunclets الخارجية في control flow الأصلي.

يصدر C backend صياغة MSVC SEH لمنطقة قابلة للاختزال ذات clause واحدة. ولأن HighC
هو C backend، تصبح catches وcleanup الخاصة بـC++ تعليقات C حتمية، ولا يُدعى أن
الخرج C++ قابل للترجمة.

## مخطط metadata في LLVM

تحصل كل دالة استثناء محللة على metadata بلا فقد، حتى دون WinEH lowering أصلي:

- attachment باسم `neverd.windows.eh`؛
- marker أصلي `neverd.windows.eh.native`؛
- module table باسم `neverd.windows.eh.functions`؛
- schema version رقم `3`.

يحفظ record الثابت الحالة وencoding والنطاق وRVA وrecord kind والسلسلة والكلمة
packed وframe وأسماء personality وhandler وunwind bytes والعمليات وepilogs وSEH
scopes وخرائط C++ وبيانات GS والتشخيص وإذن regeneration. يتطلب patch النسخة
الدقيقة وتطابق النطاق الكامل مع الصورة المحملة.

يستخدم lowering الأصلي لـx64 SEH بنية LLVM WinEH ولا يصدر control flow من
`invoke`/funclet يكون verifier-clean إلا للرسم القابل للتمثيل كله. ويتطلب FH3:

- x64 COFF وunwind v1/v2 وmetadata كاملة ورسم FH3 متزامن صالح؛
- غياب `noexcept` وasync وseparated-funclet وGS وFH4 وflags المجهولة؛
- فترات متداخلة أو منفصلة، لا متقاطعة؛
- غياب destructor/unwind action وبناء catch object واعتماد parent frame؛
- handler في كتلة عادية بلا predecessor أو call؛
- LLVM `invoke` لكل عملية محمية قد تنفذ unwind.

وإلا يبقى IR قابلًا للتحليل وبلا فقد، لكن يُرفض الاستبدال الأصلي. تمثل PE entry
point وTLS callbacks وCRT roots حدود حفظ لا مرشحين عاديين لإعادة ABI.

## معاملة patch

تمثل إعادة الكتابة المدعومة معاملة PE واحدة:

1. تحقق كل دالة متأثرة مقابل الرسم المحمل وmetadata في LLVM.
2. ترجم مع حفظ هوية ومحاذاة وtraits الأقسام والمراجع الدلالية؛ externalize
   personality المحلية واربط xdata بالhandler التنفيذي الأصلي المثبت.
3. احتفظ بـruntime functions غير المتأثرة واحذف الإغلاق المستبدل كله مع chained records.
4. Relocate للشفرة وxdata، وادمج ورتب pdata، وارفض overlap، وأثبت personality
   class، ثم ثبت exception directory وحيدًا.
5. احتفظ بـCFG، وحل `.gfids`/`.gehcont`، وادمج Guard CF/EH continuation، وحدث
   load-config. يوقف helper غير المحلول العملية؛ وتبقى CFW وreturn-flow guard
   وretpolines وXFG للتحليل فقط.
6. أعد تحليل byte image المكتملة قبل الكتابة.

يبقى امتداد fork الخاص بـLLVM عامًا: يحفظ final-image writer صفات الأقسام ومراجع
الرموز. تبقى PE/MSVC والسياسة والدمج وload-config والتحقق النهائي داخل NeverD.

تُحفظ entries الأصلية لـGuard CF/EH continuation لأن trampolines تظل أهدافًا غير
مباشرة صالحة. يجب أن تقع الأهداف المولدة في الشفرة المصدرة وأن ترتب الجداول بدقة
حسب RVA.

## التحقق من الصورة النهائية

يُرفض PE المعدل ما لم يتحقق كله:

- يقبل LLVM صيغة COFF وتتطابق machine وclass وsections وdirectories وbase وextent؛
- تكون extents الخام والافتراضية محدودة وغير متداخلة؛
- يكون exception directory مدعومًا بالملف وداخل الصورة؛
- تكون runtime functions مرتبة وغير فارغة وغير متداخلة وتنفيذية؛
- تكون RVA/header/version/flags/handler/chains الخاصة بـx64 unwind صالحة؛
- تسمح imports وexports وCOFF symbols النهائية بإعادة تحليل SEH/FH3؛
- تصف records ARM وxdata نسخة ونطاقًا مدعومين؛
- توجد حقول Guard CF/EH عندما تعلن flags الجداول؛
- تقع pointers/counts/strides داخل الملف والصورة وتكون الأهداف مرتبة وتنفيذية.

يوقف أي فشل patch ولا تكتب صورة best-effort.

## التحقق المركز

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

تستخدم fixture x64 المحمية `/guard:cf` و`/guard:ehcont` وتتحقق من scopes SEH
وGuard وHighC وpatch وإعادة التحميل والترتيب والأهداف. تتحقق fixture FH3 من
الجداول الثابتة وannotations وpersonality وtry/catch وIP-to-state. شغل أيضًا
حالات ARM عند تغيير parser.

## توسيع الدعم الأصلي

يجب أن يضيف كل شكل أصلي جديد في التغيير نفسه:

- parser كاملًا ومحدودًا وثوابت النموذج؛
- round-trip لـHighIR وmetadata في LLVM؛
- IR أصليًا verifier-clean لكل رسم مقبول؛
- حفظ الأقسام والمراجع اللازمة؛
- fixture PE مرتبطة للarchitecture/personality/version الدقيقة؛
- تحقق exception-directory وload-config وfinal-image؛
- اختبارات رفض صريحة لأقرب الأشكال غير المدعومة.

لا تكفي القدرة على فك record لتوسيع allow-list. المعيار هو حفظ سلوك الاستثناءات
وقت التشغيل في الصورة النهائية المرتبطة.
