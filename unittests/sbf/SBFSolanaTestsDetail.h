//===- SBFSolanaTestsDetail.h - Shared Solana recovery fixtures ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// In-memory strict v3 images the Solana recovery tests analyze: an encoder
/// for single instructions and a read-only region assembled a piece at a time.
///
/// This is deliberately separate from SBFFixtureBuilder.h, which serializes
/// real ELF bytes for the loader tests; nothing here goes near an ELF header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SBF_SBFSOLANATESTSDETAIL_H
#define NEVERD_UNITTESTS_SBF_SBFSOLANATESTSDETAIL_H

#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/solana/SBFPubkey.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace neverd::sbf::test {

using EncodedInstruction = std::array<uint8_t, kInstructionSize>;

inline EncodedInstruction encode(Opcode ID, uint8_t Dst = 0, uint8_t Src = 0,
                                 int16_t Offset = 0, int32_t Immediate = 0) {
  EncodedInstruction Bytes{};
  const OpcodeInfo *Info = getOpcodeInfo(ID);
  EXPECT_NE(Info, nullptr);
  if (!Info)
    return Bytes;
  Bytes[kOpcodeOffset] = Info->Encoding;
  Bytes[kRegisterByteOffset] =
      static_cast<uint8_t>((Src << kRegisterEncodingBits) | Dst);
  llvm::support::endian::write16le(Bytes.data() + kBranchOffsetOffset,
                                   static_cast<uint16_t>(Offset));
  llvm::support::endian::write32le(Bytes.data() + kImmediateOffset,
                                   static_cast<uint32_t>(Immediate));
  return Bytes;
}

/// Emit a 64-bit constant load as its two encoded slots.
inline std::vector<EncodedInstruction> loadImm64(uint8_t Dst, uint64_t Value) {
  EncodedInstruction High{};
  llvm::support::endian::write32le(High.data() + kImmediateOffset,
                                   static_cast<uint32_t>(Value >> 32));
  return {encode(Opcode::LDDW, Dst, 0, 0, static_cast<int32_t>(Value)), High};
}

/// A strict v3 image whose read-only region holds \p Rodata.
inline BinaryImage makeImage(llvm::ArrayRef<EncodedInstruction> Instructions,
                             llvm::ArrayRef<uint8_t> Rodata) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart;
  for (const EncodedInstruction &Instruction : Instructions)
    Image.Raw.insert(Image.Raw.end(), Instruction.begin(), Instruction.end());

  Section Text;
  Text.Name = kTextSectionName.str();
  Text.VA = kBytecodeStart;
  Text.Size = Image.Raw.size();
  Text.FileSz = Image.Raw.size();
  Text.Flags = SegmentFlags::Executable;
  Text.Alignment = kInstructionSize;
  Text.Data = Image.Raw;
  Image.Sections.push_back(std::move(Text));

  Section ReadOnly;
  ReadOnly.Name = kRodataSectionName.str();
  ReadOnly.VA = kRodataStartV3;
  ReadOnly.Size = Rodata.size();
  ReadOnly.FileSz = Rodata.size();
  ReadOnly.Alignment = kInstructionSize;
  ReadOnly.Data.assign(Rodata.begin(), Rodata.end());
  Image.Sections.push_back(std::move(ReadOnly));

  Metadata Meta;
  Meta.Machine = kELFMachineBPF;
  Meta.ELFFlags = static_cast<uint32_t>(Version::V3);
  Meta.Version = Version::V3;
  Meta.StrictLayout = true;
  Meta.TextFile = {0, Image.Raw.size()};
  Meta.TextVM = {kBytecodeStart, Image.Raw.size()};
  Meta.RodataVM = {kRodataStartV3, Rodata.size()};
  Image.SBF = Meta;
  return Image;
}

/// Read-only data assembled a piece at a time, tracking where each piece went.
class RodataBuilder {
public:
  va_t append(llvm::ArrayRef<uint8_t> Piece) {
    const va_t Address = kRodataStartV3 + Bytes.size();
    Bytes.insert(Bytes.end(), Piece.begin(), Piece.end());
    return Address;
  }

  va_t appendKey(const Pubkey &Key) { return append(Key.Bytes); }

  va_t appendText(llvm::StringRef Text) {
    return append(
        {reinterpret_cast<const uint8_t *>(Text.data()), Text.size()});
  }

  /// Reserve \p Size zero bytes and return where they start, so a structure
  /// can be filled in once the addresses it refers to are known.
  va_t reserve(size_t Size) {
    const va_t Address = kRodataStartV3 + Bytes.size();
    Bytes.resize(Bytes.size() + Size);
    return Address;
  }

  void putWord(va_t Address, uint64_t Value) {
    llvm::support::endian::write64le(&Bytes[Address - kRodataStartV3], Value);
  }

  void putKey(va_t Address, const Pubkey &Key) {
    std::copy(Key.Bytes.begin(), Key.Bytes.end(),
              Bytes.begin() + (Address - kRodataStartV3));
  }

  llvm::ArrayRef<uint8_t> bytes() const { return Bytes; }

private:
  std::vector<uint8_t> Bytes;
};

inline Pubkey filledKey(uint8_t Byte) {
  Pubkey Key;
  Key.Bytes.fill(Byte);
  return Key;
}

} // namespace neverd::sbf::test

#endif // NEVERD_UNITTESTS_SBF_SBFSOLANATESTSDETAIL_H
