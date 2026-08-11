**اللغات**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# فك تجميع Solana SBF

[← فهرس التوثيق](README.ar.md)

يحمّل NeverD نواتج نشر Solana كبرامج SBF من الدرجة الأولى ويعرض المسار كاملًا
عبر CLI و`libneverd`:

```text
SBF ELF
  → ELF loader وverifier واعيان بالإصدار
  → LowIR بلا فقد + CFG
  → MedIR مطبع + حقائق السجلات
  → استعادة functions وsyscalls وملاحظات CPI/account وregions
       ├─ LLVM IR متحقق منه
       ├─ C11 قابل للنقل
       └─ Rust stable آمن
```

يتبع التنفيذ VM الحالي من Anza `sbpf` بدل eBPF العام في Linux. تعيش metadata
الخاصة بالversion وopcode وsyscall وrelocation والبروتوكول في قواعد `.def` تحت
`include/neverd/sbf/`؛ وتستهلك loaders وbackends جداول typed مولدة بلا تكرار.

## المدخل وإصدارات VM المدعومة

المدخل برنامج Solana بصيغة ELF64 little-endian (`.so`).

| SBF | ELF layout | Machine ID | سلوك ISA المهم | الحالة |
|-----|------------|------------|----------------|--------|
| v0 | sections/relocations تقليدية | `EM_BPF`, `EM_SBPF` | frames ثابتة بفجوات افتراضية وLDDW وmemory opcodes قديمة | legacy |
| v1 | sections/relocations تقليدية | `EM_BPF`, `EM_SBPF` | stack frames معدلة يدويًا | legacy |
| v2 | sections/relocations تقليدية | `EM_BPF`, `EM_SBPF` | حساب PQR وmemory encodings منقولة وimmediate subtraction معكوسة وCALLX من source register | legacy، غير رتيب |
| v3 | program headers صارمة، بلا dynamic relocations | `EM_BPF` | syscalls/calls ثابتة وJMP32 وCALLX من destination register وbytecode عند `0x100000000` وrodata عند صفر | صيغة toolchain المنشورة الحالية |
| v4 | program headers صارمة، بلا dynamic relocations | `EM_BPF` | ISA v3 مع عقد memory mapping بمحاذاة | upstream `sbpf` الحالي؛ يختلف توفر cluster |

لا تنتقل تغييرات v2 عمدًا إلى v3. feature checks صريحة وليست تخمين
`version >= N`. يرفض strict الافتراضي headers/ranges/alignments التالفة وwritable
legacy sections غير المدعومة وcontinuations/registers/frame-pointer writes/
branches غير الصالحة وopcodes غير النشطة، مع instruction slot وvirtual address.

يستخدم toolchain الحالي `cargo build-sbf`. برامج v3+ الحديثة موجهة إلى Rust ولا
يستهدف upstream C toolchain v3؛ لا يحد ذلك خرج NeverD، فأي SBF مقبول يخرج C أو Rust.

- [برامج Solana](https://solana.com/docs/core/programs)
- [تنفيذ البرامج](https://solana.com/docs/core/programs/program-execution)
- [مرجع syscall](https://solana.com/docs/core/programs/syscall-reference)
- [Anza sbpf VM](https://github.com/anza-xyz/sbpf)
- [Agave changelog](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

## CLI

```bash
neverd info program.so
neverd headers --json program.so

neverd lift --dump-low program.so
neverd lift --dump-med program.so
neverd lift --dump-high program.so

neverd lift -o program.ll program.so
neverd decompile --language=c -o program.c program.so
neverd decompile --language=rust -o program.rs program.so

neverd lift --sbf-version=v2 program.so
neverd lift --sbf-relaxed --dump-low program.so
```

لا يغير `--sbf-version=auto|v0|v1|v2|v3|v4` semantics إلا بعد تحقق layout
المكتشف. هو للـfixtures التالفة أو البحثية، لا لإعادة تفسير ملف غير موثوق كمعيار
packaging مختلف.

## التحليل والاستعادة

يحفظ LowIR كل 8-byte encoding وraw fields وLDDW continuations وresolved calls
وsyscall hashes وblocks وedges وreachability وdiagnostics. يطبع MedIR encoding
الخاص بكل version إلى operations ذات types بعرض 32/64 بت وextensions صريحة وحساب
محروس وmemory widths وcall kinds. ويتتبع register dataflow الثوابت وعناوين
stack/rodata.

يستعيد HighIR entry/internal functions وdirect call edges وأسماء syscall الرسمية
وstrings وnatural loops وreducible conditionals وملاحظات Solana محافظة. تعد
`sol_invoke_signed_rust` و`sol_invoke_signed_c` CPI، والذاكرة المبنية على input
register هي account/input access. ولا يخترع Anchor types أو account layouts بلا IDL.

يشترك C وRust في backend-neutral structuring pass. يخرج `if`/`if-else` و
`while`/`loop` عند وجود تمثيل reducible وحيد، بينما تحتفظ internal calls وCALLX
والتدفق irreducible بـPC dispatcher الدقيق.

تغطي قاعدة syscall logging وmemory وPDA وSHA-256/Keccak/Blake3 وPoseidon و
secp256k1 وcurves/alt-bn128 وmodular exponentiation وCPI وreturn data وsibling
instructions وcompute units وsysvars مثل epoch rewards. تعالج relocations
`R_BPF_64_64` و`R_BPF_64_RELATIVE` و`R_BPF_64_32` مركزيًا. تطبق text relocations
ونصفي LDDW ومفتاح Murmur3 CALL الرسمي قبل decode. وإذا طبق `R_BPF_64_32` وأزيل،
يعاد حساب registry key من symbols وtarget slots لاستعادة internal calls.

## استعادة برنامج Solana

فوق نموذج آلة SBF يذكر NeverD ما يعنيه البرنامج بوصفه برنامج Solana. كل حقيقة
مسجَّلة تحمل الدليل الذي أنتجها، وما لا تحسمه البايتات يُترك غير محدَّد بدل تخمينه.

| المُستعاد | الدليل |
|-----------|--------|
| عناوين base58 في read-only data | تطابق `SBFKnownAddresses.def`، أو ثابت ينشئه الكود |
| عنوان البرنامج المعلَن | `sol_memcmp_` بطول مفتاح كامل مقابل ثابت في read-only |
| توزيع تعليمات Anchor | مقارنة 64-bit تساوي discriminator من SHA-256 مع namespace |
| أهداف CPI | سجل instruction المتاح من وسيط الاستدعاء |
| العملية التي يختارها الاستدعاء | selector مُدرَج في `SBFProgramInstructions.def`، أو discriminator من Anchor في المقدمة |
| بذور العنوان المشتق | مصفوفة seed descriptor المتاحة من وسيط الاشتقاق |
| قراءة وكتابة حقول الحساب | load/store يقع عنوانه إثباتًا داخل input المسلسل |

يمرر loader وسيطًا واحدًا هو input buffer المسلسل في قاعدة منطقة input، لذا ينتج
انتشار الثوابت أسماء حقول حساب بدل offset خام. يحفظ `SBFAccountLayout.def`
التسلسل الرسمي، وتُفحص حقوله الثابتة لتغطية مداها تمامًا بلا فجوة.

يشتق Anchor الـ discriminator بتجزئة `<namespace>:<name>` عبر SHA-256 والاحتفاظ
بأول ثمانية بايت، وهي عملية أحادية الاتجاه. لذلك يؤكد NeverD المرشحين فقط:
`SBFAnchorNames.def` قاموس أسماء متكررة في البرامج المنشورة، و`--sbf-idl` يوفر
IDL البرنامج نفسه وله الأولوية. لا تُسمّى مقارنة 64-bit discriminator إلا بعد أن
يُحلّ اسم واحد منها على الأقل.

يسجل `SBFKnownAddresses.def` عناوين البروتوكول والبرامج القياسية؛ كل مدخل يجب أن
يفك إلى 32 بايت بالضبط، وهو ما تفرضه مجموعة الاختبارات. يحتاج الاسترجاع أيضًا إلى
ABI الخاص بـ syscall: إذ يربط SBPFv3 الـ read-only data بالعنوان صفر، فيتساوى
وسيط الطول مع عنوان بيانات منخفض؛ لذا يسجل `SBFSyscalls.def` أي سجلات الوسائط
تحمل عنوان VM، ولا يُتتبع سواها.

يصف استدعاءا invoke التعليمة نفسها ببنيتين مختلفتين، ويحتفظ `SBFCPIABI.def`
بالتخطيطين مفهرسين بالـ syscall الذي يختار كلًا منهما؛ قراءة أحدهما بإزاحات الآخر
لا تفشل، بل تُبلّغ صامتة عن أول حساب باعتباره البرنامج المُستدعى. ثم يسمّي
`SBFProgramInstructions.def` العملية المطلوبة من برنامج قياسي انطلاقًا من الـ
selector الذي تنشره واجهته: فهرس متغير bincode لبرامج system وstake
وlookup-table وupgradeable-loader، وبايت أول لبرامج token، بما في ذلك نطاق
امتدادات Token-2022 فوق الترقيم المشترك مع برنامج token الأصلي. أما selector غير
المُدرَج فيُبلَّغ عنه كرقم.

### ذاكرة العمل ونوافذ syscall

نادرًا ما يسلّم البرنامج ثابتًا إلى runtime؛ فهو يبني مصفوفة seed وinstruction
مسلسلة وحمولتها داخل frame الخاص به أو على heap، ثم يمرّر مؤشرًا فقط. قراءة
الصورة المحمّلة وحدها ترى المؤشر ولا ترى ما يشير إليه، لذا يحتفظ الاسترجاع بنموذج
بدقة البايت للذاكرة التي لا يكتبها سوى هذا البرنامج، بحدٍّ أقصى قدره
`kMaxModeledScratchBytes`.

يقرر جدولان ما الذي يبقى بعد الاستدعاء. يذكر `SBFSyscalls.def` أي سجلات الوسائط
تحمل عنوان VM، ويذكر `SBFSyscallMemory.def` ما يفعله runtime عبرها، قراءةً أو
كتابةً بامتداد `Fixed` أو `Counted` أو `Opaque`. فـ syscall بلا نافذة كتابة لا
يمكنه تغيير أي بايت لدى المستدعي، لذا يبقى ما أُثبت قبل `sol_log_` مُثبتًا بعده.
والكتابة المحدودة بوسيط طول تُبطل تلك النافذة وحدها، بينما تُبطل الكتابة
`Opaque` عنوانها الأساسي وكل ما فوقه، لأن المخزن المؤقت لا يمتد أسفل بدايته ولا
يعبر حدود منطقة VM. ويجري التحقق المتبادل في الاتجاهين بين ملخص التأثيرات في
`SBFSyscalls.def` وجدول النوافذ، فلا ينحرف أحدهما وحده.

ويجري تتبّع `sol_memcpy_` و`sol_memmove_` و`sol_memset_` بدل الاكتفاء بإبطالها:
فمتى ثبتت الوجهة والطول والمصدر أصبحت بايتات الوجهة معروفة. وهذا بالضبط ما
يستعيد العملية التي يستدعيها برنامج Anchor، لأن حمولته تُنسخ إلى مكانها بدل أن
تُربط.

أما الاستدعاء إلى دالة لم يصفها هذا التحليل فيُفترض أنه يكتب في كل ما يمكنه
بلوغه. المُستدعى يعمل في frame خاص به، لذا يترك النموذج سليمًا كل استدعاء تُثبَت
فيه أن سجلات وسائطه لا تعنون ذاكرة العمل، وما عدا ذلك يُسقطه. ويكتب
`sol_invoke_signed_rust` و`sol_invoke_signed_c` بيانات الحسابات لا ذاكرة
المستدعي، فتبقى استدعاءتان بُنيتا في block واحد مقروءتين معًا.

النموذج تحليل must أمامي على CFG داخل الدالة: لا يبقى البايت حيًا حتى block ما
إلا إذا كتب كل مسار يصل إليه القيمة نفسها. ولا تُتتبع حواف الاستدعاء لأن المُستدعى
لا يرث شيئًا من frame مستدعيه. أما البرامج التي تتجاوز `kMaxScratchFlowBlocks`
block فتحتفظ بالاسترجاع لكل block وتفقد فقط الحقائق العابرة لحدود الـ block.

يفهرس `SBFLints.def` ملاحظات على مستوى البرنامج: غياب فحص signer أو owner، هدف
استدعاء غير ثابت، syscall مهجور أو خلف feature gate، ونسخة SBPF ستتوقف
SIMD-0500 عن قبولها للنشر. لكل منها severity وconfidence، ولا يغير أي lint
الدلالات المفكوكة. لا شيء في هذه الطبقة يتصل بالشبكة.

## عقد LLVM runtime المولد

لا يعامل LLVM عنوان VM كـhost pointer. تعيد declarations المتحققة لـload/store/
syscall قيمة status من `i32`، وتكتب load/syscall قيمة `i64` عبر output pointer.
ينتقل أي status غير صفري إلى SBF fault block صريح، ويمر module عبر
`llvm::verifyModule` قبل الخروج.

## عقد C host المولد

```c
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;
```

`width` بالبت؛ وتصبح host return غير الصفرية status صريحة لـSBF. يمثل الخرج
registers وreturn PC وr6-r9 المحفوظة وframe pointer وعناوين VM وdivision faults
وPQR wide وwrapping shifts. ولا يخرج إلا helpers المستخدمة، لذلك يجتاز
`clang -Wall -Wextra -Werror`.

## عقد Rust host المولد

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

الخرج Rust stable آمن بلا raw pointers. تكون entry point generic على trait وتستخدم
safe arrays ثابتة الحجم. تبنيها الاختبارات بـ`rustc --edition=2021 -D warnings`.

## C API

بعد تحميل SBF تبقى session functions وdisassembly وIR dumps وCFG/call graph JSON
وsections وsymbols وrelocations وstrings وheaders متاحة. يُختار Rust بقيمة
output-language enum مضافة مع ثبات ABI.

```c
neverd_session_t session = neverd_session_create();
neverd_sbf_set_strict(session, 1);
neverd_sbf_set_version(session, "auto");
const char *rust = neverd_decompile_all_ex(
    session, "program.so", NEVERD_OUTPUT_RUST, 0, 0);
/* consume rust, then: */
neverd_free_string(rust);
neverd_session_destroy(session);
```

## التحقق والحدود

يغطي `unittests/sbf/` invariants metadata وloaders من v0-v4 وstrict verifier
وCFG/recovery وLLVM المتحقق وC/Rust بلا warnings وraw interpreter مستقلًا عن
MedIR وC API. تعمل fixture فيها conditional+loop باللغتين مقابل raw oracle؛
ويستخدم corpus ELF الرسمي لـ`sbpf` محليًا بلا تضمين binaries خارجية.

- تُرفض صراحة إعادة كتابة SBF وobject-code roundtrip.
- استعادة Anchor IDL/types وlive RPC/accounts خارج نطاق loader.
- تمر syscalls وVM memory في المصدر المولد عبر host contract؛ ليس runtime مستقلًا.
- relaxed للفحص فقط ولا يمنح instruction غير صالح semantics مخمنة.

## خط أساس المطابقة الحالي (2026-08-10)

بعد تطبيق relocation تصبح `ProgramImage` واحدة غير قابلة للتغيير ومفهرسة بعناوين
VM هي مصدر الحقيقة المشترك للـdecoder وinterpreter واستعادة strings وواجهات
LLVM/C/Rust. لا توجد نسخ مستقلة من text أو rodata يمكن أن تنحرف عن loader.

توجد السجلات المغلقة في `SBFVersions.def` و`SBFOpcodes.def` و
`SBFRelocations.def` و`SBFArgumentRegisters.def` و`SBFProtocolLimits.def` و
`SBFSyscalls.def` و`SBFSyscallMemory.def` و`SBFCPIABI.def` و
`SBFProgramInstructions.def` و
`SBFUpstreamSources.def`. تبقى رسائل التشخيص وأسماء LLVM ذات الاستخدام الواحد
محلية، وفق أسلوب LLVM الفعلي.

يسجل `SBFProtocolLimits.def` قيمة 65,536 instruction التاريخية وحد بيانات
account الحالي البالغ 10 MiB؛ ويشتق NeverD حد decode المحافظ من الحد الأخير.

في strict v3/v4 تكون program headers المحدودة هي عقد runtime؛ أما section وsymbol
tables فهي debug enrichment اختيارية لا تُسقط image صالحة عند غيابها أو تلفها.
وفي legacy v0-v2 تُدمج `.text` و`.rodata` و`.data.rel.ro` و`.eh_frame`، ثم
تُطبق `R_BPF_64_64` و`R_BPF_64_RELATIVE` و`R_BPF_64_32` مرة واحدة قبل تجميد image.

| الدليل | النتيجة المدققة |
|--------|-----------------|
| manifest ELF الرسمي | 20/20 artifact من `sbpf/tests/elfs` |
| مصفوفة ISA | كل 256 encoding عبر v0-v4، أي 1,280 خلية، مع حدود verifier |
| differential execution | raw-byte oracle مقابل LLVM ORC وC11 وstable Rust، مع memory/fault/syscall trace |
| التجميع المتكامل | 124/124 حالة في 13 test binary |
| ASan + UBSan | 121/121 حالة core في 12 binary بلا report |

المراجعة مثبتة على Anza `sbpf` revision
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` وAgave
`cae40aa610fdbdb313209bc1eec737079eb59688`. لتحديثها راجع
`SBFUpstreamManifest.def` و`SBFUpstreamOpcodes.def` و`SBFUpstreamSources.def`
ثم شغّل:

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

أظهرت المقارنة أن `sol-azy` يتعطل على ELF strict الحالي ويحتفظ بعقدة CFG legacy
غير معرّفة؛ `solana-data-reverser` يركز على account data، و`SolDragon` يصف
التحليل كعمل قيد التطوير، و`bn-ebpf-solana` يتطلب Binary Ninja. لذلك يبقى
`sbpf` وAgave الرسميان المرجع الدلالي.
