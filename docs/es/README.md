**Idiomas**: [English](../README.md) | [简体中文](../zh-CN/README.md) | [繁體中文](../zh-TW/README.md) | [日本語](../ja/README.md) | [한국어](../ko/README.md) | [Français](../fr/README.md) | [Deutsch](../de/README.md) | [Español](README.md) | [Italiano](../it/README.md) | [Русский](../ru/README.md) | [العربية](../ar/README.md)

[← Proyecto NeverD](project.md)

# Documentación de NeverD

La visión general, la compilación y el CLI están en el README del repositorio. Las referencias de diseño y pruebas para contribuidores se agrupan aquí.

NeverD admite Android e iOS mediante la CLI experimental `neverd mobile`: APK (incluido multidex), DEX y archivos o directorios smali a Java con un informe JSON; IPA, `.app` y Mach-O (arm64/x86_64) a C nativo y código Objective-C/Swift compatible con un informe de cobertura JSON. El alcance depende de los patrones de código admitidos; las guías detallan los límites.

| Documento | Descripción |
|-----------|-------------|
| [README (español)](project.md) | Resumen, inicio rápido, compilación, SDK, CLI |
| [Contribución](CONTRIBUTING.md) | Entorno, perfiles de compilación, flujo, estilo y requisitos de PR |
| [Arquitectura](architecture.md) | Rutas IR, límites de componentes, lifting estricto, profundidad de soporte y puntos de edición |
| [Pruebas](testing.md) | Suites, fixtures generadas, recorridos Unicorn y comandos incrementales |
| [Reconstrucción de excepciones de Windows](windows-exception-reconstruction.md) | Matriz de soporte SEH/C++, contrato IR, reglas de patch nativo y validación PE |
| [Auditoría y caza de seguridad de memoria](memory-safety.md) | Análisis de vida del montón y desbordamiento de copia: contrato de identidad por formato, catálogo de sumideros/fuentes, veredictos, presupuestos y esquema JSON |
| [Plugins nativos](plugins.md) | ABI de descriptor en C puro, callbacks y eventos, flujo de compilación/enlace, descubrimiento y reglas de compatibilidad |
| [Plugins de Python](python-plugins.md) | Autoría, API de sesión y eventos, aislamiento, pruebas y publicación |
| [Descompilación EVM](evm.md) | Entradas, hardforks, IR por fases, ABI host C/LLVM, reconstrucción Solidity y límites |
| [Descompilación de Solana SBF](sbf.md) | SBF v0-v4, LLVM IR, salida C/Rust, verificación y límites conocidos |
| [Resumen de mobile (English)](../mobile.md) | CLI experimental de Android/iOS, entradas, salidas, informes y límites |
| [Recuperación de Java para Android](android.md) | APK (incluido multidex), DEX, archivos/directorios smali → Java; motor nativo integrado en C++20, JADX opcional, CLI, informes JSON, límites y verificación |
| [Recuperación de fuentes iOS](ios.md) | IPA/.app/Mach-O (arm64/x86_64) → C nativo y código Objective-C/Swift compatible; disposiciones, CLI/export, cobertura JSON, límites y pruebas ejecutables |
| [Hoja de ruta](roadmap.md) | Estado: formatos nativos, EVM y Solana SBF implementados |
| [English README](../../README.md) | Versión en inglés |
| [Otros idiomas](../README.md) | Resto de versiones localizadas |
