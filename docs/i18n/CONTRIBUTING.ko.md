**언어**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md) | [繁體中文](CONTRIBUTING.zh-TW.md) | [日本語](CONTRIBUTING.ja.md) | [한국어](CONTRIBUTING.ko.md) | [Français](CONTRIBUTING.fr.md) | [Deutsch](CONTRIBUTING.de.md) | [Español](CONTRIBUTING.es.md) | [Italiano](CONTRIBUTING.it.md) | [Русский](CONTRIBUTING.ru.md) | [العربية](CONTRIBUTING.ar.md)

# NeverD 기여 가이드

NeverD는 의미론을 우선하는 바이너리 분석 프로젝트입니다. 좋은 기여는 범위가 명확하고,
미지원 동작을 분명하게 실패시키며, 변경된 계약을 증명하는 최소 테스트를 포함합니다.

수정하기 전에 [아키텍처 가이드](../architecture.ko.md)를 읽으세요. 테스트 스위트 선택은
[테스트 가이드](../testing.ko.md), 제품 계획은
[로드맵](../roadmap/README.ko.md)을 참고하세요.

## 필수 도구

- 재귀 서브모듈을 지원하는 Git
- CMake 3.20 이상
- Ninja
- C++20 컴파일러
- 전체 교차 대상 fixture 세트를 위한 Clang 및 LLD(`ld.lld`, `lld-link`)

재귀 서브모듈은 NeverD의 LLVM fork, Capstone fork, Unicorn, 시그니처 데이터를
제공합니다. 변경 사항을 검증할 때 임의의 시스템 버전으로 대체하지 마세요.

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
cmake --build build-release --parallel
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
cmake --build build-debug --parallel
```

Debug 빌드에서 측정한 디코드 또는 리프트 처리량을 보고하지 마세요. 벤치마크에는
Release를, 심볼이 필요한 프로파일링에는 RelWithDebInfo를 사용하세요.

### macOS용 사전 빌드 LLVM

Apple Silicon 기여자는 로컬에서 LLVM fork를 빌드하지 않아도 됩니다.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

CMake는 저장소에 설정된 릴리스 패키지를 내려받고 SHA-256을 검증한 뒤, 이후 빌드에서
압축 해제된 사용자 캐시를 재사용합니다. 사전 빌드 채널은 macOS arm64만 지원합니다.
Intel Mac과 universal 빌드는 기본 로컬 LLVM 빌드를 사용해야 합니다.
`NEVERD_LLVM_PREBUILT_TAG`, 미러 URL, 캐시 디렉터리, 명시적 checksum 같은 고급
override는 `cmake/NeverDLLVMPrebuilt.cmake`에 문서화되어 있습니다.

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

## 코드 스타일

C와 C++는 LLVM 코딩 규칙을 따르며 `.clang-format`이 저장소의 포맷 기준입니다.
변경한 파일만 포맷하세요.

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

국소 수정 때문에 저장소 전체를 다시 포맷하지 마세요. 주변 파일의 이름과 분해 방식을
따르고, 플랫폼별 동작은 해당 loader/lifter/backend 경계에 두며, 순수 C SDK로 내부
C++ 타입을 노출하지 마세요.

Markdown은 간결하고 소스로 검증할 수 있어야 합니다. 저장소 내부 파일에는 상대 링크를
사용하고, CLI 동작, 공개 API, 지원 주장, 빌드 플래그 또는 테스트 명령이 변경되면 같은
풀 리퀘스트에서 문서를 갱신하세요.

## 테스트 실행

집계 target을 통해 등록된 모든 테스트를 실행하세요.

```bash
cmake --build build-release --target check-neverd
```

개발 중에는 가장 작은 관련 target 또는 CTest label을 사용하세요.

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

[테스트 가이드](../testing.ko.md)는 모든 편의 target, label 전용 변환 스위트, 단일
테스트 정규식, fixture 컴파일, Unicorn 왕복을 설명합니다. 교차 컴파일러나 링커가 없어
target이 건너뛰어졌다면 그 제한을 보고하고, 실행되지 않은 경로를 통과했다고 설명하지
마세요.

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
- 동작, 지원 범위, 플래그, 명령 또는 테스트 소유권이 바뀌면 공개 문서와 기여자 문서를
  갱신합니다.

공개 풀 리퀘스트로 시작하면 안 되는 보안 관련 보고는
[SECURITY.md](../../SECURITY.md)를 따르세요.
