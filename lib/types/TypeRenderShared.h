//===- TypeRenderShared.h - What every type language shares -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What the renderers of every language share: offset comments, names
/// that step around a language's keywords, and where a language's own
/// layout would put a member, so that a filler can move it to the offset
/// the library records.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TYPES_TYPERENDERSHARED_H
#define NEVERD_LIB_TYPES_TYPERENDERSHARED_H

#include "neverd/types/LocalTypes.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace neverd::types::render {

/// Hex digits that show every offset of a record of \p Size bytes.
unsigned offsetDigits(uint64_t Size);

/// A byte offset as a comment shows it: 0x1A0 padded to \p Digits, or a
/// decimal +416.
std::string offsetText(uint64_t Byte, const RenderOptions &Options,
                       unsigned Digits);

/// A bitfield's place in its storage unit: "bit 3" or "bits 4..63".
std::string bitText(uint64_t BitInUnit, uint32_t Width);

/// \p Name made safe from \p Lang's keywords.
std::string escapedName(Language Lang, llvm::StringRef Name);

/// How one language spells a library's names.  A name that is an identifier
/// keeps its spelling, with a keyword escaped; any other takes `_` for each
/// character the language does not allow (`std::pair<int,int>` as
/// `std__pair_int_int_`).  Types and enumerators share one namespace, as they
/// do in C++ and Go: an enumerator whose name a type or an earlier enumerator
/// took is prefixed with its enumeration's name, and a type whose spelling
/// another type took gets a number.
class NameSpelling {
public:
  NameSpelling(const TypeLibrary &Library, Language Lang);

  Language language() const { return Lang; }
  /// The spelling of the type named \p Name.
  std::string type(llvm::StringRef Name) const;
  /// The spelling of \p Enum's enumerator \p Index.
  std::string enumerator(const EnumDef &Enum, size_t Index) const;
  /// The spelling of a member, a parameter or an accessor named \p Name.
  std::string member(llvm::StringRef Name) const;
  /// The names spelled otherwise than they are, with their spellings, in
  /// the library's order.
  const std::vector<std::pair<std::string, std::string>> &renamed() const {
    return Renamed;
  }

private:
  std::string identifier(llvm::StringRef Name, bool TypeName) const;
  void enumeration(const EnumDef &Enum, llvm::StringRef Owner,
                   llvm::StringSet<> &Taken);
  void inlineEnums(const RecordDef &Record, llvm::StringRef Owner,
                   llvm::StringSet<> &Taken);

  Language Lang;
  llvm::StringMap<std::string> Types;
  std::map<const EnumDef *, std::vector<std::string>> Enumerators;
  std::vector<std::pair<std::string, std::string>> Renamed;
};

/// \p Code followed by "// Comment" at the comment column, or the comment
/// \p Marker the language starts one with.
std::string withComment(std::string Code, llvm::StringRef Comment,
                        const RenderOptions &Options,
                        llvm::StringRef Marker = "//");

uint64_t alignTo(uint64_t Value, uint64_t Alignment);

/// Where a language that lays a structure's members out one after another
/// would put the next member, and how many bytes a filler needs to put it
/// where the library records it instead.
class SequentialLayout {
public:
  explicit SequentialLayout(uint32_t Pack) : Pack(Pack) {}

  struct Step {
    /// Bytes an explicit filler must take before the member.
    uint64_t Filler = 0;
    /// Bytes the language pads with on its own before the member.
    uint64_t Padding = 0;
  };

  /// Places a member of \p Size and \p Alignment at \p Byte.
  Step place(uint64_t Byte, uint64_t Size, uint32_t Alignment);
  /// Closes a record of \p Size and \p Alignment.
  Step finish(uint64_t Size, uint32_t Alignment) const;
  uint64_t end() const { return End; }
  /// Counts the bytes up to \p Byte as taken, as bit-fields packed into
  /// them take them.
  void cover(uint64_t Byte) { End = std::max(End, Byte); }

private:
  uint32_t Pack;
  uint64_t End = 0;
};

/// One place of a record laid out for a language without anonymous records
/// or bitfields, as Rust and Go are.
struct Slot {
  enum class Kind : uint8_t {
    Field,     ///< A member; an anonymous record has a generated type.
    Bitfields, ///< A run of bitfields in one storage unit.
    Filler,    ///< Bytes that move the next place to its offset.
  };
  Kind TheKind = Kind::Field;
  /// Field: the member.  Bitfields: the run, in order.
  const Member *Field = nullptr;
  std::vector<const Member *> Bits;
  /// Bitfields: the storage unit's type.
  TypeExprRef Storage;
  /// In bytes from the record that holds the place.
  uint64_t Byte = 0;
  /// Filler: its length in bytes.
  uint64_t Size = 0;
  /// Bytes the language pads with on its own before the place.
  uint64_t PaddingBefore = 0;
  /// The field's name: the member's, Anonymous for an anonymous record,
  /// _bitfield for a run, as windows-rs names them.
  std::string Name;
  /// Field: the generated name of an anonymous record's or enumeration's
  /// type, and the bounds of the arrays of it, outermost first.
  std::string NestedType;
  std::vector<uint64_t> Bounds;
};

/// A record laid out in slots, with its anonymous records, which take the
/// names <Name>_0, <Name>_1 and so on.
struct RecordPlan {
  std::string Name;
  const RecordDef *Record = nullptr;
  uint32_t Pack = 0;
  std::vector<Slot> Slots;
  uint64_t TailFiller = 0;
  uint64_t TailPadding = 0;
  std::vector<RecordPlan> Nested;
  /// Anonymous enumerations, by their generated names.
  std::vector<std::pair<std::string, const EnumDef *>> Enums;
};

/// \p Record laid out as \p Name under \p Pack, which its anonymous records
/// share unless they have their own.
RecordPlan planRecord(const TypeLibrary &Library, const RecordDef &Record,
                      std::string Name, uint32_t Pack,
                      const NameSpelling &Names);

/// How a bitfield of \p Type reads: the bytes of its storage unit, and
/// whether its value is signed, which C extends from its top bit.
struct BitStorage {
  unsigned Bytes = 0;
  bool Signed = false;
};
BitStorage bitStorage(const TypeLibrary &Library, const TypeExpr &Type);

/// One accessor's arithmetic on a run's unsigned storage, as Rust and Go
/// spell it alike: the field's bits, sign-extended when the field is signed.
struct BitAccess {
  unsigned UnitBits = 0;
  uint64_t Shift = 0;
  uint32_t Width = 0;
  bool Signed = false;
  /// The mask of the field's bits, before shifting, in hexadecimal.
  std::string Mask;
};
BitAccess bitAccess(const TypeLibrary &Library, const Member &Field);

/// \p Type's innermost element through arrays, with their bounds.
const TypeExpr &arrayElement(const TypeExpr &Type,
                             std::vector<uint64_t> &Bounds);

/// The named types \p Roots need, closed over: by value and, when
/// \p ThroughPointers, behind pointers too.  Every type when \p Roots is
/// empty.  In ordinal order.
std::vector<const LocalType *>
neededTypes(const TypeLibrary &Library, llvm::ArrayRef<const LocalType *> Roots,
            bool ThroughPointers);

/// The named types \p Type holds by value, which C needs complete first.
void valueDependencies(const TypeExpr &Type, std::vector<std::string> &Names);
void valueDependencies(const LocalType &Type, std::vector<std::string> &Names);

/// Every named type \p Type mentions, through pointers too.
void mentionedTypes(const TypeExpr &Type, std::vector<std::string> &Names);
void mentionedTypes(const LocalType &Type, std::vector<std::string> &Names);

/// Spells types as C or C++ declares them, with one name table for every
/// type it spells.
class CSpeller {
public:
  CSpeller(const TypeLibrary &Library, const RenderOptions &Options);
  ~CSpeller();
  /// \p Type declaring \p Declarator, or alone when that is empty.
  std::string declare(const TypeExpr &Type,
                      const std::string &Declarator = std::string()) const;

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

/// The renderers of each language.
std::string renderC(const TypeLibrary &Library, const LocalType &Type,
                    const RenderOptions &Options);
std::string renderCHeader(const TypeLibrary &Library,
                          llvm::ArrayRef<const LocalType *> Types,
                          const RenderOptions &Options);
EmbeddedC renderEmbeddedC(const TypeLibrary &Library,
                          llvm::ArrayRef<const LocalType *> Roots,
                          const RenderOptions &Options);
std::string renderRust(const TypeLibrary &Library, const LocalType &Type,
                       const RenderOptions &Options);
std::string renderRustModule(const TypeLibrary &Library,
                             llvm::ArrayRef<const LocalType *> Types,
                             const RenderOptions &Options);
EmbeddedSource renderRustEmbedded(const TypeLibrary &Library,
                                  llvm::ArrayRef<const LocalType *> Defined,
                                  llvm::ArrayRef<const LocalType *> Declared,
                                  const RenderOptions &Options);
std::string renderGo(const TypeLibrary &Library, const LocalType &Type,
                     const RenderOptions &Options);
std::string renderGoFile(const TypeLibrary &Library,
                         llvm::ArrayRef<const LocalType *> Types,
                         const RenderOptions &Options);
EmbeddedSource renderGoEmbedded(const TypeLibrary &Library,
                                llvm::ArrayRef<const LocalType *> Defined,
                                llvm::ArrayRef<const LocalType *> Declared,
                                const RenderOptions &Options);
std::string renderPython(const TypeLibrary &Library, const LocalType &Type,
                         const RenderOptions &Options);
std::string renderPythonModule(const TypeLibrary &Library,
                               llvm::ArrayRef<const LocalType *> Types,
                               const RenderOptions &Options);
std::string renderJavaScript(const TypeLibrary &Library, const LocalType &Type,
                             const RenderOptions &Options);
std::string renderJavaScriptModule(const TypeLibrary &Library,
                                   llvm::ArrayRef<const LocalType *> Types,
                                   const RenderOptions &Options);
std::string renderLua(const TypeLibrary &Library, const LocalType &Type,
                      const RenderOptions &Options);
std::string renderLuaModule(const TypeLibrary &Library,
                            llvm::ArrayRef<const LocalType *> Types,
                            const RenderOptions &Options);

} // namespace neverd::types::render

#endif // NEVERD_LIB_TYPES_TYPERENDERSHARED_H
