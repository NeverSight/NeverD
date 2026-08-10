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
