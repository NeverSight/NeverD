**语言**: [English](../driver-emulation.md) | [简体中文](driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← 文档索引](README.md)

# Windows 驱动模拟

NeverD 的可选驱动模拟器执行受支持的 x64 WDM 驱动的 PE 入口点，并可在卸载前执行显式指定的请求场景（默认串行）。它使用 Unicorn 执行 CPU 指令，使用 NeverD 自有的有界 Windows 环境模型。它不会将驱动加载到宿主内核，也不会把来宾 API 调用转发给宿主操作系统服务。

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
| `METHOD_IN_DIRECT`、`METHOD_OUT_DIRECT` | 请求拥有的 MDL、系统映射、共享物理页身份及 SG DMA | 用户映射及其他 DMA 接口 |
| 驱动自行分配的 MDL | 描述非分页池或单个用户分配的独立描述符，共享物理页身份 | 不支持 IRP 关联、MDL 链或任意进程 |
| READ/WRITE | 缓冲／直接／neither I/O，可由工作项或 DPC 完成 | 仅支持下列 API 子集；同步文件仍不支持重叠请求，也不模拟隐式文件位置 |
| WDM `METHOD_NEITHER` | 独立用户缓冲区、访问探测、MDL 锁页、合成请求进程身份、派发后 VA 撤销或进程退出及有限取消 | 不支持通用进程附加、任意用户映射或重新映射 |
| KMDF 1.33 非 PnP 驱动 | 版本绑定、对象／上下文、具名控制设备、手动、顺序或有限或无限并行默认及非默认队列，以及实际执行回调的缓冲／直接／neither 请求 | 不支持 PnP 设备、通用队列调度、类扩展或 UMDF |
| PnP 总线／功能／过滤驱动 | 显式无资源或固定寄存器银行 PDO、来宾 AddDevice 和八种常见 PnP 生命周期次功能 | 其他 PnP 操作、通用电源管理、其他硬件／资源及 通用 KMDF PnP |
| 存储、网络、显示、文件系统及微过滤驱动 | 不支持相关子系统契约 | 端口／类／微端口框架、NDIS/WFP、图形或文件系统服务 |
| 工作项、定时器、DPC、事件与等待 | 当前执行 IRQL 在派发与工作项中为 `PASSIVE_LEVEL`，在 DPC 中为 `DISPATCH_LEVEL` | 仅支持下列 API 子集；仅允许跨文件或同一异步文件上的有界批量提交 |
| 使用进程／线程回调、句柄、注册表／文件操作或内核模块发现的驱动 | 支持配置的注册表；其他行为仅限下列 API | 对象管理器、系统状态以及回调／事件产生机制 |
| 硬件、DMA、PCI、中断或虚拟化驱动 | 支持显式内存寄存器银行、MMIO、独占 latched 中断回调及有界一致性公共缓冲区／SG／通道 DMA | 其他设备模型、任意物理 RAM、PCI、端口、其他中断模式、其他 DMA 接口及特权 CPU 状态 |
| x86 或 ARM64 Windows 驱动 | 拒绝 | 相应架构的加载、ABI 及执行模型 |
| x64 CFG | 验证目标表及检查／分派调用；未启用的插桩保留来宾回退函数 | XFG、导出抑制、不支持的加载配置和 TLS 仍被拒绝 |

未使用的不支持导入项可以保持绑定。一旦执行到不支持的操作，便会停止并给出诊断及此前收集的观察结果。仅 DriverEntry 成功，并不能证明后续派发、硬件或框架路径也受支持。下方 API 表是受支持子集的权威定义。

## 执行契约

此配置在 CPU0 上以确定性的协作调度模拟 x64 WDM 生命周期。 执行从 PE 入口点开始；若存在编译器生成的入口包装函数，也会保留并执行。DriverEntry 必须返回 `STATUS_SUCCESS` 才能完成初始化；非零的成功状态或待处理状态会因初始化契约不受支持而停止。失败状态则作为已完成的初始化结果保留。所有对象、字符串、栈、函数指针与分配均位于来宾内存。模型根据配置的服务名（默认为 `NeverDDriver`）提供 `DRIVER_OBJECT` 和注册表路径。

适配器使用 Unicorn 的虚拟 TLB 模式保留来宾虚拟地址，包括规范的高位内核地址，无需合成 Windows 页表。初始 RFLAGS 为 `0x202`；软件设备配置采用固定的 64 字节缓存行。这些都是本执行场景的显式属性。内联 x64 CR8 读取观察到相同的 `PASSIVE_LEVEL` / `DISPATCH_LEVEL`；CR8 写入及其他控制寄存器操作仍不受支持。

未知导入项绑定到延迟陷阱。未使用的导入项不会阻止执行；执行其 thunk 或读取未建模的导出数据值时，会以 `unsupported_api` 停止。不支持的 CPU 环境效果也会明确停止。NeverD 不会用成功返回值替代未实现的调用。格式错误的映像或不支持的加载要求会在执行前失败。

排队的 `DelayedWorkQueue` 工作项在 `PASSIVE_LEVEL` 执行，来宾 DPC 回调在 `DISPATCH_LEVEL` 接收规定的四个参数。CPU0 在调用返回和阻塞等待的边界进行确定性的协作调度。相对、绝对和周期定时器使用虚拟时间；没有可运行的执行帧时，时间推进到下一定时器、等待或取消期限。通知型与同步型事件／定时器保留各自的信号消耗语义。每个回调拥有独立的来宾栈；多个阻塞帧保留局部变量和完整 CPU 上下文，来宾内存仍然共享。Win64 回调入口将前四个参数放入寄存器，其余参数放入栈。请求默认串行处理：标记 IRP 为待处理的派发函数必须返回 `STATUS_PENDING`。跨文件或同一异步文件上的 WDM 传输可显式设置 `defer_callback_drain`，在回调运行前提交下一个请求。待处理请求或无限等待没有可用生产者时，以停滞的 `model_error` 停止。指令、内存、观察记录与墙钟时间预算仍共用。

这是有界调度模型，并不代表完整 Windows 异步支持。可警报或用户模式等待、APC、通用 WDM 请求取消、任意并发场景提交、UMDF、通用 KMDF PnP 设备及通用队列调度、完整 PnP／电源、通用硬件、其他 DMA 接口与其他中断模式仍不支持。仅初始化调用会执行显式排队的回调，不会隐式生成请求或卸载。

工作项在回调开始前出队，因此回调可以释放自身的工作项。释放仍在队列中的项、重复入队、使用失效对象或非来宾可执行内存中的回调地址都会明确失败。设备引用保留到回调返回。请求卸载要求释放全部工作项并完成排队工作。CPU 上下文保存与恢复包含通用、SIMD、FPU 和控制状态；来宾内存始终共享，故障 CPU 不能靠恢复上下文继续执行。
删除会延后到文件对象及排队／执行中的工作项引用全部释放。对象区耗尽时，工作项分配返回 NULL。

映像默认使用首选基址，除非场景选择了有效的重定位地址。映像必须为使用 native 子系统的 PE32+ x64 可执行文件。导入可来自 `ntoskrnl.exe`、`ntkrnlmp.exe` 或 `WDFLDR.SYS`。

执行加载器支持经验证的 x64 `DIR64` 基址重定位，以及有限的安全 cookie 加载配置；它会在入口包装函数执行前设置确定性的来宾 cookie。其他未建模的加载配置字段、TLS、延迟／绑定导入、按序号导入以及托管映像都会被拒绝。映像还必须通过严格的范围与对齐检查。

启用的控制流保护（CFG）会验证 PE 标志、指针槽和已排序的可执行目标表。检查与分派辅助函数仅允许已声明的映像入口或已登记的 API 跳板，保留 Win64 调用状态，并拒绝未声明的目标。只有插桩而未启用 CFG 时，保留原始来宾回退指针。启用的 XFG、导出抑制和其他未建模的保护策略仍被拒绝；地址位于可执行内存并不使它成为合法目标。

WDM 设备栈可以包含同一来宾驱动拥有的多个设备对象。`IoAttachDeviceToDeviceStack` 将独立源设备附加到目标的当前栈顶，返回原栈顶，并设置 `StackSize` 和 `AlignmentRequirement`；它不修改驱动的 `NextDevice` 链，也不复制缓冲标志。`IoDetachDevice` 接收保存的下层设备，要求 `PASSIVE_LEVEL`；附加允许 IRQL 不高于 `DISPATCH_LEVEL`。打开具名下层设备时，请求派发到当前栈顶，而 `FILE_OBJECT.DeviceObject` 和报告保留具名设备身份。READ/WRITE 使用所选栈顶的缓冲标志。请求保存的路径在拆链、删除后仍保留各层设备，直到派发返回；内部引用不增加表示打开句柄数的 `ReferenceCount`。

`IofCallDriver` 和 `IoCallDriver` 辅助入口调用保存路径中的确切目标。真实内联 `IoCopyCurrentIrpStackLocationToNext`、`IoSkipCurrentIrpStackLocation` 和 `IoSetCompletionRoutine` 操作原始来宾 IRP；模型验证游标、数量和控制标志。下层派发返回真实状态，与 `IoStatus` 及完成回调返回值分开。完成展开先推进游标，按成功／错误／取消标志选择回调并向上传递 pending 状态；执行完成回调时，回调负责传播 pending，包括派发已返回 `STATUS_PENDING` 后的传播。`STATUS_MORE_PROCESSING_REQUIRED` 暂停展开并保留 IRP、MDL 和缓冲区，后续完成调用可以继续。嵌套完成要求外层返回停止结果，最终展开只释放一次存储。带所属子系统标记的续接保留嵌套 WDM／WDF 调用帧和继承的 IRQL。 在调用上层完成回调之前，已消耗的下层栈位置会被清零。

通用电源管理、驱动自行分配的 IRP、其他 PnP 次功能以及其他硬件／资源模型仍不支持。WDF 附加／转发、向仍有文件或回调的栈附加设备、拆除中间层、改变转发的主功能及指向保存路径之外的设备均明确报错。可选真实 WDK 样例 `driver_wdm_stack.c` 使用 `NEVERD_WDM_STACK_FIXTURE` 和 `NEVERD_WDM_STACK_CFG_FIXTURE`；原生与 C API／CLI 测试包括重定位，缺少产物会明确跳过。执行证据仍仅来自 Linux。

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

除 Removing/Removed 外，设备仍存在时，普通 CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE 会进入真实来宾派发。模型不根据 Stopped、StopPending、RemovePending 或电源状态虚构失败；驱动可按自身代码完成软件 I/O、拒绝或保留请求。公开执行器默认串行；跨文件或同一异步文件上的挂起 WDM 传输可显式批量提交。当前保留 IRP 若无可用生产者，不能靠后续场景中的 start 或 cleanup 唤醒，会以停滞 `model_error` 结束。Remove 前关闭文件、排空先前请求是当前配置的限制。`query_stop` 的最终 `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119) 要求尚未实现的资源重新查询，因此场景预检和来宾最终完成都明确拒绝；参见 [Microsoft QUERY_STOP 合约](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device)。停止／重启及突然移除不代表资源重平衡、通用电源管理 或 通用 KMDF PnP 支持。

无资源 PnP 使用原创真实 WDK `driver_wdm_pnp.c`、可选 `NEVERD_WDM_PNP_FIXTURE`／`NEVERD_WDM_PNP_CFG_FIXTURE`，并提供原生及 C API／CLI 测试；缺少产物会明确跳过，执行证据仍仅来自 Linux。

合成总线 `bus: "register_bank"` 为 PDO 提供显式固定内存资源。当 `interrupts` 非空时可省略 `resources`，两份列表合计至少包含一个资源。每个内存资源项包含 `id`、`raw_start`、`translated_start`、`length` 和 `registers`；每个寄存器必须指定 `offset`、`width`、`access`（`read_only` 或 `read_write`）以及初始 `value`。`DriverResources.h`／`DriverResources.def` 定义共享 C++ 与 JSON 契约。资源 ID 遵循有界 ASCII 标识符规则，并在每个 PDO 内唯一。上限为每 PDO 8 个／合计 32 个资源，每资源 256 个／合计 4096 个寄存器，每资源 1–1048576 字节。只支持自然对齐、精确匹配的 1／2／4 字节寄存器访问，值必须适合该宽度。两种物理区间均不得溢出；同一 PDO 内的原始区间不能重叠，转换后区间在全局不能重叠。空 `registers` 明确表示整个银行不可访问。地址和初值都是声明的事实，不来自宿主硬件，也不隐含零填充内存。`resource_free` 继续省略资源清单，START 指针保持 null。

START 接收独立、只读的原始与转换后 `CM_RESOURCE_LIST` 分配；对应的 Memory 描述符顺序一致，使用一个完整描述符、Internal 接口、总线 0、版本／修订号 1、DeviceExclusive 共享方式及 READ_WRITE 区间标志。寄存器自身的只读权限仍独立生效。下层 START 成功后，资源在上层完成回调之前可用；从 NotStarted／Stopped 发起的每次 START 都为相同固定分配建立新一轮资源身份。寄存器值仅在创建 PDO 时初始化一次，取消映射、STOP 和重启均保留值。失败的 START 及成功的 STOP／REMOVE 要求驱动在最终 IRP 完成前释放映射，模型不会静默清理。突然移除立即禁止新映射和寄存器访问，但仍可取消已有映射。提供者实际成功完成设备 SET 电源请求后更新硬件可访问性：D3 禁止访问，D0 仅在资源分配可用时允许访问；D3 仍允许建立映射，电源变化不会丢弃映射或重置寄存器值。

`MmMapIoSpace` 支持 NonCached；`MmMapIoSpaceEx` 支持 PAGE_NOCACHE 与 PAGE_READONLY 或 PAGE_READWRITE 的组合。两者仅映射单一资源分配内已声明的转换后子区间，并保留页内偏移。别名共享同一寄存器银行、保留独立权限，`MmUnmapIoSpace` 必须使用原始基址和精确长度。映射数量／虚拟地址窗口或配置内存预算耗尽时返回 NULL，后端故障仍明确失败。已取消映射的地址不会因后续映射而重新有效。标量及真实 REP 寄存器缓冲指令通过 CPU MMIO 检查执行；空洞、错误宽度、未对齐、只读写入、跨映射访问和执行均在寄存器产生效果前失败。内存 API 可执行自然对齐、精确匹配单个寄存器的 1／2／4 字节事务；更大的批量区间若触及 MMIO，会明确失败，不会自动拆分成寄存器事务。本范围不提供任意物理 RAM、资源重平衡、端口、其他中断模式、其他 DMA 接口或通用硬件行为。

可运行的[寄存器银行场景](../examples/driver-register-bank-scenario.json) 使用原创真实 WDK `driver_wdm_resources.c`，通过可选 `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE` 指定产物。它执行 14 个请求，覆盖延迟 START、文件 I/O、STOP、重启和移除，并通过驱动 IOCTL 观察持久寄存器值。`configuration.pnp_devices[].resources` 记录初始事实，物理地址使用无损十六进制表示；它不是另一份银行状态报告。C API 与 Python 继续使用原有 `scenario_json` 入口，不改变 `neverd_driver_options_v1`。缺少真实产物时明确跳过；当前执行证据仅来自 Linux。

同一 `register_bank` 提供者可单独声明 `interrupts`，也可与 `resources` 同时声明，至少一份列表必须非空。`resource_free` 拒绝显式 `interrupts` 字段，包括 `[]`。每个中断必须提供 `id`、`raw_vector`、`raw_level`、`raw_affinity`、`translated_vector`、`translated_level`、`translated_affinity`、`mode: "latched"` 及 `share: "device_exclusive"`。`DriverInterrupts.h`／`DriverInterrupts.def` 定义类型、拼写和上限：每 PDO 8 个／合计 32 个中断，PDO 内唯一且长度有界的 ASCII ID，全局独占的转换后向量，原始 level 0–65535，转换后 DIRQL 3–12。向量保留全部 32 位，不猜测向量与 IRQL 的关系。两个 affinity 掩码都必须为 1，CPU0 和 group0 是显式提供者事实。原始值与转换后值相互独立。同一 `CM_RESOURCE_LIST` 中先保留内存描述符顺序，再追加有序中断描述符；中断使用 type 2、DeviceExclusive 共享方式和 LATCHED 标志。此合成边沿触发配置不代表 PCI 共享电平中断线。

READ／WRITE／IOCTL 请求可声明 `interrupt_events`，每项显式指定 `after_100ns`、`device_id` 及 `interrupt_id`；其他请求种类即使该字段为空也会拒绝。上限为每请求 64 个／合计 1024 个事件，延迟是至多 INT64_MAX 的非负数。成功提交请求时确定计时起点，并捕获已连接的中断及其 PDO 资源代次。事件代表独立外部线路脉冲：源 IRP 完成不会取消事件，寄存器写入也不推导启用、状态或确认行为。时间只在空闲时推进，到期事件在下一个受支持的回调边界执行，因此 `after_100ns: 0` 不保证指令级抢占，也不保证在第一条来宾指令之前递送。同一时刻的生产者共同接受容量预检；提供者先发布硬件状态，再判定递送资格，ISR 回调先于 DPC／工作项。连接丢失、过期／不可用资源代次或物理 D3 会记录 `undelivered_reason` 并以 `model_error` 停止，不会重新绑定或静默延迟事件。

`IoConnectInterrupt` 使用真实的十一参数及精确的转换后分配。`IoConnectInterruptEx` 支持 FullySpecified（1）、LineBased（2，显式 PDO 上唯一分配的线路）和 FullySpecifiedGroup（4，group 0）；`IoDisconnectInterruptEx` 要求版本／上下文匹配。注册和断连要求 PASSIVE_LEVEL。仅支持私有中断锁、独占 latched 模式、CPU0／group0，且不保存浮点状态。同步 IRQL 必须等于分配的 DIRQL，LineBased 中的零会选择该级别。`KINTERRUPT` 不透明，地址永不复用。真实 ISR 接收 `(Interrupt, ServiceContext)`，从 AL 返回 BOOLEAN；`FALSE` 表示未认领，并非 NTSTATUS 失败。`KeSynchronizeExecution` 在同一锁下、DIRQL 级别执行真实单参数回调，返回其 BOOLEAN 并恢复调用方 IRQL／CR8。`KeAcquireInterruptSpinLock`／`KeReleaseInterruptSpinLock` 检查非递归所有权、原执行身份和保存的 IRQL，不能带着未释放锁从回调返回。中断内等待、调用方提供的共享锁、共享／电平／MSI／被动级中断和指令级中断抢占仍不支持。失败 START 及成功 STOP／REMOVE 要求最终完成前断开连接，上层完成回调可先拆除；断连不会静默丢弃已排队 DPC。

报告区分声明与观测。`configuration.pnp_devices[].interrupts` 保留资源，`configuration.interrupt_events` 将输入事件平铺，附带从零开始的 `source_request_index`（配置请求下标）及 `event_index`。根级 `interrupts` 行增加 `device_id`、`interrupt_id`、`epoch`、绝对 `due_at_100ns`，以及可空的 `occurred_at_100ns`、`delivered_at_100ns`、`returned_at_100ns`、`interrupt_object`、`return_value`、`claimed` 和 `undelivered_reason`。`claimed` 只根据真实返回值的低字节推导，时间戳是观测，不是虚构的回调结果。`scenario_success` 要求所有配置事件均已返回且没有未递送失败，未认领的 ISR 仍然有效。DPC 效果通过真实请求完成、API 调用及消息体现。可执行的[中断场景](../examples/driver-interrupt-scenario.json)使用原创真实 WDK `driver_wdm_interrupts.c`，由可选 `NEVERD_WDM_INTERRUPT_FIXTURE`／`NEVERD_WDM_INTERRUPT_CFG_FIXTURE` 指定：七个请求包括延迟 START、由 ISR→DPC 完成的 pending IOCTL、文件清理／关闭及移除。现有 C／Python `scenario_json` 边界和 `neverd_driver_options_v1` 布局不变。缺少真实产物明确跳过，执行证据仅来自 Linux。

`DriverDMA.h`／`DriverDMA.def` 为 `register_bank` PDO 增加可选的 `dma` 对象，与内存／中断分配并存；仅声明 DMA 不能取代这两类资源列表。七个字段都必须显式提供：`address_bits`（32 或 64）、`maximum_length`（1–1048576 字节）、`map_registers`（1–256）、`alignment`（1–4096 之间的二次幂）、`logical_base`（非零、页对齐）、`logical_length`（页对齐、4096–1073741824 字节）及布尔值 `scatter_gather`。逻辑地址窗口必须无溢出且落在地址位宽内。每个 PDO 都有独立逻辑地址域，不同设备的相同地址不会互为别名。转换后的 MMIO 资源不能与保留的模型 RAM 区间 `[0x1000000000, 0x1000100000)` 重叠。这些声明描述具备一致性的合成总线主设备，不代表宿主物理内存或 PCI 设备。

`IoGetDmaAdapter` 接受 Internal 总线主设备的历史 `DEVICE_DESCRIPTION` 版本 0／1 字段，发布版本为 1 的 `DMA_ADAPTER` 及真实的 104 字节 `DMA_OPERATIONS` 表。版本 2／3 探测返回 NULL，不读取现代结构尾部。每个间接方法绑定到确切的有效适配器，身份独立于内核导入。已实现 `AllocateCommonBuffer`、`FreeCommonBuffer`、`GetDmaAlignment`、`GetScatterGatherList`、`PutScatterGatherList`、`PutDmaAdapter`、`AllocateAdapterChannel`、`MapTransfer`、`FlushAdapterBuffers` 和 `FreeMapRegisters`。本配置不建模从属／系统 DMA 控制器，因此 `FreeAdapterChannel` 和 `ReadDmaCounter` 仍保留具名的不支持错误。公共缓冲区分配／释放及对齐查询要求 PASSIVE_LEVEL；Get／PutScatterGatherList 要求 DISPATCH_LEVEL，适配器释放允许 IRQL 不高于 DISPATCH_LEVEL。x64 忽略 `CacheEnabled`。不支持的版本探测、声明能力不兼容以及文档规定的分配资源不足返回 NULL；非法或未建模的接口选择及后端故障仍明确报错。

`AllocateAdapterChannel` 要求 DISPATCH_LEVEL，预约非 NULL 的不透明映射寄存器令牌。公共缓冲区、SG 列表和通道预约共享每 PDO 的同一额度及 FIFO。成功即接纳真实的立即或排队 `AdapterControl` 回调；请求个数过大时返回 `STATUS_INSUFFICIENT_RESOURCES`，不执行回调。每个来宾设备最多允许一个尚未结束的分配回调，禁止在 AdapterControl 内调用 AllocateAdapterChannel，包括经过嵌套回调的调用。回调四个参数包含注册时实际 `DEVICE_OBJECT.CurrentIrp` 的快照。来宾设备的该确切八字节字段可写，只接受零或经过此设备路由的有效 IRP。排队回调保持该包有效直到进入回调，此后回调可以完成它。当前仍无 StartIo，因此另一种 SG 回调的未使用 IRP 参数仍为 NULL。

`AdapterControl` 返回 32 位 `IO_ALLOCATION_ACTION`，忽略 RAX 高位，且该动作不会替换 AllocateAdapterChannel 的 `STATUS_SUCCESS`。`DeallocateObject` 在回调返回时释放未使用或已整体刷新完毕的寄存器；`DeallocateObjectKeepRegisters` 保留寄存器，直到 FreeMapRegisters 使用确切适配器、令牌及原始个数释放。返回 `KeepObject` 需要未建模的系统控制器，因此明确失败。新递送的分配在回调返回之前还不能作为已保留分配释放；其他先前保留的分配可按其自身契约释放。回调身份、保留的寄存器及正在映射的数据字节具有独立生命周期，适配器或设备销毁前会分别检查。

`MapTransfer` 和 `FlushAdapterBuffers` 允许 IRQL 不高于 DISPATCH_LEVEL；通道分配和寄存器释放要求 DISPATCH_LEVEL。MapTransfer 接受相对 MDL 的位置，读取并更新真实 ULONG 长度，按值返回逻辑地址。有界 SG 配置每次返回一个底层页片段，同一 MDL 和方向上紧接的后续位置扩展同一次操作。非 SG 在预约个数足够时一次映射整个请求范围，不缩短长度。第一次映射按寄存器预约大小预留永不复用的逻辑窗口，其他分配可穿插但不会与其重叠。所有片段共用一个原地增长的物理固定范围。设备事务可以跨越当前已映射的整个操作，但 CPU 访问、MDL 释放以及会退休固定存储的完成操作，在整体刷新前均被禁止。Flush 必须匹配最初位置、MDL、方向及实际映射总长度。它只释放映射字节而不释放寄存器，因此保留令牌可用于下一次操作。部分刷新、混合 MDL 操作及其他 MapTransfer 模式处于此配置范围之外，不意味着所有 Windows 系统都会认定这些模式非法。

`KeFlushIoBuffers` 验证有效的已锁定／非分页 MDL。模型平台具有缓存一致性，因此 ReadOperation 和 DmaOperation 的任意取值均无需额外缓存副本；此调用不释放 DMA 所有权，也不替代 FlushAdapterBuffers。[通道场景](../examples/driver-dma-channel-scenario.json)运行原创 `driver_wdm_dma_channel.c`，执行两次 MapTransfer、一个跨页设备事务、独立声明的 IRQ/DPC、整体刷新及确切寄存器释放。真实普通／CFG 镜像使用 `NEVERD_WDM_DMA_CHANNEL_FIXTURE` 和 `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`。

`KernelPhysicalMemory` 为现有 RAM 分配最多 256 个、每页 4096 字节的模型物理页身份。CPU 虚拟地址、物理页身份和设备逻辑地址彼此区分。已构建 MDL 的 PFN 数组以只读方式暴露这些共享身份，未构建描述符没有可用 PFN。相邻小分配可以共享 PFN，但字节范围和生命周期仍独立。公共缓冲区、池和请求缓冲区使用 `GuestMemory` 已有的同一份字节，不增加 DMA 数据副本。有效 SG 映射固定确切数据范围及描述符；完成、池／MDL 释放和拆除在退休存储前拒绝尚存依赖。解除直接 MDL 映射只撤销 CPU 系统映射，DMA 仍可访问锁定的底层 RAM。`DmaWritable` 将写锁定契约与 CPU 映射权限分开记录：设备写入要求直接 READ／OUT_DIRECT 或可写非分页存储；WRITE／IN_DIRECT 不会因为 CPU 映射可写就取得该许可。

`GetScatterGatherList` 按 MDL 原始范围验证 CurrentVa／Length，并在现有底层 RAM 上生成逻辑页片段。映射寄存器可用时，真实的四参数 void `AdapterListControl` 在 API 返回前内嵌执行；否则接纳过程保留数据／描述符，并为 PDO 的 FIFO 预留回调，直到资源释放。此范围没有 StartIo 所有权，因此回调第二个 IRP 参数为 NULL。回调返回不会释放映射。`PutScatterGatherList` 可以在回调内执行；Put 后驱动可以完成请求并释放最后一个适配器，而回调续接和设备引用持续到返回。有效 SG 映射期间，CPU 必须先 Put 才能访问数据；仍在等待映射寄存器的回调尚未把字节交给设备独占。释放公共缓冲区必须匹配原适配器、长度、逻辑地址和 CPU 地址。逻辑地址在整个会话中永不复用，重启也不例外。缺少真实生产者的资源等待会明确停滞，不虚构完成或截止时间。

只有 READ／WRITE／IOCTL 请求接受 `dma_events`。每个事件必须提供 `after_100ns`、`device_id`、`logical_address`、`direction` 和 `length`；`write_memory` 还必须提供长度准确的 `data_hex`，`read_memory` 则拒绝该字段。方向以设备为视角。上限为每请求 64 个事件、合计 1024 个、事务字节总计 16 MiB、每事务 1 MiB；延迟范围为非负至 INT64_MAX。提交捕捉 PDO 当前已分配资源代次并确定虚拟时间起点，但不要求后续派发尚未创建的映射已经存在。递送时解析完整有效逻辑范围及方向，要求物理 D0，并在任何事务效果前验证全部底层字节。源 IRP 完成不会取消事件。映射缺失或已释放、陈旧资源代次、突然移除或 D3 都会记录失败并停止；不会重新绑定，也不虚构中断、寄存器协议或 IRP 完成。在同一调度边界，提供者先发布硬件状态，然后执行 DMA 字节访问，最后处理独立声明的中断脉冲。时间仍为协作式，不提供指令级抢占。

报告保留 `configuration.pnp_devices[].dma` 和平铺的 `configuration.dma_events`。根级 `dma_transfers` 行标明 `source_request_index`、`event_index`、`device_id`、`epoch`、`logical_address`、`direction`、`length` 和 `due_at_100ns`，并提供可空的 `occurred_at_100ns`、`completed_at_100ns`、`mapping`、`adapter`、`failure_reason`；`data_hex` 仅包含实际传输字节。所有声明的事务都必须无失败完成，`scenario_success` 才能成立。[DMA 场景](../examples/driver-dma-scenario.json)使用原创真实 WDK `driver_wdm_dma.c` 和可选 `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE`，通过真实适配器指针操作公共缓冲区，并以独立声明的 ISR→DPC 完成请求。C／Python 仍使用 `scenario_json`，不修改 `neverd_driver_options_v1`。缺少产物明确跳过，证据仅限 Linux。从属控制器、V2／V3 方法、硬件描述符引擎、通用 KMDF DMA 及其他设备模型仍不支持。

WDM remove-lock 使用真实导出 `IoInitializeRemoveLockEx`、`IoAcquireRemoveLockEx`、`IoReleaseRemoveLockEx` 和 `IoReleaseRemoveLockAndWaitEx`；不带 Ex 的 WDK 名称是宏。锁属于完整对齐存储所在扩展的确切 DEVICE_OBJECT，与 PDO 生命周期和 Tag 形状无关；附加前即可初始化。支持 retail 32 字节和 DBG 120 字节，必须传入匹配的独立大小参数，注册后整个区域不透明。NULL 和重复 Tag 按每把锁计数，Tag 从不解引用，因此 IRP 完成后仍可释放。初始化与 AndWait 要求 `PASSIVE_LEVEL`，acquire/release 允许 `DISPATCH_LEVEL`。

AndWait 关闭获取入口，释放一次匹配获取，并挂起真实来宾帧，直到其余获取全部释放；之后 acquire 返回 `STATUS_DELETE_PENDING` 且不产生释放义务。最后一次 release 在释放回调返回前锁存就绪，允许工作项 release 后等待 REMOVE 续接发出的事件。模型不会制造回调、超时或无生产者的成功。当前 AndWait 要求所有者位于关联的活动 REMOVE 路径，且提供者已实际接收（`bus_received_at_100ns` 可以为零），无需等下层完成。下层在到达提供者前排队 REMOVE 不在本范围内；此检查不是完整 OutsideRemoveDevice／Driver Verifier。REMOVE 前仍要求关闭文件并排空先前请求，但允许尚未结束的回调释放锁。保留路径覆盖等待、来宾拆链／删除和下层 pending；所有相关回调帧返回后才最终释放路径。

未知或大小不匹配的存储、不匹配 release、重复 drain、重初始化，以及仍有获取或未消费 drain 等待时删除扩展，均在修改前失败。干净的 AddDevice 失败可以删除已初始化但未使用的锁。锁不替代真实设备／工作项引用，只在扩展物理退休时注销；有效调试元数据不启用 Verifier 超时／高水位行为。真实 WDK `driver_wdm_remove_lock.c` 通过 `NEVERD_WDM_REMOVE_LOCK_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`／`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` 提供四种变体。缺少产物明确跳过；原生与 C API／CLI 证据仅来自 Linux，不代表完整移除管理或通用并发 I/O 排空。

WDM 电源请求使用 `kind: "power"` 和已配置的 `device_id`。每个包必须显式提供 `minor`（`query`／`set`）、`power_type`（`device`／`system`）、`power_state`（`D0`／`D3` 或 `working`／`sleeping3`）、`power_action`（`none`／`sleep`）、32 位整数或十六进制字符串 `system_context` 及 `bus_completion`。不支持 System Query 到 Working；拒绝文件、传输及取消字段。`system_context` 原样作为不透明事实保留，不用于推断父请求、休眠或快速启动。路径必须具有 `DO_POWER_PAGABLE` 且没有 `DO_POWER_INRUSH`；当前电源派发及 `PoRequestPowerIrp` 在 `PASSIVE_LEVEL` 执行。`PoCallDriver` 转发同一受管理的电源 IRP；`PoStartNextPowerIrp` 遵循 Vista+ 无额外串行化握手的契约。通用电源策略、WAIT_WAKE、其他状态／动作、关机／休眠、浪涌、不可分页路径、通用硬件及 通用 KMDF PnP 不受支持。

每个 `pnp_devices` 条目可提供 `initial_reported_device_power: "D0"` 或 `"D3"`，与必填的初始生命周期 D0／working 独立。PDO 与首次关联的每个来宾 DEVICE_OBJECT 各有独立通知状态；`PoSetPowerState` 只返回并更新调用设备的前值。缺少该事实时，实际调用会失败，不猜测 D0。可选 `requested_device_power` 保存相同六项必填事实的 device 类型模板；所有 PDO 合计最多 64 项。只有实际 `PoRequestPowerIrp` 的 PDO、minor 和目标匹配该 PDO 的 FIFO 队首时才消费；缺失／不匹配报错，未消费条目不生成请求，也不根据回调 context 猜父请求。子请求有独立 IRP 和报告行，`origin: "PoRequestPowerIrp"` 及从零开始的 `response_index`；场景行使用 `origin: "scenario"` 和空索引。同步子请求可在 API 返回 `STATUS_PENDING` 前执行五参数 void 回调；回调可等待，System S0 可先于独立 D0 子请求完成。回调的 IO_STATUS_BLOCK 快照一直有效到回调返回。

请求报告增加可空的 `power`，电源行的 `file` 为 null。`power` 记录包事实、`device_state_before`／`device_state_after`、`system_state_before`／`system_state_after`、可空的 `requested_device_object` 及实际 `bus_status`／`bus_received_at_100ns`／`bus_completed_at_100ns`。最终 PnP 设备增加 `device_power`／`system_power`；存活设备增加可空的 `reported_device_power`。`scenario_success` 只用场景来源行核对配置数量，但所有实际场景／子请求都必须成功完成；未消费模板不导致失败。真实 `driver_wdm_power.c` 使用 `NEVERD_WDM_POWER_FIXTURE`／`NEVERD_WDM_POWER_CFG_FIXTURE`，覆盖普通／active-CFG 原生与 C API／CLI 路径；缺少产物明确跳过，执行证据仅来自 Linux。

[完整电源场景](../examples/driver-power-scenario.json) 可通过 `--scenario` 运行真实样例，包含启动、系统查询／睡眠／唤醒、移除和三个显式子响应。

KMDF 1.33 支持使用精确的 1.33.0 ABI：458 个函数槽具有稳定的来宾身份，下列 63 个 API 实现了执行语义。`WdfVersionBind` 和 `WdfVersionUnbind` 在真实 WDK `FxDriverEntry` 包装函数前后管理来宾绑定。`WdfGetDriver` 读取公共驱动全局结构。非 PnP 驱动、通用对象、控制设备、队列和传入请求共享类型化上下文、引用计数，以及实际执行的清理／销毁／卸载回调。所有已建模的框架调用和回调目前都要求 `PASSIVE_LEVEL`；清理完成后新增引用仍不在此配置的支持范围内。未建模的函数槽、`WdfLdrQueryInterface`、类扩展和 UMDF 会明确停止。

控制设备要求复制可打印 ASCII 名称，并且 SDDL 必须精确为 `D:P(A;;GA;;;WD)`。这授予所有调用方访问权限，无需虚构调用方令牌；不支持其他安全描述符、未命名设备和自动名称。设备初始化拥有一个 WDM 设备。请求可通过现有会话命名空间中的符号链接别名 `\DosDevices\Name` 或 `\??\Name` 选择设备，报告仍保留规范设备名。创建成功会消耗初始化对象并清空其指针；失败则回滚部分设备所有权。`WdfControlFinishInitializing` 决定何时可以递送 I/O。仅在已建模的文件、工作项和请求允许时，删除操作才移除设备及其链接；不支持删除过程中取消或排空请求。

96 字节的 `WDF_IO_QUEUE_CONFIG` 支持默认及非默认的手动、顺序、有限或无限并行队列，要求显式被动执行且不使用框架同步。控制设备队列不参与电源管理。专用 READ／WRITE／IOCTL 回调优先于默认回调。已接受的队列请求即使同步完成也返回 `STATUS_PENDING`；void 回调的返回寄存器不会使请求完成。延迟完成使用现有调度器。没有处理函数时，请求以 `STATUS_INVALID_DEVICE_REQUEST` 完成；未启用零长度递送时，零长度 READ／WRITE 直接完成。默认文件包以成功状态和 Information=0 完成 CREATE／CLEANUP／CLOSE。非默认手动队列通过 `WdfRequestForwardToIoQueue` 接收请求，`WdfIoQueueRetrieveNextRequest` 按 FIFO 顺序取回。取回前取消的请求由框架从队列移除并以 `STATUS_CANCELLED` 完成。非默认自动队列通过自身回调递送转发请求；手动默认队列保留传入请求，直到驱动取回。`WdfIoQueueRetrieveNextRequest` 可从手动和顺序队列取回等待中的请求；并行队列返回 `STATUS_INVALID_DEVICE_STATE`。若自动队列没有匹配回调，则在递送槽可用时以 `STATUS_INVALID_DEVICE_REQUEST` 完成该请求。文件回调、PnP 设备和完整 PnP／电源仍不支持。 `WdfRequestRequeue` 将已取回的请求重新放到同一手动队列队首。 `NumberOfPresentedRequests` 限制并行队列已递送请求的数量；超出上限的请求等待已递送请求完成或取消。 默认顺序队列在已有请求递送给驱动时仍接受后续请求；这些请求按 FIFO 等待空槽，并可在递送前取消。 `WdfIoQueueStop` 暂停递送但继续接收请求；`WdfIoQueueStart` 恢复等待请求的递送，`WdfIoQueueGetState` 报告排队和已递送请求数。停止期间取请求返回 `STATUS_WDF_PAUSED`；停止完成回调会在所有已递送请求完成或离开队列后带着指定上下文执行；仍在排队的请求不会阻碍回调。前一个回调待执行时再次注册会被拒绝。

`WdfIoQueueReadyNotify` 为手动队列注册一个 `EvtIoQueueState` 回调。当排队请求数从零变为非零时，回调在 `PASSIVE_LEVEL` 接收 `(WDFQUEUE, WDFCONTEXT)`；即使驱动仍持有此前取出的请求也一样。已非空队列在注册时可以立即通知；停止的队列等到 `WdfIoQueueStart` 才通知。重复注册或在停止前注销返回 `STATUS_INVALID_DEVICE_REQUEST`；调用 `WdfIoQueueStop` 后传入 NULL 即可注销。

`WdfIoQueueFindRequest` 在手动队列中查找请求，但不转移处理权；查找成功会增加一次请求引用，驱动须用 `WdfObjectDereference` 释放。`WdfIoQueueRetrieveFoundRequest` 仅对仍在队列中的请求转移处理权；已被取消移除的请求返回 `STATUS_NOT_FOUND`。可选参数沿用 `WdfRequestGetParameters` 的结构布局。框架文件对象筛选仍不支持。

配置的 `EvtIoCanceledOnQueue` 仅接收先前交给驱动后又转发或重新入队的请求，或由调用方上下文回调显式入队的请求；参数为 `(WDFQUEUE, WDFREQUEST)`。从未交给驱动的排队请求由框架直接以 `STATUS_CANCELLED` 完成，不调用该回调。回调将请求所有权交还驱动；驱动必须在回调内或之后完成请求，不能再次入队。队列清除与状态完成会等待回调返回及该驱动拥有的请求完成。

`WdfIoQueueDrain` 拒绝新请求（`STATUS_INVALID_DEVICE_STATE`），继续递送已排队请求；排队数和驱动持有数归零后调用完成回调。转发到已耗尽队列返回 `STATUS_WDF_BUSY`；`WdfIoQueueStart` 恢复接收。

`WdfIoQueuePurge` 还会以 `STATUS_CANCELLED` 取消框架尚未递送的请求，并在原 IRP 上取消已标记为可取消的驱动持有请求。清理回调先于 IRP 释放执行；状态完成回调等待排队、驱动持有及取消回调结束。未标记可取消的请求仍由驱动完成。

`WdfIoQueueStopSynchronously`、`WdfIoQueueDrainSynchronously` 和 `WdfIoQueuePurgeSynchronously` 会挂起来宾的 `PASSIVE_LEVEL` 调用帧，直到相关请求退出。同步停止继续接收但暂停递送，并等待已递送请求；同步耗尽拒绝新请求、继续递送排队请求，并等待两类请求完成；同步清空取消排队及已标记可取消的请求，等待取消回调返回。没有完成来源的等待会报告停滞模型错误。

`WdfIoQueueStopAndPurge` 和 `WdfIoQueueStopAndPurgeSynchronously` 取消调用前已排队及已标记可取消的驱动持有请求，随后继续接收新请求，但直到 `WdfIoQueueStart` 才重新递送。异步状态回调和同步等待在原有请求及其取消回调结束后完成；调用后进入队列的新请求保持排队，不延迟完成通知。普通和同步停止也会从先前的耗尽或清空状态恢复接收。

请求参数使用 40 字节的 `WDF_REQUEST_PARAMETERS` 布局。输入／输出访问函数返回逻辑长度，保留缓冲区别名及现有直接 I/O 的 MDL 映射；直接 IOCTL 的输入仍使用缓冲区。方向错误或缓冲区不足返回文档规定的状态。完成操作先在缓冲区仍有效时执行请求清理，再完成 IRP 并释放请求所属锁页；引用允许时才销毁子对象和请求。一旦开始完成操作，就拒绝新的缓冲区与参数访问函数调用；已取得的缓冲区指针在清理期间仍可使用。外部对象引用保留上下文，但不保留对已完成 IRP 的访问权。

取消支持限于上述控制设备队列中的请求。若取消已经发生，`WdfRequestMarkCancelableEx` 返回 `STATUS_CANCELLED`，不会调用取消回调。`WdfRequestUnmarkCancelable` 成功后会移除回调；之后发生的取消只记录已取消状态，不再递送该回调。`WdfRequestIsCanceled` 可在未标记为可取消的存活请求上读取此状态。成功标记为可取消后，完成请求需要成功解除标记，或取消回调已经开始递送；仅仅排队还不允许完成。回调开始后可与工作项协调完成，包括回调正在等待的情况。独立的内部引用保留请求直到取消回调返回；完成操作仍先使 IRP 失效，最终的请求销毁续接本身也可等待。调度优先级依次为 DPC、按 FIFO 排列的取消回调、普通工作项；排队的取消回调也先于就绪的被动级等待帧恢复。

请求已取消时，旧版 void `WdfRequestMarkCancelable` 会在返回前同步执行来宾取消回调。该子续接可以等待，通过嵌套清理完成请求，并在原 API 恢复前执行最终销毁。注册后才发生的取消仍走上述调度路径。这复现了 `PASSIVE_LEVEL` 与 `WdfSynchronizationScopeNone` 下的公开源码行为；[Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) 要求不使用自动同步的驱动选择 Ex。这里执行旧版 API 是兼容行为，并非推荐在该配置下使用它。

`WdfRequestGetInformation` 与 `WdfRequestSetInformation` 共享原 IRP 中的 64 位 `IoStatus.Information`，与来宾直接写入保持一致。Set 只赋值，传输长度在完成时才验证。`WdfRequestCompleteWithInformation` 在清理前写入同一字段；清理回调通过事先保存的 IRP 修改此值后，最终 Information 使用修改后的值，即使此阶段 GetInformation 已返回零。`WdfRequestGetIoQueue` 在驱动持有请求时返回递送队列；手动取回后返回目标手动队列。请求停留在手动队列期间归框架所有，驱动不可访问。默认文件配置下，`WdfRequestGetFileObject` 返回 NULL，不会把 WDM FILE_OBJECT 伪装成 WDF 文件对象。`WdfRequestWdmGetIrp` 返回同一个 IRP；来宾不能通过 `IoCompleteRequest`／`IofCompleteRequest` 绕过 WDF 完成流程。完成过程中或完成后，只要请求句柄仍有效，GetInformation／GetIoQueue 返回零；MDL 获取先将有效输出槽清为 NULL，再返回 `STATUS_INTERNAL_ERROR`。此时 SetInformation／GetFileObject／WdmGetIrp 仍被拒绝，已有的缓冲区与参数访问限制不变。

`WdfRequestRetrieveInputWdmMdl` 与 `WdfRequestRetrieveOutputWdmMdl` 按需为缓冲 WRITE 输入、READ 输出及 IOCTL 输入／输出描述现有 SystemBuffer。每次获取都先验证方向有效且长度非零，再使用该请求唯一的缓存描述符；首次成功获取决定 ByteCount，即使另一方向的逻辑长度不同也保留此值。对此描述符，`MmGetSystemAddressForMdlSafe` 返回原 VA；额外映射、解除映射及驱动释放均被拒绝。直接 READ 输出、WRITE 输入及 IOCTL 输出返回现有 `IRP.MdlAddress`，获取本身不建立映射；直接 IOCTL 输入使用 SystemBuffer 缓存。描述符、IRP 和缓冲区在完成时一同失效。取消内部引用或外部引用只保留 WDF 上下文，不保留已完成的 I/O 存储。WDF 对 `METHOD_NEITHER` 的 MDL 获取函数仍不支持；请求所属的 WDFMEMORY 使用独立的锁页映射。已构建描述符的模型 PFN 数组可只读访问；未构建描述符的 PFN 访问被拒绝。

对缓冲、直接和 neither KMDF 请求，`WdfDeviceInitSetIoInCallerContextCallback` 在请求方进程的 `PASSIVE_LEVEL` 执行预队列回调。回调必须完成请求，或恰好调用一次 `WdfDeviceEnqueueRequest` 后进入默认队列。对于 `METHOD_NEITHER` IOCTL 和 neither READ／WRITE，`WdfRequestRetrieveUnsafeUserInputBuffer` 与 `WdfRequestRetrieveUnsafeUserOutputBuffer` 仅在该回调中返回原始用户地址。`WdfRequestProbeAndLockUserBufferForRead` 与 `WdfRequestProbeAndLockUserBufferForWrite` 检查页面权限并锁定请求所属的页面；`WdfMemoryGetBuffer` 返回系统别名，在离开请求方上下文后的队列回调中仍可使用。请求完成时释放锁页和别名。本模型仅支持场景原始缓冲区，不支持嵌入式用户指针或任意用户映射。

当前支持无资源 KMDF PnP 的直连 FDO/PDO 子集：`EvtDriverDeviceAdd` 接收框架持有的初始化器，`WdfDeviceCreate` 创建 FDO，`WdfFdoInitWdmGetPhysicalDevice` 与 `WdfDeviceWdmGetPhysicalDevice` 保留 PDO 身份。AddDevice 失败及完成 Remove 后，框架会运行清理／销毁回调并删除设备。PnP 队列必须显式设置 `PowerManaged = WdfFalse`；资源列表、D0 回调和通用电源策略尚未实现。真实 WDK 样例 `driver_kmdf_pnp.c` 可通过 `NEVERD_KMDF_PNP_FIXTURE`／`NEVERD_KMDF_PNP_CFG_FIXTURE` 验证普通及 active-CFG 映像。

已建模的 KMDF API: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetIoType`, `WdfDeviceInitSetIoInCallerContextCallback`, `WdfFdoInitWdmGetPhysicalDevice`, `WdfDeviceCreate`, `WdfDeviceEnqueueRequest`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfDeviceWdmGetPhysicalDevice`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfIoQueueGetDevice`, `WdfIoQueueGetState`, `WdfIoQueueStop`, `WdfIoQueueStopSynchronously`, `WdfIoQueueStart`, `WdfIoQueueDrain`, `WdfIoQueueDrainSynchronously`, `WdfIoQueuePurge`, `WdfIoQueuePurgeSynchronously`, `WdfIoQueueStopAndPurge`, `WdfIoQueueStopAndPurgeSynchronously`, `WdfIoQueueReadyNotify`, `WdfIoQueueRetrieveNextRequest`, `WdfIoQueueFindRequest`, `WdfIoQueueRetrieveFoundRequest`, `WdfRequestForwardToIoQueue`, `WdfRequestRequeue`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveUnsafeUserInputBuffer`, `WdfRequestRetrieveUnsafeUserOutputBuffer`, `WdfRequestProbeAndLockUserBufferForRead`, `WdfRequestProbeAndLockUserBufferForWrite`, `WdfMemoryGetBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

可选的真实 WDK 验证以真正的 KMDF 入口库分别编译 `driver_kmdf_lifecycle.c` 和 `driver_kmdf_control.c`。`NEVERD_KMDF_FIXTURE`／`NEVERD_KMDF_CFG_FIXTURE` 选择生命周期映像；`NEVERD_KMDF_CONTROL_FIXTURE`／`NEVERD_KMDF_CONTROL_CFG_FIXTURE` 选择普通／启用 CFG 的控制设备映像。缺少外部产物时会明确跳过。原生及 C API／CLI 覆盖见[测试指南](testing.md)。当前执行证据仅来自 Linux 主机。

初始 API 模型刻意采用有限契约：

| API | 建模行为与限制 |
|-----|----------------|
| `RtlInitUnicodeString` | 根据有界、以 NUL 结尾的源字符串构造来宾 `UNICODE_STRING` |
| `RtlCopyUnicodeString`、`RtlCompareUnicodeString`、`RtlEqualUnicodeString` | 带长度的 UTF-16 复制及区分大小写比较；不区分大小写的比较需要 Windows 大小写表，因此会停止 |
| `ExRaiseStatus`, `ExRaiseAccessViolation`, `ExRaiseDatatypeMisalignment` | 为支持的常量 C `__except` 处理器抛出来宾异常；没有正常 API 返回，过滤器／finally 和 CPU 故障恢复仍不支持 |
| `ExAllocatePool2` | 分页／非分页 NX 分配，默认清零；支持未初始化与缓存行对齐标志；无效的必需标志返回 NULL，配额／可执行池以及分配失败引发异常的路径会停止 |
| `MmGetSystemRoutineAddress` | 通过共享导出清单解析带长度的来宾名称 |
| `MmMapIoSpace`, `MmMapIoSpaceEx`, `MmUnmapIoSpace` | 声明的转换后子区间；非缓存 RO／RW、共享别名、精确取消映射；不提供任意物理内存 |
| `IoConnectInterrupt`, `IoDisconnectInterrupt`, `IoConnectInterruptEx`, `IoDisconnectInterruptEx` | 精确分配的独占 latched 线路，PASSIVE_LEVEL 下的传统 ABI 及 Ex 版本 1／2／4；不透明连接和精确资源代次生命周期 |
| `KeSynchronizeExecution`, `KeAcquireInterruptSpinLock`, `KeReleaseInterruptSpinLock` | 真实 BOOLEAN 同步回调，分配 DIRQL 下的同一非递归锁；恢复原调用方 IRQL 和所有权 |
| `IoGetDmaAdapter` | PASSIVE_LEVEL 下显式 Internal 总线主设备的版本 0／1 描述和绑定的 V1 操作表；新版探测返回 NULL |
| `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `PutDmaAdapter` | 操作共享一致性 RAM 的适配器表方法，精确分配身份及独立适配器生命周期 |
| `GetScatterGatherList`, `PutScatterGatherList` | DISPATCH_LEVEL 下的适配器表方法；真实内嵌或资源排队回调、固定 MDL 视图和显式映射释放 |
| `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers`, `FreeMapRegisters` | 经过地址转换的总线主设备通道回调、共享寄存器额度、连续 MDL 片段、整体刷新及确切保留寄存器释放 |
| `KeFlushIoBuffers` | 针对有效已锁定／非分页 MDL 的一致性 CPU 缓存刷新，不释放 DMA 映射 |
| `MmMapLockedPagesSpecifyCache`、`MmGetSystemAddressForMdlSafe`、`MmUnmapLockedPages` | 请求拥有的 MDL 支持 KernelMode 缓存映射及权限；非分页池 MDL 通过安全辅助函数复用原始池映射 |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | 独立描述符，完整范围须位于同一个有效非分页池分配内；描述符与缓冲区具有独立生命周期，不支持 IRP 关联、MDL 链或配额 |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | 显式配置的会话注册表、逐句柄权限与生命周期、查询缓冲区大小及修改；不访问宿主注册表 |
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
| `KfRaiseIrql`, `KeLowerIrql` | 真实的 x64 WDK IRQL 升降导入；保存的 IRQL 必须由同一执行在返回或挂起前按 LIFO 顺序恢复。CR8 读取会反映每次变化；不模拟指令级中断抢占。 |
| `KeInitializeSpinLock`, `KeAcquireSpinLockRaiseToDpc`, `KeReleaseSpinLock`, `KeAcquireSpinLockAtDpcLevel`, `KeReleaseSpinLockFromDpcLevel`, `KeTryToAcquireSpinLockAtDpcLevel` | CPU0 上驻留且对齐的执行自旋锁；检查所有者、获取与释放配对和 IRQL 恢复。竞争的阻塞获取会显式停止。 |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | 设备拥有的不透明工作项；仅支持 `DelayedWorkQueue`，在 `PASSIVE_LEVEL` 将设备和上下文传给回调；禁止释放仍在队列中的项 |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | 不透明 DPC 存储、四个来宾回调参数、`DISPATCH_LEVEL`、重复入队／移除及优先级；仅目标 CPU0 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | 通知／同步定时器；相对／绝对 100 ns 期限、毫秒周期、重新设置／取消及虚拟时间中的信号查询 |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | 通知／同步事件保留不同信号消耗行为；`KeSetEvent` 仅接受 Increment=0、Wait=FALSE |
| `KeInitializeSemaphore`, `KeReleaseSemaphore`, `KeReadStateSemaphore` | 驻留的计数信号量，上限必须为正；每次成功等待消耗一个计数。释放仅接受 Increment=0、Wait=FALSE；超过上限时抛出 `STATUS_SEMAPHORE_LIMIT_EXCEEDED`。 |
| `KeInitializeMutex`, `KeReleaseMutex`, `KeReadStateMutex` | 驻留的 KMUTEX 按执行帧持有并支持递归获取；KeReleaseMutex 返回先前的有符号信号状态，要求持有者和匹配的 DISPATCH_LEVEL 获取上下文，且仅接受 Wait=FALSE。持有期间禁止返回、重新初始化或释放存储。 非持有者释放触发 `STATUS_MUTANT_NOT_OWNED`。 |
| `PsCreateSystemThread`, `PsTerminateSystemThread`, `ObReferenceObjectByHandle`, `ObfDereferenceObject`, `ZwClose` | 系统进程中的有界线程在 PASSIVE_LEVEL 运行。句柄与不透明线程对象的引用各自持有生命周期；PsTerminateSystemThread 不返回客体代码，并使线程对象可等待且进入信号状态。APC、线程优先级和带类型的对象引用尚未建模。 |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeEnterGuardedRegion`, `KeLeaveGuardedRegion`, `KeAreApcsDisabled`, `KeAreAllApcsDisabled` | 按线程跟踪可嵌套的 APC 禁用状态。临界区及持有的 KMUTEX 禁用普通内核 APC；保护区和 IRQL >= APC_LEVEL 禁用全部 APC。系统线程启动时处于一层临界区内。未匹配的离开和带未平衡状态返回都会失败；尚未实现 APC 投递。 |
| `KeWaitForSingleObject` | 单个已初始化的事件、定时器、信号量或互斥体；非警报 `KernelMode`、原因 `Executive`；零超时轮询、有限相对／绝对或无限等待；非零／无限等待要求 IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | IRQL <= APC_LEVEL 的非警报 `KernelMode` 相对／绝对延迟；虚拟时间推进后恢复保存的来宾执行帧 |
| `IoMarkIrpPending` | 标记当前存活的 IRP；也支持 WDM 宏对栈控制字段的等效写入；派发必须返回 `STATUS_PENDING` |
| `IofCompleteRequest`、`IoCompleteRequest` | 使用 `IO_NO_INCREMENT` 执行完成展开，支持暂停／继续；仅在最终展开边界释放 IRP、MDL 和缓冲区 |
| `memcpy`、`memmove`、`memset`、`memcmp`、`RtlCopyMemory`、`RtlMoveMemory`、`RtlFillMemory`、`RtlZeroMemory`、`RtlCompareMemory` | 有界的来宾缓冲区操作，每次调用最多 1 MiB；要求不重叠的复制 API 会拒绝重叠 |

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

可选布尔字段 `user_unmap_after_dispatch` 适用于非空的 WDM neither 传输：派发返回后、排队工作或取消执行前撤销原用户虚拟地址的访问权限。已锁定的 MDL 页面及其系统别名仍可使用，直到驱动解锁；原始用户指针和新的锁页操作会失败。若输出用户地址已撤销，`output_hex` 为空。此有界场景不会复用地址，也不模拟重新映射或任意时刻解除映射。

`requestor_process_id` 指定合成请求进程 ID（默认 4096，范围 5 到 `UINT32_MAX`）。`IoGetRequestorProcessId` 返回活动 IRP 的请求进程 ID；`PsGetCurrentProcessId` 在前台派发时返回该 ID，在建模的系统工作项中返回 4。切换进程后，其他进程的原始用户 VA 不可访问，但已锁定 MDL 的系统别名仍有效。`requestor_exit_after_dispatch` 在派发返回后撤销该进程所有原始用户 VA，并拒绝来自该 ID 的新 I/O；显式 CLEANUP/CLOSE 仍可执行，不自动推断取消或句柄清理。

系统工作项可通过 `IoGetRequestorProcess` 获取活动 IRP 的不透明请求进程对象，用 `KeStackAttachProcess` 和可写的内核 `KAPC_STATE` 临时附加，在原始用户 VA 上操作，再以同一状态调用 `KeUnstackDetachProcess`。`IoGetCurrentProcess` 与 `PsGetProcessId` 反映附加进程；`PsGetCurrentProcessId` 仍返回创建工作线程的系统进程 ID 4。进程退出、IRP 结束、错误配对、附加期间等待或完成 IRP 均明确报错。

对于 direct IOCTL，`input` 初始化第一个系统缓冲区，`direct_input` 初始化由 MDL 描述的独立第二缓冲区，并补零至 `output_size`。`METHOD_IN_DIRECT` 要求可读访问，但不意味着系统映射为只读。两种方法都使用可读写的场景缓冲区。`MdlMappingNoWrite` 移除映射的写权限，`MdlMappingNoExecute` 移除执行权限。解除映射会撤销系统虚拟地址；重新映射保留同一份锁定数据。请求完成时 MDL 和映射均失效。模型支持 WDM 宏使用的公共 MDL 字段；进程字段、未构建描述符的 PFN 访问、手工构造的 MDL、用户映射以及通过原始 UserBuffer 直接访问都会被拒绝；已构建描述符的 PFN 数组只读。零长度 direct 缓冲区的 MDL 为空。

`IoAllocateMdl` 为非空、不溢出且不超过 1 MiB 的缓冲区分配独立元数据，不探测或锁定缓冲区。`Irp` 必须为 NULL，`SecondaryBuffer` 和 `ChargeQuota` 必须为 FALSE；对象区耗尽时返回 NULL。`MmBuildMdlForNonPagedPool` 要求完整描述范围位于同一个有效非分页池分配内。安全辅助函数及常规 WDM 宏复用原始地址，保留别名关系与既有权限，即使再次传入禁止写入／执行标志也不改变权限。额外系统映射和解除映射会被拒绝。`IoFreeMdl` 仅使描述符失效，池缓冲区有独立生命周期；只要不再使用已释放的存储，两种释放顺序均受支持。所有建模 MDL 字段及已构建描述符的 PFN 数组均为只读；进程字段、未构建描述符的 PFN、描述符链和手工修改字段仍不受支持。卸载前必须释放所有驱动拥有的描述符。

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

`configuration` 对象记录本次执行的限制、服务名及 `kernel_exports` 覆盖配置。配置标识为 `wdm-x64-scheduled-v49`。`nt_status` 始终是 DriverEntry 的结果，而 `scenario_success` 综合描述初始化及已完成请求的结果。`phase`、`requests` 和 `unload_completed` 表明请求生命周期的哪些部分已运行。每次 API 调用及 CPU 写入也会记录阶段（`driver_entry`、`add_device:<ID>`、`request:N`, `callback:N` 或 `unload`）。每个请求报告派发状态与 I/O 状态、是否完成、information 长度以及返回的 `output_hex` 字节。`preferred_image_base` 描述原始 PE 基址。`security_cookie` 为已初始化 cookie 的来宾地址；若无需 cookie，则为 `"0x0"`。请求报告字段为 `kind`、`device`、`device_id`、`pnp`、`file`, `requestor_process_id`、`byte_offset`、`code`、`irp`、`completed`、`cancel_requested_at_100ns`、`dispatch_status`、`io_status`、`information`、`information_hex` 和 `output_hex`。 `configuration.registry` 保留原始注册表配置。 `information_hex` 以十六进制字符串精确保留原始 64 位 `IoStatus.Information`；原有数值字段 `information` 仍保留。

工作项观察记录使用 `callback:N` 阶段。待处理请求的 `dispatch_status` 保留 `STATUS_PENDING`，最终完成状态单独记录在 `io_status`，并据此计算该请求对 `scenario_success` 的影响。

`ExRaiseStatus` 将 NTSTATUS 低 32 位传给来宾异常处理器；`ExRaiseAccessViolation` 和 `ExRaiseDatatypeMisalignment` 分别抛出 `STATUS_ACCESS_VIOLATION` 和 `STATUS_DATATYPE_MISALIGNMENT`。此配置遵循各自的 Microsoft DDI 文档：ExRaiseStatus 允许 `APC_LEVEL`，两个无参例程要求 `PASSIVE_LEVEL`。部分 WDK SAL 注解允许这两个包装函数在 APC_LEVEL 运行；此配置保留文档规定的较严格上限。抛出异常的调用保持 `result: null`，在 `detail` 记录异常码，不会报告 API 成功返回。

异常递送使用镜像已解码的 x64 版本 1 展开表及 `__C_specific_handler` 的常量 `EXCEPTION_EXECUTE_HANDLER` 作用域。它执行真实来宾处理器代码，支持跨普通辅助函数栈帧展开、恢复保存的非易失通用寄存器，并保留当前执行的栈边界。`GetExceptionCode()` 取得抛出的异常码。处理器可向受支持的外层作用域再次抛出异常。路径中遇到过滤器函数、`__finally`、GS／C++ 异常处理例程、链式或不完整元数据、序言展开或 XMM 恢复操作时，均明确失败。未捕获的 API 异常以 `model_error` 停止；CPU 访存／中断／无效指令故障仍会终止执行。

原创 `driver_wdm_seh.c` 测试驱动使用真实 WDK 头文件及 `/GS-`。通过 `NEVERD_WDM_SEH_FIXTURE` 和 `NEVERD_WDM_SEH_CFG_FIXTURE` 配置普通及活动 CFG 镜像。[driver-seh-scenario.json](../examples/driver-seh-scenario.json) 示例重定位镜像，在 DriverEntry 中捕获 API 异常后卸载。 独立的 WDM METHOD_NEITHER 路径现已支持用户内存探测、MDL 锁页和可捕获的内存故障。

可为空的 `fault` 对象保留后端首次故障。其 `kind`、`pc`、可为空的 `address`、`size`、`access` 和 `interrupt` 区分未映射或受保护内存、无效范围、无效指令及 CPU 异常。地址使用十六进制字符串；大小和中断向量使用整数。用于观察的读取不能替换原始故障。发生故障的后端不能恢复执行，此记录也不会使这些后端故障能够由来宾 SEH 处理。

`instructions` 统计执行策略已准许的来宾指令尝试次数。被执行策略拒绝的指令不计数；已准许但在 CPU 中发生故障的指令计数。合成的 API 派发与返回哨兵不增加该计数。

每个 `writes` 条目都带有 `semantics: "attempted_guest_write"`：记录栈外的 CPU 写入尝试，包括随后可能发生故障或被预算停止的尝试。它不保证写入已完成，也不包含 API 模型所做的写入。设备与驱动对象快照描述执行停止时观察到的状态。

包含 `neverd/sdk/NeverDCAPIEmulation.h`（或 C API 总头文件），创建会话，然后调用 `neverd_emulate_driver_json(session, path, options)`。显式传入非空路径会直接进入严格执行预检，无需先通过通用分析 API 加载。CLI 采用此路径。传入 `NULL` options 使用默认值。显式 `neverd_driver_options_v1` 选项要求 `struct_size` 精确匹配，且指令、内存、事件和超时预算均为正数。结果须用 `neverd_free_string` 释放。

若路径传入 `NULL`，则要求会话已加载文件，并独立于 IR 分析和函数受限加载重新解析该文件。两种方式均保留会话映像不变。调用期间必须保持输入文件可用且不变。请求／执行环境建立失败时返回 `NULL` 并设置 `neverd_last_error`；执行停止则返回 JSON。未启用该功能的构建仍提供此 API，并报告启用方法。

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)` 使用相同的 v1 选项与所有权规则，另外接受严格验证的场景输入。必须传入非 NULL、以 NUL 结尾的 JSON 字符串。原有 `neverd_emulate_driver_json` ABI 保持不变，仍仅执行初始化。C++ 解析器 `driverOptionsFromScenarioJSON` 为 `emulateDriver` 的调用方提供相同的场景验证。

内部 C++ 入口为 `include/neverd/emulation/DriverSession.h` 中的 `neverd::emulation::emulateDriver`。格式解析由现有加载器负责；Windows 对象／API 行为由 `lib/emulation/windows` 负责；CPU 状态及执行由 Unicorn 适配器负责。适配器与模型使用相同的来宾内存接口。Windows API 行为不应放入 Unicorn fork。

WDM `METHOD_NEITHER` 的 `Type3InputBuffer` 与 `IRP.UserBuffer` 分别指向独立的用户内存。`ProbeForRead` 只检查范围和对齐，不触碰页面；`ProbeForWrite` 会触碰每一页。`ExGetPreviousMode` 返回请求模式。`MmProbeAndLockPages` 锁定单个用户分配的页面，`MmGetSystemAddressForMdlSafe` 建立共享内核别名，`MmUnlockPages` 撤销别名并解锁。不支持任意进程地址空间。

非空 WDM `METHOD_NEITHER` IOCTL 请求可以分别将 `user_input_access` 和 `user_output_access` 设为 `read_write`（默认）、`read_only` 或 `no_access`。Neither WRITE 仅接受输入权限，neither READ 仅接受输出权限；缓冲／直接传输及空缓冲区拒绝这些字段。`no_access` 保留非空指针，但禁止访问对应页面。
报告的 `configuration.user_page_access` 仅列出显式设置的权限，并用从零开始的 `source_request_index` 标识请求；未设置的方向默认为 `read_write`。

## 有界并发 WDM 请求

WDM READ/WRITE/IOCTL 请求或无限并行 KMDF 默认队列中的请求可设 `defer_callback_drain: true`。只有派发返回 `STATUS_PENDING` 且 IRP 仍待完成时，执行器才会先提交下一条请求；下一条未设置该字段的请求之后会排空回调并完成整批请求。重叠请求可使用不同的文件对象，或使用显式异步打开的同一文件对象；最后一条请求若设置该字段，场景末尾会排空回调。同步文件对象上的重叠请求、任意线程抢占和外部请求到达仍不支持。

## 异步文件对象

仅 CREATE 请求可设置布尔字段 `asynchronous_file: true`；省略或 false 仍为同步打开。异步打开会清除来宾 `FILE_OBJECT` 的 `FO_SYNCHRONOUS_IO`，后续文件 IRP 也不设置 `IRP_SYNCHRONOUS_API`。只有显式延迟回调排空时，同一异步文件的 READ/WRITE/IOCTL 才能重叠；CLEANUP/CLOSE 仍须等待全部先前传输完成并最终化。异步文件不维护隐式文件位置，`byte_offset` 仍逐请求指定，省略时为零。非 CREATE 请求即使传入 false 也拒绝此字段。
