**语言**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.md) | [繁體中文](../zh-TW/CONTRIBUTING.md) | [日本語](../ja/CONTRIBUTING.md) | [한국어](../ko/CONTRIBUTING.md) | [Français](../fr/CONTRIBUTING.md) | [Deutsch](../de/CONTRIBUTING.md) | [Español](../es/CONTRIBUTING.md) | [Italiano](../it/CONTRIBUTING.md) | [Русский](../ru/CONTRIBUTING.md) | [العربية](../ar/CONTRIBUTING.md)

<!-- i18n-source: af6c34901bacab00e5d1ed68b031f0a48a3ff64ac85ee208695647ec935b9790 -->

# 为 NeverD 做贡献

NeverD 是一个语义优先的二进制分析项目。有价值的贡献应当目标集中、
让不支持的行为明确失败，并包含能够证明变更契约的最小测试。

引入现有代码时，须保留版权、许可证和来源声明，包括借助 AI/LLM 工具复制或改编的代码。
请遵循[署名与引用指南](ATTRIBUTION.md)，注明复用内容的原始来源和版本，
并在提交前检查其许可证。第三方声明应按其原有许可证保留。

开始修改前，请阅读[架构指南](architecture.md)。测试套件的选择请参考
[测试指南](testing.md)，产品规划请参考
[路线图](roadmap.md)。

<!-- i18n-section: prerequisites -->
## 前置条件

- 支持递归子模块的 Git
- CMake 3.20 或更高版本
- Ninja
- 支持 C++20 的编译器
- 启用 Python 插件时，需要 CPython 3.10+ 及其嵌入式开发文件
- 用于完整跨目标 fixture 集的 Clang 和 LLD（`ld.lld` 与 `lld-link`）

递归子模块提供 NeverD 的 LLVM fork、Capstone fork、Unicorn 和签名数据。
验证变更时，请勿将它们替换为任意的系统版本。

<!-- i18n-section: clone -->
## 克隆并初始化

开发成果合入 `dev`，它也是仓库的默认分支。克隆时获取全部子模块：

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

对于已有的克隆，请在首次构建前，以及任何修改子模块记录版本的提交之后，
同步并初始化子模块：

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

<!-- i18n-section: build -->
## 选择构建配置

| 配置 | 用途 | 重要行为 |
|------|------|----------|
| Release | 常规开发、完整测试、解码/提升基准测试 | 已优化；吞吐量具有代表性 |
| RelWithDebInfo | 分析或调试已优化的热点路径 | 已优化并包含调试符号 |
| Debug | 断言、源码级单步调试、局部正确性工作 | 未优化；解码基准会明显变慢 |

除非任务明确需要 Debug 行为，否则使用 Release：

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
python scripts/build.py build-release --parallel
```

默认情况下，构建会将 `third_party/llvm-project` 作为集成依赖一并编译。
首次构建通常需要 30–60 分钟，之后的构建是增量的。`CMakePresets.json`
还定义了 `release`、`relwithdebinfo` 和 `debug` 配置/构建预设；上面使用显式
构建目录，是为了让测试开关清晰可见。

进行源码级调试时，请使用独立目录，不要重新配置 Release 构建树：

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
python scripts/build.py build-debug --parallel
```

绝不要使用 Debug 构建报告解码或提升吞吐量。基准测试使用 Release；需要
调试符号进行性能分析时使用 RelWithDebInfo。

<!-- i18n-section: allocator -->
### 进程分配器

Linux、macOS 和 Windows MSVC/clang-cl 构建默认启用 `NEVERD_ENABLE_MIMALLOC`。
通过公共 CMake 辅助函数创建的工具、分析 worker 和测试使用 NeverSight mimalloc fork
的默认分支，遵循 NeverC 的宿主分配器方案。Unix 在每个可执行文件中嵌入一次分配器；
Windows 使用一个 mimalloc DLL 和对应架构的 `mimalloc-redirect*.dll` 重定向动态 CRT
分配，覆盖 libneverd 和原生插件。两个 DLL 都放在可执行文件旁并安装至 `bin`，发布时一并分发。
Windows 要求 `/MD` 或 `/MDd`，静态 CRT 构建必须关闭 mimalloc。SDK 库本身不强制嵌入
应用使用某个分配器。Windows ARM64 应使用原生 ARM64 构建；x64 分配器 DLL 无法重定向
x64 仿真所用的 ARM64EC CRT。MinGW 保留系统分配器。参见
[上游 Windows 要求](https://github.com/NeverSight/mimalloc/blob/main3/bin/readme.md)。

使用 `-DNEVERD_ENABLE_MIMALLOC=OFF` 获取系统分配器基线或使用外部分配器；旧选项
`NEVERD_WORKER_MIMALLOC=OFF` 仅关闭 worker 的分配器。CMake 会在 address、thread、
memory、leak sanitizer 构建中自动禁用 mimalloc，包括组合及特定配置的编译/链接标志；
仅启用 UBSan 仍兼容。只通过目标专用标志启用 sanitizer 时，也须显式设为 OFF。
Windows 可通过 `MIMALLOC_DISABLE_REDIRECT=1` 不重新构建就进行运行时基线比较，
`MIMALLOC_VERBOSE=1` 显示激活情况。

支持的构建入口 `python scripts/build.py BUILD_DIR` 每次构建前刷新五个 NeverSight fork
的远端默认分支。它们与 Capstone、Unicorn 一样是 `third_party` 下的 Git 子模块。
脚本解析实际分支、更新这五个子模块，并将已提交源码导出至
`BUILD_DIR/third_party/<library>/<commit>/src`。未提交或已分叉的本地子模块工作会阻止构建，
不会被覆盖；网络失败也会停止构建。刷新、重新配置和编译全程锁定构建目录，直到下次调用前
使用同一源码快照，因此增量构建也会采用新提交。

`neverd-dependency-inputs.json` 记录配置输入、提交及导出内容的 SHA-256，随安装产物分发，
不作为二进制认证。`neverd-build-receipt.json` 仅记录所请求目标和配置的成功或失败，CI 保留
两者。安装拒绝失败构建、已变输入及未构建配置；产品 CI 先构建全部安装目标再测试安装。
主线可能在两次运行间变化，跨平台资格验证前先比较提交。直接 `cmake --build` 检查新鲜度，
拒绝过期快照；使用 Python 入口刷新，其自动重新生成仍保持同一快照。
`FETCHCONTENT_SOURCE_DIR_NEVERD_*` 仅供隔离的本地 fixture 覆盖，主线入口会拒绝它。
多配置生成器须显式传入 `--config`。通过入口编译目标，再分别运行测试和 `cmake --install`；
直接传入 `install` 目标会在编译前被拒绝。

冒烟测试和性能比较见[分配器验证](../testing.md#allocator-validation)。

<!-- i18n-section: acceleration -->
### 加速库

常规构建启用 Highway 字节扫描、simdutf UTF-8 校验、simdjson 传输解析和 oneTBB 流水线
调度，保留进程分配器。JSON 继续使用 nlohmann DOM 和序列化；合法的纯整数帧直接从 SIMD
tape 复制，小数和负零沿用原数值转换，超出 simdjson 范围的输入走原准入路径。无效外部文本
保持按字节保真的转义。这些库与 mimalloc 来自 `third_party/forks.json` 列出的 NeverSight
fork，兼容性修复在各默认分支上；构建复制当前源码许可证并记录准确提交，不将这五个依赖
固定在旧发行版。

`NEVERD_ENABLE_HIGHWAY`、`NEVERD_ENABLE_SIMDUTF`、`NEVERD_ENABLE_SIMDJSON`、
`NEVERD_ENABLE_TBB` 默认均为 `ON`，可分别设为 `OFF` 进行比较。oneTBB 在批次间保留
worker，检查其 8 MiB 栈，绝不构建或加载 `tbbmalloc` 及其分配器代理。大栈调用方复用可用
栈空间，小栈调用方获得原生协调线程。首次使用后 fork 或动态卸载 SDK 的宿主必须使用原生
线程基线 `NEVERD_ENABLE_TBB=OFF`，保留调度器不支持这些生命周期。oneTBB 静态链接到共享
核心中，其符号仅供该核心内部使用。CLI 组件调用与 GUI worker 请求使用同一个调度器；
不分发 TBB DLL/共享库。链接 `neverd_shared` 的目标会自动选择共享调度器。
通过其他目标访问 SDK、同时链接组件归档库的客户端，必须对最终目标使用
`neverd_use_shared_task_scheduler`。独立的组件工具保留各自的运行时。
这一限定范围的集成避免了同一进程中存在多个 NeverD 调度器；上游仍将
[oneTBB 静态构建](https://uxlfoundation.github.io/oneTBB/main/intro/static_linking.html)
列为不受支持，并不建议将其用于一般用途。

原生测试、关闭选项比较及调度器兼容范围见[加速验证](../testing.md#acceleration-libraries)。

<!-- i18n-section: prebuilt-llvm -->
### macOS 上的预编译 LLVM

Apple Silicon 贡献者可以避免在本地编译 LLVM fork：

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
python scripts/build.py build-release --parallel
```

CMake 会下载仓库配置的发布包，校验 SHA-256，并在后续构建中复用已解压的
用户缓存。预编译通道仅支持 macOS arm64；Intel Mac 和通用二进制构建必须
使用默认的本地 LLVM 构建。`NEVERD_LLVM_PREBUILT_TAG`、镜像 URL、缓存目录
和显式校验和等高级覆盖项记录在 `cmake/NeverDLLVMPrebuilt.cmake` 中。

LLVM 修复应在 [NeverSight LLVM fork](https://github.com/NeverSight/llvm-project) 中维护。
先将修复提交并合并到 fork，再更新 NeverD 的 LLVM 子模块版本。
集成构建直接编译该源码，不要在 NeverD 中添加 LLVM patch 文件或构建目录源码覆盖层。
预编译包需要从修正后的 fork 重新构建，才能验证相同的行为。

<!-- i18n-section: workflow -->
## 分支与拉取请求流程

从最新的 `dev` 开始，并创建目标集中的主题分支：

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

拉取请求应面向 `dev`，不要假定某个发布分支。保持提交便于审查：每次提交只做
一件连贯的事，不包含生成的构建输出和无关格式化；除非提案本身需要，否则不要
修改子模块版本。

<!-- i18n-section: code-style -->
## 代码风格

C 和 C++ 遵循 [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html)，
以 [`.clang-format`](../../.clang-format) 为格式权威，
[`.editorconfig`](../../.editorconfig) 负责编码、换行和空白。
C++ 版本以 `CMakeLists.txt` 为准，上游风格文档不决定项目语言版本。只格式化修改的文件：

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

CI 使用 [`scripts/check_clang_format.py`](../../scripts/check_clang_format.py) 固定的版本；
复现结果时使用同一版本。格式检查不会判断头文件归属，也不会识别策略裸数字。
局部修复不进行全仓库格式化。遵循周边命名和拆分方式，平台行为留在对应 loader/lifter/backend
边界内，不通过纯 C SDK 暴露内部 C++ 类型。

<!-- i18n-section: headers -->
### 源文件与头文件布局

- 新 C++ 源文件和头文件使用 LLVM 文件名/用途横幅、`NeverD Decompiler` 和 Doxygen
  `\file` 描述，参照 `lib/debug/PDBLoader.cpp`。保留版权和许可证声明。
- 自包含头使用 `.h`，显式包含依赖；实现首先包含对应接口。其余由 `.clang-format` 排序：
  同目录私有头、`neverd/` 公共头、引号形式 LLVM/Capstone 头、尖括号头，不依赖偶然顺序或传递包含。
- 新头使用路径推导的 `NEVERD_..._H` guard，如 `neverd/sdk/Limits.h` 对应
  `NEVERD_SDK_LIMITS_H`；私有/工具头保留足够目录信息避免重名，结尾 `#endif` 注明 guard。
  明确整理旧头时使用此布局，不顺带修改无关头。
- 共享接口位于 `include/neverd/<component>/`，私有实现头放在 `lib/` 或 `tools/` 源码旁。
  位置不等于稳定公共 C SDK API。依赖无环，核心模型/reader 不依赖会话或传输适配层；
  模块算法归所属目录，CLI、SDK、worker 不重复语义。参见[架构](architecture.md)。
- 使用所属命名空间及清晰名称。新类型/字段用 `UpperCamelCase`，函数用 `lowerCamelCase`，
  策略常量用 `kUpperCamelCase`。扩展接口遵循周边命名，保留公共 C/Python 合同和既有 STL
  接口拼写。头文件不加入命名空间级 `using namespace`。
- 有意多次包含的表沿用 `.def`/`.inc` 并说明宏合同；它们不是独立 `.h` 接口，不添加阻止
  重复展开的 guard。生成头通过生成器修改。

独立策略头文件示例：

```cpp
//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include <cstdint>

namespace neverd::sdk {

/// Traversal ceiling for a function without a recorded instruction extent.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
```

<!-- i18n-section: policy -->
### Limits、默认值与配置归属

直接加入头文件搜索路径的工具目录使用 `ResourceLimits.h`。在不区分大小写的文件系统上，本地 `Limits.h` 会遮蔽系统 `<limits.h>`。

每个策略在所属模块的 limits/configuration 头中只有一个权威定义，即使只有一个调用方。
包括输入/输出上限、递归/图边界、工作/迭代预算、启发阈值、缓存、分页/批次、重试及超时；
同样适用于 `constexpr`、宏、裸比较、`min`/`clamp` 参数、构造/字段初始化和默认参数。

先复用已有归属：

| 策略归属 | 定义位置 |
| --- | --- |
| 共享 core/pipeline | [`include/neverd/Limits.h`](../../include/neverd/Limits.h) |
| SDK | [`include/neverd/sdk/Limits.h`](../../include/neverd/sdk/Limits.h) |
| Web | [`include/neverd/web/Limits.h`](../../include/neverd/web/Limits.h) 或已有专门定义 |
| 其他库模块 | 已有策略头/定义表，否则在所属层建立专门 `Limits.h` |
| 仅 CLI、worker、GUI | 工具内 `ResourceLimits.h` 或 `Config.h`；core 不依赖工具头 |
| 调用方可选设置 | 所属 `Config.h`、`*Options.h` 或 `*Configuration.h` 模型 |

策略头保持轻量、按用途分组。已有 `*Limits.def` 或配置表负责时复用，通过自包含头暴露 C++
视图。不要在 `Limits.h`、`Constants.h`、`Config.h` 重复定义同一策略；共享 core 策略不放
SDK/工具头，无关模块策略不全部塞进根 `Limits.h`。

C++ 标量使用有类型的 `inline constexpr` 及既有命名空间（`neverd::limits`、`neverd::sdk`）。
名称说明用途和单位，如 `kMaxSourceBytes`、`kDefaultPageRows`、`kRetryDelayMilliseconds`；
说明零/哨兵含义和特殊边界。默认值与硬上限意义不同则分开。Options 模型可直接拥有默认值，
调用方复用模型/默认工厂或同一常量；有关联的默认值和上限从共同定义推导，不重复裸数字。

适配层、持久化校验、CLI/GUI/worker、Python/MCP 绑定复用权威定义，schema 上限及含限值
诊断也从同一值派生。跨语言值由既有表/生成绑定派生，不能生成时添加漂移检查，并保持纯 C
头兼容。前端预览/分页可比引擎更严格，但单独命名和归属；数值相等不代表同一策略。

二进制布局、ABI/协议/指令常量、整数范围、具体证明算法的计费项保留在语义模型。状态初值、
索引运算、分配提示、独立测试输入不自动属于策略；不可用被测实现常量替代测试独立预期边界。

审查定义及裸值使用，包括跨行保护、循环、宏、默认值、计时器、绑定；核实每个新增/改动策略
的含义和归属，仅搜 `Max*` 不够。集中定义保持类型、值、比较方向、哨兵及耗尽/错误行为；
策略变化分开处理，并运行[测试指南](testing.md)要求的受影响检查。

<!-- i18n-section: artifacts -->
### 文档与本机产物

Markdown 应简洁且可从源码核实。内部文件用相对链接，CLI、公共 API、支持范围、构建选项、
测试命令变化时同一 PR 更新文档。修改本指南时同步所有支持语言；先审阅译文，再更新
`i18n-source` SHA-256，保持章节、示例、API/选项和链接目标一致。运行
`python3 scripts/check_docs_i18n.py` 检查链接与翻译同步。

本机性能报告、原始追踪和耗时汇总保存在忽略的构建或 benchmark-results 目录。提交可复用
测量工具和回归测试，不提交本机报告或复制的结果表。

<!-- i18n-section: tests -->
## 运行测试

通过聚合目标运行所有已注册测试：

```bash
python scripts/build.py build-release --parallel
cmake --build build-release --target check-neverd
```

开发期间使用最小的相关目标或 CTest 标签：

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
python scripts/build.py build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

[测试指南](testing.md)记录了全部便捷目标、仅标签的变换套件、单测试
正则表达式、fixture 编译和 Unicorn 往返。如果某个目标因缺少交叉编译器或
链接器而跳过，请报告这一限制；不要把未执行的路径描述为通过。

<!-- i18n-section: python-sdk -->
### Python 插件 SDK

修改 Python 插件 SDK 时，先运行纯 Python 和漂移检查，再运行原生适配层测试：

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
python scripts/build.py build-release --target \
  NeverDPluginRuntimeTests NeverDPythonRuntimeTests NeverDPythonPluginTests
ctest --test-dir build-release -R 'NeverD(Python|Plugin)' --output-on-failure
```

公共 C 声明、Python ABI 签名、包/CMake 版本和交付工作流策略属于同一审查合同，同次变更
一起更新。参见 [Python 插件指南](python-plugins.md)。

<!-- i18n-section: review -->
## 拉取请求检查清单

请求审查前：

- 按维护者偏好的流程 rebase 或 merge 最新 `dev`，并有意处理子模块变更。
- 以 Release 构建受影响目标；如需其他配置，请说明原因。
- 运行精确的回归测试和实际可行的最广相关套件；在 PR 描述中列出准确命令和
  所有跳过项。
- 保持严格提升：不支持的指令不得静默变为猜测操作或 `NOP`。
- 行为变更应增加语义覆盖，而不只是文本 IR 快照。
- 差异中不得包含无关清理、生成文件或本地构建产物。
- 检查[头文件与策略归属规范](#代码风格)，包括裸数字保护/默认值以及绑定漂移；
  仅通过格式化检查还不够。
- 当行为、支持范围、选项、命令或测试所有权变化时，更新公共文档和贡献者文档。

对不应以公开拉取请求开始的安全敏感报告，请遵循
[SECURITY.md](../../SECURITY.md)。
