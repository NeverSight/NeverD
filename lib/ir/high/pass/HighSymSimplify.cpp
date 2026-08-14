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

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace neverd {

namespace {

namespace sym = neverd::symbolic;

/// A leaf or one already-canonical operator cannot become shorter.  Four nodes
/// is the first useful case: `~x + 1` is four and simplifies to `-x`.
constexpr size_t kMinInterestingNodes = 4;

/// How much a rewrite has to save before it is worth making.
///
/// The cost is the rendered tree rather than the shared graph, so one unit is a
/// real operator removed from the decompiled expression.  Requiring a strict
/// improvement prevents canonical-form churn without hiding compact identities
/// such as `~x + 1`.
constexpr size_t kMinGain = 1;

/// Widest expression this pass hands the engine, in bits.
///
/// The engine itself has no width ceiling — its literals are arbitrary
/// precision, and a 256-bit word is measured exactly like a 32-bit one — so
/// the only reason to have one here is that HighIR records a size for every
/// type, including a struct and an array.  An aggregate is not a machine word,
/// and measuring one would allocate values of its size at every corner to
/// learn nothing.  Past the widest word a machine or a virtual machine
/// actually has, a size is therefore read the way an unreadable one already
/// is: the expression is preserved rather than measured.
constexpr uint32_t kMaxWidth = 1024;

/// The width a HighIR size stands for, or zero when it is not one to measure.
uint32_t widthFromBytes(uint16_t Bytes) {
  const auto Bits = static_cast<uint32_t>(Bytes) * 8;
  return Bits <= kMaxWidth ? Bits : 0;
}

/// The value a HighIR literal certainly denotes at \p Width, or nothing when
/// no reading of it is certainly right.  \p Have is the width the literal
/// itself was lifted at.
///
/// A literal keeps its value in a 64-bit field beside a size, and neither has
/// to agree with the expression around it.  Reading it narrower is exact — the
/// operator would take the value modulo its own width anyway.  Reading it
/// wider is exact only while zero- and sign-extension agree, which is what a
/// clear top bit says; anything else is a conversion HighIR left implicit, and
/// picking one of its meanings is how a rewrite silently becomes wrong.
///
/// Past 64 bits the field is itself narrower than the size written beside it,
/// so the same question is asked of bit 63.  There is no room in the record
/// for what lies above it, and a literal filling the field may equally be a
/// wider one that did not fit — which is why a wide all-ones stays opaque
/// rather than being taken for -1.
std::optional<llvm::APInt> literalAt(const HighExpr &E, uint32_t Have,
                                     uint32_t Width) {
  const uint32_t Held = Have == 0 || Have > 64 ? 64 : Have;
  if (Width <= Held)
    return llvm::APInt(Width, E.ConstVal, /*isSigned=*/false,
                       /*implicitTrunc=*/true);
  if ((E.ConstVal >> (Held - 1)) != 0)
    return std::nullopt;
  return llvm::APInt(Width, E.ConstVal);
}

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
  llvm::SmallPtrSet<const HighExpr *, 8> Seen;
  const HighExpr *Current = E.get();
  while (Current) {
    if (!Seen.insert(Current).second)
      return 0;

    uint16_t Bytes = Current->Type ? Current->Type->Size : 0;
    if (Bytes == 0 && Current->Kind == ExprKind::Var)
      Bytes = Current->Var.Size;
    if (Bytes != 0)
      return widthFromBytes(Bytes);

    if (Current->Operands.empty() || (Current->Kind != ExprKind::BinOp &&
                                      Current->Kind != ExprKind::UnaryOp))
      return 0;
    Current = Current->Operands[0].get();
  }
  return 0;
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
  /// 64-bit `x` beside a 32-bit `2` — so a literal is widened when \c
  /// literalAt says the widening is exact.  Anything else — a wider literal,
  /// or a non-literal of the wrong width — is a conversion HighIR left
  /// implicit, and picking one of its meanings is how a rewrite silently
  /// becomes wrong.
  std::optional<sym::SymRef> operandAt(const ExprPtr &E, uint32_t Width);

  uint32_t widthOf(const ExprPtr &E) {
    auto Cached = Widths.find(E.get());
    if (Cached != Widths.end())
      return Cached->second;

    llvm::SmallVector<const HighExpr *, 8> Path;
    llvm::SmallPtrSet<const HighExpr *, 8> Seen;
    const HighExpr *Current = E.get();
    uint32_t Width = 0;
    while (Current) {
      Cached = Widths.find(Current);
      if (Cached != Widths.end()) {
        Width = Cached->second;
        break;
      }
      if (!Seen.insert(Current).second)
        break;
      Path.push_back(Current);

      uint16_t Bytes = Current->Type ? Current->Type->Size : 0;
      if (Bytes == 0 && Current->Kind == ExprKind::Var)
        Bytes = Current->Var.Size;
      if (Bytes != 0) {
        Width = widthFromBytes(Bytes);
        break;
      }
      if (Current->Operands.empty() || (Current->Kind != ExprKind::BinOp &&
                                        Current->Kind != ExprKind::UnaryOp))
        break;
      Current = Current->Operands[0].get();
    }
    for (const HighExpr *Node : Path)
      Widths.emplace(Node, Width);
    return Width;
  }

  sym::SymRef applyBinary(BinKind Kind, sym::SymRef A, sym::SymRef B);

  sym::SymContext &Ctx;
  std::unordered_map<const HighExpr *, sym::SymRef> Memo;
  std::unordered_map<const HighExpr *, uint32_t> Widths;
  /// Engine variable node to the HighIR it stands for, for the way back.
  std::unordered_map<uint32_t, ExprPtr> Sources;
};

std::optional<sym::SymRef> Translator::operandAt(const ExprPtr &E,
                                                 uint32_t Width) {
  const uint32_t Have = widthOf(E);
  if (Have == Width)
    return in(E);
  if (E->Kind != ExprKind::Const || Have == 0 || Have >= Width)
    return std::nullopt;
  std::optional<llvm::APInt> Val = literalAt(*E, Have, Width);
  if (!Val)
    return std::nullopt;
  return Ctx.mkConst(*Val);
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

  // HighIR copy propagation can turn thousands of straight-line assignments
  // into one expression.  Walk that DAG explicitly: a fixed recursion cutoff
  // avoids a stack overflow but arbitrarily hides every identity below it.
  // The symbolic engine's own deep simplifier is iterative for the same reason.
  struct WorkItem {
    ExprPtr Expr;
    bool ChildrenReady = false;
  };
  llvm::SmallVector<WorkItem, 64> Work{{E, false}};
  std::unordered_set<const HighExpr *> Active;

  while (!Work.empty()) {
    WorkItem Item = std::move(Work.back());
    Work.pop_back();
    const ExprPtr &Current = Item.Expr;

    Cached = Memo.find(Current.get());
    if (Cached != Memo.end()) {
      if (Item.ChildrenReady)
        Active.erase(Current.get());
      continue;
    }

    const uint32_t Width = widthOf(Current);
    if (!Item.ChildrenReady) {
      // HighIR is a DAG.  If malformed input nevertheless closes a cycle,
      // preserve that node opaquely rather than spinning the worklist forever.
      if (!Active.insert(Current.get()).second) {
        Memo.emplace(Current.get(), opaque(Current, Width ? Width : 64));
        continue;
      }

      Work.push_back({Current, true});
      llvm::SmallVector<ExprPtr, 2> Dependencies;
      if (Width != 0 && Current->Kind == ExprKind::BinOp &&
          binKindOf(Current->Op) != BinKind::None &&
          Current->Operands.size() == 2) {
        for (const ExprPtr &Operand : Current->Operands)
          if (widthOf(Operand) == Width)
            Dependencies.push_back(Operand);
      } else if (Width != 0 && Current->Kind == ExprKind::UnaryOp &&
                 (Current->Op == NdOp::INT_NOT ||
                  Current->Op == NdOp::INT_NEGATE ||
                  Current->Op == NdOp::INT_NEG2) &&
                 Current->Operands.size() == 1 &&
                 widthOf(Current->Operands[0]) == Width) {
        Dependencies.push_back(Current->Operands[0]);
      }
      for (auto It = Dependencies.rbegin(); It != Dependencies.rend(); ++It)
        if (Memo.find(It->get()) == Memo.end())
          Work.push_back({*It, false});
      continue;
    }

    Active.erase(Current.get());
    sym::SymRef Result;
    // A node with no usable width cannot even be an input, because the engine
    // has nothing to give the placeholder.  Fall back to the widest word,
    // which keeps it opaque and keeps its identity.
    if (Width == 0) {
      Result = opaque(Current, 64);
      Memo.emplace(Current.get(), Result);
      continue;
    }

    switch (Current->Kind) {
    case ExprKind::Const: {
      std::optional<llvm::APInt> Val = literalAt(*Current, Width, Width);
      Result = Val ? Ctx.mkConst(*Val) : opaque(Current, Width);
      break;
    }

    case ExprKind::Var:
      // Named by SSA identity, so every mention of one value is one input.
      Result = Ctx.mkVar("v" + std::to_string(Current->Var.Id) + "_" +
                             std::to_string(Current->Var.SSAVer),
                         Width);
      // Recorded like an opaque input, because the way back cannot tell the two
      // apart: both are engine variables that have to become HighIR again.
      Sources.emplace(Result.index(), Current);
      break;

    case ExprKind::BinOp: {
      BinKind Kind = binKindOf(Current->Op);
      if (Kind == BinKind::None || Current->Operands.size() != 2) {
        Result = opaque(Current, Width);
        break;
      }
      std::optional<sym::SymRef> Lhs = operandAt(Current->Operands[0], Width);
      std::optional<sym::SymRef> Rhs = operandAt(Current->Operands[1], Width);
      if (!Lhs || !Rhs) {
        Result = opaque(Current, Width);
        break;
      }
      Result = applyBinary(Kind, *Lhs, *Rhs);
      break;
    }

    case ExprKind::Cast: {
      // A literal wearing a cast is still a literal, and a very common one:
      // leaving `(int32_t)0` opaque costs the engine every fold that depends on
      // knowing a term is zero.  Anything else a cast wraps stays opaque, since
      // what a narrowing or widening means is exactly what this pass declines
      // to guess at.
      std::optional<llvm::APInt> Val;
      if (Current->Operands.size() == 1 &&
          Current->Operands[0]->Kind == ExprKind::Const)
        Val = literalAt(*Current->Operands[0], Width, Width);
      Result = Val ? Ctx.mkConst(*Val) : opaque(Current, Width);
      break;
    }

    case ExprKind::UnaryOp: {
      bool Complement =
          Current->Op == NdOp::INT_NOT || Current->Op == NdOp::INT_NEGATE;
      bool Negate = Current->Op == NdOp::INT_NEG2;
      if ((!Complement && !Negate) || Current->Operands.size() != 1 ||
          widthOf(Current->Operands[0]) != Width) {
        Result = opaque(Current, Width);
        break;
      }
      sym::SymRef Operand = in(Current->Operands[0]);
      Result = Complement ? Ctx.mkNot(Operand) : Ctx.mkNeg(Operand);
      break;
    }

    default:
      Result = opaque(Current, Width);
      break;
    }

    Memo.emplace(Current.get(), Result);
  }

  Cached = Memo.find(E.get());
  assert(Cached != Memo.end() && "iterative HighIR translation lost its root");
  return Cached->second;
}

//===----------------------------------------------------------------------===//
// The engine back to HighIR
//===----------------------------------------------------------------------===//

/// The HighIR literal denoting \p Val at \p Bytes bytes, or nothing when
/// HighIR has no way to say it.
///
/// A literal keeps its value in a sixty-four bit field whatever size stands
/// beside it, so a wider value the engine derived has to be spelled some other
/// way or not at all.  Writing the low half down would be a shorter expression
/// computing something else, which is the one thing a rewrite may never be.
///
/// Negation and complement are the two other spellings, and between them they
/// cover what measuring a wide word actually produces: a small negative and a
/// high run of ones each leave a magnitude that fits.  Negation is tried first
/// because `-1` is what a reader expects where `~0` would also do.
ExprPtr literalExpr(const llvm::APInt &Val, uint16_t Bytes) {
  if (std::optional<uint64_t> Direct = Val.tryZExtValue())
    return HighExpr::makeConst(*Direct, Bytes);
  if (std::optional<uint64_t> Magnitude = (-Val).tryZExtValue())
    return HighExpr::makeUnary(NdOp::INT_NEG2,
                               HighExpr::makeConst(*Magnitude, Bytes));
  if (std::optional<uint64_t> Complement = (~Val).tryZExtValue())
    return HighExpr::makeUnary(NdOp::INT_NOT,
                               HighExpr::makeConst(*Complement, Bytes));
  return nullptr;
}

ExprPtr Translator::out(sym::SymRef R, uint32_t /*Width*/) {
  struct WorkItem {
    sym::SymRef Ref;
    bool ChildrenReady = false;
  };
  llvm::SmallVector<WorkItem, 64> Work{{R, false}};
  std::unordered_map<uint32_t, ExprPtr> Built;
  std::unordered_set<uint32_t> Active;

  while (!Work.empty()) {
    WorkItem Item = Work.pop_back_val();
    const uint32_t Index = Item.Ref.index();
    if (Built.find(Index) != Built.end()) {
      if (Item.ChildrenReady)
        Active.erase(Index);
      continue;
    }

    if (!Item.ChildrenReady) {
      if (!Active.insert(Index).second) {
        Built.emplace(Index, nullptr);
        continue;
      }
      Work.push_back({Item.Ref, true});
      llvm::ArrayRef<sym::SymRef> Operands = Ctx.operands(Item.Ref);
      for (auto It = Operands.rbegin(); It != Operands.rend(); ++It)
        if (Built.find(It->index()) == Built.end())
          Work.push_back({*It, false});
      continue;
    }

    Active.erase(Index);
    const uint32_t NodeWidth = Ctx.width(Item.Ref);
    if (NodeWidth == 0 || NodeWidth % 8 != 0) {
      Built.emplace(Index, nullptr);
      continue;
    }
    const auto ByteSize = static_cast<uint16_t>(NodeWidth / 8);

    auto get = [&](sym::SymRef Child) -> ExprPtr {
      auto It = Built.find(Child.index());
      assert(It != Built.end() && "iterative HighIR rebuild lost an operand");
      return It->second;
    };
    auto binop = [&](NdOp Op, llvm::ArrayRef<sym::SymRef> Operands) -> ExprPtr {
      if (Operands.empty())
        return nullptr;
      ExprPtr Acc = get(Operands[0]);
      for (size_t I = 1; I < Operands.size() && Acc; ++I) {
        ExprPtr Rhs = get(Operands[I]);
        Acc = Rhs ? HighExpr::makeBinop(Op, Acc, Rhs) : nullptr;
      }
      return Acc;
    };

    ExprPtr Result;
    switch (Ctx.op(Item.Ref)) {
    case sym::SymOp::Const:
      // Null when the value is one HighIR cannot write at all, which abandons
      // the rewrite rather than recording a truncation of it.
      Result = literalExpr(Ctx.constValue(Item.Ref), ByteSize);
      break;

    case sym::SymOp::Var: {
      auto It = Sources.find(Index);
      // Every variable in the result came from the way in, so a miss would mean
      // the solver invented one — which only its placeholders are, and those
      // are substituted away before it returns.
      Result = It == Sources.end() ? nullptr : It->second;
      break;
    }

    case sym::SymOp::Add: {
      // A sum whose term carries a negative coefficient reads far better as a
      // subtraction, and the C backend has no other way to be told so.
      llvm::SmallVector<sym::SymRef, 8> Plus, Minus;
      for (sym::SymRef Term : Ctx.operands(Item.Ref)) {
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

      ExprPtr Acc = Plus.empty() ? HighExpr::makeConst(0, ByteSize)
                                 : binop(NdOp::INT_ADD, Plus);
      for (sym::SymRef Term : Minus) {
        if (!Acc)
          break;
        ExprPtr Rhs = get(Term);
        Acc = Rhs ? HighExpr::makeBinop(NdOp::INT_SUB, Acc, Rhs) : nullptr;
      }
      Result = Acc;
      break;
    }

    case sym::SymOp::Mul: {
      // The engine stores negation as a product with all-ones.  Emitting that
      // literally gives `-1 * x`, where every reader wants `-x`.
      llvm::ArrayRef<sym::SymRef> Factors = Ctx.operands(Item.Ref);
      if (Factors.size() == 2 && Ctx.isConst(Factors[0]) &&
          Ctx.constValue(Factors[0]).isAllOnes()) {
        ExprPtr Operand = get(Factors[1]);
        Result =
            Operand ? HighExpr::makeUnary(NdOp::INT_NEG2, Operand) : nullptr;
      } else {
        Result = binop(NdOp::INT_MULT, Factors);
      }
      break;
    }
    case sym::SymOp::And:
      Result = binop(NdOp::INT_AND, Ctx.operands(Item.Ref));
      break;
    case sym::SymOp::Or:
      Result = binop(NdOp::INT_OR, Ctx.operands(Item.Ref));
      break;
    case sym::SymOp::Xor:
      Result = binop(NdOp::INT_XOR, Ctx.operands(Item.Ref));
      break;

    case sym::SymOp::Not: {
      ExprPtr Operand = get(Ctx.operand(Item.Ref, 0));
      Result = Operand ? HighExpr::makeUnary(NdOp::INT_NOT, Operand) : nullptr;
      break;
    }

    case sym::SymOp::Shl:
      Result = binop(NdOp::INT_LEFT, Ctx.operands(Item.Ref));
      break;
    case sym::SymOp::LShr:
      Result = binop(NdOp::INT_RIGHT, Ctx.operands(Item.Ref));
      break;
    case sym::SymOp::AShr:
      Result = binop(NdOp::INT_ASHR, Ctx.operands(Item.Ref));
      break;
    case sym::SymOp::UDiv:
      Result = binop(NdOp::INT_DIV, Ctx.operands(Item.Ref));
      break;
    case sym::SymOp::SDiv:
      Result = binop(NdOp::INT_SDIV, Ctx.operands(Item.Ref));
      break;
    case sym::SymOp::URem:
      Result = binop(NdOp::INT_REM, Ctx.operands(Item.Ref));
      break;
    case sym::SymOp::SRem:
      Result = binop(NdOp::INT_SREM, Ctx.operands(Item.Ref));
      break;

    default:
      // Extract, Concat, the casts, the select and the predicates never come
      // back out, because nothing on the way in ever puts one in.
      Result = nullptr;
      break;
    }
    Built.emplace(Index, std::move(Result));
  }

  auto It = Built.find(R.index());
  assert(It != Built.end() && "iterative HighIR rebuild lost its root");
  return It->second;
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

  // The iterative walk has no nesting cutoff.  Its default budget applies only
  // to exponential corner measurements and product search, keeping automatic
  // decompilation bounded while the public API still offers an explicit
  // exhaustive policy for trusted inputs.
  sym::MBAResult Result = sym::simplifyMBADeep(Ctx, Before);
  // Shorter by enough to be worth it.  The engine settles a tie towards its
  // own canonical form, which is the right answer to what an expression *is*;
  // the question here is what to show someone, and a rewrite that saves
  // nothing only churns the output.
  if (!Result.Changed || Result.Evidence != sym::MBAEvidence::Derivation ||
      Result.SizeBefore < Result.SizeAfter + kMinGain)
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
  walkStmts(Stmts, [](HighStmt &S) {
    forEachRhsExpr(S, [](ExprPtr &E) { simplifyOne(E); });
  });
}

} // namespace neverd
