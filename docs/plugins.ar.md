**اللغات**: [English](plugins.md) | [简体中文](plugins.zh-CN.md) | [繁體中文](plugins.zh-TW.md) | [日本語](plugins.ja.md) | [한국어](plugins.ko.md) | [Français](plugins.fr.md) | [Deutsch](plugins.de.md) | [Español](plugins.es.md) | [Italiano](plugins.it.md) | [Русский](plugins.ru.md) | [العربية](plugins.ar.md)

[← فهرس التوثيق](README.ar.md)

# الإضافات الأصلية

إضافات NeverD الأصلية هي مكتبات مشتركة موثوقة تُحمَّل داخل عملية المضيف.
وهي تستخدم تصريحات C الخالصة في `neverd/sdk/NeverDPlugin.h` وتستدعي واجهة C
العامة في `neverd/sdk/NeverDCAPI.h`. استخدم
[دليل إضافات Python](python-plugins.ar.md) عندما يكون التأليف بـPython داخل
العملية أنسب.

## التوافق وحدود الثقة

لا يحتوي الواصف الحالي `neverd_plugin_t` على حقل لإصدار ABI أو لحجم البنية.
ابنِ الإضافة باستخدام الرؤوس المرحّلة من مراجعة NeverD نفسها بالضبط التي
ستحمّلها، وأعد بناء الإضافة كلما رُقّي NeverD. يجب أيضًا أن تستخدم الإضافة
والمضيف نظام التشغيل والمعمارية نفسيهما وسلاسل أدوات متوافقة مع ABI.

الإضافات الأصلية شفرة عشوائية تعمل داخل العملية. لا يعزلها NeverD في sandbox،
ولا يعزل الأعطال، ولا يقيّد وصولها إلى الجلسة أو عملية المضيف. لا تحمّل إلا
الإضافات التي تثق بها.

## الواصف والاستدعاءات الراجعة

تُصدّر كل مكتبة رمز بيانات واحدًا اسمه بالضبط `neverd_plugin`:

```c
#include "neverd/sdk/NeverDPlugin.h"

static int on_init(neverd_session_t session) {
  (void)session;
  return 0;
}

static void on_term(void) {}

static int on_run(neverd_session_t session, int arg) {
  (void)session;
  return arg;
}

static int on_event(const neverd_event_t *event) {
  if (event && event->Type == NEVERD_EVT_BINARY_LOADED) {
    const char *path = event->Data.BinaryLoaded.Path; /* borrowed */
    (void)path;
  }
  return 0;
}

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = "My Plugin",
    .Version = "1.0.0",
    .Author = "Your Name",
    .Description = "A native NeverD extension",
    .Type = NEVERD_PLUGIN_GENERIC,
    .Init = on_init,
    .Term = on_term,
    .Run = on_run,
    .Event = on_event,
};
```

يتوسع `NEVERD_PLUGIN_EXPORT` إلى `__declspec(dllexport)` على Windows وإلى
الرؤية الافتراضية لـELF/Mach-O في الأنظمة الأخرى. أبقِ المصدر بلغة C، أو امنح
الواصف ربط C صراحةً إذا كان تنفيذ C++ لا مفر منه.

يجب أن يكون `Name` غير فارغ وفريدًا داخل المضيف الواحد. يأخذ المضيف نسخة من
سلاسل البيانات الوصفية الأربع عند التحميل. يجوز أن تكون `Version` و`Author`
و`Description` فارغة. قيم الأنواع الأربع ليست إلا بيانات وصفية للتصنيف:

| القيمة | معناها حاليًا |
|--------|---------------|
| `NEVERD_PLUGIN_GENERIC` | تسمية إضافة عامة |
| `NEVERD_PLUGIN_LOADER` | تسمية محمّل؛ لا تسجّل محمّل ملفات ثنائية |
| `NEVERD_PLUGIN_PROCESSOR` | تسمية تحليل/معالجة؛ لا تجدول عملًا |
| `NEVERD_PLUGIN_UI` | تسمية واجهة مستخدم؛ لا يوفّر NeverD مضيف GUI للإضافات الأصلية |

كل مؤشرات الاستدعاءات الراجعة اختيارية. الاستدعاءات مباشرة ومتزامنة على خيط
مستدعي المضيف.

| الاستدعاء الراجع | العقد |
|------------------|-------|
| `Init(session)` | أعد `0` عند النجاح. تسجّل القيمة غير الصفرية خطأً؛ ولن يُستدعى `Term` بعد ذلك التهيئة الفاشلة. |
| `Term()` | يُستدعى أثناء الإنهاء فقط بعد `Init` ناجح. ثم تُفرّغ المكتبة. |
| `Run(session, arg)` | نفّذ عمل الإضافة وأعد نتيجة صحيحة. تمرّر CLI القيمة `0`؛ وقد تختار واجهة C المضمِّنة وسيطة أخرى. يعيد غياب الاستدعاء `-1`. |
| `Event(event)` | عالج حدثًا أرسله المضيف. أعد `0` عند النجاح؛ وتسجّل القيمة غير الصفرية خطأً. غياب الاستدعاء لا يفعل شيئًا. |

ترتيب التضمين المعتاد هو التحميل، فالتهيئة، ثم التشغيل أو إرسال الأحداث، ثم
الإنهاء. لا تفرض واجهة C هذا الترتيب على `Run` أو `Event`، ولذلك يملك المضيف
المضمِّن دورة الحياة.

## الأحداث يرسلها المضيف

يحمل `neverd_event_t` إحدى قيم الأحداث الست:

| الحدث | الحمولة في `event->Data` |
|-------|--------------------------|
| `NEVERD_EVT_BINARY_LOADED` | `BinaryLoaded.Path` |
| `NEVERD_EVT_BINARY_CLOSING` | بلا حمولة |
| `NEVERD_EVT_FUNC_SELECTED` | `FuncSelected.Addr`, `FuncSelected.Name` |
| `NEVERD_EVT_ADDR_CHANGED` | `AddrChanged.Addr` |
| `NEVERD_EVT_ANALYSIS_DONE` | بلا حمولة |
| `NEVERD_EVT_PATCH_APPLIED` | `PatchApplied.OutputPath`, `PatchApplied.CodeSize` |

لا تُصدر واجهات الجلسة ولا أداة سطر الأوامر `neverd` الأحداث تلقائيًا. ينشئ
المضيف المضمِّن الحدث ويرسله:

```c
neverd_event_t event = {0};
event.Type = NEVERD_EVT_BINARY_LOADED;
event.Session = session;
event.Data.BinaryLoaded.Path = input_path;
neverd_plugins_dispatch_event(session, &event);
```

الحدث وسلاسل حمولته مستعارة حتى عودة الاستدعاء؛ فلا تحررها ولا تحتفظ بها.
مقبض الجلسة ملك للمضيف: يجب ألا تدمره الإضافة أو تستخدمه بعد إنهاء المضيف.
النتائج التي تخصصها واجهة C لـNeverD تخص NeverD ويجب تحريرها بواسطة
`neverd_free_string()`. لا يَعِد NeverD بإتاحة متزامنة للاستدعاءات أو الجلسة؛
وعلى المضيفين المضمِّنين تسلسل استدعاءات دورة الحياة والتشغيل والأحداث ما لم
يوفروا مزامنة آمنة خاصة بهم.

## بناء المثال المضمّن

فعّل الأمثلة صراحةً؛ يبقى الافتراضي `NEVERD_BUILD_PLUGINS=OFF`:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --target neverd example_plugin
```

مع مولّد أحادي الإعداد، تكون المنتجات ذات الصلة:

| المنتج | المسار |
|--------|--------|
| CLI | `build/bin/neverd` (`neverd.exe` on Windows) |
| مكتبة المضيف | `build/bin/libneverd.so`, `build/bin/libneverd.dylib`, or `build/bin/neverd.dll` |
| الرؤوس المرحّلة | `build/bin/sdk/neverd/sdk/` |
| المثال | `build/bin/plugins/example_plugin.so`, `.dylib`, or `.dll` |

يضع البناء متعدد الإعدادات منتجات التشغيل نفسها تحت الإعداد المحدد، مثل
`build/bin/Release/`، مع الإضافة في `build/bin/Release/plugins/` والرؤوس في
`build/bin/Release/sdk/`:

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DNEVERD_BUILD_PLUGINS=ON
cmake --build build --config Release --target neverd example_plugin
```

## بناء إضافة مستقلة

لا يثبّت NeverD حاليًا SDK الخاص به ولا يوفّر `NeverDConfig.cmake`، لذلك لا
يوجد `find_package(NeverD)` مدعوم. وجّه البناء المستقل إلى الرؤوس المرحّلة
ومكتبة ربط صريحة من بناء المضيف نفسه بالضبط:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_neverd_plugin LANGUAGES C)

set(NEVERD_SDK_ROOT "" CACHE PATH
    "Directory containing neverd/sdk/NeverDPlugin.h")
set(NEVERD_LINK_LIBRARY "" CACHE FILEPATH
    "libneverd.so, libneverd.dylib, or the Windows neverd.lib import library")

if(NOT EXISTS "${NEVERD_SDK_ROOT}/neverd/sdk/NeverDPlugin.h")
  message(FATAL_ERROR "Set NEVERD_SDK_ROOT to the staged NeverD SDK")
endif()
if(NOT EXISTS "${NEVERD_LINK_LIBRARY}")
  message(FATAL_ERROR "Set NEVERD_LINK_LIBRARY to the matching libneverd")
endif()

add_library(my_plugin SHARED my_plugin.c)
target_include_directories(my_plugin PRIVATE "${NEVERD_SDK_ROOT}")
target_link_libraries(my_plugin PRIVATE "${NEVERD_LINK_LIBRARY}")
set_target_properties(my_plugin PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  PREFIX "")
```

اضبط `NEVERD_SDK_ROOT=/absolute/path/to/build/bin/sdk` (أو دليل `sdk` الخاص
بالإعداد). على Linux/macOS تكون `NEVERD_LINK_LIBRARY` ملف `.so`/`.dylib`
المطابق. وعلى Windows تكون مكتبة الاستيراد `neverd.lib` التي أنشأها المولّد،
في حين يجب أن يبقى `neverd.dll` المطابق بجوار الملف التنفيذي للمضيف. تعتمد
مواقع مكتبة الاستيراد على المولّد، لذا مرّر الملف الفعلي صراحةً.

## الاكتشاف وجولة CLI

تفحص CLI الأدلة بالترتيب التالي؛ تفوز المسارات المعيارية وأسماء الإضافات
الأسبق:

1. `plugins` بجوار الملف التنفيذي `neverd` الجاري.
2. `$HOME/.neverd/plugins` (يُستخدم `HOME` عندما يكون غير فارغ؛ وفي Windows يكون دليل ملف التعريف الأصلي هو الخيار الاحتياطي).
3. كل مدخلة غير فارغة في `NEVERD_PLUGIN_PATH`، بالترتيب.
4. الدليل الممرّر بواسطة `--plugin-dir`.

يستخدم `NEVERD_PLUGIN_PATH` الرمز `:` بين المدخلات على Linux/macOS والرمز `;`
على Windows. تُفحص الأدلة المتكافئة بعد تطبيع مساراتها مرة واحدة. لا يُنظر للمكتبات
الأصلية إلا إلى لاحقة المضيف: `.so` على Linux، و`.dylib` على macOS، و`.dll`
على Windows. كما تفحص الأبنية المفعّل فيها Python ملفات `.py`.

مثال شجرة البناء موجود مسبقًا في الدليل المجاور للملف التنفيذي:

```bash
build/bin/neverd plugins --list
build/bin/neverd plugins --list --json
build/bin/neverd plugins --run "Example Plugin"
build/bin/neverd plugins --run "Example Plugin" --binary path/to/binary
```

في البناء متعدد الإعدادات استبدل `build/bin` بـ`build/bin/Release`. ولتثبيت
نسخة لملف NeverD تنفيذي لا توجد الإضافة نفسها بجواره مسبقًا، استخدم أمر منصة
المضيف ثم شغّل ذلك الملف التنفيذي بخياري `--list` و`--run` نفسيهما:

```bash
# Linux
mkdir -p "$HOME/.neverd/plugins"
cp build/bin/plugins/example_plugin.so "$HOME/.neverd/plugins/"

# macOS
mkdir -p "$HOME/.neverd/plugins"
cp build/bin/plugins/example_plugin.dylib "$HOME/.neverd/plugins/"

# Windows PowerShell
New-Item -ItemType Directory -Force "$HOME/.neverd/plugins"
Copy-Item build/bin/plugins/example_plugin.dll "$HOME/.neverd/plugins/"
```

تُتجاهل أدلة الجوار/المنزل الاختيارية المفقودة. تنتج الإضافة المشوّهة في دليل
اختياري تحذيرًا ويستمر الفحص. كل دليل يسمّيه `NEVERD_PLUGIN_PATH` أو
`--plugin-dir` مطلوب: يؤدي الدليل المفقود أو الملف المرشح المرفوض إلى خروج CLI
بقيمة غير صفرية. تُرفض الملفات ذات المسار المعياري المكرر، وأسماء الإضافات
المكررة، وغياب تصدير `neverd_plugin`، وأنواع الواصف غير الصالحة. كما يرفض
الاستدعاء المباشر `neverd_plugins_load_file` ملفًا ذا لاحقة غير مدعومة.

على المضيفين المضمِّنين التحقق من نتائج إدارة الإضافات ومن
`neverd_last_error(session)` معًا. يعيد `neverd_plugins_load_file` القيمة `1` أو
`0`؛ ويعيد `neverd_plugins_load_dir` عدد الإضافات المحمّلة ويمكنه الإبلاغ عن
مرشحين مرفوضين حتى بعد نجاح جزئي. يعيد `neverd_plugins_run` نتيجة الإضافة
ويستخدم `-1` عندما تكون الإضافة غائبة أو يتعذر تشغيلها. حرّر كل سلسلة خطأ أو
JSON تعيدها واجهة C بواسطة `neverd_free_string()`.
