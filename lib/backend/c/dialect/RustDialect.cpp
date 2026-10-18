//===- RustDialect.cpp - Emitted C spelled as Rust ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Spells emitted C in Rust syntax with C's meaning: every conversion C
/// makes implicitly is an `as` cast, conditions compare with zero, `void *`
/// arithmetic is `byte_add`, a C `break` out of a switch arm leaves a
/// labeled block, and a `continue` in a do-while reaches its condition.
/// Integer arithmetic wraps as in the C view, which the header says once
/// rather than writing `wrapping_add` everywhere.
///
//===----------------------------------------------------------------------===//

#include "DialectPrinter.h"

#include "neverd/loader/SymbolSpelling.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"

using namespace neverd;
using namespace neverd::csyntax;

namespace {

const llvm::StringSet<> &rustKeywords() {
  static const llvm::StringSet<> Words = [] {
    llvm::StringSet<> Result;
#define NEVERD_RUST_KEYWORD(Word) Result.insert(Word);
#include "neverd/backend/c/dialect/DialectReservedWords.def"
    return Result;
  }();
  return Words;
}

struct BuiltinSpelling {
  llvm::StringRef Template;
  llvm::StringRef Result;
};

std::optional<BuiltinSpelling> rustBuiltin(llvm::StringRef Name) {
#define NEVERD_DIALECT_BUILTIN(CName, Rust, RustResult, Go, GoResult)          \
  if (Name == CName)                                                           \
    return BuiltinSpelling{Rust, RustResult};
#include "neverd/backend/c/dialect/DialectBuiltins.def"
  return std::nullopt;
}

/// Where a break or continue goes, innermost last.
struct Target {
  bool Loop = false;
  /// What `break` prints; empty when a trailing `break` ends an arm.
  std::string Break;
  std::string Continue;
};

class RustPrinter final : public DialectPrinter {
public:
  RustPrinter(const Tree &T, const SourceDialectOptions &Opts)
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
    line("// NeverD pseudocode in Rust syntax, with the C view's meaning:");
    line("// integers wrap, `addr as *mut T` points at an address, abort() "
         "traps,");
    line("// and c\"...\" points at a C string.");
  }

  std::string escapeIdentifier(llvm::StringRef Name) const override {
    if (Name == "self" || Name == "Self" || Name == "super" || Name == "crate")
      return (Name + "_").str();
    if (rustKeywords().contains(Name))
      return ("r#" + Name).str();
    return Name.str();
  }

  std::optional<std::string> sourceName(llvm::StringRef Symbol) const override {
    const SymbolScheme Scheme = symbolScheme(Symbol);
    if (Scheme != SymbolScheme::RustLegacy && Scheme != SymbolScheme::RustV0)
      return std::nullopt;
    std::string Readable = readableSymbolName(Symbol);
    if (Readable.empty())
      return std::nullopt;
    return Readable;
  }

  //===--- Types ---===//

  std::string type(const CType *Ty) const {
    switch (Ty->TheKind) {
    case CType::Kind::Void:
      return "()";
    case CType::Kind::Bool:
      return "bool";
    case CType::Kind::Integer:
      return (Ty->Signed ? "i" : "u") + std::to_string(Ty->Bits);
    case CType::Kind::Floating:
      return "f" + std::to_string(Ty->Bits);
    case CType::Kind::Pointer: {
      const CType *Pointee = Ty->Inner;
      // A segment-relative address is not an address of the flat memory a
      // Rust pointer reads.
      if (Pointee->AddressSpace)
        unsupported("a pointer into address space " +
                    llvm::Twine(Pointee->AddressSpace));
      if (Pointee->isFunction())
        return functionType(Pointee, {});
      std::string Target =
          Pointee->isVoid() ? std::string("c_void") : type(Pointee);
      return (Pointee->Const ? "*const " : "*mut ") + Target;
    }
    case CType::Kind::Array:
      if (!Ty->Count)
        return "[" + type(Ty->Inner) + "]";
      return "[" + type(Ty->Inner) + "; " + std::to_string(*Ty->Count) + "]";
    case CType::Kind::Function:
      return functionType(Ty, {});
    case CType::Kind::Record:
      return llvm::StringRef(Ty->Spelling).drop_front(7).str();
    case CType::Kind::Named:
      return Ty->Spelling.empty() ? "_" : Ty->Spelling;
    }
    return "_";
  }

  /// `extern "C" fn(A, B) -> R`; a function C declares with `()` takes the
  /// promoted types of \p Arguments.
  /// A function C declares with `()` takes the promoted types of the
  /// \p Arguments of a call, when \p Called.
  std::string functionType(const CType *Fn,
                           llvm::ArrayRef<const CType *> Arguments,
                           bool Called = false) const {
    std::string Result = "extern \"C\" fn(";
    llvm::ArrayRef<const CType *> Params =
        Fn->Prototyped ? llvm::ArrayRef<const CType *>(Fn->Params) : Arguments;
    for (size_t I = 0; I < Params.size(); ++I)
      Result += (I ? ", " : "") + type(Params[I]);
    if (Fn->Variadic || (!Fn->Prototyped && !Called))
      Result += Params.empty() ? "..." : ", ...";
    Result += ")";
    if (!Fn->Inner->isVoid())
      Result += " -> " + type(Fn->Inner);
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

  /// The literal \p V as \p Ty holds it, in the spelling of \p Like.
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

  /// \p P with its literal given \p Ty's suffix, where Rust cannot infer it.
  Printed typedLiteral(Printed P, const CType *Ty) const {
    if (!P.Literal || !Ty->isInteger() || Ty->TheKind == CType::Kind::Bool)
      return P;
    P.Text += type(Ty);
    P.Literal = false;
    return P;
  }

  /// \p P, a value of C type \p From, as a value of C type \p To.
  Printed convert(Printed P, const CType *From, const CType *To) {
    if (!To || !From || To->isVoid())
      return P;
    From = unqualifiedType(From);
    To = unqualifiedType(To);
    if (From->isUnknown() || To->isUnknown() || P.Diverges)
      return P;
    if (P.Bool) {
      if (To->TheKind == CType::Kind::Bool)
        return P;
      Printed R{paren(P, PrecCast) + " as " + type(To), PrecCast};
      if (To->isFloating())
        R.Text = paren(P, PrecCast) + " as u8 as " + type(To);
      return R;
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
    }
    if (sameType(From, To))
      return P;
    if (From->isPointer() && To->isPointer() &&
        sameType(unqualifiedType(From->Inner), unqualifiedType(To->Inner)))
      return P;
    if (To->TheKind == CType::Kind::Bool) {
      if (From->isPointer())
        return Printed{"!" + paren(P, PrecAtom) + ".is_null()", PrecUnary,
                       false, std::nullopt, false};
      Printed R{paren(P, PrecCompare + 1) + " != 0", PrecCompare};
      return R;
    }
    if (P.Literal)
      P = typedLiteral(P, From);
    return Printed{paren(P, PrecCast) + " as " + type(To), PrecCast};
  }

  static const CType *unqualifiedType(const CType *Ty) {
    // Qualifiers do not change a value's type in Rust.
    return Ty;
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
    // A character a byte holds is that byte's literal.
    if (E->Kind == ExprKind::Character && E->Text.starts_with("'") && Want &&
        Want->isInteger() && Want->Bits == 8 && !Want->Signed)
      return withComments(E, Printed{"b" + E->Text.str(), PrecAtom});
    if (const Expr *Narrow = narrowable(E, Want))
      return withComments(E, narrowed(Narrow, Want));
    return convert(raw(E), valueType(E), Want);
  }

  /// \p E computed at the narrower integer type \p Want (narrowable()).
  Printed narrowed(const Expr *E, const CType *Want) {
    Printed P;
    switch (E->Kind) {
    case ExprKind::Cast:
      P = value(E->Ops.front(), Want);
      break;
    case ExprKind::Unary: {
      Printed Op = value(E->Ops.front(), Want);
      if (E->Value) {
        P = convert(raw(E), E->Ty, Want);
        break;
      }
      if (E->Text == "~")
        P = Printed{"!" + paren(Op, PrecUnary), PrecUnary};
      else if (!Want->Signed)
        P = Printed{paren(typedLiteral(Op, Want), PrecAtom) + ".wrapping_neg()",
                    PrecAtom};
      else
        P = Printed{"-" + paren(Op, PrecUnary), PrecUnary};
      break;
    }
    default: {
      Printed L = value(E->Ops[0], Want);
      Printed R = value(E->Ops[1], Want);
      if (L.Literal && R.Literal)
        L = typedLiteral(L, Want);
      P = binaryText(L, E->Text, R, binaryPrecedence(E->Text));
      break;
    }
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
    if (Ty->isPointer())
      return Printed{"!" + paren(P, PrecAtom) + ".is_null()", PrecUnary, false,
                     std::nullopt, true};
    Printed R{paren(P, PrecCompare + 1) +
                  (Ty->isFloating() ? " != 0.0" : " != 0"),
              PrecCompare};
    R.Bool = true;
    return R;
  }

  /// Parentheses a reader expects where C's precedence differs from what
  /// the eye assumes: shifts and bitwise operators under another operator.
  static bool needsClarity(const Printed &Operand, llvm::StringRef Op) {
    static const llvm::StringSet<> Bitwise = {"<<", ">>", "&", "|", "^"};
    return !Operand.Operator.empty() && Operand.Operator != Op &&
           (Bitwise.contains(Operand.Operator) || Bitwise.contains(Op));
  }

  int binaryPrecedence(llvm::StringRef Op) const {
    if (Op == "*" || Op == "/" || Op == "%")
      return PrecMul;
    if (Op == "+" || Op == "-")
      return PrecAdd;
    if (Op == "<<" || Op == ">>")
      return PrecShift;
    if (Op == "&")
      return PrecBitAnd;
    if (Op == "^")
      return PrecBitXor;
    if (Op == "|")
      return PrecBitOr;
    if (Op == "&&")
      return PrecAnd;
    if (Op == "||")
      return PrecOr;
    return PrecCompare;
  }

  Printed binaryText(Printed L, llvm::StringRef Op, Printed R, int Prec,
                     bool Bool = false) {
    std::string LT = needsClarity(L, Op) ? "(" + L.Text + ")" : paren(L, Prec);
    // `x as u32 < y` would open generic arguments of u32.
    if (L.Prec == PrecCast && Op.starts_with("<"))
      LT = "(" + L.Text + ")";
    // Comparisons do not chain in Rust; other operators associate left.
    const int RightMin = Prec == PrecCompare ? Prec + 1 : Prec + 1;
    if (Prec == PrecCompare && L.Prec == PrecCompare)
      LT = "(" + L.Text + ")";
    std::string RT =
        needsClarity(R, Op) ? "(" + R.Text + ")" : paren(R, RightMin);
    Printed P{LT + " " + Op.str() + " " + RT, Prec};
    P.Bool = Bool;
    P.Operator = Op;
    return P;
  }

  /// The pointer \p Ptr moved by \p Offset elements of its pointee, or
  /// bytes for `void *` (GNU C).
  Printed pointerOffset(Printed Ptr, const Expr *Offset, bool Subtract,
                        const CType *PtrTy) {
    const bool Bytes = PtrTy->Inner->isVoid();
    const CType *OffsetTy = valueType(Offset);
    Printed N = raw(Offset);
    std::string Method;
    std::string Amount;
    if (N.Literal && N.Value) {
      int64_t V = *N.Value;
      if (Subtract)
        V = -V;
      Method = V < 0 ? "sub" : "add";
      Amount = std::to_string(V < 0 ? -static_cast<uint64_t>(V)
                                    : static_cast<uint64_t>(V));
    } else if (OffsetTy->Signed) {
      Method = "offset";
      Amount = paren(N, PrecCast) + " as isize";
      if (Subtract)
        Amount = "-(" + Amount + ")";
    } else {
      Method = Subtract ? "sub" : "add";
      Amount = paren(N, PrecCast) + " as usize";
    }
    if (Bytes)
      Method = "byte_" + Method;
    return Printed{paren(Ptr, PrecAtom) + "." + Method + "(" + Amount + ")",
                   PrecAtom};
  }

  Printed raw(const Expr *E) { return withComments(E, rawInner(E)); }

  Printed rawInner(const Expr *E) {
    switch (E->Kind) {
    case ExprKind::Name: {
      const bool File = E->Ref && FileScope.contains(E->Ref);
      std::string N = File ? name(E->Text) : escapeIdentifier(E->Text);
      if (E->Ty->isArray())
        return Printed{
            N + (E->Ty->Inner->Const ? ".as_ptr()" : ".as_mut_ptr()"),
            PrecAtom};
      if (E->Value && !E->Ref) {
        Printed P{N, PrecAtom};
        return P;
      }
      return Printed{N, PrecAtom};
    }
    case ExprKind::Integer: {
      Printed P{E->Text.rtrim("uUlL").str(), PrecAtom};
      P.Literal = true;
      P.Value = E->Value;
      return P;
    }
    case ExprKind::Floating:
      return Printed{floatLiteral(E), PrecAtom};
    case ExprKind::Character:
      return Printed{"b" + E->Text.str() + " as i32", PrecCast};
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
    case ExprKind::Subscript: {
      Printed P = place(E);
      return P;
    }
    case ExprKind::Member:
      if (auto Bits = bitField(E))
        return Printed{Bits->Holder + "." + Bits->Getter + "()", PrecAtom};
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
        return Printed{"size_of::<" + type(E->Written) + ">() as " +
                           type(E->Ty),
                       PrecCast};
      return Printed{"size_of_val(&" + raw(E->Ops.front()).Text + ") as " +
                         type(E->Ty),
                     PrecCast};
    case ExprKind::AlignofType:
      return Printed{"align_of::<" + type(E->Written) + ">() as " + type(E->Ty),
                     PrecCast};
    case ExprKind::Offsetof:
      if (E->ThroughAnonymous) {
        unsupported("a member of an anonymous structure or union");
        return Printed{"_", PrecAtom};
      }
      return Printed{"offset_of!(" + type(E->Written) + ", " + E->Text.str() +
                         ") as " + type(E->Ty),
                     PrecCast};
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

  std::string floatLiteral(const Expr *E) const {
    llvm::StringRef Text = E->Text;
    const bool Single = Text.ends_with_insensitive("f");
    Text = Text.rtrim("fFlL");
    llvm::APFloat F(llvm::APFloat::IEEEdouble());
    auto Status = F.convertFromString(Text, llvm::APFloat::rmNearestTiesToEven);
    std::string Result;
    if (!Status) {
      Result = Text.str();
    } else {
      llvm::SmallString<32> S;
      F.toString(S, /*FormatPrecision=*/0, /*FormatMaxPadding=*/0);
      Result = S.str().str();
      if (llvm::StringRef(Result).find_first_of(".eEn") == std::string::npos)
        Result += ".0";
    }
    return Result + (Single ? "_f32" : "");
  }

  /// \p Bytes as the byte string of an array of \p Count bytes, which C
  /// zero-fills past them.
  static std::string byteString(llvm::StringRef Bytes, uint64_t Count) {
    std::string Text = "*b\"";
    for (const unsigned char Byte : Bytes) {
      if (Byte == '"' || Byte == '\\')
        Text += {'\\', static_cast<char>(Byte)};
      else if (Byte == '\n')
        Text += "\\n";
      else if (Byte == '\t')
        Text += "\\t";
      else if (Byte == '\r')
        Text += "\\r";
      else if (!Byte)
        Text += "\\0";
      else if (Byte >= 32 && Byte < 127)
        Text += static_cast<char>(Byte);
      else
        Text += "\\x" + llvm::utohexstr(Byte, /*LowerCase=*/false, 2);
    }
    for (uint64_t I = Bytes.size(); I < Count; ++I)
      Text += "\\0";
    return Text + "\"";
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
    return (Wide ? "L\"" : "c\"") + Body + "\"";
  }

  Printed unary(const Expr *E) {
    const Expr *Op = E->Ops.front();
    const llvm::StringRef O = E->Text;
    if (O == "&") {
      if (Op->Ty->isFunction())
        return raw(Op);
      // `&(T){v}`: a pointer to a temporary.
      const Expr *Literal = skipParens(Op);
      if (Literal->Kind == ExprKind::CompoundLiteral &&
          Literal->Written->isScalar()) {
        const Expr *Init = Literal->Ops[0];
        if (Init->Kind == ExprKind::InitList && Init->Ops.size() == 1)
          Init = Init->Ops[0];
        Printed V =
            typedLiteral(value(Init, Literal->Written), Literal->Written);
        return Printed{"&mut " + paren(V, PrecUnary) + " as *mut " +
                           type(Literal->Written),
                       PrecCast};
      }
      Printed P = place(Op);
      const bool Const = Op->Ty->Const;
      return Printed{(Const ? "&raw const " : "&raw mut ") + P.Text, PrecUnary};
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
      if (P.Bool)
        R = Printed{"!" + paren(P, PrecUnary), PrecUnary};
      else if (Ty->isPointer())
        R = Printed{paren(P, PrecAtom) + ".is_null()", PrecAtom};
      else if (Ty->TheKind == CType::Kind::Bool)
        R = Printed{"!" + paren(P, PrecUnary), PrecUnary};
      else
        R = Printed{paren(P, PrecCompare + 1) + " == 0", PrecCompare};
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
    if (O == "~")
      return Printed{"!" + paren(P, PrecUnary), PrecUnary};
    if (E->OpTy && E->OpTy->isInteger() && !E->OpTy->Signed)
      return Printed{paren(typedLiteral(P, E->OpTy), PrecAtom) +
                         ".wrapping_neg()",
                     PrecAtom};
    return Printed{"-" + paren(P, PrecUnary), PrecUnary};
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
    if (!E->OpTy || E->OpTy->isUnknown()) {
      Printed P = binaryText(raw(L), O, raw(R), binaryPrecedence(O), Compare);
      return P;
    }
    if (Compare && E->OpTy->isPointer() && (O == "==" || O == "!=") &&
        (isNull(L) || isNull(R))) {
      const Expr *Ptr = isNull(L) ? R : L;
      Printed P{(O == "==" ? "" : "!") + paren(value(Ptr, E->OpTy), PrecAtom) +
                    ".is_null()",
                O == "==" ? PrecAtom : PrecUnary};
      P.Bool = true;
      return P;
    }
    if (E->OpTy->isPointer() && !Compare) {
      const CType *LT = valueType(L);
      if (LT->isPointer() && valueType(R)->isPointer())
        return Printed{paren(value(L, nullptr), PrecAtom) + ".offset_from(" +
                           value(R, nullptr).Text + ") as " + type(E->Ty),
                       PrecCast};
      const bool PointerLeft = LT->isPointer();
      return pointerOffset(value(PointerLeft ? L : R, E->OpTy),
                           PointerLeft ? R : L, O == "-", E->OpTy);
    }
    const bool Shift = O == "<<" || O == ">>";
    Printed LP = value(L, E->OpTy);
    Printed RP = Shift ? value(R, nullptr) : value(R, E->OpTy);
    if (LP.Literal && (RP.Literal || Shift))
      LP = typedLiteral(LP, E->OpTy);
    if (Shift && RP.Literal)
      RP = Printed{RP.Text, RP.Prec};
    Printed P = binaryText(LP, O, RP, binaryPrecedence(O), Compare);
    if (!Compare && E->Value && E->OpTy->isInteger()) {
      P.Value = E->Value;
    }
    return P;
  }

  Printed conditional(const Expr *E) {
    const Expr *C = E->Ops[0], *A = E->Ops[1], *B = E->Ops[2];
    const CType *Ty = E->OpTy;
    if (Ty && Ty->isInteger() && A->Value && B->Value && *A->Value == 1 &&
        *B->Value == 0)
      return Printed{paren(condition(C), PrecCast) + " as " + type(Ty),
                     PrecCast};
    Printed P{"if " + condition(C).Text + " { " + value(A, Ty).Text +
                  " } else { " + value(B, Ty).Text + " }",
              PrecLowest};
    return P;
  }

  Printed comma(const Expr *E) {
    if (isUnknownValue(E)) {
      Printed P{"abort()", PrecAtom};
      P.Diverges = true;
      return withComments(E->Ops[1], P);
    }
    if (const Expr *Source = unalignedLoadSource(E)) {
      const Expr *Var = E->Ops[1];
      const CType *Ty = valueType(Var);
      Printed Load{"(" + paren(stripVoidCast(Source), PrecCast) +
                       " as *const " + type(Ty) + ").read_unaligned()",
                   PrecAtom};
      if (isLoadScratch(Var))
        return Load;
      return Printed{"{ " + raw(Var).Text + " = " + Load.Text + "; " +
                         raw(Var).Text + " }",
                     PrecAtom};
    }
    std::string Text = "{ ";
    for (size_t I = 0; I + 1 < E->Ops.size(); ++I)
      Text += value(E->Ops[I], nullptr).Text + "; ";
    Text += value(E->Ops.back(), nullptr).Text + " }";
    return Printed{Text, PrecAtom};
  }

  /// A memcpy source cast to `const void *` reads as the address it is.
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
      return Printed{"let _ = " + value(Op, nullptr).Text, PrecLowest};
    if (narrowable(E, To))
      return narrowed(skipParens(E), To);
    Printed P = raw(Op);
    const CType *From = valueType(Op);
    if (P.Literal && P.Value && To->isInteger() &&
        To->TheKind != CType::Kind::Bool) {
      const int64_t V = truncated(*P.Value, To);
      Printed R{literalText(V, To, P.Text) + type(To),
                V < 0 ? PrecUnary : PrecAtom};
      R.Value = V;
      return R;
    }
    if (To->isPointer() && To->Inner->isFunction() && From->isInteger())
      return Printed{paren(P, PrecCast) + " as " + functionType(To->Inner, {}),
                     PrecCast};
    if (To->isPointer())
      if (const Expr *Address = addressOperand(Op))
        return convert(raw(Address), valueType(Address), To);
    return convert(P, From, To);
  }

  /// The integer an address cast reads through same-width integer casts:
  /// `(T *)(uintptr_t)(v + 16)` points at `v + 16` whatever its signedness.
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

  Printed call(const Expr *E) {
    const Expr *Callee = E->Ops.front();
    while (Callee->Kind == ExprKind::Paren)
      Callee = Callee->Ops.front();
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
      if (auto Overflow = overflowBuiltin(E, Callee->Text))
        return *Overflow;
      if (auto Spelling = rustBuiltin(Callee->Text)) {
        std::vector<Printed> Args;
        for (size_t I = 0; I + 1 < E->Ops.size(); ++I)
          Args.push_back(Argument(I));
        std::string Text = substitute(Spelling->Template, Args, Fn);
        Printed P{Text, PrecAtom};
        if (Spelling->Template.contains(" as "))
          P.Prec = PrecAtom;
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
      // `((R (*)())p)(a, b)` calls p with the arguments' types.
      const Expr *Target = Callee->Ops.front();
      if (const Expr *Address = addressOperand(Target))
        Target = Address;
      Text = "(" + paren(value(Target, nullptr), PrecCast) + " as " +
             functionType(Fn, ArgTypes, /*Called=*/true) + ")";
    } else {
      Text = paren(value(Callee, nullptr), PrecAtom);
    }
    Text += "(";
    for (size_t I = 0; I + 1 < E->Ops.size(); ++I)
      Text += (I ? ", " : "") + Argument(I).Text;
    Text += ")";
    return Printed{Text, PrecAtom};
  }

  /// The type C passes an argument without a prototype in (C17 6.5.2.2).
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

  std::string substitute(llvm::StringRef Template, llvm::ArrayRef<Printed> Args,
                         const CType *Fn) {
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
      const llvm::StringRef After = Template.drop_front(I + 1);
      Printed Arg = Args[Index];
      if (After.starts_with(".")) {
        if (Fn && Index < Fn->Params.size())
          Arg = typedLiteral(Arg, Fn->Params[Index]);
        Result += paren(Arg, PrecAtom);
      } else if (After.starts_with(" as ")) {
        Result += paren(Arg, PrecCast);
      } else {
        Result += Arg.Text;
      }
    }
    return Result;
  }

  /// `__builtin_add_overflow(a, b, &(T){0})` with a, b of type T: whether
  /// `a + b` wraps in T.
  std::optional<Printed> overflowBuiltin(const Expr *E, llvm::StringRef Name) {
    const llvm::StringRef Method =
        llvm::StringSwitch<llvm::StringRef>(Name)
            .Case("__builtin_add_overflow", "overflowing_add")
            .Case("__builtin_sub_overflow", "overflowing_sub")
            .Case("__builtin_mul_overflow", "overflowing_mul")
            .Default("");
    if (Method.empty() || E->Ops.size() != 4)
      return std::nullopt;
    const Expr *Out = E->Ops[3];
    if (Out->Kind != ExprKind::Unary || Out->Text != "&" ||
        Out->Ops.front()->Kind != ExprKind::CompoundLiteral)
      return std::nullopt;
    const CType *Ty = Out->Ops.front()->Written;
    if (!sameType(valueType(E->Ops[1]), Ty) ||
        !sameType(valueType(E->Ops[2]), Ty))
      return std::nullopt;
    Printed A = typedLiteral(value(E->Ops[1], Ty), Ty);
    Printed P{paren(A, PrecAtom) + "." + Method.str() + "(" +
                  value(E->Ops[2], Ty).Text + ").1",
              PrecAtom};
    return P;
  }

  /// The place \p E, a member of one of the program's types, names through
  /// \p Hops, which Rust reaches as fields.
  std::string localPlace(const Expr *E,
                         llvm::ArrayRef<SourceDialectLocalTypes::Hop> Hops) {
    const Expr *Base = E->Ops.front();
    std::string Text = E->Postfix
                           ? "(*" + paren(value(Base, nullptr), PrecUnary) + ")"
                           : paren(place(Base), PrecAtom);
    for (const SourceDialectLocalTypes::Hop &Hop : Hops) {
      if (Hop.Accessor) {
        unsupported("a member the program's types read through a method");
        return "_";
      }
      Text += "." + Hop.Name;
    }
    return Text;
  }

  /// A bit-field of one of the program's types, as Rust reads and writes
  /// it: the record whose methods do, and their names.
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
    return BitField{localPlace(E, Path->Hops), Path->Getter, Path->Setter};
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
      const CType *IndexTy = valueType(Index);
      Printed I = raw(Index);
      std::string IndexText =
          I.Literal && I.Value && *I.Value >= 0
              ? std::to_string(*I.Value)
              : paren(convert(I, IndexTy, IndexTy), PrecCast) + " as usize";
      if (Base->Ty->isArray()) {
        Printed B = place(Base);
        return Printed{paren(B, PrecAtom) + "[" + IndexText + "]", PrecAtom};
      }
      return Printed{"*" + paren(value(Base, nullptr), PrecAtom) + ".add(" +
                         IndexText + ")",
                     PrecUnary};
    }
    case ExprKind::Member: {
      const Expr *Base = E->Ops.front();
      // A member of one of the program's types, by the fields that hold it.
      if (auto Path = localMember(E)) {
        // Only the methods of a bit-field's record read and write it.
        if (Path->bitField())
          unsupported("a bit-field where Rust needs a place");
        if (Path->Hops.empty() || refused())
          return Printed{"_", PrecAtom};
        return Printed{localPlace(E, Path->Hops), PrecAtom};
      }
      if (E->Field && E->Field->BitWidth) {
        unsupported("a bit-field");
        return Printed{"_", PrecAtom};
      }
      if (E->ThroughAnonymous) {
        unsupported("a member of an anonymous structure or union");
        return Printed{"_", PrecAtom};
      }
      if (E->Postfix)
        return Printed{"(*" + paren(value(Base, nullptr), PrecUnary) + ")." +
                           E->Text.str(),
                       PrecAtom};
      return Printed{paren(place(Base), PrecAtom) + "." + E->Text.str(),
                     PrecAtom};
    }
    case ExprKind::String:
      return Printed{stringLiteral(E), PrecAtom};
    default:
      break;
    }
    unsupported("an assignment to a value Rust cannot name");
    return Printed{"_", PrecAtom};
  }

  Printed statementExpr(const Expr *E) {
    const Stmt *Body = E->Body;
    const Stmt *Last = nullptr;
    for (const Stmt *S : Body->Body)
      if (S->Kind != StmtKind::Comment)
        Last = S;
    const std::string Inner = capture([&] {
      for (const Stmt *S : Body->Body) {
        if (S == Last && S->Kind == StmtKind::Expression) {
          line(value(S->Value, nullptr).Text);
          continue;
        }
        stmt(S);
      }
    });
    return Printed{"{\n" + Inner + indentation(depth()) + "}", PrecAtom};
  }

  Printed bitCast(const Expr *E) {
    const Expr *Op = E->Ops.front();
    const CType *From = valueType(Op);
    const CType *To = E->Written;
    Printed P = raw(Op);
    if (P.Literal)
      P = typedLiteral(P, From);
    const bool SameSize = From->Bits && From->Bits == To->Bits;
    if (SameSize && (From->isInteger() || From->isPointer()) &&
        (To->isInteger() || To->isPointer()))
      return Printed{paren(P, PrecCast) + " as " + type(To), PrecCast};
    if (From->isFloating() && To->isInteger()) {
      Printed Bits{paren(P, PrecAtom) + ".to_bits()", PrecAtom};
      return convert(
          Bits,
          const_cast<TypeContext &>(T.Types).integer(From->Bits, false, "bits"),
          To);
    }
    if (From->isInteger() && To->isFloating()) {
      const CType *Bits =
          const_cast<TypeContext &>(T.Types).integer(To->Bits, false, "bits");
      return Printed{type(To) + "::from_bits(" + convert(P, From, Bits).Text +
                         ")",
                     PrecAtom};
    }
    return Printed{"transmute::<" + type(From) + ", " + type(To) + ">(" +
                       P.Text + ")",
                   PrecAtom};
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

  /// The statements of \p S, without its braces.
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

  const Target *breakTarget() const {
    for (auto It = Targets.rbegin(); It != Targets.rend(); ++It)
      return &*It;
    return nullptr;
  }
  const Target *continueTarget() const {
    for (auto It = Targets.rbegin(); It != Targets.rend(); ++It)
      if (It->Loop)
        return &*It;
    return nullptr;
  }

  std::string newLabel(llvm::StringRef Kind) {
    return ("'" + Kind + std::to_string(++Labels)).str();
  }

  void loopBody(const Stmt *Body, Target Into) {
    Targets.push_back(std::move(Into));
    body(Body);
    Targets.pop_back();
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
      openBlock(Forever ? "loop" : "while " + condition(S->Value).Text);
      loopBody(S->Then, Target{true, "break", "continue"});
      closeBlock();
      return;
    }
    case StmtKind::DoWhile:
      doWhile(S);
      return;
    case StmtKind::For:
      forLoop(S);
      return;
    case StmtKind::Switch:
      switchStmt(S);
      return;
    case StmtKind::Case:
    case StmtKind::Default:
      unsupported("a case label outside its switch");
      return;
    case StmtKind::Break: {
      const Target *To = breakTarget();
      if (!To) {
        unsupported("a break outside a loop");
        return;
      }
      if (!To->Break.empty())
        line(To->Break + ";");
      return;
    }
    case StmtKind::Continue: {
      const Target *To = continueTarget();
      if (!To) {
        unsupported("a continue outside a loop");
        return;
      }
      line(To->Continue + ";");
      return;
    }
    case StmtKind::Return:
      if (!S->Value) {
        line("return;");
        return;
      }
      line("return " + value(S->Value, ReturnType).Text + ";");
      return;
    case StmtKind::Goto:
      line("goto " + S->Text.str() + ";");
      return;
    case StmtKind::Label: {
      // Labels keep C's column 0.
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
        line("throw;");
        return;
      }
      line("throw " + T.Source.slice(S->Value->Begin, S->Value->End).str() +
           ";");
      return;
    case StmtKind::Leave:
      line("__leave;");
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

  void expressionStatement(const Expr *E) {
    while (E->Kind == ExprKind::Paren && E->Leading.empty() &&
           E->Trailing.empty())
      E = E->Ops.front();
    switch (E->Kind) {
    case ExprKind::Assign: {
      const Expr *L = E->Ops[0], *R = E->Ops[1];
      const CType *Target = valueType(L);
      // A bit-field takes what its setter stores, computed from its getter.
      if (auto Bits = bitField(L)) {
        Printed Stored = value(R, Target);
        if (E->Text != "=") {
          const llvm::StringRef Op = E->Text.drop_back();
          Printed Current = convert(
              Printed{Bits->Holder + "." + Bits->Getter + "()", PrecAtom},
              Target, E->OpTy);
          Stored = convert(
              binaryText(Current, Op, value(R, E->OpTy), binaryPrecedence(Op)),
              E->OpTy, Target);
        }
        line(Bits->Holder + "." + Bits->Setter + "(" + Stored.Text + ");");
        return;
      }
      Printed Place = place(L);
      if (E->Text == "=") {
        line(Place.Text + " = " + value(R, Target).Text + ";");
        return;
      }
      const llvm::StringRef Op = E->Text.drop_back();
      if (Target->isPointer()) {
        Printed Moved = pointerOffset(Place, R, Op == "-", Target);
        line(Place.Text + " = " + Moved.Text + ";");
        return;
      }
      const bool Shift = Op == "<<" || Op == ">>";
      if (sameType(E->OpTy, Target)) {
        line(Place.Text + " " + E->Text.str() + " " +
             (Shift ? value(R, nullptr) : value(R, E->OpTy)).Text + ";");
        return;
      }
      // Low bits come out the same at the target's width.
      if (Op == "+" || Op == "-" || Op == "*" || Op == "&" || Op == "|" ||
          Op == "^") {
        line(Place.Text + " " + E->Text.str() + " " + value(R, Target).Text +
             ";");
        return;
      }
      Printed Wide = convert(Place, Target, E->OpTy);
      Printed Result =
          binaryText(Wide, Op, value(R, E->OpTy), binaryPrecedence(Op));
      line(Place.Text + " = " + convert(Result, E->OpTy, Target).Text + ";");
      return;
    }
    case ExprKind::Unary:
      if (E->Text == "++" || E->Text == "--") {
        if (auto Bits = bitField(E->Ops.front())) {
          line(Bits->Holder + "." + Bits->Setter + "(" + Bits->Holder + "." +
               Bits->Getter + "()" + (E->Text == "++" ? " + 1" : " - 1") +
               ");");
          return;
        }
        line(place(E->Ops.front()).Text +
             (E->Text == "++" ? " += 1;" : " -= 1;"));
        return;
      }
      break;
    case ExprKind::Cast:
      if (E->Written->isVoid()) {
        line("let _ = " + value(E->Ops.front(), nullptr).Text + ";");
        return;
      }
      break;
    case ExprKind::Comma:
      if (!isUnknownValue(E) && !unalignedLoadSource(E)) {
        for (const Expr *Op : E->Ops)
          expressionStatement(Op);
        return;
      }
      break;
    default:
      break;
    }
    line(value(E, nullptr).Text + ";");
  }

  void local(const Decl *D) {
    // A record the declaration defines (a frame's layout) prints first.
    record(D->Ty);
    std::string Text = D->Static      ? "static mut "
                       : D->Ty->Const ? "let "
                                      : "let mut ";
    Text += escapeIdentifier(D->Name) + ": " + type(D->Ty);
    if (D->Init)
      Text += " = " + initializer(D->Init, D->Ty);
    Text += ";";
    if (D->AlignAs)
      Text += " /* aligned to " + std::to_string(*D->AlignAs) + " */";
    if (!D->AsmLabel.empty())
      Text += " /* in register " + D->AsmLabel.str() + " */";
    line(Text);
  }

  std::string initializer(const Expr *Init, const CType *Ty) {
    if (Init->Kind != ExprKind::InitList) {
      if (Ty->isArray() && Init->Kind == ExprKind::String) {
        if (byteArray(Ty))
          if (const auto Bytes = stringBytes(Init);
              Bytes && Bytes->size() <= *Ty->Count)
            return byteString(*Bytes, *Ty->Count);
        return stringLiteral(Init);
      }
      return value(Init, Ty).Text;
    }
    if (Ty->isArray()) {
      // Bytes that are mostly text read as Rust's byte string of them.
      if (byteArray(Ty))
        if (const auto Bytes = listBytes(Init, *Ty->Count)) {
          size_t Text = 0, Set = 0;
          for (const unsigned char Byte : *Bytes) {
            Set += Byte != 0;
            Text += (Byte >= 32 && Byte < 127) || Byte == '\n' ||
                    Byte == '\t' || Byte == '\r';
          }
          if (Set && Text * 4 >= Set * 3)
            return byteString(*Bytes, *Ty->Count);
        }
      // Each element in its place, designated or next; C zeroes the rest.
      std::vector<std::string> Elements;
      size_t Next = 0;
      for (const Expr *Item : Init->Ops) {
        if (Item->Kind == ExprKind::Designated) {
          if (Item->Ops.size() != 2 || !Item->Ops[0]->Value ||
              *Item->Ops[0]->Value < 0 ||
              (Ty->Count &&
               static_cast<uint64_t>(*Item->Ops[0]->Value) >= *Ty->Count)) {
            unsupported("an array designator outside its array");
            return "_";
          }
          Next = static_cast<size_t>(*Item->Ops[0]->Value);
          Item = Item->Ops[1];
        }
        if (Elements.size() <= Next)
          Elements.resize(Next + 1);
        Elements[Next++] = initializer(Item, Ty->Inner);
      }
      if (Ty->Count && Ty->Inner->isScalar() && Elements.size() < *Ty->Count)
        Elements.resize(*Ty->Count);
      for (std::string &Element : Elements)
        if (Element.empty()) {
          if (!Ty->Inner->isScalar()) {
            unsupported("a partly initialized array of records");
            return "_";
          }
          Element = "0";
        }
      if (Ty->Count && Elements.size() < *Ty->Count) {
        unsupported("a partly initialized array of records");
        return "_";
      }
      return "[" + llvm::join(Elements, ", ") + "]";
    }
    if (Ty->isRecord()) {
      std::string Text = type(Ty) + " { ";
      for (size_t I = 0; I < Init->Ops.size(); ++I) {
        if (!Ty->Record->Complete || I >= Ty->Record->Fields.size()) {
          unsupported("a record initializer");
          return "_";
        }
        const RecordField &Field = Ty->Record->Fields[I];
        Text += (I ? ", " : "") + Field.Name + ": " +
                initializer(Init->Ops[I], Field.Type);
      }
      return Text + " }";
    }
    if (Init->Ops.size() == 1)
      return initializer(Init->Ops.front(), Ty);
    unsupported("a scalar initializer list");
    return "_";
  }

  void doWhile(const Stmt *S) {
    const bool Once = S->Value->Value && *S->Value->Value == 0;
    const bool Continues = continuesLoop(S->Then);
    if (Once && !Continues && !breaksOut(S->Then)) {
      // `do { ... } while (0)` only scopes its statements.
      openBlock("");
      body(S->Then);
      closeBlock();
      return;
    }
    openBlock("loop");
    if (Continues) {
      const std::string Label = newLabel("body");
      openBlock(Label + ":");
      loopBody(S->Then, Target{true, "break", "break " + Label});
      closeBlock();
    } else {
      loopBody(S->Then, Target{true, "break", "continue"});
    }
    if (Once)
      line("break;");
    else if (!(S->Value->Value && *S->Value->Value != 0))
      line("if " + negated(S->Value) + " { break; }");
    closeBlock();
  }

  std::string negated(const Expr *Cond) {
    Printed P = condition(Cond);
    return "!" + paren(P, PrecUnary);
  }

  void forLoop(const Stmt *S) {
    if (S->Value)
      expressionStatement(S->Value);
    const Expr *Cond = S->Else ? S->Else->Value : nullptr;
    const bool Continues = S->Step && continuesLoop(S->Then);
    openBlock(Cond ? "while " + condition(Cond).Text : std::string("loop"));
    if (Continues) {
      const std::string Label = newLabel("body");
      openBlock(Label + ":");
      loopBody(S->Then, Target{true, "break", "break " + Label});
      closeBlock();
    } else {
      loopBody(S->Then, Target{true, "break", "continue"});
    }
    if (S->Step)
      expressionStatement(S->Step);
    closeBlock();
  }

  void switchStmt(const Stmt *S) {
    auto Arms = switchArms(S);
    if (!Arms) {
      unsupported("a switch with statements before its first case");
      return;
    }
    const CType *Ty = valueType(S->Value);
    bool InnerBreak = false;
    for (const SwitchArm &Arm : *Arms) {
      if (Arm.FallsThrough) {
        unsupported("a switch case that falls through");
        return;
      }
      for (size_t I = 0; I < Arm.Body.size(); ++I) {
        const Stmt *B = Arm.Body[I];
        const bool Trailing =
            B->Kind == StmtKind::Break && I + 1 == Arm.Body.size();
        if (!Trailing && breaksOut(B))
          InnerBreak = true;
      }
    }
    std::string Label;
    if (InnerBreak) {
      Label = newLabel("switch");
      openBlock(Label + ":");
    }
    openBlock("match " + value(S->Value, Ty).Text);
    bool HasDefault = false;
    for (const SwitchArm &Arm : *Arms) {
      std::vector<std::string> Patterns;
      for (const Stmt *L : Arm.Labels) {
        if (L->Kind == StmtKind::Default) {
          HasDefault = true;
          Patterns.push_back("_");
          continue;
        }
        Patterns.push_back(
            literalText(truncated(*L->Value->Value, Ty), Ty, L->Value->Text));
      }
      // `_` already covers the other patterns of its arm.
      if (llvm::is_contained(Patterns, "_"))
        Patterns = {"_"};
      openBlock(llvm::join(Patterns, " | ") + " =>");
      Targets.push_back(Target{false, InnerBreak ? "break " + Label : "", ""});
      for (size_t I = 0; I < Arm.Body.size(); ++I) {
        const Stmt *B = Arm.Body[I];
        if (B->Kind == StmtKind::Break && I + 1 == Arm.Body.size()) {
          leadingComments(B->Leading);
          trailingComments(B->Trailing);
          continue;
        }
        stmt(B);
      }
      Targets.pop_back();
      closeBlock();
    }
    if (!HasDefault)
      line("_ => {}");
    closeBlock();
    if (InnerBreak)
      closeBlock();
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
          prototype(D);
        break;
      case DeclKind::Variable:
        // A definition later in the text declares it.
        if (!D->Extern || !DefinedObjects.contains(D->Name))
          global(D);
        break;
      case DeclKind::Typedef:
        // Rust names a record and its type alike: a typedef that names a
        // record by its own tag is the record, which its definition
        // defines, or which the typedef declares opaque.
        if (D->Ty->isRecord() && type(D->Ty) == escapeIdentifier(D->Name)) {
          if (!D->Ty->Record->Complete)
            record(D->Ty);
        } else if (!D->Ty->isInteger())
          line("type " + escapeIdentifier(D->Name) + " = " + type(D->Ty) + ";");
        break;
      case DeclKind::Record:
        record(D->Ty);
        break;
      case DeclKind::Enumeration:
        // C's enumeration is an int and its enumerators int constants.
        for (const Decl *Enumerator : D->Fields)
          line("const " + escapeIdentifier(Enumerator->Name) +
               ": i32 = " + std::to_string(*Enumerator->Value) + ";");
        break;
      case DeclKind::Enumerator:
      case DeclKind::Parameter:
        break;
      }
    }
    trailingComments(Item.Trailing);
  }

  /// The parameters a body assigns or takes the address of, which Rust
  /// binds `mut`.
  llvm::DenseSet<const Decl *> mutatedParameters(const Decl *D) const {
    llvm::DenseSet<const Decl *> Result;
    if (!D->Body)
      return Result;
    forEachExpr(D->Body, [&](const Expr *E) {
      const bool Writes =
          E->Kind == ExprKind::Assign ||
          (E->Kind == ExprKind::Unary &&
           (E->Text == "&" || E->Text == "++" || E->Text == "--"));
      if (!Writes)
        return;
      const Expr *Target = skipParens(E->Ops.front());
      if (Target->Kind == ExprKind::Name && Target->Ref &&
          Target->Ref->Kind == DeclKind::Parameter)
        Result.insert(Target->Ref);
    });
    return Result;
  }

  std::string signature(const Decl *D, bool Named) {
    const CType *Fn = D->Ty;
    const llvm::DenseSet<const Decl *> Mutated =
        Named ? mutatedParameters(D) : llvm::DenseSet<const Decl *>();
    std::string Text = name(D->Name) + "(";
    for (size_t I = 0; I < Fn->Params.size(); ++I) {
      const Decl *P = I < D->Params.size() ? D->Params[I] : nullptr;
      std::string ParamName =
          Named && P && !P->Name.empty() ? escapeIdentifier(P->Name) : "_";
      if (P && Mutated.contains(P))
        ParamName = "mut " + ParamName;
      Text += (I ? ", " : "") + ParamName + ": " + type(Fn->Params[I]);
    }
    if (Fn->Variadic || !Fn->Prototyped)
      Text += Fn->Params.empty() ? "..." : ", ...";
    Text += ")";
    bool Noreturn = D->Noreturn;
    for (llvm::StringRef A : D->Attributes)
      Noreturn |= A.contains("noreturn");
    if (Noreturn)
      Text += " -> !";
    else if (!Fn->Inner->isVoid())
      Text += " -> " + type(Fn->Inner);
    return Text;
  }

  void function(const Decl *D) {
    for (llvm::StringRef A : D->Attributes) {
      const size_t Target = A.find("target(\"");
      if (Target != llvm::StringRef::npos) {
        const llvm::StringRef Features =
            A.drop_front(Target + 8).take_until([](char C) {
              return C == '"';
            });
        line("#[target_feature(enable = \"" + Features.str() + "\")]");
      }
    }
    if (D->Inline)
      line("#[inline]");
    ReturnType = D->Ty->Inner;
    noteLoadScratch(D->Body);
    openBlock("unsafe fn " + signature(D, /*Named=*/true));
    Targets.clear();
    body(D->Body);
    closeBlock();
  }

  void prototype(const Decl *D) {
    line("extern \"C\" { fn " + signature(D, /*Named=*/false) + "; }");
  }

  void global(const Decl *D) {
    std::string Text = "static mut " + name(D->Name) + ": " + type(D->Ty);
    if (D->Init)
      Text += " = " + initializer(D->Init, D->Ty);
    Text += ";";
    if (D->Extern)
      Text = "extern \"C\" { " + Text + " }";
    line(Text);
  }

  void record(const CType *Ty) {
    // The program's types define their records themselves.
    if (!Ty->isRecord() || PrintedRecords.contains(Ty) || localRecord(Ty))
      return;
    const RecordDef &Record = *Ty->Record;
    // What a Rust structure of these fields would not lay out as C does.
    if (Record.Union)
      return unsupported("a union");
    for (const RecordField &Field : Record.Fields) {
      if (Field.BitWidth)
        return unsupported("a bit-field");
      if (Field.isAnonymousRecord())
        return unsupported("an anonymous structure or union");
      if (Field.Type->Volatile)
        return unsupported("a volatile member");
    }
    PrintedRecords.insert(Ty);
    std::string Repr = "C";
    if (Record.Pack)
      Repr += ", packed(" + std::to_string(Record.Pack) + ")";
    if (Record.Alignment)
      Repr += ", align(" + std::to_string(Record.Alignment) + ")";
    line("#[repr(" + Repr + ")]");
    openBlock("struct " + type(Ty));
    // A record the text only declares stays opaque: its address passes.
    if (!Record.Complete) {
      line("_opaque: [u8; 0],");
      closeBlock();
      return;
    }
    for (const RecordField &Field : Record.Fields)
      line(escapeIdentifier(Field.Name) + ": " + type(Field.Type) + ",");
    closeBlock();
  }

  void staticAssert(const TopLevel &Item) override {
    line("const _: () = assert!(" + condition(Item.Assertion).Text + ");");
    trailingComments(Item.Trailing);
  }

  llvm::DenseSet<const Decl *> FileScope;
  llvm::DenseSet<const CType *> PrintedRecords;
  llvm::StringSet<> Defined, DefinedObjects;
  std::vector<Target> Targets;
  const CType *ReturnType = nullptr;
  unsigned Labels = 0;
};

} // namespace

std::unique_ptr<DialectPrinter>
csyntax::makeRustPrinter(const Tree &T, const SourceDialectOptions &Opts) {
  return std::make_unique<RustPrinter>(T, Opts);
}
