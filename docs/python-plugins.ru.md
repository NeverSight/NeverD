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

Примеры в репозитории: [`minimal.py`](../pluginsdk/python/examples/minimal.py) и [`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py).

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

Шесть неизменяемых вариантов событий: `BINARY_LOADED`, `BINARY_CLOSING`, `FUNCTION_SELECTED`, `ADDRESS_CHANGED`, `ANALYSIS_DONE` и `PATCH_APPLIED`. Строки payload копируются во время callback; поля, не относящиеся к варианту события, равны `None`.

Никогда не сохраняйте `Session` для использования после завершения. Нативная capsule становится недействительной до начала `on_term` и до того, как нативную сессию можно освободить. Последующий вызов завершается с `RuntimeError`, а не разыменовывает устаревшую память.

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
