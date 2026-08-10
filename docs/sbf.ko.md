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
