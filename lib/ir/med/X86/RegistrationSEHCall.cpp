//===- RegistrationSEHCall.cpp - Checked SEH finally call ABI ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind returning SEH finally invocations to their current MedIR continuations.

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"

#include <set>
#include <tuple>

namespace neverd {

std::optional<RegistrationSEHReturns>
getRegistrationSEHReturns(const MedFunc &Function) {
  if (!Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Cxx ||
      Function.ExceptionMetadata->ParseStatus !=
          ExceptionParseStatus::Complete ||
      (Function.ExceptionMetadata->Encoding !=
           ExceptionEncoding::X86ScopeTableEH3 &&
       Function.ExceptionMetadata->Encoding !=
           ExceptionEncoding::X86ScopeTableEH4) ||
      !Function.RegistrationStates || Function.SkippedSSA ||
      !Function.RegistrationStates->Complete ||
      !Function.RegistrationStates->CallbackStatesComplete ||
      !Function.RegistrationStates->RegistrationLifetimeComplete ||
      !Function.RegistrationStates->SEHInvocationExitsComplete)
    return std::nullopt;
  const auto &Exits = Function.RegistrationStates->SEHInvocationExits;
  if (Exits.size() > limits::kMaxRegistrationEHStateWork ||
      Function.ExceptionMetadata->Registration->Scopes.size() >
          limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  RegistrationSEHReturns Result;
  if (Exits.empty())
    return Result;
  // Near-return ABI recovery is independent of native parent-jump projection.
  // Each parent exit is checked separately by its own binding and native gate.
  std::set<va_t> Entries;
  for (const auto &Scope : Function.ExceptionMetadata->Registration->Scopes)
    if (Scope.IsFinally)
      Entries.insert(Scope.HandlerVA);
  std::map<int, const MedBlock *> Blocks;
  std::map<va_t, std::pair<const MedBlock *, const MedOp *>> Calls;
  std::map<std::pair<va_t, int>, const MedBlock *> Returns;
  size_t Work = Exits.size();
  for (const auto &Block : Function.Blocks) {
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        !Blocks.emplace(Block.Id, &Block).second)
      return std::nullopt;
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Op.Opcode == NdOp::CALL &&
          !Calls.emplace(Op.Addr, std::make_pair(&Block, &Op)).second)
        return std::nullopt;
      if (Op.Opcode == NdOp::RETURN &&
          (Op.Dead || &Op != &Block.Ops.back() || !Block.Succs.empty() ||
           !Returns.emplace(std::make_pair(Op.Addr, Op.OriginSeq), &Block)
                .second))
        return std::nullopt;
    }
  }
  std::set<std::tuple<va_t, va_t, va_t, int>> Seen;
  for (const auto &Exit : Exits) {
    if (Exit.isParentExit())
      continue;
    const auto Call = Calls.find(Exit.CallVA);
    const auto Return = Returns.find({Exit.Address, Exit.OpSeq});
    if (Exit.Exit != RegistrationSEHInvocationExit::Kind::NearReturn ||
        !Exit.CallVA || !Entries.count(Exit.EntryVA) || Exit.OpSeq < 0 ||
        Exit.StackRestoreVA != InvalidVA || Exit.StackRestoreSeq != -1 ||
        Call == Calls.end() || Return == Returns.end() ||
        Return->second->EndAddr != Exit.EndAddress ||
        !Seen.emplace(Exit.EntryVA, Exit.CallVA, Exit.Address, Exit.OpSeq)
             .second)
      return std::nullopt;
    const auto &[Caller, Operation] = Call->second;
    if (Operation->Dead || Operation->DoesNotReturn ||
        Operation->OriginSeq < 0 || Operation->ExactArguments ||
        Operation->SourceCallHint ||
        Operation->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Operation->MemoryOrdering != NdMemoryOrdering::None ||
        Operation->NumInputs != 1 || !Operation->Inputs[0].isConst() ||
        Operation->Inputs[0].Size != 4 ||
        Operation->Inputs[0].ConstVal != Exit.EntryVA ||
        Caller->Succs.size() != 1 || !Blocks.count(Caller->Succs.front()) ||
        Blocks.at(Caller->Succs.front())->StartAddr != Exit.TargetVA)
      return std::nullopt;
    auto [It, Inserted] =
        Result.ReturnBlocks.emplace(Return->second->Id, Exit.EntryVA);
    if (!Inserted && It->second != Exit.EntryVA)
      return std::nullopt;
    const auto Key =
        std::make_tuple(Caller->Id, Operation->Addr, Operation->OriginSeq);
    auto [CallIt, NewCall] = Result.ReturningCalls.emplace(Key, Exit.EntryVA);
    if (!NewCall && CallIt->second != Exit.EntryVA)
      return std::nullopt;
  }
  return Result;
}

} // namespace neverd
