**语言**: [English](../driver-emulation.md) | [简体中文](driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← 文档索引](README.md)

# Windows 驱动模拟

NeverD 的可选驱动模拟器执行受支持的 x64 WDM 驱动的 PE 入口点，并可在卸载前执行显式指定的同步请求场景。它使用 Unicorn 执行 CPU 指令，使用 NeverD 自有的有界 Windows 环境模型。它不会将驱动加载到宿主内核，也不会把来宾 API 调用转发给宿主操作系统服务。

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

兼容性由实际执行的代码路径及其依赖决定，而非由 `.sys` 扩展名决定。目前的验收证据涵盖原创的独立测试样例，以及 Microsoft SIOCTL WDM 示例的缓冲 I/O 路径；这并不代表与任意第三方驱动兼容。

| 驱动类别或要求 | 当前范围 | 缺少的环境 |
|----------------|----------|------------|
| 使用下列 API 的 x64 软件 WDM 驱动 | 有界初始化与一个同步文件生命周期 | 每个额外执行到的 API 都必须具有明确的模型 |
| `METHOD_BUFFERED` IOCTL | 支持显式请求场景 | 尚不支持同时打开多个文件以及异步完成 |
| `METHOD_IN_DIRECT`、`METHOD_OUT_DIRECT`、`METHOD_NEITHER` | 拒绝 | MDL、锁定页面、访问探测以及用户缓冲区生命周期 |
| KMDF / UMDF 驱动 | 不支持 | 框架绑定、对象、队列、回调及相应宿主运行时 |
| PnP 总线／功能／过滤驱动 | 初始化可在 API 子集内运行；不支持设备栈生命周期 | 设备附加、向下层驱动派发、PnP 和电源 IRP |
| 存储、网络、显示、文件系统及微过滤驱动 | 不支持相关子系统契约 | 端口／类／微端口框架、NDIS/WFP、图形或文件系统服务 |
| 使用工作线程、定时器、DPC、APC、等待或取消的驱动 | 不支持 | 调度、IRQL 转换、同步与异步所有权 |
| 使用进程／线程回调、句柄、注册表／文件操作或内核模块发现的驱动 | 下列 API 以外的行为不支持 | 对象管理器、系统状态以及回调／事件产生机制 |
| 硬件、DMA、PCI、中断或虚拟化驱动 | 不支持所需环境 | 设备模型、物理内存、总线、中断及特权 CPU 状态 |
| x86 或 ARM64 Windows 驱动 | 拒绝 | 相应架构的加载、ABI 及执行模型 |
| 需要 CFG、不支持的加载配置、TLS 或其他被拒绝的 PE 特性的 x64 映像 | 加载时拒绝 | 针对这些要求的明确加载器／运行时语义 |

未使用的不支持导入项可以保持绑定。一旦执行到不支持的操作，便会停止并给出诊断及此前收集的观察结果。仅 DriverEntry 成功，并不能证明后续派发、硬件或框架路径也受支持。下方 API 表是受支持子集的权威定义。

## 执行契约

该配置在 `PASSIVE_LEVEL` 下模拟单线程的 x64 WDM 生命周期。执行从 PE 入口点开始；若存在编译器生成的入口包装函数，也会保留并执行。DriverEntry 必须返回 `STATUS_SUCCESS` 才能完成初始化；非零的成功状态或待处理状态会因初始化契约不受支持而停止。失败状态则作为已完成的初始化结果保留。所有对象、字符串、栈、函数指针与分配均位于来宾内存。模型根据配置的服务名（默认为 `NeverDDriver`）提供 `DRIVER_OBJECT` 和注册表路径。

适配器使用 Unicorn 的虚拟 TLB 模式保留来宾虚拟地址，包括规范的高位内核地址，无需合成 Windows 页表。初始 RFLAGS 为 `0x202`；软件设备配置采用固定的 64 字节缓存行。这些都是本执行场景的显式属性。

未知导入项绑定到延迟陷阱。未使用的导入项不会阻止执行；执行其 thunk 或读取未建模的导出数据值时，会以 `unsupported_api` 停止。不支持的 CPU 环境效果也会明确停止。NeverD 不会用成功返回值替代未实现的调用。格式错误的映像或不支持的加载要求会在执行前失败。

此配置没有实现完整的 Windows 内核、KMDF 运行时、PnP／电源生命周期、异步或待处理 IRP、直接／neither 方法 IOCTL、中断或多线程调度。只有场景显式请求时才执行回调；仅初始化模式仍在 DriverEntry 之后停止。

映像默认使用首选基址，除非场景选择了有效的重定位地址。映像必须为使用 native 子系统的 PE32+ x64 可执行文件。导入可来自 `ntoskrnl.exe` 或 `ntkrnlmp.exe`。

执行加载器支持经验证的 x64 `DIR64` 基址重定位，以及有限的安全 cookie 加载配置；它会在入口包装函数执行前设置确定性的来宾 cookie。CFG、其他未建模的加载配置字段、TLS、延迟／绑定导入、按序号导入以及托管映像都会被拒绝。映像还必须通过严格的范围与对齐检查。

初始 API 模型刻意采用有限契约：

| API | 建模行为与限制 |
|-----|----------------|
| `RtlInitUnicodeString` | 根据有界、以 NUL 结尾的源字符串构造来宾 `UNICODE_STRING` |
| `ExAllocatePoolWithTag`、`ExFreePoolWithTag`、`ExFreePool` | 对池类型 `0`、`1`、`512` 提供数据分配；大小／标签必须为正，带标签释放必须匹配，地址不复用 |
| `IoCreateDevice`、`IoDeleteDevice` | 设备类型为 `0x22`，characteristics 为 `0` 或 `0x100`，扩展大小有界，名称为 ASCII `\Device\Name` |
| `IoCreateSymbolicLink`、`IoDeleteSymbolicLink` | 一个会话命名空间内的 ASCII `\DosDevices\Name` 或 `\??\Name`，目标为 `\Device\Name` |
| `DbgPrint`、`DbgPrintEx` | ASCII 字面文本与 `%%`，最多输出 512 字节；可变参数格式化会停止；启用所有调试器过滤器 |
| `IoGetCurrentIrpStackLocation` | 返回当前建模 IRP 的栈位置；正常编译的 WDM 宏读取相同来宾字段 |
| `KeGetCurrentIrql` | 返回 `PASSIVE_LEVEL` |
| `IofCompleteRequest`、`IoCompleteRequest` | 以 `IO_NO_INCREMENT` 完成当前同步建模 IRP；已完成的 IRP 或缓冲区不能再次访问 |
| `memcpy`、`memmove`、`memset`、`memcmp`、`RtlCopyMemory`、`RtlMoveMemory`、`RtlFillMemory`、`RtlZeroMemory`、`RtlCompareMemory` | 有界的来宾缓冲区操作，每次调用最多 1 MiB；要求不重叠的复制 API 会拒绝重叠 |

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

设备名称与 IOCTL 代码必须匹配驱动。省略 `device` 时选择唯一的存活设备；无法唯一选择则失败。本模型跟踪一个打开的文件，并要求按 create、IOCTL、cleanup、close 的顺序执行。仅支持 `METHOD_BUFFERED` IOCTL。派发必须同步完成每个 IRP；返回 `STATUS_PENDING`、未完成请求、输出长度无效以及访问已完成的 IRP 都会明确失败。请求卸载后，不得留下存活的设备、符号链接、池分配或文件对象。

可选根字段 `"load_address": "0x190000000"` 请求更改加载基址；省略该字段或指定 `"0x0"` 时使用首选地址。映像必须满足重定位要求。原有的初始化命令或 C API 不会隐式执行任何请求场景。

根对象仅接受 `load_address`、`requests` 和 `unload`。请求字段为 `kind`、可选的 `device`，以及仅用于 `ioctl` 的必填 `code` 和可选 `input`、`output_size`。未知或重复字段会被拒绝。`code` 接受无符号 32 位 JSON 整数或 `0x` 十六进制字符串。`input` 是长度为偶数且不带前缀或空格的十六进制字节字符串；省略表示空输入。`output_size` 为无符号 JSON 整数，省略表示零。不接受小数以及浮点数写法。

场景文本上限为 2 MiB，最多包含 64 个请求，每个输入或输出缓冲区最多 65536 字节，输入加输出的总字节数最多 512 KiB。指令、观察事件、来宾内存和时间预算覆盖整个场景。1 MiB 区域还需存放对象与元数据，因此即使尚未用尽场景缓冲区总额度，也可能耗尽模型内存。

## Microsoft 示例验收检查

可选的[验证脚本](../../scripts/validate_windows_driver_sample.py)下载[验证清单](../../unittests/emulation/fixtures/sioctl-validation.json)中固定版本的 Microsoft SIOCTL 源码，验证 SHA-256 哈希，并使用 MinGW-w64 DDK 头文件编译未修改的源码。脚本在选定的输出目录中保留上游许可证／来源信息、构建命令、场景与报告。它需要网络访问、Clang、`lld-link`、`nm` 以及 MinGW-w64 的 DDK 头文件：

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

非默认 MinGW-w64 包含目录可通过 `--headers` 指定。脚本根据已编译对象的依赖生成 MS COFF 导入库。检查依次运行 DriverEntry、create、示例的缓冲 IOCTL、cleanup、close 和 unload。上游示例没有注册 cleanup 处理函数，因此模型默认处理函数以 `STATUS_INVALID_DEVICE_REQUEST`（`0xC0000010`）完成 cleanup。驱动仍会关闭并卸载，成功的 IOCTL 则返回预期字节。对于这个完整场景，CLI 的预期退出码为 **2**，`scenario_success` 为 false。脚本仅在所有结果均符合预期时成功，包括可见的 cleanup 失败；它不会改写示例来隐藏该结果。

## 报告与 SDK

JSON 报告区分 `stop_reason`、可为空的 `nt_status` 和 `nt_success`、停止位置 PC，以及指令计数。它保留停止前收集的 API 调用和可观察状态，包括设备对象与驱动回调地址。来宾地址以十六进制字符串表示，避免 JSON 使用方丢失 64 位精度。

`configuration` 对象记录本次执行的限制与服务名。配置标识为 `wdm-x64-synchronous-v1`。`nt_status` 始终是 DriverEntry 的结果，而 `scenario_success` 综合描述初始化及已完成请求的结果。`phase`、`requests` 和 `unload_completed` 表明请求生命周期的哪些部分已运行。每次 API 调用及 CPU 写入也会记录阶段（`driver_entry`、`request:N` 或 `unload`）。每个请求报告派发状态与 I/O 状态、是否完成、information 长度以及返回的 `output_hex` 字节。`preferred_image_base` 描述原始 PE 基址。`security_cookie` 为已初始化 cookie 的来宾地址；若无需 cookie，则为 `"0x0"`。请求报告字段为 `kind`、`device`、`code`、`irp`、`completed`、`dispatch_status`、`io_status`、`information` 和 `output_hex`。

`instructions` 统计执行策略已准许的来宾指令尝试次数。被执行策略拒绝的指令不计数；已准许但在 CPU 中发生故障的指令计数。合成的 API 派发与返回哨兵不增加该计数。

每个 `writes` 条目都带有 `semantics: "attempted_guest_write"`：记录栈外的 CPU 写入尝试，包括随后可能发生故障或被预算停止的尝试。它不保证写入已完成，也不包含 API 模型所做的写入。设备与驱动对象快照描述执行停止时观察到的状态。

包含 `neverd/sdk/NeverDCAPIEmulation.h`（或 C API 总头文件），创建会话，然后调用 `neverd_emulate_driver_json(session, path, options)`。显式传入非空路径会直接进入严格执行预检，无需先通过通用分析 API 加载。CLI 采用此路径。传入 `NULL` options 使用默认值。显式 `neverd_driver_options_v1` 选项要求 `struct_size` 精确匹配，且指令、内存、事件和超时预算均为正数。结果须用 `neverd_free_string` 释放。

若路径传入 `NULL`，则要求会话已加载文件，并独立于 IR 分析和函数受限加载重新解析该文件。两种方式均保留会话映像不变。调用期间必须保持输入文件可用且不变。请求／执行环境建立失败时返回 `NULL` 并设置 `neverd_last_error`；执行停止则返回 JSON。未启用该功能的构建仍提供此 API，并报告启用方法。

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)` 使用相同的 v1 选项与所有权规则，另外接受严格验证的场景输入。必须传入非 NULL、以 NUL 结尾的 JSON 字符串。原有 `neverd_emulate_driver_json` ABI 保持不变，仍仅执行初始化。C++ 解析器 `driverOptionsFromScenarioJSON` 为 `emulateDriver` 的调用方提供相同的场景验证。

内部 C++ 入口为 `include/neverd/emulation/DriverSession.h` 中的 `neverd::emulation::emulateDriver`。格式解析由现有加载器负责；Windows 对象／API 行为由 `lib/emulation/windows` 负责；CPU 状态及执行由 Unicorn 适配器负责。适配器与模型使用相同的来宾内存接口。Windows API 行为不应放入 Unicorn fork。
