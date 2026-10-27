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

#include "llvm/ADT/SmallVector.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace neverd::sbf::callx_detail {

RegisterState symbolicInputs();
RegisterValue substituteOrigin(const RegisterValue &Origin,
                               const RegisterState &Entry,
                               const SBFProgram &Program);
ProvenValueSet
substituteOrigins(const ProvenValueSet &Origins,
                  const std::array<ProvenValueSet, kRegisterCount> &Entry,
                  const SBFProgram &Program);

struct MemoryWrite {
  RegisterValue Address;
  uint64_t Size;
  bool operator==(const MemoryWrite &) const = default;
};

struct MemoryEffects {
  bool Unknown = false;
  bool WritesFrames = false;
  // Coalesced intervals: at most one per entry register, plus one absolute
  // interval. Filling holes is conservative invalidation, never a must-write.
  std::vector<MemoryWrite> Writes;

  bool operator==(const MemoryEffects &Other) const {
    if (Unknown || Other.Unknown)
      return Unknown == Other.Unknown;
    return WritesFrames == Other.WritesFrames && Writes == Other.Writes;
  }

  void addWrite(RegisterValue Address, uint64_t Size, const SBFProgram &Program,
                const SymbolicAddressSpace *Addresses = nullptr);
  void include(const MemoryEffects &Callee, const RegisterState &Entry,
               const SBFProgram &Program,
               const SymbolicAddressSpace *Addresses = nullptr);
  bool invalidatesEntryFrame(const RegisterValue &Address,
                             const SBFProgram &Program) const;
  bool invalidate(ScratchState &Scratch,
                  const std::array<ProvenValueSet, kRegisterCount> &Entry,
                  const SBFProgram &Program) const;
};

/// Full-width postconditions at absolute scratch addresses or relative to a
/// function's entry registers. Symbolic keys are bases and signed
/// displacements, never synthetic VM addresses. Distinct entry registers may
/// alias; a write through either invalidates the other's cells. CFG joins meet
/// cell presence and join complete stored alternatives under the value limit.
class MemoryPostconditions {
public:
  void clear() {
    Values.clear();
    Copies.clear();
  }
  void transformSymbols(llvm::function_ref<RegisterValue(RegisterValue)> Map);
  void store(const RegisterValue &Address, uint64_t Size,
             const ProvenValueSet &Value);
  ProvenValueSet read(const RegisterValue &Address) const;
  void copyFrom(const RegisterValue &Source, const RegisterValue &Destination,
                uint64_t Size, const SBFProgram &Program);
  void recordInputCopy(const RegisterValue &Source,
                       const RegisterValue &Destination, uint64_t Size,
                       const MemoryEffects &Before);
  void discardInputCopies() { Copies.clear(); }
  size_t copyCount() const { return Copies.size(); }
  void meet(const MemoryPostconditions &Other);
  void invalidate(const MemoryEffects &Effects, const RegisterState &Entry,
                  const SBFProgram &Program);
  void include(const MemoryPostconditions &Callee,
               const std::array<ProvenValueSet, kRegisterCount> &Entry,
               const SBFProgram &Program, const MemoryEffects *Before = nullptr,
               const SymbolicAddressSpace *Addresses = nullptr);
  size_t apply(ScratchState &Scratch,
               const std::array<ProvenValueSet, kRegisterCount> &Entry,
               const SBFProgram &Program,
               const ScratchState *EntryScratch = nullptr) const;
  size_t size() const { return Values.size(); }
  uint64_t retainedByteEstimate() const;
  bool operator==(const MemoryPostconditions &) const = default;

private:
  struct InputCopy {
    RegisterValue Source;
    RegisterValue Destination;
    uint64_t Size;
    MemoryEffects Before;
    bool operator==(const InputCopy &) const = default;
  };
  llvm::SmallVector<InputCopy, 0> Copies;
  void invalidate(const RegisterValue &Address, uint64_t Size);
  std::map<std::tuple<RegisterValue::Kind, uint64_t, int64_t>, ProvenValueSet>
      Values;
};

} // namespace neverd::sbf::callx_detail

#endif // NEVERD_LIB_SBF_ANALYSIS_SBFCALLXMEMORYEFFECTS_H
