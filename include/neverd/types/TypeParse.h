//===- TypeParse.h - Local types from C and C++ source ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Local types read from C or C++ declarations: structures, unions and
/// enumerations with their members, typedefs, and the prototypes of
/// functions, laid out as the compiler of the library's data model would.
/// A user's declarations and the headers NeverD ships read alike, and so
/// does what the renderers write, so a type converts from C to any language
/// they render and back.
///
/// Every builtin spelling the renderers write is read: ULONG, unsigned int,
/// unsigned long and uint32_t.  #pragma pack, __declspec(align),
/// DECLSPEC_ALIGN and alignas set packing and alignment;
/// _Static_assert and static_assert of sizeof and offsetof are checked
/// against the layout.  Other preprocessor directives are ignored but for
/// macros that name a single word.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_TYPEPARSE_H
#define NEVERD_TYPES_TYPEPARSE_H

#include "neverd/types/LocalTypes.h"
#include "neverd/types/TypeLayout.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace neverd::types {

/// A problem with the source, where it is: lines and columns count from 1.
struct SourceProblem {
  unsigned Line = 0;
  unsigned Column = 0;
  std::string Message;
};

struct ParseOptions {
  /// C or C++.
  Language Lang = Language::C;
  /// What the types defined record as their origin.
  TypeOrigin Origin = TypeOrigin::User;
  /// C and C++: keep the declarations that read when others do not, each
  /// problem still reported, as a generator reading a system's headers
  /// does.  A problem the lexer finds, such as a #pragma pack it cannot
  /// read, still changes nothing: the layouts after it are not known.
  bool KeepGoing = false;
  /// Identifier projections recorded by the emitter for this declaration.
  /// Only type positions use these aliases; arbitrary identifiers do not.
  const llvm::StringMap<std::string> *TypeNames = nullptr;
};

struct ParseResult {
  /// The types and prototypes defined, in source order.
  std::vector<std::string> Types;
  std::vector<std::string> Prototypes;
  std::vector<SourceProblem> Problems;
  bool ok() const { return Problems.empty(); }
};

/// Reads the types and prototypes \p Source declares into \p Library.  A
/// type of a name the library has replaces it, keeping its ordinal.  The
/// library changes only when the whole source reads without a problem.
ParseResult parseCTypes(llvm::StringRef Source, TypeLibrary &Library,
                        const ParseOptions &Options = {});

/// Reads the #[repr(C)] structures and unions, type aliases, constants and
/// extern "C" functions \p Source declares, as the Rust renderer writes
/// them: a record named <Parent>_<n> that its parent holds is in place, a
/// _bitfield field's doc comment lists its bitfields, and an integer alias
/// with constants of its type is an enumeration.
ParseResult parseRustTypes(llvm::StringRef Source, TypeLibrary &Library,
                           const ParseOptions &Options = {});

/// Reads the structs, defined types, constants and accessor methods
/// \p Source declares, as the Go renderer writes them: a struct of raw
/// storage whose methods return pointers to it is a union of what they
/// return, a _bitfield field's comment lists its bitfields, a byte field
/// Name_ that method Name reads is of the type it returns, and "packed to N"
/// in a type's comment is its packing.
ParseResult parseGoTypes(llvm::StringRef Source, TypeLibrary &Library,
                         const ParseOptions &Options = {});

/// Reads \p Source in the language \p Options names.
ParseResult parseTypes(llvm::StringRef Source, TypeLibrary &Library,
                       const ParseOptions &Options = {});

/// One function's C declaration, as IDA's Y takes it.
struct FunctionDeclaration {
  /// The function it names, and its type.
  std::string Name;
  std::shared_ptr<const FunctionDef> Function;
  std::vector<SourceProblem> Problems;
  bool ok() const { return Function && Problems.empty(); }
};

/// Reads \p Source as the declaration of one function, such as
/// `int f(struct S *s, int n)`, its semicolon optional, against the types
/// \p Library holds, which it does not change.  A declaration that names a
/// type the library lacks, or defines one, is a problem: the types it uses
/// are the library's.
FunctionDeclaration parseFunctionDeclaration(llvm::StringRef Source,
                                             const TypeLibrary &Library,
                                             const ParseOptions &Options = {});

} // namespace neverd::types

#endif // NEVERD_TYPES_TYPEPARSE_H
