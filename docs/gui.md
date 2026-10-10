# NeverD desktop workbench

The desktop workbench is a Qt Widgets application with separate
`neverd-worker` processes for browsing and expensive analysis views. It starts
in the classic interactive disassembler layout and keeps that layout's window
names, menus and default shortcuts, so
existing muscle memory carries over; colors follow the Visual Studio Code Dark+
(default) and Light+ themes, and every icon is original NeverD artwork. The
workers link the same `libneverd` shared library as the CLI through its public C
ABI: analysis, function discovery and decompilation stay in that library, and Qt
owns presentation and request coordination. Source and graph requests use a
disposable read-only worker, so a long decompile does not hold up uncached
function-list pages or disassembly requests. The GUI executable does not link
LLVM or the CLI, and no model service is needed to browse binaries.

## Build

The normal engine/CLI configuration is unchanged. Qt is optional and is searched
only when `NEVERD_BUILD_GUI=ON`. To add the workbench to an engine build, use:

```sh
cmake -S . -B build -DNEVERD_BUILD_GUI=ON -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/platform
cmake --build build --target neverd-gui
```

For fast desktop development, build against a matching existing shared engine:

```sh
cmake -S tools/neverd-gui -B build-gui -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/platform \
  -DNEVERD_ENGINE_LIBRARY=/path/to/libneverd.so
cmake --build build-gui
ctest --test-dir build-gui --output-on-failure
build-gui/bin/neverd-gui /absolute/path/to/binary
```

Requires C++20, CMake 3.24+, Qt 6.8+ Core, Gui, Widgets, Network, Svg, Sql
(with its SQLite driver), Concurrent and LinguistTools, the GuiPrivate and
WidgetsPrivate headers for the pinned KDDockWidgets frontend (Qt Test for
tests), and Python 3.10+ for tests and the MCP adapter. The first configure
downloads nlohmann/json 3.11.3 and KDDockWidgets 2.4.1 using pinned SHA-256
digests. Ship the exact Qt build used to compile its private headers. On Windows
set `NEVERD_ENGINE_LIBRARY` to the runtime DLL and `NEVERD_ENGINE_IMPLIB` to its
matching import `.lib`; make runtime dependencies available alongside the
worker.

Open a binary or a project with **File → Open**, or drag one file from
the system file manager onto the quick-start dialog, workbench or a floating
view. File drops use the same open workflow, including the save/discard/cancel
prompt for unsaved changes. Drop one existing file at a time; directories and
web URLs are not opened. Opening by drag and drop leaves the source file in
place.

PE browsing tolerates complete legacy relocation layouts and reports unusable
entry/import metadata in **Output**. An unknown entry opens at a mapped code
region for browsing; this does not reconstruct the program's OEP. Invalid import
bindings remain unknown, and fixed-image semantic checks retain their stricter
requirements. Truncated or unmappable image structures can still prevent loading.

Local file paths passed between the GUI, worker, CLI and C ABI use UTF-8 on
all platforms. Windows paths are converted to native filesystem paths before
loading binaries, companion PDB/MAP files, signatures and project sidecars.
Chinese names, spaces and other Unicode characters are supported without
changing the Windows system code page.

Code views, the output window and text fields use the code font: Consolas at
10 points when that family is installed, otherwise the system fixed-width font.
Menus, tabs, lists, buttons and labels keep the system font, as in the classic
disassembler's default font settings. A saved code-font choice overrides the
default for code views and text fields.

Double-clicking a function name in a code view follows it in that same window,
keeping its C or LLVM C representation even when the window is locked. Back and
forward navigation also stays in the active code window. Imports and global
objects continue to open at their addresses in the disassembly or hex view.

The worker can also be built without Qt, either with `NEVERD_BUILD_WORKER=ON` in
the root build or by configuring `tools/neverd-worker` standalone. The shipped
worker never links the test engine.

## Layout

A start without a file shows the quick start over the default desktop, as IDA
does. Drag its window edges or lower-right grip to resize it; the next opening
remembers that size. The action pane stays the same width while the recent
files use the available space. Its side pane holds the ways to start: **New**
(N) disassembles a new file, **Go** (G) works on your own and **Previous** (P)
loads the selected recent file; the one Enter takes has a subtle row background,
and keyboard focus has a thin neutral outline. The actions use matching line
icons without individual tiles. Each recent file shows its format in an outlined
file icon (ELF, PE, Mach-O, a NeverD database or a plain binary file),
its name above the folder and last-opened time. Long names retain both ends;
hovering shows the full path and time. Delete or its context menu forgets it.
A file dragged over the dialog shows that a drop opens it. **Display at
startup** decides whether the next start shows it again; **File → Quick start**
shows it any time. Behind it is the default desktop:

- the navigation band across the top: the whole address space colored by
  library functions, regular functions, instructions, data, unexplored bytes
  and external symbols, with the current position; click or drag to navigate;
- **Functions** on the left, paged from the worker so a million functions cost
  only their visible rows; import thunks and recognized library functions are
  tinted;
- **NeverD View-A** in the center with **Hex View-1**, **Imports** and
  **Exports** as tabs, and the status line under the listing;
- **Output** across the bottom, with a command line for expressions and
  workbench commands;
- the status bar with the background analysis indicator (`AU: idle` or busy
  with progress), the search direction and free disk space.

Every window is a dock: drag tabs to split, stack or float them. Docked windows
are one hairline apart; drag the line to resize its neighbors, or double-click
it to share their space evenly. A window narrows to about the width of its tab:
a status line or a row of buttons that no longer fits is cut at the window's
edge. **Windows → Save desktop** remembers an
arrangement and **Reset desktop** returns to the default. **Graph overview**
appears under the function list in graph view, and the pseudocode and IR windows
open beside the disassembly.

## Views

**Disassembly** is the whole image as one address-ordered listing in the classic
format: segment headers, function headers with their attributes and stack
variables, unwind frame markers, `proc`/`endp`, `loc_` and `locret_` labels,
data items, alignment and cross-reference comments, and the classic instruction
spellings (`jz`, `retn`, `[rbp+var_30]`, sizes only where no operand implies
them), with `segment:address` prefixes and optional opcode bytes (**Options →
General**). Stack variables are named through the frame pointer and, in 64-bit
code, through the stack pointer wherever every path agrees on its distance
from the frame (`[rsp+48h+var_30]`); each is as wide as its widest access.
A named object is as large as its symbol says (`stderr dq ?`), and data the
code reads or writes is laid out as wide as those accesses, under the name its
operands and the pseudocode use (`qword_A410 dq ?`, or `unk_` where the widths
differ); other unwritten bytes run as `db N dup(?)` up to the next item. Only a window of lines around the viewport is held; the scroll bar
maps to the linear address space. The arrow gutter draws branches, and clicking
an identifier highlights every occurrence. Names the engine leaves generic take
their classic forms in the listing, function list and jumps: import thunks after
their import, the entry point `start`, and `main` as passed to the C runtime's
start routine.

**Graph view** (Space) lays the current function out in layers with the
conditional (green/red) and unconditional edges routed around blocks; panning
and zooming are local, and text is dropped at low zoom. The graph overview
shows the whole function and the visible area.

Opening a file or database starts with disassembly and does not generate
pseudocode, including when a saved desktop had it open. **F5** or **Tab** opens
Pseudocode on the right of disassembly. The panes browse independently: clicks,
arrow keys and scrolling do not move the other pane. **Tab** from a code window
selects its mapped instruction in disassembly; Tab from disassembly selects the
mapped source row in the last code window used, keeping its representation and
both panes visible. A folded target row expands, and a new function waits for
all its source pages before selecting the row. An unmapped source row falls
back to its function entry with a message; an unmapped instruction reports that
no source row is available and keeps the existing source cursor. Closing or
hiding a pane stops its requests; showing it again resumes its own function.

**Pseudocode** and **IR** windows show pseudocode in the function's
own language, C, Rust, Go, C through LLVM, LowIR, MedIR, HighIR or LLVM IR of
the requested function. F5 or Tab pressed while a jump is loading decompiles the
function the jump lands in. Source arrives in 256-line pages; the view appends
new lines without recoloring all preceding pages. The engine emits a function's
source once and pages it from there; a function longer than one page keeps the listing's names,
such as `main`. The
LLVM views translate the current function alone,
with the others declared, so a function the engine refuses to translate shows
its reason without affecting other functions. Tab navigates using explicit
instruction mappings. C opens at the function: the includes, support types and
declarations before its definition fold into one line. Recognized library
operations in C can fold into one-line summaries too. Click a summary or press
Keypad + on it to expand it; Keypad - folds the declarations again, and the
context menu expands or collapses either kind. Hovering a type or macro the code
declares, such as an unaligned access type, shows its declaration, and
double-clicking it goes there. Double-clicking a function's name shows that
function's pseudocode in place; an import's name opens its thunk, or its slot
when it has none, in the disassembly, as do other names of data. Memory reads
and writes print through the standard scalar types, `*(uint64_t *)p`. That C
reads scalars at any address and through any type, so it is built for a target
that allows unaligned access and with `-fno-strict-aliasing`, as its prelude
notes; recovered bytecode and devirtualized sources, which are compiled to run,
declare one-byte-aligned, `may_alias` types instead. A call argument that points
to a string, directly or through a pointer the image holds, shows the string
beside it, after its encoding unless that is ASCII or UTF-8:
`puts(u8s /* "你好" */)`, `/* GBK "中文" */`. Copy and export always use the
complete code.

Global data is declared the way C source declares it, with the value the image
holds: a string the code reaches by its address is its array
(`const char gbk[] = "\xD6\xD0\xCE\xC4"; /* GBK "中文" */`), a pointer the code
only reads that holds a read-only string's address is that string's pointer
(`const char *u8s = "你好";`, `const char16_t *w = u"宽字";`), and a scalar
shows its initial value (`int32_t counter = 5;`). Every initializer spells the
image's bytes exactly: text another encoding than UTF-8 holds is escaped, with
the decoded text in a comment beside it. Unnamed data is named as the listing
names it, so a name in the pseudocode matches the disassembly operand and
double-clicking it goes there: `off_3FC0` for a slot holding a pointer or one
the code calls through, the size of its accesses otherwise (`qword_3FB8`,
`dword_4010`), and `unk_` for data reached by its address alone. The command
line's `neverd decompile` prints the same declarations.

A function's address reads as the function: `_start` passes `main`, not
`0x1169`. A function the output defines is declared before the code that
names it, and one it does not define is declared as a callee is. A routine no
standard header declares is declared with the prototype the C library tables
give it: the start-up and exit routines (`__libc_start_main`, `__cxa_atexit`,
`__cxa_finalize`), the errno and ctype accessors (`__errno_location`,
`__ctype_b_loc`), the Itanium C++ runtime and unwinder (`__cxa_throw`,
`__cxa_begin_catch`, `_Unwind_Resume`), glibc's fortified and ISO C routines
(`__printf_chk`, `__isoc23_sscanf`) and, in a PE image, the Windows C
runtime's (`__getmainargs`, `_initterm`, `__stdio_common_vfprintf`). Each
argument converts to its parameter type as a disassembler's decompiler shows
it: `__libc_start_main((int (*)(int, char **, char **))main, argc, (char
**)argv, 0, 0, (void (*)(void))rtld_fini, (void *)stack_end)`. A pointer such
a routine returns keeps the integer the machine reads,
`(uintptr_t)__errno_location()`. A function passed to a standard function
converts to the type its header declares: `qsort(base, n, 4, (int (*)(const
void *, const void *))compare)`.

Mangled names read as their language spells them: the listing keeps the
linkage name an instruction uses and adds the readable form as a comment
(`call _ZN8QDomNodeC1Ev ; QDomNode::QDomNode()`), a function with a mangled
name has the readable one above its header, and the Functions window lists
every function that way, PLT entries included, with the filter matching
either spelling. Itanium and Microsoft C++, Rust (legacy and v0), Swift and D
names are read. C++ names are as short as they can be without naming anything
else: `std::__cxx11::basic_string<char, std::char_traits<char>,
std::allocator<char>>::~basic_string()` reads `std::string::~string()` and
`std::vector<int, std::allocator<int>>` reads `std::vector<int>`. A legacy
Rust name loses its `::h<hash>`, and a Swift name reads as a declaration path
with its argument labels (`Demo.Box.update(with:)`).

The pseudocode is C that compiles, so a name C cannot spell reads by a C
identifier made from it, with its readable form in a comment: above a
definition, after an `extern` declaration and on the `neverd.image` line of a
global. A C++ function reads by its scopes joined with underscores:
`QDomNode_nodeType`, constructors and destructors as `QDomNode_ctor` and
`QDomNode_dtor`, operators by name (`QString_assign`), and the objects the C++
ABI emits by what they are (`QDomNode_vtable`). Rust and D paths read the same
way (`core_fmt_write`; a trait's method keeps the trait,
`String_Write_write_fmt`), Swift by its declaration path
(`Demo_Box_count_getter`) and an Objective-C method as the GNU runtime names it
(`-[NSString length]` is `_i_NSString__length`). Punctuation in other names
separates words: Go's `fmt.(*pp).doPrintf` is `fmt_pp_doPrintf` and GCC's
`foo.constprop.0` is `foo_constprop_0`. Distinct symbols that read alike are
numbered (`QDomNodeList_ctor_2`), except MSVC C++ stems, which the MSVC rules
share between overloads. An imported function whose identifier is not its
symbol keeps the symbol in an `__asm__` label so that the code still links.
Other names keep every byte of their symbol (`__libc_start_main`), apart from
the underscore Mach-O and 32-bit Windows add to C names and the start-up
functions the C runtime defines itself (`_start` reads `start`). A name an
image spells with control characters, and that no scheme reads, is never
copied into a comment.

**Pseudocode** reads in the language the function was written in: a Rust
function as Rust, a Go function as Go and any other as C. The choice is made
for each function, from its symbol, and for a function without one from the
language the image was built in; the window says when it is not C
(`Pseudocode-A (Rust)`). A program with Rust or Go code also offers **C** in
the window's menu, to read every function as C; a C or C++ program's
Pseudocode is C, so its menu offers no other language. The Rust and Go views
spell exactly what the C says. Each conversion C makes on its own is written out (`a as u32 + b as u32`,
`uint32(a) + uint32(b)`), conditions compare with zero (`v != 0`,
`!p.is_null()`, `p != nil`), memory is read and written through a pointer made
from the address (`*((v0 + 8) as *mut i64)`, `*(*int64)(v0 + 8)`), and names
read as their language spells them (`core::fmt::write`,
`<&str as core::fmt::Display>::fmt`, `internal/cpu.Initialize`). Such a name is
one name: a click highlights it everywhere, a double-click goes to it, and
hovering it shows the C identifier and the symbol it reads. A few forms keep
C's meaning where the language has none, as the first lines of the view say:
integers wrap, `abort()` and `trap()` stand for a value the decompiler does not
know and trap when computed, Go converts booleans and pointers as plainly as
integers, and `goto`, SEH and C++ handlers and inline assembly keep their C
spelling. A declaration a view cannot spell, such as a pointer into another
address space, is shown as C with the reason above it, and the status line
counts them. Rust that avoids those forms compiles: the test suite builds the
Rust view of a set of functions with rustc and checks that it computes what the
C does. `neverd decompile --language=rust`, `go` or `source` prints the same
views for the whole program or, with `--func`, one function, and refuses Rust
or Go for a program without that language's code; `source` reads native code
in the image's language, EVM bytecode as Solidity and an SBF program as Rust.
`neverd headers --json` lists the languages a program offers under
`language.pseudocode`.

Strings are found by default in ASCII, UTF-8, UTF-16LE and UTF-32LE (the
`wchar_t` of Linux and macOS), and C strings that are not UTF-8 in the common
code pages: windows-1252, GBK, Big5, Shift-JIS and EUC-KR, all in one pass.
UTF-8 text such as Chinese, Japanese or Korean prints as itself
(`db '中文字符串',0`), each wide character two columns wide so the comments
after it line up, a string in another encoding under its label as
`text "Shift-JIS", '日本語',0`, and an instruction that refers to a string
quotes it in a comment (`; "Usage: %s"`). A code page's string is shown when
one of them reads it as text in a script it is made for and no code page for
another script reads it too: the Big5 bytes of 中文檔, which GBK reads as kana,
stay Big5, and KOI8-R text that GBK would read as ideographs is left alone.
**Options → String literals** chooses the encodings searched, including
UTF-16BE and UTF-32BE; whether the common code pages are detected; a preferred
code page (GBK, Big5, Shift-JIS, EUC-KR, Windows-1250 to 1258, ISO-8859, KOI8
or IBM866), which reads C strings first and wins where other code pages read
them too; and the minimum length in display columns, a wide East Asian
character counting two. Text mostly beyond ASCII also needs three characters,
since two random code points read as text too often. The Strings window shows
each string's encoding in its Type column. Code pages decode by the WHATWG
Encoding Standard, and a string reads as text only when its characters keep to
one script. Settings from versions that searched one code page keep it as the
preferred one.

**Search → String references** (Ctrl+Shift+F12, or `strref [text]` on the
command line) lists every instruction that refers to a string: directly, into
the middle of one (the text from that character on), or by reading a pointer
slot that holds its address. The list opens with its filter ready; the filter
matches the text, the function or the address, and every column but
Disassembly sorts. Enter goes to the instruction. In this list and in
Strings, Ctrl+X lists the references to the selected string.

**Hex View-1** follows the disassembly cursor; while it is the active view, a
jump (G or `g` on the command line) moves it and keeps it in front. Its
context menu's **Text encoding** reads the text column as ASCII, UTF-8,
UTF-16, UTF-32 or a code page; a character shows at its first byte, and wide
characters take two columns. The choice is kept for later sessions.
**Imports**, **Exports**, **Names**, **Strings**, **Segments** and
**Bookmarks** are choosers with a quick filter and sortable columns. **Jump
anywhere** (G) takes an address, a name or an expression such as `main+0x10`
and suggests names as you type. Cross references (X, Ctrl+X, Ctrl+J) list call
(`p`), jump (`j`), read (`r`), write (`w`) and offset (`o`) references from the
worker's reference index.

The output window's command line evaluates expressions in hexadecimal by
default (`#10` is decimal) and runs `g`, `x`, `n`, `c`, `d`, `f`, `graph`,
`hex`, `strref`, `analyze` and `save`; `help` lists them. **Options → Show command palette**
(Ctrl+Shift+P) searches every command.

## Keyboard

| Key | Action |
| --- | --- |
| G | Jump to an address, name or expression |
| Esc / Ctrl+Enter | Previous / next position (mouse Back/Forward work too) |
| Enter / Alt+Enter | Follow the operand / follow it in a new view |
| Space | Toggle graph and text view |
| F5 / Tab | Pseudocode in the function's own language / switch between disassembly and pseudocode |
| X / Ctrl+X / Ctrl+J | References to the operand / to the item / from the item |
| N | Rename the name under the cursor, or the address: a function at its entry, data, a label in code |
| P | Create a function at the address (**Edit → Functions** also deletes the current one) |
| D / A / U | Make data (again for the next size) / a string / bytes of the item |
| C | Define native instructions from the selected byte through a basic block |
| : or ; | Comment the address |
| Alt+M / Ctrl+M | Mark a position / jump to a marked position |
| Ctrl+P / Ctrl+L / Ctrl+S / Ctrl+E | Choose a function / name / segment / entry point |
| Alt+T, Ctrl+T / Alt+B, Ctrl+B | Search text / bytes, and repeat |
| Alt+Up / Alt+Down | Previous / next occurrence of the highlighted identifier |
| Ctrl+Shift+Up / Ctrl+Shift+Down | Previous / next function |
| Shift+F3, Shift+F4, Shift+F7, Shift+F12 | Functions, Names, Segments, Strings |
| Ctrl+Shift+F12 | String references |
| F6 / Shift+F6 | Next / previous window |
| Ctrl+F | Quick filter in a list; find text in a disassembly, pseudocode or IR window |
| Ctrl+F5 | Write the function's pseudocode to a C file |
| Alt+A | String literal options |
| Ctrl+W | Save the database |
| Ctrl+Shift+P | Command palette |

Single-key shortcuts apply only while an analysis view has the keyboard focus,
so typing in a field or dialog keeps normal text editing. **Options →
Shortcuts** lists every command with its key.

A command IDA also has takes IDA's default key: the legacy scheme of IDA's
`cfg/idagui.cfg`, and the decompiler's F5, Tab and Ctrl+F5.
`tools/neverd-gui/app/IdaActions.def` pairs each command with IDA's, and the
workbench unit tests fail when a default drifts from IDA's or when a command of
the workbench's own takes a key IDA gives another command. **File → Open**
therefore has no key: IDA's Ctrl+O makes an operand an offset into the current
segment. One difference is deliberate: in a disassembly, pseudocode or IR
window Ctrl+F finds text, where IDA's disassembly goes to the next error.

`scripts/compare_gui_with_ida.py --ida <installation>` checks the table against
an IDA installation and reports, menu by menu, which of IDA's commands the
workbench has, plans and lacks. Against IDA 9.4 on 2026-10-09, all 89 rows
matched IDA's configuration, and the workbench had 83 of the 255 commands in
IDA's main menus, with 59 more planned.

**Edit → Copy** (Ctrl+C) copies from the window that has the keyboard focus:
the selected lines of a disassembly, pseudocode or IR window (the current line
when nothing is selected), the selected rows of a list with their columns
separated by tabs, or the text selected in a field.

GNOME attaches a modal dialog to its parent window, so dragging the dialog
would drag the whole workbench. On GNOME the workbench keeps dialogs
free-standing: under X11 a modal dialog takes the utility window type, and
under Wayland the compositor is not told which dialogs are modal
(`xdg-dialog-v1`). File, color and font dialogs are Qt's own there, styled
like the rest of the workbench, because GTK's are another toolkit's windows,
which GNOME attaches. A modal dialog still blocks the workbench until it
closes.

Without a remembered size the window opens centered at three quarters of the
screen's width and four fifths of its height. GNOME maximizes a window that
opens at nearly the size of the screen, and a maximized window does not resize
from its edges; IDA opens that way. Under Wayland GNOME draws no frame and the
one Qt draws resizes only from a few pixels outside the visible edge, so the
last four pixels inside the left, right and bottom edges resize the window
too, under the matching cursor.

## Loading a new file

Opening a binary NeverD keeps no project for (no `.nddb` beside it and no
sidecar files) with **File → Open** or by dropping it on the window first shows
**Load a new file**, as IDA does. The list names every way the engine can read
the file, the way IDA writes it: `ELF64 for x86-64 (Shared object) [elf]`,
`Portable executable for AMD64 (PE) [pe]` (listed first), `Mach-O file
(EXECUTE). ARM64 [macho]`, a row per slice of a universal Mach-O file, `EVM
bytecode [evm]`, and `Binary file` last. A row NeverD cannot load is shown greyed
with the reason in its tooltip: a processor NeverD has no lifter for (`NeverD has
no MIPS processor`), a big-endian ELF file, the other slices of a universal file
(NeverD loads the host's slice, else the first). The first row loadable for
the file's contents is chosen, so Enter opens the file as before. A row a loader
took for the file's name alone (EVM bytecode for a `.bin` file whose bytes are
not bytecode) is listed but never chosen by default: the engine reads such a
file as EVM bytecode only when that row is chosen, and refuses to guess when
nothing is. **Processor type** shows the processor the header states; it cannot
be changed, since every loader takes it from the header.

**Binary file** reads any file as one processor's code, as firmware and memory
dumps need. Its processor list reads **Processor type (double-click to set)**
and offers the processors NeverD can decode: x86, x86-64, AArch64, and two ARM
rows, **ARM Little-endian, starting in ARM state** and **ARM Little-endian,
starting in Thumb state**. Both read ARM and Thumb code, as IDA's single ARM
Little-endian processor does; they differ only in the state the code starts in,
which IDA sets with its T segment register. The engine reads the processor from the bytes and the list opens on
it. Each instruction set leaves its own statistics of which byte follows which;
4 KiB windows of the file vote for the set that explains them best, padding,
text and compressed data are passed over, and a set settles the file only when
its family takes nearly all of the vote, from enough of the file and with
enough weight per window: code of a set the model lacks scatters a little
weight and settles nothing. Sets that differ in word size -- 32- and 64-bit
MIPS, PowerPC, SPARC, RISC-V and x86 -- are then told apart by their
instructions: the 64-bit set's code is full of encodings the 32-bit set has
none of (MIPS `ld` and `daddiu`, PowerPC `std`, x86-64 REX.W), read at the
alignment the set's byte-position statistics find. That alignment also tells a
file whose code starts mid-word where to start reading, and **File offset**
fills in. The model knows 28 instruction sets -- MIPS, PowerPC, RISC-V, SPARC,
z/Architecture, 68000, Hexagon, LoongArch, MSP430, AVR, Xtensa, eBPF, Alpha,
PA-RISC and SuperH besides the five NeverD decodes. `scripts/generate_isa_model.py`
trains it from code clang compiles for it and from Debian packages GCC built
(`scripts/isa_model_corpus.json` pins them by hash), and
`lib/loader/Raw/ISAModel.json` records how. `scripts/validate_isa_model.py`
runs the engine on real programs and libraries the model never saw -- Debian
packages for 19 architectures, OpenWrt builds for 10 targets, and builds for
ARC, IA-64 and the MIPS16e code OpenWrt builds busybox as, which the model
does not know -- each read whole and two bytes in: all 180 read right, those
of unknown sets settling nothing. The note under the list says what the bytes
showed ("Read from the bytes: MIPS big-endian (32-bit) 100% of the code, no
64-bit-only instructions"), or that they look like a set NeverD cannot decode
yet, like a family whose instructions do not tell its width, or like no code of
a set NeverD knows; then, as when nothing names one, the processor is the
user's to choose, and the one chosen last time is chosen again. A file that
opens with a Cortex-M vector table names Thumb, its entry point and where the
image sits, and the fields fill in. IDA, by comparison, reads every binary file
as its default processor until told otherwise. **Image base** is the address the bytes
map at, **File offset** and **Loading size** pick the bytes (the rest of the
file when the size is empty), and **Entry point** is where execution starts (the
image base when empty); all are hexadecimal. **Platform** is the platform the
code was built for, whose conventions decompiling follows: how calls pass
arguments, which registers they keep and the sizes of C types. It defaults to
**Detect from the code**: the engine reads it from where calls put their first
argument (`rdi` under System V, `rcx` under Windows), what prologues save and
spill, the thread block and system calls the code reaches, and the system
libraries its text names; when the code shows too little it assumes System V
and says so. IDA, by comparison, leaves a binary file's compiler unknown and
reads x86-64 arguments in System V order, so a Windows function gains phantom
parameters. The Output window reports the platform and the evidence ("Platform:
Windows, read from the code: calls set their first argument in rcx (212)"), and
the status line names it. The file opens as one read and execute segment,
`seg000`, with `start` at the entry point; idle-time function discovery finds
the functions its calls reach and, in x86-64 code, the functions it takes the
address of (`lea rdi, [rip+main]`). The engine keeps the choice, the detected
platform with it, in `<input>.neverd-load.json` (packed into the `.nddb`), so
the file reopens the same way without the dialog. The command line takes the
same choice with `--loader binary --processor aarch64 --load-base 0x80000`
(plus `--load-offset`, `--load-size`, `--load-entry` and `--platform
auto|sysv|windows|darwin`), or `--loader evm`, keeps it too, and `neverd info`
prints the platform and its evidence.

**File → Load file → Reload the input file** reads the input file again
through a fresh worker, as IDA's command does: the bytes are the file's as it
is now, and the saved names, comments and other annotations come back with it.
Unsaved changes are saved or discarded first, as the user chooses. A binary
file shows the dialog again, opened on the choice it was loaded with, so a
wrong processor, base or offset changes without starting over; a processor or
platform the engine read from the bytes is read again.

**Analysis → Enabled**
turns idle-time analysis (function discovery and the reference index) on or off
for the file, **Indicator enabled** shows or hides the status line's analysis
indicator, and **Options → Load debug information** reads the PDB, DWARF or
linker map that belongs to the input. Projects, recent files and files opened
from the command line skip the dialog. The command line lists the same rows with
`neverd identify <input>` (`--json` for the engine's `neverd_identify_json`).

## Databases

**File → Save** (Ctrl+W) packs the project into a NeverD database next to the
input: `ls` saves to `ls.nddb`. A database is one SQLite file holding the input
itself, its comments, renames, function edits, data items, operand formats, how
a binary file is read and edit history, and the workbench state (location, graph mode, bookmarks and desktop). Each save is a
single transaction, so a database is never left half written; the input is
stored in independently compressed chunks that compress and expand in parallel,
and an unchanged input is not rewritten.

Open a `.nddb` file directly to continue a project anywhere, even without the
original binary: the input is unpacked into a per-database working directory
and checked against its SHA-256 digest, and a damaged database is reported
instead of loaded. Opening a binary whose `.nddb` sits beside it restores the
saved location, bookmarks and desktop; when its comment files are missing they
are restored from the database, and a database that describes a different
version of the file is reported and left unused until the next save replaces
it. Closing the window updates the saved location of an existing database.

A binary in a folder you cannot write to, such as `/usr/bin` or a read-only
mount, opens all the same. IDA asks for another place for its database; the
workbench keeps the database, the comment files and the writer lock in the
user's data directory, beside a copy of the binary, and says where in the
output window. Opening or reloading the binary again finds them there, and a
copy whose original changed size or time is made again.

Every database carries the SQLite application id `NDDB` (`0x4E444442`) in its
header, so a renamed database still opens as a project and `file` tells it from
other SQLite files (`application id 1313096770`). Another application's SQLite
file is never read or written, even when it is named `.nddb`; it is reported as
not a NeverD database.

## Edits, history and analysis

Comments are staged and saved explicitly; renames, function edits and data
items commit at once (staged comments are saved first). Opening another file, restarting the
worker or quitting with unsaved comments offers Save, Discard and Cancel. Edits
prepared for a session that has since closed are refused, never applied to the
new one. Annotation, rename, function edit and data item commands have bounded undo/redo
history bound to the input hash and sidecar contents; a write-ahead journal
recovers an interrupted save, and foreign edits disable replay instead of
silently applying commands to another state. One worker owns a writable input
through an operating-system advisory lock.

**Edit → Functions → Create function** (P) starts a function at the cursor,
inside another function or in code nothing reaches; **Delete function** stops
treating the current function as one. The last edit at an address decides over
symbols, the function detector and analysis, and the edits are kept in
`<input>.neverd-functions.json`, which the command line reads too (`neverd
function-edits <input> --create <address>`, `--delete <address>`, `--list`).
A function edit drops whole-program analysis results: analysis continues
function by function until **Analyze** runs again.

**Edit → Rename** (N) names any address, as the listing and the pseudocode
show it: a name under the cursor renames what it denotes, otherwise the item
the cursor is on. A data name replaces `qword_A410` in its label, every operand
(`mov rdx, cs:pname`) and the C (`fprintf(stderr, "%s: %s\n", pname, msg)`). A
name has no spaces, leads to one address and is never an automatic name such
as `sub_1234`. Names are kept in `<input>.neverd-renames.json`, which the
command line reads and writes too (`neverd rename <input> --addr <address>
--to <name>`, `--clear`).

**Edit → Code** (C) defines native instructions from the selected byte through
one basic block, ending at a branch, return, unmodelled control transfer or
existing code. It uses the image's processor and instruction mode, validates
every instruction and refuses truncated bytes or overlaps with other defined
items. The bytes must be file-backed executable code. It does not create a
function; P does that separately. One press is one undo/redo step, with the
definitions kept in the item sidecar and `.nddb` database. Already defined code
is unchanged. C belongs to the disassembly; source windows keep their keys.

**Edit → Data** (D) makes the item under the cursor a value, and pressing it
again cycles the value through byte, word, dword and qword; **Edit → String**
(A) makes the string that starts there an item, read as the string scan reads
one; **Edit → Undefine** (U) shows the item's bytes as bytes, whatever
analysis reads in them. D or A inside undefined bytes takes just the bytes the
new item needs and leaves the rest undefined, and each press is one step of
undo history. Automatic code belongs to its function and cannot be made data
without deleting that function; code defined with C can be replaced or undefined. The items
are kept in `<input>.neverd-items.json`, which the command line reads and
writes too (`neverd items <input> --data <address> --size 4`, `--string
<address>`, `--undefine <address> --size <n>`, `--clear <address>`).

**Edit → Operand type** shows an instruction operand's number the way the user
picks, as IDA's keys do: **Hexadecimal** (Q), **Decimal** (H), **Binary** (B),
**Character** (R, `'ABCD'` when every byte is printable), **Offset** (O, the
name of the address the number points at), **Change sign** (`_`, the two's
complement with a minus) and **Bitwise negate** (`~`); **Number** (`#`) goes
back to the listing's own choice. The number under the cursor changes; with
the cursor elsewhere on the line, the line's last number does, and a line
without one is refused. Only x86 instructions show operand types so far. A form
the value cannot take, such as characters for unprintable bytes or an offset to
an address with no name, leaves the listing's number. Each change is one step
of undo history and is kept in `<input>.neverd-operands.json`, which the command
line reads and writes too (`neverd operands <input> --addr <address> --operand 1
--base decimal`, `--negate`, `--invert`, `--clear`; the address lies in
executable code).

Opening a file never starts whole-program analysis. The listing, function list,
references and graph come from the loader and from per-function work: a
decompile, graph or IR request analyzes only the function it names. While the
window is idle the worker first lets the engine add the functions its detector
finds from the image alone (call targets, prologues and format tables), so
binaries without unwind tables list their functions too, and then builds the
reference index in parallel across all cores; references and labels appear when
it finishes, and an explicit cross-reference request completes it at once.
**Options → Analysis → Whole-program analysis** runs the full pipeline when
wanted, and the listing then shows the switch jump tables it recovered as a
classic disassembler does: the table under its `jpt_` name with one slot per
line (`dd offset loc_164C0 - 27444h`, or `dq offset`/`dd rva` for absolute and
image-relative tables), `switch 54 cases` on the instruction that loads it,
`switch jump` on the dispatch, and on each target a code reference from the
dispatch and a data reference from the table. A table is laid out only when
the engine has checked that every slot holds its target. Cancel removes
queued work; a running engine call finishes unless the worker is restarted.

## Languages, extensions and MCP

The UI starts in English and bundles all 11 project languages; **Options →
Language** switches immediately without reloading the analysis, and Arabic
mirrors the window chrome while code and addresses stay left to right.

**View → Open subviews → Extensions** imports versioned JSON manifests with namespaced
read-only query contributions and runs them on the current address. Manifests
cannot run scripts or register arbitrary engine operations. See the manifest
schema in the [worker protocol](../tools/neverd-worker/PROTOCOL.md).

**View → Open subviews → MCP connections** starts connections only on
request. It supports local
stdio programs with an explicit argument list and Streamable HTTP with TLS
verification, optional bearer credentials and a custom CA file. Tool schemas,
arguments and results are inspectable, and the call history keeps parameters,
results and cancellation states. MCP is an interoperability client, not a model
provider. The standalone, Qt-free adapter uses MCP **2025-11-25** newline
JSON-RPC:

```sh
tools/neverd-mcp/neverd-mcp --worker /absolute/path/to/neverd-worker \
  --file /absolute/path/to/binary
```

To share the open GUI project, enable session sharing in MCP connections and
copy its credential-file path for:

```sh
tools/neverd-mcp/neverd-mcp --attach /absolute/path/to/credentials.json
```

Attachment queries the same worker and revision, never starts another project
writer, and ends when sharing or the GUI closes or the project changes. See
[MCP details](../tools/neverd-mcp/README.md).

## Packaging and qualification

`cmake --install build-gui --prefix dist` invokes Qt's deployment script for
the Widgets, Svg and Sql plugins. The matching engine and its dependencies must
be included in a distributable package. macOS can create an ad-hoc signed
development bundle with:

```sh
python3 tools/neverd-gui/package_macos.py --build-dir build-gui \
  --engine /absolute/path/to/libneverd.dylib \
  --qt-dir /path/to/Qt/6.8.3/macos --output dist/NeverD.app
```

This produces an ad-hoc signed development application, not a notarized
release. Test native dialogs, IME, accessibility, mixed-DPI screens and platform
packaging on each target platform before a public release; automated offscreen
tests do not prove those properties. `--startup-benchmark` writes startup
milestones from main entry to the first painted listing of the opened file; see
the [benchmark harness](../tools/neverd-gui/benchmarks/README.md).

See the [packaging guide](../tools/neverd-gui/PACKAGING.md) for dependency and
license inputs, and the [qualification record](gui-qualification.md) for
measured evidence and remaining release criteria.
