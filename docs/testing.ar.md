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
| `unittests/lift` | `NeverDLiftTests` | أشكال LowIR لـ decoder/lifter، ومراحل IR، وloader، وrelocation، وfixtures الصيغ، وإعادة التجميع، ومسارات patch الممثلة |
| معظم ملفات `unittests/semantic` | `NeverDSemanticTests` | دلالات تفاضلية للتعليمات وABI والتحكم وتعابير C وlift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests` و`NeverDEVMBytecodeTests` و`NeverDEVMLoaderTests` و`NeverDEVMAnalyzerTests` و`NeverDEVMSemanticTests` و`NeverDEVMEmitterTests` و`NeverDEVMIntegrationTests` | metadata للـhardfork وتطبيع الإدخال وCFG/SSA والاستعادة ودلالات interpreter وتنفيذ LLVM/C/Solidity التفاضلي وتوجيه API العامة |
| `unittests/sbf` | `NeverDSBFMetadataTests` و`NeverDSBFLoaderTests` و`NeverDSBFAnalyzerTests` و`NeverDSBFSemanticTests` و`NeverDSBFLLVMEmitterTests` و`NeverDSBFEmitterTests` و`NeverDSBFIntegrationTests` | بيانات v0-v4 الوصفية وتخطيطات ELF، والتحقق الصارم، وCFG/الاستعادة، والتنفيذ الخام المستقل، والتحقق من LLVM، وتجميع C/Rust، وتوجيه API العامة |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | تكافؤ إعادة الكتابة/التشويش عبر أربع ISA وثلاث صيغ كائنات |
| ملفات التحويل المحددة في `unittests/semantic` | `NeverDSwitchXformTests` و`NeverDIndCallXformTests` و`NeverDCFGLoopXformTests` و`NeverDTwoTableXformTests` و`NeverDAvxUpperXformTests` | مجسات سريعة الربط منفصلة عن الثنائي الدلالي الكبير |

مصادر التسجيل الموثوقة هي
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt) و
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) و
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt) و
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) و
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt).

ينفذ تدقيق opcodes ‏EVM في كل تشغيل `git fetch` سطحيًا للـremote `HEAD` من
[مستودع go-ethereum الرسمي](https://github.com/ethereum/go-ethereum)، ثم يبلغ
عن commit الدقيق الذي دُقق. يعيد استخدام bare cache المتجاهلة في
`build/evm-opcode-audit/go-ethereum.git`، لكنه يحدّثها قبل قراءة inventory المغلق
للـopcodes وتعيينات bytes:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

يشغل CI التدقيق الحي نفسه عند كل push وpull request وعند التشغيل اليدوي ومرة
يوميًا، لاكتشاف upstream drift حتى إن لم يتغير NeverD. للاختبار offline أو إعادة
إنتاج نسخة تاريخية، اختر checkout موجودة صراحة:

```bash
python3 scripts/audit_evm_opcode_metadata.py \
  --geth-root /path/to/go-ethereum
```

لا يسمح التدقيق إلا بالاستثناءات المسماة في `EVMUpstreamOpcodePolicy.def`؛ ويفشل
الأمر إذا لم يكن opcode من upstream ممثلًا أو مراجعًا صراحة. للـparser وتشخيص
الانحراف تغطية unit مستقلة في CI، ويمكن تشغيلها عبر:

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

لتغييرات control flow في EVM، شغّل أولًا عقد fixed point وheight domain:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

تغطي هذه الحالات returns داخلية عابرة للـblocks وmerges محدودة متعددة الأهداف
وتقارب loops وترتيب edges الحتمي وstack heights المعتمدة على المسار وwidening
المحدود وCartesian over-approximation الناتجة عن correlation وunknown jumps
والأهداف غير الصالحة الدقيقة وstack faults في الوضعين strict وrelaxed. بعدها
شغّل ثنائيات EVM السبعة كلها مع تدقيق metadata upstream؛ فتغييرات CFG قد تؤثر
في emitter وintegration حتى لو كان شكل analyzer المحلي صحيحًا.

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

يفرض `NeverDEVMOpcodeTests` بنية metadata أيضًا: تدور كل 150 opcode بين encoding
وtyped value، وتُختبر حدود العائلات وaliases للـhardfork، وتبقى maxima لعقد stack
وhost arguments مشتقة بدل تكرارها في backends.

### واجهات Solana SBF الخلفية التفاضلية

تتحقق اختبارات بيانات SBF الوصفية من كل ميزة مرتبطة بالإصدار، وحدود تصادم opcodes، وhash ‏Murmur3 لـ syscall، وrelocations، وثوابت ELF machine والسجلات وعناوين VM. تولّد fixtures الخاصة بالـ loader، من دون ثنائيات مضمّنة، تخطيطات sections القديمة لـ v0-v2 وتخطيطات v3/v4 الصارمة الخالية من sections والمعتمدة على program headers.

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
  NeverDEVMAnalyzerTests NeverDEVMSemanticTests NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# جميع أهداف/حالات Solana SBF المحددة
cmake --build build-release --target \
  NeverDSBFMetadataTests NeverDSBFLoaderTests NeverDSBFAnalyzerTests \
  NeverDSBFSemanticTests NeverDSBFLLVMEmitterTests NeverDSBFEmitterTests \
  NeverDSBFIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'SBF' --output-on-failure --parallel 4
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
| تنفيذ العمليات أو quoting | `NeverDTestProcessTests` | حالة CLI/دلالية متأثرة على كل مضيف مدعوم |

يجب أن تعبر الاختبارات عن العقد عند أدنى حد مستقر. يفيد اختبار شكل LowIR في
نسب السلوك إلى lifter؛ وتلزم دورة دلالية إذا كان لشكلين IR معقولين سلوك مختلف.
تجنب golden dump لدوال كاملة عندما يكفي assertion صغير على opcode أو CFG أو
حالة ملحوظة.

## العلاقة مع CI

تبني CI ‏Release مع الاختبارات على Linux وmacOS وWindows، ثم تدقق المخزون
المكتشف قبل تطبيق استثناءات الوسوم الخاصة بالمنصة. تُعرّف الملفات في
`.github/workflows/ci.yml` و`scripts/audit_ci_test_inventory.py`. ولأن لا shard
واحدًا من المصفوفة يمثل كل المجموعات المكلفة، يبقى `check-neverd` المحلي أوضح
إشارة كاملة قبل الدمج عندما تملك الآلة كل الأدوات العابرة اللازمة.

## ملف مطابقة وتعقيم Solana SBF الحالي

تحل هذه القائمة الحالية محل قائمة SBF المختصرة أعلاه. تحتاج اختبارات source
differential إلى `rustc` بالإضافة إلى clang؛ تخطي compiler يعني coverage ناقصة.
يشمل التجميع الكامل `NeverDSBFProgramImageTests` و
`NeverDSBFMalformedCorpusTests` و`NeverDSBFISAConformanceTests` و
`NeverDSBFUpstreamConformanceTests` و`NeverDSBFLLVMDifferentialTests` و
`NeverDSBFSourceDifferentialTests` مع targets metadata/loader/analyzer/semantic/
emitter/integration. يمر profile المتكامل 107/107 حالة في 13 binaries.

يجب بناء profile sanitizer في `build-sbf-asan-ubsan` منفصلًا. يمر 101/101 حالة
core في 12 binaries بلا ASan أو UBSan report؛ ويظل integration في build LLVM
المتكامل لأن package الجاهزة لا تحتوي fork-only header المطلوب.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```
