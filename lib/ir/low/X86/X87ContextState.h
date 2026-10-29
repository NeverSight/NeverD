//===- X87ContextState.h - Owned x87 physical register mapping -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Change TOP in an FXSAVE64 image without changing physical payload identity.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_LOW_X86_X87CONTEXTSTATE_H
#define NEVERD_LIB_IR_LOW_X86_X87CONTEXTSTATE_H

#include <algorithm>
#include <array>
#include <cstdint>

namespace neverd {

/// NewTop is an architectural three-bit TOP value. Payload slots are logical;
/// tags and provenance are physical and do not rotate with them.
inline void setX87ContextTop(std::array<uint8_t, 512> &State, unsigned NewTop) {
  const unsigned OldTop = (State[3] >> 3) & 7;
  const auto Old = State;
  for (unsigned I = 0; I != 8; ++I) {
    const unsigned OldLogical = (NewTop + I + 8 - OldTop) & 7;
    std::copy_n(Old.begin() + 32 + OldLogical * 16, 16,
                State.begin() + 32 + I * 16);
  }
  State[3] = (State[3] & 0xc7) | (NewTop << 3);
}

} // namespace neverd

#endif // NEVERD_LIB_IR_LOW_X86_X87CONTEXTSTATE_H
