**语言**: [English](../android.md) | [简体中文](android.md) | [繁體中文](../zh-TW/android.md) | [日本語](../ja/android.md) | [한국어](../ko/android.md) | [Français](../fr/android.md) | [Deutsch](../de/android.md) | [Español](../es/android.md) | [Italiano](../it/android.md) | [Русский](../ru/android.md) | [العربية](../ar/android.md)

# Android Java 代码恢复

[← 文档索引](README.md)

`neverd mobile` 默认使用 NeverD 内置引擎，将 APK、DEX 和 smali 恢复为可读 Java。独立实现的读取器共用带类型的 Dalvik 模型和有界 Java 生成器。这是实验性的 CLI 功能，不代表与 JADX 功能等价，也不保证任意 APK 都能完整恢复。原生 C SDK、Python 插件 SDK、GUI 加载器和 `neverd decompile --language` 均未提供 APK 容器与 Java 输出入口。

生成的 Java 是字节码的重建结果。原始注释、排版、原源码语言选择和已删除标识符无法恢复；Kotlin 编译后的字节码也输出 Java。命令成功不代表已经证明语义等价，也不保证每个方法都能重新编译。此流程不会启动被分析的应用。

## 快速开始

按下文准备运行环境后，选择一个尚不存在的输出目录：

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

在 `recovered-app/sources/` 中阅读 Java，在 `recovered-app/report.json` 中查看输入清单和恢复限制。smali 类存在相互引用时，优先提供包含它们的目录。

## 运行环境

| 组件 | 要求 | 选择顺序 |
|------|------|----------|
| NeverD | 使用支持 C++20 的工具链构建 `neverd` 目标。移动端工作流已编译进原生 CLI，不调用 Python 解释器。分发时携带当前构建所需的原生依赖库。| `build/bin/neverd` / PATH |

默认引擎以 C++20 实现，运行时不需要 Python、Java 或 JADX。它接受 DEX 035、037–040 和 smali 中可表示的常规声明与操作。DEX 041、`invoke-custom` 等动态调用、部分初始化路径、未知的语义注解或操作，以及无法用 Java 表示的标识符都会明确失败。接受某种文件格式，不意味着支持该格式中的所有指令和声明。

Java 名称绑定区分类头与类体，可处理已知的同包名称遮蔽；若缺少外部父类或接口声明，导致无法确认类型名或生成的 Java 辅助代码引用的绑定对象，则明确拒绝，当前仅在 `java.lang.Object` 缺少声明时仍假定它不提供可继承的成员类型。Smali 浮点数字面量直接舍入至目标单精度或双精度，保留所得位模式。

内置 C++ 引擎保留并验证支持范围内的类、字段和方法 `Signature` 元数据，包括类型变量、数组、通配符、类型边界和方法级名称遮蔽。泛型擦除必须匹配原始 DEX 声明的身份。`Throws` 不会丢弃；无法证明异常继承关系时会拒绝。泛型继承或成员类型替换、桥接方法再生成、泛型方法调用、参数化内部类型，以及构造器隐藏参数映射，在缺少所需证明时仍明确不支持。

类、字段、方法和构造器上不含元素且运行时可见的 Java 8 `@java.lang.Deprecated` 标记会被保留。参数注解、带注解的静态初始化器、其他可见性级别，以及 `since` 或 `forRemoval` 等元素值均会被拒绝。重新编译后，CI 分别比较 classfile 的 `Deprecated` 属性和运行时注解，并检查无注解的对照声明和生成的辅助方法；辅助方法不计入原始方法数。

CI 使用自有 Java 8 样例，经 D8 和 NeverD 处理后重新编译全部生成的 Java，并对比完整 `Signature` 元数据、反射结果和行为。这些样例检查不代表真实应用已达到完整恢复标准。

### Linux 与 macOS

```sh
cmake --build build --target neverd

./build/bin/neverd mobile app.apk -o recovered-app
```

`NEVERD_JADX` 和 PATH 中的 `jadx` 不会选择外部引擎；只有显式 `--jadx PATH` 才会启用外部适配器。没有自动回退。包含空格的路径需要加引号。

### Windows PowerShell

```powershell
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app
```

多配置构建可能将可执行文件放在 `build/bin/Release/`。分发时请遵循该构建的常规原生依赖库部署要求。

## 支持的输入与范围

| 输入 | 处理方式 | 重要限制 |
|------|----------|----------|
| `.apk` | 验证整个 ZIP，再一起分析根目录的 `classes.dex`、`classes2.dex` 和后续编号 DEX | 仅恢复代码，不解码资源或 Manifest |
| `.dex` | 内置读取器验证并解析 DEX 035 或 037–040 | DEX 041、不支持的声明或操作会失败；改名或截断的文件不是有效字节码 |
| `.smali` | 分析指定类 | 不会自动加载被引用的相邻类 |
| smali 目录 | 递归收集 `.smali` 并联合分析 | 输入目录中应包含嵌套类和相关 smali 根目录 |

分析包含 `smali/` 和 `smali_classes2/` 的 APK 解码目录时，传入它们的公共父目录。只有 `.smali` 文件会交给后端，但整个输入目录会先验证和复制；无关的大型资源也会占用输入限额。把相关 smali 根目录放进一个精简目录可以减少工作量。

Split APK 视为独立输入。含 DEX 的每个 APK 可单独处理，但此命令不会合并 APK 集合；只有资源的 split 因不含根目录 DEX 而失败。`.aab`、`.apks`、`.xapk`、`.odex`、`.oat` 和 `.vdex` 不是当前支持的移动输入。后端本身支持某个格式，不代表 NeverD 的此命令也支持它。

APK 资源、`AndroidManifest.xml`、assets、JNI/原生库和运行时下载代码不在 Java 恢复范围内。原生 `.so` 可单独提取后执行 `neverd decompile library.so -o library.c`。加密或加壳载荷需要事先以普通 DEX/smali 形式可用；本静态流程不执行脱壳、设备附加或保护绕过。

## 参数与优先级

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --timeout=600 --max-files=30000 --max-bytes=4294967296 --json
```

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `-o DIRECTORY` | 必填 | 尚不存在的输出目录，不能位于目录输入内部；不覆盖已有输出 |
| `--platform=auto\|android` | `auto` | 明确选择 Android，或自动识别输入平台 |
| `--jadx PATH` | 未设置：内置引擎 | 显式选择单独安装的 JADX 兼容适配器；不通过环境变量选择，也不自动回退 |
| `--timeout N` | `300` | 内置分析的正数时间预算；外部后端则为每个进程的秒数上限，包括版本探测 |
| `--max-files N` | `20000` | 条目数量上限，包含实际创建的目录，必须为正 |
| `--max-bytes N` | `2147483648` | 输入、解包数据和最终输出的字节上限，必须为正 |
| `--json` | 关闭 | 向标准输出打印 JSON 报告，而非人类可读摘要 |

非默认 `--arch`、`--artifact`、`--metadata-only` 和非零 `--max-func` 属于 iOS，在 Android 下会被拒绝；显式 `--arch=auto` 可用。不支持透传任意后端参数。显式 JADX 适配器为每次运行隔离配置、缓存和临时目录，不导入环境中的后端设置或插件配置。

输入、解包数据和最终输出继续受文件数与字节预算约束。内置读取器和生成器还检查有界工作量及耗时。外部后端工作区最多使用配置条目数和字节数的三倍，以容纳暂存输入和中间输出；每个进程日志最多 16 MiB。这些是资源控制，不是安全沙箱。提高某一上限不会关闭其他限制。

## 输出目录与 JSON 报告

```text
recovered-app/
  sources/                       恢复的 Java 包和类
  metadata/android-methods.json  内置引擎的方法覆盖
  report.json                    带版本的清单和恢复限制
```

临时输入会被清理。嵌套类可能共用外层类的源码文件，因此 Java 文件数不等于 DEX 类数。生成的方法可能使用 Java 分派循环，不执行原始 DEX，也不通过运行时桥接调用它。

内置报告包含 `android_method_recovery`，相同内容写入 `metadata/android-methods.json`，并保留每个原始方法。计数满足 `method_count = recovered_method_count + projected_method_count + declaration_only_method_count + unrecovered_method_count`；缺省的 `projected_method_count` 视为零，发布前 `unrecovered_method_count` 仍须为零。原有 `native`、`abstract` 方法的状态为 `declaration-only`，不计入已恢复方法体。

部分具名且不捕获外部变量的局部类可以还原到其精确所属的静态方法内。当前要求外层是普通标量方法，局部类直接继承 `Object`、没有字段、具有真实无参构造方法，其他方法为标量实例方法，且对象用途经过验证、不会逸出支持的作用域。匿名类、变量捕获、不支持的修饰符和无法证明的对象用途仍会明确失败。

此类输出中，局部类方法及所属外层方法标记为 `source-projected`，`projection_kind` 为 `named-method-local`；即使外层流程报告为 `success`，方法覆盖状态仍为 `partial`。重新编译后的二进制名称和访问标志均尚未验证。Java 编译器可能选择不同的局部类二进制名称，因此 `class_source_bindings` 保留原始类、精确所属方法、源码路径和局部名称，并将 `binary_name_status` 标为 `unverified`。

`generated_source_helpers` 单独列出额外方法，类型精确为 `throw-helper`、`constant-helper`、`default-constructor` 和 `field-initializer`。最后一种表示额外生成、原始方法清单中不存在的 `<clinit>`。这些额外方法不计入原始方法总数。编译成功或某次名称一致不会将其升级为完整恢复。以下缩略示例不含这类投影方法：

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {
    "name": "neverd",
    "version": "1",
    "execution": "builtin"
  },
  "input_code_files": [
    "classes.dex",
    "classes2.dex"
  ],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": [
    "sources/example/Main.java",
    "sources/example/Peer.java"
  ],
  "logs": [],
  "android_method_recovery": {
    "schema_version": 1,
    "status": "recovered",
    "class_count": 2,
    "method_count": 6,
    "recovered_method_count": 5,
    "declaration_only_method_count": 1,
    "unrecovered_method_count": 0
  }
}
```

`input_kind` 为 `apk`、`dex`、`smali` 或 `smali-directory`。`input_code_files` 是输入字节码名称或 smali 路径清单；`java_sources` 和 `logs` 相对于输出根目录；`source` 是输入的基本文件名。真实报告还会包含其他恢复限制，把结果交给其他工具展示时应保留这些信息。

自动化时先检查进程退出码，再读取 `status`。重定向标准输出时，把报告放在待创建的输出目录之外：

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

原生 CLI 成功返回零，恢复失败返回非零。使用 `--json` 时，已处理的失败包含 `schema_version`、`status: "error"` 和 `error`。参数解析、原生程序或依赖库启动失败以及中断仍可能只报告 stderr。调用方应先检查退出状态。

## 失败处理与排查

发布具有事务性：保留已有输出，失败后删除暂存结果。不支持的操作、未解析的寄存器流、无法表示的声明、格式错误的异常处理以及预算耗尽，会让内置流程失败，不会发布缺失的方法体。外部适配器还拒绝非零退出、日志中的汇编或反编译错误、重复类省略、不完整代码标记、空 Java 文件和未生成 Java 的结果。恢复成功不等于语义等价证明。

| 现象 | 处理 |
|------|------|
| 不支持的 DEX、指令、声明或初始化 | 阅读明确诊断并核对支持范围；只有主动选择独立兼容适配器时才使用 `--jadx PATH` |
| 输入无效或重复类 | 修正输入字节码或类集合；不支持的方法体不会被静默省略 |
| 超时或超出预算 | 缩小输入，或按可用资源调整 `--timeout`、`--max-files`、`--max-bytes` |
| 输出已存在 | 选择新的输出目录 |

## 可选 JADX 兼容适配器

`--jadx PATH` 选择外部 JADX，而非内置实现。需安装带标准 DEX/smali 输入插件的 JADX 1.5.6 或更高版本，以及 Java 11 或更高版本。请获取完整 [JADX 发行包](https://github.com/skylot/jadx/releases/tag/v1.5.6)，保持 `bin/`、`lib/` 结构，重新分发时保留随附依赖许可证。不会自动下载任何依赖。适配器报告记录实际 `jadx` 引擎和探测到的版本，不声称提供内置方法覆盖。

Windows 可指定发行包的 `.bat`/`.cmd` 启动器或 `lib/jadx-*-all.jar`。NeverD 解析发行包 JAR 并直接调用 Java，应用路径不会进入命令解释器。`JAVA_HOME` 或 PATH 用于选择 Java。成功的适配器运行保留 `logs/jadx-version.log` 和 `logs/jadx.log`；失败暂存目录及日志会删除。只有后端非零退出会附带有界日志尾部；启动失败、超时和预算错误有各自诊断。

```sh
neverd mobile app.apk -o recovered-jadx --jadx /opt/jadx/bin/jadx
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

## 验证与支持深度

Python 仅用于下面的开发测试脚本；内置移动端恢复在原生 C++20 CLI 中运行。

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
```

组件和 CLI 测试检查解析、输出契约与失败清理。内置执行对照脚本使用 JDK（`java`、`javac`）和 D8 构建独立 DEX/APK 样例，再编译运行恢复的 Java；它们是测试依赖，不是内置恢复的运行要求。请针对当前构建执行并检查结果，再声称某个样例已经验证。独立兼容测试还需要 JADX，用于验证外部适配器。样例成功不代表任意应用都能完整恢复。

相关 iOS 流程见 [移动应用概览](mobile.md)。
