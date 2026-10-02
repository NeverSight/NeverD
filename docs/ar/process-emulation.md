**اللغات**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](process-emulation.md)

[← فهرس التوثيق](README.md)

# محاكاة عمليات الضيف

ينفذ `neverd emulate` صورة ضمن ملف صريح لنظام الضيف. لمحرك CPU وتحليل الصورة ودخول العملية وخدمات نظام التشغيل ملاك منفصلون. فعّل `NEVERD_ENABLE_CPU_EMULATION=ON`؛ وتمكين محاكاة برامج التشغيل يتضمنه أيضاً.

أول ملف هو `linux-elf64-v1`: يشغّل برامج ELF `ET_EXEC` وبرامج static PIE ذاتية الترحيل من نوع `ET_DYN` لـx64 وAArch64 عند CPL3 أو EL0. يحمّل المقاطع الحقيقية ويبني المكدس الابتدائي ويستأنف التنفيذ على دفعات ويعالج طلبات Linux الصريحة. هذا نموذج عملية مستقل، لا توزيعة Linux كاملة ولا وعد بتشغيل ملفات libc عشوائية. الربط الديناميكي والإشارات والخيوط وأنظمة الملفات والخدمات غير المدعومة تفشل صراحةً.

<!-- i18n-section: cli-sdk -->

## CLI وSDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

على مضيف Linux مطابق يختار KVM، وعلى Windows مطابق يختار WHP؛ وتستخدم تراكيب المضيف/الضيف الأخرى Unicorn. عدم توفر الخلفية المختارة خطأ بلا رجوع صامت. يظل نموذج Linux مستخدماً حتى عند تشغيل ELF على Windows. راجع [تنفيذ CPU](cpu-execution.md) لقائمة التعليمات والقيود.

يصدر CLI تقرير JSON واحداً: رمز الخروج 0 لحالة ضيف صفر، و2 لحالة أخرى، و3 لتنفيذ غير مكتمل (بما فيه الأعطال والحدود)، و1 لإعداد/API غير صالح. حالة الضيف الفعلية في `exit_status`. نقطة C المضافة هي [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h)؛ تتطلب جلسة ومساراً غير فارغ وملفاً صريحاً وخيارات اختيارية. حرر النتيجة عبر `neverd_free_string`؛ النتيجة NULL تعني فشل الإعداد ويشرحها `neverd_last_error`. عطل الضيف أو توقف الموارد يعيدان تقريراً، ولا تتطلب صورة التحليل المحملة في الجلسة ولا تتغير.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## الخيارات والنتائج

خيارات JSON كائن لا يتجاوز 64 KiB. الحقول المجهولة أو null والأنواع غير الصحيحة وNUL المضمّن والحدود غير الموجبة مرفوضة.

| الخيار | الافتراضي | العقد |
|---|---|---|
| `backend` | `auto` | `auto` أو `unicorn` أو `kvm` أو `whp` |
| `arguments` | اسم الإدخال | argv كامل مع argv[0]؛ الفارغ يستخدم الافتراضي |
| `environment` | `[]` | سلاسل الضيف فقط؛ لا يرث بيئة المضيف |
| `instruction_limit` | 100000 | محاولات التعليمات المقبولة المشتركة |
| `event_limit` | 10000 | أحداث system-call؛ تُخصم قبل معالجة الخدمة |
| `timeout_microseconds` | 5000000 | موعد رتيب يبدأ بعد إعداد العملية |
| `memory_limit` | 67108864 | ميزانية الذاكرة الفعلية/المربوطة |
| `stack_size` | 1048576 | مكدس بمحاذاة الصفحة ضمن الميزانية |
| `output_limit` | 1048576 | مجموع stdout/stderr الملتقط |
| `instruction_quantum` | 1024 | فاصل القبول قبل إفساح المجال للمشغل |

`schema_version` يساوي 1. يتضمن التقرير الملف والمعمارية والخلفية المختارة وسببها و`stop_reason` و`exit_status` القابل لـnull والتشخيص وعناوين PC والعدادات وسجلات الخدمة وآخر نتيجة CPU ذات نوع. العناوين وأرقام الخدمات والسجلات والقيم الخام سلاسل سداسية **بلا** `0x` كيلا تفقد الدقة في JSON؛ و`stdout_hex` و`stderr_hex` يحفظان NUL وUTF-8 غير الصالح. نتيجة syscall بقيمة null تعني عدم وجود نتيجة ممذجة، مثل الخروج أو طلب غير مدعوم، ولا تعني صفراً ناجحاً.

<!-- i18n-section: linux-semantics -->

## دلالات ملف Linux

يستخدم نموذج نظام التشغيل رؤوس البرامج التي فكها محمل ELF الموجود. تتحقق السياسة من وسوم ABI ومحاذاة المقاطع وجداول رؤوس البرامج المربوطة وحدود عناوين المستخدم. تفحص خطة الربط النطاقات والصلاحيات والتداخل والميزانية قبل التخصيص، ولا تنشر إلا فضاء عناوين خاصاً جاهزاً بالكامل. تحفظ المقاطع بادئة/ذيل صفحات الملف، وتصفّر BSS، وتحترم الصلاحيات وتحجز فجوات حارس للمكدس؛ وتُرفض تخطيطات تداخل الصفحات والرؤوس المتعارضة بدلاً من التخمين.

يستخدم static PIE قيمة load bias حتمية لا تقل عن `0x40000000` وتزداد لمراعاة محاذاة `PT_LOAD`. تستخدم المقاطع وPC الدخول و`AT_PHDR`/`AT_ENTRY` القيمة نفسها، وتبقى قيم ترويسات الملف الأصلية بلا تغيير؛ يظل `AT_BASE` صفراً لعدم وجود مفسر. مصدر الربط هو بايتات الملف الأصلية، لا مقاطع التحليل المعدلة، وعلى بدء الضيف تنفيذ relocation والتهيئة بنفسه. يفك المحمّل `PT_DYNAMIC` من سجلات الملف الأصلية المحدودة دون الاعتماد على section headers؛ ويشترط النموذج جدولاً مقروءاً ومنتهياً لا يتجاوز 4096 إدخالاً. يُرفض `PT_INTERP` وتبعيات الربط الخارجية ووسوم filter/audit؛ لا يوفر النموذج dynamic linker أو حل الرموز أو تشغيل المنشئات.

يحتوي المكدس الابتدائي argc/argv/envp/auxv بمحاذاة، وPHDR/PHENT/PHNUM وentry وحجم الصفحة وقيماً ثابتة للهوية. PID/TID/UID/GID النموذجية تساوي 1000. أول 16 بايتاً من SHA-256 للمدخل هي `AT_RANDOM` لضمان قابلية التكرار؛ وهذه سياسة نموذج حتمية لا عشوائية تشفيرية. HWCAP/HWCAP2 صفريان ولا يوجد vDSO.

الخدمات المنفذة هي `write` و`exit` و`exit_group` و`getpid` و`gettid`, `mmap`, `mprotect`, `munmap`, `brk` مع أرقام منفصلة لـ[x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) و[ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). عودة SYSCALL على x64 تطبق clobber لـRCX/R11 إضافة إلى RAX وPC التالي؛ يستخدم ARM64 x8 للرقم وx0 للنتيجة. الطلبات الأخرى تتوقف كـ`unsupported_service` ولا تنفذ system calls على المضيف.

تُتحقق قوالب TLS الساكنة `PT_TLS` كحقائق من المحمّل: قالب واحد، وحدود ملفات/ذاكرة محدودة، وتوافق المحاذاة وبايتات أولية مقروءة. يخصص بدء الضيف كتل TLS ويهيئها ويركب مؤشر الخيط؛ لا يخترع نموذج Linux بنية TCB أو DTV خاصة بـlibc. هذا يدعم local-exec TLS المولد من المترجم في البرامج المستقلة. TLS الديناميكي وجدولة خيوط OS خارج النطاق.

على x64 تدعم `arch_prctl` عمليات `ARCH_SET_FS` و`ARCH_GET_FS` و`ARCH_SET_GS` و`ARCH_GET_GS`. يقبل Set قاعدة ضمن نطاق المستخدم حتى إن لم تكن مربوطة؛ ويظل الوصول اللاحق خاضعاً للصلاحيات. قاعدة نطاق النواة تعيد `EPERM`، ومؤشر Get غير الصالح يعيد `EFAULT` دون fault للـCPU. العمليات الأخرى غير مدعومة وتفشل صراحة. يثبت ARM64 `TPIDR_EL0` بتعليمة `MSR`؛ وتحافظ `MRS` ومراجع FS/GS واستعادة السياق على مؤشر كل خيط بين الدفعات ومداخل المحرك. هذا لا ينشئ مجدولاً للخيوط.

الواصفان 1 و2 مصرفا بايتات افتراضيان. يتحقق `write` من صفحات المستخدم المقروءة؛ يعيد بادئة قابلة للقراءة إن تعذر الوصول إلى صفحة لاحقة، و`EFAULT` إن لم تكن أي بايتات مقروءة. الواصف الخاطئ يعيد `EBADF`، والكتابة ذات العدد صفر على واصف صالح لا تفحص المؤشر. لا يحاكي ذلك ذرية أنابيب Linux أو الملفات. حد الإخراج يوقف التنفيذ قبل نشر كتابة تتجاوزه.

تشارك خدمات الذاكرة المجهولة الصورة والمكدس في فضاء عناوين العملية وميزانية الذاكرة الفعلية. يقبل `mmap` حصراً `MAP_PRIVATE | MAP_ANONYMOUS` مع `PROT_NONE` أو `PROT_READ` أو `PROT_READ | PROT_WRITE` أو `PROT_READ | PROT_EXEC` أو RWX القابل للقراءة. تُحترم التلميحات الحرة المحاذاة للصفحة؛ وإلا يبدأ البحث عن فجوات من `0x100000000` ثم من أدنى عنوان مستخدم، مع حجز حواجز المكدس. هذه سياسة حتمية لا تحاكي Linux ASLR. الصفحات الجديدة مستقلة ومصفّرة، لذلك قد يسترجع إلغاء جزئي الصفحات غير المثبتة. قد يُبقي إسقاط CPU أو عرض backing محتفظ به تخصيصاً متقاعداً حياً حتى انتهاء عمره.

تُقرّب الأطوال إلى الصفحات. يتسامح `munmap` مع الفجوات والإزالة المتكررة؛ ويغيّر `mprotect` البادئة المعيّنة قبل إعادة `ENOMEM` عند فجوة. يحفظ `PROT_NONE` التخصيص والبايتات مع منع وصول الضيف. يعيد `brk` الخام الحد المطلوب عند النجاح والحد السابق عند الفشل، وليس اتفاقية صفر/ناقص واحد لغلاف libc. الحد الأولي هو نهاية الصورة المحاذاة للصفحة. يحترم النمو التعيينات الأخرى والميزانية، ويحفظ التقليص بايتات الصفحة الجزئية الباقية. تتبع القواعد وأولوية الأخطاء خدمات Linux [للتعيين](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c) و[الحماية](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c).

تعيينات الملفات والمشاركة والتثبيت، والنمو للأسفل، والصفحات الضخمة، وقفل الذاكرة، ومفاتيح الحماية، وسياسات التنفيذ فقط أو الكتابة فقط، والأعلام الأخرى غير مدعومة صراحةً: تتوقف قبل نشر آثار أو اختلاق قيمة إرجاع. تعيد أخطاء النطاق والطول والمحاذاة المعتادة ضمن الجزء المدعوم أخطاء للضيف وتسمح باستمرار التنفيذ. لا تُمرّر أي مؤشرات ضيف أو طلبات تعيين إلى نظام المضيف.

<a id="windows-pe64-profile"></a>

<!-- i18n-section: windows-pe64 -->

## ملف Windows PE64

يدعم `windows-pe64-v1` عمليات Windows x64/ARM64 محدودة لوحدة التحكم مع PEB/TEB وTLS لملف EXE وواجهات Win32 مسماة ورسوم DLL بدء صريحة بلا دورات. تدعم DLL استيراد الشيفرة والبيانات بالاسم أو الرقم وإعادة التموضع DIR64 وهويات حقيقية بقوائم المحمّل. ما زالت مداخل/TLS ملفات DLL والتحميل الديناميكي والتصدير المحوّل وCRT/GUI وSEH المستخدم والخيوط غير مكتملة؛ وتغيب أدلة ARM64 KVM/WHP الأصلية.

تضيف الذاكرة الافتراضية في Windows دعم `VirtualAlloc` و`VirtualFree` و`VirtualProtect` و`VirtualQuery` و`FlushInstructionCache` للعملية الحالية. تدير طبقة OS الحجوزات، وتبقى `AddressSpace` المرجع للصفحات الملتزم بها والصلاحيات والتخزين الفعلي. تشمل الاختبارات تعديل الشيفرة وأخطاء الوصول وإعادة استخدام ميزانية الذاكرة.

تدعم التخصيصات الخاصة `MEM_RESERVE` و`MEM_COMMIT` و`MEM_DECOMMIT` و`MEM_RELEASE` و`MEM_TOP_DOWN`، بمحاذاة حجز 64 KiB وصفحات 4 KiB. الحجز وحده لا يستهلك RAM الضيف. تحافظ إعادة الالتزام على البيانات وتحدّث الصلاحيات، ويلغي فك الالتزام تخزين كل صفحة. يمنع فحص النطاق كاملاً وتجهيز التخصيص مسبقاً التغييرات الجزئية عند الأخطاء العادية. يعيد الاستعلام بنية x64/ARM64 بحجم 48 بايت ويجمع الصفحات التالية ضمن التخصيص نفسه فقط. تشمل سياسة المواضع الصورة والبيئة والكومة ومداخل API وحدود المكدس؛ وتتفق هوية تخصيص المكدس مع TEB. إذا جعل استدعاء `VirtualProtect` الناجح موضع إخراج الصلاحيات السابقة للقراءة فقط، تبقى الصلاحيات الجديدة سارية وتظل بايتات الإخراج دون تغيير، ويعيد الاستدعاء النجاح. يعيد فشل تعديل صلاحيات نطاق يحتوي صفحات غير ملتزم بها `ERROR_INVALID_ADDRESS`، ويكتب `PAGE_NOACCESS` في ناتج الصلاحيات السابقة دون تغيير صلاحيات الصفحات.

الصلاحيات المدعومة هي `PAGE_NOACCESS` و`PAGE_READONLY` و`PAGE_READWRITE` و`PAGE_EXECUTE_READ` و`PAGE_EXECUTE_READWRITE`. تبقى صفحات الحراسة والتنفيذ فقط والنسخ عند الكتابة وسمات التخزين المؤقت والصفحات الكبيرة وreset/write-watch/العناصر النائبة وتعديل خرائط وقت التشغيل المملوكة للنموذج غير مدعومة صراحةً. يمكن فك الالتزام أو التحرير للتخصيصات الافتراضية الخاصة فقط. لا تضيف هذه الميزة توزيع استثناءات المستخدم أو إثبات تنفيذ أصلي على عتاد ARM64.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

يبقى EXE من نوع PE32+ وأحادي الخيط عند عنوانه المفضل. يجب أن تكون مداخل DLL الصريحة صفراً ومن دون دليل TLS. يحدد `WindowsProcessOptions::Modules` أو JSON `windows.modules` حتى 64 اسماً أساسياً ضيفاً ومسار إدخال بواسطة `name` و`path` دون بحث أو تنفيذ DLL المضيف. لا تراعي أسماء ASCII حالة الأحرف؛ تُرفض الأسماء المكررة واستبدالات مزودي النظام وتُقرأ الملفات التي يصل إليها الرسم فقط. تربط الاستيرادات المسماة/الرقمية بالتصديرات الفعلية؛ تفشل الفجوات والرموز المفقودة والدورات والتحويل والاستيرادات المرتبطة/المؤجلة وload configuration/CFG غير المدعومة. يعالج DIR64 تعارض DLL القابلة للنقل؛ تفشل التعارضات الثابتة والكتابة في بيانات الربط قبل إنشاء CPU.

يمتلك `readPEProgramExports` هويات التصدير الأصلية ونطاقات القراءة، ويمتلك `WindowsProcessModules` الرسم وبوابات API المشتركة للعملية لكل مزود/اسم. يحجز `VirtualMemory` كل الصور قبل ربطها ويملك `AddressSpace` الصفحات والصلاحيات. تضم PEB/LDR الصور الحقيقية وترتب قائمة التهيئة DLL وفق الاعتماد. يقبل `GetModuleHandleW` قيمة NULL أو اسماً أساسياً ASCII دون مراعاة حالة الأحرف ويضيف `.dll` عند غياب الامتداد. لا تُدعم المسارات والأسماء غير ASCII وقواعد النقطة النهائية. يعيد الاسم المفقود الخطأ 126 ويحفظ النجاح LastError. نماذج API ليست DLL نظام مثبتة.

تخضع بايتات الإدخال الإجمالية ونطاقات الصور كل منهما إلى `memory_limit`؛ وتستهلك خرائط البيئة أيضاً ميزانية الصور. تشترك التهيئة في 65,536 سجلاً و64 MiB من قراءة البيانات الوصفية وحدود الأسماء والمهلة العامة دون ضمان صارم لزمن I/O المضيف. يتحقق المثال الأصلي EXE→DLL→DLL من التموضع والأرقام والبيانات المشتركة وهوية API و`MEM_IMAGE` والقوائم وattach/detach لـTLS EXE. يشمل `NeverDWindowsProcessTests` مرجع Windows الأصلي، و`NeverDPEProgramExportsTests` البيانات المشوهة والميزانية، و`NeverDProcessPublicTests` تطابق C ABI/CLI. تُسجل المحركات غير المتاحة كتجاوز صريح.

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

يشير GS في x64 وx18 في ARM64 إلى TEB الذي يحتوي حدود المكدس ومؤشر الذات وPID/TID وPEB والمعاملات وLastError وTLS. يتحول UTF-8 الصارم إلى UTF-16 ويقتبس argv وفق Microsoft CRT. أسماء البيئة ASCII وتُرفض الأسماء المكررة دون اعتبار حالة الأحرف؛ القيم تقبل Unicode والكتلة المرتبة تنتهي بصفرين NUL. لا تُورث بيئة المضيف أو ملفاته. ينسخ TLS الثابت القالب ويصفّر BSS ويكتب فهرساً من 32 بت؛ ويستخدم TLS الديناميكي خانات TEB مستقلة. تقرأ عمليتا attach/detach مصفوفة الاستدعاءات الحية بالترتيب ضمن ميزانية وموعد نهائي مشتركين. العودة من المدخل والخروج الطبيعي ينفذان detach؛ والخروج المتكرر أثناءه يتوقف صراحةً.

يحدد `WindowsProcessServices.def` واجهات `ExitProcess` و`RtlExitUserProcess` ومقابض الإخراج و`WriteFile` المتزامن وLastError ومعرفات ومقابض العملية/الخيط الوهمية و`GetCommandLineW` وتخصيص/تحرير/حجم الكومة وTLS الديناميكي و`GetModuleHandleW`. تُحل الأسماء الدقيقة من `kernel32.dll` و`kernelbase.dll` و`ntdll.dll` فقط. لا تختار syscall المباشرة أو بوابات الاستدعاء المزيفة نماذج API. الكومة مملوكة للعملية وتُستعاد عند التحرير؛ يحتفظ الإخراج بالبايتات الثنائية. تُفصل أخطاء Win32 عن الإدخال/الإخراج غير المتزامن واستثناءات المستخدم غير المدعومة. تراعي المؤشرات المتداخلة تصفير عداد الإكمال الأولي وعنوان العودة الفعلي.

يحفظ `windows.native_calls` اسم الوحدة/الدالة والمعاملات العددية المعلنة والنتيجة القابلة لـNULL دون اختراع أرقام NT. يختبر `NeverDWindowsProcessTests` ملفات PE فعلية وTLS المترجم وتغيير الاستدعاءات والكومة والتداخل والبيانات التالفة والصلاحيات والميزانيات؛ ويختبر `NeverDProcessPublicTests` واجهتي CLI/C ABI. تشغّل CI Windows ملف EXE نفسه مباشرةً كمرجع مستقل وتفرض اختبارات WHP. وما زال إثبات التنفيذ الأصلي ARM64 يتطلب جهازاً مناسباً.

عندما يكون مخزن الإدخال المؤقت غير فارغ وغير قابل للقراءة، تُرجع `WriteFile` الخطأ `ERROR_INVALID_USER_BUFFER` (1784)، وتصفّر عدد البايتات المكتوبة ولا تُخرج أي بايتات.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c).

<!-- i18n-section: verification -->

## التحقق

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# In a shared-library/CLI build:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

تترجم الاختبارات مداخل ELF مستقلة بلغة التجميع وC لكلتا ISA، وتتحقق من data/BSS وبيانات البدء الفعلية وأخطاء الاستدعاءات والمخرجات الثنائية والصلاحيات والكتابات الجزئية والخدمات غير المدعومة والميزانيات بين حصص التنفيذ. تهيئ حالات TLS كتلاً مستقلة محاذاة وBSS ومؤشرات خيوط وتتحقق من بقائها؛ ويفحص x64 أخطاء `arch_prctl` دون فقد القاعدة السابقة. تُسجل الخلفيات غير المتاحة كتجاوز صريح. تمر المجموعة العامة عبر C ABI المشتركة وCLI وتقارن التقرير ورمز الخروج. تتحقق PIE من auxv وخانات RELA الصفرية قبل ترحيل البيانات ومؤشرات الدوال ذاتياً. تختبر التعيينات حفظ fixup عند اختيار مصدر التحليل؛ وتغطي الجداول الديناميكية غياب الأقسام والمدخلات التالفة أو المعتمدة على خارجها. تغطي كلتا ISA التخصيص والحماية والفجوات وإعادة التعيين ونمو الكومة وتقليصها والأخطاء المعالجة. تفشل كتابات ضيف فعلية بعد تغييرات حماية عادية وجزئية. يعيد x64 كتابة الكود في العنوان نفسه بين RW وRX ويستدعي النسختين؛ ويعمل ELF نفسه أصلياً على Linux كمرجع مستقل للنتائج والأعطال. تختبر الذاكرة استنفاد الميزانية والاسترجاع ولقطات التعيين المرجعية دون الاحتفاظ بـRAM. الترجمة المتقاطعة وUnicorn ARM64 ليستا دليلاً على ARM64 KVM/WHP الأصلي.
