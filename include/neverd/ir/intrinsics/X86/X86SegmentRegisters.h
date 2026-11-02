//===- X86SegmentRegisters.h - x86 segment registers ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86SEGMENTREGISTERS_H
#define NEVERD_IR_INTRINSICS_X86_X86SEGMENTREGISTERS_H

#include <cstdint>
#include <optional>
#include <string_view>

namespace neverd {

/// The x86 segment registers by encoding (X86SegmentRegisters.def).
enum class X86SegmentRegister : uint8_t {
#define X86_SEGMENT_REGISTER(ID, NUMBER, NAME) ID = NUMBER,
#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.def"
};

/// The AT&T name of the segment register encoded as \p Number, or null when
/// no segment register has that encoding.
constexpr const char *x86SegmentRegisterName(uint64_t Number) {
#define X86_SEGMENT_REGISTER(ID, NUMBER, NAME)                                 \
  if (Number == NUMBER)                                                        \
    return NAME;
#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.def"
  return nullptr;
}

/// The segment register with AT&T name \p Name (without the `%` sigil).
constexpr std::optional<X86SegmentRegister>
x86SegmentRegisterNamed(std::string_view Name) {
#define X86_SEGMENT_REGISTER(ID, NUMBER, NAME)                                 \
  if (Name == NAME)                                                            \
    return X86SegmentRegister::ID;
#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.def"
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86SEGMENTREGISTERS_H
