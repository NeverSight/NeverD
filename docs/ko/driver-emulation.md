**언어**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](../es/driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← 문서 인덱스](README.md)

# Windows 드라이버 에뮬레이션

NeverD의 선택적 드라이버 에뮬레이터는 지원되는 x64 WDM 드라이버의 PE 진입점을 실행하며, 필요하면 명시적인 동기 요청 시나리오를 실행한 뒤 드라이버를 언로드합니다. CPU 실행에는 Unicorn을, Windows 환경에는 NeverD 자체의 제한된 모델을 사용합니다. 드라이버를 호스트 커널에 로드하거나 게스트 API 호출을 호스트 OS 서비스에 전달하지 않습니다.

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

호환성은 `.sys` 확장자가 아니라 실제 실행된 코드 경로와 의존성에 따라 결정됩니다. 현재 검증 근거는 독자적으로 작성한 freestanding fixture와 Microsoft SIOCTL WDM 예제의 buffered 경로입니다. 임의의 타사 드라이버와의 호환성을 입증하지는 않습니다.

| 드라이버 종류 또는 요구 사항 | 현재 범위 | 부족한 환경 |
|-----------------------------|-----------|-------------|
| 아래 API를 사용하는 x64 소프트웨어 WDM 드라이버 | 제한된 초기화와 하나의 동기 파일 수명 주기 | 추가로 실행되는 각 API에 명확한 모델이 필요함 |
| `METHOD_BUFFERED` IOCTL | 명시적인 요청 시나리오에서 지원 | 여러 열린 파일과 비동기 완료는 지원하지 않음 |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT`, `METHOD_NEITHER` | 거부됨 | MDL, 잠긴 페이지, 접근 검사, 사용자 버퍼 수명 |
| KMDF / UMDF 드라이버 | 지원하지 않음 | 프레임워크 바인딩, 객체, 큐, 콜백과 해당 호스트 런타임 |
| PnP 버스/기능/필터 드라이버 | API 하위 집합 내에서 초기화가 실행될 수 있으나 장치 스택 수명 주기는 지원하지 않음 | 장치 연결, 하위 드라이버 디스패치, PnP 및 전원 IRP |
| 저장 장치, 네트워크, 디스플레이, 파일 시스템 및 미니필터 드라이버 | 서브시스템 계약을 지원하지 않음 | 포트/클래스/미니포트 프레임워크, NDIS/WFP, 그래픽 또는 파일 시스템 서비스 |
| 작업자 스레드, 타이머, DPC, APC, 대기 또는 취소를 사용하는 드라이버 | 지원하지 않음 | 스케줄링, IRQL 전환, 동기화와 비동기 소유권 |
| 프로세스/스레드 콜백, 핸들, 레지스트리/파일 작업 또는 커널 모듈 탐색을 사용하는 드라이버 | 아래 API 범위 밖에서는 지원하지 않음 | 객체 관리자, 시스템 상태와 콜백/이벤트 생성 주체 |
| 하드웨어, DMA, PCI, 인터럽트 또는 가상화 드라이버 | 환경을 지원하지 않음 | 장치 모델, 물리 메모리, 버스, 인터럽트와 특권 CPU 상태 |
| x86 또는 ARM64 Windows 드라이버 | 거부됨 | 아키텍처별 로딩, ABI와 실행 모델 |
| CFG, 지원되지 않는 로드 구성, TLS 또는 기타 거부되는 PE 기능이 필요한 x64 이미지 | 로드 시 거부됨 | 해당 요구 사항에 대한 명시적인 로더/런타임 의미론 |

사용되지 않는 미지원 import는 바인딩된 상태로 남아 있을 수 있습니다. 지원되지 않는 작업에 도달하면 진단과 그때까지 수집한 관찰 결과를 남기고 중단합니다. DriverEntry의 성공만으로 이후 디스패치, 하드웨어 또는 프레임워크 경로가 지원됨을 입증할 수는 없습니다. 아래 API 표가 지원 하위 집합의 기준입니다.

## 실행 계약

이 프로필은 `PASSIVE_LEVEL`에서 단일 스레드로 동작하는 하나의 x64 WDM 수명 주기를 모델링합니다. 실행은 PE 진입점에서 시작하며, 컴파일러의 진입 래퍼가 있으면 그대로 유지합니다. 초기화하려면 DriverEntry가 `STATUS_SUCCESS`를 반환해야 합니다. 0이 아닌 성공 상태나 pending 상태는 지원하지 않는 초기화 계약으로 중단됩니다. 실패 상태는 완료된 초기화 결과로 보존됩니다. 모든 객체, 문자열, 스택, 함수 포인터와 할당은 게스트 메모리에 존재합니다. 모델은 설정된 서비스 이름(기본값 `NeverDDriver`)에 대한 `DRIVER_OBJECT`와 레지스트리 경로를 제공합니다. 어댑터는 Windows 페이지 테이블을 합성하지 않고 Unicorn의 가상 TLB 모드로 정규 상위 커널 주소를 포함한 게스트 가상 주소를 보존합니다. 초기 RFLAGS는 `0x202`이며, 소프트웨어 장치 프로필은 고정된 64바이트 캐시 라인을 사용합니다. 이는 이 실행 시나리오의 명시적인 속성입니다.

알 수 없는 import는 지연 트랩에 바인딩됩니다. 사용되지 않는 import는 실행을 막지 않지만, 해당 thunk를 실행하거나 모델링되지 않은 export 데이터 값을 읽으면 `unsupported_api`로 중단됩니다. 지원하지 않는 CPU 환경 효과도 명시적으로 중단됩니다. NeverD는 미구현 호출을 성공 값으로 대체하지 않습니다. 잘못된 이미지나 지원하지 않는 로딩 요구 사항은 실행 전에 실패합니다.

이 프로필은 완전한 Windows 커널, KMDF 런타임, PnP/전원 수명 주기, 비동기 또는 pending IRP, direct/neither 방식 IOCTL, 인터럽트, 다중 스레드 스케줄링을 구현하지 않습니다. 콜백은 시나리오에서 명시적으로 요청한 경우에만 실행됩니다. 초기화만 요청하면 여전히 DriverEntry 이후에 멈춥니다.

시나리오에서 유효한 재배치 주소를 지정하지 않으면 이미지는 선호 베이스를 사용하며, native 서브시스템의 PE32+ x64 실행 파일이어야 합니다. import 제공자는 `ntoskrnl.exe` 또는 `ntkrnlmp.exe`일 수 있습니다. 실행 로더는 검증된 x64 `DIR64` 베이스 재배치와 제한적인 security-cookie 로드 구성을 지원합니다. security cookie는 진입 래퍼 실행 전에 결정적인 게스트 값으로 초기화됩니다. CFG와 기타 모델링되지 않은 로드 구성 필드, TLS, 지연/바인딩 import, ordinal import, managed 이미지는 거부됩니다. 이미지에는 엄격한 범위 및 정렬 검사도 적용됩니다.

초기 API 모델에는 의도적으로 유한한 계약이 있습니다.

| API | 모델링된 동작 및 제한 |
|-----|----------------------|
| `RtlInitUnicodeString` | 길이가 제한된 NUL 종료 소스로 게스트 `UNICODE_STRING`을 구성함 |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | 풀 유형 `0`, `1`, `512`의 데이터 할당. 크기/태그는 양수이며, 태그가 있는 해제는 일치해야 하고 주소를 재사용하지 않음 |
| `IoCreateDevice`, `IoDeleteDevice` | 장치 유형 `0x22`, 특성 `0` 또는 `0x100`, 제한된 확장, ASCII `\Device\Name` 이름 |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | 하나의 세션 네임스페이스에서 ASCII `\DosDevices\Name` 또는 `\??\Name`을 사용하며 `\Device\Name`을 가리킴 |
| `DbgPrint`, `DbgPrintEx` | ASCII 리터럴과 `%%`, 최대 출력 512바이트. 가변 인수 형식은 중단하며 모든 디버거 필터가 활성화됨 |
| `IoGetCurrentIrpStackLocation` | 활성 모델 IRP의 스택 위치를 반환함. 일반적인 컴파일된 WDM 매크로도 동일한 게스트 필드를 읽음 |
| `KeGetCurrentIrql` | `PASSIVE_LEVEL`을 반환함 |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT`로 활성 동기 모델 IRP를 완료함. 완료된 IRP나 버퍼에는 다시 접근할 수 없음 |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | 호출당 최대 1 MiB의 제한된 게스트 버퍼 작업. 비중첩 복사 API는 겹치는 범위를 거부함 |

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

장치 이름과 IOCTL 코드는 드라이버와 일치해야 합니다. `device`를 생략하면 유일한 활성 장치를 선택하며, 선택이 모호하면 실패합니다. 이 모델은 열린 파일 하나를 추적하며 create, IOCTL, cleanup, close 순서를 요구합니다. `METHOD_BUFFERED` IOCTL만 지원합니다. 디스패치는 각 IRP를 동기적으로 완료해야 합니다. `STATUS_PENDING` 반환, 미완료, 잘못된 출력 길이, 완료된 IRP 접근은 명시적으로 실패합니다. 언로드를 요청했다면 활성 장치, 심볼릭 링크, 풀 할당 또는 파일 객체가 남아서는 안 됩니다.

선택적인 루트 필드 `"load_address": "0x190000000"`는 베이스 재배치를 요청합니다. 생략하거나 `"0x0"`을 지정하면 선호 주소를 사용합니다. 이미지는 재배치 요구 사항을 충족해야 합니다. 기존 초기화 명령이나 C API에는 암묵적인 시나리오가 없습니다.

루트에서는 `load_address`, `requests`, `unload`만 허용됩니다. 요청 필드는 `kind`, 선택적인 `device`, 그리고 `ioctl`에만 필요한 필수 `code` 및 선택적 `input`, `output_size`입니다. 알 수 없거나 중복된 필드는 거부합니다. `code`는 부호 없는 32비트 JSON 정수 또는 `0x` 16진 문자열을 받습니다. `input`은 접두사나 공백이 없는 짝수 길이의 16진 바이트 문자열이며, 생략하면 빈 입력입니다. `output_size`는 부호 없는 JSON 정수이며 생략하면 0입니다. 소수와 부동소수점 표기는 거부합니다.

시나리오 텍스트는 2 MiB, 요청은 최대 64개, 각 입력 또는 출력 버퍼는 최대 65536바이트, 입력과 출력 바이트 합계는 최대 512 KiB입니다. 명령어, 관찰, 게스트 메모리 및 시간 예산은 전체 시나리오에 적용됩니다. 1 MiB arena에는 객체와 메타데이터도 저장되므로 최대 시나리오 버퍼 용량을 사용하기 전에 모델 메모리가 고갈될 수 있습니다.

## Microsoft 예제 검증

선택적으로 실행하는 [검증 스크립트](../../scripts/validate_windows_driver_sample.py)는 [검증 매니페스트](../../unittests/emulation/fixtures/sioctl-validation.json)에 고정된 리비전의 Microsoft SIOCTL 소스를 다운로드하고 SHA-256 해시를 검증한 뒤, 수정하지 않은 소스를 MinGW-w64 DDK 헤더로 컴파일합니다. 선택한 출력 디렉터리에 업스트림 라이선스/출처, 빌드 명령, 시나리오 및 보고서를 보존합니다. 네트워크 접근, Clang, `lld-link`, `nm`, MinGW-w64 DDK 헤더가 필요합니다.

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

MinGW-w64 include 디렉터리가 기본 위치가 아니면 `--headers`를 사용합니다. 스크립트는 컴파일된 객체의 의존성으로 MS COFF import 라이브러리를 생성합니다. 검증은 DriverEntry, create, 예제의 buffered IOCTL, cleanup, close 및 unload를 실행합니다. 업스트림 예제는 cleanup 핸들러를 등록하지 않으므로, 모델의 기본 핸들러는 cleanup을 `STATUS_INVALID_DEVICE_REQUEST`(`0xC0000010`)로 완료합니다. 드라이버는 계속 close와 unload를 수행하며, 성공한 IOCTL은 예상 바이트를 반환합니다. 이 전체 시나리오에서 CLI의 예상 종료 코드는 **2**이고 `scenario_success`는 false입니다. 스크립트 자체는 눈에 보이는 cleanup 실패까지 포함해 모든 결과가 예상과 일치할 때만 성공합니다. 이 결과를 숨기려고 예제를 수정하지 않습니다.

## 보고서 및 SDK

JSON 보고서는 `stop_reason`, null이 가능한 `nt_status`와 `nt_success`, 중단 PC, 명령어 수를 구분합니다. 장치 객체와 드라이버 콜백 주소를 포함하여 중단 전에 수집한 API 호출 및 관찰 가능한 상태를 보존합니다. JSON 소비자가 64비트 정밀도를 잃지 않도록 게스트 주소는 16진 문자열로 표현합니다. `configuration` 객체는 실행 한도와 서비스 이름을 기록합니다. 프로필은 `wdm-x64-synchronous-v1`입니다. `nt_status`는 계속 DriverEntry 결과를 나타내고, `scenario_success`는 초기화와 완료된 요청을 함께 나타냅니다. `phase`, `requests`, `unload_completed`는 요청한 수명 주기의 어느 부분이 실행되었는지 식별합니다. 각 API 호출과 CPU 쓰기에도 단계(`driver_entry`, `request:N`, `unload`)가 기록됩니다. 각 요청은 디스패치 및 I/O 상태, 완료 여부, 정보 길이와 반환된 `output_hex` 바이트를 보고합니다. `preferred_image_base`는 원래 PE 베이스를 나타냅니다. `security_cookie`는 초기화된 cookie의 게스트 주소이며, 필요하지 않았다면 `"0x0"`입니다. 요청 필드는 `kind`, `device`, `code`, `irp`, `completed`, `dispatch_status`, `io_status`, `information`, `output_hex`입니다.

`instructions`는 허용된 게스트 명령어 실행 시도 수입니다. 실행 정책이 거부한 명령어는 세지 않지만, 허용된 후 CPU에서 오류가 난 명령어는 셉니다. 합성 API 디스패치와 반환 센티널은 이 카운터를 증가시키지 않습니다.

각 `writes` 항목의 `semantics: "attempted_guest_write"`는 스택 밖의 CPU 쓰기 시도를 기록한다는 뜻입니다. 이후 오류가 발생하거나 예산으로 중단되는 시도도 포함됩니다. 쓰기 완료를 보장하지 않으며 API 모델이 수행한 쓰기는 포함하지 않습니다. 장치와 드라이버 객체 스냅샷은 실행이 중단될 때 관찰한 상태를 나타냅니다.

`neverd/sdk/NeverDCAPIEmulation.h`(또는 C API 통합 헤더)를 include하고 세션을 만든 뒤 `neverd_emulate_driver_json(session, path, options)`를 호출합니다. 비어 있지 않은 경로를 명시하면 일반 분석 API로 먼저 로드하지 않고 엄격한 실행 사전 검사로 바로 진입합니다. CLI는 이 경로를 사용합니다. 옵션에 `NULL`을 전달하면 기본값을 선택합니다. `neverd_driver_options_v1` 옵션을 명시할 때는 정확한 `struct_size`와 양수인 명령어, 메모리, 이벤트, 시간 예산이 필요합니다. 결과는 `neverd_free_string`으로 해제합니다.

대신 경로에 `NULL`을 전달하려면 로드된 세션이 필요하며, 해당 파일을 IR 분석 및 함수 제한 로딩과 독립적으로 다시 파싱합니다. 두 경로 모두 세션 이미지를 보존합니다. 호출이 끝날 때까지 입력 파일을 접근 가능한 상태로 유지하고 변경하지 마세요. 요청/설정 실패는 `NULL`을 반환하고 `neverd_last_error`를 설정합니다. 실행 중단은 JSON을 반환합니다. 비활성화된 빌드에도 API가 존재하며 기능을 활성화하는 방법을 알려 줍니다.

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)`는 동일한 v1 옵션 및 소유권 규칙을 사용하면서 엄격한 시나리오 입력을 추가합니다. NULL이 아닌 NUL 종료 JSON 문자열이 필요합니다. 기존 `neverd_emulate_driver_json` ABI는 변경되지 않으며 초기화만 수행합니다. C++ 파서 `driverOptionsFromScenarioJSON`은 `emulateDriver` 호출자에게 동일한 시나리오 검증을 제공합니다.

내부 C++ 진입점은 `include/neverd/emulation/DriverSession.h`의 `neverd::emulation::emulateDriver`입니다. 형식 파싱은 기존 로더, Windows 객체/API 동작은 `lib/emulation/windows`, CPU 상태와 실행은 Unicorn 어댑터가 담당합니다. 어댑터와 모델은 같은 게스트 메모리 인터페이스를 사용합니다. Windows API 동작을 Unicorn fork에 넣지 않습니다.
