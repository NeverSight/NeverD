**语言**: [English](../testing.md) | [简体中文](testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](../ja/testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](../de/testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← 文档索引](README.md)

# 测试 NeverD

NeverD 的测试回答三个不同问题：表示形状是否符合预期、完整 pipeline 路径能否
处理二进制 fixture，以及生成的代码是否保持行为。先选择能回答本次变更问题的
最小套件；对于高风险拉取请求，再运行更广的聚合测试。

## 配置测试构建

除非启用 `BUILD_TESTING`，否则测试不会构建。完整套件通常使用 Release；Debug
保留断言和单步能力，但有意不优化，不代表解码基准性能。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

完整 fixture 集要求 `clang` 能进行跨目标编译，并要求 LLVM linker（`ld.lld`
与 `lld-link`）位于 `PATH`。CMake 无条件构建许多可重定位 fixture，并在存在
对应 linker 时构建已链接 ELF/PE fixture。因主机无法编译或链接 fixture 而跳过
的测试属于未执行覆盖，不代表该目标通过。

克隆、构建配置与 macOS 预编译 LLVM 说明见
[CONTRIBUTING.md](CONTRIBUTING.md)。

## 驱动模拟检查

同时启用 `NEVERD_ENABLE_DRIVER_EMULATION=ON` 与 `BUILD_TESTING=ON`，即可构建专项执行套件及共享 C API／CLI 检查：

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

fixture 覆盖来宾初始化、成功与失败返回、不支持的行为、内存故障、严格场景解析、有界执行，以及经过 create、传输、cleanup、close 和 unload 的同步 buffered／direct I/O、READ/WRITE、独立文件生命周期、MDL 权限、动态导出解析、来宾变参和结构化 CPU 故障。使用 [`emulate-driver` CLI](driver-emulation.md) 验证 JSON 与进程退出码。生产构建可在 `BUILD_TESTING=OFF` 时启用此功能；`libneverd` 不得依赖仅供测试使用的 Unicorn 配置。

补充测试覆盖驱动独立拥有的非分页池 MDL、描述符与缓冲区的独立生命周期、注册表查询布局与短缓冲区、句柄权限、删除和泄漏，以及无输出 IOCTL 的完整 64 位 `information_hex`。真实样本验收还包括 Zero 的同步直接读写和统计查询。

后端测试验证完整 CPU 上下文（寄存器、标志、SIMD、FPU、CR8）、共享内存及跨后端／故障上下文拒绝。编译后的 `driver_dispatcher.c` 样例实际执行 DPC 与工作项回调，覆盖定时器边界、通知／同步事件与定时器、原因 `Executive` 的非警报 `KernelMode` 等待、超时／延迟、多个阻塞栈、置位后重置仍保留唤醒、回调参数及非法 IRQL／生命周期。工作项测试继续覆盖待处理／完成、队列、停滞和共享预算。这些用例证明所述子集，不代表完整 Windows 异步支持。

`driver_context_limits.c`: API 的 IRQL 上限来自 `KernelAPIIRQL.def`，参数相关限制由所属模型检查。DPC 不能调用注册表 API，也不能分配、释放或访问分页池；Unicode `DbgPrint` 转换要求 `PASSIVE_LEVEL`，支持的 ANSI 输出和非分页操作仍可在 `DISPATCH_LEVEL` 使用。回调栈有明确边界，越界栈指针不能进入另一阻塞工作项的栈。设备扩展中的已启动定时器会阻止设备提前回收。这些检查并未开放通用 IRQL 切换。

`KernelDeviceStackTests.cpp` 检查独立的所有者／附着关系、栈顶选择、失败原子性、栈容量、不透明字段、打开句柄计数、拆链／删除时的工作项及请求保活，以及文件身份与派发栈顶的区别。原创 `driver_wdm_stack.c` 使用真实 WDK 头文件和内联 Copy/Skip/SetCompletion；普通／启用 CFG 映像由可选 `NEVERD_WDM_STACK_FIXTURE`／`NEVERD_WDM_STACK_CFG_FIXTURE` 配置。`DriverWDMStackTests.cpp` 覆盖重定位、真实下层状态、完成顺序和标志、延迟 pending 传播、工作项／DPC、等待、`STATUS_MORE_PROCESSING_REQUIRED`、直接 MDL 保留、嵌套完成及畸形游标／控制值。`DriverScenarioPublicTests.cpp` 覆盖 C API／CLI 转发及 C API 保留／嵌套完成，包括已配置 CFG 映像。缺少产物会明确跳过；Linux 证据仅证明同驱动设备栈子集，不代表 PDO／PnP／电源支持。 `KernelIRPStackTests.cpp` 检查计数游标、完整内联 Copy 前缀、已消耗栈位置清零、状态／pending 传播、MPR 与嵌套完成、续接所有者检查及保留路径。真实 READ/WRITE 与文件生命周期也使用内联 Copy 验证。

`DriverPnpScenarioTests.cpp` 检查 JSON／原生预检一致、显式初始事实、ID／数量限制、字段互斥、最终总线状态及可空观测报告。`KernelPnpDeviceTests.cpp`、`KernelPnpRequestTests.cpp` 和 `KernelPnpCompletionTests.cpp` 检查提供者所有权、AddDevice 成功／失败／泄漏、初始 IRP、文件准入、生命周期回滚、延迟完成、MPR／嵌套／等待续接及失败原子性。原创真实 WDK 样例 `driver_wdm_pnp.c` 使用可选 `NEVERD_WDM_PNP_FIXTURE`／`NEVERD_WDM_PNP_CFG_FIXTURE`。`DriverWDMPnpTests.cpp` 验证普通／启用 CFG 且重定位的 AddDevice、文件 I/O、有序移除、延迟启动／移除、启动／query 失败及干净／泄漏的 AddDevice 失败。缺少产物会明确跳过。执行证据仅来自 Linux，只证明所述无资源 PnP 子集。 `DriverScenarioPublicTests.cpp` 还通过 C API 和 CLI 验证普通／启用 CFG 映像的七请求延迟 PnP 报告。

V9 schema 测试往返验证八种次功能名称，并与生命周期完成共享最终状态校验；QueryStop 0x119 在加载映像前拒绝。扩展模型及真实样例检查 query-stop 回滚、cancel-stop、停止／重启、突然移除、精确成功失败边界、停止／待移除状态的软件 I/O、突然移除后的来宾拒绝、设备身份及混合 AddDevice 结果。`DriverScenarioPublicTests.cpp` 通过 C API 和 CLI，在普通／启用 CFG 样例上运行 16 请求的停止／重启／突然移除序列，保留成功软件 IOCTL 字节、来宾拒绝的 IOCTL 和最终 cleanup/close/remove。公开执行为串行：保留 IRP 若无当前可用生产者，无法等待后续场景请求来启动或清理设备。Remove 排空要求是配置边界，不是通用 Windows I/O 准入策略。证据仍仅来自 Linux。

`KernelRemoveLocksTests.cpp` 检查独立锁／设备身份、NULL／重复 Tag、retail／DBG 大小、立即／延迟排空、失败获取义务、失败原子性、容量及退休。`KernelRemoveLockBridgeTests.cpp` 检查附加前初始化、扩展范围、不透明存储、IRQL 和修改前拒绝不安全 Delete／Detach。真实 `driver_wdm_remove_lock.c` 的 retail／DBG、普通／active-CFG 四种样例通过 `NEVERD_WDM_REMOVE_LOCK_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` 指定。`DriverWDMRemoveLockTests.cpp` 覆盖包退休后释放、锁排空后总线才完成、最后 release 先于回调返回唤醒、工作项等待及干净 AddDevice 失败。C API／CLI 使用既有 PnP 场景，保留总线接收／完成与最终拆除观测。缺少产物明确跳过；Linux 证据不代表完整 Driver Verifier 或通用并发排空。

`DriverPowerScenarioTests.cpp` 验证必填电源事实、JSON／原生一致性、不透明32位 context、响应 FIFO 限制和独立子报告。`KernelPowerRequestTests.cpp` 与 `KernelPowerCompletionTests.cpp` 检查包布局、路径标志、生命周期与设备通知的区别、FIFO 匹配、最终回调所有权、MPR、等待及释放边界。真实 WDK 原始样例 `driver_wdm_power.c` 使用可选 `NEVERD_WDM_POWER_FIXTURE`／`NEVERD_WDM_POWER_CFG_FIXTURE`；`DriverWDMPowerTests.cpp` 覆盖普通／active-CFG 重定位、直接及嵌套 Query/Set、独立延迟完成、S0 先于 D0、跨等待五参数回调快照、工作项来源子请求、空回调、query 拒绝、独立 PDO 初值／FIFO 和缺失事实错误。`DriverScenarioPublicTests.cpp` 增加格式错误预检及 C API／CLI 的六场景请求／三子请求睡眠唤醒序列。缺少真实产物明确跳过；执行证据仅来自 Linux，只证明文档中的可分页无资源电源子集。

`DriverResourceScenarioTests.cpp` 检查显式 JSON／原生事实、整数宽度、数量、物理／寄存器区间重叠、对齐、ID、空银行及配置序列化。`KernelMMIOTests.cpp`、`KernelMMIOFailureTests.cpp`、`KernelResourceBridgeTests.cpp` 与 `UnicornMMIOTests.cpp` 覆盖银行／映射所有权、别名、资源身份、紧凑清单生命周期、提供者时序、重启后的值保留、突然移除／电源可访问性、精确 CPU／API 事务及失败原子性。原创真实 WDK `driver_wdm_resources.c` 使用 `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`；`DriverWDMResourceTests.cpp` 执行真实标量及 REP 访问函数、普通／active-CFG 重定位、子区间别名、页尾映射、STOP／重启及非法访问。C API／CLI 测试在加载映像前拒绝无效事实，并执行相同的 14 请求重启场景，核对持久 IOCTL 输出及精确映射／取消映射次数。共享 [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) 需要此样例的寄存器／IOCTL 协议。缺少产物明确跳过，证据仅来自 Linux；测试不访问宿主物理内存，也不覆盖通用设备后端。

`DriverDMAScenarioTests.cpp` 验证显式能力、逻辑地址域、字节／数量／时间上限、严格事件方向和独立配置／观测。`KernelPhysicalMemoryTests.cpp` 与 `BackendBackingTests.cpp` 检查同页分配边界、固定引用、CPU 权限不变、MMIO／重入排除和整区间失败原子性；`KernelRequestMDLTests.cpp` 检查已构建描述符的别名及模型只读 PFN 与同一物理身份一致。`KernelDMATests.cpp`、`KernelDMABridgeTests.cpp` 和 `SchedulerDMATests.cpp` 覆盖实际 RAM 字节、适配器绑定的表调用、内嵌／排队 FIFO 所有权、独立回调／映射寿命、页片段、错误方向、释放预检、独立 PDO 地址域及资源代次／电源失败。原创真实 WDK `driver_wdm_dma.c` 使用 `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE`；`DriverWDMDMATests.cpp` 及 C API／CLI 覆盖真实适配器指针、公共／SG 存储和分别配置的 DMA／中断事件。共享 [driver-dma-scenario.json](../examples/driver-dma-scenario.json)要求该 fixture 的协议。缺少产物明确跳过；执行证据仅限 Linux，不代表宿主 DMA、PCI 或通用设备引擎。 `pluginsdk/python/tests/test_driver_dma_integration.py` 使用 `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_FIXTURE` 和 `NEVERD_TEST_WDM_DMA_CFG_FIXTURE` 执行现有带所有权管理的 JSON 绑定，覆盖实际字节、回调顺序和报告中的失败。

`KernelDMAChannelTests.cpp`、`KernelDMAChannelBridgeTests.cpp` 和共用 `SchedulerDMATests.cpp` 检查混合分配 FIFO、回调返回宽度、纯接纳／释放预检、寄存器复用、连续页片段、整次操作刷新、CurrentIrp 快照及包／MDL／设备生命周期。原创真实 WDK `driver_wdm_dma_channel.c` 使用可选 `NEVERD_WDM_DMA_CHANNEL_FIXTURE`／`NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`；`DriverWDMDMAChannelTests.cpp` 执行普通／活动 CFG／重定位驱动，覆盖真实 MapTransfer 与 FlushAdapterBuffers 调用、公共／SG／通道共享额度、显式设备事务、IRQ/DPC 完成、连续操作、两个 PDO 及失败案例。C API／CLI 运行七请求 [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json)，包括一个跨越两个已映射片段的单独事务。`pluginsdk/python/tests/test_driver_dma_channel_integration.py` 使用 `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE` 和 `NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` 验证同一公开 JSON 接口。缺少产物明确跳过；Linux 证据不代表已支持系统 DMA 控制器或任意 HAL 映射／刷新模式。

`DriverInterruptScenarioTests.cpp` 覆盖显式原始／转换后描述符、混合及纯中断分配、严格事件字段／数量、源身份及独立 BOOLEAN 观测。`KernelInterruptsTests.cpp`、`KernelInterruptBridgeTests.cpp` 与 `SchedulerInterruptTests.cpp` 覆盖独占元组匹配、不透明令牌、资源代次／连接捕获、事件生命周期、精确选定的 Ex 字段、共用锁和 IRQL 恢复、回调所有权、同一时刻 ISR 优先级及修改前的容量失败。`KernelFrameworkRequestTests.cpp` 检查纯取消预览和批量令牌容量，不发布回调或消耗引用。原创真实 WDK `driver_wdm_interrupts.c` 使用 `NEVERD_WDM_INTERRUPT_FIXTURE`／`NEVERD_WDM_INTERRUPT_CFG_FIXTURE`；`DriverWDMInterruptTests.cpp` 执行普通／active-CFG 重定位、传统十一参数 ABI、Ex 版本 1／2／4、真实 ISR→DPC 完成、低位 AL 的 FALSE、同步／手动锁、独立 PDO、重启资源代次及非法硬件事实。C API／CLI 测试在加载映像前拒绝非法声明，并执行七请求的 [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json)，检查 pending IOCTL 字节和独立递送观测。缺少映像明确跳过，执行证据仅来自 Linux，不代表支持共享／电平／MSI 中断或指令级抢占。

`DriverGuardTests.cpp` 与四个原创 `driver_guard.c` 变体覆盖启用／未启用的 CFG、重定位、检查／分派 ABI 和畸形目标。`KernelFrameworkTests.cpp`、`KernelFrameworkControlTests.cpp`、`KernelFrameworkQueueTests.cpp` 和 `KernelFrameworkRequestTests.cpp` 覆盖绑定、可回滚的设备创建、队列路由、缓冲区逻辑长度，以及清理顺序与 IRP／上下文生命周期。原创 `driver_kmdf_lifecycle.c` 与 `driver_kmdf_control.c` 可选用真实 WDK 1.33 头文件编译，并通过真正的 `FxDriverEntry` 库链接。将 CMake 缓存路径 `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` 指向生命周期映像，将 `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` 指向普通／启用 CFG 的控制设备映像。缺少外部产物时会明确跳过。`DriverKMDFLifecycleTests.cpp`、`DriverKMDFControlTests.cpp` 及 `DriverScenarioPublicTests.cpp` 中的 C API／CLI 用例覆盖实际回调、缓冲／直接 I/O、工作项完成待处理请求、失败状态、卸载和重定位后的 CFG 执行。验证证据仍限于 Linux，不代表完整 KMDF 或 PnP／电源管理支持。

旧版取消测试将 API 续接保留到取消、嵌套清理与最终销毁结束；对于已经取消的请求，Ex 仍返回取消状态而不递送回调。`KernelFrameworkRequestAccessorTests.cpp` 与 `KernelRequestMDLTests.cpp` 覆盖共享的 64 位 Information、完成时长度验证、来源队列／IRP 身份、NULL WDF 文件句柄、保留句柄的 getter 结果、缓冲 MDL 缓存与首个方向的 ByteCount、直接描述符身份与延后映射、完成时回收，以及拒绝绕过 WDF 完成流程。真实控制设备 fixture 的 L、M、D、C 模式在普通／启用 CFG 映像中分别执行旧版取消、缓冲 MDL／信息、直接 READ／WRITE MDL 和完成后的访问。

取消测试覆盖仅允许传输请求配置的虚拟期限及报告字段、完成优先与已取消路径、标记／解除标记结果、排队与已递送回调的完成权限、回调等待及内部引用生命周期。调度器测试独立验证 DPC／取消／工作项顺序、容量、身份隔离和暂停／恢复。WDM 取消仍明确报告模型错误。


## 测试布局

`add_neverd_unittest` 创建一个 GoogleTest 可执行文件，并为每个发现的用例分配
与该可执行目标同名的 CTest 标签。

| 源码区域 | 目标与 CTest 标签 | 覆盖内容 |
|----------|-------------------|----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | 跨平台子进程调用、引号、重定向与退出码 |
| `unittests/libc` | `NeverDLibCTests` | 已知 libc 名称与分类 |
| `unittests/safety` | `NeverDSafetyTests`、`NeverDSafetyIntegrationTests` | 汇目录、身份优先序、参数预过滤、拷贝越界猎取、堆生命周期审计，以及强制执行的 PE/ELF/Mach-O × x86-64/AArch64 六单元矩阵 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR 形状、IR 阶段、loader、重定位、格式 fixture、反编译与代表性 patch 流程 |
| `unittests/semantic` 中的大多数文件 | `NeverDSemanticTests` | 指令、ABI、控制流、C 表达式和 lift/recompile 差分语义 |
| `unittests/evm` | `NeverDEVMOpcodeTests`、`NeverDEVMBytecodeTests`、`NeverDEVMLoaderTests`、`NeverDEVMABITests`、`NeverDEVMAnalyzerTests`、`NeverDEVMDecoderPropertyTests`、`NeverDEVMProxyTests`、`NeverDEVMCallTests`、`NeverDEVMSemanticTests`、`NeverDEVMEmitterTests`、`NeverDEVMIntegrationTests` | 硬分叉元数据、输入规范化、ABI/签名歧义、CFG/SSA/恢复、穷举 decoder 边界与恶意输入、proxy/call 事实、解释器语义、LLVM/C/Solidity 差分执行及公共 API 路由 |
| `unittests/sbf` | `NeverDSBFMetadataTests`、`NeverDSBFProgramImageTests`、`NeverDSBFLoaderTests`、`NeverDSBFAnalyzerTests`、`NeverDSBFVerifierTests`、`NeverDSBFISAConformanceTests`、`NeverDSBFAgaveConformanceTests`、`NeverDSBFSemanticTests`、`NeverDSBFEmitterTests`、`NeverDSBFLLVMEmitterTests`、`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`、`NeverDSBFMalformedCorpusTests`、`NeverDSBFUpstreamConformanceTests`、`NeverDSBFExternalOracleTests`、`NeverDSBFSolanaModelTests`、`NeverDSBFIntegrationTests` | v0-v4 元数据与 ELF 布局、严格 verifier/loader 行为、23 个固定 ELF 工件、独立 official oracle、全部 opcode 可用性、恶意输入、CFG/恢复及已执行的 LLVM/C/Rust 差分 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 四 ISA×三对象格式的重写/混淆等价性 |
| `unittests/semantic` 中的聚焦变换文件 | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 从大型语义二进制拆出的快速重链接探针 |
| `unittests/corpus`（子模块） | `NeverDWindowsEHCorpusTests`、`NeverDRustEHCorpusTests`、`NeverDGoEHCorpusTests`、`NeverDCxxItaniumEHCorpusTests`、`NeverDObjCEHCorpusTests` | 从 317 个钉住的真实二进制中读出的异常与运行时元数据，每个都在清单里声明了其恢复必须达到的下限 |

注册的事实来源是
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt) 和
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt)、
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt) 和
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt) 和
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt)。

### 钉住的二进制 corpus

其它每个测试套件都自己构建被测对象，corpus 不是：它是一个子模块，装的是真实工具链
在本仓库够不到的宿主机上、为够不到的目标产出的二进制，每一个都按摘要钉住，旁边的
清单声明了它的恢复必须达到的下限。要回答"NeverD 从一个 `-O2` stripped 的 `armv7`
共享库里到底读出了什么"这类问题，只有这里给得出答案而不是论断。

这些套件只在 configure 被告知去找它们时才构建，所以这个开关就是它们是否受测的全部：

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` 跑全部产线；`check-neverd-windows-eh-corpus`、
`check-neverd-rust-eh-corpus`、`check-neverd-go-eh-corpus`、
`check-neverd-cxx-itanium-eh-corpus` 与 `check-neverd-objc-eh-corpus` 各跑一条。三个
CI 宿主都带着这个开关配置并跑全部五条产线：字节到处都一样，但读字节的东西不一样，
在一台宿主上跑通不能说明另外两台。`scripts/audit_ci_test_inventory.py` 会拒绝缺少五
个标签中任何一个的清单——构建悄悄不再读 corpus 是一种没有任何测试能捕获的回归，因为
消失的正是那个测试。

EVM 操作码审计每次运行都会用 `git fetch --depth=1 --force` 强制获取官方默认分支的远端
`HEAD`：`https://github.com/ethereum/go-ethereum.git`。脚本解析并报告刚取得的精确
SHA，再在 detached 临时 worktree 中探测该对象。每次运行都使用名称不可预测的私有临时 bare
repository，在 detached worktree 的整个生命周期持有官方 fetch 的 authority ref 与精确 SHA，
最后一起销毁 repository 和 worktree。不使用共享持久 Git repository 或 cache。
本地与 CI 都不读取 `local_docs`、已有源码 checkout 或 submodule。固定 submodule 反而会在
最需发现实时漂移时陈旧：

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

公开 CLI 唯一接受的选项是 `--manifest-output`，不提供 remote/ref/toolchain override。输出
manifest 的封闭契约是 `schema 3`。

每条 Git 命令都会先清空全部继承的 `GIT_*`（包括 `GIT_CONFIG_*`），再只装入经过审计的
设置。`GIT_CONFIG_NOSYSTEM` 与 `GIT_CONFIG_GLOBAL` 禁用 system/global 配置；
`GIT_ATTR_NOSYSTEM` 与按命令设置的 `core.attributesFile` 禁用 system/global attributes，
`core.hooksPath` 禁用 hooks。意外的 private-repository 配置、graft、`objects/info/alternates` 或
`refs/replace` 都会让校验失败；`GIT_NO_REPLACE_OBJECTS` 会禁用 replacement 查找。

CI 仅在 `dev` 分支 push、pull request、手动触发与每日定时任务中运行同一项在线审计。
Go 探针反射 `params.Rules` 导出的全部 bool 字段，针对每个映射分叉调用公开的
`LookupInstructionSet(params.Rules)`，并扫描全部 256 个 byte slot。
`EVMUpstreamOpcodePolicy.def` 管理名称别名及类型化的历史/未排期 EOF 排除项，并校验
overlap/inactive 不变量；正交的 `EVMUpstreamSemanticsPolicy.def` 管理封闭的 Rules
清单、分叉映射、base-stack 例外与 EIP-8024 dynamic opcode family 声明。封闭 manifest 检查精确
revision、fork activation、byte/name、`base_min_stack` 和 `net_stack_delta`，拒绝未知或
重复字段、规则、分叉、名称与字节。槽位分配只依据 `operation.undefined`；`HasCost` 只用于
费用交叉检查，因为已定义的零费用操作也返回 false。每个 `defined && !HasCost` 槽位都必须
从声明的分叉起与 `EVM_GETH_ACTIVE_WITHOUT_COST` 精确匹配。未定义却有费用、未经评审却已
定义，或 marker 消失都会封闭失败。失败的 CI 会上传精确 revision、manifest 与日志 artifact。
parser 与漂移诊断有独立 Python 单元测试：

`EVMUpstreamSemanticsPolicy.def` 用唯一一条 `EVM_GETH_RULE_FIELD` 将每个导出的布尔
`params.Rules` 字段归入 `MappedForkSelector`、`NoOpcodeAllocation` 或
`ExcludedSelectorExpectedError`。probe 每次只启用一个字段并调用 `LookupInstructionSet`；前两类
必须无错误，第三类必须报错，返回的完整 256 槽 opcode/stack 指纹都必须等于 `ExpectedFork`。
当前 `IsEIP155`、`IsEIP2929`、`IsEIP4762` 与 `IsPetersburg` 是 Frontier 指纹的无分配字段；
`IsUBT` 必须报错并呈现 Cancun 指纹。

EIP-8024 dynamic opcode family 的成员与启用条件由 `EVMUpstreamSemanticsPolicy.def` 声明；
`EVMEIP8024Immediates.def` 仍是 single/pair 各字节 immediate semantics 的唯一权威，其清单都
显式分类全部 256 个字节。生产代码直接查表；实时审计以 `go -overlay` 向 `core/vm` 虚拟注入
wrapper，取得真正的私有 `operation.execute` handler，并对每个 active table/family 执行
`DUPN`、`SWAPN` 和 `EXCHANGE` 的 `3x256` candidates 加 `3 missing-operand cases`。测试核对
接受性、PC 增量、marker 推导的 operand/stack 变更、有效值的精确 underflow 和缺少 operand 时的
`0x00`；Python 对照同一 `.def`，不重复公式。

`EVM_HARDFORK_LATEST` 只有一个规范目标；封闭的 `EVMUpstreamForkAliases.def` 将 Prague 映射到
Pectra，将 Osaka 与 BPO1 至 BPO5 映射到 Fusaka，而 Paris/Shanghai/Cancun/Amsterdam/Bogota
映射到自身。未知名称封闭失败。单次审计记录的 `audit_unix_time` 同时驱动
`MainnetChainConfig.LatestFork(time)`（必须等于 NeverD latest）和
`LatestFork(max uint64)` 的 alias/已探测规范分叉检查。探针枚举真实的
`canonical fork jump tables` 与 `mainnet active/scheduled jump tables`，逐表完整比较，并显式
记录 dynamic family 或分叉的 `inactive` 状态。只得到部分表、family 或探针的 `partial` result
不会被接受，而会封闭失败。manifest 固定
`authority=official-fresh-fetch`、官方 URL、请求的 `HEAD` 与 SHA；公开 CLI 没有
remote/ref/toolchain 绕过，probe 使用 `GOTOOLCHAIN=local`。

Go request/response 与 Python controller 会在分配恶意元数据前执行
`input/collection/string hard limits`，超限输入、数组或字符串均封闭失败。它们还独立执行
`bounded diagnostic output`：超长展示包含 full-content `digest` 与
`explicit truncated marker`。每条命令都有有界的子进程输出和共享 deadline；超时或输出超限会
终止整个 `process group` 及其后代 process tree，并排空 pipe。所有 `.def parser` 都会拒绝
unparsed、unknown、duplicate、missing、out-of-range 条目并封闭失败。

当前 schema-3 实时回执记录 `schema_version=3`、`audit_unix_time=1787534659`、
`authority=official-fresh-fetch`、`remote=https://github.com/ethereum/go-ethereum.git`、
`ref=HEAD`、revision `02b73d4ea7181464175e0a6cbecc0a3a2655a562`、本地 `Go 1.24.0`、
`stack_limit=1024` 与 `diagnostics=[]`。它覆盖 `21 fork tables` 和 `20 Rules probes`，分类为
`15 mapped/4 no-op/1 expected-error`。两个 `mainnet active/scheduled` 记录均报告
`upstream BPO2`，由封闭映射对应到 `NeverD Fusaka`。EIP-8024 有 `23 table targets`，其中只有
`Amsterdam/Bogota` 为 active，产生 `1536 candidate executions` 与
`6 missing-operand cases`。`three handler symbols` 在两个 active target 间一致。Python audit 为
`67/67`，`C++ Opcode 10/10`。macOS 真实运行在 `sandbox-exec` 下成功，最终 `go run` 保持
offline；Linux workflow 强制 `bubblewrap`。

所有 Go 阶段——`go env`、`go mod init`、`go mod edit`、`go mod tidy`、
`go mod download` 与 `go run`——都必须经过 `capability-root` 文件系统沙箱。其读取能力只包含
私有 probe、fresh geth、校验后的 `resolved GOROOT` 和精确必需的系统 runtime root；只有隔离的
environment root 可写。网络仅授予需要它的依赖阶段，最终运行保持离线。测试在
`host HOME/workspace` 中放置 sentinel，要求访问被拒，并要求任何输出都不含其内容。Linux 验证
同构的 `bubblewrap` 策略，且不使用 `/` broad bind。

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

当前 CMake 注册的 11 个 EVM 测试目标为：

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` 会在每个改变 decoder 的分叉上穷举全部双字节输入，比较
完整解码和精确 `JUMPDEST` 边界；它还以长度受限的确定性恶意输入覆盖所有分叉。

修改 EVM 控制流时，先运行不动点与高度域契约：

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

这些用例覆盖跨基本块 internal return、有限多目标合并、循环收敛与确定性边排序、
路径相关 whole-stack lane、相关性保留、未知跳转、精确非法目标，以及包括
`MaxAbstractInstructionTransfers` 在内的 fail-loud 分析预算。strict 只在已证明
`Reachable` 的 lane 上拒绝未知或分叉未激活 opcode；`MayReachable` 只保留 CFG 候选，
不能产出确定语义。随后应
运行全部 11 个 EVM 测试目标与在线上游审计；CFG 修改也可能影响 emitter 与集成行为。

修改 MedIR/HighIR 数据流时，还要运行 constant-phi、selector、类型化操作数、
格式错误图和深链契约：

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

这些用例验证相等与冲突的循环 phi、非相邻和跨基本块 selector 表达式、等式两种操作数
顺序、精确 ABI 位宽检查、类型化 storage/event/calldata 操作数、只从 root lane 沿
dispatcher 不匹配边恢复 selector/receive/fallback、共享 selector 的标准歧义、逐标准
`KnownFunctionVariantInfo` 选择，以及只在所有已证明可达的成功终态返回形状一致时输出
return list。它们还覆盖格式错误 MedIR 的确定性处理与深 producer walk。

## fixture 如何生成

### Lift 与格式 fixture

`unittests/lift/CMakeLists.txt` 在构建期间跨目标编译 C 与汇编源码。Clang target
triple 生成 x86-64、i386、AArch64、ARM32 ELF 对象，PE/COFF 对象和已链接
镜像，以及 PIC/no-PIC Mach-O i386 对象。存在 LLD 时，选定对象还会链接为
patch 测试所需的可执行文件。`NeverDLiftTests` 依赖 `lift-test-objects` 目标，
因此正常构建该测试二进制会刷新生成的 fixture。

多数 lift 测试使用 `NeverDLiftFixture.h` 调用构建出的 `neverd` CLI，并检查
LowIR、MedIR、HighIR、LLVM IR、生成的 C 或重写后的二进制。聚焦手动实验可用
`NEVERD` 环境变量覆盖 CLI 路径；普通 CTest 运行使用 CMake 嵌入的可执行文件。

### 内存安全 fixture

`unittests/safety/fixtures/binaries` 检入了 x86-64 与 AArch64 的 PE、ELF、Mach-O 镜像，以及各格式对应的 PDB 或 dSYM 伴生文件，每个镜像还附带一份链接器 MAP。MAP 是被 strip 的构建唯一还会留下的身份信息，因此每个单元还会显式指定 MAP 再分析一遍，用来钉住在既无类型也无源码行号时结论还能说什么。`NeverDSafetyIntegrationTests` 在每个主机上运行全部六个单元；任何必需镜像或伴生文件缺失都会在配置阶段失败，测试不存在按宿主工具链跳过的路径。

六个等价二进制来自同一源文件。`make` 只重建宿主原生 smoke fixture；完整矩阵用：

```bash
make -C unittests/safety/fixtures matrix
```

完整重建需要 Clang 的 Linux／Windows 交叉目标、LLD COFF 工具、两个 Darwin 架构与 `dsymutil`。规则会重映射调试路径并关闭 CodeView 命令行记录，避免检入的伴生文件捕获开发者工作区绝对路径。

### Windows 异常重建

修改 Windows 表驱动异常时，既要测试表示层，也要对已链接 PE 运行 patch 测试。
下面的聚焦 lift-suite 过滤器覆盖规范化 unwind/SEH/C++ 模型、损坏输入处理、
异常 CFG 边、HighIR、LLVM WinEH 生成、异常目录替换，以及 Guard CF/EH
continuation 重建：

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

受保护的 x64 汇编 fixture 需要 Clang Windows target 和 `lld-link`；其 CMake
链接使用 `/guard:cf` 与 `/guard:ehcont`。因缺少交叉链接器而跳过，不能作为
final-image 路径的有效证据。集成用例通过后，才证明重写后的 PE 可以重新加载，
且 runtime-function、unwind、load-config、Guard CF 与 Guard EH continuation
表保持有序、由文件承载，并只指向可执行目标。

已链接的 FH3 fixture 独立覆盖原生 C++ 闭包：固定状态表、HighC 注释、
personality 保留、生成的 catch 目标，以及重新加载后的 IP-to-state 图。

分析/原生支持矩阵和 fail-closed patch 契约见
[Windows 异常重建](windows-exception-reconstruction.md)。

### 语言异常模型

除 Windows 表模型以外的一切都集中在一个聚焦 target 中。
`NeverDLanguageEHTests` 覆盖 DWARF 帧链、Itanium 语言特定数据区、ARM EHABI、
Darwin compact unwind、Go 运行时帧元数据、Rust panic 机制，以及三种
Objective-C 运行时：

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

本套件中的表是逐字节手工装配而非编译出来的，因为其中大多数要验证的组合
没有任何单一工具链会同时产出。Objective-C 是最典型的例子：三种运行时都发出
Itanium LSDA，差别只在类型表槽位里放什么——而这个差别是彻底的，不是程度问题。
Apple 的槽位指向 `objc_typeinfo`，其前两个字段刻意模仿 `std::type_info`；
GNUstep 的 Objective-C++ 槽位指向真正的 `std::type_info` 子类；GNU 运行时的
槽位根本不是指针，而是类名字符串本身。把一种运行时的约定套到另一种的表上
不会报错，只会报出一个从别的东西中间读出来的类名——所以在读任何槽位之前，
先由帧的 personality 确定运行时。

同一套件还钉住两个容易混为一谈、但混淆即错误的区分。`@catch(id)` 与
`@catch(...)` 是不同的处理器——前者接收任意 Objective-C 对象，并放外来异常
从旁边继续传播——而每种运行时对二者的拼写都不同，所以把两者都报成 catch-all
的解码器，等于给那些本会飞过去的异常安上了处理器。另外，setjmp/longjmp 的
call-site 表索引的是调用点序号而不是地址，因此没能认出某个 SJLJ personality
的读取器不会报错，而是会凭空造出程序从未指定过的保护区间和 landing pad。

认出这种形式，和拒绝解码它，是两回事。一条 SJLJ 条目是一对 ULEB128 值——
一个派发选择子和一个动作偏移——而这个动作偏移在此处的含义与地址形式中完全
一致，所以动作链、catch 类型、异常规格，全都能从一张根本不指名任何代码的表
里读出来。唯一读不出的是每条条目守护的区间，因为说明它的是函数自己对
call-site 槽位的写入，而不是表里的任何东西。该套件还钉住了此处唯一不可信的
那个字节：GCC 把 call-site 编码写成 `DW_EH_PE_uleb128`，LLVM 写成
`DW_EH_PE_udata4`，两者随后都照样发射 ULEB128，而没有任何 personality 会去
读它——所以解码器也不许读。

personality 身份同样在这里钉住，因为它决定了上面每张表该怎么读。GNAT 用
GCC 给每个前端的那三种拼法命名自己的例程——`_v0`、`_sj0`、`_seh0`——并且在
Windows 上注册一个符号却转发到另一个，所以这四种拼法都必须落到 Ada 上。D
则是镜像的情形：三个编译器，同一个例程的三个名字，背后是同一套表。

### Unicorn 差分往返

语义 fixture 测试行为而不是文本形状：

1. 编写一个小型 C/汇编用例，或构造 LLVM IR。
2. 用 Clang/LLVM 为请求的目标编译它。
3. 在 Unicorn 中执行原始机器码，并捕获期望返回值或 fixture 定义的其他状态。
4. 通过 NeverD 加载并提升，发射 LLVM IR，再将结果编译回机器码。
5. 使用相同 ABI、输入、内存布局和 CPU 模型执行再生成的代码。
6. 比较可观察结果。

主要实现位于
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h)。
patch-full fixture 使用 `Codegen::compileForRewrite`（与 patch 操作相同的重写
backend），随后在完整 4×3 ISA/格式网格中比较基线与变换后代码。

确定性的 NeverD 语义失败应当成为失败测试。跳过只用于明确的外部能力边界，并应
阅读 skip 原因：缺少跨目标 linker 的绿色摘要不能证明该格式路径实际运行。

### EVM 差分后端

EVM 解释器测试提供确定性的 256 位 oracle。emitter suite 直接编译执行生成 LLVM，
通过 Clang lower 生成 C23 并在同一 host harness 中执行；安装 `solc`、`anvil`、
`cast` 与 `jq` 后，还会将生成 Solidity harness 部署至本地 Anvil。测试比较 status、
storage 与 instruction trace count。独立 raw-bytecode corpus 直接在 Anvil 原生 EVM
中执行 pre-Fusaka 标量 ALU、calldata/memory 复制、重叠 `MCOPY`、Keccak 与 return data。

解释器在任何 opcode 特有副作用之前执行类型化 stack preflight；
`EVMForkSemantics.def` 规定字节 `0x44` 在 Paris 前为 `DIFFICULTY`、从 Paris 起为
`PREVRANDAO`。`REVERT`、fault、step limit 和资源耗尽都会回滚事务状态；分配失败标为
`ExecutionFaultKind::ResourceExhausted`，若入口快照都无法建立，
`HasPersistentStateSnapshot` 为 false，结果不可提交。

### EVM 公开边界与预算回归

公开 API 测试会分别篡改规范
`Code`/`Fork`/`Instructions`/`JumpDestinations`，以及每个 LowIR table、range、ID、lane 与
edge reference。`execute` 必须在查找 instruction 前返回 `llvm::Error`；`lowerToMedIR`
必须在建立索引或按输入规模分配输出前，拒绝完整结构非法或超预算的 LowIR。
`lowerToMedIR` 测试还强制 option validation、resource validation、structure validation 的顺序，
并要求它们先于逐字段 `canonical decode replay` 和 `lowerCanonicalLowToMedIR`。公开 HighIR
恢复会重放校验外部 LowIR/MedIR；只有 `analyze` 能对自己持有的规范 IR 使用
`lowerCanonicalLowToMedIR` 与 `recoverCanonicalHighIR`，既避免递归或重复重放，也继续强制
所有 HighIR option/resource 预算。解释器随后对
`EVMInterpreterLimits.def` 声明的所有上限执行 exact-boundary 与 +1 测试：`MaxSteps`
保持专用 `StepLimit`；`MaxMemoryBytes`、`MaxTraceEntries`、`MaxLogEntries`、aggregate
`MaxLogDataBytes` 与运行期 `MaxPersistentStateEntries` 耗尽都返回
`ResourceExhausted` 并回滚事务效果。初始 aggregate `MaxHostReturnDataBytes` 或 persistent
state 过大是 API error。初始 `MaxCalldataBytes`、横跨 `BlockHashes`/`Balances`/`CodeHashes`/
`ExternalCode`/`BlobHashes` 的 aggregate `MaxHostEnvironmentEntries`，以及 aggregate
`MaxExternalCodeBytes` 同样属于 API error。`const execute preflight` 会在复制 environment、
snapshot 或 result 前拒绝它们。测试也覆盖 return-data `ArrayRef` view 与排序表 `lower_bound`
lookup，无需复制 buffer 或建立 PC map。

独立的 LowIR 边界测试覆盖 aggregate diagnostic 上限 `MaxLowDiagnostics` 与
`MaxLowDiagnosticBytes`，验证线性 decode/CFG 构造按精确数量和最终字节预先计费并拒绝零上限。
HighIR 安全测试覆盖按 lane 排序的 `Any/Exact/Excluded` domain、相等 match/exclusion、原始
`XOR(selector, constant)` 的 false-edge match 与 true-edge mismatch、零 word/calldata
size/call value 的逐边精化，以及 unknown condition 的 fail-closed 行为。
测试还包含 `EQ` 与 `raw XOR` 两类 back-jump regression，确保 `arguments`、`mutability`、
`return shape`、`region` 不受另一函数污染。其 exact-boundary 与 -1 测试覆盖
`EVMAnalysisLimits.def` 中的 `MaxHighDispatchCandidates`、aggregate
`MaxHighRecoveredArguments`、`MaxHighDiagnostics`、`MaxHighDiagnosticBytes`、
`MaxHighReferenceVisits`、`MaxHighMemoryTransferCells` 与
`MaxHighMemoryValueVisits`；所有输出 diagnostic（包括固定 malformed diagnostic）都必须在
分配前计入数量与最终字节数。LowIR 与 HighIR diagnostic 预算会独立测试；构造默认根 CFG
region 时必须在 reserve 或复制 block-PC 清单前计入 `MaxHighRegionBlockReferences`。
外部 CALL/CREATE 结果作为非确定 host outcome 探索两条精确 CFG 边，因此保留 ERC-1167
fallback 恢复；不可读的 selector 条件仍是 Unknown，不能凭空产生 fallback 或 function 事实。

控制流测试从 `EVMLowFaultKinds.def` 取得 `InvalidJumpDestination`，并用于
`end-of-code JUMPI`：目标非法且条件确定为 true 时没有成功 tail，属于确定 fault；条件确定为
false 时成功；条件未知时保留可能成功的 false 路径，不把整条 lane 标为确定 fault。

ABI 测试在精确上限和 +1 位置验证 `EVMABIParserLimits.def` 的 grammar 边界，以及
`EVMABITableLimits.def` 的公开表基数/text 边界；还会拒绝非法 kind/standard/evidence
enum、错配 metadata、非规范 signature/return list、被错误标记为 independent 的共享
selector、悬空或重复 variant，以及非 word 宽度的 event-topic `APInt`，再进入索引化
selector 或排序 topic lookup。

`NeverDEVMOpcodeTests` 还约束 metadata 架构：每个已分配 opcode 都在 byte encoding 与
typed value 之间往返，测试 family helper 边界与 hardfork alias，完整 stack contract
和 host argument maximum 保持推导而不在 backend 中重复。

### Solana SBF 差分后端

SBF 元数据测试会验证每个版本特性、操作码冲突边界、Murmur3 syscall hash、重定位、ELF machine、寄存器和 VM 地址常量。Loader fixture 不依赖 vendored 二进制，直接生成旧式 v0-v2 section 布局和无 section 的严格 v3/v4 program-header 布局。

`NeverDSBFISAConformanceTests` 按 v0-v4 的每个版本，将每一种 byte encoding
与独立审计的 typed manifest 对照。`NeverDSBFExternalOracleTests` 随后把 activation
和 boundary 决策与单独构建的官方 Anza 进程比较。
`NeverDSBFUpstreamConformanceTests` 为固定 Anza revision 中的全部 23 个 ELF
指定明确结果。

`NeverDSBFSemanticTests` 直接执行已验证的指令字节而不消费 MedIR，因此修改或破坏规范化 IR 不会让源 oracle 与后端意外达成一致。覆盖范围包括非单调的 v2 语义、内存、syscall、内部调用帧、fault、trace 和资源限制。LLVM module 会被验证；生成的 C 以 warnings-as-errors 编译，Rust 使用 `-D warnings`。公共 API 测试从生成的严格 SBF ELF 出发，遍历所有 IR 阶段、反汇编、CFG、元数据、LLVM、C 与 Rust。

## 一次性目标

自定义目标会构建其依赖，然后以主机 CPU 推导的并行度运行 CTest：

| CMake 目标 | 选择范围 |
|------------|----------|
| `check-neverd` | 所有已注册测试 |
| `check-neverd-semantic` | 仅 `NeverDSemanticTests` |
| `check-neverd-sbf` | 所有 `NeverDSBF*Tests` 目标/用例 |
| `check-neverd-patch-full` | 仅 `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | 仅 `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | 仅 `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | 仅 `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` 与 `NeverDAvxUpperXformTests` 当前没有
`check-neverd-*` 便捷目标；请按下文先构建，再用标签选择。
`check-neverd-semantic` 也不包含单独的变换或 patch-full 二进制；完整聚合应使用
`check-neverd`。

## 增量 CTest 工作流

先构建所属可执行文件，再选择其标签。这样可以避免重链接无关的大型语义目标。

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# 所有聚焦的 EVM 目标/用例
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# 所有聚焦的 Solana SBF 目标/用例
cmake --build build-release --target check-neverd-sbf --parallel 4
```

用 GoogleTest 派生的 CTest 名称运行单个回归：

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

常用选择器：

| 命令 | 用途 |
|------|------|
| `ctest --test-dir build-release -N` | 列出已发现用例而不运行 |
| `ctest --test-dir build-release -L '<regex>'` | 选择测试二进制标签 |
| `ctest --test-dir build-release -R '<regex>'` | 选择用例名称 |
| `ctest --test-dir build-release --output-on-failure` | 仅为失败显示诊断 |
| `ctest --test-dir build-release --stop-on-failure` | 第一个失败后停止 |
| `ctest --test-dir build-release --parallel 4` | 最多并行运行四个用例 |

GoogleTest 发现使用 `DISCOVERY_MODE PRE_TEST`，因此 CTest 枚举前必须存在对应测试
二进制。每用例 timeout 与独立的发现 timeout 定义于 `cmake/AddNeverD.cmake`，
只有存在测得的重型用例时才应放宽。

## 哪些测试应随代码变化？

| 变更区域 | 从这里开始 | 随后考虑 |
|----------|------------|----------|
| 架构 lifter 或 decode | `NeverDLiftTests` 中的命名用例 | 对应 ISA 语义往返 |
| LowIR CFG、函数检测、跳转表 | Lift CFG/switch 用例 | `NeverDSwitchXformTests`、`NeverDCFGLoopXformTests` 或 `NeverDTwoTableXformTests` |
| MedIR、ABI、标志、类型、SSA | MedIR/调用约定 lift 用例 | 跨 ISA 的 `NeverDSemanticTests` 用例 |
| HighIR 或结构化 C | HighIR/decompile 用例 | `NeverDCFGLoopXformTests` 与生成 C 编译检查 |
| PE/ELF/Mach-O loader 或输入重定位 | 对应的 `unittests/lift` 格式 fixture | 该单元格的全阶段加载/反编译测试 |
| 重写 codegen 或输出重定位 | `RewriteCodegenRTTests` 用例 | `NeverDPatchFullTests` 及存在时的已链接 patch fixture |
| patch 使用的 LLVM IR 变换 | 聚焦变换二进制 | `NeverDPatchFullTests` 组合 pass 网格 |
| C API 或 CLI | 直接 SDK/query 测试与 `unittests/semantic/CLIEndToEndTests.cpp` | 相关 pipeline/格式套件 |
| EVM loader、opcode、IR 或 backend | 最小的所属 `NeverDEVM*Tests` 目标 | 所有 EVM 目标，以及生成 C/Solidity 的编译检查 |
| SBF loader、ISA、IR 或后端 | 最小的所属 `NeverDSBF*Tests` 目标 | 所有 SBF 目标，以及生成 C/Rust 的编译检查 |
| Libc 识别 | `NeverDLibCTests` | 行为变化时的语义 call/ABI 用例 |
| 堆生命周期审计或拷贝越界猎取 | `NeverDSafetyTests` | `NeverDSafetyIntegrationTests` 的全部六个单元 |
| 进程执行或 quoting | `NeverDTestProcessTests` | 每个受支持主机上的一个受影响 CLI/语义用例 |

测试应在最低的稳定边界表达契约。LowIR 形状测试适合归因到 lifter；若两种看似
合理的 IR 形状可能行为不同，则必须使用语义往返。若小型 opcode、CFG 或可观察状态
断言已经足够，应避免保存整个函数的 golden dump。

## 与 CI 的关系

CI 在 Linux、macOS 和 Windows 上以 Release 开启测试构建，先审核发现的测试清单，
再应用平台特定的标签排除。配置定义于 `.github/workflows/ci.yml` 和
`scripts/audit_ci_test_inventory.py`。每个矩阵主机都必须包含 `NeverDSafetyTests`
和 `NeverDSafetyIntegrationTests`，而且每次都读取同一组已检入的 PE、ELF、Mach-O × x86-64、AArch64 fixture。由于没有单个矩阵 shard 代表所有昂贵套件，当机器具备全部跨目标工具时，本地 `check-neverd` 仍是最清晰的完整合并前信号。

## 当前 Solana SBF 一致性与 sanitizer 配置

本节的当前清单取代上方较短的 SBF 清单。source differential suite 除 clang 外还
需要 `rustc`；compiler skip 表示覆盖缺失。完整 aggregate 包括
`NeverDSBFProgramImageTests`、`NeverDSBFMalformedCorpusTests`、
`NeverDSBFISAConformanceTests`、`NeverDSBFUpstreamConformanceTests`、
`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`，以及 metadata、
loader、analyzer、semantic、emitter、integration target。integrated profile 记录
命名 target 与结果，不冻结快速变化的汇总 case 数。

sanitizer profile 单独构建在 `build-sbf-asan-ubsan`。按 revision 锁定的 prebuilt
package 已包含所需的 fork-only header，因此 integration 也在同一个 fail-fast
ASan/UBSan profile 中运行。

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

### 固定的 SBF 证据快照（2026-08-24）

gate 将 Anza `sbpf` 固定在
`2510663bb8d894e8e3094be351e4bb4b604f1f84`、Agave 固定在
`ef210d67f2fabeee1730498188fa78854260c679`、Solana SDK 固定在
`122f32e571ce39face4beffaccea733e37c207fd`。官方 ELF manifest 全部 23/23 通过；
`NeverDSBFExternalOracleTests` 经 `SBFOfficialOracleProtocol.def` 和
`SBFOfficialVerifierCases.def` 与 `SBFOfficialExecutionConstants.def` 对照
1,411 个 opcode/verifier boundary case。
`SBFOfficialELFMutations.def` 是畸形 ELF 的表驱动契约；其总数仍会演进，因此不冻结。
另有独立的 `41-case strict ELF differential`，将完整 strict-v3 mutation matrix 送入
官方 `verify-elf-batch` 与 NeverD；这 41 个 case 不计入 1,411 总数。
`NeverDSBFAgaveConformanceTests` 认证 Firedancer test-vectors 的
`68bb4af40235562e8852fa23d5727e49c2a0b862`，匹配全部 1,955 `sol_compat_elf_loader_v1` 个 loader fixture
（接受 1,399、拒绝 556），并为每个接受的 ELF 比较 `entry_pc`、`text_off`、`text_cnt`、
`rodata_hash` 与 `calldests_hash`。此门禁不运行后续 instruction verifier。

额外的官方执行矩阵单独统计：恰有 508 个 active `(Version,Opcode)` case，另有
58 个 boundary case，共 566 个 exact execution case。它既不替代、也不计入
1,411 个 verifier probe 或 `41-case strict ELF differential`。
Linux Release CI 使用 `--print-pinned-revision`、`--print-test-vectors-revision` 与
`--print-toolchain`，并导出 `NEVERD_SBPF_ORACLE` 和
`NEVERD_AGAVE_CONFORMANCE_ROOT`，因此两个 external gate 都强制执行；普通本地运行
未提供明确 oracle/corpus env 时仍会发现 case，但允许 skip。

`SBF_RUNTIME_VERSION` 让 `RuntimeVersionPolicy::ChainProfile` 按历史 cluster/slot
计算：官方 feature account activation 使最大 ISA 从 V0 依次推进到 V1、V2、V3；
当前仍是 V3。显式 v4 使用 `RuntimeVersionPolicy::UpstreamToolchain` 做离线分析。
当前 10 MiB 上限精确为
`10'485'760` byte；65,536 仅是历史 provenance/test。`SBFFaultCodes.def` 固定
execution fault 的稳定值；`SBFSourceStatuses.def` 单独拥有 generated-source ABI。

10,000 规模 fixture 守护 worklist、function ownership 与 multi-latch，不固定某台机器
的耗时。cluster/account/slot row 支持 `RPC activation audit`，普通测试仍保持
deterministic 与 offline。
