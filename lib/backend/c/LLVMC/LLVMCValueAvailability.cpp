//===- LLVMCValueAvailability.cpp - Final LLVM-C availability paths -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Describe the normalized LLVM function and the writer's actual flag
/// assignments to the shared availability proof. This never changes IR,
/// payload transport, or the guards of other observations.
//===----------------------------------------------------------------------===//

#include "LLVMCWriter.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Attributes.h"
#include "llvm/Support/ErrorHandling.h"

#include <utility>

namespace neverd {
namespace {

c_availability::Value identity(const llvm::Value *V) {
  return reinterpret_cast<c_availability::Value>(V);
}

struct BranchComparison {
  const llvm::ICmpInst *Compare;
  bool Inverted = false;
};

std::optional<BranchComparison>
branchComparison(const llvm::BasicBlock &Block) {
  const auto *Branch = llvm::dyn_cast<llvm::CondBrInst>(Block.getTerminator());
  if (!Branch)
    return std::nullopt;
  const auto *Compare = llvm::dyn_cast<llvm::ICmpInst>(Branch->getCondition());
  if (!Compare || !Compare->getOperand(0)->getType()->isIntegerTy())
    return std::nullopt;
  bool Inverted = false;
  while (Compare->isEquality()) {
    const llvm::Value *Boolean = Compare->getOperand(0);
    const llvm::Value *Constant = Compare->getOperand(1);
    if (llvm::isa<llvm::ConstantInt>(Boolean))
      std::swap(Boolean, Constant);
    const auto *Zero = llvm::dyn_cast<llvm::ConstantInt>(Constant);
    if (!Zero || !Zero->isZero())
      break;
    // Both zero- and sign-extension preserve the zero/nonzero distinction
    // of an i1 comparison. A truncated integer is not such a comparison.
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Boolean);
        Cast && llvm::isa<llvm::ZExtInst, llvm::SExtInst>(Cast) &&
        Cast->getSrcTy()->isIntegerTy(1))
      Boolean = Cast->getOperand(0);
    const auto *Inner = llvm::dyn_cast<llvm::ICmpInst>(Boolean);
    if (!Inner || !Inner->getOperand(0)->getType()->isIntegerTy())
      break;
    Inverted ^= Compare->getPredicate() == llvm::CmpInst::ICMP_EQ;
    Compare = Inner;
  }
  return BranchComparison{Compare, Inverted};
}

std::pair<const llvm::Value *, const llvm::Value *>
comparisonOperands(const llvm::ICmpInst &Compare) {
  const llvm::Value *Left = Compare.getOperand(0);
  const llvm::Value *Right = Compare.getOperand(1);
  if (Compare.isEquality()) {
    if (const auto *Zero = llvm::dyn_cast<llvm::ConstantInt>(Left);
        Zero && Zero->isZero())
      std::swap(Left, Right);
    const auto *Difference = llvm::dyn_cast<llvm::BinaryOperator>(Left);
    const auto *Zero = llvm::dyn_cast<llvm::ConstantInt>(Right);
    // For the exact bit width, sub/xor is zero iff its inputs are equal.
    // Do not peel truncations, extensions, freeze or other arithmetic: a
    // narrowed zero test, for example, can equate different whole words.
    if (Zero && Zero->isZero() && Difference &&
        (Difference->getOpcode() == llvm::Instruction::Sub ||
         Difference->getOpcode() == llvm::Instruction::Xor)) {
      Left = Difference->getOperand(0);
      Right = Difference->getOperand(1);
    }
  }
  return {Left, Right};
}

c_availability::Predicate edgePredicate(const BranchComparison &Branch,
                                        unsigned Successor) {
  using namespace c_availability;
  const auto *Compare = Branch.Compare;
  const auto Kind = bool(Successor) != Branch.Inverted
                        ? Compare->getInversePredicate()
                        : Compare->getPredicate();
  const auto [Left, Right] = comparisonOperands(*Compare);
  uint8_t Mask;
  switch (Kind) {
  case llvm::CmpInst::ICMP_EQ:
    Mask = Equal;
    break;
  case llvm::CmpInst::ICMP_NE:
    Mask = Less | Greater;
    break;
  case llvm::CmpInst::ICMP_ULT:
  case llvm::CmpInst::ICMP_SLT:
    Mask = Less;
    break;
  case llvm::CmpInst::ICMP_ULE:
  case llvm::CmpInst::ICMP_SLE:
    Mask = Less | Equal;
    break;
  case llvm::CmpInst::ICMP_UGT:
  case llvm::CmpInst::ICMP_SGT:
    Mask = Greater;
    break;
  case llvm::CmpInst::ICMP_UGE:
  case llvm::CmpInst::ICMP_SGE:
    Mask = Greater | Equal;
    break;
  default:
    llvm_unreachable("integer comparison predicate");
  }
  return Predicate{identity(Left), identity(Right),
                   Left->getType()->getIntegerBitWidth(), Mask,
                   llvm::CmpInst::isSigned(Kind)};
}

} // namespace

c_availability::Assignment
LLVMCWriter::deferredIncomingAssignment(const llvm::Instruction &Join,
                                        const llvm::Value *Incoming) const {
  using namespace c_availability;
  Assignment A;
  A.Destination = identity(&Join);
  if (isDeferredUnknownInput(Incoming)) {
    A.Availability = FlagWrite::Unknown;
    return A;
  }
  A.Source = identity(Incoming);
  A.CopyBits = Join.getType()->getIntegerBitWidth();
  A.Availability =
      DeferredUnknownFlags.count(Incoming) ? FlagWrite::Copy : FlagWrite::Known;
  return A;
}

std::optional<c_availability::Assignment>
LLVMCWriter::deferredInstructionAssignment(
    const llvm::Instruction &Inst) const {
  using namespace c_availability;
  if (!DeferredUnknownFlags.count(&Inst) || llvm::isa<llvm::PHINode>(Inst))
    return std::nullopt;
  Assignment A;
  A.Destination = identity(&Inst);
  // The printed select sets its flag from the chosen arm at runtime. A
  // straight-line availability proof cannot assume either arm was chosen.
  if (llvm::isa<llvm::SelectInst>(Inst) || isDeferredUnknownInput(&Inst)) {
    A.Availability = FlagWrite::Unknown;
    return A;
  }
  const auto *Source = deferredScalarSource(Inst);
  if (!Source || !DeferredUnknownFlags.count(Source))
    return std::nullopt;
  A.Source = identity(Source);
  // Casts and a literal XOR transfer availability. Neither proves the full
  // payload equal to its input for a later branch comparison.
  A.Availability = FlagWrite::Copy;
  return A;
}

void LLVMCWriter::buildAvailabilityFlow(const llvm::Function &Fn) {
  using namespace c_availability;
  Graph &Flow = AvailabilityFlow.emplace();
  Flow.Nodes.resize(Fn.size());
  llvm::DenseMap<const llvm::BasicBlock *, NodeIndex> Blocks;
  llvm::DenseMap<const llvm::BasicBlock *, BranchComparison> Comparisons;
  Blocks.reserve(Fn.size());
  size_t Index = 0;
  for (const auto &Block : Fn) {
    Blocks[&Block] = Index++;
    if (auto Compare = branchComparison(Block))
      Comparisons.try_emplace(&Block, *Compare);
  }

  // Only these identities can enter predicate facts. Exact PHI copies can
  // introduce their incoming values while walking backwards; ordinary
  // computations instead kill the fact. Index that closure once, so live
  // values unrelated to any predicate cannot exhaust the proof's work.
  llvm::DenseSet<const llvm::Value *> PredicateValues;
  llvm::SmallVector<const llvm::PHINode *> PendingPhis;
  auto AddPredicateValue = [&](const llvm::Value *V) {
    if (PredicateValues.insert(V).second)
      if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(V))
        PendingPhis.push_back(Phi);
  };
  for (const auto &Block : Fn) {
    const auto At = Comparisons.find(&Block);
    if (At == Comparisons.end())
      continue;
    const auto [Left, Right] = comparisonOperands(*At->second.Compare);
    AddPredicateValue(Left);
    AddPredicateValue(Right);
  }
  while (!PendingPhis.empty())
    for (const auto &Incoming : PendingPhis.pop_back_val()->incoming_values())
      AddPredicateValue(Incoming);

  AvailabilityPoints.reserve(Fn.getInstructionCount());
  for (const auto &Block : Fn) {
    Node &N = Flow.Nodes[Blocks.lookup(&Block)];
    // Detached analysis entries and exceptional/address-taken entries do
    // not inherit flags from an ordinary predecessor. Treating them as
    // fresh entries also keeps the proof conservative after EH layout.
    N.Entry = &Block == &Fn.getEntryBlock() || llvm::pred_empty(&Block) ||
              Block.isEHPad() || Block.hasAddressTaken();
    for (const auto &Inst : Block) {
      AvailabilityPoints[&Inst] = {Blocks.lookup(&Block), N.Assignments.size()};
      if (llvm::isa<llvm::IndirectBrInst, llvm::CallBrInst>(Inst))
        Flow.Complete = false;
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
          Call && Call->hasFnAttr(llvm::Attribute::ReturnsTwice))
        Flow.Complete = false;
      if (!Inst.getType()->isIntegerTy() || llvm::isa<llvm::PHINode>(Inst))
        continue;
      if (auto A = deferredInstructionAssignment(Inst))
        N.Assignments.push_back(*A);
      else if (PredicateValues.contains(&Inst))
        // Even omitted or inlined SSA computations must kill predicates
        // from a later iteration before the backward walk crosses them.
        N.Assignments.push_back(
            {identity(&Inst), std::nullopt, 0, FlagWrite::None});
    }
  }
  for (const auto &Block : Fn) {
    const auto *Terminator = Block.getTerminator();
    for (unsigned I = 0; I != Terminator->getNumSuccessors(); ++I) {
      const auto *Successor = Terminator->getSuccessor(I);
      Edge E;
      E.Predecessor = Blocks.lookup(&Block);
      if (const auto At = Comparisons.find(&Block); At != Comparisons.end())
        E.Condition = edgePredicate(At->second, I);
      for (const auto &Phi : Successor->phis()) {
        if (!Phi.getType()->isIntegerTy())
          continue;
        const auto *Incoming = Phi.getIncomingValueForBlock(&Block);
        if (DeferredUnknownFlags.count(&Phi))
          E.addCopy(deferredIncomingAssignment(Phi, Incoming));
        else if (PredicateValues.contains(&Phi))
          E.addCopy({identity(&Phi), identity(Incoming),
                     Phi.getType()->getIntegerBitWidth(), FlagWrite::None});
      }
      Flow.Nodes[Blocks.lookup(Successor)].Predecessors.push_back(std::move(E));
    }
  }
}

bool LLVMCWriter::deferredValueAvailable(const llvm::Value *Value) {
  if (!EmittingInstruction)
    return false;
  if (!AvailabilityFlow)
    buildAvailabilityFlow(*EmittingInstruction->getFunction());
  // Inline rendering always establishes the instruction's own context.
  // Consequently an InlineCache entry never imports a proof from its first
  // caller's stronger path conditions into a different caller.
  const auto At = AvailabilityPoints.find(EmittingInstruction);
  return At != AvailabilityPoints.end() &&
         c_availability::availableBefore(*AvailabilityFlow, At->second,
                                         identity(Value));
}

} // namespace neverd
