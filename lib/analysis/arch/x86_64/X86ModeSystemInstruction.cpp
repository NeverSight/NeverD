//===- X86ModeSystemInstruction.cpp - Privileged x86 steps ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordered fetch, debug and retirement for halt, fast calls and bound MSRs.
//===----------------------------------------------------------------------===//

#include "X86ModeSystemInstruction.h"

#include "X86ModeState.h"

#include "neverd/ir/X86/X86FastSystemRules.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

namespace neverd::analysis::detail {
using namespace translate;

namespace {
uint64_t &sysenterRegister(X64ContinuationStateV7 &Bank, uint32_t Index) {
  return Index == 0x174   ? Bank.SysenterCS
         : Index == 0x175 ? Bank.SysenterESP
                          : Bank.SysenterEIP;
}

bool fastTransfer(X86ModeState &S, const X86PagedMemory &Memory,
                  X86ExecutionMode Source, X86ModeSystemOp Op,
                  X86ModeExecutionResult &Out) {
  auto &Regs = modeRegisters(S);
  auto &V = S.Core.Core;
  const bool Enter = Op == X86ModeSystemOp::Sysenter;
  const bool LongTarget = Enter ? bool(Memory.controls().EFER & 0x400)
                                : Op == X86ModeSystemOp::SysexitQ;
  if (!(Memory.controls().CR0 & 1) || !(S.Core.SysenterCS & 0xfffc))
    return false;
  uint64_t PC = Enter ? S.Core.SysenterEIP : Regs.GPR[2];
  uint64_t SP = Enter ? S.Core.SysenterESP : Regs.GPR[1];
  if (!LongTarget) {
    PC = uint32_t(PC);
    SP = uint32_t(SP);
  } else if (!Enter) {
    const unsigned Bits = Memory.controls().CR4 & (1u << 12) ? 57 : 48;
    if (!llvm::isIntN(Bits, int64_t(PC)) || !llvm::isIntN(Bits, int64_t(SP)))
      return false;
  }
  if (Source != X86ExecutionMode::Long64)
    publishNon64(S, projectNon64(S, false), {});
  const unsigned CPL = Enter ? 0 : 3;
  const auto Segment = [&](bool Code) -> X64ContinuationSegment {
    const auto Offset =
        neverd::detail::x86FastSystemSelectorOffset(Enter, LongTarget, Code);
    return {0, UINT32_MAX,
            uint16_t(((S.Core.SysenterCS & 0xfffc) + Offset) | CPL),
            neverd::detail::x86FlatSystemAttributes(Code, LongTarget, CPL)};
  };
  V.Core.CS = Segment(true);
  V.Core.SS = Segment(false);
  V.Unusable &= ~3u;
  S.RetainedRealCS = false;
  Regs.PC = PC;
  Regs.GPR[4] = SP;
  Regs.RFlags &= ~uint64_t(neverd::detail::x86FastSystemClearedFlags(Enter));
  Out.CSWritten = Out.SSWritten = true;
  return true;
}
} // namespace

llvm::Expected<X86ModeExecutionResult> executeModeSystemInstruction(
    X86ModeState &S, X86PagedMemory &Memory, const X86ModeInstruction &I,
    const X86ModeExecutionOptions &O, X86ModeSystemOp Operation, bool Locked,
    X86EventKind &FaultKind, X86SgxMemory *Enclaves) {
  X86ModeExecutionResult R;
  auto &Regs = modeRegisters(S);
  const uint32_t Index = uint32_t(Regs.GPR[1]);
  const bool MSR = Operation == X86ModeSystemOp::ReadMSR ||
                   Operation == X86ModeSystemOp::WriteMSR;
  const bool Fast = !MSR && Operation != X86ModeSystemOp::Halt;
  if (MSR && !sysenterMSR(Index))
    if (auto E = S.MCA->validate())
      return std::move(E);
  const uint8_t CPL = modeCPL(S, Memory.controls());
  const bool TF = Regs.RFlags & 0x100;
  if (TF && !S.Debug)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "single-step system instruction requires the CPU debug bank");
  auto Fetch = fetchX86SystemInstruction(
      S, Memory, I.Bytes, O.Legacy.Segments.FullLimitChoice, R, FaultKind);
  if (!Fetch)
    return Fetch.takeError();
  if (!*Fetch)
    return R;
  const bool Enter = Operation == X86ModeSystemOp::Sysenter;
  const bool EnclaveEnter =
      Enter && Memory.sgxMemory() && Memory.sgxMemory()->active();
  if (Locked || EnclaveEnter || (CPL && !Enter)) {
    R.Exception =
        X86MemoryException{uint8_t(Locked || EnclaveEnter ? 6 : 13), 0, {}};
    return R;
  }
  if (Operation == X86ModeSystemOp::Halt) {
    S.Halted = true;
    R.Status = X86LegacyExecutionStatus::Halted;
  } else if (Fast) {
    if (!fastTransfer(S, Memory, I.Mode, Operation, R)) {
      R.Exception = X86MemoryException{13, 0, {}};
      return R;
    }
  } else {
    if (Operation == X86ModeSystemOp::ReadMSR) {
      std::optional<uint64_t> Value =
          sysenterMSR(Index)
              ? std::optional<uint64_t>(sysenterRegister(S.Core, Index))
              : S.MCA->readMSR(Index);
      if (!Value) {
        R.Exception = X86MemoryException{13, 0, {}};
        return R;
      }
      const uint64_t HighMask =
          I.Mode == X86ExecutionMode::Long64 ? 0 : ~uint64_t(UINT32_MAX);
      Regs.GPR[0] = (Regs.Undefined[0] & HighMask) | uint32_t(*Value);
      Regs.GPR[2] = (Regs.Undefined[2] & HighMask) | (*Value >> 32);
    } else {
      const uint64_t Value = uint32_t(Regs.GPR[0]) | (Regs.GPR[2] << 32);
      bool ClearedBankEnables = false;
      if (sysenterMSR(Index)) {
        const bool FullAddress = Index != 0x174 && Memory.features().LongMode;
        if (FullAddress &&
            !llvm::isIntN(Memory.features().LA57 ? 57 : 48, int64_t(Value))) {
          R.Exception = X86MemoryException{13, 0, {}};
          return R;
        }
        sysenterRegister(S.Core, Index) = FullAddress ? Value : uint32_t(Value);
      } else if (!S.MCA->writeMSR(Index, Value, ClearedBankEnables)) {
        R.Exception = X86MemoryException{13, 0, {}};
        return R;
      }
      if (ClearedBankEnables && Enclaves)
        Enclaves->disableAfterMachineCheck();
    }
  }
  if (!Fast) {
    Regs.PC += I.Bytes.size();
    if (I.Mode != X86ExecutionMode::Long64)
      Regs.PC = uint32_t(Regs.PC);
  }
  Regs.RFlags &= ~uint64_t(0x10000);
  auto Controls = Memory.controls();
  Controls.RFlags = Regs.RFlags;
  llvm::cantFail(Memory.bindControls(Controls));
  S.SSBlocked = S.STIBlocked = false;
  R.InstructionCompleted = true;
  if (TF && x86InstructionDebugStep(TF, S.Debug->DebugCTL, Fast)) {
    x86PublishInstructionDebug(*S.Debug, true);
    R.Exception = X86MemoryException{1, 0, {}};
  }
  return R;
}

} // namespace neverd::analysis::detail
