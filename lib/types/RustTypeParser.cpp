//===- RustTypeParser.cpp - Local types from Rust source ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Reads #[repr(C)] structures and unions, type aliases, constants and
// extern "C" functions, as the Rust renderer writes them and as people
// write FFI types: an anonymous record is a type named <Parent>_<n> that a
// field of its parent holds, a run of bitfields is a _bitfield field whose
// doc comment says where each field is, and an integer alias with constants
// of its type is an enumeration.  The Windows headers' names of builtins,
// such as ULONG, are those builtins, as wdk-sys's aliases declare them.
// Everything is read before any record is laid out, since Rust names types
// before it defines them.
//
//===----------------------------------------------------------------------===//

#include "neverd/types/TypeParse.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"

#include <map>

namespace neverd::types {
namespace {

enum class RKind : uint8_t { End, Ident, Number, String, Punct, Doc };

struct RToken {
  RKind Kind = RKind::End;
  std::string Text;
  uint64_t Value = 0;
  unsigned Line = 1;
  unsigned Column = 1;
};

const char *const RustPunctuators[] = {
    "::", "->", "=>", "..", "#!", "#", "[", "]", "{", "}", "(",
    ")",  "<",  ">",  ",",  ";",  ":", "=", "*", "&", "!", "-",
    "+",  "|",  ".",  "?",  "/",  "%", "^", "~", "@", "'", "$"};

std::vector<RToken> lexRust(llvm::StringRef Source,
                            std::vector<SourceProblem> &Problems) {
  std::vector<RToken> Tokens;
  size_t At = 0;
  unsigned Line = 1, Column = 1;
  const auto advance = [&](size_t Count = 1) {
    for (size_t I = 0; I < Count && At < Source.size(); ++I) {
      if (Source[At++] == '\n') {
        ++Line;
        Column = 1;
      } else {
        ++Column;
      }
    }
  };
  while (At < Source.size()) {
    const char C = Source[At];
    if (llvm::isSpace(C)) {
      advance();
      continue;
    }
    RToken T;
    T.Line = Line;
    T.Column = Column;
    llvm::StringRef Rest = Source.substr(At);
    if (Rest.starts_with("//")) {
      const bool Doc = Rest.starts_with("///") && !Rest.starts_with("////");
      const size_t Start = At;
      while (At < Source.size() && Source[At] != '\n')
        advance();
      if (Doc) {
        T.Kind = RKind::Doc;
        T.Text = Source.slice(Start + 3, At).trim().str();
        Tokens.push_back(std::move(T));
      }
      continue;
    }
    if (Rest.starts_with("/*")) {
      unsigned Depth = 0;
      do {
        if (Source.substr(At).starts_with("/*")) {
          ++Depth;
          advance(2);
        } else if (Source.substr(At).starts_with("*/")) {
          --Depth;
          advance(2);
        } else {
          advance();
        }
      } while (Depth && At < Source.size());
      if (Depth)
        Problems.push_back({T.Line, T.Column, "a comment does not end"});
      continue;
    }
    if (llvm::isAlpha(C) || C == '_') {
      const size_t Start = At;
      // A raw identifier keeps its word.
      if (Rest.starts_with("r#") && Rest.size() > 2 &&
          (llvm::isAlpha(Rest[2]) || Rest[2] == '_'))
        advance(2);
      const size_t Word = At;
      while (At < Source.size() &&
             (llvm::isAlnum(Source[At]) || Source[At] == '_'))
        advance();
      T.Kind = RKind::Ident;
      T.Text = Source.slice(Word, At).str();
      (void)Start;
      Tokens.push_back(std::move(T));
      continue;
    }
    if (llvm::isDigit(C)) {
      const size_t Start = At;
      while (At < Source.size() &&
             (llvm::isAlnum(Source[At]) || Source[At] == '_'))
        advance();
      std::string Digits;
      for (char D : Source.slice(Start, At))
        if (D != '_')
          Digits += D;
      llvm::StringRef Text = Digits;
      // A type suffix: 0x10u32, 5i64.
      // Suffixes begin with u or i, which no digit is.
      for (const char *Suffix : {"usize", "isize", "u128", "i128", "u64", "i64",
                                 "u32", "i32", "u16", "i16", "u8", "i8"})
        if (Text.size() > strlen(Suffix) && Text.ends_with(Suffix)) {
          Text = Text.drop_back(strlen(Suffix));
          break;
        }
      unsigned Radix = 10;
      if (Text.starts_with_insensitive("0x"))
        Radix = 16, Text = Text.drop_front(2);
      else if (Text.starts_with_insensitive("0b"))
        Radix = 2, Text = Text.drop_front(2);
      else if (Text.starts_with_insensitive("0o"))
        Radix = 8, Text = Text.drop_front(2);
      T.Kind = RKind::Number;
      T.Text = Source.slice(Start, At).str();
      if (Text.getAsInteger(Radix, T.Value))
        Problems.push_back(
            {T.Line, T.Column, "'" + T.Text + "' is no integer this reads"});
      Tokens.push_back(std::move(T));
      continue;
    }
    if (C == '"') {
      advance();
      std::string Text;
      while (At < Source.size() && Source[At] != '"') {
        if (Source[At] == '\\')
          advance();
        if (At < Source.size())
          Text += Source[At];
        advance();
      }
      advance();
      T.Kind = RKind::String;
      T.Text = std::move(Text);
      Tokens.push_back(std::move(T));
      continue;
    }
    bool Matched = false;
    for (const char *Punct : RustPunctuators)
      if (Rest.starts_with(Punct)) {
        advance(strlen(Punct));
        T.Kind = RKind::Punct;
        T.Text = Punct;
        Tokens.push_back(std::move(T));
        Matched = true;
        break;
      }
    if (!Matched) {
      Problems.push_back(
          {Line, Column, std::string("'") + C + "' is no Rust this reads"});
      advance();
    }
  }
  RToken End;
  End.Line = Line;
  End.Column = Column;
  Tokens.push_back(End);
  return Tokens;
}

/// A Rust type as written.
struct RType {
  enum Kind : uint8_t {
    Path,
    Pointer,
    Reference,
    Array,
    Function,
    Option,
    Unit
  };
  Kind K = Path;
  std::string Name;
  bool Mutable = true;
  std::shared_ptr<RType> Inner;
  std::optional<uint64_t> Count;
  std::string Abi;
  std::vector<std::pair<std::string, std::shared_ptr<RType>>> Params;
  bool Variadic = false;
  std::shared_ptr<RType> Return;
  unsigned Line = 0, Column = 0;
};
using RTypeRef = std::shared_ptr<RType>;

struct RField {
  std::string Name;
  RTypeRef Type;
  std::string Doc;
  unsigned Line = 0, Column = 0;
};

struct RRecord {
  std::string Name;
  bool Union = false;
  uint32_t Pack = 0;
  uint32_t Alignment = 0;
  std::vector<RField> Fields;
  unsigned Line = 0, Column = 0;
};

struct RAssert {
  bool Offset = false;
  std::string Type, Field;
  int64_t Value = 0;
  unsigned Line = 0, Column = 0;
};

class RustReader {
public:
  RustReader(std::vector<RToken> Tokens, TypeLibrary &Library,
             const ParseOptions &Options, ParseResult &Result)
      : Tokens(std::move(Tokens)), Library(Library), Options(Options),
        Result(Result) {}

  void run() {
    while (peek().Kind != RKind::End && !Failed)
      item();
    if (!Failed)
      build();
  }

private:
  const RToken &peek(size_t Ahead = 0) const {
    return Tokens[std::min(Pos + Ahead, Tokens.size() - 1)];
  }
  RToken take() {
    RToken T = peek();
    if (Pos < Tokens.size() - 1)
      ++Pos;
    return T;
  }
  bool isPunct(llvm::StringRef P, size_t Ahead = 0) const {
    return peek(Ahead).Kind == RKind::Punct && peek(Ahead).Text == P;
  }
  bool isIdent(llvm::StringRef W, size_t Ahead = 0) const {
    return peek(Ahead).Kind == RKind::Ident && peek(Ahead).Text == W;
  }
  bool accept(llvm::StringRef P) {
    if (!isPunct(P))
      return false;
    take();
    return true;
  }
  bool expect(llvm::StringRef P, llvm::StringRef Where) {
    if (accept(P))
      return true;
    fail(peek(), "expected '" + P.str() + "' " + Where.str());
    return false;
  }
  void fail(unsigned Line, unsigned Column, const std::string &Message) {
    if (!Failed)
      Result.Problems.push_back({Line, Column, Message});
    Failed = true;
  }
  void fail(const RToken &At, const std::string &Message) {
    fail(At.Line, At.Column, Message);
  }
  /// Skips a bracketed group, the opening bracket next.
  void skipGroup() {
    const std::string Open = take().Text;
    const std::string Close = Open == "{" ? "}" : Open == "(" ? ")" : "]";
    int Depth = 1;
    while (Depth && peek().Kind != RKind::End) {
      if (isPunct(Open))
        ++Depth;
      else if (isPunct(Close))
        --Depth;
      take();
    }
  }
  /// The words no raw identifier can be take an underscore instead: Self_.
  static std::string unescaped(std::string Name) {
    for (const char *Word : {"self", "Self", "crate", "super"})
      if (Name.size() == strlen(Word) + 1 &&
          llvm::StringRef(Name).starts_with(Word) && Name.back() == '_')
        Name.pop_back();
    return Name;
  }

  std::string ident(llvm::StringRef What) {
    if (peek().Kind != RKind::Ident) {
      fail(peek(), "expected " + What.str());
      return std::string();
    }
    return take().Text;
  }

  // Items.
  struct Attributes {
    uint32_t Pack = 0;
    uint32_t Alignment = 0;
    bool Packed = false;
  };

  void attribute(Attributes &Attrs) {
    // # [ ... ]
    take();
    if (!expect("[", "after '#'"))
      return;
    if (isIdent("repr")) {
      take();
      if (!expect("(", "after repr"))
        return;
      while (!Failed && !accept(")")) {
        const RToken Word = take();
        if (Word.Text == "packed") {
          Attrs.Packed = true;
          Attrs.Pack = 1;
          if (accept("(")) {
            if (peek().Kind == RKind::Number)
              Attrs.Pack = uint32_t(take().Value);
            expect(")", "after packed's size");
          }
        } else if (Word.Text == "align") {
          if (expect("(", "after align") && peek().Kind == RKind::Number)
            Attrs.Alignment = uint32_t(take().Value);
          expect(")", "after the alignment");
        }
        accept(",");
      }
      expect("]", "after an attribute");
      return;
    }
    int Depth = 1;
    while (Depth && peek().Kind != RKind::End) {
      if (isPunct("["))
        ++Depth;
      else if (isPunct("]"))
        --Depth;
      take();
    }
  }

  void visibility() {
    if (isIdent("pub")) {
      take();
      if (isPunct("("))
        skipGroup();
    }
  }

  void item() {
    std::string Doc;
    Attributes Attrs;
    while (true) {
      if (peek().Kind == RKind::Doc) {
        Doc = take().Text;
        continue;
      }
      if (isPunct("#!")) {
        take();
        skipGroup();
        continue;
      }
      if (isPunct("#")) {
        attribute(Attrs);
        continue;
      }
      break;
    }
    visibility();
    if (accept(";"))
      return;
    const RToken At = peek();
    if (isIdent("use") || isIdent("mod") || isIdent("static")) {
      while (!accept(";") && peek().Kind != RKind::End) {
        if (isPunct("{"))
          skipGroup();
        else
          take();
      }
      return;
    }
    if (isIdent("struct") || isIdent("union")) {
      record(Attrs);
      return;
    }
    if (isIdent("type")) {
      take();
      const RToken NameToken = peek();
      const std::string Name = ident("an alias's name");
      if (!expect("=", "after the alias's name"))
        return;
      RTypeRef Type = type();
      if (!Type || !expect(";", "after an alias"))
        return;
      Aliases.push_back({Name, Type});
      AliasPlaces[Name] = {NameToken.Line, NameToken.Column};
      return;
    }
    if (isIdent("const")) {
      constant();
      return;
    }
    if (isIdent("impl")) {
      implementation();
      return;
    }
    if (isIdent("enum")) {
      enumeration();
      return;
    }
    if (isIdent("extern") || isIdent("unsafe")) {
      externBlock();
      return;
    }
    if (isIdent("fn")) {
      // A function with a body declares no type.
      while (!isPunct("{") && peek().Kind != RKind::End)
        take();
      if (isPunct("{"))
        skipGroup();
      return;
    }
    fail(At, At.Kind == RKind::Ident
                 ? "'" + At.Text + "' begins no item this reads"
                 : "expected an item");
  }

  void record(const Attributes &Attrs) {
    RRecord Record;
    Record.Union = take().Text == "union";
    Record.Line = peek().Line;
    Record.Column = peek().Column;
    Record.Name = ident("a record's name");
    Record.Pack = Attrs.Pack;
    Record.Alignment = Attrs.Alignment;
    if (accept(";")) {
      Records.push_back(std::move(Record));
      return;
    }
    if (!expect("{", "to open a record"))
      return;
    while (!Failed && !accept("}")) {
      RField Field;
      while (peek().Kind == RKind::Doc || isPunct("#")) {
        if (peek().Kind == RKind::Doc) {
          Field.Doc = take().Text;
          continue;
        }
        Attributes Ignored;
        attribute(Ignored);
      }
      if (accept("}"))
        break;
      visibility();
      Field.Line = peek().Line;
      Field.Column = peek().Column;
      Field.Name = unescaped(ident("a field's name"));
      if (!expect(":", "after a field's name"))
        return;
      Field.Type = type();
      if (!Field.Type)
        return;
      Record.Fields.push_back(std::move(Field));
      if (!accept(",")) {
        expect("}", "after a record's fields");
        break;
      }
    }
    Records.push_back(std::move(Record));
  }

  void constant() {
    const RToken At = take();
    if (isIdent("_")) {
      // const _: () = assert!(...);
      take();
      while (!isPunct("=") && peek().Kind != RKind::End)
        take();
      take();
      assertion();
      return;
    }
    const std::string Name = ident("a constant's name");
    if (!expect(":", "after a constant's name"))
      return;
    RTypeRef Type = type();
    if (!Type || !expect("=", "after a constant's type"))
      return;
    std::optional<int64_t> Value = integer();
    if (!Value || !expect(";", "after a constant"))
      return;
    if (Type->K == RType::Path)
      Constants.push_back({Name, Type->Name, *Value});
    (void)At;
  }

  /// assert!(core::mem::size_of::<T>() == N) and offset_of!(T, f) == N.
  void assertion() {
    const RToken At = peek();
    if (!isIdent("assert")) {
      while (!accept(";") && peek().Kind != RKind::End)
        take();
      return;
    }
    take();
    expect("!", "after assert");
    expect("(", "after assert!");
    RAssert Check;
    Check.Line = At.Line;
    Check.Column = At.Column;
    // A path ending in size_of or offset_of.
    std::string Last;
    while (peek().Kind == RKind::Ident || isPunct("::")) {
      if (peek().Kind == RKind::Ident)
        Last = peek().Text;
      take();
    }
    if (Last == "size_of" && accept("<")) {
      Check.Type = ident("a type");
      expect(">", "after size_of's type");
      expect("(", "after size_of");
      expect(")", "after size_of(");
    } else if (Last == "offset_of" && accept("!")) {
      Check.Offset = true;
      expect("(", "after offset_of!");
      Check.Type = ident("a type");
      expect(",", "after offset_of's type");
      Check.Field = ident("a field");
      expect(")", "after offset_of's field");
    } else {
      while (!accept(";") && peek().Kind != RKind::End)
        take();
      return;
    }
    expect("=", "in an assertion");
    expect("=", "in an assertion");
    std::optional<int64_t> Value = integer();
    if (!Value)
      return;
    Check.Value = *Value;
    expect(")", "after an assertion");
    expect(";", "after an assertion");
    Asserts.push_back(std::move(Check));
  }

  /// impl T { ... }: the type each accessor reads a bitfield as.
  void implementation() {
    take();
    const std::string Record = ident("a type");
    if (!expect("{", "to open an impl"))
      return;
    int Depth = 1;
    while (Depth && peek().Kind != RKind::End) {
      if (isIdent("fn") && peek(1).Kind == RKind::Ident && isPunct("(", 2)) {
        take();
        const std::string Method = take().Text;
        // (&self) -> T
        int Paren = 0;
        do {
          if (isPunct("("))
            ++Paren;
          else if (isPunct(")"))
            --Paren;
          take();
        } while (Paren && peek().Kind != RKind::End);
        if (accept("->"))
          if (RTypeRef Return = type())
            Accessors[Record + "::" + Method] = Return;
        continue;
      }
      if (isPunct("{"))
        ++Depth;
      else if (isPunct("}"))
        --Depth;
      take();
    }
  }

  /// A Rust enum with explicit discriminants and an integer repr.
  void enumeration() {
    take();
    const RToken NameToken = peek();
    const std::string Name = ident("an enumeration's name");
    if (!expect("{", "to open an enumeration"))
      return;
    int64_t Next = 0;
    while (!Failed && !accept("}")) {
      while (peek().Kind == RKind::Doc)
        take();
      const std::string Value = ident("a variant");
      if (accept("=")) {
        std::optional<int64_t> Given = integer();
        if (!Given)
          return;
        Next = *Given;
      }
      Constants.push_back({Value, Name, Next++});
      if (!accept(",")) {
        expect("}", "after the variants");
        break;
      }
    }
    // The variants are constants of an i32 alias, as C enumerations are.
    auto Alias = std::make_shared<RType>();
    Alias->Name = "i32";
    Aliases.push_back({Name, Alias});
    AliasPlaces[Name] = {NameToken.Line, NameToken.Column};
  }

  /// extern "C" { fn f(a: T) -> R; }: prototypes.
  void externBlock() {
    while (isIdent("unsafe") || isIdent("extern"))
      take();
    std::string Abi = "C";
    if (peek().Kind == RKind::String)
      Abi = take().Text;
    if (isIdent("fn")) {
      // An extern function with a body declares no type.
      while (!isPunct("{") && !isPunct(";") && peek().Kind != RKind::End)
        take();
      if (isPunct("{"))
        skipGroup();
      else
        accept(";");
      return;
    }
    if (!expect("{", "to open an extern block"))
      return;
    while (!Failed && !accept("}")) {
      while (peek().Kind == RKind::Doc || isPunct("#")) {
        if (peek().Kind == RKind::Doc)
          take();
        else {
          Attributes Ignored;
          attribute(Ignored);
        }
      }
      visibility();
      if (!isIdent("fn")) {
        while (!accept(";") && !isPunct("}") && peek().Kind != RKind::End)
          take();
        continue;
      }
      take();
      const std::string Name = ident("a function's name");
      RTypeRef Fn = signature(Abi);
      if (!Fn || !expect(";", "after a function"))
        return;
      Functions.push_back({Name, Fn});
    }
  }

  // Types.
  RTypeRef type() {
    const RToken At = peek();
    auto T = std::make_shared<RType>();
    T->Line = At.Line;
    T->Column = At.Column;
    if (accept("*")) {
      T->K = RType::Pointer;
      if (isIdent("mut"))
        take();
      else if (isIdent("const"))
        take(), T->Mutable = false;
      T->Inner = type();
      return T->Inner ? T : nullptr;
    }
    if (accept("&")) {
      T->K = RType::Reference;
      if (peek().Kind == RKind::Punct && peek().Text == "'")
        take(), take();
      if (isIdent("mut"))
        take();
      else
        T->Mutable = false;
      T->Inner = type();
      return T->Inner ? T : nullptr;
    }
    if (accept("[")) {
      T->K = RType::Array;
      T->Inner = type();
      if (!T->Inner || !expect(";", "in an array type"))
        return nullptr;
      std::optional<int64_t> Count = integer();
      if (!Count || !expect("]", "after an array's length"))
        return nullptr;
      T->Count = uint64_t(*Count);
      return T;
    }
    if (accept("(")) {
      expect(")", "for the unit type");
      T->K = RType::Unit;
      return T;
    }
    if (isIdent("unsafe") || isIdent("extern") || isIdent("fn")) {
      std::string Abi = "Rust";
      if (isIdent("unsafe"))
        take();
      if (isIdent("extern")) {
        take();
        Abi = "C";
        if (peek().Kind == RKind::String)
          Abi = take().Text;
      }
      if (!isIdent("fn")) {
        fail(peek(), "expected fn");
        return nullptr;
      }
      take();
      return signature(Abi, At);
    }
    if (peek().Kind != RKind::Ident) {
      fail(At, "expected a type");
      return nullptr;
    }
    // A path: its last segment names the type.
    T->Name = take().Text;
    while (accept("::"))
      T->Name = ident("a path's segment");
    if (T->Name == "Option" || T->Name == "NonNull") {
      const bool Option = T->Name == "Option";
      if (!expect("<", "after " + T->Name))
        return nullptr;
      RTypeRef Inner = type();
      if (!Inner || !expect(">", "after " + T->Name + "'s type"))
        return nullptr;
      T->K = Option ? RType::Option : RType::Pointer;
      T->Inner = Inner;
      return T;
    }
    if (isPunct("<")) {
      fail(peek(), "generic types are no local types");
      return nullptr;
    }
    return T;
  }

  /// (params) -> R after fn.
  RTypeRef signature(const std::string &Abi, const RToken &At = RToken()) {
    auto T = std::make_shared<RType>();
    T->K = RType::Function;
    T->Abi = Abi;
    T->Line = At.Line;
    T->Column = At.Column;
    if (!expect("(", "after fn"))
      return nullptr;
    while (!Failed && !accept(")")) {
      if (accept("...")) {
        T->Variadic = true;
        accept(",");
        continue;
      }
      if (isPunct(".") && isPunct(".", 1) && isPunct(".", 2)) {
        take(), take(), take();
        T->Variadic = true;
        accept(",");
        continue;
      }
      if (isPunct("..") && isPunct(".", 1)) {
        take(), take();
        T->Variadic = true;
        accept(",");
        continue;
      }
      std::string Name;
      if ((peek().Kind == RKind::Ident && isPunct(":", 1) &&
           !isPunct("::", 1)) ||
          (isIdent("_") && isPunct(":", 1))) {
        Name = take().Text;
        take();
        if (Name == "_")
          Name.clear();
      }
      RTypeRef Param = type();
      if (!Param)
        return nullptr;
      T->Params.push_back({Name, Param});
      if (!accept(",")) {
        expect(")", "after the parameters");
        break;
      }
    }
    if (accept("->")) {
      T->Return = type();
      if (!T->Return)
        return nullptr;
    }
    return T;
  }

  std::optional<int64_t> integer() {
    bool Negative = false;
    if (accept("-"))
      Negative = true;
    if (accept("!")) {
      std::optional<int64_t> V = integer();
      return V ? std::optional<int64_t>(~*V) : V;
    }
    if (peek().Kind == RKind::Number) {
      int64_t Value = int64_t(take().Value);
      // 0x10 as i32 and 5u32 keep their value.
      if (isIdent("as"))
        take(), take();
      return Negative ? int64_t(0 - uint64_t(Value)) : Value;
    }
    if (peek().Kind == RKind::Ident) {
      auto Found = ConstantValues.find(peek().Text);
      if (Found != ConstantValues.end())
        return take(), Found->second;
    }
    fail(peek(), "expected an integer");
    return std::nullopt;
  }

  // Building.
  uint32_t pointerSize() const { return pointerBytes(Library.dataModel()); }

  std::optional<Builtin> primitive(llvm::StringRef Name) const {
    if (std::optional<Builtin> Windows = windowsTypedefNamed(Name))
      return Windows;
    const bool Wide = pointerSize() == 8;
    return llvm::StringSwitch<std::optional<Builtin>>(Name)
        .Case("u8", Builtin::UChar)
        .Case("i8", Builtin::SChar)
        .Case("u16", Builtin::UShort)
        .Case("i16", Builtin::Short)
        .Case("u32", Builtin::UInt)
        .Case("i32", Builtin::Int)
        .Case("u64", Builtin::ULongLong)
        .Case("i64", Builtin::LongLong)
        .Case("u128", Builtin::UInt128)
        .Case("i128", Builtin::Int128)
        .Case("f32", Builtin::Float)
        .Case("f64", Builtin::Double)
        .Case("bool", Builtin::Bool)
        .Case("char", Builtin::Char32)
        .Case("usize", Wide ? Builtin::ULongLong : Builtin::UInt)
        .Case("isize", Wide ? Builtin::LongLong : Builtin::Int)
        .Case("c_char", Builtin::Char)
        .Case("c_schar", Builtin::SChar)
        .Case("c_uchar", Builtin::UChar)
        .Case("c_short", Builtin::Short)
        .Case("c_ushort", Builtin::UShort)
        .Case("c_int", Builtin::Int)
        .Case("c_uint", Builtin::UInt)
        .Case("c_long", Builtin::Long)
        .Case("c_ulong", Builtin::ULong)
        .Case("c_longlong", Builtin::LongLong)
        .Case("c_ulonglong", Builtin::ULongLong)
        .Case("c_float", Builtin::Float)
        .Case("c_double", Builtin::Double)
        .Case("c_void", Builtin::Void)
        .Default(std::nullopt);
  }

  /// Whether \p Name is <Parent>_<n>, a record or enumeration in place.
  static bool nestedName(llvm::StringRef Name, llvm::StringRef Parent) {
    if (!Name.consume_front(Parent) || !Name.consume_front("_") || Name.empty())
      return false;
    return llvm::all_of(Name, llvm::isDigit);
  }

  /// \p Type through arrays, with the bounds.
  static const RType &element(const RType &Type,
                              std::vector<uint64_t> &Bounds) {
    const RType *T = &Type;
    while (T->K == RType::Array && T->Inner) {
      Bounds.push_back(T->Count.value_or(0));
      T = T->Inner.get();
    }
    return *T;
  }

  TypeExprRef convert(const RType &Type) {
    switch (Type.K) {
    case RType::Unit:
      return makeBuiltin(Builtin::Void);
    case RType::Pointer:
    case RType::Reference: {
      TypeExprRef Inner = convert(*Type.Inner);
      if (!Inner)
        return nullptr;
      return makePointer(qualified(Inner, !Type.Mutable, false), pointerSize());
    }
    case RType::Array: {
      TypeExprRef Inner = convert(*Type.Inner);
      return Inner ? makeArray(Inner, Type.Count) : nullptr;
    }
    case RType::Function: {
      // A Rust fn is a pointer to the function.
      TypeExprRef Function = function(Type);
      return Function ? makePointer(Function, pointerSize()) : nullptr;
    }
    case RType::Option: {
      const RType &Inner = *Type.Inner;
      if (Inner.K == RType::Function)
        return convert(Inner);
      if (Inner.K == RType::Reference || Inner.K == RType::Pointer)
        return convert(Inner);
      if (Inner.K == RType::Path) {
        TypeExprRef Named = convert(Inner);
        if (Named && Named->Kind == TypeExprKind::Named)
          return makePointer(Named, pointerSize());
      }
      fail(Type.Line, Type.Column,
           "Option holds a function or a reference in a local type");
      return nullptr;
    }
    case RType::Path:
      break;
    }
    if (std::optional<Builtin> Primitive = primitive(Type.Name))
      return makeBuiltin(*Primitive);
    if (const RRecord *Record = recordNamed(Type.Name))
      return makeNamed(Type.Name,
                       Record->Union ? TypeKind::Union : TypeKind::Struct);
    if (EnumNames.contains(Type.Name))
      return makeNamed(Type.Name, TypeKind::Enum);
    if (AliasNames.contains(Type.Name))
      return makeNamed(Type.Name, FunctionAliases.contains(Type.Name)
                                      ? TypeKind::Function
                                      : TypeKind::Typedef);
    if (const LocalType *Known = Library.find(Type.Name))
      return makeNamed(Type.Name, Known->Kind);
    fail(Type.Line, Type.Column, "'" + Type.Name + "' names no type");
    return nullptr;
  }

  TypeExprRef function(const RType &Type) {
    auto Function = std::make_shared<FunctionDef>();
    if (Type.Abi == "stdcall")
      Function->Convention = CallingConvention::Stdcall;
    else if (Type.Abi == "cdecl")
      Function->Convention = CallingConvention::Cdecl;
    else if (Type.Abi == "fastcall")
      Function->Convention = CallingConvention::Fastcall;
    else if (Type.Abi == "thiscall")
      Function->Convention = CallingConvention::Thiscall;
    else if (Type.Abi == "vectorcall")
      Function->Convention = CallingConvention::Vectorcall;
    else if (Type.Abi == "win64")
      Function->Convention = CallingConvention::Win64;
    else if (Type.Abi == "sysv64")
      Function->Convention = CallingConvention::SysV;
    for (const auto &[Name, Param] : Type.Params) {
      TypeExprRef Converted = convert(*Param);
      if (!Converted)
        return nullptr;
      Function->Params.push_back({Name, Converted});
    }
    Function->Variadic = Type.Variadic;
    Function->Return =
        Type.Return ? convert(*Type.Return) : makeBuiltin(Builtin::Void);
    if (!Function->Return)
      return nullptr;
    return makeFunction(std::move(Function));
  }

  const RRecord *recordNamed(llvm::StringRef Name) const {
    auto Found = RecordIndex.find(Name);
    return Found == RecordIndex.end() ? nullptr : &Records[Found->second];
  }

  /// "A bit 0, B bits 1..31": each field of a run, by its place.
  static std::optional<std::vector<std::tuple<std::string, uint32_t, uint32_t>>>
  bitfieldDoc(llvm::StringRef Doc) {
    std::vector<std::tuple<std::string, uint32_t, uint32_t>> Fields;
    llvm::SmallVector<llvm::StringRef, 8> Parts;
    Doc.split(Parts, ',', -1, /*KeepEmpty=*/false);
    for (llvm::StringRef Part : Parts) {
      llvm::SmallVector<llvm::StringRef, 3> Words;
      Part.trim().split(Words, ' ', -1, /*KeepEmpty=*/false);
      if (Words.size() != 3 || (Words[1] != "bit" && Words[1] != "bits"))
        return std::nullopt;
      uint32_t First = 0, Last = 0;
      auto [From, To] = Words[2].split("..");
      if (From.getAsInteger(10, First))
        return std::nullopt;
      Last = First;
      if (!To.empty() && To.getAsInteger(10, Last))
        return std::nullopt;
      if (Last < First)
        return std::nullopt;
      Fields.emplace_back(Words[0].str(), First, Last - First + 1);
    }
    if (Fields.empty())
      return std::nullopt;
    return Fields;
  }

  /// The record \p Name laid out, its records in place laid out first.
  std::shared_ptr<const RecordDef> layOut(const RRecord &Record) {
    if (auto Done = Laid.find(Record.Name); Done != Laid.end())
      return Done->second;
    if (!Visiting.insert(Record.Name).second) {
      fail(Record.Line, Record.Column,
           "'" + Record.Name + "' holds itself by value");
      return nullptr;
    }
    DraftRecord Draft;
    Draft.Union = Record.Union;
    Draft.Pack = Record.Pack;
    Draft.Alignment = Record.Alignment;
    bool LastBitfields = false;
    for (const RField &Field : Record.Fields) {
      if (Failed)
        return nullptr;
      std::vector<uint64_t> Bounds;
      const RType &Element = element(*Field.Type, Bounds);
      // A run of bitfields: its storage and the doc comment's fields.
      if (llvm::StringRef(Field.Name).starts_with("_bitfield") &&
          Element.K == RType::Path && Bounds.empty() &&
          primitive(Element.Name)) {
        if (auto Bits = bitfieldDoc(Field.Doc)) {
          const Builtin Storage = *primitive(Element.Name);
          const unsigned Size = Library.builtinBytes(Storage);
          // A run after a run begins a unit of its own.
          if (LastBitfields)
            Draft.Members.push_back(
                {"", makeBuiltin(Storage), true, 0, std::string()});
          uint32_t Used = 0;
          llvm::sort(*Bits, [](const auto &A, const auto &B) {
            return std::get<1>(A) < std::get<1>(B);
          });
          for (const auto &[Name, First, Width] : *Bits) {
            if (First > Used)
              Draft.Members.push_back({"", makeBuiltin(Storage), true,
                                       First - Used, std::string()});
            // An accessor that reads the field signed says it is.
            Builtin Type = Storage;
            if (auto Found = Accessors.find(Record.Name + "::" + Name);
                Found != Accessors.end() && Found->second->K == RType::Path)
              if (std::optional<Builtin> Read = primitive(Found->second->Name);
                  Read && Library.builtinBytes(*Read) == Size)
                Type = *Read;
            Draft.Members.push_back(
                {Name, makeBuiltin(Type), true, Width, std::string()});
            Used = First + Width;
          }
          LastBitfields = true;
          continue;
        }
      }
      LastBitfields = false;
      // A record or an enumeration in place.
      if (Element.K == RType::Path && nestedName(Element.Name, Record.Name)) {
        const llvm::StringRef Name = Field.Name;
        const bool Anonymous =
            Name == "Anonymous" ||
            (Name.starts_with("Anonymous") &&
             llvm::all_of(Name.drop_front(9), llvm::isDigit));
        TypeExprRef Inner;
        if (const RRecord *Nested = recordNamed(Element.Name)) {
          std::shared_ptr<const RecordDef> Def = layOut(*Nested);
          if (!Def)
            return nullptr;
          Inner = makeRecord(Def);
        } else if (auto Enum = InlineEnums.find(Element.Name);
                   Enum != InlineEnums.end()) {
          Inner = makeEnum(Enum->second);
        }
        if (Inner) {
          for (auto Bound = Bounds.rbegin(); Bound != Bounds.rend(); ++Bound)
            Inner = makeArray(Inner, *Bound);
          Draft.Members.push_back(
              {Anonymous && Bounds.empty() ? std::string() : Field.Name, Inner,
               false, 0, std::string()});
          continue;
        }
      }
      // A record held by value is laid out first.
      if (Element.K == RType::Path && !Laid.count(Element.Name))
        if (const RRecord *Held = recordNamed(Element.Name)) {
          layOut(*Held);
          if (Failed)
            return nullptr;
        }
      TypeExprRef Type = convert(*Field.Type);
      if (!Type)
        return nullptr;
      Draft.Members.push_back({Field.Name, Type, false, 0, std::string()});
    }
    Visiting.erase(Record.Name);
    llvm::Expected<RecordDef> Layout = layOutRecord(Library, Draft);
    if (!Layout) {
      fail(Record.Line, Record.Column, llvm::toString(Layout.takeError()));
      return nullptr;
    }
    auto Def = std::make_shared<RecordDef>(std::move(*Layout));
    Laid[Record.Name] = Def;
    // A named record is a type of the library as soon as it is laid out,
    // for the records that hold it to measure.
    if (!Nested.contains(Record.Name)) {
      LocalType Local;
      Local.Name = Record.Name;
      Local.Kind = Record.Union ? TypeKind::Union : TypeKind::Struct;
      Local.Origin = Options.Origin;
      Local.Record = Def;
      Library.define(std::move(Local));
      note(Result.Types, Record.Name);
    }
    return Def;
  }

  void note(std::vector<std::string> &Names, const std::string &Name) {
    if (!llvm::is_contained(Names, Name))
      Names.push_back(Name);
  }

  /// Whether \p A and \p B hold the same values: their size, sign and
  /// whether they are floating.
  bool sameValues(Builtin A, Builtin B) const {
    const BuiltinInfo &First = builtinInfo(A), &Second = builtinInfo(B);
    return Library.builtinBytes(A) == Library.builtinBytes(B) &&
           First.Signed == Second.Signed &&
           (First.Class == BuiltinClass::Float) ==
               (Second.Class == BuiltinClass::Float);
  }

  void build() {
    for (size_t I = 0; I < Records.size(); ++I)
      RecordIndex[Records[I].Name] = I;
    // An alias of a Windows name, as a module in Windows' names declares
    // ULONG, leaves the builtin it names: one of another width or sign
    // would change every type that uses it.
    llvm::erase_if(Aliases, [&](const std::pair<std::string, RTypeRef> &Alias) {
      const std::optional<Builtin> Windows = windowsTypedefNamed(Alias.first);
      if (!Windows)
        return false;
      const RType &Target = *Alias.second;
      const std::optional<Builtin> Named =
          Target.K == RType::Path ? primitive(Target.Name) : std::nullopt;
      if (!Named || !sameValues(*Named, *Windows)) {
        const auto [Line, Column] = AliasPlaces.lookup(Alias.first);
        fail(Line, Column,
             "'" + Alias.first + "' is the builtin of " +
                 std::to_string(Library.builtinBytes(*Windows)) +
                 " bytes that Windows names so; an alias cannot make it "
                 "another");
      }
      return true;
    });
    for (const auto &[Name, Type] : Aliases) {
      AliasNames.insert(Name);
      if (Type->K == RType::Function)
        FunctionAliases.insert(Name);
    }
    // An integer alias with constants of its type is an enumeration.
    std::map<std::string, std::vector<std::pair<std::string, int64_t>>> Values;
    for (const auto &[Name, TypeName, Value] : Constants) {
      Values[TypeName].push_back({Name, Value});
      ConstantValues[Name] = Value;
    }
    for (const auto &[Name, Type] : Aliases) {
      auto Found = Values.find(Name);
      if (Found == Values.end() || Type->K != RType::Path ||
          !primitive(Type->Name))
        continue;
      auto Enum = std::make_shared<EnumDef>();
      Enum->Underlying = *primitive(Type->Name);
      const unsigned Bits = Library.builtinBytes(Enum->Underlying) * 8;
      for (const auto &[Value, Number] : Found->second) {
        uint64_t Raw = uint64_t(Number);
        if (Bits < 64) {
          Raw &= (uint64_t(1) << Bits) - 1;
          if (builtinInfo(Enum->Underlying).Signed && (Raw >> (Bits - 1)) & 1)
            Raw |= ~((uint64_t(1) << Bits) - 1);
        }
        Enum->Values.push_back({Value, Raw, std::string()});
      }
      EnumNames.insert(Name);
      AliasNames.erase(Name);
      // An enumeration named <Parent>_<n> is in place in its parent.
      bool Inline = false;
      for (const RRecord &Record : Records)
        if (nestedName(Name, Record.Name))
          Inline = true;
      if (Inline) {
        InlineEnums[Name] = Enum;
        continue;
      }
      LocalType Local;
      Local.Name = Name;
      Local.Kind = TypeKind::Enum;
      Local.Origin = Options.Origin;
      Local.Enum = Enum;
      Library.define(std::move(Local));
      note(Result.Types, Name);
    }
    // Records in place: held by a field of the record whose name begins
    // theirs.
    for (const RRecord &Record : Records)
      for (const RField &Field : Record.Fields) {
        std::vector<uint64_t> Bounds;
        const RType &Element = element(*Field.Type, Bounds);
        if (Element.K == RType::Path && nestedName(Element.Name, Record.Name) &&
            recordNamed(Element.Name))
          Nested.insert(Element.Name);
      }
    // Records only declared: an empty body of zero size means opaque.
    for (const RRecord &Record : Records) {
      const bool Opaque =
          Record.Fields.size() == 1 && Record.Fields[0].Name == "_opaque";
      if (!Record.Fields.empty() && !Opaque)
        continue;
      LocalType Declared;
      Declared.Name = Record.Name;
      Declared.Kind = Record.Union ? TypeKind::Union : TypeKind::Struct;
      Declared.Origin = Options.Origin;
      auto Def = std::make_shared<RecordDef>();
      Def->Union = Record.Union;
      Def->Defined = false;
      Declared.Record = std::move(Def);
      Library.define(std::move(Declared));
      note(Result.Types, Record.Name);
      Laid[Record.Name] = nullptr;
    }
    for (const RRecord &Record : Records) {
      if (Failed)
        return;
      if (Nested.contains(Record.Name) || Laid.count(Record.Name))
        continue;
      layOut(Record);
    }
    // Aliases, after the records they may name.
    for (const auto &[Name, Type] : Aliases) {
      if (Failed || !AliasNames.contains(Name))
        continue;
      LocalType Local;
      Local.Name = Name;
      Local.Origin = Options.Origin;
      if (Type->K == RType::Function) {
        TypeExprRef Function = function(*Type);
        if (!Function)
          return;
        Local.Kind = TypeKind::Function;
        Local.Function = Function->Function;
      } else {
        Local.Kind = TypeKind::Typedef;
        Local.Target = convert(*Type);
        if (!Local.Target)
          return;
      }
      Library.define(std::move(Local));
      note(Result.Types, Name);
    }
    for (const auto &[Name, Fn] : Functions) {
      TypeExprRef Function = function(*Fn);
      if (!Function)
        return;
      Library.definePrototype(Name, Function->Function);
      note(Result.Prototypes, Name);
    }
    for (const RAssert &Check : Asserts) {
      const LocalType *Type = Library.find(Check.Type);
      if (!Type || !Type->Record)
        continue;
      if (!Check.Offset) {
        if (int64_t(Type->Record->Size) != Check.Value)
          fail(Check.Line, Check.Column,
               "a size assertion fails: " + Check.Type + " is " +
                   std::to_string(Type->Record->Size) + " bytes");
        continue;
      }
      for (const Member &Field : Type->Record->Members)
        if (Field.Name == Check.Field && !Field.isBitfield() &&
            int64_t(Field.BitOffset / 8) != Check.Value)
          fail(Check.Line, Check.Column,
               "an offset assertion fails: " + Check.Type + "." + Check.Field +
                   " is at " + std::to_string(Field.BitOffset / 8));
    }
  }

  std::vector<RToken> Tokens;
  size_t Pos = 0;
  TypeLibrary &Library;
  const ParseOptions &Options;
  ParseResult &Result;
  bool Failed = false;

  std::vector<RRecord> Records;
  std::vector<std::pair<std::string, RTypeRef>> Aliases;
  llvm::StringMap<std::pair<unsigned, unsigned>> AliasPlaces;
  std::vector<std::tuple<std::string, std::string, int64_t>> Constants;
  std::vector<RAssert> Asserts;
  llvm::StringMap<RTypeRef> Accessors;
  std::vector<std::pair<std::string, RTypeRef>> Functions;

  llvm::StringMap<size_t> RecordIndex;
  llvm::StringSet<> AliasNames, FunctionAliases, EnumNames, Nested, Visiting;
  llvm::StringMap<std::shared_ptr<const EnumDef>> InlineEnums;
  llvm::StringMap<int64_t> ConstantValues;
  llvm::StringMap<std::shared_ptr<const RecordDef>> Laid;
};

} // namespace

ParseResult parseRustTypes(llvm::StringRef Source, TypeLibrary &Library,
                           const ParseOptions &Options) {
  ParseResult Result;
  std::vector<RToken> Tokens = lexRust(Source, Result.Problems);
  if (!Result.Problems.empty())
    return Result;
  TypeLibrary::Trial Trial(Library);
  RustReader(std::move(Tokens), Library, Options, Result).run();
  if (Result.Problems.empty())
    Trial.keep();
  return Result;
}

} // namespace neverd::types
