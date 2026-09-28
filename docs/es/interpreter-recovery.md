**Idiomas**: [English](../interpreter-recovery.md) | [简体中文](../zh-CN/interpreter-recovery.md) | [繁體中文](../zh-TW/interpreter-recovery.md) | [日本語](../ja/interpreter-recovery.md) | [한국어](../ko/interpreter-recovery.md) | [Français](../fr/interpreter-recovery.md) | [Deutsch](../de/interpreter-recovery.md) | [Español](interpreter-recovery.md) | [Italiano](../it/interpreter-recovery.md) | [Русский](../ru/interpreter-recovery.md) | [العربية](../ar/interpreter-recovery.md)

# Recuperación de código fuente a partir de intérpretes

[← Índice de documentación](README.md)

La etapa experimental de especialización de intérpretes elimina el despacho
resuelto estáticamente de una función x64 ya enlazada, conservando sus entradas
en tiempo de ejecución, efectos de memoria, bifurcaciones y bucles. Utiliza la
semántica de las instrucciones, no firmas de manejadores ni la tabla de opcodes
de un protector concreto.

```sh
neverd decompile program --func vm_entry --devirtualize \
  --recovery-report recovery.json -o recovered.c
neverd decompile program --func vm_entry --devirtualize \
  --llvm -o recovered-llvm.c
```

`--vm-control` selecciona registros generales completos que distinguen los
contextos del intérprete. Puede repetirse y no proporciona valores concretos.
Por ejemplo, seleccione un cursor de bytecode cuyo valor establezca el código
de entrada. Una entrada de ejecución usada como contador debe seguir siendo
dinámica. La falta de separación entre contextos puede detener la recuperación
cuando confluyen valores distintos del cursor; el motor no debe compensarlo
adivinando un destino de despacho. Para un cursor guardado en la pila,
`--vm-control-stack=-16:8` selecciona ocho bytes en RSP de entrada menos 16.
El desplazamiento se refiere a la entrada de la función, no al puntero de pila
tras sus ajustes.

La API C es `neverd_devirtualize_source_v1()`, declarada en
`neverd/sdk/NeverDCAPIDevirtualize.h`. Ejecuta una transacción independiente sin
modificar la caché de descompilación habitual de la sesión. Un fallo no devuelve
código fuente, aunque puede devolver un diagnóstico JSON. Ambas cadenas
asignadas se liberan con `neverd_free_string()`.

## Descubrimiento automático del estado de control

La CLI y todas las versiones de las API C de recuperación de código activan el descubrimiento automático por defecto. La API C++ independiente del proveedor mantiene `SpecializationOptions::DiscoverControlState = false`; el llamador puede establecer `true`. Las pistas manuales `--vm-control` y `--vm-control-stack` siguen siendo claves de contexto opcionales. Los campos automáticos ordinarios conservan relaciones conjuntas de valores finitos y acotados, sin crear claves de contexto ni fijar entradas de ejecución a valores muestreados. Los contadores siguen siendo dinámicos salvo que el refinamiento selectivo de memoria descrito abajo necesite sus constantes demostradas.

El descubrimiento sigue las dependencias de control y direcciones sin resolver hasta las entradas estructuradas de registros al entrar en el nodo y el origen de creación de las entradas de memoria, incluidos rangos estrechos de bytes. Solo se propone una ranura del marco cuando ese origen identifica memoria no modificada desde la entrada al nodo en un rango exacto relativo al marco de entrada de la función. Las lecturas posteriores de valores iguales o reenviados desde una escritura no crean dependencias de entrada adicionales; los bytes desconocidos creados tras una invalidación de memoria no se consideran ranuras de entrada. Los registros históricos de lecturas siguen disponibles para otros análisis. Al encontrar dependencias ausentes, el análisis vuelve a empezar desde la entrada de la función. Cada relación conservada aún requiere una prueba completa del dominio finito de los bits que restringe; las escrituras con posibles alias siguen invalidando hechos de memoria. La memoria externa arbitraria y las relaciones de valores no acotadas no se vuelven finitas.

Las demandas a los productores conservan las máscaras de bits entre nodos sin ampliarlas a bytes completos; la aritmética sigue incluyendo de forma conservadora el prefijo de bits menos significativos que puede propagar acarreos o préstamos hacia un bit requerido.

Si dependencias de memoria ya rastreadas impiden repetidamente demostrar una dirección exacta, el refinamiento puede promover sus constantes de entrada demostradas a claves de contexto. No divide tuplas multivaluadas en nuevas aristas ni añade lecturas de memoria invitada para elegir el contexto: demostrar un dominio finito no garantiza que una lectura adicional sea segura. Los estados dinámicos o no acotados que dependen de memoria aún pueden detener la recuperación dentro de los límites configurados.

Este refinamiento selectivo también puede distinguir contextos mediante valores completos de registros de 64 bits o punteros guardados en la pila, si se demuestra que equivalen a la base del marco de entrada más un desplazamiento exacto. La clave conserva el desplazamiento, sin adivinar la dirección numérica de la invocación. Las escrituras parciales o con posible alias invalidan ese hecho. Las condiciones de rama pueden proponer dependencias para un refinamiento acotado antes de la prueba final de control, incluso cuando una unión imprecisa expone un sucesor no admitido. Una propuesta no demuestra que la condición sea falsa. La alcanzabilidad y los destinos conservados aún requieren pruebas completas; una operación no admitida que siga siendo alcanzable o un presupuesto necesario agotado impiden publicar código. Ante un sucesor no admitido, la búsqueda elige las condiciones predecesoras indecisas más cercanas; no realiza un análisis retrospectivo completo de dependencias.

Las demandas hacia los productores se identifican por la entrada del nodo nativo, el modo de instrucción, el tipo de campo y su intervalo de bytes. Solo una arista cuyo sucesor demande ese campo amplía sus dependencias de producción, incluso si su dominio finito sigue siendo impreciso. Esto puede reiniciar el análisis desde la entrada dentro de los presupuestos, sin asignar un único papel global a un registro físico reutilizado. La detección de dependencias es acotada e incompleta; no se garantiza recuperar todos los intérpretes sin indicaciones manuales.

La proyección de los campos automáticos ordinarios también se limita a las demandas del nodo de destino. Un campo no constante solo se incluye en la relación conjunta de valores finitos de una arista cuando su destino lo requiere. Los campos manuales y los campos promovidos automáticamente a claves de contexto siguen proyectándose globalmente. Los bytes constantes conocidos, los punteros exactos relativos al marco de entrada y los hechos de procedencia se conservan con independencia de las demandas. Esto evita que campos sin relación usados en distintas fases de los manejadores multipliquen las combinaciones de valores de la relación. Los presupuestos configurados y las pruebas exigidas para los destinos de control, las direcciones de memoria y el estado de retorno no cambian.

Para los campos automáticos ordinarios, cada columna de una tupla restringe solo los bits requeridos y lleva la máscara correspondiente. Las uniones conservan únicamente los bits restringidos en todas las rutas entrantes. Los demás bits del mismo byte siguen siendo valores de ejecución: el rango de almacenamiento de un campo no certifica un dominio finito completo para todo ese rango. Un byte solo se vuelve constante cuando se ha demostrado que sus ocho bits son constantes.

Los campos manuales y los promovidos a claves de contexto siguen intentando primero demostrar relaciones de ancho completo de forma global. Si la prueba no es concluyente, pueden conservar una relación solo para los bits requeridos; esto nunca aporta bytes sin demostrar a una clave de contexto ni elude un presupuesto global agotado.

Al reiniciar con un grafo nuevo, los rangos de almacenamiento automáticos ordinarios contenidos por completo en otros campos pueden compartir esos campos más amplios. Las ubicaciones de fase y las demandas de bits originales no cambian. Los campos manuales, los campos de contexto y los propuestos directamente por una dirección de memoria sin resolver conservan sus rangos exactos. `MaxControlFields` limita los campos realmente conservados tras esta normalización. `DiscoveredControlFields` sigue siendo acumulativo y puede superar el número activo; no se elevan el límite de campos ni los presupuestos globales de trabajo.

Un campo automático ordinario con un dominio finito completamente demostrado no añade inmediatamente todos sus productores como campos de control. El descubrimiento registra esas dependencias candidatas y solo las activa si la recuperación sigue bloqueada y el refinamiento inmediato no produce candidatos. Los campos promovidos a claves de contexto siguen ampliando sus productores de inmediato. Las visitas de descubrimiento, los reinicios y el trabajo de prueba conservan los presupuestos acumulados existentes.

Un dominio finito completo también puede demostrar que ciertos bytes son constantes aunque varíe la palabra entera. Por ejemplo, el dominio `{0, 0x100}` tiene un byte menos significativo constante. Solo se conservan como constantes los bytes iguales en todas las tuplas enumeradas, según el orden de bytes del destino; los demás siguen siendo dinámicos. Una enumeración parcial, un resultado desconocido del solver o el agotamiento del presupuesto de prueba no aportan hechos de este tipo.

Las pruebas de dominios finitos pueden reutilizarse dentro de una ejecución de recuperación, incluidos los reinicios de refinamiento. Una caché acotada compara el DAG ordenado completo de las expresiones salvo un renombrado coherente de variables libres; las variables compartidas, los anchos, los bits constantes, los parámetros de operadores y el límite de proyección siguen formando parte de la clave. Solo se guardan dominios completos y pruebas de que un dominio supera su límite. No se guardan resultados desconocidos o parciales; una ausencia en caché o falta de capacidad usa la prueba ordinaria. Todos los presupuestos globales siguen vigentes, y `solverQueries` cuenta las llamadas reales al solver.

Bajo el mismo predicado, el dominio completo de una sola columna variable y un valor único demostrado para cada una de las demás determinan la relación conjunta exacta, si se ha establecido la alcanzabilidad. Varias columnas variables siguen requiriendo una prueba conjunta. Una columna enmascarada que cubra todo su dominio de bits solo puede omitirse tras demostrar que sus bits de entrada son independientes del predicado y de todas las demás columnas. Las entradas compartidas, los resultados desconocidos o la enumeración parcial nunca justifican suponer un producto cartesiano.

Los valores predeterminados son `MaxControlFields = 16` para campos manuales y automáticos en conjunto, `MaxControlRefinements = 16` y `MaxDiscoveryVisits = 65536`. Los reinicios comparten presupuestos globales de nodos (incluidos los sintéticos), operaciones, evaluaciones, consultas y visitas de descubrimiento. `contexts` cuenta contextos nativos y, como `evaluatedOperations`, `nodeEvaluations` y `solverQueries`, acumula todos los intentos. Los contextos por dirección, slots activos de retorno nativo, campos y tuplas siguen siendo límites estructurales por intento. El límite de consultas permanece en 4096. El agotamiento no publica resultados parciales; `residualBlocks` describe solo el grafo residual final.

La opción CLI `--vm-max-refinements=N` exige un entero positivo y tiene el valor predeterminado 16. En C, el mismo límite se configura mediante `neverd_devirtualize_source_v2()` o `neverd_devirtualize_machine_source_v2()`: inicializar `neverd_devirtualize_options_v2` a cero, establecer `base.struct_size = sizeof(neverd_devirtualize_options_v2)` y después `max_control_refinements` (cero conserva el valor predeterminado 16). El miembro integrado `base` contiene las opciones v1; ambos miembros reserved deben permanecer a cero. Los diseños y puntos de entrada v1 existentes no cambian e ignoran las extensiones finales. Los demás presupuestos de trabajo y prueba siguen vigentes.

El informe JSON añade `discoverControlState`, `maxControlRefinements`, `maxDiscoveryVisits`, `discoveredControlFields`, `discoveredContextFields`, `controlRefinements` y `discoveryVisits` para registrar activación, límites y trabajo de análisis. Descubrir campos no demuestra por sí solo que la recuperación haya tenido éxito.

## Contrato de ejecución

El adaptador binario admite actualmente imágenes x64 ELF y PE enlazadas en sus
direcciones de mapeo. Los mapeos, bytes y permisos deben permanecer fijos, sin
modificaciones concurrentes. El lifting estricto bajo demanda sigue el código
máquina alcanzable. Las instrucciones no admitidas, llamadas, operaciones
opacas, accesos de memoria ordenados, control sin resolver y manejo de
excepciones del lenguaje detienen la recuperación.

La recuperación PE requiere todos los metadatos de la imagen, incluidas las reubicaciones globales y los registros de excepciones. La CLI los carga antes de aplicar `--func`; quienes usan la API C no deben restringir previamente la sesión mediante `neverd_session_restrict_function()`. El adaptador rechaza imágenes cargadas con un conjunto limitado de funciones, porque los metadatos omitidos no prueban la ausencia de correcciones ni de aristas de excepción.

Solo los rangos completos de solo lectura respaldados por el archivo, sin
mapeos superpuestos ni ajustes del cargador, pueden proporcionar lecturas
constantes de la imagen. Las tablas modificables, relocaciones sin resolver y
capturas puntuales de ejecución no demuestran la inmutabilidad. Las relocaciones COPY y los directorios de excepciones estructuralmente
incompletos se rechazan. Si el directorio PE y los rangos de funciones están
completos, un controlador desconocido en otra función no impide analizar la
entrada elegida; alcanzar el código que cubre detiene la recuperación. El dominio admitido exige retornos ABI ordinarios: el rango
de destino de cada escritura de origen externo debe ser disjunto del espacio
de la dirección de retorno al entrar. Es una precondición explícita del
llamador y del entorno, incluso para direcciones calculadas a partir de enteros
externos; la ausencia de procedencia del marco de pila no demuestra la
disjunción numérica. Las direcciones de escritura derivadas del marco deben
demostrar esa disjunción y el puntero de pila original debe restaurarse al
retornar. La información de origen se conserva al guardar valores en la pila
y en las uniones; perder una expresión afín no la convierte en un puntero
externo. Actualmente se rechazan los cambios de base de pila, los retornos que
retiran argumentos desde la función llamada y el despacho basado en RET.
El adaptador impone la semántica little-endian de x64.

Se producen código fuente e IR para análisis. Esto no demuestra la seguridad
de las relocaciones, el desenrollado de pila, las excepciones asíncronas ni la
sustitución binaria. El modo patch rechaza esta opción. No se afirma que todos
los intérpretes o configuraciones de protección estén admitidos.

Las formas x64 exactas de `PUSHFQ`/`POPFQ` permanecen en el programa residual. El análisis trata cada instantánea de los indicadores de la máquina como un valor desconocido en tiempo de ejecución; el levantador combina por separado los indicadores aritméticos modelados. Restaurar los indicadores sigue siendo un efecto de ejecución. Una dirección derivada de indicadores desconocidos no puede acogerse al contrato de no solapamiento del puntero externo con la dirección de retorno; un despacho derivado sin destinos acotados sigue fallando.

Antes de capturar todos los indicadores, cada indicador aritmético o de dirección modelado debe estar definido dentro de la función recuperada. Toda lectura directa de un indicador también requiere una definición en cada ruta predecesora alcanzable, aunque la simplificación simbólica anule su valor. De lo contrario, la recuperación se rechaza en lugar de emitir C con una trampa de «registro desconocido».

Los temporales de LowIR solo viven dentro de una instrucción nativa levantada. Cada byte leído debe haberse definido antes en esa misma instrucción; reutilizar una posición de una instrucción anterior o cancelar algebraicamente un valor indefinido no demuestra que el código fuente sea válido. Las constantes de entrada solo pueden vincular registros físicos.

## Límites actuales

Las direcciones de bytecode dependientes de la entrada y las relaciones entre estados del decodificador solo se admiten cuando los dominios finitos y las correlaciones necesarios pueden demostrarse dentro de los límites configurados. Esto no demuestra compatibilidad con todos los esquemas de decodificación indirecta. Se pueden recuperar ramas y bucles dinámicos si se demuestra cada destino de despacho; la cobertura de ramas ordinarias no basta para probarlo. El control sin resolver o el agotamiento de un presupuesto de prueba necesario son fallos y no publican código recuperado ni sustituciones parciales. Las llamadas auxiliares nativas, los límites de excepciones o reentrada, el código mutable y otras arquitecturas siguen fuera del contrato del adaptador.

## Implementación compartida

`SpecializationProvider` proporciona instrucciones íntegramente elevadas y
evidencias de lecturas inmutables. `NeverDInterpreterSpecialization` utiliza la
semántica existente de `SymExec` para evaluar parcialmente operaciones enteras
y de control. El adaptador binario se ocupa de los mapeos y la decodificación;
no implementa un segundo evaluador de instrucciones.

Para una dirección simbólica de lectura con dominio finito, el solver de vectores de bits integrado enumera las direcciones candidatas bajo las restricciones actuales. Solo se acepta el conjunto tras un resultado UNSAT final que demuestre que no existen otras direcciones, y cada dirección debe contar con un certificado completo de lectura inmutable que no produzca un fallo de memoria. Esa lectura certificada puede sustituirse en LowIR por una captura de la dirección y una cadena exacta de SELECT; las lecturas ordinarias no certificadas siguen siendo dinámicas. Una muestra de direcciones nunca sustituye al conjunto completo. Los registros de control y las posiciones del marco de entrada seleccionados pueden conservar tuplas conjuntas acotadas entre nodos, como la relación entre un cursor y su clave de decodificación. Las uniones y ampliaciones siguen siendo conservadoras. Los modelos SAT parciales o los resultados desconocidos no prueban la exhaustividad de direcciones ni destinos. Este mecanismo no necesita el backend Z3 opcional.

Cada nodo se identifica mediante su cursor nativo, modo de instrucción y las constantes seleccionadas de registros de control y espacios del marco de entrada. Los demás hechos a nivel de byte se combinan por intersección. Cuando se debilita un hecho entrante, se vuelve a evaluar el nodo. Así se conservan los bucles del programa, en lugar de expandir cada iteración observada. Todos los valores alcanzables de un destino indirecto deben pertenecer a un conjunto acotado cuya exhaustividad esté demostrada; los destinos seleccionados se convierten en comparaciones residuales explícitas y aristas del CFG.

Las operaciones dinámicas y las lecturas/escrituras ordinarias permanecen en
LowIR. Las constantes escalares, los punteros afines relativos al marco de entrada y
los bytes del marco cuya constancia se haya probado pueden pasar entre nodos;
las demás expresiones se descartan en lugar de expandirse sin límite. La
memoria del marco utiliza la invalidación conservadora de alias del estado
simbólico existente. Una escritura mediante un puntero desconocido que pueda
solaparse invalida los hechos en conflicto. No se presupone que un espacio de
pila sea privado ni se eliminan sus efectos mediante un contrato de ausencia
de alias sin demostrar.

Etiquetas sintéticas únicas de instrucciones distinguen los contextos clonados.
Los límites originales de las instrucciones se conservan en un mapa de origen
separado; los certificados originales de relocaciones, excepciones o tablas
de saltos no se copian a las nuevas apariciones. El LowIR recuperado pasa por
la conversión habitual de LowIR a MedIR antes de separar las rutas HighC y LLVM,
compartiendo el tratamiento de registros, pila, CFG, SSA y ABI. La ruta HighC
también exige que la verificación de MedIR termine correctamente.

Los presupuestos de nodos, contextos por dirección, operaciones, evaluaciones
de nodos y destinos finitos acotan el análisis. Si se agota un presupuesto o
aparece semántica no admitida, no se publica ninguna función residual. Un grafo
de control completo no equivale a una emisión de código fuente correcta;
la API pública comprueba ambos resultados e informa de la diferencia.

Los conjuntos finitos de direcciones de lectura, las tuplas conjuntas de control y el número de campos de control también tienen límites explícitos. Un límite global de consultas al solver y límites por consulta de puertas, conflictos, propagaciones y visitas a literales vigilados acotan el trabajo de prueba; el límite de nodos simbólicos acota el crecimiento de las expresiones. El informe JSON incluye esos presupuestos junto con `solverQueries` y `relationalWidenings`.

## Evidencias y pruebas

El informe JSON local opcional incluye el hash de entrada, controles elegidos,
presupuestos, estado, contadores de trabajo, número de bloques residuales,
ubicaciones originales de instrucciones y bytes inmutables usados durante la
recuperación. Contiene información derivada de la entrada y solo se escribe
en la ruta local solicitada.

Las pruebas públicas utilizan máquinas originales con despacho por registros
y por pila, cada una con programas aritméticos, bifurcaciones con unión y bucles
en tiempo de ejecución. Un oráculo independiente sin signo comprueba retornos,
escrituras de memoria, acarreos/préstamos y valores centinela de salida. El
HighC y LLVMC recuperado se compila con O0/O2 y trampas de comportamiento
indefinido, y se ejecuta frente al oráculo. Los casos negativos cubren el
despacho sin resolver o modificable, el orden de bytes incompatible, los
metadatos de excepciones y los presupuestos.

Otras fixtures originales de direcciones finitas usan registros de datos de solo lectura elegidos por la entrada, campos de control cursor/clave relacionados y un mismo handler en distintas posiciones virtuales. Cubren ramas con uniones y bucles cuya selección de registro depende del estado actual del programa. Su oráculo nativo independiente usa las convenciones SysV y Win64; ambas rutas C recuperadas se comprueban en O0/O2 con trampas de comportamiento indefinido y centinelas de salida. La falta de certificados de lectura o de presupuesto de prueba no debe publicar resultados parciales.

La matriz ampliada exige recuperar otras tres formas independientes sin indicaciones manuales de control: bytecode de punteros con encadenamiento directo, una pila software CALL/RET acotada con llamadas virtuales anidadas y un bucle con estado rotatorio de decodificación de opcodes. La ejecución nativa SysV/Win64 y HighC/LLVMC recuperado en O0/O2 deben coincidir con oráculos matemáticos independientes, incluidos centinelas de salida. Los cursores de retorno virtual desconocidos, las claves de decodificación sin restricciones y los presupuestos agotados no deben publicar código.

Otra matriz de estado de máquina exige que la ejecución nativa y ambas rutas C coincidan en O0/O2 para los 16 registros generales, los flags definidos y cada byte de la pila invitada comprobada. Los destinatarios de CALL indirectas por registro leen y sobrescriben el registro de destino; el despacho mediante retornos selecciona destinos conocidos. El oráculo comprueba la escritura real de la dirección siguiente, los flags sin cambios y RSP restaurado. Las direcciones esperadas se obtienen de símbolos. Los conjuntos desconocidos o inválidos y las llamadas indirectas por memoria deben fallar sin código fuente. Son requisitos de cobertura de fixtures originales, no garantías para cualquier máquina virtual o producto de protección. Estas fixtures originales han superado las comprobaciones indicadas localmente en x64 Linux.

Consulte [testing.md](testing.md) para los objetivos de prueba específicos.

## Recuperación con estado de máquina explícito

`--devirtualize --vm-machine-state` o `neverd_devirtualize_machine_source_v1()` selecciona una ABI separada: un puntero a 17 palabras `uint64_t` alineadas (RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8 a R15, RFLAGS). Solo el estado sin signo de 64 bits igual a cero indica éxito. Un estado distinto no revierte escrituras. El estado se captura antes de extraer la dirección del RET final. Su almacenamiento no debe solaparse con memoria invitada; se requieren los mapas originales y un anfitrión de 64 bits little-endian.

El perfil exige CPL3/IOPL0, pila sombra desactivada, ausencia de eventos asíncronos y ejecución normal sin fallos. Los flags de entrada deben ser canónicos con TF/RF/VM/AC/VIF/VIP a cero; POPFQ debe mantener TF/AC a cero, comprobado por el código generado. PUSHFQ/POPFQ usan el estado explícito; RDSSP conserva su destino e INCSSP alcanzado se rechaza. Se propagan punteros de marco guardados completos; escrituras parciales o posibles alias invalidan hechos. Los metadatos de excepción solo permiten el camino normal, sin equivalencia de despacho ni desenrollado. `sourceABI` y `executionProfile` registran el contrato; la ABI predeterminada conserva sus restricciones.

Esta ABI de estado de máquina admite CALL near directas y CALL near indirectas por registro con un conjunto finito de destinos demostrado exhaustivamente. La llamada indirecta captura el registro de destino original antes de modificar RSP y guarda la dirección real de la instrucción siguiente exactamente una vez. Las CALL indirectas por memoria siguen rechazándose, incluso mediante ranuras de puntero de solo lectura y `[rsp]`: la dirección de una ranura no es la del destinatario. Un RET near interno puede elegir entre un conjunto finito completamente demostrado. El código residual conserva una lectura de la pila invitada, captura su valor antes de incrementar el puntero y despacha según ese valor. Alcanzar la ranura de retorno de entrada conservada sigue siendo una salida exterior, incluso tras descartar marcos internos. Se rechazan destinos desconocidos, conjuntos con destinos ausentes o no ejecutables, retornos con limpieza adicional y cambios arbitrarios de pila.

La ABI de fuente ordinaria reconstruye un marco privado de la invocación. Todo rango LOAD/STORE de origen externo, incluidas las direcciones calculadas, debe ser disjunto del marco nativo privado y de su almacenamiento reconstruido en la fuente: es una precondición explícita. La prueba compartida rechaza direcciones de marco que escapan, resultados o ramas dependientes de ellas y lecturas privadas sin inicializar. La ABI con estado de máquina conserva las direcciones invitadas y no usa esta precondición del marco privado.

Si los indicadores indefinidos influyen en el control, las direcciones o las salidas definidas, se necesita una prueba independiente de no interferencia. El informe actual no aporta esa prueba ni certifica ese comportamiento dependiente del procesador.
