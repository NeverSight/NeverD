**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](../ja/ios.md) | [한국어](../ko/ios.md) | [Français](../fr/ios.md) | [Deutsch](../de/ios.md) | [Español](ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# Recuperación de código nativo y fuentes de iOS

[← Índice de documentación](README.md) · [Resumen móvil](../mobile.md)

`neverd mobile` acepta IPA, `.app` y Mach-O. Exporta C nativo, metadatos del runtime y fuentes experimentales Objective-C `.m` y Swift `.swift` para los cuerpos nativos compatibles. Un resultado publicado puede incluir métodos sin recuperar: consulte la cobertura antes de usarlo. Los contenedores móviles pertenecen a la CLI; el SDK C nativo carga por separado el Mach-O seleccionado.

La compilación elimina comentarios, formato, identificadores y estructuras del lenguaje. Este flujo reconstruye una representación fuente; no recupera el texto original ni certifica un comportamiento equivalente para aplicaciones arbitrarias. Tampoco ejecuta la aplicación analizada.

## Inicio y dependencias

```sh
cmake --build build --target neverd
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

Compile el objetivo `neverd` con una cadena compatible con C++20. El flujo móvil se ejecuta dentro de la CLI nativa sin intérprete de Python. El desmanglado de firmas Swift lo proporciona `LLVMSwiftDemangle` en el fork de LLVM de NeverD. Tanto las compilaciones desde sus fuentes como los paquetes LLVM publicados correspondientes incluyen este componente; NeverD no descarga las fuentes de Swift como dependencia adicional. Compilar y ejecutar NeverD no requiere instalar el compilador ni la cadena Swift. Siguen siendo necesarias bibliotecas nativas como LLVM y Capstone; distribuya las bibliotecas y avisos de licencia requeridos por su compilación. La compilación independiente de las fuentes Apple generadas y las pruebas de comportamiento Swift en macOS requieren, según corresponda, Apple Clang, el SDK y `swiftc`.

La recuperación de firmas Swift utiliza directamente los nodos estructurados de `LLVMSwiftDemangle` dentro del proceso C++. No busca ni inicia un ejecutable externo de desmanglado ni comandos para localizar una cadena de herramientas. La antigua opción de ruta del ejecutable se ha eliminado y ya no se lee la antigua variable de entorno del desmanglador. `--metadata-only` no ejecuta ni el exportador nativo de fuentes ni el desmanglado de firmas.

El inventario de firmas de `metadata/swift-signatures.json` identifica el componente integrado así:

```json
{
  "demangler": {
    "name": "llvm-swift-demangle",
    "execution": "builtin",
    "version": "6.3.3"
  }
}
```

```sh
neverd mobile App.ipa -o recovered-swift --timeout=600 --json
```

## Entradas y selección

Una IPA debe contener exactamente un `Payload/*.app` de primer nivel. En IPA y `.app`, `CFBundleExecutable` de `Info.plist` identifica el programa principal. En ambos formatos `--artifact` es relativo a ese bundle y selecciona un solo ejecutable integrado, sin analizar recursivamente todos los frameworks o extensiones. Una entrada Mach-O directa no acepta `--artifact`.

Para binarios fat, `--arch=auto` prioriza arm64, arm, x86_64 e i386. Una sección de arquitectura ausente o no compatible falla explícitamente. La proyección fuente se dirige actualmente a arm64/x86_64; seleccionar otra familia no implica compatibilidad Objective-C/Swift. Se rechaza una sección seleccionada con `cryptid != 0`: proporcione una entrada ya descifrada y legible. Archivos y directorios rechazan rutas peligrosas, enlaces simbólicos, archivos especiales y entradas en conflicto.

## Opciones y límites de recursos

| Opción | Predeterminado | Significado |
|--------|----------------|-------------|
| `-o DIRECTORY` | Obligatorio | Directorio nuevo fuera del directorio de entrada; se conserva la salida existente |
| `--platform=auto\|ios` | `auto` | Detectar la plataforma o elegir iOS |
| `--arch=auto\|arm64\|arm\|x86_64\|i386` | `auto` | Seleccionar una arquitectura Mach-O |
| `--artifact PATH` | Programa principal | Ruta ejecutable relativa a la aplicación |
| `--metadata-only` | Desactivado | Solo metadatos, sin recuperar fuentes ni invocar herramientas |
| `--max-func N` | `0` | Límite de funciones nativas; cero indica todas las descubiertas; ignorado en modo metadatos |
| `--timeout N` | `300` | Presupuesto total positivo de análisis en segundos; los procesos hijos usan el tiempo restante |
| `--max-files N` | `20000` | Presupuesto positivo de entradas; el inventario Swift también está limitado |
| `--max-bytes N` | `2147483648` | Presupuesto positivo de bytes para entrada, extracción y salida final |
| `--json` | Desactivado | Imprimir el informe versionado como JSON |

Se supervisa el área de trabajo, permitiendo hasta tres veces los presupuestos de entradas/bytes para copias y resultados intermedios. Los registros están limitados a 16 MiB por proceso y el JSON de firmas Swift a 32 MiB. Son controles de recursos, no aislamiento de procesos. Ampliar el tiempo no desactiva otros límites. Los métodos excluidos por `--max-func` siguen figurando como no recuperados si están en el inventario de metadatos.

## Fuentes Objective-C y estructura del runtime

Ambos modos de informe fuente incluyen `source_projection_graph`. Sus nodos muestran los cuerpos nativos tipados finales, diagnósticos locales, `dependencies` nativas y de Block combinadas, y el resultado de producción `closure_closed`. Las comprobaciones locales y los fallos propagados de dependencias tienen razones distintas; los cuerpos tipados ausentes conservan diagnósticos incompletos. Vincular una llamada pendiente puede revelar nuevas dependencias. Un nodo cerrado solo ha superado la fase de dependencias: cada método aún debe superar la emisión y las comprobaciones de texto antes de ser `recovered`. `native_dependency_graph` sigue siendo un inventario LowIR separado.

Para análisis repetidos de cobertura, la orden siguiente ejecuta las mismas comprobaciones de análisis y publicación que `--format=objc-methods`, pero omite `native_source` y el campo `source` de cada método. El JSON añade `sources_omitted=true` y conserva el significado completo de identidades, estados, diagnósticos, firmas, auxiliares compartidos y pruebas de dependencias. Las comprobaciones de renderizado siguen ejecutándose; el modo completo produce fuentes compilables. La entrada C correspondiente es `neverd_objc_methods_summary_json(session, max_functions)` y su resultado se libera con `neverd_free_string`.

```sh
neverd export WMF --format=objc-methods-summary -o summary.json
```

El cargador nativo vincula registro de método, dirección IMP ejecutable y codificación de tipo compatible con posiciones ABI fuente explícitas. Los parámetros fijos escalares/punteros conservan `self`/`_cmd`, argumentos sin usar, bancos enteros/flotantes separados y posiciones de pila compatibles. La reinterpretación de bits float/double se distingue de la conversión numérica. Las pistas de tipo sirven para proyectar fuentes; no son pruebas ABI autenticadas ni permiso para parchear código ejecutable.

`sources/objc.m` coloca las instrucciones realmente reconstruidas dentro de `@implementation` y conserva auxiliares C y llamadas tipadas necesarias. Los destinos requieren una vinculación fuente compatible; destinos desconocidos y grupos de dependencias incompletos quedan sin recuperar. Definiciones ausentes, direcciones no ejecutables, tipos contradictorios, ABI no compatibles, decodificación incompleta o IR rechazado no se convierten en métodos recuperados solo por tener declaración.

Los parámetros de funciones auxiliares nativas pueden adquirir tipos de puntero para la proyección de código cuando los valores de entrada completos llegan mediante COPY/PHI a argumentos de puntero ya vinculados, sin usos escalares contradictorios. Esta inferencia conserva las ubicaciones físicas de la ABI y los tipos del IR genérico; los cuerpos y todos sus grupos de dependencias nativas aún deben validarse.

Un auxiliar escalar nativo puede devolver un parámetro de entrada observado si su registro ABI exacto cubre todo el ancho del resultado. Una prueba acotada combina cada ruta de retorno con la entrada inicial y las aristas de vuelta de los bucles; las llamadas y escrituras parciales invalidan el valor. Las entradas no declaradas, los parámetros de relleno sin uso, las semillas SSA y los PHI no demuestran entradas. Esta inferencia solo para proyección fuente conserva las ubicaciones físicas y sigue exigiendo validar por completo el cuerpo y sus dependencias.

Las funciones nativas locales pueden exponer entradas enteras de ancho completo en registros auxiliares, incluidos punteros a resultados, cuando el análisis de entradas observables coincide con las lecturas del LowIR completo. Los registros guardados por el llamador pueden sobrescribirse después como temporales; los contextos preservados siguen exigiendo que no se escriban registros preservados ajenos al marco. Las definiciones implícitas de llamadas, incluso con versión SSA 0, no son entradas; solo los bytes preservados explícitamente se rastrean a través de llamadas. Los llamadores y las definiciones regenerados comparten las mismas posiciones físicas. No se deducen prototipos C/Swift externos y sigue siendo obligatoria la validación completa de cuerpos y dependencias.

Los tipos de callback C fijos conservan sus firmas de parámetros y retorno en las declaraciones y conversiones de código fuente. La importación exacta `swift_once` vincula un indicador, un callback `void (*)(void *)` y un contexto, sin resultado. Se conserva la llamada al runtime; esto no prueba la recuperación del cuerpo del callback ni la propiedad del almacenamiento compartido de inicialización. Las dependencias incompletas siguen sin recuperarse.

Las importaciones conocidas del runtime Objective-C conservan enlaces explícitos de argumentos y resultados: retain/release, liberación automática, referencias fuertes y débiles, asignación de objetos y setters de firma fija. Las variantes ARM64 específicas de un registro leen ese registro; la identidad importada y la ABI deben coincidir. Las llamadas para obtener, establecer y eliminar objetos asociados conservan objeto, clave, valor y política con anchura de puntero; el C generado usa la cabecera pública del runtime. Las pruebas de recompilación en macOS comparan vida útil, puesta a cero de referencias débiles, copia y eliminación con los métodos originales.

Las consultas optimizadas de clase y selector de libobjc conservan las llamadas reales del runtime, incluido el tratamiento de nil y las redefiniciones personalizadas. El resultado de un byte conserva las conversiones nativas del llamador; no se sustituye por una comprobación supuesta de la jerarquía de clases.

La exportación Objective-C también enlaza un conjunto fijo de importaciones del runtime Swift con ABI C ordinaria: recuento de referencias, referencias débiles nativas y de objetos de tipo desconocido, metadatos de objetos e inicio y fin de comprobaciones de acceso. El C generado conserva las llamadas y requiere el runtime Swift al enlazar. No se admiten entradas con registros especializados, convenciones de llamada Swift no reconocidas ni símbolos Swift arbitrarios; las identidades importadas y las ubicaciones escalares deben coincidir exactamente.

Las vinculaciones independientes admiten en arm64 y x86_64 las importaciones Darwin exactas String → NSString `_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF` y NSString opcional → String `_$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ`. Ambas preservan la propiedad y Clang `swiftcall`; el enlace requiere Swift Foundation y Swift Core. El puente inverso transporta las dos palabras String devueltas mediante un entero sin signo de 128 bits y las separa en registros de retorno explícitos antes de SSA. Es un transporte de bits, no un diseño String recuperado ni una ABI general de agregados. Las firmas desconocidas y las dependencias de inicialización incompletas siguen sin admitirse.

En los argumentos de clave verificados de las API de objetos asociados, las direcciones exactas de cadenas C Mach-O de solo lectura se reconstruyen como identidades compartidas. Una misma dirección original comparte clave; distintos desplazamientos interiores mantienen identidades distintas. La exportación mobile combina automáticamente las funciones auxiliares. La API C enumera sus nombres en `shared_identity_functions`; al enlazar las unidades de métodos debe conservarse una sola definición de cada función. Estas claves pertenecen al código reconstruido, no al almacenamiento de una imagen original ya cargada. Los demás usos de direcciones de imagen sin vincular siguen siendo una limitación.

Las importaciones verificadas de `os_unfair_lock_lock`, `os_unfair_lock_unlock`, `os_unfair_lock_trylock`, `os_unfair_lock_assert_owner`, `os_unfair_lock_assert_not_owner` llaman a la implementación real de Darwin mediante `<os/lock.h>`, preservando la dirección del bloqueo, las comprobaciones de propiedad y el resultado booleano. Las variantes desconocidas siguen sin admitirse. Los resultados enteros conservan solo los bits declarados por la ABI: Darwin arm64 amplía los resultados de 8/16 bits a 32 bits según su signo; el resto del registro permanece desconocido. Los anchos de retorno declarados entran en SSA antes de fusionar ramas, para que los bits altos no utilizados no oculten los bytes bajos válidos.

Los registros Darwin `__cfstring` verificados se reconstruyen como objetos constantes cuando la importación de clase, la disposición, los caracteres y los fixups están completos. Se conservan los bytes ASCII y las unidades UTF-16, incluidos los NUL internos. Las mismas direcciones originales comparten un objeto reconstruido; los registros distintos permanecen separados. Los auxiliares figuran en `shared_identity_functions` y requieren Foundation al enlazar. Esto no permite accesos directos a memoria no verificados sobre los objetos constantes.

Los contadores numéricos de una sección `__DATA,__llvm_prf_cnts` con límites verificados y sin punteros pueden usar almacenamiento reconstruido compartido. Se conservan los bytes iniciales, las lecturas y escrituras superpuestas de 1–16 bytes y sus actualizaciones entre archivos de métodos. La exportación mobile combina las funciones auxiliares; los usuarios de la API C deben enlazar una sola definición de cada función de `shared_storage_functions`. Este almacenamiento es independiente de la imagen original y su entorno de perfilado. No se admiten direcciones que escapan, accesos ordenados, mapeos incompletos ni reubicaciones de punteros. Los callbacks block instrumentados pueden actualizar este almacenamiento de la imagen sin exponer direcciones privadas; las escrituras normales de contadores se distinguen de las escrituras en el marco de pila privado.

Los metadatos conservan superclase, inicio/tamaño de instancia e ivars escalares/punteros con desplazamientos, anchuras y alineaciones comprobados. Las declaraciones añaden relleno cuando hace falta. Un método que necesita una disposición desconocida queda sin recuperar. Las categorías mantienen identidades clase/categoría/dirección e implementaciones separadas; registros exactamente iguales repetidos en ambos inventarios cuentan una vez. Las categorías externas compatibles usan declaraciones Foundation existentes. Los encabezados externos desconocidos se señalan como dependencias ausentes; no se inventan clases sustitutas.

La recuperación también exige declaraciones de clase completas, definiciones de las clases antecesoras locales y un fuente donde los valores estén definidos antes de usarse en todos los caminos y las salidas alcanzables tengan el retorno requerido; una superclase local vacía solo recibe una `@implementation` vacía si su disposición verificada y un inventario completo demuestran que carece de métodos ordinarios propios. Si faltan esas pruebas o un nombre entra en conflicto con una importación Foundation conocida, los métodos afectados permanecen en el denominador de cobertura con sus motivos, mientras se siguen emitiendo las clases recuperables de forma independiente; estas comprobaciones de nombres no abarcan todos los nombres de SDK, SDK de iOS ni versiones.

Las llamadas a Blocks Objective-C compatibles requieren una ABI escalar fija completa, con el objeto Block implícito y todas las ubicaciones de argumentos y retorno. La codificación de ejecución `@?` se amplía a `id` solo en las declaraciones; no proporciona el prototipo de invocación. Las referencias a Blocks globales conservan la identidad del objeto compartido. Las capturas escalares síncronas compatibles exigen demostrar el almacenamiento nativo de las capturas y el flujo de invocación. Las capturas fuertes copiadas mediante una llamada verificada a `objc_retainBlock` / `_Block_copy` pueden escapar si la construcción inicializa su almacenamiento en cada ruta entrante y se recuperan por completo los cuerpos de invocación, copia y destrucción. El descriptor generado conserva las ABI auxiliares y la disposición de propiedad originales. Las referencias débiles/byref, las disposiciones desconocidas y los consumidores no demostrados siguen sin recuperarse. También se admiten parámetros block de C declarados sin escape por el compilador si coinciden la importación exacta, la posición del parámetro y la ABI completa del callback. Este contrato de vida útil no implica memoria de solo lectura. Al enlazar unidades de código de la API C, cada entrada de `shared_block_functions` debe tener una sola definición. La exportación mobile combina las definiciones coincidentes y rechaza conflictos, incluidas diferencias en las funciones privadas llamadas.

Las declaraciones de métodos de protocolo se leen de registros locales resueltos del runtime, incluidos los protocolos heredados y los métodos obligatorios u opcionales, de instancia o de clase. Las listas ordinarias y relativas comparten el decodificador. Todas las declaraciones coincidentes de clases y protocolos deben concordar antes de asignar una firma fija al selector; los registros malformados y los ciclos de herencia no aportan tipos. Los punteros válidos a estructuras, uniones y matrices usan representaciones opacas sin inferir su disposición. `objc_metadata.protocols` expone las declaraciones por separado: no cuentan como implementaciones recuperadas ni demuestran conformidad de clases. Si el resto de la firma es idéntico, los resultados enteros de 64 bits con y sin signo comparten una representación binaria sin signo; los conflictos de enteros más estrechos, flotantes, punteros o argumentos siguen rechazándose.

Las llamadas de formato NSString declaradas por el compilador pueden recuperar argumentos escalares promovidos desde un objeto de formato constante verificado. Los argumentos secuenciales y posicionales deben estar completos y tener tipos compatibles. Darwin arm64 lee los argumentos variables en posiciones de pila de ocho bytes; x86_64 usa registros enteros, flotantes y la pila. Las llamadas generadas conservan la elipsis y el despacho dinámico. Se rechazan formatos desconocidos, escrituras de recuento, long double y extensiones no admitidas. El mismo análisis admite importaciones C declaradas como `NSLog`, verifica la biblioteca exportadora exacta y comparte el prototipo variádico entre llamadas con distintas cantidades de argumentos.

Las escrituras en el marco de pila privado conservan cada byte leído en cualquier parte de la función. Con límites del marco, alias inmutables de entrada y ausencia de escape de direcciones demostrados, NeverD puede eliminar escrituras no leídas o acortar el final no leído de una escritura entera. Así se recuperan argumentos escalares promovidos cuando solo su relleno sin usar es desconocido. No se inventan bits desconocidos. Los accesos ordenados, llamadas sin vincular, direcciones ambiguas, valores con efectos y presupuestos de análisis agotados conservan las escrituras originales.

Los métodos de categoría en conflicto conservan las declaraciones ABI validadas aunque se desconozca el orden de sustitución. Una llamada dinámica solo puede usar ese ABI si todas las declaraciones coinciden; las implementaciones ambiguas siguen excluidas de la selección del cuerpo fuente. Las declaraciones incompatibles o mal formadas siguen bloqueando la llamada.

Las llamadas conocidas a `objc_enumerationMutation` conservan el argumento objeto y la continuación, porque un manejador de mutaciones instalado puede retornar. Las importaciones exactas de Darwin de `__stack_chk_guard` vinculan la identidad del objeto del entorno de ejecución; las lecturas, comparaciones y llamadas a `__stack_chk_fail` siguen siendo observables en el código recuperado.

Las imágenes Darwin enlazadas de 64 bits que importan Foundation del sistema también consultan declaraciones integradas extraídas por el compilador. Todas las declaraciones del runtime y del framework para un selector deben ser compatibles; las firmas variádicas, los agregados no admitidos y las diferencias entre plataformas quedan sin enlazar. El catálogo aporta tipos de llamadas, sin determinar la clase del receptor ni generar cuerpos de funciones. Usar NeverD no requiere un SDK de Apple local. CoreData dispone de un catálogo independiente activado por su dependencia exacta del sistema. Las declaraciones pertenecen al framework de su cabecera pública; las cabeceras de dependencias incluidas indirectamente no activan otro framework.

El catálogo de declaraciones C también cubre las exportaciones de CoreGraphics e ImageIO. Los punteros opacos de imagen y color, los recuentos enteros y los resultados flotantes conservan su ABI declarado. Los alias de frameworks públicos del sistema se generan junto con los datos de exportación; las rutas privadas, otras versiones y los símbolos no declarados no obtienen vinculación. Las declaraciones mobile usan también la gramática de tipos del cargador: los punteros a agregados válidos permanecen opacos sin suponer su disposición.

Los literales Swift grandes e inmortales pueden vincular sus bytes UTF-8 a almacenamiento estático compartido en un puente Foundation verificado. Se comprueban conjuntamente longitud, indicadores, terminación, UTF-8 válido, inmutabilidad e importación exacta. Se conservan la representación etiquetada y el puente originales; los ceros internos y otras formas de almacenamiento siguen sin vincularse.

Los objetos NSString constantes validados conservan su identidad compartida al asignarse o almacenarse mediante enteros con procedencia de dirección de datos completa. Los valores escalares, las direcciones incompletas, las operaciones numéricas y los accesos a bytes internos del objeto no reciben este enlace.

Las cargas escalares ordinarias de bytes de la imagen cuya inmutabilidad y ausencia de reubicación se han probado pueden convertirse en constantes que conservan sus bits. Se admiten enteros de 1, 2, 4 y 8 bytes y valores flotantes de 4 y 8 bytes. El almacenamiento modificable o ambiguo, las cargas ordenadas y los usos como dirección siguen sin vincularse; un uso numérico no autoriza usos como puntero de la misma expresión.

Las llamadas C con parámetros fijos también usan declaraciones extraídas por el compilador y datos de exportación y reexportación del SDK. Deben coincidir la biblioteca dyld exacta, el símbolo y la ABI escalar; las importaciones débiles, proveedores desconocidos y prototipos no admitidos quedan sin vincular. El C generado usa identificadores separados enlazados con los símbolos originales. Las llamadas de sincronización ordinarias sin tablas de excepciones conservan las llamadas reales y sus efectos en memoria.

Las lecturas escalares indexadas pueden usar una tabla de bytes inmutables cuando el análisis compartido del flujo fuente demuestra una cota superior sin signo en cada ruta entrante. Se conservan guardas, máscaras, anchos enteros nativos y desbordamiento modular; las escrituras y direcciones locales que escapan invalidan los hechos anteriores. Cada tabla admite hasta 4.096 entradas y 65.536 bytes. Al publicar se vuelven a comprobar cotas, almacenamiento, reubicaciones y todos los usos auxiliares. Los bits de ancho completo de puntero que designan una sección de la imagen siguen siendo ambiguos; los fragmentos escalares más estrechos y el relleno de segmentos no prueban identidad de puntero. Los bytes copiados solo permiten lecturas escalares y la dirección de la tabla no puede escapar. Las regresiones ejecutables comparan bits enteros, bits flotantes incluido el cero negativo y comportamiento fuera de rango con los métodos originales.

Los datos externos requieren declaraciones SDK comunes sin TLS y pruebas exactas de exportación de la biblioteca. El C generado referencia el almacenamiento real del símbolo y conserva los accesos de memoria posteriores, incluida la diferencia entre un puntero global y su destino. Las importaciones débiles, identidades contradictorias y tipos de almacenamiento no admitidos quedan sin vincular. Las declaraciones de datos no demuestran la construcción ni la propiedad de los bloques. El catálogo incluye datos de CoreData, CoreImage, CoreGraphics, ImageIO y CoreSpotlight. El almacenamiento de literales integrados se obtiene compilando colecciones vacías y objetos booleanos para cada destino; solo se aceptan direcciones directas de datos externos sin TLS, con las mismas comprobaciones de exportación. La generación requiere también el compilador Clang mediante `--clang`, además de libclang.

La reconstrucción del runtime es limitada. No se prometen propiedades y protocolos completos, anotaciones originales de propiedad, agregados arbitrarios, colas variádicas, cuerpos dependientes de excepciones ni disposiciones Block/capturas no modeladas. La codificación solo describe argumentos fijos y no demuestra que la declaración original careciera de puntos suspensivos. Los punteros encadenados se utilizan solo en posiciones resueltas por el cargador; los formatos no resueltos conservan diagnósticos.

## Fuentes Swift y almacenamiento

La salida estructurada del demangler separa firmas invocables y metadatos no invocables. Antes de proyectar el cuerpo nativo, las firmas compatibles se vinculan a símbolos, entradas y ABI de máquina explícita del binario seleccionado. Los receptores Swift siguen su ABI, sin sustituirlos por argumentos ocultos Objective-C. Un archivo de firmas proporcionado por el usuario sigue siendo una pista que debe validarse.

El emisor experimental construye funciones libres, métodos de clase, inicializadores designados y métodos de struct de disposición fija compatibles, incluidas ciertas formas de receptor mutating. Las declaraciones y campos almacenados necesitan metadatos de disposición recuperados. Las llamadas nativas solo se emiten si declaraciones y cuerpos necesarios forman un grupo completo de dependencias compatibles. Las unidades fuente reúnen declaraciones y métodos, sin puentes que llamen al binario original.

Los cuerpos de getter/setter Swift compatibles proceden de la implementación nativa y se ensamblan en propiedades. El almacenamiento privado de respaldo conserva la disposición de campos establecida; los inicializadores y otros métodos usan los mismos nombres de almacenamiento. Una declaración de propiedad o un registro de campo no basta para demostrar que se recuperó el cuerpo de un accesor.

Los constructores con asignación, destructores/liberaciones triviales, accesores de metadatos de tipo y entradas `_modify`/resume compatibles pueden proyectarse en una unidad de tipo emitida. Cada entrada requiere una prueba acotada del flujo nativo completo y sus efectos, dependencias de contexto/inicializador/propiedad realmente recuperadas y auditorías de manejo de excepciones e IR de los cuerpos relacionados. Las escrituras del asignador deben coincidir con el inicializador real; `_modify` debe vincular el campo mutable exacto y la continuación. Las llamadas de metadatos en ejecución conservan su semántica modelada dentro del tipo recuperado. Estas entradas se indican expresamente como proyecciones de fuente del compilador; no constituyen cuerpos de métodos ordinarios recuperados por separado ni el texto fuente original.

Disposiciones genéricas o resilient, funciones async/throwing, convenciones desconocidas, accessor/allocator/thunk no compatibles, inicialización incompleta y dependencias nativas/runtime sin vincular permanecen individualmente `unrecovered`. Un símbolo mangled o nombre de tipo nominal no equivale a un método recuperado. Símbolos eliminados y nodos del demangler sin clasificar hacen que la cobertura sea incompleta o desconocida.

## Salida y cobertura

```text
recovered-ios/
  artifacts/selected.macho
  sources/native.c
  sources/objc.m
  sources/swift.swift
  metadata/objc.h
  metadata/objc.json
  metadata/objc-methods.json
  metadata/swift.json
  metadata/swift-signatures.json
  metadata/swift-methods.json
  logs/
  report.json
```

Los archivos del lenguaje fuente existen solo cuando se puede emitir código. `objc.json` guarda clases, categorías, ivars y codificaciones originales; `objc.h` contiene declaraciones compatibles. `swift.json` contiene tipos nominales y símbolos mangled. Los JSON de firmas/métodos conservan clasificación, omisiones, razones y conteos. Los registros contienen diagnósticos nativos y del exportador Swift nativo cuando se ejecuta. No se generan registros de búsqueda de cadenas Swift externas ni de desmanglado externo. Las rutas de `report.json` son relativas a su directorio. El binario seleccionado es un artefacto de análisis; el código generado no lo enlaza como puente de recuperación.

Se eliminan copias temporales del paquete y JSON intermedios. Sin cuerpos nativos, una ejecución normal falla aunque haya metadatos. El modo metadatos solo produce el artefacto seleccionado, `objc.h`, `objc.json`, `swift.json`, `report.json`; no hay fuentes ni archivos de firmas/cobertura, y `native_function_count`, `objc_method_recovery`, `swift_method_recovery` son `null`. Todos los modos usan los metadatos Objective-C resueltos por el cargador nativo. Los metadatos Swift se leen de la imagen nativa con límites comprobados; los ajustes, disposiciones reubicables o referencias no compatibles conservan diagnósticos de resultados parciales.

Este informe ilustrativo abreviado muestra deliberadamente recuperación parcial:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "ios",
  "architecture": "arm64",
  "objc_method_recovery": {
    "status": "partial",
    "method_count": 3,
    "recovered_method_count": 2,
    "unrecovered_method_count": 1
  },
  "swift_method_recovery": {
    "status": "partial",
    "coverage_status": "partial",
    "method_count": 4,
    "recovered_method_count": 2,
    "source_body_method_count": 1,
    "compiler_projection_method_count": 1,
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

El `status: "success"` exterior indica que se publicó una salida validada. `recovered`, `partial`, `unrecovered`, `no-methods` describen el inventario descubierto, no equivalencia semántica ni integridad del programa original. Cada método no recuperado tiene una razón. Objective-C `recovered` exige también metadatos runtime completos. Un inventario vacío no demuestra que no existieran métodos.

Una fila Objective-C no recuperada puede incluir `native_backend: {status, reason, diagnostics}` si un único resultado del backend coincide exactamente con su identidad runtime. Este resumen opcional y limitado en tamaño conserva el resultado intermedio aunque falle una comprobación de declaraciones o disposición de memoria. El estado, el motivo y los recuentos principales de la fila siguen siendo definitivos; la ausencia del resumen indica que la evidencia no estaba disponible.

Swift `coverage_status` cuenta solo elementos invocables clasificados. El `status` Swift global contempla símbolos desconocidos y puede ser `unclassified`, `unsupported-architecture` o `no-symbols`. Los metadatos no invocables aparecen en `non_method_symbols` como `not-callable`; los desconocidos, como `unclassified`. `types`, `type_metadata_count`, `source_type_count` cuentan por separado tipos y unidades emitidas, sin inflar la cantidad de métodos.

Cada fila Swift recuperada indica `source_representation` como `native-method-body` o `compiler-generated-from-type`. Las proyecciones del compilador también conservan `compiler_projection_kind` y `compiler_projection_evidence`. `source_body_method_count` cuenta los cuerpos de métodos nativos recuperados; `compiler_projection_method_count` cuenta las proyecciones del compilador demostradas. Su suma es `recovered_method_count`. Las entradas del compilador permanecen en el denominador `method_count` y mantienen su identidad exacta en una única unidad fuente `type` correspondiente. Los metadatos de tipo o el nombre de una dependencia por sí solos no aumentan la cobertura recuperada. El JSON nativo por lotes incluye `source` en las filas del compilador y las unidades de tipo; los `source_units` del informe mobile conservan solo descripciones sin `source`, y el fuente completo está en `sources/swift.swift`.

El lote Swift describe `source_units` con `{kind, module, name, source, method_entries, method_identities}`; kind es `function` o `type`, y cada identidad `{entry, mangled_symbol}`. Símbolos distintos pueden compartir entrada y mantener proyecciones ABI diferentes. Cada identidad recuperada debe aparecer exactamente una vez y ninguna no recuperada puede figurar. `method_entries` debe coincidir con la proyección ordenada de entradas de `method_identities`, incluidas direcciones repetidas. No se deben fusionar silenciosamente identidades idénticas duplicadas. `source` concatena en orden la fuente de cada unidad más un salto de línea. Mobile guarda el agregado en `sources/swift.swift` y las descripciones en el JSON de cobertura. El `source` individual sirve para inspección; concatenarlo no reconstruye correctamente clases.

## Exportación nativa directa y SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

La exportación Swift consume el inventario estructurado de firmas que produce el analizador integrado durante una ejecución mobile normal. El lote Objective-C incluye `native_source`, `native_function_count`, `objc_metadata` y, por método, fuente C, nombre, tipo de retorno y parámetros. Mobile comprueba además declaraciones, cuerpos y disposiciones antes de `.m`, por lo que su cobertura puede ser menor que la del lote C. Una exportación nativa exitosa puede no contener métodos recuperados.

Cada método incluye `projection_diagnostics`: sus `items` registran obstáculos comprobables independientemente con `code`, `reason` y pruebas disponibles de sentencia, llamada, valor o dependencia. `checks_complete: false` indica requisitos ausentes o límites de recursos; un informe parcial vacío no demuestra recuperación. Las comprobaciones se comparten con la admisión y mantienen `status`, `reason` y `unbound_call`.

El `native_dependency_graph` recorre el LowIR final desde firmas admitidas siguiendo llamadas directas al código de la imagen. Conserva direcciones de llamador, bloque e instrucción, destinos compartidos y ciclos; los destinos indirectos son null. `inventory_complete` es false si falta una función LowIR necesaria o se alcanza el límite de registros. `targets_complete` exige además destinos directos para todas las llamadas registradas. Se excluyen raíces no admitidas y destinos indirectos sin resolver; no demuestra cobertura completa de ejecución nativa ni recuperación de fuentes dependientes.

Las referencias locales a protocolos validadas usan `objc_getProtocol` para conservar la identidad registrada. El inventario de dependencias `runtime_protocols` incluye las funciones nativas llamadas. Los nombres conflictivos, las declaraciones incompletas, los slots importados y las reubicaciones sin resolver no se vinculan. La exportación independiente informa de la falta de registro en vez de emitir un cuerpo que podría recibir un protocolo nulo.

El catálogo ABI del runtime Swift se genera desde declaraciones upstream fijadas y conserva la convención C o Swift. Los punteros conocidos y enteros sin signo del ancho de un puntero forman firmas escalares fijas. Las llamadas Swift requieren una importación fuerte exacta de libswiftCore; las dos clases admitidas de disponibilidad versionada de metadatos tampoco permiten importaciones débiles. Se excluyen registros especiales, representaciones desconocidas, disponibilidades no admitidas y conflictos. El compilador añade a swift_willThrow atributos swiftself/swifterror ausentes del DSL. Se conservan efectos y contratos existentes de callbacks y bytes. Generar con `scripts/generate_swift_runtime_declarations.py --input RuntimeFunctions.def --source-sha256 <pinned-hash>`; `--check` verifica el catálogo. Los hechos incluyen revisión, hash y avisos de terceros.

Las declaraciones fijas del runtime Swift también conservan resultados de dos palabras: dos punteros, o el puntero de metadatos y la palabra de estado declarados. La capa ABI común asigna ambos registros de resultado; la validación del código vuelve a comprobar orden, tipos e importación exacta. La asignación de cajas y las consultas de metadatos ejecutan llamadas reales al runtime; siguen sin admitirse parámetros agregados, disposiciones desconocidas y contextos ocultos.

Por separado, los puentes exactos de valores Foundation observados por el compilador para URLRequest, Notification, URL, Data, Date e IndexPath conservan `swiftcall` y la identidad del proveedor. Los resultados indirectos y el contexto/self usan `swift_indirect_result` y `swift_context` en sus registros dedicados (x8/x20 en arm64 y RAX/R13 en x86_64) sin consumir el banco normal de argumentos enteros. Data conserva su transporte explícito de dos palabras. Son declaraciones de portadores de llamada, no disposiciones de valores recuperadas ni una ABI Swift general.

La inferencia de retornos escalares nativos reconoce la extracción completa e inmediata de ambas palabras de una llamada con ABI de resultado validada. Deben coincidir la identidad temporal, los desplazamientos, los anchos y los registros físicos. Esto permite que una función auxiliar devuelva directamente el primer miembro, manteniendo las reglas de vistas de registros, las sobrescrituras por llamadas y el requisito de un resultado definido en cada ruta de retorno.

El catálogo C fijo también conserva valores enteros de 32 bits y resultados booleanos con extensión explícita por ceros. Cada resultado booleano ocupa un byte completo sin signo; esto no establece reglas de extensión para parámetros booleanos. Se siguen excluyendo las representaciones más estrechas desconocidas y las declaraciones de 32 bits con convención Swift. Las pruebas ejecutadas cubren conversiones correctas y fallidas, retenciones contadas y destrucción de objetos, conservando las llamadas y sus efectos de propiedad.

Los metadatos de campos distinguen la disposición de bytes conocida de los tipos de lenguaje desconocidos. Las clases Swift con ABI estable usan variables de desplazamiento del tamaño de un puntero incluso en arm64; los campos no expuestos pueden tener una codificación de tipo Objective-C vacía. NeverD conserva esa ausencia y valida desplazamientos, tamaños, alineación y solapamientos. Los cuerpos C recuperados obtienen los desplazamientos mediante consultas de ivars en ejecución. Un tipo desconocido sigue impidiendo declaraciones de clase que requieran inventar un tipo.

La identidad de los ivars Swift con ABI estable se conserva aunque sus posiciones se inicialicen en almacenamiento de relleno cero durante la ejecución. Estas clases usan `ivar_status: "runtime"`; los offsets desconocidos son JSON `null` y el tamaño de campo 0 indica una anchura determinada en ejecución. Los métodos pueden consultar la clase existente y los tipos de objeto declarados pueden seguir slots exactos. Los offsets literales requieren una disposición conocida. Esto no reconstruye la disposición de una clase Swift: la exportación independiente sigue rechazando disposiciones y tipos de campo desconocidos.

En llamadas nativas vinculadas directamente, la dirección de un desplazamiento ivar puede sustituirse por la de un escalar local solo si la función llamada lee el puntero exactamente una vez al entrar, antes de efectos observables, y no lo escribe, retiene, compara ni usa de otra forma. El valor local procede de la consulta ivar existente en tiempo de ejecución; se conservan la ABI y el cuerpo de la función. Esta prueba acotada acepta flujo de control lineal y rechaza usos inciertos o límites de análisis agotados. La C API recupera `.cxx_destruct`; la sintaxis de métodos Objective-C independientes aún no puede emitir ese selector.

Los búferes inmutables se reconstruyen solo si el contrato validado de la llamada importada limita la lectura a una longitud no negativa y excluye escrituras, retención de punteros y comparaciones de identidad de direcciones. Se conservan los bytes y la longitud; los demás usos del puntero quedan sin resolver. El fallo de aserción exacto de la biblioteca estándar Swift conserva sus portadores escalares y de pila derivados del compilador, `swiftcall`, el efecto `noreturn` y la grafía del enlazador; los shims de diagnóstico C mantienen su ABI C declarada y una trampa posterior independiente. Las cadenas estáticas y el almacenamiento de literales String inmortales se copian solo para este consumidor terminal autenticado que no retiene el puntero. Los valores String dinámicos o con propiedad no se tratan como literales.

En una sesión con Mach-O ya cargado, `neverd_objc_methods_json(session, max_functions)` y `neverd_swift_methods_json(session, signatures_json, max_functions)` devuelven los informes. Cero selecciona todas las funciones descubiertas. Libere las cadenas con `neverd_free_string`; `NULL` indica fallo, explicado por el error de sesión. Estas API no cargan contenedores IPA/`.app`.

La inferencia de dependencias nativas de Objective-C puede reducir un parámetro entero de registro de 64 bits de `NativeAnalysis` a 32 bits después de enlazar las llamadas fuente. Una prueba exhaustiva de usos HighIR debe demostrar que cada aparición observa exactamente los cuatro bytes bajos, mediante una extracción de bytes con desplazamiento cero o un argumento entero exacto de una llamada enlazada. La prueba es independiente para cada parámetro; los usos de ancho completo, con desplazamiento distinto de cero, mal formados o que agotan el presupuesto conservan el ancho original. La función auxiliar refinada vuelve a someterse al lifting y debe superar las comprobaciones ordinarias del cuerpo y del cierre de dependencias; la ABI de reescritura no cambia.

Los getters de objetos estáticos con inicialización diferida de Swift expuestos mediante thunks de entrada de Objective-C solo se pueden proyectar cuando el símbolo `vgZTo`, la firma del método en tiempo de ejecución, la prueba del predicado, la llamada a `swift_once`, la carga del almacenamiento y el resultado de `objc_retainAutoreleaseReturnValue` forman un patrón exacto del compilador. Deben coincidir los símbolos del predicado `_Wz`, el inicializador `_WZ` y el almacenamiento `vpZ`; el tercer registro sin procesar solo puede aparecer como contexto de once, y el inicializador debe ignorar ese contexto y no tener ningún llamador directo ordinario. La proyección reconstruye el predicado y el almacenamiento del objeto, pasa null como contexto irrelevante y conserva el inicializador como dependencia comprobada. Cualquier variación en la forma, los símbolos, el uso de parámetros o el callback permanece sin recuperar.

Los thunks de constructor Swift Objective-C con un símbolo `cfcTo` autenticado y la firma de ejecución `init` también pueden eliminar un tercer registro de argumento no declarado cuando su única aparición es el contexto de una sola llamada exacta a `swift_once`. El predicado `_Wz` y el inicializador `_WZ` deben coincidir; el callback debe ignorar el contexto y no tener ningún llamador directo ordinario. La proyección pasa null, deduce únicamente el rango de almacenamiento de ocho bytes del predicado y conserva todos los demás efectos de control, memoria y llamadas; siguen aplicándose las comprobaciones habituales del cuerpo fuente y del cierre de dependencias.

Los callbacks once de ARM64 pueden reenviar un contexto x2 sin otros usos a un único `swift_once` anidado si el símbolo exterior `_WZ` y el par interior `_Wz`/`_WZ` son exactos, ninguno tiene llamadores directos ordinarios y el callback hoja, tipado de forma independiente, ignora su contexto. El mismo contrato controla el descubrimiento y la proyección. Esta pasa null y conserva todas las instrucciones; la ABI void solo permite descartar retornos puros de registros, temporales o constantes. Se rechazan lecturas de pila, cargas y llamadas en el retorno, así como dependencias sin resolver.

## Verificación y resolución de problemas

En macOS, las compilaciones con `BUILD_TESTING` activado ofrecen `check-neverd-mobile-ios`, que ejecuta las tres suites de recuperación nativa mediante CTest.

Python solo se utiliza en los scripts de prueba de desarrollo siguientes; la recuperación móvil integrada se ejecuta en la CLI nativa C++20.

```sh
cmake --build build --target check-neverd-mobile-ios
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_ios_calls_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

En macOS, los scripts Objective-C compilan los originales, recuperan `.m` y enlazan únicamente el fuente generado con un programa de llamadas independiente. El script escalar cubre límites enteros, ramas, bucles, lecturas/escrituras por puntero, argumentos implícitos, identidad de bits float/double, argumentos mixtos y argumentos en pila. El script de llamadas añade despacho de mensajes, herencia, Category, almacenamiento de variables de instancia, auxiliares nativos e invocación/capturas/identidad compartida de Blocks. El corpus de llamadas exige 21/21 métodos recuperados y 134/134 resultados independientes en cada variante arm64/x86_64 × classic/default. Ejecute estas pruebas con la compilación actual de la CLI nativa.

El script Swift estricto comprueba 22 declaraciones de usuario, tres entradas getter/setter y nueve entradas invocables generadas por el compilador; ninguna puede desaparecer del inventario. Cada variante incluye 858 comprobaciones independientes de resultados del programa original. Compila por separado el `.swift` generado y su programa de llamadas, sin la dylib, el módulo o el puente original ni declaraciones sustitutivas escritas a mano. Los casos incluyen llamadas escalares/nativas, inicialización y almacenamiento de clases, métodos de estructuras por valor/mutating, argumentos flotantes y en pila, punteros y bucles. La CLI nativa C++20 debe superar las cuatro variantes arm64/x86_64 × classic/default sin omisiones: cada una debe recuperar 25 cuerpos nativos y nueve proyecciones del compilador, conservar las 34 identidades invocables y superar 858/858 comprobaciones tanto para los originales como para el Swift generado compilado independientemente. Estos resultados se limitan al corpus y no garantizan la recuperación de aplicaciones arbitrarias ni del texto fuente original. El script rechaza cobertura ausente, fallos de compilación del fuente y diferencias de comportamiento.

Estas cuatro variantes tienen macOS como destino. Las dos entradas adicionales del compilador son el inicializador del valor vacío y su accesor de metadatos; las pruebas nativas los validan por separado y una unidad de fuente `struct Empty {}` conserva ambas identidades. Superar este corpus no acredita una aplicación iOS real.

Los tres scripts admiten `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N` y `--work-dir NEW_DIRECTORY`. `--setup-only` valida los originales y no prueba la recuperación. La aceptación exige completar todas las variantes de arquitectura y fixup solicitadas, también en el script escalar. Una variante ausente o una arquitectura solicitada que el equipo no pueda ejecutar provoca un fallo; no se permiten omisiones. Los artefactos de fallo conservados permiten distinguir cobertura de fuente ausente, errores de compilación y diferencias de comportamiento. Consulte los resultados actuales antes de afirmar que el soporte está verificado.

El [workflow Mobile Real Applications](../../.github/workflows/mobile-real-apps.yml) utiliza aplicaciones públicas con versiones fijadas en el [manifiesto del corpus](../../scripts/mobile_real_apps.json). La aceptación exige inventarios independientes de todos los Mach-O de los bundles iOS completos y todos los DEX de los APK, reconstrucciones independientes de los originales y del código generado, y comparaciones de comportamiento. Las etapas ausentes, la cobertura de inventario desconocida o los casos obligatorios que falten hacen fallar la validación. Las etapas recompile y behavior de las aplicaciones reales siguen incompletas, por lo que se conserva la etiqueta Experimental. Superar las pruebas de protección del arnés no demuestra que las aplicaciones reales hayan pasado.

La publicación es transaccional: elija una carpeta nueva, revise primero el estado de salida y redirija JSON fuera de ella. Los fallos eliminan resultados temporales y conservan los existentes. Una salida no nula del backend incluye un tramo final acotado del registro. Los tiempos agotados del backend conservan el mensaje original y añaden un tramo final acotado cuando hay texto del registro capturado disponible. Los presupuestos excedidos conservan sus propios mensajes. La CLI nativa devuelve cero si tiene éxito y un valor distinto de cero si falla la recuperación. Con `--json`, los errores controlados incluyen `schema_version`, `status: "error"` y `error`. El análisis de argumentos, los fallos de inicio del ejecutable o las bibliotecas nativas y las interrupciones pueden notificarse solo por stderr. Compruebe primero el estado de salida.

Para secciones cifradas, proporcione entradas legibles; para arquitecturas ausentes, revise las disponibles. Lea motivos exactos y diagnósticos de métodos omitidos. Ampliar `--max-func` solo ayuda a funciones excluidas por ese límite. Disposiciones, firmas, encabezados externos, excepciones o ABI ausentes requieren implementación o metadatos válidos adicionales, no una afirmación de recuperación completa. Conserve los avisos de licencia aplicables al distribuir herramientas o paquetes generados.

La inferencia de auxiliares escalares nativos también admite parámetros y resultados float/double en registros. El análisis compartido de bytes de entrada MedIR debe demostrar que una entrada vectorial ancha solo observa un carril escalar inferior. Los valores locales CONCAT solo se estrechan si todas sus definiciones tienen el mismo ancho inferior, las expresiones superiores descartadas no tienen efectos y cada uso lee explícitamente ese prefijo. Se conservan llamadas, escrituras, posiciones de ramas y bits flotantes distintos de NaN; no se inventan bits superiores desconocidos.

Un auxiliar nativo hoja que transfiere el control a llamadas finales void externas validadas puede usar una firma fuente interna void si no tiene un resultado escalar completo. LowIR debe concordar con cada llamada y su retorno sintético. La prueba de hoja prohíbe escribir registros preservados, de marco, de pila y de enlace; un punto fijo acotado de contaminación por bytes también rechaza almacenar valores derivados de la pila o pasarlos a una llamada. Se mantienen las comprobaciones de CFG, cuerpo y dependencias. No se suministran bits de resultado: los llamadores que leen un resultado desconocido siguen sin recuperarse. Las regresiones comprueban la destrucción condicional de objetos, resultados independientes y el rechazo de lecturas de resultados desconocidos.

Los resúmenes nativos void también admiten marcos de pila y llamadas ordinarias cuando un análisis LowIR acotado demuestra que todas las salidas restauran los bytes de registros preservados, el puntero de pila y el registro de enlace. Las escrituras parciales, extensiones implícitas con ceros, almacenamientos solapados y llamadas invalidan los hechos afectados. Los almacenamientos mediante direcciones que no contienen bytes derivados del marco están separados del marco privado de la invocación; los alias derivados parcial o totalmente del marco, así como guardar o dejar escapar su dirección, impiden la prueba. La única excepción de préstamo del marco es el primer argumento exacto de un `objc_msgSendSuper2` vinculado a la fuente: puede señalar un objeto `objc_super` completo de 16 bytes situado íntegramente dentro del marco asignado porque ese contrato de ejecución lo lee de forma síncrona. Los mensajes ordinarios, punteros incompletos, objetos fuera del marco y cualquier otro argumento del marco siguen rechazados. La vivacidad por bytes de HighIR trata un valor de ranura de pila como una lectura acotada de los bytes de esa ranura, mientras que tomar o pasar su dirección sigue siendo un escape; los accesos mal formados o solapados se tratan de forma conservadora. Así, los almacenamientos privados separados solo pueden descartar los bytes finales no leídos. Tras repetir el lifting, se pueden eliminar los parámetros de registros auxiliares sin ninguna aparición en HighIR y ejecutar de nuevo el proceso. La limpieza HighIR existente sigue encargándose de los almacenamientos privados; los parámetros ordinarios y las entradas utilizadas se conservan.

El catálogo UIKit para arm64 también vincula el resultado objeto de `observedProgress` y `setProgress:animated:` de `UIProgressView`. Este último conserva registros separados para los argumentos `float` y booleano. Ambas declaraciones requieren pruebas coincidentes del compilador para iPhoneOS y arm64 iPhoneSimulator, y el proveedor exacto de UIKit del sistema. Las demás arquitecturas siguen sin soporte.

Las palabras de literales etiquetadas pueden contener una dirección completa de la imagen de un grupo inmutable de cadenas C bajo la etiqueta exacta del bit superior. El enlace de fuente reubica únicamente esa dirección probada en el grupo compartido permanente y conserva el OR entero, la etiqueta y el desplazamiento interno. Se rechazan valores escalares o direcciones parciales similares, procedencia contradictoria, grupos modificables o con reubicaciones y usos como dirección de memoria directa; no se deduce la disposición de un objeto Swift String.

`setProgress:` de `UIProgressView` requiere un receptor demostrado, como el resultado tipado de una propiedad conservado por una llamada ARC de identidad exacta. La clausura verificada de superclases y protocolos selecciona el argumento `float`. Sin receptor cualificado, la llamada sigue siendo ambigua porque UIKit y clases locales también declaran setters con argumentos objeto para ese selector.

El catálogo UIKit para arm64 también registra `setTitleColor:forState:` de `UIButton`, con un objeto y un `UIControlState` de 64 bits sin signo, y el método void `invalidateIntrinsicContentSize` de `UIView`. Los AST completos del dispositivo y del simulador coinciden. La herencia observada `UIImageView → UIView` permite a las pruebas de receiver existentes distinguir setters locales de objetos de setters de coma flotante homónimos en clases no relacionadas. Los receivers desconocidos, los conflictos de declaraciones de subclases, la falta de proveedores de declaraciones padre y x86_64 siguen sin admitirse.
