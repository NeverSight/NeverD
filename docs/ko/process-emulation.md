**언어**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← 문서 색인](README.md)

# 게스트 프로세스 에뮬레이션

`neverd emulate`는 명시적 게스트 OS 프로필로 이미지를 실행합니다. CPU 전송, 이미지 파싱, 프로세스 진입, OS 서비스는 각각 별도 소유 경계입니다. `NEVERD_ENABLE_CPU_EMULATION=ON`으로 활성화합니다. 드라이버 에뮬레이션에도 포함됩니다.

첫 프로필 `linux-elf64-v1`은 CPL3 또는 EL0에서 x64/AArch64 ELF `ET_EXEC`와 자체 재배치 static PIE `ET_DYN`을 실행합니다. 실제 ELF 세그먼트를 적재하고 초기 스택을 만들며 명령 quantum으로 재개하고 명시적 Linux 시스템 호출 요청을 처리합니다. 이는 완전한 Linux 배포판이나 임의 libc 바이너리 실행을 보장하지 않는 독립 프로세스 모델입니다. 동적 링크, 시그널, 스레드, 파일 시스템, 미지원 서비스는 명시적으로 실패합니다.

<!-- i18n-section: cli-sdk -->

## CLI 및 SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

호환 Linux 호스트는 KVM, 호환 Windows 호스트는 WHP를 선택합니다. 그 외 호스트/게스트 ISA 조합은 Unicorn을 사용합니다. 선택한 백엔드를 사용할 수 없으면 조용한 대체 없이 오류입니다. Windows에서도 ELF 게스트는 Linux 프로세스 모델을 사용합니다. 명령 목록과 제한은 [CPU 실행](cpu-execution.md)을 참조하세요.

CLI는 JSON 보고서 하나를 출력합니다. 게스트 상태 0은 종료 코드 0, 다른 상태는 2, 불완전 실행(오류/한도 포함)은 3, 잘못된 설정/API는 1입니다. 실제 게스트 상태는 `exit_status`에 있습니다. 추가 C 진입점 [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h)은 session, 비어 있지 않은 입력 경로, 명시 프로필, 선택적 옵션 JSON을 받습니다. 결과는 `neverd_free_string`으로 해제합니다. NULL은 설정 실패이며 `neverd_last_error`에 원인이 있습니다. 게스트 fault나 자원 중지도 보고서를 반환합니다. session의 분석용 로드 이미지는 필요하지 않고 변경되지 않습니다.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## 옵션과 결과

옵션은 최대 64 KiB JSON 객체입니다. 알 수 없는/null 필드, 잘못된 유형, 문자열 안의 NUL, 양수가 아닌 한도는 거부됩니다.

| 옵션 | 기본값 | 계약 |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm`, `whp` |
| `arguments` | 입력 파일명 | argv[0] 포함 완전한 argv; 비우면 기본값 |
| `environment` | `[]` | 명시적 게스트 문자열; 호스트 환경을 상속하지 않음 |
| `instruction_limit` | 100000 | 공유되는 허용 명령 시도 수 |
| `event_limit` | 10000 | OS 서비스 처리 전에 차감되는 syscall 이벤트 수 |
| `timeout_microseconds` | 5000000 | 프로세스 설정 후 시작하는 단조 deadline |
| `memory_limit` | 67108864 | 물리/매핑 메모리 예산 |
| `stack_size` | 1048576 | 예산 내 페이지 정렬 스택 |
| `output_limit` | 1048576 | stdout/stderr 합산 캡처 바이트 |
| `instruction_quantum` | 1024 | runtime으로 양보하기 전 admission 간격 |

`schema_version`은 1입니다. 보고서에는 프로필, 아키텍처, 선택 백엔드와 이유, `stop_reason`, nullable `exit_status`, 진단, 진입/현재 PC, 카운터, 서비스 기록, 마지막 typed CPU exit가 포함됩니다. 주소, syscall 번호, 인자 레지스터, raw 반환 비트는 `0x` 없는 16진수 문자열입니다. `stdout_hex`/`stderr_hex`는 NUL과 잘못된 UTF-8을 보존합니다. syscall 결과 null은 모델링된 반환이 없다는 뜻(exit 또는 미지원 요청 등)이지 성공한 0이 아닙니다.

<!-- i18n-section: linux-semantics -->

## Linux 프로필 의미론

OS 정책은 기존 ELF 로더가 디코딩한 프로그램 헤더를 사용합니다. ABI 태그, 세그먼트 정렬, 매핑된 프로그램 헤더 테이블, user 주소 범위를 검증합니다. 매핑 계획은 할당 전에 범위, 권한, 겹침, 예산을 확인하고 완전히 준비한 전용 주소 공간만 공개합니다. 파일 페이지 앞/뒤 바이트를 보존하고 BSS를 0으로 채우며 세그먼트 권한을 지키고 스택 guard gap을 예약합니다. 페이지가 겹치는 레이아웃과 모순 헤더는 추측하지 않고 거부합니다.

Static PIE는 최소 `0x40000000`의 결정적 load bias를 사용하고 더 큰 `PT_LOAD` 정렬 요구에 따라 높입니다. 모든 매핑 세그먼트, entry PC, `AT_PHDR`/`AT_ENTRY`가 같은 bias를 사용하며 원래 program-header 값은 유지되고 interpreter가 없으므로 `AT_BASE`는 0입니다. 매핑 원본은 분석 fixup이 적용되지 않은 파일 바이트입니다. guest 시작 코드가 직접 relocation과 초기화를 수행해야 합니다. loader는 section header 없이 원본 파일의 제한된 record에서 `PT_DYNAMIC`을 decode합니다. 존재 시 readable/terminated 상태이며 최대 4096개 항목이어야 합니다. `PT_INTERP`와 외부 dependency/filter/audit tag는 거부합니다. dynamic linker, symbol resolver, constructor runner는 제공하지 않습니다.

초기 스택은 정렬된 argc/argv/envp/auxv, PHDR/PHENT/PHNUM, entry, 페이지 크기 및 identity 값을 포함합니다. 모델 PID/TID/UID/GID는 1000입니다. 재현성을 위해 `AT_RANDOM`은 입력 SHA-256의 첫 16바이트입니다. 이는 암호학적 엔트로피가 아닌 결정적 모델 정책입니다. HWCAP/HWCAP2는 0이며 vDSO는 없습니다.

`write`, `exit`, `exit_group`, `getpid`, `gettid`, `mmap`, `mprotect`, `munmap`, `brk`를 구현하며 번호는 [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)와 [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)에서 다릅니다. 반환하는 x64 SYSCALL은 RCX/R11 clobber, RAX, 다음 PC를 반영합니다. ARM64는 번호에 x8, 결과에 x0을 사용합니다. 나머지는 `unsupported_service`로 중단하며 호스트 syscall을 실행하지 않습니다.

Static TLS template `PT_TLS`는 loader 소유 사실로 검증합니다. template 하나, 제한된 file/memory 범위, 일치하는 정렬, 읽을 수 있는 초기화 바이트가 필요합니다. guest startup이 TLS block을 할당·초기화하고 thread pointer를 설치합니다. Linux 모델은 libc별 TCB/DTV를 만들지 않습니다. 이에 따라 freestanding 프로그램의 컴파일러 생성 local-exec TLS를 지원합니다. dynamic TLS와 OS 스레드는 별도 작업입니다.

x64 `arch_prctl`은 `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS`, `ARCH_GET_GS`를 지원합니다. Set은 매핑되지 않은 user 범위 base도 받아들이지만 이후 역참조는 권한을 검사합니다. kernel 범위 base는 guest `EPERM`, 잘못된 Get 대상은 CPU fault 없이 `EFAULT`를 반환합니다. 나머지 operation은 명시적으로 실패합니다. ARM64 시작은 `MSR`로 `TPIDR_EL0`를 설정합니다. `MRS`, FS/GS 메모리 접근, context 복원은 quantum 및 backend 진입 사이에서 thread pointer를 보존합니다. 이는 thread scheduler를 구현하지 않습니다.

파일 디스크립터 1과 2는 가상 바이트 sink입니다. `write`는 읽기 가능한 user 페이지를 검증하고, 뒤쪽 페이지 접근이 막히면 읽을 수 있는 prefix를 반환하며 한 바이트도 읽지 못하면 게스트 `EFAULT`를 반환합니다. 잘못된 디스크립터는 `EBADF`; 유효한 디스크립터에 0바이트 write는 포인터를 읽지 않습니다. Linux pipe 원자성이나 파일 객체는 모델링하지 않습니다. 출력 한도를 넘을 쓰기는 게시 전에 중단됩니다.

익명 메모리 서비스는 이미지 및 스택과 같은 프로세스 주소 공간과 물리 메모리 예산을 사용합니다. `mmap`은 정확히 `MAP_PRIVATE | MAP_ANONYMOUS`와 일반 `PROT_NONE`, `PROT_READ`, `PROT_READ | PROT_WRITE`, `PROT_READ | PROT_EXEC` 또는 읽기 가능한 RWX 권한을 허용합니다. 비어 있고 페이지 정렬된 힌트를 우선하며, 그렇지 않으면 `0x100000000`부터, 이어 최소 사용자 주소부터 빈 영역을 찾고 스택 보호 영역을 보존합니다. 이 결정적 배치는 Linux ASLR을 모방하지 않습니다. 새 페이지는 개별 소유하며 0으로 채우므로 부분 해제로 고정되지 않은 페이지를 회수할 수 있습니다. CPU 투영이나 유지된 backing view는 자신의 수명이 끝날 때까지 폐기된 할당을 유지할 수 있습니다.

길이는 페이지 단위로 올림합니다. `munmap`은 빈 영역과 반복 해제를 허용하며, `mprotect`는 빈 영역 앞의 매핑을 변경한 후 `ENOMEM`을 반환합니다. `PROT_NONE`은 할당과 바이트를 보존하면서 게스트 접근을 거부합니다. 원시 `brk`는 성공 시 요청한 바이트 경계, 실패 시 이전 경계를 반환하며 libc의 0/-1 규약을 사용하지 않습니다. 초기 break는 페이지 정렬된 이미지 끝입니다. 확장은 다른 매핑과 예산을 준수하며 축소는 남은 부분 페이지의 바이트를 보존합니다. 지원 범위의 규칙과 오류 우선순위는 Linux [매핑](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c) 및 [보호](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c)를 따릅니다.

파일/공유/고정 매핑, 아래 방향 확장, 대형 페이지, 메모리 잠금, 보호 키, 실행 전용/쓰기 전용 정책 및 다른 플래그는 명시적으로 지원하지 않습니다. 효과를 게시하거나 반환값을 만들기 전에 중단합니다. 지원 범위 안의 일반 범위/길이/정렬 오류는 게스트 오류를 반환하고 실행을 계속합니다. 게스트 포인터나 매핑 요청을 호스트 OS에 전달하지 않습니다.

<a id="windows-pe64-profile"></a>

<!-- i18n-section: windows-pe64 -->

## Windows PE64 프로필

`windows-pe64-v1`은 PEB/TEB, EXE TLS, 명명된 Win32 API, 명시적인 비순환 시작 DLL 그래프를 갖춘 제한된 Windows x64/ARM64 콘솔 프로세스를 지원합니다. DLL의 이름/서수 코드·데이터 가져오기, DIR64 재배치, 실제 로더 목록을 지원합니다. DLL 진입점/TLS, 동적 로딩, 전달된 내보내기, CRT/GUI, 사용자 SEH와 스레드는 미완성입니다. 네이티브 ARM64 KVM/WHP 실행 증거도 아직 없습니다.

Windows 가상 메모리는 `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery`와 현재 프로세스의 `FlushInstructionCache`를 지원합니다. OS 계층은 예약 영역을 소유하고 `AddressSpace`는 커밋된 페이지, 권한, 실제 저장 공간을 관리합니다. 테스트는 동적 코드 수정, 접근 오류, 메모리 한도 재사용을 검증합니다.

전용 할당은 `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT`, `MEM_RELEASE`, `MEM_TOP_DOWN`을 지원하며 예약 정렬은 64 KiB, 페이지는 4 KiB입니다. 예약만으로는 게스트 RAM을 사용하지 않습니다. 재커밋은 데이터를 보존하며 권한을 갱신하고 커밋 해제는 각 페이지의 저장 공간을 반환합니다. 전체 범위 검증과 사전 할당으로 일반적인 실패 시 일부만 변경되는 일을 방지합니다. 조회는 48바이트 x64/ARM64 메모리 정보 구조를 반환하고 동일한 할당 안에서만 이후 영역을 합칩니다. 초기 이미지, 환경, 힙 영역, API 진입점, 스택 경계도 배치에 반영하며 스택 할당 식별은 TEB와 일치합니다. 성공한 `VirtualProtect`가 이전 권한의 출력 위치를 읽기 전용으로 바꾸면 새 권한은 유지되고 출력 내용은 바뀌지 않으며 호출은 성공을 반환합니다. 커밋되지 않은 페이지를 포함한 범위의 권한 변경은 `ERROR_INVALID_ADDRESS`를 반환하고 이전 권한 출력에 `PAGE_NOACCESS`를 기록하며 페이지 권한은 변경하지 않습니다.

지원 권한은 `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE_READ`, `PAGE_EXECUTE_READWRITE`입니다. 가드 페이지, 실행 전용, 쓰기 시 복사 정책, 캐시 수식자, 대형 페이지, reset/write-watch/자리 표시자 및 모델이 소유한 런타임 매핑 변경은 명시적으로 거부합니다. 전용 가상 할당만 커밋 해제하거나 해제할 수 있습니다. 사용자 예외 전달이나 ARM64 네이티브 하드웨어 검증은 포함하지 않습니다.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

단일 스레드 PE32+ EXE는 선호 기준 주소를 유지하며 진입점이 0이고 TLS 디렉터리가 없는 명시적 DLL을 허용합니다. `WindowsProcessOptions::Modules` 또는 JSON `windows.modules`의 `name`, `path`로 최대 64개 게스트 기본 이름과 호스트 입력 경로를 지정합니다. 호스트 DLL을 검색하거나 실행하지 않습니다. ASCII 이름은 대소문자를 구분하지 않으며 중복과 시스템 API 제공자 재정의를 거부하고 도달 가능한 파일만 읽습니다. 이름/서수 함수·데이터 가져오기는 실제 내보내기에 연결됩니다. 빈 서수, 없는 심볼, 순환, 전달, bound/delay import와 미지원 load configuration/CFG는 실패합니다. 이동 가능한 DLL 충돌에는 DIR64를 적용하며 고정 주소 충돌과 링크 메타데이터를 덮는 재배치는 CPU 생성 전에 거부합니다.

`readPEProgramExports`는 원본 내보내기와 읽기 범위를, `WindowsProcessModules`는 그래프와 프로세스 공통 제공자/이름 API 게이트를 소유합니다. `VirtualMemory`가 모든 이미지를 먼저 예약하고 `AddressSpace`가 페이지와 권한을 관리합니다. PEB/LDR에는 실제 이미지만 있으며 초기화 목록은 DLL 의존 순서입니다. `GetModuleHandleW`는 NULL 또는 ASCII 기본 이름을 받으며 대소문자를 무시하고 확장자가 없으면 `.dll`을 붙입니다. 경로, 비 ASCII 조회, 끝의 점 규칙은 미지원입니다. 없는 이름은 오류 126, 성공은 LastError를 유지합니다. API 모델은 설치된 DLL이 아닙니다.

입력 총 바이트와 이미지 전체 범위는 각각 `memory_limit`로 제한하고 런타임 매핑도 이미지 예산에 포함합니다. 준비 단계는 65,536개 레코드, 64 MiB 메타데이터 읽기, 이름 길이와 전체 작업 기한을 공유합니다. 호스트 I/O의 강제 시간 보장은 없습니다. 독자 EXE→DLL→DLL은 재배치, 서수, 공유 데이터, API 포인터, `MEM_IMAGE`, 목록, EXE TLS attach/detach를 확인합니다. `NeverDWindowsProcessTests`는 원본 네이티브 Windows 대조, `NeverDPEProgramExportsTests`는 잘못된 메타데이터와 예산, `NeverDProcessPublicTests`는 C ABI/CLI 일치를 검증합니다. 사용할 수 없는 백엔드는 명시적으로 건너뜁니다.

```json
{"windows":{"modules":[{"name":"middle.dll","path":"inputs/middle.dll"},{"name":"leaf.dll","path":"inputs/leaf.dll"}]}}
```

x64 GS와 ARM64 x18은 TEB를 가리키며 스택 경계, self, PID/TID, PEB, 프로세스 매개변수, LastError와 TLS를 제공합니다. UTF-8을 엄격히 UTF-16으로 변환하고 argv는 Microsoft CRT 규칙으로 인용합니다. 환경 이름은 ASCII이며 대소문자 무시 중복을 거부합니다. 값은 Unicode가 가능하며 정렬된 환경은 이중 NUL로 끝납니다. 호스트 환경과 파일 시스템은 상속하지 않습니다. 정적 TLS는 템플릿/BSS/32비트 인덱스를 초기화하고 동적 TLS는 별도 TEB 슬롯을 사용합니다. 시작·종료는 변경된 콜백 배열을 순서대로 읽으며 기한과 예산을 공유합니다. 진입점 반환과 정상 종료 모두 종료 콜백을 실행하고 종료 중 재귀 종료는 명시적으로 중단합니다.

정확한 API는 `WindowsProcessServices.def`에 있습니다. `ExitProcess`, `RtlExitUserProcess`, 표준 출력 핸들과 동기 `WriteFile`, LastError, 프로세스/스레드 ID와 의사 핸들, `GetCommandLineW`, 힙 할당/해제/크기, 동적 TLS, `GetModuleHandleW`를 지원합니다. `kernel32.dll`, `kernelbase.dll`, `ntdll.dll`의 정확한 이름만 해석합니다. 직접 syscall과 위조 콜백 게이트는 API를 선택하지 못합니다. 힙 소유권과 회수, 이진 출력, API 오류와 미지원 비동기 I/O·사용자 예외를 구분합니다. 포인터 별칭도 완료 수 초기 0과 실제 반환 주소 변경을 반영합니다.

`windows.native_calls`는 모듈/함수명, 선언된 스칼라 인수, nullable 결과를 기록하며 NT syscall 번호를 만들지 않습니다. `NeverDWindowsProcessTests`는 실제 PE, 컴파일러 TLS, 콜백 변경, 힙, 별칭, 잘못된 메타데이터, 권한과 예산을 검증합니다. `NeverDProcessPublicTests`는 CLI/C ABI를 검증합니다. Windows CI는 같은 EXE를 직접 실행해 독립 비교하고 WHP 검사도 필수입니다. 네이티브 ARM64 실행 증거에는 해당 머신이 필요합니다.

비어 있지 않은 입력 버퍼를 읽을 수 없으면 `WriteFile`은 `ERROR_INVALID_USER_BUFFER`(1784)를 반환하고, 기록한 바이트 수를 0으로 설정하며 아무 바이트도 출력하지 않습니다.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c).

<!-- i18n-section: verification -->

## 검증

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# shared-library/CLI 빌드:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

테스트는 두 ISA의 독립 ELF 진입 어셈블리와 C를 컴파일해 data/BSS, 실제 시작 메타데이터, 시스템 호출 오류, 이진 출력, 권한 오류, 부분 쓰기, 미지원 서비스와 실행 구간 간 예산을 확인합니다. TLS는 독립 정렬 블록, BSS 초기화, 스레드 포인터 설치와 전환 후 보존을 검사하며 x64는 `arch_prctl` 오류 후 이전 베이스 보존도 검사합니다. 사용할 수 없는 백엔드는 건너뜀을 명시합니다. 공개 테스트는 공유 C ABI/CLI의 보고서와 종료 코드를 비교합니다. 정적 PIE는 자체 데이터/함수 포인터 재배치 전에 auxv와 원래 0인 RELA 슬롯을 검사합니다. 매핑 테스트는 분석 바이트를 선택할 때의 fixup 보존을, 동적 테이블은 section 부재 및 잘못된/의존 입력을 검사합니다. 익명 메모리 테스트는 두 ISA의 할당, 보호, 빈 영역, 재매핑, 힙 확장/축소 및 처리 가능한 호출 오류를 다룹니다. 실제 게스트 쓰기로 일반/부분 보호 변경 후 오류를 검사합니다. x64는 RW/RX 전환 사이에 같은 주소의 코드를 다시 쓰고 두 버전을 호출하며, 같은 ELF의 Linux 네이티브 실행을 독립 결과/오류 기준으로 사용합니다. 순수 메모리 테스트는 예산 소진, 회수, RAM을 유지하지 않는 권위 있는 매핑 스냅샷을 검사합니다. 교차 컴파일과 Unicorn ARM64 결과는 네이티브 ARM64 KVM/WHP 증거가 아닙니다.
