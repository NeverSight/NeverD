//===- NeverDCAPILocalTypes.h - C API local types -----------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The named types of a loaded program, as IDA's Local Types window lists
/// them: read from its PDB or its DWARF when first asked for, laid out with
/// every member's offset, and rendered as C, C++, Rust or Go source.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_LOCALTYPES_H
#define NEVERD_SDK_CAPI_LOCALTYPES_H

#include "neverd/sdk/NeverDCAPITypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef NEVERD_EXPORTS
#define NEVERD_API __declspec(dllexport)
#else
#define NEVERD_API __declspec(dllimport)
#endif
#else
#define NEVERD_API __attribute__((visibility("default")))
#endif

/// The program's local types in brief:
/// {"source":"pdb"|"dwarf"|"none","dataModel":"LLP64"|"LP64"|"ILP32",
/// "recordABI":"microsoft"|"itanium"|"i386-sysv"|"aapcs32",
/// "stats":{"structs","unions","enums","declaredOnly","nestingMismatches":
/// [name,...]},"types":[{"ordinal","name","kind","size","members",
/// "origin"},...]}.  "recordABI" says how the program's compilers laid its
/// records out, which declarations are laid out by and the renderings
/// follow: Microsoft's for a Windows program, else its architecture's
/// System V ABI, whose bit-fields go as the Itanium ABI's and whose 32-bit
/// x86 scalars align to four.  "kind" is struct, union, enum, typedef or
/// function;
/// "size" is absent for a type without one, and "members" counts a record's
/// named members, anonymous records' included, or an enumeration's values.
/// "stats" adds "origins", how many types each origin gave: "pdb", "dwarf",
/// "header" for a header library NeverD ships, "user" for the user's
/// declarations; "headers" lists the libraries joined as
/// [{"name","types","prototypes","conflicts":[name,...]} | {"name","error"}].
/// "skipped" lists the types of the DWARF left out, with why
/// (["name: reason",...]), and "conflicts" the names two of its different
/// types share, of which the first stays; a PDB leaves both empty.  An image
/// with neither a PDB nor DWARF has none.  When a declaration of the user's
/// no longer reads over the program's types, "declarationProblems" lists
/// what it met: [{"declaration" (its index in
/// neverd_type_declarations_json), "line","column","message"},...].
/// OptionsJson may be NULL or {"filter":"text","origin":"pdb"|"dwarf"|
/// "header"|"user","language":"c"|"cpp"|"rust"|"go"}: "filter" keeps the
/// types whose name holds the text, ignoring the case of ASCII letters,
/// "origin" those of one origin, and "language" adds
/// "renamed":[[name,spelling],...], the names that language cannot spell as
/// they are.  NULL with neverd_last_error() when no binary is loaded, the
/// options are malformed or the debug information's types cannot be read.
NEVERD_API const char *neverd_local_types_json(neverd_session_t Sess,
                                               const char *OptionsJson);

/// The local type \p Name in full: {"ordinal","name","kind","origin"} and, for
/// a record, "size", "alignment", "pack", "declaredAlignment", "defined" and
/// "members"; for an enumeration "size", "underlying", "flags" and
/// "values":[{"name","value"},...]; for a typedef "target"; for a function
/// type "declaration".  Each member has "kind" (field, bitfield, struct or
/// union for a record in place, padding), "name" (empty when anonymous),
/// "offset" in bytes and "bitOffset" in bits from the outermost record,
/// "size" in bytes, "type" as C spells it, "refers" (the local type it
/// names through pointers and arrays, if any), a bitfield's "bitWidth" and
/// "storageOffset", and a record in place's "members".  OptionsJson may be
/// NULL or {"spelling":"portable"|"windows"|"keywords"}, how "type" spells
/// builtins, portable unless asked.  NULL with neverd_last_error() when there
/// is no such type or the options are malformed.
NEVERD_API const char *neverd_local_type_json(neverd_session_t Sess,
                                              const char *Name,
                                              const char *OptionsJson);

/// Local types as source.  OptionsJson may be NULL or
/// {"language":"c"|"cpp"|"rust"|"go"|"python"|"javascript"|"lua",
/// "spelling":"portable"|"windows"|"keywords","offsets":bool,
/// "hexOffsets":bool,"asserts":bool,"types":[name,...],"dependencies":bool}.
/// Without "types", every type renders as one unit that compiles; with
/// them, those types do, and with "dependencies" (the default) every type
/// they need, else each alone for reading.  "asserts" adds checks of every
/// size and offset that the compiler verifies, that Python's ctypes do as
/// the module runs, or, in JavaScript and Lua, that every accessor stays
/// within its record.  "spelling" names builtins: every language takes
/// portable, exact widths (uint32_t, u32, uint32, ctypes.c_uint32, number),
/// unless asked, and windows, which Rust, Go, Python, JavaScript and Lua
/// declare as aliases of exact widths as wdk-sys does (pub type ULONG =
/// u32); C and C++ take keywords (unsigned long) too.  NULL
/// with neverd_last_error() for an unknown type, a spelling the language
/// does not take or other malformed options.
NEVERD_API const char *neverd_local_types_source(neverd_session_t Sess,
                                                 const char *OptionsJson);

/// Adds the types \p Source declares to the program's local types: C, C++,
/// Rust or Go as OptionsJson's "language" says ("c" when NULL), which may
/// replace types of their names.  Returns {"types":[name,...],
/// "prototypes":[name,...],"problems":[{"line","column","message"},...]}:
/// with any problem, nothing changes.  The declaration joins the user's,
/// which are read again over the program's types whenever those are read
/// anew, as after loading a PDB; neverd_type_declarations_save keeps them.
/// NULL with neverd_last_error() when no binary is loaded or the options
/// are malformed.
NEVERD_API const char *neverd_local_types_declare(neverd_session_t Sess,
                                                  const char *Source,
                                                  const char *OptionsJson);
/// Removes the types the JSON array \p NamesJson names from the program's
/// local types, the program's own as well as the user's.  Returns
/// {"removed":[name,...],"usedBy":[name,...]}: the types that still name a
/// removed one keep their layout and name it as a type the program lacks.
/// The removal joins the user's declarations.  NULL with neverd_last_error()
/// when no binary is loaded, the names are malformed, or one names no type.
NEVERD_API const char *neverd_local_types_forget(neverd_session_t Sess,
                                                 const char *NamesJson);
/// Converts declarations between C, C++, Rust and Go without a program.
/// \p RequestJson is {"dataModel":"LLP64"|"LP64"|"ILP32","recordABI"?:
/// "microsoft"|"itanium"|"i386-sysv"|"aapcs32","declarations":
/// [{"language":"c"|"cpp"|"rust"|"go","source"},...],"keepGoing"?:bool,
/// "render"?:{...as neverd_local_types_source's options, and
/// "prototypes":bool}}: the declarations read in order into one library
/// laid out for the data model, as the record ABI lays records out (the
/// data model's own unless named), then render as asked.  Returns {"types",
/// "prototypes","problems":[{"declaration","line","column","message"},...],
/// "source"?}: with a problem nothing renders unless "keepGoing" keeps the
/// C and C++ declarations that read.  {"error"} when the request is
/// malformed.  Free with neverd_free_string.
NEVERD_API const char *neverd_types_convert(const char *RequestJson);

/// The languages local types render and read in, and how each names
/// builtins: {"languages":[{"id","display","spelling","spellings":[id,...],
/// "reads":bool,"prototypes":bool},...]}, "spelling" the one a language
/// takes unless asked, "reads" whether its declarations read back, and
/// "prototypes" whether it renders function prototypes.  Needs no program.
/// Free with neverd_free_string.
NEVERD_API const char *neverd_types_languages_json(void);

/// The user's declarations in the order made:
/// [{"language","source"} | {"forget":[name,...]}, ...].
NEVERD_API const char *neverd_type_declarations_json(neverd_session_t Sess);
/// Write or read the declarations as `<input>.neverd/types.json`, which
/// neverd_session_load reads too.  Reading them reads the program's types
/// anew.  Return 0, or -1 with neverd_last_error.
NEVERD_API int neverd_type_declarations_save(neverd_session_t Sess);
NEVERD_API int neverd_type_declarations_load(neverd_session_t Sess);

/// The local types a pointer may point at, best first, judged by what the
/// code does through it, as an analyst checks a guess against the code.
/// RequestJson is {"accesses":[{"offset","size","use":"read"|"write"|
/// "address"},...], "function":"0x..", "target":{"address":"0x.."} or
/// {"parameter":N}, "text", "records":bool, "shifts":bool, "limit":N,
/// "spelling"}, every field optional:
/// - "accesses": what the code does at each offset from where the pointer
///   points; "size" is zero for an address taken.
/// - "target" adds the accesses the code of "function" makes through the
///   pointer the memory operand of the instruction at "address" goes
///   through (or, for an address it computes, as `lea` does, the pointer
///   it adds to), read from the function's MedIR; or through its parameter
///   N (from 0, as its declaration lists it, neverd_func_type_json), from
///   the integer register its calling convention passes it in.
/// - "text" matches type names and the typedefs of a record and of a
///   pointer to it, as an editor's quick open matches; "Type.member" also
///   matches member paths, ".member" those of every record.
/// - "records" (default true) keeps structures and unions, each with its
///   typedefs as aliases; false lists every type under its own name, a
///   typedef judged by the record it names or points at.
/// - "shifts" (default true) lets the pointer point into a record when
///   accesses reach before it, as CONTAINING_RECORD undoes.
/// - "limit": at most this many candidates (default 50, at most 500).
/// Returns {"accesses":[{"offset","size","use"},...], "target":{"base",
/// "offset","size","use" or "parameter","accesses":[{"address","offset",
/// "size","use"},...]}?,
/// "candidates":[{"name","kind","origin","size","record"?,
/// "aliases"?,"pointerAliases"?,"shift"?,"shiftPath"?,"exact",
/// "exactElements","partial","padding","outside","fits":[{"fit","path",
/// "memberOffset","memberSize"?,"type"?,"bitfield"?},...]?,"matchedName"?,
/// "matched"?,"matchedMember"?,"memberMatched"?},...], "total"}:
/// "accesses" are every access judged, the request's and then the
/// target's, which each candidate's "fits" follow; the target's "base" is
/// the register the pointer is in, when it is one, and "offset", "size"
/// and "use" the instruction's own access.  "fit" is exact, start, inside,
/// padding or outside, and a match is [[begin, length],...] in bytes of
/// the matched text.  NULL with neverd_last_error() when no binary is
/// loaded, the request is malformed, the instruction reaches no memory
/// through a pointer, or the parameter does not arrive in an integer
/// register.
NEVERD_API const char *neverd_local_types_fit_json(neverd_session_t Sess,
                                                   const char *RequestJson);

/// Where \p Size bytes at \p Offset land in the record \p Name names,
/// through typedefs, as the listing names a structure offset:
/// {"record","size","fit","path","memberOffset","memberSize"?,"type"?,
/// "bitfield"?}, "record" and "size" the record's and the rest as
/// neverd_local_types_fit_json's fits say.  OptionsJson may be NULL or
/// {"spelling"}.  NULL with neverd_last_error() when no defined structure
/// or union has that name.
NEVERD_API const char *neverd_local_type_member_json(neverd_session_t Sess,
                                                     const char *Name,
                                                     long long Offset,
                                                     unsigned long long Size,
                                                     const char *OptionsJson);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_LOCALTYPES_H
