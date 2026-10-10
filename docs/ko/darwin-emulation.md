**언어**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 1939e117643388dff149085b992e1ad646feefca63b630abec9b09122e15c2f1 -->

[← 문서 목록](README.md)

# macOS 및 iOS 게스트 프로세스 환경

`lib/emulation/os/darwin/`은 호스트 CPU 전송 계층과 별개로 제한된 독립 Mach-O 프로세스를 모델링합니다. `NEVERD_ENABLE_CPU_EMULATION`을 켜면 되며 Windows 드라이버 에뮬레이션은 필요하지 않습니다. `macos/`와 `ios/`가 명시적 플랫폼 프로필을 정의합니다.

| 프로필 | Mach-O 플랫폼 | 게스트 ISA | OS 페이지 |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, 기본 ARM64 | x64 4 KiB; ARM64 16 KiB |
| `ios-macho64-v1` | iOS 기기 | 기본 ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, 기본 ARM64 | x64 4 KiB; ARM64 16 KiB |

기기용 바이너리는 시뮬레이터 이미지가 아니며 호스트에서 게스트 플랫폼을 추론하지 않습니다. macOS에서 ISA가 같으면 [HVF](macos-hvf.md)를 쓸 수 있고, 다르면 `auto`가 Unicorn을 선택합니다. CPU 매핑 단위는 4 KiB로 유지합니다. [C, Python, CLI API](process-emulation.md)는 옵션, 제한 및 보고서를 공유합니다.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## 이미지와 시작

`MachOExecutionImage`는 분석의 재배치 패치 없이 원래 바이트를 보존합니다. 플랫폼과 진입점이 명확한 thin little-endian `MH_EXECUTE`만 허용합니다. 유니버설 이미지는 필요한 아키텍처 슬라이스를 명시적으로 추출해야 합니다.

메타데이터와 끝부분 바이트를 포함한 전체 파일이 파싱이나 복사 전에 `memory_limit` 안에 들어가야 합니다. 로더는 일반 파일에서 크기가 제한된 전용 스냅샷을 읽고 NUL 경로, 짧은 읽기, 크기 변경을 거부합니다. 살아 있는 파일 매핑은 유지하지 않습니다. 파일과 게스트 메모리는 같은 값의 별도 한도를 가지며 호스트 파일 I/O는 엄격한 실시간 기한을 보장하지 않습니다.

세그먼트는 현재/최대 권한과 0 채움을 유지합니다. `__PAGEZERO`는 큰 물리 메모리 할당 없이 주소를 예약합니다. 파일/VM 범위, OS 페이지 정렬, 반올림한 겹침, 헤더 소유권, 실행 가능한 진입점과 예산을 검사합니다. 헤더 세그먼트는 읽기 및 실행 가능해야 하며 보호 페이지와 전용 복귀 게이트는 예약됩니다. 마지막 파일 페이지는 페이지 경계 또는 EOF까지 원본 바이트를 유지하고 후속 완전한 VM 페이지는 0으로 채웁니다. 근거는 [XNU 로더](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c)입니다.

`LC_MAIN`은 `argc`, `argv`, `envp`, apple 벡터를 정수 인수 네 개로 받고 반환값의 하위 8비트를 종료 상태로 사용합니다. `/usr/lib/dyld`는 가져오기가 없는 이 진입 전달에서만 허용하며 호스트 dyld는 실행하지 않습니다. 0이 아닌 `stacksize`는 거부합니다. 예산은 호출자의 명시적 `stack_size`가 결정합니다.

`LC_UNIXTHREAD`는 PC만 설정된 완전한 네이티브 64비트 일반 레지스터 레코드 하나를 요구합니다. 초기 스택에는 argc, 종료된 argv/envp, `executable_path=<input filename>`을 포함하는 종료된 apple 벡터가 있습니다. 사용자 SP/플래그, 다른 레지스터, 추가 flavor, 충돌하는 진입점은 거부합니다. 호스트 환경이나 Linux 보조 벡터를 상속하지 않습니다. [dyld 구조](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md)를 참고하세요.

외부 dylib, 가져오기, rebases/chained fixups, 생성자/소멸자, TLS 섹션, arm64e/PAC, 미지원 CPU 하위 유형, 암호화 및 모델링하지 않은 로드 명령은 실행 전에 실패합니다. fixup 없는 PIE는 선호 주소를 사용하며 ASLR이 아닙니다. 서명 blob은 메타데이터이며 AMFI나 entitlement 정책을 구현하지 않습니다.

## Darwin 서비스

BSD 호출에서 ARM64는 X16, X0–X5와 `svc #0x80`을 사용하고 x64는 BSD 클래스 `0x02000000`, RAX, RDI/RSI/RDX/R10/R8/R9를 사용합니다. 성공 시 carry를 지우고 오류 시 carry와 양수 errno를 반환합니다. ARM64는 X1을 지우고 x64는 성공 시 RDX를 지우며 오류 시 보존합니다. SYSCALL의 레지스터 변경은 명시적입니다. 보고서의 `result`와 `error=true`는 BSD 오류를 나타내며 반환하지 않거나 미지원인 요청에는 두 필드가 없습니다. XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c)와 [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c)의 규칙을 따르며 Apple 구현 코드를 포함하지 않습니다.

지원 서비스는 `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `getgroups`, `mmap`, `mprotect`, `munmap`입니다. PID는1000, PPID는1입니다. UID/GID 기본값은1000이며 아래 명시적 자격 정보로 실제/유효 ID를 따로 지정합니다. 설명자 1과 2는 NUL과 비 UTF8을 포함한 바이트를 캡처하고 닫혔거나 읽기 전용인 설명자는 EBADF를 반환합니다. 부분 복사된 바이트는 유지하지만 이후 오류는 EFAULT로 남습니다. 길이가 `INT_MAX`를 넘으면 설명자, 포인터, 예산을 검사하기 전에 EINVAL을 반환합니다. 근거는 [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c)입니다.

메모리 서비스는 `flags=0x1002`, 설명자 -1, 오프셋 0의 전용 익명 데이터 매핑을 지원합니다. 길이와 고정되지 않은 주소 힌트는 OS 페이지로 올림합니다. 점유된 힌트는 높은 주소부터 검색한 뒤 기본 배치로 돌아갑니다. 기존 raw mmap은 길이 0에서 할당 없이 0을 반환하며 `MAP_UNIX03`을 지원하며 길이 0은 EINVAL입니다. Unmap/protect는 정렬된 주소를 요구합니다. NONE/READ/WRITE를 지원하고 WRITE는 READ를 포함합니다. 물리 메모리는 OS 페이지별로 소유하므로 부분 해제는 예산을 반환하고 새 페이지는 0으로 채워집니다. 빈 구간이나 최대 권한을 넘는 protect가 실패하면 전체 범위를 변경하지 않습니다. 출처: [XNU VM 서비스](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

공유/고정/JIT 매핑, 실행 가능한 익명 메모리, 기타 Mach trap, 간접 syscall, 스레드, 신호, 호스트 파일/네트워크, dyld, Objective-C/Swift runtime, Foundation/UIKit은 지원하지 않으며 명시적으로 중지합니다. 전체 Apple OS나 iOS Simulator 애플리케이션이 아닙니다.

## 검증

직접 작성한 C 테스트 입력을 Clang과 `ld64.lld`로 만들며 Apple SDK나 독점 바이너리가 필요하지 않습니다. 플랫폼/ISA 다섯 조합, 잘못된 Mach-O 레코드, 4/16 KiB 페이지, 예산이 가득 찬 상태의 부분 해제를 검사합니다. `NeverDProcessPublicTests`가 C API/CLI를 비교하며 `NEVERD_TEST_LIBNEVERD`와 `NEVERD_TEST_DARWIN_FIXTURES`는 Python SDK의 동일한 다섯 조합을 활성화합니다.

## 명시적 파일 입력과 설명자

`darwin_files`는 세 프로필에 기본적으로 읽기 전용인 닫힌 파일 목록을 제공합니다. 필수 `files` 항목에는 정규 절대 게스트 `path`와 16진수 `bytes_hex`가 있으며 선택적 `stdin_hex`는 유한 입력입니다. 입력 생략은 알 수 없는 상태로 비영 읽기를 중지하며 빈 문자열은 EOF입니다. 목록 생략 시 open을 중지하고 명시적 빈 목록의 없는 절대 경로는 ENOENT를 반환합니다. 호스트 파일이나 입력을 사용하지 않습니다.

`open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl`을 추가합니다. read/write/open/close/fcntl/pread의 nocancel도 같은 구현을 사용합니다. O_RDONLY/O_CLOEXEC와 F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL을 지원합니다. 개별 open은 독립 위치를, dup은 공유 위치와 독립 close-on-exec 플래그를 가지며 pread는 위치를 바꾸지 않습니다. 0/1/2의 닫기와 교체는 이후 I/O에 적용되고 복제 출력은 원래 캡처 대상과 예산을 유지합니다.

최대 256개 파일, 경로/NUL/파일/입력 합계 16 MiB, 1024바이트 미만 경로와 255바이트 이하 구성 요소를 허용합니다. 배타적 상한 `descriptor_limit`은 3–4096, 기본 256이며 JSON은 64 KiB입니다. 잘못된 설정은 로드 전에 거부합니다. INT_MAX 초과 읽기는 FD 조회 전에 EINVAL이며 EOF는 목적지에 접근하지 않고 잘못된 목적지는 EFAULT입니다. 일부만 쓰기 가능한 버퍼는 복사와 위치 변경 전에 중지합니다. SET/CUR/END 실패는 위치를 보존합니다. 구형 stat와 기타 fcntl은 미지원입니다. 파일을 경로 조상으로 쓰면 ENOTDIR입니다. 같은 오브젝트를 네이티브 macOS와 비교하고 C/CLI/Python으로 다섯 게스트 조합을 검사하며 iOS 실기기 검증은 아닙니다.

2026-10-05 Release 검증은 381개 중 177개 통과, 204개 건너뜀, 실패 0이며 ARM64 HVF 필수 51/51을 실행했습니다. 네이티브 macOS 7개 프로그램, 공개 C/CLI 및 보고서 35개, Python 다섯 게스트 조합과 검증 스크립트 66개도 통과했습니다. 집계는 겹칩니다. 새 파일 서비스의 Intel HVF/KVM/WHP 네이티브 증거는 없으며 Intel HVF는 미검증 상태로 Actions가 중지되어 있습니다. iOS SDK와 실기기 대조도 없습니다.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## 기존 파일 내용 변경

엄격한 불리언 `"writable":true` 또는 C++ `DarwinFileOptions::WritableFiles`로 프로세스 내 변경을 명시합니다. 생략/false는 읽기 전용이며 알 수 없는 권한은 중지합니다. 호스트나 입력 옵션을 바꾸지 않습니다. write(4/397), pwrite(154/415), truncate(200), ftruncate(201), O_TRUNC는 내용 노드를 공유합니다. 별도 open의 위치는 독립적이고 dup은 위치와 상태를 공유하며 마지막 close 뒤에도 내용이 남습니다. 확장은 0으로 채우고 절단은 위치를 보존하며 O_RDONLY|O_TRUNC도 절단합니다.

F_SETFL은 원시 플래그 변환 후 O_APPEND|O_NONBLOCK만 바꾸고 접근 모드, close-on-exec, FWASWRITTEN을 보존합니다. 실제 비영 바이트 전송 뒤 F_GETFL에 0x10000이 나타나며 pwrite와 출력 캡처도 포함합니다. pwrite는 append를 무시하고 위치를 유지합니다. INT_MAX 길이 검사는 FD보다 앞서고 pwrite의 -1은 더 먼저 EINVAL입니다. INT64_MAX는 길이 0보다 먼저 EFBIG이며 길이를 제한한 뒤 EOF를 선택합니다.

성공한 ftruncate는 크기가 같아도 호출 open 설명과 dup에 FWASWRITTEN을 설정합니다. O_TRUNC는 O_RDONLY를 포함해 새 설명에 설정하며 경로 truncate는 기존 설명의 플래그를 바꾸지 않습니다.

일부만 읽을 수 있는 입력은 효과 전에 중지합니다. 전체 EFAULT는 바이트를 보존하지만 비어 있지 않은 append는 위치를 EOF로 옮깁니다. 전송 실패는 내용/위치를 확정하지 않습니다. `mutation_policy`가 없으면 비영 쓰기, 절단, 비영 전체 EFAULT는 완전한 stat 관측을 무효화해 후속 stat을 출력 전에 중지하며 0 쓰기는 유지합니다. 16 MiB는 경로/NUL, 입력, 디렉터리 레코드, CWD, 현재 내용 및 쓰기 경로 참조의 합계 논리 예산입니다. 축소 시 backing을 교체해 용량을 회수하고 초기 입력과 제한된 교체 버퍼는 별도입니다. 알려진 inode 별칭 및 immutable/append-only 플래그는 거부합니다.

DarwinMemory의 모든 매핑 구간을 unmap하기 전에는 변경을 거부하며 PROT_NONE과 닫힌 FD도 포함합니다. 실패/구형 0 길이 매핑은 임대를 남기지 않습니다. 새 매핑은 현재 바이트를 봅니다. O_WRONLY의 READ/WRITE mmap은 EACCES, PROT_NONE은 성공하고 mprotect로 읽기/쓰기를 부여할 수 있습니다.

원본 일반/nocancel 프로그램의 네이티브 비교, 4K/16K 단위 테스트, C/CLI/Python의 다섯 구성을 검사합니다. 권한 강제 검사/디렉터리 삭제/서로 다른 초기 디렉터리 영역 간 이름 변경/하드링크, 실제 파일 시스템 메타데이터 갱신, 매핑 일관성, EOF SIGBUS와 완전한 환경은 아직 미완성입니다. iOS 실기기와 Intel HVF 증거는 없고 Intel Actions는 중지 상태입니다.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## 명시적 가변 메타데이터

파일의 `writable: true` 및 완전한 metadata 옆에 mutation_policy를 지정할 수 있습니다. C++은 `DarwinFileOptions::MutationPolicies`를 사용합니다. 명시적 가상 희소 할당 계약이며 APFS를 추측하거나 호스트 시계를 읽지 않습니다. 생략 시 변경 후 메타데이터는 계속 알 수 없습니다.

allocation_unit, mutation_time 및 seconds/nanoseconds는 필수이며 기존 무손실 정수 규칙을 따릅니다. 단위는512바이트~16 MiB의2의 거듭제곱이며 block_size/VM 페이지와 독립적입니다. 일반 권한(set-id/sticky 없음), flags=0, link_count=1 및 초기 밀집 할당 blocks=ceil(size/allocation_unit)*(allocation_unit/512)가 필요합니다. 0 바이트로 구멍을 추론하지 않습니다. 정책 경로 참조도16 MiB 논리 예산에 포함하며 할당 장부로 ENOSPC를 추론하지 않습니다.

쓰기는 닿은 모든 단위를 할당하며 구멍에 0을 써도 할당합니다. truncate 확장은 0만 추가하고 축소는 올림 EOF 밖의 단위를 버리며 마지막 부분 단위는 유지합니다. 재확장은 버린 할당을 복원하지 않습니다. 비영 성공 쓰기와 모든 성공 truncate(같은 크기/빈 O_TRUNC 포함)는 size/blocks와 고정 mtime/ctime을 갱신합니다. 다른 필드와 초기 입력은 그대로이며 읽기는 atime을 진행하지 않습니다. 경로 stat, 별도 open, dup, 재열기는 노드를 공유합니다.

0 쓰기, 예산/매핑 거부, 부분 입력 거부, 백엔드 실패는 알려진 상태를 보존합니다. 비영 전체 EFAULT 뒤에는 메타데이터를 알 수 없으며 이후 성공으로 복원하지 않습니다. stat 출력 실패는 노드를 바꾸지 않습니다. virtual-file-metadata는 다섯 구성 및 C/CLI/Python으로144바이트를 검사합니다. 할당 검사는 정책 테스트이며 APFS 동등성 증거가 아닙니다. 네이티브는 플래그/위치/오류 순서를 별도로 검증합니다. 네임스페이스, 실제 FS 일관성, Mach, 동적 런타임은 미완성입니다.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```


## 희소 파일 위치 지정

일반 파일에 mutation_policy가 있고 할당 상태가 알려져 있으면 lseek가 SEEK_HOLE=3, SEEK_DATA=4를 지원하며 stat과 같은 장부를 읽습니다. 첫 변경 전에는 0 값까지 밀집 할당입니다. 원하는 종류의 단위 안에서는 입력 위치, 아니면 다음 일치 단위의 시작을 반환하며 끝 구멍은 EOF입니다. 음수는 EINVAL, EOF 이상(빈 파일 포함) 또는 뒤 데이터 없음은 ENXIO=6입니다. 실패는 커서를 유지하고 성공은 해당 open 설명과 dup만 바꿉니다. 별도 open은 독립적이며 재열기는 현재 할당을 봅니다. 메타데이터/플래그/바이트는 그대로이고 whence 상위 비트는 무시합니다.

정책 없음, 디렉터리, 전체 EFAULT 뒤 알 수 없는 할당은 거부합니다. 0 값이나 거부된 변경으로 할당을 추론하지 않습니다. 자체 sparse-file-seek는 네이티브/게스트의 오류, 쓴 바이트, EOF와 설명 수명을 확인하며 앞선 FS별 구간 위치를 가정하지 않습니다. virtual-file-metadata는 정확한 정책 배치를 별도로 검사하고 C/CLI/Python은 다섯 구성을 검사합니다.

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 일반 파일 이름 삭제

디렉터리 `mutable:true`(C++ `MutableDirectories`)는 바로 아래 이름 변경 권한이며 내용의 `writable`과 별개입니다. 권한이 없으면 미지원으로 중지합니다. 알려진 비영 flags, 부모 특수 권한, 자식 link_count≠1, 부모/자식의 알려진 별칭을 거부합니다. stat/스냅샷 inode를 함께 확인하고 명시적으로 다른 장치는 구별하며 경로 비용을 예산에 포함합니다.

`unlink(10)` / `unlinkat(472)`는 기존 일반 이름을 제거합니다. 일반 파일 제거의 하위32비트는 0 또는 `0x800`만 지원합니다. 알 수 없는 비트는 경로/FD보다 먼저 EINVAL; AT_REMOVEDIR는 아래 제한된 제거를 사용하고 DATALESS와 SYSTEM_DISCARDED는 미지원입니다. 공통 경로 해석으로 ENOENT, 파일 뒤 슬래시 ENOTDIR, 일반 디렉터리 EPERM, 슬래시만 있는 루트 EISDIR, `.`/`..`로 끝나는 루트 EBUSY를 보존하며 네이티브로 마지막 `.`/`..`도 검사했습니다.

기존 FD/dup/독립 open은 데이터·커서·플래그를 유지하고 F_GETPATH는 이전 경로를 반환합니다. 새 open은 실패하되 암시적 부모와 CWD는 남습니다. 쓰기 권한은 객체에 속하며 마지막 설명자와 매핑 범위가 해제된 뒤 close/dup2/다음 변경에서 현재 바이트 예산을 회수합니다. 초기 경로 비용은 유지하며 권한 강제 검사·서로 다른 초기 디렉터리 영역 간 이름 변경·하드링크는 남아 있습니다. 초기 디렉터리 제거는 아래의 명시적 허가를 사용합니다.

부모 stat/readdir/SEEK_END는 구·신 FD 및 경로 모두에서 무효화되어 복사/커서 변경 전에 중지합니다. read/pread의 EISDIR와 SET/CUR/F_GETPATH/fchdir/상대 조회는 유지됩니다. 알려진 변경 정책은 nlink=0 및 고정 ctime만 적용하며 후속 쓰기도 nlink=1을 복구하지 않습니다. 정책 없음/EFAULT 뒤 메타데이터는 미상입니다. 실패는 상태를 보존하고 원본 `unlinked-file`은 네이티브 이름/FD 동작을 비교합니다. 정책 시간과 부모 무효화는 명시적 모델 규칙입니다.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## 일반 파일 생성

O_CREAT=0x200은 명시적 mutable 부모 바로 아래 빈 파일을 만듭니다. 일반/nocancel open·openat이 공유합니다. 새 객체는 쓰기 가능하고 기존 객체는 WritableFiles를 유지합니다. 읽기 전용 FD도 생성할 수 있으나 쓸 수 없습니다. 생성 정책이 없으면 stat64와 희소 탐색은 미상입니다. 새 객체는 같은 이름의 옛 metadata/mutation_policy를 상속하지 않습니다.

O_CREAT과 O_EXCL=0x800의 조합은 기존 파일/디렉터리에 자르기 전 EEXIST를 반환하며 O_EXCL만으로는 효과가 없습니다. 기존 디렉터리의 읽기 전용 O_CREAT은 성공합니다. 아래의 openat 첫 바이트 및 디렉터리 사전 검사 뒤에는 잘못된 접근 모드→FD 여유→O_CREAT|O_DIRECTORY의 EINVAL→경로 순서입니다. 원래 경로의 마지막 누락 요소만 생성하고 누락 조상 및 끝 `/`, `//`, `/.`, `/..`는 ENOENT입니다. 새 O_CREAT|O_TRUNC는 FWASWRITTEN을 설정하지 않지만 기존 파일 자르기는 설정합니다.

실제 삽입만 부모 관측을 무효화하며 같은 이름의 새/옛 데이터·FD·메타데이터·매핑 수명은 독립적입니다. 256개 제한에는 초기 비파일 항목과 살아 있는 파일 객체가 포함됩니다. 새 정규 경로/NUL과 현재 바이트는 16 MiB에 포함하고 삭제 후 마지막 FD/매핑 해제 때 동적 비용을 회수합니다. 초기 비용은 유지합니다. 예산 또는 1024바이트 정규 경로 한계는 명시적으로 중단하며 ENOSPC나 네이티브 경로 오류를 만들지 않습니다. 실패는 이름/FD를 남기지 않습니다. created-file 네이티브/다섯 프로필, 4K/16K 경계 테스트가 계약을 검사합니다. 권한 강제 검사·서로 다른 초기 디렉터리 영역 간 이름 변경·링크·디렉터리 변경은 남아 있습니다.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## 명시적 생성 메타데이터와 프로세스 umask

선택적 `darwin_files.umask`(C++ `InitialUmask`)는 초기 마스크를 8진수 0..07777로 지정하며 생성 권한과 독립적입니다. `umask(60)`은 이전 마스크를 반환하고 입력의 하위 07777 비트를 저장합니다. 게스트 메모리나 빈 FD가 필요하지 않습니다. 생략은 미상이며 호스트 값이나 기본값을 추측하지 않습니다. 한 번만 초기화하고 이후 생성에만 영향을 주며 호출자 입력은 바꾸지 않습니다. 아래 10진수 18은 8진수 0022입니다.

`namespace_policy`를 생략하면 선택적 `darwin_files.creation_policy`(C++ `CreationPolicy`)는 새 객체에 완전한 메타데이터를 제공합니다. 엄격한 객체는 `first_inode`, `block_size`, `generation`, `creation_time`, `mutation_policy` 다섯 필드만 포함하며 시간과 변경 정책은 기존 형식을 사용합니다. 명시적 umask, 하나 이상의 mutable 부모와 모든 해당 부모의 완전한 metadata가 필요합니다. block_size는 1..INT32_MAX, generation은 uint32입니다. 할당 단위는 512..16 MiB 범위의 2의 거듭제곱이며 블록 크기/VM 페이지와 독립적이고 나노초는 [0,1000000000)입니다. first_inode는 0이 아닌 uint64로 다른 장치를 포함한 모든 stat/스냅샷 inode보다 커야 합니다. JSON의 정확한 정수 범위를 넘으면 10진수 문자열을 사용합니다.

성공한 새 삽입만 전역 inode 순서를 진행합니다. UINT64_MAX 사용 후 영구 소진되며 close/unlink/이름 재사용/umask/이후 조회로 초기화되지 않습니다. 배타·FD·경로·항목·바이트 예산 실패는 이름/FD/번호 증가를 남기지 않고 기존 O_CREAT도 번호를 쓰지 않습니다. 새 stat64는 직계 부모 device/GID, 선택한 게스트 유효 UID（기본1000）, mode `S_IFREG | (mode & 0777 & ~umask)`, nlink=1, size/blocks/flags=0을 사용합니다. 블록 크기, generation, 네 초기 고정 시간은 정책에서 얻습니다. 부모 전체 stat/열거가 무효화되어도 불변 device/GID를 쓸 수 있지만 전체 기록은 복원하지 않습니다.

새 노드는 자체 메타데이터/할당 상태를 가지며 옛 동명 객체를 상속하지 않습니다. write/truncate/unlink는 변경 정책을 공유하고 inode/mode/birthtime과 삭제 후 nlink=0을 보존합니다. 전체 EFAULT 후 영구 미상 상태는 유지하며 기존 노드에 소급 적용하지 않습니다. `created-file-metadata`는 다섯 구성에서 네이티브 권한, 이전 마스크, 유효 UID, 부모 장치/그룹과 수명을 비교하고 `virtual-created-metadata`는 144바이트 전체 기록을 별도로 비교합니다. 네이티브의 네 시간은 같지 않을 수 있습니다. 고정 시간/희소 할당은 가상 규칙이며 권한 강제 검사, 자격 증명 전환, ACL, 네이티브 APFS 동작은 아직 지원하지 않습니다.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## 일반 파일과 생성 디렉터리의 원자적 교환

RENAME_SWAP=0x2는 renameatx_np로 기존 일반 파일 두 개, 살아 있는 프로세스 생성 디렉터리 두 개, 또는 파일과 생성 디렉터리를 교환합니다. RENAME_NOFOLLOW_ANY를 추가할 수 있습니다. 명시적 초기 디렉터리는 mutable:true와 swap_rename:true를 선언하며 C++는 DarwinFileOptions::SwapRenameDirectories를 사용합니다. 생성 후손은 원래 객체의 능력을 상속하지만 삭제한 이름의 재사용은 이전 선언을 옮기지 않습니다. false 또는 생략은 지원 여부를 알 수 없다는 뜻입니다. 같은 장치나 변경 권한만으로 교환 지원을 증명할 수 없으며 다른 초기 영역은 미지원입니다.

`openat_nocancel`, `fstatat64`, `F_GETPATH=50`은 같은 파일 구성요소를 사용합니다. 교환 후 경로와 관찰은 각 객체에 속하며 전체 stat의 가용성은 기존 메타데이터 계약을 따릅니다.

없는 대상은 후행 슬래시가 있어도 ENOENT이며 원본 점/이중 점, 영역, 권한, 능력 검사보다 우선합니다. 초기 또는 삭제된 디렉터리 피연산자는 미지원입니다. 허용된 영역에서 양방향 부모/자식 교환과 디렉터리/자식 파일 교환은 EINVAL, 일반 파일 대상의 후행 슬래시는 ENOTDIR입니다. 일반 구성요소의 같은 객체는 이름공간 권한 검사 후 무동작이며 능력 선언 없이도 가능합니다. 같은 객체의 원본 점/이중 점은 알 수 없는 파일시스템 대소문자 속성이 필요합니다. RENAME_EXCL=0x4 + RENAME_SWAP=0x2과 알 수 없는 flags는 경로 입력 전에 EINVAL이며 SECLUDE는 미지원입니다.

비어 있지 않은 두 하위 트리는 부모 객체를 따라 이동하며 FD/매핑이 보유한 삭제된 디렉터리와 고아 파일도 포함합니다. 혼합 교환은 정확한 파일 루트만 옮기고 같은 옛 이름의 고아는 자신의 부모에 남습니다. FD, dup, CWD, 이중 점은 객체와 새 부모를 따릅니다. 후손 파일의 바이트, 정체성, 메타데이터, 쓰기 권한, 커서, flags, 임대는 보존됩니다. 이동 루트와 양쪽 직계 부모에는 기존 이름공간 메타데이터 규칙이 적용되며 설정 정책은 루트 파일 자체 ctime을 갱신합니다. 정책이 없거나 무효화되면 전체 메타데이터는 알 수 없습니다.

능력 참조 경로+NUL은 고정 초기 16 MiB 예산에 예약됩니다. 양방향의 모든 연결/보유 경로를 1024바이트 한계와 공유 총액으로 사전 검사한 뒤 옛 이름을 함께 제거하고 새 이름을 게시합니다. 루트 삭제나 교체 환급이 없고 열리지 않은 대상 바이트도 사용할 수 없습니다. 초기 파일의 첫 이동은 동적 경로 비용을 얻으며 되돌려도 없애지 않고 반복해도 누적하지 않습니다. 새 FD, 항목, 생성 inode가 필요하지 않습니다. 실패하면 두 이름공간, 부모, 커서, 관측, 매핑이 그대로입니다. 독자 SDK-free swapped-directory는 네이티브 macOS 및 다섯 C++/C/CLI/Python 프로필에서 디렉터리 교환과 혼합 양순서를 비교합니다.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## 명시적 디렉터리 스냅샷

`getdirentries64` (344)는 기존 `directories` 항목의 선택적 불변 `contents`를 열거합니다. C++에서는 `DarwinFileOptions::DirectoryContents`를 사용합니다. `entries`는 `.`과 `..`, 모든 직접 자식을 명시한 순서대로 포함해야 합니다. 빈 디렉터리도 스냅샷이 없으면 미지정입니다. 경로나 stat 정보를 생성하거나 호스트를 조회하지 않습니다.

각 항목은 `name`, 0이 아닌 `inode`, `type`(0 미지정, 4 디렉터리, 8 일반 파일), `next_offset`, `seek_offset`이 필수입니다. 타입은 경로와, 같은 경로의 inode는 다른 스냅샷·메타데이터와 일치해야 합니다. `next_offset`은 디렉터리 내 유일한 0 초과 INT64_MAX 이하 값이며 증가할 필요는 없습니다. 0은 되감기입니다. 별도 관측값 `seek_offset`은 부호 없는 64비트 d_seekoff이며 중복 0도 허용합니다. 정수는 stat과 같은 무손실 십진 문자열 규칙을 사용합니다.

필수 `contents.minimum_buffer_size`는 EOF를 포함한 페이로드 최소값 1–128 MiB입니다. 항목별 선택적 `minimum_buffer_size`(기본 0)는 해당 위치에서 시작할 때 추가 제한입니다. 예제는 APFS 시작 점 항목 쌍에 64바이트, EOF에 1바이트가 필요한 관측을 담습니다. 다른 위치도 완전한 기록 하나가 필요합니다. LP64 기록은 8바이트 정렬이며 길이는 `roundUp(25 + nameBytes, 8)`입니다. 전체 4096항목 한도, 기록 바이트도 16 MiB 예산에 포함됩니다. 메타데이터/스냅샷만 명시한 조상 경로는 중복 없이 256경로 한도에 포함되며 JSON은 64 KiB입니다.

독립 open은 별도 커서, dup는 공유 커서를 사용합니다. 0 또는 제공한 값에서만 재개하며 알 수 없는 위치는 중단합니다. 들어가는 완전한 기록의 최대 접두부를 반환합니다. 길이 >=1024이면 원래 요청 끝 4바이트에 EOF(끝이면 1, 아니면 0)를 예약하며 기록 페이로드만 128 MiB로 제한합니다. 플래그 주소는 원래 부호 없는 연산과 래핑을 유지합니다. 데이터 복사, 커서 갱신, 읽기 전 위치 복사, 플래그 순서입니다. 뒤의 EFAULT는 앞선 효과를 유지하고 EOF는 빈 데이터 복사를 생략합니다. 개별 복사가 일부만 쓰기 가능하면 그 복사 전에 미지원으로 중단하며 이전 효과는 유지합니다.

동일한 `directory-entries`는 원본 macOS와 기록, dup/되감기, 작은 읽기, EOF 및 복사 순서를 비교합니다. 별도 테스트는 SDK 배치와 긴 이름을 포함한 원본 기록의 전체 바이트를 비교합니다. 고정 스냅샷 값은 되감기에도 유지되어 APFS의 동적 세대를 재현하지 않습니다. 이전 `getdirentries` (196), 변경 후 열거, 다른 네이티브 백엔드와 iOS 실기기는 이 검증 밖입니다.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

열거 검증(2026-10-05, Release): Darwin 498개 중 246개 통과, 사용 불가 백엔드 252개 건너뜀, 실패 0개입니다. ARM64 HVF 필수 63/63개를 실행했습니다. 네이티브 macOS 프로그램 11개, C/CLI/보고서 40개(건너뜀 없음), Python 5개 조합의 파일 작업 각 8개, 실행기 66개가 통과했습니다. 수치는 중복됩니다. 증거: `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel HVF Actions는 중지 상태이며 다른 네이티브 백엔드와 iOS 실기기는 미검증입니다.

## 전용 파일 매핑

`mmap`은 일반 카탈로그 파일의 `MAP_PRIVATE`를 지원합니다. `flags=0x2` 또는 `MAP_UNIX03`을 더한 `0x40002`를 쓰며 오프셋은 OS 페이지에 정렬해야 합니다. 짧은 길이를 요청해도 페이지의 원본 파일 바이트를 유지하고 EOF 마지막 페이지의 나머지는 0으로 채웁니다. 전용 쓰기는 해당 매핑만 변경하며 원본, 다른 매핑, 고정 메타데이터와 공유 커서를 바꾸지 않습니다. close 또는 설명자 재사용 후에도 매핑은 유지됩니다. 읽기 전용과 PROT_NONE도 초기 바이트를 보존하며 `mprotect`로 쓰기를 허용할 수 있습니다.

파일 끝 산술 오버플로, UNIX03의 길이 0 및 미정렬 오프셋은 FD 조회 전 EINVAL, 잘못된 FD는 예산 검사 전 EBADF입니다. 기존 길이 0도 FD를 검사한 뒤 할당 없이 0을 반환합니다. 기존 미정렬 오프셋, 스트림, 빈 파일 페이지, 완전히 EOF 밖인 페이지는 할당 전에 미지원으로 중지합니다. macOS는 EOF 밖 매핑을 허용하지만 접근 시 SIGBUS가 발생하므로 모델은 읽을 수 있는 0 페이지나 신호 전달을 만들지 않습니다. 공유, 고정, 실행 가능, JIT 매핑은 미지원입니다.

`DarwinFiles`는 설명자와 바이트를, `DarwinMemory`는 배치·권한·예산·롤백을 소유합니다. 데이터는 `darwin_files`만 사용합니다. 동일한 `file-mapping` 프로그램으로 전용 쓰기, close 이후 수명, 커서, 오류와 익명 페이지 재사용을 검증합니다. 별도 네이티브 비교는 0이 아닌 파일 오프셋, 전체 페이지와 실제 SIGBUS 경계를 검사합니다.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### 전용 매핑 검증 (2026-10-05)

Release Darwin은 고유 등록 438건 중 210건 통과, 228건 건너뜀, 실패 0건입니다. ARM64 HVF 필수 57/57건을 실행했고 Unicorn은 다섯 게스트 조합을 검증했습니다. 네이티브 macOS 프로그램 9개, 비영 오프셋 전체 페이지 비교와 격리 자식 프로세스의 SIGBUS 검증이 통과했습니다. 공개 API/보고서 36건은 건너뜀 없이 통과했고 Python 다섯 조합의 `file-mapping`, 검증 스크립트 66건과 출처 회귀 38건도 통과했습니다. 집계는 중복됩니다. 증거: `build-hvf-arm64/darwin-mmap-verified-evidence/`. 새 Intel HVF/KVM/WHP 및 iOS 실기기 증거는 없으며 Intel HVF Actions는 중지 상태입니다.

## 명시적 파일 메타데이터

파일 항목에 `metadata`를 추가할 수 있으며 아래 필드는 모두 필수입니다. 십진 문자열은 정수의 전체 폭을 보존하고 JSON 숫자는 ±(2^53−1) 내 정확한 정수로 제한됩니다. device는 부호 있는 32비트, mode/link_count는 부호 없는 16비트, inode는 부호 없는 64비트, uid/gid/flags/generation은 부호 없는 32비트입니다. size는 파일 바이트 수와 같아야 하며 blocks는 부호 있는 64비트 상한 이하, block_size는 음수가 아닌 부호 있는 32비트입니다. 시간은 부호 있는 64비트 초와 0–999999999 나노초입니다.

`stat64` (338), `fstat64` (339), `lstat64` (340)는 ARM64/x64에서 동일한 144바이트 LP64 레코드를 반환합니다. open과 경로 해석을 공유하며 FD 복제와 닫기를 따릅니다. FD를 할당하거나 커서를 바꾸지 않고 rdev, 패딩, 예약 필드는 0입니다. 입력은 초기 메타데이터를 제공하고 변경은 선택적 정책을 따릅니다. 읽기가 시간을 갱신하지 않고 mode가 접근 허가를 바꾸지 않습니다. 미지정 메타데이터, 스트림 상태, 구형 stat, 확장 보안은 미지원입니다. 경로/FD 오류를 출력 포인터보다 먼저 처리하며 부분 쓰기 가능 출력은 변경 전에 중지합니다. 네이티브 테스트는 실제 파일의 모든 바이트와 SDK 배치를 비교하고 동일한 자체 프로그램으로 세 호출을 검증합니다.

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

### 메타데이터 검증과 남은 작업 (2026-10-05)

stat64 추가 후 Release 검증은 고유 409건 중 193건 통과, 216건 건너뜀, 실패 0건입니다. ARM64 HVF 필수 54/54건을 실행했고 Unicorn은 다섯 게스트 조합을 검증했습니다. SDK 배치 및 실제 레코드 전체 비교, 네이티브 프로그램 8개, 공개 API/보고서 36건(건너뜀 없음), Python 다섯 조합, 검증 스크립트 66건도 통과했습니다. 집계는 중복됩니다. 네이티브 테스트의 출력 파일을 사례별로 분리해 짧은 출력에 이전 끝 바이트가 남는 문제를 수정했습니다. 추가 기능의 Intel HVF/KVM/WHP 및 iOS 실기기 증거는 없습니다.

다음은 공유 매핑과 EOF 페이지 오류, 제한된 쓰기(EOF 페이지, close 이후 수명, 오류 순서 검증), 명시적 시간/시스템 정보, 필수 Mach/스레드 서비스, Mach-O 의존성·재배치/바인딩·초기화/TLS 순입니다. 이후 실제 네이티브 프로그램으로 Objective-C/Swift와 Foundation/UIKit을 검증합니다. iOS 실기기는 SDK와 장치가 필요하며 Intel HVF는 미검증이고 Actions는 계속 중지합니다.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

독립 워크로드 검증은 ARM64 105개 또는 x64 70개 네이티브 사례를 모두 요구하며 각 플랫폼의 `LC_MAIN`과 `LC_UNIXTHREAD`를 포함합니다. 필수 항목 누락, 건너뛰기 또는 `ld64.lld` 부재는 실패입니다.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Linux에서는 `kvm`, Windows에서는 `whp`를 사용합니다. [Darwin 워크플로](../../.github/workflows/darwin-native.yml)는 Unicorn 없이 두 x64 전송 계층을 검사하고 개별 재실행도 지원합니다. [커널 참조](../../.github/workflows/darwin-kernel-reference.yml)는 NeverD/LLVM 없이 두 macOS ISA에서 프로그램을 직접 실행합니다. `DarwinNativeCases.def`가 모드, 종료 상태, 예상 바이트를 정의합니다. 실제 dyld 진입을 위해 libSystem을 링크하는 것은 호스트 참조뿐입니다. ISA 불일치, Rosetta, 시간 초과 또는 결과 차이는 실패이며 iOS 실기기 커널 증거가 아닙니다.

## 증거와 남은 범위

2026-10-03 결과입니다. 겹치는 행은 합산하지 않습니다.

| 전송 계층 | 소스 | 통과 | 실패 | 건너뜀 | 네이티브 워크로드 |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

[Intel 실행](https://github.com/NeverSight/NeverD/actions/runs/37106013999)은 CTest 식별자 286개와 프로세스 32개를 원본 XML과 대조했습니다. 건너뛴 234개는 비활성 Unicorn 65개, ARM64 게스트 39개, 다른 호스트 플랫폼 130개입니다. 산출물 `11267489438`의 SHA-256은 `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`로 검증했습니다. [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703)도 독립 확인했습니다. [커널 참조](https://github.com/NeverSight/NeverD/actions/runs/37064795867)는 ISA마다 4/4 프로그램을 통과했으며 종료 상태 37, 정확한 출력, 빈 stderr를 확인했습니다.

Unicorn을 켠 C API/CLI는 138개 통과, 156개 건너뜀, 실패 0개입니다. Python은 다섯 조합을 다룹니다. 패키지 엔진은 ARM64 CLI 보고서 18개와 일치하고 Mach-O 186개 서명을 검증했습니다. HVF/Unicorn OFF는 38개 통과, 231개 건너뜀이며 Hypervisor.framework를 링크하지 않습니다. 이는 통합 증거로 추가 네이티브 실행 수에 포함하지 않습니다. 전체 Intel CPU는 아직 검증되지 않았습니다. [HVF](macos-hvf.md)와 [상세 기록](../darwin-emulation.md#hosted-native-verification-2026-10-03)을 참고하세요.

## 명시적 시간 관측값

`ProcessOptions::DarwinTime` / `darwin_time`은 모든 Darwin profile에서 raw `gettimeofday` (116)에 고정 관측값을 제공합니다. 세 번째 `mach_absolute_time` 출력도 포함합니다. `time_of_day`, `timezone`, `mach_absolute_time`은 각각 선택 사항이며 생략은 알 수 없음을, 명시적 0은 값을 뜻합니다. 빈 객체는 기본 시계를 만들지 않습니다. 호스트 시계 조회, 시간대 추론, 시간 진행, 절대 tick 변환은 하지 않습니다.

제공하는 레코드는 모든 멤버가 필요합니다. `seconds`는 부호 없는 32비트, `microseconds`는 [0, 999999], `minutes_west` / `dst_time`은 부호 있는 32비트, 절대 tick은 부호 없는 64비트입니다. JSON은 공통 무손실 정수 규칙을 따르며 안전한 정수 범위 밖은 십진 문자열로 전달합니다. 알 수 없는 필드, 범위 초과, Darwin 이외 profile은 이미지 로드 전에 거부됩니다.

LP64 `timeval`은 16바이트입니다. 오프셋 0에는 0 확장 초, 8에는 32비트 마이크로초, 12에는 4바이트 0 패딩이 있습니다. 시간대는 부호 있는 32비트 필드 둘이며 tick은 8바이트입니다. 달력 시간과 절대 시간은 먼저 함께 관측하므로 요청한 두 관측값이 복사나 포인터 검사 전에 있어야 합니다. 이후 timeval, timezone, absolute ticks 순서로 씁니다. 시간대 누락과 뒤따르는 EFAULT는 앞선 쓰기를 유지하며 겹치는 주소도 같은 순서입니다. 개별 출력이 일부만 쓰기 가능하면 해당 복사 전에 미지원으로 중지하고 앞선 복사는 유지합니다. 포인터가 모두 null이면 설정 없이 성공하며 선택적 조회는 요청한 값만 필요합니다.

직접 작성한 `time` 작업으로 네이티브 동작을 확인하고, `time-values`는 다섯 게스트 조합의 C/CLI/Python에서 설정한 32바이트를 정확히 출력합니다. 별도 SDK 비교는 한 번의 네이티브 raw 호출에서 세 출력을 얻어 모든 바이트를 비교합니다. 진행하는 시계, 변환, commpage 카운터, 타이머, Mach 시계 객체/IPC는 포함하지 않습니다. dyld, 스레드, Objective-C/Swift, Foundation/UIKit은 남은 작업입니다. Intel HVF Actions는 중단 상태이며 네이티브 Intel이나 실제 iOS 기기 검증을 추가하지 않습니다.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

시간 검증(2026-10-06, Release): Darwin 등록 538개 중 274개 통과, 사용 불가 백엔드로 264개 건너뜀, 실패 없음. 필수 ARM64 HVF 66/66개를 모두 실행했습니다. 원본 네이티브 macOS 작업 12개와 단일 표본 SDK 바이트 비교도 통과했습니다. 공개 C/CLI/보고서 43/43개 통과, 건너뜀 없음. Python은 정확한 시간 바이트 및 기존 파일 모드 8개를 포함한 다섯 게스트 조합에서 통과했습니다. 실행기 66개, 현지화, 기능 목록, 서식 검사도 통과했습니다. 집계는 중복됩니다. 증거: `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/`, `darwin-time-public.xml`.

## Mach 시간과 반환 규약

`darwin_time.timebase`의 `numerator`와 `denominator`는 0이 아닌 부호 없는 32비트 값이며 약분하거나 변환하지 않습니다. `mach_timebase_info_trap` 번호 89는 ARM64 X16=-89 또는 x64 RAX=0x01000059입니다. 분자와 분모를 리틀 엔디언 8바이트로 쓰고 0을 반환합니다. 완전히 잘못된 출력 주소도 0을 반환하지만 일부만 쓸 수 있으면 복사 전에 중지하고 전송 자체의 오류는 전파합니다. timebase가 없으면 null을 포함한 포인터 검사보다 먼저 중지합니다.

ARM64 X16=-3과 X16=-4는 `mach_absolute_time`과 `mach_continuous_time`의 부호 없는 64비트 전체를 반환합니다. 각각 자기 관측값만 필요하며 명시적 0도 유효합니다. x64의 해당 네이티브 표 항목은 EXC_SYSCALL을 발생시키므로 지원하지 않습니다. 시간 진행, commpage, 타이머, Mach 시계 객체/IPC는 미구현입니다.

분기는 번호의 하위 32비트만 사용하고 보고서는 원래 64비트를 보존합니다. ARM64 음수는 Mach, x64 Mach 클래스는 0x01000000, BSD는 0x02000000입니다. BSD 3/4는 read/write이며 알 수 없는 번호와 외부 클래스는 중지합니다. 해석된 바인딩이 반환 규약을 결정합니다. Mach는 플래그와 X1/RDX를 보존하고 BSD는 기존 carry 규칙을 따르며 x64 RCX/R11 갱신은 유지합니다. Mach 보고서는 입력 carry와 관계없이 `result`를 포함하고 `error`를 생략합니다.

`mach-time`은 ARM64 네이티브에서 플래그, 보조 결과, 번호 상위 비트, 잘못된 포인터와 BSD 전환을 비교합니다. `mach-timebase-values`는 다섯 게스트, `mach-clock-values`는 ARM64에서 정확한 바이트를 확인하고 SDK는 배치와 관측 비율을 검증합니다. Intel HVF Actions는 중단 상태입니다. x64 소프트웨어 및 구문 검사는 네이티브 Intel이나 실제 iOS 검증을 의미하지 않습니다.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach 검증(2026-10-06, Release): Darwin 569개 중 성공 293, 사용 불가 백엔드 건너뛰기 276, 실패 0이며 필수 ARM64 HVF 69/69를 실행했습니다. 최종 실행에서 네이티브 13개 작업과 시간 SDK 비교 2개가 통과했습니다. 공개 C/CLI/report는 100/100, 건너뛰기 없음이며 Python은 다섯 게스트를 검증했습니다. 공개 비교는 플랫폼과 작업별로 분리하고 명시적인 10초 게스트 예산을 사용합니다. 제품 기본값과 기한 회귀는 유지합니다. 집계는 중복됩니다.

처음 네이티브 실행은 기존 5초 제한을 넘었으며 별도 측정은 6.056초, 재사용은 0.010초였습니다. 같은 바이너리는 원래 제한에서 13개 모두 통과했고 실패 기록을 보존합니다. 호스트 부하 중 시간 초과 후 별도 순차 검증은 통과했습니다. 증거: `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`. 커밋 전 작업 트리입니다. 당시 ARM64 MRS/MSR NZCV는 미지원이어서 정수 명령으로 플래그를 관찰했습니다. 아래 변경으로 이 CPU 공백을 해소합니다.

## ARM64 조건 플래그 레지스터

공유 checked ARM64 계약은 EL0/EL1에서 정확한 `MRS Xt, NZCV` 및 `MSR NZCV, Xt` 인코딩을 허용합니다. 읽기는 비트 31–28만 반환하고 쓰기는 입력의 그 네 비트만 사용하며 나머지는 무시합니다. `XZR`로 읽으면 결과를 버리고 `XZR`에서 쓰면 SP를 읽지 않고 네 플래그를 지웁니다. 각 백엔드는 원래 명령을 실행합니다. 호스트 레지스터 설정 검증과 FPCR/FPSR 제한은 유지하며 인접한 미등록 시스템 레지스터는 계속 미지원입니다.

`NeverDAArch64NZCVTests`는 모든 플래그 조합을 호스트 명령과 비교하고 전체 스칼라/벡터 상태, 메모리, 레지스터 경계, 관찰자 중지/실패, 컨텍스트 복원 재시도와 공유 명령 예산을 검사합니다. ARM64 `mach-time`은 SVC 전후에 실제 MSR/MRS를 사용하여 Mach 플래그 보존과 BSD 전환을 확인합니다. 네이티브 HVF 필수 항목은 두 권한의 여섯 메서드 및 호스트 비교를 포함합니다. ARM64 KVM/WHP와 실제 iOS는 미검증입니다. 쓰기 파일, 시스템 정보, 진행하는 시계, Mach IPC/스레드, dyld/런타임/프레임워크 및 기기 검증은 후속 환경 작업입니다.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


변경 가능한 파일 검증(2026-10-06): Release Darwin 610개 중 322개 통과, 사용 불가 백엔드 288개 건너뜀, 실패 0. ARM64 HVF 필수 72/72를 실행했습니다. 마지막 EFAULT 메타데이터 검사를 포함한 집중 테스트는 114개 중 102개 통과, 12개 건너뜀이며 네이티브 15개 프로그램과 공개 C/CLI/보고서 111개도 통과했습니다. 집계는 겹칩니다. 최초 네이티브 실행이 발견한 FWASWRITTEN 누락을 수정했으며 실패 증거도 보존합니다. 제한 시간은 바꾸지 않았고 전체 GitHub CI 및 iOS 실기기는 별도 검증이 필요합니다. Intel Actions는 중지 상태입니다.

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

첫 Python 전체 테스트에서 ARM64 디렉터리 세 사례가 시간 초과했습니다. 인수를 바꾸지 않은 진단에서 새 쓰기 10/10은 통과했지만 iOS 한 사례는 경과 5.005초/CPU 1.263초 후 중지했습니다. 동일 5초 제한의 개별 재검사는 세 사례 모두 통과했습니다(2.43–3.17초, 10,941명령, 출력65). 논리 CPU16개에 부하54–70은 스케줄링 압력을 뒷받침하지만 지연 안정성을 보장하지 않으며 최초 실패도 보존합니다.

마지막 변경 없는 Python 통합 메서드는 다섯 구성을 모두 통과했고 총41.118초였습니다. 프로세스별5초 제한은 그대로이며 앞선 실패와 진단 기록도 따로 보존합니다.


메타데이터 검증(2026-10-06): Release 집중148개는124통과/24백엔드skip. 전체Darwin645개는343통과/300skip/기존ARM64 HVF디렉터리2개timeout. 동일한20개와 원래5초 제한 재검사는8통과/12skip, 해당 사례3.818/3.949초. 필수HVF75개 모두 통과 관측이 있으나 최초 실패를 보존합니다. 공개C/CLI/보고117/117(Darwin73), Python5구성27.359초, 네이티브15/15, runner66/66통과. 할당은APFS증거가 아니며 시한을 바꾸지 않았습니다. 전체CI/Intel/iOS실기기/완전한 환경은 미완성입니다.

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

희소 파일 위치 검증(2026-10-06): Release Darwin 671개 중 359개 통과, 사용 불가 백엔드 312개 건너뜀, 실패 0개. 필수 ARM64 HVF 78개를 모두 실행했고 Unicorn은 다섯 게스트를 검증했습니다. 집중 검사 147개 중 123개 통과, 24개 건너뜀. 네이티브 16개, 공개 C/CLI/report 122개(Darwin 비교 78개), Python 다섯 구성(12.344초), runner 66개가 통과했습니다. 집계는 중복되며 제한 시간과 과거 실패 기록을 유지합니다. 증거: `build-hvf-arm64/sparse-seek-validation-summary.json`. 할당 규칙은 명시적 가상 정책이며 APFS 동등성을 뜻하지 않습니다. 전체 CI와 실제 iOS 검증은 별도로 필요하고 Intel HVF Actions는 중지 상태입니다.

unlink 검증(2026-10-06): Release Darwin 708개 중 384개 통과, 사용 불가324개 건너뜀, 실패0개. 필수 ARM64 HVF81개 모두 실행. 집중156개는137통과/19건너뜀; 네이티브17/17, C/CLI/report128/128(Darwin83), Python 다섯 구성16.268초, runner66/66 통과. 독립 설계·구현 검토에서 남은 차단 사항이 없었습니다. 집계 중복, 제한 시간 유지, 재시도 불필요. 증거: `build-hvf-arm64/unlink-validation-summary.json`. 부모 무효화와 고정 시간은 모델 규칙이며 전체 파일 시스템/런타임 및 실제 iOS 검증은 남아 있습니다. Intel HVF Actions는 중지, 전체 CI는 별도입니다.

### 생성 검증, 2026-10-06

Release Darwin 748개: 412 통과, 백엔드 부재 336 건너뜀, 실패 없음. 필수 ARM64 HVF 84개 실행. 집중162개:150 통과/12 건너뜀. C/CLI/보고133/133(Darwin88), Python5구성9.982초, 원본 네이티브18/18, runner66/66 통과. 초기 ARM64는 테스트 포인터 표의 재배치를 올바르게 거절했고 인라인 바이트로 고쳐 통과했습니다. 로더 허용 범위를 바꾸지 않았고 최초 실패와 바이너리는 보존합니다. runner 예상 목록27→28 갱신. 부모 관측을 보존하는 용량 실패도 검사했고 독립 검토에 남은 차단 사항이 없습니다. 수치는 중복되며 시간 제한은 그대로입니다. 전체 CI·iOS 실기기는 별도, Intel HVF Actions는 중지 상태입니다.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### 생성 메타데이터 검증, 2026-10-06

Release Darwin 등록 787개: 439개 통과, 백엔드 사용 불가 348개 건너뜀, 실패 0개. 필수 ARM64 HVF 87개 모두 실행. 집중 151개 중 139개 통과, 12개 건너뜀. 공개 C/CLI/보고서 145/145(Darwin 입력 비교 98개), 변경 없는 Python 메서드 다섯 구성 12.211초 통과. 네이티브 19/19, 검증 스크립트 66/66 통과. 독립 검토에서 차단 사항 없음. 서로 다른 부모 device/GID와 전역 inode, 첫 쓰기 전 unlink, FD/입력이 없는 umask를 추가 검증했습니다. 집계 중복, 제한 시간 유지, 실패 재실행 불필요. 고정 생성/변경 시간과 할당은 가상 정책입니다. 전체 GitHub CI와 실제 iOS 검증은 별도이며 Intel HVF Actions는 중지 상태입니다.

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### 이름 변경 검증, 2026-10-06

Release Darwin 835개: 통과474, 백엔드 미지원 건너뜀360, 기존 macOS ARM64 HVF 가상 메타데이터 한 건이5.087초에 시간 초과. 같은 인자와5초 제한으로20개 재검사: 통과8, 건너뜀12, 해당 항목0.113초. 두 실행에서 필수 ARM64 HVF90개 성공을 관측했지만 전체 게이트 실패 기록은 유지합니다. 이름 변경42/54 통과,12 건너뜀; C/CLI/보고150/150(Darwin 비교103), 변경 없는 Python 다섯 구성18.478초, 네이티브20/20, 검증 스크립트66/66. 독립 검토에서 중첩 점 경로 분류를 수정했으며4K/16K 회귀는 수정 전 실패·후 통과했습니다. 이전 읽기 전용 ftruncate 테스트 기대값을 EINVAL로 수정했습니다. 실패와 프로브 버전을 보존하고 집계는 중복되며 시간 제한은 그대로입니다. 전체 GitHub CI와 iOS 실기기는 별도, Intel HVF Actions는 중단 상태입니다.

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## 명시적 시스템 관측값

`ProcessOptions::DarwinSystem` / `darwin_system`은 모든 Darwin 프로필의 `sysctl(202)`과 raw `sysctlbyname(274)`에 고정 관측값을 제공합니다. 필드는 모두 선택 사항이며, 누락된 값이나 목록 밖의 키는 지원되지 않음으로 중단합니다. 호스트 조회나 버전·모델 추정은 없습니다. 엄격한 JSON 및 C++ 검증은 이미지 로드 전에 잘못된 값과 비 Darwin 프로필을 거부합니다.

`os_revision`은 부호 있는 32비트, `cpu_count`는 1..INT32_MAX, `memory_size`는 부호 없는 64비트입니다. `max_files_per_process`는 0..INT32_MAX의 4바이트 int입니다. 나머지 스칼라 필드는 최대 1023바이트(`hostname`은 255바이트)이며 내부 NUL이 없는 문자열입니다. 명시적 빈 문자열도 유효하고 결과는 끝 NUL을 포함합니다. 관측값은 스케줄링, 할당 또는 기술자 예산을 변경하지 않습니다.

| JSON 필드 | sysctl 이름 | MIB |
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

`hw.pagesize`는 기존 게스트 메모리 정책을 따르며 보통8바이트, null이 아닌 출력의 용량이 정확히4이면4바이트입니다. 기존 MIB `[6,7]`과 `hw.pagesize_compat`는 항상4바이트입니다. `hw.pagesize`의 동적 숫자 OID는 지원하지 않습니다. `hw.memsize`도 용량4에서64비트 패턴이 부호 있는32비트 값의 부호 확장과 일치할 때만 축소합니다. 그렇지 않으면 ERANGE34이며 출력과 길이를 유지합니다.

MIB 개수는 하위32비트로2～12, 이름 길이는 전체64비트로1024 미만이어야 합니다. 지정된 모든 바이트를 검사한 뒤 첫 NUL을 해석하고 끝의 점 하나를 제거합니다. 빈 이름은 ENOENT, 부분적으로 읽을 수 있는 입력은 미지원입니다. null이 아닌 `oldlenp`는 효과 발생 전에8바이트 전체를 읽고 쓸 수 있어야 합니다. 잘못된 길이 포인터의 네이티브 실험이 제한 시간 안에 반환하지 않아 명시적으로 지원 범위 밖에 둡니다. null `oldlenp`는 용량0, null `oldp`는 크기 조회입니다. `kern.hostname` 외의 짧은 버퍼는 ENOMEM12, 데이터 불변, 길이0입니다. 데이터 EFAULT는 이전 길이를 보존합니다. 입력과 용량을 먼저 캡처하고 데이터 다음 길이를 쓰므로 별칭 순서와 후속 전송 실패 전의 복사를 보존합니다.

`hostname`은 이 guest 호출자에게 보이는 바이트를 선언합니다. 호스트 조회, 모바일 환경의 `localhost` 기본값 또는 entitlement 추론은 없습니다. 생략은 미지이며 명시적 빈 문자열은 NUL 하나를 반환합니다. `kern.hostname`의 null이 아닌 출력이 양수 용량에서 짧으면 정확히 그 용량의 바이트를 끝 NUL과 함께 성공적으로 반환하고 해당 용량을 보고합니다. 용량0은 여전히 ENOMEM12, 길이0이며 데이터를 쓰지 않습니다. null 출력은 NUL을 포함한 전체 길이를 보고합니다. 실제 출력 범위만 검사합니다. 일부만 쓸 수 있는 범위는 미지원이고 접두부를 게시하지 않습니다. 네이티브 부분 복사는 모델 밖입니다. libc uname/gethostname이 쓰는 raw 관측만 추가하며 dylib 가져오기나 완전한 런타임을 구현하지 않습니다.

newp/newlen 모두0이 아니면 쓰기입니다. 이름/MIB와 oldlenp 전체 읽기/쓰기 사전 검사를 먼저 유지합니다. 기본 또는 명시적 비root EUID는 관측/데이터 출력 검사 전 EPERM1이며 EUID0의 kern.osversion / kern.maxfilesperproc / kern.hostname 특권 쓰기는 미구현이므로 unsupported입니다. RUID는 결정하지 않습니다. 다른 네이티브 읽기 전용 노드는root도 EPERM1입니다. 새 길이0은 포인터를 무시하고 알 수 없는 키/트리/동적OID의 ENOENT는 추정하지 않습니다.

자체 작성 `system-info`는 네이티브 macOS와 게스트 ABI를 검사하고 `virtual-system`은 C++·C/CLI·Python에서 설정 바이트를 비교합니다. 별도 SDK 검사는 호스트의 아홉 관측값을 명시적 테스트 입력으로 삼아 이름·숫자 출력을 비교합니다. iOS 실기기나 Intel HVF 검증을 뜻하지 않습니다.

```json
{"darwin_system":{"hostname":"guest-node","os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/sysctl.3.html).

### 시스템 조회 검증, 2026-10-06

Release Darwin 881개: 통과509, 백엔드 미지원 건너뜀372, 실패0. ARM64 HVF 필수93개를 모두 실행했습니다. 집중49개는37 통과·12 건너뜀. C/CLI/보고163/163(Darwin 비교113), 변경 없는 Python 다섯 구성15.302초, 원본 네이티브21/21, 검증 스크립트66/66 통과. 독립 검토에 차단 문제는 없으며 추가 오류 우선순위 조합과 SDK 캡처 비교도 통과했습니다. 새 SDK 검사의 StringExtras 헤더 누락으로 한 번 컴파일 실패 후 추가하여 성공했고 원본 로그·소스를 보존했습니다. 잘못된 길이 포인터의 네이티브 실험도 보존하며 명시적으로 지원 범위 밖입니다. 통과 후 파일 머리말 주석 두 개만 정리하고 재빌드했습니다. 집계는 중복, 시간 제한은 그대로이며 실행 실패 재검사는 없었습니다. 전체 GitHub CI·iOS 실기기는 별도, Intel HVF Actions는 중단 상태입니다.

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## 벡터 파일 I/O와 출력 캡처

`readv`/`writev`, `preadv`/`pwritev` 및 nocancel 진입점은 스칼라 파일·출력 캡처 구현을 공유합니다. 새 설정이나 호스트 접근은 없습니다. LP64 iovec는 8바이트 주소와 8바이트 길이이며, iovcnt의 부호 있는 하위32비트는1–1024입니다. 설명자 조회 전에 전체 배열을 복사하여 출력 별칭이 요청을 바꾸지 못합니다. 일부만 읽을 수 있는 배열은 미지원입니다.

접근 권한과 스트림 위치 지정 여부를 먼저 검사한 뒤 각 길이와 합계를 INT64_MAX 이하로 제한합니다. 일반 파일·디렉터리는 합계가 INT_MAX 이하여야 합니다. 유한 stdin은 남은 입력으로 줄이고 캡처는 출력 예산을 적용합니다. pwritev의 모든 음수 위치는 배열 전에 거부하고, preadv는 설명자·길이 뒤에 위치를 검사합니다. 빈 항목은 주소를 무시하지만 설명자·종류·위치 규칙은 유지합니다. EOF 뒤 항목은 접근하지 않습니다. 위치 지정 호출은 커서를 보존하고 pwritev는 추가 쓰기를 무시합니다. 일반 추가 쓰기는 원래 커서로 전체 요청을 한 번 자른 뒤 EOF를 선택합니다.

뒤의 완전히 잘못된 항목은 EFAULT를 반환하면서 앞서 완료한 바이트와 일반 커서 진행, 0보다 많은 바이트를 쓴 경우의 FWASWRITTEN을 보존합니다. 허용된 비어 있지 않은 파일 쓰기가 데이터 버퍼 EFAULT를 반환하면 전체 메타데이터를 무효화합니다. 인수 오류, 모델 허용 거부 및 백엔드 실패는 메타데이터를 유지합니다. 일부만 쓸 수 있는 읽기 목적지는 UnsupportedService로 중단하며 현재 항목은 복사하지 않고 이전 복사는 보존합니다. 일부만 읽을 수 있는 파일 쓰기 원본은 모든 파일 효과 전에 미지원으로 중단합니다. 권한·매핑 임대·전체 저장 예산을 먼저 검사하며, 백엔드 사전 검사나 읽기 실패는 파일·캡처 바이트를 게시하지 않습니다.

캡처는 stdout/stderr 공유 예산을 먼저 검사합니다. 사용자 주소 한계를 넘는 항목은 복사하지 않고 이전 항목만 보존합니다. 다른 부분 입력은 확인된 접두부와 EFAULT를 남깁니다. 스칼라 범위 오류는 계속 예산보다 우선합니다. 복제·리디렉션된 설명자는 원래 출력 대상을 유지합니다. 독자 `vectored-io`는 원시 macOS와5개 게스트 구성, 공개 C/CLI/Python에서8개 진입점을 검증합니다. 취소·파이프·스레드·실제 iOS 검증은 추가하지 않습니다.

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### 벡터 I/O 검증, 2026-10-06

Release Darwin 937개 등록:553개 통과, 백엔드 미제공384개 건너뜀, 실패 없음. 필수 ARM64 HVF 96개를 모두 실행했습니다. 집중 검사는45/57개 통과,12개 건너뜀. 공개 C/CLI/report 168/168개 통과, Darwin 입력 비교118개 포함. Python은20.397초에5개 구성을 검증했고 네이티브22/22, 검증 스크립트66/66 통과. 독립 검토로 희소 위치 지정 쓰기 실패 테스트를 추가해 커서·실제 EOF·메타데이터 거부·정확한 남은 용량을 확인했습니다. 첫 빌드는 기존 테스트가 삭제된 내부 조회를 참조해 실패했으며 실제 캡처 출력 검사로 바꿨습니다. 새 이벤트 단언의 optional<bool> 오용은 성공한 게스트8개를 실패로 보고했지만 수정 후 관련 검사는 모두 통과했습니다. 두 실패의 소스와 로그를 보존합니다. 수치는 중복되고 기한은 그대로입니다. 전체 GitHub CI와 실제 iOS는 별도이며 Intel HVF Actions는 계속 중지합니다.

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## 파일 존재 여부 조회

`access(33)`과 `faccessat(466)`은 현재 가상 카탈로그를 조회하며 설명자 할당이나 파일 내용·커서·플래그·메타데이터 변경을 하지 않습니다. F_OK는 기존 탐색 계약에 따른 이름의 존재를 확인합니다. 메타데이터는 카탈로그 접근 권한을 부여·취소하지 않으며, 이 조회는 원시 상위 디렉터리 검색 권한·ACL·MAC 검증이 아닙니다. stat 관측값이 없거나 무효여도 조회할 수 있습니다. 이전 FD나 매핑이 객체를 유지해도 삭제된 이름은 ENOENT이며 생성·이름 재사용·변경은 현재 네임스페이스를 따릅니다.

모드는 하위32비트입니다. R/W/X는 비트0–2, 확장 권한은9–21을 사용합니다. `(mode & 0x003ffe07) == 0`이면 존재 조회이며 부호 비트 등 나머지는 EINVAL로 거부하지 않고 무시합니다. 권한 요청은 조회 성공 뒤 UnsupportedService로 중단하며, 메타데이터나 변경 허가로 권한을 추측하지 않습니다. 알려진 경로·설명자 오류가 먼저입니다.

Faccessat는 AT_EACCESS(0x10), AT_SYMLINK_NOFOLLOW(0x20), AT_SYMLINK_NOFOLLOW_ANY(0x800) 하위 플래그의 모든 조합을 허용합니다. 나머지는 카탈로그가 없어도 경로·FD 접근 전에 EINVAL입니다. 실제/유효 ID는 고정이며 고정 링크는 아래 해석 정책을 사용합니다. 절대 경로는 dirfd를 무시하며 상대 경로는 설정된 CWD/디렉터리 FD 규칙을 유지합니다. 비 AT_FDCWD nameiat는 첫 바이트/상대 디렉터리 FD를 검사한 뒤 전체 문자열을 읽습니다. `/`는 FD 검사를 생략합니다. 첫 바이트 오류는 EFAULT14, 상대 미지/파일 FD는 후속 오류보다 먼저 EBADF9/ENOTDIR20입니다. 빈 상대 경로에서도 미지 FD는 EBADF, 일반 파일 FD는 ENOTDIR, 그 외에는 ENOENT입니다. 미설정 카탈로그와 미지 스트림 디렉터리 종류는 미지원입니다.

독자 `file-access`는 네이티브 macOS와5개 게스트, C++/C/CLI/Python에서 두 진입점·무시 비트·플래그·순서를 비교합니다. NOFOLLOW_ANY는 상대 디렉터리 FD로 시험해 호스트 `/tmp`, `/var` 링크의 영향을 피합니다. 직접 테스트는 이름 변경, 혼합 권한 비트, FD 고갈, 메타데이터 독립성과 메모리 실패를 다룹니다.

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### 파일 존재 여부 검증, 2026-10-06

Release Darwin 971개 등록:575개 통과, 백엔드 미제공396개 건너뜀, 실패 없음. 필수 ARM64 HVF 99개를 모두 실행했습니다. 집중35개 중23개 통과·12개 건너뜀이며 직접14개를 포함합니다. 공개 C/CLI/report 173/173개 통과, Darwin 입력 비교123개 포함. Python은16.235초에5개 구성을 검증했고 네이티브23/23, 검증 스크립트66/66 통과. 독립 설계·구현 검토에 차단 문제는 없습니다. 호스트 /tmp 링크로 인한 초기 NOFOLLOW_ANY 결과와 정규 경로 비교를 보존하며 공통 작업은 상대 디렉터리 FD를 사용합니다. 수치는 중복되고 기한은 동일하며 실행 실패 재검사는 불필요했습니다. 전체 GitHub CI와 실제 iOS는 별도이며 Intel HVF Actions는 계속 중지합니다.

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## 디렉터리 생성과 삭제

`mkdir(136)`과 `mkdirat(475)`는 명시적으로 변경 가능한 바로 위 부모에 디렉터리를 만듭니다. 새 디렉터리는 네임스페이스 변경 권한을 상속하고 초기 디렉터리는 별도 허가를 유지합니다. 알려진 부모 장치/GID만 상속하며 전체 stat·크기·할당·시간·열거 커서는 추측하지 않습니다. 재사용한 파일 이름의 이전 관측을 가립니다. `creation_policy`는 일반 파일 전용으로, 하위 파일은 상속된 부모 식별과 기존 전역 inode 순서를 사용하고 mkdir는 파일 inode를 소비하지 않습니다. 권한 강제와 원시 디렉터리 메타데이터는 범위 밖입니다.

공통 구성요소 탐색에서 mkdir는 끝 슬래시만 뒤따르는 누락 이름을 생성합니다. 누락 조상 뒤 점/두 점은 ENOENT, 파일 조상은 ENOTDIR, 기존 이름은 EEXIST입니다. 상대 FD/CWD, 절대 경로의 FD 무시, 문자열 실패 우선순위를 유지합니다. 사용 가능한 FD가 필요 없고 탐색·허가·예산·전송 거부는 이름이나 부모 관측을 변경하지 않습니다.

`rmdir(137)`와 AT_REMOVEDIR(0x80)를 준 `unlinkat(472)`는 이 프로세스가 생성한 빈 디렉터리를 제거하며 AT_SYMLINK_NOFOLLOW_ANY(0x800)를 함께 쓸 수 있습니다. 미지 하위32비트는 입력 전에 EINVAL, DATALESS와 SYSTEM_DISCARDED는 미지원입니다. 알려진 경로/종류/루트 오류는 유지하고 removable 허가 없는 초기 디렉터리 삭제는 여전히 UnsupportedService입니다. 허가된 대상의 끝 점은 EINVAL, 연결된 디렉터리에서의 두 점/비어 있지 않은 대상은 ENOTEMPTY입니다. 디렉터리 FD(dup 포함)와 CWD는 원래 객체를 유지하며 삭제를 막지 않습니다. unlink된 일반 파일 FD/매핑은 이름에 포함하지 않으며 부모 삭제와 재사용 후 내용·inode·마지막 F_GETPATH가 유지됨을 네이티브로 비교합니다.

새 정규 경로+NUL과1개 항목을 공통16 MiB/256개 예산에 합산하고 삭제 후 모든 참조가 없어질 때 해당 비용만 반환합니다. 고아 파일·매핑은 유지합니다. 성공할 때만 부모의 전체 stat/열거를 무효화하며 새 디렉터리 관측은 계속 미지입니다. 독자 `directory-mutations`는 네이티브 macOS,5개 게스트,C++/C/CLI/Python에서 중첩 생성·이름 변경·unlink·삭제·고아 객체 재사용을 비교합니다. FD 또는 매핑 임대가 유지하는 고아 파일도 부모 디렉터리 객체를 유지합니다. 모든 디렉터리 FD를 닫아도 현재 부모 경로/NUL과 항목 비용은 남으며 파일 회수 후 부모 체인을 회수합니다. 연결된 생성 조상을 이동하면 F_GETPATH가 갱신되지만 같은 이름의 교체 객체로 소속이 바뀌지 않습니다.

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### 디렉터리 변경 검증, 2026-10-06

Release Darwin:1,017개 등록,609개 통과,백엔드 미제공408개 건너뜀,실패 없음. 필수 ARM64 HVF102개 실행. 집중58개 통과·12개 건너뜀,새4K/16K 직접26개 포함. 공개 C/CLI/report178/178(Darwin 입력128),Python5개 구성17.255초,네이티브24/24,스크립트66/66 통과. 독립 검토는 예산·이름 재사용·부모 식별·파일 임대·롤백을 확인했습니다. 초기 성공 후 추가 네이티브 검사가 슬래시 전용 루트 삭제 EISDIR와 끝 점/두 점 루트 EBUSY 차이를 발견해 공통 판단과 테스트를 수정했습니다. 초기 결과·소스·바이너리는 보존합니다. 수치는 중복,기한 불변,Intel HVF Actions 중지 유지. 전체 GitHub CI와 실제 iOS는 별도입니다.

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## 유지된 디렉터리 식별

삭제된 디렉터리의 FD/CWD는 원래 부모 체인을 유지하며 이름 재사용으로 다른 객체에 연결되지 않습니다. 점 open은 독립 커서,dup는 공유 커서,두 점은 원래 부모를 사용합니다. 삭제된 디렉터리의 일반 자식은 ENOENT입니다. LOOKUP은 보존된 삭제 부모를 통과하지만 생성/삭제/이름 변경 탐색은 ENOENT입니다. 이름 변경의 끝 점/두 점은 해당 구성요소 탐색 전에 EINVAL이며 앞선 조상 오류가 우선합니다. F_GETPATH는 마지막 경로를 유지하고 전체 stat/열거는 미지입니다. 경로+NUL과 한 항목 비용은 FD/CWD/이전 자식 참조가 모두 해제될 때까지 유지합니다. close/dup2/CWD변경/수정 허용 시 도달 불가 체인을 회수하며 초기 비용과 파일 임대는 별개입니다. 독자 `deleted-directories`는 네이티브 macOS와5개 구성에서 보유 중 삭제·부모와 이름 재사용·탐색 의도·CWD 단독 유지를 비교합니다. FD 또는 매핑 임대가 유지하는 고아 파일도 부모 디렉터리 객체를 유지합니다. 모든 디렉터리 FD를 닫아도 현재 부모 경로/NUL과 항목 비용은 남으며 파일 회수 후 부모 체인을 회수합니다. 연결된 생성 조상을 이동하면 F_GETPATH가 갱신되지만 같은 이름의 교체 객체로 소속이 바뀌지 않습니다.

### 디렉터리 수명 검증, 2026-10-06

Release Darwin1,051개,631통과,백엔드 미제공420건너뜀,실패 없음;필수 ARM64 HVF105개 실행. 집중98통과·12건너뜀,초기 직접64/64(신규14개와 보유 삭제 갱신). C/CLI/report183/183,Darwin133개;Python5구성18.691초,네이티브25/25,스크립트66/66. 추가 원시 이름 변경 검사로 끝 점 오류 순서를 수정하고 초기 소스·결과·스냅샷을 보존했습니다. 주 에이전트가 증거를 대조했으며 최종 독립 검토는 이용 불가했습니다. 수치 중복,기한 불변,전체CI/실제iOS 별도,Intel HVF Actions 중지 유지.

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## 명시적으로 허용한 초기 디렉터리 제거

디렉터리 항목의 엄격한 Boolean `"removable": true`(C++ `DarwinFileOptions::RemovableDirectories`)는 이름 공간에서 하나의 정체성을 가지며 마운트가 아닌 일반 디렉터리를 선언합니다. 루트가 아닌 명시적 초기 `directories` 항목이어야 하며 직접 부모에도 명시적 수정 허가가 필요합니다. 알려진 특수 모드/플래그, 스냅샷을 포함한 inode 별칭, 부모와 대상의 알려진 장치 번호 충돌은 거부합니다. 같은 장치 번호만으로 마운트가 없다고 증명하지 않습니다. 생략/false는 미지원이며 다른 JSON 타입은 잘못된 입력입니다. 일반 권한이나 마운트 모델을 제공하지 않습니다.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

삭제하려면 현재 이름 공간이 비어 있어야 합니다. 초기의 암시적 자식 디렉터리는 마지막 원본 파일을 unlink해도 남습니다. 성공하면 대상과 직접 부모의 전체 stat/열거 관측을 무효화하지만 이전 FD/dup/CWD는 원래 객체와 부모 체인을 유지합니다. 불변 입력에서 삭제된 이름을 되살리지 않습니다. 같은 이름의 새 파일/디렉터리는 별도 정체성을 가지며 이전 메타데이터나 스냅샷을 상속하지 않고 호출자 입력도 바꾸지 않습니다.

removable 참조마다 경로와 NUL을 초기 16 MiB 비용에 더합니다. 초기 항목·경로·참조·스냅샷 비용과 256개 제한의 초기 항목은 삭제와 마지막 close 후에도 반환하지 않습니다. 새 객체의 동적 비용은 별도입니다. 독자 `initial-directory-removal`은 네이티브 시험의 기존 빈 디렉터리를 보유 중 삭제하고 같은 이름의 파일, 다음으로 디렉터리를 만들어 CWD 단독 보유를 확인한 뒤 빈 디렉터리를 복구합니다. C++/C/CLI/Python의 다섯 게스트 조합에서 같은 프로그램을 실행합니다.

### 초기 디렉터리 삭제 검증, 2026-10-06

최종 Release 소스로 Darwin 등록 1,089개를 대조했다. 657개 통과, 백엔드 사용 불가로 432개 건너뜀, 실패 0개이며 필수 ARM64 HVF 108개를 모두 실행했다. 집중 검증은 27/39개 통과와 사용 불가 12개 건너뜀이며, 추가한 스냅샷만의 별칭 검사도 통과했다. 공개 C/CLI/보고서 191/191, Python 5개 조합 76.276초, 독자적인 네이티브 작업 26/26, 증거 실행기 66/66이 통과했다. 처음에 모순된 inode/스냅샷 테스트 입력은 수정했고 실패 기록은 보존했다.

앞선 전체 실행 두 번에서는 기존 파일/이름 변경 사례에 시간 초과가 각각 1개와 3개 있었다. 진단 실행에서도 실제 시간 5.008초, 프로세스 CPU 시간 0.171초의 파일 시간 초과를 재현했다. 동일 메서드와 이전 프로그램 비교는 통과했지만 지연 원인은 아직 밝혀지지 않았으며, 최종 통과가 시간 초과 안정성을 입증하지는 않는다. 임시 진단을 제거하고 프로그램 파일 해시를 복원했으며 원래 게스트 5초 제한을 유지했다. 주 담당자가 소스/증거를 검토했고 독립 검토는 불가능했다. 수치는 겹친다. 전체 GitHub CI, 실제 iOS 기기, 중단된 Intel HVF Actions는 이 로컬 검증 범위 밖이다.

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## 일반 파일의 배타적 이름 변경

원본과 대상 조회 후 RENAME_EXCL은 서로 다른 기존 파일이나 디렉터리에 EEXIST를 반환하며 마운트 및 이름 공간 변경 검사보다 앞섭니다. 마지막 점/두 점의 EINVAL 등 앞선 경로 오류는 계속 우선합니다. 대상이 없으면 기존의 제한된 이름 변경 트랜잭션을 사용하며 열린 설명, 커서, 플래그, 매핑 임대 및 설정된 메타데이터 변경을 보존합니다. 동일 객체의 배타적 이름 변경은 파일 시스템의 대소문자 구분 여부에 의존하므로 명시적으로 미지원입니다. 정확한 카탈로그 키로 이 속성을 추정하지 않습니다. 대소문자 통합, 초기 디렉터리 이동, SECLUDE은 범위 밖입니다. 기존 독자적 `renamed-file` 작업은 거부 시 메타데이터 유지와 EXCL|NOFOLLOW_ANY 성공을 네이티브 macOS 및 C++/C/CLI/Python에서 비교합니다.

검증, 2026-10-06(Release): Darwin 등록 1,097개, 통과 665개, 사용 불가로 432개 건너뜀, 실패 없음. 필수 ARM64 HVF 108개를 모두 실행했다. 집중 검증은 새 직접 검사 8개를 포함해 44개 통과, 12개 건너뜀이었다. 공개 C/CLI/보고서 191/191, Python 5개 조합 19.241초, 독립 원시 호출 탐침 26개 통과. 첫 전체 네이티브 실행은 기존 return에서 시간 초과가 발생했고 renamed-file을 포함한 나머지 25개는 통과했다. 변경하지 않은 동일 바이너리의 return 재확인 3회는 0.014–0.034초였고 이후 전체 26개가 원래 5초 제한으로 통과했다. 최초 실패는 보존했고 원인은 미해결이며, 이전 최종 HVF 통과까지 포함해 지연 안정성을 입증하지 않는다. 주 담당 검토 완료, 독립 검토 불가. 수치는 겹친다. 실제 iOS, 전체 GitHub CI, 중단된 Intel HVF Actions는 로컬 검증 범위 밖이다.

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.


## 생성한 디렉터리 간 이름 변경

초기 디렉터리와 이 프로세스가 mkdir/mkdirat으로 생성한 모든 자손은 같은 가상 이름 공간 영역을 공유합니다. `rename`, `renameat`, `renameatx_np`는 이 부모들 사이에서 일반 파일을 이동하며 새 JSON 필드가 필요 없습니다. 변경 가능한 초기 `/work` 아래 `/work/left`와 `/work/right`를 만든 뒤 `/work/data`를 자식으로, 자식 사이로 이동할 수 있습니다. 별도 초기 입력으로 선언한 `/work/left`는 device가 같아도 다른 영역입니다. 일반적인 마운트 구조는 미상입니다.

두 직접 부모 모두 이름 변경 권한이 필요하고 생성한 디렉터리는 권한과 알려진 device/GID를 상속합니다. 파일 자체 식별자, 소유자/그룹, 쓰기 권한과 할당은 유지합니다. 알려진 장치 충돌은 거부하고 실제 이동은 두 부모의 전체 stat/열거를 무효화합니다. 삭제한 초기 디렉터리와 재사용한 경로는 다른 객체이며 옛 FD/CWD는 대체 영역을 얻지 않습니다.

기존의 제한된 덮어쓰기, 매핑 수명, 경로/NUL 비용과 오류 순서를 유지합니다. EXCL의 다른 기존 대상은 영역/권한 검사 전에 EEXIST를 반환합니다. `renamed-file`은 생성한 자식으로 이동, 초기 부모에서 덮어쓰기, 자식으로 재이동을 C++/C/CLI/Python과 네이티브 macOS로 비교합니다. 권한 강제 검사, 초기 디렉터리 이동, 하드 링크, 동적 심볼릭 링크, 네이티브 APFS 메타데이터는 미완성입니다.

### 서로 다른 부모 간 검증, 2026-10-06

최종 Release 검증은 Darwin 등록 1,115개를 대조했다. 683개 통과, 백엔드 사용 불가로 432개 건너뜀, 실패 없음이며 필수 ARM64 HVF 108개를 모두 실행했다. 직접 검사 56/56에는 새 4K/16K 사례 18개가 포함된다. 공개 C/CLI/report 191/191이 통과했고 Python 메서드는 22.254초 동안 환경 다섯 개를 검증했다. 원본 네이티브 프로그램 26/26, 별도 원시 시스템 호출 프로브 34개, 문서/기능/증거 실행기 스크립트 296/296도 통과했다. 집계는 중복된다. MSVC 전용 CMake 변경 후에도 검증 바이너리 열 개의 해시는 그대로였다.

이전 전체 검증 두 번의 기존 HVF 파일 메서드 시간 초과 세 건과 두 건은 보존했다. 전체 메서드, 작업 디렉터리 및 세션 비교는 통과했으나 원인을 확인하지 못했으므로 최종 통과가 지연 안정성을 입증하지 않는다. 프로브는 처음 /tmp와 정규 경로 /private/tmp를 비교했으며 root FD의 경로를 조회하여 기대값 네 개를 수정했다. 확장 네이티브 프로그램의 정리에 mkdir(136)을 잘못 사용한 exit150은 rmdir(137)로 수정했다. 초기 소스와 실패 증거를 보존하고 게스트 제한은 원래 5초로 유지했다.

전체 Linux CI에서 발견한 LP64 초기화 목록 충돌 네 곳은 명시적 uint64_t로 수정했고 MSVC NeverDJumpTableTests에 /bigobj를 추가했다. 실제 Linux/Windows 컴파일은 CI를 기다린다. 이전 전체 CI의 Windows EH 코퍼스 및 닫힌 PR 취소 실패는 별도 문제로 남아 있다. 주 에이전트가 소스/증거 자체 검토를 완료했으며 독립 검토, 실제 iOS 또는 중단된 Intel HVF 검증 통과를 주장하지 않는다.

`build-hvf-arm64/cross-parent-rename-validation-summary.json`, `cross-parent-rename-darwin-synced-evidence/`, `cross-parent-rename-darwin-evidence/`, `cross-parent-rename-darwin-rechecked-evidence/`, `cross-parent-rename-public-synced.xml`, `cross-parent-rename-python-synced.log`, `cross-parent-rename-native-fixed-evidence/`, `cross-parent-rename-probe/`.

## 일반 파일 이름의 원자적 교환

RENAME_SWAP=0x2는 renameatx_np로 기존 일반 파일 두 개의 이름을 교환하며 RENAME_NOFOLLOW_ANY를 추가할 수 있습니다. 명시적인 초기 디렉터리에 mutable:true와 swap_rename:true가 모두 필요합니다. C++는 DarwinFileOptions::SwapRenameDirectories를 사용합니다. 생성된 후손은 원래 디렉터리 객체의 기능을 상속합니다. 삭제 후 경로 재사용은 옛 선언을 이전하지 않습니다. false 또는 생략은 기능을 알 수 없다는 뜻이며 같은 장치와 이름 공간 권한만으로 지원을 증명하지 않습니다. 서로 다른 초기 영역은 계속 미지원입니다.

양쪽 경로는 기존 구성 요소 해석기를 사용합니다. 대상이 없으면 영역·권한·기능 검사 전에 ENOENT입니다. 디렉터리 피연산자는 명시적으로 미지원입니다. 네이티브 swap은 파일과 디렉터리도 교환하므로 일반 이름 변경의 EISDIR를 적용하지 않습니다. 동일 객체는 이름 공간 권한이 있으면 기능 선언 없이도 상태가 바뀌지 않는 무동작입니다. RENAME_EXCL=0x4 + RENAME_SWAP=0x2과 알 수 없는 flags는 경로 입력 전에 EINVAL이며 SECLUDE는 미지원입니다.

두 파일은 연결된 상태를 유지합니다. 각자의 식별, 소유자/그룹, 바이트, 쓰기 권한, 열린 설명, 커서, flags와 매핑 임대를 보존합니다. 설정된 가상 정책은 각자의 ctime만 갱신하고 정책이 없거나 무효화되면 전체 메타데이터는 계속 미상입니다. 실제 교환은 양쪽 부모의 전체 메타데이터와 열거 관측을 무효화합니다. 생성 inode, 항목, FD를 소비하지 않고 호출자 입력도 바꾸지 않습니다.

기능 참조의 경로+NUL은 고정 초기 16 MiB 예산에 예약됩니다. 트랜잭션은 양쪽 전체 동적 이름 비용을 검사한 후 게시하며 아직 연결된 바이트나 임대를 교체 회수로 사용하지 않습니다. 반복 교환은 동적 비용을 재사용합니다. 독자적인 renamed-file 프로그램은 생성된 자식으로 교환하고 되돌린 뒤 두 객체를 확인하고 일반 교체를 계속하며 네이티브 macOS와 모든 C++/C/CLI/Python 프로필에서 실행합니다. 권한 강제, 마운트 구성, 대소문자 통합 및 초기 디렉터리 이동은 별도 작업입니다.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/work","mutable":true,"swap_rename":true}]}}
```

[Apple volume swap capability](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsswaprenaming?changes=__1_2), [XNU rename](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

### 교환 검증, 2026-10-06

Release Darwin은 1,133개 등록, 701개 성공, 백엔드 불가 432개 건너뜀, 실패 없음이며 필수 ARM64 HVF 108개를 모두 실행했습니다. 직접 이름 변경 검사 68/68, 새 옵션 및 4K/16K 검사 18개 포함. 공개 C/CLI/보고서 192/192; Python 메서드 하나가 16.153초에 다섯 구성을 검증했습니다. 독자적인 네이티브 프로그램 26/26, 독립 원시 호출 탐침 45개 검사. 횟수는 겹치며 게스트 제한은 그대로입니다.

초기 직접 테스트 두 개는 알 수 없는 쓰기 권한과 변경 후 메타데이터에 잘못된 기대가 있어 기대만 수정했습니다. 첫 JSON 필터는 0개를 선택했으므로 검증에 포함하지 않고 이후 실제 테스트 소유자와 전체 공개 검사는 성공했습니다. 초기 소스와 결과는 보존됩니다. 기존 HVF/네이티브 지연은 아직 미해명이며 이번 성공이 안정성을 입증하지 않습니다. 소스/증거 자체 검토는 완료했으나 독립 검토, iOS 실기기, 전체 GitHub CI 및 중단된 Intel HVF 검증은 주장하지 않습니다.

`build-hvf-arm64/swap-rename-validation-summary.json`, `swap-rename-darwin-evidence/`, `swap-rename-public.xml`, `swap-rename-python.log`, `swap-rename-native-evidence/`, `swap-rename-probe/`.


## 프로세스가 생성한 디렉터리 이름 변경

일반 rename, renameat, renameatx_np는 한 초기 디렉터리 객체 영역 안에서 연결된 프로세스 생성 디렉터리와 하위 트리를 이동합니다. 양쪽 직접 부모의 변경 권한이 필요합니다. 없는 디렉터리 대상은 끝 슬래시를 허용하고 일반 파일 대상은 ENOTDIR, 비어 있지 않은 디렉터리는 ENOTEMPTY, 후손으로 이동하면 EINVAL입니다. 승인된 일반 같은 이름 호출은 무변경입니다. EXCL의 다른 기존 대상은 종류·순환·영역·권한보다 먼저 EEXIST이며 대상 조회 오류는 원본 점/두 점보다 앞섭니다. 같은 객체를 점/두 점으로 지정하면 대소문자 속성을 모르므로 UnsupportedService입니다. 초기/삭제된 원본, 초기 교체 대상, 다른 초기 영역, 링크, 권한 강제, SECLUDE는 미지원입니다.

문자열 접두사가 아닌 부모 객체 체인으로 후손을 선택하여 이름이 있는 트리, FD가 유지한 삭제된 자식 디렉터리와 FD/매핑이 유지한 고아 파일 경로를 갱신합니다. 원본 FD, dup, CWD, 두 점은 같은 객체와 새 부모를 따릅니다. 교체된 빈 생성 대상은 옛 경로와 부모를 유지하고 일반 자식은 ENOENT이며 점/두 점/CWD는 옛 객체를 유지합니다. 새 원본을 재이동해도 같은 경로 문자열의 옛 대상 고아는 이동하지 않습니다.

자식 파일 바이트, 정체성, 메타데이터, 쓰기 권한, 공유/독립 커서, FD 플래그와 매핑 임대를 보존합니다. 원본과 두 직접 부모의 완전한 stat/열거만 무효화되고 조상 이동만으로 자식 파일 관찰은 변하지 않습니다. 생성 디렉터리 권한은 후속 생성/승인된 파일 이름 변경에 남고 새 항목, FD, 생성 inode는 필요 없습니다.

게시 전 모든 이름 있는/유지된 후손의 새 키와 경로를 할당하고 각 정규 경로/NUL의1024바이트 경계 및 공유16 MiB를 검증합니다. 이전 동적 비용은 한 번 교체합니다. FD/CWD/유지된 후손이 없는 빈 생성 대상만 회수 용량을 제공하며 한 번만 회수합니다. 실패는 모든 이름, 부모, 관찰, 커서, 매핑을 보존합니다. 매핑만 남은 고아 파일도 삭제된 부모의 경로/항목 비용을 유지합니다.

독자적 SDK-free `renamed-directory`는 네이티브 macOS와 다섯 게스트를 C++/C/CLI/Python으로 비교합니다. 4K/16K는 객체 재사용, 전체 롤백, 정확한 용량, 긴 후손 경로, 대상 회수, 매핑 유지 부모 회수 및 항목/FD/inode 소진을 확인합니다.


### 2026-10-06

최종 Release Darwin1175개:731통과,444후단 미사용 건너뜀,실패0; 필수 ARM64 HVF111개 모두 실행. 집중32/44(12건너뜀,새4K/16K22개),최종 경계/기존 테스트4/4. 공개 C/CLI165/165,보고32/32 건너뜀 없음. Python5구성21.759초,네이티브27/27,독립 프로브79개. 독립 검토로 테스트 루트 구분자 오류와 동일 객체 원본 점의FS속성 경계를 수정 확인했습니다. 초기 게스트exit124 및 오래된 예상2개 실패와 원본/바이너리를 보존했고 최종 전체 검증은 통과했습니다. 횟수 중복,시간 제한 불변. 기존HVF/네이티브 시간 초과 원인은 미확정이며 지연 안정성 증거는 아닙니다. Intel HVF Actions는 중단,실제iOS와 전체GitHub CI는 별도입니다.

`build-hvf-arm64/directory-rename-validation-summary.json`, `directory-rename-darwin-final-evidence/`, `directory-rename-public.xml`, `directory-rename-reports.xml`, `directory-rename-python.log`, `directory-rename-native-evidence/`, `directory-rename-probe/`, `directory-rename-initial-fixture/`, `directory-rename-darwin-evidence/failed-source/`.

### 디렉터리 및 혼합 형식 교환 검증, 2026-10-07

Release Darwin 등록 1,219개를 대조했다. 763개 통과, 사용할 수 없는 백엔드로 인한 건너뜀 456개, 실패 0개이며 필수 ARM64 HVF 114개가 모두 실행됐다. 집중 검증은 32/44개 통과와 12개 건너뜀이며 새 4K/16K 직접 검증 24개를 모두 포함한다. 전체 파일 구성요소는 317/317개, 원본 네이티브 프로그램은 28/28개 통과했다. 독립 원시 시스템 호출 탐침은 이 대소문자를 구분하지 않는 macOS 파일 시스템에서 성공한 관찰 32개를 기록했다. 집계에는 중복이 있으며 게스트 제한 시간은 그대로다.

독립 계획 및 구현 검토에서 양방향 트랜잭션, 초기 파일 경로 비용, 양쪽 보존 하위 트리, 매핑만 유지하는 고아 객체와 정확한 반환을 확인했다. 최초 실패 테스트는 명시적 루트 교환 선언이 없어 인수 근거에서 제외했다. 수정한 이전 구현 기준에서는 선택한 6개가 모두 디렉터리 교환 거부로 실패했다. 초기 혼합 파일 단언 2개는 설정된 메타데이터를 잘못 버렸으며 현재는 ctime만 변경된 전체 레코드를 비교한다. 지역 상수 포인터 배열이 ARM64 재배치를 만들어 로딩 6개를 거부하게 했다. 스칼라 단언 4개로 교체한 프로그램에는 기존 방식의 재배치가 없고 로더는 미지원 fixups를 계속 명시적으로 거부한다.

수정 프로그램의 최초 집중 실행에서 5초 HVF 시간 초과 3개를 보존했다. 개별 및 세 프로필 대조 검사와 이후 전체 검증은 통과했지만 원인은 아직 알 수 없고 지연 안정성을 증명하지 않는다. 초기 소스, 바이너리, 실패 및 대조 기록은 보존된다. 초기 디렉터리 이동, 별도 초기 도메인, 권한/마운트/대소문자 선언, 동적 의존성과 프레임워크 실행 환경은 미완성이다. 실제 iOS 기기, 중단된 Intel HVF와 전체 GitHub CI는 별도 인수 경계다.

`build-hvf-arm64/directory-swap-research/`: `darwin-final-evidence/`, `darwin-evidence/`, `focused-v5.xml`, `file-owner-v5.xml`, `native-workload-fixed-evidence/`, `native-probe-v2-results.json`, `fixture-fixups-before/`, `hvf-controls.json`.

최종 링크된 바이너리로 전체 Darwin 검증을 다시 통과했다. 고정 dev 0a9a1d28d 통합 검증은 공통 구성요소 20그룹의 4,515개를 기록했다. 4,489개 통과, 선택적 Z3 6개 및 없는 Windows EH 코퍼스 20개 건너뜀, 실패 0개다. 이 집계에 C/CLI 170/170과 보고서 32/32가 포함된다. Python은 30.780초에 다섯 프로필을 모두 검증했다. 네이티브 모바일 아키텍처/fixup 변형 12개 모두에서 원본 프로그램 관찰 4,532개가 일치했고 단일 세션 및 전체 Swift 메타데이터 일치는 12/12다. Swift witness 생성기도 주석 들여쓰기를 수정한 뒤 기록된 SDK/컴파일러로 카탈로그를 재현했다. 이는 로컬 Release LLVM 23/Apple Clang 17 인수이며 이후 dev 또는 Linux Clang 18 인수를 의미하지 않는다.


## 명시한 초기 디렉터리 하위 트리의 일반 이동

명시적인 초기 비루트 디렉터리의 엄격한 Boolean `"movable": true`는 해당 루트의 일반 rename을 허용하고 초기 하위 트리 전체가 고유한 이름의 일반 비마운트 디렉터리임을 선언합니다. C++ `DarwinFileOptions::MovableDirectories`는 기존 aggregate 멤버 뒤에 추가합니다. 바로 위 부모는 mutable이어야 합니다. 생략/false는 미상이며 다른 JSON 타입은 거부합니다. 후손의 mutable/removable/movable 및 파일 쓰기 권한은 그대로 별개입니다. 초기 디렉터리 SWAP, 일반 권한 판단이나 마운트를 허용하지 않습니다. 알려진 flags, 특수 디렉터리 모드, 다중 링크 일반 파일, stat/스냅샷의 inode 별칭을 거부합니다. 선언으로 연결한 비마운트 영역 전체에는 알려진 장치 번호가 하나만 있어야 합니다. stat 없는 공통 조상의 형제 트리와 파일도 포함하며, 장치 번호 일치만으로 별도 영역을 연결하지 않습니다.

이름, 부모, 원래 stat/스냅샷, 권한은 객체가 소유합니다. 이전 입력 경로로 이동·삭제된 이름을 복원하지 않습니다. 열지 않은 후손, FD/dup/CWD, 매핑, 삭제된 후손도 원래 객체를 유지합니다. 변경되지 않은 후손은 stat, 스냅샷, cookie와 SEEK_END를 유지하고 이동 루트와 이름이 변경된 부모의 전체 관측은 미상이 됩니다. 옛 이름 재사용은 관측이나 권한을 상속하지 않습니다. 초기 입력은 한 번의 동기 실행을 위한 선언이며 실행 중 카탈로그 교체 API가 아닙니다. SWAP 능력은 별개입니다. 초기 객체는 직접 swap_rename 선언을 유지하고 mkdir은 부모 능력을 복사하며 이동은 이를 다시 계산하지 않습니다. 양쪽 SWAP 부모 모두 지원 선언이 필요합니다.

빈 초기 대상 교체에는 별도 removable이 필요합니다. 이름 공개 전에 모든 경로의 1023바이트 한도와 공유 16 MiB 예산을 검사합니다. movable 참조는 원래 경로와 NUL을 고정 예약하며 항목을 늘리지 않습니다. 초기 디렉터리 경로·참조·스냅샷·항목은 삭제 후에도 예약됩니다. 동적 PathCharge는 0에서 시작해 이동할 때 연결되거나 보존된 각 객체의 현재 경로를 한 번 청구합니다. 즉시 해제 가능한 대상의 기존 동적 비용만 공제합니다. FD/CWD/자식/고아 매핑 보존은 공제 근거가 아니며 최종 회수는 한 번만 환급합니다. 암묵적 초기 조상은 256항목 한도에 추가하지 않고 초기 일반 파일 항목 회수는 유지합니다.

예:

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/work","mutable":true,"movable":true}]}}
```

SDK 없는 initial-directory-move는 이동, 공유/독립 커서, FD flags, CWD, 사설 매핑, 교체와 원래 이름 복원을 확인합니다. 로컬 guest 설정과 실제 iOS 비교는 별도 검증입니다.

초기 디렉터리 SWAP 미지원은 시스템 호출의 원본 또는 대상 루트 객체를 뜻합니다. 생성한 조상을 교환하면 그 아래로 이동한 초기 자손도 객체 상태와 동적 비용을 유지하며 함께 이동합니다. 앞서 설명한 초기 디렉터리 제한은 필요한 선언이 없는 경우에 적용됩니다.

최종 독립 검토 후 ARM64 macOS 검증은 Darwin 1,264개 중 796개 통과, 사용할 수 없는 백엔드 468개 건너뜀, 실패 0개입니다. 필수 ARM64 HVF 117개는 모두 실행했습니다. 파일 소유 계층은 342/342이며 새 4K/16K 동작 22개와 승인 조건 3개를 포함합니다. CreationPolicy는 이동한 부모의 원래 Device/GID를 유지하고 이름 재사용, inode 순서, 현재 umask를 구분합니다. 정확한 16 MiB 한도에서 16회 왕복 교환은 비용을 누적하지 않으며 원위치 이동은 실제 6바이트 차이만 반환합니다. 공개 C/CLI 175/175, 보고서 파싱 33/33, 네이티브 커널 29/29, 독립 원시 프로브 19개 관측이 통과했습니다. Python 5개 설정은 27.865초에 통과했고 순수 API 71개, SDK 차이, runner 49개도 통과했습니다. 집계는 중복됩니다. 소스, 바이너리, 실패 시도, 최종 결과는 build-hvf-arm64/initial-directory-move/에 보존하고 커밋에 연결합니다. 실제 iOS, 중단된 Intel HVF, 초기 루트 SWAP, 권한·마운트·대소문자, 공유 매핑 EOF, Mach·스레드·dyld와 프레임워크 런타임은 별도 미완료 범위입니다.

## 선언한 초기 디렉터리 루트의 원자 교환

엄격한 Boolean exchangeable:true(C++ DarwinFileOptions::ExchangeableDirectories, aggregate 끝에 추가)는 명시한 비루트 초기 디렉터리만 RENAME_SWAP 루트 피연산자로 허용합니다. 바로 위 초기 부모는 mutable이어야 합니다. 생략/false는 미지원, 다른 타입은 무효입니다. movable과 일반 비마운트·유일 이름 자손 선언 및 flags/특수 모드/별칭/하드 링크/연결 성분 전체 장치 검증을 공유하지만 두 선언의 합집합은 토폴로지만 정합니다. 각각 원래 경로+NUL을 고정 예약하고 둘 다 선언하면 둘 다 계산합니다. 엔트리는 늘지 않으며 장치 일치만으로 영역을 연결하지 않습니다.

일반/EXCL 초기 원본은 movable, 일반 초기 교체 대상은 removable이 별도로 필요합니다. exchangeable은 이 권한, 자손 mutable, 파일 쓰기, 일반 권한/마운트를 주지 않습니다. 다른 객체 교환은 양쪽 실제 부모가 mutable이며 각각 swap_rename을 지원해야 합니다. 허용된 일반 구성요소의 같은 이름 SWAP는 부모/장치를 확인한 뒤 변화가 없고 최초 요금이나 다른 객체용 교환 능력이 필요하지 않습니다. 같은 객체 dot/대소문자는 여전히 미지이며 없는 대상과 dot 순서는 유지됩니다.

초기 비어 있지 않은 루트 쌍, 초기/생성 루트, 디렉터리/파일을 양방향 교환합니다. 양쪽 연결·보존 하위 트리를 모두 검사한 뒤 모든 이름을 뽑고 공개하며 두 루트는 연결됩니다. 교체/내용 공제나 FD/inode/엔트리 생성이 없습니다. FD/dup/CWD/커서, 부모 객체, 매핑 lease, 권한은 원래 객체를 따르고 변경 없는 자손은 stat/스냅샷을 유지합니다. 같은 이름의 삭제 객체와 새 객체는 분리됩니다. 동적 경로 비용은 처음 0, 최초 교환에 현재 경로를 한 번 계산하며 다음은 이전 비용을 대체합니다. 고정 비용은 반환하지 않으며 경로/예산 실패는 양쪽 상태를 유지합니다.

SDK 독립 original initial-directory-swap은 기존 empty와 data를 교환하고 복원하며 자손, 매핑, CWD, 커서, FD flags, 이동 후 생성과 정리를 확인합니다. 앞의 f98068c07은 별도로 고정한 일반 이동 검증이며 초기 루트 SWAP는 이 선언으로만 확장합니다.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/left","exchangeable":true},{"path":"/right","exchangeable":true}]}}
```


이번 macOS ARM64 Release 검증은 Darwin 등록 1,303개, 통과 823개, 백엔드 사용 불가 건너뜀 480개, 실패 0개이며 필수 ARM64 HVF 120개를 모두 실행했다. 파일 검사 361/361(새 4K/16K 동작 16개와 입력 승인 검사 3개), C/CLI 180/180, 보고서 파싱 34/34, 원본 커널 프로그램 30/30, 독립 프로브 관측 35개가 통과했다. Python 5개 구성은 33.894초에 통과했고 순수 API 71개, 목록/대조 단위 검사 49개, SDK 차이, 형식, 기능, 출처, 문서 검사도 통과했다. 집계는 중복된다.

정확한 용량 경계에서 같은 이름 작업과 16회 왕복 교환은 원래 객체와 비용을 유지한다. 6바이트가 필요한데 5바이트만 남으면 양방향 거부가 두 트리, 커서, 후속 생성 예산을 모두 보존한다. 독립 계획 및 최종 소스 검토는 승인되었다. 시도, 소스, 바이너리, 결과를 `build-hvf-arm64/initial-directory-swap/`에 보존하고 커밋에 연결한다. 잘못 겹친 보고서 실행은 제외하고 순차 재실행했으며 번역 마커를 동기화했다. 시간 제한과 부정 대조를 완화하지 않았다. 권한, 선언하지 않은 마운트/대소문자, 공유 매핑/EOF, 진행 시계, Mach/스레드/dyld/프레임워크는 미완료다. 실제 iOS, 중단된 Intel HVF, 원격 병합 CI는 별도로 검증한다.

## 명시적 읽기 전용 리소스 제한 관측값

다섯 Darwin guest 구성의 `getrlimit(194)`는 `DarwinSystemOptions::ResourceLimits` (`darwin_system.resource_limits`)를 읽습니다. 키 0..8의 `DarwinResourceLimit`는 오프셋 0/8에 두 little-endian uint64, 총 16바이트입니다. `0 <= current <= maximum <= 9223372036854775807`이어야 하며 0은 명시값, INT64_MAX는 무한입니다. 호스트 조회나 FD/VM/저장/실행 예산 변경은 없습니다. `setrlimit`, 제한 집행, 신호, 스케줄링은 미완성입니다.

엄격 JSON은 최대 아홉 고유 키와 정확히 `resource`, `current`, `maximum`만 허용하며 정확한 정수 또는 부호 없는 십진 문자열을 씁니다. 잘못된 형식/필드, 중복, 비정규 키, 역전 값은 로드 전에 실패합니다. 빈/생략 배열은 미상입니다. 구성 키에는 syscall 플래그나 절단 규칙을 적용하지 않습니다.

syscall만 선택자의 하위 32비트를 취하고 `_RLIMIT_POSIX_FLAG=0x1000`를 지웁니다. 잘못된 리소스는 메모리 접근 전 EINVAL, 누락값은 출력 접근 전 unsupported입니다. 전부 쓰기 불가이면 EFAULT, 일부만 가능한 쌍은 모든 바이트를 보존하고 unsupported입니다. 완전한 비정렬/페이지 경계 복사는 16바이트만 변경하며 backend 오류는 전송 오류입니다. 네이티브 ARM64 probe는 값/선택자 23개와 독립 fault 4개를 통과했습니다. 이 호스트의 부분 prefix 불변을 보편 보장으로 확대하지 않습니다.

```json
{"darwin_system":{"resource_limits":[{"resource":8,"current":256,"maximum":"9223372036854775807"}]}}
```

macOS ARM64 Release 등록/통과/미실행 skip/실패: 1337 / 845 / 492 / 0, 필수 HVF 123개 모두 실행. 새 4K/16K 동작 검사 열두 개 및 SDK capture oracle, C/CLI 190/190, parser 37/37, native workload 31/31 통과. Python 다섯 구성 71.978초, 순수 API 71개와 runner 49개 통과; SDK drift, 형식, 기능, 출처, 문서 검사 통과. 수치는 겹칩니다. 증거는 `build-hvf-arm64/resource-limit-observations/`에 동결하고 커밋에 연결합니다. 초기 테스트 enum 컴파일 오류 및 연결/필터 시도를 보존하며 수정 후 직렬 검증에서 기한과 음성 대조를 완화하지 않았습니다. 제한 집행, 권한, 변경 후 디렉터리 정보/열거, 공유 map/EOF, 진행 시계, Mach/thread/dyld, framework는 미완성입니다. 물리 iOS, 중단된 Intel HVF와 원격 merge CI는 별도 검증이 필요합니다.

## 명시적 읽기 전용 리소스 사용량

다섯 Darwin guest 구성에서 getrusage(117)는 독립적인 선택 항목 DarwinSystemOptions::ResourceUsageSelf / ResourceUsageChildren을 읽습니다. 엄격 JSON: darwin_system.resource_usage.self / .children. 각 DarwinResourceUsage는 int64 user_seconds/system_seconds, 1000000 미만 uint32 user_microseconds/system_microseconds, 정확히 14개 int64 counters를 요구합니다. 정확한 정수나 부호 있는 십진 문자열은 전체 범위를 보존하며 잘못된 형식/필드/마이크로초/길이는 로드 전에 거부합니다. 누락된 상대는 미상이고 제공된 항목 조회를 막지 않습니다. 모두 0인 명시는 유효합니다.

한 번에 144바이트 little-endian 출력: timeval 0/16에는 8바이트 초, 4 마이크로초, 4 zero padding; counter는32부터 각8바이트. Darwin 원값/단위를 보존하며 ru_maxrss에 Linux KiB 변환은 없습니다. 고정값은 호스트 성능 측정, 회계, fork/wait, 스케줄링, 제한 집행을 구현하지 않습니다.

선택자 하위32비트만 사용해 0=SELF, -1=CHILDREN. 0x1000은 잘못된 값이며 POSIX flag 제거 없음. 잘못된 값은 메모리 전 EINVAL, 누락값은 출력 전 unsupported. 완전한 비정렬/페이지 경계 복사는 guard 보존; 모두 쓰기 불가 EFAULT, 일부 쓰기 가능은 복사 전 unsupported, backend 오류는 전송 오류입니다. SDK는 선택자별 한 번 캡처만 비교해 변화하는 후속 SELF를 피합니다. native probe 13개와 독립 fault4개는 기존5초로 통과. 이 호스트 partial SELF는 EFAULT 전64바이트 쓰기를 관측했으며 범용 prefix 보장으로 확대하지 않습니다.

0..13: `ru_maxrss`, `ru_ixrss`, `ru_idrss`, `ru_isrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`, `ru_inblock`, `ru_oublock`, `ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`, `ru_nvcsw`, `ru_nivcsw`.

```json
{"darwin_system":{"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":0,"system_microseconds":0,"counters":["9223372036854775807",0,0,0,0,0,0,0,0,0,0,0,0,0]}}}}
```

[Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html), [XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [SDK layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

macOS ARM64 Release 등록/통과/미실행 skip/실패 1371/867/504/0, 필수 HVF 126 전부 실행. File361/361, C/CLI 200/200, JSON 40/40, System/SDK 53/53, native 32/32 통과. 새4K/16K12개 및 입장/SDK 검사, Python5구성 42.865초; API71, runner/reference49, SDK drift/형식/기능/출처/문서 검사 통과. 수치는 겹치며 독립 검토 통과. 증거 `build-hvf-arm64/resource-usage-observations/`를 동결해 commit에 연결합니다. 초기 x64 EINVAL fixture는 RDX보존, ARM64 X1초기화로 수정했고 실패/빈 filter 기록 보존. runtime/기한/음성대조 불변. 집행, 권한, 변경 후 디렉터리, shared map/EOF, 시계, Mach/thread/dyld, framework 미완성. 물리iOS, 중단Intel HVF, remote merge CI 별도 검증입니다.

첫 전체 검증866통과, 기존iOS ARM64 HVF rename5초timeout1개,504skip. 같은 binary의 해당 case386ms통과 후 전체 직렬 검증통과. 두 기록 보존, 원인미확정이며 latency보장 아님.


## 명시적 자격 정보와 그룹 및 생성 소유자 일관성

선택Credentials는RealUID/EffectiveUID/RealGID/EffectiveGID와 독립 선택GroupAccessList입니다. 생략하면 네조회1000, 명시0/root 유효, ID0..INT32_MAX. 그룹1..16, 첫항EffectiveGID, 순서/중복보존. 누락은 알수없음이며host/EGID로 채우지 않습니다. 엄격darwin_system.credentials는real_uid/effective_uid/real_gid/effective_gid 필수, groups선택. 무손실 정수/중앙검증은 모양/필드/범위/수/첫그룹 오류를 로드전 거부하며 비Darwin도 거부합니다.

getuid24/geteuid25/getgid47/getegid43/getgroups79는 단일system소유자입니다. 새일반파일UID는 유효UID, device/GID는 직계부모상속. rename/보존FD/이름재사용은 객체 유지, 입력stat불변. root는 쓰기/디렉터리변경/권한/ACL을 부여하지 않으며 setuid/setgid/setgroups와process/session 미구현입니다.

getgroups용량은 하위32bit 부호int입니다. 음수먼저EINVAL, 미설정unsupported, 알려진0은포인터접근없이count, 양수부족은메모리전EINVAL, 충분하면4*count작은엔디언byte한번복사. 0x1000은양수, POSIXflag제거없음. 비정렬/페이지경계guard보존, 전부쓰기불가EFAULT, 부분은byte전unsupported, backend오류는transport. BSD오류는x64 RDX보존/ARM64 X1소거, 성공은둘다secondary소거, 보고는raw인자보존.


~~~json
{"darwin_system":{"credentials":{"real_uid":101,"effective_uid":202,
 "real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}}}
~~~


[Apple getgroups contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getgroups.2.html), [pinned XNU credential/group ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c).

등록/통과/불가skip/실패: 1413/897/516/0; ARM64 HVF 129/129. System/SDK72/72, File365/365, Report43/43, C/CLI210/210; guest8/12skip; native33/33; Python5 82.918s; API71, runner/reference49. SDK drift / clang-format22.1.2 / capabilities23 / provenance / docs11+negative3: OK. Independent source review: OK.

21 value +5 native fault,5s; host16groups, positive short capacity: OK. Native partial32byte thenEFAULT: observation only. Counts overlap. `build-hvf-arm64/credential-observations/`.

첫8실패（명령예산5, 5초deadline3）、12skip. 새fixture2페이지scan을 같은경계132byte guard（앞64, 최대data64, 뒤최소4）로제한. 직접owner는전체2페이지검증. 예산/인자/오류음성대조불변, source/binary/두run보존. 사전검토로transport경계수정, public설정내용비교. 권한/ACL、link、변경후directory전체관측、shared maps/EOF、clock、Mach/thread/dyld/framework미완；실물iOS、정지Intel HVF、merge CI별도.

## 명시적 프로세스와 커널 한도로 결정되는 기술자 표 조회

선택 `DarwinSystemOptions::MaxFilesPerProcess` / `darwin_system.max_files_per_process`는 음수가 아닌 int 관측값입니다. 누락은 미지정, 명시적 0은 유효합니다. 이름 `kern.maxfilesperproc`와 숫자 MIB `[1,29]`는 자원 제한과 독립적으로 같은 4바이트 값을 읽습니다. 무손실 정수 파싱과 중앙 검증은 잘못된 형식, 음수, 범위 초과를 로드 전에 거부하며 비 Darwin 설정도 거부합니다.

BSD `getdtablesize(89)`는 이 상한과 `ResourceLimits[8].Current`를 모두 요구하고 작은 값을 반환합니다. 완전한 64비트 Current를 먼저 제한하므로 Current=`0x100000001`, cap=64이면 64이며 무한 값도 안전하게 제한됩니다. Maximum, 호스트 값, 실제 FD 수, `DescriptorLimit`로 추정하지 않습니다. 한쪽이 누락되면 다른 쪽이 0이어도 unsupported입니다. 여섯 인수를 모두 무시하며 사용자 메모리를 접근하지 않고 기존 BSD carry/보조 레지스터 규약을 사용합니다. Mach timebase trap 89는 독립적입니다.

sysctl 복사 단계는 유지됩니다. EUID0의 실제 쓰기는 이름/MIB와 oldlenp 검사 뒤, 관측/출력 전에 unsupported이고 비 root는 EPERM입니다. 새 포인터 길이가 0이면 읽기입니다. 제한 실행이나 쓰기 권한은 추정하지 않습니다. 필수 자원 workload는 두 cap 조회와 낮은/높은 syscall number를 검증하고 `l` / 144바이트 출력을 유지합니다. 뒤의 unsupported도 이미 출력한 바이트를 보존합니다. 독립 스칼라 fixture는 누락, 0, 넓은 Current와 DescriptorLimit=3의 독립성을 검증합니다.

```json
{"darwin_system":{"max_files_per_process":64,"resource_limits":[{"resource":8,"current":"4294967297","maximum":"9223372036854775807"}]}}
```

[XNU getdtablesize](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU proc_limitgetcur_nofile](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [XNU MIB constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/sysctl.h).

검증: Darwin (등록/통과/사용 불가/실패) 1441/913/528/0; ARM64 HVF 132/132; System/SDK 80/80; File 365/365; C/CLI 211/211; ProcessReport 45/45; native 33/33; Python 5 (42.254s); API 71; runner 44 + reference 5. clang-format 22.1.2; capabilities/provenance; docs 286 / locales 10 / negative controls 3.

시도 기록: 새 스칼라 메서드의 필수 등록이 없어 첫 runner 검사에서 ARM64와 x86-64 목록 검사가 실패했습니다. 전체 메서드 일치 규칙을 유지하며 등록을 추가했습니다. 초기 필수 129개 게이트와 실패 기록을 보존하며, 새 최종 ARM64 게이트는 132개를 요구합니다.

개수는 서로 겹칩니다. `build-hvf-arm64/descriptor-table-observations/`는 실제 실행 기준과 정확한 소스/바이너리/로그 해시를 보존하고 이전 증거는 변경하지 않습니다. 일회용 ARM64 macOS 자식 프로세스 5개, 검사 40개, 각 5초 제한: 기본 Current=1048575/cap=245760은245760, 자식 Current=0/1/32/245777은0/1/32/245760입니다. 부모와 시스템 제한은 변경하지 않았습니다. 실제 iOS, 중지된 Intel HVF, 원격 merge CI는 별도 검증입니다. 권한, 변경 후 디렉터리 관측, 공유 map/EOF, 진행 시계, Mach/thread/dyld 및 framework runtime은 미완성입니다.

## 명시적 프로세스 관측값

`DarwinSystemOptions::ProcessGroupID`, `SessionID`, `ProcessTainted`는 서로 독립적인 선택 입력이며 JSON 이름은 `process_group_id`, `session_id`, `process_tainted`입니다. ID는 양수이며 INT32_MAX 이하여야 하고, 오염 상태는 JSON Boolean `true`/`false`만 받습니다. 생략은 알 수 없는 값이고 명시적 `false`는 알려진 0입니다. 호스트, PID1000, 자격 증명 또는 다른 관측값에서 추론하지 않습니다.

원시 `getpgrp(81)`는 프로세스 그룹을 읽습니다. `getpgid(151)`와 `getsid(310)`은 부호 있는 하위32비트 `pid_t`를 사용하며 0 또는 고정된 현재 PID1000을 자기 자신으로 처리합니다. `0xffffffff000003e8`도 자기 자신입니다. 음수 하위32비트 PID는 관측값 조회 전에 ESRCH3을 반환하며, 읽기 전용 네이티브 프로브와 XNU의 프로세스 할당·조회 규칙으로 확인했습니다. `0x1000`을 포함한 알려지지 않은 양수의 다른 프로세스는 UnsupportedService로 중단하고 ESRCH나 플래그 마스크를 추측하지 않습니다. 선택한 자기 관측값이 없을 때도 지원하지 않음으로 중단합니다. `getpgrp`와 `issetugid(327)`는 모든 인수를 무시하며 네 개의 스칼라 조회는 게스트 메모리에 접근하지 않습니다. 기존 BSD carry와 두 번째 반환 레지스터 규칙을 유지합니다.

`process_tainted`는 고정된 `P_SUGID` 관측값이며 실제 ID와 유효 ID의 일치 여부와 독립적입니다. EUID, 파일 소유권, sysctl 쓰기 권한, entitlement, 리더 또는 터미널 상태를 바꾸지 않습니다. `setpgid`, `setsid`와 자격 증명 변경은 계속 지원하지 않습니다. 원본 읽기 전용 `process-observations`는 자기 PID를 캡처하고 상위 비트 전달값, 음수 오류, 반환 상태를 확인합니다. `virtual-process-observations`는 설정한 그룹·세션·오염 상태 바이트를 출력합니다. 다른 프로세스, 누락값, 설정 호출의 모델 전용 사례는 네이티브 실행 목록에서 제외합니다. macOS 참조 실행은 실제 iOS 기기 검증이 아닙니다.

```json
{"darwin_system":{"process_group_id":7,"session_id":16909060,"process_tainted":false}}
```

[XNU process queries](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c), [XNU service numbers](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/syscalls.master), [XNU PID allocation](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_fork.c), [XNU PID lookup](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_proc.c).


## 명시적 세션 로그인 버퍼

`DarwinSystemOptions::LoginNameBytes` 는 세션의 전체255바이트(`MAXLOGNAME`)를 독립적으로 선언하는 선택적 관측값입니다. JSON `login_name_hex` 는 대소문자를 허용하는 정확히510개의 ASCII 16진수 문자입니다. 내부NUL과 종료 뒤의 비영 바이트도 유효합니다. 생략은 미지, 명시적 전체0은 알려진 값입니다. 짧은 이름을 채우거나 호스트, 자격 증명, 프로세스 그룹, 세션ID, 오염 상태에서 추론하지 않습니다. 로그인 또는 파일 권한을 부여하지 않습니다.

원시 `getlogin(49)` 는 길이의 부호 없는 하위32비트 `u_int` 를 사용하여 min(length,255) 바이트만 복사합니다. 문자열 해석, NUL 추가, 필요 크기 출력은 없습니다. 길이0은 관측값과 게스트 메모리 접근 없이 잘못된 포인터에서도 성공하며 `0xffffffff00000000` 도0을 선택합니다. 양의 길이는 목적지 검사 전에 전체 선언값을 요구합니다. 전혀 쓰지 못하면 EFAULT14, 일부만 쓸 수 있으면 복사 전에 미지원으로 중단합니다. 사전 검사 오류는 바이트를 쓰지 않습니다. 기존 복사 계층은 백엔드 쓰기 오류를 전파하며 일반 롤백을 보장하지 않습니다. BSD carry 와 두 번째 반환 레지스터 규칙 및 오류 시 원래 x64 RDX 를 유지합니다.

`setlogin(50)` 은 명시root 또는 전체0에서도 미지원입니다. 독자적 읽기 전용 `login-buffer` 는 전체 길이 인수, 접두부, 인접 바이트, 길이0 포인터, EFAULT를 검사하고 `virtual-login-buffer` 는 선언한255바이트를 출력합니다. 누락 및 설정기 모델 테스트는 네이티브 실행에서 제외합니다. macOS ARM64 참조는 물리iOS나Intel 네이티브 검증을 뜻하지 않습니다. 예제는255개의0을 명시적으로 선언하며 빈 로그인 이름을 추론하지 않습니다.

게스트 검증기는 매 복사 전에 출력 265바이트 전체를 초기화하고 서로 겹치지 않는 세 구간을 오름차순으로 검사합니다: [0,3), [3,3+n), [3+n,265). n은 기존과 같은 상한 적용 복사 길이입니다. 첫 구간과 마지막 구간은 모든 보호 바이트를, 중간 구간은 복사된 모든 바이트를 원래 스냅샷과 비교합니다. 전체 길이 인수 12개, 길이0 호출 21개, EFAULT 호출 42개, carry/보조 레지스터 검사와 raw/virtual 경로는 기존 실행 한도에서 유지됩니다. 검증 작업을 줄여도 바이트 범위와 첫 오류 순서는 보존되며 제품 런타임 성능은 별도 측정이 필요합니다.

```json
{"darwin_system":{"login_name_hex":"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}}
```

[XNU getlogin / MAXLOGNAME](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c#L1561).


## 현재 프로세스 우선순위 명시값

`DarwinSystemOptions::ProcessNice` / JSON `darwin_system.nice` 는 -20 에서 20 사이의 고정 부호 있는 nice 값을 독립적으로 선언합니다. 생략은 미지이며 명시적0과 -1은 알려진 값입니다. 기존 무손실 정수 파서는 정수 숫자와 십진 정수 문자열을 허용하고 잘못된 형식, 소수, 지수 문자열, 공백, 범위 초과는 이미지 로드 전에 거부합니다. 정확한 정수의 숫자 JSON은 유효합니다. 비 Darwin 프로필은 거부합니다. 호스트, 자격 증명, 그룹, 세션, 오염, 로그인, 자원, CPU 값에서 추론하지 않고 스케줄링, 권한, 실행 예산을 바꾸지 않습니다.

원시 `getpriority(100)` 은 int 선택자와 부호 없는 `id_t` 대상의 하위32비트를 씁니다. 대상이 INT32_MAX보다 크면 먼저 EINVAL22를 반환합니다. 알 수 없는 선택자(GPU5,0x1000 포함)와 스레드3의0이 아닌 대상도 관측값 전에 EINVAL입니다. `PRIO_PROCESS`0은0 또는 현재 PID1000만 지원하며 자신을 선택한 뒤 nice를 요구합니다. 다른 양수 PID는 ESRCH를 추측하지 않고 미지원입니다. 그룹1, 사용자2, 스레드3/대상0, 확장4,6,7,8은 관련 값이 있어도 미지원입니다. 스레드 대상의 상위만 비영이면 미지 상태이며 EINVAL이 아닙니다. 결과는64비트 부호 확장으로 -1은 carry가 지워진 성공 UINT64_MAX입니다. 게스트 메모리를 쓰지 않고 미사용 인수를 무시합니다. BSD 두 번째 레지스터 규칙은 x64 오류의 RDX를 보존하고 성공 때0으로, ARM64는 두 경로의 X1을0으로 합니다.

`setpriority(96)` 은 명시 root/nice에서도 미지원입니다. 독자적 읽기 전용 `process-priority`는 자기 인수, 확실히 잘못된 인수, 성공/오류/성공 전환을 검사하며 `virtual-process-priority`는 설정한 부호 있는8바이트를 출력합니다. 다른 프로세스, 집계, 누락, 설정 경로는 모델만 시험합니다. 새 ARM64 macOS 탐침은 nice0으로191개 검사를 통과했습니다. 비음수 샘플은 음수 하드웨어 확장을 입증하지 않으므로 고정 버전의 부호 있는 진입 선언과 독립 모델 경계를 사용합니다. 물리 iOS와 Intel 네이티브 검증은 별개입니다.

```json
{"darwin_system":{"nice":-1}}
```

[XNU getpriority](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_resource.c), [XNU signed INT entry](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/dev/arm/systemcalls.c).


## 링크 추적 금지 open 플래그와 디렉터리 사전 검사

일반 및 nocancel `open` / `openat`은 부호 없는 하위32비트 O_NOFOLLOW=0x100 또는 O_NOFOLLOW_ANY=0x20000000을 허용합니다. 검색 플래그는 F_GETFL에 포함되지 않으며 접근, 추가, 잘라내기, 생성, FD별 CLOEXEC 동작을 바꾸지 않습니다. 두 플래그를 함께 지정하면 FD 용량 검사 뒤 전체 경로를 읽기 전에 EINVAL22를 반환하고, 테이블이 가득 차면 EMFILE24가 먼저입니다. 알 수 없는 플래그는 지원하지 않습니다.

AT_FDCWD가 아닌 `openat`은 접근 모드와 FD 용량 검사보다 먼저 경로 첫 바이트만 읽습니다. 읽을 수 없으면 EFAULT14입니다. 상대 접두사(NUL 포함)는 FD가 보유한 디렉터리 객체를 먼저 확인하여 알 수 없는 FD는 EBADF9, 일반 파일은 ENOTDIR20을 반환하고 알 수 없는 스트림 vnode 유형은 지원하지 않습니다. `/`는 dirfd 검사를 건너뛰고 이후 기존 open 순서로 전체 경로를 읽습니다. 일반 `open`과 AT_FDCWD는 이 사전 검사를 하지 않으며 다른 nameiat 경로 서비스는 플래그/크기 검사 후 첫 바이트와 상대 dirfd를 확인하고 전체 문자열을 읽습니다. 거부된 호출은 FD나 새 inode를 소비하지 않고 전송 오류를 그대로 전달하며 이름 공간을 변경하지 않습니다.

기존 `file-access`는 상대 디렉터리 FD로 NOFOLLOW_ANY를 검사하여 네이티브 `/var`, `/tmp` 링크 별칭을 피합니다. 직접 테스트는 첫 바이트와 후속 오류, 슬래시/NUL, 사용자 주소와 페이지 경계, FD 고갈, 삭제된 디렉터리 객체를 구분합니다. ARM64 macOS 독립 원시 프로브는 기존 5초 제한으로30건을 통과했습니다. 마지막 고갈 상태 절대 경로 행은 원래 라벨과 달리 유효한 디렉터리 FD를 사용합니다. 실제 iOS와 Intel 네이티브 검증은 별도입니다.

[XNU open1at / open1](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU vn_open_auth](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_vnops.c), [XNU open flags / FMASK](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/sys/fcntl.h).

## 고정 초기 심볼릭 링크

아래 초기 이름은 기본적으로 보호됩니다. 명시적 변경 권한을 부여한 동작은 마지막 절을 참조하세요. `DarwinFileOptions::SymbolicLinks` / `darwin_files.symbolic_links`는 정규 절대 `path`, 원시 16진수 `target_hex`, 선택적인 링크 자체 `metadata`를 선언합니다. 대상은1..1023 비NUL 바이트이며 비UTF-8, 반복 슬래시, 점, 없는 대상을 보존합니다. 빈 배열이어도 `files`는 필수입니다. 이름 충돌과 링크 아래 선언은 거부합니다. 경로/NUL/대상은256개/16 MiB를 공유합니다. S_IFLNK, 대상 길이와 같은size, DT_LNK=10, 일치inode가 필요하며 설정 CWD는 실제 디렉터리여야 합니다.

링크 확장 후 점을 처리합니다. 상대 대상은 실제 부모, 절대 대상은 게스트 루트에서 시작합니다. 확장마다 끝 슬래시를 다시 해석하며 소비한 입력 슬래시를 물려주지 않습니다.32회까지 허용,33회는ELOOP62; 대상+접미사+NUL이1024바이트 초과면ENAMETOOLONG63. FD/CWD/F_GETPATH/mmap은 최종 객체를 유지합니다.

stat64/open/access/truncate/chdir는 끝 링크를 따르고 lstat64/readlink는 유지합니다. O_NOFOLLOW는ELOOP, O_DIRECTORY와 결합하면 먼저ENOTDIR20. O_NOFOLLOW_ANY는 필요한 확장을 거부합니다. O_CREAT|O_EXCL은 기존 끝 링크에EEXIST17(없는 대상/순환 포함). AT0x20과AT0x800은 끝 링크를 유지하며0x800은 중간/끝 슬래시 확장도 거부하고 결합 가능합니다. AT_FDONLY는 플래그 검사 후 경로를 무시합니다.

readlink(58)는 부호 있는 하위32비트count, readlinkat(473)는 전체size_t이며int를 반환합니다. INT32_MAX 초과는 경로/FD보다 먼저EINVAL22. min(count,대상 길이)만 복사하고NUL을 추가하지 않으며 실제 범위만 검사합니다.0길이도 경로/유형 검사 후 출력 포인터를 무시합니다. 비링크EINVAL22, 모두 쓰기 불가EFAULT14, 부분 쓰기는 복사 전에 중지합니다. 전송/메모리 예산 오류를 전달합니다.

고정 링크 이름과 원시 대상 바이트는 변하지 않습니다. MutableDirectories는 루트 또는 고정 링크 이름의 경로 구간 조상이 될 수 없으며 /work는 /workspace/link를 포함하지 않습니다. 별도의 가변 디렉터리에서 대상 이름을 실행 중 생성, 이동, 삭제, 교체할 수 있습니다. 기존 부모, 마운트, 별칭, 플래그, SWAP 지원, 생성 정책 검증은 유지되며 새 inode는 보호 링크를 포함한 모든 메타데이터/스냅샷 inode보다 커야 합니다. 고정 WritableFiles/MutationPolicies는 최종 일반 파일을 변경할 수 있습니다. 링크unlink/rename은 효과 전에 중지합니다. 실행 중 링크 생성은 다음 절에 설명합니다. 하드 링크,ACL,개별 권한 없는 초기 링크 변경 목록은 미지원입니다. ARM64 macOS 독립 프로브는 기존5초 내189관찰/115전체 버퍼를 통과했으며 물리iOS/Intel HVF/전체OS를 입증하지 않습니다.

추가 ARM64 macOS DELETE/RENAME 60개 제어는 기존5초 내 전체 stat 버퍼, 변경 전후 이름 공간과 유지 FD/CWD 식별을 기록합니다. 끝 슬래시는 링크를 펼쳐 실제 대상을 변경할 수 있고 NOFOLLOW_ANY는 필요한 펼침을 ELOOP로 거부합니다. SDK 없는 symbolic-link-mutations는 생성, 없는 대상, 이동/삭제/교체, 유지 CWD 부모, FD 종료 전 원래 파일10바이트 전체와 종료 후에도 유지된 매핑10바이트 전체를 검사합니다. 물리 iOS나 네이티브 Intel 증거는 아닙니다.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"symbolic_links":[{"path":"/link","target_hex":"64617461"}],"working_directory":"/"}}
```

[XNU namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c), [XNU readlink / AT](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU open authorization](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_subr.c).

## 실행 중 심볼릭 링크 생성

원시 `symlink(57)`와 `symlinkat(474)`는 변경을 허용한 디렉터리에 프로세스 로컬 링크를 만들고 int를 반환합니다. dirfd는 하위32비트이며 절대 목적지 이름은 FD를 무시합니다. 목적지 검사 전에 첫 NUL까지 대상을 읽습니다.0..1023바이트는 빈 값, 비UTF-8, 점, 반복 슬래시를 허용합니다.1024바이트에 NUL이 없으면 ENAMETOOLONG63, 먼저 읽을 수 없는 바이트를 만나면 EFAULT14입니다. 초기 JSON 대상은 계속1..1023바이트입니다.

현재 링크 표가 실제 이름, 부모와 대상 바이트를 보유합니다. 기존 말단은 EEXIST17입니다. 소모한 마지막 슬래시는 dangling 링크의 실제 대상 이름에서 생성을 허용하며 기존 링크는 바뀌지 않습니다. 빈 대상 확장은 ENOENT2입니다. 빈 대상 readlink는 양의 용량에서도 출력 포인터에 접근하지 않고0을 반환하지만 count/경로/유형을 먼저 검사합니다.

이름/NUL과 대상은 한 번만 계산하며256항목/16 MiB를 공유합니다. 거부는 노드, 부모, FD, 일반 파일 inode를 바꾸지 않습니다. 새 링크의 전체 메타데이터는 알 수 없으며 일반 파일 CreationPolicy나 재사용 이름의 이전 관측을 상속하지 않습니다. 생성 후 부모 stat/스냅샷은 알 수 없습니다. 대상 삭제/교체 후에도 FD/CWD/매핑은 원래 객체를 유지합니다. rmdir와 디렉터리 교체는 링크 자식을 검사합니다. 이동/SWAP 양쪽 중 보호된 초기 링크가 있는 쪽이 있으면 효과 전에 중지합니다. 링크/디렉터리 교체, 하드 링크, ACL, 개별 권한 없는 초기 링크 변경 목록은 미지원이며 별칭은 실제 부모의 권한을 옮기지 않습니다.

ARM64 macOS의150개 원시 기록은 관측기 실패4개를 유지하며 별도10개로 실제 새 대상과 빈 링크 경계를 확인합니다. SDK 없는 `symbolic-link-creation`은 두 진입점, 대상/버퍼 경계, 부모, 교체 파일 및 기존 FD/매핑10바이트 전체를 확인합니다. 물리 iOS, 네이티브 Intel, 전체 OS 호환성 증거는 아닙니다.

[XNU symlink / symlinkat](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU empty-link expansion](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c).

## 런타임에 만든 심볼릭 링크 삭제

`unlink(10)`과 `unlinkat(472)`는 실제 부모에 변경 권한이 있는 런타임 링크를 삭제합니다. 초기 고정 링크는 계속 보호됩니다. 단독 `AT_SYMLINK_NOFOLLOW_ANY`는 마지막 링크를 유지하므로 끊긴 대상, 순환 또는 빈 대상도 삭제할 수 있습니다. 중간 경로나 끝 슬래시의 확장이 필요하면 ELOOP62입니다. 플래그 없이 `a → b → target`의 `a/`를 삭제하면 `b`만 삭제하고 `a`와 최종 대상을 유지합니다. 기존 플래그·경로·dirfd 오류 순서는 같습니다.

성공하면 동적 항목 하나와 현재 경로/NUL/대상 바이트 비용을 한 번 반환하고 실제 부모의 전체 stat/열거 관측만 무효화합니다. 빈 FD나 생성 inode는 필요 없습니다. 대상 내용·변경 정책·설명자·공유 커서·CWD·매핑은 원래 객체를 유지합니다. 이름 재사용은 이전 링크를 복원하지 않으며 거부는 상태나 비용을 바꾸지 않습니다. 실행 중 생성된 링크의 전체 메타데이터는 여전히 알 수 없습니다. 링크/디렉터리 교체와 보호된 초기 링크가 남은 하위 트리 이동/SWAP은 미지원입니다.

독립 ARM64 macOS 40개 관측은 기존 5초 제한에서 삭제 성공28개, 오류12개, 재삭제 ENOENT17개와 FD/CWD/비공개 매핑 보존을 기록했습니다. 게스트·실제 iOS·Intel 검증은 아닙니다. `symbolic-link-unlink` 및 공개 API 검사는 별도입니다.

## 실행 중 생성한 심볼릭 링크 이름 변경

`rename(128)`, `renameat(465)`, `renameatx_np(488)`는 일반 이동과 `RENAME_EXCL=4`를 지원합니다. 실행 중 링크를 빈 이름으로 이동하거나 링크/링크, 링크/일반 파일, 일반 파일/링크를 교체할 수 있습니다. 실제 양쪽 부모는 확인된 동일 마운트에서 수정 권한이 있어야 하며 초기 링크는 불변입니다. 공유 해석기가 실제 노드를 선택하고 빈·누락·순환·비UTF-8 대상 바이트를 유지합니다. 상대 대상은 새 부모에서 해석합니다. 단독 `RENAME_NOFOLLOW_ANY=16`는 마지막 링크를 유지하며 중간 확장이 필요하면 ELOOP62입니다. 다른 기존 EXCL 대상은 EEXIST17이며 같은 객체 EXCL은 파일 시스템 대소문자 계약 없이 미지원입니다.

이름을 게시하기 전에 새 경로/NUL 비용을 예약하며 NUL 포함1024바이트로 제한합니다. 교체된 실행 중 링크의 현재 경로/NUL/대상 비용은 대상 FD나 매핑과 무관하게 한 번 반환합니다. 교체된 일반 파일의 내용/동적 경로 비용은 모든 설명자와 매핑 임대가 해제될 때 회수하며 즉시 회수 가능한 일반 대상만 예약 크레딧을 제공합니다. 추가 항목, FD, 일반 파일 생성 inode는 불필요합니다. 거부는 두 노드를 유지하고 성공은 실제 부모의 전체 stat/열거 관측을 무효화합니다. 실행 중 링크의 전체 메타데이터는 알 수 없습니다. 링크/디렉터리 교체, 하드 링크, 보호된 초기 링크가 남은 하위 트리 이동/SWAP은 미지원입니다.

독립 ARM64 macOS19사례는 기존5초 제한에서14성공과5개 EEXIST/ELOOP를 기록하고 링크 inode/원시 바이트, 상대 대상 재결합, 유지된 FD/dup/커서/CWD/개인 매핑을 검사합니다. SDK 없는 `symbolic-link-rename`와 공개 SDK/CLI 검사는 별도입니다. 원시 참고만으로 실제 iOS, 네이티브 Intel, 전체 OS 호환성을 증명하지 않습니다.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 실행 중 생성한 심볼릭 링크 교환

`renameatx_np(488)`는 `RENAME_SWAP=2`로 링크/링크, 링크/일반 파일, 일반 파일/링크를 교환합니다. 실제 양쪽 부모가 확인된 동일 마운트에서 수정과 SWAP 권한을 제공해야 합니다. 같은 이름은 추가 SWAP 선언 없이 효과 없이 성공합니다. 대상 누락은 ENOENT2, `SWAP|NOFOLLOW_ANY=18`은 마지막 링크를 유지하고 필요한 중간 확장을 ELOOP62로 거부하며 `SWAP|EXCL=6`은 경로 읽기 전에 EINVAL22입니다. 원시 바이트는 유지하고 상대 대상은 양쪽 새 부모에서 해석합니다.

두 경로/NUL 비용을 이름 게시 전에 예약합니다. 두 객체는 연결 상태를 유지하며 데이터나 매핑 임대로 교체 크레딧을 얻지 않습니다. 초기 파일은 첫 교환에서 동적 경로 비용을 얻고 되돌릴 때도 재사용합니다. 항목, FD, 생성 inode를 소비하지 않습니다. 파일 정체성, nlink, 설명자, 커서, CWD, 매핑을 유지하고 부모의 전체 관측은 알 수 없게 됩니다. 실행 중 링크 전체 메타데이터, 실제 디렉터리/링크, 보호된 초기 링크를 포함한 하위 트리 이동/SWAP은 미지원입니다. ARM64 macOS 22사례는 원래 5초 제한에서 14교환, 같은 객체 2성공, 6오류를 기록했습니다. `symbolic-link-rename`와 SDK/CLI/Python이 교환과 오류를 검사하지만 실제 iOS, 네이티브 Intel, 전체 OS 호환성은 증명하지 않습니다.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 실행 중 링크를 포함한 트리 이동

일반/EXCL 디렉터리 이동과 SWAP은 실행 중 링크 자손을 포함하고 디렉터리/파일 교환도 지원합니다. 기존 트랜잭션은 모든 디렉터리·파일·링크 새 이름과 NUL 포함1024바이트 한도를 검사하며 세 테이블을 모두 추출한 뒤 이름을 게시합니다. 대상 바이트와 비용은 그대로 유지하고 현재 경로/NUL 비용만 바꿉니다. SWAP은 교체 크레딧을 제공하지 않고 새 항목·FD·생성 inode도 소비하지 않습니다.

링크는 이동하는 실제 부모 객체를 유지하며 상대 대상은 새 경로에서 해석합니다. 설명자, 공유 커서, CWD, 삭제된 노드와 매핑 임대는 원래 객체를 유지합니다. 보호된 초기 링크와 해당 트리, 루트 디렉터리/링크 조합, 링크 전체 메타데이터, 하드 링크, ACL은 미지원입니다. ARM64 macOS25대조는 기존5초 제한에서5이동·10교환·같은 객체2성공·이름 변경 없는8거부를 기록했습니다. 부모 간 대조는 원시 바이트 유지와 새 상대 해석을 확인하고 `symbolic-link-rename`가C++/SDK/CLI/Python을 검사합니다. 실제 iOS·네이티브 Intel·전체 OS를 입증하지 않습니다.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## 새 링크와 디렉터리의 메타데이터

선택적 `creation_policy.namespace_policy`（C++ `DarwinFileCreationPolicy::Namespace`）는 기존 필수5필드에 엄격한 객체를 추가합니다. `symbolic_link_allocation_unit`, `directory_entry_size`, `directory_blocks`만 허용합니다. 링크 단위는512..16 MiB의2의 거듭제곱, 항목 크기는 양수이며 최대16 MiB, 블록 수는INT64_MAX 이하 uint64입니다. 십진 문자열도 허용합니다. 부모 메타데이터, 초기umask, 새inode 조건은 유지됩니다. 생략하면 앞 절의 링크/디렉터리 메타데이터 미확인과 일반 파일만 inode를 소비하는 기본 동작을 유지합니다.

활성화하면 일반 파일,symlink,mkdir의 성공 삽입이 하나의inode 순서를 공유하고 UINT64_MAX에서 영구 고갈됩니다. 거부와 기존 이름open은 소비하지 않습니다. Device/GID는 실제 부모, UID는 유효 게스트 ID에서 옵니다. 링크mode는S_IFLNK와 `0777 & ~umask`, nlink=1, size는 빈 대상/비UTF-8을 포함한 원시 바이트 수이며 blocks는 선언 단위로 올림한512바이트 블록 수입니다. 디렉터리mode는S_IFDIR와 `mode & 0777 & ~umask`, nlink는2와 모든 종류의 직접 연결 이름 수의 합, size는nlink와 선언 항목 크기의 곱, blocks는 고정입니다. 유지된 삭제된 빈 디렉터리에도 적용하는 명시적 가상 계약이며 APFS 추론이 아닙니다.

초기 시간은creation_time입니다. 자식 이름 변경은 새 부모mtime/ctime을, 직접 이동은 링크/디렉터리ctime만 mutation_time으로 갱신합니다. 조상 이동은 후손 기록을 유지하며 dup/CWD/교체/SWAP/삭제/이름 재사용에서 기록은 객체에 속합니다. 생성 확장만으로 초기 부모 전체 stat과 고정 스냅샷을 유지하지 않습니다. 아래 독립 정책이 stat과 실시간 열거를 제공합니다. ACL, 개별 권한 없는 초기 링크 변경, 일반 디렉터리/링크 루트 거래는 미지원입니다. 원래 `created-namespace-metadata`는 공통mode/소유자/정체성/수명을, `virtual-created-namespace-metadata`는C++/SDK/CLI/Python에서 전체144바이트 상수 기록을 확인합니다. 대상 검증은 모델/준입11,엄격JSON1,기존5초의 원래43,실행 가능한 게스트8（불가12건 건너뜀,필수HVF3실행）,공개10을 통과했습니다. 포인터 표의 실패와 수정된 정적ARM64 기록을 보존합니다. 실제iOS/네이티브Intel은 미검증입니다。

```json
{"namespace_policy":{"symbolic_link_allocation_unit":512,"directory_entry_size":32,"directory_blocks":7}}
```

## 이름 공간 변경 후 가상 디렉터리 열거

선택적 `directories[].enumeration_policy`로 `getdirentries64`가 현재 이름을 열거합니다. C++에서는 `DarwinFileOptions::DirectoryEnumerationPolicies`와 `DarwinDirectoryEnumerationPolicy`를 사용합니다. 엄격한 객체에는 `minimum_buffer_size`, `initial_minimum_buffer_size`, `seek_offset` 세 필드가 필요합니다. 첫 값은 양수, 두 하한은128 MiB 이하이며 seek_offset은 손실 없는 uint64입니다. 초기 디렉터리 자신의 0이 아닌 inode 메타데이터가 필요하며 불변 `contents`와 함께 선언할 수 없습니다. 정책 참조마다 경로+NUL 비용을 한 번 부과합니다. mkdir는 실제 부모 정책을 상속하고 기존 하위 디렉터리는 자신의 선언을 유지합니다. 열거 정책과 변경 권한은 독립입니다.

가상 순서는 `.`, `..`, 부호 없는 바이트 순으로 정렬한 직접 연결 이름입니다. 관찰되거나 프로세스가 만든 객체의 inode를 사용하며 `..`는 유지된 실제 부모를 따릅니다. 자식/부모 신원이 없거나0이면 출력 전에 중단합니다. 초기 전체 stat이 무효화되어도 inode만 유지하고 다른 오래된 필드는 재사용하지 않습니다. 커서는1부터 시작하는 지역 순번이고 d_seekoff는 선언 상수입니다. dup는 공유하고 open은 독립입니다. 확정된 항목 변경이나 직접 디렉터리 이동 후에는0으로 되감아야 하며 거부와 동일 객체 무동작은 유지됩니다. 조상 이동은 후손 커서를 유지합니다. 버전 소진은 명시 중단하며 유지된 삭제 빈 디렉터리는 이름 재사용 후에도0개 레코드입니다.

이 단락은 열거 정책만 지정하고 아래 초기 stat 정책을 지정하지 않은 경우의 기록입니다. 기존 레코드 인코더, 완전한 레코드 묶음, 버퍼 하한, 페이로드 상한, EOF 접미사와 데이터/커서/위치/플래그 순서를 재사용합니다. 초기 부모 전체 stat과 고정 스냅샷은 계속 무효화되며 정책을 생략하면 이전 미지원 동작입니다. APFS 세대는 재현하지 않습니다. 독립 ARM64 준비는 변경 없는5초 내에25이벤트/16뷰를 기록했습니다. `directory-enumeration-mutations`는 네이티브 신원과 객체 유지, `virtual-directory-enumeration`는 guest/C/CLI/Python의160바이트 고정 뷰를 검증합니다. Intel 네이티브, 실제 iOS, ACL, 하드 링크 및 전체 OS/프레임워크는 미검증 또는 미지원입니다.

## 초기 디렉터리의 명시적 stat 변경 정책

선택적 `directories[].mutation_policy`는 허용된 초기 디렉터리의 전체 stat을 이름 공간 변경 뒤에도 유지합니다. C++은 `DarwinDirectoryMutationPolicy`와 `DarwinFileOptions::DirectoryMutationPolicies`를 사용합니다. 엄격한 객체에는 `directory_entry_size`와 `mutation_time`만 있습니다. 항목 크기는 양수이며 최대 16 MiB, 시간은 손실 없는 부호 있는 64비트 초와 [0,1000000000) 나노초입니다. 디렉터리 자체의 완전한 metadata와 0이 아닌 inode가 필요합니다. 참조마다 경로와 NUL 비용을 한 번 부과하며 투영 size는 파일 바이트를 할당하지 않습니다. 이 정책은 이름 공간 변경이나 권한을 부여하지 않으며 생성·열거 정책도 요구하지 않습니다.

첫 실제 변경 확정 전에는 전체 관측 기록을 유지하고, 확정 시 스칼라 필드를 할당 없이 디렉터리 객체에 복사합니다. 자식 이름 변경은 nlink를 2와 모든 종류의 직접 연결 이름 수의 합, size를 nlink와 directory_entry_size의 곱, mtime/ctime을 mutation_time으로 설정합니다. 직접 이동, SWAP, 삭제는 ctime만 바꾸고 조상 이동은 후손 기록을 유지합니다. 거부와 동일 객체 무동작은 아무것도 바꾸지 않습니다. Device, inode, mode, 소유자, blocks, 블록 크기, flags, generation, atime, birthtime은 관측값을 유지합니다. 명시적 가상 규칙이며 APFS 할당, 링크 수, 시계 추론이 아닙니다.

기록과 정책은 dup, 유지 FD, CWD, 교체, 삭제, 이름 재사용에서도 원래 객체에 속합니다. 새 mkdir 객체는 별도로 지정된 생성 정책을 사용하며 부모나 같은 이름의 이전 객체에서 초기 stat 정책을 상속하지 않습니다. 불변 스냅샷은 변경 후 계속 미상이며 독립 enumeration_policy가 실시간 보기를 제공할 수 있습니다. 이 stat 정책을 생략하면 변경된 초기 디렉터리의 전체 메타데이터는 미상으로 남습니다.

독립 ARM64 네이티브 준비는 변경 없는 5초 안에 보호된 원시 stat 보기 27개와 작업 15개를 보존하고 144바이트 SDK ABI와 유지 신원을 확인했습니다. 네이티브 시간이나 할당 규칙을 일반화하지 않습니다. 독자적인 `initial-directory-metadata`는 공통 네이티브 관측을, `virtual-initial-directory-metadata`는 guest, C/CLI, Python의 전체 144바이트 상수 기록을 확인합니다. 모델은 첫 삭제, 변경 권한 부재, 생성 정책 생략, 스냅샷/열거 독립성도 다룹니다. 네이티브 Intel, 실제 iOS, ACL, 하드 링크, 개별 권한 없는 초기 링크 변경, 전체 OS/프레임워크는 미검증 또는 미지원입니다.

```json
{"mutation_policy":{"directory_entry_size":17,"mutation_time":{"seconds":-11,"nanoseconds":321}}}
```

## 초기 심볼릭 링크의 명시적 변경 권한

`symbolic_links[].mutable:true`와 C++ `DarwinFileOptions::MutableSymbolicLinks`는 원래 링크 객체의 이름 공간 변경을 허용합니다. 실제 부모도 별도 변경 권한이 필요합니다. 알려진 flags, 특수 mode, link_count≠1, 식별자 별칭, 부모 장치 충돌은 거부됩니다. 생략하면 이름이 보호되어 변경 가능한 조상 영역에 놓을 수 없습니다. 권한은 고정 path/NUL 참조를 예약하며 항목이나 생성 inode를 추가하지 않습니다. 초기 대상 바이트는 불변입니다.

unlink, 일반/EXCL rename, 말단 링크/파일/link SWAP, 선언된 디렉터리 하위 트리 거래는 실제 부모·mount·SWAP 조건을 유지합니다. 상대 대상은 새 실제 부모에서 해석하고 FD/dup, CWD, 매핑 lease는 기존 참조 객체를 유지합니다. 삭제/교체 후에도 초기 이름/대상/참조 비용은 고정 예약되며 첫 이름 변경은 동적 이름 비용을 별도로 예약합니다. 객체 소유 동적 이름/생성 대상 비용만 환불하고 생성 링크만 동적 항목으로 셉니다. 거부와 같은 객체 무효 동작은 상태를 바꾸지 않습니다.

`symbolic_links[].mutation_policy`는 `DarwinSymbolicLinkMutationPolicy`, `DarwinFileOptions::SymbolicLinkMutationPolicies`를 사용합니다. 유일한 `mutation_time`은 무손실 signed 64-bit 초와 [0, 1000000000) 나노초이며 권한과 완전한 nonzero inode 관측이 필요합니다. 첫 직접 이동/SWAP은 할당 없이 스칼라를 복사하고 ctime만 바꿉니다. blocks 등 나머지 필드, 조상 이동, 거부, 무효 동작은 보존합니다. 정책이 없으면 직접 이동 후 전체 stat은 미상이지만 열거 inode와 알려진 장치 충돌은 유지됩니다. 정책 path/NUL 참조를 한 번 예약합니다. 이름 재사용이나 새 symlink는 초기 기록/정책을 상속하지 않고 별도 namespace policy를 사용합니다.

독립 ARM64 준비는 guarded raw-stat 14개, 동작 11개, 144-byte SDK ABI, compile120s/native5s/drain1s/reap1s와 사적 영역 정리를 기록합니다. `mutable-initial-links`는 원생 식별자·대상 재해석·참조 수명을, `virtual-mutable-initial-links`는 다섯 guest·C/CLI·Python의 전체 stat을 확인합니다. 두 페이지 크기, 하위 트리 SWAP, 정확한 비용과 entry/inode 고갈을 모델로 검사합니다. Intel 및 실제 iOS는 원생 검증되지 않았고 hard link, ACL, 전체 OS/runtime/framework는 남아 있습니다.

```json
{"mutable":true,"mutation_policy":{"mutation_time":{"seconds":-13,"nanoseconds":456}}}
```

## 디렉터리와 심볼릭 링크 루트 교환

`renameatx_np(RENAME_SWAP)`는 비어 있지 않은 하위 트리를 포함해 실제 디렉터리와 링크를 양방향으로 교환합니다. 초기 디렉터리는 `exchangeable`, 초기 링크는 `mutable`, 양쪽 실제 부모는 확립된 동일 mount의 변경/SWAP 권한이 필요합니다. 보호된 초기 후손은 계속 거부합니다. 기존 일반 이동 권한에서 디렉터리→링크는 ENOTDIR20, 반대는 EISDIR21, 다른 기존 이름에 대한 EXCL은 EEXIST17이며 자신의 후손 링크와의 교환은 두 방향 모두 변경 전에 EINVAL22입니다. 링크 자체를 교환하므로 자기 참조가 생기는 교환도 성공할 수 있고 이후 따라가기는 ELOOP62입니다.

기존 세 이름 표 거래는 루트, 연결되거나 보유된 후손, 전체 경로와 동적 공간을 게시 전에 검사합니다. 초기 이름/대상/참조, 파일 내용, 매핑 임대는 SWAP 비용을 줄이지 않습니다. 첫 초기 이름 변경은 별도 동적 경로/NUL을 차감하고 반복 교환은 기존 비용을 한 번 교체합니다. 새 항목/FD/생성 inode가 필요하지 않으며 같은 철자의 오래된 고아 객체도 실제 부모가 다르면 새 트리에 속하지 않습니다. 원시 대상은 유지되고 상대 해석은 새 부모에 결합됩니다. FD/dup 커서, CWD, 참조 대상과 매핑은 유지됩니다. 직접 루트는 자신의 stat 정책으로 ctime만 갱신하고 조상 이동은 후손 기록을 유지합니다. 정책 생략 시 전체 stat은 미지지만 inode는 실시간 열거에 사용하며 관련 열거 버전은 기존 영점 되감기 계약을 따릅니다. JSON 필드나 권한을 추가하지 않습니다.

독립 ARM64 준비는 guarded 144-byte stat 36개, raw rename 16개, compile120s/native5s/drain1s/reap1s와 회수/사적 정리를 기록합니다. `directory-link-roots`와 `virtual-directory-link-roots`는 다섯 guest·C/CLI·Python에서 원생 식별과 전체 stat 상수를 검사합니다. 두 페이지 모델은 정확한 예산, 미개방 후손 초과, 고아 객체와 FD/항목/inode 고갈을 다룹니다. 이 절은 위 권한 안에서 이전 제한을 확장합니다. Intel 원생, 실제 iOS, hard link, ACL과 전체 OS/runtime/framework는 별도 검증·구현 과제입니다.

## 고정 커널 pathconf 쿼리

원시 pathconf(191) / fpathconf(192)는 XNU 고정 vnode 쿼리를 지원합니다:15/16/17→1,19/25→0,20/22/23→4096,21→65536,24→255. 링크/할당/I/O 선언과 전송 권고를 조회할 뿐 비동기 실행이나 권한을 활성화하지 않으며 페이지 크기나 카탈로그 예산을 파일시스템 값으로 추정하지 않습니다.

전체 경로 링크 해석 또는 FD 조회가 low32 선택자보다 먼저입니다. CWD, 링크와 EFAULT/ENOENT/ENOTDIR/ELOOP 순서를 유지하고 알 수 없거나 닫힌 FD는 EBADF입니다. 유지된 일반 파일/디렉터리는 stat 없이 dup/이동/삭제/이름 재사용 후에도 조회됩니다. 입력/캡처 FD의 원시 종류는 알 수 없어 명시적으로 중단합니다. BSD int/캐리/보조 레지스터 계약을 사용하며 출력 복사, 커서, 메타데이터/열거, 항목/FD/inode를 바꾸지 않습니다. NAME_MAX, 대소문자 속성 및 미지 선택자는 조회 후에도 지원하지 않고 호스트 값이나 EINVAL을 추측하지 않습니다.

kernel-pathconf, kernel-pathconf-values, kernel-pathconf-unsupported는 공통 동작, 독립80바이트 값, 기존 출력을 유지하는 중단 보고를 게스트/C/CLI/Python에서 검사합니다. 별도 ARM64 원시 준비는250회 조회와249회 SDK 대조를0.262초에 완료했으며5초 제한은 그대로입니다. 원시 Intel/실제 iOS/ACL/하드 링크/전체 런타임과 프레임워크는 미검증 또는 미구현입니다.

[XNU vn_pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU fpathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_descrip.c).

`pathconf`: `file/` → ENOTDIR; `alias/`, `alias//` → 1 (selector15); `alias/.`, `alias/child` → ENOTDIR.

## 고정 공통 속성 목록

getattrlist(220), fgetattrlist(228), getattrlistat(476)는 명시적 카탈로그의 고정 공통 속성11개를 조회합니다: 장치, 객체 유형, 네 시간, 소유자/그룹, 전체 모드, 플래그, 파일ID. stat64와 완전한 메타데이터 유효성 판단을 공유하며 누락/무효화 관찰은 미지 상태로 남습니다. 유형과 빈 선택은 stat이 필요 없습니다. 루트/마운트 NAME, 볼륨, 디렉터리/파일/fork 전용 마스크, ACL 및 미지 옵션은 반환 마스크를 요청해도 명시적으로 미지원이며 호스트 정보나 마운트 이름을 추론하지 않습니다.

경로/at는24바이트 요청을 먼저 읽고 FD는 low32 FD와 원래 유형을 먼저 검사합니다. reserved는 무시합니다. 공통 CWD/상대FD/링크 해석은 크기/비트맵 검사 전에 원래 오류를 유지합니다. 리틀엔디언/4바이트 정렬, 전체 st_mode, 부호 있는 초를 사용하며 반환 마스크 포함120바이트/일반100바이트입니다. 짧은 버퍼에는 요청한 앞부분만 복사하지만 전체 필요 길이를 보고합니다. 부분 접근은 해당 복사 전에 중단하고 signed-uio 초과 크기는 유효한 지원 요청 후 EINVAL입니다. 커서, 메타데이터, 열거 및 항목/FD/inode 예산은 변하지 않으며 dup/삭제/이름 재사용 수명은 유지됩니다.

세 모드는 guest/C/CLI/Python에서 공통 동작, 독립 설정 바이트, 기존 출력을 유지하는 미지원 ATTR_CMN_EXTENDED_SECURITY을 검사합니다. ARM64 전용 준비는187 raw 조회/176 SDK 경로-FD 비교를 통과했고 native5초 제한은 그대로입니다. SDK15.5는 getattrlistat를 선언하지 않아 raw476은 별도 기록합니다. 새 guest/Python은5,000,000us/quantum1024, 기존 공개 테스트는10s입니다. 원시 Intel/실제 iOS/FS 특성/하드 링크/권한·ACL/전체 런타임·프레임워크는 미검증 또는 미완성입니다.

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


## 명시적 확장 속성 읽기

getxattr(234), fgetxattr(235), listxattr(240), flistxattr(241)은 darwin_files의 완전하고 순서가 있는 관찰 값을 읽는다. 파일·디렉터리·링크는 엄격한 {name,bytes_hex} 배열 extended_attributes를 제공한다. 생략은 알 수 없음, []는 알려진 빈 목록이다. 이름은 슬래시도 허용하는 1..127바이트 UTF-8이고 값은 불투명 바이트다. 중복은 거부하며 선언 순서를 보존한다. 최대 4096개이며 이름과 NUL 및 값을 기존 16 MiB 예산에 계산하고 기존 객체 경로는 중복 계산하지 않는다.

관찰 값은 dup, 이동, 삭제, CWD, 이름 재사용 후에도 객체에 속하며 stat와 열거 유효성과 독립적이다. 새 객체는 알 수 없다. 내용 쓰기, 성공한 절단, 불확실한 비어 있지 않은 복사 실패는 속성을 무효화한다. 복사 전 부분 버퍼 거부는 값을 보존한다. 조회는 오프셋·입력·메타데이터·항목/FD/inode 예산을 바꾸지 않는다.

ABI는 낮은32비트 FD/options/position, 전체64비트 size와 BSD user_ssize_t/carry/secondary를 사용한다. NULL 조회는 position을 무시한다. 비어 있지 않은 값에서 경로의 비NULL size0은 ERANGE, FD size0은 길이 조회다. 경로 get의 UINT32_MAX/UINT64_MAX만 이전 길이 조회이고 FD get은 INT32_MAX로 제한한다. 양의 짧은 목록은 완전한 이름 접두사를 기록한 뒤 ERANGE를 반환하며 비어 있지 않은 목록의 비NULL 음수64비트 길이도 ERANGE다. 네이티브 빈 목록은 검증하지 못했으므로 명시적 빈 목록의 음수 길이는 UnsupportedService다. 완전히 쓸 수 없는 출력은 복사 전에 중단한다. NOFOLLOW1과 NOFOLLOW_ANY64는 독립적이다. 8/16은 검색 전, FD1/64는 FD/이름 전에 거부한다. CREATE2/REPLACE4는 읽기에서 무시하고 SHOWCOMPRESSION32 및 알 수 없는 비트는 미지원이다. com.apple.system.*, ResourceFork, FinderInfo, decmpfs, 설정/삭제, 권한/ACL, 파일시스템 추론은 범위 밖이다.

extended-attributes / extended-attributes-values / extended-attributes-unsupported는 공유 네이티브 동작, 명시적 가상 바이트, 기존 출력을 보존하는 미지원 중단을 검증한다. guest/Python5,000,000us/quantum1024, 공개API10s, 네이티브5s는 그대로다. ARM64 전용 준비의320 raw/SDK 비교에서 전체288 보호 바이트와 carry/secondary가 일치했다. 자동 com.apple.provenance는 관찰 값이며 기본 빈 목록을 뜻하지 않는다. 네이티브 Intel과 실제 iOS 장치는 미검증이며 전체 dyld, Mach IPC, Objective-C/Swift, 프레임워크는 미완성이다.

Sources: [XNU syscall ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU xattr calls](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ordinary attributes](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_xattr.c), [XNU xattr definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Original code/probes; Apple implementation is not copied.

## 제한된 객체 이름

ATTR_CMN_NAME=1은 getattrlist220/fgetattrlist228/getattrlistat476에서 명시적 카탈로그의 이름이 유일한 비루트 객체를 지원합니다. 리프 이름은 유효한 UTF-8 1..255바이트입니다. F_GETPATH와 공유하는 실제 객체 경로는 dup, CWD, 이동, SWAP, 삭제와 이름 재사용 뒤에도 마지막 연결 이름을 유지하며 호출자 별칭을 사용하지 않습니다. 이름과 유형은 stat 없이 조회하지만 선택한 stat 필드는 완전한 유효 관찰이 필요합니다. 루트/마운트 이름, 잘못된 이름, 하드 링크·대소문자 별칭, 정규화와 전체 경로 속성은 미지 상태입니다.

8바이트 attrreference_t는 다른 공통 필드보다 앞에 있고 attr_dataoffset은 참조 자체 기준이며 attr_length는 NUL을 포함합니다. 이름 영역은4바이트 정렬됩니다. 짧은 출력은 전체 필요 길이와 UTF-8 중간을 포함한 정확한 접두 바이트를 유지합니다. attribute-names / attribute-names-values / attribute-names-unsupported는 guest/C/CLI/Python에서 원시 동작, 독립 바이트와 기존 출력을 유지하는 루트 이름 중단을 확인합니다. ARM64 준비는601 raw 조회,453 전체 보호 버퍼 SDK 비교,384 접두 검사를 통과했습니다. SDK15.5에는 raw476 형식 선언이 없습니다. native5s, guest/Python5,000,000us/quantum1024, 기존 공개10s는 그대로입니다. 원시 Intel, 실제 iOS, 전체 런타임/프레임워크는 미검증 또는 미완성입니다.

## 제한된 디렉터리 일괄 속성

getattrlistbulk(461)은 명시적인 enumeration_policy.bulk_attributes=true가 필요하며 생략 또는 false는 권한을 주지 않는다. 이 가상 TYPE 계약은 현재 직계 자식 이름을 부호 없는 바이트 순서로 반환한다. 점 항목 없이 로컬 서수를 사용하며 네이티브 파일 시스템 cookie를 추론하지 않는다. 초기 디렉터리 객체는 dup, 이동, SWAP, 삭제, 이름 재사용 후에도 권한을 유지하고 새 디렉터리는 일괄 권한을 상속하지 않는다. minimum_buffer_size, initial_minimum_buffer_size, seek_offset은 getdirentries64 전용이다.

NAME|OBJTYPE|RETURNED_ATTRS (0x80000009)가 필수이며 선택한 관측이 유효하면 기존 열한 공통 필드를 사용할 수 있다. Options0/8을 허용한다. bulk는 두16비트 bitmap/reserved 워드를 독립 attrlist 검증과 별도로 무시한다. 단일 속성 인코더가 attrreference_t와 stat64 유효성을 공유한다. 완전한 레코드만 반환하며 공간이 충분하면8바이트 패딩, 마지막 그룹은4바이트 크기도 허용한다. 첫 그룹이 맞지 않으면 ERANGE로 출력과 커서를 유지한다. 필요한 출력 일부만 쓰기 가능하면 복사 전에 명시적으로 중단한다. 실제 반환 바이트만 쓰기 가능한 메모리가 필요하다.

dup는 진행을 공유하고 별도 open은 독립적이다. 0이 아닌 완료 순회는 이름 공간 변경 뒤에도 EOF를 유지하고 요청 검증 후 크기/출력 검사를 건너뛴다. 처음 빈 디렉터리 offset0은 뷰를 다시 확인한다. 0 lseek는 반복을 재설정한다. EOF 전 구성 변경, 임의의 0 아닌 seek, getdirentries64/bulk 혼합은 중단한다. NAME-only 경로, ERROR 항목, 스냅샷, ACL/권한 판단, 호스트 순서와 다른 mask/options는 미지원이다. bulk-attributes / bulk-attributes-values / bulk-attributes-unsupported는 네이티브 공통 동작, 가상 리터럴과 이전 출력을 보존하는 미지원 선택을 검사한다. ARM64 비공개 준비에서 보호된 raw/SDK 비교728개가 통과했다. native5s, guest/Python5,000,000us/quantum1024, public10s는 그대로다. 네이티브 Intel, 실제 iOS, 완전한 런타임/프레임워크는 미검증 또는 미완성이다.

Sources: [XNU bulk ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/man/man2/getattrlistbulk.2), [XNU attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h). Original implementation and probes; no Apple implementation copied.

## 명시적으로 허가한 일반 확장 속성 변경

setxattr(236), fsetxattr(237), removexattr(238), fremovexattr(239)는 초기 객체에 대한 독립적인 허가를 사용합니다. C++에서는 `MutableExtendedAttributes`, JSON에서는 엄격한 불리언 `mutable_extended_attributes=true`입니다. 허가받은 파일, 디렉터리, 링크에는 알려진 빈 목록을 포함한 완전한 일반 `extended_attributes` 목록이 필요합니다. 내용 쓰기나 이름 공간 변경 허가는 이를 대신하지 않습니다. 허가와 값은 dup, 이동, 삭제 및 매핑 유지 기간에도 보존된 객체에 속합니다. 새 객체와 재사용한 이름의 속성은 처음에 알 수 없습니다. 알려진 별칭, 충돌하는 메타데이터 플래그, 보호된 시스템 속성, ResourceFork, FinderInfo 및 압축 의미는 제외합니다.

교체는 가상 목록의 위치를 유지하고, 삭제는 항목을 제거하며, 생성은 끝에 추가합니다. 이 순서는 프로세스 내에서 선언하며 APFS 순서를 추정하지 않습니다. 초기 속성의 바이트 수, 개수 및 허가 경로 참조는 계속 예약됩니다. 런타임 증가분은 기존 16 MiB/4096 한도에서 함께 관리하며, 삭제, 내용 무효화 및 최종 객체 해제는 증가분만 회수합니다. 용량, 전송 및 기한 실패는 임시 상태를 공개하지 않습니다. 필요한 입력을 전혀 읽을 수 없으면 EFAULT, 일부만 읽을 수 있으면 공개 전에 명시적으로 미지원 상태로 중단합니다. 변경 성공은 시간을 추정하지 않고 완전한 stat을 무효화하며, 객체 식별 정보, 디렉터리 구성, 열거 버전/스냅샷과 커서를 유지합니다.

변경 ABI는 low32 FD/options/position과 full64 size를 사용합니다. 특권 및 FD 링크 옵션의 초기 검사는 이름 가져오기보다 먼저, 이름 가져오기는 객체 조회보다 먼저 수행합니다. set은 과도한 VFS 입력(E2BIG7)을 검사하기 전에 길이가 0이 아닌 NULL을 거부합니다. 조회는 일반 이름, position 및 충돌 검사보다 먼저입니다. set은 전체 값을 가져온 다음 기존 항목의 CREATE에 EEXIST17, 없는 항목의 REPLACE에 ENOATTR93을 반환합니다. CREATE와 REPLACE를 함께 지정하면 EINVAL이며, 삭제는 두 비트를 무시합니다. 길이가 0인 set은 값 포인터를 읽지 않습니다. 다른 플래그, 알 수 없는 권한 및 관측하지 않은 제공자 동작은 명시적으로 중단합니다.

xattr-mutations / xattr-mutations-values / xattr-mutations-unsupported는 직접 작성한 네이티브/게스트 대조, 독립적인 가상 바이트 리터럴, 기존 출력을 유지하는 허가 부족 중단을 검사합니다. ARM64 비공개 준비는726회 raw/SDK 호출, 전체544바이트 보호 영역 관측 및 읽을 수 있는 전체 페이지를 확인했습니다. native5s/compile120s/drain1s/reap1s, guest/Python5,000,000us/quantum1024, public10s는 그대로입니다. 네이티브 Intel, 실제 iOS 기기, dyld, Mach IPC, 스레드/신호, Objective-C/Swift 런타임 및 전체 프레임워크는 아직 검증되지 않았거나 미완성입니다.

주요 ABI 자료: [XNU 시스템 호출 선언](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [xattr 정의](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). 코드와 프로브는 직접 작성했으며 Apple 구현을 복사하지 않았습니다.

## 범위가 제한된 Darwin 하드 링크

link는 마지막 심볼릭 링크 대상을 따라가며 linkat flags=0은 링크 객체, AT_SYMLINK_FOLLOW는 대상을 선택한다. low32 0/0x40만 허용하고 나머지 하위 비트는 입력 전에 EINVAL이다. 소스 조회와 디렉터리 EPERM이 대상 입력보다 먼저이며 기존 대상은 EEXIST다. 대상 수정 권한과 명시된 같은 마운트 영역이 필요하다. 초기 식별 별칭과 알려진 장치/모드/플래그 충돌은 미지원이다.

별칭은 항목 및 경로/NUL 비용만 추가하고 새 inode를 소비하지 않는다. 바이트, 속성 권한, 메타데이터 유효성과 매핑 임대는 공유 객체가 소유한다. 명시 정책이 링크 수와 ctime을 갱신하며 정책이 없으면 전체 stat는 미지다. 속성 변경은 stat를, 내용 변경은 속성 관찰을 무효화한다. 설명 및 마지막 매핑이 삭제 이름의 비용을 유지하며 교체는 즉시 해제 가능한 비용만 공제한다. 하위 트리는 정확한 식별과 부모로 이동하고 외부 별칭은 유지한다. 상대 심볼릭 대상은 선택 항목의 부모를 사용한다.

여러 이름을 가졌던 객체의 F_GETPATH/ATTR_CMN_NAME은 한 개 또는 0개가 남아도 미지원이다. APFS 캐시 모델을 일반화하지 않는다. bulk NAME은 실제 항목을 사용하며 같은 객체의 일반 rename/SWAP는 두 이름을 유지한다. EXCL 대소문자, Intel HVF, 실제 iOS, ACL, 매핑 일관성/EOF 신호, dyld, Mach IPC, 스레드 및 전체 프레임워크는 별도 과제다. 이 계약 범위에서만 이전 제외를 확장한다.

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

## 제한된 O_SYMLINK 설명자

O_SYMLINK=0x00200000은 읽기 전용, 쓰기 전용, 읽기/쓰기에서 마지막 심볼릭 링크 자체를 보존하며 끊어진 링크와 순환 링크도 포함합니다. 대상 내용의 쓰기 권한은 부여하지 않습니다. O_CREAT는 마지막 대상을 따라가고 NOFOLLOW의 ELOOP, 독점 생성의 EEXIST, 보존된 링크에 대한 O_DIRECTORY의 ENOTDIR 순서를 유지합니다. 중간 또는 끝 슬래시 확장은 기존 해석기와 NOFOLLOW_ANY를 따르며 F_GETFL은 선택 비트를 제외합니다.

실제 LinkNode와 선택한 NameIdentity를 보존합니다. dup은 상태 플래그와 커서를 공유하고 독립 open은 별도 설명을 사용합니다. 이름 변경, 삭제, 이름 재사용, 부모 삭제 후에도 기존 객체가 유지됩니다. 단일 이름의 F_GETPATH/ATTR_CMN_NAME은 선택한 보존 이름을 사용합니다. 여러 이름을 가졌던 객체는 이름을 모두 삭제해도 vnode 이름 추론을 거부합니다. 초기 예약은 고정이며 동적 이름, 대상, 항목, 속성 증가 비용은 마지막 소유자까지 유지됩니다. 다른 설명자가 객체나 선택한 이름을 보존하면 마지막 별칭을 교체 비용에서 공제할 수 없습니다.

I/O는 대상 문자열을 파일 내용으로 노출하지 않습니다. 기존 스칼라/벡터 가져오기, 접근, 개수 검사 후 음수 오프셋은 EINVAL입니다. INT64_MAX 읽기는 0, 다른 허용 오프셋은 길이 0을 포함해 EPERM입니다. INT64_MAX 쓰기는 EFBIG, 나머지는 길이 0 성공, APPEND, 데이터 접근 전에 EPERM입니다. pwrite/pwritev의 기존 조기 음수 규칙을 유지합니다. DATA/HOLE seek은 음수가 아니면 ENXIO, 음수면 EINVAL이며 커서는 그대로입니다.

쓰기 전용/읽기·쓰기의 음수가 아닌 ftruncate와 허용 open TRUNC는 WasWritten만 설정하며 대상 바이트, 전체 stat, 속성, 커서, 저장 예산, inode를 변경하지 않습니다. 읽기 전용 또는 음수 길이는 EINVAL입니다. 허용 F_SETFL은 APPEND|NONBLOCK를 변경한 후 ENOTTY25를 반환하고 dup에 반영됩니다. 알 수 없는 인수는 효과 전에 중지합니다.

고정 fpathconf, fgetattrlist, 독립 선언된 일반 FD 속성 권한은 심볼릭 객체에 적용됩니다. 상대 디렉터리 FD와 fchdir는 ENOTDIR입니다. 속성 변경으로 무효화된 stat을 truncate가 복구하지 않습니다. 정렬된 비실행 레거시 private/shared mmap은 객체 종류에서 EINVAL로 거부되며 매핑이나 임대를 만들지 않습니다. 일반 shared 매핑, 알 수 없는 플래그, 실행 보호 등 기존 미지원 경계는 유지됩니다. 네이티브 mmap은 length16384, offset0, protection1/2/3의18개 조합만 검증합니다.

독자 ARM64 준비는15개 truncate의 전체144바이트 stat, 극단 오프셋/개수 I/O, 희소 seek, 실패 F_SETFL 효과를 확인합니다. SDK 없는 공통 프로그램은 O0/O1/O2에서 실행하고 실제 부모 설명자 경로를 비교합니다. 가상 경로는 독립 stat/type 리터럴 또는 기존 출력을 보존하는 다중 이름 거부를 검증합니다. Intel HVF, 실제 iOS, ACL/권한, 매핑 EOF/신호, dyld, Mach IPC, 스레드, 전체 런타임/프레임워크는 미검증 또는 미완성입니다.

일차 해석 자료: [동일 버전 XNU 매핑 경계](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_mman.c). 구현과 탐침은 독자 작성하며 Apple 구현을 복사하지 않습니다.

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

## 제한된 비차단 설명자 상태

일반 파일, 디렉터리, O_SYMLINK open은 O_NONBLOCK=4를 허용하며 F_GETFL이 이를 보존합니다. dup은 상태와 위치를 공유하고 독립 open은 별도 설명을 유지합니다. 접근, close-on-exec, WasWritten, 메타데이터, 바이트, 위치의 기존 규칙이 적용됩니다. 명시한 유한 stdin은 EOF와 포인터 오류 순서를 유지하고 생략 입력은 미지 상태입니다. 출력 캡처는 복사 오류와 공유 예산을 유지합니다.

F_SETFL은 효과 전에 허용 low32 인수를 검사하고 원시 open 플래그 변환으로 1을 더한 뒤 APPEND|NONBLOCK만 변경합니다. high32는 무시하며 접근 및 입력 WasWritten 비트로 권한이나 쓰기 이력을 만들지 않습니다. 독립 원시 관찰에서 요청3/7/11/15는 상태4/8/12/0을 선택합니다. 심볼릭 설명은 상태 변경 후 ENOTTY25를 반환합니다. ASYNC0x40 등 미지 플래그는 효과 전에 중지합니다.

독자 ARM64 macOS 준비의122개 관찰은 유효 객체/접근 조합별16개 요청, 유지 dup, 독립 open, 상태 해제, 실제 쓰기 및 FD별 CLOEXEC를 포함합니다. SDK 없는 공통 프로그램은 O0/O1/O2에서 바이트, 상태, 위치와 원시 BSD carry/errno ABI를 비교합니다. 게스트, C/CLI, Python도 미지 플래그 거부를 검증하며 ARM64 HVF 세 profile의 실제 실행이 필수입니다. 준비 상태 대기, 파이프, 네트워크, kqueue, 비동기 신호나 호스트 I/O는 추가하지 않습니다. O_EVTONLY 프로세스 정책은 미지원이며 Intel HVF, 실제 iOS, 전체 macOS/iOS 환경은 미검증 또는 미완성입니다.

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

## 명시적인 유한 getentropy 관측

원시 BSD getentropy500은 순서가 있는 `DarwinSystemOptions::EntropyReads`를 사용하며 JSON 필드는 `darwin_system.entropy_reads`입니다. 비어 있지 않은 짝수 길이 16진수 문자열을 최대256개, 각1..256바이트로 제공합니다. 모델 한도이며 기존65536바이트 JSON 전송 한도는 유지합니다. 생략은 알 수 없음, `[]`는 명시적 소진입니다. 이미지나 백엔드 변경 전에 입력을 검증하며 다른 OS 프로필은 Darwin 옵션을 거부합니다.

전체64비트 길이를 먼저 확인합니다. 256 초과는 메모리 접근이나 소비 없이 EINVAL22, 길이0은 모든 포인터에 대해 입력 없이 성공합니다. 비영 요청은 다음 기록의 정확한 길이를 먼저 승인합니다. 누락·소진·불일치는 잘못된 주소에서도 효과 전 UnsupportedService로 멈춥니다. 이는 재생 승인 순서입니다. 성공 또는 완전히 쓸 수 없는 EFAULT14는 한 기록을 소비합니다. 일부만 쓸 수 있는 대상은 복사 및 커서 변경 전에 거부하며 전송 오류는 커서를 진행하지 않습니다. 이후 반환 레지스터 실패는 완료된 효과를 유지합니다. 같은 옵션을 재사용해도 매 실행은 첫 기록에서 시작합니다.

매 실행의 DarwinEntropy가 독립 커서를 소유하고 입력 바이트는 불변입니다. 기존 BSD 분배와 returnService가 양쪽 ISA, 캐리와 보조 레지스터를 담당합니다. SDK 없는 재생은5개 게스트와3개 ARM64 HVF 프로필을 검증하며 고정 바이트는 원시 결정적 RNG 목록에 넣지 않습니다. ARM64 O0/O1/O2 탐침은594회 호출이며 센티널 변화 수는 정확한 복사 길이가 아닙니다. 호스트 난수, 암호학적 품질, /dev/random, libc 가져오기와 프레임워크는 제공하지 않습니다. Intel HVF·물리 iOS·완전한 OS 호환성은 미검증 또는 미완료입니다。

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

## 명시적인 현재 스레드 식별자

BSD thread_selfid372는 변경되지 않는 선택적 `DarwinSystemOptions::ThreadID` / JSON `darwin_system.thread_id`를 읽습니다. 0을 포함한 모든 uint64 비트 패턴은 알려진 관측이며, 생략하면 UnsupportedService로 중단합니다. 십진 문자열은64비트를 보존하고 JSON 숫자는2^53-1 이하의 정확한 정수만 허용합니다. 호스트, PID 또는 Mach 포트에서 ID를 추정하지 않습니다. 인자 없는 호출은 여섯 전달값을 무시하고 메모리에 접근하지 않습니다. 기존 하위32비트 해석은 이벤트의 원래 전체 번호를 보존하며 BSD 반환 계층은64비트 값과 carry 및 RDX/X1 초기화를 담당합니다. Mach 번호 형식은 지원하지 않습니다.

반복 실행은 입력 관측을 유지하며 서로 다른 옵션은 독립적입니다. ID 할당, 유일성, 스케줄러 이벤트의 식별, 스레드 생명주기, pthread, TLS, Mach IPC를 제공하지 않습니다. ARM64 O0/O1/O2 원본 프로브24회는 SDK의 현재 pthread ID, 임의 인자와 번호 상위32비트를 비교합니다. 공통 네이티브 프로그램은 같은 프로세스 내부 관계만 비교하며 고정 ID 바이트는 결정적 네이티브 참조 목록에서 제외합니다. Intel HVF, 실제 iOS 기기 및 완전한 OS 호환성은 검증되지 않았거나 미완성입니다.

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
