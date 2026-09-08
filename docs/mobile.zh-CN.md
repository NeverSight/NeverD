# 移动应用逆向支持

[← 文档索引](README.zh-CN.md) · [完整 Android 指南](android.zh-CN.md)

`neverd mobile` 支持将 Android APK、DEX、smali 恢复为可读 Java。对 iOS IPA、`.app` 和 Mach-O，命令导出可恢复的运行时元数据，并复用 NeverD 原生反编译器输出 C 伪代码。目前这是实验性的 CLI 功能；原生 C SDK 和 GUI 加载器尚不支持移动应用容器。

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

`sources/` 保存 C 伪代码，`metadata/` 保存可恢复的 Objective-C 声明和运行时/符号元数据，选中的单架构二进制保留在 `artifacts/selected.macho`，临时解包内容会清理。`report.json` 记录架构、所选文件、输出、恢复函数数量及限制。只有头部内容、没有恢复出函数体的输出会失败。`--metadata-only` 跳过原生反编译；`--max-func N` 限制函数数量，零表示全部发现的函数。

Objective-C 声明恢复取决于运行时元数据和指针编码。Swift 符号名称可能仍可用，但不等于能恢复 Swift 原始源码。符号裁剪、优化、缺失元数据和未支持的指针修正都会限制结果。本功能不还原原始 Swift 或 Objective-C 方法体，方法实现以 C 伪代码表示，原生类型和调用约定也可能需要手动修正。空声明列表需要结合报告中的限制判断。

## 限制与失败处理

`-o` 必须指定不存在的目录，且不能位于目录输入内部。已有输出不会覆盖，只有恢复和验证成功后才发布结果。`--json` 输出带版本的报告；失败返回非零状态，辅助进程启动后的错误支持 JSON。

默认最多 20,000 个条目、2 GiB 输入/解包或最终输出数据，每个后端进程最多 300 秒。可用 `--max-files`、`--max-bytes` 和 `--timeout` 调整，均必须大于零。后台运行期间也会监测临时输出工作区，每个进程的诊断最多 16 MiB。解包拒绝路径穿越、符号链接、特殊文件、大小写路径冲突和加密 ZIP 条目；目录输入也拒绝链接和特殊文件。

这些限制用于增强健壮性，不是第三方后端的安全沙箱。失败的临时结果会清理，后端进程错误会附带长度受限的诊断尾部。

## 验证

```sh
cmake --build build --target check-neverd-mobile
python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

测试覆盖不安全容器、损坏或加密的原生输入、后端失败、Java 恢复不完整、架构选择和已有输出保护。真实 CLI 测试通过 `NEVERD_BUILD_DIR` 选择构建目录。真实 Android smoke runner 还需要后端和 JDK，会恢复自建 smali/DEX/多 DEX APK，然后编译运行生成的 Java。
