//===- RegistrationJumpCapture.h - Bound SEH jump capture ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind the current SEH invocation to its source jump-buffer capture.

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONJUMPCAPTURE_H
#define NEVERD_IR_LOW_X86_REGISTRATIONJUMPCAPTURE_H

#include "RegistrationFrame.h"

#include "neverd/ir/RegistrationState.h"

#include <array>
#include <functional>
#include <optional>
#include <set>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct RegistrationChainInfo;

namespace registration_state {

struct SEHJumpCapture {
  static constexpr uint16_t BufferBytes =
      RegistrationSEHJumpCapture::BufferBytes;
  va_t Call = InvalidVA;
  va_t ReturnPC = InvalidVA;
  va_t UnwindTarget = InvalidVA;
  int32_t BufferOffset = 0;
  int32_t StackOffset = 0;
  int32_t Level = -1;
  RegistrationSEHJumpCapture::Kind Protocol =
      RegistrationSEHJumpCapture::Kind::EH3;
  va_t SecurityCookieVA = 0;
  /// Actual EBP, EBX, EDI and ESI values. A later resume must retain their
  /// provenance; an unknown saved register is never a fabricated scalar.
  std::array<FrameValue, 4> Nonvolatile;
  bool operator==(const SEHJumpCapture &) const = default;
};

/// The image owner authenticates calls; the state owner proves buffer
/// lifetime, effects and resumption. These callbacks grant no target-wide
/// stack or memory summary.
struct SEHJumpRuntime {
  std::set<va_t> ImportSlots;
  std::function<std::optional<SEHJumpCapture>(const LowOp &, const FrameState &,
                                              bool, size_t &)>
      Capture;
  std::function<bool(const LowOp &, size_t &)> Resume;
  std::function<bool(const LowOp &, const FrameState &, size_t &)> Raise;
  std::function<bool(const LowOp &, size_t &)> ControlFP;
  std::function<bool(const LowOp &, size_t &)> StartupInfo;
  std::function<bool(const LowOp &, size_t &)> HeapTermination;
  std::function<bool(const LowOp &, size_t &)> Interlocked;
  std::function<bool(const LowOp &, size_t &)> Sleep;
  /// An authenticated ordinary call with zero stack cleanup and no parent
  /// frame borrow. This grants capture preservation only; its volatile
  /// registers, result and native caller ABI remain separate obligations.
  std::function<bool(const LowOp &, size_t &)> PrivateCall;
  /// Proves image storage disjoint from suspended callback/parent frames.
  /// Contents remain runtime values; writes cannot replace import bindings.
  std::function<bool(va_t, uint16_t, bool, size_t &)> ImageMemory;
};

/// EH3 initializes ten DWORDs; EH4 also saves its cookie address at offset 40.
/// The remaining unwind-data DWORDs remain untouched.
void storeSEHJumpCapture(FrameState &Frame, const SEHJumpCapture &Capture);
bool hasSEHJumpCapture(const FrameState &Frame, const SEHJumpCapture &Capture);

/// Prepare a capture for a checked installed direct EH3/EH4 frame. The exact
/// runtime protocol owns the unwind arguments. Buffer storage must be fully
/// separated from outgoing arguments and registration administration. This
/// supplies no surviving-buffer or resumption proof and is not a call-pop
/// summary. Unknown roles, source slices and protocol values refuse.
std::optional<SEHJumpCapture>
prepareSEHJumpCapture(const BinaryImage &Image, const LowFunc &Function,
                      const LowOp &Op, const FrameState &Frame, bool Installed,
                      size_t &Work);

/// Authenticate EH4's encoded scope table and third unwind argument. Cookie
/// initialization, slot disjointness and lifetime remain state obligations.
std::optional<va_t> checkedEH4JumpCookie(const BinaryImage &Image,
                                         const RegistrationChainInfo &Chain,
                                         const FrameState &Frame, int32_t SP);

} // namespace registration_state
} // namespace neverd

#endif
