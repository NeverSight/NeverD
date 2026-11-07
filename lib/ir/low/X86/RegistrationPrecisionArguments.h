//===- RegistrationPrecisionArguments.h - Private CRT arguments -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep a block-local scalar argument packet out of general frame memory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONPRECISIONARGUMENTS_H
#define NEVERD_IR_LOW_X86_REGISTRATIONPRECISIONARGUMENTS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd {
struct BinaryImage;
struct LowOp;
namespace registration_state {
struct FrameState;
class FrameTransfer;
} // namespace registration_state

namespace registration_abi {

/// The three PE32 _controlfp_s carriers, sampled only by complete stores.
/// No packet word can supply a frame LOAD or restore a saved stack pointer.
class PrecisionArguments {
  struct Word {
    int32_t Offset;
    uint32_t Value;
  };
  std::array<std::optional<Word>, 3> Words;
  size_t Next = 0;
  bool Active = false;

public:
  void clear();
  bool observe(const LowOp &Op,
               const registration_state::FrameTransfer &Transfer, size_t &Work);
  bool provesSuccessfulCall(const BinaryImage &Image, const LowOp &Op,
                            const registration_state::FrameState &State,
                            size_t &Work) const;
};

} // namespace registration_abi
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONPRECISIONARGUMENTS_H
