**Languages**: [English](../ios.md) | [简体中文](../zh-CN/ios.md) | [繁體中文](../zh-TW/ios.md) | [日本語](../ja/ios.md) | [한국어](../ko/ios.md) | [Français](../fr/ios.md) | [Deutsch](../de/ios.md) | [Español](ios.md) | [Italiano](../it/ios.md) | [Русский](../ru/ios.md) | [العربية](../ar/ios.md)

# Recuperación de código nativo y fuentes de iOS

[← Índice de documentación](README.md) · [Resumen móvil](../mobile.md)

`neverd mobile` acepta IPA, `.app` y Mach-O. Exporta C nativo, metadatos del runtime y fuentes experimentales Objective-C `.m` y Swift `.swift` para los cuerpos nativos compatibles. Un resultado publicado puede incluir métodos sin recuperar: consulte la cobertura antes de usarlo. Los contenedores móviles pertenecen a la CLI; el SDK C nativo carga por separado el Mach-O seleccionado.

La compilación elimina comentarios, formato, identificadores y estructuras del lenguaje. Este flujo reconstruye una representación fuente; no recupera el texto original ni certifica un comportamiento equivalente para aplicaciones arbitrarias. Tampoco ejecuta la aplicación analizada.

## Inicio y dependencias

```sh
cmake --build build --target neverd
python3 --version
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile executable -o metadata --metadata-only
neverd mobile App.app -o recovered-framework --artifact Frameworks/Example.framework/Example
```

Compile NeverD normalmente y distribuya el directorio contiguo `mobile/` junto al ejecutable. Se necesita Python 3.10+, elegido mediante `--python PATH`, después `NEVERD_PYTHON` y finalmente `python3`/`python` en PATH. No se descargan dependencias automáticamente. Compilar de forma independiente las fuentes Apple generadas en macOS requiere Apple Clang, el SDK y la cadena Swift; son requisitos separados del análisis nativo estático.

Las firmas Swift utilizan `--swift-demangle PATH`, después `NEVERD_SWIFT_DEMANGLE` y luego `swift-demangle` en PATH. En macOS se intenta finalmente `xcrun --find swift-demangle` con tiempo limitado. Una herramienta explícitamente configurada pero ausente provoca un error; si falla la búsqueda automática, los símbolos quedan sin clasificar con estado `unavailable`. Sin símbolos Swift no hace falta demangler. `--metadata-only` no invoca el backend nativo ni el demangler.

```sh
neverd mobile App.ipa -o recovered-swift \
  --python python3 --swift-demangle /path/to/swift-demangle --timeout=600 --json
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
| `--python PATH` | Entorno/PATH | Intérprete Python 3.10+ del auxiliar |
| `--swift-demangle PATH` | Entorno/PATH/cadena | Demangler de firmas Swift |
| `--timeout N` | `300` | Segundos positivos por proceso backend |
| `--max-files N` | `20000` | Presupuesto positivo de entradas; el inventario Swift también está limitado |
| `--max-bytes N` | `2147483648` | Presupuesto positivo de bytes para entrada, extracción y salida final |
| `--json` | Desactivado | Imprimir el informe versionado como JSON |

Se supervisa el área de trabajo, permitiendo hasta tres veces los presupuestos de entradas/bytes para copias y resultados intermedios. Los registros están limitados a 16 MiB por proceso y el JSON de firmas Swift a 32 MiB. Son controles de recursos, no aislamiento de procesos. Ampliar el tiempo no desactiva otros límites. Los métodos excluidos por `--max-func` siguen figurando como no recuperados si están en el inventario de metadatos.

## Fuentes Objective-C y estructura del runtime

El cargador nativo vincula registro de método, dirección IMP ejecutable y codificación de tipo compatible con posiciones ABI fuente explícitas. Los parámetros fijos escalares/punteros conservan `self`/`_cmd`, argumentos sin usar, bancos enteros/flotantes separados y posiciones de pila compatibles. La reinterpretación de bits float/double se distingue de la conversión numérica. Las pistas de tipo sirven para proyectar fuentes; no son pruebas ABI autenticadas ni permiso para parchear código ejecutable.

`sources/objc.m` coloca las instrucciones realmente reconstruidas dentro de `@implementation` y conserva auxiliares C y llamadas tipadas necesarias. Los destinos requieren una vinculación fuente compatible; destinos desconocidos y grupos de dependencias incompletos quedan sin recuperar. Definiciones ausentes, direcciones no ejecutables, tipos contradictorios, ABI no compatibles, decodificación incompleta o IR rechazado no se convierten en métodos recuperados solo por tener declaración.

Los metadatos conservan superclase, inicio/tamaño de instancia e ivars escalares/punteros con desplazamientos, anchuras y alineaciones comprobados. Las declaraciones añaden relleno cuando hace falta. Un método que necesita una disposición desconocida queda sin recuperar. Las categorías mantienen identidades clase/categoría/dirección e implementaciones separadas; registros exactamente iguales repetidos en ambos inventarios cuentan una vez. Las categorías externas compatibles usan declaraciones Foundation existentes. Los encabezados externos desconocidos se señalan como dependencias ausentes; no se inventan clases sustitutas.

La reconstrucción del runtime es limitada. No se prometen propiedades y protocolos completos, anotaciones originales de propiedad, agregados arbitrarios, colas variádicas, cuerpos dependientes de excepciones ni disposiciones Block/capturas no modeladas. La codificación solo describe argumentos fijos y no demuestra que la declaración original careciera de puntos suspensivos. Los punteros encadenados se utilizan solo en posiciones resueltas por el cargador; los formatos no resueltos conservan diagnósticos.

## Fuentes Swift y almacenamiento

La salida estructurada del demangler separa firmas invocables y metadatos no invocables. Antes de proyectar el cuerpo nativo, las firmas compatibles se vinculan a símbolos, entradas y ABI de máquina explícita del binario seleccionado. Los receptores Swift siguen su ABI, sin sustituirlos por argumentos ocultos Objective-C. Un archivo de firmas proporcionado por el usuario sigue siendo una pista que debe validarse.

El emisor experimental construye funciones libres, métodos de clase, inicializadores designados y métodos de struct de disposición fija compatibles, incluidas ciertas formas de receptor mutating. Las declaraciones y campos almacenados necesitan metadatos de disposición recuperados. Las llamadas nativas solo se emiten si declaraciones y cuerpos necesarios forman un grupo completo de dependencias compatibles. Las unidades fuente reúnen declaraciones y métodos, sin puentes que llamen al binario original.

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

Los archivos del lenguaje fuente existen solo cuando se puede emitir código. `objc.json` guarda clases, categorías, ivars y codificaciones originales; `objc.h` contiene declaraciones compatibles. `swift.json` contiene tipos nominales y símbolos mangled. Los JSON de firmas/métodos conservan clasificación, omisiones, razones y conteos. Los registros incluyen diagnósticos nativos y, cuando se usan, búsqueda de herramientas Swift, demangling y exportación nativa Swift. Las rutas de `report.json` son relativas a su directorio. El binario seleccionado es un artefacto de análisis; el código generado no lo enlaza como puente de recuperación.

Se eliminan copias temporales del paquete y JSON intermedios. Sin cuerpos nativos, una ejecución normal falla aunque haya metadatos. El modo metadatos solo produce el artefacto seleccionado, `objc.h`, `objc.json`, `swift.json`, `report.json`; no hay fuentes ni archivos de firmas/cobertura, y `native_function_count`, `objc_method_recovery`, `swift_method_recovery` son `null`. Su lector Objective-C Python no resuelve punteros encadenados ni de objetos reubicables. La recuperación completa usa metadatos Objective-C resueltos por el cargador nativo. El lector Swift básico aún puede informar referencias no compatibles como parciales.

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
    "unrecovered_method_count": 2,
    "metadata_symbol_count": 5,
    "unclassified_symbol_count": 1
  }
}
```

El `status: "success"` exterior indica que se publicó una salida validada. `recovered`, `partial`, `unrecovered`, `no-methods` describen el inventario descubierto, no equivalencia semántica ni integridad del programa original. Cada método no recuperado tiene una razón. Objective-C `recovered` exige también metadatos runtime completos. Un inventario vacío no demuestra que no existieran métodos.

Swift `coverage_status` cuenta solo elementos invocables clasificados. El `status` Swift global contempla símbolos desconocidos y puede ser `unavailable`, `unclassified`, `unsupported-architecture` o `no-symbols`. Los metadatos no invocables aparecen en `non_method_symbols` como `not-callable`; los desconocidos, como `unclassified`. `types`, `type_metadata_count`, `source_type_count` cuentan por separado tipos y unidades emitidas, sin inflar la cantidad de métodos.

El lote Swift describe `source_units` con `{kind, module, name, source, method_entries, method_identities}`; kind es `function` o `type`, y cada identidad `{entry, mangled_symbol}`. Símbolos distintos pueden compartir entrada y mantener proyecciones ABI diferentes. Cada identidad recuperada debe aparecer exactamente una vez y ninguna no recuperada puede figurar. `method_entries` debe coincidir con la proyección ordenada de entradas de `method_identities`, incluidas direcciones repetidas. No se deben fusionar silenciosamente identidades idénticas duplicadas. `source` concatena en orden la fuente de cada unidad más un salto de línea. Mobile guarda el agregado en `sources/swift.swift` y las descripciones en el JSON de cobertura. El `source` individual sirve para inspección; concatenarlo no reconstruye correctamente clases.

## Exportación nativa directa y SDK

```sh
neverd export recovered-ios/artifacts/selected.macho \
  --format=objc-methods --max-func=20 -o objc-batch.json
neverd export recovered-ios/artifacts/selected.macho \
  --format=swift-methods \
  --source-signatures=recovered-ios/metadata/swift-signatures.json -o swift-batch.json
```

La exportación Swift consume el inventario estructurado de firmas generado por una ejecución mobile normal con demangler. El lote Objective-C incluye `native_source`, `native_function_count`, `objc_metadata` y, por método, fuente C, nombre, tipo de retorno y parámetros. Mobile comprueba además declaraciones, cuerpos y disposiciones antes de `.m`, por lo que su cobertura puede ser menor que la del lote C. Una exportación nativa exitosa puede no contener métodos recuperados.

En una sesión con Mach-O ya cargado, `neverd_objc_methods_json(session, max_functions)` y `neverd_swift_methods_json(session, signatures_json, max_functions)` devuelven los informes. Cero selecciona todas las funciones descubiertas. Libere las cadenas con `neverd_free_string`; `NULL` indica fallo, explicado por el error de sesión. Estas API no cargan contenedores IPA/`.app`.

## Verificación y resolución de problemas

```sh
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd
```

En macOS, el runner Objective-C compila ejemplos propios originales, recupera `.m` y enlaza solo fuentes generadas con un programa de llamadas independiente. Comprueba límites enteros, ramas, bucles, punteros, argumentos ocultos, identidad de bits float/double, parámetros mixtos y pila. El runner Swift recompila `.swift` y su programa de prueba sin dylib, módulo, puente ni declaraciones sustitutas manuales originales. Cubre escalares/llamadas nativas, inicialización/almacenamiento de clases, métodos de struct por valor/mutating, flotantes, pila, punteros y bucles. Son pruebas estrictas que pueden revelar límites; su existencia no demuestra que todos los casos pasen en cualquier build.

Ambos scripts aceptan `--arch all|arm64|x86_64`, `--fixups both|classic|default`, `--timeout N`, `--work-dir NEW_DIRECTORY`. `--setup-only` valida originales, no recuperación. Cuando se permite, las arquitecturas que el host no puede ejecutar se omiten explícitamente; omitir no equivale a aprobar. Conserve los artefactos para distinguir cobertura ausente, errores de compilación y diferencias de comportamiento. Compruebe resultados actuales antes de afirmar compatibilidad verificada.

La publicación es transaccional: elija una carpeta nueva, revise primero el estado de salida y redirija JSON fuera de ella. Los fallos eliminan resultados temporales y conservan los existentes. Un backend con salida no cero incluye una cola de registro limitada; timeout y presupuesto tienen mensajes separados. Con `--json`, errores controlados del auxiliar producen `status: "error"`; análisis de argumentos, auxiliar/intérprete ausente, Python anterior a 3.10 o interrupciones pueden fallar antes en stderr.

Para secciones cifradas, proporcione entradas legibles; para arquitecturas ausentes, revise las disponibles; para Swift, seleccione el demangler real. Lea motivos exactos y diagnósticos de métodos omitidos. Ampliar `--max-func` solo ayuda a funciones excluidas por ese límite. Disposiciones, firmas, encabezados externos, excepciones o ABI ausentes requieren implementación o metadatos válidos adicionales, no una afirmación de recuperación completa. Conserve los avisos de licencia aplicables al distribuir herramientas o paquetes generados.
