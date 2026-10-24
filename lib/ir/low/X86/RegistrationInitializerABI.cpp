//===- RegistrationInitializerABI.cpp - PE32 object initialization --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove scalar constructor arguments, return identity and object writes.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ScopeExit.h"

namespace neverd::registration_abi {

std::optional<RegistrationInitializerCalleeABI>
checkedInitializerCalleeABI(const BinaryImage &Image, va_t Target, size_t &Work,
                            InitializerContext &Context) {
  if (!chargeCalleeWork(Work, 1) ||
      !isPlainRegistrationCalleeEntry(Image, Target) ||
      Context.Depth >= kMaxInitializerGraphDepth)
    return std::nullopt;
  if (const auto It = Context.Callees.find(Target);
      It != Context.Callees.end()) {
    if (It->second) {
      const auto &ABI = It->second->Object;
      if (!chargeCalleeWork(
              Work, ABI.ECXReads.size() + ABI.ECXWrites.size() +
                        ABI.ImageReads.size() + ABI.ImageWrites.size() +
                        ABI.CodeRanges.size() + It->second->Initialized.size()))
        return std::nullopt;
    }
    return It->second;
  }
  if (Context.Callees.size() >= kMaxInitializerGraphNodes)
    return std::nullopt;
  // An in-progress entry has no contract, so direct and mutual recursion fail.
  auto &Cached = Context.Callees.emplace(Target, std::nullopt).first->second;
  ++Context.Depth;
  const llvm::scope_exit Leave([&] { --Context.Depth; });
  const auto Pop = getCheckedX86CalleeStackPop(Image, Target, &Work);
  if (!Pop || *Pop > UINT16_MAX || *Pop % 4)
    return std::nullopt;
  registration_abi::InitializerFrame Frame{*Pop, std::nullopt};
  auto Object = registration_abi::checkedLeafCalleeABI(
      Image, Target, Work, RegistrationLeafResult::Observed, 0, nullptr, &Frame,
      &Context);
  if (!Object || !Frame.Initialized || Frame.Initialized->empty() ||
      !Object->CallerPCWrites.empty())
    return std::nullopt;
  for (const auto &Range : Object->CodeRanges) {
    const auto *Owner = Image.getSegmentFor(Range.Begin);
    if (!registration_abi::chargeCalleeWork(Work, 1) || !Owner ||
        !Owner->isReadable() || !Owner->isExecutable() ||
        (Owner->isWritable() && !Owner->ReadOnlyAfterRelocations) ||
        Range.End - Owner->VA > Owner->Size)
      return std::nullopt;
  }
  for (const auto &Write : Object->ImageWrites) {
    const auto *Owner = Image.getSegmentFor(Write.Begin);
    if (!registration_abi::chargeCalleeWork(Work, 1) || !Owner ||
        !Owner->isWritable() || Owner->isExecutable() ||
        Owner->ReadOnlyAfterRelocations)
      return std::nullopt;
  }
  RegistrationInitializerCalleeABI Result;
  Result.Object = std::move(*Object);
  for (int32_t Byte : *Frame.Initialized) {
    if (!registration_abi::chargeCalleeWork(Work, 1))
      return std::nullopt;
    if (!Result.Initialized.empty() && Result.Initialized.back().End == Byte)
      ++Result.Initialized.back().End;
    else
      Result.Initialized.push_back({Byte, Byte + 1});
  }
  Cached = std::move(Result);
  return Cached;
}

} // namespace neverd::registration_abi

namespace neverd {
std::optional<RegistrationInitializerCalleeABI>
getCheckedX86RegistrationInitializerCalleeABI(const BinaryImage &Image,
                                              va_t Target,
                                              size_t *CumulativeWork) {
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  registration_abi::InitializerContext Context;
  return registration_abi::checkedInitializerCalleeABI(Image, Target, Work,
                                                       Context);
}
} // namespace neverd
