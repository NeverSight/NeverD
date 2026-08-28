**Idiomas**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← Índice de documentación](README.es.md)

# Auditoría y caza de seguridad de memoria

NeverD analiza un binario cargado en busca de dos familias de defectos de seguridad de memoria y los informa como JSON estructurado. Ambas pistas se ejecutan sobre el IR levantado, independiente del formato, de modo que **PE/COFF, ELF y Mach-O son objetivos de primer nivel, en igualdad** — un hallazgo nunca queda detrás del escáner o la tabla de importaciones de un solo formato.

| Pista | Comando | Informa |
|-------|---------|---------|
| **Audit** | `neverd audit <binary>` | Defectos de vida del montón y lecturas locales de pila sin inicializar |
| **Hunt** | `neverd hunt <binary>` | Desbordamientos de copias peligrosas con evidencia simbólica y valores candidatos; `replayable=true` solo con un plan `process-input-v1` completo |

El motor reutiliza la ejecución simbólica y el solver de vectores de bits internos de NeverD para testigos y alcanzabilidad. No hay solver externo, VM ni contenedor.

---

## Invariante central: fallar cerrado

Una operación no levantada, una llamada cuyos argumentos el paso ABI no pudo recuperar, un destino indirecto sin resolver o un presupuesto agotado producen **UNKNOWN**, nunca SAFE. Un destino cuya capacidad no se puede recuperar es UNKNOWN. El lifting estricto no cambia; la capa de seguridad solo añade veredictos conservadores encima.

Los efectos de llamada usan semántica de mundo cerrado: un resumen solo se aplica cuando se conocen sus precondiciones y todos los efectos pertinentes. Un efecto desconocido o un resumen aplicable solo en parte conserva UNKNOWN; nunca se rellena el hueco suponiendo que no hay efecto o que la llamada tuvo éxito.

---

## Contrato de identidad por formato

Ambas pistas requieren el pipeline de lift (recupera argumentos por llamada). Cada callee se nombra con la misma vista de identidad que el resto de NeverD. El orden de descubrimiento de depuración no cambia.

| Formato | Depuración (por precedencia) | Resolución de importaciones / thunks |
|---------|------------------------------|--------------------------------------|
| **PE/COFF** | `--pdb`, directorio de depuración o `.pdb` hermano, luego `/MAP` de MSVC | Ranuras IAT y thunks `__imp_`, importaciones ordinales |
| **ELF** | DWARF en la imagen, `*.debug` separado, luego MAP GNU/LLD | Stubs PLT resueltos al nombre importado |
| **Mach-O** | DWARF en la imagen, `.dSYM` adyacente, luego `-map` de ld64 | Bind dyld / ranuras de símbolos indirectos y helpers de stub |

`--pdb` / `--map` nombran un archivo compañero autoritativo: no leerlo es un error, no un repliegue silencioso. `--no-debug` lee solo la imagen en todos los formatos.

Las firmas de procedimiento del PDB sirven para distinguir los asignadores que devuelven un valor de las funciones de liberación `void`. La recuperación rica de tipos locales y de pila desde un PDB sigue siendo limitada; cuando no puede establecer un tamaño de objeto exacto, la caza recurre al modelo de marco o de asignación e informa UNKNOWN en lugar de inventar un tamaño.

### Precedencia de `name_source`

Cada hallazgo lleva un `name_source` que describe de dónde salió el nombre del callee, con esta precedencia:

1. `rename` — un renombrado suministrado por el llamante
2. `import` — una entrada IAT (PE), PLT (ELF) o dyld-bind / stub (Mach-O)
3. `export` / `symbol` — una exportación o entrada de símbolos ya declarada por la imagen
4. `pdb` / `dwarf` / `map` — un símbolo de depuración que establece un marcador o coincide con el nombre declarado
5. `sig` — una coincidencia de firmas
6. `synthetic` — un marcador de posición para una rutina sin nombre

Un `memcpy` enlazado estáticamente nombrado por DWARF informa `dwarf`; un `memcpy` importado informa `import` en todos los formatos. Una coincidencia de firmas nunca desplaza un nombre que el depurador o la tabla de importaciones ya establecieron.

---

## Catálogo de sumideros y fuentes

El catálogo es una tabla configurable, no un conjunto fijo. Cada **sumidero** declara su clase de debilidad, su rol (copy, format, alloc, free, realloc) y las ranuras de argumentos que importan (destino, origen, longitud, capacidad). Un sumidero JSON de tipo copy o format también proporciona un efecto de llamada ejecutable. Cada **fuente** nombra un proveedor de entrada influida por el atacante.

Las entradas integradas viven en [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) y [`SafetySources.def`](../include/neverd/safety/SafetySources.def); cubren la familia de copias habitual del runtime C (`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), las variantes fortificadas `_chk` (límite explícito de destino), la familia de asignación y liberación (`malloc`/`calloc`/`realloc`/`free`, operadores `new`/`delete`) y APIs de montón Win32 opcionales. Las fuentes de entrada incluyen POSIX (`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, argumentos del programa) **y** Win32 (`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`), de modo que una caza PE no queda restringida a entradas POSIX.

Las grafías por formato se pliegan a una sola entrada: se quitan los guiones bajos iniciales (`_malloc`, `___strcpy_chk`) y los operadores `new`/`delete` mangled coinciden por alias.

Cuando un sumidero JSON de tipo copy o format omite `effect`, su aplicabilidad se deduce de la ranura de argumento referenciada más alta. Una copia exige entonces esa aridad exacta; un sumidero de formato acepta llamadas desde esa aridad mínima hasta el máximo variádico. Un objeto `effect` opcional puede establecer explícitamente mediante `min_arity` y `max_arity` (o `"variadic"`) un intervalo de aridad aceptado, incluidos argumentos adicionales de wrapper más allá de la aridad exacta de copy deducida; `min_arity` debe ser al menos la ranura de rol referenciada más alta más uno, mientras que `formats` y `abis` restringen la aplicabilidad. Si la aridad, el formato de objeto o la ABI de la llamada no coinciden, no se aplica ningún resumen y el resultado de mundo cerrado permanece UNKNOWN.

Amplíe o reemplace el catálogo con un archivo de especificación:

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
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

En una fuente personalizada, `out` y `return_tainted` son solo metadatos de descubrimiento. No establecen efectos ejecutables de memoria, valor de retorno ni taint. El esquema de fuentes actual carece de los contratos tipados de éxito, mutación, formato y ABI necesarios para esas semánticas, por lo que un análisis que dependa del efecto de una fuente personalizada permanece UNKNOWN. Las fuentes integradas no se ven afectadas: sus descriptores tipados y comprobados por aplicabilidad siguen proporcionando efectos ejecutables.

Un sumidero personalizado no acotado que solo tiene destino no se deduce de una entrada de fuente con el mismo nombre. Un sumidero personalizado similar a `gets` debe activar explícitamente `"unbounded": true`; añadir el mismo nombre al catálogo de fuentes no le concede un efecto ejecutable, y los campos de origen/longitud contradictorios se rechazan transaccionalmente.

---

## Hunt: veredictos de desbordamiento de copia

Para cada sumidero de copia, la caza recupera la capacidad de destino — tamaño de array declarado por depuración, luego un sitio de asignación de montón de tamaño conocido, luego un límite sano de marco de pila — y clasifica el argumento que decide la longitud de escritura con un recorrido SSA hacia atrás (siguiendo spill/reload por ranuras de pila):

- Una **longitud constante** dentro de una capacidad exacta es SAFE. Un desbordamiento constante solo es UNSAFE si el sumidero es alcanzable en una ruta corroborada; en otro caso permanece UNKNOWN.
- Las copias **fortificadas** `_chk` llevan un límite de destino en tiempo de ejecución. Un rechazo o un límite que cabe de forma demostrable es SAFE; una escritura factible más allá del objeto es UNSAFE; un límite no recuperado o inconcluso es UNKNOWN.
- Longitud **demostrablemente acotada** (llamada que devuelve longitud, máscara, clamp) se retira antes del solver, registrando el motivo. Solo es SAFE con un tamaño de destino exacto; una cota de región permanece UNKNOWN.
- Longitud **influida por el atacante** con capacidad conocida: el solver de vectores de bits. Si una longitud mayor que la capacidad es factible, el veredicto es UNSAFE. Los candidatos solo son reproducibles con un plan `process-input-v1` completo: inicialmente valores literales exactos del entorno y, como máximo, los bytes devueltos por el primer consumo de entrada estándar. argv, archivos, red, fuentes personalizadas o ambiguas siguen sin ser reproducibles e incluyen el motivo.
- Cualquier otra cosa — longitud o capacidad desconocidas — es UNKNOWN.

Toda capacidad recuperada es una **cota superior** del tamaño real, así que un desbordamiento demostrado nunca es un falso positivo.

### Entrada con formato

Para `scanf`/`fscanf` y sus grafías versionadas, un formato constante legible asigna cada conversión no suprimida a su argumento variádico de salida real. Las salidas `%s`/`%[` no acotadas contaminan los usos posteriores de cadenas; las salidas numéricas y de caracteres contaminan los valores cargados desde el objeto escrito, pero no el valor del puntero de salida. `sscanf` solo propaga esos efectos cuando su cadena de entrada ya está influida por el atacante. Las salidas de texto acotadas como `%Ns`/`%N[` propagan taint junto con una extensión `MaxBytes` que incluye el terminador; las variantes de caracteres anchos calculan esa extensión en bytes con el ancho de `wchar_t` de la plataforma. Las conversiones suprimidas, los argumentos sobrantes, los formatos dependientes de posición o no admitidos y `%n` permanecen UNKNOWN en lugar de adivinarse.

---

## Audit: veredictos de vida del montón

Para cada asignación, la auditoría sigue el handle en el grafo de control, incluso a través de spill/reload de pila, y aplica un resumen de escape (devuelto, almacenado por una dirección fuera de pila o entregado a un callee opaco):

- **Fuga** — el handle no se libera ni se deja escapar.
- **Doble liberación** — una segunda liberación es alcanzable tras la primera en un camino.
- **Uso después de liberar** — una desreferencia o uso opaco es alcanzable tras una liberación.

Los **envoltorios** de asignación y liberación se reconocen con resúmenes de escape por función, de modo que un reenvío `malloc`/`free` no oculta el defecto. Liberaciones en ramas mutuamente excluyentes no se informan como doble liberación.

La máquina de estados del montón emite primero una secuencia de eventos candidata (asignación, liberación, uso o salida por retorno). Una segunda pasada debe reproducir esa secuencia sobre un camino LowIR simbólico y demostrar que su predicado de camino es satisfacible antes de que el hallazgo sea UNSAFE de confianza ALTA. La falta de LowIR, las operaciones opacas, las llamadas sin resumen, la incertidumbre del solucionador y los límites de exploración rebajan el candidato a UNKNOWN. El havoc de memoria may-alias conservador se rastrea por separado, de modo que las escrituras ordinarias al marco de pila no invalidan una evidencia de alcanzabilidad por lo demás exacta.

---

## Presupuestos, salida y enlaces

La exploración de caza y el solver están acotados (`--max-paths`, `--max-steps`, `--max-loop`, `--solver-conflicts`); agotar el presupuesto produce UNKNOWN. Ambos comandos imprimen JSON y respetan `-o`. El código de salida es `0` para SAFE, `2` para UNSAFE y `1` para UNKNOWN o un error.

Los mismos análisis están disponibles por la API C (`neverd_session_audit_json` / `neverd_session_hunt_json` con `neverd_safety_options` versionado) y el SDK de Python (`Session.audit()` / `Session.hunt()`).

### Esquema de un hallazgo

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" }, "candidate_values": [{ "name": "copy_length", "value": "17" }, { "name": "argv[1]", "value": "16 bytes" }], "replayable": false, "replay": { "adapter": "process-input-v1", "reason": "argv input is not supported by process-input-v1" }, "symbolic_model": [{ "id": 0, "name": "copy_len", "width": 64, "value_hex": "0x11", "origin": "input" }] }
}
```

`replayable` es evidencia derivada, no una promesa independiente: solo es verdadero si `replay` contiene un plan de entrada completo para el adaptador `process-input-v1`. El plan registra los bytes exactos del entorno, la primera secuencia de entrada estándar si se usa y los enlaces desde los identificadores de asignación del solver; si no existe, `replay.reason` explica el motivo. Los campos son aditivos; el `schema_version` superior sigue siendo `1`.

---

## Cotas de falsos positivos y alcance

- La capacidad es exacta o una cota superior del tamaño real, así que UNSAFE refleja un desbordamiento real. Sin tamaño exacto, una cota de región insuficiente para probar seguridad produce UNKNOWN.
- Una copia acotada se retira antes del solver y cuenta en `skipped`; una capacidad exacta puede probar SAFE, mientras una cota sola permanece UNKNOWN.
- Las copias de caracteres anchos y de anexado catalogadas permanecen UNKNOWN hasta recuperar el ancho de elemento y la extensión actual del destino. Los asignadores por parámetro de salida y la propiedad condicional de `realloc` también permanecen UNKNOWN si no puede probarse la transición del manejador.
- **P0** (esta versión, los tres formatos): catálogo de sumideros, prefiltro de argumentos, caza de desbordamiento de copia, auditoría de vida del montón. Cada host ejecuta seis fixtures PE, ELF y Mach-O para x86-64 y AArch64.
- **P1**: ya están disponibles los desbordamientos de pila/global, las lecturas locales no inicializadas y las comprobaciones de cadenas de formato; los tipos de pila PDB más ricos y más asignadores de plataforma siguen siendo cobertura incremental, y la ausencia de un resumen exacto permanece UNKNOWN.
- **P2**: comprobaciones de tiempo de ejecución insertadas por patch, alcanzabilidad interprocedural del atacante.
