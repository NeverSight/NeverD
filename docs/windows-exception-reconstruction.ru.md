**Языки**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# Реконструкция исключений Windows

[← Индекс документации](README.ru.md)

NeverD переносит табличные сведения об исключениях Windows через загрузку,
подъём, декомпиляцию и двоичное переписывание. Эти metadata входят в исполняемый
контракт функции: переписывание отклоняется, если нельзя доказать согласованность
созданного кода, records runtime-function, языковых таблиц и защитных таблиц.

Различаются три уровня поддержки:

- **Анализ**: нативное представление декодируется в проверенные нормализованные
  records и передаётся IR pipeline.
- **Декомпиляция**: сводимые защищённые области становятся явными узлами HighIR;
  остальные сохраняют детерминированные нативные аннотации.
- **Нативная реконструкция**: patch mode может поручить LLVM создать полный
  замещающий контракт исключений и установить его в конечный PE.

Поддержка анализа не означает поддержку нативной реконструкции.

## Матрица поддержки

| Нативная форма | Lift и анализ | Высокоуровневый вывод | Patch mode |
|----------------|---------------|-----------------------|------------|
| unwind x64 v1/v2 | Полные проверенные records, операции, цепочки, handler data и provenance | Сводка frame/unwind и структурированные языковые области, когда возможно | Полные primary records; созданные `.pdata`/`.xdata` заменяют устаревшее замыкание |
| unwind x64 v3/APX | Отдельные payload v3, эпилоги и учёт операций | Явные аннотации v3 | Только анализ; затронутая функция отклоняется |
| unwind ARM32/ARM64 packed | Диапазоны, packed-поля и identity primary/fragment | Сводка frame/unwind | Только полный primary record без языкового handler и отдельно адресуемых fragments |
| unwind ARM32/ARM64 unpacked | Проверенные header/extent xdata, связь handler и fragments | Сводка frame/unwind | Только полный primary record без языкового handler и отдельно адресуемых fragments |
| `__C_specific_handler` | Scopes, filters, цели finally, handlers и continuations | `__try`/`__except`/`__finally` для сводимых областей, иначе аннотация | Нативная x64-реконструкция полных представимых scope-графов |
| `__CxxFrameHandler3` | Unwind/try maps, catches, offsets object/frame, continuations и IP-to-state | Сводимые интервалы как C++ HighIR с C-совместимыми type annotations | Нативная x64-реконструкция узкого verifier-clean подмножества ниже |
| `__CxxFrameHandler4` | Ограниченное variable-length декодирование в общий C++-граф | Тот же HighIR с provenance FH4 | Только анализ; затронутая функция отклоняется |
| `__GSHandlerCheck_SEH/EH/EH4` | Wrapped personality и проверенная provenance GS cookie | Базовый языковой граф и wrapper-аннотация | Только анализ; отказ без downgrade |
| x86 registration-chain EH | Отделено от табличного EH | Аннотация неподдерживаемой формы | Не реконструируется |

Malformed record никогда не считается полным. Partial decode остаётся полезным
для исследования, но не разрешает нативную генерацию. Если header xdata ARM всё
ещё доказывает ограниченный исполняемый диапазон fragment при повреждённом unwind
body, диапазон сохраняется для disassembly; record остаётся malformed и не
становится patchable.

## Нормализованная модель

`ExceptionInfo` принадлежит `BinaryImage`. Каждая `ExceptionFunction` содержит:

- проверенный полуоткрытый диапазон кода;
- identity primary, chained или fragment;
- нативный unwind encoding и точную runtime/unwind provenance;
- нормализованные операции/эпилоги и непрозрачные operands неизвестных операций;
- точную personality identity и handler data;
- необязательные scopes SEH, карты состояний C++ и данные GS cookie;
- статус `Complete`, `Partial` или `Malformed` и детерминированные диагностики.

Loader не раскрывает необработанные указатели. Нативные RVA сохраняются для
диагностики и замены; потребители IR используют только проверенные VA/диапазоны.

Общий индекс допускает перекрывающиеся chained/fragment records и возвращает
наиболее конкретную функцию. Повреждённые каталоги, диапазоны, указатели,
счётчики, переходы, сжатые числа, циклы цепочек и исчерпание decode budget
понижают соответствующий parse status.

Лимиты действуют для каждой таблицы и полного графа функции. Повторное
использование handler map многими try entries не превышает общий бюджет. FH3
records с общими `FuncInfo` и personality образуют ограниченную группу: catch
funclets родителя разрешены, адреса посторонних runtime functions — нет.

## Контракт IR

Metadata исключений проходят через все представления, не меняя обычный CFG:

- LowIR делит блоки на границах диапазонов, состояний, filters, handlers, cleanup и continuation.
- Исключительные successors/predecessors отделены от обычных рёбер.
- MedIR сохраняет нормализованный descriptor и стабильные исключительные рёбра.
- HighIR различает `SEHTry` и `CxxTry`, сохраняя VA, type descriptors,
  adjectives, offsets, actions, states и continuations.

HighIR structurer консервативен: перемещает только непрерывный фрагмент, полностью
лежащий в complete region, и обрабатывает вложенность изнутри. Пересекающиеся
области, partial graphs, неоднозначные границы и внешние funclets сохраняют
исходный control flow.

C backend выводит синтаксис MSVC SEH для сводимой области с одной clause. HighC
является C backend, поэтому C++ catches/cleanup становятся детерминированными
C-совместимыми комментариями, а не якобы компилируемым C++.

## Схема metadata LLVM

Каждая проанализированная функция исключений получает lossless metadata даже
без нативного WinEH lowering:

- attachment `neverd.windows.eh`;
- нативный marker `neverd.windows.eh.native`;
- module table `neverd.windows.eh.functions`;
- версия схемы `3`.

Фиксированный record хранит status, encoding, range, runtime/unwind RVA, тип и
цепочку, packed word, frame, имена personality, handler, unwind bytes,
operations/epilogues, SEH scopes, C++ maps, GS data, diagnostics и флаг
regeneration. Patch требует точную версию и полное совпадение range с image.

Нативный x64 SEH lowering использует LLVM WinEH и создаёт verifier-clean
`invoke`/funclets только для полностью представимого графа. FH3 требует:

- x64 COFF, unwind v1/v2, complete metadata и валидный синхронный FH3 graph;
- отсутствие `noexcept`, async, separated-funclet, GS, FH4 и неизвестных flags;
- вложенные или непересекающиеся интервалы, без crossing;
- отсутствие destructor/unwind action, catch-object construction и parent frame;
- handler в обычном блоке без predecessor и call;
- LLVM `invoke` для каждой защищённой операции, способной выполнить unwind.

Иначе IR остаётся анализируемым и lossless, но нативная замена отклоняется. PE
entry point, TLS callbacks и CRT roots являются границами сохранения.

## Транзакция patch

Поддерживаемое переписывание — единая транзакция PE:

1. Проверить каждую затронутую функцию по загруженному графу и metadata LLVM.
2. Компилировать с сохранением identity/alignment/traits секций и semantic
   references; externalize локальную Windows personality и привязать xdata к
   доказанному исходному executable handler.
3. Сохранить нетронутые runtime functions и удалить всё заменяемое замыкание,
   включая chained records.
4. Relocate code/xdata, merge/sort pdata, отклонить overlap, доказать personality
   class и установить единственный exception directory.
5. Сохранить CFG, разрешить `.gfids`/`.gehcont`, объединить Guard CF/EH
   continuation и обновить load-config. Неразрешённый helper прерывает операцию;
   CFW, return-flow guard, retpolines и XFG остаются analysis-only.
6. Повторно разобрать готовый byte image до записи.

Расширение fork LLVM остаётся общим: final-image writer сохраняет traits секций
и symbol references. PE/MSVC, policy, merging, load-config и финальная проверка
остаются в NeverD.

Исходные entries Guard CF/EH continuation сохраняются, поскольку их trampolines
остаются допустимыми indirect targets. Созданные цели должны лежать в emitted
code, а таблицы — быть строго отсортированы по RVA.

## Проверка конечного image

Patched PE отклоняется, если не выполнено всё:

- LLVM принимает COFF, а machine, class, sections, directories, base и extent совпадают;
- raw/virtual extents ограничены и не перекрываются;
- exception directory file-backed и находится в image;
- runtime functions отсортированы, непусты, не перекрываются и исполняемы;
- x64 unwind RVA/header/version/flags/handler/chains валидны;
- финальные imports, exports и COFF symbols позволяют повторно разобрать SEH/FH3;
- ARM records/xdata описывают поддерживаемые version/range;
- поля Guard CF/EH существуют, когда flags объявляют таблицы;
- pointers/counts/strides находятся в file/image, а цели упорядочены и исполняемы.

Любая ошибка прерывает patch; best-effort image не записывается.

## Целевая проверка

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

Защищённая x64 fixture использует `/guard:cf` и `/guard:ehcont` и проверяет SEH
scopes, Guard, HighC, patch, reload, порядок и цели. FH3 fixture проверяет
фиксированные таблицы, аннотации, personality, try/catch и IP-to-state. При
изменении parser следует также запускать ARM format cases.

## Расширение нативной поддержки

Каждая новая нативная форма должна в том же change добавить:

- полный ограниченный parser и invariants модели;
- round-trip HighIR/metadata LLVM;
- verifier-clean native IR для каждого принятого графа;
- необходимое сохранение секций и references;
- linked PE fixture для точных architecture/personality/version;
- validation exception-directory, load-config и final image;
- явные rejection tests ближайших неподдерживаемых форм.

Само умение декодировать record не расширяет allow-list. Критерий — сохранение
поведения runtime exceptions в конечном linked image.
