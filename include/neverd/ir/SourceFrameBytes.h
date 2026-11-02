//===- SourceFrameBytes.h - Observed stack bytes -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Display-only byte observations at original machine instruction positions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_SOURCEFRAMEBYTES_H
#define NEVERD_IR_SOURCEFRAMEBYTES_H

#include "neverd/Common.h"

#include <cstdint>
#include <vector>

namespace neverd {

/// Bytes known immediately before Instruction, relative to the incoming SP.
/// This grants no source ABI, writable-storage replacement or pointer identity.
struct SourceFrameBytes {
  va_t Instruction = 0;
  int64_t FrameOffset = 0;
  std::vector<uint8_t> Bytes;
};

} // namespace neverd

#endif // NEVERD_IR_SOURCEFRAMEBYTES_H
