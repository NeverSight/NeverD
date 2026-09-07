**Idiomas**: [English](../../ATTRIBUTION.md) | [简体中文](ATTRIBUTION.zh-CN.md) | [繁體中文](ATTRIBUTION.zh-TW.md) | [日本語](ATTRIBUTION.ja.md) | [한국어](ATTRIBUTION.ko.md) | [Français](ATTRIBUTION.fr.md) | [Deutsch](ATTRIBUTION.de.md) | [Español](ATTRIBUTION.es.md) | [Italiano](ATTRIBUTION.it.md) | [Русский](ATTRIBUTION.ru.md) | [العربية](ATTRIBUTION.ar.md)

# Atribución y citas

Esta es una traducción de la [guía en inglés](../../ATTRIBUTION.md); prevalecen los términos de [LICENSE](../../LICENSE).

NeverD es desarrollado por **los colaboradores de NeverD**. Su repositorio de código fuente es
[NeverSight/NeverD](https://github.com/NeverSight/NeverD).

## Obligaciones de la licencia al reutilizar código

El material original de NeverD se publica bajo la
[GNU AGPL, solo versión 3](../../LICENSE). Al transmitir copias o
adaptaciones sujetas a esta licencia, conserve los avisos aplicables de derechos de autor,
licencia y garantía, incluido el aviso del proyecto en [NOTICE](../../NOTICE).
Conserve también los avisos de los autores individuales. El código fuente modificado
sujeto a la licencia debe incluir avisos destacados que identifiquen los cambios y su fecha correspondiente.

Estas obligaciones se aplican al material sujeto a la licencia que se reutilice manualmente,
se copie o adapte con un asistente de IA o un modelo de lenguaje de gran tamaño (LLM),
o se transforme mediante LLVM IR, compilación, descompilación u otro lenguaje de programación.
Cambiar nombres, formato, lenguaje o herramientas no elimina por sí mismo
las obligaciones. Identifique a NeverD como la fuente del material de NeverD reutilizado;
mencionar únicamente un modelo de IA o LLVM no identifica esa fuente.

Incluya la licencia completa y los avisos aplicables en las distribuciones de código fuente y
en el código fuente correspondiente a los binarios distribuidos. Para los binarios y los servicios
de red, cumpla también las disposiciones aplicables de las secciones 6 y 13 de la AGPL.
Una cita, un enlace o un agradecimiento por sí solo **no** sustituye los requisitos de la AGPL
relativos a la licencia, los avisos de modificación o la disponibilidad del código fuente.

Esta guía explica la licencia existente; no añade restricciones ni
términos adicionales conforme a la sección 7. Los términos que rigen figuran en
[LICENSE](../../LICENSE), en particular en las secciones 0, 2, 4–6 y 13, y también están disponibles
en la [Free Software Foundation](https://www.gnu.org/licenses/agpl-3.0.html).

## Hacer que la fuente sea rastreable

Para cada parte reutilizada, recomendamos registrar el archivo o símbolo original,
el commit o la versión exactos y una breve descripción de sus cambios
junto al código o en los avisos de su proyecto. Utilice un enlace permanente de GitHub con el
hash completo del commit para que la cita siga identificando la misma fuente.
Este detalle adicional sobre la procedencia es una recomendación de citación, no una
condición adicional de la licencia.

Por ejemplo, sustituya los campos entre corchetes por los datos reales de la fuente:

```text
This project includes material from NeverD.
Copyright (C) 2026 NeverD contributors (https://github.com/NeverSight/NeverD)
License: GNU Affero General Public License, version 3 only (AGPL-3.0-only).
Original source: https://github.com/NeverSight/NeverD/blob/[full-commit]/[path]
Changes: [description], [YYYY-MM-DD].
The applicable license and notices are included with this distribution.
```

En los flujos de trabajo asistidos por IA, conserve esta información de procedencia con el contexto
del código fuente seleccionado y trasládela a cualquier código sujeto a la licencia que publique.
Revise el código resultante y sus avisos antes de compartirlo. En el caso de conjuntos de datos
que contengan código fuente de NeverD sujeto a la licencia, conserve los avisos aplicables y la
información de licencia al transmitir ese código fuente.

## Investigación, referencias y resultados

Por favor, cite NeverD en los artículos, la documentación, las evaluaciones comparativas y los proyectos
que lo utilicen o se basen en su implementación. [CITATION.cff](../../CITATION.cff) proporciona
metadatos de citación de software legibles por máquina, y esta cita en texto sin formato
puede utilizarse con la versión o el commit que haya usado realmente:

```text
NeverD contributors. NeverD: Binary analysis and decompilation engine.
https://github.com/NeverSight/NeverD. Version or commit: [revision used].
```

El mero estudio de una idea o un algoritmo no somete automáticamente una implementación
independiente a la licencia de NeverD. Del mismo modo, ejecutar NeverD sobre un programa ajeno
no hace que el resultado quede automáticamente sujeto a la AGPL:
según la sección 2, el resultado solo está sujeto a la licencia si su contenido constituye una obra
cubierta por ella. La misma distinción se aplica a los resultados de la IA; entrenar con NeverD o
leer su código no convierte automáticamente cada resultado de un modelo en una obra cubierta.
Para estos usos sin material sujeto a la licencia, se solicita la cita como práctica académica
y de ingeniería, en lugar de imponerla como una nueva condición de la licencia.

## Material de terceros y copias anteriores

Los componentes como LLVM, Capstone y Unicorn conservan sus propias licencias.
Consulte sus avisos en el código fuente, [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)
y cualquier licencia específica del directorio, incluida la
[licencia del corpus de pruebas](https://github.com/NeverSight/testbins/blob/9d9362d2cdfe0b4b0347bd0e12b3b8fac67d3a5b/LICENSE).
Conserve los créditos originales de terceros y cumpla esas licencias
al reutilizar dicho material. Esta guía no cambia la licencia del material de terceros
ni revoca los permisos concedidos previamente para copias anteriores.
