//===- CSyntaxSema.cpp - The C types of emitted C's expressions -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Gives each expression of emitted C the type C gives it and the type its
/// operator computes in, from the declarations of the text and of the
/// headers it includes (CHeaderNames.def).  A declaration whose code uses a
/// name or a construct the checker cannot type becomes Unread: a reader then
/// shows it as C rather than guessing what it means.
///
//===----------------------------------------------------------------------===//

#include "../LibraryPrototype.h"
#include "CSyntaxTree.h"

#include "neverd/libc/LibCNames.h"

#include "llvm/ADT/StringExtras.h"

using namespace neverd;
using namespace neverd::csyntax;

namespace {

enum class GenericResult : uint8_t { Bool, Void, Int, Pointee };

const llvm::StringMap<llvm::StringRef> &headerPrototypes() {
  static const llvm::StringMap<llvm::StringRef> Prototypes = [] {
    llvm::StringMap<llvm::StringRef> Result;
    auto Add = [&](llvm::StringRef Prototype) {
      // The name is the identifier before the parameter list.
      const llvm::StringRef Head =
          Prototype.take_until([](char C) { return C == '('; });
      const size_t Start = Head.find_last_of(" *") + 1;
      Result[Head.drop_front(Start)] = Prototype;
    };
#define NEVERD_C_HEADER_FUNCTION(Prototype) Add(Prototype);
#include "neverd/backend/c/dialect/CHeaderNames.def"
    return Result;
  }();
  return Prototypes;
}

std::optional<int64_t> headerConstant(llvm::StringRef Name) {
#define NEVERD_C_HEADER_CONSTANT(HeaderName, Value)                            \
  if (Name == HeaderName)                                                      \
    return Value;
#include "neverd/backend/c/dialect/CHeaderNames.def"
  return std::nullopt;
}

std::optional<GenericResult> genericBuiltin(llvm::StringRef Name) {
#define NEVERD_C_HEADER_GENERIC(HeaderName, Result)                            \
  if (Name == HeaderName)                                                      \
    return GenericResult::Result;
#include "neverd/backend/c/dialect/CHeaderNames.def"
  return std::nullopt;
}

/// \p V as \p T holds it.
int64_t truncateTo(int64_t V, const CType *T) {
  if (!T->isInteger() || T->Bits >= 64 || T->Bits == 0)
    return V;
  const uint64_t Mask = (uint64_t(1) << T->Bits) - 1;
  uint64_t U = static_cast<uint64_t>(V) & Mask;
  if (T->Signed && (U >> (T->Bits - 1)) & 1)
    U |= ~Mask;
  return static_cast<int64_t>(U);
}

/// The number of characters a string literal's text spells, without its
/// terminator.
uint64_t literalLength(llvm::StringRef Text) {
  const size_t Open = Text.find('"');
  llvm::StringRef Body = Text.slice(Open + 1, Text.size() - 1);
  uint64_t Count = 0;
  for (size_t I = 0; I < Body.size(); ++I, ++Count) {
    if (Body[I] != '\\')
      continue;
    ++I;
    if (I >= Body.size())
      break;
    if (Body[I] == 'x') {
      while (I + 1 < Body.size() && llvm::isHexDigit(Body[I + 1]))
        ++I;
    } else if (Body[I] >= '0' && Body[I] <= '7') {
      for (int Digits = 1; Digits < 3 && I + 1 < Body.size() &&
                           Body[I + 1] >= '0' && Body[I + 1] <= '7';
           ++Digits)
        ++I;
    }
  }
  return Count;
}

class Checker {
public:
  explicit Checker(Tree &Result) : T(Result), Types(Result.Types) {}

  void run() {
    Scopes.emplace_back();
    // File-scope names first, so that a use before its declaration still
    // finds it.
    for (TopLevel &Item : T.Items)
      if (Item.TheKind == TopLevel::Kind::Declaration)
        for (Decl *D : Item.Decls) {
          declare(D);
          if (D->Kind == DeclKind::Enumeration)
            for (Decl *Enumerator : D->Fields)
              declare(Enumerator);
        }
    for (TopLevel &Item : T.Items) {
      Error.clear();
      if (Item.TheKind == TopLevel::Kind::Declaration) {
        for (Decl *D : Item.Decls)
          if (!topLevelDecl(D))
            break;
      } else if (Item.TheKind == TopLevel::Kind::StaticAssert) {
        if (value(Item.Assertion))
          value(Item.Message);
      }
      if (!Error.empty()) {
        Item.TheKind = TopLevel::Kind::Unread;
        Item.Text = T.Source.slice(Item.Begin, Item.End);
        Item.Reason = Error;
      }
      Scopes.resize(1);
    }
  }

private:
  bool fail(const llvm::Twine &What, const Expr *At = nullptr) {
    if (Error.empty()) {
      Error = What.str();
      if (At)
        Error += " (byte " + std::to_string(At->Begin) + ")";
    }
    return false;
  }

  //===--- Names ---===//

  void declare(const Decl *D) {
    if (D->Name.empty() || D->Kind == DeclKind::Typedef ||
        D->Kind == DeclKind::Record || D->Kind == DeclKind::Enumeration)
      return;
    const Decl *&Slot = Scopes.back()[D->Name];
    // A definition, or a declaration with named parameters, says most.
    if (!Slot || D->Body ||
        (Slot->Kind == DeclKind::Function && !Slot->Body &&
         D->Kind == DeclKind::Function))
      Slot = D;
  }

  const Decl *lookup(llvm::StringRef Name) {
    for (auto It = Scopes.rbegin(); It != Scopes.rend(); ++It)
      if (auto Found = It->find(Name); Found != It->end())
        return Found->second;
    if (auto It = HeaderDecls.find(Name); It != HeaderDecls.end())
      return It->second;
    llvm::StringRef Prototype;
    const auto &Prototypes = headerPrototypes();
    if (auto It = Prototypes.find(Name); It != Prototypes.end())
      Prototype = It->second;
    else if (const libc::LibCPrototype *P =
                 libc::libcPrototype(Name.str(), T.Format))
      Prototype = T.Sources.emplace_back(libcDeclaration(*P));
    const Decl *Result = nullptr;
    if (!Prototype.empty())
      for (TopLevel &Item : parseDeclarations(Prototype, T))
        if (Item.TheKind == TopLevel::Kind::Declaration && !Item.Decls.empty())
          Result = Item.Decls.front();
    HeaderDecls[Name] = Result;
    return Result;
  }

  /// The C declaration of a C runtime routine NeverD knows.
  static std::string libcDeclaration(const libc::LibCPrototype &P) {
    std::string Function = std::string(P.Name) + "(";
    for (unsigned I = 0; I < P.ParamCount; ++I) {
      std::string Param(P.Params[I]);
      // The Windows API convention is spelled as MSVC spells it.
      for (size_t At = Param.find(libc::kWinapiMarker); At != std::string::npos;
           At = Param.find(libc::kWinapiMarker))
        Param.replace(At, libc::kWinapiMarker.size(), "__stdcall ");
      Function += (I ? ", " : "") + Param;
    }
    if (P.Variadic)
      Function += P.ParamCount ? ", ..." : "...";
    else if (!P.ParamCount)
      Function += "void";
    return c_library::declarator(P.Return, Function + ")") + ";";
  }

  //===--- Types ---===//

  /// The type an expression of \p Ty yields as a value (C17 6.3.2.1).
  const CType *rvalue(const CType *Ty) {
    if (Ty->isArray())
      return Types.pointerTo(Ty->Inner);
    if (Ty->isFunction())
      return Types.pointerTo(Ty);
    return Types.unqualified(Ty);
  }

  /// Checks \p E and gives its value's type, or null after a failure.
  const CType *value(Expr *E) {
    const CType *Ty = expr(E);
    return Ty ? rvalue(Ty) : nullptr;
  }

  bool isNullConstant(const Expr *E) const {
    while (E->Kind == ExprKind::Paren ||
           (E->Kind == ExprKind::Cast && E->Written->isPointer() &&
            E->Written->Inner->isVoid()))
      E = E->Ops.front();
    return E->Value && *E->Value == 0 && E->Ty && E->Ty->isInteger();
  }

  /// Whether a value of \p From may initialize or be assigned to \p To.
  /// C's constraints are followed loosely where GNU C converts with a
  /// warning (integers and pointers); a reader still spells the conversion.
  bool assignable(const CType *From, const CType *To, const Expr *E) {
    To = Types.unqualified(To);
    if (From->isUnknown() || To->isUnknown())
      return true;
    if (From->isScalar() && To->isScalar())
      return true;
    if (To->TheKind == CType::Kind::Bool && From->isScalar())
      return true;
    if (From == To)
      return true;
    if (From->TheKind == CType::Kind::Named &&
        To->TheKind == CType::Kind::Named && From->Spelling == To->Spelling)
      return true;
    if (From->isRecord() && To->isRecord() && From->Record == To->Record)
      return true;
    // The text converts what C would not; a reader shows it as written.
    (void)E;
    return true;
  }

  //===--- Expressions ---===//

  const CType *expr(Expr *E) {
    if (!E)
      return nullptr;
    if (!exprInner(E))
      return nullptr;
    return E->Ty;
  }

  bool setType(Expr *E, const CType *Ty, bool LValue = false) {
    E->Ty = Ty;
    E->LValue = LValue;
    return Ty != nullptr;
  }

  bool exprInner(Expr *E) {
    switch (E->Kind) {
    case ExprKind::Name:
      return name(E);
    case ExprKind::Integer:
      return integerLiteral(E);
    case ExprKind::Floating: {
      const llvm::StringRef Text = E->Text;
      if (Text.ends_with_insensitive("f"))
        return setType(E, Types.floating(32, "float"));
      if (Text.ends_with_insensitive("l"))
        return setType(
            E, Types.floating(Types.model().LongDoubleBits, "long double"));
      return setType(E, Types.doubleType());
    }
    case ExprKind::Character:
      return setType(E, Types.intType());
    case ExprKind::String: {
      const bool Wide = E->Text.starts_with("L");
      uint64_t Length = 0;
      if (E->Ops.empty())
        Length = literalLength(E->Text);
      for (const Expr *Part : E->Ops)
        Length += literalLength(Part->Text);
      const CType *Element =
          Wide ? Types.integer(Types.model().WCharBits,
                               Types.model().WCharSigned, "wchar_t")
               : Types.charType();
      return setType(E, Types.arrayOf(Element, Length + 1), true);
    }
    case ExprKind::Paren: {
      Expr *Inner = E->Ops.front();
      if (!expr(Inner))
        return false;
      E->Value = Inner->Value;
      return setType(E, Inner->Ty, Inner->LValue);
    }
    case ExprKind::Unary:
      return unary(E);
    case ExprKind::Binary:
      return binary(E);
    case ExprKind::Assign:
      return assign(E);
    case ExprKind::Conditional:
      return conditional(E);
    case ExprKind::Comma: {
      const CType *Last = nullptr;
      for (Expr *Op : E->Ops)
        if (!(Last = value(Op)))
          return false;
      return setType(E, Last);
    }
    case ExprKind::Cast: {
      const CType *From = value(E->Ops.front());
      if (!From)
        return false;
      const CType *To = Types.unqualified(E->Written);
      if (!To->isVoid() && !To->isScalar() && !To->isUnknown() &&
          To->TheKind != CType::Kind::Named)
        return fail("cast to a non-scalar type", E);
      if (E->Ops.front()->Value && To->isInteger())
        E->Value = truncateTo(*E->Ops.front()->Value, To);
      return setType(E, To);
    }
    case ExprKind::Call:
      return call(E);
    case ExprKind::Subscript: {
      const CType *Base = value(E->Ops[0]);
      const CType *Index = Base ? value(E->Ops[1]) : nullptr;
      if (!Index)
        return false;
      if (Index->isPointer() && Base->isInteger())
        std::swap(Base, Index);
      if (!Base->isPointer() || !Index->isInteger())
        return fail("subscript of a non-pointer", E);
      return setType(E, Base->Inner, true);
    }
    case ExprKind::Member:
      return member(E);
    case ExprKind::SizeofExpr: {
      const CType *Of = expr(E->Ops.front());
      if (!Of)
        return false;
      if (auto Size = Types.sizeOf(Of))
        E->Value = static_cast<int64_t>(*Size);
      return setType(E, Types.sizeType());
    }
    case ExprKind::SizeofType:
    case ExprKind::AlignofType:
      if (E->Kind == ExprKind::SizeofType)
        if (auto Size = Types.sizeOf(E->Written))
          E->Value = static_cast<int64_t>(*Size);
      return setType(E, Types.sizeType());
    case ExprKind::Offsetof: {
      const CType *Of = Types.unqualified(E->Written);
      if (!Of->isRecord() || !Of->Record->Complete)
        return fail("offsetof of an incomplete record", E);
      bool Through = false;
      E->Field = field(*Of->Record, E->Text, Through);
      E->ThroughAnonymous = Through;
      if (!E->Field || E->Field->BitWidth)
        return fail("offsetof of no field '" + E->Text + "'", E);
      return setType(E, Types.sizeType());
    }
    case ExprKind::CompoundLiteral:
      if (!initializer(E->Ops.front(), E->Written))
        return false;
      return setType(E, E->Written, true);
    case ExprKind::InitList:
    case ExprKind::Designated:
      return fail("an initializer list outside an initializer", E);
    case ExprKind::StatementExpr: {
      Scopes.emplace_back();
      const CType *Result = Types.voidType();
      for (Stmt *S : E->Body->Body) {
        if (!stmt(S)) {
          Scopes.pop_back();
          return false;
        }
        if (S->Kind == StmtKind::Expression)
          Result = rvalue(S->Value->Ty);
        else if (S->Kind != StmtKind::Comment)
          Result = Types.voidType();
      }
      Scopes.pop_back();
      return setType(E, Result);
    }
    case ExprKind::BitCast: {
      const CType *From = value(E->Ops.front());
      if (!From)
        return false;
      return setType(E, Types.unqualified(E->Written));
    }
    case ExprKind::VaArg:
      if (!value(E->Ops.front()))
        return false;
      return setType(E, Types.unqualified(E->Written));
    case ExprKind::Asm:
      return setType(E, Types.unknownType());
    }
    return fail("unknown expression", E);
  }

  bool name(Expr *E) {
    if (const Decl *D = lookup(E->Text)) {
      E->Ref = D;
      // An enumerator is an int constant (C17 6.4.4.3).
      if (D->Kind == DeclKind::Enumerator) {
        if (!D->Value)
          return fail("an enumerator used before its value", E);
        E->Value = *D->Value;
        return setType(E, Types.intType());
      }
      return setType(E, D->Ty, D->Kind != DeclKind::Function);
    }
    if (auto Value = headerConstant(E->Text)) {
      E->Value = *Value;
      return setType(E, Types.intType());
    }
    // A name the text uses without declaring it (which C would refuse) is
    // shown as written.
    return setType(E, Types.unknownType(), true);
  }

  bool integerLiteral(Expr *E) {
    llvm::StringRef Text = E->Text;
    const llvm::StringRef Digits = Text.rtrim("uUlL");
    const std::string Suffix = Text.drop_front(Digits.size()).lower();
    uint64_t V = 0;
    if (Digits.getAsInteger(0, V))
      return fail("integer literal out of range", E);
    const bool Decimal = !Digits.starts_with("0") || Digits == "0";
    const bool U = llvm::StringRef(Suffix).contains('u');
    const unsigned L = llvm::StringRef(Suffix).count('l');
    const unsigned LongBits = Types.model().LongBits;
    struct Candidate {
      unsigned Bits;
      bool Signed;
      const char *Spelling;
    };
    llvm::SmallVector<Candidate, 6> Candidates;
    auto Add = [&](unsigned Bits, bool Signed, const char *Spelling,
                   unsigned MinLong) {
      if (L <= MinLong && (!U || !Signed) && (Signed || !Decimal || U))
        Candidates.push_back({Bits, Signed, Spelling});
    };
    Add(32, true, "int", 0);
    Add(32, false, "unsigned int", 0);
    Add(LongBits, true, "long", 1);
    Add(LongBits, false, "unsigned long", 1);
    Add(64, true, "long long", 2);
    Add(64, false, "unsigned long long", 2);
    for (const Candidate &C : Candidates) {
      const unsigned ValueBits = C.Signed ? C.Bits - 1 : C.Bits;
      if (ValueBits >= 64 || V < (uint64_t(1) << ValueBits)) {
        E->Value = static_cast<int64_t>(V);
        return setType(E, C.Spelling == llvm::StringRef("int")
                              ? Types.intType()
                              : Types.integer(C.Bits, C.Signed, C.Spelling));
      }
    }
    E->Value = static_cast<int64_t>(V);
    return setType(E, Types.integer(64, false, "unsigned long long"));
  }

  bool unary(Expr *E) {
    Expr *Op = E->Ops.front();
    const llvm::StringRef O = E->Text;
    if (O == "&") {
      const CType *Ty = expr(Op);
      if (!Ty)
        return false;
      if (!Op->LValue && !Ty->isFunction())
        return fail("address of a value", E);
      return setType(E, Types.pointerTo(Ty));
    }
    if (O == "*") {
      const CType *Ty = value(Op);
      if (!Ty)
        return false;
      if (!Ty->isPointer() || Ty->Inner->isVoid())
        return fail("dereference of a non-pointer", E);
      return setType(E, Ty->Inner, !Ty->Inner->isFunction());
    }
    if (O == "++" || O == "--") {
      const CType *Ty = expr(Op);
      if (!Ty)
        return false;
      if (!Op->LValue || !rvalue(Ty)->isScalar())
        return fail("increment of a non-scalar", E);
      return setType(E, rvalue(Ty));
    }
    const CType *Ty = value(Op);
    if (!Ty)
      return false;
    if (Ty->isUnknown())
      return setType(E, Ty);
    if (O == "!") {
      if (!Ty->isScalar())
        return fail("'!' of a non-scalar", E);
      if (Op->Value)
        E->Value = *Op->Value == 0;
      return setType(E, Types.intType());
    }
    if (!(O == "~" ? Ty->isInteger() : Ty->isArithmetic()))
      return fail("'" + O + "' of a non-arithmetic value", E);
    const CType *Result = Types.promoted(Ty);
    E->OpTy = Result;
    if (Op->Value && Result->isInteger())
      E->Value = truncateTo(O == "-"   ? -*Op->Value
                            : O == "~" ? ~*Op->Value
                                       : *Op->Value,
                            Result);
    return setType(E, Result);
  }

  std::optional<int64_t> fold(llvm::StringRef O, int64_t A, int64_t B,
                              const CType *Ty) {
    const bool S = Ty->Signed;
    const uint64_t UA = static_cast<uint64_t>(A), UB = static_cast<uint64_t>(B);
    if (O == "+")
      return truncateTo(static_cast<int64_t>(UA + UB), Ty);
    if (O == "-")
      return truncateTo(static_cast<int64_t>(UA - UB), Ty);
    if (O == "*")
      return truncateTo(static_cast<int64_t>(UA * UB), Ty);
    if (O == "&")
      return A & B;
    if (O == "|")
      return A | B;
    if (O == "^")
      return truncateTo(A ^ B, Ty);
    if ((O == "<<" || O == ">>") && B >= 0 && B < 64)
      return O == "<<" ? truncateTo(static_cast<int64_t>(UA << B), Ty)
             : S       ? A >> B
                       : static_cast<int64_t>(UA >> B);
    return std::nullopt;
  }

  bool binary(Expr *E) {
    const CType *L = value(E->Ops[0]);
    const CType *R = L ? value(E->Ops[1]) : nullptr;
    if (!R)
      return false;
    const llvm::StringRef O = E->Text;
    if (L->isUnknown() || R->isUnknown()) {
      E->OpTy = L->isUnknown() ? R : L;
      const bool Logical = O == "&&" || O == "||" || O == "==" || O == "!=" ||
                           O == "<" || O == ">" || O == "<=" || O == ">=";
      return setType(E, Logical ? Types.intType() : Types.unknownType());
    }
    if (O == "&&" || O == "||") {
      if (!L->isScalar() || !R->isScalar())
        return fail("'" + O + "' of a non-scalar", E);
      return setType(E, Types.intType());
    }
    if (O == "==" || O == "!=" || O == "<" || O == ">" || O == "<=" ||
        O == ">=") {
      if (L->isArithmetic() && R->isArithmetic())
        E->OpTy = Types.usualArithmetic(L, R);
      else if (L->isPointer() && R->isPointer())
        E->OpTy = L->Inner->isVoid() ? L : R;
      else if (L->isPointer() && R->isInteger())
        E->OpTy = L;
      else if (R->isPointer() && L->isInteger())
        E->OpTy = R;
      else
        return fail("comparison of incompatible values", E);
      if (E->OpTy->isInteger() && E->Ops[0]->Value && E->Ops[1]->Value) {
        const int64_t A = *E->Ops[0]->Value, B = *E->Ops[1]->Value;
        if (O == "==")
          E->Value = A == B;
        else if (O == "!=")
          E->Value = A != B;
      }
      return setType(E, Types.intType());
    }
    if (O == "<<" || O == ">>") {
      if (!L->isInteger() || !R->isInteger())
        return fail("shift of a non-integer", E);
      E->OpTy = Types.promoted(L);
      if (E->Ops[0]->Value && E->Ops[1]->Value)
        E->Value = fold(O, *E->Ops[0]->Value, *E->Ops[1]->Value, E->OpTy);
      return setType(E, E->OpTy);
    }
    if ((O == "+" || O == "-") && (L->isPointer() || R->isPointer())) {
      if (L->isPointer() && R->isPointer()) {
        if (O != "-")
          return fail("sum of two pointers", E);
        E->OpTy = L;
        return setType(E, Types.ptrdiffType());
      }
      if (R->isPointer() && O == "-")
        return fail("integer minus pointer", E);
      const CType *Pointer = L->isPointer() ? L : R;
      const CType *Offset = L->isPointer() ? R : L;
      if (!Offset->isInteger())
        return fail("pointer offset is not an integer", E);
      if (Pointer->Inner->isFunction())
        return fail("arithmetic on a function pointer", E);
      E->OpTy = Pointer;
      return setType(E, Pointer);
    }
    const bool IntegerOnly = O == "%" || O == "&" || O == "|" || O == "^";
    if (IntegerOnly ? !(L->isInteger() && R->isInteger())
                    : !(L->isArithmetic() && R->isArithmetic()))
      return fail("'" + O + "' of incompatible values", E);
    E->OpTy = Types.usualArithmetic(L, R);
    if (E->OpTy->isInteger() && E->Ops[0]->Value && E->Ops[1]->Value)
      E->Value = fold(O, truncateTo(*E->Ops[0]->Value, E->OpTy),
                      truncateTo(*E->Ops[1]->Value, E->OpTy), E->OpTy);
    return setType(E, E->OpTy);
  }

  bool assign(Expr *E) {
    const CType *L = expr(E->Ops[0]);
    if (!L)
      return false;
    if (!E->Ops[0]->LValue)
      return fail("assignment to a value", E);
    const CType *Target = rvalue(L);
    const CType *R = value(E->Ops[1]);
    if (!R)
      return false;
    if (E->Text == "=") {
      if (!assignable(R, Target, E))
        return false;
      return setType(E, Target);
    }
    const llvm::StringRef O = E->Text.drop_back();
    if (Target->isUnknown() || R->isUnknown())
      E->OpTy = Target;
    else if (O == "<<" || O == ">>")
      E->OpTy = Types.promoted(Target);
    else if (Target->isPointer() && (O == "+" || O == "-") && R->isInteger())
      E->OpTy = Target;
    else if (Target->isArithmetic() && R->isArithmetic())
      E->OpTy = Types.usualArithmetic(Target, R);
    else
      return fail("'" + E->Text + "' of incompatible values", E);
    return setType(E, Target);
  }

  bool conditional(Expr *E) {
    const CType *C = value(E->Ops[0]);
    const CType *A = C ? value(E->Ops[1]) : nullptr;
    const CType *B = A ? value(E->Ops[2]) : nullptr;
    if (!B)
      return false;
    if (!C->isScalar() && !C->isUnknown())
      return fail("condition is not a scalar", E);
    const CType *Result = nullptr;
    if (A->isUnknown() || B->isUnknown())
      Result = A->isUnknown() ? B : A;
    else if (A->isArithmetic() && B->isArithmetic())
      Result = Types.usualArithmetic(A, B);
    else if (A->isPointer() && B->isPointer())
      Result = A == B ? A : A->Inner->isVoid() ? A : B;
    else if (A->isPointer() && isNullConstant(E->Ops[2]))
      Result = A;
    else if (B->isPointer() && isNullConstant(E->Ops[1]))
      Result = B;
    else if (A->isVoid() && B->isVoid())
      Result = A;
    else if (A == B)
      Result = A;
    else
      return fail("conditional arms of incompatible types", E);
    E->OpTy = Result;
    return setType(E, Result);
  }

  bool call(Expr *E) {
    Expr *Callee = E->Ops.front();
    llvm::SmallVector<const CType *, 8> Args;
    for (size_t I = 1; I < E->Ops.size(); ++I) {
      const CType *Arg = value(E->Ops[I]);
      if (!Arg)
        return false;
      Args.push_back(Arg);
    }
    if (Callee->Kind == ExprKind::Name && !lookup(Callee->Text)) {
      if (auto Generic = genericBuiltin(Callee->Text)) {
        Callee->Ty = Types.unknownType();
        switch (*Generic) {
        case GenericResult::Bool:
          return setType(E, Types.boolType());
        case GenericResult::Void:
          return setType(E, Types.voidType());
        case GenericResult::Int:
          return setType(E, Types.intType());
        case GenericResult::Pointee:
          if (Args.empty() || !Args.front()->isPointer())
            return fail("'" + Callee->Text + "' needs a pointer", E);
          return setType(E, Types.unqualified(Args.front()->Inner));
        }
      }
      // An intrinsic or a routine of the headers HighC includes: its
      // signature is the header's, as in the C text.
      Callee->Ty = Types.unknownType();
      return setType(E, Types.unknownType());
    }
    const CType *Fn = value(Callee);
    if (!Fn)
      return false;
    if (Fn->isUnknown())
      return setType(E, Fn);
    if (!Fn->isPointer() || !Fn->Inner->isFunction())
      return fail("call of a non-function", E);
    const CType *Signature = Fn->Inner;
    // Arguments the prototype does not match are shown as written.
    for (size_t I = 0; I < Args.size(); ++I)
      if (I < Signature->Params.size() && Signature->Prototyped &&
          !assignable(Args[I], Signature->Params[I], E->Ops[I + 1]))
        return false;
    return setType(E, Types.unqualified(Signature->Inner));
  }

  bool member(Expr *E) {
    const CType *Base = expr(E->Ops.front());
    if (!Base)
      return false;
    if (E->Postfix) {
      Base = rvalue(Base);
      if (Base->isUnknown())
        return setType(E, Base, true);
      if (!Base->isPointer())
        return fail("'->' of a non-pointer", E);
      Base = Base->Inner;
    }
    Base = Types.unqualified(Base);
    if (Base->isUnknown())
      return setType(E, Base, true);
    if (!Base->isRecord() || !Base->Record->Complete)
      return fail("member of an incomplete record", E);
    bool Through = false;
    E->Field = field(*Base->Record, E->Text, Through);
    E->ThroughAnonymous = Through;
    if (!E->Field)
      return fail("no field '" + E->Text + "'", E);
    return setType(E, E->Field->Type, true);
  }

  /// The field of \p Record named \p Name: its own, or a member of an
  /// anonymous structure or union it holds, which C counts as its own (C17
  /// 6.7.2.1p13), when \p Through says so.
  static const RecordField *field(const RecordDef &Record, llvm::StringRef Name,
                                  bool &Through) {
    for (const RecordField &Field : Record.Fields) {
      if (!Field.Name.empty() && Field.Name == Name)
        return &Field;
      if (!Field.isAnonymousRecord() || !Field.Type->Record->Complete)
        continue;
      bool Inner = false;
      if (const RecordField *Found = field(*Field.Type->Record, Name, Inner)) {
        Through = true;
        return Found;
      }
    }
    return nullptr;
  }

  bool initializer(Expr *Init, const CType *Ty) {
    if (Init->Kind != ExprKind::InitList) {
      // A string initializes a character array.
      if (Ty->isArray() && Init->Kind == ExprKind::String)
        return expr(Init) != nullptr;
      const CType *V = value(Init);
      return V && assignable(V, Ty, Init);
    }
    Init->Ty = Ty;
    const CType *Element = nullptr;
    if (Ty->isArray())
      Element = Ty->Inner;
    for (size_t I = 0; I < Init->Ops.size(); ++I) {
      Expr *Item = Init->Ops[I];
      const CType *ItemTy = Element;
      if (Item->Kind == ExprKind::Designated) {
        if (!Item->Ops.empty() && Item->Ops.size() == 2 &&
            !value(Item->Ops.front()))
          return false;
        if (!Item->Text.empty()) {
          if (!Ty->isRecord() || !Ty->Record->Complete)
            return fail("field designator outside a record", Item);
          ItemTy = nullptr;
          for (const RecordField &Field : Ty->Record->Fields)
            if (Field.Name == Item->Text)
              ItemTy = Field.Type;
        }
        if (!ItemTy)
          return fail("designator of an unknown member", Item);
        Item->Ty = ItemTy;
        if (!initializer(Item->Ops.back(), ItemTy))
          return false;
        continue;
      }
      if (Ty->isRecord()) {
        if (!Ty->Record->Complete || I >= Ty->Record->Fields.size())
          return fail("too many initializers", Item);
        ItemTy = Ty->Record->Fields[I].Type;
      } else if (!Element) {
        // `{0}` for a scalar.
        ItemTy = Ty;
      }
      if (!initializer(Item, ItemTy))
        return false;
    }
    return true;
  }

  //===--- Declarations and statements ---===//

  bool topLevelDecl(Decl *D) {
    if (D->Kind == DeclKind::Function && D->Body) {
      Scopes.emplace_back();
      for (Decl *P : D->Params)
        declare(P);
      ReturnType = Types.unqualified(D->Ty->Inner);
      const bool Ok = stmt(D->Body);
      Scopes.pop_back();
      return Ok;
    }
    if (D->Kind == DeclKind::Enumeration)
      return enumerators(D);
    if (D->Init)
      return initializer(D->Init, D->Ty);
    return true;
  }

  /// Each enumerator's value: its constant, or the one before it plus one;
  /// an enumerator int does not hold fails, as the enumeration would not
  /// be int's (C17 6.7.2.2).
  bool enumerators(Decl *D) {
    int64_t Next = 0;
    for (Decl *Enumerator : D->Fields) {
      if (Enumerator->Init) {
        if (!value(Enumerator->Init))
          return false;
        if (!Enumerator->Init->Value)
          return fail("an enumerator whose value is not a constant",
                      Enumerator->Init);
        Next = *Enumerator->Init->Value;
      }
      if (Next < INT32_MIN || Next > INT32_MAX)
        return fail("an enumerator int does not hold");
      Enumerator->Value = Next++;
    }
    return true;
  }

  bool condition(Expr *E) {
    const CType *Ty = value(E);
    if (!Ty)
      return false;
    if (!Ty->isScalar() && !Ty->isUnknown())
      return fail("condition is not a scalar", E);
    return true;
  }

  bool stmt(Stmt *S) {
    switch (S->Kind) {
    case StmtKind::Compound: {
      Scopes.emplace_back();
      for (Stmt *Child : S->Body)
        if (!stmt(Child)) {
          Scopes.pop_back();
          return false;
        }
      Scopes.pop_back();
      return true;
    }
    case StmtKind::Declaration:
      for (Decl *D : S->Decls) {
        if (D->Kind == DeclKind::Typedef || D->Kind == DeclKind::Record)
          return fail("a type declared inside a function");
        if (D->Init && !initializer(D->Init, D->Ty))
          return false;
        declare(D);
      }
      return true;
    case StmtKind::Expression:
      return value(S->Value) != nullptr;
    case StmtKind::If:
      return condition(S->Value) && stmt(S->Then) &&
             (!S->Else || stmt(S->Else));
    case StmtKind::While:
    case StmtKind::DoWhile:
      return condition(S->Value) && stmt(S->Then);
    case StmtKind::For:
      if (S->Value && !value(S->Value))
        return false;
      if (S->Else && !condition(S->Else->Value))
        return false;
      if (S->Step && !value(S->Step))
        return false;
      return stmt(S->Then);
    case StmtKind::Switch: {
      const CType *Ty = value(S->Value);
      if (!Ty)
        return false;
      if (!Ty->isInteger())
        return fail("switch on a non-integer", S->Value);
      return stmt(S->Then);
    }
    case StmtKind::Case: {
      if (!value(S->Value))
        return false;
      if (!S->Value->Value)
        return fail("case value is not a constant", S->Value);
      return true;
    }
    case StmtKind::Return: {
      if (!S->Value)
        return true;
      const CType *Ty = value(S->Value);
      if (!Ty)
        return false;
      return ReturnType->isVoid() || assignable(Ty, ReturnType, S->Value);
    }
    case StmtKind::Try:
      if (!stmt(S->Then))
        return false;
      for (Handler &H : S->Handlers) {
        if (H.Filter && !value(H.Filter))
          return false;
        Scopes.emplace_back();
        if (H.TheKind == Handler::Kind::Catch) {
          // `const T &name` names the exception, typed by C++ alone.
          const llvm::StringRef Name = H.Parameter.rsplit('&').second.trim();
          if (!Name.empty() && Name != "...") {
            Decl *P = T.newDecl(DeclKind::Variable, S->Begin);
            P->Name = Name;
            P->Ty = Types.unknownType();
            declare(P);
          }
        }
        const bool Ok = stmt(H.Body);
        Scopes.pop_back();
        if (!Ok)
          return false;
      }
      return true;
    case StmtKind::Throw:
      // C++ thrown objects and their constructors stay as written.
      return true;
    case StmtKind::Default:
    case StmtKind::Break:
    case StmtKind::Continue:
    case StmtKind::Goto:
    case StmtKind::Label:
    case StmtKind::Empty:
    case StmtKind::Comment:
    case StmtKind::Leave:
    case StmtKind::Asm:
      return true;
    }
    return fail("unknown statement");
  }

  Tree &T;
  TypeContext &Types;
  std::vector<llvm::StringMap<const Decl *>> Scopes;
  llvm::StringMap<const Decl *> HeaderDecls;
  const CType *ReturnType = nullptr;
  std::string Error;
};

} // namespace

void csyntax::check(Tree &Result) { Checker(Result).run(); }
