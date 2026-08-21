//===- JumpTableResolverTargets.cpp - Table entry read and validation -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Turning a recovered table description into a validated target list: the
/// per-target sanity checks (executable segment, distance, alignment, known
/// function entries), the multi-format entry decoder (1/2/4/8 byte, signed or
/// unsigned, absolute / base-relative / scaled compact), the bounded and
/// unbounded entry reads, and post-read truncation.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// isValidTarget — sanity check a resolved target address
//===----------------------------------------------------------------------===//

uint32_t CFGBuilder::getInsnAlignment() const {
  if (!CurrentImg)
    return 1;
  uint32_t Align = getTargetRegInfo(CurrentImg->Arch).MinInsnAlign;
  return Align ? Align : 1;
}

bool CFGBuilder::isValidTarget(const BinaryImage &Img, va_t Target,
                               va_t FuncEntry) {
  const auto *Seg = Img.getSegmentFor(Target);
  if (!Seg || !Img.hasExecutableCodeOwnerAt(Target))
    return false;

  uint64_t Dist = Target > FuncEntry ? Target - FuncEntry : FuncEntry - Target;
  if (Dist > limits::kMaxJumpTargetDistance)
    return false;

  uint32_t Align = getInsnAlignment();
  if (Align > 1 && (Target % Align) != 0)
    return false;
  const size_t Off = static_cast<size_t>(Target - Seg->VA);
  if (!rangeInBounds(Off, Align, Seg->Data.size()) ||
      !Img.hasExecutableCodeOwnerRange(Target, Align))
    return false;

  if (KnownFuncEntries && KnownFuncEntries->count(Target) &&
      Target != FuncEntry)
    return false;

  return true;
}

//===----------------------------------------------------------------------===//
// sanityCheckTargets — post-read validation with truncation
//===----------------------------------------------------------------------===//

bool CFGBuilder::sanityCheckTargets(const BinaryImage &Img,
                                    std::vector<va_t> &Targets) const {
  if (Targets.empty())
    return false;

  if (Targets.size() <= limits::kMinJumpTableEntries)
    return Targets.size() >= limits::kMinJumpTableEntries;

  uint32_t Align = getInsnAlignment();

  va_t RefAddr = Targets[0];
  size_t TruncAt = Targets.size();
  int InvalidCount = 0;

  for (size_t I = 1; I < Targets.size(); ++I) {
    uint64_t Dist =
        Targets[I] > RefAddr ? Targets[I] - RefAddr : RefAddr - Targets[I];
    if (Dist > limits::kMaxJumpTargetDistance) {
      TruncAt = I;
      break;
    }

    const auto *TSeg = Img.getSegmentFor(Targets[I]);
    if (!TSeg || !Img.hasExecutableCodeOwnerAt(Targets[I])) {
      TruncAt = I;
      break;
    }

    // A target only needs room for one minimum-size instruction.  Using a
    // fixed 4-byte slack wrongly rejects short trailing blocks (e.g. an x86
    // `ret` is 1 byte), truncating an otherwise valid table at that entry.
    size_t TOff = static_cast<size_t>(Targets[I] - TSeg->VA);
    if (!rangeInBounds(TOff, getInsnAlignment(), TSeg->Data.size()) ||
        !Img.hasExecutableCodeOwnerRange(Targets[I], getInsnAlignment())) {
      TruncAt = I;
      break;
    }

    if (Align > 1 && (Targets[I] % Align) != 0)
      ++InvalidCount;

    if (KnownFuncEntries && KnownFuncEntries->count(Targets[I]) &&
        Targets[I] != CurrentFuncEntry)
      ++InvalidCount;
  }

  if (TruncAt < Targets.size()) {
    LLVM_DEBUG(llvm::dbgs()
               << "  sanity: truncating table from " << Targets.size() << " to "
               << TruncAt << " entries\n");
    Targets.resize(TruncAt);
  }

  if (Targets.size() > limits::kMinJumpTableEntries && InvalidCount > 0) {
    int ValidPercent = static_cast<int>((Targets.size() - InvalidCount) * 100 /
                                        Targets.size());
    if (ValidPercent < limits::kMinValidTargetPercent) {
      LLVM_DEBUG(llvm::dbgs() << "  sanity: only " << ValidPercent
                              << "% valid targets, rejecting table\n");
      Targets.clear();
      return false;
    }
  }

  return Targets.size() >= limits::kMinJumpTableEntries;
}

//===----------------------------------------------------------------------===//
// readTableEntries — read entries from memory with format awareness
//===----------------------------------------------------------------------===//

/// Decode a single table entry into a target address.  For the compact-table
/// form (TargetBase != 0) the target is `TargetBase + entry * Scale`; otherwise
/// relative tables use `BaseAddr + entry` and absolute tables store the target.
va_t decodeTableEntry(const uint8_t *P, uint16_t EntrySize, bool IsRelative,
                      bool IsSigned, va_t BaseAddr, va_t TargetBase,
                      uint32_t Scale) {
  if (TargetBase != 0) {
    int64_t Entry = 0;
    switch (EntrySize) {
    case 1:
      Entry = IsSigned ? static_cast<int8_t>(*P) : static_cast<uint8_t>(*P);
      break;
    case 2: {
      uint16_t Val;
      std::memcpy(&Val, P, 2);
      Entry = IsSigned ? static_cast<int16_t>(Val) : static_cast<int64_t>(Val);
      break;
    }
    case 4: {
      uint32_t Val;
      std::memcpy(&Val, P, 4);
      Entry = IsSigned ? static_cast<int32_t>(Val) : static_cast<int64_t>(Val);
      break;
    }
    default:
      break;
    }
    return static_cast<va_t>(static_cast<int64_t>(TargetBase) +
                             Entry * static_cast<int64_t>(Scale));
  }
  if (IsRelative) {
    int64_t Offset = 0;
    switch (EntrySize) {
    case 1:
      Offset = IsSigned ? static_cast<int8_t>(*P) : static_cast<uint8_t>(*P);
      break;
    case 2: {
      uint16_t Val;
      std::memcpy(&Val, P, 2);
      Offset = IsSigned ? static_cast<int16_t>(Val) : static_cast<int64_t>(Val);
      break;
    }
    case 4: {
      uint32_t Val;
      std::memcpy(&Val, P, 4);
      Offset = IsSigned ? static_cast<int32_t>(Val) : static_cast<int64_t>(Val);
      break;
    }
    default:
      break;
    }
    return static_cast<va_t>(static_cast<int64_t>(BaseAddr) + Offset);
  }

  va_t Target = 0;
  switch (EntrySize) {
  case 8:
    std::memcpy(&Target, P, 8);
    break;
  case 4: {
    uint32_t Val;
    std::memcpy(&Val, P, 4);
    Target = Val;
    break;
  }
  case 2: {
    uint16_t Val;
    std::memcpy(&Val, P, 2);
    Target = Val;
    break;
  }
  case 1:
    Target = *P;
    break;
  default:
    break;
  }
  return Target;
}

std::vector<va_t>
CFGBuilder::readTableEntries(const BinaryImage &Img, const JumpTableInfo &Info,
                             std::vector<uint32_t> *KeptIndices) {
  if (KeptIndices)
    KeptIndices->clear();
  const auto *Seg = Img.getSegmentFor(Info.BaseAddr);
  if (!Seg || Seg->Data.empty())
    return {};
  const std::optional<va_t> StorageEnd =
      Img.mappedObjectOwnerEnd(Info.BaseAddr);
  if (!StorageEnd || *StorageEnd <= Info.BaseAddr)
    return {};

  const bool Bounded = Info.MaxEntries > 0;
  uint32_t Limit = Info.MaxEntries;
  if (Limit == 0 || Limit > limits::kMaxJumpTableEntries)
    Limit = limits::kMaxJumpTableEntries;

  std::vector<va_t> Targets;
  Targets.reserve(std::min(Limit, 64u));
  size_t Off = static_cast<size_t>(Info.BaseAddr - Seg->VA);
  const uint64_t EntryStride =
      Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
  va_t PrevTarget = InvalidVA;
  int DuplicateRun = 0;
  int SkippedRun = 0;

  for (uint32_t I = 0; I < Limit; ++I) {
    if (I != 0 &&
        EntryStride > (std::numeric_limits<size_t>::max() - Off) / uint64_t(I))
      break;
    size_t EntryOff = Off + static_cast<size_t>(uint64_t(I) * EntryStride);
    if (!rangeInBounds(EntryOff, Info.EntrySize, Seg->Data.size()))
      break;
    if (EntryOff > InvalidVA - Seg->VA || Info.EntrySize == 0)
      break;
    const va_t EntryVA = Seg->VA + EntryOff;
    if (Info.EntrySize - 1 > InvalidVA - EntryVA)
      break;
    if (EntryVA >= *StorageEnd || Info.EntrySize > *StorageEnd - EntryVA)
      break;

    const uint8_t *P = Seg->Data.data() + EntryOff;
    va_t Target =
        decodeTableEntry(P, Info.EntrySize, Info.IsRelative, Info.IsSigned,
                         Info.BaseAddr, Info.TargetBase, Info.EntryScale);

    if (!isValidTarget(Img, Target, CurrentFuncEntry)) {
      if (Bounded) {
        ++SkippedRun;
        if (SkippedRun > limits::kMaxSkippedEntries)
          break;
        continue;
      }
      break;
    }
    SkippedRun = 0;

    if (Target == PrevTarget) {
      if (++DuplicateRun > limits::kMaxDuplicateRun && !Bounded)
        break;
    } else {
      DuplicateRun = 0;
      PrevTarget = Target;
    }

    Targets.push_back(Target);
    if (KeptIndices)
      KeptIndices->push_back(I);
  }

  return Targets;
}

} // namespace neverd
