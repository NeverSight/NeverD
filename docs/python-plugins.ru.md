**Языки**: [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← Указатель документации](README.ru.md)

# Плагины Python

NeverD может загружать файл Python как полноценный плагин. Плагины Python используют те же метаданные, жизненный цикл, порядок, правила повторяющихся имён, поток событий и C ABI сессии, что и нативные плагины. Поддерживаемый пакет для разработки — `neverd-plugin`; не импортируйте закрытый мост `_neverd_plugin` напрямую.

## Требования к сборке и среде выполнения

По умолчанию `NEVERD_ENABLE_PYTHON_PLUGINS` имеет значение `ON`. Для включённой сборки CMake должен обнаружить интерпретатор CPython 3.10 или новее и библиотеку разработки для встраивания:

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

Укажите `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`, чтобы получить только нативную `libneverd` без зависимости от CPython при линковке. Сборка с Python размещает соответствующий пакет и примеры в `build/bin/sdk/python/`; этот каталог также можно установить напрямую командой `python3 -m pip install build/bin/sdk/python`.

## Написание плагина

Один модуль объявляет ровно один декорированный класс:

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

Все hooks необязательны. `None` означает успех; целочисленный результат должен помещаться в C `int`. Версии метаданных используют строгий SemVer. Имя должно быть непустой строкой UTF-8, а любые метаданные со встроенным NUL отклоняются.

Примеры в репозитории: [`minimal.py`](../pluginsdk/python/examples/minimal.py), [`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py) и [`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py) для API оптимизации с обязательным доказательством.

## Загрузка и просмотр плагинов

C API позволяет детерминированно загрузить конкретный файл `.py` или просканировать каталог:

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

`neverd_plugins_list_json` помечает каждый элемент как `"kind":"python"` или `"kind":"native"`. Результаты поиска в каталоге сортируются по каноническому пути; в одном каталоге могут находиться и нативные библиотеки, и файлы Python. Повторяющиеся канонические пути и имена плагинов считаются ошибками.

## API сессий и событий

`Session` повторно проверяет возможности хоста перед каждым вызовом C. Типизированный интерфейс охватывает метаданные файла, архитектуры и формата, разрядность и счётчики таблиц, представления функций, загрузку и анализ, чтение байтов, дизассемблирование, декомпиляцию и распространённые запросы. Для расширенных операций `session.raw` предоставляет все объявления из `neverd_plugin.abi`:

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

### Ограниченное символьное исследование путей

Для нативных функций LowIR `session.symbolic_explore` возвращает типизированные результаты путей, трассы базовых блоков, сведения об использовании ресурсов и необязательные предикаты путей:

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

`complete` имеет значение false, если обход остановлен ограничением числа путей, шагов, посещений цикла или неразрешённых ветвлений. Кроме того, `exact` требует, чтобы ни одна операция не была консервативно заменена неизвестным состоянием; неподдерживаемые операции LowIR, вызовы без сводки и записи по неразрешённым адресам учитываются в `unmodelled_ops`. Сессии EVM и SBF не предоставляют исследование нативного LowIR.

### Проверенные конколические инверсии ветвей LowIR

`session.lowir_concolic` проходит один нативный маршрут LowIR от явно заданных байтовых диапазонов входных регистров и возвращает только созданные решателем кандидаты, которые новый повторный запуск подтвердил в том же вхождении управляющего решения:

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

Смещение регистра — это байтовое смещение в файле регистров NeverD, а не нативный указатель или номер регистра. Отчёт всегда неисчерпывающий; UNSAT, пределы решателя и отказы проекции или повтора остаются типизированными результатами инверсии, а не исключениями.

### Аудит и охота на ошибки безопасности памяти

`session.audit()` и `session.hunt()` возвращают разобранные JSON-отчёты (та же схема, что у CLI). Нужна поднятая нативная сессия:

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

Сессии EVM и SBF отклоняют эти вызовы.

Шесть неизменяемых вариантов событий: `BINARY_LOADED`, `BINARY_CLOSING`, `FUNCTION_SELECTED`, `ADDRESS_CHANGED`, `ANALYSIS_DONE` и `PATCH_APPLIED`. Строки payload копируются во время callback; поля, не относящиеся к варианту события, равны `None`.

Никогда не сохраняйте `Session` для использования после завершения. Нативная capsule становится недействительной до начала `on_term` и до того, как нативную сессию можно освободить. Последующий вызов завершается с `RuntimeError`, а не разыменовывает устаревшую память.

### Синтез под контролем доказательства и оптимизация LLVM

`synthesize_expression` отделён от сохранённого ради совместимости ABI
`simplify_expression`, который работает только с MBA. Переписывание принимается
лишь при ответе решателя `ProofStatus.EQUIVALENT`. Контрпример, незавершённое
доказательство или исчерпанный бюджет сохраняют исходное выражение и отдельно
сообщают исход, объём поиска и объём доказательства.
`ProofStatus.INVALID` означает некорректную постановку задачи доказательства и
не смешивается с вызванным бюджетом `ProofStatus.UNKNOWN`; оба результата
безопасно запрещают переписывание.

`optimize_llvm_ir` объединяет семантическую неподвижную точку NeverD и выбранный
стандартный конвейер LLVM на транзакционной копии и возвращает только
проверенный, принятый модуль:

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

В рабочей среде можно раздельно ограничивать работу и арность MBA, поиск и
SAT-работу синтеза, а также сходимость LLVM. Для `simplify_expression` явный
режим `exhaustive=True` выбирает политику MBA без ограничений арности и работы
и снимает ограничения политики вложенности и разрядности нативного парсера.
Для `synthesize_expression` он снимает ограничения парсера, поисковой работы и
SAT, сохраняя заданную вызывающей стороной грамматику; для `optimize_llvm_ir` —
ограничения сходимости, поиска и SAT. Слой Python не добавляет иных ограничений
выражения; границы безопасности памяти и представления IR продолжают
действовать. Соответствующие точки входа C — `neverd_simplify_expr`,
`neverd_synthesize_expr` и `neverd_optimize_llvm_ir`; предусмотрены
типизированные функции освобождения и версионированные JSON-адаптеры.

## Ошибки, изоляция и доверие

Исключения Python никогда не раскручивают стек через C++. NeverD перехватывает полный форматированный traceback и предоставляет его через `neverd_last_error`. Каждый канонический путь плагина загружается под уникальным именем модуля; при завершении модуль удаляется, поэтому последующая загрузка получает чистое состояние модуля и класса. CPython инициализируется один раз, начальная GIL освобождается, а callbacks захватывают GIL в любом потоке хоста. NeverD не завершает интерпретатор, который может совместно использоваться другим компонентом.

Плагины выполняют произвольный Python внутри процесса NeverD и могут вызывать весь C API. Загружайте только доверенные файлы. Это граница расширения, а не sandbox.

## Разработка, тестирование и пакеты

Для поддержки редактора и проверки типов установите чистый пакет Python либо добавьте дерево исходников в `PYTHONPATH`:

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

Аудит требует точного соответствия каждого экспортированного объявления C его сигнатуре `ctypes` и правилу владения. Он также проверяет значения языков вывода, версии CMake и пакета, флаги функций CI, закреплённые версии Actions, поток артефактов и политику PyPI OIDC. Тесты нативного адаптера — `NeverDPluginRuntimeTests`; тесты встроенного Python — `NeverDPythonRuntimeTests` и `NeverDPythonPluginTests`.

Workflow `Python Plugin SDK` собирает один wheel и один исходный дистрибутив, устанавливает оба в чистые окружения и загружает проверенные артефакты. Публикация выполняется только для опубликованного GitHub Release через защищённое подтверждением environment `pypi` и Trusted Publishing; долгоживущий токен PyPI не используется.
