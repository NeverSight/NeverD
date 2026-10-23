//===- X87PortableContext.h - Portable owned x87 execution -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Transactional Intel 64 architectural masked-exception slice. The existing
/// FXSAVE64 image remains the state owner: logical payload slots, physical tags
/// and physical payload provenance. This is not OS exception delivery.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X87PORTABLECONTEXT_H
#define NEVERD_IR_LOW_X87PORTABLECONTEXT_H

#include <array>
#include <cstdint>
#include <span>

namespace neverd {

inline constexpr const char *kX87PortableProfile =
    "intel64-architectural-masked-v1";

enum class X87PortableOutcome {
  Completed,
  InvalidInstruction,
  UnsupportedInstruction,
  InvalidControl,
  PendingException,
  UnmaskedControl,
  UnknownPayload,
  StackFault,
  UnsupportedValue,
  InvalidMemory
};

const char *x87PortableOutcomeName(X87PortableOutcome Outcome);

/// State's environment must be authenticated by the caller. KnownPayload uses
/// physical register bits and must not be inferred from tags or an ABI. Memory
/// is an authenticated ordinary operand copy of exactly the admitted size;
/// the caller owns address/permission checks and publication of memory writes.
/// IP and DP are guest addresses, never addresses of host scratch buffers.
/// On refusal all arguments remain unchanged. FNINIT does not authenticate
/// retained payloads. Only defined architectural fields are qualified: FOP is
/// retained for masked instructions, FDP is retained on register operations,
/// and undefined C0/C2/C3 bits are retained. No native fallback is performed.
X87PortableOutcome
executePortableX87Context(uint32_t Selector, std::array<uint8_t, 512> &State,
                          uint8_t &KnownPayload, std::span<uint8_t> Memory,
                          uint64_t IP, uint64_t DP, uint64_t &Result);

} // namespace neverd

#endif // NEVERD_IR_LOW_X87PORTABLECONTEXT_H
