**اللغات**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md) | [繁體中文](CONTRIBUTING.zh-TW.md) | [日本語](CONTRIBUTING.ja.md) | [한국어](CONTRIBUTING.ko.md) | [Français](CONTRIBUTING.fr.md) | [Deutsch](CONTRIBUTING.de.md) | [Español](CONTRIBUTING.es.md) | [Italiano](CONTRIBUTING.it.md) | [Русский](CONTRIBUTING.ru.md) | [العربية](CONTRIBUTING.ar.md)

# المساهمة في NeverD

NeverD مشروع لتحليل الملفات الثنائية يضع الدلالات أولًا. تكون المساهمة
المفيدة محددة النطاق، وتجعل السلوك غير المدعوم يفشل بوضوح، وتتضمن أصغر اختبار
يثبت العقد الذي تغير.

اقرأ [دليل العمارة](../architecture.ar.md) قبل التعديل. استخدم
[دليل الاختبارات](../testing.ar.md) لاختيار المجموعة، وراجع
[خارطة الطريق](../roadmap/README.ar.md) للعمل المخطط على المنتج.

## المتطلبات

- Git مع دعم الوحدات الفرعية المتكررة
- CMake 3.20 أو أحدث
- Ninja
- مترجم يدعم C++20
- Clang وLLD ‏(`ld.lld` و`lld-link`) لمجموعة fixtures الكاملة متعددة الأهداف

توفر الوحدات الفرعية المتكررة فرعي LLVM وCapstone الخاصين بـ NeverD، وUnicorn،
وبيانات التواقيع. لا تستبدلها بإصدارات نظام عشوائية عند التحقق من تغيير.

## الاستنساخ والتهيئة

يُدمج التطوير في `dev`، وهو أيضًا الفرع الافتراضي للمستودع. استنسخه مع جميع
الوحدات الفرعية:

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

في نسخة موجودة، زامن الوحدات الفرعية قبل أول بناء وبعد أي commit يغير
مراجعاتها المسجلة:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

## اختيار ملف البناء

| الملف | الاستخدام | السلوك المهم |
|-------|-----------|---------------|
| Release | التطوير العادي، كل الاختبارات، معايير decode/lift | محسّن؛ إنتاجية ممثلة |
| RelWithDebInfo | تحليل أو تصحيح المسارات الساخنة المحسّنة | محسّن مع رموز التصحيح |
| Debug | التأكيدات، التتبع على مستوى المصدر، التحقق المحلي | غير محسّن؛ معايير decode أبطأ بكثير عمدًا |

استخدم Release ما لم تتطلب المهمة سلوك Debug تحديدًا:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel
```

يبني الإعداد الافتراضي `third_party/llvm-project` كتبعية مدمجة. يستغرق البناء
الأول عادة 30–60 دقيقة؛ وما بعده تزايدي. يعرّف `CMakePresets.json` أيضًا إعدادات
التهيئة/البناء `release` و`relwithdebinfo` و`debug`، لكن الدلائل الصريحة أعلاه
تجعل تفعيل الاختبارات ظاهرًا.

للتصحيح على مستوى المصدر، استخدم دليلًا منفصلًا بدل إعادة تهيئة شجرة Release:

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build-debug --parallel
```

لا تنشر أبدًا إنتاجية decode أو lift من بناء Debug. استخدم Release للمعايير،
أو RelWithDebInfo عندما يحتاج التحليل إلى الرموز.

### LLVM جاهز على macOS

يمكن للمساهمين على Apple Silicon تجنب بناء فرع LLVM محليًا:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

ينزّل CMake حزمة الإصدار المضبوطة في المستودع، ويتحقق من SHA-256، ثم يعيد
استخدام ذاكرة المستخدم المؤقتة المفكوكة في الأبنية اللاحقة. تدعم القناة
الجاهزة macOS arm64 فقط. يجب أن تستخدم أجهزة Intel Mac والأبنية العامة بناء
LLVM المحلي الافتراضي. توثّق الخيارات المتقدمة مثل
`NEVERD_LLVM_PREBUILT_TAG`، وعنوان المرآة، ودليل الذاكرة المؤقتة، والمجموع
الصريح في `cmake/NeverDLLVMPrebuilt.cmake`.

## سير الفروع وطلبات السحب

ابدأ من `dev` محدّث وأنشئ فرع موضوع محددًا:

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

افتح طلبات السحب إلى `dev`، لا إلى فرع إصدار مفترض. اجعل الـ commits سهلة
المراجعة: غرض واحد مترابط، دون مخرجات بناء مولدة أو تنسيق غير متعلق، ودون
تغيير مراجعات الوحدات الفرعية إلا إذا كانت جزءًا من المقترح.

## نمط الشفرة

تتبع C وC++ أعراف LLVM، ويمثل `.clang-format` مرجع التنسيق في المستودع.
نسّق الملفات التي عدلتها فقط:

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

لا تعد تنسيق المستودع كله لإصلاح محدد. اتبع أنماط التسمية والتقسيم المحيطة،
وأبق السلوك الخاص بالمنصة عند حد loader/lifter/backend المناسب، ولا تعرض أنواع
C++ الداخلية عبر SDK بلغة C الخالصة.

يجب أن يكون Markdown موجزًا وقابلًا للتحقق من المصدر. استخدم روابط نسبية
لملفات المستودع، وحدّث التوثيق في طلب السحب نفسه عندما يتغير سلوك CLI أو API
العامة أو ادعاءات الدعم أو خيارات البناء أو أوامر الاختبار.

## تشغيل الاختبارات

شغّل كل الاختبارات المسجلة عبر الهدف التجميعي:

```bash
cmake --build build-release --target check-neverd
```

أثناء التطوير استخدم أصغر هدف ذي صلة أو وسم CTest:

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

يوثّق [دليل الاختبارات](../testing.ar.md) جميع الأهداف المريحة، ومجموعات التحويل
ذات الوسم فقط، والتعابير النمطية لاختبار واحد، وتجميع fixtures، ودورات Unicorn.
إذا جرى تخطي هدف لغياب مترجم عابر أو linker، فأبلغ عن القيد؛ ولا تصف المسار
الذي لم يُنفّذ بأنه ناجح.

## قائمة مراجعة طلب السحب

قبل طلب المراجعة:

- نفّذ rebase أو merge لآخر `dev` وفق السير الذي يفضله المشرفون، وتعامل مع
  تغييرات الوحدات الفرعية عمدًا.
- ابنِ الأهداف المتأثرة في Release، أو اشرح سبب الحاجة إلى ملف آخر.
- شغّل اختبارات الانحدار الدقيقة وأوسع مجموعة عملية ذات صلة؛ وأدرج الأوامر
  الدقيقة وكل skips في وصف PR.
- حافظ على الرفع الصارم: يجب ألا تتحول التعليمة غير المدعومة بصمت إلى عملية
  مخمّنة أو `NOP`.
- أضف تغطية دلالية لتغييرات السلوك، لا لقطات IR نصية فقط.
- أبق التنظيف غير المتعلق، والملفات المولدة، وآثار البناء المحلية خارج الفرق.
- حدّث التوثيق العام وتوثيق المساهمين عندما يتغير السلوك أو الدعم أو الخيارات
  أو الأوامر أو ملكية الاختبارات.

للتقارير الأمنية الحساسة التي ينبغي ألا تبدأ بطلب سحب عام، اتبع
[SECURITY.md](../../SECURITY.md).
