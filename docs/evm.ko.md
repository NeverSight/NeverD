**언어**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM 디컴파일

[← 문서 색인](README.ko.md)

NeverD는 기존 Ethereum Virtual Machine 바이트코드를 로드하고 전용 256비트 LowIR,
스택 SSA MedIR, 복구된 HighIR을 구성하여 LLVM IR, C23 또는 Solidity를 출력합니다.
strict 분석이 기본이지만 legacy EVM은 image 전체의 opcode를 미리 검증하지 않습니다.
확실히 `Reachable`한 execution lane이 할당되지 않았거나 선택 hardfork에서 inactive인
opcode에 실제로 도달할 때만 정확한 PC에서 거부합니다. dead byte와 단순 `MayReachable`
CFG candidate는 strict error가 아닙니다.

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
slice를 추출합니다. constructor 순회는 분석 대상 hardfork 아래에서 실제 decoder와
동일한 단일 instruction decoder를 사용하므로, 어떤 fork에서는 데이터이고 다른
fork에서는 opcode인 byte가 경계를 옮길 수 없습니다. 존재하는 `deployedBytecode` 또는
`runtimeBytecode` field는 권위 있는 입력입니다. 명시적 `0x`는 비어 있고 자연 종료하는
runtime으로 받아들이며 creation bytecode fallback을 의도적으로 막습니다. field가 없을 때만
다음 candidate를 찾고, 명시적 prefix가 없는 누락 또는 공백뿐인 hex는 거부합니다. 명시적 raw
입력도 빈 값을 허용합니다.

### 컴파일러 trailer

`EVMMetadataFields.def`는 두 trailer 형식을 모두 표로 정리합니다. Solidity는 마지막
두 byte가 map 자체만 세는 CBOR map을 쓰고, `vyper`는 그 map으로 끝나는 CBOR array를
쓰는데 그 마지막 두 byte는 자기 자신을 포함한 footer 전체를 셉니다. 한쪽 framing을
다른 쪽으로 읽어도 요란하게 실패하지 않습니다. 두 byte 어긋난 자리에 떨어져 실제
code 두 byte를 지울 뿐이므로, 둘 다 시도하고 어느 쪽에도 맞지 않는 입력은 그대로
둡니다.

trailer는 두 번 읽습니다. 주어진 입력 그대로 한 번, deployment wrapper를 벗겨낸 뒤
남은 runtime code에 대해 한 번입니다. Vyper는 trailer를 initcode로 옮겨 runtime
code에는 남기지 않으므로, 벗겨낸 뒤에만 보는 reader는 스스로 이름을 밝힌 contract를
알 수 없는 build로 보고하게 됩니다. sequence footer는 runtime code 길이, data
section 길이, immutables 길이도 밝히며, 이는 constructor를 실행하지 않고도 반환될
code를 한정합니다.

### instruction이 아닌 container

`EVMBytecodeContainers.def`는 어떤 decode보다 먼저 입력을 분류합니다. EIP-3541이
`0xEF`를 배포 불가로 만든 이후로, 선두 `0xEF`는 그 byte들이 instruction이 아님을
약속합니다.

| 컨테이너 | 마커 | 처리 |
|----------|------|------|
| legacy | — | instruction으로 decode |
| delegation (`eip-7702`) | `0xef0100`과 정확히 23 byte | 대상 account를 보고하고 분석 중단 |
| eof (`eip-3540`) | `0xef00` | 거부; 활성화한 fork 없음 |

delegation indicator의 20 byte는 code가 아니라 주소입니다. 이를 decode하면 주소를
opcode로 읽어 account의 control-flow graph를 만들어내므로, `info`는 대상을 보고하고
분석은 이유를 밝히며 거부합니다. 그 거부는 두 경우를 구분합니다. Pectra 이전에는
marker가 아직 할당되지 않았고, Pectra부터는 대상의 runtime code가 단지 없는
것입니다. 길이가 다른 marker는 container의 변종이 아니라 malformed 입력이므로
instruction으로 남겨, decoder가 읽지 못한 byte를 지목할 수 있게 합니다.

잘못된 hex, 홀수 자릿수, 미해결 linker placeholder, 모호한 multi-contract artifact,
잘못된 metadata bound, 누락되거나 공백뿐인 hex는 실행 가능한 오류를 냅니다. 명시적 빈
raw 입력 또는 `0x` runtime은 유효한 빈 program입니다. C++ API의
`BytecodeLoadOptions::ArtifactContract`는 `Contract` 또는
`path/File.sol:Contract`를 선택합니다. 여러 source file에 같은 이름이 있으면
unqualified name을 거부해 artifact 순서에 따른 잘못된 선택을 막습니다.

EVM은 backend plugin이 아니라 core loader registry에 등록됩니다. 따라서 CLI, C API,
disassembler, CFG builder, Low/Med/High/LLVM query가 동일한 normalized image와 EVM
option을 받아 entry point 사이의 인식/분석 drift를 방지합니다.

## Hardfork와 opcode

Frontier부터 Fusaka까지 할당된 legacy opcode를 모두 지원하며 `PUSH0`, transient
storage, `MCOPY`, blob opcode, `CLZ`를 포함합니다. 기본 `latest`는 Fusaka입니다.

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, amsterdam, bogota, latest
```

`dao`, `tangerine_whistle` 같은 underscore 표기, `merge`, `prague`, `osaka`도 허용하며
현재 `latest`와 `osaka`는 canonical `fusaka` revision으로 해석됩니다.

`latest`는 NeverD가 구현한 최신 확정 mainnet revision이지 Ethereum 개발 branch의
끝이 아닙니다. [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/)은 2026년 4분기
예정이며 Review 단계의 [SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843)과
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024)는
`--evm-hardfork=amsterdam`(또는 `bogota`)에서만 활성화되며 확정 전까지 `latest`에서
제외됩니다. EIP-8024는 유효한 immediate만 소비하고, 유효하지 않은 후보는 다음
instruction으로 남습니다.

EOF는 [Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2)에서
제외되었습니다. EOFv1/EIP-7692는 일정이 없고 container proposal
[EIP-3540](https://eips.ethereum.org/EIPS/eip-3540)은 Stagnant 상태입니다. 이전
`execution-spec-tests` repository는 archive되었으며 유지되는 tests는
[execution-specs](https://github.com/ethereum/execution-specs/tree/master/tests)로 이동했습니다.
NeverD는 experimental EOF container를 확정 mainnet behavior처럼 취급하지 않습니다.

strict mode는 확실히 `Reachable`한 state lane이 실행 도달을 증명한 unknown 또는
fork-inactive byte만 거부합니다. `--evm-relaxed`는 이를 typed fault prefix와 diagnostic으로
보존하지만 backend는 도달 시 fault하며, unknown byte를 NOP으로 처리하지 않습니다.

## LLVM 스타일 metadata 아키텍처

수동 EVM metadata는 LLVM의 multiply-included `.def` pattern을 따릅니다.

- `EVMOpcodes.def`는 확정 legacy 및 opt-in 개발 opcode 전체의 단일 source of
  truth입니다. encoding, 실제 pop/push 변화, immediate kind, class, activation
  fork, primary effect, 독립된 EVM
  memory/state/call-value access와 termination을 한 record에 두어 암묵적 default가 없습니다.
- `EVMMemoryAccesses.def`, `EVMStateAccesses.def`,
  `EVMCallValueAccesses.def`는 닫힌 typed domain입니다. `CALL`은 external call이면서
  memory read/write이고 `EXTCODECOPY`는 context read이면서 memory write일 수 있습니다.
  state access는 `None/Read/Write/Unknown` lattice입니다. payability는 별도 제약으로,
  일반 `CALLVALUE` read는 `payable`을 유도합니다. 분석기는 canonical
  `ISZERO(CALLVALUE)` guard와 nonzero branch의 `REVERT`를 증명한 경우에만 컴파일러가
  만든 read를 제외합니다.
- `EVMImmediateKinds.def`는 fixed-width PUSH data와 EIP-8024 conditional single/pair
  encoding을 정의하고 `EVMDecodeStatuses.def`는 LowIR 및 disassembly가 공개하는 stable
  vocabulary를 소유합니다. `EVMUpstreamOpcodePolicy.def`는 go-ethereum naming alias와
  의도적인 historical/unscheduled-EOF exclusion을 기록하며,
  `scripts/audit_evm_opcode_metadata.py`는 byte drift와 검토하지 않은 새 upstream constant를
  거부합니다.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def`,
  `OutputLanguages.def`가 ordered enum, parser, display name, CLI choice, C ABI value를
  생성합니다. `EVMAnalysisLimits.def`, `EVMInterpreterLimits.def`,
  `EVMABIParserLimits.def`, `EVMABITableLimits.def`는 analysis, interpreter, parser,
  public table의 단계별 상한을 선언합니다. `EVMConstants.h`는 shared protocol width와 stable
  internal name을 소유하고 `EVMAnalysisLimits.def`에서 analysis default와 diagnostic option
  name을 생성합니다. interpreter/ABI header는 각자의 table에서 limit를 생성합니다.
- `EVMCalls.def`는 다른 프로그램을 호출하는 네 개의 instruction과 callee 주소가
  올 수 있는 출처의 lattice를 기술합니다. 레코드당 하나의 flag, 즉 callee와 인자
  window 사이에 value operand가 있는지 여부가 이후 모든 operand 위치를 유도하며,
  그 유도가 선언된 pop 수와 어긋나지 않도록 opcode 데이터베이스에 대해 검증됩니다.
- `EVMPrecompiles.def`는 프로토콜이 직접 응답하는 주소들의 사전이며, 각 항목은 그
  주소를 예약한 fork와 그것을 일정에 올린 proposal을 함께 가집니다. `0x100`의
  `P256VERIFY`는 `eip-7951`에 귀속됩니다. Fusaka와 함께 mainnet에 그 주소를 예약한
  Final proposal이 바로 이것이며, 그 인터페이스가 유래한 rollup proposal은 끝내
  일정에 올리지 못했기 때문입니다. gas는 의도적으로 없습니다. precompile의 비용은
  입력의 함수이고, 주소나 연산이 바뀌지 않은 채로 여러 번 재가격되었기 때문입니다.
- `EVMMetadataFields.def`와 `EVMBytecodeContainers.def`는 입력이 decode되기 전에
  그것이 무엇인지를 기술합니다. 두 가지 컴파일러 trailer framing, 그리고 그 byte가
  애초에 instruction이 아닌 container들입니다.
- `EVMRecoveredFacts.def`는 복구 사실 어휘의 철자를 소유하므로, 출력에 도달하는
  이름은 새 enumerator가 빠질 수 있는 `switch`가 아니라 한 곳에 존재합니다.
  `EVMKnownSignatures.def`는 canonical function spelling과 selector를 한 번만 저장하고,
  standard별 `KnownFunctionVariantInfo`에 return list와 independent/non-independent evidence
  role을 분리합니다. ERC-20/ERC-721 shared spelling은 하나의 callable candidate로 남지만
  어느 standard도 단독으로 증명하지 않으며 첫 variant의 return type을 빌리지 않습니다.
  event와 custom error는 별도 typed record를 유지합니다.
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

- **EVM LowIR**: PC, encoding, typed immediate status, 디코딩된 stack-depth operand
  (PUSH right-zero padding과 EIP-8024 conditional-consumption rule 포함), basic block,
  predecessor/successor edge, 검증된 `JUMPDEST` target, reachability, stack-height domain을
  보존합니다. CFG recovery는 deterministic whole-program fixed point입니다. stack slot마다
  bounded finite set of 256-bit values를 전파하고 concrete height마다 abstract stack 하나를
  유지합니다. internal-call/return block을 가로지르는 constant, stack shuffle,
  `PC`/`CODESIZE`, scalar ALU operation으로 하나 이상의 concrete jump target을 해결할 수
  있습니다. 실제로 알 수 없는 target은 추측하지 않고 explicit indirect edge로 남습니다.

  back-edge에서 바뀐 loop-carried slot은 fixed point 수렴을 위해 semantic하게 `Top`으로
  over-approximate됩니다. 이 loop recurrence abstraction은 resource budget과 독립입니다.
  instruction, block, state, value node, abstract stack, lane, edge, worklist update,
  instruction×lane transfer는 named budget으로 과금하며 `MaxAbstractValuesPerSlot`,
  `MaxStackHeightVariants`, `MaxAbstractInstructionTransfers`를 포함합니다. zero 또는
  exhaustion은 insertion 전 hard error입니다.
  추가 emergency widening이나 silent truncation은 없습니다. exact invalid target은 jump PC에서
  실패합니다.

  `EVMLowFaultKinds.def::InvalidJumpDestination`는 `end-of-code JUMPI`에서 path-sensitive입니다.
  condition이 확실히 true이고 target이 invalid라면 successful tail 없이 definite fault를 기록하며,
  확실히 false라면 성공합니다. unknown condition은 성공할 수 있는 false path만 남기고 lane 전체를
  definite fault로 잘못 표시하지 않습니다.
- **EVM MedIR**: 모든 stack value를 256-bit SSA value로 표현하고 모든 merge phi를 연결한
  뒤 deterministic sparse constant worklist를 실행합니다. private lattice는
  `Uninitialized`, 하나의 exact `Constant`, `Overdefined`입니다. 같은 constant는 block과
  anchored phi cycle을 넘어 전파되지만 conflict하거나 runtime-dependent인 cycle은 constant를
  만들어낼 수 없습니다. worklist는 def-use ID를 검사하며 value, state lane, stack entry,
  operation, operation-lane reference, phi incoming, worklist update마다 독립 budget을 둡니다.
  interpreter와 같은 `Semantics.h` ALU evaluator를 사용합니다. MedIR은 primary semantic effect와 별도로
  `none/read/write/readwrite` EVM-memory access, source-level state access, call-value access도
  보존합니다. 각 LowIR whole-stack lane에는 독립된 SSA execution lane이 대응하고 phi는
  source lane을 명시합니다. 서로 맞지 않는 stack을 maximum height로 top-align하지 않습니다.
- **EVM HighIR**: Solidity dispatcher selector, 추정 calldata/return word, mutability,
  constant storage slot, LOG/event 및 revert fact, function/CFG region을 복구합니다. checked
  producer index와 iterative memoized value walk는 instruction distance가 아니라 typed MedIR
  operand에서 fact를 복구합니다. selector comparison은 block과 phi를 가로지를 수 있고 `EQ`
  operand 순서를 모두 지원하며 derived 32-bit mask를 유지합니다. argument offset, storage
  key, event topic0, non-payable/receive guard, exact 32-byte return size는 semantic input을
  사용합니다. iterative walk는 MedIR graph에 의해 구조적으로 bounded되며 malformed, mixed,
  cyclic expression을 unknown으로 취급합니다. 같은 selector의 conflicting target은 진단 후
  생략합니다. payability는 state-access lattice와 독립이고 reachable unresolved dynamic jump는
  보수적인 `nonpayable` recovery를 강제합니다. byte 단위 flow-sensitive memory dataflow는
  block을 넘는 constant-offset write를 추적하고 overlap/kill로 byte를 합성하며
  dynamic/unknown write에서 지식을 무효화합니다. 현재 증명된 payload recovery는 selector와
  알려진 Panic byte입니다. 알려진 custom-error declaration의 canonical parameter type은
  Solidity emitter가 유지하지만, 각 runtime argument value를 복구한다고 주장하지 않습니다.

  selector discovery는 root lane에서만 시작해 dispatcher의 unmatched edge를 따릅니다.
  handler 안의 selector-like test는 public function으로 승격하지 않습니다. receive/fallback도
  root-constrained이며 확실히 reachable한 successful terminal이 필요합니다. revert, fault,
  non-payable empty-calldata handler, 단순 possible path는 entry point를 증명하지 못합니다.
  canonical function candidate는 충돌하는 calldata use로 탈락하고 shared selector는 independent
  standard evidence에 포함되지 않습니다. 설정 수의 독립 compatible selector 또는 exact event
  topic/arity, storage slot, proxy 같은 강한 evidence가 있어야 standard와 per-standard variant를
  선택합니다. static return list도 확실히 reachable한 모든 successful terminal이 exact ABI byte
  count에 동의할 때만 출력합니다. unresolved transfer, conflicting shape, mismatch는 fail closed이고
  revert/fault는 successful return이 아닙니다. name, type, event, standard label은 evidence-backed
  candidate입니다.

  HighIR은 function, lane/operation visit, region block reference, memory read request, tracked byte,
  memory state cell, memory worklist update별 hostile-input budget을 둡니다. memory fixed point는
  확실히 reachable하고 실제 실행되는 lane만 사용하고 predecessor byte를 consensus로 meet합니다.
  budget exhaustion은 fact를 자르지 않고 hard error가 됩니다.

  HighIR는 interface의 나가는 절반도 기록합니다. 즉 모든 `CALL`, `CALLCODE`,
  `DELEGATECALL`, `STATICCALL`에 대해 callee의 출처, 분석 대상 fork가 예약한 경우 그
  예약 주소, 호출이 callee의 calldata 앞에 놓는 selector, 그리고 상수일 때 전송되는
  value를 기록합니다. `CREATE`와 `CREATE2`는 아직 주소가 없는 코드를 실행하므로 복구할
  callee가 없어 제외됩니다.

  복구된 외부 signature는 프로그램이 응답하는 표준 목록에 결코 들어가지 않습니다.
  `transfer(address,uint256)`를 보낸다는 것은 프로그램이 토큰을 사용한다는 뜻이지
  토큰이라는 뜻이 아니며, 둘을 뒤섞으면 모든 router와 vault가 ERC-20으로 보고됩니다.
  delegate하는 call은 추가로 proxy fact로도 보고되는데, callee의 코드가 이 프로그램
  자신의 storage에 대해 실행되는 것은 이 family에서 그것뿐이기 때문입니다.

  precompile 조회는 존재하는 가장 새로운 fork가 아니라 분석 대상 fork를 기준으로
  gate됩니다. 나중 fork가 도입하는 precompile 주소를 호출하면 코드 없는 계정에 도달해
  성공하고 아무것도 반환하지 않으므로, 이름을 붙이면 프로그램이 명백히 수행하지 않은
  연산을 보고하게 됩니다.
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

interpreter는 opcode-specific side effect 전에 typed required stack height, pop count,
retained-plus-pushed height를 preflight하므로 underflow/overflow가 instruction을 절반만 실행하지
않습니다. `EVMForkSemantics.def`는 byte `0x44`를 Paris 전 `DIFFICULTY`, Paris 이후
`PREVRANDAO`로 선택합니다. `REVERT`, semantic fault, step limit, allocation/length resource
exhaustion은 storage, transient storage, log, selfdestruct effect를 entry snapshot으로 되돌리면서
frame-local diagnostic과 명시적 revert byte는 유지합니다. allocation failure는 error string을
추가 할당하지 않고 `ExecutionFaultKind::ResourceExhausted`로 표시합니다. entry snapshot조차
만들지 못하면 `HasPersistentStateSnapshot`은 false이고 결과는 commit할 수 없습니다.

### Public IR 및 resource boundary

public `execute`는 먼저
`Code`/`Fork`/`Instructions`/`JumpDestinations`가 canonical LowIR를 이루는지 검증합니다. fork,
instruction record, encoding, jump-destination table을 변조하면 interpreter가 instruction table을
index하기 전에 `llvm::Error`가 반환됩니다. public `lowerToMedIR`도 설정된 모든
option, resource bound, structural invariant를 이 순서대로 검증한 뒤 `canonical decode replay`로
내장된 fork/strictness에서 `Low.Code`를 decode하고 LowIR field를 하나씩 비교합니다. 그 뒤에야
`lowerCanonicalLowToMedIR`를 호출하거나 index를 만들고 caller-controlled record에 비례한 output을
allocation할 수 있습니다. public `recoverHighIR`도 외부 LowIR/MedIR를 replay 검증합니다. private
`lowerCanonicalLowToMedIR`와 `recoverCanonicalHighIR`는 `analyze`가 소유한 IR에만 쓰며 중복된
비재귀 replay만 생략합니다. HighIR option/resource budget은 여전히 모두 적용됩니다.

dispatcher proof는 `MedStateLane`마다 정렬된 `Any/Exact/Excluded` selector domain을 보관합니다.
join은 Exact set을 union하고 Excluded exclusion set을 intersect하며 cofinite exclusion에서 Exact
set을 뺍니다. domain이 widen되면 lane을 다시 방문합니다. equality는 selector가 허용될 때만
true-edge candidate를 기록하고 false edge에서 이를 제외합니다. raw
`XOR(selector, constant)`는 모든 canonical successor가 같은 entry를 가리킬 때 zero/false edge를
match로 기록하며, 이 fallthrough form은 `JUMPDEST` target을 요구하지 않습니다. nonzero/true
mismatch edge는 selector를 제외하고 `ISZERO`는 같은 expression을 equality로 바꿉니다. selector
word, zero-calldata word, calldata size, call value guard는 edge별로 refine되며 unknown
conditional은 possible branch를 탐색하지 않고 proof를 중단합니다.

function을 인식한 뒤 function-scope traversal은 해당 candidate의
`exact singleton selector`를 유지한 채 계속됩니다. shared dispatcher로 jump해 돌아오면
`SelectorEquality`, raw `XOR`, `SelectorWord`는 이미 match된 selector와 일치하는
`definite edge`만 따릅니다. Unknown이거나 무관한 predicate에서는 모든 `definite edges`를
보수적으로 유지합니다. 다른 entry block을 제외하는 heuristic은 사용하지 않으므로 올바른
`shared body/tail-call`이 보존됩니다.

외부 CALL/CREATE 결과는 다릅니다. host outcome 자체가 비결정적이므로 분석은 두 개의 정확한 CFG
edge를 모두 탐색합니다. 이로써 ERC-1167 fallback recovery를 유지하면서 읽을 수 없는 selector
condition을 증거로 삼지 않습니다. 실제로 Unknown인 dispatcher는 계속 fail closed입니다.

`EVMAnalysisLimits.def`는 `MaxLowDiagnostics`와 `MaxLowDiagnosticBytes`로 linear decoder와 CFG
builder에 하나의 aggregate LowIR diagnostic budget을 제공합니다. 두 경로 모두 정확한 count와
최종 byte를 precharge하고 zero limit를 거부합니다. LowIR와 HighIR diagnostic budget은 서로
독립적입니다. 같은 표는 `MaxHighDispatchCandidates`,
program-wide aggregate
`MaxHighRecoveredArguments`, `MaxHighDiagnostics`와 `MaxHighDiagnosticBytes`,
`MaxHighReferenceVisits`, `MaxHighMemoryTransferCells`, `MaxHighMemoryValueVisits`를 각각
과금합니다. candidate/recovered-argument record는 destination container 삽입이나 name/type
allocation 전에 precharge됩니다. fixed malformed-IR diagnostic을 포함한 모든 HighIR output
diagnostic은 생성/복사 전에 count와 최종 message byte를 정확히 과금합니다. budget 부족은 named
hard error이며 diagnostic이나 fact를 조용히 생략하지 않습니다.
default root CFG region은 block-PC list를 reserve하거나 copy하기 전에
`MaxHighRegionBlockReferences`를 과금합니다.

`EVMABIParserLimits.def`는 tuple nesting, type node, aggregate array dimension을 제한하고,
`EVMABITableLimits.def`는 public signature/variant table의 cardinality와 aggregate text를
제한합니다. public table validation은 parse/hash 전에 상한을 적용한 뒤 invalid enum, kind
metadata, standard, selector-evidence role, noncanonical type, derived hash, membership,
collision을 거부합니다. production selector lookup은 indexed이고 event lookup은 topic 정렬 table을
사용하며, topic API는 비교/ordering 전에 `APInt`가 정확히 한 EVM word인지 확인합니다.

`EVMInterpreterLimits.def`는 `MaxSteps`, `MaxMemoryBytes`, `MaxTraceEntries`,
`MaxLogEntries`, aggregate `MaxLogDataBytes`, aggregate `MaxHostReturnDataBytes`,
`MaxCalldataBytes`, aggregate `MaxHostEnvironmentEntries`, aggregate `MaxExternalCodeBytes`,
`MaxPersistentStateEntries`를 선언합니다. host-entry aggregate는 `BlockHashes`, `Balances`,
`CodeHashes`, `ExternalCode`, `BlobHashes` 전체에 적용되고 external-code byte limit는 모든
`ExternalCode` body를 합산합니다. `MaxSteps`는 명시적인 `StepLimit` 결과를 유지합니다.
runtime memory, trace, log, log data, 새 persistent-state key는 precharge되며 상한 초과는
`ResourceExhausted`로 persistent state, log, selfdestruct effect를 rollback합니다. 초기 host
return-data aggregate나 persistent-state map이 너무 크면 `execute` API error입니다. interpreter는
host return data를 `ArrayRef` view로 보관하고 검증된 정렬 instruction table에서 `lower_bound`를
사용하므로 buffer를 복사하거나 실행마다 PC map을 다시 만들지 않습니다.
`const execute preflight`는 environment copy, persistent-state snapshot 생성, result 구성보다 먼저
program과 모든 host-input limit를 검증합니다.

### 최신 go-ethereum 차등 감사

표준 로컬 감사와 CI는 실행할 때마다 `git fetch --depth=1 --force`로 공식
`https://github.com/ethereum/go-ethereum.git` default branch의 remote `HEAD`를
가져옵니다. 실행마다 예측할 수 없는 이름의 private temporary bare repository를 만들며 shared
persistent Git repository나 cache는 쓰지 않습니다. revision을 선택하는 유일한 authority는 그
fetch가 돌려준 authority ref와 거기서 해석한 exact SHA입니다. SHA를 보고하고 detached 임시
worktree에서 probe한 뒤 authority repository와 worktree를 함께 폐기합니다. `local_docs`, 기존 checkout,
submodule은 감사 경로가 아니며, pin된 submodule은 live drift를 찾아야 할 때 낡습니다.

모든 Git command는 상속된 `GIT_*`를 먼저 전부 지우고(`GIT_CONFIG_*` 포함), 검토한 값만
설정합니다. `GIT_CONFIG_NOSYSTEM`과 `GIT_CONFIG_GLOBAL`은 system/global config를,
`GIT_ATTR_NOSYSTEM`과 command scope의 `core.attributesFile`은 system/global attributes를,
`core.hooksPath`는 hooks를 끕니다. private repository는 예상 밖 local config, graft,
`objects/info/alternates`, `refs/replace`를 거부하고, `GIT_NO_REPLACE_OBJECTS`도 replacement
lookup을 비활성화합니다. 어긋나면 fail closed입니다.

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

public CLI가 받는 유일한 option은 `--manifest-output`이며 remote/ref/toolchain override를 제공하지
않습니다. 출력 manifest의 closed contract는 `schema 3`입니다.

Go probe는 `params.Rules`의 exported boolean field 전체를 reflection하고 매핑된 각 fork에서
공개 `LookupInstructionSet(params.Rules)`를 호출하며 256 byte slot 전부를 스캔합니다. 검토하지
않은 activation은 NeverD request 밖에도 숨을 수 없습니다. allocation은 geth의
`operation.undefined`만으로 판정합니다. `HasCost`는 defined zero-cost operation에도 false이므로
cost cross-check에만 씁니다. 모든 `defined && !HasCost` slot은 선언된 activation fork부터
`EVM_GETH_ACTIVE_WITHOUT_COST`와 정확히 일치해야 합니다. cost가 있는 undefined slot, 검토하지
않은 defined slot, marker를 숨기는 upstream 변경은 fail closed입니다. closed schema는 schema version,
exact geth revision, Go version, stack limit, fork rules, 각 opcode의 byte, name,
`base_min_stack`, `net_stack_delta`만 허용하고 audit manifest는 diagnostics array만 더합니다.
알 수 없거나 중복된 field, rule, fork, name, byte는 오류입니다.
`EVMUpstreamOpcodePolicy.def`는 name alias와 typed·검토된 historical/unscheduled-EOF exclusion을
소유하고 overlap/inactive invariant를 검증합니다. 직교하는 `EVMUpstreamSemanticsPolicy.def`는
closed `params.Rules` reflection inventory, fork mapping, base-stack exception, EIP-8024 dynamic
opcode family 선언을 소유합니다. CI는 `dev` push, pull request, 수동 실행,
일일 schedule에서만 실행하며, 실패하면 정확한 revision, manifest, log를 artifact로 올립니다.

구체적으로 `EVMUpstreamSemanticsPolicy.def`는 export된 boolean `params.Rules` field마다 정확히 한
개의 `EVM_GETH_RULE_FIELD`를 두고 `MappedForkSelector`, `NoOpcodeAllocation`,
`ExcludedSelectorExpectedError` 중 하나로 분류합니다. audit는 field를 하나씩만 켜서
`LookupInstructionSet`을 호출합니다. 앞의 두 category는 nil error, 세 번째는 error여야 하며,
반환된 전체 256-slot opcode/stack fingerprint는 항상 `ExpectedFork`와 같아야 합니다. 현재
no-allocation fields인 `IsEIP155`, `IsEIP2929`, `IsEIP4762`, `IsPetersburg`는 Frontier
fingerprint이고, `IsUBT`는 error와 Cancun fingerprint가 기대값입니다.

EIP-8024 dynamic opcode family의 membership과 activation은
`EVMUpstreamSemanticsPolicy.def`가 선언하며, `EVMEIP8024Immediates.def`는 계속해서 single/pair의
각 byte에 대한 immediate semantics의 유일한 authority입니다. single/pair inventory는 각각 256개
byte value 전부를 valid/invalid로 명시하며 production decoder는 직접 lookup합니다. live audit는
`go -overlay`로 `core/vm`에 virtual wrapper를 주입해 실제 private `operation.execute` handler를
얻고, active table/family마다 `DUPN`, `SWAPN`, `EXCHANGE`의 `3x256` candidates와
`3 missing-operand cases`를 실행합니다. acceptance, PC delta, unique marker로 유도한 stack
operand/mutation, valid case의 정확한 underflow, operand 누락 시 `0x00`을 검사합니다. Python은
같은 `.def`와 항목별로 비교하며 decode formula를 복제하지 않습니다.

`EVM_HARDFORK_LATEST`의 canonical target은 정확히 하나입니다. closed
`EVMUpstreamForkAliases.def`는 Prague를 Pectra로, Osaka와 BPO1~BPO5를 Fusaka로 mapping하며
Paris, Shanghai, Cancun, Amsterdam, Bogota는 identity입니다. 알 수 없는 새 이름은 fail
closed입니다. 각 audit는 하나의 `audit_unix_time`을 고정해 기록하고,
`MainnetChainConfig.LatestFork(time)`가 NeverD latest로 map되는지,
`LatestFork(max uint64)`가 alias inventory에 있고 canonical fork도 probe되었는지 확인합니다.
probe는 실제 `canonical fork jump tables`와 `mainnet active/scheduled jump tables`를 열거해
table별로 완전 비교하고 dynamic family 또는 fork의 `inactive` 상태도 명시적으로 기록합니다.
table, family, probe의 일부만 얻은 `partial` result는 manifest로 받지 않고 fail closed입니다.
manifest는 `authority=official-fresh-fetch`, 공식 URL, 요청한
`HEAD`, resolve된 SHA를 기록합니다. public CLI에 remote/ref/toolchain bypass는 없으며 probe는
`GOTOOLCHAIN=local`로 고정됩니다.

Go와 Python은 hostile metadata를 materialize하기 전에 제한합니다. 양쪽 모두
`input/collection/string hard limits`를 적용하며 한도를 넘는 JSON input, array, string은 fail
closed입니다. 별도로 `bounded diagnostic output`을 강제하므로 너무 긴 diagnostic display에는
full-content `digest`와 `explicit truncated marker`가 포함되어 완전한 message로 오인되지 않습니다.
모든 child command에는 bounded output과 deadline이 적용됩니다. timeout 또는 output-limit 위반은
전체 `process group`과 하위 process tree를 kill하고 pipe를 drain합니다. 모든 `.def parser`는
unparsed, unknown, duplicate, missing, out-of-range entry를 거부하고 어떤 불일치에도 fail
closed합니다.

현재 schema-3 live receipt는 `schema_version=3`, `audit_unix_time=1787534659`,
`authority=official-fresh-fetch`, `remote=https://github.com/ethereum/go-ethereum.git`,
`ref=HEAD`, revision `02b73d4ea7181464175e0a6cbecc0a3a2655a562`, local `Go 1.24.0`,
`stack_limit=1024`, `diagnostics=[]`를 기록합니다. `21 fork tables`와 `20 Rules probes`를
비교하며 분류는 `15 mapped/4 no-op/1 expected-error`입니다. 두 `mainnet active/scheduled`
record 모두 `upstream BPO2`를 가리키며 closed alias가 이를 `NeverD Fusaka`로 map합니다.
EIP-8024는 `23 table targets`를 대상으로 하고 `Amsterdam/Bogota`만 active입니다. 그 결과
`1536 candidate executions`와 `6 missing-operand cases`가 생성되며 `three handler symbols`는
두 active target에서 일치합니다. closing test는 Python audit `67/67`과
`C++ Opcode 10/10`입니다. macOS에서는 실제 audit가 `sandbox-exec` 안에서 성공했고 마지막
`go run`은 offline이었습니다. Linux workflow는 `bubblewrap`을 필수로 사용합니다.

모든 Go phase인 `go env`, `go mod init`, `go mod edit`, `go mod tidy`, `go mod download`,
`go run`은 `capability-root` filesystem sandbox 안에서 실행됩니다. read capability는 private
probe, fresh geth worktree, 검증된 `resolved GOROOT`, 필요한 system runtime root의 정확한
집합으로 제한되고 isolated environment root만 쓸 수 있습니다. network capability는 필요한
dependency phase에만 추가되며 final run은 offline입니다. `host HOME/workspace`의 sentinel
접근은 거부되고 그 내용도 output에 나타날 수 없습니다. Linux는 `/` broad bind가 없는 동형의
`bubblewrap` policy를 사용합니다.

`NeverDEVMDecoderPropertyTests`는 decoder가 달라지는 각 fork의 모든 2-byte 입력에 대해
완전한 decode와 정확한 `JUMPDEST` boundary를 비교하고, 길이가 제한된 결정적 hostile byte
string을 모든 fork에 통과시킵니다.

LowIR/MedIR path lane은 같은 path 안의 상관관계를 보존하고 `MayReachable`은 CFG candidate일
뿐 확정 fact를 만들지 않습니다. HighIR의 selector, receive, fallback, return shape,
byte-granular memory fact는 definitely reachable executing lane만 사용합니다. shared selector와
per-standard `KnownFunctionVariantInfo`는 분리되고 return type은 모든 successful terminal의
shape check를 통과해야 합니다. 모든 analysis budget exhaustion은 fail loud이며 emergency
widening이나 silent truncation을 하지 않습니다.

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
- Amsterdam/Bogota는 명시적 개발 target이며, 예정 opcode가 확정될 때까지 `latest`는
  확정된 Fusaka로 유지됩니다.
- RPC, chain-state discovery, gas/refund, precompile execution은 제공하지 않습니다.
- creation extraction은 흔한 static wrapper만 인식하며 constructor emulator가 아닙니다.
- dynamic jump는 bounded constant analysis로 증명되지 않으면 indirect edge로 남습니다.
- ABI type, source name, mapping, event, custom error는 best-effort recovery입니다.
- memory/storage/calldata/call/log/hash/context를 독립 실행하려면 host hook이 필요합니다.
