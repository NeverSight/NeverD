//===- LocalTypes.h - The named types of a program --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A program's named types: the structures, unions, enumerations, typedefs
/// and function types its debug information declares or a user writes, as
/// IDA's Local Types window keeps them.  A type is held as its source
/// declares it: anonymous unions and structures stay in place, and every
/// member keeps its offset in bits from the record that holds it.  One
/// definition therefore prints the same layout in C, C++, Rust and Go
/// (TypeRender.h).
///
/// Debug information decides offsets.  A PDB or DWARF type keeps the
/// offsets its compiler chose; MSVC flattens anonymous unions and structures
/// into their parent, and nestFlatMembers() rebuilds them around those
/// offsets.  Sizes follow the library's data model, which decides how wide
/// long and a pointer are.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_LOCALTYPES_H
#define NEVERD_TYPES_LOCALTYPES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::types {

/// How wide long and a pointer are.
enum class DataModel : uint8_t {
  LLP64, ///< 64-bit Windows: 32-bit long, 64-bit pointers.
  LP64,  ///< 64-bit Unix: 64-bit long and pointers.
  ILP32, ///< 32-bit targets: 32-bit int, long and pointers.
};

/// How a compiler lays a record out.
enum class LayoutRules : uint8_t {
  /// Microsoft's: a bitfield shares a storage unit only with the bitfields
  /// of its type's size before it.
  MSVC,
  /// The Itanium ABI's, as GCC and Clang outside Windows: a bitfield goes
  /// wherever it fits without crossing a unit of its type.
  Itanium,
};

/// How a target's compilers lay records out, beyond what its data model
/// says (RecordABIs.def).
enum class RecordABI : uint8_t {
#define NEVERD_RECORD_ABI(Name, Spelling, Rules, ScalarAlignLimit) Name,
#include "neverd/types/RecordABIs.def"
};

/// Where \p ABI's bit-fields go.
LayoutRules layoutRulesOf(RecordABI ABI);
/// The largest alignment a builtin scalar takes in a record of \p ABI.
uint32_t scalarAlignLimitOf(RecordABI ABI);
/// \p ABI's name in the C API.
llvm::StringRef recordABIName(RecordABI ABI);
/// The ABI a data model's compilers follow when nothing else says:
/// Microsoft's for LLP64 and ILP32, the Itanium ABI's for LP64.
RecordABI defaultRecordABI(DataModel Model);

/// What a builtin type is.
enum class BuiltinClass : uint8_t { Void, Bool, Character, Integer, Float };

/// The builtin types (BuiltinTypes.def).
enum class Builtin : uint8_t {
#define NEVERD_BUILTIN_TYPE(Name, Class, Signed, LLP64, LP64, ILP32, Keywords, \
                            Windows)                                           \
  Name,
#include "neverd/types/BuiltinTypes.def"
};

/// What BuiltinTypes.def records about a builtin type.
struct BuiltinInfo {
  BuiltinClass Class;
  bool Signed;
  uint8_t Bytes[3]; ///< By DataModel.
  const char *Keywords;
  const char *Windows;
};

const BuiltinInfo &builtinInfo(Builtin Type);
unsigned builtinBytes(Builtin Type, DataModel Model);
unsigned pointerBytes(DataModel Model);

/// Whether the Windows headers name \p Type by a typedef of their own, as
/// ULONG and HRESULT: the upper-case names of BuiltinTypes.def's Windows
/// column.  VOID is a macro there, and the column's other names are C's and
/// C++'s own, such as char16_t.
bool windowsTypedef(Builtin Type);
/// The builtin the Windows headers' typedef \p Name names, if any.
std::optional<Builtin> windowsTypedefNamed(llvm::StringRef Name);

/// What a named type is.
enum class TypeKind : uint8_t { Struct, Union, Enum, Typedef, Function };

/// Where a named type comes from.
enum class TypeOrigin : uint8_t {
  PDB,    ///< A PDB's type records.
  DWARF,  ///< DWARF type entries.
  Header, ///< A type library NeverD ships, read from headers.
  User,   ///< Declared or edited by the user.
};

/// How a function takes its arguments (CallingConventions.def).
enum class CallingConvention : uint8_t {
#define NEVERD_CALLING_CONVENTION(Name, C, Rust, IDA, Wide, Pops) Name,
#include "neverd/types/CallingConventions.def"
};

struct RecordDef;
struct EnumDef;
struct FunctionDef;

/// What a type expression is.
enum class TypeExprKind : uint8_t {
  Builtin,  ///< A builtin type.
  Named,    ///< A type of the library, by name.
  Pointer,  ///< A pointer or C++ reference.
  Array,    ///< An array.
  Function, ///< A function type, as a function pointer points to.
  Record,   ///< An anonymous union or structure defined in place.
  Enum,     ///< An anonymous enumeration defined in place.
};

/// A type as a member, a parameter or a typedef refers to it.
struct TypeExpr {
  TypeExprKind Kind = TypeExprKind::Builtin;
  bool Const = false;
  bool Volatile = false;
  /// Builtin: which.
  Builtin TheBuiltin = Builtin::Void;
  /// Named: the library type's name, and what it is, so that C says struct.
  std::string Name;
  TypeKind NamedKind = TypeKind::Struct;
  /// Pointer: the pointee.  Array: the element.
  std::shared_ptr<const TypeExpr> Element;
  /// Pointer: its size in bytes, and whether it is a C++ reference.
  uint8_t PointerBytes = 8;
  bool Reference = false;
  /// Array: the element count; none for an array of unknown bound.
  std::optional<uint64_t> Count;
  /// Record, Enum and Function: the definition in place.
  std::shared_ptr<const RecordDef> Record;
  std::shared_ptr<const EnumDef> Enum;
  std::shared_ptr<const FunctionDef> Function;
};

using TypeExprRef = std::shared_ptr<const TypeExpr>;

TypeExprRef makeBuiltin(Builtin Type);
TypeExprRef makeNamed(std::string Name, TypeKind Kind);
TypeExprRef makePointer(TypeExprRef Pointee, unsigned Bytes,
                        bool Reference = false);
TypeExprRef makeArray(TypeExprRef Element, std::optional<uint64_t> Count);
TypeExprRef makeFunction(std::shared_ptr<const FunctionDef> Function);
TypeExprRef makeRecord(std::shared_ptr<const RecordDef> Record);
TypeExprRef makeEnum(std::shared_ptr<const EnumDef> Enum);
/// \p Type with const and volatile added.
TypeExprRef qualified(TypeExprRef Type, bool Const, bool Volatile);

/// A member of a union or structure.
struct Member {
  /// Empty for an anonymous union or structure, whose members are reached
  /// as if they were the parent's.
  std::string Name;
  TypeExprRef Type;
  /// From the start of the record that holds the member.
  uint64_t BitOffset = 0;
  /// A bitfield's width; zero for other members.  A bitfield's type is that
  /// of the storage it lives in.
  uint32_t BitWidth = 0;
  /// Where a bitfield's storage unit begins, in bits from the record that
  /// holds the member; a packed record need not align it.
  uint64_t StorageBitOffset = 0;
  std::string Comment;

  bool isBitfield() const { return BitWidth != 0; }
  bool isAnonymous() const {
    return Name.empty() && Type && Type->Kind == TypeExprKind::Record;
  }
};

/// A union or structure.
struct RecordDef {
  bool Union = false;
  /// In bytes.
  uint64_t Size = 0;
  /// In bytes; zero when its members decide.
  uint32_t Alignment = 0;
  /// The #pragma pack it was laid out under; zero for none.
  uint32_t Pack = 0;
  /// False for a type only declared, whose members are unknown.
  bool Defined = true;
  std::vector<Member> Members;
};

/// A value of an enumeration.
struct Enumerator {
  std::string Name;
  /// Its bits; the enumeration's underlying type says whether it is signed.
  uint64_t Value = 0;
  std::string Comment;
};

/// An enumeration.
struct EnumDef {
  Builtin Underlying = Builtin::Int;
  /// Its values are bits to combine rather than choices.
  bool Flags = false;
  std::vector<Enumerator> Values;
};

/// A parameter of a function type.
struct Parameter {
  std::string Name;
  TypeExprRef Type;
};

/// A function type.
struct FunctionDef {
  CallingConvention Convention = CallingConvention::Unspecified;
  TypeExprRef Return;
  std::vector<Parameter> Params;
  bool Variadic = false;
  /// A prototype of a function that never returns (_Noreturn,
  /// __declspec(noreturn), __attribute__((noreturn))).
  bool NoReturn = false;
};

/// A named type of the library.
struct LocalType {
  /// Its number in the library, kept while it lives, as IDA's ordinals are.
  uint32_t Ordinal = 0;
  std::string Name;
  TypeKind Kind = TypeKind::Struct;
  TypeOrigin Origin = TypeOrigin::User;
  /// Struct and Union.
  std::shared_ptr<const RecordDef> Record;
  /// Enum.
  std::shared_ptr<const EnumDef> Enum;
  /// Typedef: what it names.
  TypeExprRef Target;
  /// Function.
  std::shared_ptr<const FunctionDef> Function;
  std::string Comment;
};

/// The named types of a program.
class TypeLibrary {
public:
  explicit TypeLibrary(DataModel Model = DataModel::LLP64)
      : Model(Model), ABI(defaultRecordABI(Model)) {}
  /// Copies are whole libraries of their own, which share the definitions,
  /// so that a change can be tried on one and kept or dropped.
  TypeLibrary(const TypeLibrary &Other);
  TypeLibrary &operator=(const TypeLibrary &Other);
  TypeLibrary(TypeLibrary &&) = default;
  TypeLibrary &operator=(TypeLibrary &&) = default;

  DataModel dataModel() const { return Model; }
  /// How the program's compilers laid its records out, which parsing lays
  /// declarations out by and every rendering follows: a 32-bit Linux
  /// program's i386 System V ABI, which its data model alone does not say.
  /// Its data model's unless set.
  RecordABI recordABI() const { return ABI; }
  void setRecordABI(RecordABI Laid) { ABI = Laid; }
  LayoutRules layoutRules() const { return layoutRulesOf(ABI); }

  /// Defines \p Type, which replaces the type of its name and keeps that
  /// type's ordinal.  Returns the ordinal.
  uint32_t define(LocalType Type);
  /// Removes the type named \p Name; false when there is none.
  bool remove(llvm::StringRef Name);

  const LocalType *find(llvm::StringRef Name) const;
  const LocalType *byOrdinal(uint32_t Ordinal) const;
  /// The named type \p Name names, through any typedefs.
  const LocalType *resolve(llvm::StringRef Name) const;
  /// Every type, in ordinal order.
  std::vector<const LocalType *> types() const;
  size_t size() const { return ByName.size(); }

  /// The size of \p Type in bytes; none for a function, an array of unknown
  /// bound, void, or a name the library lacks.
  std::optional<uint64_t> sizeOf(const TypeExpr &Type) const;
  /// The alignment of \p Type in bytes, as the data model lays it out.
  uint32_t alignOf(const TypeExpr &Type) const;
  /// The alignment of a record's members together.
  uint32_t alignOf(const RecordDef &Record) const;
  /// The alignment \p Type is declared with, itself or through a member
  /// (__declspec(align), alignas): Microsoft's compiler lowers it for no
  /// #pragma pack.  Zero for none.
  uint32_t requiredAlignOf(const TypeExpr &Type) const;

  /// The prototype of the function named \p Name, as a header declares it,
  /// with its parameters' names; replaces one of that name.
  void definePrototype(std::string Name,
                       std::shared_ptr<const FunctionDef> Function);
  const FunctionDef *findPrototype(llvm::StringRef Name) const;
  /// Every prototype, by function name.
  const llvm::StringMap<std::shared_ptr<const FunctionDef>> &
  prototypes() const {
    return Prototypes;
  }

private:
  DataModel Model;
  RecordABI ABI;
  llvm::StringMap<std::shared_ptr<const FunctionDef>> Prototypes;
  /// By ordinal - 1; null where a type was removed.
  std::vector<std::unique_ptr<LocalType>> ByOrdinal;
  llvm::StringMap<uint32_t> ByName;
};

/// A member as MSVC lists it: anonymous unions and structures flattened into
/// the record that holds them, with offsets from that record.
struct FlatMember {
  std::string Name;
  TypeExprRef Type;
  uint64_t BitOffset = 0;
  /// A bitfield's width, zero otherwise.
  uint32_t BitWidth = 0;
  /// A bitfield's storage unit, or the member, in bits from the record.
  uint64_t StorageBitOffset = 0;
  uint64_t StorageBits = 0;
  /// How the member aligns, which the record's packing may lower.
  uint32_t Alignment = 1;
};

/// Rebuilds the anonymous unions and structures of a record whose members
/// MSVC flattened.  Members that begin where an earlier one began are the
/// alternatives of a union; a member that begins inside such a union, its
/// padding up to its alignment included, belongs to its last alternative,
/// which becomes a structure; a run of bitfields that share a storage unit
/// is one alternative.  The nesting is the smallest that lays the members
/// out as \p Flat does.  Anonymous records take the record's \p Pack.
std::vector<Member> nestFlatMembers(llvm::ArrayRef<FlatMember> Flat, bool Union,
                                    uint32_t Pack = 0);

/// \p Record's members with its anonymous unions and structures flattened,
/// as MSVC lists them: the inverse of nestFlatMembers().
std::vector<FlatMember> flattenMembers(const RecordDef &Record);

/// Why \p Record's members cannot sit where they say, or empty when they
/// can: a member reaching past the record that holds it, a member of a
/// structure overlapping the one before it, or an alternative of a union
/// that does not begin with the union.
std::string layoutProblem(const TypeLibrary &Library, const RecordDef &Record);

} // namespace neverd::types

#endif // NEVERD_TYPES_LOCALTYPES_H
