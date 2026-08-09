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

EVM과 Solana는 연구·설계 단계에 있으며, 네이티브 포맷 완성은 회귀 테스트로 검증되었습니다. 출시일을 약속하지 않습니다.

| 기능 | 상태 |
|------|------|
| 네이티브 포맷 완성 (PE ARM*, Mach-O i386) | 완료 |
| EVM 바이트코드 디컴파일 | 연구 / 설계 |
| Solana eBPF (SBF) 디컴파일 | 연구 / 설계 |
| 엔진·제품 강화 | 지속 |
