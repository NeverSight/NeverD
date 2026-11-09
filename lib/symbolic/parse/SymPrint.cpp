//===- SymPrint.cpp - Rendering symbolic expressions ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements \c SymContext::toString.  The precedence ladder shared with the
/// parser lives in SymParseDetail.h so the two halves of the textual syntax
/// cannot drift after living in separate translation units.
///
//===----------------------------------------------------------------------===//

#include "../SymPrintShape.h"
#include "SymParseDetail.h"

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"

#include <optional>
#include <string>
#include <utility>

namespace neverd::symbolic {

namespace {

using namespace detail;

bool isTextIdentifier(llvm::StringRef Name) {
  return !Name.empty() && isIdentStart(Name.front()) &&
         llvm::all_of(Name.drop_front(), isIdentBody);
}

/// A rendered subexpression, tagged with the strength of its outermost
/// operator so the parent can decide about parentheses.
struct Fragment {
  std::string Text;
  int Prec = PrecPrimary;
};

std::string wrap(const Fragment &F, int Need) {
  if (F.Prec >= Need)
    return F.Text;
  return "(" + F.Text + ")";
}

// Append directly to the parent's buffer. Materializing a wrapped copy for
// every child allocates and copies long MBA subexpressions twice.
void appendWrapped(std::string &Out, const Fragment &F, int Need) {
  if (F.Prec < Need)
    Out += '(';
  Out += F.Text;
  if (F.Prec < Need)
    Out += ')';
}

void appendDigits(std::string &Out, const llvm::APInt &V) {
  llvm::SmallString<48> S;
  // Small numbers read as quantities and belong in decimal.  Larger ones are
  // nearly always masks or magic constants, where the hex form is the one that
  // carries the meaning.
  if (V.ule(9999)) {
    V.toString(S, 10, /*Signed=*/false);
  } else {
    Out += "0x";
    V.toString(S, 16, /*Signed=*/false);
  }
  Out.append(S.begin(), S.end());
}

class Printer {
public:
  Printer(const SymContext &Ctx, uint32_t Ambient)
      : Ctx(Ctx), Ambient(Ambient) {}

  std::string run(SymRef Root);

private:
  const Fragment &frag(SymRef R) const { return Memo.find(R.index())->second; }
  /// A child in a comma-separated argument list, which never needs brackets.
  const std::string &arg(SymRef R) const { return frag(R).Text; }
  std::string sub(SymRef R, int Need) const { return wrap(frag(R), Need); }

  std::string join(llvm::ArrayRef<SymRef> Ops, const char *Sep, int Need) const;
  std::string call(const char *Name, llvm::ArrayRef<SymRef> Ops) const;

  Fragment leaf(std::string Text, uint32_t W) const {
    if (W != Ambient) {
      Text += '#';
      Text += std::to_string(W);
    }
    return {std::move(Text), PrecPrimary};
  }

  Fragment literal(const llvm::APInt &Mag, uint32_t W) const {
    std::string T;
    appendDigits(T, Mag);
    return leaf(std::move(T), W);
  }

  /// The unsigned magnitude of a term that is worth writing behind a minus
  /// sign, and whether that sign can ride on a leading literal coefficient
  /// instead of governing the whole term.
  struct Magnitude {
    Fragment Frag;
    bool CoeffLed = false;
  };

  std::optional<Fragment> negatedLiteral(const llvm::APInt &V) const;
  std::optional<Magnitude> negatedProduct(SymRef R) const;
  std::optional<Magnitude> negatedTerm(SymRef R) const;
  std::string negate(const Magnitude &M) const;

  Fragment render(SymRef R) const;

  const SymContext &Ctx;
  uint32_t Ambient;
  // Completed fragments are consumed before inserting their parent. No
  // reference survives growth, so one dense table owns all fragment strings.
  // Widen node IDs to keep every valid handle distinct from the sentinels.
  llvm::SmallDenseMap<uint64_t, Fragment,
                      limits::symbolic::kSymExprTraversalInlineBuckets>
      Memo;
};

std::string Printer::join(llvm::ArrayRef<SymRef> Ops, const char *Sep,
                          int Need) const {
  std::string T;
  for (unsigned I = 0, E = Ops.size(); I < E; ++I) {
    if (I)
      T += Sep;
    appendWrapped(T, frag(Ops[I]), Need);
  }
  return T;
}

std::string Printer::call(const char *Name, llvm::ArrayRef<SymRef> Ops) const {
  std::string T = Name;
  T += '(';
  for (unsigned I = 0, E = Ops.size(); I < E; ++I) {
    if (I)
      T += ", ";
    T += arg(Ops[I]);
  }
  T += ')';
  return T;
}

/// The magnitude of \p V, when writing it behind a minus sign is faithful.
std::optional<Fragment> Printer::negatedLiteral(const llvm::APInt &V) const {
  // At one bit, -1 and 1 are the same value and the minus sign only confuses.
  if (!print_detail::hasSignedMagnitude(V))
    return std::nullopt;
  llvm::APInt Mag = -V;
  return literal(Mag, V.getBitWidth());
}

/// The magnitude of a product with a negative leading coefficient, so a sum
/// prints `x - 2*y` rather than `x + 0xfffffffe*y`.  The caller writes the
/// sign.  `mkNeg` is multiplication by all-ones and `mkMul` folds the constant
/// factor back in, so both spellings intern to the same node.
std::optional<Printer::Magnitude> Printer::negatedProduct(SymRef R) const {
  llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
  if (Ops.size() < 2 || !Ctx.isConst(Ops[0]))
    return std::nullopt;
  llvm::APInt C = Ctx.constValue(Ops[0]);
  std::optional<Fragment> Coeff = negatedLiteral(C);
  if (!Coeff)
    return std::nullopt;

  Magnitude M;
  M.CoeffLed = !(-C).isOne();
  llvm::ArrayRef<SymRef> Factors = Ops.drop_front();
  std::string T;
  if (M.CoeffLed) {
    T = Coeff->Text;
    T += " * ";
  }
  // Division sits at the same strength as multiplication but does not
  // associate with it, so a quotient among the factors has to be bracketed or
  // the chain regroups: `d * (b / c)` must not flatten to `d * b / c`.
  T += join(Factors, " * ", PrecMultiplicative + 1);

  // A unit coefficient over a single factor contributes no operator of its
  // own, so the magnitude is that factor's text and keeps its strength — or is
  // primary, if the join had to bracket it.
  int P = PrecMultiplicative;
  if (!M.CoeffLed && Factors.size() == 1) {
    int FactorPrec = frag(Factors[0]).Prec;
    P = FactorPrec > PrecMultiplicative ? FactorPrec : PrecPrimary;
  }
  M.Frag = {std::move(T), P};
  return M;
}

/// Split a term of a sum, or a product standing on its own, into a sign and a
/// magnitude.  Nothing is returned when the term reads better as written.
std::optional<Printer::Magnitude> Printer::negatedTerm(SymRef R) const {
  if (Ctx.isConst(R)) {
    if (std::optional<Fragment> Mag = negatedLiteral(Ctx.constValue(R)))
      return Magnitude{std::move(*Mag), /*CoeffLed=*/true};
  } else if (Ctx.op(R) == SymOp::Mul) {
    return negatedProduct(R);
  }
  return std::nullopt;
}

/// Write \p M with its sign, in a position where the minus is unary.
///
/// A leading literal coefficient absorbs the sign — `-2 * x` is `(-2) * x`,
/// and multiplication is flattened, so that is the same node.  Without one the
/// minus governs the whole term and has to bind at least as tightly as it
/// does, or `-(e / a)` would come back as `(-e) / a`.
std::string Printer::negate(const Magnitude &M) const {
  return "-" + (M.CoeffLed ? M.Frag.Text : wrap(M.Frag, PrecUnary));
}

Fragment Printer::render(SymRef R) const {
  const SymNode &N = Ctx.node(R);
  llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);

  switch (N.Op) {
  case SymOp::Const: {
    llvm::APInt V = Ctx.constValue(R);
    if (std::optional<Fragment> Mag = negatedLiteral(V))
      return {"-" + Mag->Text, PrecUnary};
    return literal(V, N.Width);
  }
  case SymOp::Var: {
    const uint32_t Id = Ctx.varId(R);
    const SymVarInfo &Info = Ctx.varInfo(Id);
    const std::optional<uint32_t> Unique = Ctx.findVar(Info.Name);
    if (Unique && *Unique == Id && isTextIdentifier(Info.Name))
      return leaf(Info.Name, N.Width);
    return {"varid(" + std::to_string(Id) + ")", PrecPrimary};
  }

  case SymOp::Add: {
    unsigned Lead = print_detail::leadingAddOperand(Ctx, Ops);
    std::optional<Magnitude> First = negatedTerm(Ops[Lead]);
    std::string T =
        First ? negate(*First) : wrap(frag(Ops[Lead]), PrecAdditive);
    for (unsigned I = 0, E = Ops.size(); I < E; ++I) {
      if (I == Lead)
        continue;
      std::optional<Magnitude> Neg = negatedTerm(Ops[I]);
      T += Neg ? " - " : " + ";
      appendWrapped(T, Neg ? Neg->Frag : frag(Ops[I]), PrecAdditive + 1);
    }
    return {std::move(T), PrecAdditive};
  }
  case SymOp::Mul:
    if (std::optional<Magnitude> Neg = negatedProduct(R))
      return {negate(*Neg), Neg->CoeffLed ? PrecMultiplicative : PrecUnary};
    return {join(Ops, " * ", PrecMultiplicative + 1), PrecMultiplicative};

  case SymOp::And:
    return {join(Ops, " & ", PrecBitAnd), PrecBitAnd};
  case SymOp::Or:
    return {join(Ops, " | ", PrecBitOr), PrecBitOr};
  case SymOp::Xor:
    return {join(Ops, " ^ ", PrecBitXor), PrecBitXor};

  case SymOp::Not:
    // `~(a == b)` is what mkNe leaves behind, and `a != b` is both shorter and
    // exactly what the parser rebuilds.
    if (Ctx.op(Ops[0]) == SymOp::Eq) {
      llvm::ArrayRef<SymRef> Cmp = Ctx.operands(Ops[0]);
      return {sub(Cmp[0], PrecEquality) +
                  " != " + sub(Cmp[1], PrecEquality + 1),
              PrecEquality};
    }
    return {"~" + sub(Ops[0], PrecUnary), PrecUnary};

  case SymOp::Shl:
    return {sub(Ops[0], PrecShift) + " << " + sub(Ops[1], PrecShift + 1),
            PrecShift};
  case SymOp::LShr:
    return {sub(Ops[0], PrecShift) + " >> " + sub(Ops[1], PrecShift + 1),
            PrecShift};
  case SymOp::AShr:
    return {call("ashr", Ops), PrecPrimary};

  case SymOp::UDiv:
    return {sub(Ops[0], PrecMultiplicative) + " / " +
                sub(Ops[1], PrecMultiplicative + 1),
            PrecMultiplicative};
  case SymOp::URem:
    return {sub(Ops[0], PrecMultiplicative) + " % " +
                sub(Ops[1], PrecMultiplicative + 1),
            PrecMultiplicative};
  case SymOp::SDiv:
    return {call("sdiv", Ops), PrecPrimary};
  case SymOp::SRem:
    return {call("srem", Ops), PrecPrimary};
  case SymOp::Rol:
    return {call("rol", Ops), PrecPrimary};
  case SymOp::Ror:
    return {call("ror", Ops), PrecPrimary};

  case SymOp::Extract:
    return {"extract(" + arg(Ops[0]) + ", " + std::to_string(N.Aux) + ", " +
                std::to_string(N.Width) + ")",
            PrecPrimary};
  case SymOp::Concat:
    return {call("concat", Ops), PrecPrimary};
  case SymOp::ZExt:
    return {"zext(" + arg(Ops[0]) + ", " + std::to_string(N.Width) + ")",
            PrecPrimary};
  case SymOp::SExt:
    return {"sext(" + arg(Ops[0]) + ", " + std::to_string(N.Width) + ")",
            PrecPrimary};
  case SymOp::Ite:
    return {sub(Ops[0], PrecLogOr) + " ? " + sub(Ops[1], PrecTernary) + " : " +
                sub(Ops[2], PrecTernary),
            PrecTernary};

  case SymOp::Eq:
    return {sub(Ops[0], PrecEquality) + " == " + sub(Ops[1], PrecEquality + 1),
            PrecEquality};
  case SymOp::Ult:
    return {sub(Ops[0], PrecRelational) + " < " +
                sub(Ops[1], PrecRelational + 1),
            PrecRelational};
  case SymOp::Ule:
    return {sub(Ops[0], PrecRelational) +
                " <= " + sub(Ops[1], PrecRelational + 1),
            PrecRelational};
  case SymOp::Slt:
    return {call("slt", Ops), PrecPrimary};
  case SymOp::Sle:
    return {call("sle", Ops), PrecPrimary};
  }
  llvm_unreachable("unhandled SymOp in render");
}

std::string Printer::run(SymRef Root) {
  if (Ctx.operands(Root).empty())
    return render(Root).Text;

  // Visit operands before completing their parent. Interned nodes form a DAG,
  // so an already visited operand is complete, never an active ancestor.
  // Iterative postorder avoids both recursion on deep expressions and sorting
  // the reachable set, while rendering each shared node only once.
  llvm::SmallVector<uint32_t, 64> Order;
  llvm::SmallDenseSet<uint64_t,
                      limits::symbolic::kSymExprTraversalInlineBuckets>
      Seen;
  struct Frame {
    SymRef Ref;
    bool Expanded;
  };
  llvm::SmallVector<Frame, 64> Work{{Root, false}};
  while (!Work.empty()) {
    auto [R, Expanded] = Work.pop_back_val();
    if (Expanded) {
      Order.push_back(R.index());
      continue;
    }
    if (!Seen.insert(R.index()).second)
      continue;
    Work.push_back({R, true});
    for (SymRef C : Ctx.operands(R))
      Work.push_back({C, false});
  }

  Memo.reserve(Order.size());
  for (uint32_t I : Order)
    Memo.try_emplace(I, render(SymRef(I)));
  return std::move(Memo.find(Root.index())->second.Text);
}

} // namespace

std::string SymContext::toString(SymRef R) const {
  if (!R.isValid())
    return "<invalid>";
  return Printer(*this, width(R)).run(R);
}

} // namespace neverd::symbolic
