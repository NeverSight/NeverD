//===- RegistrationJumpBuffer.cpp - Live SEH capture storage --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Tracks the initialized fields and lifetime of an SEH jump buffer.

#include "RegistrationJumpCapture.h"

namespace neverd::registration_state {
namespace {
std::array<FrameValue, 10> captureCells(const SEHJumpCapture &Capture) {
  return {Capture.Nonvolatile[0],
          Capture.Nonvolatile[1],
          Capture.Nonvolatile[2],
          Capture.Nonvolatile[3],
          FrameValue::frame(Capture.StackOffset - 4),
          FrameValue::imageAddress(uint32_t(Capture.ReturnPC)),
          FrameValue::frame(-16),
          FrameValue::constant(uint32_t(Capture.Level)),
          FrameValue::constant(0x56433230),
          FrameValue::imageAddress(uint32_t(Capture.UnwindTarget))};
}
} // namespace

void storeSEHJumpCapture(FrameState &Frame, const SEHJumpCapture &Capture) {
  const auto Cells = captureCells(Capture);
  for (unsigned I = 0; I != Cells.size(); ++I)
    Frame.store(Capture.BufferOffset + 4 * I, 4, Cells[I]);
  if (Capture.Protocol == RegistrationSEHJumpCapture::Kind::EH4)
    Frame.store(Capture.BufferOffset + 40, 4,
                FrameValue::imageAddress(uint32_t(Capture.SecurityCookieVA)));
}

bool hasSEHJumpCapture(const FrameState &Frame, const SEHJumpCapture &Capture) {
  const auto Cells = captureCells(Capture);
  for (unsigned I = 0; I != Cells.size(); ++I)
    if (Frame.load(Capture.BufferOffset + 4 * I, 4) != Cells[I])
      return false;
  return Capture.Protocol != RegistrationSEHJumpCapture::Kind::EH4 ||
         Frame.load(Capture.BufferOffset + 40, 4) ==
             FrameValue::imageAddress(uint32_t(Capture.SecurityCookieVA));
}

} // namespace neverd::registration_state
