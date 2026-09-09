**Idiomas**: [English](../android.md) | [简体中文](../zh-CN/android.md) | [繁體中文](../zh-TW/android.md) | [日本語](../ja/android.md) | [한국어](../ko/android.md) | [Français](../fr/android.md) | [Deutsch](../de/android.md) | [Español](android.md) | [Italiano](../it/android.md) | [Русский](../ru/android.md) | [العربية](../ar/android.md)

# Recuperación de Java para Android

[← Índice de documentación](README.md)

`neverd mobile` recupera Java legible de APK, DEX y smali con el motor integrado de NeverD de forma predeterminada. Los lectores, implementados de manera independiente, comparten un modelo Dalvik tipado y un generador Java con trabajo acotado. Esta función experimental de CLI no promete equivalencia funcional con JADX ni la recuperación completa de cualquier APK. Los contenedores APK y la salida Java no están disponibles mediante el SDK C nativo, el SDK de plugins Python, el cargador gráfico ni `neverd decompile --language`.

El Java recuperado es una reconstrucción del bytecode. No se pueden recuperar los comentarios originales, el formato, las decisiones propias del lenguaje de origen ni los identificadores eliminados; el bytecode de Kotlin también produce Java. Una ejecución correcta no demuestra equivalencia semántica ni garantiza que todos los métodos se puedan recompilar. Este proceso no ejecuta ninguna aplicación analizada.

## Inicio rápido

Después de preparar los entornos de ejecución indicados a continuación, elija un directorio de salida nuevo:

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Abra `recovered-app/sources/` para leer los archivos Java y `recovered-app/report.json` para consultar el inventario de entradas y las limitaciones. Es preferible usar un directorio de entrada cuando las clases smali hacen referencia unas a otras.

## Preparación de los entornos de ejecución

| Componente | Requisito | Selección |
|------------|-----------|-----------|
| NeverD | Compile el objetivo `neverd` con una cadena compatible con C++20. El flujo móvil está integrado en la CLI nativa y no invoca un intérprete de Python. Distribuya el ejecutable con las bibliotecas nativas que requiera su compilación. | `build/bin/neverd` / PATH |

El motor predeterminado está implementado en C++20 y no necesita Python, Java ni JADX en tiempo de ejecución. Admite declaraciones y operaciones habituales representables de DEX 035, 037–040 y smali. DEX 041, las llamadas dinámicas como `invoke-custom`, algunas rutas de inicialización, las anotaciones semánticas u operaciones desconocidas y los identificadores que Java no puede expresar fallan explícitamente. Aceptar un formato no implica admitir todas sus instrucciones y declaraciones.

La resolución de nombres Java distingue entre la cabecera y el cuerpo de una clase y puede resolver casos conocidos de ocultación de nombres dentro del mismo paquete; rechaza explícitamente los casos en que faltan declaraciones de superclases o interfaces externas y no se pueden determinar los tipos o las referencias del código auxiliar Java generado, y solo para `java.lang.Object` se supone, sin disponer de su declaración, que no aporta tipos miembro heredables. Los literales de coma flotante de smali se redondean directamente a la precisión simple o doble de destino, conservando los patrones de bits resultantes.

### Linux y macOS

```sh
cmake --build build --target neverd

./build/bin/neverd mobile app.apk -o recovered-app
```

Ni `NEVERD_JADX` ni un ejecutable `jadx` en PATH seleccionan el motor externo: solo lo hace un `--jadx PATH` explícito. No hay cambio automático a otro motor. Ponga entre comillas las rutas con espacios.

### PowerShell de Windows

```powershell
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app
```

Las compilaciones de varias configuraciones pueden colocar el ejecutable en `build/bin/Release/`. Siga los requisitos habituales de distribución de bibliotecas nativas de esa compilación.

## Entradas admitidas y límites

| Entrada | Comportamiento | Límite importante |
|---------|----------------|-------------------|
| `.apk` | Valida todo el ZIP y analiza juntos `classes.dex`, `classes2.dex` y los siguientes archivos DEX numerados de la raíz | Solo código; no decodifica recursos ni el manifiesto |
| `.dex` | Validación y análisis de DEX 035 o 037–040 con el lector integrado | DEX 041 y las declaraciones u operaciones no admitidas fallan; renombrar o truncar un archivo no produce bytecode válido |
| `.smali` | Analiza la clase proporcionada | No carga implícitamente las clases del mismo conjunto a las que se hace referencia |
| Directorio smali | Recopila los archivos `.smali` de forma recursiva y los analiza juntos | Incluya las clases anidadas y las raíces smali dependientes en el directorio de entrada |

Para analizar un árbol extraído de un APK que contenga `smali/` y `smali_classes2/`, pase el directorio común que los contiene. Solo los archivos `.smali` llegan al backend, pero primero se valida y copia todo el árbol proporcionado; los recursos grandes no relacionados también cuentan para los límites de entrada. Un directorio reducido que contenga únicamente las raíces smali pertinentes disminuye el trabajo.

Los APK divididos son entradas separadas. Cada APK que contenga DEX se puede procesar por separado, pero este comando no combina un conjunto de APK; los fragmentos que solo contienen recursos fallan porque no tienen DEX en la raíz. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat` y `.vdex` no se aceptan como entradas móviles. Que el backend admita alguno de estos formatos no significa que este comando de NeverD lo admita.

Los recursos del APK, `AndroidManifest.xml`, los assets, las bibliotecas JNI/nativas y el código descargado durante la ejecución no se recuperan como Java. Extraiga por separado una biblioteca nativa `.so` y use `neverd decompile library.so -o library.c`. Las cargas cifradas o empaquetadas deben estar disponibles previamente como DEX/smali convencional para este proceso estático; no se desempaquetan cargas protegidas, no se conecta a dispositivos ni se eluden protecciones.

## Opciones y prioridad

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --timeout=600 --max-files=30000 --max-bytes=4294967296 --json
```

| Opción | Valor predeterminado | Significado |
|--------|----------------------|-------------|
| `-o DIRECTORY` | Obligatorio | Directorio de salida nuevo, fuera de cualquier directorio de entrada; nunca sobrescribe una salida existente |
| `--platform=auto\|android` | `auto` | Selecciona Android explícitamente o deduce la plataforma de la entrada |
| `--jadx PATH` | Sin definir: motor integrado | Selecciona explícitamente el adaptador de compatibilidad JADX instalado por separado; no se selecciona por el entorno ni se usa como alternativa automática |
| `--timeout N` | `300` | Presupuesto de tiempo positivo del análisis integrado; segundos por proceso externo, incluida la consulta de versión |
| `--max-files N` | `20000` | Límite positivo de entradas, incluidos los directorios creados |
| `--max-bytes N` | `2147483648` | Límite positivo de bytes para la entrada, los datos extraídos y la salida final |
| `--json` | Desactivado | Imprime el informe como JSON en lugar de un resumen para personas |

Una selección de `--arch` distinta de la predeterminada, `--artifact`, `--metadata-only` y un `--max-func` distinto de cero pertenecen a iOS y se rechazan en Android; se acepta `--arch=auto` explícito. No se reenvían opciones arbitrarias al backend. El adaptador JADX explícito aísla los directorios de configuración, caché y temporales, sin importar ajustes ambientales del backend ni configuración de plugins.

Las entradas, los datos extraídos y la salida final conservan los presupuestos de archivos y bytes. Los lectores y el generador integrados también comprueban el trabajo acotado y el tiempo transcurrido. Los espacios de trabajo externos permiten hasta tres veces los presupuestos de entradas y bytes para alojar entradas preparadas y resultados intermedios; los registros se limitan a 16 MiB por proceso. Son controles de recursos, no un entorno aislado de seguridad. Aumentar un límite no desactiva los demás.

## Estructura de salida e informe JSON

```text
recovered-app/
  sources/                       paquetes y clases Java recuperados
  metadata/android-methods.json  cobertura de métodos del motor integrado
  report.json                    inventario versionado y límites
```

Se eliminan las entradas temporales. Las clases anidadas pueden compartir el archivo de su clase contenedora, por lo que el número de archivos Java no equivale al de clases DEX. Los métodos generados pueden usar un bucle de despacho Java; no ejecutan el DEX original ni lo llaman mediante un puente de ejecución.

El informe integrado incluye `android_method_recovery`, cuyo contenido también se escribe en `metadata/android-methods.json`, y conserva cada método original. Debe cumplirse `method_count = recovered_method_count + projected_method_count + declaration_only_method_count + unrecovered_method_count`; si falta `projected_method_count`, vale cero, y `unrecovered_method_count` sigue siendo cero antes de publicar. Los métodos originales `native` y `abstract` tienen estado `declaration-only` y no cuentan como cuerpos recuperados.

Un subconjunto de clases locales con nombre y sin capturas puede emitirse dentro de su método estático contenedor exacto. Requiere un método ordinario con tipos escalares, una clase sin campos que herede directamente de `Object`, un constructor real sin argumentos, métodos de instancia escalares y usos de objetos verificados que no escapen del ámbito admitido. Las clases anónimas, capturas, modificadores no admitidos y usos no demostrados siguen fallando explícitamente.

Los métodos locales y el método contenedor reciben `source-projected`, con `projection_kind: "named-method-local"`; la cobertura permanece en `partial` aunque el informe general indique `success`. Los nombres binarios y los indicadores de acceso tras la recompilación siguen sin verificarse. El compilador Java puede elegir otro nombre binario, por lo que `class_source_bindings` conserva la clase original, el método contenedor exacto, la ruta fuente y el nombre local con `binary_name_status: "unverified"`.

`generated_source_helpers` enumera los métodos adicionales con los tipos exactos `throw-helper`, `constant-helper`, `default-constructor` y `field-initializer`. El último identifica un `<clinit>` generado adicional que no figuraba en el inventario de métodos originales. Estos métodos no se incluyen en el total original. Compilar correctamente o coincidir los nombres una vez no demuestra recuperación completa. El siguiente ejemplo abreviado no contiene métodos proyectados:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {
    "name": "neverd",
    "version": "1",
    "execution": "builtin"
  },
  "input_code_files": [
    "classes.dex",
    "classes2.dex"
  ],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": [
    "sources/example/Main.java",
    "sources/example/Peer.java"
  ],
  "logs": [],
  "android_method_recovery": {
    "schema_version": 1,
    "status": "recovered",
    "class_count": 2,
    "method_count": 6,
    "recovered_method_count": 5,
    "declaration_only_method_count": 1,
    "unrecovered_method_count": 0
  }
}
```

`input_kind` puede ser `apk`, `dex`, `smali` o `smali-directory`. `input_code_files` enumera los nombres del bytecode de entrada o las rutas smali, mientras que `java_sources` y `logs` son rutas relativas a la raíz de salida. `source` es el nombre base de la entrada. Los informes reales incluyen otras limitaciones de reconstrucción; consérvelas cuando presente resultados a otras herramientas.

Para automatizar el proceso, compruebe el código de salida antes de consumir `status` y guarde el informe fuera del nuevo directorio de salida al redirigir stdout:

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

La CLI nativa devuelve cero si tiene éxito y un valor distinto de cero si falla la recuperación. Con `--json`, los errores controlados incluyen `schema_version`, `status: "error"` y `error`. El análisis de argumentos, los fallos de inicio del ejecutable o las bibliotecas nativas y las interrupciones pueden notificarse solo por stderr. Compruebe primero el estado de salida.

## Gestión de errores y resolución de problemas

La publicación es transaccional: se conserva la salida existente y se eliminan los resultados temporales fallidos. Las operaciones no admitidas, los flujos de registros sin resolver, las declaraciones no representables, el manejo de excepciones mal formado y los presupuestos agotados hacen fallar el motor integrado, en lugar de publicar cuerpos ausentes. El adaptador externo también rechaza salidas distintas de cero, errores registrados de ensamblado o descompilación, omisiones de clases duplicadas, marcas de código incompleto, archivos Java vacíos y ausencia de Java. Una recuperación correcta no prueba equivalencia semántica.

| Síntoma | Acción |
|---------|--------|
| DEX, instrucción, declaración o inicialización no admitidos | Leer el diagnóstico y revisar el subconjunto admitido. Usar `--jadx PATH` solo al elegir deliberadamente el adaptador independiente |
| Entrada inválida o clase duplicada | Corregir el bytecode o conjunto de clases; los cuerpos no admitidos no se omiten en silencio |
| Tiempo o presupuesto excedido | Reducir la entrada o ajustar `--timeout`, `--max-files` y `--max-bytes` según los recursos disponibles |
| La salida ya existe | Elegir un directorio de salida nuevo |

## Adaptador opcional de compatibilidad JADX

`--jadx PATH` selecciona JADX externo, no la implementación integrada. Instale JADX 1.5.6 o posterior con los plugins estándar de entrada DEX/smali y Java 11 o posterior. Obtenga la [distribución completa de JADX](https://github.com/skylot/jadx/releases/tag/v1.5.6), conserve su estructura `bin/` y `lib/` y, al redistribuirla, las licencias de dependencias incluidas. No se descarga nada automáticamente. El informe del adaptador identifica el motor real `jadx` y la versión detectada; no afirma ofrecer la cobertura de métodos del motor integrado.

En Windows, indique el lanzador `.bat`/`.cmd` de la distribución o `lib/jadx-*-all.jar`. NeverD localiza el JAR e invoca Java directamente; las rutas de la aplicación no pasan por un intérprete de comandos. `JAVA_HOME` o PATH seleccionan Java. Las ejecuciones correctas conservan `logs/jadx-version.log` y `logs/jadx.log`; los directorios temporales fallidos y sus registros se eliminan. Solo una salida no nula del backend incluye un tramo final acotado del registro. Los fallos de inicio, tiempos agotados y presupuestos excedidos tienen sus propios diagnósticos.

```sh
neverd mobile app.apk -o recovered-jadx --jadx /opt/jadx/bin/jadx
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

## Verificación y alcance del soporte

Python solo se utiliza en los scripts de prueba de desarrollo siguientes; la recuperación móvil integrada se ejecuta en la CLI nativa C++20.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
```

Las pruebas de componentes y CLI comprueban el análisis, los contratos de salida y la limpieza tras fallos. El ejecutor interno de comparación usa un JDK (`java` y `javac`) y D8 para crear ejemplos DEX/APK independientes y compilar y ejecutar el Java recuperado. Son dependencias de prueba, no requisitos de la recuperación integrada. Ejecútelo con la compilación actual y examine sus resultados antes de declarar verificado un caso. El ejecutor de compatibilidad separado necesita además JADX y comprueba ese adaptador. El éxito de ejemplos no demuestra la recuperación completa de cualquier aplicación.

Consulte la [descripción general móvil](../mobile.md) para el flujo relacionado de iOS.
