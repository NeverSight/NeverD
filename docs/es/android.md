**Idiomas**: [English](../android.md) | [简体中文](../zh-CN/android.md) | [繁體中文](../zh-TW/android.md) | [日本語](../ja/android.md) | [한국어](../ko/android.md) | [Français](../fr/android.md) | [Deutsch](../de/android.md) | [Español](android.md) | [Italiano](../it/android.md) | [Русский](../ru/android.md) | [العربية](../ar/android.md)

# Recuperación de Java para Android

[← Índice de documentación](README.md)

`neverd mobile` recupera Java legible a partir de APK, DEX y smali mediante un backend JADX instalado por separado. El proceso valida y prepara el bytecode, analiza juntas las clases relacionadas, comprueba los resultados y publica un directorio de código fuente con un informe legible por máquinas. Es una función experimental de la CLI. Los contenedores APK y la salida Java no están disponibles a través del SDK nativo de C, el SDK de plugins de Python, el cargador de la GUI ni `neverd decompile --language`.

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
| NeverD | Compile el objetivo `neverd`; distribuya también el directorio `mobile/` que lo acompaña | `build/bin/neverd` o un ejecutable en PATH |
| Python | Python 3.10 o posterior, independiente del host de plugins integrado | `--python`, después `NEVERD_PYTHON` y, por último, `python3`/`python` en PATH |
| Backend de Java | JADX 1.5.6 o posterior con los plugins estándar de entrada DEX y smali | `--jadx`, después `NEVERD_JADX` y, por último, `jadx` en PATH |
| Entorno de Java | Java 11 o posterior; se necesita un JDK para verificar mediante compilación y ejecución | `JAVA_HOME` o Java en PATH |

NeverD no descarga dependencias automáticamente. Obtenga la [distribución completa de JADX](https://github.com/skylot/jadx/releases/tag/v1.5.6), conserve su estructura `bin/` y `lib/` y mantenga las licencias incluidas si la redistribuye. La versión del backend probada es la 1.5.6; las versiones posteriores deben cumplir el mismo contrato de CLI. La preparación de estas dependencias es independiente de la compilación del pipeline LLVM de NeverD.

### Linux y macOS

```sh
cmake --build build --target neverd
python3 --version
java -version
/opt/jadx/bin/jadx --version

./build/bin/neverd mobile app.apk -o recovered-app \
  --python python3 --jadx /opt/jadx/bin/jadx
```

Para un uso frecuente, configure `NEVERD_JADX=/opt/jadx/bin/jadx` y, si lo desea, `NEVERD_PYTHON` con la ruta de un intérprete. Si Java aún no está disponible, apunte `JAVA_HOME` al directorio de instalación del JDK. Las rutas con espacios deben ir entre comillas.

### PowerShell de Windows

```powershell
$env:JAVA_HOME = 'C:\Tools\jdk'
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app `
  --python 'C:\Tools\Python\python.exe' `
  --jadx 'C:\Tools\jadx\bin\jadx.bat'
```

La ruta `.bat`/`.cmd` del backend permite localizar el único `lib/jadx-*-all.jar` de la distribución; NeverD invoca Java directamente. También puede pasar ese JAR a `--jadx`. Las rutas de las aplicaciones nunca se insertan en un intérprete de comandos. Las compilaciones con varias configuraciones pueden colocar el ejecutable en `build/bin/Release/`. Si mueve únicamente el ejecutable y omite su directorio `mobile/`, aparecerá un error de helper ausente.

## Entradas admitidas y límites

| Entrada | Comportamiento | Límite importante |
|---------|----------------|-------------------|
| `.apk` | Valida todo el ZIP y analiza juntos `classes.dex`, `classes2.dex` y los siguientes archivos DEX numerados de la raíz | Solo código; no decodifica recursos ni el manifiesto |
| `.dex` | Valida la firma mágica DEX y deja que el backend decodifique el contenido | Renombrar o truncar un archivo no lo convierte en bytecode válido |
| `.smali` | Analiza la clase proporcionada | No carga implícitamente las clases del mismo conjunto a las que se hace referencia |
| Directorio smali | Recopila los archivos `.smali` de forma recursiva y los analiza juntos | Incluya las clases anidadas y las raíces smali dependientes en el directorio de entrada |

Para analizar un árbol extraído de un APK que contenga `smali/` y `smali_classes2/`, pase el directorio común que los contiene. Solo los archivos `.smali` llegan al backend, pero primero se valida y copia todo el árbol proporcionado; los recursos grandes no relacionados también cuentan para los límites de entrada. Un directorio reducido que contenga únicamente las raíces smali pertinentes disminuye el trabajo.

Los APK divididos son entradas separadas. Cada APK que contenga DEX se puede procesar por separado, pero este comando no combina un conjunto de APK; los fragmentos que solo contienen recursos fallan porque no tienen DEX en la raíz. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat` y `.vdex` no se aceptan como entradas móviles. Que el backend admita alguno de estos formatos no significa que este comando de NeverD lo admita.

Los recursos del APK, `AndroidManifest.xml`, los assets, las bibliotecas JNI/nativas y el código descargado durante la ejecución no se recuperan como Java. Extraiga por separado una biblioteca nativa `.so` y use `neverd decompile library.so -o library.c`. Las cargas cifradas o empaquetadas deben estar disponibles previamente como DEX/smali convencional para este proceso estático; no se desempaquetan cargas protegidas, no se conecta a dispositivos ni se eluden protecciones.

## Opciones y prioridad

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --jadx /opt/jadx/bin/jadx --timeout=600 \
  --max-files=30000 --max-bytes=4294967296 --json
```

| Opción | Valor predeterminado | Significado |
|--------|----------------------|-------------|
| `-o DIRECTORY` | Obligatorio | Directorio de salida nuevo, fuera de cualquier directorio de entrada; nunca sobrescribe una salida existente |
| `--platform=auto\|android` | `auto` | Selecciona Android explícitamente o deduce la plataforma de la entrada |
| `--jadx PATH` | Entorno/PATH | Lanzador del backend o JAR de la distribución; la opción explícita tiene prioridad |
| `--python PATH` | Entorno/PATH | Intérprete del helper incluido; la opción explícita tiene prioridad |
| `--timeout N` | `300` | Segundos positivos por proceso del backend, incluida la consulta de versión |
| `--max-files N` | `20000` | Límite positivo de entradas, incluidos los directorios creados |
| `--max-bytes N` | `2147483648` | Límite positivo de bytes para la entrada, los datos extraídos y la salida final |
| `--json` | Desactivado | Imprime el informe como JSON en lugar de un resumen para personas |

`--arch` con un valor distinto de `auto`, `--artifact`, `--metadata-only` y un `--max-func` distinto de cero pertenecen a iOS y se rechazan para Android. El valor explícito `--arch=auto` sí se acepta para Android. No hay una opción para reenviar parámetros arbitrarios al backend. Los directorios de configuración, caché y archivos temporales del backend están aislados en cada ejecución; no se importan los ajustes externos del backend ni la configuración de plugins.

Los límites controlan los recursos; no constituyen un sandbox para el proceso del backend. También se supervisa el área de preparación, con espacio para la entrada, los datos extraídos y la salida, hasta tres veces los presupuestos configurados de entradas y bytes. Los registros están limitados a 16 MiB por proceso. Las entradas grandes pueden necesitar más memoria de heap de Java o un tiempo de espera mayor; aumentar un límite no desactiva los demás.

## Estructura de salida e informe JSON

```text
recovered-app/
  sources/                 paquetes y clases Java recuperados
  logs/jadx-version.log    consulta de versión del backend
  logs/jadx.log            diagnósticos del backend
  report.json              inventario versionado y límites de recuperación
```

Las copias temporales y las cachés del backend se eliminan. Los nombres exactos y el número de archivos Java dependen de la reconstrucción del backend; las clases anidadas pueden compartir el archivo fuente de su clase exterior. Por tanto, el número de archivos fuente Java no equivale al número de clases DEX.

Ejemplo abreviado de informe:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {"name": "jadx", "version": "1.5.6"},
  "input_code_files": ["classes.dex", "classes2.dex"],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": ["sources/example/Main.java", "sources/example/Peer.java"],
  "logs": ["logs/jadx-version.log", "logs/jadx.log"],
  "limitations": ["Java is reconstructed from bytecode; original comments, formatting, and stripped names cannot be restored."]
}
```

`input_kind` puede ser `apk`, `dex`, `smali` o `smali-directory`. `input_code_files` enumera los nombres del bytecode de entrada o las rutas smali, mientras que `java_sources` y `logs` son rutas relativas a la raíz de salida. `source` es el nombre base de la entrada. Los informes reales incluyen otras limitaciones de reconstrucción; consérvelas cuando presente resultados a otras herramientas.

Para automatizar el proceso, compruebe el código de salida antes de consumir `status` y guarde el informe fuera del nuevo directorio de salida al redirigir stdout:

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

Las ejecuciones correctas del helper devuelven cero. Los fallos de recuperación devuelven un valor distinto de cero; una vez iniciado el helper, `--json` produce un objeto de error que contiene `schema_version`, `status: "error"` y `error`. El análisis nativo de argumentos, la ausencia de Python, una versión de Python anterior a 3.10 o la falta del helper pueden provocar un fallo anterior que escriba en stderr en lugar de JSON. Las interrupciones también pueden notificarse por stderr. Los consumidores deben contemplar estos casos.

## Gestión de errores y resolución de problemas

La publicación es transaccional: se conserva la salida existente y se elimina la salida provisional fallida. El comando falla si el backend devuelve un código distinto de cero, registra errores de ensamblado o descompilación, omite clases duplicadas, incluye marcadores explícitos de código incompleto, produce archivos Java vacíos o no produce Java. Que el backend indique éxito no demuestra por sí mismo la corrección de cada método.

| Síntoma | Acción |
|---------|--------|
| Falta Python o el helper | Instale o seleccione Python 3.10+ y mantenga el directorio `mobile/` junto a NeverD |
| El backend no se ejecuta o su versión no es compatible | Verifique `--jadx`, la estructura completa de la distribución, Java y la versión mínima del backend |
| Cabecera DEX no válida o ausencia de DEX en la raíz | Compruebe el formato real de la entrada; use un APK que contenga código, DEX convencional o smali |
| No hay archivos smali | Seleccione un directorio con archivos `.smali`, no código fuente Java ni un árbol formado solo por assets |
| Clase duplicada o recuperación parcial | Elimine las definiciones duplicadas de entrada o analice por separado el conjunto de bytecode pertinente; corrija el smali mal formado en lugar de aceptar un resultado incompleto |
| Se supera el tiempo de espera o un límite de bytes o entradas | Use una entrada pertinente más pequeña o aumente deliberadamente el límite correspondiente |
| Ruta o enlace de archivo no seguro | Vuelva a crear una entrada convencional y portable, sin rutas que salgan del directorio, enlaces, archivos especiales ni rutas en conflicto |
| La salida ya existe | Elija otro directorio de salida; no reutilice el de una ejecución correcta anterior |

Las ejecuciones correctas conservan los registros del backend. Los directorios de preparación fallidos, incluidos sus registros, se eliminan; cuando el backend termina con un código distinto de cero, el error incluye una parte final acotada del diagnóstico. Los errores por tiempo de espera o por superar los límites de recursos tienen mensajes específicos. Para investigar un problema específico del backend, reprodúzcalo con una entrada aislada, la propia CLI del backend y un directorio de diagnóstico separado. Nunca deduzca que hubo éxito solo porque apareció algo de Java antes de un fallo.

## Verificación y alcance del soporte

```sh
cmake --build build --target check-neverd-mobile
NEVERD_BUILD_DIR=build python3 -m unittest discover -s scripts/tests -p 'test_mobile_*.py' -v
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

Los dos primeros comandos comprueban los contratos de los componentes móviles y de la CLI compilada; los requisitos de pruebas específicos de cada plataforma pueden generar omisiones explícitas. El ejecutor del backend real también necesita un JDK (`java` y `javac`). Construye casos de un solo archivo smali, clases relacionadas y anidadas en smali, DEX y APK con multidex real; después compila y ejecuta el Java recuperado. Los casos cubren bifurcaciones, bucles, arrays, gestión de excepciones, referencias entre clases, entradas mal formadas y omisiones de clases duplicadas. Esto aporta evidencia para esas pruebas, no promete recuperar por completo cualquier aplicación.

El [workflow Mobile Decompilation](../../.github/workflows/mobile.yml) ejecuta pruebas de componentes en Linux, macOS y Windows con Python 3.10 y 3.13, además de un trabajo del backend real de Android en Linux cuya distribución se fija mediante una suma de comprobación. Consulte la [visión general móvil](../mobile.md) para conocer el proceso independiente de iOS y sus límites actuales.
