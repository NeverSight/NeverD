**语言**: [English](android.md) | [简体中文](android.zh-CN.md) | [繁體中文](android.zh-TW.md) | [日本語](android.ja.md) | [한국어](android.ko.md) | [Français](android.fr.md) | [Deutsch](android.de.md) | [Español](android.es.md) | [Italiano](android.it.md) | [Русский](android.ru.md) | [العربية](android.ar.md)

# Android Java 代码恢复

[← 文档索引](README.zh-CN.md)

`neverd mobile` 通过单独安装的 JADX 后端，把 APK、DEX 和 smali 恢复为可读 Java。流程会验证并暂存字节码、联合分析相关类、检查生成结果，最后发布源码目录和机器可读报告。目前是实验性 CLI 功能，原生 C SDK、Python 插件 SDK、GUI 加载器和 `neverd decompile --language` 尚未提供 APK 容器或 Java 输出接口。

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
| NeverD | 构建 `neverd` 目标，分发时保留可执行文件旁的 `mobile/` 目录 | `build/bin/neverd` 或 PATH 中的可执行文件 |
| Python | Python 3.10 或更高版本，独立于嵌入式插件宿主 | `--python`、`NEVERD_PYTHON`、PATH 中的 `python3`/`python` |
| Java 反编译后端 | JADX 1.5.6 或更新版本及标准 DEX、smali 输入插件 | `--jadx`、`NEVERD_JADX`、PATH 中的 `jadx` |
| Java 运行时 | Java 11 或更高版本；编译运行验证还需要 JDK | `JAVA_HOME` 或 PATH 中的 Java |

NeverD 不会自动下载依赖。请获取完整的 [JADX 发行包](https://github.com/skylot/jadx/releases/tag/v1.5.6)，保留 `bin/` 与 `lib/` 的目录关系；再分发时保留附带的许可证。已验证的后端版本为 1.5.6，后续版本需满足相同 CLI 契约。依赖配置与 NeverD 的 LLVM 管线构建分开进行。

### Linux 与 macOS

```sh
cmake --build build --target neverd
python3 --version
java -version
/opt/jadx/bin/jadx --version

./build/bin/neverd mobile app.apk -o recovered-app \
  --python python3 --jadx /opt/jadx/bin/jadx
```

重复使用时，可设置 `NEVERD_JADX=/opt/jadx/bin/jadx`，并按需用 `NEVERD_PYTHON` 指定解释器路径。如果环境中尚无 Java，把 `JAVA_HOME` 指向 JDK 安装目录。含空格的路径必须加引号。

### Windows PowerShell

```powershell
$env:JAVA_HOME = 'C:\Tools\jdk'
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app `
  --python 'C:\Tools\Python\python.exe' `
  --jadx 'C:\Tools\jadx\bin\jadx.bat'
```

传入后端的 `.bat`/`.cmd` 路径时，NeverD 会定位发行包中唯一的 `lib/jadx-*-all.jar`，再直接启动 Java。也可以把该 JAR 直接传给 `--jadx`。应用路径不会拼接进命令解释器。多配置构建可能把程序放在 `build/bin/Release/`。只移动可执行文件、遗漏 `mobile/` 目录会出现辅助程序缺失错误。

## 支持的输入与范围

| 输入 | 处理方式 | 重要限制 |
|------|----------|----------|
| `.apk` | 验证整个 ZIP，再一起分析根目录的 `classes.dex`、`classes2.dex` 和后续编号 DEX | 仅恢复代码，不解码资源或 Manifest |
| `.dex` | 验证 DEX 魔数，再由后端解码内容 | 改名或截断的文件不等于有效字节码 |
| `.smali` | 分析指定类 | 不会自动加载被引用的相邻类 |
| smali 目录 | 递归收集 `.smali` 并联合分析 | 输入目录中应包含嵌套类和相关 smali 根目录 |

分析包含 `smali/` 和 `smali_classes2/` 的 APK 解码目录时，传入它们的公共父目录。只有 `.smali` 文件会交给后端，但整个输入目录会先验证和复制；无关的大型资源也会占用输入限额。把相关 smali 根目录放进一个精简目录可以减少工作量。

Split APK 视为独立输入。含 DEX 的每个 APK 可单独处理，但此命令不会合并 APK 集合；只有资源的 split 因不含根目录 DEX 而失败。`.aab`、`.apks`、`.xapk`、`.odex`、`.oat` 和 `.vdex` 不是当前支持的移动输入。后端本身支持某个格式，不代表 NeverD 的此命令也支持它。

APK 资源、`AndroidManifest.xml`、assets、JNI/原生库和运行时下载代码不在 Java 恢复范围内。原生 `.so` 可单独提取后执行 `neverd decompile library.so -o library.c`。加密或加壳载荷需要事先以普通 DEX/smali 形式可用；本静态流程不执行脱壳、设备附加或保护绕过。

## 参数与优先级

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --jadx /opt/jadx/bin/jadx --timeout=600 \
  --max-files=30000 --max-bytes=4294967296 --json
```

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `-o DIRECTORY` | 必填 | 尚不存在的输出目录，不能位于目录输入内部；不覆盖已有输出 |
| `--platform=auto\|android` | `auto` | 明确选择 Android，或自动识别输入平台 |
| `--jadx PATH` | 环境变量/PATH | 后端启动器或发行包 JAR，命令行显式参数优先 |
| `--python PATH` | 环境变量/PATH | 运行辅助程序的解释器，命令行显式参数优先 |
| `--timeout N` | `300` | 每个后端进程的超时秒数，必须为正；包含版本探测 |
| `--max-files N` | `20000` | 条目数量上限，包含实际创建的目录，必须为正 |
| `--max-bytes N` | `2147483648` | 输入、解包数据和最终输出的字节上限，必须为正 |
| `--json` | 关闭 | 向标准输出打印 JSON 报告，而非人类可读摘要 |

非默认 `--arch` 选择、`--artifact`、`--metadata-only` 和非零 `--max-func` 属于 iOS，对 Android 使用会报错；显式 `--arch=auto` 可以接受。此命令不提供任意后端参数透传。每次运行使用独立的后端配置、缓存和临时目录，不会导入环境中已有的后端设置或插件配置。

这些限制是资源控制，不是后端进程的安全沙箱。暂存工作区也会监控，为原输入、解包数据及生成输出最多预留三倍条目/字节预算。每个进程的日志上限为 16 MiB。大型输入仍可能需要更多 Java 堆内存或更长超时；增大某一限额不会关闭其他限制。

## 输出目录与 JSON 报告

```text
recovered-app/
  sources/                 恢复的 Java 包和类
  logs/jadx-version.log    后端版本探测结果
  logs/jadx.log            后端诊断
  report.json              带版本号的输入清单和恢复限制
```

临时副本和后端缓存会清理。Java 文件名和数量取决于后端重建方式，嵌套类可能放在外部类的同一源码文件中，因此 Java 文件数不等于 DEX 类数。

下面是经过缩短的示意报告：

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {"name": "jadx", "version": "1.5.6"},
  "input_code_files": ["classes.dex", "classes2.dex"],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": ["sources/example/Main.java", "sources/example/Peer.java"],
  "logs": ["logs/jadx-version.log", "logs/jadx.log"],
  "limitations": ["Java is reconstructed from bytecode; original comments, formatting, and stripped names cannot be restored."]
}
```

`input_kind` 为 `apk`、`dex`、`smali` 或 `smali-directory`。`input_code_files` 是输入字节码名称或 smali 路径清单；`java_sources` 和 `logs` 相对于输出根目录；`source` 是输入的基本文件名。真实报告还会包含其他恢复限制，把结果交给其他工具展示时应保留这些信息。

自动化时先检查进程退出码，再读取 `status`。重定向标准输出时，把报告放在待创建的输出目录之外：

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

辅助程序成功返回零，恢复失败返回非零。在受支持的解释器上启动辅助程序后，`--json` 的错误对象包含 `schema_version`、值为 `"error"` 的 `status` 和 `error`。原生参数解析、Python 缺失、Python 版本低于 3.10 或辅助程序缺失可能在更早阶段失败，仅输出 stderr；中断也可能通过 stderr 报告。调用方必须兼容这些情况。

## 失败处理与排查

输出按事务方式发布：保留已有目录，清理失败的暂存结果。后端返回非零、记录组装/反编译错误、因重复类遗漏代码、出现明确的不完整代码标记、生成空 Java 文件或没有 Java 输出，都会使命令失败。后端报告成功本身仍不是方法级正确性的独立证明。

| 现象 | 处理建议 |
|------|----------|
| Python 或辅助程序缺失 | 安装/选择 Python 3.10+，并把同级 `mobile/` 目录与 NeverD 一起保留 |
| 后端无法执行或版本不支持 | 核对 `--jadx`、完整发行包布局、Java 和最低后端版本 |
| DEX 头部无效/没有根目录 DEX | 核对真实格式，使用包含代码的 APK、普通 DEX 或 smali |
| 没有 smali 文件 | 指向含 `.smali` 的目录，而非 Java 源码或只有资源的目录 |
| 重复类或部分恢复 | 移除重复输入定义或分别分析相关字节码集合；修复损坏 smali，不能接受不完整结果 |
| 超时或字节/条目限额 | 缩小到相关输入，或明确调大对应限额 |
| 不安全归档路径或链接 | 重新准备普通可移植输入，移除穿越路径、链接、特殊文件和冲突路径 |
| 输出目录已存在 | 换一个输出目录，不复用之前成功的目录 |

成功运行会保留后端日志，失败的暂存目录及其中日志会删除。后端非零退出时，错误中包含长度受限的诊断尾部；超时和资源限额失败则给出各自的诊断消息。需要进一步排查后端时，用其独立 CLI 在隔离输入和另一个诊断目录中复现。不能因为失败前出现过若干 Java 文件就判定成功。

## 验证与支持深度

```sh
cmake --build build --target check-neverd-mobile
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

前两个命令验证移动组件和已构建 CLI 的契约；平台专属 fixture 所需工具缺失时会明确跳过。真实后端 runner 还需要 JDK 中的 `java` 和 `javac`。它构造单 smali、跨类/嵌套 smali、DEX 和真实多 DEX APK，随后编译运行生成的 Java；样本涵盖分支、循环、数组、异常处理、类引用、损坏输入和重复类遗漏。这些证据适用于所测样本，不代表任意应用都能完整恢复。

[Mobile Decompilation 工作流](../.github/workflows/mobile.yml) 在 Linux、macOS、Windows 的 Python 3.10 和 3.13 上运行组件测试，并在 Linux 使用校验和固定的真实 Android 后端。独立的 iOS 工作流及当前限制见[移动应用总览](mobile.zh-CN.md)。
