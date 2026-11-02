//===- TypeRender.h - Local types as C, C++, Rust and Go --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Local types as source: C and C++ as their headers would declare them,
/// Rust as #[repr(C)] items and Go as structs, every member marked with its
/// offset.  Each language lays the type out exactly as the library records
/// it.  Where its own rules would place a member elsewhere, an explicit
/// filler moves it; a language without unions or bitfields reads them
/// through storage of their size and accessors.
///
/// A type renders alone for reading, or with every type it needs for a
/// header that compiles, optionally followed by checks of each size and
/// offset that the compiler verifies.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_TYPERENDER_H
#define NEVERD_TYPES_TYPERENDER_H

#include "neverd/types/HeaderLibrary.h"
#include "neverd/types/LocalTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::types {

/// The languages local types render in (TypeLanguages.def).
enum class Language : uint8_t {
#define NEVERD_TYPE_LANGUAGE(Name, Id, Display, Spelling) Name,
#include "neverd/types/TypeLanguages.def"
};

/// How APIs name \p Lang, such as "rust".
llvm::StringRef languageId(Language Lang);
/// Every language's id, as a message lists them: "c, cpp, ... or python".
std::string languagesText();
/// The language an API names, if any.
std::optional<Language> languageNamed(llvm::StringRef Id);

/// How rendered source names builtin types (BuiltinSpellings.def).
enum class BuiltinSpelling : uint8_t {
#define NEVERD_BUILTIN_SPELLING(Name, Id) Name,
#include "neverd/types/BuiltinSpellings.def"
};

/// How APIs name \p Spelling, such as "windows".
llvm::StringRef spellingId(BuiltinSpelling Spelling);
/// The spelling an API names, if any.
std::optional<BuiltinSpelling> spellingNamed(llvm::StringRef Id);
/// Whether \p Lang names builtins as \p Spelling.
bool spellsBuiltins(Language Lang, BuiltinSpelling Spelling);
/// How \p Lang names builtins unless asked.
BuiltinSpelling defaultSpelling(Language Lang);
/// The spellings \p Lang takes, its default first (TypeLanguages.def).
std::vector<BuiltinSpelling> spellingsOf(Language Lang);
/// The spellings \p Lang takes, by their ids: "portable or windows".
std::string spellingsText(Language Lang);
/// Every spelling, by its id: "portable, windows or keywords".
std::string spellingsText();

struct RenderOptions {
  Language Lang = Language::C;
  /// How builtins are named; the language's default when unset, or when
  /// the language does not take it.
  std::optional<BuiltinSpelling> Spelling;
  /// Portable C prototypes retain integer keyword identity when the host has
  /// the target width; otherwise they select the target's exact-width type.
  bool PrototypeIntegerIdentity = false;
  /// A comment with each member's offset.
  bool Offsets = true;
  /// Offsets in hexadecimal rather than decimal.
  bool HexOffsets = true;
  /// After each record, checks of its size and member offsets that the
  /// compiler verifies.
  bool Asserts = false;
  /// The column offset comments start at.
  unsigned CommentColumn = 64;
  /// When every type renders, the library's function prototypes after them,
  /// in the languages rendersPrototypes says.
  bool Prototypes = false;
  /// C and C++ headers: the library of a system's headers
  /// (HeaderLibrarySystems.def) whose types, and the prototypes it gives,
  /// come from those headers where the compiler has them, and are defined
  /// only where it does not.
  std::optional<HeaderLibrary> SystemLibrary;
  /// C and C++ headers: each type after the system header that declares it
  /// (#pragma neverd_header), as the libraries NeverD ships keep it.
  bool HeaderPragmas = false;
};

/// C that another unit holds, as a decompiled function's does: the
/// definitions of the records it names and of what they hold by value, the
/// typedefs and enumerations these name, and declarations of the records
/// they only point to.  Each type sits under a guard of its own, so that
/// units holding the same types concatenate.  Where RenderOptions names the
/// library of the system's headers, a type that library gives and lays out
/// as the program does comes from its header, which the C includes, with
/// checks of the program's layout.
struct EmbeddedC {
  std::string Text;
  /// The records Text defines, by their C names, and whether each is a
  /// union.
  std::vector<std::pair<std::string, bool>> Records;
  /// The records Text only declares, which it points to, likewise.
  std::vector<std::pair<std::string, bool>> Declared;
  /// The records Text has from the system's headers, which lay each out as
  /// the program does with its members' names, likewise.
  std::vector<std::pair<std::string, bool>> HeaderRecords;
  /// The ordinary identifiers Text declares, the names of typedefs and of
  /// enumerators, which the unit's own names step around.
  std::vector<std::string> Identifiers;
  /// The library's types Text holds, by their names.
  std::vector<std::string> Types;
  /// The library's records Text only declares, by their names.
  std::vector<std::string> DeclaredTypes;
  /// The library's types taken from the system headers instead of defined.
  std::vector<std::string> HeaderTypes;
  /// The records the system's headers name by a typedef alone, which Text
  /// takes from them (`typedef struct {...} pthread_mutex_t;`), by their C
  /// names: a unit spells each by its name and declares no typedef of it,
  /// which would contradict the header's.
  std::vector<std::string> HeaderTypedefs;
};
/// \p Roots, records, as another unit holds them, in C: \p Options gives
/// the spelling, offsets and checks.
EmbeddedC renderEmbeddedC(const TypeLibrary &Library,
                          llvm::ArrayRef<const LocalType *> Roots,
                          const RenderOptions &Options);

/// How code reaches a member that C names on its own, directly or through
/// the anonymous records that hold it, in a language that spells the record
/// otherwise: the fields, and the methods that return a pointer to a
/// member, that lead to it, the member's own last.  A bit-field, which both
/// languages read and write through methods of the record that holds its
/// storage, has the hops that lead to that record, none for the outer one,
/// and the methods' names.
struct MemberPath {
  struct Hop {
    std::string Name;
    /// A method that returns a pointer to the member, as Go reads a union's
    /// members and those it cannot align.
    bool Accessor = false;
  };
  std::vector<Hop> Hops;
  /// A bit-field's methods: one returns its value, the other takes the
  /// value to store.  Empty for other members.
  std::string Getter, Setter;
};

/// Types as a Rust or Go text holds them beside code that uses them, as a
/// decompiled function's pseudocode does: no module header (crate
/// attributes, package, imports).
struct EmbeddedSource {
  std::string Text;
  /// For each record Text defines, by its name, how code reaches each
  /// member it names in C, by the member's name.  A member the language
  /// reads only through a method that takes more has none.
  std::map<std::string, std::map<std::string, MemberPath>> Members;
};
/// \p Defined as \p Options.Lang, Rust or Go, defines them, and \p Declared,
/// records the text only points to, as opaque types: as the language
/// spells the types a C unit holds (EmbeddedC::Types and its declared
/// records).
EmbeddedSource renderEmbeddedSource(const TypeLibrary &Library,
                                    llvm::ArrayRef<const LocalType *> Defined,
                                    llvm::ArrayRef<const LocalType *> Declared,
                                    const RenderOptions &Options);

/// Whether \p Lang renders function prototypes: C and C++ do.
bool rendersPrototypes(Language Lang);
/// Whether \p Lang's declarations read back (TypeParse.h): C, C++, Rust
/// and Go's do; Python, JavaScript and Lua render only.
bool readsDeclarations(Language Lang);
/// How the workbench shows \p Lang, such as "Rust".
llvm::StringRef languageDisplay(Language Lang);

/// The spelling \p Options names builtins in.
BuiltinSpelling spellingOf(const RenderOptions &Options);

/// \p Type's declaration, alone.
std::string renderType(const TypeLibrary &Library, const LocalType &Type,
                       const RenderOptions &Options);

/// \p Type in C or C++ as \p Options spell builtins, declaring
/// \p Declarator, or alone when that is empty: `struct S *`, or
/// `int f(struct S *s)` for a function type and its name.
std::string spellType(const TypeLibrary &Library, const TypeExpr &Type,
                      const std::string &Declarator,
                      const RenderOptions &Options);

namespace render {
class CSpeller;
class NameSpelling;
} // namespace render

/// Spells types as spellType does, the library's names spelled once for
/// every type it spells: a library a system's headers declare holds tens of
/// thousands.  It reads \p Library, which must outlive it and stay as it is.
class TypeSpeller {
public:
  TypeSpeller(const TypeLibrary &Library, const RenderOptions &Options);
  ~TypeSpeller();
  std::string spell(const TypeExpr &Type,
                    const std::string &Declarator = std::string()) const;

private:
  std::unique_ptr<render::CSpeller> Speller;
};

/// Renders records as renderEmbeddedC does, the library's names spelled once
/// for every call: each unit a session decompiles embeds a few types of a
/// library that can hold tens of thousands.  It reads \p Library, which must
/// outlive it and stay as it is.
class EmbeddedCRenderer {
public:
  EmbeddedCRenderer(const TypeLibrary &Library, const RenderOptions &Options);
  ~EmbeddedCRenderer();
  EmbeddedC render(llvm::ArrayRef<const LocalType *> Roots) const;

private:
  const TypeLibrary &Library;
  RenderOptions Options;
  std::unique_ptr<render::NameSpelling> Names;
};

/// \p Types and every type they need, in an order that compiles, after what
/// the spelling needs declared.  Every type when \p Types is empty.
std::string renderTypes(const TypeLibrary &Library,
                        llvm::ArrayRef<const LocalType *> Types,
                        const RenderOptions &Options);

/// The names \p Lang cannot spell as they are, such as `std::pair<int,int>`
/// or an enumerator another enumeration has too, each with the identifier
/// rendered source calls it by.  In the library's order.  C's are those of
/// the C that C++ reads too, which steps around C++'s keywords as well.
std::vector<std::pair<std::string, std::string>>
renamedNames(const TypeLibrary &Library, Language Lang);

/// The names of the types \p Type mentions: its members', target's and
/// parameters' types, through pointers, arrays and records in place.
std::vector<std::string> mentionedTypeNames(const LocalType &Type);

/// The names of the types \p Type holds by value, whose layouts its own
/// depends on: through arrays and records in place, not pointers.
std::vector<std::string> valueDependencyNames(const LocalType &Type);

} // namespace neverd::types

#endif // NEVERD_TYPES_TYPERENDER_H
