//===- COFFStackProbe.cpp - Checked PE stack probes ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFStackProbe.h"

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/ReadOnlyBytes.h"

#include <algorithm>

namespace neverd::coff_loader {

std::optional<X86AllocatingStackProbeCall>
getCheckedX86AllocatingStackProbeCall(const BinaryImage &Image, va_t CallVA) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || CallVA < 5 ||
      CallVA > UINT32_MAX - 4)
    return std::nullopt;
  const auto Caller = readImmutablePE32CodeBytes(Image, CallVA - 5, 10, {});
  if (!Caller || (*Caller)[0] != 0xb8 || (*Caller)[5] != 0xe8)
    return std::nullopt;
  const uint32_t Allocation = readLE<uint32_t>(Caller->data() + 1);
  if (Allocation > limits::kMaxSourceCallBorrowedBytes)
    return std::nullopt;
  const auto Target = registration_detail::addSignedOffset(
      CallVA + 5, readLE<int32_t>(Caller->data() + 6));
  // Save ECX, clamp the target on subtraction underflow, touch descending
  // pages, restore ECX, exchange ESP and move the original return PC. The
  // conditional edge and backedge are part of this exact physical protocol.
  static constexpr uint8_t Bytes[] = {
      0x51, 0x8d, 0x4c, 0x24, 0x04, 0x2b, 0xc8, 0x1b, 0xc0, 0xf7, 0xd0,
      0x23, 0xc8, 0x8b, 0xc4, 0x25, 0x00, 0xf0, 0xff, 0xff, 0x3b, 0xc8,
      0x72, 0x0a, 0x8b, 0xc1, 0x59, 0x94, 0x8b, 0x00, 0x89, 0x04, 0x24,
      0xc3, 0x2d, 0x00, 0x10, 0x00, 0x00, 0x85, 0x00, 0xeb, 0xe9};
  if (!Target || *Target > UINT32_MAX - (sizeof(Bytes) - 1))
    return std::nullopt;
  const ExceptionAddressRange Helper{*Target, *Target + sizeof(Bytes)};
  const ExceptionAddressRange Prefix{CallVA - 5, CallVA + 5};
  const auto Code =
      readImmutablePE32CodeBytes(Image, *Target, sizeof(Bytes), {});
  if (Helper.overlaps(Prefix) || !Code ||
      !std::equal(std::begin(Bytes), std::end(Bytes), Code->begin()))
    return std::nullopt;
  return X86AllocatingStackProbeCall{Prefix, Helper, CallVA, Allocation};
}

std::optional<X64StackProbeCall>
getCheckedX64StackProbeCall(const BinaryImage &Image, va_t CallVA) {
  if (Image.Arch != Arch::X64 || Image.Bits != Bitness::Bits64 ||
      Image.Format != BinaryFormat::COFF || CallVA > InvalidVA - 5)
    return std::nullopt;
  const auto Caller = readImmutableCodeBytes(Image, CallVA, 5);
  if (!Caller || (*Caller)[0] != 0xe8)
    return std::nullopt;
  const auto Target = registration_detail::addSignedOffset(
      CallVA + 5, readLE<int32_t>(Caller->data() + 1));
  // Save RCX/RAX, walk down by 4096 while unsigned RAX exceeds one page,
  // touch the final partial page, and restore both registers. Authenticate
  // both branches, both memory touches and the only return, not a prefix or
  // a symbol spelling. In particular a signed comparison gives no bound.
  static constexpr uint8_t Bytes[] = {
      0x51, 0x50, 0x48, 0x3d, 0x00, 0x10, 0x00, 0x00, 0x48, 0x8d,
      0x4c, 0x24, 0x18, 0x72, 0x19, 0x48, 0x81, 0xe9, 0x00, 0x10,
      0x00, 0x00, 0x48, 0x83, 0x09, 0x00, 0x48, 0x2d, 0x00, 0x10,
      0x00, 0x00, 0x48, 0x3d, 0x00, 0x10, 0x00, 0x00, 0x77, 0xe7,
      0x48, 0x29, 0xc1, 0x48, 0x83, 0x09, 0x00, 0x58, 0x59, 0xc3};
  if (!Target || *Target > InvalidVA - sizeof(Bytes))
    return std::nullopt;
  const auto Code = readImmutableCodeBytes(Image, *Target, sizeof(Bytes));
  const ExceptionAddressRange Helper{*Target, *Target + sizeof(Bytes)};
  if (!Code || Helper.overlaps({CallVA, CallVA + 5}) ||
      !std::equal(std::begin(Bytes), std::end(Bytes), Code->begin()))
    return std::nullopt;
  return X64StackProbeCall{Helper};
}

} // namespace neverd::coff_loader
