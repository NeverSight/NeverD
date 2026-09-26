**Idiomas**: [English](../driver-emulation.md) | [简体中文](../zh-CN/driver-emulation.md) | [繁體中文](../zh-TW/driver-emulation.md) | [日本語](../ja/driver-emulation.md) | [한국어](../ko/driver-emulation.md) | [Français](../fr/driver-emulation.md) | [Deutsch](../de/driver-emulation.md) | [Español](driver-emulation.md) | [Italiano](../it/driver-emulation.md) | [Русский](../ru/driver-emulation.md) | [العربية](../ar/driver-emulation.md)

[← Índice de documentación](README.md)

# Emulación de controladores de Windows

El emulador opcional de controladores de NeverD ejecuta el punto de entrada PE
de un controlador WDM x64 compatible y, opcionalmente, recorre un escenario
explícito de solicitudes en serie antes de descargarlo. Utiliza Unicorn para
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
detiene la ejecución y conserva las observaciones parciales; las API de asignación mantienen sus contratos de retorno por falta de recursos, incluido NULL al crear mappings MMIO como se describe más abajo.

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
depuración, y el ejemplo público Zero de Pavel Yosifovich para READ/WRITE
directos y estadísticas. No demuestra compatibilidad con controladores
arbitrarios de terceros.

| Clase de controlador o requisito | Alcance actual | Entorno que falta |
|---------------------------------|----------------|-------------------|
| Controlador WDM de software x64 que utiliza las API enumeradas | Inicialización WDM x64 acotada, solicitudes seriales buffered/direct, trabajo, temporizadores, DPC, eventos y esperas, informes y límites | Cada API adicional ejecutada requiere un modelo definido |
| IOCTL `METHOD_BUFFERED` | E/S buffered/direct serial con finalización por trabajo o DPC | Solo las API siguientes; sin IRP simultáneos ni cancelación general de solicitudes WDM |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | MDL de cada solicitud, mappings de sistema e identidades PFN compartidas de solo lectura | Mappings de usuario y otras interfaces DMA |
| MDL asignados por el controlador | Descriptores de pool/usuario independientes o asociados a IRP; enlaces del invitado y liberación al completar | Cuotas, MDL manuales/parciales y mappings de usuario arbitrarios |
| READ/WRITE | E/S buffered/direct/neither serial con finalización por trabajo o DPC | Solo las API siguientes; sin IRP simultáneos, cancelación WDM general ni posición implícita del archivo |
| WDM `METHOD_NEITHER` | Búferes de usuario separados, sondeo, bloqueo MDL, identidades sintéticas, revocación de VA o salida tras el despacho y cancelación limitada | Sin asociación general de procesos ni mapeos o remapeos arbitrarios |
| Controlador KMDF 1.33 no PnP | Vinculación, objetos/contextos, dispositivos de control con nombre, colas secuenciales o paralelas predeterminadas con límite finito o ilimitado y solicitudes con búfer/directas/neither con callbacks ejecutados | Sin dispositivos PnP, planificación general de colas, extensiones de clase ni UMDF |
| Controlador PnP de bus, función o filtro | PDO explícitos sin recursos o con bancos de registros, AddDevice invitado y ocho menores comunes del ciclo PnP | Otras operaciones PnP, política general de energía, hardware/recursos generales y KMDF PnP general |
| Controladores de almacenamiento, red, pantalla, sistema de archivos y minifiltros | Contratos de subsistemas no compatibles | Frameworks de puerto/clase/miniport, NDIS/WFP, servicios gráficos o del sistema de archivos |
| Trabajo, temporizadores, DPC, eventos y esperas | El IRQL actual es `PASSIVE_LEVEL` para despacho y trabajo, y `DISPATCH_LEVEL` para DPC | Solo las API siguientes; sin IRP simultáneos ni cancelación general de solicitudes WDM |
| Operaciones del registro mediante las API Zw listadas | Árbol explícito de sesión y derechos por handle | ACL, privilegios, vistas alternativas y persistencia |
| Controlador con callbacks de proceso/hilo, otros handles, operaciones de archivo o descubrimiento de módulos del kernel | No compatible fuera de las API enumeradas | Administrador de objetos, estado del sistema y productores de callbacks/eventos |
| Controlador de hardware, DMA, PCI, interrupciones o virtualización | Bancos explícitos, MMIO, interrupciones latched exclusivas y DMA coherente common/SG/channel | Otros modelos de dispositivos, RAM física arbitraria, PCI, puertos, interrupciones compartidas/de nivel/MSI, otras interfaces DMA y estado CPU privilegiado |
| Controlador de Windows x86 o ARM64 | Rechazado | Carga, ABI y modelo de ejecución específicos de la arquitectura |
| CFG x64 | Tablas de destinos validadas y llamadas check/dispatch; la instrumentación inactiva conserva los destinos alternativos del invitado | Se rechazan XFG, supresión de exportaciones, configuración de carga no modelada y TLS |

Una importación no compatible que no se utilice puede permanecer vinculada.
Al alcanzar una operación no compatible, la ejecución se detiene con un
diagnóstico y las observaciones recopiladas hasta entonces. El éxito de
DriverEntry por sí solo no demuestra que se admitan las rutas posteriores de
despacho, hardware o framework. La siguiente tabla de API define el subconjunto
compatible de referencia.

## Contrato de ejecución

El perfil modela un ciclo WDM x64 en CPU0 con planificación cooperativa determinista. La ejecución comienza en el punto de entrada PE y conserva
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
Las lecturas x64 de CR8 en línea observan el mismo `PASSIVE_LEVEL` / `DISPATCH_LEVEL`; las escrituras
de CR8 y otras operaciones de registros de control siguen sin estar admitidas.

Las importaciones desconocidas se vinculan a trampas que se activan al usarlas.
Una importación no utilizada no impide ejecutar; ejecutar su thunk o leer un
valor de datos exportado no modelado detiene la ejecución con `unsupported_api`.
Los efectos no compatibles del entorno de CPU también provocan una detención
explícita. NeverD no sustituye llamadas sin implementar por valores de éxito.
Las imágenes malformadas o los requisitos de carga no compatibles fallan antes
de la ejecución.

Los elementos `DelayedWorkQueue` se ejecutan a `PASSIVE_LEVEL` y los callbacks DPC invitados a `DISPATCH_LEVEL` con los cuatro argumentos definidos. CPU0 usa planificación cooperativa determinista al retornar llamadas o bloquear esperas. Los temporizadores relativos, absolutos y periódicos usan tiempo virtual que avanza al siguiente vencimiento de temporizador, espera o cancelación cuando no hay marcos ejecutables. Eventos/temporizadores de notificación y sincronización conservan sus reglas distintas de consumo de señales. Cada callback tiene una pila invitada independiente; varios marcos bloqueados conservan variables locales y contextos CPU completos con memoria compartida. Win64 pasa los primeros cuatro argumentos en registros y los demás en la pila. Las solicitudes siguen siendo seriales: el despacho que marca un IRP pendiente debe devolver `STATUS_PENDING` y terminar antes de la siguiente solicitud. Sin productor disponible, una solicitud pendiente o espera infinita se detiene con `model_error` por bloqueo. Se comparten los límites de instrucciones, memoria, observaciones y tiempo real.

Este modelo acotado no ofrece todo el comportamiento asíncrono de Windows. No admite esperas alertables o de usuario, APC, cancelación general de solicitudes WDM, IRP simultáneos, UMDF, dispositivos PnP de KMDF y planificación general de colas, PnP/energía completo, hardware general, otras interfaces DMA ni otros modos de interrupción. La inicialización sola ejecuta callbacks explícitamente encolados sin crear solicitudes ni descarga implícita.

El elemento sale de la cola antes de iniciar su callback, que puede liberar su propio elemento. Liberar uno aún en cola, encolarlo dos veces, usar objetos caducados o destinos fuera de memoria invitada ejecutable provoca un fallo explícito. La referencia al dispositivo se conserva hasta que retorna el callback. La descarga exige liberar todos los elementos y terminar el trabajo en cola. Los contextos CPU conservan registros generales, SIMD, FPU y estado de control; la memoria invitada sigue compartida y restaurar un contexto no permite reanudar una CPU con fallo.
La eliminación se aplaza mientras queden objetos de archivo o referencias de trabajo en cola/en ejecución. La asignación de elementos devuelve NULL al agotarse el espacio de objetos.

Las imágenes utilizan su base preferida salvo que el escenario seleccione una
dirección de reubicación válida, y deben ser ejecutables PE32+ x64 con el
subsistema nativo. Las importaciones pueden proceder de `ntoskrnl.exe`,
`ntkrnlmp.exe` o `WDFLDR.SYS`. El cargador de ejecución admite reubicaciones de base x64
`DIR64` validadas y una configuración de carga limitada para la cookie de
seguridad, inicializada antes del wrapper de entrada con una cookie determinista
del invitado. Se rechazan otros campos no modelados de configuración de
carga, TLS, importaciones diferidas/vinculadas, importaciones por ordinal e
imágenes administradas. Las imágenes también deben superar controles estrictos
de rangos y alineación.

Control Flow Guard (CFG) activo valida indicadores PE, posiciones de punteros y destinos ejecutables ordenados. Los helpers check/dispatch solo admiten entradas declaradas de la imagen o thunks API registrados, conservan el estado Win64 y rechazan destinos no declarados. La instrumentación sin CFG activo mantiene los punteros alternativos originales del invitado. XFG activo, supresión de exportaciones y otras políticas no modeladas siguen rechazados; pertenecer a memoria ejecutable no basta para ser un destino válido.

Una pila WDM puede contener varios dispositivos del mismo controlador invitado. `IoAttachDeviceToDeviceStack` conecta una fuente aislada sobre el extremo superior actual del destino y devuelve ese extremo anterior. Establece `StackSize` y `AlignmentRequirement` sin modificar la lista `NextDevice` ni copiar los indicadores de búfer. `IoDetachDevice` recibe el dispositivo inferior guardado y exige `PASSIVE_LEVEL`; conectar permite IRQL hasta `DISPATCH_LEVEL`. Abrir un dispositivo inferior con nombre despacha al extremo superior actual, mientras `FILE_OBJECT.DeviceObject` y el informe conservan la identidad con nombre. READ/WRITE usa los indicadores del extremo superior seleccionado. La ruta retenida conserva todos los dispositivos hasta el retorno del despacho, incluso tras desconectar/eliminar; las referencias internas no aumentan `ReferenceCount`, que cuenta handles abiertos.

`IofCallDriver` y el auxiliar `IoCallDriver` llaman al destino exacto de esa ruta. Los auxiliares inline reales `IoCopyCurrentIrpStackLocationToNext`, `IoSkipCurrentIrpStackLocation` e `IoSetCompletionRoutine` operan sobre la IRP invitada original; se validan cursor, cantidad e indicadores de control. El despacho inferior devuelve su estado real, separado de `IoStatus` y del retorno de las rutinas de finalización. La finalización avanza el cursor, selecciona callbacks según éxito/error/cancelación y propaga pending hacia arriba. Un callback ejecutado se encarga de dicha propagación, incluso tras un retorno anterior `STATUS_PENDING`. `STATUS_MORE_PROCESSING_REQUIRED` detiene el desenrollado conservando IRP, MDL y búferes; una finalización posterior lo reanuda. La finalización anidada exige el resultado de detención externo, y el almacenamiento se retira una sola vez al final. Las continuaciones identificadas por subsistema conservan marcos WDM/WDF e IRQL heredado. Cada posición inferior consumida se borra antes del callback de finalización superior.

La política general de energía, IRP asignados por el controlador, otros menores PnP y modelos de hardware/recursos generales siguen sin admitirse. El reenvío a destinos WDF fuera del ciclo de vida de archivos FDO/PDO directo, conectar a pilas con archivos o callbacks activos, desconectar una capa intermedia, cambiar la función mayor y usar destinos fuera de la ruta fallan explícitamente. El fixture opcional `driver_wdm_stack.c`, compilado con WDK auténtico, usa `NEVERD_WDM_STACK_FIXTURE` y `NEVERD_WDM_STACK_CFG_FIXTURE`. Las pruebas nativas y C API/CLI incluyen reubicación; los artefactos ausentes se omiten explícitamente. La evidencia de ejecución sigue limitada a Linux.

Un escenario puede configurar explícitamente hasta 64 `pnp_devices`. Cada entrada exige `id`, `bus: "resource_free"` / `bus: "register_bank"`, `initial_device_power: "D0"` e `initial_system_power: "working"`; no se adivinan datos omitidos. Los ID son ASCII de 1–64 bytes, sensibles a mayúsculas: empiezan por un carácter alfanumérico y solo permiten alfanuméricos, `_`, `-`, `.`. Una solicitud ordinaria puede seleccionar `device_id` configurado en lugar de `device`, nunca ambos. `kind: "pnp"` exige `device_id`, `minor` y `bus_completion`; se admiten `start`, `query_remove`, `cancel_remove`, `remove`, `query_stop`, `stop`, `cancel_stop` y `surprise_removal`. `bus_completion.status` es un entero de 32 bits o cadena hexadecimal obligatorio; `delay_100ns` opcional es un entero no negativo hasta INT64_MAX desde la recepción real del proveedor. `STATUS_PENDING` no es estado final; stop/cancel-stop/surprise-removal/cancel-remove/remove requieren exactamente `STATUS_SUCCESS` (0). PnP rechaza campos de archivo, transferencia y cancelación incluso con cero. La API C++ aplica la misma validación.

```json
{
  "pnp_devices": [
    {"id": "sensor0", "bus": "resource_free", "initial_device_power": "D0", "initial_system_power": "working"}
  ],
  "requests": [
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0", "delay_100ns": 10}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "stop", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "start", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "query_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "cancel_remove", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "surprise_removal", "bus_completion": {"status": "0x0"}},
    {"kind": "pnp", "device_id": "sensor0", "minor": "remove", "bus_completion": {"status": "0x0"}}
  ],
  "unload": true
}
```

Tras DriverEntry exitoso, `AddDevice` se ejecuta una vez por PDO configurado con un `DRIVER_OBJECT` propio del proveedor; el invitado no puede borrar ni suplantar sus objetos. Los IRP PnP son `KernelMode`, sin archivo, con recursos según el bus configurado, inicialmente `STATUS_NOT_SUPPORTED`. Solo se consume la respuesta si el reenvío llega al PDO; la finalización diferida usa el reloj virtual y las continuaciones existentes. La finalización superior confirma o revierte el ciclo independientemente del estado del bus. El éxito PnP exige finalización real del proveedor; un fallo temprano de START/QUERY_STOP/QUERY_REMOVE puede dejar nula la observación del bus. La eliminación normal desde Started exige query exitoso, archivos cerrados, solicitudes previas terminadas y desconexión/borrado por el invitado. La identidad de dispositivo/archivo persiste tras desconectar. Un fallo limpio de AddDevice retira solo al proveedor; las fugas de dispositivos nuevos, incluso desconectados, causan `model_error`. Antes de unload todos los proveedores deben estar ausentes. No se incluyen otros menores PnP, política general de energía, hardware/recursos generales ni KMDF PnP general.

`configuration.pnp_devices` conserva la configuración inicial. Los `pnp_devices` observados contienen `id`, `pdo`, `add_device_status` nullable, `attached` actual, `pnp_state`, `provider_present`; tras remove, `attached` es false. La fase AddDevice es `add_device:<ID>`; sus fallos afectan `scenario_success` sin sustituir el `nt_status` de DriverEntry. Cada solicitud añade `device_id` y `pnp` nullable; PnP usa `file: null`. `pnp` registra `minor`, `state_before`, `state_after`, `bus_status` nullable, `bus_received_at_100ns`, `bus_completed_at_100ns`. El estado configurado se observa solo al finalizar realmente el bus; la recepción se registra independientemente. Los tipos de campos existentes no cambian.

Las solicitudes ordinarias CREATE/READ/WRITE/IOCTL/CLEANUP/CLOSE llegan al despacho invitado real mientras el dispositivo exista fuera de Removing/Removed. El modelo no inventa fallos por Stopped, StopPending, RemovePending ni energía: el propio controlador decide completar I/O de software, rechazarlo o retenerlo. El ejecutor público sigue siendo serial; una IRP retenida sin productor disponible no puede liberarse mediante un start/cleanup posterior del escenario y termina con `model_error` por bloqueo. Cerrar archivos y terminar solicitudes anteriores antes de Remove son restricciones del perfil. `query_stop` con estado final `STATUS_RESOURCE_REQUIREMENTS_CHANGED` (0x119) se rechaza tanto en preflight como al completar el invitado porque exige una consulta de recursos no modelada; véase [el contrato QUERY_STOP de Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-stop-device). Stop/restart y surprise-removal no añaden reasignación general de recursos, política general de energía ni KMDF PnP general.

PnP sin recursos usa el original `driver_wdm_pnp.c` con WDK auténtico, `NEVERD_WDM_PNP_FIXTURE`/`NEVERD_WDM_PNP_CFG_FIXTURE` opcionales y pruebas nativas/C API/CLI. Los artefactos ausentes se omiten explícitamente; la evidencia sigue limitada a Linux.

Los remove-locks WDM ejecutan los exports reales `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx`, `IoReleaseRemoveLockEx` y `IoReleaseRemoveLockAndWaitEx`; los nombres WDK sin Ex son macros. El dueño es el DEVICE_OBJECT exacto cuya extensión contiene todo el almacenamiento alineado, independiente del estado PDO o aspecto del Tag. Se permite inicializar antes de adjuntar. Retail 32/DBG 120 bytes requieren el parámetro separado de tamaño coincidente; toda la zona registrada es opaca. Los Tags NULL/repetidos se cuentan por bloqueo y nunca se desreferencian, por lo que release tras completar el IRP sigue siendo válido. Inicialización/AndWait requieren `PASSIVE_LEVEL`; acquire/release permiten `DISPATCH_LEVEL`.

AndWait cierra la admisión, libera una adquisición coincidente y suspende el marco invitado real hasta liberar las restantes. Acquire posterior devuelve `STATUS_DELETE_PENDING` sin obligación de release. La última liberación fija la disponibilidad antes del retorno del callback; un worker puede liberar y esperar un evento del REMOVE reanudado. No se inventan callbacks, timeout ni éxito sin productor. Se exige una ruta REMOVE activa asociada que contenga al dueño y recepción real del proveedor (`bus_received_at_100ns` puede ser cero), no finalización inferior. Encolar en un controlador inferior antes de llegar al proveedor queda fuera del perfil; no es una comprobación completa OutsideRemoveDevice/Driver Verifier. Siguen exigiéndose archivos cerrados y solicitudes previas terminadas antes de REMOVE, pero pueden quedar callbacks que liberen bloqueos. La ruta se retiene durante espera, desconexión/borrado y pending inferior hasta retornar los marcos restantes.

Almacenamiento desconocido/incompatible, release sin adquisición, drain duplicado, reinicialización o borrado con adquisiciones/espera drain sin consumir fallan antes de mutar. Un fallo limpio AddDevice puede borrar un bloqueo inicializado sin usar. Los bloqueos no reemplazan referencias reales de dispositivo/worker; se desregistran al retirar físicamente la extensión. Los metadatos debug no activan límites de tiempo/high-water de Verifier. El auténtico `driver_wdm_remove_lock.c` usa `NEVERD_WDM_REMOVE_LOCK_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`/`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`. Se omiten explícitamente artefactos ausentes; evidencia nativa y C API/CLI solo Linux, sin implicar un gestor completo de retirada ni drenaje general de I/O concurrente.

El bus sintético `bus: "register_bank"` añade recursos de memoria fijos y explícitos a un PDO. Cuando está presente, su array `resources` contiene `id`, `raw_start`, `translated_start`, `length` y `registers`; cada registro exige `offset`, `width`, `access` (`read_only` o `read_write`) y un `value` inicial. `DriverResources.h` / `DriverResources.def` definen el contrato compartido de C++ y JSON. Los ID de recurso siguen las reglas acotadas de identificadores ASCII y son únicos dentro de cada PDO. Se permiten 8 recursos por PDO / 32 en total, 256 registros por recurso / 4096 en total y entre 1 y 1048576 bytes por recurso. Solo se admiten accesos exactos de 1/2/4 bytes con alineación natural; los valores deben caber en ese ancho. Ambos intervalos físicos deben evitar desbordamientos; los rangos raw no pueden solaparse dentro de un PDO y los traducidos no pueden solaparse globalmente. Un `registers` vacío deja explícitamente inaccesible todo el banco. Las direcciones y valores iniciales son declaraciones, nunca hardware del anfitrión ni memoria rellenada implícitamente con ceros. `resource_free` mantiene el inventario de recursos omitido y los punteros START nulos.

START recibe asignaciones `CM_RESOURCE_LIST` raw y traducidas independientes y de solo lectura, con descriptores Memory correspondientes y ordenados: un descriptor completo, interfaz Internal, bus 0, versión/revisión 1, uso DeviceExclusive e indicadores READ_WRITE para el rango. La restricción RO de cada registro es independiente. START inferior exitoso habilita la asignación antes de los callbacks superiores; cada START desde NotStarted/Stopped crea una nueva generación con la misma asignación fija. Los registros se inicializan una vez por PDO y conservan sus valores al desasignar, detener y reiniciar. Tras START fallido y STOP/REMOVE exitosos, el controlador debe liberar los mappings antes de la finalización terminal de la IRP; nunca se limpian silenciosamente. La retirada inesperada impide inmediatamente nuevos mappings y accesos, pero permite desasignar los existentes. El SET de dispositivo completado con éxito por el proveedor cambia la disponibilidad física: D3 bloquea los accesos y D0 los permite solo con una asignación disponible. D3 permite crear mappings sin acceder a registros; los cambios de energía no los descartan ni reinician valores.

`MmMapIoSpace` admite NonCached; `MmMapIoSpaceEx` admite PAGE_NOCACHE con PAGE_READONLY o PAGE_READWRITE. Ambos aceptan únicamente un subrango traducido declarado dentro de una asignación, conservando su desplazamiento de página. Los alias comparten un banco, tienen permisos independientes y requieren la base y longitud originales exactas para `MmUnmapIoSpace`. El agotamiento de mappings, de la ventana o del presupuesto de memoria devuelve NULL; los fallos del backend siguen siendo errores explícitos. Una dirección desasignada no recupera acceso mediante un mapping posterior. Las instrucciones escalares y REP reales pasan por las comprobaciones MMIO de CPU; los huecos, anchos erróneos, desalineación, escrituras RO, accesos que cruzan mappings y ejecución fallan antes de afectar registros. Los accesos a memoria de las API modeladas también deben caber en una única transacción alineada de 1/2/4 bytes; los intervalos mayores fallan en lugar de dividirse en accesos a registros. No se implementan RAM física arbitraria, redistribución de recursos, puertos, otros modos de interrupción, otras interfaces DMA ni comportamiento general de hardware.

El [escenario de banco de registros](../examples/driver-register-bank-scenario.json) ejecutable usa el original `driver_wdm_resources.c`, compilado con WDK auténtico, mediante `NEVERD_WDM_RESOURCE_FIXTURE` / `NEVERD_WDM_RESOURCE_CFG_FIXTURE`. Sus 14 solicitudes incluyen START diferido, E/S de archivo, STOP, reinicio y retirada, y observan valores persistentes a través del IOCTL del controlador. `configuration.pnp_devices[].resources` registra los datos iniciales con direcciones físicas hexadecimales sin pérdida; no es un segundo informe del estado del banco. C API y Python mantienen `scenario_json` y la estructura `neverd_driver_options_v1` sin cambios. Los artefactos auténticos ausentes se omiten explícitamente; la evidencia de ejecución se limita a Linux.

El mismo proveedor `register_bank` puede declarar `interrupts` por separado o junto a `resources`; al menos una lista debe contener elementos. `resource_free` rechaza el campo explícito `interrupts`, incluso `[]`. Cada interrupción exige `id`, `raw_vector`, `raw_level`, `raw_affinity`, `translated_vector`, `translated_level`, `translated_affinity`, `mode: "latched"` y `share: "device_exclusive"`. `DriverInterrupts.h` / `DriverInterrupts.def` definen tipos, nombres y límites: 8 interrupciones por PDO / 32 en total, ID ASCII acotados y únicos por PDO, vectores traducidos exclusivos globalmente, nivel raw de 0–65535 y DIRQL traducido de 3–12. Los vectores conservan los 32 bits sin deducir una relación vector/IRQL. Ambas máscaras de afinidad deben ser 1: CPU0 y grupo0 son hechos explícitos del proveedor. Los valores raw y traducidos son independientes. Los descriptores de memoria conservan su orden, seguidos de los descriptores ordenados de interrupción en la misma `CM_RESOURCE_LIST`; estos usan tipo 2, uso compartido DeviceExclusive y flags LATCHED. Este perfil sintético activado por flancos no representa líneas PCI compartidas activadas por nivel.

Las solicitudes READ/WRITE/IOCTL pueden declarar `interrupt_events`, cada uno con `after_100ns`, `device_id` e `interrupt_id` explícitos; los demás tipos rechazan el campo incluso vacío. Se aceptan como máximo 64 eventos por solicitud / 1024 en total, con retardo no negativo hasta INT64_MAX. El envío correcto de la solicitud fija el inicio del retardo y captura una interrupción ya conectada y la generación de recursos de su PDO. Los eventos son pulsos externos de línea independientes: completar la IRP de origen no los cancela y las escrituras de registros no deducen habilitación, estado ni acuse. El tiempo solo avanza en inactividad; los eventos vencidos se ejecutan en el siguiente límite de callback admitido, por lo que `after_100ns: 0` no promete interrumpir instrucciones ni entregar antes de la primera instrucción del huésped. Los productores del mismo instante se comprueban juntos contra los límites de capacidad; la publicación del estado hardware precede a la elegibilidad y los ISR preceden a DPC/workers. La pérdida de conexión, una generación obsoleta/no disponible o D3 físico registra `undelivered_reason` y detiene con `model_error`; el evento nunca se reasigna ni se retrasa silenciosamente.

`IoConnectInterrupt` usa sus once argumentos reales y la asignación traducida exacta. `IoConnectInterruptEx` admite FullySpecified (1), LineBased (2, una línea asignada al PDO explícito) y FullySpecifiedGroup (4, grupo 0); `IoDisconnectInterruptEx` exige versión/contexto coincidentes. Registro y desconexión requieren PASSIVE_LEVEL. Solo se admiten un bloqueo privado de interrupción, modo latched exclusivo, CPU0/grupo0 y ninguna conservación del estado flotante. El IRQL de sincronización debe igualar el DIRQL asignado; cero en LineBased selecciona ese nivel. `KINTERRUPT` es opaco y su dirección nunca se reutiliza. El ISR real recibe `(Interrupt, ServiceContext)` y devuelve BOOLEAN desde AL; `FALSE` significa no reclamado, no fallo NTSTATUS. `KeSynchronizeExecution` ejecuta el callback real de un argumento bajo el mismo bloqueo a DIRQL y devuelve su BOOLEAN restaurando IRQL/CR8 del llamador. `KeAcquireInterruptSpinLock` / `KeReleaseInterruptSpinLock` exigen propiedad no recursiva, ejecución original e IRQL guardado; ningún bloqueo retenido puede cruzar el retorno del callback. Siguen sin admitirse esperas en interrupciones, bloqueos compartidos aportados por el llamador, interrupciones compartidas/de nivel/MSI/pasivas ni interrupción de instrucciones. START fallido y STOP/REMOVE exitosos exigen desconexión antes de la finalización terminal; los callbacks superiores pueden limpiar antes y desconectar nunca descarta silenciosamente un DPC encolado.

Los informes separan declaraciones y observaciones. `configuration.pnp_devices[].interrupts` conserva los recursos y `configuration.interrupt_events` aplana los eventos de entrada con `source_request_index` (índice de solicitud configurada) y `event_index`, ambos desde cero. Las filas raíz `interrupts` añaden `device_id`, `interrupt_id`, `epoch`, `due_at_100ns` absoluto y los campos anulables `occurred_at_100ns`, `delivered_at_100ns`, `returned_at_100ns`, `interrupt_object`, `return_value`, `claimed` y `undelivered_reason`. `claimed` deriva únicamente del byte bajo del retorno real; las marcas temporales son observaciones, no resultados inventados. Para `scenario_success`, cada evento configurado debe retornar sin fallo de entrega; un ISR no reclamado sigue siendo válido. Los efectos DPC aparecen en finalizaciones, llamadas API y mensajes reales. El [escenario de interrupciones](../examples/driver-interrupt-scenario.json) ejecutable usa el original `driver_wdm_interrupts.c` compilado con WDK auténtico y los artefactos opcionales `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`: siete solicitudes incluyen START retardado, IOCTL pendiente completado por ISR→DPC, limpieza/cierre de archivo y retirada. No cambian la entrada C/Python `scenario_json` ni el diseño de `neverd_driver_options_v1`. La ausencia de artefactos se omite explícitamente; la evidencia de ejecución se limita a Linux.

`DriverDMA.h` / `DriverDMA.def` añaden un objeto `dma` opcional a cada PDO `register_bank`, junto a sus asignaciones de memoria/interrupciones; DMA por sí solo no sustituye ninguna de esas listas. Los siete campos son explícitos: `address_bits` (32 o 64), `maximum_length` (1–1048576 bytes), `map_registers` (1–256), `alignment` (potencia de dos entre 1–4096), `logical_base` (no nulo y alineado a página), `logical_length` (alineado a página, 4096–1073741824 bytes) y el booleano `scatter_gather`. La ventana no debe desbordarse ni exceder el ancho de dirección. Cada PDO tiene un dominio lógico independiente; las mismas direcciones en dispositivos distintos no crean alias. Todos los recursos MMIO traducidos deben evitar la RAM reservada del modelo `[0x1000000000, 0x1000100000)`, incluso sin DMA configurado. Son hechos de un maestro de bus sintético coherente, nunca memoria física del anfitrión ni un dispositivo PCI.

`IoGetDmaAdapter` acepta los campos históricos de `DEVICE_DESCRIPTION` versión 0/1 para un maestro de bus Internal y publica un `DMA_ADAPTER` de versión uno con su tabla real `DMA_OPERATIONS` de 104 bytes. Las consultas de versión 2/3 devuelven NULL sin leer la cola moderna del descriptor. Cada método indirecto se vincula al adaptador exacto y activo, independientemente de las importaciones del kernel. Se implementan `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `GetScatterGatherList`, `PutScatterGatherList`, `PutDmaAdapter`, `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers` y `FreeMapRegisters`. `FreeAdapterChannel` y `ReadDmaCounter` siguen siendo trampas identificadas: el perfil no modela un controlador DMA subordinado o del sistema. La asignación/liberación de common buffers y las consultas de alineación exigen PASSIVE_LEVEL; Get/PutScatterGatherList exige DISPATCH_LEVEL y liberar el adaptador admite hasta DISPATCH_LEVEL. x64 ignora `CacheEnabled`. Consultas de versiones no admitidas, capacidades declaradas incompatibles y escasez documentada de recursos devuelven NULL; selecciones malformadas o no modeladas y fallos del backend siguen siendo errores explícitos.

`KernelPhysicalMemory` asigna como máximo 256 páginas físicas del modelo de 4096 bytes a la RAM existente. Las direcciones virtuales CPU, identidades de página física y direcciones lógicas del dispositivo son distintas. Los arrays PFN de MDL construidos exponen esas identidades compartidas en solo lectura; los descriptores sin construir no tienen PFN utilizables. Las asignaciones pequeñas vecinas pueden compartir PFN conservando rangos de bytes y vidas independientes. Common buffers, pool y búferes de solicitudes usan los mismos bytes que ya pertenecen a `GuestMemory`, sin otra copia DMA. Un mapping SG activo fija su rango exacto y descriptor. La finalización, liberación de pool/MDL y retirada rechazan dependencias activas antes de liberar. Desmapear un MDL directo solo revoca su mapping de sistema CPU; DMA conserva acceso al respaldo bloqueado. `DmaWritable` registra el contrato de bloqueo para escritura separado de los permisos CPU: escribir desde el dispositivo exige READ/OUT_DIRECT directo o almacenamiento no paginado escribible; WRITE/IN_DIRECT no obtiene ese permiso porque su mapping CPU permita escribir.

`GetScatterGatherList` valida CurrentVa/Length contra el rango original del MDL y crea fragmentos de páginas lógicas sobre el respaldo existente. Con registros de mapa disponibles, el `AdapterListControl` real de cuatro argumentos y retorno void se ejecuta dentro de la llamada antes del retorno de la API; en caso contrario se retienen datos/descriptor y una plaza de callback en la FIFO del PDO hasta liberar recursos. Este perfil no tiene propiedad StartIo, por lo que el segundo argumento IRP del callback es NULL. Retornar del callback no libera el mapping. `PutScatterGatherList` puede ejecutarse dentro del callback; después de Put el controlador puede completar la solicitud y liberar el último adaptador, mientras la continuación del callback y su referencia al dispositivo permanecen hasta el retorno. Liberar un common buffer exige su adaptador, longitud, dirección lógica y dirección CPU originales. Las direcciones lógicas nunca se reutilizan en la sesión, tampoco tras reiniciar. Una espera de recursos sin productor real se detiene explícitamente; no fabrica una finalización ni un plazo. Mientras un mapping SG activo posee los datos para el dispositivo, el acceso CPU exige primero Put; un callback que aún espera registros de mapa no ha cedido esa propiedad de los bytes.

`AllocateAdapterChannel` exige DISPATCH_LEVEL y reserva un identificador opaco no NULL de registros de mapeo. Common buffers, listas SG y reservas de canal comparten la cuota y FIFO de cada PDO. El éxito acepta un `AdapterControl` real inmediato o en cola; un número excesivo devuelve `STATUS_INSUFFICIENT_RESOURCES` sin callback. Solo se permite un callback de asignación sin terminar por dispositivo invitado; se rechaza llamar AllocateAdapterChannel desde AdapterControl, incluso mediante un callback anidado. Sus cuatro argumentos incluyen la instantánea real de `DEVICE_OBJECT.CurrentIrp` tomada al registrarlo. Ese campo exacto de ocho bytes es escribible en dispositivos invitados; se acepta cero o un IRP vivo cuyo recorrido incluye el dispositivo. La cola retiene el paquete hasta entrar al callback, que entonces puede completarlo. El perfil sigue sin StartIo; el argumento IRP no utilizado del callback SG independiente permanece NULL.

`AdapterControl` devuelve un `IO_ALLOCATION_ACTION` de 32 bits; se ignoran los bits altos de RAX y la acción no sustituye el `STATUS_SUCCESS` de AllocateAdapterChannel. `DeallocateObject` libera registros sin usar o totalmente vaciados al retornar. `DeallocateObjectKeepRegisters` los conserva hasta que `FreeMapRegisters` reciba el adaptador, identificador y número original exactos. `KeepObject` requiere un controlador del sistema no modelado y falla explícitamente. La asignación recién entregada no puede liberarse como retenida antes de retornar su callback; otra asignación retenida previamente puede liberarse cuando su propio contrato lo permita. La identidad del callback, los registros retenidos y los bytes mapeados tienen vidas separadas, verificadas antes de retirar el adaptador o dispositivo.

`MapTransfer` y `FlushAdapterBuffers` admiten hasta DISPATCH_LEVEL; asignar canales y liberar registros exige DISPATCH_LEVEL. MapTransfer recibe un índice relativo al MDL, lee y actualiza una longitud ULONG real y devuelve por valor la dirección lógica. El perfil SG limitado devuelve un fragmento de página de respaldo por llamada; los índices posteriores contiguos del mismo MDL y dirección amplían una operación. Sin SG, mapea toda la extensión solicitada en una llamada si cabe en la reserva, sin acortarla. El primer mapeo reserva una ventana lógica no reutilizable del tamaño de la reserva de registros; otras asignaciones pueden intercalarse sin solaparla. Los fragmentos comparten una única fijación física que crece. Las transacciones del dispositivo pueden abarcar toda la operación mapeada; el acceso CPU, liberar MDL y completar solicitudes que retiren memoria fijada quedan bloqueados hasta el flush agregado. Flush debe coincidir con el índice inicial, MDL, dirección y longitud total realmente mapeada. Libera los bytes pero conserva los registros, permitiendo otra operación con el mismo identificador. Los flush parciales, mezclar MDL y otros patrones MapTransfer quedan fuera del perfil; no se consideran inválidos en todo Windows.

`KeFlushIoBuffers` valida un MDL vivo bloqueado o de memoria no paginada. La plataforma modelada es coherente: ningún valor de ReadOperation o DmaOperation requiere otra copia de caché; la llamada no libera la propiedad DMA ni sustituye FlushAdapterBuffers. El [escenario de canal](../examples/driver-dma-channel-scenario.json) ejecuta el original `driver_wdm_dma_channel.c` con dos MapTransfer, una transacción que cruza páginas, IRQ/DPC declarado por separado, flush agregado y liberación exacta de registros. Las imágenes auténticas normal/CFG usan `NEVERD_WDM_DMA_CHANNEL_FIXTURE` y `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`.

Solo READ/WRITE/IOCTL aceptan `dma_events`. Cada evento exige `after_100ns`, `device_id`, `logical_address`, `direction` y `length`; `write_memory` exige además `data_hex` de longitud exacta y `read_memory` rechaza ese campo. Las direcciones se expresan desde la perspectiva del dispositivo. Los límites son 64 eventos por solicitud, 1024 en total, 16 MiB de bytes totales y 1 MiB por transacción; el retraso va de cero a INT64_MAX. El envío captura la generación asignada del PDO y fija el origen temporal virtual, pero no exige un mapping que el despacho aún debe crear. La entrega resuelve todo el rango lógico activo y la dirección, exige D0 físico y valida todos los bytes antes de producir efectos. Completar el IRP de origen no cancela el evento. Mappings ausentes/liberados, generaciones antiguas, retirada inesperada o D3 registran un fallo y detienen la ejecución; los eventos no se revinculan ni inventan interrupciones, protocolos de registros o finalización de IRP. En el mismo límite de planificación, primero se publica el hardware del proveedor, después los bytes DMA y luego los pulsos de interrupción declarados independientemente. La temporización sigue siendo cooperativa, sin interrupción a nivel de instrucción.

Los informes conservan `configuration.pnp_devices[].dma` y `configuration.dma_events` aplanado. Las filas raíz `dma_transfers` identifican `source_request_index`, `event_index`, `device_id`, `epoch`, `logical_address`, `direction`, `length` y `due_at_100ns`, con `occurred_at_100ns`, `completed_at_100ns`, `mapping`, `adapter` y `failure_reason` anulables; `data_hex` contiene los bytes realmente transferidos. Todas las transacciones declaradas deben completar sin fallo para `scenario_success`. El [escenario DMA](../examples/driver-dma-scenario.json) utiliza el original `driver_wdm_dma.c` compilado con WDK auténtico y las rutas opcionales `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`, ejercitando un common buffer mediante punteros reales del adaptador y una finalización ISR→DPC declarada por separado. C/Python siguen usando `scenario_json` sin cambiar `neverd_driver_options_v1`. Los artefactos ausentes se omiten explícitamente; la evidencia se limita a Linux. Controladores subordinados, métodos V2/V3, motores de descriptores hardware, DMA general de KMDF y otros modelos de dispositivos siguen sin admitirse.

Las solicitudes WDM de energía del bus sintético usan `kind: "power"` y un `device_id` configurado. Cada paquete exige `minor` (`query`/`set`), `power_type` (`device`/`system`), `power_state` (`D0`/`D3` o `working`/`sleeping3`), `power_action` (`none`/`sleep`), `system_context` (entero de 32 bits o cadena hexadecimal) y `bus_completion` explícitos. System Query a Working no está admitido; se rechazan campos de archivo, transferencia y cancelación. Todo `system_context` se conserva como dato opaco, sin inferir padres, hibernación o inicio rápido. Las rutas requieren `DO_POWER_PAGABLE` sin `DO_POWER_INRUSH`; el despacho y `PoRequestPowerIrp` se ejecutan en `PASSIVE_LEVEL`. `PoCallDriver` reenvía el mismo IRP administrado; `PoStartNextPowerIrp` sigue el contrato Vista+ sin protocolo adicional de serialización. Siguen excluidos política general de energía, WAIT_WAKE, otros estados/acciones, apagado/hibernación, corriente de arranque, rutas no paginables, hardware general y KMDF PnP general.

Cada entrada `pnp_devices` puede indicar `initial_reported_device_power: "D0"` o `"D3"`, independiente del ciclo inicial obligatorio D0/working. El PDO y cada DEVICE_OBJECT invitado asociado por primera vez tienen estado de notificación propio; `PoSetPowerState` devuelve y actualiza solo el valor previo del dispositivo llamante. Sin semilla explícita falla, sin suponer D0. `requested_device_power` opcional contiene plantillas device con los mismos seis datos obligatorios, hasta 64 entre todos los PDO. Solo un `PoRequestPowerIrp` real cuyo PDO, minor y destino coincidan consume la cabecera FIFO de ese PDO. La ausencia/incompatibilidad falla; las entradas sin usar no crean solicitudes, ni context determina un padre. Cada hijo tiene IRP y fila independientes con `origin: "PoRequestPowerIrp"` y `response_index` desde cero; las filas del escenario usan `origin: "scenario"` e índice null. Un hijo síncrono puede ejecutar el callback void de cinco argumentos antes del retorno `STATUS_PENDING`; el callback puede esperar y System S0 terminar antes del hijo D0 independiente. La instantánea IO_STATUS_BLOCK dura hasta el retorno del callback.

El informe añade `power` nullable; las filas de energía tienen `file: null`. `power` registra los datos del paquete, `device_state_before`/`device_state_after`, `system_state_before`/`system_state_after`, `requested_device_object` nullable y `bus_status`/`bus_received_at_100ns`/`bus_completed_at_100ns` reales. Los dispositivos PnP finales añaden `device_power`/`system_power`; los vivos, `reported_device_power` nullable. `scenario_success` compara con la configuración solo las filas de origen escenario, pero todas las solicitudes reales, incluidos hijos, deben terminar correctamente; las plantillas sin consumir no causan fallo. El auténtico `driver_wdm_power.c` usa `NEVERD_WDM_POWER_FIXTURE`/`NEVERD_WDM_POWER_CFG_FIXTURE` para cobertura nativa y C API/CLI normal/active-CFG. Los artefactos ausentes se omiten explícitamente y la evidencia de ejecución sigue limitada a Linux.

El [escenario completo de energía](../examples/driver-power-scenario.json), mediante `--scenario`, ejecuta el controlador real: inicio, consulta/suspensión/reactivación del sistema y retirada, con tres respuestas hijas explícitas.

KMDF 1.33 usa exactamente la ABI 1.33.0: 458 entradas de función tienen identidades estables en el invitado y las 87 API siguientes tienen semántica de ejecución. `WdfVersionBind` y `WdfVersionUnbind` gestionan la vinculación del invitado alrededor del wrapper real WDK `FxDriverEntry`. `WdfGetDriver` lee las variables globales públicas del controlador. Los controladores no PnP, objetos genéricos, dispositivos de control, colas y solicitudes entrantes comparten contextos tipados, contadores de referencias y callbacks ejecutados de limpieza/destrucción/descarga. Todas las llamadas y callbacks modelados del framework requieren actualmente `PASSIVE_LEVEL`; añadir referencias tras finalizar la limpieza sigue fuera de este perfil. Las entradas no modeladas, `WdfLdrQueryInterface`, las extensiones de clase y UMDF detienen explícitamente la ejecución.

Los dispositivos de control requieren un nombre ASCII imprimible copiado y exactamente la SDDL `D:P(A;;GA;;;WD)`. Esta concede acceso universal sin inventar un token del llamador; no se admiten otros descriptores de seguridad, dispositivos sin nombre ni nombres automáticos. La inicialización posee un dispositivo WDM. Las solicitudes pueden seleccionarlo mediante los alias de enlaces simbólicos `\DosDevices\Name` o `\??\Name` del espacio de nombres de sesión existente; el informe conserva el nombre canónico del dispositivo. Una creación correcta consume el objeto de inicialización y borra su puntero; una creación fallida revierte la propiedad parcial del dispositivo. `WdfControlFinishInitializing` habilita la entrega de E/S. La eliminación retira el dispositivo y sus enlaces solo cuando los archivos, elementos de trabajo y solicitudes modelados lo permiten; no se admite cancelar o vaciar solicitudes durante la eliminación.

La estructura `WDF_IO_QUEUE_CONFIG` de 96 bytes admite colas predeterminadas y no predeterminadas manuales, secuenciales y paralelas con límite finito o ilimitado con ejecución pasiva explícita y sin sincronización del framework. Las colas de dispositivos de control no están sujetas a gestión de energía. Los callbacks específicos READ/WRITE/IOCTL tienen prioridad sobre el predeterminado. Las solicitudes aceptadas en la cola devuelven `STATUS_PENDING` incluso si se completan de forma síncrona; el registro de retorno de un callback void no completa su solicitud. La finalización diferida usa el planificador existente. Sin un manejador, la solicitud se completa con `STATUS_INVALID_DEVICE_REQUEST`; READ/WRITE de longitud cero se completa sin entrega salvo que esta se habilite. El paquete de archivos predeterminado completa CREATE/CLEANUP/CLOSE correctamente con Information=0. Las colas manuales no predeterminadas reciben solicitudes mediante `WdfRequestForwardToIoQueue`; `WdfIoQueueRetrieveNextRequest` las devuelve en orden FIFO. Si se cancela antes de recuperarla, el framework la retira y completa con `STATUS_CANCELLED`. Las colas automáticas no predeterminadas entregan las solicitudes reenviadas mediante sus propios callbacks; la cola manual predeterminada retiene las solicitudes entrantes hasta recuperarlas. `WdfIoQueueRetrieveNextRequest` funciona con colas manuales y secuenciales; en las paralelas devuelve `STATUS_INVALID_DEVICE_STATE`. Sin un callback apropiado, la cola automática completa la solicitud con `STATUS_INVALID_DEVICE_REQUEST` cuando queda libre una posición de presentación. El comportamiento más amplio de PnP y energía queda fuera de este perfil. `WdfRequestRequeue` devuelve una solicitud recuperada al inicio de la misma cola manual. `NumberOfPresentedRequests` limita las solicitudes presentadas en paralelo; las demás esperan a que una solicitud presentada termine o se cancele. Una cola secuencial predeterminada acepta más solicitudes mientras presenta una; las siguientes esperan en orden FIFO hasta que se libere el puesto y pueden cancelarse antes de entregarse. `WdfIoQueueStop` suspende la entrega sin dejar de aceptar solicitudes. `WdfIoQueueStart` entrega las solicitudes en espera y `WdfIoQueueGetState` informa los recuentos en cola y entregados. Recuperar durante la detención devuelve `STATUS_WDF_PAUSED`; el callback de finalización de parada recibe el contexto indicado cuando terminan o salen de la cola todas las solicitudes ya entregadas; las solicitudes pendientes no lo retrasan. Se rechaza registrar otro callback mientras uno sigue pendiente.

`WdfIoQueueReadyNotify` registra una devolución de llamada `EvtIoQueueState` para una cola manual. En `PASSIVE_LEVEL` recibe `(WDFQUEUE, WDFCONTEXT)` cuando la cantidad de solicitudes en cola pasa de cero a un valor positivo, aunque el controlador todavía posea solicitudes extraídas antes. Una cola ya no vacía puede notificar inmediatamente al registrarse; una cola detenida espera a `WdfIoQueueStart`. El registro duplicado y la baja antes de detenerla devuelven `STATUS_INVALID_DEVICE_REQUEST`. Tras `WdfIoQueueStop`, NULL da de baja la devolución de llamada.

`WdfIoQueueFindRequest` busca en una cola manual sin transferir la propiedad de la solicitud y añade una referencia al encontrarla. El controlador la libera con `WdfObjectDereference`. `WdfIoQueueRetrieveFoundRequest` transfiere la propiedad de una solicitud que sigue en la cola; si se retiró por cancelación, devuelve `STATUS_NOT_FOUND`. Los parámetros opcionales usan el mismo diseño que `WdfRequestGetParameters`. Un objeto de archivo activo filtra `WdfIoQueueFindRequest`. `WdfIoQueueRetrieveRequestByFileObject` extrae la siguiente solicitud correspondiente de una cola manual o secuencial y deja la salida intacta si no hay coincidencia.

Un `EvtIoCanceledOnQueue` configurado recibe `(WDFQUEUE, WDFREQUEST)` solo para una solicitud que el controlador recibió antes y luego reenvió o volvió a encolar, o que la devolución de llamada del contexto del solicitante encoló expresamente. El marco completa con `STATUS_CANCELLED`, sin llamar a esa función, las solicitudes encoladas que nunca entregó al controlador. La notificación devuelve la propiedad al controlador, que debe completar la solicitud dentro de la devolución de llamada o después y no puede volver a encolarla. La purga y la notificación de estado esperan a que vuelva la devolución de llamada y termine la solicitud.

`WdfIoQueueDrain` rechaza solicitudes nuevas con `STATUS_INVALID_DEVICE_STATE` y entrega las ya encoladas. El callback se ejecuta cuando no quedan solicitudes en cola ni en posesión del controlador. El reenvío devuelve `STATUS_WDF_BUSY`; `WdfIoQueueStart` restaura la aceptación.

`WdfIoQueuePurge` cancela con `STATUS_CANCELLED` las solicitudes aún no entregadas y solicita cancelar en el IRP original las ya entregadas y marcadas como cancelables. La limpieza precede a la liberación del IRP. El callback de estado espera a las solicitudes en cola, las retenidas por el controlador y los callbacks de cancelación activos. El controlador completa las solicitudes no cancelables.

`WdfIoQueueStopSynchronously`, `WdfIoQueueDrainSynchronously` y `WdfIoQueuePurgeSynchronously` suspenden al llamador invitado en `PASSIVE_LEVEL` hasta que terminen las solicitudes pertinentes. Stop sigue aceptando pero detiene la entrega y espera las solicitudes ya entregadas. Drain rechaza solicitudes nuevas, entrega las pendientes y espera ambas clases. Purge cancela las pendientes y las marcadas como cancelables y espera el retorno de los callbacks de cancelación. Si no hay origen de finalización, la espera informa un error de modelo bloqueado.

`WdfIoQueueStopAndPurge` y `WdfIoQueueStopAndPurgeSynchronously` cancelan las solicitudes que ya estaban en cola y las del controlador marcadas como cancelables, y siguen aceptando solicitudes nuevas sin entregarlas hasta `WdfIoQueueStart`. El callback de estado asíncrono y la espera síncrona terminan tras las solicitudes originales y sus callbacks de cancelación; las nuevas quedan en cola sin retrasar la notificación. Ambas formas de Stop vuelven a habilitar la aceptación tras Drain o Purge.

Los parámetros de solicitud usan el diseño `WDF_REQUEST_PARAMETERS` de 40 bytes. Los accesores de entrada/salida devuelven las longitudes lógicas, conservando los alias de búfer y los mapeos MDL existentes de E/S directa; la entrada de un IOCTL directo sigue usando búfer. Las direcciones incorrectas y los búferes insuficientes devuelven los estados documentados. La finalización limpia la solicitud mientras los búferes siguen válidos, completa el IRP y libera las páginas bloqueadas de la solicitud; después destruye los objetos hijos y la solicitud cuando las referencias lo permiten. Se rechazan nuevos accesos mediante funciones de búferes y parámetros desde que empieza la finalización; los punteros de búfer ya obtenidos siguen siendo utilizables durante la limpieza. Una referencia externa conserva el contexto, no el acceso al IRP completado.

La cancelación se modela para las solicitudes de las colas de dispositivos de control descritas arriba. Si ya se canceló la solicitud, `WdfRequestMarkCancelableEx` devuelve `STATUS_CANCELLED` sin invocar un callback. Si `WdfRequestUnmarkCancelable` tiene éxito, elimina el callback; una cancelación posterior solo registra el estado cancelado. `WdfRequestIsCanceled` consulta ese estado en una solicitud activa sin marca de cancelable. Después de marcar con éxito, la finalización exige desmarcar con éxito o haber iniciado la entrega del callback de cancelación; estar encolado no basta. Una vez entregado, el callback puede coordinar la finalización con un elemento de trabajo, incluso mientras espera. Una referencia interna independiente conserva la solicitud hasta que vuelva el callback. La finalización invalida primero el IRP y la continuación final de destrucción puede esperar. Tienen prioridad los DPC, luego los callbacks de cancelación en orden FIFO y después el trabajo ordinario; los callbacks de cancelación también preceden a la reanudación de esperas pasivas listas.

Para una solicitud ya cancelada, el antiguo `WdfRequestMarkCancelable`, de tipo void, ejecuta un callback invitado síncrono antes de retornar. La continuación hija puede esperar, completar mediante limpieza anidada y ejecutar la destrucción final antes de reanudar la API. La cancelación posterior al registro usa la ruta planificada anterior. Esto reproduce el código público con `PASSIVE_LEVEL` y `WdfSynchronizationScopeNone`; [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) exige usar Ex en controladores sin sincronización automática. Es comportamiento de compatibilidad, no una recomendación para usar la API antigua en esa configuración.

`WdfRequestGetInformation` y `WdfRequestSetInformation` comparten el campo original de 64 bits `IRP.IoStatus.Information`, incluidas las escrituras directas del invitado. Set solo asigna; la longitud de transferencia se valida al completar. `WdfRequestCompleteWithInformation` escribe el mismo campo antes de la limpieza; los cambios durante la limpieza mediante un IRP guardado previamente determinan la Information final, aunque GetInformation ya devuelva cero en esa fase. `WdfRequestGetIoQueue` devuelve la cola de origen. Con la configuración de archivos predeterminada, `WdfRequestGetFileObject` devuelve NULL sin inventar un objeto de archivo WDF a partir del FILE_OBJECT WDM. `WdfRequestWdmGetIrp` devuelve el mismo IRP; `IoCompleteRequest`/`IofCompleteRequest` invitados no pueden eludir la finalización WDF. Mientras el identificador siga válido durante o después de completar, GetInformation/GetIoQueue devuelven cero; recuperar una MDL primero pone NULL en la salida válida y después devuelve `STATUS_INTERNAL_ERROR`. SetInformation/GetFileObject/WdmGetIrp siguen rechazándose entonces. No cambian las restricciones existentes de accesores de búferes y parámetros.

`WdfRequestRetrieveInputWdmMdl` y `WdfRequestRetrieveOutputWdmMdl` describen bajo demanda el SystemBuffer existente para entrada WRITE, salida READ y entrada/salida IOCTL con búfer. Cada dirección debe ser válida y no vacía antes de usar el único descriptor en caché por solicitud; la primera recuperación fija ByteCount aunque la otra dirección tenga distinta longitud lógica. `MmGetSystemAddressForMdlSafe` devuelve su VA original; se rechazan mapeos adicionales, desmapeos y liberación por el controlador. La salida directa READ/IOCTL y la entrada directa WRITE devuelven el `IRP.MdlAddress` existente sin mapearlo por el mero hecho de recuperarlo; la entrada IOCTL directa usa la caché SystemBuffer. Descriptores, IRP y búferes caducan al completar. Las referencias internas de cancelación o externas solo conservan el contexto WDF. La recuperación de MDL de WDF para `METHOD_NEITHER` sigue sin admitirse, al igual que los PFN de MDL sin construir; WDFMEMORY de la solicitud usa otra asignación de páginas bloqueadas.

Para solicitudes KMDF con búfer, directas y neither, `WdfDeviceInitSetIoInCallerContextCallback` se ejecuta antes de la cola en el proceso solicitante a `PASSIVE_LEVEL`. El callback debe completar la solicitud o llamar una vez a `WdfDeviceEnqueueRequest` para la cola predeterminada. Para IOCTL `METHOD_NEITHER` y READ/WRITE neither, `WdfRequestRetrieveUnsafeUserInputBuffer` y `WdfRequestRetrieveUnsafeUserOutputBuffer` devuelven las direcciones originales solo en este callback. `WdfRequestProbeAndLockUserBufferForRead` y `WdfRequestProbeAndLockUserBufferForWrite` comprueban permisos y bloquean páginas de la solicitud; `WdfMemoryGetBuffer` devuelve un alias del sistema válido en el callback de la cola fuera del contexto solicitante. La finalización libera páginas y alias. Se admiten punteros incrustados a `user_buffers` explícitos; los mapeos arbitrarios siguen sin admitirse.

Las solicitudes neither pueden declarar `user_buffers` (`id`, `size`, `input`/`access` opcionales) y `user_pointers`. `source`/`target` indican `buffer` (`input`, `output`, `memory`) y `offset`; solo `memory` utiliza un `id` local a la solicitud. Las posiciones de puntero de ocho bytes no pueden solaparse; el destino puede estar dentro del búfer o al final. Se permiten 64 búferes adicionales y 256 punteros por escenario, con 64 KiB por búfer dentro del presupuesto total de 512 KiB. Los destinos iguales comparten bytes reales y las reglas existentes de permisos, bloqueo, revocación y salida del proceso. `configuration.user_memory` conserva las declaraciones. `user_buffers[].backing_hex` es una captura RAM diagnóstica al detenerse, incluso tras revocación o fallo; no sustituye a `output_hex` ni concede acceso. `id`: 1–64 ASCII, `[A-Za-z0-9][A-Za-z0-9_.-]*`.

`WdfRequestRetrieveInputMemory` y `WdfRequestRetrieveOutputMemory` exponen vistas WDFMEMORY de la solicitud sobre los búferes existentes de E/S con búfer o directas. Consultar otra vez la misma dirección conserva el identificador; `WdfMemoryGetBuffer` devuelve el búfer original y su longitud lógica. Una longitud cero o una dirección incorrecta produce un estado WDF de error; la E/S neither aún requiere comprobación y bloqueo de páginas en el contexto del solicitante. Estas vistas prestadas no añaden bloqueo MDL y caducan al completarse la solicitud.

Un subconjunto KMDF PnP limitado admite un FDO conectado directamente al PDO. `EvtDriverDeviceAdd` recibe un inicializador propiedad del framework; `WdfDeviceCreate` crea el FDO y `WdfFdoInitWdmGetPhysicalDevice` y `WdfDeviceWdmGetPhysicalDevice` conservan la identidad del PDO. Tras fallar AddDevice o terminar Remove, el framework ejecuta los callbacks de limpieza/destrucción y elimina el dispositivo. Las colas PnP admiten `WdfUseDefault` y `WdfTrue` para la gestión de energía; `WdfFalse` sigue sin gestión automática. `WdfDeviceInitSetPnpPowerEventCallbacks` admite `EvtDevicePrepareHardware`, `EvtDeviceD0Entry`, `EvtDeviceD0Exit` y `EvtDeviceReleaseHardware`. PrepareHardware recibe dos listas de recursos distintas antes de D0Entry, vacías para dispositivos sin recursos; Para las listas vacías, `WdfCmResourceListGetCount` devuelve cero y `WdfCmResourceListGetDescriptor` devuelve NULL. Un fallo en PrepareHardware o D0Entry pasa a ser el estado de START y aun así se ejecuta ReleaseHardware. Al detener o retirar desde D0, D0Exit precede a ReleaseHardware y al final del IRP. Los dispositivos `register_bank` configurados exponen descriptores `CM_PARTIAL_RESOURCE_DESCRIPTOR` de solo lectura hasta ReleaseHardware; el controlador puede mapear la memoria traducida con `MmMapIoSpace` y debe desmapearla antes de completar STOP o Remove. Otros tipos de recursos y la política general siguen fuera del alcance. `WdfIoQueuePnpHeld` indica la suspensión de la entrega antes de D0 y tras salir de D0; antes de salir de D0, `EvtIoStop` recibe cada solicitud retenida por el controlador en una cola administrada por energía; puede completarla, llamar a `WdfRequestStopAcknowledge` o esperar a una fuente de finalización ya programada. Sin `EvtIoStop` registrado, el framework también espera a que terminen todas las solicitudes entregadas. TRUE la devuelve a la cola para entregarla tras reiniciar; FALSE conserva la propiedad del controlador y llama a `EvtIoResume` al volver a D0. Las solicitudes en cola esperan al reinicio. Si no queda ninguna fuente de finalización, la espera termina con un `model_error` por estancamiento. El fixture WDK `driver_kmdf_pnp.c` usa `NEVERD_KMDF_PNP_FIXTURE` / `NEVERD_KMDF_PNP_CFG_FIXTURE`.

API KMDF modeladas: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfWdmDeviceGetWdfDeviceHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetDeviceType`, `WdfDeviceInitSetExclusive`, `WdfDeviceInitSetFileObjectConfig`, `WdfDeviceInitSetIoType`, `WdfDeviceInitSetIoInCallerContextCallback`, `WdfDeviceInitSetPnpPowerEventCallbacks`, `WdfCmResourceListGetCount`, `WdfCmResourceListGetDescriptor`, `WdfFdoInitWdmGetPhysicalDevice`, `WdfFdoInitSetFilter`, `WdfDeviceCreate`, `WdfDeviceEnqueueRequest`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfDeviceWdmGetAttachedDevice`, `WdfDeviceWdmGetPhysicalDevice`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfDeviceGetDriver`, `WdfDeviceGetIoTarget`, `WdfIoQueueGetDevice`, `WdfIoQueueGetState`, `WdfIoQueueStop`, `WdfIoQueueStopSynchronously`, `WdfIoQueueStart`, `WdfIoQueueDrain`, `WdfIoQueueDrainSynchronously`, `WdfIoQueuePurge`, `WdfIoQueuePurgeSynchronously`, `WdfIoQueueStopAndPurge`, `WdfIoQueueStopAndPurgeSynchronously`, `WdfIoQueueReadyNotify`, `WdfIoQueueRetrieveNextRequest`, `WdfIoQueueRetrieveRequestByFileObject`, `WdfIoQueueFindRequest`, `WdfIoQueueRetrieveFoundRequest`, `WdfRequestForwardToIoQueue`, `WdfRequestRequeue`, `WdfRequestStopAcknowledge`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestFormatRequestUsingCurrentType`, `WdfRequestSend`, `WdfRequestGetStatus`, `WdfRequestSetCompletionRoutine`, `WdfRequestGetCompletionParams`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputMemory`, `WdfRequestRetrieveOutputMemory`, `WdfRequestRetrieveUnsafeUserInputBuffer`, `WdfRequestRetrieveUnsafeUserOutputBuffer`, `WdfRequestProbeAndLockUserBufferForRead`, `WdfRequestProbeAndLockUserBufferForWrite`, `WdfMemoryGetBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfFileObjectGetFileName`, `WdfFileObjectGetFlags`, `WdfFileObjectGetDevice`, `WdfFileObjectWdmGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

Para un FDO PnP, `WdfDeviceInitSetDeviceType` guarda el tipo de 32 bits indicado en el `DEVICE_OBJECT` de WDM; sin esa llamada, el valor predeterminado sigue siendo `FILE_DEVICE_UNKNOWN`. No se modelan los aumentos de prioridad de E/S dependientes del tipo.

`WdfDeviceInitSetExclusive` establece `DO_EXCLUSIVE` en el dispositivo WDM creado a partir del inicializador. Un dispositivo de control con nombre rechaza una segunda apertura independiente hasta que se cierra el primer archivo. En un FDO PnP, esta marca por sí sola no hace exclusivo al PDO con nombre ni a toda la pila; la exclusividad del PDO definida en un INF queda fuera de este perfil.

`WdfDeviceInitSetFileObjectConfig` registra `EvtDeviceFileCreate`, `EvtFileCleanup` y `EvtFileClose` antes de crear el dispositivo y copia los atributos opcionales de contexto. Este perfil admite `WdfFileObjectNotRequired`, `WdfFileObjectWdfCanUseFsContext`, `WdfFileObjectWdfCanUseFsContext2` y `WdfFileObjectWdfCannotUseFsContexts`. El campo de contexto WDM elegido guarda el identificador WDF hasta un CREATE fallido o CLOSE y debe estar vacío al principio. Con `WdfFileObjectCanBeOptional`, `WdfRequestGetFileObject` devuelve NULL para una solicitud de E/S sin un objeto de archivo WDM coincidente. CREATE, CLEANUP y CLOSE aún requieren un objeto de archivo WDM; `WdfFdoInitSetFilter` habilita el reenvío predeterminado de filtros; `WdfTrue` lo habilita explícitamente y `WdfFalse` lo deshabilita. El reenvío de solicitudes de archivo requiere una ruta FDO/PDO directa y un `bus_completion` explícito. Un `delay_100ns` positivo se admite para el reenvío automático de CREATE/CLEANUP/CLOSE y para CREATE síncrono, asíncrono con rutina de finalización o con `SEND_AND_FORGET`. Las devoluciones CLEANUP y CLOSE se ejecutan antes del envío inferior. Una devolución CREATE con `WdfFileObjectNotRequired` puede obtener el destino local propiedad del dispositivo mediante `WdfDeviceGetIoTarget` y reenviar la solicitud original con `WdfRequestSend` usando únicamente `WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET`. El PDO conservado consume la respuesta explícita del bus y controla la finalización, incluso cuando la respuesta inferior se retrasa. Una devolución CREATE con un objeto de archivo del framework puede usar `WDF_REQUEST_SEND_OPTION_SYNCHRONOUS`, leer el resultado inferior con `WdfRequestGetStatus` y completar la solicitud original con ese estado. El envío asíncrono predeterminado primero prepara la solicitud con `WdfRequestFormatRequestUsingCurrentType` y registra `WdfRequestSetCompletionRoutine`. Su callback recibe `WDF_REQUEST_COMPLETION_PARAMS`, puede consultar el resultado inferior mediante `WdfRequestGetCompletionParams` o `WdfRequestGetStatus` y completa la solicitud original con un estado propio que puede diferir del estado inferior. En un envío con rutina de finalización, la respuesta diferida del PDO mantiene pendiente la solicitud hasta el plazo virtual y luego encola la rutina con el estado final inferior. Un `WDF_REQUEST_SEND_OPTION_TIMEOUT` relativo para un CREATE asíncrono compite con la demora de la respuesta inferior: si vence primero, la devolución recibe `STATUS_IO_TIMEOUT`; en caso de empate prevalece la respuesta inferior. Cero desactiva el plazo; no se admiten plazos absolutos ni otras opciones de envío. El identificador WDF y el `FILE_OBJECT` WDM conservan identidades distintas, consultables con `WdfRequestGetFileObject`, `WdfFileObjectGetDevice` y `WdfFileObjectWdmGetFileObject`. Un CREATE fallido elimina el objeto WDF sin llamar a los callbacks de limpieza o cierre; tras una apertura correcta, CLEANUP y CLOSE preceden a la limpieza y destrucción del contexto.

Un `WdfRequestSend` síncrono demorado conserva y suspende el marco real de llamada del invitado, y lo reanuda con el resultado booleano de envío tras la finalización. El NTSTATUS final se obtiene con `WdfRequestGetStatus`. Un `WDF_REQUEST_SEND_OPTION_TIMEOUT` relativo también se aplica a CREATE síncrono: un vencimiento anterior produce `STATUS_IO_TIMEOUT`, el empate favorece la respuesta inferior y cero desactiva el plazo. El reenvío automático ejecuta las devoluciones de archivo antes del envío inferior y conserva el IRP y el archivo WDM durante las devoluciones de limpieza y destrucción requeridas para completar. Las referencias WDF externas conservan el contexto, pero no prolongan la vida del archivo WDM de una solicitud terminada.

CREATE/CLEANUP/CLOSE WDM reenviados a un PDO configurado también aceptan un `bus_completion` explícitamente demorado. La rutina real de finalización del invitado se ejecuta tras el plazo inferior y conserva la propagación del estado pendiente y la responsabilidad del estado final.

La validación WDK opcional compila por separado `driver_kmdf_lifecycle.c` y `driver_kmdf_control.c` con la biblioteca real de entrada KMDF. `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` seleccionan imágenes de ciclo de vida; `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` seleccionan las imágenes normal/CFG activo del dispositivo de control. La ausencia de artefactos externos produce omisiones explícitas. Consulte las [pruebas](testing.md) para la cobertura nativa y C API/CLI. La evidencia actual de ejecución se limita a hosts Linux.

El modelo inicial de API tiene deliberadamente un contrato limitado:

| API | Comportamiento modelado y restricciones |
|-----|----------------------------------------|
| `RtlInitUnicodeString` | Construye una `UNICODE_STRING` del invitado para una fuente acotada terminada en NUL |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | Copia UTF-16 de longitud explícita y comparación sensible a mayúsculas; la comparación sin distinguir mayúsculas necesita una tabla de conversión de Windows y detiene la ejecución |
| `ExRaiseStatus`, `ExRaiseAccessViolation`, `ExRaiseDatatypeMisalignment` | Generan una excepción del invitado para manejadores C `__except` constantes admitidos; no hay retorno normal de API, y filtros/finally y recuperación de fallos CPU siguen sin admitirse |
| `ExAllocatePool2` | Asignaciones NX paginadas/no paginadas, inicializadas a cero por defecto; se modelan los indicadores de memoria sin inicializar y de alineación a la caché; los indicadores requeridos inválidos devuelven NULL; los pools con cuotas/ejecutables y las excepciones de asignación detienen la ejecución |
| `MmGetSystemRoutineAddress` | Resuelve un nombre del invitado de longitud explícita mediante el inventario compartido de exportaciones |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | Árbol explícito del registro limitado a la sesión, derechos por handle, consultas con tamaños de salida exactos y duración tras la eliminación; véanse los escenarios del registro |
| `MmMapIoSpace`, `MmMapIoSpaceEx`, `MmUnmapIoSpace` | Mappings no almacenados en caché de subrangos declarados, alias compartidos RO/RW y desasignación con base/longitud exactas |
| `IoGetDmaAdapter` | Descripción explícita Internal de maestro de bus versión 0/1 y tabla vinculada de versión uno a PASSIVE_LEVEL; versiones posteriores devuelven NULL |
| `AllocateCommonBuffer`, `FreeCommonBuffer`, `GetDmaAlignment`, `PutDmaAdapter` | Métodos de tabla del adaptador sobre RAM coherente compartida, identidad exacta de asignación y vida independiente del adaptador |
| `GetScatterGatherList`, `PutScatterGatherList` | Métodos de tabla a DISPATCH_LEVEL; callback real directo o en cola por recursos, vista MDL fijada y liberación explícita del mapping |
| `AllocateAdapterChannel`, `MapTransfer`, `FlushAdapterBuffers`, `FreeMapRegisters` | Reserva de canal y callback con acción de 32 bits, mapas de fragmentos y flush agregado; cuota común con SG |
| `KeFlushIoBuffers` | MDL vivo bloqueado/no paginado y caché coherente; no libera mapeos ni registros |
| `IoConnectInterrupt`, `IoDisconnectInterrupt`, `IoConnectInterruptEx`, `IoDisconnectInterruptEx` | Línea latched exclusiva asignada exactamente, ABI heredado y Ex 1/2/4 a PASSIVE_LEVEL; conexión opaca y duración precisa por generación |
| `KeSynchronizeExecution`, `KeAcquireInterruptSpinLock`, `KeReleaseInterruptSpinLock` | Callback BOOLEAN real y mismo bloqueo no recursivo al DIRQL asignado; restauración del IRQL y propiedad originales |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | MDL de solicitud con asignaciones KernelMode en caché y permisos; los MDL del pool no paginado reutilizan la asignación original mediante el auxiliar seguro |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `MmProbeAndLockPages`, `MmUnlockPages`, `IoFreeMdl` | Descriptores de pool no paginado/usuario independientes o asociados a IRP, enlaces de cadena modificables, bloqueos y alias del sistema independientes; sin cuotas |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Asignaciones de datos para los tipos de pool `0`, `1` y `512`; tamaño/tag positivos, tags coincidentes al liberar con tag y sin reutilización de direcciones |
| `IoCreateDevice`, `IoDeleteDevice` | Tipo de dispositivo `0x22`, características `0` o `0x100`, extensiones acotadas y nombres ASCII `\Device\Name` |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | Conexión del mismo controlador; devuelve el extremo superior anterior, desconectar recibe el inferior guardado; límites anteriores |
| `IofCallDriver`, `IoCallDriver` | Despacho al destino exacto en la ruta retenida; cursor validado y NTSTATUS inferior separado |
| `IoInitializeRemoveLockEx`, `IoAcquireRemoveLockEx` | Dueño exacto en extensión,32/120 bytes opacos, Tags NULL/repetidos |
| `IoReleaseRemoveLockEx`, `IoReleaseRemoveLockAndWaitEx` | Release coincidente y espera REMOVE reanudable tras recepción del proveedor |
| `PoCallDriver`, `PoStartNextPowerIrp` | Reenvío del IRP administrado; Vista+ sin protocolo adicional |
| `PoSetPowerState`, `PoRequestPowerIrp` | Notificación independiente y verdaderos hijos desde FIFO PDO explícitas; límites anteriores |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` o `\??\Name` dentro de un espacio de nombres de sesión, con destino `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Formato variádico Win64 verificado, con un máximo de 512 bytes de salida; todos los filtros del depurador habilitados |
| `IoGetCurrentIrpStackLocation` | Devuelve la ubicación de pila de la IRP modelada activa; las macros WDM compiladas normales leen el mismo campo del invitado |
| `KeGetCurrentIrql` | Lee el IRQL/CR8 actual, incluidos los aumentos y restauraciones explícitos; el despacho y el trabajo comienzan en `PASSIVE_LEVEL`, y los DPC en `DISPATCH_LEVEL` |
| `KfRaiseIrql`, `KeLowerIrql` | Importaciones reales de subida y bajada en WDK x64; el IRQL guardado se restaura en orden LIFO en la misma ejecución antes de retornar o suspender. Las lecturas de CR8 ven cada cambio; no se simula la interrupción entre instrucciones. |
| `KeInitializeSpinLock`, `KeAcquireSpinLockRaiseToDpc`, `KeReleaseSpinLock`, `KeAcquireSpinLockAtDpcLevel`, `KeReleaseSpinLockFromDpcLevel`, `KeTryToAcquireSpinLockAtDpcLevel` | Spinlocks ejecutivos residentes y alineados en CPU0; se comprueban el propietario, la liberación correspondiente y la restauración de IRQL. La adquisición bloqueante con contención se detiene explícitamente. |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | Elementos opacos asociados al dispositivo; solo `DelayedWorkQueue`, con dispositivo y contexto a `PASSIVE_LEVEL`; no se puede liberar un elemento aún en cola |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | DPC opaco, cuatro argumentos invitados, `DISPATCH_LEVEL`, duplicados/retirada e importancia; solo destino CPU0 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | Temporizadores de notificación/sincronización; vencimientos relativos/absolutos en 100 ns, períodos en milisegundos, rearme/cancelación y señales en tiempo virtual |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | Eventos de notificación/sincronización con consumo distinto; `KeSetEvent` solo acepta Increment=0 y Wait=FALSE |
| `KeInitializeSemaphore`, `KeReleaseSemaphore`, `KeReadStateSemaphore` | Semáforo contador residente con límite positivo; cada espera correcta consume una unidad. La liberación admite Increment=0 y Wait=FALSE; superar el límite genera `STATUS_SEMAPHORE_LIMIT_EXCEEDED`. |
| `KeInitializeMutex`, `KeReleaseMutex`, `KeReadStateMutex` | KMUTEX residente con adquisición recursiva por ejecución; KeReleaseMutex devuelve el estado con signo anterior, exige el propietario y el mismo contexto DISPATCH_LEVEL, y solo admite Wait=FALSE. Un mutex poseído impide retornar, reinicializar o liberar su almacenamiento. La liberación por otro propietario genera `STATUS_MUTANT_NOT_OWNED`. |
| `PsCreateSystemThread`, `PsTerminateSystemThread`, `ObReferenceObjectByHandle`, `ObfDereferenceObject`, `ZwClose` | Hilos acotados del proceso del sistema en PASSIVE_LEVEL. El identificador y la referencia al objeto de hilo opaco tienen vidas independientes; PsTerminateSystemThread termina sin volver y señala el objeto esperable. No se admiten APC, prioridades ni referencias de objeto tipadas. |
| `KeEnterCriticalRegion`, `KeLeaveCriticalRegion`, `KeEnterGuardedRegion`, `KeLeaveGuardedRegion`, `KeAreApcsDisabled`, `KeAreAllApcsDisabled` | Estado anidado de desactivación de APC por hilo. Las regiones críticas y un KMUTEX retenido bloquean APC normales; las regiones protegidas e IRQL >= APC_LEVEL bloquean todos. Los hilos del sistema comienzan dentro de una región crítica. Las salidas sin pareja y el retorno desequilibrado fallan; no se modela la entrega de APC. |
| `KeWaitForSingleObject` | Un evento, temporizador, semáforo o mutex inicializado; `KernelMode` no alertable, razón `Executive`; sondeo cero, espera finita relativa/absoluta o infinita; espera no nula/infinita requiere IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | Retardo relativo/absoluto `KernelMode` no alertable con IRQL <= APC_LEVEL; reanuda el marco invitado tras avanzar el tiempo virtual |
| `IoMarkIrpPending` | Marca el IRP activo; también se modela la escritura equivalente de la macro WDM en el control de pila; el despacho debe devolver `STATUS_PENDING` |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT`; desenrollado con detención/reanudación, retirando IRP/MDL/búferes solo en el límite final |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Operaciones acotadas sobre búferes del invitado, como máximo 1 MiB por llamada; las API de copia sin solapamiento rechazan los solapamientos |

Los límites IRQL proceden de `KernelAPIIRQL.def`; el modelo propietario comprueba las restricciones por argumento. Un DPC no puede llamar al registro ni asignar, liberar o acceder al pool paginado. Las conversiones Unicode de `DbgPrint` requieren `PASSIVE_LEVEL`; la salida ANSI y operaciones no paginadas admitidas funcionan a `DISPATCH_LEVEL`. Las pilas tienen límites: un puntero escapado no puede entrar en la pila de otro worker bloqueado. Los temporizadores armados en la extensión impiden retirar prematuramente el dispositivo.

El vencimiento satisface las esperas registradas antes de que un DPC reinicie o rearme el temporizador. Los DPC en cola se ejecutan antes de reanudar marcos `PASSIVE_LEVEL` despertados. Si el almacenamiento de una solicitud contiene un DPC en cola, la finalización IRP rechaza liberarlo antes de completar e invalidar el búfer.

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
El despacho debe completar de forma síncrona o cumplir el contrato anterior de finalización pendiente por callbacks. Las longitudes de salida inválidas y el acceso a un IRP completado provocan un fallo explícito. La descarga solicitada no debe dejar
ningún dispositivo, enlace simbólico, asignación de pool u objeto de archivo activo.

El campo raíz opcional `"load_address": "0x190000000"` solicita cambiar la base;
su omisión o `"0x0"` utiliza la dirección preferida. La imagen debe cumplir los
requisitos de reubicación. El comando de inicialización y la API C originales
no implican ningún escenario.

En la raíz solo se aceptan `load_address`, `requests`, `unload`,
`kernel_exports`, `registry` y `pnp_devices`. Las solicitudes ordinarias de archivo aceptan `kind`, `device` o `device_id` opcionales (excluyentes) y un `file` opcional. Los IOCTL requieren `code` y aceptan `input`, `output_size`
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

Solo las solicitudes READ/WRITE/IOCTL admiten `cancel_after_100ns`, un entero JSON opcional entre 0 e `INT64_MAX` (9223372036854775807). Programa la cancelación desde el envío de la solicitud, en unidades virtuales de 100 ns, no en tiempo real. En KMDF, cero aplica la cancelación después del enrutamiento del framework y antes del callback de E/S invitado; si el enrutamiento ya completó la solicitud, gana la finalización. En WDM, cero se aplica después de que regresa el despacho. Para retrasos positivos, el tiempo avanza hasta un vencimiento de temporizador, espera o cancelación solo si no hay callbacks ni contextos listos. Un IRP WDM pendiente llama a su rutina de cancelación registrada a `DISPATCH_LEVEL` con el bloqueo de cancelación adquirido; la rutina debe liberarlo con `Irp->CancelIrql` antes de completar. No se admite la cancelación general de colas ni PnP. Cada informe de solicitud incluye `cancel_requested_at_100ns`: el instante virtual absoluto en que ocurrió la cancelación, o null si no ocurrió, también cuando la finalización ganó primero. Solicitar cancelación no completa por sí solo un IRP ni determina su estado final.
`IoSetCancelRoutine`, `IoAcquireCancelSpinLock`, `IoReleaseCancelSpinLock` e `IoCancelIrp` comparten el estado del IRP y el bloqueo de cancelación; `IoCancelIrp` llama de forma síncrona a una rutina registrada e indica si se ejecutó.

El booleano opcional `user_unmap_after_dispatch` revoca las direcciones de usuario originales de una transferencia WDM neither no vacía después del retorno del despacho y antes del trabajo o la cancelación programados. Las páginas bloqueadas por MDL y sus alias del sistema siguen disponibles hasta desbloquearlas; fallan los punteros de usuario sin bloquear y los bloqueos nuevos. Si se revoca la salida, `output_hex` queda vacío. No se modelan reutilización ni tiempos arbitrarios de desasignación.

`requestor_process_id` identifica un proceso solicitante sintético (predeterminado 4096; entre 5 y `UINT32_MAX`). `IoGetRequestorProcessId` devuelve su ID para un IRP activo; `PsGetCurrentProcessId` devuelve ese ID durante el despacho directo o 4 en el trabajador del sistema modelado. Un cambio de proceso bloquea las direcciones de usuario originales ajenas, pero conserva los alias de sistema de MDL bloqueados. `requestor_exit_after_dispatch` revoca tras el despacho todas las direcciones de usuario originales de ese proceso y rechaza su nuevo I/O; CLEANUP/CLOSE explícitos siguen disponibles, sin inferir cancelación ni cierre automático de identificadores.

Un trabajador del sistema puede obtener el proceso opaco de un IRP activo con `IoGetRequestorProcess`, usar `KeStackAttachProcess` con un `KAPC_STATE` de kernel escribible para acceder temporalmente a las direcciones de usuario originales y llamar a `KeUnstackDetachProcess` con el mismo estado. `IoGetCurrentProcess` y `PsGetProcessId` reflejan el proceso adjunto; `PsGetCurrentProcessId` sigue siendo 4, el proceso que creó el trabajador. Un proceso finalizado, un IRP completado, un estado que no coincide, o esperar o completar un IRP durante la asociación produce un error explícito.

Para IOCTL directos, `input` inicializa el primer búfer del sistema y
`direct_input` inicializa el segundo búfer independiente descrito por el MDL,
rellenado con ceros hasta `output_size`. `METHOD_IN_DIRECT` requiere acceso de
lectura; no implica una asignación de memoria del sistema de solo lectura.
Ambos métodos usan búferes de escenario con lectura/escritura.
`MdlMappingNoWrite` elimina el permiso de escritura de la asignación y
`MdlMappingNoExecute` elimina el de ejecución. Desasignar revoca la dirección
virtual del sistema; volver a asignar conserva los mismos datos bloqueados.
La finalización caduca el MDL y la asignación. Se modelan los campos públicos
del MDL usados por macros WDM; se rechazan los campos de proceso y PFN de MDL sin construir, los MDL
construidos a mano, las asignaciones de usuario y el acceso directo mediante
el UserBuffer sin procesar. Un búfer directo de longitud cero tiene un MDL nulo.

`IoAllocateMdl` asigna metadatos para un búfer no vacío, sin desbordamiento y de hasta 1 MiB, sin comprobarlo ni bloquearlo. `Irp` puede ser NULL o un IRP modelado activo. Un descriptor principal sustituye la cabeza de la cadena del controlador; los descriptores separados siguen siendo suyos. `SecondaryBuffer` añade al final o se convierte en la cabeza si la cadena está vacía. El MDL direct-I/O original de la solicitud debe seguir siendo accesible y no puede sustituirse ni liberarse por el controlador. `ChargeQuota` debe ser FALSE; el agotamiento de la arena devuelve NULL. `MmBuildMdlForNonPagedPool` exige todo el rango en una asignación activa del pool no paginado. La función segura y las macros WDM reutilizan la dirección y los permisos originales, incluso con nuevos indicadores que prohíban escritura/ejecución. Se rechazan mappings adicionales del sistema y su eliminación. `IoFreeMdl` libera únicamente el descriptor indicado del controlador, sin desenlazarlo ni seguir `Next`. Un MDL de usuario requiere desbloqueo antes de liberarlo manualmente. El búfer del pool tiene una vida independiente; se permiten ambos órdenes de liberación sin acceder después al almacenamiento liberado.

El controlador puede modificar `MDL.Next` e `IRP.MdlAddress` para insertar o separar descriptores modelados. La finalización definitiva del IRP valida primero toda la cadena actual, desbloquea los MDL de usuario adjuntos, revoca sus alias y libera todos los descriptores adjuntos. Los descriptores separados y el almacenamiento del pool siguen perteneciendo al controlador. Los ciclos, enlaces desconocidos o liberados, descriptores compartidos entre IRP activos y descriptores privados WDF insertados en cadenas WDM fallan explícitamente. Las dependencias activas de DMA y dispatcher impiden la liberación prematura. Los demás campos MDL modelados y PFN construidos siguen siendo de solo lectura; campos de proceso, PFN no construidos, MDL manuales o parciales y mappings de usuario arbitrarios no se admiten. La descarga exige liberar todos los descriptores restantes del controlador.

Para todo método IOCTL con un `output_size` distinto de cero, `Information` no
puede superar `output_size`, aunque el búfer de entrada sea mayor. Sin búfer de
salida, `Information` puede contener un resultado de 64 bits propio del IOCTL;
no se copia ningún byte de salida. El informe conserva el valor exacto en
`information_hex`.

Para READ/WRITE, `DO_BUFFERED_IO` o `DO_DIRECT_IO` selecciona el método de
transferencia con búfer o directa. Si faltan ambos indicadores, la dirección
original de usuario aparece solo en `IRP.UserBuffer`: WRITE usa la entrada y
READ la salida; no se crea SystemBuffer ni MDL implícitamente. El controlador
debe comprobar y usar la dirección en el contexto del solicitante, o fijar las
páginas antes de posponer el trabajo. `user_input_access` se aplica a neither
WRITE y `user_output_access` a neither READ. Los derechos explícitos en
READ/WRITE con búfer o directa, y los indicadores en conflicto, detienen la
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
`IoMarkIrpPending`, `IoGetCurrentIrpStackLocation` y `MmGetSystemAddressForMdlSafe` son funciones auxiliares modeladas de las cabeceras WDM; esto no las declara exportadas de forma predeterminada, por lo que su disponibilidad como exportaciones requiere una importación estática o una declaración explícita en `kernel_exports`.

El texto del escenario se limita a 2 MiB, con un máximo de 64 solicitudes,
65536 bytes por búfer de entrada o salida y 512 KiB de bytes solicitados en
total, incluido el contenido de `direct_input`. Los presupuestos de instrucciones,
observaciones, memoria del invitado y tiempo se aplican a todo el escenario.
La arena de 1 MiB también contiene objetos y metadatos, por lo que una imagen
puede agotar la memoria del modelo antes de consumir el máximo de los búferes
del escenario.

## Escenarios del registro

El array opcional `registry` define un árbol concreto del registro limitado a
la sesión. Cada clave tiene un `path` obligatorio y un array `values` opcional;
cada valor contiene `name`, un entero sin signo `type` y datos hexadecimales
`data`. Un nombre vacío selecciona el valor predeterminado. Por ejemplo, un
valor DWORD se expresa como
`{"name":"Mode","type":4,"data":"01000000"}`. Los bytes se conservan exactamente;
el modelo no corrige terminadores de cadenas ni expande variables de entorno.

Las rutas deben ser absolutas y ASCII bajo `\Registry\Machine` o
`\Registry\User`. Las claves antecesoras se crean implícitamente. Las identidades
de claves y valores se comparan sin distinguir mayúsculas según las reglas
ASCII; se rechazan los nombres no ASCII y las identidades duplicadas. Omitir el
array deja sin especificar la disponibilidad del registro y las llamadas al
registro detienen la ejecución. `"registry": []` describe explícitamente un
espacio de nombres vacío. No se deduce del controlador ninguna clave, valor,
dato del registro anfitrión ni configuración del servicio.

`ZwOpenKey` y `ZwCreateKey` devuelven handles opacos independientes, con
comprobaciones de acceso por handle para consultar, escribir, crear subclaves y
eliminar. El árbol configurado concede los bits admitidos de `KEY_ALL_ACCESS`,
incluidas las máscaras habituales `KEY_READ` y `KEY_WRITE`. Es un árbol de prueba
explícitamente accesible, sin ACL de Windows ni evaluación de privilegios.
No se admiten derechos genéricos, `MAXIMUM_ALLOWED`, vistas alternativas del
registro, descriptores de seguridad personalizados, clases ni enlaces
simbólicos. La creación relativa requiere un handle de la clave padre directa
con `KEY_CREATE_SUB_KEY`. Las claves de entrada no son volátiles; las nuevas
pueden serlo, y se rechaza una subclave no volátil de una clave volátil. No se
modelan reinicios ni persistencia en disco.

`ZwQueryValueKey` implementa las clases de información Basic, Full y Partial y
sus variantes Align64 definidas, incluidas longitudes exactas, datos alineados,
salida parcial y resultados distintos `STATUS_BUFFER_TOO_SMALL` y
`STATUS_BUFFER_OVERFLOW`. `ZwSetValueKey` y `ZwDeleteValueKey` solo modifican el
árbol de esta sesión. `ZwDeleteKey` rechaza una clave con subclaves activas;
los handles de una clave eliminada devuelven `STATUS_KEY_DELETED` hasta cerrarse.
`ZwClose` libera un handle independientemente de la clave, y la descarga
solicitada falla mientras queden handles del registro abiertos.

Los límites son 256 claves incluidas las antecesoras, 1024 valores en total,
65536 bytes por valor, 512 KiB de datos de valores en total, 1024 bytes ASCII por
ruta de clave, 256 bytes por nombre de valor y 256 handles abiertos a la vez.
La creación y modificación aplican los mismos límites que la validación previa
del escenario. `configuration.registry` conserva la entrada original en el
informe; `registry` enumera las rutas y valores de las claves activas al final,
incluidos los cambios observados antes de una detención. El estado del registro
sin especificar se representa como null. La volatilidad y las identidades de
handles no forman parte de esta instantánea de valores.

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

## Comprobación de aceptación con el ejemplo Zero

El [script de validación de Zero](../../scripts/validate_zero_driver_sample.py)
adicional compila sin modificar el ejemplo WDM público Zero de Pavel Yosifovich,
a partir de la revisión y los hashes de
[su manifiesto](../../unittests/emulation/fixtures/zero-validation.json).
Ejecute `python3 scripts/validate_zero_driver_sample.py` con los mismos requisitos
de herramientas. El código fuente, la licencia MIT, los comandos, el escenario
y el informe se conservan por defecto en `build-release/driver-validation/zero`.
Nueve solicitudes ejercitan lecturas directas que cruzan límites de página,
recuentos de escritura, estadísticas atómicas del invitado y un IOCTL con búfer
para estadísticas. El fallo de lectura de longitud cero del ejemplo y la
falta de un manejador CLEANUP siguen visibles; el código de salida esperado de
la CLI es 2, con cierre y descarga satisfactorios. El script de validación solo
tiene éxito cuando coinciden esos resultados exactos y todos los bytes de salida.

## Informes y SDK

El informe JSON distingue `stop_reason`, los campos que admiten null `nt_status`
y `nt_success`, el PC de detención y el recuento de instrucciones. Conserva las
llamadas de API y el estado observable recopilados antes de detenerse, incluidos
los objetos de dispositivo y las direcciones de callbacks del controlador.
Las direcciones del invitado son cadenas hexadecimales para que los consumidores
de JSON no pierdan precisión de 64 bits. El objeto `configuration` registra los
límites, el nombre de servicio, las sustituciones de `kernel_exports` y la
entrada `registry` de la ejecución. El perfil es
`wdm-x64-scheduled-v72`. `nt_status` sigue siendo el resultado de DriverEntry,
mientras que `scenario_success` describe conjuntamente la inicialización y las
solicitudes completadas. `phase`, `requests` y `unload_completed` identifican
las partes ejecutadas del ciclo de vida solicitado. Cada llamada de API y
escritura de CPU también registra su fase (`driver_entry`, `add_device:<ID>`, `request:N`, `callback:N` o
`unload`). Cada solicitud informa de los estados de despacho y E/S, finalización,
el valor Information y los bytes devueltos en `output_hex`. `preferred_image_base`
describe la base PE original. `security_cookie` es la dirección del invitado
de la cookie inicializada, o `"0x0"` si no se requería. Los campos de solicitud
son `kind`, `device`, `device_id`, `pnp`, `file`, `requestor_process_id`, `byte_offset`, `code`, `irp`, `completed`, `cancel_requested_at_100ns`, `dispatch_status`, `io_status`,
`information`, `information_hex` y `output_hex`. `information` conserva su forma
numérica; `information_hex` es una cadena hexadecimal con prefijo `0x` que
conserva exactamente todos los bits del resultado sin signo de 64 bits. Use
`information_hex` si el consumidor JSON no conserva la precisión de los enteros
de 64 bits, especialmente para IOCTL sin búfer de salida.

Las observaciones del trabajo usan la fase `callback:N`. Una solicitud pendiente conserva `STATUS_PENDING` en `dispatch_status`; el estado final se registra por separado en `io_status` y determina su contribución a `scenario_success`.

`ExRaiseStatus` pasa los 32 bits bajos del NTSTATUS al manejador de excepciones del invitado; `ExRaiseAccessViolation` y `ExRaiseDatatypeMisalignment` generan `STATUS_ACCESS_VIOLATION` y `STATUS_DATATYPE_MISALIGNMENT`. El perfil sigue las páginas DDI individuales de Microsoft: ExRaiseStatus permite `APC_LEVEL`, mientras que las dos rutinas sin argumentos requieren `PASSIVE_LEVEL`. Algunas anotaciones SAL del WDK permiten APC_LEVEL para esas dos rutinas; este perfil mantiene el límite documentado más estricto. Una llamada que genera una excepción conserva `result: null` y registra el código en `detail`; nunca informa de un retorno satisfactorio de la API.

La entrega de excepciones usa las tablas unwind x64 de versión uno decodificadas de la imagen y ámbitos constantes `EXCEPTION_EXECUTE_HANDLER` de `__C_specific_handler`. Ejecuta el cuerpo real del manejador del invitado, admite desenrollar marcos auxiliares ordinarios, restaura registros generales no volátiles guardados y preserva el límite de pila de la ejecución actual. `GetExceptionCode()` observa el código generado. Un manejador puede generar otra excepción hacia un ámbito envolvente admitido. Las funciones de filtro, `__finally`, personalidades GS/C++, metadatos encadenados o incompletos, desenrollado de prólogos y restauración XMM encontrados fallan explícitamente. Una excepción API no capturada se detiene con `model_error`; los fallos CPU de memoria, interrupción o instrucción inválida siguen siendo terminales.

El fixture original `driver_wdm_seh.c` usa cabeceras WDK auténticas y `/GS-`. Configure `NEVERD_WDM_SEH_FIXTURE` y `NEVERD_WDM_SEH_CFG_FIXTURE` para imágenes normal y con CFG activo. El ejemplo [driver-seh-scenario.json](../examples/driver-seh-scenario.json) reubica la imagen, captura una excepción API en DriverEntry y descarga el controlador. La ruta WDM separada para METHOD_NEITHER admite sondeos, MDL bloqueados y fallos de memoria de usuario recuperables.

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

En WDM `METHOD_NEITHER`, `Type3InputBuffer` e `IRP.UserBuffer` apuntan a asignaciones de usuario separadas. `ProbeForRead` comprueba rango y alineación sin tocar páginas; `ProbeForWrite` toca cada página. `ExGetPreviousMode` devuelve el modo de la solicitud. `MmProbeAndLockPages` bloquea una asignación de usuario, `MmGetSystemAddressForMdlSafe` crea un alias compartido y `MmUnlockPages` revoca el alias y libera las páginas. No se modelan procesos arbitrarios.

Una solicitud WDM `METHOD_NEITHER` IOCTL con búfer no vacío puede establecer `user_input_access` y `user_output_access` por separado en `read_write` (predeterminado), `read_only` o `no_access`. Estos campos se rechazan para métodos con búfer o directos y búferes vacíos; `no_access` conserva el puntero pero impide acceder a las páginas.
El informe `configuration.user_page_access` conserva solo las protecciones explícitas, con `source_request_index` desde cero; una dirección omitida usa `read_write`.

## Solicitudes WDM concurrentes acotadas

Una solicitud WDM READ/WRITE/IOCTL o una solicitud de una cola KMDF paralela ilimitada puede establecer `defer_callback_drain: true`. Solo si el despacho devuelve `STATUS_PENDING` y el IRP sigue pendiente se envía la siguiente solicitud antes de ejecutar las devoluciones de llamada. Después de la siguiente solicitud sin este campo se ejecutan las devoluciones de llamada y se finaliza el lote; si la última solicitud lo establece, se hace al final del escenario. Las solicitudes superpuestas pueden usar objetos de archivo distintos o el mismo objeto abierto explícitamente en modo asíncrono. No se admiten superposiciones en un archivo síncrono, planificación preventiva arbitraria ni llegadas externas.

## Objeto de archivo asíncrono

Solo una solicitud CREATE puede establecer el booleano `asynchronous_file: true`; si se omite o es false, la apertura sigue siendo síncrona. La apertura asíncrona borra `FO_SYNCHRONOUS_IO` del `FILE_OBJECT` invitado y no establece `IRP_SYNCHRONOUS_API` en los IRP de archivo posteriores. Los READ/WRITE/IOCTL del mismo archivo asíncrono solo se superponen cuando se difiere explícitamente el procesamiento de las devoluciones de llamada. CLEANUP/CLOSE esperan a que todas las transferencias anteriores terminen y se finalicen. No se mantiene una posición implícita; `byte_offset` es propio de cada solicitud y vale cero por defecto. Los demás tipos rechazan el campo incluso con false.
