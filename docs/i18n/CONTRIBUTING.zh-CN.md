**语言**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md) | [繁體中文](CONTRIBUTING.zh-TW.md) | [日本語](CONTRIBUTING.ja.md) | [한국어](CONTRIBUTING.ko.md) | [Français](CONTRIBUTING.fr.md) | [Deutsch](CONTRIBUTING.de.md) | [Español](CONTRIBUTING.es.md) | [Italiano](CONTRIBUTING.it.md) | [Русский](CONTRIBUTING.ru.md) | [العربية](CONTRIBUTING.ar.md)

# 为 NeverD 做贡献

NeverD 是一个语义优先的二进制分析项目。有价值的贡献应当目标集中、
让不支持的行为明确失败，并包含能够证明变更契约的最小测试。

开始修改前，请阅读[架构指南](../architecture.zh-CN.md)。测试套件的选择请参考
[测试指南](../testing.zh-CN.md)，产品规划请参考
[路线图](../roadmap/README.zh-CN.md)。

## 前置条件

- 支持递归子模块的 Git
- CMake 3.20 或更高版本
- Ninja
- 支持 C++20 的编译器
- 用于完整跨目标 fixture 集的 Clang 和 LLD（`ld.lld` 与 `lld-link`）

递归子模块提供 NeverD 的 LLVM fork、Capstone fork、Unicorn 和签名数据。
验证变更时，请勿将它们替换为任意的系统版本。

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
cmake --build build-release --parallel
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
cmake --build build-debug --parallel
```

绝不要使用 Debug 构建报告解码或提升吞吐量。基准测试使用 Release；需要
调试符号进行性能分析时使用 RelWithDebInfo。

### macOS 上的预编译 LLVM

Apple Silicon 贡献者可以避免在本地编译 LLVM fork：

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

CMake 会下载仓库配置的发布包，校验 SHA-256，并在后续构建中复用已解压的
用户缓存。预编译通道仅支持 macOS arm64；Intel Mac 和通用二进制构建必须
使用默认的本地 LLVM 构建。`NEVERD_LLVM_PREBUILT_TAG`、镜像 URL、缓存目录
和显式校验和等高级覆盖项记录在 `cmake/NeverDLLVMPrebuilt.cmake` 中。

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

## 代码风格

C 和 C++ 遵循 LLVM 编码约定，以 `.clang-format` 作为仓库的格式权威。
只格式化你修改的文件：

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

不要为一个局部修复执行全仓库格式化。遵循周边文件的命名和拆分方式，将平台特定
行为保留在对应的 loader、lifter 或 backend 边界内，并避免通过纯 C SDK 暴露
内部 C++ 类型。

Markdown 应简洁且可从源码核实。仓库内部文件使用相对链接；当 CLI 行为、公共
API、支持声明、构建选项或测试命令发生变化时，在同一个拉取请求中更新文档。

## 运行测试

通过聚合目标运行所有已注册测试：

```bash
cmake --build build-release --target check-neverd
```

开发期间使用最小的相关目标或 CTest 标签：

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

[测试指南](../testing.zh-CN.md)记录了全部便捷目标、仅标签的变换套件、单测试
正则表达式、fixture 编译和 Unicorn 往返。如果某个目标因缺少交叉编译器或
链接器而跳过，请报告这一限制；不要把未执行的路径描述为通过。

## 拉取请求检查清单

请求审查前：

- 按维护者偏好的流程 rebase 或 merge 最新 `dev`，并有意处理子模块变更。
- 以 Release 构建受影响目标；如需其他配置，请说明原因。
- 运行精确的回归测试和实际可行的最广相关套件；在 PR 描述中列出准确命令和
  所有跳过项。
- 保持严格提升：不支持的指令不得静默变为猜测操作或 `NOP`。
- 行为变更应增加语义覆盖，而不只是文本 IR 快照。
- 差异中不得包含无关清理、生成文件或本地构建产物。
- 当行为、支持范围、选项、命令或测试所有权变化时，更新公共文档和贡献者文档。

对不应以公开拉取请求开始的安全敏感报告，请遵循
[SECURITY.md](../../SECURITY.md)。
