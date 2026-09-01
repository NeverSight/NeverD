**اللغات**: [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← فهرس التوثيق](README.ar.md)

# إضافات Python

يستطيع NeverD تحميل ملف Python بوصفه إضافة من الدرجة الأولى. تشترك إضافات Python مع الإضافات الأصلية في البيانات الوصفية ودورة الحياة والترتيب وقواعد الأسماء المكررة وتدفق الأحداث وواجهة C ABI للجلسة. حزمة التطوير المدعومة هي `neverd-plugin`؛ لا تستورد الجسر الخاص `_neverd_plugin` مباشرة.

## متطلبات البناء والتشغيل

القيمة الافتراضية لـ `NEVERD_ENABLE_PYTHON_PLUGINS` هي `ON`. يتطلب البناء المفعّل مفسر CPython بإصدار 3.10 أو أحدث ومكتبة التطوير الخاصة بالتضمين، على أن يتمكن CMake من اكتشافهما:

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

استخدم `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` للحصول على `libneverd` أصلية فقط بلا اعتماد ربط على CPython. يضع البناء المفعّل لـ Python الحزمة المطابقة والأمثلة داخل `build/bin/sdk/python/`؛ ويمكن تثبيت هذا الدليل مباشرة بالأمر `python3 -m pip install build/bin/sdk/python`.

## كتابة إضافة

تصرّح الوحدة بفئة واحدة مزينة بالضبط:

```python
from neverd_plugin import Event, Plugin, PluginType, Session


@Plugin(
    name="Analysis Report",
    version="1.0.0",
    author="Your team",
    description="Reports basic information about the loaded binary",
    type=PluginType.PROCESSOR,
)
class AnalysisReport:
    def on_init(self, session: Session) -> int | None:
        print(session.architecture)
        return None

    def on_run(self, session: Session, arg: int) -> int | None:
        print(session.file_path, session.function_count)
        return 0

    def on_event(self, event: Event) -> int | None:
        print(event.type.name)
        return None

    def on_term(self) -> None:
        pass
```

جميع نقاط الربط اختيارية. تعني `None` النجاح؛ ويجب أن تتسع نتيجة العدد الصحيح داخل `int` في C. تستخدم إصدارات البيانات الوصفية SemVer الصارم. يجب أن تكون الأسماء سلاسل UTF-8 غير فارغة، وتُرفض أي بيانات وصفية تحتوي على NUL مضمّن.

أمثلة المستودع هي [`minimal.py`](../pluginsdk/python/examples/minimal.py) و[`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py) و[`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py) لواجهات التحسين المقيّدة بالإثبات.

## تحميل الإضافات وفحصها

تستطيع واجهة C تحميل ملف `.py` محدد بصورة حتمية أو فحص دليل:

```c
if (!neverd_plugins_load_file(session, "plugins/report.py")) {
  const char *message = neverd_last_error(session);
  /* log message */
  neverd_free_string(message);
}

neverd_plugins_init(session);
int result = neverd_plugins_run(session, "Analysis Report", 0);
neverd_plugins_term(session);
```

تعرّف `neverd_plugins_list_json` كل عنصر بواسطة `"kind":"python"` أو `"kind":"native"`. يُرتّب اكتشاف الدليل حسب المسار المعياري ويقبل المكتبات الأصلية وملفات Python في الدليل نفسه. تُعد المسارات المعيارية المكررة وأسماء الإضافات المكررة أخطاء.

## واجهة الجلسة والأحداث

تعيد `Session` التحقق من قدرة المضيف قبل كل استدعاء C. تشمل واجهتها محددة الأنواع البيانات الوصفية للملف والمعمارية والتنسيق، وعرض البتات وأعداد الجداول، وعروض الدوال، والتحميل والتحليل، وقراءة البايتات، والتفكيك، وفك الترجمة، والاستعلامات الشائعة. تتيح `session.raw` كل تصريح في `neverd_plugin.abi` للعمليات المتقدمة:

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

### استكشاف رمزي محدود للمسارات

تعيد `session.symbolic_explore` للدوال الأصلية في LowIR نتائج مسارات محددة الأنواع، وآثار الكتل الأساسية، واستخدام الموارد، ومسندات المسارات الاختيارية:

```python
result = session.symbolic_explore(
    0x401000,
    max_paths=64,
    max_steps=1 << 16,
    max_block_visits=3,
    include_expressions=True,
)
if not result.exact:
    print(result.unmodelled_ops)
for path in result.paths:
    print(path.outcome, path.blocks, path.predicate)
```

تكون `complete` بقيمة false عندما يوقف الاستكشاف حد للمسارات أو الخطوات أو زيارات الحلقات أو الفروع غير المحلولة. وتتطلب `exact` أيضاً ألا تكون أي عملية قد استُبدلت بصورة محافظة بحالة مجهولة؛ إذ تُحتسب عمليات LowIR غير المدعومة، والاستدعاءات التي بلا ملخص، وعمليات التخزين عبر عناوين غير محلولة ضمن `unmodelled_ops`. لا تتيح جلسات EVM وSBF استكشاف LowIR الأصلي.

### قلب فروع LowIR المتزامن المتحقق منه

يتتبع `session.lowir_concolic` مسار LowIR أصلياً واحداً انطلاقاً من نطاقات بايت صريحة لسجل الدخول، ولا يعيد إلا المرشحين الذين أنشأهم المحلل وتحقق منهم تشغيل جديد عند موضع قرار التحكم نفسه:

```python
from neverd_plugin import ConcolicRegisterSeed

report = session.lowir_concolic(
    0x401000,
    [ConcolicRegisterSeed(offset=56, bytes=4, value=0)],
)
for flip in report.flips:
    if flip.candidate_id is not None:
        print(report.candidates[flip.candidate_id].seed)
```

إزاحة السجل هي إزاحة بايت في ملف سجلات NeverD، وليست مؤشراً أصلياً أو رقم سجل. التقرير غير شامل دائماً، وتبقى حالات UNSAT وحدود المحلل ورفض الإسقاط أو إعادة التشغيل نتائج قلب ذات أنواع محددة وليست استثناءات.

### تدقيق وصيد أمان الذاكرة

تعيد `session.audit()` و`session.hunt()` تقارير JSON محلولة (نفس مخطط CLI). تتطلب جلسة أصلية مرفوعة:

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

ترفض جلسات EVM وSBF هذه الاستدعاءات.

متغيرات الأحداث الستة غير القابلة للتغيير هي `BINARY_LOADED` و`BINARY_CLOSING` و`FUNCTION_SELECTED` و`ADDRESS_CHANGED` و`ANALYSIS_DONE` و`PATCH_APPLIED`. تُنسخ سلاسل الحمولة أثناء callback؛ وتكون الحقول غير المتعلقة بنوع الحدث `None`.

لا تحتفظ أبداً بكائن `Session` لاستخدامه بعد الإنهاء. تُبطل capsule الأصلية قبل بدء `on_term` وقبل إمكان تحرير الجلسة الأصلية. يفشل أي استدعاء لاحق بـ `RuntimeError` بدلاً من إلغاء مرجع ذاكرة قديمة.

### التركيب المحكوم بالبرهان وتحسين LLVM

الدالة `synthesize_expression` منفصلة عن `simplify_expression` التي أُبقيت
لتوافق ABI وتعمل على MBA فقط. لا يُعتمد أي تحويل إلا إذا أعاد المحلّل
`ProofStatus.EQUIVALENT`. أمّا المثال المضاد أو البرهان غير المكتمل أو نفاد
ميزانية البحث فيُبقي التعبير الأصلي ويعرض النتيجة وكلفة البحث والبرهان كلًّا
على حدة.
تشير `ProofStatus.INVALID` إلى أن مسألة البرهان نفسها غير صالحة، وتبقى مميزة عن
`ProofStatus.UNKNOWN` الناتجة من الميزانية؛ وكلتاهما ترفضان التحويل بصورة آمنة.

تجمع `optimize_llvm_ir` بين نقطة الثبات الدلالية في NeverD ومسار LLVM القياسي
المختار على نسخة معاملية، ولا تعيد إلا الوحدة التي تم التحقق منها واعتمادها:

```python
from neverd_plugin import (
    LLVMOptimizationLevel,
    OptimizationMode,
    ProofStatus,
    optimize_llvm_ir,
    synthesize_expression,
)

rewrite = synthesize_expression(
    "(x >> 4) + ((x >> 2) >> 2)", exhaustive=True
)
if rewrite.changed:
    assert rewrite.proof_status is ProofStatus.EQUIVALENT

module = optimize_llvm_ir(
    llvm_ir,
    mode=OptimizationMode.DEEP,
    llvm_level=LLVMOptimizationLevel.O2,
    enable_synthesis=True,
    exhaustive=True,
)
print(module.output_ir, module.semantic_rewrites, module.proof_queries)
```

يمكن لبرامج الإنتاج تقييد عمل وعدد مدخلات MBA، وبحث التركيب وعمل SAT، وتقارب LLVM
كلٌّ على حدة. في `simplify_expression` يختار الخيار الصريح `exhaustive=True`
سياسة MBA بلا سقف للعدد أو العمل، ويزيل سقفي سياسة التداخل وعرض البت في المحلل
الأصلي. وفي `synthesize_expression` يزيل سقوف المحلل وعمل البحث وSAT مع الإبقاء
على القواعد التي يحددها المستدعي؛ أما في `optimize_llvm_ir` فيزيل سقوف التقارب
والبحث وSAT. لا تضيف طبقة Python قيدًا آخر على التعبير، وتظل حدود سلامة الذاكرة
وتمثيل IR سارية. نقاط الدخول المكافئة في C هي `neverd_simplify_expr` و
`neverd_synthesize_expr` و`neverd_optimize_llvm_ir`، مع دوال تحرير مكتوبة النوع
ومهايئات JSON ذات إصدارات.

## الأخطاء والعزل والثقة

لا تعبر استثناءات Python عبر C++ أثناء فك المكدس. يلتقط NeverD الـ traceback الكامل والمنسق ويعرضه عبر `neverd_last_error`. يُحمّل كل مسار معياري لإضافة باسم وحدة فريد؛ يزيل الإنهاء الوحدة، وتحصل إعادة التحميل اللاحقة على حالة جديدة للوحدة والفئة. تتم تهيئة CPython مرة واحدة، ويُحرر GIL الخاص بالبدء، وتكتسب callbacks الـ GIL على أي خيط مضيف. لا ينهي NeverD مفسراً قد يشاركه مع مكوّن آخر.

تنفذ الإضافات شيفرة Python اعتباطية داخل عملية NeverD ويمكنها استدعاء واجهة C كاملة. حمّل الملفات الموثوقة فقط. هذه حدود امتداد وليست sandbox.

## التطوير والاختبارات والحزم

للحصول على دعم المحرر ومدقق الأنواع، ثبّت حزمة Python الخالصة أو أضف شجرة المصدر إلى `PYTHONPATH`:

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

يتطلب التدقيق تطابقاً تاماً بين كل تصريح C مُصدّر وتوقيع `ctypes` وقاعدة الملكية المقابلة له. ويتحقق أيضاً من قيم لغة الإخراج، وإصدارات CMake والحزمة، وأعلام ميزات CI، وإصدارات Actions المثبتة، وتدفق العناصر، وسياسة PyPI OIDC. اختبارات المحوّل الأصلي هي `NeverDPluginRuntimeTests`؛ واختبارات Python المضمّن هي `NeverDPythonRuntimeTests` و`NeverDPythonPluginTests`.

يبني workflow ‏`Python Plugin SDK` ملف wheel واحداً وتوزيعة مصدر واحدة، ويثبت كليهما في بيئات نظيفة، ثم يرفع العناصر المتحقق منها. لا يحدث النشر إلا لـ GitHub Release منشور، عبر environment ‏`pypi` المحمية بالموافقة وTrusted Publishing؛ ولا يُستخدم token طويل الأجل لـ PyPI.
