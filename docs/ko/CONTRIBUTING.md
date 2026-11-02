**언어**: [English](../../CONTRIBUTING.md) | [简体中文](../zh-CN/CONTRIBUTING.md) | [繁體中文](../zh-TW/CONTRIBUTING.md) | [日本語](../ja/CONTRIBUTING.md) | [한국어](CONTRIBUTING.md) | [Français](../fr/CONTRIBUTING.md) | [Deutsch](../de/CONTRIBUTING.md) | [Español](../es/CONTRIBUTING.md) | [Italiano](../it/CONTRIBUTING.md) | [Русский](../ru/CONTRIBUTING.md) | [العربية](../ar/CONTRIBUTING.md)

<!-- i18n-source: af6c34901bacab00e5d1ed68b031f0a48a3ff64ac85ee208695647ec935b9790 -->

# NeverD 기여 가이드

NeverD는 의미론을 우선하는 바이너리 분석 프로젝트입니다. 좋은 기여는 범위가 명확하고,
미지원 동작을 분명하게 실패시키며, 변경된 계약을 증명하는 최소 테스트를 포함합니다.

기존 코드를 도입할 때는 AI/LLM 도구로 복제하거나 개작한 코드를 포함하여,
저작권, 라이선스 및 출처 고지를 보존하세요.
[저작자 표시와 인용 안내](ATTRIBUTION.md)를 따르고, 재사용한 자료의 원래 출처와
리비전을 명시하며, 제출하기 전에 라이선스를 확인하세요.
제3자 고지는 원래의 라이선스에 따라 보존하세요.

수정하기 전에 [아키텍처 가이드](architecture.md)를 읽으세요. 테스트 스위트 선택은
[테스트 가이드](testing.md), 제품 계획은
[로드맵](roadmap.md)을 참고하세요.

<!-- i18n-section: prerequisites -->
## 필수 도구

- 재귀 서브모듈을 지원하는 Git
- CMake 3.20 이상
- Ninja
- C++20 컴파일러
- Python 플러그인을 활성화하면 CPython 3.10+ 및 임베딩 개발 파일
- 전체 교차 대상 fixture 세트를 위한 Clang 및 LLD(`ld.lld`, `lld-link`)

재귀 서브모듈은 NeverD의 LLVM fork, Capstone fork, Unicorn, 시그니처 데이터를
제공합니다. 변경 사항을 검증할 때 임의의 시스템 버전으로 대체하지 마세요.

<!-- i18n-section: clone -->
## 복제 및 초기화

개발 결과는 저장소 기본 브랜치이기도 한 `dev`에 통합합니다. 모든 서브모듈과 함께
복제하세요.

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

기존 복제본에서는 첫 빌드 전과 기록된 서브모듈 리비전을 변경한 커밋 이후에
서브모듈을 동기화하세요.

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

<!-- i18n-section: build -->
## 빌드 프로필 선택

| 프로필 | 용도 | 주요 동작 |
|--------|------|-----------|
| Release | 일반 개발, 전체 테스트, 디코드/리프트 벤치마크 | 최적화됨. 대표성 있는 처리량 |
| RelWithDebInfo | 최적화된 핫 패스 프로파일링 또는 디버깅 | 최적화 및 디버그 심볼 포함 |
| Debug | assertion, 소스 수준 단계 실행, 국소 정확성 작업 | 최적화되지 않음. 디코드 벤치마크가 의도적으로 훨씬 느림 |

작업에 Debug 동작이 명시적으로 필요하지 않다면 Release를 사용하세요.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
python scripts/build.py build-release --parallel
```

기본적으로 `third_party/llvm-project`를 통합 의존성으로 빌드합니다. 첫 빌드는 보통
30~60분이 걸리며 이후 빌드는 증분 방식입니다. `CMakePresets.json`에도 `release`,
`relwithdebinfo`, `debug` 구성/빌드 preset이 있지만, 위에서는 테스트 설정을 명시하기
위해 별도 빌드 디렉터리를 사용합니다.

소스 수준 디버깅에는 Release 트리를 다시 구성하지 말고 별도 디렉터리를 사용하세요.

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
python scripts/build.py build-debug --parallel
```

Debug 빌드에서 측정한 디코드 또는 리프트 처리량을 보고하지 마세요. 벤치마크에는
Release를, 심볼이 필요한 프로파일링에는 RelWithDebInfo를 사용하세요.

<!-- i18n-section: allocator -->
### 프로세스 할당자

Linux, macOS, Windows MSVC/clang-cl 빌드는 `NEVERD_ENABLE_MIMALLOC`를 기본 활성화합니다.
공통 CMake 도우미로 만든 도구, 분석 worker, 테스트는 NeverC의 호스트 방식에 따라 NeverSight
mimalloc fork의 기본 브랜치를 사용합니다. Unix는 실행 파일마다 한 번 내장합니다. Windows는
mimalloc DLL과 같은 아키텍처의 `mimalloc-redirect*.dll`로 동적 CRT 할당(libneverd 및 네이티브
플러그인 포함)을 전환합니다. 두 DLL 모두 실행 파일 옆과 설치 `bin`에 배치하므로 함께 배포합니다.
Windows는 `/MD` 또는 `/MDd`가 필요하며 정적 CRT에서는 mimalloc을 끕니다. SDK 자체는 임베딩
호스트에 할당자를 강제하지 않습니다. Windows ARM64는 네이티브 ARM64로 빌드합니다. x64 DLL은
x64 에뮬레이션의 ARM64EC CRT를 전환할 수 없습니다. MinGW는 시스템 할당자를 유지합니다.
[상위 Windows 요구 사항](https://github.com/NeverSight/mimalloc/blob/main3/bin/readme.md)을 참고하세요.

시스템 기준선이나 외부 할당자는 `-DNEVERD_ENABLE_MIMALLOC=OFF`를 사용합니다.
기존 `NEVERD_WORKER_MIMALLOC=OFF`는 worker만 끕니다. CMake는 address/thread/memory/leak
sanitizer 빌드에서 mimalloc을 자동 억제하며 복합 및 구성별 컴파일러/링커 플래그도 포함합니다.
UBSan 단독은 호환됩니다. 대상별 플래그로만 sanitizer를 추가하면 옵션도 OFF로 설정합니다.
Windows는 `MIMALLOC_DISABLE_REDIRECT=1`로 재빌드 없이 비교하고 `MIMALLOC_VERBOSE=1`로 활성화를 확인합니다.

지원 빌드 진입점 `python scripts/build.py BUILD_DIR`는 매번 5개 NeverSight fork를 원격 기본
브랜치에서 갱신합니다. Capstone/Unicorn처럼 `third_party`의 Git 서브모듈입니다. 스크립트는 실제
브랜치를 찾아 5개를 갱신하고 커밋된 소스를 `BUILD_DIR/third_party/<library>/<commit>/src`에
내보냅니다. 미커밋/분기된 로컬 서브모듈 변경은 덮어쓰지 않고 빌드를 중단하며, 네트워크 실패도 중단합니다.
갱신부터 재구성/컴파일까지 디렉터리를 잠그고 다음 호출까지 동일 스냅샷을 유지합니다. 증분 빌드도 새 커밋을 받습니다.

`neverd-dependency-inputs.json`은 구성 입력, 커밋, 내보내기 SHA-256을 기록하고 설치 제품과 함께
배포하지만 바이너리를 인증하지 않습니다. `neverd-build-receipt.json`은 요청한 대상/구성의 성공/실패만
기록하며 CI는 둘 다 보관합니다. 설치는 실패한 빌드, 변경된 입력, 미빌드 구성을 거부합니다. 제품 CI는
모든 설치 대상을 빌드한 뒤 설치를 시험합니다. 실행 사이 주 브랜치가 바뀔 수 있으므로 플랫폼 간 결과를
통합 평가하기 전에 커밋을 비교합니다. 직접 `cmake --build`는 최신 여부를 검사하고 오래된 스냅샷을
거부합니다. 갱신은 Python 진입점을 사용하며 자동 재생성도 같은 스냅샷을 유지합니다.
`FETCHCONTENT_SOURCE_DIR_NEVERD_*`는 격리된 로컬 fixture용이며 주 빌드 진입점에서는 거부합니다.
다중 구성 생성기는 명시적 `--config`가 필요합니다. 진입점으로 컴파일한 뒤 테스트와 `cmake --install`을
별도로 실행합니다. 직접 `install` 대상을 전달하면 컴파일 전에 거부됩니다.

스모크 테스트와 성능 비교는 [할당자 검증](../testing.md#allocator-validation)을 참고하세요.

<!-- i18n-section: acceleration -->
### 가속 라이브러리

일반 빌드는 Highway 바이트 스캔, simdutf UTF-8 검증, simdjson 전송 파싱, oneTBB 파이프라인
스케줄링을 활성화하고 프로세스 할당자를 유지합니다. JSON은 기존 nlohmann DOM/직렬화를 유지합니다.
유효한 정수 전용 프레임은 SIMD tape에서 복사하고, 소수와 음의 0은 기존 숫자 변환을 사용합니다.
simdjson 영역 밖 입력은 기존 허용 경로로 처리하며 잘못된 외부 텍스트도 바이트 보존 이스케이프를 유지합니다.
이 라이브러리와 mimalloc은 `third_party/forks.json`의 NeverSight fork에서 오며 기본 브랜치에 호환
수정을 둡니다. 빌드는 현 소스 라이선스를 복사하고 정확한 커밋을 기록하며 5개 의존성을 과거 릴리스로 고정하지 않습니다.

`NEVERD_ENABLE_HIGHWAY`, `NEVERD_ENABLE_SIMDUTF`, `NEVERD_ENABLE_SIMDJSON`,
`NEVERD_ENABLE_TBB`는 기본 `ON`이며 비교 시 각각 `OFF`로 설정합니다. oneTBB는 배치 간 worker를
보존하고 8 MiB 스택을 검사하며 `tbbmalloc`이나 프록시를 빌드/로드하지 않습니다. 큰 스택 호출자는
여유를 재사용하고 작은 경우 네이티브 조정 스레드를 받습니다. 최초 사용 후 fork하거나 SDK를 동적 해제하는
호스트는 네이티브 스레드 기준선 `NEVERD_ENABLE_TBB=OFF`를 사용해야 합니다. 보존 스케줄러는 이
수명주기를 지원하지 않습니다. oneTBB는 공유 코어에 정적으로 링크되며 심볼은 해당 코어 내부로 제한됩니다.
CLI의 컴포넌트 호출과 GUI worker 요청은 같은 스케줄러를 사용하며 TBB DLL/공유 라이브러리를 배포하지 않습니다.
`neverd_shared`를 링크하는 타깃은 공유 스케줄러를 자동으로 선택합니다. 다른 타깃을 통해 SDK를 사용하면서
컴포넌트 아카이브도 링크하는 클라이언트는 최종 타깃에 `neverd_use_shared_task_scheduler`를 적용해야 합니다.
독립 실행형 컴포넌트 도구는 자체 런타임을 유지합니다.
이 제한된 통합은 한 프로세스에 여러 NeverD 스케줄러가 생기는 것을 방지합니다. 업스트림은 여전히
[oneTBB 정적 빌드](https://uxlfoundation.github.io/oneTBB/main/intro/static_linking.html)를 지원하지 않는 것으로
명시하며 일반적인 용도로는 사용을 권장하지 않습니다.

네이티브 테스트, 비활성 비교 및 호환 한계는 [가속 검증](../testing.md#acceleration-libraries)을 참고하세요.

<!-- i18n-section: prebuilt-llvm -->
### macOS용 사전 빌드 LLVM

Apple Silicon 기여자는 로컬에서 LLVM fork를 빌드하지 않아도 됩니다.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
python scripts/build.py build-release --parallel
```

CMake는 저장소에 설정된 릴리스 패키지를 내려받고 SHA-256을 검증한 뒤, 이후 빌드에서
압축 해제된 사용자 캐시를 재사용합니다. 사전 빌드 채널은 macOS arm64만 지원합니다.
Intel Mac과 universal 빌드는 기본 로컬 LLVM 빌드를 사용해야 합니다.
`NEVERD_LLVM_PREBUILT_TAG`, 미러 URL, 캐시 디렉터리, 명시적 checksum 같은 고급
override는 `cmake/NeverDLLVMPrebuilt.cmake`에 문서화되어 있습니다.

LLVM 수정은 [NeverSight LLVM fork](https://github.com/NeverSight/llvm-project)에서 관리합니다.
먼저 fork에 커밋하고 병합한 다음 NeverD의 LLVM 서브모듈 버전을 업데이트합니다.
통합 빌드는 해당 소스를 직접 컴파일합니다. NeverD에 LLVM 패치 파일이나 빌드 디렉터리 소스 덮어쓰기를 추가하지 마세요.
같은 동작을 검증하려면 수정된 fork에서 사전 빌드 패키지를 다시 빌드해야 합니다.

<!-- i18n-section: workflow -->
## 브랜치 및 풀 리퀘스트 워크플로

최신 `dev`에서 시작하여 범위가 명확한 topic 브랜치를 만드세요.

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

풀 리퀘스트는 추정한 릴리스 브랜치가 아니라 `dev`를 대상으로 여세요. 커밋은 검토하기
쉽게 유지합니다. 하나의 일관된 목적만 담고, 생성된 빌드 출력이나 무관한 포맷 변경을
포함하지 않으며, 제안의 일부가 아니라면 서브모듈 리비전을 바꾸지 마세요.

<!-- i18n-section: code-style -->
## 코드 스타일

C/C++는 [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html)를 따르며,
[`.clang-format`](../../.clang-format)이 서식, [`.editorconfig`](../../.editorconfig)가 인코딩,
줄 끝, 공백의 기준입니다. C++ 버전은 상위 문서가 아닌 `CMakeLists.txt`를 따릅니다. 변경 파일을 포맷합니다:

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

CI는 [`scripts/check_clang_format.py`](../../scripts/check_clang_format.py)에 고정된 버전을 쓰므로
재현할 때도 동일 버전을 사용합니다. 포맷은 헤더 소유권이나 정책 리터럴을 검사하지 않습니다. 국소 수정에
저장소 전체 포맷을 하지 않습니다. 주변 명명/분해 방식을 따르고 플랫폼 동작은 loader/lifter/backend 경계에
두며 순수 C SDK로 내부 C++ 타입을 노출하지 않습니다.

<!-- i18n-section: headers -->
### 파일 및 헤더 구성

- 새 C++ 파일은 LLVM 파일명/목적 배너, `NeverD Decompiler`, Doxygen `\file`로 시작합니다.
  `lib/debug/PDBLoader.cpp`를 참고하고 저작권/라이선스 고지를 유지합니다.
- 독립적으로 사용할 수 있는 `.h`에 의존성을 명시합니다. 구현은 해당 인터페이스를 먼저 포함합니다. 이후 `.clang-format`
  순서(인접 비공개, 공개 `neverd/`, 따옴표 LLVM/Capstone, 꺾쇠 헤더)를 따릅니다.
  우연한 순서나 전이적 include에 의존하지 않습니다.
- 새 헤더는 경로 기반 `NEVERD_..._H` 가드를 사용합니다. `neverd/sdk/Limits.h`는
  `NEVERD_SDK_LIMITS_H`입니다. 비공개/도구 디렉터리를 충분히 포함해 충돌을 막고 `#endif`에 이름을
  주석으로 적습니다. 기존 헤더의 명시적 통일 때 적용하며 관련 없는 헤더는 건드리지 않습니다.
- 공유 인터페이스는 `include/neverd/<component>/`, 비공개 헤더는 `lib/` 또는 `tools/`의 소스 옆에
  둡니다. 위치가 안정된 공개 C SDK API를 보장하지 않습니다. 의존성은 비순환이며 코어 모델/reader는
  세션/전송 어댑터와 독립적입니다. 알고리즘은 소유 디렉터리에 두고 CLI/SDK/worker에 의미를 중복하지
  않습니다. [아키텍처](architecture.md)를 참고하세요.
- 소유 네임스페이스와 명확한 이름을 사용합니다. 새 타입/필드 `UpperCamelCase`, 함수
  `lowerCamelCase`, 정책 상수 `kUpperCamelCase`. 기존 인터페이스, 공개 C/Python 계약 및 STL
  관례의 철자를 유지하며 헤더에 네임스페이스 전체 `using namespace`를 추가하지 않습니다.
- 의도적 다중 포함 표는 `.def`/`.inc`와 매크로 계약을 유지합니다. 독립 `.h` 인터페이스가 아니므로
  반복 확장을 막는 가드를 추가하지 않습니다. 생성 헤더는 생성기를 수정합니다.

독립 정책 헤더 예:

```cpp
//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include <cstdint>

namespace neverd::sdk {

/// Traversal ceiling for a function without a recorded instruction extent.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
```

<!-- i18n-section: policy -->
### 제한, 기본값 및 구성 소유권

헤더 검색 경로에 직접 추가하는 도구 디렉터리에서는 `ResourceLimits.h`를 사용합니다. 대소문자를 구분하지 않는 파일 시스템에서는 로컬 `Limits.h`가 시스템 `<limits.h>`를 가릴 수 있습니다.

호출자가 하나라도 정책은 해당 하위 시스템의 limits/configuration 헤더에 단일 권위 정의를 둡니다.
입출력 상한, 재귀/그래프, 작업/반복 예산, 휴리스틱 임계값, 캐시, 페이지/배치, 재시도, 시간 제한을 포함합니다.
`constexpr`, 매크로, 리터럴 비교, `min`/`clamp`, 생성자/필드 초기화, 기본 인수에도 적용합니다.

기존 정의부터 선택합니다:

| 소유자 | 정의 위치 |
| --- | --- |
| 공유 코어/파이프라인 | [`include/neverd/Limits.h`](../../include/neverd/Limits.h) |
| SDK | [`include/neverd/sdk/Limits.h`](../../include/neverd/sdk/Limits.h) |
| Web | [`include/neverd/web/Limits.h`](../../include/neverd/web/Limits.h) 또는 기존 전용 정의 |
| 다른 라이브러리 | 기존 헤더/정의 표, 없으면 소유 계층의 전용 `Limits.h` |
| CLI, worker, GUI 전용 | 도구 내부 `ResourceLimits.h`/`Config.h`; 코어가 역으로 의존하지 않음 |
| 호출자 선택 설정 | 소유 `Config.h`, `*Options.h`, `*Configuration.h` 모델 |

헤더를 가볍게 유지하고 목적별로 묶습니다. 기존 `*Limits.def`/구성 표를 재사용해 독립적인 C++ 헤더로
노출합니다. `Limits.h`, `Constants.h`, `Config.h` 사이에 같은 정책을 중복 정의하지 않습니다.
공유 코어 정책을 SDK/도구에 두거나 무관한 모든 모듈 정책을 루트 `Limits.h`에 모으지 않습니다.

C++ 스칼라는 타입 있는 `inline constexpr`와 기존 네임스페이스(`neverd::limits`, `neverd::sdk`)를
사용합니다. `kMaxSourceBytes`, `kDefaultPageRows`, `kRetryDelayMilliseconds`처럼 목적/단위를
이름에 넣고 0/센티널과 특수 경계를 설명합니다. 의미가 다른 기본값과 하드 상한은 분리합니다. 옵션 모델이
필드 기본값을 소유할 수 있으며 소비자는 모델/기본 팩토리/같은 상수를 씁니다. 관련 값은 공통 정의로 유도합니다.

어댑터, 영속성 검증, CLI/GUI/worker, Python/MCP, 스키마 상한, 제한 진단 모두 같은 정의를 씁니다.
언어 간 계약은 표/생성 바인딩으로 유도하며 불가능하면 드리프트 검사를 추가합니다. 순수 C 헤더 호환을
유지합니다. 더 엄격한 미리보기/페이지 제한은 별도 이름/소유자를 둡니다. 숫자가 같다고 같은 정책은 아닙니다.

바이너리 배치, ABI/프로토콜/명령 상수, 정수 범위, 증명 알고리즘 고유 비용은 의미 모델에 둡니다.
상태 초기값, 인덱스, 할당 힌트, 독립 테스트 입력은 자동으로 정책이 되지 않습니다. 독립 기대 경계를
검증 대상 구현 상수로 대체하지 않습니다.

선언과 리터럴 모두 검토합니다. 여러 줄 가드, 루프, 매크로, 기본값, 타이머, 바인딩을 포함하며 만지는
모든 정책의 의미/소유자를 확인합니다. `Max*` 검색만으로 부족합니다. 통합은 타입, 값, 비교 방향,
센티널, 고갈/오류 동작을 보존하고 정책 변경은 별도로 처리해 [해당 테스트](testing.md)를 실행합니다.

<!-- i18n-section: artifacts -->
### 문서 및 로컬 산출물

Markdown은 간결하고 소스로 검증 가능하게 작성하고 내부 상대 링크를 사용합니다. CLI/API, 지원,
빌드 플래그, 테스트 명령 변경은 같은 PR에 문서화합니다. 이 가이드의 모든 언어를 동기화하고 번역 검토
후 `i18n-source` SHA-256을 갱신합니다. 절, 예제, API/옵션, 링크 목적지를 맞추며
`python3 scripts/check_docs_i18n.py`로 검사합니다.

장비별 성능 보고서, 원시 추적, 시간 요약은 무시된 build/benchmark-results에 둡니다. 재사용 가능한
측정 도구와 회귀 테스트를 커밋하며 로컬 보고서나 복사된 결과 표는 넣지 않습니다.

<!-- i18n-section: tests -->
## 테스트 실행

집계 target을 통해 등록된 모든 테스트를 실행하세요.

```bash
python scripts/build.py build-release --parallel
cmake --build build-release --target check-neverd
```

개발 중에는 가장 작은 관련 target 또는 CTest label을 사용하세요.

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
python scripts/build.py build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

[테스트 가이드](testing.md)는 모든 편의 target, label 전용 변환 스위트, 단일
테스트 정규식, fixture 컴파일, Unicorn 왕복을 설명합니다. 교차 컴파일러나 링커가 없어
target이 건너뛰어졌다면 그 제한을 보고하고, 실행되지 않은 경로를 통과했다고 설명하지
마세요.

<!-- i18n-section: python-sdk -->
### Python 플러그인 SDK

SDK 변경 시 순수 Python/드리프트 검사 후 네이티브 어댑터 테스트를 실행합니다:

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
python scripts/build.py build-release --target \
  NeverDPluginRuntimeTests NeverDPythonRuntimeTests NeverDPythonPluginTests
ctest --test-dir build-release -R 'NeverD(Python|Plugin)' --output-on-failure
```

공개 C 선언, Python ABI 서명, 패키지/CMake 버전, 전달 워크플로 정책은 하나의 검토 계약입니다.
같은 변경에서 갱신합니다. [Python 플러그인 가이드](python-plugins.md)를 참고하세요.

<!-- i18n-section: review -->
## 풀 리퀘스트 체크리스트

검토를 요청하기 전에:

- 유지관리자가 선호하는 방식으로 최신 `dev`를 rebase 또는 merge하고 서브모듈 변경을
  의도적으로 해결합니다.
- 영향을 받는 target을 Release로 빌드합니다. 다른 프로필이 필요하면 이유를 설명합니다.
- 좁은 회귀 테스트와 실행 가능한 가장 넓은 관련 스위트를 실행하고, 정확한 명령과 모든
  skip을 PR 설명에 기록합니다.
- strict lifting을 유지합니다. 미지원 명령어가 추측한 연산이나 `NOP`으로 조용히 바뀌면
  안 됩니다.
- 동작 변경에는 텍스트 IR snapshot뿐 아니라 의미론적 coverage를 추가합니다.
- 무관한 정리, 생성 파일, 로컬 빌드 산출물을 diff에서 제외합니다.
- [헤더 및 정책 소유권 규칙](#코드-스타일), 숫자 가드/기본값, 바인딩 드리프트를 검토합니다.
  포맷 성공만으로 충분하지 않습니다.
- 동작, 지원 범위, 플래그, 명령 또는 테스트 소유권이 바뀌면 공개 문서와 기여자 문서를
  갱신합니다.

공개 풀 리퀘스트로 시작하면 안 되는 보안 관련 보고는
[SECURITY.md](../../SECURITY.md)를 따르세요.
