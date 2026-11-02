//===- COFFRegistrationSEHException.cpp - SEH runtime objects
//--------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationSEHException.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHException.h"

namespace neverd::coff_registration {

llvm::Expected<RegistrationSEHExceptionContract> bindSEHExceptionReads(
    const MedFunc &Source, const RegistrationFrame &Frame,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    const std::map<const llvm::Function *, const llvm::StoreInst *> &Bridges,
    llvm::ArrayRef<const llvm::Function *> Functions) {
  const auto Expected = getX86RegistrationSEHExceptionAccesses(Source);
  if (!Expected || !Frame.Slot || Frame.EntrySP < 24)
    return rejectIR("source SEH exception fields have no checked identities");
  RegistrationSEHExceptionContract Result;
  auto NullReads = bindSEHNullReads(Source, Functions);
  if (!NullReads)
    return NullReads.takeError();
  Result.NullReads = std::move(*NullReads);
  Result.Frame = Frame.Slot;
  Result.PointersOffset = Frame.EntrySP - 24;
  Result.Bridges = Bridges;
  for (const auto &[Function, Bridge] : Bridges) {
    Result.Roots.insert(Bridge);
    Result.Roots.insert(Bridge->getValueOperand());
  }
  std::set<std::pair<va_t, int>> Seen;
  size_t Work = 0;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("SEH exception field replay exceeds its work budget");
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!MD)
          continue;
        const auto Address = metadataInteger(*MD, 1, 64);
        const auto Seq = metadataInteger(*MD, 2, 32);
        if (!Address || !Seq || *Seq > INT_MAX)
          return rejectIR("SEH exception field occurrence is malformed");
        const auto Identity = std::make_pair(*Address, int(*Seq));
        const auto Access = Expected->find(Identity);
        if (Access == Expected->end())
          continue;
        const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
        const auto Owner = Callbacks.find({Access->second.FilterVA, false});
        const auto Bridge = Bridges.find(Function);
        if (!Load || Owner == Callbacks.end() || Owner->second != Function ||
            Bridge == Bridges.end() || MD->getNumOperands() != 5 ||
            metadataInteger(*MD, 0, 64) != Source.Entry ||
            metadataInteger(*MD, 4, 8) != 0 || !Seen.insert(Identity).second)
          return rejectIR("SEH exception read lost its source filter");
        Result.Reads.emplace(Load, RegistrationSEHExceptionContract::Access{
                                       Access->second.Field, Bridge->second});
      }
  if (Seen.size() != Expected->size())
    return rejectIR("SEH exception read set is incomplete");
  return Result;
}

} // namespace neverd::coff_registration
