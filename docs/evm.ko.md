**언어**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM 디컴파일

[← 문서 색인](README.ko.md)

NeverD는 기존 Ethereum Virtual Machine 바이트코드를 로드하고 전용 256비트 LowIR,
스택 SSA MedIR, 복구된 HighIR을 구성하여 LLVM IR, C23 또는 Solidity를 출력합니다.
strict 분석이 기본이며, 할당되지 않았거나 선택한 hardfork에서 비활성인 opcode는 정확한
PC에서 오류가 됩니다.

Solidity와 C 출력은 의미론적 재구성입니다. 디코딩된 opcode 순서, 256비트 산술,
스택 검사, 검증된 제어 흐름은 보존하지만 원래 소스, 식별자, 타입 복원을 주장하지 않습니다.

## 빠른 시작

```bash
# i256/i512를 사용하는 검증된 LLVM IR.
./build/bin/neverd lift contract.evm -o contract.ll

# 각 EVM 분석 단계를 확인합니다.
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# C23 또는 Solidity를 출력합니다.
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# 과거 opcode 집합을 선택하고 알 수 없는 opcode를 조사용 fault node로 유지합니다.
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

`disasm`, `cfg`, C API의 Low/Med/High/LLVM query도 EVM 입력을 받습니다. EVM binary
rewrite는 명시적으로 거부되며 `patch`는 native binary 전용입니다.

## 지원 입력

| 입력 | 인식 및 정규화 |
|------|----------------|
| raw bytes | `.raw`, `.evmraw` 또는 명시적 EVM 확장자의 binary content |
| hex text | 선택적 `0x`, 임의 ASCII 공백, `.evm`/`.hex`/`.bin`/`.bytecode`; 검증된 무확장자 hex도 감지 |
| compiler artifact | root 또는 `evm` 아래 `deployedBytecode`, `runtimeBytecode`, `bytecode`를 가진 `.json`; `contracts → file → contract → evm` solc standard JSON도 지원 |

runtime/deployed bytecode가 creation bytecode보다 우선합니다. creation code만 있으면
bounded constant `CODECOPY`/`RETURN` constructor wrapper를 인식해 복사된 runtime
slice를 추출합니다. 선택적 `0x`만 든 field는 비어 있다고 보므로 빈 runtime field가
사용 가능한 creation fallback을 숨기지 않습니다. 마지막 Solidity CBOR map은 encoded
length, map marker, 알려진 `solc`/`ipfs`/Swarm key를 모두 검증한 경우에만 제거합니다.

잘못된 hex, 홀수 자릿수, 미해결 linker placeholder, 모호한 multi-contract artifact,
잘못된 metadata bound, 정규화 후 빈 code는 실행 가능한 오류를 냅니다. C++ API의
`BytecodeLoadOptions::ArtifactContract`는 `Contract` 또는
`path/File.sol:Contract`를 선택합니다. 여러 source file에 같은 이름이 있으면
unqualified name을 거부해 artifact 순서에 따른 잘못된 선택을 막습니다.

EVM은 backend plugin이 아니라 core loader registry에 등록됩니다. 따라서 CLI, C API,
disassembler, CFG builder, Low/Med/High/LLVM query가 동일한 normalized image와 EVM
option을 받아 entry point 사이의 인식/분석 drift를 방지합니다.

## Hardfork와 opcode

Frontier부터 Fusaka까지 할당된 legacy opcode 150개를 모두 지원하며 `PUSH0`, transient
storage, `MCOPY`, blob opcode, `CLZ`를 포함합니다. 기본 `latest`는 Fusaka입니다.

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, latest
```

`dao`, `tangerine_whistle` 같은 underscore 표기, `merge`, `prague`, `osaka`도 허용하며
현재 `latest`와 `osaka`는 canonical `fusaka` revision으로 해석됩니다.

`latest`는 NeverD가 구현한 최신 확정 mainnet revision이지 Ethereum 개발 branch의
끝이 아닙니다. [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/)은 2026년 4분기
예정이며 Review 단계의 [SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843)과
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024)는 fork와 encoding이
확정될 때까지 기본 table에서 제외됩니다. EIP-8024 immediate byte는 `PUSH`와 다른
`JUMPDEST` masking rule을 가지므로 일반 1-byte immediate로 볼 수 없습니다.

EOF는 [Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2)에서
제외되었고 execution-spec-tests도
[Osaka에서 제거되고 일정이 없다고 기록](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md)합니다.
NeverD는 철회된 proposal을 확정 mainnet behavior로 취급하지 않습니다.

strict mode는 unknown 및 fork-inactive byte를 거부합니다. `--evm-relaxed`는 이를
LowIR과 diagnostic에 보존하지만 실행이 도달하면 backend가 fault하며, unknown byte를
NOP으로 조용히 처리하지 않습니다.

## LLVM 스타일 metadata 아키텍처

수동 EVM metadata는 LLVM의 multiply-included `.def` pattern을 따릅니다.

- `EVMOpcodes.def`는 150 opcode의 단일 source of truth입니다. encoding, 전체 stack
  contract, immediate width, class, activation fork, primary effect, 독립된 EVM
  memory/state/call-value access와 termination을 한 record에 두어 암묵적 default가 없습니다.
- `EVMMemoryAccesses.def`, `EVMStateAccesses.def`,
  `EVMCallValueAccesses.def`는 닫힌 typed domain입니다. `CALL`은 external call이면서
  memory read/write이고 `EXTCODECOPY`는 context read이면서 memory write일 수 있습니다.
  state access는 `None/Read/Write/Unknown` lattice입니다. payability는 별도 제약으로,
  일반 `CALLVALUE` read는 `payable`을 유도합니다. 분석기는 canonical
  `ISZERO(CALLVALUE)` guard와 nonzero branch의 `REVERT`를 증명한 경우에만 컴파일러가
  만든 read를 제외합니다.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def`,
  `OutputLanguages.def`가 ordered enum, parser, display name, CLI choice, C ABI value를
  생성합니다. `EVMConstants.h`가 protocol width/limit/default name을 소유합니다.
- `Semantics.h`는 target-independent scalar ALU evaluator입니다. constant folding과
  interpreter가 같은 checked `APInt`를 사용하고 LLVM/C/Solidity lowering은 target
  contract와 unsupported case가 보이도록 각각 명시적으로 fail-loud합니다.

decoder가 raw-byte boundary입니다. assigned identity와 fork activation을 분리하여
relaxed decode가 inactive opcode의 name/fork/immediate width를 보존하되 semantic query는
conservative fault로 유지합니다. inactive immediate가 이후 byte boundary를 밀지 않습니다.
분석, interpreter, emitter는 generated `Opcode`와 metadata query를 사용하고 raw encoding은
trace/host callback ABI boundary에만 다시 나타납니다. `SWAP16`의 17 stack input과 최대
host op의 7 argument는 서로 다른 compile-time-derived limit입니다.

`OpcodeInfo`는 반쪽짜리 record로 default construct할 수 없고 name은 dangling하지 않는
`llvm::StringLiteral`입니다. compile-time validator는 duplicate encoding, unknown
property, ALU contract, effect/state mismatch, PUSH/DUP/SWAP/LOG family, terminator 및
host ABI result count를 검사합니다. unknown stack family는 전용 lowering 전까지 거부되며
relaxed unknown metadata는 명시적 factory만 생성합니다.

`.def`는 LLVM의
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def)
같은 hand-authored database입니다. `.inc`는 TableGen output 같은 실제 generated/literal
fragment 용도입니다. 풍부한 declarative record는 `.td`에서
[TableGen](https://llvm.org/docs/TableGen/ProgRef.html)이 `.inc`를 생성합니다. NeverD에는
현재 EVM TableGen step이 없으므로 generator 없는 generated-looking `.inc`를 두지 않습니다.
주변 C++는 LLVM [coding standards](https://llvm.org/docs/CodingStandards.html), LLVM
ADT/string type, exhaustive fail-loud switch를 따릅니다.

opcode를 추가할 때는 완전한 `EVM_OPCODE` record, 공유 scalar semantics, 명시적 backend
lowering, focused test를 추가합니다. hardfork는 ordered `EVM_HARDFORK`와 alias를
추가합니다. typed API, lookup, validation, classification, CLI 값은 중복 table 없이 확장되고
semantic case 누락은 즉시 실패합니다.

## 분석 모델

- **LowIR**: PC, encoding, truncated PUSH의 right-zero padding, block/edge, validated
  `JUMPDEST`, reachability, stack height.
- **MedIR**: 256-bit stack SSA, merge phi, pure constant folding, primary effect와
  orthogonal memory/state/call-value property. dataflow, alias, mutability, payability에
  compound instruction 정보를 정확히 전달합니다.
- **HighIR**: dispatcher selector, calldata/return word, mutability, constant slot,
  event/revert, function/CFG region을 best-effort로 복구합니다. payability와 state lattice는
  독립적입니다. unresolved reachable jump는 `Unknown`으로 join되어 Solidity가
  `nonpayable`로 보수화됩니다. 같은 selector의 충돌 pattern은 진단 후 생략합니다.
- **LLVM**: verifier-clean `i32 @evm_execute(ptr)` state machine, checked 1024-word
  `i256` stack, `i512` intermediate, guarded signed division, saturated shift, 정확한
  `BYTE`/`SIGNEXTEND`/`CLZ`, validated dynamic-jump switch.

deterministic interpreter가 semantic oracle입니다. LLVM/C 실행 결과를 비교하고 Solidity는
Anvil에 deploy하여 storage/trace를 비교합니다. pre-Fusaka raw corpus도 Anvil native EVM에서
실행하여 scalar ALU, calldata copy, overlapping `MCOPY`, memory expansion, Keccak,
return data를 독립 검증합니다.

account operand는 [실행 사양](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py)에
따라 160-bit address로 mask하고 environment/map width를 실행 전에 검증합니다.
`BLOCKHASH`는 이전 256-block window를 지킵니다. EIP-211 return-data buffer는 frame output과
분리되어 `RETURN`/`REVERT`만 `ExecutionResult::ReturnData`를 설정합니다. CREATE/CREATE2
failure는 zero와 revert buffer, success는 address와 빈 buffer를 남깁니다.

## 생성 C contract

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

환경 의존 operation은 아래 host ABI를 호출합니다. `a0`가 원래 stack top이고 사용하지
않는 argument는 zero, 반환값은 첫 pushed value입니다. trace는 각 instruction 전에 실행됩니다.

```c
evm_word neverd_evm_host_op(
    struct neverd_evm_env *environment, uint8_t opcode,
    evm_word a0, evm_word a1, evm_word a2, evm_word a3,
    evm_word a4, evm_word a5, evm_word a6);
void neverd_evm_trace(
    struct neverd_evm_env *environment, uint64_t pc, uint8_t opcode);
```

```bash
clang -std=c2x -ffreestanding -c contract.c
```

frontend는 512-bit `_BitInt`를 지원해야 합니다. Apple Darwin Clang은 현재 한도가 낮으므로
macOS에서는 지원하는 non-Darwin target 또는 NeverD LLVM output을 사용합니다.

## 생성 Solidity contract

출력은 감사용 selector-specific function/storage/event/error와 정확한 PC/stack state
machine을 함께 제공합니다. constant storage는
`recovered_storage_slot_3 = uint256(0x3)` 같은 absolute slot constant이며 거짓 sequential
state layout을 만들지 않습니다.

contract는 의도적으로 `abstract`입니다. `_evmHost`를 override해 environment effect를
구현하고 `_evmTrace`는 기본으로 `EVMTrace`를 emit합니다.

```bash
solc --bin contract.sol
```

## C API

```c
neverd_session_t session = neverd_session_create();
neverd_evm_set_hardfork(session, "cancun");
neverd_evm_set_strict(session, 1);
if (!neverd_session_load(session, "contract.evm") ||
    !neverd_session_analyze(session)) {
  /* inspect neverd_last_error(session) */
}
const char *solidity = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_SOLIDITY, 0, 0);
const char *c = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_C, 0, 0);
neverd_free_string(solidity);
neverd_free_string(c);
neverd_session_destroy(session);
```

`neverd_decompile_all`은 호환성을 위해 C를 출력합니다. 새 entry point는
`neverd_session_bitness`, `neverd_evm_set_strict`, `neverd_evm_set_hardfork`,
`neverd_decompile_all_ex`입니다. native에 Solidity, EVM에 legacy LLVM-to-C flag 또는
native object roundtrip을 요청하면 조용히 무시하지 않고 명시적으로 거부합니다.

## 명시적 제한

- legacy bytecode만 지원하며 EOF container는 아직 디코딩하지 않습니다.
- Review 단계 Amsterdam opcode는 비활성이며 `latest`는 확정 Fusaka입니다.
- RPC, chain-state discovery, gas/refund, precompile execution은 제공하지 않습니다.
- creation extraction은 흔한 static wrapper만 인식하며 constructor emulator가 아닙니다.
- dynamic jump는 bounded constant analysis로 증명되지 않으면 indirect edge로 남습니다.
- ABI type, source name, mapping, event, custom error는 best-effort recovery입니다.
- memory/storage/calldata/call/log/hash/context를 독립 실행하려면 host hook이 필요합니다.
