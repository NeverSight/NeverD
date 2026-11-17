//===- ViewPlan.cpp - Records read as views over their bytes --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ViewPlan.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>

namespace neverd::types::render {
namespace {

std::string hex(uint64_t Value) {
  std::string Text;
  llvm::raw_string_ostream(Text)
      << "0x" << llvm::format_hex_no_prefix(Value, 1, /*Upper=*/true);
  return Text;
}

const RecordPlan *nested(const RecordPlan &Plan, llvm::StringRef Name) {
  for (const RecordPlan &Inner : Plan.Nested)
    if (Inner.Name == Name)
      return &Inner;
  return nullptr;
}

/// Whether \p Place's members are its parent's, as an anonymous record's
/// are in C.
bool flattens(const Slot &Place) {
  return Place.TheKind == Slot::Kind::Field && Place.Field->isAnonymous() &&
         Place.Bounds.empty();
}

} // namespace

/// The class whose accessors are being planned.
struct ViewPlanner::State {
  ViewClass &Class;
  llvm::StringSet<> Taken;

  /// \p Name, or with underscores until no other member of the class has
  /// it.
  std::string unique(std::string Name) {
    while (!Taken.insert(Name).second)
      Name += "_";
    return Name;
  }
  /// The offset a comment gives, when comments give them.
  std::string where(const RenderOptions &Options, uint64_t Byte) const {
    return Options.Offsets ? offsetText(Byte, Options, Class.Digits) + " " : "";
  }
};

ViewPlanner::ViewPlanner(const TypeLibrary &Library,
                         const RenderOptions &Options, Language Lang,
                         std::vector<std::string> Reserved)
    : Library(Library), Options(Options), Spelling(spellingOf(Options)),
      Names(Library, Lang),
      PointerBytes(types::pointerBytes(Library.dataModel())),
      C(Library,
        [&] {
          RenderOptions C = Options;
          C.Lang = Language::C;
          C.Spelling = spellingOf(Options);
          return C;
        }()),
      Reserved(std::move(Reserved)) {}

std::optional<ViewValue> ViewPlanner::builtin(Builtin Type,
                                              bool Spelled) const {
  const BuiltinInfo &Info = builtinInfo(Type);
  ViewValue Value;
  Value.Bytes = Library.builtinBytes(Type);
  Value.Signed = Info.Signed && Info.Class != BuiltinClass::Bool;
  const auto integer = [&] {
    const unsigned Bytes = Value.Bytes;
    Value.TheForm =
        Bytes == 1 || Bytes == 2 || Bytes == 4 || Bytes == 8 || Bytes == 16
            ? ViewValue::Form::Integer
            : ViewValue::Form::Bytes;
  };
  switch (Info.Class) {
  case BuiltinClass::Void:
    return std::nullopt;
  case BuiltinClass::Bool:
    if (Value.Bytes == 1)
      Value.TheForm = ViewValue::Form::Bool;
    else
      integer();
    break;
  case BuiltinClass::Float:
    // Half reads as its bits, which neither language reads as a float.
    if (Type == Builtin::Half) {
      Value.Signed = false;
      integer();
    } else {
      Value.TheForm = Value.Bytes == 4 || Value.Bytes == 8
                          ? ViewValue::Form::Float
                          : ViewValue::Form::Bytes;
    }
    break;
  case BuiltinClass::Character:
  case BuiltinClass::Integer:
    integer();
    break;
  case BuiltinClass::Complex:
    Value.TheForm = ViewValue::Form::Bytes;
    break;
  }
  // Windows' name, which the module declares.
  if (Spelled && Spelling == BuiltinSpelling::Windows && windowsTypedef(Type) &&
      Value.TheForm != ViewValue::Form::Bool) {
    Value.Named = builtinInfo(Type).Windows;
    Value.Windows = Type;
  }
  return Value;
}

const LocalType *ViewPlanner::namedRecord(const TypeExpr &Type) const {
  if (Type.Kind != TypeExprKind::Named)
    return nullptr;
  const LocalType *Named = Library.resolve(Type.Name);
  return Named && Named->Record &&
                 (Named->Kind == TypeKind::Struct ||
                  Named->Kind == TypeKind::Union)
             ? Named
             : nullptr;
}

std::optional<ViewValue> ViewPlanner::value(const TypeExpr &Type) const {
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    return builtin(Type.TheBuiltin);
  case TypeExprKind::Named: {
    const LocalType *Named = Library.find(Type.Name);
    if (!Named)
      return std::nullopt;
    std::optional<ViewValue> Value;
    if (Named->Kind == TypeKind::Enum && Named->Enum)
      Value = builtin(Named->Enum->Underlying);
    else if (Named->Kind == TypeKind::Typedef && Named->Target)
      Value = value(*Named->Target);
    // The program's own name, which the module declares; a boolean keeps
    // the language's.
    if (Value && Value->TheForm != ViewValue::Form::Bool)
      Value->Named = Names.type(Type.Name);
    return Value;
  }
  case TypeExprKind::Pointer: {
    // The address a pointer holds.
    ViewValue Value;
    Value.TheForm = ViewValue::Form::Integer;
    Value.Bytes = PointerBytes;
    Value.Address = true;
    return Value;
  }
  case TypeExprKind::Enum:
    if (Type.Enum)
      return builtin(Type.Enum->Underlying);
    return std::nullopt;
  case TypeExprKind::Array:
  case TypeExprKind::Function:
  case TypeExprKind::Record:
    return std::nullopt;
  }
  return std::nullopt;
}

const TypeExpr &ViewPlanner::element(const TypeExpr &Type,
                                     std::vector<uint64_t> &Bounds) const {
  const TypeExpr *Element = &Type;
  while (true) {
    if (Element->Kind == TypeExprKind::Array && Element->Element) {
      Bounds.push_back(Element->Count.value_or(0));
      Element = Element->Element.get();
      continue;
    }
    // A typedef of an array is an array too.
    if (Element->Kind == TypeExprKind::Named)
      if (const LocalType *Named = Library.find(Element->Name);
          Named && Named->Kind == TypeKind::Typedef && Named->Target) {
        std::vector<uint64_t> More;
        const TypeExpr &Inner = element(*Named->Target, More);
        if (!More.empty()) {
          Bounds.insert(Bounds.end(), More.begin(), More.end());
          Element = &Inner;
          continue;
        }
      }
    return *Element;
  }
}

ViewTypedef ViewPlanner::typedefinition(const LocalType &Type) const {
  ViewTypedef Def;
  Def.Name = Names.type(Type.Name);
  if (Type.Kind == TypeKind::Function) {
    Def.TheKind = ViewTypedef::Kind::Function;
    if (Type.Function)
      Def.Declared = C.declare(*makeFunction(Type.Function));
    return Def;
  }
  if (!Type.Target)
    return Def;
  const TypeExpr &Target = *Type.Target;
  Def.Declared = C.declare(Target);
  if (const LocalType *Record = namedRecord(Target)) {
    Def.TheKind = ViewTypedef::Kind::Record;
    Def.Target = Names.type(Record->Name);
    Def.Named = Names.type(Target.Name);
    return Def;
  }
  if (Target.Kind == TypeExprKind::Named)
    if (const LocalType *Named = Library.resolve(Target.Name);
        Named && Named->Kind == TypeKind::Enum && Named->Enum) {
      Def.TheKind = ViewTypedef::Kind::Enum;
      Def.Target = Names.type(Named->Name);
      Def.Named = Names.type(Target.Name);
      return Def;
    }
  std::vector<uint64_t> Bounds;
  element(Target, Bounds);
  if (!Bounds.empty()) {
    Def.TheKind = ViewTypedef::Kind::Array;
    return Def;
  }
  if (std::optional<ViewValue> Value = value(Target)) {
    Def.TheKind = ViewTypedef::Kind::Value;
    Def.Pointer = Target.Kind == TypeExprKind::Pointer;
    Def.Value = std::move(*Value);
  }
  return Def;
}

void ViewPlanner::field(const Slot &Place, uint64_t At, State &Into) const {
  const Member &Field = *Place.Field;
  if (Place.Name.empty())
    return;
  ViewAccessor Accessor;
  Accessor.Name = Into.unique(Place.Name);
  Accessor.Offset = At;
  const TypeExpr &Element = element(*Field.Type, Accessor.Bounds);
  // What C declares: a record or enumeration defined in place by the name
  // of its class.
  std::string Declared;
  if (!Place.NestedType.empty()) {
    Declared =
        std::string(Element.Kind == TypeExprKind::Enum        ? "enum"
                    : Element.Record && Element.Record->Union ? "union"
                                                              : "struct") +
        " " + Place.NestedType + " " + Accessor.Name;
    for (uint64_t Bound : Accessor.Bounds)
      Declared += "[" + (Bound ? std::to_string(Bound) : std::string()) + "]";
  } else {
    Declared = C.declare(*Field.Type, Field.Name);
  }
  Accessor.Declared = Into.where(Options, At) + Declared;

  const uint64_t ElementBytes = Library.sizeOf(Element).value_or(0);
  for (size_t I = 0; I < Accessor.Bounds.size(); ++I) {
    uint64_t Stride = ElementBytes;
    for (size_t J = I + 1; J < Accessor.Bounds.size(); ++J)
      Stride *= Accessor.Bounds[J];
    Accessor.Strides.push_back(Stride);
  }
  // The class a record in place reads through.
  if (Element.Kind == TypeExprKind::Record && !Place.NestedType.empty())
    Accessor.Class = Place.NestedType;
  else if (const LocalType *Record = namedRecord(Element))
    Accessor.Class = Names.type(Record->Name);
  if (!Accessor.Class.empty()) {
    Accessor.TheKind = ViewAccessor::Kind::Record;
    Accessor.Named = Element.Kind == TypeExprKind::Named
                         ? Names.type(Element.Name)
                         : Accessor.Class;
    Accessor.Size = ElementBytes;
    Into.Class.Accessors.push_back(std::move(Accessor));
    return;
  }
  std::optional<ViewValue> Value = value(Element);
  if (!Value) {
    Accessor.TheKind = ViewAccessor::Kind::Unread;
    Into.Class.Accessors.push_back(std::move(Accessor));
    return;
  }
  if (Element.Kind == TypeExprKind::Enum && !Place.NestedType.empty())
    Value->Named = Place.NestedType;
  Accessor.Value = std::move(*Value);
  // An element of an array writes through a method of its own.
  if (!Accessor.Bounds.empty() &&
      Accessor.Value.TheForm != ViewValue::Form::Bytes)
    Accessor.Setter =
        Into.unique("set" + std::string(1, llvm::toUpper(Accessor.Name[0])) +
                    Accessor.Name.substr(1));
  Into.Class.Accessors.push_back(std::move(Accessor));
}

void ViewPlanner::bitfields(const Slot &Place, uint64_t At, State &Into) const {
  // A view reads bytes where it likes: each field in the unit of its own
  // type the record lays out, though GNU's packing shares its bytes.
  const uint64_t Base = At - Place.Byte;
  for (const Member *Bit : Place.Bits) {
    // C's unnamed bitfields pad; nothing reads them.
    if (Bit->Name.empty())
      continue;
    ViewAccessor Accessor;
    Accessor.TheKind = ViewAccessor::Kind::Bits;
    Accessor.Name = Into.unique(Names.member(Bit->Name));
    Accessor.Offset = Base + Bit->StorageBitOffset / 8;
    Accessor.Bits = bitAccess(Library, *Bit);
    Accessor.UnitBytes = Accessor.Bits.UnitBits / 8;
    Accessor.Declared = Into.where(Options, At) +
                        bitText(Accessor.Bits.Shift, Accessor.Bits.Width) +
                        " of " + C.declare(*Bit->Type) + ": " + Bit->Name;
    if (std::optional<ViewValue> Value = value(*Bit->Type))
      Accessor.Value = std::move(*Value);
    else
      Accessor.Value.TheForm = ViewValue::Form::Bytes;
    const unsigned Bytes = Accessor.UnitBytes;
    if (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8)
      Accessor.TheKind = ViewAccessor::Kind::Unread;
    Into.Class.Accessors.push_back(std::move(Accessor));
  }
}

void ViewPlanner::members(const RecordPlan &Plan, uint64_t Base,
                          State &Into) const {
  for (const Slot &Place : Plan.Slots) {
    const uint64_t At = Base + Place.Byte;
    switch (Place.TheKind) {
    case Slot::Kind::Filler:
      break;
    case Slot::Kind::Bitfields:
      bitfields(Place, At, Into);
      break;
    case Slot::Kind::Field:
      if (flattens(Place)) {
        if (const RecordPlan *Inner = nested(Plan, Place.NestedType))
          members(*Inner, At, Into);
        break;
      }
      field(Place, At, Into);
      break;
    }
  }
}

void ViewPlanner::classes(const RecordPlan &Plan, const std::string &Doc,
                          const std::string &Outer, unsigned Digits,
                          std::vector<ViewClass> &Classes,
                          std::vector<ViewEnum> &Enums) const {
  for (const auto &[Generated, Enum] : Plan.Enums)
    Enums.push_back({Generated, Enum, Generated});
  // The records it defines in place for a named member are classes of
  // their own; an anonymous one's members are its own, and what that
  // defines in place is its too.
  std::function<void(const RecordPlan &)> inner = [&](const RecordPlan &At) {
    for (const Slot &Place : At.Slots) {
      if (Place.TheKind != Slot::Kind::Field || Place.NestedType.empty())
        continue;
      const RecordPlan *Nested = nested(At, Place.NestedType);
      if (!Nested)
        continue;
      if (flattens(Place)) {
        for (const auto &[Generated, Enum] : Nested->Enums)
          Enums.push_back({Generated, Enum, Generated});
        inner(*Nested);
        continue;
      }
      classes(*Nested,
              Nested->Name + ": the " +
                  (Nested->Record->Union ? "union " : "structure ") +
                  Plan.Name + "." + Place.Name + " holds, " +
                  hex(Nested->Record->Size) + " bytes.",
              Outer, Digits, Classes, Enums);
    }
  };
  inner(Plan);
  ViewClass Class;
  Class.Name = Plan.Name;
  Class.Doc = Doc;
  Class.Record = Plan.Record;
  Class.Digits = Digits;
  State Into{Class, {}};
  for (const std::string &Name : Reserved)
    Into.Taken.insert(Name);
  members(Plan, 0, Into);
  Classes.push_back(std::move(Class));
}

void ViewPlanner::record(const LocalType &Type, std::vector<ViewClass> &Classes,
                         std::vector<ViewEnum> &Enums) const {
  if (!Type.Record || !Type.Record->Defined)
    return;
  const RecordDef &Record = *Type.Record;
  const std::string Name = Names.type(Type.Name);
  const RecordPlan Plan = planRecord(Library, Record, Name, Record.Pack, Names);
  const std::string Doc =
      Type.Name + ": " + (Record.Union ? "a union of " : "") +
      hex(Record.Size) + " (" + std::to_string(Record.Size) + ") bytes" +
      (Record.Pack ? ", packed to " + std::to_string(Record.Pack) : "") +
      (Record.Alignment ? ", aligned to " + std::to_string(Record.Alignment)
                        : "") +
      ".";
  classes(Plan, Doc, Name, offsetDigits(Record.Size), Classes, Enums);
}

} // namespace neverd::types::render
