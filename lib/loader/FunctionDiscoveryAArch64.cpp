//===- FunctionDiscoveryAArch64.cpp - AArch64 import thunk scan --*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 import thunk recognition for heuristic function discovery.
/// Recognizes the ADRP x16 / LDR x16,[x16,#off] / BR x16 PLT-style veneer.
///
//===----------------------------------------------------------------------===//

#include "FunctionDiscoveryDetail.h"

#include "neverd/Support/ISAEncoding.h"

#include <cstring>

namespace neverd {

size_t scanImportThunksAArch64(BinaryImage &Img, const Segment &Seg,
                               const std::set<va_t> &Targets,
                               std::set<va_t> &Existing) {
  using namespace aarch64;
  const uint8_t *D = Seg.Data.data();
  size_t N = Seg.Data.size();
  if (N < kThunkLen)
    return 0;

  size_t Added = 0;
  for (size_t I = 0; I + kThunkLen <= N; I += kInsnSize) {
    uint32_t W0, W1, W2;
    std::memcpy(&W0, D + I, kInsnSize);
    std::memcpy(&W1, D + I + kInsnSize, kInsnSize);
    std::memcpy(&W2, D + I + 2 * kInsnSize, kInsnSize);
    if ((W0 & kADRP_X16_Mask) != kADRP_X16_Match)
      continue;
    if ((W1 & kLDR_X16_X16_Mask) != kLDR_X16_X16_Match)
      continue;
    if (W2 != kBR_X16)
      continue;
    int32_t ImmHi = (W0 >> kImmHiShift) & kImmHiMask;
    int32_t ImmLo = (W0 >> kImmLoShift) & kImmLoMask;
    int64_t Imm = (static_cast<int64_t>(ImmHi) << (kImmLoWidth + kPageShift)) |
                  (static_cast<int64_t>(ImmLo) << kPageShift);
    if (Imm & (1LL << kADRP_ImmBits))
      Imm |= ~((1LL << (kADRP_ImmBits + 1)) - 1);
    va_t AdrpVA = Seg.VA + I;
    va_t Page = (AdrpVA & kPageMask) + Imm;
    uint32_t LdrOff = ((W1 >> kLDR_Imm12Shift) & kLDR_Imm12Mask) << 3;
    va_t Target = Page + LdrOff;
    if (!Targets.count(Target))
      continue;
    if (!Existing.insert(AdrpVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(AdrpVA, kThunkLen));
    ++Added;
  }
  return Added;
}

} // namespace neverd
