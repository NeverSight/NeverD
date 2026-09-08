#ifndef NEVERD_LOADER_OBJC_OBJCRUNTIMEDATA_H
#define NEVERD_LOADER_OBJC_OBJCRUNTIMEDATA_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"

#include <optional>
#include <string>

namespace neverd::objc {

/// A bounded view of file-backed runtime records after Mach-O fixups.
class RuntimeData {
  const BinaryImage &Image;

public:
  explicit RuntimeData(const BinaryImage &Image) : Image(Image) {}

  const uint8_t *bytes(va_t VA, uint64_t Size) const {
    const auto *Section = Image.getSectionFor(VA);
    const auto *Segment = Image.getSegmentFor(VA);
    if (!Section || !Segment || !Section->isReadable() ||
        !Segment->isReadable() || Size > InvalidVA - VA ||
        Section->Size > InvalidVA - Section->VA ||
        Segment->Size > InvalidVA - Segment->VA || Section->VA < Segment->VA ||
        !rangeInBounds(VA - Section->VA, Size, Section->Size) ||
        !rangeInBounds(VA - Segment->VA, Size, Segment->Size) ||
        !rangeInBounds(VA - Section->VA, Size, Section->FileSz) ||
        !rangeInBounds(VA - Segment->VA, Size, Segment->FileSz) ||
        Section->FileOff < Segment->FileOff ||
        Section->FileOff - Segment->FileOff != Section->VA - Segment->VA)
      return nullptr;
    const auto Type = Section->Type & llvm::MachO::SECTION_TYPE;
    if (Type == llvm::MachO::S_ZEROFILL || Type == llvm::MachO::S_GB_ZEROFILL ||
        Type == llvm::MachO::S_THREAD_LOCAL_ZEROFILL)
      return nullptr;
    return Image.readVA(VA, Size);
  }

  std::optional<uint32_t> u32(va_t VA) const {
    const auto *Data = bytes(VA, 4);
    return Data ? std::optional(llvm::support::endian::read32le(Data))
                : std::nullopt;
  }

  std::optional<va_t> pointer(va_t VA) const {
    const auto *Data = bytes(VA, 8);
    if (!Data)
      return std::nullopt;
    const auto Value = llvm::support::endian::read64le(Data);
    if (Value && Image.MachOHasChainedFixups &&
        !Image.MachOResolvedChainedPointerSlots.count(VA))
      return std::nullopt;
    return Value;
  }

  std::optional<std::string> string(va_t VA) const {
    std::string Value;
    for (size_t Index = 0; Index < 4096 && VA <= InvalidVA - Index; ++Index) {
      const auto *Data = bytes(VA + Index, 1);
      if (!Data)
        return std::nullopt;
      if (!*Data)
        return Value.empty() ? std::nullopt : std::optional(Value);
      if (*Data < 0x20 || *Data > 0x7e)
        return std::nullopt;
      Value.push_back(static_cast<char>(*Data));
    }
    return std::nullopt;
  }

  std::optional<va_t> classRO(va_t Address) const {
    if (!bytes(Address, 40))
      return std::nullopt;
    auto Bits = pointer(Address + 32);
    if (!Bits || !*Bits)
      return std::nullopt;
    auto RO = *Bits & ~uint64_t(7);
    return bytes(RO, 72) ? std::optional(RO) : std::nullopt;
  }

  std::optional<std::string> className(va_t Address) const {
    auto RO = classRO(Address);
    auto Name = RO ? pointer(*RO + 24) : std::nullopt;
    return Name ? string(*Name) : std::nullopt;
  }
};

} // namespace neverd::objc

#endif
