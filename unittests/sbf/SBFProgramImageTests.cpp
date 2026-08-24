//===- SBFProgramImageTests.cpp - Canonical SBF VM-image tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFFixtureBuilder.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/image/SBFProgramImage.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/runtime/SBFOpcodes.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace neverd::sbf {
namespace {

class TemporaryELF {
public:
  explicit TemporaryELF(llvm::ArrayRef<uint8_t> Bytes) {
    Error = llvm::sys::fs::createTemporaryFile("neverd-sbf-image", "so", Path);
    if (Error)
      return;
    std::ofstream Output(Path.str().str(), std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()),
                 static_cast<std::streamsize>(Bytes.size()));
  }

  ~TemporaryELF() {
    if (!Path.empty())
      llvm::sys::fs::remove(Path);
  }

  llvm::StringRef path() const { return Path; }
  std::error_code error() const { return Error; }

private:
  llvm::SmallString<128> Path;
  std::error_code Error;
};

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
  Result.TextFile.Size = TextSize;
  Result.TextVM = {TextAddress, TextSize};
  return Result;
}

BinaryImage makeRawBackedLegacyImage(llvm::ArrayRef<uint8_t> Text,
                                     uint64_t TextFileOffset,
                                     va_t TextAddress) {
  BinaryImage Image = makeLegacyImage(Text, TextAddress);
  Image.Raw.resize(static_cast<size_t>(TextFileOffset) + Text.size());
  std::copy(Text.begin(), Text.end(),
            Image.Raw.begin() + static_cast<ptrdiff_t>(TextFileOffset));
  Image.Sections.front().FileOff = TextFileOffset;
  Image.Sections.front().FileSz = Text.size();
  return Image;
}

Metadata makeRawBackedLegacyMetadata(size_t TextSize, uint64_t TextFileOffset,
                                     va_t TextAddress) {
  Metadata Result = makeLegacyMetadata(TextSize, TextAddress);
  Result.TextFile = {TextFileOffset, TextSize};
  return Result;
}

ELFRelocationProvenance
exactELFSymbol(uint64_t Value, uint16_t SectionIndex, uint8_t Type,
               std::optional<llvm::StringRef> Name = std::nullopt) {
  ELFRelocationProvenance Result;
  Result.Source = ELFRelocationSource::ProgramDynamicTable;
  ELFRelocationSymbol Symbol;
  Symbol.SectionIndex = SectionIndex;
  Symbol.Type = Type;
  Symbol.Binding = llvm::ELF::STB_GLOBAL;
  Symbol.Value = Value;
  if (Name)
    Symbol.Name = Name->str();
  Result.Symbol = std::move(Symbol);
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

TEST(SBFProgramImageTest, DebugSymbolsCannotCreateExecutableCallTargets) {
  std::array<uint8_t, 4 * kInstructionSize> Text{};
  BinaryImage Source = makeLegacyImage(Text);
  const va_t TextAddress = Source.Sections.front().VA;

  Symbol Later;
  Later.Name = "later";
  Later.Addr = TextAddress + 3 * kInstructionSize;
  Later.IsFunc = true;
  Symbol Earlier;
  Earlier.Name = "earlier";
  Earlier.Addr = TextAddress + kInstructionSize;
  Earlier.IsFunc = true;
  Symbol EarlierAlias = Earlier;
  EarlierAlias.Name = "alpha";
  Source.Symbols = {Later, Earlier, EarlierAlias};

  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->functions().size(), 1u);
  EXPECT_TRUE(std::is_sorted(
      Image->functions().begin(), Image->functions().end(),
      [](const ProgramFunctionEntry &Left, const ProgramFunctionEntry &Right) {
        return Left.Key < Right.Key;
      }));
  const ProgramFunctionEntry *Entrypoint =
      Image->findFunction(legacyFunctionKey(0, kEntrySymbolName));
  ASSERT_NE(Entrypoint, nullptr);
  EXPECT_EQ(Entrypoint->TargetSlot, 0u);
  EXPECT_EQ(Entrypoint->Name, kEntrySymbolName);
  EXPECT_EQ(Image->findFunction(legacyFunctionKey(3, Later.Name)), nullptr);
  EXPECT_EQ(Image->findFunction(legacyFunctionKey(1, Earlier.Name)), nullptr);

  Metadata StaticMetadata = makeLegacyMetadata(Text.size());
  StaticMetadata.Version = Version::V3;
  StaticMetadata.StrictLayout = true;
  auto StaticImage = buildProgramImage(Source, StaticMetadata);
  ASSERT_TRUE(static_cast<bool>(StaticImage))
      << llvm::toString(StaticImage.takeError());
  EXPECT_TRUE(StaticImage->functions().empty());
}

TEST(SBFProgramImageTest, LargeRelativeCallRegistryIsCanonicalAndSorted) {
  constexpr size_t InstructionCount = 4096;
  std::vector<uint8_t> Text(InstructionCount * kInstructionSize);
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Exit, nullptr);
  for (size_t Slot = 0; Slot + 1 < InstructionCount; ++Slot)
    Text[Slot * kInstructionSize + kOpcodeOffset] = Call->Encoding;
  Text[(InstructionCount - 1) * kInstructionSize + kOpcodeOffset] =
      Exit->Encoding;

  BinaryImage Source = makeLegacyImage(Text);
  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->functions().size(), InstructionCount);
  EXPECT_TRUE(std::is_sorted(
      Image->functions().begin(), Image->functions().end(),
      [](const ProgramFunctionEntry &Left, const ProgramFunctionEntry &Right) {
        return Left.Key < Right.Key;
      }));
  for (size_t TargetSlot :
       {size_t{1}, InstructionCount / 2, InstructionCount - 1}) {
    const ProgramFunctionEntry *Entry =
        Image->findFunction(legacyFunctionKey(TargetSlot, {}));
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->TargetSlot, TargetSlot);
  }
}

TEST(SBFProgramImageTest,
     RejectsRelativeCallKeyThatCollidesWithTheRuntimeRegistry) {
  std::array<uint8_t, 2 * kInstructionSize> Text{};
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Exit, nullptr);
  Text[kOpcodeOffset] = Call->Encoding;
  Text[kInstructionSize + kOpcodeOffset] = Exit->Encoding;

  constexpr size_t TargetSlot = 1;
  const uint32_t CollisionKey = legacyFunctionKey(TargetSlot, {});
  const std::array<uint32_t, 3> Registry = {CollisionKey, 0, CollisionKey};
  auto Image =
      buildProgramImage(makeLegacyImage(Text), makeLegacyMetadata(Text.size()),
                        SBFVMConfig{}, Registry);
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("registered syscall"),
            std::string::npos);
}

TEST(SBFProgramImageTest,
     RejectsCallRelocationKeyThatCollidesWithTheRuntimeRegistry) {
  std::array<uint8_t, 2 * kInstructionSize> Text{};
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Exit, nullptr);
  Text[kOpcodeOffset] = Call->Encoding;
  llvm::support::endian::write32le(
      Text.data() + kImmediateOffset,
      static_cast<uint32_t>(kLegacyUnresolvedCallImmediate));
  Text[kInstructionSize + kOpcodeOffset] = Exit->Encoding;

  constexpr size_t TargetSlot = 1;
  BinaryImage Source = makeLegacyImage(Text);
  constexpr llvm::StringLiteral FunctionName = "relocated_function";
  Source.Relocations.push_back({Source.Entry, 0, false,
                                static_cast<uint32_t>(Relocation::Call32),
                                FunctionName.str(), 1, ".rel.text"});
  Source.Relocations.back().ELF = exactELFSymbol(
      Source.Entry - kBytecodeStart + TargetSlot * kInstructionSize, 1,
      llvm::ELF::STT_FUNC, FunctionName);

  const uint32_t CollisionKey = legacyFunctionKey(TargetSlot, FunctionName);
  const std::array<uint32_t, 1> Registry = {CollisionKey};
  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()),
                                 SBFVMConfig{}, Registry);
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("registered syscall"),
            std::string::npos);
}

TEST(SBFProgramImageTest,
     RejectsEntrypointKeyThatCollidesWithTheRuntimeRegistry) {
  std::array<uint8_t, kInstructionSize> Text{};
  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  ASSERT_NE(Exit, nullptr);
  Text[kOpcodeOffset] = Exit->Encoding;

  const uint32_t CollisionKey = hashSymbolName(kEntrySymbolName);
  const std::array<uint32_t, 1> Registry = {CollisionKey};
  auto Image =
      buildProgramImage(makeLegacyImage(Text), makeLegacyMetadata(Text.size()),
                        SBFVMConfig{}, Registry);
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("registered syscall"),
            std::string::npos);
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

TEST(SBFProgramImageTest,
     RawHeaderAndGapRelocationsDoNotPolluteCanonicalVMGaps) {
  using ELFT = llvm::object::ELF64LE;
  const auto Text = makeLDDW();
  constexpr uint64_t TextFileOffset = sizeof(ELFT::Ehdr) + 2 * kInstructionSize;
  constexpr va_t TextAddress = kBytecodeStart + 0x100;
  BinaryImage Source =
      makeRawBackedLegacyImage(Text, TextFileOffset, TextAddress);
  Source.Raw.resize(0x200);

  constexpr std::array<uint64_t, 2> RawSites = {0, sizeof(ELFT::Ehdr)};
  for (size_t Ordinal = 0; Ordinal < RawSites.size(); ++Ordinal) {
    llvm::support::endian::write32le(Source.Raw.data() + RawSites[Ordinal] +
                                         kImmediateOffset,
                                     static_cast<uint32_t>(Ordinal + 1));
    RelocationEntry Entry;
    Entry.Address = kBytecodeStart + RawSites[Ordinal];
    Entry.Type = static_cast<uint32_t>(Relocation::Relative64);
    ELFRelocationProvenance Provenance;
    Provenance.Source = ELFRelocationSource::ProgramDynamicTable;
    Provenance.Ordinal = Ordinal;
    Provenance.RawOffset = RawSites[Ordinal];
    Entry.ELF = Provenance;
    Source.Relocations.push_back(std::move(Entry));
  }

  Metadata Metadata =
      makeRawBackedLegacyMetadata(Text.size(), TextFileOffset, TextAddress);
  SBFVMConfig Config;
  Config.OptimizeRodata = false;
  auto Image = buildProgramImage(Source, Metadata, Config);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  auto Prefix =
      Image->slice(kBytecodeStart, TextAddress - kBytecodeStart, true);
  ASSERT_TRUE(static_cast<bool>(Prefix)) << llvm::toString(Prefix.takeError());
  for (const uint8_t Byte : *Prefix)
    EXPECT_EQ(Byte, 0u);
  EXPECT_EQ(Image->text(), llvm::ArrayRef<uint8_t>(Text));
}

TEST(SBFProgramImageTest, RawRelocationsFollowELFOrdinal) {
  using ELFT = llvm::object::ELF64LE;
  std::array<uint8_t, kInstructionSize> Text{};
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  ASSERT_NE(Call, nullptr);
  Text[kOpcodeOffset] = Call->Encoding;
  llvm::support::endian::write32le(
      Text.data() + kImmediateOffset,
      static_cast<uint32_t>(kLegacyUnresolvedCallImmediate));

  constexpr uint64_t TextFileOffset = sizeof(ELFT::Ehdr);
  constexpr va_t TextAddress = kBytecodeStart + 0x80;
  BinaryImage Source =
      makeRawBackedLegacyImage(Text, TextFileOffset, TextAddress);
  auto MakeRelocation = [&](llvm::StringRef Name, uint64_t Ordinal) {
    RelocationEntry Entry;
    Entry.Address = TextAddress;
    Entry.Type = static_cast<uint32_t>(Relocation::Call32);
    Entry.SymbolName = Name.str();
    Entry.ELF =
        exactELFSymbol(0, llvm::ELF::SHN_UNDEF, llvm::ELF::STT_NOTYPE, Name);
    Entry.ELF->Ordinal = Ordinal;
    Entry.ELF->RawOffset = TextFileOffset;
    return Entry;
  };
  constexpr llvm::StringLiteral FirstName = "ordinal_first";
  constexpr llvm::StringLiteral LastName = "ordinal_last";
  Source.Relocations.push_back(MakeRelocation(LastName, 1));
  Source.Relocations.push_back(MakeRelocation(FirstName, 0));

  auto Image = buildProgramImage(
      Source,
      makeRawBackedLegacyMetadata(Text.size(), TextFileOffset, TextAddress));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(
      llvm::support::endian::read32le(Image->text().data() + kImmediateOffset),
      hashSymbolName(LastName));
}

TEST(SBFProgramImageTest, RelativeCallsAreFixedBeforeRawRelocations) {
  using ELFT = llvm::object::ELF64LE;
  std::array<uint8_t, 2 * kInstructionSize> Text{};
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Exit, nullptr);
  Text[kOpcodeOffset] = Call->Encoding;
  Text[kInstructionSize + kOpcodeOffset] = Exit->Encoding;

  constexpr uint64_t TextFileOffset = sizeof(ELFT::Ehdr);
  constexpr va_t TextAddress = kBytecodeStart + 0x80;
  BinaryImage Source =
      makeRawBackedLegacyImage(Text, TextFileOffset, TextAddress);
  constexpr llvm::StringLiteral RelocatedName = "relocation_wins";
  RelocationEntry Entry;
  Entry.Address = TextAddress;
  Entry.Type = static_cast<uint32_t>(Relocation::Call32);
  Entry.SymbolName = RelocatedName.str();
  Entry.ELF = exactELFSymbol(0, llvm::ELF::SHN_UNDEF, llvm::ELF::STT_NOTYPE,
                             RelocatedName);
  Entry.ELF->RawOffset = TextFileOffset;
  Source.Relocations.push_back(std::move(Entry));

  auto Image = buildProgramImage(
      Source,
      makeRawBackedLegacyMetadata(Text.size(), TextFileOffset, TextAddress));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(
      llvm::support::endian::read32le(Image->text().data() + kImmediateOffset),
      hashSymbolName(RelocatedName));
  EXPECT_NE(Image->findFunction(legacyFunctionKey(1, {})), nullptr);
}

TEST(SBFProgramImageTest, UnknownRawRelocationPrecedesSiteValidation) {
  using ELFT = llvm::object::ELF64LE;
  const std::array<uint8_t, kInstructionSize> Text{};
  constexpr uint64_t TextFileOffset = sizeof(ELFT::Ehdr);
  constexpr va_t TextAddress = kBytecodeStart + 0x80;
  BinaryImage Source =
      makeRawBackedLegacyImage(Text, TextFileOffset, TextAddress);
  RelocationEntry Entry;
  Entry.Address = TextAddress;
  Entry.Type = std::numeric_limits<uint32_t>::max() - 1;
  ELFRelocationProvenance Provenance;
  Provenance.Source = ELFRelocationSource::ProgramDynamicTable;
  Provenance.RawOffset = std::numeric_limits<uint64_t>::max();
  Entry.ELF = Provenance;
  Source.Relocations.push_back(std::move(Entry));

  auto Image = buildProgramImage(
      Source,
      makeRawBackedLegacyMetadata(Text.size(), TextFileOffset, TextAddress));
  ASSERT_FALSE(static_cast<bool>(Image));
  const std::string Message = llvm::toString(Image.takeError());
  EXPECT_NE(Message.find("unsupported relocation type"), std::string::npos);
  EXPECT_EQ(Message.find("outside the ELF"), std::string::npos);
}

TEST(SBFProgramImageTest, RejectBrokenELFsUsesResolvedSyscallRegistry) {
  using ELFT = llvm::object::ELF64LE;
  std::array<uint8_t, kInstructionSize> Text{};
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  ASSERT_NE(Call, nullptr);
  Text[kOpcodeOffset] = Call->Encoding;
  llvm::support::endian::write32le(
      Text.data() + kImmediateOffset,
      static_cast<uint32_t>(kLegacyUnresolvedCallImmediate));

  constexpr uint64_t TextFileOffset = sizeof(ELFT::Ehdr);
  const va_t TextAddress = kBytecodeStart + TextFileOffset;
  BinaryImage Source =
      makeRawBackedLegacyImage(Text, TextFileOffset, TextAddress);
  constexpr llvm::StringLiteral SymbolName = "registered_syscall";
  RelocationEntry Entry;
  Entry.Address = TextAddress;
  Entry.Type = static_cast<uint32_t>(Relocation::Call32);
  Entry.SymbolName = SymbolName.str();
  Entry.ELF = exactELFSymbol(0, llvm::ELF::SHN_UNDEF, llvm::ELF::STT_NOTYPE,
                             SymbolName);
  Entry.ELF->RawOffset = TextFileOffset;
  Source.Relocations.push_back(std::move(Entry));
  Metadata Metadata =
      makeRawBackedLegacyMetadata(Text.size(), TextFileOffset, TextAddress);
  SBFVMConfig DeploymentConfig;
  DeploymentConfig.RejectBrokenELFs = true;

  auto Unresolved = buildProgramImage(Source, Metadata, DeploymentConfig);
  ASSERT_FALSE(static_cast<bool>(Unresolved));
  EXPECT_NE(llvm::toString(Unresolved.takeError()).find("unresolved external"),
            std::string::npos);

  const uint32_t RegisteredKey = hashSymbolName(SymbolName);
  ASSERT_NE(RegisteredKey, 0u);
  const std::array<uint32_t, 2> Registry = {RegisteredKey, 0};
  auto Resolved =
      buildProgramImage(Source, Metadata, DeploymentConfig, Registry);
  ASSERT_TRUE(static_cast<bool>(Resolved))
      << llvm::toString(Resolved.takeError());
  EXPECT_EQ(llvm::support::endian::read32le(Resolved->text().data() +
                                            kImmediateOffset),
            RegisteredKey);
}

TEST(SBFProgramImageTest, EmptyLegacyReadOnlySectionShapesMappableBounds) {
  using ELFT = llvm::object::ELF64LE;
  constexpr uint64_t TextFileOffset =
      llvm::alignTo(sizeof(ELFT::Ehdr), kInstructionSize);
  constexpr uint64_t EmptyRodataAddress = TextFileOffset + 3 * kInstructionSize;
  test::LegacyELFOptions Options;
  Options.VirtualAddressBase = TextFileOffset;
  Options.DataVirtualAddress = EmptyRodataAddress;
  Options.DataSectionName = kRodataSectionName.str();
  TemporaryELF File(test::buildLegacyELF(Options));
  ASSERT_FALSE(File.error()) << File.error().message();
  ELFLoader Loader;
  auto Source = Loader.load(File.path().str());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_TRUE(Source->SBF.has_value());

  auto Image = buildProgramImage(*Source, *Source->SBF);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_NE(Image->findRegion(kBytecodeStart + EmptyRodataAddress - 1, 1, true),
            nullptr);
  EXPECT_EQ(Image->findRegion(kBytecodeStart + EmptyRodataAddress, 1, true),
            nullptr);
}

TEST(SBFProgramImageTest, NoBitsTextKeepsOnlyItsLogicalEntrypointExtent) {
  test::LegacyELFOptions Options;
  Options.TextIsNoBits = true;
  TemporaryELF File(test::buildLegacyELF(Options));
  ASSERT_FALSE(File.error()) << File.error().message();
  ELFLoader Loader;
  auto Source = Loader.load(File.path().str());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_TRUE(Source->SBF.has_value());

  SBFVMConfig Config;
  Config.OptimizeRodata = false;
  auto Image = buildProgramImage(*Source, *Source->SBF, Config);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_TRUE(Image->text().empty());
  EXPECT_EQ(Image->textVirtualSize(), kInstructionSize);
  EXPECT_EQ(Image->entrySlot(), 0u);
}

TEST(SBFProgramImageTest, PartialInstructionTailDoesNotCreateATextSlot) {
  test::LegacyELFOptions Options;
  Options.Text.resize(kInstructionSize + 1);
  TemporaryELF File(test::buildLegacyELF(Options));
  ASSERT_FALSE(File.error()) << File.error().message();
  ELFLoader Loader;
  auto Source = Loader.load(File.path().str());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_TRUE(Source->SBF.has_value());

  SBFVMConfig Config;
  Config.OptimizeRodata = false;
  auto Image = buildProgramImage(*Source, *Source->SBF, Config);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->text().size(), Options.Text.size());
  EXPECT_EQ(Image->text().size() / kInstructionSize, 1u);
  EXPECT_EQ(Image->textVirtualSize(), Options.Text.size());
}

TEST(SBFProgramImageTest, ExecutableFlagOnRodataCannotCreateSecondText) {
  test::LegacyELFOptions Options;
  Options.AddReadOnlyData = true;
  Options.DataSectionName = kRodataSectionName.str();
  Options.ReadOnlyDataIsExecutable = true;
  TemporaryELF File(test::buildLegacyELF(Options));
  ASSERT_FALSE(File.error()) << File.error().message();
  ELFLoader Loader;
  auto Source = Loader.load(File.path().str());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_TRUE(Source->SBF.has_value());

  SBFVMConfig Config;
  Config.OptimizeRodata = false;
  auto Image = buildProgramImage(*Source, *Source->SBF, Config);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->text().size(), kInstructionSize);
  ASSERT_EQ(Image->regions().size(), 1u);
  EXPECT_EQ(std::count_if(Image->regions().front().Sections.begin(),
                          Image->regions().front().Sections.end(),
                          [](const ProgramSectionSpan &Section) {
                            return Section.Executable;
                          }),
            1u);
}

TEST(SBFProgramImageTest, NonAllocatableLegacyReadOnlyDataIsMaterialized) {
  using ELFT = llvm::object::ELF64LE;
  constexpr uint64_t TextFileOffset =
      llvm::alignTo(sizeof(ELFT::Ehdr), kInstructionSize);
  constexpr uint64_t DataFileOffset = TextFileOffset + kInstructionSize;
  constexpr uint8_t PayloadByte = 0x5a;
  test::LegacyELFOptions Options;
  Options.VirtualAddressBase = TextFileOffset;
  Options.AddReadOnlyData = true;
  Options.ReadOnlyDataIsAllocatable = false;
  Options.DataVirtualAddress = DataFileOffset;
  Options.DataSectionName = kRodataSectionName.str();
  std::vector<uint8_t> Bytes = test::buildLegacyELF(Options);
  ASSERT_LT(DataFileOffset, Bytes.size());
  Bytes[DataFileOffset] = PayloadByte;
  TemporaryELF File(Bytes);
  ASSERT_FALSE(File.error()) << File.error().message();
  ELFLoader Loader;
  auto Source = Loader.load(File.path().str());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_TRUE(Source->SBF.has_value());
  EXPECT_NE(Source->getSectionByName(kRodataSectionName), nullptr);

  auto Image = buildProgramImage(*Source, *Source->SBF);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  auto Data =
      Image->slice(kBytecodeStart + DataFileOffset, kInstructionSize, true);
  ASSERT_TRUE(static_cast<bool>(Data)) << llvm::toString(Data.takeError());
  EXPECT_EQ(Data->front(), PayloadByte);
}

TEST(SBFProgramImageTest,
     UnoptimizedLegacyImageDoesNotTreatPrebasedRawAddressAsRuntimeAddress) {
  test::LegacyELFOptions Options;
  Options.AddReadOnlyData = true;
  Options.DataSectionName = kRodataSectionName.str();
  Options.DataVirtualAddress = kBytecodeStart + kInstructionSize;
  TemporaryELF File(test::buildLegacyELF(Options));
  ASSERT_FALSE(File.error()) << File.error().message();
  ELFLoader Loader;
  auto Source = Loader.load(File.path().str());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_TRUE(Source->SBF.has_value());

  SBFVMConfig Config;
  Config.OptimizeRodata = false;
  auto Image = buildProgramImage(*Source, *Source->SBF, Config);
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("VM range"),
            std::string::npos);
}

TEST(SBFProgramImageTest,
     OptimizedLegacyImageRejectsPrebasedRawAddressBeforeBorrowing) {
  test::LegacyELFOptions Options;
  Options.AddReadOnlyData = true;
  Options.DataSectionName = kRodataSectionName.str();
  Options.DataVirtualAddress = kBytecodeStart + kInstructionSize;
  TemporaryELF File(test::buildLegacyELF(Options));
  ASSERT_FALSE(File.error()) << File.error().message();
  ELFLoader Loader;
  auto Source = Loader.load(File.path().str());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_TRUE(Source->SBF.has_value());

  SBFVMConfig Config;
  ASSERT_TRUE(Config.OptimizeRodata);
  auto Image = buildProgramImage(*Source, *Source->SBF, Config);
  ASSERT_FALSE(static_cast<bool>(Image));
  EXPECT_NE(llvm::toString(Image.takeError()).find("VM range"),
            std::string::npos);
}

TEST(SBFProgramImageTest, RejectBrokenELFsChecksLegacyTextFileIdentity) {
  TemporaryELF File(test::buildLegacyELF());
  ASSERT_FALSE(File.error()) << File.error().message();
  ELFLoader Loader;
  auto Source = Loader.load(File.path().str());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_TRUE(Source->SBF.has_value());

  SBFVMConfig ExecutionConfig;
  ExecutionConfig.RejectBrokenELFs = false;
  auto ExecutionImage =
      buildProgramImage(*Source, *Source->SBF, ExecutionConfig);
  ASSERT_TRUE(static_cast<bool>(ExecutionImage))
      << llvm::toString(ExecutionImage.takeError());

  SBFVMConfig DeploymentConfig = ExecutionConfig;
  DeploymentConfig.RejectBrokenELFs = true;
  auto DeploymentImage =
      buildProgramImage(*Source, *Source->SBF, DeploymentConfig);
  ASSERT_FALSE(static_cast<bool>(DeploymentImage));
  EXPECT_NE(llvm::toString(DeploymentImage.takeError()).find("sh_addr"),
            std::string::npos);
}

TEST(SBFProgramImageTest, RejectBrokenELFsChecksEveryReadOnlySectionOffset) {
  using ELFT = llvm::object::ELF64LE;
  constexpr uint64_t TextFileOffset =
      llvm::alignTo(sizeof(ELFT::Ehdr), kInstructionSize);
  constexpr uint64_t DataFileOffset = TextFileOffset + kInstructionSize;
  test::LegacyELFOptions Options;
  Options.VirtualAddressBase = TextFileOffset;
  Options.AddReadOnlyData = true;
  Options.DataVirtualAddress = DataFileOffset + kInstructionSize;
  Options.DataSectionName = kRodataSectionName.str();
  TemporaryELF File(test::buildLegacyELF(Options));
  ASSERT_FALSE(File.error()) << File.error().message();
  ELFLoader Loader;
  auto Source = Loader.load(File.path().str());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  ASSERT_TRUE(Source->SBF.has_value());

  SBFVMConfig ExecutionConfig;
  ExecutionConfig.RejectBrokenELFs = false;
  auto ExecutionImage =
      buildProgramImage(*Source, *Source->SBF, ExecutionConfig);
  ASSERT_TRUE(static_cast<bool>(ExecutionImage))
      << llvm::toString(ExecutionImage.takeError());

  SBFVMConfig DeploymentConfig = ExecutionConfig;
  DeploymentConfig.RejectBrokenELFs = true;
  auto DeploymentImage =
      buildProgramImage(*Source, *Source->SBF, DeploymentConfig);
  ASSERT_FALSE(static_cast<bool>(DeploymentImage));
  EXPECT_NE(llvm::toString(DeploymentImage.takeError()).find("sh_addr"),
            std::string::npos);
}

TEST(SBFProgramImageTest, RejectBrokenELFsChecksPackedReadOnlySpan) {
  const auto Text = makeLDDW();
  constexpr uint64_t RawTextAddress = 0x40;
  constexpr uint64_t RawRodataAddress = RawTextAddress + kInstructionSize;
  const va_t TextAddress = kBytecodeStart + RawTextAddress;
  BinaryImage Source = makeLegacyImage(Text, TextAddress);
  Source.Sections.front().FileOff = RawTextAddress;
  const std::array<uint8_t, kInstructionSize> Rodata = {42};
  Section RodataSection = makeSection(
      kRodataSectionName, kBytecodeStart + RawRodataAddress, Rodata);
  RodataSection.FileOff = RawRodataAddress;
  Source.Sections.push_back(std::move(RodataSection));
  Metadata Metadata = makeLegacyMetadata(Text.size(), TextAddress);
  Metadata.TextFile = {RawTextAddress, Text.size()};

  SBFVMConfig ExecutionConfig;
  ExecutionConfig.RejectBrokenELFs = false;
  auto ExecutionImage = buildProgramImage(Source, Metadata, ExecutionConfig);
  ASSERT_TRUE(static_cast<bool>(ExecutionImage))
      << llvm::toString(ExecutionImage.takeError());

  SBFVMConfig DeploymentConfig = ExecutionConfig;
  DeploymentConfig.RejectBrokenELFs = true;
  auto DeploymentImage = buildProgramImage(Source, Metadata, DeploymentConfig);
  ASSERT_FALSE(static_cast<bool>(DeploymentImage));
  EXPECT_NE(llvm::toString(DeploymentImage.takeError()).find("packed span"),
            std::string::npos);
}

TEST(SBFProgramImageTest, OfficialLegacyLoadErrorPrecedenceIsStable) {
  using ELFT = llvm::object::ELF64LE;
  std::array<uint8_t, 2 * kInstructionSize> Text{};
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Exit, nullptr);
  Text[kOpcodeOffset] = Call->Encoding;
  llvm::support::endian::write32le(Text.data() + kImmediateOffset,
                                   std::numeric_limits<int32_t>::max());
  Text[kInstructionSize + kOpcodeOffset] = Exit->Encoding;

  constexpr uint64_t TextFileOffset = sizeof(ELFT::Ehdr);
  constexpr uint64_t MismatchedTextAddress = TextFileOffset - kInstructionSize;
  constexpr uint64_t RodataAddress = TextFileOffset + 4 * kInstructionSize;
  constexpr uint64_t RodataFileOffset = RodataAddress + kInstructionSize;
  const va_t TextAddress = kBytecodeStart + MismatchedTextAddress;
  BinaryImage Source =
      makeRawBackedLegacyImage(Text, TextFileOffset, TextAddress);
  const std::array<uint8_t, kInstructionSize> Rodata = {42};
  Section RodataSection =
      makeSection(kRodataSectionName, kBytecodeStart + RodataAddress, Rodata);
  RodataSection.FileOff = RodataFileOffset;
  Source.Raw.resize(RodataFileOffset + Rodata.size());
  std::copy(Rodata.begin(), Rodata.end(),
            Source.Raw.begin() + static_cast<ptrdiff_t>(RodataFileOffset));
  Source.Sections.push_back(std::move(RodataSection));
  Source.Entry = TextAddress + 1;

  RelocationEntry Unknown;
  Unknown.Address = TextAddress;
  Unknown.Type = std::numeric_limits<uint32_t>::max() - 1;
  ELFRelocationProvenance Provenance;
  Provenance.Source = ELFRelocationSource::ProgramDynamicTable;
  Provenance.RawOffset = std::numeric_limits<uint64_t>::max();
  Unknown.ELF = Provenance;
  Source.Relocations.push_back(std::move(Unknown));
  Metadata Metadata =
      makeRawBackedLegacyMetadata(Text.size(), TextFileOffset, TextAddress);
  SBFVMConfig DeploymentConfig;
  DeploymentConfig.RejectBrokenELFs = true;

  auto TextMismatch = buildProgramImage(Source, Metadata, DeploymentConfig);
  ASSERT_FALSE(static_cast<bool>(TextMismatch));
  EXPECT_NE(llvm::toString(TextMismatch.takeError()).find(".text sh_addr"),
            std::string::npos);

  const va_t MatchedTextAddress = kBytecodeStart + TextFileOffset;
  Source.Sections.front().VA = MatchedTextAddress;
  Source.Entry = MatchedTextAddress + 1;
  Source.Relocations.front().Address = MatchedTextAddress;
  Metadata.TextVM.Address = MatchedTextAddress;
  auto RelativeCall = buildProgramImage(Source, Metadata, DeploymentConfig);
  ASSERT_FALSE(static_cast<bool>(RelativeCall));
  EXPECT_NE(llvm::toString(RelativeCall.takeError()).find("relative CALL"),
            std::string::npos);

  llvm::support::endian::write32le(
      Source.Raw.data() + TextFileOffset + kImmediateOffset,
      static_cast<uint32_t>(kLegacyUnresolvedCallImmediate));
  auto UnknownRelocation =
      buildProgramImage(Source, Metadata, DeploymentConfig);
  ASSERT_FALSE(static_cast<bool>(UnknownRelocation));
  EXPECT_NE(llvm::toString(UnknownRelocation.takeError()).find("unsupported"),
            std::string::npos);

  Source.Relocations.clear();
  auto Entrypoint = buildProgramImage(Source, Metadata, DeploymentConfig);
  ASSERT_FALSE(static_cast<bool>(Entrypoint));
  EXPECT_NE(llvm::toString(Entrypoint.takeError()).find("entrypoint"),
            std::string::npos);

  Source.Entry = MatchedTextAddress;
  auto ReadOnlyLayout = buildProgramImage(Source, Metadata, DeploymentConfig);
  ASSERT_FALSE(static_cast<bool>(ReadOnlyLayout));
  EXPECT_NE(
      llvm::toString(ReadOnlyLayout.takeError()).find("read-only sh_addr"),
      std::string::npos);
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
  Source.Relocations.back().ELF =
      exactELFSymbol(Target.Addr - kBytecodeStart, 1, llvm::ELF::STT_NOTYPE);

  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(readLDDWImmediate(Image->text()), kBytecodeStart + 0x98);
}

TEST(SBFProgramImageTest, CallRelocationRegistersItsFunctionIdentity) {
  std::array<uint8_t, 2 * kInstructionSize> Text{};
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Exit, nullptr);
  Text[kOpcodeOffset] = Call->Encoding;
  llvm::support::endian::write32le(
      Text.data() + kImmediateOffset,
      static_cast<uint32_t>(kLegacyUnresolvedCallImmediate));
  Text[kInstructionSize + kOpcodeOffset] = Exit->Encoding;

  BinaryImage Source = makeLegacyImage(Text);
  Symbol Function;
  Function.Name = "relocated_function";
  Function.Addr = Source.Entry + kInstructionSize;
  Function.IsFunc = true;
  Source.Symbols.push_back(Function);
  Source.Relocations.push_back({Source.Entry, 0, false,
                                static_cast<uint32_t>(Relocation::Call32),
                                Function.Name, 1, ".rel.text"});
  Source.Relocations.back().ELF = exactELFSymbol(
      Function.Addr - kBytecodeStart, 1, llvm::ELF::STT_FUNC, Function.Name);

  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const uint32_t Key = legacyFunctionKey(1, Function.Name);
  EXPECT_EQ(
      llvm::support::endian::read32le(Image->text().data() + kImmediateOffset),
      Key);
  const ProgramFunctionEntry *Entry = Image->findFunction(Key);
  ASSERT_NE(Entry, nullptr);
  EXPECT_EQ(Entry->TargetSlot, 1u);
  EXPECT_EQ(Entry->Name, Function.Name);
}

TEST(SBFProgramImageTest, EntrypointNameAlwaysResolvesToTheELFEntry) {
  std::array<uint8_t, 2 * kInstructionSize> Text{};
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  const OpcodeInfo *Exit = getOpcodeInfo(Opcode::EXIT);
  ASSERT_NE(Call, nullptr);
  ASSERT_NE(Exit, nullptr);
  Text[kOpcodeOffset] = Call->Encoding;
  llvm::support::endian::write32le(
      Text.data() + kImmediateOffset,
      static_cast<uint32_t>(kLegacyUnresolvedCallImmediate));
  Text[kInstructionSize + kOpcodeOffset] = Exit->Encoding;

  BinaryImage Source = makeLegacyImage(Text);
  Source.Relocations.push_back({Source.Entry, 0, false,
                                static_cast<uint32_t>(Relocation::Call32),
                                kEntrySymbolName.str(), 1, ".rel.text"});
  Source.Relocations.back().ELF =
      exactELFSymbol(Source.Entry + kInstructionSize - kBytecodeStart, 1,
                     llvm::ELF::STT_FUNC, kEntrySymbolName);

  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const uint32_t Key = hashSymbolName(kEntrySymbolName);
  EXPECT_EQ(
      llvm::support::endian::read32le(Image->text().data() + kImmediateOffset),
      Key);
  const ProgramFunctionEntry *Entrypoint = Image->findFunction(Key);
  ASSERT_NE(Entrypoint, nullptr);
  EXPECT_EQ(Entrypoint->TargetSlot, 0u);
  EXPECT_EQ(Entrypoint->Name, kEntrySymbolName);
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
  EXPECT_NE(llvm::toString(WrongResult.takeError()).find("outside"),
            std::string::npos);
}

TEST(SBFProgramImageTest, RelativeTextRelocationDoesNotRequireAnLDDWOpcode) {
  std::array<uint8_t, kLDDWSlotCount * kInstructionSize> Text{};
  llvm::support::endian::write32le(Text.data() + kImmediateOffset, 1);
  BinaryImage Source = makeLegacyImage(Text);
  Source.Relocations.push_back({Source.Entry,
                                0,
                                false,
                                static_cast<uint32_t>(Relocation::Relative64),
                                {},
                                0,
                                ".rel.text"});

  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(readLDDWImmediate(Image->text()), kBytecodeStart + 1);
}

TEST(SBFProgramImageTest, CollidingExternalNamesShareTheirRuntimeCallKey) {
  std::array<uint8_t, kLDDWSlotCount * kInstructionSize> Text{};
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  ASSERT_NE(Call, nullptr);
  Text[kOpcodeOffset] = Call->Encoding;
  Text[kInstructionSize + kOpcodeOffset] = Call->Encoding;
  llvm::support::endian::write32le(
      Text.data() + kImmediateOffset,
      static_cast<uint32_t>(kLegacyUnresolvedCallImmediate));
  llvm::support::endian::write32le(
      Text.data() + kInstructionSize + kImmediateOffset,
      static_cast<uint32_t>(kLegacyUnresolvedCallImmediate));
  BinaryImage Source = makeLegacyImage(Text);
  Source.Relocations.push_back({Source.Entry, 0, false,
                                static_cast<uint32_t>(Relocation::Call32),
                                "collision_40286", 1, ".rel.text"});
  Source.Relocations.back().ELF = exactELFSymbol(
      0, llvm::ELF::SHN_UNDEF, llvm::ELF::STT_NOTYPE, "collision_40286");
  Source.Relocations.push_back({Source.Entry + kInstructionSize, 0, false,
                                static_cast<uint32_t>(Relocation::Call32),
                                "collision_121561", 2, ".rel.text"});
  Source.Relocations.back().ELF = exactELFSymbol(
      0, llvm::ELF::SHN_UNDEF, llvm::ELF::STT_NOTYPE, "collision_121561");
  ASSERT_EQ(hashSymbolName(Source.Relocations[0].SymbolName),
            hashSymbolName(Source.Relocations[1].SymbolName));

  auto Image = buildProgramImage(Source, makeLegacyMetadata(Text.size()));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const uint32_t Key = hashSymbolName(Source.Relocations[0].SymbolName);
  EXPECT_EQ(
      llvm::support::endian::read32le(Image->text().data() + kImmediateOffset),
      Key);
  EXPECT_EQ(llvm::support::endian::read32le(
                Image->text().data() + kInstructionSize + kImmediateOffset),
            Key);
  EXPECT_EQ(Image->findFunction(Key), nullptr);
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
