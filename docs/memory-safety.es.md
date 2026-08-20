**Idiomas**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← Índice de documentación](README.es.md)

# Auditoría y caza de seguridad de memoria

NeverD analiza un binario cargado en busca de dos familias de defectos de seguridad de memoria y los informa como JSON estructurado. Ambas pistas se ejecutan sobre el IR levantado, independiente del formato, de modo que **PE/COFF, ELF y Mach-O son objetivos de primer nivel, en igualdad** — un hallazgo nunca queda detrás del escáner o la tabla de importaciones de un solo formato.

| Pista | Comando | Informa |
|-------|---------|---------|
| **Audit** | `neverd audit <binary>` | Defectos de vida del montón: fuga, doble liberación, uso después de liberar |
| **Hunt** | `neverd hunt <binary>` | Desbordamientos de copias peligrosas con un testigo concreto reproducible |

El motor reutiliza la ejecución simbólica y el solver de vectores de bits internos de NeverD para testigos y alcanzabilidad. No hay solver externo, VM ni contenedor.

---

## Invariante central: fallar cerrado

Una operación no levantada, una llamada cuyos argumentos el paso ABI no pudo recuperar, un destino indirecto sin resolver o un presupuesto agotado producen **UNKNOWN**, nunca SAFE. Un destino cuya capacidad no se puede recuperar es UNKNOWN. El lifting estricto no cambia; la capa de seguridad solo añade veredictos conservadores encima.

---

## Contrato de identidad por formato

Ambas pistas requieren el pipeline de lift (recupera argumentos por llamada). Cada callee se nombra con la misma vista de identidad que el resto de NeverD. El orden de descubrimiento de depuración no cambia.

| Formato | Depuración (por precedencia) | Resolución de importaciones / thunks |
|---------|------------------------------|--------------------------------------|
| **PE/COFF** | `--pdb`, directorio de depuración o `.pdb` hermano, luego `/MAP` de MSVC | Ranuras IAT y thunks `__imp_`, importaciones ordinales |
| **ELF** | DWARF en la imagen, `*.debug` separado, luego MAP GNU/LLD | Stubs PLT resueltos al nombre importado |
| **Mach-O** | DWARF en la imagen, `.dSYM` adyacente, luego `-map` de ld64 | Bind dyld / ranuras de símbolos indirectos y helpers de stub |

`--pdb` / `--map` nombran un archivo compañero autoritativo: no leerlo es un error, no un repliegue silencioso. `--no-debug` lee solo la imagen en todos los formatos.

### Precedencia de `name_source`

Cada hallazgo lleva un `name_source` que describe de dónde salió el nombre del callee, con esta precedencia:

1. `rename` — un renombrado suministrado por el llamante
2. `import` — una entrada IAT (PE), PLT (ELF) o dyld-bind / stub (Mach-O)
3. `pdb` / `dwarf` / `map` — un símbolo de depuración, según el cargador
4. `export` / `symbol` — una exportación o entrada de tabla de símbolos
5. `sig` — una coincidencia de firmas
6. `synthetic` — un marcador de posición para una rutina sin nombre

Un `memcpy` enlazado estáticamente nombrado por DWARF informa `dwarf`; un `memcpy` importado informa `import` en todos los formatos. Una coincidencia de firmas nunca desplaza un nombre que el depurador o la tabla de importaciones ya establecieron.

---

## Catálogo de sumideros y fuentes

El catálogo es una tabla configurable, no un conjunto fijo. Cada **sumidero** declara su clase de debilidad, su rol (copy, format, alloc, free, realloc) y las ranuras de argumentos que importan (destino, origen, longitud, capacidad). Cada **fuente** nombra un proveedor de entrada influida por el atacante.

El catálogo integrado cubre la familia de copias habitual del runtime C (`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), las variantes fortificadas `_chk` (límite explícito de destino), la familia de asignación y liberación (`malloc`/`calloc`/`realloc`/`free`, operadores `new`/`delete`) y APIs de montón Win32 opcionales. Las fuentes de entrada incluyen POSIX (`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, argumentos del programa) **y** Win32 (`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`), de modo que una caza PE no queda restringida a entradas POSIX.

Las grafías por formato se pliegan a una sola entrada: se quitan los guiones bajos iniciales (`_malloc`, `___strcpy_chk`) y los operadores `new`/`delete` mangled coinciden por alias.

Amplíe o reemplace el catálogo con un archivo de especificación:

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 }
] }
```

---

## Hunt: veredictos de desbordamiento de copia

Para cada sumidero de copia, la caza recupera la capacidad de destino — tamaño de array declarado por depuración, luego un sitio de asignación de montón de tamaño conocido, luego un límite sano de marco de pila — y clasifica el argumento que decide la longitud de escritura con un recorrido SSA hacia atrás (siguiendo spill/reload por ranuras de pila):

- **Longitud constante** comparada directamente con la capacidad → SAFE o UNSAFE.
- Copias **fortificadas** `_chk` con límite de destino en tiempo de ejecución → SAFE.
- Longitud **demostrablemente acotada** (llamada que devuelve longitud, máscara, clamp) se retira como SAFE skip, registrando el motivo.
- Longitud **influida por el atacante** con capacidad conocida: el solver de vectores de bits. Si una longitud mayor que la capacidad es factible, el veredicto es UNSAFE y el modelo del solver es el testigo concreto.
- Cualquier otra cosa — longitud o capacidad desconocidas — es UNKNOWN.

Toda capacidad recuperada es una **cota superior** del tamaño real, así que un desbordamiento demostrado nunca es un falso positivo.

---

## Audit: veredictos de vida del montón

Para cada asignación, la auditoría sigue el handle en el grafo de control, incluso a través de spill/reload de pila, y aplica un resumen de escape (devuelto, almacenado por una dirección fuera de pila o entregado a un callee opaco):

- **Fuga** — el handle no se libera ni se deja escapar.
- **Doble liberación** — una segunda liberación es alcanzable tras la primera en un camino.
- **Uso después de liberar** — una desreferencia o uso opaco es alcanzable tras una liberación.

Los **envoltorios** de asignación y liberación se reconocen con resúmenes de escape por función, de modo que un reenvío `malloc`/`free` no oculta el defecto. Liberaciones en ramas mutuamente excluyentes no se informan como doble liberación.

---

## Presupuestos, salida y enlaces

La exploración de caza y el solver están acotados (`--max-paths`, `--max-steps`, `--max-loop`, `--solver-conflicts`); agotar el presupuesto produce UNKNOWN. Ambos comandos imprimen JSON y respetan `-o`. El código de salida es `0` en una ejecución limpia, `2` si hay un hallazgo UNSAFE y `1` ante error.

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "17 bytes" } }
}
```

---

## Cotas de falsos positivos y alcance

- La capacidad es siempre una cota superior, así que UNSAFE refleja un desbordamiento real. Un búfer demasiado pequeño cuya talla declarada no está disponible puede informarse SAFE en lugar de UNSAFE (omisión conservadora, nunca falsa alarma).
- Una copia acotada en longitud se retira como SAFE skip; esto favorece la precisión en los casos controlados por el atacante que la caza está diseñada para probar.
- **P0** (esta versión, los tres formatos): catálogo de sumideros, prefiltro de argumentos, caza de desbordamiento de copia, auditoría de vida del montón.
- **P1**: desbordamiento de pila/global, lecturas no inicializadas, cadenas de formato, tipos de pila PDB más ricos, más asignadores de plataforma.
- **P2**: comprobaciones de tiempo de ejecución insertadas por patch, alcanzabilidad interprocedural del atacante.
