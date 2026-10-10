//===- FunctionDiscoveryX86.cpp - x86/x86-64 import thunk scan ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 and x86-64 import thunk recognition for heuristic function discovery.
/// Recognizes the indirect-jump trampolines emitted for PE IAT entries and
/// ELF PLT slots (jmp [rip+disp32] on x86-64, jmp [abs32] on x86, and the
/// PIC PLT entry's jmp [ebx+disp32] from the DT_PLTGOT base in EBX), including
/// the indirect-branch-tracking form that starts with an endbr marker and may
/// give the jump a bnd prefix (`.plt.sec`).
///
//===----------------------------------------------------------------------===//

#include "FunctionDiscoveryDetail.h"

#include "neverd/support/ISAEncoding.h"

#include <cstring>

namespace neverd {

size_t scanImportThunksX86(BinaryImage &Img, const Segment &Seg,
                           const std::map<va_t, size_t> &Targets,
                           std::set<va_t> &Existing,
                           const ImportThunkCandidates &Candidates) {
  const uint8_t *D = Seg.Data.data();
  size_t N = Seg.Data.size();
  if (N < x86::kJmpIndirectLen)
    return 0;

  const bool Is64 = Img.Arch == Arch::X64;
  // An i386 PIC PLT entry jumps through its GOT entry by the base the
  // dynamic section names, DT_PLTGOT, which its caller holds in EBX.
  const va_t PltGot = Is64 ? 0 : Img.DynInfo.PltGotAddr;
  size_t Added = 0;
  for (size_t I = 0; I + x86::kJmpIndirectLen <= N; ++I) {
    const bool ThroughGOT = PltGot && D[I] == x86::kJmpIndirectOp &&
                            D[I + 1] == x86::kJmpIndirectGOTModRM;
    if (!ThroughGOT &&
        (D[I] != x86::kJmpIndirectOp || D[I + 1] != x86::kJmpIndirectModRM))
      continue;
    va_t InsnVA = Seg.VA + I;
    va_t Target;
    if (ThroughGOT) {
      int32_t Disp;
      std::memcpy(&Disp, D + I + x86::kJmpIndirectDispOffset, sizeof(Disp));
      Target = static_cast<uint32_t>(PltGot + static_cast<int64_t>(Disp));
    } else if (Is64) {
      int32_t Disp;
      std::memcpy(&Disp, D + I + x86::kJmpIndirectDispOffset, sizeof(Disp));
      Target = InsnVA + x86::kJmpIndirectLen + static_cast<int64_t>(Disp);
    } else {
      uint32_t AbsAddr;
      std::memcpy(&AbsAddr, D + I + x86::kJmpIndirectDispOffset,
                  sizeof(AbsAddr));
      Target = AbsAddr;
    }
    auto TargetIt = Targets.find(Target);
    if (TargetIt == Targets.end())
      continue;
    // An endbr marker right before the jump starts the thunk, and a bnd
    // prefix between them belongs to the jump.  A lone F2 byte may end the
    // previous instruction, so it only counts after an endbr.
    size_t Start = I;
    const auto EndbrBefore = [&](size_t At) {
      if (At < x86::kEndbrLen)
        return false;
      uint32_t Word;
      std::memcpy(&Word, D + At - x86::kEndbrLen, sizeof(Word));
      return Word == (Is64 ? x86::kEndbr64Word : x86::kEndbr32Word);
    };
    if (EndbrBefore(Start))
      Start -= x86::kEndbrLen;
    else if (Start > 0 && D[Start - 1] == x86::kBndPrefix &&
             EndbrBefore(Start - 1))
      Start -= 1 + x86::kEndbrLen;
    const va_t ThunkVA = Seg.VA + Start;
    const size_t ThunkLen = I + x86::kJmpIndirectLen - Start;
    if (!Img.isCodeRange(ThunkVA, ThunkLen) ||
        !Candidates.allows(ThunkVA, ThunkLen))
      continue;
    Img.recordImportStub(ThunkVA, TargetIt->second);
    if (!Existing.insert(ThunkVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(ThunkVA, ThunkLen));
    ++Added;
  }
  return Added;
}

} // namespace neverd
