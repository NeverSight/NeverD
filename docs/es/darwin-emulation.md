**Idiomas**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](../ar/darwin-emulation.md)

<!-- i18n-source: 1939e117643388dff149085b992e1ad646feefca63b630abec9b09122e15c2f1 -->

[← Índice de documentación](README.md)

# Entornos de procesos invitados macOS e iOS

`lib/emulation/os/darwin/` modela procesos Mach-O autónomos y acotados, separados del transporte CPU anfitrión. Active `NEVERD_ENABLE_CPU_EMULATION`; no necesita emulación de controladores Windows. `macos/` e `ios/` definen perfiles explícitos.

| Perfil | Plataforma Mach-O | ISA invitadas | Página OS |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, ARM64 base | 4 KiB x64; 16 KiB ARM64 |
| `ios-macho64-v1` | dispositivo iOS | ARM64 base | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, ARM64 base | 4 KiB x64; 16 KiB ARM64 |

Un binario de dispositivo no es una imagen de simulador; el anfitrión no decide la plataforma invitada. Con ISA coincidente, macOS puede usar [HVF](macos-hvf.md); entre ISA distintas, `auto` usa Unicorn. La granularidad CPU sigue siendo 4 KiB. Las [API C, Python y CLI](process-emulation.md) comparten opciones, límites e informes.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## Imagen y arranque

`MachOExecutionImage` conserva bytes originales sin modificaciones de relocalización del análisis. Solo admite imágenes thin little-endian `MH_EXECUTE` con plataforma y entrada inequívocas. Las imágenes universales requieren extraer explícitamente la arquitectura deseada.

El archivo entero, incluidos metadatos y bytes finales, debe caber en `memory_limit` antes de analizarlo o copiarlo. Se lee una instantánea privada y acotada de un archivo regular; se rechazan rutas con NUL, lecturas cortas y cambios de tamaño. No se mantiene un mapeo vivo del archivo. Archivo y memoria invitada tienen límites separados del mismo valor; las E/S anfitrionas no ofrecen plazo de tiempo real estricto.

Los segmentos conservan permisos actuales/máximos y relleno a cero. `__PAGEZERO` reserva direcciones sin asignar toda su extensión. Se comprueban rangos de archivo/VM, alineación OS, solapamientos redondeados, propiedad de la cabecera, entrada ejecutable y presupuesto. El segmento de cabecera debe ser legible y ejecutable; las páginas de guarda y la puerta privada de retorno permanecen reservadas. La última página de archivo conserva bytes hasta el límite de página o EOF; las páginas VM completas posteriores se ponen a cero según el [cargador XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).

`LC_MAIN` recibe `argc`, `argv`, `envp` y el vector apple como cuatro argumentos enteros; el retorno aporta los ocho bits bajos del estado de salida. Se admite `/usr/lib/dyld` solo para esta entrada sin imports, sin ejecutar el dyld anfitrión. Se rechaza un `stacksize` distinto de cero: la opción `stack_size` del llamador fija el presupuesto.

`LC_UNIXTHREAD` exige exactamente un registro completo de estado general nativo de 64 bits, con solo PC establecido. La pila contiene argc, argv/envp terminados y un vector apple terminado con `executable_path=<input filename>`. Se rechazan SP/flags propios, otros registros, flavors adicionales y entradas contradictorias. No se heredan el entorno anfitrión ni un vector auxiliar Linux. Referencia: [arquitectura dyld](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

Dylibs externas, imports, rebases/chained fixups, constructores/destructores, secciones TLS, arm64e/PAC, otros subtipos CPU no admitidos, cifrado y comandos no modelados fallan antes de ejecutar. PIE sin fixups usa direcciones preferidas, sin ASLR. Los blobs de firma son metadatos, no una implementación de AMFI o políticas de entitlements.

## Servicios Darwin

Las llamadas BSD en ARM64 usan X16, X0–X5 y `svc #0x80`; x64 usa la clase BSD `0x02000000`, RAX y RDI/RSI/RDX/R10/R8/R9. El éxito limpia carry; el error lo activa y devuelve errno positivo. ARM64 limpia X1; x64 limpia RDX al tener éxito y lo conserva ante error. Los cambios de registros de SYSCALL son explícitos. El informe usa `result` y `error=true` para errores BSD; las solicitudes sin retorno o no admitidas carecen de ambos campos. Las reglas proceden de XNU [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) y [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c), sin incorporar código Apple.

Servicios: `exit`, `write`, `getpid`, `getppid`, `getuid`, `geteuid`, `getgid`, `getegid`, `getgroups`, `mmap`, `mprotect`, `munmap`. PID vale1000 y PPID1; UID/GID son1000 por defecto o los ID reales/efectivos distintos declarados abajo. Los descriptores 1 y 2 capturan bytes, incluidos NUL y no UTF8; los cerrados o de solo lectura devuelven EBADF. Una copia parcial conserva los bytes leídos, pero el fallo posterior sigue siendo EFAULT. Una longitud superior a `INT_MAX` devuelve EINVAL antes de comprobar descriptor, puntero o presupuesto: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

Se permiten mapeos privados anónimos de datos con `flags=0x1002`, descriptor -1 y offset cero. Longitudes y sugerencias no fijas se redondean hacia arriba a la página OS. Si una sugerencia está ocupada, se busca hacia arriba antes de volver a la ubicación predeterminada. El mmap histórico sin envolver de longitud cero devuelve cero sin asignar; `MAP_UNIX03` se admite y rechaza longitud cero con EINVAL. Unmap/protect requieren dirección alineada. Se admiten NONE/READ/WRITE, con WRITE implicando READ. Cada página OS posee su memoria física: un unmap parcial libera presupuesto y las páginas nuevas quedan a cero. Un protect que atraviese un hueco o supere permisos máximos deja intacto todo el rango. Fuente: [servicios VM de XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Quedan excluidos mapeos compartidos/fijos/JIT o anónimos ejecutables, otros traps Mach, syscalls indirectas, hilos, señales, archivos del host/red, dyld, runtimes Objective-C/Swift y Foundation/UIKit. Su uso detiene explícitamente la ejecución. No es un OS Apple completo ni la aplicación iOS Simulator.

## Verificación

Las muestras C propias se generan con Clang y `ld64.lld`, sin SDK Apple ni binarios propietarios. Cubren cinco combinaciones plataforma/ISA, registros Mach-O malformados, páginas de 4/16 KiB y liberación parcial con presupuesto lleno. `NeverDProcessPublicTests` compara C API/CLI; `NEVERD_TEST_LIBNEVERD` y `NEVERD_TEST_DARWIN_FIXTURES` habilitan las mismas cinco combinaciones en Python.

## Archivos y descriptores explícitos

`darwin_files` ofrece a los tres perfiles un catálogo cerrado de archivos inicialmente de solo lectura. `files` es obligatorio: cada entrada contiene un `path` absoluto canónico del invitado y `bytes_hex` hexadecimal. `stdin_hex` opcional aporta una entrada finita; omitirla significa desconocida y detiene lecturas no vacías, mientras una cadena vacía significa EOF. Sin catálogo open se detiene; un catálogo explícitamente vacío devuelve ENOENT. No se consultan archivos ni entrada del host.

Se añaden `open`, `read`, `pread`, `lseek`, `close`, `dup`, `dup2`, `fcntl` y las entradas nocancel de read/write/open/close/fcntl/pread. Se admiten O_RDONLY/O_CLOEXEC y F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL. Cada open tiene posición independiente; los duplicados comparten posición y conservan flags close-on-exec individuales. pread no cambia la posición. Cerrar o sustituir 0/1/2 afecta a la I/O posterior; duplicar salida conserva destino y presupuesto.

Límites: 256 archivos, 16 MiB totales de rutas/NUL/archivos/entrada, rutas menores de 1024 bytes y componentes de hasta 255. `descriptor_limit` es un techo exclusivo de 3–4096, por defecto 256; JSON conserva 64 KiB. La configuración inválida falla antes de cargar. read superior a INT_MAX devuelve EINVAL antes de consultar FD; EOF no toca el destino y un destino inválido da EFAULT. Un búfer parcialmente escribible detiene la operación antes de copiar o mover la posición. Los errores SET/CUR/END conservan la posición. Stat antiguo y otros fcntl siguen excluidos. Un archivo como antecesor devuelve ENOTDIR. El mismo objeto se contrasta con macOS nativo y C/CLI/Python cubren cinco combinaciones; no demuestra ejecución en un dispositivo iOS.

Verificación Release de 2026-10-05: 381 registros, 177 aprobados, 204 omitidos, ningún fallo y 51/51 requisitos ARM64 HVF ejecutados. Pasaron también siete programas macOS nativos, 35 pruebas públicas C/CLI/informes, cinco combinaciones Python y 66 pruebas del verificador. Los recuentos se solapan. Los nuevos servicios no tienen evidencia nativa Intel HVF/KVM/WHP; Intel HVF sigue sin validar y sus Actions están suspendidas. Faltan el SDK iOS y la comparación con dispositivos.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## Modificar archivos existentes

El booleano estricto `"writable":true` o `DarwinFileOptions::WritableFiles` autoriza cambios locales al proceso. Ausente/false conserva solo lectura; un permiso desconocido detiene el servicio. No cambia el host ni los datos iniciales. write(4/397), pwrite(154/415), truncate(200), ftruncate(201) y O_TRUNC comparten contenido; open mantiene posiciones independientes, dup comparte posición y estado, y el contenido sobrevive al último close. El crecimiento rellena ceros y truncar conserva posiciones, incluso O_RDONLY|O_TRUNC.

F_SETFL cambia solo O_APPEND|O_NONBLOCK tras conversión nativa y conserva acceso, close-on-exec y FWASWRITTEN. F_GETFL muestra 0x10000 tras transferir bytes no vacíos, también con pwrite y salida capturada. pwrite ignora append y conserva posición. INT_MAX se comprueba antes de FD; -1 en pwrite devuelve EINVAL antes aún. INT64_MAX devuelve EFBIG antes del caso vacío; se recorta la longitud antes de elegir EOF.

ftruncate exitoso, incluso sin cambiar tamaño, marca FWASWRITTEN en la descripción invocada y sus dup. O_TRUNC marca la nueva descripción, incluso O_RDONLY; truncate por ruta no marca las existentes.

La entrada parcialmente legible se detiene antes de efectos. EFAULT completo conserva bytes, pero append no vacío mueve la posición a EOF. El fallo de transporte no confirma contenido ni posición. Sin `mutation_policy`, escritura no vacía, truncado y EFAULT completo no vacío invalidan la observación stat completa; consultas posteriores paran antes de copiar. La escritura vacía la conserva. Los 16 MiB suman rutas/NUL, entrada, registros, CWD, contenido actual y referencias de rutas escribibles. Reducir sustituye el almacenamiento y libera capacidad; entrada inicial y un búfer acotado adicional quedan fuera del límite lógico. Se rechazan alias inode conocidos y flags immutable/append-only.

DarwinMemory mantiene reservas hasta el último unmap, incluso PROT_NONE y FD cerrados; las mutaciones paran mientras existan. Fallos y mmap antiguo de longitud cero no retienen reservas. Nuevos mapas ven bytes actuales. O_WRONLY con READ/WRITE da EACCES; PROT_NONE puede ganar lectura/escritura mediante mprotect.

Programas originales normal/nocancel comparan el kernel nativo; pruebas 4K/16K y C/CLI/Python cubren cinco combinaciones. Aplicación de permisos, borrar directorios, renombrar entre dominios de directorios iniciales distintos, enlaces físicos, metadatos del sistema de archivos nativo, coherencia de mapas y SIGBUS EOF siguen pendientes. El entorno completo, dispositivos iOS e Intel HVF no están validados; Actions Intel permanece suspendido.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

[XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU vnode](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Metadatos mutables explícitos

Cada archivo puede añadir mutation_policy junto a `writable: true` y metadata completos; C++ usa `DarwinFileOptions::MutationPolicies`. Es un contrato virtual de asignación dispersa explícito, sin inferir APFS ni consultar el reloj host. Si se omite, los metadatos posteriores siguen siendo desconocidos.

allocation_unit, mutation_time y seconds/nanoseconds son obligatorios, con las reglas enteras sin pérdida existentes. La unidad es una potencia de dos entre512 bytes y16 MiB, independiente de block_size y páginas VM. Se requieren permisos normales sin set-id/sticky, flags=0, link_count=1 y asignación inicial densa: blocks=ceil(size/allocation_unit)*(allocation_unit/512). Ceros no implican huecos. La referencia de ruta cuenta en el límite lógico de16 MiB; la contabilidad de bloques no inventa ENOSPC.

Escribir asigna toda unidad tocada, incluso ceros en huecos. truncate al crecer añade ceros sin asignar; al reducir descarta unidades después de EOF redondeado hacia arriba y retiene la última parcial. Volver a crecer no restaura asignaciones descartadas. Escrituras no vacías exitosas y todo truncate exitoso, incluso del mismo tamaño u O_TRUNC vacío, actualizan size/blocks y fijan mtime/ctime al tiempo suministrado. Otros campos y entradas se conservan; read no avanza atime. Stat por ruta, open independientes, dup y reapertura comparten nodo.

Escritura vacía, rechazo por presupuesto/mapas, entrada parcial rechazada y fallo de transporte conservan el estado. EFAULT completo no vacío lo vuelve desconocido; un éxito posterior no lo reconstruye. Fallar la copia stat no cambia el nodo. virtual-file-metadata verifica144 bytes en cinco perfiles y C/CLI/Python: es prueba de política, no equivalencia APFS. Los programas nativos verifican aparte flags, posiciones y errores. Espacio de nombres, coherencia nativa, Mach y carga dinámica siguen pendientes.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```


## Posicionamiento en archivos dispersos

Con mutation_policy y asignación conocida, lseek admite SEEK_HOLE=3 y SEEK_DATA=4 en archivos regulares usando el mismo registro que stat. La entrada inicial es densa, incluso sus ceros. Devuelve la posición de entrada dentro de una unidad del tipo buscado, o el inicio de la siguiente coincidente. El hueco final comienza en EOF. Un negativo da EINVAL; en/después de EOF, archivo vacío o sin datos posteriores, ENXIO=6. El fallo conserva el cursor; el éxito solo cambia la descripción y sus dup. Otros open conservan sus cursores y reabrir ve la asignación actual. Metadatos, flags y bytes no cambian; se ignoran bits altos de whence.

Sin política, para directorios o tras EFAULT completo con asignación desconocida, sigue sin soporte. Ceros y cambios rechazados no permiten inferir asignaciones. sparse-file-seek compara errores, bytes escritos, EOF y vida de descripciones nativas/invitadas sin asumir límites anteriores del FS. virtual-file-metadata comprueba aparte la geometría exacta de la política; C/CLI/Python cubren cinco perfiles.

[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Eliminar nombres de archivos regulares

`mutable:true` por directorio (C++ `MutableDirectories`) autoriza cambios de nombres inmediatos, independientemente de `writable`. Sin autorización se detiene explícitamente. Se rechazan flags conocidos no nulos, permisos especiales del padre, link_count≠1 del hijo y alias conocidos del padre/hijo, combinando stat e inodes de snapshots. Dispositivos explícitamente distintos siguen separados; los caminos cuentan en el presupuesto.

`unlink(10)` / `unlinkat(472)` eliminan nombres regulares existentes. el borrado de archivos acepta los32 bits bajos 0 o `0x800`; bits desconocidos dan EINVAL antes de ruta/FD y AT_REMOVEDIR usa el contrato limitado de abajo; DATALESS y SYSTEM_DISCARDED siguen excluidos. Resolución común: ENOENT, ENOTDIR tras archivo con `/`, EPERM en directorio ordinario, EISDIR para raíz con solo barras, EBUSY para raíz terminada en `.`/`..`. Se verificaron nativamente finales `.`/`..`.

FD/dup/aperturas independientes previos conservan datos, cursores y flags; F_GETPATH devuelve la ruta anterior capturada. Nuevas aperturas fallan, padres implícitos y CWD permanecen. La autorización de escritura pertenece al objeto; close/dup2/la siguiente mutación recuperan sus bytes actuales sólo tras el último descriptor y mapa. Los costes iniciales de rutas permanecen; aplicación de permisos, renombrado entre dominios de directorios iniciales distintos y enlaces físicos siguen pendientes; los directorios iniciales usan la autorización explícita descrita más abajo.

stat/readdir/SEEK_END del padre quedan desconocidos para todo FD/ruta y paran antes de copiar o mover cursores. read/pread mantienen EISDIR; SET/CUR/F_GETPATH/fchdir/resolución relativa continúan. La política conocida establece nlink=0 y ctime fijo; posteriores escrituras no restauran nlink=1. Sin política/tras EFAULT, metadatos desconocidos. Los fallos preservan estado. `unlinked-file` compara reglas nativas de nombre/FD; tiempos e invalidación son reglas explícitas del modelo.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Crear archivos normales

O_CREAT=0x200 crea un archivo vacío bajo un padre directo explícitamente mutable mediante open/openat normal o nocancel. El objeto nuevo permite escritura; los existentes conservan su autorización WritableFiles. Un FD de lectura puede crear pero no escribir. Sin política de creación explícita, stat64 y búsqueda dispersa siguen desconocidos. Nunca se heredan metadata/mutation_policy del objeto anterior con igual nombre.

O_EXCL=0x800 con O_CREAT devuelve EEXIST para archivos/directorios existentes antes de truncar; solo no tiene efecto. O_CREAT de lectura abre directorios existentes. Tras comprobar el primer byte y el directorio en openat como se describe abajo, el orden es: modo inválido, capacidad FD, EINVAL por O_CREAT|O_DIRECTORY, ruta. Solo se crea el último componente original ausente; ancestros ausentes y sufijos `/`, `//`, `/.`, `/..` dan ENOENT. O_CREAT|O_TRUNC nuevo no marca FWASWRITTEN; truncar uno existente sí.

Solo insertar invalida las observaciones del padre. Objetos nuevos/antiguos homónimos mantienen datos, FD, metadatos y mapas independientes. Las 256 entradas incluyen elementos iniciales no archivo y objetos vivos; rutas canónicas/NUL dinámicas y bytes actuales cuentan en 16 MiB. Tras unlink, el último FD/mapa libera costes dinámicos; los iniciales permanecen. Agotar presupuesto o llegar a 1024 bytes de ruta canónica detiene explícitamente sin inventar ENOSPC o errno de ruta nativo ni publicar nombre/FD. created-file compara macOS nativo y cinco perfiles; pruebas 4K/16K verifican límites. Quedan aplicación de permisos, renombrado entre dominios de directorios iniciales distintos, enlaces y mutación de directorios.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Metadatos de creación explícitos y umask del proceso

El `darwin_files.umask` opcional (C++ `InitialUmask`) declara la máscara inicial entre 0 y 07777 octal, independientemente del permiso de creación. `umask(60)` devuelve la anterior y guarda los bits bajos 07777, sin acceder a memoria invitada ni necesitar un FD libre. Omitirla significa desconocida, sin inferir valores del host o predeterminados. Se inicializa una sola vez y los cambios afectan únicamente a futuras creaciones, sin modificar la entrada. El ejemplo usa 18 decimal, equivalente a 0022 octal.

Sin `namespace_policy`: El `darwin_files.creation_policy` opcional (C++ `CreationPolicy`) aporta metadatos completos a objetos nuevos. Su objeto estricto contiene exactamente `first_inode`, `block_size`, `generation`, `creation_time`, `mutation_policy`; tiempos y política de mutación usan los formatos existentes. Exige umask explícita, al menos un padre mutable y metadata completa de todos los padres autorizados. block_size está en 1..INT32_MAX, generation es uint32; la unidad de asignación es una potencia de dos entre 512 y 16 MiB, independiente del bloque/página VM, y los nanosegundos están en [0,1000000000). first_inode es uint64 positivo mayor que todos los inode de stat/instantáneas, incluso de otros dispositivos. Las cadenas decimales conservan enteros fuera del rango exacto de JSON.

Solo insertar un objeto nuevo con éxito consume la secuencia global inode. UINT64_MAX la agota permanentemente; close/unlink/reutilización de nombre/umask/consultas no la reinician. Los rechazos por exclusividad, FD, ruta, entradas o bytes no publican nombre/FD ni avanzan el contador; O_CREAT existente no consume ninguno. stat64 nuevo hereda device/GID del padre directo, UID efectivo invitado seleccionado (1000 por defecto), mode `S_IFREG | (mode & 0777 & ~umask)`, nlink=1 y size/blocks/flags=0. Bloque, generation y cuatro tiempos iniciales fijos proceden de la política. Tras invalidar stat/enumeración completos del padre se conservan device/GID, sin restaurar el registro completo.

Cada nodo posee metadatos/asignación propios, sin heredar el antiguo homónimo. write/truncate/unlink comparten la política y preservan inode/mode/birthtime y nlink=0 tras unlink; EFAULT completo conserva el estado permanentemente desconocido. No hay efecto retroactivo en nodos existentes. `created-file-metadata` compara permisos, máscara devuelta, UID efectivo, dispositivo/grupo padre y vida nativa en cinco perfiles; `virtual-created-metadata` compara aparte los 144 bytes. Los cuatro tiempos nativos pueden diferir. Tiempo fijo/asignación dispersa son reglas virtuales; aplicación de permisos, cambio de credenciales, ACL y comportamiento APFS nativo siguen pendientes.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Intercambio atómico de archivos y directorios creados

RENAME_SWAP=0x2 intercambia mediante renameatx_np dos archivos regulares existentes, dos directorios vivos creados por el proceso, o un archivo y uno de esos directorios; admite RENAME_NOFOLLOW_ANY. El directorio inicial explícito declara mutable:true y swap_rename:true; C++ usa DarwinFileOptions::SwapRenameDirectories. Los descendientes heredan la capacidad del objeto inicial; reutilizar nombres eliminados no transfiere declaraciones. false u omisión significa desconocido. Devices iguales y permisos solos no prueban soporte; los dominios iniciales distintos siguen excluidos.

`openat_nocancel`, `fstatat64` y `F_GETPATH=50` usan el mismo componente de archivos. Tras el intercambio, rutas y observaciones siguen perteneciendo a cada objeto; la disponibilidad del stat completo conserva el contrato de metadatos.

Un destino ausente, incluso con barra final, da ENOENT antes de punto/doble punto del origen, dominio, permisos o capacidad. Los operandos de directorio iniciales o eliminados siguen excluidos. En el dominio admitido, ambos órdenes ancestro/descendiente y directorio/archivo hijo dan EINVAL; un destino archivo con barra final da ENOTDIR. El mismo objeto de componente ordinario no cambia tras autorización, incluso sin capacidad declarada. El punto origen del mismo objeto aún requiere una propiedad desconocida de sensibilidad a mayúsculas. RENAME_EXCL=0x4 + RENAME_SWAP=0x2 y flags desconocidos dan EINVAL antes de las rutas; SECLUDE no se admite.

Los dos subárboles no vacíos siguen objetos padre, incluidos directorios eliminados y archivos huérfanos retenidos por FD/mapping. El intercambio mixto mueve sólo la raíz archivo exacta; un antiguo huérfano homónimo conserva su padre. FD, dup, CWD y doble punto siguen objetos y nuevos padres. Bytes, identidad, metadatos, derechos, cursores, flags y leases de archivos descendientes se conservan. Raíces movidas y padres inmediatos aplican las reglas existentes del namespace; una política configurada actualiza el ctime propio del archivo raíz, sin ella los metadatos completos siguen desconocidos.

Cada referencia reserva ruta+NUL en los 16 MiB iniciales fijos. Todas las rutas enlazadas/retenidas de ambas direcciones se comprueban bajo 1024 bytes y el presupuesto compartido antes de retirar juntos los nombres antiguos y publicar. Ninguna raíz se elimina y no hay crédito de reemplazo, ni siquiera de bytes de un destino sin abrir. El primer movimiento de un archivo inicial adquiere costo dinámico; volver no lo borra y repetir no lo acumula. No consume nuevo FD, entrada ni inode de creación. El rechazo conserva ambos namespaces, padres, cursores, observaciones y mappings. El original SDK-free swapped-directory compara directorios y ambos órdenes mixtos en macOS nativo y cinco perfiles C++/C/CLI/Python.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Instantáneas explícitas de directorio

`getdirentries64` (344) recorre el `contents` inmutable opcional de una entrada `directories` existente; C++ usa `DarwinFileOptions::DirectoryContents`. `entries` incluye en orden explícito todos los hijos directos, `.` y `..`. Sin instantánea, incluso un directorio vacío sigue desconocido. No crea rutas ni stat ni consulta al anfitrión.

Cada entrada exige `name`, `inode` no nulo, `type` (0 desconocido, 4 directorio, 8 archivo), `next_offset` y `seek_offset`. El tipo coincide con la ruta; el inode de la misma ruta resuelta coincide entre instantáneas y metadatos. `next_offset` es positivo, único en ese directorio y <=INT64_MAX, sin orden creciente obligatorio; cero rebobina. `seek_offset` es una observación d_seekoff separada de 64 bits sin signo y admite ceros repetidos. Los enteros usan las cadenas decimales sin pérdida de stat.

`contents.minimum_buffer_size` exige un mínimo de carga de 1–128 MiB, incluido EOF. El `minimum_buffer_size` opcional por entrada (predeterminado 0) limita llamadas que empiezan allí. El ejemplo observa APFS: 64 bytes para los dos puntos iniciales, 1 en EOF; otras posiciones deben alojar un registro entero. LP64 usa alineación de ocho bytes y tamaño `roundUp(25 + nameBytes, 8)`. Máximo 4096 entradas en total; sus bytes cuentan en 16 MiB. Las rutas ancestrales declaradas solo por metadatos/instantánea cuentan una vez en las 256 rutas. JSON conserva 64 KiB.

Open independientes tienen cursores propios, dup los comparte. Solo cero o valores suministrados permiten continuar; una posición desconocida detiene explícitamente. Cada llamada devuelve el máximo prefijo de registros completos. Longitud >=1024 reserva los cuatro últimos bytes solicitados para EOF (1 al final, 0 en otro caso); solo la carga se limita a 128 MiB. La dirección conserva la aritmética original sin signo, incluido el desbordamiento. Orden: datos, avance del cursor, posición anterior, indicadores. Un EFAULT posterior conserva efectos previos; EOF omite la copia vacía. Una copia individual parcialmente escribible se detiene antes de esa copia, conservando los efectos anteriores.

`directory-entries` compara campos, dup/rebobinado, lecturas pequeñas, EOF y orden de copias con macOS. Otro test compara todos los bytes nativos capturados, con nombres largos, y el diseño SDK. Los cookies fijos no reproducen generaciones dinámicas APFS. El antiguo `getdirentries` (196), enumeración tras mutaciones, otros transportes nativos e iOS físico quedan fuera de esta validación.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

Validación de enumeración (2026-10-05, Release): 498 casos Darwin, 246 aprobados, 252 omitidos por backend no disponible, cero fallos; 63/63 casos ARM64 HVF obligatorios ejecutados. Pasaron 11 programas macOS nativos, 40 controles C/CLI/informes sin omisiones, cinco combinaciones Python con ocho escenarios de archivos cada una y 66 tests de herramientas. Los conteos se superponen. Evidencia: `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel HVF Actions sigue suspendido; otros transportes nativos e iOS físico no están validados.

## Mapeos privados de archivos

`mmap` acepta archivos regulares del catálogo con `MAP_PRIVATE`: `flags=0x2` o `0x40002` con `MAP_UNIX03` y offset alineado a la página OS. Conserva todos los bytes del archivo dentro de la página aunque la longitud pedida sea menor; el resto de la página final EOF es cero. Las escrituras privadas solo cambian ese mapeo, sin alterar archivo, otros mapeos, metadatos fijos ni cursor compartido. El mapeo sobrevive a close y a reutilizar el FD. Los mapeos de solo lectura y PROT_NONE reciben sus bytes iniciales; `mprotect` puede permitir escritura.

El desbordamiento del final, longitud UNIX03 cero y offset UNIX03 desalineado dan EINVAL antes de buscar FD; un FD inválido da EBADF antes del presupuesto. La longitud histórica cero también verifica el FD. Offsets históricos desalineados, flujos, archivos vacíos y páginas completas más allá de EOF detienen antes de asignar. macOS permite esos mapeos EOF pero el acceso produce SIGBUS; el modelo no inventa páginas cero legibles ni entrega de señales. Mapeos compartidos, fijos, ejecutables y JIT siguen excluidos.

`DarwinFiles` resuelve FD y bytes; `DarwinMemory` gestiona ubicación, permisos, presupuesto y reversión. Los datos vienen solo de `darwin_files`. `file-mapping` verifica copias, close, cursores, errores y reutilización anónima; otra comparación nativa verifica un offset no nulo, la página entera y SIGBUS.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### Verificación de mapeos privados, 2026-10-05

Release Darwin: 438 registros únicos, 210 aprobados, 228 omitidos, cero fallos. Se ejecutaron 57/57 requisitos ARM64 HVF y cinco combinaciones Unicorn. Pasaron nueve programas macOS nativos, la comparación de toda una página con offset no nulo y SIGBUS en un proceso hijo aislado. Los 36 casos API/informe no tuvieron omisiones; Python cubrió cinco combinaciones con `file-mapping`, además de 66 pruebas de herramientas y 38 de procedencia. Los recuentos se solapan. Evidencia: `build-hvf-arm64/darwin-mmap-verified-evidence/`. Sin nueva evidencia Intel HVF/KVM/WHP o iOS físico; Intel HVF Actions sigue suspendido.

## Metadatos explícitos de archivos

Cada archivo puede incluir `metadata`; todos los campos siguientes son obligatorios. Las cadenas decimales conservan el ancho completo; los números JSON deben ser enteros exactos dentro de ±(2^53−1). device es de 32 bits con signo, mode/link_count de 16 sin signo, inode de 64 sin signo y uid/gid/flags/generation de 32 sin signo. size debe coincidir con los bytes; blocks cabe en 64 bits con signo y block_size en 32 con signo no negativos. Los tiempos usan segundos de 64 bits con signo y 0–999999999 nanosegundos.

`stat64` (338), `fstat64` (339) y `lstat64` (340) devuelven el mismo registro LP64 de 144 bytes en ARM64/x64. Comparten la resolución de open y respetan dup/close sin asignar FD ni mover cursores. rdev, relleno y campos reservados son cero. Las entradas aportan metadatos iniciales y la política opcional regula cambios; read no actualiza tiempos y mode no cambia el acceso al catálogo. Metadatos ausentes, flujos, stat antiguo, y seguridad ampliada siguen excluidos. Los errores de ruta/FD preceden al puntero de salida; las salidas parcialmente accesibles se rechazan antes de escribir. Las pruebas nativas comparan todos los bytes de un archivo real y los offsets del SDK; el mismo programa original comprueba las tres llamadas.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### Verificación de metadatos y próximos pasos (2026-10-05)

Con stat64: 409 registros únicos, 193 aprobados, 216 omitidos, ningún fallo; se ejecutaron 54/54 casos ARM64 HVF obligatorios y cinco combinaciones Unicorn. Pasaron la comparación SDK/registro real, ocho programas nativos, 36 casos API/informe sin omisiones, cinco combinaciones Python y 66 pruebas de herramientas; los recuentos se solapan. Cada caso nativo tiene su propio archivo de salida, evitando residuos tras salidas más cortas. Los añadidos carecen de evidencia nativa Intel HVF/KVM/WHP o iOS físico.

Después: mapeos compartidos y fallos de página EOF, escritura acotada (páginas EOF, duración tras close, orden de errores), observaciones explícitas de tiempo/sistema, servicios Mach/hilos necesarios y dependencias Mach-O, rebases/binds, inicialización y TLS. Objective-C/Swift y Foundation/UIKit requieren programas nativos de referencia. iOS físico necesita SDK y dispositivo; Intel HVF sigue sin validar y sus Actions suspendidas.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

La validación independiente exige los 111 casos nativos ARM64 o 74 x64, con `LC_MAIN` y `LC_UNIXTHREAD` en cada plataforma. Casos obligatorios ausentes/omitidos o falta de `ld64.lld` producen fallo.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Use `kvm` en Linux o `whp` en Windows. El [workflow Darwin](../../.github/workflows/darwin-native.yml) prueba ambos transportes x64 sin Unicorn y admite repeticiones individuales. La [referencia de kernel](../../.github/workflows/darwin-kernel-reference.yml) ejecuta los programas directamente en ambas ISA macOS sin NeverD/LLVM. `DarwinNativeCases.def` fija modos, estados y bytes esperados. Solo la referencia anfitriona enlaza libSystem para la entrada real de dyld. ISA incorrecta, Rosetta, timeout o diferencias hacen fallar la prueba; no constituyen evidencia del kernel de un dispositivo iOS.

## Evidencia y alcance pendiente

Resultados del 2026-10-03; no sume filas solapadas:

| Transporte | Fuente | Correctas | Fallidas | Omitidas | Cargas nativas |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

La [ejecución Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) coteja 286 identidades CTest y 32 procesos con XML original. Las 234 omisiones son 65 casos Unicorn desactivados, 39 invitados ARM64 y 130 de otras plataformas anfitrionas. El artefacto `11267489438` tiene SHA-256 verificado `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. También se verificaron [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) de forma independiente. La [referencia de kernel](https://github.com/NeverSight/NeverD/actions/runs/37064795867) pasó 4/4 programas por ISA, con estado 37, salida exacta y stderr vacío.

C API/CLI con Unicorn: 138 correctas, 156 omitidas, cero fallos. Python cubre las cinco combinaciones; el motor empaquetado coincide con 18 informes CLI ARM64 y supera las firmas de 186 imágenes Mach-O. HVF/Unicorn OFF pasa 38 comprobaciones, omite 231 y no enlaza Hypervisor.framework. Son pruebas de integración, no más ejecuciones nativas. La CPU Intel completa sigue sin validar; consulte [HVF](macos-hvf.md) y el [registro detallado](../darwin-emulation.md#hosted-native-verification-2026-10-03).

## Observaciones temporales explícitas

`ProcessOptions::DarwinTime` / `darwin_time` aporta observaciones fijas para la llamada directa `gettimeofday` (116), incluida su tercera salida `mach_absolute_time`, en todos los perfiles Darwin. `time_of_day`, `timezone` y `mach_absolute_time` son opcionales: ausencia significa desconocido; cero explícito es un valor. Un objeto vacío no crea relojes predeterminados. No se lee el reloj anfitrión, se deduce la zona horaria, avanza el tiempo ni convierten los ticks absolutos.

Cada registro proporcionado exige todos sus miembros. `seconds` es de 32 bits sin signo, `microseconds` pertenece a [0, 999999], `minutes_west` / `dst_time` son de 32 bits con signo y los ticks de 64 bits sin signo. JSON usa las reglas enteras sin pérdida; fuera del intervalo seguro se requieren cadenas decimales. Campos desconocidos, rangos inválidos y perfiles ajenos a Darwin se rechazan antes de cargar la imagen.

El `timeval` LP64 ocupa 16 bytes: segundos extendidos con ceros en 0, microsegundos de 32 bits en 8 y cuatro bytes cero en 12. La zona tiene dos campos de 32 bits con signo y los ticks ocho bytes. La hora civil y absoluta se muestrean juntas inicialmente; toda observación solicitada debe existir antes de copiar o comprobar punteros. Luego se copian timeval, timezone y absolute ticks. Una zona ausente o un EFAULT posterior conserva las escrituras anteriores; los alias siguen ese orden. Una salida individual parcialmente escribible detiene la operación antes de esa copia y conserva las previas. Todos los punteros nulos funcionan sin configuración; consultas selectivas solo exigen los valores solicitados.

El programa original `time` comprueba el comportamiento nativo; `time-values` emite los 32 bytes configurados mediante C/CLI/Python en las cinco combinaciones invitadas. Un oráculo SDK compara cada byte con tres salidas capturadas en una sola llamada nativa directa. Quedan pendientes relojes que avanzan, conversión, contadores commpage, temporizadores y objetos de reloj Mach/IPC, además de dyld, hilos, Objective-C/Swift y Foundation/UIKit. Intel HVF Actions sigue suspendido; no se añade validación nativa Intel ni de iOS físico.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

Validación temporal (2026-10-06, Release): 538 casos Darwin, 274 aprobados, 264 omitidos por backend no disponible, sin fallos; ejecutados los 66/66 casos ARM64 HVF obligatorios. Pasan los 12 programas nativos macOS y la comparación SDK de una sola muestra. C/CLI/informes: 43/43, sin omisiones. Python pasa en cinco combinaciones, incluidos los bytes temporales exactos y ocho modos de archivos existentes. Pasan los 66 tests de herramientas, localización, capacidades y formato. Recuentos superpuestos. Evidencias: `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/`, `darwin-time-public.xml`.

## Tiempo Mach y convenciones de retorno

`darwin_time.timebase` aporta `numerator` y `denominator`, enteros de 32 bits sin signo y distintos de cero. La razón se conserva sin reducir ni convertir. `mach_timebase_info_trap`, índice 89, usa ARM64 X16=-89 o x64 RAX=0x01000059. Escribe ocho bytes little-endian (numerador, denominador) y devuelve cero, incluso con una dirección de salida totalmente inválida. Una salida parcialmente escribible detiene antes de copiar; los errores del transporte se propagan. La ausencia de configuración detiene antes de comprobar el puntero, incluso nulo.

ARM64 X16=-3 y X16=-4 devuelven los 64 bits sin signo de `mach_absolute_time` y `mach_continuous_time`. Cada llamada solo necesita su propio valor; el cero explícito es válido. Las entradas nativas x64 correspondientes generan EXC_SYSCALL y no están admitidas. Siguen pendientes relojes que avanzan, commpage, temporizadores y objetos de reloj Mach/IPC.

La resolución usa los 32 bits bajos del número; el informe conserva los 64 originales. Los negativos ARM64 eligen Mach; x64 usa 0x01000000 para Mach y 0x02000000 para BSD. BSD 3/4 siguen siendo read/write; números desconocidos y clases ajenas detienen. La entrada resuelta determina el retorno: Mach conserva flags y X1/RDX, BSD mantiene sus reglas carry; x64 sigue actualizando RCX/R11. Los informes Mach incluyen `result` y omiten `error`, incluso con carry inicial activo.

`mach-time` compara flags, resultado secundario, bits altos, punteros inválidos y transiciones BSD con el núcleo ARM64 nativo. `mach-timebase-values` verifica bytes exactos en cinco invitados, `mach-clock-values` en ARM64; el SDK comprueba estructura y razón capturada. Intel HVF Actions sigue suspendido; pruebas de software y sintaxis x64 no validan Intel nativo ni iOS físico.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Validación Mach (2026-10-06, Release): 569 casos Darwin, 293 aprobados, 276 omitidos por backend no disponible, cero fallos; se ejecutaron los 69/69 obligatorios ARM64 HVF. La ejecución final aprobó 13 programas nativos y dos oráculos temporales SDK. C/CLI/report: 100/100 sin omisiones; Python cubrió cinco invitados. Las comparaciones públicas se aíslan por plataforma y escenario con presupuesto invitado explícito de 10 segundos; valores predeterminados y regresiones de plazo siguen iguales. Los recuentos se solapan.

Los primeros arranques nativos excedieron el límite existente de 5 segundos: medición independiente de 6.056 segundos y 0.010 al reutilizar. El mismo binario aprobó después 13 casos bajo el límite original; se conservan los fallos. La verificación secuencial separada aprobó tras los tiempos agotados bajo carga. Evidencias: `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`, del árbol previo al commit. En esa revisión, ARM64 MRS/MSR NZCV no formaban parte del contrato checked; el test observaba los flags con instrucciones enteras. El cambio siguiente resuelve esa carencia de CPU.

## Registro de flags de condición ARM64

El contrato ARM64 checked compartido admite las codificaciones exactas `MRS Xt, NZCV` y `MSR NZCV, Xt` en EL0/EL1. Las lecturas devuelven solo los bits 31–28; las escrituras toman esos cuatro bits de entrada e ignoran los demás. Leer hacia `XZR` descarta el resultado; escribir desde `XZR` borra los flags sin leer SP. Cada backend ejecuta las instrucciones originales. La validación del setter del host y los límites FPCR/FPSR no cambian; los registros de sistema vecinos no declarados siguen sin admitirse.

`NeverDAArch64NZCVTests` compara todas las combinaciones con instrucciones del host y verifica estado escalar/vectorial completo, memoria, registros límite, parada/fallo del observador, restauración del contexto y presupuestos compartidos. ARM64 `mach-time` usa ahora MSR/MRS reales alrededor de SVC para comprobar la conservación Mach y la transición a BSD. Los requisitos HVF nativos incluyen seis métodos en ambos privilegios y el oráculo del host. ARM64 KVM/WHP e iOS físico siguen sin validar. Quedan archivos escribibles, información del sistema, relojes que avanzan, Mach IPC/hilos, dyld/runtimes/frameworks y aceptación en dispositivos.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).


Validación de archivos modificables (2026-10-06): Release Darwin, 610 registros, 322 aprobados, 288 omitidos por backend no disponible, cero fallos; 72/72 requisitos ARM64 HVF ejecutados. La revisión final con nuevas aserciones EFAULT/metadatos suma 102 aprobados y 12 omitidos de 114. Pasaron los 15 programas nativos y 111 pruebas públicas C/CLI/informes. Los recuentos se solapan. La primera ejecución nativa detectó FWASWRITTEN; se corrigió y se conserva el fallo original. Sin cambios de límites temporales. CI GitHub completa e iOS físico siguen aparte; Actions Intel suspendido.

`build-hvf-arm64/writable-darwin-evidence/` · `writable-native-final/` · `writable-focused-final.xml` · `writable-public.xml`

Python superó inicialmente cinco segundos en tres casos ARM64 de directorio. Con argumentos idénticos pasaron los diez escenarios nuevos de escritura; uno iOS agotó5,005 s reales con1,263 s CPU. Las tres repeticiones aisladas pasaron con el mismo límite en2,43–3,17 s,10.941 instrucciones y salida65. Carga54–70 con16 CPU lógicas apoya presión de planificación, no garantiza latencia; se conservan fallos originales.

El método Python final sin cambios pasó las cinco combinaciones en41,118 s, conservando cinco segundos por proceso y los fallos/diagnósticos anteriores por separado.


Validación de metadatos (2026-10-06): Release focalizado148=124 aprobados/24 omitidos. Darwin completo645=343 aprobados/300 omitidos/2 tiempos agotados de directorios ARM64 HVF existentes. Repetición idéntica20=8 aprobados/12 omitidos, casos afectados3.818/3.949s con límite original5s. Las75 identidades HVF requeridas tienen observaciones exitosas; se conserva el primer fallo. C/CLI/informes117/117 con73 Darwin, Python cinco perfiles27.359s, nativo15/15, runners66/66 aprobados. Asignación virtual, no prueba APFS. Sin cambiar plazos; CI completa, Intel, iOS físico y entorno completo pendientes.

`build-hvf-arm64/mutation-metadata-validation-summary.json`; `mutation-metadata-darwin-evidence/`; `mutation-metadata-directory-recheck/`; `mutation-metadata-focused.xml`; `mutation-metadata-public.xml`.

Validación de búsqueda dispersa (2026-10-06): Release Darwin, 671 casos, 359 aprobados, 312 omitidos por backends no disponibles, sin fallos. Se ejecutaron los 78 casos ARM64 HVF obligatorios; Unicorn cubrió cinco perfiles. Pruebas específicas: 123 aprobadas de 147, 24 omitidas. Pasaron 16 programas nativos, 122 comprobaciones C/CLI/report (78 comparaciones Darwin), cinco perfiles Python (12.344 s) y 66 pruebas del runner. Los recuentos se solapan; no cambiaron los límites y se conservan los fallos históricos. Evidencia: `build-hvf-arm64/sparse-seek-validation-summary.json`. La asignación es una política virtual explícita, sin equivalencia APFS. CI completa e iOS físico siguen pendientes; Intel HVF Actions permanece suspendido.

Validación unlink (2026-10-06): Release Darwin708 casos,384 aprobados,324 omitidos por backends no disponibles, sin fallos;81 ARM64 HVF obligatorios ejecutados. Específicos156:137 aprobados/19 omitidos. Nativos17/17, C/CLI/report128/128 (Darwin83), Python cinco perfiles16.268s, runner66/66 aprobados. Revisión independiente sin bloqueos pendientes. Recuentos solapados, plazos intactos, sin reintentos. Evidencia: `build-hvf-arm64/unlink-validation-summary.json`. Invalidación/tiempos fijos son reglas del modelo; sistema de archivos/runtime completo e iOS físico siguen pendientes. Intel HVF Actions suspendido; CI completa separada.

### Validación de creación, 2026-10-06

Release Darwin:748 casos,412 aprobados,336 omitidos por backend ausente, cero fallos;84 requisitos ARM64 HVF ejecutados. Dirigidos162:150 aprobados/12 omitidos. C/CLI/informes133/133 (Darwin88), Python5 perfiles9.982s, nativos18/18, runners66/66 aprobados. ARM64 rechazó correctamente las rebases de la tabla de punteros del test inicial; bytes internos la corrigieron sin relajar el cargador. Se conservan fallos/binarios iniciales; inventario esperado27→28. Revisión independiente sin bloqueos, incluida preservación del padre ante límite de capacidad. Cuentas solapadas y plazos iguales. CI completa/iOS físico separados; Actions Intel HVF suspendido.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### Validación de metadatos de creación, 2026-10-06

Release Darwin: 787 registros, 439 aprobados, 348 omitidos por backend no disponible, cero fallos; ejecutados los 87 ARM64 HVF obligatorios. Específicos: 139/151 aprobados, 12 omitidos. C/CLI/informe: 145/145, incluidos 98 contrastes de entrada Darwin; método Python sin cambios, cinco perfiles en 12.211 segundos. Nativos 19/19 y verificadores 66/66 aprobados. Revisión independiente sin bloqueos; nuevos casos cubren device/GID de distintos padres y secuencia inode global, unlink antes de escribir, umask sin FD libre/entrada utilizable. Recuentos solapados, plazos intactos, sin repetición por fallo. Tiempos fijos de creación/mutación y asignación son política virtual. GitHub CI completa e iOS físico se validan aparte; Intel HVF Actions suspendido.

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### Verificación del renombrado, 2026-10-06

Release Darwin: 835 registros,474 aprobados,360 omitidos y un timeout existente de metadatos virtuales macOS ARM 64 HVF (5.087 s). Repetición con mismos argumentos y límite de 5 s: 8 aprobados,12 omitidos; identidad afectada 0.113 s. Las 90 identidades ARM 64 HVF obligatorias tienen observaciones aprobadas entre ambas ejecuciones; el gate completo sigue registrado como fallido. Específicos 42/54 aprobados,12 omitidos; C/CLI/informe 150/150, incluidas 103 comparaciones Darwin; Python sin cambios, cinco perfiles en 18.478 s; nativos 20/20, scripts 66/66. Revisión independiente corrigió clasificación de puntos anidados, con regresión 4 K/16 K fallida antes y aprobada después. Una expectativa anterior de ftruncate de solo lectura se corrigió a EINVAL. Se conservan fallos y versiones de sondas, conteos superpuestos y plazos iguales. GitHub CI completo e iOS físico separados; Intel HVF Actions suspendido.

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## Observaciones explícitas del sistema

`ProcessOptions::DarwinSystem` / `darwin_system` aporta observaciones fijas a `sysctl(202)` y `sysctlbyname(274)` directo en todos los perfiles Darwin. Cada campo es opcional; los valores ausentes y las claves no enumeradas quedan sin soporte. No se consulta el host ni se deducen versiones o modelos. La validación estricta JSON y C++ rechaza valores incorrectos y perfiles ajenos a Darwin antes de cargar la imagen.

`os_revision` tiene 32 bits con signo; `cpu_count` vale 1..INT32_MAX; `memory_size` conserva 64 bits sin signo; `max_files_per_process` vale 0..INT32_MAX y se codifica como int de cuatro bytes. Los demás campos escalares son cadenas de hasta 1023 bytes (255 para `hostname`) sin NUL interno; una cadena vacía explícita es válida y la salida incluye el NUL final. Estas observaciones no cambian la planificación ni los presupuestos de memoria o descriptores.

| Campo JSON | Nombre sysctl | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |
| `max_files_per_process` | `kern.maxfilesperproc` | `1,29` |
| `hostname` | `kern.hostname` | `1,10` |

`hw.pagesize` procede de la política de memoria existente: normalmente ocho bytes, cuatro si la salida no nula tiene capacidad exactamente cuatro. El MIB antiguo `[6,7]` y `hw.pagesize_compat` siempre devuelven cuatro bytes. El OID numérico dinámico de `hw.pagesize` no está soportado. `hw.memsize` solo se reduce con capacidad cuatro si su patrón de 64 bits es la extensión de signo de un entero de 32 bits; de lo contrario ERANGE34 conserva salida y longitud.

El número MIB usa los 32 bits bajos y debe ser 2–12; la longitud del nombre usa 64 bits y debe ser menor que 1024. Se comprueban todos los bytes antes de interpretar el primer NUL y quitar un punto final. Un nombre vacío devuelve ENOENT; una entrada parcialmente legible queda sin soporte. Un `oldlenp` no nulo requiere ocho bytes completamente legibles y escribibles antes de los efectos. Las pruebas nativas con punteros de longitud inválidos no retornaron dentro del plazo, por lo que quedan explícitamente fuera del alcance. `oldlenp` nulo significa capacidad cero; `oldp` nulo consulta solo el tamaño. Para claves distintas de `kern.hostname`, un búfer corto devuelve ENOMEM12, conserva los datos y escribe longitud cero. EFAULT en los datos conserva la longitud anterior. Primero se capturan entrada y capacidad, después los datos y finalmente la longitud, conservando alias y copias previas ante un error de transporte posterior.

`hostname` declara los bytes visibles para este llamador guest, sin consultar el host, asumir `localhost` en móviles ni deducir entitlements. La ausencia sigue siendo desconocida; la cadena vacía explícita devuelve un NUL. Para `kern.hostname`, una salida no nula con capacidad positiva insuficiente tiene éxito con exactamente esa cantidad de bytes, terminados en NUL, y comunica esa capacidad. La capacidad cero conserva ENOMEM12, longitud cero y datos intactos; una salida nula informa la longitud completa con NUL. Solo se comprueba el intervalo de salida real. Un intervalo parcialmente escribible sigue sin soporte y no publica un prefijo; las copias nativas parciales quedan fuera del modelo. Solo se añade la observación raw de libc uname/gethostname, no sus imports dylib ni un runtime completo.

newp/newlen no nulos significan escritura. Se conservan primero lectura nombre/MIB y precomprobación completa lectura/escritura oldlenp. EUID predeterminado o explícito no-root da EPERM1 antes de observación/salida de datos. EUID0 detiene kern.osversion / kern.maxfilesperproc / kern.hostname unsupported porque su escritura privilegiada no está modelada; RUID no decide. Otros nodos nativos de solo lectura conservan EPERM1 incluso root. Nueva longitud0 ignora puntero; ningún ENOENT inventado para claves/árboles/OID dinámicos desconocidos.

El programa original `system-info` comprueba la ABI nativa macOS y del guest; `virtual-system` compara bytes configurados mediante C++, C/CLI y Python. Un oráculo SDK captura las nueve observaciones del host como entradas explícitas y compara consultas por nombre y número. Esto no valida iOS físico ni Intel HVF.

```json
{"darwin_system":{"hostname":"guest-node","os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man 3/sysctl.3.html).

### Verificación de consultas del sistema, 2026-10-06

Release Darwin: 881 registros, 509 aprobados, 372 omitidos por backend no disponible y cero fallos; se ejecutaron las 93 identidades ARM64 HVF obligatorias. Específicos: 37/49 aprobados y 12 omitidos. C/CLI/informe: 163/163, incluidas 113 comparaciones Darwin. Python sin cambios: cinco perfiles en 15.302 s; programas nativos 21/21 y scripts 66/66. Revisión independiente sin bloqueos; prioridades adicionales y oráculo SDK aprobados. La compilación del nuevo oráculo falló una vez por faltar StringExtras y pasó al añadirlo; se conservan código y registro. Las sondas nativas de longitud inválida se conservan fuera del contrato admitido. Tras las pruebas solo se normalizaron dos comentarios de cabecera y se reconstruyó con éxito. Conteos superpuestos, plazos iguales y ninguna repetición por fallo de ejecución necesaria. GitHub CI completo e iOS físico siguen separados; Intel HVF Actions suspendido.

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## E/S vectorial de archivos y captura

`readv`/`writev`, `preadv`/`pwritev` y nocancel comparten la lógica escalar de archivos y captura, sin nuevas opciones ni acceso al host. Cada iovec LP64 contiene dirección y longitud de ocho bytes. Los32 bits bajos con signo de iovcnt deben ser1–1024. Se copia toda la matriz antes de buscar el descriptor; los alias de salida no alteran la petición. Una matriz parcialmente legible sigue sin soporte.

Los permisos y la capacidad de posicionar un flujo se comprueban antes de las longitudes. Cada valor y la suma deben caber en INT64_MAX; archivos y directorios además limitan la suma a INT_MAX. El stdin finito se recorta a los bytes disponibles y la captura conserva su presupuesto. pwritev rechaza cualquier posición negativa antes de la matriz; preadv la comprueba después del descriptor y las longitudes. Los elementos vacíos ignoran su dirección, conservando las reglas de descriptor, tipo y posición. EOF evita tocar la cola no usada. Las llamadas posicionadas conservan el cursor y pwritev ignora append. El append ordinario recorta toda la petición una sola vez según el cursor inicial y después elige EOF.

Un elemento posterior totalmente inválido devuelve EFAULT conservando bytes anteriores, avance del cursor normal y FWASWRITTEN tras escribir al menos un byte. Una escritura no vacía admitida que devuelve EFAULT por un búfer de datos invalida los metadatos completos; los errores de argumentos, rechazos del modelo y fallos del backend los conservan. Un destino de lectura parcialmente accesible devuelve UnsupportedService sin copiar ese elemento y conserva copias previas. Una fuente de escritura de archivo parcialmente legible se rechaza antes de cualquier efecto. Autorización, leases de mapeo y presupuesto total preceden a la escritura; los errores de comprobación o lectura del backend no publican bytes de archivo ni captura.

La captura comprueba primero el presupuesto compartido stdout/stderr. Un elemento que cruza el límite de dirección de usuario no aporta bytes; se conservan los anteriores. Otros prefijos legibles se capturan con EFAULT. La prioridad escalar del error de rango sobre el presupuesto no cambia. Los descriptores duplicados o redirigidos conservan su destino. El programa original `vectored-io` comprueba las ocho entradas en macOS nativo, cinco combinaciones invitadas y C/CLI/Python. No añade cancelación, pipes, hilos ni aceptación de iOS físico.

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### Verificación de E/S vectorial, 2026-10-06

Release Darwin:937 registros,553 aprobados,384 omitidos por backend no disponible, cero fallos;96 identidades ARM64 HVF obligatorias ejecutadas. Focalizados45/57 aprobados,12 omitidos. C/CLI/report168/168, incluidas118 comparaciones Darwin; Python cubrió cinco combinaciones en 20.397s. Nativos22/22 y scripts66/66. La revisión independiente añadió un fallo de escritura posicionada dispersa que comprueba cursor, EOF real, rechazo de metadatos y capacidad restante exacta. La primera compilación aún referenciaba una consulta interna eliminada desde un test antiguo; ahora verifica la salida real. Un error optional<bool> de la nueva aserción de eventos marcó como fallidas ocho ejecuciones invitadas exitosas; tras corregirlo pasaron las verificaciones afectadas. Se conservan fuentes y registros de ambos fallos. Conteos solapados, límites sin cambios. GitHub CI completa e iOS físico separados; Intel HVF Actions sigue suspendido.

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## Consultas de existencia de archivos

`access(33)` y `faccessat(466)` consultan el catálogo virtual actual sin asignar descriptores ni cambiar contenido, cursores, indicadores o metadatos. F_OK confirma el nombre según el contrato de recorrido existente. Los metadatos no conceden ni revocan acceso al catálogo; no se validan permisos nativos de búsqueda de antecesores, ACL o MAC. Un stat ausente o invalidado no impide consultar. Los nombres eliminados dan ENOENT aunque antiguos FD o mapeos retengan el objeto; creación, reutilización y renombrado siguen el espacio actual.

El modo usa los 32 bits bajos. R/W/X ocupa los bits 0–2, los derechos extendidos 9–21. `(mode & 0x003ffe07) == 0` consulta existencia; los demás bits, incluido el signo, se ignoran sin EINVAL. Las peticiones de permisos siguen siendo UnsupportedService tras buscar con éxito, sin inferirlos de metadatos ni autorizaciones de modificación. Los errores conocidos de ruta/descriptor ocurren antes.

Faccessat admite cualquier combinación de AT_EACCESS(0x10), AT_SYMLINK_NOFOLLOW(0x20), AT_SYMLINK_NOFOLLOW_ANY(0x800) en los bits bajos. Otros indicadores dan EINVAL antes de ruta o FD, incluso sin catálogo. Las identidades real/efectiva son fijas; los enlaces fijos siguen las reglas siguientes. Las rutas absolutas ignoran dirfd; las relativas conservan las reglas CWD/FD de directorio. nameiat fuera de AT_FDCWD lee un byte, comprueba el FD relativo e importa la cadena; `/` omite FD. Primer byte inaccesible: EFAULT14; FD desconocido/archivo: EBADF9/ENOTDIR20 antes de fallos posteriores. La ruta relativa vacía aún comprueba el FD: desconocido EBADF, archivo normal ENOTDIR, en otro caso ENOENT. Un catálogo ausente o identidad de directorio de flujo desconocida siguen sin soporte.

El original `file-access` compara ambas llamadas, bits ignorados, indicadores y orden en macOS nativo y cinco invitados mediante C++/C/CLI/Python. NOFOLLOW_ANY usa un FD relativo para evitar enlaces host `/tmp` o `/var`. Los tests directos cubren nombres vivos, bits mixtos, agotamiento de FD, independencia de metadatos y fallos de memoria.

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### Verificación de existencia de archivos, 2026-10-06

Release Darwin:971 registros,575 aprobados,396 omitidos por backend no disponible, cero fallos;99 identidades ARM64 HVF obligatorias ejecutadas. Focalizados23/35 aprobados,12 omitidos, incluidos14 directos. C/CLI/report173/173, incluidas123 comparaciones Darwin. Python verificó cinco combinaciones en 16.235s; nativos23/23 y scripts66/66. La revisión independiente de diseño e implementación no detectó bloqueos. Se conserva el resultado NOFOLLOW_ANY inicial por el enlace host /tmp y su comparación con ruta canónica; el programa compartido usa un FD de directorio relativo. Conteos solapados, límites sin cambios y sin fallos de ejecución que repetir. GitHub CI completa e iOS físico separados; Intel HVF Actions sigue suspendido.

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## Crear y eliminar directorios

`mkdir(136)` y `mkdirat(475)` crean dentro de un padre inmediato explícitamente modificable. Los nuevos directorios heredan autorización sobre nombres; los iniciales mantienen permisos declarados propios. Solo heredan device/GID conocidos, nunca stat completo, tamaño, asignación, tiempos o cookies. Un directorio nuevo oculta todas las observaciones del archivo anterior homónimo. `creation_policy` sigue siendo exclusiva de archivos regulares: los descendientes usan esa identidad y la secuencia global de inodes; mkdir no consume inodes de archivo. La comprobación de permisos y metadatos nativos de directorio queda fuera.

El recorrido común admite para mkdir un nombre final ausente seguido solo por barras. Antecesor ausente antes de punto/doble punto da ENOENT, archivo antecesor ENOTDIR y nombre existente EEXIST. Se mantienen FD/CWD relativos, independencia del FD en rutas absolutas y prioridad de fallos de cadena. No requiere FD libre; rechazos de búsqueda, autorización, presupuesto o transporte no publican cambios.

`rmdir(137)` y `unlinkat(472)` con AT_REMOVEDIR(0x80), opcionalmente AT_SYMLINK_NOFOLLOW_ANY(0x800), eliminan directorios vacíos creados por este proceso. Bits bajos32 desconocidos dan EINVAL antes de entradas; DATALESS y SYSTEM_DISCARDED quedan fuera. Se mantienen errores conocidos de ruta/tipo/raíz; eliminar directorios iniciales sin autorización removable sigue siendo UnsupportedService. En los admitidos, punto final da EINVAL, doble punto desde un directorio enlazado o destino no vacío ENOTEMPTY. Los FD de directorio, dup y CWD conservan el objeto original y ya no impiden borrarlo. Archivos regulares ya unlink y sus mapeos no cuentan como nombres: comparaciones nativas conservan bytes, inode y último F_GETPATH tras eliminar/reutilizar el padre.

Cada ruta canónica+NUL y una entrada cuentan en el presupuesto común16 MiB/256 entradas. Solo se devuelve ese coste tras eliminar y liberar todas las referencias, conservando archivos huérfanos/mapeos. Solo el éxito invalida stat/enumeración del padre; observaciones completas del directorio nuevo siguen desconocidas. El original `directory-mutations` compara creación anidada, renombrado, unlink, eliminación y reutilización en macOS nativo y cinco invitados mediante C++/C/CLI/Python. Un archivo huérfano retenido por una descripción o lease de mapeo conserva sus directorios padres y sus costes actuales de ruta/NUL y entrada, aunque se cierren todos los FD de directorio. La recuperación del archivo precede la cadena de directorios. Mover un ancestro creado vivo actualiza F_GETPATH; reutilizar el nombre no asigna objetos antiguos al reemplazo.

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### Verificación de cambios de directorio, 2026-10-06

Release Darwin:1.017 registros,609 aprobados,408 omitidos por backend no disponible,cero fallos;102 ARM64 HVF obligatorios ejecutados. Focalizados58 aprobados,12 omitidos,incluidos26 nuevos directos4K/16K. C/CLI/report178/178,incluidos128 Darwin;Python cinco combinaciones en 17.255s,nativos24/24,scripts66/66. Revisión independiente de presupuestos,reutilización,identidad paterna,concesiones y reversión. Una sonda nativa posterior al primer éxito detectó EISDIR en raíz solo de barras y EBUSY con punto/doble punto final;se corrigió la decisión común y el programa nativo/invitado. Se conservan pruebas iniciales y copias fuente/binarias. Conteos solapados,límites sin cambios,Intel HVF Actions suspendido;GitHub CI completa e iOS físico separados.

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.

## Identidades de directorio retenidas

FD/CWD conservan el objeto borrado y su cadena de padres aunque se reutilicen nombres. Open de punto tiene cursor independiente;dup lo comparte;doble punto sigue el padre original. Hijos ordinarios de un directorio borrado dan ENOENT. LOOKUP atraviesa padres borrados retenidos;crear/borrar/renombrar devuelve ENOENT. Punto/doble punto final de renombrado da EINVAL antes de ese componente,tras errores de antecesores. F_GETPATH conserva la última ruta;stat/listado completos quedan desconocidos. Ruta+NUL y una entrada siguen contados hasta liberar todos los FD/CWD/hijos retenidos. Close/dup2/cambio de CWD/admisión de mutación recuperan cadenas inaccesibles;costes iniciales y concesiones de archivos separados. `deleted-directories` lo compara nativamente y con cinco invitados. Un archivo huérfano retenido por una descripción o lease de mapeo conserva sus directorios padres y sus costes actuales de ruta/NUL y entrada, aunque se cierren todos los FD de directorio. La recuperación del archivo precede la cadena de directorios. Mover un ancestro creado vivo actualiza F_GETPATH; reutilizar el nombre no asigna objetos antiguos al reemplazo.

### Verificación de vida de directorios, 2026-10-06

Release Darwin1.051 casos,631 aprobados,420 omitidos no disponibles,cero fallos;105 ARM64 HVF obligatorios ejecutados. Focalizados98 aprobados/12 omitidos,directos iniciales64/64 con14 nuevos. C/CLI/report183/183,133 Darwin;Python cinco combinaciones 18.691s,nativos25/25,scripts66/66. Sonda nativa adicional corrigió orden del punto final de renombrado;fuentes/resultados/copias iniciales conservados. Agente principal cotejó evidencias;revisión independiente final no disponible. Conteos solapados,límites iguales,CI completa/iOS físico separados,Intel HVF Actions suspendido.

`build-hvf-arm64/directory-lifetime-validation-summary.json`, `directory-lifetime-darwin-final-evidence/`, `directory-lifetime-focused-final.xml`, `directory-lifetime-public.xml`, `directory-lifetime-native/`, `directory-lifetime-before-rename-fix/`.

## Eliminar directorios iniciales admitidos explícitamente

El booleano estricto `"removable": true` de una entrada de directorio (C++ `DarwinFileOptions::RemovableDirectories`) declara un directorio ordinario sin montaje y con una sola identidad en el espacio de nombres. Debe ser una entrada inicial explícita de `directories`, distinta de la raíz, con padre inmediato explícitamente mutable. Se rechazan modos/flags especiales conocidos, alias de inode (incluidas las instantáneas) y números de dispositivo conocidos incompatibles entre padre y destino. Su igualdad no demuestra por sí sola que no haya montaje. La omisión o false mantiene el límite no admitido; otros tipos JSON son inválidos. No proporciona un modelo general de permisos ni montajes.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/empty","removable":true}],"working_directory":"/empty"}}
```

El espacio de nombres actual debe estar vacío. Los subdirectorios iniciales implícitos permanecen tras unlink de su último archivo original. El éxito invalida las observaciones completas stat/enumeración del objeto y su padre inmediato; los FD/dup/CWD anteriores conservan el objeto y la cadena original de padres. Las entradas inmutables nunca hacen reaparecer un nombre eliminado. Un archivo o directorio nuevo con ese nombre tiene identidad independiente y no recupera metadatos ni instantáneas anteriores. Los datos del llamador permanecen intactos.

Cada referencia removable carga su ruta y NUL al presupuesto inicial fijo de 16 MiB. Las entradas, rutas, referencias e instantáneas iniciales siguen contándose después del borrado y último cierre, incluido su espacio dentro del límite de 256 entradas. Los objetos nuevos mantienen su contabilidad dinámica propia. El programa original `initial-directory-removal` elimina el directorio vacío preexistente de la prueba nativa mientras sigue abierto, reutiliza el nombre para un archivo y luego un directorio, comprueba la retención exclusiva por CWD y restaura el directorio vacío. El mismo programa se ejecuta mediante C++/C/CLI/Python en las cinco combinaciones invitadas.

### Verificación de eliminación de directorios iniciales, 2026-10-06

Las fuentes finales en Release conciliaron 1.089 pruebas Darwin: 657 aprobadas, 432 omitidas por backend no disponible, cero fallos; se ejecutaron los 108 casos ARM64 HVF obligatorios. Las pruebas enfocadas aprobaron 27/39 con 12 omisiones, y también pasó la comprobación adicional de alias derivados solo de instantáneas. C/CLI/informes públicos: 191/191; Python: cinco combinaciones en 76,276 s; programas nativos originales: 26/26; ejecutores de evidencias: 66/66. Se corrigió una entrada de prueba incoherente de inode/instantánea y se conservaron sus fallos.

Dos ejecuciones completas anteriores tuvieron uno y tres tiempos agotados en casos existentes de archivos/renombrado. Una ejecución instrumentada reprodujo uno con 5,008 s reales y 0,171 s de CPU del proceso. Las comparaciones del mismo método y de programas anteriores pasaron, pero la causa de la latencia sigue sin resolverse; la aprobación final no demuestra estabilidad temporal. Se retiraron los diagnósticos temporales, se restauraron los hashes de los programas y se mantuvo el límite original de 5 s del invitado. Auditoría principal de fuentes/evidencias completada; revisión independiente no disponible. Los recuentos se solapan. La CI completa de GitHub, iOS físico y las Actions Intel HVF suspendidas quedan fuera de esta aceptación local.

`build-hvf-arm64/initial-directory-validation-summary.json`, `initial-directory-darwin-restored-evidence/`, `initial-directory-public.xml`, `initial-directory-native-evidence/`, `initial-directory-timeout-probe/`.

## Renombrado exclusivo de archivos regulares

Tras resolver origen y destino, RENAME_EXCL devuelve EEXIST para otro archivo o directorio existente antes de comprobar montajes o mutaciones. Siguen primero los errores anteriores de ruta, incluido EINVAL por punto/doble punto final. Un destino ausente usa la misma transacción acotada y conserva descripciones abiertas, cursores, flags, préstamos de mapeo y transiciones de metadatos configuradas. El mismo objeto continúa explícitamente no soportado: el resultado nativo depende de la distinción de mayúsculas del sistema de archivos, que las claves exactas no prueban. Normalización de mayúsculas, directorios iniciales como origen y SECLUDE quedan fuera. El programa original `renamed-file` compara rechazo sin cambios de metadatos y éxito EXCL|NOFOLLOW_ANY en macOS nativo y C++/C/CLI/Python.

Verificación, 2026-10-06 (Release): 1.097 pruebas Darwin, 665 aprobadas, 432 omitidas por backend no disponible, sin fallos; ejecutados los 108 casos ARM64 HVF obligatorios. Enfocadas: 44 aprobadas, 12 omitidas, incluidos ocho casos directos nuevos. C/CLI/informes: 191/191; Python: cinco combinaciones en 19,241 s; sonda independiente: 26 comprobaciones aprobadas. El primer intento nativo agotó el tiempo de return; los otros 25, incluido renamed-file, pasaron. Tres revisiones return del mismo binario sin cambios tardaron 0,014–0,034 s y después pasaron los 26 casos con el límite original de 5 s. El fallo inicial se conserva sin causa determinada; estos resultados y el éxito HVF anterior no demuestran estabilidad de latencia. Auditoría principal terminada; revisión independiente no disponible. Recuentos solapados; iOS físico, CI GitHub completa y Actions Intel HVF suspendidas fuera de la aceptación local.

`build-hvf-arm64/exclusive-rename-validation-summary.json`, `exclusive-rename-darwin-evidence/`, `exclusive-rename-public.xml`, `exclusive-rename-native-evidence/`, `exclusive-rename-native-rechecked-summary.json`, `exclusive-rename-probe/`.


## Renombrado entre directorios creados

Un directorio inicial y todos sus descendientes creados por este proceso con mkdir/mkdirat comparten un dominio de nombres virtual. `rename`, `renameat` y `renameatx_np` admiten mover archivos normales entre esos padres sin un nuevo campo JSON. Tras crear `/work/left` y `/work/right` bajo un `/work` inicial mutable, `/work/data` puede moverse a los hijos y entre ellos. Un `/work/left` declarado por separado como inicial conserva su propio dominio, incluso con el mismo device. La topología general de montajes sigue desconocida.

Ambos padres inmediatos necesitan autorización; los directorios creados la heredan junto con device/GID conocidos. El archivo conserva identidad, propietario/grupo, permiso de escritura y asignación. Se rechazan devices contradictorios. Un movimiento invalida stat/enumeración completos de ambos padres. Los directorios iniciales eliminados y rutas reutilizadas son objetos distintos; FD/CWD antiguos no obtienen el dominio del reemplazo.

Se mantienen transacción acotada, leases de mapas, costes de ruta/NUL y prioridad de errores. EXCL contra un destino existente distinto da EEXIST antes de dominio/autorización. `renamed-file` compara movimiento al hijo creado, reemplazo en el padre inicial y retorno mediante C++/C/CLI/Python y macOS nativo. Permisos, movimiento de directorios iniciales, creación dinámica de enlaces físicos/simbólicos y metadatos APFS nativos siguen pendientes.

### Verificación entre padres, 2026-10-06

La validación Release final concilió 1,115 registros Darwin: 683 aprobados, 432 omitidos por backend no disponible y ningún fallo; se ejecutaron los 108 casos ARM64 HVF obligatorios. Las comprobaciones directas aprobaron 56/56, incluidos 18 casos nuevos 4K/16K. C/CLI/report público aprobó 191/191; el método Python cubrió cinco perfiles en 22.254s. Programas nativos originales: 26/26; sonda independiente de llamadas crudas: 34 comprobaciones; scripts de documentación/capacidades/ejecución de evidencias: 296/296. Los recuentos se solapan. Los hashes de los diez binarios de validación no cambiaron tras el ajuste CMake exclusivo de MSVC.

Las dos validaciones completas anteriores conservan tres y dos tiempos agotados en el método HVF de archivos existente. Las comparaciones del método completo, directorio de trabajo y sesión aprobaron sin establecer la causa; la aprobación final no demuestra estabilidad de latencia. La sonda comparaba inicialmente /tmp con /private/tmp canónico; consultar la ruta del FD raíz corrigió cuatro expectativas. El programa nativo ampliado usaba mkdir(136) para limpiar; rmdir(137) corrigió exit150. Se conservan fuentes y fallos iniciales, y el límite invitado sigue en 5s.

Cuatro conflictos LP64 de listas de inicialización encontrados por la CI Linux completa usan ahora valores uint64_t explícitos; NeverDJumpTableTests recibe /bigobj bajo MSVC. La compilación real Linux/Windows espera la CI. La CI completa anterior también notificó fallos separados del corpus Windows EH y la cancelación de una PR cerrada. Se completó la revisión propia de fuentes/evidencias; no se afirma revisión independiente ni validación de iOS físico o Intel HVF suspendida.

`build-hvf-arm64/cross-parent-rename-validation-summary.json`, `cross-parent-rename-darwin-synced-evidence/`, `cross-parent-rename-darwin-evidence/`, `cross-parent-rename-darwin-rechecked-evidence/`, `cross-parent-rename-public-synced.xml`, `cross-parent-rename-python-synced.log`, `cross-parent-rename-native-fixed-evidence/`, `cross-parent-rename-probe/`.

## Intercambio atómico de nombres de archivos regulares

RENAME_SWAP=0x2 intercambia los nombres de dos archivos regulares existentes con renameatx_np, opcionalmente con RENAME_NOFOLLOW_ANY. Un directorio inicial explícito debe declarar mutable:true y swap_rename:true; C++ usa DarwinFileOptions::SwapRenameDirectories. Los descendientes creados heredan la capacidad del objeto inicial. Borrar y reutilizar una ruta no transfiere su declaración anterior. false u omisión significa capacidad desconocida; devices iguales y permisos del namespace no prueban soporte. Los dominios iniciales distintos siguen excluidos.

Ambas rutas usan el resolvedor existente. Destino ausente da ENOENT antes de dominio, autorización o capacidad. Todo operando directorio se rechaza explícitamente: swap nativo puede intercambiar archivo y directorio, por lo que EISDIR del rename ordinario no se aplica. El mismo objeto autorizado es una operación nula incluso sin declarar capacidad. RENAME_EXCL=0x4 + RENAME_SWAP=0x2 y flags desconocidos dan EINVAL antes de las rutas; SECLUDE sigue pendiente.

Ambos archivos permanecen enlazados. Conservan identidad, dueño/grupo, bytes, autorización de escritura, descripciones, cursores, flags y préstamos de mapeo propios. Las políticas virtuales configuradas actualizan cada ctime propia; políticas ausentes o invalidadas dejan metadatos completos desconocidos. Un intercambio real invalida metadatos y enumeración completos de ambos padres. No consume inode de creación, entrada ni FD; la entrada del llamante no cambia.

Cada referencia de capacidad reserva ruta+NUL en el presupuesto inicial fijo de 16 MiB. La transacción comprueba ambos costes dinámicos completos antes de publicar nombres; bytes y préstamos todavía enlazados no ofrecen crédito de reemplazo. Intercambios repetidos reutilizan esos costes. El programa original renamed-file intercambia hacia un hijo creado y vuelve, comprueba ambos objetos y continúa el reemplazo ordinario en macOS nativo y todos los perfiles C++/C/CLI/Python. Permisos, topología de montajes, normalización de mayúsculas, movimientos de directorios iniciales siguen pendientes.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/work","mutable":true,"swap_rename":true}]}}
```

[Apple volume swap capability](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsswaprenaming?changes=__1_2), [XNU rename](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

### Verificación del swap, 2026-10-06

Release Darwin: 1.133 registros, 701 aprobados, 432 omisiones por backend no disponible, cero fallos; se ejecutaron los 108 casos ARM64 HVF obligatorios. Comprobaciones directas rename 68/68, con 18 casos nuevos de opciones y 4K/16K. C/CLI/report público 192/192; el método Python cubrió cinco perfiles en 16,153s. Programas nativos originales 26/26, sonda independiente de llamadas brutas 45 comprobaciones. Recuentos solapados; límites invitados intactos.

Dos pruebas directas iniciales tenían expectativas erróneas para autorización de escritura desconocida y metadatos desconocidos tras mutación; sólo se corrigieron expectativas. Un filtro JSON inicial seleccionó cero pruebas y no cuenta; después aprobaron el propietario real y la suite pública completa. Se conservan fuentes y resultados iniciales. La latencia HVF/nativa anterior sigue sin explicación; este aprobado no demuestra estabilidad. Autorrevisión de fuentes/evidencias completada; no se afirma revisión independiente, iOS físico, CI GitHub completa ni aceptación Intel HVF suspendida.

`build-hvf-arm64/swap-rename-validation-summary.json`, `swap-rename-darwin-evidence/`, `swap-rename-public.xml`, `swap-rename-python.log`, `swap-rename-native-evidence/`, `swap-rename-probe/`.


## Rename de directorios creados por el proceso

rename, renameat y renameatx_np ordinarios mueven un directorio creado vivo y su subárbol dentro de un dominio inicial; requieren autorización de ambos padres inmediatos. Un destino ausente permite barras finales. Archivo destino: ENOTDIR; directorio no vacío: ENOTEMPTY; mover a un descendiente: EINVAL. El mismo nombre autorizado no cambia nada. EXCL da EEXIST para otro destino existente antes de tipo, ciclo, dominio o permiso. Errores de búsqueda destino preceden puntos de origen. Punto/doble punto origen al mismo objeto sigue UnsupportedService por desconocer la distinción de mayúsculas. Orígenes iniciales/eliminados, reemplazo de directorio inicial, dominios distintos, enlaces, permisos y SECLUDE siguen excluidos.

La pertenencia sigue objetos padres: hijos nombrados, directorios eliminados retenidos y huérfanos retenidos por FD/mapa actualizan sus rutas. FD, dup, CWD y doble punto origen siguen el mismo objeto y nuevo padre. El destino vacío reemplazado conserva ruta y padre antiguos; sus hijos ordinarios dan ENOENT, puntos/CWD conservan ese objeto. Sus huérfanos no siguen otra mudanza del reemplazo, aunque coincida el texto de ruta.

Los hijos conservan bytes, identidad, metadatos, permiso de escritura, cursores, flags FD y mapas. Origen y padres invalidan observaciones completas; los directorios creados mantienen autoridad heredada para crear y renombrar archivos admitidos. No requiere nueva entrada, FD ni inode. Antes de publicar se preparan todas las claves/rutas vivas y retenidas bajo 1024 bytes con NUL y 16 MiB. Solo un destino sin FD/CWD/descendientes retenidos aporta crédito, recuperado una vez. El rechazo preserva nombres, padres y observaciones; un mapa solo también retiene costes de padres eliminados.

El SDK-free original `renamed-directory` compara macOS nativo y cinco invitados por C++/C/CLI/Python; 4K/16K cubre identidad reutilizada, rollback, capacidad exacta, descendientes largos, créditos, recuperación de padres y agotamiento entrada/FD/inode.


### 2026-10-06

Release final:1,175 registros Darwin,731 aprobados,444 skips por backend ausente,cero fallos;111 ARM64 HVF obligatorios ejecutados. Foco32/44 (12 skips,22 nuevos4K/16K),límites finales4/4. C/CLI165/165 e informes32/32 sin skips;Python cinco perfiles21.759s,nativo27/27,sonda independiente79. La revisión independiente confirmó correcciones del separador raíz del programa y de la propiedad FS para puntos origen al mismo objeto. Se conservan exit124 iniciales,dos expectativas antiguas fallidas y fuentes/binarios;la verificación completa final aprobó. Recuentos superpuestos,plazos iguales. Los antiguos timeouts HVF/nativos no tienen causa probada ni establecen estabilidad. Intel HVF Actions suspendido;iOS físico y GitHub CI completo son separados.

`build-hvf-arm64/directory-rename-validation-summary.json`, `directory-rename-darwin-final-evidence/`, `directory-rename-public.xml`, `directory-rename-reports.xml`, `directory-rename-python.log`, `directory-rename-native-evidence/`, `directory-rename-probe/`, `directory-rename-initial-fixture/`, `directory-rename-darwin-evidence/failed-source/`.

### Verificación de intercambios de directorios y mixtos, 2026-10-07

La validación Release Darwin coteja 1.219 registros: 763 aprobados, 456 omitidos por backends no disponibles y cero fallos. Se ejecutaron los 114 casos ARM64 HVF obligatorios. La comprobación focalizada aprueba 32/44 con 12 no disponibles, incluidos los 24 casos directos 4K/16K nuevos; el componente completo de archivos aprueba 317/317. Los programas nativos originales aprueban 28/28 y una sonda independiente de llamadas crudas registra 32 observaciones exitosas en este sistema macOS que no distingue mayúsculas. Los recuentos se solapan y los plazos del invitado no cambian.

Una revisión independiente del plan y la implementación comprobó la transacción bidireccional, los costes de archivos iniciales, ambos subárboles retenidos, los huérfanos mantenidos solo por mapeo y los reembolsos exactos. La primera prueba roja omitía la declaración explícita de intercambio de la raíz y queda excluida de la aceptación; la base corregida fallaba en los seis casos por el antiguo rechazo de directorios. Dos aserciones iniciales de archivos mixtos descartaban incorrectamente metadatos configurados; ahora comparan el registro completo cambiando solo ctime. Una tabla local de punteros constantes introducía rebases ARM64 y seis rechazos de carga. Cuatro aserciones escalares la sustituyen: el programa corregido no tiene rebases clásicos y el cargador conserva el rechazo de fixups no admitidos.

La primera ejecución focalizada corregida conserva tres tiempos de espera HVF de cinco segundos. Pasaron los controles individuales, de tres perfiles y después el control completo; la causa sigue siendo desconocida y esto no prueba estabilidad de latencia. Se conservan fuentes, binarios, fallos y controles iniciales. Siguen pendientes movimientos de directorios iniciales, dominios iniciales distintos, declaraciones de permisos/montajes/mayúsculas, dependencias dinámicas y runtime de frameworks. iOS físico, Intel HVF suspendido y GitHub CI completo son límites de aceptación separados.

`build-hvf-arm64/directory-swap-research/`: `darwin-final-evidence/`, `darwin-evidence/`, `focused-v5.xml`, `file-owner-v5.xml`, `native-workload-fixed-evidence/`, `native-probe-v2-results.json`, `fixture-fixups-before/`, `hvf-controls.json`.

Los binarios finales enlazados repitieron con éxito la validación Darwin completa. La integración fijada de dev 0a9a1d28d registra 4.515 casos en 20 componentes compartidos: 4.489 aprobados, seis Z3 opcionales y 20 del corpus Windows EH no disponible omitidos, cero fallos. Incluye C/CLI 170/170 e informes 32/32. Python cubre los cinco perfiles en 30,780s. Las 12 variantes móviles nativas de arquitectura/fixup coinciden con 4.532 observaciones de los originales; sesión única y metadatos Swift completos coinciden 12/12. El generador de witnesses Swift reproduce su catálogo con el SDK/compilador registrado tras corregir la sangría de un comentario. Es aceptación local Release LLVM 23/Apple Clang 17, no de revisiones dev posteriores ni de Linux Clang 18.


## Movimiento ordinario de subárboles iniciales declarados

El booleano estricto `"movable": true` de un directorio inicial explícito que no sea raíz autoriza su rename ordinario y declara todo su subárbol inicial como directorios ordinarios sin montajes ni alias de nombre. `DarwinFileOptions::MovableDirectories` se añade después de los miembros agregados C++ existentes. El padre inmediato debe ser mutable. Ausencia/false mantiene el comportamiento desconocido; otros tipos JSON se rechazan. Los descendientes conservan sus propios permisos mutable/removable/movable y de escritura. No autoriza SWAP de directorios iniciales, permisos generales ni montajes. Se rechazan flags conocidos, modos especiales de directorio, archivos ordinarios con múltiples enlaces y alias inode de stat/instantáneas. Todo dominio conectado por las declaraciones puede tener como máximo un dispositivo conocido, incluidos hermanos y archivos bajo un ancestro sin stat. La igualdad de dispositivos no conecta otro dominio.

Los objetos conservan nombres, padres, stat/instantáneas y permisos originales. Las rutas de entrada antiguas no recrean nombres movidos o eliminados. Descendientes sin abrir, FD/dup/CWD, mappings y descendientes eliminados siguen sus objetos. Los descendientes sin cambios conservan stat, instantáneas, cookies y SEEK_END; la raíz movida y los padres modificados pierden observaciones completas. Reutilizar nombres no hereda observaciones ni permisos. La entrada inicial describe una ejecución síncrona, sin API de sustitución en caliente. SWAP sigue separado: declaración directa swap_rename en objetos iniciales, copia del padre en mkdir y conservación al mover. Ambos padres SWAP necesitan soporte declarado.

Sustituir un destino inicial vacío requiere removable por separado. Antes de publicar se comprueban 1023 bytes por ruta y el presupuesto común de 16 MiB. Cada referencia movable reserva la ruta original más NUL sin otra entrada. Las rutas, referencias, instantáneas y entradas de directorios iniciales siguen reservadas tras eliminarlos. PathCharge dinámico empieza en cero y cobra una vez la ruta actual de cada miembro enlazado o retenido. Solo la carga dinámica existente de un destino liberable inmediatamente aporta crédito; FD/CWD/hijos/mappings huérfanos retenidos no lo aportan. La recuperación final devuelve una vez. Los ancestros iniciales implícitos no añaden entradas al límite de 256; la recuperación de archivos ordinarios iniciales no cambia.

Ejemplo:

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/work","mutable":true,"movable":true}]}}
```

initial-directory-move verifica sin SDK movimiento, cursores compartidos/independientes, flags FD, CWD, mapping privado, sustitución y restauración de nombres. Los perfiles guest locales y el iOS físico requieren validación separada.

La restricción de SWAP inicial se refiere a los objetos raíz de origen y destino de la llamada. El intercambio de ancestros creados puede transportar descendientes iniciales ya movidos, conservando su estado y sus costes dinámicos. Las restricciones anteriores se aplican cuando faltan las declaraciones necesarias.

Tras la revisión independiente final en macOS ARM64, el control Darwin registra 1.264 pruebas: 796 pasan, 468 se omiten por backend no disponible, cero fallos. Se ejecutaron las 117 cargas ARM64 HVF obligatorias. El subconjunto de archivos pasa 342/342, incluidos 22 casos nuevos 4K/16K y tres controles de admisión. CreationPolicy conserva Device/GID del padre movido; reutilización del nombre, secuencia de inodes y umask actual permanecen separados. Con el límite exacto de 16 MiB, 16 intercambios de ida y vuelta no acumulan costes; volver libera solo la diferencia real de seis bytes. C/CLI público: 175/175; informes: 33/33; núcleo nativo: 29/29; sonda original independiente: 19 observaciones correctas. Las cinco configuraciones Python pasan en 27.865 segundos; API pura 71, deriva SDK y runners 49 también pasan. Los recuentos se solapan. Fuentes, binarios, intentos y resultados se guardan en build-hvf-arm64/initial-directory-move/ y se vinculan al commit. iOS físico, Intel HVF suspendido, SWAP de raíces iniciales, permisos/montajes/case, EOF compartido, Mach/hilos/dyld y frameworks siguen pendientes por separado.

## Intercambio atómico de raíces iniciales declaradas

El Boolean estricto exchangeable:true (C++ DarwinFileOptions::ExchangeableDirectories, al final del agregado) autoriza solo una raíz inicial explícita distinta de / como operando RENAME_SWAP. Su padre inicial directo debe ser mutable. Ausencia/false siguen excluidos; otros tipos son inválidos. Comparte con movable el subárbol ordinario sin montajes y con nombres únicos, y la validación flags/modos especiales/alias/enlaces físicos/dispositivos de todo el componente. La unión determina solo la topología. Cada referencia reserva ruta original+NUL por separado, incluso sobre la misma raíz, sin otra entrada. Igualdad de dispositivos no une dominios.

El origen inicial normal/EXCL todavía exige movable y el reemplazo inicial normal exige removable. exchangeable no concede estos derechos, mutable a descendientes, escritura ni permisos/montajes generales. Objetos distintos requieren ambos padres reales mutables y con swap_rename independiente. Un SWAP homónimo autorizado de componente normal comprueba padres/dispositivos y no cambia nada, sin primer cargo ni capacidad para objetos distintos. Dot/mayúsculas del mismo objeto siguen desconocidos; se conserva el orden de destino ausente y dot.

Raíces iniciales no vacías, iniciales/creadas y directorio/archivo se intercambian en ambos sentidos. Se prevalidan completos ambos árboles enlazados y retenidos, retirando todos los nombres antes de publicar. Ambas raíces siguen enlazadas: sin crédito de reemplazo/contenido ni FD/inode/entrada nueva. FD/dup/CWD/cursores, objetos padre, leases y derechos siguen sus objetos; descendientes intactos mantienen stat/instantáneas. Objetos borrados antiguos y nuevos homónimos permanecen separados. Costes dinámicos comienzan en cero, se cargan una vez y luego sustituyen los anteriores; los fijos quedan reservados. Un fallo de ruta/presupuesto conserva ambos estados.

La carga original sin SDK initial-directory-swap intercambia empty y data preexistentes y los restaura, verificando descendientes, mapping, CWD, cursores, flags y creación tras mover. La aceptación anterior f98068c07 es un resultado normal congelado separado; esta declaración es la única ampliación del SWAP de raíces iniciales.

```json
{"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/left","exchangeable":true},{"path":"/right","exchangeable":true}]}}
```


La validación Release en macOS ARM64 registra 1,303 casos Darwin: 823 aprobados, 480 omitidos por motores no disponibles y cero fallos; se ejecutaron los 120 casos ARM64 HVF obligatorios. Archivos 361/361 (16 nuevos casos 4K/16K y 3 admisiones), C/CLI 180/180, análisis de informes 34/34, programas originales del núcleo 30/30 y sonda independiente 35 observaciones pasan. Python cubre cinco configuraciones en 33.894 segundos; pasan 71 pruebas API puras, 49 unidades de inventario/referencia y las comprobaciones SDK, formato, capacidades, procedencia y documentación. Los recuentos se solapan.

Con capacidad exacta, el mismo nombre y 16 intercambios de ida y vuelta conservan objetos y cargos. Si hacen falta seis bytes y quedan cinco, ambos rechazos conservan árboles, cursores y presupuesto de creación. Las revisiones independientes del plan y del código final están aceptadas. Intentos, fuentes, binarios y resultados se congelan en `build-hvf-arm64/initial-directory-swap/` y se vinculan al commit. Un informe ejecutado con solapamiento se excluyó y repitió en serie; se sincronizaron los marcadores de traducción. No se ampliaron plazos ni relajaron controles negativos. Permisos, montajes/mayúsculas no declarados, mapas compartidos/EOF, relojes progresivos, Mach/hilos/dyld/frameworks siguen pendientes; iOS físico, Intel HVF suspendido y CI remota de fusión son validaciones distintas.

## Observaciones explícitas de límites de recursos, solo lectura

`getrlimit(194)` lee `DarwinSystemOptions::ResourceLimits` (`darwin_system.resource_limits`) en los cinco perfiles Darwin. Cada `DarwinResourceLimit`, con clave 0..8, contiene dos uint64 little-endian en offsets 0/8, 16 bytes en total. Se exige `0 <= current <= maximum <= 9223372036854775807`; cero es explícito e INT64_MAX significa infinito. No consulta al host ni altera presupuestos FD, VM, almacenamiento o ejecución. `setrlimit`, aplicación de límites, señales y planificación siguen pendientes.

JSON estricto permite hasta nueve claves únicas con exactamente `resource`, `current`, `maximum`, como enteros exactos o cadenas decimales sin signo. Tipos/campos inválidos, duplicados, claves no canónicas y límites invertidos fallan antes de cargar. Un array vacío u omitido sigue siendo desconocido; no se normalizan las claves de configuración.

Solo el syscall usa los 32 bits bajos del selector y elimina `_RLIMIT_POSIX_FLAG=0x1000`. Un recurso inválido devuelve EINVAL antes de acceder a memoria; una observación ausente se detiene como no soportada antes del acceso. Salida totalmente no escribible devuelve EFAULT; un par parcialmente escribible se rechaza sin publicar bytes. La copia completa desalineada o entre páginas escribe solo 16 bytes; los errores del backend siguen siendo de transporte. La sonda ARM64 nativa pasó 23 comprobaciones de valores/selectores y cuatro fallos separados; el prefijo intacto de este host no constituye una garantía portable.

```json
{"darwin_system":{"resource_limits":[{"resource":8,"current":256,"maximum":"9223372036854775807"}]}}
```

macOS ARM64 Release: registrados/aprobados/omitidos sin ejecución/fallos 1337 / 845 / 492 / 0, todos los 123 HVF requeridos ejecutados. Doce casos nuevos 4K/16K y oráculo SDK; C/CLI 190/190, parser 37/37, cargas nativas 31/31; Python con cinco perfiles en 71.978 segundos, 71 pruebas API y 49 del runner aprobadas. Pasan deriva SDK, formato, capacidades, procedencia y documentación; los conteos se solapan. Evidencias congeladas en `build-hvf-arm64/resource-limit-observations/` y vinculadas al commit. Se preservan el primer fallo de compilación por un enum de prueba y los intentos de conexión/filtro; la validación corregida es serial y conserva plazos/controles negativos. Siguen pendientes aplicación de límites, permisos, directorios tras mutaciones, mapas compartidos/EOF, relojes, Mach/hilos/dyld y frameworks. iOS físico, Intel HVF suspendido y CI de merge remoto requieren aceptación aparte.

## Observaciones explícitas de uso de recursos de solo lectura

getrusage(117) lee DarwinSystemOptions::ResourceUsageSelf / ResourceUsageChildren opcionales e independientes en cinco configuraciones Darwin. JSON estricto: darwin_system.resource_usage.self / .children. Cada DarwinResourceUsage requiere int64 user_seconds/system_seconds, uint32 user_microseconds/system_microseconds menores de1000000 y catorce counters int64. Enteros exactos o cadenas decimales con signo conservan todo el rango; tipos/campos/microsegundos/longitudes incorrectos fallan antes de cargar. El par ausente queda desconocido sin bloquear el declarado; cero explícito es válido.

Una copia completa little-endian de144 bytes: timeval en0/16 (segundos8, microsegundos4, padding cero4), counters desde32 por8. Se conservan valores/unidades Darwin, sin convertir ru_maxrss a KiB de Linux. Son valores fijos, sin rendimiento del host, contabilidad, fork/wait, planificación ni aplicación de límites.

Solo32 bits bajos del selector: 0=SELF, -1=CHILDREN; 0x1000 inválido y ninguna eliminación de flag POSIX. Inválido EINVAL antes de memoria; ausente unsupported antes de salida. Copia íntegra sin alineación/entre páginas conserva guardas; totalmente no escribible EFAULT, parcial unsupported antes de cualquier byte; errores backend siguen siendo de transporte. SDK captura una vez por selector, no compara SELF posteriores variables. Probe nativo13 controles y4 procesos de fallo independientes con límite5s sin cambios. Aquí partial SELF escribió64 bytes antes de EFAULT; no implica garantía portátil del prefijo.

0..13: `ru_maxrss`, `ru_ixrss`, `ru_idrss`, `ru_isrss`, `ru_minflt`, `ru_majflt`, `ru_nswap`, `ru_inblock`, `ru_oublock`, `ru_msgsnd`, `ru_msgrcv`, `ru_nsignals`, `ru_nvcsw`, `ru_nivcsw`.

```json
{"darwin_system":{"resource_usage":{"self":{"user_seconds":"-9223372036854775808","user_microseconds":999999,"system_seconds":0,"system_microseconds":0,"counters":["9223372036854775807",0,0,0,0,0,0,0,0,0,0,0,0,0]}}}}
```

[Apple getrusage](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getrusage.2.html), [XNU](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [SDK layout](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/resource.h).

macOS ARM64 Release registrado/aprobado/skip sin ejecutar/fallido 1371/867/504/0, 126 HVF obligatorios ejecutados. File361/361 y C/CLI 200/200, JSON 40/40, System/SDK 53/53, native 32/32 aprobados. Doce nuevos4K/16K, admisión/SDK; Python cinco configuraciones 42.865s, API71, runner/reference49 y SDK drift/formato/capacidades/procedencia/documentación aprobados. Recuentos superpuestos, revisión independiente aprobada. Evidencia congelada en `build-hvf-arm64/resource-usage-observations/` y vinculada al commit. Primera aserción x64 EINVAL corregida para conservar RDX, ARM64 borra X1; fallo/filtro vacío preservados. Runtime/plazos/controles negativos sin cambios. Límites, permisos, directorios tras mutación, shared maps/EOF, relojes, Mach/thread/dyld y frameworks incompletos; iOS físico, Intel HVF suspendido y CI de fusión separados.

Primer gate:866 aprobados, un timeout5s del rename iOS ARM64 HVF existente,504 skips. El mismo binario pasa ese caso en386ms y luego el gate completo serial pasa. Ambos preservados; causa no establecida, sin garantía de latencia.


## Credenciales explícitas, grupos y propietario coherente

Credentials opcional contiene RealUID/EffectiveUID/RealGID/EffectiveGID y GroupAccessList opcional independiente. Omisión mantiene cuatro getters1000; cero/root explícito válido, ID0..INT32_MAX. Grupos1..16, primero=EffectiveGID, orden/duplicados conservados; ausencia desconocida, sin inferencia host/EGID. darwin_system.credentials exige exactamente real_uid/effective_uid/real_gid/effective_gid, groups opcional. Enteros sin pérdida/validador central rechazan forma/campos/rango/cantidad/primero incoherente antes de cargar; perfiles noDarwin rechazados.

getuid24/geteuid25/getgid47/getegid43/getgroups79 comparten propietario system. Archivo regular nuevo usa UID efectivo, device/GID del padre directo; rename/FD retenidos/reuso nombre mantienen objeto, stat de entrada intacto. Root no concede escritura/mutación/ACL ; setuid/setgid/setgroups y proceso/sesión ausentes.

getgroups capacidad=low32 int firmado: negativo primeroEINVAL, desconocidounsupported, cero conocido=count sin puntero, positivo cortoEINVAL antes memoria; suficiente copia una vez4*count bytes little-endian. 0x1000 positivo, sin quitar POSIXflag. Guards completos noalineados/cruzando páginas; totalmente noescribibleEFAULT, parcialunsupported antes byte, backend sigue transporte. ErrorBSD conserva x64RDX/borra ARM64X1; éxito borra secundarios ambos, informe conserva argumentos brutos.


~~~json
{"darwin_system":{"credentials":{"real_uid":101,"effective_uid":202,
 "real_gid":303,"effective_gid":404,"groups":[404,0,"2147483647",7,7]}}}
~~~


[Apple getgroups contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getgroups.2.html), [pinned XNU credential/group ordering](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_prot.c).

registrados/aprobados/skips indisponibles/fallos: 1413/897/516/0; ARM64 HVF 129/129. System/SDK72/72, File365/365, Report43/43, C/CLI210/210; guest8/12skip; native33/33; Python5 82.918s; API71, runner/reference49. SDK drift / clang-format22.1.2 / capabilities23 / provenance / docs11+negative3: OK. Independent source review: OK.

21 value +5 native fault,5s; host16groups, positive short capacity: OK. Native partial32byte thenEFAULT: observation only. Counts overlap. `build-hvf-arm64/credential-observations/`.

Primera ejecución8 fallos (5 presupuestos instrucciones,3 plazos5s),12skips. Scan de dos páginas del fixture limitado a los132bytes completos alrededor del mismo cruce (64previos,hasta64datos,al menos4después); owner directo mantiene dos páginas completas. Presupuestos/argumentos/negativos intactos, fuentes/binarios/ambas ejecuciones conservados. Geometría transporte corregida antes de ejecutar según revisión ; selección pública compara contenido. Permisos/ACL,enlaces,observaciones directory tras mutar,shared maps/EOF,relojes,Mach/thread/dyld/framework incompletos ; iOS físico,IntelHVF suspendido,mergeCI separados.

## Tabla de descriptores a partir de límites declarados

`DarwinSystemOptions::MaxFilesPerProcess` / `darwin_system.max_files_per_process` declara un int opcional no negativo. Ausencia significa desconocido; cero explícito es válido. `kern.maxfilesperproc` y el MIB `[1,29]` leen los mismos cuatro bytes sin exigir límites de recursos. El parseo entero sin pérdidas y el validador central rechazan tipos, negativos y valores grandes antes de cargar; también se rechazan perfiles no Darwin.

BSD `getdtablesize(89)` exige este tope y `ResourceLimits[8].Current`, devolviendo el mínimo. Conserva los64 bits de Current antes de convertir a int: `0x100000001` con cap=64 devuelve64; infinito también se acota. Maximum, valores del host, número de FD activos y `DescriptorLimit` no sustituyen observaciones. Si falta una, sigue unsupported aunque la otra sea cero. Ignora los seis argumentos, no accede a memoria de usuario y conserva carry/registro secundario BSD. Mach timebase trap89 es independiente.

Las fases sysctl se mantienen. Escritura real con EUID0 para después de nombre/MIB y oldlenp, antes de observación/salida, como unsupported; no-root recibe EPERM. Puntero nuevo con longitud cero sigue siendo lectura. No se infieren aplicación de límites ni permisos. El workload obligatorio compara ambos cap y syscall numbers bajos/altos conservando `l` /144 bytes; un rechazo posterior preserva bytes ya emitidos. El fixture escalar verifica ausencias, cero, Current ancho e independencia de DescriptorLimit=3.

```json
{"darwin_system":{"max_files_per_process":64,"resource_limits":[{"resource":8,"current":"4294967297","maximum":"9223372036854775807"}]}}
```

[XNU getdtablesize](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c), [XNU proc_limitgetcur_nofile](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_resource.c), [XNU MIB constants](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/sysctl.h).

Verificación: Darwin (registrados/aprobados/no disponibles/fallidos) 1441/913/528/0; ARM64 HVF 132/132; System/SDK 80/80; File 365/365; C/CLI 211/211; ProcessReport 45/45; native 33/33; Python 5 (42.254s); API 71; runner 44 + reference 5. clang-format 22.1.2; capabilities/provenance; docs 286 / locales 10 / negative controls 3.

Historial: la primera comprobación del runner falló en los inventarios ARM64 y x86-64 porque faltaba el registro obligatorio del nuevo método escalar. Se añadió sin debilitar la igualdad de todos los métodos. Se conservan el primer control de 129 casos obligatorios y los fallos; el control final ARM64 exige 132.

Los conteos se solapan. `build-hvf-arm64/descriptor-table-observations/` conserva baseline real y hashes exactos de fuentes/binarios/logs; evidencia anterior inmutable. Cinco hijos desechables ARM64 macOS,40 controles, cinco segundos cada uno: Current=1048575/cap=245760 devuelve245760; hijos Current=0/1/32/245777 devuelven0/1/32/245760. Límites del padre y sistema intactos. iOS físico, Intel HVF suspendido y remote merge CI se validan por separado. Permisos, observación de directorios tras mutar, shared maps/EOF, relojes avanzados, Mach/thread/dyld y frameworks siguen incompletos.

## Observaciones explícitas del proceso

`DarwinSystemOptions::ProcessGroupID`, `SessionID` y `ProcessTainted` son entradas opcionales independientes: JSON `process_group_id`, `session_id`, `process_tainted`. Los ID deben ser positivos y como máximo INT32_MAX; el estado contaminado admite solo Boolean JSON `true`/`false`. La ausencia sigue siendo desconocida y `false` explícito es cero conocido. No se deduce ningún valor del host, PID1000, credenciales u otra observación.

La llamada cruda `getpgrp(81)` lee el grupo; `getpgid(151)` y `getsid(310)` usan los32 bits bajos con signo de `pid_t`, aceptando cero o el PID1000 actual fijo para el propio proceso. `0xffffffff000003e8` también identifica al propio proceso. Un PID bajo negativo devuelve ESRCH3 antes de buscar la observación, confirmado con sondas nativas de solo lectura y asignación/búsqueda de procesos XNU. Un proceso ajeno positivo desconocido, incluido `0x1000`, se detiene con UnsupportedService sin inventar ESRCH ni aplicar máscaras de banderas. La ausencia del valor propio seleccionado también detiene como no compatible. `getpgrp` e `issetugid(327)` ignoran todos los argumentos; las cuatro consultas escalares no acceden a memoria invitada. Se conservan BSD carry y las reglas del segundo registro de retorno.

`process_tainted` proporciona la observación fija `P_SUGID`, independiente de que coincidan los ID reales/efectivos. No cambia EUID, propiedad de archivos, autoridad de escritura sysctl, autorizaciones, liderazgo o estado del terminal. `setpgid`, `setsid` y la mutación de credenciales siguen sin soporte. La carga original de solo lectura `process-observations` captura su propio PID y comprueba bits altos, errores negativos y retornos; `virtual-process-observations` emite bytes configurados de grupo/sesión/contaminación. Los casos del modelo para otros procesos, valores ausentes y setters no entran en el inventario nativo. Las referencias macOS no validan dispositivos iOS físicos.

```json
{"darwin_system":{"process_group_id":7,"session_id":16909060,"process_tainted":false}}
```

[XNU process queries](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c), [XNU service numbers](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/syscalls.master), [XNU PID allocation](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_fork.c), [XNU PID lookup](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_proc.c).


## Búfer explícito de inicio de sesión

`DarwinSystemOptions::LoginNameBytes` declara de forma independiente los 255 bytes crudos de sesión (`MAXLOGNAME`). JSON `login_name_hex` exige exactamente 510 dígitos hexadecimales ASCII en ambas cajas. Se permiten NUL internos y bytes no nulos tras un terminador. Ausencia significa desconocido; un registro íntegramente cero es explícito. No se rellena un nombre corto ni se deduce desde anfitrión, credenciales, grupo, sesión o contaminación. La observación no concede permisos de acceso ni archivos.

`getlogin(49)` usa los 32 bits bajos sin signo de longitud (`u_int`) y copia exactamente min(length,255) bytes, sin decodificar, añadir NUL o devolver tamaño necesario. Longitud cero tiene éxito sin observación ni memoria invitada incluso con punteros inválidos; `0xffffffff00000000` selecciona cero. Una petición no nula exige el búfer completo antes de comprobar el destino. Un destino totalmente no escribible devuelve EFAULT14; rangos parciales se rechazan antes de copiar. Errores previos no publican bytes. Los errores de escritura del backend se propagan por la capa existente sin garantía general de reversión. Se mantienen BSD carry y segundo registro, incluido RDX x64 original ante error.

`setlogin(50)` sigue sin soporte con root explícito o registro cero. El programa original de solo lectura `login-buffer` comprueba longitudes completas, prefijos, bytes vecinos, punteros con longitud cero y EFAULT; `virtual-login-buffer` emite los 255 bytes declarados. Los casos ausentes y setters solo usan el modelo. La referencia macOS ARM64 no valida iOS físico ni Intel nativo. El ejemplo declara 255 ceros, sin inferir un nombre vacío.

El verificador huésped inicializa los 265 bytes de salida antes de cada copia y comprueba tres intervalos ascendentes y disjuntos: [0,3), [3,3+n), [3+n,265), donde n es la misma longitud de copia limitada. El primero y el último comprueban todos los bytes de guarda; el central compara cada byte copiado con la instantánea original. Se conservan las 12 longitudes completas, 21 llamadas de longitud cero, 42 llamadas EFAULT, comprobaciones de carry/registro secundario y rutas directas/virtuales bajo los límites existentes. Disminuye el trabajo del verificador conservando la cobertura y el orden del primer error; el rendimiento de producción requiere mediciones independientes.

```json
{"darwin_system":{"login_name_hex":"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}}
```

[XNU getlogin / MAXLOGNAME](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_prot.c#L1561).


## Prioridad explícita del proceso actual

`DarwinSystemOptions::ProcessNice` / JSON `darwin_system.nice` declara un nice fijo independiente con signo entre -20 y 20. Ausente significa desconocido; cero y -1 explícitos son conocidos. El analizador entero sin pérdida acepta números enteros y cadenas decimales enteras; rechaza tipos incorrectos, fracciones, cadenas exponenciales, espacios y valores fuera de rango antes de cargar la imagen. JSON numérico exactamente entero sigue válido. Los perfiles no Darwin rechazan el campo. No se deduce del host, credenciales, grupo/sesión, contaminación, login, recursos o CPU; no cambia planificación, permisos ni presupuestos.

`getpriority(100)` usa los32 bits bajos del selector int y destino `id_t` sin signo. Destino superior a INT32_MAX devuelve primero EINVAL22. Selectores desconocidos, incluidos GPU5/0x1000, y thread3 con destino bajo no nulo devuelven EINVAL antes de las observaciones. `PRIO_PROCESS`0 acepta cero o PID1000 actual y después exige nice. Otros PID positivos quedan sin soporte, sin inventar ESRCH. Grupo1, usuario2, thread3/destino0 y extensiones4,6,7,8 siguen sin soporte aun con observaciones relacionadas; destino thread solo con bits altos selecciona estado desconocido, no EINVAL. El resultado extiende signo a64 bits: -1 es UINT64_MAX exitoso con carry limpio. Sin memoria invitada, ignora argumentos no usados; BSD conserva RDX x64 en error y lo borra en éxito, ARM64 X1 se borra en ambas rutas.

`setpriority(96)` sigue sin soporte con root/nice explícitos. El original de solo lectura `process-priority` comprueba destinos propios, argumentos inválidos seguros y transiciones éxito/error/éxito; `virtual-process-priority` emite los ocho bytes con signo declarados. Otros procesos, agregados, ausencias y setters solo se prueban en el modelo. La nueva sonda ARM64 macOS pasó191 comprobaciones con nice0; ese valor no negativo no demuestra extensión negativa de hardware, cubierta por declaraciones con signo de la versión fijada y límites independientes del modelo. iOS físico e Intel nativo siguen separados.

```json
{"darwin_system":{"nice":-1}}
```

[XNU getpriority](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/kern/kern_resource.c), [XNU signed INT entry](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/dev/arm/systemcalls.c).


## Apertura sin seguir enlaces y comprobación del directorio

Las entradas normales y nocancel `open` / `openat` admiten O_NOFOLLOW=0x100 u O_NOFOLLOW_ANY=0x20000000 en los32 bits bajos sin signo. Estas opciones de búsqueda no aparecen en F_GETFL ni cambian acceso, adición, truncado, creación o CLOEXEC propio del FD. Combinarlas devuelve EINVAL22 después de comprobar capacidad FD, antes de importar la ruta completa; una tabla llena devuelve primero EMFILE24. Las opciones desconocidas siguen sin soporte.

Fuera de AT_FDCWD, `openat` importa exactamente el primer byte antes del modo de acceso y la capacidad FD. Si es inaccesible devuelve EFAULT14. Un prefijo relativo, incluso NUL, comprueba primero el objeto directorio retenido: FD desconocido EBADF9, archivo regular ENOTDIR20 y tipo vnode de flujo desconocido sin soporte. `/` omite dirfd; después la secuencia open existente importa la ruta completa. `open` normal y AT_FDCWD omiten esta fase, los demás servicios nameiat comprueban, tras opciones y tamaños, el primer byte y el dirfd relativo antes de importar la cadena completa. Un rechazo no consume FD ni inode nuevo; los errores de transporte se propagan sin modificar el espacio de nombres.

El `file-access` original usa FD relativos de directorio para NOFOLLOW_ANY y evita alias nativos `/var` o `/tmp`. Las pruebas directas distinguen primer byte y resto, barra/NUL, límites de usuario/página, FD agotados y directorios eliminados retenidos. La sonda bruta ARM64 macOS pasó30 casos con el mismo límite de cinco segundos; su última fila absoluta con tabla llena usa realmente un FD de directorio válido pese a su etiqueta antigua. iOS físico e Intel nativo requieren aceptación independiente.

[XNU open1at / open1](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU vn_open_auth](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_vnops.c), [XNU open flags / FMASK](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/sys/fcntl.h).

## Enlaces simbólicos iniciales fijos

Los nombres iniciales siguientes están protegidos por defecto; la última sección describe los permisos explícitos de modificación. `DarwinFileOptions::SymbolicLinks` / `darwin_files.symbolic_links` declaran `path` absoluto canónico, `target_hex` hexadecimal sin transformar y `metadata` opcional del propio enlace. Objetivos de1..1023 bytes noNUL conservan noUTF-8, barras repetidas y puntos; pueden faltar. `files` sigue obligatorio aunque vacío. Se rechazan colisiones y descendientes declarados bajo enlaces. Ruta/NUL/objetivo comparten256 entradas/16 MiB. Se requiere S_IFLNK, size=longitud, DT_LNK=10 e inode coherente; CWD debe ser directorio real.

Se expanden enlaces antes de puntos: objetivos relativos desde el padre real y absolutos desde la raíz invitada. Cada reinicio reinterpreta barras finales; las ya consumidas no pasan al objetivo. Hasta32 expansiones, la33 devuelveELOOP62; objetivo+sufijo+NUL sobre1024 bytes devuelveENAMETOOLONG63. FD/CWD/F_GETPATH/mmap conservan el objeto resuelto.

stat64/open/access/truncate/chdir siguen el enlace final; lstat64/readlink lo retienen. O_NOFOLLOW devuelveELOOP, combinado conO_DIRECTORY primeroENOTDIR20. O_NOFOLLOW_ANY rechaza expansión necesaria. O_CREAT|O_EXCL devuelveEEXIST17 para enlace final existente, incluso roto/cíclico. AT0x20/0x800 retienen el final;0x800 rechaza también expansiones intermedias/barras finales. Se combinan. AT_FDONLY ignora la ruta tras validar indicadores.

readlink(58) usa count firmado bajo32; readlinkat(473) size_t completo; ambos retornanint. SobreINT32_MAX:EINVAL22 antes de ruta/FD. Copia min(count,longitud), sinNUL, validando sólo ese prefijo. Longitud0 valida ruta/tipo y luego ignora salida. No enlace:EINVAL22; ningún byte escribible:EFAULT14; prefijo parcial: parada antes de copiar. Errores de transporte/presupuesto se propagan.

Los nombres y bytes de destino de los enlaces permanecen fijos. MutableDirectories no puede ser la raíz ni un ancestro por segmentos de un enlace fijo; /work no contiene /workspace/link. Dominios mutables separados pueden contener destinos creados, movidos, eliminados o sustituidos durante la ejecución. Se mantienen las comprobaciones de padres, montajes, alias, flags, soporte SWAP y creación; los nuevos inodos deben superar todos los de metadatos/instantáneas, incluidos los enlaces protegidos. WritableFiles/MutationPolicies fijos pueden modificar el archivo resuelto. Unlink/rename del enlace retenido paran antes de efectos. La creación durante la ejecución se describe abajo; enlaces duros,ACL y catálogos iniciales mutables siguen pendientes. Sonda ARM64 macOS:189 observaciones/115 buffers completos en los5s originales; no prueba iOS físico/Intel HVF/OS completo.

Los60 controles adicionales ARM64 macOS DELETE/RENAME conservan buffers stat completos, espacio de nombres antes/después e identidades FD/CWD dentro del límite original de5s. Las barras finales pueden expandir un enlace fijo y modificar su destino; NOFOLLOW_ANY rechaza la expansión necesaria con ELOOP. symbolic-link-mutations sin SDK comprueba creación, destinos ausentes, traslado/borrado/sustitución, padres CWD retenidos y los10 bytes originales del archivo antes de cerrar FD, además de los10 bytes del mapeo después. No acredita iOS físico ni Intel nativo.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"symbolic_links":[{"path":"/link","target_hex":"64617461"}],"working_directory":"/"}}
```

[XNU namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c), [XNU readlink / AT](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU open authorization](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_subr.c).

## Creación de enlaces simbólicos durante la ejecución

`symlink(57)` y `symlinkat(474)` crean enlaces locales al proceso en directorios mutables autorizados y devuelven int. Dirfd usa los32 bits bajos; un destino absoluto ignora el FD. La cadena objetivo se importa antes del destino hasta el primer NUL:0..1023 bytes opacos, vacíos, no UTF8, puntos y separadores repetidos.1024 bytes sin NUL dan ENAMETOOLONG63; un fallo anterior da EFAULT14. Los objetivos JSON iniciales siguen exigiendo1..1023 bytes.

La tabla actual posee nombre real, padre y bytes. Un terminal existente devuelve EEXIST17. Las barras finales consumidas pueden seguir un enlace colgante y crear en su objetivo, sin cambiar el enlace anterior. Expandir un objetivo vacío da ENOENT2. Readlink vacío devuelve0 sin acceder al puntero de salida incluso con capacidad positiva, tras verificar count/ruta/tipo.

Nombre/NUL y objetivo se cobran una vez en las256 entradas/16 MiB compartidos. Un rechazo no cambia nodo, padre, FD ni inodo de creación de archivo. Los metadatos completos nuevos son desconocidos; no heredan CreationPolicy de archivo ni observaciones de nombres reutilizados. Stat/instantánea del padre pasan a desconocidos tras crear. FD/CWD/mapas conservan objetos antiguos tras borrar/sustituir el objetivo. Rmdir y sustitución de directorios detectan hijos enlace. Mover/SWAP con enlaces iniciales protegidos en cualquiera de los lados que se mueve se detiene antes de efectos. sustituciones enlace/directorio, enlaces duros, ACL y catálogos iniciales mutables quedan pendientes; un alias no transfiere autoridad al padre real.

Los150 registros ARM64 macOS conservan cuatro fallos de observador; diez controles separados verifican el objeto nuevo real y límites vacíos. El programa sin SDK `symbolic-link-creation` verifica ambas entradas, bytes/buffers, padres, sustitución y los diez bytes completos de FD/mapas anteriores. No demuestra iOS físico, Intel nativo o compatibilidad completa.

[XNU symlink / symlinkat](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c), [XNU empty-link expansion](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_lookup.c).

## Eliminar enlaces simbólicos creados en ejecución

`unlink(10)` y `unlinkat(472)` eliminan enlaces creados en ejecución cuando el padre real autoriza cambios; los enlaces iniciales fijos siguen protegidos. `AT_SYMLINK_NOFOLLOW_ANY` por sí solo conserva el enlace final y permite eliminarlo, incluso con destino ausente, cíclico o vacío. Una expansión intermedia o por barra final devuelve ELOOP62. Sin esa opción, eliminar `a/` en `a → b → target` borra solo `b`, conservando `a` y el destino final. Se mantiene el orden de errores de opciones, ruta y dirfd.

El éxito devuelve una entrada dinámica y el coste actual de ruta/NUL/destino una sola vez, e invalida únicamente las observaciones completas stat/enumeración del padre real. No necesita FD libre ni inodo de creación. Datos, política de modificación, descripciones abiertas, cursores compartidos, CWD y mapas conservan sus objetos. Reutilizar el nombre no restaura el enlace; el rechazo no cambia estado ni presupuesto. Los metadatos completos de enlaces creados en ejecución siguen siendo desconocidos. sustituciones enlace/directorio y mover/SWAP subárboles que contienen enlaces iniciales protegidos siguen sin admitirse.

Las 40 observaciones ARM64 macOS independientes registran 28 eliminaciones, 12 errores, 17 ENOENT al repetir y conservación de FD/CWD/mapas privados con el mismo límite de cinco segundos. No certifican huéspedes, iOS físico ni Intel nativo. `symbolic-link-unlink` y los casos API públicos comprueban los límites por separado.

## Renombrar enlaces simbólicos creados en ejecución

`rename(128)`, `renameat(465)` y `renameatx_np(488)` admiten movimientos ordinarios y `RENAME_EXCL=4`: enlace a nombre libre, enlace/enlace, enlace/archivo y archivo/enlace. Ambos padres reales necesitan autorización en un dominio de montaje establecido; los enlaces iniciales siguen inmutables. El resolvedor compartido selecciona los nodos reales. Conserva destinos vacíos, ausentes, cíclicos y no UTF-8; un destino relativo se resuelve desde el nuevo padre. `RENAME_NOFOLLOW_ANY=16` sin más conserva el enlace terminal; la expansión intermedia necesaria devuelve ELOOP62. EXCL con destino existente distinto devuelve EEXIST17; EXCL del mismo objeto requiere un contrato de sensibilidad a mayúsculas que aún no se admite.

Antes de publicar nombres se reserva la nueva ruta/NUL, limitada a1024 bytes incluido NUL. Sustituir un enlace de ejecución devuelve una vez toda su ruta/NUL/destino actual, sin depender de los FD o mapas de su referente. El contenido/ruta dinámica del archivo sustituido mantiene el cargo hasta liberar todas las descripciones y reservas de mapas; solo un archivo inmediatamente recuperable aporta crédito. No requiere entrada adicional, FD ni inodo de creación de archivo. El rechazo conserva ambos nodos; el éxito invalida observaciones completas stat/enumeración de los padres reales. Los metadatos completos de enlaces de ejecución siguen desconocidos. sustitución enlace/directorio, enlaces duros y mover/SWAP árboles con enlaces iniciales protegidos siguen excluidos.

Las19 observaciones ARM64 macOS independientes registran14 éxitos y5 errores EEXIST/ELOOP con el límite original de5 segundos, identidad/bytes del enlace, nueva resolución relativa y FD/dup/cursores/CWD/mapas privados conservados. `symbolic-link-rename` sin SDK y las pruebas públicas SDK/CLI se realizan por separado. La referencia nativa no establece iOS físico, Intel nativo ni compatibilidad completa de SO.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Intercambiar enlaces simbólicos creados en ejecución

`renameatx_np(488)` admite enlace/enlace, enlace/archivo y archivo/enlace con `RENAME_SWAP=2` cuando ambos padres reales autorizan modificación y SWAP en un dominio de montaje establecido. El mismo nombre da éxito sin efectos ni declaración SWAP adicional. Un destino ausente devuelve ENOENT2; `SWAP|NOFOLLOW_ANY=18` conserva enlaces finales y rechaza expansión intermedia con ELOOP62; `SWAP|EXCL=6` devuelve EINVAL22 antes de leer rutas. Los bytes no cambian y los destinos relativos se resuelven desde ambos padres nuevos.

Se reservan ambas rutas/NUL antes de publicar nombres. Ambos objetos siguen enlazados, sin crédito de sustitución por datos o reservas de mapas. Un archivo inicial adquiere cargo dinámico en el primer intercambio y lo reutiliza al volver. No consume entrada, FD ni inodo de creación. Conserva identidad, nlink, descripciones, cursores, CWD y mapas; las observaciones completas de padres pasan a desconocidas. Metadatos completos de enlaces, pares directorio/enlace y mover/SWAP árboles con enlaces iniciales protegidos siguen excluidos. Los 22 controles ARM64 macOS registran 14 intercambios, dos éxitos del mismo objeto y seis errores con cinco segundos originales. `symbolic-link-rename` y SDK/CLI/Python verifican intercambios y errores, sin probar iOS físico, Intel nativo ni SO completo.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Mover árboles con enlaces creados en ejecución

Los movimientos ordinarios/EXCL y SWAP de directorios incluyen descendientes con enlaces de ejecución, también al intercambiar directorio/archivo. La transacción verifica todas las rutas de directorios, archivos y enlaces y el límite1024 bytes con NUL antes de extraer las tres tablas y publicar nombres. Los bytes de destino mantienen su cargo y contenido; solo cambia la ruta/NUL actual. SWAP no aporta crédito de sustitución ni consume nueva entrada, FD o inodo.

Los enlaces conservan sus padres reales al moverlos; los destinos relativos usan las rutas nuevas. Descripciones, cursores, CWD, nodos eliminados y reservas de mapas conservan sus objetos. Enlaces iniciales protegidos y sus árboles, pares raíz directorio/enlace, metadatos completos del enlace, enlaces duros y ACL siguen excluidos.25 controles ARM64 macOS registran cinco movimientos, diez intercambios, dos éxitos sin efecto y ocho rechazos sin cambiar nombres dentro de los cinco segundos originales. Los controles entre padres verifican bytes intactos y nueva resolución relativa; `symbolic-link-rename` cubre C++/SDK/CLI/Python sin probar iOS físico, Intel nativo ni SO completo.

[XNU rename / namei](https://github.com/apple-oss-distributions/xnu/blob/43a90889846e00bfb5cf1d255cdc0a701a1e05a4/bsd/vfs/vfs_syscalls.c).

## Metadatos de enlaces y directorios nuevos

La opción `creation_policy.namespace_policy` (C++ `DarwinFileCreationPolicy::Namespace`) añade a los cinco campos obligatorios un objeto estricto con `symbolic_link_allocation_unit`, `directory_entry_size` y `directory_blocks`. La unidad del enlace es potencia de dos entre512 y16 MiB; el tamaño de entrada es positivo hasta16 MiB; los bloques son uint64 hastaINT64_MAX y admiten cadenas decimales. Se mantienen metadatos parentales, umask inicial e inodes nuevos. Sin extensión, los metadatos desconocidos y el consumo de inode solo para archivos ordinarios de secciones anteriores siguen siendo el comportamiento predeterminado.

Las inserciones correctas de archivo,symlink,mkdir comparten una secuencia con agotamiento permanente enUINT64_MAX; errores y nombres existentes no consumen inodes. Device/GID proceden del padre real, UID de la identidad efectiva invitada. El enlace usa S_IFLNK con `0777 & ~umask`, nlink1, size de bytes brutos incluso vacíos/noUTF-8 y bloques512bytes redondeados a la unidad declarada. El directorio usa S_IFDIR con `mode & 0777 & ~umask`, nlink2 más todos los nombres directos enlazados, size=nlink por tamaño de entrada y blocks fijos, también para directorios vacíos retirados pero retenidos. Es un contrato virtual explícito, no una regla APFS inferida.

Los tiempos iniciales usan creation_time. Cambiar nombres hijos actualiza mtime/ctime del padre creado; mover directamente cambia solo ctime con mutation_time. Mover ancestros preserva descendientes. Los registros completos pertenecen al objeto durante dup/CWD/reemplazo/SWAP/borrado/reutilización. La extensión de creación por sí sola no conserva el stat completo inicial ni snapshots fijos; las políticas independientes siguientes ofrecen stat y enumeración actual. ACL, mutaciones de enlaces iniciales sin permiso individual y transacciones generales raíz directorio/enlace siguen pendientes. `created-namespace-metadata` verifica observaciones nativas comunes; `virtual-created-namespace-metadata` compara144bytes constantes completos porC++/SDK/CLI/Python. Pasaron11 modelos/admisiones,1JSON estricto,43 workloads nativos bajo los5segundos originales,8 invitados（12 omitidos por backend ausente,3HVF requeridos ejecutados）y10 casos públicos. Se conservan el fallo de tabla de punteros y la corrección estáticaARM64. iOS físico e Intel nativo siguen sin validar.

```json
{"namespace_policy":{"symbolic_link_allocation_unit":512,"directory_entry_size":32,"directory_blocks":7}}
```

## Enumeración virtual tras cambios de nombres

El campo opcional `directories[].enumeration_policy` habilita vistas actuales por `getdirentries 64`; C++ usa `DarwinFileOptions::DirectoryEnumerationPolicies` y `DarwinDirectoryEnumerationPolicy`. El objeto estricto exige `minimum_buffer_size`, `initial_minimum_buffer_size` y `seek_offset`: el primero positivo, ambos mínimos hasta 128 MiB y seek_offset uint 64 sin pérdidas. El directorio inicial requiere metadatos propios con inode no nulo; no admite `contents` inmutable simultáneo. Cada referencia cuenta ruta+NUL una vez. mkdir hereda la política del padre real; los descendientes existentes mantienen sus declaraciones. Enumerar y modificar son autorizaciones independientes.

El orden virtual es `.`, `..` y nombres directamente enlazados ordenados por bytes sin signo. Los inodes proceden de objetos observados o creados; `..` sigue el padre real retenido. Una identidad hija/padre ausente o nula detiene antes de escribir. Solo inode sobrevive a la invalidación del stat inicial completo. Cookies son ordinales locales desde 1 y d_seekoff una constante declarada. dup comparte cursor, open es independiente. Cambios confirmados y movimientos directos requieren rebobinar a cero; rechazos y operaciones sin efecto lo conservan. Mover un ancestro conserva cursores descendientes. Agotar versiones detiene explícitamente. Directorios vacíos eliminados y retenidos dan cero registros incluso tras reutilizar el nombre.

Este párrafo documenta la preparación con solo enumeración, sin la política stat inicial siguiente. Se reutilizan codificador, registros completos, mínimos, límite de carga, sufijo EOF y efectos datos/cursor/posición/flags. Stat completo del padre inicial e instantáneas fijas siguen invalidándose; omitir la política conserva los rechazos anteriores. No reproduce generaciones APFS. La preparación ARM 64 independiente registró 25 eventos/16 vistas dentro de los 5 segundos sin cambios. `directory-enumeration-mutations` comprueba identidades nativas y objetos retenidos; `virtual-directory-enumeration` compara 160 bytes literales por invitado, C/CLI y Python. Intel nativo, iOS físico, ACL, enlaces duros y OS/frameworks completos siguen sin validar o soportar.

## Mutación explícita del stat de directorios iniciales

La opción `directories[].mutation_policy` conserva el stat completo de un directorio inicial admitido tras cambios del espacio de nombres. C++ usa `DarwinDirectoryMutationPolicy` y `DarwinFileOptions::DirectoryMutationPolicies`. El objeto estricto contiene exactamente `directory_entry_size` y `mutation_time`. El tamaño es positivo y como máximo 16 MiB; el tiempo usa segundos de 64 bits con signo sin pérdida y nanosegundos en [0,1000000000). Se requieren metadata completas propias con inode no nulo. Cada referencia cuenta una vez ruta y NUL; el tamaño proyectado no asigna bytes de archivo. La política no otorga autoridad de modificación ni permisos, y no exige políticas de creación o enumeración.

El registro observado completo permanece intacto hasta el primer cambio realmente confirmado, que copia sus escalares al objeto sin asignación. Cambiar nombres hijos fija nlink en dos más todos los nombres directamente enlazados de cualquier tipo, size en nlink por directory_entry_size y mtime/ctime en mutation_time. Mover directamente, SWAP o borrar solo cambia ctime; mover un ancestro conserva descendientes. Rechazos y operaciones sin efecto sobre el mismo objeto no cambian nada. Device, inode, mode, propietario, blocks, tamaño de bloque, flags, generation, atime y birthtime conservan valores observados. Son reglas virtuales declaradas, sin inferir asignación, número de enlaces o reloj APFS.

El registro y la política siguen al objeto original por dup, FD retenidos, CWD, reemplazo, borrado y reutilización del nombre. Un nuevo mkdir usa la política de creación separada, si existe, sin heredar la política stat inicial del padre ni del objeto homónimo antiguo. Los snapshots inmutables siguen siendo desconocidos tras cambios; una enumeration_policy independiente puede ofrecer vistas actuales. Sin esta política stat, las metadata completas de directorios iniciales modificados siguen siendo desconocidas.

Una preparación nativa ARM64 independiente conservó 27 vistas stat brutas protegidas y 15 operaciones dentro de los cinco segundos sin modificar, verificando la ABI SDK de 144 bytes y la identidad retenida sin generalizar tiempos o asignación. El ensayo original `initial-directory-metadata` verifica observaciones nativas comunes; `virtual-initial-directory-metadata` compara un registro literal completo de 144 bytes por guest, C/CLI y Python. Los modelos también cubren primer borrado, ausencia de autoridad, omisión de creación e independencia snapshot/enumeración. Intel nativo, iOS físico, ACL, enlaces físicos, mutaciones de enlaces iniciales sin permiso individual y OS/frameworks completos siguen sin validar o sin soporte.

```json
{"mutation_policy":{"directory_entry_size":17,"mutation_time":{"seconds":-11,"nanoseconds":321}}}
```

## Mutación explícita de enlaces simbólicos iniciales

`symbolic_links[].mutable:true` y C++ `DarwinFileOptions::MutableSymbolicLinks` autorizan el nombre del objeto inicial, con permiso independiente del padre real. Se rechazan flags conocidos, mode especial, link_count≠1, alias de identidad y dispositivos contradictorios. Sin declaración el nombre queda protegido y no puede estar en un dominio ancestral mutable. Cada permiso reserva una referencia path/NUL fija, sin entrada ni inode de creación. Los destinos iniciales no cambian.

unlink, rename normal/EXCL, SWAP de hojas link/file/link y transacciones de subárboles declarados conservan requisitos de padre real, mount y SWAP. Destinos relativos se resuelven en el nuevo padre; FD/dup, CWD y leases de mapping conservan sus referentes. Costes iniciales siguen reservados tras borrar/reemplazar; el primer cambio reserva otro nombre dinámico. Solo se devuelven costes dinámicos propios y solo enlaces creados cuentan como entradas dinámicas. Rechazos y operaciones sin efecto no publican cambios.

`symbolic_links[].mutation_policy` usa `DarwinSymbolicLinkMutationPolicy` y `DarwinFileOptions::SymbolicLinkMutationPolicies`. El único campo estricto `mutation_time` tiene segundos signed 64-bit sin pérdida y nanosegundos [0, 1000000000), con permiso y metadatos completos de inode no cero. El primer movimiento directo/SWAP copia escalares sin asignar y cambia solo ctime; blocks y el resto se conservan. Ancestros/rechazos/no-ops preservan el registro. Sin política, stat completo pasa a desconocido, pero inode de enumeración y contradicciones de dispositivo permanecen. Referencia path/NUL fija; nombres reutilizados y nuevos symlink no heredan la política inicial, sino la namespace policy de creación separada.

La preparación ARM64 privada retiene 14 vistas guarded raw-stat, 11 operaciones, ABI SDK independiente de 144 bytes, límites compile120s/native5s/drain1s/reap1s y limpieza. `mutable-initial-links` comprueba identidad, resolución y referentes nativos; `virtual-mutable-initial-links` comprueba stat completo en cinco guest, C/CLI y Python. Modelos cubren dos páginas, SWAP de subárboles, costes exactos y agotamiento entry/inode. Intel e iOS físico no están validados; hard links, ACL, OS/runtime/framework completo quedan pendientes.

```json
{"mutable":true,"mutation_policy":{"mutation_time":{"seconds":-13,"nanoseconds":456}}}
```

## Transacciones raíz de directorio y enlace simbólico

`renameatx_np(RENAME_SWAP)` intercambia un directorio real y un enlace en ambos órdenes, incluso con subárboles no vacíos. El directorio inicial requiere `exchangeable`, el enlace inicial `mutable` y ambos padres reales permisos de modificación/SWAP dentro de un mount establecido. Los descendientes iniciales protegidos siguen rechazándose. Con permisos ordinarios existentes, directorio→enlace devuelve ENOTDIR20, el inverso EISDIR21 y EXCL sobre nombres distintos existentes EEXIST17. Ambos ciclos directorio/enlace descendiente devuelven EINVAL22 antes de cambios. Se intercambia el propio enlace: una autorreferencia resultante puede ser válida y seguirla después devuelve ELOOP62.

La transacción de tres tablas comprueba raíces, descendientes enlazados o retenidos, rutas completas y espacio dinámico antes de publicar. Nombres/objetivos/referencias iniciales, contenido y leases de mapping no dan crédito SWAP. La primera clave nueva reserva su ruta/NUL dinámica; los intercambios repetidos sustituyen su coste anterior una vez. No consumen entrada, FD ni inode de creación adicionales. La pertenencia sigue padres reales, sin incorporar objetos huérfanos antiguos por reutilizar nombres. Los objetivos crudos permanecen y la resolución relativa cambia de padre; FD/dup, cursores, CWD, referentes y mappings sobreviven. Las raíces directas aplican sus propias políticas stat solo a ctime; mover ancestros conserva descendientes. Sin política, stat completo queda desconocido, mientras el inode sigue disponible para enumeración viva y las versiones siguen el contrato de rebobinado existente. Sin campos JSON ni permisos nuevos.

La preparación ARM64 original conserva 36 vistas stat protegidas de 144 bytes, 16 raw rename, compile120s/native5s/drain1s/reap1s y recuperación/limpieza confirmadas. `directory-link-roots` y `virtual-directory-link-roots` comprueban identidad y stat completo constante en cinco guest, C/CLI y Python. Los modelos de dos tamaños de página cubren presupuestos exactos, desbordamientos sin abrir, huérfanos y agotamiento FD/entrada/inode. Esta sección amplía las exclusiones anteriores dentro de esos permisos. Intel nativo, iOS físico, hard links, ACL y OS/runtime/framework completo siguen pendientes.

## Consultas pathconf fijas del núcleo

pathconf(191) / fpathconf(192) admiten constantes vnode XNU:15/16/17→1,19/25→0,20/22/23→4096,21→65536,24→255. Las observaciones de enlaces, asignación, I/O y transferencia no habilitan ejecución asíncrona ni autorización; no se deducen de páginas o presupuestos del catálogo.

La resolución completa de ruta siguiendo enlaces o búsqueda del FD precede al selector low32. Se conservan CWD y EFAULT/ENOENT/ENOTDIR/ELOOP; FD ausente/cerrado devuelve EBADF. Los objetos archivo/directorio retenidos siguen consultables sin stat completo tras dup, movimiento, eliminación y reutilización del nombre. El tipo nativo de flujos capturados queda desconocido. Se mantiene BSD int/carry/registro secundario sin copiar salida, alterar cursor, metadatos/enumeración ni reservar entrada/FD/inode. NAME_MAX, propiedades de mayúsculas y selectores desconocidos siguen sin soporte tras búsqueda; no se usan valores del host ni EINVAL supuestos.

kernel-pathconf, kernel-pathconf-values y kernel-pathconf-unsupported verifican semántica común,80 bytes literales independientes y parada conservando salida previa en invitado/C/CLI/Python. Preparación privada ARM64:250 consultas,249 comparaciones SDK,0.262s con límite5s intacto. Intel nativo/iOS físico/ACL/enlaces duros/runtime y frameworks completos siguen sin validar o implementar.

[XNU vn_pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU pathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU fpathconf](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_descrip.c).

`pathconf`: `file/` → ENOTDIR; `alias/`, `alias//` → 1 (selector15); `alias/.`, `alias/child` → ENOTDIR.

## Listas de atributos comunes fijos

getattrlist(220), fgetattrlist(228) y getattrlistat(476) consultan once campos comunes del catálogo explícito: dispositivo, tipo, cuatro tiempos, propietario/grupo, modo completo, indicadores e ID. Comparten con stat64 la validez del registro completo; observaciones ausentes o invalidadas siguen desconocidas. Tipo y selección vacía no requieren stat. NAME de raíz/montaje, volumen, máscaras de directorio/archivo/fork, ACL y opciones desconocidas siguen sin soporte explícito incluso con máscara devuelta; no se infieren datos del host ni nombres de montaje.

Ruta/at importan24 bytes antes de buscar; FD valida primero low32 FD y tipo nativo. reserved se ignora. CWD, FD relativo y enlaces mantienen errores nativos antes de tamaño/bitmap. Formato little-endian, alineación4, st_mode completo y segundos con signo; con máscara120 bytes, sin ella100. Un búfer corto recibe sólo el prefijo solicitado, pero informa la longitud completa. Acceso parcial se detiene antes de esa copia; tamaño fuera de signed-uio produce EINVAL tras solicitud válida y soportada. No cambian cursores, metadatos, enumeración ni presupuestos entrada/FD/inode; se conserva la vida dup/eliminación/reuso de nombre.

Tres modos verifican comportamiento común, bytes configurados independientes y ATTR_CMN_EXTENDED_SECURITY sin soporte conservando salida por guest/C/CLI/Python. Preparación ARM64 privada:187 consultas raw/176 comparaciones SDK ruta-FD dentro de native5s sin cambios. SDK15.5 no declara getattrlistat; raw476 separado. Nuevos guest/Python:5,000,000us/quantum1024; públicos existentes:10s. Intel nativo, iOS físico, datos FS, enlaces duros, permisos/ACL y runtimes/frameworks completos siguen incompletos o sin validar.

```text
ATTR_CMN_RETURNED_ATTRS=0x80000000
FSOPT_NOFOLLOW=1, FSOPT_REPORT_FULLSIZE=4
FSOPT_PACK_INVAL_ATTRS=8 (requires ATTR_CMN_RETURNED_ATTRS)
FSOPT_NOFOLLOW_ANY=0x800
common-attributes
common-attributes-values
common-attributes-unsupported
```

[XNU attrlist](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_attrlist.c), [XNU attr.h](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h).


## Lectura explícita de atributos extendidos

getxattr(234), fgetxattr(235), listxattr(240) y flistxattr(241) leen observaciones ordinarias completas y ordenadas de darwin_files. Archivos, directorios y enlaces pueden declarar extended_attributes como una matriz estricta de {name,bytes_hex}. La omisión sigue siendo desconocida; [] declara una lista vacía conocida. Los nombres UTF-8 tienen 1..127 bytes y admiten barras; los valores son bytes opacos, se rechazan duplicados y se conserva el orden. Límite: 4096 atributos. Nombres, NUL y valores entran en el presupuesto existente de 16 MiB sin cobrar dos veces la ruta existente.

Las observaciones pertenecen al objeto tras dup, movimiento, eliminación, CWD y reutilización del nombre, independientemente del stat completo y la enumeración. Los nuevos objetos son desconocidos. Escrituras, truncado exitoso y fallos ambiguos de copia no vacía invalidan atributos; el rechazo previo de un búfer parcial los conserva. Las consultas conservan cursores, entradas, metadatos y presupuestos de objetos/FD/inode.

ABI: FD/options/position low32, size full64 y BSD user_ssize_t/carry/secondary. NULL ignora position. Para valores no vacíos, ruta nonNULL size0 devuelve ERANGE; FD size0 consulta la longitud. Solo UINT32_MAX/UINT64_MAX del get por ruta son consultas heredadas; FD limita a INT32_MAX. Las listas positivas cortas publican nombres completos antes de ERANGE; tamaños negativos full64 nonNULL dan ERANGE para listas no vacías. No se verificó una lista nativa vacía: ese caso negativo con vacío declarado sigue siendo UnsupportedService. La salida inaccesible se detiene antes de copiar. NOFOLLOW1 y NOFOLLOW_ANY64 son independientes;8/16 se rechazan antes de buscar, FD1/64 antes de FD/nombre. CREATE2/REPLACE4 se ignoran; SHOWCOMPRESSION32 y bits desconocidos no se admiten. com.apple.system.*, ResourceFork, FinderInfo, decmpfs, escritura/eliminación, permisos/ACL e inferencias del sistema de archivos quedan excluidos.

extended-attributes / extended-attributes-values / extended-attributes-unsupported comprueban el programa nativo compartido, bytes virtuales y parada desconocida conservando la salida. Sin cambios: guest/Python5,000,000us/quantum1024, API pública10s, nativo5s. Preparación privada ARM64:320 comparaciones raw/SDK con todos288 bytes de guarda y carry/secondary idénticos. com.apple.provenance automático es una observación, no un vacío predeterminado. Intel nativo/iOS físico siguen sin verificar; dyld, Mach IPC, Objective-C/Swift y frameworks completos siguen incompletos.

Sources: [XNU syscall ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU xattr calls](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU ordinary attributes](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/vfs/vfs_xattr.c), [XNU xattr definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Original code/probes; Apple implementation is not copied.

## Nombres de objetos acotados

ATTR_CMN_NAME=1 se admite mediante getattrlist220/fgetattrlist228/getattrlistat476 para objetos no raíz con nombre único en el catálogo explícito. El nombre final es UTF-8 válido de1..255 bytes. La ruta real compartida con F_GETPATH conserva la última grafía enlazada tras dup, CWD, movimientos, SWAP, eliminación y reutilización; los alias del llamador no la reemplazan. Nombre y tipo no requieren stat; los campos stat seleccionados necesitan observaciones completas válidas. Etiquetas raíz/montaje, nombres inválidos, alias de enlaces duros o mayúsculas, normalización y atributos de ruta completa siguen desconocidos.

attrreference_t ocupa8 bytes antes de los otros campos; attr_dataoffset es relativo a la referencia, attr_length incluye NUL y el área final se rellena a4 bytes. Las salidas cortas conservan longitud total y prefijos exactos, incluso UTF-8 parcial. attribute-names / attribute-names-values / attribute-names-unsupported comprueban comportamiento nativo, bytes independientes y parada en nombre raíz conservando salida por guest/C/CLI/Python. ARM64:601 consultas raw,453 comparaciones SDK del búfer protegido completo,384 prefijos. SDK15.5 no declara raw476. Native5s, guest/Python5,000,000us/quantum1024 y public10s sin cambios. Intel nativo, iOS físico y runtimes/frameworks completos siguen incompletos o sin validar.

## Atributos de directorio por lotes acotados

getattrlistbulk(461) requiere enumeration_policy.bulk_attributes=true explícito; omisión o false no concede permiso. Este contrato TYPE virtual devuelve nombres de hijos directos actuales en orden de bytes sin signo, sin entradas punto y con ordinales locales en lugar de cookies nativas. El permiso permanece en el objeto inicial tras dup, movimientos, SWAP, eliminación y reutilización del nombre; los directorios nuevos no lo heredan. minimum_buffer_size, initial_minimum_buffer_size y seek_offset solo se aplican a getdirentries64.

NAME|OBJTYPE|RETURNED_ATTRS (0x80000009) es obligatorio; los once campos comunes existentes necesitan observaciones seleccionadas válidas. Se admiten Options0/8. bulk ignora las dos palabras bitmap/reserved de16 bits separadamente de la validación attrlist normal. Un codificador comparte attrreference_t y validez stat64. Solo devuelve registros completos: relleno de8 bytes si cabe, o tamaño de4 bytes para el último grupo. Si no cabe el primero devuelve ERANGE sin cambiar salida ni cursor; salida necesaria parcialmente accesible detiene antes de copiar. Solo los bytes devueltos requieren memoria escribible.

dup comparte avance y open distintos son independientes. Un recorrido terminado con offset no nulo conserva EOF tras cambios del espacio de nombres e ignora tamaño/salida después de validar la solicitud. Un directorio inicialmente vacío en offset0 se vuelve a comprobar. lseek cero reinicia la iteración. Cambios antes de EOF, seek arbitrario no nulo y mezcla getdirentries64/bulk detienen explícitamente. NAME-only, entradas ERROR, instantáneas, ACL/permisos, orden del host y otras mask/options no están admitidos. bulk-attributes / bulk-attributes-values / bulk-attributes-unsupported comprueban comportamiento nativo común, bytes virtuales literales y selección desconocida conservando salida previa. La preparación privada ARM64 pasó728 comparaciones raw/SDK protegidas. native5s, guest/Python5,000,000us/quantum1024 y public10s no cambian. Intel nativo, iOS físico y entornos completos siguen sin verificar o incompletos.

Sources: [XNU bulk ABI](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/man/man2/getattrlistbulk.2), [XNU attribute definitions](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/attr.h). Original implementation and probes; no Apple implementation copied.

## Modificación explícitamente autorizada de atributos extendidos ordinarios

setxattr(236), fsetxattr(237), removexattr(238) y fremovexattr(239) requieren una autorización independiente sobre el objeto inicial: C++ `MutableExtendedAttributes`, booleano JSON estricto `mutable_extended_attributes=true`. Cada archivo, directorio o enlace autorizado debe declarar una lista completa de `extended_attributes` ordinarios, incluida una lista vacía conocida. Los permisos para escribir contenido o modificar el espacio de nombres no conceden esta autorización. Autorizaciones y valores siguen al objeto retenido durante dup, movimiento, eliminación y retención de mapeos. Los objetos creados y los nombres reutilizados comienzan con atributos desconocidos. Se excluyen los alias conocidos, indicadores de metadatos incompatibles, atributos del sistema protegidos, ResourceFork, FinderInfo y semántica de compresión.

La sustitución conserva la posición en la lista virtual, la eliminación quita la entrada y la creación la añade al final. Es un orden declarado dentro del proceso, sin inferir el orden APFS. Los bytes y recuentos iniciales y las referencias de rutas de autorización siguen reservados. El exceso en ejecución comparte el límite existente de 16 MiB/4096; eliminación, invalidación del contenido y liberación final recuperan solo ese exceso. Los fallos de capacidad, transporte o plazo no publican estado temporal. Una entrada necesaria totalmente ilegible devuelve EFAULT; una entrada parcialmente legible se detiene explícitamente antes de publicar. El éxito invalida el stat completo sin inventar tiempos, conservando identidad, miembros del directorio, versión/instantánea de enumeración y cursores.

La ABI utiliza low32 FD/options/position y full64 size. Las comprobaciones tempranas de opciones privilegiadas y de enlaces FD preceden a la importación del nombre, que precede a la búsqueda del objeto. set rechaza NULL con longitud no nula antes de una entrada VFS excesiva (E2BIG7); la búsqueda precede a validar nombre ordinario, position y conflictos. set importa el valor completo antes de EEXIST17 para CREATE existente o ENOATTR93 para REPLACE ausente. CREATE y REPLACE juntos devuelven EINVAL; la eliminación ignora ambos bits. El tamaño cero no lee el puntero del valor. Otros indicadores, permisos desconocidos y comportamiento no observado del proveedor se detienen explícitamente.

xattr-mutations / xattr-mutations-values / xattr-mutations-unsupported verifican controles nativos/invitados originales, bytes virtuales literales independientes y una detención por falta de autorización que conserva la salida previa. La preparación privada ARM64 comprobó726 llamadas raw/SDK, observaciones protegidas completas de544 bytes y páginas legibles completas. native5s/compile120s/drain1s/reap1s, guest/Python5,000,000us/quantum1024 y public10s no cambian. Intel nativo, iOS físico, dyld, Mach IPC, hilos/señales, runtimes Objective-C/Swift y frameworks completos siguen sin verificar o incompletos.

Referencias ABI primarias: [declaraciones de llamadas XNU](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/syscalls.master), [definiciones xattr](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/sys/xattr.h). Código y sondas originales; no se copió la implementación de Apple.

## Enlaces físicos Darwin acotados

link sigue el destino simbólico final; linkat flags=0 elige el propio enlace y AT_SYMLINK_FOLLOW su destino. Solo se admiten low32 0/0x40; otros bits bajos dan EINVAL antes de importar. La búsqueda fuente y EPERM de directorios preceden al destino; un destino existente da EEXIST. Se exige permiso de modificación del destino y el mismo dominio de montaje explícito. Alias iniciales y conflictos conocidos de dispositivo, modo o flags siguen excluidos.

Un alias consume entrada y ruta/NUL, sin nuevo inode. Bytes, permisos de atributos, validez de metadatos y reservas de mapping pertenecen al objeto compartido. Las políticas explícitas actualizan enlaces y ctime; sin ellas stat completo es desconocido. Cambiar atributos invalida stat; cambiar contenido invalida observaciones de atributos. Descriptores y últimos mappings retienen costes de nombres retirados; solo costes liberables de inmediato se descuentan. Los subárboles seleccionan identidad y padre exactos; alias externos permanecen y destinos relativos usan el padre seleccionado.

Tras varios nombres F_GETPATH/ATTR_CMN_NAME siguen sin soporte aunque quede uno o ninguno; no se afirma un modelo general de caché APFS. bulk NAME usa la entrada real; rename/SWAP del mismo objeto conserva ambos nombres. Casing EXCL, Intel HVF, iOS físico, ACL, mappings coherentes/señales EOF, dyld, Mach IPC, hilos y frameworks completos siguen pendientes. Este contrato amplía las exclusiones anteriores solo dentro de sus límites.

```text
link(9), linkat(471), AT_SYMLINK_FOLLOW=0x40
DarwinFiles, FileEntry, LinkEntry, NameIdentity, Contents, LinkNode
LinkedNames, HadMultipleNames, DetachedNames
F_GETPATH, ATTR_CMN_NAME, getattrlist(220), fgetattrlist(228), getattrlistat(476)
getattrlistbulk(461), O_SYMLINK
hard-links
hard-links-values
hard-links-name-unsupported
hard-links-attributes-unsupported
HardLink*, HardLinksShareObjectsAndRetainExplicitNameBoundary
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 guest cases / 20 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## Descriptores O_SYMLINK acotados

O_SYMLINK=0x00200000 conserva el objeto simbólico final, incluso roto o cíclico, con acceso de lectura, escritura o ambos. No concede escritura sobre el contenido del destino. O_CREAT sigue el destino; NOFOLLOW mantiene la prioridad ELOOP, la creación exclusiva EEXIST, y O_DIRECTORY devuelve ENOTDIR para el enlace retenido. Los componentes intermedios y barras finales usan el resolvedor y NOFOLLOW_ANY existentes. F_GETFL omite el bit de selección.

La descripción conserva LinkNode y NameIdentity reales. Dup comparte indicadores y cursor; los open independientes usan descripciones propias. Renombrar, eliminar, reutilizar el nombre o eliminar el padre no sustituye el objeto retenido. F_GETPATH/ATTR_CMN_NAME usan el nombre único seleccionado. El historial de varios nombres impide permanentemente inferir nombres vnode, incluso después de eliminarlos todos. Las reservas iniciales siguen fijas; nombres, destinos, entradas y crecimiento de atributos dinámicos esperan al último propietario real. La sustitución no puede acreditar un último alias todavía retenido.

La E/S nunca expone la cadena de destino como contenido. Tras las comprobaciones existentes de importación escalar/vectorial, acceso y cantidad, los desplazamientos negativos dan EINVAL. Leer en INT64_MAX devuelve cero; escribir da EFBIG. Los otros desplazamientos admitidos dan EPERM, incluida longitud cero, antes de APPEND o acceso a datos. Se conservan las reglas negativas tempranas de pwrite/pwritev. DATA/HOLE da ENXIO para posiciones no negativas, EINVAL para negativas, sin mover el cursor.

ftruncate no negativo sobre descripciones escribibles y open TRUNC admitido solo establecen WasWritten, sin cambiar destino, stat completo, xattrs, cursor, presupuesto o inode. Solo lectura o longitud negativa da EINVAL. F_SETFL admitido modifica APPEND|NONBLOCK antes de devolver ENOTTY25; dup observa el efecto, un open independiente no. Argumentos desconocidos detienen antes de efectos.

fpathconf fijo, fgetattrlist y permisos ordinarios FD-xattr declarados por separado operan sobre el enlace. FD de directorio relativo y fchdir dan ENOTDIR. truncate no restaura stat invalidado por cambios de atributos. mmap heredado private/shared alineado y no ejecutable llega a EINVAL por tipo simbólico, sin mapping ni concesión. Shared ordinario, indicadores desconocidos, protección ejecutable y demás límites existentes se conservan. Los18 controles nativos mmap cubren solo length16384, offset0 y protection1/2/3.

La preparación ARM64 original compara144 bytes stat protegidos en15 truncados, desplazamientos/cantidades extremos, seek disperso y efectos de F_SETFL fallido. El programa común sin SDK ejecuta O0/O1/O2 y compara la ruta real del FD padre. Las rutas virtuales comparan un literal stat/type independiente o rechazan explícitamente varios nombres conservando la salida. Intel HVF nativo, iOS físico, ACL/permisos, EOF/señales mapeadas, dyld, Mach IPC, hilos y entornos/frameworks completos siguen sin verificar o incompletos.

Fuente primaria: [límite mmap XNU correspondiente](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-11417.140.69/bsd/kern/kern_mman.c). Implementación y sondas originales, sin copiar implementación Apple.

```text
O_SYMLINK=0x00200000; ENOTTY=25
open O_RDONLY|O_SYMLINK|O_TRUNC: WasWritten only
LinkNode, NameIdentity, HadMultipleNames, DetachedNames
INT64_MAX read=0 / write=EFBIG; other admitted offsets=EPERM
SEEK_DATA/SEEK_HOLE nonnegative=ENXIO / negative=EINVAL
F_SETFL: APPEND|NONBLOCK effect before ENOTTY; dup shares / independent open separate
symbolic-descriptors
symbolic-descriptors-values
symbolic-descriptors-name-unsupported
SymbolicDescriptor*, SymbolicDescriptorsRetainObjectsAndNativeErrorOrder
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
34 model cases / 20 transport parameters / 15 public cases / 5 Python profiles
66 mandatory workloads per platform / ARM64 198 / Intel 132
```

## Estado no bloqueante acotado de descriptores

O_NONBLOCK=4 se admite para archivos ordinarios, directorios y O_SYMLINK y se conserva en F_GETFL. Dup comparte estado y cursor; open independientes conservan sus descripciones. Siguen las reglas de acceso, close-on-exec, WasWritten, metadatos, bytes y cursor. El stdin finito declarado conserva EOF y orden de errores de puntero; la entrada omitida sigue desconocida. La captura conserva fallos de copia y presupuesto compartido.

F_SETFL valida argumentos low32 antes de efectos, suma uno según la conversión nativa de flags open y modifica solo APPEND|NONBLOCK. High32 se ignora; acceso y WasWritten de entrada no conceden permiso ni inventan escritura. Los valores nativos literales3/7/11/15 seleccionan4/8/12/0. Las descripciones simbólicas cambian estado antes de ENOTTY25. Flags desconocidos como ASYNC0x40 detienen antes de efectos.

La preparación ARM64 macOS original contiene122 observaciones:16 solicitudes por combinación válida objeto/acceso, dup retenido, open independiente, limpieza, escrituras reales y CLOEXEC por FD. El programa común sin SDK compara en O0/O1/O2 bytes, estado, cursor y ABI BSD cruda carry/errno. Invitado, C/CLI y Python verifican también rechazo desconocido; los tres perfiles ARM64 HVF requieren ejecución real. No añade esperas de disponibilidad, tuberías, red, kqueue, señales asíncronas ni I/O del host. O_EVTONLY sigue sin modelar; Intel HVF nativo, iOS físico y el entorno completo siguen sin verificar o incompletos.

```text
O_NONBLOCK=4; F_SETFL raw low32 mask=0x1000f
requests3/7/11/15 -> APPEND|NONBLOCK status4/8/12/0
nonblocking-descriptors / nonblocking-flags-unsupported
Nonblocking*, NonblockingDescriptorsKeepNativeControlState
native5s / compile120s / drain1s / reap1s
guest/Python5,000,000us / quantum1024 / public10s
8 model cases / 20 transport parameters / 10 public cases / 5 Python profiles
67 mandatory workloads per platform / ARM64 201 / Intel 134
```

## Observaciones getentropy explícitas y finitas

BSD getentropy500 usa la cola ordenada `DarwinSystemOptions::EntropyReads`, con JSON `darwin_system.entropy_reads`. Cada cadena hexadecimal es no vacía y de longitud par: hasta256 registros de1..256 bytes. Son límites del modelo; el transporte JSON conserva65536 bytes. La omisión es desconocida; `[]` está explícitamente agotada. Las entradas nativas/JSON se validan antes de modificar imagen o backend; otros perfiles OS rechazan estas opciones.

Se comprueba primero toda la longitud64 bits: más de256 devuelve EINVAL22 sin acceso ni consumo; cero funciona con cualquier puntero sin datos. Las solicitudes no nulas admiten después el siguiente registro de longitud exacta. Datos ausentes, agotados o incompatibles detienen UnsupportedService antes de efectos, incluso con dirección inválida; es el orden de admisión de reproducción. Éxito o EFAULT14 totalmente inaccesible consume un registro. Los destinos parcialmente escribibles se rechazan antes de copiar o avanzar; los errores de transporte no avanzan. Un fallo posterior de los registros de retorno conserva efectos terminados. Cada ejecución reinicia el cursor aunque reutilice las opciones.

DarwinEntropy posee un cursor por ejecución y bytes inmutables. BSD y returnService existentes gestionan ambas ISA, acarreo y registros secundarios. El programa sin SDK cubre5 perfiles invitados y3 ARM64 HVF; los bytes fijos no entran en el inventario RNG nativo determinista. Las sondas ARM64 O0/O1/O2 cubren594 llamadas; cambios del centinela no indican longitud exacta copiada. No se ofrece RNG del host, calidad criptográfica, /dev/random, importaciones libc ni frameworks. Intel HVF, iOS físico y compatibilidad completa siguen sin verificar o incompletos.

```text
BSD getentropy500 / DarwinEntropy / EntropyReads / darwin_system.entropy_reads
full64 size>256 -> EINVAL22; zero ->0; whole EFAULT14 consumes one record
1..256 bytes per record / at most256 records / JSON transport65536 bytes
entropy-replay / entropy-missing / entropy-exhausted / entropy-mismatch / entropy-partial
15 model cases / 20 transport parameters / 26 public cases / 5 Python profiles
68 mandatory workloads per platform / ARM64 204 / Intel 136
native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU getentropy ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [XNU generation/copyout boundary](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/dev/random/randomdev.c).

## Identidad explícita del hilo actual

BSD thread_selfid372 lee la observación inmutable opcional `DarwinSystemOptions::ThreadID` / JSON `darwin_system.thread_id`. Todo patrón uint64, incluido cero, es conocido; omitirlo detiene con UnsupportedService. Las cadenas decimales conservan64 bits y los números JSON deben ser enteros exactos hasta2^53-1. No se infiere un ID del host, PID o puerto Mach. La llamada sin argumentos ignora los seis portadores y no accede a memoria. La resolución low32 existente conserva el número bruto completo en eventos; el retorno BSD mantiene64 bits y limpia carry y RDX/X1. Las formas Mach siguen sin soporte.

Las ejecuciones repetidas conservan la observación y las opciones diferentes son independientes. No asigna IDs, garantiza unicidad ni crea identidades de eventos del planificador; tampoco implementa ciclos de vida de hilos, pthread, TLS o Mach IPC. Las sondas ARM64 originales O0/O1/O2 conservan24 llamadas comparadas con el SDK pthread actual, argumentos arbitrarios y bits altos del número. El programa nativo común solo compara relaciones dentro del mismo proceso; los bytes de ID declarados se excluyen del inventario nativo determinista. Intel HVF, iOS físico y la compatibilidad completa permanecen sin verificar o incompletos.

```text
BSD thread_selfid372 / Wide / ThreadID / darwin_system.thread_id
known uint64 including0 / missing -> UnsupportedService / no memory
full64 return / low32 resolution / carry clear / RDX-X1 zero / raw event number
thread-identity / thread-identity-value / thread-identity-missing
4 model cases / 20 transport parameters / 16 public cases / 5 Python profiles
69 mandatory workloads per platform / ARM64 207 / Intel 138 unverified
original ARM64 O0/O1/O2 probes24 / native5s / compile120s / drain1s / reap1s
owner/build1200s / guest/Python5,000,000us / quantum1024 / public10s
```

[XNU thread_selfid ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/syscalls.master), [libpthread current-thread owner](https://github.com/apple-oss-distributions/libpthread/blob/42d026df5b07825070f60134b980a1ec2552dfee/kern/kern_support.c).
