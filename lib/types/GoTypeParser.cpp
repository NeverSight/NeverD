//===- GoTypeParser.cpp - Local types from Go source ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Go has no unions, bitfields or packing, so the Go renderer writes them as
// storage read through methods, and this reads them back from those
// methods: a struct of raw storage whose methods return pointers to it is a
// union of what they return; a _bitfield field's comment lists its
// bitfields, and its getters say which read signed; a field of bytes named
// Name_ whose method Name returns a pointer to it holds that type; a method
// returning unsafe.Slice past the end is a flexible array.  A type's comment
// says the #pragma pack it was laid out under.  Records named
// <Parent>_<n> that their parent holds are in place, as in Rust.  The
// Windows headers' names of builtins, such as ULONG, are those builtins.
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

enum class GKind : uint8_t { End, Ident, Number, String, Punct, Comment };

struct GToken {
  GKind Kind = GKind::End;
  std::string Text;
  uint64_t Value = 0;
  unsigned Line = 1;
  unsigned Column = 1;
};

const char *const GoPunctuators[] = {
    "...", "<<", ">>", "&^", "==", "!=", "<=", ">=", "&&", "||", ":=",
    "(",   ")",  "[",  "]",  "{",  "}",  ",",  ";",  ".",  "=",  ":",
    "*",   "&",  "-",  "+",  "^",  "|",  "<",  ">",  "!",  "/",  "%"};

/// Go's tokens, with the semicolons Go inserts at line ends, and comments
/// kept: they carry offsets, bit lists and packing.
std::vector<GToken> lexGo(llvm::StringRef Source,
                          std::vector<SourceProblem> &Problems) {
  std::vector<GToken> Tokens;
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
  // Go ends a statement at a line's end after these.
  const auto endsStatement = [&] {
    for (auto It = Tokens.rbegin(); It != Tokens.rend(); ++It) {
      if (It->Kind == GKind::Comment)
        continue;
      if (It->Kind == GKind::Ident || It->Kind == GKind::Number ||
          It->Kind == GKind::String)
        return true;
      return It->Kind == GKind::Punct &&
             (It->Text == ")" || It->Text == "]" || It->Text == "}");
    }
    return false;
  };
  while (At < Source.size()) {
    const char C = Source[At];
    GToken T;
    T.Line = Line;
    T.Column = Column;
    if (C == '\n') {
      if (endsStatement()) {
        T.Kind = GKind::Punct;
        T.Text = ";";
        Tokens.push_back(T);
      }
      advance();
      continue;
    }
    if (llvm::isSpace(C)) {
      advance();
      continue;
    }
    llvm::StringRef Rest = Source.substr(At);
    if (Rest.starts_with("//")) {
      const size_t Start = At;
      while (At < Source.size() && Source[At] != '\n')
        advance();
      T.Kind = GKind::Comment;
      T.Text = Source.slice(Start + 2, At).trim().str();
      Tokens.push_back(std::move(T));
      continue;
    }
    if (Rest.starts_with("/*")) {
      while (At < Source.size() && !Source.substr(At).starts_with("*/"))
        advance();
      advance(2);
      continue;
    }
    if (llvm::isAlpha(C) || C == '_') {
      const size_t Start = At;
      while (At < Source.size() &&
             (llvm::isAlnum(Source[At]) || Source[At] == '_'))
        advance();
      T.Kind = GKind::Ident;
      T.Text = Source.slice(Start, At).str();
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
      unsigned Radix = 10;
      if (Text.starts_with_insensitive("0x"))
        Radix = 16, Text = Text.drop_front(2);
      else if (Text.starts_with_insensitive("0b"))
        Radix = 2, Text = Text.drop_front(2);
      else if (Text.starts_with_insensitive("0o"))
        Radix = 8, Text = Text.drop_front(2);
      else if (Text.size() > 1 && Text.front() == '0')
        Radix = 8;
      T.Kind = GKind::Number;
      T.Text = Source.slice(Start, At).str();
      if (Text.getAsInteger(Radix, T.Value))
        Problems.push_back(
            {T.Line, T.Column, "'" + T.Text + "' is no integer this reads"});
      Tokens.push_back(std::move(T));
      continue;
    }
    if (C == '"' || C == '`') {
      const char Quote = C;
      advance();
      std::string Text;
      while (At < Source.size() && Source[At] != Quote) {
        if (Quote == '"' && Source[At] == '\\')
          advance();
        if (At < Source.size())
          Text += Source[At];
        advance();
      }
      advance();
      T.Kind = GKind::String;
      T.Text = std::move(Text);
      Tokens.push_back(std::move(T));
      continue;
    }
    bool Matched = false;
    for (const char *Punct : GoPunctuators)
      if (Rest.starts_with(Punct)) {
        advance(strlen(Punct));
        T.Kind = GKind::Punct;
        T.Text = Punct;
        Tokens.push_back(std::move(T));
        Matched = true;
        break;
      }
    if (!Matched) {
      Problems.push_back(
          {Line, Column, std::string("'") + C + "' is no Go this reads"});
      advance();
    }
  }
  if (endsStatement()) {
    GToken Semi;
    Semi.Kind = GKind::Punct;
    Semi.Text = ";";
    Semi.Line = Line;
    Semi.Column = Column;
    Tokens.push_back(Semi);
  }
  GToken End;
  End.Line = Line;
  End.Column = Column;
  Tokens.push_back(End);
  return Tokens;
}

/// What a blank field lays out as until it goes: no identifier.
constexpr llvm::StringLiteral BlankField = "_ blank";

/// A Go type as written.
struct GType {
  enum Kind : uint8_t { Name, Pointer, Array, Slice, Function, Struct };
  Kind K = Name;
  std::string Text;
  std::shared_ptr<GType> Inner;
  uint64_t Count = 0;
  std::vector<std::pair<std::string, std::shared_ptr<GType>>> Params;
  bool Variadic = false;
  std::shared_ptr<GType> Return;
  unsigned Line = 0, Column = 0;
};
using GTypeRef = std::shared_ptr<GType>;

struct GField {
  std::string Name;
  GTypeRef Type;
  std::string Comment;
  unsigned Line = 0, Column = 0;
};

struct GStruct {
  std::string Name;
  std::string Doc;
  std::vector<GField> Fields;
  bool Opaque = false;
  unsigned Line = 0, Column = 0;
};

/// What a method returns and what its body says.
struct GMethod {
  std::string Receiver, Name;
  GTypeRef Return;
  /// Its body reads `&s.Field`: the field it reads.
  std::string AddressOf;
  /// Its body returns unsafe.Pointer of the receiver itself.
  bool Whole = false;
  /// Its body returns unsafe.Slice past the receiver: the offset.
  std::optional<uint64_t> SliceAt;
};

class GoReader {
public:
  GoReader(std::vector<GToken> Tokens, TypeLibrary &Library,
           const ParseOptions &Options, ParseResult &Result)
      : Tokens(std::move(Tokens)), Library(Library), Options(Options),
        Result(Result) {}

  void run() {
    while (peek().Kind != GKind::End && !Failed)
      declaration();
    if (!Failed)
      build();
  }

private:
  const GToken &peek(size_t Ahead = 0) const {
    size_t At = Pos;
    // Comments are read where they are asked for.
    for (size_t Seen = 0;; ++At) {
      if (At >= Tokens.size() - 1)
        return Tokens.back();
      if (Tokens[At].Kind == GKind::Comment)
        continue;
      if (Seen++ == Ahead)
        return Tokens[At];
    }
  }
  GToken take() {
    while (Pos < Tokens.size() - 1 && Tokens[Pos].Kind == GKind::Comment)
      ++Pos;
    GToken T = Tokens[Pos];
    if (Pos < Tokens.size() - 1)
      ++Pos;
    return T;
  }
  /// The comment at the current place, if a comment is next.
  std::string comment() {
    std::string Text;
    while (Pos < Tokens.size() - 1 && Tokens[Pos].Kind == GKind::Comment)
      Text = Tokens[Pos++].Text;
    return Text;
  }
  /// The comment on the line of the token before, as a field's trailing
  /// comment is.
  std::string trailingComment(unsigned Line) {
    if (Pos < Tokens.size() - 1 && Tokens[Pos].Kind == GKind::Comment &&
        Tokens[Pos].Line == Line)
      return Tokens[Pos++].Text;
    return std::string();
  }
  bool isPunct(llvm::StringRef P, size_t Ahead = 0) const {
    return peek(Ahead).Kind == GKind::Punct && peek(Ahead).Text == P;
  }
  bool isIdent(llvm::StringRef W, size_t Ahead = 0) const {
    return peek(Ahead).Kind == GKind::Ident && peek(Ahead).Text == W;
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
  void fail(const GToken &At, const std::string &Message) {
    fail(At.Line, At.Column, Message);
  }
  std::string ident(llvm::StringRef What) {
    if (peek().Kind != GKind::Ident) {
      fail(peek(), "expected " + What.str());
      return std::string();
    }
    return take().Text;
  }
  void skipGroup() {
    const std::string Open = take().Text;
    const std::string Close = Open == "{" ? "}" : Open == "(" ? ")" : "]";
    int Depth = 1;
    while (Depth && peek().Kind != GKind::End) {
      if (isPunct(Open))
        ++Depth;
      else if (isPunct(Close))
        --Depth;
      take();
    }
  }
  void skipStatement() {
    while (!accept(";") && peek().Kind != GKind::End) {
      if (isPunct("{") || isPunct("(") || isPunct("["))
        skipGroup();
      else
        take();
    }
  }

  void declaration() {
    const std::string Doc = comment();
    if (accept(";"))
      return;
    if (isIdent("package") || isIdent("import")) {
      take();
      if (isPunct("("))
        skipGroup();
      skipStatement();
      return;
    }
    if (isIdent("var")) {
      take();
      check();
      return;
    }
    if (isIdent("type")) {
      take();
      typeDeclaration(Doc);
      return;
    }
    if (isIdent("const")) {
      take();
      constants();
      return;
    }
    if (isIdent("func")) {
      method();
      return;
    }
    fail(peek(), peek().Kind == GKind::Ident
                     ? "'" + peek().Text + "' begins no declaration this reads"
                     : "expected a declaration");
  }

  /// var _ = [1]int{}[unsafe.Sizeof(T{})-N] and its Offsetof form.
  void check() {
    const GToken At = peek();
    std::string Function, Type, Field;
    std::optional<uint64_t> Value;
    while (!isPunct(";") && peek().Kind != GKind::End) {
      if (isIdent("Sizeof") || isIdent("Offsetof")) {
        Function = take().Text;
        expect("(", "after " + Function);
        Type = ident("a type");
        expect("{", "after the type");
        expect("}", "after the type");
        if (Function == "Offsetof") {
          expect(".", "before the field");
          Field = ident("a field");
        }
        expect(")", "after " + Function);
        if (accept("-") && peek().Kind == GKind::Number)
          Value = take().Value;
        continue;
      }
      take();
    }
    accept(";");
    if (!Function.empty() && Value)
      Checks.push_back(
          {Function == "Offsetof", Type, Field, *Value, At.Line, At.Column});
  }

  void typeDeclaration(const std::string &Doc) {
    if (accept("(")) {
      while (!Failed && !accept(")")) {
        const std::string Inner = comment();
        if (accept(";"))
          continue;
        typeSpec(Inner);
      }
      accept(";");
      return;
    }
    typeSpec(Doc);
  }

  void typeSpec(const std::string &Doc) {
    const GToken NameToken = peek();
    const std::string Name = ident("a type's name");
    const bool Alias = accept("=");
    if (isIdent("struct")) {
      take();
      GStruct Struct;
      Struct.Name = Name;
      Struct.Doc = Doc;
      Struct.Line = NameToken.Line;
      Struct.Column = NameToken.Column;
      if (!expect("{", "to open a struct"))
        return;
      while (!Failed && !accept("}")) {
        comment();
        if (accept(";"))
          continue;
        if (isPunct("}"))
          continue;
        GField Field;
        Field.Line = peek().Line;
        Field.Column = peek().Column;
        Field.Name = ident("a field's name");
        Field.Type = type();
        if (!Field.Type)
          return;
        Field.Comment = trailingComment(Field.Line);
        Struct.Fields.push_back(std::move(Field));
        if (!isPunct("}"))
          expect(";", "after a field");
      }
      Struct.Opaque = Struct.Fields.empty();
      accept(";");
      Structs.push_back(std::move(Struct));
      return;
    }
    GTypeRef Type = type();
    if (!Type)
      return;
    accept(";");
    Aliases.push_back({Name, Type, Alias, NameToken.Line, NameToken.Column});
  }

  void constants() {
    const bool Group = accept("(");
    do {
      comment();
      if (accept(";"))
        continue;
      if (Group && accept(")"))
        break;
      const std::string Name = ident("a constant's name");
      std::string TypeName;
      if (peek().Kind == GKind::Ident)
        TypeName = take().Text;
      if (!expect("=", "after a constant's name"))
        return;
      std::optional<int64_t> Value = integer();
      if (!Value)
        return;
      Constants.push_back({unescaped(Name), TypeName, *Value});
      ConstantValues[Name] = *Value;
      if (!Group)
        break;
    } while (!Failed);
    accept(";");
  }

  std::optional<int64_t> integer() {
    if (accept("-")) {
      std::optional<int64_t> V = integer();
      return V ? std::optional<int64_t>(int64_t(0 - uint64_t(*V))) : V;
    }
    if (accept("^")) {
      std::optional<int64_t> V = integer();
      return V ? std::optional<int64_t>(~*V) : V;
    }
    if (peek().Kind == GKind::Number)
      return int64_t(take().Value);
    if (peek().Kind == GKind::Ident) {
      auto Found = ConstantValues.find(peek().Text);
      if (Found != ConstantValues.end())
        return take(), Found->second;
    }
    fail(peek(), "expected an integer");
    return std::nullopt;
  }

  /// func (s *T) Name(params) Return { body }: what accessors say.
  void method() {
    take();
    if (!isPunct("(")) {
      // A function, not a method: no type of a record.
      while (!isPunct("{") && peek().Kind != GKind::End)
        take();
      if (isPunct("{"))
        skipGroup();
      accept(";");
      return;
    }
    take();
    GMethod M;
    ident("a receiver");
    accept("*");
    M.Receiver = ident("the receiver's type");
    if (!expect(")", "after the receiver"))
      return;
    M.Name = ident("a method's name");
    if (isPunct("("))
      skipGroup();
    if (!isPunct("{")) {
      M.Return = type();
      if (!M.Return)
        return;
    }
    if (!expect("{", "to open a method's body"))
      return;
    int Depth = 1;
    bool Slice = false;
    while (Depth && peek().Kind != GKind::End) {
      if (isPunct("{"))
        ++Depth;
      else if (isPunct("}"))
        --Depth;
      if (Depth == 0)
        break;
      if (isPunct("&") && peek(1).Kind == GKind::Ident && isPunct(".", 2) &&
          peek(3).Kind == GKind::Ident) {
        take();
        take();
        take();
        M.AddressOf = take().Text;
        continue;
      }
      if (isIdent("Slice"))
        Slice = true;
      if (isIdent("Add") && isPunct("(", 1)) {
        // unsafe.Add(unsafe.Pointer(s), N)
        while (!isPunct(",") && peek().Kind != GKind::End)
          take();
        take();
        if (peek().Kind == GKind::Number)
          M.SliceAt = take().Value;
        continue;
      }
      if (isIdent("Pointer") && isPunct("(", 1) &&
          peek(2).Kind == GKind::Ident && isPunct(")", 3))
        M.Whole = true;
      take();
    }
    take();
    accept(";");
    if (!Slice)
      M.SliceAt.reset();
    Methods.push_back(std::move(M));
  }

  GTypeRef type() {
    const GToken At = peek();
    auto T = std::make_shared<GType>();
    T->Line = At.Line;
    T->Column = At.Column;
    if (accept("*")) {
      T->K = GType::Pointer;
      T->Inner = type();
      return T->Inner ? T : nullptr;
    }
    if (accept("[")) {
      if (accept("]")) {
        T->K = GType::Slice;
        T->Inner = type();
        return T->Inner ? T : nullptr;
      }
      std::optional<int64_t> Count = integer();
      if (!Count || !expect("]", "after an array's length"))
        return nullptr;
      T->K = GType::Array;
      T->Count = uint64_t(*Count);
      T->Inner = type();
      return T->Inner ? T : nullptr;
    }
    if (isIdent("func")) {
      take();
      T->K = GType::Function;
      if (!expect("(", "after func"))
        return nullptr;
      while (!Failed && !accept(")")) {
        std::string Name;
        if (peek().Kind == GKind::Ident && !isPunct(",", 1) &&
            !isPunct(")", 1) && !isPunct(".", 1))
          Name = take().Text;
        if (accept("...")) {
          T->Variadic = true;
          type();
        } else {
          GTypeRef Param = type();
          if (!Param)
            return nullptr;
          T->Params.push_back({Name, Param});
        }
        if (!accept(",")) {
          expect(")", "after the parameters");
          break;
        }
      }
      if (!isPunct(";") && !isPunct(")") && !isPunct("{") && !isPunct(",") &&
          peek().Kind != GKind::Comment && peek().Kind != GKind::End) {
        T->Return = type();
        if (!T->Return)
          return nullptr;
      }
      return T;
    }
    if (isIdent("struct") && isPunct("{", 1) && isPunct("}", 2)) {
      take(), take(), take();
      T->K = GType::Struct;
      return T;
    }
    if (peek().Kind != GKind::Ident) {
      fail(At, "expected a type");
      return nullptr;
    }
    T->Text = take().Text;
    if (accept("."))
      T->Text += "." + ident("a qualified type's name");
    return T;
  }

  // Building.
  uint32_t pointerSize() const { return pointerBytes(Library.dataModel()); }

  std::optional<Builtin> primitive(llvm::StringRef Name) const {
    if (std::optional<Builtin> Windows = windowsTypedefNamed(Name))
      return Windows;
    const bool Wide = pointerSize() == 8;
    return llvm::StringSwitch<std::optional<Builtin>>(Name)
        .Cases({"uint8", "byte"}, Builtin::UChar)
        .Case("int8", Builtin::SChar)
        .Case("uint16", Builtin::UShort)
        .Case("int16", Builtin::Short)
        .Case("uint32", Builtin::UInt)
        .Cases({"int32", "rune"}, Builtin::Int)
        .Case("uint64", Builtin::ULongLong)
        .Case("int64", Builtin::LongLong)
        .Case("float32", Builtin::Float)
        .Case("float64", Builtin::Double)
        .Case("bool", Builtin::Bool)
        .Cases({"uint", "uintptr"}, Wide ? Builtin::ULongLong : Builtin::UInt)
        .Case("int", Wide ? Builtin::LongLong : Builtin::Int)
        .Default(std::nullopt);
  }

  /// The words Go reserves or predeclares take an underscore: type_.
  static std::string unescaped(std::string Name) {
    static const char *const Words[] = {
        "break",     "case",    "chan",    "const",       "continue",
        "default",   "defer",   "else",    "fallthrough", "for",
        "func",      "go",      "goto",    "if",          "import",
        "interface", "map",     "package", "range",       "return",
        "select",    "struct",  "switch",  "type",        "var",
        "any",       "append",  "bool",    "byte",        "cap",
        "clear",     "close",   "complex", "complex64",   "complex128",
        "copy",      "delete",  "error",   "false",       "float32",
        "float64",   "imag",    "int",     "int8",        "int16",
        "int32",     "int64",   "iota",    "len",         "make",
        "max",       "min",     "new",     "nil",         "panic",
        "print",     "println", "real",    "recover",     "rune",
        "string",    "true",    "uint",    "uint8",       "uint16",
        "uint32",    "uint64",  "uintptr", "unsafe"};
    if (Name.size() > 1 && Name.back() == '_')
      for (const char *Word : Words)
        if (llvm::StringRef(Name).drop_back() == Word) {
          Name.pop_back();
          break;
        }
    return Name;
  }

  /// A record in place held as Anonymous or AnonymousN is anonymous.
  static bool anonymous(llvm::StringRef Name, const TypeExpr &Type) {
    if (Type.Kind != TypeExprKind::Record)
      return false;
    return Name == "Anonymous" ||
           (Name.starts_with("Anonymous") &&
            llvm::all_of(Name.drop_front(9), llvm::isDigit));
  }

  static bool nestedName(llvm::StringRef Name, llvm::StringRef Parent) {
    if (!Name.consume_front(Parent) || !Name.consume_front("_") || Name.empty())
      return false;
    return llvm::all_of(Name, llvm::isDigit);
  }

  static const GType &element(const GType &Type,
                              std::vector<uint64_t> &Bounds) {
    const GType *T = &Type;
    while (T->K == GType::Array && T->Inner) {
      Bounds.push_back(T->Count);
      T = T->Inner.get();
    }
    return *T;
  }

  const GStruct *structNamed(llvm::StringRef Name) const {
    auto Found = StructIndex.find(Name);
    return Found == StructIndex.end() ? nullptr : &Structs[Found->second];
  }

  TypeExprRef convert(const GType &Type) {
    switch (Type.K) {
    case GType::Pointer: {
      TypeExprRef Inner = convert(*Type.Inner);
      return Inner ? makePointer(Inner, pointerSize()) : nullptr;
    }
    case GType::Array: {
      TypeExprRef Inner = convert(*Type.Inner);
      return Inner ? makeArray(Inner, Type.Count) : nullptr;
    }
    case GType::Slice:
      fail(Type.Line, Type.Column, "a slice is no local type");
      return nullptr;
    case GType::Function: {
      TypeExprRef Function = function(Type);
      return Function ? makePointer(Function, pointerSize()) : nullptr;
    }
    case GType::Struct:
      return makeBuiltin(Builtin::Void);
    case GType::Name:
      break;
    }
    if (Type.Text == "unsafe.Pointer")
      return makePointer(makeBuiltin(Builtin::Void), pointerSize());
    if (std::optional<Builtin> Primitive = primitive(Type.Text))
      return makeBuiltin(*Primitive);
    const std::string Name = unescaped(Type.Text);
    if (const GStruct *Struct = structNamed(Type.Text))
      return makeNamed(Name, Unions.contains(Struct->Name) ? TypeKind::Union
                                                           : TypeKind::Struct);
    if (EnumNames.contains(Type.Text))
      return makeNamed(Name, TypeKind::Enum);
    if (AliasNames.contains(Type.Text))
      return makeNamed(Name, FunctionAliases.contains(Type.Text)
                                 ? TypeKind::Function
                                 : TypeKind::Typedef);
    if (const LocalType *Known = Library.find(Name))
      return makeNamed(Name, Known->Kind);
    fail(Type.Line, Type.Column, "'" + Type.Text + "' names no type");
    return nullptr;
  }

  TypeExprRef function(const GType &Type) {
    auto Function = std::make_shared<FunctionDef>();
    for (const auto &[Name, Param] : Type.Params) {
      TypeExprRef Converted = convert(*Param);
      if (!Converted)
        return nullptr;
      Function->Params.push_back({unescaped(Name), Converted});
    }
    Function->Variadic = Type.Variadic;
    Function->Return =
        Type.Return ? convert(*Type.Return) : makeBuiltin(Builtin::Void);
    if (!Function->Return)
      return nullptr;
    return makeFunction(std::move(Function));
  }

  /// The number after \p Words in a type's comment: "packed to 4".
  static uint32_t number(llvm::StringRef Doc, llvm::StringRef Words) {
    const size_t At = Doc.find(Words);
    if (At == llvm::StringRef::npos)
      return 0;
    uint32_t Value = 0;
    Doc.substr(At + Words.size())
        .take_while(llvm::isDigit)
        .getAsInteger(10, Value);
    return Value;
  }

  /// "0x08 A bit 0, B bits 1..31": a run's fields by place.
  static std::optional<std::vector<std::tuple<std::string, uint32_t, uint32_t>>>
  bitList(llvm::StringRef Comment) {
    // The offset comes first.
    Comment = Comment.ltrim();
    if (Comment.starts_with("0x") || Comment.starts_with("+"))
      Comment = Comment.drop_until(llvm::isSpace).ltrim();
    std::vector<std::tuple<std::string, uint32_t, uint32_t>> Fields;
    llvm::SmallVector<llvm::StringRef, 8> Parts;
    Comment.split(Parts, ',', -1, /*KeepEmpty=*/false);
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

  const GMethod *methodOf(llvm::StringRef Receiver,
                          llvm::StringRef Name) const {
    for (const GMethod &M : Methods)
      if (M.Receiver == Receiver && M.Name == Name)
        return &M;
    return nullptr;
  }

  std::shared_ptr<const RecordDef> layOut(const GStruct &Struct) {
    if (auto Done = Laid.find(Struct.Name); Done != Laid.end())
      return Done->second;
    if (!Visiting.insert(Struct.Name).second) {
      fail(Struct.Line, Struct.Column,
           "'" + Struct.Name + "' holds itself by value");
      return nullptr;
    }
    DraftRecord Draft;
    Draft.Pack = number(Struct.Doc, "packed to ");
    Draft.Alignment = number(Struct.Doc, "aligned to ");
    Draft.Union = Unions.contains(Struct.Name);
    if (Draft.Union) {
      // The alternatives are what the accessors return.
      uint64_t Widest = 0;
      for (const GMethod &M : Methods) {
        if (M.Receiver != Struct.Name || !M.Whole || !M.Return ||
            M.Return->K != GType::Pointer)
          continue;
        TypeExprRef Type = member(Struct, *M.Return->Inner, M.Name, Draft);
        if (!Type)
          return nullptr;
        Draft.Members.push_back(
            {anonymous(M.Name, *Type) ? std::string() : unescaped(M.Name), Type,
             false, 0, ""});
        Widest = std::max(Widest, Library.sizeOf(*Type).value_or(0));
      }
      // Storage wider than every alternative keeps its size.
      const uint64_t Size = storageBytes(Struct);
      if (Size > Widest)
        Draft.Members.push_back({"_size",
                                 makeArray(makeBuiltin(Builtin::UChar), Size),
                                 false, 0, ""});
    } else {
      bool LastBitfields = false;
      for (const GField &Field : Struct.Fields) {
        if (Failed)
          return nullptr;
        // A run of bitfields: its storage and the comment's fields.
        if (llvm::StringRef(Field.Name).starts_with("_bitfield") &&
            Field.Type->K == GType::Name && primitive(Field.Type->Text)) {
          if (auto Bits = bitList(Field.Comment)) {
            const Builtin Storage = *primitive(Field.Type->Text);
            const unsigned Size = Library.builtinBytes(Storage);
            if (LastBitfields)
              Draft.Members.push_back({"", makeBuiltin(Storage), true, 0, ""});
            uint32_t Used = 0;
            llvm::sort(*Bits, [](const auto &A, const auto &B) {
              return std::get<1>(A) < std::get<1>(B);
            });
            for (const auto &[Name, First, Width] : *Bits) {
              if (First > Used)
                Draft.Members.push_back(
                    {"", makeBuiltin(Storage), true, First - Used, ""});
              Builtin Type = Storage;
              if (const GMethod *Getter = methodOf(Struct.Name, Name);
                  Getter && Getter->Return && Getter->Return->K == GType::Name)
                if (std::optional<Builtin> Read =
                        primitive(Getter->Return->Text);
                    Read && Library.builtinBytes(*Read) == Size)
                  Type = *Read;
              Draft.Members.push_back(
                  {unescaped(Name), makeBuiltin(Type), true, Width, ""});
              Used = First + Width;
            }
            LastBitfields = true;
            continue;
          }
        }
        LastBitfields = false;
        // Bytes a method reads as another type, where Go cannot align it.
        std::string Name = Field.Name;
        TypeExprRef Type;
        if (llvm::StringRef(Name).ends_with("_") &&
            Field.Type->K == GType::Array)
          if (const GMethod *Reader =
                  methodOf(Struct.Name, llvm::StringRef(Name).drop_back());
              Reader && Reader->AddressOf == Name && Reader->Return &&
              Reader->Return->K == GType::Pointer) {
            Type = member(Struct, *Reader->Return->Inner, Reader->Name, Draft);
            if (!Type)
              return nullptr;
            Name.pop_back();
          }
        if (!Type) {
          Type = member(Struct, *Field.Type, Field.Name, Draft);
          if (!Type)
            return nullptr;
        }
        // A blank field is padding: it places what follows, then goes.
        Draft.Members.push_back({Field.Name == "_" ? std::string(BlankField)
                                 : anonymous(Field.Name, *Type)
                                     ? std::string()
                                     : unescaped(Name),
                                 Type, false, 0, ""});
      }
      // A flexible array past the fields.
      for (const GMethod &M : Methods)
        if (M.Receiver == Struct.Name && M.SliceAt && M.Return &&
            M.Return->K == GType::Slice) {
          TypeExprRef Element = member(Struct, *M.Return->Inner, M.Name, Draft);
          if (!Element)
            return nullptr;
          Draft.Members.push_back(
              {unescaped(M.Name), makeArray(Element, 0), false, 0, ""});
        }
    }
    Visiting.erase(Struct.Name);
    llvm::Expected<RecordDef> Layout = layOutRecord(Library, Draft);
    if (!Layout) {
      fail(Struct.Line, Struct.Column, llvm::toString(Layout.takeError()));
      return nullptr;
    }
    llvm::erase_if(Layout->Members,
                   [](const Member &Laid) { return Laid.Name == BlankField; });
    auto Def = std::make_shared<RecordDef>(std::move(*Layout));
    Laid[Struct.Name] = Def;
    if (!Nested.contains(Struct.Name)) {
      LocalType Local;
      Local.Name = unescaped(Struct.Name);
      Local.Kind = Draft.Union ? TypeKind::Union : TypeKind::Struct;
      Local.Origin = Options.Origin;
      Local.Record = Def;
      Library.define(std::move(Local));
      note(Result.Types, unescaped(Struct.Name));
    }
    return Def;
  }

  /// The bytes of a union's storage: raw [N]uintK.
  uint64_t storageBytes(const GStruct &Struct) const {
    uint64_t Bytes = 0;
    for (const GField &Field : Struct.Fields)
      if (Field.Type->K == GType::Array && Field.Type->Inner->K == GType::Name)
        if (std::optional<Builtin> Unit = primitive(Field.Type->Inner->Text))
          Bytes += Field.Type->Count * Library.builtinBytes(*Unit);
    return Bytes;
  }

  /// A member's type: a record or an enumeration in place when its name is
  /// <Parent>_<n>, the type itself otherwise, records it holds laid out
  /// first.
  TypeExprRef member(const GStruct &Parent, const GType &Type,
                     llvm::StringRef Name, DraftRecord &Draft) {
    std::vector<uint64_t> Bounds;
    const GType &Element = element(Type, Bounds);
    if (Element.K == GType::Name && nestedName(Element.Text, Parent.Name)) {
      TypeExprRef Inner;
      if (const GStruct *Struct = structNamed(Element.Text)) {
        std::shared_ptr<const RecordDef> Def = layOut(*Struct);
        if (!Def)
          return nullptr;
        Inner = makeRecord(Def);
      } else if (auto Enum = InlineEnums.find(Element.Text);
                 Enum != InlineEnums.end()) {
        Inner = makeEnum(Enum->second);
      }
      if (Inner) {
        for (auto Bound = Bounds.rbegin(); Bound != Bounds.rend(); ++Bound)
          Inner = makeArray(Inner, *Bound);
        (void)Name;
        return Inner;
      }
    }
    if (Element.K == GType::Name && !Laid.count(Element.Text))
      if (const GStruct *Held = structNamed(Element.Text)) {
        layOut(*Held);
        if (Failed)
          return nullptr;
      }
    (void)Draft;
    return convert(Type);
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
    for (size_t I = 0; I < Structs.size(); ++I)
      StructIndex[Structs[I].Name] = I;
    // A type of a Windows name, as a file in Windows' names declares ULONG,
    // leaves the builtin it names, alias or not: one of another width or
    // sign would change every type that uses it.
    llvm::erase_if(Aliases, [&](const auto &Declared) {
      const auto &[Name, Type, Alias, Line, Column] = Declared;
      const std::optional<Builtin> Windows = windowsTypedefNamed(Name);
      if (!Windows)
        return false;
      const std::optional<Builtin> Named =
          Type->K == GType::Name ? primitive(Type->Text) : std::nullopt;
      if (!Named || !sameValues(*Named, *Windows))
        fail(Line, Column,
             "'" + Name + "' is the builtin of " +
                 std::to_string(Library.builtinBytes(*Windows)) +
                 " bytes that Windows names so; a type cannot make it "
                 "another");
      return true;
    });
    // A struct of raw storage read whole through its methods is a union.
    for (const GStruct &Struct : Structs) {
      if (Struct.Fields.size() != 1 ||
          !llvm::StringRef(Struct.Fields[0].Name).starts_with("raw"))
        continue;
      for (const GMethod &M : Methods)
        if (M.Receiver == Struct.Name && M.Whole)
          Unions.insert(Struct.Name);
    }
    for (const auto &[Name, Type, Alias, Line, Column] : Aliases) {
      if (Type->K == GType::Function)
        FunctionAliases.insert(Name);
      AliasNames.insert(Name);
    }
    // A defined integer type with constants of it is an enumeration.
    std::map<std::string, std::vector<std::pair<std::string, int64_t>>> Values;
    for (const auto &[Name, TypeName, Value] : Constants)
      Values[TypeName].push_back({Name, Value});
    for (const auto &[Name, Type, Alias, Line, Column] : Aliases) {
      auto Found = Values.find(Name);
      if (Alias || Found == Values.end() || Type->K != GType::Name ||
          !primitive(Type->Text))
        continue;
      auto Enum = std::make_shared<EnumDef>();
      Enum->Underlying = *primitive(Type->Text);
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
      bool Inline = false;
      for (const GStruct &Struct : Structs)
        if (nestedName(Name, Struct.Name))
          Inline = true;
      if (Inline) {
        InlineEnums[Name] = Enum;
        continue;
      }
      LocalType Local;
      Local.Name = unescaped(Name);
      Local.Kind = TypeKind::Enum;
      Local.Origin = Options.Origin;
      Local.Enum = Enum;
      Library.define(std::move(Local));
      note(Result.Types, Local.Name);
    }
    for (const GStruct &Struct : Structs) {
      for (const GField &Field : Struct.Fields) {
        std::vector<uint64_t> Bounds;
        const GType &Element = element(*Field.Type, Bounds);
        if (Element.K == GType::Name && nestedName(Element.Text, Struct.Name) &&
            structNamed(Element.Text))
          Nested.insert(Element.Text);
      }
      for (const GMethod &M : Methods)
        if (M.Receiver == Struct.Name && M.Return &&
            M.Return->K == GType::Pointer &&
            M.Return->Inner->K == GType::Name &&
            nestedName(M.Return->Inner->Text, Struct.Name) &&
            structNamed(M.Return->Inner->Text))
          Nested.insert(M.Return->Inner->Text);
    }
    // Records declared only.
    for (const GStruct &Struct : Structs) {
      if (!Struct.Opaque)
        continue;
      LocalType Declared;
      Declared.Name = unescaped(Struct.Name);
      Declared.Kind = TypeKind::Struct;
      Declared.Origin = Options.Origin;
      auto Def = std::make_shared<RecordDef>();
      Def->Defined = false;
      Declared.Record = std::move(Def);
      Library.define(std::move(Declared));
      note(Result.Types, unescaped(Struct.Name));
      Laid[Struct.Name] = nullptr;
    }
    for (const GStruct &Struct : Structs) {
      if (Failed)
        return;
      if (Nested.contains(Struct.Name) || Laid.count(Struct.Name))
        continue;
      layOut(Struct);
    }
    for (const auto &[Name, Type, Alias, Line, Column] : Aliases) {
      if (Failed || !AliasNames.contains(Name))
        continue;
      LocalType Local;
      Local.Name = unescaped(Name);
      Local.Origin = Options.Origin;
      if (Type->K == GType::Function) {
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
      note(Result.Types, Local.Name);
    }
    for (const auto &[Offset, Type, Field, Value, Line, Column] : Checks) {
      const LocalType *Checked = Library.find(unescaped(Type));
      if (!Checked || !Checked->Record || !Checked->Record->Defined)
        continue;
      if (!Offset) {
        if (Checked->Record->Size != Value)
          fail(Line, Column,
               "a size check fails: " + Type + " is " +
                   std::to_string(Checked->Record->Size) + " bytes");
        continue;
      }
      for (const Member &Laid : Checked->Record->Members)
        if (Laid.Name == unescaped(Field) && !Laid.isBitfield() &&
            Laid.BitOffset / 8 != Value)
          fail(Line, Column,
               "an offset check fails: " + Type + "." + Field + " is at " +
                   std::to_string(Laid.BitOffset / 8));
    }
  }

  std::vector<GToken> Tokens;
  size_t Pos = 0;
  TypeLibrary &Library;
  const ParseOptions &Options;
  ParseResult &Result;
  bool Failed = false;

  std::vector<GStruct> Structs;
  std::vector<std::tuple<std::string, GTypeRef, bool, unsigned, unsigned>>
      Aliases;
  std::vector<std::tuple<std::string, std::string, int64_t>> Constants;
  std::vector<GMethod> Methods;
  std::vector<
      std::tuple<bool, std::string, std::string, uint64_t, unsigned, unsigned>>
      Checks;

  llvm::StringMap<size_t> StructIndex;
  llvm::StringSet<> Unions, AliasNames, FunctionAliases, EnumNames, Nested,
      Visiting;
  llvm::StringMap<std::shared_ptr<const EnumDef>> InlineEnums;
  llvm::StringMap<int64_t> ConstantValues;
  llvm::StringMap<std::shared_ptr<const RecordDef>> Laid;
};

} // namespace

ParseResult parseGoTypes(llvm::StringRef Source, TypeLibrary &Library,
                         const ParseOptions &Options) {
  ParseResult Result;
  std::vector<GToken> Tokens = lexGo(Source, Result.Problems);
  if (!Result.Problems.empty())
    return Result;
  TypeLibrary::Trial Trial(Library);
  GoReader(std::move(Tokens), Library, Options, Result).run();
  if (Result.Problems.empty())
    Trial.keep();
  return Result;
}

} // namespace neverd::types
