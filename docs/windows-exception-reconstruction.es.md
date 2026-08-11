**Idiomas**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# Reconstrucción de excepciones de Windows

[← Índice de documentación](README.es.md)

NeverD conserva la información de excepciones tabulares de Windows durante la
carga, el lift, la decompilación y la reescritura binaria. Los metadatos forman
parte del contrato ejecutable de una función: se rechaza una reescritura cuando
no puede demostrarse la coherencia del código generado, los records
runtime-function, las tablas de lenguaje y las tablas de protección.

Se distinguen tres niveles de soporte:

- **Análisis**: decodifica la representación nativa en records normalizados y
  comprobados, visibles para la pipeline IR.
- **Decompilación**: convierte regiones protegidas reducibles en nodos de
  excepción HighIR; las demás conservan anotaciones nativas deterministas.
- **Reconstrucción nativa**: patch mode puede pedir a LLVM un contrato de
  excepción de reemplazo completo e instalarlo en el PE final.

El soporte de análisis no implica reconstrucción nativa.

## Matriz de soporte

| Forma nativa | Lift y análisis | Salida de alto nivel | Patch mode |
|--------------|-----------------|----------------------|------------|
| unwind x64 v1/v2 | Records, operaciones, cadenas, datos de handler y procedencia completos y comprobados | Resumen frame/unwind y regiones estructuradas cuando corresponda | Records primary completos; `.pdata`/`.xdata` generadas reemplazan el cierre obsoleto |
| unwind x64 v3/APX | Payload v3, epílogos y contabilidad de operaciones dedicados | Anotación v3 explícita | Solo análisis; se rechaza una función tocada |
| unwind ARM32/ARM64 packed | Rangos, campos packed e identidad primary/fragment | Resumen frame/unwind | Solo records primary completos sin lenguaje ni fragmentos direccionables por separado |
| unwind ARM32/ARM64 unpacked | Header/extensión xdata comprobados, asociación de handler y fragmentos | Resumen frame/unwind | Solo records primary completos sin lenguaje ni fragmentos direccionables por separado |
| `__C_specific_handler` | Scopes, filtros, finally, handlers y continuaciones | `__try`/`__except`/`__finally` si es reducible; anotación en otro caso | Reconstrucción x64 nativa de grafos completos y representables |
| `__CxxFrameHandler3` | Unwind/try maps, catches, offsets de objeto/frame, continuaciones e IP-to-state | Intervalos reducibles como C++ HighIR con anotaciones de tipo compatibles con C | Reconstrucción x64 del subconjunto estrecho y verifier-clean descrito abajo |
| `__CxxFrameHandler4` | Decodificación variable acotada al grafo C++ común | Mismo HighIR con procedencia FH4 | Solo análisis; se rechaza una función tocada |
| `__GSHandlerCheck_SEH/EH/EH4` | Personality envuelta y procedencia GS cookie comprobada | Grafo base y anotación wrapper | Solo análisis; rechazo sin downgrade |
| EH x86 por registration chain | Separado del EH tabular | Anotación de forma no soportada | No se reconstruye |

Un record malformed nunca se considera completo. Una decodificación parcial
sirve para inspección, pero no autoriza generación nativa. Si el header xdata
ARM aún demuestra un rango de fragmento ejecutable acotado aunque el cuerpo
unwind esté dañado, el rango permanece para el desensamblado; el record queda
malformed y no es patchable.

## Modelo normalizado

`ExceptionInfo` pertenece a `BinaryImage`. Cada `ExceptionFunction` contiene:

- un rango de código semiabierto comprobado;
- identidad primary, chained o fragment;
- encoding unwind nativo y procedencia runtime/unwind exacta;
- operaciones y epílogos normalizados, incluidos operands opacos desconocidos;
- identidad exacta de personality y datos de handler;
- scopes SEH, maps de estado C++ y datos GS cookie opcionales;
- estado `Complete`, `Partial` o `Malformed` y diagnósticos deterministas.

El loader no expone punteros crudos. Las RVA nativas se conservan para diagnóstico
y reemplazo; los consumidores IR usan solo VA y rangos validados.

El índice de la imagen permite solapamiento chained/fragment y devuelve la
función más específica. Directorios, rangos, punteros, contadores, transiciones,
enteros comprimidos o cadenas dañados, y el agotamiento del presupuesto, reducen
el estado de parse correspondiente.

Los límites se aplican por tabla y al grafo completo de cada función. Reutilizar
una handler map en muchas try entries no supera el presupuesto agregado. Los
records FH3 que comparten `FuncInfo` y personality forman un grupo acotado:
aceptan sus catch funclets, no direcciones runtime ajenas.

## Contrato IR

Los metadatos atraviesan todas las representaciones sin cambiar el CFG ordinario:

- LowIR divide bloques en límites, estados, filtros, handlers, cleanup y continuaciones.
- Successors/predecessors excepcionales se mantienen separados de los normales.
- MedIR conserva el descriptor normalizado y aristas excepcionales estables.
- HighIR distingue `SEHTry` y `CxxTry` y preserva VA, type descriptors,
  adjectives, offsets, acciones, estados y continuaciones.

El structurer HighIR es conservador: mueve solo un tramo contiguo totalmente
contenido en una región completa y procesa anidamientos desde dentro. Regiones
cruzadas, grafos parciales, límites ambiguos y funclets externos mantienen el
flujo original.

El backend C emite sintaxis MSVC SEH para una región reducible de una cláusula.
Como HighC es un backend C, catches y cleanup C++ se expresan como comentarios C
deterministas, sin afirmar que el resultado sea C++ compilable.

## Esquema de metadatos LLVM

Cada función de excepción analizada recibe metadatos sin pérdida, incluso sin
lowering WinEH nativo:

- attachment `neverd.windows.eh`;
- marcador nativo `neverd.windows.eh.native`;
- tabla de módulo `neverd.windows.eh.functions`;
- versión de esquema `3`.

El record fijo conserva estado, encoding, rango, RVA runtime/unwind, tipo y
cadena, palabra packed, frame, nombres de personality, handler, bytes unwind,
operaciones/epílogos, scopes SEH, maps C++, datos GS, diagnósticos y permiso de
regeneración. El patch exige versión exacta y rango idéntico a la imagen cargada.

El lowering SEH x64 usa LLVM WinEH y solo emite `invoke`/funclets verifier-clean
cuando el grafo completo es representable. FH3 además exige:

- x64 COFF, unwind v1/v2, metadatos completos y grafo FH3 síncrono válido;
- sin `noexcept`, async, separated-funclet, GS, FH4 ni flags desconocidos;
- intervalos anidados o disjuntos, nunca cruzados;
- sin destructor/unwind action, construcción de catch object ni dependencia de parent frame;
- handler en un bloque normal sin predecessor ni call;
- LLVM `invoke` para toda operación protegida que pueda hacer unwind.

En otro caso el IR sigue siendo analizable y sin pérdida, pero se rechaza el
reemplazo nativo. Entry point PE, callbacks TLS y raíces CRT son fronteras de
conservación, no candidatos ABI ordinarios.

## Transacción de patch

Una reescritura soportada es una sola transacción PE:

1. Validar cada función tocada contra el grafo cargado y los metadatos LLVM.
2. Compilar preservando identidad, alineación y traits de secciones y referencias
   semánticas; externalizar la personality Windows local y enlazar xdata al
   handler ejecutable original demostrado.
3. Conservar runtime functions intactas y retirar todo el cierre reemplazado,
   incluidos records chained.
4. Relocar code/xdata, fusionar y ordenar pdata, rechazar solapamientos, demostrar
   la clase de personality e instalar un único directorio de excepciones.
5. Preservar CFG, resolver `.gfids`/`.gehcont`, fusionar Guard CF/EH continuation
   y actualizar load-config. Un helper sin resolver aborta; CFW, return-flow
   guard, retpolines y XFG quedan analysis-only.
6. Volver a analizar la imagen de bytes completa antes de escribir.

La extensión del fork LLVM es genérica: el writer final-image conserva traits de
sección y referencias simbólicas. PE/MSVC, política, fusión, load-config y
validación final permanecen en NeverD.

Las entradas Guard CF/EH continuation originales se conservan porque sus
trampolines siguen siendo objetivos indirectos válidos. Los objetivos generados
deben estar en el código emitido y las tablas estrictamente ordenadas por RVA.

## Validación de imagen final

Se rechaza un PE parcheado salvo que:

- LLVM acepte COFF y coincidan machine, clase, secciones, directorios, base y extensión;
- extensiones raw/virtuales estén acotadas y no se solapen;
- el directorio de excepciones esté respaldado por el archivo y dentro de la imagen;
- runtime functions estén ordenadas, no vacías, sin solape y sean ejecutables;
- RVA/header/version/flags/handler/cadenas unwind x64 sean válidos;
- imports, exports y símbolos COFF finales permitan volver a analizar SEH/FH3;
- records ARM/xdata describan versión y rango soportados;
- existan campos Guard CF/EH cuando los flags anuncian tablas;
- punteros, contadores y strides estén en archivo/imagen y cada objetivo sea ejecutable.

Cualquier fallo aborta el patch; no se escribe una imagen best-effort.

## Verificación enfocada

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

La fixture x64 protegida usa `/guard:cf` y `/guard:ehcont` y comprueba scopes
SEH, Guard, HighC, patch, recarga, orden y objetivos. La fixture FH3 comprueba
tablas fijas, anotaciones, personality, try/catch e IP-to-state. Ejecute también
los casos ARM al cambiar el parser.

## Ampliar el soporte nativo

Cada forma nativa nueva debe incluir en el mismo cambio:

- parser completo y acotado e invariantes del modelo;
- round-trip HighIR/metadatos LLVM;
- IR nativo verifier-clean para cada grafo aceptado;
- conservación necesaria de secciones y referencias;
- fixture PE enlazada para arquitectura/personality/versión exactas;
- validación de exception-directory, load-config e imagen final;
- pruebas de rechazo explícitas para formas no soportadas cercanas.

Poder decodificar un record no basta para ampliar la allow-list. El criterio es
preservar el comportamiento de excepciones en la imagen final enlazada.
