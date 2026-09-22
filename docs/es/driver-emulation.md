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
depuración, y el ejemplo público Zero de Pavel Yosifovich para READ/WRITE
directos y estadísticas. No demuestra compatibilidad con controladores
arbitrarios de terceros.

| Clase de controlador o requisito | Alcance actual | Entorno que falta |
|---------------------------------|----------------|-------------------|
| Controlador WDM de software x64 que utiliza las API enumeradas | Inicialización WDM x64 acotada, solicitudes seriales buffered/direct, trabajo, temporizadores, DPC, eventos y esperas, informes y límites | Cada API adicional ejecutada requiere un modelo definido |
| IOCTL `METHOD_BUFFERED` | E/S buffered/direct serial con finalización por trabajo o DPC | Solo las API siguientes; sin IRP simultáneos ni cancelación de solicitudes WDM |
| `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT` | MDL propios de cada solicitud y asignaciones de memoria del sistema | identidades de páginas físicas, DMA y asignaciones de memoria de usuario |
| MDL asignados por el controlador | Descriptores independientes del pool no paginado modelado, con las direcciones originales de los búferes | Asociación con IRP, cadenas MDL, sondeo/bloqueo, páginas físicas y asignaciones de usuario |
| READ/WRITE | E/S buffered/direct serial con finalización por trabajo o DPC | Solo las API siguientes; sin IRP simultáneos ni cancelación de solicitudes WDM; `METHOD_NEITHER` y posición implícita del archivo |
| `METHOD_NEITHER` | Rechazado | Contexto de direcciones de usuario, comprobación de accesos y gestión de excepciones del invitado |
| Controlador KMDF 1.33 no PnP | Vinculación, objetos/contextos, dispositivos de control con nombre, colas secuenciales predeterminadas y solicitudes con búfer/directas con callbacks ejecutados | Sin dispositivos PnP, planificación general de colas, extensiones de clase ni UMDF |
| Controlador PnP de bus, función o filtro | Conexión y despacho inferior del mismo controlador invitado modelados; ciclo PnP no admitido | Propiedad PDO/proveedor, ejecución AddDevice, IRP PnP y de energía |
| Controladores de almacenamiento, red, pantalla, sistema de archivos y minifiltros | Contratos de subsistemas no compatibles | Frameworks de puerto/clase/miniport, NDIS/WFP, servicios gráficos o del sistema de archivos |
| Trabajo, temporizadores, DPC, eventos y esperas | El IRQL actual es `PASSIVE_LEVEL` para despacho y trabajo, y `DISPATCH_LEVEL` para DPC | Solo las API siguientes; sin IRP simultáneos ni cancelación de solicitudes WDM |
| Operaciones del registro mediante las API Zw listadas | Árbol explícito de sesión y derechos por handle | ACL, privilegios, vistas alternativas y persistencia |
| Controlador con callbacks de proceso/hilo, otros handles, operaciones de archivo o descubrimiento de módulos del kernel | No compatible fuera de las API enumeradas | Administrador de objetos, estado del sistema y productores de callbacks/eventos |
| Controlador de hardware, DMA, PCI, interrupciones o virtualización | Entorno no compatible | Modelos de dispositivos, memoria física, buses, interrupciones y estado privilegiado de CPU |
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

Este modelo acotado no ofrece todo el comportamiento asíncrono de Windows. No admite esperas alertables o de usuario, hilos del sistema, APC, cancelación de solicitudes WDM, spinlocks, IRP simultáneos, cambios generales de IRQL, `METHOD_NEITHER`, UMDF, dispositivos PnP de KMDF y planificación general de colas, PnP/energía completo, hardware, DMA ni interrupciones. La inicialización sola ejecuta callbacks explícitamente encolados sin crear solicitudes ni descarga implícita.

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

Este subconjunto no añade PDO, ejecución de `AddDevice`, IRP PnP/energía ni IRP asignadas por el controlador. La conexión/reenvío WDF, conectar a pilas con archivos o callbacks activos, desconectar una capa intermedia, cambiar la función mayor y usar destinos fuera de la ruta fallan explícitamente. El fixture opcional `driver_wdm_stack.c`, compilado con WDK auténtico, usa `NEVERD_WDM_STACK_FIXTURE` y `NEVERD_WDM_STACK_CFG_FIXTURE`. Las pruebas nativas y C API/CLI incluyen reubicación; los artefactos ausentes se omiten explícitamente. La evidencia de ejecución sigue limitada a Linux.

KMDF 1.33 usa exactamente la ABI 1.33.0: 458 entradas de función tienen identidades estables en el invitado y las 38 API siguientes tienen semántica de ejecución. `WdfVersionBind` y `WdfVersionUnbind` gestionan la vinculación del invitado alrededor del wrapper real WDK `FxDriverEntry`. `WdfGetDriver` lee las variables globales públicas del controlador. Los controladores no PnP, objetos genéricos, dispositivos de control, colas y solicitudes entrantes comparten contextos tipados, contadores de referencias y callbacks ejecutados de limpieza/destrucción/descarga. Todas las llamadas y callbacks modelados del framework requieren actualmente `PASSIVE_LEVEL`; añadir referencias tras finalizar la limpieza sigue fuera de este perfil. Las entradas no modeladas, `WdfLdrQueryInterface`, las extensiones de clase y UMDF detienen explícitamente la ejecución.

Los dispositivos de control requieren un nombre ASCII imprimible copiado y exactamente la SDDL `D:P(A;;GA;;;WD)`. Esta concede acceso universal sin inventar un token del llamador; no se admiten otros descriptores de seguridad, dispositivos sin nombre ni nombres automáticos. La inicialización posee un dispositivo WDM. Las solicitudes pueden seleccionarlo mediante los alias de enlaces simbólicos `\DosDevices\Name` o `\??\Name` del espacio de nombres de sesión existente; el informe conserva el nombre canónico del dispositivo. Una creación correcta consume el objeto de inicialización y borra su puntero; una creación fallida revierte la propiedad parcial del dispositivo. `WdfControlFinishInitializing` habilita la entrega de E/S. La eliminación retira el dispositivo y sus enlaces solo cuando los archivos, elementos de trabajo y solicitudes modelados lo permiten; no se admite cancelar o vaciar solicitudes durante la eliminación.

La estructura `WDF_IO_QUEUE_CONFIG` de 96 bytes admite una cola secuencial predeterminada con ejecución pasiva explícita y sin sincronización del framework. Las colas de dispositivos de control no están sujetas a gestión de energía. Los callbacks específicos READ/WRITE/IOCTL tienen prioridad sobre el predeterminado. Las solicitudes aceptadas en la cola devuelven `STATUS_PENDING` incluso si se completan de forma síncrona; el registro de retorno de un callback void no completa su solicitud. La finalización diferida usa el planificador existente. Sin un manejador, la solicitud se completa con `STATUS_INVALID_DEVICE_REQUEST`; READ/WRITE de longitud cero se completa sin entrega salvo que esta se habilite. El paquete de archivos predeterminado completa CREATE/CLEANUP/CLOSE correctamente con Information=0. Siguen sin admitirse colas paralelas/manuales, callbacks de archivo, dispositivos PnP y PnP/energía completo.

Los parámetros de solicitud usan el diseño `WDF_REQUEST_PARAMETERS` de 40 bytes. Los accesores de entrada/salida devuelven las longitudes lógicas, conservando los alias de búfer y los mapeos MDL existentes de E/S directa; la entrada de un IOCTL directo sigue usando búfer. Las direcciones incorrectas y los búferes insuficientes devuelven los estados documentados. La finalización ejecuta la limpieza de la solicitud y la destrucción de sus hijos antes de invalidar el IRP y los búferes, y después destruye la solicitud cuando las referencias lo permiten. Se rechazan nuevos accesos mediante funciones de búferes y parámetros desde que empieza la finalización; los punteros de búfer ya obtenidos siguen siendo utilizables durante la limpieza. Una referencia externa conserva el contexto, no el acceso al IRP completado. `METHOD_NEITHER` de usuario sigue requiriendo soporte aún no implementado de contexto del llamador, comprobación de acceso y bloqueo.

La cancelación se modela para las solicitudes de las colas de dispositivos de control descritas arriba. Si ya se canceló la solicitud, `WdfRequestMarkCancelableEx` devuelve `STATUS_CANCELLED` sin invocar un callback. Si `WdfRequestUnmarkCancelable` tiene éxito, elimina el callback; una cancelación posterior solo registra el estado cancelado. `WdfRequestIsCanceled` consulta ese estado en una solicitud activa sin marca de cancelable. Después de marcar con éxito, la finalización exige desmarcar con éxito o haber iniciado la entrega del callback de cancelación; estar encolado no basta. Una vez entregado, el callback puede coordinar la finalización con un elemento de trabajo, incluso mientras espera. Una referencia interna independiente conserva la solicitud hasta que vuelva el callback. La finalización invalida primero el IRP y la continuación final de destrucción puede esperar. Tienen prioridad los DPC, luego los callbacks de cancelación en orden FIFO y después el trabajo ordinario; los callbacks de cancelación también preceden a la reanudación de esperas pasivas listas.

Para una solicitud ya cancelada, el antiguo `WdfRequestMarkCancelable`, de tipo void, ejecuta un callback invitado síncrono antes de retornar. La continuación hija puede esperar, completar mediante limpieza anidada y ejecutar la destrucción final antes de reanudar la API. La cancelación posterior al registro usa la ruta planificada anterior. Esto reproduce el código público con `PASSIVE_LEVEL` y `WdfSynchronizationScopeNone`; [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestmarkcancelable) exige usar Ex en controladores sin sincronización automática. Es comportamiento de compatibilidad, no una recomendación para usar la API antigua en esa configuración.

`WdfRequestGetInformation` y `WdfRequestSetInformation` comparten el campo original de 64 bits `IRP.IoStatus.Information`, incluidas las escrituras directas del invitado. Set solo asigna; la longitud de transferencia se valida al completar. `WdfRequestCompleteWithInformation` escribe el mismo campo antes de la limpieza; los cambios durante la limpieza mediante un IRP guardado previamente determinan la Information final, aunque GetInformation ya devuelva cero en esa fase. `WdfRequestGetIoQueue` devuelve la cola de origen. Con la configuración de archivos predeterminada, `WdfRequestGetFileObject` devuelve NULL sin inventar un objeto de archivo WDF a partir del FILE_OBJECT WDM. `WdfRequestWdmGetIrp` devuelve el mismo IRP; `IoCompleteRequest`/`IofCompleteRequest` invitados no pueden eludir la finalización WDF. Mientras el identificador siga válido durante o después de completar, GetInformation/GetIoQueue devuelven cero; recuperar una MDL primero pone NULL en la salida válida y después devuelve `STATUS_INTERNAL_ERROR`. SetInformation/GetFileObject/WdmGetIrp siguen rechazándose entonces. No cambian las restricciones existentes de accesores de búferes y parámetros.

`WdfRequestRetrieveInputWdmMdl` y `WdfRequestRetrieveOutputWdmMdl` describen bajo demanda el SystemBuffer existente para entrada WRITE, salida READ y entrada/salida IOCTL con búfer. Cada dirección debe ser válida y no vacía antes de usar el único descriptor en caché por solicitud; la primera recuperación fija ByteCount aunque la otra dirección tenga distinta longitud lógica. `MmGetSystemAddressForMdlSafe` devuelve su VA original; se rechazan mapeos adicionales, desmapeos y liberación por el controlador. La salida directa READ/IOCTL y la entrada directa WRITE devuelven el `IRP.MdlAddress` existente sin mapearlo por el mero hecho de recuperarlo; la entrada IOCTL directa usa la caché SystemBuffer. Descriptores, IRP y búferes caducan al completar. Las referencias internas de cancelación o externas solo conservan el contexto WDF. `METHOD_NEITHER` y el acceso a PFN físicos siguen sin admitirse.

API KMDF modeladas: `WdfDriverCreate`, `WdfDriverGetRegistryPath`, `WdfDriverWdmGetDriverObject`, `WdfWdmDriverGetWdfDriverHandle`, `WdfObjectGetTypedContextWorker`, `WdfObjectAllocateContext`, `WdfObjectContextGetObject`, `WdfObjectReferenceActual`, `WdfObjectDereferenceActual`, `WdfObjectCreate`, `WdfObjectDelete`, `WdfControlDeviceInitAllocate`, `WdfDeviceInitFree`, `WdfDeviceInitAssignName`, `WdfDeviceInitSetIoType`, `WdfDeviceCreate`, `WdfDeviceCreateSymbolicLink`, `WdfControlFinishInitializing`, `WdfDeviceWdmGetDeviceObject`, `WdfIoQueueCreate`, `WdfDeviceGetDefaultQueue`, `WdfIoQueueGetDevice`, `WdfRequestComplete`, `WdfRequestCompleteWithInformation`, `WdfRequestGetParameters`, `WdfRequestRetrieveInputBuffer`, `WdfRequestRetrieveOutputBuffer`, `WdfRequestRetrieveInputWdmMdl`, `WdfRequestRetrieveOutputWdmMdl`, `WdfRequestSetInformation`, `WdfRequestGetInformation`, `WdfRequestGetFileObject`, `WdfRequestGetIoQueue`, `WdfRequestWdmGetIrp`, `WdfRequestMarkCancelable`, `WdfRequestMarkCancelableEx`, `WdfRequestUnmarkCancelable`, `WdfRequestIsCanceled`.

La validación WDK opcional compila por separado `driver_kmdf_lifecycle.c` y `driver_kmdf_control.c` con la biblioteca real de entrada KMDF. `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` seleccionan imágenes de ciclo de vida; `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` seleccionan las imágenes normal/CFG activo del dispositivo de control. La ausencia de artefactos externos produce omisiones explícitas. Consulte las [pruebas](testing.md) para la cobertura nativa y C API/CLI. La evidencia actual de ejecución se limita a hosts Linux.

El modelo inicial de API tiene deliberadamente un contrato limitado:

| API | Comportamiento modelado y restricciones |
|-----|----------------------------------------|
| `RtlInitUnicodeString` | Construye una `UNICODE_STRING` del invitado para una fuente acotada terminada en NUL |
| `RtlCopyUnicodeString`, `RtlCompareUnicodeString`, `RtlEqualUnicodeString` | Copia UTF-16 de longitud explícita y comparación sensible a mayúsculas; la comparación sin distinguir mayúsculas necesita una tabla de conversión de Windows y detiene la ejecución |
| `ExAllocatePool2` | Asignaciones NX paginadas/no paginadas, inicializadas a cero por defecto; se modelan los indicadores de memoria sin inicializar y de alineación a la caché; los indicadores requeridos inválidos devuelven NULL; los pools con cuotas/ejecutables y las excepciones de asignación detienen la ejecución |
| `MmGetSystemRoutineAddress` | Resuelve un nombre del invitado de longitud explícita mediante el inventario compartido de exportaciones |
| `ZwOpenKey`, `ZwCreateKey`, `ZwQueryValueKey`, `ZwSetValueKey`, `ZwDeleteValueKey`, `ZwDeleteKey`, `ZwClose` | Árbol explícito del registro limitado a la sesión, derechos por handle, consultas con tamaños de salida exactos y duración tras la eliminación; véanse los escenarios del registro |
| `MmMapLockedPagesSpecifyCache`, `MmGetSystemAddressForMdlSafe`, `MmUnmapLockedPages` | MDL de solicitud con asignaciones KernelMode en caché y permisos; los MDL del pool no paginado reutilizan la asignación original mediante el auxiliar seguro |
| `IoAllocateMdl`, `MmBuildMdlForNonPagedPool`, `IoFreeMdl` | Descriptores independientes con todo el rango en una asignación activa del pool no paginado; duración independiente del descriptor y el búfer; sin IRP, cadenas ni cuotas |
| `ExAllocatePoolWithTag`, `ExFreePoolWithTag`, `ExFreePool` | Asignaciones de datos para los tipos de pool `0`, `1` y `512`; tamaño/tag positivos, tags coincidentes al liberar con tag y sin reutilización de direcciones |
| `IoCreateDevice`, `IoDeleteDevice` | Tipo de dispositivo `0x22`, características `0` o `0x100`, extensiones acotadas y nombres ASCII `\Device\Name` |
| `IoAttachDeviceToDeviceStack`, `IoDetachDevice` | Conexión del mismo controlador; devuelve el extremo superior anterior, desconectar recibe el inferior guardado; límites anteriores |
| `IofCallDriver`, `IoCallDriver` | Despacho al destino exacto en la ruta retenida; cursor validado y NTSTATUS inferior separado |
| `IoCreateSymbolicLink`, `IoDeleteSymbolicLink` | ASCII `\DosDevices\Name` o `\??\Name` dentro de un espacio de nombres de sesión, con destino `\Device\Name` |
| `DbgPrint`, `DbgPrintEx` | Formato variádico Win64 verificado, con un máximo de 512 bytes de salida; todos los filtros del depurador habilitados |
| `IoGetCurrentIrpStackLocation` | Devuelve la ubicación de pila de la IRP modelada activa; las macros WDM compiladas normales leen el mismo campo del invitado |
| `KeGetCurrentIrql` | El IRQL actual es `PASSIVE_LEVEL` para despacho y trabajo, y `DISPATCH_LEVEL` para DPC |
| `IoAllocateWorkItem`, `IoQueueWorkItem`, `IoFreeWorkItem` | Elementos opacos asociados al dispositivo; solo `DelayedWorkQueue`, con dispositivo y contexto a `PASSIVE_LEVEL`; no se puede liberar un elemento aún en cola |
| `KeInitializeDpc`, `KeInsertQueueDpc`, `KeRemoveQueueDpc`, `KeSetImportanceDpc`, `KeSetTargetProcessorDpc` | DPC opaco, cuatro argumentos invitados, `DISPATCH_LEVEL`, duplicados/retirada e importancia; solo destino CPU0 |
| `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeReadStateTimer` | Temporizadores de notificación/sincronización; vencimientos relativos/absolutos en 100 ns, períodos en milisegundos, rearme/cancelación y señales en tiempo virtual |
| `KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KeClearEvent`, `KeReadStateEvent` | Eventos de notificación/sincronización con consumo distinto; `KeSetEvent` solo acepta Increment=0 y Wait=FALSE |
| `KeWaitForSingleObject` | Un evento o temporizador inicializado; `KernelMode` no alertable, razón `Executive`; sondeo cero, espera finita relativa/absoluta o infinita; espera no nula/infinita requiere IRQL <= APC_LEVEL |
| `KeDelayExecutionThread` | Retardo relativo/absoluto `KernelMode` no alertable con IRQL <= APC_LEVEL; reanuda el marco invitado tras avanzar el tiempo virtual |
| `IoMarkIrpPending` | Marca el IRP activo; también se modela la escritura equivalente de la macro WDM en el control de pila; el despacho debe devolver `STATUS_PENDING` |
| `IofCompleteRequest`, `IoCompleteRequest` | `IO_NO_INCREMENT`; desenrollado con detención/reanudación, retirando IRP/MDL/búferes solo en el límite final |
| `memcpy`, `memmove`, `memset`, `memcmp`, `RtlCopyMemory`, `RtlMoveMemory`, `RtlFillMemory`, `RtlZeroMemory`, `RtlCompareMemory` | Operaciones acotadas sobre búferes del invitado, como máximo 1 MiB por llamada; las API de copia sin solapamiento rechazan los solapamientos |

Los límites IRQL proceden de `KernelAPIIRQL.def`; el modelo propietario comprueba las restricciones por argumento. Un DPC no puede llamar al registro ni asignar, liberar o acceder al pool paginado. Las conversiones Unicode de `DbgPrint` requieren `PASSIVE_LEVEL`; la salida ANSI y operaciones no paginadas admitidas funcionan a `DISPATCH_LEVEL`. Las pilas tienen límites: un puntero escapado no puede entrar en la pila de otro worker bloqueado. Los temporizadores armados en la extensión impiden retirar prematuramente el dispositivo. Estas comprobaciones no exponen cambios generales de IRQL.

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
`kernel_exports` y `registry`. Todas las solicitudes aceptan `kind`, un `device` opcional y
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

Solo las solicitudes READ/WRITE/IOCTL admiten `cancel_after_100ns`, un entero JSON opcional entre 0 e `INT64_MAX` (9223372036854775807). Programa la cancelación desde el envío de la solicitud, en unidades virtuales de 100 ns, no en tiempo real. Cero aplica la cancelación después del enrutamiento del framework y antes del callback de E/S invitado; si el enrutamiento ya completó la solicitud, gana la finalización. Para retrasos positivos, el tiempo avanza hasta un vencimiento de temporizador, espera o cancelación solo si no hay callbacks ni contextos listos. Configurar cancelación WDM detiene la ejecución con `model_error`; no se admite la cancelación general de colas ni PnP. Cada informe de solicitud incluye `cancel_requested_at_100ns`: el instante virtual absoluto en que ocurrió la cancelación, o null si no ocurrió, también cuando la finalización ganó primero. Solicitar cancelación no completa por sí solo un IRP ni determina su estado final.

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

`IoAllocateMdl` asigna metadatos independientes para un búfer no vacío, sin desbordamiento y de hasta 1 MiB; no sondea ni bloquea el búfer. `Irp` debe ser NULL y `SecondaryBuffer` y `ChargeQuota` deben ser FALSE. Si se agota la arena, devuelve NULL. `MmBuildMdlForNonPagedPool` exige que todo el rango pertenezca a una sola asignación activa del pool no paginado. El auxiliar seguro y las macros WDM normales reutilizan la dirección original, manteniendo los alias y los permisos existentes aunque se añadan indicadores que prohíban escritura o ejecución. Se rechazan las asignaciones de sistema adicionales y su liberación. `IoFreeMdl` invalida solo el descriptor; el búfer del pool tiene una duración independiente. Se permiten ambos órdenes de liberación si después no se utiliza memoria liberada. Todos los campos MDL modelados son de solo lectura; siguen sin admitirse acceso a proceso/PFN, cadenas ni cambios manuales de campos. La descarga debe liberar todos los descriptores del controlador.

Para todo método IOCTL con un `output_size` distinto de cero, `Information` no
puede superar `output_size`, aunque el búfer de entrada sea mayor. Sin búfer de
salida, `Information` puede contener un resultado de 64 bits propio del IOCTL;
no se copia ningún byte de salida. El informe conserva el valor exacto en
`information_hex`.

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
`wdm-x64-scheduled-v7`. `nt_status` sigue siendo el resultado de DriverEntry,
mientras que `scenario_success` describe conjuntamente la inicialización y las
solicitudes completadas. `phase`, `requests` y `unload_completed` identifican
las partes ejecutadas del ciclo de vida solicitado. Cada llamada de API y
escritura de CPU también registra su fase (`driver_entry`, `request:N`, `callback:N` o
`unload`). Cada solicitud informa de los estados de despacho y E/S, finalización,
el valor Information y los bytes devueltos en `output_hex`. `preferred_image_base`
describe la base PE original. `security_cookie` es la dirección del invitado
de la cookie inicializada, o `"0x0"` si no se requería. Los campos de solicitud
son `kind`, `device`, `file`, `byte_offset`, `code`, `irp`, `completed`, `cancel_requested_at_100ns`, `dispatch_status`, `io_status`,
`information`, `information_hex` y `output_hex`. `information` conserva su forma
numérica; `information_hex` es una cadena hexadecimal con prefijo `0x` que
conserva exactamente todos los bits del resultado sin signo de 64 bits. Use
`information_hex` si el consumidor JSON no conserva la precisión de los enteros
de 64 bits, especialmente para IOCTL sin búfer de salida.

Las observaciones del trabajo usan la fase `callback:N`. Una solicitud pendiente conserva `STATUS_PENDING` en `dispatch_status`; el estado final se registra por separado en `io_status` y determina su contribución a `scenario_success`.

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
