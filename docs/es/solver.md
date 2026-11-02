**Idiomas**: [English](../solver.md) | [简体中文](../zh-CN/solver.md) | [繁體中文](../zh-TW/solver.md) | [日本語](../ja/solver.md) | [한국어](../ko/solver.md) | [Français](../fr/solver.md) | [Deutsch](../de/solver.md) | [Español](solver.md) | [Italiano](../it/solver.md) | [Русский](../ru/solver.md) | [العربية](../ar/solver.md)

[← Índice de documentación](README.md)

# Backends de prueba de bitvectors

NeverD usa por defecto su solver de bitvectors integrado. La derivación MBA exacta no depende de un solver general. La síntesis de expresiones solo acepta un candidato tras demostrar equivalencia; un contraejemplo o consulta inconclusa conserva la expresión original.

El circuito de multiplicación integrado elimina los bits bajos conocidos como cero de ambos factores, construye el producto restante y restaura la palabra de salida completa. El adaptador Z3 opcional también codifica los productos con coeficiente constante par con el ancho exacto que observan. Ambos conservan las consultas de igualdad de ancho completo, los modelos de entrada independientes y los límites de recursos existentes. La optimización de código fuente de CLI y GUI comparte el circuito integrado sin una opción adicional.

<!-- i18n-section: builtin-comparisons -->

## Circuitos de comparación integrados

Las comparaciones de más de ocho bits comparan primero las mitades altas y usan las bajas cuando las altas son iguales. Los fragmentos pequeños usan acarreos de resta. El codificador compartido puede reutilizar las puertas del prefijo alto cuando una actualización parcial de registro solo cambia los bits bajos. Las comparaciones con signo siguen invirtiendo ambos bits de signo. La semántica de las expresiones y los límites de recursos no cambian; agotarlos sigue devolviendo `Unknown`.

`BitBlaster.WidePredicatesAgreeWithTheEvaluator` contrasta los predicados con y sin signo con el evaluador en anchos seleccionados de 8 a 256 bits, incluidos anchos impares, valores vecinos de los límites y bits significativos por encima de 64 bits. También excluye salidas incorrectas. Las pruebas de contadores parciales verifican consultas completas, modelos de contraejemplo y agotamiento del presupuesto de puertas. Las regresiones nativas con comparación almacenada prueban ambos órdenes de comparación y actualización observando todos los registros e indicadores, y rechazan un cuerpo original de bucle modificado.

<!-- i18n-section: pristine-encoding -->

## Copias de la codificación antes de buscar

`BitVectorSolver::cloneEncoding()` copia una codificación completa antes de cualquier intento de búsqueda SAT. Devuelve null después de buscar o de un fallo de codificación. Cada copia posee sus cláusulas mutables, propagación raíz, puertas y correspondencias de bits; conserva el orden de variables, el cómputo de puertas y la configuración del solucionador. El contexto debe sobrevivir a ambos solucionadores; el original puede modificarse o destruirse de forma independiente.

El motor SAT mantiene cuatro entradas de vigilancia dentro de cada lista de literal; las listas mayores crecen dinámicamente. Esto evita asignaciones separadas para listas cortas al construir, copiar antes de buscar y destruir. El orden de propagación, las cláusulas, la propiedad independiente y todos los límites de trabajo permanecen iguales.

<!-- i18n-section: context-finite-proofs -->

## Pruebas completas dentro de un contexto

El verificador nativo de independencia vincula su caché de dominios finitos al contexto simbólico real, al que solo se añaden nodos. Las claves compactas conservan el predicado exacto, las proyecciones ordenadas y el límite de valores. Solo se reutilizan dominios numéricos completos o falta de unicidad demostrada; cada fallo de caché exige la enumeración completa y la prueba final de exclusión. El contexto y el significado de los nodos existentes deben mantenerse durante la vida de la caché. Los tokens preparados no pueden pasar a otro propietario ni a un sustituto en la misma dirección. Se conserva el límite de almacenamiento en palabras. Los demás usuarios mantienen las claves estructurales y la reutilización mediante renombrado de variables.

<!-- i18n-section: z3-build -->

## Compilación opcional con Z3

Al activar Z3, CMake descarga mediante `FetchContent` la revisión fijada 4.13.3 y compila una biblioteca estática junto con NeverD. No se requiere instalar Z3 en el sistema:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_Z3=ON -DNEVERD_BUILD_SOLVER_BENCH=ON
python scripts/build.py build-release --parallel 4 --target neverd \
  NeverDSolverTests NeverDSymbolicTests neverd-solver-bench
```

`NEVERD_Z3_PROVIDER=FETCH` es el valor predeterminado. La primera configuración descarga el código; las siguientes reutilizan `_deps`. No se compilan la CLI, pruebas, ejemplos, documentación ni bindings de Z3. Sus generadores Python usan el intérprete existente de NeverD. Para una biblioteca instalada, selecciona `-DNEVERD_Z3_PROVIDER=SYSTEM` y opcionalmente `-DZ3_ROOT=/path/to/prefix`; si faltan archivos de desarrollo falla y no cambia de proveedor silenciosamente. En modo offline puedes proporcionar un checkout local con `-DFETCHCONTENT_SOURCE_DIR_NEVERD_Z3=/path/to/z3`.

Con `NEVERD_ENABLE_Z3=OFF` (predeterminado), NeverD no descarga, busca ni enlaza Z3. Una solicitud explícita de un backend no disponible falla, sin fallback.

<!-- i18n-section: synthesis -->

## Síntesis de expresiones condicionada a prueba

```sh
build-release/bin/neverd simplify --synthesize --solver=z3 \
  --solver-timeout-ms=1000 --json '(x >> 4) + ((x >> 2) >> 2)'
```

`--solver=builtin` es el predeterminado. Elegir solver requiere `--synthesize`; el simplificador MBA normal no invoca Z3. El timeout de Z3 se aplica a cada comprobación, sin contar la traducción de la expresión. La cancelación es cooperativa, no un plazo estricto de reloj. Cero selecciona 1000 ms; `--exhaustive` elimina el límite. Los límites de conflictos SAT, propagación y visitas a watch solo se aplican al backend integrado y se rechazan con Z3. Las consultas Z3 incrementan el contador de pruebas; los contadores de trabajo SAT integrado permanecen en cero.

La API C añade `solver_backend` y `solver_timeout_ms` a `neverd_synthesize_options`. Los lectores limitados por tamaño mantienen el backend integrado para clientes antiguos. `neverd_solver_backend_available()` informa de la capacidad de compilación; Python ofrece la misma opción en `synthesize_expression(..., solver='z3', solver_timeout_ms=1000)`. Actualmente se aplica solo a síntesis de expresiones. La ejecución concolic, el análisis de seguridad y los valores predeterminados de optimización IR conservan sus políticas. Los usuarios internos pueden pasar el verificador Z3 mediante el callback de prueba del simplificador semántico.

```python
from neverd_plugin import synthesize_expression

result = synthesize_expression(
    '(x >> 4) + ((x >> 2) >> 2)', solver='z3', solver_timeout_ms=1000
)
```

<!-- i18n-section: checks -->

## Comprobaciones independientes y exportación de consultas

Con Z3 habilitado, `NeverDSolverTests` incluye evaluación independiente de expresiones y comparación entre backends. Las expresiones de referencia se construyen directamente para impedir que un error en los constructores de NeverD simplifique ambos lados del test. Las compilaciones deshabilitadas omiten expresamente esos oráculos y siguen comprobando el contrato del backend no disponible.

Las pruebas de productos escalados comparan el circuito integrado con una multiplicación larga independiente y aritmética entera hasta 257 bits. Las pruebas Z3 demuestran su equivalencia con una codificación independiente de ancho completo y comprueban los bits altos de entrada y un bit más significativo de salida deliberadamente incorrecto.

```sh
build-release/bin/NeverDSolverTests --gtest_brief=1
build-release/bin/neverd-solver-bench \
  tools/neverd-bench/solver-corpus.txt --width=32 --repeat=5 \
  --backend=both --timeout-ms=1000 --max-conflicts=10000 \
  --dump-dir=/tmp/neverd-queries > /tmp/neverd-solver-results.json
```

Cada línea no comentada es `original ; candidate`; si falta el punto y coma, se usa como candidato el resultado del simplificador MBA. La herramienta informa veredictos, reproducción de modelos y tiempos (incluyen creación de sesión, traducción y resolución; excluyen parsing, simplificación MBA, exportación y cierre). Cada repetición crea un solver nuevo. Los modelos SAT deben reproducir la diferencia en el evaluador de expresiones. Veredictos concluyentes opuestos o consultas/modelos inválidos hacen fallar la ejecución; `unknown` se registra, pero no prueba equivalencia.

SMT-LIB exportado incluye el DAG codificado equivalente, aserciones permanentes y supuestos de la última consulta; se puede reproducir con `z3 query-N.smt2`. Registra límites y versiones: los backends usan unidades de presupuesto distintas, así que se comparan cargas acotadas, no trabajo idéntico. Las pruebas de bitvector usan semántica total de ancho fijo. Excepciones de máquina, efectos de memoria y LLVM poison siguen perteneciendo a los límites de lifting y traducción; una prueba de expresión no los certifica.

Una copia de la codificación aún no explorada elimina de su propia cola de decisiones las variables asignadas en la raíz y reconstruye el orden estricto del montículo por actividad e índice. Las asignaciones y cláusulas permanecen intactas; los hechos raíz sobreviven a todo retroceso. La propiedad de la fuente, las decisiones reales, los modelos completos y todos los presupuestos de búsqueda no cambian.

El DAG exportado a SMT-LIB es una codificación semánticamente equivalente y puede incluir el cociente exacto descrito arriba; su estructura puede diferir del DAG original.
