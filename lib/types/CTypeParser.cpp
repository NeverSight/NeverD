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

#include "neverd/types/Limits.h"
#include "neverd/types/TypeParse.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/StringSaver.h"

#include <functional>

namespace neverd::types {
namespace {

enum class TokenKind : uint8_t { End, Word, Number, String, Punct, Pragma };

/// The prefix a quoted literal takes: none, L, u, U or u8, which say how
/// wide its units are and, for a character, its type.
enum class Encoding : uint8_t { Plain, Wide, UTF16, UTF32, UTF8 };

struct Token {
  TokenKind Kind = TokenKind::End;
  /// Ordinary spelling borrows the parse input; generated spelling belongs
  /// to that parse's arena. Both outlive all tokens, including macro copies.
  llvm::StringRef Text;
  uint64_t Value = 0;
  /// A quoted literal's prefix; a character constant's type follows it.
  Encoding Quoted = Encoding::Plain;
  /// Whether a number is a character constant ('A', L'A').
  bool Character = false;
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

/// The value of a #pragma neverd_header token, whose text is the header.
constexpr uint64_t HeaderPragma = uint64_t(0xFF) << 32;
/// A source unit's unannotated function types use the convention in Text.
constexpr uint64_t DefaultConventionPragma = uint64_t(0xFE) << 32;

/// Whether \p A and \p B lay their members out alike.
bool sameLayout(const RecordDef &A, const RecordDef &B) {
  if (A.Size != B.Size || A.Alignment != B.Alignment ||
      A.Members.size() != B.Members.size())
    return false;
  for (size_t I = 0; I < A.Members.size(); ++I)
    if (A.Members[I].BitOffset != B.Members[I].BitOffset ||
        A.Members[I].StorageBitOffset != B.Members[I].StorageBitOffset)
      return false;
  return true;
}

constexpr llvm::StringLiteral Punctuators[] = {
    "...", "::", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "->", "{",
    "}",   "(",  ")",  "[",  "]",  ";",  ",",  "*",  "&",  ":",  "=",  "<",
    ">",   "+",  "-",  "~",  "!",  "/",  "%",  "^",  "|",  "?",  ".",  "#"};

class Lexer {
public:
  Lexer(llvm::StringRef Source, std::vector<SourceProblem> &Problems,
        llvm::StringSaver &Spellings)
      : Source(Source), Problems(Problems), Spellings(Spellings) {}

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
      if (Next.Kind == TokenKind::Word)
        if (auto Found = Macros.find(Next.Text); Found != Macros.end()) {
          expand(std::move(Next), Found->second, Tokens, 0);
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
      const llvm::StringRef Word = Source.slice(Start, At);
      // An encoding prefix directly before a quote: L"A", u'A', u8"A".
      if (At < Source.size() && (Source[At] == '"' || Source[At] == '\'')) {
        const std::optional<Encoding> Prefix =
            llvm::StringSwitch<std::optional<Encoding>>(Word)
                .Case("L", Encoding::Wide)
                .Case("u", Encoding::UTF16)
                .Case("U", Encoding::UTF32)
                .Case("u8", Encoding::UTF8)
                .Default(std::nullopt);
        if (Prefix) {
          quoted(Result);
          Result.Quoted = *Prefix;
          return Result;
        }
      }
      Result.Kind = TokenKind::Word;
      Result.Text = Word;
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
        Result.Text = Text;
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
      Result.Text = Text;
      if (Digits.getAsInteger(Radix, Result.Value))
        problem(Result.Line, Result.Column,
                "'" + Text.str() + "' is no integer this reads");
      return Result;
    }
    if (C == '"' || C == '\'') {
      quoted(Result);
      return Result;
    }
    for (const llvm::StringRef Punct : Punctuators)
      if (C == Punct.front() && Source.substr(At).starts_with(Punct)) {
        advance(Punct.size());
        Result.Kind = TokenKind::Punct;
        Result.Text = Punct;
        return Result;
      }
    problem(Line, Column,
            std::string("'") + C + "' cannot begin a declaration's word");
    advance();
    return Token();
  }

  /// Reads the quoted literal at the current place into \p Result: a string,
  /// or a character constant, which is a number.
  void quoted(Token &Result) {
    const char Quote = Source[At];
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
      Result.Character = true;
    } else {
      Result.Kind = TokenKind::String;
    }
    Result.Text = Spellings.save(Text);
  }

  /// Expand a known macro. Ordinary identifiers stay on the token append path.
  void expand(Token Word, const std::vector<Token> &Replacements,
              std::vector<Token> &Tokens, unsigned Depth) {
    if (Depth > ::neverd::limits::types::kCTypeParserExpandDepthLimit) {
      Tokens.push_back(std::move(Word));
      return;
    }
    for (Token Replacement : Replacements) {
      Replacement.Line = Word.Line;
      Replacement.Column = Word.Column;
      if (Replacement.Kind == TokenKind::Word)
        if (auto Found = Macros.find(Replacement.Text); Found != Macros.end()) {
          expand(std::move(Replacement), Found->second, Tokens, Depth + 1);
          continue;
        }
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
    // Line splicing and nested macro lexers must borrow storage that remains
    // live after this directive and after a later #undef or redefinition.
    llvm::StringRef Rest = Spellings.save(Text).drop_front().ltrim();
    const llvm::StringRef Name =
        Rest.take_while([](char C) { return llvm::isAlnum(C) || C == '_'; });
    Rest = Rest.drop_front(Name.size()).ltrim();
    // Preprocessing does not preserve compiler defaults such as the WDK's
    // x86 /Gz. A generated source unit supplies that default explicitly;
    // rendered function declarations carry the convention themselves.
    constexpr llvm::StringLiteral DefaultConventionDirective =
        "neverd_default_calling_convention";
    if (Name == "pragma" && Rest.starts_with(DefaultConventionDirective)) {
      llvm::StringRef Arguments =
          Rest.drop_front(DefaultConventionDirective.size()).trim();
      if (!Arguments.consume_front("(") || !Arguments.consume_back(")")) {
        problem(L, Col,
                "#pragma neverd_default_calling_convention takes a "
                "convention in parentheses");
        return;
      }
      Token Pragma;
      Pragma.Kind = TokenKind::Pragma;
      Pragma.Text = Arguments.trim();
      Pragma.Value = DefaultConventionPragma;
      Pragma.Line = L;
      Pragma.Column = Col;
      Tokens.push_back(std::move(Pragma));
      return;
    }
    // The system header the declarations after it come from, as NeverD's
    // libraries of a system's headers name it: neverd_header("stdio.h"),
    // or neverd_header() for none.
    if (Name == "pragma" && Rest.starts_with("neverd_header")) {
      llvm::StringRef Arguments = Rest.drop_front(13).trim();
      if (!Arguments.consume_front("(") || !Arguments.consume_back(")") ||
          (!(Arguments = Arguments.trim()).empty() &&
           (!Arguments.consume_front("\"") || !Arguments.consume_back("\"") ||
            Arguments.contains('"') || Arguments.contains('\\')))) {
        problem(L, Col,
                "#pragma neverd_header takes a header's name in quotes");
        return;
      }
      Token Pragma;
      Pragma.Kind = TokenKind::Pragma;
      Pragma.Text = Arguments;
      Pragma.Value = HeaderPragma;
      Pragma.Line = L;
      Pragma.Column = Col;
      Tokens.push_back(std::move(Pragma));
      return;
    }
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
      for (llvm::StringRef &Part : Parts)
        Part = Part.trim();
      if (Parts.size() == 1 && Parts.front() == "show")
        return;
      llvm::StringRef Label;
      const bool Stack =
          !Parts.empty() && (Parts.front() == "push" || Parts.front() == "pop");
      if (Parts.size() > (Stack ? 3u : 1u)) {
        problem(L, Col, "#pragma pack has too many arguments");
        return;
      }
      for (size_t I = 0; I < Parts.size(); ++I) {
        const llvm::StringRef Part = Parts[I];
        if (I == 0 && Stack) {
          Action = Part == "push" ? PackAction::Push : PackAction::Pop;
        } else if (std::optional<uint64_t> Number = number(Part);
                   Number && I + 1 == Parts.size()) {
          Value = *Number;
          if (!Value || Value > 16 || (Value & (Value - 1))) {
            problem(L, Col, "#pragma pack takes 1, 2, 4, 8 or 16");
            return;
          }
          Action = Action == PackAction::Push  ? PackAction::PushSet
                   : Action == PackAction::Pop ? PackAction::PopSet
                                               : PackAction::Set;
        } else if (Stack && I == 1 && !Part.empty() &&
                   (llvm::isAlpha(Part.front()) || Part.front() == '_') &&
                   llvm::all_of(Part, [](char C) {
                     return llvm::isAlnum(C) || C == '_';
                   })) {
          Label = Part;
        } else {
          problem(L, Col,
                  "'" + Part.str() +
                      "' in #pragma pack is no packing this knows; define it "
                      "as a number");
          return;
        }
      }
      if (Parts.empty())
        Action = PackAction::Reset;
      Token Pragma;
      Pragma.Kind = TokenKind::Pragma;
      Pragma.Text = Label;
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
      Lexer Inner(Body, Ignored, Spellings);
      std::vector<Token> Replacement = Inner.run();
      Replacement.pop_back();
      // A header NeverD renders names a system's types otherwise for
      // another system's compilers (#define X neverd_X); the declarations
      // read are the system's.
      if (Replacement.size() == 1 && Replacement[0].Kind == TokenKind::Word &&
          Replacement[0].Text == "neverd_" + Macro.str())
        return;
      Macros[Macro] = std::move(Replacement);
      return;
    }
    if (Name == "undef")
      Macros.erase(
          Rest.take_while([](char C) { return llvm::isAlnum(C) || C == '_'; }));
  }

  llvm::StringRef Source;
  std::vector<SourceProblem> &Problems;
  llvm::StringSaver &Spellings;
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
      std::initializer_list<llvm::StringRef>{"extern",
                                             "static",
                                             "inline",
                                             "_inline",
                                             "__inline",
                                             "__inline__",
                                             "__forceinline",
                                             "__extension__",
                                             "__w64",
                                             "register",
                                             "auto",
                                             "constexpr",
                                             "virtual",
                                             "explicit",
                                             "mutable",
                                             "__unaligned",
                                             "NTKERNELAPI",
                                             "NTSYSAPI",
                                             "NTSYSCALLAPI",
                                             "NTHALAPI",
                                             "DECLSPEC_IMPORT",
                                             "FORCEINLINE",
                                             "__restrict",
                                             "restrict",
                                             "__restrict__",
                                             "_Atomic",
                                             "thread_local",
                                             "__ptr32",
                                             "__ptr64",
                                             "__sptr",
                                             "__uptr",
                                             "_Nonnull",
                                             "_Nullable",
                                             "_Null_unspecified",
                                             "_Nullable_result",
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

/// Microsoft's original spelling remains an alias in SDK/WDK headers.
bool declspecWord(llvm::StringRef Word) {
  return Word == "__declspec" || Word == "_declspec";
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
    // A record a typedef of its name names, wherever it is defined.
    for (const auto &Entry : SameNamed)
      if (const LocalType *Named = Library.find(Entry.getKey());
          Named && !Named->TypedefNamed &&
          (Named->Kind == TypeKind::Struct || Named->Kind == TypeKind::Union ||
           Named->Kind == TypeKind::Enum)) {
        LocalType Marked = *Named;
        Marked.TypedefNamed = true;
        Library.define(std::move(Marked));
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
  void fail(const Token &At, const llvm::Twine &Message) {
    if (!Failed)
      Result.Problems.push_back({At.Line, At.Column, Message.str()});
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
           exactLongDoubleBytes(Word) || Library.find(Word) ||
           Word == "size_t" || Word == "ptrdiff_t" || Word == "intptr_t" ||
           Word == "uintptr_t" || Word == "ssize_t" ||
           (vaListWord(Word) && !Library.find(Word));
  }
  /// The compiler's va_list, as <stdarg.h>, glibc, bionic, musl and Darwin
  /// name it, and the Microsoft ABI's, which EDK II's x64 VA_LIST is on GCC
  /// and Clang.
  static bool vaListWord(llvm::StringRef Word) {
    return Word == "__builtin_va_list" || Word == "va_list" ||
           Word == "__gnuc_va_list" || Word == "__va_list" ||
           Word == "__isoc_va_list" || Word == "__darwin_va_list" ||
           Word == "__builtin_ms_va_list";
  }

  /// The bytes of each unit of a literal of the encoding \p Quoted:
  /// wchar_t's for L, as the target's ABI sizes it.
  uint64_t unitBytes(Encoding Quoted) const {
    switch (Quoted) {
    case Encoding::Wide:
      return Library.builtinBytes(Builtin::WChar);
    case Encoding::UTF16:
      return 2;
    case Encoding::UTF32:
      return 4;
    case Encoding::Plain:
    case Encoding::UTF8:
      break;
    }
    return 1;
  }

  /// The units the UTF-8 text \p Text of a literal of the encoding
  /// \p Quoted takes: its bytes, else a unit for each character, and for
  /// UTF-16, as a two-byte wchar_t holds it, two past the basic plane.
  uint64_t unitsOf(llvm::StringRef Text, Encoding Quoted) const {
    if (unitBytes(Quoted) == 1)
      return Text.size();
    uint64_t Units = 0;
    for (size_t I = 0; I < Text.size();) {
      const unsigned char Lead = Text[I];
      const size_t Length = Lead < 0x80   ? 1
                            : Lead < 0xE0 ? 2
                            : Lead < 0xF0 ? 3
                                          : 4;
      Units += Length == 4 && unitBytes(Quoted) == 2 ? 2 : 1;
      I += Length;
    }
    return Units;
  }
  /// Whether the token at \p Ahead may begin a declaration's type.
  bool startsType(size_t Ahead = 0) const {
    const Token &T = peek(Ahead);
    if (T.Kind != TokenKind::Word)
      return false;
    if (startsAttribute(Ahead))
      return true;
    return isTypeName(T.Text) || T.Text == "struct" || T.Text == "union" ||
           T.Text == "enum" || T.Text == "class" || typeofWord(T.Text) ||
           constWord(T.Text) || T.Text == "_Noreturn" ||
           T.Text == "DECLSPEC_NORETURN" || volatileWord(T.Text) ||
           T.Text == "typename" || conventionWord(T.Text) ||
           ignoredWord(T.Text) || startsAttribute(Ahead);
  }

  uint32_t pointerSize() const { return pointerBytes(Library.dataModel()); }

  /// A word that cannot name what a declarator declares.
  static bool reservedWord(llvm::StringRef Word) {
    return builtinKeyword(Word) || conventionWord(Word) || ignoredWord(Word) ||
           Word == "struct" || Word == "union" || Word == "enum" ||
           Word == "class" || Word == "const" || Word == "volatile" ||
           Word == "typedef" || declspecWord(Word) || Word == "__attribute__" ||
           Word == "sizeof" || typeofWord(Word);
  }

  static bool typeofWord(llvm::StringRef Word) {
    return Word == "__typeof__" || Word == "__typeof" || Word == "typeof";
  }

  // The pack in force.
  uint32_t Pack = 0;
  struct PackEntry {
    uint32_t Value;
    llvm::StringRef Label;
  };
  std::vector<PackEntry> PackStack;
  // The system header the declarations come from (#pragma neverd_header).
  std::string Header;
  // The names `typedef struct X X` gives a record, enumeration or union.
  llvm::StringSet<> SameNamed;
  void pragma(const Token &Pragma) {
    if (Pragma.Value == HeaderPragma) {
      Header = Pragma.Text;
      return;
    }
    if (Pragma.Value == DefaultConventionPragma) {
      DefaultConvention = conventionWord(Pragma.Text);
      if (!Pragma.Text.empty() && !DefaultConvention)
        fail(Pragma,
             "unknown default calling convention '" + Pragma.Text + "'");
      return;
    }
    const auto Action = PackAction(Pragma.Value >> 32);
    const auto Value = uint32_t(Pragma.Value);
    const auto pop = [&] {
      if (!Pragma.Text.empty()) {
        for (size_t I = PackStack.size(); I; --I) {
          if (PackStack[I - 1].Label != Pragma.Text)
            continue;
          Pack = PackStack[I - 1].Value;
          PackStack.resize(I - 1);
          return;
        }
        // Microsoft pack(pop, label) ignores a label not on the stack.
        return;
      }
      if (PackStack.empty()) {
        fail(Pragma, "#pragma pack(pop) has no push to return to");
        return;
      }
      Pack = PackStack.back().Value;
      PackStack.pop_back();
    };
    switch (Action) {
    case PackAction::Set:
      Pack = Value;
      break;
    case PackAction::Reset:
      Pack = 0;
      break;
    case PackAction::Push:
      PackStack.push_back({Pack, Pragma.Text});
      break;
    case PackAction::PushSet:
      PackStack.push_back({Pack, Pragma.Text});
      Pack = Value;
      break;
    case PackAction::Pop:
      pop();
      break;
    case PackAction::PopSet:
      pop();
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
    /// The name an assembler label gives the declaration's symbol, which a
    /// program imports it by: glibc's __REDIRECT names strtol's
    /// __isoc23_strtol.
    std::string AsmName;
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
    /// An existing type name rather than a struct/union tag declaration.
    bool TypeName = false;
    bool NoReturn = false;
  };

  /// Notes \p Name in \p Names once, in source order: a system header's
  /// library declares thousands, so \p Seen keeps them for lookup.
  void note(std::vector<std::string> &Names, llvm::StringSet<> &Seen,
            const std::string &Name) {
    if (Seen.insert(Name).second)
      Names.push_back(Name);
  }

  /// Gives \p Name the prototype \p Function, as the text declares the
  /// routine of that name, or as another routine's assembler name
  /// (\p Alias) names it.  A routine's own declaration describes its name
  /// whether or not an assembler name gives the name another prototype:
  /// bionic's strtol_l, which takes a locale, is strtol's symbol.  A routine
  /// the text declares again, or two that one assembler name alone names,
  /// must pass and return alike, as EDK II's module kinds do not each
  /// declaring _ModuleEntryPoint; the later of two that do describes it.
  bool declareRoutine(const std::string &Name,
                      std::shared_ptr<const FunctionDef> Function, bool Alias,
                      const Token &At) {
    const bool Own = OwnNames.count(Name);
    if (Alias && Own)
      return true;
    // An earlier declaration of the same standing: the routine's own, or
    // another assembler name's where no own one is.
    if (NotedPrototypes.count(Name) && (Alias || Own))
      if (const FunctionDef *Earlier = Library.findPrototype(Name);
          Earlier && !passesAlike(*Earlier, *Function)) {
        fail(At, "'" + Name + "' is declared again with another type");
        return false;
      }
    if (!Alias)
      OwnNames.insert(Name);
    Library.definePrototype(Name, std::move(Function));
    note(Result.Prototypes, NotedPrototypes, Name);
    return true;
  }

  /// How a call passes or returns a value of \p Type: its class and size,
  /// typedefs seen through, an array or a function as an address.
  std::pair<BuiltinClass, uint64_t> passing(const TypeExprRef &Type) const {
    if (!Type)
      return {BuiltinClass::Void, 0};
    const TypeExpr *At = Type.get();
    for (unsigned Depth = 0;
         Depth < ::neverd::limits::types::kCTypeParserPassingDepthLimit &&
         At->Kind == TypeExprKind::Named;
         ++Depth) {
      const LocalType *Local = Library.find(At->Name);
      if (!Local || Local->Kind != TypeKind::Typedef || !Local->Target)
        break;
      At = Local->Target.get();
    }
    switch (At->Kind) {
    case TypeExprKind::Builtin:
      return {builtinInfo(At->TheBuiltin).Class,
              Library.builtinBytes(At->TheBuiltin)};
    case TypeExprKind::Pointer:
    case TypeExprKind::Array:
    case TypeExprKind::Function:
      // An address, which no builtin class names: Character only tells it
      // from an integer of its size.
      return {BuiltinClass::Character, pointerSize()};
    default:
      // A record or an enumeration, by its size.
      return {BuiltinClass::Void, Library.sizeOf(*At).value_or(0)};
    }
  }

  /// Whether calls that \p A and \p B describe pass and return alike: as
  /// many parameters, each of the same class and size, and returns alike.
  bool passesAlike(const FunctionDef &A, const FunctionDef &B) const {
    if (A.Params.size() != B.Params.size() || A.Variadic != B.Variadic ||
        passing(A.Return) != passing(B.Return))
      return false;
    for (size_t I = 0; I < A.Params.size(); ++I)
      if (passing(A.Params[I].Type) != passing(B.Params[I].Type))
        return false;
    return true;
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
        if (!declareRoutine(Name, Type->Function, /*Alias=*/false, NameToken))
          return;
        // A program that calls the routine imports its assembler name.
        if (!Declared.AsmName.empty() && Declared.AsmName != Name &&
            !declareRoutine(Declared.AsmName, Type->Function, /*Alias=*/true,
                            NameToken))
          return;
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
        !builtinKeyword(Name) && !exactLongDoubleBytes(Name)) {
      LocalType Local;
      Local.Name = Name;
      Local.Kind = Kind;
      Local.Origin = Options.Origin;
      Local.Header = Header;
      // typedef struct { ... } X names the record by X alone.
      Local.TypedefNamed = true;
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
    if (builtinWords().count(Name) || builtinKeyword(Name) ||
        exactLongDoubleBytes(Name))
      return;
    // typedef struct X X names the record by itself, which C may then name
    // by X alone.
    if (Type->Kind == TypeExprKind::Named && Type->Name == Name) {
      SameNamed.insert(Name);
      return;
    }
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
    Local.Header = Header;
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
      } else if ((W == "__ptr32" || W == "__ptr64") && Spec.Type) {
        const TypeExpr *Pointer = Spec.Type.get();
        if (Pointer->Kind == TypeExprKind::Named) {
          const LocalType *Resolved = Library.resolve(Pointer->Name);
          Pointer = Resolved ? Resolved->Target.get() : nullptr;
        }
        if (!Pointer || Pointer->Kind != TypeExprKind::Pointer) {
          fail(T, "a pointer width annotation requires a pointer type");
          return false;
        }
        auto Sized = std::make_shared<TypeExpr>(*Pointer);
        Sized->PointerBytes = W == "__ptr32" ? 4 : 8;
        Spec.Type = std::move(Sized);
        Spec.TypeName = false;
        take();
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
      } else if (typeofWord(W)) {
        if (Spec.Type || AnyWord)
          break;
        // The type of a routine or a type declared before: musl's __REDIR
        // declares a routine again under another assembler name.
        const Token At = take();
        if (!expect("(", "after " + W.str()))
          return false;
        const Token Of = take();
        const FunctionDef *Routine = Of.Kind == TokenKind::Word
                                         ? Library.findPrototype(Of.Text)
                                         : nullptr;
        if (Routine)
          Spec.Type = makeFunction(std::make_shared<FunctionDef>(*Routine));
        else if (Of.Kind == TokenKind::Word && Library.find(Of.Text))
          Spec.Type = named(Of.Text);
        else {
          fail(At, W.str() + " reads the type of a routine or a type declared "
                             "before it, not of an expression");
          return false;
        }
        if (!expect(")", "after the routine"))
          return false;
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
        else if (const std::optional<unsigned> Bytes =
                     exactLongDoubleBytes(W)) {
          // The long double C's prelude declares exactly so large, which
          // only the program's own is.
          const unsigned Own = Library.builtinBytes(Builtin::LongDouble);
          if (*Bytes != Own) {
            fail(T, "'" + W.str() + "' is a long double of " +
                        std::to_string(*Bytes) +
                        " bytes, where the program's "
                        "is of " +
                        std::to_string(Own));
            return false;
          }
          Spec.Type = makeBuiltin(Builtin::LongDouble);
        } else if (vaListWord(W) && !Library.find(W)) {
          // Windows and i386 pass the variable arguments' address; other
          // targets a record of their own, which a parameter passes as its
          // address too: x86-64's decays from an array, and AAPCS64 passes
          // a record that large by reference.
          if (Library.dataModel() == DataModel::LP64 && !InParameter) {
            fail(T, "__builtin_va_list of an LP64 target is a record local "
                    "types do not know");
            return false;
          }
          Spec.Type = makePointer(makeBuiltin(Builtin::Char), pointerSize());
        } else if (Library.find(W)) {
          // A typedef the declarations made, such as glibc's size_t, keeps
          // its name.
          Spec.Type = named(W);
          Spec.TypeName = true;
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
      // Microsoft's unqualified __int8 is char, whose type identity differs
      // from signed char even when both have the same signed representation.
      else if (Words.Char || Words.Int8)
        Type = U              ? Builtin::UChar
               : Words.Signed ? Builtin::SChar
                              : Builtin::Char;
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
            declspecWord(T.Text) || T.Text == "DECLSPEC_ALIGN" ||
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
        // The symbol's name in the object file, its quoted parts joined.
        std::string Name;
        while (!Failed && !accept(")")) {
          if (peek().Kind == TokenKind::End) {
            fail(At, "an assembler name does not end");
            return false;
          }
          const Token Part = take();
          if (Part.Kind == TokenKind::String)
            Name += Part.Text;
        }
        Read.AsmName = std::move(Name);
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
      const bool Gnu = !declspecWord(Word);
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
        note(Result.Types, NotedTypes, Name);
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
    // A pack of the ABI's own limit that lays the record out as it would be
    // anyway, as C NeverD renders for other compilers holds i386's records
    // to four, is no packing of the record's own.
    if (Draft.Pack && Draft.Pack == scalarAlignLimitOf(Library.recordABI())) {
      DraftRecord Loose = Draft;
      Loose.Pack = 0;
      llvm::Expected<RecordDef> Unpacked = layOutRecord(Library, Loose);
      if (Unpacked && sameLayout(*Unpacked, *Laid))
        Laid->Pack = 0;
      else if (!Unpacked)
        llvm::consumeError(Unpacked.takeError());
    }
    auto Def = std::make_shared<RecordDef>(std::move(*Laid));
    if (Name.empty())
      return makeRecord(std::move(Def));
    LocalType Local;
    Local.Name = Name;
    Local.Kind = Kind;
    Local.Origin = Options.Origin;
    Local.Header = Header;
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
      const Token MemberAt = peek();
      Specifiers Spec;
      if (!specifiers(Spec))
        return;
      if (accept(";")) {
        // An anonymous record, including Microsoft's named C extensions.
        if (Spec.Type->Kind == TypeExprKind::Record)
          Draft.Members.push_back({"", Spec.Type, false, 0, ""});
        else if (!Cpp && !Spec.Typedef &&
                 libraryRecordABIOf(Library.recordABI()) ==
                     RecordABI::Microsoft &&
                 Spec.Type->Kind == TypeExprKind::Named) {
          // Microsoft's C admits both typedef and tagged anonymous members,
          // including a reference to an existing tag: struct S;. This embeds
          // the record, and an incomplete tag is an incomplete field error.
          if (const LocalType *Named = Library.resolve(Spec.Type->Name)) {
            if (Named->Record) {
              if (!Named->Record->Defined) {
                fail(MemberAt, "an anonymous member needs a complete record");
                return;
              }
              DraftMember Field;
              Field.Type = qualified(makeRecord(Named->Record),
                                     Spec.Type->Const, Spec.Type->Volatile);
              Field.Alignment = Spec.Asked.Alignment;
              Field.Packed = Spec.Asked.Packed;
              Draft.Members.push_back(std::move(Field));
            }
          }
        }
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
    const unsigned Bits = Library.builtinBytes(Underlying) * 8;
    const bool Signed = builtinInfo(Underlying).Signed;
    int64_t Next = 0;
    while (!Failed && !accept("}")) {
      if (peek().Kind != TokenKind::Word) {
        fail(peek(), "expected an enumerator's name");
        return nullptr;
      }
      Enumerator Value;
      Value.Name = take().Text;
      // Darwin's headers mark an enumerator's availability after its name.
      if (startsAttribute()) {
        Attributes Marks;
        if (!attributes(Marks))
          return nullptr;
      }
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
    Local.Header = Header;
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
    // Microsoft places this access annotation before the pointer too,
    // including after a comma in a list of typedef declarators.
    while (isWord("__unaligned") &&
           (isPunct("*", 1) || isWord("__unaligned", 1)))
      take();
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
        Name += "::";
        Name += take().Text;
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
        // C99 lets a parameter's array qualify the pointer it adjusts to and
        // promise its least size ([restrict], [static 4], bionic's
        // [_Nonnull 2]), which change no layout local types hold.
        while (peek().Kind == TokenKind::Word &&
               (constWord(peek().Text) || volatileWord(peek().Text) ||
                ignoredWord(peek().Text)))
          take();
        // A parameter's array adjusts to a pointer, so its size may name
        // another parameter (C99's regexec takes `__pmatch[__nmatch]`).
        if (InParameter && peek().Kind == TokenKind::Word &&
            !Enumerators.count(peek().Text) && isPunct("]", 1))
          take();
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
    const auto Convention =
        PendingConvention ? PendingConvention : DefaultConvention;
    PendingConvention.reset();
    if (Convention) {
      // A convention the target ignores, as 64-bit x86 does __stdcall, is
      // no convention of the function's.
      if (pointerSize() < 8 || keptWide(*Convention))
        Function->Convention = *Convention;
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
      // The parameter's type and declarator are a parameter's.
      const bool Enclosing = InParameter;
      InParameter = true;
      auto Leave = llvm::make_scope_exit([&] { InParameter = Enclosing; });
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
      const llvm::StringRef Text = take().Text;
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
          const unsigned Bits = Library.builtinBytes(Type->TheBuiltin) * 8;
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
        // The size of a string literal is its units and the terminator's,
        // as INTERNET_MAX_URL_LENGTH counts sizeof("://") and EDK II's Base.h
        // sizeof(L"A").
        if (At.Text == "sizeof" &&
            (peek().Kind == TokenKind::String ||
             (isPunct("(", 0) && peek(1).Kind == TokenKind::String))) {
          const bool Parenthesized = accept("(");
          // Adjacent literals are one, of the prefix one of them has.
          std::string Text;
          Encoding Quoted = Encoding::Plain;
          while (peek().Kind == TokenKind::String) {
            const Token Part = take();
            if (Part.Quoted != Encoding::Plain) {
              if (Quoted != Encoding::Plain && Quoted != Part.Quoted) {
                fail(Part, "adjacent string literals of two encodings");
                return std::nullopt;
              }
              Quoted = Part.Quoted;
            }
            Text += Part.Text;
          }
          if (Parenthesized && !expect(")", "after a string"))
            return std::nullopt;
          return int64_t((unitsOf(Text, Quoted) + 1) * unitBytes(Quoted));
        }
        // A character constant is an int in C; a prefixed one is of its
        // encoding's character type, as EDK II's sizeof(L'A').
        if (At.Text == "sizeof" &&
            ((peek().Kind == TokenKind::Number && peek().Character) ||
             (isPunct("(", 0) && peek(1).Kind == TokenKind::Number &&
              peek(1).Character && isPunct(")", 2)))) {
          const bool Parenthesized = accept("(");
          const Token Character = take();
          if (Parenthesized && !expect(")", "after a character"))
            return std::nullopt;
          return int64_t(Character.Quoted == Encoding::Plain
                             ? Library.builtinBytes(Builtin::Int)
                             : unitBytes(Character.Quoted));
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
    TypeExprRef Pointer = typeInParentheses();
    if (!Pointer)
      return std::nullopt;
    if (Pointer->Kind == TypeExprKind::Named)
      if (const LocalType *Named = Library.resolve(Pointer->Name);
          Named && Named->Kind == TypeKind::Typedef && Named->Target)
        Pointer = Named->Target;
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
    if (!expect("(", "after " + At.Text.str()))
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
  /// The routines the text declares by their own names, which another's
  /// assembler name does not describe.
  llvm::StringSet<> OwnNames;
  const bool Cpp;
  bool Failed = false;
  /// Open extern "C" and namespace blocks.
  unsigned Blocks = 0;
  std::optional<CallingConvention> DefaultConvention;
  std::optional<CallingConvention> PendingConvention;
  /// What the attributes of the declarator being read say.
  Attributes Declared;
  /// Whether the declarator being read is a parameter's.
  bool InParameter = false;
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
  llvm::BumpPtrAllocator Storage;
  llvm::StringSaver Spellings(Storage);
  std::vector<Token> Tokens = Lexer(Source, Result.Problems, Spellings).run();
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
  llvm::BumpPtrAllocator Storage;
  llvm::StringSaver Spellings(Storage);
  std::vector<Token> Tokens = Lexer(Text, Result.Problems, Spellings).run();
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
