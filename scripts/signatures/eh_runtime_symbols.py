#!/usr/bin/env python3
"""The exception-handling runtime symbols NeverD wants signatures for.

Locating a personality routine is a name lookup: a CIE points at an address,
the loader asks the symbol table, the import table, and the relocations what
lives there, and classifies whatever they answer.  A stripped, statically
linked image answers nothing, so the routine is an address and the frames that
install it go uninterpreted.  Signatures are the way back in -- but only for
the routines the personality table already knows, because a name NeverD cannot
classify buys nothing at that address.

So this list is not "every function in the unwinder".  It is the set worth
carrying in a signature database:

  * the personalities themselves, which decide the schema a frame's language
    data is read with, and which are the whole point of the exercise;
  * the unwinder entry points a personality is written against, which is what
    tells a reader that an unnamed routine sits in the unwinder at all rather
    than in application code;
  * the Itanium ABI's throw/catch surface, which is what marks a landing pad
    as a catch rather than a cleanup.

Names are listed the way the source spells them.  Platform decoration is
matched separately, mirroring `symbolNameCandidates` in
lib/loader/language/LanguagePersonality.cpp, so a Darwin `___cxa_throw` and a
PE `__imp___CxxFrameHandler3` reach the same entry.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class SymbolFamily:
    name: str
    why: str
    exact: tuple[str, ...] = ()
    prefixes: tuple[str, ...] = ()
    # Matched anywhere in the symbol, for the one routine whose name may
    # arrive wrapped in a mangling.  Kept a separate field rather than folded
    # into the others so that a substring rule has to be argued for: it is
    # much weaker than an exact name and only earns its place where the loader
    # would classify the mangled spelling too.
    substrings: tuple[str, ...] = ()


FAMILIES: tuple[SymbolFamily, ...] = (
    SymbolFamily(
        name="itanium-personality",
        why=(
            "the routines a DWARF CIE installs; which one a frame names is "
            "what separates a C++ frame from a cleanup-only C one, and the "
            "`_sj0`/`_seh0` spellings say which dispatch mechanism carries it"
        ),
        exact=(
            "__gxx_personality_v0",
            "__gxx_personality_seh0",
            "__gxx_personality_sj0",
            "__gcc_personality_v0",
            "__gcc_personality_seh0",
            "__gcc_personality_sj0",
        ),
    ),
    SymbolFamily(
        name="arm-ehabi-personality",
        why=(
            "the three routines an ARM EHABI index entry names by number; a "
            "linked image also carries them as symbols, and a stripped one "
            "carries neither"
        ),
        exact=(
            "__aeabi_unwind_cpp_pr0",
            "__aeabi_unwind_cpp_pr1",
            "__aeabi_unwind_cpp_pr2",
        ),
    ),
    SymbolFamily(
        name="rust-personality",
        why=(
            "Rust shares the Itanium LSDA with C++, so the personality is the "
            "only thing in the tables that says a cleanup pad runs Drop glue "
            "rather than a destructor block.  It is emitted as an ordinary "
            "Rust symbol, so a build that did not export it unmangled spells "
            "it as a mangled path ending in the same name -- which "
            "`classifyPersonalityName` demangles and accepts, so the database "
            "carries it too"
        ),
        exact=(
            "rust_eh_personality",
            "rust_eh_personality_catch",
        ),
        substrings=("rust_eh_personality",),
    ),
    SymbolFamily(
        name="objc-personality",
        why=(
            "which Objective-C personality an image installs is decided by the "
            "runtime it was built against, and that choice is the whole of how "
            "its type table is read"
        ),
        exact=(
            "__objc_personality_v0",
            "__gnu_objc_personality_v0",
            "__gnu_objc_personality_seh0",
            "__gnu_objc_personality_sj0",
            "__gnustep_objc_personality_v0",
            "__gnustep_objcxx_personality_v0",
        ),
    ),
    SymbolFamily(
        name="ada-personality",
        why=(
            "GNAT unwinds through the Itanium tables but fills the type table "
            "with Ada exception entities, so a positive filter there names "
            "something that must not be followed as RTTI"
        ),
        exact=(
            "__gnat_personality_v0",
            "__gnat_personality_sj0",
            "__gnat_personality_seh0",
            "__gnat_personality_imp",
        ),
    ),
    SymbolFamily(
        name="d-personality",
        why=(
            "three D compilers emit one set of tables and differ only in what "
            "they call the personality, so the personality is what tells them "
            "apart"
        ),
        exact=(
            "__dmd_personality_v0",
            "_d_eh_personality",
            "__gdc_personality_v0",
            "__gdc_personality_sj0",
            "__gdc_personality_seh0",
            "__gdc_personality_imp",
        ),
    ),
    SymbolFamily(
        name="windows-personality",
        why=(
            "the Windows table and registration models; a static CRT link "
            "leaves these unnamed exactly as a static libstdc++ link does"
        ),
        exact=(
            "__C_specific_handler",
            "__CxxFrameHandler",
            "__CxxFrameHandler2",
            "__CxxFrameHandler3",
            "__CxxFrameHandler4",
            "__GSHandlerCheck",
            "__GSHandlerCheck_SEH",
            "__GSHandlerCheck_EH",
            "__GSHandlerCheck_EH4",
            "_except_handler3",
            "_except_handler4",
            "_except_handler4_common",
            "_GCC_specific_handler",
        ),
    ),
    SymbolFamily(
        name="unwinder",
        why=(
            "the interface every personality is written against.  A frame "
            "does not name these, but a routine that calls them is in the "
            "unwinder, which is what makes an unnamed personality plausible "
            "before its own signature confirms it"
        ),
        prefixes=(
            "_Unwind_",
            "__unw_",
        ),
    ),
    SymbolFamily(
        name="itanium-cxx-abi",
        why=(
            "the throw and catch surface the LSDA's action table dispatches "
            "into; naming these is what turns a landing pad from `some code "
            "the unwinder jumps to` into a catch, a rethrow, or a cleanup"
        ),
        prefixes=("__cxa_",),
        exact=(
            "_ZSt9terminatev",
            "_ZSt10unexpectedv",
            "_ZSt13get_terminatev",
            "_ZSt14get_unexpectedv",
        ),
    ),
    SymbolFamily(
        name="objc-throw",
        why=(
            "the Objective-C counterpart of the Itanium throw surface; an "
            "Objective-C pad is an Itanium pad whose runtime calls are these"
        ),
        exact=(
            "objc_exception_throw",
            "objc_exception_rethrow",
            "objc_begin_catch",
            "objc_end_catch",
            "objc_exception_try_enter",
            "objc_exception_try_exit",
            "objc_exception_extract",
            "objc_exception_match",
        ),
    ),
    SymbolFamily(
        name="rust-panic",
        why=(
            "the entry points a Rust landing pad reaches; they identify the "
            "panic strategy an image was built with, which decides whether "
            "its cleanup pads run at all"
        ),
        exact=(
            "rust_begin_unwind",
            "rust_panic",
            "__rust_start_panic",
            "__rust_drop_panic",
            "__rust_foreign_exception",
        ),
    ),
    SymbolFamily(
        name="d-throw",
        why="druntime's C-linkage throw entry points, the D analogue of __cxa_throw",
        exact=(
            "_d_throw_exception",
            "_d_throwdwarf",
            "_d_throwc",
        ),
    ),
    SymbolFamily(
        name="go-seh",
        why=(
            "the only personality the Go linker emits, on windows/amd64 and "
            "only for the cgo landing pad; the `.abi0` suffix is how the Go "
            "assembler spells the pre-register ABI"
        ),
        exact=(
            "runtime.sehtramp",
            "runtime.sehtramp.abi0",
        ),
    ),
)

EXACT_NAMES: frozenset[str] = frozenset(
    name for family in FAMILIES for name in family.exact
)

NAME_PREFIXES: tuple[str, ...] = tuple(
    prefix for family in FAMILIES for prefix in family.prefixes
)

NAME_SUBSTRINGS: tuple[str, ...] = tuple(
    substring for family in FAMILIES for substring in family.substrings
)


def name_candidates(name: str) -> list[str]:
    """Every spelling of one C symbol, most decorated first.

    Mirrors `symbolNameCandidates` in the loader.  A leading underscore is
    ambiguous -- Darwin adds one to every C symbol, while `_except_handler3`
    owns its own -- so rather than guess which is decoration, every reading is
    offered and the caller keeps whichever the list knows.
    """

    candidates = [name]

    def add(candidate: str) -> None:
        if candidate and candidate not in candidates:
            candidates.append(candidate)

    base = name
    for prefix in ("DW.ref.", "__imp_"):
        if base.startswith(prefix):
            base = base[len(prefix) :]
            add(base)
    at = base.find("@")
    if at > 0:
        base = base[:at]
        add(base)
    if len(base) > 1 and base.startswith("_"):
        add(base[1:])
    return candidates


def is_eh_runtime_symbol(name: str) -> bool:
    """True when a signature for \\p name is worth carrying."""

    for candidate in name_candidates(name):
        if candidate in EXACT_NAMES:
            return True
        if candidate.startswith(NAME_PREFIXES):
            return True
    return any(substring in name for substring in NAME_SUBSTRINGS)


def coverage_report() -> str:
    lines = ["exception-handling runtime coverage", ""]
    for family in FAMILIES:
        lines.append(f"{family.name}: {family.why}")
        for name in family.exact:
            lines.append(f"    {name}")
        for prefix in family.prefixes:
            lines.append(f"    {prefix}*")
        for substring in family.substrings:
            lines.append(f"    *{substring}*")
        lines.append("")
    lines.append(
        f"{len(EXACT_NAMES)} exact names, {len(NAME_PREFIXES)} prefix families, "
        f"{len(NAME_SUBSTRINGS)} substring rules"
    )
    return "\n".join(lines)


if __name__ == "__main__":
    print(coverage_report())
