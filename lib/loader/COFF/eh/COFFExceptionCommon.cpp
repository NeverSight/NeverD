//===- COFFExceptionCommon.cpp - Shared PE exception helpers --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::coff_loader::detail {

void diagnose(ExceptionFunction &F, ExceptionParseStatus Status,
              llvm::StringRef Message) {
  F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus, Status);
  F.Diagnostics.push_back(Message.str());
}

bool addRVA(va_t ImageBase, uint32_t RVA, va_t &Address) {
  if (RVA > InvalidVA - ImageBase)
    return false;
  Address = ImageBase + RVA;
  return true;
}

bool readBytes(const BinaryImage &Img, va_t Address, size_t Size,
               const uint8_t *&Bytes) {
  Bytes = Img.readVA(Address, Size);
  return Bytes != nullptr;
}

bool isExecutableAddress(const BinaryImage &Img, va_t Address) {
  const Segment *Seg = Img.getSegmentFor(Address);
  return Seg && Seg->isExecutable() && Img.readVA(Address, 1);
}

std::optional<ExceptionAddressRange>
checkedImageRange(va_t ImageBase, uint32_t BeginRVA, uint32_t EndRVA) {
  if (EndRVA <= BeginRVA)
    return std::nullopt;
  va_t Begin = 0;
  va_t End = 0;
  if (!addRVA(ImageBase, BeginRVA, Begin) || !addRVA(ImageBase, EndRVA, End))
    return std::nullopt;
  return ExceptionAddressRange{Begin, End};
}

/// ARM32 language tables spell every code pointer with the Thumb interworking
/// bit set, the form an interworking branch needs.  The Windows unwinder masks
/// that bit before comparing an address against a function range, so a decoder
/// that keeps it reports guarded ranges that overrun their function by one
/// byte and handler addresses that name no instruction.  Data pointers in the
/// same tables — type descriptors, map bases, ESTypeList — are never tagged
/// and must not be masked.
va_t normalizeTableCodeAddress(const BinaryImage &Img, va_t Address) {
  return Img.Arch == Arch::ARM ? clearThumbBit(Address) : Address;
}

bool addCodeRVA(const BinaryImage &Img, uint32_t RVA, va_t &Address) {
  if (!addRVA(Img.Base, RVA, Address))
    return false;
  Address = normalizeTableCodeAddress(Img, Address);
  return true;
}

bool readCodeRVAField(const BinaryImage &Img, const uint8_t *P, va_t &Out) {
  uint32_t RVA = readLE<uint32_t>(P);
  if (RVA == 0) {
    Out = 0;
    return true;
  }
  return addCodeRVA(Img, RVA, Out);
}

std::optional<ExceptionAddressRange>
checkedCodeRange(const BinaryImage &Img, uint32_t BeginRVA, uint32_t EndRVA) {
  auto Range = checkedImageRange(Img.Base, BeginRVA, EndRVA);
  if (!Range)
    return std::nullopt;
  Range->Begin = normalizeTableCodeAddress(Img, Range->Begin);
  Range->End = normalizeTableCodeAddress(Img, Range->End);
  if (!Range->isValid())
    return std::nullopt;
  return Range;
}

bool readRVAField(va_t ImageBase, const uint8_t *P, va_t &Out) {
  uint32_t RVA = readLE<uint32_t>(P);
  if (RVA == 0) {
    Out = 0;
    return true;
  }
  return addRVA(ImageBase, RVA, Out);
}

} // namespace neverd::coff_loader::detail
