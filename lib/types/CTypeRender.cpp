//===- CTypeRender.cpp - Local types as C and C++ -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// C and C++ keep the library's own shape: anonymous unions and structures
// in place, bitfields as bitfields, and #pragma pack where the record was
// packed.  A member the language would place below its recorded offset gets
// an explicit filler before it; padding the compiler adds by itself is a
// comment.  C's header compiles as C++ too: its checks are static_assert,
// and the character types it declares are C++'s keywords there.
//
//===----------------------------------------------------------------------===//

#include "TypeRenderShared.h"

#include "neverd/types/TypeLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

namespace neverd::types::render {
namespace {

std::string hex(uint64_t Value) {
  std::string Text;
  llvm::raw_string_ostream(Text) << llvm::format_hex(Value, 0, /*Upper=*/true);
  // format_hex spells the prefix in the case of the digits.
  if (Text.size() > 1)
    Text[1] = 'x';
  return Text;
}

const char *conventionSpelling(CallingConvention Convention) {
  switch (Convention) {
#define NEVERD_CALLING_CONVENTION(Name, C, Rust, IDA, Wide)                    \
  case CallingConvention::Name:                                                \
    return C;
#include "neverd/types/CallingConventions.def"
  }
  return "";
}

/// A builtin as exact-width C spells it.
std::string portableBuiltin(Builtin Type, DataModel Model) {
  const BuiltinInfo &Info = builtinInfo(Type);
  const unsigned Bytes = builtinBytes(Type, Model);
  switch (Info.Class) {
  case BuiltinClass::Void:
    return "void";
  case BuiltinClass::Bool:
    return "bool";
  case BuiltinClass::Float:
    if (Type == Builtin::Half)
      return "_Float16";
    if (Bytes == 4)
      return "float";
    return Bytes == 8 ? "double" : "long double";
  case BuiltinClass::Character:
    if (Type == Builtin::Char)
      return "char";
    return "uint" + std::to_string(Bytes * 8) + "_t";
  case BuiltinClass::Integer:
    if (Bytes == 16)
      return Info.Signed ? "__int128" : "unsigned __int128";
    return std::string(Info.Signed ? "int" : "uint") +
           std::to_string(Bytes * 8) + "_t";
  }
  return "void";
}

class CWriter {
public:
  CWriter(const TypeLibrary &Library, const RenderOptions &Options)
      : Library(Library), Options(Options), Cpp(Options.Lang == Language::Cpp),
        Names(Library, Options.Lang) {}

  std::string builtin(Builtin Type) const;
  std::string declare(const TypeExpr &Type, std::string Declarator) const;

  void definition(const LocalType &Type, std::string &Out);
  void forwardDeclaration(const LocalType &Type, std::string &Out) const;
  /// The builtins rendered so far.
  const std::set<Builtin> &builtins() const { return UsedBuiltins; }
  /// Whether a record rendered so far declares its alignment.
  bool alignsRecords() const { return AlignsRecords; }
  /// Whether a type rendered so far is followed by checks.
  bool asserts() const { return Asserted; }
  const NameSpelling &names() const { return Names; }

private:
  void record(const LocalType &Type, std::string &Out);
  void enumeration(const LocalType &Type, std::string &Out);
  void asserts(const LocalType &Type, std::string &Out);
  void body(const RecordDef &Record, uint32_t Pack, unsigned Indent,
            uint64_t Base, bool Outer, std::string &Out);
  std::string qualifiers(const TypeExpr &Type) const;
  std::string enumHead(const EnumDef &Enum);
  void enumerators(const EnumDef &Enum, unsigned Indent, std::string &Out);
  void step(const SequentialLayout::Step &Step, uint64_t At, unsigned Indent,
            std::string &Out);
  void line(unsigned Indent, llvm::StringRef Code, llvm::StringRef Comment,
            std::string &Out) const;
  std::string tagged(const LocalType &Type) const;
  /// "struct" or "union", with the alignment \p Record declares.
  std::string keyword(const RecordDef &Record);
  std::string offset(uint64_t Byte) const {
    return offsetText(Byte, Options, Digits);
  }
  /// Every named member reachable as Type.Name, with its byte offset.
  void hoisted(const RecordDef &Record, uint64_t Base,
               std::vector<std::pair<std::string, uint64_t>> &Out) const;

  const TypeLibrary &Library;
  const RenderOptions &Options;
  const bool Cpp;
  const NameSpelling Names;
  unsigned Digits = 1;
  unsigned Fillers = 0;
  mutable std::set<Builtin> UsedBuiltins;
  bool AlignsRecords = false;
  bool Asserted = false;
};

std::string CWriter::builtin(Builtin Type) const {
  UsedBuiltins.insert(Type);
  const BuiltinInfo &Info = builtinInfo(Type);
  switch (spellingOf(Options)) {
  case BuiltinSpelling::Windows:
    return Info.Windows;
  case BuiltinSpelling::Keywords:
    return Info.Keywords;
  case BuiltinSpelling::Portable:
    return portableBuiltin(Type, Library.dataModel());
  }
  return Info.Keywords;
}

std::string CWriter::declare(const TypeExpr &Type,
                             std::string Declarator) const {
  const auto spaced = [&](std::string Base) {
    return Declarator.empty() ? Base : Base + " " + Declarator;
  };
  std::string Qualifiers;
  if (Type.Const)
    Qualifiers += "const ";
  if (Type.Volatile)
    Qualifiers += "volatile ";
  switch (Type.Kind) {
  case TypeExprKind::Builtin:
    return spaced(Qualifiers + builtin(Type.TheBuiltin));
  case TypeExprKind::Named: {
    std::string Base = Qualifiers;
    if (!Cpp) {
      // The tag the library's type takes now: a type of another kind may
      // have replaced the one the reference was read with, as a program's
      // own union does a header's structure of the name.
      const LocalType *Named = Library.find(Type.Name);
      const TypeKind Kind = Named ? Named->Kind : Type.NamedKind;
      if (Kind == TypeKind::Struct)
        Base += "struct ";
      else if (Kind == TypeKind::Union)
        Base += "union ";
      else if (Kind == TypeKind::Enum)
        Base += "enum ";
    }
    return spaced(Base + Names.type(Type.Name));
  }
  case TypeExprKind::Pointer: {
    std::string Inner = Type.Reference ? "&" : "*";
    if (Type.Const)
      Inner += "const";
    if (Type.Volatile)
      Inner += Type.Const ? " volatile" : "volatile";
    if (!Declarator.empty())
      Inner += (Type.Const || Type.Volatile ? " " : "") + Declarator;
    if (!Type.Element)
      return builtin(Builtin::Void) + " " + Inner;
    const TypeExpr &Pointee = *Type.Element;
    // A pointer to an array or a function binds before them.
    if (Pointee.Kind == TypeExprKind::Array ||
        Pointee.Kind == TypeExprKind::Function) {
      if (Pointee.Kind == TypeExprKind::Function && Pointee.Function) {
        const std::string Convention =
            conventionSpelling(Pointee.Function->Convention);
        if (!Convention.empty())
          Inner = Convention + " " + Inner;
      }
      Inner = "(" + Inner + ")";
    }
    return declare(Pointee, Inner);
  }
  case TypeExprKind::Array: {
    const std::string Bound = Type.Count ? std::to_string(*Type.Count) : "";
    if (!Type.Element)
      return spaced(builtin(Builtin::UChar)) + "[" + Bound + "]";
    return declare(*Type.Element, Declarator + "[" + Bound + "]");
  }
  case TypeExprKind::Function: {
    if (!Type.Function)
      return spaced(builtin(Builtin::Void));
    const FunctionDef &Function = *Type.Function;
    std::string Params;
    for (const Parameter &Param : Function.Params) {
      if (!Params.empty())
        Params += ", ";
      Params += Param.Type ? declare(*Param.Type, Names.member(Param.Name))
                           : builtin(Builtin::Int);
    }
    if (Function.Variadic)
      Params += Params.empty() ? "..." : ", ...";
    else if (Params.empty() && !Cpp)
      Params = "void";
    std::string Inner = Declarator;
    // A function named directly takes its convention before its name.
    if (!Inner.empty() && Inner.front() != '(') {
      const std::string Convention = conventionSpelling(Function.Convention);
      if (!Convention.empty())
        Inner = Convention + " " + Inner;
    }
    Inner += "(" + Params + ")";
    if (!Function.Return)
      return builtin(Builtin::Void) + " " + Inner;
    return declare(*Function.Return, Inner);
  }
  case TypeExprKind::Record:
    return spaced(Qualifiers +
                  (Type.Record && Type.Record->Union ? "union" : "struct") +
                  " /* anonymous */");
  case TypeExprKind::Enum:
    return spaced(Qualifiers + "enum /* anonymous */");
  }
  return spaced(builtin(Builtin::Void));
}

void CWriter::line(unsigned Indent, llvm::StringRef Code,
                   llvm::StringRef Comment, std::string &Out) const {
  std::string Text(Indent, ' ');
  Text += Code;
  if (Options.Offsets)
    Text = withComment(std::move(Text), Comment, Options);
  Out += Text;
  Out += '\n';
}

std::string CWriter::keyword(const RecordDef &Record) {
  std::string Text = Record.Union ? "union" : "struct";
  if (!Record.Alignment)
    return Text;
  // As the Windows headers declare it; C++ has a keyword.
  const std::string Bytes = std::to_string(Record.Alignment);
  if (Cpp)
    return Text + " alignas(" + Bytes + ")";
  AlignsRecords = true;
  return Text + " DECLSPEC_ALIGN(" + Bytes + ")";
}

std::string CWriter::tagged(const LocalType &Type) const {
  const std::string Name = Names.type(Type.Name);
  if (Cpp)
    return Name;
  switch (Type.Kind) {
  case TypeKind::Struct:
    return "struct " + Name;
  case TypeKind::Union:
    return "union " + Name;
  case TypeKind::Enum:
    return "enum " + Name;
  default:
    return Name;
  }
}

void CWriter::step(const SequentialLayout::Step &Step, uint64_t At,
                   unsigned Indent, std::string &Out) {
  if (Step.Filler) {
    const std::string Name = "__filler" + std::to_string(Fillers++);
    line(Indent,
         builtin(Builtin::UChar) + " " + Name + "[" +
             std::to_string(Step.Filler) + "];",
         offset(At) + ": " + std::to_string(Step.Filler) + " bytes moved", Out);
  } else if (Step.Padding && Options.Offsets) {
    std::string Text(Indent, ' ');
    Text += "// " + offset(At) + ": " + std::to_string(Step.Padding) +
            " bytes of padding";
    Out += Text;
    Out += '\n';
  }
}

std::string CWriter::qualifiers(const TypeExpr &Type) const {
  std::string Text;
  if (Type.Const)
    Text += "const ";
  if (Type.Volatile)
    Text += "volatile ";
  return Text;
}

std::string CWriter::enumHead(const EnumDef &Enum) {
  // C23 and C++ state an underlying type other than int, which C has
  // allowed as an extension before.
  if (Enum.Underlying == Builtin::Int)
    return "enum";
  return "enum : " + builtin(Enum.Underlying);
}

void CWriter::enumerators(const EnumDef &Enum, unsigned Indent,
                          std::string &Out) {
  const unsigned Bytes = builtinBytes(Enum.Underlying, Library.dataModel());
  const bool Signed = builtinInfo(Enum.Underlying).Signed;
  bool Hex = Enum.Flags;
  for (const Enumerator &Value : Enum.Values)
    if (!(Signed && static_cast<int64_t>(Value.Value) < 0) &&
        Value.Value >= 0x100)
      Hex = true;
  for (size_t I = 0; I < Enum.Values.size(); ++I) {
    const Enumerator &Value = Enum.Values[I];
    const int64_t AsSigned = static_cast<int64_t>(Value.Value);
    std::string Number;
    if (Signed && AsSigned < 0)
      Number = std::to_string(AsSigned);
    else if (Hex)
      Number = hex(Bytes < 8 ? Value.Value & ((uint64_t(1) << (Bytes * 8)) - 1)
                             : Value.Value);
    else
      Number = std::to_string(Value.Value);
    Out += std::string(Indent, ' ') + Names.enumerator(Enum, I) + " = " +
           Number + ",\n";
  }
}

void CWriter::body(const RecordDef &Record, uint32_t Pack, unsigned Indent,
                   uint64_t Base, bool Outer, std::string &Out) {
  SequentialLayout Layout(Pack);
  // The bitfield run being written: its storage unit, its size in bits and
  // the bit after its last field.
  struct Run {
    uint64_t Storage = 0;
    uint64_t Bits = 0;
    uint64_t Next = 0;
    std::string Type;
  };
  std::optional<Run> Bits;
  // GNU's compilers pack a bit-field at the bit after the member before it,
  // into the unit of its type that holds it, sharing bytes with members
  // before it: the source gives the bits between, rather than storage
  // units.  The bit after the last member, from the record.
  const bool Gnu = defaultRules(Library.dataModel()) == LayoutRules::Itanium;
  uint64_t EndBit = 0;
  uint64_t UnionSize = 0;
  uint32_t UnionAlign = 1;
  for (const Member &Field : Record.Members) {
    if (!Field.Type)
      continue;
    const uint64_t Bit = Base + Field.BitOffset;
    const uint64_t Size = Library.sizeOf(*Field.Type).value_or(0);
    const uint32_t Align = Library.alignOf(*Field.Type);
    const std::string Name = Names.member(Field.Name);
    if (Record.Union) {
      UnionSize = std::max(UnionSize, Size);
      UnionAlign = std::max(UnionAlign, Align);
    } else if (Field.isBitfield() && Gnu) {
      const std::string Type = declare(*Field.Type, "");
      const uint64_t Unit = Size * 8;
      // Where GNU puts the field: the bit after the last member, or the
      // next unit of its type where it would cross one, unless packed.
      uint64_t At = EndBit;
      if (Unit && !Pack && At / Unit != (At + Field.BitWidth - 1) / Unit)
        At = alignTo(At, Unit);
      if (Field.BitOffset > At) {
        // An unnamed field of width 0 moves to the next unit of its type;
        // one of a width takes the bits between.
        if (Unit && alignTo(EndBit, Unit) == Field.BitOffset) {
          line(Indent, Type + " : 0;", "", Out);
        } else {
          const uint64_t Gap = Field.BitOffset - At;
          line(Indent, Type + " : " + std::to_string(Gap) + ";",
               offset((Base + At) / 8) + " " + bitText(At % 8, Gap) +
                   " unnamed",
               Out);
        }
      }
      EndBit = Field.BitOffset + Field.BitWidth;
    } else if (Field.isBitfield()) {
      const uint64_t Storage = Field.StorageBitOffset;
      if (!Bits || Bits->Storage != Storage) {
        const std::string Type = declare(*Field.Type, "");
        // A run before this one with room left is closed by an unnamed
        // bitfield of width 0: MSVC would go on filling it with a field of
        // its size, and GCC with any field that fits.
        if (Bits && Bits->Next < Bits->Bits)
          line(Indent, Bits->Type + " : 0;", "", Out);
        if (Storage % 8 == 0) {
          const uint64_t At = Layout.end();
          step(Layout.place(Storage / 8, Size, Align), Base / 8 + At, Indent,
               Out);
        }
        Bits = Run{Storage, Size * 8, 0, Type};
      }
      // Bits the record leaves unnamed before this field.
      const uint64_t Position = Field.BitOffset - Storage;
      if (Position > Bits->Next)
        line(Indent,
             Bits->Type + " : " + std::to_string(Position - Bits->Next) + ";",
             offset((Base + Storage) / 8) + " " +
                 bitText(Bits->Next, Position - Bits->Next) + " unnamed",
             Out);
      Bits->Next = Position + Field.BitWidth;
    } else if (Field.BitOffset % 8 == 0) {
      Bits.reset();
      // The bytes GNU's bit-fields before the member took.
      if (Gnu)
        Layout.cover((EndBit + 7) / 8);
      const uint64_t At = Layout.end();
      step(Layout.place(Field.BitOffset / 8, Size, Align), Base / 8 + At,
           Indent, Out);
      EndBit = Field.BitOffset + Size * 8;
    }

    // An anonymous type is defined in place, an array of it too.
    const TypeExpr *Element = Field.Type.get();
    std::string Bounds;
    while (Element->Kind == TypeExprKind::Array && Element->Element) {
      Bounds +=
          "[" + (Element->Count ? std::to_string(*Element->Count) : "") + "]";
      Element = Element->Element.get();
    }
    if (Element->Kind == TypeExprKind::Record && Element->Record) {
      const RecordDef &Inner = *Element->Record;
      const uint32_t InnerPack = Inner.Pack ? Inner.Pack : Pack;
      // A pack of its own goes around it; the compiler takes the pack in
      // force where a record begins.
      if (InnerPack != Pack)
        Out += "#pragma pack(push, " + std::to_string(InnerPack) + ")\n";
      // An anonymous record shows its offset where it opens.
      line(Indent, qualifiers(*Element) + keyword(Inner),
           Field.Name.empty() ? offset(Bit / 8) : "", Out);
      line(Indent, "{", "", Out);
      body(Inner, InnerPack, Indent + 4, Bit, /*Outer=*/false, Out);
      if (Field.Name.empty())
        line(Indent, "};", "", Out);
      else
        line(Indent, "} " + Name + Bounds + ";", offset(Bit / 8), Out);
      if (InnerPack != Pack)
        Out += "#pragma pack(pop)\n";
      continue;
    }
    if (Element->Kind == TypeExprKind::Enum && Element->Enum) {
      line(Indent, qualifiers(*Element) + enumHead(*Element->Enum), "", Out);
      line(Indent, "{", "", Out);
      enumerators(*Element->Enum, Indent + 4, Out);
      line(Indent, "} " + Name + Bounds + ";", offset(Bit / 8), Out);
      continue;
    }
    std::string Code = declare(*Field.Type, Name);
    std::string Comment;
    if (Field.isBitfield()) {
      Code += " : " + std::to_string(Field.BitWidth);
      const uint64_t Storage = Base + Field.StorageBitOffset;
      Comment =
          offset(Storage / 8) + " " + bitText(Bit - Storage, Field.BitWidth);
    } else {
      Comment = offset(Bit / 8);
    }
    line(Indent, Code + ";", Comment, Out);
  }
  if (Record.Union) {
    // A union larger than its alternatives keeps its size with one more, as
    // a union of empty arrays, which only Microsoft's C sizes.  A union in
    // place names it apart: its members join the record's.
    if (Record.Size > alignTo(UnionSize, UnionAlign))
      line(Indent,
           builtin(Builtin::UChar) + " __size" +
               (Outer ? std::string() : std::to_string(Fillers++)) + "[" +
               std::to_string(Record.Size) + "];",
           offset(Base / 8), Out);
    return;
  }
  if (!Outer)
    return;
  if (Gnu)
    Layout.cover((EndBit + 7) / 8);
  const uint64_t At = Layout.end();
  step(Layout.finish(Record.Size, Library.alignOf(Record)), Base / 8 + At,
       Indent, Out);
}

void CWriter::record(const LocalType &Type, std::string &Out) {
  const RecordDef &Record = *Type.Record;
  const char *Keyword = Record.Union ? "union" : "struct";
  if (!Record.Defined) {
    Out += "// Declared only: its members are unknown.\n";
    Out += std::string(Keyword) + " " + Names.type(Type.Name) + ";\n";
    return;
  }
  Digits = offsetDigits(Record.Size);
  Fillers = 0;
  std::string Header = "// " + Type.Name + ": " + hex(Record.Size) + " (" +
                       std::to_string(Record.Size) + ") bytes";
  if (Record.Pack)
    Header += ", packed to " + std::to_string(Record.Pack);
  Out += Header + "\n";
  if (Record.Pack)
    Out += "#pragma pack(push, " + std::to_string(Record.Pack) + ")\n";
  Out += keyword(Record) + " " + Names.type(Type.Name) + "\n{\n";
  body(Record, Record.Pack, 4, 0, /*Outer=*/true, Out);
  Out += "};\n";
  if (Record.Pack)
    Out += "#pragma pack(pop)\n";
  if (Options.Asserts)
    asserts(Type, Out);
}

void CWriter::enumeration(const LocalType &Type, std::string &Out) {
  const EnumDef &Enum = *Type.Enum;
  Out += "// " + Type.Name + ": " +
         std::to_string(builtinBytes(Enum.Underlying, Library.dataModel())) +
         " bytes\n";
  std::string Head = enumHead(Enum);
  Head.insert(4, " " + Names.type(Type.Name));
  Out += Head + "\n{\n";
  enumerators(Enum, 4, Out);
  Out += "};\n";
  if (Options.Asserts)
    asserts(Type, Out);
}

void CWriter::hoisted(
    const RecordDef &Record, uint64_t Base,
    std::vector<std::pair<std::string, uint64_t>> &Out) const {
  for (const Member &Field : Record.Members) {
    if (Field.isAnonymous() && Field.Type->Record) {
      hoisted(*Field.Type->Record, Base + Field.BitOffset, Out);
      continue;
    }
    if (Field.isBitfield() || Field.Name.empty())
      continue;
    Out.push_back({Names.member(Field.Name), (Base + Field.BitOffset) / 8});
  }
}

void CWriter::asserts(const LocalType &Type, std::string &Out) {
  // C11's <assert.h> names _Static_assert so; the prelude does without it.
  const char *Assert = "static_assert";
  Asserted = true;
  const std::string Name = tagged(Type);
  if (Type.Kind == TypeKind::Enum) {
    Out += std::string(Assert) + "(sizeof(" + Name + ") == " +
           std::to_string(
               builtinBytes(Type.Enum->Underlying, Library.dataModel())) +
           ", \"" + Type.Name + "\");\n";
    return;
  }
  const RecordDef &Record = *Type.Record;
  Out += std::string(Assert) + "(sizeof(" + Name + ") == " + hex(Record.Size) +
         ", \"" + Type.Name + "\");\n";
  std::vector<std::pair<std::string, uint64_t>> Members;
  hoisted(Record, 0, Members);
  llvm::StringSet<> Seen;
  for (const auto &[Member, Byte] : Members)
    if (Seen.insert(Member).second)
      Out += std::string(Assert) + "(offsetof(" + Name + ", " + Member +
             ") == " + hex(Byte) + ", \"" + Type.Name + "." + Member + "\");\n";
}

void CWriter::definition(const LocalType &Type, std::string &Out) {
  switch (Type.Kind) {
  case TypeKind::Struct:
  case TypeKind::Union:
    if (Type.Record)
      record(Type, Out);
    return;
  case TypeKind::Enum:
    if (Type.Enum)
      enumeration(Type, Out);
    return;
  case TypeKind::Typedef:
    if (Type.Target)
      Out += "typedef " + declare(*Type.Target, Names.type(Type.Name)) + ";\n";
    return;
  case TypeKind::Function:
    if (Type.Function)
      Out += "typedef " +
             declare(*makeFunction(Type.Function), Names.type(Type.Name)) +
             ";\n";
    return;
  }
}

void CWriter::forwardDeclaration(const LocalType &Type,
                                 std::string &Out) const {
  if (Type.Kind == TypeKind::Struct)
    Out += "struct " + Names.type(Type.Name) + ";\n";
  else if (Type.Kind == TypeKind::Union)
    Out += "union " + Names.type(Type.Name) + ";\n";
}

/// Whether \p Type is a character type C++ has a keyword for, which C's
/// <uchar.h> declares instead.
bool cppCharacter(Builtin Type) {
  return Type == Builtin::Char8 || Type == Builtin::Char16 ||
         Type == Builtin::Char32;
}

/// C's typedef of such a character type, which C++ keeps its keyword for:
/// char8_t from C++20, the others from C++11.
std::string characterTypedef(Builtin Type, const std::string &Portable) {
  return std::string(Type == Builtin::Char8 ? "#if !defined(__cplusplus) || "
                                              "!defined(__cpp_char8_t)\n"
                                            : "#ifndef __cplusplus\n") +
         "typedef " + Portable + " " + builtinInfo(Type).Keywords +
         ";\n#endif\n";
}

/// Typedefs for the builtins whose name is no keyword: HRESULT, which the
/// Windows headers guard as _HRESULT_DEFINED, and in C the character types
/// <uchar.h> would declare.
std::string namedBuiltins(const CWriter &Writer, const TypeLibrary &Library,
                          bool Cpp) {
  std::string Out;
  for (Builtin Type : Writer.builtins()) {
    const std::string Portable = portableBuiltin(Type, Library.dataModel());
    if (Type == Builtin::HResult)
      Out += "\n#ifndef _HRESULT_DEFINED\n#define _HRESULT_DEFINED\ntypedef " +
             Portable + " HRESULT;\n#endif\n";
    else if (!Cpp && cppCharacter(Type))
      Out += characterTypedef(Type, Portable);
  }
  return Out;
}

/// The declarations the spelling needs ahead of the types.
std::string prelude(const CWriter &Writer, const TypeLibrary &Library,
                    const RenderOptions &Options) {
  const bool Cpp = Options.Lang == Language::Cpp;
  std::string Out;
  // The C headers, which freestanding C++ has too.
  Out += Cpp ? "#include <stddef.h>\n#include <stdint.h>\n"
             : "#include <stdbool.h>\n#include <stddef.h>\n#include "
               "<stdint.h>\n";
  if (Writer.alignsRecords())
    Out += "\n#ifndef DECLSPEC_ALIGN\n#if defined(_MSC_VER)\n#define "
           "DECLSPEC_ALIGN(x) __declspec(align(x))\n#else\n#define "
           "DECLSPEC_ALIGN(x) __attribute__((aligned(x)))\n#endif\n#endif\n";
  // C++'s static_assert, which C11 spells _Static_assert: <assert.h> would
  // name it, but freestanding C, as a driver's, need not have the header.
  const bool LongChecked = spellingOf(Options) == BuiltinSpelling::Keywords &&
                           (Writer.builtins().count(Builtin::Long) ||
                            Writer.builtins().count(Builtin::ULong));
  if (!Cpp && (Writer.asserts() || LongChecked))
    Out += "\n#if !defined(__cplusplus) && !defined(static_assert)\n#define "
           "static_assert _Static_assert\n#endif\n";
  switch (spellingOf(Options)) {
  case BuiltinSpelling::Windows: {
    // The Windows headers declare these; elsewhere they take exact widths.
    Out += "\n#if !defined(_WINDEF_) && !defined(_NTDEF_)\n";
    for (Builtin Type : Writer.builtins()) {
      const BuiltinInfo &Info = builtinInfo(Type);
      const std::string Portable = portableBuiltin(Type, Library.dataModel());
      // A keyword, such as char16_t in C++ or long double, needs no
      // declaring.
      if (Portable == Info.Windows ||
          escapedName(Options.Lang, Info.Windows) != Info.Windows ||
          llvm::StringRef(Info.Windows).contains(' '))
        continue;
      if (Type == Builtin::Void)
        Out += "#define VOID void\n";
      else if (cppCharacter(Type))
        Out += characterTypedef(Type, Portable);
      else
        Out += "typedef " + Portable + " " + Info.Windows + ";\n";
    }
    Out += "#endif\n";
    break;
  }
  case BuiltinSpelling::Keywords:
    if (LongChecked)
      Out +=
          "\nstatic_assert(sizeof(long) == " +
          std::to_string(builtinBytes(Builtin::Long, Library.dataModel())) +
          ", \"these types need a " +
          std::to_string(builtinBytes(Builtin::Long, Library.dataModel()) * 8) +
          "-bit long\");\n";
    Out += namedBuiltins(Writer, Library, Cpp);
    break;
  case BuiltinSpelling::Portable:
    break;
  }
  return Out;
}

} // namespace

struct CSpeller::Impl {
  Impl(const TypeLibrary &Library, const RenderOptions &Options)
      : Options(Options), Writer(Library, this->Options) {}
  RenderOptions Options;
  CWriter Writer;
};

CSpeller::CSpeller(const TypeLibrary &Library, const RenderOptions &Options)
    : P(std::make_unique<Impl>(Library, Options)) {}

CSpeller::~CSpeller() = default;

std::string CSpeller::declare(const TypeExpr &Type,
                              const std::string &Declarator) const {
  return P->Writer.declare(Type, Declarator);
}

std::string renderC(const TypeLibrary &Library, const LocalType &Type,
                    const RenderOptions &Options) {
  CWriter Writer(Library, Options);
  std::string Out;
  Writer.definition(Type, Out);
  return Out;
}

/// Whether \p Name is a C identifier, as a prototype's name must be.
bool isIdentifier(llvm::StringRef Name) {
  return !Name.empty() && !llvm::isDigit(Name.front()) &&
         llvm::all_of(Name,
                      [](char C) { return llvm::isAlnum(C) || C == '_'; });
}

namespace {

/// The types a rendering of \p Roots defines: the roots, what they hold by
/// value, and the typedefs and enumerations any of these names, which C
/// cannot declare; what they only point to is declared.
llvm::StringSet<> definedFor(const TypeLibrary &Library,
                             llvm::ArrayRef<const LocalType *> Roots) {
  llvm::StringSet<> Defined;
  std::vector<const LocalType *> Work(Roots.begin(), Roots.end());
  for (size_t I = 0; I < Work.size(); ++I) {
    if (!Defined.insert(Work[I]->Name).second)
      continue;
    std::vector<std::string> Names;
    mentionedTypes(*Work[I], Names);
    std::vector<std::string> ByValue;
    valueDependencies(*Work[I], ByValue);
    const llvm::StringSet<> Value(llvm::from_range, ByValue);
    for (const std::string &Name : Names) {
      const LocalType *Found = Library.find(Name);
      if (!Found)
        continue;
      const bool Record =
          Found->Kind == TypeKind::Struct || Found->Kind == TypeKind::Union;
      if (!Record || Value.contains(Name))
        Work.push_back(Found);
    }
  }
  return Defined;
}

/// The definitions of \p All that \p Defined holds, enumerations into
/// \p Enums and the rest into \p Body, each after what it holds by value
/// and the typedefs and enumerations it names; \p Guarded wraps each in a
/// guard of its own.  The types defined, in order.
std::vector<const LocalType *>
writeDefinitions(CWriter &Writer, const TypeLibrary &Library,
                 llvm::ArrayRef<const LocalType *> All,
                 const llvm::StringSet<> &Defined, bool Guarded,
                 std::string &Enums, std::string &Body) {
  llvm::StringSet<> Done;
  std::vector<const LocalType *> Order;
  std::function<void(const LocalType &)> Emit = [&](const LocalType &Type) {
    if (!Defined.contains(Type.Name) || !Done.insert(Type.Name).second)
      return;
    // What it holds by value, and the typedefs and enumerations it names
    // anywhere: C declares neither ahead of its definition.
    std::vector<std::string> Needs;
    valueDependencies(Type, Needs);
    std::vector<std::string> Named;
    mentionedTypes(Type, Named);
    for (const std::string &Name : Named)
      if (const LocalType *Found = Library.find(Name);
          Found && Found->Kind != TypeKind::Struct &&
          Found->Kind != TypeKind::Union)
        Needs.push_back(Name);
    for (const std::string &Name : Needs)
      if (const LocalType *Found = Library.find(Name))
        Emit(*Found);
    std::string &Into = Type.Kind == TypeKind::Enum ? Enums : Body;
    const std::string Guard = "NEVERD_TYPE_" + Writer.names().type(Type.Name);
    if (Guarded)
      Into += "#ifndef " + Guard + "\n#define " + Guard + "\n";
    Writer.definition(Type, Into);
    if (Guarded)
      Into += "#endif\n";
    Into += '\n';
    Order.push_back(&Type);
  };
  for (const LocalType *Type : All)
    if (Type->Kind == TypeKind::Enum)
      Emit(*Type);
  for (const LocalType *Type : All)
    Emit(*Type);
  return Order;
}

} // namespace

EmbeddedC renderEmbeddedC(const TypeLibrary &Library,
                          llvm::ArrayRef<const LocalType *> Roots,
                          const RenderOptions &Options) {
  EmbeddedC Result;
  if (Roots.empty())
    return Result;
  const llvm::StringSet<> Defined = definedFor(Library, Roots);
  std::vector<const LocalType *> All;
  for (const LocalType *Type : neededTypes(Library, Roots,
                                           /*ThroughPointers=*/false))
    All.push_back(Type);
  // The typedefs and enumerations the closure names through pointers too.
  for (const LocalType *Type : Library.types())
    if (Defined.contains(Type->Name) && !llvm::is_contained(All, Type))
      All.push_back(Type);
  CWriter Writer(Library, Options);
  std::string Enums, Body;
  const std::vector<const LocalType *> Order = writeDefinitions(
      Writer, Library, All, Defined, /*Guarded=*/true, Enums, Body);
  // The records the definitions only point to, declared.
  std::string Declarations;
  llvm::StringSet<> Declared;
  const NameSpelling &Names = Writer.names();
  for (const LocalType *Type : Order) {
    std::vector<std::string> Named;
    mentionedTypes(*Type, Named);
    for (const std::string &Name : Named)
      if (const LocalType *Found = Library.find(Name);
          Found && !Defined.contains(Name) &&
          (Found->Kind == TypeKind::Struct || Found->Kind == TypeKind::Union) &&
          Declared.insert(Name).second) {
        Writer.forwardDeclaration(*Found, Declarations);
        Result.Declared.emplace_back(Names.type(Name),
                                     Found->Kind == TypeKind::Union);
        Result.DeclaredTypes.push_back(Name);
      }
  }
  for (const LocalType *Type : Order) {
    Result.Types.push_back(Type->Name);
    switch (Type->Kind) {
    case TypeKind::Struct:
    case TypeKind::Union:
      Result.Records.emplace_back(Names.type(Type->Name),
                                  Type->Kind == TypeKind::Union);
      break;
    case TypeKind::Enum:
      if (Type->Enum)
        for (size_t I = 0; I < Type->Enum->Values.size(); ++I)
          Result.Identifiers.push_back(Names.enumerator(*Type->Enum, I));
      break;
    case TypeKind::Typedef:
    case TypeKind::Function:
      Result.Identifiers.push_back(Names.type(Type->Name));
      break;
    }
  }
  Result.Text = "// " + std::to_string(Order.size()) +
                " types, from the program's local types.\n";
  Result.Text += prelude(Writer, Library, Options);
  if (!Declarations.empty())
    Result.Text += "\n" + Declarations;
  Result.Text += "\n" + Enums + Body;
  return Result;
}

std::string renderCHeader(const TypeLibrary &Library,
                          llvm::ArrayRef<const LocalType *> Roots,
                          const RenderOptions &Options) {
  // What the roots hold by value is defined; what they only point to is
  // declared, except enumerations and typedefs, which C cannot declare.
  const std::vector<const LocalType *> All =
      neededTypes(Library, Roots, /*ThroughPointers=*/true);
  llvm::StringSet<> Defined;
  if (Roots.empty()) {
    for (const LocalType *Type : All)
      Defined.insert(Type->Name);
  } else {
    Defined = definedFor(Library, Roots);
  }

  CWriter Writer(Library, Options);
  std::string Declarations, Enums, Body;
  for (const LocalType *Type : All)
    Writer.forwardDeclaration(*Type, Declarations);
  // Enumerations first, then each type after what it holds by value.
  const std::vector<const LocalType *> Done = writeDefinitions(
      Writer, Library, All, Defined, /*Guarded=*/false, Enums, Body);

  // The functions the library knows the prototypes of, by name.
  std::string Prototypes;
  size_t PrototypeCount = 0;
  if (Options.Prototypes && Roots.empty()) {
    std::vector<std::pair<llvm::StringRef, const FunctionDef *>> Sorted;
    for (const auto &Entry : Library.prototypes())
      if (Entry.second && isIdentifier(Entry.first()))
        Sorted.emplace_back(Entry.first(), Entry.second.get());
    llvm::sort(Sorted);
    const bool Cpp = Options.Lang == Language::Cpp;
    for (const auto &[Name, Function] : Sorted) {
      if (Function->NoReturn)
        Prototypes += Cpp ? "[[noreturn]] " : "_Noreturn ";
      Prototypes += Writer.declare(
                        *makeFunction(std::make_shared<FunctionDef>(*Function)),
                        Name.str()) +
                    ";\n";
    }
    PrototypeCount = Sorted.size();
  }

  std::string Out =
      "// " + std::to_string(Done.size()) + " types" +
      (PrototypeCount ? ", " + std::to_string(PrototypeCount) + " prototypes"
                      : std::string()) +
      ", rendered by NeverD.\n#pragma once\n\n";
  Out += prelude(Writer, Library, Options);
  if (!Declarations.empty())
    Out += "\n" + Declarations;
  Out += "\n" + Enums + Body;
  if (!Prototypes.empty())
    Out += "\n" + Prototypes;
  return Out;
}

} // namespace neverd::types::render
