//===- RegistrationPhysicalFrame.h - Shared frame replay --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Produce a physical frame binding from authenticated private LowIR effects.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_REGISTRATIONPHYSICALFRAME_H
#define NEVERD_IR_MED_X86_REGISTRATIONPHYSICALFRAME_H

#include "neverd/ir/med/X86/X86RegistrationPhysicalFrame.h"

#include <memory>

namespace neverd::x86_registration {

/// Shared occurrence binding for independently replayed registration helpers.
bool matchesPhysicalChain(const std::vector<RegistrationChainAccess> &Source,
                          const RegistrationChainAccess &Entry,
                          const std::vector<X86RegistrationPhysicalExit> &Exits,
                          const std::vector<RegistrationChainAccess> &Physical);
bool hasCurrentPhysicalChain(
    const MedFunc &Function,
    const std::vector<RegistrationChainAccess> &ChainAccesses);

/// Reanalyze a private, authenticated expansion without changing the source
/// function or its receipts. Failed replay grants no native frame binding.
std::shared_ptr<const X86RegistrationCxxPhysicalFrame>
analyzePhysicalCxxFrame(const BinaryImage &Image, const LowFunc &Source,
                        LowFunc &Physical);

} // namespace neverd::x86_registration

#endif
