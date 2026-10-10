//===- RegistrationCleanupContext.cpp - PE32 cleanup invocation owner ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind each cleanup to the catch invocation still live at its unwind state.
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/med/X86RegistrationCallback.h"

namespace neverd {
std::optional<std::map<uint32_t, std::optional<RegistrationCatchIdentity>>>
registrationCleanupParents(const MedFunc &Function) {
  if (!registrationCatchBlocks(Function) ||
      !Function.RegistrationStates->CleanupFrameEffectsComplete)
    return std::nullopt;
  const auto &States = *Function.RegistrationStates;
  const auto &Cxx = *Function.ExceptionMetadata->Cxx;
  std::map<uint32_t, std::optional<RegistrationCatchIdentity>> Parents;
  std::map<int, const RegistrationBlockState *> Blocks;
  for (const auto &Block : States.Blocks)
    if (!Blocks.emplace(Block.BlockId, &Block).second)
      return std::nullopt;
  size_t Work = 0;
  for (const auto &Effect : States.CleanupFrameEffects) {
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        Effect.ActionState >= Cxx.UnwindMap.size() ||
        !Cxx.UnwindMap[Effect.ActionState].ActionVA ||
        !Blocks.count(Effect.BlockId))
      return std::nullopt;
    const auto &Block = *Blocks.at(Effect.BlockId);
    if (!Block.Reached || Block.Unknown ||
        Block.Range.Begin != Effect.Range.Begin ||
        Block.Range.End != Effect.Range.End ||
        !llvm::is_contained(Block.Levels, Effect.DispatchLevel))
      return std::nullopt;
    std::optional<RegistrationCatchIdentity> Parent;
    if (Block.CallbackOnly) {
      if (Block.CxxCatchStacks.size() != 1 || Block.CxxCatchStacks[0].empty())
        return std::nullopt;
      for (const auto &Identity : Block.CxxCatchStacks[0]) {
        if (++Work > limits::kMaxRegistrationEHStateWork ||
            Identity.first >= Cxx.TryBlocks.size())
          return std::nullopt;
        const auto &Try = Cxx.TryBlocks[Identity.first];
        if (Identity.second >= Try.Handlers.size())
          return std::nullopt;
        // Unwind may already have exited one or more source catches before
        // it reaches this action. Keep only guards whose state interval still
        // contains the action; the innermost remaining invocation owns it.
        if (int64_t(Effect.ActionState) > Try.TryHigh &&
            int64_t(Effect.ActionState) <= Try.CatchHigh)
          Parent = Identity;
      }
    }
    const auto [Found, New] = Parents.emplace(Effect.ActionState, Parent);
    if (!New && Found->second != Parent)
      return std::nullopt;
  }
  for (uint32_t State = 0; State < Cxx.UnwindMap.size(); ++State)
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        (Cxx.UnwindMap[State].ActionVA && !Parents.count(State)))
      return std::nullopt;
  return Parents;
}
} // namespace neverd
