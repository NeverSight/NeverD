**语言**: [English](../darwin-emulation.md) | [简体中文](darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 1939e117643388dff149085b992e1ad646feefca63b630abec9b09122e15c2f1 -->

[← 文档索引](README.md)

# macOS / iOS 来宾进程环境

`os/darwin/` 提供共享的 Mach-O 启动、Darwin 系统调用和内存模型；
`macos/`、`ios/` 分别定义平台契约。它们属于来宾 OS 层，HVF 属于宿主 CPU
后端，二者独立选择。
启用 `NEVERD_ENABLE_CPU_EMULATION` 即可，不需要开启 Windows 驱动模拟。

| Profile | 平台 | 架构 |
| --- | --- | --- |
| `macos-macho64-v1` | macOS | x64、ARM64 |
| `ios-macho64-v1` | iOS 设备 | ARM64 |
| `ios-simulator-macho64-v1` | iOS Simulator | x64、ARM64 |

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

本轮范围对齐现有 Linux/Windows 的有限进程模型：原始 Mach-O 字节、数据和 BSS、
段权限、`LC_MAIN` 的参数及返回、规范 `LC_UNIXTHREAD` 的启动栈、显式 argv/envp/apple
向量、退出、标准输出与错误输出、固定身份、匿名内存映射、保护和释放。
ARM64 使用 16 KiB OS 页，x64 使用 4 KiB；CPU 页表仍以 4 KiB 为基础。
`__PAGEZERO` 只保留地址，不消耗数 GiB 内存。
完整输入文件（包括未映射的元数据和尾部字节）在解析和复制前必须符合 `memory_limit`。
加载器只对普通文件做有界读取，拒绝含 NUL 的路径、短读和文件大小变化，解析独立快照，
不保留宿主文件映射。输入文件和来宾映射各有一个同值预算；宿主文件 I/O 没有硬实时保证。
Mach-O 文件尾页保留同页原始字节，后续完整虚拟页清零。初始数据、栈和匿名页都按
OS 页独立持有物理内存，因此部分解除映射能释放预算，重新分配的页面保持清零。

BSD 系统调用遵循 Darwin ABI。ARM64 从 X16 读取服务号，x64 使用 BSD 类别前缀；
错误返回正 errno 并置 carry。JSON 中的 `error` 字段区分错误和值相同的成功结果。
不会复用 Linux 的负 errno。内存保护跨空洞或最大权限边界失败时，整段原权限保持不变。
部分 `write` 复制的字节会保留，后续访存错误仍返回 EFAULT。
原始 `write` 长度超过 `INT_MAX` 时，直接返回 EINVAL，检查发生在描述符、来宾指针和
输出预算之前。此顺序已有真实 macOS 系统调用对照。

服务清单为 `exit`、`write`、`getpid`、`getppid`、`getuid`、`geteuid`、`getgid`、
`getegid`、`getgroups`、`mmap`、`mprotect`、`munmap`。PID 为 1000，PPID 为 1；UID/GID 默认 1000，可由下述凭据显式提供不同的真实/有效 ID。
匿名数据映射要求 `flags=0x1002`、描述符 -1 和零偏移；长度和非固定提示地址向上按 OS 页取整。
旧式原始 mmap 的零长度请求返回零而不分配内存；`MAP_UNIX03` 已支持，零长度返回 EINVAL。
Unmap/protect 地址必须对齐；允许 NONE/READ/WRITE，WRITE 隐含 READ，不接受匿名可执行映射。

设备与模拟器的 Mach-O 平台标记必须分别匹配。此版本接受不依赖动态库、重定位、
初始化函数或 TLS 的独立可执行文件。需要 dyld 链接、Mach IPC、线程、宿主文件系统、
Objective-C/Swift 运行时、Foundation/UIKit，或 arm64e/PAC 的输入会明确拒绝；
不会用空实现假装执行成功。它不等同于完整 macOS/iOS 系统，也不是 Apple 的 Simulator。

测试使用自行编写、由 Clang/LLD 生成的五种 Mach-O 平台/架构用例，另有独立构造的
线程入口和畸形文件测试、4 KiB/16 KiB 内存测试，以及 C API/CLI 报告一致性测试。
Python SDK 也通过真实共享库执行五种平台/架构组合。

## 显式文件输入与描述符

`darwin_files` 为三个 Darwin profile 提供封闭的初始只读普通文件目录。必填 `files` 的条目包含规范绝对来宾 `path` 和十六进制 `bytes_hex`；可选 `stdin_hex` 提供有限输入流。省略标准输入表示未知，非零读取会明确停止；空字符串表示 EOF。未配置目录时 `open` 停止，显式空目录中的缺失绝对路径返回 ENOENT。不会读取宿主路径或继承宿主输入。

新增 `open`、`read`、`pread`、`lseek`、`close`、`dup`、`dup2`、`fcntl`。read/write/open/close/fcntl/pread 的 nocancel 入口复用同一实现。支持 O_RDONLY/O_CLOEXEC，以及 F_DUPFD、F_DUPFD_CLOEXEC、F_GETFD、F_SETFD、F_GETFL。独立打开有独立游标；复制描述符共享游标，但 close-on-exec 标志独立。`pread` 不移动游标；关闭或替换 0/1/2 会影响后续 I/O，复制输出描述符保留原捕获通道与共享输出预算。

最多 256 个文件；路径及 NUL、文件和输入字节合计最多 16 MiB。路径短于 1024 字节，每个分量最多 255 字节；`descriptor_limit` 为排他上界，取值 3–4096，默认 256。JSON 保留 64 KiB 上限。非法配置及文件/祖先目录冲突在加载镜像前拒绝。

超出 INT_MAX 的读取先返回 EINVAL，再检查描述符；EOF 不访问目标地址，无效目标返回 EFAULT。目标缓冲区只有部分可写时，在写入和移动游标前明确停止。定位支持 SET/CUR/END，负位置和溢出失败保留原游标。旧版 stat 元数据和其他 fcntl 操作仍未实现。文件作为路径祖先返回 ENOTDIR。文件与 nocancel 程序及输出重定向使用同一份自编目标文件对照原生 macOS；C/CLI/Python 覆盖全部五种来宾组合。这不构成 iOS 真机验证。

2026-10-05 的 Release Darwin 验收共 381 项：177 通过、204 跳过、零失败，ARM64 HVF 必需项 51/51 实际执行。原生 macOS 7 个程序、公共 C/CLI 与报告 35 项、Python 五种来宾组合和验收脚本 66 项通过，各计数有重叠。新增文件服务尚无 Intel HVF/KVM/WHP 原生证据；Intel HVF 仍未验证且暂停 Actions。当前宿主没有 iOS SDK，也没有 iOS 真机对照。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## 已有文件的可写内容

每个文件可用严格布尔值 `"writable":true` 显式允许进程内修改；C++ 使用 `DarwinFileOptions::WritableFiles`。省略或 false 保持只读，未知写授权明确停止，不访问宿主或修改输入选项。write(4/397)、pwrite(154/415)、truncate(200)、ftruncate(201) 和 O_TRUNC 共用文件节点；独立 open 共享字节但游标独立，dup 共享游标与状态，最后 close 后重开仍保留内容。增长补零，截断不移动游标，O_RDONLY|O_TRUNC 也截断。

F_SETFL 按原生标志转换只改变 O_APPEND|O_NONBLOCK，保留访问模式、close-on-exec 和 FWASWRITTEN；F_GETFL 在实际传输非零字节后暴露 0x10000，包括 pwrite 和输出捕获。pwrite 忽略 append、不移动游标。超 INT_MAX 的长度在 FD 检查前返回 EINVAL，pwrite 偏移 -1 更早返回 EINVAL；INT64_MAX 偏移在零写前返回 EFBIG，长度先裁剪再选择追加位置。

成功的 ftruncate（包括大小不变）也为调用描述及其 dup 设置 FWASWRITTEN；O_TRUNC 为新描述设置，包括 O_RDONLY。按路径 truncate 不改变已有描述的标志。

部分可读输入在任何效果前停止；整段 EFAULT 保留字节，非空追加仍把游标移至 EOF。传输后端失败不提交内容或游标。若未配置 `mutation_policy`，非零成功写、截断和非零整段 EFAULT 都使完整 stat 观察失效，因为失败追加也可能改变时间戳；后续 stat 在输出前明确停止。零写保留元数据。16 MiB 是路径/NUL、输入、目录记录、CWD 与当前文件内容的合计逻辑预算，可写路径引用也计入；缩小替换 backing 并回收容量。调用方原始内容和一个有界替换缓冲区属于额外存储。已知 inode 别名及 immutable/append-only 标志暂拒绝。

DarwinMemory 持有映射租约；所有映射区间解除前，write、truncate 和 O_TRUNC 均停止，包括 PROT_NONE 和 FD 已关闭的映射。失败映射和旧式零长度映射不留租约；新映射读取当前内容。只写 FD 直接请求 READ/WRITE mmap 返回 EACCES，PROT_NONE 可成功并经 mprotect 获得读写权限。

原生 writable-files 与 nocancel 程序比较字节、游标、标志和错误顺序；单元测试覆盖 4K/16K，C/CLI/Python 覆盖五种组合。权限强制检查、目录删除、不同初始目录域之间的重命名、硬链接、真实文件系统的元数据更新、映射一致性和 EOF SIGBUS 仍待实现。完整 macOS/iOS 目标尚未完成，iOS 真机与 Intel HVF 仍无验收，Intel Actions 保持暂停。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## 显式可变元数据

文件可在 `writable: true` 与完整 `metadata` 旁配置 `mutation_policy`；C++ 使用 `DarwinFileOptions::MutationPolicies`。这是明确选择的虚拟稀疏分配策略，不推断 APFS 行为，也不读取宿主时钟；省略时仍保留修改后元数据未知的契约。

`allocation_unit` 与 `mutation_time` 及其中 seconds/nanoseconds 均必填，整数沿用无损规则。分配单位必须为 512 字节至 16 MiB 的二次幂，独立于 block_size 和 VM 页。要求普通文件权限（无 set-id/sticky）、flags=0、link_count=1，以及密集初始分配：blocks=ceil(size/allocation_unit)*(allocation_unit/512)。密集是调用方的明确断言，零字节不表示洞。策略路径引用计入 16 MiB 逻辑预算；blocks 是独立的虚拟分配账本，不据此虚构 ENOSPC。

写入分配所有触及的单位，向洞写零也分配。truncate 增长只补零，不分配；缩小时丢弃向上取整 EOF 之后的单位，保留已分配的末尾部分单位。重新增长不会恢复已丢弃的分配。成功的非零写以及每次成功截断（含同大小及空文件 O_TRUNC）更新 size/blocks，并把 mtime/ctime 设为固定输入时间；其他字段不变，读取不推进 atime。路径查询、独立 open、dup 与重开共享节点状态，初始输入不变。

零写、预算/映射拒绝、部分输入拒绝及后端失败保留已知状态。非零整段 EFAULT 仍使元数据失效，后续成功写或截断不能恢复它；stat 输出失败不改变节点。virtual-file-metadata 来宾程序通过五种组合及 C/CLI/Python 检查完整 144 字节记录；分配结果是策略测试，不是原生 APFS 等价证据。原生可写程序另行验证标志、游标和错误顺序。命名空间修改、真实文件系统一致性、Mach 服务与动态运行时加载仍待完成。

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```


## 稀疏文件定位

普通文件配置 mutation_policy 且分配状态仍已知时，lseek 支持 SEEK_HOLE=3、SEEK_DATA=4，读取与 stat 相同的单位账本。首次修改前初始文件明确为密集分配，零字节也不表示洞。输入位于所求类型单位内时返回原偏移，否则返回下一匹配单位起点；末尾洞从 EOF 开始。负偏移返回 EINVAL，到达/越过 EOF（含空文件）或找不到后续数据返回 ENXIO=6。错误保留游标，成功只改变当前 open 描述及其 dup；独立 open 保留自己的游标，重开读取当前分配。元数据、标志和字节不变，whence 高位忽略。

未配置策略、目录以及整段 EFAULT 后永久未知的分配仍明确拒绝。不能由零值或被拒绝的写入/增长推断新分配。自编 sparse-file-seek 原生/来宾程序验证错误、已写字节、EOF 和描述符生命周期，不假设更早的文件系统区段位置；virtual-file-metadata 单独验证精确虚拟几何，C/CLI/Python 覆盖全部五种组合。

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 删除普通文件名称

目录的 `mutable:true`（C++ `MutableDirectories`）显式授权修改直接子项名称，与文件内容的 `writable` 独立；下例允许删除只读文件。缺少授权会明确停止，不根据权限位虚构凭据或 EACCES。准入拒绝父目录/直接普通子文件的非零已知 flags、父目录特殊权限位、子文件 link_count 不为 1，以及父目录或子文件的已知身份别名。身份检查合并 stat 和目录快照的 inode；明确不同的设备保持独立。授权路径计入现有预算。

`unlink(10)` / `unlinkat(472)` 删除现有普通名称；普通文件删除仅接受低 32 位 flags=0 或 `AT_SYMLINK_NOFOLLOW_ANY=0x800`。未知位先返回 EINVAL；AT_REMOVEDIR 使用下述受限目录删除；DATALESS、SYSTEM_DISCARDED 仍不支持。共用解析器保留路径故障、目录 FD、CWD 和绝对路径的优先级；缺失为 ENOENT，文件后斜杠为 ENOTDIR，普通目录为 EPERM，纯斜杠根路径为 EISDIR，末尾带 `.` / `..` 的根路径为 EBUSY。原生探针也检查末尾 `.` / `..`。

名称删除后，旧 FD、dup、独立打开对象仍保留数据、游标和状态标志，新打开返回 ENOENT；隐式父目录与 CWD 继续存在。F_GETPATH 保留已捕获旧路径，与原生对照一致。写授权属于文件对象。只有所有描述符和最后一段映射均释放后，close/dup2 或下一次修改才回收当前文件字节预算；初始路径/引用费用仍保留。权限强制检查、不同初始目录域之间的重命名与硬链接仍待实现；初始目录删除采用下文的显式授权。

成功删除使直接父目录的 stat/列举观察失效，涵盖旧/新 FD、dup 和路径查询；stat/readdir/SEEK_END 在复制和移动游标前停止。read/pread 仍为 EISDIR，SET/CUR、F_GETPATH、fchdir、相对查找继续可用。有仍可信的文件修改策略时，仅将 nlink 改为 0、ctime 改为固定时间，保留 mtime/atime、数据和分配；后续写入不能恢复 nlink=1。缺少策略或曾发生整段 EFAULT 时，完整文件元数据仍未知。失败不改变状态。原生 `unlinked-file` 比较名称/描述符行为；策略时间与目录失效是明确的模型规则。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## 创建普通文件

`O_CREAT=0x200` 只在显式 mutable 的直接父目录中创建空普通文件，普通/nocancel open、openat 共用实现。新对象获得内容写授权；已有对象仍使用独立的 WritableFiles 授权。只读 FD 可以创建但不能写入。未提供创建策略时，新对象的 stat64 和稀疏定位仍明确未知；新对象始终不继承同名旧对象的 metadata/mutation_policy。

O_CREAT 配合 `O_EXCL=0x800` 对已有文件或目录先返回 EEXIST，不截断、不检查写授权或映射；O_EXCL 单独无效。已有目录可用只读 O_CREAT 打开。经过下述 openat 首字节及目录预检后，顺序为无效访问模式→FD 容量→O_CREAT|O_DIRECTORY 的 EINVAL→路径访问。只能创建原始路径的最后缺失分量；缺失祖先及末尾 `/`、`//`、`/.`、`/..` 仍为 ENOENT。新文件的 O_CREAT|O_TRUNC 不设置 FWASWRITTEN，截断已有文件则设置。

只有实际插入才使父目录 stat/枚举失效。同名重建与仍打开或映射的旧对象拥有独立字节、元数据、描述和映射租约。256 项上限计入固定初始非文件项、具名对象及仍存活的孤立对象；新对象的规范路径/NUL 和当前字节计入 16 MiB，删除且最后 FD/映射释放后才回收动态费用。关闭具名对象不释放名额，初始输入/引用费用仍保留。预算不足或规范路径达到 1024 字节会明确停止，不虚构 ENOSPC 或原生路径错误；失败不留下名称或 FD。

原生 created-file 与五种来宾组合对照，4K/16K 测试覆盖精确容量、失败原子性和旧映射存活时重建。权限强制检查、不同初始目录域之间的重命名、链接及目录修改仍待完善。

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## 显式创建元数据与进程 umask

可选 `darwin_files.umask`（C++ `InitialUmask`）单独声明初始进程掩码，范围为八进制 0..07777，不要求创建授权。`umask(60)` 返回旧掩码、保存输入的低 07777 位，不访问来宾内存，也不需要空闲 FD。省略表示未知，不读取宿主或猜测默认值。掩码只初始化一次，只影响后续创建，不改变调用方输入。下例十进制 18 即八进制 0022。

未配置 `namespace_policy` 时，可选 `darwin_files.creation_policy`（C++ `CreationPolicy`）为新对象提供完整元数据。严格对象恰含 `first_inode`、`block_size`、`generation`、`creation_time`、`mutation_policy`，时间及修改策略复用既有格式。必须显式提供 umask，至少授权一个 mutable 父目录，且每个授权父目录都有完整 metadata。block_size 为 1..INT32_MAX，generation 为 uint32；分配单元是 512..16 MiB 的二次幂，独立于块大小和 VM 页，纳秒须在 [0,1000000000)。first_inode 是非零 uint64，严格大于所有 stat/快照中的 inode，包括其他设备；超出 JSON 精确整数范围时使用十进制字符串。

只有成功插入新名称才消耗全局递增 inode；成功使用 UINT64_MAX 后永久耗尽，关闭、删除、同名重建、umask 或后续查找都不能重置。排他、FD、路径、条目或字节预算失败不留下名称、FD 或计数器增量，打开已有 O_CREAT 也不消耗编号。新 stat64 的 device/GID 继承直接父目录，UID 为配置选择的来宾有效用户 ID（默认 1000），mode 为 `S_IFREG | (mode & 0777 & ~umask)`，nlink=1，size/blocks/flags=0；块大小、generation 和四个初始固定时间来自策略。父目录完整 stat/列举失效后，仍可使用不变的 device/GID，但不会恢复完整记录。

新节点独立持有元数据与分配状态，不继承同名旧对象记录。后续写入、截断和删除共用修改策略，保留 inode/mode/birthtime 和已删除对象的 nlink=0；整段 EFAULT 后的未知状态仍不可恢复。策略不追溯修改已有节点。原生 `created-file-metadata` 对照权限、掩码返回值、有效 UID、父设备/组和身份存活，覆盖五种来宾组合；`virtual-created-metadata` 单独比较完整 144 字节记录。原生四个创建时间不一定相等；固定时间及稀疏分配是明确的虚拟文件系统规则。权限强制检查、凭据切换、ACL 和原生 APFS 元数据行为仍待实现。

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 普通文件与创建目录的原子交换

RENAME_SWAP=0x2 通过 renameatx_np 交换两个已有普通文件、两个仍链接的进程创建目录，或一份文件与一个创建目录；可加 RENAME_NOFOLLOW_ANY。显式初始目录必须声明 mutable:true 和 swap_rename:true，C++ 使用 DarwinFileOptions::SwapRenameDirectories。创建后代继承原始目录对象的能力，删除和重用名字不转移旧声明。false 或省略表示未知；相同设备和修改授权不能证明交换能力，不同初始目录域仍不支持。

`openat_nocancel`、`fstatat64` 与 `F_GETPATH=50` 使用同一个文件组件；交换后的路径和观察仍属于各自对象，完整 stat 的可用性仍由元数据契约决定。

目标不存在（含末尾斜杠）时先返回 ENOENT，先于源点/双点、域、授权和能力检查。初始或已删除目录操作数仍明确不支持。在获准域内，任一方向的父子目录交换及目录与其子文件交换返回 EINVAL；普通文件目标末尾斜杠为 ENOTDIR。普通组件的同对象交换经命名空间授权后为空操作，即使没有交换声明。源点/双点与目标同对象时，文件系统大小写属性仍未知。RENAME_EXCL=0x4 + RENAME_SWAP=0x2 和未知 flags 在路径读取前返回 EINVAL；SECLUDE 不支持。

两棵非空子树按父对象链移动，包含被 FD 或映射保留的已删除子目录和文件孤儿。混合交换只移动确切的普通文件根；旧的同名孤儿仍跟随自己的父对象。FD、dup、CWD 和双点跟随对象及新父目录。后代文件的字节、身份、元数据、写授权、游标、标志和租约保留；移动的根及两边直接父目录应用既有命名空间元数据规则。配置策略更新根文件自身 ctime；无策略或已失效时完整元数据保持未知。

能力引用的路径+NUL 保留在固定初始 16 MiB 预算。两边所有仍链接及保留路径都在发布前检查 1024 字节路径界限与共享总额，再共同撤下旧名字并发布新名字。交换不删除根、不使用覆盖回收额度，未打开目标的字节也不能抵扣。初始文件首次移动取得动态路径费用，交换回不清零，往返不累加；无需新 FD、条目或创建 inode。失败时两边名字、父对象、游标、观察与映射保持不变。原始无 SDK 的 swapped-directory 程序通过原生 macOS 与五种 C++/C/CLI/Python 配置对照目录交换及两种混合顺序。

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## 显式目录快照

`getdirentries64` (344) 枚举已有 `directories` 条目的可选只读 `contents`；C++ 使用 `DarwinFileOptions::DirectoryContents`。`entries` 必须按明确顺序完整列出 `.`、`..` 和所有直接子项。即使空目录，缺少快照仍表示未知；快照不创建路径或 stat 元数据，也不查询宿主文件。

每项必填 `name`、非零 `inode`、`type`（0 未知、4 目录、8 普通文件）、`next_offset`、`seek_offset`。类型必须匹配路径；相同解析路径的 inode 必须与其他快照及元数据一致。`next_offset` 是目录内唯一、非零、不超过 INT64_MAX 的游标标记，无需递增；零表示回绕到开头。`seek_offset` 是独立的无符号 64 位 d_seekoff 观察值，可重复为零。整数沿用 stat 元数据的无损十进制字符串规则。

`contents.minimum_buffer_size` 必填，表示包括 EOF 在内的有效载荷下限，范围为 1–128 MiB。条目可另设 `minimum_buffer_size`（默认 0），约束从该项开始的读取。示例记录 APFS 开头两个点条目至少需要 64 字节、EOF 只需 1 字节的观察；其他位置至少容纳一个完整记录。LP64 记录按 8 字节对齐，长度为 `roundUp(25 + nameBytes, 8)`。全部快照最多 4096 项，编码字节计入 16 MiB 输入预算；仅元数据/快照声明的祖先路径去重后计入 256 路径上限，JSON 仍限 64 KiB。

独立 open 使用独立游标，dup 共享游标；仅零和声明的标记可恢复遍历，未知位置明确停止。每次返回能容纳的最大完整记录前缀。长度 >=1024 时，原请求末尾四字节为 EOF 标志（到末尾为 1，否则 0），仅记录载荷截断到 128 MiB；标志地址保留原无符号长度及回绕运算。顺序为写数据、移动游标、写读取前位置、写标志。后续 EFAULT 保留此前效果；EOF 不访问空数据缓冲区。单次复制仅部分可写时，在该次复制前明确停止，保留此前复制和游标效果。

同一 `directory-entries` 程序对照原生 macOS 的记录字段、dup/回绕、小块读取、EOF 与复制顺序；独立测试按 SDK 布局逐字节核对捕获的原生记录和长文件名。快照标记在回绕后保持固定，不模拟 APFS 每次回绕时变化的游标值。旧 `getdirentries` (196)、修改后的目录枚举、其他原生后端和 iOS 真机尚未纳入验收。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

目录枚举验收（2026-10-05，Release）：Darwin 共 498 项，246 通过、252 项因后端不可用跳过、零失败；ARM64 HVF 必需项 63/63 实际执行。11 个原生 macOS 程序、40 项公共 C/CLI/报告（无跳过）、Python 五种来宾组合各八种文件场景和 66 项验收脚本通过，计数有重叠。证据：`build-hvf-arm64/darwin-dirents-merged-evidence/`。Intel HVF Actions 保持暂停；其他原生后端和 iOS 真机未验收。

## 私有文件映射

`mmap` 支持普通目录文件的 `MAP_PRIVATE` 映射：`flags=0x2`，或添加 `MAP_UNIX03` 后使用 `0x40002`，文件偏移必须按 OS 页对齐。映射保留整页内的原文件字节，即使请求长度较短；EOF 尾页剩余字节清零。私有写入仅修改当前映射，不改变原文件、其他映射、固定元数据或共享描述符游标。关闭或复用描述符不影响已有映射。支持只读和 PROT_NONE 的初始内容，以及后续 `mprotect` 加写权限。

文件末尾算术溢出、UNIX03 零长度或未对齐偏移在 FD 查询前返回 EINVAL；坏 FD 在预算检查前返回 EBADF。旧式零长度仍检查 FD，再返回零且不分配。旧式未对齐偏移、流描述符、空文件页及完整越过 EOF 的页，在分配前明确停止。真实 macOS 允许映射完整 EOF 外页面，但访问触发 SIGBUS；当前模型不伪造可读零页或信号投递。共享、固定、可执行及 JIT 映射仍未支持。

`DarwinFiles` 统一解析描述符和字节，`DarwinMemory` 负责分配、权限、预算及回滚。文件仅来自 `darwin_files`。同一 `file-mapping` 程序检查私有写入、close 后寿命、游标、错误顺序与匿名页重用；独立原生对照检查非零文件偏移、整页内容及真实 SIGBUS 边界。

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### 私有映射验证（2026-10-05）

Release Darwin 门禁核对 438 个唯一登记项：210 通过、228 跳过、零失败，57/57 个 ARM64 HVF 必需项实际执行；Unicorn 覆盖五种来宾组合。9 个原生 macOS 程序通过，独立原生测试逐字节核对非零偏移的 EOF 尾页，并在隔离子进程确认下一整页触发 SIGBUS。36 项公共接口/报告测试无跳过，Python 覆盖五种组合及 `file-mapping`；66 项验收脚本与 38 项来源检查回归通过。计数有重叠。证据位于 `build-hvf-arm64/darwin-mmap-verified-evidence/`。本轮仍无 Intel HVF/KVM/WHP 或 iOS 真机验收，Intel HVF Actions 保持暂停。

## 显式文件元数据

文件条目可添加 `metadata`；提供时，下例全部字段均必填。十进制字符串保留完整整数精度，JSON 数字限于 ±(2^53−1) 内的精确整数。device 为有符号 32 位，mode/link_count 为无符号 16 位，inode 为无符号 64 位，uid/gid/flags/generation 为无符号 32 位；size 必须等于文件字节数，blocks 不超过有符号 64 位上限，block_size 为非负有符号 32 位。四个时间使用有符号 64 位秒和 0–999999999 纳秒，mode 必须匹配文件或目录类型。

`stat64` (338)、`fstat64` (339)、`lstat64` (340) 在 ARM64/x64 返回同一 144 字节 LP64 记录。路径解析与 open 共用；FD 查询遵循复制和关闭，既不分配描述符，也不改变游标。普通文件 rdev、填充和保留字段清零。输入提供初始元数据，可选修改策略决定后续变化；读取不推进时间，mode 不改变目录访问授权。缺少元数据、流状态、旧版 stat和扩展安全查询明确不支持。路径/FD 错误先于目标地址检查，部分可写目标在任何写入前停止。原生测试逐字节对比真实文件状态并核对 SDK 布局，同一自编原始程序验证三种系统调用。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### 元数据验证与剩余工作（2026-10-05）

加入 stat64 后的 Release 验收共 409 个唯一登记项：193 通过、216 跳过、零失败；54/54 项 ARM64 HVF 必需测试实际执行，Unicorn 覆盖五种来宾组合。SDK 布局与真实文件整条记录对比、8 个原生 macOS 程序、36 个公共接口/报告测试（无跳过）、Python 五种组合和 66 个验收脚本测试均通过，计数有重叠。原生测试改为每例独立输出文件，修复短输出残留旧尾字节的问题。新增能力仍无 Intel HVF/KVM/WHP 或 iOS 真机证据。

后续优先顺序：先补共享映射与 EOF 缺页、有界写入，验证 EOF 页、close 后映射寿命与错误顺序；再补显式时间/系统信息、必要 Mach/线程服务和 Mach-O 依赖、重定位/绑定、初始化/TLS；最后通过真实程序推进 Objective-C/Swift 与 Foundation/UIKit。iOS 真机对照需要 SDK 和设备环境；Intel HVF 尚未验证，Actions 继续暂停。



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

HVF 必需门禁也包含这些用例。完整支持边界、来源和命令见[英文说明](../darwin-emulation.md)。
Intel HVF 的 10 项原生 transport 和全部 26 个 Darwin 工作负载均已通过，完整 CPU 清单仍单独验收；当前状态见
[HVF 验证记录](macos-hvf.md)，不能把内核参考程序成功当作后端通过。

新增的完整原生工作负载门禁要求本机架构的每个 Darwin 进程用例都实际通过：
ARM64 三个平台共 111 项，x64 的 macOS 和 Simulator 共 74 项。
每种平台都必须执行 `LC_MAIN` 和独立编写的 `LC_UNIXTHREAD` 程序；源码清单回归确保
以后新增的 Darwin 进程用例也进入必需集合。
macOS 本机构建还会把同一份自编目标文件链接为宿主参考程序，对照返回、退出、内存保护/
重用和超长写入的错误顺序，检查实际退出状态和逐字节输出。只有宿主程序为真实 dyld
入口链接 libSystem，来宾镜像仍不依赖动态库。这项原生对照是 HVF 门禁必需项，
不代表 iOS 真机执行证据。
独立的[原生内核工作流](../../.github/workflows/darwin-kernel-reference.yml)还会直接在
Intel 与 Apple Silicon macOS 上执行这些程序，无需构建 NeverD 或 LLVM。
`DarwinNativeCases.def` 统一维护 C++ 测试和独立宿主脚本的模式、退出状态及期望输出；
架构不符、Rosetta、超时或结果不符均失败，JSON 保留源码、系统和编译器信息。
`scripts/run_native_cpu_ci.py --require-darwin-backend hvf` 可以在本机验收；
Linux 使用 `kvm`，Windows 使用 `whp`，同时传入 `--build` 和 `--evidence` 路径。
```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

缺少用例注册、跳过必需用例或缺少 `ld64.lld` 均不能通过。
[原生 Darwin 专项工作流](../../.github/workflows/darwin-native.yml)提供关闭 Unicorn
后的 x64 KVM/WHP 构建和验收；它需要 runner 实际提供虚拟化能力，保留完整测试证据。
工作流也可以单独选择 `kvm` 或 `whp`。

## 本机验证（2026-10-03）

Apple M4 Max / macOS 15.6.1，Release 构建，源码为
`36e11ca8a3d80aecf585d3328018839ce7fdb989`：

| 范围 | 通过 | 失败 | 跳过 | 必需原生用例 |
| --- | ---: | ---: | ---: | ---: |
| 完整 HVF 门禁，关闭 Unicorn，20 个目标 | 834 | 0 | 5,903 | 13 / 13 |
| 每个 Darwin 工作负载，关闭 Unicorn | 65 | 0 | 221 | 39 / 39 |
| Darwin 与公开 C API/CLI，包含 Unicorn | 138 | 0 | 156 | — |

统计有重叠，不能相加。原生摘要记录干净源码，没有缺失注册或未执行的必需用例。
跳过项属于异架构、其他后端或关闭的软件后端。65 项 Darwin 检查包含加载器、内存、
每个 ARM64 工作负载，以及真实宿主内核对照。加入原生中断清单后，完整 HVF 门禁
现要求 ARM64 的 16 项或 Intel 的 14 项检查；Darwin 专项门禁另要求全部 39 / 26 项进程工作负载。
后续干净源码 `561ebf37b9eaaec08043ac5816b2e083ecccaf68` 的 ARM64 完整门禁达到
841 项通过、0 失败、5,939 项跳过，16 个必需用例全部执行；完整证据位于
`build-hvf-native/hvf-cancellation-full-evidence/`。

证据位于 `build-hvf-native/hvf-release-evidence/`、
`build-hvf-native/darwin-release-evidence/` 和
`build-hvf/verification/darwin-release-public.xml`。回归覆盖未映射尾部字节的文件预算、
各平台的线程入口、超长写入错误顺序及格式化换行后的测试清单解析。
原生清单、结果核对和 CI 脚本的 106 项回归通过；能力、文档、来源及格式检查也通过。

后续同步 `dev` 后，干净源码 `f4bf8dde5cbc33d18ce053cb0722a54047e5a2d0`
再次通过独立 ARM64 Darwin 门禁：65 项通过、0 失败、221 项跳过，39 个必需原生
工作负载全部执行。证据位于 `build-hvf-native/hvf-final-dev-darwin-evidence/`。
这次复跑验证 Darwin 目标，不代表同期合入的其他 Windows 进程改动已经通过新的完整 CPU 门禁。

后续干净源码 `d5864c055116a687546320e4acf0788ef4a4e735` 的方法级执行再次通过
39 个 ARM64 Darwin 工作负载（65 通过、221 跳过），286 个 CTest 身份与原始 GoogleTest
XML 全部核对一致。相同源码的完整 20 个目标覆盖 6,842 项，849 通过、0 失败、5,993 跳过，
16 个必需原生项全通过；包含后续 Windows 环境变更，摘要明确记录方法级进程隔离。
证据位于 `build-hvf-native/hvf-method-clean-{full,darwin}-evidence/`。

较早的集成验证已覆盖五种平台/架构的 Python SDK 调用；包内引擎与关闭 Unicorn 的
CLI 在三种 ARM64 平台的 18 个场景中报告一致。桌面包通过 186 个 Mach-O 的依赖、
签名和 Cocoa 启动检查。同时关闭 HVF 与 Unicorn 的配置通过 38 项检查、跳过 231 项
后端用例，且没有 Hypervisor.framework 依赖；这属于构建隔离证据。
提交 `e078b129c` 的[桌面 GUI 工作流](https://github.com/NeverSight/NeverD/actions/runs/37053518872)
也在 macOS、Windows、Ubuntu 三个平台通过。

## 托管宿主原生验证（2026-10-03）

[Intel HVF Darwin 门禁](https://github.com/NeverSight/NeverD/actions/runs/37106013999)
在干净源码 `8dcc74c59da303176801b99747a60339161b824b` 上通过 macOS 与 iOS Simulator
全部 **26/26** 个 x64 原生工作负载。286 个 CTest 身份均与原始 GoogleTest XML 核对一致：
**52 通过、0 失败、234 跳过**，无缺失、重复或未执行的必需项。跳过项分别为 65 个关闭的
Unicorn、39 个 ARM64 来宾和 130 个其他宿主后端；32 个方法进程全部正常退出。
原始程序与 macOS 宿主内核的对照也通过；这不代表 iOS device 内核或更广泛的 Intel CPU 验收。

产物 `11267489438` 已下载，SHA-256 为
`cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`，与 GitHub 元数据一致。
本地证据位于 `build-hvf/verification/hvf-intel-darwin-accepted/`。托管 Intel 使用已说明的
方法级串行执行策略，保留全部参数与 CTest 环境；宿主为四个逻辑 CPU 的 macOS x86-64、Darwin 24.6.0。
同一轮还通过 10 项 transport、100 次中断恢复和独立 CR8 回归；这些重叠检查不加入 Darwin 总数。

两个 x64 后端都在关闭 Unicorn 后通过最新 Darwin 专项门禁，包含文件预算、独立线程
入口及超长写入回归。源码均为 `36e11ca8a3d80aecf585d3328018839ce7fdb989`，
macOS 与 iOS Simulator 共 26 项必需进程用例全部执行成功：

| 宿主 / 后端 | 通过 | 失败 | 跳过 | 必需原生用例 |
| --- | ---: | ---: | ---: | ---: |
| [Windows / WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703/job/111023370601) | 51 | 0 | 235 | 26 / 26 |
| [Ubuntu 24.04 / KVM](https://github.com/NeverSight/NeverD/actions/runs/37062839703/job/111023370857) | 51 | 0 | 235 | 26 / 26 |

产物 `darwin-native-whp-x64` 和 `darwin-native-kvm-x64` 保留完整清单、JUnit、CTest
日志及源码/宿主摘要。两个工作区均干净，没有缺失或未执行的必需用例；下载产物的
SHA-256 已与 GitHub 摘要核对。跳过项包括仅适用于 macOS 的内核对照、异架构和其他后端。

这些结果验证所列传输上的有限 Darwin 模型。Intel Mac HVF 和各后端更广泛的 CPU
行为仍需各自的门禁验证。

## 独立 macOS 内核对照（2026-10-03）

提交 `e727d3eab7086063bb392444bd55014ac48d43c3` 的原始程序使用 Apple Clang 17.0.0
编译后，在两种 macOS 15.7.9 宿主上直接执行成功：

| 宿主架构 | 原生工作负载通过 |
| --- | ---: |
| [Apple Silicon ARM64](https://github.com/NeverSight/NeverD/actions/runs/37064795867/job/111029848093) | 4 / 4 |
| [Intel x86-64](https://github.com/NeverSight/NeverD/actions/runs/37064795867/job/111029847805) | 4 / 4 |

两个摘要均记录干净源码，`return`、`exit`、`memory`、`write-length` 都返回 37，
输出逐字节匹配且 stderr 为空；产物摘要也已核对。这为两种架构的原始工作负载提供
独立内核语义依据；模拟执行仍由上面的 HVF/KVM/WHP 结果覆盖。

## 显式时间观察值

`ProcessOptions::DarwinTime` / `darwin_time` 为所有 Darwin profile 提供 raw `gettimeofday` (116) 的固定观察值，包括第三个 `mach_absolute_time` 输出。`time_of_day`、`timezone` 和 `mach_absolute_time` 都可省略；省略表示未知，显式零是有效值，空对象不会补默认时钟。模型不读取宿主时钟、不推断时区、不推进时间，也不换算绝对 tick。

提供一个记录时，其全部成员必须齐全。`seconds` 是无符号 32 位，`microseconds` 为 [0, 999999]，`minutes_west` / `dst_time` 为有符号 32 位，绝对 tick 为无符号 64 位。JSON 使用共享的无损整数规则：超出安全整数范围时使用十进制字符串。未知字段、越界值及非 Darwin profile 在加载镜像前拒绝。

LP64 `timeval` 占 16 字节：偏移 0 是零扩展的秒数，偏移 8 是 32 位微秒，偏移 12 的四字节填充为零。时区是两个有符号 32 位字段，tick 占八字节。日历时间和绝对时间先联合采样，因此被请求的两种观察值必须在任何复制或指针检查前都存在。随后依次写入 timeval、timezone、absolute ticks。时区缺失在自己的阶段停止，保留已写入的 timeval；后续 EFAULT 同样保留此前写入，重叠地址遵循相同顺序。单次输出仅部分可写时，在该次复制前明确停止，保留更早的复制。三个空指针无需配置即可成功；选择性查询仅要求所请求的值。

原始 `time` 工作负载核对原生行为；来宾 `time-values` 在五种来宾组合的 C/CLI/Python 路径输出配置的精确 32 字节。独立 SDK 对照从一次原生 raw 调用采集三输出，再逐字节比较。这不包含自动推进时钟、tick 换算、commpage 计数器、定时器或 Mach 时钟对象/IPC；dyld、线程、Objective-C/Swift 和 Foundation/UIKit 仍需完善。Intel HVF Actions 保持暂停，本轮不增加原生 Intel 或实体 iOS 验收结论。

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

时间验证（2026-10-06，Release）：538 项 Darwin 注册测试，274 项通过、264 项因后端不可用跳过，零失败；66/66 项必需 ARM64 HVF 测试全部执行。12 个原始原生 macOS 工作负载及单次采样的 SDK 字节对照通过。公共 C/CLI/报告为 43/43，无跳过。Python 的五种来宾组合通过，包括精确时间字节及原有八种文件场景。66 项运行器测试、本地化、能力清单和格式检查通过。计数有重叠。证据：`build-hvf-arm64/darwin-time-verified-evidence/`、`darwin-time-native-first/`、`darwin-time-public.xml`。

## Mach 时间与返回约定

`darwin_time.timebase` 显式提供 `numerator` 和 `denominator`，均为非零无符号 32 位数；比例原样保留，不约分或换算。`mach_timebase_info_trap` 的索引为 89：ARM64 的 X16=-89，x64 的 RAX=0x01000059。输出为小端分子、分母共八字节，返回零。与 XNU 一致，完全无效的输出地址也返回零；部分可写输出在复制前明确停止，因为尚未建模其前缀写入。底层传输错误继续传播。缺少 timebase 时先停止，再检查指针，空指针也一样。

ARM64 特殊调用 X16=-3、X16=-4 分别返回完整无符号 64 位 `mach_absolute_time`、`mach_continuous_time`。各自只需要对应观测值，显式零有效。x64 的对应 Mach 表项会在原生内核触发 EXC_SYSCALL，因此模型明确拒绝。这不实现自动推进、commpage、定时器或 Mach 时钟对象/IPC。

服务分派只使用编号寄存器的低 32 位，报告保留原始 64 位。ARM64 负数选择 Mach；x64 的 Mach 类别为 0x01000000，BSD 类别为 0x02000000。命名空间独立，BSD 3/4 仍为 read/write；未知编号和其他架构的类别明确停止。解析后的绑定统一拥有返回约定：Mach 保留标志位和 X1/RDX，BSD 仍遵循 carry 和次结果规则；x64 仍更新 SYSCALL 返回所需的 RCX/R11。Mach 返回记录含 `result`，始终省略 `error`，即使输入 carry 已置位。

原始 `mach-time` 工作负载对照 ARM64 原生内核的标志位、次结果、高位编号、坏指针和 BSD 切换。`mach-timebase-values` 在五种来宾组合输出精确配置字节，`mach-clock-values` 在 ARM64 验证完整 tick；独立 SDK 对照验证布局和原生比例。Intel HVF Actions 继续暂停；x64 软件测试及语法检查不代表原生 Intel 或实体 iOS 验收。

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach 验证（2026-10-06，Release）：569 项唯一 Darwin 注册，293 通过、276 项后端不可用而跳过、零失败；69/69 项必需 ARM64 HVF 全部执行。最终运行通过全部 13 个原生工作负载及两个时间 SDK 对照。公开 C/CLI/report 为 100/100、无跳过；Python 覆盖五种来宾组合。公开接口比较按平台和场景独立运行，显式给出 10 秒来宾预算；产品默认值和超时回归保持不变。计数有重叠。

早期原生程序首次启动超过已有 5 秒门限：独立测得首次 6.056 秒、复用后 0.010 秒。同一二进制随后在原门限下通过 13 项，失败记录保留。宿主负载下的墙钟超时经独立串行复核通过。证据：`build-hvf-arm64/darwin-mach-time-final-evidence/`、`darwin-mach-time-native-recheck/existing-binary-recheck.json`、`darwin-mach-time-public-accepted.xml`，记录的是提交前工作树。当时 ARM64 MRS/MSR NZCV 尚未纳入受检查 CPU 契约，因此测试用整数指令观察标志。下述增量已补上这项 CPU 缺口。

## ARM64 条件标志寄存器

共享的受检查 ARM64 契约在 EL0、EL1 准入精确的 `MRS Xt, NZCV` 和 `MSR NZCV, Xt` 编码。读取只返回第 31–28 位；写入只取输入的这四位，其余位忽略。读到 `XZR` 会丢弃结果；从 `XZR` 写入会清空四个标志，不会读取 SP。各后端执行原始指令。宿主寄存器设置接口的验证、FPCR/FPSR 的受限策略保持不变；邻近且未列出的系统寄存器仍明确不支持。

`NeverDAArch64NZCVTests` 将全部标志组合与宿主原始指令对照，并检查完整标量/向量状态、内存、寄存器边界、观察器停止/失败、上下文恢复重试及共享指令预算。ARM64 `mach-time` 测试现在以真实 MSR/MRS 包围 SVC，覆盖 Mach 标志保持及返回 BSD 的切换。原生 HVF 必需项包含两种特权下的六个方法和宿主对照。ARM64 KVM/WHP、实体 iOS 尚未验证。可写文件、系统信息、推进时钟、Mach IPC/线程、dyld/运行时/框架及设备验收仍是后续环境工作。

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


可写文件验证（2026-10-06）：Release Darwin 610 项中 322 通过、288 不可用后端跳过、零失败；ARM64 HVF 必需项 72/72 执行。最终专项 114 项中 102 通过、12 跳过，含最后补充的 EFAULT 元数据断言；15 个原生程序与 111 个公共 C/CLI/报告测试全部通过。计数有重叠。首次原生用例发现 FWASWRITTEN 遗漏，已修复并保留失败证据。没有改变时限；GitHub 完整 CI 与 iOS 真机仍需单独验收，Intel Actions 保持暂停。

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python 首次整组测试在三个 ARM64 目录枚举场景超时；不改参数的诊断中，新增可写场景 10/10 通过，但一个 iOS 目录调用墙钟 5.005 秒、CPU 1.263 秒后超时。同一 5 秒限制下单独复验三个 ARM64 场景均通过（2.43–3.17 秒，10,941 条指令，输出 65）。16 逻辑核的宿主负载为 54–70，支持调度压力解释，不代表延迟稳定；原始失败保留。

最终未改参数的 Python 整组测试通过，覆盖五种 profile/ISA，耗时 41.118 秒；每进程仍限 5 秒，前面的失败和诊断记录独立保留。


元数据策略验证（2026-10-06）：Release 专项148项，124通过、24不可用后端跳过、零失败。完整Darwin645项中343通过、300跳过、两项既有ARM64 HVF目录枚举超时；原参数、原5秒时限的20项复测为8通过/12跳过，受影响项耗时3.818/3.949秒。合计75项必需HVF均有通过观察，但首次整轮失败仍保留。公共C/CLI/报告117/117（含73项Darwin比较）、Python五种组合27.359秒、原生程序15/15和runner66/66通过。分配策略不代表APFS；没有改时限。完整GitHub CI、Intel原生、iOS真机与完整环境仍需单独完成。

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

稀疏定位验证（2026-10-06）：Release Darwin 共 671 项，359 通过、312 项后端不可用而跳过、零失败；78 项必需 ARM64 HVF 全部执行，Unicorn 覆盖五种来宾。专项 147 项为 123 通过、24 跳过；16 个原生程序、122 项公共 C/CLI/报告（含 78 项 Darwin 比较）、Python 五组（12.344 秒）和 66 项 runner 全部通过。计数重叠，未改时限，历史失败保留。证据：`build-hvf-arm64/sparse-seek-validation-summary.json`。分配几何属于显式虚拟策略，不能视为 APFS 等价；完整 CI 和 iOS 真机仍需验收，Intel HVF Actions 保持暂停。

删除验证（2026-10-06）：Release Darwin 708 项为 384 通过、324 项后端不可用跳过、零失败，81 项必需 ARM64 HVF 全部执行。专项 156 项为 137 通过/19 跳过；原生 17/17、公共 C/CLI/报告 128/128（83 项 Darwin 比较）、Python 五组 16.268 秒、runner 66/66 均通过。独立设计与实现审查无剩余阻塞；计数重叠、时限未改，无需重试。证据：`build-hvf-arm64/unlink-validation-summary.json`。目录失效与固定策略时间属于模型规则，完整文件系统/运行时和 iOS 真机仍未验收；Intel HVF Actions 继续暂停，完整 GitHub CI 单独验收。

### 创建验证，2026-10-06

Release Darwin 共 748 项：412 通过、336 项后端不可用跳过、零失败，84 项必需 ARM64 HVF 全部执行。专项 162 项为 150 通过/12 跳过；公共 C/CLI/报告 133/133（88 项 Darwin），Python 五组 9.982 秒、原生 18/18、runner 66/66 均通过。初轮 ARM64 正确拒绝测试指针表引入的重定位；改为内联字节后通过，未放宽加载规则，原失败和二进制保留。runner 预期清单同步从 27 改为 28 个工作负载。独立审查无剩余阻塞，并补测容量失败不改变父目录观测。计数重叠，时限未改。完整 GitHub CI、iOS 真机另行验收；Intel HVF Actions 仍暂停。

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### 创建元数据验证，2026-10-06

Release Darwin 共 787 项：439 通过、348 项后端不可用跳过、零失败，87 项必需 ARM64 HVF 全部执行。专项 151 项中 139 通过、12 跳过。公共 C/CLI/报告 145/145 通过，含 98 项 Darwin 输入比较；未改动的 Python 方法在 12.211 秒内通过五组配置。原生程序 19/19、验收脚本 66/66 通过。独立审查无剩余阻塞，新增跨父目录设备/组与全局 inode、首次写入前删除、FD 满且输入不可用时修改 umask 的测试。计数重叠、时限未改，无需失败重试。固定创建/修改时间及分配仍是显式虚拟策略；完整 GitHub CI 与 iOS 真机单独验收，Intel HVF Actions 继续暂停。

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### 重命名验证，2026-10-06

Release Darwin 共 835 项：474 通过、360 项后端不可用跳过，既有 macOS ARM64 HVF 虚拟元数据用例超时一次（5.087 秒）。原参数、原 5 秒时限复查该方法：8 通过、12 跳过，受影响项耗时 0.113 秒。两次运行合计覆盖全部 90 个必需 ARM64 HVF 身份；完整门禁仍记录为失败。重命名专项 42/54 通过、12 跳过。公共 C/CLI/报告 150/150，含 103 项 Darwin 输入比较；未改动的 Python 方法在 18.478 秒内通过五组配置。原生 20/20、验收脚本 66/66 通过。独立审查发现嵌套点路径分类错误，4K/16K 回归先复现失败，修复后通过；更早的只读 ftruncate 测试预期已纠正为 EINVAL。保留全部失败和探针版本，计数重叠、时限未改。完整 GitHub CI、iOS 真机另行验收，Intel HVF Actions 继续暂停。

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## 显式系统观察值

`ProcessOptions::DarwinSystem` / `darwin_system` 为所有 Darwin 配置的 `sysctl(202)` 和原始 `sysctlbyname(274)` 提供固定观察值。各字段均可省略；未提供的值或未列出的键明确停止为不支持。不会查询宿主或推测版本、机型。严格 JSON 与 C++ 校验在加载映像前拒绝非法值和非 Darwin 配置。

`os_revision` 为有符号 32 位；`cpu_count` 为 1..INT32_MAX；`memory_size` 保留无符号 64 位；`max_files_per_process` 为 0..INT32_MAX，按四字节 int 编码。其余标量字段是最多 1023 字节（`hostname` 限 255 字节）且无内嵌 NUL 的字符串，允许显式空串，返回内容包含结尾 NUL。观测不会改变调度、分配或描述符预算。

| JSON 字段 | sysctl 名称 | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |
| `max_files_per_process` | `kern.maxfilesperproc` | `1,29` |
| `hostname` | `kern.hostname` | `1,10` |

`hw.pagesize` 来自现有客户机内存策略，通常返回8字节；非空输出且容量恰为4时返回4字节。旧 MIB `[6,7]` 与名称 `hw.pagesize_compat` 始终返回4字节。`hw.pagesize` 的动态数字 OID 仍不支持。`hw.memsize` 在容量恰为4时，仅当64位模式是有符号32位值的符号扩展才缩窄，否则返回 ERANGE34，保持输出和长度不变。

MIB 数量取低32位，须为2–12；名称长度取完整64位且须小于1024。先检查全部指定字节，再按首个 NUL 解释名称并移除一个末尾点；空名称返回 ENOENT，部分可读输入仍不支持。非空 `oldlenp` 必须在任何副作用前完整具备8字节读写权限。原生非法长度指针探测未在时限内返回，因此这类指针明确留在不支持边界。空 `oldlenp` 表示容量0；空 `oldp` 仅查询长度。除 `kern.hostname` 外，短缓冲区返回 ENOMEM12、不写数据并把长度置0；数据 EFAULT 保持旧长度。输入和容量先取快照，随后写数据，最后写长度，保留别名顺序及后续传输失败前已完成的复制。

`hostname` 声明该 guest 调用者可见的字节，不查询宿主、不为移动环境补出 `localhost`，也不推断 entitlement。缺省为未知，显式空串返回一个 NUL。`kern.hostname` 的非空输出若容量为正且不足，会成功返回恰好该容量的字节，末尾补 NUL，并报告该容量。零容量仍返回 ENOMEM12、长度0且不写数据；空输出指针报告包含 NUL 的完整长度。仅检查实际输出范围；部分可写范围仍明确不支持且不发布前缀，原生部分复制行为不在模型内。这只增加 libc uname/gethostname 使用的原始观测，不实现其 dylib 导入或完整运行时。

只有 newp/newlen 均非零才是写请求。先保留名称/MIB 与 oldlenp 完整读写预检，然后默认或显式非 root EUID 在观察值和数据输出检查前返回 EPERM1。显式 EUID0 对原生允许特权写的 kern.osversion / kern.maxfilesperproc / kern.hostname 明确停止 unsupported，因为未建模特权写入；RUID 不决定此分支。其他原生只读节点即使 root 仍 EPERM1。新长度0忽略指针；未知键、树和动态 OID 不猜 ENOENT。

原创 `system-info` 程序检查原生 macOS 与客户机 ABI；`virtual-system` 通过 C++、C/CLI、Python 比较配置的精确字节。独立 SDK 对照将宿主九项观察值显式作为测试输入，比较名称与数字查询输出。这不构成 iOS 真机或 Intel HVF 验收。

```json
{"darwin_system":{"hostname":"guest-node","os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/sysctl.3.html).

### 系统查询验证，2026-10-06

Release Darwin 共881项：509通过、372因后端不可用跳过、零失败；93项 ARM64 HVF 必需项全部执行。聚焦检查49项中37通过、12跳过。公开 C/CLI/配置报告163/163通过，含113项 Darwin 输入对照。未改变的 Python 方法在15.302秒内通过五种配置；原生程序21/21、验证脚本66/66通过。独立审查无阻塞，补充的错误优先级组合与独立 SDK 捕获对照均通过。新增 SDK 对照曾因缺少 StringExtras 头文件而编译失败，补上后通过，原始日志和源码已保留。原生非法长度指针探测也已保留，明确在支持边界之外。通过测试后仅整理了两个文件头注释，并成功重建。计数重叠、时限不变，无需运行失败复测。完整 GitHub CI 与 iOS 真机仍待分别验证；Intel HVF Actions 保持暂停。

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## 向量文件与输出捕获 I/O

`readv`/`writev`、`preadv`/`pwritev` 及 nocancel 入口复用标量文件与输出捕获实现，不增加配置或宿主访问。LP64 iovec 包含8字节地址和8字节长度；iovcnt 的有符号低32位须为1–1024。描述符查询前复制完整数组，输出别名不能改写请求；部分可读的数组仍明确不支持。

先检查描述符访问权及流的定位能力，再校验长度。每项及总长度须在 INT64_MAX 内；普通文件和目录另限总长 INT_MAX。流不套用 vnode 限制：有限 stdin 按可用字节截短，输出捕获遵守预算。pwritev 的所有负偏移均在数组读取前拒绝；preadv 在描述符和长度后检查偏移。零长度项忽略地址，但仍检查描述符、目录和偏移。EOF 后不访问多余项。定位调用保留共享游标，pwritev 忽略追加标志；普通追加先按原游标一次性裁剪总请求，再选择 EOF。

按向量顺序复制。后续完全无效的缓冲区返回 EFAULT，保留之前完成的字节、普通游标进度及实际写入非零字节时的 FWASWRITTEN。已获准的非零文件写入因数据缓冲区 EFAULT 返回时，使完整元数据失效，即使已有虚拟成功策略；参数错误、模型准入拒绝及后端失败保留元数据。单项读取目的区部分可写时，返回 UnsupportedService，不复制当前项，保留更早复制；单项文件写源部分可读时，在任何文件副作用前明确拒绝。授权、映射租约和总存储预算先检查；后端预检或读取失败不会发布文件或捕获字节。

向量捕获在访问数据前检查 stdout/stderr 共用预算。跨越用户地址上限的项不贡献任何字节，保留之前各项；其他部分可读项保留已检查前缀并返回 EFAULT。标量地址范围错误仍优先于输出预算。复制或重定向描述符保留原始输出流。原创 `vectored-io` 在原生 macOS、五种客户机及公开 C/CLI/Python 验证全部8个入口；不增加取消、管道、线程或 iOS 真机验收。

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### 向量 I/O 验证，2026-10-06

Release Darwin 共937项：553通过、384因后端不可用跳过、零失败；96项必需 ARM64 HVF 全部执行。专项57项中45通过、12跳过。公开 C/CLI/报告168/168通过，含118项 Darwin 输入对照；Python 在20.397秒内覆盖五种配置。原生工作负载22/22、验证脚本66/66通过。独立审查补充稀疏定位写入故障测试，验证游标、实际 EOF、元数据拒绝及精确剩余存储容量。初次构建因旧测试仍调用已移除的内部查询而失败，现改为验证实际捕获输出；新增服务事件断言的 optional<bool> 误用曾使8项本已成功的客户机运行报失败，修正后相关检查全部通过。两次失败均保留源码与日志。计数重叠、时限不变；完整 GitHub CI 与 iOS 真机另行验证，Intel HVF Actions 保持暂停。

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## 文件存在性查询

`access(33)` 与 `faccessat(466)` 查询当前虚拟目录，不分配描述符、不改变文件字节、游标、标志或元数据。F_OK 按现有目录遍历合同确认名字存在。元数据不会授予或撤销目录访问权；这些调用不验证原生祖先搜索权限、ACL 或 MAC。缺少或已失效的 stat 观察值不影响存在性查询。即使旧 FD 或映射仍持有对象，删除的名字仍返回 ENOENT；创建、同名重建和重命名查询当前命名空间。

模式取低32位。R/W/X 使用位0–2，扩展权限使用位9–21；`(mode & 0x003ffe07) == 0` 时是存在性查询，其余位（含符号位）按原生规则忽略，不误报 EINVAL。实际权限请求在成功查找后明确返回 UnsupportedService，即使元数据或修改授权看似允许；已知路径和描述符错误先返回，不猜测权限结果。

Faccessat 接受低位标志 AT_EACCESS(0x10)、AT_SYMLINK_NOFOLLOW(0x20)、AT_SYMLINK_NOFOLLOW_ANY(0x800) 的任意组合。其余标志在访问路径或 FD 前返回 EINVAL，即使未配置目录。真实/有效身份固定；固定链接采用下文的解析策略。绝对路径忽略 dirfd；相对路径沿用配置的 CWD/目录 FD 规则。非 AT_FDCWD 的 nameiat 路径先读首字节、检查相对目录 FD，再导入完整字符串；首字节 `/` 跳过 FD。首字节故障为 EFAULT14；相对坏/文件 FD 的 EBADF9/ENOTDIR20 先于后续字节故障。相对空路径仍检查 FD：未知 FD 为 EBADF、普通文件为 ENOTDIR，其余为 ENOENT。目录未配置或流的目录身份未知时仍明确不支持。

独立 `file-access` 工作负载在原生 macOS、五种客户机及 C++、C/CLI/Python 比较两个调用、忽略位、标志组合和查找顺序。原生 NOFOLLOW_ANY 使用相对目录 FD，避免宿主 `/tmp`、`/var` 符号链接影响对照。直接测试覆盖命名空间变化、混合权限位、描述符耗尽、元数据独立性和客户机内存失败。

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### 文件存在性验证，2026-10-06

Release Darwin 共971项：575通过、396因后端不可用跳过、零失败；99项必需 ARM64 HVF 全部执行。专项35项中23通过、12跳过，含全部14项直接测试。公开 C/CLI/报告173/173通过，含123项 Darwin 输入对照。Python 在16.235秒内通过五种配置；原生工作负载23/23、验证脚本66/66通过。独立设计与实现审查未发现阻塞。原生探针保留宿主 /tmp 符号链接导致的初始 NOFOLLOW_ANY 结果及后续规范路径对照；共用工作负载使用相对目录 FD。计数重叠、时限不变，无需运行失败重测；完整 GitHub CI 与 iOS 真机另行验证，Intel HVF Actions 保持暂停。

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## 创建与删除目录

`mkdir(136)`、`mkdirat(475)` 在明确允许修改的直接父目录中创建目录。新目录继承命名空间修改授权；初始目录保留各自授权。新目录仅继承父目录已知的设备号与组 ID，不生成完整 stat、大小、分配、时间或枚举游标；复用旧文件名时遮蔽全部旧观察值。`creation_policy` 仍仅适用于普通文件：嵌套文件使用继承的父身份和现有全局 inode 序列，mkdir 不消耗普通文件 inode。权限执行和原生目录元数据仍不在此合同内。

所有调用共用逐组件解析器。mkdir 可创建末尾仅跟斜杠的缺失名字；缺失祖先后跟点/双点仍为 ENOENT，普通文件祖先为 ENOTDIR，已存在名字为 EEXIST。相对路径沿用 FD/CWD，绝对路径忽略 dirfd，字符串故障先于相对 FD 错误。创建不需要可用 FD；路径、授权、字节/条目预算或内存传输拒绝不发布名字、不使父观察失效。

`rmdir(137)` 及带 AT_REMOVEDIR(0x80) 的 `unlinkat(472)` 可删除本进程创建的空目录，可同时带 AT_SYMLINK_NOFOLLOW_ANY(0x800)。未知低32位标志先返回 EINVAL；DATALESS、SYSTEM_DISCARDED 仍不支持。保留已知路径/类型/根目录错误；没有 removable 授权的初始目录删除仍为 UnsupportedService。已准入目录末尾点为 EINVAL，从仍有名字的目录出发的双点或非空目标为 ENOTEMPTY。目录 FD（含 dup）或 CWD 保留原目录对象，不再阻止删除。已 unlink 的普通文件 FD/映射不算目录项；原生对照确认其字节、inode 与最后链接 F_GETPATH 在父目录删除及名字复用后保持。

每个新目录以规范路径加 NUL 和一个条目计入共享16 MiB/256条目预算。已删除目录不可达后仅退回自身费用，保留孤立文件字节与映射租约。成功修改使直接父目录完整 stat/枚举观察失效，失败则保留。即使配置普通文件创建策略，新目录元数据和快照仍未知。原创 `directory-mutations` 经原生 macOS、五种客户机及 C++/C/CLI/Python 验证嵌套创建、重命名、unlink、删除与孤立对象复用。 文件描述或映射租约持有的孤立文件也保留父目录对象：即使目录 FD 全部关闭，父链的当前路径/NUL 和条目费用仍保留，直到先回收文件、再逐层回收目录。移动仍链接的创建祖先会更新这些旧对象的 F_GETPATH；同名替代对象不会接管它们。

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### 目录修改验证，2026-10-06

Release Darwin 共1,017项：609通过、408因后端不可用跳过、零失败；102项必需 ARM64 HVF 全部执行。定向测试58通过、12跳过，含26项新增4K/16K直接测试。公开 C/CLI/报告178/178通过，含128项 Darwin 输入对照；Python 在17.255秒内通过五种组合，原生工作负载24/24、验证脚本66/66通过。独立审查核对预算、名字复用、父身份、文件租约与失败回滚。初始完整测试通过后，额外原生探针发现纯斜杠根删除应为 EISDIR，而末尾点/双点根为 EBUSY；已统一判断并补充原生/客户机共用断言，保留初始测试及源文件/二进制快照。计数重叠，时限未放宽；Intel HVF Actions 继续暂停，完整 GitHub CI 与 iOS 真机另行验证。

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## 保留的目录身份

已删除目录通过 FD/CWD 保留原父目录链，父名字被删除并复用也不改变身份。打开点路径产生独立游标，dup 共用游标；双点沿原父对象遍历，已删除目录中的普通子名称返回 ENOENT，不会看到替代目录。普通 LOOKUP 可穿过保留的已删除父对象，创建/删除/重命名查找则返回 ENOENT。重命名目标末尾点/双点在该组件遍历前返回 EINVAL，但更早祖先错误优先。F_GETPATH 保留最后路径，完整 stat/枚举仍未知。删除的新目录在 FD、CWD 或旧子目录引用全部释放前，持续占用路径+NUL及一个条目；close、dup2、CWD 修改和修改准入会回收不可达链。初始输入费用和普通文件租约独立保留。原创 `deleted-directories` 在原生 macOS 与五种配置对照持有删除、父链/名字复用、查找意图和仅 CWD 持有。 文件描述或映射租约持有的孤立文件也保留父目录对象：即使目录 FD 全部关闭，父链的当前路径/NUL 和条目费用仍保留，直到先回收文件、再逐层回收目录。移动仍链接的创建祖先会更新这些旧对象的 F_GETPATH；同名替代对象不会接管它们。

### 目录生命周期验证，2026-10-06

Release Darwin 共1,051项：631通过、420因后端不可用跳过、零失败；105项必需 ARM64 HVF 全部执行。定向98通过、12跳过；初始直接测试64/64，含14项新增及持有删除更新。公开 C/CLI/报告183/183，含133项 Darwin 输入对照；Python 五种配置18.691秒通过，原生25/25、脚本66/66。初始定向通过后，额外原生重命名探针纠正末尾点错误顺序；原始源文件、结果和先前快照保留。主代理完成源码/证据核对，最终独立审查不可用。计数重叠、时限不变；完整 GitHub CI 与 iOS 真机另验，Intel HVF Actions 暂停。

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## 显式准入的初始目录删除

目录项可使用严格布尔值 `"removable": true`，C++ 对应 `DarwinFileOptions::RemovableDirectories`。这声明目标是只有一个命名空间身份的普通非挂载目录。目标必须是显式配置的初始 `directories` 项且不是根，其直接父目录必须显式允许修改。已知特殊模式/标志、inode 别名（包括目录快照中的身份）或父子已知设备号冲突均拒绝准入。设备号相同本身不能证明没有挂载。省略或 false 保持不支持，其他 JSON 类型非法；此选项不提供通用权限或挂载模型。

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

删除要求当前命名空间为空；初始隐式子目录不会因最后一个原始文件被 unlink 而消失。成功删除使对象及直接父目录的完整 stat/枚举失效，旧 FD/dup/CWD 仍保留原对象和父链。删除的初始名字不会由不可变输入重新出现；同名新文件或目录拥有独立身份，不继承旧元数据或快照，调用方输入保持不变。

每个 removable 引用的路径和 NUL 计入初始 16 MiB 预算。初始目录项、路径、引用和快照在删除及最终关闭后仍保留费用，也不返还 256 项上限中的初始项；新对象继续单独计算动态费用。原创 `initial-directory-removal` 在持有原生测试已有空目录时删除它，先重建为文件再建为目录，验证仅 CWD 保留的行为并恢复空目录；同一程序通过 C++/C/CLI/Python 覆盖五种来宾组合。

### 初始目录删除验证，2026-10-06

最终 Release 源码核对了 1,089 项 Darwin 注册测试：657 项通过、432 项因后端不可用跳过、零失败；108 项必需的 ARM64 HVF 测试全部执行。聚焦验证为 27/39 通过、12 项不可用跳过，新增的仅快照别名检查也通过。公共 C/CLI/报告测试为 191/191；Python 在 76.276 秒内覆盖五种组合；原生独立工作负载 26/26、证据运行器测试 66/66。初次测试中不一致的 inode/快照输入已修正，失败记录仍保留。

此前两轮完整验证分别在既有文件/重命名用例中出现 1 次和 3 次超时；带诊断的验证复现了一次文件超时，实际耗时 5.008 秒、进程 CPU 用时 0.171 秒。相同方法及旧程序对照均通过，但延迟根因仍未确定，最终通过不代表超时稳定性已解决。临时诊断已移除，程序文件哈希已恢复，客体原有 5 秒限时未变。已完成主代理源码/证据核查；独立审查不可用。计数相互重叠；完整 GitHub CI、物理 iOS 和已暂停的 Intel HVF Actions 不属于本地验收。

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## 普通文件的独占重命名

完成源和目标查找后，RENAME_EXCL 对不同的已有文件或目录返回 EEXIST，先于挂载和命名空间修改检查。更早的路径错误仍优先，包括末尾点/双点的 EINVAL。目标不存在时复用同一个有界重命名事务，保留已打开描述、游标、标志、映射租约和配置的元数据变化。同对象独占重命名仍明确不支持：原生结果依赖文件系统大小写敏感性，精确目录键无法证明这一属性。大小写折叠、初始目录重命名及 SECLUDE 尚未纳入。已有原生 `renamed-file` 工作负载现验证拒绝时元数据不变，以及 EXCL|NOFOLLOW_ANY 成功移动，覆盖原生 macOS 和 C++/C/CLI/Python。

验证，2026-10-06（Release）：1,097 项 Darwin 注册测试，665 项通过、432 项因后端不可用跳过、零失败；108 项必需 ARM64 HVF 测试全部执行。聚焦测试 44 项通过、12 项不可用跳过，含八项新增直接测试。公共 C/CLI/报告 191/191；Python 五种组合耗时 19.241 秒。独立原始调用探针通过 26 项检查。首次完整原生验证中，既有 return 用例超时，其余 25 项（含 renamed-file）通过。同一未修改程序的 return 三次复查耗时 0.014–0.034 秒，随后全部 26 项原生用例在原有 5 秒限时内通过。初次失败保留且根因未明；这些结果和此前最终 HVF 通过均不构成延迟稳定性保证。已完成主代理核查，独立审查不可用。计数重叠；物理 iOS、完整 GitHub CI 和暂停的 Intel HVF Actions 不属于本地验收。

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.


## 已创建目录之间的跨父目录重命名

同一个初始目录及本进程通过 mkdir/mkdirat 创建的全部后代共享一个虚拟名称空间域。`rename`、`renameat`、`renameatx_np` 可在这些父目录之间移动普通文件，无需新增 JSON 字段。例如在可修改的初始 `/work` 内创建 `/work/left` 和 `/work/right` 后，`/work/data` 可移入任一子目录，也可在子目录之间移动。若 `/work/left` 是单独提供的初始目录，它仍是独立域，即使设备号相同也不推定共享挂载。一般挂载拓扑仍未知。

两个直接父目录均需名称空间修改授权；新建目录继承授权及已知 device/GID。重命名保留文件自身的身份、所有者/组、写授权和分配。已知设备冲突仍明确拒绝。实际移动使两个父目录的完整 stat/列举观察失效。已删除初始目录与重用路径始终是不同对象，旧目录 FD/CWD 不会得到替代对象的域。

既有有界替换事务、映射保留、路径/NUL 费用及错误顺序继续适用。EXCL 遇到其他既存目标，仍在域与授权检查之前返回 EEXIST。原始 `renamed-file` 程序现在创建子目录、移入子目录、回到初始父目录覆盖文件，再移入子目录，通过 C++/C/CLI/Python 和原生 macOS 对照。权限强制检查、初始目录重命名及硬链接、动态符号链接及真实 APFS 元数据仍未完成。

### 跨父目录验证，2026-10-06

最终 Release 验收核对了 1,115 个 Darwin 注册项：683 通过、432 因后端不可用跳过、零失败；108 个必需 ARM64 HVF 项全部执行。直接检查 56/56 通过，包括新增 18 个 4K/16K 用例。公开 C/CLI/report 191/191 通过；Python 方法在 22.254 秒内覆盖五种环境。原始原生程序 26/26、单独的原始系统调用探针 34 项、文档/能力/证据运行器脚本测试 296/296 均通过。计数有重叠。仅针对 MSVC 的 CMake 调整后，十个验收二进制的哈希均未改变。

此前两次完整验收分别保留了已有 HVF 文件方法的三次和两次超时。完整方法、工作目录和会话控制检查通过，但未确立原因；最终通过不能证明延迟稳定。探针起初把 /tmp 文本路径与 /private/tmp 规范路径比较，改为从 root FD 查询路径后修正了四个预期。扩展原生程序最初清理误用 mkdir(136)，改成 rmdir(137) 后修复 exit150。初始源和失败证据均保留，来宾时限仍为原来的 5 秒。

完整 Linux CI 发现的四处 LP64 测试初始化列表冲突已改为显式 uint64_t；MSVC 下的 NeverDJumpTableTests 已添加 /bigobj。实际 Linux/Windows 编译仍等待 CI。此前完整 CI 还报告了独立的 Windows EH 语料和关闭 PR 取消任务失败。已完成主代理源/证据自审；没有宣称独立审查、实体 iOS 或暂停的 Intel HVF 验收通过。

`build-hvf-arm64/cross-parent-rename-validation-summary.json`, `cross-parent-rename-darwin-synced-evidence/`, `cross-parent-rename-darwin-evidence/`, `cross-parent-rename-darwin-rechecked-evidence/`, `cross-parent-rename-public-synced.xml`, `cross-parent-rename-python-synced.log`, `cross-parent-rename-native-fixed-evidence/`, `cross-parent-rename-probe/`.

## 普通文件名字的原子交换

RENAME_SWAP=0x2 通过 renameatx_np 交换两个已有普通文件的名字，可加 RENAME_NOFOLLOW_ANY。显式初始目录必须同时声明 mutable:true 和 swap_rename:true；C++ 使用 DarwinFileOptions::SwapRenameDirectories。创建后代继承原始目录对象的能力，删除后重用路径不会取得旧对象的声明。false 或省略表示能力未知，不能仅凭设备号相同或命名空间授权推断支持；不同初始目录域仍不支持。

源与目标复用组件解析器。目标缺失时，在目录域、授权和能力检查前返回 ENOENT。任一目录操作数都明确不支持：原生可交换文件和目录，模型不能套用普通重命名的 EISDIR。同对象交换在获得命名空间授权后为空操作，即使未声明交换能力也不改变状态。RENAME_EXCL=0x4 + RENAME_SWAP=0x2、未知 flags 仍在读取路径前返回 EINVAL；SECLUDE 仍不支持。

两份文件都保持链接，各自的身份、所有者/组、字节、写入授权、打开描述、游标、标志和映射租约保留。已配置虚拟策略只更新各对象自身 ctime；策略缺失或已失效时完整元数据仍未知。实际交换使两边父目录的完整元数据和枚举观察失效，不消耗创建 inode、目录项或 FD，也不改变调用者输入。

每个能力引用的路径+NUL 占用固定初始 16 MiB 预算。事务先验证两边完整的动态名字费用，再发布两个名字；仍链接的字节和租约不能提供覆盖回收额度。重复交换复用这些动态费用。原始 renamed-file 程序跨创建的子目录交换并交换回，检查两份对象后继续普通覆盖，覆盖原生 macOS 和所有 C++/C/CLI/Python 来宾配置。权限强制、挂载拓扑、大小写折叠和初始目录重命名仍待实现。

[Apple 卷交换能力](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsswaprenaming?changes=__1_2)、[XNU 重命名标志与查找顺序](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c)。

```json
{"darwin_files":{"files":[],"directories":[{"path":"/work","mutable":true,"swap_rename":true}]}}
```

### 交换验证，2026-10-06

Release Darwin 共 1,133 项：701 通过、432 项后端不可用跳过、零失败，108 项必需 ARM64 HVF 全部执行。重命名直接检查 68/68 通过，含 18 项新增选项和 4K/16K 检查。公共 C/CLI/报告 192/192；Python 方法在 16.153 秒内覆盖五组配置。原始原生工作负载 26/26，独立原始调用探针通过 45 项。计数重叠，来宾时限未改。

最初两项直接测试对未知写入授权和修改后未知元数据使用了错误预期；修正预期保留了已有语义所有者。最初 JSON 过滤器选中零项，不计入验收；随后实际测试所有者和完整公共测试通过。初始源与结果均保留。既有 HVF/原生延迟原因仍未知，本次通过不证明稳定性。已完成主要源码/证据自查；不宣称独立审查、iOS 真机、完整 GitHub CI 或暂停中的 Intel HVF 验收。

`build-hvf-arm64/swap-rename-validation-summary.json`, `swap-rename-darwin-evidence/`, `swap-rename-public.xml`, `swap-rename-python.log`, `swap-rename-native-evidence/`, `swap-rename-probe/`.


## 进程创建目录的重命名

普通 rename、renameat、renameatx_np 可在一个初始目录对象域内移动仍链接的进程创建目录及其子树；两边直接父目录必须获准修改。不存在的目录目标允许末尾斜杠；普通文件目标为 ENOTDIR，非空目录为 ENOTEMPTY，移入后代为 EINVAL。获准普通同名操作不改变状态。EXCL 遇到不同既有目标时先返回 EEXIST，先于类型、循环、域和授权检查；目标查找错误先于源末尾点/双点。源点/双点与目标为同对象时，大小写属性未知，明确返回 UnsupportedService。初始或已删除目录源、初始目录替换目标、不同初始域、链接、权限执行和 SECLUDE 仍不支持。

事务按父对象链选择后代，更新具名子树、被 FD 保留的已删除子目录，以及 FD/映射保留的孤立文件路径。源目录的 FD、dup、CWD 和双点仍指向同对象及其新父目录。被覆盖的空创建目录保留旧路径和原父链：普通子名称为 ENOENT，点/双点和 CWD 仍可保留旧对象。再次移动新源时，具有相同路径文本的旧目标孤立文件不会一起移动。

后代文件字节、身份、完整元数据、写授权、共享或独立游标、FD 标志及映射租约保持。源目录及两个直接父目录的完整 stat/枚举失效，子文件不会仅因祖先移动而失效；创建目录继承的命名空间授权继续适用于后续创建及获准普通文件改名。不消耗新条目、FD 或创建 inode。

发布前为全部具名和保留后代预分配新键与路径，核对每条规范路径/NUL 的 1024 字节界限和共享 16 MiB 预算。旧动态路径费用各替换一次；目标没有 FD、CWD 或保留后代时才提供回收额度，最终回收只计一次。失败保留所有名称、父对象、文件状态、游标和映射。仅映射持有的孤立文件也保留已删除父目录的路径和条目预算。

原始 SDK-free `renamed-directory` 程序在原生 macOS 与五种来宾配置间经 C++/C/CLI/Python 对照。4K/16K 直接检查覆盖对象复用、完整回滚、精确容量、长后代路径、替换额度、映射保留父链回收，以及条目/FD/inode 耗尽。


### 2026-10-06

最终 Release Darwin 共 1,175 项：731 通过、444 因后端不可用跳过、零失败；111 项必需 ARM64 HVF 全部执行。聚焦 32/44（12 跳过，含22项新增4K/16K）；最终边界/旧测试4/4。公开 C/CLI 165/165、报告32/32，无跳过；Python 五种配置21.759秒，原生27/27，独立探针79项。独立审查发现并确认修正了测试程序根路径分隔符及同对象源点的文件系统属性边界。初始来宾 exit124 和两项过时预期失败及其源码/二进制均保留，最终完整测试通过。计数重叠，时限未改；此前 HVF/原生超时根因仍未知，本轮不证明延迟稳定。Intel HVF Actions 仍暂停；iOS 真机及完整 GitHub CI 另行验证。

`build-hvf-arm64/directory-rename-validation-summary.json`, `directory-rename-darwin-final-evidence/`, `directory-rename-public.xml`, `directory-rename-reports.xml`, `directory-rename-python.log`, `directory-rename-native-evidence/`, `directory-rename-probe/`, `directory-rename-initial-fixture/`, `directory-rename-darwin-evidence/failed-source/`.

### 目录与混合类型交换验证，2026-10-07

Release Darwin 验证核对了 1,219 项注册：763 项通过、456 项因后端不可用跳过、零失败；114 项必需 ARM64 HVF 用例全部执行。聚焦检查为 32/44 通过、12 项不可用跳过，包含全部 24 项新增 4K/16K 直接用例；完整文件组件为 317/317。原生程序为 28/28，独立原始系统调用探针在本机不区分大小写的 macOS 文件系统上记录了 32 项成功观察。计数有重叠，客户程序时限未变。

独立计划与实现审查核对了双向事务、初始文件路径计费、两侧保留子树、仅映射保留的孤儿及准确回收。最初红测试漏配显式根目录交换声明，排除在验收之外；修正后的旧实现在六项用例中均因拒绝目录交换而失败。两项早期混合文件断言错误地丢弃了配置元数据，现比较仅 ctime 改变的完整记录。局部常量指针表曾引入 ARM64 重定位，导致六项加载拒绝；改为四项标量断言后，固定程序没有经典重定位，加载器仍明确拒绝不支持的 fixups。

修正程序后的首次聚焦运行保留了三次五秒 HVF 超时。单例与三配置控制检查通过，随后完整验证通过；超时原因仍不明确，不能据此证明时延稳定。初始源码、二进制、失败与控制记录均保留。初始目录移动、独立初始域、权限/挂载/大小写声明、动态依赖和框架运行时仍未完成；物理 iOS、已暂停的 Intel HVF 与完整 GitHub CI 属于独立验收边界。

`build-hvf-arm64/directory-swap-research/`: `darwin-final-evidence/`, `darwin-evidence/`, `focused-v5.xml`, `file-owner-v5.xml`, `native-workload-fixed-evidence/`, `native-probe-v2-results.json`, `fixture-fixups-before/`, `hvf-controls.json`.

最终链接后的二进制再次通过完整 Darwin 验证。固定 dev 0a9a1d28d 的 20 组共享组件检查共记录 4,515 项：4,489 项通过、6 项可选 Z3 与 20 项缺失 Windows EH 样本跳过、零失败。其中公共 C/CLI 为 170/170，报告为 32/32。Python 在 30.780 秒内验证全部五种配置。12 种原生移动端架构/重定位组合的 4,532 项结果与原程序一致；单会话及完整 Swift 元数据对照为 12/12。Swift witness 生成器在修正注释缩进后，使用记录的 SDK/编译器重现了目录。这是本地 Release LLVM 23/Apple Clang 17 的验收，不代表后续 dev 修订或 Linux Clang 18 已验收。


## 显式预置目录子树的普通移动

预置的非根目录可用严格布尔值 `"movable": true` 授权该根的普通 rename，并声明整个初始子树都是具有唯一名称的普通非挂载目录。C++ 的 `DarwinFileOptions::MovableDirectories` 追加在原聚合成员之后；直接父目录必须可变。省略或 false 保持未知，其他 JSON 类型无效。后代保留各自的 mutable、removable、movable 和文件写入授权；此声明不授权预置目录 SWAP、权限判断或一般挂载语义。已知 flags、特殊目录权限、多链接普通文件和 stat/目录快照中的 inode 别名拒绝准入。声明连接的整个非挂载域只能有一个已知设备编号，包括没有 stat 的共同祖先下的兄弟目录及文件；设备相等本身不能连接其他域。

目录对象持有名称、父关系、原 stat/快照和授权。旧输入路径不会重新构造已移动或删除的名称。未打开的后代、FD/dup/CWD、映射及已删除后代都保留原对象；未改变的后代继续使用原 stat、快照、cookie 和 SEEK_END。移动根及发生名称变化的父目录使完整观察变为未知。旧名复用不继承原观察或授权。初始输入只声明一次同步执行的环境，不是运行期替换目录的 API。SWAP 支持独立保存：初始对象保留直接 swap_rename 声明，mkdir 从父对象复制能力，移动不重算；两侧 SWAP 父对象都必须已声明支持。

替换空的预置目标另需 removable 授权。事务在发布名称前检查所有路径不超过 1023 字节及共享 16 MiB 预算。每个 movable 引用固定预留原路径加 NUL，不增加条目。初始目录的路径、引用、快照和条目删除后仍固定预留；其动态 PathCharge 从零开始，移动时为所有已链接或保留成员的当前路径计费一次。替换只能抵扣可立即释放目标已有的动态费用；FD/CWD/子对象/孤儿映射保留不提供抵扣，最终回收只退一次动态费用。隐式初始祖先不增加 256 条目上限的计数，初始普通文件的条目回收行为不变。

例：

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/work","mutable":true,"movable":true}]}}
```

SDK 无关的 initial-directory-move 工作负载检查原目录移动、共享和独立游标、FD 标志、CWD、私有映射、替换及原名恢复；本地 guest 配置与物理 iOS 对照是独立验收。

这里的“初始目录 SWAP 不支持”指系统调用的源或目标根对象；交换创建出的祖先目录可以携带先前移动进去的初始后代，保留对象状态和动态费用。前文的初始目录限制适用于未提供所需声明的情况。

最终独立复审后的 ARM64 macOS 验证登记 1,264 项 Darwin 测试：796 通过、468 项因后端不可用跳过、零失败；117 项必需 ARM64 HVF 工作负载全部执行。文件服务子集 342/342，包含 22 个新增 4K/16K 行为实例及三项准入检查。CreationPolicy 沿用移动父对象原 Device/GID，旧名复用、inode 序列和当前 umask 互不混淆；精确 16 MiB 容量下的 16 次往返交换不累计收费，移回只释放实际的六字节差额。公共 C/CLI 175/175、报告解析 33/33、原生内核工作负载 29/29，独立原始探针有 19 项成功观察。Python 五种配置在 27.865 秒内通过，纯 API 71 项、SDK 漂移及 runner 49 项检查通过。计数有重叠；源码、二进制、失败尝试和最终结果保留在 build-hvf-arm64/initial-directory-move/ 并绑定提交。物理 iOS、暂停的 Intel HVF、初始根 SWAP、权限/挂载/大小写、共享映射 EOF、Mach/线程/dyld 及框架运行时仍是独立缺口。

## 显式初始目录根的原子交换

严格布尔值 exchangeable:true（C++ DarwinFileOptions::ExchangeableDirectories，追加在聚合末尾）仅授权显式非根初始目录对象作为 RENAME_SWAP 操作数；直接初始父目录必须可变。省略/false 保持不支持，其他类型无效。它与 movable 共用普通非挂载、唯一名称子树及 flags/特殊模式/别名/硬链接/整连接域设备准入，但并集只定义拓扑。两个声明各固定预留原路径加 NUL，同一根重复声明也分别收费，不增加条目；设备相等不连接其他域。

普通及 EXCL 初始源仍需 movable，普通初始替换目标仍需 removable。exchangeable 不授予这些权限、后代 mutable、文件写入或通用权限/挂载语义。不同对象交换时，两侧实际父对象须可变且分别支持 swap_rename；已授权普通组件同名 SWAP 在父授权/设备检查后无副作用，不首次计费，也不要求不同对象的交换能力。同对象 dot/大小写仍未知，缺失目标与 dot 的顺序不变。

支持两个初始非空根、初始/创建目录和目录/文件的两个方向。双方完整已链接及保留子树先预检，再全部摘取后发布；两根保持链接，不抵扣替换目标或文件内容，不分配 FD/inode/条目。原 FD/dup/CWD/游标、父对象、映射租约和授权保持归属；未改变后代保留 stat/快照，同名删除旧对象与新对象互不混淆。初始动态路径费用为零，首次交换计一次当前路径，后续替换旧动态费用；固定费用不退。路径或预算失败保留双方状态。

原始 SDK 无关 initial-directory-swap 工作负载交换预置 empty 目录与 data 文件并换回，检查子树、映射、CWD、游标、FD 标志、移动后创建和清理。前节 f98068c07 是独立冻结的普通移动验收；初始根 SWAP 仅由此声明扩展。

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/left","exchangeable":true},{"path":"/right","exchangeable":true}]}}
```


本轮在 macOS ARM64 Release 上核对 1,303 项 Darwin 注册：823 项通过、480 项因后端不可用跳过、零失败；120 项必需 ARM64 HVF 检查均已执行。文件检查 361/361（含 16 个新增 4K/16K 行为实例和 3 项准入检查），C/CLI 180/180、报告解析 34/34、原始内核程序 30/30，独立探针 35 项观测通过。Python 五种配置用时 33.894 秒，71 项纯 API、49 项清单/对照单元检查及 SDK 漂移、格式、能力、来源和文档检查通过。计数相互重叠。

精确容量下，同名操作与 16 次往返交换保留对象及费用；需要 6 字节而只余 5 字节时，双向拒绝均保留两棵树、游标和后续创建预算。独立计划与最终源码审查通过。原始尝试、源码、二进制和结果保存在 `build-hvf-arm64/initial-directory-swap/` 并绑定提交；误重叠的报告检查已排除并串行重跑，翻译标记已同步，未放宽时限或负控。权限、未声明挂载/大小写、共享映射/EOF、推进时钟、Mach/线程/dyld/框架仍有缺口；真实 iOS、暂停的 Intel HVF 和远端合并 CI 属于独立验收。

## 显式只读资源限制观察值

`getrlimit(194)` 在五种 Darwin guest 配置中读取调用方的 `DarwinSystemOptions::ResourceLimits`（`darwin_system.resource_limits`）。资源键为 0..8；`DarwinResourceLimit` 的 Current/Maximum 是偏移 0/8 的两个小端 uint64，共 16 字节。必须满足 `0 <= current <= maximum <= 9223372036854775807`；零是显式值，INT64_MAX 表示无穷。这些固定观察值不查询宿主，也不调整现有 FD、VM、存储或执行预算；`setrlimit`、限制执行、信号和调度尚未实现。

严格 JSON 最多接受九个唯一资源，每项只能含 `resource`、`current`、`maximum`，使用精确整数或无符号十进制字符串。重复、缺项、类型错误、非规范键和倒置值在加载镜像前失败。省略或空数组表示未知；配置键不应用系统调用的截断或标志归一化。

系统调用仅取选择器低 32 位并清除 `_RLIMIT_POSIX_FLAG=0x1000`。非法资源先返回 EINVAL，缺失观察值先停为不支持，二者均不访问输出。完全不可写返回 EFAULT；部分可写的单个结果保持全部字节并停为不支持。完整非对齐或跨页写入只改变 16 字节；后端错误仍是传输错误。原生 ARM64 探针通过 23 项值/选择器检查及四个独立故障检查，宿主的部分前缀未写入现象不能推广为通用保证。

```json
{"darwin_system":{"resource_limits":[{"resource":8,"current":256,"maximum":"9223372036854775807"}]}}
```

macOS ARM64 Release 的注册/通过/未执行跳过/失败为 1337 / 845 / 492 / 0，123 项必需 HVF 全部执行。新增十二项 4K/16K 行为测试、配置负控及 SDK 捕获对照；C/CLI 190/190、解析 37/37、原生负载 31/31，实际 Python 五配置耗时 71.978 秒，纯 API 71 项和运行器 49 项均通过。SDK 漂移、固定格式、能力、来源与文档检查通过；数量重叠。源码、二进制与原始结果封存于 `build-hvf-arm64/resource-limit-observations/` 并绑定提交；首次测试枚举名编译失败、接线尝试及遗漏 admission 的初始筛选均保留，修正后串行验收且未放宽期限或负控。资源执行、权限、变更后目录元数据/枚举、共享映射/EOF、推进时钟、Mach/线程/dyld 与框架仍有缺口；真机 iOS、暂停的 Intel HVF 和远端合并 CI 另行验收。

## 显式只读资源用量观察

五种 Darwin guest 配置的 `getrusage(117)` 读取两个独立可选快照 `DarwinSystemOptions::ResourceUsageSelf` / `ResourceUsageChildren`，严格 JSON 位于 `darwin_system.resource_usage.self` / `.children`。每个 `DarwinResourceUsage` 必须有 int64 的 `user_seconds`、`system_seconds`，小于 1000000 的 uint32 `user_microseconds`、`system_microseconds`，以及恰好十四个 int64 `counters`。精确整数或有符号十进制字符串保留完整范围；类型、字段、微秒和数组长度错误在加载前失败。未提供的另一个快照保持未知，不影响查询已提供的快照；全零是有效声明。

输出是一次完整的 144 字节小端复制。timeval 位于 0/16：8 字节秒、4 字节微秒、4 字节零填充；counter 从 32 起，每项 8 字节。保留 Darwin 原值和单位，ru_maxrss 不按 Linux KiB 换算。快照固定，不采样宿主性能，也不实现记账、fork/wait、调度或限制执行。

仅取选择子低 32 位：0 为 SELF，-1 为 CHILDREN；0x1000 无效，不清除 getrlimit 的 POSIX 标志。无效值在内存访问前 EINVAL，缺失值在输出访问前 unsupported。完整跨页/非对齐写入保留护栏；全不可写 EFAULT，部分可写在任何字节复制前 unsupported，后端错误保持传输错误。SDK 对照只捕获每个选择子一次，不能比较随后变化的 SELF。原生探针通过 13 项布局/选择子/值检查和 4 个独立故障进程，仍用五秒时限。本机 partial SELF 在 EFAULT 前写了 64 字节，证据保留，不推广为通用前缀保证。

0..13: `ru_maxrss`, `ru_ixrss`, `ru_idrss`, `ru_isrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`, `ru_inblock`, `ru_oublock`, `ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`, `ru_nvcsw`, `ru_nivcsw`.

```json
{"darwin_system":{"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":0,"system_microseconds":0,"counters":["9223372036854775807",0,0,0,0,0,0,0,0,0,0,0,0,0]}}}}
```

[Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html), [XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [SDK layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

macOS ARM64 Release 登记/通过/不可用跳过/失败为 1371/867/504/0，全部 126 项必需 HVF 执行；文件组件 361/361，C/CLI 200/200, JSON 40/40, System/SDK 53/53, native 32/32 通过。新增十二项 4K/16K 行为和准入/SDK 对照；Python 五配置 42.865 秒通过，API 71、runner/reference 49 及 SDK 漂移、格式、能力、来源、文档检查通过。计数重叠。独立源码复审通过。证据冻结在 `build-hvf-arm64/resource-usage-observations/` 并绑定提交。初版 fixture 错误要求 x64 EINVAL 清零 RDX，现检查 x64 保留 RDX、ARM64 清零 X1；失败源码/二进制及空过滤尝试均保留，运行时、时限和负控未放宽。资源执行、权限、修改后目录观察、共享 map/EOF、时钟、Mach/thread/dyld 和框架仍有缺口；物理 iOS、暂停的 Intel HVF、远程合并 CI 单独验收。

首轮完整验证为 866 通过、1 个既有 iOS ARM64 HVF rename 五秒超时、504 跳过；同一二进制复查该用例 386 毫秒通过，随后完整串行验证通过。原记录保留，超时原因未确定，不据此保证时延。


## 显式凭据、用户组与一致的创建属主

可选 DarwinSystemOptions::Credentials 含 RealUID、EffectiveUID、RealGID、EffectiveGID 及独立可选 GroupAccessList。省略凭据时四个查询维持1000；显式零/root 有效。ID 支持0..INT32_MAX。组列表必须1..16项，第一项等于 EffectiveGID，保留顺序与重复；缺少列表保持未知，不从宿主或 EGID 推断。严格 darwin_system.credentials 必须恰含 real_uid/effective_uid/real_gid/effective_gid，可另含 groups；无损整数及统一验证器在加载前拒绝形状、字段、范围、数量或首组不一致，非Darwin配置仍拒绝。

getuid24/geteuid25/getgid47/getegid43/getgroups79 共用一个系统所有者。新普通文件 UID 使用有效用户，device/GID 仍继承直接父目录；改名、保留FD与旧名重用保持对象身份，输入stat不被改写。root 不自动授权文件写入、目录修改、权限或ACL；setuid/setgid/setgroups、进程/会话和权限强制检查未实现。

getgroups 只将容量低32位按有符号int判断：负数先 EINVAL；未配置列表先 unsupported；已知容量0返回数量而不访问指针；正容量不足先 EINVAL；充足时一次复制4*数量小端字节并返回数量。0x1000 是正容量，不剥POSIX旗标。非对齐/跨页完整保护，完全不可写 EFAULT，部分可写在任何字节前 unsupported，backend错误仍为传输错误。BSD错误保留x64 RDX、清ARM64 X1，成功均清次结果；报告保留完整原始参数。


~~~json
{"darwin_system":{"credentials":{"real_uid":101,"effective_uid":202,
 "real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}}}
~~~


[Apple getgroups contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getgroups.2.html), [pinned XNU credential/group ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c).

完整注册/通过/不可用跳过/失败: 1413/897/516/0; ARM64 HVF 129/129. System/SDK72/72, File365/365, Report43/43, C/CLI210/210; guest8/12skip; native33/33; Python5 82.918s; API71, runner/reference49. SDK drift / clang-format22.1.2 / capabilities23 / provenance / docs11+negative3: OK. Independent source review: OK.

21 value +5 native fault,5s; host16groups, positive short capacity: OK. Native partial32byte thenEFAULT: observation only. Counts overlap. `build-hvf-arm64/credential-observations/`.

首轮8个可用来宾失败（5个指令预算、3个5秒期限），12跳过；新程序遍历整两页过重。现检查同一跨页输出周围完整132字节（64前置、最多64数据、至少4后置），直接测试仍覆盖整两页。预算、调用参数和故障负控未变，源码/二进制/两次结果均保留；直接传输测试跨页几何在执行前按独立审阅修正。公共参数选择改为字符串内容比较。权限/ACL、链接、修改后目录完整观察、shared maps/EOF、递进时钟、Mach/thread/dyld、framework仍未完成；实体iOS、暂停Intel HVF和远端merge CI单独验收。

## 由显式进程与内核限制决定的描述符表查询

可选 `DarwinSystemOptions::MaxFilesPerProcess` / `darwin_system.max_files_per_process` 声明非负 int 观测，缺省未知，显式零有效。名称 `kern.maxfilesperproc` 与数字 MIB `[1,29]` 独立读取同一个四字节值，不要求资源限制。无损整数解析与中央验证在加载前拒绝错误类型、负数和越界值；非 Darwin 配置明确拒绝。

BSD `getdtablesize(89)` 必须同时具有该上限与 `ResourceLimits[8].Current`，返回两者较小值。先截断完整 64 位 Current，再按 int 返回：Current=`0x100000001`、cap=64 得到 64，无穷限制也安全截断。Maximum、宿主值、当前 FD 数和 `DescriptorLimit` 不能替代观测。任一项缺失时，即使另一项为零也明确停止 unsupported。忽略全部六个实参，不访问用户内存，沿用 BSD carry/次级寄存器约定；Mach timebase 的 89 号陷入保持独立。

sysctl 沿用原复制阶段。显式 EUID0 的真实写请求在名称/MIB 与 oldlenp 预检后、观测和输出前停止 unsupported；非 root 返回 EPERM，新指针长度为零仍为读取。不会据此执行限制或授予写权限。现有必需资源 workload 核对两种 cap 查询及低位/高位 syscall number，输出仍为 `l` / 144 字节；后续 unsupported 保留已输出字节。独立标量 fixture 验证缺值、零、宽 Current 与 DescriptorLimit=3 的预算独立性。

```json
{"darwin_system":{"max_files_per_process":64,"resource_limits":[{"resource":8,"current":"4294967297","maximum":"9223372036854775807"}]}}
```

[XNU getdtablesize](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU proc_limitgetcur_nofile](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [XNU MIB constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/sysctl.h).

验证: Darwin (登记/通过/不可用/失败) 1441/913/528/0; ARM64 HVF 132/132; System/SDK 80/80; File 365/365; C/CLI 211/211; ProcessReport 45/45; native 33/33; Python 5 (42.254s); API 71; runner 44 + reference 5. clang-format 22.1.2; capabilities/provenance; docs 286 / locales 10 / negative controls 3.

尝试记录：首次 runner 检查在 ARM64 与 x86-64 清单子项失败，因为新标量方法缺少必需登记。补齐登记并保留全部方法相等规则。初始 129 必需项门禁及失败记录保留，重新执行的最终 ARM64 门禁要求 132 项。

计数相互重叠。新证据保留实际执行基线及精确源码、二进制、日志哈希，目录为 `build-hvf-arm64/descriptor-table-observations/`，旧证据保持不变。原生探针为五个一次性 ARM64 macOS 子进程、40 项检查，每进程五秒期限；默认 Current=1048575/cap=245760 返回 245760，子进程 Current=0/1/32/245777 返回 0/1/32/245760，未改变父进程或系统限制。iOS 真机、暂停的 Intel HVF 与远端 merge CI 分别验收。权限、变更后目录观测、共享映射/EOF、推进时钟、Mach/thread/dyld 与框架运行时仍有缺口。

## 显式进程观察值

`DarwinSystemOptions::ProcessGroupID`、`SessionID` 和 `ProcessTainted` 是相互独立的可选输入，对应 JSON `process_group_id`、`session_id`、`process_tainted`。两个 ID 必须为正数且不超过 INT32_MAX；污染状态只接受 JSON 布尔值 `true`/`false`。缺省仍表示未知，明确的 `false` 则是已知零值。不会从宿主机、PID1000、凭据或其他观察值推断这些字段。

原始 `getpgrp(81)` 读取进程组；`getpgid(151)`、`getsid(310)` 将参数解释为有符号低32位 `pid_t`，零或当前固定 PID1000 查询自身，例如 `0xffffffff000003e8` 仍表示自身。负的低32位 PID 在读取观察值之前返回 ESRCH3；这一结果由只读原生探针和 XNU 的进程分配、查找规则确认。未知的正数其他进程，包括 `0x1000`，以 UnsupportedService 停止，不猜测 ESRCH，也不把位解释为其他接口的标志。缺少所选自身观察值同样停止为不支持。`getpgrp`、`issetugid(327)` 忽略全部参数；四个标量查询均不访问来宾内存，并保留原有 BSD carry 和第二返回寄存器规则。

`process_tainted` 提供固定的 `P_SUGID` 观察值，与真实和有效 ID 相同或不同无关；它不改变 EUID、文件所有权、sysctl 写权限、授权、领导关系或终端状态。`setpgid`、`setsid` 和凭据修改仍不支持。原始只读 `process-observations` 工作负载捕获自身 PID，核对高位参数、负数错误和返回状态；`virtual-process-observations` 输出配置的进程组、会话和污染状态字节。其他进程、缺失值及设置器的模型测试不进入原生执行目录。macOS 原生参照不能证明物理 iOS 设备兼容性。

```json
{"darwin_system":{"process_group_id":7,"session_id":16909060,"process_tainted":false}}
```

[XNU process queries](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c), [XNU service numbers](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/syscalls.master), [XNU PID allocation](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_fork.c), [XNU PID lookup](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_proc.c).


## 显式会话登录缓冲区

`DarwinSystemOptions::LoginNameBytes` 独立声明会话的全部255个原始字节（`MAXLOGNAME`）。JSON `login_name_hex` 必须包含恰好510个 ASCII 十六进制字符，允许大小写。嵌入NUL以及终止符后的非零字节均有效。缺省仍为未知，显式全零记录则是已知值。模型不会补齐短名称，也不会从宿主机、凭据、进程组、会话ID或污染状态推断字节；该观察值不授予登录或文件权限。

原始 `getlogin(49)` 将长度按无符号低32位 `u_int` 解释，恰好复制 min(length,255) 个字节，不解码字符串、不补NUL，也不输出所需大小。零长度无需观察值或客体内存访问，坏指针也成功；`0xffffffff00000000` 因而选择零长度。非零请求先要求完整观察值，再检查目标。完全不可写返回 EFAULT14；部分可写范围在复制前明确停止，预检错误不发布字节。后端写错误由原有用户复制层直接传播，不承诺任意后端的通用回滚。保留 BSD carry 和第二返回寄存器规则，包括 x64 错误时的原始 RDX。

`setlogin(50)` 在显式root凭据或全零缓冲区下仍不支持。原始只读 `login-buffer` 工作负载检查完整长度参数、前缀、相邻字节、零长度指针和EFAULT；`virtual-login-buffer` 输出声明的255字节。缺失值及设置器的模型测试不进入原生执行目录。macOS ARM64参照不能证明物理iOS或Intel原生兼容性。示例明确声明全部255个零字节，并非推断出的空登录名。

客体验证器在每次复制前仍初始化全部265个输出字节，再按升序检查三个不相交范围：[0,3)、[3,3+n)、[3+n,265)，n 沿用原有截断后的复制长度。首尾范围检查全部保护字节，中间逐字节比较原始快照。12个完整长度参数、21次零长度调用、42次EFAULT、carry/第二寄存器检查及原始/虚拟路径均保留，并沿用现有执行限额。此改动减少验证程序自身的工作量，保持全部字节覆盖和首次错误顺序；生产运行时性能仍需独立测量。

```json
{"darwin_system":{"login_name_hex":"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}}
```

[XNU getlogin / MAXLOGNAME](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c#L1561).


## 显式当前进程优先级

`DarwinSystemOptions::ProcessNice` / JSON `darwin_system.nice` 独立声明 -20 到 20 的固定有符号 nice 值。缺省表示未知，显式零和 -1 都是已知值。复用无损整数解析，接受整数数值和十进制整数字符串；错误类型、小数、指数字符串、空白和越界值在加载映像前拒绝，精确整数的数值 JSON 仍有效。非 Darwin 配置不接受该字段。不会从宿主机、凭据、进程组、会话、污染、登录、资源或 CPU 观察值推断 nice，也不改变调度、权限或执行预算。

原始 `getpriority(100)` 使用选择器 int 和无符号目标 `id_t` 的低32位。目标超过 INT32_MAX 首先返回 EINVAL22；未知选择器（包括 GPU5、0x1000）及线程选择器3的非零低32位目标也在读取观察值前返回 EINVAL。`PRIO_PROCESS`0 只接受零或当前 PID1000，选中自身后才要求 nice。其他正数 PID 保持未支持，不猜测 ESRCH；组1、用户2、线程3/目标0及扩展选择器4、6、7、8 即使相关观察值齐全也保持未支持。线程目标仅高位非零仍选择未知线程状态，不能误报 EINVAL。结果符号扩展到完整64位：-1 是 carry 清除的成功 UINT64_MAX。查询不访问来宾内存，忽略未用参数；保留 BSD 第二返回寄存器规则，x64 错误保留 RDX、成功清零，ARM64 两种路径均清零 X1。

`setpriority(96)` 在显式 root 和 nice 下仍不支持。原始只读 `process-priority` 检查自身参数载体、确定非法的参数及成功/错误/成功转换；`virtual-process-priority` 输出配置的八个有符号字节。其他进程、聚合、缺失和设置器路径只在模型中测试。新 ARM64 macOS 探针实际通过191项，nice样本为0；非负样本不能证明负数硬件扩展，负数依据固定版本的有符号入口声明及独立模型边界验证。物理 iOS 和 Intel 原生验收仍须分别完成。

```json
{"darwin_system":{"nice":-1}}
```

[XNU getpriority](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_resource.c), [XNU signed INT entry](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/dev/arm/systemcalls.c).


## 禁止跟随标志与目录预检

普通及 nocancel `open` / `openat` 接受低32位无符号 O_NOFOLLOW=0x100 或 O_NOFOLLOW_ANY=0x20000000。这两个查找标志不会出现在 F_GETFL，也不改变访问、追加、截断、创建或 FD 独立的 CLOEXEC 行为。二者同时设置时，先检查 FD 容量，再在完整路径导入前返回 EINVAL22；满表先返回 EMFILE24。未知标志仍明确不支持。

非 AT_FDCWD 的 `openat` 在访问模式和 FD 容量检查前只导入路径首字节。首字节不可读返回 EFAULT14；相对前缀（含 NUL）先验证 FD 持有的目录对象：未知 FD 返回 EBADF9，普通文件返回 ENOTDIR20，未知流 vnode 类型仍不支持。首字节 `/` 跳过 dirfd 验证，之后才按原有 open 顺序导入完整路径。普通 `open` 与 AT_FDCWD 不执行该预检，其他 nameiat 路径共用首字节/相对 FD 预检；AT_FDONLY 绕过路径。拒绝操作不占用 FD 或新 inode，内存传输错误原样传播，不发布命名空间变更。

原始 `file-access` 使用相对目录 FD 检查 NOFOLLOW_ANY，避免宿主 `/var`、`/tmp` 的链接别名。直接测试区分首字节与后续字节故障、斜杠/NUL、用户地址及页边界、满表和已删除目录对象。独立 ARM64 macOS 原始探针在不变的五秒期限内通过30项；最后一项满表绝对路径实际使用有效目录 FD，保留原标签但不扩大其证据范围。物理 iOS 和 Intel 原生验收仍须分别完成。

[XNU open1at / open1](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU vn_open_auth](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_vnops.c), [XNU open flags / FMASK](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/sys/fcntl.h).

## 固定初始符号链接

下述初始名称默认受保护；显式授权后的行为见文末初始链接可变授权一节。 C++ `DarwinFileOptions::SymbolicLinks` 与 JSON `darwin_files.symbolic_links` 声明固定链接。条目要求规范绝对 `path` 和原始十六进制 `target_hex`，可选 `metadata` 描述链接自身。目标为1..1023个非 NUL 字节，保留非 UTF-8、重复斜杠与点，可悬空。`files` 仍必填，可为空数组。链接名称不能与文件/目录冲突或成为其他已声明名称的祖先；路径/NUL与目标共同计入256条目、16 MiB限额。元数据要求 S_IFLNK、size等于目标长度；完整目录快照须含 DT_LNK=10 和一致 inode。配置 CWD 必须为实际目录。

统一文件解析器先展开链接再处理点。相对目标从实际包含目录开始，绝对目标从客体根开始；每次重新解析尾斜杠，已消耗的输入斜杠不会变成目标斜杠。最多允许32次展开，第33次返回 ELOOP62；目标、剩余后缀和 NUL 总计超过1024字节返回 ENAMETOOLONG63。描述符、CWD、F_GETPATH、mmap 持有最终目标对象。

stat64、普通 open/access/truncate/chdir 跟随末端链接；lstat64/readlink 保留末端链接。O_NOFOLLOW 保留后返回 ELOOP，叠加 O_DIRECTORY 则先返回 ENOTDIR20。O_NOFOLLOW_ANY 拒绝所需展开；O_CREAT|O_EXCL 对已有末端链接返回 EEXIST17，包括悬空和循环链接。AT0x20 保留末端，AT0x800 同时保留末端并拒绝中间/尾斜杠所需展开，二者可组合。AT_FDONLY 在标志校验后仍忽略路径。

readlink(58) 使用有符号低32位 count；readlinkat(473) 保留完整 size_t。两者返回 int，超过 INT32_MAX 先于路径/FD返回 EINVAL22。仅复制 min(count,目标长度) 字节，不补 NUL，只预检实际前缀。零长度仍验证路径和链接类型，随后忽略输出指针；非链接 EINVAL22，全不可写 EFAULT14，部分可写在复制前明确停止，传输与内存预算错误原样传播。

固定链接名称与原始目标字节保持不变。MutableDirectories 不能为根或任何固定链接名称的路径分段祖先；/work 不包含 /workspace/link。独立可变目录可容纳链接目标，包括运行期间创建、移动、删除和替换的名称。现有父目录、挂载、别名、标志、交换授权与创建策略校验仍适用；新建 inode 必须大于所有元数据/快照 inode，包括受保护链接。 固定名称 WritableFiles/MutationPolicies 仍可改变最终普通文件。保留链接的 unlink/rename 在效果前明确停止。运行时链接创建见下节；硬链接、ACL权限及未获单独授权的初始链接修改仍不支持。独立 ARM64 macOS 探针在原五秒期限内通过189观察、115完整缓冲检查；这不能单独证明物理 iOS、Intel HVF 或完整 OS兼容。

新增 60 项 ARM64 macOS DELETE/RENAME 原生矩阵在原五秒期限内记录完整 stat 缓冲、变更前后命名空间及保留 FD/CWD 身份。尾部斜杠可展开固定链接并改变实际目标；NOFOLLOW_ANY 拒绝必要展开并返回 ELOOP。无 SDK 的 symbolic-link-mutations 程序还检查创建、悬空目标、改名/删除/替换、保留 CWD 父对象，以及关闭描述符前全部原始 10 个文件字节和关闭后仍保留的全部 10 个映射字节。这些宿主观察不证明物理 iOS 或原生 Intel 覆盖。

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"symbolic_links":[{"path":"/link","target_hex":"64617461"}],"working_directory":"/"}}
```

[XNU namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c), [XNU readlink / AT](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU open authorization](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_subr.c).

## 运行时创建符号链接

原始 `symlink(57)` 和 `symlinkat(474)` 可在获准修改的目录内创建进程本地链接，返回 int；目录 FD 仅取低32位，绝对目标名称忽略 FD。先导入链接目标，再检查目标名称及 FD；首个 NUL 截断0..1023个原始字节，允许空、非 UTF-8、点和重复斜杠。1024字节无 NUL 返回 ENAMETOOLONG63，之前不可读的字节返回 EFAULT14。初始 JSON 链接仍要求1..1023字节。

当前链接表持有实际名称、父对象和原始字节。已有末端名称返回 EEXIST17；悬空链接后已消耗的尾斜杠可使创建发生在其目标名称上，旧链接不变。展开空目标返回 ENOENT2；readlink 读取空目标返回0，即使容量为正也不访问输出指针，但仍先检查计数、路径及类型。

名称/NUL和目标字节仅计费一次，共用256条目、16 MiB配额。拒绝不会发布节点、改变父目录或消耗 FD/普通文件 inode。新链接完整元数据明确未知，不能从普通文件 CreationPolicy 或被复用名称的旧观察推导；成功创建使父目录 stat/快照未知。目标删除或替换后，旧 FD/CWD/映射仍持有原对象。rmdir 和目录替换会检测链接子项；移动或 SWAP 的任一侧包含受保护初始链接时在效果前停止。链接与目录的替换、硬链接、ACL权限及未获单独授权的初始链接修改仍未支持；别名不会转移实际父目录的授权。

原生 ARM64 macOS 的150项记录保留了四项观察器失败；独立10项补充只验证实际新目标和空链接边界，不改写旧结果。无 SDK 的 `symbolic-link-creation` 程序检查两个入口、原始字节与缓冲边界、父对象、替换文件，以及旧 FD/映射的全部10字节；这些原生参考不证明物理 iOS、原生 Intel 或完整 OS兼容。

[XNU symlink / symlinkat](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU empty-link expansion](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c).

## 删除运行时创建的符号链接

`unlink(10)` 和 `unlinkat(472)` 可删除实际父目录已授权修改的运行时链接；初始固定链接仍受保护。裸 `AT_SYMLINK_NOFOLLOW_ANY` 保留末端链接，允许删除悬空、循环和空目标链接；中间或尾斜杠要求展开时返回 ELOOP62。不带该标志时统一解析器决定实际节点：`a → b → target` 中删除 `a/` 会删除 `b`，保留 `a` 和最终目标。现有标志、路径及 dirfd 错误顺序不变。

成功删除立即退还一个动态条目及其当前路径/NUL/原始目标字节费用，只使实际父目录的完整 stat/枚举观察失效，不消耗 FD 或创建 inode。目标字节、修改策略、旧描述符及共享游标、CWD、映射租约仍归原对象；名称复用不会恢复旧链接。拒绝不改变模型状态或退款。运行时创建链接的完整元数据仍未知。链接与目录的替换，以及仍含受保护初始链接的目录子树移动/SWAP 继续不支持。

独立 ARM64 macOS 原生参考在原五秒期限内记录40例：28次成功删除、12次原生错误，以及17次重复删除 ENOENT，并检查保留的 FD/CWD/私有映射。这不是客体、物理 iOS 或原生 Intel 验收；无 SDK 的 `symbolic-link-unlink` 和公开 API 用例另行验证相同边界。

## 重命名运行时创建的符号链接

`rename(128)`、`renameat(465)` 和 `renameatx_np(488)` 支持普通改名与 `RENAME_EXCL=4`：运行时链接移到空名称、链接替换链接、链接替换普通文件，以及普通文件替换链接。两个实际父目录必须在已证明的同一挂载域内授权修改；初始固定链接仍不可变。统一解析器选择实际节点，空、悬空、循环和非 UTF-8 目标字节均不改写；跨父目录移动后，相对目标从新父目录解析。裸 `RENAME_NOFOLLOW_ANY=16` 保留末端链接，中间路径必须展开时返回 ELOOP62。EXCL 对不同的已有目标返回 EEXIST17；同对象 EXCL 在缺少文件系统大小写属性时明确不支持。

事务在发布名称前预留新路径/NUL费用，包含 NUL 最多1024字节。被替换运行时链接的当前路径/NUL/原始目标费用立即且仅退还一次，与目标文件的 FD 或映射无关。被替换普通文件的内容/动态路径费用由旧描述符和映射租约继续持有，全部释放才回收；仅可立即回收的普通目标提供预留额度。改名不需要额外条目、FD 或普通文件创建 inode。拒绝保留两个节点，成功使两个实际父目录的完整 stat/枚举观察失效；运行时链接完整元数据仍未知。链接与目录的替换、硬链接，以及仍含受保护初始链接的目录子树移动/SWAP 继续不支持。

独立 ARM64 macOS 的19项原生对照在原五秒期限内记录14次成功、5次 EEXIST/ELOOP 错误，并核对链接 inode/原始字节、相对目标重新绑定及旧 FD/dup/游标/CWD/私有映射。无 SDK 的 `symbolic-link-rename` 和公开 SDK/CLI 用例分别检查这些调用；原生参考本身不能证明物理 iOS、原生 Intel 或完整 OS 兼容。

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 交换运行时创建的符号链接

`renameatx_np(488)` 的 `RENAME_SWAP=2` 支持运行时链接/链接、链接/普通文件及普通文件/链接的叶节点交换。两个实际父目录必须在已证明的同一挂载域授权修改及 SWAP；完全相同名称的链接交换无需额外 SWAP 声明即可无效果成功。目标缺失返回 ENOENT2；`SWAP|NOFOLLOW_ANY=18` 保留末端链接，必要的中间展开返回 ELOOP62；`SWAP|EXCL=6` 在读取路径前返回 EINVAL22。原始目标字节不变，相对目标分别从两个新父目录解析。

事务先预留两个路径/NUL费用，再发布名称。两个节点一直保持链接，内容和映射租约不提供替换退款；初始普通文件首次交换取得动态路径费用，交换回来继续复用。无需条目、FD 或创建 inode。文件身份、nlink、旧描述符/共享游标、CWD 和映射保留原对象，实际父目录完整观察变为未知。运行时链接完整元数据、实际目录/链接组合，以及含受保护初始链接的子树移动/SWAP 仍不支持。独立 ARM64 macOS 的22项原生对照在原五秒期限内记录14次交换、2次同对象成功及6次错误。扩展 `symbolic-link-rename` 和 C++/SDK/CLI/Python 检查实际 SWAP 标志及错误；这些参考不证明物理 iOS、原生 Intel 或完整 OS 兼容。

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 移动包含运行时符号链接的目录树

普通/EXCL 目录移动及 SWAP 现已将运行时链接子项纳入已有目录事务，也支持目录与普通文件交换。事务先检查所有目录、文件和链接的新名称、共享命名空间冲突及含 NUL 的1024字节路径上限，再完整抽出三个表的节点并发布新键。目标字节持续计费且不改写，只更新当前路径/NUL费用；SWAP不提供替换退款，不消耗额外条目、FD或创建 inode。

链接保留实际父目录对象，父目录路径移动后，相对目标按新路径解析；旧描述符、共享游标、CWD、已删除节点及映射租约继续保有原对象。受保护初始链接及所在树仍不可变，根节点目录/链接组合、运行时链接完整元数据、硬链接及 ACL 仍不支持。独立 ARM64 macOS 的25项对照在原五秒期限内记录5次移动、10次交换、2次同对象成功及8次保留命名空间的拒绝；跨父目录案例检查原文不变和相对目标重新绑定。扩展 `symbolic-link-rename` 覆盖 C++/SDK/CLI/Python，这些观察不证明物理 iOS、原生 Intel 或完整 OS兼容。

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 新建链接和目录的元数据

可选 `creation_policy.namespace_policy`（C++ `DarwinFileCreationPolicy::Namespace`）在原有五个必需字段之外增加严格对象，仅含 `symbolic_link_allocation_unit`、`directory_entry_size`、`directory_blocks`。链接分配单位为512至16 MiB的2次幂；目录条目大小为正数且不超过16 MiB；块数为不超过INT64_MAX的uint64，可用十进制字符串。父目录元数据、初始umask和新inode条件保持原契约。省略扩展时，前文链接/目录元数据未知、仅普通文件消耗创建inode的行为仍是默认。

启用后，普通文件、symlink和mkdir成功插入共用一条inode序列，UINT64_MAX永久耗尽；失败和打开已有名称不消耗inode。Device/GID来自实际父对象，UID来自有效来宾身份。链接模式为S_IFLNK加 `0777 & ~umask`，nlink=1，size为原始目标字节数（含空目标和非UTF-8），blocks按声明单位向上取整后以512字节计。目录模式为S_IFDIR加 `mode & 0777 & ~umask`，nlink为2加所有仍在命名空间内的直接子项，size为nlink乘声明条目大小，blocks固定；持有的已删除空目录也保留此规则。这是显式虚拟策略，不推断APFS分配行为。

初始时间用creation_time；子名称成功变化更新新建父目录mtime/ctime，直接移动链接/目录仅用mutation_time更新其ctime；移动祖先保留后代元数据。完整记录随对象经过dup、CWD、替换、SWAP、删除和名称重用。仅创建扩展不会保留初始父目录完整 stat 或固定快照；下文独立目录策略提供 stat 与实时枚举依据。ACL、未获单独授权的初始链接修改和一般目录/链接根事务仍未支持。原生 `created-namespace-metadata` 检查模式、所有者、身份和生命周期；`virtual-created-namespace-metadata` 经C++/SDK/CLI/Python对照目录与链接的完整144字节常量记录。定向验证通过11项模型/准入、1项严格JSON、43项原生工作负载（仍限5秒）、8项可执行客体组合（12项后端不可用跳过，3项必需HVF均已执行）及10项公共入口。指针表导致的客体失败和修正后的ARM64静态封装证据已保留；原生Intel和实体iOS尚未验证。

```json
{"namespace_policy":{"symbolic_link_allocation_unit":512,"directory_entry_size":32,"directory_blocks":7}}
```

## 命名空间修改后的虚拟目录枚举

可选 `directories[].enumeration_policy` 允许 `getdirentries64` 枚举当前名称；C++ 使用 `DarwinFileOptions::DirectoryEnumerationPolicies` 与 `DarwinDirectoryEnumerationPolicy`。严格对象必须恰好包含 `minimum_buffer_size`、`initial_minimum_buffer_size` 和 `seek_offset`：前者为正，两种最小值均不超过128 MiB，seek_offset 为无损 uint64。初始目录必须提供自身非零 inode 元数据，且不可同时声明不可变 `contents`；策略引用独立计入一次路径加 NUL 的费用。mkdir 继承实际父对象的策略，已有后代保留自身声明。枚举策略与修改授权独立。

虚拟顺序明确为 `.`、`..`，再按无符号字节排序的直接链接名称。inode 来自已观察或进程创建的对象，`..` 沿实际保留父对象；任何子项或父对象身份缺失/为零，都在输出前停止。初始完整 stat 失效后仅保留 inode 身份，不复用其他过期字段。游标为从1开始的局部序号，d_seekoff 使用声明常量；dup 共享游标，独立 open 分开。成功修改成员或直接移动目录后，非零旧游标须先回绕到零；拒绝和同对象无操作不失效，祖先移动保留后代游标。版本耗尽明确停止，不回绕复用。保留的已删除空目录输出零条记录，名称复用也不会接管旧对象。

本段描述仅配置枚举策略、尚未配置下文初始目录 stat 策略的行为。 沿用既有记录编码、完整记录装包、缓冲下限、负载上限、EOF 后缀和数据/游标/位置/标志的有序效果。初始父目录完整 stat 与不可变快照仍失效；缺少新策略时保留原有不支持行为，虚拟顺序和游标不模拟 APFS 世代。独立 ARM64 原生准备在原五秒期限内记录25个事件、16次枚举。`directory-enumeration-mutations` 核对原生名称/类型/inode 与保留对象；`virtual-directory-enumeration` 经客体、C/CLI 和 Python 核对160字节固定视图。Intel 原生、iOS 真机、ACL 执行、硬链接与完整系统/框架仍未验证或不支持。

## 初始目录的显式 stat 修改策略

可选 `directories[].mutation_policy` 让已准入的初始目录在名称空间变化后保留完整 stat。C++ 使用 `DarwinDirectoryMutationPolicy` 与 `DarwinFileOptions::DirectoryMutationPolicies`。严格对象恰含 `directory_entry_size` 和 `mutation_time`：条目大小为正且不超过 16 MiB，时间采用无损有符号 64 位秒数及 [0,1000000000) 范围纳秒。目录须提供自身完整 metadata 和非零 inode。策略引用计入一次路径加 NUL 的费用；投影 size 不分配文件字节。策略不授予名称空间操作或权限，也不要求创建或枚举策略。

首次真正提交修改前，完整观察记录保持原样；首次修改把标量字段复制到目录对象，无需分配。子名称变化令 nlink 为 2 加所有类型的直接关联名称数，size 为 nlink 乘 directory_entry_size，mtime/ctime 为 mutation_time。直接移动、SWAP 或删除仅改 ctime；移动祖先保留后代记录。拒绝和同一对象无操作均不修改记录。Device、inode、mode、所有者、blocks、块大小、flags、generation、atime 和 birthtime 保持观察值。这是显式虚拟规则，不推断 APFS 分配、链接数或时钟。

记录与策略随原对象经过 dup、保留 FD、CWD、替换、删除和名称复用。新 mkdir 对象使用独立创建策略（若提供），不继承父目录或同名旧对象的初始目录 stat 策略。不可变目录快照在修改后仍未知；独立 enumeration_policy 可提供实时视图。省略本 stat 策略时，已修改初始目录的完整元数据仍未知。

独立 ARM64 原生准备在不变的 5 秒期限内保留 27 个带保护的原始 stat 视图和 15 项操作，核对 144 字节 SDK ABI 及保留身份，不推广原生时间戳或分配规律。原创 `initial-directory-metadata` 检查原生共同观察；`virtual-initial-directory-metadata` 经 guest、C/CLI 和 Python 对照完整 144 字节常量记录。模型另覆盖首次删除、缺少修改授权、省略创建策略及快照/枚举独立性。原生 Intel、实体 iOS、ACL 执行、硬链接、未获单独授权的初始链接修改及完整 OS/框架仍未验证或未支持。

```json
{"mutation_policy":{"directory_entry_size":17,"mutation_time":{"seconds":-11,"nanoseconds":321}}}
```

## 初始符号链接的显式可变授权

`symbolic_links[].mutable:true`（C++ `DarwinFileOptions::MutableSymbolicLinks`）为原始链接对象授予命名空间修改权限，实际父目录仍须单独可变。已知 flags、特殊 mode 位、非一的 link_count、身份别名或父设备冲突会拒绝输入。省略授权时名称保持受保护，不能位于可变祖先域内。每项授权固定预留路径/NUL 引用，不增加条目，也不消耗创建 inode。初始原始目标始终不可修改。

现有 unlink、普通/EXCL rename、叶链接/文件/link SWAP 和已授权目录子树事务保持实际父对象、mount 和 SWAP 条件。目标字节不变；相对查找在新实际父目录重新解析。目标 FD/dup 游标、CWD 和映射租约保留各自对象。初始名称、目标及引用成本在替换或删除后仍固定预留；首次改名须另预留动态名称。替换与 unlink 仅退还对象拥有的动态名称/新建目标成本；只有新建链接增加动态条目。拒绝与同对象空操作不提交变化。

可选 `symbolic_links[].mutation_policy` 对应 `DarwinSymbolicLinkMutationPolicy` 和 `DarwinFileOptions::SymbolicLinkMutationPolicies`，严格只有 `mutation_time`：秒为无损有符号 64 位，纳秒范围 [0, 1000000000)。它要求上述授权和完整、非零 inode 的观测元数据。首次直接移动/SWAP 无分配复制标量，仅把 ctime 设为固定时间，其余字段（包括 blocks）保留观测值；祖先移动、拒绝、空操作保留完整记录。没有策略时，直接移动使完整 stat 未知，但实时枚举保留 inode，已知设备冲突仍拒绝。策略固定预留一次路径/NUL 引用。复用名称和新 symlink 对象不继承旧记录或策略，创建使用独立 namespace policy。

原生 ARM64 私有准备保留 14 个带守卫 raw-stat 视图、11 个操作及独立 144 字节 SDK ABI，维持 compile120s/native5s/drain1s/reap1s 并记录私有清理。`mutable-initial-links` 检查原生身份、相对重绑定和已持有目标；`virtual-mutable-initial-links` 在五种 guest、C/CLI、Python 检查完整 stat 字节。模型覆盖两种页大小、子树 SWAP、固定/动态成本、条目/inode 耗尽和枚举独立性。Intel 与实体 iOS 尚未原生验证；硬链接、ACL、完整 OS/runtime/framework 仍有缺口。

```json
{"mutable":true,"mutation_policy":{"mutation_time":{"seconds":-13,"nanoseconds":456}}}
```

## 目录与符号链接根事务

`renameatx_np(RENAME_SWAP)` 支持实际目录与符号链接双向交换，包括非空子树。初始目录须有 `exchangeable`，初始链接须有 `mutable`；双方实际父目录仍须在已确定的同一 mount 中分别授予修改/SWAP 权限。受保护的初始后代仍拒绝。在既有普通移动授权下，目录到链接返回 ENOTDIR20，反向返回 EISDIR21，EXCL 对不同的既有名称返回 EEXIST17；目录与自身后代链接的两个交换方向均在修改前返回 EINVAL22。交换的是链接本身，成功后可能形成自引用，后续跟随返回 ELOOP62。

沿用三张名称表的事务，在发布前检查根、关联或持有的后代、完整路径及动态空间。初始名称/目标/引用、文件字节和映射租约不能抵扣 SWAP；初始名称首次重键单独计入动态路径/NUL，重复交换只替换一次旧费用。不消耗新条目、FD 或创建 inode。成员关系依据实际父对象，同名的旧已删除对象不能归入新树。原始目标字节不变，相对解析绑定新父目录；FD/dup 游标、CWD、目标对象和孤立映射继续有效。

直接目录/链接根按各自既有 stat 策略更新 ctime，祖先移动保留后代记录。省略策略时完整 stat 仍未知，已知 inode 可用于实时枚举；已提交的父名称变化和直接目录移动按原契约使相关枚举版本失效，须归零重读。不增加 JSON 字段或权限。

原创 ARM64 私有准备保留 36 个带保护的 144 字节 stat 视图、16 次原始 rename，并维持 compile120s/native5s/drain1s/reap1s、进程回收和私有清理。`directory-link-roots` 与 `virtual-directory-link-roots` 经五种 guest、C/CLI、Python 检查共同原生身份和完整常量 stat；两种页大小的模型覆盖精确预算、未打开后代溢出、旧孤立对象、缺失授权及 FD/条目/inode 耗尽。本节在这些授权范围内扩展前文目录/链接限制。Intel 原生、实体 iOS、硬链接、ACL 和完整 OS/runtime/framework 仍有验证或实现缺口。

## 固定内核 pathconf 查询

原始 pathconf(191) 和 fpathconf(192) 支持 XNU 固定的 vnode 查询：选择器15/16/17 返回1，19/25 返回0，20/22/23 返回4096，21 返回65536，24 返回255。这些值对应符号链接支持、最小分配声明、异步/优先/同步 I/O 声明、传输建议和符号链接上限。查询结果不会开启异步执行或文件系统授权，也不从来宾页大小、分配策略或目录预算推导。

先导入路径并完成跟随解析，再处理选择器；保留 CWD、符号链接和 EFAULT/ENOENT/ENOTDIR/ELOOP 顺序。fpathconf 先按低32位查找 FD；未知或关闭的 FD 先返回 EBADF。已知普通文件/目录对象不依赖完整 stat，跨 dup、重命名、删除和名称复用保留查询。输入及捕获描述符的原生类型未知，明确停止。选择器使用低32位 int 和现有 BSD 返回约定；查询不复制输出、不改变游标、元数据或枚举，也不消耗条目、FD 或 inode。NAME_MAX、大小写敏感性及其他文件系统相关或未知选择器在解析后仍明确不支持，不借用宿主值或猜测 EINVAL。

kernel-pathconf、kernel-pathconf-values 和 kernel-pathconf-unsupported 分别验证共同语义、独立80字节字面值及保留既有输出的停止报告，覆盖来宾/C/CLI/Python。独立原生 ARM64 准备检查250次查询，其中249次对照 SDK；0.262秒完成，5秒期限不变。原生 Intel、真实 iOS、ACL、硬链接及完整运行时/框架仍未验证或未实现。

[XNU vn_pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU fpathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_descrip.c).

`pathconf`: `file/` → ENOTDIR; `alias/`, `alias//` → 1 (selector15); `alias/.`, `alias/child` → ENOTDIR.

## 固定公共属性列表

getattrlist(220)、fgetattrlist(228)、getattrlistat(476) 查询显式目录中的十一项固定公共属性：设备、对象类型、四种时间、所有者/组、完整模式、标志和文件 ID。它们与 stat64 共享完整元数据的有效性判断；缺失或失效的观察保持未知。对象类型和空选择无需 stat。根/挂载 NAME、卷、目录/文件/fork 专用属性、ACL 和未知选项明确不支持，即使请求返回属性掩码也不省略未知信息；不会推断宿主元数据或挂载名称。

路径/at 入口先导入 24 字节请求，FD 入口先检查 low32 FD 和原生类型，忽略 reserved 字。共享 CWD、相对 FD 和链接解析，在长度/位图检查前保留原生错误。记录采用小端、四字节对齐、完整 st_mode 和有符号秒值；含返回掩码为 120 字节，对应普通记录为 100 字节。短缓冲区只接收声明的前缀，长度仍报告完整所需大小。部分可访问的复制在该次复制前停止；超过有符号 uio 范围的大小只在有效且受支持的请求后返回 EINVAL。查询不改变游标、元数据、枚举或条目/FD/inode 预算，持有对象沿用 dup、删除和名称重用的生命周期。

三个模式通过原生公共行为、独立配置字节和保留既有输出的未知 ATTR_CMN_EXTENDED_SECURITY 查询，覆盖 guest、C、CLI、Python。ARM64 私有准备通过 187 次原始查询和 176 次路径/FD SDK 对照，原生 5 秒限制保持不变；SDK15.5 没有 getattrlistat 声明，raw476 对照单独记录。新增 guest/Python 保持 5,000,000us/quantum1024，已有公开测试保持 10s。原生 Intel、真实 iOS、文件系统专用事实、硬链接、权限/ACL 执行和完整运行时/框架仍未验证或未完成。

```text
ATTR_CMN_RETURNED_ATTRS=0x80000000
FSOPT_NOFOLLOW=1, FSOPT_REPORT_FULLSIZE=4
FSOPT_PACK_INVAL_ATTRS=8 (requires ATTR_CMN_RETURNED_ATTRS)
FSOPT_NOFOLLOW_ANY=0x800
common-attributes
common-attributes-values
common-attributes-unsupported
```

[XNU attrlist](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_attrlist.c), [XNU attr.h](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h).


## 显式扩展属性读取

getxattr(234)、fgetxattr(235)、listxattr(240)、flistxattr(241) 读取 darwin_files 中完整、有序的普通属性观察值。文件、目录、链接均可提供 extended_attributes 数组，严格使用 {name,bytes_hex}。省略表示未知，[] 表示已知空列表。名称为 1..127 字节 UTF-8，可含斜线；值是不透明字节。重复名称被拒绝，声明顺序保留。最多 4096 个属性；名称加 NUL 与值计入原有 16 MiB 预算，已有对象路径不重复收费。

属性随对象经历 dup、移动、删除、CWD 和名称复用，独立于完整 stat 与目录枚举有效性。新对象未知；内容写入、成功截断及不明确的非空复制失败使属性未知，复制前拒绝部分缓冲区则保留观察值。查询不改变偏移、输入、元数据或条目/FD/inode 预算。

ABI 使用低32位 FD/options/position、完整64位 size 和 BSD user_ssize_t/carry/secondary。NULL 查询忽略 position；非空值的路径接口在非NULL size0 时返回 ERANGE，FD size0 查询长度。只有路径 get 的 UINT32_MAX/UINT64_MAX 为兼容长度查询；FD get 限制到 INT32_MAX。短的正长度列表缓冲区可写入完整名称前缀再返回 ERANGE；非空列表的非NULL负64位长度返回 ERANGE。没有取得原生空列表证据，因此声明为空的负长度列表保持 UnsupportedService。输出不可完整写入时在复制前停止。NOFOLLOW1 与 NOFOLLOW_ANY64 独立；8/16 在查找前拒绝，FD1/64 在 FD/名称前拒绝。CREATE2/REPLACE4 在读取中忽略；SHOWCOMPRESSION32 与未知位仍不支持。受保护的 com.apple.system.*、ResourceFork、FinderInfo、decmpfs、设置/删除、权限/ACL 执行及文件系统推断不在此读取契约内。

extended-attributes / extended-attributes-values / extended-attributes-unsupported 分别验证原生共用行为、虚拟字面值和保留先前输出的未知观察停止。guest/Python5,000,000us/quantum1024、公开接口10s、原生5s 不变。ARM64 私有准备完成320组 raw/SDK 对照，全部288字节保护区、carry、secondary 一致。自动出现的 com.apple.provenance 是观察值，不能据此推断默认空列表。Intel 原生和 iOS 真机仍未验证；完整 dyld、Mach IPC、Objective-C/Swift 与框架仍未完成。

Sources: [XNU syscall ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU xattr calls](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ordinary attributes](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_xattr.c), [XNU xattr definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Original code/probes; Apple implementation is not copied.

## 有界对象名称

ATTR_CMN_NAME=1 已通过 getattrlist220/fgetattrlist228/getattrlistat476 支持显式目录中具有唯一名称的非根对象。叶名称必须是 1..255 字节的合法 UTF-8。名称来自与 F_GETPATH 共用的实际对象路径，跨 dup、CWD、移动、SWAP、删除和名称重用保留最后链接的拼写；调用者别名不会覆盖它。名称和类型不依赖 stat；所选 stat 字段仍需完整有效观察。根/挂载标签、非法名称、硬链接或大小写别名、规范化和完整路径属性保持未知。

8 字节 attrreference_t 位于其他公共字段之前；attr_dataoffset 相对于引用本身，attr_length 包含 NUL，末尾名称区域按四字节填充。短输出保留完整所需长度及精确前缀，包括被截断的 UTF-8。attribute-names / attribute-names-values / attribute-names-unsupported 经 guest/C/CLI/Python 检查原生行为、独立字节与保留既有输出的根名称停止。ARM64 私有对照通过 601 次原始查询、453 次完整保护缓冲区 SDK 比较和 384 次前缀检查。SDK15.5 没有 raw476 的类型声明。native5s、guest/Python5,000,000us/quantum1024、既有公开测试10s 不变。原生 Intel、物理 iOS 和完整运行时/框架仍未验证或未完成。

## 有界目录批量属性

getattrlistbulk(461) 需要显式 enumeration_policy.bulk_attributes=true；省略或 false 不授予权限。此虚拟 TYPE 契约按无符号字节顺序返回当前直接子项名称，不含点条目，使用本地序号而非原生文件系统 cookie。初始目录对象跨 dup、移动、SWAP、删除与名称重用保留授权；新建目录不继承批量授权。既有 minimum_buffer_size、initial_minimum_buffer_size、seek_offset 仅用于 getdirentries64。

必须选择 NAME|OBJTYPE|RETURNED_ATTRS (0x80000009)，所选观察仍有效时可使用已有十一项公共字段。支持 Options0/8；bulk 忽略两个16位 bitmap/reserved 字，与独立 attrlist 校验分离。唯一属性编码器共用 attrreference_t 和 stat64 有效性判断。只返回完整记录：容得下时按8字节填充，否则允许最后一组为4字节大小。首组放不下时返回 ERANGE，输出和游标不变；所需输出仅部分可写时在复制前明确停止。只有实际返回的字节要求可写内存。

dup 共享进度，独立 open 各自推进。非零已完成遍历在名称空间变化后仍保留 EOF，在请求校验后跳过大小和输出检查；初始空目录 offset0 重新检查视图。零 lseek 重置迭代；EOF 前成员变化、任意非零 seek、混用 getdirentries64/bulk 明确停止。NAME-only 回退、带 ERROR 的条目、快照、ACL/权限判断、主机顺序及其他 mask/options 不支持。bulk-attributes / bulk-attributes-values / bulk-attributes-unsupported 检查原生共同行为、虚拟字面字节及保留既有输出的未知选择停止。ARM64 私有准备通过728组带保护区的 raw/SDK 比较。native5s、guest/Python5,000,000us/quantum1024、public10s 不变。原生 Intel、iOS 真机及完整运行时/框架仍未验证或未完成。

Sources: [XNU bulk ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/man/man2/getattrlistbulk.2), [XNU attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h). Original implementation and probes; no Apple implementation copied.

## 显式授权的普通扩展属性修改

setxattr(236)、fsetxattr(237)、removexattr(238) 和 fremovexattr(239) 使用针对初始对象的独立授权：C++ `MutableExtendedAttributes`，严格布尔 JSON `mutable_extended_attributes=true`。获授权的文件、目录或符号链接必须声明完整的普通 `extended_attributes` 列表，也可声明已知空列表。内容可写或名称空间可修改并不授予此权限。授权和值随保留的对象经过 dup、移动、删除及映射租约继续存在；新建对象和复用名称的属性起初未知。已知别名、冲突的元数据标志、受保护的系统属性、ResourceFork、FinderInfo 和压缩语义仍不支持。

替换保留虚拟列表位置，删除移除该项，新建追加到末尾。这是声明的进程内顺序，不推断 APFS 顺序。初始属性的字节、数量和授权路径引用始终预留。运行时超出初始预留的部分由现有 16 MiB/4096 限额统一管理；删除、内容失效或最终释放对象只回收这部分增量。容量、传输或截止时间失败不会发布暂存状态。必需输入完全不可读时返回 EFAULT；部分可读时在发布前明确报告不支持。修改成功使完整 stat 失效，不猜测时间，同时保留对象身份、目录成员、枚举版本/快照和游标。

修改 ABI 使用 low32 FD/options/position 和 full64 size。特权及 FD 链接选项的早期检查先于名称导入，名称导入先于对象查找。set 在检查过大的 VFS 输入（E2BIG7）前拒绝非空长度的 NULL；查找先于普通名称、position 和冲突检查。set 先导入完整值，再返回 CREATE 已存在的 EEXIST17 或 REPLACE 不存在的 ENOATTR93。CREATE 与 REPLACE 同时设置返回 EINVAL；删除操作忽略这两个位。长度为零的 set 不读取值指针。其他标志、未知权限和未经观察的提供方行为均明确停止。

xattr-mutations / xattr-mutations-values / xattr-mutations-unsupported 使用原创原生/客体对照、独立虚拟字节字面值，以及保留已有输出的缺少授权停止用例。ARM64 私有准备验证了726次 raw/SDK 调用、完整544字节保护区观察及完整可读页。native5s/compile120s/drain1s/reap1s、guest/Python5,000,000us/quantum1024、public10s 均不变。原生 Intel、iOS 真机、dyld、Mach IPC、线程/信号、Objective-C/Swift 运行时及完整框架仍未验证或未完成。

主要 ABI 参考：[XNU 系统调用声明](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master)、[xattr 定义](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h)。实现与探针均为原创，未复制 Apple 实现。

## 有界 Darwin 硬链接

原始 link 跟随最终符号链接目标；linkat 的 flags=0 链接符号链接对象本身，AT_SYMLINK_FOLLOW 跟随目标。仅接受 low32 的 0/0x40，其余低位在导入前返回 EINVAL。源路径导入、查找和目录 EPERM 先于目标导入；已存在目标返回 EEXIST。目标实际父目录需要修改授权，且双方必须属于明确建立的同一挂载域；源父目录不要求可修改。已知设备冲突、标志、特殊模式和初始身份别名仍不支持，输入元数据不会合并独立的初始对象。

新增别名只消耗一个名称条目及路径/NUL 费用，不消耗新 inode，也不重复计费对象字节或属性。内容与属性授权、元数据有效性和映射租约属于共享对象。链接数与 ctime 使用现有显式元数据策略；缺少策略时，修改后的完整 stat 保持未知。修改扩展属性使完整 stat 失效，修改内容使普通属性观察失效。删除名称后，描述对象保留名称费用，最后一个名称及对象费用也由仅存的映射租约保留；替换只能抵扣可立即释放的费用。子树移动/SWAP 按确切身份及实际父目录选择条目，树外别名不移动，相对符号链接目标从所选条目的父目录解析。

对象一旦拥有过多个名称，即使只剩一个或零个名称，F_GETPATH 与 vnode ATTR_CMN_NAME 仍明确不支持。ARM64 原生控制显示 APFS 名称观察依赖查找历史，路径与名称缓存行为也不同，尚不能声明通用缓存模型。目录批量 NAME 直接来自实际条目。相同对象的普通重命名/SWAP 保留双方条目，大小写不敏感 EXCL 仍超出当前契约。原生 Intel HVF、实体 iOS、权限/ACL、映射一致性及 EOF 信号、dyld、Mach IPC、线程和完整框架仍是独立缺口。本节仅在上述范围内扩展前文的硬链接限制。

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## 有界 O_SYMLINK 描述符

O_SYMLINK=0x00200000 在只读、只写和读写模式下保留最终符号链接对象，包括悬空或循环链接；它不授予目标内容写权限。O_CREAT 仍跟随最终目标；NOFOLLOW 保持 ELOOP 优先顺序，独占创建保持 EEXIST，O_DIRECTORY 对保留的链接返回 ENOTDIR。必需的中间路径或末尾斜杠展开沿用现有解析器和 NOFOLLOW_ANY 边界。F_GETFL 不返回对象选择位。

描述对象共用实际 LinkNode 和所选 NameIdentity。dup 共享状态标志和游标，独立 open 使用各自的描述对象。链接被重命名、删除、名称复用或父目录删除后，已有 FD 仍保留原对象。唯一名称的 F_GETPATH 和 ATTR_CMN_NAME 使用保留的所选名称；一旦有过多个名称，即使全部删除，vnode 名称推断仍明确不支持。初始固定费用始终预留，动态名称、目标、条目和属性增量等待实际最后所有者释放。另一描述对象仍保留对象或所选名称时，替换不能抵扣最后别名的费用。

符号描述符 I/O 不把原始目标字符串当作文件内容。现有标量/向量导入、访问模式和数量检查完成后，负偏移返回 EINVAL；INT64_MAX 处读取返回零，其余允许的偏移返回 EPERM，包括零长度请求。INT64_MAX 处写入返回 EFBIG，其余允许偏移在零长度成功、APPEND 或载荷访问前返回 EPERM。已有 pwrite/pwritev 负偏移的早期检查仍是唯一依据。DATA/HOLE seek 对非负位置返回 ENXIO，负位置返回 EINVAL，游标不变。

只写/读写描述对象的非负 ftruncate 以及允许的 open TRUNC 只设置 WasWritten 状态，不改变目标字节、完整 stat、扩展属性、游标、存储准入或 inode 分配。只读 ftruncate 和负长度返回 EINVAL。F_SETFL 的允许参数先改变 APPEND|NONBLOCK，再返回 ENOTTY25；dup 可观察变化，独立打开不受影响。未知参数在产生效果前停止。

固定 fpathconf、fgetattrlist 及独立声明的普通 FD 扩展属性授权作用于符号对象。相对目录 FD 查找和 fchdir 返回 ENOTDIR。属性修改仍使 stat 失效，截断不会恢复元数据。旧式、偏移对齐、非可执行的 private/shared mmap 选择到达符号对象类型的 EINVAL 拒绝，不创建映射或租约。普通 shared 映射、未知标志、执行权限及其他现有不支持边界保持不变。原生映射对照只覆盖 length16384、offset0、protection1/2/3，并未验证所有 mmap 变体。

原创 ARM64 准备对15项截断及隔离的对象选择保留完整144字节 stat 和保护区比较。O0/O2 观察还覆盖标量/向量的极端偏移和数量、稀疏 seek、18项映射拒绝及失败 F_SETFL 的副作用。无 SDK 共用程序另以 O0/O1/O2 编译，比较描述符实际父目录路径，避免原生临时目录拼写别名。虚拟模式比较独立的完整 stat/type 字面值，或在多名称查询时明确停止并保留输出。原生 Intel HVF、iOS 真机、ACL/权限执行、映射 EOF/信号一致性、dyld、Mach IPC、线程和完整运行时/框架仍未验证或未完成。

主要解释依据：[对应版本 XNU 映射边界](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_mman.c)。实现与探针均为原创，未复制 Apple 实现。

```text
O_SYMLINK=0x00200000; ENOTTY=25
open O_RDONLY|O_SYMLINK|O_TRUNC: WasWritten only
LinkNode, NameIdentity, HadMultipleNames, DetachedNames
INT64_MAX read=0 / write=EFBIG; other admitted offsets=EPERM
SEEK_DATA/SEEK_HOLE nonnegative=ENXIO / negative=EINVAL
F_SETFL: APPEND|NONBLOCK effect before ENOTTY; dup shares / independent open separate
symbolic-descriptors
symbolic-descriptors-values
symbolic-descriptors-name-unsupported
SymbolicDescriptor*, SymbolicDescriptorsRetainObjectsAndNativeErrorOrder
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 transport parameters / 15 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## 有界的非阻塞描述符状态

普通文件、目录及 O_SYMLINK 打开允许 O_NONBLOCK=4，F_GETFL 保留该状态。dup 共享状态与游标，独立打开保留各自的描述。既有访问模式、close-on-exec、WasWritten、元数据、字节和游标规则继续生效。显式有限标准输入保留 EOF 与指针错误顺序；省略输入仍表示未知。输出捕获保留复制错误及共享输出预算。

F_SETFL 在产生效果前检查低32位允许参数，再按原生打开标志转换加一，只改变 APPEND|NONBLOCK。高32位忽略；访问及输入 WasWritten 位不能授予权限或伪造写入。原生字面控制中，参数3/7/11/15分别选择状态4/8/12/0。符号描述先提交状态变化，再返回 ENOTTY25。ASYNC0x40 等未知标志在效果前明确停止。

原创 ARM64 macOS 准备记录122项观察，包括每个有效对象/访问组合的16项低位请求、保留的 dup、独立打开、清除状态、实际写入及每个 FD 的 CLOEXEC。无 SDK 共用程序以 O0/O1/O2 执行，比较字节、状态、游标和原始 BSD carry/errno ABI。来宾、C/CLI 和 Python 路径也要求未知标志拒绝，三个 ARM64 HVF profile 均必须实际执行。这里不实现就绪等待、管道、网络、kqueue、异步信号或宿主 I/O。O_EVTONLY 进程策略仍未支持；原生 Intel HVF、iOS 真机和完整 macOS/iOS 环境仍未验证或未完成。

```text
O_NONBLOCK=4; F_SETFL raw low32 mask=0x1000f
requests3/7/11/15 -> APPEND|NONBLOCK status4/8/12/0
nonblocking-descriptors / nonblocking-flags-unsupported
Nonblocking*, NonblockingDescriptorsKeepNativeControlState
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
8 model cases / 20 transport parameters / 10 public cases / 5 Python profiles
67 mandatory workloads per platform / ARM64 201 / Intel 134
```

## 显式有限 getentropy 观测

原始 BSD getentropy500 使用有序 `DarwinSystemOptions::EntropyReads` 观测队列，JSON 字段为 `darwin_system.entropy_reads`。每条必须是非空、偶数长度的十六进制字符串；最多256条，每条1..256字节。这是有限模型的限制；已有65536字节 JSON 传输上限不变。省略表示未知，`[]` 表示明确耗尽。原生选项与 JSON 在加载镜像或修改后端之前严格校验，其他 OS 配置拒绝 Darwin 选项。

先检查完整64位长度：超过256返回 EINVAL22，不访问内存或消费记录；零长度对任何指针都成功，不需要观测。非零请求先准入下一条长度完全匹配的记录，再执行复制。缺少、耗尽或长度不匹配均在任何效果前以 UnsupportedService 停止，包括无效地址；这是回放准入顺序。完整复制成功或目标完全不可写的 EFAULT14 消费恰好一条。部分可写目标在复制和推进游标前拒绝；内存传输错误保持错误且不推进。之后返回寄存器失败不会撤销已完成的复制或消费。同一组选项重复执行也从首条记录开始。

每次执行的 DarwinEntropy 独立持有游标，输入字节不变；已有 BSD 分发和 returnService 统一负责两种 ISA、进位及次级寄存器。无 SDK 回放程序检查零及非 UTF-8 字节、哨兵、完整错误和拒绝路径，覆盖五种客体及三种 ARM64 HVF 配置；固定回放字节不加入原生确定性 RNG 清单。原始 ARM64 O0/O1/O2 探针共594次调用；哨兵发生变化的字节数不等于准确复制长度。未提供宿主随机源、密码学质量、/dev/random、libc 导入或框架；Intel HVF、iOS 真机及完整 OS 兼容性仍未验证或未完成。

```text
BSD getentropy500 / DarwinEntropy / EntropyReads / darwin_system.entropy_reads
full64 size>256 -> EINVAL22; zero ->0; whole EFAULT14 consumes one record
1..256 bytes per record / at most256 records / JSON transport65536 bytes
entropy-replay / entropy-missing / entropy-exhausted / entropy-mismatch / entropy-partial
15 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
68 mandatory workloads per platform / ARM64 204 / Intel 136
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU getentropy ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU generation/copyout boundary](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/dev/random/randomdev.c).

## 显式当前线程身份

原始 BSD thread_selfid372 读取不可变的可选 `DarwinSystemOptions::ThreadID`，JSON 字段为 `darwin_system.thread_id`。所有 uint64 位模式（包括零）均是已知观测；省略时以 UnsupportedService 停止。十进制字符串保留全部64位，数值 JSON 仅接受不超过2^53-1的精确整数。不会从宿主、PID 或 Mach 端口推断 ID。无参数调用忽略六个参数载体，不访问内存；已有低32位系统调用解析保留事件中的完整原始编号，统一 BSD 返回层保留完整64位结果并清除 carry 和 RDX/X1。Mach 编号形式仍不支持。

重复执行保留输入观测，不同选项互相独立；不分配 ID、不保证唯一性、不写入调度事件身份，也不实现线程生命周期、pthread、TLS 或 Mach IPC。原始 ARM64 O0/O1/O2 探针保留24次调用，与 SDK 当前 pthread ID 比较，并检查任意参数及编号高32位。原生通用程序仅比较同一进程内的关系；显式 ID 字节不加入确定性原生参考清单。Intel HVF、iOS 真机和完整 OS 兼容性仍未验证或未完成。

```text
BSD thread_selfid372 / Wide / ThreadID / darwin_system.thread_id
known uint64 including0 / missing -> UnsupportedService / no memory
full64 return / low32 resolution / carry clear / RDX-X1 zero / raw event number
thread-identity / thread-identity-value / thread-identity-missing
4 model cases / 20 transport parameters / 16 public cases / 5 Python profiles
69 mandatory workloads per platform / ARM64 207 / Intel 138 unverified
original ARM64 O0/O1/O2 probes24 / native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU thread_selfid ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [libpthread current-thread owner](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/kern/kern_support.c).
