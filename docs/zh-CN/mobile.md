# 移动应用逆向支持

[← 文档索引](README.md) · [完整 Android 指南](android.md) · [完整 iOS 指南](ios.md)

`neverd mobile` 支持将 Android APK、DEX、smali 恢复为可读 Java。对 iOS IPA、`.app` 和 Mach-O，命令输出原生 C，将受支持的 Objective-C 方法体恢复为 `.m` 源码，并输出实验性 Swift 源码与运行时元数据。目前这是实验性的 CLI 功能；原生 C SDK 和 GUI 加载器尚不支持移动应用容器。

## 环境

使用支持 C++20 的工具链构建 `neverd` 目标。移动端工作流已编译进原生 CLI，不调用 Python 解释器。分发时携带当前构建所需的原生依赖库。

原生 ZIP 处理使用 zlib 完成 CRC-32 和 DEFLATE。CMake 优先通过 `find_package` 使用已安装的库；缺失时按固定 SHA256 下载 zlib 1.3.2，并静态构建。移动端 ZIP 实现在 Windows 上也不需要 Python 辅助工具。分发时请保留 [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) 中的依赖声明。

默认引擎以 C++20 实现，运行时不需要 Python、Java 或 JADX。只有显式 `--jadx PATH` 才会选择单独安装的兼容适配器；`NEVERD_JADX` 和 PATH 不会自动选择它，也没有自动回退。可选适配器需要带标准 DEX/smali 输入插件的 JADX 1.5.6+ 和 Java 11+，报告记录实际 `jadx` 引擎与版本。安装方式及依赖许可证见 [Android 指南](android.md#可选-jadx-兼容适配器)。

## Android 用法

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

APK 根目录中的 `classes.dex`、`classes2.dex` 等字节码会一起分析。smali 目录会递归收集类，在同一次调用中处理嵌套类和跨类引用；单个 smali 文件仅提供该类的上下文。

内置输出包括 `sources/`、`metadata/android-methods.json` 和 `report.json`，其中 `backend: {"name": "neverd", "version": "1", "execution": "builtin"}`。报告包含 `android_method_recovery`：`method_count = recovered_method_count + declaration_only_method_count`，且发布前 `unrecovered_method_count` 必须为零。原有 `native`、`abstract` 声明与恢复的方法体分开计数。显式外部适配器保留自己的后端日志。此 Java 流程不恢复 APK 资源、Manifest、原生库或动态加载代码；原生库可另用 `neverd decompile` 分析。

内置读取器共用独立实现的带类型 Dalvik 模型和有界 Java 生成器，面向 DEX 035/037–040 与 smali 中可表示的常规代码。DEX 041、`invoke-custom` 等动态调用、部分初始化路径、未知操作和无法用 Java 表示的标识符都会明确失败。生成的 Java 可能使用分派循环，不执行原始 DEX，也不通过运行时桥接调用它。原始注释、排版和已删除名称无法还原。这个实验性引擎不承诺与 JADX 功能等价、语义等价或任意 APK 的完整恢复。

## iOS 用法

[完整 iOS 指南](ios.md) 说明 IPA、`.app`、Mach-O 选择、环境、全部 CLI 选项、源码报告结构、覆盖口径和验证方式。

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

每次运行只选择一个可执行文件，IPA 与 `.app` 的 `--artifact` 均相对于应用目录。Fat 选择顺序是 arm64、arm、x86_64、i386；源语言输出面向 arm64/x86_64，加密切片会被拒绝。实验性原生流程输出 C 和受支持的 Objective-C 方法体，包括标量/指针、浮点、混合及栈参数 ABI 绑定。经过校验的类/实例变量布局和独立 Category 会被保留；未解析布局、签名、调用和其他依赖仍明确标为省略项。

Swift 恢复通过 NeverD LLVM fork 中的 `LLVMSwiftDemangle`，直接在 C++ 进程内分类签名，不启动外部解名程序或工具链发现命令。构建和运行 NeverD 不需要本机安装 Swift 编译器。fork 源码构建及匹配版本的 LLVM 软件包均包含该组件，NeverD 不再单独获取 Swift 源码依赖。签名库存记录 `demangler: {"name": "llvm-swift-demangle", "execution": "builtin", "version": "6.3.3"}`。受支持签名绑定原生入口与 ABI 位置后，才输出函数、类方法/初始化器及固定布局结构体方法的实际 `.swift` 源码。泛型/resilient、async/throwing、不支持的运行时生成可调用形式和不完整源码依赖组仍未恢复。

正常输出包含 `sources/native.c`、可选的 `sources/objc.m` 与 `sources/swift.swift`、声明和运行时元数据、方法/签名覆盖 JSON、日志、`artifacts/selected.macho` 和 `report.json`。不再生成外部 Swift 工具链发现或解名日志。生成源码不会通过桥接调用原始二进制。Swift `source_units` 将类型声明与方法成组组织，不能直接拼接逐方法源码来重建类。最外层 `status: "success"` 表示结果发布成功，不代表方法全部恢复或已证明语义等价。

`--metadata-only` 既不运行原生源码导出器，也不执行签名解名，不生成源码和方法覆盖。所有模式均使用原生加载器解析的 Objective-C 元数据。Swift 元数据通过有界的原生映像读取获取；不支持的修正、可重定位布局或引用会保留部分恢复诊断。 `--max-func` 限制原生函数数量，元数据模式忽略此项。正常运行若没有原生函数体会失败，临时解包输入会被清理。

原始注释、排版、已删除标识符和编译丢失的源语言结构无法精确还原。使用输出前请检查每个方法的状态与原因、Swift 的可调用/不可调用/未分类统计，以及文档列出的限制。

## 限制与失败处理

`-o` 必须指定不存在的目录，且不能位于目录输入内部。已有输出不会覆盖，只有恢复和验证成功后才发布结果。原生 CLI 成功返回零，恢复失败返回非零。使用 `--json` 时，已处理的失败包含 `schema_version`、`status: "error"` 和 `error`。参数解析、原生程序或依赖库启动失败以及中断仍可能只报告 stderr。调用方应先检查退出状态。

默认最多 20,000 个条目、2 GiB 输入/解包或最终输出数据，内置 Android/iOS 分析时间预算或每个显式 JADX 进程上限为 300 秒。iOS 子进程使用总分析预算的剩余时间。内置读取器和生成器还实施有界工作量预算。可用 `--max-files`、`--max-bytes` 和 `--timeout` 调整，均必须大于零。后台运行期间会监测临时工作区，允许暂存输入与中间产物共存，条目数和字节数上限为配置值的三倍。每个进程的诊断最多 16 MiB。

APK 暂存只写出根目录的 `classes.dex`、`classes2.dex` 和后续编号 DEX。所有 ZIP 成员仍须经过头部与范围校验、解压、长度和 CRC 验证，并计入归档条目数与解压后字节数限额。不写出的资源允许使用区分大小写的不同名称，例如 `res/-A.xml` 与 `res/-a.xml`。ZIP 精确重名及同一路径的文件/目录类型冲突仍会失败；跨平台文件系统的大小写冲突检查只针对实际写出的成员。包括 IPA 在内的全量提取仍拒绝这些输出路径冲突。整个归档中的路径穿越、链接、特殊文件和加密 ZIP 条目仍被拒绝；目录输入也拒绝链接和特殊文件。

这些限制用于增强健壮性，不是第三方后端的安全沙箱。失败的临时结果会清理。后端非零退出时会附带长度受限的诊断尾部；启动失败、超时和超出预算会使用各自的错误信息。

## 验证

Python 仅用于下面的开发测试脚本；内置移动端恢复在原生 C++20 CLI 中运行。

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

组件测试覆盖解析、不安全容器、后端失败、输出清理、架构选择和已有输出保护。真实 CLI 测试通过 `NEVERD_BUILD_DIR` 选择构建目录。内置 Android 对照脚本使用 JDK（`java`、`javac`）和 D8 构建独立 DEX/APK 样例，再编译运行恢复的 Java；这些是验证依赖，不是内置恢复的运行要求。请针对当前构建执行并检查结果，再将某个样例视为已验证。独立兼容测试还需要 JADX；样例成功不证明任意应用都能恢复。

在具备 Apple Clang、SDK 和已构建 NeverD 的 macOS 上，运行真实 Objective-C 执行对照：

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

脚本编译自有 Objective-C 样本，恢复方法实现，再仅将恢复出的 `.m` 与同一个独立调用程序链接。22 方法样本对比 141 项可观察结果，覆盖整数边界、分支、循环、指针读写、隐藏与未使用参数、float/double 位身份、混合参数和栈位置。只有当前实际运行通过后才能声称样例已验证。所有请求的架构和 fixup 变体都必须完成，默认覆盖 arm64/x86_64 × classic/default；缺少变体或宿主无法执行某个要求的架构均视为失败，不允许跳过。这些样例证据不代表任意 iOS 程序都能完整恢复。macOS 的 CTest 已注册 `NeverDMobileIOSBackend`，主 CI 的测试范围包含此项。

独立 Swift 恢复脚本为 `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd`，它只重新编译生成的 Swift 与调用程序，不链接原始二进制；未支持可调用项和行为差异都会失败。覆盖口径和失败证据保留方式见 [iOS 指南](ios.md)。

[Mobile Real Applications 工作流](../../.github/workflows/mobile-real-apps.yml) 使用[样本清单](../../scripts/mobile_real_apps.json)中固定版本的公开应用。验收要求独立列出每个 APK 的全部 DEX 和每个完整 iOS bundle 的全部 Mach-O，独立重建原版与生成源码，并对照行为。阶段缺失、清单覆盖未知或必需 case 缺失都会使验收失败。真实应用的 recompile 和 behavior 阶段目前仍未完成，因此保留 Experimental 标记；测试框架的守卫测试通过不代表真实应用验收通过。
