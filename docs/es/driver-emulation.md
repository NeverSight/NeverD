**Idiomas**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← Índice de documentación](README.md)

# Emulación de controladores de Windows

El emulador opcional de controladores de NeverD ejecuta el punto de entrada PE
de un controlador WDM x64 compatible y, opcionalmente, recorre un escenario
explícito de solicitudes síncronas antes de descargarlo. Utiliza Unicorn para
ejecutar la CPU y el modelo acotado de Windows propio de NeverD. No carga el
controlador en el kernel del host ni reenvía las llamadas de API del invitado
a los servicios del sistema operativo anfitrión.

## Compilar y ejecutar

La función requiere activación explícita y es independiente de `BUILD_TESTING`:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_ENABLE_DRIVER_EMULATION=ON
cmake --build build-release --target neverd --parallel 4
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --instruction-limit 100000 > driver-report.json
```

El informe siempre se emite como JSON en stdout; los diagnósticos de solicitudes
y preparación van a stderr. Los límites predeterminados son 100000 instrucciones
del invitado, 64 MiB de memoria del invitado, 10000 eventos registrados y 5000
milisegundos. El límite de instrucciones debe ser positivo. Agotar un presupuesto
detiene la ejecución y conserva las observaciones parciales.

| Código de salida | Significado |
|------------------|-------------|
| `0` | La inicialización y todas las operaciones solicitadas y completadas tuvieron éxito |
| `1` | Entrada/opciones no válidas, fallo de preparación o función desactivada al compilar |
| `2` | La inicialización o una solicitud completada devolvió un `NTSTATUS` de error |
| `3` | La ejecución se detuvo antes de completar el escenario, por ejemplo por una API no compatible, un fallo o un límite de presupuesto |

Un estado de error devuelto constituye una observación completada de esa
operación. Un retorno satisfactorio solo describe esta ejecución modelada;
no demuestra que el controlador funcione en Windows.

## Compatibilidad de controladores

La compatibilidad depende de la ruta de código ejecutada y sus dependencias,
no de la extensión `.sys`. La evidencia actual de aceptación cubre fixtures
originales independientes y las rutas con búfer, in-direct y out-direct del
ejemplo WDM SIOCTL de Microsoft, incluida su compilación con registros de
depuración. No demuestra compatibilidad con controladores arbitrarios de terceros.

| Clase de controlador o requisito | Alcance actual | Entorno que falta |
|---------------------------------|----------------|-------------------|
| Controlador WDM de software x64 que utiliza las API enumeradas | Inicialización y ciclos de vida síncronos de archivos | Cada API adicional ejecutada requiere un modelo definido |
| IOCTL `METHOD_BUFFERED` | Compatible, con identidades de archivo independientes y solicitudes intercaladas | La finalización asíncrona no está disponible |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | MDL propios de cada solicitud y asignaciones de memoria del sistema | MDL asignados por el controlador, identidades de páginas físicas, DMA y asignaciones de memoria de usuario |
| READ/WRITE síncronos | Con búfer o directos según los indicadores del dispositivo | E/S neither, selección implícita de la posición de archivo y finalización asíncrona |
| `METHOD_NEITHER` | Rechazado | Contexto de direcciones de usuario, comprobación de accesos y gestión de excepciones del invitado |
| Controlador KMDF / UMDF | No compatible | Vinculación al framework, objetos, colas, callbacks y entorno de ejecución anfitrión adecuado |
| Controlador PnP de bus, función o filtro | La inicialización puede ejecutarse dentro del subconjunto de API; no se admite el ciclo de vida de la pila de dispositivos | Conexión de dispositivos, despacho al controlador inferior, IRP PnP y de energía |
| Controladores de almacenamiento, red, pantalla, sistema de archivos y minifiltros | Contratos de subsistemas no compatibles | Frameworks de puerto/clase/miniport, NDIS/WFP, servicios gráficos o del sistema de archivos |
| Controlador con hilos de trabajo, temporizadores, DPC, APC, esperas o cancelaciones | No compatible | Planificación, transiciones IRQL, sincronización y responsabilidad asíncrona sobre los recursos |
| Controlador con callbacks de proceso/hilo, handles, operaciones de registro/archivo o descubrimiento de módulos del kernel | No compatible fuera de las API enumeradas | Administrador de objetos, estado del sistema y productores de callbacks/eventos |
| Controlador de hardware, DMA, PCI, interrupciones o virtualización | Entorno no compatible | Modelos de dispositivos, memoria física, buses, interrupciones y estado privilegiado de CPU |
| Controlador de Windows x86 o ARM64 | Rechazado | Carga, ABI y modelo de ejecución específicos de la arquitectura |
| Imagen x64 que requiere CFG, configuración de carga no compatible, TLS u otras funciones PE rechazadas | Rechazada durante la carga | Semántica explícita del cargador y del entorno de ejecución para esos requisitos |

Una importación no compatible que no se utilice puede permanecer vinculada.
Al alcanzar una operación no compatible, la ejecución se detiene con un
diagnóstico y las observaciones recopiladas hasta entonces. El éxito de
DriverEntry por sí solo no demuestra que se admitan las rutas posteriores de
despacho, hardware o framework. La siguiente tabla de API define el subconjunto
compatible de referencia.

## Contrato de ejecución

El perfil modela un único ciclo de vida WDM x64 de un solo hilo en
`PASSIVE_LEVEL`. La ejecución comienza en el punto de entrada PE y conserva
el wrapper de entrada del compilador, si existe. DriverEntry debe devolver
`STATUS_SUCCESS` para inicializar; cualquier otro estado satisfactorio no nulo
o pendiente detiene la ejecución como contrato de inicialización no compatible.
Un estado de error se conserva como resultado de inicialización completado.
Todos los objetos, cadenas, pilas, punteros de función y asignaciones residen
en memoria del invitado. El modelo proporciona un `DRIVER_OBJECT` y una ruta
del registro para el nombre de servicio configurado (`NeverDDriver` por defecto).
El adaptador utiliza el modo TLB virtual de Unicorn para conservar las direcciones
virtuales del invitado, incluidas las direcciones canónicas altas del kernel,
sin sintetizar tablas de páginas de Windows. El valor inicial de RFLAGS es
`0x202`; el perfil de dispositivo de software utiliza una línea de caché fija
de 64 bytes. Son propiedades explícitas de este escenario de ejecución.
Las lecturas x64 de CR8 en línea observan el mismo `PASSIVE_LEVEL`; las escrituras
de CR8 y otras operaciones de registros de control siguen sin estar admitidas.

Las importaciones desconocidas se vinculan a trampas que se activan al usarlas.
Una importación no utilizada no impide ejecutar; ejecutar su thunk o leer un
valor de datos exportado no modelado detiene la ejecución con `unsupported_api`.
Los efectos no compatibles del entorno de CPU también provocan una detención
explícita. NeverD no sustituye llamadas sin implementar por valores de éxito.
Las imágenes malformadas o los requisitos de carga no compatibles fallan antes
de la ejecución.

Este perfil no implementa un kernel completo de Windows, un runtime KMDF,
un ciclo de vida PnP/energía, IRP asíncronos o pendientes, IOCTL neither,
interrupciones ni planificación multihilo. Los callbacks se ejecutan solo
cuando el escenario los solicita explícitamente; la inicialización por sí sola
sigue deteniéndose después de DriverEntry.

Las imágenes utilizan su base preferida salvo que el escenario seleccione una
dirección de reubicación válida, y deben ser ejecutables PE32+ x64 con el
subsistema nativo. Las importaciones pueden proceder de `ntoskrnl.exe` o
`ntkrnlmp.exe`. El cargador de ejecución admite reubicaciones de base x64
`DIR64` validadas y una configuración de carga limitada para la cookie de
seguridad, inicializada antes del wrapper de entrada con una cookie determinista
del invitado. Se rechazan CFG y otros campos no modelados de configuración de
carga, TLS, importaciones diferidas/vinculadas, importaciones por ordinal e
imágenes administradas. Las imágenes también deben superar controles estrictos
de rangos y alineación.

El modelo inicial de API tiene deliberadamente un contrato limitado:

| API | Comportamiento modelado y restricciones |
|-----|----------------------------------------|
| `RtlInitUnicodeString` | Construye una `UNICODE_STRING` del invitado para una fuente acotada terminada en NUL |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | Copia UTF-16 de longitud explícita y comparación sensible a mayúsculas; la comparación sin distinguir mayúsculas necesita una tabla de conversión de Windows y detiene la ejecución |
| `ExAllocatePool2` | Asignaciones NX paginadas/no paginadas, inicializadas a cero por defecto; se modelan los indicadores de memoria sin inicializar y de alineación a la caché; los indicadores requeridos inválidos devuelven NULL; los pools con cuotas/ejecutables y las excepciones de asignación detienen la ejecución |
| `MmGetSystemRoutineAddress` | Resuelve un nombre del invitado de longitud explícita mediante el inventario compartido de exportaciones |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | MDL propios de cada solicitud, asignaciones de memoria del sistema KernelMode con caché, permisos y duración explícitos; se reutilizan las asignaciones seguras existentes |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Asignaciones de datos para los tipos de pool `0`, `1` y `512`; tamaño/tag positivos, tags coincidentes al liberar con tag y sin reutilización de direcciones |
| `IoCreateDevice`, `IoDeleteDevice` | Tipo de dispositivo `0x22`, características `0` o `0x100`, extensiones acotadas y nombres ASCII `\Device\Name` |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` o `\??\Name` dentro de un espacio de nombres de sesión, con destino `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Formato variádico Win64 verificado, con un máximo de 512 bytes de salida; todos los filtros del depurador habilitados |
| `IoGetCurrentIrpStackLocation` | Devuelve la ubicación de pila de la IRP modelada activa; las macros WDM compiladas normales leen el mismo campo del invitado |
| `KeGetCurrentIrql` | Devuelve `PASSIVE_LEVEL` |
| `IofCompleteRequest`, `IoCompleteRequest` | Completa la IRP síncrona modelada activa con `IO_NO_INCREMENT`; no se puede volver a acceder a una IRP completada ni a su búfer |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Operaciones acotadas sobre búferes del invitado, como máximo 1 MiB por llamada; las API de copia sin solapamiento rechazan los solapamientos |

El formato de `DbgPrint` admite enteros `d/i/u/o/x/X`, punteros `p`, texto
`s/c`, `%%`, Unicode de longitud explícita `wZ/lZ`, cadenas anchas `ls/ws`,
indicadores, anchura/precisión incluido `*` y modificadores de longitud de enteros
de Windows. Se leen como máximo 32 argumentos variables y 1024 bytes de formato.
La anchura y la precisión se limitan a 512. Los flotantes, `%n`, las combinaciones
desconocidas y las conversiones de texto no ASCII detienen explícitamente la
ejecución; el modelo no adivina una página de códigos de Windows ni llama al
printf del anfitrión con datos del invitado.

La estructura RegistryPath original y su búfer caducan cuando DriverEntry
retorna. Los controladores que necesiten la cadena más adelante deben copiarla
durante la inicialización.

La arena de objetos/pool es de 1 MiB. Los bytes de pool sin inicializar tienen
el contenido determinista `0xCD`; los bytes liberados contienen `0xDD`. Es un
escenario de ejecución concreto. Los accesos de CPU y las API modeladas de
búferes rechazan las asignaciones de pool liberadas, los dispositivos eliminados,
los bytes no asignados de la arena, los campos opacos de objetos y las escrituras
en campos de objetos de solo lectura. Estas comprobaciones cubren las duraciones
de los objetos del modelo; no constituyen un análisis general de seguridad de
memoria de controladores. Las entradas no escritas de la tabla de despacho
indican cero como «no registrado». Una solicitud del escenario para una función
principal no registrada se completa mediante el manejador predeterminado
modelado con `STATUS_INVALID_DEVICE_REQUEST`; el fallo sigue visible en los
estados de despacho y E/S. El modelo no inventa una dirección de función del
invitado para ese manejador, y las lecturas del invitado de una entrada no
escrita siguen sin estar admitidas. Registrar explícitamente un callback nulo
es un error.

## Escenarios de solicitudes

Pase un archivo JSON con `--scenario` para seleccionar las solicitudes y la
descarga opcional:

```bash
build-release/bin/neverd emulate-driver path/to/driver.sys \
  --scenario scenario.json > driver-report.json
```

Para un controlador que crea `\Device\NeverDIO` y acepta el IOCTL con búfer
`0x222000`, un ejemplo de `scenario.json` es:

```json
{
  "requests": [
    {"kind": "create", "device": "\\Device\\NeverDIO"},
    {"kind": "ioctl", "code": "0x222000", "input": "00112233", "output_size": 4},
    {"kind": "cleanup"},
    {"kind": "close"}
  ],
  "unload": true
}
```

El nombre del dispositivo y el código IOCTL deben coincidir con el controlador.
En create, omitir `device` selecciona el único dispositivo activo; una selección
ambigua falla. Las solicitudes posteriores usan el dispositivo de su archivo,
a menos que se indique un nombre explícito coincidente. El campo opcional `file`
es una identidad de escenario sin signo de 32 bits, con valor cero por defecto.
Cada identidad tiene sus propios FILE_OBJECT y FsContext y exige create,
transferencias, cleanup y close en ese orden. Las solicitudes de archivos
independientes pueden intercalarse. Los dispositivos exclusivos rechazan una
segunda apertura. Estas identidades representan objetos de archivo, no handles
duplicados. Se admiten el método IOCTL con búfer y ambos métodos directos.
El despacho debe completar cada IRP de forma síncrona; devolver `STATUS_PENDING`,
no completarlo, indicar longitudes de salida inválidas o acceder a un IRP ya
completado provoca un fallo explícito. La descarga solicitada no debe dejar
ningún dispositivo, enlace simbólico, asignación de pool u objeto de archivo activo.

El campo raíz opcional `"load_address": "0x190000000"` solicita cambiar la base;
su omisión o `"0x0"` utiliza la dirección preferida. La imagen debe cumplir los
requisitos de reubicación. El comando de inicialización y la API C originales
no implican ningún escenario.

En la raíz solo se aceptan `load_address`, `requests`, `unload` y
`kernel_exports`. Todas las solicitudes aceptan `kind`, un `device` opcional y
un `file` opcional. Los IOCTL requieren `code` y aceptan `input`, `output_size`
y `direct_input`. Un `read` acepta `output_size` y `byte_offset`; un `write`
acepta `input` y `byte_offset`. Los desplazamientos valen cero por defecto,
aceptan enteros o cadenas hexadecimales y deben caber en un valor con signo de
64 bits no negativo. Las solicitudes de ciclo de vida rechazan los campos de
transferencia. Se rechazan los campos desconocidos o duplicados.
`code` acepta un entero JSON sin signo de 32 bits o una cadena hexadecimal `0x`.
`input` es una cadena hexadecimal de bytes de longitud par sin prefijo ni
espacios; omitirlo significa una entrada vacía. `output_size` es un entero JSON
sin signo; omitirlo significa cero. Se rechazan fracciones numéricas y
notaciones de coma flotante.

Para IOCTL directos, `input` inicializa el primer búfer del sistema y
`direct_input` inicializa el segundo búfer independiente descrito por el MDL,
rellenado con ceros hasta `output_size`. `METHOD_IN_DIRECT` requiere acceso de
lectura; no implica una asignación de memoria del sistema de solo lectura.
Ambos métodos usan búferes de escenario con lectura/escritura.
`MdlMappingNoWrite` elimina el permiso de escritura de la asignación y
`MdlMappingNoExecute` elimina el de ejecución. Desasignar revoca la dirección
virtual del sistema; volver a asignar conserva los mismos datos bloqueados.
La finalización caduca el MDL y la asignación. Se modelan los campos públicos
del MDL usados por macros WDM; se rechazan los campos de proceso/PFN, los MDL
construidos a mano, las asignaciones de usuario y el acceso directo mediante
el UserBuffer sin procesar. Un búfer directo de longitud cero tiene un MDL nulo.

Para READ/WRITE, `DO_BUFFERED_IO` o `DO_DIRECT_IO` selecciona el método de
transferencia. La ausencia de ambos indicadores o su conflicto detiene la
ejecución. Information se comprueba frente a la longitud de transferencia;
las escrituras devuelven un recuento y las lecturas devuelven bytes.

`kernel_exports` asocia nombres de rutinas con booleanos de disponibilidad
explícitos, por ejemplo `"kernel_exports": {"OptionalRoutine": false}`. Las
exportaciones modeladas y las importaciones estáticas reciben direcciones
estables compartidas con `MmGetSystemRoutineAddress`. Una exportación
explícitamente ausente se resuelve a NULL y no puede satisfacer una importación
estática. Una exportación declarada presente sin modelo de API se resuelve a
una trampa que se activa al llamarla. Un nombre dinámico desconocido detiene
la ejecución con un diagnóstico de disponibilidad no especificada; nunca se
infiere la ausencia a partir de una implementación inexistente. Los nombres
son ASCII imprimible de longitud acotada y la resolución distingue mayúsculas.
El inventario es una propiedad concreta del escenario y no pretende coincidir
con todas las versiones de Windows.
`IoGetCurrentIrpStackLocation` y `MmGetSystemAddressForMdlSafe` son funciones auxiliares modeladas de las cabeceras WDM; esto no las declara exportadas de forma predeterminada, por lo que su disponibilidad como exportaciones requiere una importación estática o una declaración explícita en `kernel_exports`.

El texto del escenario se limita a 2 MiB, con un máximo de 64 solicitudes,
65536 bytes por búfer de entrada o salida y 512 KiB de bytes solicitados en
total, incluido el contenido de `direct_input`. Los presupuestos de instrucciones,
observaciones, memoria del invitado y tiempo se aplican a todo el escenario.
La arena de 1 MiB también contiene objetos y metadatos, por lo que una imagen
puede agotar la memoria del modelo antes de consumir el máximo de los búferes
del escenario.

## Comprobación de aceptación con el ejemplo de Microsoft

El [script de validación](../../scripts/validate_windows_driver_sample.py),
de ejecución voluntaria, descarga el código fuente SIOCTL de Microsoft en la
revisión fijada en el
[manifiesto de validación](../../unittests/emulation/fixtures/sioctl-validation.json),
verifica hashes SHA-256 y compila el código sin modificar con las cabeceras DDK
de MinGW-w64. Conserva la licencia y procedencia originales, los comandos de
compilación, el escenario y los informes en el directorio de salida seleccionado.
Requiere acceso a la red, Clang, `lld-link`, `nm` y las cabeceras DDK de MinGW-w64:

```bash
python3 scripts/validate_windows_driver_sample.py \
  --neverd build-release/bin/neverd \
  --output build-release/driver-validation/sioctl
```

Use `--headers` para un directorio de inclusión MinGW-w64 distinto del
predeterminado. El script genera una biblioteca de importación MS COFF a partir
de las dependencias del objeto compilado. La comprobación ejecuta escenarios
con búfer, in-direct y out-direct independientes mediante DriverEntry, create,
IOCTL, cleanup, close y unload. Añada `--debug` y seleccione otro directorio de
salida para compilar con `DBG=1` y verificar los mensajes del registro del
invitado. El ejemplo original no registra un manejador de cleanup; por ello,
el manejador predeterminado modelado completa cleanup con
`STATUS_INVALID_DEVICE_REQUEST` (`0xC0000010`). El controlador aun así cierra
y se descarga, y el IOCTL satisfactorio devuelve los bytes esperados. Para este
escenario completo, el código de salida esperado de la CLI es **2** y
`scenario_success` es false. El propio script solo tiene éxito cuando todos
esos resultados coinciden, incluido el fallo visible de cleanup; no reescribe
el ejemplo para ocultar ese resultado.

## Informes y SDK

El informe JSON distingue `stop_reason`, los campos que admiten null `nt_status`
y `nt_success`, el PC de detención y el recuento de instrucciones. Conserva las
llamadas de API y el estado observable recopilados antes de detenerse, incluidos
los objetos de dispositivo y las direcciones de callbacks del controlador.
Las direcciones del invitado son cadenas hexadecimales para que los consumidores
de JSON no pierdan precisión de 64 bits. El objeto `configuration` registra los
límites, el nombre de servicio y las sustituciones de `kernel_exports` de la ejecución. El perfil es
`wdm-x64-synchronous-v2`. `nt_status` sigue siendo el resultado de DriverEntry,
mientras que `scenario_success` describe conjuntamente la inicialización y las
solicitudes completadas. `phase`, `requests` y `unload_completed` identifican
las partes ejecutadas del ciclo de vida solicitado. Cada llamada de API y
escritura de CPU también registra su fase (`driver_entry`, `request:N` o
`unload`). Cada solicitud informa de los estados de despacho y E/S, finalización,
longitud de información y bytes devueltos en `output_hex`. `preferred_image_base`
describe la base PE original. `security_cookie` es la dirección del invitado
de la cookie inicializada, o `"0x0"` si no se requería. Los campos de solicitud
son `kind`, `device`, `file`, `byte_offset`, `code`, `irp`, `completed`, `dispatch_status`, `io_status`,
`information` y `output_hex`.

El objeto anulable `fault` conserva el primer fallo del backend. Sus campos
`kind`, `pc`, `address` anulable, `size`, `access` e `interrupt` distinguen memoria
sin asignar o protegida, rangos inválidos, instrucciones inválidas y excepciones
de CPU. Las direcciones usan cadenas hexadecimales; los tamaños y los vectores
de interrupción usan enteros. Las lecturas de observación no pueden reemplazar
el fallo original. Un backend con un fallo no puede reanudar la ejecución, y
este registro no implica gestión SEH del invitado.

`instructions` cuenta los intentos admitidos de instrucciones del invitado.
Una instrucción rechazada por la política de ejecución no se cuenta; una
instrucción admitida que provoca un fallo en la CPU sí. El despacho sintético
de API y el centinela de retorno no incrementan este contador.

Cada entrada de `writes` tiene `semantics: "attempted_guest_write"`: registra
un intento de escritura de CPU fuera de la pila, incluso si posteriormente
provoca un fallo o se detiene por un presupuesto. No garantiza que la escritura
se completara y no incluye escrituras realizadas por los modelos de API.
Las instantáneas de objetos de dispositivo y controlador describen el estado
observado cuando se detiene la ejecución.

Incluya `neverd/sdk/NeverDCAPIEmulation.h` (o la cabecera global de la API C),
cree una sesión y llame a `neverd_emulate_driver_json(session, path, options)`.
Una ruta explícita no vacía entra directamente en la validación estricta previa
a la ejecución, sin cargar antes mediante la API de análisis general. La CLI
utiliza esta vía. Pasar `NULL` en options selecciona los valores predeterminados.
Las opciones explícitas `neverd_driver_options_v1` requieren el valor exacto de
`struct_size` y presupuestos positivos de instrucciones, memoria, eventos y
tiempo. Libere el resultado con `neverd_free_string`.

Pasar `NULL` como ruta, en cambio, requiere una sesión cargada y vuelve a analizar
su archivo de forma independiente del análisis IR y de la carga restringida a
funciones. Ambas vías conservan la imagen de la sesión. Mantenga el archivo de
entrada disponible y sin cambios durante la llamada. Los fallos de solicitud o
preparación devuelven `NULL` y establecen `neverd_last_error`; las detenciones
de ejecución devuelven JSON. La API sigue disponible en las compilaciones con
la función desactivada e indica cómo activarla.

`neverd_emulate_driver_scenario_json(session, path, scenario_json, options)`
utiliza las mismas opciones v1 y reglas de propiedad, añadiendo una entrada
estricta de escenario. Se requiere una cadena JSON no NULL terminada en NUL.
La ABI original `neverd_emulate_driver_json` permanece intacta y limitada a
inicialización. El parser C++ `driverOptionsFromScenarioJSON` proporciona la
misma validación de escenarios a quienes llaman a `emulateDriver`.

El punto de entrada interno de C++ es `neverd::emulation::emulateDriver` en
`include/neverd/emulation/DriverSession.h`. El análisis del formato pertenece al
cargador existente; el comportamiento de objetos y API de Windows pertenece a
`lib/emulation/windows`; el estado y la ejecución de CPU pertenecen al adaptador
Unicorn. El adaptador y el modelo utilizan la misma interfaz de memoria del
invitado. Ningún comportamiento de API de Windows pertenece al fork de Unicorn.
