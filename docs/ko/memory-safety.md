**언어**: [English](../memory-safety.md) | [简体中文](../zh-CN/memory-safety.md) | [繁體中文](../zh-TW/memory-safety.md) | [日本語](../ja/memory-safety.md) | [한국어](memory-safety.md) | [Français](../fr/memory-safety.md) | [Deutsch](../de/memory-safety.md) | [Español](../es/memory-safety.md) | [Italiano](../it/memory-safety.md) | [Русский](../ru/memory-safety.md) | [العربية](../ar/memory-safety.md)

[← 문서 색인](README.md)

# 메모리 안전성 검사

`neverd scan <binary>` 명령은 두 분석기를 함께 실행합니다. 하나만 선택하려면 `--analyzer=audit` 또는 `--analyzer=hunt` 옵션을 사용합니다. [통합 검사 및 보고서 형식(영문)](../memory-safety-scan.md).

NeverD는 적재된 바이너리에 대해 두 계열의 메모리 안전성 분석을 수행하고 구조화된 JSON으로 보고합니다. 두 트랙 모두 형식에 의존하지 않는 리프트된 IR 위에서 동작하므로 **PE/COFF, ELF, Mach-O는 동등한 1급 대상**입니다. 발견이 특정 형식의 스캐너나 가져오기 표 뒤에 숨지 않습니다.

| 트랙 | 명령 | 보고 내용 |
|------|------|-----------|
| **감사(Audit)** | `neverd scan <binary> --analyzer=audit` | 힙 객체 수명 결함과 초기화되지 않은 로컬 스택 읽기 |
| **헌트(Hunt)** | `neverd scan <binary> --analyzer=hunt` | 위험한 복사 오버플로의 기호 증거와 입력 후보(완전한 `process-input-v1` 계획이 있을 때만 `replayable=true`) |

엔진은 NeverD 자체 기호 실행과 비트벡터 솔버를 증거와 도달 가능성에 재사용합니다. 외부 솔버, VM, 컨테이너 의존은 없습니다.

---

<!-- i18n-section: core-invariant -->

## 핵심 불변식: 실패는 닫힘

리프트되지 않은 연산, ABI가 인수를 복구하지 못한 호출, 미해석 간접 대상, 또는 예산 소진은 모두 **UNKNOWN**이며 SAFE가 아닙니다. 용량을 복구할 수 없는 목적지 버퍼도 UNKNOWN입니다. 엄격한 리프팅은 그대로입니다. 안전성 계층은 그 위에 보수적 판정만 더합니다.

호출 효과는 폐쇄 세계 의미론을 사용합니다. 요약은 전제 조건과 관련 효과가 모두 알려진 경우에만 적용됩니다. 알 수 없는 효과나 일부만 적용 가능한 요약은 UNKNOWN으로 남으며, 빈 부분을 효과 없음이나 호출 성공으로 가정하지 않습니다.

---

<!-- i18n-section: identity -->

## 형식별 신원 계약

두 트랙 모두 lift 파이프라인이 필요합니다(호출별 인수를 복구하기 때문). 피호출 이름은 NeverD의 나머지와 같은 신원 뷰로 붙입니다. 디버그 정보 탐색 순서는 바뀌지 않습니다.

| 형식 | 디버그 정보(우선순위 높은 순) | 가져오기 / thunk 해석 |
|------|------------------------------|------------------------|
| **PE/COFF** | `--pdb`, 디버그 디렉터리 또는 옆의 `.pdb`, 그다음 MSVC `/MAP` | IAT 슬롯과 `__imp_` thunk, 서수 가져오기 |
| **ELF** | 이미지 내 DWARF, 분리 `*.debug`, 그다음 GNU/LLD MAP | PLT stub를 가져오기 이름으로 해석 |
| **Mach-O** | 이미지 내 DWARF, 인접 `.dSYM`, 그다음 ld64 `-map` | dyld bind / 간접 심볼 슬롯과 stub helper |

`--pdb` / `--map`은 권위 있는 동반 파일입니다. 읽기 실패는 오류이며 조용한 폴백이 아닙니다. `--no-debug`는 모든 형식에서 이미지만 읽습니다.

PDB 프로시저 시그니처는 값을 반환하는 할당 함수와 `void` 해제 함수를 구분하는 데 사용합니다. PDB의 지역 변수·스택 타입에 대한 풍부한 복원은 여전히 제한적입니다. 정확한 객체 크기를 확정할 수 없으면 헌트는 프레임/할당 지점 모델로 물러나 크기를 지어내지 않고 UNKNOWN을 보고합니다.

<!-- i18n-section: name-precedence -->

### 이름 출처 우선순위

각 발견은 `name_source`를 가지며 피호출 이름 출처를 다음 우선순위로 고릅니다.

1. `rename` — 호출자가 지정한 이름 변경
2. `import` — IAT(PE), PLT(ELF), 또는 dyld-bind / stub(Mach-O)
3. `export` / `symbol` — 이미지가 이미 명시한 내보내기 또는 심볼 표 이름
4. `pdb` / `dwarf` / `map` — 자리 표시자를 확정하거나 이미 명시된 이름과 일치하는 디버그 심볼
5. `sig` — 시그니처 일치
6. `synthetic` — 이름 없는 루틴의 자리 표시자

DWARF가 붙인 정적 링크 `memcpy`는 `dwarf`, 가져온 `memcpy`는 모든 형식에서 `import`입니다. 시그니처 일치는 디버거나 가져오기 표가 이미 밝힌 이름을 대체하지 않습니다.

---

<!-- i18n-section: catalog -->

## 싱크와 소스 카탈로그

카탈로그는 설정 가능한 표이며 하드코딩된 집합이 아닙니다. 각 **싱크**는 취약점 부류, 역할(copy, format, alloc, free, realloc), 관련 인수 슬롯(목적지, 원본, 길이, 용량)을 선언합니다. JSON의 copy 또는 format 싱크는 실행 가능한 호출 effect도 제공합니다. 각 **소스**는 공격자 영향을 받는 입력 제공자입니다.

내장 항목은 [`SafetySinks.def`](../../include/neverd/safety/SafetySinks.def)와 [`SafetySources.def`](../../include/neverd/safety/SafetySources.def)에 있으며, 흔한 C 런타임 복사 계열(`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), 명시적 목적지 상한을 가진 강화 `_chk` 변형, 할당·해제 계열(`malloc`/`calloc`/`realloc`/`free`, operator `new`/`delete`), 선택적 Win32 힙 API를 다룹니다. 입력원은 POSIX(`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, 프로그램 인수) **및** Win32(`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`)입니다. PE 헌트가 POSIX 입력에만 묶이지 않습니다.

형식별 철자는 한 항목으로 접힙니다. 선행 밑줄을 제거하고(`_malloc`, `___strcpy_chk`) 맹글된 operator new/delete는 별칭으로 맞춥니다.

JSON copy 또는 format 싱크에서 `effect`를 생략하면 참조된 가장 높은 인수 슬롯으로 적용 조건을 추론합니다. copy는 그 정확한 인수 개수를 요구하고, format 싱크는 그 최소 인수 개수부터 가변 인수 상한까지의 호출을 허용합니다. 선택적 `effect` 객체는 `min_arity`와 `max_arity`(또는 `"variadic"`)로 추론된 copy의 정확한 인수 개수를 넘는 추가 wrapper 인수까지 포함한 허용 arity 범위를 명시적으로 설정할 수 있습니다. `min_arity`는 참조된 가장 높은 역할 슬롯에 1을 더한 값 이상이어야 하며, `formats`와 `abis`는 적용 조건을 제한합니다. 호출의 인수 개수, 오브젝트 형식 또는 ABI가 일치하지 않으면 요약을 적용하지 않으며 폐쇄 세계 규칙에 따라 결과는 UNKNOWN으로 남습니다.

명세 파일로 카탈로그를 확장하거나 덮어쓸 수 있습니다.

```bash
neverd scan --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 },
    { "name": "my_format", "kind": "format", "dst": 0, "fmt": 2,
      "effect": { "min_arity": 3, "max_arity": "variadic",
                  "formats": ["elf"], "abis": ["sysv"] } }
  ],
  "sources": [
    { "name": "my_read", "out": 1, "return_tainted": true }
  ]
}
```

사용자 정의 소스에서 `out`과 `return_tainted`는 발견용 메타데이터일 뿐입니다. 실행 가능한 메모리, 반환값 또는 taint effect를 확립하지 않습니다. 현재 소스 스키마에는 해당 의미에 필요한 타입 지정 성공 조건, 변경, 형식 및 ABI 계약이 없으므로 사용자 정의 소스 effect에 의존하는 분석은 UNKNOWN으로 남습니다. 내장 소스는 영향을 받지 않으며, 적용 조건을 검사한 타입 지정 디스크립터가 계속 실행 가능한 effect를 제공합니다.

목적지 인수만 있는 무제한 사용자 정의 싱크는 같은 이름의 소스 항목에서 추론되지 않습니다. `gets`와 같은 사용자 정의 싱크는 `"unbounded": true`를 명시적으로 활성화해야 합니다. 같은 이름을 소스 카탈로그에 추가해도 실행 가능한 effect가 부여되지 않으며, 서로 모순되는 원본/길이 필드는 트랜잭션 단위로 거부됩니다.

---

<!-- i18n-section: copy-overflow -->

## 헌트: 복사 오버플로 판정

각 복사 싱크에 대해 헌트는 목적지 용량을 다음 순서로 복구합니다. 디버그가 선언한 배열 크기, 알려진 크기의 힙 할당 지점, 건전한 스택 프레임 상한. 쓰기 길이를 결정하는 인수는 스택 슬롯 spill/reload를 따라가는 역방향 SSA 보행으로 분류합니다.

- **상수 길이**가 정확한 용량 안에 있으면 SAFE입니다. 상수 오버플로는 입증된 경로에서 싱크에 도달할 수 있을 때만 UNSAFE이며, 그렇지 않으면 UNKNOWN입니다.
- **강화** `_chk` 복사는 런타임 목적지 상한을 가집니다. 요청이 거부되거나 그 상한이 복구된 객체 안에 들어간다고 증명되면 SAFE, 객체 밖 쓰기가 가능하면 UNSAFE, 상한을 복구하지 못했거나 결론이 나지 않으면 UNKNOWN입니다.
- **증명 가능하게 유계인 길이**(길이를 반환하는 호출, 마스크, 클램프)는 풀이 전에 제외하고 이유를 기록합니다. 목적지 크기가 정확할 때만 SAFE이며, 포함 영역 상한만 있으면 UNKNOWN을 유지합니다.
- **공격자 영향을 받는 길이**이고 용량이 알려지면 비트벡터 솔버로 검사합니다. 용량보다 큰 길이가 충족 가능하면 UNSAFE입니다. 후보는 완전한 `process-input-v1` 계획을 만들 수 있을 때만 재생 가능합니다. 초기 범위는 정확한 리터럴 환경 값과 첫 표준 입력 소비가 반환한 바이트까지입니다. argv, 파일, 네트워크, 사용자 정의 또는 모호한 입력은 이유와 함께 재생 불가로 남습니다.
- 그 외(알 수 없는 길이 또는 알 수 없는 용량)는 UNKNOWN.

복구된 용량은 항상 실제 객체 크기의 **상한**이므로 증명된 오버플로는 거짓 양성이 아닙니다.

<!-- i18n-section: formatted-input -->

### 서식 입력

`scanf`/`fscanf`와 버전이 붙은 표기에서 읽을 수 있는 상수 서식은 억제되지 않은 각 변환을 실제 가변 인수 출력 인수에 대응시킵니다. 무제한 `%s`/`%[` 출력은 이후 문자열 사용에 taint를 전파하고, 숫자 및 문자 출력은 출력 포인터 값 자체가 아니라 쓰인 객체에서 로드한 값에 taint를 전파합니다. `sscanf`는 입력 문자열이 이미 공격자 영향을 받는 경우에만 이러한 effect를 전파합니다. `%Ns`/`%N[` 같은 유계 텍스트 출력은 종결 문자를 포함한 `MaxBytes` extent와 함께 taint를 전파하며, 와이드 문자 변형은 플랫폼의 `wchar_t` 너비를 사용해 그 바이트 extent를 계산합니다. 억제된 변환, 초과 인수, 위치 의존 또는 미지원 서식, `%n`은 추측하지 않고 UNKNOWN으로 남깁니다.

<!-- i18n-section: formatted-output -->

### 서식 출력

`snprintf`/`vsnprintf`의 최대 쓰기 길이와 강화된 `_chk` 형식의 컴파일러 제공 대상 객체 크기는 서로 다른 경계입니다. 리터럴과 `%%`만 있는 신뢰 가능한 상수 서식은 종료 NUL을 포함한 정확한 출력 길이를 가지며 복사 sink와 같은 힙/스택 모델로 검사합니다. 최대 쓰기가 정확한 용량 안이면 SAFE, 리터럴 출력이 대상을 넘으면 UNSAFE입니다. 값 변환은 인수 출력 크기가 증명되기 전까지 UNKNOWN입니다. 높은 신뢰도의 UNSAFE에는 LowIR 경로 재생으로 호출 도달 가능성도 증명해야 합니다. 공격자가 제어하는 서식은 잘림 여부와 별도로 `format_string`으로 보고합니다.

---

<!-- i18n-section: heap-lifetime -->

## 감사: 힙 수명 판정

각 할당에 대해 감사는 CFG에서 핸들을 추적하고(스택 spill/reload 포함) 탈출 요약(반환, 비스택 주소에 저장, 불투명 피호출에 전달)을 적용합니다.

- **누수** — 핸들이 해제되지도 탈출하지도 않음.
- **이중 해제** — 어떤 경로에서 두 번째 해제가 첫 번째 이후에 도달 가능.
- **해제 후 사용** — 해제 이후 역참조 또는 불투명 사용이 도달 가능.

할당·해제 **래퍼**는 함수별 탈출 요약으로 인식하므로 `malloc`/`free` 전달 함수가 결함을 가리지 않습니다. 상호 배타 분기 위의 해제는 이중 해제로 보고하지 않습니다.

힙 상태 기계는 먼저 후보 이벤트 시퀀스(할당, 해제, 사용 또는 반환 종료)를 내놓습니다. 두 번째 패스가 그 시퀀스를 기호 LowIR 경로에서 순서대로 재생하고 경로 술어의 충족 가능성을 증명해야만 발견이 높은 신뢰도의 UNSAFE가 됩니다. LowIR 부재, 불투명 연산, 요약 없는 호출, 솔버 불확실성, 탐색 한도는 모두 후보를 UNKNOWN으로 낮춥니다. 보수적 may-alias 메모리 havoc은 따로 추적하므로 평범한 스택 프레임 저장이 본래 정확한 도달 가능성 증거를 무효화하지 않습니다.

---

<!-- i18n-section: stack-initialization -->

## 감사: 지역 스택 초기화

함수 진입 스택 포인터 아래의 지역 슬롯에 대한 전체 폭 쓰기와 후속 읽기를 추적합니다. 선행 초기화가 전혀 가능하지 않은 읽기는 `uninitialized_read`입니다. 높은 신뢰도의 UNSAFE에는 LowIR 경로 재생으로 읽기에 도달함을 확인해야 합니다. 조건부 초기화, 부분 쓰기, 슬롯 주소 이탈 등 불확실한 정의는 UNKNOWN으로 남습니다. 진입 스택 포인터 이상에 있는 호출자 소유 인수 슬롯은 제외합니다.

---

<!-- i18n-section: reachability -->

## 알려진 진입점 기반 프로시저 간 도달 가능성

각 발견에는 서로 혼동하면 안 되는 세 가지 독립된 판단이 있습니다.

| 필드 | 질문 | 값 |
|------|------|----|
| `verdict` | 로컬 안전성 분석이 해당 연산에 대해 무엇을 증명했는가? | `SAFE`, `UNSAFE`, `UNKNOWN` |
| `reachability.status` | 포함 함수가 알려진 네이티브 진입점에서 복구된 제어 경로에 있는가? | `REACHABLE`, `UNREACHABLE`, `UNKNOWN` |
| `reachability.attacker_control` | 인수 슬라이스가 이 발견에서 공격자 영향에 대해 무엇을 증명했는가? | `TAINTED`, `BOUNDED`, `UNKNOWN` |

도달 가능성은 추가 증거이며 발견의 `verdict`, 집계 판정, CLI 종료 코드를
바꾸지 않습니다. 따라서 로컬에서 증명된 오버플로가 `verdict=UNSAFE`이면서
`reachability.status=UNREACHABLE`일 수 있습니다. 실행 가능한 공격 경로를
요구하는 소비자는 두 필드를 모두 검사해야 합니다.

루트는 인식된 애플리케이션 진입점(`application`, 예: `main`, `WinMain`), 이미지
진입점(`image`), 내보낸 루틴(`export`)입니다. 한 함수에 여러 신원이 있으면 결정적
우선순위는 `application`, `image`, `export`입니다. `reachability.entry`는 `va`,
`name`, `kind`를 기록합니다. 도달 가능한 비루트 발견의 `call_chain`은 정확한 내부
에지의 가장 짧고 결정적인 경로이며, 각 에지는 `caller_va`, 호출 지점 `call_va`,
`callee_va`, 그리고 `direct` 또는 `indirect`인 `kind`를 담습니다.

`UNREACHABLE`은 루트가 있고 내부 호출 인벤토리가 완전하며 깊이 예산을 소진하지
않았는데도 경로가 없을 때만 출력됩니다. 다른 양의 증거로 아직 도달하지 못한 함수에서는
루트 부재, 중복되거나 모호한 함수 신원, 불일치하는 CFG/호출 인벤토리, 미해석 실행 가능
내부 대상, 깊이 소진이 부정 증명을 막아 `reachability.status=UNKNOWN`을 만들며 해당하면
`reason`과 `budget_hit`를
제공합니다. 알 수 없는 ABI, 불일치하는 인수 너비, 가변 인수 전용 슬롯, 불완전한
슬라이스, 깊이 또는 요약 예산 소진도 아직 증명되지 않은 공격자 제어를 UNKNOWN으로
유지합니다. 이미 증명한 사실은 유효하며 전파를 추측하지 않습니다.

---

<!-- i18n-section: budgets -->

## 예산, 출력, 바인딩

헌트 탐색과 솔버는 예산으로 제한됩니다(`--max-paths`, `--max-steps`, `--max-loop`, `--solver-conflicts`). 프로시저 간 분석에서는 `max_call_depth`가 알려진 진입점부터의 내부 호출 에지 수를 제한하고 `max_summary_iterations`가 공격자 제어 고정점 라운드를 제한합니다. 기본값은 각각 64개 에지와 유효 깊이 한도에 1을 더한 라운드 수입니다. 예산 소진은 위 설명처럼 fail closed입니다. `max_call_depth` 소진은 아직 도달하지 못한 함수에 `status=UNKNOWN`을 남길 수 있습니다. `max_summary_iterations` 소진은 구조적 증거를 지우지 않으므로 `status=REACHABLE`, `attacker_control=UNKNOWN`, `budget_hit=true`가 함께 존재할 수 있습니다. `neverd scan`은 JSON을 출력하며 `-o`로 출력 파일을 지정할 수 있습니다. 종료 코드는 SAFE가 `0`, UNSAFE가 `2`, UNKNOWN 또는 오류가 `1`입니다.

모든 공개 인터페이스에서 0은 엔진 기본값을 선택합니다.

| 인터페이스 | 제어 깊이 | 공격자 요약 |
|------------|-----------|-------------|
| CLI(`scan`) | `--max-call-depth <n>` | `--max-summary-iterations <n>` |
| C(`neverd_safety_options`) | `max_call_depth` | `max_summary_iterations` |
| Python(`Session.scan()`) | `max_call_depth=<n>` | `max_summary_iterations=<n>` |

C 호출자는 `neverd_safety_options`를 0으로 초기화하고
`struct_size=sizeof(neverd_safety_options)`를 설정합니다. 이전 구조체 크기는 기본값을
유지합니다. Python은 두 값을 부호 없는 32비트 정수로 검증합니다.

C에서는 `neverd_session_scan_json`을, Python에서는 `Session.scan()`을 호출합니다. C에서 분석 한도는 `neverd_scan_options.analysis`로 전달합니다.

<!-- i18n-section: finding-schema -->

### 발견 스키마

```json
{
  "class": "buffer_overflow",
  "function": "parse_header",
  "name": "strcpy",
  "name_source": "import",
  "call_va": "0x11a4",
  "source": "reader.c:42",
  "sink": "strcpy",
  "arg_index": 1,
  "flow": "TAINTED",
  "verdict": "UNSAFE",
  "confidence": "HIGH",
  "capacity": 16,
  "capacity_kind": "exact",
  "corroboration": "path predicate and overflow are jointly satisfiable",
  "reachability": {
    "status": "REACHABLE",
    "attacker_control": "TAINTED",
    "budget_hit": false,
    "entry": { "va": "0x1000", "name": "main", "kind": "application" },
    "call_chain": [
      { "caller_va": "0x1000", "call_va": "0x1080",
        "callee_va": "0x1100", "kind": "direct" }
    ]
  },
  "evidence": {
    "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" },
    "candidate_values": [
      { "name": "copy_length", "value": "17" },
      { "name": "argv[1]", "value": "16 bytes" }
    ],
    "replayable": false,
    "replay": {
      "adapter": "process-input-v1",
      "reason": "argv input is not supported by process-input-v1"
    },
    "symbolic_model": [
      { "id": 0, "name": "copy_len", "width": 64,
        "value_hex": "0x11", "origin": "input" }
    ]
  }
}
```

`replayable`은 독립적인 약속이 아니라 파생된 증거입니다. `replay`에 `process-input-v1` 어댑터용 완전한 입력 계획이 있을 때만 참입니다. 계획은 정확한 환경 바이트, 사용한 경우 첫 표준 입력 바이트열, 솔버 할당 ID에서 해당 입력으로의 바인딩을 기록하며, 만들 수 없으면 `replay.reason`이 이유를 설명합니다. 재생과 도달 가능성 필드들은 추가적이며 최상위 `schema_version`은 계속 `1`입니다.

---

<!-- i18n-section: strict-publication -->

## 엄격한 실행 가드와 인증된 게시

`binary-sanitizer-v1`은 audit/hunt 판정을 바꾸지 않는 별도의 실험적 변경 트랜잭션입니다. 완료된 엄격 hunt는 모든 발견을 정확한 잔여 용량과 일치하는 컴파일러 호출 지점 메타데이터를 갖춘 하나의 정확한 횟수 기반 쓰기 발생에 대응시켜야 합니다. 미지원 발견, 오래되거나 모호한 신원, 불완전한 리프트, 예산 소진, 가드 생성 오류 및 대상/서명 제한은 전체를 거부합니다. 부분 성공은 없습니다. 재배치 객체, 동적 라이브러리, 범용 Mach-O 및 가드 대상 Mach-O bundle 멤버도 거부합니다.

공개 진입점은 C `neverd_session_sanitize`, CLI `neverd patch --sanitize=strict`, Python `Session.sanitize`입니다. 인증 게시가 필요한 C 호출자는 먼저 `neverd_sanitize_publication_abi_version()`을 조회해야 합니다. 세 인터페이스 모두 완전하고 내부적으로 일관된 publication receipt v1이 있어야 성공합니다. 비 Darwin 호스트는 입력 인증/정규화 후, 리프트·가드 생성·후보 생성·네임스페이스 변경 전에 `UNSUPPORTED_TARGET`을 반환합니다.

Darwin에는 두 성공 형태가 있습니다. `CREATE_EXCLUSIVE`는 같은 디렉터리의 새 후보를 존재하지 않는 대상에 단 한 번의 원자적 덮어쓰기 금지 연산으로 게시합니다. receipt는 네임스페이스 원자성, 배타적 생성, 실제 피연산자 결합을 증명하지만 교체 CAS나 충돌 후 내구성은 보장하지 않습니다. 읽기 전용 `NO_CHANGE`는 빈 가드 계획에서 유지된 로드 원본 객체를 재인증하고 `NOT_PUBLISHED`를 보고합니다. 게시 보장이나 피연산자 결합은 주장하지 않습니다. 가드 계획은 원본을 출력으로 쓸 수 없고 다른 기존 대상도 교체하지 않으므로 새 경로가 필요합니다. 불확정 게시 또는 불완전한 최종 receipt는 실패입니다. 대상이 존재할 수 있으므로 사용·재시도·삭제 전에 확인해야 합니다.

원본 바이트는 session의 외부 다이제스트와 일치시킨 뒤 유지된 기술자로 관찰합니다. 원본과 대상 디렉터리 객체는 트랜잭션 동안 고정하지만, 메타데이터와 신원은 관찰 시점의 사실일 뿐 최초 로드부터 불변임을 증명하지 않습니다. 열린 디렉터리는 이름이 바뀔 수 있습니다. receipt는 해당 객체 안의 게시를 인증할 뿐 원래 경로가 실행 중이나 반환 후 계속 그 객체를 가리킨다고 증명하지 않습니다. 독립 검증 가능한 영속 경로 결합이 아니며, 나중에 경로를 다시 열 때는 외부 기준을 유지하고 재인증해야 합니다. Darwin rename은 후보 기술자 대신 경로를 쓰므로 디렉터리는 유효 uid 소유, 확장 ACL 없음, 그룹/기타 쓰기 권한 없음, 일반 POSIX 소유권을 확인하는 볼륨이라는 조건을 만족해야 합니다. 후보 생성과 게시 전에 다시 검사합니다. 동일 권한 프로세스, root/DAC 우회, 악성 파일 시스템/서버는 보장 범위 밖이며 커널·VFS·파일 시스템이 보고한 의미를 따른다고 가정합니다.

---

<!-- i18n-section: native-replay -->

## 네이티브 프로세스 재생: Phase 0만 제공

플랫폼 중립 `process-replay-v1` 계획과 실행 조정기는 향후 인증 실행의 증명 요건을 정의하지만 현재 이를 실행할 호스트는 없습니다. `NativeProcessReplayAdapter`는 C++ 가용성/팩터리 경계이며 C, Python, CLI 또는 JSON 실행 인터페이스가 아닙니다.

조회는 변경 없이 전체 계획, 실행 제한, 절대 실행 파일 위치와 유일한 물리 호출 지점 맵을 검증합니다. 대상을 열거나 백엔드를 호출하거나 프로세스를 실행하지 않습니다. 위치 정보는 향후 객체 탐색용일 뿐 인증 후 경로 실행 권한이 될 수 없습니다. 격리, 신원, 입력 중재, 발생 인증, 제한, 정리 보장 중 하나라도 없으면 `Available`과 모든 기능이 false이고 팩터리는 연산 테이블 없이 오류를 반환합니다.

현재 모든 호스트가 이 상태입니다. Linux에는 신뢰 가능한 정적 ELF 계측, 지속 감독기, 완전한 격리·제한·실행 인증이 더 필요합니다. macOS의 지원 공개 기능은 유지된 객체 실행, 대상 코드 전 임의 대상 샌드박스 설치, 경쟁 없는 프로세스 트리 격리를 모두 제공하지 못합니다. 다른 플랫폼도 미지원입니다. 단위 테스트 연산 테이블은 조정기 계약만 검증하며 네이티브 가용성 증거가 아닙니다.

---

<!-- i18n-section: scope -->

## 거짓 양성 경계와 범위

- 용량은 정확하거나 실제 객체 크기의 상한이므로 UNSAFE는 실제 오버플로를 반영합니다. 정확한 선언 크기가 없고 포함 영역 상한만으로 안전을 증명할 수 없으면 UNKNOWN입니다.
- 길이 제한 복사는 풀이 전에 제외되어 `skipped`에 집계됩니다. 정확한 용량은 SAFE를 증명할 수 있지만 상한만 있으면 UNKNOWN을 유지합니다.
- 카탈로그에 등록된 와이드 문자 및 이어붙이기 복사는 요소 너비와 기존 목적지 길이를 복구할 때까지 UNKNOWN입니다. 출력 매개변수 할당자와 조건부 `realloc` 소유권도 핸들 전이를 증명할 수 없으면 UNKNOWN입니다.
- **Phase 1**: 이번 릴리스는 세 형식의 sink 목록, 인수 사전 필터, 스택/전역 복사 오버플로 탐색, 힙 수명 감사, 지역 스택 미초기화 읽기 및 서식 문자열 검사를 제공합니다. 모든 테스트 호스트에서 PE, ELF, Mach-O × x86-64, AArch64의 등록된 fixture가 필수입니다.
- 더 풍부한 PDB 스택 형식과 추가 플랫폼 할당기는 점진적 확장 범위입니다. 정확한 요약이 없으면 UNKNOWN을 유지합니다.
- 현재 슬라이스는 알려진 진입점, 구조적 프로시저 간 도달 가능성, 공격자 매개변수의 단조 전파를 다룹니다. 별도의 실험적 `lowir-concolic-v1` 어댑터는 이제 필수 네이티브 포맷/아키텍처 매트릭스에서 레지스터 seed로 재생 검증된 브랜치 flip을 제공합니다. 이는 항상 비포괄적이며 안전성 verdict를 변경하지 않습니다. 실험적 `binary-sanitizer-v1`은 이제 Darwin에서 모든 지점을 보호하지 못하면 거부하는 counted-write 가드와 인증된 게시를 제공하지만, receipt는 트랜잭션 동안 보유한 디렉터리 object만 인증하며 원래 pathname의 영구적이고 재검증 가능한 binding은 아닙니다. 더 넓은 `process-replay-v1`은 계속해서 plan, coordinator, 가용성의 fail-closed Phase 0 경계만 제공하며 네이티브 replay를 실행하는 호스트는 없습니다.
