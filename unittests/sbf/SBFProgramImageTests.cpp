//===- SBFProgramImageTests.cpp - Canonical SBF VM-image tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sbf/runtime/SBFOpcodes.h"
#include "neverd/sbf/image/SBFProgramImage.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace neverd::sbf {
namespace {

std::array<uint8_t, kLDDWSlotCount * kInstructionSize>
makeLDDW(uint64_t Immediate = 0) {
  std::array<uint8_t, kLDDWSlotCount * kInstructionSize> Bytes{};
  const OpcodeInfo *Info = getOpcodeInfo(Opcode::LDDW);
  EXPECT_NE(Info, nullptr);
  if (!Info)
    return Bytes;
  Bytes[kOpcodeOffset] = Info->Encoding;
  llvm::support::endian::write32le(Bytes.data() + kImmediateOffset,
                                   static_cast<uint32_t>(Immediate));
  llvm::support::endian::write32le(Bytes.data() + kInstructionSize +
                                       kImmediateOffset,
                                   static_cast<uint32_t>(Immediate >> 32));
  return Bytes;
}

uint64_t readLDDWImmediate(llvm::ArrayRef<uint8_t> Bytes) {
  EXPECT_GE(Bytes.size(), kLDDWSlotCount * kInstructionSize);
  if (Bytes.size() < kLDDWSlotCount * kInstructionSize)
    return 0;
  const uint64_t Low =
      llvm::support::endian::read32le(Bytes.data() + kImmediateOffset);
  const uint64_t High = llvm::support::endian::read32le(
      Bytes.data() + kInstructionSize + kImmediateOffset);
  return Low | (High << 32);
}

Section makeSection(llvm::StringRef Name, va_t Address,
                    llvm::ArrayRef<uint8_t> Bytes, bool Executable = false) {
  Section Result;
  Result.Name = Name.str();
  Result.VA = Address;
  Result.Size = Bytes.size();
  Result.FileSz = Bytes.size();
  Result.Alignment = kInstructionSize;
  Result.Flags = SegmentFlags::Readable;
  if (Executable)
    Result.Flags = Result.Flags | SegmentFlags::Executable;
  Result.Data.assign(Bytes.begin(), Bytes.end());
  return Result;
}

BinaryImage makeLegacyImage(llvm::ArrayRef<uint8_t> Text,
                            va_t TextAddress = kBytecodeStart + 0x20) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Entry = TextAddress;
  Image.Sections.push_back(
      makeSection(kTextSectionName, TextAddress, Text, true));
  return Image;
}

Metadata makeLegacyMetadata(size_t TextSize,
                            va_t TextAddress = kBytecodeStart + 0x20) {
  Metadata Result;
  Result.Version = Version::V0;
  Result.TextVM = {TextAddress, TextSize};
  return Result;
}

TEST(SBFProgramImageTest, SyntheticImageHasCheckedHalfOpenRegions) {
  const std::array<uint8_t, kInstructionSize> Text = {0x95};
  const std::array<uint8_t, 4> Rodata = {1, 2, 3, 4};
  auto Image = createProgramImage(Text, kBytecodeStart, Rodata, kRodataStartV3);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->regions().size(), 2u);
  EXPECT_EQ(Image->regions()[0].Address, kRodataStartV3);
  EXPECT_EQ(Image->regions()[1].Address, kBytecodeStart);
  EXPECT_EQ(Image->text(), llvm::ArrayRef<uint8_t>(Text));

  const ProgramRegion *TextRegion =
      Image->findRegion(kBytecodeStart, Text.size());
  ASSERT_NE(TextRegion, nullptr);
  EXPECT_TRUE(TextRegion->contains(kBytecodeStart, Text.size()));
  EXPECT_FALSE(TextRegion->contains(kBytecodeStart + Text.size(), 1));
  EXPECT_EQ(Image->findRegion(kBytecodeStart, 1, true), nullptr);
  EXPECT_NE(Image->findRegion(kRodataStartV3, Rodata.size(), true), nullptr);

  auto Crossing = Image->slice(kBytecodeStart + Text.size() - 1, 2);
  EXPECT_FALSE(static_cast<bool>(Crossing));
  llvm::consumeError(Crossing.takeError());
}

TEST(SBFProgramImageTest, SyntheticImageRejectsOverlapAndOverflow) {
  const std::array<uint8_t, kInstructionSize> Bytes{};
  auto Overlap =
      createProgramImage(Bytes, kBytecodeStart, Bytes, kBytecodeStart + 1);
  EXPECT_FALSE(static_cast<bool>(Overlap));
  llvm::consumeError(Overlap.takeError());

  auto Overflow =
      createProgramImage(Bytes, std::numeric_limits<va_t>::max() - 1);
  EXPECT_FALSE(static_cast<bool>(Overflow));
  llvm::consumeError(Overflow.takeError());
}

TEST(SBFProgramImageTest, LegacyImageMergesRuntimeSectionsAndRelocatesData) {
  const auto Text = makeLDDW();
  BinaryImage Source = makeLegacyImage(Text);
  const va_t TextAddress = Source.Sections.front().VA;
  const va_t RodataAddress = kBytecodeStart + 0x40;
  const va_t DataAddress = kBytecodeStart + 0x60;
  const std::array<uint8_t, 8> Rodata = {42};
  std::array<uint8_t, 16> RelocatedData{};
  llvm::support::endian::write32le(RelocatedData.data() + kImmediateOffset,
                                   0x40);
  Source.Sections.push_back(
      makeSection(kRodataSectionName, RodataAddress, Rodata));
  Source.Sections.push_back(
      makeSection(kDataRelROSectionName, DataAddress, RelocatedData));
  Source.Relocations.push_back({DataAddress,
                                0,
                                false,
                                static_cast<uint32_t>(Relocation::Relative64),
                                {},
                                0,
                                ".rela.dyn"});

  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->regions().size(), 1u);
  const ProgramRegion &Region = Image->regions().front();
  EXPECT_EQ(Region.Kind, ProgramRegionKind::LegacyReadOnly);
  EXPECT_TRUE(Region.DataVisible);
  EXPECT_EQ(Region.Address, TextAddress);
  EXPECT_EQ(Region.Sections.size(), 3u);
  EXPECT_EQ(Image->text(), llvm::ArrayRef<uint8_t>(Text));

  auto Gap = Image->slice(TextAddress + Text.size(),
                          RodataAddress - TextAddress - Text.size(), true);
  ASSERT_TRUE(static_cast<bool>(Gap)) << llvm::toString(Gap.takeError());
  for (uint8_t Byte : *Gap)
    EXPECT_EQ(Byte, 0u);

  auto Field = Image->slice(DataAddress, sizeof(uint64_t), true);
  ASSERT_TRUE(static_cast<bool>(Field)) << llvm::toString(Field.takeError());
  EXPECT_EQ(llvm::support::endian::read64le(Field->data()), RodataAddress);
}

TEST(SBFProgramImageTest, UnoptimizedLegacyImageKeepsLeadingAddressRange) {
  const auto Text = makeLDDW();
  BinaryImage Source = makeLegacyImage(Text);
  SBFVMConfig Config;
  Config.OptimizeRodata = false;
  auto Image =
      buildProgramImage(Source, makeLegacyMetadata(Text.size()), Config);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->regions().size(), 1u);
  EXPECT_EQ(Image->regions().front().Address, kBytecodeStart);
  auto Prefix = Image->slice(kBytecodeStart, 0x20, true);
  ASSERT_TRUE(static_cast<bool>(Prefix)) << llvm::toString(Prefix.takeError());
  for (uint8_t Byte : *Prefix)
    EXPECT_EQ(Byte, 0u);
}

TEST(SBFProgramImageTest, RejectsSparseLegacyImagesLargerThanTheELF) {
  const auto Text = makeLDDW();
  BinaryImage Source = makeLegacyImage(Text);
  Source.Raw.assign(Text.begin(), Text.end());
  const std::array<uint8_t, 1> Rodata = {42};
  Source.Sections.push_back(makeSection(
      kRodataSectionName, kBytecodeStart + Source.Raw.size() + 1, Rodata));

  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("ELF file"),
            std::string::npos);
}

TEST(SBFProgramImageTest, AppliesTextRelocationFormsExactlyOnce) {
  auto Text = makeLDDW(0x18);
  BinaryImage Source = makeLegacyImage(Text);
  Symbol Target;
  Target.Name = "target";
  Target.Addr = kBytecodeStart + 0x80;
  Source.Symbols.push_back(Target);
  Source.Relocations.push_back({Source.Sections.front().VA, 0, false,
                                static_cast<uint32_t>(Relocation::Abs64),
                                Target.Name, 1, ".rel.text"});

  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(readLDDWImmediate(Image->text()), kBytecodeStart + 0x98);
}

TEST(SBFProgramImageTest, RejectsMalformedRelocationsPrecisely) {
  const std::array<uint8_t, kInstructionSize> Exit{};
  BinaryImage Unknown = makeLegacyImage(Exit);
  Unknown.Relocations.push_back(
      {Unknown.Entry, 0, false, 0xffffu, {}, 0, ".rel.text"});
  auto UnknownResult =
      buildProgramImage(Unknown, makeLegacyMetadata(Exit.size()));
  ASSERT_FALSE(static_cast<bool>(UnknownResult));
  EXPECT_NE(llvm::toString(UnknownResult.takeError()).find("unsupported"),
            std::string::npos);

  BinaryImage WrongInstruction = makeLegacyImage(Exit);
  WrongInstruction.Relocations.push_back(
      {WrongInstruction.Entry,
       0,
       false,
       static_cast<uint32_t>(Relocation::Relative64),
       {},
       0,
       ".rel.text"});
  auto WrongResult =
      buildProgramImage(WrongInstruction, makeLegacyMetadata(Exit.size()));
  ASSERT_FALSE(static_cast<bool>(WrongResult));
  EXPECT_NE(llvm::toString(WrongResult.takeError()).find("LDDW"),
            std::string::npos);
}

TEST(SBFProgramImageTest, RejectsDistinctCallTargetsWithTheSameHash) {
  std::array<uint8_t, kLDDWSlotCount * kInstructionSize> Text{};
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  ASSERT_NE(Call, nullptr);
  Text[kOpcodeOffset] = Call->Encoding;
  Text[kInstructionSize + kOpcodeOffset] = Call->Encoding;
  BinaryImage Source = makeLegacyImage(Text);
  Source.Relocations.push_back({Source.Entry, 0, false,
                                static_cast<uint32_t>(Relocation::Call32),
                                "collision_40286", 1, ".rel.text"});
  Source.Relocations.push_back({Source.Entry + kInstructionSize, 0, false,
                                static_cast<uint32_t>(Relocation::Call32),
                                "collision_121561", 2, ".rel.text"});
  ASSERT_EQ(hashSymbolName(Source.Relocations[0].SymbolName),
            hashSymbolName(Source.Relocations[1].SymbolName));

  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("hash collision"),
            std::string::npos);
}

TEST(SBFProgramImageTest, RelocationMetadataDefinesEveryPolicy) {
  const llvm::ArrayRef<RelocationInfo> Infos = relocationInfos();
  ASSERT_EQ(Infos.size(), 3u);
  for (size_t Left = 0; Left < Infos.size(); ++Left) {
    EXPECT_NE(Infos[Left].Name, "");
    EXPECT_TRUE(Infos[Left].Width == 32 || Infos[Left].Width == 64);
    EXPECT_NE(Infos[Left].Targets, RelocationTargetKind::None);
    EXPECT_TRUE(isRelocationAllowedForVersion(Infos[Left], Version::V0));
    EXPECT_FALSE(isRelocationAllowedForVersion(Infos[Left], Version::V3));
    for (size_t Right = Left + 1; Right < Infos.size(); ++Right) {
      EXPECT_NE(Infos[Left].Value, Infos[Right].Value);
      EXPECT_NE(Infos[Left].Name, Infos[Right].Name);
    }
  }
}

TEST(SBFProgramImageTest, RejectsInvalidVMConfiguration) {
  const auto Text = makeLDDW();
  BinaryImage Source = makeLegacyImage(Text);
  SBFVMConfig Config;
  Config.MaxCallDepth = 0;
  auto Image =
      buildProgramImage(Source, makeLegacyMetadata(Text.size()), Config);
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("call depth"),
            std::string::npos);
}

} // namespace
} // namespace neverd::sbf
