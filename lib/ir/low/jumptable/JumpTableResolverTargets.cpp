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
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

bool isExplicitlyOwnedFunctionFragment(const BinaryImage &Img,
                                       va_t FunctionEntry, va_t Target) {
  const auto &Functions = Img.ExceptionMetadata.Functions;
  std::set<size_t> Primaries;
  for (size_t I = 0; I < Functions.size(); ++I) {
    const ExceptionFunction &Function = Functions[I];
    if (Function.Kind == RuntimeFunctionKind::Primary &&
        Function.CodeRange.Begin == FunctionEntry)
      Primaries.insert(I);
  }
  if (Primaries.empty())
    return false;

  for (const ExceptionFunction &Fragment : Functions) {
    if (Fragment.Kind == RuntimeFunctionKind::Primary ||
        !Fragment.CodeRange.contains(Target))
      continue;
    if (Fragment.PrimaryFunctionIndex &&
        Primaries.count(*Fragment.PrimaryFunctionIndex))
      return true;
    if (Fragment.ChainedPrimaryRange &&
        Fragment.ChainedPrimaryRange->Begin == FunctionEntry)
      return true;
  }
  return false;
}

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
                               va_t FuncEntry) const {
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

  // Once the detector has a symbol/unwind/next-entry boundary, distance is no
  // longer an ownership proof.  In particular, a relocation can name the
  // middle of the next known function and evade the entry-only check above.
  // Permit an out-of-envelope target only when runtime unwind metadata links
  // its chained/cold fragment explicitly back to this exact primary function.
  // With no usable boundary, retain the conservative distance policy above.
  if (CurrentFuncRange &&
      (Target < CurrentFuncRange->first ||
       Target >= CurrentFuncRange->second) &&
      !isExplicitlyOwnedFunctionFragment(Img, FuncEntry, Target))
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

/// Decode a single table entry into a target address.  Address arithmetic is
/// deliberately unsigned and masked to the guest pointer width: signed C++
/// overflow is undefined, while every supported guest ISA wraps addresses.
std::optional<va_t> decodeTableEntry(const uint8_t *P, uint16_t EntrySize,
                                     bool IsRelative, bool IsSigned,
                                     va_t BaseAddr, bool HasTargetBase,
                                     va_t TargetBase, uint32_t Scale,
                                     uint16_t AddressBytes) {
  if (!P ||
      (EntrySize != 1 && EntrySize != 2 && EntrySize != 4 && EntrySize != 8))
    return std::nullopt;
  const unsigned AddressBits =
      AddressBytes > 0 && AddressBytes < sizeof(uint64_t)
          ? static_cast<unsigned>(AddressBytes) * 8u
          : 64u;
  const uint64_t AddressMask = AddressBits == 64
                                   ? std::numeric_limits<uint64_t>::max()
                                   : (uint64_t(1) << AddressBits) - 1;

  auto readEntryBits = [&]() -> uint64_t {
    uint64_t Value = 0;
    switch (EntrySize) {
    case 1:
      Value = *P;
      break;
    case 2: {
      uint16_t Raw = 0;
      std::memcpy(&Raw, P, sizeof(Raw));
      Value = Raw;
      break;
    }
    case 4: {
      uint32_t Raw = 0;
      std::memcpy(&Raw, P, sizeof(Raw));
      Value = Raw;
      break;
    }
    case 8:
      std::memcpy(&Value, P, sizeof(Value));
      break;
    default:
      llvm_unreachable("entry width validated above");
    }
    const unsigned EntryBits = static_cast<unsigned>(EntrySize) * 8u;
    if (IsSigned && EntryBits < 64 &&
        (Value & (uint64_t(1) << (EntryBits - 1))) != 0)
      Value |= ~((uint64_t(1) << EntryBits) - 1);
    return Value;
  };

  const uint64_t Entry = readEntryBits();
  if (HasTargetBase)
    return static_cast<va_t>(
        ((TargetBase & AddressMask) + (Entry * uint64_t(Scale))) & AddressMask);
  if (IsRelative)
    return static_cast<va_t>(((BaseAddr & AddressMask) + Entry) & AddressMask);

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
  return Target & AddressMask;
}

std::optional<va_t> canonicalizeAbsoluteTableCodeTarget(const BinaryImage &Img,
                                                        va_t RawTarget) {
  if (Img.Arch != Arch::ARM)
    return RawTarget;

  const bool SerializedThumb = (RawTarget & 1) != 0;
  switch (Img.Mode) {
  case InstructionMode::Thumb:
    if (!SerializedThumb)
      return std::nullopt;
    return normalizeCodeAddress(RawTarget, Img.Arch, Img.Mode);
  case InstructionMode::Default:
  case InstructionMode::ARM:
    if (SerializedThumb)
      return std::nullopt;
    return RawTarget;
  default:
    return std::nullopt;
  }
}

std::vector<va_t>
CFGBuilder::readTableEntries(const BinaryImage &Img, const JumpTableInfo &Info,
                             std::vector<uint32_t> *KeptIndices,
                             JumpTableTargetReadPolicy Policy) const {
  if (KeptIndices)
    KeptIndices->clear();
  if (!Info.HasBaseAddr || (Info.EntrySize != 1 && Info.EntrySize != 2 &&
                            Info.EntrySize != 4 && Info.EntrySize != 8))
    return {};
  const uint64_t EntryStride =
      Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
  if (EntryStride < Info.EntrySize)
    return {};
  if (Info.HasTargetBase && Info.EntryScale != 1 && Info.EntryScale != 2 &&
      Info.EntryScale != 4 && Info.EntryScale != 8)
    return {};
  const auto *Seg = Img.getSegmentFor(Info.BaseAddr);
  if (!Seg || Seg->Data.empty())
    return {};
  const std::optional<va_t> StorageEnd =
      Img.mappedObjectOwnerEnd(Info.BaseAddr);
  if (!StorageEnd || *StorageEnd <= Info.BaseAddr)
    return {};

  const bool ExplicitDomain = !Info.RuntimeSlotIndices.empty();
  if (ExplicitDomain &&
      Info.RuntimeSlotIndices.size() != Info.RuntimeCaseLabels.size())
    return {};
  const bool Bounded = Info.MaxEntries > 0 || ExplicitDomain;
  uint32_t Limit = ExplicitDomain
                       ? static_cast<uint32_t>(Info.RuntimeSlotIndices.size())
                       : Info.MaxEntries;
  if (Limit == 0 || Limit > limits::kMaxJumpTableEntries)
    Limit = limits::kMaxJumpTableEntries;

  std::vector<va_t> Targets;
  Targets.reserve(std::min(Limit, 64u));
  size_t Off = static_cast<size_t>(Info.BaseAddr - Seg->VA);
  va_t PrevTarget = InvalidVA;
  int DuplicateRun = 0;

  for (uint32_t Position = 0; Position < Limit; ++Position) {
    const uint32_t I =
        ExplicitDomain ? Info.RuntimeSlotIndices[Position] : Position;
    auto FailOrStop = [&]() { return Bounded; };
    if (I != 0 && EntryStride > (std::numeric_limits<size_t>::max() - Off) /
                                    uint64_t(I)) {
      if (FailOrStop())
        return {};
      break;
    }
    size_t EntryOff = Off + static_cast<size_t>(uint64_t(I) * EntryStride);
    if (!rangeInBounds(EntryOff, Info.EntrySize, Seg->Data.size()) ||
        EntryOff > InvalidVA - Seg->VA || Info.EntrySize == 0) {
      if (FailOrStop())
        return {};
      break;
    }
    const va_t EntryVA = Seg->VA + EntryOff;
    if (Info.EntrySize - 1 > InvalidVA - EntryVA || EntryVA >= *StorageEnd ||
        Info.EntrySize > *StorageEnd - EntryVA) {
      if (FailOrStop())
        return {};
      break;
    }

    const uint8_t *P = Seg->Data.data() + EntryOff;
    auto TargetOpt =
        decodeTableEntry(P, Info.EntrySize, Info.IsRelative, Info.IsSigned,
                         Info.BaseAddr, Info.HasTargetBase, Info.TargetBase,
                         Info.EntryScale, Img.getPointerSize());
    if (!TargetOpt)
      return {};
    va_t Target = *TargetOpt;
    if (!Info.IsRelative && !Info.HasTargetBase) {
      std::optional<va_t> Canonical =
          canonicalizeAbsoluteTableCodeTarget(Img, Target);
      if (!Canonical)
        return {};
      Target = *Canonical;
    }

    if (Policy == JumpTableTargetReadPolicy::SwitchPublication &&
        !isValidTarget(Img, Target, CurrentFuncEntry)) {
      if (Bounded)
        return {};
      break;
    }
    if (Policy == JumpTableTargetReadPolicy::SwitchPublication &&
        Target == PrevTarget) {
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

  if (Bounded && Targets.size() != Limit)
    return {};

  return Targets;
}

} // namespace neverd
