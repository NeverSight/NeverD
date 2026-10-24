# Automatic signature selection

`neverd sigs INPUT --auto --sig-base signatures` selects pattern files before
opening, parsing or mapping their cache entries. The same `SignatureDB`
selection is used by the SDK and GUI. Explicit `--sig-file` and `--sig-dir`
loads retain their caller-selected contents.

Selection proceeds through three independent dimensions:

1. The image format, architecture and pointer width choose the existing
   `pe`, `elf` or `macho` tree directory.
2. `loader/ImageProfile` classifies the declared operating system, execution
   domain and C runtime using metadata already retained by the loader.
   `sigs/SignatureScopes.def` describes reviewed bundled libraries. A file is
   skipped only when a known image dimension contradicts its recorded scope.
3. A checksum-valid PE Rich header selects the existing `vs<year>` release
   files. If the release is unavailable, the platform-compatible candidates
   remain. Parts of a library share its selection.

| Image | Evidence used |
| --- | --- |
| Windows user program | GUI/console subsystem or user runtime imports |
| Windows kernel/driver | Native subsystem plus system-image flag, or kernel provider imports |
| Windows CE / UEFI | Their distinct PE subsystems |
| Android native image | Android identification note or exact Android linker path |
| Linux / FreeBSD | Explicit OS ABI, GNU/FreeBSD ABI note, or a recognized runtime |
| glibc / musl | Recognized interpreter; glibc can also be named by `libc.so.6` |
| Linux kernel module | ELF relocatable object with module descriptor and terminated `vermagic` record |
| macOS / iOS / simulator | Mach-O build platform or supported legacy minimum-version command |
| Apple kernel extension | Mach-O `MH_KEXT_BUNDLE` file type |

PE native subsystem alone cannot distinguish a driver from a native user
process. System V ELF alone does not establish Linux, glibc or user mode.
An ordinary object, an unmarked static image or stripped kernel may therefore
remain unknown. Conflicting metadata keeps the candidates available. Neither
the filename nor an address heuristic supplies platform evidence. This
classification describes build targets; it does not prove the current CPU
privilege level or authorize execution. `user`/`kernel` cover the intended
ring 3/ring 0 distinction on x86 without applying ring terminology to ARM.

WDK and Windows SDK packs contain both platform-specific code and common
helpers. WDK also contains user-mode libraries. Their aggregate files have
no kernel/user exclusion. Common MSVC, Intel math, Watcom and libgcc helpers
are not discarded merely because an image uses another compiler or is a
kernel. GCC/Clang producer strings do not identify the version of every
statically linked library, so they do not select an exact runtime release.
Unknown library names remain eligible; adding a scope requires reviewing the
contents of that exact bundled library, not its filename prefix.
Linux and Android share a kernel family: runtime evidence separates bionic,
glibc and musl packs, while common libgcc helpers remain eligible. A Linux ABI
note alone does not establish which of those user environments an image uses.

`neverd info INPUT --json` reports `target_profile`, including evidence,
conflicts and usable Visual Studio years. The text view shows system, domain
and runtime. After automatic loading, the session's header JSON additionally
reports `signature_selection` with available, selected and skipped file
counts. The text `sigs` command shows these counts too; its existing JSON
match-array contract is unchanged. A failed load preserves the previous
database and its selection statistics; successful manual changes clear them.

`NeverDSignatureTests` covers unknown/conflicting inputs, mixed WDK contents,
split packs, unavailable MSVC releases, custom names, Linux module metadata,
and real PE/ELF/Mach-O header readers. A deliberately malformed excluded
pattern proves automatic selection happens before pattern I/O. The PE reader
test includes restricted loads that omit the original file buffer. Malformed
or duplicate `PT_INTERP` records fail in the ELF loader.
`NeverDSessionSignatureProfileTests` exercises the public SDK from a real PE
header through automatic selection, JSON reporting and manual-load reset.

Format references: [Microsoft PE specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format),
[Linux module reader](https://github.com/torvalds/linux/blob/master/kernel/module/main.c),
and [Apple Mach-O declarations](https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h).
