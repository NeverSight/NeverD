//===- DialectPrinter.cpp - Printing emitted C in another syntax ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "DialectPrinter.h"

#include "neverd/backend/c/CSourceMap.h"

#include "llvm/ADT/StringExtras.h"

#include <map>
#include <set>

using namespace neverd;
using namespace neverd::csyntax;

DialectPrinter::DialectPrinter(const Tree &T, const SourceDialectOptions &Opts)
    : T(T), Opts(Opts) {}

SourceDialectText DialectPrinter::run() {
  // A source name two identifiers share would name neither, so those keep
  // their C identifiers, unless they are functions of distinct symbols: C++
  // overloads share their name (`std::_DeallocateCaller::
  // __do_deallocate_handle_size` takes (void*, unsigned) and (void*,
  // unsigned, std::align_val_t)), and each identifier still links its own
  // symbol.  An object or a type has no such name to share, and two
  // functions of one symbol (a `static` helper of two files) are no
  // overloads: only their C identifiers tell them apart.
  struct Sharers {
    std::set<llvm::StringRef> Identifiers;
    std::set<llvm::StringRef> Symbols;
    bool AllFunctions = true;
  };
  std::map<std::string, Sharers> ByName;
  for (const CSourceName &Name : Opts.Names) {
    if (Name.Symbol.empty())
      continue;
    if (auto Spelled = Name.TheKind == CSourceName::Kind::Type
                           ? sourceTypeName(Name.Symbol)
                           : sourceName(Name.Symbol)) {
      Sharers &Sharing = ByName[*Spelled];
      Sharing.Identifiers.insert(Name.Identifier);
      Sharing.Symbols.insert(Name.Symbol);
      Sharing.AllFunctions &= Name.TheKind == CSourceName::Kind::Function;
    }
  }
  for (auto &[Spelled, Sharing] : ByName)
    if (Sharing.Identifiers.size() == 1 ||
        (Sharing.AllFunctions &&
         Sharing.Symbols.size() == Sharing.Identifiers.size()))
      for (llvm::StringRef Identifier : Sharing.Identifiers)
        SourceNames[Identifier] = Spelled;
  for (const CSourceName &Name : Opts.Names)
    if (SourceNames.contains(Name.Identifier))
      NameEntries[Name.Identifier] = &Name;

  header();
  size_t PreviousEnd = 0;
  bool LocalWritten = false;
  for (size_t I = 0; I < T.Items.size(); ++I) {
    const TopLevel &Item = T.Items[I];
    // The program's types, as the language defines them, stand for the C
    // that defines them.
    if (const auto &Local = Opts.LocalTypes;
        Local && Item.Begin >= Local->CBegin && Item.End <= Local->CEnd) {
      if (!LocalWritten) {
        LocalWritten = true;
        endLine();
        const size_t Begin = mark();
        llvm::SmallVector<llvm::StringRef, 64> Lines;
        llvm::StringRef(Local->Text).rtrim('\n').split(Lines, '\n');
        for (llvm::StringRef L : Lines) {
          Buffer += L.str();
          AtLineStart = false;
          endLine();
        }
        piece(Local->CBegin, Local->CEnd, Begin);
      }
      PreviousEnd = Item.End;
      continue;
    }
    if (I && T.Source.slice(PreviousEnd, Item.Begin).count('\n') >= 2)
      endLine();
    const TopLevel *Next = nullptr;
    for (size_t J = I + 1; J < T.Items.size() && !Next; ++J)
      if (T.Items[J].TheKind != TopLevel::Kind::Comment)
        Next = &T.Items[J];
    item(Item, Next);
    PreviousEnd = Item.End;
  }
  SourceDialectText Result;
  Result.Pieces = std::move(Pieces);
  Result.Unread = std::move(Unread);
  unmark(Result);
  return Result;
}

namespace {
// A source name in the printed text is NameOpen, its identifier, NameClose,
// the name, NameEnd.  Emitted C has no control characters.
constexpr char NameOpen = '\x01';
constexpr char NameClose = '\x02';
constexpr char NameEnd = '\x03';
} // namespace

void DialectPrinter::unmark(SourceDialectText &Result) {
  std::string &Text = Result.Text;
  Text.reserve(Buffer.size());
  // (offset in Buffer, bytes removed before it) at each mark.
  std::vector<std::pair<size_t, size_t>> Removed;
  size_t RemovedBytes = 0;
  struct Open {
    std::string Identifier;
    size_t Begin;
  };
  std::vector<Open> Opened;
  for (size_t I = 0; I < Buffer.size();) {
    const char C = Buffer[I];
    if (C == NameOpen) {
      const size_t Close = Buffer.find(NameClose, I);
      Opened.push_back({Buffer.substr(I + 1, Close - I - 1), Text.size()});
      RemovedBytes += Close + 1 - I;
      I = Close + 1;
      Removed.push_back({I, RemovedBytes});
      continue;
    }
    if (C == NameEnd && !Opened.empty()) {
      Open Name = std::move(Opened.back());
      Opened.pop_back();
      SourceDialectName Spelled;
      Spelled.Begin = Name.Begin;
      Spelled.End = Text.size();
      if (const CSourceName *Entry = NameEntries.lookup(Name.Identifier)) {
        Spelled.Symbol = Entry->Symbol;
        Spelled.Address = Entry->Address;
      }
      Spelled.Identifier = std::move(Name.Identifier);
      Result.Names.push_back(std::move(Spelled));
      ++RemovedBytes;
      ++I;
      Removed.push_back({I, RemovedBytes});
      continue;
    }
    Text += C;
    ++I;
  }
  std::sort(Result.Names.begin(), Result.Names.end(),
            [](const SourceDialectName &A, const SourceDialectName &B) {
              return A.Begin < B.Begin;
            });
  auto Clean = [&](size_t Offset) {
    auto It =
        std::upper_bound(Removed.begin(), Removed.end(), Offset,
                         [](size_t O, const std::pair<size_t, size_t> &R) {
                           return O < R.first;
                         });
    return It == Removed.begin() ? Offset : Offset - std::prev(It)->second;
  };
  for (SourceDialectPiece &P : Result.Pieces) {
    P.Begin = Clean(P.Begin);
    P.End = Clean(P.End);
  }
}

void DialectPrinter::item(const TopLevel &Item, const TopLevel *Next) {
  const size_t Begin = mark();
  const size_t PiecesBefore = Pieces.size();
  Refusal.clear();
  switch (Item.TheKind) {
  case TopLevel::Kind::Comment:
    if (repeatsName(Item, Next))
      return;
    line(commentText(Item.TheComment));
    break;
  case TopLevel::Kind::Directive:
    directive(Item);
    break;
  case TopLevel::Kind::Unread:
    unread(Item.Text, Item.Reason);
    trailingComments(Item.Trailing);
    break;
  case TopLevel::Kind::StaticAssert:
    staticAssert(Item);
    break;
  case TopLevel::Kind::Declaration:
    declaration(Item);
    break;
  }
  if (refused()) {
    Buffer.resize(Begin);
    Pieces.resize(PiecesBefore);
    Depth = 0;
    AtLineStart = true;
    unread(T.Source.slice(Item.Begin, Item.End), Refusal);
    trailingComments(Item.Trailing);
    Refusal.clear();
  }
  piece(Item.Begin, Item.End, Begin);
}

void DialectPrinter::directive(const TopLevel &Item) {
  if (Item.Text.starts_with("#include"))
    return;
  line((lineComment() + " " + Item.Text).str());
}

std::optional<SourceDialectLocalTypes::Path>
DialectPrinter::localMember(const Expr *E) const {
  if (!Opts.LocalTypes || E->Ops.empty() || !E->Ops.front()->Ty)
    return std::nullopt;
  const CType *Base = E->Ops.front()->Ty;
  if (E->Postfix) {
    if (!Base->isPointer() || !Base->Inner)
      return std::nullopt;
    Base = Base->Inner;
  }
  if (!Base->isRecord() || !localRecord(Base))
    return std::nullopt;
  llvm::StringRef Tag = Base->Record->Spelling;
  Tag.consume_front("struct ") || Tag.consume_front("union ");
  const auto Members = Opts.LocalTypes->Members.find(Tag.str());
  const bool BitField = E->Field && E->Field->BitWidth;
  if (Members != Opts.LocalTypes->Members.end())
    if (const auto Path = Members->second.find(E->Text.str());
        Path != Members->second.end() &&
        (BitField ? Path->second.bitField() : !Path->second.Hops.empty()))
      return Path->second;
  unsupported(BitField ? "a bit-field the program's types read through no "
                         "methods"
                       : "a member the program's types do not reach as a "
                         "field");
  return SourceDialectLocalTypes::Path();
}

bool DialectPrinter::localRecord(const CType *Ty) const {
  if (!Opts.LocalTypes || !Ty->isRecord() || Ty->Record->Anonymous)
    return false;
  llvm::StringRef Tag = Ty->Record->Spelling;
  if (!Tag.consume_front("struct ") && !Tag.consume_front("union "))
    return false;
  return Opts.LocalTypes->Members.contains(Tag.str()) ||
         Opts.LocalTypes->Opaque.contains(Tag.str());
}

void DialectPrinter::unread(llvm::StringRef Text, llvm::StringRef Reason) {
  Unread.push_back(Reason.str());
  line((lineComment() + " NeverD: shown as C: " + Reason).str());
  llvm::SmallVector<llvm::StringRef, 16> Lines;
  Text.split(Lines, '\n');
  for (llvm::StringRef L : Lines) {
    // The C text keeps its own indentation.
    Buffer += L.str();
    AtLineStart = false;
    endLine();
  }
}

void DialectPrinter::write(llvm::StringRef Text) {
  if (Text.empty())
    return;
  if (AtLineStart)
    Buffer += indentation(Depth);
  Buffer += Text.str();
  AtLineStart = false;
}

void DialectPrinter::endLine() {
  // Blanks before a line end are noise.
  while (!AtLineStart && !Buffer.empty() && Buffer.back() == ' ')
    Buffer.pop_back();
  Buffer += '\n';
  AtLineStart = true;
}

void DialectPrinter::piece(size_t CBegin, size_t CEnd, size_t Begin) {
  size_t End = Buffer.size();
  while (End > Begin && Buffer[End - 1] == '\n')
    --End;
  if (End > Begin && CEnd > CBegin)
    Pieces.push_back({CBegin, CEnd, Begin, End});
}

std::string DialectPrinter::commentText(const Comment &C) const {
  if (C.Text.starts_with("//"))
    return (lineComment() + C.Text.drop_front(2)).str();
  // Neither language nests block comments the way C does not: an opening
  // inside the comment would open another in Rust.
  llvm::StringRef Body = C.Text.drop_front(2).drop_back(2);
  std::string Result = "/*";
  for (size_t I = 0; I < Body.size(); ++I) {
    Result += Body[I];
    if (Body[I] == '/' && I + 1 < Body.size() && Body[I + 1] == '*')
      Result += ' ';
  }
  return Result + "*/";
}

void DialectPrinter::leadingComments(const CommentList &Comments) {
  for (const Comment &C : Comments)
    line(commentText(C));
}

void DialectPrinter::trailingComments(const CommentList &Comments) {
  if (Comments.empty())
    return;
  // After the line just ended.
  if (AtLineStart && !Buffer.empty() && Buffer.back() == '\n')
    Buffer.pop_back();
  for (const Comment &C : Comments) {
    Buffer += ' ';
    Buffer += commentText(C);
  }
  Buffer += '\n';
  AtLineStart = true;
}

namespace {
/// Appends the bytes \p Text spells inside a narrow C string or character
/// literal to \p Bytes; false for an escape that spells no single byte.
bool appendLiteralBytes(llvm::StringRef Text, std::string &Bytes) {
  for (size_t I = 0; I < Text.size(); ++I) {
    if (Text[I] != '\\') {
      Bytes += Text[I];
      continue;
    }
    if (++I == Text.size())
      return false;
    unsigned Value = 0;
    if (Text[I] >= '0' && Text[I] <= '7') {
      Value = Text[I] - '0';
      for (int Digits = 1; Digits < 3 && I + 1 < Text.size() &&
                           Text[I + 1] >= '0' && Text[I + 1] <= '7';
           ++Digits)
        Value = Value * 8 + (Text[++I] - '0');
    } else if (Text[I] == 'x') {
      if (I + 1 == Text.size() || !llvm::isHexDigit(Text[I + 1]))
        return false;
      while (I + 1 < Text.size() && llvm::isHexDigit(Text[I + 1]))
        if ((Value = Value * 16 + llvm::hexDigitValue(Text[++I])) > 0xFF)
          return false;
    } else {
      static constexpr llvm::StringLiteral Escapes = "n\nt\tr\ra\ab\bf\fv\v"
                                                     "\\\\\"\"''??";
      size_t At = 0;
      while (At < Escapes.size() && Escapes[At] != Text[I])
        At += 2;
      if (At == Escapes.size())
        return false;
      Value = static_cast<unsigned char>(Escapes[At + 1]);
    }
    Bytes += static_cast<char>(Value);
  }
  return true;
}
} // namespace

std::optional<std::string> DialectPrinter::stringBytes(const Expr *E) {
  std::string Bytes;
  const auto Append = [&](llvm::StringRef Text) {
    return Text.consume_front("\"") && Text.consume_back("\"") &&
           appendLiteralBytes(Text, Bytes);
  };
  if (E->Ops.empty() && !Append(E->Text))
    return std::nullopt;
  for (const Expr *Part : E->Ops)
    if (!Append(Part->Text))
      return std::nullopt;
  return Bytes;
}

std::optional<std::string> DialectPrinter::listBytes(const Expr *Init,
                                                     uint64_t Count) {
  std::string Bytes;
  uint64_t Next = 0;
  for (const Expr *Item : Init->Ops) {
    if (Item->Kind == ExprKind::Designated) {
      if (Item->Ops.size() != 2 || !Item->Ops[0]->Value ||
          *Item->Ops[0]->Value < 0)
        return std::nullopt;
      Next = static_cast<uint64_t>(*Item->Ops[0]->Value);
      Item = Item->Ops[1];
    }
    std::string Byte;
    llvm::StringRef Text = Item->Text;
    if (Item->Kind == ExprKind::Character) {
      if (!Text.consume_front("'") || !Text.consume_back("'") ||
          !appendLiteralBytes(Text, Byte) || Byte.size() != 1)
        return std::nullopt;
    } else if (Item->Value && *Item->Value >= 0 && *Item->Value <= 0xFF) {
      Byte = static_cast<char>(*Item->Value);
    } else {
      return std::nullopt;
    }
    if (Next >= Count)
      return std::nullopt;
    if (Bytes.size() <= Next)
      Bytes.resize(Next + 1);
    Bytes[Next++] = Byte.front();
  }
  return Bytes;
}

bool DialectPrinter::byteArray(const CType *Ty) {
  return Ty->isArray() && Ty->Count && Ty->Inner->isInteger() &&
         Ty->Inner->Bits == 8 && !Ty->Inner->Signed;
}

Printed DialectPrinter::withComments(const Expr *E, Printed P) const {
  if (E->Leading.empty() && E->Trailing.empty())
    return P;
  std::string Text;
  for (const Comment &C : E->Leading)
    Text += commentText(C) + " ";
  Text += P.Text;
  for (const Comment &C : E->Trailing)
    Text += " " + commentText(C);
  P.Text = std::move(Text);
  return P;
}

bool DialectPrinter::sourceType(llvm::StringRef Identifier) const {
  const CSourceName *Entry = NameEntries.lookup(Identifier);
  return Entry && Entry->TheKind == CSourceName::Kind::Type;
}

std::string DialectPrinter::name(llvm::StringRef Identifier) const {
  if (auto It = SourceNames.find(Identifier); It != SourceNames.end()) {
    // Type spellings are not navigable function or object identities.
    if (sourceType(Identifier))
      return It->second;
    return NameOpen + Identifier.str() + NameClose + It->second + NameEnd;
  }
  return escapeIdentifier(Identifier);
}

bool DialectPrinter::repeatsName(const TopLevel &Comment,
                                 const TopLevel *Next) const {
  if (!Next || Next->TheKind != TopLevel::Kind::Declaration ||
      Next->Decls.empty())
    return false;
  llvm::StringRef Text = Comment.TheComment.Text;
  if (!Text.consume_front("/* ") || !Text.consume_back(" */"))
    return false;
  auto Spelled = sourceSpelling(Next->Decls.front()->Name);
  return Spelled && *Spelled == Text;
}

namespace {

/// Visits the statements a `break` or `continue` in \p S could reach,
/// without entering the loops (and, for \p StopAtSwitch, the switches) that
/// would take it instead.
template <typename Fn>
bool anyStatement(const Stmt *S, bool StopAtSwitch, Fn &&Match) {
  if (!S)
    return false;
  if (Match(S))
    return true;
  switch (S->Kind) {
  case csyntax::StmtKind::While:
  case csyntax::StmtKind::DoWhile:
  case csyntax::StmtKind::For:
    return false;
  case csyntax::StmtKind::Switch:
    if (StopAtSwitch)
      return false;
    return anyStatement(S->Then, StopAtSwitch, Match);
  case csyntax::StmtKind::Compound:
    for (const Stmt *Child : S->Body)
      if (anyStatement(Child, StopAtSwitch, Match))
        return true;
    return false;
  case csyntax::StmtKind::If:
    return anyStatement(S->Then, StopAtSwitch, Match) ||
           anyStatement(S->Else, StopAtSwitch, Match);
  case csyntax::StmtKind::Try:
    if (anyStatement(S->Then, StopAtSwitch, Match))
      return true;
    for (const Handler &H : S->Handlers)
      if (anyStatement(H.Body, StopAtSwitch, Match))
        return true;
    return false;
  default:
    return false;
  }
}

} // namespace

bool DialectPrinter::continuesLoop(const Stmt *Body) {
  return anyStatement(Body, /*StopAtSwitch=*/false, [](const Stmt *S) {
    return S->Kind == csyntax::StmtKind::Continue;
  });
}

bool DialectPrinter::breaksOut(const Stmt *Body) {
  return anyStatement(Body, /*StopAtSwitch=*/true, [](const Stmt *S) {
    return S->Kind == csyntax::StmtKind::Break;
  });
}

bool DialectPrinter::callsNoreturn(const Expr *E) const {
  while (E && (E->Kind == ExprKind::Paren || E->Kind == ExprKind::Cast ||
               E->Kind == ExprKind::Comma))
    E = E->Kind == ExprKind::Comma ? E->Ops.front() : E->Ops.front();
  if (!E || E->Kind != ExprKind::Call)
    return false;
  const Expr *Callee = E->Ops.front();
  if (Callee->Kind != ExprKind::Name || !Callee->Ref)
    return false;
  const Decl *D = Callee->Ref;
  if (D->Noreturn)
    return true;
  for (llvm::StringRef A : D->Attributes)
    if (A.contains("noreturn"))
      return true;
  return false;
}

bool DialectPrinter::jumps(const Stmt *S) const {
  switch (S->Kind) {
  case csyntax::StmtKind::Break:
  case csyntax::StmtKind::Continue:
  case csyntax::StmtKind::Return:
  case csyntax::StmtKind::Goto:
  case csyntax::StmtKind::Throw:
  case csyntax::StmtKind::Leave:
    return true;
  case csyntax::StmtKind::Expression:
    return callsNoreturn(S->Value);
  case csyntax::StmtKind::Compound:
    for (auto It = S->Body.rbegin(); It != S->Body.rend(); ++It)
      if ((*It)->Kind != csyntax::StmtKind::Comment)
        return jumps(*It);
    return false;
  case csyntax::StmtKind::If:
    return S->Else && jumps(S->Then) && jumps(S->Else);
  case csyntax::StmtKind::While:
    // `while (1)` without a break never completes.
    return S->Value->Value && *S->Value->Value != 0 && !breaksOut(S->Then);
  default:
    return false;
  }
}

std::optional<std::vector<SwitchArm>>
DialectPrinter::switchArms(const Stmt *Switch) const {
  const Stmt *Body = Switch->Then;
  if (Body->Kind != csyntax::StmtKind::Compound)
    return std::nullopt;
  std::vector<SwitchArm> Arms;
  for (const Stmt *S : Body->Body) {
    const bool IsLabel = S->Kind == csyntax::StmtKind::Case ||
                         S->Kind == csyntax::StmtKind::Default;
    if (IsLabel) {
      if (Arms.empty() || !Arms.back().Body.empty())
        Arms.emplace_back();
      Arms.back().Labels.push_back(S);
      continue;
    }
    if (Arms.empty()) {
      if (S->Kind == csyntax::StmtKind::Comment)
        continue;
      return std::nullopt;
    }
    Arms.back().Body.push_back(S);
  }
  for (size_t I = 0; I + 1 < Arms.size(); ++I) {
    const std::vector<const Stmt *> &B = Arms[I].Body;
    auto Last = std::find_if(B.rbegin(), B.rend(), [](const Stmt *S) {
      return S->Kind != csyntax::StmtKind::Comment;
    });
    Arms[I].FallsThrough = Last == B.rend() || !jumps(*Last);
  }
  return Arms;
}

bool DialectPrinter::isUnknownValue(const Expr *E) {
  E = skipParens(E);
  return E->Kind == ExprKind::Comma && E->Ops.size() == 2 &&
         E->Ops[0]->Kind == ExprKind::Call &&
         E->Ops[0]->Ops.front()->Kind == ExprKind::Name &&
         E->Ops[0]->Ops.front()->Text == "__builtin_trap";
}

const Expr *DialectPrinter::unalignedLoadSource(const Expr *E) {
  E = skipParens(E);
  if (E->Kind != ExprKind::Comma || E->Ops.size() != 2)
    return nullptr;
  const Expr *Copy = E->Ops[0], *Result = E->Ops[1];
  if (Copy->Kind != ExprKind::Call || Copy->Ops.size() != 4 ||
      Copy->Ops[0]->Kind != ExprKind::Name ||
      Copy->Ops[0]->Text != "__builtin_memcpy" ||
      Result->Kind != ExprKind::Name)
    return nullptr;
  const Expr *Dest = Copy->Ops[1];
  const Expr *Size = skipParens(Copy->Ops[3]);
  if (Dest->Kind != ExprKind::Unary || Dest->Text != "&" ||
      Dest->Ops.front()->Kind != ExprKind::Name ||
      Dest->Ops.front()->Text != Result->Text ||
      Size->Kind != ExprKind::SizeofExpr)
    return nullptr;
  const Expr *Measured = skipParens(Size->Ops.front());
  if (Measured->Kind != ExprKind::Name || Measured->Text != Result->Text)
    return nullptr;
  return Copy->Ops[2];
}

void DialectPrinter::forEachSubExpr(
    const Expr *E, llvm::function_ref<void(const Expr *)> Visit) {
  if (!E)
    return;
  Visit(E);
  for (const Expr *Op : E->Ops)
    forEachSubExpr(Op, Visit);
  if (E->Body)
    forEachExpr(E->Body, Visit);
}

void DialectPrinter::forEachExpr(const Stmt *S,
                                 llvm::function_ref<void(const Expr *)> Visit) {
  if (!S)
    return;
  forEachSubExpr(S->Value, Visit);
  forEachSubExpr(S->Step, Visit);
  for (const Decl *D : S->Decls)
    forEachSubExpr(D->Init, Visit);
  forEachExpr(S->Then, Visit);
  forEachExpr(S->Else, Visit);
  for (const Stmt *Child : S->Body)
    forEachExpr(Child, Visit);
  for (const Handler &H : S->Handlers) {
    forEachSubExpr(H.Filter, Visit);
    forEachExpr(H.Body, Visit);
  }
}

void DialectPrinter::noteLoadScratch(const Stmt *Body) {
  LoadScratch.clear();
  llvm::DenseSet<const Expr *> InLoads;
  llvm::DenseSet<const Decl *> Loaded, UsedElsewhere;
  forEachExpr(Body, [&](const Expr *E) {
    if (!unalignedLoadSource(E))
      return;
    const Expr *Comma = skipParens(E);
    const Expr *Var = Comma->Ops[1];
    if (!Var->Ref || Var->Ref->Kind != DeclKind::Variable)
      return;
    Loaded.insert(Var->Ref);
    // `&v`, `sizeof(v)` and the result are the idiom's own uses.
    const Expr *Copy = Comma->Ops[0];
    InLoads.insert(Copy->Ops[1]->Ops.front());
    InLoads.insert(skipParens(skipParens(Copy->Ops[3])->Ops.front()));
    InLoads.insert(Var);
  });
  forEachExpr(Body, [&](const Expr *E) {
    if (E->Kind == ExprKind::Name && E->Ref && !InLoads.contains(E))
      UsedElsewhere.insert(E->Ref);
  });
  for (const Decl *D : Loaded)
    if (!UsedElsewhere.contains(D))
      LoadScratch.insert(D);
}

const Expr *DialectPrinter::narrowable(const Expr *E, const CType *Want) {
  if (!Want || !Want->isInteger() || Want->TheKind == CType::Kind::Bool)
    return nullptr;
  E = skipParens(E);
  auto Integral = [](const CType *Ty) {
    return Ty && Ty->TheKind == CType::Kind::Integer;
  };
  switch (E->Kind) {
  case ExprKind::Cast: {
    const Expr *Op = E->Ops.front();
    const CType *From = Op->Ty && Op->Ty->isArray() ? nullptr : Op->Ty;
    if (Integral(E->Written) && Integral(From) &&
        Want->Bits <= E->Written->Bits)
      return E;
    return nullptr;
  }
  case ExprKind::Binary: {
    const llvm::StringRef O = E->Text;
    const bool LowBits =
        O == "+" || O == "-" || O == "*" || O == "&" || O == "|" || O == "^";
    if (LowBits && Integral(E->OpTy) && Want->Bits <= E->OpTy->Bits)
      return E;
    return nullptr;
  }
  case ExprKind::Unary:
    if ((E->Text == "-" || E->Text == "~") && Integral(E->OpTy) &&
        Want->Bits <= E->OpTy->Bits)
      return E;
    return nullptr;
  default:
    return nullptr;
  }
}

void DialectPrinter::unsupported(const llvm::Twine &Reason) const {
  if (Refusal.empty())
    Refusal = Reason.str();
}

std::optional<size_t> SourceDialectText::mapOffset(size_t CBegin) const {
  const SourceDialectPiece *First = nullptr;
  for (const SourceDialectPiece &P : Pieces)
    if (P.CBegin >= CBegin &&
        (!First || P.CBegin < First->CBegin ||
         (P.CBegin == First->CBegin && P.Begin < First->Begin)))
      First = &P;
  if (!First)
    return std::nullopt;
  return First->Begin;
}

std::optional<std::pair<size_t, size_t>>
SourceDialectText::mapExact(llvm::StringRef C, size_t CBegin,
                            size_t CEnd) const {
  if (CBegin >= CEnd || CEnd > C.size())
    return std::nullopt;
  const llvm::StringRef Piece = C.slice(CBegin, CEnd).trim();
  if (Piece.empty())
    return std::nullopt;
  CBegin = Piece.data() - C.data();
  CEnd = CBegin + Piece.size();
  std::optional<std::pair<size_t, size_t>> Mapped;
  for (const SourceDialectPiece &P : Pieces) {
    if (P.CBegin != CBegin || P.CEnd != CEnd)
      continue;
    if (P.Begin >= P.End || P.End > Text.size())
      return std::nullopt;
    const auto Span = std::make_pair(P.Begin, P.End);
    if (Mapped && *Mapped != Span)
      return std::nullopt;
    Mapped = Span;
  }
  return Mapped;
}

std::optional<std::pair<size_t, size_t>>
SourceDialectText::map(size_t CBegin, size_t CEnd) const {
  std::optional<std::pair<size_t, size_t>> Hull;
  for (const SourceDialectPiece &P : Pieces)
    if (P.CBegin >= CBegin && P.CEnd <= CEnd) {
      if (!Hull)
        Hull = {P.Begin, P.End};
      Hull->first = std::min(Hull->first, P.Begin);
      Hull->second = std::max(Hull->second, P.End);
    }
  if (Hull)
    return Hull;
  const SourceDialectPiece *Around = nullptr;
  for (const SourceDialectPiece &P : Pieces)
    if (P.CBegin <= CBegin && P.CEnd >= CEnd &&
        (!Around || P.CEnd - P.CBegin < Around->CEnd - Around->CBegin))
      Around = &P;
  if (Around)
    return std::make_pair(Around->Begin, Around->End);
  return std::nullopt;
}
