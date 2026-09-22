**언어**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← 문서 인덱스](README.md)

# Windows 드라이버 에뮬레이션

NeverD의 선택적 드라이버 에뮬레이터는 지원되는 x64 WDM 드라이버의 PE 진입점을 실행하며, 필요하면 명시적인 순차 요청 시나리오를 실행한 뒤 드라이버를 언로드합니다. CPU 실행에는 Unicorn을, Windows 환경에는 NeverD 자체의 제한된 모델을 사용합니다. 드라이버를 호스트 커널에 로드하거나 게스트 API 호출을 호스트 OS 서비스에 전달하지 않습니다.

## 빌드 및 실행

이 기능은 명시적으로 활성화해야 하며 `BUILD_TESTING`과 독립적입니다.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON
cmake --build build-release --target neverd --parallel 4
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --instruction-limit 100000 > driver-report.json
```

보고서는 항상 stdout에 JSON으로 출력되며, 요청 및 설정 진단은 stderr로 출력됩니다. 기본 한도는 게스트 명령어 100000개, 게스트 메모리 64 MiB, 기록 이벤트 10000개, 실행 시간 5000밀리초입니다. 명령어 한도는 양수여야 합니다. 예산이 소진되면 수집한 일부 관찰 결과를 보존하고 실행을 중단합니다.

| 종료 코드 | 의미 |
|-----------|------|
| `0` | 초기화와 요청된 모든 완료 작업이 성공함 |
| `1` | 잘못된 입력/옵션, 설정 실패 또는 빌드 시 기능 비활성화 |
| `2` | 초기화 또는 완료된 요청이 실패 `NTSTATUS`를 반환함 |
| `3` | 지원되지 않는 API, 오류, 예산 한도 등으로 시나리오가 완료되기 전에 실행이 중단됨 |

반환된 실패 상태도 해당 작업의 완료된 관찰 결과입니다. 성공 반환은 이 모델에서 수행한 실행만 설명하며, 드라이버가 Windows에서 작동한다는 사실을 입증하지 않습니다.

## 드라이버 호환성

호환성은 `.sys` 확장자가 아니라 실제 실행된 코드 경로와 의존성에 따라 결정됩니다. 현재 검증 근거는 독자적으로 작성한 freestanding fixture와 Microsoft SIOCTL WDM 예제의 buffered, in-direct, out-direct 경로이며, 디버그 로그 빌드도 포함합니다. 임의의 타사 드라이버와의 호환성을 입증하지는 않습니다.

수용 검사는 수정하지 않은 Pavel Yosifovich의 Zero WDM 샘플에서도 direct READ/WRITE, 원자적 통계와 통계 IOCTL을 확인합니다.

| 드라이버 종류 또는 요구 사항 | 현재 범위 | 부족한 환경 |
|-----------------------------|-----------|-------------|
| 아래 API를 사용하는 x64 소프트웨어 WDM 드라이버 | 제한된 x64 WDM 초기화, 순차 buffered/direct 요청, 작업 항목, 타이머, DPC, 이벤트와 대기, 동작 보고서 및 한도 | 추가로 실행되는 각 API에 명확한 모델이 필요함 |
| `METHOD_BUFFERED` IOCTL | 순차 buffered/direct I/O와 작업 항목 또는 DPC 완료 | 아래 API 부분집합만 지원하며 동시 IRP와 요청 취소는 미지원 |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | 요청 소유 MDL과 시스템 매핑 | 물리 페이지 식별자, DMA와 사용자 매핑 |
| 드라이버가 할당한 MDL | 모델의 비페이지 풀을 설명하는 독립 MDL, 원래 버퍼 주소 공유 | IRP 연결, MDL 체인, 프로브/잠금, 물리 페이지와 사용자 매핑 |
| READ/WRITE | 순차 buffered/direct I/O와 작업 항목 또는 DPC 완료 | 아래 API 부분집합만 지원하며 동시 IRP와 요청 취소는 미지원; `METHOD_NEITHER`와 암묵적 파일 위치도 미지원 |
| `METHOD_NEITHER` | 거부됨 | 사용자 주소 공간 컨텍스트, 접근 검사와 게스트 예외 처리 |
| KMDF / UMDF 드라이버 | 지원하지 않음 | 프레임워크 바인딩, 객체, 큐, 콜백과 해당 호스트 런타임 |
| PnP 버스/기능/필터 드라이버 | API 하위 집합 내에서 초기화가 실행될 수 있으나 장치 스택 수명 주기는 지원하지 않음 | 장치 연결, 하위 드라이버 디스패치, PnP 및 전원 IRP |
| 저장 장치, 네트워크, 디스플레이, 파일 시스템 및 미니필터 드라이버 | 서브시스템 계약을 지원하지 않음 | 포트/클래스/미니포트 프레임워크, NDIS/WFP, 그래픽 또는 파일 시스템 서비스 |
| 작업 항목, 타이머, DPC, 이벤트와 대기 | 현재 실행 IRQL은 디스패치와 작업 항목에서는 `PASSIVE_LEVEL`, DPC에서는 `DISPATCH_LEVEL`입니다 | 아래 API 부분집합만 지원하며 동시 IRP와 요청 취소는 미지원 |
| 프로세스/스레드 콜백, 핸들, 레지스트리/파일 작업 또는 커널 모듈 탐색을 사용하는 드라이버 | 구성한 레지스트리는 지원하며 그 외 동작은 아래 API 범위로 제한 | 객체 관리자, 시스템 상태와 콜백/이벤트 생성 주체 |
| 하드웨어, DMA, PCI, 인터럽트 또는 가상화 드라이버 | 환경을 지원하지 않음 | 장치 모델, 물리 메모리, 버스, 인터럽트와 특권 CPU 상태 |
| x86 또는 ARM64 Windows 드라이버 | 거부됨 | 아키텍처별 로딩, ABI와 실행 모델 |
| CFG, 지원되지 않는 로드 구성, TLS 또는 기타 거부되는 PE 기능이 필요한 x64 이미지 | 로드 시 거부됨 | 해당 요구 사항에 대한 명시적인 로더/런타임 의미론 |

사용되지 않는 미지원 import는 바인딩된 상태로 남아 있을 수 있습니다. 지원되지 않는 작업에 도달하면 진단과 그때까지 수집한 관찰 결과를 남기고 중단합니다. DriverEntry의 성공만으로 이후 디스패치, 하드웨어 또는 프레임워크 경로가 지원됨을 입증할 수는 없습니다. 아래 API 표가 지원 하위 집합의 기준입니다.

## 실행 계약

이 프로필은 CPU0에서 결정적 협력 스케줄링으로 x64 WDM 수명 주기를 모델링합니다. 실행은 PE 진입점에서 시작하며, 컴파일러의 진입 래퍼가 있으면 그대로 유지합니다. 초기화하려면 DriverEntry가 `STATUS_SUCCESS`를 반환해야 합니다. 0이 아닌 성공 상태나 pending 상태는 지원하지 않는 초기화 계약으로 중단됩니다. 실패 상태는 완료된 초기화 결과로 보존됩니다. 모든 객체, 문자열, 스택, 함수 포인터와 할당은 게스트 메모리에 존재합니다. 모델은 설정된 서비스 이름(기본값 `NeverDDriver`)에 대한 `DRIVER_OBJECT`와 레지스트리 경로를 제공합니다. 어댑터는 Windows 페이지 테이블을 합성하지 않고 Unicorn의 가상 TLB 모드로 정규 상위 커널 주소를 포함한 게스트 가상 주소를 보존합니다. 초기 RFLAGS는 `0x202`이며, 소프트웨어 장치 프로필은 고정된 64바이트 캐시 라인을 사용합니다. 이는 이 실행 시나리오의 명시적인 속성입니다.
인라인 x64 CR8 읽기도 같은 `PASSIVE_LEVEL` / `DISPATCH_LEVEL`을 관찰합니다. CR8 쓰기와 다른 제어 레지스터 작업은 계속 지원하지 않습니다.

알 수 없는 import는 지연 트랩에 바인딩됩니다. 사용되지 않는 import는 실행을 막지 않지만, 해당 thunk를 실행하거나 모델링되지 않은 export 데이터 값을 읽으면 `unsupported_api`로 중단됩니다. 지원하지 않는 CPU 환경 효과도 명시적으로 중단됩니다. NeverD는 미구현 호출을 성공 값으로 대체하지 않습니다. 잘못된 이미지나 지원하지 않는 로딩 요구 사항은 실행 전에 실패합니다.

`DelayedWorkQueue` 작업 항목은 `PASSIVE_LEVEL`에서, 게스트 DPC 콜백은 정해진 네 인수와 함께 `DISPATCH_LEVEL`에서 실행됩니다. CPU0의 호출 반환 및 차단 대기 경계에서 결정적 협력 스케줄링을 수행합니다. 상대·절대·주기 타이머는 가상 시간을 사용하며 실행할 프레임이 없으면 다음 타이머 또는 대기 기한으로 진행합니다. 알림형과 동기화형 이벤트/타이머는 서로 다른 신호 소비 동작을 유지합니다. 콜백마다 별도 게스트 스택을 사용하며 여러 차단 프레임의 지역 변수와 전체 CPU 컨텍스트를 보존하고 게스트 메모리는 공유합니다. Win64 콜백의 처음 네 인수는 레지스터에, 나머지는 스택에 전달합니다. 요청은 순차 처리하며 IRP를 보류로 표시한 디스패치는 `STATUS_PENDING`을 반환하고 다음 요청 전에 완료해야 합니다. 보류 요청이나 무한 대기에 실행 가능한 생성 주체가 없으면 정체된 `model_error`로 중단합니다. 명령어·메모리·관찰·실시간 예산은 공유합니다.

이는 제한된 스케줄링 모델이며 완전한 Windows 비동기 지원은 아닙니다. 경고 가능/사용자 모드 대기, 시스템 스레드, APC, 요청 취소, 스핀락, 동시 IRP, 일반 IRQL 전환, `METHOD_NEITHER`, KMDF/UMDF, 전체 PnP/전원, 하드웨어, DMA와 인터럽트는 지원하지 않습니다. 초기화 전용 호출도 명시적으로 대기열에 넣은 콜백을 실행하지만 요청이나 언로드를 암묵적으로 만들지 않습니다.

콜백 시작 전에 작업 항목이 대기열에서 제거되므로 콜백은 자신의 작업 항목을 해제할 수 있습니다. 대기열 항목 해제, 중복 큐 삽입, 만료 객체 및 실행 가능한 게스트 메모리 밖의 콜백 주소는 명시적으로 실패합니다. 장치 참조는 콜백 반환까지 유지합니다. 언로드에는 모든 작업 항목 해제와 큐 작업 완료가 필요합니다. CPU 컨텍스트는 일반, SIMD, FPU 및 제어 상태를 저장하고 복원합니다. 게스트 메모리는 공유되며 장애가 난 CPU는 저장된 컨텍스트로 재개할 수 없습니다.
파일 객체 또는 대기/실행 중인 작업 항목 참조가 남아 있으면 삭제를 연기합니다. 객체 영역이 소진되면 작업 항목 할당은 NULL을 반환합니다.

시나리오에서 유효한 재배치 주소를 지정하지 않으면 이미지는 선호 베이스를 사용하며, native 서브시스템의 PE32+ x64 실행 파일이어야 합니다. import 제공자는 `ntoskrnl.exe` 또는 `ntkrnlmp.exe`일 수 있습니다. 실행 로더는 검증된 x64 `DIR64` 베이스 재배치와 제한적인 security-cookie 로드 구성을 지원합니다. security cookie는 진입 래퍼 실행 전에 결정적인 게스트 값으로 초기화됩니다. CFG와 기타 모델링되지 않은 로드 구성 필드, TLS, 지연/바인딩 import, ordinal import, managed 이미지는 거부됩니다. 이미지에는 엄격한 범위 및 정렬 검사도 적용됩니다.

초기 API 모델에는 의도적으로 유한한 계약이 있습니다.

| API | 모델링된 동작 및 제한 |
|-----|----------------------|
| `RtlInitUnicodeString` | 길이가 제한된 NUL 종료 소스로 게스트 `UNICODE_STRING`을 구성함 |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | 길이가 지정된 UTF-16 복사와 대소문자를 구분하는 비교. 대소문자 무시 비교에는 Windows 대소문자 테이블이 필요하므로 중단함 |
| `ExAllocatePool2` | Paged/nonpaged NX 할당이며 기본값은 0으로 초기화. uninitialized 및 cache-aligned 플래그를 모델링함. 잘못된 필수 플래그는 NULL을 반환하고, quota/executable 풀과 할당 예외 발생은 중단함 |
| `MmGetSystemRoutineAddress` | 공유 export 목록을 통해 길이가 지정된 게스트 이름을 해석함 |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | 요청 소유 MDL의 캐시된 KernelMode 매핑과 권한. 비페이지 풀 MDL은 안전 도우미로 원래 풀 매핑을 재사용 |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | 독립 MDL의 전체 범위는 하나의 유효한 비페이지 풀 할당 내부여야 함. MDL과 버퍼 수명은 독립적이며 IRP 연결, 체인, 할당량은 미지원 |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | 명시적인 세션 레지스트리, 핸들별 권한과 수명, 쿼리 버퍼 크기와 변경. 호스트 레지스트리에 접근하지 않음 |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | 풀 유형 `0`, `1`, `512`의 데이터 할당. 크기/태그는 양수이며, 태그가 있는 해제는 일치해야 하고 주소를 재사용하지 않음 |
| `IoCreateDevice`, `IoDeleteDevice` | 장치 유형 `0x22`, 특성 `0` 또는 `0x100`, 제한된 확장, ASCII `\Device\Name` 이름 |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | 하나의 세션 네임스페이스에서 ASCII `\DosDevices\Name` 또는 `\??\Name`을 사용하며 `\Device\Name`을 가리킴 |
| `DbgPrint`, `DbgPrintEx` | 검증된 Win64 가변 인수 포맷팅, 최대 출력 512바이트. 모든 디버거 필터가 활성화됨 |
| `IoGetCurrentIrpStackLocation` | 활성 모델 IRP의 스택 위치를 반환함. 일반적인 컴파일된 WDM 매크로도 동일한 게스트 필드를 읽음 |
| `KeGetCurrentIrql` | 현재 실행 IRQL은 디스패치와 작업 항목에서는 `PASSIVE_LEVEL`, DPC에서는 `DISPATCH_LEVEL`입니다 |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | 장치 소유의 불투명 작업 항목. `DelayedWorkQueue`만 지원하며 `PASSIVE_LEVEL`에서 장치와 컨텍스트를 콜백에 전달. 대기열에 있는 항목은 해제 불가 |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | 불투명 DPC, 게스트 콜백 인수 네 개, `DISPATCH_LEVEL`, 중복/제거 동작과 중요도; 대상 CPU0만 지원 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | 알림/동기화 타이머, 상대/절대 100 ns 기한, 밀리초 주기, 재설정/취소와 가상 시간 신호 조회 |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | 알림/동기화 이벤트의 서로 다른 신호 소비; `KeSetEvent`는 Increment=0, Wait=FALSE만 허용 |
| `KeWaitForSingleObject` | 초기화된 이벤트 또는 타이머 하나, 비경고 `KernelMode`, 사유 `Executive`; 0 폴링, 유한 상대/절대 또는 무한 대기; 0이 아닌/무한 대기는 IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | IRQL <= APC_LEVEL에서 비경고 `KernelMode` 상대/절대 지연; 가상 시간 진행 후 저장된 게스트 프레임 재개 |
| `IoMarkIrpPending` | 현재 유효한 IRP를 보류로 표시. WDM 매크로의 스택 제어 필드 쓰기도 지원. 디스패치는 `STATUS_PENDING`을 반환해야 함 |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT`로 활성 동기 또는 보류 모델 IRP를 완료함. 완료된 IRP나 버퍼에는 다시 접근할 수 없음 |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | 호출당 최대 1 MiB의 제한된 게스트 버퍼 작업. 비중첩 복사 API는 겹치는 범위를 거부함 |

API IRQL 상한은 `KernelAPIIRQL.def`에 정의되며 인수별 제한은 담당 모델이 검사합니다. DPC는 레지스트리 API나 페이징 풀 할당·해제·접근을 사용할 수 없습니다. Unicode `DbgPrint` 변환은 `PASSIVE_LEVEL`이 필요하며 지원되는 ANSI 출력과 비페이징 작업은 `DISPATCH_LEVEL`에서 사용할 수 있습니다. 콜백 스택에는 경계가 있어 이탈한 스택 포인터가 다른 차단 작업자의 스택을 침범할 수 없습니다. 장치 확장의 활성 타이머는 조기 장치 회수를 막습니다. 일반 IRQL 전환을 제공하는 기능은 아닙니다.

타이머 만료는 DPC가 타이머를 재설정하기 전에 등록된 대기를 충족합니다. 대기열의 DPC는 깨어난 `PASSIVE_LEVEL` 프레임이 재개되기 전에 실행합니다. 요청 저장소에 대기 중인 DPC가 있으면 IRP 완료 처리는 완료와 버퍼 무효화 전에 해제를 거부합니다.

`DbgPrint` 포맷팅은 정수 `d/i/u/o/x/X`, 포인터 `p`, 텍스트 `s/c`, `%%`, 길이가 지정된 Unicode `wZ/lZ`, wide 형식 `ls/ws`, 플래그, `*`를 포함한 너비/정밀도, Windows 정수 길이 수정자를 지원합니다. 가변 인수는 최대 32개, 형식 문자열은 최대 1024바이트를 읽습니다. 너비와 정밀도는 512로 제한됩니다. 부동소수점, `%n`, 알 수 없는 조합 및 비ASCII 텍스트 변환은 명시적으로 중단합니다. 모델은 Windows 코드 페이지를 추측하거나 게스트 데이터에 호스트 printf를 호출하지 않습니다.

원래 RegistryPath 레코드와 버퍼의 수명은 DriverEntry가 반환할 때 끝납니다. 이후에도 문자열이 필요한 드라이버는 초기화 중에 복사해야 합니다.

객체/풀 arena는 1 MiB입니다. 초기화되지 않은 풀 바이트는 결정적인 `0xCD` 값, 해제된 풀 바이트는 `0xDD` 값을 사용합니다. 이는 하나의 구체적인 실행 시나리오입니다. CPU 접근과 모델링된 버퍼 API는 해제된 풀 할당, 삭제된 장치, 할당되지 않은 arena 바이트, 불투명 객체 필드에 대한 접근 및 읽기 전용 객체 필드에 대한 쓰기를 거부합니다. 이 검사는 모델의 객체 수명을 다루며, 일반적인 드라이버 메모리 안전성 분석은 아닙니다. 쓰지 않은 디스패치 테이블 슬롯은 “미등록”을 뜻하는 0으로 보고됩니다. 미등록 major function에 대한 시나리오 요청은 모델의 기본 핸들러를 통해 `STATUS_INVALID_DEVICE_REQUEST`로 완료되며, 실패가 디스패치와 I/O 상태 모두에 표시됩니다. 모델은 이 핸들러에 대한 게스트 함수 주소를 만들어 내지 않으며, 쓰지 않은 슬롯을 게스트가 읽는 동작은 여전히 지원하지 않습니다. null 콜백을 명시적으로 등록하면 오류입니다.

## 요청 시나리오

`--scenario`로 JSON 파일을 전달하여 요청과 선택적 언로드를 지정합니다.

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --scenario scenario.json > driver-report.json
```

`\Device\NeverDIO`를 만들고 buffered IOCTL `0x222000`을 받는 드라이버에 대한 `scenario.json` 예시는 다음과 같습니다.

```json
{
  "requests": [
    {"kind": "create", "device": "\\Device\\NeverDIO"},
    {"kind": "ioctl", "code": "0x222000", "input": "00112233", "output_size": 4},
    {"kind": "cleanup"},
    {"kind": "close"}
  ],
  "unload": true
}
```

장치 이름과 IOCTL 코드는 드라이버와 일치해야 합니다. create에서 `device`를 생략하면 유일한 활성 장치를 선택하며, 선택이 모호하면 실패합니다. 이후 요청은 명시적으로 일치하는 이름을 지정하지 않는 한 해당 파일의 장치를 사용합니다. 선택적인 `file`은 부호 없는 32비트 시나리오 식별자이며 기본값은 0입니다. 각 식별자에는 자체 FILE_OBJECT와 FsContext가 있으며 create, 전송, cleanup, close 순서를 따라야 합니다. 독립된 파일의 요청은 서로 교차할 수 있습니다. 독점 장치는 두 번째 열기를 거부합니다. 이 식별자는 복제된 핸들이 아니라 파일 객체를 나타냅니다. Buffered 및 두 direct IOCTL 방식을 지원합니다. 디스패치는 동기적으로 완료하거나 위의 콜백 보류 계약을 따라야 합니다. 잘못된 출력 길이와 완료된 IRP 접근은 명시적으로 실패합니다. 언로드를 요청했다면 활성 장치, 심볼릭 링크, 풀 할당 또는 파일 객체가 남아서는 안 됩니다.

선택적인 루트 필드 `"load_address": "0x190000000"`는 베이스 재배치를 요청합니다. 생략하거나 `"0x0"`을 지정하면 선호 주소를 사용합니다. 이미지는 재배치 요구 사항을 충족해야 합니다. 기존 초기화 명령이나 C API에는 암묵적인 시나리오가 없습니다.

루트에서는 `load_address`, `requests`, `unload`, `kernel_exports`, `registry`만 허용됩니다. 모든 요청은 `kind`, 선택적인 `device`와 `file`을 받습니다. IOCTL에는 `code`가 필수이며 `input`, `output_size`, `direct_input`을 받을 수 있습니다. `read`는 `output_size`와 `byte_offset`, `write`는 `input`과 `byte_offset`을 받습니다. 오프셋 기본값은 0이며 정수 또는 16진 문자열을 받고, 음수가 아닌 부호 있는 64비트 값 범위에 들어야 합니다. 수명 주기 요청은 전송 필드를 거부합니다. 알 수 없거나 중복된 필드는 거부합니다. `code`는 부호 없는 32비트 JSON 정수 또는 `0x` 16진 문자열을 받습니다. `input`은 접두사나 공백이 없는 짝수 길이의 16진 바이트 문자열이며, 생략하면 빈 입력입니다. `output_size`는 부호 없는 JSON 정수이며 생략하면 0입니다. 소수와 부동소수점 표기는 거부합니다.

Direct IOCTL에서 `input`은 첫 번째 시스템 버퍼를 초기화하고, `direct_input`은 MDL이 설명하는 별도의 두 번째 버퍼를 초기화하며 `output_size`까지 0으로 채웁니다. `METHOD_IN_DIRECT`는 읽기 접근을 요구하지만 읽기 전용 시스템 매핑을 뜻하지는 않습니다. 두 방식 모두 읽기/쓰기가 가능한 시나리오 버퍼를 사용합니다. `MdlMappingNoWrite`는 매핑의 쓰기 권한을, `MdlMappingNoExecute`는 실행 권한을 제거합니다. 매핑 해제는 시스템 VA를 무효화하며, 다시 매핑해도 같은 잠긴 데이터가 유지됩니다. 완료되면 MDL과 매핑의 수명이 끝납니다. WDM 매크로가 사용하는 공개 MDL 필드는 모델링하지만, process/PFN 필드, 직접 만든 MDL, 사용자 매핑 및 원시 UserBuffer를 통한 직접 접근은 거부합니다. 길이가 0인 direct 버퍼의 MDL은 null입니다.

`IoAllocateMdl`은 비어 있지 않고 주소가 넘치지 않는 최대 1 MiB 버퍼의 독립 메타데이터를 할당하며, 버퍼를 프로브하거나 잠그지 않습니다. `Irp`는 NULL, `SecondaryBuffer`와 `ChargeQuota`는 FALSE여야 합니다. 아레나가 고갈되면 NULL을 반환합니다. `MmBuildMdlForNonPagedPool`은 설명하는 전체 범위가 하나의 유효한 비페이지 풀 할당에 속해야 합니다. 안전 도우미와 일반 WDM 매크로는 원래 주소를 재사용하여 별칭과 기존 권한을 유지하며, 새로운 쓰기/실행 금지 플래그도 기존 권한을 바꾸지 않습니다. 추가 시스템 매핑과 매핑 해제는 거부합니다. `IoFreeMdl`은 MDL만 만료시키며 풀 버퍼의 수명은 독립적입니다. 해제한 저장소를 다시 사용하지 않으면 어느 해제 순서든 지원합니다. 모델의 MDL 필드는 모두 읽기 전용이며 프로세스/PFN 접근, MDL 체인과 수동 필드 변경은 지원하지 않습니다. 언로드 시 드라이버 소유 MDL을 모두 해제해야 합니다.

IOCTL의 `output_size`가 0이 아니면 입력 버퍼가 더 커도 `Information`은 해당 크기를 넘을 수 없습니다. 출력 버퍼가 없는 IOCTL은 드라이버 정의 결과를 반환할 수 있으며 출력 바이트를 복사하지 않습니다. `information_hex`는 원래 64비트 값을 정확히 보존합니다.

READ/WRITE에서는 `DO_BUFFERED_IO` 또는 `DO_DIRECT_IO`가 전송 방식을 선택합니다. Neither 방식이나 충돌하는 플래그는 중단됩니다. Information은 전송 길이에 대해 검사하며, 쓰기는 개수를 반환하고 읽기는 바이트를 반환합니다.

`kernel_exports`는 루틴 이름을 명시적인 가용성 불리언에 매핑합니다. 예를 들면 `"kernel_exports": {"OptionalRoutine": false}`입니다. 모델링된 export와 정적 import는 `MmGetSystemRoutineAddress`와 공유하는 안정적인 주소를 받습니다. 명시적으로 없는 export는 NULL로 해석되며 정적 import를 충족할 수 없습니다. 존재한다고 선언되었으나 API 모델이 없는 export는 지연 트랩으로 해석됩니다. 알 수 없는 동적 이름은 가용성이 지정되지 않았다는 진단과 함께 중단됩니다. 구현이 없다는 이유로 부재를 추론하지 않습니다. 이름은 길이가 제한된 출력 가능한 ASCII이며 해석 시 대소문자를 구분합니다. 이 목록은 구체적인 시나리오 속성이며 모든 Windows 릴리스와 일치한다는 의미는 아닙니다.
`IoMarkIrpPending`, `IoGetCurrentIrpStackLocation`과 `MmGetSystemAddressForMdlSafe`는 모델링된 WDM 헤더 도우미 함수이며, 모델링되었다는 사실만으로 기본 export로 선언되지는 않으므로 export 가용성에는 정적 import 또는 명시적인 `kernel_exports` 선언이 필요합니다.

시나리오 텍스트는 2 MiB, 요청은 최대 64개, 각 입력 또는 출력 버퍼는 최대 65536바이트, `direct_input` 내용을 포함한 요청 바이트 총합은 최대 512 KiB입니다. 명령어, 관찰, 게스트 메모리 및 시간 예산은 전체 시나리오에 적용됩니다. 1 MiB arena에는 객체와 메타데이터도 저장되므로 최대 시나리오 버퍼 용량을 사용하기 전에 모델 메모리가 고갈될 수 있습니다.

## 레지스트리 시나리오

선택적인 `registry` 배열은 세션 내 구체적인 레지스트리 트리를 정의합니다. 각 키는 필수 `path`와 선택적인 `values` 배열을 갖고, 각 값은 `name`, 부호 없는 정수 `type`, 16진수 `data`를 갖습니다. 빈 값 이름은 기본값입니다. DWORD 예시는 `{"name":"Mode","type":4,"data":"01000000"}`입니다. 값의 바이트를 그대로 보존하며 문자열 종결자를 보정하거나 환경 변수를 확장하지 않습니다.

경로는 `\Registry\Machine` 또는 `\Registry\User` 아래의 절대 ASCII 경로여야 합니다. 상위 키는 자동 생성합니다. 키와 값은 ASCII 규칙으로 대소문자를 구분하지 않으며 비 ASCII 이름과 중복을 거부합니다. 구성을 생략하면 가용성이 미지정되어 레지스트리 호출이 중단됩니다. `"registry": []`는 빈 네임스페이스를 명시합니다. 드라이버에서 키, 값, 호스트 레지스트리 데이터나 서비스 구성을 추론하지 않습니다.

`ZwOpenKey`와 `ZwCreateKey`는 독립적인 불투명 핸들을 반환하며 핸들별로 쿼리, 설정, 하위 키 생성과 삭제 권한을 검사합니다. 구성 트리는 일반 `KEY_READ`와 `KEY_WRITE`를 포함한 지원되는 `KEY_ALL_ACCESS` 비트를 허용합니다. 명시적으로 접근 가능한 테스트 트리이며 Windows ACL이나 권한 평가를 구현하지 않습니다. 일반 권한, `MAXIMUM_ALLOWED`, 다른 레지스트리 뷰, 사용자 보안 설명자, 클래스와 심볼릭 링크는 지원하지 않습니다. 상대 생성에는 `KEY_CREATE_SUB_KEY`를 가진 직접 부모 핸들이 필요합니다. 입력 키는 비휘발성이며 새 키는 휘발성일 수 있지만 휘발성 키 아래의 비휘발성 자식은 거부합니다. 재부팅이나 디스크 영속성은 모델링하지 않습니다.

`ZwQueryValueKey`는 Basic, Full, Partial과 정의된 Align64 정보 클래스를 구현하여 정확한 길이, 데이터 정렬, 부분 출력, 서로 다른 `STATUS_BUFFER_TOO_SMALL` / `STATUS_BUFFER_OVERFLOW` 결과를 제공합니다. `ZwSetValueKey`와 `ZwDeleteValueKey`는 현재 세션의 트리만 변경합니다. `ZwDeleteKey`는 자식이 남은 키를 거부하며 삭제된 키의 핸들은 닫기 전까지 `STATUS_KEY_DELETED`를 반환합니다. `ZwClose`는 키와 독립적으로 핸들을 해제합니다. 열린 레지스트리 핸들이 남으면 언로드가 실패합니다.

상위 키를 포함한 키 256개, 총 값 1024개, 값당 65536바이트, 전체 값 데이터 512 KiB, 키 경로 1024 ASCII 바이트, 값 이름 256바이트, 동시 열린 핸들 256개로 제한합니다. 생성과 변경에도 시나리오 사전 검사와 같은 제한을 적용합니다. 보고서의 `configuration.registry`는 원래 입력을 보존하고 `registry`는 중단 전 변경을 포함한 최종 키 경로와 값을 나열합니다. 미지정 상태는 null로 보고합니다. 값 스냅샷에는 휘발성 속성과 핸들 식별자가 포함되지 않습니다.

## Microsoft 예제 검증

선택적으로 실행하는 [검증 스크립트](../../scripts/validate_windows_driver_sample.py)는 [검증 매니페스트](../../unittests/emulation/fixtures/sioctl-validation.json)에 고정된 리비전의 Microsoft SIOCTL 소스를 다운로드하고 SHA-256 해시를 검증한 뒤, 수정하지 않은 소스를 MinGW-w64 DDK 헤더로 컴파일합니다. 선택한 출력 디렉터리에 업스트림 라이선스/출처, 빌드 명령, 시나리오 및 보고서를 보존합니다. 네트워크 접근, Clang, `lld-link`, `nm`, MinGW-w64 DDK 헤더가 필요합니다.

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

MinGW-w64 include 디렉터리가 기본 위치가 아니면 `--headers`를 사용합니다. 스크립트는 컴파일된 객체의 의존성으로 MS COFF import 라이브러리를 생성합니다. 검증은 buffered, in-direct, out-direct 시나리오를 별도로 실행하며 각각 DriverEntry, create, IOCTL, cleanup, close, unload를 거칩니다. `--debug`를 추가하고 별도 출력 디렉터리를 선택하면 `DBG=1`로 컴파일하여 게스트 로그 메시지를 검증합니다. 업스트림 예제는 cleanup 핸들러를 등록하지 않으므로, 모델의 기본 핸들러는 cleanup을 `STATUS_INVALID_DEVICE_REQUEST`(`0xC0000010`)로 완료합니다. 드라이버는 계속 close와 unload를 수행하며, 성공한 IOCTL은 예상 바이트를 반환합니다. 이 전체 시나리오에서 CLI의 예상 종료 코드는 **2**이고 `scenario_success`는 false입니다. 스크립트 자체는 눈에 보이는 cleanup 실패까지 포함해 모든 결과가 예상과 일치할 때만 성공합니다. 이 결과를 숨기려고 예제를 수정하지 않습니다.

## Zero 샘플 수용 검사

추가 [Zero 검증 스크립트](../../scripts/validate_zero_driver_sample.py)는 [매니페스트](../../unittests/emulation/fixtures/zero-validation.json)의 리비전과 해시로 Pavel Yosifovich의 공개 Zero WDM 샘플을 수정 없이 빌드합니다. 같은 도구 체인으로 `python3 scripts/validate_zero_driver_sample.py`를 실행합니다. 기본적으로 `build-release/driver-validation/zero`에 소스, MIT 라이선스, 명령, 시나리오와 보고서를 보존합니다. 요청 9개가 페이지 경계를 넘는 direct read, write 바이트 수, 게스트 원자적 통계와 buffered 통계 IOCTL을 실행합니다. 0길이 read 실패와 누락된 CLEANUP 처리기는 그대로 표시됩니다. 예상 CLI 종료 코드는 2이고 close와 unload는 성공합니다. 모든 결과와 출력 바이트가 정확히 일치할 때만 검증이 성공합니다.

## 보고서 및 SDK

JSON 보고서는 `stop_reason`, null이 가능한 `nt_status`와 `nt_success`, 중단 PC, 명령어 수를 구분합니다. 장치 객체와 드라이버 콜백 주소를 포함하여 중단 전에 수집한 API 호출 및 관찰 가능한 상태를 보존합니다. JSON 소비자가 64비트 정밀도를 잃지 않도록 게스트 주소는 16진 문자열로 표현합니다. `configuration` 객체는 실행 한도, 서비스 이름과 `kernel_exports` 재정의를 기록합니다. 프로필은 `wdm-x64-scheduled-v3`입니다. `nt_status`는 계속 DriverEntry 결과를 나타내고, `scenario_success`는 초기화와 완료된 요청을 함께 나타냅니다. `phase`, `requests`, `unload_completed`는 요청한 수명 주기의 어느 부분이 실행되었는지 식별합니다. 각 API 호출과 CPU 쓰기에도 단계(`driver_entry`, `request:N`, `callback:N`, `unload`)가 기록됩니다. 각 요청은 디스패치 및 I/O 상태, 완료 여부, 정보 길이와 반환된 `output_hex` 바이트를 보고합니다. `preferred_image_base`는 원래 PE 베이스를 나타냅니다. `security_cookie`는 초기화된 cookie의 게스트 주소이며, 필요하지 않았다면 `"0x0"`입니다. 요청 필드는 `kind`, `device`, `file`, `byte_offset`, `code`, `irp`, `completed`, `dispatch_status`, `io_status`, `information`、`information_hex`, `output_hex`입니다. `configuration.registry`는 원래 레지스트리 구성을 보존합니다. `information_hex`는 원래 64비트 `IoStatus.Information`을 16진수 문자열로 정확히 보존합니다. 기존 숫자 필드 `information`도 유지합니다.

작업 항목 관찰에는 `callback:N` 단계가 기록됩니다. 보류 요청의 `dispatch_status`는 `STATUS_PENDING`을 유지하며 최종 완료 상태는 별도의 `io_status`에 기록되어 `scenario_success` 판정에 사용됩니다.

null이 가능한 `fault` 객체는 최초의 백엔드 오류를 보존합니다. `kind`, `pc`와 null이 가능한 `address`, `size`, `access`, `interrupt`는 매핑되지 않았거나 보호된 메모리, 잘못된 범위, 잘못된 명령어와 CPU 예외를 구분합니다. 주소는 16진 문자열, 크기와 인터럽트 벡터는 정수를 사용합니다. 관찰을 위한 읽기는 원래 오류를 대체할 수 없습니다. 오류가 발생한 백엔드는 재개할 수 없으며, 이 레코드가 게스트 SEH 처리를 뜻하지는 않습니다.

`instructions`는 허용된 게스트 명령어 실행 시도 수입니다. 실행 정책이 거부한 명령어는 세지 않지만, 허용된 후 CPU에서 오류가 난 명령어는 셉니다. 합성 API 디스패치와 반환 센티널은 이 카운터를 증가시키지 않습니다.

각 `writes` 항목의 `semantics: "attempted_guest_write"`는 스택 밖의 CPU 쓰기 시도를 기록한다는 뜻입니다. 이후 오류가 발생하거나 예산으로 중단되는 시도도 포함됩니다. 쓰기 완료를 보장하지 않으며 API 모델이 수행한 쓰기는 포함하지 않습니다. 장치와 드라이버 객체 스냅샷은 실행이 중단될 때 관찰한 상태를 나타냅니다.

`neverd/sdk/NeverDCAPIEmulation.h`(또는 C API 통합 헤더)를 include하고 세션을 만든 뒤 `neverd_emulate_driver_json(session, path, options)`를 호출합니다. 비어 있지 않은 경로를 명시하면 일반 분석 API로 먼저 로드하지 않고 엄격한 실행 사전 검사로 바로 진입합니다. CLI는 이 경로를 사용합니다. 옵션에 `NULL`을 전달하면 기본값을 선택합니다. `neverd_driver_options_v1` 옵션을 명시할 때는 정확한 `struct_size`와 양수인 명령어, 메모리, 이벤트, 시간 예산이 필요합니다. 결과는 `neverd_free_string`으로 해제합니다.

대신 경로에 `NULL`을 전달하려면 로드된 세션이 필요하며, 해당 파일을 IR 분석 및 함수 제한 로딩과 독립적으로 다시 파싱합니다. 두 경로 모두 세션 이미지를 보존합니다. 호출이 끝날 때까지 입력 파일을 접근 가능한 상태로 유지하고 변경하지 마세요. 요청/설정 실패는 `NULL`을 반환하고 `neverd_last_error`를 설정합니다. 실행 중단은 JSON을 반환합니다. 비활성화된 빌드에도 API가 존재하며 기능을 활성화하는 방법을 알려 줍니다.

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)`는 동일한 v1 옵션 및 소유권 규칙을 사용하면서 엄격한 시나리오 입력을 추가합니다. NULL이 아닌 NUL 종료 JSON 문자열이 필요합니다. 기존 `neverd_emulate_driver_json` ABI는 변경되지 않으며 초기화만 수행합니다. C++ 파서 `driverOptionsFromScenarioJSON`은 `emulateDriver` 호출자에게 동일한 시나리오 검증을 제공합니다.

내부 C++ 진입점은 `include/neverd/emulation/DriverSession.h`의 `neverd::emulation::emulateDriver`입니다. 형식 파싱은 기존 로더, Windows 객체/API 동작은 `lib/emulation/windows`, CPU 상태와 실행은 Unicorn 어댑터가 담당합니다. 어댑터와 모델은 같은 게스트 메모리 인터페이스를 사용합니다. Windows API 동작을 Unicorn fork에 넣지 않습니다.
