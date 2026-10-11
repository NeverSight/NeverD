//===- X86RegistrationFrame.h - Runtime frame coordinates -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86REGISTRATIONFRAME_H
#define NEVERD_IR_MED_X86REGISTRATIONFRAME_H

#include "neverd/ir/med/MedIR.h"

#include <optional>

namespace neverd {

/// A PE32 source address: alignDown(entry ESP + EntryOffset, Alignment) +
/// AlignedOffset. Alignment one is the ordinary affine coordinate. An aligned
/// establisher must not be collapsed to a constant entry-stack displacement.
struct RegistrationFrameCoordinate {
  int32_t EntryOffset = 0;
  uint32_t Alignment = 1;
  int32_t AlignedOffset = 0;
};

/// Project the checked LowIR frame contract without reinterpreting its state
/// graph. A metadata-only or incomplete realigned frame has no coordinate.
std::optional<RegistrationFrameCoordinate>
realignedRegistrationFrameCoordinate(const ExceptionFunction &EH,
                                     const RegistrationStateAnalysis *State);

/// Source coordinate for a complete fixed or realigned C++ frame. Native
/// runtime displacements additionally require cxxSourceFrameOffset().
std::optional<RegistrationFrameCoordinate>
cxxRegistrationFrameCoordinate(const ExceptionFunction &EH,
                               const RegistrationStateAnalysis *State);

/// Coordinate of a source-frame runtime definition. Callback ESP is a
/// separate invocation's stack and deliberately has no parent coordinate.
std::optional<RegistrationFrameCoordinate>
registrationRootFrameCoordinate(const MedFunc &Func, const MedOp &Op);

} // namespace neverd

#endif // NEVERD_IR_MED_X86REGISTRATIONFRAME_H
