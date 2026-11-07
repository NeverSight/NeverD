//===- RegistrationImportLoad.h - Cached PE32 imports -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds complete MOV loads to current eager import storage and LowIR.

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONIMPORTLOAD_H
#define NEVERD_IR_LOW_X86_REGISTRATIONIMPORTLOAD_H

#include "neverd/Common.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd {
struct BinaryImage;
struct LowBlock;
struct LowInstructionBoundary;

namespace registration_abi {
enum class RegistrationImportLoadPolicy { CallableOnly, IncludeCRTData };

struct RegistrationImportLoad {
  enum class Kind { Callable, CRTData };
  va_t Slot = InvalidVA;
  int OpSeq = -1;
  uint64_t Register = 0;
  Kind Storage = Kind::Callable;
};

/// Match the complete current LowIR MOV, including its address and final GPR.
std::optional<RegistrationImportLoad>
registrationImportLoad(const LowBlock &Block,
                       const LowInstructionBoundary &Boundary);

/// Also authenticate the immutable instruction and exact runtime provider.
std::optional<RegistrationImportLoad>
checkedRegistrationImportLoad(const BinaryImage &Image, const LowBlock &Block,
                              const LowInstructionBoundary &Boundary,
                              size_t &Work,
                              RegistrationImportLoadPolicy Policy =
                                  RegistrationImportLoadPolicy::CallableOnly);
} // namespace registration_abi
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONIMPORTLOAD_H
