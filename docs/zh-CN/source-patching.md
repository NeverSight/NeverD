**Languages**: [English](../source-patching.md) | [简体中文](source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 1eae101089e730c29ba3fda92db515fe148fe41b758d6f5b69156252591d9b3f -->

[← Documentation Index](README.md)

# 源码与原生对象 patch

NeverD 使用指定编译器编译替换函数，再安装到明确的原函数入口。编译器负责 C/C++ 语言语义；NeverD 负责地址绑定、重定位、存储、展开记录和发布。二进制修复与提升后的重写继续使用各自管线，不需要重建程序其余部分。

<!-- i18n-section: compiler -->
## 选择编译器

使用 `--compiler=clang`、`--compiler=gcc` 或 `--compiler=neverc`。Clang 和普通模式的 NeverC 可以生成 LLVM IR；`object` 制品保留编译器生成的机器指令。CLI 对 GCC 默认选择对象；Python SDK 对 GCC 也默认选择对象。选定的编译器不会静默回退。

使用 `--compiler-arg` 或 SDK 的 `compiler_args` 传入包含路径、宏定义及受支持的源码选项。禁止覆盖目标或输出模式，也禁止在任何参数位置传入响应文件标记（`@file`），包括单独选项的值。普通的 `-I` 路径和 `-D` 宏定义仍然受支持。

示例地址须替换为已确认的原函数入口。替换声明必须匹配原机器 ABI，包括调用约定和结构体布局；地址本身不能证明声明正确。C++ 需要 `--language=c++` 和正确的 Clang++/G++ 路径。建议使用 `extern "C"` 入口，否则传入精确的修饰名。允许额外辅助函数。`--entry` 与 `--func` 共同明确选择替换目标；旧入口 `--from-c` 也会遵守指定地址。

也可明确指定 `artifact="object"`。

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## 已验证配置与限制

[NeverC producer: 182fd2c5dc](https://github.com/NeverSight/NeverC/commit/182fd2c5dc)

**NeverC C 与 hosted DynCode**

NeverC 在 x64/ARM64 ELF、PE、Mach-O 上只接收纯 C 源代码。NeverD 会在启动编译器前拒绝 NeverC 与 `--language=c++` 的组合，不做隐式翻译或编译器回退。C++ 与异常替换使用 Clang，或使用已验证 ELF 对象配置下匹配的 GCC。hosted C 栈帧可依靠真实展开记录，让外围 C++ 程序抛出并捕获的异常穿过；这不代表 NeverC 支持 C++。

`dyncode`: `--compile-mode=dyncode` 使用 NeverC 的 `-fdyncode-hosted`。它要求源代码、一个明确入口、原生对象且不允许原程序符号绑定。CLI 与 Python 对该模式和 GCC 默认选择对象。适配器校验对象与最终载荷摘要，安装器在发布前逐字节比较最终代码和入口偏移，并通过真实 CFI/SEH 或完整原生 compact 记录建立展开覆盖。可写/TLS 全局状态、隐式 syscall/PEB/heap/string 改写、未解析导入以及改动或重排的代码会明确失败；数据及回调指针可通过参数传入。

```sh
neverd patch app --from-source=replacement.cpp --language=c++ \
  --compiler=clang --compiler-path=/path/to/clang++ \
  --entry=replacement --func=0x401140 --patch-report -o app.cpp
neverd patch app --from-source=replacement.c --compiler=neverc \
  --compiler-path=/path/to/neverc --compile-mode=dyncode \
  --entry=replacement --func=0x401140 --patch-report -o app.dyncode
```

Clang 的 LLVM IR 路径也接入 Windows ARM64 C++ FH3，核对编译器拥有的 xdata、父帧 ABI 和准确的 funclet/FuncInfo 分组。原生对象的语言处理器仍未接入。`check_source_patch_cpp_runtime.py` 为 x64/ARM64 生成制品，在匹配的原生 Windows 主机执行九种补丁异常场景；CPU 执行与操作系统展开器验证仍分别记录。

| 编译器 | 制品 | 已验证的操作系统运行配置 |
|---|---|---|
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++；Mach-O x64/ARM64 C/C++；PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| Clang 22.1.6 | 原生对象 | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C; Mach-O x64/ARM64 C; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| GCC/G++ 13.3.0 | 原生对象 | ELF x64/ARM64 C/C++，需要兼容的 Itanium 运行库; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| NeverC 3389.1.5 普通模式 | LLVM IR | ELF x64 C；Mach-O x64/ARM64 C |
| NeverC 3389.1.5 普通模式 | 原生对象 | ELF x64 C |
| NeverC 3389.1.5 C | LLVM IR / 原生对象 | ELF / PE / Mach-O x64/ARM64: 纯 C 源代码 |
| NeverC 3389.1.5 `hosted-v1` | 原生对象 + 封存载荷 | ELF / PE / Mach-O x64/ARM64: 纯 C 源代码及穿过 C 栈帧的异常传播 |

上表是已验证的操作系统运行配置，不承诺所有编译器版本和选项。ELF、PE、Mach-O 的原生安装器均接入四种架构。GCC 需要匹配的 ELF 驱动，不能生成 LLVM IR。未知重定位、TLS、装载时的全局构造函数、LTO、common/IFUNC 符号及未解析导入均明确失败。JSON 凭据记录编译器身份、参数、绑定与安装地址，原生对象另记录 SHA-256。

原生对象回执中的 `target` 字段来自已加载映像的 ABI，包括 ARM ELF 浮点标志以及不同的 iOS/watchOS 平台。编译产生的对象与外部对象使用同一条身份规则。

CPU 矩阵要求十二种组合的源码、外部 IR 和原生对象均成功安装。另分别覆盖 ARMv7k watchOS、ARMv7 iOS、可写／只读状态、原程序浮点与栈参数绑定，以及改变加载地址后的入口执行。CPU 执行不能证明操作系统加载器、C++ 展开器或任意 ABI 正确；这些结论应依据上表的运行配置。

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

`CPU` 表示源码／IR／对象安装、重新加载及精确函数执行。ARM ELF 从镜像确定 EABI5、ARM/Thumb 状态和浮点 ABI，并注册完整排序的 `EHABI` 索引。PE 保留 x64/ARM 运行时记录与 `HIGHLOW`/`DIR64`/`MOV32T` 基址重定位；x86 使用编译器注册代码。Mach-O 根据 CPU 子类型区分 watchOS DWARF 与 iOS `SjLj`。源码的 ARM/Thumb 入口使用相对分支，可随镜像改变加载地址。

原生对象依据自身有界函数符号、辅助／区段定义及展开记录验证范围，不伪造 MC 所有者 ID，也不重新编译 IR。COFF 函数范围须来自精确辅助长度、单函数区段（`-ffunction-sections`）或解码后的原生运行时函数记录，不能按下一个符号猜测长度；watchOS 编译适配器保留叶函数栈帧以取得真实展开记录。原生 COFF/Mach-O 语言处理程序尚不属于已验证对象配置；C++ 异常应使用已验证 IR 路径。固化 exidx 注册表的静态 ARM 展开器、未注册的 Mach-O 绝对指针及不支持的 RELRO 存储均明确拒绝。

GCC C 对象也在 ELF x86、ARM32、ARM64 上完成安装，并通过 Linux QEMU user mode 执行。适配器从镜像选择 ARM/Thumb 模式，GNU property 记录按 ELF 指针宽度对齐。

<!-- i18n-section: references -->
## 原实现回调、布局扩容与新增依赖

绑定对应外部声明。`function` 验证原函数或导入桩；`data` 要求可写存储；`const` 要求常量 IR 声明。对象数据绑定必须给出非零范围，不能跨越存储边界或授予 RELRO 写权限。调用被替换函数的原实现时，必须显式使用 `original`。公开入口仍进入替换函数；保存的入口执行按完整指令重定位的前缀，再跳回原函数后续部分。源码、外部 IR 和原生对象共享此契约，包括浮点与栈参数和多个回调。

回调必须验证函数边界、指令模式、继续执行地址和展开证据。未知指令、不安全的 PC 相对引用、跳入复制前缀的分支、不支持的加载重定位、重叠回调和未建模的带处理器序言都会在发布前失败。调用者仍须提供 ABI 匹配的声明。凭据通过 `original_callbacks` 记录保存的入口地址。

PE32/PE32+ 可通过移动文件内存储范围并更新已建模的调试、符号、证书和附加数据指针，扩容已满的节头表，同时保留原 RVA。Mach-O x64/ARM64 PIE 在相邻 page-zero 保护区和末尾 linkedit 段满足条件时，可通过经过检查的头部前缀扩展取得命令和展开存储，原代码／数据地址保持不变。此配置不要求 `-Wl,-headerpad,0x400` 或相邻的空闲展开页。显式头部地址引用、未知加载命令、混合代码／数据归属、损坏范围和不支持的布局会被拒绝。安装前统一规划头部空间和展开容量。

新代码保持 RX，可变状态保持 RW。显式 `--dependency` 请求通过 ELF 动态元数据、PE 导入描述符／IAT 或 Mach-O dylib／bind 元数据新增函数或数据导入。原导入身份、序号和版本记录保持稳定；槽位与可调用桩保留各自含义。库名／路径应符合目标 OS 加载器的规则。下例使用 ELF 共享库名。执行时缺库或缺符号必须产生真实加载器错误。TLS、IFUNC、新符号版本契约、加载期构造器和未建模重定位仍不支持；ELF `.data.rel.ro` 和 CET/IBT 仍需额外契约。 原生 COFF 对象必须用 `__declspec(dllimport)` 声明新增函数／数据导入，使重定位明确指向 IAT；普通的直接外部引用会被拒绝。

```sh
# ELF example: the original target is at 0x401140.
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=original:saved_target=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --dependency=function:extra_function=libpatch_extra.so \
  --dependency=data:extra_state=libpatch_extra.so \
  --patch-report -o app.bound
```

追加式、按结构大小检查的依赖选项适用于源码、外部 IR 和原生对象请求。凭据包含 `added_imports`；重新加载候选文件后再次核对全部回调、导入、存储权限和安装字节。失败保留输入和任何已有目标文件。

<!-- i18n-section: examples -->
## 可运行示例

[完整 Windows/macOS/Linux 示例](../../examples/source-patch/README.md) 包含原程序、替换函数和依赖库源码、构建命令及真实宿主中的 Python SDK 插件。在仓库根目录运行，使用启用 Python 的 NeverD 构建和新的输出目录。

```sh
# Linux / macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-native
```

```powershell
# Windows: Visual Studio developer PowerShell
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-native
```

默认运行两条路径；可加 `--api cli` 或 `--api sdk` 单独选择。目标地址自动发现。输入 `7` 时，原程序输出 `8`，两种补丁均输出 `36`。驱动检查真实执行、凭据和缺库加载失败，完整构建与 patch 命令保存在 `commands.json`。

<!-- i18n-section: exceptions -->
## C++ 异常

ELF 使用编译器的 FDE/LSDA；Mach-O 使用紧凑展开/DWARF。PE x64/ARM64 支持编译器的 FH3 表，要求原程序已有兼容的 MSVC 运行库和类型身份；type-info 数据可能需要显式绑定。FH4、自定义 personality、SEH 源码和启用的 CFG/EHCont/XFG 模式会被拒绝，原保护标志保持不变。

当前明确拒绝带 IBT 要求的 ELF x86/x64 原程序：源码安装器尚未提供保留 ENDBR 的跳板。

运行用例覆盖正常返回、内部捕获、新异常被原调用者捕获、原被调函数的异常穿过替换函数、恰好一次 RAII 清理、重新抛出、noexcept 终止及未替换函数。原调用点必须允许异常传播；patch 不能恢复原编译器因函数不抛异常而删除的捕获路径。

<!-- i18n-section: publication -->
## SDK 与发布

追加式 C API 为 `neverd_patch_source`，选项结构为 `neverd_source_patch_options_v1`；`neverd_patch_source_report` 返回凭据。对象输入须提供字节长度。旧 API `neverd_patch_from_c` 继续可用。

输入先保存快照，候选文件暂存于目标旁，重新加载并核对每个请求入口后才独占发布。不覆盖现有文件或符号链接别名。缺少任一映射会使整个请求失败。拒绝 `--mode=inplace`。会话 IR 变换不能用于原生对象。Mach-O 严格验证签名并重新签名；不会静默剥离 PE 签名。

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="clang", compiler_path="/path/to/clang", artifact="object",
    bindings={
        "saved_target": ("original", original_address),
        "shared_counter": ("data", original_counter_address, 4),
    },
    dependencies={
        "extra_function": ("libpatch_extra.so", "function"),
        "extra_state": ("libpatch_extra.so", "data"),
    },
)
```

<!-- i18n-section: verification -->
## 构建与验证

六个选定的原生 OS／ISA 任务均零失败、零跳过：3669 次 GoogleTest 执行、1008 次原生进程观测（504 次功能、504 次 C++ 异常）、12 次真实加载拒绝、96 个镜像和 84 份发布凭据。

| 主机 | 源码 | Harness | CI 任务 | 测试 | 观测／拒绝 |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

这些产物只验收下表的实际源码与 harness 提交；后续 dev 整合和文档提交另有本机及文档检查。早期失败或取消的任务仍保留为失败。历史 216 次补充观测、PE ARM32 及旧 32 位 Apple 的 CPU／兼容层结果，不构成新能力的原生硬件或 C++ 异常证明。

Windows ARM64 验收使用原生 ARM64 Unicorn C DLL：以固定子模块和已验证的 LLVM-MinGW 工具链真实构建，生成 MSVC 导入库并显式覆盖 CMake 目标。隔离的构建副本认证三项主机修正：非 SEH 跳转函数对、带指令缓存同步的 ARM64 页面提交回调，以及适配 Windows LLP64 的 64 位立即数掩码。子模块源码及版本不变。这证明 CPU 矩阵测试主机可用，不代表默认 MSVC ARM64 JIT 已受支持。五项主机恢复检查与补丁进程的原生执行、C++ 异常证据分开记录。

通过 `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` 指定兼容的源码编译器（已验证 Clang 22.1.6）。集成构建只修正构建目录中的 LLVM 副本，不改子模块或共享预编译缓存。r3/r4 预编译版的 x64 PLT 问题可能导致 IR patch 被拒绝；原生对象避开该 MC 路径。`NEVERD_TEST_REQUIRE_FIXED_LLVM=1` 要求编译器生成精确且已修正的 PLT 引用。

`NeverDSourcePatchTests` 包含原生 C/API/对象/异常用例；`NeverDSourcePatchMatrixTests` 使用 Unicorn 检查所有目标组合。配置 `NEVERD_TEST_WINDOWS_CRT_LIB`、`NEVERD_TEST_WINDOWS_UCRT_LIB` 和 `NEVERD_TEST_WINDOWS_SDK_LIB` 后，`SourcePatchPE` 执行七种 C++ 场景。非 Windows 宿主还需要 `NEVERD_TEST_WINDOWS_RUNNER`。`NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` 使缺少运行输入成为失败。Windows 专项工作流要求原生执行且拒绝跳过用例。

开发 Mac 上的 Linux 证据通过 Docker 和显式 Linux 加载器取得；PE 证据使用 Wine 11。Mach-O ARM64 在 Mac 本机运行。Windows 原生 CI 结果须单独查看：配置了工作流不等于运行通过。历史数量、基线失败和模拟环境限制见[详细契约与验证记录](../source-patching-details.md)。

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

LLVM 构建副本还修正了 Mach-O x64 的 GOT 数据引用：Clang 的类型捕获表以四字节重定位字段末尾为基准。r3/r4 预编译版会偏移四字节；发布前检查会拒绝未修正的结果。开发 Mac 的 x64 Mach-O 用例通过 Rosetta 2 执行，ARM64 用例直接执行。

Windows x64 原生验收 [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805) 的 16 个 PE/对象/trampoline 专项用例和 143 个 CPU 目标矩阵用例全部通过，无跳过。FH3 用例执行了七种 C++ 运行场景；目标矩阵使用 Unicorn。该轮使用 Clang 22.1.6、原生 MSVC、Windows SDK 和集成 LLVM。

Windows 工作流还会为 x86、x64 和 ARM64 生成 27 个 PE 替换产物。`scripts/check_source_patch_pe_runtime.py` 通过源码 IR、编译对象和外部对象三条路径检查非叶函数调用、可变指针状态及浮点/栈参数。每个原程序和替换产物均运行三次，即每种架构 30 次进程观测。x86/x64 在 Windows x64 上运行，ARM64 在 `windows-11-arm` 上运行。验证器要求原生主机架构、提交和文件摘要匹配，并检查用例清单完整；外部运行器的结果单独标记。这组进程测试不代表 ARM64 C++ 异常支持。

可通过 `NEVERD_TEST_MATRIX_NEVERC` 选择矩阵的可选编译器。NeverC 3389.1.5 在 x64/ARM64 × ELF、PE、Mach-O 的 66 个检查中全部通过，包括 IR 和原生对象安装及 CPU 执行。该编译器版本不支持 x86 或 ARM32；这些目标使用 Clang，ELF 也可使用匹配的 GCC 驱动。NeverC 编译 PE 时，适配器加入 `-fno-autolink`：patch 在现有映像中解析引用，不再链接另一套 CRT。带有 `.drectve` 链接契约的外部对象仍会被拒绝。

Windows 原生进程验收 [a4a5ae3cb](https://github.com/NeverSight/NeverD/actions/runs/38350079817) 的 90 次观测全部通过：x86、x64 和 ARM64 各 30 次，覆盖源码 IR、编译对象和外部对象三条路径。x86/x64 在 Windows x64 上执行；ARM64 在 `windows-11-arm` 上执行，使用同一批经过哈希校验的映像。同一轮的 16 个 PE/对象/trampoline 专项检查和 143 个 Unicorn 目标矩阵检查也全部通过，无跳过。FH3 用例继续验证七种 x64 C++ 运行场景。

原生生产语料固定 NeverC `182fd2c5dc`，使用独立原程序和当前 NeverD 安装器。`check_source_patch_hosted_pe_runtime.py` 在每种 Windows 架构要求 36 次 C IR/对象/hosted 结果与两层 hosted C 展开观测；`check_source_patch_elf_runtime.py` 在每种 Linux 架构要求 120 次 C、hosted 传播和九种 Clang C++ 异常场景观测。凭据、文件摘要及真实主机架构必须一致；这些检查不代表 CI 主机执行了 NeverC 编译器本身。 Linux 清单在 x64、ARM64 上分别覆盖 Clang IR、Clang 原生对象和 GCC 原生对象各九种异常场景；NeverC 仍仅处理 C。

旧翻译配置的 204 次观测凭据仅保留为历史记录，不能证明当前纯 C 编译契约。当前三条 NeverC 路径都使用 C，Clang C++ FH3 另行验证。开发用 Apple Silicon Mac 上的 macOS x64 执行使用 Rosetta 2；Unicorn/QEMU 的 CPU 结果不能代替原生操作系统与异常展开验证。

AArch64 指令重定位要求显式 RELA 加数。隐式 REL 指令加数会在发布前被拒绝，不会把指令位域当成普通数据。受支持的 PREL64 数据重定位在 REL、RELA 下均保留完整有符号加数。

纯 C 生产端验证 [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591930213) 已通过全部 312 次原生进程观测：Windows x64、ARM64 各 36 次，Linux x64、ARM64 各 120 次。Linux 分别验证 Clang IR、Clang 原生对象和 GCC 原生对象的九种异常场景；NeverC 的 IR、对象和 hosted DynCode 输入均为 C。

重新构建安装器的验证 [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591928600) 已通过 19 项 PE/对象专项检查、全部 143 项 CPU 矩阵和 474 次 Windows/Linux 原生进程观测。Windows x86 进程在 Windows x64 主机运行；x64、ARM64 使用匹配主机。独立的 [Linux 验收](https://github.com/NeverSight/NeverD/actions/runs/38585759996) 已通过原来的 108 项及额外 3 项 LLVM-C、3 项 Clang/GCC C++ 检查，零失败、零跳过。

[合并后验收 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627694097) 已在 Windows/Linux 的 x64 与 ARM64 原生主机上重新通过全部 312 次进程验证。30 个最终映像与之前验证的字节完全一致；源代码摘要及纯 C 发布回执已更新并独立核验。

[0a6f6d768 的扩展 Linux 验收](https://github.com/NeverSight/NeverD/actions/runs/38626866257) 执行了 429 项检查，零失败、零跳过：原始 108 项、完整 Swift 55 项、CPU 目标矩阵 143 项、原生 Session 117 项、LLVM-C 3 项及 Clang/GCC C++ 3 项。该合并版本的本地完整二进制 patch 回归也通过全部 22,961 项。原生进程次数与 CPU 模拟验证范围分别统计。

[0a6f6d768 的 Windows 安装器重建验证](https://github.com/NeverSight/NeverD/actions/runs/38627581732)通过全部 182 项专项检查和 474 次原生进程执行，零失败、零跳过。独立产物审计核对了每个镜像的哈希、架构、场景和重复次数。随后合入 ABI/展开修复的 `e1cac949f` 再次通过全部 22,961 项完整二进制 patch 回归，零失败、零跳过。这些结果遵守 NeverC 仅支持纯 C 的约定；C++ 替换使用 Clang/GCC。

<!-- i18n-section: runtime-coverage -->
## macOS 与 32 位进程验证

进程镜像工作流要求在 macOS ARM64（`macos-15`）和 Intel x64（`macos-15-intel`）上原生执行。每种架构执行 96 次观测，覆盖 Clang C 源码／对象安装、可写指针状态、浮点／栈参数、九种 Clang C++ 异常场景，以及现有 NeverC 纯 C 的 IR／对象／hosted 输出。执行前严格检查 Mach-O 签名；安装的 hosted 代码必须与编译器载荷逐字节相同。

同一工作流要求 Windows x64 上的 Windows x86 执行 30 次、Linux x64 上的 Linux x86 执行 48 次，并通过 QEMU 用户模式执行 96 次 Linux ARM32 观测，ARM 与 Thumb 各占一半。Linux 检查包含匹配目标的 GCC 对象。每种场景重复三次。报告区分 `native`、`compatibility`、`rosetta` 和 `qemu-user`，同时记录目标架构与实际宿主架构。Rosetta 不能满足 Intel 原生执行要求。

全部 14 个 CPU 目标配置仍由 283 项源码／IR／对象矩阵覆盖。PE ARM32、Mach-O i386、ARMv7 iOS 和 ARMv7k watchOS 目前有 CPU 验证，不据此宣称原生 OS 加载器或 C++ 展开器验证通过。NeverC 继续只支持 x64／ARM64 纯 C；32 位编译由 Clang 和匹配目标的 ELF GCC 驱动承担。验证器不会构建或修改 NeverC。

Windows x64 和 ARM64 还会各自执行 36 次 Clang C++ FH3 观测。

Mach-O 的可写指针用例在执行时建立指针；静态地址初始化仍需尚不支持的运行时重定位注册，并有明确拒绝的回归测试。ELF 零大小 COMDAT 函数仅在签名是其可执行节起点处的唯一函数、且 CFI 精确覆盖整个节时才被接受。节内偏移、共享、填充、无分组和缺少 CFI 的情况继续拒绝。

ELF ARM 与 Thumb 使用独立配置。源码编译器明确接收 `-marm` 或 `-mthumb`，原生函数绑定保留原函数的指令状态位。32 位 ELF 进程使用具有真实动态重定位的 PIE 镜像。ARM32 QEMU 执行会记录显式 glibc `--loader` 和 sysroot：QEMU 的 AUXV 计算没有像 Linux 内核那样处理迁移后的程序头。该路径验证 QEMU 下的 glibc 加载与执行，不代表 ARM32 真机或原生内核加载器验证。

对于这些补丁后的 ELF 镜像，Linux x86 与 ARM32 的 `qemu-user` 路径都需要显式使用匹配目标架构的 sysroot 中的 glibc `--loader`。Linux x86 的 `compatibility` 路径使用 Linux x64 宿主内核的 32 位兼容支持，并与 QEMU 执行分别记录；它不能证明在原生 32 位 x86 主机上执行。

[`fcee81c9a5e2f20a71090ade23ae833175f64c59` 的进程验证](https://github.com/NeverSight/NeverD/actions/runs/38694434082)通过全部 750 次观测：匹配主机上的 576 次原生执行、78 次 x86 兼容模式执行，以及 QEMU 用户模式下的 96 次 ARM／Thumb 执行。独立产物审计核对了全部 126 个镜像的哈希、执行身份和完整场景清单。[Windows 重建验收](https://github.com/NeverSight/NeverD/actions/runs/38694628242)通过 194 项专项检查和 474 次 Windows／Linux 进程观测。[Linux 重建验收](https://github.com/NeverSight/NeverD/actions/runs/38694638358)通过 444 项检查，包含原来的全部 62 项失败和 46 项跳过，零失败、零跳过。这些结果保留上述硬件与加载器验证边界；NeverC 仍为纯 C，未修改其仓库。

补充的 [ARM/Thumb 验证](https://github.com/gmh5225/test_mac_intel/actions/runs/38750504263)在 `ubuntu-24.04-arm` 上执行了 `fcee81c9a5e2f20a71090ade23ae833175f64c59` 的 32 个 C 测试镜像，全部 96 次观测通过（ARM 与 Thumb 各 48 次）。Linux 内核通过 `/lib/ld-linux-armhf.so.3` 直接加载镜像，未使用 QEMU 或显式加载器命令。这属于 ARM64 主机上的 `compatibility`，不是原生 32 位主机验证。测试入口提交 `3ad17ac9608d6bd9f5a4d44953ec375524e5730b` 与镜像提交分别记录。独立产物审计核对了镜像哈希、全部 30 份发布回执、完整场景及重复次数，并确认没有匹配这些镜像的 `binfmt_misc` 解释器规则。

另行执行的[托管 runner 探测](https://github.com/gmh5225/test_mac_intel/actions/runs/38749138719)确认，所提供的 Windows 11 ARM64 runner 以 `WinError 216` 拒绝启动 ARM32 程序。在 macOS 15.7.9 和 Xcode 16.4 环境中，现有 SDK 无法链接 macOS i386 或 iOS ARMv7 程序。watchOS ARMv7k 程序链接成功，但未取得实体设备执行证据。探测任务成功不等于这些平台通过验证；其原生硬件覆盖仍未补齐。NeverC 未被构建或修改。

补充的[个人 Actions Linux 验证](https://github.com/gmh5225/test_mac_intel/actions/runs/38773830274)在完整 QEMU TCG ARM32 客体中通过了 96 次 ARM/Thumb 执行，客体使用 32 位 Linux 内核和 glibc；同一客体内的 Wine 另通过 30 次 PE ARM32 执行。ELF 镜像由客体内核直接加载，这是整机模拟，与需要显式加载器的 `qemu-user` 路径分别记录。ARM64 宿主兼容层上的 Wine 注册表安装仅作为安装证据，计入零次补丁执行。客体内的 Wine 初始化和每次测量执行都必须在原定时限内成功。

[Apple 兼容性验证](https://github.com/gmh5225/test_mac_intel/actions/runs/38771214358)通过了 Linux 上 Darling 的 30 次 Mach-O i386 执行，以及 Intel macOS 上 touchHLE 的 60 次 ARMv7/ARMv7k 执行。每个原始镜像以及源码 IR、编译对象、外部对象替换结果均执行三次。独立审计核验了这些补充运行中的全部 72 个镜像、66 份补丁发布记录和 216 次执行，包括 GitHub 产物大小、SHA256、精确提交及完整用例清单。Linux 输入仍固定于 `fcee81c9a5`；PE 输入生成于 `a542f97ba`，Mach-O 输入生成于 `8f5f06762`。验证脚本的提交号在对应运行中单独记录。

Mach-O 修复按所选函数认证 Thumb 模式，避免被镜像整体的 ARM/Thumb 混合模式覆盖；仅规范化有证据的 Thumb 导出指针，并统一 ARM32 展开代码地址，不改变数据指针或范围长度。集成 LLVM 源码构建在本机通过全部 283 项 CPU 矩阵检查和 258 项异常契约检查，零失败、零跳过。旧 Mach-O 测试程序使用已记录的开源链接器并预留展开元数据容量；i386 使用正确对齐的启动入口和 64 KiB 页零段。

这些结果证明指定模拟器和兼容层下的 C 代码执行。Wine 不等于 Microsoft Windows，Darling 不等于 macOS；iOS HLE 中的 ARMv7k 执行不能证明 watchOS、WatchKit 或 Apple 实机支持。不能据此推断旧 Apple SDK 或原生 32 位硬件覆盖，这些测试也不认证 C++ 异常展开。既有原生与兼容模式证据继续分开记录。NeverC 未构建、未修改。
