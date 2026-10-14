**Languages**: [English](../source-patching.md) | [简体中文](source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: eb2a70d2b1d15c943ee92c3ca762c2328fdf4f9c742fc05a8e52771179931675 -->

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
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++；Mach-O x64/ARM64 C/C++；PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3 |
| Clang 22.1.6 | 原生对象 | ELF x64 C/C++; PE x86/x64/ARM64 C |
| GCC/G++ 13.3.0 | 原生对象 | ELF x64 C/C++，需要兼容的 Itanium 运行库 |
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
## 原程序引用与存储

绑定对应外部声明：`function` 要求已确认的原函数或导入桩；`data` 要求可写存储；`const` 要求 IR 中为 const 声明。对象数据绑定必须提供非零范围。范围不能跨越存储边界，也不能赋予 RELRO 写权限。不能回调被替换函数的原始函数体：此配置尚无保存原序言的回调跳板。

新代码保持 RX，新可变状态保持 RW。只复用已有导入，并区分 GOT/IAT 槽与可调用桩。Mach-O 需要空余加载命令空间，例如 `-Wl,-headerpad,0x400`；PE 需要空余节头空间。空间不足会失败。ELF `.data.rel.ro` 和原 CET/IBT 保护需要额外契约，目前明确拒绝。

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Mach-O 展开表增长还需要相邻的空余文件与虚拟空间；仅预留头部空间并不能满足这一要求。测试样本分别处理这两种空间限制。

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
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## 构建与验证

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

原生生产语料固定 NeverC `182fd2c5dc`，使用独立原程序和当前 NeverD 安装器。`check_source_patch_hosted_pe_runtime.py` 在每种 Windows 架构要求 36 次 C IR/对象/hosted 结果与两层 hosted C 展开观测；`check_source_patch_elf_runtime.py` 在每种 Linux 架构要求 66 次 C、hosted 传播和九种 Clang C++ 异常场景观测。凭据、文件摘要及真实主机架构必须一致；这些检查不代表 CI 主机执行了 NeverC 编译器本身。

旧翻译配置的 204 次观测凭据仅保留为历史记录，不能证明当前纯 C 编译契约。当前三条 NeverC 路径都使用 C，Clang C++ FH3 另行验证。macOS x64 使用 Rosetta 2；Unicorn/QEMU 的 CPU 结果不能代替原生操作系统与异常展开验证。