//===- X86LegacyControlRegisters.cpp - Protected control transitions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Intel MOV-CR checks and atomic legacy/PAE paging-register publication.
//===----------------------------------------------------------------------===//

#include "X86LegacyControlRegisters.h"

#include "llvm/Support/Errc.h"

namespace neverd::translate::detail {
namespace {

constexpr uint32_t PE = 1, ET = 0x10, NW = 1u << 29, CD = 1u << 30,
                   PG = 1u << 31, PAE = 1u << 5, PCIDE = 1u << 17;
constexpr uint64_t LME = 1u << 8;
constexpr uint32_t CR0Bits = 0xe005003f;

uint32_t supportedCR4(const X86PagingFeatures &F,
                      const X86LegacyControlRegisterBinding &B) {
  return B.OtherCR4Features | (uint32_t(F.PSE) << 4) | (uint32_t(F.PAE) << 5) |
         (uint32_t(F.LA57) << 12) | (uint32_t(F.SMEP) << 20) |
         (uint32_t(F.SMAP) << 21) | (uint32_t(F.PKU) << 22) |
         (uint32_t(F.PKS) << 24);
}

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

llvm::Error
validateLegacyControlBinding(const X86PagedMemory &Memory,
                             const X86LegacyControlRegisterBinding &Binding) {
  if (Binding.OtherCR4Features & ~kX86LegacyOtherCR4FeatureMask)
    return invalid("MOV-CR requires paging features at their paging owner");
  const auto &C = Memory.controls();
  if ((C.CR4 & ~uint64_t(supportedCR4(Memory.features(), Binding))) ||
      (C.CR4 & PCIDE) || (C.CR0 & ~uint64_t(CR0Bits)) || !(C.CR0 & ET) ||
      ((C.CR0 & NW) && !(C.CR0 & CD)))
    return invalid("MOV-CR requires controls valid for the bound Intel CPU");
  return llvm::Error::success();
}

llvm::Expected<X86MemoryAccessResult>
executeLegacyControlRegister(X86LegacyExecutionState &Next, X86PagingState &C,
                             X86PagedMemory &Memory,
                             const X86LegacyControlInstruction &I,
                             const X86LegacyControlRegisterBinding &Binding) {
  if (I.Locked ||
      (I.Control != 0 && I.Control != 2 && I.Control != 3 && I.Control != 4))
    return fault(6);
  if (Next.Core.CPL)
    return fault(13);
  auto &Value = Next.Core.GPR[I.General];
  if (!I.Write) {
    Value = I.Control == 0   ? C.CR0
            : I.Control == 2 ? Next.CR2
            : I.Control == 3 ? C.CR3
                             : C.CR4;
    return X86MemoryAccessResult{};
  }
  const auto Old = C;
  switch (I.Control) {
  case 0:
    C.CR0 = (Value & CR0Bits) | ET;
    if (((C.CR0 & PG) && !(C.CR0 & PE)) || ((C.CR0 & NW) && !(C.CR0 & CD)))
      return fault(13);
    if ((C.CR0 & PG) && (C.EFER & LME)) {
      if (!(C.CR4 & PAE) || (Next.Core.Segments[1].Attributes & 0x2000) ||
          (Next.TR.Attributes & 15) == 3)
        return fault(13);
      return invalid(
          "MOV-CR activation of IA-32e requires a mode-transfer owner");
    }
    if (!(C.CR0 & PE))
      return invalid("MOV-CR real-mode entry requires a mode-transfer owner");
    break;
  case 2:
    Next.CR2 = Value;
    break;
  case 3:
    C.CR3 = Value;
    break;
  case 4:
    if ((Value & ~supportedCR4(Memory.features(), Binding)) ||
        (Value & PCIDE) || ((Value & (1u << 23)) && !(C.CR0 & (1u << 16))))
      return fault(13);
    C.CR4 = Value;
    break;
  }
  const bool Reload = (C.CR0 & PG) && (C.CR4 & PAE) &&
                      (I.Control == 3 || ((Old.CR0 ^ C.CR0) & (CD | NW | PG)) ||
                       ((Old.CR4 ^ C.CR4) & (PAE | 0x90 | (1u << 20))));
  if (Reload) {
    auto Loaded = loadX86PAEPDPTE(Memory.physicalMemory(), Memory.features(),
                                  C.CR3, C.PDPTE);
    if (!Loaded)
      return Loaded.takeError();
    if (Loaded->Status != X86PageWalkStatus::Completed) {
      X86MemoryAccessResult R;
      R.Status = Loaded->Status;
      R.Exception = Loaded->Exception;
      R.BackingFault = Loaded->BackingFault;
      return R;
    }
  }
  if (auto Error = validateX86PagingState(Memory.features(), C))
    return std::move(Error);
  return X86MemoryAccessResult{};
}

} // namespace neverd::translate::detail
