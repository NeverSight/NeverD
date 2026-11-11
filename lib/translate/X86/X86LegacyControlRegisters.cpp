//===- X86LegacyControlRegisters.cpp - Protected control transitions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Intel MOV-CR checks and atomic legacy/PAE paging-register publication.
//===----------------------------------------------------------------------===//

#include "X86LegacyControlRegisters.h"

#include "neverd/translate/X86/X86ControlRegisters.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>

namespace neverd::translate::detail {
namespace {

constexpr uint32_t PE = 1, ET = 0x10, NW = 1u << 29, CD = 1u << 30,
                   PCIDE = 1u << 17;

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

X86MemoryAccessResult fault(uint8_t Vector) {
  X86MemoryAccessResult R;
  R.Status = X86PageWalkStatus::Exception;
  R.Exception = X86MemoryException{Vector, 0, {}};
  return R;
}

} // namespace

namespace {

bool sameCache(const X86LegacySegmentCache &A, const X86LegacySegmentCache &B) {
  return std::tie(A.Base, A.Limit, A.Selector, A.Attributes, A.Unusable) ==
         std::tie(B.Base, B.Limit, B.Selector, B.Attributes, B.Unusable);
}

bool sameDebug(const std::optional<X86LegacyDebugState> &A,
               const std::optional<X86LegacyDebugState> &B) {
  return bool(A) == bool(B) &&
         (!A ||
          std::tie(A->DR, A->DR6, A->DR7, A->DebugCTL, A->PendingData) ==
              std::tie(B->DR, B->DR6, B->DR7, B->DebugCTL, B->PendingData));
}

} // namespace

struct X86LegacyControlContinuation {
  X86LegacyExecutionState Entry;
  X86PagingState Controls;
  X86PagingFeatures Features;
  llvm::SmallVector<uint8_t> Instruction;
  uint32_t OtherCR4Features;
  std::optional<X86LegacyDebugProfile> DebugProfile;
  X86PAEPDPTEContinuation PDPTE;
  std::optional<X86Non64ModeBinding> Mode;

  X86LegacyControlContinuation(const X86LegacyExecutionState &S,
                               const X86PagedMemory &M,
                               llvm::ArrayRef<uint8_t> Bytes,
                               const X86LegacyExecutionOptions &O)
      : Entry(S), Controls(M.controls()), Features(M.features()),
        Instruction(Bytes.begin(), Bytes.end()),
        OtherCR4Features(O.ControlRegisters->OtherCR4Features),
        DebugProfile(O.Debug), Mode(O.Non64Mode) {}

  bool matches(const X86LegacyExecutionState &S, const X86PagedMemory &M,
               const X86LegacyExecutionOptions &O) const {
    const auto &E = Entry;
    const auto &A = E.Core;
    const auto &B = S.Core;
    return E.EIP == S.EIP && E.EFlags == S.EFlags && E.CR2 == S.CR2 &&
           E.IDTRBase == S.IDTRBase && E.IDTRLimit == S.IDTRLimit &&
           E.STIBlocked == S.STIBlocked && E.SSBlocked == S.SSBlocked &&
           E.NMIBlocked == S.NMIBlocked && E.Halted == S.Halted &&
           E.Sysenter.CS == S.Sysenter.CS && E.Sysenter.ESP == S.Sysenter.ESP &&
           E.Sysenter.EIP == S.Sysenter.EIP && E.Mpx == S.Mpx &&
           A.GPR == B.GPR && A.CPL == B.CPL && A.GDTRBase == B.GDTRBase &&
           A.GDTRLimit == B.GDTRLimit && sameCache(A.LDTR, B.LDTR) &&
           sameCache(E.TR, S.TR) && sameDebug(E.Debug, S.Debug) &&
           std::equal(A.Segments.begin(), A.Segments.end(), B.Segments.begin(),
                      sameCache) &&
           Controls == M.controls() && Features == M.features() &&
           O.ControlRegisters &&
           OtherCR4Features == O.ControlRegisters->OtherCR4Features &&
           DebugProfile == O.Debug && Mode == O.Non64Mode;
  }
};

llvm::Error validateLegacyControlContinuation(
    const X86LegacyExecutionState &S, const X86PagedMemory &M,
    const X86LegacyExecutionOptions &O,
    std::optional<llvm::ArrayRef<uint8_t>> Bytes) {
  if (!S.PendingControl)
    return llvm::Error::success();
  const auto &P = *S.PendingControl;
  if (S.PendingEvent || S.PendingStack || S.PendingMpx || S.PendingTask ||
      S.PendingTransfer || (Bytes && *Bytes != llvm::ArrayRef(P.Instruction)) ||
      !P.matches(S, M, O))
    return invalid("resume the retained MOV-CR instruction and state");
  return llvm::Error::success();
}

uint64_t legacyControlInstructionKey(const X86LegacyExecutionState &S) {
  const auto &E = S.PendingControl->Entry;
  const auto &CS = E.Core.Segments[1];
  return uint32_t(CS.Base + E.EIP) |
         (uint64_t(bool(CS.Attributes & 0x4000)) << 32);
}

llvm::Error
validateLegacyControlBinding(const X86PagedMemory &Memory,
                             const X86LegacyControlRegisterBinding &Binding) {
  if (Binding.OtherCR4Features & ~kX86LegacyOtherCR4FeatureMask)
    return invalid("MOV-CR requires paging features at their paging owner");
  const auto &C = Memory.controls();
  if ((C.CR4 & ~uint64_t(x86SupportedCR4(Memory.features(),
                                         Binding.OtherCR4Features))) ||
      ((C.CR4 & PCIDE) && !(C.EFER & 0x400)) ||
      (C.CR0 & ~uint64_t(kX86CR0StoredMask)) || !(C.CR0 & ET) ||
      ((C.CR0 & NW) && !(C.CR0 & CD)))
    return invalid("MOV-CR requires controls valid for the bound Intel CPU");
  return llvm::Error::success();
}

llvm::Expected<X86MemoryAccessResult> executeLegacyControlRegister(
    X86LegacyExecutionState &Next, X86PagingState &C, X86PagedMemory &Memory,
    const X86LegacyControlInstruction &I, llvm::ArrayRef<uint8_t> Bytes,
    const X86LegacyExecutionOptions &Options) {
  const auto &Binding = *Options.ControlRegisters;
  const auto Fault = [&](uint8_t Vector) {
    Next.PendingControl.reset();
    return fault(Vector);
  };
  if (I.Locked ||
      (I.Control != 0 && I.Control != 2 && I.Control != 3 && I.Control != 4))
    return Fault(6);
  if (Next.Core.CPL)
    return Fault(13);
  auto &Value = Next.Core.GPR[I.General];
  if (!I.Write) {
    Value = I.Control == 0   ? C.CR0
            : I.Control == 2 ? Next.CR2
            : I.Control == 3 ? C.CR3
                             : C.CR4;
    return X86MemoryAccessResult{};
  }
  bool Reload = false;
  if (I.Control == 2) {
    Next.CR2 = Value;
  } else {
    auto Plan = planX86PagingControlWrite(
        C, Memory.features(), Binding.OtherCR4Features, I.Control, Value,
        bool(Next.Core.Segments[1].Attributes & 0x2000),
        (Next.TR.Attributes & 15) == 3);
    if (Plan.Exception)
      return Fault(Plan.Exception->Vector);
    if (!(Plan.Controls.CR0 & PE))
      return invalid("MOV-CR real-mode entry requires a mode-transfer owner");
    if ((Plan.Controls.EFER & 0x400) && !Options.Non64Mode)
      return invalid(
          "MOV-CR activation of IA-32e requires a mode-transfer owner");
    C = Plan.Controls;
    Reload = Plan.ReloadPDPTE;
  }
  // Admit the requested execution domain before physical reads. Old cached
  // PDPTE bits are irrelevant when this instruction will replace that bank.
  auto Candidate = C;
  if (Reload)
    Candidate.PDPTE = {};
  if (auto Error = validateX86PagingState(Memory.features(), Candidate))
    return std::move(Error);
  if (Next.Debug && !x86InstructionDebugValid(
                        Next.Debug->DR6, Next.Debug->DR7, Next.Debug->DebugCTL,
                        *Options.Debug, C.CR4, Next.Debug->PendingData))
    return invalid("MOV-CR requests unbound debug controls");
  if (Reload) {
    auto Progress =
        Next.PendingControl
            ? *Next.PendingControl
            : X86LegacyControlContinuation(Next, Memory, Bytes, Options);
    auto Loaded = loadX86PAEPDPTE(Memory.physicalMemory(), Memory.features(),
                                  C.CR3, C.PDPTE, &Progress.PDPTE);
    if (!Loaded)
      return Loaded.takeError();
    Next.PendingControl = Loaded->Status == X86PageWalkStatus::BackingFault
                              ? std::make_shared<X86LegacyControlContinuation>(
                                    std::move(Progress))
                              : nullptr;
    if (Loaded->Status != X86PageWalkStatus::Completed) {
      X86MemoryAccessResult R;
      R.Status = Loaded->Status;
      R.Exception = Loaded->Exception;
      R.BackingFault = Loaded->BackingFault;
      return R;
    }
  }
  return X86MemoryAccessResult{};
}

} // namespace neverd::translate::detail
