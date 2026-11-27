# Python artifact recovery

Use **File → Recover Python source…** or `neverd python` to inspect a file or a PyInstaller directory. Both use the same offline C++ reader and the same `language-recovery-v1` report. The reader treats code objects as data. Recovery requires no Python interpreter or packager.

```sh
neverd python module.pyc
neverd python module.pyc --json -o report.json
neverd python module.pyc --function=1 -o function.py
neverd python dist/game --json -o components.json
neverd python dist/game --component=7 --json -o module-report.json
neverd python dist/game --component=7 --function=1 -o member-function.py
neverd python dist/game --native=12 -o extension.so
neverd python entry.marshal --profile=cpython-3.10
```

Read component IDs from the inventory. Function IDs belong to the report for the selected component or entry scripts. The default package view shows entry scripts and selects the last script in execution order. Select a module to request its own functions. Select an instruction to jump to its reconstructed line when available. **Evidence** exposes the same diagnostics and mappings as JSON. Copy and source export use the selected function. Report export includes the complete inventory for the request. All file exports require a new path. Ctrl+C or **Cancel** stops the job.

A Python source export succeeds when the requested body is available. JSON export returns success for complete source, disassembly and inventory reports. A partial or failed job also writes its diagnostic JSON and returns a nonzero status. The native command exports the selected bytes after rechecking their SHA-256, opens a regular native analysis session and prints its discovered functions. **Open in native analysis…** performs the same identity check before opening the file in the workbench.

## Inputs

| Artifact | Evidence and result |
| --- | --- |
| Original `.py` | `extracted_source`. Exports preserve exact bytes, including BOM and line endings. UTF-8, ASCII and Latin-1 have a text view. Other encodings retain exportable bytes and a display diagnostic. |
| `.pyc` | The magic selects the code-object, opcode and line-table layouts. Timestamp, checked-hash and unchecked-hash headers retain their own fields. Source hashes are header evidence. Source-file freshness requires the matching original. |
| Raw marshalled code | A `.marshal` file requires an explicit qualified CPython profile. Packaged code needs a version from its enclosing cookie or PYZ magic. Conflicting evidence produces a diagnostic. |
| PyInstaller onefile, onedir, CArchive and PYZ | The reader validates member names, extents and overlaps before expanding zlib members. It inventories entry scripts, modules, packages, data and native images. Directory traversal uses sorted names and skips symlinks. External dependencies retain their package and member names, with `missing_member` for unresolved entries. |
| Cython, Nuitka and other native images | Valid Mach-O, ELF and PE objects route to the native backend. A `PyInit_` symbol identifies extension evidence. Recognizable Cython or Nuitka symbol families supply producer evidence when present. Stripped images may retain only their native format. |
| Ren’Py | The `renpy` profile accepts supported Python sources, disk bytecode and PyInstaller members with version evidence. Ren’Py script containers (`.rpyc`) have a separate layout and are unsupported. |

## CPython release matrix

Each row uses a bytecode file produced by the recorded CPython interpreter. The fixture contains a module, two nested code objects, arithmetic and a conditional. The compiler disassembly supplies independent byte offsets and mnemonics. Executing the recovered module on the same interpreter checks both branches and the function results. A magic number identifies a disk layout. Several patch releases can share it.

| CPython | Tested producer | Magic bytes, in file order | Instruction and source qualification |
| --- | --- | --- | --- |
| 0.9.8 | 0.9.8 | 94 94 94 00 | Module, arithmetic functions and conditional |
| 0.9.9 | 0.9.9 | 01 99 99 00 | Module, arithmetic functions and conditional |
| 1.0 | 1.0.1 | 02 99 99 00 | Module, arithmetic functions and conditional |
| 1.1 | 1.1.1 | 03 99 99 00 | Module, arithmetic functions and conditional |
| 1.2 | 1.2 | 03 99 99 00 | Module, arithmetic functions and conditional |
| 1.3 | 1.3 | 89 2e 0d 0a | Module, arithmetic functions and conditional |
| 1.4 | 1.4 | 04 17 0d 0a | Module, arithmetic functions and conditional |
| 1.5 | 1.5.2 | 99 4e 0d 0a | Module, arithmetic functions and conditional |
| 1.6 | 1.6 | fc c4 0d 0a | Module, arithmetic functions and conditional |
| 2.0 | 2.0.1 | 87 c6 0d 0a | Module, arithmetic functions and conditional |
| 2.1 | 2.1.3 | 2a eb 0d 0a | Module, arithmetic functions and conditional |
| 2.2 | 2.2.3 | 2d ed 0d 0a | Module, arithmetic functions and conditional |
| 2.3 | 2.3.7 | 3b f2 0d 0a | Module, arithmetic functions and conditional |
| 2.4 | 2.4.6 | 6d f2 0d 0a | Module, arithmetic functions and conditional |
| 2.5 | 2.5.6 | b3 f2 0d 0a | Module, arithmetic functions and conditional |
| 2.6 | 2.6.9 | d1 f2 0d 0a | Module, arithmetic functions and conditional |
| 2.7 | 2.7.18 | 03 f3 0d 0a | Module, arithmetic functions and conditional |
| 3.0 | 3.0.1 | 3b 0c 0d 0a | Module, arithmetic functions and conditional |
| 3.1 | 3.1.5 | 4f 0c 0d 0a | Module, arithmetic functions and conditional |
| 3.2 | 3.2.6 | 6c 0c 0d 0a | Module, arithmetic functions and conditional |
| 3.3 | 3.3.7 | 9e 0c 0d 0a | Module, arithmetic functions and conditional |
| 3.4 | 3.4.10 | ee 0c 0d 0a | Module, arithmetic functions and conditional |
| 3.5 | 3.5.0, 3.5.10 | 16 0d 0d 0a, 17 0d 0d 0a | Module, arithmetic functions and conditional |
| 3.6 | 3.6.15 | 33 0d 0d 0a | Module, arithmetic functions and conditional |
| 3.7 | 3.7.17 | 42 0d 0d 0a | Module, arithmetic functions and conditional |
| 3.8 | 3.8.20 | 55 0d 0d 0a | Module, arithmetic functions and conditional |
| 3.9 | 3.9.6 | 61 0d 0d 0a | Module, arithmetic functions and conditional |
| 3.10 | 3.10.16 | 6f 0d 0d 0a | Module, arithmetic functions and conditional |
| 3.11 | 3.11.11 | a7 0d 0d 0a | Module, arithmetic functions and conditional |
| 3.12 | 3.12.8 | cb 0d 0d 0a | Module, arithmetic functions and conditional |
| 3.13 | 3.13.3 | f3 0d 0d 0a | Module, arithmetic functions and conditional |
| 3.14 | 3.14.5 | 2b 0e 0d 0a | Module, arithmetic functions and conditional |
| 3.15 | 3.15.0 | 52 0e 0d 0a | Module, arithmetic functions and conditional |

CPython 1.1 and 1.2 use the same disk signature and instruction layout. Automatic detection reports both candidates. An explicit profile selects the recorded producer. CPython 0.9.8 omits code names, so function names come from a proven enclosing assignment. The 0.9.1 distribution reads source files and has no disk bytecode cache.

The 3.10 source fixtures exercise closures with repeated mutation, function defaults, positional and keyword-only arguments, list comprehensions, generators, async iteration and await, imports, loops, short-circuit expressions, typed exception handling, finally blocks, context-manager suppression, assignment expressions and simple literal matches. Syntax that compiles to the same operations can acquire a different spelling. Nested functions are independently selectable. Their reports identify whether parameter defaults came from an enclosing function binding or only code-object parameter names were available. A missing operand, unsupported instruction or unproved cleanup region carries a per-function diagnostic and qualified siblings remain accessible.

CPython 3.11 and later retain disk cache extents, location data and exception-table regions. Functions outside the verified source rules retain their decoded instruction evidence.

## Reading the report

- `components` stores the logical member name, parent, ID, size, content SHA-256 and backend. Compressed members also record their stored size and hash. `stored_offset` is relative to the immediate container. `stored_file_offset` and `data_file_offset` are present when the corresponding physical position in a backing file is known. The onedir root hashes the ordered names and file identities.
- `functions` gives code-object and bytecode offsets in the expanded component. Each instruction has a byte PC, component offset, encoded length and validated control-flow successors when decoding completed. `original_line` refers to the compiler's line table. `source_line` refers to the displayed function. For 3.11, `opcode_pc` identifies the opcode after any operand prefixes, `cache_units` counts trailing two-byte cells and `exception_regions` gives protected half-open byte ranges, handler targets, unwind depth and the saved-instruction flag.
- `reconstructed_source` and `extracted_source` publish source with distinct provenance. `disassembly_only` publishes decoded evidence. `available` identifies a selectable packaged module. `metadata_only` describes inventory or a native route. Partial, malformed, unsupported, missing, cancelled, I/O and budget outcomes retain explicit statuses. For byte-preserving extraction, `source_base64` takes precedence over the optional display text.
- A job admits 256 MiB of inputs, 32 MiB per inflated member and 128 MiB of cumulative expansion. Other bounds include 4,096 components, depth 64, 131,072 marshal objects and slots, 1,024 code objects, 16,384 instruction words, 1 MiB of source-work budget and 4 MiB of serialized report. A budget diagnostic identifies work that could not finish.

## Reproducing the qualification

The owned source and manifests under `unittests/python/fixtures` record producer version, platform, architecture, build flags, digests, bytes and independent instruction observations. With CPython 3.10.16, run `python unittests/python/qualify.py --neverd /path/to/neverd` to compare observations from recompiling the owned original and its recovered text under matching optimization. Add `--check-producer` with PyInstaller 6.16.0 and Clang to regenerate the manifest inputs. Add `--bundles` to build full onefile and onedir distributions and compare the recovered entry behavior and original file hashes. With CPython 3.11.11, run `python unittests/python/qualify311.py` to reproduce the disk layout and unwind oracle. Normal CTest uses the stored artifacts and needs neither producer. The behavior comparison applies to these controlled programs.

For each entry in `fixtures/releases.json`, run `python3 unittests/python/qualify_releases.py /path/to/python EXACT_VERSION --cli /path/to/neverd`. This regenerates the owned bytecode, compares instruction offsets and names and runs only the recovered test program. The earliest producers use Linux host portability fixes for their runtime and build system. Their compiler, opcode assignments and marshal writer supply the fixture bytes. The 0.9.8 source archive omits its dictionary implementation and the producer was built with the earlier string-key dictionary. The 3.5.0 and 3.5.10 fixtures exercise both stable 3.5 magic numbers.

To regenerate the compact opcode table, pass a `RELEASE=DIR` argument for each unpacked release to `unittests/python/generate_release_opcodes.py`. Use 3.5.10 for the 3.5 opcode assignments. The 3.10 and 3.11 decoders maintain their own tables.

The bytecode, location, exception and packaging layouts are version-specific. [CPython 3.10 disassembly](https://docs.python.org/3.10/library/dis.html), [CPython 3.11 disassembly](https://docs.python.org/3.11/library/dis.html), [3.11 location encoding](https://github.com/python/cpython/blob/v3.11.11/Objects/locations.md), [3.11 exception tables](https://github.com/python/cpython/blob/v3.11.11/Objects/exception_handling_notes.txt), [PEP 552](https://peps.python.org/pep-0552/), [PEP 626](https://peps.python.org/pep-0626/) and [PyInstaller archive formats](https://pyinstaller.org/en/stable/advanced-topics.html#inspecting-archives).
