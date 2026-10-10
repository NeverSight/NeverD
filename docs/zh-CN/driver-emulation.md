**语言**: [English](../driver-emulation.md) | [简体中文](driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← 文档索引](README.md)

# Windows 驱动模拟

NeverD 的可选驱动模拟器执行受支持的 x64 WDM 驱动 PE 入口，并可在卸载前运行显式请求场景。CLI 通过 `auto` 在匹配的 Linux 主机上选择 KVM，在匹配的 Windows 主机上选择 WHP；原有 C++ 默认设置和 V1 API 保留 Unicorn。所有后端共用 NeverD 的受限 Windows 环境模型，由模型处理客体 API 调用，驱动与主机内核保持隔离。

## 执行后端

`driver-strict` 支持匹配的 Linux x64 主机上的 KVM 和 Windows x64 主机上的 WHP；`auto` 选择对应原生传输，跨 ISA 执行选择 Unicorn。显式 Unicorn 和原有 V1 API 保留可移植软件配置。原生执行在进入 CPU 前检查规范地址和指令效果；硬件不可用时明确失败且不回退。未支持的指令及 OS 行为仍明确报错。Windows x64 原生 CI 在关闭 Unicorn 的配置下通过全部 359 项必跑检查：131 项 CPU 检查、26 个内置映像与 46 个 WDK 映像及 40 个场景组合在首选和重定位地址产生的 224 项驱动结果，以及 4 项 SEH 边界检查 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). 原生 ARM64 的实机证据仍待补充，这不表示兼容任意驱动或 Android/Darwin 环境。

上面的原生验证覆盖已声明的驱动入口和已发布场景。下文的逐功能回归以及 C API／CLI／Python 检查，除非明确记录了 Windows 执行结果，其证据范围仍限于 Linux；原生样例集通过不代表每一种测试变体都已在 Windows 验证。

`DriverImage.def` 集中声明严格 PE 校验的大小、对齐限制及诊断文本；指针宽度来自 `DriverProfile.def`。`DriverImage.cpp` 负责校验与重定位，可接受的映像和错误消息保持不变。

ARM64 宿主上的 `checked-x64-v1` 使用 Unicorn 执行 x64 来宾。每条指令和访存都在单步前验证，并保留 Windows 对象检查、写入观察器、RAM 别名和仅保存 CPU 的上下文。允许标量内存算术、自然对齐的锁定算术、SETcc 和`BT/BTS/BTR/BTC`，并执行读写权限检查；标志由原生执行负责。有限 SIMD 包括传统 SSE/SSE2 移动与逻辑、`MOVLHPS`/`MOVHLPS` 及带屏蔽的标量转换／减法。全部 16 个 XMM 寄存器与 MXCSR 跨入口及上下文恢复保存。未屏蔽 SIMD 故障要求原生 KVM/WHP 的 `precise_simd_exceptions` 能力。全宽 XMM store 会先按顺序触发两个 8 字节写入观察，再修改任一字。未对齐的 aligned-vector 形式仍不支持。普通 RAM 操作数可以跨越独立分配或别名映射的页面；整段权限验证通过后才写入，失败定位到第一个不可访问的字节。MOVS 保留已完成元素及故障元素的重启寄存器，不提交部分元素。

supervisor x64 支持一次对齐的 1/2/4 字节标量 MMIO 事务；设备页不会进入原生 RAM 映射。MOVS/REP MOVS 在每个重启边界只执行一个元素。设备源必须提供无副作用的 prepared read，以便目标写观察器能在设备读取提交前停止。Windows 寄存器组实现该准备流程；其他设备会在产生效果前拒绝字符串读取。设备 RMW、宽 MMIO、端口 I/O 仍不支持。请求字节、设备状态和写事件分别比较，不依赖 Unicorn 对零计数 REP 额外触发的终止 hook。KVM 使用标准 XSAVE 接口传送 XMM/MXCSR 与 FP/SSE presence bits。该 supervisor 契约不提供用户进程环境；timeout/取消在准入的有界指令间检查，不提供通用异步抢占，也不会在 guest 开始后更换后端。内置样例及可用 WDK 场景会对 normal/CFG 和重定位映像与 Unicorn 比对；语料一致不代表支持任意驱动。

checked x64 还支持带屏蔽的传统 `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN` 和 `MAX` 的 `SS`、`SD`、`PS`、`PD` 形式。`X64SSEInstructions.def` 统一定义操作数宽度、对齐和准入规则。`MaskedSSEArithmeticMatchesIndependentHostExecution` 使用独立的本机 CPU 参照验证寄存器与 RAM 形式，覆盖四种舍入模式、FTZ、有符号零、次正规输入和 NaN；`SSEMemoryObserverStopsBeforeResultAndStatusChanges` 验证停止请求发生在效果提交之前。

构建选项为 `NEVERD_EMULATION_BACKEND_KVM`、`NEVERD_EMULATION_BACKEND_WHP`。Windows API 从系统 DLL 动态加载；KVM 要求当前用户能访问 `/dev/kvm`，模拟器不修改宿主权限。交叉编译不能替代原生运行验证。新增 C 入口 `neverd_emulate_driver_backend_json`，现有 v1 结构和入口不变；新报告包含请求/实际后端、执行契约及选择原因。

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --backend auto --execution-contract checked-x64-v1
```

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

报告始终以 JSON 输出到 stdout；请求与初始化配置诊断输出到 stderr。默认预算为 100000 条来宾指令、64 MiB 来宾内存、10000 条记录事件以及 5000 毫秒。指令数限制必须为正数。执行预算耗尽会停止执行并保留已有观察结果；具体分配 API 仍遵循其空间不足返回契约，例如 MMIO 映射空间不足返回 NULL。

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
| 使用下列 API 的 x64 软件 WDM 驱动 | 有界 x64 WDM 初始化、缓冲／直接／neither 请求、工作项、定时器、DPC、事件与等待，以及行为报告和限制 | 每个额外执行到的 API 都必须具有明确的模型 |
| `METHOD_BUFFERED` IOCTL | 缓冲／直接 I/O，可由工作项或 DPC 完成 | 仅支持下列 API 子集；仅允许跨文件或同一异步文件上的有界批量提交 |
| `METHOD_IN_DIRECT`、`METHOD_OUT_DIRECT` | 请求拥有的 MDL、系统映射、共享物理页身份及 SG DMA | 下文支持范围以外的用户映射及 DMA 接口 |
| 驱动自行分配的 MDL | 独立或 IRP 关联描述符、部分 MDL、共享用户／系统映射、进程隔离的 VA 复用及完成时释放 | 配额、手工 MDL 及通用虚拟内存分配 API |
| READ/WRITE | 缓冲／直接／neither I/O，可由工作项或 DPC 完成 | 仅支持下列 API 子集；同步文件仍不支持重叠请求，也不模拟隐式文件位置 |
| WDM `METHOD_NEITHER` | 独立用户缓冲区、访问探测、MDL 锁页、合成请求进程身份、派发后 VA 撤销或进程退出及有限取消 | 未列出的通用进程及虚拟内存分配 API |
| KMDF 1.33 非 PnP 驱动 | 版本绑定、对象／上下文、具名控制设备、手动、顺序或有限或无限并行默认及非默认队列，以及实际执行回调的缓冲／直接／neither 请求 | 不支持超出已描述子集的 PnP 行为、通用队列调度、类扩展或 UMDF |
| PnP 总线／功能／过滤驱动 | 显式无资源或固定寄存器银行 PDO、来宾 AddDevice 和八种常见 PnP 生命周期次功能 | 其他 PnP 操作、通用电源管理、其他硬件／资源及 通用 KMDF PnP |
| 存储、网络、显示、文件系统及微过滤驱动 | 不支持相关子系统契约 | 端口／类／微端口框架、NDIS/WFP、图形或文件系统服务 |
| 工作项、定时器、DPC、事件与等待 | 当前执行 IRQL 在派发与工作项中为 `PASSIVE_LEVEL`，在 DPC 中为 `DISPATCH_LEVEL` | 仅支持下列 API 子集；仅允许跨文件或同一异步文件上的有界批量提交 |
| 使用进程／线程回调、句柄、注册表／文件操作或内核模块发现的驱动 | 支持配置的注册表；其他行为仅限下列 API | 对象管理器、系统状态以及回调／事件产生机制 |
| 硬件、DMA、PCI、中断或虚拟化驱动 | 显式寄存器银行、MMIO、独占／共享 latched 与 level_sensitive 中断，以及有界一致性公共缓冲区／SG／通道 DMA | 其他设备模型、任意物理 RAM、PCI、端口、其他 DMA 接口及特权 CPU 状态 |
| x86 或 ARM64 Windows 驱动 | 拒绝 | 相应架构的加载、ABI 及执行模型 |
| x64 CFG | 验证目标表及检查／分派调用；未启用的插桩保留来宾回退函数 | XFG、导出抑制、不支持的加载配置和 TLS 仍被拒绝 |

未使用的不支持导入项可以保持绑定。一旦执行到不支持的操作，便会停止并给出诊断及此前收集的观察结果。仅 DriverEntry 成功，并不能证明后续派发、硬件或框架路径也受支持。下方 API 表是受支持子集的权威定义。

## 执行契约

此配置在 CPU0 上默认以确定性的协作调度模拟 x64 WDM 生命周期。 执行从 PE 入口点开始；若存在编译器生成的入口包装函数，也会保留并执行。DriverEntry 必须返回 `STATUS_SUCCESS` 才能完成初始化；非零的成功状态或待处理状态会因初始化契约不受支持而停止。失败状态则作为已完成的初始化结果保留。所有对象、字符串、栈、函数指针与分配均位于来宾内存。模型根据配置的服务名（默认为 `NeverDDriver`）提供 `DRIVER_OBJECT` 和注册表路径。

适配器使用 Unicorn 的虚拟 TLB 模式保留来宾虚拟地址，包括规范的高位内核地址，无需合成 Windows 页表。初始 RFLAGS 为 `0x202`；软件设备配置采用固定的 64 字节缓存行。这些都是本执行场景的显式属性。内联 x64 CR8 读取观察到相同的 `PASSIVE_LEVEL` / `DISPATCH_LEVEL`；CR8 写入及其他控制寄存器操作仍不受支持。

仅绝对寻址、只读的 8 字节 `GS:[0x188]` 访问提供当前逻辑线程的借用不透明身份。编译器生成的 `MOV`、`CMP` 等形式由后端执行原始指令，保留各自的寄存器和标志语义。同线程嵌套回调保持身份，独立线程互异，已建模系统线程复用原对象。私有处理器视图只提供该字段，不建模完整 KPCR，也不授予线程结构访问权或引用所有权。其他 GS 偏移、所有 FS 访问、索引或部分宽度读取及写入仍不支持。

未知导入项绑定到延迟陷阱。未使用的导入项不会阻止执行；执行其 thunk 或读取未建模的导出数据值时，会以 `unsupported_api` 停止。不支持的 CPU 环境效果也会明确停止。NeverD 不会用成功返回值替代未实现的调用。格式错误的映像或不支持的加载要求会在执行前失败。

排队的 `DelayedWorkQueue` 工作项在 `PASSIVE_LEVEL` 执行，来宾 DPC 回调在 `DISPATCH_LEVEL` 接收规定的四个参数。CPU0 在调用返回和阻塞等待的边界默认进行确定性的协作调度。相对、绝对和周期定时器使用虚拟时间；没有可运行的执行帧时，时间推进到下一定时器、等待或取消期限。通知型与同步型事件／定时器保留各自的信号消耗语义。每个回调拥有独立的来宾栈；多个阻塞帧保留局部变量和完整 CPU 上下文，来宾内存仍然共享。Win64 回调入口将前四个参数放入寄存器，其余参数放入栈。请求默认串行处理：标记 IRP 为待处理的派发函数必须返回 `STATUS_PENDING`。跨文件或同一异步文件上的 WDM 传输可显式设置 `defer_callback_drain`，在回调运行前提交下一个请求。待处理请求或无限等待没有可用生产者时，以停滞的 `model_error` 停止。指令、内存、观察记录与墙钟时间预算仍共用。

这是有界调度模型，并不代表完整 Windows 异步支持。可警报或用户模式等待、APC、通用 WDM 请求取消、任意并发场景提交、UMDF、通用 KMDF PnP 设备及通用队列调度、完整 PnP／电源、通用硬件、其他 DMA 接口仍不支持。仅初始化调用会执行显式排队的回调，不会隐式生成请求或卸载。

工作项在回调开始前出队，因此回调可以释放自身的工作项。释放仍在队列中的项、重复入队、使用失效对象或非来宾可执行内存中的回调地址都会明确失败。设备引用保留到回调返回。请求卸载要求释放全部工作项并完成排队工作。CPU 上下文保存与恢复包含通用、SIMD、FPU 和控制状态；来宾内存始终共享，故障 CPU 不能靠恢复上下文继续执行。
删除会延后到文件对象及排队／执行中的工作项引用全部释放。对象区耗尽时，工作项分配返回 NULL。

映像默认使用首选基址，除非场景选择了有效的重定位地址。映像必须为使用 native 子系统的 PE32+ x64 可执行文件。导入可来自 `ntoskrnl.exe`、`ntkrnlmp.exe`、`HAL.dll` 或 `WDFLDR.SYS`。

执行加载器支持经验证的 x64 `DIR64` 基址重定位，以及有限的安全 cookie 加载配置；它会在入口包装函数执行前设置确定性的来宾 cookie。其他未建模的加载配置字段、TLS、延迟／绑定导入、按序号导入以及托管映像都会被拒绝。映像还必须通过严格的范围与对齐检查。

启用的控制流保护（CFG）会验证 PE 标志、指针槽和已排序的可执行目标表。检查与分派辅助函数仅允许已声明的映像入口或已登记的 API 跳板，保留 Win64 调用状态，并拒绝未声明的目标。只有插桩而未启用 CFG 时，保留原始来宾回退指针。启用的 XFG、导出抑制和其他未建模的保护策略仍被拒绝；地址位于可执行内存并不使它成为合法目标。

WDM 设备栈可以包含同一来宾驱动拥有的多个设备对象。`IoAttachDeviceToDeviceStack` 将独立源设备附加到目标的当前栈顶，返回原栈顶，并设置 `StackSize` 和 `AlignmentRequirement`；它不修改驱动的 `NextDevice` 链，也不复制缓冲标志。`IoDetachDevice` 接收保存的下层设备，要求 `PASSIVE_LEVEL`；附加允许 IRQL 不高于 `DISPATCH_LEVEL`。打开具名下层设备时，请求派发到当前栈顶，而 `FILE_OBJECT.DeviceObject` 和报告保留具名设备身份。READ/WRITE 使用所选栈顶的缓冲标志。请求保存的路径在拆链、删除后仍保留各层设备，直到派发返回；内部引用不增加表示打开句柄数的 `ReferenceCount`。

`IofCallDriver` 和 `IoCallDriver` 辅助入口调用保存路径中的确切目标。真实内联 `IoCopyCurrentIrpStackLocationToNext`、`IoSkipCurrentIrpStackLocation` 和 `IoSetCompletionRoutine` 操作原始来宾 IRP；模型验证游标、数量和控制标志。下层派发返回真实状态，与 `IoStatus` 及完成回调返回值分开。完成展开先推进游标，按成功／错误／取消标志选择回调并向上传递 pending 状态；执行完成回调时，回调负责传播 pending，包括派发已返回 `STATUS_PENDING` 后的传播。`STATUS_MORE_PROCESSING_REQUIRED` 暂停展开并保留 IRP、MDL 和缓冲区，后续完成调用可以继续。嵌套完成要求外层返回停止结果，最终展开只释放一次存储。带所属子系统标记的续接保留嵌套 WDM／WDF 调用帧和继承的 IRQL。 在调用上层完成回调之前，已消耗的下层栈位置会被清零。

`IoAllocateIrp` 与 `IoFreeIrp` 支持调用方拥有的 IRP，允许至 `DISPATCH_LEVEL`，要求 `ChargeQuota=FALSE`。首次 `IoCallDriver` 验证包并保存精确的下层设备路径。本范围支持内核 `IRP_MJ_INTERNAL_DEVICE_CONTROL` 与驱动拥有的 `METHOD_NEITHER` 缓冲区，不创建用户文件或复制用户缓冲区。来宾执行真实完成及取消回调。在完成回调内释放包必须返回 `STATUS_MORE_PROCESSING_REQUIRED`；包访问立即失效，派发与回调元数据保留至调用帧返回。未发送包可直接释放；泄漏、重复释放、外来或仍被占有的包明确失败。任意重用、其他调用方请求格式、`IoBuildDeviceIoControlRequest` 仍不支持。

调用方请求报告使用 `kind: "internal_ioctl"`、`origin: "driver_allocated_irp"`、`file: null`，保留 `code`、`irp`、派发／I/O 状态、取消时间与精确 `information_hex`。原生与 JSON 场景在加载映像前拒绝 `internal_ioctl`；只有真实来宾分配与派发才产生这些记录。

除下述 USB idle 协议外，下层目标必须是存活的来宾 WDM 设备，完成回调必须同时启用成功、错误和取消条件。真实 `driver_wdm_owned_irp.c` 样例使用 `NEVERD_WDM_OWNED_IRP_FIXTURE`／`NEVERD_WDM_OWNED_IRP_CFG_FIXTURE`。[调用方 IRP 场景](../examples/driver-owned-irp-scenario.json) 验证延迟完成。普通／active-CFG 映像覆盖首选／重定位地址，C API／CLI 保留独立内核来源记录。缺失产物明确跳过，执行证据仅来自 Linux。

示例会主动取消一个子请求，因此 CLI 退出码 2 和 `scenario_success: false` 属于预期；同时应有 `stop_reason: "returned"`、正常卸载及零样例错误。完成后的内核输出可快照到 `output_hex`，缓冲区仍由驱动拥有。

通用电源管理、其他 PnP 次功能以及其他硬件／资源模型仍不支持。除直连 FDO/PDO 文件生命周期外的 WDF 目标转发、向仍有文件或回调的栈附加设备、拆除中间层、改变转发的主功能及指向保存路径之外的设备均明确报错。可选真实 WDK 样例 `driver_wdm_stack.c` 使用 `NEVERD_WDM_STACK_FIXTURE` 和 `NEVERD_WDM_STACK_CFG_FIXTURE`；原生与 C API／CLI 测试包括重定位，缺少产物会明确跳过。执行证据仍仅来自 Linux。

场景可显式配置 `pnp_devices`，最多 64 个。每项必须包含 `id`、`bus: "resource_free"` 或 `bus: "register_bank"`、`initial_device_power: "D0"` 和 `initial_system_power: "working"`，不会猜测缺失事实。ID 为区分大小写的 ASCII，长度 1–64 字节，以字母或数字开头，其余只允许字母、数字、`_`、`-`、`.`。普通请求可用已配置的 `device_id` 代替 `device`，两者互斥。`kind: "pnp"` 必须提供 `device_id`、`minor` 和 `bus_completion`；支持 `start`、`query_remove`、`cancel_remove`、`remove`、`query_stop`、`stop`、`cancel_stop`、`surprise_removal`。`bus_completion.status` 必须为 32 位整数或十六进制字符串；可选 `delay_100ns` 是不超过 INT64_MAX 的非负整数，从提供者实际接收请求时计时。最终总线状态不能是 `STATUS_PENDING`；stop/cancel-stop/surprise-removal/cancel-remove/remove 必须精确返回 `STATUS_SUCCESS` (0)。PnP 请求拒绝文件、传输和取消字段，即使值为零；C++ API 执行相同预检。

```json
{
  "pnp_devices": [
    {"id": "sensor0", "bus": "resource_free", "initial_device_power": "D0", "initial_system_power": "working"}
  ],
  "requests": [
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0", "delay_100ns": 10}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "surprise_removal", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "remove", "bus_completion": {"status": "0x0"}}
  ],
  "unload": true
}
```

DriverEntry 成功后，每个配置的 PDO 执行一次 `AddDevice`，提供者拥有独立的 `DRIVER_OBJECT`；来宾不能删除或冒充提供者对象。PnP IRP 为 `KernelMode`、不关联文件，START 资源由配置的总线决定，初始状态为 `STATUS_NOT_SUPPORTED`。仅在转发到对应 PDO 后才使用总线响应；延迟完成复用虚拟时钟及现有完成续接。最终上层完成决定生命周期提交或回滚，与总线状态分开。正常从 Started 移除必须先成功 query、关闭文件、排空先前请求，并由来宾拆链／删除。干净的 AddDevice 失败仅退休提供者；新来宾设备泄漏会触发 `model_error`，已拆链的泄漏也不例外。卸载前所有提供者必须已退出。本范围不实现其他 PnP 次功能、通用电源管理、其他硬件／资源或 通用 KMDF PnP。 成功 PnP 必须实际完成提供者；START/QUERY_STOP/QUERY_REMOVE 的上层早期失败可保留空总线观测。设备／文件生命周期身份在拆链后仍保留。

报告在 `configuration.pnp_devices` 保留初始配置。观测的 `pnp_devices` 包含 `id`、`pdo`、可空 `add_device_status`、当前 `attached`、`pnp_state` 和 `provider_present`；移除后 `attached` 为 false。AddDevice 阶段为 `add_device:<ID>`，失败影响 `scenario_success`，但不覆盖 DriverEntry 的 `nt_status`。每个请求新增可空 `device_id` 和 `pnp`；PnP 的 `file` 为 null。`pnp` 记录 `minor`、`state_before`、`state_after`、可空 `bus_status`、`bus_received_at_100ns` 和 `bus_completed_at_100ns`。配置状态仅在总线实际完成后成为观测；接收时间单独记录。已有请求字段类型不变。

可选 `parent_id` 通过配置 ID 声明设备提供方的父节点；父节点可在数组中后出现，省略则表示独立根节点。原生与 JSON 预检在创建 PDO 前拒绝未知父节点、自引用、循环及无效 ID。该图不建立 WDM 附加链或 WDF 对象父子关系。报告中的 `parent_id` 和 `parent_pdo` 在任一提供方退出后仍保留原身份。 子节点 START 要求父节点仍存在、已 Started、处于物理 D0，且无未完成生命周期或电源转换。父节点 STOP／SurpriseRemoval 要求子节点已退出活动 PnP 状态并排空转换与 WAIT_WAKE；父节点 REMOVE 要求所有子提供方先退出。不会隐式级联处理。

除 Removing/Removed 外，设备仍存在时，普通 CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE 会进入真实来宾派发。模型不根据 Stopped、StopPending、RemovePending 或电源状态虚构失败；驱动可按自身代码完成软件 I/O、拒绝或保留请求。公开执行器默认串行；跨文件或同一异步文件上的挂起 WDM 传输可显式批量提交。当前保留 IRP 若无可用生产者，不能靠后续场景中的 start 或 cleanup 唤醒，会以停滞 `model_error` 结束。Remove 前关闭文件、排空先前请求是当前配置的限制。`query_stop` 的最终 `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119) 要求尚未实现的资源重新查询，因此场景预检和来宾最终完成都明确拒绝；参见 [Microsoft QUERY_STOP 合约](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device)。停止／重启及突然移除不实现资源重平衡。

无资源 PnP 使用原创真实 WDK `driver_wdm_pnp.c`、可选 `NEVERD_WDM_PNP_FIXTURE`／`NEVERD_WDM_PNP_CFG_FIXTURE`，并提供原生及 C API／CLI 测试；缺少产物会明确跳过，执行证据仍仅来自 Linux。

合成总线 `bus: "register_bank"` 为 PDO 提供显式固定内存资源。当 `interrupts` 非空时可省略 `resources`，两份列表合计至少包含一个资源。每个内存资源项包含 `id`、`raw_start`、`translated_start`、`length` 和 `registers`；每个寄存器必须指定 `offset`、`width`、`access`（`read_only` 或 `read_write`）以及初始 `value`。`DriverResources.h`／`DriverResources.def` 定义共享 C++ 与 JSON 契约。资源 ID 遵循有界 ASCII 标识符规则，并在每个 PDO 内唯一。上限为每 PDO 8 个／合计 32 个资源，每资源 256 个／合计 4096 个寄存器，每资源 1–1048576 字节。只支持自然对齐、精确匹配的 1／2／4 字节寄存器访问，值必须适合该宽度。两种物理区间均不得溢出；同一 PDO 内的原始区间不能重叠，转换后区间在全局不能重叠。空 `registers` 明确表示整个银行不可访问。地址和初值都是声明的事实，不来自宿主硬件，也不隐含零填充内存。`resource_free` 继续省略资源清单，START 指针保持 null。

START 接收独立、只读的原始与转换后 `CM_RESOURCE_LIST` 分配；对应的 Memory 描述符顺序一致，使用一个完整描述符、Internal 接口、总线 0、版本／修订号 1、DeviceExclusive 共享方式及 READ_WRITE 区间标志。寄存器自身的只读权限仍独立生效。下层 START 成功后，资源在上层完成回调之前可用；从 NotStarted／Stopped 发起的每次 START 都为相同固定分配建立新一轮资源身份。寄存器值在创建 PDO 时按配置初始化，取消映射、STOP 和重启均保留值。失败的 START 及成功的 STOP／REMOVE 要求驱动在最终 IRP 完成前释放映射，模型不会静默清理。突然移除立即禁止新映射和寄存器访问，但仍可取消已有映射。提供者实际成功完成设备 SET 电源请求后更新硬件可访问性：D3 禁止访问，D0 仅在资源分配可用时允许访问；D3 仍允许建立映射，D3hot 保留映射及数值；显式 D3cold 周期在 D0 访问前恢复配置的寄存器值。

`MmMapIoSpace` 支持 NonCached；`MmMapIoSpaceEx` 支持 PAGE_NOCACHE 与 PAGE_READONLY 或 PAGE_READWRITE 的组合。两者仅映射单一资源分配内已声明的转换后子区间，并保留页内偏移。别名共享同一寄存器银行、保留独立权限，`MmUnmapIoSpace` 必须使用原始基址和精确长度。映射数量／虚拟地址窗口或配置内存预算耗尽时返回 NULL，后端故障仍明确失败。已取消映射的地址不会因后续映射而重新有效。标量及真实 REP 寄存器缓冲指令通过 CPU MMIO 检查执行；空洞、错误宽度、未对齐、只读写入、跨映射访问和执行均在寄存器产生效果前失败。内存 API 可执行自然对齐、精确匹配单个寄存器的 1／2／4 字节事务；更大的批量区间若触及 MMIO，会明确失败，不会自动拆分成寄存器事务。本范围不提供任意物理 RAM、资源重平衡、端口、其他 DMA 接口或通用硬件行为。

可运行的[寄存器银行场景](../examples/driver-register-bank-scenario.json) 使用原创真实 WDK `driver_wdm_resources.c`，通过可选 `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE` 指定产物。它执行 14 个请求，覆盖延迟 START、文件 I/O、STOP、重启和移除，并通过驱动 IOCTL 观察持久寄存器值。`configuration.pnp_devices[].resources` 记录初始事实，物理地址使用无损十六进制表示；它不是另一份银行状态报告。C API 与 Python 继续使用原有 `scenario_json` 入口，不改变 `neverd_driver_options_v1`。缺少真实产物时明确跳过；当前执行证据仅来自 Linux。

同一 `register_bank` 提供者可单独声明 `interrupts`，或与 `resources` 一起声明；至少一份列表必须非空。`resource_free` 拒绝显式 `interrupts`，包括 `[]`。每个中断提供 `id`、`raw_vector`、`raw_level`、`raw_affinity`、`translated_vector`、`translated_level`、`translated_affinity`、`mode` 和 `share`。`DriverInterrupts.h`／`DriverInterrupts.def` 统一类型、拼写和限制：每 PDO 最多 8 个、总计 32 个中断；PDO 内 ID 为唯一的有界 ASCII 字符串；raw level 为 0–65535，translated DIRQL 为 3–12。向量保留完整 32 位，不推测它与 IRQL 的关系；两份 affinity 都必须为 1，仅模拟 CPU0／group0。`mode` 支持 `latched` 和 `level_sensitive`，`share` 支持 `device_exclusive` 和 `shared`。共享同一转译向量的资源必须全部声明 shared，且模式、转译层级、affinity 和重触发周期一致。电平模式必须显式提供正数 `retrigger_after_100ns`，不超过 `INT64_MAX`。raw 与 translated 值独立；`CM_RESOURCE_LIST` 保留内存描述符顺序，随后放置 type 2 中断描述符，其共享属性和触发标志反映声明。

READ／WRITE／IOCTL 可声明 `interrupt_events`，每项指定 `after_100ns`、`device_id`、`interrupt_id` 和可选 `action`；其他请求种类即使提供空列表也拒绝。延迟为不超过 `INT64_MAX` 的非负值，从请求成功提交开始计时并捕获当时的连接及 PDO 资源代次。latched 使用默认 `action: "pulse"`，每个脉冲调用全部已捕获 ISR；level_sensitive 只接受显式 `assert`／`deassert`。电平由各 PDO／资源／代次的来源状态取 OR 得到，重复 assert 幂等；同一边界先应用全部外部状态变化，再采样。每轮 ISR 后在当前虚拟时间加 `retrigger_after_100ns` 时重新采样，按注册顺序调用直到 ISR 返回 TRUE；TRUE 只表示认领，不会撤销来源电平。来源 IRP 完成不取消事件，寄存器写入不推导 enable、status 或 acknowledgment。默认仅在空闲时推进时间；`scheduling` 也会在指令执行中推进期限。ISR 在受支持的回调边界先于 DPC／工作项运行；`after_100ns: 0` 不保证指令级抢占。每请求最多 64 个事件、总计 1024 个声明事件；实际递送批次另限 4096，每批最多 32 个处理器，并继续受调度与指令预算约束。连接丢失、资源代次失效或物理 D3 记录 `undelivered_reason` 并以 `model_error` 停止，不重新绑定或静默延期。

`IoConnectInterrupt` 使用真实十一参数 ABI 并精确匹配转译资源；`IoConnectInterruptEx` 支持 FullySpecified（1）、LineBased（2，显式 PDO 上的一条线）及 FullySpecifiedGroup（4，group0），断开时必须匹配版本和上下文。连接与断开要求 `PASSIVE_LEVEL`。独占及共享 latched／level_sensitive 线路可使用私有锁或调用者已初始化的共享 `KSPIN_LOCK`；共同 `SynchronizeIrql` 不低于各自配置 DIRQL，LineBased 的零值选择配置层级。ISR 调度优先级仍按配置 DIRQL，执行时遵循同步锁层级。`KINTERRUPT` 不透明且地址不复用。ISR 接收 `(Interrupt, ServiceContext)`，AL 的 BOOLEAN 表示是否认领，FALSE 不是 NTSTATUS 失败。`KeSynchronizeExecution` 在同一锁下执行真实单参数回调，返回 BOOLEAN 并恢复调用者 IRQL／CR8。ISR、同步回调及手动 `KeAcquireInterruptSpinLock`／`KeReleaseInterruptSpinLock` 共享实际锁所有权，验证非递归、执行身份和保存的 IRQL，不能持锁返回。仍不支持 ISR 浮点保存和任意嵌套中断。失败 START 和成功 STOP／REMOVE 要求最终完成前断开，上层完成回调可先清理；断开不丢弃已排队 DPC。

报告区分声明与观测：`configuration.pnp_devices[].interrupts` 保留资源，`configuration.interrupt_events` 保留事件及 `action`、从零开始的 `source_request_index` 和 `event_index`。根 `interrupts` 记录设备／资源、代次、到期时间和状态应用时间；`occurred_at_100ns` 表示外部状态已应用。`handlers[]` 对每次真实 ISR 记录中断对象、递送／返回时间、BOOLEAN 返回值、`claimed` 和 `delivery_index`；assert 可产生多轮。deassert 或同边界被抵消的 assert 没有 ISR 时，不虚构 ISR 字段。未认领的 ISR 是有效结果；DPC 效果通过真实请求完成、API 调用及消息体现。行字段保留 `device_id`、`interrupt_id`、`epoch`、`due_at_100ns`、`occurred_at_100ns`、`interrupt_object` 和 `undelivered_reason`。`delivered_at_100ns` 记录首次处理器入口；顶层 `returned_at_100ns`、`return_value` 与 `claimed` 汇总最近一批，`handlers` 保留完整历史。首次 assert 承载来源持续有效期间的观测，重复 assert 可无自己的 ISR 记录。可执行的[中断场景](../examples/driver-interrupt-scenario.json)使用原创真实 WDK `driver_wdm_interrupts.c`，由可选 `NEVERD_WDM_INTERRUPT_FIXTURE`／`NEVERD_WDM_INTERRUPT_CFG_FIXTURE` 指定：七个请求包括延迟 START、由 ISR→DPC 完成的 pending IOCTL、文件清理／关闭及移除。现有 C／Python `scenario_json` 边界和 `neverd_driver_options_v1` 布局不变。缺少真实产物明确跳过，执行证据仅来自 Linux。

`DriverDMA.h`／`DriverDMA.def` 为 `register_bank` PDO 增加可选的 `dma` 对象，与内存／中断分配并存；仅声明 DMA 不能取代这两类资源列表。七个字段都必须显式提供：`address_bits`（32 或 64）、`maximum_length`（1–1048576 字节）、`map_registers`（1–256）、`alignment`（1–4096 之间的二次幂）、`logical_base`（非零、页对齐）、`logical_length`（页对齐、4096–1073741824 字节）及布尔值 `scatter_gather`。逻辑地址窗口必须无溢出且落在地址位宽内。每个 PDO 都有独立逻辑地址域，不同设备的相同地址不会互为别名。转换后的 MMIO 资源不能与保留的模型 RAM 区间 `[0x1000000000, 0x1004000000)` 重叠。这些声明描述具备一致性的合成总线主设备，不代表宿主物理内存或 PCI 设备。

`IoGetDmaAdapter` 接受 Internal 总线主设备的历史 `DEVICE_DESCRIPTION` 版本 0／1 字段，发布版本为 1 的 `DMA_ADAPTER` 及真实的 104 字节 `DMA_OPERATIONS` 表。版本 2／3 探测返回 NULL，不读取现代结构尾部。每个间接方法绑定到确切的有效适配器，身份独立于内核导入。已实现 `AllocateCommonBuffer`、`FreeCommonBuffer`、`GetDmaAlignment`、`GetScatterGatherList`、`PutScatterGatherList`、`PutDmaAdapter`、`AllocateAdapterChannel`、`MapTransfer`、`FlushAdapterBuffers` 和 `FreeMapRegisters`。本配置不建模从属／系统 DMA 控制器，因此 `FreeAdapterChannel` 和 `ReadDmaCounter` 仍保留具名的不支持错误。公共缓冲区分配／释放及对齐查询要求 PASSIVE_LEVEL；Get／PutScatterGatherList 要求 DISPATCH_LEVEL，适配器释放允许 IRQL 不高于 DISPATCH_LEVEL。x64 忽略 `CacheEnabled`。不支持的版本探测、声明能力不兼容以及文档规定的分配资源不足返回 NULL；非法或未建模的接口选择及后端故障仍明确报错。

`AllocateAdapterChannel` 要求 DISPATCH_LEVEL，预约非 NULL 的不透明映射寄存器令牌。公共缓冲区、SG 列表和通道预约共享每 PDO 的同一额度及 FIFO。成功即接纳真实的立即或排队 `AdapterControl` 回调；请求个数过大时返回 `STATUS_INSUFFICIENT_RESOURCES`，不执行回调。每个来宾设备最多允许一个尚未结束的分配回调，禁止在 AdapterControl 内调用 AllocateAdapterChannel，包括经过嵌套回调的调用。回调四个参数包含注册时实际 `DEVICE_OBJECT.CurrentIrp` 的快照。来宾设备的该确切八字节字段可写，只接受零或经过此设备路由的有效 IRP。排队回调保持该包有效直到进入回调，此后回调可以完成它。当前仍无 StartIo，因此另一种 SG 回调的未使用 IRP 参数仍为 NULL。

`AdapterControl` 返回 32 位 `IO_ALLOCATION_ACTION`，忽略 RAX 高位，且该动作不会替换 AllocateAdapterChannel 的 `STATUS_SUCCESS`。`DeallocateObject` 在回调返回时释放未使用或已整体刷新完毕的寄存器；`DeallocateObjectKeepRegisters` 保留寄存器，直到 FreeMapRegisters 使用确切适配器、令牌及原始个数释放。返回 `KeepObject` 需要未建模的系统控制器，因此明确失败。新递送的分配在回调返回之前还不能作为已保留分配释放；其他先前保留的分配可按其自身契约释放。回调身份、保留的寄存器及正在映射的数据字节具有独立生命周期，适配器或设备销毁前会分别检查。

`MapTransfer` 和 `FlushAdapterBuffers` 允许 IRQL 不高于 DISPATCH_LEVEL；通道分配和寄存器释放要求 DISPATCH_LEVEL。MapTransfer 接受相对 MDL 的位置，读取并更新真实 ULONG 长度，按值返回逻辑地址。有界 SG 配置每次返回一个底层页片段，同一 MDL 和方向上紧接的后续位置扩展同一次操作。非 SG 在预约个数足够时一次映射整个请求范围，不缩短长度。第一次映射按寄存器预约大小预留永不复用的逻辑窗口，其他分配可穿插但不会与其重叠。所有片段共用一个原地增长的物理固定范围。设备事务可以跨越当前已映射的整个操作，但 CPU 访问、MDL 释放以及会退休固定存储的完成操作，在整体刷新前均被禁止。Flush 必须匹配最初位置、MDL、方向及实际映射总长度。它只释放映射字节而不释放寄存器，因此保留令牌可用于下一次操作。部分刷新、混合 MDL 操作及其他 MapTransfer 模式处于此配置范围之外，不意味着所有 Windows 系统都会认定这些模式非法。

`KeFlushIoBuffers` 验证有效的已锁定／非分页 MDL。模型平台具有缓存一致性，因此 ReadOperation 和 DmaOperation 的任意取值均无需额外缓存副本；此调用不释放 DMA 所有权，也不替代 FlushAdapterBuffers。[通道场景](../examples/driver-dma-channel-scenario.json)运行原创 `driver_wdm_dma_channel.c`，执行两次 MapTransfer、一个跨页设备事务、独立声明的 IRQ/DPC、整体刷新及确切寄存器释放。真实普通／CFG 镜像使用 `NEVERD_WDM_DMA_CHANNEL_FIXTURE` 和 `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`。

由加载器拥有的镜像区间支持的系统 MDL 别名允许访问所描述 4 KiB 页中的已知镜像字节。即使底层页面已经映射，访问声明镜像范围之外的填充字节也会失败。`ByteOffset` 和 `ByteCount` 仍表示逻辑缓冲区范围，并不是页内 CPU 权限边界。范围之外的镜像字节与原始视图共享，保护和解除映射则作用于完整映射。对象存储仍保留模型对填充区的范围限制。

`KernelPhysicalMemory` 为现有 RAM 分配最多 16384 个、每页 4096 字节的模型物理页身份。CPU 虚拟地址、物理页身份和设备逻辑地址彼此区分。已构建 MDL 的 PFN 数组以只读方式暴露这些共享身份，未构建描述符没有可用 PFN。相邻小分配可以共享 PFN，但字节范围和生命周期仍独立。公共缓冲区、池和请求缓冲区使用 `GuestMemory` 已有的同一份字节，不增加 DMA 数据副本。有效 SG 映射固定确切数据范围及描述符；完成、池／MDL 释放和拆除在退休存储前拒绝尚存依赖。解除直接 MDL 映射只撤销 CPU 系统映射，DMA 仍可访问锁定的底层 RAM。`DmaWritable` 将写锁定契约与 CPU 映射权限分开记录：设备写入要求直接 READ／OUT_DIRECT 或可写非分页存储；WRITE／IN_DIRECT 不会因为 CPU 映射可写就取得该许可。

`GetScatterGatherList` 按 MDL 原始范围验证 CurrentVa／Length，并在现有底层 RAM 上生成逻辑页片段。映射寄存器可用时，真实的四参数 void `AdapterListControl` 在 API 返回前内嵌执行；否则接纳过程保留数据／描述符，并为 PDO 的 FIFO 预留回调，直到资源释放。此范围没有 StartIo 所有权，因此回调第二个 IRP 参数为 NULL。回调返回不会释放映射。`PutScatterGatherList` 可以在回调内执行；Put 后驱动可以完成请求并释放最后一个适配器，而回调续接和设备引用持续到返回。有效 SG 映射期间，CPU 必须先 Put 才能访问数据；仍在等待映射寄存器的回调尚未把字节交给设备独占。释放公共缓冲区必须匹配原适配器、长度、逻辑地址和 CPU 地址。逻辑地址在整个会话中永不复用，重启也不例外。缺少真实生产者的资源等待会明确停滞，不虚构完成或截止时间。

只有 READ／WRITE／IOCTL 请求接受 `dma_events`。每个事件必须提供 `after_100ns`、`device_id`、`logical_address`、`direction` 和 `length`；`write_memory` 还必须提供长度准确的 `data_hex`，`read_memory` 则拒绝该字段。方向以设备为视角。上限为每请求 64 个事件、合计 1024 个、事务字节总计 16 MiB、每事务 1 MiB；延迟范围为非负至 INT64_MAX。提交捕捉 PDO 当前已分配资源代次并确定虚拟时间起点，但不要求后续派发尚未创建的映射已经存在。递送时解析完整有效逻辑范围及方向，要求物理 D0，并在任何事务效果前验证全部底层字节。源 IRP 完成不会取消事件。映射缺失或已释放、陈旧资源代次、突然移除或 D3 都会记录失败并停止；不会重新绑定，也不虚构中断、寄存器协议或 IRP 完成。在同一调度边界，提供者先发布硬件状态，然后执行 DMA 字节访问，最后处理独立声明的中断脉冲。默认采用协作式时间；`scheduling` 启用指令驱动的期限与抢占。

报告保留 `configuration.pnp_devices[].dma` 和平铺的 `configuration.dma_events`。根级 `dma_transfers` 行标明 `source_request_index`、`event_index`、`device_id`、`epoch`、`logical_address`、`direction`、`length` 和 `due_at_100ns`，并提供可空的 `occurred_at_100ns`、`completed_at_100ns`、`mapping`、`adapter`、`failure_reason`；`data_hex` 仅包含实际传输字节。所有声明的事务都必须无失败完成，`scenario_success` 才能成立。[DMA 场景](../examples/driver-dma-scenario.json)使用原创真实 WDK `driver_wdm_dma.c` 和可选 `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE`，通过真实适配器指针操作公共缓冲区，并以独立声明的 ISR→DPC 完成请求。C／Python 仍使用 `scenario_json`，不修改 `neverd_driver_options_v1`。缺少产物明确跳过，证据仅限 Linux。从属控制器、V2／V3 方法、硬件描述符引擎、通用 KMDF DMA 及其他设备模型仍不支持。

WDM remove-lock 使用真实导出 `IoInitializeRemoveLockEx`、`IoAcquireRemoveLockEx`、`IoReleaseRemoveLockEx` 和 `IoReleaseRemoveLockAndWaitEx`；不带 Ex 的 WDK 名称是宏。锁属于完整对齐存储所在扩展的确切 DEVICE_OBJECT，与 PDO 生命周期和 Tag 形状无关；附加前即可初始化。支持 retail 32 字节和 DBG 120 字节，必须传入匹配的独立大小参数，注册后整个区域不透明。NULL 和重复 Tag 按每把锁计数，Tag 从不解引用，因此 IRP 完成后仍可释放。初始化与 AndWait 要求 `PASSIVE_LEVEL`，acquire/release 允许 `DISPATCH_LEVEL`。

AndWait 关闭获取入口，释放一次匹配获取，并挂起真实来宾帧，直到其余获取全部释放；之后 acquire 返回 `STATUS_DELETE_PENDING` 且不产生释放义务。最后一次 release 在释放回调返回前锁存就绪，允许工作项 release 后等待 REMOVE 续接发出的事件。模型不会制造回调、超时或无生产者的成功。当前 AndWait 要求所有者位于关联的活动 REMOVE 路径，且提供者已实际接收（`bus_received_at_100ns` 可以为零），无需等下层完成。下层在到达提供者前排队 REMOVE 不在本范围内；此检查不是完整 OutsideRemoveDevice／Driver Verifier。REMOVE 前仍要求关闭文件并排空先前请求，但允许尚未结束的回调释放锁。保留路径覆盖等待、来宾拆链／删除和下层 pending；所有相关回调帧返回后才最终释放路径。

未知或大小不匹配的存储、不匹配 release、重复 drain、重初始化，以及仍有获取或未消费 drain 等待时删除扩展，均在修改前失败。干净的 AddDevice 失败可以删除已初始化但未使用的锁。锁不替代真实设备／工作项引用，只在扩展物理退休时注销；有效调试元数据不启用 Verifier 超时／高水位行为。真实 WDK `driver_wdm_remove_lock.c` 通过 `NEVERD_WDM_REMOVE_LOCK_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` 提供四种变体。缺少产物明确跳过；原生与 C API／CLI 证据仅来自 Linux，不代表完整移除管理或通用并发 I/O 排空。

WDM 电源请求使用 `kind: "power"` 和已配置的 `device_id`。每个包必须显式提供 `minor`（`query`／`set`）、`power_type`（`device`／`system`）、`power_state`（`D0`／`D2`／`D3` 或 `working`／`sleeping3`）、`power_action`（`none`／`sleep`）、32 位整数或十六进制字符串 `system_context` 及 `bus_completion`。不支持 System Query 到 Working；拒绝文件、传输及取消字段。`system_context` 原样作为不透明事实保留，不用于推断父请求、休眠或快速启动。路径必须具有 `DO_POWER_PAGABLE` 且没有 `DO_POWER_INRUSH`；真正的可分页电源派发在 `PASSIVE_LEVEL` 执行。`PoCallDriver` 转发同一受管理的电源 IRP；`PoStartNextPowerIrp` 遵循 Vista+ 无额外串行化握手的契约。通用电源策略、其他状态／动作、关机／休眠、浪涌、不可分页路径、通用硬件及 通用 KMDF PnP 不受支持。

Query/Set `PoRequestPowerIrp` 接受最高 `DISPATCH_LEVEL` 的调用方。在 APC 或 DISPATCH 提交会保留独立 IRP 和待完成的电源事务并立即返回 `STATUS_PENDING`；实际设备状态直到完成才改变。调用方 IRQL 和 CR8 原样保留。调度器随后在 `PASSIVE_LEVEL` 执行真正的可分页 DispatchPower，因此 DPC 唤醒回调可直接请求 D0，无需分配来宾工作项。PASSIVE 调用仍保留内联派发与提前完成。准入失败不消费响应 FIFO、报告行、包存储或调度容量。完成回调保留实际完成路径的 IRQL。WAIT_WAKE 提交仍要求 `PASSIVE_LEVEL`。参见[提升 IRQL 的电源场景](../examples/driver-wdm-elevated-power-scenario.json)。

原生 WDM 驱动可通过 `PoRequestPowerIrp(IRP_MN_WAIT_WAKE)` 请求系统 `Working` 或 `Sleeping3`。提交要求 `PASSIVE_LEVEL`、稳定的物理 D0、没有正在进行的设备／系统电源事务，以及真实成功的下层 START 确认；允许在该 START 返回上层之前提交。可选输出 `PIRP` 在来宾派发前写入，完成回调可独立省略。预检失败不改变输出、报告行或分配预算。已发出的请求返回 `STATUS_PENDING`，包括提供方同步拒绝；报告 `origin: "PoRequestPowerIrp"`、`response_index: null`，不消费 `requested_device_power`。公开场景电源包仍只接受 Query/Set；经框架管理路径提交原生 WAIT_WAKE 在分配前被拒绝。

每个提供方只保留一个原生或框架 WAIT_WAKE。缺少或全假的 `wake_capabilities` 以 `STATUS_NOT_SUPPORTED` 完成，超出支持的系统唤醒范围以 `STATUS_INVALID_DEVICE_STATE` 完成，重复请求以 `STATUS_DEVICE_BUSY` 完成。真实提供方完成先执行普通 IoCompletion 链（包括 `STATUS_MORE_PROCESSING_REQUIRED`），再执行最终五参数 void `REQUEST_POWER_COMPLETE`；独立 `IO_STATUS_BLOCK` 快照有效至回调返回。上层不能凭空成功完成唤醒。`IoCancelIrp` 遵循真实取消锁协议调用已安装的提供方取消例程，仅在实际调用时返回 TRUE；完成过程保留调用方 IRQL。 成功事件还须按实际 Working／Sleeping3 状态分别验证提供方独立的 S0／Sx 能力；IRP 的 Sleeping3 上限不授予 S0 唤醒能力。

原生 `wake` 事件捕获已经保留的精确 IRP 及成功 START 身份，取消／重发和 STOP／重启不能改变事件目标。唤醒记录 `wake_source_device_id`／`wake_source_pdo`，不改变 D0/D2/D3 或 Working/Sleeping3；驱动须另发电源请求。原始提交驱动须在不兼容的拆除前取消并等待 WAIT_WAKE 和回调结束。只有带实际取消观测且由提供方取消完成的原生 WAIT_WAKE 才算预期控制流。原始 WDM 子设备唤醒传播仍不支持。

原生 WDM USB 选择性空闲要求独立于 `bus` 的显式 `usb_idle` 配置：`role` 为 `independent_function`、`composite_parent` 或 `composite_function`；组合功能的直接 `parent_id` 必须指向组合父级。`remote_wake` 默认为 false，true 还须声明通用 `wake_capabilities`，实际 USB 唤醒要求 D2；不能仅凭 D3hot 能力推导 USB 唤醒。`power_policy_events` 的 `usb_idle_permission` 指向独立功能或组合父级，不接受 `component`／`state`，捕获所有成员保留的 IRP 与成功 START 身份。任一组合功能缺少注册时整批拒绝；取消重发或重启不会改变已捕获目标。

驱动在稳定 S0/D0、`PASSIVE_LEVEL` 分配并发送真实 `IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION`：内核 `METHOD_NEITHER`、恰好 16 字节回调信息、无输出，可直达显式 provider 或经来宾 FDO 转发。provider 借用信息记录至完成，捕获的 context 保持不透明且可为 NULL。许可在 PASSIVE 执行单参数 void 回调；回调须发出一次真实 SET D2 并等待其终端完成，实际分配失败允许取消后返回。之后 idle IRP 继续挂起。进入前取消撤销排队回调；进入后取消等待回调返回。D0 收到时先成功完成 idle，再独立确认 D0；D3 收到时以 `STATUS_POWER_STATE_INVALID` 完成，S3／移除收到时取消。最终 IoCompletion 沿用真实释放／`STATUS_MORE_PROCESSING_REQUIRED` 所有权。

请求 `usb_idle` 记录 `start_epoch`、实际收到、回调进入／返回、`d2_irp` 及结果、`completion_cause`、认领与最终完成时间，未观测字段为 null。事件 `usb_idle_members` 保留精确设备／PDO／IRP／START 身份，不伪造电源转换。只有实际 provider 与原因匹配的取消／D3 失效才算预期控制流，重复注册的 `STATUS_DEVICE_BUSY` 仍使场景失败。[USB idle 场景](../examples/driver-wdm-usb-idle-scenario.json) 使用 `NEVERD_WDM_USB_IDLE_FIXTURE`／`NEVERD_WDM_USB_IDLE_CFG_FIXTURE`。USB 描述符、URB、管道与传输目标仍不支持。

KMDF `IdleUsbSelectiveSuspend` 支持 `DriverManagedIdleTimeout` 和显式 D2。`PowerDeviceMaximum` 仅从功能设备显式声明的 `usb_idle.device_wake: "D2"` 解析，缺省仍为未知。独立的 `remote_wake` 布尔值不会提供该总线事实，`device_wake` 也不会启用远程唤醒；组合父级不接受 `device_wake`。USB 超时零采用默认 5000 ms，禁用用户覆盖，`PowerUpIdleDeviceOnSystemWake` 必须为 `WdfUseDefault`。

超时在 D0 保留真实框架 idle IRP，随后 `usb_idle_permission` 才驱动框架回调和真实 D2 确认，组合设备必须取得完整成员许可。远程唤醒同时要求显式 USB 能力与通用 S0 能力，并使用独立真实 `WAIT_WAKE`。受管 I/O 和 StopIdle 先取消原 idle 包，`framework_usb_idle` 行记录 `cancel`／`STATUS_CANCELLED`，受管请求仍等待独立 D0 确认及 D0Entry 后才能投递。此前未取消的唤醒路径在收到 D0 时完成 idle。报告的 file／response-index 为 null，保留与 WDM 相同的收到／回调／D2 证据。[KMDF USB 场景](../examples/driver-kmdf-usb-idle-scenario.json) 使用 `NEVERD_KMDF_USB_IDLE_FIXTURE`／`NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`。

USB 也支持在首次 D0 进入完成前配置 `SystemManagedIdleTimeout` 与 `SystemManagedIdleTimeoutWithHint`：前者忽略驱动超时，后者为 PoFx 提供下限，不增加本地定时器。仅 `idle` 不允许设备降电；显式 `power_not_required` 保留真实 USB idle IRP，并在物理状态仍为 D0 时确认 PoFx。独立 `usb_idle_permission` 才允许真实回调和 D2 请求。此前到达的活动在 D0 取消 idle，不虚构额外 D0 IRP；D2 后的受管 I/O 和 StopIdle 等待真实 D0 确认／D0Entry，再等待 F0／ActiveCondition 完成。远程唤醒自身恢复组件，不依赖后续 I/O。arm 或分配失败在真实取消／disarm 后恢复活动；STOP／REMOVE 回收所有者，重启使用新身份。[USB PoFx 场景](../examples/driver-kmdf-usb-pofx-scenario.json) 展示双许可与独立的设备／组件恢复。 `RemovePending` 期间真实 `EvtDeviceD0Entry` 失败时，仅为结束失败 IRP 和硬件清理而确认待处理 Required，不调用 F0／ActiveCondition。这不允许仍在场设备的 SET_POWER 失败，也不增加框架自主意外移除。

`WdfDeviceConfigureRequestDispatching` 在 IRQL <= DISPATCH_LEVEL 将 READ、WRITE 或外部 IOCTL 映射到同设备的有效非默认队列。自动队列须有对应回调或 `EvtIoDefault`，手动队列沿用取回语义；重复映射返回 `STATUS_WDF_BUSY`。未映射请求保留默认队列，已投递请求保留原所有者，映射队列不能独立删除。`WdfDeviceEnqueueRequest` 捕获当时选定的队列；后续映射不能迁移已入队请求。本模型明确不支持 CREATE 与内部 IOCTL 队列映射，文件回调和类型化 USB provider 分发保留各自所有权。真实测试同时覆盖直接 READ 与 `WdfRequestForwardToIoQueue`，均等待真实 D0 就绪后投递。

[原生 WAIT_WAKE 场景](../examples/driver-wdm-wait-wake-scenario.json) 使用真实 WDK `driver_wdm_wait_wake.c`，通过 `NEVERD_WDM_WAIT_WAKE_FIXTURE`／`NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE` 运行。测试覆盖实际 START 中提交、回调参数、唤醒不自动 D0、重发、取消、MPR、DPC 取消后工作线程提交 D0、精确事件捕获和独立提供方。普通／active-CFG 及首选／重定位地址执行证据仅来自 Linux；缺少文件明确跳过。

D2 是独立设备状态，不会映射成 D3。native／JSON 电源包、`requested_device_power` 与 `initial_reported_device_power` 接受 D0／D2／D3；`PoSetPowerState` 只更改调用对象的通知历史，D2／D3 通知要求 IRQL <= APC_LEVEL。`DeviceLifecycle::validateDevicePowerRequest` 在不修改待处理操作、不消费 ticket 的前提下拒绝不同低功耗状态之间的直接 SET。框架 S3 的目标与当前 Dx 不同时，必须先完成真实 D0 子请求，再发送所选 Dx 子请求，两步分别消费显式 FIFO 响应。D0 回调收到精确的 D2 前态／目标值。

S0 空闲与 Sx 唤醒设置接受显式 `DxState` D2 或 D3，执行及重新赋值均保留所选目标。本 provider 既有 `wake_capabilities` 承诺从 D3hot 唤醒，也覆盖 D2；它不会推导总线 `DeviceWake`。D1 与非 USB 的 `PowerDeviceMaximum` 仍不支持。D2 禁止 MMIO、DMA 与普通中断投递。register-bank provider 在 D2 保留配置寄存器、映射别名、资源分配和公共 RAM；这是该模型的契约，不代表任意硬件都会保留上下文。D2 不能进入 D3cold 或推进复位代次。 空闲 `DxState` 为 D2 时，即使后续系统转换选择 D3，也不能授权 D3cold。已处于冷态的 provider 再次收到 SET D3 时，真实电源请求仍完成，但保持原有冷态代次。

[D2 电源场景](../examples/driver-d2-power-scenario.json) 使用 `NEVERD_KMDF_CHILD_WAKE_FIXTURE`／`NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE`。native 与真实 WDK 测试覆盖 D2／D0 包、回调顺序与次数、直接 Dx 互转拒绝和系统睡眠前的真实 D0 准备；C API／CLI 保留相同观察。普通／active-CFG 映像覆盖首选／重定位地址，缺失样例明确跳过，执行证据仅来自 Linux。 回调状态参数的精确值由框架模型测试单独验证。

每个 `pnp_devices` 条目可提供 `initial_reported_device_power: "D0"` 或 `"D2"` 或 `"D3"`，与必填的初始生命周期 D0／working 独立。PDO 与首次关联的每个来宾 DEVICE_OBJECT 各有独立通知状态；`PoSetPowerState` 只返回并更新调用设备的前值。缺少该事实时，实际调用会失败，不猜测 D0。可选 `requested_device_power` 保存相同六项必填事实的 device 类型模板；所有 PDO 合计最多 64 项。只有实际 `PoRequestPowerIrp(Query/Set)` 的 PDO、minor 和目标匹配该 PDO 的 FIFO 队首时才消费；缺失／不匹配报错，未消费条目不生成请求，也不根据回调 context 猜父请求。子请求有独立 IRP 和报告行，`origin: "PoRequestPowerIrp"` 及从零开始的 `response_index`；场景行使用 `origin: "scenario"` 和空索引。同步子请求可在 API 返回 `STATUS_PENDING` 前执行五参数 void 回调；回调可等待，System S0 可先于独立 D0 子请求完成。回调的 IO_STATUS_BLOCK 快照一直有效到回调返回。

请求报告增加可空的 `power`，电源行的 `file` 为 null。`power` 记录包事实、`device_state_before`／`device_state_after`、`system_state_before`／`system_state_after`、可空的 `requested_device_object` 及实际 `bus_status`／`bus_received_at_100ns`／`bus_completed_at_100ns`。最终 PnP 设备增加 `device_power`／`system_power`；存活设备增加可空的 `reported_device_power`。`scenario_success` 只用场景来源行核对配置数量，但所有实际场景／子请求都必须成功完成；未消费模板不导致失败。真实 `driver_wdm_power.c` 使用 `NEVERD_WDM_POWER_FIXTURE`／`NEVERD_WDM_POWER_CFG_FIXTURE`，覆盖普通／active-CFG 原生与 C API／CLI 路径；缺少产物明确跳过，执行证据仅来自 Linux。

[完整电源场景](../examples/driver-power-scenario.json) 可通过 `--scenario` 运行真实样例，包含启动、系统查询／睡眠／唤醒、移除和三个显式子响应。

KMDF 1.33 支持使用精确的 1.33.0 ABI：458 个函数槽具有稳定的来宾身份，下列 API 实现了执行语义。`WdfVersionBind` 和 `WdfVersionUnbind` 在真实 WDK `FxDriverEntry` 包装函数前后管理来宾绑定。`WdfGetDriver` 读取公共驱动全局结构。非 PnP 驱动、通用对象、控制设备、队列和传入请求共享类型化上下文、引用计数，以及实际执行的清理／销毁／卸载回调。除锁 API、自动串行化回调、中断 DDI 及下文明示支持其他 IRQL 的请求／对象操作外，已建模的框架调用和回调仍要求 `PASSIVE_LEVEL`；清理完成后新增引用仍不在此配置的支持范围内。未建模的函数槽、`WdfLdrQueryInterface`、类扩展和 UMDF 会明确停止。

中断 API: `WdfInterruptCreate`, `WdfInterruptQueueDpcForIsr`, `WdfInterruptQueueWorkItemForIsr`, `WdfInterruptSynchronize`, `WdfInterruptAcquireLock`, `WdfInterruptReleaseLock`, `WdfInterruptEnable`, `WdfInterruptDisable`, `WdfInterruptWdmGetInterrupt`, `WdfInterruptGetInfo`, `WdfInterruptGetDevice`.

框架中断复用已分配的线中断／MSI 资源、连接及锁；ISR 按 DIRQL 或 `PASSIVE_LEVEL` 执行，`WdfInterruptGetInfo` 如实报告，DPC／工作项分别在 `DISPATCH_LEVEL`／`PASSIVE_LEVEL` 执行。挂起回调保留所有权直到返回，排队重复项会合并。普通中断在 D0Entry 后启用，在 D0Exit 前禁用并排空回调。`ReportInactiveOnPowerDown` 保留 inactive 连接，`WdfInterruptReportInactive`／`WdfInterruptReportActive` 使用同一状态管理；最终释放硬件才断连。被动 `CanWakeDevice` 要求分配资源显式设置 `wake_capable: true` 并已 arm，在 Dx 保持启用且不报告 inactive。真实场景脉冲请求唤醒，ISR 等到实际 D0 及成功 D0Entry 后才执行，不虚构硬件信号。

锁 API：`WdfSpinLockCreate`, `WdfSpinLockAcquire`, `WdfSpinLockRelease`, `WdfWaitLockCreate`, `WdfWaitLockAcquire`, `WdfWaitLockRelease`, `WdfObjectAcquireLock`, `WdfObjectReleaseLock`。外部 `WDFSPINLOCK`／`WDFWAITLOCK` 与中断对象复用 executive／dispatcher 锁状态，删除前验证持有者、等待者和父对象引用。自旋锁提升到 `DISPATCH_LEVEL` 并恢复原 IRQL；等待锁归实际线程所有，等待及持有期间禁用普通内核 APC，支持无限、相对、绝对和零超时。零超时要求低于 `DISPATCH_LEVEL`，其他等待要求 `PASSIVE_LEVEL`。已分配给中断的自旋锁必须使用中断锁 API。设备／队列同步范围和继承执行级别通过所选父锁串行化 I/O、文件、取消及中断延后回调，挂起期间保留所有权。被动回调锁可以等待，dispatch 回调锁提升 IRQL，并拒绝无法阻塞的争用。自动回调允许同线程嵌套，显式对象锁不可递归。中断 DPC／工作项自动串行化要求兼容的父执行级别；ISR 独立使用中断锁。

电源策略 API：`WdfDeviceInitSetPowerPolicyEventCallbacks`, `WdfDeviceAssignS0IdleSettings`, `WdfDeviceAssignSxWakeSettings`, `WdfDeviceStopIdleNoTrack`, `WdfDeviceResumeIdleNoTrack`, `WdfDeviceStopIdleActual`, `WdfDeviceResumeIdleActual`。驱动管理的空闲策略要求明确的正数毫秒超时、`IdleCannotWake`／`IdleCanWake` 和无用户覆盖。S0 与 Sleeping3 唤醒围绕实际保留的 `WAIT_WAKE` IRP 执行 arm、triggered、disarm 回调。StopIdle／ResumeIdle 使用成对、可嵌套的电源引用；`StopIdle(TRUE)` 要求 `PASSIVE_LEVEL`，真实调用帧等待 D0 完成；FALSE、ResumeIdle 和设置 API 允许至 `DISPATCH_LEVEL`，回调注册要求 `PASSIVE_LEVEL`。电源管理队列回调或断电期间同步 StopIdle 会明确报告死锁。Sx 唤醒只记录信号，仍须显式系统 Working 请求。`PowerUpIdleDeviceOnSystemWake` 决定不能唤醒的空闲设备是否保持 Dx 至出现活动。通用 PEP／平台策略及失败设备自动重新枚举仍不支持。

声明的 `parent_id` 关系支持 KMDF Sx 子设备唤醒。`ArmForWakeIfChildrenAreArmedForWake` 与 `IndicateChildWakeOnParentWake` 是独立布尔设置。`EvtDeviceArmWakeFromSxWithReason` 分别接收自身与子设备原因，仅子设备需要唤醒时为 `(FALSE, TRUE)`。只有成功完成 Sx arm 且保留真实 `WAIT_WAKE` 的子设备参与；父节点捕获其 PDO 与 START 代次。父唤醒先预检整个完成批次，再完成这些 IRP，且只通过启用传播的节点递归。禁用、已解除、arm 失败或已重新启动的子设备不会被虚构唤醒。成功 WAIT_WAKE 的可空 `wake_source_device_id` 与 `wake_source_pdo` 记录原始来源；每个设备仍须通过自己的响应 FIFO 显式执行 Working 与 D0 请求。

须先 arm 子设备，再使父节点进入 Sx/Dx。直接父节点已在 Sx/Dx 且启用任一子设备唤醒设置时，新的子 arm 在创建 WAIT_WAKE 前被拒绝。取消最后一个捕获的子设备，也会取消仅因子设备而 arm 的父节点 WAIT_WAKE，并恰好执行一次 disarm；父节点有自身原因或已经成功唤醒时，保留正常 D0 disarm 顺序。本范围使用声明的提供方拓扑，仍不支持来宾总线 PDO 枚举及原始 WDM 子设备唤醒回调。

真实 WDK 样例 `driver_kmdf_child_wake.c` 使用可选 `NEVERD_KMDF_CHILD_WAKE_FIXTURE`／`NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE`。[子设备唤醒场景](../examples/driver-kmdf-child-wake-scenario.json) 可通过 CLI 执行。缺失映像明确跳过；普通／active-CFG 及首选／重定位地址的执行证据仍仅来自 Linux。

`WdfDeviceWdmAssignPowerFrameworkSettings`（KMDF 1.33 槽 425）支持自定义单组件 Fx 状态。先成功设置系统管理空闲策略，再于 `PASSIVE_LEVEL`、首次 START 完成前调用一次；组件、状态和回调设置会复制保存。客体活动、空闲条件及空闲状态回调通过真实调用帧执行，省略的回调由框架默认处理。`PoFxDeviceFlags` 必须为 0，`DirectedPoFxEnabled` 必须为 `WdfFalse`；PEP 电源控制仍不支持。

框架注册后调用 `EvtDeviceWdmPostPoFxRegisterDevice`，成功返回后才启动管理。句柄在 `EvtDeviceWdmPrePoFxUnregisterDevice` 返回前有效；STOP／REMOVE 等待回调排空，重启获得新句柄。Post 失败会使 START 失败，并先挂起已运行的自管理 I/O 再释放硬件。框架句柄允许 `PoFxCompleteIdleCondition`、`PoFxCompleteIdleState` 以及组件延迟、驻留、唤醒提示；活动引用、设备电源与注册生命周期仍由框架拥有。直接对该句柄执行 PoFx Activate／Idle／Start／Unregister 等调用会明确报告模型不支持；这是当前配置的边界，并非 WDK 禁止。真实 `driver_kmdf_pofx.c` 使用 `NEVERD_KMDF_POFX_FIXTURE`／`NEVERD_KMDF_POFX_CFG_FIXTURE`；缺失时明确跳过，执行证据仅限 Linux。 可用该样例运行[自定义 KMDF PoFx 场景](../examples/driver-kmdf-pofx-scenario.json)。

当前配置会在硬件拆除前拒绝尚有 PoFx 设备电源确认未完成的 STOP／REMOVE；必须先完成实际 D 状态事务。静止处理只取消尚未发出的决策，并在 D0Exit 前排空已有组件回调，绝不虚构设备电源完成。这是模型的串行化边界，并非通用 WDK 限制。

WDM PoFx v1 支持复制组件／空闲状态、成对引用及真实回调；回调进入、确认与返回分别保留所有权，阻塞调用延续原线程。API：`PoFxRegisterDevice`, `PoFxUnregisterDevice`, `PoFxStartDevicePowerManagement`, `PoFxActivateComponent`, `PoFxIdleComponent`, `PoFxCompleteIdleCondition`, `PoFxCompleteIdleState`, `PoFxCompleteDevicePowerNotRequired`, `PoFxReportDevicePoweredOn`, `PoFxSetComponentLatency`, `PoFxSetComponentResidency`, `PoFxSetComponentWake`, `PoFxSetDeviceIdleTimeout`。`SystemManagedIdleTimeout`／`SystemManagedIdleTimeoutWithHint` 默认使用框架拥有的单 F0 组件，STOP／REMOVE 注销，重启重新注册。仅 idle 或超时提示不会虚构 OS 决策。`power_policy_events` 的 `component_idle_state` 要求 `component` 和 `state`，`power_not_required` 不接受两字段；决策要求当前注册与捕获的 START 世代，延迟、驻留与唤醒提示限制允许的转换。PoFx v2／v3、定向电源管理与任意平台电源控制仍不支持。

真正 WDK 样例 `driver_wdm_pofx.c` 通过可选 CMake 路径 `NEVERD_WDM_POFX_FIXTURE`／`NEVERD_WDM_POFX_CFG_FIXTURE` 指定普通／CFG 映像；[PoFx 场景](../examples/driver-pofx-scenario.json)执行 F1 后返回 F0。缺少映像时明确跳过，执行证据仅来自 Linux。

可选 PDO `d3cold: {supported, enabled_by_default, wake_s0, wake_sx}` 的四个布尔值描述独立电源，冷唤醒还要求对应 `wake_capabilities`。`ExcludeD3Cold` 为 True 时保持 D3hot，False 允许显式支持的冷状态，Default 遵循提供者默认；缺少冷唤醒能力时保持 D3hot。仅成功 D3hot→D3cold 改变电源代数，D0 访问前恢复配置的寄存器值；资源世代及 MMIO 别名保留。MMIO／DMA 仍要求实际 D0；未结束的 DMA 事务、映射或通道所有权阻止冷断电，空闲适配器及公共 RAM 缓冲区可保留。不推断共享电源轨或固件能力。

场景以布尔值明确声明 PDO 的 `wake_capabilities: {s0, sx}`。READ／WRITE／IOCTL 可携带 `power_policy_events: [{device_id, after_100ns, action}]`，`action` 为 `idle`、`active` 或 `wake`：idle 启动驱动超时，active 按需请求 D0，wake 要求已 arm。报告保留 `source_request_index`、`event_index`、`device_id`、捕获的 `device_epoch`、`action`、`due_at_100ns` 和 `occurred_at_100ns`，事件不能绑定到后续重启。D0／D2／D3 消费 `requested_device_power` 响应并报告 `origin: "framework_power_policy"`；独立的 `origin: "framework_wait_wake"` IRP 等待唤醒或 disarm，不消费该 FIFO。WAIT_WAKE 完成取消属于合法预期结果。参见[完整 KMDF 空闲／唤醒场景](../examples/driver-kmdf-power-policy-scenario.json)。

场景根对象可选 `service_name`，显式值覆盖基础配置，省略则保留调用者的服务名；必须为 1–128 个 ASCII 字母、数字、`_` 或 `-`，CLI、C 与 Python 场景 JSON 共用此校验。

以下请求／对象 API 支持不高于 `DISPATCH_LEVEL` 的 IRQL：`WdfObjectDereferenceActual`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputMemory`, `WdfRequestRetrieveOutputMemory`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestGetInformation`, `WdfRequestSetInformation`, `WdfRequestGetIoQueue`, `WdfRequestGetFileObject`, `WdfRequestWdmGetIrp`, `WdfRequestGetParameters`, `WdfRequestGetStatus`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`, `WdfMemoryGetBuffer`.

`WdfObjectGetTypedContextWorker`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual` 也支持已建模的中断 IRQL；上下文访问和引用获取不会调用被动级别回调。

在高于 `PASSIVE_LEVEL` 的 IRQL 完成请求或释放对象的最后一个引用时，真实清理、销毁及后续框架投递会调度到 `PASSIVE_LEVEL`。其他 API 保留既有 IRQL 限制。

控制设备要求复制可打印 ASCII 名称，并且 SDDL 必须精确为 `D:P(A;;GA;;;WD)`。这授予所有调用方访问权限，无需虚构调用方令牌；不支持其他安全描述符、未命名设备和自动名称。设备初始化拥有一个 WDM 设备。请求可通过现有会话命名空间中的符号链接别名 `\DosDevices\Name` 或 `\??\Name` 选择设备，报告仍保留规范设备名。创建成功会消耗初始化对象并清空其指针；失败则回滚部分设备所有权。`WdfControlFinishInitializing` 决定何时可以递送 I/O。仅在已建模的文件、工作项和请求允许时，删除操作才移除设备及其链接；不支持删除过程中取消或排空请求。

96 字节的 `WDF_IO_QUEUE_CONFIG` 支持默认及非默认的手动、顺序、有限或无限并行队列，执行级别及同步范围由对象属性配置。控制设备队列不参与电源管理。专用 READ／WRITE／IOCTL 回调优先于默认回调。已接受的队列请求即使同步完成也返回 `STATUS_PENDING`；void 回调的返回寄存器不会使请求完成。延迟完成使用现有调度器。没有处理函数时，请求以 `STATUS_INVALID_DEVICE_REQUEST` 完成；未启用零长度递送时，零长度 READ／WRITE 直接完成。默认文件包以成功状态和 Information=0 完成 CREATE／CLEANUP／CLOSE。非默认手动队列通过 `WdfRequestForwardToIoQueue` 接收请求，`WdfIoQueueRetrieveNextRequest` 按 FIFO 顺序取回。取回前取消的请求由框架从队列移除并以 `STATUS_CANCELLED` 完成。非默认自动队列通过自身回调递送转发请求；手动默认队列保留传入请求，直到驱动取回。`WdfIoQueueRetrieveNextRequest` 可从手动和顺序队列取回等待中的请求；并行队列返回 `STATUS_INVALID_DEVICE_STATE`。若自动队列没有匹配回调，则在递送槽可用时以 `STATUS_INVALID_DEVICE_REQUEST` 完成该请求。更广泛的 PnP 和电源行为仍不属于此配置。 `WdfRequestRequeue` 将已取回的请求重新放到同一手动队列队首。 `NumberOfPresentedRequests` 限制并行队列已递送请求的数量；超出上限的请求等待已递送请求完成或取消。 默认顺序队列在已有请求递送给驱动时仍接受后续请求；这些请求按 FIFO 等待空槽，并可在递送前取消。 `WdfIoQueueStop` 暂停递送但继续接收请求；`WdfIoQueueStart` 恢复等待请求的递送，`WdfIoQueueGetState` 报告排队和已递送请求数。停止期间取请求返回 `STATUS_WDF_PAUSED`；停止完成回调会在所有已递送请求完成或离开队列后带着指定上下文执行；仍在排队的请求不会阻碍回调。前一个回调待执行时再次注册会被拒绝。

`WdfIoQueueReadyNotify` 为手动队列注册一个 `EvtIoQueueState` 回调。当排队请求数从零变为非零时，回调在 `PASSIVE_LEVEL` 接收 `(WDFQUEUE, WDFCONTEXT)`；即使驱动仍持有此前取出的请求也一样。已非空队列在注册时可以立即通知；停止的队列等到 `WdfIoQueueStart` 才通知。重复注册或在停止前注销返回 `STATUS_INVALID_DEVICE_REQUEST`；调用 `WdfIoQueueStop` 后传入 NULL 即可注销。

`WdfIoQueueFindRequest` 在手动队列中查找请求，但不转移处理权；查找成功会增加一次请求引用，驱动须用 `WdfObjectDereference` 释放。`WdfIoQueueRetrieveFoundRequest` 仅对仍在队列中的请求转移处理权；已被取消移除的请求返回 `STATUS_NOT_FOUND`。可选参数沿用 `WdfRequestGetParameters` 的结构布局。可使用有效的框架文件对象筛选 `WdfIoQueueFindRequest`。`WdfIoQueueRetrieveRequestByFileObject` 从手动或顺序队列取出下一个匹配请求；没有匹配项时不改写输出。

配置的 `EvtIoCanceledOnQueue` 仅接收先前交给驱动后又转发或重新入队的请求，或由调用方上下文回调显式入队的请求；参数为 `(WDFQUEUE, WDFREQUEST)`。从未交给驱动的排队请求由框架直接以 `STATUS_CANCELLED` 完成，不调用该回调。回调将请求所有权交还驱动；驱动必须在回调内或之后完成请求，不能再次入队。队列清除与状态完成会等待回调返回及该驱动拥有的请求完成。

`WdfIoQueueDrain` 拒绝新请求（`STATUS_INVALID_DEVICE_STATE`），继续递送已排队请求；排队数和驱动持有数归零后调用完成回调。转发到已耗尽队列返回 `STATUS_WDF_BUSY`；`WdfIoQueueStart` 恢复接收。

`WdfIoQueuePurge` 还会以 `STATUS_CANCELLED` 取消框架尚未递送的请求，并在原 IRP 上取消已标记为可取消的驱动持有请求。清理回调先于 IRP 释放执行；状态完成回调等待排队、驱动持有及取消回调结束。未标记可取消的请求仍由驱动完成。

`WdfIoQueueStopSynchronously`、`WdfIoQueueDrainSynchronously` 和 `WdfIoQueuePurgeSynchronously` 会挂起来宾的 `PASSIVE_LEVEL` 调用帧，直到相关请求退出。同步停止继续接收但暂停递送，并等待已递送请求；同步耗尽拒绝新请求、继续递送排队请求，并等待两类请求完成；同步清空取消排队及已标记可取消的请求，等待取消回调返回。没有完成来源的等待会报告停滞模型错误。

`WdfIoQueueStopAndPurge` 和 `WdfIoQueueStopAndPurgeSynchronously` 取消调用前已排队及已标记可取消的驱动持有请求，随后继续接收新请求，但直到 `WdfIoQueueStart` 才重新递送。异步状态回调和同步等待在原有请求及其取消回调结束后完成；调用后进入队列的新请求保持排队，不延迟完成通知。普通和同步停止也会从先前的耗尽或清空状态恢复接收。

请求参数使用 40 字节的 `WDF_REQUEST_PARAMETERS` 布局。输入／输出访问函数返回逻辑长度，保留缓冲区别名及现有直接 I/O 的 MDL 映射；直接 IOCTL 的输入仍使用缓冲区。方向错误或缓冲区不足返回文档规定的状态。完成操作先在缓冲区仍有效时执行请求清理，再完成 IRP 并释放请求所属锁页；引用允许时才销毁子对象和请求。一旦开始完成操作，就拒绝新的缓冲区与参数访问函数调用；已取得的缓冲区指针在清理期间仍可使用。外部对象引用保留上下文，但不保留对已完成 IRP 的访问权。

取消支持限于上述控制设备队列中的请求。若取消已经发生，`WdfRequestMarkCancelableEx` 返回 `STATUS_CANCELLED`，不会调用取消回调。`WdfRequestUnmarkCancelable` 成功后会移除回调；之后发生的取消只记录已取消状态，不再递送该回调。`WdfRequestIsCanceled` 可在未标记为可取消的存活请求上读取此状态。成功标记为可取消后，完成请求需要成功解除标记，或取消回调已经开始递送；仅仅排队还不允许完成。回调开始后可与工作项协调完成，包括回调正在等待的情况。独立的内部引用保留请求直到取消回调返回；完成操作仍先使 IRP 失效，最终的请求销毁续接本身也可等待。调度优先级依次为 DPC、按 FIFO 排列的取消回调、普通工作项；排队的取消回调也先于就绪的被动级等待帧恢复。

旧版 void `WdfRequestMarkCancelable` 支持至 `DISPATCH_LEVEL`。请求已取消时，只有队列执行级别兼容当前 IRQL，且当前线程未持有同一回调锁，才同步执行 `EvtRequestCancel`；否则排入工作线程或 DPC 后返回。注册后的取消同样走调度路径。只有实际来宾回调取得回调锁并入场后才标记 delivered；排队不等于已交付，不能据此提前完成请求。未使用自动同步（`WdfSynchronizationScopeNone`）时，Microsoft 建议使用 Ex。 [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable).

`WdfRequestGetInformation` 与 `WdfRequestSetInformation` 共享原 IRP 中的 64 位 `IoStatus.Information`，与来宾直接写入保持一致。Set 只赋值，传输长度在完成时才验证。`WdfRequestCompleteWithInformation` 在清理前写入同一字段；清理回调通过事先保存的 IRP 修改此值后，最终 Information 使用修改后的值，即使此阶段 GetInformation 已返回零。`WdfRequestGetIoQueue` 在驱动持有请求时返回递送队列；手动取回后返回目标手动队列。请求停留在手动队列期间归框架所有，驱动不可访问。默认文件配置下，`WdfRequestGetFileObject` 返回 NULL，不会把 WDM FILE_OBJECT 伪装成 WDF 文件对象。`WdfRequestWdmGetIrp` 返回同一个 IRP；来宾不能通过 `IoCompleteRequest`／`IofCompleteRequest` 绕过 WDF 完成流程。完成过程中或完成后，只要请求句柄仍有效，GetInformation／GetIoQueue 返回零；MDL 获取先将有效输出槽清为 NULL，再返回 `STATUS_INTERNAL_ERROR`。此时 SetInformation／GetFileObject／WdmGetIrp 仍被拒绝，已有的缓冲区与参数访问限制不变。

`WdfRequestRetrieveInputWdmMdl` 与 `WdfRequestRetrieveOutputWdmMdl` 按需为缓冲 WRITE 输入、READ 输出及 IOCTL 输入／输出描述现有 SystemBuffer。每次获取都先验证方向有效且长度非零，再使用该请求唯一的缓存描述符；首次成功获取决定 ByteCount，即使另一方向的逻辑长度不同也保留此值。对此描述符，`MmGetSystemAddressForMdlSafe` 返回原 VA；额外系统映射、解除系统映射及驱动释放均被拒绝。直接 READ 输出、WRITE 输入及 IOCTL 输出返回现有 `IRP.MdlAddress`，获取本身不建立映射；直接 IOCTL 输入使用 SystemBuffer 缓存。描述符、IRP 和缓冲区在完成时一同失效。取消内部引用或外部引用只保留 WDF 上下文，不保留已完成的 I/O 存储。WDF 对 `METHOD_NEITHER` 的 MDL 获取函数仍不支持；请求所属的 WDFMEMORY 使用独立的锁页映射。已构建描述符的模型 PFN 数组可只读访问；未构建描述符的 PFN 访问被拒绝。

对缓冲、直接和 neither KMDF 请求，`WdfDeviceInitSetIoInCallerContextCallback` 在请求方进程的 `PASSIVE_LEVEL` 执行预队列回调。回调必须完成请求，或恰好调用一次 `WdfDeviceEnqueueRequest`。入队时捕获 `WdfDeviceConfigureRequestDispatching` 为该请求类型选择的队列，未配置映射时回退到默认队列；后续映射变化不会迁移已接收的请求。对于 `METHOD_NEITHER` IOCTL 和 neither READ／WRITE，`WdfRequestRetrieveUnsafeUserInputBuffer` 与 `WdfRequestRetrieveUnsafeUserOutputBuffer` 仅在该回调中返回原始用户地址。`WdfRequestProbeAndLockUserBufferForRead` 与 `WdfRequestProbeAndLockUserBufferForWrite` 检查页面权限并锁定请求所属的页面；`WdfMemoryGetBuffer` 返回系统别名，在离开请求方上下文后的队列回调中仍可使用。请求完成时释放锁页和别名。也支持通过内嵌指针访问显式声明的 `user_buffers`；任意用户映射仍不支持。

neither 请求可声明 `user_buffers`（`id`、`size`、可选 `input`／`access`）和 `user_pointers`。每条指针的 `source`／`target` 指定 `buffer`（`input`、`output` 或 `memory`）与 `offset`；只有 `memory` 使用请求内唯一的 `id`。八字节指针槽不能重叠或越界，目标可位于缓冲区内或末尾。全场景最多 64 个附加缓冲区、256 个指针，每区最多 64 KiB，计入 512 KiB 总预算。重复目标共享真实字节；页面权限、锁页、请求撤销和进程退出沿用现有机制。`configuration.user_memory` 保留声明，结果 `user_buffers[].backing_hex` 是停止时的诊断快照，包含撤销或故障后的数据，不代替 `output_hex`，也不恢复访问权限。 `id`: 1–64 ASCII, `[A-Za-z0-9][A-Za-z0-9_.-]*`.

`WdfRequestRetrieveInputMemory` 与 `WdfRequestRetrieveOutputMemory` 为现有缓冲或直接 I/O 缓冲区提供请求所属的 WDFMEMORY 视图。重复获取同一方向保持句柄不变；`WdfMemoryGetBuffer` 返回原缓冲区和逻辑长度。零长度或方向不符返回 WDF 状态；neither I/O 仍需在调用方上下文中探测并锁页。借用的视图不额外锁页，在请求完成时失效。

当前支持 KMDF PnP 的直连 FDO/PDO 子集：`EvtDriverDeviceAdd` 接收框架持有的初始化器，`WdfDeviceCreate` 创建 FDO，`WdfFdoInitWdmGetPhysicalDevice` 与 `WdfDeviceWdmGetPhysicalDevice` 保留 PDO 身份；`WdfDeviceWdmGetAttachedDevice` 返回下层 WDM 对象，`WdfWdmDeviceGetWdfDeviceHandle` 将本驱动创建的 WDM 对象映射回 WDF 句柄，`WdfDeviceGetDriver` 返回所属 WDFDRIVER。AddDevice 失败及完成 Remove 后，框架会运行清理／销毁回调并删除设备。`WdfDeviceInitSetPnpPowerEventCallbacks` 接受精确的 KMDF 1.33 结构，支持 `EvtDevicePrepareHardware`、`EvtDeviceD0Entry`、`EvtDeviceD0Exit`、`EvtDeviceReleaseHardware`、`EvtDeviceQueryStop`、`EvtDeviceQueryRemove` 与 `EvtDeviceSurpriseRemoval`，其他非空回调会明确报错。QueryStop／QueryRemove 在下层总线派发前调用真实回调；失败状态直接否决并完成原 IRP，不访问总线，也不改变设备及电源生命周期。SurpriseRemoval 的 void 回调同样先于下层总线派发，再按现有流程退出 D0。 总线成功完成 START 后，PrepareHardware 先接收两个不同的资源列表（无资源设备的列表为空），再在原 IRP 完成前执行 D0Entry；空列表的 `WdfCmResourceListGetCount` 返回零，`WdfCmResourceListGetDescriptor` 返回 NULL。PrepareHardware 或 D0Entry 失败时，失败状态作为 START 结果，仍执行 ReleaseHardware，且不调用 D0Exit。STOP、意外移除或从 D0 正常移除前依次执行 D0Exit 与 ReleaseHardware；再次 START 会重新准备硬件并进入 D0。START 失败后必须在场景中显式移除设备。PnP 队列可用 `WdfUseDefault` 或 `WdfTrue` 启用电源管理，`WdfFalse` 则禁用自动管理。队列在设备进入 D0 前及离开 D0 时设置 `WdfIoQueuePnpHeld`，进入 D0 后恢复投递。配置 `register_bank` 的设备提供只读 `CM_PARTIAL_RESOURCE_DESCRIPTOR` 条目，指针有效期持续到 ReleaseHardware；驱动可用 `MmMapIoSpace` 映射转换后的内存，但须在 STOP 或 Remove 完成前解除映射。离开 D0 前，电源管理队列对每个驱动持有的请求调用 `EvtIoStop`；驱动可完成请求、调用 `WdfRequestStopAcknowledge`，或等待已安排的工作完成请求。未注册 `EvtIoStop` 时，框架同样等待所有已投递请求完成。TRUE 将请求重新入队，待重新启动后再次投递；FALSE 保留驱动所有权，在重新进入 D0 后调用 `EvtIoResume`。队列中的请求在 STOP 期间保持等待；只有没有任何完成来源时，等待才以停滞的 `model_error` 结束。其他资源类型和通用电源策略尚未实现。真实 WDK 样例 `driver_kmdf_pnp.c` 可通过 `NEVERD_KMDF_PNP_FIXTURE`／`NEVERD_KMDF_PNP_CFG_FIXTURE` 验证普通及 active-CFG 映像。

已建模的 KMDF API: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfObjectAcquireLock`, `WdfObjectReleaseLock`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetDeviceType`, `WdfDeviceInitSetExclusive`, `WdfDeviceInitSetFileObjectConfig`, `WdfDeviceInitSetIoType`, `WdfDeviceInitSetIoInCallerContextCallback`, `WdfDeviceInitSetPnpPowerEventCallbacks`, `WdfDeviceInitSetPowerPolicyOwnership`, `WdfCmResourceListGetCount`, `WdfCmResourceListGetDescriptor`, `WdfDeviceCreate`, `WdfDeviceEnqueueRequest`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfDeviceWdmGetAttachedDevice`, `WdfDeviceWdmGetPhysicalDevice`, `WdfWdmDeviceGetWdfDeviceHandle`, `WdfDeviceGetDriver`, `WdfDeviceGetIoTarget`, `WdfFdoInitWdmGetPhysicalDevice`, `WdfFdoInitSetFilter`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfDeviceConfigureRequestDispatching`, `WdfIoQueueGetDevice`, `WdfIoQueueGetState`, `WdfIoQueueStop`, `WdfIoQueueStopSynchronously`, `WdfIoQueueStopAndPurge`, `WdfIoQueueStopAndPurgeSynchronously`, `WdfIoQueueReadyNotify`, `WdfIoQueueStart`, `WdfIoQueueDrain`, `WdfIoQueueDrainSynchronously`, `WdfIoQueuePurge`, `WdfIoQueuePurgeSynchronously`, `WdfIoQueueRetrieveNextRequest`, `WdfIoQueueRetrieveRequestByFileObject`, `WdfIoQueueFindRequest`, `WdfIoQueueRetrieveFoundRequest`, `WdfRequestForwardToIoQueue`, `WdfRequestRequeue`, `WdfRequestStopAcknowledge`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestFormatRequestUsingCurrentType`, `WdfRequestSend`, `WdfRequestGetStatus`, `WdfRequestSetCompletionRoutine`, `WdfRequestGetCompletionParams`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputMemory`, `WdfRequestRetrieveOutputMemory`, `WdfRequestRetrieveUnsafeUserInputBuffer`, `WdfRequestRetrieveUnsafeUserOutputBuffer`, `WdfRequestProbeAndLockUserBufferForRead`, `WdfRequestProbeAndLockUserBufferForWrite`, `WdfMemoryGetBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfFileObjectGetFileName`, `WdfFileObjectGetFlags`, `WdfFileObjectGetDevice`, `WdfFileObjectWdmGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`, `WdfInterruptCreate`, `WdfInterruptQueueDpcForIsr`, `WdfInterruptQueueWorkItemForIsr`, `WdfInterruptSynchronize`, `WdfInterruptAcquireLock`, `WdfInterruptReleaseLock`, `WdfInterruptEnable`, `WdfInterruptDisable`, `WdfInterruptWdmGetInterrupt`, `WdfInterruptGetInfo`, `WdfInterruptReportActive`, `WdfInterruptReportInactive`, `WdfInterruptGetDevice`, `WdfSpinLockCreate`, `WdfSpinLockAcquire`, `WdfSpinLockRelease`, `WdfWaitLockCreate`, `WdfWaitLockAcquire`, `WdfWaitLockRelease`, `WdfDeviceInitSetPowerPolicyEventCallbacks`, `WdfDeviceAssignS0IdleSettings`, `WdfDeviceWdmAssignPowerFrameworkSettings`, `WdfDeviceAssignSxWakeSettings`, `WdfDeviceStopIdleNoTrack`, `WdfDeviceResumeIdleNoTrack`, `WdfDeviceStopIdleActual`, `WdfDeviceResumeIdleActual`.

对于 PnP FDO，`WdfDeviceInitSetDeviceType` 将指定的 32 位类型写入 WDM `DEVICE_OBJECT`；未调用时默认为 `FILE_DEVICE_UNKNOWN`。当前不模拟依赖设备类型的 I/O 优先级提升。

`WdfDeviceInitSetExclusive` 将 `DO_EXCLUSIVE` 设置在初始化器创建的 WDM 设备上。具名控制设备在首个文件关闭前拒绝第二次独立打开。PnP FDO 上的该标志本身不会使具名 PDO 或整个设备栈独占；此配置尚不模拟 INF 指定的 PDO 独占属性。

`WdfDeviceInitSetFileObjectConfig` 在创建设备前注册 `EvtDeviceFileCreate`、`EvtFileCleanup`、`EvtFileClose`，并复制可选的文件对象上下文属性。当前支持 `WdfFileObjectNotRequired`、`WdfFileObjectWdfCanUseFsContext`、`WdfFileObjectWdfCanUseFsContext2` 和 `WdfFileObjectWdfCannotUseFsContexts`。选定的 WDM 上下文槽在 CREATE 失败或 CLOSE 前保存 WDF 句柄，创建前必须为空。设置 `WdfFileObjectCanBeOptional` 后，若 I/O 请求没有匹配的 WDM 文件对象，`WdfRequestGetFileObject` 返回 NULL。CREATE、CLEANUP 和 CLOSE 仍要求 WDM 文件对象；`WdfFdoInitSetFilter` 启用筛选驱动默认转发；`WdfTrue` 显式启用，`WdfFalse` 禁用。文件请求转发要求直连 FDO/PDO 路由和显式的 `bus_completion`；自动转发 CREATE/CLEANUP/CLOSE，以及同步、带完成回调的异步或 `SEND_AND_FORGET` CREATE 发送均允许正数 `delay_100ns`；CLEANUP 和 CLOSE 回调先于下层派发。使用 `WdfFileObjectNotRequired` 的 CREATE 回调可通过 `WdfDeviceGetIoTarget` 获取设备持有的本地目标，并以唯一受支持的 `WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET` 选项调用 `WdfRequestSend` 转发原始请求。保留的 PDO 消费显式总线响应并负责最终完成，下层响应延迟时也一样。持有框架文件对象的 CREATE 回调可使用 `WDF_REQUEST_SEND_OPTION_SYNCHRONOUS`，通过 `WdfRequestGetStatus` 读取下层结果，再以该状态完成原请求。默认异步发送需先用 `WdfRequestFormatRequestUsingCurrentType` 格式化请求，再注册 `WdfRequestSetCompletionRoutine`。完成回调收到 `WDF_REQUEST_COMPLETION_PARAMS`，可通过 `WdfRequestGetCompletionParams` 或 `WdfRequestGetStatus` 读取下层结果，并以可独立于下层结果选择的状态完成原请求。对于带完成回调的发送，延迟的 PDO 响应让请求保持未完成，直到虚拟时间达到截止点，再以下层最终状态排队执行回调。异步 CREATE 发送可设置 `WDF_REQUEST_SEND_OPTION_TIMEOUT`：负数表示相对间隔，正数表示合成虚拟时钟上的绝对 100 ns 截止点，零禁用超时。超时早于下层响应时向回调交付 `STATUS_IO_TIMEOUT`；立即完成以及与超时同刻的下层响应优先。其他未列出的发送选项仍不支持。WDF 文件句柄与 WDM `FILE_OBJECT` 保持不同身份，存活期间可由 `WdfRequestGetFileObject`、`WdfFileObjectGetDevice` 和 `WdfFileObjectWdmGetFileObject` 查询。CREATE 失败只删除 WDF 文件对象，不调用文件清理或关闭回调；成功打开后的 CLEANUP、CLOSE 回调先于上下文清理和销毁执行。

延迟的同步 `WdfRequestSend` 保留并挂起真实来宾调用帧，完成后恢复并返回表示已发送的 BOOLEAN；最终 NTSTATUS 由 `WdfRequestGetStatus` 提供。同步 CREATE 同样支持负数相对、正数绝对虚拟 100 ns 截止点的 `WDF_REQUEST_SEND_OPTION_TIMEOUT`，零禁用超时。较早的超时返回 `STATUS_IO_TIMEOUT`，立即完成或同刻的下层响应优先。自动转发仍先执行文件回调，再派发至下层；WDM IRP 和文件在完成流程所需的清理、销毁回调期间保持有效。外部 WDF 引用保留上下文，但不延长已完成请求对应 WDM 文件的生命周期。

转发到已配置 PDO 的 WDM CREATE/CLEANUP/CLOSE 也接受显式延迟 `bus_completion`。真实来宾完成例程在下层期限后执行，保留待处理状态传播和最终状态归属规则。

可选的真实 WDK 验证以真正的 KMDF 入口库分别编译 `driver_kmdf_lifecycle.c` 和 `driver_kmdf_control.c`。`NEVERD_KMDF_FIXTURE`／`NEVERD_KMDF_CFG_FIXTURE` 选择生命周期映像；`NEVERD_KMDF_CONTROL_FIXTURE`／`NEVERD_KMDF_CONTROL_CFG_FIXTURE` 选择普通／启用 CFG 的控制设备映像。缺少外部产物时会明确跳过。原生及 C API／CLI 覆盖见[测试指南](testing.md)。当前执行证据仅来自 Linux 主机。

初始 API 模型刻意采用有限契约：

| API | 建模行为与限制 |
|-----|----------------|
| `RtlInitUnicodeString` | 根据有界、以 NUL 结尾的源字符串构造来宾 `UNICODE_STRING` |
| `RtlCopyUnicodeString`、`RtlCompareUnicodeString`、`RtlEqualUnicodeString` | 带长度的 UTF-16 复制及区分大小写比较；不区分大小写的比较需要 Windows 大小写表，因此会停止 |
| `ExRaiseStatus`, `ExRaiseAccessViolation`, `ExRaiseDatatypeMisalignment` | 抛出来宾异常，无正常 API 返回；真实 C 过滤器／处理器及展开 finally，用户 CPU 访存异常的受限恢复见下文 |
| `ExAllocatePool2` | 分页／非分页 NX 分配，默认清零；支持未初始化与缓存行对齐标志；无效的必需标志返回 NULL，配额／可执行池以及分配失败引发异常的路径会停止 |
| `MmGetSystemRoutineAddress` | 通过共享导出清单解析带长度的来宾名称 |
| `NtQuerySystemInformation`, `ZwQuerySystemInformation` | 在 PASSIVE_LEVEL 支持类 `11`（`SystemModuleInformation`）：零长度大小查询，以及模拟提供者和输入驱动的完整 Win64 模块记录。部分缓冲区和未知信息类会明确停止。 |
| `MmMapIoSpace`, `MmMapIoSpaceEx`, `MmUnmapIoSpace` | 声明的转换后子区间；非缓存 RO／RW、共享别名、精确取消映射；不提供任意物理内存 |
| `IoConnectInterrupt`, `IoDisconnectInterrupt`, `IoConnectInterruptEx`, `IoDisconnectInterruptEx` | 精确配置的独占／共享 latched 或 level_sensitive 线路；PASSIVE_LEVEL 下传统 ABI 与 Ex 1／2／4，严格连接代次和锁所有权 |
| `KeSynchronizeExecution`, `KeAcquireInterruptSpinLock`, `KeReleaseInterruptSpinLock` | 真实 BOOLEAN 同步回调，在不低于配置 DIRQL 的同步 IRQL 下持有同一非递归锁；恢复原调用者 IRQL 和所有权 |
| `IoGetDmaAdapter` | PASSIVE_LEVEL 下显式 Internal 总线主设备的版本 0／1 描述和绑定的 V1 操作表；新版探测返回 NULL |
| `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `PutDmaAdapter` | 操作共享一致性 RAM 的适配器表方法，精确分配身份及独立适配器生命周期 |
| `GetScatterGatherList`, `PutScatterGatherList` | DISPATCH_LEVEL 下的适配器表方法；真实内嵌或资源排队回调、固定 MDL 视图和显式映射释放 |
| `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers`, `FreeMapRegisters` | 经过地址转换的总线主设备通道回调、共享寄存器额度、连续 MDL 片段、整体刷新及确切保留寄存器释放 |
| `KeFlushIoBuffers` | 针对有效已锁定／非分页 MDL 的一致性 CPU 缓存刷新，不释放 DMA 映射 |
| `MmMapLockedPagesSpecifyCache`、`MmGetSystemAddressForMdlSafe`、`MmUnmapLockedPages` | 用户／系统映射继承物理页缓存属性及各自权限；非分页池 MDL 通过安全辅助函数复用原系统映射 |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `MmProbeAndLockPages`, `MmUnlockPages`, `IoFreeMdl` | 独立或 IRP 关联的非分页池／用户描述符；可修改链指针，锁页与系统别名独立；不支持配额 |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | 显式配置的会话注册表、逐句柄权限与生命周期、查询缓冲区大小及修改；不访问宿主注册表 |
| `ExAllocatePool` | 旧版双参数接口，支持池类型 `0`、`1`、`512` 的数据分配；共用对齐、未初始化字节模型及大小／IRQL 检查，耗尽时返回 NULL。通过 `ExFreePool` 或零标签的 `ExFreePoolWithTag` 释放；存活分配仍属于内核依赖。 |
| `ExAllocatePoolWithTag`、`ExFreePoolWithTag`、`ExFreePool` | 对池类型 `0`、`1`、`512` 提供数据分配；大小／标签必须为正，带标签释放必须匹配，地址不复用 |
| `IoCreateDevice`、`IoDeleteDevice` | 设备类型为 `0x22`，characteristics 为 `0` 或 `0x100`，扩展大小有界，名称为 ASCII `\Device\Name` |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | 同驱动附加；返回原栈顶，拆链接收保存的下层设备；遵守上述拓扑和生命周期限制 |
| `IofCallDriver`, `IoCallDriver` | 在保留路径中向确切目标派发；验证来宾栈游标，保留下层 NTSTATUS |
| `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx` | 扩展内确切所有者及不透明32／120字节；NULL／重复 Tag |
| `IoReleaseRemoveLockEx`, `IoReleaseRemoveLockAndWaitEx` | 匹配 Tag 释放及提供者接收后的可恢复 REMOVE 等待 |
| `PoCallDriver`, `PoStartNextPowerIrp` | 转发同一受管理的电源 IRP；Vista+ start-next 验证不增加串行化握手 |
| `PoSetPowerState`, `PoRequestPowerIrp` | 独立设备通知状态及显式 PDO FIFO 驱动的真实子请求；范围见上 |
| `IoCreateSymbolicLink`、`IoDeleteSymbolicLink` | 一个会话命名空间内的 ASCII `\DosDevices\Name` 或 `\??\Name`，目标为 `\Device\Name` |
| `DbgPrint`、`DbgPrintEx` | 经检查的 Win64 可变参数格式化，最多输出 512 字节；启用所有调试器过滤器 |
| `IoGetCurrentIrpStackLocation` | 返回当前建模 IRP 的栈位置；正常编译的 WDM 宏读取相同来宾字段 |
| `KeGetCurrentIrql` | 读取当前 IRQL/CR8，包括显式升降级；派发和工作项从 `PASSIVE_LEVEL` 开始，DPC 从 `DISPATCH_LEVEL` 开始 |
| `KfRaiseIrql`, `KeLowerIrql` | 真实的 x64 WDK IRQL 升降导入，包含内联辅助函数；每个执行必须在返回前按 LIFO 顺序恢复保存值。IRQL <= APC_LEVEL 的合法等待保留升降配对，并按等待时的 IRQL 恢复；DISPATCH_LEVEL 持锁不能挂起。CR8 反映每次变化；配置的 CPU0 事件抢占使用 `scheduling`；任意嵌套中断仍不受支持。 |
| `KeInitializeSpinLock`, `KeAcquireSpinLockRaiseToDpc`, `KeReleaseSpinLock`, `KeAcquireSpinLockAtDpcLevel`, `KeReleaseSpinLockFromDpcLevel`, `KeTryToAcquireSpinLockAtDpcLevel` | CPU0 上驻留且对齐的执行自旋锁；检查所有者、获取与释放配对和 IRQL 恢复。竞争的阻塞获取会显式停止。 |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | 设备拥有的不透明工作项；仅支持 `DelayedWorkQueue`，在 `PASSIVE_LEVEL` 将设备和上下文传给回调；禁止释放仍在队列中的项 |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | 不透明 DPC 存储、四个来宾回调参数、`DISPATCH_LEVEL`、重复入队／移除及优先级；仅目标 CPU0 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | 通知／同步定时器；相对／绝对 100 ns 期限、毫秒周期、重新设置／取消及虚拟时间中的信号查询 |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | 通知／同步事件保留不同信号消耗行为；`KeSetEvent` 仅接受 Increment=0、Wait=FALSE |
| `KeInitializeSemaphore`, `KeReleaseSemaphore`, `KeReadStateSemaphore` | 驻留的计数信号量，上限必须为正；每次成功等待消耗一个计数。释放仅接受 Increment=0、Wait=FALSE；超过上限时抛出 `STATUS_SEMAPHORE_LIMIT_EXCEEDED`。 |
| `KeInitializeMutex`, `KeReleaseMutex`, `KeReadStateMutex` | 驻留的 KMUTEX 按逻辑线程持有，支持跨嵌套回调和 SEH 的递归获取；KeReleaseMutex 返回先前的有符号信号状态，要求持有者和匹配的 DISPATCH_LEVEL 获取上下文，且仅接受 Wait=FALSE。持有期间禁止最外层返回、重新初始化或释放存储。非持有者释放触发 `STATUS_MUTANT_NOT_OWNED`。 |
| `PsCreateSystemThread`, `PsTerminateSystemThread`, `ObReferenceObjectByHandle`, `ObfDereferenceObject`, `ZwClose` | 系统进程中的有界线程在 PASSIVE_LEVEL 运行。句柄与不透明线程对象的引用各自持有生命周期；PsTerminateSystemThread 不返回客体代码，并使线程对象可等待且进入信号状态。APC 交付、进程优先级类别和带类型的对象引用尚未建模。 |
| `KeSetPriorityThread`, `KeQueryPriorityThread` | 在 `PASSIVE_LEVEL` 访问运行时优先级；设置接受 1..31 并返回原值，确定性初始值为 8，要求已知线程对象。`scheduling` 启用优先级调度；动态提升与进程优先级类别尚未支持。 [driver-scheduling.md](driver-scheduling.md) |
| `KeQueryActiveProcessors`, `KeSetSystemAffinityThread`, `KeRevertToUserAffinityThread` | 组 0 只有一个处理器：`KeQueryActiveProcessors` 在任意有效 IRQL 返回掩码 `1`。旧式亲和性设置／恢复对在 IRQL <= DISPATCH_LEVEL 接受掩码 `1`，跨挂起和嵌套调用跟踪逻辑线程所有权，并要求外层返回前恢复。其他掩码和无对应设置的恢复会明确失败。这些依赖环境的调用保留 UNPACK 恢复依赖。 |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeEnterGuardedRegion`, `KeLeaveGuardedRegion`, `KeAreApcsDisabled`, `KeAreAllApcsDisabled` | 按线程跟踪可嵌套的 APC 禁用状态。临界区及持有的 KMUTEX 禁用普通内核 APC；保护区和 IRQL >= APC_LEVEL 禁用全部 APC。系统线程启动时处于一层临界区内。未匹配的离开和带未平衡状态返回都会失败；尚未实现 APC 投递。 |
| `KeWaitForSingleObject` | 单个已初始化的事件、定时器、信号量或互斥体；非警报 `KernelMode`、原因 `Executive`；零超时轮询、有限相对／绝对或无限等待；非零／无限等待要求 IRQL <= APC_LEVEL |
| `KeWaitForMultipleObjects` | 非警报式 `KernelMode`／`Executive` 下对 1..64 个对象执行 `WaitAll`／`WaitAny`；超过三个须提供非分页 `KWAIT_BLOCK`。原子获取、数组索引结果、线程引用和超时释放见[驱动调度](driver-scheduling.md)。 |
| `KeDelayExecutionThread` | IRQL <= APC_LEVEL 的非警报 `KernelMode` 相对／绝对延迟；虚拟时间推进后恢复保存的来宾执行帧 |
| `IoMarkIrpPending` | 标记当前存活的 IRP；也支持 WDM 宏对栈控制字段的等效写入；派发必须返回 `STATUS_PENDING` |
| `IofCompleteRequest`、`IoCompleteRequest` | 使用 `IO_NO_INCREMENT` 执行完成展开，支持暂停／继续；仅在最终展开边界释放 IRP、MDL 和缓冲区 |
| `memcpy`、`memmove`、`memset`、`memcmp`、`RtlCopyMemory`、`RtlMoveMemory`、`RtlFillMemory`、`RtlZeroMemory`、`RtlCompareMemory` | 有界的来宾缓冲区操作，每次调用最多 1 MiB；要求不重叠的复制 API 会拒绝重叠 |

`MmProbeAndLockPages` 支持在 IRQL <= APC_LEVEL 下，以 `KernelMode` 锁定输入驱动镜像中由加载器拥有的单个连续区间。镜像页必须可读，并受物理页配额限制。模型已持有私有且驻留的镜像后备存储：即使原镜像视图只读，`IoWriteAccess` 和 `IoModifyAccess` 也允许可写 MDL 别名；`IoReadAccess` 仍只允许只读别名。原镜像保护属性保持不变。镜像空洞和无关映射不属于该所有权；低于用户地址边界的镜像也通过所有权识别。锁定后必须解锁并释放描述符。加载器拥有的镜像页上，临时 MDL 的全部别名解除、全部锁释放且全部描述符释放后，不再额外阻止恢复。此契约允许未指定地址的内核态缓存别名。存活 MDL、其他所有权或映射类型，以及来宾对物理 PFN 身份的读取仍保留恢复依赖；建模的复制、移动和比较服务读取也计入。

`KernelDispatcher` 按带符号的 32 位 `LONG` 解码信号量的 `Count`、`Limit` 和 `Adjustment`，按 32 位 `ULONG` 解码互斥体的 `Level`，按 8 位 `BOOLEAN` 解码 `Wait`，并依照 [Windows x64 ABI](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170) 忽略寄存器中未定义的高位。有效位中的非法值及信号量溢出仍在修改对象状态之前被拒绝。

API 的 IRQL 上限来自 `KernelAPIIRQL.def`，参数相关限制由所属模型检查。DPC 不能调用注册表 API，也不能分配、释放或访问分页池；Unicode `DbgPrint` 转换要求 `PASSIVE_LEVEL`，支持的 ANSI 输出和非分页操作仍可在 `DISPATCH_LEVEL` 使用。回调栈有明确边界，越界栈指针不能进入另一阻塞工作项的栈。设备扩展中的已启动定时器会阻止设备提前回收。

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

根对象仅接受 `load_address`、`requests`、`unload`、`kernel_exports`、`registry` 和 `pnp_devices`。普通文件请求接受 `kind`、可选且互斥的 `device` 或 `device_id`，以及可选的 `file`。IOCTL 必须提供 `code`，并接受 `input`、`output_size` 和 `direct_input`。`read` 接受 `output_size` 和 `byte_offset`；`write` 接受 `input` 和 `byte_offset`。偏移默认为零，接受整数或十六进制字符串，且必须位于有符号 64 位整数的非负范围内。生命周期请求拒绝传输字段。未知或重复字段会被拒绝。`code` 接受无符号 32 位 JSON 整数或 `0x` 十六进制字符串。`input` 是长度为偶数且不带前缀或空格的十六进制字节字符串；省略表示空输入。`output_size` 为无符号 JSON 整数，省略表示零。不接受小数以及浮点数写法。

仅 READ／WRITE／IOCTL 请求接受可选字段 `cancel_after_100ns`，它必须是 0 到 `INT64_MAX`（9223372036854775807）之间的 JSON 整数。该值相对于请求提交时刻，以虚拟 100 ns 为单位，并非墙钟时间。对 KMDF，零表示框架路由后、来宾 I/O 回调前触发取消；若路由已直接完成请求，则完成优先。对 WDM，零在派发返回后生效。正数延迟仅在没有就绪回调或执行帧时，随时间推进至定时器、等待或取消期限而触发。挂起的 WDM IRP 会在持有取消自旋锁的 `DISPATCH_LEVEL` 调用已注册的取消例程；例程必须使用 `Irp->CancelIrql` 释放锁后再完成请求。通用队列与 PnP 取消仍不支持。每个请求报告都包含 `cancel_requested_at_100ns`，值为取消实际发生时的绝对虚拟时间；若未发生取消，包括完成先发生的情况，则为 null。请求取消本身不会完成 IRP，也不规定最终状态。
`IoSetCancelRoutine`、`IoAcquireCancelSpinLock`、`IoReleaseCancelSpinLock` 和 `IoCancelIrp` 共用同一 IRP 状态与取消自旋锁；`IoCancelIrp` 同步调用已注册例程，并返回是否实际调用。

可选布尔字段 `user_unmap_after_dispatch` 适用于非空的 WDM neither 传输：派发返回后、排队工作或取消执行前撤销原用户虚拟地址的访问权限。已锁定的 MDL 页面及其系统别名仍可使用，直到驱动解锁；原始用户指针和新的锁页操作会失败。若输出用户地址已撤销，`output_hex` 为空。原始及显式声明的场景分配地址不复用，也不重新映射这些地址或模拟任意时刻撤销；下文 MDL 用户视图区的地址复用不受此限制。

`requestor_process_id` 指定合成请求进程 ID（默认 4096，范围 5 到 `UINT32_MAX`）。`IoGetRequestorProcessId` 返回活动 IRP 的请求进程 ID；`PsGetCurrentProcessId` 在前台派发时返回该 ID，在建模的系统工作项中返回 4。切换进程后，其他进程的原始用户 VA 不可访问，但已锁定 MDL 的系统别名仍有效。`requestor_exit_after_dispatch` 在派发返回后撤销该进程所有原始用户 VA，并拒绝来自该 ID 的新 I/O；显式 CLEANUP/CLOSE 仍可执行，不自动推断取消或句柄清理。

系统工作项可通过 `IoGetRequestorProcess` 获取活动 IRP 的不透明请求进程对象，用 `KeStackAttachProcess` 和可写的内核 `KAPC_STATE` 临时附加，在原始用户 VA 上操作，再以同一状态调用 `KeUnstackDetachProcess`。`IoGetCurrentProcess` 与 `PsGetProcessId` 反映附加进程；`PsGetCurrentProcessId` 仍返回创建工作线程的系统进程 ID 4。进程退出、IRP 结束、错误配对、附加期间等待或完成 IRP 均明确报错。

对于 direct IOCTL，`input` 初始化第一个系统缓冲区，`direct_input` 初始化由 MDL 描述的独立第二缓冲区，并补零至 `output_size`。`METHOD_IN_DIRECT` 要求可读访问，但不意味着系统映射为只读。两种方法都使用可读写的场景缓冲区。`MdlMappingNoWrite` 移除映射的写权限，`MdlMappingNoExecute` 移除执行权限。解除映射会撤销系统虚拟地址；重新映射保留同一份锁定数据。请求完成时 MDL 和映射均失效。模型支持 WDM 宏使用的公共 MDL 字段；进程字段、未构建描述符的 PFN 访问、手工构造的 MDL、超出下述受限进程契约的用户映射以及通过原始 UserBuffer 直接访问都会被拒绝；已构建描述符的 PFN 数组只读。零长度 direct 缓冲区的 MDL 为空。

`IoAllocateMdl` 为非空、不溢出的缓冲区分配元数据，不探测或锁定缓冲区。建模描述符及其 PFN 数组必须能由 16 位大小字段精确表示；页内偏移也计入 PFN 容量。描述符存储大小与所描述缓冲区的大小独立。 `Irp` 可为 NULL 或有效的建模 IRP。主描述符替换当前驱动链的头部，脱离的描述符仍由驱动拥有；`SecondaryBuffer` 将描述符追加到链尾，空链则设置为头部。原始请求拥有的 direct-I/O MDL 必须保持可达，不能由驱动替换或释放。`ChargeQuota` 必须为 FALSE；对象区耗尽时返回 NULL。`MmBuildMdlForNonPagedPool` 要求完整范围位于同一个有效非分页池分配内。安全辅助函数和 WDM 宏复用原始地址与权限；新增禁止写入／执行标志不改变既有权限，额外系统映射及解除映射会被拒绝。`IoFreeMdl` 仅释放指定的驱动描述符，不解除链连接，也不沿 `Next` 释放后继；手动释放用户 MDL 前必须先解锁。池缓冲区具有独立生命周期，只要不再访问已释放存储，两种释放顺序均受支持。

驱动可修改 `MDL.Next` 和 `IRP.MdlAddress`，插入或脱离建模描述符。IRP 最终完成前验证当前整条链，随后解锁附加的用户 MDL、撤销其别名并释放附加描述符；脱离的描述符和池缓冲区仍由驱动拥有。环、未知或已释放节点、多个活动 IRP 共用描述符，以及把 WDF 私有描述符接入 WDM 链，均明确失败。活动 DMA 和调度器依赖阻止过早回收。其他建模 MDL 字段及已构建 PFN 数组只读；进程字段、未构建 PFN、手工 MDL、任意进程映射仍不支持。卸载前必须释放剩余驱动描述符。

当 IOCTL 的 `output_size` 非零时，`Information` 不得超过该大小，即使输入缓冲区更大。无输出缓冲区的 IOCTL 可在此字段返回驱动自定义结果，且不会复制输出字节；`information_hex` 精确保留原始 64 位值。

对于 READ/WRITE，`DO_BUFFERED_IO` 或 `DO_DIRECT_IO` 决定缓冲或直接传输。两个标志都未设置时，原始用户地址仅放在 `IRP.UserBuffer`：WRITE 对应输入，READ 对应输出；模型不会隐式创建 SystemBuffer 或 MDL。驱动必须在调用者上下文中探测并访问，或在延后处理前锁页。`user_input_access` 可用于 neither WRITE，`user_output_access` 可用于 neither READ；缓冲／直接 READ/WRITE 若显式指定这些权限，将在派发前停止。两个标志同时设置也会停止。Information 按传输长度检查；write 返回计数，read 返回字节。

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

`configuration` 对象记录本次执行的限制、服务名及 `kernel_exports` 覆盖配置。配置标识为 `wdm-x64-scheduled-v100`。`nt_status` 始终是 DriverEntry 的结果，而 `scenario_success` 综合描述初始化及已完成请求的结果。`phase`、`requests` 和 `unload_completed` 表明请求生命周期的哪些部分已运行。每次 API 调用及 CPU 写入也会记录阶段（`driver_entry`、`add_device:<ID>`、`request:N`, `callback:N` 或 `unload`）。每个请求报告派发状态与 I/O 状态、是否完成、information 长度以及返回的 `output_hex` 字节。`preferred_image_base` 描述原始 PE 基址。`security_cookie` 为已初始化 cookie 的来宾地址；若无需 cookie，则为 `"0x0"`。请求报告字段为 `kind`、`device`、`device_id`、`pnp`、`file`, `requestor_process_id`、`byte_offset`、`code`、`irp`、`completed`、`cancel_requested_at_100ns`、`dispatch_status`、`io_status`、`information`、`information_hex` 和 `output_hex`。 `configuration.registry` 保留原始注册表配置。 `information_hex` 以十六进制字符串精确保留原始 64 位 `IoStatus.Information`；原有数值字段 `information` 仍保留。

工作项观察记录使用 `callback:N` 阶段。待处理请求的 `dispatch_status` 保留 `STATUS_PENDING`，最终完成状态单独记录在 `io_status`，并据此计算该请求对 `scenario_success` 的影响。

`ExRaiseStatus` 将 NTSTATUS 低 32 位传给来宾异常处理器；`ExRaiseAccessViolation` 和 `ExRaiseDatatypeMisalignment` 分别抛出 `STATUS_ACCESS_VIOLATION` 和 `STATUS_DATATYPE_MISALIGNMENT`。此配置遵循各自的 Microsoft DDI 文档：ExRaiseStatus 允许 `APC_LEVEL`，两个无参例程要求 `PASSIVE_LEVEL`。部分 WDK SAL 注解允许这两个包装函数在 APC_LEVEL 运行；此配置保留文档规定的较严格上限。抛出异常的调用保持 `result: null`，在 `detail` 记录异常码，不会报告 API 成功返回。

异常递送使用镜像已解码的 x64 版本 1 展开表及 `__C_specific_handler` 作用域，执行真实来宾过滤器、处理器和展开过程中的 `__finally`。过滤器返回零继续搜索，正数选择处理器，负数请求继续执行；负值仅允许恢复可捕获的用户 CPU 访存异常，且只接受经过验证的 `CONTEXT_INTEGER | CONTEXT_CONTROL` 修改，其余完整原始 CPU 状态保持不变。支持普通辅助函数栈帧展开、保存的非易失通用寄存器恢复、当前执行栈边界及 `GetExceptionCode()`；正常执行的 finally 由来宾代码运行，异常展开的 finally 也实际执行。过滤器与 finally 继承父执行的进程／线程身份和用户访问权限，原本无用户权限的系统工作项不会因此获得权限。处理器可向受支持的外层作用域再次抛出异常。支持过滤器／finally 内嵌套及冲突展开、链式 V1 元数据、部分序言、规范尾声，以及完整 XMM6–XMM15 恢复。异常记录保留链接，已进入的 finally 不会重复执行。C++ 处理机制和不完整元数据仍明确拒绝。未捕获的 API 异常以 `model_error` 停止；其他不属于可捕获用户访存异常的 CPU 故障仍终止执行。 常量 `EXCEPTION_EXECUTE_HANDLER` 直接选中对应处理器。只有选中处理器后，展开才执行离开作用域的 finally；搜索期间或过滤器恢复原执行时不运行清理。每次过滤器返回都验证 `EXCEPTION_POINTERS`、异常记录及不支持的 `CONTEXT` 字段，修改这些内容会明确失败。模型 API 抛出的异常不能通过负过滤器恢复。

原生 x64 KVM/WHP 通过 `X64SIMDException` 将实际 `#XM` 故障交给驱动 C SEH。硬件故障的 `CONTEXT` 保存 XMM0–15 和 MXCSR。过滤器、异常展开的 finally 回调及选定处理器以 MXCSR `0x1f80`、清除 DF 的状态执行。负过滤器可修改 XMM 及顶层 `CONTEXT.MxCsr`（按客户 CPU 掩码截断）后重试原指令；`FltSave.MxCsr` 不控制内核恢复。模型 API 抛出仍保留整数/控制记录；x87/AVX 上下文修改继续明确拒绝。

C SEH 作用域仍使用左闭右开区间。合法的 `__C_specific_handler` 落点可能位于其保护区间内：[LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608) 将 `EndLabel + 1` 写为区间末端。Windows OS 模型保留原始端点，并独立校验目标可执行性、所属函数和续接身份，重定位后同样如此。`KernelSEHContinuationCases.def` 保留原始样例布局；`ScopeEndLabelMayOverlapTheHandlerLandingPad` 覆盖常量处理器和过滤器。配套测试验证末端排除，以及非法目标被拒绝后派发状态仍可重试。这些纯模型检查纳入 `NeverDNativeDriverTests`，禁用 Unicorn 时仍会执行。

目标展开同样使用原始作用域末端：若处理器目标仍在某个 `finally` 的保护区间内，便不会退出该作用域。`FinallyRespectsRawScopeEndAtHandlerTarget` 检查边界两侧，并在 Windows x64 上直接对照 `ntdll.dll!__C_specific_handler`。NeverD 不修补编译器生成的区间。Clang 20/21 构建的原始样例在 `T`、`J` 模式下返回来宾失败，因为偏移后的末端包含选定目标；Clang 23 构建会执行两级清理。[LLVM 修改 #144745](https://github.com/llvm/llvm-project/pull/144745) 移除了旧的 `+1` 偏移。这类编译器相关结果与后端故障分别记录。

`__GSHandlerCheck_SEH` 在搜索处理函数前和展开阶段分别检查镜像当前的安全 cookie，包括没有 finally 的栈帧。支持固定和动态对齐的 cookie 位置、带符号帧偏移和原始帧指针编码；包装层的 cookie 检查与 C 处理函数标志分别生效。序言和尾声展开不会读取尚未建立的 cookie。不匹配会在相关筛选函数、清理或处理函数运行前停止。独立的 `__GSHandlerCheck` 也在搜索与展开时检查 cookie，不虚构 C 作用域。识别必须依据精确符号或导入身份，不能从指令模式猜测匿名代码。GS/C++ 包装与 C++ 异常 personality 仍不支持。Microsoft 明确区分 SEH 与 C++ 异常，并说明 Windows 内核不支持 C++ 异常；这是内核契约，不是尚待补齐的 WDK 异常功能（[Handling Exceptions](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-exceptions)）。测试中的 `driver_seh_gs.h` 定义原创栈帧，并实际调用链接的 WDK cookie 检查函数。

原创 `driver_wdm_seh.c` 测试驱动使用真实 WDK 头文件，C 函数使用 `/GS-`，GS 路径由原创汇编栈帧覆盖。通过 `NEVERD_WDM_SEH_FIXTURE` 和 `NEVERD_WDM_SEH_CFG_FIXTURE` 配置普通及活动 CFG 镜像。[driver-seh-scenario.json](../examples/driver-seh-scenario.json) 示例重定位镜像，在 DriverEntry 中捕获 API 异常后卸载。 独立的 WDM METHOD_NEITHER 路径现已支持用户内存探测、MDL 锁页和可捕获的内存故障。

可为空的 `fault` 对象保留后端首次锁存的终止故障；可恢复的用户 CPU 访存异常通过上述 SEH 路径处理。其 `kind`、`pc`、可为空的 `address`、`size`、`access` 和 `interrupt` 区分未映射或受保护内存、无效范围、无效指令及 CPU 异常。地址使用十六进制字符串；大小和中断向量使用整数。用于观察的读取不能替换原始故障。发生故障的后端不能恢复执行，此记录也不会使这些后端故障能够由来宾 SEH 处理。

`instructions` 统计执行策略已准许的来宾指令尝试次数。被执行策略拒绝的指令不计数；已准许但在 CPU 中发生故障的指令计数。合成的 API 派发与返回哨兵不增加该计数。

场景布尔字段 `trace_memory_writes` 默认为 true。设为 false 时，`writes` 为空，事件预算仅统计 API 调用；内存校验、已完成写入的观察器以及指令和时间限制仍然生效。报告在 `configuration.trace_memory_writes` 中记录选择，C++ 接口为 `DriverOptions::TraceMemoryWrites`。

每个 `writes` 条目都带有 `semantics: "attempted_guest_write"`：记录栈外的 CPU 写入尝试，包括随后可能发生故障或被预算停止的尝试。它不保证写入已完成，也不包含 API 模型所做的写入。设备与驱动对象快照描述执行停止时观察到的状态。

包含 `neverd/sdk/NeverDCAPIEmulation.h`（或 C API 总头文件），创建会话，然后调用 `neverd_emulate_driver_json(session, path, options)`。显式传入非空路径会直接进入严格执行预检，无需先通过通用分析 API 加载。CLI 采用此路径。传入 `NULL` options 使用默认值。显式 `neverd_driver_options_v1` 选项要求 `struct_size` 精确匹配，且指令、内存、事件和超时预算均为正数。结果须用 `neverd_free_string` 释放。

若路径传入 `NULL`，则要求会话已加载文件，并独立于 IR 分析和函数受限加载重新解析该文件。两种方式均保留会话映像不变。调用期间必须保持输入文件可用且不变。请求／执行环境建立失败时返回 `NULL` 并设置 `neverd_last_error`；执行停止则返回 JSON。未启用该功能的构建仍提供此 API，并报告启用方法。

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)` 使用相同的 v1 选项与所有权规则，另外接受严格验证的场景输入。必须传入非 NULL、以 NUL 结尾的 JSON 字符串。原有 `neverd_emulate_driver_json` ABI 保持不变，仍仅执行初始化。C++ 解析器 `driverOptionsFromScenarioJSON` 为 `emulateDriver` 的调用方提供相同的场景验证。

内部 C++ 入口为 `include/neverd/emulation/DriverSession.h` 中的 `neverd::emulation::emulateDriver`。格式解析由现有加载器负责；Windows 对象／API 行为由 `lib/emulation/os/windows` 负责；CPU 状态及执行由 Unicorn 适配器负责。适配器与模型使用相同的来宾内存接口。Windows API 行为不应放入 Unicorn fork。

`IoBuildPartialMdl` 支持已构建 MDL 的非空子范围；长度为零表示剩余范围。目标必须具有足够 PFN 容量。部分 MDL 共享物理页，不重复锁页；可继承现有系统映射或建立自己的系统别名。释放或通过真实 WDK 内联 `MmPrepareMdlForReuse` 准备重用时，只撤销自身拥有的映射。非分页源描述符可独立释放；仍有部分 MDL 依赖的根锁页或系统别名不能提前释放。IRP 完成先检查整条链，清理不依赖链中顺序；活动 DMA 阻止重建或提前释放。

`MmMapLockedPagesSpecifyCache` 支持在有效请求方进程（包括受限工作项附加上下文）中，以不高于 `APC_LEVEL` 的 IRQL 使用 `UserMode` 建立独立用户视图。`MmNonCached`、`MmCached` 和 `MmWriteCombined` 都是有效缓存参数；已具有缓存属性的物理页面继承原属性，不能通过新映射改变。当前普通 RAM 的属性为 Cached；保留枚举值会被拒绝，也不模拟 CPU 缓存时序。视图共享物理内容，不改变 `MappedSystemVa`；始终不可执行，`MdlMappingNoWrite` 使其只读。池内存必须非分页且独占完整页面。再次对用户视图锁页仍保持原 PFN。指定地址必须位于专用用户映射区且保留 MDL 页内偏移；空间不足引发可捕获的 `STATUS_INSUFFICIENT_RESOURCES`。

`MmUnmapLockedPages` 要求创建进程和精确的地址／MDL 配对。它真正移除别名并归还映射预算；自动分配和 `RequestedAddress` 都可复用解除映射后的 VA，新映射使用自己的物理存储及权限，已锁定页面和系统别名仍指向原物理页。视图按 `(PID, VA)` 标识，不同进程可同时保留数值相同、物理页及权限不同的地址；进程切换以事务方式替换 CPU 可见的绑定，预检失败保留原绑定和权限。进程退出只撤销该进程的视图，不影响其他进程的同址视图。撤销情境原始 VA 不影响独立 MDL 视图；活动视图仍阻止描述符、根锁页、池或请求存储提前释放。若用户视图中存在活动的不透明调度对象，进程切换会明确失败；不推测每进程的内核对象身份。通用虚拟内存分配及其他未建模的进程 API 仍不支持。

WDM `METHOD_NEITHER` 的 `Type3InputBuffer` 与 `IRP.UserBuffer` 分别指向独立的用户内存。`ProbeForRead` 只检查范围和对齐，不触碰页面；`ProbeForWrite` 会触碰每一页。`ExGetPreviousMode` 返回请求模式。`MmProbeAndLockPages` 锁定单个用户分配的页面，`MmGetSystemAddressForMdlSafe` 建立共享内核别名，`MmUnlockPages` 撤销别名并解锁。不支持任意进程地址空间。

非空 WDM `METHOD_NEITHER` IOCTL 请求可以分别将 `user_input_access` 和 `user_output_access` 设为 `read_write`（默认）、`read_only` 或 `no_access`。Neither WRITE 仅接受输入权限，neither READ 仅接受输出权限；缓冲／直接传输及空缓冲区拒绝这些字段。`no_access` 保留非空指针，但禁止访问对应页面。
报告的 `configuration.user_page_access` 仅列出显式设置的权限，并用从零开始的 `source_request_index` 标识请求；未设置的方向默认为 `read_write`。

## 有界并发 WDM 请求

WDM READ/WRITE/IOCTL 请求或无限并行 KMDF 默认队列中的请求可设 `defer_callback_drain: true`。只有派发返回 `STATUS_PENDING` 且 IRP 仍待完成时，执行器才会先提交下一条请求；下一条未设置该字段的请求之后会排空回调并完成整批请求。重叠请求可使用不同的文件对象，或使用显式异步打开的同一文件对象；最后一条请求若设置该字段，场景末尾会排空回调。同步文件对象上的重叠请求、任意线程抢占和外部请求到达仍不支持。

## 异步文件对象

仅 CREATE 请求可设置布尔字段 `asynchronous_file: true`；省略或 false 仍为同步打开。异步打开会清除来宾 `FILE_OBJECT` 的 `FO_SYNCHRONOUS_IO`，后续文件 IRP 也不设置 `IRP_SYNCHRONOUS_API`。只有显式延迟回调排空时，同一异步文件的 READ/WRITE/IOCTL 才能重叠；CLEANUP/CLOSE 仍须等待全部先前传输完成并最终化。异步文件不维护隐式文件位置，`byte_offset` 仍逐请求指定，省略时为零。非 CREATE 请求即使传入 false 也拒绝此字段。

## 本版补齐的执行语义

独立页支持 `MmAllocatePagesForMdl`、`MmAllocatePagesForMdlEx` 和 `MmFreePagesFromMdl`：遵守物理范围、部分分配及连续块约束；页与描述符分别释放。`MmProtectMdlSystemAddress` 修改独立系统映射的 PAGE 权限，保留 PFN、缓存属性和用户映射权限。KernelMode 池内存可由 `MmProbeAndLockPages` 锁定；分页池在 APC_LEVEL 以下锁定后可在 DISPATCH_LEVEL 访问，解锁前不能释放池。

显式 `messages` 描述 MSI 分配，每条消息提供地址、数据、向量、层级、亲和性及极性。事件的 `message_id` 是描述符内索引，ISR 第三个参数则是 PDO 全局索引。`CONNECT_MESSAGE_BASED`（3）返回真实消息表；`CONNECT_MESSAGE_BASED_PASSIVE`（5）与被动线路支持 PASSIVE_LEVEL 等待，同一中断串行化，不同中断独立保留执行帧。到达与实际递送时间分别记录；不推测 PCI 或设备协议。

`WdfDeviceInitSetPowerPolicyOwnership` 控制电源策略所有权。自管理 I/O 的 Init/Suspend/Restart/Flush/Cleanup 与 D0 前后回调实际执行，普通 D3/D0 保留硬件资源。默认策略将 Sleeping3/Working 对应到 D3/D0，并消费显式 `requested_device_power`；独立子 IRP 报告 `origin: "framework_power_policy"` 和 `response_index`。S3 等待子请求，S0 可在发出 D0 后完成。失败设备自动重新枚举仍未建模。

系统 Query 等待对应设备 Query，并保留其状态；查询本身不改变电源状态。

## x64 原生同步异常

checked x64 的 `DIV`/`IDIV` 使用处理器产生的结果和 `#DE`。KVM 通过私有 supervisor IDT/IST 接收异常，WHP 使用明确的异常拦截位图；异常保留原始上下文和可用的错误码，与后端传输错误分开。OS 模型必须先消费可恢复事件，再安装明确的继续执行上下文。Windows 驱动将零除及商溢出映射为 `STATUS_INTEGER_DIVIDE_BY_ZERO`，并执行实际 SEH filter、`__finally` 和重试。`NeverDX64ExceptionTests` 可在禁用 Unicorn 时构建；原始 WDK 用例由 `DriverWDMCPUException` 验证。缺少的 ARM64 主机覆盖会明确跳过。

## 完整 x87 状态

`NeverDEmulationArch` 独立负责 ISA、页表及 FP 状态布局，原生与 Unicorn 传输共用该层。x64 上下文保存 x87 控制、状态、TOP、物理标签、操作码、指令／数据指针和八个 80 位寄存器。`FP0`–`FP7` 使用 `RegisterValue`，标量访问拒绝截断；`FPTag` 是物理非空位图。`NeverDX64FPTests` 覆盖全部 TOP、精确运算的宿主 FXSAVE/FXRSTOR 对照及上下文恢复。这不新增 checked x87 指令，也不证明全部舍入语义；缺少原生主机时明确跳过。

使用 `executionCapabilities(Contract, ISA, Backend)` 查询所选后端的能力配置。`NativeLegacyX64` 描述原生 x64 驱动执行；`NeverDNativeDriverTests` 验证原有驱动集，也可在关闭 Unicorn 的构建中运行。

Checked ARM64 使用统一的完整状态提交边界。`Registers.def` 定义 39 个标量字段及 32 个 128 位向量寄存器；`captureAArch64State` 暂存全部读取、应用声明位宽及 NZCV 规范化，最后一次提交。Unicorn、KVM 和 WHP 传递相同清单，包括 TPIDR_EL0、TPIDRRO_EL0、TPIDR_EL1、FPCR 和 FPSR。原生适配器通过 CPACR_EL1 开启 FP/SIMD 访问。任一标量或向量读取失败、进入取消，都会保留完整调用方状态。

ARM64 KVM/WHP 初始化执行私有 `AArch64MachineProbe.def` 程序：NOP、向正无穷舍入的 FP32 加法和双通道 SIMD 加法。每步比较全部 39 个标量字段与 32 个向量，包括 TLS、NZCV、目标寄存器高位清零及保留和累积的 FPCR/FPSR 状态。自检只使用特权级监控存储，共享一个总截止时间。成功仅验证这段有界初始化程序；仍需独立的 ARM64 原生工作负载验证。 程序还包括密钥关闭时的 A/B 返回地址签名和认证，以及非防护页上的四种 BTI 指令。

自检还执行两次 `MRS CTR_EL0`，以及 `DC CVAU`、`DSB ISH`、`IC IVAU` 和 `ISB`，核对缓存几何信息稳定及完整状态。Checked EL0/EL1 接纳原始指令、所有具名基线 DSB 选项及 ISB SY。CTR 来自选定虚拟 CPU，不同传输可以不同。缓存目标必须在当前权限下指向可读普通 RAM，允许非对齐地址和别名；其他目标明确报未支持。维护操作不产生数据读写观察事件。投影保证指令执行的一致性，不模拟私有缓存内容或并行硬件 SMP。`NeverDAArch64CacheTests` 检查完整状态、只读页尾、拒绝项、停止、上下文、预算，以及通过跨页 RW/RX 别名更新 guest 代码；不可用的 KVM/WHP 主机明确跳过。

x64 KVM/WHP 原生初始化在私有 supervisor 页面执行 `X64MachineProbe.def`。一个时限覆盖 NOP、向正无穷舍入的 FP32 加法、双通道 SIMD 加法、FS/GS 加载及 CS/SS/CR8 读取；每一步比较完整的标量、XMM、物理 x87 和控制状态。x64 与 ARM64 自检都必须取得物理内存的独占执行租约。`MemoryProjection` 统一保存缓存身份（ISA、地址空间、映射代次、权限及监控变体）和各 ISA 已提交的页表根历史。构建器在改写私有字节前使缓存失效；失败的重建不能复用部分写入的页表，调用者也不能传入过期页表根。这些自检仅证明有界初始化；ARM64 原生工作负载仍缺少独立验证。

共享 XSAVE 解码器区分标准格式与压缩格式的 SSE 初始状态。XSTATE_BV[1] 清零时，两种格式都初始化 XMM 寄存器；标准格式仍读取并校验 MXCSR，压缩格式才初始化 MXCSR。`X64XsaveCases.def` 提供独立的数据布局和原创主机 XRSTOR 程序。`X64XsaveTests.cpp` 检查拒绝状态的原子性，并以真实主机执行对照两种格式，同时保留调用方 FP/SSE 状态。主机架构或所需指令功能不可用时，对照测试明确跳过。

`X64FPState.def` 声明压缩 AVX、AVX-512、CET_U/CET_S 和 AMX 传输布局，包括分量的 64 字节对齐。存在的扩展数据必须符合架构的全零初始状态；缺席分量的数据与对齐填充不定义状态。偏移由布局位决定，未知布局、非初始数据或错误长度会在发布前失败。`CompactedOffsetsFollowLayoutRatherThanPresentBits`、`WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`、`InitialCETComponentsDoNotHideFPState` 和 `InitialWideComponentsDoNotHideFPState` 覆盖 872 字节及 10752 字节 WHP 数据包。这项传输支持不准入上述扩展指令。

`WhpXsaveRegisters.def` 使用具名 x87/SSE 控制寄存器补充完整 XSAVE 数据包。最后操作码及指令/数据地址会显式写入并从主机回读；可补齐数据包中的零值字段，但非零元数据冲突或共有控制字段不一致时，会在发布状态前失败。`NamedMetadataRestoresOmittedPacketFields` 验证字段缺失场景，并保留完整 FP 数据。

原生 `FOP/FIP/FDP` 遵循宿主 x87 保存、恢复规则。没有未屏蔽的待处理异常时，AMD 可能清零这些字段；快照保留实际观测值。`X64MachineProbe.def` 与精确 NOP/上下文测试使用一致的待处理异常状态，确保每个字段有效并逐项比较，不屏蔽差异。宿主进程 FXRSTOR64/FXSAVE64 参考程序覆盖两种状态；后端不会用输入元数据替代宿主结果。

共享的 `encodeX64XsaveState` / `decodeX64XsaveState` 编解码层拥有标准及压缩 FP/SSE 数据包、物理 TOP 轮转、缺失组件的初始状态和原子校验。WHP 使用完整 XSAVE API，优先选择 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`，旧 XSAVE API 作为兼容路径。单独的旧 x87 寄存器接口不能替代完整数据包。非初始扩展组件、畸形头部、非法控制位和截断捕获明确失败。WHP 映射错误保留 HRESULT、GPA 和大小以便诊断。

`CheckedX64Instructions.def` 通过既有 CPU 后端准入 8/16/32/64 位无符号 `MUL` 和 `CBW/CWDE/CDQE/CWD/CDQ/CQO`。`NeverDX64IntegerTests` 使用独立的 `X64IntegerCases.def` 编码和预期值，在两种特权级验证部分寄存器保留、32 位零扩展、乘积高低两部分、已定义的 CF/OF 结果及符号扩展不改变标志位。普通 RAM 乘法保留完整访问范围的权限检查和读观察回调；故障或观察回调中止会保留隐式输出寄存器及 PC。设备操作数仍不支持。这些用例也在 checked Unicorn 上运行；不可用的原生后端明确跳过。

`X64BitInstructions.def` 支持 16/32/64 位寄存器及普通 RAM 的 `BT/BTS/BTR/BTC`。寄存器位索引按操作数宽度解释为有符号数并选中完整数据字；立即数索引限制在基址的数据字内。地址宽度截断先于 FS/GS 基址相加。CF 与写入值由处理器提供；`RAMTransaction` 在观察回调接受前保留私有执行结果。完整范围权限检查覆盖独立页面分配和别名。停止、回调失败或页面权限不足均保留原始 CPU 和 RAM。LOCK 仅支持自然对齐的内存修改形式；MMIO 和硬件并行 SMP 仍不支持。`X64BitStringTests.cpp` 使用独立编码与 x64 本机实际执行对照，检查负索引、宽度截断、跨页访问、取消及非法 LOCK 形式。参见 [Intel 指令参考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。

`X64StringInstructions.def` 统一管理普通 RAM 上 8/16/32/64 位的 `MOVS/STOS/LODS`；`CLD/STD` 只改变方向标志。每个 REP 元素在观察回调前验证整个操作数，并在一个可恢复边界提交。后续故障保留此前完成的元素；取消或回调异常不改变当前元素。FS/GS 仅作用于源地址，且在地址宽度截断之后相加。AL/AX 加载保留高位，EAX 加载零扩展。32 位地址模式的零次 REP 要求计数高位为零，MOVS/STOS 还要求参与的地址寄存器高位为零，否则不同真实 CPU 实现会产生不同结果。MOVS/STOS/LODS 的 REPNE 形式及 STOS/LODS 设备操作数仍不支持。`X64StringTransferTests.cpp` 用独立的主机指令对照宽度、方向、重叠和零次数，并分别检查权限、别名、回绕、故障和恢复。原创 WDK 资源驱动通过 `driver_resource_strings.def` 执行四种宽度的 STOS/LODS。

`X64StringInstructions.def` 还统一管理普通 RAM 上 8/16/32/64 位的 `CMPS/SCAS` 及 `REPE/REPNE`。每个元素在观察回调前验证全部读取操作数，更新六个算术标志，并在首次满足终止条件时退出。数据故障恢复本次连续 REP 执行开始时的标志，同时保留已完成的指针和计数更新；公开接口恢复执行时，以已发布的 CPU 状态重新开始。停止和观察回调异常不改变当前元素，提前终止也不会读取下一个元素。FS/GS 仅影响 CMPS 源地址；SCAS 保留累加器和未使用的源寄存器。设备操作数及有歧义的 32 位零次数高位状态仍不支持。`X64StringComparisonTests.cpp` 用独立主机指令对照标志、方向、别名、回绕、权限和恢复，并通过 Linux x64 信号测试读取真实故障时的寄存器。原创 WDK 资源驱动通过 `driver_resource_strings.def` 执行四种宽度的两类条件重复形式。参见 [Intel 指令参考](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)。

存活的 WHP CPU 共享一个原生分区；最终关闭与重新创建由同一注册表锁串行保护。`WhpResourceCache.h` 复用协作式 VP 0；切换逻辑 CPU 前先销毁该 VP 并撤销其映射。并行 CPU 保留独立 VP 和私有 GPA 区间。寄存器、XSAVE 和取消请求均指向各自的 VP。x64 保留宿主默认 XSAVE 功能集，并通过 `WHvGetPartitionProperty` 验证实际分区配置。默认调度仍为协作式。

`CheckedAArch64Instructions.def` 和 `AArch64InstructionEffects` 在 EL0/EL1 接纳有界的基础 FP32/FP64 算术、比较、移动和定宽 SIMD 运算。FPCR 支持四种舍入模式、FZ 和 DN；FPSR 保留累积状态及 QC。未支持的控制位和状态位在修改前拒绝。FP16 算术、SVE/SME、未屏蔽异常、可选扩展及未列出的形式明确失败。这些 CPU 能力不代表已经支持 Windows ARM64 驱动加载或新增 OS 环境。

`AArch64InstructionEffects` 负责标量及 FP/SIMD 的单次、成对 RAM 访问范围，单个操作数最大 128 位。共享地址空间在进入 CPU 前验证每一页；`RAMTransaction` 只提交完整声明的物理写入。128 位写观察器在生效前按顺序收到两个 64 位字。停止和故障保留 RAM、向量和地址写回。Xn/Vn 的编号重叠合法；发生地址回绕的成对访问被拒绝。`NeverDAArch64MemoryTests` 使用独立的 `AArch64CrossPageCases.def` 与 `AArch64VectorMemoryCases.def` 编码。

KVM x64/ARM64 通过 `KvmRunControl` 在同一专用 vCPU 工作线程上准备状态、进入 `KVM_RUN` 和读取状态。`EINTR` 重试只准备一次；取消进入或读取失败不能发布状态。`KvmAArch64Machine.cpp` 在该线程上执行地址转换维护及完整标量、向量传递，并共用一次单步期限。调用线程只在确认完成后提交；ISA 解码、RAM 事务、OS 策略和观察器仍属于调用线程。ARM64 原生运行仍缺少实机证据。

KVM 根据 `X64HostRegisters.def` 和 `X64FPState.def` 将通用寄存器及完整 FP/SSE 状态与上次确认完成的调试退出状态比较，只重新安装变化的输入。宿主写入和上下文恢复也参与比较；异常、取消及失败会使复用失效。每条指令仍启用单步并读取真实的通用及 FP 状态。

硬件执行本身不保证更低的端到端耗时。当前原生执行逐条进行指令准入、观察、状态传输和 VM 退出。比较相同原始镜像与场景时，应使用一致的指令和事件预算，同时报告结果一致性与耗时；测量 CLI 延迟时应包含启动和加载。

当 `GuardFlags` 为零时，未启用 CFG 的指针槽仍然有效。加载器校验其存储、可执行回退目标以及重定位时完整的 `DIR64` 覆盖，并保留原始来宾指针。启用 CFG 的映像仍必须同时具备映像启用位和插桩/函数表标志。`DriverGuardCases.def` 与 `NeverDDriverGuardMetadataTests` 通过 Unicorn、KVM 和 WHP 覆盖这些情况，也可用于禁用 Unicorn 的构建。

checked Unicorn 使用 `MachineRunControl`：ARM64 维护、来宾执行和完整状态回读共用一次单步额度。`UC_HOOK_CODE` 在指令入口检查借用的停止令牌和期限；同步引擎调用返回前解除 hook 借用，而机器单步保留控制直到发布状态。Unicorn 与 WHP 暂存完整 CPU 状态，并在成功步骤发布前检查同一个控制条件。WHP 在准备前只创建一次额度。已确认的 x64 CPU 异常优先于回读期间到来的停止请求。回读取消时，checked RAM 事务丢弃推测写入；非受限软件契约不变。 `MachineInterruptedError` 区分已确认取消与主机或回读失败。共享 checked CPU 返回 `Stopped` 或 `Deadline`，保留 CPU/RAM 并允许重试；真实故障即使伴随停止请求也仍是 `BackendFailure`。

`RunDeadline::invoke` 在 WHP 入口已停止或过期时拒绝调用宿主，取消期间保留真实宿主结果，并在释放借用的停止标记前确认中断回调结束。KVM 和 WHP 在持有执行租约的调用线程上验证完整捕获的私有状态，然后分类同时到达的停止或超时。真实宿主错误、捕获失败以及经过认证的 x64 CPU 异常保持更高优先级。普通成功状态在取消检查结束前保持私有；已确认的中断丢弃推测性的 CPU/RAM 效果并允许重试。准备、原生执行和捕获共用一次单步宽限。这些控制提供协作式取消，不保证硬性墙钟时限。

CPU0 显式抢占、虚拟时钟语义及当前边界见[驱动调度](driver-scheduling.md)。

## HAL 导出与性能计数器

`HAL.dll` 是独立且模块名不区分大小写的导入提供者。静态导入与 `MmGetSystemRoutineAddress` 共享内核和 HAL 中精确、区分大小写的导出身份；存在冲突的有效身份时明确拒绝。`kernel_exports` 对已知 HAL 例程的覆盖作用于 HAL 命名空间，其他显式声明仍属于内核。未知 HAL 导入保留延迟陷阱，不会仅因同名而获得内核 API 语义。

`KeQueryPerformanceCounter` 返回调度器共享的 100 ns 时钟值，固定频率为每秒 10,000,000 次。可选输出指针经过完整八字节写入权限及对象生命周期检查。调用支持所有有效 x64 IRQL。协作模式只在已有调度边界推进时间；指令时钟模式沿用配置的计时方式。读取计数器不会创建第二个时钟或自行推进时间。这是确定性的执行配置，不是宿主硬件测量。独立编译的运行时样本在原始及重定位地址上检查静态／动态身份、频率与单调性，并由原生 CPU 后端执行。

`RDTSC` 与 `RDTSCP` 和 `KeQueryPerformanceCounter` 共用 10 MHz 调度时钟。`RDTSCP` 在 ECX 中返回单个模型处理器的编号零。EAX/EDX（以及 RDTSCP 的 ECX）清零高 32 位，其他寄存器和标志保持不变。协作模式下读取不推进时间；显式指令调度模式在计入本条已获准指令后读取时间，结果不依赖时间片长度。溢出在写入寄存器结果之前停止，指令预算和观察器停止仍然有效。此配置不测量宿主 TSC 频率，也不暴露宿主处理器身份；MSR 访问、RDPMC 和其他未建模 CPU 查询仍不支持。

通过 `KeQueryPerformanceCounter`、`RDTSC` 或 `RDTSCP` 读取时钟会保留明确的驱动恢复依赖。捕获的计数器和频率值尚无面向新内核环境的重绑定契约。默认恢复返回 `unsupported_state`；`snapshot_only` 保留此诊断。

`KernelModuleImages` 根据 `KernelExportRegistry` 生成可读的 PE 头和导出表；静态导入、动态查找和模块枚举共享同一组地址。提供者代码保持不透明，清单描述的是模拟环境，而非宿主内核。两个输出都在写入前检查，包括池内存生命周期与重叠。Nt 查询要求已知的内核 previous-mode；Zw 查询采用内核调用契约。完整模块查询会保留明确的恢复依赖，即使其缓冲区已释放；提供者映像指针也按借用状态跟踪。`KernelExportTests.cpp` 检查 PE 解析、权限、ABI 字段、写入拒绝和恢复依赖；原创编译运行时夹具在 CPU 后端遍历这些导出表。
