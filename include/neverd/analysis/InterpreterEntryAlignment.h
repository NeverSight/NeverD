//===- InterpreterEntryAlignment.h - Explicit entry congruence -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETERENTRYALIGNMENT_H
#define NEVERD_ANALYSIS_INTERPRETERENTRYALIGNMENT_H

#include <cstdint>

namespace neverd::analysis {

/// Caller-selected entry-root domain; no concrete address or memory facts.
/// Recovery keeps exactly the roots whose low bits equal Residue. Machine
/// source checks this condition before guest effects; raw residual users must
/// enforce it themselves. It is never inferred from the image's ABI.
struct InterpreterEntryAlignment {
  uint32_t Alignment = 1;
  uint32_t Residue = 0;

  bool valid() const {
    return Alignment && !(Alignment & (Alignment - 1)) && Residue < Alignment;
  }
};

} // namespace neverd::analysis

#endif
