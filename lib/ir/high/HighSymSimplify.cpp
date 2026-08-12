//===- HighSymSimplify.cpp - Semantic simplification of HighIR ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runs the symbolic engine over the expressions a function was decompiled
/// into, and puts back whatever came out shorter.
///
/// The peephole pass next door rewrites what it can recognise by shape.  That
/// is enough for the residue of ordinary lifting and useless against anything
/// deliberate: an expression mixing `+ - *` with `& | ^ ~` blocks every rule
/// either algebra can state, which is the entire point of writing one.  This
/// pass does not look at the shape at all.  It measures what the expression
/// computes and writes the shortest thing that computes the same.
///
/// Translation in both directions is deliberately narrow.  Only the operators
/// that are bitvector arithmetic on a whole word are carried across; a load, a
/// call, a cast, a comparison, anything of a width the engine cannot see —
/// each becomes one opaque input, and comes back untouched.  Everything the
/// pass does not understand it therefore preserves exactly, and the part it
/// does understand it reasons about exactly.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"

#include "neverd/symbolic/SymMBA.h"

#include "llvm/ADT/SmallVector.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace neverd {

namespace {

namespace sym = neverd::symbolic;

/// Expressions smaller than this are left alone.  Nothing that short can be
/// hiding anything, and translating it would cost more than the answer.
constexpr size_t kMinInterestingNodes = 6;

/// How much a rewrite has to save before it is worth making.
///
/// Not a tuning knob so much as a statement about what this pass is for.
/// Obfuscation loses enormously when it is measured — nine down to three,
/// fifteen down to three — so nothing is given up by ignoring a rewrite that
/// saves one.  What is gained is that the expressions this pass has no real
/// business touching keep the shape the rest of the pipeline built: their type
/// annotations, and the shared subterms the backend lifts into named
/// temporaries, both of which a reshuffle can cost more than the saving.
constexpr size_t kMinGain = 3;

/// Width of \p E in bits, or zero when there is no saying what it is.
///
/// HighIR records a width in two places and neither is always filled in: a
/// type, which inference reaches only some expressions, and the size a
/// variable was lifted at, which every variable carries.  An operator has
/// whichever its operands have.  Reading all three is what lets this pass work
/// on the expressions type inference did not annotate, which is most of them.
///
/// Sizes are in bytes throughout, and a boolean is stored in one of them —
/// indistinguishable here from an eight-bit integer, which is why a comparison
/// never reaches the translation below.
uint32_t bitWidthOf(const ExprPtr &E) {
  if (!E)
    return 0;

  uint16_t Bytes = E->Type ? E->Type->Size : 0;
  if (Bytes == 0 && E->Kind == ExprKind::Var)
    Bytes = E->Var.Size;
  if (Bytes == 0 && !E->Operands.empty() &&
      (E->Kind == ExprKind::BinOp || E->Kind == ExprKind::UnaryOp))
    return bitWidthOf(E->Operands[0]);

  if (Bytes == 0 || Bytes > 8)
    return 0;
  return static_cast<uint32_t>(Bytes) * 8;
}

/// The engine operator a binary HighIR operator stands for, if any.
///
/// Comparisons are absent on purpose.  They produce one bit, HighIR stores
/// that in a byte, and the two are indistinguishable here from an eight-bit
/// integer — so a predicate is carried across as an opaque input rather than
/// risk being measured at the wrong width.
enum class BinKind {
  None,
  Add,
  Sub,
  Mul,
  And,
  Or,
  Xor,
  Shl,
  LShr,
  AShr,
  UDiv,
  SDiv,
  URem,
  SRem
};

BinKind binKindOf(NdOp Op) {
  switch (Op) {
  case NdOp::INT_ADD:
    return BinKind::Add;
  case NdOp::INT_SUB:
    return BinKind::Sub;
  case NdOp::INT_MULT:
    return BinKind::Mul;
  case NdOp::INT_AND:
    return BinKind::And;
  case NdOp::INT_OR:
    return BinKind::Or;
  case NdOp::INT_XOR:
    return BinKind::Xor;
  case NdOp::INT_LEFT:
    return BinKind::Shl;
  case NdOp::INT_RIGHT:
    return BinKind::LShr;
  case NdOp::INT_ASHR:
    return BinKind::AShr;
  case NdOp::INT_DIV:
    return BinKind::UDiv;
  case NdOp::INT_SDIV:
    return BinKind::SDiv;
  case NdOp::INT_REM:
    return BinKind::URem;
  case NdOp::INT_SREM:
    return BinKind::SRem;
  default:
    return BinKind::None;
  }
}

//===----------------------------------------------------------------------===//
// HighIR to the engine
//===----------------------------------------------------------------------===//

class Translator {
public:
  explicit Translator(sym::SymContext &Ctx) : Ctx(Ctx) {}

  sym::SymRef in(const ExprPtr &E);
  /// Rebuild a HighIR expression, or nothing when the result holds an operator
  /// with no HighIR spelling.  Unreachable as things stand — every candidate
  /// the solver builds is made of operators that came from here — but the
  /// alternative to checking is emitting something wrong.
  ExprPtr out(sym::SymRef R, uint32_t Width);

  bool sawAnything() const { return !Sources.empty(); }

private:
  /// Stand an opaque input in front of \p E, so what surrounds it can still be
  /// measured.  One per node, so a subterm the obfuscator repeated stays the
  /// same input on both sides and cancels.
  sym::SymRef opaque(const ExprPtr &E, uint32_t Width) {
    std::string Name = "nd$" + std::to_string(Sources.size());
    sym::SymRef V = Ctx.mkVar(Name, Width);
    Sources.emplace(V.index(), E);
    return V;
  }

  /// An operand read at the width its operator works at, or nothing when
  /// there is no reading of it that is certainly right.
  ///
  /// HighIR sizes a literal by the machine operand it was lifted from, which
  /// is routinely narrower than the expression around it — `x * 2` holds a
  /// 64-bit `x` beside a 32-bit `2`.  Widening the literal is exact as long as
  /// its stored top bit is clear, because zero- and sign-extension then agree
  /// and there is nothing left to guess.  Anything else — a wider literal, or
  /// a non-literal of the wrong width — is a conversion HighIR left implicit,
  /// and picking one of its meanings is how a rewrite silently becomes wrong.
  std::optional<sym::SymRef> operandAt(const ExprPtr &E, uint32_t Width);

  sym::SymRef applyBinary(BinKind Kind, sym::SymRef A, sym::SymRef B);

  sym::SymContext &Ctx;
  std::unordered_map<const HighExpr *, sym::SymRef> Memo;
  /// Engine variable node to the HighIR it stands for, for the way back.
  std::unordered_map<uint32_t, ExprPtr> Sources;
};

std::optional<sym::SymRef> Translator::operandAt(const ExprPtr &E,
                                                 uint32_t Width) {
  const uint32_t Have = bitWidthOf(E);
  if (Have == Width)
    return in(E);
  if (E->Kind != ExprKind::Const || Have == 0 || Have >= Width)
    return std::nullopt;
  if (E->ConstVal >= (uint64_t(1) << (Have - 1)))
    return std::nullopt;
  return Ctx.mkConst(Width, E->ConstVal);
}

sym::SymRef Translator::applyBinary(BinKind Kind, sym::SymRef A,
                                    sym::SymRef B) {
  switch (Kind) {
  case BinKind::Add:
    return Ctx.mkAdd(A, B);
  case BinKind::Sub:
    return Ctx.mkSub(A, B);
  case BinKind::Mul:
    return Ctx.mkMul(A, B);
  case BinKind::And:
    return Ctx.mkAnd(A, B);
  case BinKind::Or:
    return Ctx.mkOr(A, B);
  case BinKind::Xor:
    return Ctx.mkXor(A, B);
  case BinKind::Shl:
    return Ctx.mkShl(A, B);
  case BinKind::LShr:
    return Ctx.mkLShr(A, B);
  case BinKind::AShr:
    return Ctx.mkAShr(A, B);
  case BinKind::UDiv:
    return Ctx.mkUDiv(A, B);
  case BinKind::SDiv:
    return Ctx.mkSDiv(A, B);
  case BinKind::URem:
    return Ctx.mkURem(A, B);
  case BinKind::SRem:
    return Ctx.mkSRem(A, B);
  case BinKind::None:
    break;
  }
  llvm_unreachable("binKindOf returned an operator applyBinary does not know");
}

sym::SymRef Translator::in(const ExprPtr &E) {
  auto Cached = Memo.find(E.get());
  if (Cached != Memo.end())
    return Cached->second;

  const uint32_t Width = bitWidthOf(E);
  sym::SymRef Result;

  // A node with no usable width cannot even be an input, because the engine
  // has nothing to give the placeholder.  Fall back to the widest word, which
  // keeps it opaque and keeps its identity.
  if (Width == 0) {
    Result = opaque(E, 64);
    Memo.emplace(E.get(), Result);
    return Result;
  }

  switch (E->Kind) {
  case ExprKind::Const:
    Result = Ctx.mkConst(Width, E->ConstVal);
    break;

  case ExprKind::Var:
    // Named by SSA identity, so every mention of one value is one input.
    Result = Ctx.mkVar("v" + std::to_string(E->Var.Id) + "_" +
                           std::to_string(E->Var.SSAVer),
                       Width);
    // Recorded like an opaque input, because the way back cannot tell the two
    // apart: both are engine variables that have to become HighIR again.
    Sources.emplace(Result.index(), E);
    break;

  case ExprKind::BinOp: {
    BinKind Kind = binKindOf(E->Op);
    if (Kind == BinKind::None || E->Operands.size() != 2) {
      Result = opaque(E, Width);
      break;
    }
    std::optional<sym::SymRef> Lhs = operandAt(E->Operands[0], Width);
    std::optional<sym::SymRef> Rhs = operandAt(E->Operands[1], Width);
    if (!Lhs || !Rhs) {
      Result = opaque(E, Width);
      break;
    }
    Result = applyBinary(Kind, *Lhs, *Rhs);
    break;
  }

  case ExprKind::Cast:
    // A literal wearing a cast is still a literal, and a very common one:
    // leaving `(int32_t)0` opaque costs the engine every fold that depends on
    // knowing a term is zero.  Anything else a cast wraps stays opaque, since
    // what a narrowing or widening means is exactly what this pass declines to
    // guess at.
    if (E->Operands.size() == 1 && E->Operands[0]->Kind == ExprKind::Const) {
      Result = Ctx.mkConst(Width, E->Operands[0]->ConstVal);
      break;
    }
    Result = opaque(E, Width);
    break;

  case ExprKind::UnaryOp: {
    bool Complement = E->Op == NdOp::INT_NOT || E->Op == NdOp::INT_NEGATE;
    bool Negate = E->Op == NdOp::INT_NEG2;
    if ((!Complement && !Negate) || E->Operands.size() != 1 ||
        bitWidthOf(E->Operands[0]) != Width) {
      Result = opaque(E, Width);
      break;
    }
    sym::SymRef Operand = in(E->Operands[0]);
    Result = Complement ? Ctx.mkNot(Operand) : Ctx.mkNeg(Operand);
    break;
  }

  default:
    Result = opaque(E, Width);
    break;
  }

  Memo.emplace(E.get(), Result);
  return Result;
}

//===----------------------------------------------------------------------===//
// The engine back to HighIR
//===----------------------------------------------------------------------===//

ExprPtr Translator::out(sym::SymRef R, uint32_t Width) {
  const uint32_t NodeWidth = Ctx.width(R);
  const auto ByteSize = static_cast<uint16_t>(NodeWidth / 8);
  if (NodeWidth == 0 || NodeWidth % 8 != 0)
    return nullptr;

  auto binop = [&](NdOp Op, llvm::ArrayRef<sym::SymRef> Ops) -> ExprPtr {
    ExprPtr Acc = out(Ops[0], NodeWidth);
    for (size_t I = 1; I < Ops.size() && Acc; ++I) {
      ExprPtr Rhs = out(Ops[I], NodeWidth);
      Acc = Rhs ? HighExpr::makeBinop(Op, Acc, Rhs) : nullptr;
    }
    return Acc;
  };

  switch (Ctx.op(R)) {
  case sym::SymOp::Const:
    return HighExpr::makeConst(Ctx.constValue(R).getZExtValue(), ByteSize);

  case sym::SymOp::Var: {
    auto It = Sources.find(R.index());
    // Every variable in the result came from the way in, so a miss would mean
    // the solver invented one — which only its placeholders are, and those are
    // substituted away before it returns.
    return It == Sources.end() ? nullptr : It->second;
  }

  case sym::SymOp::Add: {
    // A sum whose term carries a negative coefficient reads far better as a
    // subtraction, and the C backend has no other way to be told so.
    llvm::SmallVector<sym::SymRef, 8> Plus, Minus;
    for (sym::SymRef Term : Ctx.operands(R)) {
      if (Ctx.op(Term) == sym::SymOp::Mul) {
        llvm::ArrayRef<sym::SymRef> Factors = Ctx.operands(Term);
        if (Factors.size() == 2 && Ctx.isConst(Factors[0]) &&
            Ctx.constValue(Factors[0]).isAllOnes()) {
          Minus.push_back(Factors[1]);
          continue;
        }
      }
      Plus.push_back(Term);
    }
    if (Plus.empty())
      Plus.push_back(Ctx.mkZero(NodeWidth));

    ExprPtr Acc = binop(NdOp::INT_ADD, Plus);
    for (sym::SymRef Term : Minus) {
      if (!Acc)
        break;
      ExprPtr Rhs = out(Term, NodeWidth);
      Acc = Rhs ? HighExpr::makeBinop(NdOp::INT_SUB, Acc, Rhs) : nullptr;
    }
    return Acc;
  }

  case sym::SymOp::Mul: {
    // The engine stores negation as a product with all-ones.  Emitting that
    // literally gives `-1 * x`, where every reader wants `-x`.
    llvm::ArrayRef<sym::SymRef> Factors = Ctx.operands(R);
    if (Factors.size() == 2 && Ctx.isConst(Factors[0]) &&
        Ctx.constValue(Factors[0]).isAllOnes()) {
      ExprPtr Operand = out(Factors[1], NodeWidth);
      return Operand ? HighExpr::makeUnary(NdOp::INT_NEG2, Operand) : nullptr;
    }
    return binop(NdOp::INT_MULT, Factors);
  }
  case sym::SymOp::And:
    return binop(NdOp::INT_AND, Ctx.operands(R));
  case sym::SymOp::Or:
    return binop(NdOp::INT_OR, Ctx.operands(R));
  case sym::SymOp::Xor:
    return binop(NdOp::INT_XOR, Ctx.operands(R));

  case sym::SymOp::Not: {
    ExprPtr Operand = out(Ctx.operand(R, 0), NodeWidth);
    return Operand ? HighExpr::makeUnary(NdOp::INT_NOT, Operand) : nullptr;
  }

  case sym::SymOp::Shl:
    return binop(NdOp::INT_LEFT, Ctx.operands(R));
  case sym::SymOp::LShr:
    return binop(NdOp::INT_RIGHT, Ctx.operands(R));
  case sym::SymOp::AShr:
    return binop(NdOp::INT_ASHR, Ctx.operands(R));
  case sym::SymOp::UDiv:
    return binop(NdOp::INT_DIV, Ctx.operands(R));
  case sym::SymOp::SDiv:
    return binop(NdOp::INT_SDIV, Ctx.operands(R));
  case sym::SymOp::URem:
    return binop(NdOp::INT_REM, Ctx.operands(R));
  case sym::SymOp::SRem:
    return binop(NdOp::INT_SREM, Ctx.operands(R));

  default:
    // Extract, Concat, the casts, the select and the predicates never come
    // back out, because nothing on the way in ever puts one in.
    return nullptr;
  }
}

/// Simplify one expression, in place, if there is anything to gain.
void simplifyOne(ExprPtr &E) {
  const uint32_t Width = bitWidthOf(E);
  if (Width == 0)
    return;

  sym::SymContext Ctx;
  Translator Xlat(Ctx);
  sym::SymRef Before = Xlat.in(E);
  if (Ctx.dagSize(Before) < kMinInterestingNodes)
    return;

  sym::MBAResult Result = sym::simplifyMBADeep(Ctx, Before);
  // Shorter by enough to be worth it.  The engine settles a tie towards its
  // own canonical form, which is the right answer to what an expression *is*;
  // the question here is what to show someone, and a rewrite that saves
  // nothing only churns the output.
  if (!Result.Changed || Result.SizeBefore < Result.SizeAfter + kMinGain)
    return;

  ExprPtr After = Xlat.out(Result.Expr, Width);
  if (!After)
    return;
  // The engine measured a width, not a type.  Keep the one the expression
  // already carried, which type inference has more to say about than this pass
  // does.
  After->Type = E->Type;
  E = After;
}

} // namespace

void simplifyExprSemantics(std::vector<HighStmt> &Stmts) {
  walkStmts(Stmts,
            [](HighStmt &S) { forEachRhsExpr(S, [](ExprPtr &E) { simplifyOne(E); }); });
}

} // namespace neverd
