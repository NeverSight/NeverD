**اللغات**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# فك تجميع EVM

[← فهرس التوثيق](README.ar.md)

يحمّل NeverD بايت كود Ethereum Virtual Machine التقليدي، ويبني LowIR خاصًا
بعرض 256 بت وMedIR بنمط SSA للمكدس وHighIR مستعادًا، ثم يخرج LLVM IR أو C23 أو
Solidity. التحليل الصارم هو الافتراضي، لكن EVM التقليدية لا تتحقق من كل بايت في
الصورة بوصفه opcode: لا يُرفض إلا execution lane مؤكدة `Reachable` تصل إلى
opcode غير مخصص أو غير نشط في fork، وعند قيمة PC الدقيقة. لا تتحول البايتات
الميتة ولا مرشحات CFG التي ليست سوى `MayReachable` إلى أخطاء strict.

مخرجا Solidity وC إعادة بناء دلالية؛ يحافظان على ترتيب opcodes والحساب 256 بت
وفحوص المكدس وتدفق التحكم المتحقق منه، من دون ادعاء استعادة المصدر أو الأسماء أو
الأنواع الأصلية.

## بداية سريعة

```bash
# LLVM IR متحقق منه بقيم i256/i512.
./build/bin/neverd lift contract.evm -o contract.ll

# اعرض كل مرحلة من تحليل EVM.
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# أخرج C23 أو Solidity.
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# اختر مجموعة تاريخية أو احتفظ بالـ opcodes المجهولة كعقد خطأ للفحص.
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

تقبل `disasm` و`cfg` واستعلامات Low/Med/High/LLVM في C API مدخلات EVM أيضًا.
تُرفض إعادة كتابة EVM الثنائية صراحة، وتبقى `patch` للثنائيات الأصلية.

## المدخلات المقبولة

| المدخل | التعرف والتطبيع |
|--------|-----------------|
| بايتات خام | `.raw` أو `.evmraw` أو محتوى ثنائي بامتداد EVM صريح |
| نص سداسي | بادئة `0x` اختيارية ومسافات ASCII حرة وامتدادات `.evm` و`.hex` و`.bin` و`.bytecode`؛ ويُكتشف hex بلا امتداد بعد التحقق |
| ناتج مترجم | ملف `.json` فيه `deployedBytecode` أو `runtimeBytecode` أو `bytecode` في الجذر أو تحت `evm`؛ ويدعم JSON القياسي لـ solc عند `contracts → file → contract → evm` |

يفضّل بايت كود runtime/deployed على creation. عند وجود creation code فقط، يتعرف
NeverD على أغلفة constructor ثابتة ومحدودة من `CODECOPY`/`RETURN` ويستخرج مقطع
runtime المنسوخ. ويستخدم مرور constructor مفكك التعليمة الواحدة نفسه الذي
يستخدمه المفكك الحقيقي، وضمن hardfork قيد التحليل، فلا يستطيع بايت يكون بيانات
في fork وopcode في آخر أن يزحزح الحد. يكون حقل `deployedBytecode` أو
`runtimeBytecode` الموجود هو المرجع: تُقبل القيمة الصريحة `0x` كـruntime فارغ
يتوقف طبيعيًا، وتمنع عمدًا fallback إلى creation bytecode. يسمح غياب الحقل
بالانتقال إلى المرشح التالي، أما hex الغائب أو المؤلف من whitespace فقط بلا
بادئة صريحة فيُرفض. ويمكن أن يكون raw input الصريح فارغًا أيضًا.

### ذيول المترجمات

يجدول `EVMMetadataFields.def` صيغتَي الذيل معًا. يكتب Solidity خريطة CBOR يَعُدّ
بايتاها الأخيران الخريطة وحدها، بينما يكتب `vyper` مصفوفة CBOR تنتهي بتلك
الخريطة ويَعُدّ بايتاها الأخيران الذيل كله بما فيهما. وقراءة إحدى الصيغتين على
أنها الأخرى لا تفشل بصوت عالٍ، بل تقع على بعد بايتين وتحذف بايتين من كود حقيقي؛
لذلك تُجرَّب الصيغتان معًا، ويُترك المدخل الذي لا يطابق أيًّا منهما كما هو.

ويُقرأ الذيل مرتين: مرة على المدخل كما ورد، ومرة على كود runtime المتبقي بعد فك
غلاف النشر. فقد نقل Vyper ذيله إلى initcode وترك كود runtime بلا ذيل، لذا فإن
قارئًا لا ينظر إلا بعد فك الغلاف يبلّغ عن بناء مجهول لعقد سمّى نفسه. كما يذكر ذيل
التسلسل طول كود runtime وأطوال أقسام البيانات وطول immutables، وهي قيم تحدّ
الكود المُعاد دون تنفيذ constructor.

### حاويات ليست تعليمات

يصنّف `EVMBytecodeContainers.def` المدخل قبل أي فك. ومنذ أن جعل EIP-3541 البايت
`0xEF` غير قابل للنشر، صار `0xEF` في المقدمة وعدًا بأن البايتات ليست تعليمات:

| الحاوية | العلامة | المعالجة |
|---------|---------|----------|
| legacy | — | تُفك بوصفها تعليمات |
| delegation (`eip-7702`) | `0xef0100` وطول 23 بايت بالضبط | يُبلَّغ عن الحساب الهدف ويتوقف التحليل |
| eof (`eip-3540`) | `0xef00` | مرفوضة؛ لم يفعّلها أي fork |

بايتات مؤشر التفويض العشرون عنوان لا كود. وفكّها يقرأ العنوان بوصفه opcodes
وينتج رسم تدفق تحكم لحساب، لذا يبلّغ `info` عن الهدف ويرفض التحليل مع ذكر السبب.
ويميّز الرفض بين الحالتين: قبل Pectra لم تكن العلامة مخصصة بعد، ومن Pectra
فصاعدًا يكون كود runtime الخاص بالهدف غائبًا ببساطة. أما علامة بأي طول آخر فهي
مدخل مشوَّه لا صيغة أخرى من الحاوية، وتبقى تعليمات كي يستطيع المفكك تسمية البايت
الذي عجز عن قراءته.

ينتج hex التالف وعدد الخانات الفردي وlinker placeholders غير المحلولة وartefacts
متعددة العقود الملتبسة وحدود metadata غير الصالحة وhex الغائب أو الفارغ أخطاء
قابلة للتصرف. أما raw input الفارغ صراحة أو runtime بالقيمة `0x` فيظل برنامجًا
فارغًا صالحًا. يختار `BytecodeLoadOptions::ArtifactContract` الصيغة `Contract` أو
`path/File.sol:Contract`. وإذا تكرر الاسم في ملفات مصدر مختلفة يُرفض الاسم غير
المؤهل كي لا يختار ترتيب JSON عقدًا خاطئًا بصمت.

EVM مسجل في core loader registry وليس مخفيًا خلف backend plugin؛ لذلك تتلقى CLI
وC API والمفكك وCFG واستعلامات IR الصورة المطبعة والخيارات نفسها.

## Hardforks وopcodes

تُغطى مجموعة opcodes التقليدية النهائية من Frontier حتى Fusaka، ومنها `PUSH0`
وtransient storage و`MCOPY` وblob opcodes و`CLZ`. ولا تتاح opcodes المجدولة لـ
Amsterdam إلا باختيار development fork صريح؛ ويبقى `latest` هو Fusaka.

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, amsterdam, bogota, latest
```

تُقبل الأسماء البديلة `dao` وصيغ underscore و`merge` و`prague` و`osaka`.
ويُحل `latest` و`osaka` حاليًا إلى revision القانوني `fusaka`.

يعني `latest` أحدث revision نهائي للشبكة الرئيسية نفذه NeverD، لا رأس تطوير
Ethereum. ترقية [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/) مقررة
للربع الرابع 2026؛ وتبقى تعليمات Review
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) و
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) لا تُفعّل إلا مع
`--evm-hardfork=amsterdam` (أو `bogota`) وتبقى خارج `latest` حتى التثبيت. في
EIP-8024 يُستهلك immediate صالح فقط، وتبقى القيمة غير الصالحة تعليمة تالية.

أزيل EOF في
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2).
EOFv1/EIP-7692 غير مجدول، ومقترح الحاوية
[EIP-3540](https://eips.ethereum.org/EIPS/eip-3540) في حالة Stagnant. أُرشف
مستودع `execution-spec-tests` القديم ونُقلت الاختبارات التي ما زالت تُصان إلى
[execution-specs](https://github.com/ethereum/execution-specs/tree/master/tests).
لذلك لا يعامل NeverD حاوية EOF التجريبية كسلوك mainnet نهائي.

لا يرفض strict mode بايتًا مجهولًا أو غير نشط في fork إلا عندما تثبت lane
مؤكدة `Reachable` أن التنفيذ يصل إليه. يحتفظ `--evm-relaxed` به كـfault prefix
معرّف نوعيًا وفي التشخيص، لكن backend يظل يفشل عند تنفيذه؛ ولا يتحول بصمت إلى NOP.

## بنية metadata بأسلوب LLVM

تتبع metadata اليدوية لـEVM نمط LLVM لملفات `.def` متعددة التضمين:

- `EVMOpcodes.def` هو مصدر الحقيقة الوحيد لكل opcode تقليدية نهائية ولكل opcode
  تطويرية opt-in. يجمع encoding وتغييرات pop/push الفعلية ونوع immediate وclass
  وactivation fork وprimary effect والوصول
  المستقل إلى EVM memory وsource state وcall-value وtermination في سجل واحد بلا
  قيم افتراضية صامتة.
- تحدد `EVMMemoryAccesses.def` و`EVMStateAccesses.def` و
  `EVMCallValueAccesses.def` مجالات مغلقة ذات types. يمكن أن يكون `CALL` external
  call وmemory read/write معًا، و`EXTCODECOPY` context read وmemory write. يستخدم
  state شبكة `None/Read/Write/Unknown`. أما payability فقيد مستقل: يؤدي
  `CALLVALUE` عادة إلى `payable`، ولا تُهمل القراءة إلا إذا أثبت analyzer الحارس
  القانوني `ISZERO(CALLVALUE)` وانتهاء الفرع nonzero بـ`REVERT`.
- يعرّف `EVMImmediateKinds.def` بيانات PUSH ذات العرض الثابت وencoding الـsingle/pair
  الشرطي في EIP-8024، ويملك `EVMDecodeStatuses.def` المفردات المستقرة التي يكشفها
  LowIR وdisassembly. يسجل `EVMUpstreamOpcodePolicy.def` alias تسمية go-ethereum
  والاستثناءات التاريخية/EOF غير المجدولة المقصودة. أما الملف المستقل
  `EVMUpstreamSemanticsPolicy.def` فيربط forks بـ`params.Rules`، ويسمي استثناءات
  base-stack preflight ويصنف عائلات dynamic-immediate. يرفض التدقيق أي drift في
  byte أوactivation أو`base_min_stack` أو`net_stack_delta` وأي ثابت upstream جديد
  غير مراجع.
- تولد `EVMHardforks.def` و`EVMEffects.def` و`EVMExitStatuses.def` و
  `OutputLanguages.def` enums مرتبة وparsers وأسماء وخيارات CLI وقيم C ABI.
  وتعلن `EVMAnalysisLimits.def` و`EVMInterpreterLimits.def` و
  `EVMABIParserLimits.def` و`EVMABITableLimits.def` حدود التحليل والـinterpreter
  والـparser والجداول العامة كلٌ في مرحلته. يملك `EVMConstants.h` عروض البروتوكول
  المشتركة والأسماء الداخلية الثابتة، ويولّد من `EVMAnalysisLimits.def` قيم
  التحليل الافتراضية وأسماء خيارات التشخيص؛ أما headers الـinterpreter والـABI
  فتولّد الحدود المعلنة في جداولها الخاصة.
- `EVMCalls.def` يصف التعليمات الأربع التي تستدعي برنامجًا آخر، وشبكة الأماكن
  التي يمكن أن يأتي منها عنوان المستدعى. علَم واحد في كل سجل، وهو ما إذا كان
  معامل القيمة يقع بين المستدعى ونافذة الوسائط، يشتق كل موضع معامل لاحق، ويُتحقق
  من الجدول أمام قاعدة بيانات الأوبكود حتى لا ينحرف الاشتقاق عن أعداد الـ pop
  المعلنة.
- `EVMPrecompiles.def` هو قاموس العناوين التي يجيب عندها البروتوكول بنفسه، مع
  الـ fork الذي حجز كلًا منها والمقترح الذي جدوله. ويُنسب `P256VERIFY` عند
  `0x100` إلى `eip-7951`، وهو المقترح النهائي الذي حجزه على الشبكة الرئيسية مع
  Fusaka؛ أما مقترح الـ rollup الذي جاءت منه واجهته فلم يجدوله قط. الغاز غائب
  عمدًا: كلفة الـ precompile دالة في مدخلاته وقد أُعيد تسعيرها دون أن يتغير
  العنوان أو العملية.
- يصف `EVMMetadataFields.def` و`EVMBytecodeContainers.def` ما هو المدخل قبل
  فكّه: صيغتَي ذيل المترجم، والحاويات التي ليست بايتاتها تعليمات أصلًا.
- `EVMRecoveredFacts.def` يملك تهجئة مفردات الحقائق المستعادة، فالاسم الذي يظهر
  في المخرجات يعيش في مكان واحد بدل `switch` قد يُنسى فيه عنصر جديد.
  يخزن `EVMKnownSignatures.def` spelling وselector القانونيين لكل function مرة
  واحدة، ثم يعلن memberships منفصلة لكل standard في
  `KnownFunctionVariantInfo` مع return lists ودور evidence من نوع
  independent/non-independent. لذلك تبقى signature المشتركة بين ERC-20 وERC-721
  مرشحًا قابلًا للاستدعاء مرة واحدة، لكنها لا تثبت أي standard وحدها ولا ترث
  return type لأول variant. وتحتفظ events وcustom errors بسجلات typed مستقلة.
- يملك `Semantics.h` scalar ALU evaluator مستقلًا عن target. يشترك interpreter
  وconstant folding في تطبيق `APInt` المتحقق، بينما تبقى lowerings الخاصة بـ
  LLVM/C/Solidity صريحة وfail-loud.

الـdecoder هو حد البايت الخام. تُفصل هوية opcode المخصص عن تنشيطه حسب fork:
يحفظ relaxed decode الاسم وintroduction fork وعرض immediate للـopcode غير النشط
مع semantic query محافظة تفشل. وهكذا لا يزيح immediate غير نشط حدود البايتات
التالية. تستخدم مراحل التحليل والتنفيذ والإخراج enum `Opcode` المولد واستعلامات
metadata؛ ولا يعود raw encoding إلا عند حدود ABI للتتبع وhost callback. مدخلات
`SWAP16` السبعة عشر وأقصى سبعة host arguments حدان منفصلان مشتقان compile-time.

لا يمكن default-construct لـ`OpcodeInfo` كسجل نصف صالح، واسمه
`llvm::StringLiteral`. يرفض compile-time validator encoding المكرر وproperties
المجهولة وعقود ALU غير الصالحة وتعارض effect/state وأخطاء عائلات
PUSH/DUP/SWAP/LOG وterminators وhost results. ولا تنشئ metadata مجهولة محافظة
إلا factory صريحة.

ملفات `.def` قواعد مكتوبة يدويًا مثل
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def)
في LLVM. أما `.inc` فمخصص لfragment مولد فعليًا، مثل خرج TableGen. تعيش السجلات
التصريحية الغنية في `.td` ثم يولد
[TableGen](https://llvm.org/docs/TableGen/ProgRef.html) ملفات `.inc`. لا يملك
NeverD حاليًا EVM TableGen step، لذا فإن `.inc` بلا generator مجرد مظهر زائد.
ويتبع C++ [معايير LLVM](https://llvm.org/docs/CodingStandards.html) وأنواع LLVM
ADT/string على الحدود وsemantic switches شاملة وfail-loud.

إضافة opcode تتطلب سجل `EVM_OPCODE` كاملًا وscalar semantics وbackend lowerings
صريحة واختبارات مركزة. وإضافة hardfork تتطلب سجل `EVM_HARDFORK` مرتبًا وaliases.
تتوسع typed API وlookup وvalidation وclassification وCLI بلا جداول موازية.

## نموذج التحليل

- **EVM LowIR** يحفظ PC وencoding وحالة immediate المعرّفة نوعيًا وoperands عمق
  المكدس بعد فكها (بما في ذلك right-zero padding لـPUSH وقاعدة الاستهلاك الشرطي في
  EIP-8024)، وblocks وpredecessor/successor edges وأهداف `JUMPDEST` المتحققة
  وreachability وstack-height domains. استعادة CFG هي fixed point حتمي على كامل
  البرنامج: تُمرَّر مجموعة محدودة ومقيدة من قيم 256-bit لكل stack slot، ويُحتفظ
  بمكدس abstract لكل ارتفاع concrete. لذلك تستطيع الثوابت المحمولة عبر blocks
  الـinternal-call والـreturn وstack shuffles و`PC`/`CODESIZE` وعمليات ALU scalar
  حل هدف قفز concrete واحد أو عدة أهداف. ويبقى الهدف المجهول فعلًا indirect edge
  صريحًا بدل تخمينه.

  على back-edge تُجرّد أي loop-carried slot متغيرة دلاليًا إلى `Top` كي تتقارب
  fixed point؛ وهذا abstraction للـrecurrence مستقل عن الموارد. تمثل
  `MaxAbstractValuesPerSlot` و`MaxStackHeightVariants` و
  `MaxAbstractInstructionTransfers` وحدود instructions وblocks وstate وvalues
  وstacks وlanes وedges وworklist budgets مسماة. الصفر أو النفاد hard error قبل الإدراج،
  وليس emergency widening أو بترًا صامتًا.

  يتعامل `EVMLowFaultKinds.def::InvalidJumpDestination` بحساسية للمسار عند
  `end-of-code JUMPI`: إذا كان الشرط true قطعًا والهدف غير صالح فلا يوجد successful
  tail ويُسجل fault مؤكد؛ وإذا كان false قطعًا ينجح. أما الشرط unknown فيحتفظ فقط
  بمسار false الذي قد ينجح، ولا يوسم lane كاملة خطأً كـdefinite fault.
- **EVM MedIR** يمثل كل stack value بقيمة SSA بعرض 256-bit، ويوصل كل merge phis
  قبل تشغيل sparse constant worklist حتمية. الـlattice الخاصة هي
  `Uninitialized` أو `Constant` دقيقة واحدة أو `Overdefined`: تنتشر الثوابت
  المتساوية عبر blocks ودورات phi المثبتة بمرساة، بينما لا تستطيع دورة متعارضة أو
  معتمدة على runtime اختلاق ثابت. تتحقق worklist من def-use IDs وتستخدم evaluator
  الـALU نفسه في `Semantics.h` الذي يستخدمه interpreter. ويحفظ MedIR أيضًا effect
  الأساسي مع EVM-memory ‏`none/read/write/readwrite` وstate على مستوى المصدر
  وcall-value كخصائص مستقلة. تحتفظ كل whole-stack lane في LowIR بـSSA execution
  lane مستقلة، وتسمي phi الـsource lane صراحة؛ ولا تُحاذى stacks غير المتوافقة
  باستخدام أقصى ارتفاع.
- **EVM HighIR** يستعيد Solidity dispatcher selectors وكلمات calldata وreturn
  المحتملة وmutability وconstant storage slots وحقائق LOG/event وrevert ومناطق
  function/CFG. يستعيد producer index متحقق منه وvalue walk تكرارية مع memoization
  الحقائق من typed MedIR operands لا من مسافة التعليمات: قد تعبر مقارنة selector
  blocks وphis، وتستخدم أي ترتيب لـoperands ‏`EQ`، وتحفظ mask مشتقًا بعرض 32-bit؛
  كما تستخدم argument offsets وstorage keys وevent topic0 وnon-payable/receive
  guards وأحجام return الدقيقة 32-byte مدخلاتها الدلالية. يحد رسم MedIR بنيويًا
  الـwalk التكرارية، وتعامل التعبيرات malformed أو mixed أو cyclic كـunknown.
  تُشخّص الأهداف المتعارضة للـselector نفسه وتُحذف. تبقى payability مستقلة عن
  state-access lattice، ويجبر reachable unresolved dynamic jump الاستعادة على
  `nonpayable` المحافظ. يتتبع dataflow الذاكرة الحساس للتدفق وعلى مستوى byte
  الكتابات ذات offset ثابت عبر blocks، ويركب overlap/kill ويبطل المعرفة عند كتابة
  unknown. ما ثبت حاليًا من payload هو selector وبايتات Panic المعروفة. يحفظ
  Solidity emitter الأنواع القانونية لمعاملات declaration ‏custom error معروفة،
  لكنه لا يدعي استعادة كل runtime argument value. وتظل الحقائق الأخرى مرشحات.

  يبدأ اكتشاف selectors من root lane وحدها ويتبع حواف عدم التطابق في dispatcher؛
  فلا يتحول اختبار شبيه بالـselector داخل handler إلى function عامة. ويُقيد receive
  وfallback بالجذر كذلك، ويتطلب كل منهما terminal ناجحة ومؤكدة reachability؛ فلا
  يثبتهما revert أوfault أوhandler غير payable لـempty calldata أوpath محتملة فقط.
  يؤدي استعمال calldata غير المتوافق إلى إسقاط المرشح القانوني، ولا يقدم selector
  مشترك دليل standard مستقلًا. لا يُختار standard وvariant إلا بselectors مستقلة
  متوافقة وكافية، أو بدليل قوي من topic/arity دقيقين أوstorage slot أوproxy. ولا
  تُخرج static return list إلا إذا اتفقت كل terminals الناجحة المؤكدة reachability
  على عدد بايتات ABI الدقيق؛ وتفشل transfers غير المحلولة والأشكال المتعارضة وأي
  mismatch بصورة مغلقة. ولا يُعد revert أوfault return ناجحًا.

  يضع HighIR budgets مستقلة للfunctions وزيارات lane/operation ومراجع blocks في
  regions وmemory requests/bytes وstate cells وتحديثات worklist. ولا يستهلك memory
  fixed point إلا execution lanes المؤكدة reachability، ويطبق meet بإجماع كل byte،
  ويعيد hard error عند نفاد budget بلا بتر للحقائق.

  كما يسجل HighIR النصف الصادر من الواجهة: كل `CALL` و`CALLCODE`
  و`DELEGATECALL` و`STATICCALL`، مع مصدر عنوان المستدعى، والعنوان المحجوز الذي
  يسميه عندما يحجزه الـ fork المحلَّل، والـ selector الذي يضعه الاستدعاء في مقدمة
  calldata المستدعى، والقيمة المحوَّلة عندما تكون ثابتة. أما `CREATE` و`CREATE2`
  فمستثنيان لأنهما ينفذان شيفرة لا عنوان لها بعد، فلا يوجد مستدعى لاستعادته.

  التوقيع الصادر المستعاد لا ينضم أبدًا إلى المعايير التي يستجيب لها البرنامج.
  إرسال `transfer(address,uint256)` يقول إن البرنامج يستخدم رمزًا، لا أنه رمز،
  والخلط بينهما سيجعل كل router وvault يظهر كـ ERC-20. ويُبلَّغ عن الاستدعاء
  المفوَّض إضافةً إلى ذلك كحقيقة proxy، لأنه العضو الوحيد في العائلة الذي تعمل
  شيفرة المستدعى فيه على تخزين هذا البرنامج نفسه.

  يخضع البحث عن precompile للـ fork قيد التحليل لا لأحدث fork موجود. فاستدعاء
  عنوان precompile يقدمه fork لاحق يصل إلى حساب بلا شيفرة، وينجح ولا يعيد شيئًا،
  لذا فتسميته ستُبلغ عن عملية لم يقم بها البرنامج قطعًا.
- **LLVM** يخرج state machine نظيفة للـverifier باسم
  `i32 @evm_execute(ptr)`، مع مكدس 1024 كلمة `i256` متحقق وintermediates `i512`
  وsigned division محروسة وshifts مشبعة و`BYTE`/`SIGNEXTEND`/`CLZ` دقيقة وswitches
  للقفز الديناميكي متحقق منها.

الـinterpreter الحتمي هو semantic oracle. تُبنى LLVM/C وتُشغّل للمقارنة، ويُنشر
Solidity في Anvil وتُقارن storage وtrace. كما يعمل corpus خام قبل Fusaka في EVM
الأصلي لـAnvil ليتحقق مستقلًا من ALU وcalldata copy و`MCOPY` المتداخل وmemory
expansion وKeccak وreturn data. تُقنّع account operands إلى 160 بت وفق
[المواصفة](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py)،
وتُفحص عروض environment ويلتزم `BLOCKHASH` بنافذة 256 block. يُفصل buffer
EIP-211 عن final output؛ لا يملأ `ExecutionResult::ReturnData` إلا `RETURN` أو
`REVERT`، ويتبع CREATE/CREATE2 القاعدة نفسها.

قبل أي أثر خاص بـopcode، يجري interpreter preflight للارتفاع المطلوب المعرّف
نوعيًا ولـpops وللارتفاع المحتفظ به مع pushes؛ فلا يمكن لـunderflow أوoverflow
تنفيذ نصف تعليمة. يحدد `EVMForkSemantics.def` معنى البايت `0x44`: يكون
`DIFFICULTY` قبل Paris و`PREVRANDAO` بدءًا من Paris. تعيد `REVERT` وsemantic faults
وstep limit ونفاد الموارد بسبب allocation/length كلًا من storage وtransient
storage وlogs وآثار selfdestruct إلى input snapshot، مع حفظ frame diagnostics
وبايتات revert الصريحة. يُوسم فشل allocation بـ
`ExecutionFaultKind::ResourceExhausted` بلا تخصيص error string؛ وإذا تعذر إنشاء
snapshot أصلًا تكون `HasPersistentStateSnapshot` بالقيمة false ولا يمكن commit
للنتيجة.

### حدود IR والموارد في الواجهات العامة

يتحقق `execute` العام أولًا من أن
`Code`/`Fork`/`Instructions`/`JumpDestinations` تكوّن LowIR قانونيًا. لذلك يعيد
تغيير fork أو تزوير instruction record أو عدم اتساق encoding أو فساد جدول وجهات
القفز `llvm::Error` قبل أن يفهرس الـinterpreter جدول التعليمات. ويتحقق
`lowerToMedIR` العام الخيارات ثم حدود الموارد ثم البنية، بهذا الترتيب، قبل أن
ينفذ `canonical decode replay` لـ`Low.Code` وفق fork/strictness المضمّنين ويقارن
كل fields في LowIR. بعد ذلك فقط يجوز استدعاء `lowerCanonicalLowToMedIR` أو بناء
index أو تخصيص output يتناسب مع records يتحكم بها المستدعي. ويعيد
`recoverHighIR` العام التحقق من LowIR/MedIR الخارجيين بالطريقة نفسها. أما
`lowerCanonicalLowToMedIR` و`recoverCanonicalHighIR` الخاصان فمخصصان لـIR الذي
يملكه `analyze`؛ يتجاوزان فقط replay المكرر غير العودي، مع بقاء كل HighIR
option/resource budgets إلزامية.

يحفظ برهان dispatcher لكل `MedStateLane` مجال selector مرتبًا
`Any/Exact/Excluded`. تجمع عملية join مجموعات Exact، وتأخذ تقاطع مجموعات الاستبعاد
Excluded، وتطرح مجموعة Exact من استبعاد cofinite؛ وعند توسيع المجال يعاد إدراج
الـlane للزيارة. لا تسجل equality مرشح true edge إلا إذا بقي selector مسموحًا،
وتستبعده على false edge. يسجل `XOR(selector, constant)` الخام zero/false edge
كمطابقة عندما تسمي كل canonical successors المدخل نفسه؛ ولا يشترط هذا fallthrough
أن يستهدف `JUMPDEST`. أما nonzero/true edge فهو mismatch يستبعد selector، ويحوّل
`ISZERO` التعبير نفسه إلى equality. يجري تنقيح selector word وzero-calldata word
وcalldata size وcall value guard لكل edge على حدة. يوقف unknown conditional
البرهان بدل متابعة branch محتمل فقط.

بعد التعرف على function candidate، يواصل traversal نطاقها باستخدام
`exact singleton selector` الخاص به. إذا عادت function إلى dispatcher مشترك،
فإن `SelectorEquality` و`XOR` الخام و`SelectorWord` لا تسلك إلا
`definite edge` المتوافقة مع selector المطابق سلفًا. وتحتفظ predicates المجهولة
أوغير المرتبطة بكل `definite edges` بتحفظ. لا تستخدم heuristic تستبعد entry
blocks الأخرى، كي يبقى التدفق المشروع `shared body/tail-call` ضمن نطاق function.

تختلف نتائج CALL/CREATE الخارجية: فنتيجة host غير حتمية بطبيعتها، لذلك يستكشف التحليل
حافتي CFG الدقيقتين. يحافظ ذلك على استعادة fallback في ERC-1167 من دون اعتبار
selector condition غير المقروءة دليلًا؛ أما dispatcher الذي يظل Unknown حقًا فيفشل
بصورة مغلقة.

يمنح `EVMAnalysisLimits.def` الـdecoder الخطي وCFG builder ميزانية aggregate واحدة
لـLowIR diagnostics عبر `MaxLowDiagnostics` و`MaxLowDiagnosticBytes`. يحاسب المساران
مسبقًا العدد الدقيق والبايتات النهائية ويرفضان الحد الصفري. تبقى ميزانيتا diagnostic
في LowIR وHighIR مستقلتين. ويفرض الجدول نفسه حسابًا
مستقلًا لـ`MaxHighDispatchCandidates`،
وللـaggregate على مستوى البرنامج `MaxHighRecoveredArguments`، ولـ
`MaxHighDiagnostics` مع `MaxHighDiagnosticBytes`، و`MaxHighReferenceVisits`،
و`MaxHighMemoryTransferCells`، و`MaxHighMemoryValueVisits`. تُحاسب records المرشح
والوسيط المستعاد مقدمًا قبل إدخالها في أي destination container أو تخصيص
name/type. ويُحاسب كل output diagnostic في HighIR بالعدد وبايتات الرسالة النهائية قبل بنائه
أو نسخه، بما فيه diagnostic الثابت للـIR malformed؛ فنفاد الميزانية يعيد hard
error مسمى ولا يحذف diagnostic أو fact بصمت.
وتُحاسب root CFG region الافتراضية وفق `MaxHighRegionBlockReferences` قبل reserve
أو نسخ قائمة block PC.

يحد `EVMABIParserLimits.def` من tuple nesting وtype nodes ومجموع array dimensions،
ويحد `EVMABITableLimits.def` من cardinality والنص الإجمالي في جداول
signature/variant العامة. تطبق validation العامة هذه الحدود قبل parse أو hash، ثم
ترفض enum غير الصالح وkind metadata وstandard وselector-evidence role والنوع غير
القانوني والـderived hash وmembership وcollision. lookup الـselector في الإنتاج
مفهرس، وlookup الـevent يستخدم جدول topic مرتبًا، وتتحقق topic API من أن عرض
`APInt` يساوي EVM word واحدة تمامًا قبل المقارنة أو الترتيب.

يعلن `EVMInterpreterLimits.def` عن `MaxSteps` و`MaxMemoryBytes` و
`MaxTraceEntries` و`MaxLogEntries` والـaggregate `MaxLogDataBytes` والـaggregate
`MaxHostReturnDataBytes` و`MaxCalldataBytes` والـaggregate
`MaxHostEnvironmentEntries` والـaggregate `MaxExternalCodeBytes` و
`MaxPersistentStateEntries`. يجمع حد host entries كلًا من `BlockHashes` و
`Balances` و`CodeHashes` و`ExternalCode` و`BlobHashes`، ويجمع حد بايتات code كل
bodies في `ExternalCode`. يحتفظ `MaxSteps` بنتيجة
`StepLimit` الصريحة. تُحاسب مسبقًا زيادات runtime في memory وtrace وlog وlog data
ومفاتيح persistent state الجديدة؛ وتعيد مجاوزة الحدود `ResourceExhausted` مع
rollback للحالة الدائمة والـlogs وتأثير selfdestruct. أما كبر aggregate بيانات
الإرجاع الأولية للمضيف أو map الحالة الدائمة فهو خطأ في API ‏`execute`.
يحتفظ الـinterpreter ببيانات إرجاع المضيف كـviews من `ArrayRef` ويستخدم
`lower_bound` فوق جدول التعليمات المرتب والمتحقق منه، بلا نسخ buffers أو إعادة
بناء PC map لكل تشغيل. ينجز `const execute preflight` التحقق من البرنامج وكل حدود
host input قبل نسخ environment أو أخذ snapshot للحالة أو بناء result.

### تدقيق فروق مباشر مع go-ethereum

يفرض التدقيق المحلي القياسي وCI عند كل تشغيل
`git fetch --depth=1 --force` للـ`HEAD` البعيد من default branch الرسمية في
`https://github.com/ethereum/go-ethereum.git`. ينشئ كل تشغيل bare repository
خاصًا مؤقتًا باسم غير متوقع؛ ولا يوجد Git
repository أو cache دائم مشترك. لا يختار الـrevision إلا authority ref الذي
أعاده fetch والـSHA الدقيق المحلول منه. يبلّغ السكربت عن SHA ويفحصه في worktree
مؤقت detached، ثم يدمر authority repository والـworktree معًا. ولا تُعد
`local_docs` ولا
checkout موجود ولا submodule مسارات تدقيق؛ إذ يصبح pin الـsubmodule قديمًا
تحديدًا عندما يلزم كشف live drift.

تحذف كل أوامر Git أولًا جميع `GIT_*` الموروثة، ومنها `GIT_CONFIG_*`، ثم تثبت
الإعدادات المدققة فقط. يعطل `GIT_CONFIG_NOSYSTEM` و`GIT_CONFIG_GLOBAL` إعدادات
system/global؛ ويعطل `GIT_ATTR_NOSYSTEM` و`core.attributesFile` على مستوى الأمر
attributes النظام والعامة؛ كما يعطل `core.hooksPath` على مستوى الأمر hooks.
يرفض الـrepository الخاص أي إعداد محلي
غير متوقع أو grafts أو `objects/info/alternates` أو `refs/replace`، كما يعطل
`GIT_NO_REPLACE_OBJECTS` الاستبدالات. أي مخالفة تفشل بشكل مغلق.

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

لا تعرض CLI العامة سوى `--manifest-output`؛ فلا يمكن للمتصل اختيار source أو
ref أوtoolchain. يستخدم manifest المغلق `schema 3`. يعكس Go probe كامل
inventory الـbool المصدّر من `params.Rules`، ويستدعي
`LookupInstructionSet(params.Rules)` لكل fork مربوط، ويفحص كل 256 byte slots. لا
يُحسم تخصيص slot إلا من `operation.undefined` في geth؛ أما `HasCost` فهو فحص
متقاطع للكلفة فقط لأنه يعيد false أيضًا للعمليات المعرّفة ذات الكلفة الصفرية.
يجب أن يطابق كل slot بحالة `defined && !HasCost` تصريح
`EVM_GETH_ACTIVE_WITHOUT_COST` تمامًا عند fork التفعيل المحدد؛ ويفشل slot غير
معرّف وله كلفة، أو slot معرّف غير مراجع، أو اختفاء marker من upstream بشكل مغلق.
أي field أوrecord مجهول أو مكرر أو مفقود أو خارج النطاق أو لم يُحلّل هو خطأ.
ويرفض كل `.def parser` كذلك نصًا شبيهًا بالmacro لم يستهلكه، بدل قبول policy
`partial`. يملك
`EVMUpstreamOpcodePolicy.def` aliases وtyped exclusions التاريخية/EOF غير
المجدولة، ويتحقق من invariants الخاصة بـoverlap/inactive. أما
`EVMUpstreamSemanticsPolicy.def` المستقل فيملك inventory `params.Rules` المغلق
المنعكس وfork mappings وbase-stack exceptions وعائلات dynamic-immediate. يعمل CI عند push إلى `dev` وpull request والتشغيل
اليدوي والجدول اليومي، ويرفع revision الدقيق وmanifest وlog كـartifact عند الفشل.

وبصورة أدق، يصنّف `EVMUpstreamSemanticsPolicy.def` كل boolean field مصدّر في
`params.Rules` بسجل `EVM_GETH_RULE_FIELD` واحد ضمن `MappedForkSelector` أو
`NoOpcodeAllocation` أو `ExcludedSelectorExpectedError`. يفعّل التدقيق كل field منفردًا
ويستدعي `LookupInstructionSet`: يجب أن يعيد الصنفان الأولان nil error والثالث error،
ويجب أن تطابق fingerprint الكاملة لـ256 opcode/stack slots قيمة `ExpectedFork` دائمًا.
الحقول `IsEIP155` و`IsEIP2929` و`IsEIP4762` و`IsPetersburg` بلا allocation وتطابق
Frontier؛ أما `IsUBT` فيجب أن يفشل ويعيد fingerprint Cancun.

يصرح `EVMUpstreamSemanticsPolicy.def` بopcodes كل عائلة EIP-8024 ديناميكية ونوع
عملها وstack delta الصالح، بينما يبقى `EVMEIP8024Immediates.def` سلطة فك
immediates المنفصلة ويصنف كل قيم single/pair. يحصل التدقيق عبر `go -overlay`
على handlers الحقيقية الخاصة `operation.execute`، ويفحص `canonical fork jump tables`
و`mainnet active/scheduled jump tables` جدولًا بجدول. تسجل العائلة `inactive`
صراحة، أما العائلة `partial` فخطأ. في كل جدول نشط تنفذ العمليات الثلاث لكل
immediates (`3x256`) إضافة إلى `3 missing-operand cases`، مع فحص القبول وPC delta
وstack mutation وunderflow وسلوك `0x00` المفقود مقابل السياسات التصريحية نفسها.

لـ`EVM_HARDFORK_LATEST` target canonical واحد بالضبط. يربط
`EVMUpstreamForkAliases.def` المغلق Prague بـPectra وOsaka وBPO1 حتى BPO5 بـFusaka،
وتبقى Paris وShanghai وCancun وAmsterdam وBogota identity؛ ويفشل أي اسم جديد مجهول
بصورة مغلقة. يثبت كل audit قيمة `audit_unix_time` واحدة ويسجلها، ويشترط أن يطابق
`MainnetChainConfig.LatestFork(time)` أحدث fork في NeverD، وأن يكون
`LatestFork(max uint64)` في alias inventory وقد اختبر fork canonical المقابل؛ وتقارن
مجموعتا التعليمات كاملتين. يسجل manifest ‏`authority=official-fresh-fetch` وURL الرسمي
و`HEAD` المطلوب وSHA المحلول. يثبت probe ‏`GOTOOLCHAIN=local`.

يفرض Go وPython ‏`input/collection/string hard limits` قبل إنشاء metadata عدائية؛
فتفشل المدخلات أوالمجموعات أوالنصوص الضخمة مغلقًا. أما
`bounded diagnostic output` فيضع `digest` للمحتوى الكامل و
`explicit truncated marker` على العرض الطويل. لكل child process خرج وdeadline
محدودان؛ وعند التجاوز تُقتل `process group` كاملة/process tree وتُصرّف pipes.

تسجل وصلة schema 3 الحالية `schema_version=3` و
`audit_unix_time=1787534659` و`authority=official-fresh-fetch` و
`remote=https://github.com/ethereum/go-ethereum.git` و`ref=HEAD` وrevision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562` و`Go 1.24.0` محلية و
`stack_limit=1024` و`diagnostics=[]`. تقارن `21 fork tables` و`20 Rules probes`
المصنفة `15 mapped/4 no-op/1 expected-error`. يسمي سجلا
`mainnet active/scheduled` ‏`upstream BPO2`، ويربطه alias المغلق بـ
`NeverD Fusaka`. يغطي EIP-8024 ‏`23 table targets`؛ ولا ينشط إلا
`Amsterdam/Bogota`، فتنتج `1536 candidate executions` و
`6 missing-operand cases`، وتتطابق `three handler symbols` بين الهدفين النشطين.
نجح Python audit ‏`67/67` و`C++ Opcode 10/10`. نجح audit الحقيقي على macOS داخل
`sandbox-exec` مع بقاء `go run` النهائي بلا شبكة، ويفرض Linux ‏`bubblewrap`.

تعمل كل مراحل Go، أي `go env` و`go mod init` و`go mod edit` و`go mod tidy` و
`go mod download` و`go run`، داخل filesystem sandbox من نوع `capability-root`.
تقتصر صلاحية القراءة على private probe وfresh geth و`resolved GOROOT` المتحقق منه
وجذور system runtime اللازمة بدقة؛ ولا يكتب إلا داخل isolated environment roots.
تضاف الشبكة فقط لمراحل dependencies التي تحتاجها، ويظل run النهائي offline. تضع
الاختبارات sentinels في `host HOME/workspace` وتشترط رفض قراءتها وعدم ظهور محتواها
في أي output. يطبق Linux سياسة `bubblewrap` مماثلة بلا `/` broad bind.

يستنفد `NeverDEVMDecoderPropertyTests` كل مدخلات البايتين في كل fork يغيّر
decoder، ويقارن decode كاملًا وحدود `JUMPDEST` الدقيقة، ثم يمرر سلاسل bytes
عدائية حتمية محدودة الطول عبر كل forks.

تحفظ whole-stack state lanes في LowIR الارتباط داخل path نفسه، و`MayReachable`
مرشح CFG فقط. تصبح loop-carried slot المتغيرة `Top` دلاليًا على back-edge، بمعزل
عن budgets؛ ونفادها يفشل بلا emergency widening. تتبع memory ‏HighIR الكتابات
الثابتة وoverlap/kill وإبطال unknown. لا يثبت payload إلا selector وبايتات Panic
المعروفة؛ وتحفظ declaration ‏custom error المعروفة الأنواع القانونية بلا ادعاء
كل runtime argument value. وتظل الحقائق الأخرى مرشحات تسندها الأدلة.

## عقد C المولد

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

تستخدم العمليات البيئية host ABI التالية. `a0` هو قمة المكدس الأصلية، والوسائط
غير المستخدمة صفر، والقيمة المعادة أول pushed value. يعمل trace hook قبل كل تعليمة.

```c
evm_word neverd_evm_host_op(
    struct neverd_evm_env *environment, uint8_t opcode,
    evm_word a0, evm_word a1, evm_word a2, evm_word a3,
    evm_word a4, evm_word a5, evm_word a6);
void neverd_evm_trace(
    struct neverd_evm_env *environment, uint64_t pc, uint8_t opcode);
```

```bash
clang -std=c2x -ffreestanding -c contract.c
```

يجب أن يدعم frontend النوع `_BitInt` حتى 512 بت على الأقل. لا يدعمه Apple Clang
لهدف Darwin حاليًا؛ استخدم على macOS هدفًا non-Darwin قادرًا أو خرج LLVM مباشرة.

## عقد Solidity المولد

يجمع الخرج تصريحات function/storage/event/error الخاصة بكل selector مع state
machine دقيقة للـPC والمكدس. يخرج slot الثابت مثل
`recovered_storage_slot_3 = uint256(0x3)` لا كمتغير متتابع يخترع layout.

العقد `abstract` عمدًا. تجاوز `_evmHost` لتنفيذ التأثيرات البيئية؛ و`_evmTrace`
virtual ويصدر `EVMTrace` افتراضيًا.

```bash
solc --bin contract.sol
```

## C API

```c
neverd_session_t session = neverd_session_create();
neverd_evm_set_hardfork(session, "cancun");
neverd_evm_set_strict(session, 1);
if (!neverd_session_load(session, "contract.evm") ||
    !neverd_session_analyze(session)) {
  /* inspect neverd_last_error(session) */
}
const char *solidity = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_SOLIDITY, 0, 0);
const char *c = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_C, 0, 0);
neverd_free_string(solidity);
neverd_free_string(c);
neverd_session_destroy(session);
```

يبقى `neverd_decompile_all` متوافقًا ويخرج C. نقاط الدخول الجديدة هي
`neverd_session_bitness` و`neverd_evm_set_strict` و
`neverd_evm_set_hardfork` و`neverd_decompile_all_ex`. تُرفض صراحة طلبات Solidity
للـnative ومسار LLVM-to-C القديم لـEVM وnative object roundtrip لـEVM ولا تُهمل.

## حدود صريحة

- يدعم legacy bytecode فقط؛ لا يفك EOF containers بعد.
- Amsterdam/Bogota هدفا تطوير صريحان؛ يبقى `latest` على Fusaka النهائي حتى
  تثبيت opcodes المجدولة.
- لا RPC ولا اكتشاف chain state ولا حساب gas/refund ولا تنفيذ precompile.
- يستخرج creation من wrappers ثابتة شائعة، وليس محاكاة transaction كاملة.
- تبقى dynamic jumps غير مباشرة ما لم يثبتها تحليل ثابت محدود.
- أنواع ABI والأسماء وmappings وevents وcustom errors استعادة best-effort.
- يتطلب التنفيذ المستقل للتأثيرات host hooks لـC/Solidity.
