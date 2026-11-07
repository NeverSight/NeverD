//===- RegistrationObjectCallbackABI.cpp - PE32 object graphs -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove bounded object call graphs while keeping child results unobserved.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ScopeExit.h"

namespace neverd::registration_abi {

std::optional<RegistrationLeafCalleeABI>
checkedObjectCallbackCalleeABI(const BinaryImage &Image, va_t Target,
                               size_t &Work, ObjectCallbackContext &Context) {
  if (!chargeCalleeWork(Work, 1) ||
      !isPlainRegistrationCalleeEntry(Image, Target) ||
      Context.Depth >= kMaxObjectCallbackDepth)
    return std::nullopt;
  if (const auto It = Context.Callees.find(Target);
      It != Context.Callees.end()) {
    if (It->second) {
      const auto &ABI = *It->second;
      if (!chargeCalleeWork(Work, ABI.ECXReads.size() + ABI.ECXWrites.size() +
                                      ABI.ImageReads.size() +
                                      ABI.ImageWrites.size() +
                                      ABI.CodeRanges.size()))
        return std::nullopt;
    }
    return It->second;
  }
  if (Context.Callees.size() >= kMaxObjectCallbackNodes)
    return std::nullopt;
  // A cached failure also marks an in-progress node. Direct and mutual cycles
  // cannot acquire a returning contract from their own unfinished proof.
  auto &Cached = Context.Callees.emplace(Target, std::nullopt).first->second;
  ++Context.Depth;
  const llvm::scope_exit Leave([&] { --Context.Depth; });
  const auto Pop = getCheckedX86CalleeStackPop(Image, Target, &Work);
  if (!Pop || *Pop > UINT16_MAX || *Pop % 4)
    return std::nullopt;
  auto ABI = checkedLeafCalleeABI(Image, Target, Work,
                                  RegistrationLeafResult::Discarded, 0, nullptr,
                                  nullptr, nullptr, &Context, *Pop);
  if (!ABI || !ABI->CallerPCWrites.empty())
    return std::nullopt;
  for (const auto &Range : ABI->CodeRanges) {
    const auto *Owner = Image.getSegmentFor(Range.Begin);
    if (!chargeCalleeWork(Work, 1) || !Owner || !Owner->isReadable() ||
        !Owner->isExecutable() ||
        (Owner->isWritable() && !Owner->ReadOnlyAfterRelocations) ||
        Range.End - Owner->VA > Owner->Size)
      return std::nullopt;
  }
  for (const auto &Write : ABI->ImageWrites) {
    const auto *Owner = Image.getSegmentFor(Write.Begin);
    if (!chargeCalleeWork(Work, 1) || !Owner || !Owner->isWritable() ||
        Owner->isExecutable() || Owner->ReadOnlyAfterRelocations)
      return std::nullopt;
  }
  Cached = std::move(*ABI);
  return Cached;
}

} // namespace neverd::registration_abi

namespace neverd {

std::optional<RegistrationLeafCalleeABI>
getCheckedX86RegistrationObjectCalleeABI(const BinaryImage &Image, va_t Target,
                                         size_t *CumulativeWork,
                                         RegistrationLeafResult ResultKind) {
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  // Keep the existing leaf contract, including its caller-PC policy. A graph
  // receives the stronger transitive proof below instead of leaf authority.
  if (auto Leaf = registration_abi::checkedLeafCalleeABI(Image, Target, Work,
                                                         ResultKind))
    return Leaf;
  registration_abi::ObjectCallbackContext Context;
  auto ABI = registration_abi::checkedObjectCallbackCalleeABI(Image, Target,
                                                              Work, Context);
  if (!ABI || ABI->StackPopBytes ||
      (ResultKind == RegistrationLeafResult::Observed &&
       !ABI->HasIndependentScalarReturn))
    return std::nullopt;
  return ABI;
}

} // namespace neverd
