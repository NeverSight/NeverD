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

**EVM** 컨트랙트 바이트코드를 동일 IR 스택으로 1:1 리프트하고 C, Solidity-oriented source, LLVM IR를 출력합니다.

### 목표

- EVM loader, 1:1 opcode lifter (strict), 스택/메모리, JUMP/JUMPI CFG, storage/calldata, C23/Solidity/LLVM, 통일 CLI/C API

**상태:** Frontier부터 Fusaka까지 legacy opcode decode/lifting은 완료되어 회귀 테스트로
보호됩니다. source reconstruction은 보수적으로 계속 진행합니다. selector, event, type,
standard, name, dynamic control flow는 evidence가 충분할 때만 보고하며 original source,
완전한 ABI, 완전한 ERC compliance를 주장하지 않습니다. canonical function selector,
standard별 ABI variant, 성공 return shape를 분리하므로 공유 ERC selector가 standard를
지어내거나 호환되지 않는 return type을 빌리지 않습니다. Amsterdam은 explicit opt-in
Review/development target이고 `latest`는 Fusaka입니다. EOFv1/EIP-7692는 일정이 없으며
EIP-3540은 Stagnant라 확정 mainnet behavior로 다루지 않습니다. 자세한 내용은
[EVM 디컴파일](../evm.ko.md)을 참고하세요.

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

## 4. 메모리 안전성 감사와 헌트

리프트된 바이너리에서 힙 수명 결함(누수, 이중 해제, 해제 후 사용)과 위험한 복사 오버플로를 분석하고 구조화된 JSON으로 보고합니다. 증명된 오버플로에는 유계 솔버 모델을 붙입니다. 분석은 형식에 의존하지 않는 IR과 공유 신원 뷰에서 동작하므로 **PE, ELF, Mach-O는 동등한 대상**이며, 자체 기호 실행과 비트벡터 솔버를 재사용합니다. 외부 솔버나 컨테이너는 없습니다.

| 항목 | 설명 |
|------|------|
| `audit` 트랙 | IR 위의 힙 상태 기계 + 탈출 요약: 누수, 이중 해제, 해제 후 사용 |
| `hunt` 트랙 | 싱크 카탈로그 + 인수 사전 필터 + 목적지 용량 + 솔버 증거 |
| 도달 가능성 증거 | 알려진 진입점의 제어 상태, 독립적인 공격자 제어 고정점, 정확한 루트/호출 체인 증거 |
| 신원 계약 | 형식별 싱크 해석(PE IAT, ELF PLT, Mach-O dyld bind)과 PDB / DWARF / MAP 이름 출처 |

**상태:** PE, ELF, Mach-O의 Phase 1 구현이 완료되었습니다. P0은 힙 수명과 위험한 복사에 대한 폐쇄 세계 분석 및 정확한 리터럴 환경 값과 첫 표준 입력 소비를 위한 schema v1의 추가 `process-input-v1` 재생 증거를 포함합니다. 다른 입력 종류는 이유와 함께 재생 불가로 남습니다. P1은 스택/전역 오버플로, 미초기화 로컬 읽기, 형식 문자열을 다룹니다. 알 수 없거나 일부만 적용 가능한 호출 효과는 UNKNOWN입니다. 판정과 신원 커버리지는 [`unittests/safety`](../../unittests/safety)와 모든 호스트에서 필수 PE/ELF/Mach-O × x86-64/AArch64 6셀 fixture 매트릭스를 실행하는 종단 간 [`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp)로 고정됩니다. 자세한 내용은 [메모리 안전성 감사와 헌트](../memory-safety.ko.md).

현재 프로시저 간 슬라이스는 독립적인 `verdict`를 바꾸지 않고 schema v1에
`reachability.status`와 `reachability.attacker_control`을 추가합니다.
`application`, `image`, `export` 루트, 정확한 내부 호출 체인, fail-closed UNKNOWN
상태를 보고합니다. `max_call_depth`와 `max_summary_iterations` 예산은 C API, 두 CLI
명령, 두 Python 메서드에서 설정할 수 있습니다. 따라서 `control_reachable`과
`attacker_reachable`은 도달
가능성 집계이며 별도의 판정 집계가 아닙니다.

P2 분석 표면과 계획은 명시적 상태가 있는 버전 경계를 사용합니다.

| 계획 | 범위 | 상태 |
|------|------|------|
| `lowir-concolic-v1` | LowIR 하이브리드/concolic 탐색과 seed 생성 | 실험적; PE/ELF/Mach-O × x86-64/AArch64에서 재생 검증된 레지스터 seed |
| `binary-sanitizer-v1` | 다시 쓴 네이티브 바이너리에 삽입하는 런타임 검사 | Darwin에서 실험적: 모든 지점을 보호하지 못하면 거부하는 counted-write 가드와 인증된 create-exclusive 또는 동일 source no-change 게시 |
| `process-replay-v1` | argv, 파일, 네트워크, 반복 read를 포함하는 `process-input-v1`보다 넓은 프로세스 재생 | Phase 0 경계만 제공: plan/coordinator 검증과 fail-closed 네이티브 가용성 질의; 네이티브 replay 작업을 제공하는 호스트는 없음 |

concolic 어댑터는 별도의 분석 표면이며 Phase 1 안전성 보고서 승인 계약의 확장이 아닙니다. 실험적 sanitizer는 `neverd_session_sanitize`, `neverd patch --sanitize=strict`, Python `Session.sanitize`로 제공되며 Darwin이 아닌 호스트에서는 lifting이나 namespace 변경 전에 거부합니다. 완전한 receipt는 트랜잭션 동안 보유한 대상 디렉터리 object만 인증합니다. 디렉터리는 open 후 이름이 바뀔 수 있으므로 원래 pathname이 처리 중 또는 반환 후에도 그 object를 가리킨다는 사실이나 영구 path binding을 증명하지 않습니다. `NativeProcessReplayAdapter`는 계속해서 능력이 전부 있거나 전혀 없는 Phase 0 query/factory 경계이며, 현재 모든 호스트가 모든 능력을 false로 보고하고 작업 table을 반환하지 않습니다.

---

## 5. 엔진·제품 강화 (지속)

| 영역 | 방향 |
|------|------|
| Lifter 커버리지 | strict 유지하며 네이티브 공백 축소 |
| 시맨틱 테스트 | 새 ISA와 함께 확장 |
| 플러그인 ABI | [네이티브 플러그인 ABI](../plugins.ko.md)를 프로세스 내 extension contract로 유지. Loader와 UI 값은 명시적 host API가 생길 때까지 metadata일 뿐임 |
| 문서/행렬 | 테스트 통과 후에만 README 갱신 |

---

## 일정

네이티브 포맷, Fusaka까지의 legacy EVM decode/lifting, Solana SBF, 메모리 안전성
Phase 1과 현재 알려진 진입점 도달 가능성 슬라이스는 회귀 테스트로 보호됩니다. 보수적인
EVM source reconstruction은 계속 진행 중입니다. 출시일을 약속하지 않습니다.

| 기능 | 상태 |
|------|------|
| 네이티브 포맷 완성 (PE ARM*, Mach-O i386) | 완료 |
| EVM legacy decode/lifting | Fusaka까지 완료; 회귀 테스트 적용 |
| EVM source reconstruction | 진행 중 — evidence-backed, 보수적 |
| Solana eBPF (SBF) 디컴파일 | 완료 — v0-v4, C, Rust, LLVM; 회귀 테스트 완료 |
| 메모리 안전성 감사와 헌트 | Phase 1 및 알려진 진입점 도달 가능성 슬라이스 완료; `lowir-concolic-v1`과 Darwin `binary-sanitizer-v1`은 실험적; 네이티브 `process-replay-v1`은 fail-closed Phase 0 어댑터 뒤에서 아직 제공되지 않음 |
| 엔진·제품 강화 | 지속 |
