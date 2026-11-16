//===- MIPS16EntryTransport.h - Proven classic entry transfers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_MIPS_MIPS16ENTRYTRANSPORT_H
#define NEVERD_IR_LOW_MIPS_MIPS16ENTRYTRANSPORT_H

#include "neverd/Common.h"

#include <array>
#include <optional>

namespace neverd {
struct BinaryImage;
class ExecutableCodeOwnerIndex;

/// A bounded, straight-line classic entry that transfers to a proven MIPS16
/// body without touching the stack or return link. Each byte of a0..a3 records
/// its incoming register-byte offset; a constant byte has no register source.
/// This is an original-machine target proof, independent of names and types.
struct MIPS16EntryTransport {
  va_t Body = InvalidVA;
  va_t Transfer = InvalidVA;
  std::array<std::optional<uint64_t>, 16> ArgumentBytes{};
};

std::optional<MIPS16EntryTransport>
proveMIPS16EntryTransport(const BinaryImage &Image, va_t Entry,
                          const ExecutableCodeOwnerIndex *Owners = nullptr);

} // namespace neverd

#endif // NEVERD_IR_LOW_MIPS_MIPS16ENTRYTRANSPORT_H
