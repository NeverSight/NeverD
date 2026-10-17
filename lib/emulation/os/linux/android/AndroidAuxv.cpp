//===- AndroidAuxv.cpp - Explicit Bionic auxiliary vector queries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include <set>

namespace neverd::emulation::android_model {
llvm::Error validateAuxiliaryVector(const AndroidNativeOptions &Options) {
  if (!Options.AuxiliaryVector)
    return llvm::Error::success();
  if (Options.AuxiliaryVector->size() > 128)
    return failure(diagnostic::AuxiliaryVectorInput);
  std::set<uint64_t> Types;
  for (const auto &Entry : *Options.AuxiliaryVector)
    if (!Entry.Type || !Types.insert(Entry.Type).second)
      return failure(diagnostic::AuxiliaryVectorInput);
  return llvm::Error::success();
}

BionicResult Bionic::queryAuxiliaryVector(const NativeCallEvent &Call) {
  const auto &Entries = Options.Android->AuxiliaryVector;
  if (!Entries) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = diagnostic::AuxiliaryVectorUnspecified;
    return std::optional<BionicValue>();
  }
  // API 28 takes unsigned long for both the type and result. A present zero
  // preserves errno; only absence in the complete vector sets ENOENT. The
  // implicit AT_NULL terminator is not a searchable entry.
  for (const auto &Entry : *Entries)
    if (Entry.Type == Call.Arguments[0])
      return value(Entry.Value);
  if (auto E = setErrno(linux_model::NoEntry))
    return std::move(E);
  return value(0);
}
} // namespace neverd::emulation::android_model
