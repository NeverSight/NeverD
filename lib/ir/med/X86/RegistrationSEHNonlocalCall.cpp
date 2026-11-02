//===- RegistrationSEHNonlocalCall.cpp - Nonlocal finally exits ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds nested finally invocations to their checked nonlocal continuations.

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/ir/med/X86/X86RegistrationSEHJump.h"
#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"

#include <set>

namespace neverd {

std::optional<std::map<std::tuple<int, va_t, int>, va_t>>
getRegistrationSEHNonlocalCalls(const MedFunc &Function) {
  if (!Function.RegistrationStates ||
      !Function.RegistrationStates->SEHInvocationExitsComplete)
    return std::nullopt;
  const auto Jumps = getX86RegistrationSEHJumpCalls(Function);
  const auto ParentExits = getRegistrationSEHParentExits(Function);
  if (!Jumps || !ParentExits)
    return std::nullopt;
  std::map<std::tuple<int, va_t, int>, va_t> Result;
  if (Jumps->empty() && ParentExits->empty())
    return Result;
  std::set<va_t> FinallyEntries;
  for (const auto &Scope : Function.ExceptionMetadata->Registration->Scopes)
    if (Scope.IsFinally)
      FinallyEntries.insert(Scope.HandlerVA);
  std::map<va_t, va_t> Invocations;
  size_t Work = 0;
  for (const auto &[Id, Exit] : *ParentExits)
    for (const auto *Invocation : Exit.Invocations) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (!Invocation->CallVA)
        continue;
      const auto [It, New] =
          Invocations.emplace(Invocation->CallVA, Exit.EntryVA);
      if (!New && It->second != Exit.EntryVA)
        return std::nullopt;
    }
  for (const auto &Resume : Function.RegistrationStates->SEHJumpResumptions) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    if (!Resume.OrdinaryCall)
      continue;
    // The jump binder checks the exact CALL, physical near-call contract,
    // callback root, active buffer, and complete continuation state mapping.
    if (!FinallyEntries.count(Resume.EntryVA) || !Jumps->count(Resume.Address))
      return std::nullopt;
    const auto [It, New] =
        Invocations.emplace(Resume.OrdinaryCall, Resume.EntryVA);
    if (!New && It->second != Resume.EntryVA)
      return std::nullopt;
  }
  std::set<va_t> Seen;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Op.Opcode != NdOp::CALL)
        continue;
      const auto Found = Invocations.find(Op.Addr);
      if (Found == Invocations.end())
        continue;
      if (!Seen.insert(Op.Addr).second || Op.Dead || Op.OriginSeq < 0 ||
          Op.ExactArguments || Op.SourceCallHint || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
          Op.Inputs[0].ConstVal != Found->second ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.MemoryOrdering != NdMemoryOrdering::None)
        return std::nullopt;
      Result.emplace(std::make_tuple(Block.Id, Op.Addr, Op.OriginSeq),
                     Found->second);
    }
  if (Seen.size() != Invocations.size())
    return std::nullopt;
  return Result;
}

} // namespace neverd
