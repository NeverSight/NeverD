**언어**: [English](../architecture.md) | [简体中文](../zh-CN/architecture.md) | [繁體中文](../zh-TW/architecture.md) | [日本語](../ja/architecture.md) | [한국어](architecture.md) | [Français](../fr/architecture.md) | [Deutsch](../de/architecture.md) | [Español](../es/architecture.md) | [Italiano](../it/architecture.md) | [Русский](../ru/architecture.md) | [العربية](../ar/architecture.md)

[← 문서 인덱스](README.md)

# NeverD 아키텍처

이 가이드는 기여자가 NeverD를 안전하게 변경하는 데 필요한 프로덕션 경계를
설명합니다. 의도적으로 NeverD 소유 코드만 다루며 LLVM, Capstone, Unicorn
서브모듈은 각자의 내부 아키텍처를 유지합니다.

## 시스템 경계

```mermaid
flowchart LR
  CLI["tools/neverd CLI"] --> CAPI["libneverd C API"]
  SDKUser["SDK user or plugin"] --> CAPI
  CAPI --> Session["sdk::Session"]
  Session --> Loader["format loader"]
  Loader --> Image["BinaryImage"]
  Image --> Pipeline["Pipeline"]
  Pipeline --> Low["LowIR"]
  Low --> Med["MedIR"]
  Med --> High["HighIR"]
  High --> HighC["structured C"]
  Med --> LLVM["LLVM IR"]
  LLVM --> LLVMOut["LLVM IR or LLVM-derived C"]
  LLVM --> Codegen["target codegen"]
  Codegen --> Rewriter["PE / ELF / Mach-O rewriter"]
  Rewriter --> Patched["patched binary"]
```

NeverD에는 네 가지 IR 표현이 있지만 반드시 네 단계를 모두 거치는 단일 순서는
아닙니다. `LowIR -> MedIR`은 공통입니다. 구조화 디컴파일은
`MedIR -> HighIR -> C`를 사용하고, `lift`, `decompile --llvm`, `patch`는
`MedIR -> LLVM IR`로 직접 이동합니다. 특히 patch와 lift 모드는 의도적으로
HighIR을 건너뜁니다.

CLI는 `tools/neverd`에서 명령을 파싱하고 `neverd_session_t`를 만든 뒤
`include/neverd/sdk/NeverDCAPI.h`의 공개 API를 호출합니다. 엔진 상태는
`lib/sdk/SessionImpl.h`에 있습니다. `neverd_session_load`가 loader를 선택해
`BinaryImage`를 만들고, IR 기반 작업은 필요할 때 `lib/pipeline/Pipeline.cpp`를
실행합니다. `neverd` 실행 파일은 `neverd_shared`에 링크되며, 구성 요소 archive와
LLVM/Capstone 의존성은 공유 라이브러리의 비공개 구현 세부 사항입니다. CLI는 명령줄
UI에 LLVM Support를 사용하지만 엔진을 구동할 때 C API를 우회하지 않습니다.

HighIR의 `HighSourceFlow`는 출력 문장의 제어 흐름 간선, 지역 변수 식별, 확정 대입 분석을 담당합니다.
소스 검증과 불필요한 PHI 복사 제거는 같은 그래프를 사용합니다. 반복되는 스칼라 조건을 제한된
영/비영 상태로 나누며, 쓰기는 기존 조건을 무효화합니다. 주소가 외부로 전달된 변수는 미지로 두고
분할 한도에 도달하면 보수적인 그래프로 돌아갑니다. 모든 가능한 문맥에서 값이 사용되지 않을 때만
PHI 복사를 제거합니다. 호출, 읽기, 쓰기의 관찰 가능한 동작과 소스 레이블은 유지합니다.

Block 소비자의 이스케이프 분석도 이 그래프를 사용합니다. 제한된 고정점 분석은 분기와 루프에서 포인터 식별 정보와 전용 스택 슬롯을 추적합니다. 합류 지점은 가능한 컨텍스트 주소를 보존하며 완전한 덮어쓰기만 이를 지웁니다. 알 수 없는 간선, 예외 흐름, 증명 예산 초과는 바인딩을 거부합니다.

Objective-C 수신자 정보는 메서드 진입점의 self와 정확한 클래스 참조를 구분합니다. 진입점을 공유하는 모든 메타데이터가 일치해야 self 정보를 설정합니다. 전체 폭 복사와 ABI가 보존하는 레지스터는 진입점 역방향 간선을 포함한 동일한 고정점 분석으로 전달됩니다. 선언 검증은 클래스/인스턴스 구분, 기록된 카테고리, 상위 클래스와 채택 프로토콜을 따르며 self에는 알려진 하위 클래스 선언도 포함합니다. 컴파일러 카탈로그는 소유자와 상속 정보를 전역 선택자 검증과 별도로 유지합니다. SDK는 소스를 내보내기 전에 현재 이미지에서 수신자의 출처와 선언을 다시 검증합니다. 이 정보는 특정 IMP를 선택하거나 바이너리 재작성을 허용하지 않습니다.
외부 상속 정보가 없으면 수신자로 범위를 좁히지 않고 전역 선택자 일치를 요구합니다. 명시적으로 미지원이거나 충돌하는 선언은 계속 거부 근거로 유지합니다.

객체 타입이 명시된 ivar는 최대 여덟 번의 전체 폭 로드로 수신자 증명을 확장합니다. 각 단계는 런타임 오프셋 슬롯, 폭, 명령이 사용한 상수 바이트 오프셋을 기록합니다. 상수는 현재 레이아웃과 일치해야 하며 런타임 참조는 이동한 필드를 따를 수 있습니다. 로더는 기록된 클래스 상속과 필드 선언을 검증합니다. 부분 접근, 타입명 없는 id, 블록, 프로토콜만 있는 타입, 모호한 저장소, 알 수 없는 기준 포인터는 클래스 정보를 제공하지 않습니다. 소스 검증은 현재 이미지에서 전체 경로를 다시 확인합니다. 타입 정보는 객체 동일성이나 메모리 연산 제거 권한을 뜻하지 않습니다.

로더는 Darwin 상수 문자열, 정수 객체, 배열과 정렬된 사전으로 이루어진 유한 비순환 그래프를 검증합니다. 컨테이너 필드와 각 간선에는 불변 매핑 저장소와 모호하지 않은 임포트 또는 재배치 증거가 필요하며, 미지원 인코딩, 순환, 불완전한 그래프는 명시적으로 실패합니다. 소스 바인딩은 그래프와 입력 포인터 슬롯을 다시 검증합니다. 생성된 도우미는 정수 비트, 자식 순서와 공유 주소를 보존하고 기존 문자열 정체성을 재사용합니다. 컨테이너 슬롯은 검증된 자식 도우미만 호출하여 acquire/release 방식으로 한 번 초기화하고 공개합니다. 각 도우미는 자식 선언을 포함하므로 독립적으로 복원한 메서드가 하나의 정의를 공유할 수 있습니다. 이식 가능한 테스트는 손상된 입력과 증명 예산을 다루며, 네이티브 컴파일러 픽스처는 원본 메서드와 내용, 별칭, 복사 정체성, 동시 초기화를 비교합니다.

## IR 표현 및 경로

실험적 [인터프리터 복원 단계](interpreter-recovery.md)는 공통 MedIR 경계 이전에 엄격하게 리프트한 LowIR을 특수화합니다. provider는 불변 이미지의 근거를, `SymExec`는 명령어 의미론을 담당하며 잔여 CFG는 일반 SSA와 소스 백엔드를 재사용합니다. 복원 근거는 네이티브 명령어 인스턴스 및 바이너리 patch 증명 정보와 별도로 유지합니다.

`InterpreterSpecialization`은 실패한 시도 이후의 유한한 역방향 비트 요구 전파를 담당합니다. 스칼라 평가기를 재사용하며 그래프의 사실을 바꾸거나 제어 필드 및 컨텍스트를 추가하지 않습니다. 모든 작업에는 예산이 적용되고 게시에는 새로운 완전한 증명이 필요합니다.

후속 메모리 요구가 바이트 조각만 사용하면 문맥 정밀화는 이미 추적 중인 8바이트 주소 저장 영역을 추가 후보로 지정할 수 있습니다. 기존의 좁은 생성자 좌표가 기준이며, 입증된 상수나 프레임 오프셋으로 키를 만드는 책임은 큐 삽입 로직에만 있습니다.

유한 값 열거는 증명 쿼리를 변경하지 않고 실행 가능한 튜플을 관찰할 수 있습니다. 관찰자가 거부하면 튜플이 없는 불완전한 결과를 반환합니다. 캐시는 수학적 도메인 증명만 유지하며, 불변 읽기 인증서는 열거가 완료될 때까지 로컬에 보관합니다.

`NeverDLoader`가 `PEFixedImageView`를 소유하며 완전한 베이스 재배치 파서를 일반 PE 로드와 공유합니다. 바이너리 인터프리터 어댑터는 인증된 기본 베이스 뷰를 복구와 네이티브 증명에 사용하며 PE 테이블을 직접 파싱하지 않습니다. 준비 단계에서 가져오기 쓰기 범위, 매핑 식별 정보와 완전한 원시 필드를 검증한 뒤 바이트를 인증합니다. 뷰는 변경되지 않는 이미지를 빌려 쓰며 ASLR이나 초기화의 동등성을 주장하지 않습니다.

`FrameOffsets`는 예산이 적용된 진입 상대 변위의 단일 값 증명을 담당합니다. 복구기는 잔여 주소 표현식을 바꾸지 않고 실제 기호 메모리 접근을 정규화하며, 네이티브 검사는 두 실행의 주소 동일성을 유지합니다. 모든 정렬 분기와 공유 재시도 예산은 복구기가 관리합니다. native/LLVM 분할 증명 집계는 별도의 미완료 작업입니다. `NativeStackControl`은 내부 부호 없는 16비트 반환 정리를 담당하고 바이너리 제공기는 8바이트를 팝하는 정규 인코딩을 인증합니다.

일반 의존성 탐색이 더 진행되지 않으면 함수 진입점을 제외한 네이티브 위치와 명령 모드마다 기존 레지스터 컨텍스트 필드 하나를 분할할 수 있습니다. 진입 도메인은 완전하고 여러 값을 포함하며 선언된 모든 비트를 포괄해야 합니다. 실제 선행 간선마다 현재의 완전한 도메인을 독립적으로 증명하고 살아 있는 물리 필드를 비교해 각 경우를 다시 투영합니다. 후속 선행 간선과 확장된 노드도 다시 검사하며 지정된 진입점에서는 연결을 멈춥니다. 비교는 합성 출처를 가지며 노드, 연산, 컨텍스트, 질의, 정밀화 예산을 공유합니다. 부분 마스크, 미정의 플래그, 프레임 전용 필드는 제외합니다. 게스트 읽기나 호출자 가정을 추가하지 않습니다. 불완전한 도메인은 보수적인 간선을 유지하며 도달 가능한 모든 경우가 완료되어야 결과를 게시합니다.

제어 및 가드 정밀도 개선을 선택적 프레임 분할 재시도보다 우선하며 필요한 추가 분할은 유지합니다. 나머지별 고정점을 순서대로 계산하지만 모든 허용 나머지가 완료되어야 결과를 공개합니다. 명시적 정렬 영역과 분할 영역의 교집합을 사용하고 실제 나머지를 비교합니다. 컨텍스트, 연산, 노드 및 솔버 예산은 재시도 간 공유하며 한도를 유지합니다. 머신 상태 복원은 `--vm-entry-alignment=A:R`로 진입 RSP 합동 조건을 명시하고 실행 시 검사합니다. `A`는 양의 2의 거듭제곱이며 `R < A`여야 합니다. 다른 진입 값은 게스트 메모리 접근이나 상태 쓰기 전에 상태 2를 반환합니다. 주소 상위 비트는 자유롭고 기본값은 정렬을 가정하지 않습니다. 이 옵션은 네이티브 동등성 인증을 제공하지 않습니다.

공유 `SymContext::constantWindow`는 결과의 모든 비트가 상수임을 증명한 경우에만 비트 연산 구간의 값을 반환합니다. 기존 추출, 단일 연결 피연산자 안에 완전히 포함된 구간, 0 확장과 비트 연산을 추적하며 알 수 없는 XOR 입력과 산술 올림은 추측하지 않습니다. 결과는 64비트, 재귀는 32단계, 작업량은 256단위로 제한됩니다. 노드 방문, 깊이 제한에 따른 거부, 연결 피연산자 검사마다 비용을 계산하고 상수 복사에는 원본 전체의 64비트 워드 수도 부과합니다. 증명이 불완전하면 상수를 반환하지 않습니다. `SymState`는 이 읽기 전용 질의로 레지스터와 임시 공간의 상수 바이트를 내보내며, 저장된 표현식과 전체 워드 재구성은 원래의 표현식 정체성을 유지합니다. 메모리 영역은 리터럴 상수 바이트만 내보냅니다. 도출한 부분 메모리 정보를 미리 추가하면 후속 입력의 출처가 조각나고 제어 상태 탐색 작업량이 늘어날 수 있습니다.

`SymState`가 명시적 분리 계약 아래 각 STORE의 기존 바이트 보존을 담당합니다. 실제 STORE는 실행되며 다른 메모리 영역, 무효화 세대 및 미지 값의 기본 상태는 쓰기 이후 상태를 유지합니다. 누락된 바이트를 읽거나 초기화하지 않습니다. `SymExec`는 일반 STORE 한 번에만 계약을 적용합니다. 복원 계층은 범위 내 완전한 아핀 슬롯과 출처 사실을 별도로 유지하고 합류는 보수적으로 처리합니다.

`StringTransfer`는 제한된 순차 스칼라 변환을 담당합니다. 복구 계층은 값 증명, 공유 예산 및 완전히 복사된 프레임 포인터의 재인증을 담당하며, 생성된 접근은 기존 메모리 검사를 재사용합니다.

제어 의존성 순회는 분석이 완전히 끝난 경우에만 루트의 비트 의존성을 보고합니다. 증명된 상대 주소에서 상위 비트가 최소 32개 자유롭게 남아 있으면 복원은 선택적인 이미지 주소 열거를 생략할 수 있습니다. 이는 도달 가능성을 증명하지 않으며 메모리 접근도 제거하지 않습니다.

전체 폭의 아핀 제어 값 투영에도 같은 루트 의존성 분석을 사용합니다. 분석 결과는 하나의 간선 조건에만 유효하며, 너무 큰 값 집합은 수학적 증명 캐시에 들어가지 않는 불완전한 거부 결과를 만듭니다. 좁은 마스크로 재시도합니다. 기존 실행 가능성 처리는 독립적으로 유지되며, 값 집합의 거부만으로는 간선의 도달 가능성이나 불가능성을 증명할 수 없습니다.

유한 질의 캐시는 직렬화된 키, 수치 결과, 사용 순서 메타데이터를 하나의 저장 한도에 계산합니다. 기록을 제거하기 전에 후보 전체를 검증하고 단독으로 수용 가능한지 확인합니다. 키는 안정적인 맵 노드가 소유합니다. 적중은 사용 순서를 갱신하고 제거 후에도 유효한 독립 소유 결과 사본을 반환합니다. 캐시 객체는 복사하거나 이동할 수 없습니다. 교체는 증명 재사용만 바꾸며 질의 의미나 결과 수용 조건은 바꾸지 않습니다.

`InterpreterSpecialization`은 결합 제어 관계와 독립적인 유한 필드 영역을 모두 관리합니다. 간선 투영은 완전한 열별 증명만 기록하며, 합류는 마스크의 교집합과 다시 마스크를 적용한 값의 합집합을 구합니다. 노드를 재구성할 때 동일한 심볼릭 상태를 초기화한 뒤 영역 제약과 결합 술어를 논리곱으로 연결하여 프레임 정체성과 제한되지 않은 비트를 보존합니다. 모든 결합 튜플이 포함된다는 정확한 검사로 함의가 확인된 소속 제약만 생략할 수 있습니다. 이 정밀도 정책은 컨텍스트 키나 네이티브 반환 검증을 바꾸지 않습니다.

`FrameEntryConstraints.h`는 복구와 관계 증명이 공유하는 주소 비순환 조건을 관리합니다. `InterpreterSpecialization`은 한도 내 유일 전이 연결과 확정된 순서의 재생을 담당합니다. 이 C++ 옵션은 기본적으로 꺼져 있으며 증명 계약 비교와 해시 결합은 바이너리 어댑터가 담당합니다.

`modelInterpreterMachineStateX64`와 소스 래퍼는 게스트 레지스터 부분 영역, 패킹된 플래그, 프로파일 상태, 제어 흐름을 위한 생성기를 공유합니다. 모델은 상태 객체 접근만 명시적 레지스터 바이트로 바꾸며 상태와 게스트 RAX를 분리합니다. 컴파일러 의미론이나 증명 정책을 소유하지 않으며 진입 영역, 관찰 항목, 프레임 계약과 완전한 정제 검사는 호출자가 담당합니다.

`NeverDLLVMInterpreterModel`은 동일한 원시 상태 ABI로 별도의 유계 스칼라 LLVM 가져오기를 담당합니다. `modelLLVMInterpreterMachineStateX64`는 실제 상태 반환과 명시적 정의성 검사를 유지합니다. `llvmInterpreterMachineStateContract`는 전체 관찰값과 0인 감시 바이트의 보존 의무를 제공하며 도메인, 메모리, 완전한 증명은 호출자 책임입니다. 일반 리프팅이나 소스 게시를 변경하거나 컴파일러를 증명하지 않습니다.

LLVM 모델이 `initializes` 매개변수 계약의 검증을 담당합니다. 상태 포인터 투영을 재사용하고 일반 스칼라 생성 전에 예산이 제한된 바이트 단위 필수 데이터 흐름 분석을 수행하며, 별도의 값 평가기를 추가하지 않습니다.

`NeverDInterpreterLLVMRefinement`는 네이티브에서 LLVM으로의 증명 조합을 담당합니다. 두 상태 모델과 필수 계약을 재구성하고 권위 있는 프로필에서 진입 전용 플래그 투영을 생성하여 두 전제를 새로 검사합니다. 호출자는 루프 후보를 제시할 수 있지만 모델, 관찰 항목, 증명 기록을 바꿀 수 없습니다. 분석 모델은 실행 그래프와 선언된 루트만 복사하며, 상태 초기화가 반복되는 진입 역방향 간선을 거부합니다.

복구 C API v3와 CLI는 필드, 정제, 솔버 질의 예산을 공통 특수화기에 전달합니다. 어댑터는 확장을 읽기 전에 구조체 크기와 reserved 필드를 검사하며 v1/v2 레이아웃과 기본값을 유지합니다. 예산 증가는 허용 작업량만 바꾸며 실행 계약이나 결과 게시 조건은 바꾸지 않습니다.

복원은 `--vm-chain-transfers=N`(기본값 0)과 `--vm-no-control-discovery`도 제공합니다. 연결은 단일 대상이 증명된 제어 전송 사이의 기호 상관관계를 보존하며 한도에 도달하면 일반 CFG 경계로 돌아갑니다. 기계 상태 복원은 `--vm-entry-frame=begin:end`로 실행 시 검사하지 않는 비래핑 진입 RSP 오프셋 범위를 선언할 수 있습니다. 정확한 숫자 전제는 생성 C와 보고서에 남으며 메모리 접근 권한이나 동등성 증명을 제공하지 않습니다.

SSA 구성 한도를 넘는 큰 복원 함수는 `--llvm`을 통해 제한된 스칼라 가변 저장소 계약을 사용할 수 있습니다. 진입 입력, 루프에서 전달되는 값, 앞선 읽기의 의미를 보존합니다. 지원하지 않는 암시적 상태, 벡터 레지스터 매개변수, 이미지 재배치, 모호한 저장소와 잘못된 제어 흐름은 명시적으로 실패하며 HighC는 이 대체 경로를 거부합니다. 소스 출력은 기존 기계 상태 계약을 따르며 동등성 인증서를 추가하지 않습니다.

`analyzeMedMutableSource`는 정규화된 CFG, 저장소 식별과 보수적인 진입 바이트 요구 사항을 전담합니다. 이 요구 사항은 읽기의 상한이며 관측 가능성을 입증하는 증거가 아닙니다. LLVM은 모듈 순회 전에 검증하고 계획을 재사용해 진입 값을 한 번만 초기화합니다. 블록 수와 값 수의 곱, 전파 작업량에는 별도 한도가 있습니다. C 전달은 각 읽기에 대해 형식이 일치하고 부분 별칭이 없는 정확한 선행 쓰기를 사용하며 블록 간 합류에는 명시적 저장소를 유지합니다.

검증된 가변 변수 함수 본문에서 LLVM은 명령을 끝에 추가하는 동일 기본 블록 안에서만 비공개 슬롯의 정확한 값을 전달하고 마지막 쓰기를 유지합니다. 진입 초기화와 블록·함수·모듈 경계는 분리되며 대상 프로그램의 메모리 접근은 명시적으로 남습니다. C 출력은 스칼라 식 확장과 즉시값 접기의 작업량을 제한하고 이름이 있는 중간값을 유지합니다. 접을 수 있는 상수 식에는 LLVM 대상 배치를 사용하고 지원하지 않는 식은 명시적으로 거부합니다. 지정된 가변 반환값은 0이더라도 void 반환으로 바뀌지 않습니다.

가변 부분 집합은 8/16/32/64/128비트 스칼라 저장소를 허용하며 비트 개수 입력은 최대 64비트입니다. 비표준 너비와 더 넓은 저장소에는 별도 소스 계약이 필요합니다.

아키텍처 lifter는 미정의 출력의 부가 메타데이터를 트랜잭션 방식으로 관리합니다. 각 시도 전에 이전 증거를 지우고 정확히 대응하는 성공한 리프트에만 효과를 게시합니다. `Missing`은 증거가 없다는 뜻이며 빈 `Complete` 설명과 다릅니다. 일반 LowIR은 선택된 결정적 값을 유지합니다. `LowIRUndefinedIndependence`는 전달받은 완전한 비순환 LowIR 그래프의 제한된 관계 증명을 담당하며, 일반 입력을 공유하고 새 미정의 값의 상관관계를 보존합니다. 전체 명령 경계와 연산 다이제스트에 증거를 결합하고 불완전한 증명을 거부합니다. 아래 유한 네이티브 실행 경로 범위를 넘는 일반 네이티브 그래프 인증, 루프 불변식, 네이티브 코드에서 C로의 동등성 증명은 별도 작업입니다.

기존 스칼라 SHL/SAL, SHR, SAR는 8/16/32/64비트 피연산자에 대해 횟수별 미정의 비트 증거를 제공합니다. 마스킹한 횟수가 0이면 모든 플래그를 보존하고, 0이 아니면 AF, 1보다 크면 OF, SHL/SHR에서 피연산자 비트 폭 이상이면 CF가 임의 값입니다. SAR의 CF는 정의됩니다. 명령 내부 불리언 가드는 겹치는 대상 쓰기 전에 저장한 횟수를 사용하며 메타데이터 요청 여부와 관계없이 동일한 연산을 생성합니다. 감사하지 않은 인코딩은 부분 효과를 게시하지 않습니다.

레거시 ROL/ROR는 아키텍처 마스크를 적용한 횟수가 1보다 클 때만 새로운 임의 OF 비트를 만듭니다. 이후 바이트/워드 폭으로 나머지를 구해도 이 조건은 바뀌지 않습니다. 횟수 0은 플래그를 보존합니다. 레지스터 비트 기준의 BT/BTS/BTR/BTC는 독립적인 네 비트(OF/SF/AF/PF)를 만들며 CF는 정의되고 ZF/DF는 보존됩니다. 정확한 레지스터 및 imm8 인코딩을 감사하며 메모리 비트열, LOCK, APX, 캐리를 포함한 회전은 이번 추가 범위에서 제외됩니다. 효과는 핵심 명령 완료 후 발생하며 메타데이터 요청 여부에 따라 LowIR가 달라지지 않습니다. [Intel 비트 테스트 참조](https://cdrdv2-public.intel.com/929353/253666-093-sdm-vol-2a.pdf)와 [회전 참조](https://cdrdv2-public.intel.com/929354/253667-093-sdm-vol-2b.pdf)를 참고하세요.

레거시 레지스터 XADD도 정확한 8/16/32/64비트 인코딩 감사를 제공합니다. CF/PF/AF/ZF/SF/OF를 정의하고 DF를 보존하며 새로운 임의 비트를 만들지 않습니다. 교환되는 두 레지스터 모두 아키텍처의 부분 쓰기 및 32비트 0 확장 규칙을 따릅니다. 메모리/LOCK 및 APX XADD는 아직 감사되지 않았습니다. [Intel 호환 규칙](https://cdrdv2-public.intel.com/929360/253669-093-sdm-vol-3b.pdf)에 따라 C0/C1/D0/D1/D2/D3의 Group 2 `/6`을 SAL/SHL `/4`로 인정하고 같은 횟수 가드와 미정의 플래그를 사용합니다. [XADD 참조](https://cdrdv2-public.intel.com/929356/334569-093-sdm-vol-2d.pdf)를 참조하세요.

네이티브 불변 읽기는 `MaxImmutableLoadAddresses`로 제한한 완전한 유한 주소 집합도 지원합니다. 열거 전에 두 실행의 주소 동등성을 증명해야 하며, 모든 후보에 불변 바이트, 매핑 증거, 가변 프레임과의 비중첩 증거가 필요합니다. 선택한 값의 입력 의존성은 유지합니다. 누락 후보, 쓰기 가능 또는 재배치된 데이터, 열거 예산 소진은 인증서를 거부합니다. 인증서는 읽기 증거와 주소 한도를 결합합니다. 동적 프레임 오프셋과 임의의 외부 메모리는 지원하지 않습니다.

다음 동작에는 기본 엄격 감사 계약이 적용됩니다. `checkBinaryUndefinedIndependence`는 선언된 관찰 항목에 대해 완전하고 유한한 원본 x64 실행 경로를 검사하며, 실행 가능성에 따른 가지치기 전에 직접 분기의 양쪽을 수집합니다. 실제 near CALL은 다음 명령 주소를 스택에 넣고 내부 RET는 수정된 반환 대상도 포함하여 현재 스택 값을 읽습니다. 간접 대상은 유한 집합 전체를 열거하고 경로를 제한하기 전에 두 실행의 대상이 같음을 증명해야 합니다. 정확한 불변 메모리 읽기에는 증거와 가변 프레임으로부터의 분리 증명이 필요합니다. 실행되지 않는 분기도 포함한 모든 수집 명령에 불변 원본 바이트를 요구하며, 아래에 설명한 엄격하게 리프트한 `INT3`/`UD2` 종단 경계와 명시적 RDSSP/INCSSP 프로파일 투영을 제외한 모든 명령에는 완전한 아키텍처 메타데이터도 요구합니다. 엄격하게 리프트한 `INT3`/`UD2`는 전체 원본 바이트와 LowIR 연산 다이제스트를 보존하는 `Terminator` 경계로 남길 수 있지만, 미정의 출력 사이드카를 `Missing`에서 `Complete`로 승격하지 않습니다. 인증서는 기호 실행으로 도달 불가능함을 증명한 트랩만 보존할 수 있습니다. 실행 가능한 경로가 트랩에 도달하면 `ContractViolation`을 반환하며 인증서나 잔여 코드를 생성하지 않습니다. 이 규칙은 트랩 이후의 순차 실행이나 예외 복귀를 모델링하지 않고, `codeFollowsTrap` 휴리스틱을 사용하지 않으며, 정적 LowIR API의 지원 범위도 확장하지 않습니다. 인증서는 바이트, 매핑, 효과, 읽기 증거, 실행 설정 및 증명 예산에 결합됩니다. 정상 실행, 폴트 없음, CET 비활성화를 명시한 설정은 진입 프레임에서 모든 이미지 매핑을 제외합니다. 모든 실행 가능 경로는 외부 반환까지 완료되어야 하며 네이티브 팝 전에 진입 RSP와 원래 반환 주소 슬롯을 보존해야 합니다. 예산이 소진된 실행 접두부는 증명이 아닙니다. 직접 및 간접 루프는 완전한 유한 전개로만 인증합니다. 종료하지 않거나 예산을 초과하는 경로와 그 밖의 미감사 효과는 거부합니다. `specializeBinaryInterpreterWithIndependence`는 증명 후 복원하며 실패 시 잔여 코드를 반환하지 않습니다. 일반 복원은 이 게이트를 기본으로 사용하지 않으며 루프 불변식, 예외 디스패치, CET 활성 실행, 네이티브 코드에서 C로의 동등성은 인증 범위 밖입니다.

명시적 `RetainUnauditedNativeBoundaries` 옵션은 유한 네이티브 독립성 증명과 네이티브에서 LowIR로의 정제 증명에 거부 경계를 추가합니다. 엄격하게 디코딩하고 리프트한 명령 중 커버리지가 `Missing`이고 효과가 비어 있으며 비어 있지 않은 연산 다이제스트가 일치하는 경우만 허용합니다. 구조, 제어, 중첩, 프로필 및 자원 검사를 모두 유지합니다. 해당 경계의 후속 수집만 중지하며 다른 간선이 후속 바이트에 진입하면 독립적으로 수집합니다. 실행 가능한 도달은 실행 전에 거부하며 솔버의 미확정 결과나 예산 소진은 도달 불가를 증명하지 않습니다. 성공한 인증서는 정확한 경계, 네이티브 바이트 다이제스트, 연산 다이제스트를 담은 유형 및 버전이 있는 기록에 결합되며 `Missing`을 `Complete`로 바꾸지 않습니다. 수집하지 않은 후속 영역에는 감사 결론이 없습니다. 독립성은 모든 임의 선택을 포함하지만 선택값 정제의 도달 불가는 선언한 증인에 한정됩니다. 정적 LowIR, 루프 증명/추론 및 정확한 LLVM API는 이 옵션을 활성화하지 않습니다.

`AllowOverlappingNativeInstructions`는 유한 네이티브 독립성 증명과 네이티브에서 LowIR로의 정제에 사용하는 별도 옵션이며 기본값은 꺼짐입니다. 각 진입점을 독립적으로 디코딩하고 검증하며, 겹치는 명령어 바이트는 후보의 읽기를 포함한 모든 이전 명령어 및 불변 읽기 증거와 일치해야 합니다. 후보 LowIR 주소는 레이블이며 바이트 증거가 아닙니다. `MaxNativeInstructionBytes`의 기본값은 1048576이며, 새로 가져온 각 진입점의 전체 명령어 크기를 중복 바이트까지 포함하여 비교 전에 차감합니다. 예산 소진이나 바이트 충돌은 인증서를 거부합니다. 옵션과 한도는 증명 다이제스트에 포함됩니다. 정적 LowIR, 귀납적 루프 증명과 추론은 빈 루프 계획에서도 이 옵션을 거부합니다. 정확한 LLVM API와 CLI의 기본 동작은 유지됩니다.

네이티브 패킹 플래그 증명은 옵션과 계약 모두에 `X64FlagsProfile = UserX64NoFaultV1`을 지정해야 하며, 기존 실행 Boolean 옵션으로 자동 활성화되지 않습니다. 정규화된 공유 진입 플래그와 지속되는 시스템 플래그는 기계 상태 소스 래퍼와 같은 PUSHFQ/POPFQ 스칼라 변환 및 CPL3/IOPL0 마스크를 사용합니다. 매 POPFQ에서 두 실행의 TF/AC가 모두 0임을 증명하며 이 조건을 경로 가정으로 사용하지 않습니다. 레지스터나 기록된 프레임 관찰을 꺼도 최종 시스템 플래그는 항상 비교합니다. 인증서는 프로파일 버전과 정확한 변환 다이제스트를 결합합니다. 이 명시적 CET 비활성 프로파일에서는 독립적으로 검사한 정규 RDSSPD/RDSSPQ 바이트를 정확한 NOP로 투영하고 타입이 있는 증거를 기록할 수 있습니다. 원래 `Missing` 메타데이터는 유지되며 32비트 대상도 전체 레지스터를 보존합니다. 정규 INCSSPD/INCSSPQ는 프로파일에 따른 #UD 경계로 보존하고 도달할 수 없는 원본 명령의 증거를 기록합니다. 실행 가능한 방문은 피연산자가 0이어도 무결함 실행 계약을 위반합니다. 다른 CET 명령, CET 활성 실행 및 정적 LowIR API의 프로파일은 지원하지 않습니다. 유한 루프의 매 방문은 상태를 보존하고 새로운 미정의 선택을 생성하지만 불변식 증명은 아닙니다.

`checkLowIRRefinement`와 `checkBinaryLowIRRefinement`는 결정적 LowIR 후보에 대한 별도의 구성적 정제를 검증합니다. `LiftedBits`는 각 미정의 값 생성 시 원래 리프터가 계산한 비트를 선택하고, `ZeroBits`는 감사된 조건이 활성화된 비트만 0으로 선택합니다. 각 동적 발생을 기록하며 복사와 스필은 같은 선택을 유지합니다. 두 프로그램은 기존 스칼라, 물리 스택, 메모리, 플래그 실행기와 진입 스냅샷을 공유합니다. 모든 실행 가능한 경로가 종료하고 허용된 전체 진입 영역을 덮어야 하며, RETURN 피연산자, 지정 레지스터, 필수 네이티브 시스템 플래그와 양쪽이 쓴 프레임 바이트의 합집합이 일치하고 진입 보존 계약을 만족해야 합니다. 실행 및 관계 검사 예산을 공유하며 `MaxTerminalPairs`도 적용합니다. 인증서는 후보, 원본 증거, 선택 정책과 한도를 결합합니다. 선택 실패가 다른 선택을 배제하지는 않습니다. 유한 전개는 루프 불변식, 특정 CPU 일치 또는 C 백엔드 동등성을 증명하지 않으며 미정의 상태 독립성 검사를 대체하지 않습니다. 두 API 모두 증명용 메모리 임시 영역과 겹치는 입력 임시 값을 거부합니다. 바이너리 정제 API는 옵션과 관찰 계약 양쪽에 일치하는 `UserX64NoFaultV1`을 요구합니다.

`checkLowIRLoopRefinement`와 `checkBinaryLowIRLoopRefinement`는 별도 유형의 귀납 인증서를 제공합니다. 대응하는 절단점과 순수 스칼라 LowIR 상태 템플릿은 증명 후보이며, 공통 실행기가 실제 진입점에서의 성립, 구간 전체 범위, 모든 실행 가능한 후속 상태, 불변식 보존과 최종 관찰을 검사합니다. 절단점 사이의 모든 간선에서 유한한 부호 없는 사전식 순위가 엄격하게 감소해야 합니다. 매개변수 역투영 검사로 실제 상태 변화 없이 순위를 초기화할 수 없게 합니다. 템플릿은 공통 진입 상태 또는 `UseEntryPrefix` (`GeneralizeEntryPrefix = false`)로 실제 도달한 대응 접두 구간을 사용하며, 후자의 조건도 보존을 증명합니다. 절단점에서 변경된 모든 레지스터와 전체 프레임을 검사하고 이전 반복의 쓰기도 최종 관찰에 유지합니다. 현재 각 절단점은 양쪽에서 고유한 주소를 요구합니다. 불변식과 순위의 자동 발견, 임의 제어 흐름 정렬은 지원 범위 밖입니다. 누락된 절단점, 잘못된 불변식, 정수 순환, 미증명 종료성, 미지원 의미론 또는 공유 예산 소진은 인증서를 거부합니다. 계획, 모든 네이티브 구간과 원본 증거를 다이제스트에 결합합니다. 유한 정제와 엄격한 독립성의 의미는 유지되며, C 백엔드나 특정 CPU의 미정의 비트 선택을 인증하지 않습니다.

아래 접두 경로 술어 유지 요구는 `GeneralizeEntryPrefix = false`일 때 적용됩니다。

`inferLowIRLoopRefinementPlan`은 공유 기호 실행기로 예산 내 템플릿을 제안합니다. 피드백 절단점은 CFG의 모든 순환을 덮으며, 확장은 증명된 고정 비트를 유지하고 보존되지 않는 부호 없는 접두 경계를 제거합니다. 관측된 단위 증감 카운터와 추론한 단계 상수로 사전식 순위를 구성하여 증가·감소 중첩 루프를 처리합니다. `OriginalPrefix`와 `CandidatePrefix`에는 `UseEntryPrefix`가 필요합니다. 앞선 절단점 뒤의 절단점은 실제 진입점에서 별도로 제한된 재실행을 수행하여 도달 가능한 대응 접두 증거를 얻을 수 있습니다. 이 증거는 전체 진입 영역을 덮지 않습니다. 모든 실제 도달 상태가 그 술어를 함의해야 하며 진입 및 전이 경로의 완전한 검증은 여전히 필수입니다. `inferAndCheckBinaryLowIRLoopRefinement`은 완전한 복원과 유일한 네이티브 출처를 요구하고 원본/후보 전체 검사를 독립적으로 다시 수행합니다. 제안과 출처 매핑은 신뢰하지 않으며 `Refinement`만 인증서를 포함할 수 있습니다. 추론과 증명은 각각 명시적 예산을 사용합니다. 이 C++ API는 `--devirtualize`에서 자동 실행되지 않습니다. 도달 불가능한 접두, 임의 제어 정렬, 탐색 밖의 순위 유형, C 백엔드 동등성 및 실제 CPU의 미정의 비트 선택은 지원하지 않습니다.

단위 카운터 탐색은 기존 전체 워드 및 영 확장 형태를 먼저 시도한 뒤, 바깥의 모든 비트를 보존하는 바이트 정렬 부분 갱신도 인식합니다. 여러 절단점에서는 저장된 실제 도달 상태와 현재의 모든 유입 상태에서 증명한 경우에만 레인 끝값 제외 조건을 제안합니다. 확장은 실패한 조건을 제거하고 다시 추가하지 않으며, 이미 알려진 카운터 워드에서 나중에 다른 레인을 발견할 수 있습니다. 마스크는 기존 전체 워드 매개변수를 재사용하고 전체 워드 순위와 상태 관측을 유지합니다. 공유 노드·질의 예산과 독립적인 완전 정제 검사는 계속 적용됩니다.

여러 절단점에서는 관측된 카운터 튜플이 실패할 때마다 다음 튜플로 넘어가기 전에 64비트 상수 단계를 앞에 붙인 변형을 시도합니다. 두 변형은 각각 `MaxRankCandidates` 시도를 소비하며 같은 질의 예산을 공유합니다. 선행 단계는 모든 실행 가능한 전이에서 증가하지 않아야 합니다. 나머지 튜플의 엄격한 감소를 증명하지 못하는 전이는 단계 값의 엄격한 감소가 필요합니다. 음이 아닌 차이 제약은 양의 가중치 순환을 거부합니다. 따라서 순차 루프가 재초기화 후 카운터를 재사용할 수 있으면서 순환 내부의 실제 진행 의무는 유지됩니다. 기존 카운터 사이 단계와 선행 단계를 함께 사용할 수 있습니다. 완전한 튜플은 원래의 전체 전이 술어로 검사하고 최종 정제 검사기가 제안된 순위를 독립적으로 다시 검사합니다. 단일 절단점 검색에는 효과 없는 상수 접두부를 추가하지 않습니다.

단일 절단점 후보는 원래 도달 가능했던 모든 블록의 순환을 끊어야 하며, 절단점을 제거한 뒤 진입점에서 분리되는 순환도 포함합니다. 각 검사에서는 기호 실행 전에 `MaxCutpointAttempts`를 차감합니다. 모든 복귀 간선에서 단위 증감을 보이는 스칼라 카운터를 우선합니다. 이후 관찰된 단위 카운터 튜플 후보를 최대 8개까지 더 넓은 스칼라 가드·경계 가설과 하나씩 번갈아 시도합니다. 나머지 스칼라 가설을 처리한 뒤 튜플 탐색을 중복 없이 재개합니다. 이 순서는 예산 상한과 무관합니다. 튜플을 시도할 때마다 저장된 완전한 안정 템플릿과 전이를 복원하며, 튜플 영역 검사 실패는 해당 후보군만 비활성화합니다. 실패한 스칼라 가드로 그 영역을 좁힐 수 없습니다. `MaxRankCandidates`, 쿼리, 연산과 경로 예산은 계속 누적됩니다. 제안은 여전히 완전한 refinement 검사를 통과해야 합니다. 다른 분기가 값을 유지하거나 초기화하더라도 절단점으로 돌아오는 덧셈 점화식 분기는 구조적 확장을 유도할 수 있습니다. 제안된 템플릿은 여전히 모든 진입 및 전이 검사를 통과해야 합니다.

`inferAndCheckLowIRLoopRefinement`는 LowIR 루프 사이의 검증된 관계를 탐색합니다. 기본, 전체 분기 진입, 필터링된 분기 진입 계획을 차례로 시도합니다. 같은 후보군을 먼저 짝짓고 모든 서로 다른 후보군 조합을 확인한 뒤 후보 측 개별 순환 절단점을 시도합니다. 전체 분기 제안은 순환 성분 안에서 분기 블록의 후속 중 나가는 간선이 하나인 블록을 선택합니다. 필터는 모든 경로가 DFS 역방향 간선의 대상에 도달하기 전에 같은 비종료 노드에서 합쳐질 때만 해당 분기를 제외합니다. 종료와 경계를 포함한 모든 후속을 확인하며, 경계에서만 합쳐지는 분기는 유지합니다. 탐욕적으로 추가하는 절단점은 원래 도달 가능한 모든 순환을 포괄합니다. 기본 추론의 성공을 요구하지 않으면서 동작 단계를 보존합니다. `MaxCutSelectionWork`는 집합 구성과 비교를 포함한 공통 경로 분석을 별도로 제한합니다. `CutSelectionWork`는 실패 시에도 작업량을 기록하며 남은 전역 `MaxSearchWork`에서도 차감합니다. 기본 및 전체 분기 선택기는 그대로입니다. 성공과 실패를 캐시하고 각 측 최대 세 계획을 보관하며 순열은 필요할 때 열거합니다. 추가 후보군은 같은 측에서 이미 보관한 절단점 집합을 건너뛰며, 먼저 성공한 계획의 후보군 번호가 더 커도 동일합니다. 비거나 중복된 후보군은 기호 질의를 쓰지 않지만 후보 시도 횟수에 포함됩니다. 최대 여섯 계획은 하나의 `MaxMetadata` 풀을 공유하고 각 짝짓기 구성도 별도로 제한합니다. 오프셋과 너비가 같은 프레임 입력에만 동등성을 제안합니다. 레지스터 이름 변경, 아핀 관계, 임의의 피드백 집합은 명시적 짝짓기가 필요합니다. 권위 있는 짝짓기 도구와 완전한 검사기는 호출자의 원본 감사 기록, witness, 진입 영역, 프레임 관측, 종료 증명 의무를 유지합니다. `LowIRLoopAlignmentLimits`의 `MaxSolverQueries`는 실패를 포함한 모든 추론과 증명에 공유되며, 각 호출에는 단계 한도와 남은 총량 중 작은 값만 허용됩니다. `MaxSearchWork`, `MaxMetadata`, `MaxCandidateAttempts`, `MaxPairingAttempts`, `MaxCuts`는 탐색 구성과 열거를 제한합니다. 개별 시도의 예산 소진 후 재시도할 수 있지만 전체 예산 소진 시 중단합니다. `Unsupported`는 관계를 찾지 못했다는 뜻이며 프로그램의 비동등성을 뜻하지 않습니다. 새 검증에 성공한 `Refinement`만 인증서를 포함합니다. CLI 기본 동작은 바뀌지 않습니다.

`GeneralizeEntryPrefix`의 기본값은 `false`이며 `UseEntryPrefix`가 필요합니다. 기본 모드는 모든 도착에서 캡처한 경로 술어를 유지합니다. 명시적 일반화 모드는 경로 밖의 접두 상태 식을 전역에서 정의되는 미검증 템플릿 함수로 취급합니다. 실행 가능한 쌍의 증거, 실제 진입 및 구간의 완전한 경로 포함, 전체 레지스터/프레임 일치, 매개변수 투영, 네이티브 실행 조건, 확장된 귀납 영역의 엄격한 순위 감소가 계속 필요합니다. 추론은 유입 상태가 증거 영역을 벗어날 때만 절단점을 확장하고 모든 일반 전이를 다시 구성하고 검사합니다. 첫 증거가 반복을 하지 않아도 다른 진입의 루프를 숨길 수 없습니다. 정책은 귀납 인증서 다이제스트에 바인딩됩니다.

중첩 추론은 관찰된 단위 변화 카운터와 제어 술어가 참조하는 변경되지 않은 접두 값 사이의 엄격하거나 비엄격한 부호 없는 경계 후보를 생성할 수 있습니다. 등식 종료도 포함합니다. 구체적인 진입/유입 상태를 먼저 확인하고 일반 유입 전이에서 성립하지 않는 관계를 단조롭게 제거합니다. 술어 순회와 솔버 작업은 기존 명시적 추론 예산을 사용하며 최종 원본/후보 검사기가 템플릿을 독립적으로 증명합니다.

중첩 루프 추론은 접두 상태 식이 같을 때 캐시된 피연산자와 카운터 또는 변하지 않은 제어 입력 사이의 등식도 제안합니다. 모든 구체적인 유입 상태를 검사하고, 이후 확장에서 발견되는 캐시를 위한 후보를 보관하며, 일반 유입 상태가 관계를 위반하면 제거합니다. 각 워드는 복원 가능한 자체 매개변수를 유지하고 등식은 검증 대상 조건입니다. 복사 점화식은 우연한 고정 비트 제거를 빠르게 할 수 있지만 의미를 가정하지 않습니다.

중첩 추론은 변하는 비트가 최대 16개인 필드에서 카운터의 같음 또는 다름을 저장하는 관계도 탐색합니다. 관계는 현재 카운터와 경계 값을 사용하며 캐시에는 독립적으로 복원 가능한 상태 매개변수를 유지합니다. 진입 가드나 상수 접기된 초기화로 비교가 이후 확장에서 드러나면 새 후보를 제안할 수 있지만, 거부되거나 제거된 후보는 다시 넣지 않습니다. 후보는 저장된 모든 구체적 도달 상태와 현재 모든 입력 전이에서 성립해야 합니다. 변수 DAG 순회는 절단점별로 캐시하고 공통 술어 예산에 계산합니다. 완전한 네이티브 경로 범위, 전체 상태 동등성, 엄격한 순위 감소는 독립적으로 검증합니다. 일반 전이 뒤에도 카운터 탐색을 계속합니다. 최초 내부 루프 증거에서 가려진 외부 카운터에도 검증된 경계와 피연산자 복사 후보를 제공합니다. 거부된 관계는 복원하지 않으며 단위 증감 탐색에도 기호 노드 예산을 적용합니다.

진입 접두 경로를 재실행할 때 이전 루프를 더 펼치기 전에 대기 중인 분기를 방문합니다. 다른 분기가 임의 횟수의 반복을 허용해도 짧은 도달 가능한 증거를 찾을 수 있습니다. 추론과 원본／후보 검사기는 이 실행 순서를 공유합니다. 모든 작업은 기존 예산에 포함되며 접두 경로 증거가 전체 진입 범위, 불변식 유지 또는 종료 검사를 대신하지 않습니다.

| 표현 | 목적 | 주요 정의 및 변환 |
|------|------|-------------------|
| LowIR | 아키텍처 독립 `NdOp` 연산, 기본 블록, CFG, jump-table 메타데이터 | `include/neverd/ir/low`, `lib/ir/low`; `lib/decode` + `lib/lift`가 생성 |
| MedIR | 타입, ABI/호출 규약, 메모리/스택 모델, 플래그, 호출, SSA 유사 데이터 흐름 | `include/neverd/ir/med`, `lib/ir/med` |
| HighIR | 읽기 쉬운 C를 위한 구조화 표현식과 제어 흐름 | `include/neverd/ir/high`, `lib/ir/high`; `lib/backend/c/HighC`가 출력 |
| LLVM IR | 최적화, LLVM 유래 C, 대상 코드 생성, 바이너리 재작성 입력 | `lib/backend/llvm`; `lib/pipeline`이 최적화/조정 |

상수는 LowIR, MedIR, HighIR 전반에서 각 출현 위치의 스칼라/주소 출처와 주소 소유 정보를 유지합니다. 숫자 비트가 같아도 출처가 다르면 병합하지 않습니다. HighIR 기호 단순화는 주소 식별 정보를 불투명 입력으로 취급합니다. 소스 바인딩은 공통 숫자 피연산자 분류를 사용하며, 메모리 및 포인터로 사용하는 경우에는 계속 재배치 바인딩을 요구합니다.

| 사용자 경로 | 표현 경로 | 출력 |
|-------------|-----------|------|
| Low/Med dump | Binary -> LowIR, 선택적으로 -> MedIR | 진단 텍스트 |
| High dump 또는 `decompile` | Binary -> LowIR -> MedIR -> HighIR | HighIR 또는 구조화 C |
| `lift` | Binary -> LowIR -> MedIR -> LLVM IR | `.ll` |
| `decompile --llvm` | Binary -> LowIR -> MedIR -> LLVM IR | LLVM 유래 C |
| `patch` | Binary -> LowIR -> MedIR -> LLVM IR -> codegen | 재작성 바이너리 |

경로 선택의 기준은 `lib/pipeline/Pipeline.cpp`입니다. 표현별 로직은 소유한 IR 또는
backend 라이브러리에 두고, pipeline은 알고리즘을 흡수하지 말고 해당 구성 요소를
조정해야 합니다.

## 교차 아키텍처 변환 계약

`include/neverd/translate`는 실행 backend가 아닌 계약 계층을 정의합니다.
`GuestState`는 `x86_32`, `x86_64`, `AArch64`, `ARM32`의 아키텍처 독립적이고
머신에서 관찰 가능한 상태를 모델링합니다. 정규 version 1 직렬화는 고정 폭
little-endian 필드, 안정적인 레지스터 ID, 정렬된 컬렉션, fail-closed 검증을 사용하므로
지속된 상태가 호스트 C++ 레이아웃에 의존하지 않습니다.

`GuestState`의 wire v1 baseline은 영구히 동결됩니다. 이 baseline 밖의 상태는 확장
범위의 extension-register ID와 정규 소문자 이름을 함께 사용하거나, 명시적 upgrader를
갖춘 새 wire version으로 옮겨야 합니다. v1 baseline을 제자리에서 변경하는 것은
금지됩니다.

`ARM32` guest에서는 `ExecutionMode`가 권위 있는 decode mode이며 `CPSR.T`와
일치해야 합니다. 저장되는 PC는 항상 bit 0을 지운 정규 명령어 주소이고, ARM
mode에서는 추가로 word alignment를 만족해야 합니다.

아키텍처 쌍 정책은 `x86_64 -> AArch64`, `AArch64 -> x86_64`,
`x86_32 -> AArch64/ARM32`, `ARM32 -> x86_32/x86_64`를 정의합니다.
`ContractDefined`는 요청을 검증하고 지속할 수 있다는 뜻이며 코드를 변환하거나
실행할 수 있다는 뜻은 아닙니다. JIT 정책은 실행 중인 프로세스의 native host만
허용하고, AOT 정책은 호스트 아키텍처와 target triple을 명시하도록 요구합니다.
CPU 또는 feature set을 선택하는 경우에도 명시해야 합니다.

`ResolvedHostTarget`은 이 선택을 구체적인 결과로 해석합니다. `Native` 해석은 현재
process에서 triple, CPU, 활성/비활성 feature set을 가져옵니다. `Explicit` 해석은
호출자가 제공한 architecture, triple, CPU, feature를 검증하고 정규화하며 충돌하는
입력을 거부합니다. version이 있는 cache identity는 정규화된 target input을 결정적인
byte 순서로 구성하며 process address나 locale 의존 text를 포함하지 않습니다.

version이 있는 `TranslationExit`는 안정적인 중지 이유와 그에 대응하는 typed payload를
기록합니다. syscall, 예외 또는 signal, breakpoint, 미지원 명령어, self-modification,
리소스 budget, 외부 호출, memory fault 및 그 밖의 종료 조건을 다룹니다. 따라서
소비자는 중지 이유에 따라 타입 없는 정수를 다시 해석할 필요가 없습니다.

대응하는 `BudgetExhausted` 경우를 제외하면 결과가 보고하는 instruction, block,
generated-code count는 요청의 대응하는 0이 아닌 budget을 초과할 수 없습니다.
instruction과 block 고갈은 limit에서 정확히 멈춥니다. 생성 object 크기는 나눌 수 없는
codegen이 끝난 뒤에만 정확히 측정되므로 해당 고갈 결과는 `Observed > Limit`을 보고할
수 있습니다. 거부된 object는 link, publish, execute되지 않습니다. 모든
`BudgetExhausted` payload는 파생 값이나 구현 전용 임계값이 아니라 요청 limit을 정확히
식별합니다.

backend-private `RuntimeControlBlockV1` 계약은 정확히 128 byte이고
8 byte alignment를 사용하며 고정된 v1 magic, version, size, field offset, 0인 reserved
field, 일관된 typed exit로 제약됩니다. C++ container, host pointer, guest address alias를
포함하지 않으며 `GuestState`의 C++ layout이나 wire format도 아닙니다. 이 계약을 구현하는
backend는 상태를 이 record로 명시적으로 변환해야 합니다.

고정 v1 generated-code 호출 표면에는 정확히 8개의 helper만 있습니다:
`nvd_rt_v1_load8_le`, `nvd_rt_v1_load16_le`, `nvd_rt_v1_load32_le`,
`nvd_rt_v1_load64_le`, `nvd_rt_v1_store8_le`, `nvd_rt_v1_store16_le`,
`nvd_rt_v1_store32_le`, `nvd_rt_v1_store64_le`. 이름, signature, pointer provenance는
정확히 일치해야 하며 backend는 이 유한 table을 명시적으로 bind하고 ambient symbol
resolution으로 fallback해서는 안 됩니다. executable generation 검증과
budget/cancellation polling은 신뢰된 dispatcher만 수행하는 작업입니다.
`nvd_rt_v1_validate_generation`과 `nvd_rt_v1_poll`은 generated-code helper가 아닙니다.
신뢰된 host dispatcher는 block 선택도 소유하며 생성 IR에서 호출할 수 없습니다.
translated block은 대신 typed exit code를 반환합니다. 생성 IR은 선언된
scalar-result runtime slot만 직접 읽을 수 있습니다.

`RuntimeSymbolRegistryV1`은 이 helper table을 닫힌 host-side registry로 구현합니다.
생성 시 완전한 ABI-v1 집합, 정확한 canonical name, helper class, signature, 그리고 각
entry에 class와 일치하는 non-null function pointer가 정확히 하나 있는지 검증합니다.
lookup은 완전 일치하는 이름만 허용하고 process 환경이나 dynamic loader의 symbol을
조회하지 않으며 object verifier allowlist에 동일한 정렬된 이름을 제공합니다. version이
있는 identity는 이름, helper class, ABI shape를 포함하지만 native address는 의도적으로
제외하므로 ASLR과 무관합니다.

`RuntimeCodeMemory`는 page 단위로 격리된 generated-code storage를 소유하며 단방향
`RW -> RX` publication만 허용합니다. memory는 동시에 writable 및 executable이 될 수
없고 publication 뒤에 write 가능 상태로 되돌릴 수도 없습니다. write와 entry offset은
bounds-check되며 publication 시 host instruction cache를 invalidation합니다. native
smoke test는 publication 이후 작은 host instruction sequence만 실행합니다. 이는 W^X
memory boundary만 증명할 뿐 translation engine을 증명하지 않습니다.

`GuestMemoryRuntime`는 논리적 `GuestState`와 격리됩니다. 생성 시 state를 검증하고
region byte와 metadata를 정렬된 private index로 복사합니다. guest virtual address는
lookup key일 뿐이며 host pointer로 변환되지 않습니다. 검사된 scalar access는 width,
alignment, overflow, unmapped, cross-region, permission, executable-write, generation
overflow/mismatch, policy fault를 typed 결과로 보고합니다. instruction/block budget,
cancellation, generation tracking과 `RejectExecutableWrites`,
`InvalidateOnExecutableWrite`, `ValidateBeforeDispatch` code-write policy도 암묵적인 host
동작 대신 일관된 typed record를 생성합니다.

`TranslationObjectCompilerV1`은 검증된 LLVM IR-to-object 경계입니다. const input
module을 검증하고 모든 변환 전에 clone하며, proof-gated semantic simplification과
LLVM `O0`~`O3` optimization을 결합한 뒤 final IR을 다시 검증하고 4개의 contract host
architecture용 relocatable ELF, COFF, Mach-O object를 emit합니다. 정확한
target-mangled block/runtime symbol manifest를 canonicalize하고 emit된 모든 object를
audit하며 runtime registry identity와 version이 있는 request/artifact cache key를
반환합니다. generated-byte budget이 0이 아니면 이를 만족하는 object만 artifact
verification으로 진행할 수 있습니다. LLVM은 먼저 private buffer에 나눌 수 없는 emit을
완료해 정확한 크기를 측정합니다. 초과 object는 publish와 artifact audit 전에 거부되며,
typed telemetry는 관측 크기와 요청된 정확한 limit을 보존합니다. 0은 caller policy에서
unlimited를 뜻합니다. compiler는 audit된 relocatable byte까지만 만들며 link, publish,
dispatch, execute 또는 guest instruction lowering을 수행하지 않습니다.

post-codegen verifier는 relocatable ELF, COFF, Mach-O object를 닫힌
집합으로 감사합니다. format과 architecture는 선택한 host와 정확히 일치해야 하고,
undefined symbol은 유한 helper allowlist에 정확히 포함되어야 하며 dynamic symbol은
금지됩니다. relocation은 명시적인 direct whitelist로 제한되고 encoding, width,
alignment, offset, loadable destination, object-local non-preemptible definition 또는
정확히 허용된 helper target을 검사합니다. W+X, unwind/exception 및 initializer
metadata, TLS, IFUNC, GOT와 일반 PLT indirection, dynamic relocation, weak/preemptible
또는 선택 가능한 definition, 알 수 없는 allocated section, linker directive를
거부합니다. LLVM이 hidden x86-64 ELF call에 사용하는 `R_X86_64_PLT32` 표기는 v1
policy가 exact runtime helper로 향하는 sealed direct branch임을 증명할 때만 허용되며,
PLT나 GOT path를 허용하지 않습니다. ELF `ET_REL` artifact에는 program header나
segment가 없어야 합니다. Mach-O load command는 positive list로 제한되며 bit 폭이
일치하는 segment는 정확히 하나, symbol table, dynamic-symbol table, platform-version,
data-in-code command는 각각 최대 하나만 허용하고 의존 관계도 검사합니다. linker
option과 그 밖의 모든 command는 거부됩니다.

`TranslationObjectRequestV1`은 이 계약 위에 구축된 최초의 공개
guest-byte-to-object slice이며 범위를 의도적으로 좁혔습니다. 현재 공개된 fail-closed
x86-64 v1 scalar-register subset에서는 legacy prefix가 없는 canonical encoding만
허용합니다. 즉, 지원되는 register/immediate LowIR 형태를 갖는 REX.W full-width GPR
`MOV`, `ADD`/`SUB`, `AND`/`OR`/`XOR` 형식만 포함합니다. schema 9는 full-width
register/register `CMP`의 `39/3B`, register/immediate `CMP`의 `81/7`, `83/7`, `3D`,
full-width register/register `TEST`의 `85`, register/immediate `TEST`의 `F7/0`, `A9`도 허용합니다.
산술 형식은 기존 scalar flag 계산을 유지하고, 논리 형식과 `TEST`는 아키텍처가 정의한
flags를 계산하면서 NeverD state model의 `AF`를 보존합니다.
canonical `C3` `RET`와 `C2 iw` `RET imm16`은 return block을 종료하고, canonical `EB cb`와
`E9 cd` direct-relative `JMP` encoding은 direct-branch block을 종료합니다. 공개 lowering
schema는 9입니다. canonical이며 legacy prefix가 없는 traditional Jcc는 `JO`/`JNO`의
short `70/71 cb` 또는 near `0F 80/81 cd`, `JB`/`JAE`의 `72/73 cb` 또는
`0F 82/83 cd`, `JE`/`JNE`의 `74/75 cb` 또는 `0F 84/85 cd`, `JBE`/`JA`의
`76/77 cb` 또는 `0F 86/87 cd`, `JS`/`JNS`의 `78/79 cb` 또는 `0F 88/89 cd`,
`JP`/`JNP`의 `7A/7B cb` 또는 `0F 8A/8B cd`, `JL`/`JGE`의 `7C/7D cb` 또는
`0F 8C/8D cd`, `JLE`/`JG`의 `7E/7F cb` 또는 `0F 8E/8F cd`로 제한됩니다.
`JRCXZ`/`JECXZ`/`JCXZ`와 `LOOP`/`LOOPE`/`LOOPNE`는 아직 공개되지 않았으며 fail closed합니다.
예약된 `F7 /1`, guest-memory operand, partial-register form, legacy prefix, 의미적으로 중복된
REX extension bit도 fail closed합니다.
출력은 audit된 little-endian AArch64 ELF
또는 Mach-O relocatable object로 제한됩니다. 일반 guest memory operation,
partial-register form, 이 정확한 subset 밖의 모든 instruction/encoding, return, 이러한
direct jump와 위에 공개된 Jcc branch 이외의 control flow 및 lowerer가
구현하지 않은 모든 LowIR operation은 object emission 전에 거부됩니다.
`RET`에 필요한 검사된 return-address read는 terminator contract 내부 동작이며 일반적인
guest-memory lowering을 공개하지 않습니다. request는 block descriptor를 다시 구성해
검증하고, lowering과 object emission에 동일한 resolved target machine을 사용하며,
proof-gated semantic simplification과 LLVM 기본 `O2` optimization pipeline을 결합합니다.
이 slice는 그 밖의 x86-64 instruction, 다른 guest/host pair 또는 AArch64에서 x86-64로의
역방향을 지원한다는 뜻이 아닙니다.

공개 C entry point `neverd_translate_x86_64_block_to_aarch64_object_v1`, Python ctypes
wrapper `translate_x86_64_block_to_aarch64_object`, `neverd translate-object` command는
같은 object-only 경계를 노출합니다. Python은 `TranslationObjectFormat.ELF` 또는
`.MACHO`를 사용합니다. native translation 실패 시 `TranslationErrorCode`를 담은 typed
`TranslationError`를 발생시키고, local argument validation은 `TypeError` 또는
`ValueError`를 발생시킵니다. 성공 시 Python이 소유하는 immutable result를
반환합니다. C result는 object byte, 안정적인 cache identity, optimization telemetry를
소유하며 CLI는 선택한 ELF 또는 Mach-O object만 기록합니다. 세 surface 모두 link,
load, dispatch, execute, debug 전에 멈추며 execution session interface가 아닙니다.

`verifyTranslationLinkGraphV1`은 독립적인 allocation 전 두 번째 감사를 추가합니다. 승인된
AArch64 ELF 또는 Mach-O object에서 임시 LLVM JITLink graph를 만들고 target, section
permission, block/runtime symbol manifest, external-symbol closure, edge kind와 target을
검사합니다. 주소 없는 감사 결과를 만든 뒤 graph는 폐기됩니다. 이 감사를 통과해도
code를 link, allocate, resolve, load, publish, dispatch 또는 execute하지 않습니다.

`linkTranslationObjectV1`은 별도의 native linking 경계입니다. pruning, allocation,
symbol resolution, fixup 전후에 trusted descriptor, raw object, JITLink graph를 다시
감사합니다. runtime symbol은 sealed registry에서만 가져옵니다. dispatcher credential은
유일한 manifest entry를 해당 session, block identity, guest entry PC, cache generation,
code epoch에 결속하고, invoke 시 runtime guest `RIP`도 그 entry와 일치해야 합니다.
finalization에 성공하면 최종 permission으로 executable memory를 publish합니다. unload는
새 invoke를 무효화하고 진행 중인 한 번의 invoke가 끝나기를 기다린 뒤 allocation을
해제합니다. credential-free overload는 audit-only이며 invoke할 수 없습니다.

`NativeTranslationSessionV1`은 이 요소들을 experimental C++ x86-64-to-native-AArch64
execution 경계로 결합합니다. little-endian AArch64 ELF 또는 Mach-O process에서
compile-link-validate-invoke-unload dispatcher loop 전체에 걸쳐 하나의 checked guest-memory
runtime과 고정 guest state를 여러 block 사이에 유지합니다. canonical direct jump는 정확한
static target에서 계속됩니다. 공개된 canonical Jcc branch는 block
manifest가 선언한 taken 또는 fallthrough successor에서만 계속되며 dispatcher는 그 밖의 selected PC를 모두
거부합니다. return은 종료합니다. global instruction, block, generated-object-byte budget은
block 전체에서 정확하게 유지되며 guest가 성공적으로 멈추면 실행된 state와 authoritative
memory를 함께 commit합니다. cancellation은 final commit에 대해 linearize됩니다.

이는 실행 가능한 vertical slice이지 완전한 translator가 아닙니다. 일반 guest-memory
instruction, partial register, 위의 정확한 schema-9 traditional-Jcc slice 밖의 conditional control
flow(`JRCXZ`/`JECXZ`/`JCXZ`와 `LOOP`/`LOOPE`/`LOOPNE` 포함), indirect control flow,
call, floating-point, SIMD, x87, atomic, system instruction, 일반 exception propagation,
block cache, 다른 guest/host pair, 역방향 AArch64-to-x86-64는 아직 지원하지 않습니다.
execution session에는
C, Python, CLI, JSON surface가 없으며 debugging은 별도의 미지원 기능입니다. 위 object API는
native execution을 선택하지 않아도 계속 사용할 수 있습니다.

생성 IR 계약은 이 계약의 적용을 받는 모든 translated block이 hidden 및
non-preemptible이고 C ABI `i32 (ptr state, ptr runtime)`를 사용하도록 요구합니다.
block은 private registry로만 발견되며 프로세스 환경의 symbol lookup에 의존하지
않습니다. block 간 직접 호출도 금지됩니다.

IR verifier는 legalization이 알려진 compiler-runtime libcall을 도입하지 않도록 정수
폭을 호스트 scalar register 폭 이하로 제한합니다. 다만 이는 필요조건일 뿐입니다.
이 계약을 구현하는 모든 실행 backend는 post-codegen control transfer, `MachineIR`,
target object의 relocation을 동일한 유한 runtime-symbol allowlist에 대해 정확히
감사해야 합니다.

TranslationIR의 직접 load/store와 private constant가 보관하는 값에는 호스트
scalar-register 폭 이하인 단일 scalar integer만 허용됩니다. aggregate는 verifier
경계 전에 scalarize하여 압축된 IR이 backend에서 무제한 확장을 유발하지 않게 해야 합니다.

generated-code ABI는 scalar integer에 대해서만 정의됩니다. 부동소수점, SIMD, x87,
atomic 및 system instruction은 이 계약의 범위 밖입니다. `ProvenSemanticAndLLVM`을
선택하는 구현은 NeverD의 proof-gated semantic simplification을 LLVM 최적화와의 공동
fixed point까지 실행해야 합니다. 이 정책 자체는 실행 가능한 translation backend를
제공하지 않습니다.

## Windows 드라이버 에뮬레이션

`lib/emulation`은 `NEVERD_ENABLE_DRIVER_EMULATION`으로 활성화하는 선택적 실행 구성 요소입니다. `emulate-driver` CLI는 공개 C API로 접근합니다. `DriverSession`은 제한된 x64 WDM 초기화와 선택적인 순차 create/IOCTL/read/write/cleanup/close/unload 호출을 담당합니다. Windows 이미지 매핑은 기존 로더의 완전한 `BinaryImage`를 사용하고 Windows 모델은 게스트 객체와 API 의미를 관리합니다. `driver-strict`에서는 선택한 Unicorn, KVM 또는 WHP 어댑터가 동일한 공유 물리 메모리와 주소 공간 관리 주체를 사용합니다. 백엔드별 기능 정보는 이식 가능한 엔진 콜백과 네이티브 아키텍처 진입 전 검사를 구분합니다. 이 경로는 실험적인 네이티브 변환 파이프라인을 사용하거나 그 지원 범위를 변경하지 않습니다.

Unicorn은 `cmake/NeverDUnicorn.cmake`에서 한 번 구성되며 의미론 테스트와 공유합니다. `BUILD_TESTING=OFF`일 때도 사용할 수 있습니다. 알 수 없는 API와 CPU 환경 동작은 명시적으로 중단되며, 드라이버가 반환한 실패는 미완료 에뮬레이션과 구별됩니다. 한도, 보고서 및 지원하지 않는 수명 주기 작업은 [드라이버 에뮬레이션](driver-emulation.md)을 참조하세요.

기존 C API는 초기화만 수행합니다. 시나리오 JSON은 동일한 실행 옵션에 대해 하나의 엄격한 파서를 사용하며, 필드와 요청 종류는 `.def` 목록에 선언됩니다. 요청한 베이스 재배치와 security-cookie 초기화는 실행 로더가 담당합니다. Windows 모델은 IRP/스택 위치/파일 객체를 소유하고 동기 완료 또는 작업 항목에 의한 보류 완료를 검증하며, 세션은 공유 실행 예산 아래에서 콜백 순서를 제어합니다. 사용되지 않는 알 수 없는 import는 지연 바인딩이며, 이를 실행하거나 모델링되지 않은 export 데이터를 읽으면 명시적으로 중단됩니다.

Export 레지스트리는 정적 import와 동적 루틴 조회에 안정적인 게스트 주소를 부여합니다. Export의 가용성과 구현 여부는 별개입니다. 명시적인 부재는 NULL로 해석되고, 존재하지만 모델이 없는 루틴은 트랩에 바인딩되며, 동적 가용성이 지정되지 않으면 중단합니다. 요청 모델은 독립적인 파일 식별자와 요청 소유 MDL을 관리하며, 매핑 권한과 수명 만료도 포함합니다. 런타임은 세션의 검증된 Win64 인수 판독기를 통해 게스트 가변 인수를 읽습니다. 백엔드 오류는 최초 원인을 구조화된 형태로 보존합니다. 관찰과 보고는 오류가 발생한 CPU를 재개하지 않으며 이러한 백엔드 오류를 Windows 예외로 처리하지 않습니다.

Windows 모델은 독립적인 비페이지 풀 MDL도 관리하며, 설명자를 해제해도 원래 버퍼는 해제하지 않습니다. MDL 체인과 IRP 연결은 모델링하지 않습니다. 별도 레지스트리 모델이 명시적 시나리오 트리, 핸들 권한, 키·값 수명을 관리하며 정적 내보내기 목록과 분리됩니다. 사전 검증과 실행은 같은 검증 규칙을 사용하고 보고서에는 최종 키·값이 보존됩니다. 언로드 시 남은 핸들을 검사합니다.

`KernelScheduler`는 준비 큐 순서, 콜백 식별자와 타이머 기한을, `KernelDispatcher`는 불투명 DPC·타이머·이벤트 객체와 신호를 관리합니다. `KernelModel`은 대기 등록, 작업 항목/장치 수명과 IRP 완료를 관리합니다. `DriverSession`은 Win64 스택 인수를 포함한 별도 콜백 스택과 전체 CPU 컨텍스트를 중단·복원하며 게스트 메모리는 공유합니다. 준비된 프레임이 없을 때만 가상 시간은 타이머/대기/취소 경계에서 진행하고 CPU0의 결정적 협력 스케줄링으로 DPC는 `DISPATCH_LEVEL`, 작업 항목은 `PASSIVE_LEVEL`에서 실행합니다. 일반 스레드/APC/스핀락, 설명된 계약 밖의 WDM/PnP 취소, 임의의 동시 공개 시나리오 제출, 전체 PnP/전원 또는 일반 하드웨어 지원은 포함하지 않습니다. API IRQL 상한은 `KernelAPIIRQL.def`에 정의되며 인수별 제한은 담당 모델이 검사합니다.

`KernelModelDeviceStack`은 장치의 드라이버 소유자, 할당, 상하 연결, 삭제 대기 상태, 내부 참조를 하나의 레코드로 관리합니다. 게스트 `NextDevice` 목록과 호스트 소유 연결 그래프는 별개입니다. 이름 확인은 이름 있는 하위 장치를 `FILE_OBJECT`와 보고서에 유지하고 초기 디스패치와 READ/WRITE 방식은 현재 최상단을 선택하며 요청 경로 전체를 보존합니다. 분리·삭제 후에도 요청/콜백이 참조하는 장치는 만료되지 않으며 공개 `ReferenceCount`는 열린 핸들만 셉니다.

`KernelModelIRPStack`은 원래 게스트 패킷의 제한된 커서, 정확한 대상 디스패치, 완료 해제를 관리하고 인라인 Copy/Skip/SetCompletion 쓰기를 단일 근거로 사용합니다. 디스패치 상태, 완료 제어값, 최종 `IoStatus`는 별개이며 반환 후에도 pending을 전달할 수 있습니다. `STATUS_MORE_PROCESSING_REQUIRED`는 중첩 완료를 포함해 최종 해제를 재개할 때까지 IRP/MDL/버퍼를 보존합니다. `KernelGuestCall`의 소유 하위 시스템과 로컬 토큰이 WDM/WDF continuation 충돌을 막고 `DriverSession`은 CPU 프레임과 상속 IRQL을 유지합니다. 단일 게스트 드라이버를 별도 소유의 시나리오 PDO 위에 연결할 수 있습니다. 드라이버 할당 IRP, WDF 연결/전달, 사용 중 스택 연결, 중간 계층 분리, 주 기능 변경, 경로 외부 대상은 지원하지 않습니다. 상위 완료 콜백 전에 소비된 하위 스택 위치를 0으로 지웁니다.

`DriverPnp.h`와 공개 `DeviceLifecycle.def`가 수명 주기 열거형 및 정확한 성공 상태 계약을 소유하며 `devicePnpFinalStatusError`를 사전 검증과 게스트 최종 완료에서 공유합니다. `KernelModelPnpDevices`는 안정된 PDO 식별자, 독립 제공자 목록과 실제 AddDevice 관측을 소유합니다. `KernelModelPnpRequests`는 트랜잭션 및 불변 장치/파일 식별자를 기존 IRP에 연결합니다. 중지, 제거 대기, 전원 상태를 근거로 I/O 실패를 만들지 않고 실제 게스트가 성공, 실패, 대기를 결정합니다. `KernelModelPnpCompletion`은 버스 실제 수신/완료 및 가상 기한을 관리하며 `KernelModelIRPStack`과 소유자 표시 continuation을 재사용합니다. 최종 상위 완료가 상태를 확정하고 성공 PnP는 실제 제공자 완료를 요구합니다. Start/QueryStop/QueryRemove의 이른 상위 실패는 버스 관측을 null로 둘 수 있습니다. Stop/CancelStop/SurpriseRemoval/CancelRemove/Remove는 정확히 STATUS_SUCCESS여야 하며 QueryStop의 STATUS_RESOURCE_REQUIREMENTS_CHANGED (0x119)는 미구현 리소스 재질의로 거부합니다. 제공자 퇴역과 게스트 분리/삭제는 독립적이며 누수를 자동 정리하지 않습니다. 여덟 부 기능은 명시적 버스 설정에 따라 동작하며 PnP 패킷은 순차적으로 처리하고 Remove 전에 파일 및 이전 요청을 종료해야 합니다. 기타 PnP, 기타 하드웨어/리소스 및 설명된 부분집합 밖의 더 넓은 KMDF PnP는 지원하지 않습니다.

`KernelRemoveLocks`는 등록, 정확한 DEVICE_OBJECT 소유자, 크기, Tag 중복 계수 및 drain 래치의 유일한 소유자로 `DeviceLifecycle` 트랜잭션과 분리됩니다. PDO 상태나 IRP 형태의 Tag는 소유권을 만들지 않습니다. `KernelModel`은 확장부 전체 저장소 및 불투명 접근을 검증하고4개 Ex를 라우팅하며 형식화된 재개 가능한 RemoveLock 대기를 등록합니다. 최종 release는 콜백 반환 전에 대기를 준비시키고 합성 콜백 대신 CPU continuation을 사용합니다. AndWait는 연관 REMOVE 경로와 실제 제공자 수신을 검사하지만 하위 완료나 살아 있는 Tag 패킷은 요구하지 않으며 완전한 Driver Verifier도 아닙니다. REMOVE는 파일 닫기/이전 요청 종료 제한을 유지하면서 잠금을 해제하는 콜백을 허용합니다. 세션은 남은 프레임 종료까지 경로를 보존하고 소유권 해제 전에 최종 제거를 검증합니다. 획득/대기 저장소 검사는 delete-pending 변경보다 먼저, 등록 해제는 실제 확장부 퇴역 때 수행합니다. drain은 작업 항목/경로 참조를 소비하지 않습니다.

`DriverPower.def`는 전원 유형/동작 표기와 요청 출처를 정의하고 `DriverPnp.h`의 같은 `DriverPowerOperation`을 시나리오 패킷과 PDO별 응답 FIFO에 사용합니다. `KernelModelPowerRequests`는 명시적 패킷 사실, 보존 경로 및 DEVICE_OBJECT별 알림 상태를 소유합니다. `PoSetPowerState`는 해당 장치의 이전 값을 반환하며 수명 트랜잭션을 변경하지 않습니다. `KernelModelPowerCompletion`은 실제 `PoRequestPowerIrp(Query/Set)` 자식의 독립 IRP, 보고 행과 응답 인덱스를 관리합니다. 일치하는 PDO FIFO 선두만 소비하며 context로 부모를 추론하거나 부모 결과를 빌리지 않습니다. 중첩 디스패치와 최종5인수 void 콜백은 소유자 continuation, 보존 경로와 독립 스택을 재사용합니다. 상태 스냅샷은 대기를 거쳐 콜백 반환까지 유지됩니다. 동기 자식이 API의 STATUS_PENDING 반환 전에, System S0 부모가 D0 자식 전에 완료할 수 있습니다. 최종 상위 완료가 수명 상태를 확정하며 버스 결과는 독립적입니다. 이 제한된 범위는 DO_POWER_PAGABLE, DO_POWER_INRUSH 없음, PASSIVE_LEVEL을 요구하며 D0/D2/D3와 Working/Sleeping3의 Query/Set을 지원합니다. 명시적32비트 SystemContext는 불투명하게 유지합니다. 일반 전원 정책, 종료/최대 절전, 일반 하드웨어 및 임의의 동시 공개 시나리오 제출은 제공하지 않습니다.

`KernelModelPowerCompletion`은 응답 FIFO 없는 네이티브 WAIT_WAKE도 발행합니다. `KernelModelPnpRequests`가 성공한 START 티켓을 소유하며 `KernelModel::ProviderWakeIRPs`는 PDO마다 정확한 네이티브／프레임워크 IRP 하나를 유지합니다. `KernelProviderCallbacks.def`는 커널 import와 별개인 공급자 취소 루틴을 선언합니다. `KernelModelIRPStack`은 일반 완료, MPR, 최종 콜백을 소유하며 실제 유지된 미완료 패킷만 대기 검사에서 제외합니다. `KernelModelPowerEvents`는 타입이 있는 프레임워크, 네이티브 `(PDO, START, IRP)`, PoFx 대상을 캡처합니다. 오래된 이벤트는 대체 IRP로 옮겨지지 않고 성공과 취소는 같은 소유권을 경쟁합니다. 전원 전환이나 WDM 부모 전파는 추론하지 않습니다. 하위 START 성공 후 안정 D0에서 Working/Sleeping3를 받고 WAIT_WAKE 발행은 PASSIVE_LEVEL이며 WDF 경로의 네이티브 발행은 할당 전에 거부합니다.

`PowerRequestDelivery`는 Inline／Queued를 구분합니다. `planPowerRequest`가 경로, 작업, 수명을 순수 검증하고 저장소와 용량 확인 후 `commitPowerRequest`가 실제 IRP, 전원 티켓, 경로 참조를 예약합니다. `DeviceLifecycle::validateSystemPowerRequest`는 begin과 검증 및 티켓 한도를 공유합니다. 상승 IRQL Query/Set은 즉시 게스트를 호출하거나 IRQL을 낮추지 않습니다. `WDMDispatch`는 PASSIVE 작업 FIFO에 실제 PC를 넣고 `WDMProviderDispatch`는 실행 PC 없는 타입 지정 내부 작업으로 필요하면 같은 슬롯을 `WDMCompletion`으로 바꿉니다. PowerDispatch와 `ScheduledModelContinuations`를 재사용하며 게스트 작업 항목이나 추가 완료 슬롯을 만들지 않습니다. 캡처 경로는 detach／논리 삭제 후에도 완료까지 유지됩니다.

`DriverUsbIdle.def`는 공개 이름을, `KernelUsbIdleValues.def`는 WDK ABI를 통합합니다. `KernelUsbIdle`은 정확한 IRP／START, info 대여, 콜백／D2 인과와 첫 완료 원인만 소유하며 기존 IRP·토폴로지·수명 권한을 재사용합니다. `KernelModelUsbIdle`은 실제 패킷과 전체 복합 구성원／용량을 사전 검사하고 `GuestCallOwner::UsbIdle`의 PASSIVE 콜백을 게시합니다. 전용 provider 취소는 대기 콜백을 제거하고 진입한 콜백은 반환까지 기다립니다. `KernelModelUsbIdleReceipt`는 IoCompletion／MPR 후 원래 수신 작업을 재개하며 provider 전용 전원 작업도 보존합니다. 수신과 하드웨어 확인은 독립적이고 이전 등록은 게스트 완료 전에 폐기하여 재등록을 지우지 않습니다. 보고는 실제 증거만 기록하고 단계는 `UsbIdleCallbackPhase`입니다.

`KernelModelFrameworkUsbIdle`은 실제 프레임워크 패킷/info의 저장과 폐기를 맡고 `KernelUsbIdle` 프로토콜 소유권을 재사용합니다. 네이티브 콜백은 형식화된 provider binding이며 게스트 실행 코드가 아닙니다. `FrameworkUsbIdle` 작업은 같은 슬롯에서 실제 WDF 콜백으로 전환되며 D2 완료·콜백 반환·패킷 완료는 각각의 식별자를 유지합니다. 명시적 DeviceWake로 Maximum을 결정하고 정확한 key/epoch를 보존하며 활동·제거 전에 취소합니다. 복합 배치를 모두 사전 검사하고 직접/전달 관리 큐 모두 실제 D0 확인과 D0Entry 뒤에 요청을 표시합니다.

`KernelFramework::Device`는 물리 `PowerQueuesHeld`와 `PoFxComponentHeld`를 분리하고 `queuesHeld()`에서 전달 조건만 합칩니다. Required 응답은 실제 D0를 검사하며 응답으로 활성화할 구성 요소를 순환 대기하지 않습니다. `CompletePowerNotRequired`는 USB 보관 또는 D0의 명시적 거절 후 정확한 PoFx 콜백 소유권을 종료합니다. 비 USB는 실제 Dx 완료를 기다립니다. USB 허가는 정확한 IRP／START를 유지합니다. `DispatchQueues`는 게시 전에 검증하고 `WdfDeviceEnqueueRequest`가 당시 큐를 캡처하고 연속 실행은 경로를 보존하며 실제 객체 부모 소유권을 옮깁니다. 매핑 변경은 새 요청에만 적용되며 기존 요청이나 IRP 소유자를 복제하지 않습니다. `RemovePending` D0 실패는 `PoFxQuiesce`의 실제 게스트 실패와 정확한 반환 완료 Required 토큰을 근거로 quiesce／응답을 원자적으로 수행합니다. 원래 IRP 실패와 나머지 정리를 유지하며 F0／ActiveCondition이나 성공 상태를 만들지 않습니다.

`DriverResources.h` / `DriverResources.def`와 `DriverInterrupts.h` / `DriverInterrupts.def`는 고정 `register_bank` 메모리 및 인터럽트 할당을 정의합니다. `DriverScenario`는 JSON／네이티브 사전 검증을 담당하고 `DriverResult`는 관측한 뱅크 상태를 복제하지 않고 초기 설정을 기록합니다. packed raw／변환 후 할당, 리소스 세대, 물리적 존재와 전원은 `KernelResources`만 관리합니다. `KernelMMIO`는 지속 레지스터 값과 독립 매핑 별칭을, `KernelInterrupts`는 불투명 연결, 잠금과 명시적 펄스를 관리합니다. `KernelModelResources`는 읽기 전용 START 패킷을 만들고 실제 제공자 완료를 통합합니다. 하위 START 성공은 상위 콜백 전에 세대를 공개하고 실제 제공자 장치 SET 완료가 D0／D3 접근성을 갱신합니다. START 실패 또는 STOP／REMOVE 해제는 상위 콜백이 unmap과 연결 해제를 수행할 수 있도록 최종 IRP 완료 전에 검사하며 암묵적인 정리는 없습니다. 갑작스러운 제거는 즉시 하드웨어 접근을 거부합니다. `GuestMemory`와 `UnicornBackend`는 MMIO 효과 전에 CPU／API 트랜잭션 전체를 검증하고 최초 결함을 보존합니다. 고정 할당으로 다시 시작하면 뱅크 값이 유지됩니다. 임의 RAM, 리소스 재배치, 포트, 공유／레벨／메시지 인터럽트와 기타 DMA 인터페이스는 계속 지원하지 않습니다.

`KernelInterrupts`는 각 명시적 펄스를 원본 요청 제출이 성공할 때 존재한 연결 토큰과 리소스 세대에 결합합니다. `KernelModelInterruptEvents`는 시계 진행이나 관측값 변경 전에 같은 시각의 모든 생산자 용량을 사전 검증하며 프레임워크 취소 콜백 수도 정확히 계산합니다. 타이머, 제공자 완료와 취소가 ISR에 예약한 공간을 조용히 소비할 수 없습니다. 실제 제공자 하드웨어 상태 공개 후 펄스 전달 가능 여부를 판단하며, 허용된 인터럽트가 DPC와 수동 수준 콜백보다 먼저 실행됩니다. 스케줄링은 협력 방식이며 가상 시간은 유휴 상태에서만 진행합니다. 지연 0은 명령어 단위 선점을 의미하지 않습니다. `KernelModelInterrupts`는 기존 11인수 ABI와 선택된 Ex 필드를 해석하고 `KernelGuestCall`은 인터럽트 콜백에 별도 소유자／토큰을 부여합니다. ISR과 동기화 콜백은 할당된 DIRQL에서 같은 비재귀 잠금을 보유합니다. 중첩 CPU 프레임은 호출자의 IRQL／CR8을 보존하고 BOOLEAN은 AL만 사용합니다. 수동 잠금에는 같은 실행 식별자와 저장된 IRQL이 필요하며, 콜백은 잠금을 보유한 채 반환할 수 없습니다. 설정된 펄스는 원본 IRP가 완료된 뒤에도 남습니다. 연결 해제, 사용할 수 없는 세대 또는 D3에서 전달을 시도하면 명시적 미전달 이유를 기록하고 중단하며, 새 연결에 재결합하거나 enable／ack 레지스터 동작을 추측하지 않습니다. `DriverResult.Interrupts`에는 독립 관측만 기록하며 합성 IRP나 NTSTATUS 완료로 표현하지 않습니다.

`DriverDMA.h` / `DriverDMA.def`는 PDO별 명시적 기능과 독립 외부 트랜잭션을 정의합니다. `KernelPhysicalMemory`는 정확한 유효 RAM 할당을 등록하고 공유 페이지 식별자를 부여하며 바이트 범위를 고정합니다. MDL은 이 권위 있는 저장소의 뷰이며 버퍼 복사본이 아닙니다. `GuestMemory`／`UnicornBackend`는 CPU 권한을 변경하지 않고 우회하는 전체 범위 기반 RAM 접근을 제공합니다. MMIO, 실행 중, 재진입 또는 장애 상태의 접근을 거부하며 예상 밖 백엔드 실패를 유지합니다. `KernelDMA`는 독립 논리 도메인, 어댑터에 연결된 메서드 식별자, 공통／SG 매핑, 매핑 레지스터 수용, 콜백 참조를 관리하며 `KernelDMAEvents`는 실제 전달 시 캡처된 PDO 세대를 확인합니다. `KernelModelPhysicalMemory`, `KernelModelDMA`, `KernelModelDMATransfers`는 원래 할당／MDL 소유권을 실제 간접 게스트 콜백에 연결합니다. SG 자원이 있으면 인라인으로 전달하며 대기 콜백은 FIFO 승격까지 식별자와 용량을 예약합니다. 매핑과 콜백 수명은 별개입니다. Put은 콜백 반환 전에 데이터／설명자 고정을 해제할 수 있고 콜백 유지는 완료된 IRP를 살려 두지 않습니다. `DmaWritable`은 CPU 매핑 권한과 독립적으로 잠금 의도를 기록합니다. 같은 시간에는 제공자 공개, DMA RAM 효과, 인터럽트 자격 검증 순서입니다. 리소스 세대／존재／전원은 오직 `KernelResources`가 관리합니다. DMA는 제조사 레지스터를 추론하거나 IRQ를 일으키거나 IRP를 완료하거나 별도 수명 주기를 소유하지 않습니다. 논리 주소는 재사용하지 않으며 트랜잭션 검증 실패는 RAM을 바꾸지 않고 관측을 보존합니다. 모델링한 인터페이스는 제한된 모델 RAM의 일관성 있는 공통 버퍼, V1 SG DMA와 변환된 버스 마스터 채널 DMA를 포함합니다. 일반 하드웨어, 종속 컨트롤러, 기타 DMA 인터페이스는 지원하지 않습니다.

`KernelDMAChannels`는 같은 도메인 할당자에 채널 예약과 종류를 구별하는 SG／채널 FIFO를 추가합니다. 콜백 상태, 보유 매핑 레지스터, 각 작업의 전체 매핑을 분리합니다. 채널 작업은 논리 범위를 한 번만 예약하고 단일 물리 고정 범위를 제자리에서 확장하므로 MapTransfer 호출이 교차해도 RAM을 복사하거나 레지스터를 이중 차감하거나 다른 매핑과 겹치지 않습니다. 순수 전송／반환／플러시／해제 계획은 공개 전에 식별자와 승격 배치 전체를 검증합니다. `KernelModelDMAChannels`는 간접 ABI와 등록 시 실제 CurrentIrp 스냅샷을 해석하고 SG와 MDL 뷰 헬퍼를 공유합니다. 스케줄러의 별도 종류 `DMAAdapterControl`은 DMA 순서, 용량, 인라인 부모 보존을 공유하며 DMA 모델만 반환값 하위 32비트 동작을 해석합니다. 큐에서 캡처한 IRP는 최종 스택 해제 전에 보호되며 콜백 진입 시 이 입력 참조를 해제하므로 콜백 본문에서 완료할 수 있습니다. 전체 플러시는 매핑 바이트를 무효화하고 정확한 FreeMapRegisters는 별도 예약을 해제합니다. `KeFlushIoBuffers`의 일관성 캐시 계약은 두 해제 의무 중 어느 것도 대신하지 않습니다.

`KernelFramework`는 KMDF 1.33 바인딩, 함수 테이블 식별, WDF 객체와 컨텍스트, 제어 장치 초기화 레코드, 기본 및 비기본 수동·순차·병렬 큐(유한 또는 무제한 병렬 처리)와 요청 핸들을 관리합니다. 형식화된 장치 및 요청 호스트 인터페이스를 통해 WDM 네임스페이스, 저장 공간, 패킷 상태, MDL 매핑과 완료 검증을 `KernelModel`에 위임하며, 어느 쪽도 중복 장치나 IRP를 만들지 않습니다. 큐 라우팅은 프레임워크가 관리하는 디스패치 상태를 반환형이 `void`인 게스트 콜백의 복귀와 별도로 유지합니다. 완료 연속 작업은 버퍼가 유효한 동안 정리하고 원래 IRP를 완료한 뒤 페이지 고정을 해제하며 메모리 별칭을 무효화합니다. 참조가 허용할 때 자식 객체를 파괴하며 외부 참조는 WDF 컨텍스트만 유지합니다. 대기 중인 요청 삭제는 상위 객체를 변경하기 전에 거부합니다. 삭제 시 자동 취소나 큐 비우기는 여전히 지원하지 않습니다. `DriverSession`은 공유 실행 예산 안에서 중첩 콜백을 실행합니다. `DriverImage`는 CFG 메타데이터를 검증하고, `GuardControlFlow`는 선언된 이미지/API 대상을 관리하며, CPU 어댑터는 check/dispatch 호출 상태를 보존합니다. PnP는 리소스가 없거나 `register_bank`를 구성한 직접 FDO/PDO 범위를 지원합니다. 프레임워크는 AddDevice 초기화자, WDF 객체 그래프, PrepareHardware／ReleaseHardware, D0Entry／D0Exit, QueryStop／QueryRemove／SurpriseRemoval 콜백과 제한된 리소스 목록을 소유합니다. `KernelModelPnpDevices`는 PDO 식별과 공급자 해제를 담당합니다. `KernelModelIRPStack`은 하위 버스에서 완료한 원래 PnP IRP를 순서대로 실행한 콜백이 반환할 때까지 보관하고, `KernelModelFramework`는 하드웨어나 D0 콜백의 실패 상태를 유지하며 완료를 재개합니다. 다른 리소스 유형, 더 넓은 KMDF PnP 계약, 클래스 확장 및 UMDF는 지원하지 않습니다.

시나리오의 `cancel_after_100ns`는 전송 요청의 가상 취소 기한을 지정합니다. `KernelModel`의 IRP별 레코드가 기한과 실제 절대 시간 `cancel_requested_at_100ns`를 관리하며 공개 시나리오는 기본적으로 순차 처리하며 명시적인 `defer_callback_drain`으로 지원되는 요청을 제한적으로 일괄 제출할 수 있습니다. `KernelModel`은 라우팅 후 게스트 I/O 콜백 전에 0 지연 취소를 적용하고 완료가 먼저 발생한 결과를 보존하며, 유휴 시간 진행에 양수 취소 기한을 포함합니다. `KernelFramework`는 표시/해제, 큐에 들어감/전달됨 상태와 취소 콜백 반환까지 유지하는 내부 참조를 관리합니다. 큐에 들어간 것만으로는 완료할 수 없고, 전달 후에는 콜백이 대기하는 동안 작업 항목과 협력해 완료할 수 있습니다. WDF 수명 유지는 무효화된 IRP 저장 공간을 복구하지 않습니다. `KernelScheduler`는 취소 콜백과 작업 항목을 분리하고 중단/복원 시 종류를 유지하며 용량과 디스패치 예산을 공유합니다. 취소 콜백은 큐 실행 수준에 따라 `PASSIVE_LEVEL` 또는 `DISPATCH_LEVEL`에서 실행하며 차단 대기는 `PASSIVE_LEVEL`에서만 허용합니다. 이 제어 장치 계약은 WDM 취소 루틴이나 일반 큐 스케줄러를 제공하지 않습니다.

이전 `WdfRequestMarkCancelable`은 이미 취소된 IRP에서 현재 API 연속 실행의 중첩 `GuestCall`을 사용합니다. 취소, 정리와 최종 파괴 콜백은 대기할 수 있고 전체 연속 실행이 끝나야 호출자를 재개합니다. 등록 후 취소는 스케줄러를 사용합니다. `KernelFramework`는 WDF 핸들 식별과 완료 중/후의 정의된 getter 반환값을 관리합니다. 요청 접근 호스트는 원래 IRP, 64비트 Information과 MDL 식별을 `KernelModel`에 위임하며, 이 모델은 프레임워크 소유 IRP를 게스트 WDM으로 완료하는 것도 거부합니다. 요청마다 SystemBuffer MDL 하나를 필요할 때 만들고 직접 버퍼는 기존 설명자를 유지하며 검색만으로 매핑하지 않습니다. 완료 시 두 종류 설명자와 IRP/버퍼가 함께 무효화되며 WDF 컨텍스트 참조 유지와는 독립적입니다.

`KernelGuestException`은 32비트 상태를 담은 형식화된 API 결과이며 모델 오류 및 백엔드 오류와 구별됩니다. `DriverImage`는 로더의 기존 기본 주소 기반 예외 메타데이터를 보존합니다. `KernelSEH`는 검증된 주소 변환과 스택 읽기로 해당 메타데이터에 대해 순수하고 제한된 x64 V1 C catch-all 전환 계획을 만듭니다. 일반 헬퍼 프레임을 거쳐 지원하는 비휘발성 GPR 저장값을 복원하고 실제 게스트 처리기를 선택합니다. 경로에서 만나는 필터／finally, GS／C++ 성격 처리기, 체인, 불완전한 레코드, 프롤로그 및 XMM 복원은 거부합니다. `DriverSession`은 정상 API 중지 경계에서만 검증된 레지스터 계획을 적용하며 API 추적 결과를 null로 두고 같은 실행 안에서 처리기를 재개합니다. 보존된 백엔드 오류를 지우거나 다른 콜백 스택까지 해제하지 않습니다. 이 경계는 ExRaiseStatus／ExRaiseAccessViolation／ExRaiseDatatypeMisalignment를 지원합니다. 사용자 접근 검사, 잠긴 사용자 버퍼와 CPU 오류 복구는 별도 작업으로 남습니다.


## 예외 재작성 경계

Mach-O compact unwind에는 원본 `__unwind_info`용 strict parser, 생성된
`__LD,__compact_unwind` record용 fixup-aware parser, 원본/생성 range의 정확한 merge,
regular page용 deterministic encoder와 transactional 최종 section installer가 있습니다.
installer는 기존 file-backed `__TEXT,__unwind_info`가 encoded table을 수용할 때만
in-place로 다시 씁니다. architecture, layout, byte preimage를 재검증하고 사용하지 않은
tail을 0으로 지운 뒤, 바깥 Mach-O transaction이 한 번 commit되기 전에 결과를 다시 parse해
semantic equivalence를 확인합니다. 생성 record는 compiler가 정확히 기록한 IR source
function→target MC owner symbol 매핑(private definition 포함, object-format prefix나 mangling
추측 없음), opaque한 0이 아닌 range ID, 정확한 반개구간 fragment range로 인증됩니다. 생성된
각 FDE는 정확히 하나의 인증된 fragment와 일치해야 하고, 필요한 각 fragment도 해당
transaction이 install한 정확히 하나의 FDE와 일치해야 합니다. 단, 엄격한 encoding 검증을
통과한 정확한 non-DWARF compact record가 덮는 경우는 예외입니다. 같은 function 소유의
인접하거나 떨어진 fragment는 하나의 source recipe를 재사용할 수 있지만, 누락·중복·dangling·
cross-owner 또는 boundary 불일치 identity는 출력 변경 전에 실패합니다. 새 RX segment는
`__LINKEDIT`가 유일하고 file/VM 끝에 있으며 offset shift가 overflow checked이고 최종 file/VM
layout의 strict replay가 성공했음을 증명한 뒤에만 commit됩니다. 최종 section이 없으면 생성된
compact record를 install하지 않고 아래의 정확하고 인증된 DWARF-FDE 폐쇄를 통과할 때만
transaction을 계속할 수 있습니다. 기존 최종 section의 용량이 부족하거나 malformed이면 계속
fail closed하며, link된 native throw/catch 증명은 아직 남아 있습니다.

외부 참조는 완전한 MC fixup 계약으로 분류합니다. call은 인증된 callable target만 선택할 수
있고, 생성 compact-unwind의 personality field는 검증된 non-lazy pointer slot만 선택하며 그
파일 내용을 dereference하지 않습니다. TLS, authenticated pointer, subtractive, malformed
compact field 및 알 수 없는 relocation은 fail closed합니다.

ARM32 compact unwind에서 encoding에 포함된 stack adjustment와 GPR layout은 `Complete`입니다.
D-register pattern selector 0~3도 `Complete`이지만, 4~7은 compact word만으로 runtime-aligned
CFA-relative slot을 모두 증명할 수 없어 `Partial`입니다. `Partial` entry는 분석을 위해 증명된
register identity를 유지할 수 있지만 모든 rewrite path가 fail closed로 거부합니다. 각
EH-frame install receipt는 target architecture, pointer width, byte order를 정확히 결속하며,
compact-unwind DWARF binding은 receipt target identity 불일치를 거부합니다.

상위 ARM32 section transaction의 범위는 compact-unwind decoder보다 좁습니다.
Mach-O header가 정확히 `CPU_SUBTYPE_ARM_V7K`이고 원본 symbol table의
`N_ARM_THUMB_DEF` bit가 필요한 모든 function을 Thumb code로 명시적으로 인증할
때만 이 경로가 활성화됩니다. 이후 정확한 `thumbv7k-apple-watchos` triple과 Thumb
mode가 code generation 전체에 결속되며, 입력 feature 요구 사항은 Cortex-A7 한계를
초과할 수 없습니다. flag가 없거나 mode를 알 수 없는 function, generic non-v7k
subtype, ARM mode, 혼합되거나 알 수 없는 external-code target, ARM Mach-O in-place
entry point, C source 기반 ARM Mach-O patch는 출력 변경 전에 모두 fail closed로
거부됩니다. function discovery가 `LC_FUNCTION_STARTS`에만 의존하는 stripped input은
아직 지원하지 않습니다.

PE, ELF, Mach-O에는 각각 format별 예외 component가 있지만 NeverD는 모든 format과 모든
exception type을 포괄하는 end-to-end rewrite pipeline을 아직 공개하지 않습니다. 지원하지
않는 encoding 또는 해결되지 않은 registration/layout 요구 사항은 출력 변경 전에
실패해야 하며, 현재의 부분적인 format 지원을 완전한 예외 재작성 폐쇄로 표현하면 안 됩니다.

Ada 또는 D Itanium personality를 인식하는 것은 Ada/D 예외 지원이 아닙니다. GNAT,
GDC, DMD, LDC의 address-form LSDA는 파싱할 수 있습니다. type-table 슬롯은 불투명하게
유지되며(GNAT는 `Exception_Id` / `Exception_Data`, D는 `ClassInfo`)
`std::type_info`로 따라가지 않습니다. native reconstruction은 LLVM `personality`와
address-form `invoke`/`landingpad` 절을 방출합니다. corpus-proven은 별도의 주장이며
personality 인식이나 native lowering에서 추론하면 안 됩니다.

## 구성 요소 맵

실험적 모바일 CLI는 `tools/neverd/mobile`에서 APK/DEX 클래스 목록과 코드 참조 쿼리를 담당합니다.
두 기능 모두 복원과 같은 DEX 외곽 구조/MUTF-8 판독기 및 ZIP 메타데이터 검증기를 사용합니다.
클래스 목록은 클래스 식별 정보만 구체화합니다. 참조 쿼리는 기존 명령 디코더에서 풀 피연산자를 관찰하고
클래스/멤버 소유 관계 및 코드 흐름 검증을 공유하며, 별도의 명령 너비 디코더는 없습니다.
디코더는 두 모드 모두에서 간결한 흐름 정보를 만들며, 복원은 추가로 소유권을 가진 명령을 생성합니다.
쿼리는 모든 피연산자를 검증한 뒤 선택된 참조 피연산자만 유지합니다. 비공개 멤버 풀 항목은 완성된 불변 식별자 테이블을 빌려 쓰며,
복원 모델과 참조 결과는 소유권을 가진 멤버 데이터를 명시적으로 구체화합니다. 프로토타입 항목도 검증된 타입 목록을 빌려 씁니다.
두 표현은 같은 정규 메서드 식별 정보 포매터와 인코딩된 접근 플래그 검증기를 사용합니다.
디코더는 비공개 분기 간선을 명령 순번으로 한 번만 해석하며, 공개 복원 대상은 code unit 단위의 PC를 유지합니다.
쿼리는 검증된 클래스 테이블을 재사용하고 map 섹션별로 항목 범위를 모아 물리적 항목의 순서가 달라도 공개 전에 겹침을 검사합니다.
처리량은 즉시 예산에 반영합니다. 범위가 제한된 스칼라 읽기, 짧은 비교, 명령 단계는 마감 시간 검사 지점을 공유하고 큰 작업은 직접 검사합니다.
디버그 스트림은 각 code item의 프레임과 범위에 대해 검사하며 컨텍스트와 무관한 성공 플래그를 캐시하지 않습니다.
공유 물리 code item의 간결한 참조 위치는 모든 소유자에 대해 다시 적용합니다. 쿼리 일치 계층은 리터럴 대상 선택을 담당하며,
컨테이너 연결 계층은 손실 없는 UTF-16 문자열 단위를 포함한 DEX 간 집계와 JSON 공개를 담당합니다.
`visitZipMembers`는 모든 멤버 메타데이터와 선택된 페이로드 전체를 검증한 뒤 메모리에서 방문하며,
`extractZip`는 전체 아카이브의 페이로드 검증을 유지합니다. 쿼리 결과는 공개 전에 모으며 선택되지 않은 페이로드의 무결성은 명시적으로 제외합니다.
클래스 목록은 메서드 본문을 제외합니다. 참조 쿼리는 정의된 모든 본문을 검증하지만 애너테이션이나 Java 복원 검증을 주장하지 않습니다.
두 경로 모두 네이티브 바이너리 SDK의 형식 계약을 확장하지 않습니다.

각 구성 요소는 `add_neverd_component_library`가 만드는 정적 archive입니다. 표에는
주요 NeverD 의존성만 나열하며 CMake helper가 공통 제공하는 LLVM과 Capstone
라이브러리는 모두 열거하지 않습니다.

| 디렉터리 | 책임 | 주요 의존성 |
|----------|------|-------------|
| `lib/loader` | 포맷 감지, PE/COFF·ELF·Mach-O 로드, 정규화된 `BinaryImage`, 함수 탐지 | LLVM Object API |
| `lib/lift` | 수작업 x86/i386·AArch64·ARM32 명령어 의미론 | IR 데이터 타입 |
| `lib/decode` | Capstone/native 디코드 및 아키텍처 lifter로 디스패치 | `NeverDIR`, `NeverDLift` |
| `lib/ir` | 공통 타입과 LowIR·MedIR·HighIR·intrinsic 정의/변환 | 네 IR 하위 구성 요소 |
| `lib/pipeline` | 함수 감지와 Low/Med/High/LLVM 경로 조정 | IR, decode, lift, LLVM backend, 디버그 정보, IR pass |
| `lib/backend/c` | HighIR-to-C 및 LLVM-IR-to-C 렌더링 | IR |
| `lib/backend/llvm` | MedIR-to-LLVM lowering | IR |
| `lib/backend/codegen` | 대상 코드 생성과 PE/ELF/Mach-O patch 및 in-place 재작성 | IR, loader |
| `lib/sdk` | 공개 C ABI, session 수명 주기, query, 지속성, 플러그인, lift/decompile/patch/audit/hunt 진입점 | 엔진 구성 요소를 `libneverd`로 집계 |
| `lib/pass` | LLVM IR 난독화 pass와 MIR pass runner | IR |
| `lib/debug` | DWARF, PDB, linker-map 디버그 context | IR |
| `lib/sigs` | 시그니처 파싱, 데이터베이스, 매칭 | Loader |
| `lib/libc` | 알려진 libc 이름과 호출 모델 지원 | 독립 구성 요소 |
| `lib/safety` | 리프트된 IR 위의 힙 수명 감사와 복사 오버플로 헌트 | Symbolic, Solver |
| `lib/support` | 공유 바이너리 로드 helper | Loader |
| `lib/translate` | version이 있는 guest state/policy/exit, 고정 runtime ABI, 검사된 guest memory, 생성 IR/object/LinkGraph audit, sealed native linking, experimental x86-64-to-AArch64 C++ dispatcher | IR, LLVM, LLVM Object 및 JITLink 계약 |

`lib/pass/ir/simplify`의 `ByteMemoryForwardingPass`는 단일 기본 블록에서 고정 바이트 alloca의 각 바이트를 마지막으로 쓴 값으로 완전한 정수 로드를 재구성합니다. 대상 바이트 순서에 따라 8~128비트의 바이트 배수 폭과 객체 내부의 정확한 상수 GEP만 허용합니다. 호출, 알 수 없는 쓰기, 순서가 지정된 메모리 접근은 기록을 지웁니다. 기존 전용 주소 복원 후 두 SROA 사이에서 실행하며 원래 store를 유지합니다. 명령 스캔, 주소 탐색, 추적 바이트, 교체할 사용 지점, 추가 IR에는 유한 예산이 있습니다. 기본값은 스냅샷을 추가하지 않습니다. 명시적 `AllowStoreSnapshots`는 store 직전에 한 번 freeze하여 쓰기와 모든 조각이 같은 값을 공유하게 합니다. 이 선택적 LLVM 정제는 네이티브 값의 정의성이나 함수 시그니처를 증명하지 않습니다.

의미 단순화 고정점 파이프라인은 `SimplifyNumericMemory`도 활성화합니다. 블록 내 integral AS0 정수-포인터 변환에서 포인터·인덱스·피연산자가 모두 같은 32/64비트 폭이고 정확히 같은 SSA 루트와 모듈러 상수 오프셋을 갖도록 요구합니다. 하나의 마지막 쓰기에서 정수 전체 또는 한 번의 시프트와 절단으로 포함된 부분을 전달하며 새 스냅샷은 추가하지 않습니다. 두 번째 순회는 남은 읽기, 호출, 예외 발생 가능 연산 또는 순서 연산 전에 후속 쓰기가 전체를 덮는 경우에만 이전 쓰기를 제거합니다. 다른 루트도 별칭일 수 있고 마지막 쓰기는 관찰 가능합니다. 바이트 캐시 연산은 유한한 `MaxMemorySteps` 예산을 공유합니다. 이는 전용 메모리, 블록 간 메모리 내용나 일반 ABI의 증명이 아닙니다.

블록 내부 순회 전에 유한한 고정점 분석으로 전체 폭 정수 및 포인터 PHI/select를 같은 함수 진입 값과 모듈러 상수 오프셋으로 정규화할 수 있습니다. 역방향 간선을 포함한 모든 입력 간선이 일치해야 하며, 뒤늦게 발견된 충돌은 변경 전에 관련 후보를 철회합니다. 오프셋은 현재 IR에서 유도하며 바이너리 서명에 의존하지 않습니다. 비트를 잃는 변환과 freeze를 통과하지 않고, undef/poison 간선이나 진입 기준이 없는 순환은 동등성 증거가 아닙니다. 메모리 내용은 블록 사이로 전파하지 않습니다. 증명 작업은 `MaxAddressSteps`로 계산하며 각 주소 변경의 전체 `MaxNewInstructions` 비용을 먼저 확보합니다. 고정점이 미완료이면 부분 사실을 공개하지 않습니다. 주소만 바뀌어도 `CanonicalizedAddresses`가 기존 분석을 무효화합니다.

공개 헤더는 `include/neverd` 아래에서 이 영역들을 반영합니다. 내부 C++ 클래스가
실수로 SDK의 일부가 되지 않게 하세요. 안정적인 외부 작업은 순수 C 헤더와 책임이
분명한 `lib/sdk/NeverDCAPI*.cpp` 파일 중 하나에 두어야 합니다.

## CPU 실행과 워크로드 경계

CPU 실행은 게스트 OS 및 이미지와 독립적입니다. OS 정책과 프로세스 진입은 전송 계층 및 ISA와 분리됩니다.

`NEVERD_ENABLE_SEMANTIC_TESTS`의 기본값은 `ON`이며 `unittests/semantic`의 테스트 그룹과 통합 실행 대상을 제어합니다. Unicorn 없이 네이티브 CPU 테스트를 빌드하려면 `BUILD_TESTING=ON`을 유지하고 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF`와 `NEVERD_EMULATION_BACKEND_UNICORN=OFF`를 설정합니다. 적절한 SDK 헤더가 있는 Windows ARM64/MSVC를 포함하여 네이티브 KVM/WHP 테스트를 계속 빌드할 수 있습니다. Windows ARM64에서 Unicorn을 활성화하려면 ARM64 LLVM-MinGW 도구 모음이 필요합니다. 이 빌드 분리는 ARM64 네이티브 실행 검증을 의미하지 않습니다.

| 구성 요소 | 책임 |
|---|---|
| `NeverDEmulationCore` | 메모리, fault, 레지스터, 공통 실행 루프 |
| `NeverDEmulationNative` / `NeverDEmulationUnicorn` | 네이티브 KVM/WHP/HVF 전송 및 이식 가능한 Unicorn 실행 |
| `NeverDEmulationArch` | ISA 허용, 아키텍처 상태, 페이지 테이블과 FP 배치 |
| `NeverDEmulationCPU` | CPU 구성과 백엔드 조합 |
| `NeverDEmulationABI` / `NeverDEmulationRuntime` | 정수 ABI, CPU 세션, 워크로드 예산 |
| `NeverDEmulationImage` | 로더 세그먼트 매핑 계획 |
| `NeverDEmulationLinux` / `NeverDEmulationProcess` | ELF 시작, Linux 서비스 정책, 프로세스 보고서 |
| `NeverDEmulation` | Windows 모델 및 드라이버 수명 주기 |

macOS의 네이티브 전송 [HVF](macos-hvf.md)는 `NeverDEmulationNative`가 담당합니다. Apple Silicon은 ARM64, Intel은 x86-64를 사용합니다. 호스트 ISA는 전송을 선택하지만 게스트 OS를 결정하지 않습니다. [Darwin 프로필](darwin-emulation.md)은 macOS, iOS 기기 및 iOS Simulator의 시작과 서비스를 별도로 정의합니다. 아직 남은 Linux ARM64 KVM 및 Windows ARM64 WHP 검증은 이미 확보한 macOS ARM64 HVF 증거와 구분합니다. 가용성과 전체 검증 상태는 HVF 가이드에서 명시합니다.

CPU factory와 기능 질의는 같은 `ExecutionConfiguration`을 사용하며 할당 전에 아키텍처, 권한, 주소 폭, 기능을 검증합니다. `ExecutionBudget`은 워크로드별 공유 명령/이벤트 계정과 절대 monotonic deadline을 소유하며 재개해도 예산이 초기화되지 않습니다. `ExecutionSession`은 CPU, hook, 대기 중 서비스/fault continuation을 소유합니다. 세션은 메모리와 예산을 공유할 수 있지만 실행은 협력식이며 병렬 SMP가 아닙니다. 대기 중 요청은 재개 전 정확히 한 번 소비해야 합니다. CPU 오류는 자원 정지보다 우선하며 설명되지 않은 엔진 정지는 성공이 아닙니다.

`ImageMappingPlan`은 기존 로더 세그먼트를 사용하고 header 재분석이나 import 해결을 하지 않습니다. 주소 공간을 게시하기 전에 전체 범위와 겹침을 검사합니다. 명시적 `linux-elf64-v1` 프로필은 초기 스택, 명시적 서비스 요청, 제한된 바이트 출력을 갖춘 x64/AArch64 freestanding ELF `ET_EXEC`와 static PIE `ET_DYN`을 시작합니다. 동적 링크, dynamic TLS, 시그널, OS 스레드, 미지원 서비스는 실패합니다. static TLS와 제한된 x64 SSE/SSE2는 지원합니다. KVM만으로 Linux를, WHP만으로 Windows를 추론하지 않습니다. [CPU 실행](cpu-execution.md) 및 [게스트 프로세스 에뮬레이션](process-emulation.md)을 참조하세요.

`driver-strict`는 일치하는 Linux x64 host의 KVM과 Windows x64 host의 WHP를 지원합니다. `auto`는 해당 native transport를, cross-ISA는 Unicorn을 선택합니다. 명시적 Unicorn과 기존 V1 API는 portable software profile을 유지합니다. native 실행은 진입 전에 canonical address와 instruction effect를 검증하고, hardware가 없으면 fallback 없이 실패합니다. 지원되지 않는 instruction/OS behavior는 명시적 오류입니다. Windows x64 네이티브 CI는 Unicorn을 비활성화하고 필수 검사 359개를 모두 통과합니다. CPU 검사 131개, 내장 이미지 26개·WDK 이미지 46개·시나리오 사례 40개를 기본 및 재배치 주소에서 실행한 드라이버 결과 224개, SEH 경계 검사 4개를 포함합니다 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). native ARM64 실기 증거는 아직 없으며, 임의 driver나 Android/Darwin 호환성을 의미하지 않습니다.

`DriverImage.def`는 엄격한 PE 검증의 크기·정렬 제한과 진단 문구를 선언하며, 포인터 너비는 `DriverProfile.def`에서 가져옵니다. `DriverImage.cpp`가 검증과 재배치를 담당하며 허용되는 이미지와 오류 메시지는 바뀌지 않습니다.

`executionCapabilities(Contract, ISA, Backend)`로 선택한 백엔드의 기능을 조회합니다. `NativeLegacyX64`는 네이티브 x64 드라이버 실행을 나타내며, `NeverDNativeDriverTests`는 기존 드라이버 모음을 검증합니다. 이 테스트는 Unicorn을 비활성화한 빌드에서도 실행할 수 있습니다.

Checked ARM64는 하나의 완전한 상태 커밋 경계를 사용합니다. `Registers.def`가 39개 스칼라 필드와 32개 128비트 벡터를 정의하며 `captureAArch64State`는 모든 읽기, 선언된 폭과 NZCV 정규화를 완료한 뒤 한 번에 게시합니다. Unicorn/KVM/WHP/HVF는 TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR, FPSR를 포함한 같은 상태를 전송합니다. 네이티브 어댑터는 CPACR_EL1로 FP/SIMD를 활성화합니다. 읽기 실패나 진입 취소 시 호출자의 전체 상태가 보존됩니다.

ARM64 KVM/WHP/HVF 초기화는 전용 `AArch64MachineProbe.def` 프로그램을 실행합니다. NOP, 양의 무한대 방향으로 반올림하는 FP32 덧셈, 두 레인의 SIMD 덧셈입니다. 각 단계에서 39개 스칼라 필드와 32개 벡터를 모두 비교하여 TLS, NZCV, 결과 상위 비트 초기화, FPCR/FPSR 보존 및 누적 상태를 확인합니다. 감독자 전용 모니터 메모리와 하나의 전체 마감 시간을 사용합니다. 이 검사는 제한된 초기화만 검증합니다. Linux ARM64 KVM과 Windows ARM64 WHP의 워크로드 검증은 아직 남아 있습니다. macOS 네이티브 결과는 [HVF 가이드](macos-hvf.md)에 기록되어 있습니다.

x64 KVM/WHP/HVF 네이티브 초기화는 비공개 supervisor 페이지에서 `X64MachineProbe.def`를 실행합니다. 하나의 기한 안에 NOP, 양의 무한대 방향으로 반올림하는 FP32 덧셈, 두 레인 SIMD 덧셈, FS/GS 로드와 CS/SS/CR8 읽기를 수행하며 각 단계에서 전체 스칼라, XMM, 물리 x87 및 제어 상태를 비교합니다. x64와 ARM64 검사는 물리 메모리의 독점 실행 임대를 요구합니다. `MemoryProjection`은 캐시 식별 정보(ISA, 주소 공간, 매핑 세대, 권한, 모니터 구성)와 ISA별 확정된 페이지 테이블 루트 이력을 소유합니다. 비공개 바이트를 다시 쓰기 전에 캐시를 무효화하므로 실패한 재구축의 부분 테이블이나 호출자의 오래된 루트를 재사용할 수 없습니다. 이 검사는 제한된 초기화만 검증합니다. Linux ARM64 KVM과 Windows ARM64 WHP의 워크로드 검증은 아직 남아 있습니다. macOS 네이티브 결과는 [HVF 가이드](macos-hvf.md)에 기록되어 있습니다.

공유 XSAVE 디코더는 표준 형식과 압축 형식의 SSE 초기 상태를 구분합니다. XSTATE_BV[1]이 0이면 두 형식 모두 XMM을 초기화하지만 표준 형식은 MXCSR을 읽고 검증하며 압축 형식은 MXCSR을 초기화합니다. `X64XsaveCases.def`는 독립적인 데이터 배치와 직접 작성한 호스트 XRSTOR 프로그램을 제공합니다. `X64XsaveTests.cpp`는 거부 시 상태의 원자성을 확인하고 호출자의 FP/SSE 상태를 보존하면서 두 형식을 실제 호스트 실행과 비교합니다. 호스트 아키텍처나 필요한 명령 기능을 사용할 수 없으면 명시적으로 건너뜁니다.

`X64FPState.def`는 압축 AVX, AVX-512, CET_U/CET_S, AMX 전송 배치와 구성 요소의 64바이트 정렬을 선언합니다. 존재하는 확장 데이터는 모두 0인 초기 상태여야 하며, 없는 구성 요소의 데이터와 정렬 패딩은 상태를 정의하지 않습니다. 배치 비트가 오프셋을 결정하고 알 수 없는 배치, 초기 상태가 아닌 데이터, 잘못된 길이는 공개 전에 실패합니다. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState`, `InitialWideComponentsDoNotHideFPState`가 872바이트와 10752바이트 WHP 패킷을 검증합니다. 이 전송 지원은 해당 확장 명령 실행을 허용하지 않습니다.

`WhpXsaveRegisters.def`는 이름이 있는 x87/SSE 제어 레지스터로 완전한 XSAVE 패킷을 보완합니다. 마지막 연산 코드와 명령/데이터 포인터를 명시적으로 쓰고 호스트에서 읽습니다. 패킷의 0 필드는 보완할 수 있지만, 0이 아닌 메타데이터 충돌이나 공통 제어값 불일치는 상태 공개 전에 실패합니다. `NamedMetadataRestoresOmittedPacketFields`는 전체 FP 데이터를 유지하면서 누락 필드를 검증합니다.

네이티브 `FOP/FIP/FDP`는 호스트 x87 저장·복원 규칙을 따릅니다. 마스크되지 않은 대기 예외가 없으면 AMD는 이 필드를 0으로 만들 수 있으며 스냅샷은 관측값을 유지합니다. `X64MachineProbe.def`와 정밀 NOP/컨텍스트 테스트는 일관된 대기 예외를 설정하여 모든 필드를 유효한 상태에서 차이를 숨기지 않고 비교합니다. 호스트 프로세스 FXRSTOR64/FXSAVE64 참조는 두 상태를 검사하며, 백엔드는 호스트 결과를 입력 메타데이터로 대체하지 않습니다.

공통 `encodeX64XsaveState` / `decodeX64XsaveState` 코덱은 표준·압축 FP/SSE 패킷, 물리 TOP 순환, 누락된 구성 요소의 초기 상태 및 원자적 검증을 소유합니다. WHP는 완전한 XSAVE API를 사용하며 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`를 우선하고 이전 XSAVE API를 호환 경로로 사용합니다. 이전 개별 x87 레지스터 인터페이스는 완전한 패킷을 대체할 수 없습니다. 초기 상태가 아닌 확장 구성 요소, 잘못된 헤더·제어 값 및 잘린 캡처는 명시적으로 실패합니다. WHP 매핑 실패는 진단을 위해 HRESULT, GPA 및 크기를 보존합니다.

`CheckedX64Instructions.def`는 기존 CPU 백엔드에서 8/16/32/64비트 부호 없는 `MUL`과 `CBW/CWDE/CDQE/CWD/CDQ/CQO`를 허용합니다. `NeverDX64IntegerTests`는 독립적인 `X64IntegerCases.def` 인코딩과 예상값을 사용하여 두 권한 수준에서 부분 레지스터 보존, 32비트 제로 확장, 곱의 상위·하위 결과, 정의된 CF/OF 및 부호 확장 시 플래그 보존을 검증합니다. 일반 RAM 곱셈은 전체 접근 범위의 권한 검사와 읽기 관찰 콜백을 유지하며, 오류나 관찰 콜백의 중지는 암시적 출력 레지스터와 PC를 보존합니다. 장치 피연산자는 지원하지 않습니다. checked Unicorn에서도 실행하며 사용할 수 없는 네이티브 백엔드는 명시적으로 건너뜁니다.

`X64BitInstructions.def`는 16/32/64비트 레지스터 및 일반 RAM의 `BT/BTS/BTR/BTC`를 허용합니다. 레지스터 비트 인덱스는 피연산자 너비의 부호 있는 값으로 전체 워드를 선택하며, 즉시값은 기준 워드 안에 머뭅니다. 주소 너비에 따른 절단은 FS/GS 기준 주소를 더하기 전에 적용됩니다. 프로세서가 CF와 쓰기 값을 제공하고, `RAMTransaction`은 관찰 콜백이 수락할 때까지 결과를 비공개로 유지합니다. 전체 범위 권한 검사는 독립 페이지 할당과 별칭을 포함하며, 중지·콜백 실패·페이지 접근 거부 시 원래 CPU와 RAM을 보존합니다. LOCK은 자연 정렬된 메모리 수정 형식만 허용하며 MMIO와 하드웨어 병렬 SMP는 지원하지 않습니다. `X64BitStringTests.cpp`는 독립 인코딩을 실제 x64 호스트 실행과 비교하고 음수 인덱스, 너비 절단, 페이지 경계 접근, 취소, 잘못된 LOCK 형식을 검사합니다. [Intel 명령어 참조](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)를 참고하세요.

`X64StringInstructions.def`는 일반 RAM의 8/16/32/64비트 `MOVS/STOS/LODS`를 관리하며, `CLD/STD`는 다른 플래그를 보존하면서 방향만 바꿉니다. REP의 각 요소는 관찰 전에 전체 피연산자를 검사하고 재개 경계에서 확정됩니다. 후속 오류가 발생해도 완료한 요소는 유지되며, 중지나 콜백 예외는 현재 요소를 변경하지 않습니다. FS/GS는 주소 폭을 자른 뒤 소스에만 더합니다. AL/AX 로드는 상위 비트를 보존하고 EAX 로드는 0으로 확장합니다. 32비트 주소 모드에서 반복 횟수가 0이면 카운터 상위 비트가 0이어야 하며 MOVS/STOS의 참여 주소 레지스터도 동일합니다. 그렇지 않으면 실제 CPU 구현마다 결과가 다릅니다. MOVS/STOS/LODS의 REPNE 형식과 STOS/LODS 장치 피연산자는 아직 지원하지 않습니다. `X64StringTransferTests.cpp`는 독립적인 호스트 명령으로 폭, 방향, 중첩, 0회 반복을 비교하고 권한, 별칭, 주소 순환, 오류와 재개도 검사합니다. 자체 WDK 리소스 드라이버는 `driver_resource_strings.def`로 STOS/LODS의 네 폭을 모두 실행합니다.

`X64StringInstructions.def`는 일반 RAM의 8/16/32/64비트 `CMPS/SCAS`와 `REPE/REPNE`도 관리합니다. 각 요소는 관찰 전에 전체 읽기 범위를 검사하고 여섯 산술 플래그를 갱신하며 첫 종료 조건에서 멈춥니다. 데이터 오류는 이번 연속 REP 시작 시점의 플래그를 복원하면서 완료된 포인터와 카운터 변경을 유지합니다. 공개 API로 재개하면 게시된 CPU 상태에서 다시 시작합니다. 중지와 관찰 예외는 현재 요소를 변경하지 않으며 조기 종료 후 다음 요소를 읽지 않습니다. FS/GS는 CMPS 소스에만 적용되고 SCAS는 누산기와 사용하지 않는 소스 레지스터를 유지합니다. 장치 피연산자와 모호한 32비트 0회 반복 상위 비트는 제외됩니다. `X64StringComparisonTests.cpp`는 독립 호스트 명령으로 플래그, 방향, 별칭, 주소 순환, 권한과 복구를 비교하고 Linux x64 신호로 실제 오류 시점의 레지스터를 검사합니다. 자체 WDK 리소스 드라이버는 `driver_resource_strings.def`로 네 폭의 두 조건 반복 형식을 실행합니다. [Intel 명령 참조](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)를 참고하세요.

`WhpResourceCache.h`는 논리 CPU 상태와 WHP 파티션을 분리합니다. 런타임은 활성 네이티브 파티션 하나를 유지하며 같은 CPU의 연속 단계에서 재사용합니다. CPU를 전환할 때 이전 파티션을 먼저 제거한 다음 매핑과 가상 프로세서를 다시 만들고 전체 상태를 복원합니다. 논리 CPU는 독립적인 `MemoryProjection` 뷰와 권위 있는 RAM을 유지합니다. 임대 획득은 취소와 현재 기한을 따르며 비활성 CPU를 제거해도 다른 CPU의 파티션은 제거되지 않습니다. x64는 호스트의 기본 XSAVE 기능 조합을 보존하고 `WHvGetPartitionProperty`로 실제 파티션을 검증하며 종속 기능을 지워 마스크를 축소하지 않습니다. 협력적 CPU 전환은 병렬 하드웨어 SMP를 제공하지 않습니다.

`CheckedAArch64Instructions.def`와 `AArch64InstructionEffects`는 EL0/EL1에서 제한된 기본 FP32/FP64 연산·비교·이동과 고정 폭 SIMD를 허용합니다. FPCR는 네 가지 반올림 모드, FZ, DN을 지원하고 FPSR는 누적 상태와 QC를 보존합니다. 미지원 제어·상태 비트는 변경 전에 거부합니다. FP16 연산, SVE/SME, 마스크되지 않은 예외, 선택적 확장과 목록 밖 형식은 명시적으로 실패합니다. Windows ARM64 드라이버 로딩이나 다른 OS 환경은 추가하지 않습니다.

`AArch64InstructionEffects`는 최대 128비트 피연산자의 스칼라·FP/SIMD 단일/쌍 RAM 범위를 소유합니다. 공유 주소 공간은 CPU 진입 전 모든 페이지를 검사하고 `RAMTransaction`은 선언된 전체 물리 쓰기만 커밋합니다. 128비트 쓰기는 실행 전에 두 개의 64비트 값으로 순서대로 관찰됩니다. 정지와 오류는 RAM, 벡터와 주소 갱신을 보존합니다. Xn/Vn 번호 중복은 유효하며 쌍 접근 범위의 주소 래핑은 거부됩니다. `NeverDAArch64MemoryTests`는 독립적인 `AArch64CrossPageCases.def`와 `AArch64VectorMemoryCases.def`를 사용합니다.

KVM x64는 진입 전마다 실제 특수 레지스터를 읽고 `KvmX64State.def`에 정의된 프로토콜 필드만 비교합니다. CR3, CPL, TLS, CR8 등의 값이 바뀌면 투영을 다시 설정합니다. 상태를 완전히 수집한 단일 단계 디버그 종료 후에만 실행 가능 상태를 재사용하며, 예외·취소·진입 실패 후에는 다시 설정합니다. `X64StateTransition`은 실제 CPU 읽기로 TLS, 권한 수준, CR8 변경과 반복 예외 및 취소를 검증합니다. KVM은 `X64HostRegisters.def`와 `X64FPState.def`에 따라 일반 레지스터와 전체 FP/SSE 상태를 마지막으로 완료 확인한 디버그 종료 상태와 비교하고 변경된 입력을 다시 설치합니다. 호스트 쓰기와 컨텍스트 복원도 비교에 포함되며 예외, 취소와 실패는 재사용을 무효화합니다. 단일 단계 설정과 실제 일반/FP 상태 읽기는 명령마다 수행합니다.

KVM x64/ARM64는 `KvmRunControl`을 통해 같은 전용 vCPU 작업 스레드에서 상태 준비, `KVM_RUN`, 상태 캡처를 수행합니다. `EINTR` 재시도에도 준비는 한 번이며 취소나 캡처 실패는 게시할 수 없습니다. `KvmAArch64Machine.cpp`의 주소 변환 유지와 전체 스칼라·벡터 전송도 하나의 단일 단계 기한을 공유합니다. 호출 스레드는 완료 확인 후 커밋하며 ISA 해석, RAM 트랜잭션, OS 정책과 관찰자를 담당합니다. 네이티브 ARM64 실기 증거는 아직 없습니다.

## strict lifting 계약

`Decoder`와 각 아키텍처 lifter는 strict 모드로 시작합니다. Capstone이 명령어를
디코드할 수 있지만 선택된 lifter에 구현이 없으면 lifter는
`UnliftedInstruction`을 던집니다. 예외에는 명령어 주소, mnemonic, operand 문자열이
기록되므로 미지원 의미론은 누락하거나 추측하지 않고 분명하게 실패해야 합니다.

내부 non-strict 경로는 `NdOp::NOP`을 출력하지만 이는 진단용 탈출구일 뿐 명령어의
허용 가능한 구현이 아닙니다. 기여자와 CI 테스트는 strict 모드를 유지해야 합니다.
strict 실패가 나타나면:

1. 가장 작은 아키텍처별 fixture로 재현합니다.
2. `lib/lift/<ISA>`에 누락된 의미론을 추가합니다.
3. `unittests/lift`에서 예상 LowIR 모양을 검증합니다.
4. 명령어에 관측 가능한 동작이 있으면 `unittests/semantic`에 Unicorn 차등 왕복을 추가합니다.

pipeline을 계속 진행하려고 `UnliftedInstruction`을 잡지 마세요. 새로운 의도적 근사는
명시적인 계약과 테스트가 필요하며 1:1 lifting인 것처럼 보여서는 안 됩니다.

## 포맷 및 ISA 소유권

입력 포맷 로직과 출력 재작성 로직은 의도적으로 분리되어 있습니다.

| 포맷 | 로드, 메타데이터, 입력 relocation | Patch 및 출력 relocation |
|------|----------------------------------|--------------------------|
| PE/COFF | `lib/loader/COFF` | `lib/backend/codegen/COFF` |
| ELF | `lib/loader/ELF` | `lib/backend/codegen/ELF` |
| Mach-O | `lib/loader/MachO` | `lib/backend/codegen/MachO` |

아키텍처 lifter는 `lib/lift/X86`, `lib/lift/AArch64`, `lib/lift/ARM`에 있습니다.
해당 공개 lifter/register 선언은 `include/neverd/lift`에 있습니다. 대상별 LLVM 출력과
코드 생성은 `lib/backend/llvm/<ISA>` 및 `lib/backend/codegen/CodeGen<ISA>.cpp`에
있습니다.

<a id="support-and-test-depth"></a>

### 지원 및 테스트 깊이

루트 지원 매트릭스는 각 셀이 구현되었다는 뜻입니다. 모든 opcode, ABI 경계 사례,
바이너리 제작 도구 또는 운영체제 버전을 빠짐없이 테스트했다는 뜻은 아닙니다. 명령어
의미론이 lifter의 구현된 coverage 밖에 있으면 strict 모드는 fail-closed로 중지합니다.

12개 포맷×아키텍처 셀 모두
`unittests/semantic/PatchFullSubstRTTests.cpp`에서 의미론적 재작성 backend
coverage를 갖습니다. 통합 깊이는 다음과 같습니다.

| 포맷 | x86-64 | i386 | AArch64 | ARM32 |
|------|--------|------|---------|-------|
| PE/COFF | 링크된 fixture | backend grid | 링크된 fixture | 링크된 Thumb fixture |
| ELF | 링크된 fixture + 의미론 왕복 | object pipeline + 의미론 왕복 | 링크된 fixture + 의미론 왕복 | 링크된 fixture + 의미론 왕복 |
| Mach-O | 링크된 fixture\* | PIC/no-PIC object pipeline\* | 링크된 fixture\* | backend grid |

- **링크된 fixture**는 대표 프로그램의 링크된 실행 파일에 대해 loader/pipeline과
  patch 동작을 검증합니다.
- **object pipeline**은 재배치 가능 object의 로드, 모든 IR 단계, 디컴파일을
  검증하지만 host linking과 patch된 바이너리 실행은 포함하지 않습니다.
- **backend grid**는 정확한 재작성 코드 생성 경로로 대표 IR을 컴파일하고 Unicorn에서
  동작을 비교합니다. 링크된 실행 파일에 해당 포맷 loader를 실행하지는 않습니다.
- `*` Mach-O 링크 fixture는 요청 대상을 생성할 수 있는 host toolchain에 의존합니다.
  현대 macOS는 과거 i386 실행 파일을 링크할 수 없으므로 i386은 PIC/no-PIC thin
  object와 재작성 grid를 사용합니다.

링크된 fixture 셀은 해당 대표 프로그램에 대해 가장 강한 포맷 통합 증거입니다.
object pipeline과 backend grid 셀은 부분적인 포맷 통합 coverage입니다. 아무 셀도
제한 없이 “완전히 테스트됨”이라 할 수 없으며 ISA coverage의 완전성을 주장하지 않습니다.

주요 근거는 링크된 ELF/PE fixture를 위한
[`PatchFormatTests.cpp`](../../unittests/lift/format/PatchFormatTests.cpp), Windows ARM 로드/
디컴파일을 위한
[`COFFARMFormatTests.cpp`](../../unittests/lift/format/COFFARMFormatTests.cpp), i386 thin
object를 위한
[`MachOI386RelocationTests.cpp`](../../unittests/lift/format/MachOI386RelocationTests.cpp),
링크된 Mach-O를 위한
[`X86_64_PipelineE2ETests.cpp`](../../unittests/lift/x86_64/X86_64_PipelineE2ETests.cpp)와
[`AArch64_PipelineE2ETests.cpp`](../../unittests/lift/aarch64/AArch64_PipelineE2ETests.cpp),
12셀 backend grid를 위한
[`PatchFullSubstRTTests.cpp`](../../unittests/semantic/probe/patchfull/PatchFullSubstRTTests.cpp)입니다.
명령은 [테스트 가이드](testing.md)를 참고하세요.

## 수정 위치

| 변경 | 시작 위치 | 최소 집중 검증 |
|------|-----------|----------------|
| 명령어 추가/수정 | `lib/lift/X86`, `AArch64`, `ARM`의 해당 파일; 디스패치 변경 시 공개 lifter 헤더 | `unittests/lift`의 아키텍처 테스트, `unittests/semantic`의 의미론 왕복 |
| `NdOp` 추가 | `include/neverd/ir/NdOps.h`, 이후 Low-to-Med, emitter/renderer, verifier/emulator, dump 점검 | `NeverDLiftTests` + 관련 `NeverDSemanticTests` 사례 |
| CFG 또는 함수 감지 변경 | `lib/ir/low`, `lib/loader/FunctionDiscovery*.cpp`, `lib/pipeline/PipelineFuncDetect.cpp` | lift CFG/jump-table 테스트 및 집중 의미론 변환 스위트 |
| PE 입력 relocation/unwind 규칙 추가 | `lib/loader/COFF` | `COFFARMFormatTests` 또는 새 집중 loader fixture |
| PE 출력 relocation/patch 규칙 추가 | `lib/backend/codegen/COFF` | `PatchFormatTests`, `RewriteCodegenRTTests`, PE backend grid |
| ELF/Mach-O 포맷 동작 변경 | 해당 `lib/loader/<Format>` 및/또는 `lib/backend/codegen/<Format>` 디렉터리 | 해당 포맷 테스트와 재작성 grid |
| MedIR/ABI 복구 변경 | `lib/ir/med` | 호출 규약 lift 테스트 + ISA 교차 의미론 왕복 |
| 구조화 제어 흐름 복구 변경 | `lib/ir/high` | `NeverDCFGLoopXformTests` 및 구조화 C 테스트 |
| LLVM 변환 추가 | `lib/pass/ir`, `include/neverd/pass/ir`의 공개 헤더, 노출 시 pipeline toggle | 집중 변환 스위트 + patch 출력 변경 시 `NeverDPatchFullTests` |
| C API 작업 추가 | `include/neverd/sdk/NeverDCAPI.h`, 집중된 `lib/sdk/NeverDCAPI*.cpp`, 상태에만 `SessionImpl.h` 사용 | SDK/CLI 의미론 테스트, `neverd_last_error` 및 할당 규약 유지 |
| CLI 명령 추가 | `tools/neverd/NeverDCLIOptions.cpp`, `NeverDCLI.h`, 집중된 `NeverDCmd*.cpp`, `neverd.cpp`의 디스패치 | `unittests/semantic/CLIEndToEndTests.cpp` 및 직접 CLI smoke test |
| 힙 수명 감사 또는 복사 오버플로 헌트 변경 | `lib/safety`, `include/neverd/safety`, `include/neverd/sdk/NeverDCAPISafety.h` | `NeverDSafetyTests` 및 `NeverDSafetyIntegrationTests` |
| 의미론 회귀 추가 | 집중된 `unittests/semantic/*Tests.cpp`; 새 파일을 `unittests/semantic/CMakeLists.txt`에 등록 | 테스트 바이너리를 빌드하고 `ctest -R`로 이름 있는 사례 선택 |

변경 범위를 좁게 유지하세요. 표현을 정의하는 파일은 해당 변환과 함께 바뀔 수 있지만,
큰 리팩터링을 균일하게 보이게 하려고 관련 없는 loader, lifter, backend를 수정하지 마세요.

소스 구조체 선언은 필드 배치를 ABI 분류와 별도로 보존합니다. Darwin ARM64에서는 같은 float 또는 double 멤버 1~4개를 포함한 중첩 구조체를 지원하며, 부동소수점 레지스터가 부족하면 인수 전체를 스택에 배치합니다. MedIR은 SSA 전에 물리 멤버를 연결하고 HighIR은 하나의 논리 인수나 반환값을 복원하며 C 출력은 배치를 검사합니다. Darwin ARM64와 x86_64는 64비트 정수 또는 포인터 1~2개로 구성된 중첩 구조체도 지원합니다. 구조체 전체가 스택으로 이동하면 ARM64는 해당 레지스터 뱅크를 소진 처리하고 x86_64는 남은 레지스터를 후속 인수에 사용합니다. 패딩, 압축 필드, 부동소수점과 정수의 혼합, 불완전한 구성 요소는 거부하며 바이너리 재작성 권한을 부여하지 않습니다.

Darwin ARM64의 고정 C 호출은 부호 있는 64비트 정수 3개로 구성된 자연 배치 구조체를 숨겨진 x8 포인터로 반환할 수도 있습니다. 공통 소스 ABI 계층이 이를 분류하며 Low→Med는 호출 전 포인터를 저장하고 하나의 논리 결과를 필드별로 호출자 저장 공간에 기록합니다. 일반 인수 레지스터는 이동하지 않으며 x0에는 반환값이 생기지 않습니다. 호출 분석은 결과 저장 공간의 기존 정보를 무효화합니다. 진입점 투영과 네이티브 상태 보존 증명에는 해당 저장 공간 증명이 없어 이런 반환을 거부합니다. 부호 없는 정수나 포인터를 포함한 세 워드 구조체, 세 워드 인수, x86_64의 간접 구조체 반환도 지원하지 않습니다. Objective-C 간접 반환은 selector 전체 조회와 일반 receiver 조회에서 계속 거부합니다. nil 메시지 전송이 기존 결과 버퍼를 변경하지 않기 때문입니다. receiver로 한정된 ARM64 호출은 receiver가 현재 메서드의 정확한 non-null self이고 x8이 완전한 비탈출 전용 프레임 범위를 가리킬 때만 고정 record ABI를 사용할 수 있습니다. 소스 공개 시 메서드 진입점, self 피연산자, receiver 선언, record 크기와 프레임 경계를 다시 검증하며, 증거가 없거나 바뀌면 메시지는 미해결로 남습니다.
Darwin ARM64의 고정 C 호출은 자연 배치된 double 여섯 개의 구조체도 숨겨진 x8 포인터로 반환합니다. 이는 레지스터로 전달하는 동종 부동소수점 집계의 네 멤버 제한을 넘습니다. double 여섯 개를 값으로 전달하는 매개변수는 일반적으로 아직 지원하지 않습니다. 정확히 가져온 arm64 CoreGraphics `CGContextConcatCTM`은 제한된 예외입니다. 두 번째 물리 인수는 48바이트 저장 공간을 가리키며, 생성된 도우미가 이를 값으로 전달할 C `CGAffineTransform`으로 복사한 뒤 원래 함수를 호출합니다. 바인딩은 정확한 강한 제공자를 요구하고 게시 시 재검증하며, 다른 간접 구조체 인수를 추론하지 않습니다.

동일한 소스 ABI 관리 계층은 자연 배치된 double 16개로 구성된 `CATransform3D` 결과를 arm64 x8을 통해 반환하는 경우도 지원합니다. 컴파일러에서 추출한 선언과 정확한 QuartzCore 내보내기로 `CATransform3DMakeTranslation`, `CATransform3DMakeScale`, `CATransform3DMakeRotation`을 바인딩하며, 변환 과정은 결과의 128바이트를 모두 보존합니다. 일반 간접 구조체 매개변수, Swift 또는 x86_64 행렬 반환, nil 저장 공간 증명이 없는 Objective-C 간접 결과는 지원하지 않습니다. 생성된 C를 O0/O2에서 실행하여 16개 필드 전체, 부동소수점 비트 패턴, 정확한 스칼라 인수, 결과 버퍼 양쪽의 보호 바이트를 검증합니다.

정확한 arm64 QuartzCore 강한 가져오기 `CATransform3DScale`은 같은 제한된 변환 브리지를 사용합니다. 128바이트 입력 포인터는 x0, double 세 개는 d0–d2에 배치되며 x8은 결과를 가리킵니다. HighC는 SDK 호출 전에 전체 입력을 실제 값 전달 구조체로 복사하고 호출 후 16개 결과 필드를 모두 기록합니다. O0/O2 실행은 입력과 결과 버퍼가 분리되거나 겹치는 경우, 모든 필드의 비트 패턴, 경계 보호를 검증합니다. 잘못된 제공자, 약한 가져오기, 유효하지 않은 너비 및 다른 ABI는 거부합니다.

정확한 arm64 CoreGraphics 강한 가져오기 `CGRectApplyAffineTransform`은 x0이 가리키는 48바이트 간접 변환 입력을 d0–d3의 32바이트 사각형 입력 및 결과와 독립적으로 처리합니다. 브리지는 변환의 여섯 필드를 모두 실제 값 전달 SDK 인자로 복사하며, 입력 범위를 사각형 결과 크기에서 구하지 않습니다. O0/O2 실행은 부동소수점 비트 패턴, 네 결과 필드, 별칭 버퍼와 경계 보호를 검증합니다. 다른 제공자, 약한 가져오기, 변경된 전달 위치나 너비 및 x86_64는 계속 거부합니다.

UIKit 대상/동작 바인딩은 선언된 대상 객체, `SEL`, 부호 없는 64비트 `UIControlEvents` 인수를 보존합니다. `addTarget:action:forControlEvents:`는 void를, `initWithTarget:action:`은 객체를 반환합니다. 이 arm64 정보는 정확한 UIKit 제공자 및 내장 선언과의 일치를 요구합니다. 등록 호출의 바인딩으로 콜백 시그니처나 Block 수명을 확정하지 않습니다. `images`와 `viewControllers` getter도 객체를 반환하는 UIKit 선언의 일치를 요구합니다. 기기와 시뮬레이터의 전체 AST에서 모든 선언 주체가 일치합니다.

런타임 호출 목록은 정확히 확인된 가져오기 함수가 원래 인수 포인터를 반환할 때만 `ReturnedArgument`를 선언합니다. 수신자 분석은 일반 ABI 레지스터 무효화 전에 선언된 물리 인수를 읽고, 결과에 입증된 수신자 타입만 복원합니다. SDK는 이 효과를 다시 검증하며 호출, 소유권 효과, 메모리 접근을 제거하지 않습니다.

컴파일러에서 생성한 프레임워크 및 수신자 목록은 Foundation, CoreData, CoreLocation, CoreSpotlight, QuartzCore, UniformTypeIdentifiers, UserNotifications를 공통 제공자로 사용합니다. QuartzCore는 공개 헤더 `CoreAnimation.h`를 사용하며 다른 프레임워크의 호환성 가져오기는 소속 선언을 제공하지 않습니다. 두 생성기는 네 가지 전처리 설정, 정확한 제공자 식별, 부정적인 선언 증거를 유지합니다.

객체 반환 형식은 일치하는 메서드 선언을 통해 동일한 제한된 수신자 증명을 확장합니다. 이름 있는 객체 반환 형식과 컴파일러가 선언한 연관 반환 형식은 클래스 정보를 제공하지만 id만으로는 제공하지 않습니다. 필드 읽기와 메시지 결과는 총 여덟 단계로 제한되며 소스 검증은 현재 선언을 기준으로 각 단계를 다시 확인합니다. 정확한 임포트에 연결된 할당 도우미는 해당 메시지의 반환 형식 계약을 사용하며 호출, 사용자 재정의 및 소유권 효과를 보존합니다. 반환 클래스가 충돌하거나 수신자 계층이 불완전하면 전파를 중단합니다.

형식 호출은 언어별 규칙을 보존합니다. NSString 특성과 공개 술어 API를 SDK 선언 및 모든 런타임 선언과 대조합니다. 술어는 따옴표 안의 자리표시자를 치환하지 않으며 `%K`는 속성 이름 객체를 받습니다. 실제 인수에는 공통 스칼라 승격과 Darwin 가변 인수 ABI를 적용하고 지원하지 않는 이스케이프와 수정자는 거부합니다. 소스 공개 전에 언어, 상수 객체 식별 및 인수 증거를 다시 검증하며 원래 프레임워크 파서를 호출합니다.

컴파일러가 선언한 고정 인자 C 가져오기와 Objective-C 메시지는 지원되는 구조체의 소스 ABI 할당을 공유합니다. 명시적으로 선언된 인자만 전달 위치를 사용하며, Objective-C 계층이 숨겨진 수신자와 선택자 인자를 제공합니다. SDK 내보내기 식별 정보와 시그니처의 정확한 일치는 계속 필요합니다. 스칼라 전용 콜백과 가변 인자의 기존 제한은 유지됩니다.

소스 함수 선언은 호출 규약을 시그니처 식별 정보로 유지합니다. 공통 ABI 계층은 1, 2, 4, 8바이트 정수 인자와 포인터 인자를 사용하는 제한된 Swift 호출을 지원하며, 정수 레지스터 뱅크를 먼저 배정한 뒤 진입 SP 기준 스택 전달 위치를 사용합니다. 좁은 전달 위치마다 정확한 확장 규칙을 기록하고 결과는 최대 두 워드의 정수 또는 포인터와 arm64에서 x0–x3로 반환하는 정확히 네 개의 완전한 워드를 지원합니다. HighC는 선언과 정의에 `swiftcall`을 유지합니다. arm64에서 Swift float 및 double 매개변수와 결과는 독립적인 FP 레지스터 뱅크를 사용합니다. FP 스택 인자와 x86_64 Swift FP 시그니처는 계속 지원하지 않습니다. 컴파일러에서 관찰한 Foundation 값 브리지는 `swift_indirect_result` 포인터와 `swift_context` 포인터를 각각 하나씩 선언할 수 있습니다. arm64는 x8/x20, x86_64는 RAX/R13을 사용하며 일반 정수 인자 레지스터 뱅크를 소비하지 않습니다. HighC는 두 매개변수 속성을 모두 유지합니다. 공개 Foundation 메타데이터 가져오기는 ARM64/x86-64의 macOS와 Mac Catalyst 구성 모두에서 컴파일러 심볼 그래프, 실제 메타데이터 조회 IR, 정확한 SDK 내보내기가 일치해야 합니다. 맹글링된 이름의 접미사만으로 ABI를 결정하지 않습니다. 제네릭 인자와 선언되지 않은 숨겨진 인자, Swift 콜백 타입, 지원하지 않는 물리적 전달 위치는 거부합니다. Mac Catalyst 선언은 iOS 기기의 실행 검증을 의미하지 않습니다.

Swift 6.1의 arm64 코드 생성은 직접 호출되는 부동소수점 속성의 ABI 두 가지도 확인합니다. `CGFloat`를 반환하는 `Double` 확장 getter는 `swiftcc double(double)`이고, 제네릭이 아닌 중첩 값 형식의 `Double` 속성 초기화 함수는 `swiftcc double()`입니다. getter의 입력과 두 함수의 반환값은 v0에 놓입니다. 이러한 선언에는 완전한 맹글링 구문 트리가 필요하며, 소스 공개에는 완전한 리프팅, 소스 본문 검증, 의존성 폐쇄도 필요합니다. 제네릭 바깥 형식은 초기화 함수의 구문 트리가 같아도 숨겨진 메타데이터 인자가 필요할 수 있으므로, 검증된 정확한 초기화 함수 심볼만 허용합니다.

MedIR은 루프를 포함하여 폭이 같은 SSA 복사와 완전한 PHI에서 제한된 불변 상수 전파를 담당합니다. 모든 입력 값의 비트, 폭, 출처, 주소 소유자가 동일하게 수렴해야 합니다. 알 수 없는 정의, 초기값 없는 순환, 불완전한 간선, 충돌하는 상수는 치환을 막으며 분석 예산이 소진되면 함수를 변경하지 않습니다. 분석은 피연산자만 치환하고 호출, 로드, 저장과 부수 효과를 유지합니다. HighIR과 LLVM은 같은 결과를 사용합니다.

작업 단위 코드 소유자 인덱스는 런타임 메타데이터의 기본 함수와 코드 조각 사이의 정확한 관계도 저장합니다. 인덱스 조회와 직접 탐색은 같은 관계 순회를 공유하며 기본 함수의 원래 진입 주소를 유지하고 고립된 참조나 기본 함수가 아닌 부모 참조를 거부합니다. 점프 대상 검증, 범위 증명, 임시 그룹 분석은 같은 불변 인덱스와 조회 비용 계산을 사용합니다. 다른 이미지의 인덱스는 직접 탐색으로 대체되며 예산이 소진된 불완전한 증명은 계속 거부됩니다.

소스 ABI는 Darwin ARM64와 x86_64의 좁은 정수 레지스터 인자에 대한 32비트 부호 확장 또는 영 확장을 명시적으로 기록합니다. HighIR는 원래 인자 타입을 유지하면서 저장과 복사를 통해 알려진 비트를 보존합니다. 32비트를 넘는 읽기, 스택 패딩, 호출로 손상된 레지스터는 여전히 알 수 없는 값입니다. 이는 Apple의 ARM64 및 Intel 호출 규약을 따릅니다. 네이티브 값의 하위 바이트만 관찰해서는 확장을 입증할 수 없습니다.

Mach-O 로더는 초기 권한을 유지하면서 재배치 후 읽기 전용이 된다는 세그먼트의 명시적 보장을 보존합니다. 소스 바이트 및 포인터 읽기는 유일한 파일 매핑 검사를 공유하며, 섹션 이름만으로 불변성을 증명하지 않습니다. 일반적인 전체 너비 로드는 해석된 로컬 데이터 포인터를 별도로 검증한 상수 문자열 객체에 바인딩할 수 있습니다. 바인딩은 재검증을 위해 원래 슬롯을 보존하며 별칭은 생성된 대상 객체의 동일성을 공유합니다. 쓰기 가능한 저장소, 충돌하는 재배치, 부분 또는 순서 지정 로드, 슬롯 자체의 주소는 계속 지원하지 않습니다.

점프 테이블 복구는 별도 경로로 문장을 재구성하지 않고 일반 후속 블록으로의 전이를 생성합니다. 공유 분기, 기본 대상, 루프 진입점을 포함해 각 블록은 한 변환 경로가 처리합니다. 분기 간선의 PHI 복사는 해당 전이 전에 실행되며 병렬 대입 스냅샷을 유지합니다. 불완전한 간선 바인딩은 명시적으로 실패합니다.

루프 구조화는 네이티브 진입점의 정확한 소유 관계를 유지합니다. 항상 참인 래퍼는 본문 첫 명령의 레이블을 중복하지 않습니다. 조건부 역방향 간선의 종료는 네이티브 주소가 없는 간선 복사를 포함해 원래 후속 문장으로 이어집니다. 후속 진입점과 정확히 일치하는 전송만 break로 변환하며, 중첩 루프나 switch 안의 전송은 제어 범위를 유지합니다.

죽은 값 제거는 PHI 간선 복사를 삭제하기 전에 네이티브 진입점의 소유 관계를 정규화합니다. 공통 병합 처리는 분기 진입점과 바로 이어지는 합성 간선 복사 접두부를 구분하며, 연속하지 않거나 관련 없는 중첩 레이블은 여전히 모호한 것으로 처리합니다.

`scripts/collect_objc_sdk_declarations.py`는 실제 기기, 시뮬레이터, Mac Catalyst 및 데스크톱 SDK 구성에서 프레임워크 소유 클래스, 프로토콜, 카테고리와 메서드 ABI 후보를 수집합니다. 각 구성은 공개 헤더, 내보내기 및 컴파일러 증거를 보존하며 미지원 선언과 관련 객체 반환 타입도 포함합니다. 일부 SDK 수집은 정확한 범위를 기록하고, 구성 하나가 실패하면 기록은 미완료 상태로 남습니다. CI 산출물은 이후 선언 카탈로그 일치 검증의 입력이며 그 자체로 새로운 소스 호출 바인딩을 활성화하지 않습니다.

Objective-C 호출 사실은 진입 SP 기준의 제한된 전용 스택 슬롯을 추적합니다. CFG 합류에서는 정확한 값의 교집합과 프레임에서 유래할 수 있는 바이트의 합집합을 사용하여 경로 충돌이나 부분 레지스터 쓰기가 주소 탈출을 숨기지 못하게 합니다. ABI가 명시된 호출은 할당되어 있고 출력 인자 영역 밖에 있는 전용 저장소만 유지합니다. 탈출, 알 수 없는 호출, 겹치거나 원자적인 쓰기, 스택 해제는 해당 증명을 무효화합니다. 바인딩을 공개하기 전에 역방향 간선이 수렴해야 합니다.

HighC는 128비트 이하의 부분 정수 표현에 정확한 폭의 `_BitInt` 타입을 사용합니다. 일반 메모리 보조 함수는 C 객체 패딩과 관계없이 IR 바이트 수만 전송하며, 부호 없는 연산은 순환과 시프트 경계를 보존합니다. 부분 폭의 원자적 접근은 접근 폭을 넓히지 않고 지원되지 않는 것으로 거부합니다.

HighIR은 128비트 값과 동일한 증명 조건으로 64비트 소스 지역 변수를 32비트로 줄일 수 있습니다. 모든 정의의 전체 너비와 하위 부분 너비가 일치하고, 모든 읽기가 하위 부분을 명시적으로 선택해야 합니다. 전체 너비 저장, 이스케이프, 상위 바이트 읽기 또는 상위 식의 부작용은 축소를 막습니다. 소스 매개변수의 패딩은 계속 미정으로 유지됩니다.
값을 구성하는 정의가 하위 너비를 확정하면 정확한 전체 변수 복사가 유한한 그래프를 통해 이 증명을 공유할 수 있습니다. 모든 복사 대상도 축소 조건을 만족해야 하며, 전체 너비를 사용하는 곳이 있으면 모든 상위 예외가 무효화됩니다. 너비 근거가 없는 순환, 너비 충돌 또는 분석 예산 소진 시 원래 값을 유지합니다.
유한한 정수 변환, 오프셋 0의 슬라이스와 확장도 모든 중간 너비가 필요한 하위 부분을 보존하면 이 증명을 전달할 수 있습니다. 각 사용은 해당 문장의 식 루트별로 검사합니다. 최대 두 번의 유한한 분석으로 벡터 패딩을 제거한 뒤 더 좁은 하위 부분을 증명할 수 있습니다.

MedIR 소스 매개변수 검증은 선언된 반환값, 제어 흐름, 메모리 부작용과 호출에서 필요한 바이트를 역방향으로 추적합니다. COPY, PHI, CONCAT, 추출과 확장은 바이트 요구를 유지하며 다른 연산은 보수적으로 모든 입력을 요구합니다. 사용하지 않는 부동소수점 레지스터 상위 부분은 추가 인수가 되지 않습니다. 관찰 가능한 상위 부분, 불완전한 그래프, 분석 예산 소진은 기존 거부를 유지합니다. 이 분석은 기계 연산을 삭제하거나 재작성 ABI를 부여하지 않습니다.

조건 구조화는 분기하지 않는 경로의 PHI 대입을 원래 간선에 유지합니다. 출처 주소를 새 연속 실행 점프 대상으로 만들 수 없으며, 코드 구간 이동에 이런 대상이 필요하면 공유 후속 경로를 원래 위치에 둡니다. 무조건 합성 루프는 정확한 헤더 앞에 연산이 없을 때 첫 네이티브 명령과 같은 후속 진입점을 공유합니다. 조건 검사나 앞선 부작용이 있으면 이 동등성은 성립하지 않습니다.

네이티브 보조 함수의 소스 시그니처 추론은 제한된 CFG 분석으로 모든 기계 반환 경로에 완전한 정수 결과가 있는지 증명합니다. 공통 출구에서 선행 블록의 사실을 교차하며 진입 경로가 초기화되지 않은 루프의 자기 증명을 막습니다. 호출과 부분 쓰기는 다시 완전히 계산할 때까지 결과 증명을 무효화합니다. 잘못된 그래프, 입력값만 반환하는 경로, x86-64 에필로그 복원은 계속 거부합니다. 후보 소스 시그니처만 생성하며 두 번째 파이프라인에서 본문과 의존성 폐쇄를 검증하고 재작성 ABI는 변경하지 않습니다.

## 최근 소스 복원 경계

- Swift 지연 witness table accessor는 `Wl`/`WL` 캐시 패턴, 정확한 런타임 질의, 재구축된 캐시가 증명될 때만 복원합니다. 원래 캐시 주소는 복사하지 않습니다.
- 직접 연결된 Swift 프로토콜 준수 설명자와 명목형 메타데이터를 사용할 때는 둘 다 고유하게 내보내져야 하고 설명자가 불변이어야 하며 디맹글된 명목형이 일치해야 합니다. 가져온 슬롯과 직접 주소의 혼합, 충돌하는 내보내기, 형식 불일치는 거부합니다.
- 소스에서 준수 설명자 주소를 직접 사용할 때는 고유하게 내보낸 불변 `Mc` 기호를 이름으로 다시 연결하고 출력 전에 재검증합니다. 원본 이미지 주소는 복사하지 않습니다.
- 감지된 함수에 accessor의 최종 반환 뒤 독립적인 Swift 코드가 포함되면, 모든 경로가 반환하고 뒤따르는 블록으로 점프하지 않음이 증명될 때만 accessor 증명을 앞부분으로 제한합니다. 뒤따르는 코드는 자체 진단을 유지합니다.
- `Any.self`는 전체 existential container의 정확한 내부 멤버나 공개 export `$sypN`이 메타데이터 정체성을 증명할 때만 상수로 만듭니다.
- Objective-C class-reference cell은 추가 간접 참조 단계를 유지하며, 모호한 사용이 없는 타입 지정 네이티브 load에서만 허용됩니다.
- ivar offset은 클래스와 폭이 같고 load가 하나뿐일 때만 CFG 전반에서 병합합니다. 두 단어 Swift `String` once getter에는 네 carrier 모두에 대한 정확한 계약도 필요합니다.
- 컴파일러가 생성한 인자 없는 Swift 지연 전역 addressor는 정확한 `vau`/`vpZ`/`_Wz`/`_WZ` 심볼군이 하나의 load, 완료 검사, 인증된 `swift_once` 호출, 양쪽 경로의 동일한 저장소 주소 반환과 일치할 때만 재구축합니다. load는 별도 문장이거나 검사 안에 인라인될 수 있지만, 두 형태 모두 같은 predicate를 한 번만 읽는 동작을 보존해야 합니다. initializer는 부수적인 context를 무시하고 일반 소스와 같은 방식으로 의존성을 완전히 닫아야 합니다. 투영은 새 공유 once predicate와 값 셀을 만들며 로드된 이미지의 해당 주소나 initializer 주소를 유지하지 않습니다. 인자 없는 소스 callee ABI는 호출 지점에만 적용하며, 부수적인 context carrier가 계약 증명에 계속 사용될 수 있도록 네이티브 entry ABI는 분리해 둡니다.
- 이러한 initializer가 컴파일러 생성 imported Objective-C 클래스 metadata accessor를 호출하면, 투영은 zero cache, 클래스 참조, `objc_opt_self`, `swift_getObjCClassMetadata`, release publish가 정확히 일치하는 템플릿만 허용합니다. 인증된 runtime lookup을 직접 출력하며 이미지의 cache를 유지하지 않습니다.
- 이름 있는 네이티브 저장소는 각 함수가 시그니처와 제한된 저장소 사용을 증명하는 정확한 네이티브 호출 체인만 통과할 수 있습니다.
- Swift 구체 타입 메타데이터 참조와 캐시는 descriptor, export, provider가 일치할 때만 재구축합니다. 이미지의 초기화된 메타데이터 포인터는 복사하지 않습니다.
- 중첩 또는 로컬 Swift 타입의 nominal metadata reference에는 제한된 컨텍스트 경로와 모호하지 않은 demangle 결과가 필요합니다. 모호하면 거부합니다.
- 출력 가능한 메타데이터 참조 이름은 완전하며 symbolic 또는 private이 아닌 레코드에서만 복원합니다. 잘못되거나 충돌하는 항목은 미해결로 남습니다.
- stack block은 일반적인 frame 사용 중에도 살아 있습니다. 정확히 증명된 consumer, escape 또는 겹치는 write만 이 증명을 취소합니다.
- Objective-C SDK의 `noescape` block 인자는 부모 선언, receiver, callback 위치가 정확히 일치할 때만 허용됩니다.
- selector 전용의 정확한 Objective-C stub은 가변 인자가 없거나, 모두 증명된 포인터이거나, 아래의 완전한 64비트 정수 계약을 충족할 때 동적 format을 바인딩할 수 있습니다. 다른 인자 목록, 부정확한 stub, 선언 충돌 또는 물리 ABI 불일치는 미해결로 남습니다.

소스에 바인딩된 런타임 호출에서 Low→Med 변환은 인증된 외부 비반환 선언을 MedIR 호출 효과에 전달합니다. 런타임 가져오기 트램펄린도 네이티브 함수 목록에 포함될 수 있습니다. 검증된 런타임 바인딩과 완전한 호출 피연산자가 일치할 때만 비반환 고정점 분석이 기존 기계 종료 사실을 보존하며, 소스 힌트만으로 그 사실을 만들지 않습니다. 바인딩은 가져오기 슬롯을, 호출은 트램펄린을 가리킵니다. 추론된 네이티브 효과는 현재 그래프에서 다시 계산하고, 소스 게시 시 가져오기 식별 정보를 다시 검증합니다.

단일 직선 ARM64 헬퍼는 최대 두 개의 Swift 런타임 가져오기를 각각 재검증하고 마지막 호출만 종료할 때 덮어쓰지 않은 완전히 관찰된 진입 컨텍스트를 유지할 수 있다. 앞선 반환 호출에는 일반 호출의 레지스터 파괴 규칙을 적용한다. 동일한 바이트 식별성과 프레임 이탈 증명이 앞선 모든 연산을 검사하며, 각 스칼라 스택 인자는 완전히 기록된 전용 바이트를 사용해야 한다. 분기, 반환, 예외 간선 및 알 수 없는 호출은 계속 거부한다. 효과만 추적하는 진입 바이트 분석은 독립적으로 증명된 종료 그래프를 허용하지만, 기본 미사용 입력 증명에는 관찰된 반환이 필요하다. 이는 후보 시그니처만 만들며 소스 본문과 의존성 폐쇄는 계속 검증해야 한다.

Swift 지연 전역 addressor가 네이티브 상태 복원에 호출 전용 ABI를 제공하려면 현재 이미지와 파이프라인 결과에서 계약을 독립적으로 다시 구성해야 한다. 공통 once 검증기는 정확한 본문, 저장소와 초기화 함수의 식별 정보, 표준 콜백 ABI를 다시 확인하고 현재 초기화 함수가 컨텍스트를 사용하지 않음을 증명한다. 기존 MedIR나 지속 옵션 힌트는 인증 근거가 아니다. 네이티브 추론은 정확한 직접 대상과 인자 없는 포인터 ABI를 대조한 뒤 완전한 Low/Med 호출 대응 및 바이트·프레임 복원 증명을 유지한다. 일반적인 레지스터 손상과 초기화 효과는 그대로 남으며, 이 계약은 읽기 전용이나 종료 호출을 선언하거나 소스 본문과 의존성 검증을 완료하지 않는다.

일반 ARM64 호출은 같은 LowIR 블록에서 할당된 전용 프레임의 모든 바이트가 기록되고 프레임 주소에서 파생된 바이트가 없을 때만 정렬된 8바이트 스칼라 스택 인자를 사용할 수 있다. ABI와 각 네이티브 호출은 계속 독립적으로 검증한다. AAPCS64는 피호출자가 인자 영역을 덮어쓰도록 허용하므로, 호출 후 패딩을 포함한 송신 인자 영역 전체를 무효화한 다음 후속 인자나 저장 레지스터 복원을 검사한다. 재사용하려면 완전히 다시 기록해야 한다. 블록 간 정의, 부분 워드, 꼬리 호출, x64는 이 확장의 대상이 아니며 일반 레지스터 손상 및 종료 복원 검사는 모두 유지된다.

링크된 Mach-O ARM64 코드에서 LowIR은 원래의 무조건 B를 따라 전체 범위가 현재 함수 진입점보다 앞에 있는 공유 에필로그로 진행할 수 있습니다. 허용되는 짧은 형태는 SP에서 x19–x30을 복원하는 전체 폭 LDP, 정확히 한 번의 정렬된 양수 스택 해제, 등록된 실행 가능 임포트로의 마지막 분기만 포함합니다. 정확한 간선마다 유일한 불변 바이트, 픽스업 부재, 내부 함수 진입점 부재를 검증합니다. CFG의 두 진입 검사는 이 간선 증거를 공유하며 BL, 조건 분기, 순차 진입만으로는 진입점을 넘는 디코딩을 허용하지 않습니다. 디코딩된 블록에는 모든 실제 CFG 선행 간선이 유지됩니다. 원래 로드, 스택 갱신, 외부 전송은 LowIR에 남고 공유 함수는 독립적으로 리프트됩니다. 소스 복원은 기존 프레임 증명이 결정하며 앞쪽 공유 블록은 주 함수 크기를 늘리지 않습니다.

arm64의 별도 동적 서식 계약은 모든 도달 정의가 현재 검증된 Objective-C 선언의 동일한 완전한 정수 반환형으로 이어질 때만 비어 있지 않은 64비트 정수 가변 인자 목록을 허용합니다. 같은 폭의 정수 캐스트는 전달 값을 보존하며, 원시 로드, 증명되지 않은 매개변수, 상수, 순환, 부동소수점 또는 좁은 중간값, 부호 불일치는 계속 거부합니다. 바인딩 전에 완전한 네이티브 ABI가 공통 Darwin 가변 인자 배치와 일치해야 하며, 공개 시 값과 선언 증명을 다시 검증합니다. 실행 시 서식 문자열을 추측하거나 파서를 변경하지 않으며, 생성된 메시지는 고정 접두 인자와 실제 생략 부호, 원래 인자 비트를 유지합니다.

스택 block의 소스 제어 흐름 전송은 소유한 출력 사실을 지역 작업 상태와 일시적으로 교환한다. 노드마다 지역 값과 바이트 사실 전체를 두 번 복사하는 비용을 줄이면서 합류 규칙, 작업량과 저장 공간 한도, 성공 근거 및 실패 진단을 동일하게 유지한다.

네이티브 상태 보존 분석은 공유된 검증 완료 ABI 매핑으로 ARM64 동종 부동소수점 집합체를 포함한 구조체 인자의 모든 레지스터 성분을 검사합니다. 프레임이 있는 호출과 프레임 없는 꼬리 호출 모두 각 성분의 프레임 주소 이탈을 확인합니다. 프레임 대여는 원래 스칼라 인자 인덱스에 연결되며 구조체 멤버를 허용하지 않습니다. 스택에 전달되는 구조체와 간접 결과 저장소에는 여전히 별도 증명이 필요합니다. 이 변경은 상태 보존 근거만 보완하며 피호출 함수 ABI나 소스 의존성 폐쇄 요건을 바꾸지 않습니다.

제한된 ARM64 Objective-C 클래스 팩토리는 호출자의 전체 5개 명령과 공유 본문의 전체 9개 명령을 SDK가 증명한 뒤 해당 호출자에만 투영합니다. 호출자가 profiling 카운터와 메타데이터 접근자를 확정하고, 공유 본문은 같은 카운터를 증가시키고 접근자를 간접 호출한 뒤 프레임을 복원하여 인증된 Swift 클래스 변환을 꼬리 호출합니다. 상위 클래스 getter와 팩토리는 현재의 8개 명령 클래스 접근자 증명을 공유합니다. 원래 LowIR 간접 호출 위치에는 독립적으로 검증한 ABI와 전체 프레임 증명을 적용합니다. 소스 helper는 기존 전체 섹션 profiling 저장소를 사용하며 부호 없는 64비트 순환과 호출 순서, 접근자 의존성을 유지하고 게시 시 재검증합니다. 다른 호출자와 공유 native ABI는 바뀌지 않습니다.

ARM64 네이티브 소스 복원은 기존 정수 반환이 정의된 운반자 검사에 실패한 경우에만 Float64 후보를 시도할 수 있습니다. 소스 복원 전용 SSA 증명은 모든 정상 종료에서 반환값의 하위 8바이트가 해당 블록 안에서 완전히 기록되고, 그 출처에 도달 가능하며 현재 소스에 바인딩된 Float64 호출이 있음을 요구합니다. 모든 PHI 입력이 정의되어야 하고, 정의가 사용을 지배해야 하며, 각 순환 구성 요소에는 실제 외부 초기값이 필요합니다. 초기 범위에서는 진입 블록으로 향하는 역방향 간선을 거부합니다. 호출의 부분 덮어쓰기는 대상 ABI가 보존하는 8바이트 접두부에 대해 현재 CallSiteId와 기록된 PreservedInput을 정확히 따르며, 오래된 좁은 별칭으로 이를 대체하지 않습니다. 상수만 있는 값, 로드, 산술 및 알 수 없는 반환값은 Float64를 증명하지 못합니다. 현재 호출, definedReturnPaths, 네이티브 상태, 재리프팅 및 최종 바인딩 검사는 계속 필수이며 일반 Med 타입 추론은 바뀌지 않습니다.

일반 ARM64 프레임 증명에는 현재 로더의 호출 바인딩과 `/usr/lib/libSystem.B.dylib`의 강한 `___stack_chk_fail` 가져오기로 인증한 정확한 `__stack_chk_fail` 종료 경로도 포함할 수 있습니다. 선언은 인자가 없고 반환형이 `void`이며 호출자에게 돌아오지 않는 호출이어야 합니다. 이 마지막 호출만 후속 블록이나 레지스터 복원 없이 블록을 끝낼 수 있으며, 앞선 모든 메모리 및 인자 검사는 유지됩니다. 도달 가능한 일반 반환이 하나 이상 있어야 하며, 모든 일반 반환은 보존 대상 머신 상태를 완전히 복원해야 합니다. 별도의 종료 전용 진입점 증명은 기존 규칙을 유지합니다.

LowIR은 명령 주소, 연산 순서, opcode, 정적 대상으로 이루어진 정확한 호출 위치 식별 정보를 관리합니다. 네이티브 상태 증명과 소스 반환값 증명은 이 식별 정보를 공유하지만 권한은 분리합니다. ARM64 단일 비트 반환값 증명은 비트 63:1을 0으로 바꾸어도 관찰되지 않는다고 판단하기 전에 도달 가능한 모든 경로를 검사합니다. 호출이 레지스터를 변경할 수 있다는 사실은 피호출 함수가 해당 비트를 실제로 덮어썼다는 증거가 아닙니다. 차이가 호출에 도달하면 이후 일반 레지스터, 벡터 레지스터, 플래그의 모든 휘발성 바이트가 달라질 수 있다고 처리하며, 보존 대상 바이트는 기존 정보를 유지합니다. 일반 호출에는 독립적으로 확립된 완전한 ABI가 계속 필요합니다. 별도로 인증한 Swift 비교 후보는 컴파일러의 원시 단일 비트 반환값과 정확한 가져오기 제공자를 기록하며, 단독으로 바이트 반환 선언을 공개하거나 소스 투영을 허용하지 않습니다.

HighC는 값의 정확한 폭을 사용하는 바이트 복사 헬퍼로 일반 메모리 쓰기를 출력합니다. 기계 주소만으로는 C의 정렬이나 유효 타입을 입증할 수 없습니다. 쓰기 문장, 쓰기 표현식, 메모리를 통한 대입은 같은 헬퍼 경로를 사용하며 주소와 값을 각각 한 번 평가하고 기록한 값을 표현식 결과로 반환합니다.

Swift 불리언 검증은 현재 Objective-C 진입 ABI, 변경 불가능한 직접 호출, 정확한 강한 가져오기와 완전한 LowIR 사용 증명을 결합합니다. 다른 호출은 현재 런타임 ABI, 8개 명령의 완전한 클래스 접근자 증명, 또는 공유 선택자 또는 수신자 선언 계층에서 완전한 스칼라 ABI를 다시 검증한 정확한 강한 super 호출 가져오기가 필요합니다. 공개 시 네이티브 의존성, super 수신자와 프레임 검사는 유지됩니다. 증명되지 않은 네이티브·동적 호출과 중복 호출은 거부하며, 이 사실만으로 소스를 공개하거나 런타임 바이트 반환 ABI를 선언하지 않습니다.

정확한 진입 주소의 함수 기호가 있는 네이티브 진입은 임시로 전체 x0 워드만 관찰 가능한 결과로 간주할 수 있습니다. 소스 추론이 완전한 진입 ABI를 바인딩하면 공개 단계에서 그 ABI로 동일한 LowIR 증명을 다시 실행합니다. 임시 가정만으로 소스 바인딩이나 바이트 반환 ABI를 제공하지 않습니다. 보존 레지스터 추론은 현재 진입 ABI로 정확한 LowIR 호출 위치를 다시 검증한 뒤에만 이러한 Bool 호출을 사용할 수 있습니다. 관찰된 보존 레지스터가 매개변수가 될 수 있는지는 진입 바이트와 완전한 상태 복원 증명이 계속 결정합니다. 바인딩된 두 워드 네이티브 결과는 두 반환 레지스터가 네이티브 쌍 증명을 통과한 경우에만 관찰 범위를 x0/x1로 확장할 수 있습니다. 공개 단계는 Bool 호출을 허용하기 전에 완전한 쌍을 다시 추론합니다.

정확한 libswiftCore Hasher seed, String.hash(into:), Hasher.finalize 가져오기는 검증된 Swift ABI 인수를 통해 ARM64 개인 프레임의 72바이트 영역을 빌릴 수 있습니다. 상태 증명은 호출 후 빌린 모든 바이트를 무효화하고 저장된 레지스터와의 겹침이나 프레임 탈출을 거부합니다. 현재 가져오기와 ABI의 증명 없이 이름만 일치해서는 빌릴 수 없습니다.

Swift 6.1.2 클라이언트 IR은 정확한 libswiftCore `_DictionaryStorage.allocate(capacity:)` 가져오기가 ARM64와 x64에서 포인터를 반환하고 정수 용량과 `swiftself`의 사전 메타데이터를 받는다고 보여 줍니다. 검증된 ABI는 할당 효과를 유지하며 호출을 바인딩하지만 이것만으로 호출자나 다른 사전 의존성을 복구하지는 않습니다.

Swift 6.1.2는 정확한 libswiftCore `_DictionaryStorage.copy(original:)` 및 `resize(original:capacity:move:)` 가져오기도 `swiftself`에 구체적인 사전 메타데이터를 받고 포인터를 반환하는 호출로 정의합니다. resize는 정수 용량과 1바이트 Bool도 받습니다. 증명은 호출과 할당 효과를 유지하고 제공자와 완전한 ABI를 검증하며 다른 미해결 의존성이 있는 호출자는 공개하지 않습니다.

정확한 libswiftCore `KEY_TYPE_OF_DICTIONARY_VIOLATES_HASHABLE_REQUIREMENTS` 가져오기는 Swift 6.1.2 선언대로 형식 메타데이터 포인터 하나를 받고 반환하지 않습니다. 이 종료 계약은 검증된 제공자와 ABI에만 적용되며 원래 호출과 트랩은 소스 경로에 남습니다. 정상적으로 반환하는 ARM64 함수에서도 이 정확한 호출은 후속 블록이 없는 예외 분기를 끝낼 수 있으며, 바로 뒤에 트랩이 있는 경우도 포함됩니다. 모든 정상 반환 경로에는 여전히 완전한 상태 증명이 필요합니다.

8개 명령으로 구성된 ARM64 클래스 접근자 증명을 하나의 공통 구현으로 통합했습니다. 변경 불가능한 명령과 강한 `objc_opt_self` 가져오기를 확인하여 입력 인수가 사용되지 않고 결과의 8바이트 모두가 런타임 호출에서 나옴을 증명합니다. 이 사실만으로 클래스 객체의 동일성이나 소스 의존성 완결을 인정하지 않습니다. super getter와 메타데이터 팩토리는 클래스, 파이프라인, 프레임, 의존성 검사를 유지합니다. 구조적 언와인드 판단도 공유하며 부분 해석과 언어 예외 디스패치는 계속 거부합니다.

불리언 정규화 증명은 인수가 없거나 레지스터 인수만 있는 호출도 SP를 암시적 입력으로 취급합니다. 호출 전에 SP 차이를 거부합니다. 나중에 SP를 복원해도 피호출자의 스택 접근을 되돌릴 수는 없습니다.

불리언 결과 증명은 피연산자 폭 안의 상수 정수 왼쪽 이동, 논리 오른쪽 이동, 산술 오른쪽 이동과 더 작은 출력으로 잘리는 비트 연산의 차이 비트를 추적합니다. 관찰되지 않은 런타임 패딩 비트를 정의된 것으로 간주하지 않고 ARM64 비트 검사 조건을 처리합니다. 가변 시프트와 SELECT는 두 실행의 모든 입력이 같을 때만 허용합니다. 입력이 다른 가변 시프트, 범위 밖 상수 시프트, 분기·인수·저장·반환값에 도달하는 패딩 비트는 계속 정규화를 거부합니다.

추론한 네이티브 64비트 정수 반환값은 모든 반환 경로가 하위 32비트 투영을 지원하고 하나 이상의 경로에 정의되지 않은 상위 패딩이 명시된 경우에만 32비트로 좁힙니다. 완전한 소스 제어 흐름과 모든 지역 정의가 일치해야 합니다. 알 수 없는 하위 바이트, 순환 정의, 누락된 분기, 부작용이 있는 상위 식은 거부합니다. 새 소스 ABI로 다시 리프트하며, 버린 상위 워드를 읽는 호출자는 미해결 값을 유지하므로 복원 소스로 게시할 수 없습니다.

Objective-C 상수 배열과 사전은 정확히 가져온 CoreFoundation 불리언 싱글턴을 요소로 유지할 수 있습니다. 각 간선은 유일하고 불변이며 파일에 저장된 영역의 강한 SDK 데이터 바인딩을 요구하고, 가산값은 0이며 겹치는 재배치가 없어야 합니다. 생성 도우미는 객체 표현을 복사하지 않고 가져온 객체 주소를 반환해 반복 요소의 동일성을 유지합니다. 가져오기 슬롯 주소는 로드된 객체와 같지 않으며 불리언을 사전 문자열 키로 쓰지 않습니다. 게시 시 전체 그래프를 다시 검증합니다.

AArch64 Swift 타입 참조 구성은 보존된 기기 및 시뮬레이터 SDK 내보내기 증거에 따라 `libswiftCore`의 정확한 `_ContiguousArrayStorage` 명목 타입 설명자도 허용합니다. 유일한 불변 저장소의 가산값이 0인 강한 가져오기와 기존 캐시·참조·이름 맹글링 검증이 필요합니다. 생성 C는 설명자 바이트를 복사하지 않고 설명자 정체성, 상대 참조 및 공유 쓰기 가능 캐시를 유지합니다. 정확한 `_DictionaryStorage` 설명자도 연결된 이미지가 `libswiftCore`에 대한 가산값 0의 강한 바인딩을 증명할 때만 허용합니다. 다른 표준 라이브러리 설명자는 지원하지 않습니다.

불변 자기 참조 전역 포인터의 정확한 데이터 주소는 해당 값을 읽을 때와 동일한 재구성된 포인터 크기 저장소를 공유합니다. 초기 포인터는 저장소 자체를 가리키므로 불투명 키의 정체성과 포인터 내용을 모두 유지합니다. 직접 주소 게시 시 고유 매핑, 로컬 자기 재배치 및 불변성을 재검증합니다. 가변·중첩·잘림·충돌 저장소는 미해결 상태로 유지합니다.

소스 복원은 전체 폭 레지스터 복사와 `RET x30`만 포함하는 제한된 AArch64 로컬 리프 함수를 투영할 수 있습니다. 로더는 링크된 Mach-O의 불변 기계어, 로컬 링크 속성, 원래 BL 호출 위치를 검증하며 플랫폼·프레임·링크·제로 레지스터 피연산자, 메모리 효과 및 기타 명령을 거부합니다. 순차 복사는 리프 진입 시 값으로 정규화하고 모든 소비자는 목적지에 쓰기 전에 입력 전체를 읽습니다. MedIR 변환, Objective-C 수신자와 프레임 사실, 네이티브 상태 복원은 같은 변환을 사용하며 BL의 실제 링크 레지스터 쓰기를 유지합니다. 원래 LowIR와 일반 리프트·패치 동작은 변하지 않습니다. MedIR와 HighIR는 일치하는 증명 기록을 보관하고 소스 게시 시 현재 바이트와 호출자 명령 경계를 재검증합니다. 누락·중복·충돌·오래된 증거는 미해결로 남으며 메모리 작업을 포함한 추출 보조 함수에는 별도 증명이 필요합니다. 피호출자가 보존해야 하는 레지스터를 쓰는 리프 함수는 일반 C 함수로 선언할 수도 없습니다.

이 소스 전용 리프 투영은 완전한 상수 문자열 객체 주소를 만드는 `ADRP`와 시프트 없는 64비트 `ADD`도 허용합니다. 레지스터 값은 진입 입력과 객체 주소를 명시적으로 구분합니다. 페이지 값은 내부 중간값이며 반환 시 남은 페이지, 산술 오버플로 또는 검증되지 않은 객체는 전체 투영을 거부합니다. 주소 계산은 피호출 명령의 PC를 사용합니다. `readObjCConstantString`은 각 최종 객체와 내용을 검증하고 새 비교를 위해 증명 기록에 보관합니다. MedIR은 객체 자체를 소유자로 하는 `DataAddress`를 부여하며 게시 시 일반 소스 바인딩도 필요합니다. 상수 쓰기는 겹치는 수신자, 진입 레지스터 및 프레임 바이트 사실을 무효화하고 나머지 레지스터와 메모리를 보존합니다. 같은 효과를 네이티브 입력 추론과 비표준 출력 거부에 사용합니다.

별도의 일반 호출 증명은 로컬 `ADRP x8; LDR x0,[x8,#imm]; RET x30` 클래스 getter를 다룹니다. 공유 로더 증명은 원래 BL, 완전한 리프, 불변 클래스 가져오기 슬롯과 정확한 SDK 클래스 및 제공 라이브러리 소유권을 검증합니다. Objective-C 사실 전달은 검증된 무입력 동작으로 프레임 비공개성과 클래스 수신자를 보존하며 이전 탈출은 지우지 않습니다. CALL은 남고 독립적인 네이티브 서명 바인딩과 완전한 소스 의존성이 필요합니다. MedIR과 HighIR은 같은 증명을 유지하며 게시 시 현재 기계어, 가져오기 신원과 유일한 일반 호출을 다시 확인합니다. 전용 클래스 슬롯 검사만 일치하는 클래스 메타데이터를 허용하고 일반 읽기 규칙은 유지합니다.

투영 리프는 새로 인증한 상수 문자열 주소를 정확히 한 번 `STR Xn,[SP,#0]`으로 저장할 수 있습니다. 증거는 원래 명령과 저장 당시 값을 최종 레지스터 값과 별도로 유지합니다. 사실 전파와 바이트 보존 모두 알려진 현재 SP의 16바이트 정렬과 할당된 전용 프레임 안의 완전한 8바이트 슬롯을 요구하며, 덮어쓴 사실과 호출의 스택 인수 영역은 무효화됩니다. 이탈한 프레임의 전용성은 복구하지 않습니다. Objective-C와 미리 형식이 선언된 네이티브 함수를 포함한 모든 게시 함수는 일치하는 명시적 Med/High 진입 ABI로 전체 프레임 상태 증명을 통과해야 합니다. 프레임 없는 대체 경로나 도우미의 일반 독립 ABI는 허용하지 않습니다.

동일한 단일 SP 저장은 리프 진입 레지스터로 정규화한 값도 기록할 수 있습니다. 모든 소비자는 최종 레지스터 쓰기 전에 이 독립 입력을 보관합니다. 프레임에서 유래한 바이트는 거부하고 덮어쓴 사실을 모두 교체하며, 기록된 슬롯도 미정의 입력 비트를 정의하지 않습니다. 선언된 출력 스택 인수가 연속적이고 완전한 진입 레지스터 8바이트를 소비할 때만 진입 사용을 인정합니다. 사용하지 않은 보존 슬롯은 인수를 추가하지 않습니다. Objective-C는 증명된 스칼라·수신자·인수 사실만 보존하고 알려진 복사 Block은 거부합니다. 입력 정의, ABI, 프레임 상태 및 소스 종속성 전체 검사는 계속 필수입니다.

불투명한 직접 네이티브 호출은 해당 지점에서 두 실행의 모든 물리 레지스터와 플래그 비트가 같을 때만 불리언 정규화 증명에 참여할 수 있습니다. 증명은 메모리 관측의 차이를 계속 거부하고 임시 값의 차이를 유지하며 루프의 역방향 간선에서도 다시 검사합니다. 이는 피호출자의 ABI, 반환 값 정의 또는 소스 바인딩을 부여하지 않습니다. 게시하려면 원래 호출과 의존성을 각각 독립적으로 바인딩해야 합니다. 현재 ABI가 없는 식별 가능한 가져오기 스텁과 유효하지 않은 기존 바인딩은 계속 거부됩니다.

Swift 지연 객체 getter는 각각 검증된 `swift_retain`과 뒤따르는 `objc_autoreleaseReturnValue`도 허용하며 두 호출과 실제 반환 값 전달을 유지합니다. 초기화 앞의 선택적 빈 레이블에는 표현식, 중첩 문 또는 메모리 효과가 없어야 합니다. 부수적인 once 컨텍스트를 제거하려면 초기화 함수가 컨텍스트를 사용하지 않는다는 독립적 증명과 게시 시 재검증이 필요합니다.

Swift `NSObject` 동등 비교 후보는 기기와 시뮬레이터에서 독립적으로 검증한 ABI `swiftcc i1(ptr, ptr, ptr swiftself)`를 사용합니다. 객체 인수는 x0/x1, 메타데이터는 x20에 배치됩니다. `libswiftObjectiveC`의 정확한 강한 가져오기, 불변 저장소 및 기존의 완전한 호출자 정규화 증명이 필요합니다. HighC는 동일한 표준 입력 계약에서 `_Bool` 선언과 `swift_context` 매개변수를 생성하며, 조회만으로 바이트 반환 ABI를 게시하지 않습니다.

고정된 무인수 Objective-C 객체 getter는 opaque identical-state 호출로만 이 정규화 증명에 참여할 수 있습니다. 현재 selector stub의 불변 20바이트 전체, selector 참조, 강한 `objc_msgSend` 가져오기 및 정확한 SDK 포인터 ABI가 일치해야 하며, 실패한 `__objc_stubs` 증거는 알 수 없는 네이티브 호출로 되돌아갈 수 없습니다. clobber, 결과 또는 소스 바인딩 사실을 부여하지 않으므로 호출 지점의 모든 물리 레지스터, 플래그 및 메모리 관찰이 이미 같아야 합니다.

## MBA 후보 점수와 표본 검증

MBA 단순화는 캐시된 출력 점수로 먼저 펼친 연산자와 잎 노드 수를 비교하고, 크기가 같으면 연산 수를 비교합니다. 결합법칙을 따르는 식은 출력되는 이항 연산자를 모두 셉니다. 부호 있는 리터럴은 잎이며, 암묵적인 음의 단위 계수에서만 모든 비트가 1인 상수를 생략합니다. 뺄셈은 항의 단항 부호를 흡수하고 `Not(Eq)`는 하나의 부등식으로 출력합니다. 출력기와 점수는 부호 및 첫 항 규칙을 공유합니다. 공유 하위 트리는 등장할 때마다 비용을 내므로 작은 DAG가 더 큰 출력 트리의 복제를 정당화하지 못하며, 포화된 크기도 증가를 허용하지 않습니다. 캐시는 기하급수적으로 커지고 추가된 노드와 간선을 한 번씩만 방문하며 공유 트리를 문자열로 펼치지 않습니다. 공개 크기 카운터는 첫 성분을 보고하며, 같은 크기의 재작성은 두 번째 성분을 개선할 수 있습니다. 이전 버전의 카운터와 직접 비교할 수 없습니다. 두 계획과 모든 문맥 변수가 64비트에 들어가면 검증 표본은 컴파일된 평가기의 워드 크기 경로를 재사용합니다. 경계 대입과 난수 흐름은 유지하고 더 넓은 입력은 임의 정밀도로 평가합니다.

## ARM32 코드 모드와 아키텍처 간 프레임 전달

ELF의 ARM32 코드 모드는 Thumb 주소 태그를 정규화하기 전에 정의된 실행 가능 함수 심볼, ARM/Thumb 매핑 심볼, 실행 가능한 진입점으로 결정합니다. BinaryImage는 현재 이미지 전체에 모드 하나만 가지며 단일 ARM 또는 Thumb 이미지를 지원합니다. 서로 다른 영역은 심볼과 재배치 접근을 유지한 채 혼합 모드 메타데이터로 기록하고, 같은 주소의 상충하는 증거는 거부합니다. 디코딩, 리프팅, 재작성에는 지원되는 단일 모드가 필요합니다. 기존 SDK 세션에 혼합 메타데이터를 로드하면 이전 디코더를 제거합니다. 데이터 심볼이나 관련 없는 이름으로 모드를 선택하지 않습니다. ARM/Thumb 상호 운용 실행에는 발견, 디코딩, 재작성 전반에 주소별 모드 계약이 필요합니다.

HighIR 대수 변환 전에 비공개 프레임 전달은 이름 변경 후의 소스 로컬 식별자와 대상 폭에 대한 공통 프레임 주소 증명을 사용합니다. 직선형 함수의 정확한 정수 읽기는 입력과 바이트가 변하지 않는 동안 저장값을 재사용할 수 있습니다. 알 수 없거나 겹치는 쓰기는 사실을 무효화하며, 호출, 순서가 지정된 메모리, 잘못된 그래프, 제어 흐름 합류는 증명을 막습니다. 재생되는 연산은 전역적으로 정의되고 명시적으로 타입이 있어야 하며 store/load 절단은 유지하고 예산에는 공유 DAG의 반복 간선도 포함합니다. 32/64비트 대상에 적용됩니다. MedIR/HighIR 메모리 경계는 LowIR VA 운반체로의 명시적인 제로 확장에서만 대상 폭의 부호 없는 주소를 복원합니다. 다른 넓은 식과 부호 확장은 유지합니다. 후속 HighC 텍스트 기반 전달은 캐시된 저장값에 쓰인 정의 이름을 추적하여 생존성 분석과 인라인화가 이를 제거하지 못하게 합니다.

AArch64에서는 비공개 Darwin `os_unfair_lock_s` 설명자에 한해 직접 심볼 참조 `0x01`을 공개 텍스트 타입 이름으로 변환할 수 있습니다. 변경 불가능한 플래그, 상위 모듈, 이름, 접근자 및 고유한 로컬 심볼을 검증합니다. 다른 직접 참조와 증명되지 않은 비공개 컨텍스트는 계속 지원하지 않습니다.

64비트 AArch64 및 x64 이미지에서는 포인터보다 폭이 좁은 정수 직접 저장값의 해당 IR 발생 위치에서 스칼라 출처가 증명되면, 비트가 매핑된 이미지 주소와 같아도 숫자 값으로 유지합니다. 포인터 폭의 값, 출처가 불명확하거나 주소에서 왔거나, 주소로 사용되거나, 포인터 타입인 값은 계속 재배치 가능한 바인딩이 필요합니다.

HighIR의 공통 비공개 프레임 주소 증명은 대상 폭의 정수 덧셈에서 한 피연산자가 증명된 프레임 기준이고 다른 피연산자가 제한된 상수 오프셋이면 두 피연산자 순서를 모두 허용합니다. 뺄셈은 순서를 그대로 구분합니다. 따라서 증명되지 않은 프레임 주소를 허용하지 않으면서 nil로 끝나는 Objective-C 인수의 스택 저장을 다시 검증할 수 있습니다.

AArch64 소스 바인딩에서 스칼라 비트가 이미지 주소와 일치하는 전체 폭 저장값은 정확한 로컬 명령열이 W 레지스터의 제로 확장 페이로드와 정규 인라인 Swift String 태그를 만들고 단일 STP로 연속된 두 워드를 저장할 때만 숫자로 유지합니다. 명령 바이트와 재배치가 없음을 다시 확인하며, 쌍이 없거나 출처가 증명되지 않으면 미해결로 둡니다.

Swift 접근 임시 저장소는 명시적인 공유 수명 계약을 사용한다. 로더는 원래 ARM64 BL, 강한 libswiftCore 가져오기와 현재의 완전한 ABI를 인증한다. 바이트 단위 증명은 정확한 Read/Modify 플래그 `0`/`1`과 추적 플래그 `32`/`33`을 허용한다. 추적 시 24바이트 레코드는 대응하는 `swift_endAccess`까지 TLS에 유지된다. 모든 도달 경로의 활성 레코드가 일치해야 하며 반환, 꼬리 호출 또는 프레임 해제 전에 모두 종료해야 한다. 겹치는 쓰기, 중복 초기화, 누락되거나 중복된 종료는 거부한다. 중첩 레코드는 어느 순서로든 종료할 수 있다. 연결 해제가 다른 활성 레코드를 바꿀 수 있으므로 내용은 불투명하며 비공개 프레임 포인터를 포함할 수 있다. 이 가능성은 종료, 부분 쓰기, 쓰기 가능 호출과 프레임 재사용 후에도 남으며 확정적인 저장이 각 바이트를 덮어쓸 때만 제거된다. 일반 차용자는 이 오염된 내용을 사용할 수 없다. 프레임 로드의 접두 구간 질의는 미래의 종료를 가정하지 않고 아직 유지 중인 레코드를 거부한다. 추적하지 않는 `swift_beginAccess`는 동기 차용이며 종료를 생략할 수 있지만, 종료 호출에는 모든 경로에서 초기화된 레코드가 필요하다. 스칼라 결과 추론도 재검증하며 런타임 충돌 검사와 종료 동작을 보존한다. ([Swift 런타임](https://github.com/swiftlang/swift/blob/swift-6.1.2-RELEASE/stdlib/public/runtime/Exclusivity.cpp), [접근 플래그](https://github.com/swiftlang/swift/blob/swift-6.1.2-RELEASE/include/swift/ABI/MetadataValues.h)).

프로파일 계측과 전체 모듈 최적화가 적용된 Swift 병합 `@objc` `CGFloat` setter는 self, selector, double 값, ivar 오프셋 포인터, 프로파일 카운터 포인터를 받는 C ABI를 사용합니다. 정확한 맹글 심벌과 함수 진입부에서 x3를 통한 카운터 읽기·증가·저장을 모두 확인한 경우에만 이 5인자 ABI를 부여합니다. 계측하지 않은 함수는 인자가 4개입니다. 후보는 일반적인 소스 본문, 데이터 바인딩, 의존성 폐쇄 증명도 통과해야 합니다.

값 증인 코드가 사용하는 비공개 Swift 구조체 또는 열거형 메타데이터는 고유하게 내보낸 메타데이터 접근자를 통해 링크된 이미지의 동일성을 유지할 수 있다. 소스 바인딩은 일치하는 불변의 비공개 명목 형식 설명자와 비공개 메타데이터 주소를 정확히 계산하는 불변 AArch64 `ADRP x0; ADD x0, x0, #offset; MOV x1, #0; RET` 리프 함수만 허용한다. 생성된 소스는 해당 접근자를 호출하며, 게시할 때 바이트, 재배치, 심볼 및 내보내기 기록을 다시 검사한다.

AArch64 네이티브 보조 함수는 각 벡터 입력의 16바이트가 모두 관측되고 앞선 부동소수점 인자가 이전 `q` 레지스터를 빠짐없이 차지하며 일반 호출, 반환, 스택 프레임 증명이 성립할 때에만 `q0` 이후의 완전한 입력을 값으로 전달하는 C 벡터로 바인딩할 수 있습니다. HighC는 소스 경계에서 비트 단위로 변환합니다. `x0`/`x1`의 128비트 정수는 다른 ABI입니다. 일부 레인, 부동소수점 레지스터 접두 구간의 빈자리, 네이티브가 아닌 선언은 지원하지 않습니다.

중첩 Objective-C 스택 블록은 현재 파이프라인 결과에서 부모 블록의 메서드 수신자 강한 캡처와 자식 블록의 동일 필드에 대한 완전한 소유 복사가 증명된 경우에만 수신자 클래스를 이어받습니다. 탐색은 해당 결과 안에서 제한된 고정점까지 반복하며, 다음 파이프라인 실행에서는 전체 증명을 다시 수행합니다. 선택자, 타입 정보가 없는 `id`, 수신자가 특정되지 않은 블록 소비자만으로는 수신자 클래스, 호출 ABI 또는 블록 수명을 확정할 수 없습니다. 16바이트 컨텍스트 복사는 해당 8바이트 레인이 검증된 모든 부모 블록 리터럴 안에 완전히 들어갈 때만 이 증명을 보존하며, 일부만 복사하거나 레인 순서를 바꾸면 보존하지 않습니다.

설명자에 바인딩된 Block invoke는 현재 블록 계획이 강하게 캡처한 수신자의 출처를 증명하고 `objcReceiverIvarStorageSize`가 클래스 상속 관계, 정확한 오프셋 슬롯과 읽기 폭, 전체 필드 인코딩, 기록된 저장 범위를 다시 확인할 때만 해당 필드에 쓸 수 있습니다. 이탈 분석은 정확한 복사, 전용 스택 저장, 모든 경로가 일치하는 제어 흐름 합류를 통해 이 증거를 유지합니다. 런타임 오프셋 읽기와 원래 쓰기는 유지됩니다. 부동소수점 수치 변환, 부분 포인터, 임의의 주소 연산, 범위를 넘는 쓰기, 전용 컨텍스트나 프레임 주소의 저장에는 필드 쓰기를 허용하지 않습니다. 런타임 크기가 알려지지 않은 레이아웃은 계속 지원하지 않습니다. `ObjCBlockSources`와 `ObjCCallHints`는 스칼라 및 부동소수점 필드, 오래된 메타데이터, 변환, 경로 간 충돌을 검사합니다.

ARM64 로컬 ARC 해제 브리지는 변경 불가능한 8바이트 전체가 `MOV x0, x19..x28` 뒤에 검증된 가져오기 스텁으로 향하는 `B`가 있음을 입증할 때만 `objc_release`로 바인딩됩니다. `objcRuntimeSourceCallHint`는 로컬 링크와 정확한 libobjc 강한 가져오기 제공자를 요구하고, 보존 레지스터의 원래 인수 위치를 유지하며 소스 게시 시 브리지를 다시 검증합니다. 생성된 C는 해당 인수에 실제 런타임 해제를 한 번 실행합니다. 부분 폭 또는 시프트 복사, 추가 효과, 약한 가져오기나 충돌하는 가져오기, 재배치 및 기계어 바이트 변경은 거부합니다. `SourceObjCRuntimeTail`은 이러한 반례를 검사하고 생성된 C를 O0/O2에서 실행합니다.

검증된 블록 디스크립터는 본문이 승인되기 전에도 invoke ABI를 제공할 수 있습니다. 소비자 호출은 같은 검증된 블록 계획의 수신자 캡처를 사용하며, 공개에는 본문과 수명에 대한 독립적인 증명이 계속 필요합니다.

## x64 네이티브 동기 예외

checked x64의 `DIV`/`IDIV`는 실제 프로세서 결과와 `#DE`를 사용합니다. KVM은 비공개 supervisor IDT/IST, WHP는 명시적인 예외 비트맵을 사용하며 원래 컨텍스트와 제공된 오류 코드를 전송 오류와 구분합니다. OS는 복구 가능한 이벤트를 소비한 뒤 계속 실행할 컨텍스트를 설치합니다. Windows 드라이버는 0으로 나누기와 몫 오버플로를 `STATUS_INTEGER_DIVIDE_BY_ZERO`로 변환하고 실제 SEH filter, `__finally`, 재시도를 실행합니다. `NeverDX64ExceptionTests`는 Unicorn 없이 빌드되며 `DriverWDMCPUException`은 원본 WDK 사례를 검증합니다. 사용할 수 없는 ARM64 호스트는 명시적으로 건너뜁니다.

## 단계적으로 보관하는 RAM 효과

`RAMTransaction`은 물리 실행 임대 아래에서 명령이 선언한 쓰기 범위의 물리적 합집합만 보관합니다. 결과 관찰자 호출 전에 원래 RAM을 복원하며 취소, 전송 오류, 관찰자 예외는 부분 RAM이나 레지스터를 공개하지 않습니다. CPU 예외는 RAM 복원 후에도 아키텍처 예외 상태를 유지합니다. ARM64 단일·쌍 저장도 같은 계층을 사용합니다. x64는 8/16/32/64비트 `XCHG`, `XADD`, `CMPXCHG`를 실행하며 LOCK 또는 암시적 잠금 형식에는 자연 정렬을 요구합니다. `NeverDRAMTransactionTests`는 호스트 CPU와의 결과 비교, 복원, 별칭, 권한을 검증하며 사용할 수 없는 플랫폼은 명시적으로 건너뜁니다. 장치와 병렬 SMP는 제외되며 CPU 스냅샷은 이미 확정된 RAM을 복원하지 않습니다.

## 전체 x87 상태

`NeverDEmulationArch`는 ISA, 페이지 테이블과 FP 상태 배치를 소유하며 네이티브 및 Unicorn 전송이 공유합니다. x64 컨텍스트는 x87 제어, 상태, TOP, 물리 태그, 연산 코드, 명령/데이터 포인터와 8개의 80비트 레지스터를 보존합니다. `FP0`–`FP7`은 `RegisterValue`를 사용하고 스칼라 접근은 잘림을 거부합니다. `FPTag`는 물리 비어 있지 않음 비트맵입니다. `NeverDX64FPTests`는 모든 TOP, 정확한 연산의 호스트 FXSAVE/FXRSTOR 비교와 복원을 검사합니다. checked x87 명령 또는 모든 반올림 의미를 입증하지 않으며 없는 네이티브 호스트는 명시적으로 건너뜁니다.

checked x64는 마스크된 legacy `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`의 `SS`, `SD`, `PS`, `PD` 형식도 허용합니다. `X64SSEInstructions.def`가 operand 너비, 정렬, 허용 규칙을 관리합니다. `MaskedSSEArithmeticMatchesIndependentHostExecution`은 독립 host CPU oracle로 register/RAM 형식, 네 반올림 모드, FTZ, signed zero, subnormal, NaN을 검증하며, `SSEMemoryObserverStopsBeforeResultAndStatusChanges`는 효과 반영 전 중단을 검증합니다. DAZ, 마스크되지 않은 예외, x87, AVX는 허용하지 않습니다.

checked Unicorn은 `MachineRunControl`을 사용하며 ARM64 유지보수, 게스트 실행과 전체 상태 읽기를 한 단계 시간 한도에서 처리합니다. `UC_HOOK_CODE`는 명령 진입에서 빌린 정지 토큰과 기한을 확인합니다. 동기 엔진 호출은 반환 전에 hook 참조를 해제하지만 기계 단계는 상태 게시까지 제어를 유지합니다. Unicorn과 WHP는 전체 CPU 상태를 임시 저장하고 성공한 단계의 게시 직전에 같은 제어 조건을 확인합니다. WHP는 준비 전에 시간 한도를 한 번만 만듭니다. 확인된 x64 CPU 예외는 상태 읽기 중 도착한 정지 요청보다 우선합니다. 읽기가 취소되면 checked RAM 트랜잭션은 추측 쓰기를 버리며 비제한 소프트웨어 계약은 그대로입니다. `MachineInterruptedError`는 확인된 취소와 호스트 또는 상태 읽기 실패를 구분합니다. 공통 checked CPU는 `Stopped` 또는 `Deadline`을 반환하고 CPU/RAM을 유지하며 재시도를 허용합니다. 동시 정지 요청이 있어도 실제 실패는 `BackendFailure`로 남습니다.

`RunDeadline::invoke`는 중지되었거나 기한이 지난 WHP 실행을 호스트 호출 전에 거부하고, 취소 중에도 실제 호스트 결과를 보존하며, 빌린 중지 토큰을 해제하기 전에 인터럽트 콜백의 종료를 확인합니다. KVM과 WHP는 실행 임대를 보유한 호출 스레드에서 완전히 캡처된 비공개 상태를 검증한 다음 동시에 도착한 중지나 기한을 분류합니다. 실제 호스트·캡처 실패와 인증된 x64 CPU 예외가 우선합니다. 일반 성공 상태는 취소 확인이 끝날 때까지 비공개로 유지하며, 확인된 중단은 추측적 CPU/RAM 효과를 버리고 재시도를 허용합니다. 준비, 네이티브 실행, 캡처는 하나의 스텝 유예를 공유합니다. 협력적 취소를 제공하지만 엄격한 실제 시간 상한은 보장하지 않습니다.

구체 메타데이터 레시피의 Swift 비공개 명목 타입 설명자는 유일하게 내보낸 클래스 설명자의 필드 메타데이터를 통해 접근할 수 있습니다. 증명은 클래스의 필드 설명자, 정확한 12바이트 필드 레코드의 타입 참조, 그 안의 직접 또는 검증된 로컬 GOT 심볼 설명자 참조라는 세 개의 불변 부호 있는 상대 참조를 추적합니다. 레코드 경계, 플래그, 심볼 정체성과 전체 캐시·참조 이름 맹글링을 확인하며 내보내기와 필드 검색 예산을 각각 제한합니다. 생성 C는 로드된 내보내기에서 참조를 따라 원래 설명자 포인터를 재구성 레시피에 보존합니다. 비공개 이름을 문자열로 조회하거나 설명자 저장소를 복사하지 않습니다. 참조, 내보내기 또는 저장소 변경은 바인딩과 출력을 무효화합니다. AArch64/x64 모델 테스트와 함께 네이티브 Swift 대조가 O0/O2에서 설명자 및 Optional 타입 메타데이터 정체성을 검증합니다.

필드는 같은 설명자를 다른 제네릭 레시피로 감쌀 수 있습니다. 제한된 탐색으로 전체 필드 참조를 검증하고 정확한 원래 설명자 주소로 이어지는 경로만 사용합니다. 마지막 경로가 GOT를 거치면 불변이며 해석이 완료된 로컬 포인터 저장소도 필요합니다. 가져온 C typedef 래퍼는 버전 0의 비제네릭 외부 구조체 설명자, `__C` 모듈, 일치하는 ABI 이름과 완전한 `St` 가져오기 이름 공간 정보를 요구합니다. 같은 이름의 설명자가 여러 개 있어도 검증된 필드 경로가 원래 주소를 선택해야 합니다. 참조나 가져오기 정체성이 달라지면 게시를 거부합니다. 네이티브 Swift 사전 메타데이터와 설명자 정체성을 O0/O2에서 검증합니다.

Swift 구체 타입 레시피는 서술자의 직접 참조와 로컬 GOT 참조에 동일한 안정적 타입 식별 검증을 적용합니다. 간접 참조는 고유하게 매핑된 불변 저장소의 완전한 8바이트 포인터, 정확히 해결된 체인 재배치와 대상 소유 관계를 먼저 검증하며, 충돌하는 가져오기나 겹치는 재배치를 거부합니다. 해결된 서술자에도 기존 등록, 모듈, 컨텍스트, 이름, 타입 종류 검사 또는 가져온 잠금 타입 검증이 필요합니다. 생성 레시피와 새 캐시는 직접 참조 형식과 같으며 비공개 서술자 바이트를 복사하지 않습니다. AArch64와 x64 테스트는 클래스, 구조체, 열거형, 중첩 레시피, 오래된 출력 힌트 및 21가지 잘못된 저장소나 식별 정보 변형을 검증합니다.

AArch64 구체 타입 레시피는 완전히 디맹글된 `Swift` 모듈의 최상위 명목 타입 또는 프로토콜 선언과, 고유한 불변 가져오기 저장소에서 `/usr/lib/swift/libswiftCore.dylib`로 향하는 정확한 강한 바인딩 및 0인 가산값을 통해 표준 라이브러리 서술자 식별을 검증합니다. 개별 서술자 이름 목록을 대체하여 스칼라, 열거형, 클래스, 프로토콜 및 혼합 제네릭 레시피를 처리합니다. 캐시와 참조의 전체 타입 일치, 재배치 검사, 출력 시 재검증은 필수입니다. 접근자, 메타데이터 값, 중첩 또는 외부 모듈 선언, 약한 가져오기, 충돌하는 저장소는 거부하며 서술자 이름으로 호출 ABI나 인스턴스 배치를 추론하지 않습니다.

첫 형식 인자가 리터럴 `String`이고 두 설명자를 포함하는 완전한 14바이트 Swift 제네릭 레시피에서, 독립 설명자와 바깥 형식 이름의 치환 인덱스가 다르면 제한된 디맹글 형식 트리를 비교하여 캐시와 참조의 일치를 검증한다. 제네릭 종류, 두 인자 형식, 모든 모듈과 중첩 선언, 모든 노드의 텍스트와 인덱스를 인증된 설명자와 대조한다. 생성된 심볼 참조는 원래의 동일성을 유지한다. 다른 형식, 추가 인자, 잘못된 레시피와 유효하지 않은 설명자 증거는 계속 거부한다. O0/O2의 네이티브 Swift 실행으로 최종 메타데이터의 동일성을 검증한다.

동일한 제한된 비교는 두 명목 타입으로 구성된 레이블 없는 튜플의 완전한 12바이트 레시피도 지원합니다. 캐시 이름에 모듈 치환이 사용되어도 두 요소의 정체성과 순서를 검증합니다. 이 규칙은 레이블 없는 요소가 정확히 두 개여야 하며, 레이블이나 추가 요소 또는 다른 타입 트리는 일치할 수 없습니다. 설명자 인증, 원래 런타임 정체성, 새 공유 캐시 및 게시 시 재검증은 계속 필수입니다. 이러한 튜플 하나를 타입 인자로 갖는 명목 컨테이너의 완전한 19바이트 레시피도 지원하며, 외부 설명자와 두 요소 설명자를 각각 인증하고 컨테이너 종류, 인자 수 및 요소 순서를 검증합니다.

AArch64에서는 선언된 레지스터 매개변수가 최대 8개인 네이티브 보조 함수가 여러 구체 타입 메타데이터 쌍을 전달할 수 있습니다. 타입이 지정된 전체 함수 본문을 제한된 범위에서 검사하여 각 캐시/타입 참조 매개변수가 원래 값을 유지하고 기존 구체 타입 인스턴스화 함수의 직접 호출에만 사용되며 호출 양쪽의 ABI와 코드 식별이 일치하는지 확인합니다. 재할당, 산술 연산, 이스케이프, 역할 충돌, 간접 대상, 불완전한 제어 흐름 및 검사 한도 초과는 거부합니다. 인수 위치마다 별도의 쌍을 유지하고 호출자의 정의가 유일한 지역 변수에도 같은 증명을 적용합니다. 관련 없는 스칼라 인수는 비트 값이 같아도 이 바인딩을 상속하지 않습니다. 생성된 인스턴스화 함수, 전달 호출자 및 배열 보조 함수를 수정하지 않고 O0/O2에서 네이티브 Swift 튜플 버퍼로 실행 검증합니다.

생성된 Swift 외부 데이터 카탈로그는 네 가지 Darwin 컴파일러 및 SDK 내보내기 구성이 모두 일치할 때만 Foundation의 `String: CVarArg` 적합성 설명자를 포함한다. 별도의 Foundation 프로브는 컴파일러 병합으로 필수 직접 제네릭 호출이 간접 썽크로 바뀌는 것을 방지한다. 추출에는 정확한 비 TLS 설명자 선언, String 메타데이터, 지연 witness 접근자와 release 저장 캐시가 계속 필요하다. 소스 바인딩은 가져온 설명자의 주소를 보존하고 게시 시 정확한 제공자, 심볼 및 가산값이 0인 강한 바인딩을 다시 검증한다. witness 멤버 ABI나 설명자 레이아웃은 부여하지 않는다.

Swift 외부 데이터 생성기는 제네릭 레코드에서 타입 메타데이터를 인스턴스화하는 적합성 기술자도 처리합니다. 구체적·추상적 메타데이터 헬퍼, 메타타입 조회, 제약된 제네릭 호출, 지연 witness 캐시의 두 경로 전체를 비교하며 SSA와 레이블 이름 변경 및 의미에 영향을 주지 않는 컴파일러 힌트만 허용합니다. 모든 사용은 같은 불투명 타입 레코드를 공유해야 하며 네 컴파일러·내보내기 프로필이 일치해야 합니다. Combine의 `CurrentValueSubject: Publisher` 기술자가 이 경로를 사용합니다. 게시 시 정확한 강한 가져오기 제공자와 기술자 주소를 다시 검증하며, 기술자 레이아웃, witness 멤버 ABI, 런타임 인수 대체 또는 프레임 효과 권한을 추가하지 않습니다.

별도의 Swift 제네릭 컴파일 프로브는 모든 유효한 `Output`과 `Failure: Error`에 대해 `CurrentValueSubject: Publisher`가 `swift_getWitnessTable`의 세 번째 인자를 사용하지 않음을 증명한다. 네 컴파일러 및 내보내기 구성이 모두 일치해야 한다. 정확한 강한 conformance 가져오기, 완전한 세 포인터 runtime ABI, 단독 8바이트 `undef`가 모두 확인된 경우에만 소스 투영이 0을 선택한다. 설명자는 현재 가져오기 또는 유일하게 완전 대입되고 주소가 노출되지 않은 지역 정의에서 와야 한다. 부분 쓰기, 모호한 정의와 변경된 가져오기는 거부하며 게시 시 다시 확인한다. 메타데이터 식, runtime 호출, 캐시 연산과 부작용은 유지하고 순수성, 레이아웃, 프레임 계약은 부여하지 않는다. [Swift runtime](https://github.com/swiftlang/swift/blob/swift-6.1.2-RELEASE/stdlib/public/runtime/Metadata.cpp)은 조건부 요구 사항과 사용자 정의 인스턴스화에서 이 인자를 사용할 수 있으므로 전역 규칙으로 확대할 수 없다.

정확한 강한 가져오기인 `URL.path`와 `String.count`도 Swift 6.1.2의 네 컴파일러 및 SDK 내보내기 구성에서 일치해야 합니다. 경로 getter는 `swiftself`로 불투명한 URL을 읽고 String의 두 워드를 반환합니다. 문자 수는 두 워드를 일반 인수로 받아 정수 한 워드를 반환합니다. 게시 시 현재 가져오기와 전체 ABI를 다시 검증하며 값 배치, 순수성, 프레임 대여 계약을 추가하지 않습니다. 생성된 C와 원래 ARM64 호출을 O0/O2에서 실제 Foundation/Swift 연산과 비교하여 Unicode 문자소, 브리지 문자열, URL 결과 수명을 검증합니다.

정확한 강한 임포트 `AnyHashable.init<T: Hashable>`도 Swift 6.1.2의 네 컴파일러 및 SDK 내보내기 검증 결과가 일치해야 합니다. 완전한 Swift ABI는 불투명한 간접 결과 주소, 소비되는 값 주소, 타입 메타데이터, Hashable witness table 순서이며 어느 것도 `swiftself`가 아닙니다. 공통 Swift 선언 계층은 네 전달 값을 모두 유지하고, 게시 시 현재 제공자와 전체 ABI를 다시 검증합니다. 컴파일러는 결과 주소에만 `nocapture`를 지정하므로 이 선언은 입력 빌림, 값 배치, 프레임 효과 또는 순수성 계약을 제공하지 않습니다. 생성된 C는 O0/O2에서 원래 ARM64 호출 및 실제 Swift 생성, 소유권, 해시 연산과 비교 검증합니다.

SwiftConsumedInputEffects는 AnyHashable 생성이 소비하는 정확한 UInt 입력을 별도로 증명합니다. 네 가지 실제 컴파일러 및 SDK 내보내기 구성에서 8바이트 UInt 임시 저장소, 완전한 초기화, 정확한 메타데이터/위트니스 인수, 호출 직후 수명 종료를 확인합니다. 현재 ARM64 기계어/LowIR로 강한 런타임 가져오기와 두 불변 가져오기 로드를 인증합니다. 공유 프레임 분석은 8바이트 전체 초기화를 요구하고 이전 탈출과 확정적인 재기록 전의 후속 읽기를 거부하며 모든 호출 효과를 보존합니다. ABI는 불투명 포인터 네 개로 유지됩니다. 게시 시 제한된 직선 단일 호출 함수를 표준 파이프라인으로 다시 디코딩하고 진입점을 재추론하며 현재 MedIR, 저장된 HighIR, 게시할 HighIR의 모든 초기화, 메모리 효과, 인수와 유일한 실행 위치를 비교합니다. 증거 삭제나 저장된 두 표현의 동시 수정으로 현재 증명을 건너뛸 수 없습니다. 일반 입력 noescape, 결과 배치 또는 순수성을 부여하지 않습니다. 17개 명령의 전체 UInt 래퍼와 수정하지 않은 바인딩 C를 실제 Swift에서 O0/O2로 비교하여 카운터 순환, 스택 재사용 및 보호 영역을 검증합니다.

소비되는 UInt 입력의 상태 검사는 현재 인증된 UInt 호출 위치에만 적용합니다. 다른 타입이 같은 제네릭 AnyHashable 생성자를 사용해도 이 효과를 부여하지 않으며 일반 내부 ABI 추론을 막지 않습니다. 추론 전에 현재 LowIR에서 모든 UInt 증명을 다시 구성하므로 증명 삭제, 스칼라 반환 정의 또는 ABI 변경으로 초기화와 소비 수명 검사를 우회할 수 없습니다. 내부 타입 추론과 소스 게시 성공은 별도 판정입니다.

SwiftSDKDeclarations는 AnyHashable 원시 해시의 완전한 ABI인 일반 seed 인자와 불투명 Swift self를 관리합니다. Boolean 소유자는 두 일반 불투명 입력과 실제 swiftcc i1 결과를 갖는 동등 비교를 별도로 인증하며, 실행 결과를 전체가 정의된 바이트로 선언하지 않습니다. 네 컴파일러 및 SDK 내보내기 구성에서 두 선언을 확인합니다. 게시 시 현재 MedIR을 다시 실행해 기존 String과 NSObject 연산을 포함한 모든 Boolean 인자와 원래 호출의 유일한 발생 위치를 비교합니다. 값 배치, 비공개 프레임 대여, noescape 또는 순수성을 추론하지 않습니다. 완전한 5명령 해시 및 6명령 동등 비교 ABI 호출 코드를 수정하지 않은 바인딩 C와 실제 Swift에 대해 O0/O2에서 비교하며 seed 비트, 값 및 참조 타입, 별칭 입력과 저장 영역 보호를 검사합니다. 이 검증은 전체 WMF 사전 함수를 인증하지 않습니다.

SourceFrameAnalysis는 범용 레지스터의 완전한 진입 값과 복원해야 할 상태를 구분합니다. 호출, 정확한 전용 스택 저장, CFG 합류를 통해 휘발성 진입 바이트를 추적하면서 기존 보존 레지스터, 프레임, 링크의 복원 의무를 유지합니다. 호출에 의한 손상, 부분 쓰기, 경로 불일치는 동일성을 제거하며 루프는 수렴한 상태를 요구합니다. 네이티브 매개변수를 추가하려면 MedIR도 8바이트 전체를 독립적으로 관찰해야 합니다. 따라서 외부 진입 ABI나 새 프레임 효과를 가정하지 않고 호출에 직접 전달되는 ARM64 x8 결과 주소를 보존합니다. 완전한 5개 명령 호출자와 추론하여 생성한 원본 C를 O0/O2에서 실제 Swift 런타임으로 비교합니다.

네이티브 입력 추론은 바인딩된 호출에 암시적으로 전달되는 완전한 정수 워드도 고려한다. LowIR에는 ABI 인수 없이 호출 대상만 기록되므로 직접 꼬리 호출에서 보존된 컨텍스트가 누락될 수 있다. 기존 네이티브 상태 증명은 각 호출 위치를 대조하고 쓰기, 호출에 의한 파괴, 프레임 저장을 거쳐 진입 값의 8바이트 전체를 추적하며 상태 복원을 증명해야 한다. MedIR도 동일한 전체 진입 워드의 사용을 독립적으로 확인해야 한다. 부분 값, 덮어쓴 값, 모호하거나 바인딩되지 않은 값으로는 매개변수를 만들지 않는다. ARM64 및 x86_64 파이프라인 테스트는 재리프팅과 완전한 소스 검증을 요구한다. 원본 ARM64 꼬리 분기 명령과 수정하지 않은 생성 C를 O0/O2에서 비교하며, 계측용 피호출 함수로 두 입력과 두 반환 워드를 확인한다. 이는 전달 동작을 분리해 검증하며 사전 구현은 실행하지 않는다.

ARM64 소스 바인딩은 완전한 기기 및 시뮬레이터 SDK 선언이 외부 비 TLS `NSString *const` 저장소를 증명하고 두 UIKit 내보내기 표가 정확한 링커 식별자를 확인한 12개 서식 문자열 속성 키 전역 변수를 지원한다. 외부 저장소 주소와 모든 네이티브 읽기를 유지하며 문자열 내용이나 객체 값으로 대체하지 않는다. 잘못된 프레임워크, 변경된 심볼, 약한 가져오기, 0이 아닌 가산값과 오래된 게시 증거는 거부한다. 이 추가 목록은 x86_64 바인딩을 활성화하지 않는다.

비공개 Swift witness 테이블은 안정적인 등록 이름을 가진 내부 프로토콜도 사용할 수 있다. 공통 타입 식별 증명은 설명자, 전체 모듈 문맥과 모든 직접 `__swift5_protos` 레코드를 검사하며 중복 식별, 누락된 등록과 지원하지 않는 플래그를 거부한다. 요구 사항 시그니처나 연관 타입이 없는 일반 프로토콜만 허용한다. 생성된 C는 단순 존재 타입 메타데이터를 찾고 종류와 단일 프로토콜 배치를 검증하여 원래 프로토콜 설명자를 얻은 뒤 원래 클래스의 준수를 조회한다. witness 항목을 재구성하거나 내보내지 않은 설명자를 링크하지 않는다. 게시와 출력 생성 시 현재 이미지에 대해 식별 증명을 다시 수행한다.

구체 타입 레시피는 직접 참조와 인증된 로컬 GOT 참조에 동일한 내부 프로토콜 등록 정체성 증명을 재사용합니다. 레시피 자체의 실존 타입, 옵셔널 또는 배열 연산자를 유지하면서 안정적인 선언 이름을 재구성한 뒤 전체 캐시 타입과의 일치를 요구합니다. 비공개 프로토콜 심볼에 링크하거나 설명자를 복사하지 않습니다. 누락된 등록, 요구사항 시그니처, 연관 타입, 불완전한 레코드 및 오래된 정체성 정보는 계속 거부됩니다.

super 호출 증명은 좁은 반환값의 정의되지 않은 패딩을 유지하고 모든 인수를 검사합니다. 집계형, 가변 인수, 오래되거나 모호한 선언은 계속 거부합니다. 포인터 크기 값으로 구체화된 정확하고 완전한 클래스 또는 메타클래스 주소는 `objc_super`에 저장하는 경우에도 직접 수신자와 동일한 런타임 객체 식별 증명을 사용합니다. 클래스 참조 셀, 스칼라 즉시값, 불완전한 주소, 충돌하는 메타데이터는 이 바인딩을 얻지 못합니다. 공개 시 원래 클래스 식별을 다시 확인합니다.

UIButton의 `contentEdgeInsets`, `imageEdgeInsets`, `titleEdgeInsets` getter/setter는 32바이트 `UIEdgeInsets` 레코드를 보존합니다. 위, 왼쪽, 아래, 오른쪽의 double 값은 arm64에서 d0–d3로 전달됩니다. 전체 기기 및 시뮬레이터 SDK 선언이 일치하며 Apple Clang으로 여섯 인코딩을 독립적으로 재현합니다. 수신자 조회는 UIButton의 익명 카테고리와 UIButton → UIControl → UIView 상속 관계를 유지합니다. 런타임 선언 충돌, 다른 수신자, 클래스 메서드, 잘못된 제공 라이브러리, 일치하는 근거가 없는 아키텍처는 계속 지원하지 않습니다.

`windows-pe64-v1`은 PEB/TEB, 정적·동적 TLS, `DllMain`, 이름 기반 Win32 API 및 명시적 비순환 DLL 그래프를 갖춘 제한된 Windows x64/ARM64 콘솔 프로세스를 지원합니다. 게스트 모듈은 이름/서수 코드·데이터 가져오기, DIR64 재배치, 전달 내보내기 및 실제 로더 목록 식별자를 지원합니다. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary`, `GetProcAddress`는 설정된 모듈 카탈로그를 사용합니다. CRT/GUI, 스택 프레임 기반 사용자 SEH, 스레드 및 일반 Windows 앱 호환성은 미완성이며 네이티브 ARM64 KVM/WHP 증거도 아직 없습니다.

`readPEProgramExports`는 원본 내보내기와 읽기 범위를, `WindowsProcessModules`는 그래프와 프로세스 공통 제공자/이름 API 게이트를 소유합니다. `VirtualMemory`가 모든 이미지를 먼저 예약하고 `AddressSpace`가 페이지와 권한을 관리합니다. PEB/LDR에는 실제 이미지만 있으며 초기화 목록은 로더 등록 순서입니다. 등록 순서와 의존성에 따른 attach 호출 순서를 별도로 유지합니다. `GetModuleHandleW`는 NULL 또는 ASCII 기본 이름을 받으며 대소문자를 무시하고 확장자가 없으면 `.dll`을 붙입니다. 경로, 비 ASCII 조회, 끝의 점 규칙은 미지원입니다. 없는 이름은 오류 126, 성공은 LastError를 유지합니다. API 모델은 설치된 DLL이 아닙니다.

`WindowsProcessLifetime`은 같은 CPU와 실행 예산에서 의존 순서대로 DLL TLS 콜백과 `DllMain`, 이어서 EXE TLS와 진입점을 실행합니다. 모듈마다 독립 TLS 인덱스와 정렬된 블록을 할당하고 재배치·연결된 이미지에서 공용 64 KiB 영역으로 복사합니다. TLS 예약 인수는 0이며 시작/프로세스 종료 `DllMain`은 불투명한 비 NULL 값을 받습니다. 명시적 프로세스 종료는 초기화를 완료한 DLL을 로더 목록의 역순으로 분리한 뒤 EXE TLS를 호출하며 EXE 초기화 전에도 같습니다. 시작 `DllMain(FALSE)`는 분리 통지 없이 `0xc0000142`로 종료합니다. 오류와 예산 소진은 가짜 정리를 수행하지 않습니다. 게스트 DLL이 있는 PE 진입점 반환은 미지원 스레드 종료가 필요하므로 명시적으로 중단합니다. 0이 아닌 `SizeOfZeroFill`은 미지원이며 실제 TLS 템플릿의 0으로 초기화된 바이트는 지원합니다. 진입점 없는 DLL은 TLS attach를 받지만 프로세스 detach 통지는 받지 않습니다.

`WindowsProcessExports`는 정적 가져오기와 `GetProcAddress`에 같은 이름/서수 해석을 사용하여 코드, 데이터, 별칭, 연쇄 전달을 처리합니다. 실제로 참조하는 시작 전달만 카탈로그 모듈과 초기화 의존성을 추가하며 사용하지 않는 전달은 파일을 읽지 않습니다. 이름은 대소문자를 구분하며 없는 이름은 NULL/오류 127, 직접 조회한 없는 서수는 빈 슬롯을 포함해 NULL/오류 182, NULL 조회 인수는 오류 87을 반환하고 성공은 LastError를 보존합니다. 알 수 없는 모듈 핸들은 미지원입니다. 제한된 API 목록에서 정확한 제공자/이름별 진입점을 한 번 예약합니다. 각 이미지의 현재 PE 헤더와 내보내기 메타데이터를 검사하고 변경 또는 읽을 수 없는 바이트를 거부합니다. 체인은 최대 64개이며 준비 단계의 남은 메타데이터 예산과 실행 기한을 공유합니다. 빈 슬롯으로 전달하면 대상 이미지 기준 주소를 반환하고 LastError를 보존합니다. 서수 0으로 전달하면 오류 87을 반환합니다. 기준 주소는 데이터 주소이며 이미지 헤더 실행 권한을 부여하지 않습니다. 런타임 전달은 설정 카탈로그의 모듈을 로드하고 초기화를 마친 후 조회 결과를 반환할 수 있습니다. 실행 중 내보내기 테이블 변경은 여전히 지원하지 않습니다.

`WindowsProcessLoader`는 `windows.modules`의 ASCII DLL 기본 이름을 로드하며 명시적 참조, 공유 의존성과 시작 모듈 유지를 관리합니다. 전달 조회를 반복해도 참조가 추가되지 않습니다. 다시 로드할 때 카탈로그 슬롯에 새 상주 세대를 부여합니다. TLS와 `DllMain`은 같은 CPU에서 중단된 API 프레임 아래에서 실행되며 레지스터 복원은 게스트 메모리 쓰기를 보존하고 현재 반환 주소를 사용합니다. 동적 attach/detach 예약 포인터는 0입니다. 명시적 로드 중 attach 실패는 정리 후 오류 1114를 반환하되 성공한 독립 중첩 로드는 유지합니다. 언로드는 이미지 매핑과 TLS를 해제하고 재로드는 원본 내용을 복원합니다. 모델 밖에서 로더 목록이나 TLS 포인터를 바꾸면 명시적으로 실패합니다. 파일·이미지·메타데이터 작업 예산은 실패와 재로드에도 누적됩니다. 시스템 제공자는 매핑된 PE 베이스를 모듈 핸들로 사용합니다. 파일 시스템 검색, 비 ASCII 경로, `LoadLibraryEx` 플래그, 순환 가져오기, 초기화 또는 언로드 중인 같은 모듈의 재진입 상태 전환은 지원하지 않습니다.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` 는 PEB 프로세스 매개변수의 실제 게스트 환경 블록을 공유합니다. 이름은 대소문자를 구분하지 않는 ASCII이며 값은 UTF-16입니다. 변경 전에 입력, 용량, 쓰기 가능한 메모리를 검증합니다. 스냅샷은 이후 변경과 독립적이며 해제하면 게스트 메모리를 회수합니다. 모델의 블록 한도는 64 KiB이고 문자열과 확장에는 크기 및 실행 기한 검사가 적용됩니다. 알 수 없는 포인터 소유권, 잘못된 블록, ANSI 코드 페이지, 확장 버퍼 중첩은 지원하지 않습니다. `WindowsEnvironmentTests.cpp`는 사용 가능한 백엔드에서 자체 x64/ARM64 픽스처를 비교하며 CI는 독립적인 네이티브 Windows 오라클을 필수로 실행합니다.

`WindowsProcessHeap`은 프로세스 힙의 할당, `HeapReAlloc`, 해제와 크기 조회를 통합 관리합니다. 크기 변경은 유지되는 데이터를 보존하며 `HEAP_ZERO_MEMORY`는 추가 바이트를 0으로 만들고 `HEAP_REALLOC_IN_PLACE_ONLY`는 이동을 금지합니다. 재할당 실패 시 기존 블록을 보존하고 NULL을 반환하며 `ERROR_NOT_ENOUGH_MEMORY`(8)를 설정하여 네이티브 관측과 일치합니다. 독립적인 페이지는 축소와 해제 시 용량을 반환하며 단계별 확장과 제한된 복사는 실행 기한을 확인합니다. 사용자 정의 힙, 예외 생성 플래그, 알 수 없는 소유권, 접근 불가능한 복사 또는 초기화 범위는 명시적으로 중단합니다. `WindowsHeapTests.cpp`는 두 ISA, 강제 이동, 예산 재사용, 실패 원자성을 검증하며 CI는 동일한 자체 EXE를 네이티브 Windows에서도 실행합니다.

`WindowsSystemModules`는 두 ISA에 대해 `ntdll.dll`, `kernelbase.dll`, `kernel32.dll`의 제한된 PE64 모델 이미지를 만듭니다. ASCII `GetModuleHandleA` / `GetModuleHandleW`, `LoadLibraryA` / `LoadLibraryW`, `GetProcAddress`는 매핑된 베이스를 공유하며 PEB/LDR과 `MEM_IMAGE`도 같은 이미지를 나타냅니다. 정적 가져오기, 이름 조회와 게스트 DLL 전달은 동일한 API 게이트와 내보내기 해석기를 사용합니다. 제공자는 고정 상주하고 게스트 초기화 콜백이 없으며 일반 게스트 DLL을 모두 해제한 뒤 진입점 반환을 막지 않습니다. 헤더 또는 내보내기 메타데이터가 바뀌면 조회를 중단합니다. 미지원 시스템 내보내기 이름과 0이 아닌 서수 조회는 명시적으로 중단하며 지원 이름의 대소문자 불일치와 빈 이름은 오류 127, NULL 조회는 87을 반환합니다. 생성 바이트와 주소는 모델 정책이며 Windows DLL 버전별 배치, 네이티브 서수와 제공자 간 별칭은 재구성하지 않습니다. `WindowsSystemTests.cpp`는 자체 x64/ARM64 EXE를 네이티브 Windows와 비교하고 초기 스레드 반환을 독립적으로 8회 관측합니다.

`WindowsProcessExceptions`는 같은 CPU와 프로세스 예산으로 `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler`, `RaiseException`을 구현합니다. 순서가 있는 처리기는 등록·삭제, 중첩 예외, 모델 API 호출, DLL 로드와 프로세스 종료를 수행할 수 있습니다. x64/ARM64 데이터 접근 위반과 x64 정수 나눗셈 예외는 게스트의 `CONTEXT` 변경을 검증한 뒤 재개하며 범용 레지스터, SIMD 및 지원 FP 상태를 보존합니다. 소프트웨어 예외는 모델 공급자 내부의 실제 반환 명령으로 재개합니다. 보관된 등록은 128개, 중첩은 16프레임으로 제한합니다. 잘못된 처리 결과, 바뀐 예외 포인터, 미지원 필드와 한도 초과는 명시적으로 실패합니다. 스택 프레임 기반 SEH/언와인딩, 디버거 전달과 실행/가드 페이지 예외는 미지원입니다. `WindowsExceptionTests.cpp`는 자체 EXE/DLL을 네이티브 Windows와 비교하며 ARM64 KVM/WHP 실기기 증거는 아직 없습니다. 소프트웨어 예외 레코드에는 `EXCEPTION_SOFTWARE_ORIGINATE`(`0x80`)가 포함되며 호출자의 계속 불가 플래그와 별도로 처리됩니다. 원본 Windows 실행 파일은 소프트웨어 및 하드웨어 예외의 정확한 플래그 값을 검증합니다.

`AddVectoredContinueHandler`와 `RemoveVectoredContinueHandler`는 독립된 순서 목록을 관리하며 예외 처리기와 최대 128개 보존 등록 제한을 공유합니다. 벡터 예외 처리기가 실행 재개를 수락하면 계속 처리기는 같은 수정 가능한 예외 레코드와 `CONTEXT`를 봅니다. 중첩 예외와 DLL 알림을 포함한 콜백이 끝난 뒤 최종 컨텍스트를 검증합니다. 다른 종류의 처리기 API로 핸들을 제거할 수 없습니다. `WindowsContinuationTests.cpp`는 순서, 조기 종료, 등록 변경, 컨텍스트 복구, 중첩 전달, 로더 콜백과 프로세스 종료를 독자 EXE와 네이티브 Windows로 비교합니다. 검증한 Windows x64 벡터 경로에서는 `EXCEPTION_NONCONTINUABLE`이 설정되어도 재개할 수 있지만 스택 프레임 기반 SEH 동작의 근거는 아닙니다. 네이티브 ARM64 실행은 아직 검증하지 않았습니다.

Windows 가상 메모리는 `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery`와 현재 프로세스의 `FlushInstructionCache`를 지원합니다. OS 계층은 예약 영역을 소유하고 `AddressSpace`는 커밋된 페이지, 권한, 실제 저장 공간을 관리합니다. 테스트는 동적 코드 수정, 접근 오류, 메모리 한도 재사용을 검증합니다.

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

네이티브 의존성 탐색은 현재의 완전한 LowIR과 불변 명령이 정확히 해석된 체인 코드 포인터 슬롯을 증명할 때만 ARM64 간접 호출을 따라갑니다. 별도 코드 포인터 판독기는 유일한 읽기 전용 저장소, 충돌하는 수정 정보, 현재 함수 진입점을 확인하며 일반 데이터 포인터 판독기의 기존 경계를 유지합니다. 제한된 추적은 하나의 기본 블록 안에서만 수행되고 호출을 넘어 레지스터 값을 유지하려면 특정 레지스터를 사용하는 ARC 임포트를 포함한 현재 런타임 또는 네이티브 ABI가 필요합니다. 프레임 재로드, 알 수 없는 호출, 불완전한 증거는 미해결 상태로 남습니다. 의존성 목록은 원래 간접 호출 위치를 보존하며 그 자체로 ABI를 바인딩하거나 소스 공개를 허용하지 않습니다.

동일한 불변 네이티브 호출 증명으로 SSA 이전에 현재의 완전한 스칼라 `NativeAnalysis` ABI를 바인딩하며, LowIR/MedIR에는 원래 간접 호출 연산과 발생 위치를 보존합니다. 네이티브 상태 추론은 현재 LowIR에서 증명을 다시 구성하고 일반 호출의 레지스터 손상 규칙과 프레임 검사를 유지합니다. HighIR은 증명된 불변 대상 평가만 선택한 소스 정의로 투영합니다. 게시 단계에서는 호출자와 피호출자의 현재 LowIR, MedIR, HighIR 및 승인된 감사의 일치를 별도로 요구하고, 슬롯·명령·ABI를 다시 검증하여 각 원래 바인딩 호출이 정확히 한 번 평가되는지 확인합니다. 저장된 힌트나 의존성 목록은 게시를 승인하지 못합니다. 누락되거나 오래된 증거, 중복, 충돌은 여전히 지원하지 않으며, 각 피호출자의 완전한 소스 본문과 의존성 폐쇄도 검증해야 합니다.

`SourceFrameEffects`는 범위가 제한된 동기 프레임 대여와 프레임 또는 외부 저장소를 가리킬 수 있는 반환 별칭을 로더와 파이프라인 증명에서 공유한다. ARM64 Swift 값 버퍼 투영기는 완전한 불변 본문, 원래 BL/LowIR 호출 위치, 현재 두 매개변수 네이티브 ABI, 강한 `swift_makeBoxUnique` 임포트를 검증해야 한다. 버퍼의 세 워드는 보수적으로 무효화된다. 반환값은 버퍼 시작 주소 또는 외부 저장소일 수 있으며 저장된 바이트의 동일성을 증명하지 않는다. 복사와 합류도 프레임에서 유래했을 가능성을 유지한다. 이후 대여는 살아 있는 범위 안에 있어야 하며 부분 포인터, 이탈, 수명이 끝난 프레임, 불확실한 반환값을 통한 저장 레지스터 복원은 거부된다. 스칼라 반환 추론도 이 증명을 다시 검사한다. [Swift 6.1.2 런타임 계약](https://github.com/swiftlang/swift/blob/swift-6.1.2-RELEASE/stdlib/public/runtime/HeapObject.cpp)에 따라 할당, 값 위트니스 복사와 해제는 관측 가능한 효과로 남는다. 이는 순수성이나 존재 타입 호출의 완전한 복원을 뜻하지 않는다.

`SourceFrameAnalysis`는 IR 구성 요소에서 LowIR 바이트 동일성, 프레임 주소 탈출, 호출 효과와 CFG 병합을 관리하고 pipeline은 MedIR 어댑터를 유지합니다. 제한된 ARM64 프레임 로드 질의는 원래의 완전한 8바이트 LowIR 정의와 프레임 오프셋을 반환하며 모든 도달 경로에서 동일한 순서의 바이트를 요구합니다. 루프는 모든 입력 상태가 수렴한 뒤에만 인정하며, 질의 지점으로 돌아올 수 있으면 로드 이후의 효과도 검증합니다. 정의는 비순환 진입 접두부에 있어야 하고 같은 명령의 반복 실행을 같은 값으로 간주하지 않습니다. 저장 우회, 부분 쓰기, 만료된 슬롯, 이전 프레임 탈출과 해제되지 않은 보존 scratch를 거부하며 Swift 접근 플래그 조건을 유지합니다. 질의에 도달하지 않는 호출에는 계약이 필요하지 않습니다. 동적 스택 할당에는 별도 증명이 필요합니다. 결과 자체는 주소, 코드 포인터나 소스 공개 권한을 부여하지 않으며 사용 측에서 원래 명령, CFG, 슬롯과 호출 대상을 다시 검증합니다. 원래 ARM64 루프와 수정하지 않은 생성 C를 O0/O2에서 비교합니다.

ARM64 Swift 값 증인의 스필 추적은 불변 네이티브 호출 대상과 같은 loader 소유자 `AuthenticatedSourceFrameLoads`를 사용한다. 도달 바이트, 이스케이프와 호출 효과는 IR이 담당하고, loader는 현재 기계 명령과 CFG 간선을 다시 검증한다. 기존의 독립 스필 스캔은 기계 어댑터가 준비될 때까지 x64에만 남긴다. 프레임에서 유래한 증명은 원래 간접 호출 위치만 식별한다. 네이티브 추론은 스칼라 반환에서도 증명을 반복하며, 게시 시 표준 Med-to-High 변환을 재실행하여 전체 ABI, 동적 대상, 메타데이터와 나머지 인수, 위치마다 정확히 한 번의 평가를 확인한다. 이 확장은 동적 스택 할당이나 새로운 값 증인의 메모리·noescape 효과를 허용하지 않는다. 함수 본문이 반환해야 한다는 조건은 프레임에서 유래한 호출에만 적용한다. 레지스터에서 유래한 값 증인은 반환하지 않는 호출이나 트랩 앞에 올 수 있다. 현재 LowIR 바인딩을 다시 구성하므로 프레임 증명을 삭제해도 검사를 우회할 수 없다.

ARM64 Objective-C 컨텍스트 thunk는 같은 프레임 효과 모델을 사용합니다. 완전하고 불변인 컨텍스트 읽기와 꼬리 분기는 현재 선언이 일치하는 강한 가져오기의 selector stub에 도달해야 하며, 전달하는 모든 물리 인자는 완전한 네이티브 ABI와 일치해야 합니다. 선택적 카운터 갱신은 유일하게 매핑된 쓰기 가능한 이미지 저장소로 제한됩니다. 이 증명은 컨텍스트 첫 8바이트를 동기적으로 빌리는 것만 허용하며 주소를 보관할 수 없습니다. 호출자는 이 바이트나 다른 메모리에 사유 프레임 주소를 저장하는 것도 거부하므로 읽어 낸 receiver를 통한 탈출도 허용하지 않습니다. 메시지, 객체 효과, 카운터 갱신은 관찰 가능한 상태로 남습니다. 직접 BL과 불변 간접 호출 위치는 스칼라 결과 추론에서도 현재 LowIR로 다시 검증합니다. 이후 테이블 재읽기와 존재 타입 정리는 별도의 증명이 필요합니다.

`immutableNativeCallTargets`는 공유 프레임 쿼리로 ARM64 테이블 기준 주소의 완전한 재로드를 증명합니다. 먼저 표준 디코더로 불변 명령을 다시 리프트하고 STORE/LOAD 피연산자를 포함한 현재 LowIR와 기계어에 인코딩된 모든 CFG 후속 간선을 확인합니다. 원래 정의는 기존 ADRP/ADD 및 코드 포인터 슬롯 증명을 다시 통과해야 합니다. 제한된 단조 반복은 이전 반복에서 증명된 대상과 현재의 완전한 ABI만 사용합니다. 공유 Swift 접근, 값 버퍼, Objective-C 컨텍스트 효과는 조건, 별칭 규칙, 수명 의무를 유지합니다. 블록 순서는 증거가 아닙니다. 호출은 원래 간접 호출 위치를 유지하며, 소스 게시에는 현재 callee 감사와 이후 정리 작업의 독립적인 증명이 여전히 필요합니다.

직접 꼬리 호출의 LowIR 연산은 IR 계층의 단일 정의인 `directTailCallOperations`를 CFG 구성과 불변 기계 명령 재검증에서 공유합니다. ARM64 재검증은 원래의 무조건 `B` 대상이 현재 인증된 함수 진입점이며 모든 소속 블록 밖에 있을 때만 허용합니다. 이후 제한된 예산 안에서 정규 `CALL + RETURN` 연산 전체, 명령 경계와 CFG 후속 블록을 비교합니다. 바이트, 피연산자, 제어 정보 변경과 내부 대상, 간접 전송, 불완전한 분석 범위는 거부합니다. 이는 기계 의미의 일치만 증명하며 숨은 인자 ABI, 동적 대상, 프레임 효과 또는 소스 공개 권한을 부여하지 않습니다. 별도의 디버그 선언이 없는 고정 소스 선언에서 HighC는 본문 분석 전에 각 무명 인자에 충돌 없는 표시 이름을 할당합니다. 함수 정의, 인자 사용, 지역 선언 제외는 같은 매핑을 사용하며 소스 ABI와 원래 HighIR 이름은 유지합니다.

병합된 Objective-C BOOL setter는 형식을 독립적으로 확인한 꼬리 호출자에 한해서 투영합니다. 호출자·helper·클래스 접근자의 완전한 불변 명령과 현재 LowIR, 파이프라인 감사, selector 선언, 클래스 식별 및 공유 카운터 저장소가 일치해야 합니다. BOOL의 정의된 바이트와 숨겨진 두 주소 인자는 이 증명에서 얻으며 병합 Swift 심볼로 추측하지 않습니다. `ObjCMergedSetterSources`는 접근자의 실제 반환값, selector 읽기, retain 반환값, 상위 클래스 메시지, 카운터 갱신, 실행 시 masked-isa 가상 호출과 release 순서를 보존합니다. `SwiftVirtualSlot`이 네이티브 호출자와 이 투영에 완전한 void/swiftself 슬롯 선언을 공통으로 제공합니다. 기존 IR 프레임 분석이 16바이트 objc_super의 동기 대여와 상태 복원을 검증합니다. 게시 시 현재 증거, 저장 내용, 정확한 소스 인자, 접근자 의존성 및 helper의 단일 평가를 다시 확인하며 HighC는 사용 전에 선언을 출력합니다. 전역 helper ABI, 고정 가상 구현 또는 일반 프레임/noescape 권한을 부여하지 않습니다.

Swift 값 위트니스 바인딩은 메타데이터 리터럴 주소와 이미지 전역 변수 또는 가져오기 슬롯에서 읽은 메타데이터 포인터를 구분합니다. 리터럴 주소에는 이미지에 있는 정확한 위트니스 테이블 접두부가 필요합니다. 읽은 포인터의 동일성은 해당 읽기가 만든 런타임 값을 기준으로 하며, 모든 도달 경로에서 위트니스 조회와 메타데이터 인수가 같은 값을 공유해야 합니다. 전역 변수의 내용을 고정하지 않습니다. 별도 읽기, 호출에 의한 손상, 부분 전달자, 루프의 반복 관측은 동일성을 증명하지 않습니다. 이 증명은 기존 호출 ABI만 제공하며, 재배치 가능한 데이터 선언과 제한된 프레임 효과는 별도로 증명해야 합니다.

네이티브 Swift 클래스 가상 호출은 loader의 완전한 클래스 메서드 ABI 분류기를 공유한다. 모든 도달 CFG 경로의 제한된 레지스터 출처 분석과 표준 디코더의 재리프팅으로 진입점 `swiftself`, 마스크 처리한 isa, 원래 간접 호출 위치 및 일치하는 void 슬롯 선언을 인증한다. 관련 순환, 부분 값, 호출에 의한 손상, 메타데이터 충돌과 오래된 명령은 거부한다. 공개 시 현재 호출자의 LowIR/MedIR/HighIR와 감사를 다시 검사하고, 정확한 동적 SSA 대상과 진입점 self 인수를 유지하며 각 호출 위치를 한 번만 평가하도록 요구한다. 실제 구현은 실행 중인 가상 테이블이 선택한다. 불리언 정규화는 이 진입점 및 호출 ABI와 포인터 인수만 받는 void 메시지를 포함한 현재 Objective-C 선택자 선언의 일치를 재사용한다. 공유 IR 증명은 동적 대상 전체와 인수를 함께 검사하고 실제 Swift `i1` 계약을 유지한다. 프레임 대여나 noescape 권한은 추가하지 않는다.

CoreText의 `CTFontGetSize`와 `CTFramesetterCreateWithAttributedString`은 기존 컴파일러 기반 C 선언 카탈로그를 사용합니다. 네 가지 macOS/iOS 전처리 구성이 일치하고 정확한 CoreText 제공자가 해당 심벌을 내보내야 합니다. 두 함수 모두 불투명 포인터 하나를 받으며, 전자는 8바이트 부동소수점 값을, 후자는 불투명 포인터를 반환합니다. 바인딩과 게시 시 아키텍처별 전체 ABI와 가져오기 식별 정보를 다시 검증합니다. 이 선언은 메모리, 수명 또는 비탈출 효과를 추가하지 않습니다.

Combine의 정확한 강한 가져오기인 `CurrentValueSubject` 초기화 생성자는 ARM64 및 x86-64 macOS와 Mac Catalyst에서 확인한 Swift 6.1.2 ABI를 사용합니다. 소비되는 불투명 값의 주소는 일반 인수 레지스터에, 할당된 인스턴스는 `swiftself`에, 포인터 결과는 정수 반환 레지스터에 배치됩니다. 게시 시 제공자와 전체 ABI를 다시 검증합니다. 이 선언은 제네릭 값의 레이아웃을 추론하거나 전용 스택 프레임 차용 효과를 부여하지 않습니다.

Combine의 정확한 강한 가져오기인 `Publisher.sink(receiveValue:)`의 `Failure == Never` 오버로드는 클로저 코드와 컨텍스트, Publisher 메타데이터와 witness table, `swiftself`로 전달되는 불투명 Publisher 주소의 다섯 포인터를 받고 `AnyCancellable` 포인터를 반환합니다. `AnyCancellable.store(in: Set<AnyCancellable>)`는 변경 가능한 Set 주소와 `swiftself`의 객체를 받고 void를 반환합니다. 두 선언은 ARM64 및 x86-64 macOS와 Mac Catalyst의 Swift 6.1.2 컴파일러 증거를 사용하며, 게시 시 현재 제공자와 전체 ABI를 다시 검증합니다. 이 선언은 제네릭 레이아웃, 클로저 수명 또는 전용 스택 프레임 차용 효과를 추론하지 않습니다.

불변 Swift 정적 스칼라 객체는 제한된 구조화 저장소 선언과 현재의 전체 객체 범위가 일치할 때만 재구성된 하나의 주소 정체성을 유지할 수 있습니다. 명목 타입과 제네릭이 아닌 확장 컨텍스트를 지원합니다. Darwin arm64/x86_64의 동결된 `CoreGraphics.CGFloat` 선언은 객체 크기가 8바이트임을 보장하며([Apple ABI 설명](https://developer.apple.com/documentation/corefoundation/cgfloat-swift.struct/nativetype)), 네 가지 macOS/Mac Catalyst 컴파일 대상에서도 독립적으로 확인했습니다. `SwiftMetadata`가 선언과 유일한 불변 저장소 증명을 소유하고 소스 바인딩과 게시에서 이를 재사용합니다. 가변, 중첩, 재배치, 부분, TLS, 제네릭 또는 모호한 객체는 계속 미해결 상태로 남습니다. 정렬된 바이트 도우미는 전체 비트와 공유 주소를 보존하지만 접근자 ABI, 프레임 빌림 또는 noescape 권한은 추론하지 않습니다.

명시적 인수가 없는 네이티브 Swift 클래스 메서드는 loader와 C API에서 하나의 완전한 ABI 선언 소유자를 공유한다. `NativeSwiftSelf` 수신자 사실에는 현재 진입 선언, 일치하는 클래스 메타데이터, 모든 기계 명령과 CFG 간선의 정규 재리프팅이 필요하다. 복사, 덮어쓰기, 합류는 기존 레지스터/바이트 분석을 사용한다. ObjC 인코딩이 비어 있는 객체 필드는 `SwiftMetadata`가 경계가 확인된 kind-7 반사 레코드, 완전한 필드 타입, 클래스와 상위 클래스 설명자, ObjC ivar, 오프셋 벡터, 정확한 필드 오프셋 심볼을 독립적으로 대조한다. 생성 코드는 실행 시 ivar 오프셋을 읽으며 초기 바이트로 상속 레이아웃을 고정하지 않는다. 게시 단계는 현재 LowIR 힌트를 다시 만들고 완전한 ABI, 승인된 감사, 정규 HighIR 인수, 정확한 소스 self/필드 경로와 단일 평가를 검사한다. 모호한 레코드, 부분 값, 경로 충돌, 저장소 변경, 오래된 증거는 거부한다. 필드 식별은 복사본 저장소, 프레임 대여, noescape 또는 순수성 권한을 부여하지 않는다. `CALayer.setTransform:`에는 128바이트 인수 복사본의 별도 증명이 필요하다. O0/O2에서 원래 ARM64와 수정하지 않은 생성 C를 실제 ObjC/CALayer 호출, 변경된 실행 시 필드 오프셋, nil 필드 값으로 비교한다.

유한한 다중 목적지 간접 분기도 지연 조건 의존성을 보존합니다. 합류 후 목적지 집합이 유한해도 실행 불가능한 경로가 포함될 수 있습니다. 실패 후 역방향 탐색은 새 필드·컨텍스트·생산자 요구 비트를 추가할 수 없는 조건을 지나 유용한 외부 조건을 찾습니다. 후보 선택과 활성화는 같은 규칙과 탐색 예산을 사용합니다. 후보만으로 간선을 제거하지 않으며, 결과 공개에는 전체 도달 가능 그래프의 새로운 증명이 필요합니다. 유한 분기의 의존성 탐색은 기존 정밀도 개선이 더 진행되지 않을 때 시작합니다. 활성화는 같은 누적 작업 한도 안에서 새 개선 한 번을 사용하므로 관련 없는 선택자가 기존 생산자나 네이티브 조건의 개선 예산을 먼저 소모하지 않습니다. 조건 후보가 소진되어도 지연된 생산자와 다른 정밀도 개선 절차는 계속할 수 있으며, 실패 해소에는 새로운 증명이 필요합니다.

HighIR은 호출 꼬리를 복제하기 전에 `SourceCallTypeHint::requiresUniqueSourceOccurrence`를 확인한다. 불리언 결과, 콜백 인자, 불변 대상, 프레임 값 증인, 가상 디스패치 및 네이티브 Swift 수신자 증거는 각각 원래 기계 호출 한 번을 가리킨다. 반환 꼬리, 점프 꼬리 및 중첩 출구 변환은 경로가 상호 배타적이어도 평가 위치를 공유한 채 유지한다. 일반 호출 선언은 기존 복제 규칙을 따른다. 게시 단계는 현재 기계 코드, ABI, 피연산자와 동적 대상을 다시 증명하고 소스 평가가 한 번인지 확인한다. 모든 증거 종류, 중첩 표현식, 일반 호출 최적화를 검사하며 완전한 ARM64 공유 저장 및 콜백 꼬리를 O0/O2에서 생성 C와 비교한다.

공유 `SourceABI`는 논리적인 값 전달 레코드와 물리적인 주소 운반자를 구분한다. 6개 또는 16개의 double을 갖는 Darwin ARM64 C 선언은 전체 레코드 타입을 유지하고, 부동소수점 인수 레지스터 및 x8 결과 포인터와 독립적인 8바이트 정수 레지스터나 자연 정렬 스택 슬롯을 사용한다. ABI 비교와 투영 그룹은 이 구분, 호출 규약, 인수 역할을 유지하며 부분 값, 겹침, 불일치, 오래된 운반자를 거부한다. 선언만으로는 LowIR 호출 바인딩, 진입 투영, HighC 호출 출력이나 프레임 대여를 허용하지 않으며 별도의 복사본 저장소 증명이 필요하다. 컴파일러 및 네이티브 ABI 테스트는 레지스터 소진, 스택 배치, 임의의 부동소수점 비트, 복사본 변경 격리, 독립적인 간접 반환을 검증한다. 완전한 `setTransform:` 복원 테스트는 아니다.

공유 `SourceFrameAnalysis`는 소비자와 완전한 간접 결과 생산자에 대한 독립적인 효과 증명이 있을 때 원래 호출 위치의 초기화된 비공개 값 복사본을 검증합니다. 모든 도달 경로와 루프 역방향 간선, 정확한 유효 범위, 다른 인수의 별칭과 이후 사용을 확인합니다. 복사본을 소비하면 초기화 사실과 저장된 바이트의 동일성이 무효화되며, 다시 읽으려면 새로운 확정 쓰기가 필요합니다. 쓰기 가능성과 프레임 해제도 초기화 사실을 지우지만, 유지 중인 Swift scratch의 수명 의무는 보존합니다. 질의는 전체 프레임 복원을 증명한 뒤 인수 범위만 반환하며 기계 코드 동일성, SDK 효과, 호출 바인딩이나 소스 공개 권한을 부여하지 않습니다. 분기, 루프, 부분 쓰기, 별칭과 유지 중인 scratch를 테스트합니다. 실제 ObjC/CALayer를 사용하는 ARM64 사례는 소비된 복사본 읽기를 거부하고, 독립적으로 초기화가 증명된 일회용 복사본을 허용합니다.

Objective-C 소비자는 공유 프레임 분석이 원래 호출에서 일회용 복사본의 전체 수명을 증명할 때만 간접 값 전달 레코드를 바인딩한다. 논리 레코드 선언과 물리 포인터는 별도로 유지한다. 현재 정규 기계어/LowIR, 독립적인 Swift 진입 및 필드 수신자 선언, 중간 호출의 모든 ABI, 별도 소유자가 증명한 `objc_msgSendSuper2`와 완전한 행렬 반환 효과를 요구하며 재귀나 프레임 규칙 중복을 만들지 않는다. 게시 시 다시 검증하고 초기화와 이후 사용을 포함한 직선 HighIR 본문 전체를 정규 재실행과 비교한다. 오래되거나 삭제된 증명과 중복 평가는 거부한다. HighC는 한 번의 memcpy 스냅샷으로 논리 레코드를 구성한다. 첫 소비자는 ARM64 고정 비공개 복사본만 지원하며 임의 포인터, 동적 스택, 일반 noescape를 허용하지 않는다. O0/O2 네이티브 테스트는 수정하지 않은 생성 C를 원래 ARM64 호출 및 실제 ObjC/CALayer와 비교하여 nil, 부동소수점 비트, 실행 시 필드 오프셋, 피호출자의 합법적인 복사본 변경을 검증한다.

Swift 메타데이터 바인딩은 두 개 또는 세 개의 기호 명목 타입 설명자로 중첩된 레이블 튜플과 단일 타입 인자를 갖는 저장소 타입을 검증합니다. 제한된 레시피 파서는 전체 타입 트리, 정확한 레이블, 반복 타입의 치환 동일성을 비교하며, 설명자 문자열 확장으로 치환 인덱스를 바꾸지 않습니다. 레이블 안의 `A`는 일반 문자입니다. `libswiftCoreFoundation`의 정확한 강한 `CoreGraphics.CGFloat` 설명자 가져오기는 Swift 6.1.2의 macOS/Mac Catalyst 네 컴파일 대상과 SDK 내보내기로 인증합니다. 게시 시 현재 설명자, 캐시, 참조, 바이트를 다시 검사합니다. 생성 도우미는 원래 레시피와 공유 저장소의 동일성을 유지하며, O0/O2의 실제 Swift 메타데이터 조회로 서로 다른 레이블과 저장소 타입을 구별합니다. 제네릭 레이아웃, 값 witness 구현, 콜백 ABI, 프레임 효과 또는 noescape 권한은 부여하지 않습니다.

동일한 Swift 메타데이터 레시피 소유자는 키 타입과 레이블 튜플 값이라는 두 제네릭 인자를 갖는 저장소도 인증합니다. 두 인자를 각각 전체 선언 타입 트리, 정확한 레이블, 원래 세 기호 설명자의 동일성과 비교합니다. 원래 `AC` 치환은 세 번째 설명자를 가리켜야 하며 문자열 확장으로 얻은 이름의 일치는 이 증명을 대신하지 못합니다. 하나의 중첩 튜플 인자와 구별합니다. 바인딩과 게시 시 원래 레시피, 캐시, 참조를 다시 검사합니다. 네 컴파일 대상과 전체 원본 ARM64 인스턴스화 helper 및 생성 C의 O0/O2 비교로 실제 Swift 사전 저장소 동일성과 캐시 적중 및 미적중을 확인합니다. 배치, 콜백 ABI 또는 프레임 효과를 추론하지 않습니다.
