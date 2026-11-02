//===- RegistrationSEHException.cpp - SEH field identities ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHException.h"
#include "neverd/ir/med/MedIR.h"

#include <set>

namespace neverd {

std::optional<X86RegistrationSEHExceptionAccesses>
getX86RegistrationSEHExceptionAccesses(const MedFunc &Function) {
  if (!Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      !Function.RegistrationStates ||
      !Function.RegistrationStates->SEHExceptionAccessesComplete)
    return std::nullopt;
  X86RegistrationSEHExceptionAccesses Result;
  std::set<va_t> Filters;
  for (const auto &Scope : Function.ExceptionMetadata->Registration->Scopes)
    if (!Scope.IsFinally)
      Filters.insert(Scope.FilterVA);
  for (const auto &Access : Function.RegistrationStates->SEHExceptionAccesses)
    if (Access.OpSeq < 0 || !Filters.count(Access.FilterVA) ||
        Access.Field > RegistrationSEHExceptionAccess::Kind::ExceptionFlags ||
        !Result.emplace(std::make_pair(Access.Address, Access.OpSeq), Access)
             .second)
      return std::nullopt;
  std::set<std::pair<va_t, int>> Seen;
  size_t Work = Result.size();
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      const auto Identity = std::make_pair(Op.Addr, Op.OriginSeq);
      if (!Result.count(Identity))
        continue;
      if (Op.Opcode != NdOp::LOAD || Op.NumInputs != 1 || Op.Output.Size != 4 ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          !Seen.insert(Identity).second)
        return std::nullopt;
    }
  if (Seen.size() != Result.size())
    return std::nullopt;
  return Result;
}

} // namespace neverd
