**语言**: [English](../driver-emulation.md) | [简体中文](driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← 文档索引](README.md)

# Windows 驱动模拟

NeverD 的可选驱动模拟器执行受支持的 x64 WDM 驱动的 PE 入口点，并可在卸载前执行显式指定的串行请求场景。它使用 Unicorn 执行 CPU 指令，使用 NeverD 自有的有界 Windows 环境模型。它不会将驱动加载到宿主内核，也不会把来宾 API 调用转发给宿主操作系统服务。

## 构建与运行

该功能需显式启用，且不依赖 `BUILD_TESTING`：

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON
cmake --build build-release --target neverd --parallel 4
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --instruction-limit 100000 > driver-report.json
```

报告始终以 JSON 输出到 stdout；请求与初始化配置诊断输出到 stderr。默认预算为 100000 条来宾指令、64 MiB 来宾内存、10000 条记录事件以及 5000 毫秒。指令数限制必须为正数。任一预算耗尽都会停止执行，并保留已收集的部分观察结果。

| 退出码 | 含义 |
|--------|------|
| `0` | 初始化以及所有请求中已完成的操作均成功 |
| `1` | 输入或选项无效、执行环境建立失败，或构建时未启用该功能 |
| `2` | 初始化或已完成的请求返回失败的 `NTSTATUS` |
| `3` | 场景尚未完成便停止执行，例如遇到不支持的 API、故障或预算限制 |

返回失败状态仍表示已完整观察到该操作的结果。成功返回只描述这一次模型执行，并不能证明该驱动可在 Windows 下正常工作。

## 驱动兼容性

兼容性由实际执行的代码路径及其依赖决定，而非由 `.sys` 扩展名决定。目前的验收证据涵盖原创的独立测试样例，以及 Microsoft SIOCTL WDM 示例的 buffered、in-direct 和 out-direct 路径，包括其调试日志构建；这并不代表与任意第三方驱动兼容。

验收还覆盖 Pavel Yosifovich 未修改的 Zero WDM 示例，包括 direct READ/WRITE、原子统计计数及统计 IOCTL。

| 驱动类别或要求 | 当前范围 | 缺少的环境 |
|----------------|----------|------------|
| 使用下列 API 的 x64 软件 WDM 驱动 | 有界 x64 WDM 初始化、串行缓冲／直接请求、工作项、定时器、DPC、事件与等待，以及行为报告和限制 | 每个额外执行到的 API 都必须具有明确的模型 |
| `METHOD_BUFFERED` IOCTL | 串行缓冲／直接 I/O，可由工作项或 DPC 完成 | 仅支持下列 API 子集；不支持并发 IRP 或请求取消 |
| `METHOD_IN_DIRECT`、`METHOD_OUT_DIRECT` | 请求拥有的 MDL 及系统映射 | 物理页身份、DMA 及用户映射 |
| 驱动自行分配的 MDL | 覆盖模型非分页池的独立描述符，复用原始缓冲区地址 | IRP 关联、MDL 链、探测／锁页、物理页及用户映射 |
| READ/WRITE | 串行缓冲／直接 I/O，可由工作项或 DPC 完成 | 仅支持下列 API 子集；不支持并发 IRP 或请求取消；`METHOD_NEITHER` 与隐式文件位置 |
| `METHOD_NEITHER` | 拒绝 | 用户地址空间上下文、访问探测及来宾异常处理 |
| KMDF 1.33 非 PnP 驱动 | 版本绑定、对象／上下文、具名控制设备、顺序默认队列，以及实际执行回调的缓冲／直接请求 | 不支持 PnP 设备、取消、通用队列调度、类扩展或 UMDF |
| PnP 总线／功能／过滤驱动 | 初始化可在 API 子集内运行；不支持设备栈生命周期 | 设备附加、向下层驱动派发、PnP 和电源 IRP |
| 存储、网络、显示、文件系统及微过滤驱动 | 不支持相关子系统契约 | 端口／类／微端口框架、NDIS/WFP、图形或文件系统服务 |
| 工作项、定时器、DPC、事件与等待 | 当前执行 IRQL 在派发与工作项中为 `PASSIVE_LEVEL`，在 DPC 中为 `DISPATCH_LEVEL` | 仅支持下列 API 子集；不支持并发 IRP 或请求取消 |
| 使用进程／线程回调、句柄、注册表／文件操作或内核模块发现的驱动 | 支持配置的注册表；其他行为仅限下列 API | 对象管理器、系统状态以及回调／事件产生机制 |
| 硬件、DMA、PCI、中断或虚拟化驱动 | 不支持所需环境 | 设备模型、物理内存、总线、中断及特权 CPU 状态 |
| x86 或 ARM64 Windows 驱动 | 拒绝 | 相应架构的加载、ABI 及执行模型 |
| x64 CFG | 验证目标表及检查／分派调用；未启用的插桩保留来宾回退函数 | XFG、导出抑制、不支持的加载配置和 TLS 仍被拒绝 |

未使用的不支持导入项可以保持绑定。一旦执行到不支持的操作，便会停止并给出诊断及此前收集的观察结果。仅 DriverEntry 成功，并不能证明后续派发、硬件或框架路径也受支持。下方 API 表是受支持子集的权威定义。

## 执行契约

此配置在 CPU0 上以确定性的协作调度模拟 x64 WDM 生命周期。 执行从 PE 入口点开始；若存在编译器生成的入口包装函数，也会保留并执行。DriverEntry 必须返回 `STATUS_SUCCESS` 才能完成初始化；非零的成功状态或待处理状态会因初始化契约不受支持而停止。失败状态则作为已完成的初始化结果保留。所有对象、字符串、栈、函数指针与分配均位于来宾内存。模型根据配置的服务名（默认为 `NeverDDriver`）提供 `DRIVER_OBJECT` 和注册表路径。

适配器使用 Unicorn 的虚拟 TLB 模式保留来宾虚拟地址，包括规范的高位内核地址，无需合成 Windows 页表。初始 RFLAGS 为 `0x202`；软件设备配置采用固定的 64 字节缓存行。这些都是本执行场景的显式属性。内联 x64 CR8 读取观察到相同的 `PASSIVE_LEVEL` / `DISPATCH_LEVEL`；CR8 写入及其他控制寄存器操作仍不受支持。

未知导入项绑定到延迟陷阱。未使用的导入项不会阻止执行；执行其 thunk 或读取未建模的导出数据值时，会以 `unsupported_api` 停止。不支持的 CPU 环境效果也会明确停止。NeverD 不会用成功返回值替代未实现的调用。格式错误的映像或不支持的加载要求会在执行前失败。

排队的 `DelayedWorkQueue` 工作项在 `PASSIVE_LEVEL` 执行，来宾 DPC 回调在 `DISPATCH_LEVEL` 接收规定的四个参数。CPU0 在调用返回和阻塞等待的边界进行确定性的协作调度。相对、绝对和周期定时器使用虚拟时间；没有可运行的执行帧时，时间推进到下一定时器或等待期限。通知型与同步型事件／定时器保留各自的信号消耗语义。每个回调拥有独立的来宾栈；多个阻塞帧保留局部变量和完整 CPU 上下文，来宾内存仍然共享。Win64 回调入口将前四个参数放入寄存器，其余参数放入栈。请求仍串行处理：标记 IRP 为待处理的派发函数必须返回 `STATUS_PENDING`，且该请求完成后才能开始下一个。待处理请求或无限等待没有可用生产者时，以停滞的 `model_error` 停止。指令、内存、观察记录与墙钟时间预算仍共用。

这是有界调度模型，并不代表完整 Windows 异步支持。可警报或用户模式等待、系统线程、APC、请求取消、自旋锁、并发 IRP、通用 IRQL 切换、`METHOD_NEITHER`、UMDF、KMDF PnP 设备及通用队列调度、完整 PnP／电源、硬件、DMA 与中断仍不支持。仅初始化调用会执行显式排队的回调，不会隐式生成请求或卸载。

工作项在回调开始前出队，因此回调可以释放自身的工作项。释放仍在队列中的项、重复入队、使用失效对象或非来宾可执行内存中的回调地址都会明确失败。设备引用保留到回调返回。请求卸载要求释放全部工作项并完成排队工作。CPU 上下文保存与恢复包含通用、SIMD、FPU 和控制状态；来宾内存始终共享，故障 CPU 不能靠恢复上下文继续执行。
删除会延后到文件对象及排队／执行中的工作项引用全部释放。对象区耗尽时，工作项分配返回 NULL。

映像默认使用首选基址，除非场景选择了有效的重定位地址。映像必须为使用 native 子系统的 PE32+ x64 可执行文件。导入可来自 `ntoskrnl.exe`、`ntkrnlmp.exe` 或 `WDFLDR.SYS`。

执行加载器支持经验证的 x64 `DIR64` 基址重定位，以及有限的安全 cookie 加载配置；它会在入口包装函数执行前设置确定性的来宾 cookie。其他未建模的加载配置字段、TLS、延迟／绑定导入、按序号导入以及托管映像都会被拒绝。映像还必须通过严格的范围与对齐检查。

启用的控制流保护（CFG）会验证 PE 标志、指针槽和已排序的可执行目标表。检查与分派辅助函数仅允许已声明的映像入口或已登记的 API 跳板，保留 Win64 调用状态，并拒绝未声明的目标。只有插桩而未启用 CFG 时，保留原始来宾回退指针。启用的 XFG、导出抑制和其他未建模的保护策略仍被拒绝；地址位于可执行内存并不使它成为合法目标。

KMDF 1.33 支持使用精确的 1.33.0 ABI：458 个函数槽具有稳定的来宾身份，下列 27 个 API 实现了执行语义。`WdfVersionBind` 和 `WdfVersionUnbind` 在真实 WDK `FxDriverEntry` 包装函数前后管理来宾绑定。`WdfGetDriver` 读取公共驱动全局结构。非 PnP 驱动、通用对象、控制设备、队列和传入请求共享类型化上下文、引用计数，以及实际执行的清理／销毁／卸载回调。所有已建模的框架调用和回调目前都要求 `PASSIVE_LEVEL`；清理完成后新增引用仍不在此配置的支持范围内。未建模的函数槽、`WdfLdrQueryInterface`、类扩展和 UMDF 会明确停止。

控制设备要求复制可打印 ASCII 名称，并且 SDDL 必须精确为 `D:P(A;;GA;;;WD)`。这授予所有调用方访问权限，无需虚构调用方令牌；不支持其他安全描述符、未命名设备和自动名称。设备初始化拥有一个 WDM 设备。请求可通过现有会话命名空间中的符号链接别名 `\DosDevices\Name` 或 `\??\Name` 选择设备，报告仍保留规范设备名。创建成功会消耗初始化对象并清空其指针；失败则回滚部分设备所有权。`WdfControlFinishInitializing` 决定何时可以递送 I/O。仅在已建模的文件、工作项和请求允许时，删除操作才移除设备及其链接；不支持删除过程中取消或排空请求。

96 字节的 `WDF_IO_QUEUE_CONFIG` 支持顺序默认队列，要求显式被动执行且不使用框架同步。控制设备队列不参与电源管理。专用 READ／WRITE／IOCTL 回调优先于默认回调。已接受的队列请求即使同步完成也返回 `STATUS_PENDING`；void 回调的返回寄存器不会使请求完成。延迟完成使用现有调度器。没有处理函数时，请求以 `STATUS_INVALID_DEVICE_REQUEST` 完成；未启用零长度递送时，零长度 READ／WRITE 直接完成。默认文件包以成功状态和 Information=0 完成 CREATE／CLEANUP／CLOSE。不支持并行／手动队列、取消、文件回调、PnP 设备和完整 PnP／电源。

请求参数使用 40 字节的 `WDF_REQUEST_PARAMETERS` 布局。输入／输出访问函数返回逻辑长度，保留缓冲区别名及现有直接 I/O 的 MDL 映射；直接 IOCTL 的输入仍使用缓冲区。方向错误或缓冲区不足返回文档规定的状态。完成操作先执行请求清理和子对象销毁，再使 IRP／缓冲区失效，最后在引用允许时销毁请求。一旦开始完成操作，就拒绝新的请求访问函数调用；已取得的缓冲区指针在清理期间仍可使用。外部对象引用保留上下文，但不保留对已完成 IRP 的访问权。用户模式 `METHOD_NEITHER` 仍需要尚未实现的调用方上下文／探测／锁定支持。

已建模的 KMDF API: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetIoType`, `WdfDeviceCreate`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfIoQueueGetDevice`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`.

可选的真实 WDK 验证以真正的 KMDF 入口库分别编译 `driver_kmdf_lifecycle.c` 和 `driver_kmdf_control.c`。`NEVERD_KMDF_FIXTURE`／`NEVERD_KMDF_CFG_FIXTURE` 选择生命周期映像；`NEVERD_KMDF_CONTROL_FIXTURE`／`NEVERD_KMDF_CONTROL_CFG_FIXTURE` 选择普通／启用 CFG 的控制设备映像。缺少外部产物时会明确跳过。原生及 C API／CLI 覆盖见[测试指南](testing.md)。当前执行证据仅来自 Linux 主机。

初始 API 模型刻意采用有限契约：

| API | 建模行为与限制 |
|-----|----------------|
| `RtlInitUnicodeString` | 根据有界、以 NUL 结尾的源字符串构造来宾 `UNICODE_STRING` |
| `RtlCopyUnicodeString`、`RtlCompareUnicodeString`、`RtlEqualUnicodeString` | 带长度的 UTF-16 复制及区分大小写比较；不区分大小写的比较需要 Windows 大小写表，因此会停止 |
| `ExAllocatePool2` | 分页／非分页 NX 分配，默认清零；支持未初始化与缓存行对齐标志；无效的必需标志返回 NULL，配额／可执行池以及分配失败引发异常的路径会停止 |
| `MmGetSystemRoutineAddress` | 通过共享导出清单解析带长度的来宾名称 |
| `MmMapLockedPagesSpecifyCache`、`MmGetSystemAddressForMdlSafe`、`MmUnmapLockedPages` | 请求拥有的 MDL 支持 KernelMode 缓存映射及权限；非分页池 MDL 通过安全辅助函数复用原始池映射 |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | 独立描述符，完整范围须位于同一个有效非分页池分配内；描述符与缓冲区具有独立生命周期，不支持 IRP 关联、MDL 链或配额 |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | 显式配置的会话注册表、逐句柄权限与生命周期、查询缓冲区大小及修改；不访问宿主注册表 |
| `ExAllocatePoolWithTag`、`ExFreePoolWithTag`、`ExFreePool` | 对池类型 `0`、`1`、`512` 提供数据分配；大小／标签必须为正，带标签释放必须匹配，地址不复用 |
| `IoCreateDevice`、`IoDeleteDevice` | 设备类型为 `0x22`，characteristics 为 `0` 或 `0x100`，扩展大小有界，名称为 ASCII `\Device\Name` |
| `IoCreateSymbolicLink`、`IoDeleteSymbolicLink` | 一个会话命名空间内的 ASCII `\DosDevices\Name` 或 `\??\Name`，目标为 `\Device\Name` |
| `DbgPrint`、`DbgPrintEx` | 经检查的 Win64 可变参数格式化，最多输出 512 字节；启用所有调试器过滤器 |
| `IoGetCurrentIrpStackLocation` | 返回当前建模 IRP 的栈位置；正常编译的 WDM 宏读取相同来宾字段 |
| `KeGetCurrentIrql` | 当前执行 IRQL 在派发与工作项中为 `PASSIVE_LEVEL`，在 DPC 中为 `DISPATCH_LEVEL` |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | 设备拥有的不透明工作项；仅支持 `DelayedWorkQueue`，在 `PASSIVE_LEVEL` 将设备和上下文传给回调；禁止释放仍在队列中的项 |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | 不透明 DPC 存储、四个来宾回调参数、`DISPATCH_LEVEL`、重复入队／移除及优先级；仅目标 CPU0 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | 通知／同步定时器；相对／绝对 100 ns 期限、毫秒周期、重新设置／取消及虚拟时间中的信号查询 |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | 通知／同步事件保留不同信号消耗行为；`KeSetEvent` 仅接受 Increment=0、Wait=FALSE |
| `KeWaitForSingleObject` | 单个已初始化事件或定时器；非警报 `KernelMode`、原因 `Executive`；零超时轮询、有限相对／绝对或无限等待；非零／无限等待要求 IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | IRQL <= APC_LEVEL 的非警报 `KernelMode` 相对／绝对延迟；虚拟时间推进后恢复保存的来宾执行帧 |
| `IoMarkIrpPending` | 标记当前存活的 IRP；也支持 WDM 宏对栈控制字段的等效写入；派发必须返回 `STATUS_PENDING` |
| `IofCompleteRequest`、`IoCompleteRequest` | 以 `IO_NO_INCREMENT` 完成当前同步或待处理的建模 IRP；已完成的 IRP 或缓冲区不能再次访问 |
| `memcpy`、`memmove`、`memset`、`memcmp`、`RtlCopyMemory`、`RtlMoveMemory`、`RtlFillMemory`、`RtlZeroMemory`、`RtlCompareMemory` | 有界的来宾缓冲区操作，每次调用最多 1 MiB；要求不重叠的复制 API 会拒绝重叠 |

API 的 IRQL 上限来自 `KernelAPIIRQL.def`，参数相关限制由所属模型检查。DPC 不能调用注册表 API，也不能分配、释放或访问分页池；Unicode `DbgPrint` 转换要求 `PASSIVE_LEVEL`，支持的 ANSI 输出和非分页操作仍可在 `DISPATCH_LEVEL` 使用。回调栈有明确边界，越界栈指针不能进入另一阻塞工作项的栈。设备扩展中的已启动定时器会阻止设备提前回收。这些检查并未开放通用 IRQL 切换。

定时器到期会先满足已登记的等待，再允许 DPC 重置或重新设置定时器。排队 DPC 先于已唤醒的 `PASSIVE_LEVEL` 执行帧恢复运行。若请求存储仍包含排队的 DPC，IRP 完成操作会在完成和缓冲区失效之前拒绝释放。

`DbgPrint` 格式化支持整数 `d/i/u/o/x/X`、指针 `p`、文本 `s/c`、`%%`、带长度的 Unicode `wZ/lZ`、宽字符串 `ls/ws`、标志、宽度／精度（包括 `*`），以及 Windows 整数长度修饰符。最多读取 32 个可变参数及 1024 字节格式串，宽度与精度上限均为 512。浮点数、`%n`、未知组合及非 ASCII 文本转换都会明确停止；模型不会猜测 Windows 代码页，也不会将来宾数据交给宿主 printf。

原始 RegistryPath 记录及其缓冲区在 DriverEntry 返回时失效。后续仍需使用该字符串的驱动必须在初始化期间复制它。

对象／池区域大小为 1 MiB。未初始化池字节采用确定性的 `0xCD` 内容；释放后的池字节采用 `0xDD`。这是一种具体执行场景。CPU 访问及建模的缓冲区 API 会拒绝访问已释放的池分配、已删除设备、区域内尚未分配的字节、不透明对象字段，以及对只读对象字段的写入。这些检查覆盖的是本模型的对象生命周期，并非通用的驱动内存安全分析。未写入的派发表槽位在报告中以零表示“未注册”。场景请求若对应未注册的主功能，则由模型的默认处理函数完成，状态为 `STATUS_INVALID_DEVICE_REQUEST`；该失败在派发状态与 I/O 状态中均可见。模型不会为此处理函数虚构来宾函数地址，来宾读取未写入槽位仍不受支持。显式注册空回调属于错误。

## 请求场景

通过 `--scenario` 传入 JSON 文件，指定请求及可选的卸载操作：

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --scenario scenario.json > driver-report.json
```

对于创建 `\Device\NeverDIO` 并接受缓冲 IOCTL `0x222000` 的驱动，`scenario.json` 示例为：

```json
{
  "requests": [
    {"kind": "create", "device": "\\Device\\NeverDIO"},
    {"kind": "ioctl", "code": "0x222000", "input": "00112233", "output_size": 4},
    {"kind": "cleanup"},
    {"kind": "close"}
  ],
  "unload": true
}
```

设备名称与 IOCTL 代码必须匹配驱动。create 时省略 `device` 会选择唯一的存活设备；无法唯一选择则失败。后续请求默认使用所属文件的设备，也可显式指定匹配的设备名称。可选的 `file` 是无符号 32 位场景身份，默认为零。每个身份有独立的 FILE_OBJECT 和 FsContext，并要求按 create、传输、cleanup、close 的顺序执行。不同文件的请求可以交错执行。独占设备会拒绝第二次打开。这些身份表示文件对象，而非复制的句柄。支持 buffered 和两种 direct IOCTL 方法。派发必须同步完成，或遵循上述回调待处理契约。输出长度无效及访问已完成的 IRP 都会明确失败。请求卸载后，不得留下存活的设备、符号链接、池分配或文件对象。

可选根字段 `"load_address": "0x190000000"` 请求更改加载基址；省略该字段或指定 `"0x0"` 时使用首选地址。映像必须满足重定位要求。原有的初始化命令或 C API 不会隐式执行任何请求场景。

根对象仅接受 `load_address`、`requests`、`unload`、`kernel_exports` 和 `registry`。所有请求均接受 `kind`、可选的 `device` 和可选的 `file`。IOCTL 必须提供 `code`，并接受 `input`、`output_size` 和 `direct_input`。`read` 接受 `output_size` 和 `byte_offset`；`write` 接受 `input` 和 `byte_offset`。偏移默认为零，接受整数或十六进制字符串，且必须位于有符号 64 位整数的非负范围内。生命周期请求拒绝传输字段。未知或重复字段会被拒绝。`code` 接受无符号 32 位 JSON 整数或 `0x` 十六进制字符串。`input` 是长度为偶数且不带前缀或空格的十六进制字节字符串；省略表示空输入。`output_size` 为无符号 JSON 整数，省略表示零。不接受小数以及浮点数写法。

对于 direct IOCTL，`input` 初始化第一个系统缓冲区，`direct_input` 初始化由 MDL 描述的独立第二缓冲区，并补零至 `output_size`。`METHOD_IN_DIRECT` 要求可读访问，但不意味着系统映射为只读。两种方法都使用可读写的场景缓冲区。`MdlMappingNoWrite` 移除映射的写权限，`MdlMappingNoExecute` 移除执行权限。解除映射会撤销系统虚拟地址；重新映射保留同一份锁定数据。请求完成时 MDL 和映射均失效。模型支持 WDM 宏使用的公共 MDL 字段；进程／PFN 字段、手工构造的 MDL、用户映射以及通过原始 UserBuffer 直接访问都会被拒绝。零长度 direct 缓冲区的 MDL 为空。

`IoAllocateMdl` 为非空、不溢出且不超过 1 MiB 的缓冲区分配独立元数据，不探测或锁定缓冲区。`Irp` 必须为 NULL，`SecondaryBuffer` 和 `ChargeQuota` 必须为 FALSE；对象区耗尽时返回 NULL。`MmBuildMdlForNonPagedPool` 要求完整描述范围位于同一个有效非分页池分配内。安全辅助函数及常规 WDM 宏复用原始地址，保留别名关系与既有权限，即使再次传入禁止写入／执行标志也不改变权限。额外系统映射和解除映射会被拒绝。`IoFreeMdl` 仅使描述符失效，池缓冲区有独立生命周期；只要不再使用已释放的存储，两种释放顺序均受支持。所有建模 MDL 字段均为只读；进程／PFN 访问、描述符链和手工修改字段仍不受支持。卸载前必须释放所有驱动拥有的描述符。

当 IOCTL 的 `output_size` 非零时，`Information` 不得超过该大小，即使输入缓冲区更大。无输出缓冲区的 IOCTL 可在此字段返回驱动自定义结果，且不会复制输出字节；`information_hex` 精确保留原始 64 位值。

对于 READ/WRITE，`DO_BUFFERED_IO` 或 `DO_DIRECT_IO` 决定传输方法。Neither 或冲突的标志会停止执行。Information 按传输长度检查；write 返回计数，read 返回字节。

`kernel_exports` 将例程名称映射到显式可用性布尔值，例如 `"kernel_exports": {"OptionalRoutine": false}`。已建模的导出和静态导入获得与 `MmGetSystemRoutineAddress` 共享的稳定地址。显式不存在的导出解析为 NULL，且不能满足静态导入。声明存在但没有 API 模型的导出解析为延迟陷阱。未知的动态名称会以可用性未指定的诊断停止；绝不会根据缺少实现推断不存在。名称为有界的可打印 ASCII，解析区分大小写。此清单是具体场景的属性，并不宣称匹配所有 Windows 版本。
`IoMarkIrpPending`, `IoGetCurrentIrpStackLocation` 和 `MmGetSystemAddressForMdlSafe` 是已建模的 WDM 头文件辅助函数；模型默认不会据此声明它们是导出项，其导出可用性需要静态导入或显式 `kernel_exports` 声明。

场景文本上限为 2 MiB，最多包含 64 个请求，每个输入或输出缓冲区最多 65536 字节，请求总字节数最多 512 KiB，包括 `direct_input` 内容。指令、观察事件、来宾内存和时间预算覆盖整个场景。1 MiB 区域还需存放对象与元数据，因此即使尚未用尽场景缓冲区总额度，也可能耗尽模型内存。

## 注册表场景

可选的 `registry` 数组定义具体的会话内注册表树。每个键必须提供 `path`，可选提供 `values` 数组；每个值包含 `name`、无符号整数 `type` 和十六进制 `data`。空名称表示默认值。例如 DWORD 值为 `{"name":"Mode","type":4,"data":"01000000"}`。值字节原样保留，不修复字符串终止符或展开环境变量。

路径必须是 `\Registry\Machine` 或 `\Registry\User` 下的绝对 ASCII 路径；祖先键自动建立。键和值按 ASCII 规则忽略大小写比较；拒绝非 ASCII 名称和重复身份。省略配置表示可用性未指定，注册表调用会停止。`"registry": []` 显式表示空命名空间。不根据驱动推断任何键、值、宿主注册表数据或服务配置。

`ZwOpenKey` 和 `ZwCreateKey` 返回独立的不透明句柄，逐句柄检查查询、设置、子键创建及删除权限。配置树授予受支持的 `KEY_ALL_ACCESS` 位，包括普通 `KEY_READ` 和 `KEY_WRITE` 掩码。这是显式可访问的测试树，不实现 Windows ACL 或权限提升判断。通用权限、`MAXIMUM_ALLOWED`、其他注册表视图、自定义安全描述符、类及符号链接均不受支持。相对创建要求具有 `KEY_CREATE_SUB_KEY` 的直接父键句柄。输入键为非易失键，新建键可以为易失键；拒绝在易失父键下创建非易失子键。不模拟重启或磁盘持久化。

`ZwQueryValueKey` 实现 Basic、Full、Partial 及其有定义的 Align64 信息类，包括精确长度、对齐数据、部分输出，以及不同的 `STATUS_BUFFER_TOO_SMALL`／`STATUS_BUFFER_OVERFLOW` 结果。`ZwSetValueKey` 和 `ZwDeleteValueKey` 只修改当前会话的树。`ZwDeleteKey` 拒绝删除仍有子键的键；已删除键的句柄在关闭前返回 `STATUS_KEY_DELETED`。`ZwClose` 独立释放句柄而不删除键；请求卸载时若仍有打开的注册表句柄则失败。

限制为含祖先在内的 256 个键、总计 1024 个值、每值 65536 字节、值数据总计 512 KiB、键路径 1024 个 ASCII 字节、值名 256 字节及同时打开 256 个句柄。创建和修改遵守与场景预检相同的限制。报告的 `configuration.registry` 保留原始输入；`registry` 列出最终存活的键路径和值，包括停止前已观察到的修改。未指定注册表时报告 null。此值快照不包含易失属性及句柄身份。

## Microsoft 示例验收检查

可选的[验证脚本](../../scripts/validate_windows_driver_sample.py)下载[验证清单](../../unittests/emulation/fixtures/sioctl-validation.json)中固定版本的 Microsoft SIOCTL 源码，验证 SHA-256 哈希，并使用 MinGW-w64 DDK 头文件编译未修改的源码。脚本在选定的输出目录中保留上游许可证／来源信息、构建命令、场景与报告。它需要网络访问、Clang、`lld-link`、`nm` 以及 MinGW-w64 的 DDK 头文件：

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

非默认 MinGW-w64 包含目录可通过 `--headers` 指定。脚本根据已编译对象的依赖生成 MS COFF 导入库。检查分别运行 buffered、in-direct 和 out-direct 场景，经过 DriverEntry、create、IOCTL、cleanup、close 和 unload。添加 `--debug` 并选择独立输出目录，可以使用 `DBG=1` 编译并验证来宾日志消息。上游示例没有注册 cleanup 处理函数，因此模型默认处理函数以 `STATUS_INVALID_DEVICE_REQUEST`（`0xC0000010`）完成 cleanup。驱动仍会关闭并卸载，成功的 IOCTL 则返回预期字节。对于这个完整场景，CLI 的预期退出码为 **2**，`scenario_success` 为 false。脚本仅在所有结果均符合预期时成功，包括可见的 cleanup 失败；它不会改写示例来隐藏该结果。

## Zero 示例验收检查

额外的 [Zero 验证脚本](../../scripts/validate_zero_driver_sample.py)按[清单](../../unittests/emulation/fixtures/zero-validation.json)固定的版本及哈希构建 Pavel Yosifovich 未修改的公开 Zero WDM 示例。使用相同工具链运行 `python3 scripts/validate_zero_driver_sample.py`。默认在 `build-release/driver-validation/zero` 保留源码、MIT 许可证、命令、场景及报告。九个请求覆盖跨页 direct read、write 计数、来宾原子统计和 buffered 统计 IOCTL。示例的零长度 read 失败及缺失 CLEANUP 处理函数仍可见；预期 CLI 退出码为 2，随后成功 close 和 unload。仅当这些结果及全部输出字节精确匹配时，验证脚本才成功。

## 报告与 SDK

JSON 报告区分 `stop_reason`、可为空的 `nt_status` 和 `nt_success`、停止位置 PC，以及指令计数。它保留停止前收集的 API 调用和可观察状态，包括设备对象与驱动回调地址。来宾地址以十六进制字符串表示，避免 JSON 使用方丢失 64 位精度。

`configuration` 对象记录本次执行的限制、服务名及 `kernel_exports` 覆盖配置。配置标识为 `wdm-x64-scheduled-v4`。`nt_status` 始终是 DriverEntry 的结果，而 `scenario_success` 综合描述初始化及已完成请求的结果。`phase`、`requests` 和 `unload_completed` 表明请求生命周期的哪些部分已运行。每次 API 调用及 CPU 写入也会记录阶段（`driver_entry`、`request:N`, `callback:N` 或 `unload`）。每个请求报告派发状态与 I/O 状态、是否完成、information 长度以及返回的 `output_hex` 字节。`preferred_image_base` 描述原始 PE 基址。`security_cookie` 为已初始化 cookie 的来宾地址；若无需 cookie，则为 `"0x0"`。请求报告字段为 `kind`、`device`、`file`、`byte_offset`、`code`、`irp`、`completed`、`dispatch_status`、`io_status`、`information`、`information_hex` 和 `output_hex`。 `configuration.registry` 保留原始注册表配置。 `information_hex` 以十六进制字符串精确保留原始 64 位 `IoStatus.Information`；原有数值字段 `information` 仍保留。

工作项观察记录使用 `callback:N` 阶段。待处理请求的 `dispatch_status` 保留 `STATUS_PENDING`，最终完成状态单独记录在 `io_status`，并据此计算该请求对 `scenario_success` 的影响。

可为空的 `fault` 对象保留后端首次故障。其 `kind`、`pc`、可为空的 `address`、`size`、`access` 和 `interrupt` 区分未映射或受保护内存、无效范围、无效指令及 CPU 异常。地址使用十六进制字符串；大小和中断向量使用整数。用于观察的读取不能替换原始故障。发生故障的后端不能恢复执行，此记录也不意味着支持来宾 SEH 处理。

`instructions` 统计执行策略已准许的来宾指令尝试次数。被执行策略拒绝的指令不计数；已准许但在 CPU 中发生故障的指令计数。合成的 API 派发与返回哨兵不增加该计数。

每个 `writes` 条目都带有 `semantics: "attempted_guest_write"`：记录栈外的 CPU 写入尝试，包括随后可能发生故障或被预算停止的尝试。它不保证写入已完成，也不包含 API 模型所做的写入。设备与驱动对象快照描述执行停止时观察到的状态。

包含 `neverd/sdk/NeverDCAPIEmulation.h`（或 C API 总头文件），创建会话，然后调用 `neverd_emulate_driver_json(session, path, options)`。显式传入非空路径会直接进入严格执行预检，无需先通过通用分析 API 加载。CLI 采用此路径。传入 `NULL` options 使用默认值。显式 `neverd_driver_options_v1` 选项要求 `struct_size` 精确匹配，且指令、内存、事件和超时预算均为正数。结果须用 `neverd_free_string` 释放。

若路径传入 `NULL`，则要求会话已加载文件，并独立于 IR 分析和函数受限加载重新解析该文件。两种方式均保留会话映像不变。调用期间必须保持输入文件可用且不变。请求／执行环境建立失败时返回 `NULL` 并设置 `neverd_last_error`；执行停止则返回 JSON。未启用该功能的构建仍提供此 API，并报告启用方法。

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)` 使用相同的 v1 选项与所有权规则，另外接受严格验证的场景输入。必须传入非 NULL、以 NUL 结尾的 JSON 字符串。原有 `neverd_emulate_driver_json` ABI 保持不变，仍仅执行初始化。C++ 解析器 `driverOptionsFromScenarioJSON` 为 `emulateDriver` 的调用方提供相同的场景验证。

内部 C++ 入口为 `include/neverd/emulation/DriverSession.h` 中的 `neverd::emulation::emulateDriver`。格式解析由现有加载器负责；Windows 对象／API 行为由 `lib/emulation/windows` 负责；CPU 状态及执行由 Unicorn 适配器负责。适配器与模型使用相同的来宾内存接口。Windows API 行为不应放入 Unicorn fork。
