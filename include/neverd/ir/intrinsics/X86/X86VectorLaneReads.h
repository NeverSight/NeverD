//===- X86VectorLaneReads.h - Vector bytes x86 intrinsics read --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The bytes of a vector operand an x86 intrinsic reads, where it reads fewer
/// than all of them.  `cvtsi2sd` writes a register's low double and keeps its
/// incoming upper lane, and an unpack of the low lanes that follows reads
/// none of what was kept: neither a parameter's recovery nor an unknown
/// value's trap may count those bytes as read.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86VECTORLANEREADS_H
#define NEVERD_IR_INTRINSICS_X86_X86VECTORLANEREADS_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace neverd {

/// The bytes of a \p Bytes-wide vector operand that intrinsic \p Id reads,
/// one bit per byte up to 64, or nothing where it reads every byte.  An
/// unpack of the low lanes and a movddup read the low eight bytes of each
/// sixteen.
inline std::optional<uint64_t> x86VectorOperandReadBytes(Intrinsic Id,
                                                         unsigned Bytes) {
  switch (Id) {
  case Intrinsic::Unpcklps:
  case Intrinsic::Unpcklpd:
  case Intrinsic::Punpcklbw:
  case Intrinsic::Punpcklwd:
  case Intrinsic::Punpckldq:
  case Intrinsic::Punpcklqdq:
  case Intrinsic::Movddup:
    break;
  default:
    return std::nullopt;
  }
  uint64_t Read = 0;
  for (unsigned Lane = 0; Lane < Bytes && Lane < 64; Lane += 16)
    Read |= ((uint64_t{1} << std::min(8u, Bytes - Lane)) - 1) << Lane;
  return Read;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86VECTORLANEREADS_H
