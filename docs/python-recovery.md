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

Rows record the exact producer exercised in this repository. Matching magic selects a layout at runtime. It cannot establish the producer's patch release or optimization level. The 3.10 fixtures cover optimization levels 0, 1 and 2 and all three header modes. The 3.11 fixture covers optimization level 0 and a checked-hash header. Runtime-adaptive and populated inline-cache streams are outside the disk profile. Release families follow the [CPython version inventory](https://devguide.python.org/versions/).

| CPython minor | Qualified fixture and magic bytes | Decoder | Source |
| --- | --- | --- | --- |
| 2.7 | Unvalidated | Unsupported | Unsupported |
| 3.0 | Unvalidated | Unsupported | Unsupported |
| 3.1 | Unvalidated | Unsupported | Unsupported |
| 3.2 | Unvalidated | Unsupported | Unsupported |
| 3.3 | Unvalidated | Unsupported | Unsupported |
| 3.4 | Unvalidated | Unsupported | Unsupported |
| 3.5 | Unvalidated | Unsupported | Unsupported |
| 3.6 | Unvalidated | Unsupported | Unsupported |
| 3.7 | Unvalidated | Unsupported | Unsupported |
| 3.8 | Unvalidated | Unsupported | Unsupported |
| 3.9 | Unvalidated | Unsupported | Unsupported |
| 3.10 | 3.10.16, `6f0d0d0a` | Wordcode, PEP 626 lines, block setup and handler edges | Qualified functions and structured regions |
| 3.11 | 3.11.11, `a70d0d0a` | Disk instructions, inline-cache extents, localsplus, source lines and exception table | Disassembly only |
| 3.12 | Unvalidated | Unsupported | Unsupported |
| 3.13 | Unvalidated | Unsupported | Unsupported |
| 3.14 | Unvalidated | Unsupported | Unsupported |
| 3.15 | Unvalidated | Unsupported | Unsupported |

The 3.10 source fixtures exercise closures with repeated mutation, function defaults, positional and keyword-only arguments, list comprehensions, generators, async iteration and await, imports, loops, short-circuit expressions, typed exception handling, finally blocks, context-manager suppression, assignment expressions and simple literal matches. Syntax that compiles to the same operations can acquire a different spelling. Nested functions are independently selectable. Their reports identify whether parameter defaults came from an enclosing function binding or only code-object parameter names were available. A missing operand, unsupported instruction or unproved cleanup region carries a per-function diagnostic and qualified siblings remain accessible.

## Reading the report

- `components` stores the logical member name, parent, ID, size, content SHA-256 and backend. Compressed members also record their stored size and hash. `stored_offset` is relative to the immediate container. `stored_file_offset` and `data_file_offset` are present when the corresponding physical position in a backing file is known. The onedir root hashes the ordered names and file identities.
- `functions` gives code-object and bytecode offsets in the expanded component. Each instruction has a byte PC, component offset, encoded length and validated control-flow successors when decoding completed. `original_line` refers to the compiler's line table. `source_line` refers to the displayed function. For 3.11, `opcode_pc` identifies the opcode after any operand prefixes, `cache_units` counts trailing two-byte cells and `exception_regions` gives protected half-open byte ranges, handler targets, unwind depth and the saved-instruction flag.
- `reconstructed_source` and `extracted_source` publish source with distinct provenance. `disassembly_only` publishes decoded evidence. `available` identifies a selectable packaged module. `metadata_only` describes inventory or a native route. Partial, malformed, unsupported, missing, cancelled, I/O and budget outcomes retain explicit statuses. For byte-preserving extraction, `source_base64` takes precedence over the optional display text.
- A job admits 256 MiB of inputs, 32 MiB per inflated member and 128 MiB of cumulative expansion. Other bounds include 4,096 components, depth 64, 131,072 marshal objects and slots, 1,024 code objects, 16,384 instruction words, 1 MiB of source-work budget and 4 MiB of serialized report. A budget diagnostic identifies work that could not finish.

## Reproducing the qualification

The owned source and manifests under `unittests/python/fixtures` record producer version, platform, architecture, build flags, digests, bytes and independent instruction observations. With CPython 3.10.16, run `python unittests/python/qualify.py --neverd /path/to/neverd` to compare observations from recompiling the owned original and its recovered text under matching optimization. Add `--check-producer` with PyInstaller 6.16.0 and Clang to regenerate the manifest inputs. Add `--bundles` to build full onefile and onedir distributions and compare the recovered entry behavior and original file hashes. With CPython 3.11.11, run `python unittests/python/qualify311.py` to reproduce the disk layout and unwind oracle. Normal CTest uses the stored artifacts and needs neither producer. The behavior comparison applies to these controlled programs.

The bytecode, location, exception and packaging layouts are version-specific. [CPython 3.10 disassembly](https://docs.python.org/3.10/library/dis.html), [CPython 3.11 disassembly](https://docs.python.org/3.11/library/dis.html), [3.11 location encoding](https://github.com/python/cpython/blob/v3.11.11/Objects/locations.md), [3.11 exception tables](https://github.com/python/cpython/blob/v3.11.11/Objects/exception_handling_notes.txt), [PEP 552](https://peps.python.org/pep-0552/), [PEP 626](https://peps.python.org/pep-0626/) and [PyInstaller archive formats](https://pyinstaller.org/en/stable/advanced-topics.html#inspecting-archives).
