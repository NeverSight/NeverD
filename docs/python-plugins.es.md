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

Los ejemplos del repositorio son [`minimal.py`](../pluginsdk/python/examples/minimal.py), [`analysis_report.py`](../pluginsdk/python/examples/analysis_report.py) y [`semantic_optimizer.py`](../pluginsdk/python/examples/semantic_optimizer.py), que muestra las API de optimización condicionadas por pruebas.

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

### Exploración simbólica acotada de rutas

Para las funciones LowIR nativas, `session.symbolic_explore` devuelve resultados de ruta tipados, trazas de bloques básicos, uso de recursos y predicados de ruta opcionales:

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

`complete` es false cuando un límite de rutas, pasos, visitas de bucle o ramas no resueltas detiene el recorrido. `exact` requiere además que ninguna operación se haya sustituido de forma conservadora por un estado desconocido; las operaciones LowIR no admitidas, las llamadas sin resumen y los almacenamientos mediante direcciones no resueltas se cuentan en `unmodelled_ops`. Las sesiones EVM y SBF no exponen la exploración LowIR nativa.

### Inversiones concolic verificadas de ramas LowIR

`session.lowir_concolic` sigue una ruta LowIR nativa desde rangos de bytes explícitos del registro de entrada y solo devuelve candidatos del solver que una nueva repetición verifica en la misma ocurrencia de decisión de control:

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

El desplazamiento del registro es un desplazamiento de bytes en el archivo de registros de NeverD, no un puntero nativo ni un número de registro. El informe nunca es exhaustivo; UNSAT, los límites del solver y los rechazos de proyección o repetición siguen siendo resultados de inversión tipados, no excepciones.

### Auditoría y caza de seguridad de memoria

`session.audit()` y `session.hunt()` devuelven informes JSON analizados (el mismo esquema que el CLI). Requieren una sesión nativa levantada:

```python
audit = session.audit()
hunt = session.hunt(max_paths=64, max_steps=1 << 16)
print(audit.get("ok"), hunt.get("findings"))
```

Las sesiones EVM y SBF rechazan estas llamadas.

Las seis variantes de evento inmutables son `BINARY_LOADED`, `BINARY_CLOSING`, `FUNCTION_SELECTED`, `ADDRESS_CHANGED`, `ANALYSIS_DONE` y `PATCH_APPLIED`. Las cadenas del payload se copian durante el callback; los campos no relacionados con la variante valen `None`.

Nunca conserve una `Session` para usarla después de terminar. La cápsula nativa se invalida antes de que comience `on_term` y antes de que pueda liberarse la sesión nativa. Una llamada posterior falla con `RuntimeError` en vez de desreferenciar memoria obsoleta.

### Síntesis gobernada por pruebas y optimización LLVM

`synthesize_expression` está separado de `simplify_expression`, conservado por
compatibilidad ABI y limitado a MBA. Una reescritura solo se confirma cuando el
solucionador devuelve `ProofStatus.EQUIVALENT`. Los contraejemplos, las pruebas
incompletas y el agotamiento del presupuesto mantienen la expresión original y
comunican por separado el resultado y el trabajo de búsqueda y prueba.
`ProofStatus.INVALID` identifica una consulta de prueba mal formada y se
mantiene distinto de `ProofStatus.UNKNOWN`, causado por el presupuesto; ambos
rechazan la reescritura de forma segura.

`optimize_llvm_ir` combina el punto fijo semántico de NeverD y el pipeline LLVM
estándar elegido sobre una copia transaccional, y solo devuelve el módulo
verificado y confirmado:

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

Los clientes de producción pueden limitar por separado el trabajo y la aridad
MBA, la búsqueda y el trabajo SAT de síntesis, y la convergencia de LLVM. En
`simplify_expression`, `exhaustive=True` selecciona la política MBA sin límite
de aridad ni trabajo y elimina los límites de política de anidamiento y ancho
del analizador nativo. En `synthesize_expression`, elimina los límites del
analizador, del trabajo de búsqueda y de SAT, pero conserva la gramática indicada
por quien llama; en `optimize_llvm_ir`, elimina los límites de convergencia,
búsqueda y SAT. Python no añade otro límite de expresión; siguen vigentes los
límites de seguridad de memoria y representación del IR. Las entradas C son
`neverd_simplify_expr`, `neverd_synthesize_expr` y `neverd_optimize_llvm_ir`,
con liberadores tipados y adaptadores JSON versionados.

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
