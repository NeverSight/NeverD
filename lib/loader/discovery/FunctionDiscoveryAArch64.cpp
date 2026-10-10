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
/// PLT form that loads the destination through x17, each perhaps starting
/// with a BTI landing pad, and the PLT form perhaps authenticating x17.
///
//===----------------------------------------------------------------------===//

#include "FunctionDiscoveryDetail.h"

#include "neverd/support/ISAEncoding.h"

#include <cstring>
#include <optional>

namespace neverd {

size_t scanImportThunksAArch64(BinaryImage &Img, const Segment &Seg,
                               const std::map<va_t, size_t> &Targets,
                               std::set<va_t> &Existing,
                               const ImportThunkCandidates &Candidates) {
  using namespace aarch64;
  const uint8_t *D = Seg.Data.data();
  const size_t N = Seg.Data.size();
  auto WordAt = [&](size_t Offset) -> std::optional<uint32_t> {
    if (Offset > N || N - Offset < kInsnSize)
      return std::nullopt;
    uint32_t Word;
    std::memcpy(&Word, D + Offset, kInsnSize);
    return Word;
  };

  size_t Added = 0;
  for (size_t I = 0; I + kThunkLen <= N; I += kInsnSize) {
    // A BTI build starts each veneer with the landing pad an indirect branch
    // may enter, and calls name the veneer there.
    size_t At = I;
    if (WordAt(At) == kBTI_C)
      At += kInsnSize;
    // ELF PLT0 saves the caller's x16 and LR before entering the loader's
    // lazy resolver. That prologue is not an ordinary function entry.
    const bool ResolverHeader =
        Img.isELF() && WordAt(At) == 0xa9bf7bf0u; // stp x16,x30,[sp,#-16]!
    if (ResolverHeader)
      At += kInsnSize;
    const std::optional<uint32_t> W0 = WordAt(At), W1 = WordAt(At + kInsnSize),
                                  W2 = WordAt(At + 2 * kInsnSize);
    if (!W0 || !W1 || !W2 || (*W0 & kADRP_X16_Mask) != kADRP_X16_Match)
      continue;

    size_t End = At + kThunkLen;
    bool Matches =
        (*W1 & kLDR_X16_X16_Mask) == kLDR_X16_X16_Match && *W2 == kBR_X16;
    if (!Matches && (*W1 & kLDR_X16_X16_Mask) == kLDR_X17_X16_Match &&
        (*W2 & kADD_X16_X16_Mask) == kADD_X16_X16_Match) {
      // A PAC build authenticates x17 first, against x16: the cell's address.
      size_t Jump = At + 3 * kInsnSize;
      std::optional<uint32_t> W3 = WordAt(Jump);
      if (W3 == kAUTIA1716 || W3 == kAUTIB1716)
        W3 = WordAt(Jump += kInsnSize);
      const uint32_t LoadOff = ((*W1 >> kLDR_Imm12Shift) & kLDR_Imm12Mask) << 3;
      const uint32_t AddOff = (*W2 >> kLDR_Imm12Shift) & kLDR_Imm12Mask;
      Matches = W3 == kBR_X17 && LoadOff == AddOff;
      End = Jump + kInsnSize;
    }
    if (!Matches)
      continue;
    int32_t ImmHi = (*W0 >> kImmHiShift) & kImmHiMask;
    int32_t ImmLo = (*W0 >> kImmLoShift) & kImmLoMask;
    int64_t Imm = (static_cast<int64_t>(ImmHi) << (kImmLoWidth + kPageShift)) |
                  (static_cast<int64_t>(ImmLo) << kPageShift);
    if (Imm & (1LL << kADRP_ImmBits))
      Imm |= ~((1LL << (kADRP_ImmBits + 1)) - 1);
    const va_t StubVA = Seg.VA + I;
    const size_t ThunkSize = End - I;
    if (!Img.isCodeRange(StubVA, ThunkSize) ||
        !Candidates.allows(StubVA, ThunkSize))
      continue;
    const va_t AdrpVA = Seg.VA + At;
    va_t Page = (AdrpVA & kPageMask) + Imm;
    uint32_t LdrOff = ((*W1 >> kLDR_Imm12Shift) & kLDR_Imm12Mask) << 3;
    va_t Target = Page + LdrOff;
    if (ResolverHeader) {
      if ((*W1 & kLDR_X16_X16_Mask) == kLDR_X17_X16_Match &&
          Img.hasRuntimeCallablePointerSlotAt(
              Target, RuntimeCallablePointerSlotKind::ELFLazyResolver)) {
        Img.recordImportStubRange(StubVA, ThunkSize);
        I = End - kInsnSize;
      }
      continue;
    }
    auto TargetIt = Targets.find(Target);
    if (TargetIt == Targets.end())
      continue;
    Img.recordImportStub(StubVA, TargetIt->second);
    // The veneer's own instructions start no other one.
    I = End - kInsnSize;
    if (!Existing.insert(StubVA).second)
      continue;
    Img.Symbols.push_back(Symbol::makeFunc(StubVA, ThunkSize));
    ++Added;
  }
  return Added;
}

} // namespace neverd
