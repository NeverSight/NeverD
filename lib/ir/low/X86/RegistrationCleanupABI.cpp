//===- RegistrationCleanupABI.cpp - PE32 unwind relay contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate EBP-relative cleanup relays and their returning leaf calls.
//===----------------------------------------------------------------------===//
#include "RegistrationABIPrivate.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {
std::optional<RegistrationCleanupRelayABI>
getCheckedX86RegistrationCleanupRelayABI(const BinaryImage &Image, va_t Target,
                                         size_t *CumulativeWork) {
  using namespace registration_abi;
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX ||
      !Image.isCodeAddress(Target) ||
      Image.ExceptionMetadata.findFunction(Target))
    return std::nullopt;
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  if (!chargeCalleeWork(Work, 1))
    return std::nullopt;
  const auto Prefix = getCleanupFramePrefix(Image, Target, Work);
  if (!Prefix)
    return std::nullopt;
  const bool CallsLeaf = Prefix->CallsLeaf;
  unsigned AddressOffset = Prefix->Size;
  RegistrationCleanupRelayABI Result;
  Result.Target = Target;
  Result.RealignedParent = Prefix->RealignedParent;
  for (;;) {
    const auto Header =
        readImmutableCodeBytes(Image, Target + AddressOffset, 3);
    const unsigned NarrowAddress = 0x48 | Prefix->BaseRegister;
    const unsigned WideAddress = 0x88 | Prefix->BaseRegister;
    if (!Header || (*Header)[0] != 0x8d ||
        ((*Header)[1] != NarrowAddress && (*Header)[1] != WideAddress))
      return std::nullopt;
    const unsigned AddressSize = (*Header)[1] == NarrowAddress ? 3 : 6;
    const unsigned Size = AddressSize + 5;
    const va_t CallEnd = Target + AddressOffset + Size;
    if (CallEnd > uint64_t(UINT32_MAX) + 1 ||
        !chargeCalleeWork(Work, Size + 1) ||
        Result.Calls.size() >= limits::kMaxRegistrationEHRecords)
      return std::nullopt;
    auto Bytes = readImmutableCodeBytes(Image, Target + AddressOffset, Size);
    if (!Bytes || (*Bytes)[AddressSize] != (CallsLeaf ? 0xe8 : 0xe9))
      return std::nullopt;
    const int32_t Displacement = AddressSize == 3
                                     ? int8_t((*Bytes)[2])
                                     : readLE<int32_t>(Bytes->data() + 2);
    const int64_t ObjectOffset = int64_t(Prefix->BaseOffset) + Displacement;
    if (ObjectOffset < INT32_MIN || ObjectOffset > INT32_MAX)
      return std::nullopt;
    const uint32_t LeafTarget =
        uint32_t(CallEnd) + readLE<uint32_t>(Bytes->data() + AddressSize + 1);
    auto Leaf =
        getCheckedX86RegistrationLeafCalleeABI(Image, LeafTarget, &Work);
    if (!Leaf || !Leaf->CallerPCWrites.empty())
      return std::nullopt;
    Result.Calls.push_back({int32_t(ObjectOffset), std::move(*Leaf)});
    Result.EndAddress = CallEnd;
    if (!CallsLeaf)
      return Result;
    // Only the ordered LEA/CALL pairs may intervene before the exact restore.
    // Each leaf preserves EBP, and no caller-PC observation survives changing
    // the relay into generated cleanup calls.
    auto Tail = readImmutableCodeBytes(Image, CallEnd, 2);
    if (!Tail)
      return std::nullopt;
    if ((*Tail)[0] == 0x5d && (*Tail)[1] == 0xc3) {
      if (CallEnd > uint64_t(UINT32_MAX) - 1 || !chargeCalleeWork(Work, 2))
        return std::nullopt;
      Result.EndAddress += 2;
      return Result;
    }
    AddressOffset += Size;
  }
}
} // namespace neverd
