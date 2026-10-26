//===- SBFCallXMemoryEffects.h - CALLX writes ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private mod/ref summaries. Symbolic StackAddress bases 1..11 denote entry
/// registers, with base 11 denoting the entry frame. They are never VM values
/// or addresses and are used only with the canonical register transfer.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SBF_ANALYSIS_SBFCALLXMEMORYEFFECTS_H
#define NEVERD_LIB_SBF_ANALYSIS_SBFCALLXMEMORYEFFECTS_H

#include "neverd/sbf/analysis/SBFDataflow.h"

#include <array>
#include <cstdint>
#include <vector>

namespace neverd::sbf::callx_detail {

RegisterState symbolicInputs();
RegisterValue substituteOrigin(const RegisterValue &Origin,
                               const RegisterState &Entry,
                               const SBFProgram &Program);

struct MemoryWrite {
  RegisterValue Address;
  uint64_t Size;
};

struct MemoryEffects {
  bool Unknown = false;
  bool WritesFrames = false;
  // Coalesced intervals: at most one per entry register, plus one absolute
  // interval. Filling holes is conservative invalidation, never a must-write.
  std::vector<MemoryWrite> Writes;

  void addWrite(RegisterValue Address, uint64_t Size,
                const SBFProgram &Program);
  void include(const MemoryEffects &Callee, const RegisterState &Entry,
               const SBFProgram &Program);
  bool invalidate(ScratchState &Scratch,
                  const std::array<ProvenValueSet, kRegisterCount> &Entry,
                  const SBFProgram &Program) const;
};

} // namespace neverd::sbf::callx_detail

#endif // NEVERD_LIB_SBF_ANALYSIS_SBFCALLXMEMORYEFFECTS_H
