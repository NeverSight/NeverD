**اللغات**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# فك تجميع EVM

[← فهرس التوثيق](README.ar.md)

يحمّل NeverD بايت كود Ethereum Virtual Machine التقليدي، ويبني LowIR خاصًا
بعرض 256 بت وMedIR بنمط SSA للمكدس وHighIR مستعادًا، ثم يخرج LLVM IR أو C23 أو
Solidity. التحليل الصارم هو الافتراضي: أي opcode غير مخصص أو غير نشط في hardfork
المختار ينتج خطأ عند قيمة PC الدقيقة.

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
runtime المنسوخ. يُعد الحقل الذي لا يحوي سوى `0x` الاختيارية فارغًا، فلا يخفي
runtime فارغ fallback صالحًا من creation. ولا تُزال خريطة CBOR النهائية إلا بعد
التحقق من طولها وعلامة map ومفتاح معروف من `solc` أو `ipfs` أو Swarm.

ينتج hex التالف وعدد الخانات الفردي وlinker placeholders غير المحلولة وartefacts
متعددة العقود الملتبسة وحدود metadata غير الصالحة والكود الفارغ أخطاء قابلة
للتصرف. يختار `BytecodeLoadOptions::ArtifactContract` الصيغة `Contract` أو
`path/File.sol:Contract`. وإذا تكرر الاسم في ملفات مصدر مختلفة يُرفض الاسم غير
المؤهل كي لا يختار ترتيب JSON عقدًا خاطئًا بصمت.

EVM مسجل في core loader registry وليس مخفيًا خلف backend plugin؛ لذلك تتلقى CLI
وC API والمفكك وCFG واستعلامات IR الصورة المطبعة والخيارات نفسها.

## Hardforks وopcodes

تُغطى كل opcodes التقليدية المخصصة وعددها 150 من Frontier حتى Fusaka، ومنها
`PUSH0` وtransient storage و`MCOPY` وblob opcodes و`CLZ`. يختار `latest` Fusaka
افتراضيًا.

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
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2)، وتسجله
execution-spec-tests على أنه
[أزيل من Osaka وغير مجدول](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md).
لذلك لا يعامل NeverD المقترح المسحوب كسلوك mainnet نهائي.

يرفض strict mode البايتات المجهولة أو غير النشطة في fork. يحتفظ
`--evm-relaxed` بها في LowIR والتشخيص، لكن backend يفشل إن وصل التنفيذ إليها؛
ولا تتحول بصمت إلى NOP.

## بنية metadata بأسلوب LLVM

تتبع metadata اليدوية لـEVM نمط LLVM لملفات `.def` متعددة التضمين:

- `EVMOpcodes.def` هو مصدر الحقيقة الوحيد لـ150 opcode نهائية وأربعة opcodes
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
- تولد `EVMHardforks.def` و`EVMEffects.def` و`EVMExitStatuses.def` و
  `OutputLanguages.def` enums مرتبة وparsers وأسماء وخيارات CLI وقيم C ABI.
  ويملك `EVMConstants.h` العروض والحدود والأسماء الثابتة.
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

- **LowIR** يحفظ PC وencoding وPUSH immediate مع right-zero padding عند القطع
  وblocks وedges وأهداف `JUMPDEST` المتحققة وreachability وstack height.
- **MedIR** يمثل المكدس SSA بعرض 256 بت، وينشئ phi ويطوي العمليات pure ويحفظ
  effect وmemory وstate وcall-value كخصائص مستقلة.
- **HighIR** يستعيد best-effort selectors وكلمات calldata/return المحتملة و
  mutability وconstant slots وevents وreverts ومناطق function/CFG. تبقى payability
  مستقلة عن state lattice. يجعل dynamic jump القابل للوصول وغير المحلول state
  تساوي `Unknown` ويجعل Solidity محافظًا `nonpayable`؛ وتُشخّص selectors المتعارضة
  وتُحذف.
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
