**언어**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<!-- i18n-source: 7d465e53d8b02df92cfd1d84c9817ee79377e7094f58b046232e3ea5523fe1e5 -->

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**AI 친화적인 바이너리 분석·디컴파일 엔진 — 1:1 리프트, LLVM 기반**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; C + Python SDK

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#빌드)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk와-플러그인)

[문서](README.md) · [Android](android.md) · [iOS](ios.md) · [로드맵](roadmap.md) · [기여](CONTRIBUTING.md)

</div>

---

> GitHub 저장소 홈은 항상 영어 `README.md`를 표시합니다. 위 언어 링크로 지역화 버전을 보세요.

<!-- i18n-section: overview -->

## 개요

NeverD는 **1:1 명령어 수준 리프트**를 중심으로 한 네이티브 및 스마트 컨트랙트 분석·디컴파일 엔진입니다. **PE**, **ELF**, **Mach-O**, legacy **EVM** bytecode와 Solana **SBF ELF** program을 로드합니다. native target은 [Capstone](https://www.capstone-engine.org/)으로 decode하고 EVM/SBF는 전용 version-aware decoder와 staged IR을 사용합니다. 모든 경로는 hand-written semantics입니다. 지원 instruction은 **LLVM IR**, **C**, **SBF Rust**, **EVM Solidity reconstruction**, 또는 native의 **재작성 binary**에서 observable behavior를 보존합니다.

**strict는 기본 ON**입니다. lifter가 없는 명령어는 `UnliftedInstruction`을 던지며, 건너뛰기·추측·조용한 `NOP` 변환을 하지 않습니다.

CLI, 통합, AI 에이전트는 **순수 C API**로 동일한 엔진 **`libneverd`**를 사용하며 Capstone, LLVM, 내부 C++에 직접 링크하지 않습니다.

input format, host contract와 제한은 [EVM 가이드](evm.md)와 [Solana SBF 가이드](sbf.md)를 참고하세요.

실험적 CLI `neverd mobile app.apk -o recovered-app`는 APK, DEX, smali에서 Java와 `report.json`을 생성합니다. Android 복원은 NeverD의 C++20 내장 엔진만 사용하며 Python 또는 Java 런타임이 필요하지 않습니다. 공백이 있는 경로는 따옴표로 감싸세요. 지원 입력, 보고서 및 복구 제한은 [Android 가이드](android.md)를 참조하세요.

실험적 iOS 흐름 `neverd mobile App.ipa -o recovered-ios`는 IPA, `.app`, Mach-O에서 네이티브 C와 지원되는 Objective-C/Swift 소스를 출력합니다. 런타임 배치, 소스 단위, 메서드별 생략 이유를 유지하며 원래 바이너리를 호출하는 브리지를 사용하지 않습니다. 설정, 범위 계산, 독립 컴파일 검증은 [iOS 가이드](ios.md)를 참조하세요.

실험적인 [인터프리터 소스 복원](interpreter-recovery.md)은 `neverd decompile --devirtualize --func ENTRY`를 사용하여 지원하는 링크된 x64 ELF/PE 인터프리터를 공통 LowIR/MedIR 파이프라인을 거쳐 HighC 또는 LLVMC로 특수화합니다. 제어 힌트는 디코더 컨텍스트를 구분하며 실행 시 입력을 고정하지 않습니다. 미해결 제어 흐름, 미지원 의미론, 예산 소진은 명시적으로 실패합니다. 이 모드는 바이너리 교체나 예외 동등성을 증명하지 않습니다.

복구 예산은 `--vm-max-fields`, `--vm-max-refinements`, `--vm-max-queries`로 명시하며 기본값은 16, 16, 4096으로 유지됩니다. 호환되는 v3 C API와 실패 규칙은 복구 가이드를 참고하세요.

복원은 `--vm-chain-transfers=N`(기본값 0)과 `--vm-no-control-discovery`도 제공합니다. 연결은 단일 대상이 증명된 제어 전송 사이의 기호 상관관계를 보존하며 한도에 도달하면 일반 CFG 경계로 돌아갑니다. 기계 상태 복원은 `--vm-entry-frame=begin:end`로 실행 시 검사하지 않는 비래핑 진입 RSP 오프셋 범위를 선언할 수 있습니다. 정확한 숫자 전제는 생성 C와 보고서에 남으며 메모리 접근 권한이나 동등성 증명을 제공하지 않습니다.

머신 상태 복원은 `--vm-entry-alignment=A:R`로 진입 RSP 합동 조건을 명시하고 실행 시 검사합니다. `A`는 양의 2의 거듭제곱이며 `R < A`여야 합니다. 다른 진입 값은 게스트 메모리 접근이나 상태 쓰기 전에 상태 2를 반환합니다. 주소 상위 비트는 자유롭고 기본값은 정렬을 가정하지 않습니다. 이 옵션은 네이티브 동등성 인증을 제공하지 않습니다.

`--vm-external-stores-disjoint-frame`은 기계 상태 복원에 명시적이며 런타임에 검사하지 않는 전제를 추가합니다. 외부 STORE의 전체 범위가 `--vm-entry-frame`과 겹치지 않아야 하며 그 범위 안의 기존 사실만 유지합니다. LOAD나 외부 포인터 간 별칭은 제한하지 않고 기본 동작은 보수적으로 유지됩니다. 네이티브 증명 API는 이 도메인을 거부합니다.

SSA 구성 한도를 넘는 큰 복원 함수는 `--llvm`을 통해 제한된 스칼라 가변 저장소 계약을 사용할 수 있습니다. 진입 입력, 루프에서 전달되는 값, 앞선 읽기의 의미를 보존합니다. 지원하지 않는 암시적 상태, 벡터 레지스터 매개변수, 이미지 재배치, 모호한 저장소와 잘못된 제어 흐름은 명시적으로 실패하며 HighC는 이 대체 경로를 거부합니다. 소스 출력은 기존 기계 상태 계약을 따르며 동등성 인증서를 추가하지 않습니다.

별도의 C++ 루프 증명 API는 예산 내에서 중첩 루프 불변식과 사전식 순위를 추론한 후 네이티브 코드와 LowIR의 정제 관계를 다시 검사합니다. [복원 가이드](interpreter-recovery.md)를 참조하세요. 출력 C의 동등성은 증명하지 않습니다.

별도의 C++ `checkBinaryLLVMRefinement` API는 정확한 LLVM 산출물에 대해 새로운 네이티브 및 LLVM 검사를 조합합니다. C 컴파일은 증명 범위 밖입니다.

PE 복구는 기본 베이스의 DIR64 바이트도 인증하고 가져오기 쓰기를 제외합니다. 고정 이미지 계약은 ASLR이나 초기화의 동등성을 증명하지 않습니다.

명시적 머신 상태 계약에서 제한된 진입 스택 정렬 분할과 내부 `RET imm16` 스택 정리를 복구합니다. 이 분할의 자동 native-to-LLVM 증명 합성은 아직 구현되지 않았습니다.

제한된 `REP MOVS/STOS` 복구는 요소 순서와 겹침 동작을 보존하며, 원본 명령 증명 지원은 아직 완료되지 않았습니다.

<!-- i18n-section: why-neverd -->

## 왜 NeverD인가?

- **1:1 의미론** — 손수 작성 lifter; 기본 strict에서 미지원 명령어는 예외
- **LLM 친화적** — 구조화 C, LLVM IR, JSON 분석을 순수 C API로 노출하며 오류는 결정적
- **하나의 파이프라인, 여러 출구** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → 네이티브 바이너리 재작성
- **바이너리 재작성** — PE / ELF / Mach-O, section 트램폴린 또는 inplace
- **분석 도구 모음** — CLI, 디버그 정보, 시그니처, 플러그인, 선택적 난독화 패스

<!-- i18n-section: supported-targets -->

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

<!-- i18n-section: mobile-source-recovery -->

### 모바일 소스 복원

실험적 `neverd mobile` CLI는 다음 모바일 입력과 소스 출력을 지원합니다.

| 플랫폼 | 입력 | 출력 |
|--------|------|------|
| [Android](android.md) | APK(multidex 포함), DEX, smali 파일/디렉터리 | Java 및 JSON 보고서 |
| [iOS](ios.md) | IPA, `.app`, Mach-O(arm64 / x86_64) | 네이티브 C, 지원되는 Objective-C / Swift 소스 및 JSON 커버리지 보고서 |

복원은 지원하는 코드 패턴에 따라 달라집니다. 범위와 제한은 [모바일 개요(영어)](../mobile.md) 및 플랫폼별 가이드를 참고하세요.

<!-- i18n-section: cpu-workloads -->

### CPU 실행과 게스트 환경

CPU 실행은 ISA 허용, 게스트 메모리, 백엔드 전송과 게스트 OS 정책을 분리합니다. `NEVERD_ENABLE_CPU_EMULATION`은 x64/ARM64 CPU 계층을 켜고 `NEVERD_ENABLE_DRIVER_EMULATION`은 제한된 x64 Windows WDM/KMDF 환경을 추가합니다. `linux-elf64-v1`은 지원되는 Linux ELF 프로세스를 실행합니다. [CPU 실행](cpu-execution.md), [게스트 프로세스 에뮬레이션](process-emulation.md), [Windows 드라이버 에뮬레이션](driver-emulation.md)를 참조하세요.

`windows-pe64-v1`은 PEB/TEB, 정적·동적 TLS, `DllMain`, 이름 기반 Win32 API 및 명시적 비순환 DLL 그래프를 갖춘 제한된 Windows x64/ARM64 콘솔 프로세스를 지원합니다. 게스트 모듈은 이름/서수 코드·데이터 가져오기, DIR64 재배치, 전달 내보내기 및 실제 로더 목록 식별자를 지원합니다. `LoadLibraryA` / `LoadLibraryW`, `FreeLibrary`, `GetProcAddress`는 설정된 모듈 카탈로그를 사용합니다. CRT/GUI, 사용자 SEH, 스레드 및 일반 Windows 앱 호환성은 미완성이며 네이티브 ARM64 KVM/WHP 증거도 아직 없습니다.

`GetEnvironmentVariableW`, `SetEnvironmentVariableW`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `ExpandEnvironmentStringsW` 는 PEB 프로세스 매개변수의 실제 게스트 환경 블록을 공유합니다. 이름은 대소문자를 구분하지 않는 ASCII이며 값은 UTF-16입니다. 변경 전에 입력, 용량, 쓰기 가능한 메모리를 검증합니다. 스냅샷은 이후 변경과 독립적이며 해제하면 게스트 메모리를 회수합니다. 모델의 블록 한도는 64 KiB이고 문자열과 확장에는 크기 및 실행 기한 검사가 적용됩니다. 알 수 없는 포인터 소유권, 잘못된 블록, ANSI 코드 페이지, 확장 버퍼 중첩은 지원하지 않습니다. `WindowsEnvironmentTests.cpp`는 사용 가능한 백엔드에서 자체 x64/ARM64 픽스처를 비교하며 CI는 독립적인 네이티브 Windows 오라클을 필수로 실행합니다.

Windows 가상 메모리는 `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery`와 현재 프로세스의 `FlushInstructionCache`를 지원합니다. OS 계층은 예약 영역을 소유하고 `AddressSpace`는 커밋된 페이지, 권한, 실제 저장 공간을 관리합니다. 테스트는 동적 코드 수정, 접근 오류, 메모리 한도 재사용을 검증합니다.

`driver-strict` / `checked-x64-v1`는 일치하는 Linux x64 host의 KVM과 Windows x64 host의 WHP를 지원합니다. `auto`는 해당 native transport를, cross-ISA는 Unicorn을 선택합니다. 명시적 Unicorn과 기존 V1 API는 portable software profile을 유지합니다. native 실행은 진입 전에 canonical address와 instruction effect를 검증하고, hardware가 없으면 fallback 없이 실패합니다. 지원되지 않는 instruction/OS behavior는 명시적 오류입니다. Windows x64 네이티브 CI는 Unicorn을 비활성화하고 필수 검사 359개를 모두 통과합니다. CPU 검사 131개, 내장 이미지 26개·WDK 이미지 46개·시나리오 사례 40개를 기본 및 재배치 주소에서 실행한 드라이버 결과 224개, SEH 경계 검사 4개를 포함합니다 ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). native ARM64 실기 증거는 아직 없으며, 임의 driver나 Android/Darwin 호환성을 의미하지 않습니다.

검사형 x64는 일반 RAM의 `MOVS/STOS/LODS`와 `CLD/STD`를 지원하며 요소별 재개, 취소와 페이지 경계를 검사합니다. CPU별로 다른 0회 반복 상위 비트와 STOS/LODS 장치 피연산자는 계약 범위 밖입니다.

검사형 x64는 일반 RAM의 `CMPS/SCAS`와 `REPE/REPNE`도 지원하며 산술 플래그, 조기 종료, 요소별 중지와 오류 복구를 처리합니다. 장치 비교는 아직 지원하지 않습니다.

`checked-aarch64-v1`와 `checked-user-aarch64-v1`는 제한된 ARM64 FP32/FP64, 고정 폭 SIMD와 전체 FPCR/FPSR/벡터 상태를 제공합니다. ISA가 일치하는 Linux ARM64는 KVM, Windows ARM64는 WHP, 다른 ISA는 Unicorn을 사용합니다. 네이티브 ARM64 실기 검증은 남아 있으며 Windows 드라이버 로딩은 x64로 제한됩니다.

x64와 ARM64 네이티브 시작 검사는 독점 메모리 임대하에서 제한된 전체 상태 실행을 검증합니다. XSAVE 패킷과 ISA를 식별하는 페이지 테이블 캐시는 하나의 권한 계층이 관리하며 네이티브 ARM64 작업 증거는 아직 불완전합니다.

네이티브 x64 `FOP/FIP/FDP`는 호스트 저장·복원 규칙을 따르며 AMD는 비활성 x87 예외 메타데이터를 0으로 만들 수 있습니다. 시작 검사는 마스크되지 않은 대기 예외로 이 필드를 검증합니다.

<!-- i18n-section: how-it-works -->

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

<!-- i18n-section: quick-start -->

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

# Android / iOS 소스 복원 (실험적)
./build/bin/neverd mobile app.apk -o recovered-android
./build/bin/neverd mobile App.ipa -o recovered-ios

# 분석
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

빌드 시 시그니처 라이브러리는 `build/bin/signatures/`에 설치됩니다. `sigs --auto`는 포맷·아키텍처·비트 너비로 세트를 고릅니다. PE 파일의 Rich 헤더가 링커의 Visual Studio 릴리스를 알려 주면 그 릴리스의 `vs<year>.pat`와 어느 릴리스에도 속하지 않는 파일만 불러옵니다. `--sig-base <dir>`는 다른 시그니처 트리에서 같은 방식으로 고릅니다. 1 MiB 이상인 패턴 파일은 한 번만 파싱됩니다. 그 모듈은 사용자 캐시 디렉터리의 `neverd/signatures`에 보관되고 이후 로드에서는 매핑됩니다. `NEVERD_SIGNATURE_CACHE`로 다른 디렉터리를 지정하거나 `off`로 캐시를 끌 수 있습니다.

<!-- i18n-section: building -->

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r3
cmake --build build
```

NeverD의 일반 push 및 pull request CI는 의도적으로 LLVM submodule을 소스에서 빌드합니다. `CI` 워크플로를 수동 실행할 때 `use_prebuilt_llvm`을 선택하면 공개된 패키지를 검증할 수 있습니다. 사전 빌드 LLVM은 수동으로 `true`를 선택했을 때만 켜지며, 선택하지 않으면 자동 CI와 같은 소스 빌드 경로를 유지합니다.

공개 패키지는 CMake를 실행하는 호스트에 따라 선택됩니다:

| 호스트 | 릴리스 자산 |
|--------|-------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

각 아카이브는 `~/.cache/neverd-llvm/<tag>/<arch>/` 또는 `NEVERD_LLVM_PREBUILT_CACHE_DIR` 경로에 압축을 풀기 전에 `cmake/NeverDLLVMPrebuilt.cmake`에 고정된 다이제스트로 검증됩니다. 고정 정보에 없는 태그는 게시된 `.sha256`을 사용합니다. 기본 고정 버전은 `BUILDINFO.txt`의 LLVM 서브모듈 커밋도 정확히 일치해야 합니다. 릴리스 빌드는 macOS/Linux에서 ccache, Windows clang-cl에서 sccache와 GitHub Actions 캐시를 사용합니다. 컴파일러 캐시는 재빌드만 가속하며 릴리스 자산으로 게시하지 않습니다.

기본 패키지 리비전은 `neverd-llvm-v23.0.0-r3`입니다. Git 태그, 릴리스 대상, 소스 커밋, 아카이브 세 개의 다이제스트를 하나의 변경 불가 소스 리비전으로 고정합니다. 이전 기본 태그, `neverd-llvm-v23.0.0-r1`, `neverd-llvm-v23.0.0-r2`를 캐시한 빌드 디렉터리는 명시적인 `NEVERD_LLVM_PREBUILT_SHA256`이 없으면 자동으로 `r3`로 이동합니다. `Prebuilt LLVM Audit`는 push, pull request 및 6시간마다 실행되고, `scripts/audit_prebuilt_llvm_release.py`가 고정 정보와 현재 GitHub 릴리스 및 각 체크섬 파일을 비교합니다.

LLVM fork가 변경되었지만 LLVM 버전이 여전히 `23.0.0`이면 다음 패키지 리비전인 `neverd-llvm-v23.0.0-r4`, 이후 `-r5`를 게시합니다. 기존 릴리스를 덮어쓰거나 LLVM 버전 `23.0.1`을 임의로 만들지 않습니다.

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r4 \
  -f overwrite_existing_assets=false
```

워크플로 성공 후 `cmake/NeverDLLVMPrebuilt.cmake`의 기본 태그, 고정 커밋, 다이제스트 세 개를 함께 갱신합니다. 새 패키지는 `.cache/neverd-llvm/<tag>` 아래에 캐시되며 오래되거나 다시 게시된 아카이브는 압축 해제 전에 실패합니다. `overwrite_existing_assets`는 이전 릴리스 복구 전용이며 일반 리비전 게시에서는 끕니다.

**산출물**

| 경로 | 설명 |
|------|------|
| `build/bin/neverd` | 통합 CLI |
| `build/bin/neverd-bench` | 벤치마크(JSON) |
| `build/bin/neverd-sigmaker` | 정적 라이브러리에서 `.pat` 생성 |
| `build/bin/libneverd.*` | 엔진 공유 라이브러리 |
| `build/bin/sdk/` | C SDK의 canonical include root. `neverd/sdk/` 계층을 유지한 `<neverd/sdk/NeverDCAPI.h>` 또는 `<neverd/sdk/NeverDPlugin.h>` 사용 |
| `build/bin/sdk/python/` | 타입 정보를 제공하는 Python 플러그인 패키지와 예제 |
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
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | CPython 3.10+ 플러그인 지원 내장 |
| `NEVERD_BUILD_PLUGINS` | `OFF` | 예제 플러그인 |
| `BUILD_TESTING` | `OFF` | 단위 테스트 |
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | Unicorn에 의존하는 의미론 테스트 그룹 (`BUILD_TESTING=ON`일 때) |

</details>

<!-- i18n-section: desktop-workbench -->

## 데스크톱 워크벤치

선택적인 [Qt Quick 데스크톱 워크벤치 (영문)](../gui.md)는 도킹 가능한 명령어·CFG·16진수·C·IR 보기, 11개 UI 언어, 주석 저장, MCP 연결을 제공합니다. 분석은 Qt에 의존하지 않는 별도 작업 프로세스에서 실행되며 CLI 전용 빌드는 독립적으로 유지됩니다. 지원하는 작업 흐름과 출시 전에 남아 있는 플랫폼 검증은 [검증 기록 (영문)](../gui-qualification.md)을 참고하세요.

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### 파이프라인

| 명령 | 출력 | 설명 |
|------|------|------|
| `lift` | `.ll` | LLVM IR로 리프트 |
| `decompile` | `.c` / `.sol` / `.rs` | `--language`로 C, EVM Solidity 또는 SBF Rust 선택 |
| `decompile -llvm` | `.c` | LLVM IR + 최적화 경로 |
| `decompile --devirtualize` | `.c` + 선택적 JSON | 실험적 x64 인터프리터 복원. `--func` 필수. [계약과 예제](interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | 실험적: [Android](android.md), [iOS](ios.md) |
| `patch` | 바이너리 | 기계어 재작성 |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

32비트 ARM 바이너리에 함수의 ARM/Thumb 모드 메타데이터가 없다면 디컴파일 전에 진입 모드를 지정합니다.

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

모호한 다른 진입점에는 옵션을 반복해서 사용하고 필요하면 `:arm`을 지정합니다. 검증된 바이너리 메타데이터와 충돌하는 지정은 로드를 실패시키며, 유효한 지정은 해당 진입점에만 적용됩니다. C API는 `neverd_session_set_arm_function_mode()`로 같은 로드 전 설정을 제공합니다.

<details>
<summary><strong>분석 명령</strong></summary>

<br>

| 명령 | 용도 |
|------|------|
| `info` / `dashboard` / `headers` | 메타데이터와 개요 |
| `funcs` | 발견된 함수 |
| `disasm` | 디스어셈블(`--func` 이름 또는 hex) |
| `sym-explore` | 제한된 네이티브 LowIR 경로 탐색(`--func`, JSON 출력) |
| `audit` | 힙 객체 수명 결함 및 초기화되지 않은 로컬 스택 읽기（JSON） |
| `hunt` | 위험한 복사 범위 초과와 기호 증거. 완전한 계획이 있으면 `process-input-v1` 재생 증거 추가（JSON schema v1） |
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

<!-- i18n-section: sdk-and-plugins -->

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

<!-- i18n-section: dependencies -->

## 의존성

| 구성 요소 | 역할 | 소스 |
|-----------|------|------|
| **LLVM**(fork) | IR, 최적화, 코드 생성, 진단 | `third_party/llvm-project` 또는 사전 빌드 |
| **Capstone** | 디코드 | `third_party/capstone` |

서드파티 구성 요소는 자체 라이선스를 유지합니다.

<!-- i18n-section: contributing -->

## 기여

개발 결과는 **`dev`** 브랜치에 통합합니다. 환경 설정, Release/Debug 지침, 스타일, 집중 테스트, 풀 리퀘스트 요구사항은 [기여 가이드](CONTRIBUTING.md)를 참고하세요. [아키텍처](architecture.md)와 [테스트](testing.md) 가이드는 일반적인 변경을 관련 코드 및 검증 스위트에 연결합니다.

<!-- i18n-section: license -->

## 라이선스

[GNU AGPL 버전 3 전용](../../LICENSE). 해당 NeverD 코드나 파생물을 재배포할 때 저작권, 라이선스, 보증 부인 고지와 [NOTICE](../../NOTICE)의 프로젝트 귀속 및 출처를 유지해야 합니다. AI/LLM을 이용한 재사용 및 LLVM 기반 코드 변환에도 적용됩니다.

요구 사항, 범위, 예시는 [귀속 및 인용](ATTRIBUTION.md)을 참고하세요. 추적 가능한 참조에는 소스 파일과 정확한 버전 또는 커밋을 인용하는 것을 권장합니다. [CITATION.cff](../../CITATION.cff)는 소프트웨어 인용 메타데이터를 제공하지만 인용만으로 라이선스 준수를 대체할 수는 없습니다.

LLVM 구성 요소는 Apache-2.0 WITH LLVM-exception 라이선스를 유지합니다. Capstone은 자체 라이선스를 유지합니다.
