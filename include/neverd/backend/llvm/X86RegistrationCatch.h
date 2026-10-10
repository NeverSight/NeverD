//===- X86RegistrationCatch.h - PE32 catch object projection -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONCATCH_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONCATCH_H

#include <cstdint>
#include <optional>

namespace neverd {
struct ExceptionFunction;
struct RegistrationStateAnalysis;
struct X86RegistrationFrameLayout;

struct X86RegistrationCatchHome {
  uint32_t Offset = 0;
  uint32_t ObjectSize = 0;
  bool Reference = false;

  uint32_t slotSize() const { return Reference ? 4 : ObjectSize; }
};

/// Absence of Home means that the runtime binds no object. It is distinct
/// from an invalid or missing source proof, which has no plan at all.
struct X86RegistrationCatchPlan {
  std::optional<X86RegistrationCatchHome> Home;
};

/// Project the single catch of a classified native source into its checked
/// parent allocation. No-object catches must have no runtime-object accesses.
std::optional<X86RegistrationCatchPlan>
projectX86RegistrationCatch(const ExceptionFunction &EH,
                            const RegistrationStateAnalysis &State,
                            const X86RegistrationFrameLayout &Frame);
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86REGISTRATIONCATCH_H
