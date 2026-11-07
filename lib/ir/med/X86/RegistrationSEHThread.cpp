//===- RegistrationSEHThread.cpp - Current PE32 thread receipts
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share exact source occurrence checks across physical replay and emitters.
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86RegistrationSEHThread.h"

#include <algorithm>
#include <set>

namespace neverd {

bool isX86RegistrationThreadSelfLoad(const MedOp &Op,
                                     const RegistrationStateAnalysis &States) {
  using Thread = RegistrationSEHThreadAccess;
  if (!States.SEHThreadAccessesComplete || Op.Opcode != NdOp::LOAD ||
      Op.NumInputs != 1 || Op.Output.Size != Thread::WordBytes ||
      !Thread::isAddressWidth(Op.Inputs[0].Size) ||
      (Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal != Thread::SelfOffset) ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::X86FS ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  const auto Identity = std::make_pair(Op.Addr, Op.OriginSeq);
  const auto Found = std::lower_bound(
      States.SEHThreadAccesses.begin(), States.SEHThreadAccesses.end(),
      Identity, [](const auto &Access, auto Key) {
        return std::make_pair(Access.Address, Access.OpSeq) < Key;
      });
  return Found != States.SEHThreadAccesses.end() && Found->Address == Op.Addr &&
         Found->OpSeq == Op.OriginSeq && Found->Field == Thread::Kind::Self;
}

std::optional<X86RegistrationSEHThreadAccesses>
getX86RegistrationSEHThreadAccesses(const MedFunc &Function) {
  using Thread = RegistrationSEHThreadAccess;
  if (!Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      !Function.RegistrationStates ||
      !Function.RegistrationStates->SEHThreadAccessesComplete ||
      (Function.ExceptionMetadata->Cxx &&
       !Function.RegistrationStates->SEHThreadAccesses.empty()))
    return std::nullopt;
  X86RegistrationSEHThreadAccesses Result;
  for (const auto &Access : Function.RegistrationStates->SEHThreadAccesses)
    if (Access.OpSeq < 0 || Access.RootOpSeq < 0 ||
        Access.Field > Thread::Kind::StackBase ||
        !Result.emplace(std::make_pair(Access.Address, Access.OpSeq), Access)
             .second)
      return std::nullopt;
  for (const auto &[Identity, Access] : Result) {
    const auto Root = Result.find({Access.RootAddress, Access.RootOpSeq});
    if (Root == Result.end() || Root->second.Field != Thread::Kind::Self ||
        (Access.Field == Thread::Kind::Self && Root->first != Identity))
      return std::nullopt;
  }
  std::set<std::pair<va_t, int>> Seen;
  size_t Work = Result.size();
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      const auto Identity = std::make_pair(Op.Addr, Op.OriginSeq);
      const auto Found = Result.find(Identity);
      if (Found == Result.end())
        continue;
      if (Op.Dead || Op.Opcode != NdOp::LOAD || Op.NumInputs != 1 ||
          !Thread::isAddressWidth(Op.Inputs[0].Size) ||
          Op.Output.Size != Thread::WordBytes ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          (Found->second.Field == Thread::Kind::Self
               ? !isX86RegistrationThreadSelfLoad(Op,
                                                  *Function.RegistrationStates)
               : Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) ||
          !Seen.insert(Identity).second)
        return std::nullopt;
    }
  if (Seen.size() != Result.size())
    return std::nullopt;
  return Result;
}

} // namespace neverd
