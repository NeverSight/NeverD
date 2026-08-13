//===- COFFRegistrationEHCommon.cpp - Shared x86-32 EH helpers -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::coff_loader::registration_detail {

void diagnose(ExceptionFunction &F, ExceptionParseStatus Status,
              const std::string &Message) {
  F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus, Status);
  F.Diagnostics.push_back(Message);
}

bool isExecutableAddress(const BinaryImage &Img, va_t Address) {
  const Segment *Seg = Img.getSegmentFor(Address);
  return Seg && Seg->isExecutable() && Img.readVA(Address, 1) != nullptr;
}

std::optional<va_t> addSignedOffset(va_t Base, int64_t Displacement) {
  if (Displacement >= 0) {
    if (static_cast<uint64_t>(Displacement) > InvalidVA - Base)
      return std::nullopt;
    return Base + static_cast<uint64_t>(Displacement);
  }
  uint64_t Magnitude = static_cast<uint64_t>(-Displacement);
  if (Magnitude > Base)
    return std::nullopt;
  return Base - Magnitude;
}

} // namespace neverd::coff_loader::registration_detail
