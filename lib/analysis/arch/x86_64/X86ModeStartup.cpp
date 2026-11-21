//===- X86ModeStartup.cpp - INIT and startup IPI -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Intel KVM INIT register image and the BSP/AP startup transition.
//===----------------------------------------------------------------------===//

#include "X86ModeStartup.h"

#include "X86ModeState.h"
#include "X86SgxExecution.h"

#include "llvm/Support/Errc.h"

#include <algorithm>

namespace neverd::analysis::detail {
namespace {
llvm::Error initializeProcessor(X86ModeState &S, translate::X86PagedMemory &M,
                                X86ModeInterrupts &Inputs) {
  auto Paging = M.controls();
  Paging.CR0 = 0x10 | (Paging.CR0 & 0x60000000);
  Paging.CR3 = Paging.CR4 = Paging.EFER = 0;
  Paging.RFlags = 2;
  Paging.PDPTE = {};
  if (auto E = translate::validateX86PagingState(M.features(), Paging))
    return E;

  auto &V = S.Core.Core;
  auto &R = modeRegisters(S);
  if (auto *EPC = M.sgxMemory(); EPC && EPC->active()) {
    X64ContinuationEventV1 Arrival{R.PC, 0, 0, 0, X86EventKind::Interrupt};
    if (auto E = X86SgxExecution::asynchronousExit(V, *EPC, Arrival, R.RFlags))
      return E;
  }

  llvm::cantFail(M.bindControls(Paging));
  std::fill(std::begin(R.GPR), std::end(R.GPR), 0);
  R.GPR[2] = S.Startup->ProcessorSignature;
  R.PC = 0xfff0;
  R.RFlags = 2;
  R.FSBase = R.GSBase = V.Core.Core.CR2 = V.Core.Core.Core.CR8 = 0;
  V.Core.CS = {0xffff0000, 0xffff, 0xf000, 0x9b};
  V.Core.SS = V.DS = V.ES = {0, 0xffff, 0, 0x93};
  V.FS = V.GS = {0xffff, 0, 0x93};
  V.TR = {0, 0xffff, 0, 0x8b};
  V.LDTR = {0, 0xffff, 0, 0x82};
  V.GDTR = V.IDTR = {0, 0xffff};
  V.NMIBlocked = V.Unusable = 0;
  if (S.Debug) {
    // KVM INIT resets DR0..7 and pending debug causes. DEBUGCTL belongs
    // to the live MSR bank, independently of the breakpoint address bank.
    const auto DebugCTL = S.Debug->DebugCTL;
    S.Debug.emplace();
    S.Debug->DebugCTL = DebugCTL;
  }
  S.RetainedRealCS = S.STIBlocked = S.SSBlocked = S.Halted = S.Shutdown = false;
  if (S.SMM) {
    S.SMM->EnteredInsideNMI = false;
    S.SMM->InterruptedRealCS.reset();
  }
  S.Startup->WaitingForSIPI = !S.Startup->BootstrapProcessor;
  Inputs.APIC = *translate::X86LocalApicFixed::fromState({});
  if (Inputs.LocalVectorCMCI)
    Inputs.LocalVectorCMCI = 1u << 16;
  Inputs.INITPending = Inputs.NMIPending = Inputs.SMIPending = false;
  return llvm::Error::success();
}
} // namespace

llvm::Expected<bool> pollX86Startup(X86ModeState &S,
                                    translate::X86PagedMemory &Normal,
                                    X86ModeInterrupts *Inputs,
                                    X86ModeExecutionResult &R) {
  const bool Waiting = S.Startup && S.Startup->WaitingForSIPI;
  if (!Waiting && (!Inputs || (!Inputs->INITPending && !Inputs->StartupVector)))
    return false;
  if (!S.Startup)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "INIT/SIPI requires a bound processor signature and BSP/AP role");
  if (Waiting && (S.Startup->BootstrapProcessor || S.Halted || S.Shutdown ||
                  (S.SMM && S.SMM->Active) ||
                  (Normal.sgxMemory() && Normal.sgxMemory()->active())))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "wait-for-SIPI requires a stopped "
                                   "application processor outside SMM and SGX");
  if (Inputs && Inputs->INITPending && !(S.SMM && S.SMM->Active)) {
    if (auto E = initializeProcessor(S, Normal, *Inputs))
      return std::move(E);
    R.EventAccepted = true;
    R.Status = translate::X86LegacyExecutionStatus::EventDelivered;
    return true;
  }
  if (Inputs && Inputs->StartupVector) {
    const uint8_t Vector = *Inputs->StartupVector;
    Inputs->StartupVector.reset();
    if (Waiting) {
      auto &CS = S.Core.Core.Core.CS;
      CS.Selector = uint16_t(Vector) << 8;
      CS.Base = uint64_t(Vector) << 12;
      modeRegisters(S).PC = 0;
      S.Startup->WaitingForSIPI = false;
      R.EventAccepted = true;
      R.Status = translate::X86LegacyExecutionStatus::EventDelivered;
      return true;
    }
  }
  if (Waiting)
    R.Status = translate::X86LegacyExecutionStatus::EventDeferred;
  return Waiting;
}
} // namespace neverd::analysis::detail
