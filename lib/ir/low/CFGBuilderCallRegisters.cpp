//===- CFGBuilderCallRegisters.cpp - Decoded call preservation -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuses the call-register authority when a CFG proof crosses a small callee.

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/CallRegisterEffects.h"

#include "llvm/ADT/ScopeExit.h"

#include <map>
#include <vector>

namespace neverd {

std::optional<uint32_t> CFGBuilder::provedCalleePreservedGPRs(
    const LowOp &Call, const std::function<bool(size_t)> &Consume) const {
  if (SkipCalleeRegisterProofs || !CurrentImg ||
      CurrentImg->Arch != Arch::X64 || Call.Opcode != NdOp::CALL ||
      Call.NumInputs == 0 || !Call.Inputs[0].isConst() || !Consume(1))
    return std::nullopt;
  const va_t Target = Call.Inputs[0].Offset;
  if (auto It = CalleePreservedGPRs.find(Target);
      It != CalleePreservedGPRs.end())
    return It->second;
  if (CalleePreservedGPRs.size() >= limits::kMaxCFGCallRegisterProofFunctions)
    return std::nullopt;
  auto &Result = CalleePreservedGPRs[Target];
  bool BudgetComplete = true;
  const auto Finish = llvm::scope_exit([&] {
    // A later fixed-point stage may have a fresh allowance. Exhaustion is
    // neither a semantic refusal nor a reusable negative callee fact.
    if (!BudgetComplete)
      CalleePreservedGPRs.erase(Target);
  });
  const auto Spend = [&](size_t Work) {
    if (Consume(Work))
      return true;
    BudgetComplete = false;
    return false;
  };
  const BinaryImage &Image = *CurrentImg;
  std::map<va_t, LocalRegisterEffect> Effects;
  std::map<va_t, unsigned> Depths{{Target, 0}};
  std::vector<va_t> Pending{Target};
  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  for (size_t Next = 0; Next < Pending.size(); ++Next) {
    const va_t Entry = Pending[Next];
    if (Entry == CurrentFuncEntry || !Image.hasExecutableCodeOwnerAt(Entry))
      return std::nullopt;
    const va_t End = Image.getFunctionMetadataEnd(Entry, ExecutableCodeOwners);
    if (End == InvalidVA || End <= Entry ||
        End - Entry > limits::kMaxCFGCallRegisterProofBytes ||
        !Spend(End - Entry))
      return std::nullopt;
    CFGBuilder Builder;
    Builder.SkipCalleeRegisterProofs = true;
    Builder.setKnownFuncEntries(KnownFuncEntries);
    Builder.setAbsoluteRelocationRootIndex(AbsoluteRelocationRoots);
    Builder.setExecutableCodeOwnerIndex(ExecutableCodeOwners);
    // No no-return callback or shared cache can re-enter this in-progress
    // proof. Unmodelled tails and unresolved control retain an unknown effect.
    const LowFunc Body = Builder.buildIntegerFrameProof(Image, Dec, Entry, "");
    size_t Work = Body.Blocks.size();
    for (const LowBlock &Block : Body.Blocks)
      Work += Block.Ops.size();
    if (!Spend(Work))
      return std::nullopt;
    LocalRegisterEffect Effect = localRegisterEffect(Image, Body);
    if (Effect.Unknown || Effect.Incomplete)
      return std::nullopt;
    for (va_t Callee : Effect.Callees) {
      if (Depths.count(Callee))
        continue;
      const unsigned Depth = Depths.at(Entry) + 1;
      if (Depth > limits::kMaxCallEffectCalleeDepth ||
          Pending.size() >= limits::kMaxCFGCallRegisterProofFunctions)
        return std::nullopt;
      Depths.emplace(Callee, Depth);
      Pending.push_back(Callee);
    }
    Effects.emplace(Entry, std::move(Effect));
  }
  // Every admitted body has a complete, closed call graph. This is exactly
  // the register-effect solver used by ABI recovery, not an ABI/name guess.
  const auto Summary = solveCallRegisterEffects(Effects, ~uint32_t{0}, 0);
  if (auto It = Summary.MayWrite.find(Target); It != Summary.MayWrite.end())
    Result = ~It->second;
  return Result;
}

} // namespace neverd
