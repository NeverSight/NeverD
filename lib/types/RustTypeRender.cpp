//===- RustTypeRender.cpp - Local types as Rust ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Rust as windows-rs declares Windows types: #[repr(C)] structures and
// unions, anonymous records as types of their own named <Parent>_0 and
// fields named Anonymous, a run of bitfields as one _bitfield storage field
// with an accessor for each, enumerations as an integer type and constants,
// and nullable function pointers as Option<unsafe extern fn>.  Builtins are
// exact widths (u32), or Windows' names (ULONG), which a module declares as
// aliases of them, as wdk-sys does.
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

class RustWriter {
public:
  RustWriter(const TypeLibrary &Library, const RenderOptions &Options)
      : Library(Library), Options(Options), Spelling(spellingOf(Options)),
        Names(Library, Language::Rust) {}

  /// \p Type's definition; with \p Paths, how code reaches each member a
  /// record names in C.
  void definition(const LocalType &Type, std::string &Out,
                  std::map<std::string, MemberPath> *Paths = nullptr);
  /// \p Type, a record, as an opaque type, as the text only points to it.
  void opaque(const LocalType &Type, std::string &Out) const;
  /// The builtins rendered so far by their Windows names.
  const std::set<Builtin> &aliased() const { return Aliased; }
  /// The definition of the type long double takes where it is no float
  /// Rust has, which the types use: bytes aligned as C aligns them.
  std::string longDouble() const;
  /// \p Type as an exact width, as Rust spells it.
  std::string exact(Builtin Type, bool Pointee) const;

private:
  std::string type(const TypeExpr &Type, bool Pointee = false) const;
  std::string integer(uint64_t Bytes, bool Signed) const;
  std::string builtin(Builtin Type, bool Pointee) const;
  std::string function(const FunctionDef &Function) const;
  std::string abi(CallingConvention Convention) const;
  void record(const RecordPlan &Plan, bool Outer, std::string &Out);
  std::string slotType(const Slot &Place) const;
  void enumeration(const std::string &Name, const EnumDef &Enum,
                   std::string &Out) const;
  void accessors(const RecordPlan &Plan, std::string &Out) const;
  /// The body of the getter and of the setter of a bit-field whose run's
  /// storage is of another width than its value, or bytes.
  std::pair<std::string, std::string>
  foreignBits(const BitAccess &Access, const std::string &Field,
              const std::string &Value) const;
  void asserts(const RecordPlan &Plan, std::string &Out) const;
  std::string comment(uint64_t Byte) const {
    return Options.Offsets ? offsetText(Byte, Options, Digits) : "";
  }

  /// Records that code reaches \p Place's member through the field \p Hop,
  /// or the anonymous record it holds.
  void reach(const Slot &Place, MemberPath::Hop Hop);

  const TypeLibrary &Library;
  const RenderOptions &Options;
  const BuiltinSpelling Spelling;
  const NameSpelling Names;
  mutable std::set<Builtin> Aliased;
  mutable bool LongDouble = false;
  unsigned Digits = 1;
  /// The outer record's offset of the record being written, in bytes.
  uint64_t Base = 0;
  /// The outer record's name.
  std::string Parent;
  /// Where definition() records the members' paths, the hops from the
  /// outer record to the one being written, and the field that holds each
  /// anonymous record written so far, by its type's name.
  std::map<std::string, MemberPath> *Paths = nullptr;
  std::vector<MemberPath::Hop> Prefix;
  std::map<std::string, MemberPath::Hop> Holders;
};

void RustWriter::reach(const Slot &Place, MemberPath::Hop Hop) {
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

std::string RustWriter::integer(uint64_t Bytes, bool Signed) const {
  if (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8 && Bytes != 16)
    return "[u8; " + std::to_string(Bytes) + "]";
  return std::string(Signed ? "i" : "u") + std::to_string(Bytes * 8);
}

std::string RustWriter::builtin(Builtin Type, bool Pointee) const {
  if (Spelling == BuiltinSpelling::Windows && windowsTypedef(Type)) {
    Aliased.insert(Type);
    return builtinInfo(Type).Windows;
  }
  return exact(Type, Pointee);
}

std::string RustWriter::longDouble() const {
  if (!LongDouble)
    return {};
  const std::string Bytes =
      std::to_string(builtinBytes(Builtin::LongDouble, Library.dataModel()));
  return "\n/// C's long double: the x87's extended precision in " + Bytes +
         " bytes, aligned as C aligns it.\n#[repr(C, align(" +
         std::to_string(Library.alignOf(*makeBuiltin(Builtin::LongDouble))) +
         "))]\n#[derive(Clone, Copy)]\npub struct c_longdouble(pub [u8; " +
         Bytes + "]);\n";
}

std::string RustWriter::exact(Builtin Type, bool Pointee) const {
  const BuiltinInfo &Info = builtinInfo(Type);
  const unsigned Bytes = builtinBytes(Type, Library.dataModel());
  switch (Info.Class) {
  case BuiltinClass::Void:
    return Pointee ? "c_void" : "()";
  case BuiltinClass::Bool:
    return "bool";
  case BuiltinClass::Float:
    if (Type == Builtin::Half)
      return "u16";
    if (Bytes == 4)
      return "f32";
    if (Bytes == 8)
      return "f64";
    if (Type == Builtin::LongDouble) {
      LongDouble = true;
      return "c_longdouble";
    }
    return "[u8; " + std::to_string(Bytes) + "]";
  case BuiltinClass::Character:
    return integer(Bytes, Type == Builtin::Char);
  case BuiltinClass::Integer:
    return integer(Bytes, Info.Signed);
  }
  return "u8";
}

std::string RustWriter::abi(CallingConvention Convention) const {
  // Windows code calls through "system": stdcall on x86, C on x64.
  if (Convention == CallingConvention::Unspecified &&
      Library.dataModel() == DataModel::LLP64)
    return "system";
  switch (Convention) {
#define NEVERD_CALLING_CONVENTION(Name, C, Rust, IDA, Wide, Pops)              \
  case CallingConvention::Name:                                                \
    return Rust;
#include "neverd/types/CallingConventions.def"
  }
  return "C";
}

std::string RustWriter::function(const FunctionDef &Function) const {
  std::string Text = "unsafe extern \"" + abi(Function.Convention) + "\" fn(";
  for (size_t I = 0; I < Function.Params.size(); ++I) {
    if (I)
      Text += ", ";
    const Parameter &Param = Function.Params[I];
    if (!Param.Name.empty())
      Text += Names.member(Param.Name) + ": ";
    Text += Param.Type ? type(*Param.Type) : "i32";
  }
  if (Function.Variadic)
    Text += Function.Params.empty() ? "..." : ", ...";
  Text += ")";
  if (Function.Return && !(Function.Return->Kind == TypeExprKind::Builtin &&
                           Function.Return->TheBuiltin == Builtin::Void))
    Text += " -> " + type(*Function.Return);
  return Text;
}

std::string RustWriter::type(const TypeExpr &Type, bool Pointee) const {
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    return builtin(Type.TheBuiltin, Pointee);
  case TypeExprKind::Named:
    return Names.type(Type.Name);
  case TypeExprKind::Pointer: {
    if (!Type.Element)
      return "*mut c_void";
    const TypeExpr &Target = *Type.Element;
    // A function pointer may be null: Option of the fn type.
    if (Target.Kind == TypeExprKind::Function && Target.Function)
      return "Option<" + function(*Target.Function) + ">";
    if (Target.Kind == TypeExprKind::Named) {
      const LocalType *Named = Library.resolve(Target.Name);
      if (Named && Named->Kind == TypeKind::Function)
        return "Option<" + Names.type(Target.Name) + ">";
    }
    return std::string(Target.Const ? "*const " : "*mut ") +
           type(Target, /*Pointee=*/true);
  }
  case TypeExprKind::Array:
    return "[" + (Type.Element ? type(*Type.Element) : std::string("u8")) +
           "; " + std::to_string(Type.Count.value_or(0)) + "]";
  case TypeExprKind::Function:
    return Type.Function ? function(*Type.Function)
                         : "unsafe extern \"C\" fn()";
  case TypeExprKind::Record:
    return "[u8; " +
           std::to_string(Type.Record ? Type.Record->Size : uint64_t(0)) + "]";
  case TypeExprKind::Enum:
    return Type.Enum ? builtin(Type.Enum->Underlying, false) : "i32";
  }
  return "u8";
}

void RustWriter::record(const RecordPlan &Plan, bool Outer, std::string &Out) {
  const RecordDef &Record = *Plan.Record;
  // Rust cannot both pack a type and align it further; the packing stays,
  // and the size check holds the tail.
  std::string Repr = "#[repr(C)]";
  if (Plan.Pack)
    Repr = "#[repr(C, packed(" + std::to_string(Plan.Pack) + "))]";
  else if (Record.Alignment || Plan.Align)
    Repr = "#[repr(C, align(" +
           std::to_string(std::max(Record.Alignment, Plan.Align)) + "))]";
  if (Outer)
    Out += "/// " + Plan.Name + ": " + hex(Record.Size) + " (" +
           std::to_string(Record.Size) + ") bytes" +
           (Plan.Pack ? ", packed to " + std::to_string(Plan.Pack) : "") +
           ".\n";
  else
    Out += "/// " + Plan.Name + ": the anonymous " +
           (Record.Union ? "union" : "structure") + " at " + Parent + "+" +
           comment(Base) + ", " + hex(Record.Size) + " bytes.\n";
  Out += Repr + "\n#[derive(Clone, Copy)]\n";
  Out += std::string(Record.Union ? "pub union " : "pub struct ") + Plan.Name +
         " {\n";
  uint64_t UnionSize = 0;
  for (const Slot &Place : Plan.Slots) {
    if (Place.PaddingBefore && Options.Offsets)
      Out += "    // " + comment(Base + Place.Byte - Place.PaddingBefore) +
             ": " + std::to_string(Place.PaddingBefore) + " bytes of padding\n";
    switch (Place.TheKind) {
    case Slot::Kind::Filler:
      Out += withComment("    pub " + Place.Name + ": [u8; " +
                             std::to_string(Place.Size) + "],",
                         comment(Base + Place.Byte), Options) +
             "\n";
      break;
    case Slot::Kind::Bitfields: {
      std::string Doc;
      for (const Member *Bit : Place.Bits)
        Doc += (Doc.empty() ? "" : ", ") + Bit->Name + " " +
               bitText(Bit->BitOffset - Place.Byte * 8, Bit->BitWidth);
      Out += "    /// " + Doc + "\n";
      const std::string Storage =
          Place.StorageArray
              ? "[u8; " + std::to_string(Place.StorageBytes) + "]"
              : "u" + std::to_string(Place.StorageBytes * 8);
      Out += withComment("    pub " + Place.Name + ": " + Storage + ",",
                         comment(Base + Place.Byte), Options) +
             "\n";
      break;
    }
    case Slot::Kind::Field: {
      const std::string Type = slotType(Place);
      Out += withComment("    pub " + Place.Name + ": " + Type + ",",
                         comment(Base + Place.Byte), Options) +
             "\n";
      reach(Place, {Place.Name, /*Accessor=*/false});
      UnionSize =
          std::max(UnionSize, Library.sizeOf(*Place.Field->Type).value_or(0));
      break;
    }
    }
  }
  if (Record.Union && Record.Size > UnionSize)
    Out += "    pub _size: [u8; " + std::to_string(Record.Size) + "],\n";
  if (Plan.TailFiller)
    Out += withComment("    pub _filler_end: [u8; " +
                           std::to_string(Plan.TailFiller) + "],",
                       comment(Base + Record.Size - Plan.TailFiller), Options) +
           "\n";
  else if (Plan.TailPadding && Options.Offsets)
    Out += "    // " + comment(Base + Record.Size - Plan.TailPadding) + ": " +
           std::to_string(Plan.TailPadding) + " bytes of padding\n";
  Out += "}\n";
  accessors(Plan, Out);
  if (Options.Asserts)
    asserts(Plan, Out);
  // Anonymous types, after their parent, at their own offsets.
  for (const auto &[Name, Enum] : Plan.Enums) {
    Out += '\n';
    enumeration(Name, *Enum, Out);
  }
  const uint64_t Saved = Base;
  for (size_t I = 0; I < Plan.Nested.size(); ++I) {
    uint64_t At = 0;
    for (const Slot &Place : Plan.Slots)
      if (Place.NestedType == Plan.Nested[I].Name)
        At = Place.Byte;
    Base = Saved + At;
    Out += '\n';
    // Its members are reached through the field that holds it.
    std::map<std::string, MemberPath> *Outer = Paths;
    const auto Holder = Holders.find(Plan.Nested[I].Name);
    const bool Held = Paths && Holder != Holders.end();
    if (Held)
      Prefix.push_back(Holder->second);
    else
      Paths = nullptr;
    record(Plan.Nested[I], /*Outer=*/false, Out);
    if (Held)
      Prefix.pop_back();
    Paths = Outer;
  }
  Base = Saved;
}

std::pair<std::string, std::string>
RustWriter::foreignBits(const BitAccess &Access, const std::string &Field,
                        const std::string &Value) const {
  const std::string Unit = std::to_string(Access.UnitBits);
  const std::string Shift = std::to_string(Access.Shift);
  const bool Whole = Access.Width >= Access.UnitBits;
  std::string Load, Store, Raw;
  if (Access.ArrayBytes) {
    // The bytes, low first, as the record holds them; a ninth holds the
    // top of a field of 64 bits that begins inside a byte.
    const auto byte = [&](unsigned K) {
      return Field + "[" + std::to_string(Access.ArrayByte + K) + "]";
    };
    const unsigned Low = std::min(Access.ArrayBytes, 8u);
    std::string Bytes;
    for (unsigned K = 0; K < Low; ++K)
      Bytes += K ? " | (" + byte(K) + " as u64) << " + std::to_string(8 * K)
                 : byte(K) + " as u64";
    Load = "let unit = " + Bytes + ";\n        ";
    Raw = Access.Shift ? "(unit >> " + Shift + ")" : "unit";
    Store = "let mut unit = " + Bytes + ";\n        unit = (unit & !(" +
            Access.Mask + " << " + Shift + ")) | (((value as u64) & " +
            Access.Mask + ") << " + Shift + ");";
    for (unsigned K = 0; K < Low; ++K)
      Store += "\n        " + byte(K) + " = " +
               (K ? "(unit >> " + std::to_string(8 * K) + ") as u8;"
                  : "unit as u8;");
    if (Access.ArrayBytes > 8) {
      const std::string Rest = std::to_string(64 - Access.Shift);
      Load += "let top = " + byte(8) + " as u64;\n        ";
      Raw = "(" + Raw + " | top << " + Rest + ")";
      Store += "\n        let top = (" + byte(8) + " as u64 & !(" +
               Access.Mask + " >> " + Rest + ")) | (((value as u64) & " +
               Access.Mask + ") >> " + Rest + ");\n        " + byte(8) +
               " = top as u8;";
    }
  } else {
    Raw = Access.Shift ? "(" + Field + " >> " + Shift + ")" : Field;
    Store = Field + " = (" + Field + " & !(" + Access.Mask + " << " + Shift +
            ")) | (((value as u" + Unit + ") & " + Access.Mask + ") << " +
            Shift + ");";
  }
  std::string Read;
  if (Access.Signed) {
    // Up to the top, then back down: C extends a signed field's sign.
    const std::string Spare = std::to_string(Access.UnitBits - Access.Width);
    Read = "(((" + Raw + " << " + Spare + ") as i" + Unit + ") >> " + Spare +
           ") as " + Value;
  } else {
    Read =
        (Whole ? Raw : "(" + Raw + " & " + Access.Mask + ")") + " as " + Value;
  }
  return {Load + Read, Store};
}

void RustWriter::accessors(const RecordPlan &Plan, std::string &Out) const {
  std::string Methods;
  llvm::StringSet<> Taken;
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
      const std::string Bits = std::to_string(Access.ValueBits);
      const std::string Unsigned = "u" + std::to_string(Access.UnitBits);
      std::string Value = (Access.Signed ? "i" : "u") + Bits;
      // A builtin of the unit's size keeps its own name: ULONG, not UINT.
      if (Bit->Type->Kind == TypeExprKind::Builtin &&
          builtinBytes(Bit->Type->TheBuiltin, Library.dataModel()) * 8 ==
              Access.ValueBits &&
          builtinInfo(Bit->Type->TheBuiltin).Signed == Access.Signed &&
          builtinInfo(Bit->Type->TheBuiltin).Class != BuiltinClass::Float &&
          builtinInfo(Bit->Type->TheBuiltin).Class != BuiltinClass::Bool)
        Value = builtin(Bit->Type->TheBuiltin, /*Pointee=*/false);
      const std::string Field = "self." + Place.Name;
      std::string Read;
      std::string Write;
      if (Place.StorageArray || Access.UnitBits != Access.ValueBits) {
        // Storage of another width than the value, as GNU's packing makes
        // it: the bits in the storage's unsigned integer, the bytes that
        // hold them read as one, then the value of the field's own type.
        std::tie(Read, Write) = foreignBits(Access, Field, Value);
      } else if (Access.Signed) {
        // Up to the top, then back down: C extends a signed field's sign.
        const uint64_t Up = Access.UnitBits - Access.Shift - Access.Width;
        const uint64_t Down = Access.UnitBits - Access.Width;
        Read = Field;
        if (Up)
          Read = "(" + Read + " << " + std::to_string(Up) + ")";
        Read += " as " + Value;
        if (Down)
          Read = "(" + Read + ") >> " + std::to_string(Down);
      } else {
        Read = Access.Shift
                   ? "(" + Field + " >> " + std::to_string(Access.Shift) + ")"
                   : Field;
        if (Access.Width < Access.UnitBits)
          Read += " & " + Access.Mask;
      }
      const std::string Shift =
          Access.Shift ? " << " + std::to_string(Access.Shift) : "";
      const std::string Stored =
          Access.Signed ? "(value as " + Unsigned + ")" : "value";
      Methods += "    /// " + Bit->Name + ", " +
                 bitText(Bit->BitOffset - Place.Byte * 8, Access.Width) +
                 " of " + Place.Name + ".\n";
      if (Write.empty())
        Write = Field + " = (" + Field + " & !(" + Access.Mask + Shift +
                ")) | ((" + Stored + " & " + Access.Mask + ")" + Shift + ");";
      // A union's field reads only in unsafe code.
      if (Plan.Record->Union) {
        Read = "unsafe { " + Read + " }";
        Write = "unsafe { " + Write + " }";
      }
      const std::string Getter = unique(Names.member(Bit->Name));
      const std::string Setter = unique("set_" + Names.member(Bit->Name));
      Methods += "    pub const fn " + Getter + "(&self) -> " + Value +
                 " {\n        " + Read + "\n    }\n";
      Methods += "    pub fn " + Setter + "(&mut self, value: " + Value +
                 ") {\n        " + Write + "\n    }\n";
      if (Paths && !Bit->Name.empty()) {
        MemberPath &Path = (*Paths)[Bit->Name];
        Path.Hops = Prefix;
        Path.Getter = Getter;
        Path.Setter = Setter;
      }
    }
  }
  if (!Methods.empty())
    Out += "impl " + Plan.Name + " {\n" + Methods + "}\n";
}

void RustWriter::asserts(const RecordPlan &Plan, std::string &Out) const {
  Out += "const _: () = assert!(core::mem::size_of::<" + Plan.Name +
         ">() == " + hex(Plan.Record->Size) + ");\n";
  if (Plan.Record->Union)
    return;
  for (const Slot &Place : Plan.Slots)
    if (Place.TheKind != Slot::Kind::Filler)
      Out += "const _: () = assert!(core::mem::offset_of!(" + Plan.Name + ", " +
             Place.Name + ") == " + hex(Place.Byte) + ");\n";
}

std::string RustWriter::slotType(const Slot &Place) const {
  if (Place.NestedType.empty())
    return type(*Place.Field->Type);
  std::string Type = Place.NestedType;
  for (auto Bound = Place.Bounds.rbegin(); Bound != Place.Bounds.rend();
       ++Bound)
    Type = "[" + Type + "; " + std::to_string(*Bound) + "]";
  return Type;
}

void RustWriter::enumeration(const std::string &Name, const EnumDef &Enum,
                             std::string &Out) const {
  const std::string Underlying = builtin(Enum.Underlying, false);
  Out += "/// " + Name + ": an enumeration of " + Underlying + ".\n";
  Out += "pub type " + Name + " = " + Underlying + ";\n";
  const bool Signed = builtinInfo(Enum.Underlying).Signed;
  const unsigned Bytes = builtinBytes(Enum.Underlying, Library.dataModel());
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
    Out += "pub const " + Names.enumerator(Enum, I) + ": " + Name + " = " +
           Number + ";\n";
  }
}

void RustWriter::opaque(const LocalType &Type, std::string &Out) const {
  Out += "/// " + Type.Name + ": only pointed to here.\n";
  Out += "#[repr(C)]\npub struct " + Names.type(Type.Name) +
         " {\n    _opaque: [u8; 0],\n}\n";
}

void RustWriter::definition(const LocalType &Type, std::string &Out,
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
      Out += "/// " + Type.Name + ": declared only; its members are unknown.\n";
      Out +=
          "#[repr(C)]\npub struct " + Name + " {\n    _opaque: [u8; 0],\n}\n";
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
      Out += "pub type " + Name + " = " + type(*Type.Target) + ";\n";
    return;
  case TypeKind::Function:
    if (Type.Function)
      Out += "pub type " + Name + " = " + function(*Type.Function) + ";\n";
    return;
  }
}

} // namespace

std::string renderRust(const TypeLibrary &Library, const LocalType &Type,
                       const RenderOptions &Options) {
  RustWriter Writer(Library, Options);
  std::string Out;
  Writer.definition(Type, Out);
  return Out;
}

EmbeddedSource renderRustEmbedded(const TypeLibrary &Library,
                                  llvm::ArrayRef<const LocalType *> Defined,
                                  llvm::ArrayRef<const LocalType *> Declared,
                                  const RenderOptions &Options) {
  RustWriter Writer(Library, Options);
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
    Result.Text += "\n/// The Windows headers' names of builtin types.\n";
    for (Builtin Type : Writer.aliased())
      Result.Text += "pub type " + std::string(builtinInfo(Type).Windows) +
                     " = " + Writer.exact(Type, /*Pointee=*/false) + ";\n";
  }
  Result.Text += Writer.longDouble() + Body;
  return Result;
}

std::string renderRustModule(const TypeLibrary &Library,
                             llvm::ArrayRef<const LocalType *> Roots,
                             const RenderOptions &Options) {
  const std::vector<const LocalType *> Types =
      neededTypes(Library, Roots, /*ThroughPointers=*/true);
  RustWriter Writer(Library, Options);
  std::string Body;
  for (const LocalType *Type : Types) {
    Body += '\n';
    Writer.definition(*Type, Body);
  }
  std::string Out = "// " + std::to_string(Types.size()) +
                    " types, rendered by NeverD.\n"
                    "#![allow(non_camel_case_types, non_snake_case, "
                    "non_upper_case_globals, dead_code)]\n\n"
                    "use core::ffi::c_void;\n";
  // The Windows names the types use, as the headers declare them, at the
  // program's widths.
  if (!Writer.aliased().empty()) {
    Out += "\n/// The Windows headers' names of builtin types.\n";
    for (Builtin Type : Writer.aliased())
      Out += "pub type " + std::string(builtinInfo(Type).Windows) + " = " +
             Writer.exact(Type, /*Pointee=*/false) + ";\n";
  }
  return Out + Writer.longDouble() + Body;
}

} // namespace neverd::types::render
