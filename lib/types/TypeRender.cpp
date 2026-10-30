//===- TypeRender.cpp - Local types as C, C++, Rust and Go ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/TypeRender.h"

#include "TypeRenderShared.h"

#include "neverd/types/TypeLayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>

namespace neverd::types {
namespace {

struct LanguageName {
  Language Lang;
  const char *Id;
  const char *Display;
  BuiltinSpelling Spelling;
};

constexpr LanguageName Languages[] = {
#define NEVERD_TYPE_LANGUAGE(Name, Id, Display, Spelling)                      \
  {Language::Name, Id, Display, BuiltinSpelling::Spelling},
#include "neverd/types/TypeLanguages.def"
};

struct SpellingName {
  BuiltinSpelling Spelling;
  const char *Id;
};

constexpr SpellingName Spellings[] = {
#define NEVERD_BUILTIN_SPELLING(Name, Id) {BuiltinSpelling::Name, Id},
#include "neverd/types/BuiltinSpellings.def"
};

struct LanguageSpelling {
  Language Lang;
  BuiltinSpelling Spelling;
};

constexpr LanguageSpelling Taken[] = {
#define NEVERD_TYPE_SPELLING(Lang, Spelling)                                   \
  {Language::Lang, BuiltinSpelling::Spelling},
#include "neverd/types/TypeLanguages.def"
};

struct Keyword {
  Language Lang;
  const char *Word;
};

constexpr Keyword Keywords[] = {
#define NEVERD_TYPE_KEYWORD(Lang, Word) {Language::Lang, Word},
#include "neverd/types/TypeLanguages.def"
};

bool reserved(Language Lang, llvm::StringRef Name) {
  // A header can spell thousands of names. Index the shared language table
  // once instead of scanning every language's keywords for each identifier.
  static const auto ByLanguage = [] {
    std::array<llvm::StringSet<>, std::size(Languages)> Sets;
    for (const Keyword &Entry : Keywords) {
      Sets[size_t(Entry.Lang)].insert(Entry.Word);
      // C++ reserves C's words as well as its own.
      if (Entry.Lang == Language::C)
        Sets[size_t(Language::Cpp)].insert(Entry.Word);
    }
    return Sets;
  }();
  const size_t Index = size_t(Lang);
  return Index < ByLanguage.size() && ByLanguage[Index].contains(Name);
}

} // namespace

llvm::StringRef spellingId(BuiltinSpelling Spelling) {
  for (const SpellingName &Entry : Spellings)
    if (Entry.Spelling == Spelling)
      return Entry.Id;
  return "";
}

std::optional<BuiltinSpelling> spellingNamed(llvm::StringRef Id) {
  for (const SpellingName &Entry : Spellings)
    if (Id == Entry.Id)
      return Entry.Spelling;
  return std::nullopt;
}

bool spellsBuiltins(Language Lang, BuiltinSpelling Spelling) {
  for (const LanguageSpelling &Entry : Taken)
    if (Entry.Lang == Lang && Entry.Spelling == Spelling)
      return true;
  return false;
}

BuiltinSpelling defaultSpelling(Language Lang) {
  for (const LanguageName &Entry : Languages)
    if (Entry.Lang == Lang)
      return Entry.Spelling;
  return BuiltinSpelling::Portable;
}

std::vector<BuiltinSpelling> spellingsOf(Language Lang) {
  std::vector<BuiltinSpelling> Out;
  for (const LanguageSpelling &Entry : Taken)
    if (Entry.Lang == Lang)
      Out.push_back(Entry.Spelling);
  return Out;
}

std::string spellingsText() {
  std::string Text;
  constexpr size_t Count = std::size(Spellings);
  for (size_t I = 0; I < Count; ++I) {
    if (I)
      Text += I + 1 == Count ? " or " : ", ";
    Text += Spellings[I].Id;
  }
  return Text;
}

std::string spellingsText(Language Lang) {
  const std::vector<BuiltinSpelling> Listed = spellingsOf(Lang);
  std::string Text;
  for (size_t I = 0; I < Listed.size(); ++I) {
    if (I)
      Text += I + 1 == Listed.size() ? " or " : ", ";
    Text += spellingId(Listed[I]).str();
  }
  return Text;
}

BuiltinSpelling spellingOf(const RenderOptions &Options) {
  return Options.Spelling && spellsBuiltins(Options.Lang, *Options.Spelling)
             ? *Options.Spelling
             : defaultSpelling(Options.Lang);
}

bool rendersPrototypes(Language Lang) {
  return Lang == Language::C || Lang == Language::Cpp;
}

bool readsDeclarations(Language Lang) {
  return Lang == Language::C || Lang == Language::Cpp ||
         Lang == Language::Rust || Lang == Language::Go;
}

llvm::StringRef languageDisplay(Language Lang) {
  for (const LanguageName &Entry : Languages)
    if (Entry.Lang == Lang)
      return Entry.Display;
  return "";
}

std::string languagesText() {
  std::string Text;
  constexpr size_t Count = std::size(Languages);
  for (size_t I = 0; I < Count; ++I) {
    if (I)
      Text += I + 1 == Count ? " or " : ", ";
    Text += Languages[I].Id;
  }
  return Text;
}

llvm::StringRef languageId(Language Lang) {
  for (const LanguageName &Entry : Languages)
    if (Entry.Lang == Lang)
      return Entry.Id;
  return "";
}

std::optional<Language> languageNamed(llvm::StringRef Id) {
  for (const LanguageName &Entry : Languages)
    if (Id.equals_insensitive(Entry.Id))
      return Entry.Lang;
  return std::nullopt;
}

namespace render {

unsigned offsetDigits(uint64_t Size) {
  unsigned Digits = 1;
  for (uint64_t Value = Size; Value >= 16; Value >>= 4)
    ++Digits;
  return Digits;
}

std::string offsetText(uint64_t Byte, const RenderOptions &Options,
                       unsigned Digits) {
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  if (Options.HexOffsets)
    Out << "0x" << llvm::format_hex_no_prefix(Byte, Digits, /*Upper=*/true);
  else
    Out << '+' << Byte;
  return Text;
}

std::string bitText(uint64_t BitInUnit, uint32_t Width) {
  if (Width == 1)
    return "bit " + std::to_string(BitInUnit);
  return "bits " + std::to_string(BitInUnit) + ".." +
         std::to_string(BitInUnit + Width - 1);
}

std::string escapedName(Language Lang, llvm::StringRef Name) {
  if (!reserved(Lang, Name))
    return Name.str();
  // A raw identifier keeps a Rust name as it was, except for the few that
  // cannot be raw.
  if (Lang == Language::Rust && Name != "self" && Name != "Self" &&
      Name != "crate" && Name != "super" && Name != "_")
    return "r#" + Name.str();
  return Name.str() + "_";
}

/// Names Go declares in the universe scope, and the package local types
/// import, which a type of the same name would hide.
constexpr const char *GoPredeclared[] = {
    "any",    "append",     "bool",    "byte",      "cap",        "clear",
    "close",  "comparable", "complex", "complex64", "complex128", "copy",
    "delete", "error",      "false",   "float32",   "float64",    "imag",
    "int",    "int8",       "int16",   "int32",     "int64",      "iota",
    "len",    "make",       "max",     "min",       "new",        "nil",
    "panic",  "print",      "println", "real",      "recover",    "rune",
    "string", "true",       "uint",    "uint8",     "uint16",     "uint32",
    "uint64", "uintptr",    "unsafe",
};

/// What a Python module of local types imports, and the builtin its classes
/// decorate with, which a type of the same name would hide.
constexpr const char *PythonPredeclared[] = {"ctypes", "enum", "property"};

/// What a JavaScript module of local types reads from the global scope and
/// declares itself, which a type of the same name would hide.
constexpr const char *JavaScriptPredeclared[] = {
    "ArrayBuffer",
    "BigInt",
    "DataView",
    "Infinity",
    "NaN",
    "Number",
    "Object",
    "check",
    "RangeError",
    "Uint8Array",
    "index",
    "undefined",
    // The types TypeScript names itself, which the module's JSDoc spells: a
    // program's own `boolean`, as the Windows headers declare, would read
    // as TypeScript's.
    "any",
    "bigint",
    "boolean",
    "never",
    "number",
    "object",
    "string",
    "symbol",
    "unknown",
};

/// Rust's primitive types, which a type of the same name would hide.
constexpr const char *RustPrimitives[] = {
    "bool",  "char", "f32", "f64", "i8",  "i16", "i32",  "i64",   "i128",
    "isize", "str",  "u8",  "u16", "u32", "u64", "u128", "usize",
};

NameSpelling::NameSpelling(const TypeLibrary &Library, Language Lang,
                           bool CppToo)
    : Lang(Lang),
      Reserved(CppToo && Lang == Language::C ? Language::Cpp : Lang) {
  const std::vector<const LocalType *> All = Library.types();
  llvm::StringSet<> Taken;
  for (const LocalType *Type : All) {
    std::string Spelled = identifier(Type->Name, /*TypeName=*/true);
    if (!Taken.insert(Spelled).second) {
      unsigned Number = 2;
      while (!Taken.insert(Spelled + "_" + std::to_string(Number)).second)
        ++Number;
      Spelled += "_" + std::to_string(Number);
    }
    if (Spelled != Type->Name)
      Renamed.push_back({Type->Name, Spelled});
    Types[Type->Name] = std::move(Spelled);
  }
  for (const LocalType *Type : All) {
    if (Type->Enum)
      enumeration(*Type->Enum, type(Type->Name), Taken);
    if (Type->Record)
      inlineEnums(*Type->Record, type(Type->Name), Taken);
  }
}

std::string NameSpelling::identifier(llvm::StringRef Name,
                                     bool TypeName) const {
  std::string Text;
  for (char C : Name)
    Text += llvm::isAlnum(C) || C == '_' ? C : '_';
  if (Text.empty() || llvm::isDigit(Text.front()))
    Text.insert(Text.begin(), '_');
  if (TypeName && Lang == Language::Go)
    for (const char *Word : GoPredeclared)
      if (Text == Word)
        return Text + "_";
  if (TypeName && Lang == Language::Rust)
    for (const char *Word : RustPrimitives)
      if (Text == Word)
        return Text + "_";
  if (TypeName && Lang == Language::Python)
    for (const char *Word : PythonPredeclared)
      if (Text == Word)
        return Text + "_";
  if (TypeName && Lang == Language::JavaScript)
    for (const char *Word : JavaScriptPredeclared)
      if (Text == Word)
        return Text + "_";
  return escapedName(Reserved, Text);
}

void NameSpelling::enumeration(const EnumDef &Enum, llvm::StringRef Owner,
                               llvm::StringSet<> &Taken) {
  std::vector<std::string> &Spelled = Enumerators[&Enum];
  for (const Enumerator &Value : Enum.Values) {
    std::string Name = identifier(Value.Name, /*TypeName=*/true);
    if (!Taken.insert(Name).second) {
      Name = Owner.str() + "_" + Name;
      while (!Taken.insert(Name).second)
        Name += "_";
    }
    if (Name != Value.Name)
      Renamed.push_back({Owner.str() + "::" + Value.Name, Name});
    Spelled.push_back(std::move(Name));
  }
}

void NameSpelling::inlineEnums(const RecordDef &Record, llvm::StringRef Owner,
                               llvm::StringSet<> &Taken) {
  for (const Member &Field : Record.Members) {
    if (!Field.Type)
      continue;
    std::vector<uint64_t> Bounds;
    const TypeExpr &Element = arrayElement(*Field.Type, Bounds);
    if (Element.Kind == TypeExprKind::Enum && Element.Enum)
      enumeration(*Element.Enum, Owner, Taken);
    else if (Element.Kind == TypeExprKind::Record && Element.Record)
      inlineEnums(*Element.Record, Owner, Taken);
  }
}

std::string NameSpelling::type(llvm::StringRef Name) const {
  auto Found = Types.find(Name);
  // A name the library lacks, such as a declared-only reference.
  return Found != Types.end() ? Found->second
                              : identifier(Name, /*TypeName=*/true);
}

std::string NameSpelling::enumerator(const EnumDef &Enum, size_t Index) const {
  auto Found = Enumerators.find(&Enum);
  if (Found != Enumerators.end() && Index < Found->second.size())
    return Found->second[Index];
  return Index < Enum.Values.size()
             ? identifier(Enum.Values[Index].Name, /*TypeName=*/true)
             : std::string("_");
}

std::string NameSpelling::member(llvm::StringRef Name) const {
  return Name.empty() ? std::string() : identifier(Name, /*TypeName=*/false);
}

std::string withComment(std::string Code, llvm::StringRef Comment,
                        const RenderOptions &Options, llvm::StringRef Marker) {
  if (Comment.empty())
    return Code;
  const size_t Column =
      std::max<size_t>(Options.CommentColumn, Code.size() + 1);
  Code.append(Column - Code.size(), ' ');
  Code += Marker;
  Code += ' ';
  Code += Comment;
  return Code;
}

uint64_t alignTo(uint64_t Value, uint64_t Alignment) {
  if (Alignment <= 1)
    return Value;
  return (Value + Alignment - 1) / Alignment * Alignment;
}

SequentialLayout::Step SequentialLayout::place(uint64_t Byte, uint64_t Size,
                                               uint32_t Alignment) {
  if (Pack && Alignment > Pack)
    Alignment = Pack;
  Step Result;
  const uint64_t Natural = alignTo(End, Alignment);
  if (Byte > Natural)
    Result.Filler = Byte - End;
  else if (Byte == Natural)
    Result.Padding = Natural - End;
  End = std::max(End, Byte + Size);
  return Result;
}

SequentialLayout::Step SequentialLayout::finish(uint64_t Size,
                                                uint32_t Alignment) const {
  if (Pack && Alignment > Pack)
    Alignment = Pack;
  Step Result;
  const uint64_t Natural = alignTo(End, Alignment);
  if (Size > Natural)
    Result.Filler = Size - End;
  else if (Size > End)
    Result.Padding = Size - End;
  return Result;
}

namespace {

void recordValueDependencies(const RecordDef &Record,
                             std::vector<std::string> &Names) {
  for (const Member &Field : Record.Members)
    if (Field.Type)
      valueDependencies(*Field.Type, Names);
}

void recordMentions(const RecordDef &Record, std::vector<std::string> &Names) {
  for (const Member &Field : Record.Members)
    if (Field.Type)
      mentionedTypes(*Field.Type, Names);
}

void functionMentions(const FunctionDef &Function,
                      std::vector<std::string> &Names) {
  if (Function.Return)
    mentionedTypes(*Function.Return, Names);
  for (const Parameter &Param : Function.Params)
    if (Param.Type)
      mentionedTypes(*Param.Type, Names);
}

} // namespace

std::string stringLiteralBody(llvm::StringRef Text) {
  std::string Body;
  for (char C : Text) {
    if (C == '"' || C == '\\') {
      Body += '\\';
      Body += C;
    } else if (static_cast<unsigned char>(C) < 0x20 || C == 0x7f) {
      // Three octal digits end the escape, where hexadecimal ones would run
      // on into a following digit.
      const unsigned char Byte = static_cast<unsigned char>(C);
      Body += '\\';
      Body += char('0' + (Byte >> 6));
      Body += char('0' + ((Byte >> 3) & 7));
      Body += char('0' + (Byte & 7));
    } else {
      Body += C;
    }
  }
  return Body;
}

void valueDependencies(const TypeExpr &Type, std::vector<std::string> &Names) {
  switch (Type.Kind) {
  case TypeExprKind::Named:
    Names.push_back(Type.Name);
    return;
  case TypeExprKind::Pointer:
    if (Type.Element && Type.Element->Kind == TypeExprKind::Array)
      valueDependencies(*Type.Element, Names);
    return;
  case TypeExprKind::Array:
    if (Type.Element)
      valueDependencies(*Type.Element, Names);
    return;
  case TypeExprKind::Record:
    if (Type.Record)
      recordValueDependencies(*Type.Record, Names);
    return;
  default:
    return;
  }
}

void valueDependencies(const LocalType &Type, std::vector<std::string> &Names) {
  if (Type.Record)
    recordValueDependencies(*Type.Record, Names);
  if (Type.Target)
    valueDependencies(*Type.Target, Names);
}

void mentionedTypes(const TypeExpr &Type, std::vector<std::string> &Names) {
  switch (Type.Kind) {
  case TypeExprKind::Named:
    Names.push_back(Type.Name);
    return;
  case TypeExprKind::Pointer:
  case TypeExprKind::Array:
    if (Type.Element)
      mentionedTypes(*Type.Element, Names);
    return;
  case TypeExprKind::Record:
    if (Type.Record)
      recordMentions(*Type.Record, Names);
    return;
  case TypeExprKind::Function:
    if (Type.Function)
      functionMentions(*Type.Function, Names);
    return;
  default:
    return;
  }
}

void mentionedTypes(const LocalType &Type, std::vector<std::string> &Names) {
  if (Type.Record)
    recordMentions(*Type.Record, Names);
  if (Type.Target)
    mentionedTypes(*Type.Target, Names);
  if (Type.Function)
    functionMentions(*Type.Function, Names);
}

std::vector<const LocalType *>
neededTypes(const TypeLibrary &Library, llvm::ArrayRef<const LocalType *> Roots,
            bool ThroughPointers) {
  if (Roots.empty())
    return Library.types();
  llvm::StringSet<> Seen;
  std::vector<const LocalType *> Work(Roots.begin(), Roots.end());
  for (const LocalType *Root : Roots)
    Seen.insert(Root->Name);
  for (size_t I = 0; I < Work.size(); ++I) {
    std::vector<std::string> Names;
    if (ThroughPointers)
      mentionedTypes(*Work[I], Names);
    else
      valueDependencies(*Work[I], Names);
    for (const std::string &Name : Names)
      if (Seen.insert(Name).second)
        if (const LocalType *Found = Library.find(Name))
          Work.push_back(Found);
  }
  std::sort(Work.begin(), Work.end(),
            [](const LocalType *A, const LocalType *B) {
              return A->Ordinal < B->Ordinal;
            });
  return Work;
}

BitStorage bitStorage(const TypeLibrary &Library, const TypeExpr &Type) {
  switch (Type.Kind) {
  case TypeExprKind::Builtin: {
    const BuiltinInfo &Info = builtinInfo(Type.TheBuiltin);
    return {Library.builtinBytes(Type.TheBuiltin),
            Info.Signed && Info.Class != BuiltinClass::Bool};
  }
  case TypeExprKind::Enum:
    if (Type.Enum)
      return bitStorage(Library, *makeBuiltin(Type.Enum->Underlying));
    break;
  case TypeExprKind::Named:
    if (const LocalType *Named = Library.find(Type.Name)) {
      if (Named->Enum)
        return bitStorage(Library, *makeBuiltin(Named->Enum->Underlying));
      if (Named->Target)
        return bitStorage(Library, *Named->Target);
    }
    break;
  default:
    break;
  }
  return {static_cast<unsigned>(Library.sizeOf(Type).value_or(4)), false};
}

BitAccess bitAccess(const TypeLibrary &Library, const Member &Field) {
  const BitStorage Storage = bitStorage(Library, *Field.Type);
  BitAccess Access;
  Access.UnitBits = Access.ValueBits = Storage.Bytes * 8;
  Access.Shift = Field.BitOffset - Field.StorageBitOffset;
  Access.Width = Field.BitWidth;
  Access.Signed = Storage.Signed;
  const uint64_t Mask =
      Field.BitWidth >= 64 ? ~uint64_t(0) : (uint64_t(1) << Field.BitWidth) - 1;
  llvm::raw_string_ostream(Access.Mask)
      << llvm::format_hex(Mask, 0, /*Upper=*/true);
  if (Access.Mask.size() > 1)
    Access.Mask[1] = 'x';
  return Access;
}

BitAccess bitAccess(const TypeLibrary &Library, const Member &Field,
                    const Slot &Run) {
  BitAccess Access = bitAccess(Library, Field);
  if (!Run.StorageBytes)
    return Access;
  if (Run.StorageArray) {
    // The bytes of the array that hold the field, read as one integer.
    Access.ArrayByte = Field.BitOffset / 8 - Run.Byte;
    Access.ArrayBytes = unsigned((Field.BitOffset + Field.BitWidth + 7) / 8 -
                                 Field.BitOffset / 8);
    Access.Shift = Field.BitOffset % 8;
    Access.UnitBits = 64;
    return Access;
  }
  Access.Shift = Field.BitOffset - Run.Byte * 8;
  Access.UnitBits = unsigned(Run.StorageBytes * 8);
  return Access;
}

const TypeExpr &arrayElement(const TypeExpr &Type,
                             std::vector<uint64_t> &Bounds) {
  const TypeExpr *Element = &Type;
  while (Element->Kind == TypeExprKind::Array && Element->Element) {
    Bounds.push_back(Element->Count.value_or(0));
    Element = Element->Element.get();
  }
  return *Element;
}

RecordPlan planRecord(const TypeLibrary &Library, const RecordDef &Record,
                      std::string Name, uint32_t Pack,
                      const NameSpelling &Names) {
  RecordPlan Plan;
  Plan.Name = std::move(Name);
  Plan.Record = &Record;
  Plan.Pack = Pack;
  unsigned Anonymous = 0;
  for (const Member &Field : Record.Members)
    if (Field.isAnonymous())
      ++Anonymous;
  // The runs of bit-fields, each to the member after its last: those in one
  // storage unit after one another, and under GNU's rules those that share
  // a byte too, as GNU's compilers pack a field at the bit after the one
  // before it.
  const bool Gnu =
      !Record.Union && Library.layoutRules() == LayoutRules::Itanium;
  const auto firstByte = [](const Member &Bit) { return Bit.BitOffset / 8; };
  const auto endByte = [](const Member &Bit) {
    return (Bit.BitOffset + Bit.BitWidth + 7) / 8;
  };
  std::vector<size_t> RunEnd(Record.Members.size(), 0);
  unsigned Runs = 0;
  for (size_t I = 0; I < Record.Members.size(); ++I) {
    const Member &First = Record.Members[I];
    if (!First.isBitfield() || !First.Type)
      continue;
    uint64_t End = endByte(First);
    size_t Next = I + 1;
    for (; Next < Record.Members.size(); ++Next) {
      const Member &Bit = Record.Members[Next];
      if (!Bit.isBitfield() || !Bit.Type ||
          (Bit.StorageBitOffset != First.StorageBitOffset &&
           !(Gnu && firstByte(Bit) < End)))
        break;
      End = std::max(End, endByte(Bit));
    }
    RunEnd[I] = Next;
    ++Runs;
    I = Next - 1;
  }

  SequentialLayout Layout(Pack);
  unsigned AnonymousSeen = 0, InlineSeen = 0, RunsSeen = 0;
  // The alignment the places give the record, and whether a run of
  // bit-fields is stored otherwise than in the unit of its first's type.
  uint32_t Placed = 1;
  bool Narrowed = false;
  const auto place = [&](Slot Next, uint64_t Size, uint32_t Align) {
    Placed = std::max(Placed, Pack ? std::min(Align, Pack) : Align);
    if (!Record.Union) {
      const uint64_t At = Layout.end();
      const SequentialLayout::Step Step = Layout.place(Next.Byte, Size, Align);
      if (Step.Filler) {
        Slot Filler;
        Filler.TheKind = Slot::Kind::Filler;
        Filler.Byte = At;
        Filler.Size = Step.Filler;
        Filler.Name = "_filler" + std::to_string(Plan.Slots.size());
        Plan.Slots.push_back(std::move(Filler));
      }
      Next.PaddingBefore = Step.Padding;
    }
    Plan.Slots.push_back(std::move(Next));
  };
  for (size_t I = 0; I < Record.Members.size(); ++I) {
    const Member &Field = Record.Members[I];
    if (!Field.Type)
      continue;
    if (Field.isBitfield()) {
      Slot Run;
      Run.TheKind = Slot::Kind::Bitfields;
      uint64_t First = firstByte(Field), Last = endByte(Field);
      for (size_t Bit = I; Bit < RunEnd[I]; ++Bit) {
        Run.Bits.push_back(&Record.Members[Bit]);
        First = std::min(First, firstByte(Record.Members[Bit]));
        Last = std::max(Last, endByte(Record.Members[Bit]));
      }
      const size_t After = RunEnd[I];
      // The storage unit of the first field's type, where the record lays
      // it out.
      Run.Storage = Field.Type;
      Run.Byte = Field.StorageBitOffset / 8;
      Run.StorageBytes = Library.sizeOf(*Field.Type).value_or(0);
      if (Gnu) {
        // Where it holds the run's bits and none of the members around
        // them; else the smallest unsigned integer that does at its own
        // alignment; else the run's bytes.
        uint64_t Next = Record.Size;
        for (size_t Later = After; Later < Record.Members.size(); ++Later)
          if (Record.Members[Later].Type) {
            Next = firstByte(Record.Members[Later]);
            break;
          }
        const uint64_t Taken = Layout.end();
        const auto holds = [&](uint64_t At, uint64_t Bytes) {
          return At >= Taken && At <= First && At + Bytes >= Last &&
                 At + Bytes <= Next;
        };
        if (!holds(Run.Byte, Run.StorageBytes)) {
          Run.StorageBytes = 0;
          for (const Builtin Unsigned : {Builtin::UChar, Builtin::UShort,
                                         Builtin::UInt, Builtin::ULongLong}) {
            const uint64_t Bytes = Library.builtinBytes(Unsigned);
            if (holds(First / Bytes * Bytes, Bytes)) {
              Run.Storage = makeBuiltin(Unsigned);
              Run.Byte = First / Bytes * Bytes;
              Run.StorageBytes = Bytes;
              break;
            }
          }
          if (!Run.StorageBytes) {
            Run.Storage = makeArray(makeBuiltin(Builtin::UChar), Last - First);
            Run.Byte = First;
            Run.StorageBytes = Last - First;
            Run.StorageArray = true;
          }
          Narrowed = true;
        }
      }
      I = After - 1;
      ++RunsSeen;
      Run.Name =
          Runs == 1 ? "_bitfield" : "_bitfield" + std::to_string(RunsSeen);
      const uint64_t Bytes = Run.StorageBytes;
      const uint32_t Alignment = Library.alignOf(*Run.Storage);
      place(std::move(Run), Bytes, Alignment);
      continue;
    }
    Slot Place;
    Place.Field = &Field;
    Place.Byte = Field.BitOffset / 8;
    Place.Name = Names.member(Field.Name);
    const TypeExpr &Element = arrayElement(*Field.Type, Place.Bounds);
    if (Element.Kind == TypeExprKind::Record && Element.Record) {
      Place.NestedType = Plan.Name + "_" + std::to_string(InlineSeen++);
      if (Field.Name.empty()) {
        ++AnonymousSeen;
        Place.Name = Anonymous == 1
                         ? "Anonymous"
                         : "Anonymous" + std::to_string(AnonymousSeen);
      }
      const RecordDef &Inner = *Element.Record;
      Plan.Nested.push_back(planRecord(Library, Inner, Place.NestedType,
                                       Inner.Pack ? Inner.Pack : Pack, Names));
    } else if (Element.Kind == TypeExprKind::Enum && Element.Enum) {
      Place.NestedType = Plan.Name + "_" + std::to_string(InlineSeen++);
      Plan.Enums.push_back({Place.NestedType, Element.Enum.get()});
    } else {
      Place.Bounds.clear();
    }
    place(std::move(Place), Library.sizeOf(*Field.Type).value_or(0),
          Library.alignOf(*Field.Type));
  }
  if (!Record.Union) {
    const uint64_t End = Layout.end();
    // Where the places no longer align the record as its bit-fields' types
    // do, the language pads only to theirs; the rest is filled, and the
    // record's own alignment declared.
    uint32_t Alignment = Library.alignOf(Record);
    if (Narrowed && Placed < Alignment) {
      Plan.Align = Alignment;
      Alignment = Placed;
    }
    const SequentialLayout::Step Step = Layout.finish(Record.Size, Alignment);
    Plan.TailFiller = Step.Filler ? Record.Size - End : 0;
    Plan.TailPadding = Step.Padding;
  }
  return Plan;
}

} // namespace render

std::string renderType(const TypeLibrary &Library, const LocalType &Type,
                       const RenderOptions &Options) {
  switch (Options.Lang) {
  case Language::C:
  case Language::Cpp:
    return render::renderC(Library, Type, Options);
  case Language::Rust:
    return render::renderRust(Library, Type, Options);
  case Language::Go:
    return render::renderGo(Library, Type, Options);
  case Language::Python:
    return render::renderPython(Library, Type, Options);
  case Language::JavaScript:
    return render::renderJavaScript(Library, Type, Options);
  case Language::Lua:
    return render::renderLua(Library, Type, Options);
  }
  return std::string();
}

EmbeddedC renderEmbeddedC(const TypeLibrary &Library,
                          llvm::ArrayRef<const LocalType *> Roots,
                          const RenderOptions &Options) {
  RenderOptions C = Options;
  C.Lang = Language::C;
  return render::renderEmbeddedC(Library, Roots, C);
}

std::vector<std::pair<std::string, std::string>>
renamedNames(const TypeLibrary &Library, Language Lang) {
  return render::NameSpelling(Library, Lang, /*CppToo=*/true).renamed();
}

std::string spellType(const TypeLibrary &Library, const TypeExpr &Type,
                      const std::string &Declarator,
                      const RenderOptions &Options) {
  return TypeSpeller(Library, Options).spell(Type, Declarator);
}

TypeSpeller::TypeSpeller(const TypeLibrary &Library,
                         const RenderOptions &Options) {
  RenderOptions C = Options;
  if (C.Lang != Language::Cpp)
    C.Lang = Language::C;
  Speller = std::make_unique<render::CSpeller>(Library, C);
}

TypeSpeller::~TypeSpeller() = default;

std::string TypeSpeller::spell(const TypeExpr &Type,
                               const std::string &Declarator) const {
  return Speller->declare(Type, Declarator);
}

EmbeddedCRenderer::EmbeddedCRenderer(const TypeLibrary &Library,
                                     const RenderOptions &Options)
    : Library(Library), Options(Options) {
  this->Options.Lang = Language::C;
  Names = std::make_unique<render::NameSpelling>(Library, Language::C,
                                                 /*CppToo=*/false);
}

EmbeddedCRenderer::~EmbeddedCRenderer() = default;

EmbeddedC
EmbeddedCRenderer::render(llvm::ArrayRef<const LocalType *> Roots) const {
  return render::renderEmbeddedC(Library, Roots, Options, Names.get());
}

std::string renderTypes(const TypeLibrary &Library,
                        llvm::ArrayRef<const LocalType *> Types,
                        const RenderOptions &Options) {
  switch (Options.Lang) {
  case Language::C:
  case Language::Cpp:
    return render::renderCHeader(Library, Types, Options);
  case Language::Rust:
    return render::renderRustModule(Library, Types, Options);
  case Language::Go:
    return render::renderGoFile(Library, Types, Options);
  case Language::Python:
    return render::renderPythonModule(Library, Types, Options);
  case Language::JavaScript:
    return render::renderJavaScriptModule(Library, Types, Options);
  case Language::Lua:
    return render::renderLuaModule(Library, Types, Options);
  }
  return std::string();
}

EmbeddedSource renderEmbeddedSource(const TypeLibrary &Library,
                                    llvm::ArrayRef<const LocalType *> Defined,
                                    llvm::ArrayRef<const LocalType *> Declared,
                                    const RenderOptions &Options) {
  switch (Options.Lang) {
  case Language::Rust:
    return render::renderRustEmbedded(Library, Defined, Declared, Options);
  case Language::Go:
    return render::renderGoEmbedded(Library, Defined, Declared, Options);
  case Language::C:
  case Language::Cpp:
  case Language::Python:
  case Language::JavaScript:
  case Language::Lua:
    break;
  }
  return EmbeddedSource();
}

std::vector<std::string> valueDependencyNames(const LocalType &Type) {
  std::vector<std::string> Names;
  render::valueDependencies(Type, Names);
  llvm::sort(Names);
  Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
  return Names;
}

std::vector<std::string> mentionedTypeNames(const LocalType &Type) {
  std::vector<std::string> Names;
  render::mentionedTypes(Type, Names);
  llvm::sort(Names);
  Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
  return Names;
}

} // namespace neverd::types
