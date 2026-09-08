# 移动应用逆向支持

[← 文档索引](README.md) · [完整 Android 指南](android.md) · [完整 iOS 指南](ios.md)

`neverd mobile` 支持将 Android APK、DEX、smali 恢复为可读 Java。对 iOS IPA、`.app` 和 Mach-O，命令输出原生 C，将受支持的 Objective-C 方法体恢复为 `.m` 源码，并输出实验性 Swift 源码与运行时元数据。目前这是实验性的 CLI 功能；原生 C SDK 和 GUI 加载器尚不支持移动应用容器。

## 环境

正常构建 `neverd` 即可。移动构建产物时，保留可执行文件旁边的 `mobile/` 目录。需要 Python 3.10 或更高版本，可通过 `--python PATH` 或 `NEVERD_PYTHON` 指定。

Android 还需要单独安装 JADX 1.5.6 或更新版本、标准 DEX/smali 输入插件及 Java 11 或更新版本。使用 `--jadx PATH`、`NEVERD_JADX`，或把 `jadx` 放进 PATH。Windows 请指定发行包中 `jadx.bat` 的完整路径或 `lib/jadx-*-all.jar`；NeverD 使用 Java 直接启动 JAR，不通过命令解释器传递应用路径。JAR 启动方式支持 `JAVA_HOME`。不会自动下载依赖。安装方法和依赖许可证见 [JADX 发行说明](https://github.com/skylot/jadx#download)。

## Android 用法

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java --jadx /opt/jadx/bin/jadx
```

APK 根目录中的 `classes.dex`、`classes2.dex` 等字节码会一起分析。smali 目录会递归收集类，在同一次调用中处理嵌套类和跨类引用；单个 smali 文件仅提供该类的上下文。

输出包括 `sources/`、`logs/` 和 `report.json`。报告记录后端版本、输入代码、生成的 Java 文件和限制。此流程不恢复 APK 资源、Manifest、原生库和动态下载代码；原生库可单独提取后用 `neverd decompile` 分析。

原始注释、排版和已删除名称无法还原。后端错误、明确的代码恢复失败标记或没有生成 Java 都会使命令失败。成功输出仍属于恢复代码，不代表已证明语义等价，也不保证每个方法都能重新编译。

## iOS 用法

[完整 iOS 指南](ios.md) 说明 IPA、`.app`、Mach-O 选择、环境、全部 CLI 选项、源码报告结构、覆盖口径和验证方式。

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

每次运行只选择一个可执行文件，IPA 与 `.app` 的 `--artifact` 均相对于应用目录。Fat 选择顺序是 arm64、arm、x86_64、i386；源语言输出面向 arm64/x86_64，加密切片会被拒绝。实验性原生流程输出 C 和受支持的 Objective-C 方法体，包括标量/指针、浮点、混合及栈参数 ABI 绑定。经过校验的类/实例变量布局和独立 Category 会被保留；未解析布局、签名、调用和其他依赖仍明确标为省略项。

Swift 恢复会分类 demangled 签名，将其绑定原生入口和 ABI 位置，然后输出受支持的函数、类方法/初始化器及固定布局结构体方法的实际 `.swift` 源码。Demangler 依次通过 `--swift-demangle`、`NEVERD_SWIFT_DEMANGLE`、PATH 和 macOS 的 `xcrun --find swift-demangle` 查找。自动查找失败会保留未分类覆盖，显式指定工具缺失则失败。泛型/resilient、async/throwing、不支持的运行时生成可调用形式和不完整源码依赖组仍未恢复。

正常输出包含 `sources/native.c`、可选的 `sources/objc.m` 与 `sources/swift.swift`、声明和运行时元数据、方法/签名覆盖 JSON、日志、`artifacts/selected.macho` 和 `report.json`。生成源码不会通过桥接调用原始二进制。Swift `source_units` 将类型声明与方法成组组织，不能直接拼接逐方法源码来重建类。最外层 `status: "success"` 表示结果发布成功，不代表方法全部恢复或已证明语义等价。

`--metadata-only` 不调用后端或 demangler，也不生成源码和方法覆盖。其 Python Objective-C 读取器不解析链式或可重定位对象指针，完整恢复则使用原生加载器解析的元数据。原始 Swift 元数据中的不支持引用可能仍标为部分恢复。`--max-func` 限制原生函数数量，元数据模式忽略此项。正常运行若没有原生函数体会失败，临时解包输入会被清理。

原始注释、排版、已删除标识符和编译丢失的源语言结构无法精确还原。使用输出前请检查每个方法的状态与原因、Swift 的可调用/不可调用/未分类统计，以及文档列出的限制。

## 限制与失败处理

`-o` 必须指定不存在的目录，且不能位于目录输入内部。已有输出不会覆盖，只有恢复和验证成功后才发布结果。`--json` 输出带版本的报告；已捕获的恢复错误返回非零状态和 JSON 错误。启动失败、辅助程序或解释器缺失、Python 低于 3.10、参数解析错误和中断也可能只在 stderr 输出纯文本。

默认最多 20,000 个条目、2 GiB 输入/解包或最终输出数据，每个后端进程最多 300 秒。可用 `--max-files`、`--max-bytes` 和 `--timeout` 调整，均必须大于零。后台运行期间会监测临时工作区，允许暂存输入与中间产物共存，条目数和字节数上限为配置值的三倍。每个进程的诊断最多 16 MiB。解包拒绝路径穿越、符号链接、特殊文件、大小写路径冲突和加密 ZIP 条目；目录输入也拒绝链接和特殊文件。

这些限制用于增强健壮性，不是第三方后端的安全沙箱。失败的临时结果会清理。后端非零退出时会附带长度受限的诊断尾部；启动失败、超时和超出预算会使用各自的错误信息。

## 验证

```sh
cmake --build build --target check-neverd-mobile
python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

测试覆盖不安全容器、损坏或加密的原生输入、后端失败、Java 恢复不完整、架构选择和已有输出保护。真实 CLI 测试通过 `NEVERD_BUILD_DIR` 选择构建目录。真实 Android smoke runner 还需要后端和 JDK，会恢复自建 smali/DEX/多 DEX APK，然后编译运行生成的 Java。

在具备 Apple Clang、SDK 和已构建 NeverD 的 macOS 上，运行真实 Objective-C 执行对照：

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

脚本编译自有 Objective-C 样本，恢复方法实现，再仅将恢复出的 `.m` 与同一个独立调用程序链接。22 方法样本对比 141 项可观察结果，覆盖整数边界、分支、循环、指针读写、隐藏与未使用参数、float/double 位身份、混合参数和栈位置。只有当前实际运行通过后才能声称样例已验证。会尝试 arm64、x86_64 和传统/默认指针布局；宿主无法执行的架构会明确跳过，宿主架构必须实际执行。这些样例证据不代表任意 iOS 程序都能完整恢复。macOS 的 CTest 已注册 `NeverDMobileIOSBackend`，主 CI 的测试范围包含此项。

独立 Swift 恢复脚本为 `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd`，它只重新编译生成的 Swift 与调用程序，不链接原始二进制；未支持可调用项和行为差异都会失败。覆盖口径和失败证据保留方式见 [iOS 指南](ios.md)。
