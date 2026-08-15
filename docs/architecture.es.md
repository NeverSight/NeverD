**Idiomas**: [English](architecture.md) | [简体中文](architecture.zh-CN.md) | [繁體中文](architecture.zh-TW.md) | [日本語](architecture.ja.md) | [한국어](architecture.ko.md) | [Français](architecture.fr.md) | [Deutsch](architecture.de.md) | [Español](architecture.es.md) | [Italiano](architecture.it.md) | [Русский](architecture.ru.md) | [العربية](architecture.ar.md)

[← Índice de documentación](README.es.md)

# Arquitectura de NeverD

Esta guía describe los límites de producción que debe conocer quien contribuya
para modificar NeverD con seguridad. Cubre deliberadamente solo el código
propio de NeverD; los submódulos LLVM, Capstone y Unicorn mantienen su propia
arquitectura interna.

## Límite del sistema

```mermaid
flowchart LR
  CLI["tools/neverd CLI"] --> CAPI["libneverd C API"]
  SDKUser["SDK user or plugin"] --> CAPI
  CAPI --> Session["sdk::Session"]
  Session --> Loader["format loader"]
  Loader --> Image["BinaryImage"]
  Image --> Pipeline["Pipeline"]
  Pipeline --> Low["LowIR"]
  Low --> Med["MedIR"]
  Med --> High["HighIR"]
  High --> HighC["structured C"]
  Med --> LLVM["LLVM IR"]
  LLVM --> LLVMOut["LLVM IR or LLVM-derived C"]
  LLVM --> Codegen["target codegen"]
  Codegen --> Rewriter["PE / ELF / Mach-O rewriter"]
  Rewriter --> Patched["patched binary"]
```

NeverD tiene cuatro representaciones IR, pero no forman una secuencia
obligatoria de cuatro saltos. `LowIR -> MedIR` es común. La descompilación
estructurada usa después `MedIR -> HighIR -> C`; `lift`, `decompile --llvm` y
`patch` toman la ruta directa `MedIR -> LLVM IR`. En particular, los modos
patch y lift omiten HighIR de forma deliberada.

El CLI analiza comandos en `tools/neverd`, crea un `neverd_session_t` y llama a
la API pública de `include/neverd/sdk/NeverDCAPI.h`. El estado del motor reside
en `lib/sdk/SessionImpl.h`; `neverd_session_load` elige un loader y construye
una `BinaryImage`, mientras que las operaciones basadas en IR ejecutan
`lib/pipeline/Pipeline.cpp` bajo demanda. El ejecutable `neverd` enlaza
`neverd_shared`; los archivos de componentes y sus dependencias LLVM/Capstone
son detalles privados de esa biblioteca compartida. El CLI usa LLVM Support
para su interfaz de línea de comandos, pero no evita la API C
para controlar el motor.

## Representaciones IR y rutas

| Representación | Propósito | Definiciones y transformaciones principales |
|----------------|-----------|----------------------------------------------|
| LowIR | Operaciones `NdOp` independientes de arquitectura, bloques básicos, CFG y metadatos de jump tables | `include/neverd/ir/low`, `lib/ir/low`, producido por `lib/decode` + `lib/lift` |
| MedIR | Tipos, ABI/convenciones de llamada, modelo de memoria/pila, flags, llamadas y flujo similar a SSA | `include/neverd/ir/med`, `lib/ir/med` |
| HighIR | Expresiones y control de flujo estructurados para C legible | `include/neverd/ir/high`, `lib/ir/high`, emitido por `lib/backend/c/HighC` |
| LLVM IR | Optimización, C derivado de LLVM, generación de código objetivo y entrada de reescritura binaria | `lib/backend/llvm`, optimizado/orquestado por `lib/pipeline` |

| Ruta del usuario | Camino de representaciones | Salida |
|-----------------|--------------------------|--------|
| Volcado Low/Med | Binary -> LowIR, opcionalmente -> MedIR | Texto de diagnóstico |
| Volcado High o `decompile` | Binary -> LowIR -> MedIR -> HighIR | HighIR o C estructurado |
| `lift` | Binary -> LowIR -> MedIR -> LLVM IR | `.ll` |
| `decompile --llvm` | Binary -> LowIR -> MedIR -> LLVM IR | C derivado de LLVM |
| `patch` | Binary -> LowIR -> MedIR -> LLVM IR -> codegen | Binario reescrito |

`lib/pipeline/Pipeline.cpp` es la referencia para elegir la ruta. Mantenga la
lógica específica de una representación en su biblioteca IR o backend; el
pipeline debe orquestar esos componentes, no absorber sus algoritmos.

## Contrato de traducción entre arquitecturas

`include/neverd/translate` define una capa de contrato, no un
backend de ejecución. `GuestState` modela el estado visible de la máquina de
forma independiente de la arquitectura para `x86_32`, `x86_64`, `AArch64` y
`ARM32`. Su serialización canónica versión 1 usa campos little-endian de ancho
fijo, identificadores de registro estables, colecciones ordenadas y validación
fail-closed, por lo que el estado persistido no depende de la disposición C++
del host.

La base wire v1 de `GuestState` queda congelada de forma permanente. Todo estado
fuera de esa base debe usar un ID de registro de extensión dentro del rango reservado junto
con un nombre canónico en minúsculas, o pasar a una nueva versión wire con un
upgrader explícito; queda prohibido modificar la base v1 en el sitio.

Para un guest `ARM32`, `ExecutionMode` es el modo de decodificación autoritativo
y debe concordar con `CPSR.T`. El PC almacenado es siempre la dirección de
instrucción canónica con el bit 0 borrado; el modo ARM exige además alineación
de palabra.

El contrato de pares define `x86_64 -> AArch64`,
`AArch64 -> x86_64`, `x86_32 -> AArch64/ARM32` y
`ARM32 -> x86_32/x86_64`. `ContractDefined` significa que una solicitud se
puede validar y persistir, no que el código se pueda traducir o ejecutar. La
política JIT solo acepta el host nativo del proceso; la política AOT requiere
una arquitectura host y un target triple explícitos; una CPU o un conjunto de
features seleccionados también deben ser explícitos.

`ResolvedHostTarget` convierte esa selección en un resultado concreto. La
resolución `Native` obtiene del proceso el triple, la CPU y el conjunto de
features habilitadas o deshabilitadas. La resolución `Explicit` valida y
normaliza la arquitectura, el triple, la CPU y las features proporcionados por
el llamador, y rechaza conflictos. Su identidad de caché versionada se construye
en un orden de bytes determinista a partir del target normalizado, sin
direcciones del proceso ni texto dependiente de la locale.

Un `TranslationExit` versionado registra una causa de parada estable y el
payload tipado correspondiente para syscalls, excepciones o señales, puntos de
interrupción, instrucciones no admitidas, automodificación, presupuestos de
recursos, llamadas externas, fallos de memoria y otras condiciones terminales.
Así, los consumidores no tienen que reinterpretar un entero sin tipo según la
causa de parada.

Salvo en el caso `BudgetExhausted` correspondiente, los conteos de instrucciones,
blocks y código generado no pueden superar el presupuesto no nulo de la solicitud.
El agotamiento de instrucciones y blocks se detiene exactamente en el limit. El
tamaño de un objeto generado solo se conoce tras un codegen indivisible, por lo
que su resultado de agotamiento puede indicar `Observed > Limit`; ese objeto
rechazado nunca se enlaza, publica ni ejecuta. Cada payload `BudgetExhausted`
identifica exactamente el limit solicitado, nunca un umbral derivado o privado
de la implementación.

El contrato backend-private `RuntimeControlBlockV1` mide
exactamente 128 bytes, está alineado a 8 bytes y queda restringido por magic,
version, size y offsets de campo fijos de v1, campos reservados a cero y exits
tipados coherentes. No contiene contenedores C++, punteros del host ni alias de
direcciones guest. No es el layout C++ ni el formato wire de `GuestState`; un
backend que implemente este contrato debe convertir explícitamente el estado a
este registro.

La superficie fija de llamadas v1 para código generado contiene exactamente
ocho helpers: `nvd_rt_v1_load8_le`, `nvd_rt_v1_load16_le`,
`nvd_rt_v1_load32_le`, `nvd_rt_v1_load64_le`, `nvd_rt_v1_store8_le`,
`nvd_rt_v1_store16_le`, `nvd_rt_v1_store32_le` y `nvd_rt_v1_store64_le`.
Sus nombres, firmas y procedencia de punteros deben coincidir exactamente; un
backend enlaza esta tabla finita de forma explícita y nunca recurre a la
resolución ambiental de símbolos. La validación de la generation ejecutable y
el polling de presupuesto/cancelación son operaciones exclusivas del dispatcher
de confianza; `nvd_rt_v1_validate_generation` y `nvd_rt_v1_poll` no son helpers
para código generado. El dispatcher de confianza del host también controla la
selección de blocks y no es invocable desde el IR generado; los translated
blocks devuelven en su lugar un código de exit tipado. El IR generado solo puede
leer directamente el slot runtime scalar-result declarado.

`RuntimeSymbolRegistryV1` materializa esa tabla de helpers como un registro
cerrado del host. Su construcción valida el conjunto ABI-v1 completo, los
nombres canónicos exactos, las clases de helper, las firmas y, para cada
entrada, exactamente un puntero de función no nulo acorde con su clase. La
búsqueda solo acepta el nombre exacto, nunca consulta símbolos ambientales del
proceso ni del cargador dinámico y proporciona al verifier de objetos los mismos
nombres ordenados como allowlist. Su identidad versionada cubre nombres, clases
de helper y forma de ABI, pero excluye deliberadamente las direcciones nativas,
por lo que es estable bajo ASLR.

`RuntimeCodeMemory` posee almacenamiento de código generado aislado por páginas
y solo permite la publicación unidireccional `RW -> RX`. La memoria nunca es
escribible y ejecutable a la vez, no se puede reabrir para escritura, comprueba
los límites de escrituras y puntos de entrada e invalida la caché de
instrucciones del host al publicarse. El smoke test nativo solo ejecuta una
pequeña secuencia de instrucciones host después de la publicación; demuestra
esta frontera de memoria W^X, no un motor de traducción.

`GuestMemoryRuntime` está aislado del `GuestState` lógico: su construcción
primero valida el estado y después copia los bytes y metadatos de las regiones
a un índice privado ordenado. Las direcciones virtuales guest son solo claves
de búsqueda y nunca se convierten en punteros del host. Los accesos escalares
comprobados notifican faults tipados de ancho, alineación, overflow, ausencia de
mapping, cruce de región, permisos, escritura ejecutable, overflow o discordancia
de generation y violación de policy. Los presupuestos de instrucciones/blocks,
la cancelación, el seguimiento de generation y las policies de escritura de
código `RejectExecutableWrites`, `InvalidateOnExecutableWrite` y
`ValidateBeforeDispatch` también producen registros tipados coherentes en vez
de comportamiento implícito del host.

`TranslationObjectCompilerV1` es la frontera verificada entre LLVM IR y objeto.
Valida un módulo de entrada const, lo clona antes de cualquier transformación,
compone la simplificación semántica controlada por pruebas con la optimización
LLVM de `O0` a `O3`, vuelve a validar el IR final y emite objetos relocatable
ELF, COFF o Mach-O para las cuatro arquitecturas host del contrato. Canonicaliza
los manifests exactos de blocks y símbolos runtime con el mangling del target,
audita cada objeto emitido y devuelve la identidad del registro runtime junto
con claves de caché versionadas para la solicitud y el artefacto. Con un
presupuesto de bytes generados distinto de cero, solo un objeto que lo satisfaga
puede pasar a la verificación del artefacto. LLVM emite primero en un buffer
privado para medir el tamaño exacto e indivisible; un objeto sobredimensionado se
rechaza antes de publicarse y auditarse, y la telemetría tipada conserva el tamaño
observado y el limit exacto solicitado. Cero significa sin límite de la política
del llamador. El compilador termina en bytes relocatable auditados: no los enlaza,
publica, despacha ni ejecuta, y no proporciona lowering de instrucciones guest.

El verifier post-codegen audita objetos relocatable ELF,
COFF y Mach-O como un conjunto cerrado. El formato y la arquitectura deben
coincidir exactamente con el host elegido; los símbolos indefinidos deben
pertenecer exactamente a la allowlist finita de helpers y los símbolos
dinámicos están prohibidos. Las relocations usan whitelists directas explícitas
con comprobaciones de encoding, ancho, alineación, offset, destino cargable y
una definición non-preemptible local al objeto o un helper autorizado
exactamente. Se rechazan W+X, metadatos unwind/exception/initializer, TLS,
IFUNC, GOT y la indirección PLT ordinaria, relocations dinámicas, definiciones
weak/preemptible o seleccionables, secciones asignadas desconocidas y
directivas del linker. La forma `R_X86_64_PLT32` que LLVM usa para una llamada
ELF x86-64 hidden solo se admite cuando la policy v1 demuestra un branch directo
sealed al helper runtime exacto; no autoriza una ruta PLT ni GOT. Los artefactos
ELF `ET_REL` no pueden contener program headers ni segmentos. Los load commands
de Mach-O siguen una lista positiva: exactamente un segmento del ancho
correspondiente y como máximo una symbol table, dynamic-symbol table,
platform-version y orden data-in-code, con comprobación de sus dependencias. Las
opciones del linker y cualquier otro command se rechazan.

`TranslationObjectRequestV1` es la primera etapa pública y deliberadamente
estrecha que transforma bytes guest en un objeto sobre estos contratos. Del
subconjunto v1 fail-closed publicado de registros escalares x86-64 solo acepta
codificaciones canónicas sin prefijos legacy: formas `MOV`, `ADD`/`SUB` y
`AND`/`OR`/`XOR` con REX.W sobre GPR de ancho completo cuyos operandos tienen
las formas LowIR admitidas de registro/inmediato. Las formas aritméticas
conservan sus cálculos de flags escalares; las lógicas y `TEST` calculan los
flags definidos por la arquitectura y conservan `AF` en el modelo de estado de
NeverD. El schema 9 también acepta `CMP` registro/registro de ancho completo con
`39/3B`, `CMP` registro/inmediato con `81/7`, `83/7` y `3D`, `TEST` de ancho
completo registro/registro con `85` y registro/inmediato con `F7/0` y `A9`. Los
encodings canónicos `C3`
`RET` y `C2 iw` `RET imm16` terminan blocks de retorno; los encodings de `JMP`
relativo directo canónicos `EB cb` y `E9 cd` terminan blocks de branch directo.
El schema de lowering publicado es 9. Los branches Jcc tradicionales,
canónicos y sin prefijo legacy se limitan a: `JO`/`JNO` corto `70/71 cb` o
cercano `0F 80/81 cd`; `JB`/`JAE` con `72/73 cb` o `0F 82/83 cd`; `JE`/`JNE`
con `74/75 cb` o `0F 84/85 cd`; `JBE`/`JA` con `76/77 cb` o `0F 86/87 cd`;
`JS`/`JNS` con `78/79 cb` o `0F 88/89 cd`; `JP`/`JNP` con `7A/7B cb` o
`0F 8A/8B cd`; `JL`/`JGE` con `7C/7D cb` o `0F 8C/8D cd`; y `JLE`/`JG` con
`7E/7F cb` o `0F 8E/8F cd`. `JRCXZ`/`JECXZ`/`JCXZ` y
`LOOP`/`LOOPE`/`LOOPNE` siguen sin publicarse y fallan fail-closed. El `F7 /1`
reservado, los operandos de memoria guest, los registros parciales, los prefijos
legacy y los bits de extensión REX semánticamente redundantes también fallan
fail-closed. Solo emite un objeto relocatable ELF o Mach-O AArch64 little-endian
auditado. Las operaciones ordinarias de memoria guest, las formas
de registro parcial, toda instrucción o codificación fuera de ese subconjunto
exacto, todo flujo de control salvo retornos, esos saltos directos y los branches
Jcc publicados arriba, y toda operación LowIR no implementada
por el lowerer se rechazan antes de emitir el objeto. La
lectura comprobada de la dirección de retorno que requiere `RET` forma parte de su
contrato de terminador y no publica un lowering general de memoria guest. La
solicitud reconstruye y valida el descriptor del block, usa la misma target
machine resuelta para el lowering y la emisión del objeto, y combina la
simplificación semántica controlada por pruebas con la pipeline de optimización
`O2` predeterminada de LLVM. Esta etapa no cubre otras instrucciones x86-64,
otros pares guest/host ni la dirección inversa de AArch64 a x86-64.

El punto de entrada C público
`neverd_translate_x86_64_block_to_aarch64_object_v1`, el wrapper Python ctypes
`translate_x86_64_block_to_aarch64_object` y el comando
`neverd translate-object` exponen ese mismo límite solo de objeto. Python usa
`TranslationObjectFormat.ELF` o `.MACHO`. Los fallos de traducción nativa lanzan
una `TranslationError` tipada que porta `TranslationErrorCode`; la validación
local de argumentos lanza en cambio `TypeError` o `ValueError`. Cuando tiene
éxito, Python devuelve un resultado inmutable de su propiedad. El resultado C
es propietario de los bytes del objeto, las identidades estables de caché y la
telemetría de optimización; la CLI solo escribe el objeto ELF o Mach-O
seleccionado. Las tres
superficies terminan antes del enlace, la carga, el dispatch, la ejecución y la
depuración; no son interfaces de sesión de ejecución.

`verifyTranslationLinkGraphV1` añade una segunda auditoría independiente antes de cualquier
allocation. Construye un grafo LLVM JITLink efímero desde un objeto ELF o Mach-O
AArch64 aceptado y comprueba el target, los permisos de secciones, los manifests
de símbolos block/runtime, el cierre de símbolos externos y los tipos y destinos
de edges. El grafo se destruye tras producir el resultado de auditoría sin
direcciones. Superar esta auditoría no enlaza, asigna, resuelve, carga, publica,
despacha ni ejecuta código.

`linkTranslationObjectV1` es la frontera independiente de enlace nativo. Vuelve
a auditar el descriptor de confianza, el objeto sin procesar y el grafo JITLink
antes y después del pruning, la asignación, la resolución de símbolos y los
fixups. Los símbolos runtime proceden únicamente del registro sellado. Una
credencial del dispatcher vincula la única entrada del manifest a su sesión,
identidad de block, PC de entrada guest, generación de caché y época de código;
la invocación también exige que el `RIP` guest del runtime coincida con esa
entrada. Tras finalizar correctamente, publica memoria ejecutable con sus
permisos definitivos. Unload revoca nuevas invocaciones y espera a una invocación
activa antes de liberar la asignación. El overload sin credencial sigue siendo
solo de auditoría y no puede invocar.

`NativeTranslationSessionV1` combina esas piezas en la frontera experimental de
ejecución C++ de x86-64 a AArch64 nativo. En un proceso ELF o Mach-O AArch64
little-endian conserva un único runtime de memoria guest comprobado y un estado
guest fijo entre múltiples blocks de un bucle de dispatcher
compile-link-validate-invoke-unload. Un salto directo canónico continúa en su
target estático exacto. Un branch canónico publicado de un solo flag solo continúa
en el sucesor taken o fallthrough declarado por el manifest del block; el
dispatcher rechaza cualquier otro PC seleccionado. Un retorno termina. Los
presupuestos globales de instrucciones, blocks y bytes de objeto generados se
mantienen exactos entre blocks. Cuando el guest se detiene correctamente, el
estado ejecutado y la memoria autoritativa se confirman juntos. La cancelación
se linealiza respecto a ese commit final.

Esta es una vertical slice ejecutable, no un traductor completo. Todavía no
cubre instrucciones ordinarias de memoria guest, registros parciales, flujo de
control condicional fuera del slice exacto schema-9 de Jcc tradicionales descrito
arriba —incluidos `JRCXZ`/`JECXZ`/`JCXZ` y `LOOP`/`LOOPE`/`LOOPNE`—, flujo de control
indirecto, calls, punto flotante, SIMD, x87, operaciones atómicas, instrucciones
de sistema, propagación general de excepciones, caché de blocks, otros pares
guest/host ni la dirección inversa de AArch64 a x86-64.
La sesión de ejecución aún no tiene superficies C, Python, CLI ni JSON; la
depuración permanece separada y sin soporte. Las API de objeto anteriores siguen
siendo útiles sin activar la ejecución nativa.

El contrato del IR generado exige que todo translated block sujeto a él sea
hidden y non-preemptible y use el C ABI `i32 (ptr state, ptr runtime)`. Los
blocks solo se descubren mediante un registro privado, nunca mediante la
búsqueda ambiental de símbolos del proceso; se prohíben las llamadas directas
entre blocks.

El IR verifier también limita el ancho de los enteros al ancho del registro
escalar del host para evitar compiler-runtime libcalls conocidos introducidos
durante legalization. Esta comprobación es necesaria, pero no suficiente:
cualquier backend de ejecución que implemente este contrato debe auditar de
forma exacta las transferencias de control post-codegen, `MachineIR` y las
relocations del objeto de destino frente a la misma runtime-symbol allowlist
finita.

Los loads y stores directos de TranslationIR, así como los valores de private
constants, solo pueden contener un entero escalar que no supere el ancho del
registro escalar del host. Los aggregates deben escalarizarse antes del límite
del verifier para que un IR compacto no provoque expansión ilimitada en el
backend.

La ABI de código generado solo está definida para enteros escalares. El punto
flotante, SIMD, x87, las operaciones atómicas y las instrucciones de sistema
quedan fuera de este contrato. Toda implementación que seleccione
`ProvenSemanticAndLLVM` debe ejecutar la simplificación semántica de NeverD,
condicionada por prueba, hasta un punto fijo conjunto con la optimización LLVM;
la política no proporciona un backend de traducción ejecutable.

## Fronteras de reescritura de excepciones

El compact unwind de Mach-O dispone de un parser estricto del `__unwind_info`
original, un parser consciente de fixups para los records
`__LD,__compact_unwind` generados, un merge exacto de rangos originales y
generados, un encoder determinista de páginas regulares y un instalador
transaccional de la sección final. El instalador solo reescribe in-place una
`__TEXT,__unwind_info` existente y file-backed cuando la tabla codificada cabe
en su capacidad declarada. Revalida la arquitectura, el layout y el byte
preimage, pone a cero la cola sin usar y vuelve a parsear el resultado para
probar su equivalencia semántica antes del único commit de la transacción Mach-O
externa. Si la sección final está ausente, no se instalan los records compact
generados y la transacción solo puede continuar mediante el cierre DWARF-FDE
exacto y autenticado descrito abajo; una sección final existente pero
insuficiente o malformada sigue fallando en modo fail-closed. Los records
generados se autentican mediante una asociación exacta, registrada
por el compilador, entre la función IR de origen y el owner symbol MC de destino
(incluidas las definiciones privadas, sin adivinar prefijos ni mangling), IDs de
rango opacos y distintos de cero y rangos de fragmento semiabiertos exactos.
Cada FDE generado debe coincidir exactamente con un único fragmento autenticado;
cada fragmento requerido debe coincidir con un único FDE instalado por esa
transacción, salvo que lo cubra un record compact no DWARF exacto y validado
estrictamente. Los fragmentos adyacentes o separados del mismo owner de función
pueden reutilizar una receta fuente; una identidad ausente, duplicada, colgante,
cross-owner o con límites incoherentes falla antes de modificar la salida. El
nuevo segmento RX solo se confirma tras demostrar un `__LINKEDIT` único y
terminal en archivo/VM, offsets desplazados con aritmética comprobada y una
revalidación estricta del layout final de archivo y memoria virtual.

Las referencias externas se clasifican con el contrato MC fixup completo. Las
llamadas solo pueden elegir targets callable autenticados; los campos
personality del compact unwind generado solo pueden elegir non-lazy pointer
slots validados, sin desreferenciar nunca su contenido en el archivo. TLS,
authenticated pointers, términos sustraídos, campos compact malformados y
formas de relocation desconocidas fallan en modo fail-closed.

En el compact unwind ARM32, el ajuste de stack codificado y el layout GPR son
`Complete`. Los selectores de pattern de registros D de 0 a 3 también son
`Complete`; de 4 a 7 son `Partial` porque el compact word por sí solo no demuestra
todos los slots relativos al CFA alineados en runtime. Una entrada `Partial` puede
conservar identidades de registro demostradas para análisis, pero toda ruta de
reescritura la rechaza fail-closed. Cada receipt de instalación EH-frame vincula
exactamente la arquitectura target, el ancho de pointer y el byte order; el
binding DWARF compact-unwind rechaza cualquier diferencia de target identity del
receipt. Aún falta una prueba nativa throw/catch sobre un binario enlazado.

La transacción de sección ARM32 de nivel superior es más limitada que el
decoder de compact unwind. Solo se habilita cuando el header Mach-O es
exactamente `CPU_SUBTYPE_ARM_V7K` y los bits `N_ARM_THUMB_DEF` de la symbol
table original autentican positivamente cada función requerida como código
Thumb. El triple exacto `thumbv7k-apple-watchos` y el modo Thumb permanecen
vinculados durante toda la generación de código, cuyos requisitos de features
de entrada no pueden superar el límite de Cortex-A7. Las funciones sin flag o
de modo desconocido, los subtipos genéricos que no sean v7k, el modo ARM, los
targets de código externo mixtos o desconocidos, el entry point in-place de
ARM Mach-O y el patch de ARM Mach-O desde código fuente C fallan en modo
fail-closed antes de modificar la salida. Los inputs stripped cuyas funciones
solo puedan descubrirse mediante `LC_FUNCTION_STARTS` aún no están soportados.

PE, ELF y Mach-O tienen componentes de excepción específicos de cada formato,
pero NeverD todavía no publica una pipeline de reescritura end-to-end para todos
los formatos y todos los tipos de excepción. Un encoding no admitido o los
requisitos de registro/layout no resueltos deben fallar antes de modificar la
salida; el soporte parcial existente no debe presentarse como cierre completo
de excepciones.

Reconocer una personalidad Itanium de Ada o D no es soporte de excepciones Ada
o D. Las LSDA en forma de dirección de GNAT, GDC, DMD y LDC son analizables; las
entradas de type-table permanecen opacas (`Exception_Id` / `Exception_Data` en
GNAT, `ClassInfo` en D) y nunca se siguen como `std::type_info`. La
reconstrucción nativa emite `personality` de LLVM más cláusulas
`invoke`/`landingpad` en forma de dirección. El estado corpus-proven es una
afirmación distinta y no se deduce del reconocimiento de personalidad ni del
lowering nativo.

## Mapa de componentes

Cada componente es un archivo estático creado por
`add_neverd_component_library`. La tabla enumera dependencias importantes de
NeverD, no todas las bibliotecas comunes LLVM y Capstone proporcionadas por el
helper de CMake.

| Directorio | Responsabilidad | Dependencias importantes |
|------------|-----------------|--------------------------|
| `lib/loader` | Detección de formato, carga PE/COFF, ELF y Mach-O, `BinaryImage` normalizada, descubrimiento de funciones | API LLVM Object |
| `lib/lift` | Semántica manuscrita de instrucciones x86/i386, AArch64 y ARM32 | Tipos de datos IR |
| `lib/decode` | Decodificación Capstone/native y despacho a lifters de arquitectura | `NeverDIR`, `NeverDLift` |
| `lib/ir` | Tipos comunes y definiciones/transformaciones LowIR, MedIR, HighIR e intrinsic | Sus cuatro subcomponentes IR |
| `lib/pipeline` | Detección de funciones y orquestación de rutas Low/Med/High/LLVM | IR, decode, lift, backend LLVM, debug, pases IR |
| `lib/backend/c` | Renderizado HighIR-a-C y LLVM-IR-a-C | IR |
| `lib/backend/llvm` | Lowering de MedIR a LLVM | IR |
| `lib/backend/codegen` | Generación de código objetivo y patch/reescritura in-place PE/ELF/Mach-O | IR, loader |
| `lib/sdk` | ABI C pública, ciclo de session, consultas, persistencia, plugins, entradas lift/decompile/patch | Agrega el motor en `libneverd` |
| `lib/pass` | Pases de ofuscación LLVM IR y ejecutor de pases MIR | IR |
| `lib/debug` | Contextos de depuración DWARF, PDB y linker-map | IR |
| `lib/sigs` | Análisis, bases de datos y coincidencia de firmas | Loader |
| `lib/libc` | Nombres libc conocidos y soporte del modelo de llamada | Componente independiente |
| `lib/support` | Helpers compartidos de carga binaria | Loader |
| `lib/translate` | Contratos versionados de estado/policy/exit guest, ABI runtime fija, memoria guest comprobada, auditorías de IR/objetos/LinkGraphs generados, enlace nativo sellado y dispatcher C++ experimental de x86-64 a AArch64 | Contratos IR, LLVM, LLVM Object y JITLink |

Los encabezados públicos reflejan estas áreas bajo `include/neverd`. Evite que
una clase C++ interna pase a formar parte del SDK por accidente: las operaciones
externas estables pertenecen al encabezado C puro y a uno de los archivos
específicos `lib/sdk/NeverDCAPI*.cpp`.

## Contrato de lifting estricto

`Decoder` y cada lifter de arquitectura arrancan en modo estricto. Si Capstone
puede decodificar una instrucción pero el lifter seleccionado no la implementa,
lanza `UnliftedInstruction`. La excepción registra dirección, mnemónico y
operandos; la semántica no soportada debe fallar de forma visible en vez de
omitirse o inferirse.

La ruta interna no estricta emite `NdOp::NOP`, pero es una salida de diagnóstico,
no una implementación aceptable. Las pruebas de contribuidores y CI deben
mantener el modo estricto. Cuando aparezca un fallo estricto:

1. Reprodúzcalo con la fixture específica de arquitectura más pequeña.
2. Añada la semántica ausente en `lib/lift/<ISA>`.
3. Compruebe la forma LowIR esperada en `unittests/lift`.
4. Añada un recorrido diferencial Unicorn en `unittests/semantic` si la instrucción tiene comportamiento observable.

No capture `UnliftedInstruction` solo para que el pipeline continúe. Una nueva
aproximación intencional necesita contrato y pruebas explícitos; no debe hacerse
pasar por lifting 1:1.

## Propiedad de formatos e ISA

La lógica del formato de entrada y la reescritura de salida están separadas de
forma deliberada:

| Formato | Carga, metadatos y relocations de entrada | Patch y relocations de salida |
|---------|------------------------------------------|-------------------------------|
| PE/COFF | `lib/loader/COFF` | `lib/backend/codegen/COFF` |
| ELF | `lib/loader/ELF` | `lib/backend/codegen/ELF` |
| Mach-O | `lib/loader/MachO` | `lib/backend/codegen/MachO` |

Los lifters de arquitectura están en `lib/lift/X86`, `lib/lift/AArch64` y
`lib/lift/ARM`. Las declaraciones públicas de lifter/register están en
`include/neverd/lift`. La emisión LLVM y generación de código específicas del
objetivo residen bajo `lib/backend/llvm/<ISA>` y
`lib/backend/codegen/CodeGen<ISA>.cpp`.

<a id="support-and-test-depth"></a>

### Soporte y profundidad de pruebas

La matriz de soporte de la raíz indica que cada celda está implementada. No
significa que cada opcode, caso límite ABI, productor binario o versión del
sistema operativo se haya probado de forma exhaustiva. El modo estricto falla
de forma cerrada cuando la semántica de una instrucción queda fuera de la
cobertura implementada por el lifter.

Las 12 celdas formato-por-arquitectura tienen cobertura semántica del backend
de reescritura en `unittests/semantic/PatchFullSubstRTTests.cpp`. La profundidad
de integración es más específica:

| Formato | x86-64 | i386 | AArch64 | ARM32 |
|---------|--------|------|---------|-------|
| PE/COFF | Fixture enlazada | Cuadrícula backend | Fixture enlazada | Fixture Thumb enlazada |
| ELF | Fixture enlazada + ida y vuelta semántica | Pipeline de objeto + ida y vuelta semántica | Fixture enlazada + ida y vuelta semántica | Fixture enlazada + ida y vuelta semántica |
| Mach-O | Fixture enlazada\* | Pipeline de objeto PIC/no-PIC\* | Fixture enlazada\* | Cuadrícula backend |

- Una **fixture enlazada** ejercita loader/pipeline y patch sobre un ejecutable
  enlazado para programas representativos.
- Un **pipeline de objeto** ejercita carga, todas las etapas IR y descompilación
  de un objeto reubicable, pero no enlazado del host ni ejecución del binario
  parcheado.
- Una **cuadrícula backend** compila IR representativo por la ruta exacta de
  generación para reescritura y compara el comportamiento en Unicorn; no
  ejercita el loader del formato sobre un ejecutable enlazado.
- `*` Las fixtures Mach-O enlazadas dependen de una toolchain del host capaz de
  producir el objetivo. macOS moderno no enlaza ejecutables i386 históricos;
  por ello se usan objetos thin PIC/no-PIC y la cuadrícula de reescritura.

Las celdas de fixture enlazada son la evidencia más fuerte de integración
del formato para esos programas. Las celdas de pipeline de objeto y cuadrícula
backend solo tienen cobertura de integración parcial. Ninguna celda está
«totalmente probada» sin esa precisión ni afirma cobertura exhaustiva del ISA.

La evidencia principal es
[`PatchFormatTests.cpp`](../unittests/lift/format/PatchFormatTests.cpp) para fixtures
ELF y PE enlazadas,
[`COFFARMFormatTests.cpp`](../unittests/lift/format/COFFARMFormatTests.cpp) para carga/
descompilación de Windows ARM,
[`MachOI386RelocationTests.cpp`](../unittests/lift/format/MachOI386RelocationTests.cpp)
para objetos thin i386,
[`X86_64_PipelineE2ETests.cpp`](../unittests/lift/x86_64/X86_64_PipelineE2ETests.cpp) y
[`AArch64_PipelineE2ETests.cpp`](../unittests/lift/aarch64/AArch64_PipelineE2ETests.cpp)
para Mach-O enlazado, y
[`PatchFullSubstRTTests.cpp`](../unittests/semantic/probe/patchfull/PatchFullSubstRTTests.cpp)
para la cuadrícula de 12 celdas. Consulte la [guía de pruebas](testing.es.md).

## Dónde editar

| Cambio | Punto de partida | Verificación mínima enfocada |
|--------|------------------|------------------------------|
| Añadir o corregir una instrucción | Archivos correspondientes en `lib/lift/X86`, `AArch64` o `ARM`; encabezado público si cambia el despacho | Prueba de arquitectura en `unittests/lift`; recorrido semántico en `unittests/semantic` |
| Añadir un `NdOp` | `include/neverd/ir/NdOps.h`, luego auditar Low-to-Med, emitters/renderers, verifier/emulator y volcados | `NeverDLiftTests` + casos pertinentes de `NeverDSemanticTests` |
| Cambiar CFG o descubrimiento de funciones | `lib/ir/low`, `lib/loader/FunctionDiscovery*.cpp`, `lib/pipeline/PipelineFuncDetect.cpp` | Pruebas CFG/jump-table de lift y suite de transformación semántica enfocada |
| Añadir relocation de entrada o regla unwind PE | `lib/loader/COFF` | `COFFARMFormatTests` o nueva fixture loader enfocada |
| Añadir relocation de salida o regla patch PE | `lib/backend/codegen/COFF` | `PatchFormatTests`, `RewriteCodegenRTTests` y cuadrícula backend PE |
| Cambiar comportamiento ELF o Mach-O | Directorios `lib/loader/<Format>` y/o `lib/backend/codegen/<Format>` correspondientes | Pruebas del formato más cuadrícula de reescritura |
| Cambiar recuperación MedIR/ABI | `lib/ir/med` | Pruebas lift de convención de llamada + recorridos semánticos entre ISA |
| Cambiar recuperación de control estructurado | `lib/ir/high` | `NeverDCFGLoopXformTests` y pruebas de C estructurado |
| Añadir transformación LLVM | `lib/pass/ir`, encabezado público en `include/neverd/pass/ir`, opción pipeline si se expone | Suite de transformación enfocada + `NeverDPatchFullTests` si cambia la salida patch |
| Añadir operación C API | `include/neverd/sdk/NeverDCAPI.h`, `lib/sdk/NeverDCAPI*.cpp` enfocado, `SessionImpl.h` solo para estado | Pruebas semánticas SDK/CLI; conservar `neverd_last_error` y convenciones de asignación |
| Añadir comando CLI | `tools/neverd/NeverDCLIOptions.cpp`, `NeverDCLI.h`, `NeverDCmd*.cpp` enfocado y despacho en `neverd.cpp` | `unittests/semantic/CLIEndToEndTests.cpp` y smoke test CLI directo |
| Añadir regresión semántica | `unittests/semantic/*Tests.cpp` enfocado; registrar archivo nuevo en `unittests/semantic/CMakeLists.txt` | Construir su binario de pruebas y seleccionar el caso con `ctest -R` |

Mantenga los cambios estrechos. Los archivos que definen una representación
pueden cambiar con sus transformaciones, pero loaders, lifters y backends no
relacionados no deben modificarse solo para uniformar un refactor amplio.
