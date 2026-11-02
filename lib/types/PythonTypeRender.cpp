//===- PythonTypeRender.cpp - Local types as Python -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Python as ctypes declares C's types: a ctypes.Structure or ctypes.Union for
// each record, laid out at the library's offsets by C's own rules, with a
// filler where those would place a member elsewhere.  Anonymous records are
// classes of their own named <Parent>_0, which _anonymous_ promotes as C
// does.  A run of bitfields is one storage field read and written through
// properties, so that it lays out the same on every Python and platform.
// Enumerations are enum.IntEnum, their fields the underlying integers, and
// pointers to functions are ctypes.c_void_p.  A module declares every record
// before it lays any out, so that a pointer may name one defined later, and
// lays each out after what it holds.
//
//===----------------------------------------------------------------------===//

#include "TypeRenderShared.h"

#include "neverd/types/Limits.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <set>
#include <tuple>

namespace neverd::types::render {
namespace {

std::string hex(uint64_t Value) {
  std::string Text;
  llvm::raw_string_ostream(Text) << llvm::format_hex(Value, 0, /*Upper=*/true);
  if (Text.size() > 1)
    Text[1] = 'x';
  return Text;
}

std::string integer(uint64_t Bytes, bool Signed) {
  if (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8)
    return "ctypes.c_uint8 * " + std::to_string(Bytes);
  return std::string("ctypes.c_") + (Signed ? "int" : "uint") +
         std::to_string(Bytes * 8);
}

class PythonWriter {
public:
  PythonWriter(const TypeLibrary &Library, const RenderOptions &Options)
      : Library(Library), Options(Options), Spelling(spellingOf(Options)),
        Names(Library, Language::Python),
        Wide(pointerBytes(Library.dataModel()) == 8),
        C(Library, cOptions(Options)) {}

  /// \p Type alone, for reading: a record's layout in its class.
  void alone(const LocalType &Type, std::string &Out);
  /// \p Types as a module that runs.
  void module(llvm::ArrayRef<const LocalType *> Types, std::string &Out);

private:
  static RenderOptions cOptions(const RenderOptions &Options) {
    RenderOptions C = Options;
    C.Lang = Language::C;
    C.Spelling = spellingOf(Options);
    return C;
  }

  std::string builtin(Builtin Type) const;
  std::string exact(Builtin Type) const;
  std::string type(const TypeExpr &Type) const;
  std::string slotType(const Slot &Place) const;
  /// What a comment says of \p Type beside its field: the C type it holds
  /// where the field holds less, such as a function's address.
  std::string note(const TypeExpr &Type) const;
  std::string comment(uint64_t Byte) const {
    return Options.Offsets ? offsetText(Byte, Options, Digits) : "";
  }
  std::string line(std::string Code, const std::string &Comment) const {
    return withComment(std::move(Code), Comment, Options, "#") + "\n";
  }

  void enumeration(const std::string &Name, const EnumDef &Enum,
                   const std::string &Doc, std::string &Out) const;
  void typedefinition(const LocalType &Type, std::string &Out) const;
  /// \p Plan's class: its docstring and bitfield properties, and when
  /// \p Inline its layout too.
  void statement(const RecordPlan &Plan, bool Outer, bool Inline,
                 std::string &Out);
  /// \p Plan's _pack_, _align_, _anonymous_ and _fields_, set on \p Owner,
  /// "" inside the class body.
  void layout(const RecordPlan &Plan, const std::string &Indent,
              const std::string &Owner, std::string &Out);
  void properties(const RecordPlan &Plan, std::string &Out) const;
  void asserts(const RecordPlan &Plan, std::string &Out) const;
  /// Each of \p Plan's and its anonymous records', innermost first.
  static void eachPlan(const RecordPlan &Plan,
                       const std::function<void(const RecordPlan &)> &Visit) {
    for (const RecordPlan &Nested : Plan.Nested)
      eachPlan(Nested, Visit);
    Visit(Plan);
  }
  /// The type a typedef names in the end, through other typedefs.
  const LocalType *resolved(llvm::StringRef Name) const {
    return Library.resolve(Name);
  }
  /// Names each run's storage within \p Plan, its own first: _bitfield,
  /// _bitfield1 and so on.  _anonymous_ promotes the fields of a record in
  /// place into its parent, and one _bitfield would hide another.
  void nameStorage(const RecordPlan &Plan) {
    unsigned Count = 0;
    std::function<void(const RecordPlan &)> visit =
        [&](const RecordPlan &Inner) {
          for (const Slot &Place : Inner.Slots)
            if (Place.TheKind == Slot::Kind::Bitfields)
              Storage[&Place] =
                  Count++ ? Place.Name + std::to_string(Count - 1) : Place.Name;
          for (const RecordPlan &Nested : Inner.Nested)
            visit(Nested);
        };
    visit(Plan);
  }
  /// \p Place's field name: a run's storage as nameStorage names it.
  std::string fieldName(const Slot &Place) const {
    const auto Found = Storage.find(&Place);
    return Found != Storage.end() ? Found->second : Place.Name;
  }

  const TypeLibrary &Library;
  const RenderOptions &Options;
  const BuiltinSpelling Spelling;
  const NameSpelling Names;
  const bool Wide;
  const CSpeller C;
  std::map<const Slot *, std::string> Storage;
  unsigned Digits = 1;
  /// The outer record's offset of the record being written, in bytes.
  uint64_t Base = 0;
  /// The outer record's name.
  std::string Parent;

public:
  mutable std::set<Builtin> Aliased;
  /// Whether the types use long double where it is no float ctypes has.
  mutable bool LongDouble = false;
  mutable bool Int128 = false;
  mutable bool UInt128 = false;
  /// Whether a field holds a pointer as the host's own, which needs a
  /// Python of the program's pointer width.
  mutable bool HostPointers = false;
  bool Enums = false;
};

std::string PythonWriter::builtin(Builtin Type) const {
  if (Spelling == BuiltinSpelling::Windows && windowsTypedef(Type)) {
    Aliased.insert(Type);
    return builtinInfo(Type).Windows;
  }
  return exact(Type);
}

std::string PythonWriter::exact(Builtin Type) const {
  const BuiltinInfo &Info = builtinInfo(Type);
  const unsigned Bytes = Library.builtinBytes(Type);
  switch (Info.Class) {
  case BuiltinClass::Void:
    return "None";
  case BuiltinClass::Bool:
    return Bytes == 1 ? "ctypes.c_bool" : integer(Bytes, false);
  case BuiltinClass::Float:
    if (Type == Builtin::Half)
      return "ctypes.c_uint16";
    if (Bytes == 4)
      return "ctypes.c_float";
    if (Bytes == 8)
      return "ctypes.c_double";
    if (Type == Builtin::LongDouble) {
      LongDouble = true;
      return "c_longdouble";
    }
    return integer(Bytes, false);
  case BuiltinClass::Character:
    // C's char is ctypes' own; the wide ones are their widths, which
    // ctypes.c_wchar is not on every platform.
    return Type == Builtin::Char ? "ctypes.c_char" : integer(Bytes, false);
  case BuiltinClass::Integer:
    // ctypes has no integer of 16 bytes; C aligns its own.
    if (Bytes == 16) {
      (Info.Signed ? Int128 : UInt128) = true;
      return Info.Signed ? "c_int128" : "c_uint128";
    }
    return integer(Bytes, Info.Signed);
  }
  return "ctypes.c_uint8";
}

std::string PythonWriter::type(const TypeExpr &Type) const {
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    return builtin(Type.TheBuiltin);
  case TypeExprKind::Named: {
    // An enumeration's fields hold its underlying integer; its IntEnum reads
    // the value.
    if (const LocalType *Named = resolved(Type.Name);
        Named && Named->Kind == TypeKind::Enum && Named->Enum)
      return builtin(Named->Enum->Underlying);
    return Names.type(Type.Name);
  }
  case TypeExprKind::Pointer: {
    // A 32-bit program's pointer is its address on a host of any width.
    if (!Wide || Type.PointerBytes != pointerBytes(Library.dataModel()))
      return "ctypes.c_uint" + std::to_string(Type.PointerBytes * 8);
    HostPointers = true;
    if (!Type.Element)
      return "ctypes.c_void_p";
    const TypeExpr &Target = *Type.Element;
    if (Target.Kind == TypeExprKind::Builtin &&
        Target.TheBuiltin == Builtin::Void)
      return "ctypes.c_void_p";
    if (Target.Kind == TypeExprKind::Function)
      return "ctypes.c_void_p";
    if (Target.Kind == TypeExprKind::Named) {
      const LocalType *Named = resolved(Target.Name);
      if (Named && Named->Kind == TypeKind::Function)
        return "ctypes.c_void_p";
      if (Named && Named->Kind == TypeKind::Typedef && Named->Target &&
          Named->Target->Kind == TypeExprKind::Builtin &&
          Named->Target->TheBuiltin == Builtin::Void)
        return "ctypes.c_void_p";
    }
    return "ctypes.POINTER(" + type(Target) + ")";
  }
  case TypeExprKind::Array:
    return (Type.Element ? type(*Type.Element)
                         : std::string("ctypes.c_uint8")) +
           " * " + std::to_string(Type.Count.value_or(0));
  case TypeExprKind::Function:
    return "ctypes.c_void_p";
  case TypeExprKind::Record:
    return "ctypes.c_uint8 * " +
           std::to_string(Type.Record ? Type.Record->Size : uint64_t(0));
  case TypeExprKind::Enum:
    return Type.Enum ? builtin(Type.Enum->Underlying) : "ctypes.c_int32";
  }
  return "ctypes.c_uint8";
}

std::string PythonWriter::note(const TypeExpr &Type) const {
  const TypeExpr *Element = &Type;
  while (Element->Kind == TypeExprKind::Array && Element->Element)
    Element = Element->Element.get();
  // A function's address, a 32-bit program's pointer and an enumeration's
  // integer hold less than the C type says.
  bool Says = Element->Kind == TypeExprKind::Enum;
  if (Element->Kind == TypeExprKind::Named)
    if (const LocalType *Named = resolved(Element->Name))
      Says = Named->Kind == TypeKind::Enum;
  if (Element->Kind == TypeExprKind::Pointer) {
    Says = !Wide || Element->PointerBytes != pointerBytes(Library.dataModel());
    if (const TypeExpr *Target = Element->Element.get()) {
      Says |= Target->Kind == TypeExprKind::Function;
      if (Target->Kind == TypeExprKind::Named)
        if (const LocalType *Named = resolved(Target->Name))
          Says |= Named->Kind == TypeKind::Function;
    }
  }
  return Says ? C.declare(Type) : std::string();
}

std::string PythonWriter::slotType(const Slot &Place) const {
  std::vector<uint64_t> Bounds;
  // An enumeration in place is its integer, which its IntEnum reads.
  if (Place.NestedType.empty() ||
      arrayElement(*Place.Field->Type, Bounds).Kind == TypeExprKind::Enum)
    return type(*Place.Field->Type);
  // C's int[2][3] is ctypes' c_int * 3 * 2: the innermost bound first.
  std::string Type = Place.NestedType;
  for (auto Bound = Place.Bounds.rbegin(); Bound != Place.Bounds.rend();
       ++Bound)
    Type += " * " + std::to_string(*Bound);
  return Type;
}

void PythonWriter::enumeration(const std::string &Name, const EnumDef &Enum,
                               const std::string &Doc, std::string &Out) const {
  const unsigned Bytes = Library.builtinBytes(Enum.Underlying);
  const bool Signed = builtinInfo(Enum.Underlying).Signed;
  Out += "class " + Name + "(enum.IntEnum):\n";
  Out += "    \"\"\"" + Doc + ": an enumeration of " + std::to_string(Bytes) +
         " bytes, which fields hold as " + exact(Enum.Underlying) + ".\"\"\"\n";
  for (size_t I = 0; I < Enum.Values.size(); ++I) {
    const Enumerator &Value = Enum.Values[I];
    std::string Number;
    if (Signed && static_cast<int64_t>(Value.Value) < 0)
      Number = std::to_string(static_cast<int64_t>(Value.Value));
    else if (Value.Value >= 0x100)
      Number = hex(Bytes < 8 ? Value.Value & ((uint64_t(1) << (Bytes * 8)) - 1)
                             : Value.Value);
    else
      Number = std::to_string(Value.Value);
    Out += "    " + Names.enumerator(Enum, I) + " = " + Number + "\n";
  }
}

void PythonWriter::typedefinition(const LocalType &Type,
                                  std::string &Out) const {
  const std::string Name = Names.type(Type.Name);
  if (Type.Kind == TypeKind::Function) {
    // No value is a function: a pointer to it is ctypes.c_void_p.
    if (Type.Function)
      Out += "# " + Name + " is the function type " +
             C.declare(*makeFunction(Type.Function)) +
             "; a pointer to it is ctypes.c_void_p.\n";
    return;
  }
  if (!Type.Target)
    return;
  // A typedef of a function type is one too, which no value is.
  const TypeExpr *Target = Type.Target.get();
  for (unsigned Hop = 0;
       Target && Hop < ::neverd::limits::types::
                           kPythonTypeRenderPythonWriterTypedefinitionHopLimit;
       ++Hop) {
    if (Target->Kind == TypeExprKind::Function && Target->Function) {
      Out += "# " + Name + " is the function type " +
             C.declare(*makeFunction(Target->Function)) +
             "; a pointer to it is ctypes.c_void_p.\n";
      return;
    }
    const LocalType *Named = Target->Kind == TypeExprKind::Named
                                 ? Library.find(Target->Name)
                                 : nullptr;
    if (!Named)
      break;
    if (Named->Kind == TypeKind::Function && Named->Function) {
      Out += "# " + Name + " is the function type " +
             C.declare(*makeFunction(Named->Function)) +
             "; a pointer to it is ctypes.c_void_p.\n";
      return;
    }
    Target = Named->Kind == TypeKind::Typedef ? Named->Target.get() : nullptr;
  }
  const std::string Says = note(*Type.Target);
  Out += Says.empty() ? Name + " = " + type(*Type.Target) + "\n"
                      : line(Name + " = " + type(*Type.Target), Says);
}

void PythonWriter::statement(const RecordPlan &Plan, bool Outer, bool Inline,
                             std::string &Out) {
  const RecordDef &Record = *Plan.Record;
  Out += "class " + Plan.Name + "(ctypes." +
         (Record.Union ? "Union" : "Structure") + "):\n";
  if (Outer)
    Out += "    \"\"\"" + Plan.Name + ": " + hex(Record.Size) + " (" +
           std::to_string(Record.Size) + ") bytes" +
           (Plan.Pack ? ", packed to " + std::to_string(Plan.Pack) : "") +
           ".\"\"\"\n";
  else
    Out += "    \"\"\"" + Plan.Name + ": the anonymous " +
           (Record.Union ? "union" : "structure") + " at " + Parent + "+" +
           comment(Base) + ", " + hex(Record.Size) + " bytes.\"\"\"\n";
  if (Inline) {
    Out += "\n";
    layout(Plan, "    ", "", Out);
  }
  properties(Plan, Out);
}

void PythonWriter::layout(const RecordPlan &Plan, const std::string &Indent,
                          const std::string &Owner, std::string &Out) {
  const RecordDef &Record = *Plan.Record;
  // ctypes reads these when _fields_ is set: they go first.  A pack lays
  // out as MSVC does, as GCC packs too; Python 3.14 asks for it by name.
  if (Plan.Pack) {
    Out += Indent + Owner + "_pack_ = " + std::to_string(Plan.Pack) + "\n";
    Out += Indent + Owner + "_layout_ = \"ms\"\n";
  }
  if (Record.Alignment || Plan.Align)
    Out += Indent + Owner + "_align_ = " +
           std::to_string(std::max(Record.Alignment, Plan.Align)) + "\n";
  std::vector<std::string> Anonymous;
  for (const Slot &Place : Plan.Slots)
    if (Place.TheKind == Slot::Kind::Field && Place.Field->isAnonymous() &&
        Place.Bounds.empty())
      Anonymous.push_back(Place.Name);
  if (!Anonymous.empty()) {
    // A tuple: one name keeps its comma.
    std::string Listed;
    for (const std::string &Name : Anonymous)
      Listed += "\"" + Name + "\", ";
    if (Anonymous.size() > 1)
      Listed.erase(Listed.size() - 2);
    else
      Listed.pop_back();
    Out += Indent + Owner + "_anonymous_ = (" + Listed + ")\n";
  }
  Out += Indent + Owner + "_fields_ = [\n";
  const std::string Item = Indent + "    ";
  uint64_t UnionSize = 0;
  for (const Slot &Place : Plan.Slots) {
    if (Place.PaddingBefore && Options.Offsets)
      Out += Item + "# " + comment(Base + Place.Byte - Place.PaddingBefore) +
             ": " + std::to_string(Place.PaddingBefore) + " bytes of padding\n";
    switch (Place.TheKind) {
    case Slot::Kind::Filler:
      Out += line(Item + "(\"" + Place.Name + "\", ctypes.c_uint8 * " +
                      std::to_string(Place.Size) + "),",
                  comment(Base + Place.Byte));
      break;
    case Slot::Kind::Bitfields: {
      std::string Doc;
      for (const Member *Bit : Place.Bits)
        Doc += (Doc.empty() ? "" : ", ") + Bit->Name + " " +
               bitText(Bit->BitOffset - Place.Byte * 8, Bit->BitWidth);
      const std::string Storage =
          Place.StorageArray
              ? "ctypes.c_uint8 * " + std::to_string(Place.StorageBytes)
              : integer(unsigned(Place.StorageBytes), false);
      Out += line(Item + "(\"" + fieldName(Place) + "\", " + Storage + "),",
                  comment(Base + Place.Byte) + " " + Doc);
      break;
    }
    case Slot::Kind::Field: {
      std::string Comment = comment(Base + Place.Byte);
      std::vector<uint64_t> Bounds;
      if (!Place.NestedType.empty() &&
          arrayElement(*Place.Field->Type, Bounds).Kind == TypeExprKind::Enum)
        Comment += (Comment.empty() ? "" : " ") + Place.NestedType;
      else if (Place.NestedType.empty())
        if (const std::string Says = note(*Place.Field->Type); !Says.empty())
          Comment += (Comment.empty() ? "" : " ") + Says;
      Out += line(Item + "(\"" + Place.Name + "\", " + slotType(Place) + "),",
                  Comment);
      UnionSize =
          std::max(UnionSize, Library.sizeOf(*Place.Field->Type).value_or(0));
      break;
    }
    }
  }
  // A union larger than its alternatives keeps its size with one more.
  if (Record.Union && Record.Size > UnionSize)
    Out += line(Item + "(\"_size\", ctypes.c_uint8 * " +
                    std::to_string(Record.Size) + "),",
                comment(Base));
  if (Plan.TailFiller)
    Out += line(Item + "(\"_filler_end\", ctypes.c_uint8 * " +
                    std::to_string(Plan.TailFiller) + "),",
                comment(Base + Record.Size - Plan.TailFiller));
  else if (Plan.TailPadding && Options.Offsets)
    Out += Item + "# " + comment(Base + Record.Size - Plan.TailPadding) + ": " +
           std::to_string(Plan.TailPadding) + " bytes of padding\n";
  Out += Indent + "]\n";
}

void PythonWriter::properties(const RecordPlan &Plan, std::string &Out) const {
  llvm::StringSet<> Taken;
  for (const Slot &Place : Plan.Slots)
    Taken.insert(fieldName(Place));
  const auto unique = [&](std::string Name) {
    while (!Taken.insert(Name).second)
      Name += "_";
    return Name;
  };
  for (const Slot &Place : Plan.Slots) {
    if (Place.TheKind != Slot::Kind::Bitfields)
      continue;
    for (const Member *Bit : Place.Bits) {
      const BitAccess Access = bitAccess(Library, *Bit, Place);
      const std::string Name = unique(Names.member(Bit->Name));
      std::string Field = "self." + fieldName(Place);
      // A run stored as bytes: those that hold the field, read as one
      // integer, low first, and written back so.
      std::string Load, Store;
      if (Access.ArrayBytes) {
        const std::string Bytes =
            Field + "[" + std::to_string(Access.ArrayByte) + ":" +
            std::to_string(Access.ArrayByte + Access.ArrayBytes) + "]";
        Load =
            "        unit = int.from_bytes(bytes(" + Bytes + "), \"little\")\n";
        Store = "        " + Bytes + " = list(unit.to_bytes(" +
                std::to_string(Access.ArrayBytes) + ", \"little\"))\n";
        Field = "unit";
      }
      const std::string Shifted =
          Access.Shift
              ? "(" + Field + " >> " + std::to_string(Access.Shift) + ")"
              : Field;
      Out += "\n    @property\n";
      Out += "    def " + Name + "(self) -> int:\n";
      Out += "        \"\"\"" + Bit->Name + ", " +
             bitText(Bit->BitOffset - Place.Byte * 8, Access.Width) + " of " +
             fieldName(Place) + ".\"\"\"\n";
      Out += Load;
      if (Access.Signed) {
        // C extends a signed field's top bit.
        const uint64_t Top = uint64_t(1) << (Access.Width - 1);
        Out += "        bits = " + Shifted + " & " + Access.Mask + "\n";
        Out += "        return bits - " + hex(Top << 1) + " if bits & " +
               hex(Top) + " else bits\n";
      } else {
        Out += "        return " + Shifted +
               (Access.Width < Access.UnitBits || Access.ArrayBytes
                    ? " & " + Access.Mask
                    : "") +
               "\n";
      }
      const std::string Shift =
          Access.Shift ? " << " + std::to_string(Access.Shift) : "";
      Out += "\n    @" + Name + ".setter\n";
      Out += "    def " + Name + "(self, value: int) -> None:\n";
      Out += Load;
      Out += "        " + Field + " = " + Field + " & ~(" + Access.Mask +
             Shift + ") | (value & " + Access.Mask + ")" + Shift + "\n";
      Out += Store;
    }
  }
}

void PythonWriter::asserts(const RecordPlan &Plan, std::string &Out) const {
  Out += "assert ctypes.sizeof(" + Plan.Name +
         ") == " + hex(Plan.Record->Size) + ", \"" + Plan.Name + "\"\n";
  if (Plan.Record->Union)
    return;
  for (const Slot &Place : Plan.Slots)
    if (Place.TheKind != Slot::Kind::Filler)
      Out += "assert " + Plan.Name + "." + fieldName(Place) +
             ".offset == " + hex(Place.Byte) + ", \"" + Plan.Name + "." +
             fieldName(Place) + "\"\n";
}

void PythonWriter::alone(const LocalType &Type, std::string &Out) {
  const std::string Name = Names.type(Type.Name);
  switch (Type.Kind) {
  case TypeKind::Struct:
  case TypeKind::Union: {
    if (!Type.Record)
      return;
    if (!Type.Record->Defined) {
      Out += "class " + Name + "(ctypes." +
             (Type.Record->Union ? "Union" : "Structure") + "):\n";
      Out += "    \"\"\"" + Type.Name +
             ": declared only; its members are unknown.\"\"\"\n";
      return;
    }
    Digits = offsetDigits(Type.Record->Size);
    Parent = Name;
    const RecordPlan Plan =
        planRecord(Library, *Type.Record, Name, Type.Record->Pack, Names);
    nameStorage(Plan);
    // What a record holds in place is a class before it.
    for (const auto &[Generated, Enum] : Plan.Enums) {
      enumeration(Generated, *Enum, Generated, Out);
      Out += '\n';
    }
    std::function<void(const RecordPlan &, uint64_t)> nested =
        [&](const RecordPlan &Inner, uint64_t At) {
          for (const RecordPlan &Deeper : Inner.Nested) {
            uint64_t Where = 0;
            for (const Slot &Place : Inner.Slots)
              if (Place.NestedType == Deeper.Name)
                Where = Place.Byte;
            nested(Deeper, At + Where);
          }
          if (&Inner == &Plan)
            return;
          Base = At;
          statement(Inner, /*Outer=*/false, /*Inline=*/true, Out);
          Out += '\n';
        };
    nested(Plan, 0);
    Base = 0;
    statement(Plan, /*Outer=*/true, /*Inline=*/true, Out);
    return;
  }
  case TypeKind::Enum:
    if (Type.Enum)
      enumeration(Name, *Type.Enum, Type.Name, Out);
    return;
  case TypeKind::Typedef:
  case TypeKind::Function:
    typedefinition(Type, Out);
    return;
  }
}

void PythonWriter::module(llvm::ArrayRef<const LocalType *> Types,
                          std::string &Out) {
  // Each record's plan, at the offsets its anonymous records begin.
  struct Planned {
    const LocalType *Type;
    RecordPlan Plan;
    std::vector<std::pair<const RecordPlan *, uint64_t>> Places;
  };
  std::vector<Planned> Records;
  std::string EnumText, Classes, Checks;
  for (const LocalType *Type : Types) {
    if (Type->Kind == TypeKind::Enum && Type->Enum) {
      enumeration(Names.type(Type->Name), *Type->Enum, Type->Name, EnumText);
      EnumText += '\n';
      Enums = true;
    } else if ((Type->Kind == TypeKind::Struct ||
                Type->Kind == TypeKind::Union) &&
               Type->Record) {
      Planned Entry{Type, {}, {}};
      if (Type->Record->Defined)
        Entry.Plan = planRecord(Library, *Type->Record, Names.type(Type->Name),
                                Type->Record->Pack, Names);
      Records.push_back(std::move(Entry));
    }
  }
  // Records first, laid out later: a pointer may name one not yet laid out.
  for (Planned &Entry : Records) {
    const std::string Name = Names.type(Entry.Type->Name);
    if (!Entry.Type->Record->Defined) {
      Classes += "class " + Name + "(ctypes." +
                 (Entry.Type->Record->Union ? "Union" : "Structure") + "):\n";
      Classes += "    \"\"\"" + Entry.Type->Name +
                 ": declared only; its members are unknown.\"\"\"\n\n";
      continue;
    }
    nameStorage(Entry.Plan);
    for (const auto &[Generated, Enum] : Entry.Plan.Enums) {
      enumeration(Generated, *Enum, Generated, EnumText);
      EnumText += '\n';
      Enums = true;
    }
    Digits = offsetDigits(Entry.Type->Record->Size);
    Parent = Name;
    std::function<void(const RecordPlan &, uint64_t)> visit =
        [&](const RecordPlan &Inner, uint64_t At) {
          for (const RecordPlan &Deeper : Inner.Nested) {
            uint64_t Where = 0;
            for (const Slot &Place : Inner.Slots)
              if (Place.NestedType == Deeper.Name)
                Where = Place.Byte;
            visit(Deeper, At + Where);
          }
          Entry.Places.emplace_back(&Inner, At);
        };
    visit(Entry.Plan, 0);
    for (const auto &[Inner, At] : Entry.Places) {
      Base = At;
      statement(*Inner, Inner == &Entry.Plan, /*Inline=*/false, Classes);
      Classes += '\n';
    }
  }
  // Typedefs and layouts in one order.  A typedef is bound after the
  // typedefs it names; one of an array, after the layout of what the array
  // holds, since ctypes sizes an array type once, when first asked for it.
  // A layout follows those of what its record holds by value, through
  // typedefs too, and the typedefs it names.
  llvm::StringMap<const Planned *> ByName;
  for (const Planned &Entry : Records)
    ByName[Entry.Type->Name] = &Entry;
  llvm::StringSet<> Bound, Laid;
  std::string Body;
  const auto typedefsNamed = [&](const LocalType &Type) {
    std::vector<std::string> Named, Typedefs;
    mentionedTypes(Type, Named);
    for (const std::string &Name : Named)
      if (const LocalType *Found = Library.find(Name);
          Found && (Found->Kind == TypeKind::Typedef ||
                    Found->Kind == TypeKind::Function))
        Typedefs.push_back(Name);
    return Typedefs;
  };
  std::function<void(llvm::StringRef)> complete;
  std::function<void(llvm::StringRef)> bind = [&](llvm::StringRef Name) {
    const LocalType *Type = Library.find(Name);
    if (!Type || !Bound.insert(Name).second)
      return;
    if (Type->Kind == TypeKind::Typedef && Type->Target &&
        Type->Target->Kind == TypeExprKind::Array) {
      std::vector<std::string> Held;
      valueDependencies(*Type, Held);
      for (const std::string &Element : Held)
        complete(Element);
    }
    for (const std::string &Named : typedefsNamed(*Type))
      bind(Named);
    typedefinition(*Type, Body);
  };
  complete = [&](llvm::StringRef Name) {
    const LocalType *Type = Library.find(Name);
    if (!Type || Type->Kind == TypeKind::Enum)
      return;
    std::vector<std::string> Held;
    valueDependencies(*Type, Held);
    if (Type->Kind == TypeKind::Typedef || Type->Kind == TypeKind::Function) {
      bind(Name);
      for (const std::string &Value : Held)
        complete(Value);
      return;
    }
    if (!Laid.insert(Name).second)
      return;
    for (const std::string &Value : Held)
      complete(Value);
    for (const std::string &Named : typedefsNamed(*Type))
      bind(Named);
    const auto Found = ByName.find(Name);
    if (Found == ByName.end() || !Found->second->Type->Record->Defined)
      return;
    const Planned &Entry = *Found->second;
    Digits = offsetDigits(Entry.Type->Record->Size);
    if (!Body.empty() && !llvm::StringRef(Body).ends_with("\n\n"))
      Body += '\n';
    for (const auto &[Inner, At] : Entry.Places) {
      Base = At;
      layout(*Inner, "", Inner->Name + ".", Body);
      if (Options.Asserts)
        asserts(*Inner, Checks);
    }
    Body += '\n';
  };
  for (const LocalType *Type : Types)
    complete(Type->Name);

  Out += "import ctypes\n";
  if (Enums)
    Out += "import enum\n";
  if (!Aliased.empty()) {
    Out += "\n# The Windows headers' names of builtin types.\n";
    for (Builtin Type : Aliased)
      Out +=
          std::string(builtinInfo(Type).Windows) + " = " + exact(Type) + "\n";
  }
  // C's integers of 16 bytes, which ctypes has none of, likewise.
  for (const auto &[Used, Name, Type] :
       {std::tuple(Int128, "c_int128", Builtin::Int128),
        std::tuple(UInt128, "c_uint128", Builtin::UInt128)})
    if (Used)
      Out += std::string("\n\nclass ") + Name +
             "(ctypes.Structure):\n    \"\"\"C's " +
             (Type == Builtin::Int128 ? "__int128" : "unsigned __int128") +
             ", in 16 bytes aligned as C aligns them.\"\"\"\n    _align_ = " +
             std::to_string(Library.alignOf(*makeBuiltin(Type))) +
             "\n    _fields_ = [(\"bytes\", ctypes.c_uint8 * 16)]\n";
  // C's long double: bytes aligned as C aligns them, which ctypes'
  // c_longdouble is only on a host of the program's kind.
  if (LongDouble)
    Out += "\n\nclass c_longdouble(ctypes.Structure):\n    \"\"\"C's long "
           "double, in " +
           std::to_string(Library.builtinBytes(Builtin::LongDouble)) +
           " bytes aligned as C aligns them.\"\"\"\n    _align_ = " +
           std::to_string(Library.alignOf(*makeBuiltin(Builtin::LongDouble))) +
           "\n    _fields_ = [(\"bytes\", ctypes.c_uint8 * " +
           std::to_string(Library.builtinBytes(Builtin::LongDouble)) + ")]\n";
  if (HostPointers)
    Out += "\n# The program's pointers are 64-bit, as the host's must be.\n"
           "assert ctypes.sizeof(ctypes.c_void_p) == 8, \"these types need a "
           "64-bit Python\"\n";
  Out += "\n";
  Out += EnumText + Classes + Body;
  if (!Checks.empty())
    Out += Checks;
}

} // namespace

std::string renderPython(const TypeLibrary &Library, const LocalType &Type,
                         const RenderOptions &Options) {
  PythonWriter Writer(Library, Options);
  std::string Out;
  Writer.alone(Type, Out);
  return Out;
}

std::string renderPythonModule(const TypeLibrary &Library,
                               llvm::ArrayRef<const LocalType *> Roots,
                               const RenderOptions &Options) {
  const std::vector<const LocalType *> Types =
      neededTypes(Library, Roots, /*ThroughPointers=*/true);
  PythonWriter Writer(Library, Options);
  std::string Body;
  Writer.module(Types, Body);
  return "# " + std::to_string(Types.size()) + " types, rendered by NeverD.\n" +
         Body;
}

} // namespace neverd::types::render
