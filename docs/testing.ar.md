**اللغات**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← فهرس التوثيق](README.ar.md)

# اختبار NeverD

تجيب اختبارات NeverD عن ثلاثة أسئلة مختلفة: هل للتمثيل الشكل المتوقع، وهل يعمل
مسار pipeline كامل مع fixture ثنائية، وهل تحافظ الشفرة المولدة على السلوك؟ اختر
أصغر مجموعة تجيب عن سؤال التغيير، ثم شغّل التجميع الأوسع قبل طلب سحب عالي المخاطر.

## تهيئة بناء الاختبار

تكون الاختبارات معطلة ما لم يُفعّل `BUILD_TESTING`. يمثل Release الخيار العادي
للمجموعة الكاملة؛ يحتفظ Debug بالتأكيدات والتتبع، لكنه غير محسّن عمدًا ولا يمثل
معايير decode.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

تحتاج مجموعة fixtures الكاملة إلى `clang` للتجميع العابر للأهداف وإلى linker
الخاصة بـ LLVM ‏(`ld.lld` و`lld-link`) في `PATH`. يبني CMake كثيرًا من fixtures
القابلة لإعادة التموضع دائمًا، وfixtures ‏ELF/PE المرتبطة عند توفر linker المطابق.
الاختبار المتخطى لأن المضيف لا يستطيع تجميع fixture أو ربطها هو تغطية غير
منفذة، وليس نجاحًا للهدف.

راجع [CONTRIBUTING.md](i18n/CONTRIBUTING.ar.md) للاستنساخ وملفات البناء وLLVM
الجاهز على macOS.

## توزيع الاختبارات

ينشئ `add_neverd_unittest` ملف GoogleTest تنفيذيًا واحدًا، ويمنح كل حالة مكتشفة
وسم CTest يساوي اسم ذلك الهدف التنفيذي.

| منطقة المصدر | الهدف ووسم CTest | التغطية |
|--------------|------------------|---------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | استدعاء العمليات الفرعية عابر المنصات، وquoting، وإعادة التوجيه، ورموز الخروج |
| `unittests/libc` | `NeverDLibCTests` | أسماء libc المعروفة وتصنيفها |
| `unittests/safety` | `NeverDSafetyTests`، `NeverDSafetyIntegrationTests` | كتالوج المصارف، وأولوية الهوية، ومرشح الوسائط المسبق، وصيد فيضان النسخ، وتدقيق عمر الكومة، ومصفوفة إلزامية من ست خلايا PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/lift` | `NeverDLiftTests` | أشكال LowIR لـ decoder/lifter، ومراحل IR، وloader، وrelocation، وfixtures الصيغ، وإعادة التجميع، ومسارات patch الممثلة |
| معظم ملفات `unittests/semantic` | `NeverDSemanticTests` | دلالات تفاضلية للتعليمات وABI والتحكم وتعابير C وlift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests` و`NeverDEVMBytecodeTests` و`NeverDEVMLoaderTests` و`NeverDEVMABITests` و`NeverDEVMAnalyzerTests` و`NeverDEVMDecoderPropertyTests` و`NeverDEVMProxyTests` و`NeverDEVMCallTests` و`NeverDEVMSemanticTests` و`NeverDEVMEmitterTests` و`NeverDEVMIntegrationTests` | metadata للـhardfork وتطبيع الإدخال وغموض ABI/signature وCFG/SSA والاستعادة واستنفاد حدود decoder والمدخلات العدائية وحقائق proxy/call ودلالات interpreter وتنفيذ LLVM/C/Solidity التفاضلي وتوجيه API العامة |
| `unittests/sbf` | `NeverDSBFMetadataTests`، `NeverDSBFProgramImageTests`، `NeverDSBFLoaderTests`، `NeverDSBFAnalyzerTests`، `NeverDSBFVerifierTests`، `NeverDSBFISAConformanceTests`، `NeverDSBFAgaveConformanceTests`، `NeverDSBFSemanticTests`، `NeverDSBFEmitterTests`، `NeverDSBFLLVMEmitterTests`، `NeverDSBFLLVMDifferentialTests`، `NeverDSBFSourceDifferentialTests`، `NeverDSBFMalformedCorpusTests`، `NeverDSBFUpstreamConformanceTests`، `NeverDSBFExternalOracleTests`، `NeverDSBFSolanaModelTests`، `NeverDSBFIntegrationTests` | بيانات v0-v4 الوصفية وتخطيطات ELF، وسلوك التحقق والتحميل الصارم، و23 من عناصر ELF المثبتة، وoracle الرسمي المنفصل، وتغطية opcode الشاملة، والمدخلات العدائية، وCFG/الاستعادة، وفروق LLVM/C/Rust المنفّذة |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | تكافؤ إعادة الكتابة/التشويش عبر أربع ISA وثلاث صيغ كائنات |
| ملفات التحويل المحددة في `unittests/semantic` | `NeverDSwitchXformTests` و`NeverDIndCallXformTests` و`NeverDCFGLoopXformTests` و`NeverDTwoTableXformTests` و`NeverDAvxUpperXformTests` | مجسات سريعة الربط منفصلة عن الثنائي الدلالي الكبير |
| `unittests/corpus` (وحدة فرعية) | `NeverDWindowsEHCorpusTests` و`NeverDRustEHCorpusTests` و`NeverDGoEHCorpusTests` و`NeverDCxxItaniumEHCorpusTests` و`NeverDObjCEHCorpusTests` | metadata الاستثناءات ووقت التشغيل المقروءة من 317 ثنائيًا حقيقيًا مثبّتًا، كل واحد منها معلن في manifest يذكر الحدود الدنيا التي يجب أن يتجاوزها استرجاعه |

مصادر التسجيل الموثوقة هي
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt) و
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) و
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt) و
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) و
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt) و
[`unittests/safety/CMakeLists.txt`](../unittests/safety/CMakeLists.txt).

### الـcorpus الثنائي المثبّت

كل مجموعة اختبارات أخرى تبني ما تختبره، أما الـcorpus فلا: إنه وحدة فرعية من
ثنائيات أنتجتها سلاسل أدوات حقيقية، على مضيفات ولأهداف لا يستطيع هذا المستودع
بلوغها، وكل ملف مثبّت بالبصمة وإلى جانبه manifest يذكر الحدود الدنيا التي يجب أن
يتجاوزها استرجاعه. هذا هو المكان الوحيد الذي يصير فيه ادعاء عن ما تقرأه NeverD
من — مثلًا — كائن مشترك `armv7` مبني بـ`-O2` ومجرّد من الرموز قابلًا للإجابة بدل
أن يكون محل جدل.

لا تُبنى هذه المجموعات إلا حين يُطلب من خطوة الإعداد البحث عنها، فهذا الخيار هو
كل ما يبقيها تحت الاختبار:

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

يشغّل `check-neverd-corpus` كل الخطوط، بينما يشغّل
`check-neverd-windows-eh-corpus` و`check-neverd-rust-eh-corpus` و
`check-neverd-go-eh-corpus` و`check-neverd-cxx-itanium-eh-corpus` و
`check-neverd-objc-eh-corpus` خطًا واحدًا لكل منها. تُعدّ مضيفات الـCI الثلاثة جميعها
بهذا الخيار وتشغّل الخطوط الخمسة: البايتات واحدة في كل مكان، أما ما يقرؤها فليس
كذلك، وتشغيل الـcorpus على مضيف واحد لا يثبت شيئًا عن المضيفين الآخرين. يرفض
`scripts/audit_ci_test_inventory.py` أي inventory ينقصه أحد الـlabels الخمسة، لأن
بناءً توقف بصمت عن قراءة الـcorpus هو انحدار لا يستطيع أي اختبار التقاطه —
فالاختبار نفسه هو ما اختفى.

يُشغّل تدقيق opcodes ‏EVM الحي بالأمر التالي:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

يفرض المسار القياسي محليًا وفي CI تنفيذ
`git fetch --depth=1 --force` من URL الرسمي
`https://github.com/ethereum/go-ethereum.git`، ولا يفحص في worktree مؤقت detached
إلا SHA الدقيق الذي جُلب للتو من remote `HEAD` للـdefault branch. كل تشغيل يستخدم
bare repository خاصًا مؤقتًا باسم غير متوقع، ويحفظ authority ref
الذي أعاده fetch والـSHA الدقيق المحلول منه طوال عمر worktree الـdetached، ثم
يدمرهما معًا. لا يوجد Git repository أو cache دائم مشترك. ليست
`local_docs` ولا checkout موجود ولا submodule مسارات تدقيق؛ إذ يصبح pin
الـsubmodule قديمًا تحديدًا عند الحاجة إلى كشف live drift.

تحذف كل أوامر Git أولًا جميع `GIT_*` الموروثة، ومنها `GIT_CONFIG_*`، ثم تثبت
القيم المدققة فقط. يعطل `GIT_CONFIG_NOSYSTEM` و`GIT_CONFIG_GLOBAL` إعدادات
system/global؛ ويعطل `GIT_ATTR_NOSYSTEM` و`core.attributesFile` على مستوى الأمر
attributes النظام والعامة، كما يعطل `core.hooksPath` hooks. يفشل الفحص عند إعداد غير متوقع في
الـrepository الخاص أو
grafts أو `objects/info/alternates` أو `refs/replace`، ويعطل
`GIT_NO_REPLACE_OBJECTS` replacement lookup.

يعكس probe كل حقول bool المصدرة في `params.Rules`،
ويستدعي `LookupInstructionSet(params.Rules)` ويفحص كل 256 slots. يملك
`EVMUpstreamOpcodePolicy.def` aliases وtyped exclusions التاريخية/EOF غير
المجدولة؛ ويملك `EVMUpstreamSemanticsPolicy.def` inventory الـRules المغلق وfork
mappings وbase-stack exceptions وعائلات dynamic-immediate.

يشغل CI التدقيق الحي نفسه فقط عند push إلى `dev` وpull request والتشغيل اليدوي
والجدول اليومي. يستدعي Go probe الواجهة العامة
`LookupInstructionSet(params.Rules)` لكل fork مربوط. يملك
لا تعرض CLI العامة سوى `--manifest-output`؛ ويستخدم manifest المغلق `schema 3`
ولا يسمح باختيار source أوref أوcheckout أوtoolchain.
`EVMUpstreamOpcodePolicy.def` aliases والاستثناءات التاريخية/EOF غير المجدولة المراجعة،
بينما يملك `EVMUpstreamSemanticsPolicy.def` المستقل قواعد forks واستثناءات stack
semantics. يفحص manifest المغلق revision الدقيقة وactivation وbyte/name و
`base_min_stack` و`net_stack_delta`، ويرفض fields وforks والأسماء والbytes المجهولة
أو المكررة. يحدد probe allocation من `operation.undefined` وحده؛ و`HasCost` مجرد
فحص متقاطع للكلفة لأن العملية المعرّفة صفرية الكلفة تعيد false أيضًا. يجب أن
يطابق كل `defined && !HasCost` ‏slot تصريح `EVM_GETH_ACTIVE_WITHOUT_COST` تمامًا
من fork التفعيل المحدد؛ ويفشل undefined slot ذو كلفة أو defined slot غير مراجع
أو اختفاء marker بشكل مغلق. تفشل كذلك declarations المفقودة أوخارج النطاق أوغير
المستهلكة نحويًا؛ فكل `.def parser` يرفض policy ‏`partial`. عند فشل CI تُرفع
revision وmanifest وlog كـartifact. للـparser وتشخيص الانحراف تغطية unit مستقلة:

يصنّف `EVMUpstreamSemanticsPolicy.def` كل boolean field مصدّر في `params.Rules`
بسجل `EVM_GETH_RULE_FIELD` واحد: `MappedForkSelector` أو `NoOpcodeAllocation` أو
`ExcludedSelectorExpectedError`. يفعّل probe كل field منفردًا عبر
`LookupInstructionSet`؛ يجب أن يعيد الصنفان الأولان nil error والثالث error، وأن
تطابق fingerprint الكاملة لـ256 opcode/stack slots قيمة `ExpectedFork`.
الحقول `IsEIP155` و`IsEIP2929` و`IsEIP4762` و`IsPetersburg` بلا allocation وتطابق
Frontier؛ أما `IsUBT` فيجب أن يفشل ويطابق Cancun.

يصرح `EVMUpstreamSemanticsPolicy.def` بعائلات opcodes الديناميكية EIP-8024 ونوع
العملية وstack delta الصالح، بينما يملك `EVMEIP8024Immediates.def` فك immediate
المنفصل ويصنف 256 byte من single/pair. عبر `go -overlay` يحصل التدقيق على private
handlers الحقيقية `operation.execute`، ويمر على `canonical fork jump tables` و
`mainnet active/scheduled jump tables` جدولًا بجدول. يسجل العائلة `inactive`
ويرفض `partial`. يختبر كل جدول نشط `DUPN` و`SWAPN` و`EXCHANGE` مع كل immediate (`3x256`)
و`3 missing-operand cases` مقابل المصادر التصريحية نفسها.

لـ`EVM_HARDFORK_LATEST` target canonical واحد. يربط
`EVMUpstreamForkAliases.def` المغلق Prague→Pectra وOsaka وBPO1–BPO5→Fusaka، وتعود
Paris/Shanghai/Cancun/Amsterdam/Bogota إلى نفسها؛ وتفشل الأسماء المجهولة مغلقًا.
تقود قيمة `audit_unix_time` المسجلة فحص `MainnetChainConfig.LatestFork(time)`
(يجب أن يساوي NeverD latest) وفحص alias/probe لـ`LatestFork(max uint64)`؛ وتقارن
مجموعتا التعليمات كاملتين. يثبت manifest ‏`authority=official-fresh-fetch` وURL الرسمي
و`HEAD` وSHA. يستخدم probe ‏`GOTOOLCHAIN=local`.

يفرض Go probe وPython controller ‏`input/collection/string hard limits`؛ فتفشل
المدخلات أوالمجموعات أوالنصوص الضخمة مغلقًا. أما
`bounded diagnostic output` فيرفق بالعرض الطويل `digest` كامل المحتوى و
`explicit truncated marker`. يطبق على كل child خرج وdeadline محدودان؛ وعند
التجاوز تُقتل `process group` كاملة/process tree وتُصرّف pipes.

تسجل وصلة schema 3 الحالية `schema_version=3` و
`audit_unix_time=1787534659` و`authority=official-fresh-fetch` و
`remote=https://github.com/ethereum/go-ethereum.git` و`ref=HEAD` وrevision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562` و`Go 1.24.0` محلية و
`stack_limit=1024` و`diagnostics=[]`. تغطي `21 fork tables` و`20 Rules probes`
بتصنيف `15 mapped/4 no-op/1 expected-error`. يسمي سجلا
`mainnet active/scheduled` ‏`upstream BPO2` المربوط مغلقًا بـ`NeverD Fusaka`.
ومن `23 table targets` لا ينشط إلا `Amsterdam/Bogota`، بنتيجة
`1536 candidate executions` و`6 missing-operand cases`. وتتطابق
`three handler symbols` بين الهدفين النشطين. نجح Python audit ‏`67/67` و
`C++ Opcode 10/10`. نجح macOS تحت `sandbox-exec` مع `go run` نهائي بلا شبكة،
ويفرض Linux ‏`bubblewrap`.

تمر كل مراحل Go، أي `go env` و`go mod init` و`go mod edit` و`go mod tidy` و
`go mod download` و`go run`، عبر filesystem sandbox من نوع `capability-root`.
تسمح القراءة فقط لـprivate probe وfresh geth و`resolved GOROOT` المتحقق منه وجذور
system runtime المطلوبة بدقة، وتسمح الكتابة فقط في isolated environment roots.
تمنح الشبكة لمراحل dependencies التي تحتاجها وحدها ويبقى run النهائي offline.
تثبت الاختبارات أن sentinels في `host HOME/workspace` مرفوضة وأن محتواها لا يظهر
في output. ويطابق Linux هذه السياسة بـ`bubblewrap` من دون `/` broad bind.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

أهداف اختبار EVM الأحد عشر المسجلة حاليًا في CMake هي:

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

يستنفد `NeverDEVMDecoderPropertyTests` كل مدخلات البايتين في كل fork يغيّر
decoder، ويقارن decode كاملًا وحدود `JUMPDEST` الدقيقة، ثم يمرر مدخلات عدائية
حتمية محدودة الطول عبر كل forks.

لتغييرات control flow في EVM، شغّل أولًا عقد fixed point وheight domain:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

تغطي هذه الحالات returns عابرة للـblocks وmerges محدودة متعددة الأهداف وتقارب
loops وترتيب edges الحتمي وwhole-stack lanes الحساسة للمسار وحفظ correlation و
unknown jumps والأهداف غير الصالحة الدقيقة وfail-loud budgets وstack faults.
لا يمثل `MayReachable` إلا مرشح CFG ولا ينتج حقيقة يقينية. بعدها شغّل أهداف EVM
الأحد عشر كلها مع live upstream audit.

ولتغييرات dataflow في MedIR/HighIR، شغّل أيضًا عقود constant-phi وselector
وtyped-operand وmalformed-graph وdeep-chain:

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

تثبت هذه الحالات phis الدورية المتساوية والمتعارضة، وتعابير selector غير
المتجاورة والعابرة للـblocks، وترتيبي operands للمساواة، وفحوص ABI width الدقيقة،
وoperands النوعية لـstorage/event/calldata، والتعامل الحتمي مع MedIR malformed،
وproducer walk تكرارية من 16,384 قيمة.

## كيفية إنتاج fixtures

### Fixtures الرفع والصيغ

يجمع `unittests/lift/CMakeLists.txt` مصادر C وassembly لأهداف متعددة أثناء
البناء. تنتج triple ‏Clang كائنات ELF لـ x86-64 وi386 وAArch64 وARM32، وكائنات
PE/COFF وصورًا مرتبطة، وكائنات Mach-O i386 ‏PIC/no-PIC. عند توفر LLD، تُربط
كائنات مختارة أيضًا كملفات تنفيذية لاختبارات patch. يعتمد `NeverDLiftTests` على
هدف `lift-test-objects`، لذلك يجدّد البناء العادي لذلك الثنائي fixtures المولدة.

تستخدم معظم اختبارات lift ‏`NeverDLiftFixture.h` لاستدعاء CLI المبنية `neverd`
وفحص LowIR وMedIR وHighIR وLLVM IR وC المولدة أو ثنائي معاد كتابته. يمكن لمتغير
البيئة `NEVERD` تجاوز مسار CLI في تجربة يدوية محددة؛ وتستخدم عمليات CTest العادية
الملف التنفيذي الذي يضمنه CMake.

### Fixtures سلامة الذاكرة

يحتوي `unittests/safety/fixtures/binaries` على صور PE وELF وMach-O مُودعة
لمعماريتَي x86-64 وAArch64، مع ملف PDB أو dSYM المرافق الذي توفّره كل صيغة،
إضافةً إلى ملف MAP من الرابط لكل صورة. الـMAP هو ما تبقى البُنية المجرّدة من
الرموز تُصدره، لذلك تُحلَّل كل خانة أيضًا مع تسمية الـMAP صراحةً، وهو ما يثبّت
ما يحق للنتيجة أن تدّعيه حين لا تبقى أنواع ولا أسطر مصدرية. يشغّل
`NeverDSafetyIntegrationTests` الخانات الست كلها على كل مضيف؛ وتفشل التهيئة إذا
غابت أي صورة أو ملف مرافق مطلوب، ولا يملك الطقم أي مسار تخطٍّ يعتمد على سلسلة
أدوات المضيف.

تأتي الثنائيات المتكافئة من ملف مصدري واحد. أعد بناء fixture الدخان الأصلية
للمضيف بـ `make`، أو أعد توليد المصفوفة المُودعة كاملةً بـ:

```bash
make -C unittests/safety/fixtures matrix
```

تحتاج وصفة المصفوفة إلى أهداف Clang المتقاطعة للينكس وويندوز، وأدوات COFF من
LLD، ومعماريتَي Darwin كلتيهما، و`dsymutil`. تُعاد خرائط مسارات التنقيح ويُعطَّل
تسجيل سطر أوامر CodeView حتى لا تلتقط الملفات المرافقة المُودعة المسار المطلق
لمساحة عمل المطوّر.

### إعادة بناء استثناءات Windows

تحتاج تغييرات الاستثناءات الجدولية في Windows إلى اختبارات للتمثيل واختبار patch
لملف PE مرتبط. يغطي مرشح مجموعة lift المركّز النموذج الموحّد لـ
unwind/SEH/C++، ومعالجة المدخلات التالفة، وحواف CFG الاستثنائية، وHighIR،
وتوليد LLVM WinEH، واستبدال دليل الاستثناءات، وإعادة بناء Guard CF/EH
continuation:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

تتطلب fixture التجميع x64 المحمية هدف Windows في Clang و`lld-link`؛ ويستخدم
ربط CMake الخيارين `/guard:cf` و`/guard:ehcont`. التخطي بسبب غياب cross-linker
ليس دليلاً على مسار الصورة النهائية. تثبت حالة التكامل الناجحة أن PE المعاد
كتابته يمكن تحميله مجددًا وأن جداول runtime-function وunwind وload-config وGuard
CF وGuard EH continuation مرتبة، ومدعومة ببيانات الملف، وتشير إلى أهداف قابلة
للتنفيذ.

تغطي fixture FH3 المرتبطة إغلاق C++ الأصلي بصورة مستقلة: جداول الحالة الثابتة،
وتعليقات HighC، والحفاظ على personality، وأهداف catch المولدة، ومخطط IP-to-state
بعد إعادة التحميل.

راجع [إعادة بناء استثناءات Windows](windows-exception-reconstruction.ar.md)
لمصفوفة دعم التحليل/التوليد الأصلي وعقد patch الذي يفشل بأمان.

### نماذج الاستثناءات حسب اللغة

كل ما ليس نموذج جداول Windows يقع في هدف واحد مركّز. يغطي
`NeverDLanguageEHTests` سلسلة إطارات DWARF، ومنطقة البيانات الخاصة باللغة في
Itanium، وARM EHABI، وcompact unwind الخاص بـ Darwin، وبيانات إطارات زمن تشغيل
Go، وآلية panic في Rust، وأزمنة تشغيل Objective-C الثلاثة:

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

تُبنى جداول هذه المجموعة بايتًا بايت بدل تصريفها، لأن معظم التوليفات المقصودة لا
تُصدِرها أي سلسلة أدوات واحدة معًا. وObjective-C أوضح مثال: أزمنة التشغيل الثلاثة
تُصدِر جميعها LSDA بصيغة Itanium ولا تختلف إلا فيما تحمله خانة جدول الأنواع، وهذا
الاختلاف كلي لا تدريجي. خانة Apple تُعنون `objc_typeinfo` الذي صُمِّم حقلاه الأولان
عمدًا ليحاكيا `std::type_info`؛ وخانة Objective-C++ في GNUstep تُعنون صنفًا مشتقًا
حقيقيًا من `std::type_info`؛ أما خانة زمن تشغيل GNU فليست مؤشرًا أصلًا بل سلسلة اسم
الصنف نفسها. وتطبيق عرف زمن تشغيل على جدول زمن تشغيل آخر لا يفشل، بل يُبلِّغ عن اسم
صنف قُرئ من وسط شيء آخر تمامًا؛ ولهذا يُحدَّد زمن التشغيل من personality الإطار قبل
قراءة أي خانة.

وتثبِّت المجموعة نفسها تمييزين يسهل دمجهما ويكون دمجهما خطأً. فـ `@catch(id)` و
`@catch(...)` معالِجان مختلفان — الأول يستقبل أي كائن Objective-C ويدع الاستثناء
الغريب يمر بجانبه — وكل زمن تشغيل يكتبهما بصورة مختلفة، فالمفكِّك الذي يُبلِّغ عنهما
معًا كـ catch-all يضع معالجًا على استثناءات كانت في الواقع ستمر دون توقف. كما أن
جدول مواقع الاستدعاء في نموذج setjmp/longjmp يفهرس مواقع الاستدعاء لا العناوين،
فالقارئ الذي لا يتعرف على إحدى personalities الخاصة بـ SJLJ لا يُخفق، بل يخترع
نطاقات محمية وlanding pads لم يسمِّها البرنامج قط.

والتعرّف على هذا الشكل ليس كرفض فكّه. فالمدخل الواحد في SJLJ زوج من قيم ULEB128 —
مُحدِّد إرسال وإزاحة action — وهذه الإزاحة تعني هنا ما تعنيه تمامًا في الشكل العنواني،
ولذلك تُقرأ سلسلة الـ action وأنواع الـ catch ومواصفات الاستثناء كلها من جدول لا يسمّي
أي شيفرة على الإطلاق. ولا يبقى مجهولًا سوى النطاق الذي يحرسه كل مدخل، لأن ما ينصّ عليه
هو ما تكتبه الدالة نفسها في خانة call-site الخاصة بها، لا أي شيء في الجدول. كما تثبّت
المجموعة البايت الوحيد الذي لا يجوز الوثوق به هنا: يكتب GCC `DW_EH_PE_uleb128` ترميزًا
لـ call-site ويكتب LLVM `DW_EH_PE_udata4`، ثم يُصدر كلاهما ULEB128 على أي حال، ولا
تقرأه أي personality قط — فلا يجوز لمُفكِّك الترميز أن يقرأه أيضًا.

وتُثبَّت إلى جانب ذلك هوية الـ personality، لأنها هي ما يقرّر كيف يُقرأ كل جدول أعلاه.
فـ GNAT يسمّي إجراءه بالطرق الثلاث التي يسمّي بها GCC إجراء كل واجهة أمامية — `_v0`
و`_sj0` و`_seh0` — ويسجّل على Windows رمزًا بينما يحيل إلى آخر، فوجب أن تؤول التهجئات
الأربع جميعها إلى Ada. أما D فهي الصورة المعكوسة: ثلاثة مترجمات، وثلاثة أسماء لإجراء
واحد، ووراءها مجموعة جداول واحدة.

### دورات Unicorn التفاضلية

تختبر fixture الدلالية السلوك بدل الشكل النصي:

1. اكتب حالة C/assembly صغيرة أو أنشئ LLVM IR.
2. اجمعها بـ Clang/LLVM للهدف المطلوب.
3. نفّذ شفرة الآلة الأصلية في Unicorn والتقط قيمة العودة المتوقعة أو حالة أخرى تعرفها fixture.
4. حمّلها وارفعها عبر NeverD، وأصدر LLVM IR، ثم أعد تجميع النتيجة إلى شفرة آلة.
5. نفّذ الشفرة المعاد توليدها بنفس ABI والمدخلات وتخطيط الذاكرة ونموذج CPU.
6. قارن النتائج الملحوظة.

التنفيذ الأساسي هو
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h).
تستخدم fixture ‏patch-full ‏`Codegen::compileForRewrite`، وهو backend إعادة
الكتابة نفسه لعمليات patch، ثم تقارن الشفرة الأساس والمحوّلة عبر شبكة ISA/صيغة
الكاملة 4×3.

يجب أن يكون فشل NeverD الدلالي الحتمي اختبارًا فاشلًا. احصر skips في حدود قدرة
خارجية صريحة واقرأ سببها: لا يثبت ملخص أخضر بلا cross-linker أن مسار الصيغة نُفذ.

### واجهات EVM الخلفية التفاضلية

توفر اختبارات interpreter لـEVM oracle حتميًا بعرض 256 بت. تبني suite الـemitter
وتشغل LLVM، وتحوّل C23 عبر Clang إلى harness host نفسه، وعند توفر `solc` و`anvil`
و`cast` و`jq` تنشر Solidity مولدًا إلى Anvil محلي. تقارن status وstorage وعدد
تعليمات trace. ويشغل corpus raw-bytecode مستقل ALU قبل Fusaka ونسخ calldata/
memory و`MCOPY` المتداخل وKeccak وreturn data في EVM الأصلي لـAnvil.

تحفظ اختبارات Low/Med ‏execution lanes لكامل stack الحساسة للـpath وهوية lane في
phi؛ ويكون نفاد أي budget، ومنها `MaxAbstractInstructionTransfers`، hard error.
لا يرفض strict opcode مجهولة أوfork-inactive إلا على lane ثبت أنها `Reachable`،
ولا تنتج `MayReachable` حقائق مؤكدة. يقيد HighIR ‏selector/receive/fallback بالـroot
lane والـterminals الناجحة. لا يمثل selector مشترك دليل standard مستقلًا؛ ولا
تُختار variant وreturn list إلا من `KnownFunctionVariantInfo` الخاصة بالـstandard
وبعد اتفاق كل terminals الناجحة على return shape الدقيقة.

يجري interpreter ‏typed stack preflight قبل أي أثر خاص بالـopcode. يعرّف
`EVMForkSemantics.def` البايت `0x44` بأنه `DIFFICULTY` قبل Paris و`PREVRANDAO`
بدءًا من Paris. تعيد `REVERT` وfaults وstep limit ونفاد الموارد حالة transaction
إلى snapshot. يكون فشل allocation من النوع
`ExecutionFaultKind::ResourceExhausted`؛ وإذا تعذر حتى snapshot الدخول تكون
`HasPersistentStateSnapshot` بالقيمة false ولا يمكن commit للنتيجة.

### اختبارات انحدار حدود EVM العامة وميزانياتها

تعبث اختبارات الـAPI العامة كلًا على حدة بالـ
`Code`/`Fork`/`Instructions`/`JumpDestinations` القانونية وبكل table وrange وID
وlane وedge reference في LowIR. يجب أن يعيد `execute` ‏`llvm::Error` قبل lookup
التعليمات، وأن يرفض `lowerToMedIR` كامل LowIR الـmalformed أو المتجاوز للميزانية
قبل بناء index أو تخصيص output متناسب مع input. وتفرض tests في `lowerToMedIR`
ترتيب validation: options ثم resources ثم structure، وبعدها مقارنة field-by-field
عبر `canonical decode replay` وقبل `lowerCanonicalLowToMedIR`. يعيد public HighIR
recovery التحقق من LowIR/MedIR الخارجيين؛ ولا يستخدم
`lowerCanonicalLowToMedIR` و`recoverCanonicalHighIR` على IR القانوني الخاص إلا
`analyze`، بلا replay عودي أو مكرر ومع إبقاء HighIR option/resource budgets.
ثم تختبر حالات الـinterpreter
الحد الدقيق و+1 لكل حدود `EVMInterpreterLimits.def`: يحتفظ `MaxSteps` بـ
`StepLimit` المخصص، بينما يعيد نفاد `MaxMemoryBytes` أو `MaxTraceEntries` أو
`MaxLogEntries` أو aggregate ‏`MaxLogDataBytes` أو runtime
`MaxPersistentStateEntries` القيمة `ResourceExhausted` مع rollback لتأثيرات
المعاملة. يعد تجاوز aggregate أولي `MaxHostReturnDataBytes` أو persistent state
خطأ API. كما تعد مجاوزة `MaxCalldataBytes` أو aggregate
`MaxHostEnvironmentEntries` عبر `BlockHashes` و`Balances` و`CodeHashes` و
`ExternalCode` و`BlobHashes`، أو aggregate `MaxExternalCodeBytes`، خطأ API.
يرفضها `const execute preflight` قبل نسخ environment أو snapshot أو result. وتغطي
الاختبارات views ‏return-data من `ArrayRef` وlookup ‏`lower_bound`
في الجدول المرتب بلا نسخ buffer أو PC map.

تغطي اختبارات LowIR المنفصلة عند الحد الدقيق ميزانيتي diagnostic الـaggregate
`MaxLowDiagnostics` و`MaxLowDiagnosticBytes`: يحاسب linear decode وبناء CFG العدد
الدقيق والبايتات النهائية مسبقًا ويرفضان الصفر.
تغطي اختبارات أمان HighIR مجال `Any/Exact/Excluded` المرتب لكل lane، ومطابقة/
استبعاد equality، ومطابقة false edge وmismatch ‏true edge في
`XOR(selector, constant)` الخام، وتنقيح zero word/calldata size/call value،
والإغلاق عند unknown condition. كما تختبر الحدود الدقيقة و-1 في
`EVMAnalysisLimits.def`
`MaxHighDispatchCandidates` والـaggregate `MaxHighRecoveredArguments` و
`MaxHighDiagnostics` و`MaxHighDiagnosticBytes` و`MaxHighReferenceVisits` و
`MaxHighMemoryTransferCells` و`MaxHighMemoryValueVisits` من
`EVMAnalysisLimits.def`. ويجب أن يحاسب كل output diagnostic، بما فيه malformed
diagnostic الثابت، العدد والبايتات النهائية قبل allocation.
وتُختبر ميزانيتا diagnostic في LowIR وHighIR كل على حدة، ويجب أن تحاسب root CFG
region الافتراضية `MaxHighRegionBlockReferences` قبل reserve أو نسخ block PC.
تغطي regressions نطاق function كلا back-jump عبر `EQ` و`raw XOR` إلى dispatcher
مشترك، وتثبت أن function أخرى لا تلوث `arguments` أو`mutability` أو
`return shape` أو`region`، مع إبقاء shared bodies وtail calls قابلة للوصول.
تختبر نتائج CALL/CREATE الخارجية كـhost outcomes غير حتمية على حافتي CFG الدقيقتين،
فتبقى استعادة fallback في ERC-1167. ويظل selector condition غير المقروء Unknown ولا
يمكنه اختلاق facts لـfallback أو function.

تستمد اختبارات CFG ‏`InvalidJumpDestination` من `EVMLowFaultKinds.def` لحالة
`end-of-code JUMPI`: true المؤكد إلى هدف غير صالح definite fault بلا successful
tail، وfalse المؤكد ناجح، وunknown يحتفظ فقط بمسار false محتمل النجاح من دون وسم
lane كاملة كـdefinite fault.

تطبق اختبارات ABI حدود grammar من `EVMABIParserLimits.def` وحدود cardinality/text
للجداول العامة من `EVMABITableLimits.def` عند الحد الدقيق و+1. كما ترفض
kind/standard/evidence enums غير الصالحة وmetadata غير المتطابقة وsignature/return
list غير القانونية وshared selector الموسوم independent خطأً وvariant المعلق أو
المكرر وevent-topic ‏`APInt` غير بعرض word، قبل indexed selector أو sorted topic
lookup.

يفرض `NeverDEVMOpcodeTests` بنية metadata أيضًا: تدور كل opcode مخصصة بين encoding
وtyped value، وتُختبر حدود العائلات وaliases للـhardfork، وتبقى maxima لعقد stack
وhost arguments مشتقة بدل تكرارها في backends.

### واجهات Solana SBF الخلفية التفاضلية

تتحقق اختبارات بيانات SBF الوصفية من كل ميزة مرتبطة بالإصدار، وحدود تصادم opcodes، وhash ‏Murmur3 لـ syscall، وrelocations، وثوابت ELF machine والسجلات وعناوين VM. تولّد fixtures الخاصة بالـ loader، من دون ثنائيات مضمّنة، تخطيطات sections القديمة لـ v0-v2 وتخطيطات v3/v4 الصارمة الخالية من sections والمعتمدة على program headers.

يفحص `NeverDSBFISAConformanceTests` كل byte encoding في كل version من v0 إلى
v4 مقابل typed manifest مدقّق بصورة مستقلة. ثم يقارن
`NeverDSBFExternalOracleTests` قرارات activation وboundary مع official Anza
process مبني بصورة منفصلة. ويعيّن `NeverDSBFUpstreamConformanceTests` نتيجة
صريحة لكل ملفات ELF الثلاثة والعشرين عند Anza revision المثبتة.

ينفّذ `NeverDSBFSemanticTests` بايتات التعليمات المتحقق منها مباشرة ولا يستهلك MedIR، لذلك لا يمكن لتغيير IR المطبّع أو إفساده أن يجعل oracle المصدر يتفق مصادفةً مع backend. ويغطي دلالات v2 غير الرتيبة، والذاكرة، وsyscalls، وإطارات الاستدعاء الداخلية، وfaults، وtraces، وحدود الموارد. يجري التحقق من وحدات LLVM؛ ويُجمّع C المولد مع اعتبار warnings أخطاء، وRust مع `-D warnings`. تمر اختبارات API العامة عبر جميع مراحل IR والتفكيك وCFG والبيانات الوصفية وLLVM وC وRust بدءًا من ملف SBF ELF صارم مولد.

## أهداف بأمر واحد

تبني الأهداف المخصصة تبعياتها ثم تشغّل CTest بتوازٍ مشتق من CPU المضيف:

| هدف CMake | الاختيار |
|-----------|----------|
| `check-neverd` | كل الاختبارات المسجلة |
| `check-neverd-semantic` | `NeverDSemanticTests` فقط |
| `check-neverd-sbf` | جميع أهداف/حالات `NeverDSBF*Tests` |
| `check-neverd-patch-full` | `NeverDPatchFullTests` فقط |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests` فقط |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests` فقط |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests` فقط |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

لا يملك `NeverDIndCallXformTests` و`NeverDAvxUpperXformTests` حاليًا هدف راحة
`check-neverd-*`. ابنِهما واخترهما بالوسم كما أدناه. كذلك لا يتضمن
`check-neverd-semantic` ثنائيات التحويل أو patch-full المنفصلة؛ استخدم
`check-neverd` للتجميع الكامل.

## سير CTest التزايدي

ابنِ الملف التنفيذي المالك أولًا ثم اختر وسمه. يتجنب ذلك إعادة ربط أهداف
دلالية كبيرة غير مرتبطة.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# جميع أهداف/حالات EVM المحددة
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# جميع أهداف/حالات Solana SBF المحددة
cmake --build build-release --target check-neverd-sbf --parallel 4
```

استخدم اسم CTest مشتقًا من GoogleTest لانحدار واحد:

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

محددات مفيدة:

| الأمر | الغرض |
|-------|-------|
| `ctest --test-dir build-release -N` | سرد الحالات المكتشفة دون تشغيلها |
| `ctest --test-dir build-release -L '<regex>'` | اختيار وسم ثنائي اختبار |
| `ctest --test-dir build-release -R '<regex>'` | اختيار أسماء الحالات |
| `ctest --test-dir build-release --output-on-failure` | عرض التشخيص عند الفشل فقط |
| `ctest --test-dir build-release --stop-on-failure` | التوقف بعد أول فشل |
| `ctest --test-dir build-release --parallel 4` | تشغيل أربع حالات بالتوازي كحد أقصى |

يستخدم اكتشاف GoogleTest ‏`DISCOVERY_MODE PRE_TEST`، لذلك يجب أن يوجد ثنائي
الاختبار المطابق قبل تعداد CTest. تُعرّف مهلات الحالة والاكتشاف المنفصل في
`cmake/AddNeverD.cmake`، ولا تُوسّع إلا لمجموعات ذات حالات ثقيلة مقاسة.

## أي اختبارات يجب أن تتغير مع الشفرة؟

| منطقة التغيير | ابدأ بـ | ثم فكّر في |
|---------------|---------|------------|
| lifter العمارة أو decode | الحالة المسماة في `NeverDLiftTests` | دورة دلالية لـ ISA المطابقة |
| LowIR CFG واكتشاف الدوال وجداول القفز | حالات lift ‏CFG/switch | `NeverDSwitchXformTests` أو `NeverDCFGLoopXformTests` أو `NeverDTwoTableXformTests` |
| MedIR وABI والأعلام والأنواع وSSA | حالات lift ‏MedIR/أعراف الاستدعاء | حالات `NeverDSemanticTests` العابرة لـ ISA |
| HighIR أو C المنظمة | حالات HighIR/decompile | `NeverDCFGLoopXformTests` وفحوص تجميع C المولدة |
| loader ‏PE/ELF/Mach-O أو relocation الإدخال | fixture الصيغة المطابقة في `unittests/lift` | اختبار تحميل/إعادة تجميع كل المراحل للخلية |
| Rewrite codegen أو relocation الإخراج | حالات `RewriteCodegenRTTests` | `NeverDPatchFullTests` وfixture ‏patch مرتبطة عند توفرها |
| تحويل LLVM IR يستخدمه patch | ثنائي التحويل المحدد | شبكة pass المركبة لـ `NeverDPatchFullTests` |
| C API أو CLI | اختبار SDK/query مباشر و`unittests/semantic/CLIEndToEndTests.cpp` | مجموعة pipeline/صيغة ذات الصلة |
| ‏EVM loader أو opcode أو IR أو backend | أصغر هدف مالك من `NeverDEVM*Tests` | جميع أهداف EVM مع فحوص تجميع C/Solidity المولدين |
| ‏SBF loader أو ISA أو IR أو backend | أصغر هدف مالك من `NeverDSBF*Tests` | جميع أهداف SBF مع فحوص تجميع C/Rust المولدين |
| تعرف libc | `NeverDLibCTests` | حالات call/ABI دلالية إذا تغير السلوك |
| تدقيق عمر الكومة أو صيد فيضان النسخ | `NeverDSafetyTests` | الخلايا الست كلها في `NeverDSafetyIntegrationTests` |
| تنفيذ العمليات أو quoting | `NeverDTestProcessTests` | حالة CLI/دلالية متأثرة على كل مضيف مدعوم |

يجب أن تعبر الاختبارات عن العقد عند أدنى حد مستقر. يفيد اختبار شكل LowIR في
نسب السلوك إلى lifter؛ وتلزم دورة دلالية إذا كان لشكلين IR معقولين سلوك مختلف.
تجنب golden dump لدوال كاملة عندما يكفي assertion صغير على opcode أو CFG أو
حالة ملحوظة.

## العلاقة مع CI

تبني CI ‏Release مع الاختبارات على Linux وmacOS وWindows، ثم تدقق المخزون
المكتشف قبل تطبيق استثناءات الوسوم الخاصة بالمنصة. تُعرّف الملفات في
`.github/workflows/ci.yml` و`scripts/audit_ci_test_inventory.py`. يجب أن يضم كل
مضيف في المصفوفة `NeverDSafetyTests` و`NeverDSafetyIntegrationTests`؛ وتقرأ كل
عملية تشغيل fixtures نفسها المثبتة لـ PE وELF وMach-O على x86-64 وAArch64.
ولأن لا shard واحدًا من المصفوفة يمثل كل المجموعات المكلفة، يبقى
`check-neverd` المحلي أوضح إشارة كاملة قبل الدمج عندما تملك الآلة كل الأدوات
العابرة اللازمة.

## ملف مطابقة وتعقيم Solana SBF الحالي

تحل هذه القائمة الحالية محل قائمة SBF المختصرة أعلاه. تحتاج اختبارات source
differential إلى `rustc` بالإضافة إلى clang؛ تخطي compiler يعني coverage ناقصة.
يشمل التجميع الكامل `NeverDSBFProgramImageTests` و
`NeverDSBFMalformedCorpusTests` و`NeverDSBFISAConformanceTests` و
`NeverDSBFUpstreamConformanceTests` و`NeverDSBFLLVMDifferentialTests` و
`NeverDSBFSourceDifferentialTests` مع targets metadata/loader/analyzer/semantic/
emitter/integration. يسجل profile المتكامل الأهداف المسماة ونتائجها ولا يثبت
عدداً تجميعياً سريع التغيّر.

يجب بناء profile sanitizer في `build-sbf-asan-ubsan` منفصلًا. تحتوي package
الجاهزة المثبتة ذات الإصدار المحدد الآن fork-only header المطلوب، لذلك يعمل
integration في profile نفسه بإعدادات ASan وUBSan بنمط fail-fast.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

### لقطة أدلة SBF المثبتة (2026-08-24)

تثبت البوابة Anza `sbpf` عند
`2510663bb8d894e8e3094be351e4bb4b604f1f84` وAgave عند
`ef210d67f2fabeee1730498188fa78854260c679` وSolana SDK عند
`122f32e571ce39face4beffaccea733e37c207fd`. يمر ELF manifest الرسمي 23/23؛
ويقارن `NeverDSBFExternalOracleTests` عدد 1,411 من حالات opcode/boundary عبر
`SBFOfficialOracleProtocol.def` و`SBFOfficialVerifierCases.def` و`SBFOfficialExecutionConstants.def`.
`SBFOfficialELFMutations.def` هو عقد malformed ELF الجدولي، ولا يثبت المستند
عدداً إجمالياً متغيراً.
وبشكل مستقل يشغّل `41-case strict ELF differential` كامل مصفوفة strict-v3 عبر
`verify-elf-batch` الرسمي وNeverD؛ ولا تدخل الحالات الـ41 في مجموع 1,411.
ويصادق `NeverDSBFAgaveConformanceTests` على corpus ‏Firedancer test-vectors عند
`68bb4af40235562e8852fa23d5727e49c2a0b862` ويطابق كل fixtures الـloader البالغ
عددها 1,955 `sol_compat_elf_loader_v1` (قبول 1,399 ورفض 556)، ويطابق لكل ELF مقبول
`entry_pc` و`text_off` و`text_cnt` و`rodata_hash` و`calldests_hash`. ولا تشغّل هذه gate
الـinstruction verifier اللاحق.

مصفوفة التنفيذ الرسمية الإضافية مستقلة: تضم بالضبط 508 حالات فعالة من
`(Version,Opcode)` و58 حالة boundary، أي 566 حالة تنفيذ دقيقة. وهي لا تستبدل
ولا تدخل ضمن 1,411 من اختبارات verifier أو `41-case strict ELF differential`.
تستخدم Linux Release CI الخيارات `--print-pinned-revision` و
`--print-test-vectors-revision` و`--print-toolchain`، وتصدر
`NEVERD_SBPF_ORACLE` و`NEVERD_AGAVE_CONFORMANCE_ROOT`، فتكون البوابتان
الخارجيتان إلزاميتين؛ محلياً، غياب env الصريح للـoracle/corpus يسمح باكتشاف
الحالات ثم skip.

تجعل rows باسم `SBF_RUNTIME_VERSION` نطاق
`RuntimeVersionPolicy::ChainProfile` تاريخياً حسب cluster/slot: ينتقل maximum
ISA من V0 إلى V1 ثم V2 ثم V3 مع تفعيل feature accounts الرسمية، والحالي V3.
أما v4 الصريح فيستخدم `RuntimeVersionPolicy::UpstreamToolchain` للتحليل offline. حد 10 MiB
الحالي هو بالضبط `10'485'760` byte، و65,536 provenance/test تاريخي غير منفذ.
يثبت `SBFFaultCodes.def` قيم execution fault؛ أما `SBFSourceStatuses.def` فيثبت
generated-source ABI المستقل.

تحرس fixtures بحجم 10,000 خصائص worklist/function ownership/multi-latch من دون
تثبيت زمن جهاز. وتتيح rows الخاصة بالـcluster/account/slot تنفيذ
`RPC activation audit` مع بقاء الاختبارات العادية deterministic وoffline.
