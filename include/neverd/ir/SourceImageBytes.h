//===- SourceImageBytes.h - Conditional image-byte observations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Conditional byte observations and their required block-entry inputs.

#ifndef NEVERD_IR_SOURCEIMAGEBYTES_H
#define NEVERD_IR_SOURCEIMAGEBYTES_H

#include "neverd/Common.h"

#include <cstdint>
#include <vector>

namespace neverd {

/// A required block-entry byte, with a constant STORE that proposes its value.
/// Its address may also be conditional. The initializer is evidence for the
/// candidate, not proof it reaches Block or writes this address at runtime.
struct SourceImageByteInput {
  va_t Address = 0;
  va_t Initializer = 0;
  uint8_t Value = 0;
  bool operator==(const SourceImageByteInput &) const = default;
};

/// A required physical register slice at the observation's block entry. Exact
/// scalar facts or the separate display-candidate query propose its numeric
/// value. Isolated original-instruction replay must supply these bytes; the
/// condition grants no reachability, preservation, pointer or call ABI proof.
struct SourceImageRegisterInput {
  uint64_t Offset = 0;
  uint16_t Bytes = 0;
  uint64_t Value = 0;
  bool operator==(const SourceImageRegisterInput &) const = default;
};

/// Bytes after Instruction, conditional on Inputs and EntryRegisters at Block
/// entry and ordinary successful execution of its original instructions and
/// authenticated
/// straight-line continuations. Display only: no entry-path feasibility,
/// call preservation, concurrency, pointer identity or rewrite authority.
struct SourceImageBytes {
  va_t Block = 0;
  va_t Instruction = 0;
  va_t Address = 0;
  std::vector<uint8_t> Bytes;
  std::vector<SourceImageByteInput> Inputs;
  std::vector<SourceImageRegisterInput> EntryRegisters;
  bool operator==(const SourceImageBytes &) const = default;
};

struct SourceImageByteObservations {
  std::vector<SourceImageBytes> Values;
  bool Complete = true;
};

} // namespace neverd

#endif // NEVERD_IR_SOURCEIMAGEBYTES_H
