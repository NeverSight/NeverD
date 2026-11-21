//===- X86ModeSystemInstruction.cpp - Privileged x86 steps ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Common fetch, privilege and debug ordering for halt and MCA register access.
//===----------------------------------------------------------------------===//

#include "X86ModeSystemInstruction.h"

#include "X86ModeState.h"

#include "llvm/Support/Errc.h"

namespace neverd::analysis::detail {
using namespace translate;

llvm::Expected<X86ModeExecutionResult> executeModeSystemInstruction(
    X86ModeState &S, X86PagedMemory &Memory, const X86ModeInstruction &I,
    const X86ModeExecutionOptions &O, X86ModeSystemOp Operation, bool Locked,
    X86EventKind &FaultKind, X86SgxMemory *Enclaves) {
  X86ModeExecutionResult R;
  if (Operation != X86ModeSystemOp::Halt)
    if (auto E = S.MCA->validate())
      return std::move(E);
  auto &Regs = modeRegisters(S);
  const uint8_t CPL = modeCPL(S, Memory.controls());
  const bool TF = Regs.RFlags & 0x100;
  if (TF && !S.Debug)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "single-step system instruction requires the CPU debug bank");
  R.InstructionAttempted = true;
  if (x86PendingVirtualInterrupt(CPL, Memory.controls().CR4, Regs.RFlags)) {
    R.Exception = X86MemoryException{13, 0, {}};
    return R;
  }
  const bool Enclave = Memory.sgxMemory() && Memory.sgxMemory()->active();
  if (S.Debug && !Enclave) {
    const auto Matches = x86ExecutionBreakpoints(
        S.Debug->DR, S.Debug->DR7, I.Address, Regs.RFlags, S.SSBlocked);
    if (Matches) {
      x86PublishInstructionDebug(*S.Debug, false, Matches);
      R.Exception = X86MemoryException{1, 0, {}};
      FaultKind = X86EventKind::InstructionBreakpoint;
      return R;
    }
  }
  R.InstructionAttempted = false;
  auto Fetch = fetchX86SystemInstruction(S, Memory, I.Bytes,
                                         O.Legacy.Segments.FullLimitChoice, R);
  if (!Fetch)
    return Fetch.takeError();
  if (!*Fetch)
    return R;
  if (Locked || CPL) {
    R.Exception = X86MemoryException{uint8_t(Locked ? 6 : 13), 0, {}};
    return R;
  }
  if (Operation == X86ModeSystemOp::Halt) {
    S.Halted = true;
    R.Status = X86LegacyExecutionStatus::Halted;
  } else {
    auto &MCA = *S.MCA;
    const auto Index = uint32_t(Regs.GPR[1]);
    if (Operation == X86ModeSystemOp::ReadMCA) {
      auto Value = MCA.readMSR(Index);
      if (!Value) {
        R.Exception = X86MemoryException{13, 0, {}};
        return R;
      }
      const uint64_t HighMask =
          I.Mode == X86ExecutionMode::Long64 ? 0 : ~uint64_t(UINT32_MAX);
      Regs.GPR[0] = (Regs.Undefined[0] & HighMask) | uint32_t(*Value);
      Regs.GPR[2] = (Regs.Undefined[2] & HighMask) | (*Value >> 32);
    } else {
      bool ClearedBankEnables;
      if (!MCA.writeMSR(Index, uint32_t(Regs.GPR[0]) | (Regs.GPR[2] << 32),
                        ClearedBankEnables)) {
        R.Exception = X86MemoryException{13, 0, {}};
        return R;
      }
      if (ClearedBankEnables && Enclaves)
        Enclaves->disableAfterMachineCheck();
    }
  }
  Regs.PC += I.Bytes.size();
  if (I.Mode != X86ExecutionMode::Long64)
    Regs.PC = uint32_t(Regs.PC);
  Regs.RFlags &= ~uint64_t(0x10000);
  auto Controls = Memory.controls();
  Controls.RFlags = Regs.RFlags;
  llvm::cantFail(Memory.bindControls(Controls));
  S.SSBlocked = S.STIBlocked = false;
  R.InstructionCompleted = true;
  if (TF && x86InstructionDebugStep(TF, S.Debug->DebugCTL, false)) {
    x86PublishInstructionDebug(*S.Debug, true);
    R.Exception = X86MemoryException{1, 0, {}};
  }
  return R;
}

} // namespace neverd::analysis::detail
