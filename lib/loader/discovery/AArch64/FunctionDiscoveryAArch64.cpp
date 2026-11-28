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

#include "../FunctionDiscoveryDetail.h"

#include "neverd/loader/DirectBranch.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/ProloguePatterns.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace neverd {

static size_t scanVeneers(BinaryImage &Img, const Segment &Seg,
                          const std::map<va_t, size_t> &Targets,
                          std::set<va_t> &Existing,
                          const ImportThunkCandidates &Candidates,
                          std::optional<size_t> OnlyStart) {
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

  if (N < kThunkLen || (OnlyStart && *OnlyStart > N - kThunkLen))
    return 0;
  size_t Added = 0;
  const size_t Last = OnlyStart.value_or(N - kThunkLen);
  for (size_t I = OnlyStart.value_or(0); I <= Last; I += kInsnSize) {
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

size_t scanImportThunksAArch64(BinaryImage &Img, const Segment &Seg,
                               const std::map<va_t, size_t> &Targets,
                               std::set<va_t> &Existing,
                               const ImportThunkCandidates &Candidates) {
  return scanVeneers(Img, Seg, Targets, Existing, Candidates, std::nullopt);
}

/// File-backed fallthrough when a PE has no unwind or symbol size for this
/// entry. Stop at the end of control flow or the next known function.
static size_t fallthroughBytes(const BinaryImage &Img, va_t Entry) {
  const Segment *Seg = Img.getSegmentFor(Entry);
  if (!Seg || Entry - Seg->VA >= Seg->Data.size())
    return 0;
  const size_t Offset = Entry - Seg->VA;
  uint64_t Available = Seg->Data.size() - Offset;
  if (const Section *Sec = Img.getSectionFor(Entry))
    Available = std::min(Available, Sec->Size - (Entry - Sec->VA));
  const va_t Next = Img.nextKnownFunctionEntryAfter(Entry);
  if (Next != InvalidVA)
    Available = std::min(Available, Next - Entry);
  constexpr size_t Width = aarch64::kInsnSize;
  for (size_t At = 0; Available - At >= Width; At += Width)
    if (isAArch64BlockEndWord(readLE<uint32_t>(Seg->Data.data() + Offset + At)))
      return At + Width;
  return Available;
}

void scanSelectedImportThunksAArch64(BinaryImage &Img,
                                     const std::map<va_t, size_t> &Targets) {
  if (Targets.empty())
    return;
  // A direct BL may name a linker veneer outside the caller's unwind range.
  // Test only the selected entries and the branch targets in their known
  // bodies, or in their fallthrough when the binary records no extent.
  std::set<va_t> Starts = Img.LoadOnlyFunctionEntries;
  for (va_t Entry : Img.LoadOnlyFunctionEntries) {
    if (!Img.isFunctionEntryCandidateAt(Entry))
      continue;
    uint64_t Size;
    if (const auto *EH = Img.ExceptionMetadata.findFunction(Entry);
        EH && EH->CodeRange.contains(Entry))
      Size = EH->CodeRange.End - Entry;
    else if (const auto *Sym = Img.findSymbolAt(Entry);
             Sym && Sym->IsFunc && Sym->Size)
      Size = Sym->Size;
    else
      Size = fallthroughBytes(Img, Entry);
    if (!Img.isCodeRange(Entry, Size))
      continue;
    if (const uint8_t *Bytes = Img.readVA(Entry, Size))
      forEachDirectBranch(Img.Arch, InstructionMode::Default, Bytes, Size,
                          Entry,
                          [&](va_t, va_t Target) { Starts.insert(Target); });
  }

  // Most selected functions have no import veneer among their callees.
  // Build the image's ownership index only after seeing a possible start.
  std::optional<ImportThunkCandidates> Candidates;
  std::set<va_t> Existing;
  for (va_t Start : Starts) {
    const uint8_t *Bytes = Img.readVA(Start, aarch64::kInsnSize);
    if (!Bytes)
      continue;
    const uint32_t Word = readLE<uint32_t>(Bytes);
    if ((Word & aarch64::kADRP_X16_Mask) != aarch64::kADRP_X16_Match &&
        Word != aarch64::kBTI_C)
      continue;
    const Segment *Seg = Img.getSegmentFor(Start);
    if (!Seg || !Seg->isExecutable() || !Img.isFunctionEntryCandidateAt(Start))
      continue;
    if (!Candidates) {
      Candidates.emplace(Img);
      Existing = Img.getSymbolAddresses();
    }
    scanVeneers(Img, *Seg, Targets, Existing, *Candidates, Start - Seg->VA);
  }
}

} // namespace neverd
