**Idiomas**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md) | [繁體中文](CONTRIBUTING.zh-TW.md) | [日本語](CONTRIBUTING.ja.md) | [한국어](CONTRIBUTING.ko.md) | [Français](CONTRIBUTING.fr.md) | [Deutsch](CONTRIBUTING.de.md) | [Español](CONTRIBUTING.es.md) | [Italiano](CONTRIBUTING.it.md) | [Русский](CONTRIBUTING.ru.md) | [العربية](CONTRIBUTING.ar.md)

# Contribuir a NeverD

NeverD es un proyecto de análisis binario que prioriza la semántica. Una
contribución útil tiene un alcance concreto, hace que el comportamiento no
soportado falle de forma visible e incluye la prueba mínima que demuestra el
contrato modificado.

Antes de editar, lea la [guía de arquitectura](../architecture.es.md). Use la
[guía de pruebas](../testing.es.md) para elegir suites y la
[hoja de ruta](../roadmap/README.es.md) para consultar el trabajo de producto
planificado.

## Requisitos previos

- Git con soporte para submódulos recursivos
- CMake 3.20 o posterior
- Ninja
- Un compilador C++20
- Clang y LLD (`ld.lld` y `lld-link`) para el conjunto completo de fixtures
  entre objetivos

Los submódulos recursivos proporcionan los forks de LLVM y Capstone de NeverD,
Unicorn y los datos de firmas. No los sustituya por revisiones arbitrarias del
sistema al validar un cambio.

## Clonar e inicializar

El desarrollo se integra en `dev`, que también es la rama predeterminada del
repositorio. Clónela con todos los submódulos:

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

En un clon existente, sincronice los submódulos antes de la primera compilación
y después de cualquier commit que cambie sus revisiones registradas:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

## Elegir un perfil de compilación

| Perfil | Uso | Comportamiento importante |
|--------|-----|--------------------------|
| Release | Desarrollo normal, pruebas completas, benchmarks de decodificación/lift | Optimizado; rendimiento representativo |
| RelWithDebInfo | Perfilado o depuración de rutas críticas optimizadas | Optimizado con símbolos de depuración |
| Debug | Aserciones, ejecución paso a paso, corrección local | Sin optimizar; benchmarks de decodificación deliberadamente mucho más lentos |

Use Release salvo que la tarea necesite específicamente el comportamiento de
Debug:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel
```

De forma predeterminada, la compilación construye `third_party/llvm-project`
como dependencia integrada. La primera compilación suele tardar entre 30 y 60
minutos; las siguientes son incrementales. `CMakePresets.json` también define
los presets de configuración/compilación `release`, `relwithdebinfo` y `debug`,
pero arriba se usan directorios explícitos para dejar visible la activación de
las pruebas.

Para depurar a nivel de código fuente, use un directorio separado en vez de
reconfigurar el árbol Release:

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build-debug --parallel
```

Nunca publique el rendimiento de decodificación o lift obtenido en una
compilación Debug. Use Release para benchmarks, o RelWithDebInfo cuando el
perfilado necesite símbolos.

### LLVM precompilado en macOS

Quienes contribuyan desde Apple Silicon pueden evitar compilar localmente el
fork de LLVM:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

CMake descarga el paquete de versión configurado por el repositorio, verifica
su suma SHA-256 y reutiliza la caché de usuario extraída en compilaciones
posteriores. El canal precompilado solo admite macOS arm64. Los Mac Intel y las
compilaciones universales deben usar la compilación LLVM local predeterminada.
Las opciones avanzadas, como `NEVERD_LLVM_PREBUILT_TAG`, la URL de espejo, el
directorio de caché y una suma explícita, están documentadas en
`cmake/NeverDLLVMPrebuilt.cmake`.

## Flujo de ramas y pull requests

Parta de un `dev` actualizado y cree una rama temática concreta:

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

Abra pull requests contra `dev`, no contra una supuesta rama de versión.
Mantenga los commits fáciles de revisar: un propósito coherente, sin salida de
compilación generada, sin formateo ajeno al cambio y sin revisiones de
submódulos modificadas salvo que formen parte de la propuesta.

## Estilo de código

C y C++ siguen las convenciones de LLVM, con `.clang-format` como autoridad de
formato del repositorio. Formatee solo los archivos modificados:

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

No reformatee todo el repositorio para un arreglo específico. Siga los patrones
de nombres y descomposición cercanos, mantenga el comportamiento dependiente de
plataforma en el límite loader/lifter/backend correspondiente y no exponga
tipos C++ internos mediante el SDK C puro.

El Markdown debe ser conciso y verificable desde el código fuente. Use enlaces
relativos para los archivos del repositorio y actualice la documentación en el
mismo pull request cuando cambien el comportamiento del CLI, las API públicas,
las afirmaciones de soporte, las opciones de compilación o los comandos de
prueba.

## Ejecutar pruebas

Ejecute todas las pruebas registradas mediante el objetivo agregado:

```bash
cmake --build build-release --target check-neverd
```

Durante el desarrollo, use el objetivo pertinente más pequeño o una etiqueta
CTest:

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

La [guía de pruebas](../testing.es.md) documenta todos los objetivos de
conveniencia, las suites de transformación que solo tienen etiqueta, las
expresiones regulares para una sola prueba, la compilación de fixtures y los
recorridos de ida y vuelta con Unicorn. Si se omite un objetivo por faltar un
compilador cruzado o linker, informe de esa limitación; no describa la ruta no
ejecutada como aprobada.

## Lista de control del pull request

Antes de solicitar una revisión:

- Rebase o fusione el `dev` más reciente según el flujo preferido de los
  mantenedores y resuelva deliberadamente los cambios de submódulos.
- Compile los objetivos afectados en Release, o explique por qué hace falta
  otro perfil.
- Ejecute las pruebas de regresión precisas y la suite pertinente más amplia
  que resulte práctica; incluya los comandos exactos y todos los skips en la
  descripción del PR.
- Conserve el lifting estricto: una instrucción no soportada no debe convertirse
  silenciosamente en una operación inferida ni en un `NOP`.
- Añada cobertura semántica para los cambios de comportamiento, no solo
  instantáneas textuales de IR.
- Mantenga fuera del diff la limpieza ajena, los archivos generados y los
  artefactos de compilación locales.
- Actualice la documentación pública y para contribuidores cuando cambien el
  comportamiento, el soporte, las opciones, los comandos o la propiedad de las
  pruebas.

Para informes sensibles de seguridad que no deban comenzar como un pull request
público, siga [SECURITY.md](../../SECURITY.md).
