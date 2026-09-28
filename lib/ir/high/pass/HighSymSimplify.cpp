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
/// general MBA pass measures what the expression computes and writes the
/// shortest thing that computes the same. A separate proof-gated search
/// reconstructs arithmetic spanning two machine words.
///
/// Translation in both directions is deliberately narrow. Integer arithmetic,
/// exact word concatenation, and byte-valued integer predicates with known
/// operand widths are carried across. A load, a call, or an ambiguous width
/// becomes an opaque input and comes back untouched. The result only uses
/// operators the source IR can spell, so unknown semantics remain in place.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymWideArithmetic.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
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
  // Address identities cannot be round-tripped through an unannotated numeric
  // symbolic literal. Keep them opaque, including fragments and owned values.
  if ((E.ConstProvenance != ConstantAddressProvenance::Unknown &&
       E.ConstProvenance != ConstantAddressProvenance::Scalar) ||
      E.AddressOwnerVA != InvalidVA)
    return std::nullopt;
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
/// Comparisons are handled separately below: HighIR stores their one-bit
/// result in a byte, which requires an explicit symbolic zero-extension.
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

/// A conversion is numeric only when both sides explicitly describe integers.
/// Widening a C cast extends according to its source signedness; the target's
/// signedness only interprets the resulting bits. Machine extensions instead
/// specify their extension kind in the opcode.
bool isIntegerConversion(const ExprPtr &E) {
  if (!E || !E->Type || E->Type->Kind != NdTypeKind::Int ||
      !widthFromBytes(E->Type->Size) || E->Operands.size() != 1 ||
      !E->Operands[0] || !E->Operands[0]->Type ||
      E->Operands[0]->Type->Kind != NdTypeKind::Int ||
      !widthFromBytes(E->Operands[0]->Type->Size))
    return false;
  if (E->Kind == ExprKind::Cast)
    return E->CastTo && E->CastTo->Kind == NdTypeKind::Int &&
           E->CastTo->Size == E->Type->Size &&
           E->CastTo->IsSigned == E->Type->IsSigned;
  return E->Kind == ExprKind::UnaryOp &&
         (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT) &&
         E->Type->Size >= E->Operands[0]->Type->Size;
}

bool isIntegerSlice(const ExprPtr &E) {
  if (!E || E->Kind != ExprKind::BinOp || E->Op != NdOp::SUBBYTES || !E->Type ||
      E->Type->Kind != NdTypeKind::Int || !widthFromBytes(E->Type->Size) ||
      E->Operands.size() != 2 || !E->Operands[0] || !E->Operands[0]->Type ||
      E->Operands[0]->Type->Kind != NdTypeKind::Int ||
      !widthFromBytes(E->Operands[0]->Type->Size) || !E->Operands[1] ||
      E->Operands[1]->Kind != ExprKind::Const)
    return false;
  const auto &Offset = E->Operands[1];
  return bitWidthOf(Offset) != 0 &&
         literalAt(*Offset, bitWidthOf(Offset), bitWidthOf(Offset)) &&
         Offset->ConstVal <= E->Operands[0]->Type->Size &&
         E->Type->Size <= E->Operands[0]->Type->Size - Offset->ConstVal;
}

bool isIntegerPredicate(NdOp Op) {
  switch (Op) {
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
    return true;
  default:
    return false;
  }
}

/// Rewriting arithmetic must not erase an observation or manufacture pointer
/// or unknown-value facts. Descendants are checked separately by the DAG walk.
bool isRewriteableNode(const ExprPtr &E) {
  if (!E || (E->Type && E->Type->Kind != NdTypeKind::Int) ||
      E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty() ||
      E->IndirectTarget)
    return false;
  switch (E->Kind) {
  case ExprKind::Const:
  case ExprKind::Var:
    return E->Operands.empty();
  case ExprKind::Cast:
    return isIntegerConversion(E);
  case ExprKind::BinOp:
    if (E->Op == NdOp::SUBBYTES && !isIntegerSlice(E))
      return false;
    return E->Op != NdOp::INT_DIV && E->Op != NdOp::INT_SDIV &&
           E->Op != NdOp::INT_REM && E->Op != NdOp::INT_SREM &&
           E->Op != NdOp::ATOMIC_ADD && E->Op != NdOp::ATOMIC_XCHG &&
           E->Op != NdOp::ATOMIC_CMPXCHG;
  case ExprKind::UnaryOp:
    return (E->Op != NdOp::INT_ZEXT && E->Op != NdOp::INT_SEXT) ||
           isIntegerConversion(E);
  case ExprKind::BitCast:
    return E->Type && E->Operands.size() == 1 && E->Operands[0] &&
           E->Operands[0]->Type && E->Type->Size != 0 &&
           E->Type->Size == E->Operands[0]->Type->Size;
  default:
    return false;
  }
}

template <typename WidthFn>
bool canTranslateOperands(const ExprPtr &E, const WidthFn &WidthOf) {
  const uint32_t Width = WidthOf(E);
  if (!Width)
    return false;
  if (isIntegerConversion(E) || isIntegerSlice(E))
    return true;
  if (E->Kind == ExprKind::BinOp && E->Operands.size() == 2 && E->Operands[0] &&
      E->Operands[1]) {
    const uint32_t Left = WidthOf(E->Operands[0]);
    const uint32_t Right = WidthOf(E->Operands[1]);
    if (E->Op == NdOp::CONCAT)
      return Left && Right && Left + Right == Width;
    if (isIntegerPredicate(E->Op))
      return Width == 8 && Left && Left == Right;
  }
  if (E->Kind == ExprKind::UnaryOp && E->Operands.size() == 1 &&
      (E->Op == NdOp::INT_NOT || E->Op == NdOp::INT_NEGATE ||
       E->Op == NdOp::INT_NEG2))
    return WidthOf(E->Operands[0]) == Width;
  if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
      binKindOf(E->Op) == BinKind::None)
    return false;
  for (const auto &Operand : E->Operands) {
    if (!Operand)
      return false;
    const uint32_t Have = WidthOf(Operand);
    if (Have == Width)
      continue;
    if (Operand->Kind != ExprKind::Const || Have == 0 || Have >= Width ||
        !literalAt(*Operand, Have, Width))
      return false;
  }
  return true;
}

/// Count the rendered tree without expanding shared subgraphs. Algebra may
/// discard or repeat any input, so an effect, unknown value, trapping operator,
/// or noninteger source excludes the complete root from this rewrite.
std::optional<size_t> expressionCost(const ExprPtr &Root) {
  struct Item {
    ExprPtr Expr;
    bool ChildrenReady = false;
  };
  llvm::SmallVector<Item, 64> Work{{Root, false}};
  std::unordered_map<const HighExpr *, size_t> Costs;
  std::unordered_set<const HighExpr *> Active;
  constexpr size_t Limit = std::numeric_limits<size_t>::max() / 2;
  while (!Work.empty()) {
    Item Current = Work.pop_back_val();
    const auto &E = Current.Expr;
    if (!E)
      return std::nullopt;
    if (Costs.count(E.get()))
      continue;
    if (!Current.ChildrenReady) {
      if (!Active.insert(E.get()).second || !isRewriteableNode(E))
        return std::nullopt;
      Work.push_back({E, true});
      for (auto It = E->Operands.rbegin(); It != E->Operands.rend(); ++It)
        Work.push_back({*It, false});
      continue;
    }
    Active.erase(E.get());
    size_t Cost = 1;
    for (const ExprPtr &Operand : E->Operands)
      Cost += std::min(Limit - Cost, Costs.at(Operand.get()));
    Costs.emplace(E.get(), Cost);
  }
  return Costs.at(Root.get());
}

using LocalIdentity = HighSourceLocalIdentity;
struct AvailableDefinition {
  ExprPtr Value;
  std::set<LocalIdentity> Dependencies;
};
using AvailableDefinitions = std::map<LocalIdentity, AvailableDefinition>;

bool isScalarLocal(const ExprPtr &E) {
  return E && E->Kind == ExprKind::Var && isRewriteableNode(E) && E->Type &&
         E->Type->Kind == NdTypeKind::Int && widthFromBytes(E->Type->Size) &&
         E->Type->Size == E->Var.Size &&
         (E->Var.Kind == MedVar::Reg || E->Var.Kind == MedVar::Temp ||
          E->Var.Kind == MedVar::Param);
}

//===----------------------------------------------------------------------===//
// HighIR to the engine
//===----------------------------------------------------------------------===//

class Translator {
public:
  Translator(sym::SymContext &Ctx, const AvailableDefinitions &Definitions)
      : Ctx(Ctx), Definitions(Definitions) {}

  sym::SymRef in(const ExprPtr &E);
  /// Rebuild a HighIR expression, or nothing when the result holds an operator
  /// with no HighIR spelling.  Unreachable as things stand — every candidate
  /// the solver builds is made of operators that came from here — but the
  /// alternative to checking is emitting something wrong.
  ExprPtr out(sym::SymRef R, uint32_t Width, bool ScalarLiterals);

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

  ExprPtr definitionFor(const ExprPtr &E) {
    if (!isScalarLocal(E))
      return nullptr;
    auto It = Definitions.find(highSourceLocalIdentity(E->Var));
    return It != Definitions.end() && widthOf(It->second.Value) == widthOf(E)
               ? It->second.Value
               : nullptr;
  }

  /// Formal integer parameter occurrences can select different low prefixes
  /// of the same incoming value. One unconstrained widest carrier preserves
  /// that relation without supplying values for its unknown upper bits.
  void prepareParameterViews(const ExprPtr &Root) {
    if (ParametersPrepared)
      return;
    ParametersPrepared = true;
    std::vector<ExprPtr> Work{Root};
    std::unordered_set<const HighExpr *> Seen;
    std::set<LocalIdentity> Conflicts;
    while (!Work.empty()) {
      auto E = Work.back();
      Work.pop_back();
      if (!E || !Seen.insert(E.get()).second)
        continue;
      if (ExprPtr Definition = definitionFor(E))
        Work.push_back(Definition);
      if (isScalarLocal(E) && E->Var.Kind == MedVar::Param &&
          E->Var.RenameTag < 0) {
        const auto Key = highSourceLocalIdentity(E->Var);
        if (!Conflicts.count(Key)) {
          auto It = ParameterViews.find(Key);
          if (It != ParameterViews.end() &&
              It->second->Var.TheArch != E->Var.TheArch) {
            ParameterViews.erase(It);
            Conflicts.insert(Key);
          } else if (It == ParameterViews.end() ||
                     widthOf(It->second) < widthOf(E)) {
            ParameterViews[Key] = E;
          }
        }
      }
      E->forEachChildExpr([&](const ExprPtr &Child) { Work.push_back(Child); });
    }
  }

  sym::SymContext &Ctx;
  const AvailableDefinitions &Definitions;
  bool ParametersPrepared = false;
  std::map<LocalIdentity, ExprPtr> ParameterViews;
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
  prepareParameterViews(E);
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
      if (ExprPtr Definition = definitionFor(Current)) {
        Dependencies.push_back(Definition);
      } else if (isIntegerConversion(Current) || isIntegerSlice(Current)) {
        Dependencies.push_back(Current->Operands[0]);
      } else if (Width != 0 && Current->Kind == ExprKind::BinOp &&
                 (binKindOf(Current->Op) != BinKind::None ||
                  Current->Op == NdOp::CONCAT ||
                  isIntegerPredicate(Current->Op)) &&
                 Current->Operands.size() == 2) {
        for (const ExprPtr &Operand : Current->Operands)
          if (widthOf(Operand) != 0 &&
              (widthOf(Operand) == Width || Current->Op == NdOp::CONCAT ||
               isIntegerPredicate(Current->Op)))
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
      if (ExprPtr Definition = definitionFor(Current)) {
        Result = in(Definition);
        break;
      }
      // The pass runs after source renaming. Distinct SSA versions can now
      // denote the same mutable C local, while different unrenamed kinds
      // must remain distinct. Share the source-flow identity policy.
      {
        const auto Key = highSourceLocalIdentity(Current->Var);
        const auto [Kind, Id, Version, Offset] = Key;
        auto Parameter = ParameterViews.find(Key);
        const bool UseParameter =
            Parameter != ParameterViews.end() && isScalarLocal(Current) &&
            widthOf(Parameter->second) >= Width &&
            Parameter->second->Var.TheArch == Current->Var.TheArch;
        const auto &Source = UseParameter ? Parameter->second : Current;
        Result = Ctx.mkVar(
            "v" + std::to_string(Kind) + "_" + std::to_string(Id) + "_" +
                std::to_string(Version) + "_" + std::to_string(Offset),
            widthOf(Source));
        Sources.emplace(Result.index(), Source);
        if (widthOf(Source) > Width)
          Result = Ctx.mkExtract(Result, 0, Width);
      }
      break;

    case ExprKind::BinOp: {
      if (isIntegerSlice(Current)) {
        Result = Ctx.mkExtract(in(Current->Operands[0]),
                               Current->Operands[1]->ConstVal * 8, Width);
        break;
      }
      if (Current->Op == NdOp::CONCAT && Current->Operands.size() == 2 &&
          canTranslateOperands(Current, [&](const ExprPtr &Operand) {
            return widthOf(Operand);
          })) {
        Result =
            Ctx.mkConcat(in(Current->Operands[0]), in(Current->Operands[1]));
        break;
      }
      if (isIntegerPredicate(Current->Op) && Current->Operands.size() == 2 &&
          canTranslateOperands(Current, [&](const ExprPtr &Operand) {
            return widthOf(Operand);
          })) {
        const sym::SymRef A = in(Current->Operands[0]);
        const sym::SymRef B = in(Current->Operands[1]);
        sym::SymRef Predicate;
        switch (Current->Op) {
        case NdOp::INT_EQUAL:
          Predicate = Ctx.mkEq(A, B);
          break;
        case NdOp::INT_NOTEQUAL:
          Predicate = Ctx.mkNe(A, B);
          break;
        case NdOp::INT_LESS:
          Predicate = Ctx.mkUlt(A, B);
          break;
        case NdOp::INT_SLESS:
          Predicate = Ctx.mkSlt(A, B);
          break;
        case NdOp::INT_LESSEQUAL:
          Predicate = Ctx.mkUle(A, B);
          break;
        case NdOp::INT_SLESSEQUAL:
          Predicate = Ctx.mkSle(A, B);
          break;
        case NdOp::INT_CARRY:
          Predicate = Ctx.mkUlt(Ctx.mkAdd(A, B), A);
          break;
        default:
          llvm_unreachable("non-predicate entered predicate translation");
        }
        Result = Ctx.mkZExt(Predicate, Width);
        break;
      }
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
      if (!isIntegerConversion(Current)) {
        Result = opaque(Current, Width);
        break;
      }
      sym::SymRef Operand = in(Current->Operands[0]);
      if (Ctx.width(Operand) >= Width)
        Result = Ctx.mkExtract(Operand, 0, Width);
      else if (Current->Operands[0]->Type->IsSigned)
        Result = Ctx.mkSExt(Operand, Width);
      else
        Result = Ctx.mkZExt(Operand, Width);
      break;
    }

    case ExprKind::UnaryOp: {
      if (isIntegerConversion(Current)) {
        sym::SymRef Operand = in(Current->Operands[0]);
        Result = Current->Op == NdOp::INT_SEXT ? Ctx.mkSExt(Operand, Width)
                                               : Ctx.mkZExt(Operand, Width);
        break;
      }
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
ExprPtr literalExpr(const llvm::APInt &Val, uint16_t Bytes,
                    bool ScalarLiterals) {
  const auto Provenance = ScalarLiterals ? ConstantAddressProvenance::Scalar
                                         : ConstantAddressProvenance::Unknown;
  if (std::optional<uint64_t> Direct = Val.tryZExtValue())
    return HighExpr::makeConst(*Direct, Bytes, Provenance);
  if (std::optional<uint64_t> Magnitude = (-Val).tryZExtValue())
    return HighExpr::makeUnary(
        NdOp::INT_NEG2, HighExpr::makeConst(*Magnitude, Bytes, Provenance));
  if (std::optional<uint64_t> Complement = (~Val).tryZExtValue())
    return HighExpr::makeUnary(
        NdOp::INT_NOT, HighExpr::makeConst(*Complement, Bytes, Provenance));
  return nullptr;
}

ExprPtr Translator::out(sym::SymRef R, uint32_t /*Width*/,
                        bool ScalarLiterals) {
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
      Result = literalExpr(Ctx.constValue(Item.Ref), ByteSize, ScalarLiterals);
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

      ExprPtr Acc =
          Plus.empty()
              ? HighExpr::makeConst(0, ByteSize,
                                    ScalarLiterals
                                        ? ConstantAddressProvenance::Scalar
                                        : ConstantAddressProvenance::Unknown)
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

    case sym::SymOp::Extract: {
      ExprPtr Operand = get(Ctx.operand(Item.Ref, 0));
      const uint64_t Low = Ctx.node(Item.Ref).Aux;
      if (!Operand || Low % 8 != 0)
        break;
      Result = std::make_shared<HighExpr>();
      Result->Type = NdType::makeInt(ByteSize, false);
      if (Low == 0) {
        Result->Kind = ExprKind::Cast;
        Result->CastTo = Result->Type;
        Result->Operands = {Operand};
      } else {
        Result->Kind = ExprKind::BinOp;
        Result->Op = NdOp::SUBBYTES;
        Result->Operands = {
            Operand,
            HighExpr::makeConst(Low / 8, 4, ConstantAddressProvenance::Scalar)};
      }
      break;
    }
    case sym::SymOp::ZExt:
    case sym::SymOp::SExt: {
      ExprPtr Operand = get(Ctx.operand(Item.Ref, 0));
      if (Operand) {
        Result = HighExpr::makeUnary(Ctx.op(Item.Ref) == sym::SymOp::ZExt
                                         ? NdOp::INT_ZEXT
                                         : NdOp::INT_SEXT,
                                     Operand);
        Result->Type = NdType::makeInt(ByteSize, false);
      }
      break;
    }
    case sym::SymOp::Concat: {
      llvm::ArrayRef<sym::SymRef> Parts = Ctx.operands(Item.Ref);
      if (Parts.size() != 2)
        break;
      ExprPtr High = get(Parts[0]);
      ExprPtr Low = get(Parts[1]);
      if (!High || !Low)
        break;
      Result = HighExpr::makeBinop(NdOp::CONCAT, High, Low);
      Result->Type = NdType::makeInt(ByteSize, false);
      break;
    }

    default:
      // Selects and predicates have no output spelling here.
      Result = nullptr;
      break;
    }
    Built.emplace(Index, std::move(Result));
  }

  auto It = Built.find(R.index());
  assert(It != Built.end() && "iterative HighIR rebuild lost its root");
  return It->second;
}

/// All literal leaves actually read by the symbolic region must be known
/// scalars before a newly folded literal can carry scalar provenance. Visible
/// definitions participate because the translator reads them at the same
/// width. Unknown or owned address literals keep the conservative provenance.
bool hasOnlyScalarLiteralInputs(const ExprPtr &Root,
                                const AvailableDefinitions &Definitions) {
  std::vector<ExprPtr> Work{Root};
  std::unordered_set<const HighExpr *> Seen;
  size_t Budget = 4096;
  while (!Work.empty()) {
    ExprPtr Current = Work.back();
    Work.pop_back();
    if (!Current || !Budget--)
      return false;
    if (!Seen.insert(Current.get()).second)
      continue;
    if (Current->Kind == ExprKind::Const) {
      if (Current->ConstProvenance != ConstantAddressProvenance::Scalar ||
          Current->AddressOwnerVA != InvalidVA)
        return false;
      continue;
    }
    if (isScalarLocal(Current)) {
      auto It = Definitions.find(highSourceLocalIdentity(Current->Var));
      if (It != Definitions.end() &&
          bitWidthOf(It->second.Value) == bitWidthOf(Current))
        Work.push_back(It->second.Value);
    }
    // Match Translator::in: a slice reads its source but uses the second
    // operand only as an extraction index. Unsupported nodes are opaque, so
    // literals inside their operands cannot contribute to a folded constant.
    if (isIntegerConversion(Current) || isIntegerSlice(Current)) {
      Work.push_back(Current->Operands[0]);
    } else if (canTranslateOperands(Current, bitWidthOf)) {
      Work.insert(Work.end(), Current->Operands.begin(),
                  Current->Operands.end());
    }
  }
  return true;
}

/// Simplify one expression, in place, if there is anything to gain.
void simplifyOne(ExprPtr &E, const AvailableDefinitions &Definitions) {
  const uint32_t Width = bitWidthOf(E);
  if (Width == 0)
    return;

  const std::optional<size_t> BeforeCost = expressionCost(E);
  if (!BeforeCost ||
      (*BeforeCost < kMinInterestingNodes && Definitions.empty()))
    return;

  sym::SymContext Ctx;
  Translator Xlat(Ctx, Definitions);
  sym::SymRef Before = Xlat.in(E);

  // Constructor identities can already recover the result while translating
  // exact slices and extensions. Compare the final HighIR tree with the input,
  // including that work, rather than requiring another solver rewrite.
  // This pass visits every source expression, including large copy-propagated
  // compiler output. Give each optional rewrite a bounded share of work;
  // callers explicitly simplifying an obfuscated MBA retain the solver's
  // larger default budget. Exhausted regions remain unchanged.
  sym::MBAOptions Options;
  Options.MaxWork = size_t(1) << 16;
  sym::MBAResult Result = sym::simplifyMBADeep(Ctx, Before, Options);
  if (Result.Changed && Result.Evidence != sym::MBAEvidence::Derivation)
    return;

  solver::SymSynthVerifier WideVerifier;
  const sym::SymRef Recovered = sym::recoverSplitWordArithmetic(
      Ctx, Result.Expr,
      [&](sym::SymContext &Context, sym::SymRef A, sym::SymRef B) {
        return WideVerifier(Context, A, B);
      });
  if (Recovered != Result.Expr)
    Result.Expr = Recovered;

  ExprPtr After =
      Xlat.out(Result.Expr, Width, hasOnlyScalarLiteralInputs(E, Definitions));
  if (!After)
    return;
  if (After->Kind == ExprKind::Var && E->Type && After->Type &&
      E->Type->IsSigned != After->Type->IsSigned) {
    auto Cast = std::make_shared<HighExpr>();
    Cast->Kind = ExprKind::Cast;
    Cast->Type = Cast->CastTo = E->Type;
    Cast->Operands = {After};
    After = std::move(Cast);
  } else {
    // A solver variable may be a shared source node. Do not change its type
    // at another use when preserving the enclosing expression's result type.
    After = std::make_shared<HighExpr>(*After);
    if (E->Type) {
      After->Type = E->Type;
      if (After->Kind == ExprKind::Cast)
        After->CastTo = E->Type;
    }
  }
  const std::optional<size_t> AfterCost = expressionCost(After);
  // An equal-size source expression can hide a much larger proved expansion
  // through earlier scalar definitions. Accept that tie when the expanded
  // algebra became strictly smaller; dead definitions can then be removed.
  const bool ShorterExpandedDefinition =
      AfterCost && *AfterCost == *BeforeCost && !Definitions.empty() &&
      Result.Changed && Result.SizeAfter < Result.SizeBefore;
  if (!AfterCost ||
      (*BeforeCost < *AfterCost + kMinGain && !ShorterExpandedDefinition))
    return;
  E = After;
}

/// Simplify maximal numeric regions, including those beneath opaque operators
/// such as POPCOUNT and comparisons. Rebuild only changed ancestor nodes so a
/// call, memory access, pointer cast, or other boundary stays in its original
/// evaluation position. Each numeric region reaches the solver once.
void simplifyRegions(ExprPtr &Root, const AvailableDefinitions &Definitions) {
  struct Item {
    ExprPtr Expr;
    bool ChildrenReady = false;
  };
  llvm::SmallVector<Item, 64> Work{{Root, false}};
  std::unordered_set<const HighExpr *> Active;
  std::unordered_map<const HighExpr *, bool> Safe;
  std::unordered_map<const HighExpr *, uint32_t> Widths;
  std::vector<ExprPtr> Order;
  while (!Work.empty()) {
    Item Current = Work.pop_back_val();
    const auto &E = Current.Expr;
    if (!E || Safe.count(E.get()))
      continue;
    if (!Current.ChildrenReady) {
      if (!Active.insert(E.get()).second)
        return;
      Work.push_back({E, true});
      E->forEachChildExpr(
          [&](const ExprPtr &Child) { Work.push_back({Child, false}); });
      continue;
    }
    Active.erase(E.get());
    bool IsSafe = isRewriteableNode(E);
    for (const auto &Child : E->Operands)
      IsSafe &= Child && Safe.at(Child.get());
    Safe.emplace(E.get(), IsSafe);
    uint16_t Bytes = E->Type ? E->Type->Size : 0;
    if (!Bytes && E->Kind == ExprKind::Var)
      Bytes = E->Var.Size;
    uint32_t Width = widthFromBytes(Bytes);
    if (!Bytes && !E->Operands.empty() && E->Operands[0] &&
        (E->Kind == ExprKind::BinOp || E->Kind == ExprKind::UnaryOp))
      Width = Widths.at(E->Operands[0].get());
    Widths.emplace(E.get(), Width);
    Order.push_back(E);
  }
  if (!Root)
    return;

  auto WidthOf = [&](const ExprPtr &E) {
    return E ? Widths.at(E.get()) : uint32_t(0);
  };
  std::unordered_set<const HighExpr *> Regions{Root.get()}, Numeric;
  for (const auto &E : Order) {
    const bool Translatable = canTranslateOperands(E, WidthOf);
    if (Translatable)
      Numeric.insert(E.get());
    if (!Safe.at(E.get()) || !Translatable)
      E->forEachChildExpr(
          [&](const ExprPtr &Child) { Regions.insert(Child.get()); });
  }

  std::unordered_map<const HighExpr *, ExprPtr> Rebuilt;
  for (const auto &E : Order) {
    ExprPtr Result = E;
    bool Changed = false;
    E->forEachChildExpr([&](const ExprPtr &Child) {
      Changed |= Rebuilt.at(Child.get()) != Child;
    });
    if (Changed) {
      Result = std::make_shared<HighExpr>(*E);
      for (auto &Child : Result->Operands)
        if (Child)
          Child = Rebuilt.at(Child.get());
      if (Result->IndirectTarget)
        Result->IndirectTarget = Rebuilt.at(Result->IndirectTarget.get());
    }
    if (Safe.at(E.get()) && Regions.count(E.get()) && Numeric.count(E.get()))
      simplifyOne(Result, Definitions);
    Rebuilt.emplace(E.get(), std::move(Result));
  }
  Root = Rebuilt.at(Root.get());
}

/// Keep only bounded, scalar dependencies that remain stable at every use.
/// Definitions stay in HighIR; only their symbolic reading is expanded. The
/// transitive dependency set invalidates that reading after any local write.
std::optional<std::set<LocalIdentity>>
definitionInputs(const ExprPtr &Value, const AvailableDefinitions &Definitions,
                 const std::set<LocalIdentity> &Escaped) {
  const auto Cost = expressionCost(Value);
  if (!Cost || *Cost > 4096)
    return std::nullopt;
  std::set<LocalIdentity> Inputs;
  std::vector<ExprPtr> Work{Value};
  std::unordered_set<const HighExpr *> Seen;
  while (!Work.empty()) {
    auto E = Work.back();
    Work.pop_back();
    if (!Seen.insert(E.get()).second)
      continue;
    if (E->Kind == ExprKind::Var) {
      if (!isScalarLocal(E))
        return std::nullopt;
      const auto Key = highSourceLocalIdentity(E->Var);
      if (Escaped.count(Key))
        return std::nullopt;
      Inputs.insert(Key);
      auto It = Definitions.find(Key);
      if (It != Definitions.end())
        Inputs.insert(It->second.Dependencies.begin(),
                      It->second.Dependencies.end());
      if (Inputs.size() > 256)
        return std::nullopt;
    }
    Work.insert(Work.end(), E->Operands.begin(), E->Operands.end());
  }
  return Inputs;
}

void simplifyStatementRegions(std::vector<HighStmt> &Stmts) {
  std::set<LocalIdentity> Escaped;
  std::unordered_set<va_t> Entries;
  std::unordered_set<const HighExpr *> Seen;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto)
      Entries.insert(S.GotoTarget);
    for (const auto &Clause : S.EHClauses) {
      Entries.insert(Clause.HandlerVA);
      Entries.insert(Clause.LandingPadVAs.begin(), Clause.LandingPadVAs.end());
      Entries.insert(Clause.ContinuationVAs.begin(),
                     Clause.ContinuationVAs.end());
    }
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Work{Root};
      while (!Work.empty()) {
        auto E = Work.back();
        Work.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (E->Kind == ExprKind::Addr) {
          std::vector<ExprPtr> AddressWork{E};
          std::unordered_set<const HighExpr *> AddressSeen;
          while (!AddressWork.empty()) {
            auto A = AddressWork.back();
            AddressWork.pop_back();
            if (!A || !AddressSeen.insert(A.get()).second)
              continue;
            if (A->Kind == ExprKind::Var)
              Escaped.insert(highSourceLocalIdentity(A->Var));
            A->forEachChildExpr(
                [&](const ExprPtr &Child) { AddressWork.push_back(Child); });
          }
        }
        E->forEachChildExpr(
            [&](const ExprPtr &Child) { Work.push_back(Child); });
      }
    });
  });
  Entries.erase(0);
  Entries.erase(InvalidVA);

  std::vector<std::vector<HighStmt> *> Scopes{&Stmts};
  while (!Scopes.empty()) {
    auto *Scope = Scopes.back();
    Scopes.pop_back();
    AvailableDefinitions Definitions;
    for (auto &S : *Scope) {
      for (auto *Body : {&S.Body, &S.ElseBody, &S.DefaultBody})
        if (!Body->empty())
          Scopes.push_back(Body);
      for (auto &Case : S.Cases)
        Scopes.push_back(&Case.Body);
      for (auto &Body : S.EHClauseBodies)
        Scopes.push_back(&Body);

      const bool Straight =
          (S.Kind == StmtKind::Assign || S.Kind == StmtKind::ExprStmt ||
           S.Kind == StmtKind::Return || S.Kind == StmtKind::Nop ||
           S.Kind == StmtKind::Store) &&
          S.Body.empty() && S.ElseBody.empty() && S.Cases.empty() &&
          S.DefaultBody.empty() && S.EHClauses.empty() &&
          S.EHClauseBodies.empty() && !S.IsPhiCopy &&
          S.MemoryOrdering == NdMemoryOrdering::None &&
          S.MemoryAddressSpace == NdMemoryAddressSpace::Default;
      bool Pure = true;
      forEachRhsExpr(
          S, [&](const ExprPtr &E) { Pure &= expressionCost(E).has_value(); });
      if (!Straight || !Pure || Entries.count(S.Addr) ||
          (S.Kind == StmtKind::Assign && !isScalarLocal(S.Dst)))
        Definitions.clear();
      forEachRhsExpr(S, [&](ExprPtr &E) { simplifyRegions(E, Definitions); });
      if (!Straight || !Pure || S.Kind == StmtKind::Return) {
        Definitions.clear();
        continue;
      }
      // An ordinary store changes memory, not a scalar register, temporary,
      // or unescaped parameter.  Its address and value have already been
      // checked for effects; keep proven scalar definitions available to the
      // following expressions, including a store/reload pair in the frame.
      if (S.Kind == StmtKind::Store)
        continue;
      if (S.Kind != StmtKind::Assign)
        continue;
      if (!isScalarLocal(S.Dst)) {
        Definitions.clear();
        continue;
      }

      const auto Key = highSourceLocalIdentity(S.Dst->Var);
      auto Inputs = definitionInputs(S.Val, Definitions, Escaped);
      for (auto It = Definitions.begin(); It != Definitions.end();) {
        if (It->first == Key || It->second.Dependencies.count(Key))
          It = Definitions.erase(It);
        else
          ++It;
      }
      if (Inputs && !Inputs->count(Key) && !Escaped.count(Key) &&
          Definitions.size() < 256 && S.Val && S.Val->Type &&
          S.Val->Type->Size == S.Dst->Type->Size &&
          S.Val->Type->IsSigned == S.Dst->Type->IsSigned)
        Definitions.emplace(Key,
                            AvailableDefinition{S.Val, std::move(*Inputs)});
    }
  }
}

} // namespace

void simplifyExprSemantics(std::vector<HighStmt> &Stmts) {
  simplifyStatementRegions(Stmts);
}

} // namespace neverd
