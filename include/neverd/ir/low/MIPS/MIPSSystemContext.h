//===- MIPSSystemContext.h - Authenticated system observations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One-step MIPS32 observations. No interrupt delivery, CP0 timing or exception
/// entry is inferred. The caller authenticates privilege and register values.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_MIPS_MIPSSYSTEMCONTEXT_H
#define NEVERD_IR_LOW_MIPS_MIPSSYSTEMCONTEXT_H

#include <cstdint>
#include <optional>

namespace neverd {
class MIPSCachePlatform;
class MIPSMSAExecutor;
struct MIPSHardwareObservation {
  uint8_t Index;
  uint32_t Value;
};
/// Observed state of the issuing TC and the currently free physical WR pool.
/// Other TCs must not change the pool during one bound transition.
struct MIPSMSAPartition {
  uint32_t Access, Save, Modify, Request;
  uint8_t Map, Unmap;
  unsigned FreePhysical;
};
struct MIPSSystemContext {
  bool CP0Usable = false;
  bool ImplementedMT = false;
  uint32_t HWREna = 0;
#define NEVERD_MIPS_CP0(Index, Selector, Name, Writable, MT)                   \
  std::optional<uint32_t> Name;
#include "neverd/ir/intrinsics/MIPS/MIPSCP0.def"
  std::optional<MIPSHardwareObservation> Hardware;
  bool ExecutionHazardPending = false;
  MIPSCachePlatform *Cache = nullptr;

  // This context belongs to the issuing TC. The control-register read
  // hazard (EHB) and suspension of peer instruction issue (JR.HB etc.) are
  // architecturally distinct.
  bool ThreadIssueHazard = false;
  bool ProcessorIssueHazard = false;
  void completeInstructionHazardBarrier() {
    ExecutionHazardPending = ThreadIssueHazard = ProcessorIssueHazard = false;
  }
  /// Whether *these MT gates* permit a different TC to issue. Halt, debug,
  /// activation and scheduling policy are independent of these gates.
  std::optional<bool> peerIssueEnabled(bool SameVPE) const {
    if (!ImplementedMT)
      return std::nullopt;
    const auto Gate = [](std::optional<uint32_t> Word, unsigned Mask,
                         bool Pending) -> std::optional<bool> {
      if (Pending || !Word)
        return std::nullopt;
      return (*Word & Mask) != 0;
    };
    const auto Global = Gate(MVPControl, 1, ProcessorIssueHazard);
    const auto Local = SameVPE ? Gate(VPEControl, 1u << 15, ThreadIssueHazard)
                               : std::optional<bool>(true);
    if (Global == false || Local == false)
      return false;
    if (Global && Local)
      return true;
    return std::nullopt;
  }

  // Explicit MSA implementation and per-TC control state. WRP platforms
  // require separate register partition state for selectors 2 through 7.
  std::optional<uint32_t> MSAImplementation, MSAControl, MSAControlWritableMask;
  std::optional<MIPSMSAPartition> MSAPartition;
  MIPSMSAExecutor *MSAFloat = nullptr;
  bool MSAFloatingPointException = false;
};
} // namespace neverd
#endif // NEVERD_IR_LOW_MIPS_MIPSSYSTEMCONTEXT_H
