**언어**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Solana SBF 디컴파일

[← 문서 색인](README.ko.md)

NeverD는 Solana deploy artifact를 일급 SBF program으로 로드하고 CLI와 `libneverd`로
전체 경로를 제공합니다.

```text
SBF ELF
  → version-aware ELF loader 및 verifier
  → lossless LowIR + CFG
  → normalized MedIR + register facts
  → function, syscall, CPI/account observation, region 복구
       ├─ verified LLVM IR
       ├─ portable C11
       └─ safe stable Rust
```

Solana program을 generic Linux eBPF로 취급하지 않고 현재 Anza `sbpf` VM을 따릅니다.
version/opcode/syscall/relocation/protocol metadata는 `include/neverd/sbf/`의 `.def`
database에 모으며 loader/backend는 generated typed table을 사용합니다.

## 지원 입력 및 VM version

입력은 ELF64 little-endian Solana program(`.so`)입니다.

| SBF | ELF layout | Machine ID | 주요 ISA behavior | 상태 |
|-----|------------|------------|-------------------|------|
| v0 | legacy section/relocation | `EM_BPF`, `EM_SBPF` | virtual gap이 있는 fixed frame, LDDW, legacy memory opcode | legacy |
| v1 | legacy section/relocation | `EM_BPF`, `EM_SBPF` | manually adjusted stack frame | legacy |
| v2 | legacy section/relocation | `EM_BPF`, `EM_SBPF` | PQR arithmetic, 이동된 memory encoding, swapped immediate subtraction, source-register CALLX | legacy, 비단조 |
| v3 | strict program header, dynamic relocation 없음 | `EM_BPF` | static syscall/call, JMP32, destination-register CALLX, bytecode `0x100000000`, rodata zero | 현재 deploy toolchain format |
| v4 | strict program header, dynamic relocation 없음 | `EM_BPF` | v3 ISA와 aligned memory-mapping contract | 현재 upstream `sbpf`; cluster availability는 다를 수 있음 |

version 번호 자체는 명세가 아니므로 `SBFVersionFeatures.def`가 동작 변경을 담고
version 표가 그것들을 조합합니다. 각 레코드는 그 변경을 채택한 SIMD 제안과
`anza-xyz/sbpf`가 같은 질문에 대해 노출하는 predicate를 함께 가집니다. 여러 제안이
하나의 version에 안착하고 하나의 제안이 서로 무관한 여러 가지를 바꾸기 때문입니다.
SIMD-0173은 memory instruction class를 옮기면서 동시에 `lddw`를 폐기하고,
SIMD-0174는 같은 version에서 독립적으로 PQR class를 추가합니다. 제안을 version이
아니라 feature에 기록하는 것이 복구된 version 주장을 그것을 결정한 문서까지
추적 가능하게 하며, 두 `callx` 규칙을 별개의 feature로 두는 이유이기도 합니다.
SIMD-0173은 source register를, SIMD-0377은 destination register를 읽습니다.

v2 변경은 의도적으로 v3에 이어지지 않습니다. feature check는 명시적이며
`version >= N`으로 추측하지 않습니다. 기본 strict mode는 malformed header/range/
alignment, unsupported writable legacy section, invalid continuation/register/
frame-pointer write/branch, version-inactive opcode를 instruction slot과 virtual address로
보고하며 거부합니다.

## 설명이 대상으로 삼는 runtime

ISA version은 file에서 나오지만, 그 밖의 것은 거의 나오지 않습니다. 어떤 syscall이
해석되는지는 chain과 slot에 달려 있고, account 필드가 어느 byte에 놓이는지는 그
프로그램을 소유한 loader에 달려 있으며, entrypoint가 두 번째 인자를 받는지는 chain이
내리는 switch에 달려 있습니다. 그리고 프로그램을 배포할 수 있는지는 그것이 실행되는지와
별개의 질문입니다. 하나의 version switch로는 그중 무엇도 표현할 수 없으므로, 이들은
각자의 표를 가진 별개의 축입니다.

`SBFRuntimeFeatures.def`는 cluster와 purpose, 그리고 NeverD의 보고 내용을 바꾸는 gate를
기록합니다. 각 항목은 runtime identifier, 활성화 상태를 기록하는 feature account, 그리고 각
cluster가 그것을 활성화한 slot을 함께 가집니다. pending account는 존재해도 gate를 켜지 않을
수 있습니다. 어떤 cluster에 활성화 행이 없는
gate는 거기서 아직 활성화되지 않은 것입니다. `simd-0321`은 모든 cluster에서 켜져
있고, `simd-0449`와 SHA-512 syscall은 testnet과 devnet에서 켜져 있고 mainnet에서는
꺼져 있습니다. devnet에서 되는 프로그램이 mainnet에서 실패하는 이유가 바로 이것입니다.

고정된 Agave revision에서 `syscall_parameter_address_restrictions` gate
(`simd-0459`)는 syscall과 CPI 매개변수의 VM 주소 및 정렬 계약을 강화합니다. finalized
RPC 상태가 기록한 활성화 slot은 mainnet 429,840,000, testnet 407,468,256, devnet
462,240,000입니다. `account_data_direct_mapping` gate는 조정된 주소 공간을 사용할 때
account data를 input buffer 복사본에서 직접 backing되는 memory region으로 바꿉니다.
mainnet에서는 활성화되지 않았고 testnet 408,332,256과 devnet 463,968,000에서
활성화되었습니다. 두 gate 모두 새 Account ABI를 만들거나 ABIv0/ABIv1의 논리 field
offset을 바꾸지 않습니다. 직렬화는 여전히 소유 loader가 선택하며 NeverD는 두 gate를
runtime topology metadata로 기록합니다.

feature bit는 append-only를 유지합니다. 관찰 가능한 snapshot이 32 bit를 넘었으므로
`RuntimeFeatureMask`가 storage와 host ABI의 유일한 `uint64_t` 형식입니다.
v2 ABI 폭은 고정되어 in-place로 확장하지 않습니다. 64 bit를 넘는 필드는 v3 또는 multiword 표현을 추가해야 하며 v2 폭을 바꾸지 않습니다.
`RuntimeFeatureDisposition`은 현재 살아 있는 `RuntimeBranch`와, 고정 revision에서는
활성 쪽이 무조건 적용되지만 역사적 slot에서는 이전 쪽도 여전히 중요한
`FoldedBranch`를 구분합니다. finalized RPC 활성화 정보(`—`는 미활성):

| gate | domain / disposition | mainnet | testnet | devnet |
|------|----------------------|---------|---------|--------|
| `disable_deploy_of_alloc_free_syscall` | `ProgramAdmission` / `FoldedBranch` | 209,088,008 | 195,356,264 | 224,208,000 |
| `enable_bpf_loader_set_authority_checked_ix` | `LoaderManagement` / `RuntimeBranch` | 251,424,000 | 247,628,260 | 255,744,000 |
| `remove_bpf_loader_incorrect_program_id` | `LoaderManagement` / `FoldedBranch` | 237,168,000 | 224,300,256 | 247,104,000 |
| `simplify_alt_bn128_syscall_error_codes` | `SyscallSemantics` / `FoldedBranch` | 274,320,000 | 278,300,256 | 308,448,000 |
| `abort_on_invalid_curve` | `SyscallSemantics` / `RuntimeBranch` | 311,904,000 | 300,764,256 | 342,576,000 |
| `deplete_cu_meter_on_vm_failure` | `VMFaultPolicy` / `RuntimeBranch` | 327,888,000 | 319,340,257 | 364,176,000 |
| `fix_alt_bn128_multiplication_input_length` | `SyscallSemantics` / `FoldedBranch` | 361,152,000 | 346,988,256 | 397,440,000 |
| `raise_cpi_nesting_limit_to_8` | `CPIExecution` / `RuntimeBranch` | — | — | — |
| `increase_cpi_account_info_limit` | `CPIExecution` / `FoldedBranch` | 403,056,000 | 385,868,256 | 435,456,000 |
| `poseidon_enforce_padding` | `SyscallSemantics` / `FoldedBranch` | 406,080,000 | 385,868,256 | 438,048,000 |
| `fix_alt_bn128_pairing_length_check` | `SyscallSemantics` / `FoldedBranch` | 406,944,000 | 385,868,256 | 438,480,000 |
| `alt_bn128_little_endian` | `SyscallSemantics` / `RuntimeBranch` | 425,088,000 | 406,604,256 | 456,192,000 |
| `enable_alt_bn128_g2_syscalls` | `SyscallSemantics` / `RuntimeBranch` | 425,520,000 | 406,604,256 | 457,056,000 |
| `loader_v3_minimum_extend_program_size` | `LoaderManagement` / `RuntimeBranch` | 432,864,000 | 416,540,256 | 470,880,000 |

이 범위는 Agave `FeatureSnapshot` 전체를 다룬다고 의도적으로 주장하지 않습니다.
NeverD는 decoding이나 출력 host contract를 직접 바꾸는 loader, verifier, VM,
entry/input, syscall, CPI infrastructure gate만 포함합니다. transaction scheduling,
fees, consensus, transaction-level precompile verification, `CPI target built-in`의
업무 semantics는 `external runtime`의 책임입니다. 해당 built-in을 구현하지 않고
bit만 추가하면 NeverD에 없는 능력을 광고하게 됩니다.

`SBFLoaders.def`는 소유와 직렬화를 기록합니다. 배포와 실행이 같은 답이기를 그만둔 지
여러 해가 지났습니다. `loader-v1`과 `loader-v2`는 자신에게 오는 모든 관리 instruction을
거부하면서 이미 소유한 프로그램은 계속 실행합니다. 그래서 그 직렬화는 지금도 읽을 수
있어야 합니다.

| 로더 | 직렬화 | 배포 | 실행 |
|------|--------|------|------|
| loader-v1 | `abi-v0` | 아니오 | 예 |
| loader-v2 | `abi-v1` | 아니오 | 예 |
| loader-v3 | `abi-v1` | 예 | 예 |
| loader-v4 | `abi-v1` | 아니오 | 아니오 (built-in 제거됨) |

`SBFAccountLayout.def`는 각 직렬화 아래에서 account의 각 필드가 놓이는 자리를 정합니다.
둘은 padding만 다른 것이 아니라 필드 순서 자체가 다릅니다. offset 3에서 unaligned
형식은 account 주소의 첫 byte를 두고 aligned 형식은 executable flag를 두는데, 값
자체는 어느 쪽으로 읽혔는지 알려주지 않습니다. 반복된 account도 `abi-v0`에서는 1
byte, `abi-v1`에서는 8 byte를 차지하므로, 필드 하나가 아니라 항목 전체를 훑는 순회가
어긋납니다.

호출이 해석되는지는 하나가 아니라 세 개의 질문입니다. 그래서
`SBFSyscallLifecycle.def`가 공표된 signature가 얼마나 확정되었는지를 담고,
`SBFSyscallRegistration.def`가 나머지를 담습니다. syscall이 어느 registry에
나타나는지, 어느 gate가 그것을 지배하는지, 그리고 그 gate가 어느 쪽을 가리키는지입니다.
gate는 무언가를 더하는 것만큼 쉽게 빼앗을 수도 있으므로 방향이 중요합니다. fees sysvar
syscall을 없앤 것이 바로 `disable_fees_sysvar`의 활성화였고, 없애는 gate를 더하는
gate로 읽으면 모든 cluster에 대한 답이 한꺼번에 뒤집힙니다. `sol_alloc_free_`는 경계
전후 모두 실행 registry에 등록된 채로 남습니다. deployment는
`disable_deploy_of_alloc_free_syscall` 이전에는 이를 등록하고 cluster별 활성화 slot부터
거부합니다. 고정된 Agave revision은 활성 deployment 쪽을 registry 구성에 fold했지만,
NeverD는 역사적 profile이 활성화 전 답을 얻도록 gate를 보존합니다.

`simd-0321`을 활성화한 runtime에서는 entrypoint가 `r2`로 instruction data의 주소도
받습니다. NeverD는 이를 상수가 아니라 그 자체로 하나의 값 종류로 모델링합니다. 그것이
어디에 놓이는지는 account에 달려 있어서, 주소를 지어내면 그것을 통한 load가 이름 있는
account 필드로 보고될 수 있기 때문입니다. 활성화 이전에는 이 register가 0으로 도착하고,
그것을 읽는 프로그램은 0을 읽습니다. 따라서 생성되는 LLVM, C, Rust entry point는 input
buffer와 instruction data를 함께 받습니다. 두 번째를 건넬 수 없는 callable은 그것을 읽는
프로그램을 재현할 수 없기 때문입니다.

현재 Solana toolchain은 `cargo build-sbf`를 사용하고 v3+ production program은 Rust
중심입니다. upstream C toolchain이 v3를 target하지 않아도 NeverD output은 제한되지 않아
모든 accepted SBF input을 C 또는 Rust로 출력할 수 있습니다.

- [Solana programs](https://solana.com/docs/core/programs)
- [Program execution](https://solana.com/docs/core/programs/program-execution)
- [Syscall reference](https://solana.com/docs/core/programs/syscall-reference)
- [Anza sbpf VM](https://github.com/anza-xyz/sbpf)
- [Agave changelog](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

## CLI

```bash
neverd info program.so
neverd headers --json program.so

neverd lift --dump-low program.so
neverd lift --dump-med program.so
neverd lift --dump-high program.so

neverd lift -o program.ll program.so
neverd decompile --language=c -o program.c program.so
neverd decompile --language=rust -o program.rs program.so

neverd lift --sbf-version=v2 program.so
neverd lift --sbf-relaxed --dump-low program.so

# 답이 어느 runtime에 대한 것인지 밝힙니다. 이 중 무엇도 프로그램 file에 없습니다.
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

`--sbf-cluster`, `--sbf-slot`, `--sbf-loader`, `--sbf-purpose`는 runtime profile을
선택합니다. 기본값은 현재 상태의 mainnet-beta를, `loader-v3` 아래에서, 이미 배포된
프로그램에 대해 기술합니다. 대신 배포를 물으면, chain이 계속 실행해 주기는 해도 그
프로그램을 chain에 올리지 못하게 만들 syscall을 보고합니다.

`--sbf-version=auto|v0|v1|v2|v3|v4`는 감지된 layout 검사를 통과한 뒤 instruction
semantics만 바꾸는 연구 fixture용 option입니다. 신뢰할 수 없는 file을 다른 packaging
standard로 재해석하는 용도가 아닙니다.

## 분석 및 복구

LowIR은 8-byte encoding, raw field, LDDW continuation, resolved call, syscall hash,
block/edge, reachability, diagnostic을 보존합니다. MedIR은 version-specific encoding을
typed 32/64-bit operation, explicit extension, guarded arithmetic, memory width, call kind로
정규화하고 register dataflow는 constant와 stack/rodata address를 추적합니다.

HighIR은 entry/internal function, direct call edge, official syscall name, string, natural
loop, reducible conditional, conservative Solana observation을 복구합니다.
`sol_invoke_signed_rust`/`sol_invoke_signed_c`는 CPI, input register 기반 memory는
account/input access입니다. IDL 없이 Anchor type/account layout을 만들지 않습니다.

C/Rust는 backend-neutral structuring pass를 공유합니다. 유일한 reducible 표현이면
`if`/`if-else`, `while`/`loop`를 출력하고 internal call, CALLX, irreducible flow는 exact
PC dispatcher를 유지하므로 가독성이 semantics를 바꾸지 않습니다.

syscall database는 logging, memory, PDA, SHA-256/Keccak/Blake3, Poseidon, secp256k1,
curve/alt-bn128, big modular exponentiation, CPI, return data, sibling instruction,
compute unit, epoch rewards 등을 포함합니다. legacy relocations `R_BPF_64_64`,
`R_BPF_64_RELATIVE`, `R_BPF_64_32`는 중앙 처리합니다. text relocation은 decode 전
LDDW 두 half와 official Murmur3 CALL key에 적용됩니다. 이미 `R_BPF_64_32`가 적용되고
strip됐다면 symbol/target slot으로 registry key를 다시 계산해 internal call을 복구합니다.

## Solana 프로그램 복원

SBF 머신 모델 위에서 NeverD는 해당 프로그램이 Solana 프로그램으로서 무엇을
의미하는지 보고합니다. 기록된 모든 사실에는 그것을 만든 근거가 함께 붙고,
바이트가 결정하지 않는 것은 추측하지 않고 미설정으로 둡니다.

| 복원 대상 | 근거 |
|-----------|------|
| read-only 데이터의 base58 주소 | `SBFKnownAddresses.def` 및 `SBFAnchorNamespaces.def` 일치, 또는 코드가 만들어내는 상수 |
| 프로그램 자신의 선언 주소 | read-only 상수에 대한 정확히 키 길이만큼의 `sol_memcmp_` |
| Anchor instruction dispatch | 상수가 namespace 붙은 SHA-256 discriminator와 같은 64-bit 비교 |
| CPI 대상 | invoke 인자에서 도달 가능한 instruction 레코드 |
| 호출이 선택하는 연산 | `SBFProgramInstructions.def`에 등재된 selector, 또는 선두의 Anchor discriminator |
| PDA seed | derivation 인자에서 도달 가능한 seed descriptor 배열 |
| account 필드 읽기/쓰기 | 주소가 serialized input 안에 있음이 증명되는 load/store |

loader가 넘기는 인자는 input region 시작의 serialized input buffer 하나뿐이므로,
그 entry state에서의 상수 전파가 raw offset이 아니라 이름 있는 account 필드를
만들어냅니다. `SBFAccountLayout.def`가 공식 직렬화를 담고 있으며, 고정 필드가
빈틈없이 영역을 채우는지 검사합니다.

Anchor는 `<namespace>:<name>`을 SHA-256으로 해싱해 앞 8바이트를 남기는 단방향
방식으로 discriminator를 만듭니다. 그래서 NeverD는 후보 확인만 수행합니다.
`SBFAnchorNames.def`는 배포된 프로그램에서 반복되는 이름 사전이고, `--sbf-idl`은
프로그램 자신의 IDL을 제공하며 우선합니다. 64-bit 비교는 그중 최소 하나가 이름으로
해석될 때에만 discriminator라고 부릅니다.

`SBFKnownAddresses.def`는 프로토콜 및 표준 프로그램 주소를 기록합니다. 각 항목은
정확히 32바이트로 디코딩되어야 하며 테스트가 이를 강제합니다. 복원에는 syscall
ABI도 필요합니다. SBPFv3는 read-only 데이터를 가상 주소 0에 매핑하므로 길이 인자와
낮은 데이터 주소가 같은 값이 됩니다. 따라서 `SBFSyscalls.def`가 어느 인자 레지스터가
VM 주소를 담는지 기록하고, 그것만 추적합니다.

두 invoke syscall은 같은 instruction을 서로 다른 구조로 표현하므로 `SBFCPIABI.def`가
두 레이아웃을 그것을 고르는 syscall별로 보관합니다. 잘못 읽어도 실패하지 않고 첫
account를 호출 대상으로 조용히 잘못 보고할 뿐입니다. `SBFProgramInstructions.def`는
각 프로그램이 스스로 공표한 selector로 연산을 명명합니다. system, stake,
lookup-table, upgradeable-loader는 bincode variant 번호를, token 프로그램은 선두
바이트를 쓰며, 원래 token program과 공유하는 번호 위에 Token-2022의 확장 구간이
얹힙니다. 등재되지 않은 selector는 숫자로 보고합니다.

### scratch 메모리와 syscall 창

프로그램이 runtime에 상수를 그대로 건네는 일은 거의 없습니다. seed 배열, 직렬화된
instruction, 그 payload를 자신의 frame이나 heap에 조립한 뒤 포인터만 넘깁니다.
적재된 image만 읽으면 포인터만 보이므로, 복원은 이 프로그램만 쓸 수 있는 메모리의
바이트 단위 모델을 유지하며 상한은 `kMaxModeledScratchBytes`입니다.

scratch 복구는 demand-driven입니다. Solana CPI/PDA scratch의 fixed point는
실제 `scratch consumer`가 있을 때만 구성하며, 그런 consumer가 없는 프로그램은
`whole-CFG fixed point`를 건너뜁니다. `SBFAnalysisLimits.def`는 호스트의
`analysis policy`를 정의할 뿐 `protocol limits`가 아닙니다. `MaxModeledScratchBytes`는
`program point`마다 1,024 bytes이고, `ScratchFlowRetainedByteBudget`은 8,388,608 bytes의
`logical retained estimate`입니다. 예산을 넘으면 복구는 명시적으로
`ScratchRecoveryPrecision::BlockLocal`로 widening합니다. `cross-block must-facts`만
버리고 `block-local replay`는 `sound`를 유지하며 `same-block stores`도 복구할 수 있습니다. printer는 안정적으로
`recovery scratch-precision=block-local`을 출력하며 widening은
`half-converged must-facts`를 절대 반환하지 않습니다.

호출 뒤에 무엇이 남는지는 두 표가 결정합니다. `SBFSyscalls.def`는 어느 인자
레지스터가 VM 주소를 담는지를, `SBFSyscallMemory.def`는 runtime이 그것을 통해 무엇을
하는지를 `Fixed`, `Counted`, `Opaque` 범위를 가진 read 또는 write로 적습니다. write
창이 없는 syscall은 호출자의 어떤 바이트도 바꿀 수 없으므로 `sol_log_` 이전에 증명된
내용은 그 뒤에도 성립합니다. 길이 인자로 한정된 write는 그 창만 무효화하고, `Opaque`
write는 기준 주소와 그 위를 무효화합니다. 버퍼는 시작점 아래로 자라지 않고 VM region
경계를 넘지도 않기 때문입니다. `SBFSyscalls.def`의 효과 요약과 이 창 표는 양방향으로
검증되어 어느 한쪽만 어긋날 수 없습니다.

`sol_memcpy_`, `sol_memmove_`, `sol_memset_`은 무효화에 그치지 않고 따라갑니다.
목적지와 길이와 원본이 모두 증명되면 목적지 바이트가 알려집니다. Anchor 프로그램의
payload는 매핑이 아니라 복사로 놓이므로, 어떤 연산을 호출하는지는 이 단계에서
드러납니다.

scratch를 보존할 수 있는 것은 해석된 runtime syscall뿐이며, 감사된 write window를
따르는 경우로 한정됩니다. 내부·간접·기타 미해석 call은 현재 scratch를 가리키는 인자가
없어도 모델링된 byte를 지웁니다. 이전에 escape한 pointer나 global alias를 통해
피호출자가 여전히 이를 바꿀 수 있기 때문입니다. `sol_invoke_signed_rust`와
`sol_invoke_signed_c`는 호출자 메모리가 아니라 account data를 쓰므로, 한 block에서
조립한 두 invocation은 모두 읽힙니다.

이 모델은 함수 내 CFG 위의 전방 must 분석입니다. 어떤 block에 이르는 모든 경로가 같은
값을 썼을 때에만 그 바이트가 그 block까지 살아남습니다. 피호출자는 호출자의 frame을
물려받지 않으므로 call edge는 따라가지 않습니다. 의존성 worklist에는 block 수에 따른
정밀도 탈출구가 없으며, 선택형 Release gate가 10 MiB, `1,310,720`개 명령의 전체 상한을
검사합니다.

`SBFLints.def`는 프로그램 전체 관찰을 분류합니다. signer 또는 owner 검사 누락,
상수가 아닌 invoke 대상, deprecated 또는 feature gate 뒤의 syscall, 그리고
SIMD-0500이 배포를 받지 않게 될 SBPF version입니다. 각각 severity와 confidence를
가지며, lint가 디코딩된 의미를 바꾸는 일은 없습니다. 이 계층은 네트워크에 접속하지
않습니다.

## 생성 LLVM runtime contract

LLVM은 VM address를 host pointer로 사용하지 않습니다. checked load/store/syscall
declaration은 `i32` status를 반환하고 load/syscall value는 output pointer에 `i64`로
씁니다. nonzero status는 explicit SBF fault block으로 분기하며 module은
`llvm::verifyModule`을 통과합니다.

## 생성 C host contract

```c
#include <stdint.h>

typedef enum neverd_sbf_status {
  NEVERD_SBF_OK = 0,
  NEVERD_SBF_INVALID_INSTRUCTION = 1,
  NEVERD_SBF_MEMORY_ACCESS = 2,
  NEVERD_SBF_DIVIDE_BY_ZERO = 3,
  NEVERD_SBF_DIVIDE_OVERFLOW = 4,
  NEVERD_SBF_CALL_DEPTH = 5,
  NEVERD_SBF_UNKNOWN_SYSCALL = 6,
  NEVERD_SBF_UNKNOWN_FUNCTION = 7,
  NEVERD_SBF_EXECUTION_OVERRUN = 8,
} neverd_sbf_status;
/* v2 is fixed-width: values 0..8 reuse the legacy constants above. */
typedef uint32_t neverd_sbf_status_v2;
enum {
  NEVERD_SBF_INVALID_REGISTER = 9,
  NEVERD_SBF_INVALID_BRANCH = 10,
};
typedef uint64_t neverd_sbf_runtime_feature_mask;
typedef struct neverd_sbf_runtime_features {
  neverd_sbf_runtime_feature_mask bits;
} neverd_sbf_runtime_features;

/* Generated feature constants have the form NEVERD_SBF_RUNTIME_FEATURE_<Name>. */
typedef struct neverd_sbf_syscall_invocation {
  uint32_t hash;
  uint64_t arguments[5];
  neverd_sbf_runtime_features runtime_features;
} neverd_sbf_syscall_invocation;

/* v1 is the exact legacy four-field ABI. */
/* All callback fields return int, including the v2 callback. */
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  /* Legacy syscall callback: hash, five arguments, output value. */
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;

/* The v1 entrypoint reads only the four fields above. */
neverd_sbf_status neverd_sbf_program(
    neverd_sbf_environment *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);

/* v2 is a distinct ABI: the old layout is embedded and never extended in place. */
typedef struct neverd_sbf_environment_v2 {
  neverd_sbf_environment base;
  /* NULL callback falls back to base.syscall. */
  int (*syscall_with_features)(
      void *, const neverd_sbf_syscall_invocation *, uint64_t *result);
  /* NULL selects the program snapshot; a pointer to zero is an explicit empty snapshot. */
  const neverd_sbf_runtime_features *runtime_features;
} neverd_sbf_environment_v2;

neverd_sbf_status_v2 neverd_sbf_program_v2(
    neverd_sbf_environment_v2 *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);
```

`width`는 bit 단위입니다. 생성되는 모든 C callback은 `int`를 반환하며
`syscall_with_features`도 같습니다. v1 entrypoint `neverd_sbf_program`에서는 0이 성공을
뜻하고, `load` 또는 `store`의 0이 아닌 반환은 `NEVERD_SBF_MEMORY_ACCESS`로,
`syscall`의 0이 아닌 반환은 `NEVERD_SBF_UNKNOWN_SYSCALL`로 정규화됩니다
(`v1-load-store-nonzero`, `v1-syscall-nonzero`). v1은
callback의 exact status를 전달하지 않습니다. 내부 `InvalidRegister`와 `InvalidBranch`도
`NEVERD_SBF_INVALID_INSTRUCTION`으로 정규화됩니다
(`internal-invalid-instruction`).
v2 entrypoint `neverd_sbf_program_v2`는 exact status 경로입니다. 인식된
`neverd_sbf_status_v2` callback 값(9와 10 포함)은 처리된 fault로 보존되며
(`v2-exact-status`), v2 entrypoint는
내부 `InvalidRegister`와 `InvalidBranch`도 9와 10으로 보존합니다. 알 수 없는 callback 값은
생성된 operation-specific fallback(`operation-specific-fallback`)을 사용합니다.
`syscall_with_features`가 null이면 `base.syscall`로 fallback하며, 그 callback도 `int`를
반환합니다(`feature-aware-null-base-syscall`).
v1 struct와 entrypoint는 legacy host와 호환됩니다. 별도 v2 entrypoint를 사용하면
`syscall_with_features`와 해석된 runtime-feature snapshot을 받을 수 있습니다. 생성 source는
register, return PC, callee-saved r6-r9, frame pointer, VM address, division fault, wide PQR
operation, wrapping shift를 표현합니다. 실제 사용하는 helper만 출력하므로 최소 output도
`clang -Wall -Wextra -Werror`를 통과합니다.

## 생성 Rust host contract

```rust
// The v1 source contract remains Result-based.
pub enum SbfError {
    InvalidInstruction, MemoryAccess, DivideByZero, DivideOverflow,
    CallDepth, UnknownSyscall, UnknownFunction, ExecutionOverrun,
}

#[repr(u32)]
#[non_exhaustive]
pub enum SbfErrorV2 {
    InvalidInstruction = 0, MemoryAccess = 1, DivideByZero = 2,
    DivideOverflow = 3, CallDepth = 4, UnknownSyscall = 5,
    UnknownFunction = 6, ExecutionOverrun = 7, InvalidRegister = 8,
    InvalidBranch = 9,
}

pub struct SbfRuntimeFeatures { bits: u64 }
impl SbfRuntimeFeatures {
    pub const fn from_bits(bits: u64) -> Self { Self { bits } }
    pub const fn bits(self) -> u64 { self.bits }
    pub const fn contains(self, feature: Self) -> bool {
        (self.bits & feature.bits) != 0
    }
}

pub struct SbfSyscallInvocation {
    pub hash: u32,
    pub args: [u64; 5],
    pub runtime_features: SbfRuntimeFeatures,
}

pub enum SbfSyscallOutcomeV2 {
    Unregistered,
    Returned(u64),
    Fault(SbfErrorV2),
}

pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}

pub trait SbfEnvironmentV2 {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfErrorV2>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfErrorV2>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfErrorV2> {
        let _ = (hash, args);
        Err(SbfErrorV2::UnknownSyscall)
    }
    fn syscall_outcome(&mut self, hash: u32, args: [u64; 5])
        -> SbfSyscallOutcomeV2 {
        match self.syscall(hash, args) {
            Ok(value) => SbfSyscallOutcomeV2::Returned(value),
            Err(SbfErrorV2::UnknownSyscall) => SbfSyscallOutcomeV2::Unregistered,
            Err(error) => SbfSyscallOutcomeV2::Fault(error),
        }
    }
    // Some(SbfRuntimeFeatures::from_bits(0)) is an explicit empty snapshot.
    fn runtime_features(&self) -> Option<SbfRuntimeFeatures> { None }
    fn syscall_with_features(
        &mut self, invocation: SbfSyscallInvocation
    ) -> SbfSyscallOutcomeV2 {
        self.syscall_outcome(invocation.hash, invocation.args)
    }
}

pub fn neverd_sbf_program<E: SbfEnvironment>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfError> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated program body")
}
pub fn neverd_sbf_program_v2<E: SbfEnvironmentV2>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfErrorV2> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated v2 program body")
}
```

기존 entrypoint `neverd_sbf_program`과 `SbfEnvironment`는
`v1-result-abi`를 이룹니다. host method는 `Result`를 사용합니다.
`Some(SbfRuntimeFeatures::from_bits(0))`은 `explicit-empty-snapshot`을 뜻하며
`None`과 다릅니다. `syscall_outcome`은 Result 기반 host method와
`SbfSyscallOutcomeV2` 사이의 `result-host-bridge`입니다.
`SbfErrorV2`에는 `#[non_exhaustive]`가 붙으므로 호출자는 match에서
`non-exhaustive-wildcard`(`_`)를 사용해야 합니다.

출력은 raw pointer가 없는 safe stable Rust입니다. entry point는 trait generic이고
register/call frame에 fixed-size safe array를 쓰며 test는
`rustc --edition=2021 -D warnings`로 compile합니다.

## C API

SBF load 뒤에도 recovered function, disassembly, IR dump, CFG/call graph JSON, section,
symbol, relocation, string, header의 session operation은 같습니다. Rust는 ABI-stable하게
추가한 output-language enum으로 명시합니다.

```c
neverd_session_t session = neverd_session_create();
neverd_sbf_set_strict(session, 1);
neverd_sbf_set_version(session, "auto");
/* 답이 어느 runtime에 대한 것인지. 기본값은 현재 상태의 mainnet-beta를,
   loader-v3 아래에서, 이미 배포된 프로그램에 대해 기술합니다. */
neverd_sbf_set_cluster(session, "devnet");
neverd_sbf_set_slot(session, 474768000);
neverd_sbf_set_loader(session, "loader-v3");
neverd_sbf_set_purpose(session, "deployment");
/* Optional: name Anchor handlers from the program's own IDL. */
neverd_sbf_set_idl(session, idl_json);
const char *rust = neverd_decompile_all_ex(
    session, "program.so", NEVERD_OUTPUT_RUST, 0, 0);
/* consume rust, then: */
neverd_free_string(rust);
neverd_session_destroy(session);
```

## 검증 및 제한

`unittests/sbf/`는 metadata invariant, v0-v4 loader, strict verifier, CFG/recovery,
verified LLVM, warning-free C/Rust compile, MedIR과 독립된 raw interpreter, public C API를
포함합니다. conditional+loop fixture는 두 언어로 실행해 raw oracle과 비교하며 official
`sbpf` ELF corpus도 vendoring 없이 local compatibility check에 사용합니다.

- SBF binary rewrite와 object-code roundtrip은 명시적으로 거부합니다.
- Anchor IDL/type recovery와 live RPC/account fetch는 loader 범위 밖입니다.
- generated source의 syscall/VM memory는 host contract 경유이며 독립 runtime이 아닙니다.
- relaxed mode는 inspection용이며 invalid instruction에 추측 semantics를 주지 않습니다.

## 현재 conformance baseline (2026-08-24)

relocation 뒤에는 VM address 기반의 단일 immutable `ProgramImage`가 decoder,
interpreter, string recovery, LLVM/C/Rust backend가 공유하는 source of truth입니다.
loader semantics와 달라질 수 있는 별도 text/rodata copy는 없습니다.

닫힌 record는 `SBFVersions.def`, `SBFOpcodes.def`, `SBFRelocations.def`,
`SBFArgumentRegisters.def`, `SBFVersionFeatures.def`, `SBFProtocolLimits.def`, `SBFSyscalls.def`,
`SBFSyscallMemory.def`, `SBFCPIABI.def`, `SBFProgramInstructions.def`,
`SBFUpstreamSources.def`에 둡니다.
한 번만 쓰는 diagnostic과 LLVM block name은 LLVM 자체 관례대로 local에 둡니다.

`SBFProtocolLimits.def`는 과거의 65,536 instruction 값과 현재의 10 MiB
account data 상한을 기록하며, NeverD는 후자에서 보수적인 decode 상한을 유도합니다.

strict v3/v4에서는 bounds-check된 program header가 runtime contract입니다.
section/symbol table은 optional debug enrichment이므로 누락되거나 손상되어도 유효한
image를 무효화하지 않습니다. legacy v0-v2는 `.text`, `.rodata`, `.data.rel.ro`,
`.eh_frame`을 합치고 `R_BPF_64_64`, `R_BPF_64_RELATIVE`, `R_BPF_64_32`을
image가 immutable해지기 전에 정확히 한 번 적용합니다.

| Evidence | 감사 결과 |
|----------|-----------|
| official ELF manifest | `sbpf/tests/elfs` artifact 23/23 |
| official oracle | `NeverDSBFExternalOracleTests`가 pinned verifier와 1,411 opcode/boundary case를 대조 |
| differential execution | raw-byte oracle과 LLVM ORC/C11/stable Rust의 memory/fault/syscall trace 비교 |
| integrated aggregate | `check-neverd-sbf`가 등록된 suite 전체를 실행하며 변동하는 총계는 고정하지 않음 |
| ASan + UBSan | focused target이 fail-fast로 report 없이 실행되며 변동하는 총계는 고정하지 않음 |

감사는 Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`와 Agave
`ef210d67f2fabeee1730498188fa78854260c679`에 pin되어 있습니다. 갱신 시
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def`, `SBFUpstreamSources.def`를
검토하고 다음을 실행합니다.

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

비교 결과 `sol-azy`는 현재 strict ELF에서 crash하고 legacy CFG에 undefined node를
남겼습니다. `solana-data-reverser`는 account data용이고, `SolDragon`은 analysis를
WIP로 표시하며, `bn-ebpf-solana`는 Binary Ninja가 필요합니다. 따라서 official
`sbpf`와 Agave가 semantic authority입니다.

## 2026-08-24 감사 evidence contract

`SBFUpstreamSources.def`는 Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679`, Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`를 고정합니다. official manifest는
23/23을 통과하고, `NeverDSBFExternalOracleTests`는 독립 build한 official verifier와
1,411 opcode/boundary case를 `SBFOfficialOracleProtocol.def` 및
`SBFOfficialVerifierCases.def`와 `SBFOfficialExecutionConstants.def`를 통해 대조합니다. malformed ELF는
`SBFOfficialELFMutations.def`와 table-driven corpus에서 나오며, 자주 변하는 총계는
문서 계약으로 고정하지 않습니다.
별도 축인 `41-case strict ELF 차등 검증`은 strict-v3 matrix 전체를 official
`verify-elf-batch` process와 NeverD에 통과시킵니다. 이 41 case는 1,411
opcode/verifier total에 포함되지 않습니다.

공식 추가 실행 행렬(`additional execution matrix`)은 별도입니다.
정확히 508개의 활성 `(Version,Opcode)` case와 58개의 boundary case, 즉
566개의 exact execution case를 가집니다. 이는 1,411개의 `verifier probes`나
41-case strict ELF 차등 검증을 대체하지 않으며 그 집계에도 포함되지 않습니다.

`NeverDSBFAgaveConformanceTests`는 Firedancer test-vectors revision
`68bb4af40235562e8852fa23d5727e49c2a0b862`도 인증하고, 1,955개의 모든
`sol_compat_elf_loader_v1` fixture(accept 1,399, reject 556)를 대조합니다. accept된 각
ELF에서는 `entry_pc`, `text_off`, `text_cnt`, `rodata_hash`, `calldests_hash`도
비교합니다. Agave의 단계를 섞지 않도록 이 gate는 loader만 검사하고 후속 instruction
verifier는 실행하지 않습니다.

기본 chain profile은 Agave에 정직합니다. `SBF_RUNTIME_VERSION` row가 historical
cluster/slot별 maximum ISA를 official feature account activation에 따라
V0→V1→V2→V3으로 전진시키며 현재 maximum은 V3입니다. 이는
`RuntimeVersionPolicy::ChainProfile`로 처리합니다. 명시적 `--sbf-version=v4`만
`RuntimeVersionPolicy::UpstreamToolchain`을 선택해 pinned `sbpf` 기준 offline 분석을
허용하며, v4가 on-chain에서 활성화됐다고 주장하지 않습니다. 현재 10 MiB 상한은
정확히 `10'485'760` byte입니다. 65,536은 historical provenance/test로만 남고
실행 제한으로 쓰이지 않습니다.

feature, syscall, fault, source ABI의 authority는 typed `.def` registry인
`SBFSyscallRegistration.def`, `SBFValidationRules.def`, `SBFFaultCodes.def`,
`SBFSourceStatuses.def`, `SBFArgumentRegisters.def`, `SBFEdgeKinds.def`입니다.
`SBFFaultCodes.def`는 execution fault의 안정된 값을 소유하고,
`SBFSourceStatuses.def`는 별도 계층인 generated-source ABI를 소유합니다.
loader는 `raw-first`로 relative CALL을 고친 뒤 raw relocation을 ELF ordinal 순서로
한 번만 적용합니다. error order는 text identity, CALL, relocation, entrypoint,
read-only layout으로 고정됩니다. file/VM mapping은 gap-aware이며 gap에 byte를
만들어 내지 않습니다.

CFG/dataflow는 per-function입니다. call edge는 local predecessor가 아니고, shared
tail은 ambiguous로 남으며, 한 loop의 모든 latch는 하나의 multi-latch region을
이룹니다. worklist/ownership은 10,000 function, reverse-order block, conditional
latch fixture로 검증하며 machine별 초 단위 수치는 주장하지 않습니다.

공개 SBF call graph는 `callgraph-budget=fail-closed`를 사용합니다. typed input,
provenance, node, edge, element, `CallGraphOutputByteBudget` 한계로 JSON을
exact-or-empty로 만듭니다. budget이 소진되면 `{"nodes":[],"edges":[]}`를 반환하고
`neverd_last_error()`를 설정하며, 부분 relation은 공개하지 않습니다.

각 activation row는 cluster, feature account, slot을 담으므로 일반 분석을 offline으로
유지한 채 live node와 `RPC activation audit`할 수 있습니다. 비교 대상은 Blueshift,
`qedsvm`(selected path Lean proof, 다만 현재 ELF loader는 V0만 허용),
`leanprover-solanalib`, `sol-azy`, `bn-ebpf-solana`, Ghidra/SolDragon입니다.
`ezBPF`는 `88829078a6d7682a2baed0d696d500401c263750`에서 deprecated라고
명시하며 Blueshift를 안내합니다. 단일 byte-to-enum map을 쓰는 archived
predecessor일 뿐 moved-memory, JMP32, 현재 v0-v4 matrix를 아는 version-aware
decoder는 아닙니다. 이 snapshot에서 감사한 공개 general-purpose SBF decompiler 중
비교 pin은 Blueshift `704e40f7aa82446555b19d9ffbc0a6e18a35480f`, `qedsvm`
`99bd5ede85374adc7fc5c835c2432ecf4e123fd1`, `leanprover-solanalib`
`6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec`입니다. local tool은 `sol-azy`
`362327a798e5dad6e12aa9abf3ed9ed52c17ef6a`, `solana-data-reverser`
`bf90923adec984a61ca0437e9d341360ac1b11ee`, `SolDragon`
`002b98677a5e595a773af6607b77210f5ea71db7`, `bn-ebpf-solana`
`c3fe0de45d37eb68dcb08f2498c6e1f986056572`로 고정됩니다.
NeverD가 우리가 찾은
가장 강한 재현 가능 evidence를 갖습니다. 이는 범위가 정해진 비교이며 절대적인
“세계 1위” 주장이 아닙니다.

공개 경쟁 도구 감사에는 `r2ghidra-solana`을
`eca0b8e2d307e00991e289b8f9b0f45743819f1b`에 고정해 포함한다. Ghidra C-like UX와
`C-like-pdg`, account/Anchor/
string/syscall 보기를 제공하고 고정 HEAD의 CI는 성공했지만, Solana 전용
testsuite는 주석 처리되어 있으며 CI smoke는 `/bin/ls`만 decompile한다. 직접
재현해도 공식 V0 `relative_call_sbpfv0.so`는 합리적인 C를 내지만, 공식 V3
`relative_call.so`는 `pdg`에서 실패한다는 결과가 재현된다. `radare2-solana`는
`292d845681be377cadc9959a74c2cadeb6e7f412`에 고정되어
V2-only SIMD-0173/0174를 `>=V2`로 V3/V4까지 확장하지만, 공식 `program.rs`는
이를 V2-only로 명시한다. `SBPF-3-1`은
`0e602c93007faa96bccb8e1e12040954ff108b6f`에 고정되고 cargo test 2/2만
통과하는 trivial 결과이며 CI가 없다. version detection은 none/V0을 반환하는
placeholder이고 high-nibble opcode decoder가 틀렸으며 jump는 off 대신 imm을
사용한다. V0/V3 relative_call ELF도 동일하게 잘못된 pseudocode를 낸다.
NeverD의 이점은 V0–V4 공식 loader/verifier/runtime/process-oracle evidence를
재현할 수 있다는 점이며, 각 도구의 UX나 C output을 부정하지 않는다.

`SBFComparisonTools.def`가 비교 도구 display name과 전체 revision의 유일한 authority다.
마지막 bounded public sweep에서는 다음도 확인했다.

- `blastrock/Solana-eBPF-for-Ghidra`는
  `c3ad719004726fe924dbed901eca2744ad82c85d`에 고정했다. 실제 Ghidra P-code UX는
  있지만 version 비인식 SLEIGH model 하나가 CALLX를 `dst`로 고정하고 legacy/current
  opcode를 섞는다. 실질적인 test/CI가 없고 default source에는 참조하는 relocation
  constant class도 없다.
- `SolEmu-Ghidra`는 `6520af2ff104d5adbec24632ba3afa3bef0da529`에서 동일한 decoder를
  상속하고, 명시적으로 simulated/placeholder인 CPI·crypto·ZK behavior 주위에 emulator
  UI를 더하지만 실질적인 test/CI가 없다. `Ghidra_sBPF`는
  `907bd4476432ca83bb2352686ad1ccafdb38504c`에서 v1-v3을 수동 선택하지만 V2-only
  encoding을 V3에 누적하며 V0/V4 auto-selection과 test/CI가 없다.
- `solana-ebpf-ida-processor`는
  `aacd215907266190ed9c6c1b408ca9309f92ecdd`의 유용한 IDA disassembler/relocation UI일
  뿐 source lifter가 아니다. 혼합 opcode map은 CALLX를 항상 `imm`에서 읽고 test/CI가
  없다. `solana-bpf-reverse`는 `39479a3bddb8cb866ee499266a76a1b54069b222`에서
  hard-coded layout 추측으로 heuristic report와 Rust TODO scaffold를 만들며, 실행 결과는
  9 pass, 2 fail, 1 skip이고 CI가 없다.
- `solens`는 `22defa1c8f4118dacd42f5c291f1ac31609fc0e5`의 V2-only terminal
  disassembler로 테스트가 0개이고 CI가 없다. `sbpf-decompiler`는
  `37b8bc0edc7ce347abee466f5f974e900c1948df`에서 현재 구현이 3줄
  `Hello, world!`뿐이며 테스트가 0개이고 CI가 없다.
- `sbpf-eye`는 `5277a52aeb58e50b6ff8f9020414334765369b49`의 명시적인 lightweight
  WIP instruction/CFG TUI다. 테스트 3개는 통과하지만 semantic IR, source emitter, CI가 없다.
  `svm_bytecode_analyzer`는 `12aa236db8964e6be661e38131c2dc81588cf19c`의
  disassembler/CFG analyzer이지 lifter가 아니며, register/offset byte decode가 잘못됐고
  실행 결과는 17 pass, 1 fail, CI 없음이다.
- `giraffexiu/Solana-eBPF-for-Ghidra`는
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07`의 동일 Ghidra lineage 1-commit
  snapshot으로 version semantics, test, CI를 추가하지 않는다. `CertSBF`는
  `bb93a97cf0c64d119d08ec851e8e820315beb59e`의 가치 있는 구형 rBPF Isabelle/HOL
  formalization이지만 현재 V0-V4 whole-program source decompiler는 아니다.

이는 제한된 공개 snapshot의 비교 evidence이며 미래 도구나 private project에 대한 절대
결론이 아니다.

2026-08-24 최종 RPC audit는 정확히 일치했다. feature account 38개와 activation
row 89개이며 mainnet slot 441305159, testnet 433055669, devnet 487238699이다.
system-owned 빈 pending account(`VirtualAddressSpaceAdjustments`, mainnet)은
활성화되지 않았다. RPC URL은 문서에 고정하지 않는다.

Linux Release CI는 `--print-pinned-revision`, `--print-test-vectors-revision`,
`--print-toolchain`으로 exact pin을 읽고 official oracle과 sparse corpus를 인증한 뒤
`NEVERD_SBPF_ORACLE`과 `NEVERD_AGAVE_CONFORMANCE_ROOT`를 export하므로 두 external
test가 필수입니다. 명시적 oracle/corpus env가 없는 일반 local run은 case를
discover하지만 skip할 수 있습니다.
