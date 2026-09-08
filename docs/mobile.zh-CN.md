# 移动应用逆向支持

[← 文档索引](README.zh-CN.md) · [完整 Android 指南](android.zh-CN.md)

`neverd mobile` 支持将 Android APK、DEX、smali 恢复为可读 Java。对 iOS IPA、`.app` 和 Mach-O，命令输出原生 C，将受支持的 Objective-C 方法体恢复为 `.m` 源码，并导出运行时元数据。目前这是实验性的 CLI 功能；原生 C SDK 和 GUI 加载器尚不支持移动应用容器。

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

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-ios --arch arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
neverd mobile App.ipa -o quick-analysis --max-func 20 --timeout 600
```

IPA 需要有唯一的顶层 `Payload/*.app`，主程序通过 `Info.plist` 的 `CFBundleExecutable` 定位。`--artifact` 可选择相对该应用目录的内嵌 Framework 等文件；每次命令分析一个原生文件。

Fat 二进制的 `auto` 选择顺序为 arm64、arm、x86_64、i386，也可以用 `--arch` 明确选择。缺失或不支持的架构会报错。选中的 Mach-O 切片若仍标记为加密，会拒绝反编译，需要提供已经解密的分析样本。

正常恢复会生成以下文件：

| 路径 | 内容 |
|------|------|
| `sources/native.c` | 已恢复函数的原生 C 分析结果 |
| `sources/objc.m` | 受支持的 Objective-C 方法的实际恢复方法体；没有可输出方法时不存在 |
| `metadata/objc.h`、`metadata/objc.json` | 声明、类、选择子、类型编码、实现地址和元数据限制 |
| `metadata/objc-methods.json` | 逐方法覆盖报告，保留未恢复的方法、原因和原生诊断 |
| `metadata/swift.json` | 可用的名义类型和 mangled Swift 符号 |
| `artifacts/selected.macho` | 选中的单架构二进制 |
| `logs/native.log` | 原生后端诊断 |
| `report.json` | 汇总恢复报告及相对输出路径 |

临时解包内容和中间批量源码报告会清理。正常恢复若没有生成原生函数体，即使有元数据也会失败。`--metadata-only` 保留选中的二进制、三个元数据/声明文件和 `report.json`，不生成 `sources/`、`metadata/objc-methods.json` 或 `logs/native.log`。其报告中的 `native_function_count` 和 `objc_method_recovery` 均为 `null`。

`--metadata-only` 使用的 Python Objective-C 元数据读取器不解析链式指针或可重定位目标文件的指针。完整原生分析则使用原生加载器的 Objective-C 元数据，包括已经逐槽解析的链式指针。两种模式的 Swift 元数据都使用 Python 读取器，不支持的间接类型引用仍会标记为部分恢复。`--max-func N` 限制原生函数恢复数量，零表示全部发现的函数。因数量限制而排除的方法，只要元数据可用，仍会在清单中标记为未恢复；该选项对 `--metadata-only` 没有影响。

### Objective-C 方法体

原生加载器把可读的方法记录关联到可执行的实现地址。受支持的类型编码为源码输出提供固定参数位置、有无符号、位宽和返回类型。未使用的隐藏接收者、选择子以及显式参数也会保留其位置。`sources/objc.m` 的 `@implementation` 中包含恢复后的实际语句、类型绑定和语句需要的 C 辅助函数，不会通过插入空方法体来提高覆盖率。

目前方法体路径支持 arm64 和 x86_64、void 返回类型、整数与普通指针签名，以及能放入整数参数寄存器的固定参数（arm64 为八个，x86_64 为六个，均包含两个隐藏参数）。支持有效的绝对指针和小型相对方法记录；链式指针元数据只有在对应槽确实已被加载器解析时才使用。无效指针、互相冲突的声明、不支持的编码、不完整解码、IR 验证失败和降级源码输出都不会被列为已恢复方法。

浮点或聚合类型签名、栈上传参、可变参数尾部、Block、异常，以及需要未绑定原生调用或动态调用的方法，目前不能输出为完整 Objective-C 方法体。运行时编码只描述固定参数，不能证明原声明没有省略号。属性、协议、Category 和原始实例变量布局也尚未恢复；访问实例存储的方法在重建类之前可能还需要补全布局。这些情况仍可查看原生 C 分析输出。

使用 `.m` 前应检查 `report.json` 的 `objc_method_recovery`，或内容相同的 `metadata/objc-methods.json`。`method_count`、`recovered_method_count`、`unrecovered_method_count` 和 `methods` 描述已发现的方法清单，每个未恢复项都带有原因。`diagnostics` 保留可用的原生方法诊断，包括运行时类型提示的不确定性。

| 覆盖状态 | 含义 |
|----------|------|
| `recovered` | 清单中的所有方法均已输出方法体，且 Objective-C 元数据状态为 `recovered` |
| `partial` | 已输出至少一个方法体，但仍有方法被省略，或元数据不完整 |
| `unrecovered` | 清单中存在方法，但没有输出任何方法体 |
| `no-methods` | 清单为空；这不能证明原程序没有方法 |

最外层 `status: "success"` 表示结果发布成功，也可能仅恢复了部分方法，或没有可用的 Objective-C 方法体。即使状态是 `recovered`，它也只是源码生成覆盖情况，不是语义等价证明。

对已经可读的原始 Mach-O，也可以独立使用同一批量导出接口：

```sh
neverd export executable --format=objc-methods -o method-sources.json
neverd export executable --format=objc-methods --max-func=20 -o limited-sources.json
```

批量 JSON 包含 `native_source`、`native_function_count`、`objc_metadata`、`methods`、`pointer_size` 和限制。每个已恢复方法包含准确的生成 C 函数名、源码、返回类型和参数列表。批量报告的 `recovered_method_count` 统计原生 C 输出；mobile 的 `.m` 渲染器在校验标识符、声明和函数体时，还可能排除其他方法。最终 `.m` 方法清单应以 mobile 覆盖报告为准。批量报告的 `status: "success"` 表示报告已生成，也可能没有恢复任何方法。

原生 C API `neverd_objc_methods_json(session, max_functions)` 对已加载的 Mach-O 返回这份 JSON，使用 `neverd_free_string` 释放结果。零表示全部发现的函数；对有效会话调用失败时，返回 `NULL` 并设置会话错误。此 API 不加载 IPA 或 `.app` 容器。

### 恢复限制

Swift 源语言方法体尚未恢复，Swift 符号和名义类型名称只是元数据。符号裁剪、优化、缺失元数据和不支持的指针修正都会限制结果。原始注释、排版、已删除标识符和编译后丢失的源语言结构无法精确还原。受支持的运行时签名之外，原生类型和调用约定仍是推断结果。任意应用的重建还依赖外部组件、实例布局和运行时行为，单独的方法清单无法提供这些信息。

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

脚本编译自有 Objective-C 样本，恢复方法实现，再仅将恢复出的 `.m` 与同一个独立调用程序链接。对比的 85 项可观察结果覆盖常量、有无符号边界、分支、循环、指针读写、隐藏参数和未使用参数的位置。会尝试 arm64、x86_64 和传统/默认指针布局；宿主无法执行的架构会明确跳过，宿主架构必须实际执行。这些样例证据不代表任意 iOS 程序都能完整恢复。macOS 的 CTest 已注册 `NeverDMobileIOSBackend`，主 CI 的测试范围包含此项。
