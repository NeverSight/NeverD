**언어**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← 문서 인덱스](README.ko.md)

# NeverD 테스트

NeverD 테스트는 표현 모양이 예상과 같은지, 바이너리 fixture의 전체 pipeline 경로가
동작하는지, 생성 코드가 동작을 보존하는지라는 세 가지 질문에 답합니다. 변경의 질문에
답하는 가장 작은 스위트를 고른 뒤 위험이 큰 풀 리퀘스트에서는 더 넓은 집계를 실행하세요.

## 테스트 빌드 구성

`BUILD_TESTING`을 활성화하지 않으면 테스트가 비활성화됩니다. 전체 스위트에는 보통
Release를 사용합니다. Debug는 assertion과 단계 실행을 보존하지만 의도적으로
최적화하지 않으므로 디코드 벤치마크를 대표하지 않습니다.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

전체 fixture 세트에는 교차 대상 컴파일용 `clang`과 `PATH`에 있는 LLVM
linker(`ld.lld`, `lld-link`)가 필요합니다. CMake는 많은 재배치 가능 fixture를
항상 만들고 해당 linker가 있으면 링크된 ELF/PE fixture도 만듭니다. host가 fixture를
컴파일하거나 링크할 수 없어 건너뛴 테스트는 실행되지 않은 coverage이지 해당 대상의
통과가 아닙니다.

복제, 빌드 프로필, macOS 사전 빌드 LLVM은
[CONTRIBUTING.md](i18n/CONTRIBUTING.ko.md)를 참고하세요.

## 테스트 배치

`add_neverd_unittest`는 GoogleTest 실행 파일 하나를 만들고 발견한 각 사례에 실행
target 이름과 같은 CTest label을 지정합니다.

| 소스 영역 | Target 및 CTest label | 범위 |
|-----------|------------------------|------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | 교차 플랫폼 하위 프로세스 호출, quoting, redirect, 종료 코드 |
| `unittests/libc` | `NeverDLibCTests` | 알려진 libc 이름과 분류 |
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | 싱크 카탈로그, 신원 우선순위, 인수 사전 필터, 복사 오버플로 헌트, 힙 수명 감사, 필수 PE/ELF/Mach-O × x86-64/AArch64 6셀 매트릭스 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR 모양, IR 단계, loader, relocation, 포맷 fixture, 디컴파일, 대표 patch 흐름 |
| `unittests/semantic`의 대부분 파일 | `NeverDSemanticTests` | 명령어, ABI, 제어 흐름, C 표현식, lift/recompile 차등 의미론 |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | hardfork metadata, input normalization, ABI/signature ambiguity, CFG/SSA/recovery, decoder boundary 전수 검사와 hostile input, proxy/call fact, interpreter semantics, LLVM/C/Solidity differential execution, public API routing |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | v0-v4 메타데이터와 ELF 레이아웃, 엄격한 verifier/loader 동작, 고정된 ELF 아티팩트 23개, 독립 official oracle, 모든 opcode 가용성, 적대적 입력, CFG/복원, 실행된 LLVM/C/Rust 차분 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 네 ISA×세 object 포맷 재작성/난독화 동등성 |
| `unittests/semantic`의 집중 변환 파일 | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | 큰 의미론 바이너리에서 분리한 빠른 재링크 probe |
| `unittests/corpus`(submodule) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests` | pin 된 실제 바이너리 317개에서 읽어내는 예외 및 런타임 metadata. 각 바이너리는 manifest에 복원이 넘어야 할 하한을 선언한다 |

등록의 기준은
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt),
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt),
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt),
[`unittests/safety/CMakeLists.txt`](../unittests/safety/CMakeLists.txt)입니다.

### pin 된 바이너리 corpus

다른 모든 스위트는 테스트 대상을 직접 빌드하지만 corpus는 그렇지 않습니다. 이것은
실제 툴체인이 이 저장소가 닿을 수 없는 호스트에서, 닿을 수 없는 타깃을 대상으로
만들어낸 바이너리들의 submodule이며, 각 파일은 다이제스트로 pin 되고 옆의 manifest가
그 복원이 넘어야 할 하한을 선언합니다. "`-O2`로 strip 된 `armv7` 공유 오브젝트에서
NeverD가 무엇을 읽어내는가" 같은 물음에 논쟁이 아니라 답을 줄 수 있는 곳은 여기뿐
입니다.

이 스위트들은 configure가 그것들을 찾도록 지시받았을 때에만 빌드되므로, 이 플래그가
곧 그것들이 테스트되고 있는지의 전부입니다.

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus`는 모든 라인을, `check-neverd-windows-eh-corpus`,
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus`,
`check-neverd-cxx-itanium-eh-corpus`, `check-neverd-objc-eh-corpus`는 각각 한 라인을
실행합니다. CI의 세 호스트 모두 이 플래그로 configure 하고 다섯 라인을 전부
실행합니다. 바이트는 어디서나 같지만 그것을 읽는 쪽은 같지 않으며, 한 호스트에서의
corpus 실행은 나머지 두 호스트에 대해 아무것도 증명하지 않습니다.
`scripts/audit_ci_test_inventory.py`는 다섯 label 중 하나라도 빠진 inventory를
거부합니다. corpus를 조용히 읽지 않게 된 빌드는 어떤 테스트도 잡을 수 없는
회귀이기 때문입니다. 사라진 것이 바로 그 테스트입니다.

live EVM opcode audit는 다음 명령으로 실행합니다.

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

public CLI가 받는 유일한 option은 `--manifest-output`이며 remote/ref/toolchain override를 제공하지
않습니다. 출력 manifest의 closed contract는 `schema 3`입니다.

로컬과 CI의 표준 경로는 공식 `https://github.com/ethereum/go-ethereum.git`에
`git fetch --depth=1 --force`를 실행해 default
branch의 remote `HEAD`에서 방금 얻은 정확한 SHA만 detached worktree에서 검사합니다.
실행마다 예측할 수 없는 이름의 private temporary bare repository를 쓰고, official fetch의
authority ref와 exact SHA를 detached worktree 수명 동안 유지한 뒤 repository와 worktree를 함께
폐기합니다. shared persistent Git repository나 cache는 없습니다. `local_docs`,
기존 checkout, submodule은 감사 경로가 아니며, pin된 submodule은 live drift를 찾아야 할 때
낡습니다.

각 Git command는 상속된 `GIT_*`를 먼저 전부 지우고(`GIT_CONFIG_*` 포함), 검토한 값만
설정합니다. `GIT_CONFIG_NOSYSTEM`과 `GIT_CONFIG_GLOBAL`은 system/global config를,
`GIT_ATTR_NOSYSTEM`과 command scope의 `core.attributesFile`은 system/global attributes를,
`core.hooksPath`는 hooks를 끕니다. 예상 밖 private-repository config, graft,
`objects/info/alternates`, `refs/replace`는 검증을 실패시키고,
`GIT_NO_REPLACE_OBJECTS`도 replacement lookup을 비활성화합니다.

probe는 `params.Rules`가 export한 모든 bool field를 반사하고 각 fork에서
`LookupInstructionSet(params.Rules)`를 호출해 256 byte slot 전부를 스캔합니다.
`EVMUpstreamOpcodePolicy.def`는 typed historical/unscheduled-EOF exclusion과 alias를,
`EVMUpstreamSemanticsPolicy.def`는 closed Rules inventory, fork mapping, base-stack exception,
EIP-8024 dynamic opcode family 선언을 각각 소유합니다.

CI는 `dev` push, pull request, manual dispatch, daily schedule에서만 같은 live audit를
실행합니다. Go probe는 매핑된 각 fork에서 공개 `LookupInstructionSet(params.Rules)` API를
호출합니다. `EVMUpstreamOpcodePolicy.def`는 name alias와 검토된 historical/unscheduled-EOF
exclusion을, 직교하는 `EVMUpstreamSemanticsPolicy.def`는 fork rule, stack semantics 예외,
EIP-8024 dynamic opcode family의 membership/activation을 소유합니다. closed manifest는 정확한 revision, fork activation, byte/name,
`base_min_stack`, `net_stack_delta`를 검사하며 알 수 없거나 중복된 field, fork, name, byte를
거부합니다. allocation은 `operation.undefined`만으로 판정하고 `HasCost`는 defined zero-cost
operation에도 false이므로 cost cross-check로만 씁니다. 모든 `defined && !HasCost` slot은
선언된 fork부터 `EVM_GETH_ACTIVE_WITHOUT_COST`와 정확히 일치해야 합니다. cost가 있는 undefined
slot, 검토하지 않은 defined slot, marker 소실은 fail closed입니다.
CI 실패 시 정확한 revision, manifest, log가 artifact로 올라갑니다. parser와
drift diagnostic에는 독립적인 Python unit coverage가 있습니다.

`EVMUpstreamSemanticsPolicy.def`는 export된 boolean `params.Rules` field마다 정확히 하나의
`EVM_GETH_RULE_FIELD`를 두고 `MappedForkSelector`, `NoOpcodeAllocation`,
`ExcludedSelectorExpectedError`로 분류합니다. probe는 field 하나만 켜서 `LookupInstructionSet`을
호출합니다. 앞의 두 category는 nil error, 세 번째는 error여야 하며 반환된 전체 256-slot
opcode/stack fingerprint는 `ExpectedFork`와 일치해야 합니다. 현재 `IsEIP155`, `IsEIP2929`,
`IsEIP4762`, `IsPetersburg`는 Frontier fingerprint인 no-allocation fields이고, `IsUBT`는 error와
Cancun fingerprint가 기대값입니다.

`EVMEIP8024Immediates.def`는 계속해서 single/pair의 각 byte에 대한 immediate semantics의 유일한
authority이며, 각각 256개 byte 전부를 명시적으로 분류합니다. production은 직접 lookup합니다.
live audit는 `go -overlay`로 `core/vm`에 virtual wrapper를 주입해 실제 private
`operation.execute` handler를 얻고, active table/family마다 `DUPN`, `SWAPN`, `EXCHANGE`의
`3x256` candidates와 `3 missing-operand cases`를 실행합니다. acceptance, PC delta,
marker-derived operand/stack mutation, valid case의 정확한 underflow, operand 누락 시 `0x00`을
확인하며 Python은 formula를 다시 쓰지 않고 같은 `.def`와 비교합니다.

`EVM_HARDFORK_LATEST`의 canonical target은 하나뿐입니다. closed
`EVMUpstreamForkAliases.def`는 Prague→Pectra, Osaka와 BPO1~BPO5→Fusaka,
Paris/Shanghai/Cancun/Amsterdam/Bogota→자기 자신을 정의하며 알 수 없는 이름은 fail
closed입니다. 기록된 하나의 `audit_unix_time`으로 `MainnetChainConfig.LatestFork(time)`
(NeverD latest와 일치해야 함)와 `LatestFork(max uint64)`의 alias/probed canonical fork를
검사합니다. probe는 실제 `canonical fork jump tables`와 `mainnet active/scheduled jump tables`를
열거해 table별로 완전 비교하고 dynamic family 또는 fork의 `inactive` 상태도 명시적으로
기록합니다. table/family/probe의 일부만 얻은 `partial` result는 받지 않고 fail closed입니다.
manifest는 `authority=official-fresh-fetch`, 공식 URL,
요청한 `HEAD`, SHA를 고정합니다. public CLI에 remote/ref/toolchain bypass는 없고 probe는
`GOTOOLCHAIN=local`을 사용합니다.

Go request/response와 Python controller는 hostile metadata를 allocate하기 전에
`input/collection/string hard limits`를 적용하고 한도를 넘는 input, array, string을 fail
closed합니다. 별도로 `bounded diagnostic output`을 강제하여 너무 긴 display에 full-content
`digest`와 `explicit truncated marker`를 포함합니다. 모든 command에는 bounded child output과
공통 deadline이 적용되며 timeout 또는 output-limit 위반은 전체 `process group`과 하위
process tree를 kill하고 pipe를 drain합니다. 모든 `.def parser`는 unparsed, unknown, duplicate, missing,
out-of-range entry를 거부하고 fail closed합니다.

현재 schema-3 live receipt는 `schema_version=3`, `audit_unix_time=1787534659`,
`authority=official-fresh-fetch`, `remote=https://github.com/ethereum/go-ethereum.git`,
`ref=HEAD`, revision `02b73d4ea7181464175e0a6cbecc0a3a2655a562`, local `Go 1.24.0`,
`stack_limit=1024`, `diagnostics=[]`를 기록합니다. `21 fork tables`와 `20 Rules probes`를
다루며 분류는 `15 mapped/4 no-op/1 expected-error`입니다. 두 `mainnet active/scheduled`
record는 모두 `upstream BPO2`를 보고하며 closed map으로 `NeverD Fusaka`에 대응됩니다.
EIP-8024의 `23 table targets` 가운데 `Amsterdam/Bogota`만 active이며
`1536 candidate executions`와 `6 missing-operand cases`를 생성합니다. `three handler symbols`는
두 active target에서 일치합니다. Python audit는 `67/67`, `C++ Opcode 10/10`입니다. macOS 실제
run은 `sandbox-exec` 안에서 성공했고 마지막 `go run`은 offline이었습니다. Linux workflow는
`bubblewrap`을 강제합니다.

모든 Go stage인 `go env`, `go mod init`, `go mod edit`, `go mod tidy`, `go mod download`,
`go run`은 `capability-root` filesystem sandbox를 통과해야 합니다. read capability에는 private
probe, fresh geth, 검증된 `resolved GOROOT`, 필요한 system runtime root의 정확한 집합만 포함되고
isolated environment root만 쓸 수 있습니다. network는 필요한 dependency stage에만 허용되며 final
run은 offline입니다. test는 `host HOME/workspace`에 sentinel을 놓고 접근이 거부되며 어떤 output에도
그 내용이 나타나지 않음을 요구합니다. Linux는 `/` broad bind가 없는 동형의 `bubblewrap` policy를
검증합니다.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

CMake에 등록된 EVM test target 11개는 다음과 같습니다.

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests`는 decoder가 달라지는 각 fork의 모든 2-byte input에 대해
완전한 decode와 정확한 `JUMPDEST` boundary를 비교하고, 길이가 제한된 결정적 hostile input을
모든 fork에 통과시킵니다.

EVM control-flow 변경에서는 fixed-point 및 height-domain contract를 먼저 실행합니다.

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

이 case들은 block을 가로지르는 internal return, finite multi-target merge, loop convergence,
deterministic edge ordering, path-sensitive whole-stack lane, correlation preservation, unknown
jump, exact invalid target, fail-loud budget, strict/relaxed stack fault를 포함합니다.
`MayReachable`은 CFG candidate일 뿐 확정 semantic fact를 만들지 못합니다. 이어서 11개 EVM
target과 live upstream audit를 모두 실행하세요.

MedIR/HighIR dataflow 변경에서는 constant-phi, selector, typed-operand,
malformed-graph, deep-chain contract도 실행합니다.

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

이 case들은 equal/conflicting cyclic phi, 인접하지 않거나 block을 가로지르는 selector
expression, 두 equality operand order, exact ABI width check, typed
storage/event/calldata operand, malformed MedIR의 deterministic handling, 16,384-value iterative
producer walk를 검증합니다.

## fixture 생성 방식

### Lift 및 포맷 fixture

`unittests/lift/CMakeLists.txt`는 빌드 중 C와 assembly 소스를 교차 컴파일합니다.
Clang target triple이 x86-64, i386, AArch64, ARM32 ELF object, PE/COFF object와
링크된 image, PIC/no-PIC Mach-O i386 object를 만듭니다. LLD가 있으면 선택한
object를 patch 테스트용 실행 파일로도 링크합니다. `NeverDLiftTests`는
`lift-test-objects` target에 의존하므로 일반적인 테스트 바이너리 빌드가 생성
fixture를 갱신합니다.

대부분의 lift 테스트는 `NeverDLiftFixture.h`로 빌드된 `neverd` CLI를 호출하고
LowIR, MedIR, HighIR, LLVM IR, 생성된 C 또는 재작성 바이너리를 검사합니다. 집중된
수동 실험은 `NEVERD` 환경 변수로 CLI 경로를 override할 수 있습니다. 일반 CTest는
CMake에 포함된 실행 파일을 사용합니다.

### 메모리 안전성 fixture

`unittests/safety/fixtures/binaries`에는 x86-64와 AArch64용 PE, ELF, Mach-O
이미지가 체크인되어 있으며, 각 포맷이 제공하는 PDB 또는 dSYM 동반 파일과 함께
이미지마다 링커 MAP이 하나씩 들어 있습니다. MAP은 strip된 빌드가 유일하게
남기는 신원 정보이므로, 각 셀은 MAP을 명시적으로 지정한 분석도 함께 수행하여
타입도 소스 줄 번호도 남지 않았을 때 발견이 무엇을 주장할 수 있는지 고정합니다.
`NeverDSafetyIntegrationTests`는 모든 호스트에서 여섯 셀을 전부 실행합니다.
필요한 이미지나 동반 파일이 없으면 구성 단계에서 실패하며, 호스트 툴체인에 따른
건너뛰기 경로는 없습니다.

여섯 개의 동등한 바이너리는 하나의 소스 파일에서 나옵니다. `make`는 호스트
네이티브 smoke fixture만 다시 만듭니다. 체크인된 전체 행렬을 재생성하려면 다음을
사용합니다.

```bash
make -C unittests/safety/fixtures matrix
```

전체 재생성에는 Clang의 Linux/Windows 크로스 타깃, LLD의 COFF 도구, 두 Darwin
아키텍처, 그리고 `dsymutil`이 필요합니다. 디버그 경로는 재매핑되고 CodeView
명령줄 기록은 비활성화되므로, 체크인된 동반 파일이 개발자 작업 공간의 절대
경로를 담지 않습니다.

### Windows 예외 재구성

Windows 테이블 기반 예외를 변경할 때는 표현 테스트와 링크된 PE patch 테스트가 모두
필요합니다. 집중 lift-suite 필터는 정규화된 unwind/SEH/C++ 모델, 손상 입력 처리,
예외 CFG edge, HighIR, LLVM WinEH 생성, 예외 디렉터리 교체, Guard CF/EH
continuation 재구성을 검사합니다.

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

보호된 x64 assembly fixture에는 Clang Windows target과 `lld-link`가 필요하며 CMake
link는 `/guard:cf`와 `/guard:ehcont`를 사용합니다. cross-linker 누락으로 인한 skip은
final-image 경로의 증거가 아닙니다. 통합 사례가 통과하면 다시 작성된 PE를 재로드할 수
있고 runtime-function, unwind, load-config, Guard CF, Guard EH continuation 테이블이
정렬되고 파일에 존재하며 실행 가능한 target만 가리킨다는 것을 입증합니다.

링크된 FH3 fixture는 고정 상태 테이블, HighC 주석, personality 보존, 생성된 catch
target, 재로드한 IP-to-state 그래프로 네이티브 C++ closure를 독립적으로 검사합니다.

분석/네이티브 지원 표와 fail-closed patch 계약은
[Windows 예외 재구성](windows-exception-reconstruction.ko.md)을 참조하십시오.

### 언어 예외 모델

Windows 테이블 모델이 아닌 모든 것은 하나의 집중 target 에 모여 있습니다.
`NeverDLanguageEHTests` 는 DWARF 프레임 체인, Itanium 언어별 데이터 영역,
ARM EHABI, Darwin compact unwind, Go 런타임 프레임 메타데이터, Rust panic
기구, 그리고 세 가지 Objective-C 런타임을 다룹니다.

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

이 스위트의 테이블은 컴파일이 아니라 바이트 단위로 조립합니다. 검증하려는 조합
대부분은 단일 toolchain 이 한꺼번에 내보내지 않기 때문입니다. Objective-C 가
가장 분명한 사례입니다. 세 런타임 모두 Itanium LSDA 를 내보내고, 차이는 타입
테이블 슬롯에 무엇이 들어가는지 뿐이며, 그 차이는 정도가 아니라 전면적입니다.
Apple 의 슬롯은 `objc_typeinfo` 를 가리키며 그 첫 두 필드는 의도적으로
`std::type_info` 를 흉내 냅니다. GNUstep 의 Objective-C++ 슬롯은 진짜
`std::type_info` 파생 타입을 가리키고, GNU 런타임의 슬롯은 포인터조차 아닌
클래스 이름 문자열 그 자체입니다. 한 런타임의 규약을 다른 런타임의 테이블에
적용해도 실패하지 않습니다. 전혀 다른 것의 중간에서 읽어낸 클래스 이름을 보고할
뿐입니다. 그래서 슬롯을 읽기 전에 프레임의 personality 로 런타임을 먼저
확정합니다.

같은 스위트는 뭉뚱그리기 쉬우면서 뭉뚱그리면 틀리는 두 가지 구분도 고정합니다.
`@catch(id)` 와 `@catch(...)` 는 서로 다른 핸들러이며 — 앞의 것은 임의의
Objective-C 객체를 받고 외래 예외는 그 옆으로 지나가게 둡니다 — 런타임마다
표기가 다릅니다. 둘 다 catch-all 로 보고하는 디코더는 원래 지나쳤을 예외에
핸들러를 붙이는 셈입니다. 또한 setjmp/longjmp call-site 테이블은 주소가 아니라
호출 지점 색인을 담으므로, SJLJ personality 를 알아보지 못한 판독기는 오류를
내지 않고 프로그램이 지정한 적 없는 보호 구간과 landing pad 를 지어냅니다.

그 형식을 알아보는 것과 해독을 거부하는 것은 다릅니다. SJLJ 항목 하나는 ULEB128
한 쌍 — 디스패치 선택자와 action 오프셋 — 이며, 이 action 오프셋의 의미는 주소
형식에서와 완전히 같습니다. 따라서 action 체인도, catch 타입도, 예외 명세도, 코드를
전혀 가리키지 않는 표에서 그대로 읽어낼 수 있습니다. 읽어낼 수 없는 것은 각 항목이
지키는 구간뿐인데, 그것을 말해 주는 것은 함수가 자기 call-site 슬롯에 수행하는
저장이지 표 안의 무엇이 아니기 때문입니다. 이 스위트는 여기서 믿어서는 안 되는
바이트 하나도 못박습니다. call-site 인코딩으로 GCC 는 `DW_EH_PE_uleb128` 을,
LLVM 은 `DW_EH_PE_udata4` 를 적지만 둘 다 그 뒤로는 ULEB128 을 내보내며, 어떤
personality 도 그 바이트를 읽지 않습니다 — 그러니 디코더도 읽어서는 안 됩니다.

personality 의 정체도 함께 못박습니다. 위의 모든 표를 어떻게 읽을지 결정하는 것이
바로 그것이기 때문입니다. GNAT 은 GCC 가 모든 프런트엔드에 부여하는 세 가지 철자
— `_v0`, `_sj0`, `_seh0` — 로 자기 루틴의 이름을 짓고, Windows 에서는 한 심벌을
등록하면서 다른 심벌로 전달하므로 네 가지 철자가 모두 Ada 로 귀착해야 합니다. D 는
그 거울상으로, 세 개의 컴파일러, 한 루틴에 대한 세 개의 이름, 그리고 그 뒤에 있는
것은 동일한 표입니다.

### Unicorn 차등 왕복

의미론 fixture는 텍스트 모양이 아니라 동작을 테스트합니다.

1. 작은 C/assembly 사례를 작성하거나 LLVM IR을 구성합니다.
2. Clang/LLVM으로 요청한 대상을 위해 컴파일합니다.
3. 원래 machine code를 Unicorn에서 실행하고 예상 반환값이나 fixture가 정의한 상태를 캡처합니다.
4. NeverD로 로드하고 lift하여 LLVM IR을 출력한 뒤 다시 machine code로 컴파일합니다.
5. 같은 ABI, 입력, 메모리 배치, CPU 모델로 재생성 코드를 실행합니다.
6. 관측 가능한 결과를 비교합니다.

주요 구현은
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h)입니다.
patch-full fixture는 patch 작업과 같은 rewrite backend인
`Codegen::compileForRewrite`를 사용한 뒤 전체 4×3 ISA/포맷 grid에서 baseline과
변환 코드를 비교합니다.

결정적인 NeverD 의미론 실패는 실패 테스트여야 합니다. skip은 명시적인 외부 기능
경계에만 사용하고 이유를 읽으세요. 교차 linker가 없는 녹색 요약은 해당 포맷 경로가
실행되었음을 증명하지 않습니다.

### EVM 차등 백엔드

interpreter test는 deterministic 256-bit oracle입니다. emitter suite는 LLVM을
compile/execute하고 C23을 Clang으로 같은 host harness에 lower하며 `solc`, `anvil`,
`cast`, `jq`가 있으면 generated Solidity를 local node에 deploy합니다. status, storage,
instruction trace count를 비교합니다. 별도 raw-bytecode corpus는 Anvil native EVM에서
pre-Fusaka ALU, calldata/memory copy, overlapping `MCOPY`, Keccak, return data를 실행합니다.

Low/Med 테스트는 path-sensitive whole-stack execution lane과 phi lane identity를 보존하고
`MaxAbstractInstructionTransfers`를 포함한 budget exhaustion을 hard error로 만듭니다.
strict는 증명된 `Reachable` lane의 unknown/fork-inactive opcode만 거부하며
`MayReachable`은 definite fact를 만들지 않습니다. HighIR의 selector/receive/fallback은 root
lane과 성공 terminal로 제한됩니다. 공유 selector는 독립 standard evidence가 아니며,
standard별 `KnownFunctionVariantInfo`와 성공 terminal의 정확한 return shape가 일치할 때만
variant와 return list를 선택합니다.

interpreter는 opcode별 side effect 전에 typed stack preflight를 수행합니다.
`EVMForkSemantics.def`는 byte `0x44`를 Paris 이전의 `DIFFICULTY`, Paris부터의
`PREVRANDAO`로 정의합니다. `REVERT`, fault, step limit, resource exhaustion은 state를
rollback합니다. allocation failure는 `ExecutionFaultKind::ResourceExhausted`이고 entry
snapshot 자체를 만들지 못하면 `HasPersistentStateSnapshot`은 false이며 commit할 수 없습니다.

### EVM public boundary 및 budget regression

public API test는 canonical
`Code`/`Fork`/`Instructions`/`JumpDestinations`와 모든 LowIR table, range, ID, lane, edge
reference를 각각 변조합니다. `execute`는 instruction lookup 전에 `llvm::Error`를 반환해야 하고,
`lowerToMedIR`는 index 생성이나 입력 비례 allocation 전에 전체 malformed/over-budget LowIR를
거부해야 합니다. `lowerToMedIR`는 option validation, resource validation, structure validation을
이 순서로 수행하고 field별 `canonical decode replay` 및 `lowerCanonicalLowToMedIR`보다 먼저
완료해야 합니다. public HighIR recovery는 외부 LowIR/MedIR를 replay 검증하며 `analyze`만 자체
canonical IR에 `lowerCanonicalLowToMedIR`와 `recoverCanonicalHighIR`를 사용할 수 있습니다.
따라서 recursive/duplicate replay를 피하면서 모든 HighIR option/resource budget을 계속 적용합니다.
interpreter는 `EVMInterpreterLimits.def`의 모든 limit를 exact boundary/+1로
검증합니다. `MaxSteps`는 전용 `StepLimit`를 유지하고, `MaxMemoryBytes`, `MaxTraceEntries`,
`MaxLogEntries`, aggregate `MaxLogDataBytes`, runtime `MaxPersistentStateEntries` exhaustion은
`ResourceExhausted`로 transaction effect를 rollback합니다. 초기 aggregate
`MaxHostReturnDataBytes`나 persistent state 초과는 API error입니다. 초기 `MaxCalldataBytes`,
`BlockHashes`/`Balances`/`CodeHashes`/`ExternalCode`/`BlobHashes` 전체의 aggregate
`MaxHostEnvironmentEntries`, aggregate `MaxExternalCodeBytes`도 API error입니다.
`const execute preflight`는 environment, snapshot, result copy 전에 이를 거부합니다. return-data
`ArrayRef` view와 정렬 table의 `lower_bound` lookup도 buffer copy나 PC map 없이 검증합니다.

별도의 LowIR boundary test는 aggregate diagnostic limit `MaxLowDiagnostics`와
`MaxLowDiagnosticBytes`를 검증하며 linear decode/CFG construction이 정확한 count/final byte를
precharge하고 zero를 거부하는지 확인합니다.
HighIR safety test는 lane별 정렬 `Any/Exact/Excluded` domain, equality match/exclusion, raw
`XOR(selector, constant)`의 false-edge match/true-edge mismatch, zero word/calldata size/call
value refinement, unknown condition fail-closed를 다룹니다.
`EQ`와 `raw XOR` 두 back-jump regression은 다른 function이 `arguments`, `mutability`,
`return shape`, `region`을 오염시키지 않음을 보장합니다. `EVMAnalysisLimits.def`의
`MaxHighDispatchCandidates`, aggregate `MaxHighRecoveredArguments`, `MaxHighDiagnostics`,
`MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`, `MaxHighMemoryTransferCells`,
`MaxHighMemoryValueVisits`는 exact boundary/-1로 검증됩니다. fixed malformed diagnostic을 포함한
모든 output diagnostic은 allocation 전에 count와 최종 byte를 과금해야 합니다.
LowIR와 HighIR diagnostic budget은 독립적으로 검증하며 default root CFG region은 block-PC list를
reserve/copy하기 전에 `MaxHighRegionBlockReferences`를 과금해야 합니다.
외부 CALL/CREATE result는 nondeterministic host outcome으로 두 개의 정확한 CFG edge를 검사하므로
ERC-1167 fallback recovery가 유지됩니다. 읽을 수 없는 selector condition은 Unknown으로 남아
fallback/function fact를 만들 수 없습니다.

control-flow test는 `EVMLowFaultKinds.def`의 `InvalidJumpDestination`을
`end-of-code JUMPI`에 적용합니다. invalid target에서 확실히 true면 successful tail 없이 definite
fault이고 확실히 false면 성공합니다. unknown은 성공 가능한 false path를 남기며 lane 전체를 definite
fault로 표시하지 않습니다.

ABI test는 `EVMABIParserLimits.def`의 grammar boundary와 `EVMABITableLimits.def`의 public table
cardinality/text boundary를 exact limit/+1로 검증합니다. 또한 invalid kind/standard/evidence enum,
metadata mismatch, noncanonical signature/return list, 잘못 independent로 표시된 shared selector,
dangling/duplicate variant, word width가 아닌 event-topic `APInt`를 indexed selector/sorted topic
lookup 전에 거부합니다.

`NeverDEVMOpcodeTests`는 metadata architecture도 강제합니다. 할당된 opcode의
encoding/typed-value roundtrip, family boundary, hardfork alias, derived stack/host
maxima를 검증합니다.

### Solana SBF 차등 백엔드

SBF 메타데이터 테스트는 모든 버전 기능, opcode 충돌 경계, Murmur3 syscall hash, 재배치, ELF machine, 레지스터, VM 주소 상수를 검증합니다. Loader fixture는 vendored 바이너리 없이 레거시 v0-v2 section 레이아웃과 section이 없는 엄격한 v3/v4 program-header 레이아웃을 모두 생성합니다.

`NeverDSBFISAConformanceTests`는 v0-v4 각 version의 모든 byte encoding을 독립적으로
감사한 typed manifest와 대조합니다. `NeverDSBFExternalOracleTests`는 activation 및
boundary 결정을 별도로 build한 official Anza process와 비교합니다.
`NeverDSBFUpstreamConformanceTests`는 pinned Anza revision의 ELF 23개 모두에 명시적
outcome을 부여합니다.

`NeverDSBFSemanticTests`는 검증된 명령 바이트를 직접 실행하고 MedIR을 사용하지 않으므로, 정규화된 IR을 변경하거나 손상해도 source oracle과 backend가 우연히 일치할 수 없습니다. 비단조 v2 시맨틱, 메모리, syscall, 내부 call frame, fault, trace, resource limit을 다룹니다. LLVM module은 검증하며, 생성 C는 warning을 error로 처리하고 Rust는 `-D warnings`로 컴파일합니다. 공개 API 테스트는 생성된 엄격한 SBF ELF에서 모든 IR 단계, 디스어셈블리, CFG, 메타데이터, LLVM, C, Rust를 통과합니다.

## 일회성 target

custom target은 의존성을 빌드한 뒤 host CPU에서 정한 병렬도로 CTest를 실행합니다.

| CMake target | 선택 범위 |
|--------------|-----------|
| `check-neverd` | 등록된 모든 테스트 |
| `check-neverd-semantic` | `NeverDSemanticTests`만 |
| `check-neverd-sbf` | 모든 `NeverDSBF*Tests` target/case |
| `check-neverd-patch-full` | `NeverDPatchFullTests`만 |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests`만 |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests`만 |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests`만 |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests`와 `NeverDAvxUpperXformTests`에는 현재
`check-neverd-*` 편의 target이 없습니다. 아래처럼 빌드하고 label로 선택하세요.
`check-neverd-semantic`에도 별도 변환이나 patch-full 바이너리는 포함되지 않습니다.
완전한 집계에는 `check-neverd`를 사용하세요.

## 증분 CTest 워크플로

소유 실행 파일을 먼저 빌드한 뒤 label을 선택합니다. 관련 없는 큰 의미론 target을
다시 링크하지 않아도 됩니다.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# 모든 집중 EVM target/case
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# 모든 집중 Solana SBF target/case
cmake --build build-release --target check-neverd-sbf --parallel 4
```

GoogleTest에서 파생된 CTest 이름으로 단일 회귀를 실행합니다.

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

유용한 selector:

| 명령 | 목적 |
|------|------|
| `ctest --test-dir build-release -N` | 발견한 사례를 실행하지 않고 나열 |
| `ctest --test-dir build-release -L '<regex>'` | 테스트 바이너리 label 선택 |
| `ctest --test-dir build-release -R '<regex>'` | 사례 이름 선택 |
| `ctest --test-dir build-release --output-on-failure` | 실패 진단만 표시 |
| `ctest --test-dir build-release --stop-on-failure` | 첫 실패 후 중단 |
| `ctest --test-dir build-release --parallel 4` | 최대 네 사례 병렬 실행 |

GoogleTest discovery는 `DISCOVERY_MODE PRE_TEST`를 사용하므로 CTest가 나열하기 전에
해당 테스트 바이너리가 있어야 합니다. 사례별 timeout과 독립 discovery timeout은
`cmake/AddNeverD.cmake`에 정의되며 측정된 무거운 사례가 있는 스위트만 늘릴 수 있습니다.

## 코드와 함께 바뀌어야 하는 테스트

| 변경 영역 | 먼저 시작 | 다음 고려 |
|-----------|-----------|-----------|
| 아키텍처 lifter 또는 decode | `NeverDLiftTests`의 이름 있는 사례 | 해당 ISA 의미론 왕복 |
| LowIR CFG, 함수 감지, jump table | Lift CFG/switch 사례 | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests` |
| MedIR, ABI, 플래그, 타입, SSA | MedIR/호출 규약 lift 사례 | ISA 교차 `NeverDSemanticTests` 사례 |
| HighIR 또는 구조화 C | HighIR/decompile 사례 | `NeverDCFGLoopXformTests` 및 생성 C 컴파일 검사 |
| PE/ELF/Mach-O loader 또는 입력 relocation | 해당 `unittests/lift` 포맷 fixture | 해당 셀의 전 단계 로드/디컴파일 테스트 |
| Rewrite codegen 또는 출력 relocation | `RewriteCodegenRTTests` 사례 | `NeverDPatchFullTests` 및 가능한 링크된 patch fixture |
| patch가 쓰는 LLVM IR 변환 | 집중 변환 바이너리 | `NeverDPatchFullTests` 조합 pass grid |
| C API 또는 CLI | 직접 SDK/query 테스트 및 `unittests/semantic/CLIEndToEndTests.cpp` | 관련 pipeline/포맷 스위트 |
| EVM loader, opcode, IR 또는 backend | 가장 작은 소유 `NeverDEVM*Tests` target | 모든 EVM target과 생성 C/Solidity 컴파일 검사 |
| SBF loader, ISA, IR 또는 backend | 가장 작은 소유 `NeverDSBF*Tests` target | 모든 SBF target과 생성 C/Rust 컴파일 검사 |
| Libc 인식 | `NeverDLibCTests` | 동작 변경 시 의미론 call/ABI 사례 |
| 힙 수명 감사 또는 복사 오버플로 헌트 | `NeverDSafetyTests` | `NeverDSafetyIntegrationTests`의 전체 6셀 |
| 프로세스 실행 또는 quoting | `NeverDTestProcessTests` | 지원 host마다 영향받는 CLI/의미론 사례 하나 |

테스트는 가장 낮은 안정 경계에서 계약을 표현해야 합니다. LowIR 모양 테스트는 lifter
귀속에 유용합니다. 그럴듯한 두 IR 모양이 다르게 동작할 수 있다면 의미론 왕복이
필요합니다. 작은 opcode, CFG, 관측 상태 assertion으로 충분할 때 함수 전체 golden
dump를 피하세요.

## CI 관계

CI는 Linux, macOS, Windows에서 테스트를 켠 Release를 빌드하고 발견 inventory를
audit한 다음 플랫폼별 label 제외를 적용합니다. 프로필은
`.github/workflows/ci.yml`과 `scripts/audit_ci_test_inventory.py`에 있습니다.
`NeverDSafetyTests`와 `NeverDSafetyIntegrationTests`는 모든 matrix 호스트에서
필수이며, 각 실행은 체크인된 동일한 PE, ELF, Mach-O × x86-64, AArch64 fixture를 읽습니다. 비싼 모든 스위트를 대표하는 단일 matrix shard는 없으므로 필요한 교차 도구를 갖춘 머신에서는 로컬 `check-neverd`가 가장 명확한 전체 병합 전 신호입니다.

## 현재 Solana SBF conformance 및 sanitizer profile

이 current list는 위의 짧은 SBF list를 대체합니다. source differential suite는 clang
외에 `rustc`가 필요하며 compiler skip은 coverage 누락입니다. 전체 aggregate에는
`NeverDSBFProgramImageTests`, `NeverDSBFMalformedCorpusTests`,
`NeverDSBFISAConformanceTests`, `NeverDSBFUpstreamConformanceTests`,
`NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`와 metadata,
loader, analyzer, semantic, emitter, integration target이 포함됩니다. integrated
profile은 변동하는 총계 대신 named target과 결과를 기록합니다.

sanitizer profile은 `build-sbf-asan-ubsan`에 별도로 build합니다. focused target을
fail-fast로 실행해 ASan/UBSan report가 없음을 확인합니다. prebuilt package에 필요한
fork-only header가 없으므로 integration은 integrated LLVM build에서 실행합니다.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```

### pinned SBF evidence snapshot (2026-08-24)

gate는 Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679`, Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`를 고정합니다. official ELF manifest는
23/23을 통과하고, `NeverDSBFExternalOracleTests`는 1,411 opcode/boundary case를
`SBFOfficialOracleProtocol.def`, `SBFOfficialVerifierCases.def`,
`SBFOfficialExecutionConstants.def`를 통해 대조합니다.
`SBFOfficialELFMutations.def`가 malformed ELF의 table-driven contract이며 변동하는
총계는 고정하지 않습니다.
별도 축인 `41-case strict ELF differential`은 strict-v3 matrix 전체를 official
`verify-elf-batch`와 NeverD에 통과시킵니다. 이 41 case는 1,411 total에 포함하지 않습니다.
`NeverDSBFAgaveConformanceTests`는 Firedancer test-vectors의
`68bb4af40235562e8852fa23d5727e49c2a0b862`를 인증하고 loader fixture 1,955 `sol_compat_elf_loader_v1`개
(accept 1,399, reject 556)를 대조하고, 승인된 각 ELF에 대해 `entry_pc`, `text_off`,
`text_cnt`, `rodata_hash`, `calldests_hash`를 비교합니다. 이 gate는 후속 instruction verifier를 실행하지 않습니다.

추가 official execution matrix는 별도입니다. active `(Version,Opcode)` case 정확히
508개와 boundary case 58개를 합쳐 exact execution case 566개입니다. 1,411개의
verifier probe나 `41-case strict ELF differential`을 대체하지 않으며 그 합계에도 넣지 않습니다.
Linux Release CI는 `--print-pinned-revision`, `--print-test-vectors-revision`,
`--print-toolchain`을 사용하고 `NEVERD_SBPF_ORACLE`과
`NEVERD_AGAVE_CONFORMANCE_ROOT`를 export하므로 두 external gate가 필수입니다. 명시
oracle/corpus env가 없는 local run은 case를 discover하지만 skip할 수 있습니다.

`SBF_RUNTIME_VERSION`에 따라 `RuntimeVersionPolicy::ChainProfile`은 historical
cluster/slot을 반영하며 official feature account activation에 맞춰 maximum ISA를
V0→V1→V2→V3으로 전진시킵니다. 현재는 V3입니다. 명시 v4는 offline 분석용
`RuntimeVersionPolicy::UpstreamToolchain`을 사용합니다. 현재 10 MiB
상한은 정확히 `10'485'760` byte이고, 65,536은 historical provenance/test뿐입니다.
`SBFFaultCodes.def`는 execution fault의 안정된 값을, `SBFSourceStatuses.def`는 별도
계층인 generated-source ABI를 소유합니다.

10,000 scale fixture가 worklist, function ownership, multi-latch를 보호하며 machine별
시간은 고정하지 않습니다. cluster/account/slot row는 일반 test를 deterministic 및
offline으로 유지하면서 `RPC activation audit`를 가능하게 합니다.
