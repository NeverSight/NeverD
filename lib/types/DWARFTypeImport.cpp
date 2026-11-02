//===- DWARFTypeImport.cpp - Local types from DWARF type entries ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/DWARFTypeImport.h"

#include "neverd/types/Limits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
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
#include <limits>
#include <optional>
#include <string>

namespace neverd::types {
namespace {

namespace dw = llvm::dwarf;

std::optional<uint64_t> unsignedOf(const llvm::DWARFDie &Die,
                                   dw::Attribute Attribute) {
  return dw::toUnsigned(Die.find(Attribute));
}

bool isRecordTag(dw::Tag Tag) {
  return Tag == dw::DW_TAG_structure_type || Tag == dw::DW_TAG_class_type ||
         Tag == dw::DW_TAG_union_type;
}

/// The language of \p Die's unit (DW_LANG_*), or none.
uint64_t languageOf(const llvm::DWARFDie &Die) {
  llvm::DWARFUnit *Unit = Die.getDwarfUnit();
  return Unit ? dw::toUnsigned(Unit->getUnitDIE().find(dw::DW_AT_language), 0)
              : 0;
}

/// Whether \p Die is of a Go unit, whose functions list results as parameters.
bool ofGo(const llvm::DWARFDie &Die) {
  return languageOf(Die) == dw::DW_LANG_Go;
}

struct GoFunctionArity {
  size_t Inputs;
  size_t Results;
};

/// Older Go describes results as trailing pointer parameters without marking
/// them. Its anonymous function type's name still records both lists. Count
/// those lists, keeping nested types and quoted struct tags together; the
/// actual types continue to come from the DIE references.
std::optional<GoFunctionArity> goFunctionArity(llvm::StringRef Name) {
  if (!Name.consume_front("func"))
    return std::nullopt;
  const auto list = [](llvm::StringRef &Text) -> std::optional<size_t> {
    if (!Text.consume_front("("))
      return std::nullopt;
    llvm::SmallVector<char, 8> Closing{')'};
    size_t Count = 0, Start = 0;
    char Quote = 0;
    for (size_t I = 0; I < Text.size(); ++I) {
      const char C = Text[I];
      if (Quote) {
        if (C == '\\' && Quote == '"')
          ++I;
        else if (C == Quote)
          Quote = 0;
        continue;
      }
      if (C == '"' || C == '`') {
        Quote = C;
      } else if (C == '(' || C == '[' || C == '{') {
        Closing.push_back(C == '(' ? ')' : C == '[' ? ']' : '}');
      } else if (C == ')' || C == ']' || C == '}') {
        if (C != Closing.back())
          return std::nullopt;
        Closing.pop_back();
        if (Closing.empty()) {
          const bool Empty = Text.slice(Start, I).trim().empty();
          if (Empty && Count)
            return std::nullopt;
          Text = Text.drop_front(I + 1).trim();
          return Count + !Empty;
        }
      } else if (C == ',' && Closing.size() == 1) {
        if (Text.slice(Start, I).trim().empty())
          return std::nullopt;
        ++Count;
        Start = I + 1;
      }
    }
    return std::nullopt;
  };
  const auto Inputs = list(Name);
  if (!Inputs)
    return std::nullopt;
  if (Name.empty())
    return GoFunctionArity{*Inputs, 0};
  if (Name.starts_with("(")) {
    const auto Results = list(Name);
    if (Results && Name.empty())
      return GoFunctionArity{*Inputs, *Results};
    return std::nullopt;
  }
  const std::string Result = "(" + Name.str() + ")";
  llvm::StringRef ResultList = Result;
  const auto Results = list(ResultList);
  if (Results == 1 && ResultList.empty())
    return GoFunctionArity{*Inputs, 1};
  return std::nullopt;
}

/// The type \p Die names, through its typedefs and qualifiers.
llvm::DWARFDie heldType(llvm::DWARFDie Die) {
  for (unsigned Hops = 0;
       Die.isValid() &&
       Hops < ::neverd::limits::types::kDWARFTypeImportMaxDepth;
       ++Hops) {
    switch (Die.getTag()) {
    case dw::DW_TAG_typedef:
    case dw::DW_TAG_const_type:
    case dw::DW_TAG_volatile_type:
    case dw::DW_TAG_restrict_type:
    case dw::DW_TAG_atomic_type:
      Die = Die.getAttributeValueAsReferencedDie(dw::DW_AT_type);
      continue;
    default:
      return Die;
    }
  }
  return llvm::DWARFDie();
}

/// The bytes a value of type \p Die holds, as DWARF says them, which it says
/// of a type the library may not hold yet; none where it says none.
std::optional<uint64_t> declaredBytes(const llvm::DWARFDie &Die) {
  const llvm::DWARFDie Held = heldType(Die);
  return Held.isValid() ? unsignedOf(Held, dw::DW_AT_byte_size) : std::nullopt;
}

/// Whether \p Die, a type, is a pointer to a function.
bool pointsToFunction(const llvm::DWARFDie &Die) {
  const llvm::DWARFDie Held = heldType(Die);
  return Held.getTag() == dw::DW_TAG_pointer_type &&
         heldType(Held.getAttributeValueAsReferencedDie(dw::DW_AT_type))
                 .getTag() == dw::DW_TAG_subroutine_type;
}

/// Whether \p Die, a record, holds a variant part, as a Rust enumeration
/// with data does.
bool hasVariants(const llvm::DWARFDie &Die) {
  for (const llvm::DWARFDie &Child : Die.children())
    if (Child.getTag() == dw::DW_TAG_variant_part)
      return true;
  return false;
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
    // A record or enumeration a typedef of its name names, which C may then
    // name by that alone.
    for (const auto &Entry : NamedByTypedef)
      if (const LocalType *Named = Library.find(Entry.getKey());
          Named && Named->Origin == TypeOrigin::DWARF && !Named->TypedefNamed &&
          (Named->Record || Named->Enum)) {
        LocalType Marked = *Named;
        Marked.TypedefNamed = true;
        Library.define(std::move(Marked));
      }
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
      if (hasVariants(Die))
        Local.Comment = "a Rust enumeration: its variants, which local "
                        "types do not spell, are its bytes";
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
         Hop < ::neverd::limits::types::kDWARFTypeImportMaxDepth &&
         qualifiedName(Target) == Name;
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
          NamedByTypedef.insert(Name);
          return;
        }
        if (Naming->second == Name)
          return;
      } else if (TargetName == Name) {
        NamedByTypedef.insert(Name);
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
    if (Depth > ::neverd::limits::types::kDWARFTypeImportMaxDepth) {
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
      else if (Tagged && TargetName == Name)
        // A typedef of the record of its own name, as Go gives each of its
        // types and C's `typedef struct x x;` does, is that record, which
        // what holds it by value lays out by.
        later(Target);
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
    case dw::DW_TAG_subroutine_type: {
      // A subroutine type with a size is a value of it: a Go func value,
      // the address of the closure that calls the function.  Several
      // results, which C returns as no one value, leave it an address.
      const std::optional<uint64_t> Bytes =
          unsignedOf(Die, dw::DW_AT_byte_size);
      if (!Bytes)
        return function(Die, Why, Depth);
      const llvm::DWARFUnit *Unit = Die.getDwarfUnit();
      if (!Unit || *Bytes != Unit->getAddressByteSize()) {
        Why =
            Refusal{"a function value of " + std::to_string(*Bytes) + " bytes"};
        return nullptr;
      }
      TypeExprRef Function = function(Die, Why, Depth);
      if (!Function && Why)
        return nullptr;
      return makePointer(Function ? std::move(Function)
                                  : makeBuiltin(Builtin::Void),
                         unsigned(*Bytes));
    }
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
    const auto fits = [&](Builtin Type) {
      return Library.builtinBytes(Type) == Bytes;
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
      // long double by its name too where it is as wide as a double, as
      // 32-bit ARM's and Microsoft's are.
      if (Name == "long double" && fits(Builtin::LongDouble)) {
        Found = Builtin::LongDouble;
        break;
      }
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
          if (High && *High >= Lower) {
            // Subtract unsigned representations: even ordered signed bounds
            // can have a difference greater than INT64_MAX.
            const uint64_t Span = uint64_t(*High) - uint64_t(Lower);
            if (Span == std::numeric_limits<uint64_t>::max()) {
              Why = Refusal{"an array bound range that exceeds 64 bits"};
              return nullptr;
            }
            Count = Span + 1;
          }
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

  /// A function type.  Go lists a function's results as parameters marked
  /// DW_AT_variable_parameter, or as unmarked pointers in older DWARF. One
  /// is its result, and several, which C returns as no one value, leave no
  /// function, with no reason. An unmarked named type without a signature
  /// does too: its input pointers cannot be told apart from result pointers.
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
    const bool Go = ofGo(Die);
    std::optional<GoFunctionArity> Legacy;
    if (Go && !Return.isValid() &&
        !llvm::any_of(Die.children(), [](const llvm::DWARFDie &Child) {
          return Child.getTag() == dw::DW_TAG_formal_parameter &&
                 Child.find(dw::DW_AT_variable_parameter).has_value();
        })) {
      const llvm::StringRef Name(Die.getShortName() ? Die.getShortName() : "");
      Legacy = goFunctionArity(Name);
      if (!Legacy) {
        if (Name.starts_with("func("))
          Why = Refusal{"an incomplete Go function signature"};
        return nullptr;
      }
    }
    size_t Parameters = 0;
    unsigned Results = 0;
    for (const llvm::DWARFDie &Child : Die.children()) {
      if (Child.getTag() == dw::DW_TAG_unspecified_parameters) {
        Function->Variadic = true;
        continue;
      }
      if (Child.getTag() != dw::DW_TAG_formal_parameter)
        continue;
      const bool LegacyResult = Legacy && Parameters >= Legacy->Inputs;
      ++Parameters;
      if (LegacyResult ||
          (Go && dw::toUnsigned(Child.find(dw::DW_AT_variable_parameter), 0))) {
        llvm::DWARFDie Result =
            Child.getAttributeValueAsReferencedDie(dw::DW_AT_type);
        if (LegacyResult) {
          Result = heldType(Result);
          if (Result.getTag() != dw::DW_TAG_pointer_type) {
            Why = Refusal{"a legacy Go result that is not a pointer"};
            return nullptr;
          }
          Result = Result.getAttributeValueAsReferencedDie(dw::DW_AT_type);
        }
        if (++Results > 1)
          continue;
        Function->Return =
            Result.isValid() ? expr(Result, Why, Depth + 1, false) : nullptr;
        if (!Function->Return) {
          if (!Why)
            Why = Refusal{"a result of no type"};
          return nullptr;
        }
        continue;
      }
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
    if (Legacy && Parameters != Legacy->Inputs + Legacy->Results) {
      Why = Refusal{"a Go signature that disagrees with its parameters"};
      return nullptr;
    }
    if (Results > 1)
      return nullptr;
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
      TypeExprRef Type =
          expr(Underlying, Ignored,
               ::neverd::limits::types::kDWARFTypeImportMaxDepth - 4, true);
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
        if (Library.builtinBytes(Type) == Bytes)
          Base = Type;
    }
    if (!Base) {
      Why = Refusal{"an enumeration of " + std::to_string(Bytes) + " bytes"};
      return nullptr;
    }
    Enum->Underlying = *Base;
    // A signed enumeration's values extend their sign from its width.
    const unsigned Bits = Library.builtinBytes(*Base) * 8;
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

  /// A Rust enumeration with data, whose variants local types do not spell:
  /// its bytes, as large and as aligned as the enumeration, so that what
  /// holds it lies where the compiler laid it.
  std::shared_ptr<const RecordDef> variantBytes(const llvm::DWARFDie &Die,
                                                std::optional<Refusal> &Why) {
    const std::optional<uint64_t> Bytes = unsignedOf(Die, dw::DW_AT_byte_size);
    const uint64_t Align = unsignedOf(Die, dw::DW_AT_alignment).value_or(0);
    if (!Bytes || !Align || (Align & (Align - 1))) {
      Why = Refusal{"variants, as a Rust enumeration's, of no stated "
                    "alignment"};
      return nullptr;
    }
    auto Record = std::make_shared<RecordDef>();
    Record->Size = *Bytes;
    Record->Alignment = uint32_t(Align);
    if (*Bytes) {
      Member Field;
      Field.Name = "__variants";
      Field.Type = makeArray(makeBuiltin(Builtin::UChar), *Bytes);
      Record->Members.push_back(std::move(Field));
    }
    return Record;
  }

  std::shared_ptr<const RecordDef> record(const llvm::DWARFDie &Die,
                                          std::optional<Refusal> &Why,
                                          unsigned Depth) {
    auto Record = std::make_shared<RecordDef>();
    Record->Union = Die.getTag() == dw::DW_TAG_union_type;
    Record->Size = unsignedOf(Die, dw::DW_AT_byte_size).value_or(0);
    Record->Alignment =
        uint32_t(unsignedOf(Die, dw::DW_AT_alignment).value_or(0));
    // Where a Rust record's members begin, and its last, which rustc's DWARF
    // describes otherwise than they are (rustMember).
    const bool Rust = languageOf(Die) == dw::DW_LANG_Rust;
    std::vector<uint64_t> Starts;
    uint64_t LastMember = 0;
    if (Rust)
      for (const llvm::DWARFDie &Child : Die.children())
        if (Child.getTag() == dw::DW_TAG_member)
          if (const std::optional<uint64_t> Bytes = memberBytes(Child)) {
            Starts.push_back(*Bytes);
            LastMember = Child.getOffset();
          }
    for (const llvm::DWARFDie &Child : Die.children()) {
      const dw::Tag Tag = Child.getTag();
      if (Tag == dw::DW_TAG_variant_part)
        return variantBytes(Die, Why);
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
      if (Rust && !BitSize &&
          !rustMember(Field, TypeDie, Record->Size, Starts,
                      Child.getOffset() == LastMember))
        continue;
      // A member that holds no bytes, as Rust's () and PhantomData, lies
      // nowhere a record's bytes say; C spells none.  An array of none, as
      // a trailing `data[0]`, still names the bytes after it.  DWARF says so
      // of a type the library may hold only later.
      const bool Void = Field.Type->Kind == TypeExprKind::Builtin &&
                        Field.Type->TheBuiltin == Builtin::Void;
      if (!BitSize && Field.Type->Kind != TypeExprKind::Array &&
          (Void || declaredBytes(TypeDie) == std::optional<uint64_t>(0) ||
           Library.sizeOf(*Field.Type) == std::optional<uint64_t>(0)))
        continue;
      Record->Members.push_back(std::move(Field));
    }
    distinctNames(*Record);
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

  /// \p Field, a member of a Rust record of \p RecordBytes whose members
  /// begin at \p Starts, as what it is where rustc's DWARF describes it as
  /// something else; false for one that holds nothing.
  /// - A function item holds no bytes, but DWARF gives it as a pointer to
  ///   its function, as it gives a function pointer: a pointer to a function
  ///   the record leaves no room for, as it leaves none past its end or where
  ///   another member begins, is one.
  /// - An unsized tail, as a slice `[u8]`, DWARF gives as its element: the
  ///   last member, reaching past the record's end, is an array of unknown
  ///   bound of it, C's flexible array.
  bool rustMember(Member &Field, const llvm::DWARFDie &TypeDie,
                  uint64_t RecordBytes, llvm::ArrayRef<uint64_t> Starts,
                  bool Last) const {
    const uint64_t At = Field.BitOffset / 8;
    if (pointsToFunction(TypeDie)) {
      const uint64_t Width = pointerBytesOf(heldType(TypeDie));
      const size_t Overlaps = llvm::count_if(Starts, [&](uint64_t Start) {
        return Start >= At && Start < At + Width;
      });
      return At + Width <= RecordBytes && Overlaps <= 1;
    }
    if (!Last || At > RecordBytes || Field.Type->Kind == TypeExprKind::Array)
      return true;
    std::optional<uint64_t> Bytes = Library.sizeOf(*Field.Type);
    if (!Bytes)
      Bytes = declaredBytes(TypeDie);
    if (Bytes && *Bytes && At + *Bytes > RecordBytes)
      Field.Type = makeArray(std::move(Field.Type), std::nullopt);
    return true;
  }

  /// Numbers the second and later members of a name another has, as Go's
  /// blank fields `_` are, past any name taken; the comment keeps it.
  static void distinctNames(RecordDef &Record) {
    llvm::StringSet<> Taken;
    for (const Member &Field : Record.Members)
      if (!Field.Name.empty())
        Taken.insert(Field.Name);
    llvm::StringMap<unsigned> Seen;
    for (Member &Field : Record.Members) {
      if (Field.Name.empty() || Seen[Field.Name]++ == 0)
        continue;
      unsigned Number = Seen[Field.Name] - 1;
      std::string Name;
      do
        Name = Field.Name + std::to_string(Number++);
      while (!Taken.insert(Name).second);
      Field.Comment = Field.Comment.empty()
                          ? "DWARF names it " + Field.Name
                          : Field.Comment + "; DWARF names it " + Field.Name;
      Field.Name = std::move(Name);
    }
  }

  /// Gives \p Field, a bit-field, the storage unit it lives in, as the
  /// layout of a record its source declares does (layOutRecord): the unit
  /// of its type's size, at its type's alignment, that holds it, else, in
  /// a packed record that lets it cross such units, the bytes from the one
  /// that holds its first bit.  False where neither holds it.
  bool bitStorage(Member &Field) const {
    const std::optional<Builtin> Own = integerOf(*Field.Type);
    if (!Own)
      return false;
    const uint64_t Bits = Library.builtinBytes(*Own) * 8;
    if (!Bits || Field.BitWidth > Bits)
      return false;
    const uint64_t Span = uint64_t(Library.alignOf(*makeBuiltin(*Own))) * 8;
    Field.StorageBitOffset = Field.BitOffset / Span * Span;
    if (Field.BitOffset + Field.BitWidth <= Field.StorageBitOffset + Bits)
      return true;
    Field.StorageBitOffset = Field.BitOffset / 8 * 8;
    return Field.BitOffset + Field.BitWidth <= Field.StorageBitOffset + Bits;
  }

  /// The integer builtin \p Type is, through typedefs and enumerations.
  std::optional<Builtin> integerOf(const TypeExpr &Type) const {
    const TypeExpr *At = &Type;
    for (unsigned Hop = 0;
         At && Hop < ::neverd::limits::types::kDWARFTypeImportMaxDepth; ++Hop) {
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
      for (uint64_t Candidate = uint64_t(Aligned) * 2;
           Candidate <= ::neverd::limits::types::kMaxInferredRecordAlignment;
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
  /// The records and enumerations a typedef of their name names.
  llvm::StringSet<> NamedByTypedef;
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

std::optional<unsigned> dwarfLongDoubleBytes(llvm::DWARFContext &Context) {
  // Compilers put base types beside the unit's other top-level entries.
  for (const auto &Unit : Context.compile_units())
    for (const llvm::DWARFDie &Die : Unit->getUnitDIE(false).children())
      if (Die.getTag() == dw::DW_TAG_base_type &&
          llvm::StringRef(Die.getShortName()) == "long double")
        if (const std::optional<uint64_t> Bytes =
                llvm::dwarf::toUnsigned(Die.find(dw::DW_AT_byte_size)))
          return static_cast<unsigned>(*Bytes);
  return std::nullopt;
}

} // namespace neverd::types
