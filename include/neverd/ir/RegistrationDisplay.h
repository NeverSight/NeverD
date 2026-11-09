//===- RegistrationDisplay.h - Shared x86 SEH source layout ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source projections share the recovered registration frame coordinates.
/// These display facts do not qualify a native runtime frame.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_REGISTRATIONDISPLAY_H
#define NEVERD_IR_REGISTRATIONDISPLAY_H

#include "neverd/ir/RegistrationState.h"
#include "neverd/loader/LanguageEHRegistration.h"

namespace neverd::registration_display {

/// The runtime's exception-pointer cell is one PE32 word below the record.
/// Retain the display model's conventional EBP coordinate when the prologue
/// does not publish an explicit bias; native projection requires its own proof.
inline int64_t exceptionPointersSlot(const RegistrationDisplayFacts &Facts,
                                     const RegistrationChainInfo &Chain) {
  return int64_t(Facts.FrameFromEntry.value_or(-4)) +
         Chain.RegistrationOffset.value_or(Facts.RecordFromFrame) - 4;
}

} // namespace neverd::registration_display

#endif // NEVERD_IR_REGISTRATIONDISPLAY_H
