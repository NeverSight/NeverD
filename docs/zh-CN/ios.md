**Languages**: [English](../ios.md) | [简体中文](ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](../ja/ios.md) | [한국어](../ko/ios.md) | [Français](../fr/ios.md) | [Deutsch](../de/ios.md) | [Español](../es/ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# iOS 原生代码与源码恢复

[← 文档索引](README.md) · [移动应用概览](mobile.md)

`neverd mobile` 接受 IPA、`.app` 和 Mach-O，输出原生 C、运行时元数据，以及受支持原生方法体的实验性 Objective-C `.m` 和 Swift `.swift` 源码。发布成功的结果仍可能包含未恢复方法，使用源码前请检查覆盖报告。移动应用容器由 CLI 处理；原生 C SDK 可单独加载选中的 Mach-O。

编译会丢失注释、排版、标识符和源语言结构。此流程重建源码表示，无法还原原始文本，也不能为任意应用证明行为等价。静态恢复流程不会启动被分析的应用。

## 开始使用与依赖

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

使用支持 C++20 的工具链构建 `neverd` 目标。移动端工作流在原生 CLI 内运行，不需要 Python 解释器。Swift 签名解名由 NeverD LLVM fork 中的 `LLVMSwiftDemangle` 提供；该 fork 的源码构建和配套版本的 LLVM 软件包均包含此组件，NeverD 不再单独获取 Swift 源码依赖。构建和运行 NeverD 不需要本机安装 Swift 编译器或工具链。LLVM、Capstone 等原生库依赖仍然适用；分发时携带当前构建所需的库及许可证声明。在 macOS 上独立编译生成的 Apple 语言源码和运行 Swift 行为回归，才按需使用 Apple Clang、SDK 和 `swiftc`。

Swift 签名恢复直接在 C++ 进程内读取 `LLVMSwiftDemangle` 的结构化节点，不查找或启动外部解名程序，也不执行工具链发现命令。原可执行文件路径选项已移除，原解名环境变量不再读取。`--metadata-only` 既不运行原生源码导出器，也不执行签名解名。

`metadata/swift-signatures.json` 的签名清单以如下字段记录内置组件：

```json
{
  "demangler": {
    "name": "llvm-swift-demangle",
    "execution": "builtin",
    "version": "6.3.3"
  }
}
```

```sh
neverd mobile App.ipa -o recovered-swift --timeout=600 --json
```

## 输入与选择

IPA 必须包含唯一的顶层 `Payload/*.app`。`.app` 和 IPA 均通过 `Info.plist` 的 `CFBundleExecutable` 选择主程序。两种容器中的 `--artifact` 都相对于该应用目录，只选择一个内嵌可执行文件，不递归分析所有 Framework 或 Extension。原始 Mach-O 输入不接受 `--artifact`。

Fat 二进制的 `--arch=auto` 优先顺序是 arm64、arm、x86_64、i386。缺失或不支持的切片会明确失败。目前源语言输出面向 arm64 和 x86_64；选择其他架构不代表支持 Objective-C/Swift 源码恢复。选中切片若 `cryptid != 0` 会被拒绝，需要提供已经解密且可读的分析输入。归档和目录输入会拒绝不安全路径、符号链接、特殊文件及冲突条目。

## 选项与资源限制

| 选项 | 默认值 | 含义 |
|------|--------|------|
| `-o DIRECTORY` | 必填 | 不存在且位于目录输入之外的输出目录；保留已有输出 |
| `--platform=auto\|ios` | `auto` | 推断平台或明确选择 iOS |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | 选择一个 Mach-O 切片 |
| `--artifact PATH` | 主程序 | 相对于应用目录的可执行文件路径 |
| `--metadata-only` | 关闭 | 只读元数据，不恢复源码或调用工具 |
| `--max-func N` | `0` | 原生函数数量上限；零表示所有发现的函数；元数据模式忽略此项 |
| `--timeout N` | `300` | 正数总分析时间预算（秒）；子进程使用剩余预算 |
| `--max-files N` | `20000` | 正整数条目预算；Swift 符号清单也有数量限制 |
| `--max-bytes N` | `2147483648` | 输入、解包数据和最终输出的正整数字节预算 |
| `--json` | 关闭 | 以 JSON 输出带版本的报告 |

工作区会被监测，暂存输入和中间输出可使用配置条目/字节预算的最多三倍。每个进程的日志上限为 16 MiB，Swift 签名 JSON 还受 32 MiB 限制。这些是资源控制，不是进程隔离。增加超时不会取消字节或条目限制。因 `--max-func` 被排除但仍在元数据清单中的方法会保留为未恢复项。

## Objective-C 源码与运行时结构

原生加载器把运行时方法记录、可执行 IMP 地址和受支持的类型编码绑定到明确的源码 ABI 位置。固定标量/指针绑定包含隐藏的 `self`/`_cmd`、未使用参数、独立的整数/浮点寄存器组，以及受支持的栈参数位置。float/double 的位重解释与数值转换分别处理。类型提示只是源码输出的输入，不是经过认证的 ABI 证据，也不授权修改可执行代码。

`sources/objc.m` 将实际恢复语句放入 `@implementation` 方法体，保留必要的 C 辅助函数和具有类型绑定的调用。可输出的调用目标必须有受支持的源码绑定；未知目标或不完整依赖组仍是未恢复项。缺失定义、无效可执行地址、冲突编码、不支持的 ABI 映射、不完整解码和被 IR 校验拒绝的结果，不会仅因存在声明就被标为已恢复。

类元数据保留父类身份、实例起始位置/大小，以及偏移、宽度和对齐已校验的标量/指针实例变量；声明在必要处插入填充。依赖不可用实例布局的方法仍标记为未恢复。Category 保留独立的类/分类/地址身份和实现；类与分类清单中重复出现的完全相同记录只计一次。外部 Category 在受支持时使用已有 Foundation 类声明；未知外部类头文件会报告为缺失依赖，不会虚构替代类布局。

恢复状态还要求完整的类声明、本地祖先类定义，以及生成源码中所有使用路径上的先定义后使用和可达出口所需的返回；只有布局已验证、完整方法清单证明没有自身普通方法的本地空父类，才会生成空 `@implementation`。缺少这些证据或名称与已知 Foundation 导入项冲突时，受影响方法仍保留在覆盖率分母中并注明原因，可独立恢复的类继续输出；这些名称检查不覆盖全部 SDK 名称、iOS SDK 或版本。

受支持的 Objective-C Block 调用必须具备完整的固定标量调用 ABI，包括隐藏的 Block 对象及全部参数和返回值载体。运行时编码 `@?` 只在声明中宽化为 `id`，不能提供调用原型。全局 Block 引用保留共享对象身份。受支持的同步标量捕获需要证明原生捕获存储和调用流程。逃逸或异步捕获、模型未覆盖的对象/byref 所有权、copy/dispose 辅助函数和未知布局仍保留为未恢复。

这只是对运行时信息的有限重建，不承诺完整恢复属性、协议、原始所有权标注、任意聚合类型、可变参数尾部、依赖异常的方法体和模型未覆盖的 Block/捕获布局。运行时编码只描述固定参数，不能证明原声明不存在省略号。只有原生加载器已解析相关槽时才使用链式指针；未解析格式会保留诊断。

## Swift 源码与存储布局

结构化 demangler 输出将可调用签名与不可调用元数据分别分类。受支持签名在恢复原生方法体前，必须绑定选中二进制的符号、入口和明确的机器 ABI。Swift 接收者遵循 Swift ABI，不替换成 Objective-C 隐藏参数。用户提供的签名文件也只是需要验证的提示。

实验性输出器可以构建受支持的自由函数、类方法、指定初始化器和固定布局结构体方法，包括受支持的 mutating 接收者形式。类/结构体声明和存储字段需要恢复出的布局元数据。只有所需源码声明和方法体组成完整且受支持的依赖组时，才输出原生调用。恢复的源码单元将声明与方法放在一起，不通过桥接代码调用原始二进制。

受支持的 Swift getter/setter 方法体来自原生实现，再组装为属性。私有 backing storage 保留已确认的字段布局，初始化器和其他方法使用同一套存储名称。仅有属性声明或字段记录，不能证明已经恢复访问器方法体。

受支持的分配式初始化器、平凡析构器/释放器、类型元数据访问器和 `_modify`/resume 入口可以投影到已输出的类型单元。每项都需要对完整原生流程与效果进行有界证明，实际已恢复的上下文/初始化器/属性依赖，以及相关方法体的异常处理和 IR 审计。分配器写入必须与真实初始化器一致；`_modify` 必须绑定准确的可变字段及继续执行入口。运行时元数据调用在恢复类型内保留其建模语义。这些入口明确报告为编译器源码投影，不代表单独恢复出的普通方法体或原始源码文本。

泛型或 resilient 布局、async/throwing 函数、未知调用约定、不支持的访问器/分配器/thunk、不完整初始化及未绑定的原生或运行时依赖，会逐项保留为 `unrecovered`。仅有 mangled 符号或名义类型名称不等于恢复了方法。符号裁剪和未分类的 demangler 节点会使覆盖不完整或未知。

## 输出与覆盖口径

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

只有能够输出源码时，才生成对应源语言文件。`objc.json` 保存类、Category、实例变量和原始方法编码，`objc.h` 保存受支持声明；`swift.json` 保存名义类型元数据及 mangled 符号。签名和方法 JSON 保留分类、省略项、原因及数量。日志包含原生诊断，以及实际运行时的原生 Swift 导出诊断，不再生成外部 Swift 工具链发现或解名日志。`report.json` 中的输出路径相对于其目录。选中的二进制是分析产物，生成源码不会把它作为恢复桥接依赖来链接。

临时包副本和中间后端 JSON 会被删除。正常运行若没有原生函数体，即使存在元数据也会失败。元数据模式仅生成选中的文件、`objc.h`、`objc.json`、`swift.json` 和 `report.json`，没有源码目录或方法覆盖/签名文件；`native_function_count`、`objc_method_recovery`、`swift_method_recovery` 均为 `null`。所有模式均使用原生加载器解析的 Objective-C 元数据。Swift 元数据通过有界的原生映像读取获取；不支持的修正、可重定位布局或引用会保留部分恢复诊断。

下面的缩略示例明确展示部分恢复：

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

最外层 `status: "success"` 表示已发布通过校验的输出。方法覆盖 `recovered`、`partial`、`unrecovered`、`no-methods` 描述的是已发现清单，不是语义等价或原程序完整性。每个未恢复方法都有原因。Objective-C 的 `recovered` 还要求运行时元数据完整。空清单不能证明原程序没有方法。

Swift 的 `coverage_status` 只统计已分类的可调用项。整体 Swift `status` 还考虑未知符号，可为 `unclassified`、`unsupported-architecture` 或 `no-symbols`。不可调用元数据位于 `non_method_symbols`，状态为 `not-callable`；未知符号使用 `unclassified`。`types`、`type_metadata_count`、`source_type_count` 分别记录类型元数据/输出类型单元，不得用来增加方法数量。

每个已恢复 Swift 条目的 `source_representation` 为 `native-method-body` 或 `compiler-generated-from-type`。编译器投影还保留 `compiler_projection_kind` 和 `compiler_projection_evidence`。`source_body_method_count` 统计已恢复原生方法体，`compiler_projection_method_count` 统计通过证明的编译器投影，两者之和等于 `recovered_method_count`。编译器入口继续计入 `method_count` 分母，其准确身份必须出现在唯一对应的 `type` 源码单元中。仅有类型元数据或依赖名称不能增加已恢复覆盖。原生批量 JSON 的编译器条目和类型单元包含 `source`；mobile 的 `source_units` 仅保留描述、不含 `source`，完整源码见 `sources/swift.swift`。

原生 Swift 批量报告的 `source_units` 记录 `{kind, module, name, source, method_entries, method_identities}`，kind 为 `function` 或 `type`，每个 identity 为 `{entry, mangled_symbol}`。不同符号可以共享入口并保留各自 ABI 输出；每个已恢复 identity 必须且只能出现一次，未恢复 identity 不得出现。`method_entries` 必须精确等于 `method_identities` 的有序入口投影，允许重复地址；不能静默合并完全相同的重复 identity。批量 `source` 等于按顺序拼接每个单元源码再加一个换行。Mobile 在 `sources/swift.swift` 保存完整源码，在覆盖 JSON 保留单元描述。逐方法 `source` 用于查看，直接拼接无法正确重建类声明。

## 直接原生导出与 SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

Swift 导出使用正常 mobile 流程由内置签名解析器生成的结构化签名清单。Objective-C 批量 JSON 包含 `native_source`、`native_function_count`、`objc_metadata`，以及逐方法 C 源码、函数名、返回类型和参数。Mobile 在生成 `.m` 前还会校验声明、方法体和布局，因此最终方法覆盖可能少于批量 C 覆盖。原生导出成功也可能没有任何已恢复方法。

对已加载 Mach-O 的会话，`neverd_objc_methods_json(session, max_functions)` 和 `neverd_swift_methods_json(session, signatures_json, max_functions)` 返回相应报告。零表示所有发现的函数。成功返回的字符串用 `neverd_free_string` 释放；`NULL` 表示失败，原因见会话错误。这些 API 不加载 IPA 或 `.app` 容器。

## 验证与故障排查

macOS 上启用 `BUILD_TESTING` 的构建提供 `check-neverd-mobile-ios`，通过 CTest 运行三组原生恢复验证。

Python 仅用于下面的开发测试脚本；内置移动端恢复在原生 C++20 CLI 中运行。

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

macOS 上的 Objective-C 验证脚本先编译原样本，再恢复 `.m`，最后只把生成源码与独立调用程序链接。标量脚本覆盖整数边界、分支、循环、指针读写、隐藏参数、float/double 位身份、混合参数和栈参数。调用脚本另覆盖消息分派、继承、Category、实例变量存储、原生辅助函数及 Block 调用/捕获/共享身份。调用样本要求 arm64/x86_64 × classic/default 每个变体恢复 21/21 个方法，并通过 134/134 项独立预期结果检查。请对当前原生 CLI 构建运行这些验证。

严格 Swift 脚本检查 22 个用户声明、3 个 getter/setter 入口和 7 个编译器生成的可调用入口，任何一项都不能从清单中消失。每个变体有 855 个原程序独立预期结果检查。脚本独立编译生成的 `.swift` 与调用程序，不使用原始 dylib、模块、桥接或手写替代声明。覆盖标量/原生调用、类初始化与存储、结构体按值/mutating 方法、浮点和栈参数、指针及循环。原生 C++20 CLI 的验收要求是 arm64/x86_64 × classic/default 四个变体零跳过：每组必须恢复 25 个原生方法体和 7 个编译器投影，保留全部 32 个可调用身份，原程序与独立编译的生成 Swift 均须通过 855/855 项独立预期结果检查。这些结果限于该样本，不保证任意应用或原始源码文本的恢复。脚本会拒绝覆盖缺失、源码编译失败及行为差异。

三个脚本都支持 `--arch all|arm64|x86_64`、`--fixups both|classic|default`、`--timeout N` 和 `--work-dir NEW_DIRECTORY`。`--setup-only` 只验证原样本，不测试恢复。包括标量脚本在内，验收要求完成所有请求的架构和 fixup 变体；缺少变体或宿主无法执行某个要求的架构均视为失败，不允许跳过。保留的失败产物可用于区分源码覆盖缺失、编译错误和行为差异；声称已验证前应查看当前测试结果。

[Mobile Real Applications 工作流](../../.github/workflows/mobile-real-apps.yml) 使用[样本清单](../../scripts/mobile_real_apps.json)中固定版本的公开应用。验收要求独立列出完整 iOS bundle 和 APK 中的全部 Mach-O/DEX，独立重建原版与生成源码，并对照行为。阶段缺失、清单覆盖未知或必需 case 缺失都会使验收失败。真实应用的 recompile 和 behavior 阶段目前仍未完成，因此保留 Experimental 标记；测试框架的守卫测试通过不代表真实应用验收通过。

结果发布具有事务性：选择新目录，先检查进程退出状态，并把重定向的 JSON 放在该目录外。失败会删除暂存输出并保留已有结果。后端非零退出附带长度受限的日志尾部。后端超时保留原有超时消息；若已捕获的日志文本可用，会追加长度受限的尾部；预算失败保留独立诊断。原生 CLI 成功返回零，恢复失败返回非零。使用 `--json` 时，已处理的失败包含 `schema_version`、`status: "error"` 和 `error`。参数解析、原生程序或依赖库启动失败以及中断仍可能只报告 stderr。调用方应先检查退出状态。

加密切片需要可读输入；缺少架构时检查可用切片；方法被省略时查看其准确原因和元数据诊断。增加 `--max-func` 只对因数量上限被排除的函数有帮助。缺少布局、签名、外部头文件、异常支持或 ABI 行为支持，需要补充实现或有效元数据，不能直接宣称完整恢复。分发工具或生成的软件包时保留适用的依赖许可证声明。
