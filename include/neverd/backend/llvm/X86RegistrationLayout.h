//===- X86RegistrationLayout.h - PE32 frame layout --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONLAYOUT_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONLAYOUT_H

#include <cstdint>
#include <optional>

namespace neverd {
struct RegistrationFrameCoordinate;

/// A checked projection into one synthetic allocation. This says nothing
/// about an arbitrary source entry ESP: the allocation's actual alignment
/// must establish the residue used in this projection.
struct X86RegistrationFrameLayout {
  uint32_t Size = 0;
  uint32_t EntrySP = 0;
  uint32_t Establisher = 0;

  std::optional<uint32_t> runtimeOffset(int32_t Offset, uint64_t Width) const;
};

std::optional<X86RegistrationFrameLayout>
projectX86RegistrationFrame(const RegistrationFrameCoordinate &Coordinate,
                            uint64_t Size, uint64_t EntrySP,
                            uint64_t Alignment);

/// Fold an integer stack-alignment mask only when the physical allocation
/// proves its residue. Bounds and initialization remain the caller's job.
std::optional<int64_t> alignX86RegistrationOffset(int64_t Offset,
                                                  uint64_t RootAlignment,
                                                  uint32_t Mask);
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86REGISTRATIONLAYOUT_H
