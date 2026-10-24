//===- RegistrationCxxState.h - Image-bound C++ state analysis ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_REGISTRATIONCXXSTATE_H
#define NEVERD_IR_LOW_REGISTRATIONCXXSTATE_H

#include "neverd/ir/RegistrationState.h"

namespace neverd {
struct BinaryImage;
class RegistrationCallCalleeIndex;

/// Bind current handler and shared frame-call bytes before solving the same
/// LowIR state lattice. Shared exits require the separately proved prologue;
/// they are chain removals, never ordinary callee stack-pop effects.
RegistrationStateAnalysis analyzeCheckedX86CxxStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeFrameContract> *Callees = nullptr,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups = nullptr,
    const std::vector<RegistrationCalleeStackContract> *Stacks = nullptr,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds = nullptr,
    const std::vector<RegistrationSEHControlContract> *SEHControls = nullptr,
    RegistrationCallCalleeIndex *CalleeIndex = nullptr);

} // namespace neverd

#endif
