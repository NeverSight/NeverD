//===- X86RegistrationSEH4PhysicalFrame.h -----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind independently replayed shared SEH4 frame effects to their source calls.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86REGISTRATIONSEH4PHYSICALFRAME_H
#define NEVERD_IR_MED_X86REGISTRATIONSEH4PHYSICALFRAME_H

#include <cstdint>
#include <optional>

namespace neverd {
struct BinaryImage;
struct MedFunc;
struct RegistrationStateAnalysis;
class X86RegistrationSEH4PhysicalFrame;

/// Direct source states, or independently replayed physical states whose
/// non-helper facts and current chain operations still match the source.
/// A stale, removed or incomplete shared-frame binding returns null.
const RegistrationStateAnalysis *
getX86RegistrationSEHPhysicalStates(const MedFunc &Function);

/// Recheck current immutable helpers. A checked shared image entry cannot
/// fall back to direct-frame lowering after its saved receipt is removed.
bool hasCheckedX86RegistrationSEHPhysicalFrame(const BinaryImage &Image,
                                               const MedFunc &Function);

/// Bytes below entry ESP required by a valid shared projection, including
/// transient helper PC storage. Direct frames contribute zero; invalid
/// projections return no extent.
std::optional<uint64_t>
getX86RegistrationSEHFrameStorage(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_IR_MED_X86REGISTRATIONSEH4PHYSICALFRAME_H
