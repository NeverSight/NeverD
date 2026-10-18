//===- GoDialect.cpp - Emitted C spelled as Go ----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Spells emitted C in Go syntax with C's meaning.  Go has the C operators
/// that matter (`goto`, `switch` with C's `break`), but no conditional or
/// comma expressions: at the top of a statement they become statements, and
/// inside an expression a function literal computes them in C's order.  A
/// do-while is `for again := true; again; again = cond`, whose `continue`
/// reaches the condition as C's does.  Conversions between pointers and
/// integers, and from booleans, are written as plain conversions, which the
/// header says once.
///
//===----------------------------------------------------------------------===//

#include "DialectPrinter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"

using namespace neverd;
using namespace neverd::csyntax;

namespace {

const llvm::StringSet<> &goKeywords() {
  static const llvm::StringSet<> Words = [] {
    llvm::StringSet<> Result;
#define NEVERD_GO_KEYWORD(Word) Result.insert(Word);
#include "neverd/backend/c/dialect/DialectReservedWords.def"
    return Result;
  }();
  return Words;
}

struct BuiltinSpelling {
  llvm::StringRef Template;
  llvm::StringRef Result;
};

std::optional<BuiltinSpelling> goBuiltin(llvm::StringRef Name) {
#define NEVERD_DIALECT_BUILTIN(CName, Rust, RustResult, Go, GoResult)          \
  if (Name == CName)                                                           \
    return BuiltinSpelling{Go, GoResult};
#include "neverd/backend/c/dialect/DialectBuiltins.def"
  return std::nullopt;
}

/// Whether \p Symbol is a Go symbol: a package path and a name, as Go's
/// tools print it (`internal/cpu.Initialize`, `main.(*T).Method`).
bool isGoSymbol(llvm::StringRef Symbol) {
  if (Symbol.empty() || Symbol.contains(' ') || Symbol.starts_with("_Z") ||
      Symbol.starts_with("_R") || Symbol.starts_with("?"))
    return false;
  const size_t Dot = Symbol.find('.');
  if (Dot == llvm::StringRef::npos || Dot == 0)
    return false;
  // GCC's clones (`foo.cold`, `foo.part.0`) are C.
  const llvm::StringRef Suffix = Symbol.drop_front(Dot + 1);
  for (llvm::StringRef Clone :
       {"cold", "part.", "isra.", "constprop.", "lto_priv.", "localalias"})
    if (Suffix.starts_with(Clone))
      return false;
  return true;
}

class GoPrinter final : public DialectPrinter {
public:
  GoPrinter(const Tree &T, const SourceDialectOptions &Opts)
      : DialectPrinter(T, Opts) {
    for (const TopLevel &Item : T.Items)
      if (Item.TheKind == TopLevel::Kind::Declaration)
        for (const Decl *D : Item.Decls) {
          FileScope.insert(D);
          if (D->Body)
            Defined.insert(D->Name);
          if (D->Kind == DeclKind::Variable && !D->Extern)
            DefinedObjects.insert(D->Name);
        }
  }

private:
  //===--- DialectPrinter ---===//

  void header() override {
    line("// NeverD pseudocode in Go syntax, with the C view's meaning:");
    line("// (*T)(addr) points at an address, pointer and bool conversions "
         "are");
    line("// written as plain conversions, trap() traps, and \"...\" points "
         "at a C string.");
  }

  std::string escapeIdentifier(llvm::StringRef Name) const override {
    if (goKeywords().contains(Name))
      return (Name + "_").str();
    return Name.str();
  }

  std::optional<std::string> sourceName(llvm::StringRef Symbol) const override {
    if (!isGoSymbol(Symbol))
      return std::nullopt;
    return Symbol.str();
  }

  //===--- Types ---===//

  std::string type(const CType *Ty) const {
    switch (Ty->TheKind) {
    case CType::Kind::Void:
      return "";
    case CType::Kind::Bool:
      return "bool";
    case CType::Kind::Integer:
      return (Ty->Signed ? "int" : "uint") + std::to_string(Ty->Bits);
    case CType::Kind::Floating:
      return "float" + std::to_string(Ty->Bits);
    case CType::Kind::Pointer:
      if (Ty->Inner->AddressSpace)
        unsupported("a pointer into address space " +
                    llvm::Twine(Ty->Inner->AddressSpace));
      if (Ty->Inner->isVoid())
        return "unsafe.Pointer";
      if (Ty->Inner->isFunction())
        return functionType(Ty->Inner, {});
      return "*" + type(Ty->Inner);
    case CType::Kind::Array:
      return "[" + (Ty->Count ? std::to_string(*Ty->Count) : "") + "]" +
             type(Ty->Inner);
    case CType::Kind::Function:
      return functionType(Ty, {});
    case CType::Kind::Record:
      return llvm::StringRef(Ty->Spelling).drop_front(7).str();
    case CType::Kind::Named:
      return Ty->Spelling.empty() ? "any" : Ty->Spelling;
    }
    return "any";
  }

  /// A function C declares with `()` takes the promoted types of the
  /// \p Arguments of a call, when \p Called.
  std::string functionType(const CType *Fn,
                           llvm::ArrayRef<const CType *> Arguments,
                           bool Called = false) const {
    std::string Result = "func(";
    llvm::ArrayRef<const CType *> Params =
        Fn->Prototyped ? llvm::ArrayRef<const CType *>(Fn->Params) : Arguments;
    for (size_t I = 0; I < Params.size(); ++I)
      Result += (I ? ", " : "") + type(Params[I]);
    if (Fn->Variadic || (!Fn->Prototyped && !Called))
      Result += Params.empty() ? "...any" : ", ...any";
    Result += ")";
    if (!Fn->Inner->isVoid())
      Result += " " + type(Fn->Inner);
    return Result;
  }

  bool sameType(const CType *A, const CType *B) const {
    return type(A) == type(B);
  }

  //===--- Expressions ---===//

  static std::string paren(const Printed &P, int Min) {
    if (P.Prec >= Min)
      return P.Text;
    return "(" + P.Text + ")";
  }

  static std::string literalText(int64_t V, const CType *Ty,
                                 llvm::StringRef Like) {
    const bool Hex = Like.starts_with_insensitive("0x");
    if (Ty->Signed && V < 0)
      return "-" + (Hex ? "0x" + llvm::utohexstr(-static_cast<uint64_t>(V))
                        : std::to_string(-static_cast<uint64_t>(V)));
    const uint64_t U = Ty->Bits >= 64 ? static_cast<uint64_t>(V)
                                      : static_cast<uint64_t>(V) &
                                            ((uint64_t(1) << Ty->Bits) - 1);
    return Hex ? "0x" + llvm::utohexstr(U) : std::to_string(U);
  }

  static int64_t truncated(int64_t V, const CType *Ty) {
    if (!Ty->isInteger() || Ty->Bits >= 64)
      return V;
    const uint64_t Mask = (uint64_t(1) << Ty->Bits) - 1;
    uint64_t U = static_cast<uint64_t>(V) & Mask;
    if (Ty->Signed && (U >> (Ty->Bits - 1)) & 1)
      U |= ~Mask;
    return static_cast<int64_t>(U);
  }

  /// `T(x)`, or `(*T)(x)` where T does not start a name.
  std::string conversion(const CType *To, const Printed &P) const {
    std::string Ty = type(To);
    if (llvm::StringRef(Ty).starts_with("*") ||
        llvm::StringRef(Ty).starts_with("func"))
      Ty = "(" + Ty + ")";
    return Ty + "(" + P.Text + ")";
  }

  Printed convert(Printed P, const CType *From, const CType *To) {
    if (!To || !From || To->isVoid())
      return P;
    if (From->isUnknown() || To->isUnknown() || P.Diverges)
      return P;
    if (P.Bool) {
      if (To->TheKind == CType::Kind::Bool)
        return P;
      return Printed{conversion(To, P), PrecAtom};
    }
    if (P.Literal && P.Value) {
      if (To->TheKind == CType::Kind::Bool)
        return Printed{*P.Value ? "true" : "false", PrecAtom};
      if (To->isInteger()) {
        Printed R = P;
        const int64_t V = truncated(*P.Value, To);
        R.Value = V;
        R.Text = literalText(V, To, P.Text);
        R.Prec = V < 0 ? PrecUnary : PrecAtom;
        return R;
      }
      if (To->isFloating())
        return P;
    }
    if (sameType(From, To))
      return P;
    if (To->TheKind == CType::Kind::Bool) {
      Printed R{paren(P, PrecCompare + 1) +
                    (From->isPointer() ? " != nil" : " != 0"),
                PrecCompare};
      return R;
    }
    return Printed{conversion(To, P), PrecAtom};
  }

  const CType *valueType(const Expr *E) const {
    const CType *Ty = E->Ty;
    if (Ty->isArray())
      return const_cast<TypeContext &>(T.Types).pointerTo(Ty->Inner);
    if (Ty->isFunction())
      return const_cast<TypeContext &>(T.Types).pointerTo(Ty);
    return Ty;
  }

  Printed value(const Expr *E, const CType *Want) {
    // A character a byte holds is a constant Go assigns to it.
    if (E->Kind == ExprKind::Character && E->Text.starts_with("'") && Want &&
        Want->isInteger() && Want->Bits == 8 && !Want->Signed)
      return withComments(E, Printed{E->Text.str(), PrecAtom});
    if (const Expr *Narrow = narrowable(E, Want))
      return withComments(E, narrowed(Narrow, Want));
    return convert(raw(E), valueType(E), Want);
  }

  Printed narrowed(const Expr *E, const CType *Want) {
    Printed P;
    switch (E->Kind) {
    case ExprKind::Cast:
      P = value(E->Ops.front(), Want);
      if (P.Literal)
        P = Printed{conversion(Want, P), PrecAtom};
      break;
    case ExprKind::Unary: {
      if (E->Value) {
        P = convert(raw(E), E->Ty, Want);
        break;
      }
      Printed Op = value(E->Ops.front(), Want);
      P = Printed{(E->Text == "~" ? "^" : "-") + paren(Op, PrecUnary),
                  PrecUnary};
      break;
    }
    default:
      P = binaryText(value(E->Ops[0], Want), E->Text, value(E->Ops[1], Want),
                     binaryPrecedence(E->Text));
      break;
    }
    return withComments(E, P);
  }

  Printed condition(const Expr *E) {
    Printed P = raw(E);
    if (P.Bool || P.Diverges)
      return P;
    const CType *Ty = valueType(E);
    if (Ty->TheKind == CType::Kind::Bool || Ty->isUnknown())
      return P;
    Printed R{paren(P, PrecCompare + 1) +
                  (Ty->isPointer() ? " != nil" : " != 0"),
              PrecCompare};
    R.Bool = true;
    return R;
  }

  static bool needsClarity(const Printed &Operand, llvm::StringRef Op) {
    static const llvm::StringSet<> Bitwise = {"<<", ">>", "&", "|", "^"};
    return !Operand.Operator.empty() && Operand.Operator != Op &&
           (Bitwise.contains(Operand.Operator) || Bitwise.contains(Op));
  }

  /// Go's five levels: `* / % << >> &` bind as multiplication and `| ^` as
  /// addition.
  int binaryPrecedence(llvm::StringRef Op) const {
    if (Op == "*" || Op == "/" || Op == "%" || Op == "<<" || Op == ">>" ||
        Op == "&")
      return PrecMul;
    if (Op == "+" || Op == "-" || Op == "|" || Op == "^")
      return PrecAdd;
    if (Op == "&&")
      return PrecAnd;
    if (Op == "||")
      return PrecOr;
    return PrecCompare;
  }

  Printed binaryText(Printed L, llvm::StringRef Op, Printed R, int Prec,
                     bool Bool = false) {
    std::string LT = needsClarity(L, Op) ? "(" + L.Text + ")" : paren(L, Prec);
    if (Prec == PrecCompare && L.Prec == PrecCompare)
      LT = "(" + L.Text + ")";
    std::string RT =
        needsClarity(R, Op) ? "(" + R.Text + ")" : paren(R, Prec + 1);
    Printed P{LT + " " + Op.str() + " " + RT, Prec};
    P.Bool = Bool;
    P.Operator = Op;
    return P;
  }

  Printed pointerOffset(Printed Ptr, const Expr *Offset, bool Subtract,
                        const CType *PtrTy, const Expr *Base = nullptr) {
    const CType *Pointee = PtrTy->Inner;
    Printed N = raw(Offset);
    // An element of an array: `&stack_storage[40]`.
    if (Base && !Subtract && N.Literal && N.Value) {
      const Expr *Array = skipParens(Base);
      if (Array->Kind == ExprKind::Name && Array->Ty->isArray() &&
          Array->Ty->Count && *N.Value >= 0 &&
          static_cast<uint64_t>(*N.Value) < *Array->Ty->Count)
        return Printed{"&" + place(Array).Text + "[" +
                           std::to_string(*N.Value) + "]",
                       PrecUnary};
    }
    std::string Amount;
    if (N.Literal && N.Value) {
      int64_t V = *N.Value;
      if (!Pointee->isVoid()) {
        auto Size = T.Types.sizeOf(Pointee);
        if (!Size) {
          unsupported("arithmetic on a pointer to an unsized type");
          return Ptr;
        }
        V *= static_cast<int64_t>(*Size);
      }
      Amount = std::to_string(Subtract ? -V : V);
    } else {
      Amount = paren(N, PrecMul + 1);
      if (!Pointee->isVoid()) {
        auto Size = T.Types.sizeOf(Pointee);
        if (!Size) {
          unsupported("arithmetic on a pointer to an unsized type");
          return Ptr;
        }
        if (*Size != 1)
          Amount += "*" + std::to_string(*Size);
      }
      if (Subtract)
        Amount = "-(" + Amount + ")";
    }
    Printed Moved{"unsafe.Add(" + Ptr.Text + ", " + Amount + ")", PrecAtom};
    if (Pointee->isVoid())
      return Moved;
    return Printed{conversion(PtrTy, Moved), PrecAtom};
  }

  Printed raw(const Expr *E) { return withComments(E, rawInner(E)); }

  Printed rawInner(const Expr *E) {
    switch (E->Kind) {
    case ExprKind::Name: {
      const bool File = E->Ref && FileScope.contains(E->Ref);
      std::string N = File ? name(E->Text) : escapeIdentifier(E->Text);
      if (E->Ty->isArray())
        return Printed{"&" + N + "[0]", PrecUnary};
      return Printed{N, PrecAtom};
    }
    case ExprKind::Integer: {
      Printed P{E->Text.rtrim("uUlL").str(), PrecAtom};
      P.Literal = true;
      P.Value = E->Value;
      return P;
    }
    case ExprKind::Floating:
      return Printed{E->Text.rtrim("fFlL").str(), PrecAtom};
    case ExprKind::Character:
      return Printed{E->Text.str(), PrecAtom};
    case ExprKind::String:
      return Printed{stringLiteral(E), PrecAtom};
    case ExprKind::Paren:
      return raw(E->Ops.front());
    case ExprKind::Unary:
      return unary(E);
    case ExprKind::Binary:
      return binary(E);
    case ExprKind::Assign:
      unsupported("an assignment inside an expression");
      return Printed{"_", PrecAtom};
    case ExprKind::Conditional:
      return conditional(E);
    case ExprKind::Comma:
      return comma(E);
    case ExprKind::Cast:
      return cast(E);
    case ExprKind::Call:
      return call(E);
    case ExprKind::Member:
      if (auto Bits = bitField(E))
        return Printed{Bits->Holder + "." + Bits->Getter + "()", PrecAtom};
      return place(E);
    case ExprKind::Subscript:
      return place(E);
    case ExprKind::SizeofExpr:
    case ExprKind::SizeofType:
      if (E->Value) {
        Printed P{std::to_string(*E->Value), PrecAtom};
        P.Literal = true;
        P.Value = E->Value;
        return P;
      }
      if (E->Kind == ExprKind::SizeofType)
        return Printed{type(E->Ty) + "(unsafe.Sizeof(*new(" + type(E->Written) +
                           ")))",
                       PrecAtom};
      return Printed{type(E->Ty) + "(unsafe.Sizeof(" +
                         raw(E->Ops.front()).Text + "))",
                     PrecAtom};
    case ExprKind::AlignofType:
      return Printed{type(E->Ty) + "(unsafe.Alignof(*new(" + type(E->Written) +
                         ")))",
                     PrecAtom};
    case ExprKind::Offsetof:
      if (E->ThroughAnonymous) {
        unsupported("a member of an anonymous structure or union");
        return Printed{"_", PrecAtom};
      }
      return Printed{type(E->Ty) + "(unsafe.Offsetof(" + type(E->Written) +
                         "{}." + E->Text.str() + "))",
                     PrecAtom};
    case ExprKind::CompoundLiteral:
      unsupported("a compound literal");
      return Printed{"_", PrecAtom};
    case ExprKind::InitList:
    case ExprKind::Designated:
      unsupported("an initializer list inside an expression");
      return Printed{"_", PrecAtom};
    case ExprKind::StatementExpr:
      return statementExpr(E);
    case ExprKind::BitCast:
      return bitCast(E);
    case ExprKind::VaArg:
      unsupported("va_arg");
      return Printed{"_", PrecAtom};
    case ExprKind::Asm:
      return Printed{E->Text.str(), PrecAtom};
    }
    unsupported("an unknown expression");
    return Printed{"_", PrecAtom};
  }

  std::string stringLiteral(const Expr *E) const {
    std::string Body;
    bool Wide = false;
    auto Append = [&](llvm::StringRef Text) {
      Wide |= Text.starts_with("L");
      Text = Text.drop_until([](char C) { return C == '"'; });
      Text = Text.drop_front().drop_back();
      for (size_t I = 0; I < Text.size(); ++I) {
        if (Text[I] == '\\' && I + 1 < Text.size() && Text[I + 1] == '?') {
          Body += '?';
          ++I;
          continue;
        }
        if (Text[I] == '\\' && I + 1 < Text.size() && Text[I + 1] >= '0' &&
            Text[I + 1] <= '7') {
          unsigned V = 0, Digits = 0;
          while (Digits < 3 && I + 1 < Text.size() && Text[I + 1] >= '0' &&
                 Text[I + 1] <= '7') {
            V = V * 8 + (Text[I + 1] - '0');
            ++I;
            ++Digits;
          }
          Body += "\\x" + llvm::utohexstr(V, /*LowerCase=*/false, 2);
          continue;
        }
        Body += Text[I];
        if (Text[I] == '\\' && I + 1 < Text.size())
          Body += Text[++I];
      }
    };
    if (E->Ops.empty())
      Append(E->Text);
    for (const Expr *Part : E->Ops)
      Append(Part->Text);
    return (Wide ? "L\"" : "\"") + Body + "\"";
  }

  Printed unary(const Expr *E) {
    const Expr *Op = E->Ops.front();
    const llvm::StringRef O = E->Text;
    if (O == "&") {
      if (Op->Ty->isFunction())
        return raw(Op);
      // `&(T){0}`: a new zero T; `&(T){v}`: a new T holding v, the one
      // element of a slice made of it, which Go can take the address of.
      const Expr *Literal = skipParens(Op);
      if (Literal->Kind == ExprKind::CompoundLiteral &&
          Literal->Written->isScalar()) {
        if (isZeroInitializer(Literal->Ops[0]))
          return Printed{"new(" + type(Literal->Written) + ")", PrecAtom};
        const Expr *Init = Literal->Ops[0];
        if (Init->Kind == ExprKind::InitList && Init->Ops.size() == 1)
          Init = Init->Ops[0];
        if (Init->Kind != ExprKind::InitList &&
            Init->Kind != ExprKind::Designated)
          return Printed{"&[]" + type(Literal->Written) + "{" +
                             value(Init, Literal->Written).Text + "}[0]",
                         PrecUnary};
      }
      return Printed{"&" + paren(place(Op), PrecAtom), PrecUnary};
    }
    if (O == "*") {
      refuseVolatile(Op);
      return Printed{"*" + paren(value(Op, nullptr), PrecUnary), PrecUnary};
    }
    if (O == "++" || O == "--") {
      unsupported("an increment inside an expression");
      return Printed{"_", PrecAtom};
    }
    if (O == "!") {
      const CType *Ty = valueType(Op);
      Printed P = raw(Op);
      Printed R;
      if (P.Bool || Ty->TheKind == CType::Kind::Bool)
        R = Printed{"!" + paren(P, PrecUnary), PrecUnary};
      else
        R = Printed{paren(P, PrecCompare + 1) +
                        (Ty->isPointer() ? " == nil" : " == 0"),
                    PrecCompare};
      R.Bool = true;
      return R;
    }
    if (E->Value && E->OpTy && E->OpTy->isInteger()) {
      Printed P{literalText(*E->Value, E->OpTy, Op->Text),
                *E->Value < 0 ? PrecUnary : PrecAtom};
      P.Literal = true;
      P.Value = E->Value;
      return P;
    }
    Printed P = value(Op, E->OpTy);
    if (O == "+")
      return P;
    return Printed{(O == "~" ? "^" : "-") + paren(P, PrecUnary), PrecUnary};
  }

  static bool isZeroInitializer(const Expr *Init) {
    if (Init->Kind == ExprKind::InitList)
      return Init->Ops.size() == 1 && isZeroInitializer(Init->Ops.front());
    return Init->Value && *Init->Value == 0;
  }

  /// A volatile access has no plain dereference in the dialect.
  void refuseVolatile(const Expr *Pointer) const {
    const CType *Ty = Pointer->Ty;
    if (Ty && Ty->isPointer() && Ty->Inner->Volatile)
      unsupported("a volatile memory access");
  }

  bool isNull(const Expr *E) const {
    while (E->Kind == ExprKind::Paren || E->Kind == ExprKind::Cast)
      E = E->Ops.front();
    return E->Value && *E->Value == 0;
  }

  Printed binary(const Expr *E) {
    const Expr *L = E->Ops[0], *R = E->Ops[1];
    const llvm::StringRef O = E->Text;
    if (O == "&&" || O == "||")
      return binaryText(condition(L), O, condition(R), binaryPrecedence(O),
                        /*Bool=*/true);
    const bool Compare = O == "==" || O == "!=" || O == "<" || O == ">" ||
                         O == "<=" || O == ">=";
    if (!E->OpTy || E->OpTy->isUnknown())
      return binaryText(raw(L), O, raw(R), binaryPrecedence(O), Compare);
    if (Compare && E->OpTy->isPointer() && (O == "==" || O == "!=") &&
        (isNull(L) || isNull(R))) {
      const Expr *Ptr = isNull(L) ? R : L;
      Printed P = binaryText(value(Ptr, E->OpTy), O, Printed{"nil", PrecAtom},
                             PrecCompare, true);
      return P;
    }
    if (E->OpTy->isPointer() && !Compare) {
      const CType *LT = valueType(L);
      if (LT->isPointer() && valueType(R)->isPointer()) {
        auto Size = T.Types.sizeOf(LT->Inner);
        Printed Diff{"(uintptr(" + value(L, nullptr).Text + ") - uintptr(" +
                         value(R, nullptr).Text + "))",
                     PrecAtom};
        if (Size && *Size != 1)
          Diff.Text += " / " + std::to_string(*Size);
        return Printed{type(E->Ty) + "(" + Diff.Text + ")", PrecAtom};
      }
      const bool PointerLeft = LT->isPointer();
      return pointerOffset(value(PointerLeft ? L : R, E->OpTy),
                           PointerLeft ? R : L, O == "-", E->OpTy,
                           PointerLeft ? L : R);
    }
    const bool Shift = O == "<<" || O == ">>";
    Printed LP = value(L, E->OpTy);
    Printed RP = Shift ? value(R, nullptr) : value(R, E->OpTy);
    if (LP.Literal && (RP.Literal || Shift))
      LP = Printed{conversion(E->OpTy, LP), PrecAtom};
    return binaryText(LP, O, RP, binaryPrecedence(O), Compare);
  }

  /// `func() T { ... }()`: Go's way to compute statements in an expression.
  Printed closure(const CType *Ty, llvm::StringRef Body) {
    const std::string Result = Ty && !Ty->isVoid() ? " " + type(Ty) : "";
    return Printed{"func()" + Result + " { " + Body.str() + " }()", PrecAtom};
  }

  Printed conditional(const Expr *E) {
    const Expr *C = E->Ops[0], *A = E->Ops[1], *B = E->Ops[2];
    const CType *Ty = E->OpTy;
    if (Ty && Ty->isInteger() && A->Value && B->Value && *A->Value == 1 &&
        *B->Value == 0)
      return Printed{conversion(Ty, condition(C)), PrecAtom};
    return closure(Ty, "if " + condition(C).Text + " { return " +
                           value(A, Ty).Text + " }; return " +
                           value(B, Ty).Text);
  }

  Printed comma(const Expr *E) {
    if (isUnknownValue(E)) {
      Printed P{"trap()", PrecAtom};
      P.Diverges = true;
      return withComments(E->Ops[1], P);
    }
    if (const Expr *Source = unalignedLoadSource(E)) {
      const Expr *Var = E->Ops[1];
      Printed Load{"*(*" + type(valueType(Var)) + ")(" +
                       stripVoidCast(Source).Text + ")",
                   PrecUnary};
      if (isLoadScratch(Var))
        return Load;
      return closure(valueType(Var), raw(Var).Text + " = " + Load.Text +
                                         "; return " + raw(Var).Text);
    }
    std::string Body;
    for (size_t I = 0; I + 1 < E->Ops.size(); ++I)
      Body += discarded(E->Ops[I]) + "; ";
    Body += "return " + value(E->Ops.back(), nullptr).Text;
    return closure(valueType(E->Ops.back()), Body);
  }

  /// An expression evaluated for its effects, as a Go statement.
  std::string discarded(const Expr *E) {
    const Expr *Inner = skipParens(E);
    if (Inner->Kind == ExprKind::Call)
      return value(Inner, nullptr).Text;
    if (Inner->Kind == ExprKind::Cast && Inner->Written->isVoid())
      return discarded(Inner->Ops.front());
    return "_ = " + value(E, nullptr).Text;
  }

  Printed stripVoidCast(const Expr *E) {
    while (E->Kind == ExprKind::Paren ||
           (E->Kind == ExprKind::Cast && E->Written->isPointer() &&
            E->Written->Inner->isVoid()))
      E = E->Ops.front();
    return value(E, nullptr);
  }

  Printed cast(const Expr *E) {
    const Expr *Op = E->Ops.front();
    const CType *To = E->Written;
    if (To->isVoid())
      return Printed{"_ = " + value(Op, nullptr).Text, PrecLowest};
    if (narrowable(E, To))
      return narrowed(skipParens(E), To);
    Printed P = raw(Op);
    const CType *From = valueType(Op);
    if (P.Literal && P.Value && To->isInteger() &&
        To->TheKind != CType::Kind::Bool) {
      const int64_t V = truncated(*P.Value, To);
      Printed Literal{literalText(V, To, P.Text), PrecAtom};
      return Printed{conversion(To, Literal), PrecAtom};
    }
    if (To->isPointer() && To->Inner->isFunction() && From->isInteger())
      return Printed{"(" + functionType(To->Inner, {}) + ")(" + P.Text + ")",
                     PrecAtom};
    if (To->isPointer())
      if (const Expr *Address = addressOperand(Op))
        return convert(raw(Address), valueType(Address), To);
    return convert(P, From, To);
  }

  /// The integer an address cast reads through same-width integer casts.
  const Expr *addressOperand(const Expr *E) const {
    const Expr *Inner = skipParens(E);
    const Expr *Found = nullptr;
    while (Inner->Kind == ExprKind::Cast &&
           Inner->Written->TheKind == CType::Kind::Integer &&
           Inner->Written->Bits == T.Types.model().PointerBits) {
      const Expr *Op = skipParens(Inner->Ops.front());
      if (!Op->Ty || Op->Ty->TheKind != CType::Kind::Integer ||
          Op->Ty->Bits != Inner->Written->Bits)
        break;
      Found = Op;
      Inner = Op;
    }
    return Found;
  }

  const CType *promotedArgument(const CType *Ty) const {
    auto &Types = const_cast<TypeContext &>(T.Types);
    if (Ty->isFloating() && Ty->Bits < 64)
      return Types.doubleType();
    if (Ty->isInteger())
      return Types.promoted(Ty);
    return Ty;
  }

  const CType *resultType(llvm::StringRef Name) const {
    auto &Types = const_cast<TypeContext &>(T.Types);
    if (Name == "_Bool")
      return Types.boolType();
    if (Name == "intptr_t")
      return Types.integer(Types.model().PointerBits, true, Name);
    if (Name == "uint32_t")
      return Types.integer(32, false, Name);
    return Types.unknownType();
  }

  std::string substitute(llvm::StringRef Template,
                         llvm::ArrayRef<Printed> Args) {
    std::string Result;
    for (size_t I = 0; I < Template.size(); ++I) {
      if (Template[I] != '$' || I + 1 >= Template.size() ||
          !llvm::isDigit(Template[I + 1])) {
        Result += Template[I];
        continue;
      }
      const unsigned Index = Template[I + 1] - '0';
      ++I;
      if (Index >= Args.size()) {
        unsupported("a builtin with too few arguments");
        continue;
      }
      Result += Args[Index].Text;
    }
    return Result;
  }

  Printed call(const Expr *E) {
    const Expr *Callee = skipParens(E->Ops.front());
    std::vector<const CType *> ArgTypes;
    for (size_t I = 1; I < E->Ops.size(); ++I)
      ArgTypes.push_back(promotedArgument(valueType(E->Ops[I])));
    const CType *Fn = nullptr;
    if (Callee->Ty && !Callee->Ty->isUnknown()) {
      const CType *CalleeTy = valueType(Callee);
      if (CalleeTy->isPointer() && CalleeTy->Inner->isFunction())
        Fn = CalleeTy->Inner;
    }
    auto Argument = [&](size_t I) {
      const Expr *Arg = E->Ops[I + 1];
      const CType *To = Fn && Fn->Prototyped && I < Fn->Params.size()
                            ? Fn->Params[I]
                            : promotedArgument(valueType(Arg));
      return value(Arg, To);
    };
    if (Callee->Kind == ExprKind::Name) {
      if (auto Spelling = goBuiltin(Callee->Text)) {
        std::vector<Printed> Args;
        for (size_t I = 0; I + 1 < E->Ops.size(); ++I)
          Args.push_back(Argument(I));
        Printed P{substitute(Spelling->Template, Args), PrecAtom};
        if (Callee->Text == "__builtin_trap" ||
            Callee->Text == "__builtin_unreachable")
          P.Diverges = true;
        if (!Spelling->Result.empty() && Fn)
          return convert(P, resultType(Spelling->Result), Fn->Inner);
        return P;
      }
    }
    std::string Text;
    if (Callee->Kind == ExprKind::Name &&
        (!Callee->Ref || Callee->Ref->Kind == DeclKind::Function)) {
      const bool File = Callee->Ref && FileScope.contains(Callee->Ref);
      Text = File ? name(Callee->Text) : escapeIdentifier(Callee->Text);
    } else if (Callee->Kind == ExprKind::Cast && Fn && !Fn->Prototyped) {
      const Expr *Target = Callee->Ops.front();
      if (const Expr *Address = addressOperand(Target))
        Target = Address;
      Text = "(" + functionType(Fn, ArgTypes, /*Called=*/true) + ")(" +
             value(Target, nullptr).Text + ")";
    } else {
      Text = paren(value(Callee, nullptr), PrecAtom);
    }
    Text += "(";
    for (size_t I = 0; I + 1 < E->Ops.size(); ++I)
      Text += (I ? ", " : "") + Argument(I).Text;
    Text += ")";
    return Printed{Text, PrecAtom};
  }

  /// What \p E, a member of one of the program's types, reaches through
  /// \p Hops: fields, and methods that return pointers.
  std::string localPath(const Expr *E,
                        llvm::ArrayRef<SourceDialectLocalTypes::Hop> Hops) {
    std::string Text = paren(E->Postfix ? value(E->Ops.front(), nullptr)
                                        : place(E->Ops.front()),
                             PrecAtom);
    for (const SourceDialectLocalTypes::Hop &Hop : Hops)
      Text += "." + Hop.Name + (Hop.Accessor ? "()" : "");
    return Text;
  }

  /// A bit-field of one of the program's types, as Go reads and writes it:
  /// the record whose methods do, and their names.
  struct BitField {
    std::string Holder, Getter, Setter;
  };
  std::optional<BitField> bitField(const Expr *E) {
    if (E->Kind != ExprKind::Member || !E->Field || !E->Field->BitWidth ||
        !valueType(E)->isInteger() ||
        valueType(E)->TheKind == CType::Kind::Bool)
      return std::nullopt;
    std::optional<SourceDialectLocalTypes::Path> Path = localMember(E);
    if (!Path || !Path->bitField())
      return std::nullopt;
    return BitField{localPath(E, Path->Hops), Path->Getter, Path->Setter};
  }

  Printed place(const Expr *E) {
    switch (E->Kind) {
    case ExprKind::Paren:
      return place(E->Ops.front());
    case ExprKind::Name: {
      const bool File = E->Ref && FileScope.contains(E->Ref);
      return Printed{File ? name(E->Text) : escapeIdentifier(E->Text),
                     PrecAtom};
    }
    case ExprKind::Unary:
      if (E->Text == "*") {
        refuseVolatile(E->Ops.front());
        return Printed{"*" + paren(value(E->Ops.front(), nullptr), PrecUnary),
                       PrecUnary};
      }
      break;
    case ExprKind::Subscript: {
      const Expr *Base = E->Ops[0], *Index = E->Ops[1];
      if (!valueType(Base)->isPointer())
        std::swap(Base, Index);
      if (Base->Ty->isArray())
        return Printed{paren(place(Base), PrecAtom) + "[" +
                           value(Index, nullptr).Text + "]",
                       PrecAtom};
      const CType *PtrTy = valueType(Base);
      Printed Moved = pointerOffset(value(Base, nullptr), Index, false, PtrTy);
      return Printed{"*" + paren(Moved, PrecUnary), PrecUnary};
    }
    case ExprKind::Member: {
      // A member of one of the program's types, by the fields and methods
      // that reach it; a method returns a pointer to the member.
      if (auto Path = localMember(E)) {
        // Only the methods of a bit-field's record read and write it.
        if (Path->bitField())
          unsupported("a bit-field where Go needs a place");
        if (Path->Hops.empty() || refused())
          return Printed{"_", PrecAtom};
        const std::string Text = localPath(E, Path->Hops);
        if (Path->Hops.back().Accessor)
          return Printed{"*" + Text, PrecUnary};
        return Printed{Text, PrecAtom};
      }
      if (E->Field && E->Field->BitWidth) {
        unsupported("a bit-field");
        return Printed{"_", PrecAtom};
      }
      if (E->ThroughAnonymous) {
        unsupported("a member of an anonymous structure or union");
        return Printed{"_", PrecAtom};
      }
      return Printed{paren(E->Postfix ? value(E->Ops.front(), nullptr)
                                      : place(E->Ops.front()),
                           PrecAtom) +
                         "." + E->Text.str(),
                     PrecAtom};
    }
    case ExprKind::String:
      return Printed{stringLiteral(E), PrecAtom};
    default:
      break;
    }
    unsupported("an assignment to a value Go cannot name");
    return Printed{"_", PrecAtom};
  }

  Printed statementExpr(const Expr *E) {
    const Stmt *Body = E->Body;
    const Stmt *Last = nullptr;
    for (const Stmt *S : Body->Body)
      if (S->Kind != StmtKind::Comment)
        Last = S;
    const CType *Ty = E->Ty;
    const std::string Inner = capture([&] {
      for (const Stmt *S : Body->Body) {
        if (S == Last && S->Kind == StmtKind::Expression && !Ty->isVoid()) {
          line("return " + value(S->Value, nullptr).Text);
          continue;
        }
        stmt(S);
      }
    });
    const std::string Result = !Ty->isVoid() ? " " + type(Ty) : "";
    return Printed{"func()" + Result + " {\n" + Inner + indentation(depth()) +
                       "}()",
                   PrecAtom};
  }

  Printed bitCast(const Expr *E) {
    const Expr *Op = E->Ops.front();
    const CType *From = valueType(Op);
    const CType *To = E->Written;
    Printed P = value(Op, nullptr);
    const bool SameSize = From->Bits && From->Bits == To->Bits;
    if (SameSize && (From->isInteger() || From->isPointer()) &&
        (To->isInteger() || To->isPointer()))
      return Printed{conversion(To, P), PrecAtom};
    if (From->isFloating() && To->isInteger() &&
        (From->Bits == 32 || From->Bits == 64)) {
      Printed Bits{"math.Float" + std::to_string(From->Bits) + "bits(" +
                       P.Text + ")",
                   PrecAtom};
      return convert(
          Bits,
          const_cast<TypeContext &>(T.Types).integer(From->Bits, false, "bits"),
          To);
    }
    if (From->isInteger() && To->isFloating() &&
        (To->Bits == 32 || To->Bits == 64)) {
      const CType *Bits =
          const_cast<TypeContext &>(T.Types).integer(To->Bits, false, "bits");
      return Printed{"math.Float" + std::to_string(To->Bits) + "frombits(" +
                         convert(P, From, Bits).Text + ")",
                     PrecAtom};
    }
    return Printed{"*(*" + type(To) + ")(unsafe.Pointer(&" + P.Text + "))",
                   PrecUnary};
  }

  //===--- Statements ---===//

  void openBlock(llvm::StringRef Head) {
    write(Head);
    write(Head.empty() ? "{" : " {");
    endLine();
    indent();
  }
  void closeBlock(llvm::StringRef Tail = "}") {
    dedent();
    line(Tail);
  }

  void body(const Stmt *S) {
    if (S->Kind != StmtKind::Compound) {
      stmt(S);
      return;
    }
    leadingComments(S->Leading);
    for (const Stmt *Child : S->Body)
      stmt(Child);
  }

  void stmt(const Stmt *S) {
    leadingComments(S->Leading);
    const size_t Begin = mark();
    stmtInner(S);
    trailingComments(S->Trailing);
    piece(S->Begin, S->End, Begin);
  }

  void stmtInner(const Stmt *S) {
    switch (S->Kind) {
    case StmtKind::Compound:
      openBlock("");
      body(S);
      closeBlock();
      return;
    case StmtKind::Declaration:
      for (const Decl *D : S->Decls)
        local(D);
      return;
    case StmtKind::Expression:
      expressionStatement(S->Value);
      return;
    case StmtKind::If: {
      openBlock("if " + condition(S->Value).Text);
      body(S->Then);
      const Stmt *Else = S->Else;
      while (Else) {
        dedent();
        if (Else->Kind == StmtKind::If && Else->Leading.empty()) {
          openBlock("} else if " + condition(Else->Value).Text);
          body(Else->Then);
          Else = Else->Else;
          continue;
        }
        openBlock("} else");
        body(Else);
        break;
      }
      closeBlock();
      return;
    }
    case StmtKind::While: {
      const bool Forever = S->Value->Value && *S->Value->Value != 0;
      openBlock(Forever ? "for" : "for " + condition(S->Value).Text);
      body(S->Then);
      closeBlock();
      return;
    }
    case StmtKind::DoWhile: {
      const bool Once = S->Value->Value && *S->Value->Value == 0;
      if (Once && !continuesLoop(S->Then) && !breaksOut(S->Then)) {
        openBlock("");
        body(S->Then);
        closeBlock();
        return;
      }
      // `continue` runs the post statement, which tests the condition.
      openBlock("for again := true; again; again = " +
                condition(S->Value).Text);
      body(S->Then);
      closeBlock();
      return;
    }
    case StmtKind::For: {
      std::string Head = "for ";
      if (S->Value)
        Head += simpleStatement(S->Value);
      Head += "; ";
      if (S->Else)
        Head += condition(S->Else->Value).Text;
      Head += "; ";
      if (S->Step)
        Head += simpleStatement(S->Step);
      if (!S->Value && !S->Step)
        Head = S->Else ? "for " + condition(S->Else->Value).Text : "for";
      openBlock(Head);
      body(S->Then);
      closeBlock();
      return;
    }
    case StmtKind::Switch:
      switchStmt(S);
      return;
    case StmtKind::Case:
    case StmtKind::Default:
      unsupported("a case label outside its switch");
      return;
    case StmtKind::Break:
      line("break");
      return;
    case StmtKind::Continue:
      line("continue");
      return;
    case StmtKind::Return:
      returnStatement(S->Value);
      return;
    case StmtKind::Goto:
      line("goto " + S->Text.str());
      return;
    case StmtKind::Label: {
      const unsigned Saved = depth();
      for (unsigned I = 0; I < Saved; ++I)
        dedent();
      line(S->Text.str() + ":");
      for (unsigned I = 0; I < Saved; ++I)
        indent();
      return;
    }
    case StmtKind::Empty:
      return;
    case StmtKind::Comment:
      line(commentText(S->TheComment));
      return;
    case StmtKind::Try:
      tryStmt(S);
      return;
    case StmtKind::Throw:
      if (!S->Value) {
        line("throw");
        return;
      }
      line("throw " + T.Source.slice(S->Value->Begin, S->Value->End).str());
      return;
    case StmtKind::Leave:
      line("__leave");
      return;
    case StmtKind::Asm: {
      llvm::SmallVector<llvm::StringRef, 8> Lines;
      T.Source.slice(S->Begin, S->End).split(Lines, '\n');
      for (size_t I = 0; I < Lines.size(); ++I)
        line(I ? Lines[I].ltrim().str() : Lines[I].str());
      return;
    }
    }
  }

  /// An assignment or call as Go's for-statement header takes it.
  std::string simpleStatement(const Expr *E) {
    const std::string Text = capture([&] { expressionStatement(E); });
    llvm::StringRef Trimmed = llvm::StringRef(Text).trim();
    if (Trimmed.contains('\n')) {
      unsupported("a for header Go cannot write as one statement");
      return "_";
    }
    return Trimmed.str();
  }

  void returnStatement(const Expr *E) {
    if (!E) {
      line("return");
      return;
    }
    const Expr *Inner = skipParens(E);
    if (Inner->Kind == ExprKind::Conditional &&
        !(Inner->Ops[1]->Value && Inner->Ops[2]->Value)) {
      openBlock("if " + condition(Inner->Ops[0]).Text);
      returnStatement(Inner->Ops[1]);
      closeBlock();
      returnStatement(Inner->Ops[2]);
      return;
    }
    if (hoists(Inner)) {
      hoisted(Inner, [&](const Expr *Last) { returnStatement(Last); });
      return;
    }
    line("return " + value(E, ReturnType).Text);
  }

  /// Whether an expression at the top of a statement becomes statements:
  /// a comma (other than a load or an unknown value) or a GNU statement
  /// expression.
  bool hoists(const Expr *E) const {
    E = skipParens(E);
    if (E->Kind == ExprKind::StatementExpr)
      return true;
    return E->Kind == ExprKind::Comma && !isUnknownValue(E) &&
           !(unalignedLoadSource(E) && isLoadScratch(E->Ops[1]));
  }

  /// Prints the statements of \p E, then \p Rest with its value.
  void hoisted(const Expr *E, llvm::function_ref<void(const Expr *)> Rest) {
    E = skipParens(E);
    if (E->Kind == ExprKind::Comma) {
      for (size_t I = 0; I + 1 < E->Ops.size(); ++I)
        line(discarded(E->Ops[I]));
      Rest(E->Ops.back());
      return;
    }
    // A statement expression keeps its declarations in a block.
    const Stmt *Body = E->Body;
    const Stmt *Last = nullptr;
    for (const Stmt *S : Body->Body)
      if (S->Kind != StmtKind::Comment)
        Last = S;
    openBlock("");
    for (const Stmt *S : Body->Body) {
      if (S == Last && S->Kind == StmtKind::Expression) {
        Rest(S->Value);
        continue;
      }
      stmt(S);
    }
    closeBlock();
  }

  void expressionStatement(const Expr *E) {
    while (E->Kind == ExprKind::Paren && E->Leading.empty() &&
           E->Trailing.empty())
      E = E->Ops.front();
    switch (E->Kind) {
    case ExprKind::Assign:
      assignment(E);
      return;
    case ExprKind::Unary:
      if (E->Text == "++" || E->Text == "--") {
        if (auto Bits = bitField(E->Ops.front())) {
          line(Bits->Holder + "." + Bits->Setter + "(" + Bits->Holder + "." +
               Bits->Getter + "()" + (E->Text == "++" ? " + 1" : " - 1") + ")");
          return;
        }
        line(place(E->Ops.front()).Text + E->Text.str());
        return;
      }
      break;
    case ExprKind::Cast:
      if (E->Written->isVoid()) {
        line(discarded(E->Ops.front()));
        return;
      }
      break;
    case ExprKind::Comma:
      if (!isUnknownValue(E)) {
        for (const Expr *Op : E->Ops)
          expressionStatement(Op);
        return;
      }
      break;
    case ExprKind::Conditional: {
      openBlock("if " + condition(E->Ops[0]).Text);
      expressionStatement(E->Ops[1]);
      dedent();
      openBlock("} else");
      expressionStatement(E->Ops[2]);
      closeBlock();
      return;
    }
    case ExprKind::StatementExpr:
      hoisted(E, [&](const Expr *Last) { expressionStatement(Last); });
      return;
    case ExprKind::Call:
      line(value(E, nullptr).Text);
      return;
    default:
      break;
    }
    line(discarded(E));
  }

  void assignment(const Expr *E) {
    const Expr *L = E->Ops[0], *R = E->Ops[1];
    const CType *Target = valueType(L);
    // A bit-field takes what its setter stores, computed from its getter.
    const std::optional<BitField> Bits = bitField(L);
    const std::string Place = Bits ? std::string() : place(L).Text;
    const auto store = [&](const std::string &Value) {
      line(Bits ? Bits->Holder + "." + Bits->Setter + "(" + Value + ")"
                : Place + " = " + Value);
    };
    if (E->Text == "=") {
      const Expr *Inner = skipParens(R);
      if (Inner->Kind == ExprKind::Conditional &&
          !(Inner->Ops[1]->Value && Inner->Ops[2]->Value &&
            *Inner->Ops[1]->Value == 1 && *Inner->Ops[2]->Value == 0)) {
        openBlock("if " + condition(Inner->Ops[0]).Text);
        store(value(Inner->Ops[1], Target).Text);
        dedent();
        openBlock("} else");
        store(value(Inner->Ops[2], Target).Text);
        closeBlock();
        return;
      }
      if (hoists(Inner)) {
        hoisted(Inner,
                [&](const Expr *Last) { store(value(Last, Target).Text); });
        return;
      }
      store(value(R, Target).Text);
      return;
    }
    const llvm::StringRef Op = E->Text.drop_back();
    if (Bits) {
      Printed Current =
          convert(Printed{Bits->Holder + "." + Bits->Getter + "()", PrecAtom},
                  Target, E->OpTy);
      store(convert(binaryText(Current, Op, value(R, E->OpTy),
                               binaryPrecedence(Op)),
                    E->OpTy, Target)
                .Text);
      return;
    }
    if (Target->isPointer()) {
      line(Place + " = " +
           pointerOffset(Printed{Place, PrecAtom}, R, Op == "-", Target).Text);
      return;
    }
    const bool Shift = Op == "<<" || Op == ">>";
    if (sameType(E->OpTy, Target)) {
      line(Place + " " + E->Text.str() + " " +
           (Shift ? value(R, nullptr) : value(R, E->OpTy)).Text);
      return;
    }
    // Low bits come out the same at the target's width.
    if (Op == "+" || Op == "-" || Op == "*" || Op == "&" || Op == "|" ||
        Op == "^") {
      line(Place + " " + E->Text.str() + " " + value(R, Target).Text);
      return;
    }
    Printed Wide = convert(Printed{Place, PrecAtom}, Target, E->OpTy);
    Printed Result =
        binaryText(Wide, Op, value(R, E->OpTy), binaryPrecedence(Op));
    line(Place + " = " + convert(Result, E->OpTy, Target).Text);
  }

  void local(const Decl *D) {
    // A record the declaration defines (a frame's layout) prints first.
    record(D->Ty);
    std::string Text = "var " + escapeIdentifier(D->Name) + " " + type(D->Ty);
    if (D->Init) {
      const Expr *Inner = skipParens(D->Init);
      if (Inner->Kind == ExprKind::Conditional &&
          !(Inner->Ops[1]->Value && Inner->Ops[2]->Value &&
            *Inner->Ops[1]->Value == 1 && *Inner->Ops[2]->Value == 0)) {
        // Go has no conditional expression: the arms assign.
        const std::string Name = escapeIdentifier(D->Name);
        line(Text);
        openBlock("if " + condition(Inner->Ops[0]).Text);
        line(Name + " = " + value(Inner->Ops[1], D->Ty).Text);
        dedent();
        openBlock("} else");
        line(Name + " = " + value(Inner->Ops[2], D->Ty).Text);
        closeBlock();
        return;
      }
      if (D->Init->Kind != ExprKind::InitList && hoists(Inner)) {
        line(Text);
        hoisted(Inner, [&](const Expr *Last) {
          line(escapeIdentifier(D->Name) + " = " + value(Last, D->Ty).Text);
        });
        return;
      }
      Text += " = " + initializer(D->Init, D->Ty);
    }
    if (D->AlignAs)
      Text += " /* aligned to " + std::to_string(*D->AlignAs) + " */";
    if (!D->AsmLabel.empty())
      Text += " /* in register " + D->AsmLabel.str() + " */";
    line(Text);
  }

  std::string initializer(const Expr *Init, const CType *Ty) {
    if (Init->Kind != ExprKind::InitList) {
      if (Ty->isArray() && Init->Kind == ExprKind::String) {
        // An array of bytes takes them as its elements, which Go zero-fills
        // past the last.
        if (byteArray(Ty))
          if (auto Bytes = stringBytes(Init);
              Bytes && Bytes->size() <= *Ty->Count) {
            while (!Bytes->empty() && !Bytes->back())
              Bytes->pop_back();
            std::vector<std::string> Elements;
            for (const unsigned char Byte : *Bytes) {
              if (!Byte)
                Elements.push_back("0");
              else if (Byte == '\'' || Byte == '\\')
                Elements.push_back({'\'', '\\', static_cast<char>(Byte), '\''});
              else if (Byte >= 32 && Byte < 127)
                Elements.push_back({'\'', static_cast<char>(Byte), '\''});
              else
                Elements.push_back("0x" + llvm::utohexstr(Byte));
            }
            return type(Ty) + "{" + llvm::join(Elements, ", ") + "}";
          }
        return stringLiteral(Init);
      }
      return value(Init, Ty).Text;
    }
    std::vector<std::string> Elements;
    for (const Expr *Item : Init->Ops) {
      if (Item->Kind == ExprKind::Designated) {
        const CType *ItemTy = Item->Ty ? Item->Ty : Ty->Inner;
        std::string Key = Item->Text.empty() ? value(Item->Ops[0], nullptr).Text
                                             : Item->Text.str();
        Elements.push_back(Key + ": " + initializer(Item->Ops.back(), ItemTy));
        continue;
      }
      const CType *ItemTy = Ty->isArray() ? Ty->Inner : Ty;
      if (Ty->isRecord()) {
        const size_t I = Elements.size();
        if (!Ty->Record->Complete || I >= Ty->Record->Fields.size()) {
          unsupported("a record initializer");
          return "_";
        }
        ItemTy = Ty->Record->Fields[I].Type;
      }
      Elements.push_back(initializer(Item, ItemTy));
    }
    if (!Ty->isArray() && !Ty->isRecord()) {
      if (Elements.size() == 1)
        return Elements.front();
      unsupported("a scalar initializer list");
      return "_";
    }
    return type(Ty) + "{" + llvm::join(Elements, ", ") + "}";
  }

  void switchStmt(const Stmt *S) {
    auto Arms = switchArms(S);
    if (!Arms) {
      unsupported("a switch with statements before its first case");
      return;
    }
    const CType *Ty = valueType(S->Value);
    line("switch " + value(S->Value, Ty).Text + " {");
    for (size_t A = 0; A < Arms->size(); ++A) {
      const SwitchArm &Arm = (*Arms)[A];
      std::vector<std::string> Values;
      bool Default = false;
      for (const Stmt *L : Arm.Labels) {
        if (L->Kind == StmtKind::Default) {
          Default = true;
          continue;
        }
        Values.push_back(
            literalText(truncated(*L->Value->Value, Ty), Ty, L->Value->Text));
      }
      if (!Values.empty())
        line("case " + llvm::join(Values, ", ") + ":");
      if (Default) {
        // `default` among other labels runs the same statements.
        if (!Values.empty()) {
          indent();
          line("fallthrough");
          dedent();
        }
        line("default:");
      }
      indent();
      for (const Stmt *B : Arm.Body)
        stmt(B);
      if (Arm.FallsThrough && A + 1 < Arms->size())
        line("fallthrough");
      dedent();
    }
    line("}");
  }

  void tryStmt(const Stmt *S) {
    openBlock(S->CxxTry ? "try" : "__try");
    body(S->Then);
    for (const Handler &H : S->Handlers) {
      dedent();
      leadingComments(H.Leading);
      switch (H.TheKind) {
      case Handler::Kind::Except:
        openBlock("} __except (" + value(H.Filter, nullptr).Text + ")");
        break;
      case Handler::Kind::Finally:
        openBlock("} __finally");
        break;
      case Handler::Kind::Catch:
        openBlock("} catch (" + H.Parameter.str() + ")");
        break;
      }
      body(H.Body);
    }
    closeBlock();
  }

  //===--- Declarations ---===//

  void declaration(const TopLevel &Item) override {
    for (const Decl *D : Item.Decls) {
      switch (D->Kind) {
      case DeclKind::Function:
        if (D->Body)
          function(D);
        else if (!Defined.contains(D->Name))
          line("func " + signature(D, /*Named=*/false));
        break;
      case DeclKind::Variable: {
        // A definition later in the text declares it.
        if (D->Extern && DefinedObjects.contains(D->Name))
          break;
        std::string Text = "var " + name(D->Name) + " " + type(D->Ty);
        if (D->Init)
          Text += " = " + initializer(D->Init, D->Ty);
        line(Text);
        break;
      }
      case DeclKind::Typedef:
        // Go names a record and its type alike: a typedef that names a
        // record by its own tag is the record, which its definition
        // defines, or which the typedef declares opaque.
        if (D->Ty->isRecord() && type(D->Ty) == escapeIdentifier(D->Name)) {
          if (!D->Ty->Record->Complete)
            record(D->Ty);
        } else if (!D->Ty->isInteger())
          line("type " + escapeIdentifier(D->Name) + " = " + type(D->Ty));
        break;
      case DeclKind::Record:
        record(D->Ty);
        break;
      case DeclKind::Enumeration:
        // C's enumeration is an int and its enumerators int constants.
        for (const Decl *Enumerator : D->Fields)
          line("const " + escapeIdentifier(Enumerator->Name) +
               " int32 = " + std::to_string(*Enumerator->Value));
        break;
      case DeclKind::Enumerator:
      case DeclKind::Parameter:
        break;
      }
    }
    trailingComments(Item.Trailing);
  }

  std::string signature(const Decl *D, bool Named) {
    const CType *Fn = D->Ty;
    std::string Text = name(D->Name) + "(";
    for (size_t I = 0; I < Fn->Params.size(); ++I) {
      const Decl *P = I < D->Params.size() ? D->Params[I] : nullptr;
      Text += I ? ", " : "";
      if (Named && P && !P->Name.empty())
        Text += escapeIdentifier(P->Name) + " ";
      Text += type(Fn->Params[I]);
    }
    if (Fn->Variadic || !Fn->Prototyped)
      Text += Fn->Params.empty() ? "...any" : ", ...any";
    Text += ")";
    if (!Fn->Inner->isVoid())
      Text += " " + type(Fn->Inner);
    return Text;
  }

  void function(const Decl *D) {
    for (llvm::StringRef A : D->Attributes)
      line("// " + A.str());
    ReturnType = D->Ty->Inner;
    noteLoadScratch(D->Body);
    openBlock("func " + signature(D, /*Named=*/true));
    body(D->Body);
    closeBlock();
  }

  void record(const CType *Ty) {
    // The program's types define their records themselves.
    if (!Ty->isRecord() || PrintedRecords.contains(Ty) || localRecord(Ty))
      return;
    const RecordDef &Record = *Ty->Record;
    // What a Go structure of these fields would not lay out as C does.
    if (Record.Union)
      return unsupported("a union");
    if (Record.Pack)
      return unsupported("a packed record");
    if (Record.Alignment)
      return unsupported("an aligned record");
    for (const RecordField &Field : Record.Fields) {
      if (Field.BitWidth)
        return unsupported("a bit-field");
      if (Field.isAnonymousRecord())
        return unsupported("an anonymous structure or union");
      if (Field.Type->Volatile)
        return unsupported("a volatile member");
    }
    PrintedRecords.insert(Ty);
    // A record the text only declares stays opaque: its address passes.
    if (!Record.Complete) {
      line("type " + type(Ty) + " struct{}");
      return;
    }
    openBlock("type " + type(Ty) + " struct");
    for (const RecordField &Field : Ty->Record->Fields)
      line(escapeIdentifier(Field.Name) + " " + type(Field.Type));
    closeBlock();
  }

  void staticAssert(const TopLevel &Item) override {
    line("// static assert: " +
         T.Source.slice(Item.Assertion->Begin, Item.Assertion->End).str());
    trailingComments(Item.Trailing);
  }

  llvm::DenseSet<const Decl *> FileScope;
  llvm::DenseSet<const CType *> PrintedRecords;
  llvm::StringSet<> Defined, DefinedObjects;
  const CType *ReturnType = nullptr;
};

} // namespace

std::unique_ptr<DialectPrinter>
csyntax::makeGoPrinter(const Tree &T, const SourceDialectOptions &Opts) {
  return std::make_unique<GoPrinter>(T, Opts);
}
