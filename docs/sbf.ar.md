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

رقم النسخة ليس مواصفة بحد ذاته، لذلك يحمل `SBFVersionFeatures.def` التغييرات
السلوكية بينما يؤلفها جدول النسخ. يحمل كل سجل مقترح SIMD الذي اعتمد التغيير
والمُسنَد الذي يعرضه `anza-xyz/sbpf` للسؤال نفسه، لأن عدة مقترحات تهبط في نسخة
واحدة والمقترح الواحد يغير أشياء غير مترابطة: فـSIMD-0173 ينقل أصناف تعليمات
الذاكرة ويتقاعد `lddw` معًا، بينما يضيف SIMD-0174 صنف PQR بشكل مستقل في النسخة
ذاتها. تسجيل المقترح على الخاصية لا على النسخة هو ما يبقي ادعاء النسخة المستعادة
قابلًا للتتبع إلى الوثيقة التي قررته، وهو سبب فصل قاعدتَي `callx`: فـSIMD-0173
يقرأ سجل المصدر وSIMD-0377 يقرأ سجل الوجهة.

لا تنتقل تغييرات v2 عمدًا إلى v3. feature checks صريحة وليست تخمين
`version >= N`. يرفض strict الافتراضي headers/ranges/alignments التالفة وwritable
legacy sections غير المدعومة وcontinuations/registers/frame-pointer writes/
branches غير الصالحة وopcodes غير النشطة، مع instruction slot وvirtual address.

## الـ runtime الذي يتحدث عنه الوصف

تأتي نسخة ISA من الملف، ولا يأتي منه سوى ذلك تقريبًا. فأيّ syscalls تُحَلّ يعتمد
على الشبكة والـ slot؛ وعند أي بايتات يقع حقل الحساب يعتمد على loader الذي يملك
البرنامج؛ وهل يتلقى entrypoint وسيطًا ثانيًا يعتمد على مفتاح تقلبه الشبكة؛ أما
سؤال هل يمكن نشر البرنامج فهو غير سؤال هل يعمل. ولا يستطيع مفتاح نسخة واحد
التعبير عن شيء من ذلك، لذا فهذه محاور منفصلة بجداول منفصلة.

يسجل `SBFRuntimeFeatures.def` الـ clusters والأغراض والبوابات التي تغير ما
يبلّغ عنه NeverD، ومع كل بوابة معرّفها في runtime وحساب feature الذي تسجل حالته
التفعيل والـ slot الذي فعّلها عنده كل cluster. قد يوجد حساب pending من دون أن
يشغّل البوابة، والبوابة التي لا سطر تفعيل لها في cluster لم
تُفعّل هناك. فـ`simd-0321` مفعّلة في كل cluster، بينما `simd-0449` وsyscall
الخاص بـSHA-512 مفعّلان في testnet وdevnet ومعطّلان في mainnet، وهذا بالضبط سبب
فشل برنامج يعمل على devnet حين ينتقل إلى mainnet.

في مراجعة Agave المثبّتة تشدّد بوابة
`syscall_parameter_address_restrictions` (`simd-0459`) عقد عناوين VM والمحاذاة
لمعاملات syscall وCPI؛ وتسجّل حالة RPC النهائية التفعيل عند slot 429,840,000 في
mainnet و407,468,256 في testnet و462,240,000 في devnet. تغيّر بوابة
`account_data_direct_mapping` بيانات account من نسخة داخل input buffer إلى
مناطق ذاكرة ذات backing مباشر عند استخدام فضاء العناوين المعدّل؛ وهي غير مفعّلة
في mainnet ومفعّلة عند 408,332,256 في testnet و463,968,000 في devnet. لا تنشئ
أي من البوابتين Account ABI جديدًا ولا تغيّر إزاحات حقول ABIv0/ABIv1 المنطقية:
يبقى loader المالك هو الذي يختار التسلسل، ويسجلهما NeverD كبيانات topology
خاصة بالـ runtime.

تبقى بتات الميزات append-only. وبعد أن تجاوزت اللقطة المرصودة 32 بت، صار
`RuntimeFeatureMask` نوع التخزين وhost ABI الوحيد من نوع `uint64_t`.
عرض ABI الخاص بـv2 ثابت ولا يُوسَّع in-place؛ وإذا تجاوزت الحقول 64 بت فالإضافة تكون v3 أو تمثيلاً multiword، لا تغيير عرض v2.
ويميز `RuntimeFeatureDisposition` بين `RuntimeBranch` حي و`FoldedBranch`
أصبح جانبه النشط غير مشروط في revision المثبت، مع بقاء جانبه القديم مهمًا عند
slots التاريخية. حقائق التفعيل من RPC النهائي (`—` تعني غير مفعّل):

| gate | domain / disposition | mainnet | testnet | devnet |
|------|----------------------|---------|---------|--------|
| `disable_deploy_of_alloc_free_syscall` | `ProgramAdmission` / `FoldedBranch` | 209,088,008 | 195,356,264 | 224,208,000 |
| `enable_bpf_loader_set_authority_checked_ix` | `LoaderManagement` / `RuntimeBranch` | 251,424,000 | 247,628,260 | 255,744,000 |
| `remove_bpf_loader_incorrect_program_id` | `LoaderManagement` / `FoldedBranch` | 237,168,000 | 224,300,256 | 247,104,000 |
| `simplify_alt_bn128_syscall_error_codes` | `SyscallSemantics` / `FoldedBranch` | 274,320,000 | 278,300,256 | 308,448,000 |
| `abort_on_invalid_curve` | `SyscallSemantics` / `RuntimeBranch` | 311,904,000 | 300,764,256 | 342,576,000 |
| `deplete_cu_meter_on_vm_failure` | `VMFaultPolicy` / `RuntimeBranch` | 327,888,000 | 319,340,257 | 364,176,000 |
| `fix_alt_bn128_multiplication_input_length` | `SyscallSemantics` / `FoldedBranch` | 361,152,000 | 346,988,256 | 397,440,000 |
| `raise_cpi_nesting_limit_to_8` | `CPIExecution` / `RuntimeBranch` | — | — | — |
| `increase_cpi_account_info_limit` | `CPIExecution` / `FoldedBranch` | 403,056,000 | 385,868,256 | 435,456,000 |
| `poseidon_enforce_padding` | `SyscallSemantics` / `FoldedBranch` | 406,080,000 | 385,868,256 | 438,048,000 |
| `fix_alt_bn128_pairing_length_check` | `SyscallSemantics` / `FoldedBranch` | 406,944,000 | 385,868,256 | 438,480,000 |
| `alt_bn128_little_endian` | `SyscallSemantics` / `RuntimeBranch` | 425,088,000 | 406,604,256 | 456,192,000 |
| `enable_alt_bn128_g2_syscalls` | `SyscallSemantics` / `RuntimeBranch` | 425,520,000 | 406,604,256 | 457,056,000 |
| `loader_v3_minimum_extend_program_size` | `LoaderManagement` / `RuntimeBranch` | 432,864,000 | 416,540,256 | 470,880,000 |

لا يدّعي هذا النطاق عمدًا تغطية `FeatureSnapshot` كاملة في Agave. يضم NeverD
بوابات loader وverifier وVM وentry/input وsyscall وCPI infrastructure فقط عندما
تغيّر decoding أو host contract المولّد مباشرة. أما transaction scheduling
والfees والconsensus والتحقق من precompile على مستوى transaction ودلالات أعمال
`CPI target built-in` فتخص `external runtime`؛ وإضافة بتاتها بلا تنفيذ تلك
built-ins ستعلن قدرة لا يملكها NeverD.

ويسجل `SBFLoaders.def` الملكية والتسلسل. فالنشر والتنفيذ لم يعودا الجواب نفسه
منذ سنوات: يرفض `loader-v1` و`loader-v2` كل تعليمة إدارة تصلهما ويواصلان تشغيل
البرامج التي يملكانها فعلًا، ولهذا يجب أن يبقى تسلسلهما مقروءًا.

| Loader | التسلسل | ينشر | ينفّذ |
|--------|---------|------|-------|
| loader-v1 | `abi-v0` | لا | نعم |
| loader-v2 | `abi-v1` | لا | نعم |
| loader-v3 | `abi-v1` | نعم | نعم |
| loader-v4 | `abi-v1` | لا | لا (أُزيل built-in) |

ويضع `SBFAccountLayout.def` كل حقل حساب في موضعه ضمن كل تسلسل. والاثنان لا
يختلفان في الحشو فحسب، بل يرتبان الحقول ترتيبًا مختلفًا: فعند الإزاحة ثلاثة يحمل
الشكل غير المحاذى أول بايت من عنوان الحساب، ويحمل الشكل المحاذى راية executable،
ولا شيء في القيمة يعلن أيّهما قُرئ. كذلك يشغل الحساب المكرر بايتًا واحدًا في
`abi-v0` وثمانية في `abi-v1`، وهو ما يزيح المرور على المدخلات كلها لا حقلًا
واحدًا.

وسؤال هل يُحَلّ الاستدعاء هو في الحقيقة ثلاثة أسئلة لا سؤال واحد، لذا يحمل
`SBFSyscallLifecycle.def` مدى استقرار التوقيع المنشور، ويحمل
`SBFSyscallRegistration.def` ما تبقى: في أي registry يظهر الـ syscall، وأي بوابة
تحكمه، وإلى أي اتجاه تشير تلك البوابة.
والاتجاه مهم لأن البوابة قد تسلب شيئًا كما تمنحه — فتفعيل `disable_fees_sysvar`
هو ما أزال syscall الخاص بـ fees sysvar — وقراءة بوابة سالبة على أنها مانحة
تقلب الجواب لكل الـ clusters دفعة واحدة. ويبقى `sol_alloc_free_` مسجّلًا للتنفيذ
على جانبي الحد. كان deployment يسجّله قبل
`disable_deploy_of_alloc_free_syscall`، ثم يرفضه عند slot التفعيل الخاص بكل
cluster وبعده. وقد طوى Agave المثبت جانب deployment النشط في بناء registry،
لكن NeverD يحتفظ بالبوابة كي يعطي profile تاريخي جواب ما قبل التفعيل.

وعلى runtime فعّل `simd-0321` يتلقى entrypoint أيضًا عنوان بيانات التعليمة في
`r2`. ويمثّله NeverD نوعًا قائمًا بذاته من القيم لا ثابتًا، لأن موضعه يعتمد على
الحسابات: فاختراع عنوان يجعل تحميلًا عبره يُبلَّغ عنه بوصفه حقل حساب مسمّى. وقبل
التفعيل يصل السجل صفرًا، والبرنامج الذي يقرؤه يقرأ صفرًا. لذلك تأخذ نقاط الدخول
المولدة بلغات LLVM وC وRust مخزن الإدخال وبيانات التعليمة معًا، لأن دالة لا يمكن
إعطاؤها الثاني لا تستطيع إعادة إنتاج برنامج يقرؤه.

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

# حدد الـ runtime الذي يتحدث عنه الجواب. لا شيء من ذلك موجود في ملف البرنامج.
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

تختار `--sbf-cluster` و`--sbf-slot` و`--sbf-loader` و`--sbf-purpose` ملف
الـ runtime. وتصف القيم الافتراضية mainnet-beta على حالها الراهن، تحت
`loader-v3`، لبرنامج منشور بالفعل. أما السؤال عن النشر بدل التشغيل فيبلّغ عن
الـ syscalls التي تمنع البرنامج من الوصول إلى الشبكة رغم أن الشبكة ستواصل
تشغيله.

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
| عناوين base58 في read-only data | تطابق `SBFKnownAddresses.def` و`SBFAnchorNamespaces.def`، أو ثابت ينشئه الكود |
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

استرجاع scratch موجّه بالطلب: لا يُنشأ fixed point لـ Solana CPI/PDA scratch
إلا عند وجود `scratch consumer` حقيقي؛ والبرامج التي لا تملك هذا المستهلك تتجاوز
`whole-CFG fixed point`. يعرّف `SBFAnalysisLimits.def` سياسة المضيف (`analysis policy`)
لا `protocol limits`: يحدّد `MaxModeledScratchBytes` مقدار 1,024 bytes لكل
`program point`، ويقدّر `ScratchFlowRetainedByteBudget` مقدار 8,388,608 في
`logical retained estimate`. عند تجاوز الميزانية يوسّع الاسترجاع صراحة إلى
`ScratchRecoveryPrecision::BlockLocal`. لا تُفقد إلا `cross-block must-facts`، وتبقى
`block-local replay` تبقى `sound`، مع إمكان استعادة `same-block stores`.
يطبع printer السطر الثابت
`recovery scratch-precision=block-local`، ولا يعيد widening أي
`half-converged must-facts`.

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

لا يحافظ على scratch إلا syscall محلول من runtime، وذلك وفق نوافذ الكتابة
المراجَعة الخاصة به فقط. وكل استدعاء داخلي أو غير مباشر أو غير محلول يمحو
البايتات الممثلة، حتى إن لم تُشر أي وسيطة حالية إلى scratch، لأن مؤشرًا تسرّب
سابقًا أو alias عامًا قد يتيح للمُستدعى تعديلها. ويكتب
`sol_invoke_signed_rust` و`sol_invoke_signed_c` بيانات الحسابات لا ذاكرة
المستدعي، فتبقى استدعاءتان بُنيتا في block واحد مقروءتين معًا.

النموذج تحليل must أمامي على CFG داخل الدالة: لا يبقى البايت حيًا حتى block ما
إلا إذا كتب كل مسار يصل إليه القيمة نفسها. ولا تُتتبع حواف الاستدعاء لأن المُستدعى
لا يرث شيئًا من frame مستدعيه. ولا تملك worklist التبعيات مخرجًا يخفض الدقة وفق
عدد الـ block؛ إذ يختبر gate اختياري في Release كامل حد 10 MiB البالغ
`1,310,720` تعليمة.

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
#include <stdint.h>

typedef enum neverd_sbf_status {
  NEVERD_SBF_OK = 0,
  NEVERD_SBF_INVALID_INSTRUCTION = 1,
  NEVERD_SBF_MEMORY_ACCESS = 2,
  NEVERD_SBF_DIVIDE_BY_ZERO = 3,
  NEVERD_SBF_DIVIDE_OVERFLOW = 4,
  NEVERD_SBF_CALL_DEPTH = 5,
  NEVERD_SBF_UNKNOWN_SYSCALL = 6,
  NEVERD_SBF_UNKNOWN_FUNCTION = 7,
  NEVERD_SBF_EXECUTION_OVERRUN = 8,
} neverd_sbf_status;
/* v2 is fixed-width: values 0..8 reuse the legacy constants above. */
typedef uint32_t neverd_sbf_status_v2;
enum {
  NEVERD_SBF_INVALID_REGISTER = 9,
  NEVERD_SBF_INVALID_BRANCH = 10,
};
typedef uint64_t neverd_sbf_runtime_feature_mask;
typedef struct neverd_sbf_runtime_features {
  neverd_sbf_runtime_feature_mask bits;
} neverd_sbf_runtime_features;

/* Generated feature constants have the form NEVERD_SBF_RUNTIME_FEATURE_<Name>. */
typedef struct neverd_sbf_syscall_invocation {
  uint32_t hash;
  uint64_t arguments[5];
  neverd_sbf_runtime_features runtime_features;
} neverd_sbf_syscall_invocation;

/* v1 is the exact legacy four-field ABI. */
/* All callback fields return int, including the v2 callback. */
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  /* Legacy syscall callback: hash, five arguments, output value. */
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;

/* The v1 entrypoint reads only the four fields above. */
neverd_sbf_status neverd_sbf_program(
    neverd_sbf_environment *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);

/* v2 is a distinct ABI: the old layout is embedded and never extended in place. */
typedef struct neverd_sbf_environment_v2 {
  neverd_sbf_environment base;
  /* NULL callback falls back to base.syscall. */
  int (*syscall_with_features)(
      void *, const neverd_sbf_syscall_invocation *, uint64_t *result);
  /* NULL selects the program snapshot; a pointer to zero is an explicit empty snapshot. */
  const neverd_sbf_runtime_features *runtime_features;
} neverd_sbf_environment_v2;

neverd_sbf_status_v2 neverd_sbf_program_v2(
    neverd_sbf_environment_v2 *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);
```

`width` بوحدة البت. كل callback C مولّد يعيد `int`، بما في ذلك
`syscall_with_features`. في entrypoint v1 `neverd_sbf_program` يعني الصفر نجاحاً؛
وأي إرجاع غير صفري من `load` أو `store` يُطبّع إلى
`NEVERD_SBF_MEMORY_ACCESS`، وأي إرجاع غير صفري من `syscall` إلى
`NEVERD_SBF_UNKNOWN_SYSCALL`؛ ويمثّل ذلك العقد `v1-load-store-nonzero` و
`v1-syscall-nonzero`؛ ولا يمرّر v1 حالة callback الدقيقة.
وتُطبّع أخطاء `InvalidRegister` و`InvalidBranch` الداخلية أيضاً إلى
`NEVERD_SBF_INVALID_INSTRUCTION` ضمن `internal-invalid-instruction`.
أما entrypoint v2 `neverd_sbf_program_v2` فهو مسار الحالات الدقيقة: قيمة callback
المعروفة من `neverd_sbf_status_v2`، بما في ذلك 9 أو 10، تُحفظ كخطأ مُعالَج ضمن
`v2-exact-status`.
كما يحفظ entrypoint v2 أخطاء `InvalidRegister` و`InvalidBranch` الداخلية كـ9 و10.
وتستخدم قيمة callback غير المعروفة fallback الخاص بالعملية المولّدة ضمن
`operation-specific-fallback`. وإذا كان `syscall_with_features` فارغاً، يعود إلى
`base.syscall`، مع بقاء نوع callback هو `int` ضمن `feature-aware-null-base-syscall`.
يبقى struct وentrypoint v1 متوافقين مع المضيفين legacy. استخدم entrypoint v2 المنفصل
لاستقبال `syscall_with_features` وsnapshot ميزات runtime المحلولة. وتمثّل الشفرة
المولّدة السجلات وreturn PCs وcallee-saved r6-r9 وframe pointers وعناوين VM وأخطاء
القسمة وعمليات PQR الواسعة وwrapping shifts. ولا تُصدر إلا helpers المستخدمة فعلاً،
لذلك ينجح الناتج الأدنى مع `clang -Wall -Wextra -Werror`.

## عقد Rust host المولد

```rust
// The v1 source contract remains Result-based.
pub enum SbfError {
    InvalidInstruction, MemoryAccess, DivideByZero, DivideOverflow,
    CallDepth, UnknownSyscall, UnknownFunction, ExecutionOverrun,
}

#[repr(u32)]
#[non_exhaustive]
pub enum SbfErrorV2 {
    InvalidInstruction = 0, MemoryAccess = 1, DivideByZero = 2,
    DivideOverflow = 3, CallDepth = 4, UnknownSyscall = 5,
    UnknownFunction = 6, ExecutionOverrun = 7, InvalidRegister = 8,
    InvalidBranch = 9,
}

pub struct SbfRuntimeFeatures { bits: u64 }
impl SbfRuntimeFeatures {
    pub const fn from_bits(bits: u64) -> Self { Self { bits } }
    pub const fn bits(self) -> u64 { self.bits }
    pub const fn contains(self, feature: Self) -> bool {
        (self.bits & feature.bits) != 0
    }
}

pub struct SbfSyscallInvocation {
    pub hash: u32,
    pub args: [u64; 5],
    pub runtime_features: SbfRuntimeFeatures,
}

pub enum SbfSyscallOutcomeV2 {
    Unregistered,
    Returned(u64),
    Fault(SbfErrorV2),
}

pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}

pub trait SbfEnvironmentV2 {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfErrorV2>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfErrorV2>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfErrorV2> {
        let _ = (hash, args);
        Err(SbfErrorV2::UnknownSyscall)
    }
    fn syscall_outcome(&mut self, hash: u32, args: [u64; 5])
        -> SbfSyscallOutcomeV2 {
        match self.syscall(hash, args) {
            Ok(value) => SbfSyscallOutcomeV2::Returned(value),
            Err(SbfErrorV2::UnknownSyscall) => SbfSyscallOutcomeV2::Unregistered,
            Err(error) => SbfSyscallOutcomeV2::Fault(error),
        }
    }
    // Some(SbfRuntimeFeatures::from_bits(0)) is an explicit empty snapshot.
    fn runtime_features(&self) -> Option<SbfRuntimeFeatures> { None }
    fn syscall_with_features(
        &mut self, invocation: SbfSyscallInvocation
    ) -> SbfSyscallOutcomeV2 {
        self.syscall_outcome(invocation.hash, invocation.args)
    }
}

pub fn neverd_sbf_program<E: SbfEnvironment>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfError> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated program body")
}
pub fn neverd_sbf_program_v2<E: SbfEnvironmentV2>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfErrorV2> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated v2 program body")
}
```

يبقى entrypoint القديم `neverd_sbf_program` و`SbfEnvironment` عقد
`v1-result-abi`؛ وتستخدم واجهات المضيف `Result`. تمثّل
`Some(SbfRuntimeFeatures::from_bits(0))` العلامة `explicit-empty-snapshot`، وهي
مختلفة عن `None`. ويمثّل `syscall_outcome` جسر `result-host-bridge` من طريقة
المضيف القائمة على Result إلى `SbfSyscallOutcomeV2`. وبما أن `SbfErrorV2`
موسوم بـ`#[non_exhaustive]`، يجب على المستدعين استخدام
`non-exhaustive-wildcard` (`_`) عند مطابقته.

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
/* الـ runtime الذي يتحدث عنه الجواب. تصف القيم الافتراضية mainnet-beta على
   حالها الراهن، تحت loader-v3، لبرنامج منشور بالفعل. */
neverd_sbf_set_cluster(session, "devnet");
neverd_sbf_set_slot(session, 474768000);
neverd_sbf_set_loader(session, "loader-v3");
neverd_sbf_set_purpose(session, "deployment");
/* Optional: name Anchor handlers from the program's own IDL. */
neverd_sbf_set_idl(session, idl_json);
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

## خط أساس المطابقة الحالي (2026-08-24)

بعد تطبيق relocation تصبح `ProgramImage` واحدة غير قابلة للتغيير ومفهرسة بعناوين
VM هي مصدر الحقيقة المشترك للـdecoder وinterpreter واستعادة strings وواجهات
LLVM/C/Rust. لا توجد نسخ مستقلة من text أو rodata يمكن أن تنحرف عن loader.

توجد السجلات المغلقة في `SBFVersions.def` و`SBFOpcodes.def` و
`SBFRelocations.def` و`SBFArgumentRegisters.def` و`SBFVersionFeatures.def`, `SBFProtocolLimits.def` و
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
| manifest ELF الرسمي | 23/23 artifact من `sbpf/tests/elfs` |
| oracle الرسمي | `NeverDSBFExternalOracleTests` يطابق 1,411 حالة opcode/boundary مع verifier المثبت |
| differential execution | raw-byte oracle مقابل LLVM ORC وC11 وstable Rust، مع memory/fault/syscall trace |
| التجميع المتكامل | هدف `check-neverd-sbf` يشغّل كل suites المسجلة؛ لا نثبت عدداً تجميعياً سريع التغيّر |
| ASan + UBSan | الأهداف المركزة تعمل بإعدادات fail-fast بلا report؛ لا نثبت عدداً تجميعياً سريع التغيّر |

المراجعة مثبتة على Anza `sbpf` revision
`2510663bb8d894e8e3094be351e4bb4b604f1f84` وAgave
`ef210d67f2fabeee1730498188fa78854260c679`. لتحديثها راجع
`SBFUpstreamManifest.def` و`SBFUpstreamOpcodes.def` و`SBFUpstreamSources.def`
ثم شغّل:

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

أظهرت المقارنة أن `sol-azy` يتعطل على ELF strict الحالي ويحتفظ بعقدة CFG legacy
غير معرّفة؛ `solana-data-reverser` يركز على account data، و`SolDragon` يصف
التحليل كعمل قيد التطوير، و`bn-ebpf-solana` يتطلب Binary Ninja. لذلك يبقى
`sbpf` وAgave الرسميان المرجع الدلالي.

## عقد الأدلة المدققة في 2026-08-24

تثبت `SBFUpstreamSources.def` المراجعة على Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84` وAgave
`ef210d67f2fabeee1730498188fa78854260c679` وSolana SDK
`122f32e571ce39face4beffaccea733e37c207fd`. يمر manifest الرسمي كله 23/23،
ويقارن `NeverDSBFExternalOracleTests` عدد 1,411 من حالات opcode/boundary مع
verifier الرسمي المستقل عبر `SBFOfficialOracleProtocol.def` و
`SBFOfficialVerifierCases.def` و`SBFOfficialExecutionConstants.def`. تأتي حالات ELF المعطوبة من
`SBFOfficialELFMutations.def` وcorpus جدولي؛ لا نثبت عدداً إجمالياً سريع التغير.
وبشكل مستقل يشغّل `41-case فرق ELF الصارم` كامل مصفوفة strict-v3 عبر
العملية الرسمية `verify-elf-batch` وNeverD؛ ولا تدخل الحالات الـ41 في مجموع 1,411.

مصفوفة التنفيذ الإضافية الرسمية (`additional execution matrix`) منفصلة: تضم
بالضبط 508 حالة نشطة من نوع `(Version,Opcode)`، إضافة إلى 58 حالة حدودية، أي
566 حالة تنفيذ دقيقة. ولا تستبدل هذه المصفوفة ولا تُحتسب ضمن 1,411
`verifier probes` أو فرق ELF الصارم ذي 41 حالة.

ويتحقق `NeverDSBFAgaveConformanceTests` أيضاً من revision الخاصة بـFiredancer
test-vectors `68bb4af40235562e8852fa23d5727e49c2a0b862` ويطابق كل fixtures البالغ عددها
1,955 من `sol_compat_elf_loader_v1` (قبول 1,399 ورفض 556). ولكل ELF مقبول يقارن
أيضاً `entry_pc` و`text_off` و`text_cnt` و`rodata_hash` و`calldests_hash`.
تختبر هذه gate الـloader وحده عمداً ولا تشغّل instruction verifier اللاحق حتى
تبقى مرحلتا Agave منفصلتين.

يبقى chain profile الافتراضي صادقاً مع Agave: تستخدم rows باسم
`SBF_RUNTIME_VERSION` حساب cluster/slot التاريخي لرفع maximum ISA من V0 إلى V1
ثم V2 ثم V3 عند تفعيل feature accounts الرسمية؛ والحد الحالي V3. يتم ذلك عبر
`RuntimeVersionPolicy::ChainProfile`. أما طلب `--sbf-version=v4` الصريح فيختار
`RuntimeVersionPolicy::UpstreamToolchain` للتحليل offline وفق `sbpf` المثبت، من
دون الادعاء بأن v4 مفعل على السلسلة. الحد الحالي 10 MiB هو بالضبط
`10'485'760` byte؛ والقيمة 65,536 محفوظة كـprovenance/test تاريخي فقط ولا تُنفذ.

السجلات typed `.def` هي سلطة الميزات وsyscalls وfaults وsource ABI:
`SBFSyscallRegistration.def` و`SBFValidationRules.def`, `SBFFaultCodes.def` و`SBFSourceStatuses.def`
و`SBFArgumentRegisters.def` و`SBFEdgeKinds.def`. يثبت `SBFFaultCodes.def` قيم
execution fault، بينما يملك `SBFSourceStatuses.def` طبقة generated-source ABI
المستقلة. يعمل loader بأسلوب `raw-first`:
يثبت relative CALL، ثم relocations الخام مرة واحدة حسب ELF ordinal، ثم يتحقق
بالترتيب من text identity وCALL والـrelocation والـentrypoint وread-only layout.
التحويل بين file offset وVM address هو gap-aware ولا يخترع bytes داخل الفجوات.

CFG وdataflow خاصان بكل function؛ call edge ليس predecessor محلياً، والـshared
tail يبقى ambiguous، وكل latches لحلقة واحدة تكوّن multi-latch region واحدة.
تثبت worklist وملكية blocks باختبارات 10,000 function وreverse-order block
وconditional latch، بلا تخمين زمن خاص بجهاز.

تستخدم call graph العامة لـSBF سياسة `callgraph-budget=fail-closed`: تجعل حدود
input وprovenance وnode وedge وelement و`CallGraphOutputByteBudget` نتيجة JSON
كاملة أو فارغة. عند نفاد الميزانية تعيد `{"nodes":[],"edges":[]}` وتضبط
`neverd_last_error()`، ولا تنشر علاقة جزئية.

كل activation row يحمل cluster وfeature account وslot، لذلك يمكن إجراء
`RPC activation audit` مقابل node حي مع بقاء التحليل العادي offline. شملت
المقارنة Blueshift و`qedsvm` (إثبات Lean لمسار مختار، لكن ELF loader حالياً V0
فقط) و`leanprover-solanalib` و`sol-azy` و`bn-ebpf-solana` وGhidra/SolDragon.
ويعرّف `ezBPF` عند `88829078a6d7682a2baed0d696d500401c263750` نفسه كأداة
deprecated ويوجّه إلى Blueshift؛ إنه predecessor مؤرشف ذو خريطة byte-to-enum
واحدة، لا decoder واعٍ بإصدارات moved-memory وJMP32 ومصفوفة v0-v4 الحالية.
وتثبت المراجعة أيضاً رؤوس المقارنة: Blueshift عند
`704e40f7aa82446555b19d9ffbc0a6e18a35480f`، و`qedsvm` عند
`99bd5ede85374adc7fc5c835c2432ecf4e123fd1`، و`leanprover-solanalib` عند
`6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec`؛ كما تثبت pins الأدوات الأربع
`sol-azy` عند `362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`، و`solana-data-reverser`
عند `bf90923adec984a61ca0437e9d341360ac1b11ee`، و`SolDragon` عند
`002b98677a5e595a773af6607b77210f5ea71db7`، و`bn-ebpf-solana` عند
`c3fe0de45d37eb68dcb08f2498c6e1f986056572`.
ضمن أدوات SBF العامة المفتوحة التي دُققت في هذه اللقطة، يمتلك NeverD أقوى
دليل قابل لإعادة الإنتاج وجدناه؛ هذا ادعاء مقارنة محدود، لا ادعاء «الأول عالمياً» مطلقاً.

وتشمل لقطة المقارنة أيضاً `r2ghidra-solana` عند
`eca0b8e2d307e00991e289b8f9b0f45743819f1b`، الذي يقدم تجربة Ghidra ذات
`C-like-pdg` وإظهاراً للحسابات وAnchor وstrings وsyscalls؛ نجح CI عند هذا الـpin،
لكن اختبارات Solana الخاصة به معلّقة، وsmoke في CI يفكك `/bin/ls` فقط. ويؤكد
reproducer المباشر أن official `relative_call_sbpfv0.so` لـV0 ينتج C معقولاً،
بينما يفشل official V3 `relative_call.so` في `pdg`؛ وهذه نتيجة قابلة لإعادة الإنتاج.
ويأتي
`radare2-solana` عند `292d845681be377cadc9959a74c2cadeb6e7f412`؛ إذ يوسّع
SIMD-0173/0174 الخاصة بـV2 إلى `>=V2` حتى V3/V4، مع أن `program.rs` الرسمي
يصرح بأنها V2 فقط. أما `SBPF-3-1` عند
`0e602c93007faa96bccb8e1e12040954ff108b6f` فاختبارا cargo لديه 2/2 بسيطان بلا
CI؛ كشف الإصدار placeholder يعيد none/V0، وdecoder high-nibble للـopcode خاطئ،
والـjump يستخدم imm بدل off، كما أن ELFين relative_call لـV0/V3 يعطيان pseudocode
الخاطئ نفسه. يظل تفوق NeverD هنا دليلاً رسمياً قابلاً لإعادة الإنتاج للـloader و
verifier وruntime وprocess-oracle عبر V0–V4، مع الاعتراف بجودة UX وC output لدى الأدوات.

يمثل `SBFComparisonTools.def` المصدر الوحيد لأسماء العرض وrevision الكاملة لأدوات
المقارنة. وأضاف المسح العام النهائي والمحدود النتائج التالية:

- ثُبّت `blastrock/Solana-eBPF-for-Ghidra` عند
  `c3ad719004726fe924dbed901eca2744ad82c85d`. وهو يقدم Ghidra P-code UX حقيقية،
  لكن نموذج SLEIGH واحداً غير واعٍ بالإصدار يثبت CALLX على `dst` ويمزج opcodes
  legacy/current. لا توجد اختبارات حقيقية أو CI، كما يفتقد المصدر الافتراضي class
  لثوابت relocation يُشار إليه.
- ثُبّت `SolEmu-Ghidra` عند `6520af2ff104d5adbec24632ba3afa3bef0da529`؛ يرث
  decoder المطابق ويضيف emulator UX حول سلوك CPI وcrypto وZK الموصوف صراحة بأنه
  simulated أو placeholder، من دون اختبارات حقيقية أو CI. أما `Ghidra_sBPF` عند
  `907bd4476432ca83bb2352686ad1ccafdb38504c` فيتيح اختيار v1-v3 يدوياً، لكنه
  يراكم encodings الخاصة بـV2 داخل V3، بلا اختيار آلي لـV0/V4 ولا اختبارات أو CI.
- `solana-ebpf-ida-processor` عند
  `aacd215907266190ed9c6c1b408ca9309f92ecdd` هو IDA disassembler/relocation UI
  مفيد وليس source lifter؛ وجدول opcode المختلط يقرأ CALLX دائماً من `imm`، ولا
  اختبارات أو CI. ويولد `solana-bpf-reverse` عند
  `39479a3bddb8cb866ee499266a76a1b54069b222` تقارير heuristic وRust TODO scaffold
  من تخمينات layout ثابتة؛ أعطى التشغيل 9 pass و2 fail و1 skip، بلا CI.
- `solens` عند `22defa1c8f4118dacd42f5c291f1ac31609fc0e5` هو terminal disassembler
  خاص بـV2، مع 0 test وبلا CI. أما `sbpf-decompiler` عند
  `37b8bc0edc7ce347abee466f5f974e900c1948df` فتنفيذه الحالي ثلاثة أسطر
  `Hello, world!` فقط، مع 0 test وبلا CI.
- `sbpf-eye` عند `5277a52aeb58e50b6ff8f9020414334765369b49` يعرّف نفسه بوضوح
  كـlightweight WIP instruction/CFG TUI؛ تمر 3 اختبارات لكنه بلا semantic IR أو
  source emitter أو CI. و`svm_bytecode_analyzer` عند
  `12aa236db8964e6be661e38131c2dc81588cf19c` هو disassembler/CFG analyzer لا lifter؛
  يفك bytes الخاصة بالـregister/offset خطأ، وأعطى التشغيل 17 pass و1 fail، بلا CI.
- `giraffexiu/Solana-eBPF-for-Ghidra` عند
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07` هو snapshot من commit واحد للسلالة
  نفسها في Ghidra، بلا semantics إصدار إضافية أو اختبارات أو CI. أما `CertSBF` عند
  `bb93a97cf0c64d119d08ec851e8e820315beb59e` فهو formalization قيّم بـIsabelle/HOL
  لدلالات rBPF الأقدم، وليس whole-program source decompiler حديثاً لـV0-V4.

تعزز هذه النتائج دليل المقارنة داخل العينة العامة المحدودة فقط، ولا تمثل حكماً
مطلقاً على أدوات مستقبلية أو مشاريع خاصة.

وتطابقت مراجعة RPC النهائية في 2026-08-24 تماماً: 38 feature accounts و89
activation rows؛ mainnet عند slot 441305159، وtestnet عند 433055669، وdevnet عند
487238699. أما الحساب الفارغ المعلّق المملوك للنظام (`VirtualAddressSpaceAdjustments`
على mainnet) فلم يُفعّل. ولا تُثبّت هذه النتيجة عنوان RPC في الوثائق.

تقرأ Linux Release CI الـpins الدقيقة عبر `--print-pinned-revision` و
`--print-test-vectors-revision` و`--print-toolchain`، وتصادق على oracle الرسمي
وsparse corpus، وتصدر `NEVERD_SBPF_ORACLE` و`NEVERD_AGAVE_CONFORMANCE_ROOT`،
لذلك يكون الاختباران الخارجيان إلزاميين فيها. أما التشغيل المحلي العادي بلا
env صريح للـoracle/corpus فيكتشف الحالات ويجوز أن يتخطاها.
