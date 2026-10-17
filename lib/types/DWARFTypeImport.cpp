//===- DWARFTypeImport.cpp - Local types from DWARF type entries ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/DWARFTypeImport.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <string>

namespace neverd::types {
namespace {

namespace dw = llvm::dwarf;

/// How deep a type expression nests: pointers, arrays, qualifiers and the
/// anonymous records defined in place.
constexpr unsigned MaxDepth = 64;

std::optional<uint64_t> unsignedOf(const llvm::DWARFDie &Die,
                                   dw::Attribute Attribute) {
  return dw::toUnsigned(Die.find(Attribute));
}

bool isRecordTag(dw::Tag Tag) {
  return Tag == dw::DW_TAG_structure_type || Tag == dw::DW_TAG_class_type ||
         Tag == dw::DW_TAG_union_type;
}

bool isTypeTag(dw::Tag Tag) {
  return isRecordTag(Tag) || Tag == dw::DW_TAG_enumeration_type ||
         Tag == dw::DW_TAG_typedef;
}

/// The name DWARF gives \p Die, qualified by the namespaces and records
/// that hold it, as the DWARF debug context names the records the
/// decompiler reads (llvm::dumpTypeQualifiedName); empty for an anonymous
/// type.
std::string qualifiedName(const llvm::DWARFDie &Die) {
  if (!Die.getShortName())
    return std::string();
  std::string Name;
  llvm::raw_string_ostream OS(Name);
  llvm::dumpTypeQualifiedName(Die, OS);
  return Name;
}

/// Why an import leaves a type out.
struct Refusal {
  std::string Reason;
};

class Importer {
public:
  Importer(TypeLibrary &Library, DWARFTypeImportStats &Stats)
      : Library(Library), Stats(Stats) {}

  void unit(llvm::DWARFUnit &Unit) {
    const llvm::DWARFDie Root = Unit.getUnitDIE(/*ExtractUnitDIEOnly=*/false);
    if (!Root.isValid())
      return;
    scope(Root);
    drain();
  }

  void finish() {
    for (const auto &Entry : Declared)
      if (!Library.find(Entry.getKey())) {
        LocalType Local;
        Local.Name = Entry.getKey().str();
        Local.Kind = Entry.getValue();
        Local.Origin = TypeOrigin::DWARF;
        if (Local.Kind == TypeKind::Enum) {
          Local.Enum = std::make_shared<EnumDef>();
        } else {
          auto Record = std::make_shared<RecordDef>();
          Record->Union = Local.Kind == TypeKind::Union;
          Record->Defined = false;
          Local.Record = std::move(Record);
        }
        Library.define(std::move(Local));
        ++Stats.DeclaredOnly;
      }
  }

private:
  /// The types a scope declares: a unit's, a namespace's or a record's.
  void scope(const llvm::DWARFDie &Scope) {
    for (const llvm::DWARFDie &Child : Scope.children()) {
      const dw::Tag Tag = Child.getTag();
      if (isTypeTag(Tag))
        Pending.push_back(Child);
      // The types of namespaces, and those records declare inside them;
      // a function's own types are its own.
      if (Tag == dw::DW_TAG_namespace || isRecordTag(Tag))
        scope(Child);
    }
  }

  void drain() {
    while (!Pending.empty()) {
      const llvm::DWARFDie Die = Pending.front();
      Pending.pop_front();
      named(Die);
    }
  }

  /// Defines the named type \p Die, once: a record, an enumeration or a
  /// typedef.  An anonymous one is defined where a typedef or a member
  /// names it.
  void named(const llvm::DWARFDie &Die) {
    const dw::Tag Tag = Die.getTag();
    if (Tag == dw::DW_TAG_typedef) {
      if (Visited.insert(Die.getDebugInfoEntry()).second)
        typedefOf(Die);
      return;
    }
    const std::string Name = qualifiedName(Die);
    if (Name.empty() || !Visited.insert(Die.getDebugInfoEntry()).second)
      return;
    const TypeKind Kind = kindOf(Tag);
    if (dw::toUnsigned(Die.find(dw::DW_AT_declaration), 0)) {
      Declared.try_emplace(Name, Kind);
      return;
    }
    // Another unit's definition of the name, as a header's types are: the
    // first stays, and one of another size is reported.
    if (Defined.count(Name)) {
      const LocalType *First = Library.find(Name);
      const std::optional<uint64_t> Bytes =
          unsignedOf(Die, dw::DW_AT_byte_size);
      if (First && First->Record && Bytes && *Bytes != First->Record->Size &&
          Conflicting.insert(Name).second)
        Stats.Conflicts.push_back(Name);
      return;
    }
    define(Name, Kind, Die);
  }

  static TypeKind kindOf(dw::Tag Tag) {
    switch (Tag) {
    case dw::DW_TAG_union_type:
      return TypeKind::Union;
    case dw::DW_TAG_enumeration_type:
      return TypeKind::Enum;
    case dw::DW_TAG_typedef:
      return TypeKind::Typedef;
    default:
      return TypeKind::Struct;
    }
  }

  /// Defines \p Name as \p Die, a record or an enumeration, unless the
  /// library holds a definition of it already: the same, as another unit
  /// gave it, or another one, which stays.
  void define(const std::string &Name, TypeKind Kind,
              const llvm::DWARFDie &Die) {
    if (Defined.count(Name) || Refused.count(Name))
      return;
    if (const LocalType *Existing = Library.find(Name);
        Existing && Existing->Origin != TypeOrigin::DWARF) {
      // A type the library had before keeps its name.
      Stats.Conflicts.push_back(Name);
      return;
    }
    Defined.insert(Name);
    LocalType Local;
    Local.Name = Name;
    Local.Kind = Kind;
    Local.Origin = TypeOrigin::DWARF;
    std::optional<Refusal> Why;
    if (Kind == TypeKind::Enum) {
      std::shared_ptr<const EnumDef> Enum = enumeration(Die, Why);
      if (!Enum) {
        refuse(Name, Why);
        return;
      }
      Local.Enum = std::move(Enum);
      ++Stats.Enums;
    } else {
      std::shared_ptr<const RecordDef> Record = record(Die, Why, 0);
      if (!Record) {
        refuse(Name, Why);
        return;
      }
      Local.Record = std::move(Record);
      ++(Kind == TypeKind::Union ? Stats.Unions : Stats.Structs);
    }
    Library.define(std::move(Local));
  }

  void refuse(const std::string &Name, const std::optional<Refusal> &Why) {
    Defined.erase(Name);
    Refused.insert(Name);
    Stats.Skipped.push_back(Name + ": " +
                            (Why ? Why->Reason : std::string("unreadable")));
  }

  /// `typedef T Name`.  A typedef of an anonymous record or enumeration
  /// names it, as `typedef struct { ... } Name` does in C; one of a type of
  /// its own name, `typedef struct X X`, is that type.
  void typedefOf(const llvm::DWARFDie &Die) {
    const std::string Name = qualifiedName(Die);
    if (Name.empty() || Defined.count(Name) || Refused.count(Name))
      return;
    // A typedef of a typedef of its own name, as units repeat one, names
    // what that one names: a typedef of itself would be a cycle.
    llvm::DWARFDie Target =
        Die.getAttributeValueAsReferencedDie(dw::DW_AT_type);
    for (unsigned Hop = 0;
         Target.isValid() && Target.getTag() == dw::DW_TAG_typedef &&
         Hop < MaxDepth && qualifiedName(Target) == Name;
         ++Hop)
      Target = Target.getAttributeValueAsReferencedDie(dw::DW_AT_type);
    if (Target.isValid() && (isRecordTag(Target.getTag()) ||
                             Target.getTag() == dw::DW_TAG_enumeration_type)) {
      const std::string TargetName = qualifiedName(Target);
      if (TargetName.empty()) {
        // The first typedef names the anonymous type; another typedef of
        // it names it through that one.
        const auto [Naming, First] =
            AnonymousNames.try_emplace(Target.getDebugInfoEntry(), Name);
        if (First) {
          define(Name, kindOf(Target.getTag()), Target);
          return;
        }
        if (Naming->second == Name)
          return;
      } else if (TargetName == Name) {
        return;
      }
    }
    if (const LocalType *Existing = Library.find(Name);
        Existing && Existing->Origin != TypeOrigin::DWARF) {
      Stats.Conflicts.push_back(Name);
      return;
    }
    Defined.insert(Name);
    std::optional<Refusal> Why;
    TypeExprRef Type = Target.isValid() ? expr(Target, Why, 0, /*ByValue=*/true)
                                        : makeBuiltin(Builtin::Void);
    if (!Type) {
      refuse(Name, Why);
      return;
    }
    LocalType Local;
    Local.Name = Name;
    Local.Kind = TypeKind::Typedef;
    Local.Origin = TypeOrigin::DWARF;
    Local.Target = std::move(Type);
    Library.define(std::move(Local));
    ++Stats.Typedefs;
  }

  /// The type \p Die is, as members and typedefs refer to it.  A named
  /// type held \p ByValue is defined first, as laying out what holds it
  /// needs its size and alignment; one only pointed to is defined later.
  TypeExprRef expr(const llvm::DWARFDie &Die, std::optional<Refusal> &Why,
                   unsigned Depth, bool ByValue) {
    if (Depth > MaxDepth) {
      Why = Refusal{"its types nest too deep"};
      return nullptr;
    }
    const dw::Tag Tag = Die.getTag();
    const auto inner = [&](bool InnerByValue) -> TypeExprRef {
      const llvm::DWARFDie Inner =
          Die.getAttributeValueAsReferencedDie(dw::DW_AT_type);
      return Inner.isValid() ? expr(Inner, Why, Depth + 1, InnerByValue)
                             : makeBuiltin(Builtin::Void);
    };
    const auto later = [&](const llvm::DWARFDie &Named) {
      if (ByValue)
        named(Named);
      else
        Pending.push_back(Named);
    };
    switch (Tag) {
    case dw::DW_TAG_base_type:
      return builtin(Die, Why);
    case dw::DW_TAG_unspecified_type:
      // C++'s decltype(nullptr) is a pointer's bytes; anything else is void.
      if (llvm::StringRef(Die.getShortName() ? Die.getShortName() : "")
              .contains("nullptr"))
        return makePointer(makeBuiltin(Builtin::Void), pointerBytesOf(Die));
      return makeBuiltin(Builtin::Void);
    case dw::DW_TAG_pointer_type:
    case dw::DW_TAG_reference_type:
    case dw::DW_TAG_rvalue_reference_type: {
      TypeExprRef Pointee = inner(/*InnerByValue=*/false);
      if (!Pointee)
        return nullptr;
      return makePointer(std::move(Pointee), pointerBytesOf(Die),
                         Tag != dw::DW_TAG_pointer_type);
    }
    case dw::DW_TAG_const_type:
    case dw::DW_TAG_volatile_type: {
      TypeExprRef Type = inner(ByValue);
      if (!Type)
        return nullptr;
      return qualified(std::move(Type), Tag == dw::DW_TAG_const_type,
                       Tag == dw::DW_TAG_volatile_type);
    }
    case dw::DW_TAG_restrict_type:
    case dw::DW_TAG_atomic_type:
    case dw::DW_TAG_immutable_type:
    case dw::DW_TAG_shared_type:
      return inner(ByValue);
    case dw::DW_TAG_typedef: {
      const std::string Name = qualifiedName(Die);
      if (Name.empty())
        return inner(ByValue);
      // The typedef of an anonymous record names the record, which it
      // defines now, so that whatever refers to it knows its kind.
      const llvm::DWARFDie Target =
          Die.getAttributeValueAsReferencedDie(dw::DW_AT_type);
      const bool Tagged =
          Target.isValid() && (isRecordTag(Target.getTag()) ||
                               Target.getTag() == dw::DW_TAG_enumeration_type);
      const std::string TargetName = Tagged ? qualifiedName(Target) : "";
      if (Tagged && TargetName.empty())
        named(Die);
      else
        later(Die);
      if (Tagged && TargetName == Name)
        return makeNamed(Name, kindOf(Target.getTag()));
      // What the name became, in this unit or the one that defined it.
      if (Tagged && TargetName.empty())
        if (const LocalType *Named = Library.find(Name);
            Named && Named->Kind != TypeKind::Typedef)
          return makeNamed(Name, Named->Kind);
      return makeNamed(Name, TypeKind::Typedef);
    }
    case dw::DW_TAG_structure_type:
    case dw::DW_TAG_class_type:
    case dw::DW_TAG_union_type:
    case dw::DW_TAG_enumeration_type: {
      const TypeKind Kind = kindOf(Tag);
      std::string Name = qualifiedName(Die);
      if (!Name.empty()) {
        later(Die);
        return makeNamed(Name, Kind);
      }
      // An anonymous type a typedef names, by that name.
      if (const auto Typedef = AnonymousNames.find(Die.getDebugInfoEntry());
          Typedef != AnonymousNames.end())
        return makeNamed(Typedef->second, Kind);
      // An anonymous type, defined where it is used.
      if (Kind == TypeKind::Enum) {
        std::shared_ptr<const EnumDef> Enum = enumeration(Die, Why);
        return Enum ? makeEnum(std::move(Enum)) : nullptr;
      }
      std::shared_ptr<const RecordDef> Record = record(Die, Why, Depth + 1);
      return Record ? makeRecord(std::move(Record)) : nullptr;
    }
    case dw::DW_TAG_array_type:
      return array(Die, Why, Depth, ByValue);
    case dw::DW_TAG_subroutine_type:
      return function(Die, Why, Depth);
    case dw::DW_TAG_ptr_to_member_type:
      Why = Refusal{"a pointer to a member"};
      return nullptr;
    default:
      Why = Refusal{"a type DWARF tags " + dw::TagString(Tag).str()};
      return nullptr;
    }
  }

  unsigned pointerBytesOf(const llvm::DWARFDie &Die) const {
    if (std::optional<uint64_t> Bytes = unsignedOf(Die, dw::DW_AT_byte_size))
      return unsigned(*Bytes);
    if (const llvm::DWARFUnit *Unit = Die.getDwarfUnit())
      return Unit->getAddressByteSize();
    return pointerBytes(Library.dataModel());
  }

  /// A base type: the builtin of its encoding and size, a name telling
  /// those of equal size apart; else bytes of its size, as a complex
  /// number, which local types do not spell, is.
  TypeExprRef builtin(const llvm::DWARFDie &Die, std::optional<Refusal> &Why) {
    const uint64_t Bytes = unsignedOf(Die, dw::DW_AT_byte_size).value_or(0);
    const uint64_t Encoding = unsignedOf(Die, dw::DW_AT_encoding).value_or(0);
    const llvm::StringRef Name(Die.getShortName() ? Die.getShortName() : "");
    const DataModel Model = Library.dataModel();
    const auto fits = [&](Builtin Type) {
      return builtinBytes(Type, Model) == Bytes;
    };
    std::optional<Builtin> Found;
    switch (Encoding) {
    case dw::DW_ATE_boolean:
      if (Bytes == 1)
        Found = Builtin::Bool;
      break;
    case dw::DW_ATE_signed_char:
      if (Bytes == 1)
        Found = Name == "char" ? Builtin::Char : Builtin::SChar;
      break;
    case dw::DW_ATE_unsigned_char:
      // An unsigned plain char, as ARM's, keeps its sign.
      if (Bytes == 1)
        Found = Builtin::UChar;
      break;
    case dw::DW_ATE_UTF:
      Found = Bytes == 1   ? Builtin::Char8
              : Bytes == 2 ? Builtin::Char16
              : Bytes == 4 ? Builtin::Char32
                           : std::optional<Builtin>();
      break;
    case dw::DW_ATE_signed:
    case dw::DW_ATE_unsigned: {
      const bool Signed = Encoding == dw::DW_ATE_signed;
      if (Name == "wchar_t" && fits(Builtin::WChar)) {
        Found = Builtin::WChar;
        break;
      }
      // C's names first, where two builtins are as wide: long or long long.
      const bool LongLong = Name.contains("long long");
      const bool Long = !LongLong && Name.contains("long");
      static constexpr Builtin SignedOrder[] = {
          Builtin::SChar, Builtin::Short,    Builtin::Int,
          Builtin::Long,  Builtin::LongLong, Builtin::Int128};
      static constexpr Builtin UnsignedOrder[] = {
          Builtin::UChar, Builtin::UShort,    Builtin::UInt,
          Builtin::ULong, Builtin::ULongLong, Builtin::UInt128};
      const auto &Order = Signed ? SignedOrder : UnsignedOrder;
      if (Long && fits(Order[3]))
        Found = Order[3];
      else if (LongLong && fits(Order[4]))
        Found = Order[4];
      else if (Bytes == 8 && fits(Order[4]))
        Found = Order[4];
      else
        for (Builtin Type : Order)
          if (fits(Type)) {
            Found = Type;
            break;
          }
      break;
    }
    case dw::DW_ATE_float:
      for (Builtin Type : {Builtin::Half, Builtin::Float, Builtin::Double,
                           Builtin::LongDouble})
        if (fits(Type) && (Type != Builtin::LongDouble || Bytes > 8)) {
          Found = Type;
          break;
        }
      break;
    default:
      break;
    }
    if (Found)
      return makeBuiltin(*Found);
    // Rust's () and !, which hold nothing.
    if (!Bytes)
      return makeBuiltin(Builtin::Void);
    if (Bytes > (uint64_t(1) << 20)) {
      Why = Refusal{"a base type of " + std::to_string(Bytes) + " bytes"};
      return nullptr;
    }
    return makeArray(makeBuiltin(Builtin::UChar), Bytes);
  }

  TypeExprRef array(const llvm::DWARFDie &Die, std::optional<Refusal> &Why,
                    unsigned Depth, bool ByValue) {
    const llvm::DWARFDie ElementDie =
        Die.getAttributeValueAsReferencedDie(dw::DW_AT_type);
    TypeExprRef Element = ElementDie.isValid()
                              ? expr(ElementDie, Why, Depth + 1, ByValue)
                              : makeBuiltin(Builtin::UChar);
    if (!Element)
      return nullptr;
    // Each subrange is a dimension, the first outermost.
    std::vector<std::optional<uint64_t>> Counts;
    for (const llvm::DWARFDie &Child : Die.children()) {
      if (Child.getTag() != dw::DW_TAG_subrange_type &&
          Child.getTag() != dw::DW_TAG_enumeration_type)
        continue;
      std::optional<uint64_t> Count = unsignedOf(Child, dw::DW_AT_count);
      if (!Count) {
        const std::optional<llvm::DWARFFormValue> Upper =
            Child.find(dw::DW_AT_upper_bound);
        // A bound only an expression or a variable gives, as a variable
        // length array's, is unknown, as is C's flexible array's -1.
        if (Upper && Upper->isFormClass(llvm::DWARFFormValue::FC_Constant)) {
          const int64_t Lower =
              dw::toSigned(Child.find(dw::DW_AT_lower_bound), 0);
          // A fixed-size form holds the bound's bits, -1 as all ones.
          std::optional<int64_t> High =
              Upper->getForm() == dw::DW_FORM_udata
                  ? std::optional<int64_t>(
                        int64_t(Upper->getAsUnsignedConstant().value_or(0)))
                  : Upper->getAsSignedConstant();
          if (High && *High >= Lower)
            Count = uint64_t(*High - Lower + 1);
        }
      }
      Counts.push_back(Count);
    }
    if (Counts.empty())
      Counts.push_back(std::nullopt);
    for (auto Count = Counts.rbegin(); Count != Counts.rend(); ++Count)
      Element = makeArray(std::move(Element), *Count);
    return Element;
  }

  TypeExprRef function(const llvm::DWARFDie &Die, std::optional<Refusal> &Why,
                       unsigned Depth) {
    auto Function = std::make_shared<FunctionDef>();
    const llvm::DWARFDie Return =
        Die.getAttributeValueAsReferencedDie(dw::DW_AT_type);
    Function->Return = Return.isValid()
                           ? expr(Return, Why, Depth + 1, /*ByValue=*/false)
                           : makeBuiltin(Builtin::Void);
    if (!Function->Return)
      return nullptr;
    for (const llvm::DWARFDie &Child : Die.children()) {
      if (Child.getTag() == dw::DW_TAG_unspecified_parameters) {
        Function->Variadic = true;
        continue;
      }
      if (Child.getTag() != dw::DW_TAG_formal_parameter)
        continue;
      const llvm::DWARFDie Type =
          Child.getAttributeValueAsReferencedDie(dw::DW_AT_type);
      Parameter Param;
      if (const char *Name = Child.getShortName())
        Param.Name = Name;
      Param.Type = Type.isValid() ? expr(Type, Why, Depth + 1, false) : nullptr;
      if (!Param.Type) {
        if (!Why)
          Why = Refusal{"a parameter of no type"};
        return nullptr;
      }
      Function->Params.push_back(std::move(Param));
    }
    return makeFunction(std::move(Function));
  }

  std::shared_ptr<const EnumDef> enumeration(const llvm::DWARFDie &Die,
                                             std::optional<Refusal> &Why) {
    auto Enum = std::make_shared<EnumDef>();
    const uint64_t Bytes = unsignedOf(Die, dw::DW_AT_byte_size).value_or(4);
    bool Signed = false;
    const llvm::DWARFDie Underlying =
        Die.getAttributeValueAsReferencedDie(dw::DW_AT_type);
    std::optional<Builtin> Base;
    if (Underlying.isValid()) {
      std::optional<Refusal> Ignored;
      TypeExprRef Type = expr(Underlying, Ignored, MaxDepth - 4, true);
      while (Type && Type->Kind == TypeExprKind::Named) {
        const LocalType *Named = Library.resolve(Type->Name);
        Type =
            Named && Named->Kind == TypeKind::Typedef ? Named->Target : nullptr;
      }
      if (Type && Type->Kind == TypeExprKind::Builtin)
        Base = Type->TheBuiltin;
    }
    for (const llvm::DWARFDie &Child : Die.children()) {
      if (Child.getTag() != dw::DW_TAG_enumerator)
        continue;
      const std::optional<llvm::DWARFFormValue> Value =
          Child.find(dw::DW_AT_const_value);
      if (!Value || !Child.getShortName()) {
        Why = Refusal{"an enumerator of no value"};
        return nullptr;
      }
      Enumerator Entry;
      Entry.Name = Child.getShortName();
      if (Value->getForm() == dw::DW_FORM_sdata) {
        const int64_t Number = Value->getAsSignedConstant().value_or(0);
        Signed |= Number < 0;
        Entry.Value = uint64_t(Number);
      } else {
        Entry.Value = Value->getAsUnsignedConstant().value_or(0);
      }
      Enum->Values.push_back(std::move(Entry));
    }
    if (!Base) {
      static constexpr Builtin SignedOrder[] = {
          Builtin::SChar, Builtin::Short, Builtin::Int, Builtin::LongLong};
      static constexpr Builtin UnsignedOrder[] = {
          Builtin::UChar, Builtin::UShort, Builtin::UInt, Builtin::ULongLong};
      for (Builtin Type : Signed ? SignedOrder : UnsignedOrder)
        if (builtinBytes(Type, Library.dataModel()) == Bytes)
          Base = Type;
    }
    if (!Base) {
      Why = Refusal{"an enumeration of " + std::to_string(Bytes) + " bytes"};
      return nullptr;
    }
    Enum->Underlying = *Base;
    // A signed enumeration's values extend their sign from its width.
    const unsigned Bits = builtinBytes(*Base, Library.dataModel()) * 8;
    if (builtinInfo(*Base).Signed && Bits && Bits < 64)
      for (Enumerator &Entry : Enum->Values)
        if (Entry.Value >> (Bits - 1) & 1)
          Entry.Value |= ~uint64_t(0) << Bits;
    return Enum;
  }

  /// Where a member begins, in bytes: a constant, or DWARF 2's expression
  /// that adds it to the record's address.
  static std::optional<uint64_t> memberBytes(const llvm::DWARFDie &Member) {
    const std::optional<llvm::DWARFFormValue> Value =
        Member.find(dw::DW_AT_data_member_location);
    if (!Value)
      return 0;
    if (std::optional<uint64_t> Bytes = Value->getAsUnsignedConstant())
      return Bytes;
    std::optional<llvm::ArrayRef<uint8_t>> Block = Value->getAsBlock();
    if (!Block || Block->empty())
      return std::nullopt;
    llvm::DataExtractor Data(*Block, /*IsLittleEndian=*/true, 8);
    llvm::DataExtractor::Cursor At(0);
    const uint8_t Op = Data.getU8(At);
    uint64_t Bytes = 0;
    if (Op == dw::DW_OP_plus_uconst) {
      Bytes = Data.getULEB128(At);
    } else if (Op == dw::DW_OP_constu) {
      Bytes = Data.getULEB128(At);
      if (Data.getU8(At) != dw::DW_OP_plus)
        return std::nullopt;
    } else {
      return std::nullopt;
    }
    if (!At || At.tell() != Block->size()) {
      llvm::consumeError(At.takeError());
      return std::nullopt;
    }
    return Bytes;
  }

  std::shared_ptr<const RecordDef> record(const llvm::DWARFDie &Die,
                                          std::optional<Refusal> &Why,
                                          unsigned Depth) {
    auto Record = std::make_shared<RecordDef>();
    Record->Union = Die.getTag() == dw::DW_TAG_union_type;
    Record->Size = unsignedOf(Die, dw::DW_AT_byte_size).value_or(0);
    Record->Alignment =
        uint32_t(unsignedOf(Die, dw::DW_AT_alignment).value_or(0));
    for (const llvm::DWARFDie &Child : Die.children()) {
      const dw::Tag Tag = Child.getTag();
      if (Tag == dw::DW_TAG_variant_part) {
        Why = Refusal{"variants, as a Rust enumeration's"};
        return nullptr;
      }
      if (Tag == dw::DW_TAG_inheritance) {
        if (Child.find(dw::DW_AT_virtuality)) {
          Why = Refusal{"a virtual base"};
          return nullptr;
        }
        const llvm::DWARFDie Base =
            Child.getAttributeValueAsReferencedDie(dw::DW_AT_type);
        const std::optional<uint64_t> Bytes = memberBytes(Child);
        TypeExprRef Type =
            Base.isValid() ? expr(Base, Why, Depth + 1, true) : nullptr;
        if (!Type || !Bytes) {
          if (!Why)
            Why = Refusal{"a base at no offset"};
          return nullptr;
        }
        Member Field;
        Field.Name = "baseclass_" + std::to_string(*Bytes);
        Field.Type = std::move(Type);
        Field.BitOffset = *Bytes * 8;
        Record->Members.push_back(std::move(Field));
        continue;
      }
      if (Tag != dw::DW_TAG_member)
        continue;
      // A static member is no part of the record's bytes.
      if (dw::toUnsigned(Child.find(dw::DW_AT_external), 0) ||
          dw::toUnsigned(Child.find(dw::DW_AT_declaration), 0))
        continue;
      const llvm::DWARFDie TypeDie =
          Child.getAttributeValueAsReferencedDie(dw::DW_AT_type);
      if (!TypeDie.isValid()) {
        Why = Refusal{"a member of no type"};
        return nullptr;
      }
      Member Field;
      if (const char *Name = Child.getShortName())
        Field.Name = Name;
      Field.Type = expr(TypeDie, Why, Depth + 1, /*ByValue=*/true);
      if (!Field.Type)
        return nullptr;
      const std::optional<uint64_t> BitSize =
          unsignedOf(Child, dw::DW_AT_bit_size);
      // An unnamed member of a named type is no anonymous member C reaches
      // through: it takes a name of its own.  An unnamed bit-field pads.
      if (Field.Name.empty() && !BitSize &&
          Field.Type->Kind != TypeExprKind::Record)
        Field.Name = "__unnamed" + std::to_string(Record->Members.size());
      if (std::optional<uint64_t> Bits =
              unsignedOf(Child, dw::DW_AT_data_bit_offset)) {
        Field.BitOffset = *Bits;
      } else if (std::optional<uint64_t> Bytes = memberBytes(Child)) {
        Field.BitOffset = *Bytes * 8;
        // DWARF 2 and 3 count a bit-field's bits from the top of the
        // storage unit at that offset, which little-endian targets fill
        // from the bottom.
        if (BitSize)
          if (std::optional<uint64_t> Top =
                  unsignedOf(Child, dw::DW_AT_bit_offset)) {
            const uint64_t Unit =
                unsignedOf(Child, dw::DW_AT_byte_size)
                    .value_or(Library.sizeOf(*Field.Type).value_or(0));
            if (*Top + *BitSize > Unit * 8) {
              Why = Refusal{"a bit-field outside its storage"};
              return nullptr;
            }
            Field.BitOffset += Unit * 8 - *Top - *BitSize;
          }
      } else {
        Why = Refusal{"a member at an offset DWARF computes"};
        return nullptr;
      }
      if (BitSize) {
        if (!*BitSize) {
          Why = Refusal{"a bit-field of no width"};
          return nullptr;
        }
        Field.BitWidth = uint32_t(*BitSize);
        if (!bitStorage(Field)) {
          Why = Refusal{"a bit-field that crosses its storage unit"};
          return nullptr;
        }
      } else if (Field.BitOffset % 8) {
        Why = Refusal{"a member that does not begin at a byte"};
        return nullptr;
      }
      Record->Members.push_back(std::move(Field));
    }
    // Rust lays a structure's fields out in an order of its own, which
    // DWARF lists in the source's: a structure holds them by offset.
    if (!Record->Union)
      std::stable_sort(Record->Members.begin(), Record->Members.end(),
                       [](const Member &A, const Member &B) {
                         return A.BitOffset < B.BitOffset;
                       });
    inferPacking(*Record);
    if (std::string Problem = layoutProblem(Library, *Record);
        !Problem.empty()) {
      Why = Refusal{Problem};
      return nullptr;
    }
    return Record;
  }

  /// Gives \p Field, a bit-field, the storage unit it lives in, as the
  /// layout of a record its source declares does (layOutRecord): the unit
  /// of its type's size that holds it, else, in a packed record that lets
  /// it cross such units, the bytes from the one that holds its first bit.
  /// False where neither holds it.
  bool bitStorage(Member &Field) const {
    const std::optional<Builtin> Own = integerOf(*Field.Type);
    if (!Own)
      return false;
    const uint64_t Bits = builtinBytes(*Own, Library.dataModel()) * 8;
    if (!Bits || Field.BitWidth > Bits)
      return false;
    Field.StorageBitOffset = Field.BitOffset / Bits * Bits;
    if (Field.BitOffset + Field.BitWidth <= Field.StorageBitOffset + Bits)
      return true;
    Field.StorageBitOffset = Field.BitOffset / 8 * 8;
    return Field.BitOffset + Field.BitWidth <= Field.StorageBitOffset + Bits;
  }

  /// The integer builtin \p Type is, through typedefs and enumerations.
  std::optional<Builtin> integerOf(const TypeExpr &Type) const {
    const TypeExpr *At = &Type;
    for (unsigned Hop = 0; At && Hop < MaxDepth; ++Hop) {
      switch (At->Kind) {
      case TypeExprKind::Builtin: {
        const BuiltinClass Class = builtinInfo(At->TheBuiltin).Class;
        if (Class == BuiltinClass::Integer ||
            Class == BuiltinClass::Character || Class == BuiltinClass::Bool)
          return At->TheBuiltin;
        return std::nullopt;
      }
      case TypeExprKind::Enum:
        return At->Enum ? std::optional<Builtin>(At->Enum->Underlying)
                        : std::nullopt;
      case TypeExprKind::Named: {
        const LocalType *Named = Library.find(At->Name);
        if (!Named)
          return std::nullopt;
        if (Named->Kind == TypeKind::Enum)
          return Named->Enum ? std::optional<Builtin>(Named->Enum->Underlying)
                             : std::nullopt;
        if (Named->Kind != TypeKind::Typedef)
          return std::nullopt;
        At = Named->Target.get();
        continue;
      }
      default:
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  /// The packing that lays \p Record's members out where DWARF says, where
  /// their alignment would place one further, and the alignment it was
  /// declared with, where its size goes past what its members need.
  void inferPacking(RecordDef &Record) const {
    const auto divisor = [](uint64_t Value) {
      return uint32_t(std::min<uint64_t>(Value & (~Value + 1), 16));
    };
    const auto alignUp = [](uint64_t Value, uint64_t Alignment) {
      return (Value + Alignment - 1) / Alignment * Alignment;
    };
    uint32_t Natural = 1;
    uint32_t Pack = 0;
    uint64_t End = 0;
    for (const Member &Field : Record.Members) {
      const uint32_t Alignment =
          std::max<uint32_t>(1, Library.alignOf(*Field.Type));
      Natural = std::max(Natural, Alignment);
      const uint64_t Byte =
          (Field.isBitfield() ? Field.StorageBitOffset : Field.BitOffset) / 8;
      End = std::max(End, Field.isBitfield()
                              ? (Field.BitOffset + Field.BitWidth + 7) / 8
                              : Byte + Library.sizeOf(*Field.Type).value_or(0));
      if (Alignment > 1 && Byte % Alignment) {
        const uint32_t Allowed = divisor(Byte);
        if (!Pack || Allowed < Pack)
          Pack = Allowed;
      }
    }
    uint32_t Aligned = Pack ? std::min(Natural, Pack) : Natural;
    if (Record.Size && Record.Size % Aligned) {
      const uint32_t Allowed = divisor(Record.Size);
      if (!Pack || Allowed < Pack)
        Pack = Allowed;
      Aligned = std::min(Natural, Pack);
    }
    Record.Pack = Pack;
    if (!Record.Alignment && Record.Size > alignUp(End, Aligned))
      for (uint64_t Candidate = uint64_t(Aligned) * 2; Candidate <= 4096;
           Candidate *= 2)
        if (alignUp(End, Candidate) == Record.Size) {
          Record.Alignment = uint32_t(Candidate);
          break;
        }
  }

  TypeLibrary &Library;
  DWARFTypeImportStats &Stats;
  std::deque<llvm::DWARFDie> Pending;
  llvm::DenseSet<const llvm::DWARFDebugInfoEntry *> Visited;
  llvm::StringSet<> Defined, Refused, Conflicting;
  llvm::StringMap<TypeKind> Declared;
  /// The typedef that names each anonymous record, by its entry.
  llvm::DenseMap<const llvm::DWARFDebugInfoEntry *, std::string> AnonymousNames;
};

} // namespace

llvm::Expected<DWARFTypeImportStats>
importDWARFTypes(llvm::DWARFContext &Context, TypeLibrary &Into) {
  DWARFTypeImportStats Stats;
  Importer Import(Into, Stats);
  for (const std::unique_ptr<llvm::DWARFUnit> &Unit :
       Context.info_section_units())
    Import.unit(*Unit);
  for (const std::unique_ptr<llvm::DWARFUnit> &Unit :
       Context.types_section_units())
    Import.unit(*Unit);
  Import.finish();
  return Stats;
}

llvm::Expected<std::unique_ptr<TypeLibrary>>
importDWARFTypes(llvm::DWARFContext &Context, DataModel Model,
                 DWARFTypeImportStats *Stats) {
  auto Library = std::make_unique<TypeLibrary>(Model);
  llvm::Expected<DWARFTypeImportStats> Read =
      importDWARFTypes(Context, *Library);
  if (!Read)
    return Read.takeError();
  if (Stats)
    *Stats = std::move(*Read);
  return std::move(Library);
}

} // namespace neverd::types
