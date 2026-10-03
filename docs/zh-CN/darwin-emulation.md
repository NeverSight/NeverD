# macOS / iOS 来宾进程环境

`os/darwin/` 提供共享的 Mach-O 启动、Darwin 系统调用和内存模型；
`macos/`、`ios/` 分别定义平台契约。它们属于来宾 OS 层，HVF 属于宿主 CPU
后端，二者独立选择。

| Profile | 平台 | 架构 |
| --- | --- | --- |
| `macos-macho64-v1` | macOS | x64、ARM64 |
| `ios-macho64-v1` | iOS 设备 | ARM64 |
| `ios-simulator-macho64-v1` | iOS Simulator | x64、ARM64 |

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

本轮范围对齐现有 Linux/Windows 的有限进程模型：原始 Mach-O 字节、数据和 BSS、
段权限、`LC_MAIN` 的参数及返回、规范 `LC_UNIXTHREAD` 的启动栈、显式 argv/envp/apple
向量、退出、标准输出与错误输出、固定身份、匿名内存映射、保护和释放。
ARM64 使用 16 KiB OS 页，x64 使用 4 KiB；CPU 页表仍以 4 KiB 为基础。
`__PAGEZERO` 只保留地址，不消耗数 GiB 内存。
完整输入文件（包括未映射的元数据和尾部字节）在解析和复制前必须符合 `memory_limit`。
加载器只对普通文件做有界读取，拒绝含 NUL 的路径、短读和文件大小变化，解析独立快照，
不保留宿主文件映射。输入文件和来宾映射各有一个同值预算；宿主文件 I/O 没有硬实时保证。
Mach-O 文件尾页保留同页原始字节，后续完整虚拟页清零。初始数据、栈和匿名页都按
OS 页独立持有物理内存，因此部分解除映射能释放预算，重新分配的页面保持清零。

系统调用遵循 Darwin ABI。ARM64 从 X16 读取服务号，x64 使用 BSD 类别前缀；
错误返回正 errno 并置 carry。JSON 中的 `error` 字段区分错误和值相同的成功结果。
不会复用 Linux 的负 errno。内存保护跨空洞或最大权限边界失败时，整段原权限保持不变。
部分 `write` 复制的字节会保留，后续访存错误仍返回 EFAULT。
原始 `write` 长度超过 `INT_MAX` 时，直接返回 EINVAL，检查发生在描述符、来宾指针和
输出预算之前。此顺序已有真实 macOS 系统调用对照。

设备与模拟器的 Mach-O 平台标记必须分别匹配。此版本接受不依赖动态库、重定位、
初始化函数或 TLS 的独立可执行文件。需要 dyld 链接、Mach IPC、线程、文件系统、
Objective-C/Swift 运行时、Foundation/UIKit，或 arm64e/PAC 的输入会明确拒绝；
不会用空实现假装执行成功。它不等同于完整 macOS/iOS 系统，也不是 Apple 的 Simulator。

测试使用自行编写、由 Clang/LLD 生成的五种 Mach-O 平台/架构用例，另有独立构造的
线程入口和畸形文件测试、4 KiB/16 KiB 内存测试，以及 C API/CLI 报告一致性测试。
Python SDK 也通过真实共享库执行五种平台/架构组合。
HVF 必需门禁也包含这些用例。完整支持边界、来源和命令见[英文说明](../darwin-emulation.md)。
Intel HVF 的 10 项原生 transport 检查已全部通过，完整 CPU/Darwin 验收仍待完成；当前状态见
[HVF 验证记录](macos-hvf.md)，不能把内核参考程序成功当作后端通过。

新增的完整原生工作负载门禁要求本机架构的每个 Darwin 进程用例都实际通过：
ARM64 三个平台共 39 项，x64 的 macOS 和 Simulator 共 26 项。
每种平台都必须执行 `LC_MAIN` 和独立编写的 `LC_UNIXTHREAD` 程序；源码清单回归确保
以后新增的 Darwin 进程用例也进入必需集合。
macOS 本机构建还会把同一份自编目标文件链接为宿主参考程序，对照返回、退出、内存保护/
重用和超长写入的错误顺序，检查实际退出状态和逐字节输出。只有宿主程序为真实 dyld
入口链接 libSystem，来宾镜像仍不依赖动态库。这项原生对照是 HVF 门禁必需项，
不代表 iOS 真机执行证据。
独立的[原生内核工作流](../../.github/workflows/darwin-kernel-reference.yml)还会直接在
Intel 与 Apple Silicon macOS 上执行这些程序，无需构建 NeverD 或 LLVM。
`DarwinNativeCases.def` 统一维护 C++ 测试和独立宿主脚本的模式、退出状态及期望输出；
架构不符、Rosetta、超时或结果不符均失败，JSON 保留源码、系统和编译器信息。
`scripts/run_native_cpu_ci.py --require-darwin-backend hvf` 可以在本机验收；
Linux 使用 `kvm`，Windows 使用 `whp`，同时传入 `--build` 和 `--evidence` 路径。
缺少用例注册、跳过必需用例或缺少 `ld64.lld` 均不能通过。
[原生 Darwin 专项工作流](../../.github/workflows/darwin-native.yml)提供关闭 Unicorn
后的 x64 KVM/WHP 构建和验收；它需要 runner 实际提供虚拟化能力，保留完整测试证据。
工作流也可以单独选择 `kvm` 或 `whp`。

## 本机验证（2026-10-03）

Apple M4 Max / macOS 15.6.1，Release 构建，源码为
`36e11ca8a3d80aecf585d3328018839ce7fdb989`：

| 范围 | 通过 | 失败 | 跳过 | 必需原生用例 |
| --- | ---: | ---: | ---: | ---: |
| 完整 HVF 门禁，关闭 Unicorn，20 个目标 | 834 | 0 | 5,903 | 13 / 13 |
| 每个 Darwin 工作负载，关闭 Unicorn | 65 | 0 | 221 | 39 / 39 |
| Darwin 与公开 C API/CLI，包含 Unicorn | 138 | 0 | 156 | — |

统计有重叠，不能相加。原生摘要记录干净源码，没有缺失注册或未执行的必需用例。
跳过项属于异架构、其他后端或关闭的软件后端。65 项 Darwin 检查包含加载器、内存、
每个 ARM64 工作负载，以及真实宿主内核对照。加入原生中断清单后，完整 HVF 门禁
现要求 ARM64 的 16 项或 Intel 的 14 项检查；Darwin 专项门禁另要求全部 39 / 26 项进程工作负载。
后续干净源码 `561ebf37b9eaaec08043ac5816b2e083ecccaf68` 的 ARM64 完整门禁达到
841 项通过、0 失败、5,939 项跳过，16 个必需用例全部执行；完整证据位于
`build-hvf-native/hvf-cancellation-full-evidence/`。

证据位于 `build-hvf-native/hvf-release-evidence/`、
`build-hvf-native/darwin-release-evidence/` 和
`build-hvf/verification/darwin-release-public.xml`。回归覆盖未映射尾部字节的文件预算、
各平台的线程入口、超长写入错误顺序及格式化换行后的测试清单解析。
原生清单、结果核对和 CI 脚本的 106 项回归通过；能力、文档、来源及格式检查也通过。

后续同步 `dev` 后，干净源码 `f4bf8dde5cbc33d18ce053cb0722a54047e5a2d0`
再次通过独立 ARM64 Darwin 门禁：65 项通过、0 失败、221 项跳过，39 个必需原生
工作负载全部执行。证据位于 `build-hvf-native/hvf-final-dev-darwin-evidence/`。
这次复跑验证 Darwin 目标，不代表同期合入的其他 Windows 进程改动已经通过新的完整 CPU 门禁。

较早的集成验证已覆盖五种平台/架构的 Python SDK 调用；包内引擎与关闭 Unicorn 的
CLI 在三种 ARM64 平台的 18 个场景中报告一致。桌面包通过 186 个 Mach-O 的依赖、
签名和 Cocoa 启动检查。同时关闭 HVF 与 Unicorn 的配置通过 38 项检查、跳过 231 项
后端用例，且没有 Hypervisor.framework 依赖；这属于构建隔离证据。
提交 `e078b129c` 的[桌面 GUI 工作流](https://github.com/NeverSight/NeverD/actions/runs/37053518872)
也在 macOS、Windows、Ubuntu 三个平台通过。

## 托管宿主原生验证（2026-10-03）

两个 x64 后端都在关闭 Unicorn 后通过最新 Darwin 专项门禁，包含文件预算、独立线程
入口及超长写入回归。源码均为 `36e11ca8a3d80aecf585d3328018839ce7fdb989`，
macOS 与 iOS Simulator 共 26 项必需进程用例全部执行成功：

| 宿主 / 后端 | 通过 | 失败 | 跳过 | 必需原生用例 |
| --- | ---: | ---: | ---: | ---: |
| [Windows / WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703/job/111023370601) | 51 | 0 | 235 | 26 / 26 |
| [Ubuntu 24.04 / KVM](https://github.com/NeverSight/NeverD/actions/runs/37062839703/job/111023370857) | 51 | 0 | 235 | 26 / 26 |

产物 `darwin-native-whp-x64` 和 `darwin-native-kvm-x64` 保留完整清单、JUnit、CTest
日志及源码/宿主摘要。两个工作区均干净，没有缺失或未执行的必需用例；下载产物的
SHA-256 已与 GitHub 摘要核对。跳过项包括仅适用于 macOS 的内核对照、异架构和其他后端。

这些结果验证所列传输上的有限 Darwin 模型。Intel Mac HVF 和各后端更广泛的 CPU
行为仍需各自的门禁验证。

## 独立 macOS 内核对照（2026-10-03）

提交 `e727d3eab7086063bb392444bd55014ac48d43c3` 的原始程序使用 Apple Clang 17.0.0
编译后，在两种 macOS 15.7.9 宿主上直接执行成功：

| 宿主架构 | 原生工作负载通过 |
| --- | ---: |
| [Apple Silicon ARM64](https://github.com/NeverSight/NeverD/actions/runs/37064795867/job/111029848093) | 4 / 4 |
| [Intel x86-64](https://github.com/NeverSight/NeverD/actions/runs/37064795867/job/111029847805) | 4 / 4 |

两个摘要均记录干净源码，`return`、`exit`、`memory`、`write-length` 都返回 37，
输出逐字节匹配且 stderr 为空；产物摘要也已核对。这为两种架构的原始工作负载提供
独立内核语义依据；模拟执行仍由上面的 HVF/KVM/WHP 结果覆盖。
