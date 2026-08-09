**언어**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← 문서 인덱스](README.ko.md)

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
[CONTRIBUTING.md](i18n/CONTRIBUTING.ko.md)를 참고하세요.

## 테스트 배치

`add_neverd_unittest`는 GoogleTest 실행 파일 하나를 만들고 발견한 각 사례에 실행
target 이름과 같은 CTest label을 지정합니다.

| 소스 영역 | Target 및 CTest label | 범위 |
|-----------|------------------------|------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | 교차 플랫폼 하위 프로세스 호출, quoting, redirect, 종료 코드 |
| `unittests/libc` | `NeverDLibCTests` | 알려진 libc 이름과 분류 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR 모양, IR 단계, loader, relocation, 포맷 fixture, 디컴파일, 대표 patch 흐름 |
| `unittests/semantic`의 대부분 파일 | `NeverDSemanticTests` | 명령어, ABI, 제어 흐름, C 표현식, lift/recompile 차등 의미론 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 네 ISA×세 object 포맷 재작성/난독화 동등성 |
| `unittests/semantic`의 집중 변환 파일 | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | 큰 의미론 바이너리에서 분리한 빠른 재링크 probe |

등록의 기준은
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt),
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt)입니다.

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

### Unicorn 차등 왕복

의미론 fixture는 텍스트 모양이 아니라 동작을 테스트합니다.

1. 작은 C/assembly 사례를 작성하거나 LLVM IR을 구성합니다.
2. Clang/LLVM으로 요청한 대상을 위해 컴파일합니다.
3. 원래 machine code를 Unicorn에서 실행하고 예상 반환값이나 fixture가 정의한 상태를 캡처합니다.
4. NeverD로 로드하고 lift하여 LLVM IR을 출력한 뒤 다시 machine code로 컴파일합니다.
5. 같은 ABI, 입력, 메모리 배치, CPU 모델로 재생성 코드를 실행합니다.
6. 관측 가능한 결과를 비교합니다.

주요 구현은
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h)입니다.
patch-full fixture는 patch 작업과 같은 rewrite backend인
`Codegen::compileForRewrite`를 사용한 뒤 전체 4×3 ISA/포맷 grid에서 baseline과
변환 코드를 비교합니다.

결정적인 NeverD 의미론 실패는 실패 테스트여야 합니다. skip은 명시적인 외부 기능
경계에만 사용하고 이유를 읽으세요. 교차 linker가 없는 녹색 요약은 해당 포맷 경로가
실행되었음을 증명하지 않습니다.

## 일회성 target

custom target은 의존성을 빌드한 뒤 host CPU에서 정한 병렬도로 CTest를 실행합니다.

| CMake target | 선택 범위 |
|--------------|-----------|
| `check-neverd` | 등록된 모든 테스트 |
| `check-neverd-semantic` | `NeverDSemanticTests`만 |
| `check-neverd-patch-full` | `NeverDPatchFullTests`만 |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests`만 |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests`만 |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests`만 |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
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
| Libc 인식 | `NeverDLibCTests` | 동작 변경 시 의미론 call/ABI 사례 |
| 프로세스 실행 또는 quoting | `NeverDTestProcessTests` | 지원 host마다 영향받는 CLI/의미론 사례 하나 |

테스트는 가장 낮은 안정 경계에서 계약을 표현해야 합니다. LowIR 모양 테스트는 lifter
귀속에 유용합니다. 그럴듯한 두 IR 모양이 다르게 동작할 수 있다면 의미론 왕복이
필요합니다. 작은 opcode, CFG, 관측 상태 assertion으로 충분할 때 함수 전체 golden
dump를 피하세요.

## CI 관계

CI는 Linux, macOS, Windows에서 테스트를 켠 Release를 빌드하고 발견 inventory를
audit한 다음 플랫폼별 label 제외를 적용합니다. 프로필은
`.github/workflows/ci.yml`과 `scripts/audit_ci_test_inventory.py`에 있습니다.
비싼 모든 스위트를 대표하는 단일 matrix shard는 없으므로 필요한 교차 도구를 갖춘
머신에서는 로컬 `check-neverd`가 가장 명확한 전체 병합 전 신호입니다.
