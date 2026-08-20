//===- FunctionDiscoveryAArch64.cpp - AArch64 import thunk scan --*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 import thunk recognition for heuristic function discovery.
/// Recognizes both three-instruction import veneers and the four-instruction
/// PLT form that loads the destination through x17.
///
//===----------------------------------------------------------------------===//

#include "FunctionDiscoveryDetail.h"

#include "neverd/support/ISAEncoding.h"

#include <cstring>

namespace neverd {

size_t scanImportThunksAArch64(BinaryImage &Img, const Segment &Seg,
                               const std::map<va_t, size_t> &Targets,
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

    size_t ThunkSize = kThunkLen;
    bool Matches =
        (W1 & kLDR_X16_X16_Mask) == kLDR_X16_X16_Match && W2 == kBR_X16;
    if (!Matches && I + kELFPLTThunkLen <= N &&
        (W1 & kLDR_X16_X16_Mask) == kLDR_X17_X16_Match &&
        (W2 & kADD_X16_X16_Mask) == kADD_X16_X16_Match) {
      uint32_t W3;
      std::memcpy(&W3, D + I + 3 * kInsnSize, kInsnSize);
      const uint32_t LoadOff = ((W1 >> kLDR_Imm12Shift) & kLDR_Imm12Mask) << 3;
      const uint32_t AddOff = (W2 >> kLDR_Imm12Shift) & kLDR_Imm12Mask;
      Matches = W3 == kBR_X17 && LoadOff == AddOff;
      ThunkSize = kELFPLTThunkLen;
    }
    if (!Matches)
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
    auto TargetIt = Targets.find(Target);
    if (TargetIt == Targets.end())
      continue;
    Img.recordImportStub(AdrpVA, TargetIt->second);
    if (!Existing.insert(AdrpVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(AdrpVA, ThunkSize));
    ++Added;
  }
  return Added;
}

} // namespace neverd
