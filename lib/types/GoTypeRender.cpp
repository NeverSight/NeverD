//===- GoTypeRender.cpp - Local types as Go -------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Go lays a structure out in the order of its fields, but has no unions,
// bitfields or packing, and aligns by its own rules.  So every field takes
// its offset through explicit `_ [n]byte` fillers, a member Go cannot align
// where the record has it is bytes read through an accessor, a union is
// storage of its size with an accessor for each alternative, a run of
// bitfields is its storage unit with a getter and a setter for each, and a
// flexible array, which would pad a Go structure, is an accessor.  Function
// pointers are uintptr, as in golang.org/x/sys/windows.  Builtins are exact
// widths (uint32), or Windows' names (ULONG), which a file declares as
// aliases of them.
//
//===----------------------------------------------------------------------===//

#include "TypeRenderShared.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <tuple>
#include <utility>

namespace neverd::types::render {
namespace {

std::string hex(uint64_t Value) {
  std::string Text;
  llvm::raw_string_ostream(Text) << llvm::format_hex(Value, 0, /*Upper=*/true);
  if (Text.size() > 1)
    Text[1] = 'x';
  return Text;
}

/// How Go writes one place of a structure.
struct Field {
  std::string Name;
  std::string Type;
  uint64_t Size = 0;
  /// How Go aligns the field as written.
  uint32_t Align = 1;
  /// Its type as the record has it, when Go holds it as bytes.
  std::string Bytes;
};

class GoWriter {
public:
  GoWriter(const TypeLibrary &Library, const RenderOptions &Options)
      : Library(Library), Options(Options), Spelling(spellingOf(Options)),
        Names(Library, Language::Go),
        Wide(pointerBytes(Library.dataModel()) == 8),
        Receiver(Library.find("s") ? "s_" : "s") {}

  /// \p Type's definition; with \p Paths, how code reaches each member a
  /// record names in C.
  void definition(const LocalType &Type, std::string &Out,
                  std::map<std::string, MemberPath> *Paths = nullptr);
  /// \p Type, a record, as an opaque type, as the text only points to it.
  void opaque(const LocalType &Type, std::string &Out) const;
  /// The builtins rendered so far by their Windows names.
  const std::set<Builtin> &aliased() const { return Aliased; }
  /// \p Type as an exact width, as Go spells it.
  std::string exact(Builtin Type) const;

private:
  std::string type(const TypeExpr &Type) const;
  std::string builtin(Builtin Type) const;
  std::string function(const FunctionDef &Function) const;
  uint32_t scalarAlign(uint64_t Bytes) const;
  /// How Go aligns \p Type; an anonymous record inside one packed to
  /// \p Pack takes that pack unless it has its own.
  uint32_t goAlign(const TypeExpr &Type, uint32_t Pack) const;
  /// How Go aligns the type it writes for \p Record packed to \p Pack.
  uint32_t recordAlign(const RecordDef &Record, uint32_t Pack) const;
  uint32_t unionAlign(const RecordDef &Record, uint32_t Pack) const;
  /// How a structure packed to \p Pack holds a field of \p Own alignment at
  /// \p Byte: aligned, or as bytes when Go cannot align it there.
  static bool asBytes(uint32_t Own, uint64_t Byte, uint32_t Pack) {
    return (Pack && Own > Pack) || Byte % Own;
  }
  Field field(const Slot &Place, uint32_t Pack) const;
  void record(const RecordPlan &Plan, bool Outer, std::string &Out);
  std::string slotType(const Slot &Place) const;
  void enumeration(const std::string &Name, const EnumDef &Enum,
                   std::string &Out) const;
  void structure(const RecordPlan &Plan, std::string &Out);
  void unionStorage(const RecordPlan &Plan, std::string &Out);
  /// The body of the getter and of the setter of a bit-field whose run's
  /// storage is of another width than its value, or bytes.
  std::pair<std::string, std::string>
  foreignBits(const BitAccess &Access, const std::string &Storage,
              const std::string &Value) const;
  void bitAccessors(const std::string &Type, const Slot &Place,
                    const std::string &Storage, llvm::StringSet<> &Taken,
                    std::string &Out) const;
  void asserts(const std::string &Name, uint64_t Size,
               const std::vector<std::pair<std::string, uint64_t>> &Offsets,
               std::string &Out) const;
  std::string comment(uint64_t Byte) const {
    return Options.Offsets ? offsetText(Byte, Options, Digits) : "";
  }
  static std::string unique(std::string Name, llvm::StringSet<> &Taken) {
    while (!Taken.insert(Name).second)
      Name += "_";
    return Name;
  }

  const TypeLibrary &Library;
  const RenderOptions &Options;
  const BuiltinSpelling Spelling;
  const NameSpelling Names;
  mutable std::set<Builtin> Aliased;
  const bool Wide;
  /// What methods call their receiver: s, unless a type is named so.
  const std::string Receiver;
  unsigned Digits = 1;
  /// The outer record's offset of the record being written, in bytes.
  uint64_t Base = 0;
  /// The outer record's name.
  std::string Parent;
  /// Records that code reaches \p Place's member through \p Hop, a field
  /// or a method that returns a pointer to it, or the anonymous record it
  /// holds.
  void reach(const Slot &Place, MemberPath::Hop Hop);
  /// Where definition() records the members' paths, the hops from the
  /// outer record to the one being written, and what holds each anonymous
  /// record written so far, by its type's name.
  std::map<std::string, MemberPath> *Paths = nullptr;
  std::vector<MemberPath::Hop> Prefix;
  std::map<std::string, MemberPath::Hop> Holders;
};

void GoWriter::reach(const Slot &Place, MemberPath::Hop Hop) {
  if (!Paths || !Place.Field)
    return;
  if (Place.Field->isAnonymous()) {
    Holders[Place.NestedType] = std::move(Hop);
    return;
  }
  MemberPath &Path = (*Paths)[Place.Field->Name];
  Path.Hops = Prefix;
  Path.Hops.push_back(std::move(Hop));
}

uint32_t GoWriter::scalarAlign(uint64_t Bytes) const {
  if (Bytes >= 8)
    return Wide ? 8 : 4;
  if (Bytes >= 4)
    return 4;
  return Bytes >= 2 ? 2 : 1;
}

std::string GoWriter::builtin(Builtin Type) const {
  if (Spelling == BuiltinSpelling::Windows && windowsTypedef(Type)) {
    Aliased.insert(Type);
    return builtinInfo(Type).Windows;
  }
  return exact(Type);
}

std::string GoWriter::exact(Builtin Type) const {
  const BuiltinInfo &Info = builtinInfo(Type);
  const unsigned Bytes = Library.builtinBytes(Type);
  switch (Info.Class) {
  case BuiltinClass::Void:
    return "struct{}";
  case BuiltinClass::Bool:
    return Bytes == 1 ? "bool" : "uint" + std::to_string(Bytes * 8);
  case BuiltinClass::Float:
    if (Type == Builtin::Half)
      return "uint16";
    if (Bytes == 4)
      return "float32";
    if (Bytes == 8)
      return "float64";
    return "[" + std::to_string(Bytes) + "]byte";
  case BuiltinClass::Complex:
    return "complex" + std::to_string(Bytes * 8);
  case BuiltinClass::Character:
  case BuiltinClass::Integer:
    if (Bytes > 8)
      return "[" + std::to_string(Bytes) + "]byte";
    return std::string(Info.Signed ? "int" : "uint") +
           std::to_string(Bytes * 8);
  }
  return "byte";
}

uint32_t GoWriter::unionAlign(const RecordDef &Record, uint32_t Pack) const {
  // Storage in units of the union's alignment aligns it as the union.
  uint32_t Align = Library.alignOf(Record);
  if (Pack)
    Align = std::min(Align, Pack);
  Align = std::min<uint32_t>(Align, Wide ? 8 : 4);
  return Align && Record.Size % Align == 0 ? Align : 1;
}

uint32_t GoWriter::recordAlign(const RecordDef &Record, uint32_t Pack) const {
  if (Record.Union)
    return unionAlign(Record, Pack);
  // As structure() writes it: the fields Go aligns where the record has
  // them; flexible arrays are accessors.
  uint32_t Align = 1;
  for (const Member &Field : Record.Members) {
    if (!Field.Type || !Library.sizeOf(*Field.Type).value_or(0))
      continue;
    uint32_t Own;
    uint64_t Byte;
    if (Field.isBitfield()) {
      Own = scalarAlign(bitStorage(Library, *Field.Type).Bytes);
      Byte = Field.StorageBitOffset / 8;
    } else {
      Own = goAlign(*Field.Type, Pack);
      Byte = Field.BitOffset / 8;
    }
    if (!asBytes(Own, Byte, Pack))
      Align = std::max(Align, Own);
  }
  return Align;
}

uint32_t GoWriter::goAlign(const TypeExpr &Type, uint32_t Pack) const {
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    if (exact(Type.TheBuiltin).front() == '[')
      return 1;
    return scalarAlign(Library.builtinBytes(Type.TheBuiltin));
  case TypeExprKind::Pointer:
    return scalarAlign(Type.PointerBytes);
  case TypeExprKind::Array:
    return Type.Element ? goAlign(*Type.Element, Pack) : 1;
  case TypeExprKind::Function:
    return 1;
  case TypeExprKind::Record:
    if (!Type.Record)
      return 1;
    return recordAlign(*Type.Record,
                       Type.Record->Pack ? Type.Record->Pack : Pack);
  case TypeExprKind::Enum:
    return Type.Enum ? scalarAlign(Library.builtinBytes(Type.Enum->Underlying))
                     : 4;
  case TypeExprKind::Named: {
    const LocalType *Named = Library.resolve(Type.Name);
    if (!Named)
      return 1;
    if (Named->Record)
      return Named->Record->Defined
                 ? recordAlign(*Named->Record, Named->Record->Pack)
                 : 1;
    if (Named->Enum)
      return scalarAlign(Library.builtinBytes(Named->Enum->Underlying));
    if (Named->Target)
      return goAlign(*Named->Target, 0);
    return 1;
  }
  }
  return 1;
}

std::string GoWriter::function(const FunctionDef &Function) const {
  // Go names every parameter or none: where C names some, the others are
  // blank.
  const bool Named = llvm::any_of(Function.Params, [](const Parameter &Param) {
    return !Param.Name.empty();
  });
  std::string Text = "func(";
  for (size_t I = 0; I < Function.Params.size(); ++I) {
    if (I)
      Text += ", ";
    const Parameter &Param = Function.Params[I];
    if (!Param.Name.empty())
      Text += Names.member(Param.Name) + " ";
    else if (Named)
      Text += "_ ";
    Text += Param.Type ? type(*Param.Type) : "int32";
  }
  if (Function.Variadic)
    Text += Function.Params.empty() ? "...uintptr"
            : Named                 ? ", _ ...uintptr"
                                    : ", ...uintptr";
  Text += ")";
  if (Function.Return && !(Function.Return->Kind == TypeExprKind::Builtin &&
                           Function.Return->TheBuiltin == Builtin::Void))
    Text += " " + type(*Function.Return);
  return Text;
}

std::string GoWriter::type(const TypeExpr &Type) const {
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    return builtin(Type.TheBuiltin);
  case TypeExprKind::Named:
    return Names.type(Type.Name);
  case TypeExprKind::Pointer: {
    if (Type.PointerBytes != pointerBytes(Library.dataModel()))
      return "uint" + std::to_string(Type.PointerBytes * 8);
    if (!Type.Element)
      return "unsafe.Pointer";
    const TypeExpr &Target = *Type.Element;
    if (Target.Kind == TypeExprKind::Function)
      return "uintptr";
    if (Target.Kind == TypeExprKind::Builtin &&
        Target.TheBuiltin == Builtin::Void)
      return "unsafe.Pointer";
    if (Target.Kind == TypeExprKind::Named) {
      const LocalType *Named = Library.resolve(Target.Name);
      if (Named && Named->Kind == TypeKind::Function)
        return "uintptr";
    }
    return "*" + type(Target);
  }
  case TypeExprKind::Array:
    return "[" + std::to_string(Type.Count.value_or(0)) + "]" +
           (Type.Element ? type(*Type.Element) : std::string("byte"));
  case TypeExprKind::Function:
    return Type.Function ? function(*Type.Function) : "func()";
  case TypeExprKind::Record:
    return "[" + std::to_string(Type.Record ? Type.Record->Size : uint64_t(0)) +
           "]byte";
  case TypeExprKind::Enum:
    return Type.Enum ? builtin(Type.Enum->Underlying) : "int32";
  }
  return "byte";
}

std::string GoWriter::slotType(const Slot &Place) const {
  if (Place.NestedType.empty())
    return type(*Place.Field->Type);
  std::string Type;
  for (uint64_t Bound : Place.Bounds)
    Type += "[" + std::to_string(Bound) + "]";
  return Type + Place.NestedType;
}

Field GoWriter::field(const Slot &Place, uint32_t Pack) const {
  Field Result;
  Result.Name = Place.Name;
  uint32_t Own;
  if (Place.TheKind == Slot::Kind::Bitfields && Place.StorageArray) {
    Result.Type = "[" + std::to_string(Place.StorageBytes) + "]byte";
    Result.Size = Place.StorageBytes;
    Own = 1;
  } else if (Place.TheKind == Slot::Kind::Bitfields) {
    const unsigned Bytes = unsigned(Place.StorageBytes);
    Result.Type = "uint" + std::to_string(Bytes * 8);
    Result.Size = Bytes;
    Own = scalarAlign(Bytes);
  } else {
    Result.Type = slotType(Place);
    Result.Size = Library.sizeOf(*Place.Field->Type).value_or(0);
    Own = goAlign(*Place.Field->Type, Pack);
  }
  if (asBytes(Own, Place.Byte, Pack)) {
    Result.Bytes = Result.Type;
    Result.Type = "[" + std::to_string(Result.Size) + "]byte";
    Result.Name += "_";
    Own = 1;
  }
  Result.Align = Own;
  return Result;
}

void GoWriter::bitAccessors(const std::string &Type, const Slot &Place,
                            const std::string &Storage,
                            llvm::StringSet<> &Taken, std::string &Out) const {
  for (const Member *Bit : Place.Bits) {
    const BitAccess Access = bitAccess(Library, *Bit, Place);
    const std::string Bits = std::to_string(Access.UnitBits);
    std::string Value =
        (Access.Signed ? "int" : "uint") + std::to_string(Access.ValueBits);
    // A builtin of the unit's size keeps its own name: ULONG, not UINT.
    if (Bit->Type && Bit->Type->Kind == TypeExprKind::Builtin &&
        Library.builtinBytes(Bit->Type->TheBuiltin) * 8 == Access.ValueBits &&
        builtinInfo(Bit->Type->TheBuiltin).Signed == Access.Signed &&
        builtinInfo(Bit->Type->TheBuiltin).Class != BuiltinClass::Float &&
        builtinInfo(Bit->Type->TheBuiltin).Class != BuiltinClass::Bool)
      Value = builtin(Bit->Type->TheBuiltin);
    std::string Read, Write;
    const bool Foreign =
        Place.StorageArray || Access.UnitBits != Access.ValueBits;
    if (Foreign) {
      // Storage of another width than the value, as GNU's packing makes it.
      std::tie(Read, Write) = foreignBits(Access, Storage, Value);
    } else if (Access.Signed) {
      // Up to the top, then back down: C extends a signed field's sign.
      const uint64_t Up = Access.UnitBits - Access.Shift - Access.Width;
      const uint64_t Down = Access.UnitBits - Access.Width;
      Read = Value + "(" + Storage +
             (Up ? "<<" + std::to_string(Up) : std::string()) + ")";
      if (Down)
        Read += " >> " + std::to_string(Down);
    } else {
      Read = Access.Shift
                 ? "(" + Storage + " >> " + std::to_string(Access.Shift) + ")"
                 : Storage;
      if (Access.Width < Access.UnitBits)
        Read += " & " + Access.Mask;
    }
    const std::string Shift =
        Access.Shift ? "<<" + std::to_string(Access.Shift) : "";
    const std::string Stored =
        Access.Signed ? "uint" + Bits + "(value)" : "value";
    const std::string Getter = unique(Names.member(Bit->Name), Taken);
    const std::string Setter = unique("Set" + Names.member(Bit->Name), Taken);
    if (Paths && !Bit->Name.empty()) {
      MemberPath &Path = (*Paths)[Bit->Name];
      Path.Hops = Prefix;
      Path.Getter = Getter;
      Path.Setter = Setter;
    }
    const std::string Where =
        bitText(Bit->BitOffset - Place.Byte * 8, Access.Width) + " of " +
        Place.Name + ".\n";
    if (Write.empty())
      Write = Storage + " = " + Storage + "&^(" + Access.Mask + Shift +
              ") | (" + Stored + "&" + Access.Mask + ")" + Shift;
    Out += "\n// " + Getter + " reads " + Where;
    Out += "func (" + Receiver + " *" + Type + ") " + Getter + "() " + Value +
           " {\n\t" + (Foreign ? "" : "return ") + Read + "\n}\n";
    Out += "\n// " + Setter + " writes " + Where;
    Out += "func (" + Receiver + " *" + Type + ") " + Setter + "(value " +
           Value + ") {\n\t" + Write + "\n}\n";
  }
}

std::pair<std::string, std::string>
GoWriter::foreignBits(const BitAccess &Access, const std::string &Storage,
                      const std::string &Value) const {
  const std::string Unit = std::to_string(Access.UnitBits);
  const std::string Shift = std::to_string(Access.Shift);
  const bool Whole = Access.Width >= Access.UnitBits;
  // The mask in a variable: shifted past 64 bits, a constant does not
  // compile.
  const std::string Mask = "mask := uint" + Unit + "(" + Access.Mask + ")\n\t";
  std::string Load, Store = Mask, Raw;
  if (Access.ArrayBytes) {
    // The bytes, low first, as the record holds them; a ninth holds the
    // top of a field of 64 bits that begins inside a byte.
    const auto byte = [&](unsigned K) {
      return Storage + "[" + std::to_string(Access.ArrayByte + K) + "]";
    };
    const unsigned Low = std::min(Access.ArrayBytes, 8u);
    std::string Bytes;
    for (unsigned K = 0; K < Low; ++K)
      Bytes += (K ? " | " : "") + std::string("uint64(") + byte(K) + ")" +
               (K ? "<<" + std::to_string(8 * K) : "");
    Load += "unit := " + Bytes + "\n\t";
    Raw = Access.Shift ? "unit>>" + Shift : "unit";
    Store += "unit := " + Bytes + "\n\tunit = unit&^(mask<<" + Shift +
             ") | (uint64(value)&mask)<<" + Shift;
    for (unsigned K = 0; K < Low; ++K)
      Store += "\n\t" + byte(K) + " = byte(unit" +
               (K ? ">>" + std::to_string(8 * K) : "") + ")";
    if (Access.ArrayBytes > 8) {
      const std::string Rest = std::to_string(64 - Access.Shift);
      Load += "top := uint64(" + byte(8) + ")\n\t";
      Raw = "(" + Raw + " | top<<" + Rest + ")";
      Store += "\n\t" + byte(8) + " = byte(uint64(" + byte(8) + ")&^(mask>>" +
               Rest + ") | (uint64(value)&mask)>>" + Rest + ")";
    }
  } else {
    Raw = Access.Shift ? Storage + ">>" + Shift : Storage;
    Store += Storage + " = " + Storage + "&^(mask<<" + Shift + ") | (uint" +
             Unit + "(value)&mask)<<" + Shift;
  }
  std::string Read;
  if (Access.Signed) {
    // Up to the top, then back down: C extends a signed field's sign.
    const std::string Spare = std::to_string(Access.UnitBits - Access.Width);
    Read = Value + "(int" + Unit + "((" + Raw + ")<<" + Spare + ") >> " +
           Spare + ")";
  } else if (Whole) {
    Read = Value + "(" + Raw + ")";
  } else {
    Load = Mask + Load;
    Read = Value + "((" + Raw + ")&mask)";
  }
  return {Load + "return " + Read, Store};
}

void GoWriter::asserts(
    const std::string &Name, uint64_t Size,
    const std::vector<std::pair<std::string, uint64_t>> &Offsets,
    std::string &Out) const {
  // An index out of range of a one-element array fails to compile.
  Out +=
      "\nvar _ = [1]int{}[unsafe.Sizeof(" + Name + "{})-" + hex(Size) + "]\n";
  for (const auto &[Field, Byte] : Offsets)
    // A blank field, as Go's own types hold, is no field Go reaches.
    if (Field != "_")
      Out += "var _ = [1]int{}[unsafe.Offsetof(" + Name + "{}." + Field + ")-" +
             hex(Byte) + "]\n";
}

void GoWriter::unionStorage(const RecordPlan &Plan, std::string &Out) {
  const RecordDef &Record = *Plan.Record;
  const uint32_t Align = unionAlign(Record, Plan.Pack);
  std::string Unit = "byte";
  uint64_t Count = Record.Size;
  if (Align > 1) {
    Unit = "uint" + std::to_string(Align * 8);
    Count = Record.Size / Align;
  }
  llvm::StringSet<> Taken;
  for (const Slot &Place : Plan.Slots)
    Taken.insert(Place.Name);
  const std::string Raw = unique("raw", Taken);
  std::string Methods;
  for (const Slot &Place : Plan.Slots) {
    if (Place.TheKind == Slot::Kind::Bitfields) {
      // Bits of the union's first bytes.
      const std::string Storage =
          "*(*uint" +
          std::to_string(bitStorage(Library, *Place.Storage).Bytes * 8) +
          ")(unsafe.Pointer(" + Receiver + "))";
      bitAccessors(Plan.Name, Place, Storage, Taken, Methods);
      continue;
    }
    if (Place.TheKind != Slot::Kind::Field)
      continue;
    const std::string Type = slotType(Place);
    Methods += "\nfunc (" + Receiver + " *" + Plan.Name + ") " + Place.Name +
               "() *" + Type + " {\n\treturn (*" + Type + ")(unsafe.Pointer(" +
               Receiver + "))\n}\n";
    reach(Place, {Place.Name, /*Accessor=*/true});
  }
  Out += "type " + Plan.Name + " struct {\n";
  Out += withComment("\t" + Raw + " [" + std::to_string(Count) + "]" + Unit,
                     comment(Base), Options) +
         "\n}\n" + Methods;
  if (Options.Asserts)
    asserts(Plan.Name, Record.Size, {}, Out);
}

void GoWriter::structure(const RecordPlan &Plan, std::string &Out) {
  const RecordDef &Record = *Plan.Record;
  // Methods may not take a field's name; a flexible array is no field.
  llvm::StringSet<> Taken;
  for (const Slot &Place : Plan.Slots)
    if (Place.TheKind != Slot::Kind::Field ||
        Library.sizeOf(*Place.Field->Type).value_or(0))
      Taken.insert(Place.Name);
  std::string Fields, Methods;
  std::vector<std::pair<std::string, uint64_t>> Offsets;
  // Where Go puts the next field, which fillers move to the record's offset.
  uint64_t End = 0;
  uint32_t Align = 1;
  const auto fill = [&](uint64_t To) {
    Fields += withComment("\t_ [" + std::to_string(To - End) + "]byte",
                          comment(Base + End) + " " + std::to_string(To - End) +
                              " bytes moved",
                          Options) +
              "\n";
  };
  const auto padding = [&](uint64_t Bytes) {
    if (Options.Offsets)
      Fields += "\t// " + comment(Base + End) + ": " + std::to_string(Bytes) +
                " bytes of padding\n";
  };
  for (const Slot &Place : Plan.Slots) {
    if (Place.TheKind == Slot::Kind::Filler || Place.Byte < End)
      continue;
    const Field Written = field(Place, Plan.Pack);
    if (!Written.Size) {
      // A flexible array would pad the structure; its elements follow it.
      if (Place.TheKind != Slot::Kind::Field || !Place.Field->Type->Element)
        continue;
      std::string Element = Written.Type.substr(Written.Type.find(']') + 1);
      const std::string Name = unique(Place.Name, Taken);
      Methods += "\n// " + Name + " returns the n " + Element +
                 " that follow at " + comment(Base + Place.Byte) + ".\n";
      Methods += "func (" + Receiver + " *" + Plan.Name + ") " + Name +
                 "(n int) []" + Element + " {\n\treturn unsafe.Slice((*" +
                 Element + ")(unsafe.Add(unsafe.Pointer(" + Receiver + "), " +
                 std::to_string(Place.Byte) + ")), n)\n}\n";
      continue;
    }
    // Go pads up to the field's alignment by itself; past that, a filler.
    if (Place.Byte > alignTo(End, Written.Align))
      fill(Place.Byte);
    else if (Place.Byte > End)
      padding(Place.Byte - End);
    std::string Note;
    if (Place.TheKind == Slot::Kind::Bitfields)
      for (const Member *Bit : Place.Bits)
        Note += (Note.empty() ? " " : ", ") + Bit->Name + " " +
                bitText(Bit->BitOffset - Place.Byte * 8, Bit->BitWidth);
    if (!Written.Bytes.empty())
      Note += " " + Written.Bytes + ", unaligned";
    Fields += withComment("\t" + Written.Name + " " + Written.Type,
                          comment(Base + Place.Byte) + Note, Options) +
              "\n";
    End = Place.Byte + Written.Size;
    Align = std::max(Align, Written.Align);
    Offsets.push_back({Written.Name, Place.Byte});
    if (Place.TheKind == Slot::Kind::Field)
      reach(Place, Written.Bytes.empty()
                       ? MemberPath::Hop{Written.Name, /*Accessor=*/false}
                       : MemberPath::Hop{Place.Name, /*Accessor=*/true});
    if (!Written.Bytes.empty()) {
      Methods += "\n// " + Place.Name + " reads the " + Written.Bytes +
                 " Go cannot align at " + comment(Base + Place.Byte) + ".\n";
      Methods += "func (" + Receiver + " *" + Plan.Name + ") " + Place.Name +
                 "() *" + Written.Bytes + " {\n\treturn (*" + Written.Bytes +
                 ")(unsafe.Pointer(&" + Receiver + "." + Written.Name +
                 "))\n}\n";
    }
    if (Place.TheKind == Slot::Kind::Bitfields)
      bitAccessors(Plan.Name, Place,
                   Written.Bytes.empty()
                       ? Receiver + "." + Written.Name
                       : "*" + Receiver + "." + Place.Name + "()",
                   Taken, Methods);
  }
  // Go rounds a structure up to its own alignment, which may fall short of
  // the record's size.
  if (Record.Size > alignTo(End, Align))
    fill(Record.Size);
  else if (Record.Size > End)
    padding(Record.Size - End);
  Out += "type " + Plan.Name + " struct {\n" + Fields + "}\n" + Methods;
  if (Options.Asserts)
    asserts(Plan.Name, Record.Size, Offsets, Out);
}

void GoWriter::record(const RecordPlan &Plan, bool Outer, std::string &Out) {
  const RecordDef &Record = *Plan.Record;
  // Go cannot pack or align a type; its comment says, for readers and to
  // read it back.
  const std::string Layout =
      (Plan.Pack ? ", packed to " + std::to_string(Plan.Pack) : "") +
      (Record.Alignment ? ", aligned to " + std::to_string(Record.Alignment)
                        : "");
  if (Outer)
    Out += "// " + Plan.Name + ": " + hex(Record.Size) + " (" +
           std::to_string(Record.Size) + ") bytes" + Layout +
           (Record.Union ? "; a union, read through its accessors" : "") +
           ".\n";
  else
    Out += "// " + Plan.Name + ": the anonymous " +
           (Record.Union ? "union" : "structure") + " at " + Parent + "+" +
           comment(Base) + ", " + hex(Record.Size) + " bytes" + Layout +
           (Record.Union ? "; read it through its accessors" : "") + ".\n";
  if (Record.Union)
    unionStorage(Plan, Out);
  else
    structure(Plan, Out);
  for (const auto &[Name, Enum] : Plan.Enums) {
    Out += '\n';
    enumeration(Name, *Enum, Out);
  }
  const uint64_t Saved = Base;
  for (const RecordPlan &Nested : Plan.Nested) {
    uint64_t At = 0;
    for (const Slot &Place : Plan.Slots)
      if (Place.NestedType == Nested.Name)
        At = Place.Byte;
    Base = Saved + At;
    Out += '\n';
    // Its members are reached through what holds it.
    std::map<std::string, MemberPath> *Outer = Paths;
    const auto Holder = Holders.find(Nested.Name);
    const bool Held = Paths && Holder != Holders.end();
    if (Held)
      Prefix.push_back(Holder->second);
    else
      Paths = nullptr;
    record(Nested, /*Outer=*/false, Out);
    if (Held)
      Prefix.pop_back();
    Paths = Outer;
  }
  Base = Saved;
}

void GoWriter::enumeration(const std::string &Name, const EnumDef &Enum,
                           std::string &Out) const {
  const std::string Underlying = builtin(Enum.Underlying);
  Out += "// " + Name + ": an enumeration of " + Underlying + ".\n";
  Out += "type " + Name + " " + Underlying + "\n";
  if (Enum.Values.empty())
    return;
  const bool Signed = builtinInfo(Enum.Underlying).Signed;
  const unsigned Bytes = Library.builtinBytes(Enum.Underlying);
  Out += "\nconst (\n";
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
    Out +=
        "\t" + Names.enumerator(Enum, I) + " " + Name + " = " + Number + "\n";
  }
  Out += ")\n";
}

void GoWriter::opaque(const LocalType &Type, std::string &Out) const {
  Out += "// " + Type.Name + ": only pointed to here.\n";
  Out += "type " + Names.type(Type.Name) + " struct{}\n";
}

void GoWriter::definition(const LocalType &Type, std::string &Out,
                          std::map<std::string, MemberPath> *Members) {
  Paths = Members;
  Prefix.clear();
  Holders.clear();
  const std::string Name = Names.type(Type.Name);
  switch (Type.Kind) {
  case TypeKind::Struct:
  case TypeKind::Union: {
    if (!Type.Record)
      return;
    if (!Type.Record->Defined) {
      Out += "// " + Name + ": declared only; its members are unknown.\n";
      Out += "type " + Name + " struct{}\n";
      return;
    }
    Digits = offsetDigits(Type.Record->Size);
    Base = 0;
    Parent = Name;
    record(planRecord(Library, *Type.Record, Name, Type.Record->Pack, Names),
           /*Outer=*/true, Out);
    return;
  }
  case TypeKind::Enum:
    if (Type.Enum)
      enumeration(Name, *Type.Enum, Out);
    return;
  case TypeKind::Typedef:
    if (Type.Target)
      Out += "type " + Name + " = " + type(*Type.Target) + "\n";
    return;
  case TypeKind::Function:
    if (Type.Function)
      Out += "type " + Name + " " + function(*Type.Function) + "\n";
    return;
  }
}

} // namespace

std::string renderGo(const TypeLibrary &Library, const LocalType &Type,
                     const RenderOptions &Options) {
  GoWriter Writer(Library, Options);
  std::string Out;
  Writer.definition(Type, Out);
  return Out;
}

EmbeddedSource renderGoEmbedded(const TypeLibrary &Library,
                                llvm::ArrayRef<const LocalType *> Defined,
                                llvm::ArrayRef<const LocalType *> Declared,
                                const RenderOptions &Options) {
  GoWriter Writer(Library, Options);
  EmbeddedSource Result;
  std::string Body;
  for (const LocalType *Type : Declared) {
    Body += '\n';
    Writer.opaque(*Type, Body);
  }
  for (const LocalType *Type : Defined) {
    Body += '\n';
    std::map<std::string, MemberPath> Paths;
    Writer.definition(*Type, Body, &Paths);
    if (Type->Kind == TypeKind::Struct || Type->Kind == TypeKind::Union)
      Result.Members[Type->Name] = std::move(Paths);
  }
  Result.Text = "// " + std::to_string(Defined.size()) +
                " types, from the program's local types.\n";
  if (!Writer.aliased().empty()) {
    Result.Text += "\n// The Windows headers' names of builtin types.\n";
    for (Builtin Type : Writer.aliased())
      Result.Text += "type " + std::string(builtinInfo(Type).Windows) + " = " +
                     Writer.exact(Type) + "\n";
  }
  Result.Text += Body;
  return Result;
}

std::string renderGoFile(const TypeLibrary &Library,
                         llvm::ArrayRef<const LocalType *> Roots,
                         const RenderOptions &Options) {
  const std::vector<const LocalType *> Types =
      neededTypes(Library, Roots, /*ThroughPointers=*/true);
  GoWriter Writer(Library, Options);
  std::string Body;
  for (const LocalType *Type : Types) {
    Body += '\n';
    Writer.definition(*Type, Body);
  }
  std::string Out = "// " + std::to_string(Types.size()) +
                    " types, rendered by NeverD.\n\npackage types\n\n"
                    "import \"unsafe\"\n\nvar _ unsafe.Pointer\n";
  // The Windows names the types use, as the headers declare them, at the
  // program's widths.
  if (!Writer.aliased().empty()) {
    Out += "\n// The Windows headers' names of builtin types.\n";
    for (Builtin Type : Writer.aliased())
      Out += "type " + std::string(builtinInfo(Type).Windows) + " = " +
             Writer.exact(Type) + "\n";
  }
  return Out + Body;
}

} // namespace neverd::types::render
