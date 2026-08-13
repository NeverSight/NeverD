//===- FunctionDiscoveryX86.cpp - x86/x86-64 import thunk scan ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 and x86-64 import thunk recognition for heuristic function discovery.
/// Recognizes the indirect-jump trampolines emitted for PE IAT entries
/// (jmp [rip+disp32] on x86-64, jmp [abs32] on x86).
///
//===----------------------------------------------------------------------===//

#include "FunctionDiscoveryDetail.h"

#include "neverd/support/ISAEncoding.h"

#include <cstring>

namespace neverd {

size_t scanImportThunksX86(BinaryImage &Img, const Segment &Seg,
                           const std::map<va_t, size_t> &Targets,
                           std::set<va_t> &Existing) {
  const uint8_t *D = Seg.Data.data();
  size_t N = Seg.Data.size();
  if (N < x86::kJmpIndirectLen)
    return 0;

  const bool Is64 = Img.Arch == Arch::X64;
  size_t Added = 0;
  for (size_t I = 0; I + x86::kJmpIndirectLen <= N; ++I) {
    if (D[I] != x86::kJmpIndirectOp || D[I + 1] != x86::kJmpIndirectModRM)
      continue;
    va_t InsnVA = Seg.VA + I;
    va_t Target;
    if (Is64) {
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
    Img.recordImportStub(InsnVA, TargetIt->second);
    if (!Existing.insert(InsnVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(InsnVA, x86::kJmpIndirectLen));
    ++Added;
  }
  return Added;
}

} // namespace neverd
