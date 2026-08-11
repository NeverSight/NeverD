**언어**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# Windows 예외 재구성

[← 문서 색인](README.ko.md)

NeverD는 Windows 테이블 기반 예외 정보를 로드, lift, 디컴파일, 바이너리 재작성 전 과정에
전달합니다. 예외 metadata는 함수 실행 계약의 일부입니다. 생성 코드, runtime-function
record, 언어 테이블, guard table의 일관성을 증명할 수 없는 재작성은 거부됩니다.

이 문서는 세 가지 지원 수준을 구분합니다.

- **분석**: 네이티브 표현을 검사된 정규화 record로 decode하여 IR pipeline에 제공.
- **디컴파일**: reducible protected region은 명시적 HighIR 예외 node로 만들고, 나머지는
  handler나 state transition을 잃지 않는 결정적 네이티브 주석으로 유지.
- **네이티브 재구성**: patch mode가 LLVM으로 완전한 대체 예외 계약을 생성하고 최종 PE에 설치.

분석 지원이 네이티브 재구성 지원을 의미하지는 않습니다.

## 지원 표

| 네이티브 형식 | Lift/분석 | 고수준 출력 | Patch mode |
|---------------|-----------|-------------|------------|
| x64 unwind v1/v2 | 완전하고 검사된 unwind record, operation, chain, handler data, provenance | frame/unwind 요약 및 해당 시 구조화 언어 region | 완전한 primary record 지원. 생성 `.pdata`/`.xdata`가 대체 closure를 갱신 |
| x64 unwind v3/APX | 전용 v3 payload, epilog, operation accounting | 명시적 v3 주석 | 분석 전용. 대상 함수 변경 거부 |
| ARM32/ARM64 packed unwind | function range, packed field, primary/fragment identity | frame/unwind 요약 | 완전한 비언어 primary record이고 독립 addressable fragment가 없을 때만 |
| ARM32/ARM64 unpacked unwind | 검사된 xdata header/code extent, handler association, fragment | frame/unwind 요약 | 완전한 비언어 primary record이고 독립 addressable fragment가 없을 때만 |
| `__C_specific_handler` | scope range, filter, finally target, handler, continuation target | reducible region은 `__try`/`__except`/`__finally`, 나머지는 주석 | 완전하고 표현 가능한 scope graph의 네이티브 x64 재구성 |
| `__CxxFrameHandler3` | unwind/try map, catch, catch-object/frame offset, continuation, IP-to-state map | reducible state interval을 명시적 C++ HighIR 및 C 호환 형식 주석으로 변환 | 아래의 좁은 verifier-clean subset에 대해 네이티브 x64 재구성 |
| `__CxxFrameHandler4` | action kind/object offset를 포함한 bounded variable-length decode | FH4 provenance를 가진 공통 HighIR graph | 분석 전용. 대상 함수 변경 거부 |
| `__GSHandlerCheck_SEH/EH/EH4` | wrapped personality와 검사된 GS cookie provenance | base language graph와 wrapper 주석 | 분석 전용. downgrade 없이 대상 함수 변경 거부 |
| x86 registration-chain EH | table-based EH와 구분 | unsupported-form 주석 | 재구성하지 않음 |

Malformed record를 완전한 일반 record로 취급하지 않습니다. partial decode는 조사에 쓸 수 있지만
네이티브 metadata 생성을 허가하지 않습니다. ARM xdata header가 bounded executable fragment
range를 증명해도 뒤 unwind body가 손상되면 range는 disassembly에 남지만 record는 malformed로
표시되고 patchable function으로 승격되지 않습니다.

## 정규화 모델

`ExceptionInfo`는 `BinaryImage`가 소유합니다. 각 `ExceptionFunction`은 다음을 가집니다.

- 검사된 half-open code range.
- primary, chained, fragment identity.
- native unwind encoding과 정확한 runtime/unwind provenance.
- 정규화 unwind operation/epilog 및 미해석 operation의 opaque operand bytes.
- 정확한 personality identity와 handler data.
- 선택적 SEH scope, C++ state map, GS cookie data.
- `Complete`, `Partial`, `Malformed` status와 결정적 diagnostic.

loader는 모델을 통해 raw file pointer를 노출하지 않습니다. native RVA는 진단과 patch 교체용으로
보존하고 IR consumer는 검증된 VA/range만 사용합니다.

image-wide index는 chained/fragment record overlap을 허용하고 address를 덮는 가장 구체적인
function을 반환합니다. 손상된 directory/range/pointer/count/state transition/compressed integer,
chain cycle, decode-budget exhaustion은 해당 parse status를 낮춥니다.

language-table limit는 개별 테이블과 함수 전체 정규화 graph에 모두 적용합니다. 여러 try-map
entry가 같은 handler map을 재사용해도 총 예산을 넘지 않습니다. 동일 `FuncInfo`와 personality를
공유하는 FH3 record는 bounded function group으로 decode하므로 부모 IP-to-state map은 자신의
catch funclet을 참조할 수 있지만 관련 없는 runtime function address는 거부합니다.

## IR 계약

예외 metadata는 일반 CFG 의미를 바꾸지 않고 모든 표현으로 전달됩니다.

- LowIR는 protected-range endpoint, state transition, filter, handler, cleanup action,
  continuation target에서 block을 분할.
- exceptional successor/predecessor를 일반 edge와 분리하여 dominator/structuring pass가 runtime
  dispatch edge를 machine branch로 오인하지 않게 함.
- MedIR는 정규화 function descriptor와 안정적 exceptional edge를 보존.
- HighIR는 별도 `SEHTry`/`CxxTry` statement 사용. clause descriptor는 native target VA,
  type descriptor, adjective, catch-object/parent-frame offset, cleanup action kind/object
  offset, state, continuation VA를 보존.

HighIR structurer는 interval-conservative합니다. 완전한 protected range 안에 모든 address가
있는 연속 statement slice만 이동하고 nested region은 안쪽부터 처리합니다. crossing region,
partial graph, address-less ambiguous boundary, out-of-line funclet은 원래 control flow에 남고
function의 unstructured-EH count를 증가시킵니다.

C backend는 reducible single-clause SEH region에 MSVC SEH syntax를 출력합니다. HighC는 C
backend이므로 C++ catch/cleanup state를 결정적 C-compatible comment로 출력하고 compilable
C++라고 주장하지 않습니다. out-of-line native funclet은 정확한 address를 보존합니다.

## LLVM metadata schema

emitted function과 연결된 모든 분석된 예외 function은 native WinEH lowering 대상이 아니어도
lossless LLVM metadata를 받습니다.

- function attachment: `neverd.windows.eh`
- native-lowering marker: `neverd.windows.eh.native`
- module table: `neverd.windows.eh.functions`
- current schema version: `3`

fixed record는 parse status, encoding, code range, native runtime/unwind RVA, runtime-record kind/
chain provenance, packed-unwind word, frame description, canonical/resolved personality name,
handler data, native unwind bytes, operation(native slot count 포함)/epilog, SEH scope, C++ header/
map, GS data, diagnostic, regeneration flag를 보존합니다. patch validation은 정확한 schema version과
loaded image에 대한 완전한 range match를 요구하며 예외 계약이 있는 auto-named lifted function은
attachment를 생략할 수 없습니다.

native x64 SEH lowering은 LLVM WinEH를 사용하고 전체 scope graph가 표현 가능할 때만
verifier-clean `invoke`/funclet control flow를 생성합니다. native FH3 조건은 다음과 같습니다.

- x64 COFF, unwind v1/v2, complete metadata, 유효한 synchronous FH3 state graph.
- `noexcept`, asynchronous, separated-funclet, GS-wrapper, FH4, unknown flag semantics 없음.
- protected interval은 nested 또는 disjoint이며 crossing하지 않음.
- destructor/unwind action, catch-object construction, parent-frame dependency 없음.
- handler는 lifted function 내부의 predecessor-free, call-free 일반 block.
- unwind 가능한 모든 protected operation은 LLVM `invoke`로 표현.

하나라도 맞지 않으면 LLVM은 lossless metadata와 함께 분석 가능하지만 patch planning은 native
language-table replacement를 거부합니다. PE entry point, TLS callback, CRT callback root는
preservation boundary이며 일반 ABI rewrite candidate가 아닙니다.

## Patch transaction

지원 rewrite는 하나의 PE transaction으로 처리합니다.

1. loaded exception graph와 LLVM metadata attachment에 대해 각 touched function 검증.
2. section identity/alignment/allocation, code/data trait, semantic symbol-index reference를 보존해
   replacement code compile. local Windows personality는 codegen 전에 externalize하여 emitted
   xdata를 입증된 original executable handler에 binding.
3. untouched runtime-function entry를 보존하고 touched primary function이 대체하는 native
   closure와 관련 chained record를 제거.
4. generated code/xdata를 relocate하고 generated/retained pdata를 merge/sort하며 overlap 거부.
   같은 personality class의 generated runtime-function record가 redirected language-EH entry를
   덮는지 증명한 뒤 단일 replacement exception directory 설치.
5. input CFG instrumentation mode 보존. `.gfids` reference와 redirected entry를 Guard CF table에,
   `.gehcont` reference를 generated executable VA로 해결해 Guard EH continuation table에 merge하고
   guard flag를 유지한 채 load-config 갱신. unresolved helper는 중단. CFW, return-flow guard,
   retpoline, XFG는 별도 codegen contract가 필요하므로 analysis-only.
6. 디스크 기록 전 완성 byte image를 다시 parse.

LLVM fork extension은 일반적입니다. final-image writer가 section trait와 semantic symbol-index
reference를 보존하며 PE/MSVC decode, policy, directory merge, load-config update, final validation은
NeverD에 남습니다.

original Guard CF/EH continuation entry는 original entry trampoline이 유효한 indirect target이므로
보존됩니다. generated target은 emitted code 안을 가리키고 결과 table은 strict RVA sort여야 합니다.

## 최종 image validation

다음 조건을 모두 만족하지 않는 patched PE는 거부합니다.

- LLVM이 bytes를 COFF object로 수용하고 PE machine/class/section table, optional-header directory
  bounds, image base/extent가 일치.
- section raw/virtual extent가 범위 내이고 section range overlap 없음.
- exception directory가 file-backed이며 image 내부.
- runtime-function entry가 sorted, nonempty, non-overlapping, fully executable.
- x64 unwind RVA/header/code array/version/flag/handler target이 유효하고 chain은 bounded/acyclic.
- final import/export/COFF symbol을 memory에서 재구성해 알려진 SEH/FH3 personality와 scope/state
  table을 완성 bytes에서 다시 parse.
- ARM runtime entry/xdata version/range가 유효하고 지원됨.
- guard flag가 table을 표시할 때 load-config에 Guard CF/EH continuation field 존재.
- guard pointer/count/stride가 PE image와 file 범위 안이며 entry가 strict-sorted executable target.

실패하면 patch를 중단하고 best-effort image를 기록하지 않습니다.

## 집중 검증

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

guarded x64 fixture는 `/guard:cf`와 `/guard:ehcont`로 cross-assemble/link합니다. 통합 테스트는
SEH scope/guard table, structured HighC, patched image reload, table count/order/executable target을
검증합니다.

별도 linked x64 FH3 fixture는 같은 transaction으로 지원 C++ closure를 검사하며 original fixed
table, HighC state annotation, personality binding, 재구성 try/catch graph, reload 후 IP-to-state
map을 검증합니다. parser 변경 시 공통 모델을 쓰는 ARM format case도 실행합니다.

## 네이티브 지원 확장

새 네이티브 재구성 지원에는 같은 변경 안에서 다음이 필요합니다.

- complete bounded parser와 normalized-model invariant.
- HighIR/LLVM metadata round-trip coverage.
- 새로 허용하는 모든 graph shape의 verifier-clean native IR.
- 필요한 emitted-section/semantic-reference retention.
- 정확한 architecture/personality/version의 linked PE fixture.
- exception-directory/load-config/final-image structural validation.
- 가장 가까운 unsupported shape의 explicit rejection test.

decode 가능하다는 이유만으로 allow-list를 넓히지 않습니다. 최종 linked image에서 runtime
exception behavior가 보존되는 것이 수용 기준입니다.
