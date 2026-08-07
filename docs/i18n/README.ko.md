**언어**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI 친화적인 바이너리 분석·디컴파일 엔진 — 1:1 리프트, LLVM 기반**

PE · ELF · Mach-O &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 &nbsp;|&nbsp; 순수 C SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#빌드)
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O-informational.svg)](#지원-대상)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM-orange.svg)](#지원-대상)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-lightgrey.svg)](#sdk와-플러그인)

[문서](../README.ko.md) · [로드맵](../roadmap/README.ko.md) · [기여](#기여)

</div>

---

> GitHub 저장소 홈은 항상 영어 `README.md`를 표시합니다. 위 언어 링크로 지역화 버전을 보세요.

## 개요

NeverD는 **1:1 명령어 수준 리프트**를 중심으로 한 네이티브 바이너리 분석·디컴파일 엔진입니다. **PE**, **ELF**, **Mach-O**를 로드하고 [Capstone](https://www.capstone-engine.org/)으로 디코드한 뒤, **손으로 작성한 의미론**의 4단계 IR 파이프라인으로 리프트합니다 — 근사 변환이 아닙니다. 목표는 **100% 의미론적 충실도**: 지원 명령어가 **LLVM IR**, **구조화 C**, 또는 **재작성된 바이너리**에서 완전한 관측 가능 동작을 유지하는 것입니다.

**strict는 기본 ON**입니다. lifter가 없는 명령어는 `UnliftedInstruction`을 던지며, 건너뛰기·추측·조용한 `NOP` 변환을 하지 않습니다.

CLI, 통합, AI 에이전트는 **순수 C API**로 동일한 엔진 **`libneverd`**를 사용하며 Capstone, LLVM, 내부 C++에 직접 링크하지 않습니다.

이후 릴리스에서는 같은 IR 스택에 [EVM](../roadmap/README.ko.md#2-evm-바이트코드-디컴파일)과 [Solana eBPF / SBF](../roadmap/README.ko.md#3-solana-ebpf-sbf-디컴파일) 디컴파일을 추가합니다 — [로드맵](../roadmap/README.ko.md)을 참고하세요.

## 왜 NeverD인가?

- **1:1 의미론** — 손수 작성 lifter; 기본 strict에서 미지원 명령어는 예외
- **LLM 친화적** — 구조화 C, LLVM IR, JSON 분석을 순수 C API로 노출하며 오류는 결정적
- **하나의 파이프라인, 세 출구** — `lift` → LLVM IR · `decompile` → C · `patch` → 재작성 바이너리
- **바이너리 재작성** — PE / ELF / Mach-O, section 트램폴린 또는 inplace
- **분석 도구 모음** — CLI, 디버그 정보, 시그니처, 플러그인, 선택적 난독화 패스

## 지원 대상

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Mach-O i386 통합 범위는 `thin` 재배치 가능 객체와 실행 파일 재작성 백엔드 테스트를 사용합니다. 현재 macOS 호스트에서는 과거 i386 실행 파일을 링크할 수 없습니다.

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

# 분석
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

**산출물**

| 경로 | 설명 |
|------|------|
| `build/bin/neverd` | 통합 CLI |
| `build/bin/neverd-bench` | 벤치마크(JSON) |
| `build/bin/neverd-sigmaker` | 정적 라이브러리에서 `.pat` 생성 |
| `build/bin/libneverd.*` | 엔진 공유 라이브러리 |
| `build/bin/sdk/` | `NeverDCAPI.h`, `NeverDPlugin.h` |
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

**CMake 옵션**

| 옵션 | 기본 | 설명 |
|------|------|------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | CI 사전 빌드 LLVM |
| `NEVERD_BUILD_SHARED` | `ON` | `libneverd` 빌드 |
| `NEVERD_BUILD_PLUGINS` | `OFF` | 예제 플러그인 |
| `BUILD_TESTING` | `OFF` | 단위 테스트 |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### 파이프라인

| 명령 | 출력 | 설명 |
|------|------|------|
| `lift` | `.ll` | LLVM IR로 리프트 |
| `decompile` | `.c` | 구조화 C(HighIR) |
| `decompile -llvm` | `.c` | LLVM IR + 최적화 경로 |
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

`-DNEVERD_BUILD_PLUGINS=ON`으로 예제 플러그인을 빌드합니다. 로드 경로: `<neverd-dir>/plugins`, `~/.neverd/plugins`, `$NEVERD_PLUGIN_PATH`.

## 의존성

| 구성 요소 | 역할 | 소스 |
|-----------|------|------|
| **LLVM**(fork) | IR, 최적화, 코드 생성, 진단 | `third_party/llvm-project` 또는 사전 빌드 |
| **Capstone** | 디코드 | `third_party/capstone` |

서드파티 구성 요소는 자체 라이선스를 유지합니다.

## 기여

스타일은 LLVM 관례(`.clang-format`)를 따릅니다.

개발은 **`dev`** 브랜치에서 진행됩니다(GitHub 기본 브랜치).

```bash
git clone -b dev https://github.com/NeverSight/NeverD.git
cd NeverD
git submodule update --init --recursive
```

## 라이선스

[AGPL-3.0](../../LICENSE)

LLVM 구성 요소는 Apache-2.0 WITH LLVM-exception 라이선스를 유지합니다. Capstone은 자체 라이선스를 유지합니다.
