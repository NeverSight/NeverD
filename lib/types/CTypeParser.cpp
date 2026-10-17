//===- CTypeParser.cpp - Local types from C and C++ source ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A recursive descent reader of the declarations headers hold: no
// statements, no templates, no preprocessor beyond #pragma pack and macros
// that stand for words.  Records are laid out as they close, so a record
// held by value must be defined before, as C requires.  Everything reads
// into a copy of the library, which replaces it only when the whole source
// reads without a problem.
//
//===----------------------------------------------------------------------===//

#include "neverd/types/TypeParse.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"

#include <functional>

namespace neverd::types {
namespace {

enum class TokenKind : uint8_t { End, Word, Number, String, Punct, Pragma };

struct Token {
  TokenKind Kind = TokenKind::End;
  std::string Text;
  uint64_t Value = 0;
  /// A number with a point or an exponent, which no constant expression
  /// this reads takes.
  bool Floating = false;
  unsigned Line = 1;
  unsigned Column = 1;
};

/// What a #pragma pack asks, kept in a pragma token's value.
enum class PackAction : uint8_t { Set, Reset, Push, PushSet, Pop, PopSet };

uint64_t packToken(PackAction Action, uint64_t Value) {
  return (uint64_t(Action) << 32) | Value;
}

const char *const Punctuators[] = {
    "...", "::", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "->", "{",
    "}",   "(",  ")",  "[",  "]",  ";",  ",",  "*",  "&",  ":",  "=",  "<",
    ">",   "+",  "-",  "~",  "!",  "/",  "%",  "^",  "|",  "?",  ".",  "#"};

class Lexer {
public:
  Lexer(llvm::StringRef Source, std::vector<SourceProblem> &Problems)
      : Source(Source), Problems(Problems) {}

  std::vector<Token> run() {
    std::vector<Token> Tokens;
    bool LineStart = true;
    while (At < Source.size()) {
      const char C = Source[At];
      if (C == '\n') {
        advance();
        LineStart = true;
        continue;
      }
      if (llvm::isSpace(C)) {
        advance();
        continue;
      }
      if (Source.substr(At).starts_with("//")) {
        while (At < Source.size() && Source[At] != '\n')
          advance();
        continue;
      }
      if (Source.substr(At).starts_with("/*")) {
        const unsigned L = Line, Col = Column;
        advance(2);
        while (At < Source.size() && !Source.substr(At).starts_with("*/"))
          advance();
        if (At >= Source.size())
          problem(L, Col, "a comment does not end");
        advance(2);
        continue;
      }
      if (C == '#' && LineStart) {
        directive(Tokens);
        LineStart = true;
        continue;
      }
      LineStart = false;
      Token Next = token();
      if (Next.Kind == TokenKind::Word) {
        expand(Next, Tokens, 0);
        continue;
      }
      if (Next.Kind != TokenKind::End)
        Tokens.push_back(std::move(Next));
    }
    Token End;
    End.Line = Line;
    End.Column = Column;
    Tokens.push_back(End);
    return Tokens;
  }

private:
  void advance(size_t Count = 1) {
    for (size_t I = 0; I < Count && At < Source.size(); ++I) {
      if (Source[At++] == '\n') {
        ++Line;
        Column = 1;
      } else {
        ++Column;
      }
    }
  }

  void problem(unsigned L, unsigned Col, const std::string &Message) {
    Problems.push_back({L, Col, Message});
  }

  /// One token at the current place, comments and space skipped already.
  Token token() {
    Token Result;
    Result.Line = Line;
    Result.Column = Column;
    const char C = Source[At];
    if (llvm::isAlpha(C) || C == '_' || C == '$') {
      const size_t Start = At;
      while (At < Source.size() && (llvm::isAlnum(Source[At]) ||
                                    Source[At] == '_' || Source[At] == '$'))
        advance();
      Result.Kind = TokenKind::Word;
      Result.Text = Source.slice(Start, At).str();
      return Result;
    }
    if (llvm::isDigit(C) ||
        (C == '.' && At + 1 < Source.size() && llvm::isDigit(Source[At + 1]))) {
      const size_t Start = At;
      const bool Hex = Source.substr(At).starts_with_insensitive("0x");
      while (At < Source.size()) {
        const char D = Source[At];
        const char Before = llvm::toLower(Source[At - (At > Start)]);
        // An exponent's sign belongs to the number: 1e-5, 0x1p+3.
        const bool Sign =
            (D == '+' || D == '-') && At > Start && Before == (Hex ? 'p' : 'e');
        if (!llvm::isAlnum(D) && D != '.' && !Sign)
          break;
        advance();
      }
      llvm::StringRef Text = Source.slice(Start, At);
      if (Text.contains('.') ||
          (Hex ? Text.contains_insensitive("p")
               : Text.find_first_of("eE") != llvm::StringRef::npos)) {
        Result.Kind = TokenKind::Number;
        Result.Text = Text.str();
        Result.Floating = true;
        return Result;
      }
      // Drop the suffixes: u, l, ll, i64 and their mixes.
      llvm::StringRef Digits = Text;
      while (!Digits.empty()) {
        const char Last = llvm::toLower(Digits.back());
        if (Last == 'u' || Last == 'l')
          Digits = Digits.drop_back();
        else if (Digits.ends_with_insensitive("i64") ||
                 Digits.ends_with_insensitive("i32") ||
                 Digits.ends_with_insensitive("i16"))
          Digits = Digits.drop_back(3);
        else if (Digits.ends_with_insensitive("i8"))
          Digits = Digits.drop_back(2);
        else
          break;
      }
      unsigned Radix = 10;
      if (Digits.starts_with_insensitive("0x")) {
        Radix = 16;
        Digits = Digits.drop_front(2);
      } else if (Digits.starts_with_insensitive("0b")) {
        Radix = 2;
        Digits = Digits.drop_front(2);
      } else if (Digits.size() > 1 && Digits.front() == '0') {
        Radix = 8;
      }
      Result.Kind = TokenKind::Number;
      Result.Text = Text.str();
      if (Digits.getAsInteger(Radix, Result.Value))
        problem(Result.Line, Result.Column,
                "'" + Text.str() + "' is no integer this reads");
      return Result;
    }
    if (C == '"' || C == '\'') {
      const char Quote = C;
      advance();
      std::string Text;
      while (At < Source.size() && Source[At] != Quote && Source[At] != '\n') {
        if (Source[At] == '\\' && At + 1 < Source.size()) {
          advance();
          const char Escaped = Source[At];
          Text += Escaped == 'n' ? '\n' : Escaped == 't' ? '\t' : Escaped;
          advance();
          continue;
        }
        Text += Source[At];
        advance();
      }
      if (At >= Source.size() || Source[At] != Quote)
        problem(Result.Line, Result.Column, "a quoted text does not end");
      advance();
      if (Quote == '\'') {
        Result.Kind = TokenKind::Number;
        Result.Value = Text.empty() ? 0 : uint8_t(Text.front());
      } else {
        Result.Kind = TokenKind::String;
      }
      Result.Text = std::move(Text);
      return Result;
    }
    for (const char *Punct : Punctuators)
      if (Source.substr(At).starts_with(Punct)) {
        advance(strlen(Punct));
        Result.Kind = TokenKind::Punct;
        Result.Text = Punct;
        return Result;
      }
    problem(Line, Column,
            std::string("'") + C + "' cannot begin a declaration's word");
    advance();
    return Token();
  }

  /// A word, or the tokens of the macro it names.
  void expand(Token Word, std::vector<Token> &Tokens, unsigned Depth) {
    auto Found = Macros.find(Word.Text);
    if (Found == Macros.end() || Depth > 16) {
      Tokens.push_back(std::move(Word));
      return;
    }
    for (Token Replacement : Found->second) {
      Replacement.Line = Word.Line;
      Replacement.Column = Word.Column;
      if (Replacement.Kind == TokenKind::Word)
        expand(std::move(Replacement), Tokens, Depth + 1);
      else
        Tokens.push_back(std::move(Replacement));
    }
  }

  /// A preprocessor line: #pragma pack becomes a token, an object-like
  /// #define a macro, and anything else is skipped.
  void directive(std::vector<Token> &Tokens) {
    const unsigned L = Line, Col = Column;
    std::string Text;
    while (At < Source.size() && Source[At] != '\n') {
      if (Source[At] == '\\' && At + 1 < Source.size() &&
          Source[At + 1] == '\n') {
        advance(2);
        Text += ' ';
        continue;
      }
      Text += Source[At];
      advance();
    }
    llvm::StringRef Rest = llvm::StringRef(Text).drop_front().ltrim();
    const llvm::StringRef Name =
        Rest.take_while([](char C) { return llvm::isAlnum(C) || C == '_'; });
    Rest = Rest.drop_front(Name.size()).ltrim();
    if (Name == "pragma" && Rest.starts_with("pack")) {
      llvm::StringRef Arguments = Rest.drop_front(4).trim();
      if (!Arguments.consume_front("(") || !Arguments.consume_back(")")) {
        problem(L, Col, "#pragma pack takes its arguments in parentheses");
        return;
      }
      llvm::SmallVector<llvm::StringRef, 3> Parts;
      Arguments.split(Parts, ',', -1, /*KeepEmpty=*/false);
      PackAction Action = PackAction::Reset;
      uint64_t Value = 0;
      // The packing a part gives: a number, or a macro defined as one.
      const auto number = [&](llvm::StringRef Part) -> std::optional<uint64_t> {
        uint64_t Number = 0;
        if (!Part.getAsInteger(0, Number))
          return Number;
        auto Macro = Macros.find(Part);
        if (Macro != Macros.end() && Macro->second.size() == 1 &&
            Macro->second.front().Kind == TokenKind::Number &&
            !Macro->second.front().Floating)
          return Macro->second.front().Value;
        return std::nullopt;
      };
      bool HasNumber = false;
      for (llvm::StringRef Part : Parts)
        HasNumber |= number(Part.trim()).has_value();
      for (llvm::StringRef Part : Parts) {
        Part = Part.trim();
        if (Part == "push") {
          Action = PackAction::Push;
        } else if (Part == "pop") {
          Action = PackAction::Pop;
        } else if (Part == "show") {
          continue;
        } else if (std::optional<uint64_t> Number = number(Part)) {
          Value = *Number;
          Action = Action == PackAction::Push  ? PackAction::PushSet
                   : Action == PackAction::Pop ? PackAction::PopSet
                                               : PackAction::Set;
        } else if (Action == PackAction::Push && HasNumber) {
          // push, label, n: the label names the push, which this reads
          // without.
          continue;
        } else {
          // An identifier alone may be a label or a macro this source does
          // not define; the packing is not known either way.
          problem(L, Col,
                  "'" + Part.str() +
                      "' in #pragma pack is no packing this knows; define it "
                      "as a number");
          return;
        }
        if (Value && (Value > 16 || (Value & (Value - 1)))) {
          problem(L, Col, "#pragma pack takes 1, 2, 4, 8 or 16");
          return;
        }
      }
      if (Parts.empty())
        Action = PackAction::Reset;
      Token Pragma;
      Pragma.Kind = TokenKind::Pragma;
      Pragma.Text = "pack";
      Pragma.Value = packToken(Action, Value);
      Pragma.Line = L;
      Pragma.Column = Col;
      Tokens.push_back(std::move(Pragma));
      return;
    }
    if (Name == "define") {
      const llvm::StringRef Macro =
          Rest.take_while([](char C) { return llvm::isAlnum(C) || C == '_'; });
      const llvm::StringRef Body = Rest.drop_front(Macro.size());
      // A macro with parameters stands for no single word.
      if (Macro.empty() || Body.starts_with("("))
        return;
      std::vector<SourceProblem> Ignored;
      Lexer Inner(Body, Ignored);
      std::vector<Token> Replacement = Inner.run();
      Replacement.pop_back();
      Macros[Macro] = std::move(Replacement);
      return;
    }
    if (Name == "undef")
      Macros.erase(
          Rest.take_while([](char C) { return llvm::isAlnum(C) || C == '_'; }));
  }

  llvm::StringRef Source;
  std::vector<SourceProblem> &Problems;
  size_t At = 0;
  unsigned Line = 1;
  unsigned Column = 1;
  llvm::StringMap<std::vector<Token>> Macros;
};

/// The builtins a single word names, in every spelling the renderers write.
const llvm::StringMap<Builtin> &builtinWords() {
  static const llvm::StringMap<Builtin> Words = [] {
    llvm::StringMap<Builtin> Map;
    const auto add = [&](llvm::StringRef Word, Builtin Type) {
      if (!Word.contains(' '))
        Map.try_emplace(Word, Type);
    };
#define NEVERD_BUILTIN_TYPE(Name, Class, Signed, LLP64, LP64, ILP32, Keywords, \
                            Windows)                                           \
  add(Windows, Builtin::Name);
#include "neverd/types/BuiltinTypes.def"
    for (const auto &[Word, Type] :
         std::initializer_list<std::pair<const char *, Builtin>>{
             {"int8_t", Builtin::SChar},
             {"uint8_t", Builtin::UChar},
             {"int16_t", Builtin::Short},
             {"uint16_t", Builtin::UShort},
             {"int32_t", Builtin::Int},
             {"uint32_t", Builtin::UInt},
             {"int64_t", Builtin::LongLong},
             {"uint64_t", Builtin::ULongLong},
             {"HRESULT", Builtin::HResult},
             {"_Float16", Builtin::Half},
             {"char8_t", Builtin::Char8},
             {"char16_t", Builtin::Char16},
             {"char32_t", Builtin::Char32}})
      Map.try_emplace(Word, Type);
    return Map;
  }();
  return Words;
}

/// The words that make builtin types together: unsigned long long int.
bool builtinKeyword(llvm::StringRef Word) {
  return llvm::is_contained(
      std::initializer_list<llvm::StringRef>{
          "void",    "char",     "short",     "int",        "long",
          "signed",  "unsigned", "float",     "double",     "bool",
          "_Bool",   "wchar_t",  "__wchar_t", "__int8",     "__int16",
          "__int32", "__int64",  "__int128",  "__signed__", "__signed"},
      Word);
}

std::optional<CallingConvention> conventionWord(llvm::StringRef Word) {
  if (Word == "__cdecl" || Word == "_cdecl" || Word == "WINAPIV")
    return CallingConvention::Cdecl;
  if (Word == "__stdcall" || Word == "_stdcall" || Word == "NTAPI" ||
      Word == "WINAPI" || Word == "CALLBACK" || Word == "APIENTRY" ||
      Word == "STDMETHODCALLTYPE" || Word == "PASCAL")
    return CallingConvention::Stdcall;
  if (Word == "__fastcall" || Word == "_fastcall" || Word == "FASTCALL")
    return CallingConvention::Fastcall;
  if (Word == "__thiscall")
    return CallingConvention::Thiscall;
  if (Word == "__vectorcall")
    return CallingConvention::Vectorcall;
  return std::nullopt;
}

/// Words a declaration may hold that do not change its type.
bool ignoredWord(llvm::StringRef Word) {
  return llvm::is_contained(
      std::initializer_list<llvm::StringRef>{
          "extern",      "static",        "inline",        "__inline",
          "__inline__",  "__forceinline", "__extension__", "__w64",
          "register",    "auto",          "constexpr",     "virtual",
          "explicit",    "mutable",       "__unaligned",   "NTKERNELAPI",
          "NTSYSAPI",    "NTSYSCALLAPI",  "NTHALAPI",      "DECLSPEC_IMPORT",
          "FORCEINLINE", "__restrict",    "restrict",      "__restrict__",
          "_Atomic",     "thread_local",  "__ptr32",       "__ptr64",
          "__sptr",      "__uptr",        "_Nonnull",      "_Nullable",
          "__based"},
      Word);
}

/// Whether a target with 64-bit pointers keeps \p Convention.
bool keptWide(CallingConvention Convention) {
  switch (Convention) {
#define NEVERD_CALLING_CONVENTION(Name, C, Rust, IDA, Wide, Pops)              \
  case CallingConvention::Name:                                                \
    return Wide;
#include "neverd/types/CallingConventions.def"
  }
  return true;
}

/// Whether the callee of \p Convention pops its stack arguments.
bool calleePops(CallingConvention Convention) {
  switch (Convention) {
#define NEVERD_CALLING_CONVENTION(Name, C, Rust, IDA, Wide, Pops)              \
  case CallingConvention::Name:                                                \
    return Pops;
#include "neverd/types/CallingConventions.def"
  }
  return false;
}

/// const and volatile in GNU's spellings too.
bool constWord(llvm::StringRef Word) {
  return Word == "const" || Word == "__const" || Word == "__const__";
}
bool volatileWord(llvm::StringRef Word) {
  return Word == "volatile" || Word == "__volatile" || Word == "__volatile__";
}

/// An attribute's name without GNU's underscores: __aligned__ is aligned.
llvm::StringRef attributeName(llvm::StringRef Name) {
  if (Name.size() > 4 && Name.starts_with("__") && Name.ends_with("__"))
    return Name.drop_front(2).drop_back(2);
  return Name;
}

class Parser {
public:
  Parser(std::vector<Token> Tokens, TypeLibrary &Library,
         const ParseOptions &Options, ParseResult &Result)
      : Tokens(std::move(Tokens)), Library(Library), Options(Options),
        Result(Result), Cpp(Options.Lang == Language::Cpp) {}

  void run() {
    while (peek().Kind != TokenKind::End) {
      const size_t Start = Pos;
      Failed = false;
      topLevel();
      if (Failed)
        recover(Start);
    }
  }

private:
  // Tokens.
  const Token &peek(size_t Ahead = 0) const {
    return Tokens[std::min(Pos + Ahead, Tokens.size() - 1)];
  }
  Token take() {
    Token Taken = peek();
    if (Pos < Tokens.size() - 1)
      ++Pos;
    return Taken;
  }
  bool isPunct(llvm::StringRef Text, size_t Ahead = 0) const {
    return peek(Ahead).Kind == TokenKind::Punct && peek(Ahead).Text == Text;
  }
  bool isWord(llvm::StringRef Text, size_t Ahead = 0) const {
    return peek(Ahead).Kind == TokenKind::Word && peek(Ahead).Text == Text;
  }
  bool accept(llvm::StringRef Punct) {
    if (!isPunct(Punct))
      return false;
    take();
    return true;
  }
  bool expect(llvm::StringRef Punct, llvm::StringRef Where) {
    if (accept(Punct))
      return true;
    fail(peek(), "expected '" + Punct.str() + "' " + Where.str());
    return false;
  }
  void fail(const Token &At, const std::string &Message) {
    if (!Failed)
      Result.Problems.push_back({At.Line, At.Column, Message});
    Failed = true;
  }
  /// Skips past the declaration a problem was found in: to the semicolon
  /// that ends it, or the brace that closes the record it is in.
  void recover(size_t Start) {
    int Depth = 0;
    for (size_t I = Start; I < Pos; ++I)
      if (Tokens[I].Kind == TokenKind::Punct)
        Depth += Tokens[I].Text == "{" ? 1 : Tokens[I].Text == "}" ? -1 : 0;
    if (Pos == Start)
      take();
    while (peek().Kind != TokenKind::End) {
      if (isPunct("{")) {
        ++Depth;
      } else if (isPunct("}")) {
        take();
        // The declarators after a record's body belong to the declaration
        // too: } NAME, *PNAME;
        if (--Depth < 0)
          return;
        continue;
      } else if (isPunct(";") && Depth <= 0) {
        take();
        return;
      }
      take();
    }
  }

  // Names.
  bool isTypeName(llvm::StringRef Word) const {
    return builtinKeyword(Word) || builtinWords().count(Word) ||
           Library.find(Word) || Word == "size_t" || Word == "ptrdiff_t" ||
           Word == "intptr_t" || Word == "uintptr_t" || Word == "ssize_t" ||
           ((Word == "__builtin_va_list" || Word == "va_list") &&
            !Library.find(Word));
  }
  /// Whether the token at \p Ahead may begin a declaration's type.
  bool startsType(size_t Ahead = 0) const {
    const Token &T = peek(Ahead);
    if (T.Kind != TokenKind::Word)
      return false;
    if (startsAttribute(Ahead))
      return true;
    return isTypeName(T.Text) || T.Text == "struct" || T.Text == "union" ||
           T.Text == "enum" || T.Text == "class" || constWord(T.Text) ||
           T.Text == "_Noreturn" || T.Text == "DECLSPEC_NORETURN" ||
           volatileWord(T.Text) || T.Text == "typename" ||
           conventionWord(T.Text) || ignoredWord(T.Text) ||
           startsAttribute(Ahead);
  }

  uint32_t pointerSize() const { return pointerBytes(Library.dataModel()); }

  /// A word that cannot name what a declarator declares.
  static bool reservedWord(llvm::StringRef Word) {
    return builtinKeyword(Word) || conventionWord(Word) || ignoredWord(Word) ||
           Word == "struct" || Word == "union" || Word == "enum" ||
           Word == "class" || Word == "const" || Word == "volatile" ||
           Word == "typedef" || Word == "__declspec" ||
           Word == "__attribute__" || Word == "sizeof";
  }

  // The pack in force.
  uint32_t Pack = 0;
  std::vector<uint32_t> PackStack;
  void pragma(const Token &Pragma) {
    const auto Action = PackAction(Pragma.Value >> 32);
    const auto Value = uint32_t(Pragma.Value);
    switch (Action) {
    case PackAction::Set:
      Pack = Value;
      break;
    case PackAction::Reset:
      Pack = 0;
      break;
    case PackAction::Push:
      PackStack.push_back(Pack);
      break;
    case PackAction::PushSet:
      PackStack.push_back(Pack);
      Pack = Value;
      break;
    case PackAction::Pop:
      if (PackStack.empty()) {
        fail(Pragma, "#pragma pack(pop) has no push to return to");
        return;
      }
      Pack = PackStack.back();
      PackStack.pop_back();
      break;
    case PackAction::PopSet:
      if (PackStack.empty()) {
        fail(Pragma, "#pragma pack(pop) has no push to return to");
        return;
      }
      PackStack.pop_back();
      Pack = Value;
      break;
    }
  }

  // Declarations.
  /// What __attribute__, __declspec and the alignment specifiers say that
  /// a local type keeps.
  struct Attributes {
    uint32_t Alignment = 0;
    bool Packed = false;
    bool NoReturn = false;
  };

  struct Specifiers {
    TypeExprRef Type;
    bool Typedef = false;
    bool Const = false, Volatile = false;
    /// The alignment and packing asked before the type is complete: a
    /// record defined here takes them, a declaration of another type keeps
    /// them for what it declares.
    Attributes Asked;
    /// Whether the specifiers define the record or enumeration they name.
    bool Defines = false;
    bool NoReturn = false;
  };

  /// Notes \p Name in \p Names once, in source order: a system header's
  /// library declares thousands, so \p Seen keeps them for lookup.
  void note(std::vector<std::string> &Names, llvm::StringSet<> &Seen,
            const std::string &Name) {
    if (Seen.insert(Name).second)
      Names.push_back(Name);
  }

  void topLevel() {
    const Token &First = peek();
    if (First.Kind == TokenKind::Pragma) {
      pragma(take());
      return;
    }
    if (accept(";"))
      return;
    // extern "C" { and namespace N { hold declarations as if outside.
    if (isWord("extern") && peek(1).Kind == TokenKind::String &&
        isPunct("{", 2)) {
      take();
      take();
      take();
      ++Blocks;
      return;
    }
    if (isWord("namespace") && (isPunct("{", 1) || isPunct("{", 2))) {
      take();
      if (!isPunct("{"))
        take();
      take();
      ++Blocks;
      return;
    }
    if (isPunct("}") && Blocks > 0) {
      take();
      --Blocks;
      return;
    }
    if (isWord("_Static_assert") || isWord("static_assert")) {
      staticAssert();
      return;
    }
    declaration();
  }

  void declaration() {
    Specifiers Spec;
    if (!specifiers(Spec))
      return;
    if (accept(";"))
      return;
    // typedef struct { ... } X, *PX: an anonymous record's typedefs, defined
    // together once all are read.
    const bool Anonymous =
        Spec.Typedef && (Spec.Type->Kind == TypeExprKind::Record ||
                         Spec.Type->Kind == TypeExprKind::Enum);
    std::vector<std::tuple<std::string, TypeExprRef, Token>> Typedefs;
    while (true) {
      std::string Name;
      Token NameToken = peek();
      TypeExprRef Type = declarator(Spec.Type, Name, NameToken, false);
      if (Failed)
        return;
      // Alignment asked of what the declaration declares, rather than of a
      // record its specifiers define.
      const uint32_t Alignment = std::max(
          Declared.Alignment, Spec.Defines ? 0u : Spec.Asked.Alignment);
      const bool Packed =
          Declared.Packed || (!Spec.Defines && Spec.Asked.Packed);
      if (Spec.Typedef) {
        // A typedef cannot change its type's layout here: the library's
        // typedefs name a type as it is.
        if (Packed || (Alignment && Alignment != Library.alignOf(*Type))) {
          fail(NameToken, "a typedef that aligns or packs its type is not a "
                          "local type");
          return;
        }
        if (Anonymous)
          Typedefs.emplace_back(Name, Type, NameToken);
        else
          defineTypedef(Name, Type, NameToken);
      } else if (Type->Kind == TypeExprKind::Function && Type->Function) {
        if (Spec.NoReturn || Declared.NoReturn) {
          auto Function = std::make_shared<FunctionDef>(*Type->Function);
          Function->NoReturn = true;
          Type = makeFunction(std::move(Function));
        }
        Library.definePrototype(Name, Type->Function);
        note(Result.Prototypes, NotedPrototypes, Name);
        // A definition's body holds no declarations this reads.
        if (isPunct("{")) {
          skipBody();
          return;
        }
      }
      // Initializers of variables are skipped.
      if (accept("=")) {
        int Depth = 0;
        while (peek().Kind != TokenKind::End &&
               (Depth > 0 || (!isPunct(",") && !isPunct(";")))) {
          if (isPunct("{") || isPunct("("))
            ++Depth;
          else if (isPunct("}") || isPunct(")"))
            --Depth;
          take();
        }
      }
      if (accept(","))
        continue;
      if (expect(";", "after a declaration") && Anonymous)
        defineAnonymousTypedefs(Spec.Type, Typedefs);
      return;
    }
  }

  /// Defines the typedefs of an anonymous record or enumeration: the first
  /// that names it as it is, then the others through that one, as C names
  /// the anonymous type only through it.
  void defineAnonymousTypedefs(
      const TypeExprRef &Type,
      const std::vector<std::tuple<std::string, TypeExprRef, Token>>
          &Typedefs) {
    const auto Naming = llvm::find_if(Typedefs, [&](const auto &Typedef) {
      return std::get<1>(Typedef).get() == Type.get();
    });
    if (Naming == Typedefs.end()) {
      for (const auto &[Name, Target, At] : Typedefs)
        defineTypedef(Name, Target, At);
      return;
    }
    // The record or enumeration takes the typedef's name, as a tag would
    // give it, unless the name is another type's.
    const std::string &Name = std::get<0>(*Naming);
    const TypeKind Kind = Type->Kind == TypeExprKind::Enum ? TypeKind::Enum
                          : Type->Record && Type->Record->Union
                              ? TypeKind::Union
                              : TypeKind::Struct;
    const LocalType *Existing = Library.find(Name);
    const bool Declared = Existing && Existing->Kind == Kind &&
                          ((Existing->Record && !Existing->Record->Defined) ||
                           (Existing->Enum && Existing->Enum->Values.empty()));
    TypeExprRef Named;
    if ((!Existing || Declared) && !builtinWords().count(Name) &&
        !builtinKeyword(Name)) {
      LocalType Local;
      Local.Name = Name;
      Local.Kind = Kind;
      Local.Origin = Options.Origin;
      if (Kind == TypeKind::Enum)
        Local.Enum = Type->Enum;
      else
        Local.Record = Type->Record;
      Library.define(std::move(Local));
      note(Result.Types, NotedTypes, Name);
      Named = makeNamed(Name, Kind);
    } else {
      defineTypedef(Name, Type, std::get<2>(*Naming));
      Named = makeNamed(Name, TypeKind::Typedef);
    }
    for (const auto &[Name, Target, At] : Typedefs)
      if (&std::get<0>(*Naming) != &Name)
        defineTypedef(Name, substituted(Target, Type.get(), Named), At);
  }

  /// \p Type with \p Old, wherever it stands, replaced by \p New.
  static TypeExprRef substituted(const TypeExprRef &Type, const TypeExpr *Old,
                                 const TypeExprRef &New) {
    if (!Type)
      return Type;
    if (Type.get() == Old)
      return New;
    switch (Type->Kind) {
    case TypeExprKind::Pointer:
    case TypeExprKind::Array: {
      TypeExprRef Element = substituted(Type->Element, Old, New);
      if (Element == Type->Element)
        return Type;
      auto Copy = std::make_shared<TypeExpr>(*Type);
      Copy->Element = std::move(Element);
      return Copy;
    }
    case TypeExprKind::Function: {
      if (!Type->Function)
        return Type;
      auto Function = std::make_shared<FunctionDef>(*Type->Function);
      Function->Return = substituted(Function->Return, Old, New);
      for (Parameter &Param : Function->Params)
        Param.Type = substituted(Param.Type, Old, New);
      return makeFunction(std::move(Function));
    }
    default:
      return Type;
    }
  }

  void skipBody() {
    int Depth = 0;
    do {
      if (isPunct("{"))
        ++Depth;
      else if (isPunct("}"))
        --Depth;
      take();
    } while (Depth > 0 && peek().Kind != TokenKind::End);
  }

  void defineTypedef(const std::string &Name, const TypeExprRef &Type,
                     const Token &At) {
    // The renderers' preludes name builtins; a builtin's name stays its own.
    if (builtinWords().count(Name) || builtinKeyword(Name))
      return;
    // typedef struct X X names the record by itself.
    if (Type->Kind == TypeExprKind::Named && Type->Name == Name)
      return;
    if (const LocalType *Existing = Library.find(Name);
        Existing && Existing->Kind != TypeKind::Typedef &&
        Existing->Kind != TypeKind::Function && Existing->Record &&
        Existing->Record->Defined) {
      fail(At, "'" + Name + "' already names a record");
      return;
    }
    LocalType Local;
    Local.Name = Name;
    Local.Origin = Options.Origin;
    if (Type->Kind == TypeExprKind::Function && Type->Function) {
      Local.Kind = TypeKind::Function;
      Local.Function = Type->Function;
    } else {
      Local.Kind = TypeKind::Typedef;
      Local.Target = Type;
    }
    Library.define(std::move(Local));
    note(Result.Types, NotedTypes, Name);
  }

  /// Reads a declaration's specifiers into \p Spec; false when none name a
  /// type.
  bool specifiers(Specifiers &Spec) {
    struct {
      int Signed = 0, Unsigned = 0, Short = 0, Long = 0, Int = 0, Char = 0;
      int Int8 = 0, Int16 = 0, Int32 = 0, Int64 = 0, Int128 = 0;
      int Void = 0, Bool = 0, Float = 0, Double = 0, WChar = 0;
    } Words;
    bool AnyWord = false;
    const Token Start = peek();
    while (!Failed) {
      const Token &T = peek();
      if (T.Kind == TokenKind::Pragma) {
        pragma(take());
        continue;
      }
      if (T.Kind != TokenKind::Word)
        break;
      const llvm::StringRef W = T.Text;
      if (W == "typedef") {
        take();
        Spec.Typedef = true;
      } else if (constWord(W)) {
        take();
        Spec.Const = true;
      } else if (volatileWord(W)) {
        take();
        Spec.Volatile = true;
      } else if (ignoredWord(W) || W == "typename") {
        take();
      } else if (W == "_Noreturn" || W == "DECLSPEC_NORETURN" ||
                 W == "noreturn") {
        take();
        Spec.NoReturn = true;
      } else if (conventionWord(W)) {
        PendingConvention = conventionWord(take().Text);
      } else if (startsAttribute()) {
        Attributes Read;
        if (!attributes(Read))
          return false;
        Spec.Asked.Alignment = std::max(Spec.Asked.Alignment, Read.Alignment);
        Spec.Asked.Packed |= Read.Packed;
        Spec.NoReturn |= Read.NoReturn;
      } else if (W == "struct" || W == "union" || W == "class") {
        if (Spec.Type)
          break;
        take();
        Spec.Type = record(W == "union", Spec.Asked, Spec.Defines);
      } else if (W == "enum") {
        if (Spec.Type)
          break;
        take();
        Spec.Type = enumeration(Spec.Defines);
      } else if (builtinKeyword(W)) {
        if (Spec.Type)
          break;
        take();
        AnyWord = true;
        if (W == "signed" || W == "__signed" || W == "__signed__")
          ++Words.Signed;
        else if (W == "unsigned")
          ++Words.Unsigned;
        else if (W == "short")
          ++Words.Short;
        else if (W == "long")
          ++Words.Long;
        else if (W == "int")
          ++Words.Int;
        else if (W == "char")
          ++Words.Char;
        else if (W == "__int8")
          ++Words.Int8;
        else if (W == "__int16")
          ++Words.Int16;
        else if (W == "__int32")
          ++Words.Int32;
        else if (W == "__int64")
          ++Words.Int64;
        else if (W == "__int128")
          ++Words.Int128;
        else if (W == "void")
          ++Words.Void;
        else if (W == "bool" || W == "_Bool")
          ++Words.Bool;
        else if (W == "float")
          ++Words.Float;
        else if (W == "double")
          ++Words.Double;
        else
          ++Words.WChar;
      } else if (!Spec.Type && !AnyWord && isTypeName(W)) {
        take();
        if (auto Found = builtinWords().find(W); Found != builtinWords().end())
          Spec.Type = makeBuiltin(Found->second);
        else if ((W == "__builtin_va_list" || W == "va_list") &&
                 !Library.find(W)) {
          // Windows and i386 pass the variable arguments' address; other
          // targets a record of their own.
          if (Library.dataModel() == DataModel::LP64) {
            fail(T, "__builtin_va_list of an LP64 target is a record local "
                    "types do not know");
            return false;
          }
          Spec.Type = makePointer(makeBuiltin(Builtin::Char), pointerSize());
        } else if (W == "size_t" || W == "uintptr_t")
          Spec.Type = makeBuiltin(pointerSize() == 8 ? Builtin::ULongLong
                                                     : Builtin::UInt);
        else if (W == "ptrdiff_t" || W == "intptr_t" || W == "ssize_t")
          Spec.Type = makeBuiltin(pointerSize() == 8 ? Builtin::LongLong
                                                     : Builtin::Int);
        else
          Spec.Type = named(W);
      } else {
        break;
      }
    }
    if (Failed)
      return false;
    if (AnyWord) {
      Builtin Type = Builtin::Int;
      const bool U = Words.Unsigned > 0;
      if (Words.Void)
        Type = Builtin::Void;
      else if (Words.Bool)
        Type = Builtin::Bool;
      else if (Words.Float)
        Type = Builtin::Float;
      else if (Words.Double)
        Type = Words.Long ? Builtin::LongDouble : Builtin::Double;
      else if (Words.WChar)
        Type = Builtin::WChar;
      else if (Words.Char)
        Type = U              ? Builtin::UChar
               : Words.Signed ? Builtin::SChar
                              : Builtin::Char;
      else if (Words.Int8)
        Type = U ? Builtin::UChar : Builtin::SChar;
      else if (Words.Int16 || Words.Short)
        Type = U ? Builtin::UShort : Builtin::Short;
      else if (Words.Int32)
        Type = U ? Builtin::UInt : Builtin::Int;
      else if (Words.Int64 || Words.Long >= 2)
        Type = U ? Builtin::ULongLong : Builtin::LongLong;
      else if (Words.Int128)
        Type = U ? Builtin::UInt128 : Builtin::Int128;
      else if (Words.Long)
        Type = U ? Builtin::ULong : Builtin::Long;
      else
        Type = U ? Builtin::UInt : Builtin::Int;
      Spec.Type = makeBuiltin(Type);
    }
    if (!Spec.Type) {
      // The word that ended the specifiers is the one no type has.
      const Token &At = peek().Kind == TokenKind::Word ? peek() : Start;
      fail(At, At.Kind == TokenKind::Word ? "'" + At.Text + "' names no type"
                                          : "expected a type");
      return false;
    }
    Spec.Type = qualified(Spec.Type, Spec.Const, Spec.Volatile);
    return true;
  }

  /// The named type \p Name refers to, with its kind.
  TypeExprRef named(llvm::StringRef Name) {
    const LocalType *Type = Library.find(Name);
    return makeNamed(Name.str(), Type ? Type->Kind : TypeKind::Typedef);
  }

  /// Whether an attribute, an alignment specifier or an assembler name
  /// begins at \p Ahead.
  bool startsAttribute(size_t Ahead = 0) const {
    const Token &T = peek(Ahead);
    if (isPunct("[", Ahead) && isPunct("[", Ahead + 1))
      return true;
    return T.Kind == TokenKind::Word &&
           (T.Text == "__attribute__" || T.Text == "__attribute" ||
            T.Text == "__declspec" || T.Text == "DECLSPEC_ALIGN" ||
            T.Text == "alignas" || T.Text == "_Alignas" ||
            T.Text == "__asm__" || T.Text == "__asm");
  }

  /// Reads the attributes, alignment specifiers and assembler names that
  /// follow into \p Read: __attribute__((...)), __declspec(...),
  /// DECLSPEC_ALIGN(n), alignas(n) and __asm__("name").  Alignment,
  /// packing, calling conventions and noreturn are kept; attributes that
  /// change a type into one local types do not hold (vector_size, mode)
  /// fail, as does aligned without its value.  False on a problem.
  bool attributes(Attributes &Read) {
    while (startsAttribute() && !Failed) {
      // [[noreturn]], [[nodiscard]]: C++ and C23 attributes.
      if (isPunct("[")) {
        const Token Open = take();
        take();
        int Depth = 0;
        while (!Failed && !(Depth == 0 && isPunct("]") && isPunct("]", 1))) {
          if (peek().Kind == TokenKind::End) {
            fail(Open, "an attribute list does not end");
            return false;
          }
          if (isPunct("(") || isPunct("["))
            ++Depth;
          else if (isPunct(")") || isPunct("]"))
            --Depth;
          if (peek().Kind == TokenKind::Word &&
              attributeName(peek().Text) == "noreturn")
            Read.NoReturn = true;
          take();
        }
        take();
        take();
        continue;
      }
      const Token At = take();
      const llvm::StringRef Word = At.Text;
      if (!expect("(", "after " + Word.str()))
        return false;
      if (Word == "__asm__" || Word == "__asm") {
        // The symbol's name in the object file says nothing of its type.
        while (!Failed && !accept(")")) {
          if (peek().Kind == TokenKind::End) {
            fail(At, "an assembler name does not end");
            return false;
          }
          take();
        }
        continue;
      }
      if (Word == "DECLSPEC_ALIGN" || Word == "alignas" || Word == "_Alignas") {
        std::optional<int64_t> Value = alignment(At);
        if (!Value || !expect(")", "after the alignment"))
          return false;
        Read.Alignment = std::max(Read.Alignment, uint32_t(*Value));
        continue;
      }
      // __attribute__((a, b(x))) lists its attributes in a second pair of
      // parentheses; __declspec(a b(x)) in its one.
      const bool Gnu = Word != "__declspec";
      if (Gnu && !expect("(", "after " + Word.str() + "("))
        return false;
      while (!Failed && !isPunct(")")) {
        if (accept(","))
          continue;
        if (peek().Kind != TokenKind::Word) {
          fail(peek(), "expected an attribute's name");
          return false;
        }
        const Token Name = take();
        const llvm::StringRef Attribute = attributeName(Name.Text);
        const bool Arguments = isPunct("(");
        if (Attribute == "aligned" || Attribute == "align") {
          if (!Arguments) {
            fail(Name, "'" + Name.Text +
                           "' without an alignment asks the target's largest, "
                           "which local types do not assume");
            return false;
          }
          take();
          std::optional<int64_t> Value = alignment(Name);
          if (!Value || !expect(")", "after the alignment"))
            return false;
          Read.Alignment = std::max(Read.Alignment, uint32_t(*Value));
          continue;
        }
        if (Attribute == "vector_size" || Attribute == "mode" ||
            Attribute == "ext_vector_type") {
          fail(Name,
               "'" + Name.Text + "' makes a type local types do not hold");
          return false;
        }
        if (Attribute == "packed")
          Read.Packed = true;
        else if (Attribute == "noreturn")
          Read.NoReturn = true;
        else if (std::optional<CallingConvention> Convention =
                     conventionWord("__" + Attribute.str()))
          PendingConvention = Convention;
        // Any other attribute says nothing of a type: skip its arguments.
        if (Arguments) {
          take();
          int Depth = 1;
          while (Depth > 0 && !Failed) {
            if (peek().Kind == TokenKind::End) {
              fail(Name, "an attribute's arguments do not end");
              return false;
            }
            if (isPunct("("))
              ++Depth;
            else if (isPunct(")"))
              --Depth;
            take();
          }
        }
      }
      if (!expect(")", "after the attributes") ||
          (Gnu && !expect(")", "after the attributes")))
        return false;
    }
    return !Failed;
  }

  /// An alignment's value: a power of two.
  std::optional<int64_t> alignment(const Token &At) {
    std::optional<int64_t> Value = constant();
    if (!Value)
      return Value;
    if (*Value <= 0 || (*Value & (*Value - 1))) {
      fail(At, "an alignment must be a power of two");
      return std::nullopt;
    }
    return Value;
  }

  /// struct or union: a reference, a definition, or an anonymous record.
  TypeExprRef record(bool Union, Attributes Asked, bool &Defines) {
    const TypeKind Kind = Union ? TypeKind::Union : TypeKind::Struct;
    if (!attributes(Asked))
      return nullptr;
    std::string Name;
    Token NameToken = peek();
    if (peek().Kind == TokenKind::Word && !isPunct("{", 0)) {
      Name = take().Text;
      // C++ may name a base after a colon; local types have none.
      if (Cpp && isPunct(":")) {
        fail(peek(), "local types hold no base classes");
        return nullptr;
      }
    }
    if (!isPunct("{")) {
      if (Name.empty()) {
        fail(peek(), Union ? "expected a union's name or body"
                           : "expected a structure's name or body");
        return nullptr;
      }
      // A record named before its definition is declared.
      if (!Library.find(Name)) {
        LocalType Declared;
        Declared.Name = Name;
        Declared.Kind = Kind;
        Declared.Origin = Options.Origin;
        auto Def = std::make_shared<RecordDef>();
        Def->Union = Union;
        Def->Defined = false;
        Declared.Record = std::move(Def);
        Library.define(std::move(Declared));
      }
      return makeNamed(Name, Kind);
    }
    const Token Open = take();
    Defines = true;
    DraftRecord Draft;
    Draft.Union = Union;
    Draft.Pack = Pack;
    Draft.Cpp = Cpp;
    body(Draft);
    if (Failed)
      return nullptr;
    // GNU attributes after the body belong to the record too.
    if (!attributes(Asked))
      return nullptr;
    Draft.Alignment = Asked.Alignment;
    if (Asked.Packed)
      Draft.Pack = 1;
    llvm::Expected<RecordDef> Laid = layOutRecord(Library, Draft);
    if (!Laid) {
      fail(Open, llvm::toString(Laid.takeError()));
      return nullptr;
    }
    auto Def = std::make_shared<RecordDef>(std::move(*Laid));
    if (Name.empty())
      return makeRecord(std::move(Def));
    LocalType Local;
    Local.Name = Name;
    Local.Kind = Kind;
    Local.Origin = Options.Origin;
    Local.Record = std::move(Def);
    Library.define(std::move(Local));
    note(Result.Types, NotedTypes, Name);
    return makeNamed(Name, Kind);
  }

  /// A record's members up to its closing brace.
  void body(DraftRecord &Draft) {
    while (!Failed && !accept("}")) {
      if (peek().Kind == TokenKind::End) {
        fail(peek(), "a record's body does not end");
        return;
      }
      if (peek().Kind == TokenKind::Pragma) {
        pragma(take());
        continue;
      }
      if (accept(";"))
        continue;
      if (isWord("_Static_assert") || isWord("static_assert")) {
        staticAssert();
        continue;
      }
      // C++ access labels say nothing of the layout.
      if ((isWord("public") || isWord("private") || isWord("protected")) &&
          isPunct(":", 1)) {
        take();
        take();
        continue;
      }
      // An unnamed bitfield without specifiers is not C.
      Specifiers Spec;
      if (!specifiers(Spec))
        return;
      if (accept(";")) {
        // An anonymous record; a definition alone adds no member.
        if (Spec.Type->Kind == TypeExprKind::Record)
          Draft.Members.push_back({"", Spec.Type, false, 0, ""});
        continue;
      }
      while (!Failed) {
        DraftMember Field;
        Declared = Attributes();
        if (accept(":")) {
          Field.Type = Spec.Type;
          Field.Bitfield = true;
        } else {
          Token NameToken = peek();
          Field.Type = declarator(Spec.Type, Field.Name, NameToken, false);
          if (Failed)
            return;
          if (Field.Type->Kind == TypeExprKind::Function) {
            fail(NameToken, "local types hold no methods");
            return;
          }
          if (accept(":"))
            Field.Bitfield = true;
        }
        if (Field.Bitfield) {
          const Token At = peek();
          std::optional<int64_t> Width = constant();
          if (!Width)
            return;
          if (*Width < 0 || *Width > 128) {
            fail(At, "a bitfield's width must be 0 to 128 bits");
            return;
          }
          Field.Width = uint32_t(*Width);
        }
        // GNU puts a member's attributes after its width too.
        if (!attributes(Declared))
          return;
        Field.Alignment = std::max(Declared.Alignment,
                                   Spec.Defines ? 0u : Spec.Asked.Alignment);
        Field.Packed = Declared.Packed || (!Spec.Defines && Spec.Asked.Packed);
        Draft.Members.push_back(std::move(Field));
        if (accept(","))
          continue;
        expect(";", "after a member");
        break;
      }
    }
  }

  /// An enumeration: a reference or a definition, with its values known to
  /// the expressions after it.
  TypeExprRef enumeration(bool &Defines) {
    if (isWord("class") || isWord("struct"))
      take();
    Attributes Asked;
    if (!attributes(Asked))
      return nullptr;
    std::string Name;
    if (peek().Kind == TokenKind::Word)
      Name = take().Text;
    Builtin Underlying = Builtin::Int;
    bool Fixed = false;
    if (accept(":")) {
      Specifiers Spec;
      if (!specifiers(Spec))
        return nullptr;
      if (Spec.Type->Kind == TypeExprKind::Builtin) {
        Underlying = Spec.Type->TheBuiltin;
      } else if (const LocalType *Type = Spec.Type->Kind == TypeExprKind::Named
                                             ? Library.resolve(Spec.Type->Name)
                                             : nullptr;
                 Type && Type->Target &&
                 Type->Target->Kind == TypeExprKind::Builtin) {
        Underlying = Type->Target->TheBuiltin;
      } else {
        fail(peek(), "an enumeration's underlying type must be an integer");
        return nullptr;
      }
      Fixed = true;
    }
    if (!accept("{")) {
      if (Name.empty()) {
        fail(peek(), "expected an enumeration's name or body");
        return nullptr;
      }
      if (!Library.find(Name)) {
        LocalType Declared;
        Declared.Name = Name;
        Declared.Kind = TypeKind::Enum;
        Declared.Origin = Options.Origin;
        auto Def = std::make_shared<EnumDef>();
        Def->Underlying = Underlying;
        Declared.Enum = std::move(Def);
        Library.define(std::move(Declared));
      }
      return makeNamed(Name, TypeKind::Enum);
    }
    (void)Fixed;
    Defines = true;
    auto Def = std::make_shared<EnumDef>();
    Def->Underlying = Underlying;
    const unsigned Bits = builtinBytes(Underlying, Library.dataModel()) * 8;
    const bool Signed = builtinInfo(Underlying).Signed;
    int64_t Next = 0;
    while (!Failed && !accept("}")) {
      if (peek().Kind != TokenKind::Word) {
        fail(peek(), "expected an enumerator's name");
        return nullptr;
      }
      Enumerator Value;
      Value.Name = take().Text;
      if (accept("=")) {
        std::optional<int64_t> Given = constant();
        if (!Given)
          return nullptr;
        Next = *Given;
      }
      // The value takes the underlying type's width and sign.
      uint64_t Raw = uint64_t(Next);
      if (Bits && Bits < 64) {
        Raw &= (uint64_t(1) << Bits) - 1;
        if (Signed && (Raw >> (Bits - 1)) & 1)
          Raw |= ~((uint64_t(1) << Bits) - 1);
      }
      Value.Value = Raw;
      Enumerators[Value.Name] = int64_t(Raw);
      Def->Values.push_back(std::move(Value));
      ++Next;
      if (!accept(",")) {
        expect("}", "after an enumeration's values");
        break;
      }
    }
    if (Failed || !attributes(Asked))
      return nullptr;
    // A packed or aligned enumeration takes a size its values do not say.
    if (Asked.Packed || Asked.Alignment) {
      fail(peek(), "a packed or aligned enumeration is not a local type");
      return nullptr;
    }
    if (Name.empty())
      return makeEnum(std::move(Def));
    LocalType Local;
    Local.Name = Name;
    Local.Kind = TypeKind::Enum;
    Local.Origin = Options.Origin;
    Local.Enum = std::move(Def);
    Library.define(std::move(Local));
    note(Result.Types, NotedTypes, Name);
    return makeNamed(Name, TypeKind::Enum);
  }

  /// A declarator over \p Base: pointers, the name, arrays and parameters,
  /// with parentheses binding as C binds them.
  TypeExprRef declarator(TypeExprRef Base, std::string &Name, Token &NameToken,
                         bool Abstract) {
    Declared = Attributes();
    std::function<TypeExprRef(TypeExprRef)> Wrap =
        parseDeclarator(Name, NameToken, Abstract);
    if (Failed)
      return nullptr;
    TypeExprRef Type = Wrap(std::move(Base));
    if (!Abstract && Name.empty() && !Failed)
      fail(NameToken, "expected a name");
    return Type;
  }

  std::function<TypeExprRef(TypeExprRef)>
  parseDeclarator(std::string &Name, Token &NameToken, bool Abstract) {
    struct Pointer {
      bool Const = false, Volatile = false, Reference = false;
      uint32_t Bytes = 0;
    };
    std::vector<Pointer> Pointers;
    while (isPunct("*") || (Cpp && (isPunct("&") || isPunct("&&")))) {
      Pointer P;
      P.Reference = !isPunct("*");
      P.Bytes = pointerSize();
      take();
      while (peek().Kind == TokenKind::Word || startsAttribute()) {
        if (startsAttribute()) {
          if (!attributes(Declared))
            return Identity;
          continue;
        }
        const llvm::StringRef W = peek().Text;
        if (constWord(W))
          P.Const = true;
        else if (volatileWord(W))
          P.Volatile = true;
        else if (W == "__ptr32")
          P.Bytes = 4;
        else if (W == "__ptr64")
          P.Bytes = 8;
        else if (conventionWord(W))
          PendingConvention = conventionWord(W);
        else if (!ignoredWord(W))
          break;
        take();
      }
      Pointers.push_back(P);
    }
    std::function<TypeExprRef(TypeExprRef)> Inner = Identity;
    while ((peek().Kind == TokenKind::Word && conventionWord(peek().Text)) ||
           startsAttribute()) {
      if (!startsAttribute()) {
        PendingConvention = conventionWord(take().Text);
        continue;
      }
      if (!attributes(Declared))
        return Identity;
    }
    if (isPunct("(") && nestedDeclaratorFollows()) {
      take();
      while ((peek().Kind == TokenKind::Word &&
              (conventionWord(peek().Text) || ignoredWord(peek().Text))) ||
             startsAttribute()) {
        if (startsAttribute()) {
          if (!attributes(Declared))
            return Identity;
          continue;
        }
        if (std::optional<CallingConvention> Convention =
                conventionWord(peek().Text))
          PendingConvention = Convention;
        take();
      }
      Inner = parseDeclarator(Name, NameToken, Abstract);
      if (Failed || !expect(")", "after a declarator"))
        return Inner;
    } else if (peek().Kind == TokenKind::Word && !reservedWord(peek().Text)) {
      // A type's name may be declared again: typedef uint8_t UCHAR.
      NameToken = peek();
      Name = take().Text;
      // C++ qualified names keep their spelling.
      while (isPunct("::") && peek(1).Kind == TokenKind::Word) {
        take();
        Name += "::" + take().Text;
      }
    }
    // Arrays and parameters, in their order.
    struct Suffix {
      bool Array = false;
      std::optional<uint64_t> Count;
      std::shared_ptr<FunctionDef> Function;
    };
    std::vector<Suffix> Suffixes;
    while (!Failed) {
      // GNU puts attributes and assembler names after a declarator too.
      if (startsAttribute()) {
        if (!attributes(Declared))
          break;
        continue;
      }
      if (accept("[")) {
        Suffix S;
        S.Array = true;
        if (!isPunct("]")) {
          const Token At = peek();
          std::optional<int64_t> Count = constant();
          if (!Count)
            break;
          if (*Count < 0) {
            fail(At, "an array cannot have fewer than no elements");
            break;
          }
          S.Count = uint64_t(*Count);
        }
        expect("]", "after an array's size");
        Suffixes.push_back(std::move(S));
        continue;
      }
      if (isPunct("(")) {
        Suffix S;
        S.Function = parameters();
        Suffixes.push_back(std::move(S));
        // C++ const and noexcept after parameters say nothing of the type.
        while (isWord("const") || isWord("noexcept") || isWord("override"))
          take();
        continue;
      }
      break;
    }
    return [Pointers, Inner, Suffixes](TypeExprRef T) {
      for (const Pointer &P : Pointers)
        T = qualified(makePointer(T, P.Bytes, P.Reference), P.Const,
                      P.Volatile);
      for (auto It = Suffixes.rbegin(); It != Suffixes.rend(); ++It) {
        if (It->Array) {
          T = makeArray(T, It->Count);
          continue;
        }
        auto Function = std::make_shared<FunctionDef>(*It->Function);
        Function->Return = T;
        T = makeFunction(std::move(Function));
      }
      return Inner(T);
    };
  }

  /// Whether '(' begins a parenthesized declarator rather than parameters.
  bool nestedDeclaratorFollows() const {
    if (startsAttribute(1))
      return true;
    const Token &Next = peek(1);
    if (Next.Kind == TokenKind::Punct)
      return Next.Text == "*" || Next.Text == "&" || Next.Text == "(" ||
             Next.Text == "^";
    if (Next.Kind != TokenKind::Word)
      return false;
    if (conventionWord(Next.Text))
      return true;
    return !startsType(1) && !isWord("void", 1);
  }

  /// A parameter list, the convention pending applied.
  std::shared_ptr<FunctionDef> parameters() {
    // Each parameter's declarator has attributes of its own.
    const Attributes Outer = Declared;
    auto Restore = llvm::make_scope_exit([&] { Declared = Outer; });
    auto Function = std::make_shared<FunctionDef>();
    if (PendingConvention) {
      // A convention the target ignores, as 64-bit x86 does __stdcall, is
      // no convention of the function's.
      if (pointerSize() < 8 || keptWide(*PendingConvention))
        Function->Convention = *PendingConvention;
      PendingConvention.reset();
    }
    take(); // (
    if (accept(")"))
      return Function;
    if (isWord("void") && isPunct(")", 1)) {
      take();
      take();
      return Function;
    }
    while (!Failed) {
      if (accept("...")) {
        Function->Variadic = true;
        // Only the caller knows what a variadic call pushes: compilers take
        // a convention whose callee pops, as phnt's NTAPI
        // RtlInitializeSidEx, as cdecl.
        if (calleePops(Function->Convention))
          Function->Convention = CallingConvention::Cdecl;
        expect(")", "after '...'");
        return Function;
      }
      Specifiers Spec;
      if (!specifiers(Spec))
        return Function;
      Parameter Param;
      Token NameToken = peek();
      Param.Type = declarator(Spec.Type, Param.Name, NameToken, true);
      if (Failed)
        return Function;
      // An array or a function passes as a pointer to it.
      if (Param.Type->Kind == TypeExprKind::Array)
        Param.Type = makePointer(Param.Type->Element, pointerSize());
      else if (Param.Type->Kind == TypeExprKind::Function)
        Param.Type = makePointer(Param.Type, pointerSize());
      // A default argument says nothing of the type.
      if (accept("="))
        constant();
      Function->Params.push_back(std::move(Param));
      if (accept(","))
        continue;
      expect(")", "after the parameters");
      break;
    }
    return Function;
  }

  // Constant expressions.
  std::optional<int64_t> constant() {
    std::optional<int64_t> Condition = binary(0);
    if (!Condition || !accept("?"))
      return Condition;
    std::optional<int64_t> Then = constant();
    if (!Then || !expect(":", "in a conditional constant"))
      return std::nullopt;
    std::optional<int64_t> Else = constant();
    if (!Else)
      return std::nullopt;
    return *Condition ? *Then : *Else;
  }

  static int precedence(llvm::StringRef Op) {
    static const std::pair<const char *, int> Levels[] = {
        {"||", 1}, {"&&", 2}, {"|", 3},  {"^", 4},  {"&", 5},  {"==", 6},
        {"!=", 6}, {"<", 7},  {"<=", 7}, {">", 7},  {">=", 7}, {"<<", 8},
        {">>", 8}, {"+", 9},  {"-", 9},  {"*", 10}, {"/", 10}, {"%", 10}};
    for (const auto &[Text, Level] : Levels)
      if (Op == Text)
        return Level;
    return 0;
  }

  std::optional<int64_t> binary(int Lowest) {
    std::optional<int64_t> Left = unary();
    while (Left && !Failed) {
      const Token &Op = peek();
      const int Level = Op.Kind == TokenKind::Punct ? precedence(Op.Text) : 0;
      if (!Level || Level <= Lowest)
        break;
      const std::string Text = take().Text;
      std::optional<int64_t> Right = binary(Level);
      if (!Right)
        return std::nullopt;
      const int64_t A = *Left, B = *Right;
      if ((Text == "/" || Text == "%") && B == 0) {
        fail(Op, "a constant divides by zero");
        return std::nullopt;
      }
      Left = llvm::StringSwitch<int64_t>(Text)
                 .Case("||", A || B)
                 .Case("&&", A && B)
                 .Case("|", A | B)
                 .Case("^", A ^ B)
                 .Case("&", A & B)
                 .Case("==", A == B)
                 .Case("!=", A != B)
                 .Case("<", A < B)
                 .Case("<=", A <= B)
                 .Case(">", A > B)
                 .Case(">=", A >= B)
                 .Case("<<", B >= 0 && B < 64 ? int64_t(uint64_t(A) << B) : 0)
                 .Case(">>", B >= 0 && B < 64 ? A >> B : 0)
                 .Case("+", int64_t(uint64_t(A) + uint64_t(B)))
                 .Case("-", int64_t(uint64_t(A) - uint64_t(B)))
                 .Case("*", int64_t(uint64_t(A) * uint64_t(B)))
                 .Case("/", B ? A / B : 0)
                 .Case("%", B ? A % B : 0)
                 .Default(0);
    }
    if (Left && Lowest == 0 && accept("?")) {
      std::optional<int64_t> Then = binary(0);
      if (!Then || !expect(":", "in a conditional constant"))
        return std::nullopt;
      std::optional<int64_t> Else = binary(0);
      if (!Else)
        return std::nullopt;
      return *Left ? *Then : *Else;
    }
    return Left;
  }

  std::optional<int64_t> unary() {
    const Token At = peek();
    if (accept("-")) {
      std::optional<int64_t> V = unary();
      return V ? std::optional<int64_t>(int64_t(0 - uint64_t(*V))) : V;
    }
    if (accept("+"))
      return unary();
    if (accept("~")) {
      std::optional<int64_t> V = unary();
      return V ? std::optional<int64_t>(~*V) : V;
    }
    if (accept("!")) {
      std::optional<int64_t> V = unary();
      return V ? std::optional<int64_t>(!*V) : V;
    }
    if (At.Kind == TokenKind::Number) {
      if (At.Floating) {
        fail(At, "'" + At.Text + "' is a floating constant, not an integer");
        return std::nullopt;
      }
      return int64_t(take().Value);
    }
    if (isPunct("(")) {
      // A cast to an integer type keeps the value's bits in its width.
      if (startsType(1) && !isWord("sizeof", 1)) {
        take();
        Specifiers Spec;
        if (!specifiers(Spec))
          return std::nullopt;
        std::string Ignored;
        Token NameToken = peek();
        TypeExprRef Type = declarator(Spec.Type, Ignored, NameToken, true);
        if (Failed || !expect(")", "after a cast's type"))
          return std::nullopt;
        std::optional<int64_t> V = unary();
        if (!V)
          return V;
        if (Type && Type->Kind == TypeExprKind::Builtin) {
          const unsigned Bits =
              builtinBytes(Type->TheBuiltin, Library.dataModel()) * 8;
          if (Bits && Bits < 64) {
            uint64_t Raw = uint64_t(*V) & ((uint64_t(1) << Bits) - 1);
            if (builtinInfo(Type->TheBuiltin).Signed && (Raw >> (Bits - 1)) & 1)
              Raw |= ~((uint64_t(1) << Bits) - 1);
            return int64_t(Raw);
          }
        }
        return V;
      }
      take();
      std::optional<int64_t> V = binary(0);
      if (!V || !expect(")", "after a constant"))
        return std::nullopt;
      return V;
    }
    if (At.Kind == TokenKind::Word) {
      if (At.Text == "sizeof" || At.Text == "alignof" ||
          At.Text == "_Alignof" || At.Text == "__alignof" ||
          At.Text == "__alignof__") {
        take();
        // The size of a member reached through a null pointer to its
        // record, as RTL_FIELD_SIZE spells it: sizeof(((T *)0)->Member).
        if (At.Text == "sizeof" && isPunct("(") && isPunct("(", 1) &&
            isPunct("(", 2) && startsType(3))
          return memberSize(At);
        // The size of a string literal is its bytes and the terminator's,
        // as INTERNET_MAX_URL_LENGTH counts sizeof("://").
        if (At.Text == "sizeof" &&
            (peek().Kind == TokenKind::String ||
             (isPunct("(", 0) && peek(1).Kind == TokenKind::String))) {
          const bool Parenthesized = accept("(");
          uint64_t Size = 1;
          while (peek().Kind == TokenKind::String)
            Size += take().Text.size();
          if (Parenthesized && !expect(")", "after a string"))
            return std::nullopt;
          return int64_t(Size);
        }
        TypeExprRef Type = typeInParentheses();
        if (!Type)
          return std::nullopt;
        if (At.Text != "sizeof")
          return int64_t(Library.alignOf(*Type));
        std::optional<uint64_t> Size = Library.sizeOf(*Type);
        if (!Size) {
          fail(At, "sizeof a type whose size is unknown");
          return std::nullopt;
        }
        return int64_t(*Size);
      }
      if (At.Text == "offsetof" || At.Text == "FIELD_OFFSET" ||
          At.Text == "__builtin_offsetof") {
        take();
        return offsetOf(At);
      }
      if (At.Text == "true" || At.Text == "TRUE")
        return take(), 1;
      if (At.Text == "false" || At.Text == "FALSE")
        return take(), 0;
      if (auto Found = Enumerators.find(At.Text); Found != Enumerators.end())
        return take(), Found->second;
    }
    fail(At, At.Kind == TokenKind::Word
                 ? "'" + At.Text + "' is no constant this knows"
                 : "expected a constant");
    return std::nullopt;
  }

  TypeExprRef typeInParentheses() {
    if (!expect("(", "before a type"))
      return nullptr;
    Specifiers Spec;
    if (!specifiers(Spec))
      return nullptr;
    std::string Ignored;
    Token NameToken = peek();
    TypeExprRef Type = declarator(Spec.Type, Ignored, NameToken, true);
    if (Failed || !expect(")", "after a type"))
      return nullptr;
    return Type;
  }

  /// offsetof(type, member.member...), anonymous records looked through.
  std::optional<int64_t> offsetOf(const Token &At) {
    if (!expect("(", "after offsetof"))
      return std::nullopt;
    Specifiers Spec;
    if (!specifiers(Spec))
      return std::nullopt;
    if (!expect(",", "after offsetof's type"))
      return std::nullopt;
    const RecordDef *Record = recordOf(*Spec.Type);
    uint64_t Bits = 0;
    while (!Failed) {
      if (peek().Kind != TokenKind::Word) {
        fail(peek(), "expected a member's name");
        return std::nullopt;
      }
      const Token Member = take();
      const neverd::types::Member *Found =
          Record ? find(*Record, Member.Text, Bits) : nullptr;
      if (!Found) {
        fail(Member, "no member '" + Member.Text + "' to take the offset of");
        return std::nullopt;
      }
      if (!accept("."))
        break;
      Record = recordOf(*Found->Type);
    }
    if (!expect(")", "after offsetof's member"))
      return std::nullopt;
    (void)At;
    return int64_t(Bits / 8);
  }

  /// sizeof(((type *)0)->member.member...), anonymous records looked
  /// through: the size of the member.
  std::optional<int64_t> memberSize(const Token &At) {
    take();
    take();
    const TypeExprRef Pointer = typeInParentheses();
    if (!Pointer)
      return std::nullopt;
    if (Pointer->Kind != TypeExprKind::Pointer || !Pointer->Element ||
        peek().Kind != TokenKind::Number || peek().Value != 0) {
      fail(peek(), "expected a null pointer to a record");
      return std::nullopt;
    }
    take();
    if (!expect(")", "after the null pointer") ||
        !expect("->", "after the null pointer"))
      return std::nullopt;
    const RecordDef *Record = recordOf(*Pointer->Element);
    const neverd::types::Member *Found = nullptr;
    uint64_t Bits = 0;
    do {
      if (peek().Kind != TokenKind::Word) {
        fail(peek(), "expected a member's name");
        return std::nullopt;
      }
      const Token Name = take();
      Found = Record ? find(*Record, Name.Text, Bits) : nullptr;
      if (!Found) {
        fail(Name, "no member '" + Name.Text + "' to take the size of");
        return std::nullopt;
      }
      Record = recordOf(*Found->Type);
    } while (accept("."));
    if (!expect(")", "after the member"))
      return std::nullopt;
    const std::optional<uint64_t> Size =
        Found->isBitfield() ? std::nullopt : Library.sizeOf(*Found->Type);
    if (!Size) {
      fail(At, "sizeof a member whose size is unknown");
      return std::nullopt;
    }
    return int64_t(*Size);
  }

  const RecordDef *recordOf(const TypeExpr &Type) const {
    if (Type.Kind == TypeExprKind::Record)
      return Type.Record.get();
    if (Type.Kind == TypeExprKind::Named)
      if (const LocalType *Named = Library.resolve(Type.Name))
        return Named->Record.get();
    return nullptr;
  }

  /// The member \p Name of \p Record, anonymous records looked through,
  /// with its offset added to \p Bits.
  const Member *find(const RecordDef &Record, llvm::StringRef Name,
                     uint64_t &Bits) const {
    for (const Member &Field : Record.Members) {
      if (Field.Name == Name) {
        Bits += Field.BitOffset;
        return &Field;
      }
      if (Field.isAnonymous()) {
        uint64_t Inner = Bits + Field.BitOffset;
        if (const Member *Found = find(*Field.Type->Record, Name, Inner)) {
          Bits = Inner;
          return Found;
        }
      }
    }
    return nullptr;
  }

  /// _Static_assert and static_assert: checked against the layout.
  void staticAssert() {
    const Token At = take();
    if (!expect("(", "after " + At.Text))
      return;
    std::optional<int64_t> Value = constant();
    if (!Value)
      return;
    std::string Message;
    if (accept(",")) {
      if (peek().Kind != TokenKind::String) {
        fail(peek(), "expected the assertion's message");
        return;
      }
      Message = take().Text;
      while (peek().Kind == TokenKind::String)
        Message += take().Text;
    }
    if (!expect(")", "after the assertion") || !expect(";", "after it"))
      return;
    if (!*Value)
      fail(At, Message.empty() ? "a static assertion fails"
                               : "a static assertion fails: " + Message);
  }

  std::vector<Token> Tokens;
  size_t Pos = 0;
  TypeLibrary &Library;
  const ParseOptions &Options;
  ParseResult &Result;
  /// The names Result lists.
  llvm::StringSet<> NotedTypes, NotedPrototypes;
  const bool Cpp;
  bool Failed = false;
  /// Open extern "C" and namespace blocks.
  unsigned Blocks = 0;
  std::optional<CallingConvention> PendingConvention;
  /// What the attributes of the declarator being read say.
  Attributes Declared;
  const std::function<TypeExprRef(TypeExprRef)> Identity = [](TypeExprRef T) {
    return T;
  };
  llvm::StringMap<int64_t> Enumerators;
};

} // namespace

ParseResult parseTypes(llvm::StringRef Source, TypeLibrary &Library,
                       const ParseOptions &Options) {
  switch (Options.Lang) {
  case Language::C:
  case Language::Cpp:
    return parseCTypes(Source, Library, Options);
  case Language::Rust:
    return parseRustTypes(Source, Library, Options);
  case Language::Go:
    return parseGoTypes(Source, Library, Options);
  case Language::Python:
  case Language::JavaScript:
  case Language::Lua:
    break;
  }
  // The language renders, but reads in none of its forms.
  ParseResult Result;
  Result.Problems.push_back(
      {1, 1,
       languageDisplay(Options.Lang).str() +
           " declarations do not read; declare in C/C++, Rust or Go"});
  return Result;
}

ParseResult parseCTypes(llvm::StringRef Source, TypeLibrary &Library,
                        const ParseOptions &Options) {
  ParseResult Result;
  std::vector<Token> Tokens = Lexer(Source, Result.Problems).run();
  if (!Result.Problems.empty())
    return Result;
  TypeLibrary Scratch(Library);
  Parser(std::move(Tokens), Scratch, Options, Result).run();
  if (Result.Problems.empty() || Options.KeepGoing)
    Library = std::move(Scratch);
  return Result;
}

FunctionDeclaration parseFunctionDeclaration(llvm::StringRef Source,
                                             const TypeLibrary &Library,
                                             const ParseOptions &Options) {
  FunctionDeclaration Declared;
  ParseResult Result;
  // A prototype is typed without its semicolon as often as with it.
  std::string Text = Source.rtrim().str();
  if (!llvm::StringRef(Text).ends_with(";") &&
      !llvm::StringRef(Text).ends_with("}"))
    Text += ";";
  std::vector<Token> Tokens = Lexer(Text, Result.Problems).run();
  if (Result.Problems.empty()) {
    TypeLibrary Scratch(Library);
    ParseOptions One = Options;
    One.KeepGoing = false;
    Parser(std::move(Tokens), Scratch, One, Result).run();
    if (Result.Problems.empty()) {
      if (Result.Prototypes.size() != 1)
        Result.Problems.push_back(
            {1, 1,
             Result.Prototypes.empty()
                 ? "this declares no function"
                 : "this declares " + std::to_string(Result.Prototypes.size()) +
                       " functions; declare one"});
      else if (!Result.Types.empty())
        Result.Problems.push_back(
            {1, 1,
             Library.find(Result.Types.front())
                 ? "the declaration defines " + Result.Types.front() +
                       " again; a function's declaration only uses types"
                 : Result.Types.front() +
                       " is not a local type; declare it among the local "
                       "types first"});
      else {
        Declared.Name = Result.Prototypes.front();
        Declared.Function = Scratch.prototypes().lookup(Declared.Name);
        // A name C lets a declaration use before it is defined, such as
        // `struct S *`, must still be one of the library's.
        LocalType Signature;
        Signature.Kind = TypeKind::Function;
        Signature.Function = Declared.Function;
        for (const std::string &Name : mentionedTypeNames(Signature))
          if (!Library.find(Name)) {
            Result.Problems.push_back(
                {1, 1,
                 Name + " is not a local type; declare it among the local "
                        "types first"});
            break;
          }
      }
    }
  }
  Declared.Problems = std::move(Result.Problems);
  if (!Declared.Problems.empty())
    Declared.Function.reset();
  return Declared;
}

} // namespace neverd::types
