**언어**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI 친화적인 바이너리 분석·디컴파일 엔진 — 1:1 리프트, LLVM 기반**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; 순수 C SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#빌드)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-orange.svg)](#sdk와-플러그인)

[문서](README.md) · [로드맵](roadmap.md) · [기여](CONTRIBUTING.md)

</div>

---

> GitHub 저장소 홈은 항상 영어 `README.md`를 표시합니다. 위 언어 링크로 지역화 버전을 보세요.

## 개요

NeverD는 **1:1 명령어 수준 리프트**를 중심으로 한 네이티브 및 스마트 컨트랙트 분석·디컴파일 엔진입니다. **PE**, **ELF**, **Mach-O**, legacy **EVM** bytecode와 Solana **SBF ELF** program을 로드합니다. native target은 [Capstone](https://www.capstone-engine.org/)으로 decode하고 EVM/SBF는 전용 version-aware decoder와 staged IR을 사용합니다. 모든 경로는 hand-written semantics입니다. 지원 instruction은 **LLVM IR**, **C**, **SBF Rust**, **EVM Solidity reconstruction**, 또는 native의 **재작성 binary**에서 observable behavior를 보존합니다.

**strict는 기본 ON**입니다. lifter가 없는 명령어는 `UnliftedInstruction`을 던지며, 건너뛰기·추측·조용한 `NOP` 변환을 하지 않습니다.

CLI, 통합, AI 에이전트는 **순수 C API**로 동일한 엔진 **`libneverd`**를 사용하며 Capstone, LLVM, 내부 C++에 직접 링크하지 않습니다.

input format, host contract와 제한은 [EVM 가이드](evm.md)와 [Solana SBF 가이드](sbf.md)를 참고하세요.

## 왜 NeverD인가?

- **1:1 의미론** — 손수 작성 lifter; 기본 strict에서 미지원 명령어는 예외
- **LLM 친화적** — 구조화 C, LLVM IR, JSON 분석을 순수 C API로 노출하며 오류는 결정적
- **하나의 파이프라인, 여러 출구** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → 네이티브 바이너리 재작성
- **바이너리 재작성** — PE / ELF / Mach-O, section 트램폴린 또는 inplace
- **분석 도구 모음** — CLI, 디버그 정보, 시그니처, 플러그인, 선택적 난독화 패스

## 지원 대상

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> 표의 모든 셀은 구현되어 있지만 통합 테스트 깊이는 서로 다릅니다. 자세한 내용은 [아키텍처 범위 표](architecture.md#support-and-test-depth)를 참고하세요. 현대 macOS는 과거 i386 실행 파일을 링크할 수 없으므로 Mach-O i386에는 `thin` 재배치 가능 객체를 사용합니다.

legacy EVM bytecode는 native container와 독립적으로 지원합니다. Frontier부터 Fusaka까지
assigned opcode 150개가 전용 Low/Med/High IR, verified LLVM `i256`, C23
`_BitInt(256)`, Solidity output으로 이어집니다. [EVM 디컴파일](evm.md)을 참고하세요.

Solana SBF v0-v4 ELF 프로그램은 전용 strict loader, 완전한 버전별 ISA metadata,
Low/Med/High IR, 검증된 LLVM, portable C11, 안전한 stable Rust를 사용합니다.
[Solana SBF 디컴파일](sbf.md)을 참고하세요.

## 동작 방식

```text
Binary (PE / ELF / Mach-O)
  → Loader + DebugInfo
  → Capstone decode
  → LowIR     architecture-neutral NdOps · CFG
  → MedIR     types · ABI · calls · memory · SSA
       │
       ├─ lift        MedIR → LLVM IR
       ├─ decompile   MedIR → HighIR → C
       │              MedIR → LLVM IR → opt → C   (-llvm)
       └─ patch       MedIR → LLVM IR → codegen → binary

EVM (raw / hex / compiler artifact)
  → runtime normalization + hardfork-aware decode
  → EVM LowIR → EVM stack-SSA MedIR → recovered EVM HighIR
       ├─ lift        → verified LLVM i256/i512
       └─ decompile   → C23 _BitInt(256) 또는 Solidity reconstruction

Solana SBF ELF (v0-v4)
  → 버전 인식 legacy/strict loader + verifier
  → SBF LowIR → 정규화 MedIR → 복구된 SBF HighIR
       ├─ lift        → 검증된 LLVM i64 runtime ABI
       └─ decompile   → portable C11 또는 안전한 stable Rust
```

| 단계 | 역할 |
|------|------|
| **LowIR** | 약 77종 `NdOp` + CFG |
| **MedIR** | 타입, 호출 규약, 메모리 모델, SSA |
| **HighIR** | 구조화 제어 흐름(`if` / `while` / `for`) |
| **LLVM** | 최적화, C 출력, 또는 기계어 생성 |

## 빠른 시작

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 파이프라인
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# 분석
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

빌드 시 시그니처 라이브러리는 `build/bin/signatures/`에 설치됩니다. `sigs --auto`는 포맷·아키텍처·비트 너비로 세트를 고릅니다.

## 빌드

**요구 사항:** CMake ≥ 3.20 · Ninja · C++20 컴파일러 · Git submodule(LLVM fork + Capstone)

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

첫 configure는 LLVM fork를 로컬 빌드합니다(보통 30–60분). 이후는 증분입니다. 프리셋: `CMakePresets.json` → `release` / `relwithdebinfo` / `debug`.

<details>
<summary><strong>사전 빌드 LLVM · 산출물 · 테스트 · CMake 옵션</strong></summary>

<br>

**사전 빌드 LLVM**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r2
cmake --build build
```

NeverD의 일반 push 및 pull request CI는 의도적으로 LLVM submodule을 소스에서 빌드합니다. `CI` 워크플로를 수동 실행할 때 `use_prebuilt_llvm`을 선택하면 공개된 패키지를 검증할 수 있습니다. 사전 빌드 LLVM은 수동으로 `true`를 선택했을 때만 켜지며, 선택하지 않으면 자동 CI와 같은 소스 빌드 경로를 유지합니다.

공개 패키지는 CMake를 실행하는 호스트에 따라 선택됩니다:

| 호스트 | 릴리스 자산 |
|--------|-------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

각 아카이브는 `~/.cache/neverd-llvm/<tag>/<arch>/`(또는 `NEVERD_LLVM_PREBUILT_CACHE_DIR`가 가리키는 경로)에 풀리기 전에 `cmake/NeverDLLVMPrebuilt.cmake`에 고정된 다이제스트와 대조됩니다. 그 pin이 설명하지 않는 tag라면 아카이브와 함께 공개된 `.sha256`과 대조합니다. 릴리스 빌드는 macOS와 Linux에서 ccache를, Windows clang-cl에서는 GitHub Actions 캐시를 백엔드로 하는 sccache를 사용합니다. 컴파일러 캐시는 재빌드를 빠르게 할 뿐 릴리스 자산으로 공개되지 않습니다.

기본 패키지 리비전은 `neverd-llvm-v23.0.0-r2`입니다. 기존 빌드 디렉터리에 리비전 번호 없는 이전 tag나 `neverd-llvm-v23.0.0-r1`이 캐시되어 있으면 자동으로 `r2`로 이전합니다. `NEVERD_LLVM_PREBUILT_SHA256`을 명시적으로 제공한 경우에는 원래 tag를 유지합니다.

릴리스 tag는 NeverD 패키지의 버전을 나타내고, `BUILDINFO.txt`는 정확한 LLVM fork commit을 기록합니다. LLVM이 여전히 `23.0.0`을 보고하더라도 fork 소스가 바뀌었다면, 통상적인 불변 선택은 `neverd-llvm-v23.0.0-r3`(다음은 `-r4`) 같은 패키지 리비전이며, LLVM 자체의 patch 버전이 바뀌지 않는 한 `23.0.1`이 아닙니다. `NEVERD_LLVM_PREBUILT_TAG`를 그 새 리비전으로 지정하십시오.

다음 불변 package revision을 게시하려면 llvm-project의 `main` 브랜치에서 `NeverD LLVM Release` 워크플로를 실행하고 `overwrite_existing_assets`는 비활성화로 둡니다:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r3 \
  -f overwrite_existing_assets=false
```

워크플로가 성공하면 `cmake/NeverDLLVMPrebuilt.cmake`의 기본 tag, 고정 commit, 세 archive digest를 함께 갱신합니다. 기존 release를 교체하지 말고, `overwrite_existing_assets`는 legacy recovery에만 사용합니다.

**산출물**

| 경로 | 설명 |
|------|------|
| `build/bin/neverd` | 통합 CLI |
| `build/bin/neverd-bench` | 벤치마크(JSON) |
| `build/bin/neverd-sigmaker` | 정적 라이브러리에서 `.pat` 생성 |
| `build/bin/libneverd.*` | 엔진 공유 라이브러리 |
| `build/bin/sdk/` | C SDK의 canonical include root. `neverd/sdk/` 계층을 유지한 `<neverd/sdk/NeverDCAPI.h>` 또는 `<neverd/sdk/NeverDPlugin.h>` 사용 |
| `build/bin/signatures/` | 번들 시그니처 |

**테스트**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| 타깃 | 설명 |
|------|------|
| `check-neverd` | 전체 테스트 |
| `check-neverd-semantic` | 시맨틱 roundtrip만(Unicorn) |

집중 타깃, CTest 레이블, fixture 요구사항, 형식 간 재작성 그리드는 [NeverD 테스트](testing.md)를 참고하세요.

**CMake 옵션**

| 옵션 | 기본 | 설명 |
|------|------|------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI 사전 빌드 LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | `libneverd` 빌드 |
| `NEVERD_BUILD_PLUGINS` | `OFF` | 예제 플러그인 |
| `BUILD_TESTING` | `OFF` | 단위 테스트 |

</details>

## CLI

실험적 CLI `neverd mobile app.apk -o recovered-app`는 APK, DEX, smali에서 Java와 `report.json`을 생성합니다. 기본 엔진은 C++20으로 구현되며 실행 시 Python, Java 또는 JADX가 필요하지 않습니다. `NEVERD_JADX`와 PATH의 `jadx`는 외부 엔진을 선택하지 않습니다. 명시적인 `--jadx PATH`만 호환 어댑터를 선택하며 자동 전환은 없습니다. 공백이 있는 경로는 따옴표로 감싸세요. 지원 입력, 보고서 및 복구 제한은 [Android 가이드](android.md)를 참조하세요.

실험적 iOS 흐름 `neverd mobile App.ipa -o recovered-ios`는 IPA, `.app`, Mach-O에서 네이티브 C와 지원되는 Objective-C/Swift 소스를 출력합니다. 런타임 배치, 소스 단위, 메서드별 생략 이유를 유지하며 원래 바이너리를 호출하는 브리지를 사용하지 않습니다. 설정, 범위 계산, 독립 컴파일 검증은 [iOS 가이드](ios.md)를 참조하세요.

```text
neverd <command> [options] <binary>
```

### 파이프라인

| 명령 | 출력 | 설명 |
|------|------|------|
| `lift` | `.ll` | LLVM IR로 리프트 |
| `decompile` | `.c` / `.sol` / `.rs` | `--language`로 C, EVM Solidity 또는 SBF Rust 선택 |
| `decompile -llvm` | `.c` | LLVM IR + 최적화 경로 |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | 실험적: [Android](android.md), [iOS](ios.md) |
| `patch` | 바이너리 | 기계어 재작성 |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>분석 명령</strong></summary>

<br>

| 명령 | 용도 |
|------|------|
| `info` / `dashboard` / `headers` | 메타데이터와 개요 |
| `funcs` | 발견된 함수 |
| `disasm` | 디스어셈블(`--func` 이름 또는 hex) |
| `sym-explore` | 제한된 네이티브 LowIR 경로 탐색(`--func`, JSON 출력) |
| `audit` | 힙 수명 결함: 누수, 이중 해제, 해제 후 사용(JSON) |
| `hunt` | 위험한 복사 오버플로의 기호 증거와 후보 값(JSON) |
| `hex` | 주소의 hex dump |
| `cfg` / `callgraph` | CFG / 호출 그래프(JSON; DOT/SVG 선택) |
| `xrefs` | 교차 참조 |
| `strings` / `search` | 문자열 / 바이트 또는 텍스트 검색 |
| `imports` / `exports` / `symbols` / `relocs` | 테이블 |
| `segments` / `sections` / `entrypoints` | 레이아웃 |
| `diff` | 두 바이너리 비교(`-a` / `-b`) |
| `sigs` | 시그니처(`--auto`) |
| `rename` / `annotate` / `bookmarks` | 세션 주석 |
| `export` | 결과 내보내기 |
| `plugins` | 플러그인 목록 또는 실행 |

대부분의 분석 명령은 `--json`을 받습니다.

</details>

## SDK와 플러그인

통합은 `libneverd`의 **순수 C API**를 사용합니다:

| 헤더 | 역할 |
|------|------|
| `NeverDCAPI.h` | 세션, 리프트, 디컴파일, patch, IR / CFG, 주석 |
| `NeverDPlugin.h` | 동적 라이브러리 플러그인 ABI |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

EVM은 `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)`로 Solidity를
명시적으로 선택합니다. 기존 `neverd_decompile_all`은 C를 출력합니다.
[EVM C API 예시](evm.md#c-api)를 참고하세요.

네이티브 공유 라이브러리와 Python `.py` file은 같은 플러그인 lifecycle을 사용합니다.
`-DNEVERD_BUILD_PLUGINS=ON`으로 네이티브 예제를 빌드합니다. 순수 C descriptor,
callback, build/link, discovery, CLI workflow 및 ABI 제약은
[네이티브 플러그인 가이드](plugins.md)를 참고하십시오. Python 지원은 default로
활성화되며 `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`로 완전히 제외할 수 있습니다.
typed SDK와 package workflow는 [Python 플러그인 가이드](python-plugins.md)에
설명되어 있습니다. 두 종류 모두 `<neverd-dir>/plugins`, `~/.neverd/plugins`,
`$NEVERD_PLUGIN_PATH`를 사용합니다.

## 의존성

| 구성 요소 | 역할 | 소스 |
|-----------|------|------|
| **LLVM**(fork) | IR, 최적화, 코드 생성, 진단 | `third_party/llvm-project` 또는 사전 빌드 |
| **Capstone** | 디코드 | `third_party/capstone` |

서드파티 구성 요소는 자체 라이선스를 유지합니다.

## 기여

개발 결과는 **`dev`** 브랜치에 통합합니다. 환경 설정, Release/Debug 지침, 스타일, 집중 테스트, 풀 리퀘스트 요구사항은 [기여 가이드](CONTRIBUTING.md)를 참고하세요. [아키텍처](architecture.md)와 [테스트](testing.md) 가이드는 일반적인 변경을 관련 코드 및 검증 스위트에 연결합니다.

## 라이선스

[AGPL-3.0](../../LICENSE)

AGPL이 적용되는 NeverD 코드의 복제본이나 수정본을 재배포할 때는 AGPL에 따라 저작권 및 라이선스 고지를 유지해야 합니다. 이는 AI/LLM을 활용한 재사용과 LLVM을 통한 변환에도 적용됩니다. 출처를 추적할 수 있도록 저장소, 파일, 사용한 버전도 명시할 것을 권장합니다. [저작자 표시 및 출처 인용 안내](ATTRIBUTION.md), [NOTICE](../../NOTICE), [CITATION.cff](../../CITATION.cff)를 참고하세요.

LLVM 구성 요소는 Apache-2.0 WITH LLVM-exception 라이선스를 유지합니다. Capstone은 자체 라이선스를 유지합니다.
