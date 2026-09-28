//===- SymSimplifyTranslator.cpp - LLVM IR <-> symbolic engine --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Carries a function's integer expression trees into the symbolic engine and
/// rebuilds LLVM IR from the engine's result.  Only the operators that are
/// bitvector arithmetic on a whole word are translated; everything else becomes
/// one opaque input and comes back untouched.  SymSimplifyPass.cpp drives this.
///
/// Width is not one of the limits.  A literal crosses as an \c llvm::APInt and
/// an input as its declared bit width, so an i128 or i512 expression is
/// carried, measured and rebuilt exactly as an i32 one is -- which is what
/// lets the pass reach obfuscation written in a wide word.
///
//===----------------------------------------------------------------------===//

#include "SymSimplifyDetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/ConstantFolder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/ErrorHandling.h"

#include <cstdint>
#include <optional>

namespace neverd {

namespace {

struct SplitWordAssembly {
  llvm::Value *High;
  llvm::Value *Low;
};

/// A disjoint OR may be read as a concatenation only when its operands are
/// statically confined to the upper and lower halves. This also proves the
/// widened shift's unsigned no-wrap flag for every value of its source.
std::optional<SplitWordAssembly> splitWordAssembly(const llvm::Instruction &I) {
  if (I.getOpcode() != llvm::Instruction::Or || !I.getType()->isIntegerTy() ||
      I.getType()->getIntegerBitWidth() % 2 != 0)
    return std::nullopt;
  const unsigned Wide = I.getType()->getIntegerBitWidth();
  const unsigned Half = Wide / 2;
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto *Shift =
        llvm::dyn_cast<llvm::BinaryOperator>(I.getOperand(Side));
    const auto *Low = llvm::dyn_cast<llvm::ZExtInst>(I.getOperand(1 - Side));
    if (!Shift || Shift->getOpcode() != llvm::Instruction::Shl || !Low ||
        Shift->getType() != I.getType() || Low->getType() != I.getType() ||
        !Low->getSrcTy()->isIntegerTy(Half))
      continue;
    const auto *High = llvm::dyn_cast<llvm::ZExtInst>(Shift->getOperand(0));
    const auto *Amount =
        llvm::dyn_cast<llvm::ConstantInt>(Shift->getOperand(1));
    if (!High || High->getType() != I.getType() ||
        !High->getSrcTy()->isIntegerTy(Half) || !Amount ||
        !Amount->equalsInt(Half) || Shift->hasNoSignedWrap())
      continue;
    return SplitWordAssembly{High->getOperand(0), Low->getOperand(0)};
  }
  return std::nullopt;
}

bool safeWidenedShift(const llvm::Instruction &I) {
  if (I.getOpcode() != llvm::Instruction::Shl || !I.getType()->isIntegerTy())
    return false;
  const auto *Source = llvm::dyn_cast<llvm::ZExtInst>(I.getOperand(0));
  const auto *Amount = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(1));
  if (!Source || !Amount || !Source->getSrcTy()->isIntegerTy() ||
      I.hasNoSignedWrap())
    return false;
  const unsigned ResultWidth = I.getType()->getIntegerBitWidth();
  const unsigned SourceWidth = Source->getSrcTy()->getIntegerBitWidth();
  return Amount->getValue().ule(ResultWidth - SourceWidth);
}

/// Whether \p I has the same domain in LLVM IR and in the engine's total
/// bitvector algebra.
///
/// The engine deliberately gives every operator a value for every input.
/// LLVM instructions carrying poison-generating flags, or operations such as
/// an unchecked shift, have a smaller domain.  Looking through one would let a
/// total-algebra identity turn poison into an ordinary value.  Keep the whole
/// candidate opaque instead; a future translation can carry the precondition
/// explicitly rather than silently dropping it.
bool hasCompatibleSemantics(const llvm::Instruction &I) {
  // This catches poison-generating flags added to LLVM after this switch was
  // written.  The per-opcode checks below remain deliberate documentation of
  // the exact language subset accepted today.
  if ((I.hasPoisonGeneratingFlags() && !splitWordAssembly(I) &&
       !safeWidenedShift(I)) ||
      I.hasPoisonGeneratingAttributes() || I.hasPoisonGeneratingMetadata())
    return false;

  auto HasNoWrap = [&] {
    const auto &Op = llvm::cast<llvm::OverflowingBinaryOperator>(I);
    return Op.hasNoUnsignedWrap() || Op.hasNoSignedWrap();
  };
  auto HasInRangeConstantShift = [&] {
    const auto *Amount = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(1));
    return Amount && Amount->getValue().ult(I.getType()->getIntegerBitWidth());
  };

  switch (tagOf(I)) {
  case OpTag::Add:
  case OpTag::Sub:
  case OpTag::Mul:
    return !HasNoWrap();
  case OpTag::And:
  case OpTag::Xor:
    return true;
  case OpTag::Or:
    return !llvm::cast<llvm::PossiblyDisjointInst>(I).isDisjoint() ||
           splitWordAssembly(I).has_value();
  case OpTag::Shl:
    return (!HasNoWrap() || safeWidenedShift(I)) && HasInRangeConstantShift();
  case OpTag::LShr:
  case OpTag::AShr:
    return !llvm::cast<llvm::PossiblyExactOperator>(I).isExact() &&
           HasInRangeConstantShift();
  case OpTag::UDiv:
  case OpTag::SDiv:
  case OpTag::URem:
  case OpTag::SRem:
    return false;
  case OpTag::Trunc: {
    const auto &Trunc = llvm::cast<llvm::TruncInst>(I);
    return !Trunc.hasNoUnsignedWrap() && !Trunc.hasNoSignedWrap();
  }
  case OpTag::ZExt:
    return !I.hasNonNeg();
  case OpTag::SExt:
    return true;
  case OpTag::ICmp:
    return !llvm::cast<llvm::ICmpInst>(I).hasSameSign();
  case OpTag::FShl: {
    const auto *Amount = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(2));
    return Amount != nullptr;
  }
  case OpTag::None:
    return false;
  }
  llvm_unreachable("unhandled symbolic operator tag");
}

/// Whether traversing \p I would cross semantics outside the engine's total
/// scalar bitvector algebra.
bool hasIncompatibleSemantics(const llvm::Instruction &I,
                              bool WithComparisons) {
  if (llvm::isa<llvm::FreezeInst>(I) ||
      (I.hasPoisonGeneratingAnnotations() && !splitWordAssembly(I) &&
       !safeWidenedShift(I)))
    return true;
  return isTranslatable(&I, WithComparisons) && !hasCompatibleSemantics(I);
}

/// Whether a translatable value hidden behind an opaque shared leaf contains
/// semantics the total bitvector engine cannot preserve.
///
/// Ordinary translation visits every operand and rejects the whole candidate
/// when it reaches undef, poison, or a poison-generating instruction.  A value
/// with a use outside the measured region stays opaque, though, so its operands
/// would otherwise escape that check.  Follow the defining operand graph only
/// to reject unsafe candidates; this never makes an opaque operation
/// translatable.
bool hasHiddenIncompatibleSemantics(const llvm::Instruction &Root,
                                    bool WithComparisons) {
  llvm::SmallVector<const llvm::Value *, 8> Work{&Root};

  llvm::DenseSet<const llvm::Value *> Seen;
  while (!Work.empty()) {
    const llvm::Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(V))
      return true;

    const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
    if (!I)
      continue;
    if (hasIncompatibleSemantics(*I, WithComparisons))
      return true;
    // Loads remain exact opaque inputs to the symbolic expression. Their
    // address graph is preserved with the load, so poison-generating pointer
    // arithmetic there must not block simplification of surrounding values.
    if (llvm::isa<llvm::LoadInst>(I))
      continue;
    for (const llvm::Use &Op : I->operands())
      Work.push_back(Op.get());
  }
  return false;
}

} // namespace

OpTag tagOf(const llvm::Instruction &I) {
  if (const auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&I))
    if (Intrinsic->getIntrinsicID() == llvm::Intrinsic::fshl)
      return OpTag::FShl;
  if (llvm::isa<llvm::TruncInst>(I))
    return OpTag::Trunc;
  if (llvm::isa<llvm::ZExtInst>(I))
    return OpTag::ZExt;
  if (llvm::isa<llvm::SExtInst>(I))
    return OpTag::SExt;
  if (llvm::isa<llvm::ICmpInst>(I))
    return OpTag::ICmp;
  switch (I.getOpcode()) {
  case llvm::Instruction::Add:
    return OpTag::Add;
  case llvm::Instruction::Sub:
    return OpTag::Sub;
  case llvm::Instruction::Mul:
    return OpTag::Mul;
  case llvm::Instruction::And:
    return OpTag::And;
  case llvm::Instruction::Or:
    return OpTag::Or;
  case llvm::Instruction::Xor:
    return OpTag::Xor;
  case llvm::Instruction::Shl:
    return OpTag::Shl;
  case llvm::Instruction::LShr:
    return OpTag::LShr;
  case llvm::Instruction::AShr:
    return OpTag::AShr;
  case llvm::Instruction::UDiv:
    return OpTag::UDiv;
  case llvm::Instruction::SDiv:
    return OpTag::SDiv;
  case llvm::Instruction::URem:
    return OpTag::URem;
  case llvm::Instruction::SRem:
    return OpTag::SRem;
  default:
    return OpTag::None;
  }
}

bool isTranslatable(const llvm::Value *V, bool WithComparisons) {
  const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
  if (!I || !I->getType()->isIntegerTy())
    return false;
  OpTag Tag = tagOf(*I);
  if (Tag == OpTag::FShl && !llvm::isa<llvm::ConstantInt>(I->getOperand(2)))
    return false;
  return Tag != OpTag::None && (WithComparisons || Tag != OpTag::ICmp);
}

sym::SymRef Translator::build(const llvm::Instruction &I) {
  auto M = [&](llvm::Value *V) { return Memo.lookup(V); };
  llvm::Value *A = I.getOperand(0);
  llvm::Value *B = I.getNumOperands() > 1 ? I.getOperand(1) : nullptr;
  const uint32_t Width = I.getType()->getIntegerBitWidth();

  switch (tagOf(I)) {
  case OpTag::Add:
    return Ctx.mkAdd(M(A), M(B));
  case OpTag::Sub:
    // `0 - x` is negation, which the engine represents as a product with -1;
    // handing it that form directly keeps the sign out of the measured value.
    return Ctx.isConstZero(M(A)) ? Ctx.mkNeg(M(B)) : Ctx.mkSub(M(A), M(B));
  case OpTag::Mul:
    return Ctx.mkMul(M(A), M(B));
  case OpTag::And:
    return Ctx.mkAnd(M(A), M(B));
  case OpTag::Or:
    if (auto Pair = splitWordAssembly(I))
      return Ctx.mkConcat(M(Pair->High), M(Pair->Low));
    return Ctx.mkOr(M(A), M(B));
  case OpTag::Xor:
    // `x ^ -1` is complement, a generator of the bitwise algebra the solver
    // works in, so it is worth recognising rather than leaving as a xor.
    if (Ctx.isConstOnes(M(B)))
      return Ctx.mkNot(M(A));
    if (Ctx.isConstOnes(M(A)))
      return Ctx.mkNot(M(B));
    return Ctx.mkXor(M(A), M(B));
  case OpTag::Shl:
    return Ctx.mkShl(M(A), M(B));
  case OpTag::LShr:
    return Ctx.mkLShr(M(A), M(B));
  case OpTag::AShr:
    return Ctx.mkAShr(M(A), M(B));
  case OpTag::UDiv:
    return Ctx.mkUDiv(M(A), M(B));
  case OpTag::SDiv:
    return Ctx.mkSDiv(M(A), M(B));
  case OpTag::URem:
    return Ctx.mkURem(M(A), M(B));
  case OpTag::SRem:
    return Ctx.mkSRem(M(A), M(B));
  case OpTag::Trunc:
    return Ctx.mkExtract(M(A), 0, Width);
  case OpTag::ZExt:
    return Ctx.mkZExt(M(A), Width);
  case OpTag::SExt:
    return Ctx.mkSExt(M(A), Width);
  case OpTag::ICmp:
    switch (llvm::cast<llvm::ICmpInst>(I).getPredicate()) {
    case llvm::CmpInst::ICMP_EQ:
      return Ctx.mkEq(M(A), M(B));
    case llvm::CmpInst::ICMP_NE:
      return Ctx.mkNe(M(A), M(B));
    case llvm::CmpInst::ICMP_ULT:
      return Ctx.mkUlt(M(A), M(B));
    case llvm::CmpInst::ICMP_ULE:
      return Ctx.mkUle(M(A), M(B));
    case llvm::CmpInst::ICMP_UGT:
      return Ctx.mkUgt(M(A), M(B));
    case llvm::CmpInst::ICMP_UGE:
      return Ctx.mkUge(M(A), M(B));
    case llvm::CmpInst::ICMP_SLT:
      return Ctx.mkSlt(M(A), M(B));
    case llvm::CmpInst::ICMP_SLE:
      return Ctx.mkSle(M(A), M(B));
    case llvm::CmpInst::ICMP_SGT:
      return Ctx.mkSgt(M(A), M(B));
    case llvm::CmpInst::ICMP_SGE:
      return Ctx.mkSge(M(A), M(B));
    default:
      break;
    }
    break;
  case OpTag::FShl: {
    const auto *Amount = llvm::cast<llvm::ConstantInt>(I.getOperand(2));
    const uint32_t Shift = static_cast<uint32_t>(
        Amount->getValue()
            .urem(llvm::APInt(Amount->getBitWidth(), Width))
            .getZExtValue());
    if (Shift == 0)
      return M(A);
    return Ctx.mkOr(Ctx.mkShl(M(A), Ctx.mkConst(Width, Shift)),
                    Ctx.mkLShr(M(B), Ctx.mkConst(Width, Width - Shift)));
  }
  case OpTag::None:
    break;
  }
  llvm_unreachable("build called on an instruction with no operator tag");
}

void Translator::collectRegion(llvm::Value *Root) {
  llvm::SmallVector<llvm::Value *, 64> Pending{Root};
  while (!Pending.empty()) {
    llvm::Value *V = Pending.pop_back_val();
    if (!isTranslatable(V, CarryComparisons) || !Region.insert(V).second)
      continue;
    auto Ops = children(*llvm::cast<llvm::Instruction>(V));
    Pending.append(Ops.begin(), Ops.end());
  }

  // The closed region is cheaper to measure and gives shared computations a
  // stable opaque identity.  If it cannot simplify, a second translation may
  // inspect pure shared definitions, which is needed when a split-word carry
  // also reads a carry-save expression's XOR or AND.
  for (const llvm::Value *V : Region)
    if (V != Root && llvm::any_of(V->users(), [&](const llvm::User *U) {
          return !Region.contains(U);
        })) {
      SharedBoundary = true;
      if (!ExpandSharedPure)
        Pending.push_back(const_cast<llvm::Value *>(V));
    }
  if (!ExpandSharedPure)
    while (!Pending.empty()) {
      llvm::Value *V = Pending.pop_back_val();
      if (!Region.erase(V))
        continue;
      for (llvm::Value *Operand : children(*llvm::cast<llvm::Instruction>(V)))
        if (Operand != Root && Region.contains(Operand))
          Pending.push_back(Operand);
    }

  // Count only operations whose every use dies after Root is replaced. Pure
  // shared expressions remain live for their other consumers even when the
  // second translation uses their definitions to prove an identity.
  llvm::DenseMap<const llvm::Value *, unsigned> RemainingUses;
  for (const llvm::Value *V : Region)
    RemainingUses[V] = V->getNumUses();
  llvm::SmallVector<const llvm::Value *, 64> Dead{Root};
  llvm::DenseSet<const llvm::Value *> Scheduled{Root};
  while (!Dead.empty()) {
    const llvm::Value *V = Dead.pop_back_val();
    ++NumDescended;
    for (const llvm::Value *Operand :
         children(*llvm::cast<llvm::Instruction>(V))) {
      auto It = RemainingUses.find(Operand);
      if (It == RemainingUses.end() || It->second == 0)
        continue;
      if (--It->second == 0 && Scheduled.insert(Operand).second)
        Dead.push_back(Operand);
    }
  }
}

sym::SymRef Translator::in(llvm::Value *Root) {
  collectRegion(Root);
  struct WorkItem {
    llvm::Value *V;
    bool ChildrenReady;
  };
  llvm::SmallVector<WorkItem, 64> Work{{Root, false}};
  llvm::DenseSet<const llvm::Value *> Active;

  while (!Work.empty()) {
    WorkItem Item = Work.pop_back_val();
    llvm::Value *V = Item.V;
    if (Memo.count(V)) {
      if (Item.ChildrenReady)
        Active.erase(V);
      continue;
    }

    // An explicit undef may take a different value at each use, and poison is
    // outside the engine's total value domain.  Likewise, do not cross an
    // instruction that can introduce poison from otherwise ordinary operands.
    // Aborting the candidate is stricter than standing an opaque variable in
    // front of it: an algebraic rewrite could otherwise cancel that variable
    // and erase the very condition the placeholder was meant to preserve.
    if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(V))
      return {};
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
        I && hasIncompatibleSemantics(*I, CarryComparisons))
      return {};

    if (!descend(V)) {
      if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
          I && hasHiddenIncompatibleSemantics(*I, CarryComparisons))
        return {};
      sym::SymRef Leaf = leaf(V);
      // A leaf with no bitvector reading cannot even be an opaque input, so
      // the candidate goes no further.
      if (!Leaf.isValid())
        return {};
      Memo[V] = Leaf;
      continue;
    }

    auto &I = *llvm::cast<llvm::Instruction>(V);
    if (!Item.ChildrenReady) {
      // Def-use of non-PHI values is acyclic, but malformed input should
      // preserve a node opaquely rather than spin the worklist forever.
      if (!Active.insert(V).second) {
        Memo[V] = leaf(V);
        continue;
      }
      Work.push_back({V, true});
      llvm::SmallVector<llvm::Value *, 2> Kids = children(I);
      for (auto It = Kids.rbegin(); It != Kids.rend(); ++It)
        if (!Memo.count(*It))
          Work.push_back({*It, false});
      continue;
    }

    Active.erase(V);
    Memo[V] = build(I);
  }
  return Memo.lookup(Root);
}

bool Translator::retainsOpaqueInstructionLeaves(sym::SymRef R) const {
  if (!R.isValid())
    return false;
  if (OpaqueInstructionLeaves.empty())
    return true;

  llvm::SmallVector<sym::SymRef, 32> Work{R};
  llvm::DenseSet<uint32_t> Seen;
  unsigned Retained = 0;
  while (!Work.empty()) {
    sym::SymRef Current = Work.pop_back_val();
    if (!Seen.insert(Current.index()).second)
      continue;
    Retained += OpaqueInstructionLeaves.contains(Current.index());
    llvm::ArrayRef<sym::SymRef> Ops = Ctx.operands(Current);
    Work.append(Ops.begin(), Ops.end());
  }
  return Retained == OpaqueInstructionLeaves.size();
}

llvm::Value *
Translator::out(sym::SymRef R, llvm::Instruction *At,
                llvm::SmallVectorImpl<llvm::Instruction *> &NewInsts) {
  llvm::LLVMContext &LLCtx = At->getContext();

  // Collecting through the inserter rather than diffing the block afterwards
  // keeps the list exact: the folder answers some of these builder calls with a
  // constant and others with a value the original code already computes, and
  // neither is an instruction this rewrite is paying for.
  llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderCallbackInserter> B(
      LLCtx, llvm::ConstantFolder(),
      llvm::IRBuilderCallbackInserter(
          [&NewInsts](llvm::Instruction *I) { NewInsts.push_back(I); }));
  B.SetInsertPoint(At);
  // The rebuilt value computes what the root computed, so it belongs to the
  // same source construct.  Without this the recovered arithmetic would carry
  // no location at all and a debug build would lose the line the obfuscated
  // expression came from -- the one line a reader most wants back.
  B.SetCurrentDebugLocation(At->getDebugLoc());

  struct WorkItem {
    sym::SymRef Ref;
    bool ChildrenReady;
  };
  llvm::SmallVector<WorkItem, 64> Work{{R, false}};
  llvm::DenseMap<uint32_t, llvm::Value *> Built;
  llvm::DenseSet<uint32_t> Active;

  auto get = [&](sym::SymRef C) -> llvm::Value * {
    return Built.lookup(C.index());
  };

  while (!Work.empty()) {
    WorkItem Item = Work.pop_back_val();
    const uint32_t Index = Item.Ref.index();
    if (Built.count(Index)) {
      if (Item.ChildrenReady)
        Active.erase(Index);
      continue;
    }
    if (!Item.ChildrenReady) {
      if (!Active.insert(Index).second) {
        Built[Index] = nullptr;
        continue;
      }
      Work.push_back({Item.Ref, true});
      llvm::ArrayRef<sym::SymRef> Ops = Ctx.operands(Item.Ref);
      for (auto It = Ops.rbegin(); It != Ops.rend(); ++It)
        if (!Built.count(It->index()))
          Work.push_back({*It, false});
      continue;
    }

    Active.erase(Index);
    const uint32_t Width = Ctx.width(Item.Ref);
    auto *Ty = llvm::IntegerType::get(LLCtx, Width);
    llvm::ArrayRef<sym::SymRef> Ops = Ctx.operands(Item.Ref);

    auto reduce = [&](auto Make) -> llvm::Value * {
      if (Ops.empty())
        return nullptr;
      llvm::Value *Acc = get(Ops[0]);
      for (size_t I = 1; I < Ops.size() && Acc; ++I) {
        llvm::Value *Rhs = get(Ops[I]);
        Acc = Rhs ? Make(Acc, Rhs) : nullptr;
      }
      return Acc;
    };

    llvm::Value *Result = nullptr;
    switch (Ctx.op(Item.Ref)) {
    case sym::SymOp::Const:
      Result = llvm::ConstantInt::get(Ty, Ctx.constValue(Item.Ref));
      break;
    case sym::SymOp::Var:
      // Every variable in the result came from the way in; a miss would mean
      // the solver returned one of its own placeholders, which it never does.
      Result = Sources.lookup(Index);
      break;
    case sym::SymOp::Add:
      Result = reduce(
          [&](llvm::Value *X, llvm::Value *Y) { return B.CreateAdd(X, Y); });
      break;
    case sym::SymOp::Mul: {
      // Negation is stored as a product with -1; `-x` reads better than `-1 *
      // x` and folds the same.
      if (Ops.size() == 2 && Ctx.isConstOnes(Ops[0])) {
        llvm::Value *X = get(Ops[1]);
        Result = X ? B.CreateNeg(X) : nullptr;
      } else {
        Result = reduce(
            [&](llvm::Value *X, llvm::Value *Y) { return B.CreateMul(X, Y); });
      }
      break;
    }
    case sym::SymOp::And:
      Result = reduce(
          [&](llvm::Value *X, llvm::Value *Y) { return B.CreateAnd(X, Y); });
      break;
    case sym::SymOp::Or:
      Result = reduce(
          [&](llvm::Value *X, llvm::Value *Y) { return B.CreateOr(X, Y); });
      break;
    case sym::SymOp::Xor:
      Result = reduce(
          [&](llvm::Value *X, llvm::Value *Y) { return B.CreateXor(X, Y); });
      break;
    case sym::SymOp::Not: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      Result = X ? B.CreateNot(X) : nullptr;
      break;
    }
    case sym::SymOp::Shl: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateShl(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::LShr: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateLShr(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::AShr: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateAShr(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::UDiv: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateUDiv(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::SDiv: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateSDiv(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::URem: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateURem(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::SRem: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *Y = get(Ctx.operand(Item.Ref, 1));
      Result = X && Y ? B.CreateSRem(X, Y) : nullptr;
      break;
    }
    case sym::SymOp::Extract: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      if (X) {
        const uint32_t Low = static_cast<uint32_t>(Ctx.node(Item.Ref).Aux);
        if (Low != 0)
          X = B.CreateLShr(X, llvm::ConstantInt::get(X->getType(), Low));
        Result = B.CreateTrunc(X, Ty);
      }
      break;
    }
    case sym::SymOp::ZExt: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      Result = X ? B.CreateZExt(X, Ty) : nullptr;
      break;
    }
    case sym::SymOp::SExt: {
      llvm::Value *X = get(Ctx.operand(Item.Ref, 0));
      Result = X ? B.CreateSExt(X, Ty) : nullptr;
      break;
    }
    case sym::SymOp::Concat: {
      llvm::Value *Acc = get(Ops.front());
      if (Acc)
        Acc = B.CreateZExt(Acc, Ty);
      for (sym::SymRef Op : Ops.drop_front()) {
        llvm::Value *V = get(Op);
        if (!Acc || !V) {
          Acc = nullptr;
          break;
        }
        Acc = B.CreateShl(Acc, Ctx.width(Op));
        Acc = B.CreateOr(Acc, B.CreateZExt(V, Ty));
      }
      Result = Acc;
      break;
    }
    case sym::SymOp::Ite: {
      llvm::Value *C = get(Ctx.operand(Item.Ref, 0));
      llvm::Value *T = get(Ctx.operand(Item.Ref, 1));
      llvm::Value *E = get(Ctx.operand(Item.Ref, 2));
      Result = C && T && E ? B.CreateSelect(C, T, E) : nullptr;
      break;
    }
    default:
      // Rotates and comparisons have no output spelling in this translator.
      Result = nullptr;
      break;
    }
    Built[Index] = Result;
  }
  return Built.lookup(R.index());
}

} // namespace neverd
