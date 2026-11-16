//===- X86ModeState.h - Mixed-mode bank projections -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ephemeral non-64-bit views and exact continuation input authentication.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86MODESTATE_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86MODESTATE_H

#include "neverd/analysis/arch/x86_64/X86ModeExecution.h"

#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <tuple>

namespace neverd::analysis::detail {

inline auto &modeRegisters(X86ModeState &S) {
  return S.Core.Core.Core.Core.Core.Core;
}
inline const auto &modeRegisters(const X86ModeState &S) {
  return S.Core.Core.Core.Core.Core.Core;
}

inline bool sameModeState(const X86ModeState &A, const X86ModeState &B) {
  return A.Core.Core == B.Core.Core && A.Debug == B.Debug &&
         std::tie(A.Core.SysenterCS, A.Core.SysenterESP, A.Core.SysenterEIP,
                  A.Core.Reserved, A.STIBlocked, A.SSBlocked, A.Halted,
                  A.Shutdown) == std::tie(B.Core.SysenterCS, B.Core.SysenterESP,
                                          B.Core.SysenterEIP, B.Core.Reserved,
                                          B.STIBlocked, B.SSBlocked, B.Halted,
                                          B.Shutdown);
}

inline translate::X86LegacyExecutionState projectNon64(const X86ModeState &S) {
  using namespace translate;
  X86LegacyExecutionState L;
  const auto &V = S.Core.Core;
  const auto &R = modeRegisters(S);
  const auto Cache = [&](const X64ContinuationSegment &C, unsigned Bit) {
    return X86LegacySegmentCache{uint32_t(C.Base), C.Limit, C.Selector,
                                 C.Attributes, bool(V.Unusable & (1u << Bit))};
  };
  std::copy_n(R.GPR, 8, L.Core.GPR.begin());
  L.Core.Segments = {
      Cache(V.ES, 3),
      Cache(V.Core.CS, 0),
      Cache(V.Core.SS, 1),
      Cache(V.DS, 2),
      Cache({R.FSBase, V.FS.Limit, V.FS.Selector, V.FS.Attributes}, 4),
      Cache({R.GSBase, V.GS.Limit, V.GS.Selector, V.GS.Attributes}, 5)};
  L.Core.CPL = V.Core.CS.Selector & 3;
  L.Core.GDTRBase = V.GDTR.Base;
  L.Core.GDTRLimit = V.GDTR.Limit;
  L.Core.LDTR = Cache(V.LDTR, 6);
  L.TR = Cache(V.TR, 7);
  L.EIP = R.PC;
  L.EFlags = R.RFlags;
  L.CR2 = V.Core.Core.CR2;
  L.IDTRBase = V.IDTR.Base;
  L.IDTRLimit = V.IDTR.Limit;
  L.STIBlocked = S.STIBlocked;
  L.SSBlocked = S.SSBlocked;
  L.NMIBlocked = V.NMIBlocked;
  L.Halted = S.Halted;
  L.Sysenter = {uint32_t(S.Core.SysenterCS), uint32_t(S.Core.SysenterESP),
                uint32_t(S.Core.SysenterEIP)};
  if (S.Debug) {
    L.Debug.emplace();
    std::copy(S.Debug->DR.begin(), S.Debug->DR.end(), L.Debug->DR.begin());
    L.Debug->DR6 = S.Debug->DR6;
    L.Debug->DR7 = S.Debug->DR7;
    L.Debug->DebugCTL = S.Debug->DebugCTL;
    L.Debug->PendingData = S.Debug->PendingData;
  }
  return L;
}

inline void publishNon64(X86ModeState &S,
                         const translate::X86LegacyExecutionState &L,
                         const translate::X86LegacyExecutionResult &Result) {
  auto &V = S.Core.Core;
  auto &R = modeRegisters(S);
  for (unsigned I = 0; I != 8; ++I)
    R.GPR[I] =
        uint64_t(L.Core.GPR[I]) | (R.Undefined[I] & ~uint64_t(UINT32_MAX));
  R.PC = L.EIP;
  R.RFlags = L.EFlags;
  S.STIBlocked = L.STIBlocked;
  S.SSBlocked = L.SSBlocked;
  S.Halted = L.Halted;
  V.NMIBlocked = L.NMIBlocked;
  if (Result.CR2Written)
    V.Core.Core.CR2 = L.CR2;
  const auto Cache = [&](unsigned Index, X64ContinuationSegment &C,
                         unsigned Bit) {
    const auto &From = L.Core.Segments[Index];
    C = {From.Base, From.Limit, From.Selector, From.Attributes};
    V.Unusable = (V.Unusable & ~(1u << Bit)) | (uint8_t(From.Unusable) << Bit);
  };
  if (Result.CSWritten)
    Cache(1, V.Core.CS, 0);
  if (Result.SSWritten)
    Cache(2, V.Core.SS, 1);
  if (Result.SegmentWritten) {
    const auto &Write = *Result.SegmentWritten;
    const auto &From = L.Core.Segments[unsigned(Write.Register)];
    const auto Load = [&](auto &C, uint64_t &Base, unsigned Bit) {
      C.Selector = From.Selector;
      if (!Write.NullBase) {
        C.Limit = From.Limit;
        C.Attributes = From.Attributes;
        Base = From.Base;
      } else if (*Write.NullBase == X86NullSegmentBasePolicy::Clear) {
        Base = 0;
      }
      V.Unusable =
          (V.Unusable & ~(1u << Bit)) | (uint8_t(From.Unusable) << Bit);
    };
    switch (Write.Register) {
    case X86SegmentRegister::ES:
      Load(V.ES, V.ES.Base, 3);
      break;
    case X86SegmentRegister::SS:
      Load(V.Core.SS, V.Core.SS.Base, 1);
      break;
    case X86SegmentRegister::DS:
      Load(V.DS, V.DS.Base, 2);
      break;
    case X86SegmentRegister::FS:
      Load(V.FS, R.FSBase, 4);
      break;
    case X86SegmentRegister::GS:
      Load(V.GS, R.GSBase, 5);
      break;
    case X86SegmentRegister::CS:
      llvm_unreachable("a segment load cannot write CS");
    }
  }
  if (S.Debug && L.Debug) {
    if (Result.DebugAddressWritten) {
      const auto Index = *Result.DebugAddressWritten;
      S.Debug->DR[Index] = L.Debug->DR[Index];
    }
    S.Debug->DR6 = L.Debug->DR6;
    S.Debug->DR7 = L.Debug->DR7;
    S.Debug->DebugCTL = L.Debug->DebugCTL;
    S.Debug->PendingData = L.Debug->PendingData;
  }
}

} // namespace neverd::analysis::detail
#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86MODESTATE_H
