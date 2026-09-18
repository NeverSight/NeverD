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

普通辅助函数的完整入口值经 COPY/PHI 传到已有源码绑定的指针参数，且不存在冲突的标量用途时，可以为源码投影补全指针类型。这种推断保留物理 ABI 位置和通用 IR 类型；函数体及其完整原生依赖组仍须通过验证。

如果原生标量辅助函数的已观察入口参数位于确切的返回寄存器，且覆盖完整结果宽度，则可证明部分路径原样返回该参数。有界分析同时检查每条返回路径、初始入口和循环回边；调用及部分写入会使该值失效。未声明的入口值、未使用的参数占位、SSA 初始标记及 PHI 均不能建立输入证明。这只用于源码投影，保持物理位置不变，且仍要求完整验证函数体及其依赖。

本地原生辅助函数可将额外寄存器中的完整宽度整数输入（包括结果缓冲区指针）恢复为显式参数，但入口可观察性分析必须与完整 LowIR 中的读取证据一致。调用方保存的输入寄存器之后可用作临时寄存器；若写入保持型上下文，则必须由独立的字节级证明确认每个出口都恢复入口时的保持寄存器、栈指针和链接寄存器。若该上下文寄存器本身会被覆盖，其完整入口身份还必须先流入已有绑定调用、地址计算等非保存用途；COPY、返回以及私有栈帧中的保存/恢复流量本身不会产生源码参数。调用产生的隐式定义（包括 SSA 版本 0）不能变成入口输入，只有明确保留的字节可跨调用追踪。重新生成的调用方和函数定义共享相同的物理输入位置。这不推断外部 C 或 Swift 原型，完整函数体和依赖闭包验证仍为必需。

固定 C 回调类型在源码声明和转换中保留参数及返回签名。精确的 `swift_once` 导入绑定初始化标记、`void (*)(void *)` 回调和上下文，无返回值。这会保留运行时调用，但不证明回调体已恢复，也不证明共享初始化存储的所有权；依赖不完整的方法仍不计为已恢复。

已知的 Objective-C 运行时导入调用具有明确的参数和返回值绑定，包括 retain/release、自动释放、强弱引用存储、对象分配及固定签名的属性设置函数。ARM64 的寄存器专用 retain/release 入口读取名称指定的寄存器，并输出对应的普通运行时调用。导入身份与 ABI 必须一致，名称相似的任意函数不符合条件。macOS 上的 ARC 重编译测试对比原始方法，检查强引用生命周期、弱引用清零、属性复制和析构行为。

对 libobjc 优化后的类型和选择子查询，保留精确的运行时调用，包括空对象处理和自定义重写。返回字节保留调用方的原生转换，不替换为猜测的类继承关系检查。

关联对象的读取、写入和清除调用保留对象、键、值及指针宽度的策略参数。生成的 C 使用公开的 Objective-C 运行时头文件；可执行回归测试对比原始方法的保留、复制和清除行为。

Objective-C 导出还支持一组固定的、使用普通 C ABI 的 Swift 运行时导入：引用计数、原生及未知对象弱引用、对象元数据和访问检查的开始与结束。生成的 C 保留这些调用，链接时需要 Swift 运行时。专用寄存器入口、未知的 Swift 调用约定和任意 Swift 符号仍不支持；导入身份及标量载体必须精确匹配。

独立绑定支持 arm64 和 x86_64 上精确的 Darwin String → NSString 导入 `_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF`，以及可选 NSString → String 导入 `_$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ`。两者均保留所有权语义和 Clang `swiftcall`；链接需要 Swift Foundation 与 Swift Core。反向桥接将返回的两个 String 机器字作为无符号 128 位整数传递，在 SSA 建立前拆入显式返回寄存器。这只是位载体，不代表已恢复 String 布局或通用聚合 ABI。未知签名和不完整的初始化依赖仍不受支持。

对于已确认的关联对象 key 参数，NeverD 可将只读 Mach-O C 字符串区域内的确定地址重建为共享的键标识。同一原始地址共用一个键，不同内部偏移保持不同身份。mobile 导出会自动合并辅助函数；C API 在 `shared_identity_functions` 中列出它们的名称，跨方法源码文件链接时，每个辅助函数只保留一个定义。这些键属于重新构建的代码，不指向已加载的原始映像；其他用途的未绑定映像地址仍会阻止完整恢复。

经过验证的 `os_unfair_lock_lock`, `os_unfair_lock_unlock`, `os_unfair_lock_trylock`, `os_unfair_lock_assert_owner`, `os_unfair_lock_assert_not_owner` 导入通过 `<os/lock.h>` 调用真实的 Darwin 实现，保留锁地址、所有权检查和布尔返回值。未知变体仍不受支持。整数调用结果只保留声明的 ABI 位：Darwin arm64 按符号性将 8/16 位结果扩展到 32 位，其余寄存器位保持未知。方法声明的返回宽度在分支合并前进入 SSA，避免未使用的高位掩盖有效的低字节结果。

当类导入、布局、字符存储和修正信息完整时，经过验证的 Darwin `__cfstring` 记录可重建为常量对象。保留 ASCII 字节和 UTF-16 码元，包括内嵌 NUL。相同的原始对象地址共享一个重建身份，不同记录保持独立。辅助函数列入 `shared_identity_functions`，链接时需要 Foundation。这不会允许对常量对象记录进行未经验证的原始内存访问。

对于边界明确且不含指针重定位的 `__DATA,__llvm_prf_cnts` 数值计数器节，NeverD 可重建跨方法共享的存储，保留映像中的初始字节、相互重叠的 1–16 字节读写及更新。mobile 导出会合并存储辅助函数；使用 C API 时，跨源码文件链接的每个 `shared_storage_functions` 辅助函数只能有一个定义。重建存储独立于原始映像及其性能分析运行时；地址逃逸、有序内存访问、不完整映射和指针重定位仍不受支持。 带性能计数的 block 回调可以更新这类映像存储，同时仍须保证私有地址不逃逸；普通计数器写入不再被当作私有栈帧写入。

类元数据保留父类身份、实例起始位置/大小，以及偏移、宽度和对齐已校验的标量/指针实例变量；声明在必要处插入填充。依赖不可用实例布局的方法仍标记为未恢复。Category 保留独立的类/分类/地址身份和实现；类与分类清单中重复出现的完全相同记录只计一次。外部 Category 在受支持时使用已有 Foundation 类声明；未知外部类头文件会报告为缺失依赖，不会虚构替代类布局。

恢复状态还要求完整的类声明、本地祖先类定义，以及生成源码中所有使用路径上的先定义后使用和可达出口所需的返回；只有布局已验证、完整方法清单证明没有自身普通方法的本地空父类，才会生成空 `@implementation`。缺少这些证据或名称与已知 Foundation 导入项冲突时，受影响方法仍保留在覆盖率分母中并注明原因，可独立恢复的类继续输出；这些名称检查不覆盖全部 SDK 名称、iOS SDK 或版本。

受支持的 Objective-C Block 调用必须具备完整的固定标量调用 ABI，包括隐藏的 Block 对象及全部参数和返回值载体。运行时编码 `@?` 只在声明中宽化为 `id`，不能提供调用原型。全局 Block 引用保留共享对象身份。受支持的同步标量捕获需要证明原生捕获存储和调用流程。通过已验证的 `objc_retainBlock` / `_Block_copy` 调用复制的强对象捕获可以逃逸，前提是每条可达构造路径都已初始化捕获存储，并完整恢复调用、复制和销毁函数体。生成的描述符保留原始辅助函数 ABI 和所有权布局。弱引用/byref 所有权、未知布局和未经证明的消费者仍保留为未恢复。 对于编译器声明为不逃逸的 C block 参数，还须核对确切导入、参数位置和完整回调 ABI，符合时可建立绑定。该生命周期约束不表示内存只读。 合并链接 C API 源码单元时，每个 `shared_block_functions` 条目只能保留一份定义。mobile 导出会合并匹配的定义，并拒绝包括内部被调用函数差异在内的冲突。

协议方法声明来自已解析的本地运行时记录，覆盖继承协议、必选/可选的实例方法和类方法。普通列表与相对地址列表共用解码器。只有所有匹配的类和协议声明一致，才为 selector 提供固定调用签名；畸形记录和继承环不能提供类型提示。语法完整的结构体、联合体和数组指针使用不透明指针传递，不推断其布局。`objc_metadata.protocols` 单独展示声明，不计入已恢复实现，也不据此推断类遵循的协议。 其余签名完全一致时，有符号和无符号 64 位整数返回值可共用无符号位载体；较窄整数、浮点、指针或参数类型冲突仍会拒绝调用。

带编译器格式声明的 NSString 调用，可从已验证的常量格式对象恢复提升后的标量实参。顺序参数与序号参数必须完整且类型一致。Darwin arm64 从八字节栈槽读取可变实参；x86_64 按整数、浮点寄存器组及溢出栈槽传参。生成调用保留省略号与动态派发。未知格式、计数写入、long double 及未支持扩展仍明确拒绝。 同一套格式分析也支持 `NSLog` 等已声明的 C 导入，要求精确的动态库导出证据，并让实参数量不同的调用共用正确的可变参数原型。

私有栈写入会保留函数内任何位置读取的每个字节。只有证明栈帧边界、入口别名不可变且地址未逃逸后，NeverD 才能删除无人读取的写入，或缩短整数写入中无人读取的尾部。因此，仅未使用的栈填充包含未知位时，提升后的标量参数仍可恢复。分析不会为未知位补值。具有内存顺序的访问、未绑定调用、模糊地址、有副作用的值及分析预算耗尽都会保留原始写入。

分类中的同名方法即使运行时覆盖顺序未知，也会保留已验证的 ABI 声明。只有所有匹配声明一致时，动态调用才可使用该 ABI；存在歧义的实现仍不能被选为源码方法体。类型冲突或格式错误的声明继续阻止调用恢复。

已知的 `objc_enumerationMutation` 调用保留对象参数和后续执行路径，因为已安装的变更处理器可能返回。精确的 Darwin `__stack_chk_guard` 导入绑定运行时对象身份；恢复源码保留栈保护值读取、比较和 `__stack_chk_fail` 调用的可观察行为。

对于导入系统 Foundation 的 64 位 Darwin 链接映像，NeverD 还会查询内置的、由编译器提取的框架声明。同一选择器的运行时声明和框架声明必须全部兼容；可变参数、不支持的聚合类型或平台间不一致的签名仍保持未绑定。声明库只提供调用类型，不确定接收者的类，也不生成函数体。使用 NeverD 不需要本地安装 Apple SDK。 CoreData 使用独立的声明库，仅由确切的系统框架依赖启用。声明归属于其公共头文件所属的框架；间接包含的依赖头文件不会启用其他框架。

ARM64 UIKit Objective-C 声明只有在编译器生成的 iPhoneOS 与 arm64 iPhoneSimulator 证据对所属类型和展开后签名完全一致，且映像导入精确的系统 UIKit 提供库时才会启用。目录覆盖 `CGFloat`、`NSInteger`、`CGSize`、Core Graphics 对象指针等标量、指针和已支持的 record 载体，并保留 UIImage 从右到左布局方向翻转等由所属类型限定的对象结果。若同名选择器在不同 owner 间存在不兼容声明，只有接收者证据、精确的返回载体用途，或外层 Objective-C 方法中保持不变的指针到指针参数能唯一选择兼容声明。参数证明要求共享调用者入口的全部方法记录一致；发布时重新检查入口载体、消息参数位置和当前 selector 声明。完整宽度的接收者或已声明指针到指针值可以经过精确私有栈 spill，但其槽必须完全位于每个已逃逸 frame 地址之下。处在逃逸基址或其上方的 reload、重叠写入、部分或经过变换的值，以及相互冲突的控制流事实均不适用。例如，UIKit 已声明的 `+[UIScreen mainScreen]` 结果经过精确的 ARC retain 返回同一对象后，仍保留 `UIScreen` 接收者身份，使 `-[UIScreen scale]` 能选择该 owner 限定的 `double` 合同。单独观察到的 `scale` 浮点返回用途也能选择这个兼容 ABI。缺少对应编译器证据的架构继续保持不支持。

直接作为消息参数传入的精确私有栈存储地址，也可选择唯一的指针到指针声明，包括 CoreData 的 `save:` 合同。这项结构证据只证明指针到指针形状，不推断所指类型或槽内值。发布时会重新检查 HighIR 实参、相对入口 SP 的精确负偏移、私有栈帧边界和当前声明。传入槽或正偏移、重新加载或经过变换的值，以及多个匹配声明仍保持未绑定。

即使某条消息缺少源码声明，经过认证的 Objective-C 消息目标也会保留完整 Darwin 调用保存寄存器中的接收者身份。所有调用者保存事实和非接收者事实都会被丢弃，私有栈帧存储仍按已逃逸处理。后续消息只有具备自身精确的接收者声明和 ABI 时才能建立绑定。

C 声明库也覆盖 CoreGraphics 和 ImageIO 的导出。不透明的图像及颜色指针、整数计数和浮点返回值保留其声明的 ABI。公共系统框架的路径别名与导出事实一起生成；私有路径、不同的框架版本和未声明的符号不会因此获得绑定。 mobile 声明也使用加载器的类型语法：语法完整的聚合类型指针按不透明指针处理，不推测其布局。固定 `notify.h` API 也来自同一套四目标编译器交集，并且必须精确匹配 `libSystem` 或 `libsystem_notify` 导出。

当声明参数是指针时，即使 ARM64 通过较窄的 `W` 寄存器构造精确整数零，该值仍表示空指针。源码生成只对这一情况输出显式空指针常量；非零窄整数和缺少声明的指针用途继续按载体不匹配拒绝。

命令行工具 SDK 的声明库未覆盖 UIKit 固定 C 函数，因此这类绑定必须同时精确匹配符号和 dyld 提供库。在 ARM64 上，`NSStringFromCGSize` 与 `UIGraphicsBeginImageContext` 复用共享的固定 record ABI：由两个 `double` 组成的 `CGSize` 从两个浮点载体读取。`UIGraphicsGetCurrentContext` 与 `UIGraphicsGetImageFromCurrentImageContext` 返回不透明指针，`UIGraphicsEndImageContext` 返回 void。生成的源码保留每个真实 UIKit 调用。弱导入、非零 addend、冲突存储、其他提供库及尚未支持的架构保持未绑定。

对于已建立 Foundation 桥接绑定的 Swift 大型永久字符串字面量，可将其 UTF-8 字节重建为共享静态存储。长度、标记、终止字节、UTF-8 有效性、存储不可变性及确切导入必须同时得到验证。原有带标记的表示和桥接调用保持完整；含内嵌零字节的字面量及其他存储形式仍保持未绑定。

经验证的常量 NSString 对象在赋值或存储时，即使使用整数载体，只要保留完整数据地址的来源信息，也能保持共享身份。标量立即数、不完整地址、数值运算和对对象私有字节的访问不会因此获得绑定。

对于已证明不可变、且不涉及重定位的映像字节，普通标量加载可恢复为保持原始位模式的常量。支持 1、2、4、8 字节整数和 4、8 字节浮点数。可写或有歧义的存储、有序加载和地址用途仍保持未绑定；同一表达式的数值用途不能为指针用途提供证明。

当共享的源码控制流分析证明每条到达路径上的无符号上界时，索引标量加载可使用重建的不可变字节表。守卫、掩码、原生整数宽度及回绕语义保持不变；写入和局部变量地址逃逸会使先前的证明失效。每张表最多包含 4,096 项、65,536 字节。发布源码时重新检查当前范围、存储、重定位及辅助函数的每处用途。完整指针宽度的位模式若指向映像节，仍存在指针歧义；较窄的标量片段和段填充本身不能证明指针身份。复制的字节仅用于标量读取，表地址不能逃逸。可执行回归将整数位、包含负零的浮点位及越界行为与原始方法逐项比较。

固定参数 C 调用还会使用编译器提取的声明和 SDK 导出、再导出信息。绑定必须匹配实际 dyld 库、符号及标量 ABI；弱导入、未知提供库和不支持的原型保持未绑定。生成的 C 使用独立标识符链接原始符号。没有语言分派表的普通同步调用保留真实调用及内存副作用。

外部数据绑定要求各平台共有的非 TLS SDK 声明及精确的库导出证据。生成的 C 引用真实符号存储并保留后续内存访问，包括全局指针与其所指对象的区别。弱导入、身份冲突及不支持的存储保持未绑定。数据声明不能证明 block 的构造或所有权。 目录涵盖 CoreData、CoreGraphics、ImageIO 和 CoreSpotlight 数据。内建字面量存储通过在每个目标上编译空集合和布尔对象取得；只接受外部非 TLS 数据的直接地址，并执行相同的导出检查。生成目录时，除 libclang 外，还须通过 `--clang` 指定 Clang 编译器。

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

未恢复的 Objective-C 方法行可能包含 `native_backend: {status, reason, diagnostics}`，前提是唯一的后端结果与运行时身份完全匹配。这个有大小限制的可选摘要会保留后端的中间结果，即使声明或布局检查先失败。方法行的主状态、原因和恢复计数仍为最终依据；没有摘要表示该证据不可用。

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

方法因调用绑定失败时，批量记录包含 `unbound_call`，给出第一个被拒绝调用的目标名称、地址、间接调用标记和已恢复参数数量。已有源码签名时，还包含绑定名称和预期参数数量。这是首个阻塞调用的诊断，并非该方法所有剩余问题的清单。

每个方法还包含 `projection_diagnostics`：`items` 记录可独立检查的阻塞原因，包括 `code`、`reason` 以及可用的语句、调用、值或依赖证据。`checks_complete: false` 表示缺少前提或达到资源限制，无法继续检查；不完整报告为空不代表恢复成功。它与准入门槛共用校验逻辑，保持现有 `status`、`reason` 和 `unbound_call` 契约。

批次中的 `native_dependency_graph` 从签名受支持的 Objective-C 方法出发，沿映像代码内的直接调用遍历最终 LowIR。它保留调用者、基本块和指令地址，包括共享被调函数和环；间接目标保持 null。缺少所需 LowIR 函数或达到调用记录上限时，`inventory_complete` 为 false；`targets_complete` 还要求所有已记录调用都有直接目标。范围不包含签名不受支持的方法根和未解析的间接目标，也不证明完整的原生执行覆盖或依赖源码恢复。

已验证的本地协议引用槽通过 `objc_getProtocol` 保持已注册协议的身份。批量结果中的 `runtime_protocols` 依赖清单包含原生被调函数的需求。名称冲突、声明不完整、导入槽和未解析重定位仍不绑定。独立导出会报告缺少协议注册，不会生成可能取得空协议对象的方法体。

Swift 运行时 ABI 目录从固定版本的上游声明生成，并保留 C 或 Swift 调用约定。已知指针和指针宽度的无符号类型组成固定标量签名。Swift 调用必须有精确、非弱的 libswiftCore 导入；支持的两类版本化元数据可用性不允许弱导入。特殊参数寄存器、未知表示、未支持的可用性类别及冲突声明仍被排除。尤其 swift_willThrow 需要编译器另行添加的 swiftself/swifterror 属性，声明 DSL 并未包含这些属性。调用保留副作用及既有回调、字节契约。用 `scripts/generate_swift_runtime_declarations.py --input RuntimeFunctions.def --source-sha256 <pinned-hash>` 重新生成，追加 `--check` 校验目录。声明事实附有版本、内容哈希和第三方说明。

固定 Swift 运行时声明也保留双字返回值：两个指针，或声明规定的元数据指针与状态字。共享 ABI 层分配两个返回寄存器，源码校验重新核对顺序、类型和精确导入。Box 分配与元数据查询仍执行真实运行时调用；聚合参数、未知布局和隐藏上下文仍不支持。

此外，经过编译器精确观测的 URLRequest、Notification、URL、Data、Date 和 IndexPath Foundation 值桥接会保留 `swiftcall` 及提供方身份。间接结果和 context/self 通过专用寄存器上的 `swift_indirect_result` 与 `swift_context` 传递（arm64 为 x8/x20，x86_64 为 RAX/R13），不占用普通整数参数寄存器组。Data 保留明确的双字传输。这些只是调用载体声明，不代表恢复了值布局或通用 Swift ABI。

Native 标量返回值推断可识别已验证调用结果 ABI 后紧接的完整双字提取。临时值身份、成员偏移、宽度及物理寄存器必须全部匹配。辅助函数因此可以直接返回第一个成员，同时保留普通寄存器视图规则、调用覆盖规则，以及每条返回路径都具有已定义结果的要求。

固定 C 运行时目录还保留 32 位整数载体及明确零扩展的布尔结果。布尔结果使用完整的无符号字节；这不提供布尔参数的扩展约定。更窄的未知表示及采用 Swift 调用约定的 32 位声明仍不纳入。运行时测试覆盖成功与失败的类型转换、准确的计数式保留及对象析构；调用和所有权效果始终保留。

存储字段元数据区分已知字节布局与未知语言类型。稳定 ABI 的 Swift 类即使在 arm64 上也使用指针宽度的字段偏移变量；未公开的字段可能具有空的 Objective-C 类型编码。NeverD 保留类型缺失这一事实，同时验证偏移、大小、对齐和重叠。恢复的 C 函数体通过运行时 ivar 查询获取偏移。字段类型不可用时，仍不会生成需要虚构类型的类声明。

稳定 ABI 的 Swift 成员在偏移槽属于运行时初始化的零填充存储时，仍可保留其元数据身份。此类类标记为 `ivar_status: "runtime"`；未知偏移在 JSON 中为 `null`，字段大小为零表示宽度由运行时决定。方法体可从已有运行时类查询偏移，声明的对象类型也可沿精确偏移槽追踪；字面量偏移仍要求已知布局。这不会重建 Swift 类布局：独立类导出继续拒绝未知布局及字段类型。

对于已直接绑定的原生调用，只有证明被调函数在入口、可观察副作用之前读取该指针恰好一次，且不写入、保存、比较或另作他用时，才能将 ivar 偏移变量的地址替换为局部标量的地址。局部值由现有运行时 ivar 查询取得，原生 ABI 与被调函数体保持原样。该有界证明仅接受平直控制流，对用途不确定或分析超限的情况拒绝恢复。C API 支持恢复 `.cxx_destruct`，独立 Objective-C 方法语法仍无法生成该 selector。

只有导入调用的已验证契约限定了非负读取长度，并排除写入、保存指针和比较地址身份时，才重建不可变字节缓冲区。保留原始字节与长度，其他指针用途仍保持未解析。精确识别的 Swift 标准库断言失败会保留编译器推导出的标量与栈载体、`swiftcall`、`noreturn` 效果及准确链接符号；C 诊断垫片仍使用声明的 C ABI，并保留独立的后续陷阱。只有面对这个已经认证、不会保存指针的终止消费者，才复制静态字符串和不朽 String 字面量存储；动态或带所有权的 String 值不会被当作字面量。

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

严格 Swift 脚本检查 22 个用户声明、3 个 getter/setter 入口和 9 个编译器生成的可调用入口，任何一项都不能从清单中消失。每个变体有 858 个原程序独立预期结果检查。脚本独立编译生成的 `.swift` 与调用程序，不使用原始 dylib、模块、桥接或手写替代声明。覆盖标量/原生调用、类初始化与存储、结构体按值/mutating 方法、浮点和栈参数、指针及循环。原生 C++20 CLI 的验收要求是 arm64/x86_64 × classic/default 四个变体零跳过：每组必须恢复 25 个原生方法体和 9 个编译器投影，保留全部 34 个可调用身份，原程序与独立编译的生成 Swift 均须通过 858/858 项独立预期结果检查。这些结果限于该样本，不保证任意应用或原始源码文本的恢复。脚本会拒绝覆盖缺失、源码编译失败及行为差异。

这四个变体的编译目标是 macOS。新增的两个编译器入口分别为空值初始化器及其元数据访问器，二者经过独立的原生证明，并在同一个 `struct Empty {}` 源码单元中保留各自身份。通过该样本不代表真实 iOS 应用已通过验收。

三个脚本都支持 `--arch all|arm64|x86_64`、`--fixups both|classic|default`、`--timeout N` 和 `--work-dir NEW_DIRECTORY`。`--setup-only` 只验证原样本，不测试恢复。包括标量脚本在内，验收要求完成所有请求的架构和 fixup 变体；缺少变体或宿主无法执行某个要求的架构均视为失败，不允许跳过。保留的失败产物可用于区分源码覆盖缺失、编译错误和行为差异；声称已验证前应查看当前测试结果。

[Mobile Real Applications 工作流](../../.github/workflows/mobile-real-apps.yml) 使用[样本清单](../../scripts/mobile_real_apps.json)中固定版本的公开应用。验收要求独立列出完整 iOS bundle 和 APK 中的全部 Mach-O/DEX，独立重建原版与生成源码，并对照行为。阶段缺失、清单覆盖未知或必需 case 缺失都会使验收失败。真实应用的 recompile 和 behavior 阶段目前仍未完成，因此保留 Experimental 标记；测试框架的守卫测试通过不代表真实应用验收通过。

结果发布具有事务性：选择新目录，先检查进程退出状态，并把重定向的 JSON 放在该目录外。失败会删除暂存输出并保留已有结果。后端非零退出附带长度受限的日志尾部。后端超时保留原有超时消息；若已捕获的日志文本可用，会追加长度受限的尾部；预算失败保留独立诊断。原生 CLI 成功返回零，恢复失败返回非零。使用 `--json` 时，已处理的失败包含 `schema_version`、`status: "error"` 和 `error`。参数解析、原生程序或依赖库启动失败以及中断仍可能只报告 stderr。调用方应先检查退出状态。

加密切片需要可读输入；缺少架构时检查可用切片；方法被省略时查看其准确原因和元数据诊断。增加 `--max-func` 只对因数量上限被排除的函数有帮助。缺少布局、签名、外部头文件、异常支持或 ABI 行为支持，需要补充实现或有效元数据，不能直接宣称完整恢复。分发工具或生成的软件包时保留适用的依赖许可证声明。

原生标量辅助函数推断也支持 float/double 寄存器参数与返回值。共享 MedIR 入口字节分析必须证明宽向量输入只观察一个低位标量通道。只有所有定义的低位宽度相同、丢弃的高位表达式没有副作用，且每次使用都明确读取该低位前缀时，才能缩窄源码局部 CONCAT 值。调用、存储、分支位置和非 NaN 浮点值的精确位型均保留；不会为未知高位补造数值。

当叶子原生辅助函数转发到已验证的外部 void 尾调用，且没有完整标量返回值时，可以使用内部 void 源码签名。LowIR 必须逐一匹配已绑定调用及其合成返回。叶子证明禁止写入保留寄存器、帧寄存器、栈指针和链接寄存器；有界的逐字节污点不动点还会拒绝存储栈派生值或将其传给调用。控制流、函数体和依赖检查仍然适用。该签名不提供任何结果位：读取未知结果的调用者仍保持未恢复。运行回归检查条件对象销毁、调用者独立返回值，以及对读取未知结果调用者的拒绝。

对于保存寄存器、使用栈帧并执行普通调用的原生辅助函数，有界 LowIR 分析必须证明每条退出路径都恢复原有的保留寄存器字节、栈指针和链接寄存器，才能生成 void 源码摘要。部分写入、隐式零扩展、重叠存储和调用破坏会使相应事实失效；将帧地址保存到栈槽或向外传递会使证明失败。唯一的栈帧借用例外是已有精确源码绑定的 `objc_msgSendSuper2` 首参数：它可以指向完全位于已分配栈帧内的完整 16 字节 `objc_super` 对象，因为该运行时合同只在调用期间同步读取它。普通消息、不完整指针、越过栈帧边界的对象及其他任何栈帧参数仍会被拒绝。重新提升后，可以移除在 HighIR 中完全没有出现的辅助寄存器参数，并再次运行流水线。私有存储消除仍由现有 HighIR 清理负责，标准参数和实际输入使用保持不变。
