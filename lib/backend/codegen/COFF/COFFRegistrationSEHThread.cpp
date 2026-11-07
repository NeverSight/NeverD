//===- COFFRegistrationSEHThread.cpp - Thread read replay -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Metadata locates occurrences; source replay and actual IR prove accesses.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationSEHThread.h"

#include "COFFRegistrationIRProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86RegistrationSEHThread.h"

namespace neverd::coff_registration {
llvm::Expected<std::set<const llvm::LoadInst *>>
bindSEHThreadReads(const MedFunc &Source, const llvm::Function &Parent,
                   llvm::ArrayRef<const llvm::Function *> Functions) {
  const auto Expected = getX86RegistrationSEHThreadAccesses(Source);
  if (!Expected)
    return rejectIR("source thread reads have no checked identities");
  std::map<std::pair<va_t, int>, const llvm::Instruction *> Operations;
  size_t Work = 0;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("thread read replay exceeds its work budget");
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!MD)
          continue;
        const auto Address = metadataInteger(*MD, 1, 64);
        const auto Seq = metadataInteger(*MD, 2, 32);
        if (!Address || !Seq || *Seq > INT_MAX)
          return rejectIR("thread read occurrence is malformed");
        const auto Identity = std::make_pair(*Address, int(*Seq));
        if (!Expected->count(Identity))
          continue;
        if (Function != &Parent || MD->getNumOperands() != 5 ||
            metadataInteger(*MD, 0, 64) != Source.Entry ||
            metadataInteger(*MD, 4, 8) != 0 ||
            !Operations.emplace(Identity, &I).second)
          return rejectIR("thread read lost its unique source occurrence");
      }
  auto Reads = getCheckedX86SEHThreadReads(Source, Parent, Operations, true);
  if (!Reads)
    return rejectIR("thread read changed its runtime field or Self pointer");
  return std::move(*Reads);
}
} // namespace neverd::coff_registration
