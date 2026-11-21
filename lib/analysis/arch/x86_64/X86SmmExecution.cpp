//===- X86SmmExecution.cpp - Software SMI and RSM -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// System RAM is a separate address space. RSM consumes the guest's current
/// save image, including firmware edits, independently of the previous CPU.
//===----------------------------------------------------------------------===//

#include "X86SmmExecution.h"

#include "X86ModeState.h"
#include "X86SgxExecution.h"

#include "neverd/ir/X86/X86DebugRegisterRules.h"
#include "neverd/translate/X86/X86ControlRegisters.h"

#include "llvm/Support/Errc.h"

namespace neverd::analysis::detail {
using namespace translate;

namespace {
using Status = X86LegacyExecutionStatus;
constexpr unsigned SaveOffset = 0xfe00;
constexpr unsigned EntryOffset = 0x8000;
bool supportsControlFeatures(const X86PagingFeatures &Features,
                             const X86ModeExecutionOptions &Options,
                             uint64_t CR4, uint64_t EFER) {
  const uint32_t Other = Options.Legacy.ControlRegisters
                             ? Options.Legacy.ControlRegisters->OtherCR4Features
                             : 0;
  return !(CR4 & ~uint64_t(x86SupportedCR4(Features, Other))) &&
         !(EFER & ~x86SupportedEFER(Features, Options.EFERSystemCall));
}

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}
} // namespace

llvm::Expected<X86ModeExecutionResult>
enterX86Smm(X86ModeState &S, X86PagedMemory &Normal, X86PagedMemory &SMRAM,
            const X86ModeExecutionOptions &Options) {
  if (!S.Debug || (Normal.controls().CR4 & (uint64_t{1} << 23)))
    return invalid(
        "software SMM requires a debug register bank and non-CET state");
  auto C = Normal.controls();
  if (!supportsControlFeatures(Normal.features(), Options, C.CR4, C.EFER))
    return invalid("SMI requires the interrupted CR4 and EFER features to be "
                   "bound to the processor");
  C.CR0 &= ~uint64_t(0x8000000d);
  C.CR4 = 0;
  if (Normal.features().LongMode)
    C.EFER = 0;
  C.RFlags = 2;
  if (auto E = validateX86PagingState(SMRAM.features(), C))
    return std::move(E);
  if (SMRAM.sgxMemory() && SMRAM.sgxMemory()->active())
    return invalid("the system-management address space requires an inactive "
                   "enclave context");
  if (Normal.sgxMemory() && Normal.sgxMemory()->active()) {
    auto Probe = SMRAM.physicalMemory().validateStoreBytes(
        uint64_t(S.SMM->SMBase) + SaveOffset, X86SmmSaveImage{}.size());
    if (Probe.Status == GuestMemoryAccessStatus::Fault) {
      X86ModeExecutionResult R;
      R.Status = Status::BackingFault;
      R.BackingFault = Probe.Fault;
      return R;
    }
    X64ContinuationEventV1 Event{modeRegisters(S).PC, 0, 0, 0,
                                 X86EventKind::Interrupt};
    if (auto E = X86SgxExecution::asynchronousExit(
            S.Core.Core, *Normal.sgxMemory(), Event, modeRegisters(S).RFlags))
      return std::move(E);
  }
  auto B = saveX86SmmState(S, Normal.controls(), Normal.features().LongMode);
  const uint64_t Base = S.SMM->SMBase;
  auto W = SMRAM.physicalMemory().storeBytes(Base + SaveOffset, B);
  X86ModeExecutionResult R;
  if (W.Status == GuestMemoryAccessStatus::Fault) {
    R.Status = Status::BackingFault;
    R.BackingFault = W.Fault;
    return R;
  }
  llvm::cantFail(SMRAM.bindControls(C));
  auto &V = S.Core.Core;
  auto &Regs = modeRegisters(S);
  if (S.RetainedRealCS)
    S.SMM->InterruptedRealCS = V.Core.CS;
  else
    S.SMM->InterruptedRealCS.reset();
  S.RetainedRealCS = false;
  S.SMM->EnteredInsideNMI = V.NMIBlocked;
  S.SMM->Active = V.NMIBlocked = true;
  S.SSBlocked = S.STIBlocked = S.Halted = false;
  S.Debug->PendingData = 0;
  S.Debug->DR7 = 0x400;
  Regs.PC = EntryOffset;
  Regs.RFlags = 2;
  V.IDTR = {};
  V.Core.CS = {Base, UINT32_MAX, uint16_t(Base >> 4), 0x8093};
  V.Core.SS = V.DS = V.ES = {0, UINT32_MAX, 0, 0x8093};
  V.FS = V.GS = {UINT32_MAX, 0, 0x8093};
  Regs.FSBase = Regs.GSBase = 0;
  V.Unusable &= 0xc0;
  R.Status = Status::EventDelivered;
  R.EventAccepted = true;
  return R;
}

llvm::Expected<X86ModeExecutionResult>
executeX86Rsm(X86ModeState &S, X86PagedMemory &Normal, X86PagedMemory &SMRAM,
              llvm::ArrayRef<uint8_t> Bytes, bool Locked, bool Fetched,
              const X86ModeExecutionOptions &Options, X86RsmProgress &Progress,
              X86EventKind &FaultKind) {
  const auto &Regs = modeRegisters(S);
  const bool TF = Regs.RFlags & 0x100;
  if ((S.SMM->Active || TF) && !S.Debug)
    return invalid("software RSM requires a bound debug register bank");
  X86ModeExecutionResult R;
  if (!Fetched) {
    const auto &CS = S.Core.Core.Core.CS;
    const uint64_t Address =
        CS.Attributes & 0x2000 ? Regs.PC : uint32_t(CS.Base + Regs.PC);
    if (S.Debug && (!SMRAM.sgxMemory() ||
                    SMRAM.sgxMemory()->instructionBreakpointsAt(Address))) {
      const auto Matches = x86ExecutionBreakpoints(
          S.Debug->DR, S.Debug->DR7, Address, Regs.RFlags, S.SSBlocked);
      if (Matches) {
        x86PublishInstructionDebug(*S.Debug, false, Matches);
        FaultKind = X86EventKind::InstructionBreakpoint;
        R.InstructionAttempted = true;
        R.Exception = X86MemoryException{1, 0, {}};
        return R;
      }
    }
    auto OK = fetchX86SystemInstruction(
        S, SMRAM, Bytes, Options.Legacy.Segments.FullLimitChoice, R);
    if (!OK)
      return OK.takeError();
    if (!*OK)
      return R;
  }
  if (Locked || !S.SMM->Active) {
    R.Exception = X86MemoryException{6, 0, {}};
    return R;
  }
  if (!Progress.SaveArea) {
    X86SmmSaveImage B;
    auto Read = SMRAM.physicalMemory().loadBytes(
        uint64_t(S.SMM->SMBase) + SaveOffset, B);
    if (Read.Status == GuestMemoryAccessStatus::Fault) {
      R.Status = Status::BackingFault;
      R.BackingFault = Read.Fault;
      return R;
    }
    Progress.SaveArea = std::move(B);
  }
  if (TF && (S.Debug->DebugCTL & kX86BranchTraceFlag))
    return invalid("software RSM branch single-stepping requires a bound "
                   "processor behavior");
  auto Next = S;
  auto C = SMRAM.controls();
  restoreX86SmmState(*Progress.SaveArea, Next, C, Normal.features().LongMode);
  const auto Stop = [&] {
    S.Shutdown = true;
    R.Status = Status::Shutdown;
    return R;
  };
  if ((C.CR0 >> 32) || ((C.CR0 & 0x20000000) && !(C.CR0 & 0x40000000)) ||
      !supportsControlFeatures(Normal.features(), Options, C.CR4, C.EFER) ||
      (C.CR3 >> Normal.features().PhysicalAddressBits) ||
      ((C.CR4 & (1u << 17)) && !(C.EFER & 0x400)) || (Next.Debug->DR6 >> 32) ||
      (Next.Debug->DR7 >> 32))
    return Stop();
  C.CR0 = (C.CR0 & kX86CR0StoredMask) | 0x10;
  Next.Debug->DR6 = x86NormalizedDR6(Next.Debug->DR6);
  Next.Debug->DR7 = x86NormalizedDR7(Next.Debug->DR7);
  C.PDPTE = {};
  if (auto E = validateX86PagingState(Normal.features(), C)) {
    llvm::consumeError(std::move(E));
    return Stop();
  }
  if ((C.CR0 & 0x80000000) && (C.CR4 & 0x20) && !(C.EFER & 0x400)) {
    auto Read = loadX86PAEPDPTE(Normal.physicalMemory(), Normal.features(),
                                C.CR3, C.PDPTE, &Progress.PDPTE);
    if (!Read)
      return Read.takeError();
    if (Read->BackingFault) {
      R.Status = Status::BackingFault;
      R.BackingFault = Read->BackingFault;
      return R;
    }
    if (Read->Exception)
      return Stop();
  }
  if (auto E = validateX86PagingState(Normal.features(), C)) {
    llvm::consumeError(std::move(E));
    return Stop();
  }
  if (!x86InstructionDebugValid(Next.Debug->DR6, Next.Debug->DR7,
                                Next.Debug->DebugCTL, Options.Long.DebugProfile,
                                C.CR4, Next.Debug->PendingData))
    return invalid("restored debug state exceeds the bound SMM profile");
  llvm::cantFail(Normal.bindControls(C));
  S = std::move(Next);
  R.InstructionCompleted = true;
  if (TF) {
    x86PublishInstructionDebug(*S.Debug, true);
    R.Exception = X86MemoryException{1, 0, {}};
  }
  return R;
}

} // namespace neverd::analysis::detail
