//===- X86SmmExecution.cpp - Software SMI and RSM -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// KVM's 64-bit SMM save map. Offsets follow Linux arch/x86/kvm/smm.h.
/// System RAM is a separate address space. RSM consumes the guest's current
/// save image, including firmware edits, independently of the previous CPU.
//===----------------------------------------------------------------------===//

#include "X86SmmExecution.h"

#include "X86ModeState.h"
#include "X86SgxExecution.h"

#include "neverd/ir/X86/X86DebugRegisterRules.h"
#include "neverd/translate/X86/X86ControlRegisters.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"

namespace neverd::analysis::detail {
using namespace translate;

namespace {
using Image = std::array<uint8_t, 512>;
using Status = X86LegacyExecutionStatus;
namespace le = llvm::support::endian;
constexpr unsigned SaveOffset = 0xfe00;
constexpr unsigned EntryOffset = 0x8000;
constexpr uint32_t Revision = 0x20064;

void putSegment(Image &B, unsigned Offset, X64ContinuationSegment D,
                bool Unusable) {
  le::write16le(B.data() + Offset, D.Selector);
  le::write16le(B.data() + Offset + 2,
                Unusable ? D.Attributes & ~0x80u : D.Attributes);
  le::write32le(B.data() + Offset + 4, D.Limit);
  le::write64le(B.data() + Offset + 8, D.Base);
}

X64ContinuationSegment segment(const Image &B, unsigned Offset) {
  return {le::read64le(B.data() + Offset + 8),
          le::read32le(B.data() + Offset + 4), le::read16le(B.data() + Offset),
          uint16_t(le::read16le(B.data() + Offset + 2) & 0xf0ff)};
}

void putTable(Image &B, unsigned Offset,
              const X64ContinuationDescriptorTable &T) {
  le::write32le(B.data() + Offset + 4, T.Limit);
  le::write64le(B.data() + Offset + 8, T.Base);
}

X64ContinuationDescriptorTable table(const Image &B, unsigned Offset) {
  return {le::read64le(B.data() + Offset + 8),
          le::read16le(B.data() + Offset + 4)};
}

Image save(const X86ModeState &S, const X86PagingState &C) {
  Image B{};
  const auto &V = S.Core.Core;
  const auto &R = modeRegisters(S);
  putSegment(B, 0x00, V.ES, V.Unusable & 8);
  putSegment(B, 0x10, V.Core.CS, V.Unusable & 1);
  putSegment(B, 0x20, V.Core.SS, V.Unusable & 2);
  putSegment(B, 0x30, V.DS, V.Unusable & 4);
  putSegment(B, 0x40, {R.FSBase, V.FS.Limit, V.FS.Selector, V.FS.Attributes},
             V.Unusable & 16);
  putSegment(B, 0x50, {R.GSBase, V.GS.Limit, V.GS.Selector, V.GS.Attributes},
             V.Unusable & 32);
  putTable(B, 0x60, V.GDTR);
  putSegment(B, 0x70, V.LDTR, V.Unusable & 64);
  putTable(B, 0x80, V.IDTR);
  putSegment(B, 0x90, V.TR, V.Unusable & 128);
  B[0xcb] = uint8_t(S.SSBlocked) | (uint8_t(S.STIBlocked) << 1);
  le::write64le(B.data() + 0xd0, C.EFER);
  le::write32le(B.data() + 0xfc, Revision);
  le::write32le(B.data() + 0x100, S.SMM->SMBase);
  le::write64le(B.data() + 0x148, C.CR4);
  le::write64le(B.data() + 0x150, C.CR3);
  le::write64le(B.data() + 0x158, C.CR0);
  le::write64le(B.data() + 0x160, S.Debug->DR7);
  le::write64le(B.data() + 0x168, S.Debug->DR6);
  le::write64le(B.data() + 0x170, R.RFlags);
  le::write64le(B.data() + 0x178, R.PC);
  for (unsigned I = 0; I != 16; ++I)
    le::write64le(B.data() + 0x1f8 - 8 * I, R.GPR[I]);
  return B;
}

void restore(const Image &B, X86ModeState &S, X86PagingState &C) {
  auto &V = S.Core.Core;
  auto &R = modeRegisters(S);
  V.Unusable = 0;
  const auto Get = [&](unsigned Offset, unsigned Bit) {
    auto D = segment(B, Offset);
    if (!(D.Attributes & 0x80))
      V.Unusable |= 1u << Bit;
    return D;
  };
  V.ES = Get(0x00, 3);
  V.Core.CS = Get(0x10, 0);
  V.Core.SS = Get(0x20, 1);
  V.DS = Get(0x30, 2);
  auto FS = Get(0x40, 4), GS = Get(0x50, 5);
  V.FS = {FS.Limit, FS.Selector, FS.Attributes};
  V.GS = {GS.Limit, GS.Selector, GS.Attributes};
  R.FSBase = FS.Base;
  R.GSBase = GS.Base;
  V.GDTR = table(B, 0x60);
  V.LDTR = Get(0x70, 6);
  V.IDTR = table(B, 0x80);
  V.TR = Get(0x90, 7);
  S.SSBlocked = B[0xcb] & 1;
  S.STIBlocked = B[0xcb] & 2;
  C.CR4 = le::read64le(B.data() + 0x148);
  C.CR3 = le::read64le(B.data() + 0x150);
  C.CR0 = le::read64le(B.data() + 0x158);
  C.EFER = le::read64le(B.data() + 0xd0) & ~uint64_t(0x400);
  if ((C.CR0 & 0x80000000) && (C.EFER & 0x100))
    C.EFER |= 0x400;
  S.Debug->DR7 = le::read64le(B.data() + 0x160);
  S.Debug->DR6 = le::read64le(B.data() + 0x168);
  C.RFlags = R.RFlags = le::read64le(B.data() + 0x170) | 2;
  R.PC = le::read64le(B.data() + 0x178);
  for (unsigned I = 0; I != 16; ++I)
    R.GPR[I] = le::read64le(B.data() + 0x1f8 - 8 * I);
  S.SMM->SMBase = le::read32le(B.data() + 0x100);
  if (!S.SMM->EnteredInsideNMI)
    V.NMIBlocked = false;
  S.RetainedRealCS = (C.CR0 & 1) && !(R.RFlags & 0x20000) &&
                     S.SMM->InterruptedRealCS == V.Core.CS;
  S.SMM->InterruptedRealCS.reset();
  S.SMM->EnteredInsideNMI = S.SMM->Active = false;
}

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}
} // namespace

llvm::Expected<X86ModeExecutionResult>
enterX86Smm(X86ModeState &S, X86PagedMemory &Normal, X86PagedMemory &SMRAM) {
  if (!S.Debug || !Normal.features().LongMode ||
      (Normal.controls().CR4 & (uint64_t{1} << 23)))
    return invalid("KVM64 SMM requires the debug bank, long-mode capability "
                   "and non-CET state");
  auto C = Normal.controls();
  C.CR0 &= ~uint64_t(0x8000000d);
  C.CR4 = C.EFER = 0;
  C.RFlags = 2;
  if (auto E = validateX86PagingState(SMRAM.features(), C))
    return std::move(E);
  if (SMRAM.sgxMemory() && SMRAM.sgxMemory()->active())
    return invalid("the system-management address space requires an inactive "
                   "enclave context");
  if (Normal.sgxMemory() && Normal.sgxMemory()->active()) {
    auto Probe = SMRAM.physicalMemory().validateStoreBytes(
        uint64_t(S.SMM->SMBase) + SaveOffset, Image{}.size());
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
  auto B = save(S, Normal.controls());
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
              const X86ModeExecutionOptions &Options,
              X86RsmProgress &Progress) {
  if ((S.SMM->Active && !S.Debug) || (modeRegisters(S).RFlags & 0x100) ||
      (S.Debug && (S.Debug->DR7 & 0xff)))
    return invalid(
        "software RSM requires an inactive instruction-debug producer");
  X86ModeExecutionResult R;
  if (!Fetched) {
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
    Image B;
    auto Read = SMRAM.physicalMemory().loadBytes(
        uint64_t(S.SMM->SMBase) + SaveOffset, B);
    if (Read.Status == GuestMemoryAccessStatus::Fault) {
      R.Status = Status::BackingFault;
      R.BackingFault = Read.Fault;
      return R;
    }
    Progress.SaveArea = std::move(B);
  }
  auto Next = S;
  auto C = SMRAM.controls();
  restore(*Progress.SaveArea, Next, C);
  const auto Stop = [&] {
    S.Shutdown = true;
    R.Status = Status::Shutdown;
    return R;
  };
  const uint64_t AllowedEFER = uint64_t(Options.EFERSystemCall) |
                               (Normal.features().LongMode ? 0x500u : 0u) |
                               (Normal.features().NX ? 0x800u : 0u);
  const uint32_t Other = Options.Legacy.ControlRegisters
                             ? Options.Legacy.ControlRegisters->OtherCR4Features
                             : 0;
  if ((C.CR0 >> 32) || ((C.CR0 & 0x20000000) && !(C.CR0 & 0x40000000)) ||
      (C.CR4 & ~uint64_t(x86SupportedCR4(Normal.features(), Other))) ||
      (le::read64le(Progress.SaveArea->data() + 0xd0) & ~AllowedEFER) ||
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
  return R;
}

} // namespace neverd::analysis::detail
