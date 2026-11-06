//===- LocalTypesDebugContext.cpp - Debug info from local types -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LocalTypesDebugContext.h"

#include "neverd/Limits.h"
#include "neverd/debug/UserPrototype.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/libc/LibCStartup.h"
#include "neverd/loader/FunctionNames.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/sdk/Limits.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"

#include <limits>
#include <map>
#include <set>

namespace neverd::sdk {
namespace {
#define LOCAL_TYPE_SOURCE_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "LocalTypesSourceValues.def"

/// Whether emitted C takes the type \p Name from the headers it includes
/// (CHeaderNames.def) rather than declaring it.
bool fromCHeader(llvm::StringRef Name) {
#define NEVERD_C_HEADER_INTEGER(Header, Bits, Signed)                          \
  if (Name == Header)                                                          \
    return true;
#define NEVERD_C_HEADER_NAMED_TYPE(Header, Bits)                               \
  if (Name == Header)                                                          \
    return true;
#include "neverd/backend/c/dialect/CHeaderNames.def"
  return false;
}

bool isDisplayName(llvm::StringRef Name) {
  return !Name.empty() &&
         (llvm::isAlpha(Name.front()) || Name.front() == '_') &&
         llvm::all_of(Name,
                      [](char C) { return llvm::isAlnum(C) || C == '_'; });
}

/// The enumerators of \p Enum, of \p Bytes, a display names its values by,
/// as a PDB's enumerations hold them: none negative, none wider than 16
/// bits.
std::vector<std::pair<uint16_t, std::string>>
displayedEnumerators(const types::EnumDef &Enum, unsigned Bytes) {
  std::vector<std::pair<uint16_t, std::string>> Shown;
  const unsigned Bits = Bytes * 8;
  for (const types::Enumerator &Value : Enum.Values) {
    if (types::builtinInfo(Enum.Underlying).Signed && Bits &&
        (Value.Value >> (Bits - 1) & 1))
      continue;
    if (Value.Value > std::numeric_limits<uint16_t>::max())
      continue;
    Shown.emplace_back(uint16_t(Value.Value), Value.Name);
  }
  return Shown;
}

/// The records a session shares (LocalTypesDebugContext::libraryRecord): a
/// conversion names each record by its one node instead of converting it
/// again.  Pending holds the records named and not yet converted.
struct SharedRecordBuild {
  llvm::StringMap<TypeRef> &Records;
  std::vector<std::pair<TypeRef, const types::RecordDef *>> Pending;
};

class Converter {
public:
  explicit Converter(const types::TypeLibrary &Library,
                     const types::TypeSpeller *PrototypeSpeller = nullptr,
                     bool CompleteValues = false, bool BorrowRecords = false,
                     SharedRecordBuild *Shared = nullptr)
      : Library(Library), PrototypeSpeller(PrototypeSpeller),
        CompleteValues(CompleteValues), BorrowRecords(BorrowRecords),
        Shared(Shared) {}

  /// Converts the members of \p Named, a record a shared conversion named,
  /// as record() converts those of a record it names.
  void fillShared(NdType &Named, const types::RecordDef &Record) {
    if (Record.Defined)
      members(Named, Record, 0, 0);
  }
  /// The scalars of \p Named, a record a shared conversion filled, as
  /// record() finds them once the records it holds have theirs.
  void sharedLeaves(NdType &Named, const types::RecordDef &Record) {
    if (Record.Defined)
      leaves(Named, Record, 0);
  }

  TypeRef convert(const types::TypeExpr &Type, unsigned Depth) {
    if (Depth > kMaxLocalTypeConversionDepth)
      return nullptr;
    switch (Type.Kind) {
    case types::TypeExprKind::Builtin:
      return builtin(Type.TheBuiltin);
    case types::TypeExprKind::Pointer: {
      // A pointer's own value is all a decompiled signature needs: what it
      // points to only names it, so a pointee it cannot describe is void.
      TypeRef Pointee =
          Type.Element
              ? (CompleteValues ? Converter(Library, PrototypeSpeller)
                                      .convert(*Type.Element, Depth + 1)
                                : convert(*Type.Element, Depth + 1))
              : NdType::makeVoid();
      if (!Pointee)
        Pointee = NdType::makeVoid();
      TypeRef Pointer = NdType::makePtr(Pointee);
      Pointer->Size = uint16_t(Type.PointerBytes);
      return Pointer;
    }
    case types::TypeExprKind::Array: {
      TypeRef Element =
          Type.Element ? convert(*Type.Element, Depth + 1) : nullptr;
      const std::optional<uint64_t> Size = Library.sizeOf(Type);
      if (!Element || !Size || *Size > std::numeric_limits<uint16_t>::max())
        return nullptr;
      return NdType::makeArray(Element, uint32_t(Type.Count.value_or(0)),
                               uint16_t(*Size));
    }
    case types::TypeExprKind::Function:
      return Type.Function ? function(*Type.Function, Depth) : nullptr;
    case types::TypeExprKind::Record:
      return Type.Record ? record("", *Type.Record, Depth) : nullptr;
    case types::TypeExprKind::Enum:
      return Type.Enum ? builtin(Type.Enum->Underlying) : nullptr;
    case types::TypeExprKind::Named:
      return named(Type.Name, Depth);
    }
    return nullptr;
  }

private:
  TypeRef builtin(types::Builtin Builtin) {
    const types::BuiltinInfo &Info = types::builtinInfo(Builtin);
    const uint16_t Bytes = uint16_t(Library.builtinBytes(Builtin));
    switch (Builtin) {
    case types::Builtin::Void:
      return NdType::makeVoid();
    case types::Builtin::Half:
    case types::Builtin::Float:
    case types::Builtin::Double:
    case types::Builtin::LongDouble:
      return NdType::makeFloat(Bytes);
    default:
      break;
    }
    if (!Bytes)
      return nullptr;
    // The C spellings the decompiler keeps for the character types, as it
    // reads them from a PDB.
    TypeRef Integer = NdType::makeInt(Bytes, Info.Signed);
    if (Builtin == types::Builtin::Char)
      Integer->SourceName = "char";
    else if (Builtin == types::Builtin::SChar)
      Integer->SourceName = "signed char";
    else if (Builtin == types::Builtin::Bool)
      Integer->SourceName = "bool";
    else if (Builtin == types::Builtin::HResult)
      Integer->SourceName = "HRESULT";
    // Equal machine widths do not imply compatible C pointer types. Use
    // the prototype's scalar spelling for its call conversions as well,
    // without adding the source object's const/volatile to machine values.
    if (PrototypeSpeller && Integer->SourceName.empty())
      Integer->SourceName =
          PrototypeSpeller->spell(*types::makeBuiltin(Builtin));
    return Integer;
  }

  TypeRef named(llvm::StringRef Name, unsigned Depth) {
    const types::LocalType *Type = Library.find(Name);
    if (!Type)
      return nullptr;
    switch (Type->Kind) {
    case types::TypeKind::Typedef: {
      TypeRef Target =
          Type->Target ? convert(*Type->Target, Depth + 1) : nullptr;
      // A type this conversion made, a scalar, a pointer or an enumeration,
      // keeps the typedef's name for display; a record is shared, and
      // keeps its tag's.
      if (!Target || (Target->Kind == NdTypeKind::Struct && !Target->IsEnum))
        return Target;
      TypeRef Named = std::make_shared<NdType>(*Target);
      Named->TypedefName = Name.str();
      return Named;
    }
    case types::TypeKind::Enum:
      return Type->Enum ? enumeration(Name, *Type->Enum) : nullptr;
    case types::TypeKind::Function:
      return Type->Function ? function(*Type->Function, Depth) : nullptr;
    case types::TypeKind::Struct:
    case types::TypeKind::Union:
      if (BorrowRecords || ActiveRecords.contains(Name))
        return NdType::makeNamedRecord(Name.str(), 0);
      if (CompleteValues && Type->Kind == types::TypeKind::Union)
        return nullptr;
      if (!Type->Record)
        return nullptr;
      if (Shared)
        return sharedRecord(Name, *Type->Record);
      if (auto Done = Records.find(Name); Done != Records.end())
        return Done->second;
      return record(Name, *Type->Record, Depth);
    }
    return nullptr;
  }

  /// A named enumeration as debug information gives one: a record of its
  /// integer's size and sign whose display fields are its enumerators, by
  /// value, so that the decompiler names the values it compares and
  /// stores.
  TypeRef enumeration(llvm::StringRef Name, const types::EnumDef &Enum) {
    TypeRef Integer = builtin(Enum.Underlying);
    if (!Integer || Integer->Kind != NdTypeKind::Int)
      return Integer;
    TypeRef Named = NdType::makeNamedRecord(Name.str(), Integer->Size,
                                            /*Enum=*/true);
    Named->IsSigned = Integer->IsSigned;
    for (const auto &[Value, Enumerator] :
         displayedEnumerators(Enum, Library.builtinBytes(Enum.Underlying))) {
      Named->FieldDisplayNames.push_back(Enumerator);
      Named->FieldDisplayOffsets.push_back(Value);
    }
    return Named;
  }

  /// A function type, as a pointer to it is spelled: scalars and pointers
  /// retain their declared pointees; void where a part of it
  /// is neither, or for a variadic one, whose calls pass more.
  TypeRef function(const types::FunctionDef &Function, unsigned Depth) {
    if (Function.Variadic)
      return NdType::makeVoid();
    // A callback needs borrowed record identities, not member reconstruction
    // or recursive ownership through a record's own callback fields.
    Converter Borrowed(Library, PrototypeSpeller, false, true, Shared);
    const auto scalar = [&](const types::TypeExpr *Type) -> TypeRef {
      TypeRef Converted = Type ? Borrowed.convert(*Type, Depth + 1) : nullptr;
      if (!Converted)
        return nullptr;
      return Converted->Kind == NdTypeKind::Ptr ||
                     Converted->Kind == NdTypeKind::Int ||
                     Converted->Kind == NdTypeKind::Float
                 ? Converted
                 : nullptr;
    };
    TypeRef Return =
        Function.Return &&
                !(Function.Return->Kind == types::TypeExprKind::Builtin &&
                  Function.Return->TheBuiltin == types::Builtin::Void)
            ? scalar(Function.Return.get())
            : NdType::makeVoid();
    if (!Return)
      return NdType::makeVoid();
    std::vector<TypeRef> Params;
    for (const types::Parameter &Param : Function.Params) {
      TypeRef Converted = scalar(Param.Type.get());
      if (!Converted)
        return NdType::makeVoid();
      Params.push_back(std::move(Converted));
    }
    return NdType::makeFunc(std::move(Return), std::move(Params),
                            Function.Convention);
  }

  /// A record's node before its members: its name, size and alignment.
  TypeRef recordNode(llvm::StringRef Name, const types::RecordDef &Record) {
    if (Record.Size > std::numeric_limits<uint16_t>::max())
      return nullptr;
    TypeRef Named = NdType::makeNamedRecord(Name.str(), uint16_t(Record.Size));
    // Local types are C's records, whose calls pass and return them as C
    // does: no constructor sends a small one through memory.
    Named->Passing = NdRecordPassing::ByValue;
    const uint32_t Alignment = Library.alignOf(Record);
    if (Alignment <= std::numeric_limits<uint16_t>::max()) {
      Named->Alignment = static_cast<uint16_t>(Alignment);
      Named->FieldDisplayAlignment = static_cast<uint16_t>(Alignment);
    }
    return Named;
  }

  /// The shared node of the record \p Name; one not yet converted is
  /// converted after this conversion (SharedRecordBuild::Pending).
  TypeRef sharedRecord(llvm::StringRef Name, const types::RecordDef &Record) {
    if (const auto Found = Shared->Records.find(Name);
        Found != Shared->Records.end())
      return Found->second;
    TypeRef Named = recordNode(Name, Record);
    if (!Named)
      return nullptr;
    Shared->Records[Name] = Named;
    Shared->Pending.emplace_back(Named, &Record);
    return Named;
  }

  /// The scalars \p Named holds, through every member of \p Record, or
  /// none where one of them has no complete layout.
  void leaves(NdType &Named, const types::RecordDef &Record, unsigned Depth) {
    unsigned Budget = 256;
    bool Complete = true;
    for (const auto &Field : Record.Members) {
      if (!Field.Type || Field.isBitfield() || Field.BitOffset % 8 ||
          !scalarLeaves(Named, convert(*Field.Type, Depth + 1),
                        Field.BitOffset / 8, Budget)) {
        Complete = false;
        break;
      }
    }
    if (!Complete)
      Named.ScalarLeaves.clear();
  }

  /// A record by its name, its members named for display where they lie, as
  /// a PDB's flat member list gives them: through records in place, which
  /// have no names of their own.  Bitfields share their storage's bytes and
  /// are left out.
  TypeRef record(llvm::StringRef Name, const types::RecordDef &Record,
                 unsigned Depth) {
    TypeRef Named = recordNode(Name, Record);
    if (!Named)
      return nullptr;
    const uint32_t Alignment = Library.alignOf(Record);
    if (!Name.empty())
      Records[Name] = Named;
    // A recursive member refers to this identity, not ownership of the
    // record currently being built. A later member access may complete that
    // borrow from the library without a shared_ptr cycle.
    if (!Name.empty())
      ActiveRecords.insert(Name);
    const llvm::scope_exit Leave([&] {
      if (!Name.empty())
        ActiveRecords.erase(Name);
    });
    if (Record.Defined) {
      members(*Named, Record, 0, Depth);
      leaves(*Named, Record, Depth);
    }
    if (CompleteValues) {
      if (!Record.Defined)
        return nullptr;
      std::vector<TypeRef> Fields;
      std::vector<uint16_t> Offsets;
      for (const auto &Field : Record.Members) {
        if (!Field.Type || Field.isBitfield() ||
            Field.BitOffset % kSourceValueByteBits ||
            Field.BitOffset / kSourceValueByteBits > Record.Size)
          return nullptr;
        Fields.push_back(convert(*Field.Type, Depth + 1));
        Offsets.push_back(
            static_cast<uint16_t>(Field.BitOffset / kSourceValueByteBits));
      }
      const auto Natural = NdType::makeStruct(std::move(Fields));
      if (!Natural || Natural->Size != Record.Size ||
          Natural->Alignment != Alignment || Natural->FieldOffsets != Offsets)
        return nullptr;
      Named->Fields = Natural->Fields;
      Named->FieldOffsets = Natural->FieldOffsets;
      Named->FieldDisplayNames.clear();
      Named->FieldDisplayOffsets = Natural->FieldOffsets;
      Named->FieldDisplayTypes = Natural->Fields;
      for (const auto &Field : Record.Members)
        Named->FieldDisplayNames.push_back(Field.Name);
    }
    return Named;
  }

  /// Preserve the type library's complete byte layout for ABI classification,
  /// independently of display names. Bitfields and incomplete fields cannot
  /// become a partial scalar layout that accidentally admits a record.
  bool scalarLeaves(NdType &Into, const TypeRef &Part, uint64_t Offset,
                    unsigned &Budget) {
    if (!Part || !Part->Size || !Budget || Offset > Into.Size ||
        Part->Size > Into.Size - Offset)
      return false;
    --Budget;
    if (Part->Kind == NdTypeKind::Int || Part->Kind == NdTypeKind::Ptr ||
        Part->Kind == NdTypeKind::Float || Part->IsEnum) {
      Into.ScalarLeaves.push_back({static_cast<uint16_t>(Offset), Part->Size,
                                   Part->Kind == NdTypeKind::Float});
      return true;
    }
    if (Part->Kind == NdTypeKind::Array && Part->ElemType &&
        Part->ArrayCount <= Budget) {
      for (uint32_t I = 0; I < Part->ArrayCount; ++I)
        if (!scalarLeaves(Into, Part->ElemType,
                          Offset + uint64_t(I) * Part->ElemType->Size, Budget))
          return false;
      return Part->ArrayCount != 0;
    }
    if (Part->Kind != NdTypeKind::Struct || Part->ScalarLeaves.empty() ||
        Part->ScalarLeaves.size() > Budget)
      return false;
    Budget -= static_cast<unsigned>(Part->ScalarLeaves.size());
    for (const auto &Leaf : Part->ScalarLeaves) {
      if (!Leaf.Size || Leaf.Offset > Part->Size ||
          Leaf.Size > Part->Size - Leaf.Offset)
        return false;
      Into.ScalarLeaves.push_back({static_cast<uint16_t>(Offset + Leaf.Offset),
                                   Leaf.Size, Leaf.Floating});
    }
    return true;
  }

  void members(NdType &Into, const types::RecordDef &Record, uint64_t Base,
               unsigned Depth) {
    for (const types::Member &Field : Record.Members) {
      if (Into.FieldDisplayNames.size() >= limits::kMaxTpiDisplayFields)
        return;
      if (!Field.Type || Field.isBitfield())
        continue;
      const uint64_t Byte = (Base + Field.BitOffset) / 8;
      if (Byte > std::numeric_limits<uint16_t>::max())
        continue;
      if (Field.isAnonymous()) {
        members(Into, *Field.Type->Record, Base + Field.BitOffset, Depth);
        continue;
      }
      if (!isDisplayName(Field.Name))
        continue;
      Into.FieldDisplayOffsets.push_back(uint16_t(Byte));
      Into.FieldDisplayNames.push_back(Field.Name);
      Into.FieldDisplayTypes.push_back(memberType(*Field.Type, Depth + 1));
    }
  }

  /// A member's type.  A flexible array member's, an array of no stated
  /// length, is an array of its elements of no size, as DWARF's is: an
  /// access there reads an element (NdType::isFlexibleArrayAt).
  TypeRef memberType(const types::TypeExpr &Type, unsigned Depth) {
    if (Type.Kind == types::TypeExprKind::Array && !Type.Count && Type.Element)
      if (TypeRef Element = convert(*Type.Element, Depth + 1))
        return NdType::makeArray(std::move(Element), 0, 0);
    return convert(Type, Depth);
  }

  const types::TypeLibrary &Library;
  const types::TypeSpeller *PrototypeSpeller;
  bool CompleteValues;
  bool BorrowRecords;
  SharedRecordBuild *Shared;
  llvm::StringMap<TypeRef> Records;
  llvm::StringSet<> ActiveRecords;
};

/// The records \p Type holds in place, by name: through arrays, typedefs and
/// records of no name, never through a pointer.
void heldRecords(const types::TypeLibrary &Library, const types::TypeExpr &Type,
                 llvm::SmallVectorImpl<llvm::StringRef> &Held, unsigned Depth) {
  if (Depth > kMaxLocalTypeConversionDepth)
    return;
  switch (Type.Kind) {
  case types::TypeExprKind::Array:
    if (Type.Element)
      heldRecords(Library, *Type.Element, Held, Depth + 1);
    return;
  case types::TypeExprKind::Record:
    if (Type.Record)
      for (const types::Member &Field : Type.Record->Members)
        if (Field.Type)
          heldRecords(Library, *Field.Type, Held, Depth + 1);
    return;
  case types::TypeExprKind::Named:
    if (const types::LocalType *Named = Library.find(Type.Name)) {
      if (Named->Kind == types::TypeKind::Typedef && Named->Target)
        heldRecords(Library, *Named->Target, Held, Depth + 1);
      else if (Named->Kind == types::TypeKind::Struct ||
               Named->Kind == types::TypeKind::Union)
        Held.push_back(Named->Name);
    }
    return;
  default:
    return;
  }
}

/// Converts the records a session shares: a record and every record it
/// reaches, each once, as one node that the others name.  Each node's
/// members and scalars read the library and the names alone, whichever
/// record the session converted first.
class SharedRecordConversion {
public:
  SharedRecordConversion(const types::TypeLibrary &Library,
                         llvm::StringMap<TypeRef> &Records)
      : Library(Library), Build{Records, {}},
        Convert(Library, nullptr, false, false, &Build) {}

  /// The shared node of the library's record \p Local, with every record it
  /// reaches converted.
  TypeRef convert(const types::LocalType &Local) {
    const TypeRef Root =
        Convert.convert(*types::makeNamed(Local.Name, Local.Kind), 0);
    // Finding a record's scalars can name records no member did.
    while (fillPending(), !Unfinished.empty())
      finish(Unfinished.begin()->second.first);
    return Root;
  }

private:
  void fillPending() {
    while (!Build.Pending.empty()) {
      auto [Node, Def] = std::move(Build.Pending.back());
      Build.Pending.pop_back();
      Convert.fillShared(*Node, *Def);
      Unfinished.try_emplace(Node.get(), std::move(Node), Def);
    }
  }
  /// Finds \p Node's scalars after those of the records it holds in place.
  void finish(TypeRef Node) {
    const auto Found = Unfinished.find(Node.get());
    if (Found == Unfinished.end())
      return;
    const types::RecordDef &Def = *Found->second.second;
    // Taken first: a record that holds itself has no complete layout.
    Unfinished.erase(Found);
    llvm::SmallVector<llvm::StringRef, 8> Held;
    for (const types::Member &Field : Def.Members)
      if (Field.Type)
        heldRecords(Library, *Field.Type, Held, 0);
    for (llvm::StringRef Name : Held) {
      const types::LocalType *Type = Library.find(Name);
      if (!Type || !Type->Record)
        continue;
      const TypeRef Part =
          Convert.convert(*types::makeNamed(Type->Name, Type->Kind), 0);
      fillPending();
      if (Part)
        finish(Part);
    }
    Convert.sharedLeaves(*Node, Def);
  }

  const types::TypeLibrary &Library;
  SharedRecordBuild Build;
  Converter Convert;
  /// Records whose members are converted and whose scalars are not found.
  std::map<const NdType *, std::pair<TypeRef, const types::RecordDef *>>
      Unfinished;
};

/// Each node \p Type reaches through its parts, in \p Nodes.
void collectTypeNodes(const TypeRef &Type,
                      llvm::DenseSet<const NdType *> &Nodes) {
  llvm::SmallVector<const NdType *, 64> Work;
  if (Type)
    Work.push_back(Type.get());
  while (!Work.empty()) {
    const NdType *Node = Work.pop_back_val();
    if (!Nodes.insert(Node).second)
      continue;
    for (const NdType *Part :
         {Node->Pointee.get(), Node->ElemType.get(), Node->RetType.get()})
      if (Part)
        Work.push_back(Part);
    for (const auto *Parts :
         {&Node->Fields, &Node->ParamTypes, &Node->FieldDisplayTypes})
      for (const TypeRef &Part : *Parts)
        if (Part)
          Work.push_back(Part.get());
  }
}

/// Whether the library's record \p Name reaches more than \p Limit records
/// through the types its definitions name.
bool reachesMoreRecords(const types::TypeLibrary &Library, llvm::StringRef Name,
                        size_t Limit) {
  std::vector<std::string> Work{Name.str()};
  llvm::StringSet<> Seen;
  size_t Records = 0;
  while (!Work.empty()) {
    const std::string Next = std::move(Work.back());
    Work.pop_back();
    if (!Seen.insert(Next).second)
      continue;
    const types::LocalType *Type = Library.find(Next);
    if (!Type)
      continue;
    if ((Type->Kind == types::TypeKind::Struct ||
         Type->Kind == types::TypeKind::Union) &&
        ++Records > Limit)
      return true;
    for (std::string &Named : types::mentionedTypeNames(*Type))
      Work.push_back(std::move(Named));
  }
  return false;
}

/// Whether every member \p Record names for display is one \p Def has
/// where the name says, and is no bitfield, which C reads otherwise than
/// its storage's bytes.
bool membersAgree(const NdType &Record, const types::RecordDef &Def) {
  if (Record.FieldDisplayNames.size() != Record.FieldDisplayOffsets.size())
    return Record.FieldDisplayNames.empty();
  std::set<std::pair<uint64_t, std::string>> Members;
  for (const types::FlatMember &Flat : types::flattenMembers(Def))
    if (!Flat.BitWidth && !Flat.Name.empty())
      Members.emplace(Flat.BitOffset / 8, Flat.Name);
  for (size_t I = 0; I < Record.FieldDisplayNames.size(); ++I)
    if (!Record.FieldDisplayNames[I].empty() &&
        !Members.count(
            {Record.FieldDisplayOffsets[I], Record.FieldDisplayNames[I]}))
      return false;
  return true;
}

} // namespace

TypeRef decompilerTypeOf(const types::TypeLibrary &Library,
                         const types::TypeExpr &Type) {
  return Converter(Library).convert(Type, 0);
}

TypeRef sourceCallTypeOf(const types::TypeLibrary &Library,
                         const types::TypeExpr &Type) {
  return Converter(Library, nullptr, true).convert(Type, 0);
}

std::optional<FunctionSym>
signatureOf(const types::TypeLibrary &Library,
            const types::FunctionDef &Function, llvm::StringRef Name,
            const types::TypeSpeller *PrototypeSpeller) {
  Converter Convert(Library, PrototypeSpeller);
  FunctionSym Declared;
  Declared.Name = Name.str();
  Declared.IsVariadic = Function.Variadic;
  Declared.NoReturn = Function.NoReturn;
  Declared.ReturnType = Function.Return ? Convert.convert(*Function.Return, 0)
                                        : NdType::makeVoid();
  if (!Declared.ReturnType)
    return std::nullopt;
  for (const types::Parameter &Param : Function.Params) {
    TypeRef Type = Param.Type ? Convert.convert(*Param.Type, 0) : nullptr;
    if (!Type || Type->Kind == NdTypeKind::Func ||
        Type->Kind == NdTypeKind::Void)
      return std::nullopt;
    Declared.Params.emplace_back(Param.Name, std::move(Type));
  }
  switch (Function.Convention) {
  case types::CallingConvention::Cdecl:
    Declared.CallConv = DebugCallConv::Cdecl;
    break;
  case types::CallingConvention::Stdcall:
    Declared.CallConv = DebugCallConv::Stdcall;
    break;
  case types::CallingConvention::Fastcall:
    Declared.CallConv = DebugCallConv::Fastcall;
    break;
  case types::CallingConvention::Thiscall:
    Declared.CallConv = DebugCallConv::Thiscall;
    break;
  default:
    break;
  }
  return Declared;
}

namespace {

/// How a value of \p Type passes: void, a float, a record or an integer or
/// pointer, with its size.
std::pair<NdTypeKind, uint16_t> passing(const TypeRef &Type) {
  if (!Type || Type->Kind == NdTypeKind::Void)
    return {NdTypeKind::Void, 0};
  if (Type->Kind == NdTypeKind::Float ||
      (Type->Kind == NdTypeKind::Struct && !Type->IsEnum))
    return {Type->Kind, Type->Size};
  return {NdTypeKind::Int, Type->Size};
}

} // namespace

bool passesAlike(const FunctionSym &A, const FunctionSym &B) {
  if (A.IsVariadic != B.IsVariadic || A.Params.size() != B.Params.size() ||
      passing(A.ReturnType) != passing(B.ReturnType))
    return false;
  for (size_t I = 0; I < A.Params.size(); ++I)
    if (passing(A.Params[I].second) != passing(B.Params[I].second))
      return false;
  return true;
}

llvm::StringSet<> prototypeTypeNames(const types::TypeLibrary &Library) {
  std::vector<std::string> Work;
  for (const auto &Entry : Library.prototypes()) {
    if (!Entry.second)
      continue;
    types::LocalType Signature;
    Signature.Kind = types::TypeKind::Function;
    Signature.Function = Entry.second;
    for (std::string &Name : types::mentionedTypeNames(Signature))
      Work.push_back(std::move(Name));
  }
  llvm::StringSet<> Names;
  while (!Work.empty()) {
    const std::string Name = std::move(Work.back());
    Work.pop_back();
    if (!Names.insert(Name).second)
      continue;
    if (const types::LocalType *Type = Library.find(Name))
      for (std::string &Named : types::mentionedTypeNames(*Type))
        Work.push_back(std::move(Named));
  }
  return Names;
}

LocalTypesDebugContext::LocalTypesDebugContext(
    const DebugContext &Inner, std::shared_ptr<const types::TypeLibrary> Types,
    std::map<va_t, std::string> Declarations, const BinaryImage *Image,
    std::vector<types::HeaderLibrary> Headers,
    std::unordered_map<uint64_t, std::string> Recognized, std::string EntryType,
    std::optional<va_t> ProgramMain, std::string EntryDeclaration)
    : Inner(Inner), Types(std::move(Types)),
      Declarations(std::move(Declarations)), Image(Image),
      Headers(std::move(Headers)), Recognized(std::move(Recognized)),
      EntryType(std::move(EntryType)),
      EntryDeclaration(std::move(EntryDeclaration)), ProgramMain(ProgramMain) {}

LocalTypesDebugContext::~LocalTypesDebugContext() {
  std::lock_guard<std::mutex> Guard(Mutex);
  for (Named &Type : Completed) {
    Type.Type->FieldDisplayNames = std::move(Type.Names);
    Type.Type->FieldDisplayOffsets = std::move(Type.Offsets);
    Type.Type->FieldDisplayTypes = std::move(Type.Types);
    Type.Type->FieldDisplayAlignment = Type.Alignment;
  }
  // What this context completed holds its parts no longer.
  Completed.clear();
  KeptTypes.clear();
  Given.clear();
  releaseSharedRecords();
}

void LocalTypesDebugContext::releaseSharedRecords() const {
  // The shared records name each other.  Where nothing outside them holds
  // one of their nodes any more, their parts let go of each other; a node
  // still held keeps its parts.
  llvm::DenseMap<const NdType *, long> Held;
  for (const NdType *Node : SharedNodes) {
    for (const NdType *Part :
         {Node->Pointee.get(), Node->ElemType.get(), Node->RetType.get()})
      if (Part)
        ++Held[Part];
    for (const auto *Parts :
         {&Node->Fields, &Node->ParamTypes, &Node->FieldDisplayTypes})
      for (const TypeRef &Part : *Parts)
        if (Part)
          ++Held[Part.get()];
  }
  for (const auto &Entry : SharedRecords)
    if (Entry.second)
      ++Held[Entry.second.get()];
  // shared_from_this is unavailable: each node's count is read through a
  // part that holds it.
  bool Outside = false;
  const auto Check = [&](const TypeRef &Part) {
    if (Part && Part.use_count() > Held.lookup(Part.get()))
      Outside = true;
  };
  for (const NdType *Node : SharedNodes) {
    Check(Node->Pointee);
    Check(Node->ElemType);
    Check(Node->RetType);
    for (const auto *Parts :
         {&Node->Fields, &Node->ParamTypes, &Node->FieldDisplayTypes})
      for (const TypeRef &Part : *Parts)
        Check(Part);
  }
  for (const auto &Entry : SharedRecords)
    Check(Entry.second);
  if (Outside)
    return;
  for (const auto &Entry : SharedRecords)
    if (Entry.second)
      Entry.second->FieldDisplayTypes.clear();
}

const types::LocalType *
LocalTypesDebugContext::definedRecord(llvm::StringRef Name,
                                      uint16_t Size) const {
  const types::LocalType *Type = Types->find(Name);
  if (!Type || !Type->Record || !Type->Record->Defined ||
      (Type->Kind != types::TypeKind::Struct &&
       Type->Kind != types::TypeKind::Union))
    return nullptr;
  return !Size || Size == Type->Record->Size ? Type : nullptr;
}

TypeRef
LocalTypesDebugContext::libraryRecord(const types::LocalType &Local) const {
  if (UnsharedRecords.contains(Local.Name))
    return Converter(*Types).convert(*types::makeNamed(Local.Name, Local.Kind),
                                     0);
  if (const auto Found = SharedRecords.find(Local.Name);
      Found != SharedRecords.end() && SharedNodes.contains(Found->second.get()))
    return Found->second;
  // Each record decides by what it reaches alone, so that a record another
  // conversion shared reads as its own decides it.
  if (reachesMoreRecords(*Types, Local.Name, kMaxLocalTypeSharedClosure)) {
    UnsharedRecords.insert(Local.Name);
    return Converter(*Types).convert(*types::makeNamed(Local.Name, Local.Kind),
                                     0);
  }
  const TypeRef Root =
      SharedRecordConversion(*Types, SharedRecords).convert(Local);
  for (const auto &Entry : SharedRecords)
    if (!SharedNodes.contains(Entry.second.get()))
      collectTypeNodes(Entry.second, SharedNodes);
  return Root;
}

bool LocalTypesDebugContext::takeLibraryNames(const TypeRef &Type) const {
  std::vector<std::string> Names;
  std::vector<uint16_t> Offsets;
  std::vector<TypeRef> Members;
  uint16_t Alignment = Type->FieldDisplayAlignment;
  const types::LocalType *Local = nullptr;
  if (Type->IsEnum) {
    Local = Types->find(Type->SourceName);
    if (!Local || Local->Kind != types::TypeKind::Enum || !Local->Enum)
      return false;
    for (const auto &[Value, Enumerator] : displayedEnumerators(
             *Local->Enum, Types->builtinBytes(Local->Enum->Underlying))) {
      Names.push_back(Enumerator);
      Offsets.push_back(Value);
    }
  } else {
    Local = definedRecord(Type->SourceName, Type->Size);
    const TypeRef From = Local ? libraryRecord(*Local) : nullptr;
    if (!From || From->Kind != NdTypeKind::Struct)
      return false;
    // Each member the debug information gave lies where one of the
    // library's does, as the members of the same record do under names the
    // user changed; a record laid out otherwise keeps its own.
    const std::set<uint16_t> Lie(From->FieldDisplayOffsets.begin(),
                                 From->FieldDisplayOffsets.end());
    for (size_t I = 0; I < Type->FieldDisplayNames.size(); ++I)
      if (!Type->FieldDisplayNames[I].empty() &&
          (I >= Type->FieldDisplayOffsets.size() ||
           !Lie.count(Type->FieldDisplayOffsets[I])))
        return false;
    Names = From->FieldDisplayNames;
    Offsets = From->FieldDisplayOffsets;
    Members = From->FieldDisplayTypes;
    Alignment = From->FieldDisplayAlignment;
  }
  if (CompletedRecords.insert(Type.get()).second)
    KeptTypes.push_back(Type);
  Read.insert(Local->Name);
  // A bounded conversion can retain every name and offset while leaving
  // member types incomplete. The matching library must complete those too;
  // otherwise a scalar access may name an entire union or structure in C.
  if (Names == Type->FieldDisplayNames &&
      Offsets == Type->FieldDisplayOffsets &&
      Members == Type->FieldDisplayTypes &&
      Alignment == Type->FieldDisplayAlignment)
    return true;
  Completed.push_back({Type, std::move(Type->FieldDisplayNames),
                       std::move(Type->FieldDisplayOffsets),
                       std::move(Type->FieldDisplayTypes),
                       Type->FieldDisplayAlignment});
  Type->FieldDisplayNames = std::move(Names);
  Type->FieldDisplayOffsets = std::move(Offsets);
  Type->FieldDisplayTypes = std::move(Members);
  Type->FieldDisplayAlignment = Alignment;
  return true;
}

void LocalTypesDebugContext::completeType(const TypeRef &Type) const {
  TypeRef Record = Type;
  while (Record && Record->Kind == NdTypeKind::Ptr)
    Record = Record->Pointee;
  if (!Record || Record->Kind != NdTypeKind::Struct ||
      Record->SourceName.empty()) {
    Inner.completeType(Type);
    return;
  }
  {
    std::lock_guard<std::mutex> Guard(Mutex);
    // A record libraryRecord shared is complete as converted.
    if (CompletedRecords.contains(Record.get()) ||
        SharedNodes.contains(Record.get()))
      return;
    // The library's members, which a record none has been given members
    // takes whole.
    if (!Record->IsEnum && Record->FieldDisplayNames.empty() &&
        takeLibraryNames(Record))
      return;
  }
  Inner.completeType(Type);
  if (!Record->IsEnum && Record->FieldDisplayNames.empty())
    return;
  std::lock_guard<std::mutex> Guard(Mutex);
  if (CompletedRecords.contains(Record.get()))
    return;
  // What the debug information gave a record or an enumeration of one of
  // the library's names: a unit holds the library's definition only where
  // that agrees with it.
  const types::LocalType *Local = Types->find(Record->SourceName);
  const bool Gives =
      Local &&
      (Record->IsEnum
           ? Local->Kind == types::TypeKind::Enum
           : definedRecord(Record->SourceName, Record->Size) != nullptr) &&
      GivenTypes.insert(Record.get()).second;
  if (Gives)
    KeptTypes.push_back(Record);
  // The library's names in place of those it gave, which are the same
  // unless the user renamed them.
  takeLibraryNames(Record);
  // definitions reads of a given type whether it is an enumeration and its
  // names and offsets, as the library's names left them.  Another type of
  // the name that reads the same changes none of its answers, and the debug
  // information gives the same types again with every unit.
  if (Gives) {
    std::vector<TypeRef> &Same = Given[Record->SourceName];
    if (llvm::none_of(Same, [&](const TypeRef &Other) {
          return Other->IsEnum == Record->IsEnum &&
                 Other->FieldDisplayNames == Record->FieldDisplayNames &&
                 Other->FieldDisplayOffsets == Record->FieldDisplayOffsets;
        })) {
      Same.push_back(Record);
      ++GivenCount;
    }
  }
}

void LocalTypesDebugContext::readRenamedInC() const {
  if (RenamedInC)
    return;
  RenamedInC.emplace();
  for (const auto &[Name, Spelled] :
       types::renamedNames(*Types, types::Language::C)) {
    RenamedInC->insert(Name);
    // An enumerator's key is its enumeration's: Owner::Value.
    if (const size_t Scope = Name.find("::"); Scope != std::string::npos)
      RenamedInC->insert(Name.substr(0, Scope) + "::");
  }
}

void LocalTypesDebugContext::makeCRenderer() const {
  if (CRenderer)
    return;
  types::RenderOptions Options;
  Options.Lang = types::Language::C;
  // Exact widths, which mean the same on every compiler the unit builds
  // with; the checks make a layout no compiler agrees with an error.
  Options.Spelling = types::BuiltinSpelling::Portable;
  Options.Asserts = true;
  // The system's own types come from its headers, which the unit includes:
  // the first library of a system's headers the program joins.
  for (const types::HeaderLibrary Library : Headers)
    if (types::headerLibrarySystem(Library)) {
      Options.SystemLibrary = Library;
      break;
    }
  CRenderer = std::make_unique<types::EmbeddedCRenderer>(*Types, Options);
}

void LocalTypesDebugContext::prepareDefinitions() const {
  std::lock_guard<std::mutex> Guard(Mutex);
  readRenamedInC();
  makeCRenderer();
}

CRecordDefinitions
LocalTypesDebugContext::definitions(const std::vector<TypeRef> &Records) const {
  std::lock_guard<std::mutex> Guard(Mutex);
  readRenamedInC();
  // Whether \p Type, of the library's type \p Local, names its members or
  // enumerators as the library does.
  const auto Agrees = [&](const NdType &Type, const types::LocalType &Local) {
    if (Local.Kind != types::TypeKind::Enum)
      return Local.Record && membersAgree(Type, *Local.Record);
    // An enumeration none of whose enumerators C spells otherwise.
    return Local.Enum && !RenamedInC->contains(Local.Name + "::") &&
           llvm::all_of(Type.FieldDisplayNames, [&](const std::string &Name) {
             return Name.empty() ||
                    llvm::any_of(Local.Enum->Values,
                                 [&](const types::Enumerator &Value) {
                                   return Value.Name == Name;
                                 });
           });
  };
  // ... as each type the debug information gave of its name does.
  const auto AllAgree = [&](const NdType &Type, const types::LocalType &Local) {
    const auto Types = Given.find(Local.Name);
    return Agrees(Type, Local) &&
           (Types == Given.end() ||
            llvm::all_of(Types->second, [&](const TypeRef &Other) {
              return Other->IsEnum == (Local.Kind == types::TypeKind::Enum) &&
                     Agrees(*Other, Local);
            }));
  };
  // The enumerators of the enumerations the decompiler displays, which a
  // unit can spell without their enumeration's name.  They follow from the
  // types Given holds, whose names are final once there, and the library:
  // read again only after Given grows.
  if (GivenEnumeratorsRead != GivenCount) {
    GivenEnumerators.clear();
    for (const auto &Entry : Given)
      for (const TypeRef &Enum : Entry.getValue())
        if (const types::LocalType *Local = Types->find(Entry.getKey());
            Enum->IsEnum && Local && Local->Kind == types::TypeKind::Enum &&
            AllAgree(*Enum, *Local))
          for (const std::string &Name : Enum->FieldDisplayNames)
            if (!Name.empty())
              GivenEnumerators.try_emplace(Name, Local);
    GivenEnumeratorsRead = GivenCount;
  }
  const llvm::StringMap<const types::LocalType *> &Enumerators =
      GivenEnumerators;
  CRecordDefinitions Result;
  std::vector<const types::LocalType *> Roots;
  for (const TypeRef &Requested : Records) {
    const bool Declaration = Requested && Requested->Kind == NdTypeKind::Ptr;
    const TypeRef &Record = Declaration ? Requested->Pointee : Requested;
    if (!Record || Record->Kind != NdTypeKind::Struct ||
        Record->SourceName.empty() || RenamedInC->contains(Record->SourceName))
      continue;
    // A name emitted C takes from the headers it includes, as its casts
    // spell uintptr_t, is the header's: a local type of the name, as a
    // system's header library gives, would define it again, as another
    // type where the header's is long and the local type's int64_t.
    if (fromCHeader(Record->SourceName))
      continue;
    const types::LocalType *Local = Types->find(Record->SourceName);
    // A name the unit only spells.
    const bool Spelled =
        !Record->IsEnum && !Record->Size && Record->FieldDisplayNames.empty();
    if (!Local) {
      if (const auto Owner = Enumerators.find(Record->SourceName);
          Spelled && Owner != Enumerators.end())
        Roots.push_back(Owner->second);
      continue;
    }
    if (Local->Kind == types::TypeKind::Struct ||
        Local->Kind == types::TypeKind::Union)
      Result.Tags[Record->SourceName] = Local->Kind == types::TypeKind::Union;
    if (Declaration) {
      Read.insert(Local->Name);
      Result.DeclaredTypes.push_back(Local->Name);
      continue;
    }
    if (Record->IsEnum || (Spelled && Local->Kind == types::TypeKind::Enum)) {
      if (Local->Kind == types::TypeKind::Enum && AllAgree(*Record, *Local))
        Roots.push_back(Local);
      continue;
    }
    // A typedef the unit spells, as a signature does a parameter's.
    if (Spelled && Local->Kind == types::TypeKind::Typedef) {
      Roots.push_back(Local);
      continue;
    }
    // A record the unit names members of otherwise than the library does
    // stays declared only: its definition would disagree with them.
    if (const types::LocalType *Defined =
            definedRecord(Record->SourceName, Record->Size);
        Defined && AllAgree(*Record, *Defined))
      Roots.push_back(Defined);
  }
  if (Roots.empty())
    return Result;
  makeCRenderer();
  const types::EmbeddedC Embedded = CRenderer->render(Roots);
  Result.Text = Embedded.Text;
  for (const auto &[Name, Union] : Embedded.Records)
    Result.Records[Name] = Result.Tags[Name] = Union;
  for (const auto &[Name, Union] : Embedded.HeaderRecords)
    Result.HeaderRecords[Name] = Union;
  for (const auto &[Name, Union] : Embedded.Declared)
    Result.Tags[Name] = Union;
  Result.Identifiers = Embedded.Identifiers;
  for (const std::string &Name : Embedded.Types) {
    Read.insert(Name);
    if (const types::LocalType *Type = Types->find(Name);
        Type && Type->Kind == types::TypeKind::Enum)
      Result.Enumerations.push_back(Name);
  }
  Result.Types = Embedded.Types;
  Result.HeaderTypes = Embedded.HeaderTypes;
  llvm::append_range(Result.DeclaredTypes, Embedded.DeclaredTypes);
  Result.HeaderTypedefs = Embedded.HeaderTypedefs;
  return Result;
}

llvm::StringSet<> LocalTypesDebugContext::readTypes() const {
  std::lock_guard<std::mutex> Guard(Mutex);
  llvm::StringSet<> Names;
  for (const auto &Name : Read)
    Names.insert(Name.getKey());
  return Names;
}

std::optional<types::HeaderLibrary> LocalTypesDebugContext::prototypeHeader(
    llvm::StringRef Name, const types::FunctionDef *Function) const {
  // Joining a header library shares its prototypes.
  for (const types::HeaderLibrary Library : Headers)
    if (const types::TypeLibrary *Header = types::headerLibrary(Library))
      if (Header->findPrototype(Name) == Function)
        return Library;
  return std::nullopt;
}

std::optional<FunctionSym>
LocalTypesDebugContext::resolvePrototype(llvm::StringRef Name) const {
  std::lock_guard<std::mutex> Guard(Mutex);
  if (auto Known = Signatures.find(Name); Known != Signatures.end())
    return Known->second;
  std::optional<FunctionSym> &Signature = Signatures[Name];
  const auto Function = Types->prototypes().find(Name);
  if (Function == Types->prototypes().end() || !Function->second)
    return Signature;
  types::RenderOptions PortableOptions;
  PortableOptions.Lang = types::Language::C;
  PortableOptions.Spelling = types::BuiltinSpelling::Portable;
  PortableOptions.PrototypeIntegerIdentity = true;
  auto &Portable = PortablePrototypeSpeller;
  if (!Portable)
    Portable = std::make_unique<types::TypeSpeller>(*Types, PortableOptions);
  Signature = signatureOf(*Types, *Function->second, Name, Portable.get());
  if (!Signature)
    return Signature;
  const std::optional<types::HeaderLibrary> Header =
      prototypeHeader(Name, Function->second.get());
  Signature->PrototypeLibrary = Header ? types::headerLibraryName(*Header).str()
                                       : UserPrototypeLibrary.str();
  Signature->PrototypeFromHeader = Header.has_value();
  Signature->PrototypeFromCLibrary =
      Header && types::headerLibrarySystem(*Header).has_value();
  types::RenderOptions Options;
  Options.Lang = types::Language::C;
  if (Header)
    Options.Spelling = types::headerLibrarySpelling(*Header);
  std::unique_ptr<types::TypeSpeller> &Speller =
      Spellers[types::spellingOf(Options)];
  if (!Speller)
    Speller = std::make_unique<types::TypeSpeller>(*Types, Options);
  Signature->PrototypeDeclaration =
      (Function->second->NoReturn ? "_Noreturn " : "") +
      Speller->spell(*types::makeFunction(Function->second), Name.str());
  auto PortableFunction = Function->second;
  if (PortableFunction->Convention == types::CallingConvention::Cdecl ||
      PortableFunction->Convention == types::CallingConvention::Stdcall) {
    auto Unspecified = std::make_shared<types::FunctionDef>(*PortableFunction);
    Unspecified->Convention = types::CallingConvention::Unspecified;
    PortableFunction = std::move(Unspecified);
  }
  Signature->PortablePrototypeDeclaration =
      (PortableFunction->NoReturn ? "_Noreturn " : "") +
      Portable->spell(*types::makeFunction(PortableFunction), Name.str());
  for (const auto &Parameter : PortableFunction->Params)
    Signature->PortableParameterTypes.push_back(
        Portable->spell(*Parameter.Type));
  return Signature;
}

bool LocalTypesDebugContext::prototypeDoesNotReturn(
    llvm::StringRef Name) const {
  // The context's type library is immutable. A missing or returning
  // declaration cannot make this predicate true, regardless of its types.
  // Do not populate the full signature cache for every name in the image.
  const auto *Function = Types->findPrototype(Name);
  if (!Function || !Function->NoReturn)
    return false;
  // Keep the ordinary resolver's validation for positive declarations: an
  // unrepresentable return or parameter type must not terminate a CFG path.
  return DebugContext::prototypeDoesNotReturn(Name);
}

std::optional<FunctionSym> LocalTypesDebugContext::entrySignature() const {
  if (!EntryDeclaration.empty()) {
    const auto Read = types::parseFunctionDeclaration(EntryDeclaration, *Types);
    if (!Read.ok())
      return std::nullopt;
    auto Signature = signatureOf(*Types, *Read.Function, Read.Name);
    if (Signature)
      Signature->PrototypeDeclaration = EntryDeclaration;
    return Signature;
  }
  // A typedef of a pointer to a function, as UEFI's EFI_IMAGE_ENTRY_POINT.
  const types::LocalType *Type = Types->find(EntryType);
  if (!Type || Type->Kind != types::TypeKind::Typedef || !Type->Target ||
      Type->Target->Kind != types::TypeExprKind::Pointer ||
      !Type->Target->Element)
    return std::nullopt;
  const auto &Pointee = *Type->Target->Element;
  const types::FunctionDef *Function = Pointee.Function.get();
  if (Pointee.Kind == types::TypeExprKind::Named) {
    const types::LocalType *Named = Types->resolve(Pointee.Name);
    Function = Named && Named->Kind == types::TypeKind::Function
                   ? Named->Function.get()
                   : nullptr;
  } else if (Pointee.Kind != types::TypeExprKind::Function)
    Function = nullptr;
  if (!Function)
    return std::nullopt;
  std::optional<FunctionSym> Signature =
      signatureOf(*Types, *Function, std::string());
  if (!Signature)
    return Signature;
  for (const types::HeaderLibrary Library : Headers)
    if (const types::TypeLibrary *Header = types::headerLibrary(Library);
        Header && Header->find(EntryType)) {
      Signature->PrototypeLibrary = types::headerLibraryName(Library).str();
      break;
    }
  return Signature;
}

std::optional<FunctionSym> LocalTypesDebugContext::mainSignature() const {
  const types::FunctionDeclaration Read = types::parseFunctionDeclaration(
      std::string(libc::kProgramMainDeclaration), *Types);
  if (!Read.ok())
    return std::nullopt;
  return signatureOf(*Types, *Read.Function, Read.Name);
}

void LocalTypesDebugContext::readStatedNames() const {
  if (!StatedNames)
    StatedNames = Image ? statedFunctionNamesByAddress(*Image)
                        : std::unordered_map<va_t, std::vector<std::string>>();
}

void LocalTypesDebugContext::indexStatedNames() const {
  std::lock_guard<std::mutex> Guard(Mutex);
  readStatedNames();
}

std::vector<std::string> LocalTypesDebugContext::statedNames(va_t Entry) const {
  if (!Image)
    return {};
  std::lock_guard<std::mutex> Guard(Mutex);
  readStatedNames();
  if (const auto Known = StatedNames->find(Entry); Known != StatedNames->end())
    return Known->second;
  return {};
}

std::string LocalTypesDebugContext::importName(va_t Addr) const {
  if (!Image)
    return {};
  std::lock_guard<std::mutex> Guard(Mutex);
  auto [Known, Added] = ImportNames.try_emplace(Addr);
  if (Added)
    Known->second = importCalleeName(*Image, Addr);
  return Known->second;
}

std::optional<FunctionSym> LocalTypesDebugContext::declared(va_t Entry) const {
  const auto Declaration = Declarations.find(Entry);
  if (Declaration == Declarations.end())
    return std::nullopt;
  std::lock_guard<std::mutex> Guard(Mutex);
  if (auto Known = DeclaredSignatures.find(Entry);
      Known != DeclaredSignatures.end())
    return Known->second;
  std::optional<FunctionSym> &Signature = DeclaredSignatures[Entry];
  const types::FunctionDeclaration Read =
      types::parseFunctionDeclaration(Declaration->second, *Types);
  if (Read.ok())
    Signature = signatureOf(*Types, *Read.Function, Read.Name);
  return Signature;
}

std::optional<FunctionSym>
LocalTypesDebugContext::resolveFunction(va_t Addr) const {
  std::optional<FunctionSym> Function = Inner.resolveFunction(Addr);
  const bool AtEntry = Function && Function->Addr == Addr;
  // The user's declaration states the function's parameters and return,
  // whatever the debug information says; its name and extent stay.
  if (std::optional<FunctionSym> User = declared(Addr)) {
    FunctionSym Declared = AtEntry ? *Function : FunctionSym();
    Declared.Addr = Addr;
    Declared.ReturnType = std::move(User->ReturnType);
    Declared.Params = std::move(User->Params);
    if (User->CallConv != DebugCallConv::Unknown)
      Declared.CallConv = User->CallConv;
    // The declaration's parameters follow its convention, not where the
    // debug information's entered.
    Declared.PlatformConvention = true;
    Declared.ParamEntryLocations.clear();
    return Declared;
  }
  // A declaration of the program's own describes its build.
  if (AtEntry && (Function->ReturnType || !Function->Params.empty()))
    return Function;
  // The prototype of the name the function is displayed under: the export
  // or symbol the image states, as C names it, else the debug information's
  // name.  Where a linker folded identical functions, the entry's other
  // names name other functions, whose prototypes must pass and return alike
  // for the code to follow the shown one.
  std::vector<std::string> Names = statedNames(Addr);
  for (std::string &Stated : Names)
    Stated = cNameOfStatedName(Stated, Image->Format, Image->Arch).str();
  std::string Name = Names.empty() ? std::string() : Names.front();
  if (Name.empty() && AtEntry)
    Name = Function->Name;
  // Signatures name a function by its bytes where nothing else does.
  if (const auto Matched = Recognized.find(Addr);
      Name.empty() && Matched != Recognized.end())
    Name = Matched->second;
  std::optional<FunctionSym> Known =
      Name.empty() ? std::nullopt : resolvePrototype(Name);
  if (Known)
    for (size_t I = 1; I < Names.size(); ++I)
      if (const std::optional<FunctionSym> Other = resolvePrototype(Names[I]);
          Other && !passesAlike(*Known, *Other))
        return Function;
  // The image's entry point, as its kind's specification types it: a UEFI
  // application's (ImageHandle, SystemTable).
  if (!Known && Image && Addr == Image->Entry &&
      (!EntryType.empty() || !EntryDeclaration.empty()))
    Known = entrySignature();
  // The program's main, as its C runtime calls it, under that name where
  // nothing else names it.
  const bool Main = !Known && ProgramMain && Addr == *ProgramMain;
  if (Main)
    Known = mainSignature();
  // A call through an import's slot or stub reaches the routine another
  // module exports under the import's name, which the call names it by.
  if (!Known && !AtEntry)
    if (const std::string Imported = importName(Addr); !Imported.empty())
      if ((Known = resolvePrototype(
               cNameOfStatedName(Imported, Image->Format, Image->Arch)))) {
        Known->Addr = Addr;
        return Known;
      }
  if (!Known)
    return Function;
  FunctionSym Typed = AtEntry ? *Function : FunctionSym();
  Typed.Addr = Addr;
  Typed.ReturnType = std::move(Known->ReturnType);
  Typed.Params = std::move(Known->Params);
  if (Known->CallConv != DebugCallConv::Unknown)
    Typed.CallConv = Known->CallConv;
  Typed.PlatformConvention = true;
  Typed.ParamEntryLocations.clear();
  Typed.PrototypeLibrary = std::move(Known->PrototypeLibrary);
  Typed.PrototypeDeclaration = std::move(Known->PrototypeDeclaration);
  Typed.PortablePrototypeDeclaration =
      std::move(Known->PortablePrototypeDeclaration);
  Typed.PortableParameterTypes = std::move(Known->PortableParameterTypes);
  Typed.PrototypeFromHeader = Known->PrototypeFromHeader;
  Typed.PrototypeFromCLibrary = Known->PrototypeFromCLibrary;
  Typed.NoReturn = Known->NoReturn;
  Typed.IsVariadic = Known->IsVariadic;
  if (Main && (Typed.Name.empty() || isSynthesizedFuncName(Typed.Name)))
    Typed.Name = "main";
  return Typed;
}

} // namespace neverd::sdk
