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

#include "neverd/types/HeaderLibrary.h"
#include "neverd/types/Limits.h"
#include "neverd/types/TypeLayout.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
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
#define NEVERD_CALLING_CONVENTION(Name, C, Rust, IDA, Wide, Pops)              \
  case CallingConvention::Name:                                                \
    return C;
#include "neverd/types/CallingConventions.def"
  }
  return "";
}

/// A builtin as exact-width C spells it for \p Library's program.
/// The type a typedef of \p Name stands for, where C reserves the name for
/// the implementation (a leading underscore and an uppercase letter or
/// another underscore): a decompiled unit spells it in the target's exact
/// widths rather than defining it, as <stdint.h> on Darwin defines
/// __darwin_time_t as long, which int64_t, a long long there, is not.
const TypeExpr *reservedTypedef(const TypeLibrary &Library,
                                llvm::StringRef Name) {
  if (Name.size() < 2 || Name[0] != '_' ||
      !(Name[1] == '_' || llvm::isUpper(Name[1])))
    return nullptr;
  const LocalType *Local = Library.find(Name);
  return Local && Local->Kind == TypeKind::Typedef ? Local->Target.get()
                                                   : nullptr;
}

std::string portableBuiltin(Builtin Type, const TypeLibrary &Library) {
  const BuiltinInfo &Info = builtinInfo(Type);
  const unsigned Bytes = Library.builtinBytes(Type);
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

/// The names of the members C fills the bytes between members with.
constexpr llvm::StringLiteral FillerPrefix = "__filler";

/// Whether \p Name is a filler's: __filler0.
bool isFillerName(llvm::StringRef Name) {
  return Name.consume_front(FillerPrefix) && !Name.empty() &&
         llvm::all_of(Name, llvm::isDigit);
}

class CWriter {
public:
  /// \p Embedded: the C a decompiled unit holds, whose code spells the
  /// members as C allows; any other C names what C++ reads too.
  /// \p Prelude: the C a header holds, whose prelude declares what the
  /// writer names, such as a long double exactly as large.
  /// \p Shared: the spelling of the names of \p Library that this writer
  /// would make itself, made once for many writers.
  CWriter(const TypeLibrary &Library, const RenderOptions &Options,
          bool Embedded = false, bool Prelude = false,
          const NameSpelling *Shared = nullptr)
      : Library(Library), Options(Options), Cpp(Options.Lang == Language::Cpp),
        Embedded(Embedded), Prelude(Prelude && !Embedded),
        System(!Embedded && Options.SystemLibrary
                   ? headerLibrary(*Options.SystemLibrary)
                   : nullptr),
        Own(Shared ? std::nullopt
                   : std::optional<NameSpelling>(std::in_place, Library,
                                                 Options.Lang,
                                                 /*CppToo=*/!Embedded)),
        Names(Shared ? *Shared : *Own) {}
  // Names may refer to Own.
  CWriter(const CWriter &) = delete;
  CWriter &operator=(const CWriter &) = delete;

  std::string builtin(Builtin Type) const;
  std::string declare(const TypeExpr &Type, std::string Declarator) const;

  void definition(const LocalType &Type, std::string &Out);
  void forwardDeclaration(const LocalType &Type, std::string &Out) const;
  /// The checks of \p Type's size and offsets, which a definition is
  /// followed by unless they are written apart (separateAsserts).
  void checks(const LocalType &Type, std::string &Out);
  /// Every record has been declared ahead of the definitions, with the
  /// typedef of its name that C names it by alone.
  void declaredAhead() { DeclaredAhead = true; }
  /// The definitions written next leave their checks to checks().
  void separateAsserts(bool Separate) { SeparateAsserts = Separate; }
  /// The library of the system's headers the types may come from.
  const TypeLibrary *system() const { return System; }
  /// The record whose members are unknown that the typedef \p Type is an
  /// array of, as ARM's glibc gives jmp_buf where local types do not hold
  /// its __jmp_buf: C declares no such array.  Null for any other type.
  const LocalType *unknownElement(const LocalType &Type) const;
  /// The builtins rendered so far.
  const std::set<Builtin> &builtins() const { return UsedBuiltins; }
  /// Whether a record rendered so far declares its alignment.
  bool alignsRecords() const { return AlignsRecords; }
  /// The sizes of the long doubles records rendered so far hold, each of
  /// which the prelude declares as exactly so large (longDouble).
  const std::set<unsigned> &longDoubles() const { return LongDoubles; }
  /// Whether a member asks GNU's i386 compilers for the alignment of eight
  /// that Microsoft's give it.
  bool alignsEightByteMembers() const { return EightByteMembers; }
  bool microsoftBitfields() const { return MicrosoftBitfields; }
  /// Whether a record rendered so far asks MinGW's compilers for GNU's
  /// bit-fields.
  bool gnuBitfields() const { return GnuBitfields; }
  /// Whether a type rendered so far is followed by checks.
  bool asserts() const { return Asserted; }
  const NameSpelling &names() const { return Names; }
  /// In a decompiled unit, the type a typedef of a name the implementation
  /// reserves stands for (reservedTypedef); none elsewhere.
  const TypeExpr *spelledThrough(llvm::StringRef Name) const {
    if (Embedded && Options.PrototypeIntegerIdentity)
      if (const LocalType *Local = Library.find(Name);
          Local && Local->Kind == TypeKind::Typedef)
        return Local->Target.get();
    return Embedded ? reservedTypedef(Library, Name) : nullptr;
  }
  /// Take the record \p Name from the system's headers, which name it by a
  /// typedef alone (`typedef struct {...} pthread_mutex_t;`): C spells it by
  /// that name, in a decompiled unit too, which has no tag of it.
  void takeFromHeaders(llvm::StringRef Name) { FromHeaders.insert(Name); }
  bool fromHeaders(llvm::StringRef Name) const {
    return FromHeaders.contains(Name);
  }

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
  /// Whether C names \p Type by its name alone, as a typedef of the name,
  /// its own or the system's, lets it: `typedef struct { ... } X` leaves
  /// the record no tag of its own.  Decompiled code keeps the tags.
  bool bare(const LocalType &Type) const;
  /// Whether a typedef of its name, its own or the system's, names \p Type.
  bool typedefNamed(const LocalType &Type) const;
  /// The typedef that names \p Type by its name alone, where C does so.
  std::string bareTypedef(const LocalType &Type) const;
  /// "struct" or "union", with the alignment \p Record declares.
  std::string keyword(const RecordDef &Record);
  /// The alignment a member of \p Type declares where the 32-bit data
  /// model's own does not hold everywhere: the library aligns an eight-byte
  /// scalar to eight, as Microsoft's compilers do, where GNU's i386 ABI
  /// aligns it to four inside a record.  Empty for any other member.
  std::string memberAlignment(const TypeExpr &Type);
  std::string offset(uint64_t Byte) const {
    return offsetText(Byte, Options, Digits);
  }
  /// Whether \p Record holds in place, not through a record of a name of
  /// its own, a scalar of eight bytes its ABI aligns to less.
  bool holdsLooseScalars(const RecordDef &Record) const;
  /// Every named member reachable as Type.Name, with its byte offset.
  void hoisted(const RecordDef &Record, uint64_t Base,
               std::vector<std::pair<std::string, uint64_t>> &Out) const;

  const TypeLibrary &Library;
  const RenderOptions &Options;
  const bool Cpp;
  const bool Embedded;
  const bool Prelude;
  const TypeLibrary *const System;
  const std::optional<NameSpelling> Own;
  const NameSpelling &Names;
  bool DeclaredAhead = false;
  bool SeparateAsserts = false;
  unsigned Digits = 1;
  unsigned Fillers = 0;
  /// How many signatures the writer is in.
  mutable unsigned Signatures = 0;
  mutable std::set<Builtin> UsedBuiltins;
  mutable std::set<unsigned> LongDoubles;
  bool AlignsRecords = false;
  bool EightByteMembers = false;
  /// Whether a record asks GNU compilers for Microsoft's bit-field layout.
  bool MicrosoftBitfields = false;
  bool GnuBitfields = false;
  bool Asserted = false;
  llvm::StringSet<> FromHeaders;
};

std::string CWriter::builtin(Builtin Type) const {
  UsedBuiltins.insert(Type);
  const BuiltinInfo &Info = builtinInfo(Type);
  const BuiltinSpelling Spelling = spellingOf(Options);
  // A long double a record or a typedef holds, whose size compilers choose
  // for themselves, as MSVC's double and MinGW's x87 bytes are: in a
  // header, one exactly as large, which its prelude declares, for every
  // compiler the spelling is for; the portable double is as large
  // everywhere, and the keywords are the target's own.  A decompiled
  // unit's code reads the program's own, and a type shown alone the
  // program's own name.
  if (Type == Builtin::LongDouble && Prelude && !Signatures) {
    const unsigned Bytes = Library.builtinBytes(Type);
    if (Spelling == BuiltinSpelling::Windows ||
        (Spelling == BuiltinSpelling::Portable && Bytes > 8)) {
      LongDoubles.insert(Bytes);
      return exactLongDoubleName(Bytes);
    }
  }
  switch (Spelling) {
  case BuiltinSpelling::Windows:
    return Info.Windows;
  case BuiltinSpelling::Keywords:
    return Info.Keywords;
  case BuiltinSpelling::Portable:
    if (Options.PrototypeIntegerIdentity &&
        (Type == Builtin::Long || Type == Builtin::ULong ||
         Type == Builtin::LongLong || Type == Builtin::ULongLong)) {
      // A Windows unsigned long and uint32_t have the same ABI width but
      // distinct C types. Keep the header's identity on its own target while
      // preserving the machine width when compiling the source on another.
      const std::string Keywords = Info.Keywords;
      return "__typeof__(__builtin_choose_expr(sizeof(" + Keywords +
             ") == " + std::to_string(Library.builtinBytes(Type)) + ", (" +
             Keywords + ")0, (" + portableBuiltin(Type, Library) + ")0))";
    }
    return portableBuiltin(Type, Library);
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
    if (const TypeExpr *Target = spelledThrough(Type.Name)) {
      if (!Type.Const && !Type.Volatile)
        return declare(*Target, std::move(Declarator));
      TypeExpr Qualified = *Target;
      Qualified.Const |= Type.Const;
      Qualified.Volatile |= Type.Volatile;
      return declare(Qualified, std::move(Declarator));
    }
    std::string Base = Qualifiers;
    const LocalType *Shared = Cpp ? Library.find(Type.Name) : nullptr;
    if (Shared && !Embedded && Shared->Record &&
        Library.findPrototype(Type.Name) && !typedefNamed(*Shared)) {
      // C++ names it by its tag where a routine of its name hides it, as
      // stat() does struct stat.
      Base += Shared->Kind == TypeKind::Union ? "union " : "struct ";
    } else if (!Cpp) {
      // The tag the library's type takes now: a type of another kind may
      // have replaced the one the reference was read with, as a program's
      // own union does a header's structure of the name.
      const LocalType *Named = Library.find(Type.Name);
      const TypeKind Kind = Named ? Named->Kind : Type.NamedKind;
      if (Named && bare(*Named))
        ;
      else if (Kind == TypeKind::Struct)
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
    if (Type.PointerBytes != pointerBytes(Library.dataModel()))
      Inner += " __ptr" + std::to_string(Type.PointerBytes * 8) + " ";
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
    // A signature passes a long double as the target's convention does,
    // not as the bytes a record holds.
    ++Signatures;
    const auto Leave = llvm::make_scope_exit([&] { --Signatures; });
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
  if (Record.Alignment) {
    // As the Windows headers declare it; C++ has a keyword.
    const std::string Bytes = std::to_string(Record.Alignment);
    if (Cpp) {
      Text += " alignas(" + Bytes + ")";
    } else {
      AlignsRecords = true;
      Text += " DECLSPEC_ALIGN(" + Bytes + ")";
    }
  }
  // Microsoft's compilers fill a bit-field's storage unit apart from the
  // members around it, where GNU's pack bits into whatever has room; GNU
  // lays such a record out as Microsoft's do only when asked.
  const bool Bitfields = llvm::any_of(
      Record.Members, [](const Member &Field) { return Field.isBitfield(); });
  if (Library.layoutRules() == LayoutRules::MSVC && Bitfields) {
    MicrosoftBitfields = true;
    Text += " NEVERD_MS_STRUCT";
  }
  // And GNU's for Windows, MinGW's, lay them out as Microsoft's do unless
  // asked otherwise; Microsoft's lay them out no other way.
  if (Library.layoutRules() == LayoutRules::Itanium && Bitfields && !Embedded) {
    GnuBitfields = true;
    Text += " NEVERD_GNU_STRUCT";
  }
  return Text;
}

std::string CWriter::memberAlignment(const TypeExpr &Type) {
  if (Library.dataModel() != DataModel::ILP32)
    return {};
  // What the member holds, through arrays and typedefs.
  const TypeExpr *Held = &Type;
  for (unsigned Hop = 0;
       Held && Hop < ::neverd::limits::types::kMaxCTypeDefinitionHops; ++Hop) {
    if (Held->Kind == TypeExprKind::Array) {
      Held = Held->Element.get();
      continue;
    }
    if (Held->Kind != TypeExprKind::Named)
      break;
    const LocalType *Named = Library.find(Held->Name);
    if (!Named || Named->Kind != TypeKind::Typedef)
      return {};
    Held = Named->Target.get();
  }
  if (!Held || Held->Kind != TypeExprKind::Builtin)
    return {};
  const BuiltinInfo &Info = builtinInfo(Held->TheBuiltin);
  const unsigned Bytes = Library.builtinBytes(Held->TheBuiltin);
  if (Bytes != 8 || Library.alignOf(*Held) != 8 ||
      (Info.Class != BuiltinClass::Integer &&
       Info.Class != BuiltinClass::Float))
    return {};
  // Microsoft's compilers align it to eight already, and a pack they cannot
  // reduce a declared alignment by decides it where one holds the record:
  // only GNU's are asked.
  EightByteMembers = true;
  return "NEVERD_ALIGN8 ";
}

const LocalType *CWriter::unknownElement(const LocalType &Type) const {
  if (Type.Kind != TypeKind::Typedef || !Type.Target)
    return nullptr;
  const TypeExpr *Held = Type.Target.get();
  bool Array = false;
  for (unsigned Hop = 0;
       Held && Hop < ::neverd::limits::types::kMaxCTypeDefinitionHops; ++Hop) {
    if (Held->Kind == TypeExprKind::Array) {
      Array = true;
      Held = Held->Element.get();
      continue;
    }
    const LocalType *Named =
        Held->Kind == TypeExprKind::Named ? Library.find(Held->Name) : nullptr;
    if (Named && Named->Kind == TypeKind::Typedef && Named->Target) {
      Held = Named->Target.get();
      continue;
    }
    return Array && Named && Named->Record && !Named->Record->Defined ? Named
                                                                      : nullptr;
  }
  return nullptr;
}

bool CWriter::holdsLooseScalars(const RecordDef &Record) const {
  for (const Member &Field : Record.Members) {
    // What the member holds: through arrays and typedefs, and the records
    // in place it holds.
    const TypeExpr *Held = Field.Type.get();
    for (unsigned Hop = 0;
         Held && Hop < ::neverd::limits::types::kMaxCTypeDefinitionHops;
         ++Hop) {
      if (Held->Kind == TypeExprKind::Array) {
        Held = Held->Element.get();
        continue;
      }
      const LocalType *Named = Held->Kind == TypeExprKind::Named
                                   ? Library.find(Held->Name)
                                   : nullptr;
      if (Named && Named->Kind == TypeKind::Typedef && Named->Target) {
        Held = Named->Target.get();
        continue;
      }
      break;
    }
    if (!Held)
      continue;
    if (Held->Kind == TypeExprKind::Record && Held->Record &&
        holdsLooseScalars(*Held->Record))
      return true;
    if (Held->Kind == TypeExprKind::Builtin &&
        Library.builtinBytes(Held->TheBuiltin) == 8 &&
        Library.alignOf(*Held) < 8)
      return true;
  }
  return false;
}

bool CWriter::typedefNamed(const LocalType &Type) const {
  if (Type.TypedefNamed)
    return true;
  const LocalType *Own = System ? System->find(Type.Name) : nullptr;
  return Own && Own->TypedefNamed;
}

bool CWriter::bare(const LocalType &Type) const {
  if (!Cpp && FromHeaders.contains(Type.Name))
    return true;
  if (Embedded || Cpp ||
      (Type.Kind != TypeKind::Struct && Type.Kind != TypeKind::Union &&
       Type.Kind != TypeKind::Enum))
    return false;
  return typedefNamed(Type);
}

std::string CWriter::bareTypedef(const LocalType &Type) const {
  if (!bare(Type))
    return {};
  const char *Keyword = Type.Kind == TypeKind::Enum    ? "enum"
                        : Type.Kind == TypeKind::Union ? "union"
                                                       : "struct";
  const std::string Name = Names.type(Type.Name);
  return "typedef " + std::string(Keyword) + " " + Name + " " + Name + ";\n";
}

std::string CWriter::tagged(const LocalType &Type) const {
  const std::string Name = Names.type(Type.Name);
  if (Cpp || bare(Type))
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
    const std::string Name = FillerPrefix.str() + std::to_string(Fillers++);
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
  const unsigned Bytes = Library.builtinBytes(Enum.Underlying);
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
  const bool Gnu = Library.layoutRules() == LayoutRules::Itanium;
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
    // An eight-byte scalar keeps the alignment the library gives it on every
    // 32-bit compiler, unless a pack below eight decides it: a pack only
    // lowers an alignment, so under pack(8) GNU's stays four.
    if (!Field.isBitfield() && (!Pack || Pack >= 8))
      Code = memberAlignment(*Field.Type) + Code;
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
    if (!DeclaredAhead)
      Out += bareTypedef(Type);
    return;
  }
  // A record of no bytes, as Rust's unit structures are, has no C
  // definition: C++ gives an empty one a byte, and C none at all.  It is
  // declared, which a pointer to it needs.
  if (!Record.Size) {
    Out += "// " + Type.Name + ": 0 bytes, which C defines no record of\n";
    Out += std::string(Keyword) + " " + Names.type(Type.Name) + ";\n";
    if (!DeclaredAhead)
      Out += bareTypedef(Type);
    return;
  }
  // A runtime can own a reserved record tag without a complete system
  // library being joined (MSVCRT and UCRT both call FILE's tag _iobuf).
  // Reuse the header only with unconditional checks of this program's
  // layout. A different runtime must fail those checks, never silently
  // substitute its definition or redefine the header's tag.
  bool HeaderRecord = false;
  if (Embedded && Options.Asserts && !SeparateAsserts && !Record.Union &&
      Names.type(Type.Name) == Type.Name) {
#define NEVERD_PRELUDE_RECORD(RecordName, Platform, Header, Declared)          \
  if (Type.Name == #RecordName) {                                              \
    Out += "#if " Platform "\n#include <" Header ">\n#endif\n";                \
    Out += "#if !(" Platform " && " Declared ")\n";                            \
    HeaderRecord = true;                                                       \
  }
#include "neverd/types/PreludeNames.def"
  }
  Digits = offsetDigits(Record.Size);
  Fillers = 0;
  std::string Header = "// " + Type.Name + ": " + hex(Record.Size) + " (" +
                       std::to_string(Record.Size) + ") bytes";
  if (Record.Pack)
    Header += ", packed to " + std::to_string(Record.Pack);
  // The i386 System V ABI aligns long long and double to four in a record,
  // where every other compiler aligns them to eight: the pack that lays
  // such a record out so everywhere, which changes nothing on i386.
  const uint32_t Limit = scalarAlignLimitOf(Library.recordABI());
  uint32_t Pack = Record.Pack;
  if (!Embedded && Limit < 8 && holdsLooseScalars(Record))
    Pack = Pack ? std::min(Pack, Limit) : Limit;
  if (Pack != Record.Pack)
    Header += ", packed to " + std::to_string(Pack) +
              " as the i386 System V ABI aligns its eight-byte scalars";
  Out += Header + "\n";
  if (Pack)
    Out += "#pragma pack(push, " + std::to_string(Pack) + ")\n";
  Out += keyword(Record) + " " + Names.type(Type.Name) + "\n{\n";
  body(Record, Pack, 4, 0, /*Outer=*/true, Out);
  Out += "};\n";
  if (Pack)
    Out += "#pragma pack(pop)\n";
  if (HeaderRecord)
    Out += "#endif\n";
  if (!DeclaredAhead)
    Out += bareTypedef(Type);
  if (Options.Asserts && !SeparateAsserts)
    asserts(Type, Out);
  if (HeaderRecord) {
    // Equal offsets and sizes do not make an integer field a pointer. The
    // source writer's member conversions depend on the declared type too.
    Out += "#if defined(__cplusplus)\n#include <type_traits>\n#endif\n";
    for (const auto &Field : flattenMembers(Record)) {
      if (!Field.Type || Field.BitWidth || Field.Name.empty() ||
          isFillerName(Field.Name))
        continue;
      const std::string Member = Names.member(Field.Name);
      const std::string Access = "((" + tagged(Type) + " *)0)->" + Member;
      const std::string Expected = declare(*Field.Type, "");
      const std::string Message = stringLiteralBody(Type.Name + "." + Member);
      Out += "#if defined(__cplusplus)\nstatic_assert(std::is_same<decltype(" +
             Access + "), " + Expected + ">::value, \"" + Message +
             " type\");\n#else\nstatic_assert(_Generic((" + Access + "), " +
             Expected + ": 1, default: 0), \"" + Message +
             " type\");\n#endif\n";
    }
  }
}

void CWriter::enumeration(const LocalType &Type, std::string &Out) {
  const EnumDef &Enum = *Type.Enum;
  Out += "// " + Type.Name + ": " +
         std::to_string(Library.builtinBytes(Enum.Underlying)) + " bytes\n";
  // An enumeration no declaration lists the values of, as Microsoft's
  // compilers take `typedef enum _X X;` alone, has its underlying type
  // stated: C has no enumeration without enumerators, and C23 and C++ take
  // the type as complete.
  if (Enum.Values.empty()) {
    Out += "enum " + Names.type(Type.Name) + " : " + builtin(Enum.Underlying) +
           ";\n";
  } else {
    std::string Head = enumHead(Enum);
    Head.insert(4, " " + Names.type(Type.Name));
    Out += Head + "\n{\n";
    enumerators(Enum, 4, Out);
    Out += "};\n";
  }
  // C declares no enumeration ahead.
  Out += bareTypedef(Type);
  if (Options.Asserts && !SeparateAsserts)
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
    // Nor a filler of the bytes between, which C reads back as a member a
    // system's own declaration does not have.
    if (Field.isBitfield() || Field.Name.empty() || isFillerName(Field.Name))
      continue;
    Out.push_back({Names.member(Field.Name), (Base + Field.BitOffset) / 8});
  }
}

void CWriter::asserts(const LocalType &Type, std::string &Out) {
  // C11's <assert.h> names _Static_assert so; the prelude does without it.
  const char *Assert = "static_assert";
  Asserted = true;
  const std::string Name = tagged(Type);
  const std::string Quoted = stringLiteralBody(Type.Name);
  if (Type.Kind == TypeKind::Enum) {
    Out += std::string(Assert) + "(sizeof(" + Name + ") == " +
           std::to_string(Library.builtinBytes(Type.Enum->Underlying)) +
           ", \"" + Quoted + "\");\n";
    return;
  }
  const RecordDef &Record = *Type.Record;
  Out += std::string(Assert) + "(sizeof(" + Name + ") == " + hex(Record.Size) +
         ", \"" + Quoted + "\");\n";
  std::vector<std::pair<std::string, uint64_t>> Members;
  hoisted(Record, 0, Members);
  llvm::StringSet<> Seen;
  for (const auto &[Member, Byte] : Members)
    if (Seen.insert(Member).second)
      Out += std::string(Assert) + "(offsetof(" + Name + ", " + Member +
             ") == " + hex(Byte) + ", \"" + Quoted + "." + Member + "\");\n";
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
    if (!Type.Target)
      return;
    if (const LocalType *Unknown = unknownElement(Type)) {
      Out += "// " + Type.Name + ": an array of " + Unknown->Name +
             ", whose members are unknown: C declares no array of it.\n";
      return;
    }
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
  if (Type.Kind != TypeKind::Struct && Type.Kind != TypeKind::Union)
    return;
  if (const std::string Typedef = bareTypedef(Type); !Typedef.empty())
    Out += Typedef;
  else
    Out += (Type.Kind == TypeKind::Struct ? "struct " : "union ") +
           Names.type(Type.Name) + ";\n";
}

void CWriter::checks(const LocalType &Type, std::string &Out) {
  const bool LaidOut =
      Type.Kind == TypeKind::Enum
          ? Type.Enum != nullptr
          : (Type.Kind == TypeKind::Struct || Type.Kind == TypeKind::Union) &&
                Type.Record && Type.Record->Defined && Type.Record->Size;
  if (Options.Asserts && LaidOut)
    asserts(Type, Out);
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
    const std::string Portable = portableBuiltin(Type, Library);
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
  const bool ByteLongDoubles =
      !Writer.longDoubles().empty() && *Writer.longDoubles().rbegin() > 8;
  if (Writer.alignsRecords() || ByteLongDoubles)
    Out += "\n#ifndef DECLSPEC_ALIGN\n#if defined(_MSC_VER)\n#define "
           "DECLSPEC_ALIGN(x) __declspec(align(x))\n#else\n#define "
           "DECLSPEC_ALIGN(x) __attribute__((aligned(x)))\n#endif\n#endif\n";
  // The program's long double: the compiler's own where it is as large,
  // else as many bytes, aligned as the program aligns them, or for one of
  // eight a double, which MSVC's long double is.
  const std::string Align =
      std::to_string(Library.alignOf(*makeBuiltin(Builtin::LongDouble)));
  for (unsigned Bytes : Writer.longDoubles()) {
    const std::string Size = std::to_string(Bytes);
    const std::string Name = exactLongDoubleName(Bytes);
    Out += "\n#ifndef NEVERD_LONG_DOUBLE_" + Size +
           "\n#define NEVERD_LONG_DOUBLE_" + Size + "\n#if " +
           (Bytes == 8 ? "defined(_MSC_VER) || " : "") +
           "(defined(__SIZEOF_LONG_DOUBLE__) && __SIZEOF_LONG_DOUBLE__ == " +
           Size + ")\ntypedef long double " + Name + ";\n#else\n" +
           (Bytes == 8 ? "typedef double " + Name + ";\n"
                       : "typedef struct DECLSPEC_ALIGN(" + Align +
                             ")\n{\n    uint8_t bytes[" + Size + "];\n} " +
                             Name + ";\n") +
           "#endif\n#endif\n";
  }
  // GNU's i386 compilers align an eight-byte member to eight as
  // Microsoft's do when asked, which a pack still reduces.
  if (Writer.alignsEightByteMembers())
    Out += "\n#ifndef NEVERD_ALIGN8\n#if !defined(_MSC_VER) && "
           "defined(__i386__)\n#define NEVERD_ALIGN8 "
           "__attribute__((aligned(8)))\n#else\n#define "
           "NEVERD_ALIGN8\n#endif\n#endif\n";
  // GNU's x86 compilers lay a record's bit-fields out as Microsoft's do
  // when asked; Microsoft's need not be.
  if (Writer.microsoftBitfields())
    Out += "\n#ifndef NEVERD_MS_STRUCT\n#if !defined(_MSC_VER) && "
           "(defined(__i386__) || defined(__x86_64__))\n#define "
           "NEVERD_MS_STRUCT __attribute__((ms_struct))\n#else\n#define "
           "NEVERD_MS_STRUCT\n#endif\n#endif\n";
  // MinGW's lay them out as GNU's do elsewhere when asked; Microsoft's own
  // compilers cannot, which the checks after such a record then report.
  if (Writer.gnuBitfields())
    Out += "\n#ifndef NEVERD_GNU_STRUCT\n#if defined(__MINGW32__) && "
           "(defined(__i386__) || defined(__x86_64__))\n#define "
           "NEVERD_GNU_STRUCT __attribute__((gcc_struct))\n#else\n#define "
           "NEVERD_GNU_STRUCT\n#endif\n#endif\n";
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
      const std::string Portable = portableBuiltin(Type, Library);
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
      Out += "\nstatic_assert(sizeof(long) == " +
             std::to_string(Library.builtinBytes(Builtin::Long)) +
             ", \"these types need a " +
             std::to_string(Library.builtinBytes(Builtin::Long) * 8) +
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
  // Types spelled as decompiled code spells them, beside the definitions
  // its unit embeds.
  Impl(const TypeLibrary &Library, const RenderOptions &Options)
      : Options(Options), Writer(Library, this->Options, /*Embedded=*/true) {}
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

/// The system's own definition of \p Type, of \p Library, as a header of
/// \p System gives it (LocalType::Header) and lays it out as the program
/// does: a type of the same kind and size whose named members are where the
/// program's are.  Null for a type the program defines its own way, or
/// where the system gives none.  A record or enumeration the header names
/// by a typedef alone (`typedef struct {...} pthread_mutex_t;`, <search.h>'s
/// `typedef enum {...} VISIT;`) has no tag: a unit spells it by that name and
/// declares no typedef of its own (EmbeddedC's HeaderTypedefs).
const LocalType *systemsOwn(const TypeLibrary &Library, const LocalType &Type,
                            const TypeLibrary *System) {
  const LocalType *Their = System ? System->find(Type.Name) : nullptr;
  if (!Their || Their->Header.empty() || Their->Kind != Type.Kind)
    return nullptr;
  if (Their == &Type)
    return Their;
  switch (Type.Kind) {
  case TypeKind::Struct:
  case TypeKind::Union: {
    if (!Type.Record || !Their->Record || !Type.Record->Defined ||
        !Their->Record->Defined || Type.Record->Size != Their->Record->Size)
      return nullptr;
    std::set<std::pair<uint64_t, std::string>> Members;
    for (const FlatMember &Flat : flattenMembers(*Their->Record))
      if (!Flat.BitWidth && !Flat.Name.empty())
        Members.emplace(Flat.BitOffset / 8, Flat.Name);
    for (const FlatMember &Flat : flattenMembers(*Type.Record))
      if (!Flat.BitWidth && !Flat.Name.empty() && !isFillerName(Flat.Name) &&
          !Members.count({Flat.BitOffset / 8, Flat.Name}))
        return nullptr;
    return Their;
  }
  case TypeKind::Typedef: {
    if (!Type.Target || !Their->Target)
      return nullptr;
    const std::optional<uint64_t> OwnSize = Library.sizeOf(*Type.Target);
    const std::optional<uint64_t> TheirSize = System->sizeOf(*Their->Target);
    return OwnSize && TheirSize && *OwnSize == *TheirSize ? Their : nullptr;
  }
  case TypeKind::Enum:
    return Type.Enum && Their->Enum &&
                   Library.builtinBytes(Type.Enum->Underlying) ==
                       System->builtinBytes(Their->Enum->Underlying)
               ? Their
               : nullptr;
  default:
    return nullptr;
  }
}

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

/// The record \p Type names, if it is a typedef, or a chain of them, of a
/// structure or union by its tag.
const LocalType *typedefRecord(const TypeLibrary &Library,
                               const LocalType &Type) {
  const LocalType *At = &Type;
  for (unsigned Depth = 0;
       Depth < ::neverd::limits::types::kCTypeRenderTypedefRecordDepthLimit &&
       At->Kind == TypeKind::Typedef && At->Target &&
       At->Target->Kind == TypeExprKind::Named;
       ++Depth) {
    const LocalType *Next = Library.find(At->Target->Name);
    if (!Next)
      return nullptr;
    if (Next->Kind == TypeKind::Struct || Next->Kind == TypeKind::Union)
      return Next;
    At = Next;
  }
  return nullptr;
}

/// The definitions of \p All that \p Defined holds, enumerations into
/// \p Enums and the rest into \p Body, each after what it holds by value
/// and the typedefs and enumerations it names; \p Guarded wraps each in a
/// guard of its own, and \p Pragmas puts it after the system header that
/// declares it.  The types defined, in order.
std::vector<const LocalType *>
writeDefinitions(CWriter &Writer, const TypeLibrary &Library,
                 llvm::ArrayRef<const LocalType *> All,
                 const llvm::StringSet<> &Defined, bool Guarded, bool Pragmas,
                 std::string &Enums, std::string &Body) {
  llvm::StringSet<> Done;
  std::vector<const LocalType *> Order;
  std::string EnumHeader, BodyHeader;
  std::function<void(const LocalType &)> Emit = [&](const LocalType &Type) {
    if (!Defined.contains(Type.Name) || !Done.insert(Type.Name).second)
      return;
    // A typedef the unit spells through defines nothing; what it stands for
    // is defined as anything the unit names is.
    if (Writer.spelledThrough(Type.Name)) {
      std::vector<std::string> Named;
      mentionedTypes(Type, Named);
      for (const std::string &Name : Named)
        if (const LocalType *Found = Library.find(Name))
          Emit(*Found);
      return;
    }
    // What it holds by value, and the typedefs and enumerations it names
    // anywhere: C declares neither ahead of its definition.  A typedef of
    // a record by its tag declares the record as it names it, and needs no
    // definition before it; in EDK II's EFI_PEI_NOTIFY_DESCRIPTOR it cannot
    // have one, as the record holds a function pointer whose parameter the
    // typedef names.  What holds such a typedef by value holds the record.
    std::vector<std::string> Needs;
    if (!typedefRecord(Library, Type))
      valueDependencies(Type, Needs);
    for (size_t I = 0, ByValue = Needs.size(); I < ByValue; ++I)
      if (const LocalType *Found = Library.find(Needs[I]))
        if (const LocalType *Record = typedefRecord(Library, *Found))
          Needs.push_back(Record->Name);
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
    if (std::string &Header =
            Type.Kind == TypeKind::Enum ? EnumHeader : BodyHeader;
        Pragmas && Type.Header != Header) {
      Header = Type.Header;
      Into += "#pragma neverd_header(" +
              (Header.empty() ? std::string() : "\"" + Header + "\"") + ")\n";
    }
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
                          const RenderOptions &Options,
                          const NameSpelling *Spelled) {
  EmbeddedC Result;
  if (Roots.empty())
    return Result;
  llvm::StringSet<> Defined = definedFor(Library, Roots);
  std::vector<const LocalType *> All;
  for (const LocalType *Type : neededTypes(Library, Roots,
                                           /*ThroughPointers=*/false))
    All.push_back(Type);
  // The typedefs and enumerations the closure names through pointers too.
  for (const LocalType *Type : Library.types())
    if (Defined.contains(Type->Name) && !llvm::is_contained(All, Type))
      All.push_back(Type);
  // The system's own types, which a unit that includes its headers has from
  // them: the unit includes the headers and checks that they lay each type
  // out as the program does, and defines the rest.
  const TypeLibrary *System =
      Options.SystemLibrary && headerLibrarySystem(*Options.SystemLibrary)
          ? headerLibrary(*Options.SystemLibrary)
          : nullptr;
  std::vector<const LocalType *> Theirs;
  std::set<std::string> TheirHeaders;
  llvm::erase_if(All, [&](const LocalType *Type) {
    const LocalType *Their = systemsOwn(Library, *Type, System);
    if (!Their)
      return false;
    Theirs.push_back(Type);
    TheirHeaders.insert(Their->Header);
    Defined.erase(Type->Name);
    return true;
  });
  CWriter Writer(Library, Options, /*Embedded=*/true, /*Prelude=*/false,
                 Spelled);
  // A record or enumeration a header names by a typedef alone comes from it
  // by that name: one the unit holds, which the header lays out as the
  // program does, and one its definitions only point to, whose members the
  // header's agree with.
  auto TakeTheirs = [&](const LocalType &Type) {
    if (const LocalType *Their = systemsOwn(Library, Type, System);
        Their && Their->TypedefNamed &&
        (Type.Kind == TypeKind::Struct || Type.Kind == TypeKind::Union ||
         Type.Kind == TypeKind::Enum) &&
        !Writer.fromHeaders(Type.Name)) {
      Writer.takeFromHeaders(Type.Name);
      Result.HeaderTypedefs.push_back(Writer.names().type(Type.Name));
    }
  };
  for (const LocalType *Type : Theirs)
    TakeTheirs(*Type);
  for (const LocalType *Type : All) {
    std::vector<std::string> Named;
    mentionedTypes(*Type, Named);
    for (const std::string &Name : Named)
      if (const LocalType *Found = Library.find(Name);
          Found && !Defined.contains(Name))
        TakeTheirs(*Found);
  }
  std::string TheirChecks;
  for (const LocalType *Type : Theirs)
    Writer.checks(*Type, TheirChecks);
  std::string Enums, Body;
  const std::vector<const LocalType *> Order =
      writeDefinitions(Writer, Library, All, Defined, /*Guarded=*/true,
                       /*Pragmas=*/false, Enums, Body);
  // The records the definitions only point to, declared, through the
  // typedefs the unit spells through too.
  std::string Declarations;
  llvm::StringSet<> Declared;
  const NameSpelling &Names = Writer.names();
  for (const LocalType *Type : Order) {
    std::vector<std::string> Named;
    mentionedTypes(*Type, Named);
    llvm::StringSet<> Through;
    for (size_t I = 0; I < Named.size(); ++I)
      if (Writer.spelledThrough(Named[I]) && Through.insert(Named[I]).second)
        mentionedTypes(*Library.find(Named[I]), Named);
    for (const std::string &Name : Named)
      if (const LocalType *Found = Library.find(Name);
          Found && !Defined.contains(Name) && !Writer.fromHeaders(Name) &&
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
  // The system's types are the program's too, which another language's
  // spelling of the unit defines.
  for (const LocalType *Type : Theirs) {
    Result.Types.push_back(Type->Name);
    Result.HeaderTypes.push_back(Type->Name);
    if (Type->Kind == TypeKind::Struct || Type->Kind == TypeKind::Union)
      Result.HeaderRecords.emplace_back(Names.type(Type->Name),
                                        Type->Kind == TypeKind::Union);
  }
  // A function the system's headers declare by the name of a record they
  // define is an ordinary identifier the text declares (<sys/sysinfo.h>'s
  // sysinfo beside struct sysinfo, sigaction, flock): a unit spells the
  // record by its tag, which a typedef of its name would contradict.
  for (const LocalType *Type : Theirs)
    if ((Type->Kind == TypeKind::Struct || Type->Kind == TypeKind::Union) &&
        System && System->findPrototype(Type->Name))
      Result.Identifiers.push_back(Names.type(Type->Name));
  Result.Text = "// " + std::to_string(Order.size()) +
                " types, from the program's local types.\n";
  if (!TheirHeaders.empty()) {
    Result.Text += "// " + std::to_string(Theirs.size()) +
                   " more, from the system's headers.\n";
    for (const std::string &Header : TheirHeaders)
      Result.Text += "#include <" + Header + ">\n";
  }
  Result.Text += prelude(Writer, Library, Options);
  if (!Declarations.empty())
    Result.Text += "\n" + Declarations;
  Result.Text += "\n" + Enums + Body;
  if (!TheirChecks.empty())
    Result.Text += "\n" + TheirChecks;
  return Result;
}

/// The condition under which the compiler's own headers declare the type
/// \p Name (PreludeNames.def), "1" where they all do; none where none does.
std::optional<llvm::StringRef> preludeType(llvm::StringRef Name) {
#define NEVERD_PRELUDE_TYPE(Type, Declared)                                    \
  if (Name == #Type)                                                           \
    return llvm::StringRef(Declared);
#include "neverd/types/PreludeNames.def"
  return std::nullopt;
}

/// Whether a C library's own headers may define \p Name as a macro
/// (PreludeNames.def).
bool preludeMacro(llvm::StringRef Name) {
#define NEVERD_PRELUDE_MACRO(Macro)                                            \
  if (Name == #Macro)                                                          \
    return true;
#include "neverd/types/PreludeNames.def"
  return false;
}

/// Whether C reserves \p Name, a type's, to its implementations: every
/// name a file declares that begins with an underscore, as macOS's
/// _opaque_pthread_t does.
bool reservedName(llvm::StringRef Name) { return Name.starts_with("_"); }

/// The names \p Record's members take, in records in place too.
void memberNames(const RecordDef &Record, std::set<std::string> &Names) {
  for (const Member &Field : Record.Members) {
    if (!Field.Name.empty())
      Names.insert(Field.Name);
    if (Field.Type && Field.Type->Kind == TypeExprKind::Record &&
        Field.Type->Record)
      memberNames(*Field.Type->Record, Names);
  }
}

/// \p Lines under the preprocessor condition \p Condition, where it is not
/// "1".
std::string under(llvm::StringRef Condition, const std::string &Lines) {
  if (Lines.empty())
    return {};
  return Condition == "1"
             ? Lines
             : "#if " + Condition.str() + "\n" + Lines + "#endif\n";
}

/// What tells C's preprocessor that a compiler's headers are \p System's,
/// which defines NEVERD_LIBC_FROM_SYSTEM where they are.
std::string systemProbe(const HeaderSystem &System) {
  const std::string Name = System.Name.str(), Feature = System.Feature.str();
  std::string Out =
      "// The types " + Name +
      "'s headers declare come from them where the "
      "compiler has\n// " +
      Name +
      " for the program's architecture, which "
      "these definitions are\n// otherwise: define NEVERD_OWN_LIBC_TYPES for "
      "these definitions alone.\n#if !defined(NEVERD_OWN_LIBC_TYPES) && " +
      System.Platform.str() + " && \\\n    " + System.Arch.str() + "\n";
  if (!Feature.empty())
    Out += "#ifndef " + Feature + "\n#define " + Feature + " 1\n#endif\n";
  if (!System.Probe.empty())
    Out += "#if defined(__has_include)\n#if __has_include(<" +
           System.Probe.str() + ">)\n#include <" + System.Probe.str() +
           ">\n#endif\n#endif\n";
  Out += "#if " + System.Test.str() + "\n";
  if (!System.Configured.empty())
    Out += "#if !(" + System.Configured.str() + ")\n#error \"" + Name +
           "'s headers came before these types without " + Feature +
           ", which these types need: include them first, or define " +
           Feature + " first\"\n#endif\n";
  Out += "#define NEVERD_LIBC_FROM_SYSTEM 1\n#endif\n#endif\n";
  return Out;
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

  CWriter Writer(Library, Options, /*Embedded=*/false, /*Prelude=*/true);
  Writer.declaredAhead();
  // The system's own types, which its headers declare: those of a name its
  // library has, the program's own definitions of them too, come from its
  // headers where the compiler has them, and are defined only where it does
  // not.
  const std::optional<HeaderSystem> System =
      Options.SystemLibrary ? headerLibrarySystem(*Options.SystemLibrary)
                            : std::nullopt;
  const TypeLibrary *SystemTypes = System ? Writer.system() : nullptr;
  const auto theirs = [&](llvm::StringRef Name) {
    return SystemTypes && SystemTypes->find(Name);
  };
  std::vector<const LocalType *> Theirs, Own;
  for (const LocalType *Type : All)
    (theirs(Type->Name) ? Theirs : Own).push_back(Type);
  llvm::StringSet<> TheirDefined, OwnDefined;
  for (const auto &Entry : Defined)
    (theirs(Entry.getKey()) ? TheirDefined : OwnDefined).insert(Entry.getKey());

  // Their definitions stand where the system's headers do not; their checks
  // hold either way.
  std::string TheirDeclarations, TheirEnums, TheirBody, TheirChecks;
  for (const LocalType *Type : Theirs)
    Writer.forwardDeclaration(*Type, TheirDeclarations);
  Writer.separateAsserts(true);
  const std::vector<const LocalType *> TheirDone = writeDefinitions(
      Writer, Library, Theirs, TheirDefined,
      /*Guarded=*/false, Options.HeaderPragmas, TheirEnums, TheirBody);
  Writer.separateAsserts(false);
  std::set<std::string> Includes;
  for (const LocalType *Type : TheirDone) {
    Writer.checks(*Type, TheirChecks);
    if (const LocalType *Declared = SystemTypes->find(Type->Name);
        !Declared->Header.empty())
      Includes.insert(Declared->Header);
  }

  std::string Declarations, Enums, Body;
  for (const LocalType *Type : Own)
    Writer.forwardDeclaration(*Type, Declarations);
  // Enumerations first, then each type after what it holds by value.
  const std::vector<const LocalType *> Done =
      writeDefinitions(Writer, Library, Own, OwnDefined, /*Guarded=*/false,
                       Options.HeaderPragmas, Enums, Body);

  // The functions the library knows the prototypes of, by name: the
  // system's where its headers do not declare them.
  std::string Prototypes, TheirPrototypes;
  size_t PrototypeCount = 0;
  if (Options.Prototypes && Roots.empty()) {
    std::vector<std::pair<llvm::StringRef, const FunctionDef *>> Sorted;
    // A routine a program imports by a name C cannot spell, as Mach-O's
    // stat$INODE64, is the assembler name of a C name that shares its
    // prototype: the declaration a reader takes both names from.
    std::map<const FunctionDef *, std::vector<llvm::StringRef>> Symbols;
    for (const auto &Entry : Library.prototypes()) {
      if (!Entry.second)
        continue;
      if (isIdentifier(Entry.first()))
        Sorted.emplace_back(Entry.first(), Entry.second.get());
      else if (llvm::all_of(Entry.first(), [](char C) {
                 return llvm::isPrint(C) && C != '"' && C != '\\';
               }))
        Symbols[Entry.second.get()].push_back(Entry.first());
    }
    llvm::sort(Sorted);
    for (auto &[Function, Names] : Symbols)
      llvm::sort(Names);
    const bool Cpp = Options.Lang == Language::Cpp;
    for (const auto &[Name, Function] : Sorted) {
      std::string &Into = SystemTypes && SystemTypes->findPrototype(Name)
                              ? TheirPrototypes
                              : Prototypes;
      // A routine that takes or returns a type C declares no definition of.
      LocalType Signature;
      Signature.Kind = TypeKind::Function;
      Signature.Function = std::make_shared<FunctionDef>(*Function);
      std::vector<std::string> Mentioned;
      mentionedTypes(Signature, Mentioned);
      if (const auto Missing =
              llvm::find_if(Mentioned,
                            [&](const std::string &Type) {
                              const LocalType *Found = Library.find(Type);
                              return Found && Writer.unknownElement(*Found);
                            });
          Missing != Mentioned.end()) {
        Into += "// " + Name.str() + ": takes " + *Missing +
                ", which C declares no definition of here.\n";
        continue;
      }
      if (Function->NoReturn)
        Into += Cpp ? "[[noreturn]] " : "_Noreturn ";
      Into += Writer.declare(
          *makeFunction(std::make_shared<FunctionDef>(*Function)), Name.str());
      // The first C name of the prototype takes its first such name; C gives
      // a routine one assembler name.
      if (auto Labels = Symbols.find(Function);
          Labels != Symbols.end() && !Labels->second.empty()) {
        Into += " __asm__(\"" + Labels->second.front().str() + "\")";
        Labels->second.erase(Labels->second.begin());
        ++PrototypeCount;
      }
      Into += ";\n";
    }
    PrototypeCount += Sorted.size();
  }

  const bool FromSystem =
      System && (!TheirDone.empty() || !TheirPrototypes.empty());
  // Where the compiler is not the system, its own headers may declare the
  // system's names otherwise: the system's types defined here take other
  // names for these definitions, which leave the names the compiler's.
  std::map<std::string, std::string> Renames, Restores;
  if (FromSystem)
    for (const LocalType *Type : Theirs) {
      const std::string Name = Writer.names().type(Type->Name);
      const std::optional<llvm::StringRef> Declared = preludeType(Name);
      if (!Declared && !reservedName(Name))
        continue;
      const std::string Condition = Declared ? Declared->str() : "1";
      Renames[Condition] += "#pragma push_macro(\"" + Name + "\")\n#define " +
                            Name + " neverd_" + Name + "\n";
      Restores[Condition] += "#pragma pop_macro(\"" + Name + "\")\n";
    }
  // The program's own types defined here likewise, where the compiler's
  // headers declare their names, as MinGW's do the Windows C runtime's
  // records a Windows program joins.
  std::map<std::string, std::string> OwnRenames, OwnRestores;
  // The source a library NeverD ships holds is NeverD's to read alone,
  // which needs none of what other compilers do.
  const bool Shipped = Options.HeaderPragmas;
  for (const LocalType *Type :
       Shipped ? llvm::ArrayRef<const LocalType *>() : llvm::ArrayRef(Own)) {
    const std::string Name = Writer.names().type(Type->Name);
    if (const std::optional<llvm::StringRef> Declared = preludeType(Name)) {
      OwnRenames[Declared->str()] += "#pragma push_macro(\"" + Name +
                                     "\")\n#define " + Name + " neverd_" +
                                     Name + "\n";
      OwnRestores[Declared->str()] += "#pragma pop_macro(\"" + Name + "\")\n";
    }
  }
  // And may define the name of a member as a macro.
  std::set<std::string> Members;
  for (const auto *Order : {&TheirDone, &Done})
    for (const LocalType *Type : *Order)
      if (Type->Record)
        memberNames(*Type->Record, Members);
  // after the system's headers have read them, and before the definitions.
  std::string Undefines, Redefines;
  for (const std::string &Member : Members)
    if (!Shipped && preludeMacro(Member)) {
      Undefines +=
          "#pragma push_macro(\"" + Member + "\")\n#undef " + Member + "\n";
      Redefines += "#pragma pop_macro(\"" + Member + "\")\n";
    }
  if (!Undefines.empty())
    Undefines = "// The member names a C library's headers may define as "
                "macros.\n" +
                Undefines;
  std::string Hide, Show;
  if (!Renames.empty()) {
    Hide += "\n// The names the system's types defined here take where the "
            "compiler's own\n// headers may declare them otherwise.\n#if "
            "!defined(NEVERD_LIBC_FROM_SYSTEM)\n";
    for (const auto &[Condition, Lines] : Renames)
      Hide += under(Condition, Lines);
    Hide += "#endif\n";
    Show += "\n#if !defined(NEVERD_LIBC_FROM_SYSTEM)\n";
    for (const auto &[Condition, Lines] : Restores)
      Show += under(Condition, Lines);
    Show += "#endif\n";
  }
  if (!OwnRenames.empty()) {
    Hide += "\n// The names the types defined here take where the compiler's "
            "own headers\n// declare them otherwise.\n";
    for (const auto &[Condition, Lines] : OwnRenames)
      Hide += under(Condition, Lines);
    Show += "\n";
    for (const auto &[Condition, Lines] : OwnRestores)
      Show += under(Condition, Lines);
  }
  if (!Redefines.empty())
    Show += "\n" + Redefines;
  std::string Out =
      "// " + std::to_string(Done.size() + TheirDone.size()) + " types" +
      (PrototypeCount ? ", " + std::to_string(PrototypeCount) + " prototypes"
                      : std::string()) +
      ", rendered by NeverD.\n#pragma once\n\n";
  // The system's feature macro comes before any header that reads it.
  if (FromSystem)
    Out += systemProbe(*System) + "\n";
  Out += prelude(Writer, Library, Options);
  Out += Hide;
  if (FromSystem && !TheirDone.empty()) {
    Out += "\n#if defined(NEVERD_LIBC_FROM_SYSTEM)\n";
    for (const std::string &Header : Includes)
      Out += "#include <" + Header + ">\n";
    Out += Undefines + "#else\n" + Undefines;
    if (!TheirDeclarations.empty())
      Out += TheirDeclarations + "\n";
    Out += TheirEnums + TheirBody + "#endif\n";
    // The system's own declarations are checked as C, which some C
    // libraries declare otherwise for C++, as glibc's __SOCKADDR_ARG is.
    if (!TheirChecks.empty())
      Out += "\n#if !defined(NEVERD_LIBC_FROM_SYSTEM) || "
             "!defined(__cplusplus)\n" +
             TheirChecks + "#endif\n";
  } else {
    if (!Undefines.empty())
      Out += "\n" + Undefines;
    if (!TheirDone.empty())
      Out += "\n" + TheirDeclarations + "\n" + TheirEnums + TheirBody +
             TheirChecks;
  }
  if (!Declarations.empty())
    Out += "\n" + Declarations;
  Out += "\n" + Enums + Body;
  if (!TheirPrototypes.empty())
    Out += FromSystem ? "\n#if !defined(NEVERD_LIBC_FROM_SYSTEM)\n" +
                            TheirPrototypes + "#endif\n"
                      : "\n" + TheirPrototypes;
  if (!Prototypes.empty())
    Out += "\n" + Prototypes;
  Out += Show;
  return Out;
}

} // namespace neverd::types::render
