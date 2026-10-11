//===- DarwinEntropy.h - Explicit finite entropy replay --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINENTROPY_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINENTROPY_H

#include "DarwinKernel.h"

namespace neverd::emulation::darwin_model {
/// One workload's immutable supplied bytes and private next-record cursor.
/// These observations are neither a host RNG nor a cryptographic guarantee.
class DarwinEntropy final {
public:
  explicit DarwinEntropy(const std::optional<DarwinSystemOptions> &Options)
      : Options(Options) {}
  DarwinEntropy(const DarwinEntropy &) = delete;
  DarwinEntropy &operator=(const DarwinEntropy &) = delete;

  llvm::Expected<std::optional<ServiceResult>>
  handle(GuestMemory &Memory, const ProcessServiceEvent &Event,
         ProcessResult &Result);

private:
  const std::optional<DarwinSystemOptions> &Options;
  size_t Next = 0;
};
} // namespace neverd::emulation::darwin_model
#endif
