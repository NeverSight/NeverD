//===- X64Imports.cpp - Witnessed x64 import instruction forms ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64Imports.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::unpack::x64 {
namespace {
std::optional<unsigned> helperCall(llvm::ArrayRef<uint8_t> Memory,
                                   uint64_t RVA) {
  if (RVA >= Memory.size())
    return std::nullopt;
  auto Bytes = Memory.drop_front(RVA);
  unsigned Offset = 0;
  if (Bytes[0] >= 0x50 && Bytes[0] <= 0x5f)
    Offset = 1;
  else if (Bytes.size() >= 2 && Bytes[0] == 0x41 && Bytes[1] >= 0x50 &&
           Bytes[1] <= 0x5f)
    Offset = 2;
  if (Bytes.size() < Offset + 5 || Bytes[Offset] != 0xe8)
    return std::nullopt;
  const int64_t Target =
      int64_t(RVA + Offset + 5) +
      int32_t(llvm::support::endian::read32le(Bytes.data() + Offset + 1));
  if (Target < 0 || uint64_t(Target) >= Memory.size())
    return std::nullopt;
  return Offset;
}
} // namespace
std::optional<unsigned> importHelper(llvm::ArrayRef<uint8_t> Memory,
                                     uint64_t InstructionRVA) {
  return helperCall(Memory, InstructionRVA);
}
std::optional<ImportSite> importSite(llvm::ArrayRef<uint8_t> Memory,
                                     uint64_t ReturnRVA, bool AddressLoad,
                                     uint64_t InstructionRVA,
                                     std::optional<unsigned> ResultRegister) {
  if (ReturnRVA > Memory.size() || InstructionRVA > ReturnRVA)
    return std::nullopt;
  const uint64_t Size = ReturnRVA - InstructionRVA;
  if (Size < (AddressLoad ? MinAddressLoadSize : 6) ||
      Size > MaxAddressLoadSize ||
      (AddressLoad
           ? (!ResultRegister || *ResultRegister >= 16 || *ResultRegister == 4)
           : bool(ResultRegister)))
    return std::nullopt;
  auto Call = helperCall(Memory, InstructionRVA);
  if (!Call || *Call + 5 > Size)
    return std::nullopt;
  // The witness establishes the actual start, continuation and complete
  // state. Bytes following CALL are never interpreted as a signature.
  return ImportSite{InstructionRVA, Size,
                    AddressLoad ? ResultRegister : std::nullopt};
}

void writeImport(llvm::MutableArrayRef<uint8_t> Memory, const ImportSite &Site,
                 uint64_t SlotRVA) {
  uint8_t *Bytes = Memory.data() + Site.RVA;
  unsigned Offset;
  if (Site.Register) {
    Bytes[0] = *Site.Register < 8 ? 0x48 : 0x4c;
    Bytes[1] = 0x8b;
    Bytes[2] = 0x05 | ((*Site.Register & 7) << 3);
    Offset = 3;
  } else {
    const unsigned Padding = Site.Size - 6;
    std::fill_n(Bytes, Padding, uint8_t(0x90));
    Bytes += Padding;
    Bytes[0] = 0xff;
    Bytes[1] = 0x15;
    Offset = 2;
  }
  llvm::support::endian::write32le(
      Bytes + Offset,
      uint32_t(int32_t(int64_t(SlotRVA) - int64_t(Site.instructionEnd()))));
  if (Site.Register)
    std::fill(Bytes + Offset + 4, Memory.data() + Site.RVA + Site.Size,
              uint8_t(0x90));
}
} // namespace neverd::unpack::x64
