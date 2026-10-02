**Idiomas**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](../ar/process-emulation.md)

[← Índice de documentación](README.md)

# Emulación de procesos invitados

`neverd emulate` ejecuta una imagen bajo un perfil explícito de sistema operativo invitado. El transporte CPU, el análisis de la imagen, la entrada del proceso y los servicios del SO tienen propietarios separados. Activa `NEVERD_ENABLE_CPU_EMULATION=ON`; la emulación de controladores también lo incluye.

El primer perfil, `linux-elf64-v1`, ejecuta ELF `ET_EXEC` x64/AArch64 y PIE estáticos `ET_DYN` con autorrelocación en CPL3 o EL0. Carga segmentos ELF reales, construye la pila inicial, reanuda por intervalos y atiende solicitudes explícitas de llamadas al sistema Linux. Es un modelo de proceso independiente, no una distribución Linux completa ni una promesa de ejecutar binarios libc arbitrarios. El enlace dinámico, señales, hilos, sistemas de archivos y servicios no admitidos fallan explícitamente.

<!-- i18n-section: cli-sdk -->

## CLI y SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Hosts Linux compatibles seleccionan KVM y hosts Windows compatibles, WHP. Otras combinaciones de ISA anfitrión/invitada usan Unicorn. Un backend elegido que no esté disponible causa error, sin fallback silencioso. El ELF mantiene el perfil Linux aunque se ejecute en Windows. Consulta [ejecución CPU](cpu-execution.md) para el inventario de instrucciones y sus límites.

El CLI emite un único informe JSON. Código de salida 0: estado invitado cero; 2: otro estado; 3: ejecución incompleta (incluidos fallos y límites); 1: configuración/API inválida. El estado real aparece en `exit_status`. La entrada C aditiva [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h) recibe una sesión, ruta no vacía, perfil explícito y JSON de opciones opcional. Libera el resultado con `neverd_free_string`; NULL indica error de configuración, descrito por `neverd_last_error`. Un fallo invitado o parada de recursos devuelve un informe. No requiere ni modifica la imagen de análisis de la sesión.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

<!-- i18n-section: options-results -->

## Opciones y resultados

Las opciones son un objeto JSON de hasta 64 KiB. Se rechazan campos desconocidos/null, tipos inválidos, NUL incrustados y límites no positivos.

| Opción | Predeterminado | Contrato |
|---|---|---|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` o `whp` |
| `arguments` | Nombre del archivo | argv completo, incluido argv[0]; vacío usa el predeterminado |
| `environment` | `[]` | Cadenas explícitas del invitado; nunca hereda el entorno del host |
| `instruction_limit` | 100000 | Intentos de instrucción admitidos y compartidos |
| `event_limit` | 10000 | Eventos syscall, contabilizados antes de atender el servicio |
| `timeout_microseconds` | 5000000 | Deadline monotónica iniciada tras preparar el proceso |
| `memory_limit` | 67108864 | Presupuesto de memoria física/mapeada |
| `stack_size` | 1048576 | Pila alineada a página dentro del presupuesto |
| `output_limit` | 1048576 | Bytes combinados capturados de stdout/stderr |
| `instruction_quantum` | 1024 | Intervalo de admisión antes de ceder a la runtime |

`schema_version` vale 1. El informe incluye perfil, arquitectura, backend seleccionado y motivo, `stop_reason`, `exit_status` anulable, diagnóstico, PC de entrada/actual, contadores, registros de servicios y última salida CPU tipada. Direcciones, números syscall, registros de argumentos y bits de retorno son cadenas hexadecimales **sin** `0x`; `stdout_hex`/`stderr_hex` preservan NUL y UTF-8 inválido. Un resultado syscall null significa que no hay retorno modelado (por ejemplo, exit o solicitud no admitida), no un cero exitoso.

<!-- i18n-section: linux-semantics -->

## Semántica del perfil Linux

La política OS reutiliza las cabeceras de programa ya decodificadas por el cargador ELF. Comprueba etiquetas ABI, alineación de segmentos, tablas de cabeceras mapeadas y límites de direcciones de usuario. Un plan genérico valida extensiones, permisos, solapamientos y presupuesto antes de asignar memoria, y solo publica un espacio privado completamente preparado. Conserva prefijos/colas de páginas de archivo, pone BSS a cero, respeta permisos y reserva huecos de guarda para la pila. Rechaza diseños con páginas solapadas y cabeceras contradictorias; no adivina.

El PIE estático usa un load bias determinista de al menos `0x40000000`, aumentado para respetar la alineación mayor de `PT_LOAD`. Los segmentos, el PC de entrada y `AT_PHDR`/`AT_ENTRY` comparten ese bias; los valores originales de los encabezados no cambian y `AT_BASE` permanece en cero porque no hay intérprete. El mapeo usa bytes del archivo original, no los fixups de análisis; el propio invitado debe realizar sus relocations e inicialización. El loader decodifica `PT_DYNAMIC` desde registros acotados del archivo, sin depender de secciones. Si existe, la tabla debe ser legible, terminar correctamente y tener como máximo 4096 entradas. Se rechazan `PT_INTERP` y tags externos de dependencias/filter/audit; no se proporciona linker dinámico, resolución de símbolos ni ejecución de constructores.

La pila inicial contiene argc/argv/envp/auxv alineados, PHDR/PHENT/PHNUM, entry, tamaño de página e identidades. PID/TID/UID/GID modelados valen 1000. `AT_RANDOM` usa los primeros 16 bytes del SHA-256 de entrada para reproducibilidad; es política determinista del modelo, no entropía criptográfica. HWCAP/HWCAP2 son cero y no existe vDSO.

Se implementan `write`, `exit`, `exit_group`, `getpid` y `gettid`, `mmap`, `mprotect`, `munmap`, `brk`, con números separados para [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) y [ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). El retorno de SYSCALL x64 aplica sus clobbers RCX/R11 además de RAX y el PC siguiente. ARM64 usa x8 para el número y x0 para el resultado. Las demás llamadas paran como `unsupported_service`; nunca ejecutan syscalls del host.

Las plantillas TLS estáticas `PT_TLS` se validan como hechos del loader: una plantilla, extensiones acotadas de archivo/memoria, alineación congruente y bytes iniciales legibles. El inicio invitado asigna e inicializa bloques TLS e instala el puntero de hilo; el modelo Linux no inventa un TCB/DTV específico de libc. Esto permite TLS local-exec generado por compilador en programas freestanding. TLS dinámico e hilos del SO siguen fuera del alcance.

En x64, `arch_prctl` admite `ARCH_SET_FS`, `ARCH_GET_FS`, `ARCH_SET_GS` y `ARCH_GET_GS`. Set acepta una base de rango usuario aunque no esté mapeada; toda desreferencia posterior verifica permisos. Las bases de rango kernel devuelven `EPERM` invitado y los destinos Get inválidos `EFAULT`, sin fault de CPU. Otras operaciones fallan explícitamente. ARM64 instala `TPIDR_EL0` con `MSR`; `MRS`, accesos FS/GS y restauración de contexto preservan el puntero entre quanta y entradas de backend. Esto no implementa un scheduler.

Los descriptores 1 y 2 son sumideros virtuales de bytes. `write` valida páginas de usuario legibles; devuelve el prefijo legible si una página posterior no es accesible y `EFAULT` si no puede leerse ningún byte. Un descriptor incorrecto da `EBADF`; una escritura de cero bytes con descriptor válido no accede al puntero. No se modelan la atomicidad de tuberías Linux ni archivos. El límite de salida detiene antes de publicar una escritura que lo excedería.

Los servicios de memoria anónima comparten el espacio del proceso y el presupuesto físico con la imagen y la pila. `mmap` acepta exactamente `MAP_PRIVATE | MAP_ANONYMOUS`, con `PROT_NONE`, `PROT_READ`, `PROT_READ | PROT_WRITE`, `PROT_READ | PROT_EXEC` o RWX legible. Respeta sugerencias libres alineadas a página; si no, busca huecos desde `0x100000000` y después desde la dirección mínima de usuario, reservando las guardas de pila. Esta colocación determinista no emula ASLR de Linux. Las páginas nuevas son independientes y se inicializan a cero; una retirada parcial puede recuperar páginas no fijadas. Una proyección CPU o vista de respaldo retenida puede mantener viva una asignación retirada hasta terminar su propia vida.

Las longitudes se redondean a páginas. `munmap` tolera huecos y retiradas repetidas; `mprotect` modifica el prefijo mapeado antes de devolver `ENOMEM` ante un hueco. `PROT_NONE` conserva la asignación y sus bytes, pero impide el acceso invitado. El `brk` bruto devuelve el límite solicitado si tiene éxito y el anterior si falla, no la convención cero/menos uno de libc. El límite inicial es el final de imagen alineado a página. El crecimiento respeta otros mapeos y el presupuesto; la reducción conserva los bytes de la página parcial restante. Las reglas y prioridades de error siguen los servicios Linux de [mapeo](https://github.com/torvalds/linux/blob/v6.8/mm/mmap.c) y [protección](https://github.com/torvalds/linux/blob/v6.8/mm/mprotect.c).

Los mapeos de archivos, compartidos o fijos, crecimiento descendente, páginas enormes, bloqueo, claves de protección, permisos solo de ejecución/escritura y otros indicadores siguen sin soporte explícito: se detienen antes de publicar efectos o inventar un retorno. Los errores normales de rango, longitud y alineación del subconjunto admitido devuelven errores invitados y permiten continuar. Ningún servicio reenvía punteros ni peticiones de mapeo al OS anfitrión.

<a id="windows-pe64-profile"></a>

<!-- i18n-section: windows-pe64 -->

## Perfil Windows PE64

`windows-pe64-v1` añade procesos de consola Windows x64/ARM64 limitados: carga PE, PEB/TEB, TLS estático y dinámico, callbacks de inicio/salida y modelos Win32 por nombre. Usa la capa CPU sin depender de la emulación de controladores; carga DLL/CRT, GUI, SEH de usuario, hilos y compatibilidad general con Windows siguen pendientes.

La memoria virtual de Windows incorpora `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` y `FlushInstructionCache` para el proceso actual. La capa OS administra las reservas; `AddressSpace` mantiene la autoridad sobre páginas confirmadas, permisos y almacenamiento. Las pruebas cubren cambios de código, fallos de acceso y reutilización del presupuesto de memoria.

Las asignaciones privadas admiten `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT`, `MEM_RELEASE` y `MEM_TOP_DOWN`, con reservas alineadas a 64 KiB y páginas de 4 KiB. Reservar no consume RAM del invitado. Volver a confirmar conserva los bytes y actualiza permisos; desconfirmar devuelve cada página. La validación completa del rango y la preparación de asignaciones evitan cambios parciales ante errores ordinarios. La consulta devuelve la estructura x64/ARM64 de 48 bytes y agrupa páginas posteriores de una misma asignación. Imagen, entorno, área de heap, entradas API y márgenes de pila intervienen en la ubicación; la identidad de la pila coincide con el TEB. Si un `VirtualProtect` correcto vuelve de solo lectura la ubicación de salida de los permisos anteriores, los nuevos permisos siguen aplicados, los bytes de salida no cambian y la llamada devuelve éxito. Un cambio de protección sobre páginas sin confirmar devuelve `ERROR_INVALID_ADDRESS`, escribe `PAGE_NOACCESS` en la salida de los permisos anteriores y conserva los permisos de las páginas.

Las protecciones admitidas son `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE_READ` y `PAGE_EXECUTE_READWRITE`. Las páginas de guarda, solo ejecución, copia al escribir, modificadores de caché, páginas grandes, reset/write-watch/marcadores y cambios en mapeos internos del modelo siguen sin soporte explícito. Solo se pueden desconfirmar o liberar asignaciones virtuales privadas. No se añade gestión de excepciones de usuario ni evidencia de hardware ARM64 nativo.

[VirtualAlloc](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc), [VirtualFree](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualfree), [VirtualProtect](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect), [VirtualQuery](https://learn.microsoft.com/windows/win32/api/memoryapi/nf-memoryapi-virtualquery), [MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-memory_basic_information).

```bash
neverd emulate guest.exe --profile=windows-pe64-v1 \
  --options='{"backend":"auto","arguments":["guest.exe","argument"],"environment":["MODE=test"]}'
```

Este modelo versionado de un hilo carga EXE de consola PE32+ AMD64/ARM64 en su base preferida. Verifica bytes originales y datos del cargador, conserva permisos de usuario y huecos de protección de pila. Rechaza importaciones desconocidas, ordinales, enlazadas o diferidas, configuración de carga/CFG, imágenes administradas, GUI y entradas DLL. Valida reubicaciones sin cambiar la base. Los modelos API no son DLL instaladas: las listas del cargador solo incluyen el EXE y `GetModuleHandleW` acepta únicamente NULL.

GS en x64 y x18 en ARM64 apuntan a TEB: límites de pila, puntero propio, PID/TID, PEB, parámetros, LastError y TLS. Convierte UTF-8 estricto a UTF-16 y entrecomilla argv según Microsoft CRT. Los nombres de entorno son ASCII, se rechazan duplicados sin distinguir mayúsculas, los valores pueden ser Unicode y el bloque ordenado acaba con dos NUL. No hereda entorno ni archivos del host. TLS estático copia plantilla, pone BSS a cero y escribe un índice de 32 bits; TLS dinámico usa otras ranuras TEB. Attach/detach lee la tabla viva en orden con presupuesto y plazo compartidos. Retorno de entrada y salida normal ejecutan detach; la salida recursiva durante detach se detiene explícitamente.

`WindowsProcessServices.def` define `ExitProcess`, `RtlExitUserProcess`, handles de salida y `WriteFile` síncrono, LastError, ID y pseudohandles de proceso/hilo, `GetCommandLineW`, asignación/liberación/tamaño del heap, TLS dinámico y NULL `GetModuleHandleW`. Solo resuelve nombres exactos de `kernel32.dll`, `kernelbase.dll` y `ntdll.dll`. Syscalls directos y puertas falsas no seleccionan modelos. El heap pertenece al proceso y se recupera; la salida conserva bytes binarios. Los errores Win32 se separan de E/S asíncrona y excepciones de usuario no implementadas. Los alias respetan el cero inicial del contador y la dirección de retorno real.

`windows.native_calls` conserva módulo/función, argumentos escalares declarados y resultados anulables, sin inventar números NT. `NeverDWindowsProcessTests` comprueba PE reales, TLS del compilador, cambios de callbacks, heap, alias, metadatos inválidos, privilegios y presupuestos; `NeverDProcessPublicTests` verifica CLI/C ABI. CI de Windows ejecuta el mismo EXE directamente como oráculo independiente y exige pruebas WHP. La evidencia ARM64 nativa aún requiere una máquina adecuada.

Si el búfer de entrada no está vacío y no es legible, `WriteFile` devuelve `ERROR_INVALID_USER_BUFFER` (1784), pone a cero el número de bytes escritos y no produce salida.

[PE/COFF](https://learn.microsoft.com/windows/win32/debug/pe-format), [ARM64 ABI](https://learn.microsoft.com/cpp/build/arm64-windows-abi-conventions), [WriteFile](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile), [TLS](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue), [Wine 10.0 loader](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/ntdll/loader.c).

<!-- i18n-section: verification -->

## Verificación

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# En una compilación con biblioteca compartida/CLI:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Las pruebas compilan entradas ELF autónomas en ensamblador y C para ambas ISA: data/BSS, inicio real, errores de llamadas, salida binaria, permisos, escrituras parciales, servicios no admitidos y presupuestos entre cuantos. TLS inicializa bloques alineados independientes, BSS y punteros de hilo y comprueba su conservación; x64 verifica errores de `arch_prctl` sin perder la base anterior. Los backends no disponibles se omiten explícitamente. La suite pública compara informes y códigos de salida mediante la ABI C compartida y la CLI. PIE verifica auxv y slots RELA originalmente nulos antes de sus propias reubicaciones de datos y funciones. Las pruebas de mapeo conservan fixups cuando se elige la fuente de análisis; las tablas dinámicas cubren ausencia de secciones y entradas malformadas o dependientes. Ambas ISA ejercitan asignación, protección, huecos, remapeo, crecimiento/reducción del heap y errores tratados. Escrituras invitadas reales fallan tras cambios de protección ordinarios y parciales. x64 reescribe código en la misma dirección entre RW y RX y llama ambas versiones; el mismo ELF se ejecuta nativamente en Linux como oráculo independiente. Las pruebas de memoria cubren agotamiento, recuperación e instantáneas autoritativas sin retener RAM. La compilación cruzada y Unicorn ARM64 no son evidencia de ARM64 KVM/WHP nativo.
