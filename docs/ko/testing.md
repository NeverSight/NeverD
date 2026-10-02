**언어**: [English](../testing.md) | [简体中文](../zh-CN/testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](../ja/testing.md) | [한국어](testing.md) | [Français](../fr/testing.md) | [Deutsch](../de/testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← 문서 인덱스](README.md)

# NeverD 테스트

NeverD 테스트는 표현 모양이 예상과 같은지, 바이너리 fixture의 전체 pipeline 경로가
동작하는지, 생성 코드가 동작을 보존하는지라는 세 가지 질문에 답합니다. 변경의 질문에
답하는 가장 작은 스위트를 고른 뒤 위험이 큰 풀 리퀘스트에서는 더 넓은 집계를 실행하세요.

## 테스트 빌드 구성

`BUILD_TESTING`을 활성화하지 않으면 테스트가 비활성화됩니다. 전체 스위트에는 보통
Release를 사용합니다. Debug는 assertion과 단계 실행을 보존하지만 의도적으로
최적화하지 않으므로 디코드 벤치마크를 대표하지 않습니다.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

전체 fixture 세트에는 교차 대상 컴파일용 `clang`과 `PATH`에 있는 LLVM
linker(`ld.lld`, `lld-link`)가 필요합니다. CMake는 많은 재배치 가능 fixture를
항상 만들고 해당 linker가 있으면 링크된 ELF/PE fixture도 만듭니다. host가 fixture를
컴파일하거나 링크할 수 없어 건너뛴 테스트는 실행되지 않은 coverage이지 해당 대상의
통과가 아닙니다.

복제, 빌드 프로필, macOS 사전 빌드 LLVM은
[CONTRIBUTING.md](CONTRIBUTING.md)를 참고하세요.

## 인터프리터 복원 검사

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests \
  NeverDDevirtualizationSourceTests --parallel 4
ctest --test-dir build-release -L '^NeverD(InterpreterSpecialization|DevirtualizationSource)Tests$' \
  --output-on-failure
```

```sh
cmake --build build-release --target NeverDLowIRUndefinedIndependenceTests \
  NeverDOriginalBinaryUndefinedIndependenceTests \
  NeverDX86UndefinedEffectsTests NeverDX86CarryArithmeticFlagTests \
  NeverDX86LogicIdentityTests NeverDX86NoIndexAddressTests --parallel 4
build-release/bin/NeverDLowIRUndefinedIndependenceTests
build-release/bin/NeverDOriginalBinaryUndefinedIndependenceTests \
  --gtest_filter='OriginalBinaryUndefinedIndependence.*'
build-release/bin/NeverDX86UndefinedEffectsTests
build-release/bin/NeverDX86CarryArithmeticFlagTests
build-release/bin/NeverDX86LogicIdentityTests
build-release/bin/NeverDX86NoIndexAddressTests
```

복구 API 테스트는 v1/v2/v3 기본값, 명시적 예산, 잘린 구조체, 모든 reserved 필드 및 미래 꼬리 호환성을 검사합니다. CLI 테스트는 두 ABI와 두 백엔드에서 필드／질의 예산 소진 및 복구 성공을 확인하고 잘못된 십진 한도와 `--devirtualize` 누락을 거부합니다. 예산 소진 시 소스나 부분 잔여 그래프를 게시하면 안 됩니다.

v4 테스트는 접두 구조의 크기와 패딩, 잘린 구조와 알 수 없는 플래그의 거부, 이전 API/미래 확장 호환성과 보고서 없이도 C에 남는 부호 있는 범위를 검사합니다. 독립 CLI 예제는 상관관계 보존에 연결을, 부호 없는 스택 비교 증명에 범위를 요구하며 두 C 백엔드를 O0/O2 및 미정의 동작 트랩으로 실행합니다. 탐색 비활성화는 탐색 의존 예제의 결과를 바꿔야 합니다. 파싱은 0 연결, 정수 극값, 오버플로, 잘못된 범위와 전제 누락을 검사하고 Python은 레이아웃, 플래그, 시그니처 및 소유권이 있는 실패 보고서를 검사합니다.

`NeverDByteMemoryForwardingTests`는 겹치는 마지막 쓰기, 양쪽 바이트 순서, i128까지의 바이트 배수 폭, 정의된 값, 상관관계를 보존하는 undef/poison 스냅샷과 부분 덮어쓰기를 검증합니다. 알 수 없는 별칭, 주소 공간 변환, 호출, 순서 접근, 수명 변경, 누락 바이트, 동적 또는 잘못된 오프셋, 분기와 루프에서는 로드를 보존합니다. 입력이 많은 PHI, 0·정확·소진 예산, 기본 스냅샷 거부도 검사합니다. 원래 LLVM과 변환된 LLVM을 O0/O2에서 독립 산술 기준과 비교하며 기존 MBA, LLVMC, 인터프리터 소스 테스트로 호환성을 검증합니다.

`NeverDMedMutableSourceTests`와 `NeverDLLVMCValueTests`는 독립적으로 작성한 루프, 블록 재배열, 진입 역방향 간선, 런타임 스택 연산, 앞선 읽기, 분기 합류, 부분 별칭, 불리언 참값 및 0을 포함한 비트 개수를 O0/O2로 실행합니다. 부정 사례는 잘못된 입력, 잘린 대상, 모호한 전달 위치와 예산 소진을 출력 전에 거부합니다. SSA 한도를 넘는 CLI 사례는 실행 가능한 LLVMC 출력과 명시적인 HighC 거부를 요구합니다. 반복 갱신과 여러 블록에 걸친 저장 표현식 체인도 생성된 C의 크기와 실행 결과를 검증합니다.

복합 조건 회귀 테스트는 O0/O2에서 0이 아닌 상수와의 동등 비교, 부호 없는 비교, 양쪽 피연산자 순서의 부호 있는 비교, 확장된 불리언 입력과 모든 부정 조합의 논리곱·논리합을 실행합니다. C 출력은 전체 진리표를 보존하고 없는 0 비교 피연산자를 역참조하지 않아야 합니다. 정수 주소 저장은 정렬 및 비정렬 32/64/128비트 값을 검사합니다. 바이트 저장 배열은 명시적 정렬과 시작·부분 접근을 정확히 보존하며 배열에 대한 스칼라 대입이나 호환되지 않는 타입의 별칭 접근을 생성하지 않습니다.

`NeverDLowIRRefinementTests`는 실제 복원 그래프, 구조가 다른 유한 루프, 0회 반복, 동적 생성자, 조건부 선택, 겹치는 입력 뷰, 복사 및 스필 상관관계, 양쪽 불변 읽기 증거, 시스템 플래그와 반환 슬롯 보존을 검사합니다. 잘못된 후보, 추가 쓰기, 불완전하거나 무한한 경로, 오래된 증거, 임시 영역 충돌과 공유 예산 소진은 인증서를 거부해야 합니다. 기존 독립성 테스트도 관찰 가능한 임의 값을 계속 거부합니다.

같은 대상의 `LowIRLoopRefinement.*`와 `BinaryLowIRLoopRefinement.*`는 임의 64비트 반복 횟수, 중첩 사전식 순위, 실제 네이티브 잔여 코드, 진입 접두 템플릿, 겹치는 뷰와 상관된 스필을 검사합니다. 부정 사례는 잘못된 본문, 진입 영역 축소, 감소하지 않는 순위, 부호 없는 순환, 이전 쓰기 누락, 절단점 누락, 잘못된 템플릿과 공유 예산 소진을 거부합니다. 유한한 형제 경로의 성공은 불완전한 귀납 증명을 승인하지 않습니다.

`LowIRLoopInference.*`와 `BinaryLowIRLoopInference.*`는 독립적으로 작성한 카운터, 스택 저장, 조기 반환, 네이티브 호출 및 패킹된 플래그를 사용합니다. 좁은 비트 폭의 산술 확장과 표현식이 달라도 의미가 같은 플래그를 검사합니다. 잘못된 그래프, 누락되거나 위조된 원본, 종료하지 않거나 래핑하는 루프, 추론 또는 증명 예산 소진에서는 인증서를 생성하면 안 됩니다.

공유 헤더와 공유 래치 회귀 검사는 0 확장된 32비트 및 전체 64비트 카운터, 비단위 증감 스칼라 순위, 잘못된 결과, 진행하지 않거나 래핑되는 경로, 스칼라 및 튜플 탐색 사이에 누적되는 정확한 예산과 예산 소진을 다룹니다. `LowIRLoopInference.SharedHeaderAndLatchNeedLexicographicRanks`. 추가 증가·초기화 회귀 검사는 반복마다 카운터 비트 하나씩 펼치지 않고 수렴하도록 요구하며, 진행 누락과 부호 없는 래핑을 거부합니다. 스케줄링 회귀는 비단위 증감 누산기, 유효한 비단위 스칼라 순위 옆에서 래핑될 수 있는 단위 카운터, 성공 튜플이 초기 구간 뒤에 나오는 세 카운터를 다룹니다. 정확히 충분한 순위 예산과 한 번 부족한 예산으로 후보 중복 없는 결정적 탐색 재개를 확인합니다.

같은 대상의 `LowIRLoopPlanPairing.*`는 레지스터 이름 변경, 서로 다른 산술 본문, 각 측의 접두 스냅샷, 술어 보존, 공유 프레임 입력, 중첩 절단점 포괄 및 독립적인 증명 예산을 검사합니다. 관계 누락, 잘못된 쓰기, 잘못된 임시 값 바인딩, 불완전한 대응 또는 메타데이터 한도 초과 시 인증서를 생성해서는 안 됩니다.

`LowIRLoopAlignment.*`는 독립적으로 작성한 일반 및 회전형 프레임 카운터 루프를 검사합니다. 두 기본 자기 관계 계획은 각각 증명되지만 첫 짝짓기는 실패하고 다른 후보 절단점에서 관계가 증명됩니다. 여러 절단점의 순열, 잘못된 결과와 프레임 쓰기, 누락되거나 오래된 원본 기록, 명시적 미정의 값 witness, 감소하지 않거나 래핑되는 카운터, 잘못된 그래프, 실패 시도의 누적 쿼리, 정확히 충분한 총예산과 탐색 한도 소진을 회귀 검사합니다. 거부 결과에는 인증서가 없어야 합니다. 추가 사례는 분리된 초기화·진행 단계, 종료 조건을 옮겨 후보군 간 짝짓기가 필요한 동등한 루프, 재추론 없는 캐시, 메타데이터 합계 초과를 검증합니다. 뒤따르는 독립 루프는 명시적인 16384회 질의 한도로 전체 순환 포괄을 확인합니다. 비거나 중복된 후보군은 기호 질의를 쓰지 않고 절단점 부족은 거부합니다. 정확히 충분하거나 한 번 부족한 전체 예산, 잘못된 결과, 진행 누락, 원본 증거와 미정의 값 witness도 검사합니다. 필터 회귀는 결과를 유지하는 산술 다이아몬드, 로컬 합류와 경계에서만의 합류, 도달 가능한 합류점을 우회해 종료하거나 순환 경계로 돌아가는 경로를 검사합니다. 원본 필터 후보군이 중복이어도 후보 필터를 시도하는지, 먼저 성공한 필터 계획을 뒤늦은 전체 분기 시도가 재사용하는지, 정확히 충분한 값·한 단위 부족·0의 `MaxCutSelectionWork`, 실패한 `CutSelectionWork` 누적, 전역 그래프 작업 소진 후 기호 추론 중단을 검증합니다. 두 분기 후보군 모두 완전한 순환 포괄을 검사하며 다이아몬드 관계에는 명시적인 추론·증명 질의 한도를 사용합니다.

부분 카운터 회귀는 프레임과 레지스터, 증가·감소, 하위·중간·상위 위치, 특수 너비, 두 바이트 순서와 3바이트 프레임 워드를 다룹니다. 늦게 발견되는 레인, 보존 비트 변조, 진행 없음과 보호 없는 래핑, 잘못된 추가 진입, 정확하거나 부족한 추론 예산, 기존 단일 절단점 탐색을 검사합니다.

선행 단계 회귀는 같은 카운트다운 워드를 재사용하는 두 개와 세 개의 순차 루프, 기존 중첩 루프 단계와의 결합, 정확히 충분하거나 한 번 부족한 순위·질의 예산, 진행하지 않는 루프와 이전 단계로 되돌아가는 초기화를 검사합니다. 같은 너비의 잘못된 단계 상수, 잘못된 결과와 프레임 쓰기는 완전한 검사기가 인증서 없이 거부해야 하며 원본 증거 누락은 계속 미지원입니다.

`NeverDLowIRRefinementTests`의 `InterpreterMachineStateModel.*`는 독립적인 LowIR 예제로 원시 진입 플래그, 상태와 게스트 RAX의 구분, 17개 상태 워드 전체, 부분 레지스터, 플래그 패킹, 동적 거부 상태의 유지, 게스트 프레임 쓰기, 양쪽 분기와 순환 추론 후의 새 증명을 검사합니다. 잘못된 출력, 누락된 상태, 메모리 변경, 오래된 명령 기록, 잘못된 입력, 생성 예산 소진은 실패해야 합니다. 기존 머신 소스 테스트는 두 C 경로를 O0/O2에서 실행하며 모델 테스트만으로 컴파일된 C를 인증하지 않습니다.

`NeverDLLVMInterpreterModelTests`는 독립 LLVM을 전체 상태 LowIR 기준과 비교하여 비트 폭, 병렬 PHI, switch, 게스트 메모리, 별도 상태, poison 조건, 내장 함수 범위, 거부 계약과 네 가지 구성 예산을 검사합니다. 임의 워드 카운트다운의 완전한 증명을 검사하고 변조된 상태를 거부합니다. 독립 C의 O1/O2 컴파일 결과도 같은 관찰 계약을 만족해야 합니다. 지원 모델을 검증하는 테스트이며 자동 불변식 발견과 컴파일러 정확성은 별도 의무입니다. 가변 시프트 사례는 네 가지 비트 폭, 마스크나 분기로 제한한 시프트 양, 경계값과 범위 초과 값, 오버플로 금지 및 정확성 플래그, 엄격한 poison 거부, O1/O2로 컴파일한 C를 검증합니다.

초기화 계약 회귀는 부분 및 분리된 바이트 범위, 고정 별칭, 양쪽 분기, 모든 반환, 첫 반복의 읽기와 반복 안의 저장 후 읽기를 검사합니다. 저장 전 읽기, 누락된 저장, 게스트 저장, 알 수 없는 별칭, 특수 메모리 접근, 객체 밖 범위와 입력/작업 예산 소진은 실패해야 합니다. 출력 전용 상태 워드를 쓰는 독립 C 예제를 O1/O2로 컴파일해 정확한 LLVM 속성을 유지한 채 새로운 네이티브→LLVM 조합 증명을 통과하는지 확인합니다.

조건부 카운트다운 검증은 본문 템플릿 거부 후 재시도, 임의 워드 입력에 대한 완전한 헤더 증명, 절단점 및 쿼리 예산의 누적, 실제 진입 계약 위반의 즉시 거부를 검사합니다.

`NeverDInterpreterLLVMRefinementTests`는 새로운 조합 증명, 정확한 텍스트/함수 바인딩, 독립 예산, 전체 관찰과 더 넓은 소스 영역을 검사합니다. 바이트, 잔여 코드, 결과, 플래그, 상태 코드, 프레임 쓰기, poison 및 잘못되거나 오래된 루프 계획은 조합 기록을 거부해야 합니다. 임의 워드 카운트다운에는 두 귀납 전제가 필요하며, 독립 C 예제의 O1/O2 컴파일은 실제 직렬화 LLVM 입력을 검증합니다. 상태 모델 회귀는 숨겨진 진입 역방향 간선을 거부하고 부수적인 출처 정보를 복사하지 않으면서 루트 예산을 검사합니다.

```sh
cmake --build build-release --target NeverDLLVMInterpreterModelTests --parallel 4
build-release/bin/NeverDLLVMInterpreterModelTests
cmake --build build-release --target NeverDInterpreterLLVMRefinementTests --parallel 4
build-release/bin/NeverDInterpreterLLVMRefinementTests
```

2중 및 3중 루프의 캐시된 동등 종료 조건은 피연산자 상관관계, 변하는 경계, 카운터 재설정 및 손상된 복사를 검사합니다.

비교 캐시 회귀는 같음과 다름, 가드와 상수 접기된 초기화, 확장 후 처음 나타나는 필드, 1/4/8바이트 캐시의 비트 7/31/63을 검증합니다. 검사 대상 비트를 유지하며 인접 비트만 바꾸어도 전체 상태 비교가 거부해야 합니다. 0 증분, 이동 경계, 카운터 초기화와 공통 예산 소진도 거부합니다.

일반화 접두 회귀는 합류 진입, 첫 무반복 증거, 숨겨진 레지스터/프레임 차이, 비정규 불리언 술어, 네이티브 트랩 조건, 상관된 스필 및 잘못되거나 예산이 소진된 계획을 검사합니다. 독립적인 2중/3중 등식 종료 카운터와 네이티브 바이트는 부호 없는 입력 경계, 0/최댓값 영역, 비단위 증가 및 잘못된 원본 명령을 확인합니다. 추론과 최종 증명 모두 불완전한 결과를 거부해야 합니다.

독립적으로 작성한 분기 루프 회귀 테스트는 두 분기 방향, 잘못된 루프 본문, 종료하지 않는 형제 분기와 공유 탐색／증명 예산 소진을 검사합니다. `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

중첩 추론 회귀는 2중·3중 루프, 증가·감소 카운터, 자동 단계 상수 및 실제 네이티브 본문 절단점을 다룹니다. 도달 불가능하거나 서로 배타적인 접두 영역, 잘못된 본문, 무한 또는 래핑 전이, 공유 탐색/증명 예산 소진은 거부해야 합니다. 접두 증거가 전체 구간 검증을 대체하지 않습니다.

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

`NeverDLowIRUndefinedIndependenceTests`는 완전한 비순환 LowIR 그래프에서 두 실행의 독립성을 검사합니다. 일반 진입 입력은 공유하며, 아키텍처에서 정의하지 않은 값을 새로 생성할 때마다 복사, 겹치는 쓰기, 스필과 재로드에 걸쳐 해당 값의 상관관계를 유지합니다. 제어 조건은 경로 가정을 추가하기 전에 검사합니다. 인증서는 `Complete` 효과 메타데이터와 각 명령의 전체 경계 및 정확한 연산 다이제스트와의 결합을 요구합니다. 증거 누락, 도달 가능한 루프, 호출, 알 수 없는 별칭, 예산 소진 시 인증서를 거부합니다. 결과는 명시적인 관찰 항목과 오류 없는 프레임 계약으로 제한되며, 네이티브 코드에서 C로의 완전한 동등성 증명이 아닙니다.

다음 동작에는 기본 엄격 감사 계약이 적용됩니다. `NeverDOriginalBinaryUndefinedIndependenceTests`는 독립적인 고정 매핑 x64 바이트로 실제 CALL/RET, 변경된 반환 대상, 유한 간접 대상의 완전 열거와 불변 메모리 읽기를 검사합니다. 같은 테스트 대상에서 직접 분기 전체 수집, 정확한 바이트·효과·매핑·읽기 증거 결합, 외부 반환 시 진입 RSP와 반환 슬롯 보존, 프레임과 이미지 분리 전제의 충족 가능성을 확인합니다. 누락되거나 겹치는 명령, 정확한 트랩 및 명시적 프로파일 투영 규칙 밖의 미감사 분기, 종료하지 않거나 예산을 초과하는 루프, 불완전한 대상 열거, 설정·계약 불일치 및 예산 소진 시 인증서나 잔여 코드를 반환하지 않습니다. 성공하려면 모든 실행 가능 경로가 완료되어야 합니다. 이 선택적 게이트는 루프 불변식, 예외 디스패치, CET 활성 실행이나 네이티브 코드에서 C로의 동등성을 인증하지 않으며 일반 복원은 별도로 유지됩니다. 이 대상은 엄격하게 리프트한 `INT3`/`UD2`의 종단 경계와 전체 바이트 및 연산 다이제스트 결합도 검사합니다. 미정의 출력 사이드카의 `Missing`은 그대로 유지되어야 합니다. 기호 실행으로 도달 불가능함이 증명된 트랩만 인증서에 포함될 수 있으며, 실행 가능한 트랩 경로는 인증서나 잔여 코드 없이 `ContractViolation`을 반환해야 합니다. 트랩 이후의 순차 실행과 예외 복귀는 모델링하지 않고, `codeFollowsTrap`을 사용하지 않으며, 정적 LowIR API의 지원 범위는 그대로 유지합니다.

명시적 네이티브 중첩 테스트는 실제 x64 즉시값 내부로의 분기, 실행 가능한 두 분기의 결과, 이전 명령어 내부의 간접 반환 진입점을 검사합니다. 합성 제공자 테스트는 두 수집 순서의 포함 중첩, 실행되지 않는 직접 분기의 바이트 충돌, 두 순서의 코드와 읽기 일치성 및 후보 읽기를 검사합니다. 정확히 충분한 예산과 부족한 예산은 간접 전송에 걸쳐 중복 바이트까지 누적 계산합니다. 분기 결과 변경, 정적 또는 루프 API 사용, 모순된 증거는 인증서를 거부해야 하며 옵션이나 한도 변경은 다이제스트를 바꿉니다.

경계 옵션 테스트는 도달 불가능한 RCL, 메모리 XADD, REP MOVS, 기호 경로 모순, 임의 값이 제어하는 분기와 진입점·간접 분기·CALL·RET 도달 시 정확한 거부를 검사합니다. 도달 가능한 후속 영역의 별도 진입, 후보와 네이티브 주소 충돌, 잘못되거나 부분적인 증거, 자원 소진, 정적/루프 API 거부, 정제 증명의 세 다이제스트 계층도 검사합니다. 도달 불가능한 명령을 변경하거나 경계가 없는 경우 옵션을 전환해도 인증서 다이제스트가 바뀝니다. 이 테스트는 선언된 유한 증명 범위를 검증하며 미감사 명령의 의미를 증명하지 않습니다.

패킹 플래그 테스트는 모든 스칼라 진입 플래그 조합, 권한 마스크, 두 실행의 TF/AC 조건, 별개 미정의 생성자, 상관된 복사, 네이티브 호출, 형제 경로 상태, 필수 최종 시스템 상태 관찰, 잘못된 증거와 자원 제한을 검사합니다. 유한 루프의 모든 실행 가능한 입력 경로가 종료해야 하며 안전한 분기로 무한 또는 잘린 경로를 숨길 수 없습니다. RDSSPD/RDSSPQ는 16개 범용 레지스터와 두 폭, 상위 비트 보존, `Missing` 증거 유지, 위조 투영 거부를 검사합니다. 기계 상태 테스트는 두 C 경로의 O0/O2와 미정의 동작 트랩으로 독립적인 사용자 모드 플래그 오라클과 비교하고 프로파일 실패가 나중에 지워지지 않음을 확인합니다. INCSSPD/INCSSPQ 테스트는 두 폭과 모든 범용 레지스터, 도달 불가능 경계 보존, 안전한 형제 경로 완료 후의 실행 가능한 트랩, 0 피연산자, 위조 트랩 증거를 확인합니다.

`NeverDX86UndefinedEffectsTests`는 미정의 비트 메타데이터, 정의되거나 보존되는 플래그, 오래된 인증서 거부를 검사합니다. `NeverDX86CarryArithmeticFlagTests`는 산술 오라클을 기준으로 레지스터 및 메모리 형태 ADC/SBB의 보조 캐리를 검사합니다. `NeverDX86LogicIdentityTests`는 동일 피연산자 AND가 64비트 모드에서 32비트 대상에 쓸 때 해당 64비트 레지스터의 비트 63:32를 0으로 만들면서 더 좁은 쓰기의 미기록 비트는 보존하는지 검사합니다.

`X86RotateUndefinedEffects.*`는 모든 원시 횟수, 피연산자 폭, CL 중첩, 상위 바이트 별칭 및 메모리 대상을 스칼라 산술 기준과 비교합니다. `X86BitTestUndefinedEffects.*`는 레지스터/즉시값 인덱스, 원본/대상 중첩, 확장 레지스터, 정의된 플래그와 레지스터 상위 쓰기를 검사합니다. 메타데이터 반례는 변경된 피연산자, 인코딩 및 미지원 형식을 거부합니다. 네이티브 증명은 상관된 읽기와 독립적인 임의 플래그를 구별하고 생성자 예산의 정확한 경계 및 부족을 검사하며 관찰 가능한 미정의 OF를 거부합니다. 전체 상태 정제는 선택한 증인을 허용하고 0비트 증인이나 변경된 후보는 거부합니다.

`X86XaddAudit.*`는 부호 없는 산술 기준으로 65,536개의 모든 바이트 입력 쌍, 넓은 비트 폭의 플래그 경계, 레지스터/상위 바이트 중첩, 두 쓰기 결과, REX 바이트 폭 제한과 전체 레지스터 보존을 검사합니다. 네이티브 검사는 이전 의존성을 유지하면서 새로운 임의 비트가 없음을 요구합니다. 두 증인은 변경되지 않은 XADD를 허용하고 합, 교환 원본 또는 정의된 플래그의 변조를 거부합니다. `/6` 별칭은 전체 시프트 횟수 행렬로 검사하며 그룹/디코딩 ID 변경 반례가 의미 불일치를 거부합니다.

`NeverDPEFixedImageTests`는 독립적으로 구성한 PE 파일로 재배치된 명령과 불변 데이터, 가져오기 쓰기 범위, 잘못된 헤더／테이블, 별칭과 출처 정보 변경을 검사합니다. 네이티브에서 LowIR 및 정확한 LLVM으로 이어지는 증명은 일치하는 후보를 허용하고 결과, 상태 또는 원본 바이트가 변경된 후보를 거부합니다. 준비 예산 소진은 별도로 분류하며 한도를 명시적으로 늘려 재시도할 수 있습니다. 일반 로드는 기본 분석 예산을 넘는 40000개의 유효한 재배치 레코드도 허용합니다.

`FrameOffsets.*`, `NativeStackSpecialization.*`, `OriginalBinaryUndefinedIndependence.*`는 정렬 2/4/8/16/32의 모든 나머지, 자유로운 상위 비트, 호출 간 스필, 카운트다운 루프, 별칭 손상, 잘못된 분기, 무관한 큰 마스크, 필요한 분할 확대와 정확한 한도/한 번 부족한 예산을 검사합니다. 별도 네이티브 대조 사례는 경로 조건이 있는 정렬, 내부 부호 없는 반환 정리, 잘못된 정리량 및 접두사 반환을 검사합니다. 이 테스트는 분할 루프의 자동 native-to-LLVM 증명 범위를 확립하지 않습니다.

레지스터 경우 회귀는 양쪽 바이트 순서, 높은 프레임 기준 주소, 덮어쓰기와 중첩 필드, 후속 간선, 확장된 선행 노드, 네이티브 CALL/RET 및 정확한 예산과 하나 부족한 예산을 검사합니다. 두 C 경로는 O0/O2에서 네 가지 메모리 경우를 모두 실행합니다. 네이티브 정밀화는 두 선택자 값을 각각 고정하여 검사하므로 무제약 입력 증명이 아닙니다. 별도 LLVM 예제는 공유 합류점 앞으로 이동한 거짓 분기가 참 분기 뒤에 실행되지 않음을 PHI 복사와 저장을 포함해 검사합니다.

진입 정렬 회귀는 세부 분할, 모든 허용 나머지, 서로 다른 주소 상위 비트, 마지막 사례 실패 및 정확한 예산과 1 부족을 검사합니다. 두 C 백엔드의 O0/O2 실행에서 접근 불가능한 거부 대상 게스트 주소와 잘못된 플래그로 상태 2 및 모든 상태 바이트 보존을 확인합니다. 모델 의미와 C/Python v5 레이아웃·소유권·이전 및 미래 후행 필드도 검사하며, 바인딩되지 않은 정렬 영역의 네이티브 증명을 거부합니다.

`StringTransfer.*`와 반복 복사 회귀는 겹침, 횟수 0, 임시 변수 격리, 용량·예산 제한 및 포인터 무효화를 검사합니다. `MachineStringSourceTests.cpp`는 네 가지 폭과 양방향에 대해 네이티브 실행 및 두 C 경로의 O0/O2 결과를 독립적인 전체 레지스터·플래그·스택 참조 결과와 비교합니다.

`ControlDiscovery.*`와 `NativeStackSpecialization.*`는 루트 하위 비트 조건, 상위 비트 및 전체 루트 의존성, 불완전한 순회, 정확히 충분하거나 부족한 순회 예산, 유한한 불변 주소 증거의 보존을 검증합니다.

`NativeStackSpecialization.NarrowAddressDemandRetainsCompletePointer`는 조건부 레지스터 및 프레임 슬롯 포인터 병합, 두 바이트 순서, 모듈러 래핑과 높은 루트 주소, 스택 복원 및 프레임 120바이트를 검사합니다. 관련 반례는 손상된 포인터를 거부하며, 쿼리·연산·평가·정밀화 예산이 정확히 충분하거나 한 번 부족한 경우와 의존성 탐색 및 문맥 한도 소진을 검사합니다.

유한 값 관찰자 테스트는 조기 거부, 상수, 빈 투영 및 마지막 UNSAT 쿼리를 검사합니다. 불변 읽기 회귀는 반례 이후 런타임 로드를 유지하고, 다른 읽기 범위에 대해 캐시된 주소 도메인을 재검증하며, 잘못된 인증서에서 부분 증거를 게시하지 않습니다.

아핀 제어 테스트는 8회 질의 예산에서의 하위 비트 조건, 두 바이트 순서의 레지스터 및 프레임 슬롯, 모듈러 래핑, 프레임 바이트 관측 및 스택 복원을 다룹니다. 리터럴 상수가 아닌 모순 조건은 지원하지 않는 분기를 제거해야 하며, 하위 32비트가 같고 상위 비트가 다른 두 루트 값은 두 간접 분기 대상을 모두 보존해야 합니다.

`FiniteQueryCache.*`는 정확히 충분한 저장 한도와 한 단위 부족한 한도, 적중에 따른 사용 순서 갱신, 크기가 다른 여러 기록의 교체, 반복 제거, 변수 이름을 바꾼 질의를 검사합니다. 중복 저장, 미스, 잘못된 결과, 지나치게 큰 후보는 사용 순서를 유지합니다. 반환된 증명 사본은 해당 캐시 항목이 제거된 뒤에도 유효합니다.

`ControlStateRecovery.*Marginal*`은 독립적인 대상 및 업무 영역의 곱이 결합 한도를 초과하는 경우, 잔여 코드의 구체적 출력, 결합 관계 확장 후 늦은 선행 노드가 추가하는 대상, 도달 가능한 대상의 누락, 초과 또는 불완전한 열거 시 전체 영역 제거를 다룹니다. 독자적으로 작성한 픽스처는 영역 변경에 따른 재분석과 실패 시 부분 그래프를 공개하지 않는 동작을 검증합니다. 프레임 슬롯 변형은 결합 관계 확장 후 두 바이트 순서에서 별칭 쓰기에 따른 무효화를 검증합니다.

`InterpreterTransferChain.*`는 상관값, 유일 및 동적 분기, 뒤늦은 선행 노드, 프레임 경계, 별칭 거부와 예산을 검사합니다. 추가 네이티브 CALL/RET 테스트는 반복 출현, 반환 슬롯 바이트와 스택 복원을 검사합니다. 생성자 재생 및 네이티브에서 LLVM으로의 검증은 계약 불일치, 증거 변경, 잘못된 결과와 누락된 스택 쓰기를 다룹니다.

`NeverDX86NoIndexAddressTests`는 32/64비트 주소 폭에서 인덱스 없는 x86 SIB 주소 지정을 검사합니다. 무시되는 배율 비트, 대상 폭, 로드/스토어, 완전한 미정의 출력 메타데이터, 세그먼트 오프셋과 주소 출처를 확인합니다. 의사 레지스터를 베이스 또는 잘못된 폭의 인덱스로 사용하면 거부하며, REX.X가 선택하는 실제 R12 인덱스는 유지합니다. EVEX 브로드캐스트 및 마스크 이동 테스트도 이러한 형식, 비활성 메모리 접근 억제와 모순된 SIB 메타데이터를 검사합니다.

시프트 회귀는 모든 8비트 원시 횟수, 0회 플래그 조합, 두 x86 모드, 모든 스칼라 폭, CL 대상 별칭, AH/CH/DH/BH, 확장 레지스터와 메모리를 다룹니다. 바이트 단위 기호 실행을 반복 1비트 산술과 비교하여 정의된 결과와 가드를 검증합니다. 관계 테스트는 복사와 새 플래그, 스필, 루프 재방문, 미정의 값에서 나온 횟수, 분기 거부, 잘못된 인코딩, 다이제스트와 예산 실패를 확인합니다. 유한 불변 읽기 테스트는 1/2/4/8바이트, 입력별 선택, 경로별 단일 주소, 전체 읽기 증거와 한도 결합을 확인하고 의존·누락·쓰기 가능·파일 바이트 없음·재배치·무한 후보를 거부합니다.

코어 테스트는 컨텍스트 분리, 고정점 합류, 동적 루프, 겹치는 레지스터, 별칭 무효화, 유한 대상 디스패치 및 거부 시 부분 교체를 내보내지 않는지 확인합니다. 소스 테스트는 직접 작성한 레지스터형·스택형·유한 주소 x64 머신을 어셈블하고 두 C 경로를 복원하여 정의되지 않은 동작 트랩을 켠 O0/O2로 컴파일한 뒤 독립적인 부호 없는 산술과 메모리 참조 구현과 실행을 비교합니다. 유한 주소 fixture는 입력으로 선택한 레코드와 커서／키 상관관계를 검증하며 네이티브 검사는 SysV/Win64를 다룹니다. 공개 CLI, 복원 예산과 지원하지 않는 입력의 보고서도 확인합니다. 교차 대상 Clang과 LLD가 필요하며 원래 ELF 실행에는 x64 Linux 호스트도 필요합니다. 도구 누락이나 호스트 불일치로 건너뛴 범위는 통과한 검증이 아닙니다.

`ControlStateRecovery.LongTransparentLoop*`는 독립적으로 작성한 20단계 루프, 동적 산술 참조 구현, 알 수 없는 선택자 거부 및 예산 소진을 검증합니다. `LongTransparentPhasesKeepExactBitDemands`는 선택자와 같은 바이트의 무관한 비트가 제어 요구가 되지 않고 관찰 가능한 런타임 데이터로 남는지 확인합니다. `ProducerClosureChargesWorkBeforeAnotherRestart`는 새 그래프가 시작되기 전의 역방향 탐색과 재실행에도 공유 예산을 적용하며 부분 결과를 게시하지 않는지 검증합니다.

`X86ShiftCarry.*`는 반복적인 1비트 시프트를 기준으로 좁은 정수의 산술 오른쪽 시프트 캐리, 마스킹된 횟수, APX 대상 레지스터 및 플래그 억제 동작을 검사합니다.
`NarrowArithmeticShiftCarrySurvivesBothSourceBackends`는 정의되지 않은 동작 트랩을 활성화하여 O0/O2에서 두 복원 C 경로를 실행하며 모든 바이트 값과 원시 시프트 횟수를 다룹니다.
`NeverDLLVMCIntrinsicSemanticTests`는 i1/8/16/32/64/128 부호 있는/없는 정수 min/max도 O0/O2에서 실행하여 대입 및 인라인 결과, 피연산자 생성 순서와 단일 평가를 검사합니다. 지원하지 않는 스칼라 너비와 잘못된 피연산자는 명시적으로 실패해야 합니다.

## 구조화된 C 제어 흐름 및 호출 검사

`HighControlFlowSemantics.*`는 루프의 종료 지점이나 뒷부분을 이동할 때 다른 점프가 참조하는 레이블을 보존하는지 검사합니다. 루프 앞뒤 종료 지점으로 직접 진입하는 경우와 break 교체를 다루며, 생성된 C를 O0/O2로 실행해 독립적인 반환값 기준과 비교합니다.

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead`는 추론된 필수 레지스터 인수의 끝에 있는 알 수 없는 슬롯이 유지되는지 검사합니다. 알 수 없는 필수 인수나 조건을 평가하면 명시적으로 트랩해야 하며, 생략된 피연산자, null 피연산자 및 중첩 피연산자를 조용히 0으로 바꾸면 안 됩니다. 알려진 값과 읽히지 않음이 입증된 추가 피연산자는 계속 실행할 수 있습니다. 트랩은 진단 경계이며 복원된 동작의 동등성을 증명하지 않습니다.

## CPU 실행 테스트

`NeverDIntegerABITests`는 Windows x64, Linux x64, Linux ARM64용 원본 Clang fixture를 빌드합니다. 실제 10개 인자 함수로 레지스터/스택과 호출 프레임을 검사합니다. Unicorn/KVM/WHP 행렬에서 사용할 수 없는 호스트/ISA 조합은 명시적 skip이며 통과로 간주하지 않습니다. `NeverDExecutionBudgetTests`는 시간에 의존한 sleep 없이 공유 continuation 예산, reservation, 절대 deadline을 검사합니다.

`NeverDCPUEmulationTests`는 ARM64 명령, 제어, load, context, alias, 캐시 무효화, 유한 루프를 다룹니다. software 프로필은 FP/SIMD와 TLS도 실행합니다. `NeverDUserExecutionTests`는 CPL3/EL0 권한, alias, 보호 fault, context 및 공간 전환을 검증합니다. `NeverDServiceRequestTests`는 SYSCALL/SVC가 전송 전에 가로채지고 상태가 보존되며 요청이 한 번만 소비됨을 확인합니다. 이는 handoff protocol 테스트이지 OS 서비스를 완전히 구현했다는 뜻이 아닙니다. `NeverDExecutionConfigurationTests`는 공통 resolver, 빌드 지원과 실제 probe 구분, 미지원 설정의 fail-closed 동작을 검사합니다. 공개 SDK/CLI 테스트에는 Windows model이 필요하지 않습니다. `NeverDThreadPointerTests`는 FS base, `TPIDR_EL0`, context 복원, 권한을 검사합니다. `NeverDKvmCancellationTests`는 종료되지 않는 x64 guest로 활성 KVM 중단, 재개, 호출자 signal 상태 유지를 검증하고 KVM이 없으면 명시적으로 skip합니다.

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests NeverDCPUEmulationTests NeverDUserExecutionTests NeverDServiceRequestTests NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget|CPUEmulation|UserExecution|ServiceRequest|ExecutionConfiguration)Tests$' --output-on-failure
```

ARM64 하드웨어나 hypervisor가 없으면 native coverage skip이며 통과가 아닙니다. Unicorn과 교차 컴파일은 native KVM/WHP 실행 증거가 아닙니다.

## Linux 프로세스 프로필 테스트

[독립 프로세스 테스트](process-emulation.md#검증)는 실제 x64/AArch64 ELF fixture를 빌드합니다. `NeverDLinuxProcessTests`는 시작, 프로그램 헤더 정책, 서비스 이어달리기, 바이너리 출력, 게스트 fault, 자원 중지를 확인합니다. `NeverDProcessPublicTests`는 분석 이미지를 바꾸지 않고 C API/CLI를 확인합니다. `NeverDExecutionSessionTests`는 메모리/예산을 공유하는 CPU 두 개와 요청/fault exactly-once 소비를 검사합니다. `NeverDX64MemoryUpdateTests`는 메모리 산술, SETcc, BT, XMM/MXCSR, 쓰기 observer, REP 경계, 준비된 장치 읽기를 검사합니다. `DriverBackendParityTests.cpp`는 원본/재배치 WDK fixture를 실행하고 관찰 가능한 전체 보고서를 Unicorn과 비교합니다. fixture/backend가 없으면 명시적으로 skip합니다.

checked x64는 마스크된 legacy `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN`, `MAX`의 `SS`, `SD`, `PS`, `PD` 형식도 허용합니다. `X64SSEInstructions.def`가 operand 너비, 정렬, 허용 규칙을 관리합니다. `MaskedSSEArithmeticMatchesIndependentHostExecution`은 독립 host CPU oracle로 register/RAM 형식, 네 반올림 모드, FTZ, signed zero, subnormal, NaN을 검증하며, `SSEMemoryObserverStopsBeforeResultAndStatusChanges`는 효과 반영 전 중단을 검증합니다. DAZ, 마스크되지 않은 예외, x87, AVX는 허용하지 않습니다.

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
```

사용할 수 없는 백엔드는 명시적으로 skip합니다. 교차 컴파일과 Unicorn ARM64는 native KVM/WHP 증거가 아닙니다.

## 드라이버 에뮬레이션 검사

`NEVERD_ENABLE_DRIVER_EMULATION=ON`과 `BUILD_TESTING=ON`을 함께 활성화하면 전용 실행 스위트와 공유 C API/CLI 검사를 빌드할 수 있습니다.

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

fixture는 게스트 초기화, 성공/실패 반환, 미지원 동작, 메모리 오류, 엄격한 시나리오 파싱, 제한된 실행을 검증합니다. 또한 create, 전송, cleanup, close, unload를 거치며 동기 buffered/direct I/O, READ/WRITE, 독립적인 파일 수명, MDL 권한, 동적 export 해석, 게스트 가변 인수와 구조화된 CPU 오류를 검증합니다. JSON과 프로세스 종료 코드를 확인하려면 [`emulate-driver` CLI](driver-emulation.md)를 사용하세요. 프로덕션 빌드는 `BUILD_TESTING=OFF`에서도 이 기능을 활성화할 수 있으며, `libneverd`가 테스트 전용 Unicorn 구성에 의존해서는 안 됩니다.

추가 테스트는 드라이버 소유 비페이지 풀 MDL, 설명자와 버퍼의 독립적인 수명, 레지스트리 조회 레이아웃과 짧은 버퍼, 핸들 권한, 삭제와 누수, 출력 없는 IOCTL의 전체 64비트 `information_hex`를 검증합니다. 실제 샘플 검증에는 Zero의 동기 직접 읽기·쓰기와 통계 조회도 포함됩니다.

백엔드 테스트는 전체 CPU 컨텍스트(레지스터, 플래그, SIMD, FPU, CR8), 공유 메모리와 외부/장애 컨텍스트 거부를 검증합니다. 컴파일된 `driver_dispatcher.c`는 실제 DPC·작업 항목 콜백, 타이머 경계, 알림/동기화 이벤트와 타이머, 사유 `Executive`의 비경고 `KernelMode` 대기, 시간 초과/지연, 여러 차단 스택, 설정 직후 재설정해도 유지되는 깨우기, 콜백 인수와 잘못된 IRQL/수명을 검증합니다. 작업 항목의 보류/완료, 큐, 정체와 공유 예산 검증도 유지합니다. 이는 문서화된 부분집합의 증거이며 완전한 Windows 비동기 지원은 아닙니다.

`driver_context_limits.c`: API IRQL 상한은 `KernelAPIIRQL.def`에 정의되며 인수별 제한은 담당 모델이 검사합니다. DPC는 레지스트리 API나 페이징 풀 할당·해제·접근을 사용할 수 없습니다. Unicode `DbgPrint` 변환은 `PASSIVE_LEVEL`이 필요하며 지원되는 ANSI 출력과 비페이징 작업은 `DISPATCH_LEVEL`에서 사용할 수 있습니다. 콜백 스택에는 경계가 있어 이탈한 스택 포인터가 다른 차단 작업자의 스택을 침범할 수 없습니다. 장치 확장의 활성 타이머는 조기 장치 회수를 막습니다. 일반 IRQL 전환을 제공하는 기능은 아닙니다.

`KernelDeviceStackTests.cpp`는 소유/연결 관계의 독립성, 최상단 선택, 실패 원자성, 스택 용량, 불투명 필드, 핸들 수, 분리·삭제를 넘는 작업 항목/요청 유지, 파일과 디스패치 대상의 차이를 검사합니다. 원본 `driver_wdm_stack.c`는 실제 WDK 헤더와 인라인 Copy/Skip/SetCompletion을 사용하며 일반/활성 CFG 이미지는 선택적 `NEVERD_WDM_STACK_FIXTURE`/`NEVERD_WDM_STACK_CFG_FIXTURE`로 지정합니다. `DriverWDMStackTests.cpp`는 재배치, 실제 하위 상태, 완료 순서/조건, 지연 pending 전달, 작업자/DPC, 대기, `STATUS_MORE_PROCESSING_REQUIRED`, 직접 MDL 유지, 중첩 완료, 잘못된 커서/제어를 검증합니다. `DriverScenarioPublicTests.cpp`는 구성된 CFG 이미지를 포함해 C API/CLI 전달과 C API 유지/중첩 완료를 검사합니다. 산출물이 없으면 명시적으로 건너뛰며 Linux 근거는 동일 드라이버 스택 범위만 입증합니다. PDO/PnP/전원 지원을 뜻하지 않습니다. `KernelIRPStackTests.cpp`는 개수 기반 커서, 전체 인라인 Copy 범위, 소비된 위치 지우기, 상태/pending 전달, MPR 및 중첩 완료, continuation 소유자, 보존 경로를 검사합니다. 실제 READ/WRITE와 파일 수명 주기도 인라인 Copy로 검증합니다.

`DriverPnpScenarioTests.cpp`는 JSON/네이티브 검증 일치, 필수 초기 사실, ID/개수 제한, 필드 조합, 최종 버스 상태, nullable 관측 보고서를 검증합니다. `KernelPnpDeviceTests.cpp`, `KernelPnpRequestTests.cpp`, `KernelPnpCompletionTests.cpp`는 제공자 소유권, AddDevice 성공/실패/누수, 초기 IRP, 파일 허용, 상태 롤백, 지연 완료, MPR/중첩/대기 continuation, 실패 원자성을 검증합니다. 원본 실제 WDK `driver_wdm_pnp.c`는 선택적 `NEVERD_WDM_PNP_FIXTURE`/`NEVERD_WDM_PNP_CFG_FIXTURE`를 사용합니다. `DriverWDMPnpTests.cpp`는 일반/활성 CFG 재배치 이미지에서 AddDevice, 파일 I/O, 순서 있는 제거, 지연 시작/제거, 시작/query 실패, 누수 유무에 따른 AddDevice 실패를 실행합니다. 산출물 누락은 명시적으로 건너뜁니다. 실행 근거는 Linux에 한정되며 문서화한 리소스 없는 PnP 부분집합만 입증합니다. `DriverScenarioPublicTests.cpp`는 일반/활성 CFG 이미지의 7개 요청 지연 PnP 보고서를 C API와 CLI로도 검증합니다.

V9 schema 테스트는 여덟 부 기능 이름의 왕복과 공유 최종 상태 검증을 확인하고 QueryStop 0x119를 이미지 로딩 전에 거부합니다. 확장 모델/실제 픽스처는 query-stop 롤백, cancel-stop, 중지/재시작, 갑작스러운 제거, 정확한 성공 상태, 중지/제거 대기 소프트웨어 I/O, 제거 후 게스트 거부, 장치 식별자와 혼합 AddDevice 결과를 검증합니다. `DriverScenarioPublicTests.cpp`는 일반/활성 CFG 픽스처에서 16개 요청을 C API 및 CLI로 실행해 소프트웨어 IOCTL 성공 바이트, 제거 후 게스트 실패, 최종 cleanup/close/remove를 보존합니다. 공개 실행은 순차적이므로 현재 생산자가 없는 보류 IRP를 나중 start/cleanup 요청으로 해제할 수 없습니다. Remove 종료 조건은 프로필 경계이며 일반 Windows I/O 허용 정책이 아닙니다. 근거는 Linux에 한정됩니다.

`KernelRemoveLocksTests.cpp`는 독립 소유권, NULL/중복 Tag, retail/DBG 크기, 즉시/지연 drain, 실패한 획득의 의무, 원자성, 용량 및 퇴역을 검증합니다. `KernelRemoveLockBridgeTests.cpp`는 연결 전 초기화, 확장부 범위, 불투명 영역, IRQL 및 위험한 Delete/Detach의 변경 전 거부를 검사합니다. 실제 `driver_wdm_remove_lock.c`의 retail/DBG와 일반/active-CFG4종은 `NEVERD_WDM_REMOVE_LOCK_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`, `NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`로 지정합니다. `DriverWDMRemoveLockTests.cpp`는 패킷 퇴역 후 release, drain 이후 버스 완료, 콜백 반환 전 준비, 워커 대기 및 깨끗한 AddDevice 실패를 확인합니다. C API/CLI는 기존 PnP 스키마로 수신/완료/최종 제거를 구분합니다. 산출물 부재는 명시적으로 건너뛰며 Linux 증거는 완전한 Driver Verifier나 일반 동시 drain을 의미하지 않습니다.

`DriverPowerScenarioTests.cpp`는 필수 전원 사실, JSON/네이티브 일치, 불투명32비트 context, FIFO 제한 및 독립 자식 보고를 검증합니다. `KernelPowerRequestTests.cpp`와 `KernelPowerCompletionTests.cpp`는 패킷 배치, 경로 플래그, 수명과 알림 상태 구분, FIFO 매칭, 최종 콜백 소유권, MPR, 대기 및 해제 경계를 검사합니다. 정품 WDK의 자체 `driver_wdm_power.c`는 선택적 `NEVERD_WDM_POWER_FIXTURE`/`NEVERD_WDM_POWER_CFG_FIXTURE`를 사용합니다. `DriverWDMPowerTests.cpp`는 일반/active-CFG 재배치, 직접/중첩 Query/Set, 독립 지연 완료, S0가 D0보다 먼저 완료되는 순서, 대기를 넘는5인수 스냅샷, 워커 출처 자식, null 콜백, query 거부, PDO별 초기값/FIFO 및 누락 사실 실패를 확인합니다. `DriverScenarioPublicTests.cpp`는 잘못된 입력 사전 검사와 C API/CLI의6시나리오/3자식 절전·복귀를 추가합니다. 산출물이 없으면 명시적으로 건너뛰며 Linux 실행 증거는 문서화된 페이지 가능·리소스 없는 전원 부분집합에 한정됩니다.

`KernelUsbIdleTests.cpp`는 소유권·D2·대여·첫 원인, `KernelUsbIdleBridgeTests.cpp`／`KernelUsbIdleReceiptTests.cpp`는 실제 IRP·취소·복합 용량·수신/완료 순서, `DriverUsbIdleScenarioTests.cpp`는 입력과 보고를 검증합니다. `DriverWdmUsbIdleTests.cpp`는 실제 `driver_wdm_usb_idle.c`와 `NEVERD_WDM_USB_IDLE_FIXTURE`／`NEVERD_WDM_USB_IDLE_CFG_FIXTURE`로 idle, D0/D3, 취소, 깨우기, 재등록/재시작, 독립/복합 기능 및 PDO/FDO 경로를 검사합니다. `DriverWdmUsbIdlePublicTests.cpp`의 [USB 시나리오](../examples/driver-wdm-usb-idle-scenario.json)는 C API/CLI, 일반/active-CFG, 선호/재배치를 다루며 아티팩트가 없으면 명시적으로 건너뜁니다. 실행 증거는 Linux만 해당하며 KMDF USB 지원을 뜻하지 않습니다.

`KernelFrameworkUsbIdleTests.cpp`, `KernelFrameworkUsbIdleStorageTests.cpp`, `KernelFrameworkUsbIdleBridgeTests.cpp`는 정책·실제 저장·형식화된 스케줄링을 검사합니다. 실제 `driver_kmdf_usb_idle.c`는 USB idle IRP를 직접 보내지 않습니다. `DriverKMDFUsbIdleTests.cpp`는 허가 없음, 관리 I/O와 지연 D2/D0, 콜백 전/중 StopIdle, arm 실패, Maximum 능력, 원격 깨우기와 복합 구성원을 검사합니다. `DriverKMDFUsbIdlePublicTests.cpp`는 [KMDF USB 시나리오](../examples/driver-kmdf-usb-idle-scenario.json)를 `NEVERD_KMDF_USB_IDLE_FIXTURE`／`NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`로 C API/CLI, 일반/active-CFG, 선호/재배치 실행합니다. 누락은 명시적으로 건너뛰며 실행 증거는 Linux뿐입니다. 모델 테스트는 wake-arm 콜백 성공 후 할당이 고갈되어도 실제 disarm과 WAIT_WAKE 취소를 수행하고 D2 응답을 소비하지 않는지 확인합니다.

`DriverKMDFUsbPoFxTests.cpp`는 초기 SystemManaged／WithHint, 별도 PoFx／USB 허가, D0 취소, 지연 D2／D0와 실제 worker F0 응답, StopIdle, READ 이전 깨우기 복구, arm 실패, 제거와 재시작을 검사합니다. `DriverKMDFUsbPoFxPublicTests.cpp`는 [USB PoFx 시나리오](../examples/driver-kmdf-usb-pofx-scenario.json)를 C API／CLI, 두 모드, 일반／active-CFG, 기본／재배치에서 실행합니다. `DriverKMDFUsbIdleTests.cpp`는 전달 회귀를 보존하고 직접 READ의 D0Entry 이후 전달을 확인합니다. `KernelFrameworkRequestTests.cpp`는 매핑, caller-context 소유권, 수동／중지 큐와 IRQL을 검사합니다. 할당 고갈과 독립 게이트는 모델 브리지 테스트가 증명하며 실제 fixture는 arena를 고갈시키지 않습니다. 누락 fixture는 명시적으로 건너뛰고 실행 증거는 Linux뿐입니다. `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait`는 RemovePending 중 실제 D0Entry 실패를 따로 검사합니다. 정확한 Required 응답과 quiesce로 F0／ActiveCondition 없이 정리하며 일반 SET_POWER 실패나 자체 surprise removal 지원을 뜻하지 않습니다.

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: [네이티브 WAIT_WAKE 시나리오](../examples/driver-wdm-wait-wake-scenario.json)는 실제 WDK `driver_wdm_wait_wake.c`를 `NEVERD_WDM_WAIT_WAKE_FIXTURE`／`NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE`로 실행합니다. START 발행, 인수, 자동 D0 없는 깨우기, 재발행, 취소, MPR, DPC 취소 후 작업자 D0, 정확한 이벤트 캡처 및 독립 공급자를 검사합니다. 일반／active-CFG 및 선호／재배치 주소 실행 증거는 Linux에 한정되고 파일 부재는 명시적으로 건너뜁니다.

`KernelPowerCompletionTests.cpp`는 APC／DPC 승인, 용량 실패의 원자적 재시도, 공급자만 있는 동기／지연 및 콜백 유무, 캡처 경로와 MPR을 검사합니다. 실제 `DriverWdmWaitWakeTests.cpp`는 DPC 취소 콜백의 직접 D0, APC／DPC Query/Set, IRQL／CR8 유지, 호출 반환 후 PASSIVE 실행, 상승 IRQL WAIT_WAKE 거부를 검사합니다. [상승 IRQL 시나리오](../examples/driver-wdm-elevated-power-scenario.json)는 `DriverWdmWaitWakePublicTests.cpp`에서 C API／CLI, 일반／active-CFG, 선호／재배치를 실행하며 증거는 Linux만 해당합니다.

`DriverResourceScenarioTests.cpp`는 JSON／네이티브 명시 사실, 정수 폭, 개수, 물리／레지스터 중첩, 정렬, ID, 빈 뱅크와 설정 직렬화를 검사합니다. `KernelMMIOTests.cpp`, `KernelMMIOFailureTests.cpp`, `KernelResourceBridgeTests.cpp`, `UnicornMMIOTests.cpp`는 뱅크／매핑 소유권, 별칭, 세대, packed 목록 수명, 제공자 시점, 재시작 값 보존, 갑작스러운 제거／전원 접근성, 정확한 CPU／API 트랜잭션과 실패 원자성을 검증합니다. 자체 정품 WDK `driver_wdm_resources.c`는 `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`를 사용합니다. `DriverWDMResourceTests.cpp`는 실제 스칼라／REP 접근자, 일반／active-CFG 재배치, 하위 구간 별칭, 페이지 끝 매핑, STOP／재시작과 잘못된 접근을 실행합니다. C API／CLI는 이미지 로드 전에 잘못된 사실을 거부하고 동일한14요청 재시작 시나리오에서 유지된 IOCTL 출력과 정확한 map／unmap 횟수를 확인합니다. 공통 [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json)에는 이 fixture의 레지스터／IOCTL 프로토콜이 필요합니다. 산출물 누락은 명시적으로 건너뛰며 증거는 Linux에 한정됩니다. 호스트 물리 메모리나 일반 장치 백엔드는 실행하지 않습니다.

`DriverDMAScenarioTests.cpp`는 명시적 기능, 논리 도메인, 바이트／개수／시간 제한, 엄격한 이벤트 방향, 설정／관측 분리를 검증합니다. `KernelPhysicalMemoryTests.cpp`와 `BackendBackingTests.cpp`는 공유 페이지 할당 경계, 고정 참조, CPU 권한 불변, MMIO／재진입 배제, 전체 범위 검증 실패 원자성을 확인하며 `KernelRequestMDLTests.cpp`는 구성된 설명자 별칭이 같은 물리 식별자를 사용하는지 검사합니다. `KernelDMATests.cpp`, `KernelDMABridgeTests.cpp`, `SchedulerDMATests.cpp`는 실제 RAM 바이트, 어댑터 연결 테이블 호출, 인라인／대기 FIFO 소유권, 별개인 콜백／매핑 수명, 페이지 조각, 잘못된 방향, 해제 사전 검증, 독립 PDO 도메인, 세대／전원 실패를 실행합니다. 직접 작성한 실제 WDK fixture `driver_wdm_dma.c`는 `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE`를 사용합니다. `DriverWDMDMATests.cpp`와 C API／CLI는 실제 어댑터 포인터, 공통／SG 저장소, 별도로 설정한 DMA／인터럽트 이벤트를 실행합니다. 공유 [driver-dma-scenario.json](../examples/driver-dma-scenario.json)은 이 fixture 프로토콜이 필요합니다. 산출물이 없으면 명시적으로 건너뜁니다. 실행 증거는 Linux에 한정되며 실제 호스트 DMA, PCI, 일반 장치 엔진을 보장하지 않습니다. `pluginsdk/python/tests/test_driver_dma_integration.py`는 `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_FIXTURE`, `NEVERD_TEST_WDM_DMA_CFG_FIXTURE`로 기존 소유 JSON 바인딩을 실행하여 바이트, 콜백 순서, 보고된 실패를 검증합니다.

`KernelSEHTests.cpp`는 순수 스택 해제 계획, 범위 순서, 비휘발성 GPR 복원, 제한된 스택 및 명시적 미지원 메타데이터를 검사합니다. `KernelExceptionTests.cpp`는 정확한 API 인수 수, 하위 32비트 상태, 형식화된 예외, IRQL 상한과 변하지 않는 모델／CPU 상태를 확인합니다. 실제 WDK `/GS-` `driver_wdm_seh.c`는 선택적 `NEVERD_WDM_SEH_FIXTURE`／`NEVERD_WDM_SEH_CFG_FIXTURE`를 사용합니다. `DriverWDMSEHTests.cpp`는 일반／활성 CFG／재배치 이미지에서 직접 또는 헬퍼를 통한 예외, 중첩 처리기, 재발생, 실제 필터, 스택 해제 finally, 검색 순서, 안정적인 예외 레코드, 지원하는 CPU 오류의 후속 실행과 전체 CPU 상태 복원을 검사합니다. 중첩 필터와 충돌한 finally는 연결된 논리 스택을 사용하며 관련 없는 CPU 오류는 명시적으로 거부합니다. C API／CLI는 [driver-seh-scenario.json](../examples/driver-seh-scenario.json)을 실행해 null API 결과와 실제 게스트 처리기 메시지를 검증합니다. `pluginsdk/python/tests/test_driver_seh_integration.py`는 `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_SEH_FIXTURE`, `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`를 사용합니다. 외부 이미지가 없으면 명시적으로 건너뛰며 증거는 Linux에 한정되고 사용자 버퍼나 일반 SEH 지원을 입증하지 않습니다.

`KernelDMAChannelTests.cpp`, `KernelDMAChannelBridgeTests.cpp`, 공유 `SchedulerDMATests.cpp`는 혼합 할당 FIFO, 콜백 반환 폭, 순수 수용／해제 검증, 레지스터 재사용, 연속 페이지 조각, 작업 전체 플러시, CurrentIrp 스냅샷과 패킷／MDL／장치 수명을 확인합니다. 직접 작성하고 실제 WDK로 빌드한 `driver_wdm_dma_channel.c`는 선택적 `NEVERD_WDM_DMA_CHANNEL_FIXTURE`／`NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`를 사용합니다. `DriverWDMDMAChannelTests.cpp`는 일반／활성 CFG／재배치 드라이버로 실제 MapTransfer 및 FlushAdapterBuffers 호출, 공통／SG／채널 공유 할당량, 명시적 장치 트랜잭션, IRQ/DPC 완료, 순차 작업, 두 PDO와 실패 사례를 실행합니다. C API／CLI는 두 매핑 조각 전체를 가로지르는 단일 트랜잭션을 포함하여 7개 요청의 [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json)을 실행합니다. `pluginsdk/python/tests/test_driver_dma_channel_integration.py`는 `NEVERD_TEST_LIBNEVERD`, `NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE`, `NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE`로 같은 공개 JSON 인터페이스를 사용합니다. 산출물이 없으면 명시적으로 건너뛰며 Linux 증거는 시스템 DMA 컨트롤러나 임의 HAL 매핑／플러시 패턴의 지원을 입증하지 않습니다.

`DriverInterruptScenarioTests.cpp`는 명시적 raw／변환 설명자, 혼합 및 인터럽트 전용 할당, 엄격한 이벤트 필드／개수, 원본 식별자와 독립 BOOLEAN 관측을 검증합니다. `KernelInterruptsTests.cpp`, `KernelInterruptBridgeTests.cpp`, `SchedulerInterruptTests.cpp`는 독점 튜플 일치, 불투명 토큰, 세대／연결 캡처, 이벤트 수명, 선택한 Ex 필드만 읽기, 같은 잠금과 IRQL 복원, 콜백 소유권, 같은 시각 ISR 우선순위와 변경 전 용량 실패를 검증합니다. `KernelFrameworkRequestTests.cpp`는 호출을 공개하거나 참조를 소비하지 않는 순수 취소 사전 검증 및 일괄 토큰 용량을 확인합니다. 실제 WDK로 빌드하는 자체 fixture `driver_wdm_interrupts.c`는 `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`를 사용합니다. `DriverWDMInterruptTests.cpp`는 일반／active-CFG 재배치, 기존 11인수 ABI, Ex 버전 1／2／4, 실제 ISR→DPC 완료, AL 하위의 FALSE, 동기화／수동 잠금, 독립 PDO, 재시작 세대와 잘못된 하드웨어 사실을 실행합니다. C API／CLI 테스트는 이미지 로드 전에 잘못된 선언을 거부하고 7개 요청의 [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json)을 실행해 대기 IOCTL 바이트와 별도 전달 관측을 확인합니다. 이미지 누락은 명시적으로 건너뜁니다. 실행 증거는 Linux에 한정되며 공유／레벨／MSI 인터럽트나 명령어 단위 선점을 입증하지 않습니다.

`DriverGuardTests.cpp`와 네 가지 원본 `driver_guard.c` 변형은 활성/비활성 CFG, 기준 주소 재배치, check/dispatch ABI와 잘못된 대상을 검증합니다. `KernelFrameworkTests.cpp`, `KernelFrameworkControlTests.cpp`, `KernelFrameworkQueueTests.cpp`, `KernelFrameworkRequestTests.cpp`는 바인딩, 실패 시 롤백되는 장치 생성, 큐 라우팅, 논리적 버퍼 길이와 정리 순서와 IRP/컨텍스트 수명을 검증합니다. 원본 `driver_kmdf_lifecycle.c`와 `driver_kmdf_control.c`는 실제 WDK 1.33 헤더로 선택적으로 컴파일하고 진짜 `FxDriverEntry` 라이브러리를 통해 링크합니다. CMake 캐시 경로에서 수명 주기 이미지에는 `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE`를, 일반 및 활성 CFG 제어 장치 이미지에는 `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE`를 지정합니다. 외부 산출물이 없으면 명시적으로 건너뜁니다. `DriverKMDFLifecycleTests.cpp`, `DriverKMDFControlTests.cpp`와 `DriverScenarioPublicTests.cpp`의 C API/CLI 사례는 실제 콜백, 버퍼/직접 I/O, 작업 항목을 통한 대기 요청 완료, 실패 상태, 언로드와 재배치된 CFG 실행을 검증합니다. 실행 증거는 Linux에 한정되며 완전한 KMDF나 PnP/전원 관리 지원을 입증하지 않습니다.

이전 취소 API 테스트는 취소, 중첩 정리와 최종 파괴까지 API 연속 실행을 유지합니다. 이미 취소된 요청에서 Ex는 계속 콜백 없이 취소 상태를 반환합니다. `KernelFrameworkRequestAccessorTests.cpp`와 `KernelRequestMDLTests.cpp`는 공유 64비트 Information, 완료 시 길이 검증, 원래 큐/IRP 식별, NULL WDF 파일 핸들, 유지된 핸들의 getter 결과, 버퍼 MDL 캐시와 첫 방향 ByteCount, 직접 설명자 식별과 지연 매핑, 완료 시 무효화 및 WDM 완료 우회 거부를 검증합니다. 실제 제어 장치 fixture의 L, M, D, C 모드는 일반/활성 CFG 이미지에서 이전 API를 통한 취소, 버퍼 MDL/정보, 직접 READ/WRITE MDL과 완료 후 접근을 실행합니다.

취소 테스트는 전송 요청에만 허용되는 가상 기한과 보고서 필드, 완료 우선 및 이미 취소된 경로, 표시/해제 결과, 큐 등록과 전달 완료 상태의 완료 권한, 콜백 대기와 내부 참조 수명을 검증합니다. 스케줄러 테스트는 DPC/취소/작업 항목 순서, 용량, 식별자 분리와 중단/복원을 독립적으로 검증합니다. WDM 취소는 여전히 명시적 모델 오류입니다.


## 테스트 배치

`add_neverd_unittest`는 GoogleTest 실행 파일 하나를 만들고 발견한 각 사례에 실행
target 이름과 같은 CTest label을 지정합니다.

| 소스 영역 | Target 및 CTest label | 범위 |
|-----------|------------------------|------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | 교차 플랫폼 하위 프로세스 호출, quoting, redirect, 종료 코드 |
| `unittests/libc` | `NeverDLibCTests` | 알려진 libc 이름과 분류 |
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | 싱크 카탈로그, 신원 우선순위, 인수 사전 필터, 복사 오버플로 헌트, 힙 수명 감사, 필수 PE/ELF/Mach-O × x86-64/AArch64 6셀 매트릭스 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR 모양, IR 단계, loader, relocation, 포맷 fixture, 디컴파일, 대표 patch 흐름 |
| `unittests/semantic`의 대부분 파일 | `NeverDSemanticTests` | 명령어, ABI, 제어 흐름, C 표현식, lift/recompile 차등 의미론 |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | hardfork metadata, input normalization, ABI/signature ambiguity, CFG/SSA/recovery, decoder boundary 전수 검사와 hostile input, proxy/call fact, interpreter semantics, LLVM/C/Solidity differential execution, public API routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | v0-v4 메타데이터와 ELF 레이아웃, 엄격한 verifier/loader 동작, 고정된 ELF 아티팩트 23개, 독립 official oracle, 모든 opcode 가용성, 적대적 입력, CFG/복원, 실행된 LLVM/C/Rust 차분 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 네 ISA×세 object 포맷 재작성/난독화 동등성 |
| `unittests/semantic`의 집중 변환 파일 | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | 큰 의미론 바이너리에서 분리한 빠른 재링크 probe |
| `unittests/corpus`(submodule) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests`, `NeverDAdaDEHCorpusTests` | pin 된 실제 바이너리 545개에서 읽어내는 예외 및 런타임 metadata. 각 바이너리는 manifest에 복원이 넘어야 할 하한을 선언한다 |

등록의 기준은
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt),
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt),
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt),
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt)입니다.

### pin 된 바이너리 corpus

다른 모든 스위트는 테스트 대상을 직접 빌드하지만 corpus는 그렇지 않습니다. 이것은
실제 툴체인이 이 저장소가 닿을 수 없는 호스트에서, 닿을 수 없는 타깃을 대상으로
만들어낸 바이너리들의 submodule이며, 각 파일은 다이제스트로 pin 되고 옆의 manifest가
그 복원이 넘어야 할 하한을 선언합니다. "`-O2`로 strip 된 `armv7` 공유 오브젝트에서
NeverD가 무엇을 읽어내는가" 같은 물음에 논쟁이 아니라 답을 줄 수 있는 곳은 여기뿐
입니다.

이 스위트들은 configure가 그것들을 찾도록 지시받았을 때에만 빌드되므로, 이 플래그가
곧 그것들이 테스트되고 있는지의 전부입니다.

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus`는 모든 라인을, `check-neverd-windows-eh-corpus`,
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus`,
`check-neverd-cxx-itanium-eh-corpus`, `check-neverd-objc-eh-corpus`, `check-neverd-ada-d-eh-corpus`는 각각 한 라인을
실행합니다. CI의 세 호스트 모두 이 플래그로 configure 하고 여섯 라인을 전부
실행합니다. 바이트는 어디서나 같지만 그것을 읽는 쪽은 같지 않으며, 한 호스트에서의
corpus 실행은 나머지 두 호스트에 대해 아무것도 증명하지 않습니다.
`scripts/audit_ci_test_inventory.py`는 여섯 label 중 하나라도 빠진 inventory를
거부합니다. corpus를 조용히 읽지 않게 된 빌드는 어떤 테스트도 잡을 수 없는
회귀이기 때문입니다. 사라진 것이 바로 그 테스트입니다.

live EVM opcode audit는 다음 명령으로 실행합니다.

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

public CLI가 받는 유일한 option은 `--manifest-output`이며 remote/ref/toolchain override를 제공하지
않습니다. 출력 manifest의 closed contract는 `schema 3`입니다.

로컬과 CI의 표준 경로는 공식 `https://github.com/ethereum/go-ethereum.git`에
`git fetch --depth=1 --force`를 실행해 default
branch의 remote `HEAD`에서 방금 얻은 정확한 SHA만 detached worktree에서 검사합니다.
실행마다 예측할 수 없는 이름의 private temporary bare repository를 쓰고, official fetch의
authority ref와 exact SHA를 detached worktree 수명 동안 유지한 뒤 repository와 worktree를 함께
폐기합니다. shared persistent Git repository나 cache는 없습니다. `local_docs`,
기존 checkout, submodule은 감사 경로가 아니며, pin된 submodule은 live drift를 찾아야 할 때
낡습니다.

각 Git command는 상속된 `GIT_*`를 먼저 전부 지우고(`GIT_CONFIG_*` 포함), 검토한 값만
설정합니다. `GIT_CONFIG_NOSYSTEM`과 `GIT_CONFIG_GLOBAL`은 system/global config를,
`GIT_ATTR_NOSYSTEM`과 command scope의 `core.attributesFile`은 system/global attributes를,
`core.hooksPath`는 hooks를 끕니다. 예상 밖 private-repository config, graft,
`objects/info/alternates`, `refs/replace`는 검증을 실패시키고,
`GIT_NO_REPLACE_OBJECTS`도 replacement lookup을 비활성화합니다.

probe는 `params.Rules`가 export한 모든 bool field를 반사하고 각 fork에서
`LookupInstructionSet(params.Rules)`를 호출해 256 byte slot 전부를 스캔합니다.
`EVMUpstreamOpcodePolicy.def`는 typed historical/unscheduled-EOF exclusion과 alias를,
`EVMUpstreamSemanticsPolicy.def`는 closed Rules inventory, fork mapping, base-stack exception,
EIP-8024 dynamic opcode family 선언을 각각 소유합니다.

CI는 `dev` push, pull request, manual dispatch, daily schedule에서만 같은 live audit를
실행합니다. Go probe는 매핑된 각 fork에서 공개 `LookupInstructionSet(params.Rules)` API를
호출합니다. `EVMUpstreamOpcodePolicy.def`는 name alias와 검토된 historical/unscheduled-EOF
exclusion을, 직교하는 `EVMUpstreamSemanticsPolicy.def`는 fork rule, stack semantics 예외,
EIP-8024 dynamic opcode family의 membership/activation을 소유합니다. closed manifest는 정확한 revision, fork activation, byte/name,
`base_min_stack`, `net_stack_delta`를 검사하며 알 수 없거나 중복된 field, fork, name, byte를
거부합니다. allocation은 `operation.undefined`만으로 판정하고 `HasCost`는 defined zero-cost
operation에도 false이므로 cost cross-check로만 씁니다. 모든 `defined && !HasCost` slot은
선언된 fork부터 `EVM_GETH_ACTIVE_WITHOUT_COST`와 정확히 일치해야 합니다. cost가 있는 undefined
slot, 검토하지 않은 defined slot, marker 소실은 fail closed입니다.
CI 실패 시 정확한 revision, manifest, log가 artifact로 올라갑니다. parser와
drift diagnostic에는 독립적인 Python unit coverage가 있습니다.

`EVMUpstreamSemanticsPolicy.def`는 export된 boolean `params.Rules` field마다 정확히 하나의
`EVM_GETH_RULE_FIELD`를 두고 `MappedForkSelector`, `NoOpcodeAllocation`,
`ExcludedSelectorExpectedError`로 분류합니다. probe는 field 하나만 켜서 `LookupInstructionSet`을
호출합니다. 앞의 두 category는 nil error, 세 번째는 error여야 하며 반환된 전체 256-slot
opcode/stack fingerprint는 `ExpectedFork`와 일치해야 합니다. 현재 `IsEIP155`, `IsEIP2929`,
`IsEIP4762`, `IsPetersburg`는 Frontier fingerprint인 no-allocation fields이고, `IsUBT`는 error와
Cancun fingerprint가 기대값입니다.

`EVMEIP8024Immediates.def`는 계속해서 single/pair의 각 byte에 대한 immediate semantics의 유일한
authority이며, 각각 256개 byte 전부를 명시적으로 분류합니다. production은 직접 lookup합니다.
live audit는 `go -overlay`로 `core/vm`에 virtual wrapper를 주입해 실제 private
`operation.execute` handler를 얻고, active table/family마다 `DUPN`, `SWAPN`, `EXCHANGE`의
`3x256` candidates와 `3 missing-operand cases`를 실행합니다. acceptance, PC delta,
marker-derived operand/stack mutation, valid case의 정확한 underflow, operand 누락 시 `0x00`을
확인하며 Python은 formula를 다시 쓰지 않고 같은 `.def`와 비교합니다.

`EVM_HARDFORK_LATEST`의 canonical target은 하나뿐입니다. closed
`EVMUpstreamForkAliases.def`는 Prague→Pectra, Osaka와 BPO1~BPO5→Fusaka,
Paris/Shanghai/Cancun/Amsterdam/Bogota→자기 자신을 정의하며 알 수 없는 이름은 fail
closed입니다. 기록된 하나의 `audit_unix_time`으로 `MainnetChainConfig.LatestFork(time)`
(NeverD latest와 일치해야 함)와 `LatestFork(max uint64)`의 alias/probed canonical fork를
검사합니다. probe는 실제 `canonical fork jump tables`와 `mainnet active/scheduled jump tables`를
열거해 table별로 완전 비교하고 dynamic family 또는 fork의 `inactive` 상태도 명시적으로
기록합니다. table/family/probe의 일부만 얻은 `partial` result는 받지 않고 fail closed입니다.
manifest는 `authority=official-fresh-fetch`, 공식 URL,
요청한 `HEAD`, SHA를 고정합니다. public CLI에 remote/ref/toolchain bypass는 없고 probe는
`GOTOOLCHAIN=local`을 사용합니다.

Go request/response와 Python controller는 hostile metadata를 allocate하기 전에
`input/collection/string hard limits`를 적용하고 한도를 넘는 input, array, string을 fail
closed합니다. 별도로 `bounded diagnostic output`을 강제하여 너무 긴 display에 full-content
`digest`와 `explicit truncated marker`를 포함합니다. 모든 command에는 bounded child output과
공통 deadline이 적용되며 timeout 또는 output-limit 위반은 전체 `process group`과 하위
process tree를 kill하고 pipe를 drain합니다. 모든 `.def parser`는 unparsed, unknown, duplicate, missing,
out-of-range entry를 거부하고 fail closed합니다.

현재 schema-3 live receipt는 `schema_version=3`, `audit_unix_time=1787534659`,
`authority=official-fresh-fetch`, `remote=https://github.com/ethereum/go-ethereum.git`,
`ref=HEAD`, revision `02b73d4ea7181464175e0a6cbecc0a3a2655a562`, local `Go 1.24.0`,
`stack_limit=1024`, `diagnostics=[]`를 기록합니다. `21 fork tables`와 `20 Rules probes`를
다루며 분류는 `15 mapped/4 no-op/1 expected-error`입니다. 두 `mainnet active/scheduled`
record는 모두 `upstream BPO2`를 보고하며 closed map으로 `NeverD Fusaka`에 대응됩니다.
EIP-8024의 `23 table targets` 가운데 `Amsterdam/Bogota`만 active이며
`1536 candidate executions`와 `6 missing-operand cases`를 생성합니다. `three handler symbols`는
두 active target에서 일치합니다. Python audit는 `67/67`, `C++ Opcode 10/10`입니다. macOS 실제
run은 `sandbox-exec` 안에서 성공했고 마지막 `go run`은 offline이었습니다. Linux workflow는
`bubblewrap`을 강제합니다.

모든 Go stage인 `go env`, `go mod init`, `go mod edit`, `go mod tidy`, `go mod download`,
`go run`은 `capability-root` filesystem sandbox를 통과해야 합니다. read capability에는 private
probe, fresh geth, 검증된 `resolved GOROOT`, 필요한 system runtime root의 정확한 집합만 포함되고
isolated environment root만 쓸 수 있습니다. network는 필요한 dependency stage에만 허용되며 final
run은 offline입니다. test는 `host HOME/workspace`에 sentinel을 놓고 접근이 거부되며 어떤 output에도
그 내용이 나타나지 않음을 요구합니다. Linux는 `/` broad bind가 없는 동형의 `bubblewrap` policy를
검증합니다.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

CMake에 등록된 EVM test target 11개는 다음과 같습니다.

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests`는 decoder가 달라지는 각 fork의 모든 2-byte input에 대해
완전한 decode와 정확한 `JUMPDEST` boundary를 비교하고, 길이가 제한된 결정적 hostile input을
모든 fork에 통과시킵니다.

EVM control-flow 변경에서는 fixed-point 및 height-domain contract를 먼저 실행합니다.

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

이 case들은 block을 가로지르는 internal return, finite multi-target merge, loop convergence,
deterministic edge ordering, path-sensitive whole-stack lane, correlation preservation, unknown
jump, exact invalid target, fail-loud budget, strict/relaxed stack fault를 포함합니다.
`MayReachable`은 CFG candidate일 뿐 확정 semantic fact를 만들지 못합니다. 이어서 11개 EVM
target과 live upstream audit를 모두 실행하세요.

MedIR/HighIR dataflow 변경에서는 constant-phi, selector, typed-operand,
malformed-graph, deep-chain contract도 실행합니다.

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

이 case들은 equal/conflicting cyclic phi, 인접하지 않거나 block을 가로지르는 selector
expression, 두 equality operand order, exact ABI width check, typed
storage/event/calldata operand, malformed MedIR의 deterministic handling, 16,384-value iterative
producer walk를 검증합니다.

## fixture 생성 방식

### Lift 및 포맷 fixture

`unittests/lift/CMakeLists.txt`는 빌드 중 C와 assembly 소스를 교차 컴파일합니다.
Clang target triple이 x86-64, i386, AArch64, ARM32 ELF object, PE/COFF object와
링크된 image, PIC/no-PIC Mach-O i386 object를 만듭니다. LLD가 있으면 선택한
object를 patch 테스트용 실행 파일로도 링크합니다. `NeverDLiftTests`는
`lift-test-objects` target에 의존하므로 일반적인 테스트 바이너리 빌드가 생성
fixture를 갱신합니다.

대부분의 lift 테스트는 `NeverDLiftFixture.h`로 빌드된 `neverd` CLI를 호출하고
LowIR, MedIR, HighIR, LLVM IR, 생성된 C 또는 재작성 바이너리를 검사합니다. 집중된
수동 실험은 `NEVERD` 환경 변수로 CLI 경로를 override할 수 있습니다. 일반 CTest는
CMake에 포함된 실행 파일을 사용합니다.

### 메모리 안전성 fixture

`unittests/safety/fixtures/binaries`에는 x86-64와 AArch64용 PE, ELF, Mach-O
이미지가 체크인되어 있으며, 각 포맷이 제공하는 PDB 또는 dSYM 동반 파일과 함께
이미지마다 링커 MAP이 하나씩 들어 있습니다. MAP은 strip된 빌드가 유일하게
남기는 신원 정보이므로, 각 셀은 MAP을 명시적으로 지정한 분석도 함께 수행하여
타입도 소스 줄 번호도 남지 않았을 때 발견이 무엇을 주장할 수 있는지 고정합니다.
`NeverDSafetyIntegrationTests`는 모든 호스트에서 여섯 셀을 전부 실행합니다.
필요한 이미지나 동반 파일이 없으면 구성 단계에서 실패하며, 호스트 툴체인에 따른
건너뛰기 경로는 없습니다.

여섯 개의 동등한 바이너리는 하나의 소스 파일에서 나옵니다. `make`는 호스트
네이티브 smoke fixture만 다시 만듭니다. 체크인된 전체 행렬을 재생성하려면 다음을
사용합니다.

```bash
make -C unittests/safety/fixtures matrix
```

전체 재생성에는 Clang의 Linux/Windows 크로스 타깃, LLD의 COFF 도구, 두 Darwin
아키텍처, 그리고 `dsymutil`이 필요합니다. 디버그 경로는 재매핑되고 CodeView
명령줄 기록은 비활성화되므로, 체크인된 동반 파일이 개발자 작업 공간의 절대
경로를 담지 않습니다.

### Windows 예외 재구성

Windows 테이블 기반 예외를 변경할 때는 표현 테스트와 링크된 PE patch 테스트가 모두
필요합니다. 집중 lift-suite 필터는 정규화된 unwind/SEH/C++ 모델, 손상 입력 처리,
예외 CFG edge, HighIR, LLVM WinEH 생성, 예외 디렉터리 교체, Guard CF/EH
continuation 재구성을 검사합니다.

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

보호된 x64 assembly fixture에는 Clang Windows target과 `lld-link`가 필요하며 CMake
link는 `/guard:cf`와 `/guard:ehcont`를 사용합니다. cross-linker 누락으로 인한 skip은
final-image 경로의 증거가 아닙니다. 통합 사례가 통과하면 다시 작성된 PE를 재로드할 수
있고 runtime-function, unwind, load-config, Guard CF, Guard EH continuation 테이블이
정렬되고 파일에 존재하며 실행 가능한 target만 가리킨다는 것을 입증합니다.

링크된 FH3 fixture는 고정 상태 테이블, HighC 주석, personality 보존, 생성된 catch
target, 재로드한 IP-to-state 그래프로 네이티브 C++ closure를 독립적으로 검사합니다.

분석/네이티브 지원 표와 fail-closed patch 계약은
[Windows 예외 재구성](windows-exception-reconstruction.md)을 참조하십시오.

### 언어 예외 모델

Windows 테이블 모델이 아닌 모든 것은 하나의 집중 target 에 모여 있습니다.
`NeverDLanguageEHTests` 는 DWARF 프레임 체인, Itanium 언어별 데이터 영역,
ARM EHABI, Darwin compact unwind, Go 런타임 프레임 메타데이터, Rust panic
기구, 그리고 세 가지 Objective-C 런타임을 다룹니다.

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

이 스위트의 테이블은 컴파일이 아니라 바이트 단위로 조립합니다. 검증하려는 조합
대부분은 단일 toolchain 이 한꺼번에 내보내지 않기 때문입니다. Objective-C 가
가장 분명한 사례입니다. 세 런타임 모두 Itanium LSDA 를 내보내고, 차이는 타입
테이블 슬롯에 무엇이 들어가는지 뿐이며, 그 차이는 정도가 아니라 전면적입니다.
Apple 의 슬롯은 `objc_typeinfo` 를 가리키며 그 첫 두 필드는 의도적으로
`std::type_info` 를 흉내 냅니다. GNUstep 의 Objective-C++ 슬롯은 진짜
`std::type_info` 파생 타입을 가리키고, GNU 런타임의 슬롯은 포인터조차 아닌
클래스 이름 문자열 그 자체입니다. 한 런타임의 규약을 다른 런타임의 테이블에
적용해도 실패하지 않습니다. 전혀 다른 것의 중간에서 읽어낸 클래스 이름을 보고할
뿐입니다. 그래서 슬롯을 읽기 전에 프레임의 personality 로 런타임을 먼저
확정합니다.

같은 스위트는 뭉뚱그리기 쉬우면서 뭉뚱그리면 틀리는 두 가지 구분도 고정합니다.
`@catch(id)` 와 `@catch(...)` 는 서로 다른 핸들러이며 — 앞의 것은 임의의
Objective-C 객체를 받고 외래 예외는 그 옆으로 지나가게 둡니다 — 런타임마다
표기가 다릅니다. 둘 다 catch-all 로 보고하는 디코더는 원래 지나쳤을 예외에
핸들러를 붙이는 셈입니다. 또한 setjmp/longjmp call-site 테이블은 주소가 아니라
호출 지점 색인을 담으므로, SJLJ personality 를 알아보지 못한 판독기는 오류를
내지 않고 프로그램이 지정한 적 없는 보호 구간과 landing pad 를 지어냅니다.

그 형식을 알아보는 것과 해독을 거부하는 것은 다릅니다. SJLJ 항목 하나는 ULEB128
한 쌍 — 디스패치 선택자와 action 오프셋 — 이며, 이 action 오프셋의 의미는 주소
형식에서와 완전히 같습니다. 따라서 action 체인도, catch 타입도, 예외 명세도, 코드를
전혀 가리키지 않는 표에서 그대로 읽어낼 수 있습니다. 읽어낼 수 없는 것은 각 항목이
지키는 구간뿐인데, 그것을 말해 주는 것은 함수가 자기 call-site 슬롯에 수행하는
저장이지 표 안의 무엇이 아니기 때문입니다. 이 스위트는 여기서 믿어서는 안 되는
바이트 하나도 못박습니다. call-site 인코딩으로 GCC 는 `DW_EH_PE_uleb128` 을,
LLVM 은 `DW_EH_PE_udata4` 를 적지만 둘 다 그 뒤로는 ULEB128 을 내보내며, 어떤
personality 도 그 바이트를 읽지 않습니다 — 그러니 디코더도 읽어서는 안 됩니다.

personality 의 정체도 함께 못박습니다. 위의 모든 표를 어떻게 읽을지 결정하는 것이
바로 그것이기 때문입니다. GNAT 은 GCC 가 모든 프런트엔드에 부여하는 세 가지 철자
— `_v0`, `_sj0`, `_seh0` — 로 자기 루틴의 이름을 짓고, Windows 에서는 한 심벌을
등록하면서 다른 심벌로 전달하므로 네 가지 철자가 모두 Ada 로 귀착해야 합니다. D 는
그 거울상으로, 세 개의 컴파일러, 한 루틴에 대한 세 개의 이름, 그리고 그 뒤에 있는
것은 동일한 표입니다.

### Unicorn 차등 왕복

의미론 fixture는 텍스트 모양이 아니라 동작을 테스트합니다.

1. 작은 C/assembly 사례를 작성하거나 LLVM IR을 구성합니다.
2. Clang/LLVM으로 요청한 대상을 위해 컴파일합니다.
3. 원래 machine code를 Unicorn에서 실행하고 예상 반환값이나 fixture가 정의한 상태를 캡처합니다.
4. NeverD로 로드하고 lift하여 LLVM IR을 출력한 뒤 다시 machine code로 컴파일합니다.
5. 같은 ABI, 입력, 메모리 배치, CPU 모델로 재생성 코드를 실행합니다.
6. 관측 가능한 결과를 비교합니다.

주요 구현은
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h)입니다.
patch-full fixture는 patch 작업과 같은 rewrite backend인
`Codegen::compileForRewrite`를 사용한 뒤 전체 4×3 ISA/포맷 grid에서 baseline과
변환 코드를 비교합니다.

결정적인 NeverD 의미론 실패는 실패 테스트여야 합니다. skip은 명시적인 외부 기능
경계에만 사용하고 이유를 읽으세요. 교차 linker가 없는 녹색 요약은 해당 포맷 경로가
실행되었음을 증명하지 않습니다.

### EVM 차등 백엔드

interpreter test는 deterministic 256-bit oracle입니다. emitter suite는 LLVM을
compile/execute하고 C23을 Clang으로 같은 host harness에 lower하며 `solc`, `anvil`,
`cast`, `jq`가 있으면 generated Solidity를 local node에 deploy합니다. status, storage,
instruction trace count를 비교합니다. 별도 raw-bytecode corpus는 Anvil native EVM에서
pre-Fusaka ALU, calldata/memory copy, overlapping `MCOPY`, Keccak, return data를 실행합니다.

Low/Med 테스트는 path-sensitive whole-stack execution lane과 phi lane identity를 보존하고
`MaxAbstractInstructionTransfers`를 포함한 budget exhaustion을 hard error로 만듭니다.
strict는 증명된 `Reachable` lane의 unknown/fork-inactive opcode만 거부하며
`MayReachable`은 definite fact를 만들지 않습니다. HighIR의 selector/receive/fallback은 root
lane과 성공 terminal로 제한됩니다. 공유 selector는 독립 standard evidence가 아니며,
standard별 `KnownFunctionVariantInfo`와 성공 terminal의 정확한 return shape가 일치할 때만
variant와 return list를 선택합니다.

interpreter는 opcode별 side effect 전에 typed stack preflight를 수행합니다.
`EVMForkSemantics.def`는 byte `0x44`를 Paris 이전의 `DIFFICULTY`, Paris부터의
`PREVRANDAO`로 정의합니다. `REVERT`, fault, step limit, resource exhaustion은 state를
rollback합니다. allocation failure는 `ExecutionFaultKind::ResourceExhausted`이고 entry
snapshot 자체를 만들지 못하면 `HasPersistentStateSnapshot`은 false이며 commit할 수 없습니다.

### EVM public boundary 및 budget regression

public API test는 canonical
`Code`/`Fork`/`Instructions`/`JumpDestinations`와 모든 LowIR table, range, ID, lane, edge
reference를 각각 변조합니다. `execute`는 instruction lookup 전에 `llvm::Error`를 반환해야 하고,
`lowerToMedIR`는 index 생성이나 입력 비례 allocation 전에 전체 malformed/over-budget LowIR를
거부해야 합니다. `lowerToMedIR`는 option validation, resource validation, structure validation을
이 순서로 수행하고 field별 `canonical decode replay` 및 `lowerCanonicalLowToMedIR`보다 먼저
완료해야 합니다. public HighIR recovery는 외부 LowIR/MedIR를 replay 검증하며 `analyze`만 자체
canonical IR에 `lowerCanonicalLowToMedIR`와 `recoverCanonicalHighIR`를 사용할 수 있습니다.
따라서 recursive/duplicate replay를 피하면서 모든 HighIR option/resource budget을 계속 적용합니다.
interpreter는 `EVMInterpreterLimits.def`의 모든 limit를 exact boundary/+1로
검증합니다. `MaxSteps`는 전용 `StepLimit`를 유지하고, `MaxMemoryBytes`, `MaxTraceEntries`,
`MaxLogEntries`, aggregate `MaxLogDataBytes`, runtime `MaxPersistentStateEntries` exhaustion은
`ResourceExhausted`로 transaction effect를 rollback합니다. 초기 aggregate
`MaxHostReturnDataBytes`나 persistent state 초과는 API error입니다. 초기 `MaxCalldataBytes`,
`BlockHashes`/`Balances`/`CodeHashes`/`ExternalCode`/`BlobHashes` 전체의 aggregate
`MaxHostEnvironmentEntries`, aggregate `MaxExternalCodeBytes`도 API error입니다.
`const execute preflight`는 environment, snapshot, result copy 전에 이를 거부합니다. return-data
`ArrayRef` view와 정렬 table의 `lower_bound` lookup도 buffer copy나 PC map 없이 검증합니다.

별도의 LowIR boundary test는 aggregate diagnostic limit `MaxLowDiagnostics`와
`MaxLowDiagnosticBytes`를 검증하며 linear decode/CFG construction이 정확한 count/final byte를
precharge하고 zero를 거부하는지 확인합니다.
HighIR safety test는 lane별 정렬 `Any/Exact/Excluded` domain, equality match/exclusion, raw
`XOR(selector, constant)`의 false-edge match/true-edge mismatch, zero word/calldata size/call
value refinement, unknown condition fail-closed를 다룹니다.
`EQ`와 `raw XOR` 두 back-jump regression은 다른 function이 `arguments`, `mutability`,
`return shape`, `region`을 오염시키지 않음을 보장합니다. `EVMAnalysisLimits.def`의
`MaxHighDispatchCandidates`, aggregate `MaxHighRecoveredArguments`, `MaxHighDiagnostics`,
`MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`, `MaxHighMemoryTransferCells`,
`MaxHighMemoryValueVisits`는 exact boundary/-1로 검증됩니다. fixed malformed diagnostic을 포함한
모든 output diagnostic은 allocation 전에 count와 최종 byte를 과금해야 합니다.
LowIR와 HighIR diagnostic budget은 독립적으로 검증하며 default root CFG region은 block-PC list를
reserve/copy하기 전에 `MaxHighRegionBlockReferences`를 과금해야 합니다.
외부 CALL/CREATE result는 nondeterministic host outcome으로 두 개의 정확한 CFG edge를 검사하므로
ERC-1167 fallback recovery가 유지됩니다. 읽을 수 없는 selector condition은 Unknown으로 남아
fallback/function fact를 만들 수 없습니다.

control-flow test는 `EVMLowFaultKinds.def`의 `InvalidJumpDestination`을
`end-of-code JUMPI`에 적용합니다. invalid target에서 확실히 true면 successful tail 없이 definite
fault이고 확실히 false면 성공합니다. unknown은 성공 가능한 false path를 남기며 lane 전체를 definite
fault로 표시하지 않습니다.

ABI test는 `EVMABIParserLimits.def`의 grammar boundary와 `EVMABITableLimits.def`의 public table
cardinality/text boundary를 exact limit/+1로 검증합니다. 또한 invalid kind/standard/evidence enum,
metadata mismatch, noncanonical signature/return list, 잘못 independent로 표시된 shared selector,
dangling/duplicate variant, word width가 아닌 event-topic `APInt`를 indexed selector/sorted topic
lookup 전에 거부합니다.

`NeverDEVMOpcodeTests`는 metadata architecture도 강제합니다. 할당된 opcode의
encoding/typed-value roundtrip, family boundary, hardfork alias, derived stack/host
maxima를 검증합니다.

### Solana SBF 차등 백엔드

SBF 메타데이터 테스트는 모든 버전 기능, opcode 충돌 경계, Murmur3 syscall hash, 재배치, ELF machine, 레지스터, VM 주소 상수를 검증합니다. Loader fixture는 vendored 바이너리 없이 레거시 v0-v2 section 레이아웃과 section이 없는 엄격한 v3/v4 program-header 레이아웃을 모두 생성합니다.

`NeverDSBFISAConformanceTests`는 v0-v4 각 version의 모든 byte encoding을 독립적으로
감사한 typed manifest와 대조합니다. `NeverDSBFExternalOracleTests`는 activation 및
boundary 결정을 별도로 build한 official Anza process와 비교합니다.
`NeverDSBFUpstreamConformanceTests`는 pinned Anza revision의 ELF 23개 모두에 명시적
outcome을 부여합니다.

`NeverDSBFSemanticTests`는 검증된 명령 바이트를 직접 실행하고 MedIR을 사용하지 않으므로, 정규화된 IR을 변경하거나 손상해도 source oracle과 backend가 우연히 일치할 수 없습니다. 비단조 v2 시맨틱, 메모리, syscall, 내부 call frame, fault, trace, resource limit을 다룹니다. LLVM module은 검증하며, 생성 C는 warning을 error로 처리하고 Rust는 `-D warnings`로 컴파일합니다. 공개 API 테스트는 생성된 엄격한 SBF ELF에서 모든 IR 단계, 디스어셈블리, CFG, 메타데이터, LLVM, C, Rust를 통과합니다.

## 일회성 target

custom target은 의존성을 빌드한 뒤 host CPU에서 정한 병렬도로 CTest를 실행합니다.

| CMake target | 선택 범위 |
|--------------|-----------|
| `check-neverd` | 등록된 모든 테스트 |
| `check-neverd-semantic` | `NeverDSemanticTests`만 |
| `check-neverd-sbf` | 모든 `NeverDSBF*Tests` target/case |
| `check-neverd-patch-full` | `NeverDPatchFullTests`만 |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests`만 |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests`만 |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests`만 |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests`와 `NeverDAvxUpperXformTests`에는 현재
`check-neverd-*` 편의 target이 없습니다. 아래처럼 빌드하고 label로 선택하세요.
`check-neverd-semantic`에도 별도 변환이나 patch-full 바이너리는 포함되지 않습니다.
완전한 집계에는 `check-neverd`를 사용하세요.

## 증분 CTest 워크플로

소유 실행 파일을 먼저 빌드한 뒤 label을 선택합니다. 관련 없는 큰 의미론 target을
다시 링크하지 않아도 됩니다.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# 모든 집중 EVM target/case
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# 모든 집중 Solana SBF target/case
cmake --build build-release --target check-neverd-sbf --parallel 4
```

GoogleTest에서 파생된 CTest 이름으로 단일 회귀를 실행합니다.

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

유용한 selector:

| 명령 | 목적 |
|------|------|
| `ctest --test-dir build-release -N` | 발견한 사례를 실행하지 않고 나열 |
| `ctest --test-dir build-release -L '<regex>'` | 테스트 바이너리 label 선택 |
| `ctest --test-dir build-release -R '<regex>'` | 사례 이름 선택 |
| `ctest --test-dir build-release --output-on-failure` | 실패 진단만 표시 |
| `ctest --test-dir build-release --stop-on-failure` | 첫 실패 후 중단 |
| `ctest --test-dir build-release --parallel 4` | 최대 네 사례 병렬 실행 |

GoogleTest discovery는 `DISCOVERY_MODE PRE_TEST`를 사용하므로 CTest가 나열하기 전에
해당 테스트 바이너리가 있어야 합니다. 사례별 timeout과 독립 discovery timeout은
`cmake/AddNeverD.cmake`에 정의되며 측정된 무거운 사례가 있는 스위트만 늘릴 수 있습니다.

## 코드와 함께 바뀌어야 하는 테스트

| 변경 영역 | 먼저 시작 | 다음 고려 |
|-----------|-----------|-----------|
| 아키텍처 lifter 또는 decode | `NeverDLiftTests`의 이름 있는 사례 | 해당 ISA 의미론 왕복 |
| LowIR CFG, 함수 감지, jump table | Lift CFG/switch 사례 | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests` |
| MedIR, ABI, 플래그, 타입, SSA | MedIR/호출 규약 lift 사례 | ISA 교차 `NeverDSemanticTests` 사례 |
| HighIR 또는 구조화 C | HighIR/decompile 사례 | `NeverDCFGLoopXformTests` 및 생성 C 컴파일 검사 |
| PE/ELF/Mach-O loader 또는 입력 relocation | 해당 `unittests/lift` 포맷 fixture | 해당 셀의 전 단계 로드/디컴파일 테스트 |
| Rewrite codegen 또는 출력 relocation | `RewriteCodegenRTTests` 사례 | `NeverDPatchFullTests` 및 가능한 링크된 patch fixture |
| patch가 쓰는 LLVM IR 변환 | 집중 변환 바이너리 | `NeverDPatchFullTests` 조합 pass grid |
| C API 또는 CLI | 직접 SDK/query 테스트 및 `unittests/semantic/CLIEndToEndTests.cpp` | 관련 pipeline/포맷 스위트 |
| EVM loader, opcode, IR 또는 backend | 가장 작은 소유 `NeverDEVM*Tests` target | 모든 EVM target과 생성 C/Solidity 컴파일 검사 |
| SBF loader, ISA, IR 또는 backend | 가장 작은 소유 `NeverDSBF*Tests` target | 모든 SBF target과 생성 C/Rust 컴파일 검사 |
| Libc 인식 | `NeverDLibCTests` | 동작 변경 시 의미론 call/ABI 사례 |
| 힙 수명 감사 또는 복사 오버플로 헌트 | `NeverDSafetyTests` | `NeverDSafetyIntegrationTests`의 전체 6셀 |
| 프로세스 실행 또는 quoting | `NeverDTestProcessTests` | 지원 host마다 영향받는 CLI/의미론 사례 하나 |

테스트는 가장 낮은 안정 경계에서 계약을 표현해야 합니다. LowIR 모양 테스트는 lifter
귀속에 유용합니다. 그럴듯한 두 IR 모양이 다르게 동작할 수 있다면 의미론 왕복이
필요합니다. 작은 opcode, CFG, 관측 상태 assertion으로 충분할 때 함수 전체 golden
dump를 피하세요.

## CI 관계

CI는 Linux, macOS, Windows에서 테스트를 켠 Release를 빌드하고 발견 inventory를
audit한 다음 플랫폼별 label 제외를 적용합니다. 프로필은
`.github/workflows/ci.yml`과 `scripts/audit_ci_test_inventory.py`에 있습니다.
`NeverDSafetyTests`와 `NeverDSafetyIntegrationTests`는 모든 matrix 호스트에서
필수이며, 각 실행은 체크인된 동일한 PE, ELF, Mach-O × x86-64, AArch64 fixture를 읽습니다. 비싼 모든 스위트를 대표하는 단일 matrix shard는 없으므로 필요한 교차 도구를 갖춘 머신에서는 로컬 `check-neverd`가 가장 명확한 전체 병합 전 신호입니다.

## 현재 Solana SBF conformance 및 sanitizer profile

이 current list는 위의 짧은 SBF list를 대체합니다. source differential suite는 clang
외에 `rustc`가 필요하며 compiler skip은 coverage 누락입니다. 전체 aggregate에는
`NeverDSBFProgramImageTests`, `NeverDSBFMalformedCorpusTests`,
`NeverDSBFISAConformanceTests`, `NeverDSBFUpstreamConformanceTests`,
`NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`와 metadata,
loader, analyzer, semantic, emitter, integration target이 포함됩니다. integrated
profile은 변동하는 총계 대신 named target과 결과를 기록합니다.

sanitizer profile은 `build-sbf-asan-ubsan`에 별도로 build합니다. revision이 고정된
prebuilt package에 필요한 fork-only header가 포함되므로 integration도 같은 fail-fast
ASan/UBSan profile에서 실행합니다.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

### pinned SBF evidence snapshot (2026-08-24)

gate는 Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679`, Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`를 고정합니다. official ELF manifest는
23/23을 통과하고, `NeverDSBFExternalOracleTests`는 1,411 opcode/boundary case를
`SBFOfficialOracleProtocol.def`, `SBFOfficialVerifierCases.def`,
`SBFOfficialExecutionConstants.def`를 통해 대조합니다.
`SBFOfficialELFMutations.def`가 malformed ELF의 table-driven contract이며 변동하는
총계는 고정하지 않습니다.
별도 축인 `41-case strict ELF differential`은 strict-v3 matrix 전체를 official
`verify-elf-batch`와 NeverD에 통과시킵니다. 이 41 case는 1,411 total에 포함하지 않습니다.
`NeverDSBFAgaveConformanceTests`는 Firedancer test-vectors의
`68bb4af40235562e8852fa23d5727e49c2a0b862`를 인증하고 loader fixture 1,955 `sol_compat_elf_loader_v1`개
(accept 1,399, reject 556)를 대조하고, 승인된 각 ELF에 대해 `entry_pc`, `text_off`,
`text_cnt`, `rodata_hash`, `calldests_hash`를 비교합니다. 이 gate는 후속 instruction verifier를 실행하지 않습니다.

추가 official execution matrix는 별도입니다. active `(Version,Opcode)` case 정확히
508개와 boundary case 58개를 합쳐 exact execution case 566개입니다. 1,411개의
verifier probe나 `41-case strict ELF differential`을 대체하지 않으며 그 합계에도 넣지 않습니다.
Linux Release CI는 `--print-pinned-revision`, `--print-test-vectors-revision`,
`--print-toolchain`을 사용하고 `NEVERD_SBPF_ORACLE`과
`NEVERD_AGAVE_CONFORMANCE_ROOT`를 export하므로 두 external gate가 필수입니다. 명시
oracle/corpus env가 없는 local run은 case를 discover하지만 skip할 수 있습니다.

`SBF_RUNTIME_VERSION`에 따라 `RuntimeVersionPolicy::ChainProfile`은 historical
cluster/slot을 반영하며 official feature account activation에 맞춰 maximum ISA를
V0→V1→V2→V3으로 전진시킵니다. 현재는 V3입니다. 명시 v4는 offline 분석용
`RuntimeVersionPolicy::UpstreamToolchain`을 사용합니다. 현재 10 MiB
상한은 정확히 `10'485'760` byte이고, 65,536은 historical provenance/test뿐입니다.
`SBFFaultCodes.def`는 execution fault의 안정된 값을, `SBFSourceStatuses.def`는 별도
계층인 generated-source ABI를 소유합니다.

10,000 scale fixture가 worklist, function ownership, multi-latch를 보호하며 machine별
시간은 고정하지 않습니다. cluster/account/slot row는 일반 test를 deterministic 및
offline으로 유지하면서 `RPC activation audit`를 가능하게 합니다.

## Android 클래스 목록 성능

`neverd mobile INPUT --list-classes`를 측정하기 전에 `NeverDMobileTests`를 Release로 빌드하고 해당 라벨을 실행합니다.
판독기 테스트는 희소 메타데이터, Unicode, 잘못된 참조, 지원하지 않는 메서드 본문, 체크섬 및 예산을 검증하며,
아카이브 테스트는 전체 추출과 선택 페이로드 쿼리를 구분합니다. CLI 테스트는 접두사 필터, JSON 범위, 기존 출력 보존,
multidex 실패 원자성을 검사합니다.

독립적인 fixture/측정 하네스는 시간 샘플을 받아들이기 전에 각 프로세스의 전체 설명자 목록을 검증합니다.

```sh
python3 -m unittest scripts.tests.test_benchmark_mobile_inventory -v
python3 scripts/benchmark_mobile_inventory.py \
  --output-dir /tmp/neverd-inventory-benchmark \
  --class-count 6000 --dex-count 3 --code-units 64 \
  --extra-string-bytes 8388608 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

실행마다 새 출력 디렉터리를 사용합니다. `--generate-only`는 fixture와 manifest를 쓰고 시간을 측정하지 않습니다.
`--workload`는 선택적 `--peer-command 'tool {input} {prefix}'`에 사용할 공통 입력 종류를 고르며, 입력 준비는 측정 명령 밖에서 수행합니다.
보고서에는 해시, 명령, 모든 새 프로세스 샘플, 따뜻한 캐시 가정, GNU time이 있는 Linux에서는 최대 자식 프로세스 RSS를 보관합니다.
이 RSS는 여러 프로세스를 사용하는 도구의 합산 최고값이 아닙니다. 합성 APK는 쿼리 컨테이너이며 설치 가능한 앱이 아닙니다.
목록 속도는 참조 검색 속도나 Java 복원 품질을 증명하지 않습니다.

하이브리드 CPU에서는 하네스와 그 자식 프로세스를 같은 허용 CPU에 고정하여 성능 코어와 효율 코어가 섞이지 않도록 합니다
(Linux의 `taskset -c 4 python3 ...` 등). 보고서는 상속된 CPU 선호도를 기록합니다.

## Android 코드 참조 성능

참조 쿼리는 복원 판독기의 명령 경계와 코드 검증을 공유합니다. 이 경계를 변경하면 모바일 테스트 모음을 실행하세요.
판독기 테스트는 피연산자 풀 종류, 일치 모드, 메서드 소유 관계와 공유 코드, 참조처럼 보이는 페이로드/즉시값,
잘못된 입력 및 리소스 제한을 검증합니다. 공유 디버그 스트림은 각 소유 본문의 프레임, 범위 및 매개변수에 맞춰 검사합니다.
저장 공간 제한 테스트에 큰 멤버 목록과 분기가 많은 본문을 모두 유지하세요. 지속 인덱스와 임시 컨테이너 확장은 수명이 다릅니다.
순서가 바뀌거나 겹치는 항목, 같은 너비라도 호환되지 않는 프로토타입의 공유 코드, 정렬되지 않은 입력 저장 공간,
검색 블록 경계를 넘는 부분 문자열 일치도 검사해야 합니다. 비공개 디코더 데이터의 성능 변경은 전체 소유 복원 모델과
참조 발생 다중집합을 보존해야 하며, 지원하지 않는 복원 메타데이터의 실패 동작도 유지해야 합니다.
독립적으로 생성한 기대값과 실제 입력에서의 도구 간 일치를 구분하세요.

독립 참조 하네스는 명령을 생성하면서 예상 발생 위치를 기록합니다. 측정할 때마다 전체 메서드 식별 정보,
code unit 단위 PC, opcode, 대상 식별 정보, UTF-16 단위와 중복 횟수를 검사합니다.
독립적으로 예상한 NeverD의 검사 범위 집계 수와 `code_scan_complete`도 확인합니다.

```sh
NEVERD_REFERENCE_TEST_BINARY="$PWD/build-release/bin/neverd" \
  python3 -m unittest scripts.tests.test_benchmark_mobile_references -v
python3 scripts/benchmark_mobile_references.py \
  --output-dir /tmp/neverd-reference-benchmark \
  --class-count 500 --methods-per-class 64 --matching-methods 2 \
  --dex-count 3 --resource-bytes 16777216 \
  --repetitions 7 --neverd build-release/bin/neverd
```

`--kind`와 `--workload`로 사례를 선택합니다. `--extra-strings 65536`은 실제 32비트 문자열 인덱스를 검사합니다.
페이로드 미끼는 기본적으로 활성화됩니다. `--no-payload-lookalikes`는 같은 배치와 실제 참조를 유지하면서 미끼 페이로드 값을
바꾸어 공통 입력 비교에 사용합니다. 정확성과 측정 결과를 모두 보관하세요. 가짜 페이로드 참조를 반환하거나 실제 참조를 빠뜨리는
쿼리는 검증에 실패하며 측정값을 받아들이지 않습니다.

선택적 `--peer-command`는 `{input}`, `{kind}`, `{query}`가 포함된 argv 템플릿을 받습니다.
다른 도구의 의미가 다르면 쿼리 구문을 명시적으로 조정하고 전체 발생 다중집합을 비교하세요.
선언된 검증 범위를 유지하며 전체 코드 스캔을 수행했다고 주장하지 않습니다. 목록 벤치마크와 같은 새 디렉터리,
CPU 선호도, 새 프로세스, 따뜻한 캐시 및 RSS 조건이 적용됩니다. `NEVERD_REFERENCE_TEST_BINARY`가 빌드된 실행 파일을
가리키지 않으면 선택적 CLI 단위 테스트를 건너뛰므로 해당 생략을 보고하세요.

## 모바일 SDK 내보내기 증거

수동 `Mobile SDK Export Evidence` 워크플로는 고정된 Xcode SDK에서 `collect_mobile_ios_sdk_declarations.py --exports-only`를 실행합니다. iOS 기기 및 시뮬레이터 SDK의 Foundation, CoreFoundation, UIKit 링커 맵을 그대로 보존하고 대상, SDK 버전, SDK 설정 해시, 파일 크기, SHA-256을 기록합니다. 일반 선언 수집기도 이 맵을 보존합니다. 파일 누락, 빈 파일, 크기 제한 초과 또는 SDK 외부 파일은 수집 실패로 처리하며 완료된 증거는 유지합니다. 링커 맵은 심볼 내보내기 증거이며 호출 ABI나 메서드 복원 성공을 증명하지 않습니다.

## 모바일 Swift String ABI 증거

수동 워크플로 `Mobile Swift String ABI Evidence`는 Xcode 26.5로 arm64 iOS 기기와 시뮬레이터용 고정 Swift 동등성·순서 비교 프로브와 C `swiftcall` 프로브를 컴파일합니다. `collect_mobile_swift_string_abi.py`는 소스, LLVM IR, 어셈블리, 컴파일러 식별 정보, SDK 설정 및 `libswiftCore.tbd`를 해시와 함께 보관합니다. 두 언어 모두 정확한 비교 임포트가 인수 5개를 받아 `i1`을 반환해야 하며, C는 그 결과를 명시적으로 1바이트로 확장해야 합니다. 잘못된 대상이나 시그니처, 명령 실패 및 시간 초과는 부분 증거를 남기고 수집을 실패로 처리합니다. 이 증거는 런타임 선언을 등록하거나 메서드 복원을 입증하지 않습니다. SDK 없이 `python3 -m unittest scripts.tests.test_mobile_swift_string_abi`로 수집기를 테스트할 수 있습니다.

## 모듈식 MBA 단순화

`SymReadability.*`는 뺄셈과 보수 표기, 결합 연산 비용, 1비트 및 넓은 리터럴, 공유 트리 포화, 예산 제한 후보 선택, 표본 검증을 끈 상태의 3비트 완전 동치성을 검사합니다. `SymMBASample.*`는 모든 연산자, 결정적 대입, 사용하지 않는 넓은 입력에 대해 좁은 값과 임의 정밀도 검증을 AP 평가기와 비교합니다. 점수 버전이 다를 때 후보 품질을 비교하려면 두 출력을 같은 척도로 다시 세어야 합니다. SDK의 버전별 크기 카운터는 진단용입니다.

## ARM32 및 프레임 전달 테스트 행렬

```sh
cmake --build build-release --target NeverDSymbolicTests \
  NeverDSymSimplifyGuardTests NeverDLiftTests NeverDMBASourceTests \
  NeverDHighCStoreForwardingTests NeverDMetadataJSONTests --parallel 4
build-release/bin/NeverDSymbolicTests
build-release/bin/NeverDSymSimplifyGuardTests
build-release/bin/NeverDLiftTests \
  --gtest_filter='HighSymSimplify.*:HighFrameStoreForwarding.*:ELFARM32ModeTest.*'
build-release/bin/NeverDHighCStoreForwardingTests
build-release/bin/NeverDMBASourceTests
build-release/bin/NeverDMetadataJSONTests --gtest_filter='ELFARM32ModeCAPITest.*'
```

프레임 spill 행렬은 두 C 백엔드에서 x86-32(ELF/COFF/Mach-O), ARM32(ARM 및 Thumb ELF), AArch64(ELF/COFF/Mach-O)도 검사합니다. 반복되는 비공개 프레임 로드는 덧셈이나 뺄셈으로 줄어야 하고, 두 최적화 수준에서 바이트 쌍, 워드 경계 쌍, 결정적 난수 워드에 대해 올바르게 실행되어야 합니다. Clang AST 검사는 전체 spill 함수에 남은 MBA 연산을 찾되 유효한 주소식을 구분합니다. HighFrameStoreForwarding은 접근 폭, 로컬 변수 변경, 메모리 쓰기, 별칭, 겹침, 순서가 지정된 메모리, 잘못된 그래프, 확장 예산을 검사합니다. HighCStoreForwarding은 부동소수점 재해석을 포함한 네 아키텍처에서 캐시 값의 정의를 살아 있게 하고, SymSimplifyGuard는 로드 동일성과 순서, volatile/atomic 및 poison 경계를 검사합니다. ELFARM32ModeTest는 ARM/Thumb 선택, 주소 정규화, 혼합 메타데이터 및 모순 증거 거부를 검사합니다. ELFARM32ModeCAPITest는 명시적인 SDK 오류와 Thumb 재로드 뒤 디코더 복구를 확인하고, InstructionMode는 디코더, 코드 포인터, 분기, 코드 생성 경계를 검사합니다. 크로스 타깃 Clang이 없으면 건너뛰며, 해당 형식의 성공 증거로 보지 않습니다.

`NeverDHighControlFlowTests`의 `HighBoundPrivateFrameCopies.*`는 호출 ABI 바인딩 이후 외부로 노출되지 않는 전용 프레임 슬롯을 통한 복사를 검증합니다. 분기, 슬롯 재사용, 서로 다른 가드 조건에서의 사실 일치를 포함합니다. x64와 AArch64에서 생성한 C를 `-O0` 및 `-O2`로 실행하고, 정의되지 않은 동작을 트랩하면서 독립적인 산술 결과와 비교합니다. 프레임 주소 노출, 알 수 없는 호출 ABI, 누락되거나 일치하지 않는 프레임 별칭, 진입 인수 재할당, 겹치는 접근, 순서가 지정된 메모리 또는 원자적 메모리, 잘못된 문장, 순환, 예산 소진 시 원래 함수를 보존하는지 확인합니다. 일반 값 변환을 PHI 복사로 표시해서는 안 됩니다.

소스 투영은 이 정리 후에도 가변 인자 객체 목록을 다시 검증합니다. 빈 명령 주소 앵커는 허용하지만 숨겨진 효과나 제어 이동은 거부합니다. 동기화 정리는 동일하게 저장된 수신자의 단일 `int64_t` 또는 `uint64_t` 뷰를 허용하며, 축소·부동소수점 변환·주소 연산·재할당은 계속 거부합니다. Foundation 객체 집합과 정상 및 예외 잠금 해제 추적을 `-O0`와 `-O2`에서 모두 실행합니다.

## x64 네이티브 동기 예외

checked x64의 `DIV`/`IDIV`는 실제 프로세서 결과와 `#DE`를 사용합니다. KVM은 비공개 supervisor IDT/IST, WHP는 명시적인 예외 비트맵을 사용하며 원래 컨텍스트와 제공된 오류 코드를 전송 오류와 구분합니다. OS는 복구 가능한 이벤트를 소비한 뒤 계속 실행할 컨텍스트를 설치합니다. Windows 드라이버는 0으로 나누기와 몫 오버플로를 `STATUS_INTEGER_DIVIDE_BY_ZERO`로 변환하고 실제 SEH filter, `__finally`, 재시도를 실행합니다. `NeverDX64ExceptionTests`는 Unicorn 없이 빌드되며 `DriverWDMCPUException`은 원본 WDK 사례를 검증합니다. 사용할 수 없는 ARM64 호스트는 명시적으로 건너뜁니다.

## 단계적으로 보관하는 RAM 효과

`RAMTransaction`은 물리 실행 임대 아래에서 명령이 선언한 쓰기 범위의 물리적 합집합만 보관합니다. 결과 관찰자 호출 전에 원래 RAM을 복원하며 취소, 전송 오류, 관찰자 예외는 부분 RAM이나 레지스터를 공개하지 않습니다. CPU 예외는 RAM 복원 후에도 아키텍처 예외 상태를 유지합니다. ARM64 단일·쌍 저장도 같은 계층을 사용합니다. x64는 8/16/32/64비트 `XCHG`, `XADD`, `CMPXCHG`를 실행하며 LOCK 또는 암시적 잠금 형식에는 자연 정렬을 요구합니다. `NeverDRAMTransactionTests`는 호스트 CPU와의 결과 비교, 복원, 별칭, 권한을 검증하며 사용할 수 없는 플랫폼은 명시적으로 건너뜁니다. 장치와 병렬 SMP는 제외되며 CPU 스냅샷은 이미 확정된 RAM을 복원하지 않습니다.

## 전체 x87 상태

`NeverDEmulationArch`는 ISA, 페이지 테이블과 FP 상태 배치를 소유하며 네이티브 및 Unicorn 전송이 공유합니다. x64 컨텍스트는 x87 제어, 상태, TOP, 물리 태그, 연산 코드, 명령/데이터 포인터와 8개의 80비트 레지스터를 보존합니다. `FP0`–`FP7`은 `RegisterValue`를 사용하고 스칼라 접근은 잘림을 거부합니다. `FPTag`는 물리 비어 있지 않음 비트맵입니다. `NeverDX64FPTests`는 모든 TOP, 정확한 연산의 호스트 FXSAVE/FXRSTOR 비교와 복원을 검사합니다. checked x87 명령 또는 모든 반올림 의미를 입증하지 않으며 없는 네이티브 호스트는 명시적으로 건너뜁니다.

`driver-strict`는 일치하는 Linux x64 host의 KVM과 Windows x64 host의 WHP를 지원합니다. `auto`는 해당 native transport를, cross-ISA는 Unicorn을 선택합니다. 명시적 Unicorn과 기존 V1 API는 portable software profile을 유지합니다. native 실행은 진입 전에 canonical address와 instruction effect를 검증하고, hardware가 없으면 fallback 없이 실패합니다. 지원되지 않는 instruction/OS behavior는 명시적 오류입니다. Windows x64 네이티브 CI는 Unicorn을 비활성화하고 필수 검사 359개를 모두 통과합니다. CPU 검사 131개, 내장 이미지 26개·WDK 이미지 46개·시나리오 사례 40개를 기본 및 재배치 주소에서 실행한 드라이버 결과 224개, SEH 경계 검사 4개를 포함합니다 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). native ARM64 실기 증거는 아직 없으며, 임의 driver나 Android/Darwin 호환성을 의미하지 않습니다.

위 네이티브 검증은 선언된 드라이버 진입점과 공개된 시나리오를 다룹니다. 아래의 기능별 회귀 테스트와 C API／CLI／Python 검사는 Windows 실행 결과가 명시된 경우 외에는 근거 범위가 Linux로 제한됩니다. 네이티브 사례 모음의 통과가 모든 테스트 변형의 Windows 검증을 뜻하지는 않습니다.

`executionCapabilities(Contract, ISA, Backend)`로 선택한 백엔드의 기능을 조회합니다. `NativeLegacyX64`는 네이티브 x64 드라이버 실행을 나타내며, `NeverDNativeDriverTests`는 기존 드라이버 모음을 검증합니다. 이 테스트는 Unicorn을 비활성화한 빌드에서도 실행할 수 있습니다.

기존 CI 워크플로는 일반 테스트 프로필 전에 전체 에뮬레이션 테스트 디렉터리를 실행하고 검색 목록, JUnit 결과 및 CTest 로그를 `emulation-focused`에 저장합니다. 다른 모듈의 실패가 이 실행을 막지 않습니다. 사용할 수 없는 하드웨어와 선택적 드라이버 자료는 명시적으로 건너뛰며, 소프트웨어 실행이나 컴파일 성공은 네이티브 실행의 증거가 아닙니다.

Linux의 `NeverDUnicornDeadlineTests`는 pthread 스케줄링을 제어해 실제 타이머 스레드가 게스트 진입 전에 완료되도록 합니다. x64, ARM32 및 ARM64에서 진입 전 취소가 게스트에 영향을 주지 않고 다음 실행이 독립된 예산을 사용하는지 검증합니다. 공개 엔진 API를 사용하며 엔진 내부 상태는 변경하지 않습니다.

`NeverDX64ExceptionTests`의 `X64StateTransition`은 네이티브 CPU에서 독립적인 RAM 읽기와 CR8 읽기를 수행합니다. TLS 기준 주소와 권한 수준을 번갈아 변경하고, 반복되는 나눗셈 예외 후 재개하며, 취소된 진입 후 TLS를 변경합니다. 네이티브 상태 전송을 바꾸면 해당 CTest 레이블과 별칭 재매핑, CPU 컨텍스트, FP 상태, 원본 드라이버 결과 비교를 함께 실행해야 합니다. KVM/WHP를 사용할 수 없으면 명시적으로 건너뜁니다.


`NeverDKvmRunTests`는 `/dev/kvm` 없이 `KvmRunControl`의 빌린 전송 콜백을 검증합니다. `StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries`는 준비, 수집과 가로챈 호스트 진입이 같은 스레드를 사용하며 중단 후 재시도 중에도 준비를 한 번만 수행하는지 확인합니다. 다른 사례는 진입 없는 준비 실패, 수집 실패, 준비 중 정지와 활성 진입 취소를 검증한 뒤 새 실행이 이전 콜백을 재사용하지 않는지 확인합니다. 실제 취소, RAM 롤백, 예외와 기존 드라이버 테스트도 계속 포함해야 합니다. `SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers`는 같은 기한 내의 여러 진입이 작업 스레드를 재사용하고 각 전송을 한 번만 수행하며 이전 상태 패킷을 변경하지 않는지 검증합니다.

KVM x64/ARM64는 `KvmRunControl`을 통해 같은 전용 vCPU 작업 스레드에서 상태 준비, `KVM_RUN`, 상태 캡처를 수행합니다. `EINTR` 재시도에도 준비는 한 번이며 취소나 캡처 실패는 게시할 수 없습니다. `KvmAArch64Machine.cpp`의 주소 변환 유지와 전체 스칼라·벡터 전송도 하나의 단일 단계 기한을 공유합니다. 호출 스레드는 완료 확인 후 커밋하며 ISA 해석, RAM 트랜잭션, OS 정책과 관찰자를 담당합니다. 네이티브 ARM64 실기 증거는 아직 없습니다.

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops`는 호스트가 일반 레지스터, 첫 번째와 마지막 XMM 레지스터, MXCSR 및 x87 제어어를 변경한 뒤 연속 실행과 실제 CPU 저장을 검증합니다. 정지된 진입 이후의 실제 `FXSAVE64` 바이트로 모든 물리 80비트 레지스터, TOP, 태그, 연산 코드와 포인터를 확인하며 반복 나눗셈 예외도 재사용을 무효화합니다. 이 머신 경계 테스트는 checked 프로필에 추가 x87 명령을 허용하지 않습니다.

`NeverDKvmStateTransferTests`는 실제 KVM 실행 후 레지스터 또는 XSAVE 읽기 실패를 주입하고 변경되지 않은 입력으로 재시도합니다. 독립적인 정수와 패킹된 바이트 결과로 수집 실패 후 이미 진행된 네이티브 상태를 재사용하지 않는지 확인합니다. 이 실행 파일만 `ioctl`을 래핑하며 네이티브 호스트가 없으면 명시적으로 건너뜁니다.

Checked ARM64는 하나의 완전한 상태 커밋 경계를 사용합니다. `Registers.def`가 39개 스칼라 필드와 32개 128비트 벡터를 정의하며 `captureAArch64State`는 모든 읽기, 선언된 폭과 NZCV 정규화를 완료한 뒤 한 번에 게시합니다. Unicorn/KVM/WHP는 TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR, FPSR를 포함한 같은 상태를 전송합니다. 네이티브 어댑터는 CPACR_EL1로 FP/SIMD를 활성화합니다. 읽기 실패나 진입 취소 시 호출자의 전체 상태가 보존됩니다.

ARM64 KVM/WHP 초기화는 전용 `AArch64MachineProbe.def` 프로그램을 실행합니다. NOP, 양의 무한대 방향으로 반올림하는 FP32 덧셈, 두 레인의 SIMD 덧셈입니다. 각 단계에서 39개 스칼라 필드와 32개 벡터를 모두 비교하여 TLS, NZCV, 결과 상위 비트 초기화, FPCR/FPSR 보존 및 누적 상태를 확인합니다. 감독자 전용 모니터 메모리와 하나의 전체 마감 시간을 사용합니다. 성공은 이 제한된 초기화 프로그램만 검증하며 독립적인 native ARM64 워크로드 검증은 아직 필요합니다.

x64 KVM/WHP 네이티브 초기화는 비공개 supervisor 페이지에서 `X64MachineProbe.def`를 실행합니다. 하나의 기한 안에 NOP, 양의 무한대 방향으로 반올림하는 FP32 덧셈, 두 레인 SIMD 덧셈, FS/GS 로드와 CS/SS/CR8 읽기를 수행하며 각 단계에서 전체 스칼라, XMM, 물리 x87 및 제어 상태를 비교합니다. x64와 ARM64 검사는 물리 메모리의 독점 실행 임대를 요구합니다. `MemoryProjection`은 캐시 식별 정보(ISA, 주소 공간, 매핑 세대, 권한, 모니터 구성)와 ISA별 확정된 페이지 테이블 루트 이력을 소유합니다. 비공개 바이트를 다시 쓰기 전에 캐시를 무효화하므로 실패한 재구축의 부분 테이블이나 호출자의 오래된 루트를 재사용할 수 없습니다. 이 검사는 제한된 초기화만 증명하며, ARM64의 독립적인 네이티브 작업 검증은 아직 남아 있습니다.

공유 XSAVE 디코더는 표준 형식과 압축 형식의 SSE 초기 상태를 구분합니다. XSTATE_BV[1]이 0이면 두 형식 모두 XMM을 초기화하지만 표준 형식은 MXCSR을 읽고 검증하며 압축 형식은 MXCSR을 초기화합니다. `X64XsaveCases.def`는 독립적인 데이터 배치와 직접 작성한 호스트 XRSTOR 프로그램을 제공합니다. `X64XsaveTests.cpp`는 거부 시 상태의 원자성을 확인하고 호출자의 FP/SSE 상태를 보존하면서 두 형식을 실제 호스트 실행과 비교합니다. 호스트 아키텍처나 필요한 명령 기능을 사용할 수 없으면 명시적으로 건너뜁니다.

`X64FPState.def`는 압축 AVX, AVX-512, CET_U/CET_S, AMX 전송 배치와 구성 요소의 64바이트 정렬을 선언합니다. 존재하는 확장 데이터는 모두 0인 초기 상태여야 하며, 없는 구성 요소의 데이터와 정렬 패딩은 상태를 정의하지 않습니다. 배치 비트가 오프셋을 결정하고 알 수 없는 배치, 초기 상태가 아닌 데이터, 잘못된 길이는 공개 전에 실패합니다. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState`, `InitialWideComponentsDoNotHideFPState`가 872바이트와 10752바이트 WHP 패킷을 검증합니다. 이 전송 지원은 해당 확장 명령 실행을 허용하지 않습니다.

`WhpXsaveRegisters.def`는 이름이 있는 x87/SSE 제어 레지스터로 완전한 XSAVE 패킷을 보완합니다. 마지막 연산 코드와 명령/데이터 포인터를 명시적으로 쓰고 호스트에서 읽습니다. 패킷의 0 필드는 보완할 수 있지만, 0이 아닌 메타데이터 충돌이나 공통 제어값 불일치는 상태 공개 전에 실패합니다. `NamedMetadataRestoresOmittedPacketFields`는 전체 FP 데이터를 유지하면서 누락 필드를 검증합니다.

네이티브 `FOP/FIP/FDP`는 호스트 x87 저장·복원 규칙을 따릅니다. 마스크되지 않은 대기 예외가 없으면 AMD는 이 필드를 0으로 만들 수 있으며 스냅샷은 관측값을 유지합니다. `X64MachineProbe.def`와 정밀 NOP/컨텍스트 테스트는 일관된 대기 예외를 설정하여 모든 필드를 유효한 상태에서 차이를 숨기지 않고 비교합니다. 호스트 프로세스 FXRSTOR64/FXSAVE64 참조는 두 상태를 검사하며, 백엔드는 호스트 결과를 입력 메타데이터로 대체하지 않습니다.

`NativeGuestRAMDistinguishesEntryFromCaptureLoss`는 실제 게스트 FXSAVE64가 RAM에 저장한 전체 FP/SSE 상태를 호스트 XSAVE 결과와 비교합니다. 두 API 모두 직접 설치와 게스트 내부 FXRSTOR64를 기본 포인터 저장 기능 및 명시적으로 선택한 호스트 지원 설정에서 검사합니다. 진입, 게스트 실행, 캡처 경계를 구분하며 값을 보정하지 않고 불일치를 실패로 유지합니다. 경계 행렬은 대기 중인 마스크되지 않은 x87 예외도 검사하며, 호스트 프로세스의 FXRSTOR64/FXSAVE64 참조 결과와 프로세서 공급업체를 기록하여 조건부 포인터 저장 의미와 WHP 전송 동작을 구분합니다.

공통 `encodeX64XsaveState` / `decodeX64XsaveState` 코덱은 표준·압축 FP/SSE 패킷, 물리 TOP 순환, 누락된 구성 요소의 초기 상태 및 원자적 검증을 소유합니다. WHP는 완전한 XSAVE API를 사용하며 `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState`를 우선하고 이전 XSAVE API를 호환 경로로 사용합니다. 이전 개별 x87 레지스터 인터페이스는 완전한 패킷을 대체할 수 없습니다. 초기 상태가 아닌 확장 구성 요소, 잘못된 헤더·제어 값 및 잘린 캡처는 명시적으로 실패합니다. WHP 매핑 실패는 진단을 위해 HRESULT, GPA 및 크기를 보존합니다.

`CheckedX64Instructions.def`는 기존 CPU 백엔드에서 8/16/32/64비트 부호 없는 `MUL`과 `CBW/CWDE/CDQE/CWD/CDQ/CQO`를 허용합니다. `NeverDX64IntegerTests`는 독립적인 `X64IntegerCases.def` 인코딩과 예상값을 사용하여 두 권한 수준에서 부분 레지스터 보존, 32비트 제로 확장, 곱의 상위·하위 결과, 정의된 CF/OF 및 부호 확장 시 플래그 보존을 검증합니다. 일반 RAM 곱셈은 전체 접근 범위의 권한 검사와 읽기 관찰 콜백을 유지하며, 오류나 관찰 콜백의 중지는 암시적 출력 레지스터와 PC를 보존합니다. 장치 피연산자는 지원하지 않습니다. checked Unicorn에서도 실행하며 사용할 수 없는 네이티브 백엔드는 명시적으로 건너뜁니다.

`X64BitInstructions.def`는 16/32/64비트 레지스터 및 일반 RAM의 `BT/BTS/BTR/BTC`를 허용합니다. 레지스터 비트 인덱스는 피연산자 너비의 부호 있는 값으로 전체 워드를 선택하며, 즉시값은 기준 워드 안에 머뭅니다. 주소 너비에 따른 절단은 FS/GS 기준 주소를 더하기 전에 적용됩니다. 프로세서가 CF와 쓰기 값을 제공하고, `RAMTransaction`은 관찰 콜백이 수락할 때까지 결과를 비공개로 유지합니다. 전체 범위 권한 검사는 독립 페이지 할당과 별칭을 포함하며, 중지·콜백 실패·페이지 접근 거부 시 원래 CPU와 RAM을 보존합니다. LOCK은 자연 정렬된 메모리 수정 형식만 허용하며 MMIO와 하드웨어 병렬 SMP는 지원하지 않습니다. `X64BitStringTests.cpp`는 독립 인코딩을 실제 x64 호스트 실행과 비교하고 음수 인덱스, 너비 절단, 페이지 경계 접근, 취소, 잘못된 LOCK 형식을 검사합니다. [Intel 명령어 참조](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)를 참고하세요.

`X64StringInstructions.def`는 일반 RAM의 8/16/32/64비트 `MOVS/STOS/LODS`를 관리하며, `CLD/STD`는 다른 플래그를 보존하면서 방향만 바꿉니다. REP의 각 요소는 관찰 전에 전체 피연산자를 검사하고 재개 경계에서 확정됩니다. 후속 오류가 발생해도 완료한 요소는 유지되며, 중지나 콜백 예외는 현재 요소를 변경하지 않습니다. FS/GS는 주소 폭을 자른 뒤 소스에만 더합니다. AL/AX 로드는 상위 비트를 보존하고 EAX 로드는 0으로 확장합니다. 32비트 주소 모드에서 반복 횟수가 0이면 카운터 상위 비트가 0이어야 하며 MOVS/STOS의 참여 주소 레지스터도 동일합니다. 그렇지 않으면 실제 CPU 구현마다 결과가 다릅니다. MOVS/STOS/LODS의 REPNE 형식과 STOS/LODS 장치 피연산자는 아직 지원하지 않습니다. `X64StringTransferTests.cpp`는 독립적인 호스트 명령으로 폭, 방향, 중첩, 0회 반복을 비교하고 권한, 별칭, 주소 순환, 오류와 재개도 검사합니다. 자체 WDK 리소스 드라이버는 `driver_resource_strings.def`로 STOS/LODS의 네 폭을 모두 실행합니다.

`X64StringInstructions.def`는 일반 RAM의 8/16/32/64비트 `CMPS/SCAS`와 `REPE/REPNE`도 관리합니다. 각 요소는 관찰 전에 전체 읽기 범위를 검사하고 여섯 산술 플래그를 갱신하며 첫 종료 조건에서 멈춥니다. 데이터 오류는 이번 연속 REP 시작 시점의 플래그를 복원하면서 완료된 포인터와 카운터 변경을 유지합니다. 공개 API로 재개하면 게시된 CPU 상태에서 다시 시작합니다. 중지와 관찰 예외는 현재 요소를 변경하지 않으며 조기 종료 후 다음 요소를 읽지 않습니다. FS/GS는 CMPS 소스에만 적용되고 SCAS는 누산기와 사용하지 않는 소스 레지스터를 유지합니다. 장치 피연산자와 모호한 32비트 0회 반복 상위 비트는 제외됩니다. `X64StringComparisonTests.cpp`는 독립 호스트 명령으로 플래그, 방향, 별칭, 주소 순환, 권한과 복구를 비교하고 Linux x64 신호로 실제 오류 시점의 레지스터를 검사합니다. 자체 WDK 리소스 드라이버는 `driver_resource_strings.def`로 네 폭의 두 조건 반복 형식을 실행합니다. [Intel 명령 참조](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)를 참고하세요.

`WhpResourceCache.h`는 논리 CPU 상태와 WHP 파티션을 분리합니다. 런타임은 활성 네이티브 파티션 하나를 유지하며 같은 CPU의 연속 단계에서 재사용합니다. CPU를 전환할 때 이전 파티션을 먼저 제거한 다음 매핑과 가상 프로세서를 다시 만들고 전체 상태를 복원합니다. 논리 CPU는 독립적인 `MemoryProjection` 뷰와 권위 있는 RAM을 유지합니다. 임대 획득은 취소와 현재 기한을 따르며 비활성 CPU를 제거해도 다른 CPU의 파티션은 제거되지 않습니다. x64는 호스트의 기본 XSAVE 기능 조합을 보존하고 `WHvGetPartitionProperty`로 실제 파티션을 검증하며 종속 기능을 지워 마스크를 축소하지 않습니다. 협력적 CPU 전환은 병렬 하드웨어 SMP를 제공하지 않습니다.

`NeverDX64FPTests`는 전체 79개 시작 상태 손상 위치와 독립적으로 어셈블한 `X64ProbeCases.def`의 네이티브 실행, 단일 기한 및 게스트 RAM 보존을 검사합니다. `NeverDProjectionCacheTests`는 호출자 변경, ISA 순서, 루트 이력, 권한/모니터 구성, 매핑 세대, 주소 공간 식별 및 재구축 실패를 검사합니다. `NeverDRunControlTests`의 `WhpXsaveTests.cpp`는 새 API와 이전 API 패킷, 모든 TOP, 크기 범위 및 실패 시 상태 보존을 검사합니다. 이 메모리 프로토콜 테스트는 네이티브 WHP 증거를 대체하지 않으며 사용 불가능한 네이티브 전송은 명시적으로 건너뜁니다.

XSAVE 검증 진단은 크기 조회, 로컬 데이터 준비, 캡처 데이터 디코딩을 구분하고 API 이름, 반환 바이트 수, 용량, 제한된 헤더·제어 필드를 보존합니다. 독립적인 예상값은 `WhpHostFailureCases.def`에 있으며 게스트 레지스터 데이터는 출력하지 않습니다. `InvalidInputReportsPreparationWithoutHostMutation`은 잘못된 입력이 호스트를 호출하거나 호스트 데이터를 바꾸지 않는지도 검증합니다. 공유 ISA 코덱이 검증을 전담합니다.

WHP의 기능 조회, 파티션/가상 CPU 초기화, 레지스터/XSAVE 전송 및 실행 중 발생한 호스트 API 오류는 HRESULT와 `WhpProtocol.def`에 선언된 API 이름을 보존합니다. 기능 조회 실패는 형식이 지정된 사용 불가 결과를 유지합니다. `WhpHostFailureCases.def`는 취소와 동시에 발생한 호스트 오류 및 최신/레거시 XSAVE 조회·설치·캡처 실패에 대한 독립적인 예상 결과를 제공합니다. Windows 전용 CI는 네이티브 164건 통과를 요구합니다. 매핑 16건, 시작 2건, FP/컨텍스트 10건, 공유 CPU 7건, 정수 8건과 `NativeInstallRetainsFPStateBeforeAnyGuestExecution`의 두 API 변형입니다. 마지막 두 테스트는 게스트 실행 전에 전체 FP/SSE와 독립적으로 읽은 메타데이터를 비교합니다. 등록 누락, 건너뛰기, 비활성화 또는 미실행은 네이티브 증거 감사 실패로 처리됩니다. 추가 26개 검사는 두 권한 수준의 모든 `X64BitStringTests.cpp` 사례를 포함합니다. Windows PE64는 WHP 프로세스 사례 32개와 Windows에서 직접 실행하는 비교 검사 1개를 추가합니다.

`NeverDMemoryLifecycleTests`는 Unicorn과 독립적으로 빌드되며 네이티브 전용 구성에도 등록됩니다. Unicorn을 끄면 전용 소프트웨어 투영/장치 사례는 명시적으로 건너뛰지만, 호스트에 맞는 공유 CPU 사례는 유지됩니다. `WhpMemoryTests.cpp`는 `WhpMemoryCases.def`의 16개 사례로 네이티브 메모리 API를 분리합니다. 페이지/투영 크기, 공유/독립 할당, 미접근/상주 바이트, 첫 가상 프로세서의 존재 여부를 확인합니다. 각 사례는 두 논리 소유자를 유지하면서 매핑된 파티션을 반복 전환하고, 비활성 소유자를 제거한 뒤 남은 매핑을 다시 만들지 않고 사용할 수 있는지 확인합니다. 실제 매핑 오류는 HRESULT를 보존하며 테스트를 실패시킵니다. 이는 메모리 API 증거이며 명령 실행 증명은 아닙니다.

`X64MachineProbe.def`의 시작 진단은 실패한 명령과 불일치하는 모든 스칼라, TLS, 권한, x87 제어 필드, 물리 FP 레인 및 XMM 워드를 나열하고 예상값과 관측값을 보존합니다. `DiagnosticIdentifiesStepFieldAndBothValues`는 독립적인 예상 메시지로 검증합니다. 상태 비교는 완전한 일치를 계속 요구하며, 진단은 전송 손실과 명령 실행 문제를 구분합니다. 실패한 네이티브 프로브는 통과로 처리하지 않습니다.

`WhpResourceTests.cpp`는 캐시 재사용, 교체 전 제거, 실패 복구, 기한과 중지의 경합을 검증합니다. `LogicalCPUSwitchingRestoresPhysicalFPAndTLS`는 두 권한 모드에서 두 머신을 번갈아 실행하고 독립적인 물리 x87/XMM 및 FS/GS 상태를 확인한 뒤 한 머신을 제거하고 나머지를 재개합니다. Windows CI는 두 권한 모드의 WHP 사례를 모두 요구합니다.

`NEVERD_ENABLE_SEMANTIC_TESTS`의 기본값은 `ON`이며 `unittests/semantic`의 테스트 그룹과 통합 실행 대상을 제어합니다. Unicorn 없이 네이티브 CPU 테스트를 빌드하려면 `BUILD_TESTING=ON`을 유지하고 `NEVERD_ENABLE_SEMANTIC_TESTS=OFF`와 `NEVERD_EMULATION_BACKEND_UNICORN=OFF`를 설정합니다. 적절한 SDK 헤더가 있는 Windows ARM64/MSVC를 포함하여 네이티브 KVM/WHP 테스트를 계속 빌드할 수 있습니다. Windows ARM64에서 Unicorn을 활성화하려면 ARM64 LLVM-MinGW 도구 모음이 필요합니다. 이 빌드 분리는 ARM64 네이티브 실행 검증을 의미하지 않습니다.

네이티브 CPU 전용 CI는 고정 버전의 Capstone 소스를 초기화하고 검증된 LLVM 패키지를 사용합니다. `NEVERD_ENABLE_SEMANTIC_TESTS=OFF`와 Unicorn 어댑터 비활성화를 함께 설정하면 CPU 테스트의 구성, 빌드 및 링크에 Unicorn 소스가 필요하지 않습니다. 서명 데이터와 외부 코퍼스도 필요하지 않습니다. 기본 CI에서는 전체 의미론 테스트 그룹을 계속 활성화합니다.

기존 `ci.yml`은 Windows x64 runner에서 명시적으로 선택하는 수동 모드 `native_cpu_only`를 제공합니다. `NativeCPUTests.def`가 열 테스트 소유자를 선택하고, `run_native_cpu_ci.py`가 먼저 빌드한 후 필터링된 CTest를 실행하여 목록, JUnit, 로그, 요약을 저장합니다. 공통 CI 파서는 통과, 실패, 건너뜀, 비활성화, 미실행을 구분합니다. 선언된 WHP 네이티브 매핑 사례는 모두 발견되고 실행되어야 하며, 네이티브 증거가 없거나 건너뛰면 이 작업은 실패합니다. 기본 LLVM 소스 빌드 CI는 그대로 유지됩니다. 프로토콜 테스트와 컴파일은 WHP 또는 ARM64 네이티브 워크로드 검증을 대신하지 않습니다.

`native_cpu_only=true`와 `native_driver_tests=true`를 지정하면 Unicorn 없이 `NeverDNativeDriverTests`를 활성화합니다. 구성 전에 `build_wdk_driver_fixtures.py`가 공식 Microsoft WDK/SDK 10.0.26100.6584 패키지 전체의 SHA-256을 검증하고 원본 소스에서 일반/CFG/DBG 드라이버 이미지 46개를 다시 빌드합니다. `WDKDriverFixtures.def`는 패키지 식별자, 컴파일러·링커 인수와 픽스처 연결을 선언합니다. 수정하지 않은 Microsoft 파일과 라이선스는 로컬 빌드/캐시 디렉터리에 보관하며 CI는 빌드 메타데이터와 로그만 업로드합니다. 매니페스트에는 도구 버전, 명령, 소스·헤더 해시와 출력 이미지 해시를 기록합니다.

`NativeDriverTests.def`는 `DriverBuiltinImages.def`와 `DriverBackendParityCases.def`의 전체 112개 워크로드에 대해 원래 주소와 재배치 주소에서 WHP 결과 224개를 요구합니다. 내장 이미지 26개, WDK 이미지 46개, 요청 시나리오 40개이며 CPU 검사 164개와 순수 SEH 후속 실행 회귀 검사 4개를 포함하면 필수 결과는 392개입니다. 고정 이미지의 재배치는 기존의 예상된 거부 결과를 유지합니다. WDK 이미지나 시나리오가 없거나 건너뛰면 이 선택적 CI 작업은 실패합니다. 일반 로컬 빌드에서는 외부 픽스처가 계속 선택 사항입니다. `run_native_cpu_ci.py --with-drivers`는 구성된 테스트 타깃과 전체 목록/JUnit 증거를 기록합니다. 이미지 빌드만으로 Windows 또는 ARM64 네이티브 실행이 검증되지는 않습니다. 아래 명령으로 로컬에서 재현하거나 생성된 캐시를 기존 에뮬레이션 빌드에 적용할 수 있습니다. `164 CPU + 224 WHP + 4 SEH = 392`.

C SEH 범위는 끝 주소를 포함하지 않는 반개방 구간입니다. 유효한 `__C_specific_handler` 착지점이 보호 구간 안에 있을 수 있습니다. [LLVM 20.1.8](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/llvm/lib/CodeGen/AsmPrinter/WinException.cpp#L600-L608)은 구간 끝에 `EndLabel + 1`을 기록합니다. Windows OS 모델은 원래 경계를 유지하며 재배치 후에도 실행 가능 여부, 소유 함수, 후속 실행 주소의 일치를 각각 검증합니다. `KernelSEHContinuationCases.def`는 원본 픽스처 배치를 보존하고 `ScopeEndLabelMayOverlapTheHandlerLandingPad`는 상수 처리기와 필터를 검사합니다. 관련 테스트는 끝 주소 제외와 잘못된 대상 거부 후 디스패치 상태를 소비하지 않고 재시도할 수 있음을 확인합니다. 이 순수 모델 검사는 Unicorn을 비활성화한 `NeverDNativeDriverTests`에서도 실행됩니다.

대상까지 스택을 해제할 때도 원래 범위 끝을 사용합니다. 처리기 대상이 `finally` 보호 구간 안에 남아 있으면 해당 범위를 벗어나지 않습니다. `FinallyRespectsRawScopeEndAtHandlerTarget`는 경계 양쪽을 검사하고 Windows x64에서는 `ntdll.dll!__C_specific_handler`와 직접 비교합니다. NeverD는 컴파일러가 생성한 범위를 수정하지 않습니다. Clang 20/21로 빌드한 원본 픽스처는 편향된 끝 주소가 선택된 대상을 포함하므로 `T`, `J` 모드에서 게스트 실패를 반환합니다. Clang 23 빌드는 두 정리 루틴을 모두 실행합니다. [LLVM 변경 #144745](https://github.com/llvm/llvm-project/pull/144745)는 기존 `+1` 편향을 제거합니다. 이 컴파일러별 결과는 백엔드 오류와 구분합니다.

빌드 목록은 모든 자체 WDM/KMDF C 픽스처와 선택적 WDK CMake 경로를 포함합니다. 공개된 각 `driver-*-scenario.json`은 `DriverBackendParityCases.def`에 일반 및 CFG 사례가 있으며 소스, 빌드 또는 시나리오 연결이 빠지면 목록 테스트가 실패합니다. `Original`은 시나리오의 로드 주소 재정의를 지우고 이미지의 기본 베이스를 확인하며 `Rebased`는 선언된 재배치 베이스를 확인합니다. 드라이버 소유 IRP 시나리오는 자식 요청을 의도적으로 취소하므로 정상 정리 후에도 `DriverNativeOutcomes.def`는 예상된 전체 실패 결과를 유지합니다.

```bash
python3 scripts/build_wdk_driver_fixtures.py \
  --output build-driver-fixtures --cache build-driver-packages
cmake -S . -B build-native -G Ninja \
  -C build-driver-fixtures/fixtures.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
```

`NeverDAArch64StateTests`는 모든 스칼라 필드와 벡터의 두 워드 손상, 권한 변경, 부동소수점 미실행 및 전송 오류 진단 보존을 검사합니다. `NeverDAArch64FPTests`의 `OriginalProgramChecksCompleteStateAndOneDeadline`은 독립적으로 어셈블한 `AArch64ProbeCases.def` 명령을 실제 전송에서 두 권한으로 실행합니다. 테스트는 PC와 무관한 명령을 게스트 코드로 옮기며 모니터 페이지의 사용자 접근을 허용하지 않습니다. Unicorn 실행과 명시적 native 건너뛰기는 native ARM64 시작 증거를 대신하지 않습니다.

`CheckedAArch64Instructions.def`와 `AArch64InstructionEffects`는 EL0/EL1에서 제한된 기본 FP32/FP64 연산·비교·이동과 고정 폭 SIMD를 허용합니다. FPCR는 네 가지 반올림 모드, FZ, DN을 지원하고 FPSR는 누적 상태와 QC를 보존합니다. 미지원 제어·상태 비트는 변경 전에 거부합니다. FP16 연산, SVE/SME, 마스크되지 않은 예외, 선택적 확장과 목록 밖 형식은 명시적으로 실패합니다. Windows ARM64 드라이버 로딩이나 다른 OS 환경은 추가하지 않습니다.

`AArch64InstructionEffects`는 최대 128비트 피연산자의 스칼라·FP/SIMD 단일/쌍 RAM 범위를 소유합니다. 공유 주소 공간은 CPU 진입 전 모든 페이지를 검사하고 `RAMTransaction`은 선언된 전체 물리 쓰기만 커밋합니다. 128비트 쓰기는 실행 전에 두 개의 64비트 값으로 순서대로 관찰됩니다. 정지와 오류는 RAM, 벡터와 주소 갱신을 보존합니다. Xn/Vn 번호 중복은 유효하며 쌍 접근 범위의 주소 래핑은 거부됩니다. `NeverDAArch64MemoryTests`는 독립적인 `AArch64CrossPageCases.def`와 `AArch64VectorMemoryCases.def`를 사용합니다.

`NeverDAArch64StateTests`는 두 권한에서 전체 상태의 71개 읽기 위치, 폭 정규화, 누락된 리더와 재시도를 검사합니다. `NeverDAArch64FPTests`는 `AArch64FPCases.def` 원본 명령으로 모든 벡터 레인, 패킹 연산, 스칼라·벡터 FP, 네 반올림 모드, FZ/DN, 누적 FPSR, 컨텍스트와 거부 조건을 검증합니다. `NeverDAArch64MemoryTests`는 모든 페이지 교차 위치, 관찰 순서와 정지, 거부된 페이지, 별칭 및 복원된 벡터 입력을 검사합니다. `NeverDUnicornStateTransferTests` (`UnicornStateTransferCases.def`, `CapturesDeclaredWidthsWithoutStaleUpperBits`)는 실제 실행 후 모든 스칼라·벡터 읽기 실패를 주입합니다. 사용할 수 없는 네이티브 경로는 명시적으로 건너뛰며 네이티브 ARM64 KVM/WHP 실기 증거를 대신하지 않습니다.

`NeverDAArch64MemoryTests`는 두 권한 수준에서 Unicorn, KVM, WHP의 스칼라/쌍 명령 18종을 확인합니다. 모든 페이지 경계 오프셋, 부호와 폭, 관찰자 순서, 두 번째 페이지의 권한 거부/누락, 명시적인 오류 소비와 재시도, 반복된 물리 별칭, 별칭 교체 후 컨텍스트 복원을 검사합니다. 변경 전에 유효한 페이지 경계 로드가 거부되는 현상을 재현했습니다. 사용할 수 없는 전송은 명시적으로 건너뜁니다. Unicorn 및 교차 컴파일은 ARM64 KVM/WHP 실기 증거를 대신하지 않습니다.

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`)는 플래그가 0인 비활성 CFG 메타데이터, 두 로드 주소에서 유지되는 대체 포인터, 잘못된 슬롯/대상 및 누락된 재배치를 검사합니다. 실행 사례는 명시적인 Unicorn/KVM/WHP와 `driver-strict`, `checked-x64-v1`을 사용하며 사용할 수 없는 백엔드는 개별적으로 건너뜁니다. `DriverPublicCLICases.def`는 호환성을 유지하는 v1 C API와 비교하는 CLI에 `--backend unicorn`을 지정합니다. 네이티브 및 `auto` 선택은 별도의 공개 인터페이스 검증을 유지하며 호스트 API를 사용할 수 없을 때 자동으로 대체하지 않습니다.

checked Unicorn은 `MachineRunControl`을 사용하며 ARM64 유지보수, 게스트 실행과 전체 상태 읽기를 한 단계 시간 한도에서 처리합니다. `UC_HOOK_CODE`는 명령 진입에서 빌린 정지 토큰과 기한을 확인합니다. 동기 엔진 호출은 반환 전에 hook 참조를 해제하지만 기계 단계는 상태 게시까지 제어를 유지합니다. Unicorn과 WHP는 전체 CPU 상태를 임시 저장하고 성공한 단계의 게시 직전에 같은 제어 조건을 확인합니다. WHP는 준비 전에 시간 한도를 한 번만 만듭니다. 확인된 x64 CPU 예외는 상태 읽기 중 도착한 정지 요청보다 우선합니다. 읽기가 취소되면 checked RAM 트랜잭션은 추측 쓰기를 버리며 비제한 소프트웨어 계약은 그대로입니다. `MachineInterruptedError`는 확인된 취소와 호스트 또는 상태 읽기 실패를 구분합니다. 공통 checked CPU는 `Stopped` 또는 `Deadline`을 반환하고 CPU/RAM을 유지하며 재시도를 허용합니다. 동시 정지 요청이 있어도 실제 실패는 `BackendFailure`로 남습니다.

상태 읽기 회귀 테스트: `NeverDUnicornStateTransferTests`, `NeverDUnicornMachineControlTests`: `StopDuringCaptureCannotPublishAndAllowsRetry`, `ExpiredCaptureCannotPublishAndAllowsRetry`, `CompletedStoreCannotPublishCancelledCapture`, `StopDuringCaptureCannotHideRealGuestException`. `UnicornPublicCapture.CancellationKeepsTypedExitStateAndRAMConsistent`; `NeverDKvmStateTransferTests`: `PublicCancellationRetainsStateRAMAndFailurePriority`.

`NeverDUnicornMachineControlTests`는 실제 x64와 ARM64 엔진의 두 권한 수준에서 `UnicornMachineControlCases.def`의 원래 저장 명령을 실행합니다. `RejectedEntryPreservesStateAndRAMAndAllowsRetry`는 단계 전 취소, 실제 게스트 진입 시 정지 또는 기한 만료, 전체 입력 상태와 RAM 보존, 이후 한 번의 성공적인 저장을 검증합니다. 테스트 전용 진입 래퍼는 하이퍼바이저 없이 실행하며 네이티브 ARM64/WHP 증거를 제공하지 않습니다.

`RunDeadline::invoke`는 중지되었거나 기한이 지난 WHP 실행을 호스트 호출 전에 거부하고, 취소 중에도 실제 호스트 결과를 보존하며, 빌린 중지 토큰을 해제하기 전에 인터럽트 콜백의 종료를 확인합니다. KVM과 WHP는 실행 임대를 보유한 호출 스레드에서 완전히 캡처된 비공개 상태를 검증한 다음 동시에 도착한 중지나 기한을 분류합니다. 실제 호스트·캡처 실패와 인증된 x64 CPU 예외가 우선합니다. 일반 성공 상태는 취소 확인이 끝날 때까지 비공개로 유지하며, 확인된 중단은 추측적 CPU/RAM 효과를 버리고 재시도를 허용합니다. 준비, 네이티브 실행, 캡처는 하나의 스텝 유예를 공유합니다. 협력적 취소를 제공하지만 엄격한 실제 시간 상한은 보장하지 않습니다.

`NeverDRunControlTests`에는 이식 가능한 `NativeEntryTests.cpp`와 Windows에서 WHP를 활성화할 때의 `WhpEntryControlTests.cpp`가 포함됩니다. 메모리 내 호스트 콜백으로 Hyper-V 없이 실행 거부, 재시도, 늦은 취소, 실제 오류 보존, 완료 결과의 우선순위와 확인된 콜백 수명을 검증합니다. `NeverDKvmRunTests`는 호출 스레드의 완료, 오류 우선순위와 재진입 거부를 확인합니다. 실제 `NeverDKvmStateTransferTests`는 `KvmStateTransferCases.def`의 원래 명령을 실행합니다. `ActualCPUExceptionOutranksStopDuringCapture`와 `PublicCPUExceptionOutranksStopDuringCapture`는 실제 레지스터/XSAVE 읽기 후 중지하여 나눗셈 예외, 원래 컨텍스트, RAM과 명시적 복구를 보존합니다. Wine의 Windows ABI에서 실행한 이식 가능한 테스트는 스레드와 제어 증거만 제공하며 네이티브 WHP 실행을 입증하지 않습니다. 사용할 수 없는 네이티브 전송은 명시적으로 건너뜁니다.

`windows-pe64-v1`은 제한된 Windows x64/ARM64 콘솔 프로세스를 추가합니다. PE 로딩, PEB/TEB, 정적·동적 TLS, 시작·종료 콜백과 이름 기반 Win32 API 모델을 제공합니다. 드라이버 에뮬레이션 없이 CPU 계층을 사용합니다. DLL/CRT 로딩, GUI, 사용자 모드 SEH, 스레드와 일반 Windows 호환성은 아직 미완성입니다.

Windows 가상 메모리는 `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery`와 현재 프로세스의 `FlushInstructionCache`를 지원합니다. OS 계층은 예약 영역을 소유하고 `AddressSpace`는 커밋된 페이지, 권한, 실제 저장 공간을 관리합니다. 테스트는 동적 코드 수정, 접근 오류, 메모리 한도 재사용을 검증합니다.

`NeverDEmulationWindowsProcess` → `os/windows/process/`; `NeverDWindowsProcessTests` + `NeverDProcessPublicTests`: [windows-pe64-v1](process-emulation.md#windows-pe64-profile).

분리 회귀 검사는 정확하거나 부족한 예산, 부분 워드, 양쪽 바이트 순서, 미접근 메모리 식별자, 완전한 아핀 슬롯 경계, 늦은 선행 경로의 덮어쓰기 및 기본 무효화를 다룹니다. C API/CLI는 v6 호환성과 무효 도메인을 검사합니다. HighC·LLVMC 출력을 O0/O2에서 실행하여 반환값, 메모리, 스택, 보존 상태를 확인하지만 네이티브 동등성 인증은 아닙니다.
