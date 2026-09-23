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

보고서는 항상 stdout에 JSON으로 출력되며, 요청 및 설정 진단은 stderr로 출력됩니다. 기본 한도는 게스트 명령어 100000개, 게스트 메모리 64 MiB, 기록 이벤트 10000개, 실행 시간 5000밀리초입니다. 명령어 한도는 양수여야 합니다. 실행 예산이 소진되면 수집한 관찰 결과를 보존하고 중단합니다. 다만 할당 API는 공간 부족 시 규정된 결과를 사용하며, MMIO 매핑 공간 부족은 NULL을 반환합니다.

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
| `METHOD_BUFFERED` IOCTL | 순차 buffered/direct I/O와 작업 항목 또는 DPC 완료 | 아래 API 부분집합만 지원하며 동시 공개 시나리오 제출와 일반적인 WDM 요청 취소는 미지원 |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | 요청 소유 MDL, 시스템 매핑, 공유 물리 페이지 식별자와 SG DMA | 사용자 매핑과 기타 DMA 인터페이스 |
| 드라이버 할당 MDL | 비페이지 풀 또는 하나의 사용자 할당을 설명하며 물리 페이지를 공유 | IRP 연결, MDL 체인 및 임의 프로세스는 미지원 |
| READ/WRITE | 순차 buffered/direct/neither I/O와 작업 항목 또는 DPC 완료 | 아래 API 범위만 지원; 동시 요청, 일반적인 WDM 요청 취소 및 암묵적 파일 위치는 미지원 |
| WDM `METHOD_NEITHER` | 별도 사용자 버퍼, 접근 검사, MDL 잠금, 합성 요청자 ID, 디스패치 후 VA 철회 또는 프로세스 종료, 제한된 취소 | 일반적인 프로세스 연결 및 임의 사용자 매핑·재매핑은 미지원 |
| KMDF 1.33 비 PnP 드라이버 | 바인딩, 객체/컨텍스트, 이름 있는 제어 장치, 순차 기본 큐 및 콜백을 실제 실행하는 버퍼/직접 요청 | PnP 장치, 일반 큐 스케줄링, 클래스 확장 및 UMDF는 지원하지 않음 |
| PnP 버스/기능/필터 드라이버 | 명시적 리소스 없는 PDO 또는 고정 레지스터 뱅크 PDO, 게스트 AddDevice, 여덟 가지 일반 PnP 수명 주기 부 기능 | 기타 PnP 작업, 일반 전원 관리, 기타 하드웨어/리소스 및 KMDF PnP |
| 저장 장치, 네트워크, 디스플레이, 파일 시스템 및 미니필터 드라이버 | 서브시스템 계약을 지원하지 않음 | 포트/클래스/미니포트 프레임워크, NDIS/WFP, 그래픽 또는 파일 시스템 서비스 |
| 작업 항목, 타이머, DPC, 이벤트와 대기 | 현재 실행 IRQL은 디스패치와 작업 항목에서는 `PASSIVE_LEVEL`, DPC에서는 `DISPATCH_LEVEL`입니다 | 아래 API 부분집합만 지원하며 동시 공개 시나리오 제출와 일반적인 WDM 요청 취소는 미지원 |
| 프로세스/스레드 콜백, 핸들, 레지스트리/파일 작업 또는 커널 모듈 탐색을 사용하는 드라이버 | 구성한 레지스트리는 지원하며 그 외 동작은 아래 API 범위로 제한 | 객체 관리자, 시스템 상태와 콜백/이벤트 생성 주체 |
| 하드웨어, DMA, PCI, 인터럽트 또는 가상화 드라이버 | 명시적 메모리 레지스터 뱅크, MMIO, 독점 latched 인터럽트 및 제한된 일관성 있는 공통 버퍼／SG／채널 DMA 지원 | 기타 장치 모델, 임의 물리 RAM, PCI, 포트, 기타 인터럽트 모드, 기타 DMA 인터페이스 및 특권 CPU 상태 |
| x86 또는 ARM64 Windows 드라이버 | 거부됨 | 아키텍처별 로딩, ABI와 실행 모델 |
| x64 CFG | 검증된 대상 테이블과 check/dispatch 호출, 비활성 계측의 게스트 대체 함수 유지 | XFG, 내보내기 억제, 미지원 로드 구성 및 TLS는 계속 거부 |

사용되지 않는 미지원 import는 바인딩된 상태로 남아 있을 수 있습니다. 지원되지 않는 작업에 도달하면 진단과 그때까지 수집한 관찰 결과를 남기고 중단합니다. DriverEntry의 성공만으로 이후 디스패치, 하드웨어 또는 프레임워크 경로가 지원됨을 입증할 수는 없습니다. 아래 API 표가 지원 하위 집합의 기준입니다.

## 실행 계약

이 프로필은 CPU0에서 결정적 협력 스케줄링으로 x64 WDM 수명 주기를 모델링합니다. 실행은 PE 진입점에서 시작하며, 컴파일러의 진입 래퍼가 있으면 그대로 유지합니다. 초기화하려면 DriverEntry가 `STATUS_SUCCESS`를 반환해야 합니다. 0이 아닌 성공 상태나 pending 상태는 지원하지 않는 초기화 계약으로 중단됩니다. 실패 상태는 완료된 초기화 결과로 보존됩니다. 모든 객체, 문자열, 스택, 함수 포인터와 할당은 게스트 메모리에 존재합니다. 모델은 설정된 서비스 이름(기본값 `NeverDDriver`)에 대한 `DRIVER_OBJECT`와 레지스트리 경로를 제공합니다. 어댑터는 Windows 페이지 테이블을 합성하지 않고 Unicorn의 가상 TLB 모드로 정규 상위 커널 주소를 포함한 게스트 가상 주소를 보존합니다. 초기 RFLAGS는 `0x202`이며, 소프트웨어 장치 프로필은 고정된 64바이트 캐시 라인을 사용합니다. 이는 이 실행 시나리오의 명시적인 속성입니다.
인라인 x64 CR8 읽기도 같은 `PASSIVE_LEVEL` / `DISPATCH_LEVEL`을 관찰합니다. CR8 쓰기와 다른 제어 레지스터 작업은 계속 지원하지 않습니다.

알 수 없는 import는 지연 트랩에 바인딩됩니다. 사용되지 않는 import는 실행을 막지 않지만, 해당 thunk를 실행하거나 모델링되지 않은 export 데이터 값을 읽으면 `unsupported_api`로 중단됩니다. 지원하지 않는 CPU 환경 효과도 명시적으로 중단됩니다. NeverD는 미구현 호출을 성공 값으로 대체하지 않습니다. 잘못된 이미지나 지원하지 않는 로딩 요구 사항은 실행 전에 실패합니다.

`DelayedWorkQueue` 작업 항목은 `PASSIVE_LEVEL`에서, 게스트 DPC 콜백은 정해진 네 인수와 함께 `DISPATCH_LEVEL`에서 실행됩니다. CPU0의 호출 반환 및 차단 대기 경계에서 결정적 협력 스케줄링을 수행합니다. 상대·절대·주기 타이머는 가상 시간을 사용하며 실행할 프레임이 없으면 다음 타이머, 대기 또는 취소 기한으로 진행합니다. 알림형과 동기화형 이벤트/타이머는 서로 다른 신호 소비 동작을 유지합니다. 콜백마다 별도 게스트 스택을 사용하며 여러 차단 프레임의 지역 변수와 전체 CPU 컨텍스트를 보존하고 게스트 메모리는 공유합니다. Win64 콜백의 처음 네 인수는 레지스터에, 나머지는 스택에 전달합니다. 요청은 순차 처리하며 IRP를 보류로 표시한 디스패치는 `STATUS_PENDING`을 반환하고 다음 요청 전에 완료해야 합니다. 보류 요청이나 무한 대기에 실행 가능한 생성 주체가 없으면 정체된 `model_error`로 중단합니다. 명령어·메모리·관찰·실시간 예산은 공유합니다.

이는 제한된 스케줄링 모델이며 완전한 Windows 비동기 지원은 아닙니다. 경고 가능/사용자 모드 대기, APC, 일반적인 WDM 요청 취소, 동시 공개 시나리오 제출, KMDF `METHOD_NEITHER` 사용자 버퍼 API, UMDF, KMDF PnP 장치 및 일반 큐 스케줄링, 전체 PnP/전원, 일반 하드웨어, 기타 DMA 인터페이스와 기타 인터럽트 모드는 지원하지 않습니다. 초기화 전용 호출도 명시적으로 대기열에 넣은 콜백을 실행하지만 요청이나 언로드를 암묵적으로 만들지 않습니다.

콜백 시작 전에 작업 항목이 대기열에서 제거되므로 콜백은 자신의 작업 항목을 해제할 수 있습니다. 대기열 항목 해제, 중복 큐 삽입, 만료 객체 및 실행 가능한 게스트 메모리 밖의 콜백 주소는 명시적으로 실패합니다. 장치 참조는 콜백 반환까지 유지합니다. 언로드에는 모든 작업 항목 해제와 큐 작업 완료가 필요합니다. CPU 컨텍스트는 일반, SIMD, FPU 및 제어 상태를 저장하고 복원합니다. 게스트 메모리는 공유되며 장애가 난 CPU는 저장된 컨텍스트로 재개할 수 없습니다.
파일 객체 또는 대기/실행 중인 작업 항목 참조가 남아 있으면 삭제를 연기합니다. 객체 영역이 소진되면 작업 항목 할당은 NULL을 반환합니다.

시나리오에서 유효한 재배치 주소를 지정하지 않으면 이미지는 선호 베이스를 사용하며, native 서브시스템의 PE32+ x64 실행 파일이어야 합니다. import 제공자는 `ntoskrnl.exe`, `ntkrnlmp.exe` 또는 `WDFLDR.SYS`일 수 있습니다. 실행 로더는 검증된 x64 `DIR64` 베이스 재배치와 제한적인 security-cookie 로드 구성을 지원합니다. security cookie는 진입 래퍼 실행 전에 결정적인 게스트 값으로 초기화됩니다. 기타 모델링되지 않은 로드 구성 필드, TLS, 지연/바인딩 import, ordinal import, managed 이미지는 거부됩니다. 이미지에는 엄격한 범위 및 정렬 검사도 적용됩니다.

활성 Control Flow Guard(CFG)는 PE 플래그, 포인터 슬롯과 정렬된 실행 가능 대상 테이블을 검증합니다. check/dispatch 도우미는 선언된 이미지 진입점 또는 등록된 API 썽크만 허용하며 Win64 호출 상태를 보존하고 미선언 대상을 거부합니다. CFG가 활성화되지 않은 계측은 원래 게스트 대체 포인터를 유지합니다. 활성 XFG, 내보내기 억제 및 다른 미지원 보호 정책은 거부되며 실행 가능 메모리에 있다는 이유만으로 유효한 대상이 되지는 않습니다.

WDM 스택에는 같은 게스트 드라이버가 소유한 여러 장치를 둘 수 있습니다. `IoAttachDeviceToDeviceStack`은 독립 소스 장치를 대상의 현재 최상단에 연결하고 이전 최상단을 반환합니다. `StackSize`와 `AlignmentRequirement`를 설정하지만 `NextDevice` 목록이나 버퍼 플래그는 변경하지 않습니다. `IoDetachDevice`는 저장한 하위 장치를 받으며 `PASSIVE_LEVEL`이 필요합니다. 연결은 `DISPATCH_LEVEL` 이하에서 가능합니다. 이름 있는 하위 장치를 열면 현재 최상단으로 디스패치하지만 `FILE_OBJECT.DeviceObject`와 보고서는 이름 있는 장치 신원을 유지합니다. READ/WRITE 버퍼링은 선택한 최상단의 플래그를 사용합니다. 요청이 보존한 경로는 분리·삭제 후에도 디스패치 반환까지 각 장치를 유지하며, 내부 참조는 열린 핸들 수인 `ReferenceCount`에 더해지지 않습니다.

`IofCallDriver`와 `IoCallDriver` 도우미는 보존 경로에서 지정된 정확한 대상을 호출합니다. 실제 인라인 `IoCopyCurrentIrpStackLocationToNext`, `IoSkipCurrentIrpStackLocation`, `IoSetCompletionRoutine`은 원래 게스트 IRP를 조작하며 커서, 개수, 제어 플래그를 검증합니다. 하위 디스패치의 실제 반환 상태는 `IoStatus` 및 완료 루틴 반환값과 분리됩니다. 완료 처리는 커서를 전진시키고 성공/오류/취소 조건에 따라 콜백을 호출하며 pending을 위로 전달합니다. 완료 루틴이 실행되면 해당 루틴이 전달을 책임지며, 디스패치가 이미 `STATUS_PENDING`을 반환한 뒤에도 전달할 수 있습니다. `STATUS_MORE_PROCESSING_REQUIRED`는 IRP, MDL, 버퍼를 유지한 채 완료 해제를 멈추고 이후 완료 호출로 재개합니다. 중첩 완료에는 외부 루틴의 중단 결과가 필요하며 최종 해제 시 저장소를 한 번만 폐기합니다. 소유 하위 시스템이 표시된 continuation은 WDM/WDF 호출 프레임과 상속 IRQL을 보존합니다. 상위 완료 콜백 전에 소비된 하위 스택 위치를 0으로 지웁니다.

일반 전원 관리, 드라이버 할당 IRP, 기타 PnP 부 기능, 기타 하드웨어/리소스 모델은 지원하지 않습니다. WDF 연결/전달, 파일이나 콜백이 남은 스택에 연결, 중간 계층 분리, 전달 중 주 기능 변경, 보존 경로 외부 대상으로 전달하면 명시적으로 실패합니다. 실제 WDK의 선택적 `driver_wdm_stack.c`는 `NEVERD_WDM_STACK_FIXTURE`와 `NEVERD_WDM_STACK_CFG_FIXTURE`로 구성합니다. 네이티브와 C API/CLI 검증에는 재배치가 포함되며 산출물 누락 시 명시적으로 건너뜁니다. 실행 근거는 Linux에 한정됩니다.

시나리오는 최대 64개의 `pnp_devices`를 명시적으로 구성할 수 있습니다. 각 항목에는 `id`, `bus: "resource_free"` 또는 `bus: "register_bank"`, `initial_device_power: "D0"`, `initial_system_power: "working"`이 필수이며 누락된 사실을 추정하지 않습니다. ID는 대소문자를 구분하는 1–64바이트 ASCII로, 첫 문자는 영숫자이고 나머지는 영숫자, `_`, `-`, `.`만 허용합니다. 일반 요청은 `device` 대신 구성된 `device_id`를 사용할 수 있으나 둘을 함께 지정할 수 없습니다. `kind: "pnp"`는 `device_id`, `minor`, `bus_completion`이 필수입니다. 지원 minor는 `start`, `query_remove`, `cancel_remove`, `remove`, `query_stop`, `stop`, `cancel_stop` 및 `surprise_removal`입니다. `bus_completion.status`는 필수 32비트 정수 또는 16진수 문자열이며, 선택적 `delay_100ns`는 INT64_MAX 이하의 음이 아닌 정수로 제공자의 실제 수신부터 계산합니다. 최종 상태는 `STATUS_PENDING`일 수 없고 stop/cancel-stop/surprise-removal/cancel-remove/remove는 정확히 `STATUS_SUCCESS` (0)이 필요합니다. PnP 요청은 파일/전송/취소 필드를 값이 0이어도 거부합니다. C++ API도 동일하게 사전 검증합니다.

```json
{
  "pnp_devices": [
    {"id": "sensor0", "bus": "resource_free", "initial_device_power": "D0", "initial_system_power": "working"}
  ],
  "requests": [
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0", "delay_100ns": 10}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "surprise_removal", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "remove", "bus_completion": {"status": "0x0"}}
  ],
  "unload": true
}
```

DriverEntry 성공 후 각 구성 PDO에 대해 `AddDevice`를 한 번 실행합니다. 제공자는 별도 `DRIVER_OBJECT`를 소유하며 게스트는 제공자 객체를 삭제하거나 가장할 수 없습니다. PnP IRP는 `KernelMode`, 파일 없음, START 리소스는 구성된 버스에 따름, 초기 `STATUS_NOT_SUPPORTED`입니다. 전달이 해당 PDO에 도달해야 버스 응답을 사용하고, 지연 완료는 공유 가상 시계와 기존 완료 continuation을 사용합니다. 최종 상위 완료가 버스 상태와 별도로 수명 주기 확정/롤백을 결정합니다. Started에서 정상 제거하려면 query 성공, 파일 닫기, 이전 요청 종료, 게스트의 분리/삭제가 필요합니다. 누수 없는 AddDevice 실패는 제공자만 퇴역시키고, 새 게스트 장치 누수는 분리된 경우에도 `model_error`입니다. unload 전에 모든 제공자가 없어야 합니다. 기타 PnP, 일반 전원 관리, 기타 하드웨어/리소스 및 KMDF PnP는 포함하지 않습니다. PnP 성공에는 실제 제공자 완료가 필요하며 START/QUERY_STOP/QUERY_REMOVE의 이른 상위 실패는 버스 관측을 null로 남길 수 있습니다. 장치/파일 수명 주기 식별자는 분리 후에도 유지됩니다.

초기 구성은 `configuration.pnp_devices`에 보존합니다. 관측 `pnp_devices`에는 `id`, `pdo`, nullable `add_device_status`, 현재 `attached`, `pnp_state`, `provider_present`가 있으며 제거 후 `attached`는 false입니다. AddDevice 단계는 `add_device:<ID>`이고 실패는 `scenario_success`에 반영하되 DriverEntry의 `nt_status`를 덮어쓰지 않습니다. 각 요청에 nullable `device_id`, `pnp`가 추가되며 PnP의 `file`은 null입니다. `pnp`는 `minor`, `state_before`, `state_after`, nullable `bus_status`, `bus_received_at_100ns`, `bus_completed_at_100ns`를 기록합니다. 구성된 상태는 실제 버스 완료 시에만 관측되며 수신 시각은 독립적입니다. 기존 필드 형식은 바뀌지 않습니다.

Removing/Removed 외에 장치가 존재하면 일반 CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE는 실제 게스트 디스패치로 전달됩니다. 모델은 Stopped, StopPending, RemovePending 또는 전원 상태만으로 실패를 만들지 않습니다. 드라이버 코드가 소프트웨어 I/O 완료, 거부 또는 보류를 결정합니다. 공개 실행기는 순차적입니다. 보류 IRP에 현재 사용 가능한 생산자가 없으면 나중 시나리오의 start/cleanup으로 해제할 수 없고 정체 `model_error`로 끝납니다. Remove 이전 파일 닫기와 기존 요청/콜백 종료는 프로필 제한입니다. `query_stop` 최종 `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119)는 구현되지 않은 리소스 재질의를 요구하므로 사전 검증과 게스트 최종 완료에서 모두 거부합니다. [Microsoft QUERY_STOP 계약](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device)을 참고하세요. 중지/재시작 및 갑작스러운 제거는 리소스 재배치, 일반 전원 관리 또는 KMDF PnP 지원을 추가하지 않습니다.

리소스 없는 PnP는 원본 실제 WDK `driver_wdm_pnp.c`, 선택적 `NEVERD_WDM_PNP_FIXTURE`/`NEVERD_WDM_PNP_CFG_FIXTURE`와 네이티브 및 C API/CLI 테스트를 사용합니다. 산출물 누락은 명시적으로 건너뛰며 실행 근거는 Linux에 한정됩니다.

합성 버스 `bus: "register_bank"`는 PDO에 명시적인 고정 메모리 리소스를 제공합니다. `interrupts`가 비어 있지 않으면 `resources` 배열은 선택 사항이며 두 목록을 합쳐 적어도 하나의 리소스가 있어야 합니다. 각 메모리 항목은 `id`, `raw_start`, `translated_start`, `length`, `registers`를 포함하며, 레지스터마다 `offset`, `width`, `access`(`read_only` 또는 `read_write`), 초기 `value`가 필요합니다. `DriverResources.h`／`DriverResources.def`가 C++와 JSON의 공통 계약을 정의합니다. 리소스 ID는 길이가 제한된 ASCII 식별자 규칙을 따르며 PDO 안에서 고유합니다. 한도는 PDO당8개／전체32개 리소스, 리소스당256개／전체4096개 레지스터, 리소스당1–1048576바이트입니다. 자연 정렬된 정확한1／2／4바이트 접근만 지원하며 값은 해당 폭에 들어가야 합니다. 두 물리 구간 모두 오버플로가 없어야 하고, raw 구간은 PDO 안에서, 변환된 구간은 전체에서 겹칠 수 없습니다. 빈 `registers`는 뱅크 전체가 접근 불가임을 명시합니다. 주소와 초기값은 선언된 사실이며 호스트 하드웨어나 암묵적인 영 초기화 메모리가 아닙니다. `resource_free`는 리소스 목록을 생략하고 START 포인터를 null로 유지합니다.

START는 독립적인 읽기 전용 raw／변환된 `CM_RESOURCE_LIST` 할당을 받습니다. 대응하는 Memory 설명자는 동일한 순서를 사용하며 full 설명자1개, Internal 인터페이스, 버스0, version／revision 1, DeviceExclusive 공유와 READ_WRITE 구간 플래그로 구성됩니다. 레지스터 자체의 RO 권한은 독립적입니다. 하위 START가 성공하면 상위 완료 콜백 전에 할당을 사용할 수 있습니다. NotStarted／Stopped에서 시작하는 각 START는 같은 고정 할당에 새 리소스 세대를 만듭니다. 값은 PDO 생성 시 한 번만 초기화하며 unmap, STOP, 재시작에도 유지합니다. START 실패와 STOP／REMOVE 성공은 최종 IRP 완료 전에 드라이버가 매핑을 해제해야 하며 모델이 자동 정리하지 않습니다. 갑작스러운 제거는 즉시 새 map과 레지스터 접근을 금지하지만 기존 매핑의 unmap은 가능합니다. 제공자의 실제 성공한 장치 SET 완료가 하드웨어 접근성을 변경합니다. D3는 접근을 막고 D0는 사용 가능한 할당이 있을 때만 허용합니다. D3에서도 접근 없는 매핑은 가능하며 전원 변경은 매핑이나 값을 버리지 않습니다.

`MmMapIoSpace`는 NonCached를, `MmMapIoSpaceEx`는 PAGE_NOCACHE와 PAGE_READONLY 또는 PAGE_READWRITE 조합을 지원합니다. 단일 할당에 선언된 변환 후 하위 구간만 페이지 오프셋을 보존하여 매핑합니다. 별칭은 하나의 뱅크를 공유하고 독립 권한을 유지하며 `MmUnmapIoSpace`에는 원래 시작 주소와 정확한 길이가 필요합니다. 매핑 수／가상 주소 창 또는 설정된 메모리 예산이 소진되면 NULL을 반환하고 백엔드 결함은 명시적인 실패로 남습니다. unmap한 주소는 후속 매핑으로 다시 유효해지지 않습니다. 스칼라와 실제 REP 레지스터 버퍼 명령은 CPU MMIO 검사를 거칩니다. 선언되지 않은 부분, 잘못된 폭, 비정렬, RO 쓰기, 매핑 경계 통과와 실행은 레지스터 효과 전에 실패합니다. 메모리 API도 자연 정렬된 단일 레지스터에 정확한1／2／4바이트 트랜잭션을 실행할 수 있습니다. MMIO를 포함하는 더 큰 일괄 구간은 레지스터 트랜잭션으로 자동 분할하지 않고 명시적으로 거부합니다. 임의 물리 RAM, 리소스 재배치, 포트, 기타 인터럽트 모드, 기타 DMA 인터페이스나 일반 하드웨어 동작은 지원하지 않습니다.

실행 가능한 [레지스터 뱅크 시나리오](../examples/driver-register-bank-scenario.json)는 자체 정품 WDK `driver_wdm_resources.c`와 선택적 `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`를 사용합니다. 지연 START, 파일 I/O, STOP, 재시작, 제거에 걸친14개 요청을 실행하고 드라이버 IOCTL로 유지된 값을 관측합니다. `configuration.pnp_devices[].resources`는 물리 주소를 손실 없는16진수로 기록한 초기 사실이며 별도의 뱅크 상태 보고서가 아닙니다. C API와 Python은 기존 `scenario_json` 진입점과 변경 없는 `neverd_driver_options_v1`을 사용합니다. 실제 산출물 부재는 명시적으로 건너뛰며 현재 실행 증거는 Linux에 한정됩니다.

같은 `register_bank` 제공자는 `interrupts`만 또는 `resources`와 함께 선언할 수 있으며 적어도 한 목록은 비어 있지 않아야 합니다. `resource_free`는 `[]`를 포함한 명시적 `interrupts` 필드를 거부합니다. 인터럽트마다 `id`, `raw_vector`, `raw_level`, `raw_affinity`, `translated_vector`, `translated_level`, `translated_affinity`, `mode: "latched"`, `share: "device_exclusive"`가 필요합니다. `DriverInterrupts.h` / `DriverInterrupts.def`가 타입, 표기와 한도를 관리합니다. PDO당 8개／전체 32개, PDO 안에서 고유한 길이 제한 ASCII ID, 전역 독점 변환 벡터, raw level 0–65535, 변환 DIRQL 3–12를 허용합니다. 벡터는 32비트 전체를 유지하며 벡터와 IRQL의 관계를 추정하지 않습니다. 두 affinity 마스크는 모두 1이어야 하며 CPU0와 group0은 명시적 제공자 사실입니다. raw 값과 변환 값은 서로 독립적입니다. 같은 `CM_RESOURCE_LIST`에서 메모리 설명자 순서를 유지하고 그 뒤에 순서대로 인터럽트 설명자를 배치합니다. 인터럽트 설명자는 type 2, DeviceExclusive 공유와 LATCHED 플래그를 사용합니다. 이 합성 에지 트리거 프로필은 PCI 공유 레벨 인터럽트 라인을 나타내지 않습니다.

READ／WRITE／IOCTL 요청은 `interrupt_events`를 선언할 수 있으며 각 항목에 `after_100ns`, `device_id`, `interrupt_id`를 명시해야 합니다. 다른 요청 종류는 빈 필드도 거부합니다. 요청당 최대 64개／전체 1024개 이벤트와 INT64_MAX 이하의 음이 아닌 지연을 허용합니다. 요청 제출 성공 시점을 지연의 기준으로 삼고 이미 연결된 인터럽트와 해당 PDO 리소스 세대를 캡처합니다. 이벤트는 독립적인 외부 라인 펄스입니다. 원본 IRP 완료가 이벤트를 취소하지 않으며 레지스터 쓰기로 enable, status, acknowledgement 동작을 추론하지 않습니다. 시간은 유휴 상태에서만 진행하고 기한에 도달한 이벤트는 다음 지원 콜백 경계에서 실행됩니다. 따라서 `after_100ns: 0`은 명령어 단위 선점이나 첫 게스트 명령어 이전 전달을 보장하지 않습니다. 같은 시각의 생산자 용량은 함께 사전 검증합니다. 제공자 하드웨어 상태 공개 후 전달 가능 여부를 판단하고 ISR 콜백은 DPC／작업자보다 먼저 실행합니다. 연결 손실, 오래되었거나 사용할 수 없는 세대, 물리 D3는 `undelivered_reason`을 기록한 뒤 `model_error`로 중단합니다. 이벤트를 새 연결로 옮기거나 조용히 지연시키지 않습니다.

`IoConnectInterrupt`는 실제 11개 인수와 정확한 변환 할당을 사용합니다. `IoConnectInterruptEx`는 FullySpecified(1), LineBased(2, 명시적 PDO에 할당된 한 라인), FullySpecifiedGroup(4, group 0)을 지원하며 `IoDisconnectInterruptEx`에는 일치하는 버전／컨텍스트가 필요합니다. 등록과 연결 해제는 PASSIVE_LEVEL에서 수행해야 합니다. 전용 인터럽트 잠금, 독점 latched 모드, CPU0／group0, 부동소수점 상태 저장 없음만 지원합니다. 동기화 IRQL은 할당 DIRQL과 같아야 하며 LineBased의 0은 그 수준을 선택합니다. `KINTERRUPT`는 불투명하며 주소를 재사용하지 않습니다. 실제 ISR은 `(Interrupt, ServiceContext)`를 받고 AL의 BOOLEAN을 반환합니다. `FALSE`는 인터럽트를 맡지 않았다는 뜻이며 NTSTATUS 실패가 아닙니다. `KeSynchronizeExecution`은 DIRQL에서 같은 잠금 아래 실제 단일 인수 콜백을 실행하고 해당 BOOLEAN을 반환하면서 호출자 IRQL／CR8을 복원합니다. `KeAcquireInterruptSpinLock` / `KeReleaseInterruptSpinLock`은 비재귀 소유권, 원래 실행과 저장된 IRQL을 검증하며 잠금을 보유한 상태로 콜백에서 반환할 수 없습니다. 인터럽트 내 대기, 호출자가 제공하는 공유 잠금, 공유／레벨／MSI／수동 수준 인터럽트와 명령어 단위 인터럽트 선점은 지원하지 않습니다. START 실패 및 STOP／REMOVE 성공은 최종 완료 전에 연결을 해제해야 하며 상위 완료 콜백에서 먼저 해제할 수 있습니다. 연결 해제가 대기열의 DPC를 조용히 버리지는 않습니다.

보고서는 선언과 관측을 구분합니다. `configuration.pnp_devices[].interrupts`는 리소스를 보존하고 `configuration.interrupt_events`는 입력 이벤트를 0부터 시작하는 `source_request_index`(설정 요청 인덱스), `event_index`와 함께 평탄화합니다. 루트 `interrupts` 행에는 `device_id`, `interrupt_id`, `epoch`, 절대 `due_at_100ns`와 null을 허용하는 `occurred_at_100ns`, `delivered_at_100ns`, `returned_at_100ns`, `interrupt_object`, `return_value`, `claimed`, `undelivered_reason`이 추가됩니다. `claimed`는 실제 반환값의 하위 바이트로만 결정하며 타임스탬프는 관측값이지 가공한 콜백 결과가 아닙니다. `scenario_success`에는 구성한 모든 이벤트가 미전달 실패 없이 반환해야 하지만 인터럽트를 맡지 않은 ISR도 유효합니다. DPC 효과는 실제 요청 완료, API 호출과 메시지에 나타납니다. 실행 가능한 [인터럽트 시나리오](../examples/driver-interrupt-scenario.json)는 실제 WDK로 빌드하는 자체 fixture `driver_wdm_interrupts.c`와 선택적 `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`를 사용합니다. 7개 요청에서 지연 START, ISR→DPC로 완료되는 대기 IOCTL, 파일 정리／닫기와 제거를 실행합니다. 기존 C／Python `scenario_json` 경계와 `neverd_driver_options_v1` 레이아웃은 변경하지 않습니다. 실제 산출물 누락은 명시적으로 건너뛰며 실행 증거는 Linux에 한정됩니다.

`DriverDMA.h` / `DriverDMA.def`는 메모리／인터럽트 할당과 함께 `register_bank` PDO에 선택적 `dma` 객체를 추가합니다. DMA만으로 두 리소스 목록을 대체할 수 없습니다. 일곱 필드를 모두 명시해야 합니다: `address_bits`(32 또는 64), `maximum_length`(1–1048576바이트), `map_registers`(1–256), `alignment`(1–4096의 2의 거듭제곱), `logical_base`(0이 아니며 페이지 정렬), `logical_length`(페이지 정렬, 4096–1073741824바이트), 불리언 `scatter_gather`. 논리 범위는 오버플로 없이 주소 폭에 들어가야 합니다. PDO마다 독립적인 논리 도메인이 있으므로 서로 다른 장치의 같은 주소는 별칭이 아닙니다. 변환된 MMIO 리소스는 예약된 모델 RAM 범위 `[0x1000000000, 0x1000100000)`와 겹칠 수 없습니다. 이 선언은 일관성을 갖춘 합성 버스 마스터를 나타내며 호스트 물리 메모리나 PCI 장치를 나타내지 않습니다.

`IoGetDmaAdapter`는 Internal 버스 마스터의 기존 `DEVICE_DESCRIPTION` 버전 0／1 필드를 받아 버전 1 `DMA_ADAPTER`와 실제 104바이트 `DMA_OPERATIONS` 테이블을 제공합니다. 버전 2／3 탐색은 최신 구조의 뒷부분을 읽지 않고 NULL을 반환합니다. 각 간접 메서드는 정확한 유효 어댑터에 연결되며 커널 import와 별개의 식별자를 가집니다. 구현된 메서드는 `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `GetScatterGatherList`, `PutScatterGatherList`, `PutDmaAdapter`, `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers`, `FreeMapRegisters`입니다. 이 프로필은 종속／시스템 DMA 컨트롤러를 모델링하지 않으므로 `FreeAdapterChannel`과 `ReadDmaCounter`는 이름을 명시하는 미지원 오류로 남습니다. 공통 버퍼 할당／해제와 정렬 조회는 PASSIVE_LEVEL, Get／PutScatterGatherList는 DISPATCH_LEVEL을 요구하며 어댑터 해제는 DISPATCH_LEVEL 이하에서 허용합니다. x64는 `CacheEnabled`를 무시합니다. 미지원 버전 탐색, 선언된 기능과의 불일치, 문서에 규정된 할당 자원 부족은 NULL을 반환합니다. 잘못되거나 모델링되지 않은 인터페이스 선택과 백엔드 실패는 명시적 오류입니다.

`AllocateAdapterChannel`은 DISPATCH_LEVEL을 요구하고 NULL이 아닌 불투명 매핑 레지스터 토큰을 예약합니다. 공통 버퍼, SG 목록, 채널 예약은 PDO별 같은 할당량과 FIFO를 공유합니다. 성공하면 실제 `AdapterControl`을 즉시 실행하거나 큐에 넣습니다. 요청 개수가 너무 크면 콜백 없이 `STATUS_INSUFFICIENT_RESOURCES`를 반환합니다. 게스트 장치마다 미완료 할당 콜백은 하나만 허용합니다. AdapterControl 안에서 AllocateAdapterChannel을 호출하면 중첩 콜백을 거쳐도 거부합니다. 콜백의 네 인수에는 등록 시점에 캡처한 실제 `DEVICE_OBJECT.CurrentIrp`가 포함됩니다. 게스트 장치의 이 정확한 8바이트 필드는 쓰기 가능하며, 0 또는 해당 장치로 라우팅되는 유효 IRP를 허용합니다. 대기 콜백은 진입할 때까지 패킷을 유지하며 진입 후에는 직접 완료할 수 있습니다. 이 프로필에는 여전히 StartIo가 없으므로 별도 SG 콜백의 사용하지 않는 IRP 인수는 NULL입니다.

`AdapterControl`은 32비트 `IO_ALLOCATION_ACTION`을 반환합니다. RAX 상위 비트는 무시하며 이 동작은 AllocateAdapterChannel의 `STATUS_SUCCESS`를 대체하지 않습니다. `DeallocateObject`는 콜백 반환 시 미사용 또는 전체 플러시가 끝난 레지스터를 해제합니다. `DeallocateObjectKeepRegisters`는 정확한 어댑터, 토큰, 원래 개수로 FreeMapRegisters를 호출할 때까지 유지합니다. `KeepObject` 반환에는 모델링하지 않은 시스템 컨트롤러가 필요하므로 명시적으로 실패합니다. 새로 전달된 할당은 콜백이 반환하기 전에 보유 할당으로 해제할 수 없습니다. 이전부터 보유한 다른 할당은 해당 계약이 허용하면 해제할 수 있습니다. 콜백 식별자, 보유 레지스터, 활성 매핑 바이트의 수명은 서로 다르며 어댑터나 장치를 제거하기 전에 모두 확인합니다.

`MapTransfer`와 `FlushAdapterBuffers`는 DISPATCH_LEVEL 이하를 허용하며 채널 할당과 레지스터 해제는 DISPATCH_LEVEL을 요구합니다. MapTransfer는 MDL 상대 위치를 받고 실제 ULONG 길이를 읽고 수정하며 논리 주소를 값으로 반환합니다. 제한된 SG 프로필은 호출마다 기반 페이지 조각 하나를 반환합니다. 같은 MDL과 방향에서 바로 이어지는 위치는 하나의 작업을 확장합니다. 비 SG는 예약 개수에 맞으면 요청 범위 전체를 한 번에 매핑하며 길이를 줄이지 않습니다. 첫 매핑은 레지스터 예약 크기에 맞춘 재사용하지 않는 논리 범위를 확보하므로 다른 할당이 중간에 들어와도 겹치지 않습니다. 모든 조각은 커지는 단일 물리 고정 범위를 공유합니다. 장치 트랜잭션은 현재 매핑된 작업 전체를 가로지를 수 있습니다. CPU 접근, MDL 해제, 고정된 저장소를 무효화하는 완료는 전체 플러시까지 차단됩니다. Flush는 시작 위치, MDL, 방향 및 실제 매핑한 총길이가 일치해야 합니다. 매핑한 바이트만 해제하고 레지스터는 유지하므로 보유 토큰을 다음 작업에 사용할 수 있습니다. 부분 플러시, 여러 MDL을 섞은 작업, 다른 MapTransfer 패턴은 이 프로필 범위 밖이며 모든 Windows 시스템에서 잘못됐다고 가정하지 않습니다.

`KeFlushIoBuffers`는 유효한 잠금／비페이지 MDL을 검증합니다. 모델 플랫폼은 일관성을 보장하므로 ReadOperation과 DmaOperation이 어떤 값이어도 별도 캐시 복사가 필요 없습니다. 이 호출은 DMA 소유권을 해제하거나 FlushAdapterBuffers를 대신하지 않습니다. [채널 시나리오](../examples/driver-dma-channel-scenario.json)는 직접 작성한 `driver_wdm_dma_channel.c`로 두 번의 MapTransfer, 페이지를 가로지르는 하나의 장치 트랜잭션, 별도 선언한 IRQ/DPC, 전체 플러시와 정확한 레지스터 해제를 실행합니다. 실제 일반／CFG 이미지는 `NEVERD_WDM_DMA_CHANNEL_FIXTURE`와 `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`를 사용합니다.

`KernelPhysicalMemory`는 기존 RAM에 최대 256개의 4096바이트 모델 물리 페이지 식별자를 할당합니다. CPU 가상 주소, 물리 페이지 식별자, 장치 논리 주소는 서로 다릅니다. 구성된 MDL의 PFN 배열은 이 공유 식별자를 읽기 전용으로 노출하며 미구성 설명자에는 사용 가능한 PFN이 없습니다. 인접한 작은 할당은 PFN을 공유할 수 있지만 바이트 범위와 수명은 별개입니다. 공통 버퍼, 풀 저장소, 요청 버퍼는 `GuestMemory`가 소유한 같은 바이트를 사용하며 별도 DMA 복사본을 만들지 않습니다. 유효한 SG 매핑은 정확한 데이터 범위와 설명자를 고정합니다. 완료, 풀／MDL 해제, 장치 정리는 저장소를 폐기하기 전에 남은 의존성을 거부합니다. 직접 MDL의 unmap은 CPU 시스템 매핑만 취소하며 DMA는 잠긴 기반 RAM에 계속 접근할 수 있습니다. `DmaWritable`은 CPU 매핑 권한과 별도로 쓰기 잠금 계약을 기록합니다. 장치 쓰기는 직접 READ／OUT_DIRECT 또는 쓰기 가능한 비페이지 저장소를 요구하며 WRITE／IN_DIRECT는 CPU 매핑이 쓰기 가능하다는 이유만으로 이 권한을 얻지 않습니다.

`GetScatterGatherList`는 MDL의 원래 범위에 대해 CurrentVa／Length를 검증하고 기존 기반 RAM에 논리 페이지 조각을 만듭니다. 매핑 레지스터가 있으면 실제 인자 네 개의 void `AdapterListControl`이 API 반환 전에 인라인으로 실행됩니다. 부족하면 데이터／설명자를 유지하고 자원이 해제될 때까지 PDO FIFO에 콜백을 예약합니다. 이 범위에는 StartIo 소유권이 없으므로 두 번째 IRP 인자는 NULL입니다. 콜백 반환은 매핑을 해제하지 않습니다. 유효한 SG 데이터 범위에 CPU가 접근하려면 먼저 Put해야 합니다. 매핑 레지스터를 기다리는 콜백은 아직 바이트 소유권을 장치에 넘기지 않았습니다. `PutScatterGatherList`는 콜백 안에서 호출할 수 있으며 이후 요청을 완료하고 마지막 어댑터를 해제해도 콜백의 이어지는 실행과 장치 참조는 반환까지 유지됩니다. 공통 버퍼를 해제할 때는 원래 어댑터, 길이, 논리 주소, CPU 주소가 일치해야 합니다. 논리 주소는 재시작을 포함한 세션 내에서 재사용하지 않습니다. 실제 생산자가 없는 자원 대기는 명시적으로 정체를 보고하며 완료나 기한을 만들어 내지 않습니다.

`dma_events`는 READ／WRITE／IOCTL 요청만 받습니다. 모든 이벤트에는 `after_100ns`, `device_id`, `logical_address`, `direction`, `length`가 필요합니다. `write_memory`에는 정확한 길이의 `data_hex`도 필요하며 `read_memory`는 이 필드를 거부합니다. 방향은 장치 관점입니다. 제한은 요청당 64개, 전체 1024개, 총 트랜잭션 바이트 16 MiB, 건당 1 MiB이며 지연은 0부터 INT64_MAX까지입니다. 제출 시 PDO의 현재 할당 세대와 가상 시간 기준점을 저장하지만 이후 dispatch가 만들 매핑이 이미 존재할 필요는 없습니다. 전달 시 완전한 유효 논리 범위와 방향을 찾고 물리 D0 및 모든 기반 바이트를 효과 발생 전에 검증합니다. 원본 IRP 완료는 이벤트를 취소하지 않습니다. 없거나 해제된 매핑, 오래된 세대, 갑작스러운 제거, D3는 실패를 기록하고 중단합니다. 이벤트는 다시 연결되거나 인터럽트, 레지스터 프로토콜, IRP 완료를 만들어 내지 않습니다. 같은 스케줄 경계에서는 제공자의 하드웨어 상태 공개, DMA 바이트 접근, 별도로 선언한 인터럽트 펄스 순서로 처리합니다. 시간은 협력적이며 명령 단위 선점은 없습니다.

보고서는 `configuration.pnp_devices[].dma`와 평탄화한 `configuration.dma_events`를 보존합니다. 루트 `dma_transfers` 행에는 `source_request_index`, `event_index`, `device_id`, `epoch`, `logical_address`, `direction`, `length`, `due_at_100ns` 및 null이 가능한 `occurred_at_100ns`, `completed_at_100ns`, `mapping`, `adapter`, `failure_reason`가 있습니다. `data_hex`는 실제 전송 바이트입니다. `scenario_success`가 되려면 선언된 모든 트랜잭션이 실패 없이 끝나야 합니다. [DMA 시나리오](../examples/driver-dma-scenario.json)는 직접 작성하고 실제 WDK로 빌드한 `driver_wdm_dma.c`와 선택적 `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE`를 사용합니다. 실제 어댑터 포인터로 공통 버퍼에 접근하며 독립적으로 선언된 ISR→DPC가 완료합니다. C／Python은 `neverd_driver_options_v1` 변경 없이 `scenario_json`을 계속 사용합니다. 산출물이 없으면 명시적으로 건너뛰며 실행 증거는 Linux에 한정됩니다. 종속 컨트롤러, V2／V3 메서드, 하드웨어 설명자 엔진, 일반 KMDF DMA와 기타 장치 모델은 지원하지 않습니다.

WDM remove-lock은 실제 export인 `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx`, `IoReleaseRemoveLockEx`, `IoReleaseRemoveLockAndWaitEx`를 실행합니다. Ex 없는 WDK 이름은 매크로입니다. 잠금은 정렬된 전체 저장소를 확장부에 포함하는 정확한 DEVICE_OBJECT에 속하며 PDO 상태나 Tag 형태로 소유자를 추론하지 않습니다. 연결 전 초기화가 가능합니다. retail 32바이트/DBG 120바이트와 일치하는 별도 크기 인수가 필요하며 등록 영역 전체는 불투명합니다. NULL/중복 Tag를 잠금별로 세고 역참조하지 않으므로 IRP 완료 후에도 해제할 수 있습니다. 초기화/AndWait는 `PASSIVE_LEVEL`, acquire/release는 `DISPATCH_LEVEL`까지 허용합니다.

AndWait는 획득 입구를 닫고 일치하는 획득 하나를 해제한 뒤 나머지가 사라질 때까지 실제 게스트 프레임을 중단합니다. 이후 acquire는 `STATUS_DELETE_PENDING`이며 해제 의무를 추가하지 않습니다. 최종 release는 콜백 반환 전에 준비 상태를 고정하므로 워커가 해제 후 REMOVE continuation의 이벤트를 기다릴 수 있습니다. 가상 콜백, 시간 초과, 생산자 없는 성공을 만들지 않습니다. 소유자를 포함하는 연관 REMOVE 경로와 실제 제공자 수신이 필요하며 `bus_received_at_100ns`의0도 유효합니다. 하위 완료는 필요 없지만 제공자에 닿기 전 하위 큐가 REMOVE를 보류하는 경로는 범위 밖이며 완전한 OutsideRemoveDevice/Driver Verifier 검사가 아닙니다. REMOVE 전 파일 닫기와 이전 요청 종료는 유지하지만 잠금을 해제할 콜백은 남아 있어도 됩니다. 경로는 대기·분리/삭제·하위 pending 동안 보존되고 나머지 프레임이 반환한 뒤 최종 해제됩니다.

알 수 없거나 크기가 다른 저장소, 대응 없는 release, 중복 drain, 재초기화, 획득이나 미소비 drain 대기가 남은 확장부 삭제는 변경 전에 실패합니다. 깨끗한 AddDevice 실패에서는 초기화한 미사용 잠금을 삭제할 수 있습니다. 잠금은 장치/작업 항목 참조를 대체하지 않으며 실제 확장부 퇴역 때 등록을 해제합니다. 디버그 정보는 Verifier 시간/최고 수위 제한을 활성화하지 않습니다. 정품 `driver_wdm_remove_lock.c`는 `NEVERD_WDM_REMOVE_LOCK_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`의4종을 사용합니다. 없으면 명시적으로 건너뛰며 네이티브 및 C API/CLI 증거는 Linux만 해당합니다. 완전한 제거 관리자나 일반 동시 I/O 배출 지원을 뜻하지 않습니다.

WDM 전원 요청은 `kind: "power"`와 구성된 `device_id`를 사용합니다. 모든 패킷에 `minor`(`query`/`set`), `power_type`(`device`/`system`), `power_state`(`D0`/`D3` 또는 `working`/`sleeping3`), `power_action`(`none`/`sleep`), 32비트 정수나 16진수 문자열 `system_context`, `bus_completion`이 필수입니다. System Query의 Working 대상과 파일·전송·취소 필드는 지원하지 않습니다. 전체 `system_context`는 불투명한 명시적 사실이며 부모 요청, 최대 절전이나 빠른 시작을 추론하지 않습니다. 경로는 `DO_POWER_PAGABLE`이 필요하고 `DO_POWER_INRUSH`가 없어야 하며 전원 디스패치와 `PoRequestPowerIrp`는 `PASSIVE_LEVEL`에서 실행합니다. `PoCallDriver`는 같은 관리 전원 IRP를 전달하고 `PoStartNextPowerIrp`는 추가 직렬화 핸드셰이크가 없는 Vista+ 계약을 따릅니다. 일반 전원 정책, WAIT_WAKE, 기타 상태/동작, 종료/최대 절전, 돌입 전류, 비페이지 경로, 일반 하드웨어 및 KMDF PnP는 범위 밖입니다.

각 `pnp_devices`는 필수 초기 수명 상태 D0/working과 별개인 `initial_reported_device_power: "D0"` 또는 `"D3"`를 지정할 수 있습니다. PDO와 처음 연결된 각 게스트 DEVICE_OBJECT의 알림 상태는 독립적이며 `PoSetPowerState`는 호출 장치의 이전 값만 반환하고 갱신합니다. 초기 사실이 없으면 실제 호출은 D0를 가정하지 않고 실패합니다. 선택적 `requested_device_power`는 같은 필수6필드를 가진 device 템플릿이며 모든 PDO 합계64개까지 허용합니다. 실제 `PoRequestPowerIrp`의 PDO/minor/목표가 해당 FIFO 선두와 일치해야 소비합니다. 누락/불일치는 실패하고 미사용 항목은 요청을 만들지 않으며 context로 부모를 추정하지 않습니다. 자식은 별도 IRP/보고 행, `origin: "PoRequestPowerIrp"`, 0부터 시작하는 `response_index`를 갖습니다. 시나리오 행은 `origin: "scenario"`와 null 인덱스입니다. 동기 자식은 API의 `STATUS_PENDING` 반환 전에5인수 void 콜백을 실행할 수 있고 콜백은 대기할 수 있습니다. System S0는 독립 D0 자식보다 먼저 완료할 수 있습니다. IO_STATUS_BLOCK 스냅샷은 콜백 반환까지 유효합니다.

보고에 nullable `power`를 추가하고 전원 행의 `file`은 null입니다. `power`는 패킷 사실, `device_state_before`/`device_state_after`, `system_state_before`/`system_state_after`, nullable `requested_device_object`, 실제 `bus_status`/`bus_received_at_100ns`/`bus_completed_at_100ns`를 기록합니다. 최종 PnP 장치에는 `device_power`/`system_power`, 살아 있는 장치에는 nullable `reported_device_power`를 추가합니다. `scenario_success`는 시나리오 출처 행만 구성 개수와 비교하지만 모든 실제 자식/시나리오 요청이 성공 완료해야 합니다. 미사용 템플릿은 실패를 만들지 않습니다. 정품 `driver_wdm_power.c`는 `NEVERD_WDM_POWER_FIXTURE`/`NEVERD_WDM_POWER_CFG_FIXTURE`로 일반/active-CFG 네이티브 및 C API/CLI를 검증하며, 파일이 없으면 명시적으로 건너뜁니다. 실행 증거는 Linux로 한정됩니다.

[전체 전원 시나리오](../examples/driver-power-scenario.json)를 `--scenario`로 전달하면 시작, 시스템 query/절전/복귀, 제거와 명시적 자식 응답3개를 실제 픽스처로 실행합니다.

KMDF 1.33 지원은 정확한 1.33.0 ABI를 사용합니다. 458개 함수 슬롯에 안정적인 게스트 식별자가 있으며 아래 40개 API에 실행 의미가 구현되어 있습니다. `WdfVersionBind`와 `WdfVersionUnbind`는 실제 WDK `FxDriverEntry` 래퍼 전후에 게스트 바인딩을 관리합니다. `WdfGetDriver`는 공용 드라이버 전역 구조를 읽습니다. 비 PnP 드라이버, 일반 객체, 제어 장치, 큐 및 들어오는 요청은 형식화된 컨텍스트, 참조 수와 실제 실행되는 정리/파괴/언로드 콜백을 공유합니다. 모델링된 모든 프레임워크 호출과 콜백은 현재 `PASSIVE_LEVEL`을 요구하며 정리 완료 후 새 참조를 얻는 동작은 이 프로필의 범위 밖입니다. 모델링되지 않은 함수 슬롯, `WdfLdrQueryInterface`, 클래스 확장 및 UMDF는 명시적으로 중단합니다.

제어 장치에는 복사된 출력 가능 ASCII 이름과 정확히 `D:P(A;;GA;;;WD)`인 SDDL이 필요합니다. 모든 호출자에게 접근을 허용하므로 호출자 토큰을 임의로 가정하지 않습니다. 다른 보안 설명자, 이름 없는 장치 및 자동 이름은 지원하지 않습니다. 장치 초기화는 WDM 장치 하나를 소유합니다. 요청은 기존 세션 네임스페이스의 심볼릭 링크 별칭 `\DosDevices\Name` 또는 `\??\Name`으로 장치를 선택할 수 있으며 보고서에는 정규 장치 이름이 유지됩니다. 생성에 성공하면 초기화 객체를 소비하고 포인터를 비우며 실패하면 부분적인 장치 소유권을 되돌립니다. `WdfControlFinishInitializing`이 I/O 전달을 허용합니다. 모델링된 파일, 작업 항목 및 요청이 허용할 때만 삭제가 장치와 링크를 제거하며 삭제 중 취소나 요청 비우기는 지원하지 않습니다.

96바이트 `WDF_IO_QUEUE_CONFIG`는 명시적 수동 수준 실행과 프레임워크 동기화 없음으로 설정된 순차 기본 큐를 지원합니다. 제어 장치 큐는 전원 관리를 받지 않습니다. 전용 READ/WRITE/IOCTL 콜백이 기본 콜백보다 우선합니다. 수락된 큐 요청은 동기적으로 완료되어도 `STATUS_PENDING`을 반환하며 void 콜백의 반환 레지스터가 요청을 완료시키지 않습니다. 지연 완료에는 기존 스케줄러를 사용합니다. 처리기가 없으면 `STATUS_INVALID_DEVICE_REQUEST`로 완료하고 길이가 0인 READ/WRITE는 전달을 활성화하지 않으면 바로 완료합니다. 기본 파일 패키지는 CREATE/CLEANUP/CLOSE를 성공 상태와 Information=0으로 완료합니다. 병렬/수동 큐, 파일 콜백, PnP 장치 및 전체 PnP/전원은 지원하지 않습니다.

요청 매개변수는 40바이트 `WDF_REQUEST_PARAMETERS` 레이아웃을 사용합니다. 입력/출력 접근 함수는 논리 길이를 반환하고 버퍼 별칭 및 기존 직접 I/O MDL 매핑을 유지하며 직접 IOCTL 입력은 계속 버퍼 방식을 사용합니다. 잘못된 방향과 버퍼 부족에는 문서에 정의된 상태를 반환합니다. 완료 처리는 요청 정리와 자식 객체 파괴를 실행한 뒤 IRP/버퍼를 무효화하고 참조가 허용할 때 요청을 파괴합니다. 완료가 시작되면 새 버퍼 및 매개변수 접근 함수 호출을 거부하지만 이미 얻은 버퍼 포인터는 정리 중에도 사용할 수 있습니다. 외부 객체 참조는 컨텍스트를 유지할 뿐 완료된 IRP 접근을 유지하지 않습니다. 사용자 모드 KMDF `METHOD_NEITHER`에는 아직 구현되지 않은 사용자 버퍼 검사/잠금 지원이 필요합니다.

취소는 위에서 설명한 제어 장치 큐의 요청에 한해 모델링됩니다. 이미 취소되었다면 `WdfRequestMarkCancelableEx`는 콜백을 호출하지 않고 `STATUS_CANCELLED`를 반환합니다. `WdfRequestUnmarkCancelable`이 성공하면 콜백이 제거되며 이후 취소는 상태만 기록합니다. `WdfRequestIsCanceled`는 취소 가능 표시가 없는 유효한 요청에서 이 상태를 읽습니다. 취소 가능 표시가 성공한 뒤에는 요청 완료에 표시 해제 성공 또는 취소 콜백 전달 시작이 필요하며, 큐에 들어간 것만으로는 완료할 수 없습니다. 전달이 시작되면 콜백이 대기 중인 경우에도 작업 항목과 협력하여 완료할 수 있습니다. 별도 내부 참조는 취소 콜백이 반환할 때까지 요청을 유지합니다. 완료 시 IRP는 먼저 무효화되며 마지막 요청 파괴 연속 실행 자체도 대기할 수 있습니다. 우선순위는 DPC, FIFO 순서의 취소 콜백, 일반 작업 항목 순이며, 대기 중이었다가 준비된 수동 수준 프레임의 재개보다도 취소 콜백을 먼저 전달합니다.

이미 취소된 요청에서 이전 void `WdfRequestMarkCancelable`은 반환 전에 게스트 취소 콜백을 동기 실행합니다. 이 자식 연속 실행은 대기하거나 중첩 정리로 요청을 완료하고 최종 파괴를 실행한 뒤 원래 API를 재개할 수 있습니다. 등록 후 취소는 위의 예약된 취소 경로를 사용합니다. 이는 `PASSIVE_LEVEL`과 `WdfSynchronizationScopeNone`에서 공개 소스의 동작을 재현하지만, [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable)는 자동 동기화를 사용하지 않는 드라이버에 Ex 사용을 요구합니다. 이전 API 실행은 호환 동작이며 이 설정에서의 사용을 권장하지 않습니다.

`WdfRequestGetInformation`과 `WdfRequestSetInformation`은 원래 IRP의 64비트 `IoStatus.Information`을 공유하며 게스트 직접 쓰기와도 일치합니다. Set은 값을 대입하고 전송 길이는 완료 시 검증합니다. `WdfRequestCompleteWithInformation`은 정리 전에 같은 필드에 기록합니다. GetInformation이 이 단계에서 이미 0을 반환하더라도 정리 콜백이 미리 저장한 IRP로 값을 바꾸면 최종 Information에 반영됩니다. `WdfRequestGetIoQueue`는 원래 큐를 반환합니다. 기본 파일 설정에서는 `WdfRequestGetFileObject`가 NULL을 반환하며 WDM FILE_OBJECT를 WDF 파일 객체로 가장하지 않습니다. `WdfRequestWdmGetIrp`는 동일한 IRP를 반환하지만 게스트 `IoCompleteRequest`/`IofCompleteRequest`로 WDF 완료를 우회할 수 없습니다. 완료 중이거나 완료 후에도 핸들이 유효하면 GetInformation/GetIoQueue는 0을 반환하고, MDL 검색은 유효한 출력 슬롯을 NULL로 비운 뒤 `STATUS_INTERNAL_ERROR`를 반환합니다. 이때 SetInformation/GetFileObject/WdmGetIrp는 거부됩니다. 기존 버퍼 및 매개변수 접근 제한은 유지됩니다.

`WdfRequestRetrieveInputWdmMdl`과 `WdfRequestRetrieveOutputWdmMdl`은 버퍼 방식 WRITE 입력, READ 출력, IOCTL 입력/출력의 기존 SystemBuffer를 필요할 때 기술합니다. 매번 방향이 유효하고 길이가 0이 아닌지 검사한 뒤 요청의 단일 캐시 설명자를 사용합니다. 첫 성공 검색이 ByteCount를 정하며 반대 방향의 논리 길이가 달라도 유지합니다. 이 설명자에서 `MmGetSystemAddressForMdlSafe`는 원래 VA를 반환하고 추가 매핑, 매핑 해제 및 드라이버의 해제는 거부됩니다. 직접 READ 출력, WRITE 입력 및 IOCTL 출력은 기존 `IRP.MdlAddress`를 반환하며 검색 자체가 매핑하지 않습니다. 직접 IOCTL 입력은 SystemBuffer 캐시를 사용합니다. 설명자, IRP와 버퍼는 완료 시 무효화됩니다. 취소 내부 참조나 외부 참조는 WDF 컨텍스트만 유지하며 완료된 I/O 저장 공간을 유지하지 않습니다. WDF `METHOD_NEITHER`는 지원하지 않습니다. 구성된 설명자의 모델 PFN 배열은 읽기 전용이며 미구성 설명자의 PFN 접근은 거부합니다.

버퍼/직접 KMDF 요청에서는 `WdfDeviceInitSetIoInCallerContextCallback`으로 등록한 콜백을 요청 프로세스에서 실행합니다. 요청을 완료하거나 `WdfDeviceEnqueueRequest`를 한 번 호출하여 순차 기본 큐로 보내야 합니다. `METHOD_NEITHER` 사용자 버퍼 검색·검사·잠금은 지원하지 않습니다.

모델링된 KMDF API: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetIoType`, `WdfDeviceInitSetIoInCallerContextCallback`, `WdfDeviceCreate`, `WdfDeviceEnqueueRequest`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfIoQueueGetDevice`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

선택적 실제 WDK 검증은 `driver_kmdf_lifecycle.c`와 `driver_kmdf_control.c`를 진짜 KMDF 진입 라이브러리로 각각 컴파일합니다. `NEVERD_KMDF_FIXTURE`/`NEVERD_KMDF_CFG_FIXTURE`는 수명 주기 이미지를, `NEVERD_KMDF_CONTROL_FIXTURE`/`NEVERD_KMDF_CONTROL_CFG_FIXTURE`는 일반/활성 CFG 제어 장치 이미지를 선택합니다. 외부 산출물이 없으면 명시적으로 건너뜁니다. 네이티브 및 C API/CLI 범위는 [테스트](testing.md)를 참조하세요. 현재 실행 증거는 Linux 호스트로 제한됩니다.

초기 API 모델에는 의도적으로 유한한 계약이 있습니다.

| API | 모델링된 동작 및 제한 |
|-----|----------------------|
| `RtlInitUnicodeString` | 길이가 제한된 NUL 종료 소스로 게스트 `UNICODE_STRING`을 구성함 |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | 길이가 지정된 UTF-16 복사와 대소문자를 구분하는 비교. 대소문자 무시 비교에는 Windows 대소문자 테이블이 필요하므로 중단함 |
| `ExRaiseStatus`, `ExRaiseAccessViolation`, `ExRaiseDatatypeMisalignment` | 지원하는 상수 C `__except` 처리기로 게스트 예외 발생. 정상 API 반환은 없으며 필터／finally와 CPU 오류 복구는 미지원 |
| `ExAllocatePool2` | Paged/nonpaged NX 할당이며 기본값은 0으로 초기화. uninitialized 및 cache-aligned 플래그를 모델링함. 잘못된 필수 플래그는 NULL을 반환하고, quota/executable 풀과 할당 예외 발생은 중단함 |
| `MmGetSystemRoutineAddress` | 공유 export 목록을 통해 길이가 지정된 게스트 이름을 해석함 |
| `MmMapIoSpace`, `MmMapIoSpaceEx`, `MmUnmapIoSpace` | 선언된 변환 후 하위 구간, 비캐시 RO／RW, 공유 별칭과 정확한 unmap. 임의 물리 메모리는 미지원 |
| `IoConnectInterrupt`, `IoDisconnectInterrupt`, `IoConnectInterruptEx`, `IoDisconnectInterruptEx` | 정확히 할당된 독점 latched 라인, PASSIVE_LEVEL의 기존 ABI와 Ex 버전 1／2／4, 불투명 연결과 정확한 세대 수명 |
| `KeSynchronizeExecution`, `KeAcquireInterruptSpinLock`, `KeReleaseInterruptSpinLock` | 실제 BOOLEAN 동기화 콜백과 할당 DIRQL의 동일 비재귀 잠금, 원래 호출자 IRQL 및 소유권 복원 |
| `IoGetDmaAdapter` | PASSIVE_LEVEL에서 명시적 Internal 버스 마스터 버전 0／1 설명 및 연결된 V1 작업 테이블. 새 버전 탐색은 NULL |
| `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `PutDmaAdapter` | 공유 일관성 RAM, 정확한 할당 식별자, 독립 어댑터 수명을 사용하는 테이블 메서드 |
| `GetScatterGatherList`, `PutScatterGatherList` | DISPATCH_LEVEL 테이블 메서드. 실제 인라인 또는 자원 대기 콜백, 고정된 MDL 뷰, 명시적 매핑 해제 |
| `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers`, `FreeMapRegisters` | 변환된 버스 마스터 채널 콜백, 공유 레지스터 할당량, 연속 MDL 조각, 전체 플러시와 정확한 보유 레지스터 해제 |
| `KeFlushIoBuffers` | 유효한 잠금／비페이지 MDL에 대한 일관성 CPU 캐시 플러시. DMA 매핑은 해제하지 않음 |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | 요청 소유 MDL의 캐시된 KernelMode 매핑과 권한. 비페이지 풀 MDL은 안전 도우미로 원래 풀 매핑을 재사용 |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | 독립 MDL의 전체 범위는 하나의 유효한 비페이지 풀 할당 내부여야 함. MDL과 버퍼 수명은 독립적이며 IRP 연결, 체인, 할당량은 미지원 |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | 명시적인 세션 레지스트리, 핸들별 권한과 수명, 쿼리 버퍼 크기와 변경. 호스트 레지스트리에 접근하지 않음 |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | 풀 유형 `0`, `1`, `512`의 데이터 할당. 크기/태그는 양수이며, 태그가 있는 해제는 일치해야 하고 주소를 재사용하지 않음 |
| `IoCreateDevice`, `IoDeleteDevice` | 장치 유형 `0x22`, 특성 `0` 또는 `0x100`, 제한된 확장, ASCII `\Device\Name` 이름 |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | 동일 드라이버 연결. 이전 최상단을 반환하며 분리는 저장한 하위 장치를 받음. 위 토폴로지/수명 제한 적용 |
| `IofCallDriver`, `IoCallDriver` | 보존 경로의 정확한 대상으로 디스패치. 게스트 커서 검증 및 하위 NTSTATUS 보존 |
| `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx` | 확장부의 정확한 소유자와 불투명32/120바이트, NULL/중복 Tag |
| `IoReleaseRemoveLockEx`, `IoReleaseRemoveLockAndWaitEx` | 일치하는 Tag 해제와 제공자 수신 후 재개 가능한 REMOVE 대기 |
| `PoCallDriver`, `PoStartNextPowerIrp` | 관리 전원 IRP 전달 및 추가 핸드셰이크 없는 Vista+ 검증 |
| `PoSetPowerState`, `PoRequestPowerIrp` | 독립 장치 알림 상태와 명시적 PDO FIFO의 실제 자식 요청. 범위는 위 설명 |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | 하나의 세션 네임스페이스에서 ASCII `\DosDevices\Name` 또는 `\??\Name`을 사용하며 `\Device\Name`을 가리킴 |
| `DbgPrint`, `DbgPrintEx` | 검증된 Win64 가변 인수 포맷팅, 최대 출력 512바이트. 모든 디버거 필터가 활성화됨 |
| `IoGetCurrentIrpStackLocation` | 활성 모델 IRP의 스택 위치를 반환함. 일반적인 컴파일된 WDM 매크로도 동일한 게스트 필드를 읽음 |
| `KeGetCurrentIrql` | 명시적인 상승과 복원을 포함한 현재 IRQL/CR8을 읽습니다. 디스패치와 작업 항목은 `PASSIVE_LEVEL`, DPC는 `DISPATCH_LEVEL`에서 시작합니다 |
| `KfRaiseIrql`, `KeLowerIrql` | 실제 x64 WDK IRQL 상승·하강 임포트입니다. 저장한 IRQL은 동일 실행에서 반환이나 중단 전에 LIFO 순서로 복원해야 합니다. CR8 읽기는 모든 변경을 반영하며 명령 단위 인터럽트 선점은 모델링하지 않습니다. |
| `KeInitializeSpinLock`, `KeAcquireSpinLockRaiseToDpc`, `KeReleaseSpinLock`, `KeAcquireSpinLockAtDpcLevel`, `KeReleaseSpinLockFromDpcLevel`, `KeTryToAcquireSpinLockAtDpcLevel` | CPU0의 상주 정렬 실행 스핀록입니다. 소유자, 획득·해제 쌍, IRQL 복원을 검사하며, 경합 중 차단 획득은 명시적으로 중단합니다. |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | 장치 소유의 불투명 작업 항목. `DelayedWorkQueue`만 지원하며 `PASSIVE_LEVEL`에서 장치와 컨텍스트를 콜백에 전달. 대기열에 있는 항목은 해제 불가 |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | 불투명 DPC, 게스트 콜백 인수 네 개, `DISPATCH_LEVEL`, 중복/제거 동작과 중요도; 대상 CPU0만 지원 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | 알림/동기화 타이머, 상대/절대 100 ns 기한, 밀리초 주기, 재설정/취소와 가상 시간 신호 조회 |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | 알림/동기화 이벤트의 서로 다른 신호 소비; `KeSetEvent`는 Increment=0, Wait=FALSE만 허용 |
| `KeInitializeSemaphore`, `KeReleaseSemaphore`, `KeReadStateSemaphore` | 양의 상한이 있는 상주 카운팅 세마포입니다. 성공한 대기는 1을 소비합니다. 해제는 Increment=0, Wait=FALSE만 허용하며 상한 초과 시 `STATUS_SEMAPHORE_LIMIT_EXCEEDED`를 발생시킵니다. |
| `KeInitializeMutex`, `KeReleaseMutex`, `KeReadStateMutex` | 상주 KMUTEX는 실행 프레임이 소유하며 재귀적으로 획득할 수 있습니다. KeReleaseMutex는 이전 부호 있는 신호 상태를 반환하고 소유자와 일치하는 DISPATCH_LEVEL 획득 상태를 요구하며 Wait=FALSE만 허용합니다. 보유 중 반환, 재초기화 및 저장 공간 해제를 금지합니다. 소유자가 아닌 해제는 `STATUS_MUTANT_NOT_OWNED`를 발생시킵니다. |
| `PsCreateSystemThread`, `PsTerminateSystemThread`, `ObReferenceObjectByHandle`, `ObfDereferenceObject`, `ZwClose` | PASSIVE_LEVEL에서 실행되는 제한된 시스템 프로세스 스레드. 핸들과 불투명 스레드 객체 참조의 수명은 독립적이다. PsTerminateSystemThread는 복귀하지 않고 종료하며 대기 가능한 객체를 신호 상태로 만든다. APC, 우선순위 및 타입 지정 객체 참조는 지원하지 않는다. |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeEnterGuardedRegion`, `KeLeaveGuardedRegion`, `KeAreApcsDisabled`, `KeAreAllApcsDisabled` | 스레드별 중첩 APC 비활성화 상태. 중요 영역과 보유한 KMUTEX는 일반 APC를, 보호 영역과 IRQL >= APC_LEVEL은 모든 APC를 비활성화한다. 시스템 스레드는 중요 영역 하나 안에서 시작한다. 짝이 없는 종료와 균형이 맞지 않는 복귀는 실패한다. APC 전달은 모델링하지 않는다. |
| `KeWaitForSingleObject` | 초기화된 이벤트, 타이머, 세마포 또는 뮤텍스 하나, 비경고 `KernelMode`, 사유 `Executive`; 0 폴링, 유한 상대/절대 또는 무한 대기; 0이 아닌/무한 대기는 IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | IRQL <= APC_LEVEL에서 비경고 `KernelMode` 상대/절대 지연; 가상 시간 진행 후 저장된 게스트 프레임 재개 |
| `IoMarkIrpPending` | 현재 유효한 IRP를 보류로 표시. WDM 매크로의 스택 제어 필드 쓰기도 지원. 디스패치는 `STATUS_PENDING`을 반환해야 함 |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT`로 완료 해제를 수행. 중단/재개를 지원하고 최종 경계에서만 IRP/MDL/버퍼를 폐기 |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | 호출당 최대 1 MiB의 제한된 게스트 버퍼 작업. 비중첩 복사 API는 겹치는 범위를 거부함 |

API IRQL 상한은 `KernelAPIIRQL.def`에 정의되며 인수별 제한은 담당 모델이 검사합니다. DPC는 레지스트리 API나 페이징 풀 할당·해제·접근을 사용할 수 없습니다. Unicode `DbgPrint` 변환은 `PASSIVE_LEVEL`이 필요하며 지원되는 ANSI 출력과 비페이징 작업은 `DISPATCH_LEVEL`에서 사용할 수 있습니다. 콜백 스택에는 경계가 있어 이탈한 스택 포인터가 다른 차단 작업자의 스택을 침범할 수 없습니다. 장치 확장의 활성 타이머는 조기 장치 회수를 막습니다.

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

루트에서는 `load_address`, `requests`, `unload`, `kernel_exports`, `registry`, `pnp_devices`만 허용됩니다. 일반 파일 요청은 `kind`, 선택적인 `device` 또는 `device_id`(서로 배타적)와 `file`을 받습니다. IOCTL에는 `code`가 필수이며 `input`, `output_size`, `direct_input`을 받을 수 있습니다. `read`는 `output_size`와 `byte_offset`, `write`는 `input`과 `byte_offset`을 받습니다. 오프셋 기본값은 0이며 정수 또는 16진 문자열을 받고, 음수가 아닌 부호 있는 64비트 값 범위에 들어야 합니다. 수명 주기 요청은 전송 필드를 거부합니다. 알 수 없거나 중복된 필드는 거부합니다. `code`는 부호 없는 32비트 JSON 정수 또는 `0x` 16진 문자열을 받습니다. `input`은 접두사나 공백이 없는 짝수 길이의 16진 바이트 문자열이며, 생략하면 빈 입력입니다. `output_size`는 부호 없는 JSON 정수이며 생략하면 0입니다. 소수와 부동소수점 표기는 거부합니다.

READ/WRITE/IOCTL 요청만 선택적 `cancel_after_100ns` 필드를 허용합니다. 값은 0부터 `INT64_MAX`(9223372036854775807)까지의 JSON 정수입니다. 실제 시간이 아니라 요청 제출 시점을 기준으로 한 가상 100 ns 단위로 취소를 예약합니다. KMDF에서 0은 프레임워크 라우팅 후 게스트 I/O 콜백 전에 적용하며, 라우팅이 이미 요청을 완료했다면 완료가 우선합니다. WDM에서는 디스패치 반환 후 0을 적용합니다. 양수 지연에서는 준비된 콜백이나 실행 프레임이 없을 때만 타이머, 대기 또는 취소 기한으로 시간을 진행합니다. 보류 중인 WDM IRP는 등록된 취소 루틴을 취소 스핀락을 잡은 `DISPATCH_LEVEL`에서 호출합니다. 완료 전에 `Irp->CancelIrql`로 잠금을 해제해야 합니다. 일반 큐와 PnP 취소는 여전히 지원하지 않습니다. 각 요청 보고서의 `cancel_requested_at_100ns`는 취소가 실제 발생한 절대 가상 시간이거나, 완료가 먼저 발생하는 경우를 포함해 취소가 발생하지 않았다면 null입니다. 취소 요청만으로 IRP가 완료되거나 최종 상태가 정해지지는 않습니다.
`IoSetCancelRoutine`, `IoAcquireCancelSpinLock`, `IoReleaseCancelSpinLock`, `IoCancelIrp`는 같은 IRP 상태와 취소 스핀락을 사용합니다. `IoCancelIrp`는 등록된 루틴을 동기적으로 호출하고 호출 여부를 반환합니다.

선택적 불리언 `user_unmap_after_dispatch`는 비어 있지 않은 WDM neither 전송에서 디스패치 반환 후, 예약된 작업 또는 취소 전에 원래 사용자 주소의 접근을 취소합니다. MDL로 잠근 페이지와 시스템 별칭은 잠금 해제 전까지 사용할 수 있지만 원시 사용자 포인터와 새 잠금은 실패합니다. 출력 주소가 취소되면 `output_hex`는 비어 있습니다. 주소 재사용과 임의 시점의 매핑 해제는 모델링하지 않습니다.

`requestor_process_id`는 합성 요청 프로세스 ID입니다(기본값 4096, 범위 5~`UINT32_MAX`). `IoGetRequestorProcessId`는 활성 IRP의 ID를 반환하고 `PsGetCurrentProcessId`는 직접 디스패치에서는 그 ID, 모델링된 시스템 작업 항목에서는 4를 반환합니다. 프로세스를 바꾸면 다른 프로세스의 원래 사용자 VA에는 접근할 수 없지만 잠긴 MDL의 시스템 별칭은 유효합니다. `requestor_exit_after_dispatch`는 디스패치 반환 뒤 그 프로세스의 원래 사용자 VA를 모두 철회하고 새 I/O를 거부합니다. 명시적인 CLEANUP/CLOSE는 허용되며 취소나 핸들 정리를 자동으로 추론하지 않습니다.

시스템 작업 항목은 `IoGetRequestorProcess`로 활성 IRP의 불투명한 요청 프로세스 객체를 얻고, 쓰기 가능한 커널 `KAPC_STATE`로 `KeStackAttachProcess`를 호출해 원래 사용자 VA에 잠시 접근한 뒤 동일한 상태로 `KeUnstackDetachProcess`를 호출할 수 있습니다. `IoGetCurrentProcess`와 `PsGetProcessId`는 연결된 프로세스를 나타내지만 `PsGetCurrentProcessId`는 작업 스레드를 만든 시스템 프로세스 ID 4를 유지합니다. 종료된 프로세스, 완료된 IRP, 일치하지 않는 상태, 연결 중 대기 또는 IRP 완료는 명시적으로 실패합니다.

Direct IOCTL에서 `input`은 첫 번째 시스템 버퍼를 초기화하고, `direct_input`은 MDL이 설명하는 별도의 두 번째 버퍼를 초기화하며 `output_size`까지 0으로 채웁니다. `METHOD_IN_DIRECT`는 읽기 접근을 요구하지만 읽기 전용 시스템 매핑을 뜻하지는 않습니다. 두 방식 모두 읽기/쓰기가 가능한 시나리오 버퍼를 사용합니다. `MdlMappingNoWrite`는 매핑의 쓰기 권한을, `MdlMappingNoExecute`는 실행 권한을 제거합니다. 매핑 해제는 시스템 VA를 무효화하며, 다시 매핑해도 같은 잠긴 데이터가 유지됩니다. 완료되면 MDL과 매핑의 수명이 끝납니다. WDM 매크로가 사용하는 공개 MDL 필드는 모델링하지만, 프로세스 필드, 미구성 설명자의 PFN, 직접 만든 MDL, 사용자 매핑 및 원시 UserBuffer를 통한 직접 접근은 거부합니다. 길이가 0인 direct 버퍼의 MDL은 null입니다.

`IoAllocateMdl`은 비어 있지 않고 주소가 넘치지 않는 최대 1 MiB 버퍼의 독립 메타데이터를 할당하며, 버퍼를 프로브하거나 잠그지 않습니다. `Irp`는 NULL, `SecondaryBuffer`와 `ChargeQuota`는 FALSE여야 합니다. 아레나가 고갈되면 NULL을 반환합니다. `MmBuildMdlForNonPagedPool`은 설명하는 전체 범위가 하나의 유효한 비페이지 풀 할당에 속해야 합니다. 안전 도우미와 일반 WDM 매크로는 원래 주소를 재사용하여 별칭과 기존 권한을 유지하며, 새로운 쓰기/실행 금지 플래그도 기존 권한을 바꾸지 않습니다. 추가 시스템 매핑과 매핑 해제는 거부합니다. `IoFreeMdl`은 MDL만 만료시키며 풀 버퍼의 수명은 독립적입니다. 해제한 저장소를 다시 사용하지 않으면 어느 해제 순서든 지원합니다. 모델의 MDL 필드와 구성된 PFN 배열은 읽기 전용이며 프로세스 필드, 미구성 PFN, MDL 체인과 수동 필드 변경은 지원하지 않습니다. 언로드 시 드라이버 소유 MDL을 모두 해제해야 합니다.

IOCTL의 `output_size`가 0이 아니면 입력 버퍼가 더 커도 `Information`은 해당 크기를 넘을 수 없습니다. 출력 버퍼가 없는 IOCTL은 드라이버 정의 결과를 반환할 수 있으며 출력 바이트를 복사하지 않습니다. `information_hex`는 원래 64비트 값을 정확히 보존합니다.

READ/WRITE에서는 `DO_BUFFERED_IO` 또는 `DO_DIRECT_IO`가 버퍼 방식이나 직접 전송을 선택합니다. 둘 다 설정하지 않으면 원래 사용자 주소가 `IRP.UserBuffer`에만 들어가며 WRITE는 입력, READ는 출력을 뜻합니다. SystemBuffer나 MDL은 자동으로 생성되지 않습니다. 드라이버는 호출자 컨텍스트에서 검사하고 접근하거나 작업을 미루기 전에 페이지를 잠가야 합니다. `user_input_access`는 neither WRITE, `user_output_access`는 neither READ에 적용됩니다. 버퍼/직접 READ/WRITE에 권한을 지정하거나 두 플래그를 함께 설정하면 중단됩니다. Information은 전송 길이에 대해 검사하며 쓰기는 개수, 읽기는 바이트를 반환합니다.

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

JSON 보고서는 `stop_reason`, null이 가능한 `nt_status`와 `nt_success`, 중단 PC, 명령어 수를 구분합니다. 장치 객체와 드라이버 콜백 주소를 포함하여 중단 전에 수집한 API 호출 및 관찰 가능한 상태를 보존합니다. JSON 소비자가 64비트 정밀도를 잃지 않도록 게스트 주소는 16진 문자열로 표현합니다. `configuration` 객체는 실행 한도, 서비스 이름과 `kernel_exports` 재정의를 기록합니다. 프로필은 `wdm-x64-scheduled-v32`입니다. `nt_status`는 계속 DriverEntry 결과를 나타내고, `scenario_success`는 초기화와 완료된 요청을 함께 나타냅니다. `phase`, `requests`, `unload_completed`는 요청한 수명 주기의 어느 부분이 실행되었는지 식별합니다. 각 API 호출과 CPU 쓰기에도 단계(`driver_entry`, `add_device:<ID>`, `request:N`, `callback:N`, `unload`)가 기록됩니다. 각 요청은 디스패치 및 I/O 상태, 완료 여부, 정보 길이와 반환된 `output_hex` 바이트를 보고합니다. `preferred_image_base`는 원래 PE 베이스를 나타냅니다. `security_cookie`는 초기화된 cookie의 게스트 주소이며, 필요하지 않았다면 `"0x0"`입니다. 요청 필드는 `kind`, `device`, `device_id`, `pnp`, `file`, `requestor_process_id`, `byte_offset`, `code`, `irp`, `completed`, `cancel_requested_at_100ns`, `dispatch_status`, `io_status`, `information`、`information_hex`, `output_hex`입니다. `configuration.registry`는 원래 레지스트리 구성을 보존합니다. `information_hex`는 원래 64비트 `IoStatus.Information`을 16진수 문자열로 정확히 보존합니다. 기존 숫자 필드 `information`도 유지합니다.

작업 항목 관찰에는 `callback:N` 단계가 기록됩니다. 보류 요청의 `dispatch_status`는 `STATUS_PENDING`을 유지하며 최종 완료 상태는 별도의 `io_status`에 기록되어 `scenario_success` 판정에 사용됩니다.

`ExRaiseStatus`는 NTSTATUS의 하위 32비트를 게스트 예외 처리기에 전달합니다. `ExRaiseAccessViolation`과 `ExRaiseDatatypeMisalignment`는 각각 `STATUS_ACCESS_VIOLATION`과 `STATUS_DATATYPE_MISALIGNMENT`를 발생시킵니다. 프로필은 개별 Microsoft DDI 문서를 따릅니다. ExRaiseStatus는 `APC_LEVEL`을 허용하고 인수가 없는 두 루틴은 `PASSIVE_LEVEL`을 요구합니다. 일부 WDK SAL 주석은 래퍼에 APC_LEVEL을 허용하지만 이 프로필은 문서의 더 엄격한 상한을 유지합니다. 예외를 발생시킨 호출은 `result: null`을 유지하고 `detail`에 코드를 기록하며 성공적인 API 반환으로 보고하지 않습니다.

예외 전달은 이미지에서 해석한 x64 버전 1 스택 해제 테이블과 `__C_specific_handler`의 상수 `EXCEPTION_EXECUTE_HANDLER` 범위를 사용합니다. 실제 게스트 처리기 본문을 실행하고 일반 헬퍼 프레임을 해제하며 저장된 비휘발성 범용 레지스터를 복원하고 현재 실행의 스택 경계를 보존합니다. `GetExceptionCode()`는 발생한 코드를 확인합니다. 처리기는 지원하는 바깥 범위로 다른 예외를 발생시킬 수 있습니다. 경로에서 필터 함수, `__finally`, GS／C++ 성격 처리기, 연결되거나 불완전한 메타데이터, 프롤로그 해제, XMM 복원 연산을 만나면 명시적으로 실패합니다. 잡히지 않은 API 예외는 `model_error`로 멈추며 CPU 메모리／인터럽트／잘못된 명령 오류는 계속 실행 종료를 뜻합니다.

직접 작성한 `driver_wdm_seh.c` 픽스처는 실제 WDK 헤더와 `/GS-`를 사용합니다. 일반 및 활성 CFG 이미지는 `NEVERD_WDM_SEH_FIXTURE`와 `NEVERD_WDM_SEH_CFG_FIXTURE`로 설정합니다. [driver-seh-scenario.json](../examples/driver-seh-scenario.json) 예제는 이미지를 재배치하고 DriverEntry에서 API 예외를 잡은 뒤 언로드합니다. 별도의 WDM METHOD_NEITHER 경로는 사용자 접근 검사, MDL 잠금 및 처리 가능한 메모리 오류를 지원합니다.

null이 가능한 `fault` 객체는 최초의 백엔드 오류를 보존합니다. `kind`, `pc`와 null이 가능한 `address`, `size`, `access`, `interrupt`는 매핑되지 않았거나 보호된 메모리, 잘못된 범위, 잘못된 명령어와 CPU 예외를 구분합니다. 주소는 16진 문자열, 크기와 인터럽트 벡터는 정수를 사용합니다. 관찰을 위한 읽기는 원래 오류를 대체할 수 없습니다. 오류가 발생한 백엔드는 재개할 수 없으며, 이 레코드로 이러한 백엔드 오류를 게스트 SEH에서 처리할 수는 없습니다.

`instructions`는 허용된 게스트 명령어 실행 시도 수입니다. 실행 정책이 거부한 명령어는 세지 않지만, 허용된 후 CPU에서 오류가 난 명령어는 셉니다. 합성 API 디스패치와 반환 센티널은 이 카운터를 증가시키지 않습니다.

각 `writes` 항목의 `semantics: "attempted_guest_write"`는 스택 밖의 CPU 쓰기 시도를 기록한다는 뜻입니다. 이후 오류가 발생하거나 예산으로 중단되는 시도도 포함됩니다. 쓰기 완료를 보장하지 않으며 API 모델이 수행한 쓰기는 포함하지 않습니다. 장치와 드라이버 객체 스냅샷은 실행이 중단될 때 관찰한 상태를 나타냅니다.

`neverd/sdk/NeverDCAPIEmulation.h`(또는 C API 통합 헤더)를 include하고 세션을 만든 뒤 `neverd_emulate_driver_json(session, path, options)`를 호출합니다. 비어 있지 않은 경로를 명시하면 일반 분석 API로 먼저 로드하지 않고 엄격한 실행 사전 검사로 바로 진입합니다. CLI는 이 경로를 사용합니다. 옵션에 `NULL`을 전달하면 기본값을 선택합니다. `neverd_driver_options_v1` 옵션을 명시할 때는 정확한 `struct_size`와 양수인 명령어, 메모리, 이벤트, 시간 예산이 필요합니다. 결과는 `neverd_free_string`으로 해제합니다.

대신 경로에 `NULL`을 전달하려면 로드된 세션이 필요하며, 해당 파일을 IR 분석 및 함수 제한 로딩과 독립적으로 다시 파싱합니다. 두 경로 모두 세션 이미지를 보존합니다. 호출이 끝날 때까지 입력 파일을 접근 가능한 상태로 유지하고 변경하지 마세요. 요청/설정 실패는 `NULL`을 반환하고 `neverd_last_error`를 설정합니다. 실행 중단은 JSON을 반환합니다. 비활성화된 빌드에도 API가 존재하며 기능을 활성화하는 방법을 알려 줍니다.

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)`는 동일한 v1 옵션 및 소유권 규칙을 사용하면서 엄격한 시나리오 입력을 추가합니다. NULL이 아닌 NUL 종료 JSON 문자열이 필요합니다. 기존 `neverd_emulate_driver_json` ABI는 변경되지 않으며 초기화만 수행합니다. C++ 파서 `driverOptionsFromScenarioJSON`은 `emulateDriver` 호출자에게 동일한 시나리오 검증을 제공합니다.

내부 C++ 진입점은 `include/neverd/emulation/DriverSession.h`의 `neverd::emulation::emulateDriver`입니다. 형식 파싱은 기존 로더, Windows 객체/API 동작은 `lib/emulation/windows`, CPU 상태와 실행은 Unicorn 어댑터가 담당합니다. 어댑터와 모델은 같은 게스트 메모리 인터페이스를 사용합니다. Windows API 동작을 Unicorn fork에 넣지 않습니다.

WDM `METHOD_NEITHER`에서 `Type3InputBuffer`와 `IRP.UserBuffer`는 별도의 사용자 할당을 가리킵니다. `ProbeForRead`는 페이지에 접근하지 않고 범위와 정렬을 검사하며, `ProbeForWrite`는 각 페이지에 접근합니다. `ExGetPreviousMode`는 요청 모드를 반환합니다. `MmProbeAndLockPages`는 단일 사용자 할당의 페이지를 잠그고, `MmGetSystemAddressForMdlSafe`는 공유 별칭을 제공하며, `MmUnlockPages`는 별칭과 잠금을 해제합니다. 임의의 프로세스는 모델링하지 않습니다.

비어 있지 않은 WDM `METHOD_NEITHER` IOCTL 요청에서는 `user_input_access`와 `user_output_access`를 각각 `read_write`(기본값), `read_only`, `no_access`로 설정할 수 있습니다. 버퍼/직접 방식이나 빈 버퍼에서는 거부합니다. `no_access`는 포인터를 유지하면서 페이지 접근을 막습니다.
보고서의 `configuration.user_page_access`에는 명시한 설정만 0부터 시작하는 `source_request_index`와 함께 기록됩니다. 생략한 방향은 `read_write`입니다.

## 제한된 WDM 요청 동시 실행

WDM READ/WRITE/IOCTL 요청에 `defer_callback_drain: true`를 지정할 수 있습니다. 디스패치가 `STATUS_PENDING`을 반환하고 IRP가 실제로 보류 중일 때만 콜백을 실행하기 전에 다음 요청을 제출합니다. 이 필드가 없는 다음 요청 후에는 콜백을 처리하고 배치를 마무리합니다. 마지막 요청에 필드가 있으면 시나리오 끝에서 처리합니다. 겹치는 요청은 서로 다른 파일 객체 또는 명시적으로 비동기 방식으로 연 동일 파일 객체를 사용할 수 있습니다. 동기 파일의 중첩, KMDF 배치 제출, 임의 선점 및 외부 요청 도착은 지원하지 않습니다.

## 비동기 파일 객체

CREATE 요청만 불리언 `asynchronous_file: true`를 지정할 수 있습니다. 생략하거나 false이면 동기 파일입니다. 비동기 파일은 게스트 `FILE_OBJECT`의 `FO_SYNCHRONOUS_IO`를 지우고 이후 파일 IRP에 `IRP_SYNCHRONOUS_API`를 설정하지 않습니다. 같은 비동기 파일의 READ/WRITE/IOCTL은 콜백 처리를 명시적으로 미룬 경우에만 겹칠 수 있습니다. CLEANUP/CLOSE는 앞선 모든 전송이 완료되고 정리될 때까지 기다립니다. 암묵적인 파일 위치는 유지하지 않으며 `byte_offset`은 요청별 사실로서 기본값은 0입니다. CREATE 이외의 요청에서는 false도 거부합니다.
