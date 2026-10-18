//===- CSyntaxParser.cpp - Reading the C NeverD emits ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A recursive-descent reader for emitted C: declarations with C's
/// declarators, the statements HighC writes (including the SEH, C++ and
/// assembly pseudo-statements it prints for code C cannot express) and C's
/// expressions by precedence climbing.  A declaration it cannot read is kept
/// as text with the reason, and reading goes on after it.
///
//===----------------------------------------------------------------------===//

#include "CSyntaxTree.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"

using namespace neverd;
using namespace neverd::csyntax;

namespace {

/// Nesting deeper than this is refused rather than read on a stack it could
/// exhaust; emitted lane assembly reaches about 200.
constexpr unsigned MaxNesting = 1024;

/// The typedef names the headers emitted C includes declare.
const llvm::StringSet<> &headerTypeNames() {
  static const llvm::StringSet<> Names = [] {
    llvm::StringSet<> Result;
#define NEVERD_C_HEADER_INTEGER(Name, Bits, Signed) Result.insert(Name);
#define NEVERD_C_HEADER_NAMED_TYPE(Name, Bits) Result.insert(Name);
#include "neverd/backend/c/dialect/CHeaderNames.def"
    return Result;
  }();
  return Names;
}

int binaryPrecedence(llvm::StringRef Op) {
  return llvm::StringSwitch<int>(Op)
      .Cases({"*", "/", "%"}, 10)
      .Cases({"+", "-"}, 9)
      .Cases({"<<", ">>"}, 8)
      .Cases({"<", ">", "<=", ">="}, 7)
      .Cases({"==", "!="}, 6)
      .Case("&", 5)
      .Case("^", 4)
      .Case("|", 3)
      .Case("&&", 2)
      .Case("||", 1)
      .Default(0);
}

bool isAssignmentOperator(llvm::StringRef Op) {
  return llvm::StringSwitch<bool>(Op)
      .Cases({"=", "*=", "/=", "%=", "+=", "-="}, true)
      .Cases({"<<=", ">>=", "&=", "^=", "|="}, true)
      .Default(false);
}

struct Specifiers {
  size_t Begin = 0;
  const CType *Base = nullptr;
  bool Typedef = false;
  bool Extern = false;
  bool Static = false;
  bool Inline = false;
  bool Register = false;
  bool Noreturn = false;
  bool Const = false;
  bool Volatile = false;
  unsigned AddressSpace = 0;
  std::optional<uint64_t> AlignAs;
  llvm::SmallVector<llvm::StringRef, 1> Attributes;
  /// The enumerators an enumeration the specifiers define declares.
  std::vector<Decl *> Enumerators;
};

/// What a declarator adds to the type its specifiers name.
struct Declarator {
  llvm::StringRef Name;
  size_t NameOffset = 0;
  /// The parameters of the function the name declares, when it declares one.
  std::vector<Decl *> Params;
  std::optional<size_t> UnprototypedParams;
  bool DeclaresFunction = false;
  /// Applies the declarator to the specifiers' type.
  std::function<const CType *(const CType *)> Apply;
};

class Parser {
public:
  Parser(Tree &Nodes, llvm::StringRef Source, std::vector<TopLevel> &Out,
         std::vector<Token> AllTokens)
      : T(Nodes), Src(Source), Items(Out) {
    for (const Token &Tok : AllTokens) {
      if (Tok.Kind == TokenKind::Comment) {
        Comment C;
        C.Text = Tok.Text;
        C.Offset = Tok.Offset;
        const size_t LineBegin = Src.rfind('\n', Tok.Offset);
        const size_t From =
            LineBegin == llvm::StringRef::npos ? 0 : LineBegin + 1;
        C.OwnLine = Src.slice(From, Tok.Offset).trim().empty();
        Comments.push_back(C);
        CommentNext.push_back(Toks.size());
        continue;
      }
      Toks.push_back(Tok);
    }
    for (llvm::StringRef Name : headerTypeNames().keys())
      TypedefNames.insert(Name);
  }

  void run() {
    while (!atEnd()) {
      for (const Comment &C : takeCommentsBefore(Pos)) {
        TopLevel Item;
        Item.TheKind = TopLevel::Kind::Comment;
        Item.Begin = C.Offset;
        Item.End = C.Offset + C.Text.size();
        Item.TheComment = C;
        Items.push_back(std::move(Item));
      }
      if (atEnd())
        break;
      if (tok().Kind == TokenKind::Directive) {
        TopLevel Item;
        Item.TheKind = TopLevel::Kind::Directive;
        Item.Begin = tok().Offset;
        Item.End = tok().Offset + tok().Text.size();
        Item.Text = tok().Text;
        if (!packDirective(Item.Text))
          Pending = "a #pragma pack this reader does not read";
        ++Pos;
        Item.Trailing = takeSameLineComments();
        Items.push_back(std::move(Item));
        continue;
      }
      const size_t Start = Pos;
      const size_t FirstComment = CommentCursor;
      Error.clear();
      Nesting = 0;
      AllowUnnamed = false;
      // A declaration under a packing this reader does not follow is not
      // read: its layout would not be C's.
      if (!Pending.empty())
        fail(Pending);
      TopLevel Item = Error.empty() ? parseExternalDeclaration() : TopLevel();
      if (!Error.empty()) {
        // Keep the declaration as written and read on after it.
        Pos = Start;
        CommentCursor = FirstComment;
        const size_t End = skipDeclaration();
        TopLevel Unread;
        Unread.TheKind = TopLevel::Kind::Unread;
        Unread.Begin = Toks[Start].Offset;
        Unread.End = End;
        Unread.Text = Src.slice(Unread.Begin, Unread.End);
        Unread.Reason = Error;
        while (CommentCursor < Comments.size() &&
               Comments[CommentCursor].Offset < End)
          ++CommentCursor;
        Unread.Trailing = takeSameLineComments();
        Items.push_back(std::move(Unread));
        continue;
      }
      Items.push_back(std::move(Item));
    }
    for (const Comment &C : takeCommentsBefore(Toks.size())) {
      TopLevel Item;
      Item.TheKind = TopLevel::Kind::Comment;
      Item.Begin = C.Offset;
      Item.End = C.Offset + C.Text.size();
      Item.TheComment = C;
      Items.push_back(std::move(Item));
    }
  }

private:
  //===--- Tokens and comments ---===//

  const Token &tok(size_t Ahead = 0) const {
    return Toks[std::min(Pos + Ahead, Toks.size() - 1)];
  }
  bool atEnd() const { return tok().Kind == TokenKind::End; }
  bool isPunct(llvm::StringRef S, size_t Ahead = 0) const {
    return tok(Ahead).isPunctuator(S);
  }
  bool isIdent(llvm::StringRef S, size_t Ahead = 0) const {
    return tok(Ahead).is(TokenKind::Identifier, S);
  }
  size_t offset() const { return tok().Offset; }
  /// The end of the last token read.
  size_t lastEnd() const {
    if (Pos == 0)
      return 0;
    const Token &Last = Toks[Pos - 1];
    return Last.Offset + Last.Text.size();
  }
  bool accept(llvm::StringRef S) {
    if (!isPunct(S))
      return false;
    ++Pos;
    return true;
  }
  bool expect(llvm::StringRef S) {
    if (accept(S))
      return true;
    return fail("expected '" + S + "'");
  }
  bool fail(const llvm::Twine &What) {
    if (Error.empty()) {
      Error = What.str();
      if (!atEnd())
        Error += " at '" + tok().Text.take_front(24).str() + "'";
      Error += " (byte " + std::to_string(offset()) + ")";
    }
    return false;
  }
  bool failed() const { return !Error.empty(); }

  CommentList takeCommentsBefore(size_t TokenIndex) {
    CommentList Result;
    while (CommentCursor < Comments.size() &&
           CommentNext[CommentCursor] <= TokenIndex)
      Result.push_back(Comments[CommentCursor++]);
    return Result;
  }
  /// The comments after the last token read that share its line.
  CommentList takeSameLineComments() {
    CommentList Result;
    size_t From = lastEnd();
    while (CommentCursor < Comments.size() &&
           CommentNext[CommentCursor] <= Pos) {
      const Comment &C = Comments[CommentCursor];
      if (Src.slice(From, C.Offset).contains('\n'))
        break;
      Result.push_back(C);
      From = C.Offset + C.Text.size();
      ++CommentCursor;
    }
    return Result;
  }
  /// The comments between the last token read and the next one, which an
  /// expression keeps after itself.
  void attachTrailing(Expr *E) {
    while (CommentCursor < Comments.size() &&
           CommentNext[CommentCursor] == Pos &&
           Comments[CommentCursor].Offset >= E->End &&
           !Src.slice(E->End, Comments[CommentCursor].Offset).contains('\n'))
      E->Trailing.push_back(Comments[CommentCursor++]);
  }

  /// The end of the declaration starting at Pos when it cannot be read: after
  /// its `;`, after a function body's `}`, or after a record's `};`.
  size_t skipDeclaration() {
    unsigned Depth = 0;
    while (!atEnd()) {
      const Token &Tok = tok();
      if (Tok.Kind == TokenKind::Directive && Depth == 0)
        return Toks[Pos - 1].Offset + Toks[Pos - 1].Text.size();
      if (Tok.isPunctuator("(") || Tok.isPunctuator("["))
        ++Depth;
      else if ((Tok.isPunctuator(")") || Tok.isPunctuator("]")) && Depth)
        --Depth;
      else if (Tok.isPunctuator(";") && Depth == 0) {
        ++Pos;
        return lastEnd();
      } else if (Tok.isPunctuator("{") && Depth == 0) {
        const bool Initializer = Pos > 0 && Toks[Pos - 1].isPunctuator("=");
        unsigned Braces = 0;
        do {
          if (tok().isPunctuator("{"))
            ++Braces;
          else if (tok().isPunctuator("}"))
            --Braces;
          ++Pos;
        } while (Braces && !atEnd());
        if (Initializer)
          continue;
        if (isPunct(";"))
          ++Pos;
        return lastEnd();
      }
      ++Pos;
    }
    return lastEnd();
  }

  //===--- Types ---===//

  bool isTypeNameStart(size_t Ahead = 0) const {
    const Token &Tok = tok(Ahead);
    if (Tok.Kind != TokenKind::Identifier)
      return false;
    return llvm::StringSwitch<bool>(Tok.Text)
        .Cases({"void", "char", "short", "int", "long", "float", "double"},
               true)
        .Cases({"signed", "unsigned", "_Bool", "bool", "__int128", "_BitInt"},
               true)
        .Cases(
            {"const", "volatile", "restrict", "__restrict", "struct", "union"},
            true)
        .Case("enum", true)
        .Cases({"__attribute__", "_Atomic", "_Complex"}, true)
        .Default(TypedefNames.contains(Tok.Text) && !shadowed(Tok.Text));
  }
  /// Whether a block declares typedef name \p Name as an ordinary identifier,
  /// which hides the type to the end of that block.
  bool shadowed(llvm::StringRef Name) const {
    return llvm::any_of(ShadowScopes, [&](const llvm::StringSet<> &Scope) {
      return Scope.contains(Name);
    });
  }
  bool isDeclarationStart() const {
    if (isTypeNameStart())
      return true;
    const Token &Tok = tok();
    return Tok.Kind == TokenKind::Identifier &&
           llvm::StringSwitch<bool>(Tok.Text)
               .Cases({"typedef", "extern", "static", "inline", "register"},
                      true)
               .Cases({"_Noreturn", "_Alignas", "_Thread_local", "auto"}, true)
               .Cases({"__inline", "__inline__", "__declspec"}, true)
               .Cases({"__stdcall", "__cdecl", "__fastcall", "__thiscall"},
                      true)
               .Default(false);
  }

  /// `__attribute__((...))`, kept as written; `address_space(N)` also
  /// qualifies the type.
  bool parseAttribute(llvm::SmallVectorImpl<llvm::StringRef> &Attributes,
                      unsigned *AddressSpace) {
    const size_t Begin = offset();
    ++Pos;
    if (!isPunct("("))
      return fail("expected '(' after __attribute__");
    unsigned Depth = 0;
    do {
      if (isPunct("("))
        ++Depth;
      else if (isPunct(")"))
        --Depth;
      else if (AddressSpace && isIdent("address_space") && isPunct("(", 1) &&
               tok(2).Kind == TokenKind::Number) {
        uint64_t Space = 0;
        if (!tok(2).Text.getAsInteger(0, Space))
          *AddressSpace = static_cast<unsigned>(Space);
      }
      if (atEnd())
        return fail("unterminated __attribute__");
      ++Pos;
    } while (Depth);
    Attributes.push_back(Src.slice(Begin, lastEnd()));
    return true;
  }

  bool parseSpecifiers(Specifiers &S) {
    S.Begin = offset();
    unsigned Long = 0, Short = 0, Int = 0, Char = 0, Signed = 0, Unsigned = 0;
    unsigned Void = 0, Bool = 0, Float = 0, Double = 0, Int128 = 0;
    std::optional<unsigned> BitInt;
    const CType *Named = nullptr;
    auto AnyTypeKeyword = [&] {
      return Long || Short || Int || Char || Signed || Unsigned || Void ||
             Bool || Float || Double || Int128 || BitInt || Named;
    };
    while (!atEnd()) {
      const Token &Tok = tok();
      if (Tok.Kind != TokenKind::Identifier)
        break;
      const llvm::StringRef W = Tok.Text;
      if (W == "typedef")
        S.Typedef = true;
      else if (W == "extern")
        S.Extern = true;
      else if (W == "static")
        S.Static = true;
      else if (W == "inline" || W == "__inline" || W == "__inline__")
        S.Inline = true;
      else if (W == "register")
        S.Register = true;
      else if (W == "_Noreturn")
        S.Noreturn = true;
      else if (W == "auto" || W == "_Thread_local" || W == "restrict" ||
               W == "__restrict" || W == "_Atomic")
        ;
      else if (W == "const")
        S.Const = true;
      else if (W == "volatile")
        S.Volatile = true;
      else if (W == "__stdcall" || W == "__cdecl" || W == "__fastcall" ||
               W == "__thiscall")
        S.Attributes.push_back(W);
      else if (W == "__attribute__" || W == "__declspec") {
        if (!parseAttribute(S.Attributes, &S.AddressSpace))
          return false;
        continue;
      } else if (W == "_Alignas") {
        ++Pos;
        if (!expect("("))
          return false;
        Expr *E = parseConditional();
        if (!E || !expect(")"))
          return false;
        uint64_t Align = 0;
        if (E->Kind != ExprKind::Integer || E->Text.getAsInteger(0, Align))
          return fail("_Alignas needs an integer");
        S.AlignAs = Align;
        continue;
      } else if (W == "void")
        ++Void;
      else if (W == "_Bool" || W == "bool")
        ++Bool;
      else if (W == "char")
        ++Char;
      else if (W == "short")
        ++Short;
      else if (W == "int")
        ++Int;
      else if (W == "long")
        ++Long;
      else if (W == "signed" || W == "__signed__")
        ++Signed;
      else if (W == "unsigned")
        ++Unsigned;
      else if (W == "float")
        ++Float;
      else if (W == "double")
        ++Double;
      else if (W == "__int128")
        ++Int128;
      else if (W == "_BitInt") {
        ++Pos;
        uint64_t Width = 0;
        if (!expect("(") || tok().Kind != TokenKind::Number ||
            tok().Text.getAsInteger(0, Width) || Width == 0 || Width > 65535)
          return fail("_BitInt needs a width");
        ++Pos;
        if (!expect(")"))
          return false;
        BitInt = static_cast<unsigned>(Width);
        continue;
      } else if (W == "struct" || W == "union") {
        const bool Union = W == "union";
        ++Pos;
        // DECLSPEC_ALIGN(N), as the Windows headers align a record.
        uint64_t Alignment = 0;
        if (isIdent("DECLSPEC_ALIGN")) {
          ++Pos;
          if (!expect("(") || tok().Kind != TokenKind::Number ||
              tok().Text.getAsInteger(0, Alignment) || !Alignment)
            return fail("DECLSPEC_ALIGN needs an alignment");
          ++Pos;
          if (!expect(")"))
            return false;
        }
        // CTypeRender places this exact bit-field layout marker after the
        // optional alignment and before the tag (also for anonymous records).
        // The Local Types rendering owns the layout of the foreign views.
        if (isIdent("NEVERD_MS_STRUCT"))
          ++Pos;
        if (tok().Kind != TokenKind::Identifier) {
          // A structure or union without a tag, as a record's member.
          if (!AnonymousRecords || !isPunct("{"))
            return fail("expected a record name");
          Named = T.Types.anonymousRecord(Union);
          if (!parseRecordBody(Named->Spelling, Union, Alignment))
            return false;
          continue;
        }
        const std::string Spelling =
            ((Union ? "union " : "struct ") + tok().Text).str();
        ++Pos;
        Named = T.Types.record(Spelling);
        if (isPunct("{") && !parseRecordBody(Spelling, Union, Alignment))
          return false;
        continue;
      } else if (W == "enum") {
        ++Pos;
        if (tok().Kind != TokenKind::Identifier)
          return fail("expected an enumeration name");
        Named = T.Types.enumeration(tok().Text);
        ++Pos;
        // C23's fixed underlying type (`enum E : uint32_t`), which the
        // enumeration's spelling keeps as written.
        if (isPunct(":") && (isTypeNameStart(1) || isIdent("unsigned", 1))) {
          ++Pos;
          Specifiers Underlying;
          if (!parseSpecifiers(Underlying))
            return false;
        }
        if (isPunct("{")) {
          if (!EnumerationsDefined)
            return fail("an enumeration defined within another declaration");
          if (!parseEnumerators(Named, S.Enumerators))
            return false;
        }
        continue;
      } else if (!AnyTypeKeyword() && TypedefNames.contains(W) &&
                 // A name before `:` is a label, but in a record an
                 // unnamed bit-field's type.
                 (!isPunct(":", 1) || AnonymousRecords)) {
        Named = typedefType(W);
        if (!Named)
          return fail("unknown type '" + W + "'");
      } else
        break;
      ++Pos;
    }
    if (failed())
      return false;
    if (!AnyTypeKeyword())
      return fail("expected a type");

    const CDataModel &M = T.Types.model();
    const CType *Base = nullptr;
    const bool IsUnsigned = Unsigned > 0;
    if (Named)
      Base = Named;
    else if (Void)
      Base = T.Types.voidType();
    else if (Bool)
      Base = T.Types.boolType();
    else if (BitInt)
      Base = T.Types.integer(*BitInt, !IsUnsigned,
                             (IsUnsigned ? "unsigned _BitInt(" : "_BitInt(") +
                                 std::to_string(*BitInt) + ")",
                             /*BitPrecise=*/true);
    else if (Int128)
      Base = T.Types.integer(128, !IsUnsigned,
                             IsUnsigned ? "unsigned __int128" : "__int128");
    else if (Float)
      Base = T.Types.floating(32, "float");
    else if (Double)
      Base = Long ? T.Types.floating(M.LongDoubleBits, "long double")
                  : T.Types.doubleType();
    else if (Char)
      Base = Signed     ? T.Types.integer(8, true, "signed char")
             : Unsigned ? T.Types.integer(8, false, "unsigned char")
                        : T.Types.charType();
    else if (Short)
      Base = T.Types.integer(16, !IsUnsigned,
                             IsUnsigned ? "unsigned short" : "short");
    else if (Long >= 2)
      Base = T.Types.integer(64, !IsUnsigned,
                             IsUnsigned ? "unsigned long long" : "long long");
    else if (Long == 1)
      Base = T.Types.integer(M.LongBits, !IsUnsigned,
                             IsUnsigned ? "unsigned long" : "long");
    else
      Base = IsUnsigned ? T.Types.unsignedIntType() : T.Types.intType();
    S.Base = T.Types.qualified(Base, S.Const, S.Volatile, S.AddressSpace);
    return true;
  }

  const CType *typedefType(llvm::StringRef Name) {
    if (auto It = Typedefs.find(Name); It != Typedefs.end())
      return It->second;
    const CDataModel &M = T.Types.model();
    const CType *Result = nullptr;
#define NEVERD_C_HEADER_INTEGER(HeaderName, Bits, Signed)                      \
  if (!Result && Name == HeaderName)                                           \
    Result = T.Types.integer((Bits) ? (Bits) : M.PointerBits, Signed, Name);
#define NEVERD_C_HEADER_NAMED_TYPE(HeaderName, Bits)                           \
  if (!Result && Name == HeaderName)                                           \
    Result = T.Types.named(Name, Bits);
#include "neverd/backend/c/dialect/CHeaderNames.def"
    if (Result && Name == "wchar_t")
      Result = T.Types.integer(M.WCharBits, M.WCharSigned, Name);
    return Result;
  }

  /// `{ A = 1, B, ... }` after `enum Name`.
  bool parseEnumerators(const CType *Enumeration, std::vector<Decl *> &Out) {
    ++Pos;
    while (!accept("}")) {
      if (tok().Kind != TokenKind::Identifier)
        return fail("expected an enumerator");
      Decl *D = T.newDecl(DeclKind::Enumerator, offset());
      D->Name = tok().Text;
      D->Ty = Enumeration;
      ++Pos;
      if (accept("=") && !(D->Init = parseConditional()))
        return false;
      D->End = lastEnd();
      Out.push_back(D);
      if (!accept(",") && !isPunct("}"))
        return fail("expected ',' or '}'");
    }
    return true;
  }

  /// `{ T field_0; ... }` after `struct Name` or `union Name`: members,
  /// bit-fields and anonymous structures and unions.
  bool parseRecordBody(llvm::StringRef Spelling, bool Union,
                       uint64_t Alignment) {
    RecordDef *Record = T.Types.recordDef(Spelling);
    if (Record->Complete)
      return fail("record defined twice");
    Record->Union = Union;
    Record->Alignment = Alignment;
    Record->Pack = Pack;
    ++Pos;
    while (!accept("}")) {
      if (atEnd())
        return fail("unterminated record");
      takeCommentsBefore(Pos);
      Specifiers S;
      const bool Outer = AnonymousRecords;
      AnonymousRecords = true;
      const bool Read = parseSpecifiers(S);
      AnonymousRecords = Outer;
      if (!Read)
        return false;
      // An anonymous structure or union, whose members are the record's.
      if (accept(";")) {
        if (!S.Base->isRecord() || !S.Base->Record->Anonymous)
          return fail("expected a field name");
        Record->Fields.push_back({"", S.Base, std::nullopt});
        continue;
      }
      do {
        RecordField Field;
        Field.Type = S.Base;
        // An unnamed bit-field pads, or with width 0 ends its unit.
        if (!isPunct(":")) {
          Declarator D;
          if (!parseDeclarator(D, /*Abstract=*/false))
            return false;
          if (D.Name.empty())
            return fail("expected a field name");
          Field.Name = D.Name.str();
          Field.Type = D.Apply(S.Base);
        }
        if (accept(":")) {
          uint64_t Width = 0;
          if (tok().Kind != TokenKind::Number ||
              tok().Text.getAsInteger(0, Width) || Width > 128 ||
              !Field.Type->isInteger())
            return fail("a bit-field needs an integer type and a width");
          ++Pos;
          Field.BitWidth = static_cast<unsigned>(Width);
        }
        Record->Fields.push_back(std::move(Field));
      } while (accept(","));
      if (!expect(";"))
        return false;
    }
    Record->Complete = true;
    return true;
  }

  /// Whether `(` at Pos opens a nested declarator rather than parameters.
  bool opensNestedDeclarator(bool Abstract) const {
    if (!isPunct("("))
      return false;
    const Token &Next = tok(1);
    if (Next.isPunctuator("*") || Next.isPunctuator("("))
      return true;
    if (Next.is(TokenKind::Identifier, "__attribute__") ||
        Next.is(TokenKind::Identifier, "__stdcall") ||
        Next.is(TokenKind::Identifier, "__cdecl") ||
        Next.is(TokenKind::Identifier, "__fastcall") ||
        Next.is(TokenKind::Identifier, "__thiscall"))
      return true;
    return !Abstract && Next.Kind == TokenKind::Identifier &&
           !isTypeNameStart(1);
  }

  bool parseDeclarator(Declarator &D, bool Abstract) {
    if (++Nesting > MaxNesting)
      return fail("declarator nested too deeply");
    struct PointerLevel {
      bool Const = false, Volatile = false;
      unsigned AddressSpace = 0;
    };
    llvm::SmallVector<PointerLevel, 2> Pointers;
    llvm::SmallVector<llvm::StringRef, 1> Ignored;
    while (true) {
      // A calling convention between the base type and `*`.
      if (tok().Kind == TokenKind::Identifier &&
          (tok().Text == "__stdcall" || tok().Text == "__cdecl" ||
           tok().Text == "__fastcall" || tok().Text == "__thiscall")) {
        ++Pos;
        continue;
      }
      if (isIdent("__attribute__")) {
        unsigned Space = 0;
        if (!parseAttribute(Ignored, &Space))
          return false;
        if (Space && !Pointers.empty())
          Pointers.back().AddressSpace = Space;
        continue;
      }
      if (!accept("*"))
        break;
      PointerLevel Level;
      while (tok().Kind == TokenKind::Identifier) {
        if (tok().Text == "const")
          Level.Const = true;
        else if (tok().Text == "volatile")
          Level.Volatile = true;
        else if (tok().Text == "restrict" || tok().Text == "__restrict")
          ;
        else if (tok().Text == "__attribute__") {
          if (!parseAttribute(Ignored, &Level.AddressSpace))
            return false;
          continue;
        } else
          break;
        ++Pos;
      }
      Pointers.push_back(Level);
    }
    std::optional<Declarator> Inner;
    if (opensNestedDeclarator(Abstract)) {
      ++Pos;
      Inner.emplace();
      if (!parseDeclarator(*Inner, Abstract) || !expect(")"))
        return false;
      D.Name = Inner->Name;
      D.NameOffset = Inner->NameOffset;
    } else if (tok().Kind == TokenKind::Identifier &&
               // Specifiers that name a type come first, so a typedef name
               // here is what the declaration declares (`int32_t addrinfo;`,
               // `typedef uint32_t size_t;`).
               (!isTypeNameStart() ||
                (!Abstract && TypedefNames.contains(tok().Text))) &&
               !isIdent("__asm__") && !isIdent("__asm") &&
               !isIdent("__attribute__")) {
      if (Abstract)
        return fail("unexpected name in a type");
      D.Name = tok().Text;
      D.NameOffset = offset();
      ++Pos;
    } else if (!Abstract && !AllowUnnamed)
      return fail("expected a declarator");

    struct Suffix {
      bool Function = false;
      size_t ParamsOffset = 0;
      std::optional<uint64_t> Count;
      std::vector<const CType *> Params;
      std::vector<Decl *> ParamDecls;
      bool Variadic = false, Prototyped = true;
    };
    std::vector<Suffix> Suffixes;
    while (true) {
      if (accept("[")) {
        Suffix S;
        if (!accept("]")) {
          Expr *E = parseConditional();
          if (!E || !expect("]"))
            return false;
          uint64_t Count = 0;
          if (E->Kind != ExprKind::Integer ||
              E->Text.rtrim("uUlL").getAsInteger(0, Count))
            return fail("array bounds must be integers");
          S.Count = Count;
        }
        Suffixes.push_back(std::move(S));
        continue;
      }
      if (isPunct("(")) {
        Suffix S;
        S.ParamsOffset = offset();
        ++Pos;
        S.Function = true;
        if (!parseParameters(S.Params, S.ParamDecls, S.Variadic, S.Prototyped))
          return false;
        Suffixes.push_back(std::move(S));
        continue;
      }
      break;
    }
    if (!Inner) {
      for (const Suffix &S : Suffixes)
        if (S.Function) {
          D.Params = S.ParamDecls;
          if (!S.Prototyped)
            D.UnprototypedParams = S.ParamsOffset;
          D.DeclaresFunction = true;
          break;
        }
    } else {
      D.Params = Inner->Params;
      D.UnprototypedParams = Inner->UnprototypedParams;
      D.DeclaresFunction = Inner->DeclaresFunction;
    }
    TypeContext &Types = T.Types;
    auto InnerApply =
        Inner ? Inner->Apply : std::function<const CType *(const CType *)>();
    D.Apply = [&Types, Pointers, Suffixes,
               InnerApply](const CType *Base) -> const CType * {
      const CType *Ty = Base;
      for (const PointerLevel &Level : Pointers)
        Ty = Types.qualified(Types.pointerTo(Ty), Level.Const, Level.Volatile,
                             Level.AddressSpace);
      for (auto It = Suffixes.rbegin(); It != Suffixes.rend(); ++It)
        Ty = It->Function
                 ? Types.function(Ty, It->Params, It->Variadic, It->Prototyped)
                 : Types.arrayOf(Ty, It->Count);
      return InnerApply ? InnerApply(Ty) : Ty;
    };
    --Nesting;
    return true;
  }

  /// After `(`: `void`, nothing, or declarations, through `)`.
  bool parseParameters(std::vector<const CType *> &Params,
                       std::vector<Decl *> &Decls, bool &Variadic,
                       bool &Prototyped) {
    if (accept(")")) {
      Prototyped = false;
      return true;
    }
    if (isIdent("void") && isPunct(")", 1)) {
      Pos += 2;
      return true;
    }
    while (true) {
      if (accept("...")) {
        Variadic = true;
        return expect(")");
      }
      Specifiers S;
      if (!parseSpecifiers(S))
        return false;
      Declarator D;
      const bool SavedUnnamed = AllowUnnamed;
      AllowUnnamed = true;
      const bool Ok = parseDeclarator(D, /*Abstract=*/false);
      AllowUnnamed = SavedUnnamed;
      if (!Ok)
        return false;
      const CType *Ty = D.Apply(S.Base);
      // Parameters of array and function type are pointers (C17 6.7.6.3).
      if (Ty->isArray())
        Ty = T.Types.pointerTo(Ty->Inner);
      else if (Ty->isFunction())
        Ty = T.Types.pointerTo(Ty);
      Decl *P = T.newDecl(DeclKind::Parameter, S.Begin);
      P->Name = D.Name;
      P->Ty = Ty;
      P->End = lastEnd();
      Params.push_back(T.Types.unqualified(Ty));
      Decls.push_back(P);
      if (accept(")"))
        return true;
      if (!expect(","))
        return false;
    }
  }

  const CType *parseTypeName() {
    Specifiers S;
    if (!parseSpecifiers(S))
      return nullptr;
    Declarator D;
    if (!parseDeclarator(D, /*Abstract=*/true))
      return nullptr;
    return D.Apply(S.Base);
  }

  //===--- Declarations ---===//

  /// `__asm__("label")` and attributes after a declarator.
  bool parseDeclaratorTail(Decl *D) {
    while (true) {
      if (isIdent("__asm__") || isIdent("__asm") || isIdent("asm")) {
        ++Pos;
        if (!expect("(") || tok().Kind != TokenKind::String)
          return fail("expected an assembler label");
        D->AsmLabel = tok().Text;
        ++Pos;
        if (!expect(")"))
          return false;
        continue;
      }
      if (isIdent("__attribute__")) {
        if (!parseAttribute(D->Attributes, nullptr))
          return false;
        continue;
      }
      return true;
    }
  }

  Decl *declaratorDecl(const Specifiers &S, Declarator &D) {
    const CType *Ty = D.Apply(S.Base);
    DeclKind Kind = S.Typedef          ? DeclKind::Typedef
                    : Ty->isFunction() ? DeclKind::Function
                                       : DeclKind::Variable;
    Decl *Result = T.newDecl(Kind, S.Begin);
    Result->Name = D.Name;
    Result->Ty = Ty;
    if (Kind == DeclKind::Function)
      Result->UnprototypedParams = D.UnprototypedParams;
    Result->Extern = S.Extern;
    Result->Static = S.Static;
    Result->Inline = S.Inline;
    Result->Register = S.Register;
    Result->Noreturn = S.Noreturn;
    Result->AlignAs = S.AlignAs;
    Result->Attributes = S.Attributes;
    if (Kind == DeclKind::Function)
      Result->Params = D.Params;
    if (Kind == DeclKind::Typedef && !D.Name.empty()) {
      TypedefNames.insert(D.Name);
      Typedefs[D.Name] = T.Types.spelled(Ty, D.Name);
      if (!ShadowScopes.empty())
        ShadowScopes.back().erase(D.Name);
    } else if (!D.Name.empty() && !ShadowScopes.empty() &&
               TypedefNames.contains(D.Name)) {
      ShadowScopes.back().insert(D.Name);
    }
    return Result;
  }

  /// One declaration statement after its specifiers: declarators with
  /// labels, attributes and initializers, through `;`.
  bool parseDeclarators(const Specifiers &S, std::vector<Decl *> &Out) {
    if (accept(";")) {
      // `struct Name { ... };`, or `enum Name { ... };`.
      Decl *Record = T.newDecl(S.Enumerators.empty() ? DeclKind::Record
                                                     : DeclKind::Enumeration,
                               S.Begin);
      Record->Ty = S.Base;
      Record->Fields = S.Enumerators;
      Record->End = lastEnd();
      Out.push_back(Record);
      return true;
    }
    if (!S.Enumerators.empty())
      return fail("an enumeration defined within another declaration");
    do {
      Declarator D;
      if (!parseDeclarator(D, /*Abstract=*/false))
        return false;
      Decl *Result = declaratorDecl(S, D);
      if (!parseDeclaratorTail(Result))
        return false;
      if (accept("=")) {
        Result->Init = parseInitializer();
        if (!Result->Init)
          return false;
      }
      Result->End = lastEnd();
      Out.push_back(Result);
    } while (accept(","));
    return expect(";");
  }

  Expr *parseInitializer() {
    if (!isPunct("{"))
      return parseAssignment();
    Expr *List = T.newExpr(ExprKind::InitList, offset());
    ++Pos;
    while (!accept("}")) {
      Expr *Element = nullptr;
      if (isPunct("[") || isPunct(".")) {
        Element = T.newExpr(ExprKind::Designated, offset());
        if (accept("[")) {
          Expr *Index = parseConditional();
          if (!Index || !expect("]"))
            return nullptr;
          Element->Ops.push_back(Index);
        } else {
          ++Pos;
          if (tok().Kind != TokenKind::Identifier)
            return fail("expected a field name"), nullptr;
          Element->Text = tok().Text;
          ++Pos;
        }
        if (!expect("="))
          return nullptr;
        Expr *Value = parseInitializer();
        if (!Value)
          return nullptr;
        Element->Ops.push_back(Value);
        Element->End = lastEnd();
      } else {
        Element = parseInitializer();
        if (!Element)
          return nullptr;
      }
      List->Ops.push_back(Element);
      // `Postfix` marks a list written with a comma after its last element.
      List->Postfix = isPunct(",") && isPunct("}", 1);
      if (!accept(",") && !isPunct("}"))
        return fail("expected ',' or '}'"), nullptr;
    }
    List->End = lastEnd();
    attachTrailing(List);
    return List;
  }

  /// Follows `#pragma pack(push, N)`, `(pop)`, `(N)` and `()`, which the
  /// records after it are defined under; false for another `#pragma pack`.
  bool packDirective(llvm::StringRef Line) {
    llvm::StringRef Rest = Line.drop_front().ltrim();
    if (!Rest.consume_front("pragma"))
      return true;
    Rest = Rest.ltrim();
    if (!Rest.consume_front("pack"))
      return true;
    Rest = Rest.trim();
    if (!Rest.consume_front("(") || !Rest.consume_back(")"))
      return false;
    llvm::SmallVector<llvm::StringRef, 2> Args;
    Rest.split(Args, ',');
    for (llvm::StringRef &Arg : Args)
      Arg = Arg.trim();
    unsigned Value = 0;
    if (Args.size() == 1 && Args[0].empty()) {
      Pack = 0;
      return true;
    }
    if (Args.size() == 1 && Args[0] == "pop") {
      if (PackStack.empty())
        return false;
      Pack = PackStack.back();
      PackStack.pop_back();
      return true;
    }
    if (Args.size() == 1 && !Args[0].getAsInteger(10, Value)) {
      Pack = Value;
      return true;
    }
    if (Args.size() == 2 && Args[0] == "push" &&
        !Args[1].getAsInteger(10, Value)) {
      PackStack.push_back(Pack);
      Pack = Value;
      return true;
    }
    return false;
  }

  TopLevel parseExternalDeclaration() {
    TopLevel Item;
    Item.Begin = offset();
    if (isIdent("_Static_assert") || isIdent("static_assert")) {
      Item.TheKind = TopLevel::Kind::StaticAssert;
      Item.Text = tok().Text;
      ++Pos;
      if (!expect("("))
        return Item;
      Item.Assertion = parseAssignment();
      if (!Item.Assertion || !expect(","))
        return Item;
      Item.Message = parseAssignment();
      if (!Item.Message || !expect(")") || !expect(";"))
        return Item;
      Item.End = lastEnd();
      Item.Trailing = takeSameLineComments();
      return Item;
    }
    Specifiers S;
    EnumerationsDefined = true;
    const bool Read = parseSpecifiers(S);
    EnumerationsDefined = false;
    if (!Read)
      return Item;
    if (isPunct(";")) {
      parseDeclarators(S, Item.Decls);
      Item.End = lastEnd();
      Item.Trailing = takeSameLineComments();
      return Item;
    }
    Declarator D;
    if (!parseDeclarator(D, /*Abstract=*/false))
      return Item;
    Decl *First = declaratorDecl(S, D);
    if (First->Kind == DeclKind::Function && D.DeclaresFunction &&
        isPunct("{")) {
      ShadowScopes.emplace_back();
      for (Decl *P : First->Params) {
        if (P->Name.empty())
          return ShadowScopes.pop_back(),
                 fail("a definition's parameters need names"), Item;
        if (TypedefNames.contains(P->Name))
          ShadowScopes.back().insert(P->Name);
      }
      First->Body = parseCompound();
      ShadowScopes.pop_back();
      if (!First->Body)
        return Item;
      First->End = lastEnd();
      Item.Decls.push_back(First);
      Item.End = lastEnd();
      Item.Trailing = takeSameLineComments();
      return Item;
    }
    if (!parseDeclaratorTail(First))
      return Item;
    if (accept("=")) {
      First->Init = parseInitializer();
      if (!First->Init)
        return Item;
    }
    First->End = lastEnd();
    Item.Decls.push_back(First);
    if (accept(",") && !parseDeclarators(S, Item.Decls))
      return Item;
    if (Item.Decls.size() == 1 && !expect(";"))
      return Item;
    Item.End = lastEnd();
    Item.Trailing = takeSameLineComments();
    return Item;
  }

  //===--- Statements ---===//

  Stmt *finish(Stmt *S) {
    S->End = lastEnd();
    S->Trailing = takeSameLineComments();
    return S;
  }

  Stmt *parseCompound() {
    Stmt *S = T.newStmt(StmtKind::Compound, offset());
    if (!expect("{"))
      return nullptr;
    ShadowScopes.emplace_back();
    struct PopScope {
      std::vector<llvm::StringSet<>> &Scopes;
      ~PopScope() { Scopes.pop_back(); }
    } Pop{ShadowScopes};
    while (true) {
      for (const Comment &C : takeCommentsBefore(Pos)) {
        Stmt *Note = T.newStmt(StmtKind::Comment, C.Offset);
        Note->TheComment = C;
        Note->End = C.Offset + C.Text.size();
        S->Body.push_back(Note);
      }
      if (accept("}"))
        break;
      if (atEnd())
        return fail("unterminated block"), nullptr;
      Stmt *Child = parseStatement();
      if (!Child)
        return nullptr;
      S->Body.push_back(Child);
    }
    return finish(S);
  }

  /// `( expression )` after a keyword.
  Expr *parseCondition() {
    if (!expect("("))
      return nullptr;
    Expr *E = parseExpression();
    if (!E || !expect(")"))
      return nullptr;
    return E;
  }

  /// Text from Pos through the `)` matching the `(` at Pos.
  bool skipBalanced(llvm::StringRef Open, llvm::StringRef Close) {
    unsigned Depth = 0;
    do {
      if (atEnd())
        return fail("unbalanced '" + Open + "'");
      if (isPunct(Open))
        ++Depth;
      else if (isPunct(Close))
        --Depth;
      ++Pos;
    } while (Depth);
    return true;
  }

  Stmt *parseStatement() {
    if (++Nesting > MaxNesting)
      return fail("statements nested too deeply"), nullptr;
    Stmt *S = parseStatementInner();
    --Nesting;
    return S;
  }

  Stmt *parseStatementInner() {
    const size_t Begin = offset();
    const Token &Tok = tok();
    if (Tok.Kind == TokenKind::Directive)
      return fail("a preprocessor line inside a function"), nullptr;
    if (Tok.isPunctuator("{"))
      return parseCompound();
    if (Tok.isPunctuator(";")) {
      ++Pos;
      return finish(T.newStmt(StmtKind::Empty, Begin));
    }
    if (Tok.Kind == TokenKind::Identifier) {
      const llvm::StringRef W = Tok.Text;
      if (W == "if") {
        Stmt *S = T.newStmt(StmtKind::If, Begin);
        ++Pos;
        if (!(S->Value = parseCondition()))
          return nullptr;
        S->Then = parseStatement();
        if (!S->Then)
          return nullptr;
        // A comment between `}` and `else` stays with the if.
        if (isIdent("else")) {
          ++Pos;
          if (!(S->Else = parseStatement()))
            return nullptr;
        }
        S->End = lastEnd();
        return S;
      }
      if (W == "while") {
        Stmt *S = T.newStmt(StmtKind::While, Begin);
        ++Pos;
        if (!(S->Value = parseCondition()) || !(S->Then = parseStatement()))
          return nullptr;
        S->End = lastEnd();
        return S;
      }
      if (W == "do") {
        Stmt *S = T.newStmt(StmtKind::DoWhile, Begin);
        ++Pos;
        if (!(S->Then = parseStatement()))
          return nullptr;
        if (!isIdent("while"))
          return fail("expected 'while'"), nullptr;
        ++Pos;
        if (!(S->Value = parseCondition()) || !expect(";"))
          return nullptr;
        return finish(S);
      }
      if (W == "for") {
        Stmt *S = T.newStmt(StmtKind::For, Begin);
        ++Pos;
        if (!expect("("))
          return nullptr;
        if (isDeclarationStart())
          return fail("declarations in a for header are not read"), nullptr;
        if (!isPunct(";") && !(S->Value = parseExpression()))
          return nullptr;
        if (!expect(";"))
          return nullptr;
        Expr *Cond = nullptr;
        if (!isPunct(";") && !(Cond = parseExpression()))
          return nullptr;
        if (!expect(";"))
          return nullptr;
        if (!isPunct(")") && !(S->Step = parseExpression()))
          return nullptr;
        if (!expect(")"))
          return nullptr;
        if (Cond) {
          // The condition rides in Else's Value slot: the init is Value.
          Stmt *Holder = T.newStmt(StmtKind::Expression, Cond->Begin);
          Holder->Value = Cond;
          S->Else = Holder;
        }
        if (!(S->Then = parseStatement()))
          return nullptr;
        S->End = lastEnd();
        return S;
      }
      if (W == "switch") {
        Stmt *S = T.newStmt(StmtKind::Switch, Begin);
        ++Pos;
        if (!(S->Value = parseCondition()) || !(S->Then = parseStatement()))
          return nullptr;
        S->End = lastEnd();
        return S;
      }
      if (W == "case") {
        Stmt *S = T.newStmt(StmtKind::Case, Begin);
        ++Pos;
        if (!(S->Value = parseConditional()) || !expect(":"))
          return nullptr;
        return finish(S);
      }
      if (W == "default" && isPunct(":", 1)) {
        Pos += 2;
        return finish(T.newStmt(StmtKind::Default, Begin));
      }
      if (W == "break" || W == "continue" || W == "__leave") {
        Stmt *S = T.newStmt(W == "break"      ? StmtKind::Break
                            : W == "continue" ? StmtKind::Continue
                                              : StmtKind::Leave,
                            Begin);
        ++Pos;
        if (!expect(";"))
          return nullptr;
        return finish(S);
      }
      if (W == "return") {
        Stmt *S = T.newStmt(StmtKind::Return, Begin);
        ++Pos;
        if (!isPunct(";") && !(S->Value = parseExpression()))
          return nullptr;
        if (!expect(";"))
          return nullptr;
        return finish(S);
      }
      if (W == "goto") {
        Stmt *S = T.newStmt(StmtKind::Goto, Begin);
        ++Pos;
        if (tok().Kind != TokenKind::Identifier)
          return fail("expected a label"), nullptr;
        S->Text = tok().Text;
        ++Pos;
        if (!expect(";"))
          return nullptr;
        return finish(S);
      }
      if (W == "__try" || W == "try")
        return parseTry(W == "try");
      if (W == "throw") {
        Stmt *S = T.newStmt(StmtKind::Throw, Begin);
        ++Pos;
        if (!isPunct(";") && !(S->Value = parseExpression()))
          return nullptr;
        if (!expect(";"))
          return nullptr;
        return finish(S);
      }
      if ((W == "__asm__" || W == "__asm" || W == "asm") &&
          (isPunct("(", 1) || isIdent("volatile", 1) ||
           isIdent("__volatile__", 1))) {
        Stmt *S = T.newStmt(StmtKind::Asm, Begin);
        ++Pos;
        if (isIdent("volatile") || isIdent("__volatile__"))
          ++Pos;
        if (!skipBalanced("(", ")"))
          return nullptr;
        S->Text = Src.slice(Begin, lastEnd());
        if (!expect(";"))
          return nullptr;
        return finish(S);
      }
      if (W == "__asm" && isPunct("{", 1)) {
        Stmt *S = T.newStmt(StmtKind::Asm, Begin);
        ++Pos;
        if (!skipBalanced("{", "}"))
          return nullptr;
        S->Text = Src.slice(Begin, lastEnd());
        accept(";");
        return finish(S);
      }
      if (isPunct(":", 1) && !isTypeNameStart()) {
        Stmt *S = T.newStmt(StmtKind::Label, Begin);
        S->Text = W;
        Pos += 2;
        return finish(S);
      }
      if (isDeclarationStart()) {
        Stmt *S = T.newStmt(StmtKind::Declaration, Begin);
        Specifiers Spec;
        if (!parseSpecifiers(Spec) || !parseDeclarators(Spec, S->Decls))
          return nullptr;
        return finish(S);
      }
    }
    Stmt *S = T.newStmt(StmtKind::Expression, Begin);
    if (!(S->Value = parseExpression()) || !expect(";"))
      return nullptr;
    return finish(S);
  }

  Stmt *parseTry(bool Cxx) {
    Stmt *S = T.newStmt(StmtKind::Try, offset());
    S->CxxTry = Cxx;
    ++Pos;
    if (!(S->Then = parseCompound()))
      return nullptr;
    while (true) {
      Handler H;
      H.Leading = takeCommentsBefore(Pos);
      if (!Cxx && isIdent("__except")) {
        ++Pos;
        H.TheKind = Handler::Kind::Except;
        if (!(H.Filter = parseCondition()))
          return nullptr;
      } else if (!Cxx && isIdent("__finally")) {
        ++Pos;
        H.TheKind = Handler::Kind::Finally;
      } else if (Cxx && isIdent("catch")) {
        ++Pos;
        H.TheKind = Handler::Kind::Catch;
        const size_t Open = offset();
        if (!skipBalanced("(", ")"))
          return nullptr;
        H.Parameter = Src.slice(Open + 1, lastEnd() - 1).trim();
      } else {
        if (S->Handlers.empty())
          return fail("expected a handler"), nullptr;
        // Comments before the next statement belong to it.
        CommentCursor -= H.Leading.size();
        break;
      }
      if (!(H.Body = parseCompound()))
        return nullptr;
      S->Handlers.push_back(std::move(H));
      if (!Cxx)
        break;
    }
    S->End = lastEnd();
    return S;
  }

  //===--- Expressions ---===//

  Expr *parseExpression() {
    Expr *E = parseAssignment();
    if (!E || !isPunct(","))
      return E;
    Expr *Comma = T.newExpr(ExprKind::Comma, E->Begin);
    Comma->Ops.push_back(E);
    while (accept(",")) {
      Expr *Next = parseAssignment();
      if (!Next)
        return nullptr;
      Comma->Ops.push_back(Next);
    }
    Comma->End = lastEnd();
    return Comma;
  }

  Expr *parseAssignment() {
    Expr *LHS = parseConditional();
    if (!LHS)
      return nullptr;
    if (tok().Kind == TokenKind::Punctuator &&
        isAssignmentOperator(tok().Text)) {
      Expr *A = T.newExpr(ExprKind::Assign, LHS->Begin);
      A->Text = tok().Text;
      ++Pos;
      Expr *RHS = parseAssignment();
      if (!RHS)
        return nullptr;
      A->Ops = {LHS, RHS};
      A->End = lastEnd();
      return A;
    }
    return LHS;
  }

  Expr *parseConditional() {
    Expr *Cond = parseBinary(1);
    if (!Cond || !isPunct("?"))
      return Cond;
    ++Pos;
    Expr *E = T.newExpr(ExprKind::Conditional, Cond->Begin);
    Expr *Then = parseExpression();
    if (!Then || !expect(":"))
      return nullptr;
    Expr *Else = parseConditional();
    if (!Else)
      return nullptr;
    E->Ops = {Cond, Then, Else};
    E->End = lastEnd();
    return E;
  }

  Expr *parseBinary(int MinPrecedence) {
    Expr *LHS = parseUnary();
    if (!LHS)
      return nullptr;
    while (tok().Kind == TokenKind::Punctuator) {
      const int Precedence = binaryPrecedence(tok().Text);
      if (!Precedence || Precedence < MinPrecedence)
        break;
      Expr *B = T.newExpr(ExprKind::Binary, LHS->Begin);
      B->Text = tok().Text;
      ++Pos;
      Expr *RHS = parseBinary(Precedence + 1);
      if (!RHS)
        return nullptr;
      B->Ops = {LHS, RHS};
      B->End = lastEnd();
      LHS = B;
    }
    return LHS;
  }

  Expr *parseUnary() {
    if (++Nesting > MaxNesting)
      return fail("expression nested too deeply"), nullptr;
    Expr *E = parseUnaryInner();
    --Nesting;
    return E;
  }

  Expr *parseUnaryInner() {
    const size_t Begin = offset();
    CommentList Leading = takeCommentsBefore(Pos);
    auto WithLeading = [&](Expr *E) {
      if (E && !Leading.empty())
        E->Leading.insert(E->Leading.begin(), Leading.begin(), Leading.end());
      return E;
    };
    const Token &Tok = tok();
    if (Tok.Kind == TokenKind::Punctuator &&
        (Tok.Text == "-" || Tok.Text == "+" || Tok.Text == "~" ||
         Tok.Text == "!" || Tok.Text == "*" || Tok.Text == "&" ||
         Tok.Text == "++" || Tok.Text == "--")) {
      Expr *E = T.newExpr(ExprKind::Unary, Begin);
      E->Text = Tok.Text;
      ++Pos;
      Expr *Operand = parseUnary();
      if (!Operand)
        return nullptr;
      E->Ops.push_back(Operand);
      E->End = lastEnd();
      return WithLeading(E);
    }
    if (Tok.is(TokenKind::Identifier, "sizeof") ||
        Tok.is(TokenKind::Identifier, "_Alignof") ||
        Tok.is(TokenKind::Identifier, "__alignof__")) {
      const bool Sizeof = Tok.Text == "sizeof";
      ++Pos;
      if (isPunct("(") && isTypeNameStart(1)) {
        ++Pos;
        Expr *E = T.newExpr(
            Sizeof ? ExprKind::SizeofType : ExprKind::AlignofType, Begin);
        if (!(E->Written = parseTypeName()) || !expect(")"))
          return nullptr;
        E->End = lastEnd();
        attachTrailing(E);
        return WithLeading(E);
      }
      if (!Sizeof)
        return fail("_Alignof needs a type"), nullptr;
      Expr *E = T.newExpr(ExprKind::SizeofExpr, Begin);
      Expr *Operand = parseUnary();
      if (!Operand)
        return nullptr;
      E->Ops.push_back(Operand);
      E->End = lastEnd();
      return WithLeading(E);
    }
    if (Tok.isPunctuator("(") && isTypeNameStart(1)) {
      ++Pos;
      const CType *Ty = parseTypeName();
      if (!Ty || !expect(")"))
        return nullptr;
      if (isPunct("{")) {
        Expr *E = T.newExpr(ExprKind::CompoundLiteral, Begin);
        E->Written = Ty;
        Expr *Init = parseInitializer();
        if (!Init)
          return nullptr;
        E->Ops.push_back(Init);
        E->End = lastEnd();
        return WithLeading(parsePostfix(E));
      }
      Expr *E = T.newExpr(ExprKind::Cast, Begin);
      E->Written = Ty;
      Expr *Operand = parseUnary();
      if (!Operand)
        return nullptr;
      E->Ops.push_back(Operand);
      E->End = lastEnd();
      return WithLeading(E);
    }
    Expr *Primary = parsePrimary();
    if (!Primary)
      return nullptr;
    return WithLeading(parsePostfix(Primary));
  }

  Expr *parsePostfix(Expr *E) {
    while (E) {
      if (accept("[")) {
        Expr *S = T.newExpr(ExprKind::Subscript, E->Begin);
        Expr *Index = parseExpression();
        if (!Index || !expect("]"))
          return nullptr;
        S->Ops = {E, Index};
        S->End = lastEnd();
        E = S;
      } else if (accept("(")) {
        Expr *Call = T.newExpr(ExprKind::Call, E->Begin);
        Call->Ops.push_back(E);
        if (!accept(")")) {
          while (true) {
            Expr *Arg = parseAssignment();
            if (!Arg)
              return nullptr;
            Call->Ops.push_back(Arg);
            if (accept(")"))
              break;
            if (!expect(","))
              return nullptr;
          }
        }
        Call->End = lastEnd();
        E = Call;
      } else if (isPunct(".") || isPunct("->")) {
        Expr *M = T.newExpr(ExprKind::Member, E->Begin);
        M->Postfix = isPunct("->");
        ++Pos;
        if (tok().Kind != TokenKind::Identifier)
          return fail("expected a field name"), nullptr;
        M->Text = tok().Text;
        ++Pos;
        M->Ops.push_back(E);
        M->End = lastEnd();
        E = M;
      } else if (isPunct("++") || isPunct("--")) {
        Expr *U = T.newExpr(ExprKind::Unary, E->Begin);
        U->Text = tok().Text;
        U->Postfix = true;
        ++Pos;
        U->Ops.push_back(E);
        U->End = lastEnd();
        E = U;
      } else
        break;
      attachTrailing(E);
    }
    return E;
  }

  Expr *parsePrimary() {
    const size_t Begin = offset();
    const Token &Tok = tok();
    switch (Tok.Kind) {
    case TokenKind::Number: {
      const bool IsFloat =
          !Tok.Text.starts_with_insensitive("0x")
              ? Tok.Text.find_first_of(".eE") != llvm::StringRef::npos
              : Tok.Text.find_first_of(".pP") != llvm::StringRef::npos;
      Expr *E =
          T.newExpr(IsFloat ? ExprKind::Floating : ExprKind::Integer, Begin);
      E->Text = Tok.Text;
      ++Pos;
      E->End = lastEnd();
      attachTrailing(E);
      return E;
    }
    case TokenKind::Char: {
      Expr *E = T.newExpr(ExprKind::Character, Begin);
      E->Text = Tok.Text;
      ++Pos;
      E->End = lastEnd();
      attachTrailing(E);
      return E;
    }
    case TokenKind::String: {
      Expr *E = T.newExpr(ExprKind::String, Begin);
      E->Text = Tok.Text;
      ++Pos;
      if (tok().Kind == TokenKind::String) {
        // Adjacent literals are one (C17 5.1.1.2); keep each part.
        Expr *First = T.newExpr(ExprKind::String, Begin);
        First->Text = E->Text;
        First->End = lastEnd();
        E->Ops.push_back(First);
        while (tok().Kind == TokenKind::String) {
          Expr *Part = T.newExpr(ExprKind::String, offset());
          Part->Text = tok().Text;
          ++Pos;
          Part->End = lastEnd();
          E->Ops.push_back(Part);
        }
      }
      E->End = lastEnd();
      attachTrailing(E);
      return E;
    }
    case TokenKind::Identifier:
      return parseNamePrimary();
    case TokenKind::Punctuator:
      if (Tok.isPunctuator("(")) {
        ++Pos;
        if (isPunct("{")) {
          Expr *E = T.newExpr(ExprKind::StatementExpr, Begin);
          if (!(E->Body = parseCompound()) || !expect(")"))
            return nullptr;
          E->End = lastEnd();
          attachTrailing(E);
          return E;
        }
        Expr *E = T.newExpr(ExprKind::Paren, Begin);
        Expr *Inner = parseExpression();
        if (!Inner || !expect(")"))
          return nullptr;
        E->Ops.push_back(Inner);
        E->End = lastEnd();
        attachTrailing(E);
        return E;
      }
      break;
    default:
      break;
    }
    return fail("expected an expression"), nullptr;
  }

  Expr *parseNamePrimary() {
    const size_t Begin = offset();
    const llvm::StringRef W = tok().Text;
    if (W == "__builtin_bit_cast" || W == "__builtin_offsetof" ||
        W == "offsetof") {
      const bool BitCast = W == "__builtin_bit_cast";
      ++Pos;
      Expr *E =
          T.newExpr(BitCast ? ExprKind::BitCast : ExprKind::Offsetof, Begin);
      if (!expect("(") || !(E->Written = parseTypeName()) || !expect(","))
        return nullptr;
      if (BitCast) {
        Expr *Operand = parseAssignment();
        if (!Operand)
          return nullptr;
        E->Ops.push_back(Operand);
      } else {
        if (tok().Kind != TokenKind::Identifier)
          return fail("expected a field name"), nullptr;
        E->Text = tok().Text;
        ++Pos;
      }
      if (!expect(")"))
        return nullptr;
      E->End = lastEnd();
      attachTrailing(E);
      return E;
    }
    if (W == "va_arg" && isPunct("(", 1)) {
      ++Pos;
      Expr *E = T.newExpr(ExprKind::VaArg, Begin);
      ++Pos;
      Expr *List = parseAssignment();
      if (!List || !expect(",") || !(E->Written = parseTypeName()) ||
          !expect(")"))
        return nullptr;
      E->Ops.push_back(List);
      E->End = lastEnd();
      attachTrailing(E);
      return E;
    }
    if ((W == "__asm__" || W == "__asm") &&
        (isIdent("volatile", 1) || isPunct("(", 1))) {
      // `__asm__` where the emitter wrote it as a value.
      Expr *E = T.newExpr(ExprKind::Asm, Begin);
      ++Pos;
      if (isIdent("volatile"))
        ++Pos;
      if (!skipBalanced("(", ")"))
        return nullptr;
      E->Text = Src.slice(Begin, lastEnd());
      E->End = lastEnd();
      attachTrailing(E);
      return E;
    }
    if (isTypeNameStart() || isDeclarationStart())
      return fail("unexpected type"), nullptr;
    Expr *E = T.newExpr(ExprKind::Name, Begin);
    E->Text = W;
    ++Pos;
    E->End = lastEnd();
    attachTrailing(E);
    return E;
  }

  Tree &T;
  llvm::StringRef Src;
  std::vector<TopLevel> &Items;
  std::vector<Token> Toks;
  std::vector<Comment> Comments;
  /// For each comment, the index of the token after it.
  std::vector<size_t> CommentNext;
  size_t CommentCursor = 0;
  size_t Pos = 0;
  unsigned Nesting = 0;
  bool AllowUnnamed = false;
  /// Whether specifiers may declare a structure or union without a tag, as
  /// a record's member does.
  bool AnonymousRecords = false;
  /// Whether specifiers may define an enumeration: a file-scope
  /// declaration's may.
  bool EnumerationsDefined = false;
  /// The `#pragma pack` the next record is defined under, and the packs
  /// `push` saved.
  unsigned Pack = 0;
  std::vector<unsigned> PackStack;
  /// Why every declaration after a directive this reader does not follow is
  /// not read.
  std::string Pending;
  std::string Error;
  llvm::StringSet<> TypedefNames;
  llvm::StringMap<const CType *> Typedefs;
  /// Per block, from the outermost: the typedef names its declarations
  /// declare as ordinary identifiers.
  std::vector<llvm::StringSet<>> ShadowScopes;
};

} // namespace

std::vector<TopLevel> csyntax::parseDeclarations(llvm::StringRef Source,
                                                 Tree &Nodes) {
  std::vector<TopLevel> Items;
  auto Tokens = lex(Source);
  if (!Tokens) {
    TopLevel Unread;
    Unread.TheKind = TopLevel::Kind::Unread;
    Unread.End = Source.size();
    Unread.Text = Source;
    Unread.Reason = llvm::toString(Tokens.takeError());
    Items.push_back(std::move(Unread));
    return Items;
  }
  Parser(Nodes, Source, Items, std::move(*Tokens)).run();
  return Items;
}

void csyntax::parse(llvm::StringRef Source, Tree &Result) {
  Result.Source = Source;
  Result.Items = parseDeclarations(Source, Result);
}
