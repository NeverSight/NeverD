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

v2 변경은 의도적으로 v3에 이어지지 않습니다. feature check는 명시적이며
`version >= N`으로 추측하지 않습니다. 기본 strict mode는 malformed header/range/
alignment, unsupported writable legacy section, invalid continuation/register/
frame-pointer write/branch, version-inactive opcode를 instruction slot과 virtual address로
보고하며 거부합니다.

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
```

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
| read-only 데이터의 base58 주소 | `SBFKnownAddresses.def` 일치, 또는 코드가 만들어내는 상수 |
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

이 분석이 기술하지 않은 함수 호출은 닿을 수 있는 모든 곳에 쓴다고 가정합니다.
피호출자는 자기 frame에서 실행되므로 인자 레지스터가 모두 scratch를 가리키지 않음이
증명된 호출에서는 모델이 남고, 그 밖에는 버립니다. `sol_invoke_signed_rust`와
`sol_invoke_signed_c`는 호출자 메모리가 아니라 account data를 쓰므로, 한 block에서
조립한 두 invocation은 모두 읽힙니다.

이 모델은 함수 내 CFG 위의 전방 must 분석입니다. 어떤 block에 이르는 모든 경로가 같은
값을 썼을 때에만 그 바이트가 그 block까지 살아남습니다. 피호출자는 호출자의 frame을
물려받지 않으므로 call edge는 따라가지 않습니다. block 수가
`kMaxScratchFlowBlocks`를 넘는 프로그램은 block 단위 복원을 유지하고 block 경계를
넘는 사실만 잃습니다.

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
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;
```

`width` 단위는 bit이고 nonzero host return은 explicit SBF status입니다. generated source는
register, return PC, callee-saved r6-r9, frame pointer, VM address, division fault, wide
PQR, wrapping shift를 나타내며 사용 helper만 출력해
`clang -Wall -Wextra -Werror`를 통과합니다.

## 생성 Rust host contract

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

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

## 현재 conformance baseline (2026-08-10)

relocation 뒤에는 VM address 기반의 단일 immutable `ProgramImage`가 decoder,
interpreter, string recovery, LLVM/C/Rust backend가 공유하는 source of truth입니다.
loader semantics와 달라질 수 있는 별도 text/rodata copy는 없습니다.

닫힌 record는 `SBFVersions.def`, `SBFOpcodes.def`, `SBFRelocations.def`,
`SBFArgumentRegisters.def`, `SBFProtocolLimits.def`, `SBFSyscalls.def`,
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
| official ELF manifest | `sbpf/tests/elfs` artifact 20/20 |
| ISA matrix | v0-v4 각각 모든 256 encoding, 총 1,280 cell과 verifier boundary |
| differential execution | raw-byte oracle과 LLVM ORC/C11/stable Rust의 memory/fault/syscall trace 비교 |
| integrated aggregate | 13 test binary의 124/124 case |
| ASan + UBSan | 12 core binary의 121/121 case, report 없음 |

감사는 Anza `sbpf`
`71425d0de59e0bff048c6be8f4a8a9bc655916e2`와 Agave
`cae40aa610fdbdb313209bc1eec737079eb59688`에 pin되어 있습니다. 갱신 시
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def`, `SBFUpstreamSources.def`를
검토하고 다음을 실행합니다.

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

비교 결과 `sol-azy`는 현재 strict ELF에서 crash하고 legacy CFG에 undefined node를
남겼습니다. `solana-data-reverser`는 account data용이고, `SolDragon`은 analysis를
WIP로 표시하며, `bn-ebpf-solana`는 Binary Ninja가 필요합니다. 따라서 official
`sbpf`와 Agave가 semantic authority입니다.
