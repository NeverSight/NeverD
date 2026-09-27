**Idiomas**: [English](../mobile.md) | [简体中文](../zh-CN/mobile.md) | [繁體中文](../zh-TW/mobile.md) | [日本語](../ja/mobile.md) | [한국어](../ko/mobile.md) | [Français](../fr/mobile.md) | [Deutsch](../de/mobile.md) | [Español](mobile.md) | [Italiano](../it/mobile.md) | [Русский](../ru/mobile.md) | [العربية](../ar/mobile.md)

# Recuperación de aplicaciones móviles

[← Índice de documentación](README.md) · [Guía completa de Android](android.md) · [Guía completa de iOS](ios.md)

`neverd mobile` recupera Java legible de entradas Android APK, DEX y smali. Para entradas iOS IPA, `.app` y Mach-O, exporta C nativo y reconstruye los cuerpos de métodos Objective-C admitidos como código fuente `.m`, junto con código Swift experimental y metadatos de ejecución. Es un flujo experimental de CLI; el SDK C nativo y el cargador gráfico no aceptan contenedores móviles.

## Preparación

Compile el objetivo `neverd` con soporte para C++20. El flujo móvil se compila dentro de la CLI nativa y no utiliza un intérprete de Python. Distribuya el ejecutable con las bibliotecas nativas que requiera su compilación.

El tratamiento nativo de ZIP usa zlib para CRC-32 y DEFLATE. CMake prefiere una biblioteca instalada mediante `find_package`; en caso contrario, descarga zlib 1.3.2 con un SHA256 fijado y la compila estáticamente. Esta implementación de ZIP móvil no necesita herramientas auxiliares de Python en Windows. Conserve los avisos de dependencias de [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).

El motor predeterminado está implementado en C++20 y no necesita Python, Java ni JADX en tiempo de ejecución. Solo un `--jadx PATH` explícito selecciona el adaptador de compatibilidad instalado por separado; `NEVERD_JADX` y PATH no lo seleccionan automáticamente, y no hay cambio automático a otro motor. El adaptador opcional requiere JADX 1.5.6+ con los plugins estándar de entrada DEX/smali y Java 11+. Su informe identifica el motor real `jadx` y su versión. La instalación y las licencias de dependencias siguen documentadas en la [guía de Android](android.md#adaptador-opcional-de-compatibilidad-jadx).

## Android

Para obtener rápidamente un directorio de clases sin recuperación Java, use
`neverd mobile app.apk --list-classes`, opcionalmente con
`--class-prefix com.example` o `--json`. El
[contrato de inventario](android.md#inventario-rápido-de-clases) describe el
orden, los límites y la validación de cargas seleccionadas. El modo de consulta
no necesita directorio de salida; su `-o` opcional indica un archivo nuevo.

Para referencias directas de bytecode, use
`neverd mobile app.apk --find-refs string --query 'example' --json`.
El [contrato de consultas de referencias](android.md#consultas-de-referencias-de-código)
también cubre operandos de tipos, métodos y campos, coincidencia literal/exacta,
posiciones de cada aparición, conservación de UTF-16 y alcance de la validación
de código. Sin `--json`, esta operación emite JSON Lines. Comparte el
comportamiento de archivo nuevo de `-o`.

Los siguientes ejemplos y salidas describen la recuperación.

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Todos los `classes.dex`, `classes2.dex` y archivos DEX numerados posteriores de la raíz de un APK se analizan juntos. Un directorio smali se recorre recursivamente y todas sus clases se analizan en una sola invocación, incluidas las anidadas y las del mismo conjunto. Use un directorio cuando recupere clases que se referencian entre sí. Un archivo smali individual solo aporta esa clase.

La salida de recuperación integrada contiene `sources/`, `metadata/android-methods.json` y `report.json`, con `backend: {"name": "neverd", "version": "1", "execution": "builtin"}`. El informe incorpora `android_method_recovery`: `method_count = recovered_method_count + declaration_only_method_count`, y `unrecovered_method_count` debe ser cero antes de publicar. Las declaraciones originales `native`/`abstract` se cuentan por separado de los cuerpos recuperados. El adaptador externo explícito conserva sus propios registros del backend. Los recursos del APK, manifiestos, bibliotecas nativas y código cargado dinámicamente quedan fuera de esta ruta Java; las bibliotecas nativas se pueden analizar por separado con `neverd decompile`.

Los lectores de recuperación integrados comparten un modelo Dalvik tipado implementado de forma independiente y un generador Java con trabajo acotado para código habitual representable DEX 035/037–040 y smali. DEX 041, las llamadas dinámicas como `invoke-custom`, algunas rutas de inicialización, las operaciones desconocidas y los identificadores no representables en Java fallan explícitamente. El Java generado puede usar un bucle de despacho; no ejecuta el DEX original ni lo llama mediante un puente de ejecución. No se pueden restaurar comentarios originales, formato ni nombres eliminados. El motor experimental no promete equivalencia funcional con JADX, equivalencia semántica ni recuperación completa de cualquier APK.

## iOS

La [guía completa de iOS](ios.md) documenta la selección de IPA, `.app` y Mach-O, la preparación, todas las opciones CLI, los esquemas de código fuente, la cobertura y la verificación.

```sh
neverd mobile App.ipa -o recovered-ios
neverd mobile App.app -o recovered-arm64 --arch=arm64
neverd mobile App.app -o framework-analysis --artifact Frameworks/Example.framework/Example
neverd mobile executable -o metadata --metadata-only
```

Cada ejecución selecciona un ejecutable. `--artifact` es relativo al paquete de la aplicación tanto en IPA como en `.app`. La selección de binarios universales prefiere arm64, arm, x86_64 y después i386; la proyección a lenguajes fuente se dirige a arm64/x86_64. Se rechazan los segmentos seleccionados cifrados. La ruta nativa experimental emite C y cuerpos de métodos Objective-C admitidos, incluidos enlaces ABI para escalares/punteros, coma flotante, parámetros mixtos y posiciones de pila. Se conservan las disposiciones de clases/variables de instancia de ejecución y las categorías separadas cuando se validan; las disposiciones, firmas, llamadas y demás dependencias sin resolver permanecen como omisiones explícitas.

La recuperación Swift clasifica firmas dentro del proceso C++ usando `LLVMSwiftDemangle` del fork LLVM de NeverD. No inicia ningún demangler externo ni comando de detección de cadena de herramientas, y no necesita un compilador Swift instalado para compilar o ejecutar NeverD. Las compilaciones desde el código del fork y los paquetes LLVM correspondientes incluyen el componente; NeverD no obtiene una dependencia separada de código fuente Swift. El inventario de firmas registra `demangler: {"name": "llvm-swift-demangle", "execution": "builtin", "version": "6.3.3"}`. Las firmas admitidas se vinculan con entradas nativas y ubicaciones ABI antes de emitir funciones `.swift` reales, métodos/inicializadores de clases y métodos de estructuras de disposición fija. Las formas genéricas/resilientes, asíncronas/que lanzan errores, las formas invocables generadas durante la ejecución no admitidas y los grupos incompletos de dependencias de código fuente permanecen sin recuperar.

La salida normal incluye `sources/native.c`, los opcionales `sources/objc.m` y `sources/swift.swift`, declaraciones y metadatos de ejecución, JSON de cobertura de métodos/firmas, registros, `artifacts/selected.macho` y `report.json`. No hay registros de detección externa de la cadena Swift ni de demangling externo. El código generado no llama al binario original como puente de recuperación. Las `source_units` de Swift agrupan declaraciones de tipos y métodos; no se deben concatenar filas de métodos independientes para reconstruir clases. El `status: "success"` externo significa publicación de la salida, no cobertura completa de métodos ni equivalencia semántica.

`--metadata-only` no ejecuta el exportador de código nativo ni el demangling de firmas, y no emite código fuente ni cobertura de métodos. Todos los modos usan los metadatos Objective-C resueltos por el cargador nativo. Los metadatos Swift usan lecturas acotadas de la imagen nativa; las correcciones, disposiciones reubicables o referencias no admitidas conservan diagnósticos parciales. `--max-func` limita la recuperación de funciones nativas y se ignora en el modo de solo metadatos. La ausencia de cuerpos de funciones nativas hace fallar una ejecución normal. Se eliminan las entradas desempaquetadas temporales.

Los comentarios originales, el formato, los identificadores eliminados y las estructuras de código fuente perdidas al compilar no se pueden reconstruir exactamente. Antes de usar la salida, consulte el estado y motivo de recuperación de cada método, los recuentos separados de elementos Swift invocables/no invocables/no clasificados y los límites documentados.

## Límites y fallos

Para recuperación, `-o` debe indicar un directorio nuevo fuera de cualquier directorio de entrada. Los modos de consulta aceptan en cambio un archivo nuevo de salida opcional. Nunca se sobrescribe una salida existente. El trabajo de recuperación se prepara temporalmente y solo se publica tras completar correctamente la recuperación y validar la salida. Los resultados de consultas se guardan en memoria hasta que todos los DEX seleccionados se procesen correctamente. Las ejecuciones correctas de la CLI nativa devuelven cero. Los fallos de recuperación devuelven un valor distinto de cero; `--json` informa de fallos controlados mediante `schema_version`, `status: "error"` y `error`. Los errores al analizar argumentos, al iniciar el ejecutable nativo o las bibliotecas y las interrupciones pueden informarse por stderr. Los consumidores deben comprobar primero el estado de salida.

Los valores predeterminados son 20 000 entradas, 2 GiB de entrada/datos extraídos o datos finales de salida y 300 segundos para el análisis integrado Android/iOS o cada proceso JADX explícito. Los procesos hijos de iOS reciben el presupuesto total de análisis restante. El lector y el generador integrados también imponen un presupuesto de trabajo acotado. Configure `--max-files`, `--max-bytes` y `--timeout` para ajustar estos límites positivos. El área de trabajo temporal se supervisa mientras se ejecutan los backends, con hasta tres veces los límites de entradas/bytes para permitir que coexistan entradas preparadas y salidas intermedias. Los diagnósticos se limitan a 16 MiB por proceso.

Durante la recuperación, la preparación de APK solo escribe `classes.dex`, `classes2.dex` y los archivos DEX numerados posteriores de la raíz. Todas las entradas ZIP siguen pasando por comprobaciones de cabeceras/intervalos, descompresión y validación de longitud y CRC, y cuentan para los límites de entradas y bytes sin comprimir del archivo. Los recursos no escritos pueden tener nombres distintos que difieran en mayúsculas y minúsculas, como `res/-A.xml` y `res/-a.xml`. Los nombres ZIP exactamente duplicados y los conflictos de identidad entre archivos y directorios siguen siendo errores; las comprobaciones portables de colisiones de mayúsculas/minúsculas del sistema de archivos se aplican a las entradas escritas realmente. La extracción completa, incluida la entrada IPA, sigue rechazando esas colisiones de salida. Se rechazan en todo el archivo las rutas de recorrido fuera del destino, los enlaces, los archivos especiales y las entradas ZIP cifradas. Las entradas de directorio también rechazan enlaces simbólicos y archivos especiales.

Estos límites son controles de robustez, no un entorno aislado de seguridad para código de backends de terceros. Los comandos explícitos de JADX y exportación de código nativo se ejecutan como procesos hijos locales. Se eliminan las salidas temporales fallidas. Las salidas de backend con código distinto de cero incluyen un fragmento final acotado de diagnóstico. Los tiempos agotados del backend conservan el mensaje de timeout y añaden un fragmento final acotado si hay texto de registro capturado. Los fallos de inicio y las infracciones de presupuesto conservan sus propios mensajes de error.

## Verificación

Python solo se usa en los bancos de prueba de desarrollo siguientes; la recuperación móvil integrada se ejecuta en la CLI nativa C++20.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

Las pruebas de componentes cubren análisis, contenedores inseguros, fallos de backend, limpieza de salida, selección de arquitectura y conservación de salida. Las pruebas que invocan la CLI compilada usan `NEVERD_BUILD_DIR`. El ejecutor interno Android usa un JDK (`java` y `javac`) y D8 para crear casos DEX/APK independientes, y después compilar y ejecutar el Java recuperado. Son dependencias de verificación, no requisitos de recuperación integrada. Ejecute las pruebas sobre la compilación actual y examine el resultado antes de dar un caso por verificado. El ejecutor separado de compatibilidad necesita además JADX; el éxito de los casos de prueba no demuestra recuperación de cualquier aplicación.

En macOS, con Apple Clang, su SDK y NeverD compilado, ejecute la comparación real de ejecución de Objective-C:

```sh
python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
cmake --build build --target check-neverd-mobile-ios
```

El ejecutor construye su propio caso Objective-C, recupera sus implementaciones de métodos y enlaza únicamente el `.m` recuperado con el mismo banco de llamadas independiente. Su caso de 22 métodos compara 141 resultados observables que cubren límites de enteros, saltos, bucles, lecturas/escrituras mediante punteros, argumentos ocultos y no usados, bits de identidad float/double, parámetros mixtos y posiciones de pila. Se requiere una ejecución actual correcta antes de considerar verificado un caso. Deben completarse todas las arquitecturas y variantes de correcciones solicitadas; el valor predeterminado cubre arm64/x86_64 × classic/default. Una variante ausente o una arquitectura que el host no puede ejecutar es un fallo y no se permite omitirla. Esta evidencia de casos de prueba no demuestra completitud para programas iOS arbitrarios. `NeverDMobileIOSBackend` está registrado con CTest en macOS, incluido el perfil principal de pruebas CI.

El ejecutor independiente de recuperación Swift es `python3 scripts/test_mobile_swift_backend.py --neverd build/bin/neverd`. Recompila el Swift generado y su banco de pruebas sin enlazar el binario original; los elementos invocables no admitidos y las diferencias de comportamiento son fallos. Consulte la [guía de iOS](ios.md) para la semántica de cobertura y la evidencia de fallos conservada.

El [flujo Mobile Real Applications](../../.github/workflows/mobile-real-apps.yml) usa aplicaciones públicas fijadas en el [manifiesto del corpus](../../scripts/mobile_real_apps.json). Su control de aceptación exige inventarios independientes de todos los DEX de cada APK y de todos los Mach-O de cada paquete iOS completo, reconstrucción independiente de los originales y del código generado, y comparaciones de comportamiento. Las fases ausentes, la cobertura desconocida del inventario o la ausencia de casos obligatorios hacen fallar el control. Las fases de recompilación y comportamiento de aplicaciones reales siguen incompletas, por lo que el soporte conserva la etiqueta Experimental; superar las pruebas de protección del banco no demuestra éxito con aplicaciones reales.

Los casos Android intentan compilar todo el conjunto inventariado de Java generado con javac y D8, usando únicamente declaraciones del SDK Android. El bytecode original de la aplicación, las implementaciones de dependencias y los stubs de sustitución no pueden suplir la recuperación ausente. La recuperación parcial y los errores de compilación permanecen en la evidencia; compilar correctamente sigue requiriendo verificación independiente, reconstrucción completa del APK y comparación de comportamiento en ART. El oráculo de iOS concilia los registros de métodos Objective-C en disco con la salida de herramientas Apple, conservando las identidades de clase, metaclase, categoría, lista y ordinal. Los punteros sin resolver o las posiciones omitidas mantienen el inventario como desconocido. Los inventarios separados de declaraciones de SDK para dispositivos y simuladores apoyan la importación de frameworks sin tratar las cabeceras como prueba de disposición o comportamiento de instancias.
