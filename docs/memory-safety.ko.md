**언어**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← 문서 색인](README.ko.md)

# 메모리 안전성 감사와 헌트

NeverD는 적재된 바이너리에 대해 두 계열의 메모리 안전성 분석을 수행하고 구조화된 JSON으로 보고합니다. 두 트랙 모두 형식에 의존하지 않는 리프트된 IR 위에서 동작하므로 **PE/COFF, ELF, Mach-O는 동등한 1급 대상**입니다. 발견이 특정 형식의 스캐너나 가져오기 표 뒤에 숨지 않습니다.

| 트랙 | 명령 | 보고 내용 |
|------|------|-----------|
| **감사(Audit)** | `neverd audit <binary>` | 힙 객체 수명 결함: 누수, 이중 해제, 해제 후 사용 |
| **헌트(Hunt)** | `neverd hunt <binary>` | 위험한 복사 오버플로와 재현 가능한 구체적 증거 |

엔진은 NeverD 자체 기호 실행과 비트벡터 솔버를 증거와 도달 가능성에 재사용합니다. 외부 솔버, VM, 컨테이너 의존은 없습니다.

---

## 핵심 불변식: 실패는 닫힘

리프트되지 않은 연산, ABI가 인수를 복구하지 못한 호출, 미해석 간접 대상, 또는 예산 소진은 모두 **UNKNOWN**이며 SAFE가 아닙니다. 용량을 복구할 수 없는 목적지 버퍼도 UNKNOWN입니다. 엄격한 리프팅은 그대로입니다. 안전성 계층은 그 위에 보수적 판정만 더합니다.

---

## 형식별 신원 계약

두 트랙 모두 lift 파이프라인이 필요합니다(호출별 인수를 복구하기 때문). 피호출 이름은 NeverD의 나머지와 같은 신원 뷰로 붙입니다. 디버그 정보 탐색 순서는 바뀌지 않습니다.

| 형식 | 디버그 정보(우선순위 높은 순) | 가져오기 / thunk 해석 |
|------|------------------------------|------------------------|
| **PE/COFF** | `--pdb`, 디버그 디렉터리 또는 옆의 `.pdb`, 그다음 MSVC `/MAP` | IAT 슬롯과 `__imp_` thunk, 서수 가져오기 |
| **ELF** | 이미지 내 DWARF, 분리 `*.debug`, 그다음 GNU/LLD MAP | PLT stub를 가져오기 이름으로 해석 |
| **Mach-O** | 이미지 내 DWARF, 인접 `.dSYM`, 그다음 ld64 `-map` | dyld bind / 간접 심볼 슬롯과 stub helper |

`--pdb` / `--map`은 권위 있는 동반 파일입니다. 읽기 실패는 오류이며 조용한 폴백이 아닙니다. `--no-debug`는 모든 형식에서 이미지만 읽습니다.

PDB 프로시저 시그니처는 값을 반환하는 할당 함수와 `void` 해제 함수를 구분하는 데 사용합니다. PDB의 지역 변수·스택 타입에 대한 풍부한 복원은 여전히 제한적입니다. 정확한 객체 크기를 확정할 수 없으면 헌트는 프레임/할당 지점 모델로 물러나 크기를 지어내지 않고 UNKNOWN을 보고합니다.

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

## 싱크와 소스 카탈로그

카탈로그는 설정 가능한 표이며 하드코딩된 집합이 아닙니다. 각 **싱크**는 취약점 부류, 역할(copy, format, alloc, free, realloc), 관련 인수 슬롯(목적지, 원본, 길이, 용량)을 선언합니다. 각 **소스**는 공격자 영향을 받는 입력 제공자입니다.

내장 항목은 [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def)와 [`SafetySources.def`](../include/neverd/safety/SafetySources.def)에 있으며, 흔한 C 런타임 복사 계열(`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), 명시적 목적지 상한을 가진 강화 `_chk` 변형, 할당·해제 계열(`malloc`/`calloc`/`realloc`/`free`, operator `new`/`delete`), 선택적 Win32 힙 API를 다룹니다. 입력원은 POSIX(`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, 프로그램 인수) **및** Win32(`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`)입니다. PE 헌트가 POSIX 입력에만 묶이지 않습니다.

형식별 철자는 한 항목으로 접힙니다. 선행 밑줄을 제거하고(`_malloc`, `___strcpy_chk`) 맹글된 operator new/delete는 별칭으로 맞춥니다.

명세 파일로 카탈로그를 확장하거나 덮어쓸 수 있습니다.

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 }
] }
```

---

## 헌트: 복사 오버플로 판정

각 복사 싱크에 대해 헌트는 목적지 용량을 다음 순서로 복구합니다. 디버그가 선언한 배열 크기, 알려진 크기의 힙 할당 지점, 건전한 스택 프레임 상한. 쓰기 길이를 결정하는 인수는 스택 슬롯 spill/reload를 따라가는 역방향 SSA 보행으로 분류합니다.

- **상수 길이**가 정확한 용량 안에 있으면 SAFE입니다. 상수 오버플로는 입증된 경로에서 싱크에 도달할 수 있을 때만 UNSAFE이며, 그렇지 않으면 UNKNOWN입니다.
- **강화** `_chk` 복사는 런타임 목적지 상한을 가집니다. 요청이 거부되거나 그 상한이 복구된 객체 안에 들어간다고 증명되면 SAFE, 객체 밖 쓰기가 가능하면 UNSAFE, 상한을 복구하지 못했거나 결론이 나지 않으면 UNKNOWN입니다.
- **증명 가능하게 유계인 길이**(길이를 반환하는 호출, 마스크, 클램프)는 풀이 전에 제외하고 이유를 기록합니다. 목적지 크기가 정확할 때만 SAFE이며, 포함 영역 상한만 있으면 UNKNOWN을 유지합니다.
- **공격자 영향을 받는 길이**이고 용량이 알려지면 비트벡터 솔버로 검사합니다. 용량보다 큰 길이가 충족 가능하면 UNSAFE이고 솔버 모델이 구체적 증거입니다.
- 그 외(알 수 없는 길이 또는 알 수 없는 용량)는 UNKNOWN.

복구된 용량은 항상 실제 객체 크기의 **상한**이므로 증명된 오버플로는 거짓 양성이 아닙니다.

---

## 감사: 힙 수명 판정

각 할당에 대해 감사는 CFG에서 핸들을 추적하고(스택 spill/reload 포함) 탈출 요약(반환, 비스택 주소에 저장, 불투명 피호출에 전달)을 적용합니다.

- **누수** — 핸들이 해제되지도 탈출하지도 않음.
- **이중 해제** — 어떤 경로에서 두 번째 해제가 첫 번째 이후에 도달 가능.
- **해제 후 사용** — 해제 이후 역참조 또는 불투명 사용이 도달 가능.

할당·해제 **래퍼**는 함수별 탈출 요약으로 인식하므로 `malloc`/`free` 전달 함수가 결함을 가리지 않습니다. 상호 배타 분기 위의 해제는 이중 해제로 보고하지 않습니다.

힙 상태 기계는 먼저 후보 이벤트 시퀀스(할당, 해제, 사용 또는 반환 종료)를 내놓습니다. 두 번째 패스가 그 시퀀스를 기호 LowIR 경로에서 순서대로 재생하고 경로 술어의 충족 가능성을 증명해야만 발견이 높은 신뢰도의 UNSAFE가 됩니다. LowIR 부재, 불투명 연산, 요약 없는 호출, 솔버 불확실성, 탐색 한도는 모두 후보를 UNKNOWN으로 낮춥니다. 보수적 may-alias 메모리 havoc은 따로 추적하므로 평범한 스택 프레임 저장이 본래 정확한 도달 가능성 증거를 무효화하지 않습니다.

---

## 예산, 출력, 바인딩

헌트 탐색과 솔버는 예산으로 제한됩니다(`--max-paths`, `--max-steps`, `--max-loop`, `--solver-conflicts`). 예산 소진은 UNKNOWN입니다. 두 명령은 JSON을 출력하고 `-o`를 존중합니다. 종료 코드는 깨끗한 실행이 `0`, UNSAFE 발견이 있으면 `2`, 오류는 `1`입니다.

같은 분석은 C API(`neverd_session_audit_json` / `neverd_session_hunt_json`, 버전 있는 `neverd_safety_options`)와 Python SDK(`Session.audit()` / `Session.hunt()`)로도 사용할 수 있습니다.

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "17 bytes" } }
}
```

---

## 거짓 양성 경계와 범위

- 용량은 정확하거나 실제 객체 크기의 상한이므로 UNSAFE는 실제 오버플로를 반영합니다. 정확한 선언 크기가 없고 포함 영역 상한만으로 안전을 증명할 수 없으면 UNKNOWN입니다.
- 길이 제한 복사는 풀이 전에 제외되어 `skipped`에 집계됩니다. 정확한 용량은 SAFE를 증명할 수 있지만 상한만 있으면 UNKNOWN을 유지합니다.
- 카탈로그에 등록된 와이드 문자 및 이어붙이기 복사는 요소 너비와 기존 목적지 길이를 복구할 때까지 UNKNOWN입니다. 출력 매개변수 할당자와 조건부 `realloc` 소유권도 핸들 전이를 증명할 수 없으면 UNKNOWN입니다.
- **P0**(이번 릴리스, 세 형식 모두): 싱크 카탈로그, 인수 사전 필터, 복사 오버플로 헌트, 힙 수명 감사. 모든 테스트 호스트에서 PE, ELF, Mach-O × x86-64, AArch64의 체크인된 6개 fixture를 실행합니다.
- **P1**: 스택/전역 오버플로, 미초기화 읽기, 형식 문자열, 더 풍부한 PDB 스택 형, 추가 플랫폼 할당자.
- **P2**: patch로 삽입하는 런타임 검사, 프로시저 간 공격자 도달 가능성.
