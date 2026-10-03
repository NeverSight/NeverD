# macOS 原生 CPU 后端（HVF）

NeverD 使用 Apple 的 Hypervisor.framework，后端名为 `hvf`，对应 Linux 的 KVM 和 Windows 的 WHP。按本机架构执行：Apple Silicon 使用 ARM64，Intel Mac 使用 x86-64。对支持原生执行的契约，`auto` 在主客体架构匹配时选择 HVF；跨架构以及 `software-cpu-v1` 契约继续使用 Unicorn。Rosetta 下的翻译进程会明确拒绝初始化，应改用原生 arm64 构建。

启用 `NEVERD_ENABLE_CPU_EMULATION=ON` 或驱动模拟后，`NEVERD_EMULATION_BACKEND_HVF` 默认开启。关闭该选项仍保留 `hvf` 配置名称，但能力查询报告构建未启用。原生后端初始化失败时不会静默回退。传输层要求 macOS 11 及硬件虚拟化支持；其他依赖仍可能要求更新的系统。

真正调用框架的**进程可执行文件**必须具有 `com.apple.security.hypervisor` entitlement。CMake 在链接后为 CLI、worker 和模拟测试签名；默认使用 ad-hoc，可通过 `NEVERD_HVF_SIGN_IDENTITY` 指定已有签名身份。打包脚本在修改 Mach-O 依赖后重新签名 worker，并检查最终权限没有丢失。嵌入式 SDK 使用方应签署自己的宿主程序；NeverD 不会改签已安装的 Python 解释器。

独立构建 worker 或桌面程序时，导入的引擎库不提供其构建选项，因此 worker 默认仍会附加该 entitlement。若明确不需要 HVF，可设 `NEVERD_WORKER_SIGN_HVF=OFF`。

实现复用现有 checked 契约、页表、寄存器模型和 RAM 事务。一个专用线程拥有进程内的 VM/vCPU，各逻辑 CPU 串行使用它；切换前退休旧映射，销毁时先解绑再释放 backing。Apple Silicon 的宿主映射按 16 KiB 对齐，来宾的权限和内存预算仍以 4 KiB 为单位。ARM64 在每条来宾指令前单步执行 TLB/I-cache 维护，并完整传输标量、TLS 和 FP/SIMD。Intel 使用 VMCS、MTF、XSAVE 和处理器异常退出。RIP/RFLAGS 每次均直接通过 VMCS 传输，也覆盖取消后重建 vCPU 的入口。Intel 的 CR0/CR4 同时遵守框架可写掩码和硬件必置位，用读取影子保持来宾可见状态。CR8 使用 VMX 访问退出及架构层的读取完成逻辑，因为宿主 TPR 接口可能与来宾实际状态不一致；只处理经过硬件认证的 CR8 读取，其他控制寄存器访问明确失败，checked 指令准入范围不变。每次创建或取消后重建 Intel vCPU 都初始化独立的托管 `IA32_KERNEL_GS_BASE` 上下文，来宾 MSR 访问仍陷出；未支持的 MSR/SWAPGS 指令不会进入硬件。两种架构都必须通过现有完整状态启动探针。

取消会确认原生中断已结束，再允许下一个任务进入；被取消的原生入口会重建 vCPU，防止旧中断影响后续任务。排队期间也检查停止令牌和期限。真正的宿主或状态读取错误优先保留，取消的普通 CPU/RAM 状态不提交。

实际硬件门禁及完整构建命令见[英文实现说明](../macos-hvf.md)。核心命令为：

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf \
  --evidence build-hvf/native-evidence --require-hvf
```

门禁要求 HVF 用例实际通过，不能用全部跳过代替成功；支持关闭 Unicorn 后独立运行。测试包含跨线程调用、多个 CPU 的同地址隔离、权限与跨页访存、完整状态、启动探针对其他 CPU 的影响、部分映射失败回滚、排队取消、两种架构的原生死循环中断及重试。中断用例必须观察到实际原生返回，进入前取消不能算通过。当前 transport 门禁要求 ARM64 12 项、Intel 10 项；完整门禁分别要求 16 项和 14 项，Intel 包含 CR8 全部目标寄存器及权限回归。Intel 的交叉编译只能证明编译通过，仍需 Intel 真机执行门禁。

手动工作流默认使用带 `hvf` 标签的自托管宿主，也可选择 `hosted-intel` 尝试
GitHub 的 `macos-15-intel`。两种方式都先编译、签名并运行 `scripts/probe_hvf_host.c`，
实际创建和销毁 VM/vCPU，成功后才准备 LLVM。这项可用性探针不执行来宾指令。
宿主拒绝 HVF 就在此处失败，不能用 runner 名称推断硬件可用。
CPU 门禁通过后，还必须执行本机架构的全部 Darwin 工作负载。

GitHub 的[宿主策略](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners)
把 runner 内的嵌套虚拟化列为实验性用途，不保证稳定性、性能或兼容性。
因此需保留专用原生 Mac 的持续验收路径；这条平台限制不能直接说明某次停滞的根因。

工作流会先构建只依赖 LLVM Support 和解码器的 `NeverDHvfTests`，验证原生指令执行，
再构建完整进程测试的依赖。`validation=transport` 可单独运行这一诊断；默认 `full`
仍要求完整 CPU 和 Darwin 两道门禁。`validation=darwin` 只构建进程测试所需目标，
可独立要求本机架构的全部 Darwin 工作负载通过。小范围检查复用完整清单中的 HVF 必需项，并在
摘要中标记 `hvf_transport_only=true`，不能当作完整 CPU/进程验收。
本地脚本对应参数为 `--require-hvf --hvf-transport-only`。
`validation=probe` 只检查 VM/vCPU 可用性，无需 LLVM，不能作为指令执行或
NeverD 传输层正确性的证据。

硬件虚拟化不保证在逐指令 checked 执行中更快；初始化、完整状态传输和缓存维护都有成本，应与相同 checked 契约的 Unicorn 比较。该后端不增加新的跨架构模拟方案或来宾 OS 模型。

## 本次验证记录（2026-10-02 至 2026-10-03）

验证主机为 Apple M4 Max、macOS 15.6.1，使用 macOS 15.5 SDK 和 Release 构建。

| 验证范围 | 结果 |
| --- | --- |
| CPU、进程执行及公开 API 回归，包含 Unicorn 对照 | 2,304 通过、0 失败；4,205 个不适用的平台、架构或后端参数项跳过 |
| 关闭 Unicorn 的原生硬件门禁 | 扩大到 19 个测试目标后，753 通过、0 失败；5,652 个不适用或禁用的参数项跳过；9 个必需用例均实际执行 |
| 关闭 HVF 的配置测试 | 2 通过，5 个硬件用例按预期跳过，报告 `build_disabled` |
| 同时关闭测试与 Unicorn | CLI 构建成功，HVF 初始化与 ARM64 ELF 进程执行通过 |
| 打包和硬件验收脚本单元测试 | 分别 10 项和 17 项通过 |
| 实际进程和签名检查 | 原生/自动选择成功；异架构报告 `host_isa_mismatch`；缺少 entitlement 报告 `device_access`；独立 worker 与完整桌面包签名检查通过 |
| Intel 分支 | 原生 transport 及 253 项异常/状态/除法检查通过；完整 CPU 与 Darwin 门禁仍待结果（见下文） |

格式检查使用仓库指定的 clang-format 22.1.2；文档检查覆盖 231 个文件和 10 个语言目录。能力清单与 Python SDK 审计通过，CI 验收脚本相关的 92 项测试通过。

完整 Qt 6.11.1 桌面包通过了 186 个 Mach-O 文件的依赖及签名检查，最终 worker 保留 HVF entitlement。包内 GUI 的 Cocoa 启动检查和 worker 的 EVM 加载、反汇编通过；另由已签名探针直接加载包内 `libneverd.dylib`，完成 HVF 初始化及 ARM64 ELF 执行。这个包的依赖决定最低系统为 macOS 15.0；框架适配层的 macOS 11 API 基线不代表整个桌面包的最低系统。

硬件门禁还覆盖 Linux/Windows 进程、ABI、会话和预算。指定 `NEVERD_REQUIRE_HVF=1` 时，本机匹配架构的这些用例遇到不可用后端或缺失 fixture 会失败。实际移除测试进程 entitlement 后，普通运行跳过而硬件门禁失败，已验证两种行为。原生验收机需要 Clang、`ld.lld`、`lld-link` 和 `ld64.lld` 来构建原始 ELF/PE/Mach-O fixtures。

添加 [macOS/iOS 进程环境](darwin-emulation.md) 后，无 Unicorn 门禁扩大到 20 个目标：
提交 `36e11ca8a` 已达到 834 项通过、0 失败，5,903 项因后端或架构不适用而跳过；
ARM64 门禁的 13 个必需用例均执行（9 个 HVF 检查、3 个 Darwin 平台启动用例及
1 个原始程序的宿主内核对照）。Intel 门禁要求 macOS 与 Simulator 两种平台。
这些用例缺失或跳过均不能通过；CTest 使用稳定名称，不把参数中的进程地址纳入测试身份。
其中包含 65 项 Darwin 检查，三个 ARM64 平台共 39 项必需进程用例全部执行成功。

Darwin 扩展后的新桌面包再次通过 186 个 Mach-O 的依赖、签名及 Cocoa 启动检查。
已签名探针加载包内引擎，并与 `BUILD_TESTING=OFF` / Unicorn OFF 的 CLI 比较三种
ARM64 平台下的正常、部分输出、匿名内存、故障、未知服务和指令预算，共 18 份报告完全一致，
实际后端均为 HVF；两种 x64 Mach-O 的显式 HVF 请求在此 ARM64 宿主上明确拒绝。

收尾检查还修正了裸中断测试中的指令缓存干扰：它原先在同一地址交替写入循环与 `HVC`，可能误执行缓存中的旧指令。改用两段固定代码后，8 路并发重复运行 1,000 次全部通过；随后 36 项 HVF、配置和公开 API 检查全部通过。生产 ARM64 路径仍执行完整的来宾缓存维护。

后续集成验证已修复测试 SDK 缺少 `neverd_session_set_load_progress` 和测试客户端把
`status=progress` 当成最终响应的问题，并增加进度顺序、计数及文件身份的断言。
独立 worker 的 8 项检查通过，其中包括 3 项真实引擎集成；完整 Qt/IPC/MCP
测试夹具构建在 macOS 上的 19 项检查也全部通过。
提交 `e078b129c` 的[桌面 GUI 工作流](https://github.com/NeverSight/NeverD/actions/runs/37053518872)
也在 macOS、Windows、Ubuntu 三个平台全部通过，验证了测试夹具和 worker 传输修复。

同机微基准使用 `checked-aarch64-v1`，运行两条初始化指令和 1,000 次 `ADD/SUBS/B.NE` 循环，共 3,002 条指令。创建 CPU 与启动探针不计时；预热后交替运行两个后端，各取 7 次中位数，并校验最终寄存器与 PC。Unicorn 为 **73.9 ms**，HVF 为 **95.1 ms**，HVF 耗时约多 **29%**。这是短整数循环的结果，不能代表其他负载；当前版本尚未证明性能提升。

随后补充了下列测量。宿主共享负载约为 30，HVF 的耗时有很大波动；此表保留受干扰的实际结果，不能与上一轮的绝对耗时直接比较。每项预热后取 7 次中位数；硬件入口数在测试程序内拦截并转发真实 `hv_vcpu_run` 计数，未修改后端。

| 场景 | 来宾指令数 | Unicorn 中位 ms | HVF 中位 ms | HVF 最小–最大 ms | HVF 入口数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 后端创建与启动探针 | — | 0.729 | 9.832 | 4.510–13.966 | 18 |
| 整数循环 | 3,002 | 160.160 | 2,327.342 | 1,650.271–4,907.726 | 18,012 |
| 条件分支 | 5,503 | 153.081 | 203.819 | 184.760–7,934.218 | 33,018 |
| 内存读写 | 5,002 | 141.085 | 173.748 | 160.369–7,851.044 | 30,012 |
| TLS 与函数调用 | 7,003 | 345.838 | 5,995.233 | 5,091.529–10,846.128 | 42,018 |
| 两个 CPU 交替执行 | 512 | 23.098 | 301.252 | 44.290–549.546 | 3,072 |
| Linux 进程 CLI（包含启动及初始化） | 685 | 141.420 | 971.306 | 65.619–1,431.597 | 未在此进程内计数 |

普通 ARM64 指令需要 5 次维护入口加 1 次来宾入口，与上述实测计数一致。所有 CPU 场景校验寄存器、PC、访存和执行次数；Linux 示例对照完整报告，仅排除后端名称和选择说明，其正常结果以及故障、未知服务、预算停止均一致。空闲主机上的稳定性能复测仍待完成；本轮未加入批量执行或修改观测语义。

Linux KVM 和 Windows WHP 已分别在关闭 Unicorn 后通过 Darwin 专项门禁：每个后端
51 项通过、0 失败，其中 26 项 x64 必需进程用例全部执行成功。源码提交、产物和
验证范围见 [Darwin 宿主验证记录](darwin-emulation.md)。这证明了有限 Darwin 环境
及其共享 CPU 路径，不能替代各后端更广泛的 CPU 回归。

干净源码 `48042a5e90e0977585114de092e423cd64b7f95f` 的 ARM64 完整门禁达到
841 项通过、0 失败、5,942 项跳过，包含加强后的中断清单，16 个必需用例全部执行。
其中 12 项 transport 子集也已在本机全部通过，无跳过。完整证据保存在
`build-hvf-native/hvf-cr8-full-arm-evidence/`。

Intel 诊断已移除框架拥有的 VMCS link pointer 写入、保留框架 VM-exit 控制，
并补全硬件 CR0/CR4 必置位。[独立 64 位程序](https://github.com/NeverSight/NeverD/actions/runs/37076076219)
已证明该 hosted runner 能通过 QEMU HVF 执行。随后逐个隔离 12 个 MSR，
[原生对照](https://github.com/NeverSight/NeverD/actions/runs/37078094659)
确认只有启用 `IA32_KERNEL_GS_BASE` 上下文能恢复执行和 MTF 单步。
VM 指令错误 12 在成功执行前后同样存在，不能将这个残留值视为当前入口的失败原因。
生产后端已在每次创建 vCPU 时初始化托管 kernel-GS 上下文。

随后，[独立 CR8 对照](https://github.com/NeverSight/NeverD/actions/runs/37082402190)
在 `e61928b8c` 上定位到另一处 TPR 同步差异：来宾自行写入后能读回优先级
0、1、3、15，宿主 TPR 读回仍是零；宿主 TPR/APIC 写入也未正确改变来宾读值。
生产提交 `48042a5e9` 使用经过硬件认证的 CR8 读取退出；普通宿主中断在同一
取消周期内重试，不当作指令完成。新增原生用例覆盖全部 16 个目标寄存器、
用户态 #GP，以及主动取消前的普通宿主中断。
最初的取消恢复回归发现，重建 vCPU 后首次重试的 VMCS RFLAGS 为零。
`99340b586` 改为直接通过 VMCS 安装和捕获 RIP/RFLAGS。
干净源码 `19a5f63a239bf1afac892f6907a72f62e8e998c1` 的
[原生 transport 门禁](https://github.com/NeverSight/NeverD/actions/runs/37086427775)
已达到 10/10 项通过、零跳过。独立单进程与取消检查也通过，三种中断模式
重试前后的 RFLAGS 均为 `0x202`，下载产物的 SHA-256 已与 GitHub 元数据核对。
完整 Intel CPU/Darwin 验收仍待完成；`9319c880d` 的完整任务通过了 100 次中断恢复
和 transport，但合并的构建/执行阶段超过 100 分钟没有返回完整结果，已请求取消。
14 个必需项仍需完整门禁确认。停滞或取消的任务不作为成功证据。
临时指令探针、API 拦截器和阶段日志已从交付源码清理，持续验收由实际测试负责。

干净源码 `9319c880d78e93f5cb8a7a9360778084934de258` 的 ARM64 transport 再次
达到 12/12 项通过、零跳过；原生死循环中断与复跑也通过 100 次重复，无失败或跳过。

Intel 工作流还会在完整依赖构建前构建 `NeverDX64ExceptionTests`，提前验证 CR8 状态与权限异常。
完整异常/状态套件由最终 CPU 门禁执行，前置不再重复整组检查。
transport 结果与状态测试清单在执行前上传，CR8 独立结果在完整 CPU 门禁前另行保存；
产物名称包含运行次数，重跑会保留各次证据。
完整目标构建有独立步骤和日志；`test_parallel` 默认使用 4 个测试进程，也可选择 1
做串行对照，编译仍并行进行。结果摘要记录实际选择的并发数、宿主 OS/内核描述和逻辑 CPU 数。

干净源码 `e2a91ff057df563eb19183a045a3ad6446cb9af1` 的
[Intel 检查点任务](https://github.com/NeverSight/NeverD/actions/runs/37090528761)
再次通过全部 100 次中断恢复和 10 项 transport，无跳过。
独立 CR8 回归也通过，涵盖 16 个目标寄存器、CPL3 权限异常、resume flag 行为和完整状态保留。
其 JUnit 与 transport 产物均已下载并核对 SHA-256。状态目标成功编译、注册 952 项；
这些检查点尚不能证明整组测试已经完成。

干净源码 `5251cc68591dc343c954c8b7a9b57fa5cb9190e8` 的
[状态切换组](https://github.com/NeverSight/NeverD/actions/runs/37091386132)
在单个进程中通过了全部 5 项 HVF 检查，10 项其他后端用例跳过。已核对的产物覆盖
TLS/权限、CR8、取消，以及宿主修改、故障和停止前后的完整 x87/SSE 状态。
该任务随后停在产物上传；另一轮串行 transport 与状态测试通过后，硬件异常组没有返回结果。
这些现象尚不能定位具体指令失败，也不能证明是测试进程并发缺陷。
后续独立结果如下；完整 CPU/Darwin 门禁仍单独验收。

干净源码 `908a830e6e3f1bbae6bc7ed7e534f3b13aeb1c1e` 的两个专项均已成功结束：
[硬件异常专项](https://github.com/NeverSight/NeverD/actions/runs/37094333371)
通过全部 120 项原生异常检查，覆盖十种异常及两种权限级、重复恢复、映射变化和私有异常入口完整性。
[除法专项](https://github.com/NeverSight/NeverD/actions/runs/37094335126)
通过全部 128 项原生公开 CPU 检查，覆盖正常结果、终止异常、显式恢复和观察者停止。
两轮都通过了 5 项状态切换、10 项 transport 和 100 次中断恢复；这些共用检查不能重复计数。
两个产物均已下载、核对 SHA-256 并检查逐项 XML。临时隔离工作流入口与步骤已清理。
[完整 Intel 门禁](https://github.com/NeverSight/NeverD/actions/runs/37095689345)
使用 `4c6a12b913123d0555f067035527fe29f856f3b9`，20 个目标均已编译成功，
构建产物 SHA-256 已核对；CPU 阶段超过 30 分钟没有结果后已取消，结束后仍无可下载日志。
这只能证明编译成功，不能作为完整 CPU 验收。
另由 `4cbb729389df9485c9a9699c63c0be3a48862794` 的
[独立 Darwin 门禁](https://github.com/NeverSight/NeverD/actions/runs/37097301977)
要求 Intel 的全部 26 个原生工作负载通过。另有临时
[逐目标诊断](https://github.com/NeverSight/NeverD/actions/runs/37098336208)，
每个完整 CPU 目标结束后立即保存清单、CTest 退出码和 XML，再继续下一组。
两轮均为串行测试，模拟实现和测试与上述专项一致；目前尚未回传完整验收结果。

同步后续 `dev` 改动后，干净源码 `f4bf8dde5cbc33d18ce053cb0722a54047e5a2d0`
再次通过 ARM64 transport 全部 12 项，无跳过；独立 Darwin 门禁 65 项通过、0 失败、
221 项不适用用例跳过，39 个必需原生工作负载全部执行。
新增宿主和并发元数据已在 Apple Silicon 与 Intel 的真实 transport 产物中核对。
本机证据位于 `build-hvf-native/hvf-final-dev-transport-evidence/` 和
`build-hvf-native/hvf-final-dev-darwin-evidence/`。
