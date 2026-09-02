**언어**: [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← 문서 색인](README.ko.md)

# Python 플러그인

NeverD는 Python 파일을 일급 플러그인으로 로드할 수 있습니다. Python 플러그인은 네이티브 플러그인과 동일한 메타데이터, 수명 주기, 순서, 이름 중복 규칙, 이벤트 스트림 및 세션 C ABI를 공유합니다. 지원되는 작성 패키지는 `neverd-plugin`입니다. 비공개 `_neverd_plugin` 브리지를 직접 import하지 마십시오.

## 빌드 및 런타임 요구 사항

`NEVERD_ENABLE_PYTHON_PLUGINS`의 기본값은 `ON`입니다. 활성화된 빌드에는 CMake가 찾을 수 있는 CPython 3.10 이상 인터프리터와 임베딩 개발 라이브러리가 필요합니다.

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

CPython 링크 의존성이 없는 네이티브 전용 `libneverd`를 만들려면 `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`를 지정하십시오. Python을 활성화한 빌드는 일치하는 패키지와 예제를 `build/bin/sdk/python/` 아래에 배치합니다. 이 디렉터리는 `python3 -m pip install build/bin/sdk/python`으로 바로 설치할 수도 있습니다.

## 플러그인 작성

하나의 모듈은 데코레이터가 적용된 클래스를 정확히 하나 선언합니다.

```python
from neverd_plugin import Event, Plugin, PluginType, Session


@Plugin(
    name="Analysis Report",
    version="1.0.0",
    author="Your team",
    description="Reports basic information about the loaded binary",
    type=PluginType.PROCESSOR,
)
class AnalysisReport:
    def on_init(self, session: Session) -> int | None:
        print(session.architecture)
        return None

    def on_run(self, session: Session, arg: int) -> int | None:
        print(session.file_path, session.function_count)
        return 0

    def on_event(self, event: Event) -> int | None:
        print(event.type.name)
        return None

    def on_term(self) -> None:
        pass
```

모든 hook은 선택 사항입니다. `None`은 성공을 의미하며 정수 결과는 C `int` 범위에 들어가야 합니다. 메타데이터 버전은 엄격한 SemVer를 사용합니다. 이름은 비어 있지 않은 UTF-8이어야 하며 내장 NUL이 포함된 모든 메타데이터는 거부됩니다.

저장소 예제는 [`minimal.py`](../pluginsdk/python/examples/minimal.py), [`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py), 증명 게이트 최적화 API를 보여 주는 [`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py)입니다.

## 플러그인 로드 및 검사

C API는 특정 `.py` 파일을 결정적으로 로드하거나 디렉터리를 검색할 수 있습니다.

```c
if (!neverd_plugins_load_file(session, "plugins/report.py")) {
  const char *message = neverd_last_error(session);
  /* log message */
  neverd_free_string(message);
}

neverd_plugins_init(session);
int result = neverd_plugins_run(session, "Analysis Report", 0);
neverd_plugins_term(session);
```

`neverd_plugins_list_json`은 각 항목을 `"kind":"python"` 또는 `"kind":"native"`로 식별합니다. 디렉터리 탐색은 정규 경로순으로 정렬되며 같은 디렉터리에서 네이티브 라이브러리와 Python 파일을 함께 처리합니다. 중복 정규 경로와 중복 플러그인 이름은 오류입니다.

## 세션 및 이벤트 API

`Session`은 C를 호출할 때마다 호스트 기능을 다시 검증합니다. 형식화된 표면에는 파일·아키텍처·형식 메타데이터, 비트 수와 테이블 개수, 함수 뷰, 로드와 분석, 바이트 읽기, 디스어셈블, 디컴파일 및 일반 쿼리가 포함됩니다. 고급 작업에서는 `session.raw`를 통해 `neverd_plugin.abi`의 모든 선언에 접근할 수 있습니다.

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

### 제한된 기호 경로 탐색

네이티브 LowIR 함수에서 `session.symbolic_explore`는 형식화된 경로 결과, 기본 블록 추적, 리소스 사용량 및 선택적 경로 조건식을 반환합니다.

```python
result = session.symbolic_explore(
    0x401000,
    max_paths=64,
    max_steps=1 << 16,
    max_block_visits=3,
    include_expressions=True,
)
if not result.exact:
    print(result.unmodelled_ops)
for path in result.paths:
    print(path.outcome, path.blocks, path.predicate)
```

경로, 단계, 루프 방문 또는 확인되지 않은 분기 제한 때문에 탐색이 중단되면 `complete`는 false입니다. `exact`가 true이려면 어떤 연산도 보수적으로 미지 상태로 대체되지 않아야 합니다. 지원되지 않는 LowIR 연산, 요약이 없는 호출 및 확인되지 않은 주소를 통한 저장은 `unmodelled_ops`에 포함됩니다. EVM 및 SBF 세션에서는 네이티브 LowIR 탐색을 사용할 수 없습니다.

### 검증된 LowIR 콘콜릭 분기 반전

`session.lowir_concolic`는 명시적인 입력 레지스터 바이트 범위에서 하나의 네이티브 LowIR 경로를 추적하고, 새 재실행이 동일한 제어 결정 발생 지점에서 검증한 솔버 생성 후보만 반환합니다.

```python
from neverd_plugin import ConcolicRegisterSeed

report = session.lowir_concolic(
    0x401000,
    [ConcolicRegisterSeed(offset=56, bytes=4, value=0)],
)
for flip in report.flips:
    if flip.candidate_id is not None:
        print(report.candidates[flip.candidate_id].seed)
```

레지스터 오프셋은 NeverD 레지스터 파일의 바이트 오프셋이며 네이티브 포인터나 레지스터 번호가 아닙니다. 보고서는 항상 비완전하며 UNSAT, 솔버 제한, 투영 거부 및 재실행 거부는 예외가 아닌 형식화된 반전 결과로 남습니다.

### 메모리 안전성 감사와 헌트

`session.audit()`와 `session.hunt()`는 파싱된 JSON 보고서를 반환합니다(CLI와 같은 스키마). 리프트된 네이티브 세션이 필요합니다.

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

EVM 및 SBF 세션은 이러한 호출을 거부합니다.

변경 불가능한 여섯 이벤트는 `BINARY_LOADED`, `BINARY_CLOSING`, `FUNCTION_SELECTED`, `ADDRESS_CHANGED`, `ANALYSIS_DONE`, `PATCH_APPLIED`입니다. 콜백 중 payload 문자열이 복사되며 해당 이벤트 종류와 관계없는 필드는 `None`입니다.

종료 후 사용하기 위해 `Session`을 저장하지 마십시오. 네이티브 capsule은 `on_term`이 시작되기 전과 네이티브 세션을 해제할 수 있기 전에 무효화됩니다. 이후 호출은 오래된 메모리를 역참조하지 않고 `RuntimeError`로 실패합니다.

### 엄격한 바이너리 sanitizer 게시

`session.sanitize()`는 모든 지점을 보호하지 못하면 거부하는 실험적 `binary-sanitizer-v1` 트랜잭션을 실행하고, 완전하고 일관된 인증 receipt를 검증한 경우에만 불변 `SanitizeResult`를 반환합니다. Darwin이 아닌 호스트는 lifting, guard 생성, 후보 생성 또는 namespace 변경 전에 거부합니다. `PUBLISH_INDETERMINATE`와 `PUBLISHED_INCOMPLETE`는 실패이며 대상이 이미 존재할 수 있으므로 사용하거나 다시 시도하기 전에 검사해야 합니다.

완전한 Darwin receipt는 트랜잭션 동안 보유한 대상 디렉터리 object만 인증합니다. 이 object는 open 후 이름이 바뀔 수 있으므로 원래 pathname이 처리 중 또는 반환 후에도 같은 object를 가리킨다는 사실을 보장하지 않으며, 영구적이고 독립적으로 재검증 가능한 path binding도 아닙니다. 나중에 path를 다시 여는 코드는 외부 anchor를 유지하고 object를 다시 인증해야 합니다. Python에는 아직 네이티브 whole-process replay 메서드가 없습니다. `NativeProcessReplayAdapter`는 fail-closed Phase 0 C++ 가용성/factory 경계일 뿐이며 모든 호스트가 모든 능력을 false로 보고하고 작업 table을 반환하지 않습니다.

### 증명 게이트 합성과 LLVM 최적화

`synthesize_expression`은 ABI 호환성을 위해 유지되는 MBA 전용
`simplify_expression`과 분리되어 있습니다. 솔버가
`ProofStatus.EQUIVALENT`를 반환한 경우에만 재작성을 커밋합니다. 반례,
불완전한 증명, 검색 예산 소진 시에는 원래 식을 유지하면서 각각의 결과와
검색/증명 작업량을 보고합니다.
`ProofStatus.INVALID`는 증명 질의 자체가 잘못되었음을 나타내며 예산으로 인한
`ProofStatus.UNKNOWN`과 구분됩니다. 두 경우 모두 재작성을 안전하게 거부합니다.

`optimize_llvm_ir`은 트랜잭션 복제본에서 NeverD 의미 고정점과 표준 LLVM
파이프라인을 결합하고, 검증된 커밋 모듈만 반환합니다.

```python
from neverd_plugin import (
    LLVMOptimizationLevel,
    OptimizationMode,
    ProofStatus,
    optimize_llvm_ir,
    synthesize_expression,
)

rewrite = synthesize_expression(
    "(x >> 4) + ((x >> 2) >> 2)", exhaustive=True
)
if rewrite.changed:
    assert rewrite.proof_status is ProofStatus.EQUIVALENT

module = optimize_llvm_ir(
    llvm_ir,
    mode=OptimizationMode.DEEP,
    llvm_level=LLVMOptimizationLevel.O2,
    enable_synthesis=True,
    exhaustive=True,
)
print(module.output_ir, module.semantic_rewrites, module.proof_queries)
```

운영 호출자는 MBA 작업량과 항 수, 합성 검색과 SAT 작업량, LLVM 수렴을 각각
제한할 수 있습니다. `simplify_expression`에서 명시적인 `exhaustive=True`는
항 수와 작업량이 무제한인 MBA 정책을 선택하고 네이티브 파서의 중첩 및 비트
너비 정책 상한을 제거합니다. `synthesize_expression`에서는 호출자가 지정한
문법을 유지하면서 파서, 검색 작업량, SAT 상한을 제거하고,
`optimize_llvm_ir`에서는 수렴, 검색 작업량, SAT 상한을 제거합니다. Python
계층은 식에 별도 제한을 추가하지 않지만 메모리 안전 및 IR 표현 경계는 계속
적용됩니다. 대응하는 C 진입점은 `neverd_simplify_expr`,
`neverd_synthesize_expr`, `neverd_optimize_llvm_ir`이며 형식화된 해제 함수와
버전 JSON 어댑터도 제공합니다.

## 오류, 격리 및 신뢰

Python 예외는 절대로 C++을 가로질러 unwind되지 않습니다. NeverD는 완전히 포맷된 traceback을 캡처해 `neverd_last_error`로 제공합니다. 각 정규 플러그인 경로는 고유한 모듈 이름으로 로드됩니다. 종료 시 모듈을 제거하므로 나중에 다시 로드하면 새로운 모듈과 클래스 상태를 얻습니다. CPython은 한 번만 초기화되고 bootstrap GIL은 해제됩니다. 모든 호스트 스레드의 콜백은 GIL을 획득하며 NeverD는 다른 구성 요소와 공유할 수 있는 인터프리터를 종료하지 않습니다.

플러그인은 NeverD 프로세스 안에서 임의의 Python을 실행하고 전체 C API를 호출할 수 있습니다. 신뢰할 수 있는 파일만 로드하십시오. 이것은 확장 경계이지 sandbox가 아닙니다.

## 개발, 테스트 및 패키지

편집기와 타입 검사기 지원을 받으려면 순수 Python 패키지를 설치하거나 소스 트리를 `PYTHONPATH`에 지정하십시오.

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

감사는 내보낸 모든 C 선언과 `ctypes` 서명·소유권 규칙이 정확히 일치하도록 요구합니다. 또한 출력 언어 값, CMake 및 패키지 버전, CI 기능 플래그, Action 고정 버전, artifact 흐름과 PyPI OIDC 정책을 검사합니다. 네이티브 어댑터 테스트는 `NeverDPluginRuntimeTests`이며 임베디드 Python 테스트는 `NeverDPythonRuntimeTests`와 `NeverDPythonPluginTests`입니다.

`Python Plugin SDK` workflow는 wheel과 소스 배포본을 하나씩 빌드하고 둘 다 깨끗한 환경에 설치한 뒤 검증된 artifact를 업로드합니다. 게시된 GitHub Release에 대해서만 승인으로 보호되는 `pypi` environment와 Trusted Publishing을 통해 배포하며 장기 PyPI token은 사용하지 않습니다.
