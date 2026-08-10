**언어**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← 문서 색인](../README.ko.md)

# NeverD 로드맵

이 문서는 현재 네이티브 PE / ELF / Mach-O 파이프라인을 넘어선 NeverD의 주요 계획을 정리합니다. 원칙은 동일합니다: **1:1 명령 수준 리프트**, **strict fail-loud**, 공통 **4단계 IR**.

---

## 1. 네이티브 포맷 완성

로더가 부분적으로 인식하지만 포맷 수준 종단간이 아직 안 된 대상을 마무리합니다.

| 항목 | 설명 |
|------|------|
| PE AArch64 | Windows ARM64: unwind/`.pdata`, 트램펄린, rewrite 왕복 |
| PE ARM32 (Thumb-2) | Windows on ARM은 Thumb 전용 |
| Mach-O i386 | 일반 clang 재배치; thin object 우선 |

### 설계 원칙

- 포맷 수준 테스트 통과 전에는 지원 행렬에 지원으로 표시하지 않음
- 기존 ELF / PE x86 / Mach-O arm64+x64 동작 유지
- 이미지 수준 명령 모드 선호

---

## 2. EVM 바이트코드 디컴파일

**EVM** 컨트랙트 바이트코드를 동일 IR 스택으로 1:1 리프트하고 구조화 C / LLVM IR를 출력합니다.

### 목표

- EVM loader, 1:1 opcode lifter (strict), 스택/메모리 모델, JUMP/JUMPI CFG, storage/calldata, 기존 HighIR/LLVM-C 경로, 통일 CLI/C API

### 왜 EVM인가

- 감사에 충실한 복원 필요 · 네이티브와 엔진 공유 · 미지원을 조용히 건너뛰지 않음

---

## 3. Solana eBPF (SBF) 디컴파일

**Solana eBPF / SBF** 온체인 프로그램을 동일 strict 시맨틱으로 디컴파일합니다.

### 목표

- SBF loader · 1:1 eBPF/SBF lifter · Account/CPI 인지 · 동일 파이프라인 · 통일 API

**상태:** 현재 Anza `sbpf` v0-v4 계약 지원이 완료되었습니다. 레거시 section/relocation ELF와 엄격한 program-header-only ELF, 완전한 버전별 명령 데이터베이스, 엄격한 검증, 단계별 Low/Med/High IR, syscall/CPI/account 관찰, 검증된 LLVM, 이식 가능한 C11, 안전한 stable Rust, CLI/C API 통합, 독립적이고 범위가 제한된 raw-bytecode 시맨틱 oracle을 지원합니다. v4는 upstream을 추적하지만 특정 클러스터에서 배포·실행할 수 있는지는 해당 클러스터의 feature activation에 따라 달라집니다. 자세한 내용은 [Solana SBF 디컴파일](../sbf.ko.md)을 참조하세요.

### 왜 Solana eBPF인가

- EVM과 함께 주요 감사 대상 · BPF형 ISA가 MedIR에 적합 · 하나의 C SDK

---

## 4. 엔진·제품 강화 (지속)

| 영역 | 방향 |
|------|------|
| Lifter 커버리지 | strict 유지하며 네이티브 공백 축소 |
| 시맨틱 테스트 | 새 ISA와 함께 확장 |
| 플러그인 ABI | 새 포맷에 적합하면 플러그인화 |
| 문서/행렬 | 테스트 통과 후에만 README 갱신 |

---

## 일정

Solana SBF 디컴파일과 네이티브 포맷 완성은 구현 및 회귀 테스트가 완료되었습니다. EVM은 연구·설계 단계입니다. 출시일을 약속하지 않습니다.

| 기능 | 상태 |
|------|------|
| 네이티브 포맷 완성 (PE ARM*, Mach-O i386) | 완료 |
| EVM 바이트코드 디컴파일 | 연구 / 설계 |
| Solana eBPF (SBF) 디컴파일 | 완료 — v0-v4, C, Rust, LLVM; 회귀 테스트 완료 |
| 엔진·제품 강화 | 지속 |
