**语言**：[English](../process-emulation.md) | [简体中文](process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← 文档索引](README.md)

# 来宾进程模拟

`neverd emulate` 在明确指定的来宾 OS 配置下执行映像。CPU 传输、映像解析、进程入口和 OS 服务分别由不同层负责。启用 `NEVERD_ENABLE_CPU_EMULATION=ON`；驱动模拟也会启用它。

首个配置 `linux-elf64-v1` 在 CPL3 或 EL0 运行 x64/AArch64 ELF `ET_EXEC` 与可自重定位的静态 PIE `ET_DYN`。它加载真实 ELF 分段，构造初始栈，按指令量恢复执行，并处理显式 Linux 系统调用请求。这是独立进程模型，不是完整 Linux 发行版，也不承诺运行任意 libc 二进制。动态链接、信号、线程、文件系统和不支持的服务都会明确失败。

<!-- i18n-section: cli-sdk -->

## CLI 与 SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

匹配的 Linux 主机选择 KVM，匹配的 Windows 主机选择 WHP；其他主机／来宾 ISA 组合使用 Unicorn。所选后端不可用时会报错，不会静默回退。即使在 Windows 上运行，ELF 仍使用 Linux 进程模型。CPU 指令范围与限制见[CPU 执行](cpu-execution.md)。

CLI 输出一份 JSON 报告。来宾退出状态为 0 时 CLI 返回 0，其他来宾状态返回 2，执行不完整（包括故障和资源限制）返回 3，设置/API 错误返回 1。实际来宾状态位于 `exit_status`。新增 C 入口 [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) 接收 session、非空输入路径、明确配置和可选 options JSON。使用 `neverd_free_string` 释放结果；NULL 表示设置失败，可通过 `neverd_last_error` 查询。来宾故障或资源停止会返回报告。session 中已加载的分析映像既非必需，也不会被修改。

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## 选项与结果

选项为不超过 64 KiB 的 JSON object。未知或 null 字段、类型无效、字符串内含 NUL、非正数限制都会被拒绝。

| 选项 | 默认值 | 约定 |
|---|---|---|
| `backend` | `auto` | `auto`、`unicorn`、`kvm` 或 `whp` |
| `arguments` | 输入文件名 | 完整 argv，包含 argv[0]；空值使用默认值 |
| `environment` | `[]` | 显式来宾字符串；不继承宿主环境 |
| `instruction_limit` | 100000 | 共享的已接纳指令尝试次数 |
| `event_limit` | 10000 | system-call 事件数，在 OS 服务处理前计费 |
| `timeout_microseconds` | 5000000 | 进程设置后开始的单调 deadline |
| `memory_limit` | 67108864 | 物理／映射内存预算 |
| `stack_size` | 1048576 | 预算内按页对齐的栈 |
| `output_limit` | 1048576 | 捕获的 stdout/stderr 总字节数 |
| `instruction_quantum` | 1024 | 让出给 runtime 前的接纳间隔 |

`schema_version` 为 1。结果含 profile、架构、所选后端与原因、`stop_reason`、可空的 `exit_status`、诊断、入口／当前 PC、计数器、服务记录和最后一个类型化 CPU 退出。地址、syscall 编号、参数寄存器及原始返回位均为**不带** `0x` 的十六进制字符串；`stdout_hex`／`stderr_hex` 保留 NUL 和无效 UTF-8。syscall 结果为 null 表示没有建模返回值（例如退出或不支持的请求），不表示成功返回 0。

<!-- i18n-section: linux-semantics -->

## Linux 配置语义

OS 策略复用现有 ELF 加载器解析出的 program headers。它验证 ABI 标签、segment 对齐、已映射的 program-header 表及用户地址边界。通用映射计划会在分配前检查范围、权限、重叠和预算，只发布完全准备好的私有地址空间。保留文件页前缀／尾部字节，将 BSS 清零，遵循 segment 权限并为栈保留 guard gaps。页重叠布局和矛盾 header 会被拒绝，不会猜测。

静态 PIE 使用不低于 `0x40000000` 的确定性 load bias，并按较大的 `PT_LOAD` 对齐要求递增。所有映射段、入口 PC、`AT_PHDR`/`AT_ENTRY` 共用该 bias；原始 program header 值不变，没有 interpreter 时 `AT_BASE` 为零。映射来源明确为原始文件字节，不使用分析阶段的 pointer fixup；来宾启动代码必须自行完成 relocation 和初始化。Loader 从有界的原始文件记录中解码 `PT_DYNAMIC`，不依赖 section header。若存在，该表必须可读、正确终止且最多 4096 项。拒绝 `PT_INTERP` 和外部 dependency/filter/audit 标签；不会提供 dynamic linker、符号解析器或 constructor runner。

初始栈含对齐的 argc/argv/envp/auxv、PHDR/PHENT/PHNUM、entry、页大小和身份值。模型 PID/TID/UID/GID 均为 1000。为保证可复现，`AT_RANDOM` 是输入 SHA-256 的前 16 字节；这是确定性的模型策略，不是加密熵。HWCAP/HWCAP2 均为 0，没有 vDSO。

已实现的调用为 `write`、`exit`、`exit_group`、`getpid`、`gettid`, `mmap`, `mprotect`, `munmap`, `brk`，其编号分别遵循 [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) 与 [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)。x64 SYSCALL 返回时会应用 RCX/R11 clobber，同时设置 RAX 与下一 PC；ARM64 使用 x8 作为编号、x0 作为结果。未知调用会以 `unsupported_service` 停止，不会执行宿主 syscall。

静态 `PT_TLS` 模板作为 loader 提供的事实进行验证：仅一个模板，文件／内存范围有界、对齐一致且初始字节可读。来宾启动时分配并初始化 TLS 块，安装 thread pointer；Linux 模型不会虚构 libc 专用 TCB/DTV。这样支持 freestanding 程序中的编译器生成 local-exec TLS。Dynamic TLS 和 OS 线程仍属独立工作。

x64 的 `arch_prctl` 支持 `ARCH_SET_FS`、`ARCH_GET_FS`、`ARCH_SET_GS` 和 `ARCH_GET_GS`。Set 可接受尚未映射的 user-range 基址，后续解引用仍检查权限。kernel-range 基址返回来宾 `EPERM`；无效 Get 目标返回来宾 `EFAULT`，不会触发 CPU fault。其他操作明确失败。ARM64 启动用 `MSR` 安装 `TPIDR_EL0`；`MRS`、FS/GS 内存访问和上下文恢复会跨执行量与后端入口保留 thread pointer。这本身不实现线程调度器。

文件描述符 1 和 2 是虚拟字节 sink。`write` 验证可读 user pages；后续页不可读时返回已读前缀，没有任何字节可读时返回来宾 `EFAULT`。无效描述符返回 `EBADF`；对有效描述符写入零字节不会访问指针。这不模拟 Linux pipe 原子性或文件对象。若输出超过限制，会在发布写入前停止。

匿名内存服务与映像、栈共用进程地址空间和物理内存预算。`mmap` 仅接受 `MAP_PRIVATE | MAP_ANONYMOUS`，权限为普通 `PROT_NONE`、`PROT_READ`、`PROT_READ | PROT_WRITE`、`PROT_READ | PROT_EXEC` 或可读的 RWX。空闲且页对齐的提示地址会被采用；否则从 `0x100000000` 起、再从最低用户地址起查找空隙，并保留栈保护区。这是确定性布局，不模拟 Linux ASLR。新页独立分配并清零；部分解除映射能回收未被固定的页。CPU 投影或仍持有的 backing view 可以将已退役分配的生命周期延长到自身释放时。

长度向上取整到页。`munmap` 允许空洞和重复移除；`mprotect` 遇到空洞前会修改已映射前缀，然后返回 `ENOMEM`。`PROT_NONE` 保留分配和字节，但禁止来宾访问。原始 `brk` 成功时返回请求的字节边界，失败时返回旧边界，不采用 libc 包装器的零／负一约定。初始 break 是页对齐的映像末尾。增长受其他映射与内存预算限制；收缩保留剩余部分页中的字节。受支持子集的规则与错误优先级遵循 Linux 的[映射](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c)和[保护](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c)服务。

文件、共享、固定映射，向下增长、大页、内存锁定、保护键、仅执行／仅写策略及其他标志都属于明确不支持的服务：在发布效果或构造返回值前停止。已支持子集内的一般范围、长度和对齐错误会返回来宾错误，并允许继续执行。任何内存服务都不会向宿主 OS 转发来宾指针或映射请求。

<a id="windows-pe64-profile"></a>

<!-- i18n-section: windows-pe64 -->

## Windows PE64 配置

`windows-pe64-v1` 新增有界 Windows x64/ARM64 控制台进程：PE 装载、PEB/TEB、静态和动态 TLS、启动／退出回调及具名 Win32 API 模型。它独立使用 CPU 层，无需启用驱动模拟；DLL/CRT 装载、GUI、用户态 SEH、线程及通用 Windows 兼容性仍待完成。

Windows 虚拟内存新增 `VirtualAlloc`、`VirtualFree`、`VirtualProtect`、`VirtualQuery` 及当前进程的 `FlushInstructionCache`。OS 层管理预留区域，`AddressSpace` 统一管理已提交页面、权限和物理存储。测试覆盖动态代码改写、访问故障和内存额度回收。

私有分配支持 `MEM_RESERVE`、`MEM_COMMIT`、`MEM_DECOMMIT`、`MEM_RELEASE` 和 `MEM_TOP_DOWN`，预留按 64 KiB 对齐，页面大小为 4 KiB。仅预留不消耗来宾 RAM。重复提交保留数据并更新权限，解除提交归还独立页面的存储。完整范围校验和分阶段分配避免普通分配或权限失败留下部分修改。查询返回 48 字节的 x64/ARM64 内存信息结构，并仅在同一次分配内向后合并。初始映像、环境、堆区域、API 入口和栈边界均参与地址分配；栈的分配标识与 TEB 一致。如果成功的 `VirtualProtect` 将旧权限的输出地址改成只读，新权限仍生效，输出内容保持不变，调用仍返回成功。 对未完整提交范围的权限修改失败时，返回 `ERROR_INVALID_ADDRESS`，旧权限输出写为 `PAGE_NOACCESS`，各页权限保持不变。

支持的权限为 `PAGE_NOACCESS`、`PAGE_READONLY`、`PAGE_READWRITE`、`PAGE_EXECUTE_READ` 和 `PAGE_EXECUTE_READWRITE`。保护页、仅执行及写时复制策略、缓存修饰符、大页、reset/write-watch/占位区域及修改模型拥有的运行时映射仍明确拒绝。仅私有虚拟分配可解除提交或释放。本项不增加用户态异常派发能力，也不构成 ARM64 硬件原生执行证据。

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

该版本化单线程模型按首选基址装载 AMD64/ARM64 PE32+ 控制台 EXE。映射前核对原始文件字节和装载器事实；保留用户页权限，并为栈留出未映射保护间隙。未知导入、按序号／绑定／延迟导入、加载配置／CFG、托管映像、GUI 和 DLL 入口均明确拒绝。重定位记录会校验，但不执行重定位。具名系统 API 模型不等同于已安装的系统 DLL：加载器链表仅包含实际映射的 EXE，`GetModuleHandleW` 当前只接受 NULL。

x64 的 GS、ARM64 的 x18 指向 TEB。支持栈边界、自指针、PID/TID、PEB、进程参数、LastError 和 TLS。输入严格按 UTF-8 解码为 UTF-16，argv 按 Microsoft CRT 规则加引号。环境变量名限 ASCII，拒绝忽略大小写后的重名；值可为 Unicode，排序后以双 NUL 结束，不继承主机环境或文件系统。静态 TLS 复制模板、清零 BSS 并写入 32 位索引；动态 TLS 使用独立 TEB 槽位。启动和退出按顺序读取实时回调表，所有指令与具名调用共享截止时间和资源额度。入口返回或正常退出均运行退出回调；退出回调中的递归退出明确停止。

精确 API 清单由 `WindowsProcessServices.def` 管理：`ExitProcess`、`RtlExitUserProcess`、标准输出句柄及同步 `WriteFile`、LastError、进程／线程标识与伪句柄、`GetCommandLineW`、进程堆分配／释放／大小、动态 TLS，以及 NULL `GetModuleHandleW`。提供方限定为 `kernel32.dll`、`kernelbase.dll`、`ntdll.dll` 并精确匹配导出名。直接 syscall 或伪造回调入口不能选择 API 模型。堆由进程拥有并在释放时回收；输出保留二进制字节，Win32 参数错误与不支持的异步 I/O、用户异常分开处理。指针别名会观察到完成计数的初始清零和实际返回地址的变化。

`windows.native_calls` 报告保留 DLL／函数名、声明的标量参数及可空返回位值，不伪造 NT syscall 编号。`NeverDWindowsProcessTests` 覆盖真实 x64/ARM64 PE 启动、编译器 TLS、回调修改、堆／LastError、别名、畸形元数据、权限故障和预算；`NeverDProcessPublicTests` 验证 CLI/C ABI。Windows CI 直接运行相同 EXE 作为独立行为对照，并要求 WHP 用例通过；原生 ARM64 运行证据仍需要对应机器。

`WriteFile` 的非空输入缓冲区不可读时返回 `ERROR_INVALID_USER_BUFFER`（1784），将完成计数清零，并且不输出任何字节。

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c).

<!-- i18n-section: verification -->

## 验证

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# 在构建共享库／CLI 时：
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

测试为两种 ISA 编译独立 ELF 入口汇编和 C，验证 data/BSS、真实启动元数据、系统调用错误、二进制输出、权限故障、部分写入、不支持的服务和跨执行量预算。TLS 用例初始化独立对齐的块、清零 TLS BSS、安装线程指针并检查切换后保留；x64 还检查 `arch_prctl` 错误不会丢失先前基址。不可用后端明确跳过。公开测试经过共享 C ABI 和 CLI，核对报告与退出码。静态 PIE 用例在自行重定位数据和函数指针前验证 auxv 和原始为零的 RELA 槽。映射测试单独检查选择分析字节源时保留 fixup；动态表测试覆盖缺失 section 及畸形／依赖输入。匿名内存用例覆盖两种 ISA 的分配、保护、空洞、重新映射、堆增长／收缩和可处理的系统调用错误；真实来宾写入验证普通和部分保护更改后的故障。x64 还在 RW/RX 切换之间重写同址代码并调用两版；同一 ELF 在 Linux 原生运行，作为独立结果／故障参照。纯内存测试覆盖预算耗尽、回收和不持有 RAM 的权威映射快照。交叉编译与 Unicorn ARM64 结果不构成原生 ARM64 KVM/WHP 证据。
