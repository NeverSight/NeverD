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
//===----------------------------------------------------------------------===//

#include "SymSimplifyDetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/ConstantFolder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/ErrorHandling.h"

#include <cstdint>

namespace neverd {

namespace {

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
  return !llvm::canCreatePoison(llvm::cast<llvm::Operator>(&I));
}

/// Whether a translatable value hidden behind an opaque shared leaf contains
/// semantics the total bitvector engine cannot preserve.
///
/// Ordinary translation visits every operand and rejects the whole candidate
/// when it reaches undef, poison, or a poison-generating instruction.  A value
/// with multiple uses deliberately stays opaque, though, so its operands would
/// otherwise escape that check.  Follow the defining operand graph only to
/// reject unsafe candidates; this never makes an opaque operation
/// translatable.
bool hasHiddenIncompatibleSemantics(const llvm::Instruction &Root,
                                    bool WithComparisons) {
  llvm::SmallVector<const llvm::Value *, 8> Work;
  for (const llvm::Use &Op : Root.operands())
    Work.push_back(Op.get());

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
    if (isTranslatable(I, WithComparisons) && !hasCompatibleSemantics(*I))
      return true;
    for (const llvm::Use &Op : I->operands())
      Work.push_back(Op.get());
  }
  return false;
}

} // namespace

OpTag tagOf(const llvm::Instruction &I) {
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
  case OpTag::None:
    break;
  }
  llvm_unreachable("build called on an instruction with no operator tag");
}

sym::SymRef Translator::in(llvm::Value *Root) {
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
        I && isTranslatable(I, CarryComparisons) && !hasCompatibleSemantics(*I))
      return {};

    if (!descend(V, V == Root)) {
      if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
          I && hasHiddenIncompatibleSemantics(*I, CarryComparisons))
        return {};
      Memo[V] = leaf(V);
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
    ++NumDescended;
  }
  return Memo.lookup(Root);
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
      llvm::Value *Acc = llvm::ConstantInt::get(Ty, 0);
      for (sym::SymRef Op : Ops) {
        llvm::Value *V = get(Op);
        if (!V) {
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
      // Rotates and comparisons never come back out, because nothing on the way
      // in ever puts one in.
      Result = nullptr;
      break;
    }
    Built[Index] = Result;
  }
  return Built.lookup(R.index());
}

} // namespace neverd
