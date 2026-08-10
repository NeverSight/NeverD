**Idiomas**: [English](python-plugins.md) | [简体中文](python-plugins.zh-CN.md) | [繁體中文](python-plugins.zh-TW.md) | [日本語](python-plugins.ja.md) | [한국어](python-plugins.ko.md) | [Français](python-plugins.fr.md) | [Deutsch](python-plugins.de.md) | [Español](python-plugins.es.md) | [Italiano](python-plugins.it.md) | [Русский](python-plugins.ru.md) | [العربية](python-plugins.ar.md)

[← Índice de documentación](README.es.md)

# Plugins de Python

NeverD puede cargar un archivo Python como plugin de primera clase. Los plugins de Python comparten con los nativos los mismos metadatos, ciclo de vida, orden, reglas de nombres duplicados, flujo de eventos y ABI C de sesión. El paquete admitido para desarrollarlos es `neverd-plugin`; no importe directamente el puente privado `_neverd_plugin`.

## Requisitos de compilación y ejecución

`NEVERD_ENABLE_PYTHON_PLUGINS` vale `ON` de forma predeterminada. Una compilación habilitada necesita que CMake encuentre un intérprete CPython 3.10 o posterior y su biblioteca de desarrollo para integración:

```bash
cmake -S . -B build -G Ninja \
  -DNEVERD_ENABLE_PYTHON_PLUGINS=ON \
  -DPython3_EXECUTABLE="$(python3 -c 'import sys; print(sys.executable)')"
cmake --build build
```

Use `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` para obtener un `libneverd` solo nativo, sin dependencia de enlace con CPython. Una compilación con Python coloca el paquete correspondiente y los ejemplos en `build/bin/sdk/python/`; ese directorio también se instala directamente con `python3 -m pip install build/bin/sdk/python`.

## Escribir un plugin

Cada módulo declara exactamente una clase decorada:

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

Todos los hooks son opcionales. `None` significa éxito; un resultado entero debe caber en un `int` de C. Las versiones de metadatos usan SemVer estricto. Los nombres deben ser cadenas UTF-8 no vacías y se rechaza cualquier metadato que contenga un NUL interno.

Los ejemplos del repositorio son [`minimal.py`](../pluginsdk/python/examples/minimal.py) y [`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py).

## Cargar e inspeccionar plugins

La API de C puede cargar de manera determinista un archivo `.py` concreto o explorar un directorio:

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

`neverd_plugins_list_json` identifica cada elemento con `"kind":"python"` o `"kind":"native"`. La detección de directorios se ordena por ruta canónica y admite bibliotecas nativas y archivos Python en el mismo directorio. Las rutas canónicas y los nombres de plugin duplicados son errores.

## API de sesión y eventos

`Session` vuelve a validar la capacidad del host antes de cada llamada a C. Su interfaz tipada incluye metadatos de archivo, arquitectura y formato, ancho de bits y recuentos de tablas, vistas de funciones, carga y análisis, lectura de bytes, desensamblado, descompilación y consultas comunes. Para operaciones avanzadas, `session.raw` expone todas las declaraciones de `neverd_plugin.abi`:

```python
count = session.raw.session_call("neverd_plugins_count")
version = session.raw.owned_string("neverd_version")
object_bytes = session.raw.session_borrowed_bytes("neverd_roundtrip_obj")
```

Las seis variantes de evento inmutables son `BINARY_LOADED`, `BINARY_CLOSING`, `FUNCTION_SELECTED`, `ADDRESS_CHANGED`, `ANALYSIS_DONE` y `PATCH_APPLIED`. Las cadenas del payload se copian durante el callback; los campos no relacionados con la variante valen `None`.

Nunca conserve una `Session` para usarla después de terminar. La cápsula nativa se invalida antes de que comience `on_term` y antes de que pueda liberarse la sesión nativa. Una llamada posterior falla con `RuntimeError` en vez de desreferenciar memoria obsoleta.

## Errores, aislamiento y confianza

Las excepciones de Python nunca atraviesan C++ durante el desenrollado de pila. NeverD captura el traceback completo con formato y lo expone mediante `neverd_last_error`. Cada ruta canónica de plugin se carga con un nombre de módulo único; al terminar se elimina el módulo y una carga posterior obtiene estados nuevos de módulo y clase. CPython se inicializa una sola vez, se libera el GIL de arranque y los callbacks adquieren el GIL en cualquier hilo del host. NeverD no finaliza un intérprete que pueda compartir con otro componente.

Los plugins ejecutan Python arbitrario dentro del proceso de NeverD y pueden llamar a toda la API de C. Cargue únicamente archivos de confianza. Esta es una frontera de extensión, no una sandbox.

## Desarrollo, pruebas y paquetes

Para disponer de asistencia del editor y del comprobador de tipos, instale el paquete Python puro o incluya el árbol de fuentes en `PYTHONPATH`:

```bash
python3 -m pip install -e pluginsdk/python

PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
```

La auditoría exige paridad exacta entre cada declaración C exportada y su firma `ctypes` y regla de propiedad. También comprueba los valores de lenguaje de salida, las versiones de CMake y del paquete, los indicadores de función de CI, las versiones fijadas de Actions, el flujo de artefactos y la política OIDC de PyPI. Las pruebas del adaptador nativo son `NeverDPluginRuntimeTests`; las de Python integrado son `NeverDPythonRuntimeTests` y `NeverDPythonPluginTests`.

El workflow `Python Plugin SDK` construye un wheel y una distribución de código fuente, instala ambos en entornos limpios y sube los artefactos verificados. La publicación solo se ejecuta para una GitHub Release publicada mediante el environment `pypi` protegido por aprobación y Trusted Publishing; no se usa ningún token PyPI de larga duración.
